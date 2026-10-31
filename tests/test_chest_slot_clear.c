/*
	test_chest_slot_clear -- pins the C2 item-dupe fix.

	The dupe: chest_items[MAX_CHESTS][MAX_CHEST_SLOTS=54] is a SHARED pool drawn
	by both wooden (27 slots) and iron (54 slots) chests, and a freed chest row is
	NOT zeroed on break. The wooden-chest place cleared only the first 27 of 54
	(block_chest.c) -- so a row recycled from a freed iron chest kept its items in
	slots 27..53, and the wooden break loop (bound MAX_CHEST_SLOTS=54) then dropped
	those stale items = duplication. Beta's TileEntityChest.getSizeInventory()==27,
	so a single chest only ever stores/drops 27 slots and rows are per-tile-entity
	(no shared pool, no cross-chest contamination).

	Fix (block_chest.c): (1) place clears the WHOLE row (sizeof(row)); (2) the
	wooden drop loop is bounded by CHEST_SIZE_STORAGE(27), not MAX_CHEST_SLOTS(54).
	This test mirrors those two invariants on a local row -- the live
	onItemPlace/getDroppedItem path drags in the whole server, so it is pinned by
	logic equivalence (the project's established pattern).

	Header-only: no .c linked.

	--- facing quadrant (block_chest.c onItemPlace, added alongside the slot-clear
	fix above; same file, same "mirror the live logic, don't link the server"
	convention) ---

	BUG: onItemPlace derived the open-space facing from `sinf(s->player.rx),
	cosf(s->player.rx)` -- but player.rx is Beta's rotationYaw in DEGREES (every
	other look-vector site in this codebase, e.g. server_local.c:2199/8768/8848/
	12805, converts with `glm_rad(-s->player.rx)` first). Feeding degrees straight
	into sinf/cosf is feeding the wrong unit into a transcendental function, so
	the resulting quadrant is essentially garbage except near yaw==0.

	FIX: block_chest.c now does `float ang = glm_rad(-s->player.rx); float lx =
	sinf(ang), lz = cosf(ang);` before the same fabsf(lx)>fabsf(lz) facing
	decision tree.

	Ground truth: BlockChest itself has NO onBlockPlacedBy override in the real
	1.7.3 decompile (740c583) -- Block's default is a no-op, so Beta's stock
	chest "facing" is actually a render-time neighbour-opacity auto-orientation,
	not a yaw-derived metadata (BlockChest.getBlockTexture's var13/var14 logic).
	CaveWii's chest already stores an explicit yaw-derived facing meta instead
	(needed to keep a double chest's two halves agreeing on one facing, see
	block_chest.c ~282-293), so the citable Beta ground truth for what THAT
	yaw-to-quadrant math should compute is the identical formula Beta uses on
	the sibling directional blocks that DO override onBlockPlacedBy with a
	stored facing: BlockFurnace.onBlockPlacedBy and BlockPistonBase.
	determineOrientation, both `MathHelper.floor_double(yaw*4.0/360.0+0.5)&3`
	(mirrored in this repo as furnace_facing_meta() / piston_determine_
	orientation()). That integer var6 (0/1/2/3 = north/east/south/west, i.e.
	-Z/+X/+Z/-X) lines up 1:1 with chest's own front-axis metadata convention
	(getTextureIndex: 0=-Z, 1=+X, 2=+Z, 3=-X), so the fixed lx/lz branches below
	must select the SAME facing that formula gives at each cardinal yaw.

	Mirrors block_chest.c's onItemPlace (not linked -- it drags in the whole
	server, per this file's own precedent above) with the exact fixed formula,
	and separately shows the pre-fix formula (degrees fed straight into sinf/
	cosf) misses 3 of the 4 cardinal yaws -- proving the bug this fix closes.
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../source/cglm/util.h" // glm_rad
#include "../source/item/inventory.h"
#include "../source/item/items.h"

// Mirrors block_chest.c onItemPlace's open-space (n_chest==0) facing decision
// tree EXACTLY, given already-computed lx/lz look-direction components.
// Chest facing meta convention (getTextureIndex): 0=-Z, 1=+X, 2=+Z, 3=-X.
static uint8_t chest_open_facing(float lx, float lz) {
	if(fabsf(lx) > fabsf(lz))
		return (lx > 0.0F) ? 3 : 1;
	else
		return (lz > 0.0F) ? 0 : 2;
}

// (C) FIX: yaw (degrees) -> radians via glm_rad(-yaw), matching every other
// look-vector site in this codebase (server_local.c:2199 et al).
static uint8_t chest_facing_fixed(float yaw_deg) {
	float ang = glm_rad(-yaw_deg);
	return chest_open_facing(sinf(ang), cosf(ang));
}

// BUG (pre-fix): degrees fed straight into sinf/cosf as if already radians.
static uint8_t chest_facing_buggy(float yaw_deg) {
	return chest_open_facing(sinf(yaw_deg), cosf(yaw_deg));
}

// Ground truth: Beta's own yaw-quadrant classifier, MathHelper.floor_double(
// yaw*4.0/360.0+0.5)&3 -- BlockFurnace.onBlockPlacedBy / BlockPistonBase.
// determineOrientation (both real 1.7.3 overrides; BlockChest has none, see
// the file header). var6 (0/1/2/3 = N/E/S/W = -Z/+X/+Z/-X) lines up 1:1 with
// chest's own facing meta convention, so this IS the expected chest facing.
static int beta_yaw_quadrant(float yaw_deg) {
	return ((int)floorf(yaw_deg / 90.0F + 0.5F)) & 3;
}

static void check_facing_quadrant(void) {
	const float yaws[4] = {0.0F, 90.0F, 180.0F, 270.0F};
	int mismatched_before_fix = 0;
	for(int i = 0; i < 4; i++) {
		uint8_t want = (uint8_t)beta_yaw_quadrant(yaws[i]);
		uint8_t got = chest_facing_fixed(yaws[i]);
		assert(got == want && "fixed onItemPlace facing must match Beta's yaw quadrant");

		if(chest_facing_buggy(yaws[i]) != want)
			mismatched_before_fix++;
	}
	// The bug this fix closes: with degrees fed straight into sinf/cosf, 3 of
	// the 4 cardinal yaws land on the wrong quadrant (only yaw==0 survives,
	// since sinf(0)==sinf(glm_rad(-0))==0 regardless of unit).
	assert(mismatched_before_fix == 3
		   && "the pre-fix formula was expected to miss yaw 90/180/270");
	printf("  [facing] yaw 0/90/180/270 -> meta 0/1/2/3 (glm_rad(-yaw) fix); "
		   "pre-fix formula missed %d/4\n",
		   mismatched_before_fix);
}

// IRON_CHEST_SIZE_STORAGE(54) == the server-side MAX_CHEST_SLOTS pool width;
// CHEST_SIZE_STORAGE(27) == a single (wooden) chest's own storage == Beta
// getSizeInventory(). Pin both so the bug's preconditions can't silently drift.
int main(void) {
	check_facing_quadrant();

	assert(CHEST_SIZE_STORAGE == 27);
	assert(IRON_CHEST_SIZE_STORAGE == 54);

	struct item_data row[IRON_CHEST_SIZE_STORAGE]; // == MAX_CHEST_SLOTS row

	// (A) PLACE clear must zero the WHOLE row. Prefill all 54 (a freed iron-chest
	// row), then apply the fixed place-clear; every slot must be empty.
	for(int j = 0; j < IRON_CHEST_SIZE_STORAGE; j++)
		row[j] = (struct item_data) {.id = 1, .count = 64, .durability = 0};
	memset(&row, 0, sizeof(row)); // the FIX (was: 27 * sizeof(struct item_data))
	for(int j = 0; j < IRON_CHEST_SIZE_STORAGE; j++)
		assert(row[j].id == 0 && row[j].count == 0);

	// Document the bug: the old partial clear (27) would have left 27..53 dirty.
	for(int j = 0; j < IRON_CHEST_SIZE_STORAGE; j++)
		row[j] = (struct item_data) {.id = 1, .count = 64, .durability = 0};
	memset(&row, 0, 27 * sizeof(struct item_data)); // the OLD buggy clear
	int leftover = 0;
	for(int j = 0; j < IRON_CHEST_SIZE_STORAGE; j++)
		if(row[j].id != 0)
			leftover++;
	assert(leftover == IRON_CHEST_SIZE_STORAGE - CHEST_SIZE_STORAGE); // 27 stale

	// (B) A single (wooden) chest's break loop must be bounded by 27, so it can
	// NEVER drop a stale slot 27..53. Fill all 54, mark id=7 in 0..26 and id=42
	// in 27..53; the 27-bounded scan must visit only 0..26 and never see id=42.
	for(int j = 0; j < CHEST_SIZE_STORAGE; j++)
		row[j] = (struct item_data) {.id = 7, .count = 1, .durability = 0};
	for(int j = CHEST_SIZE_STORAGE; j < IRON_CHEST_SIZE_STORAGE; j++)
		row[j] = (struct item_data) {.id = 42, .count = 1, .durability = 0};

	int dropped = 0, saw_stale = 0;
	for(int j = 0; j < CHEST_SIZE_STORAGE; j++) { // the FIXED drop bound
		if(row[j].id) {
			dropped++;
			if(row[j].id == 42)
				saw_stale = 1;
		}
	}
	assert(dropped == CHEST_SIZE_STORAGE); // exactly its own 27 slots
	assert(saw_stale == 0);                // never a stale 27..53 slot

	puts("test_chest_slot_clear: OK");
	return 0;
}
