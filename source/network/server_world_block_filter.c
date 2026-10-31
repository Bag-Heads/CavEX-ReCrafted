/*
	Copyright (c) 2026

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/*
	server_world_block_needs_neighbours — extracted from server_world.c
	so it can be unit-tested without dragging in NBT, region_archive,
	libfat, and the rest of the server world's heavy dependency cone.

	Contract: returns true iff `type` is a block whose onWorldTick
	handler dereferences `info->neighbours`. Callers (see
	server_world.c::server_world_fire_scheduled_tick) use this to
	decide whether to populate the 6-element neighbour array before
	dispatching to the block's onWorldTick.

	If a future block adds an onWorldTick that reads
	`info->neighbours[side]`, its type MUST be added here. Otherwise
	the handler's `if (!info->neighbours) return;` guard will silently
	skip every scheduled tick and the block will appear to be inert.

	test_scheduled_tick_parity.c enforces this contract for the
	current set of neighbour-reading onWorldTick handlers.
*/

#include "../block/blocks_data.h"
#include "server_world.h"

bool server_world_block_needs_neighbours(uint8_t type) {
	return type == BLOCK_REDSTONE_WIRE || type == BLOCK_REDSTONE_TORCH
		|| type == BLOCK_WOOD_PRESSURE_PLATE
		|| type == BLOCK_STONE_PRESSURE_PLATE || type == BLOCK_DOOR_WOOD
		|| type == BLOCK_DOOR_IRON || type == BLOCK_RAIL
		|| type == BLOCK_POWERED_RAIL || type == BLOCK_DETECTOR_RAIL;
	// NOTE: the redstone repeater (93/94) is deliberately NOT here. Its onWorldTick
	// (redstone_repeater_server_tick) reads its input through the live world accessor, not
	// info->neighbours, so an allowlist entry would only waste a 6-cell neighbour fill per tick.
}
