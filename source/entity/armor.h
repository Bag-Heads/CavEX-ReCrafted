#ifndef ARMOR_H
#define ARMOR_H

#include <stdint.h>

struct inventory;

/* FAITHFUL SIMULATION -- [740c583: InventoryPlayer.getTotalArmorValue].
 * Computes sum3 == 0 ? 0 : (sum1 - 1) * sum2 / sum3 + 1 using C's truncating
 * integer division.  Only ItemArmor instances in the four worn slots count. */
int armor_get_total_value(const struct inventory* inventory);

/* FAITHFUL SIMULATION -- [740c583: InventoryPlayer.damageArmor].  Applies the
 * RAW pre-mitigation amount (Beta half-heart units -- the CALLER is
 * responsible for converting out of any ×8-scaled damage domain before
 * calling this) to every worn ItemArmor stack; item_stack_damage's strict
 * ItemStack.damageItem break rule clears a broken single-item slot.
 * `int`, not `uint16_t`: the caller's division (e.g. raw_damage / 8) is done
 * before this call, so widen the parameter to remove the silent-truncation
 * cast site rather than narrow an already-small value back down. */
void armor_damage_items(struct inventory* inventory, int raw_damage);

#endif
