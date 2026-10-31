#include "armor.h"

#include "combat.h"
#include "../item/inventory.h"
#include "../item/items.h"

static const int armor_reduction_by_slot[INVENTORY_SIZE_ARMOR] = {
	ARMOR_REDUCTION_HELMET, ARMOR_REDUCTION_CHESTPLATE,
	ARMOR_REDUCTION_LEGGINGS, ARMOR_REDUCTION_BOOTS,
};

static struct item* armor_item_at(const struct inventory* inventory, size_t slot) {
	struct item_data* stack = &inventory->items[INVENTORY_SLOT_ARMOR + slot];
	struct item* item = item_get(stack);
	return stack->count != 0 && item && item->armor.is_armor && item->max_damage != 0
		? item : NULL;
}

// Defensive: a caller with no real (inventory_create'd) inventory has no
// armor by definition -- never index a NULL/undersized items array. Real
// gameplay inventories are always sized INVENTORY_SIZE > this bound; the
// guard exists for test harnesses that memset a struct server_local and
// drive damage without ever calling inventory_create for the player.
static bool armor_inventory_valid(const struct inventory* inventory) {
	return inventory && inventory->items
		&& inventory->capacity >= INVENTORY_SLOT_ARMOR + INVENTORY_SIZE_ARMOR;
}

int armor_get_total_value(const struct inventory* inventory) {
	if(!armor_inventory_valid(inventory))
		return 0;
	int sum1 = 0, sum2 = 0, sum3 = 0;
	for(size_t slot = 0; slot < INVENTORY_SIZE_ARMOR; slot++) {
		struct item* item = armor_item_at(inventory, slot);
		if(!item)
			continue;
		struct item_data* stack = &inventory->items[INVENTORY_SLOT_ARMOR + slot];
		// CORRUPTION GUARD (not a Beta rule): legal play can never produce
		// durability > max_damage -- item_stack_damage's strict `>` check
		// (ItemStack.damageItem) breaks and clears the stack the instant a
		// hit would push it past max_damage, so this clamp changes NOTHING
		// for any legally-reachable state. It exists only so a hand-edited
		// or corrupt-NBT save (e.g. Damage=-1 read back as 65535 through the
		// uint16 field) can't drive `remain` deeply negative, which would
		// make sum2 negative, `(sum1-1)*sum2/sum3+1` swing hugely negative,
		// and `25 - totalArmorValue` explode well past 25 -- turning "armor"
		// into a damage AMPLIFIER for a single trivial hit. Floor at 0
		// (an already-broken/over-damaged piece protects for nothing, never
		// negative-nothing); the stored durability itself is never mutated.
		int remain = (int)item->max_damage - (int)stack->durability;
		if(remain < 0)
			remain = 0;
		sum1 += armor_reduction_by_slot[slot];
		sum2 += remain;
		sum3 += item->max_damage;
	}
	/* [740c583: InventoryPlayer.getTotalArmorValue] -- exact order and integer
	 * truncation: `(sum1 - 1) * sum2 / sum3 + 1`, with the zero guard. */
	return sum3 == 0 ? 0 : (sum1 - 1) * sum2 / sum3 + 1;
}

void armor_damage_items(struct inventory* inventory, int raw_damage) {
	if(!armor_inventory_valid(inventory))
		return;
	for(size_t slot = 0; slot < INVENTORY_SIZE_ARMOR; slot++) {
		if(armor_item_at(inventory, slot))
			(void)item_stack_damage(
				&inventory->items[INVENTORY_SLOT_ARMOR + slot], (uint16_t)raw_damage);
	}
}
