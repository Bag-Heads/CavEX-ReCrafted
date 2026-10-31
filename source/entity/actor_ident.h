/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2 (a CavEX fork).

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef ACTOR_IDENT_H
#define ACTOR_IDENT_H

#include <stdbool.h>
#include <stdint.h>

/* ===== S1-a2: LIFETIME-SAFE ACTOR IDENTITY ==============================
 * Beta holds a LIVE java reference for every "which player did this" role:
 * EntityCreature.entityToAttack / playerToAttack, EntityArrow.shootingEntity,
 * EntityFishHook.angler, Entity.riddenByEntity. A C port cannot retain the
 * pointer: the local split guest's `struct server_player` is heap-allocated
 * per admission and FREED on leave (server_local_guest.c guest_release), so a
 * retained pointer becomes a use-after-free the moment P2 leaves, and a
 * re-join would silently re-bind an old role to a new person.
 *
 * So a role stores this VALUE instead and re-resolves it every tick through
 * server_local_actor_resolve():
 *   slot 0, generation 0      -> the host actor (&s->player), always present
 *   slot 1, generation == G   -> the guest actor, valid ONLY while
 *                               s->guest_generation == G and s->guest_actor is
 *                               admitted to gameplay. Leave, death-release,
 *                               rejoin and a world switch all bump/zero that
 *                               counter, so resolution FAILS and the role
 *                               invalidates -- the explicit leave/death/rejoin
 *                               rule Astra's gate-1 item 5 asked for, by
 *                               construction rather than by a cleanup sweep.
 *
 * ACTOR_IDENT_HOST is the ZERO value, so every calloc'd / memset entity and
 * every struct initializer that predates these fields defaults to the host --
 * exactly the single-player behaviour these roles had before, which is why the
 * host path stays byte-identical (proved by test_split_off_equiv).
 * The generation is the FULL uint64 the server issues, never truncated: a
 * truncated copy could alias a future admission.
 *
 * This header is deliberately dependency-light (stdint/stdbool only, no
 * struct entity, no struct server_local) so BOTH entity.h and the opaque
 * server_local.h can take it without dragging one into the other -- the same
 * rule spawner_mob_id.h follows. */
#define ACTOR_SLOT_HOST 0u
#define ACTOR_SLOT_GUEST 1u
struct actor_ident {
	uint64_t generation; /* 0 for the host; the guest's admission generation */
	uint8_t slot;        /* ACTOR_SLOT_HOST / ACTOR_SLOT_GUEST */
};
#define ACTOR_IDENT_HOST ((struct actor_ident) {0})
static inline bool actor_ident_is_host(struct actor_ident id) {
	return id.slot == ACTOR_SLOT_HOST;
}
static inline bool actor_ident_equal(struct actor_ident a,
									 struct actor_ident b) {
	return a.slot == b.slot && a.generation == b.generation;
}

#endif
