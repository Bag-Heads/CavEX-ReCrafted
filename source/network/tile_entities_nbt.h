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

/*
	Per-chunk chest/sign tile entities <-> Beta 1.7.3 Level.TileEntities NBT.
	The data layer of the chest-store residency redesign (docs/chest_store_
	redesign.md): a plain, region_archive-agnostic struct the server fills from
	its chest/sign tables, plus build/parse to the faithful NBT compounds
	(Chest{x,y,z,Items:[{Slot,id,Count,Damage}]}, Sign{x,y,z,Text1..4}).

	Stage 1 is this serialization only (no lifecycle change yet); it is proven by
	a round-trip unit test before region_archive or the server use it.
*/

#ifndef TILE_ENTITIES_NBT_H
#define TILE_ENTITIES_NBT_H

#include <stdbool.h>
#include <stdint.h>

#include "../cNBT/nbt.h"	  // nbt_node, struct nbt_list
#include "../item/item_types.h" // struct item_data

#define TE_CHEST_SLOTS 54 // matches MAX_CHEST_SLOTS (single 27 / double 54)
#define TE_SIGN_SIZE 64	  // matches SIGN_SIZE
// CR-04: te_sign.text packs Beta's 4 independent signText[] lines into one
// TE_SIGN_SIZE(64)-byte grid -- TE_SIGN_LINE_COLS(16) columns per line,
// content confined to the first TE_SIGN_LINE_MAX(15) columns (the 16th column
// is a reserved blank). Mirrors item/inventory.h's SIGN_LINE_COLS(16)/
// SIGN_BETA_MAX_CHARS(15) without a #include, matching this file's existing
// dependency-light philosophy (same duplication server_remote.c's
// SIGN_EDIT_COLS already carries for the exact same fact). The 15-char cap is
// Beta's own: [740c583 TileEntitySign.readFromNBT] `if (signText[i].length()
// > 15) signText[i] = signText[i].substring(0, 15);` -- character-count only,
// no pixel-width term (maintainer ruling 2026-08-17: COUNT-only gate).
#define TE_SIGN_LINE_COLS 16
#define TE_SIGN_LINE_MAX 15
// The writer/reader index te_sign.text as exactly 4 TE_SIGN_LINE_COLS-wide
// lines with no bounds check beyond this identity -- if it ever drifts, the
// codec silently reads/writes out of the 64-byte grid.
_Static_assert(TE_SIGN_SIZE == 4 * TE_SIGN_LINE_COLS,
			   "te_sign.text must exactly cover 4 Beta sign lines");
#define TE_FURNACE_SLOTS 3 // TileEntityFurnace: input(0)/fuel(1)/output(2)
#define TE_DISPENSER_SLOTS 9 // TileEntityDispenser.getSizeInventory()
// FH-17: longest Beta EntityId string this port's spawner path recognises,
// plus a NUL. Mirrors spawner.h's SPAWNER_MOB_ID_MAX -- the two are
// independently defined (dependency-light duplication, matching this file's
// own TE_SIGN_LINE_COLS/TE_SIGN_LINE_MAX precedent) rather than one
// #including the other.
#define TE_SPAWNER_MOB_ID_MAX 24

/* Corrupt NBT must not turn a chunk load into unbounded work. IMP-TE-CAPS
 * (2026-10-21): tile_entities_parse/build no longer cap any kind (Beta's
 * Chunk.chunkTileEntityMap is unbounded and the server tables grow); every
 * list is bounded only by TE_RECORDS_EXAMINED_MAX, the number of distinct
 * block positions in a 16x16x128 Beta chunk. The per-kind values below are
 * kept ONLY as the fixed scratch sizes of two legacy migrations
 * (region_archive.c iron-chest / spawner migration) and the client sign-text
 * store's per-chunk erase batch -- they are not tile-entity limits. */
#define TE_CHEST_RECORDS_MAX 256
#define TE_SIGN_RECORDS_MAX 256
#define TE_SPAWNER_RECORDS_MAX 64
#define TE_PISTON_RECORDS_MAX (16 * 16 * 128)
#define TE_RECORDS_EXAMINED_MAX TE_PISTON_RECORDS_MAX
#define TE_ITEM_RECORDS_EXAMINED_MAX 128

struct te_chest {
	int32_t x, y, z;
	struct item_data items[TE_CHEST_SLOTS]; // id == 0 marks an empty slot
};

// Beta 1.7.3 Furnace compound (TileEntityFurnace.write/readFromNBT, decompile
// 740c583): {id:"Furnace",x,y,z,BurnTime:short,CookTime:short,Items:[{Slot,id,
// Count,Damage}]}. currentItemBurnTime is intentionally NOT stored -- Beta
// readFromNBT recomputes it from getItemBurnTime(fuel slot); the server load
// bridge does the same, so the on-disk format stays byte-faithful to Beta.
struct te_furnace {
	int32_t x, y, z;
	struct item_data items[TE_FURNACE_SLOTS]; // id == 0 marks an empty slot
	int16_t burn_time;						  // furnaceBurnTime  ("BurnTime")
	int16_t cook_time;						  // furnaceCookTime  ("CookTime")
};

// Beta 1.7.3 Dispenser compound (TileEntityDispenser.write/readFromNBT,
// decompile 740c583): {id:"Trap",x,y,z,Items:[{Slot,id,Count,Damage}]}. The
// NBT "id" string is "Trap" -- NOT "Dispenser" -- Beta's TileEntity class-name
// registry maps TileEntityDispenser.class to "Trap" (a naming holdover also
// visible in getInvName()==\"Trap\"); the client-visible GUI title is the
// UNRELATED literal "Dispenser" (GuiDispenser.drawGuiContainerForegroundLayer).
// dispenserRandom is intentionally NOT persisted here, matching Beta.
struct te_dispenser {
	int32_t x, y, z;
	struct item_data items[TE_DISPENSER_SLOTS]; // id == 0 marks an empty slot
};

// Beta 1.7.3 Note-block compound (TileEntityNote.write/readFromNBT, decompile
// 740c583): {id:"Music",x,y,z,note:byte}. The NBT "id" string is "Music"
// [TileEntity.<clinit> addMapping(TileEntityNote.class, "Music")]. note is
// ALWAYS written (no >0 gate, unlike the jukebox's Record tag below); on load
// it is clamped to [0,24] (a corrupt/hand-edited file could carry any signed
// byte) -- done in tile_entities_parse, matching readFromNBT's own clamp.
struct te_note {
	int32_t x, y, z;
	int8_t note;
};

// Beta 1.7.3 Jukebox compound (TileEntityRecordPlayer.write/readFromNBT,
// decompile 740c583): {id:"RecordPlayer",x,y,z,Record:int}. The NBT "id"
// string is "RecordPlayer" [TileEntity.<clinit>]. Record is written ONLY
// when > 0 (Beta: `if(this.record > 0) var1.setInteger("Record", ...)`) --
// replicate exactly for save-compat; readFromNBT's getInteger defaults to 0
// when the tag is absent OR malformed (Beta's own forgiving behaviour, not a
// CaveWii addition).
struct te_record_player {
	int32_t x, y, z;
	int32_t record; // item id (e.g. 2256/2257); 0 == no record
};

// FH-17: Beta 1.7.3 mob-spawner compound (TileEntityMobSpawner.write/
// readFromNBT, decompile 740c583): {id:"MobSpawner",x,y,z,EntityId:string,
// Delay:short}. The NBT "id" string is "MobSpawner" [TileEntity.<clinit>
// addMapping(TileEntityMobSpawner.class, "MobSpawner")]. EntityId is always
// written verbatim (mob_id == "" persists an empty string, matching Beta's
// own "" TileEntity.readFromNBT default for a missing/malformed tag -- both
// are the faithful "inert, never fires" state). yaw/yaw2 (render-only spin)
// are Beta transient fields, never persisted, and have no runtime/render
// counterpart here either.
struct te_spawner {
	int32_t x, y, z;
	char mob_id[TE_SPAWNER_MOB_ID_MAX]; // "EntityId" (NUL-terminated)
	int16_t delay;						 // "Delay"
};

struct te_sign {
	int32_t x, y, z;
	// TE_SIGN_SIZE data bytes + 1 for a nul terminator. CR-04: this is the
	// packed 4-line grid described above (TE_SIGN_LINE_COLS-wide lines,
	// content <= TE_SIGN_LINE_MAX chars), NOT one flat string -- the build
	// splits it into Beta's Text1..Text4 tags and the parse reassembles it
	// from them, matching TileEntitySign.write/readFromNBT [740c583].
	char text[TE_SIGN_SIZE + 1];
};

// Beta 1.7.3 Piston compound (TileEntityPiston.write/readFromNBT, decompile 740c583):
// {id:"Piston",x,y,z,blockId:int,blockData:int,facing:int,progress:float,extending:byte}. CaveWii's
// runtime store keeps Beta's exact leading/current and lagging/last floats. writeToNBT saves the lagging
// value; readFromNBT restores both values from it. field_31023_j (source) is NOT persisted by Beta and
// is dropped here too (defaults to false on load).
struct te_piston_moving {
	int32_t x, y, z;
	int32_t stored_id;	 // "blockId"   (the block being pushed/pulled)
	int32_t stored_meta; // "blockData" (its metadata)
	int32_t facing;		 // "facing"    (0-5)
	float progress;		 // "progress"  (0.0 .. 1.0; lagging progress)
	int8_t extending;	 // "extending" (boolean: 1 = push, 0 = pull)
};

// A chunk's tile entities, owned by the caller. n_* == 0 with NULL arrays is a
// valid "empty" value.
// ADDING A TE KIND? Its count MUST also go into tile_entities_any() below and
// tile_entities_build's validation -- see the D12 note on tile_entities_any.
struct chunk_tile_entities {
	/* True only when this is a full image after merging legacy flat rows. Raw
	 * parsed old chunks keep false; otherwise a recovery/tool rewrite could mark
	 * an incomplete empty list authoritative and erase unmigrated owners. */
	bool legacy_flat_complete;
	struct te_chest* chests;
	int n_chest;
	struct te_sign* signs;
	int n_sign;
	struct te_furnace* furnaces;
	int n_furnace;
	struct te_dispenser* dispensers;
	int n_dispenser;
	struct te_note* notes;
	int n_note;
	struct te_record_player* record_players;
	int n_record_player;
	struct te_spawner* spawners;
	int n_spawner;
	struct te_piston_moving* piston_movings;
	int n_piston_moving;
};

// THE "anything to persist?" predicate over every TE kind -- the single source
// of truth for three call sites: the chunk-save write_required decision
// (server_world), the region writer's failed-build-vs-genuinely-empty
// distinction (region_archive), and the serializer's empty early-out
// (tile_entities_build). It exists because per-site kind enumerations DRIFT:
// TE content edits never set c->modified, so this predicate is the only thing
// persisting a contents-only edit -- and the dispenser slice missed two of the
// three hand-rolled copies (D12, 2026-08-14: contents-only dispenser edits were
// silently lost on eviction, and a dispenser-only chunk whose NBT build failed
// was committed as EMPTY instead of failing the save). A new kind added here is
// covered at all three sites at once.
static inline bool tile_entities_any(const struct chunk_tile_entities* te) {
	return te
		&& (te->n_chest != 0 || te->n_sign != 0 || te->n_furnace != 0
			|| te->n_dispenser != 0 || te->n_note != 0
			|| te->n_record_player != 0 || te->n_spawner != 0
			|| te->n_piston_moving != 0);
}

// Build a Beta-faithful TileEntities TAG_LIST from te. Every supported record
// must belong to (chunk_x, chunk_z) and have a world-valid y coordinate.
// Returns the list sentinel to assign to the chunk NBT's TileEntities tag, plus
// an opaque arena that MUST be freed with tile_entities_build_free AFTER the NBT
// is dumped (the nodes live in the arena, not the cNBT tree). Returns NULL (and
// *arena_out = NULL) when te has no entries, when its counts/pointers are
// invalid, or on allocation failure. A caller saving a non-empty value must
// therefore treat NULL as a failed save. Only non-empty item slots are emitted.
struct nbt_list* tile_entities_build(const struct chunk_tile_entities* te,
									 int32_t chunk_x, int32_t chunk_z,
									 void** arena_out);
void tile_entities_build_free(void* arena);
/* Merge an existing TileEntities list off-tree. Caller owns the returned node. */
nbt_node* tile_entities_preserve(const nbt_node* original,
	const nbt_node* generated, const struct chunk_tile_entities* current, bool native, int32_t x, int32_t z);

// Parse a chunk's TileEntities TAG_LIST into out (mallocs exact-sized arrays;
// free with tile_entities_parse_free). Unknown entries are skipped; malformed
// supported owners, including records outside the owning chunk, reject the
// complete parse transaction. Ambiguous Items records instead occupy pinned
// slots; they can only be saved by merging their unchanged original bytes.
// A NULL or non-list node yields a successful empty result. Allocation failure
// or a supported-record/bounded-scan overflow returns false with out empty.
bool tile_entities_parse(const nbt_node* te_list, int32_t chunk_x,
						 int32_t chunk_z,
						 struct chunk_tile_entities* out);
void tile_entities_parse_free(struct chunk_tile_entities* out);

#ifdef CAVEWII_TILE_ENTITIES_NBT_TEST
// Fail one codec allocation after this many successful codec allocations.
// One-shot; production builds omit the hook.
void tile_entities_nbt_test_fail_allocation_after(unsigned successful_allocations);
#endif

#endif
