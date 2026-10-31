/*
 * Golden test: Beta 1.7.3 ItemArmor, InventoryPlayer.getTotalArmorValue,
 * InventoryPlayer.damageArmor and EntityPlayer.damageEntity (740c583).
 * CaveWii keeps player health at x8; C5.2 runs Beta's integer formula on that
 * native scale.  E3.7 permits at most 7 x8-health units (strictly less than
 * one raw Beta health point) of accumulated quantization difference.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "entity/armor.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "platform/thread.h"
#include "world.h" // WORLD_DIM_OVERWORLD

static char s_fixture_SAVE_ROUND_TRIP_DIR[128]; /* was #define SAVE_ROUND_TRIP_DIR='test_armor_mitigation_save_tmp' (a literal) */
#define SAVE_ROUND_TRIP_DIR s_fixture_SAVE_ROUND_TRIP_DIR

static struct server_local s;
static volatile int g_drain_run = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void reset_player(void) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.active_inventory = &s.player.inventory;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.damage_remainder = 0;
	for(size_t slot = INVENTORY_SLOT_ARMOR;
		slot < INVENTORY_SLOT_ARMOR + INVENTORY_SIZE_ARMOR; slot++)
		inventory_clear_slot(&s.player.inventory, slot);
}

static void equip(unsigned slot_offset, uint16_t id, uint16_t durability) {
	assert(slot_offset < INVENTORY_SIZE_ARMOR);
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_ARMOR + slot_offset,
		(struct item_data) {.id = id, .count = 1, .durability = durability});
}

static void equip_diamond(void) {
	equip(ARMOR_TYPE_HELMET, ITEM_DIAMOND_HELMET, 0);
	equip(ARMOR_TYPE_CHESTPLATE, ITEM_DIAMOND_CHESTPLATE, 0);
	equip(ARMOR_TYPE_LEGGINGS, ITEM_DIAMOND_LEGGINGS, 0);
	equip(ARMOR_TYPE_BOOTS, ITEM_DIAMOND_BOOTS, 0);
}

static struct item_data armor_slot(unsigned slot_offset) {
	return s.player.inventory.items[INVENTORY_SLOT_ARMOR + slot_offset];
}

static void test_all_armor_max_damage(void) {
	const struct { uint16_t id, max_damage; } table[] = {
		{ITEM_LEATHER_HELMET, 33}, {ITEM_LEATHER_CHESTPLATE, 48},
		{ITEM_LEATHER_LEGGINGS, 45}, {ITEM_LEATHER_BOOTS, 39},
		{ITEM_CHAIN_HELMET, 66}, {ITEM_CHAIN_CHESTPLATE, 96},
		{ITEM_CHAIN_LEGGINGS, 90}, {ITEM_CHAIN_BOOTS, 78},
		{ITEM_GOLD_HELMET, 66}, {ITEM_GOLD_CHESTPLATE, 96},
		{ITEM_GOLD_LEGGINGS, 90}, {ITEM_GOLD_BOOTS, 78},
		{ITEM_IRON_HELMET, 132}, {ITEM_IRON_CHESTPLATE, 192},
		{ITEM_IRON_LEGGINGS, 180}, {ITEM_IRON_BOOTS, 156},
		{ITEM_DIAMOND_HELMET, 264}, {ITEM_DIAMOND_CHESTPLATE, 384},
		{ITEM_DIAMOND_LEGGINGS, 360}, {ITEM_DIAMOND_BOOTS, 312},
	};
	for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		struct item_data stack = {.id = table[i].id, .count = 1};
		struct item* it = item_get(&stack);
		assert(it && it->max_damage == table[i].max_damage);
	}
	puts("  armor maxDamage: ItemArmor slot base * 3 << material level OK");
}

static void test_mitigation_values_and_no_armor_noop(void) {
	reset_player();
	equip_diamond();
	assert(armor_get_total_value(&s.player.inventory) == 20);
	server_local_damage_player(&s, 40, 0.0f, 0.0f);
	assert(s.player.health == MAX_PLAYER_HEALTH - 8); /* 40 * 5 / 25 */

	reset_player();
	equip(ARMOR_TYPE_CHESTPLATE, ITEM_DIAMOND_CHESTPLATE, 0);
	assert(armor_get_total_value(&s.player.inventory) == 8);
	server_local_damage_player(&s, 25, 0.0f, 0.0f);
	assert(s.player.health == MAX_PLAYER_HEALTH - 17); /* 25 * 17 / 25 */

	reset_player();
	assert(armor_get_total_value(&s.player.inventory) == 0);
	server_local_damage_player(&s, 16, 0.0f, 0.0f);
	assert(s.player.health == MAX_PLAYER_HEALTH - 16);
	assert(s.player.damage_remainder == 0);
	puts("  mitigation: diamond=20 -> 5/25, chest=8 -> 17/25, empty is no-op OK");
}

// BUG 1 fix (post-review 2026-08-12): armor_damage_items now receives
// raw_damage / (HEALTH_PER_HEART/2) -- Beta half-heart units -- not the raw
// x8 `apply` value. HEALTH_PER_HEART is 16, so /8. Every raw_damage in this
// function is chosen as an exact multiple of 8 so the wear math below is
// exact, matching how every real CaveWii damage source is constructed.
static void test_remainder_raw_wear_and_break(void) {
	reset_player();
	equip_diamond();
	// Hit 1: raw 8 (= 1 Beta half-heart x8) at full durability (0/max every
	// piece). sum1=20, sum2=sum3=1320 -> totalArmorValue=(20-1)*1320/1320+1=20
	// -> factor=25-20=5. carried=8*5+0=40 -> mitigated=40/25=1, remainder=15.
	// Wear = 8/(HEALTH_PER_HEART/2) = 8/8 = 1 raw point on EVERY worn piece
	// (damageArmor is flat, not split/prorated).
	server_local_damage_player(&s, 8, 0.0f, 0.0f);
	assert(s.player.health == MAX_PLAYER_HEALTH - 1 && s.player.damage_remainder == 15);
	for(unsigned slot = 0; slot < INVENTORY_SIZE_ARMOR; slot++)
		assert(armor_slot(slot).durability == 1);

	s.player.hurt_resist_time = 0;
	// Hit 2: every piece is now durability 1/max (hit 1's wear already
	// landed), so THIS hit's mitigation uses the durability-weighted total
	// computed live -- Beta recomputes getTotalArmorValue() before each hit,
	// not once. sum2 = (264-1)+(384-1)+(360-1)+(312-1) = 1316, sum3=1320
	// unchanged -> totalArmorValue = (20-1)*1316/1320+1 = 19*1316/1320+1 =
	// 18+1 = 19 -> factor=25-19=6. carried=8*6+15(prior remainder)=63 ->
	// mitigated=63/25=2, remainder=63%25=13. Cumulative health loss 1+2=3.
	server_local_damage_player(&s, 8, 0.0f, 0.0f);
	assert(s.player.health == MAX_PLAYER_HEALTH - 3 && s.player.damage_remainder == 13);
	for(unsigned slot = 0; slot < INVENTORY_SIZE_ARMOR; slot++)
		assert(armor_slot(slot).durability == 2); /* raw wear 1 on every piece, twice */

	reset_player();
	equip(ARMOR_TYPE_CHESTPLATE, ITEM_LEATHER_CHESTPLATE, 47); // 1 point from max (48)
	// Hit 1: raw 16 (= 2 half-hearts x8), chosen so wear (16/8=2) pushes
	// 47->49 > max 48 and breaks the chestplate -- but THIS hit is still
	// mitigated by its PRE-break value, because Beta computes
	// var2 = 25 - getTotalArmorValue() BEFORE inventory.damageArmor() runs
	// (the VERBATIM order in EntityPlayer.damageEntity; armor.c preserves
	// it). sum1=8 (chest only), sum2=48-47=1, sum3=48 ->
	// totalArmorValue=(8-1)*1/48+1 = 0+1 = 1 -> factor=25-1=24.
	// carried=16*24+0=384 -> mitigated=384/25=15, remainder=384%25=9.
	server_local_damage_player(&s, 16, 0.0f, 0.0f);
	assert(armor_slot(ARMOR_TYPE_CHESTPLATE).id == 0
		   && "wear 16/8=2 pushes 47->49 > max 48, breaking and clearing the slot");
	assert(s.player.health == MAX_PLAYER_HEALTH - 15
		   && "pre-break armor value (totalArmorValue=1, factor=24) mitigates THIS hit");
	// Discriminating check (review finding: the old assertion passed by
	// integer-truncation coincidence without pinning the actual
	// durability-weighted formula) -- the carry must be the hand-computed
	// 384 % 25 = 9, not merely "whatever value made the health assert pass."
	assert(s.player.damage_remainder == 9
		   && "durability-weighted carry: carried=16*24+0=384, 384%25=9");

	s.player.hurt_resist_time = 0;
	// Hit 2: the chestplate is gone -> totalArmorValue=0 -> factor=25 -> no
	// mitigation at all (broken armor protects nothing on the NEXT hit).
	// raw 24 (=3 half-hearts x8, still an exact multiple of 8).
	// carried=24*25+9=609 -> mitigated=609/25=24, remainder=609%25=9
	// (unchanged: raw*25 is itself a multiple of 25, so an unmitigated hit
	// never touches the carry -- broken armor is a true no-op from here).
	server_local_damage_player(&s, 24, 0.0f, 0.0f);
	assert(s.player.health == MAX_PLAYER_HEALTH - 39
		   && "broken chest no longer mitigates: full raw 24 applied on top of the prior 15");
	puts("  remainder carry, raw all-slot wear (/8 Beta units), and broken armor removal OK");
}

static void test_melee_path_and_x8_equivalence(void) {
	reset_player();
	equip_diamond();
	struct entity attacker = {0};
	attacker.type = ENTITY_MONSTER;
	attacker.data.monster.id = MONSTER_ZOMBIE;
	server_local_hurt_player(&s, &attacker);
	assert(s.player.health == MAX_PLAYER_HEALTH - 8); /* zombie raw 40 -> 8 */
	// BUG 1 regression (this is the assertion that would have caught it):
	// pushes a real x8-magnitude hit through the ACTUAL production melee
	// path (not a hand reimplementation) and checks wear, not just the
	// mitigated health delta. damageArmor wears Beta RAW half-heart units:
	// 40 / (HEALTH_PER_HEART/2) = 40/8 = 5 on every worn diamond piece.
	// Before the fix this field held 40 (the raw x8 apply value passed
	// straight through), which would fail this exact assertion.
	for(unsigned slot = 0; slot < INVENTORY_SIZE_ARMOR; slot++)
		assert(armor_slot(slot).durability == 5
			   && "armor must wear raw_damage/(HEALTH_PER_HEART/2) Beta units, not the "
				  "raw x8 apply value (BUG 1)");

	/* E3.7: independent hand computation, same armor factor, x8 direct formula.
	 * The difference from eight raw-scale runs remains within [0,7] x8 units. */
	reset_player();
	equip_diamond();
	const int raw_hits[] = {1, 2, 1, 3, 4, 1, 2};
	const int factor = 25 - armor_get_total_value(&s.player.inventory);
	int beta_remainder = 0, cave_remainder = 0;
	int beta_total = 0, cave_total = 0;
	for(size_t i = 0; i < sizeof(raw_hits) / sizeof(raw_hits[0]); i++) {
		int beta = raw_hits[i] * factor + beta_remainder;
		beta_total += beta / 25;
		beta_remainder = beta % 25;
		int cave = raw_hits[i] * 8 * factor + cave_remainder;
		cave_total += cave / 25;
		cave_remainder = cave % 25;
	}
	assert(abs(cave_total - beta_total * 8) <= 7);
	puts("  x8 equivalence: native-scale integer result stays within <1 raw hp OK");
}

// --- BUG 2 regression: an out-of-range/corrupt durability must never turn --------
// --- armor into a damage AMPLIFIER --------------------------------------------------
// Legal play can NEVER produce durability > max_damage -- item_stack_damage's strict
// `>` break check (ItemStack.damageItem) clears the stack the instant a hit would
// exceed max_damage. This is purely a corruption/hostile-save defense: a hand-edited
// or corrupt NBT read (e.g. a negative Damage short read back through the uint16
// field as 65535) must not drive armor.c's `remain` deeply negative, which -- before
// the fix -- swung getTotalArmorValue() hugely negative, made `25 - totalArmorValue`
// explode past 25, and let a SMALL hit deal wildly amplified damage.
static void test_corruption_durability_does_not_amplify_damage(void) {
	const int raw = 2; // small and non-lethal even under the UNCLAMPED regression
					   // math (which would mitigate-negative to ~96 x8 units on this
					   // exact setup -- still under MAX_PLAYER_HEALTH, so a hypothetical
					   // regression is OBSERVABLE here rather than masked by a
					   // death/respawn snapping health back to full).

	// No-armor baseline for the identical raw hit, computed first and independently.
	reset_player();
	server_local_damage_player(&s, raw, 0.0f, 0.0f);
	int no_armor_loss = MAX_PLAYER_HEALTH - s.player.health;
	assert(no_armor_loss == raw && "sanity: no armor is a full-damage no-op");

	// Corrupt case: a worn, otherwise-legal diamond chestplate whose durability is
	// hand-set to 65535 -- bypassing item_stack_damage/inventory_set_slot entirely,
	// the way a corrupt NBT read would arrive (there is no legal API path to this
	// state, which is the point).
	reset_player();
	equip(ARMOR_TYPE_CHESTPLATE, ITEM_DIAMOND_CHESTPLATE, 0);
	s.player.inventory.items[INVENTORY_SLOT_ARMOR + ARMOR_TYPE_CHESTPLATE].durability
		= 65535;
	server_local_damage_player(&s, raw, 0.0f, 0.0f);
	int corrupt_loss = MAX_PLAYER_HEALTH - s.player.health;
	// Hand trace WITH the clamp: remain = max(0, 384-65535) = 0 -> sum1=8, sum2=0,
	// sum3=384 -> totalArmorValue=(8-1)*0/384+1=1 -> factor=24. carried=2*24+0=48 ->
	// mitigated=48/25=1. (Without the clamp: remain=-65151 -> totalArmorValue=-1186
	// -> factor=1211 -> carried=2*1211=2422 -> mitigated=96 -- 48x the raw hit.)
	assert(corrupt_loss == 1);
	assert(corrupt_loss >= 0 && corrupt_loss <= raw
		   && "corrupt durability must never amplify damage past the raw hit");
	assert(corrupt_loss <= no_armor_loss
		   && "corrupt/over-damaged armor must never mitigate WORSE than no armor at all");
	puts("  corruption guard: durability=65535 (out-of-range) never amplifies damage "
		 "past the raw hit or the no-armor baseline");
}

// --- (5) save/load round-trip: durability is uint16 end-to-end, no mod-256 --------
// level_archive_{read,write}_inventory already carries durability as a TAG_SHORT with
// no truncating cast (level_archive.c:582-589) -- this pins that a diamond pickaxe
// worn to durability 1500 (> 255, would wrap to 1500 % 256 == 220 under the old
// (uint8_t) cast) and a worn (non-full, non-broken) armor piece both survive a real
// write -> destroy -> reopen -> read cycle byte-exact, and that the armor NBT slot
// translation (100..103, reversed vs. INVENTORY_SLOT_ARMOR offsets) round-trips too.
static void remove_save_round_trip_files(void) {
	char path[256];
	const char* files[] = {"level.dat", "level.dat_new", "level.dat_old"};
	for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", SAVE_ROUND_TRIP_DIR, files[i]);
		remove(path);
	}
}

static void test_save_load_durability_round_trip(void) {
	remove_save_round_trip_files();
	MKDIR(SAVE_ROUND_TRIP_DIR);

	string_t name;
	string_init_set_str(name, SAVE_ROUND_TRIP_DIR);

	struct level_archive level;
	assert(level_archive_create_new(
		&level, name, 4242, (vec3) {8.5F, 70.0F, 8.5F}));
	level_archive_destroy(&level);
	assert(level_archive_create(&level, name));

	struct item_data items[INVENTORY_SIZE] = {{0}};
	// Diamond pickaxe (maxDamage 1561) worn to 1500 -- deep past 255, the old
	// (uint8_t) cast's wrap point.
	items[INVENTORY_SLOT_HOTBAR]
		= (struct item_data) {.id = ITEM_DIAMOND_PICKAXE, .count = 1, .durability = 1500};
	// A worn (not full, not broken) chestplate in the CHESTPLATE armor offset.
	items[INVENTORY_SLOT_ARMOR + ARMOR_TYPE_CHESTPLATE]
		= (struct item_data) {.id = ITEM_DIAMOND_CHESTPLATE, .count = 1, .durability = 300};
	struct inventory inv_out = {.items = items, .capacity = INVENTORY_SIZE};

	int16_t health = MAX_PLAYER_HEALTH;
	assert(level_archive_write_player(
		&level, (vec3) {8.5F, 70.0F, 8.5F}, (vec2) {0}, NULL, WORLD_DIM_OVERWORLD));
	assert(level_archive_write_inventory(&level, &inv_out));
	assert(level_archive_write(&level, LEVEL_PLAYER_HEALTH, &health));
	assert(level_archive_save(&level));
	level_archive_destroy(&level);
	string_clear(name);

	string_t name2;
	string_init_set_str(name2, SAVE_ROUND_TRIP_DIR);
	struct level_archive level2;
	assert(level_archive_create(&level2, name2));
	struct item_data read_items[INVENTORY_SIZE] = {{0}};
	struct inventory inv_in = {.items = read_items, .capacity = INVENTORY_SIZE};
	assert(level_archive_read_inventory(&level2, &inv_in));
	level_archive_destroy(&level2);
	string_clear(name2);

	struct item_data pick = read_items[INVENTORY_SLOT_HOTBAR];
	assert(pick.id == ITEM_DIAMOND_PICKAXE && pick.count == 1);
	assert(pick.durability == 1500
		   && "durability must round-trip as uint16 -- a mod-256 regression would "
			  "read 1500 % 256 == 220 here");

	struct item_data chest = read_items[INVENTORY_SLOT_ARMOR + ARMOR_TYPE_CHESTPLATE];
	assert(chest.id == ITEM_DIAMOND_CHESTPLATE && chest.count == 1);
	assert(chest.durability == 300
		   && "worn armor durability must round-trip through the 100..103 NBT slot "
			  "translation exactly");

	remove_save_round_trip_files();
	puts("  save/load: diamond pickaxe durability 1500 and a worn armor piece "
		 "survive a real write/reopen/read cycle (no mod-256)");
}

int main(void) {
	test_fixture_dir("test_armor_mitigation_save_tmp", s_fixture_SAVE_ROUND_TRIP_DIR, sizeof s_fixture_SAVE_ROUND_TRIP_DIR);
	puts("test_armor_mitigation_env: Beta 1.7.3 armor mitigation");
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	items_init();
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, NULL,
							&s.player,
		INVENTORY_SIZE, 0, 0, 0));
	test_all_armor_max_damage();
	test_mitigation_values_and_no_armor_noop();
	test_remainder_raw_wear_and_break();
	test_melee_path_and_x8_equivalence();
	test_corruption_durability_does_not_amplify_damage();
	test_save_load_durability_round_trip();
	inventory_destroy(&s.player.inventory, &s.player);
	g_drain_run = 0;
	puts("test_armor_mitigation_env: PASS");
	test_fixture_dir_cleanup(SAVE_ROUND_TRIP_DIR);
	return 0;
}
