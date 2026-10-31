/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef GUI_CREATIVE_PICKER_LIST_H
#define GUI_CREATIVE_PICKER_LIST_H

#include <stdbool.h>
#include <stddef.h>

#include "../../item/item_types.h"

/* Creative phase-2 item picker LIST (docs/creative_mode.md phase 2) -- the
 * enumeration behind screen_creative_picker.c. NET-NEW addition (creative is
 * the sanctioned 2026-08-25 maintainer exception; Beta 1.7.3 has no creative
 * mode), so there is no decompile to be faithful to -- the design borrows
 * amahpour's creative-inventory IDEAS, re-implemented from scratch:
 * - ONE include-predicate shared by count()/entry() so they can never
 *   disagree (a cell either renders as a real item or is not offered).
 * - "Featured placeables first": a hand-curated build-blocks section leads,
 *   then every remaining registered item follows in ascending id order.
 *
 * Durability variants: the ascending sweep expands ITEM_DYE into its 16
 * Beta damage colors (the dye icon/behaviour is durability-driven, proven by
 * the debug.give kit's lapis/bonemeal entries). Wool stays a single white
 * entry: Beta 1.7.3 colors wool via BLOCK METADATA (dye-on-sheep/crafting),
 * and our block placement does not map item durability onto metadata --
 * offering 16 wool cells would grant stacks that all place white. Colored
 * wool in creative therefore still comes from dyes + crafting, like
 * survival; a meta-aware placement path is a documented follow-up. */

/* Number of entries the picker offers. */
size_t creative_picker_count(void);

/* Entry `index` (featured section first, then the ascending sweep) as a
 * grantable stack: id + durability set, count = the item's max_stack.
 * Returns false when index is out of range. */
bool creative_picker_entry(size_t index, struct item_data* out);

#endif
