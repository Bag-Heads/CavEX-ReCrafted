/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef SERVER_CREATIVE_H
#define SERVER_CREATIVE_H

#include "server_interface.h"
#include "server_local.h"

/* Creative phase-2 SRPC_CREATIVE_GIVE handler (NET-NEW addition,
 * docs/creative_mode.md phase 2). Lives OUTSIDE server_local.c by the
 * three-lane agreement: the GUI lane owns this file; server_local.c carries
 * only the 3-line dispatch case. World rules untouched -- this is an
 * inventory operation, gated server-side on creative_mode.
 *
 * Semantics: validate the requested id/durability against the item registry,
 * then place a full stack (max_stack, server-chosen) into the first empty
 * hotbar slot, else the first empty main-inventory slot, and push the
 * authoritative slot to the client (the debug.give kit's exact CRPC shape).
 * A full inventory is a silent no-op -- predictable, no partial merges. */
void server_creative_handle_give(struct server_local* s,
								 const struct server_rpc* call);

#endif
