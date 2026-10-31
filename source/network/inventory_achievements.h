/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#ifndef NETWORK_INVENTORY_ACHIEVEMENTS_H
#define NETWORK_INVENTORY_ACHIEVEMENTS_H
#include "../item/inventory.h"

/* Authoritative result callbacks; client mirrors and detached scratch copies
 * have no logic. Parent prerequisites and first-toast policy stay in B2. */
void inventory_achievement_craft_taken(struct inventory*, size_t slot,
	struct item_data result);
void inventory_achievement_furnace_taken(struct inventory*, size_t slot,
	struct item_data result);
void inventory_achievement_collected(struct inventory*, struct server_player*,
	struct item_data before);
#endif
