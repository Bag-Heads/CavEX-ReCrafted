/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef MP_ENTITY_FLAGS_H
#define MP_ENTITY_FLAGS_H

/*
	Animation-parity pass (2026-09-03): two PURE wire-decode helpers, factored
	out of server_remote_protocol.c / client_interface.c so they are directly
	golden-testable without pulling in the whole client_interface.h/entity.h
	dependency tree (the render_biped_swing_pose.h / test_mp_visual_parity_pose.c
	precedent -- a header with ONLY <stdint.h>/<stdbool.h> deps that a tiny
	standalone test links against).

	1. mp_decode_entity_metadata_flags: the generic Entity DataWatcher flag
	   byte (index 0, type 0 byte) carried by Packet40 EntityMetadata.
	   [740c583 Entity.setEntityFlag/getEntityFlag, called from
	   Entity.onEntityUpdate: flag 0 = fire > 0 and
	   flag 2 = riding something, and
	   Entity.isBurning/isSneaking/isRiding, which read bits 0/1/2
	   respectively via `getEntityFlag(n) -> (byte0 & (1<<n)) != 0`]:
	     bit 0 (0x01) onFire
	     bit 1 (0x02) sneaking
	     bit 2 (0x04) isRiding
	   Used by server_remote_protocol.c's MP_EVENT_ENTITY_METADATA player
	   branch (item 1 of the pass predating this one already decoded bit 1;
	   items 3+4 add bits 0 and 2 from the SAME byte).

	2. mp_wire_armor_slot_to_type: Packet5PlayerInventory's wire slot (1..4)
	   for a player's equipped armor -> the enum armor_type ordinal
	   (item_types.h: HELMET=0, CHESTPLATE=1, LEGGINGS=2, BOOTS=3) it
	   represents. [740c583 EntityPlayerMP.getEquipmentInSlot: wire slot N
	   (1..4) -> inventory.armorInventory[N-1]; cross-checked against
	   RenderPlayer.setArmorModel's armorItemInSlot(3 - pass), which proves
	   armorInventory index 0=boots, 1=leggings, 2=chestplate, 3=helmet --
	   i.e. wire slot 1=boots, 2=leggings, 3=chestplate, 4=helmet]. Returns
	   -1 for slot 0 (the held item, not armor -- handled separately by the
	   caller) or any value outside 1..4.
*/

#include <stdbool.h>
#include <stdint.h>

struct mp_entity_metadata_flags {
	bool on_fire;
	bool sneaking;
	bool is_riding;
};

static inline struct mp_entity_metadata_flags
mp_decode_entity_metadata_flags(uint8_t byte0) {
	return (struct mp_entity_metadata_flags) {
		.on_fire = (byte0 & 0x01) != 0,
		.sneaking = (byte0 & 0x02) != 0,
		.is_riding = (byte0 & 0x04) != 0,
	};
}

static inline int mp_wire_armor_slot_to_type(uint8_t wire_slot) {
	switch(wire_slot) {
	case 1: return 3; // boots     (enum armor_type ARMOR_TYPE_BOOTS)
	case 2: return 2; // leggings  (ARMOR_TYPE_LEGGINGS)
	case 3: return 1; // chestplate(ARMOR_TYPE_CHESTPLATE)
	case 4: return 0; // helmet    (ARMOR_TYPE_HELMET)
	default: return -1;
	}
}

#endif
