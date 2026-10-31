/* Copyright (c) 2026 CaveWii_v2 contributors */
/*
 * The Beta 1.7.3 statistic / achievement id table, as X-macro rows.
 *
 * ONE place holds the ids, parents, display keys, the client/server ownership
 * split and the canonical-block alias fold, so every consumer (state,
 * persistence, the event matrix in B2, the screens in B4/lane C) and every test
 * iterates the SAME table instead of re-typing a constant.
 *
 * Spec: docs/achievements_design.md sections 1.1 (the sixteen achievements),
 * 1.4 (the statistic ids and the "Beta owner" column) and 2 (the CaveWii data
 * model, canonical counter ids). Cited in-line as `[design 1.1]` etc. The
 * design already carries the decompile citations (AchievementList.<clinit>,
 * StatList.<clinit>, StatList.replaceAllSimilarBlocks, Block.<clinit>'s
 * disableStats() calls); this header does not re-derive them.
 *
 * Usage (the row macro is defined by the consumer, then #undef'd):
 *
 *	#define CW_STAT(id, key, type, owner) ... use id / key / type / owner ...
 *	CW_STAT_ROWS
 *	#undef CW_STAT
 *
 * `type` and `owner` arrive as the bare tokens SIMPLE|TIME|DISTANCE and
 * CLIENT|SERVER so a consumer can paste them into an enumerator name
 * (CW_STAT_TYPE_##type / CW_STAT_OWNER_##owner) or into a string.
 */

#ifndef CW_STATS_TABLE_H
#define CW_STATS_TABLE_H

#include <stdint.h>

/* ---- Array-counter bases [design 1.4, 4] --------------------------------
 * Beta's own ids, kept exactly so a remote Packet 200 maps 1:1 and the saved
 * file stays readable (design 1.4 opening line). The per-id counter for block
 * `b` is CW_STAT_MINE_BASE + b, and likewise for the three item arrays. */
#define CW_STAT_MINE_BASE 0x1000000u  /* mineBlock[block id], 256 entries */
#define CW_STAT_CRAFT_BASE 0x1010000u /* craftItem[item id] */
#define CW_STAT_USE_BASE 0x1020000u	  /* useItem[item id] */
#define CW_STAT_BREAK_BASE 0x1030000u /* breakItem[item id] */
/* Achievement i has stat id CW_ACH_BASE + i (Achievement.<init>). */
#define CW_ACH_BASE 0x500000u

/* "No counter at all": the return of stat_canonical_block() for a block whose
 * Beta registration was suppressed by Block.disableStats() [design 2]. 0xFFFF
 * is outside the 0..255 block-id range, so it can never collide with a real
 * canonical id. */
#define CW_STAT_BLOCK_NONE 0xFFFFu

enum cw_stat_type {
	CW_STAT_TYPE_SIMPLE = 0,   /* integer, thousands-separated */
	CW_STAT_TYPE_TIME = 1,	   /* ticks -> minutes (StatTypeTime) */
	CW_STAT_TYPE_DISTANCE = 2, /* centimetres -> m/km (StatTypeDistance) */
};

/* Beta's field_27088_g split [design 1.4 "Beta owner", design 6]: a
 * client-owned stat counts locally even on a remote server; a server-owned one
 * only ever arrives over Packet 200. B2/B3 consume this; B1 only records it. */
enum cw_stat_owner {
	CW_STAT_OWNER_CLIENT = 0,
	CW_STAT_OWNER_SERVER = 1,
};

/* ---- General statistics [design 1.4] ------------------------------------
 * Exactly the ids design 1.4 tabulates: 1000-1004, 1100, 2000-2008, 2010,
 * 2011, 2020-2025. The per-id array stats (mineBlock/craftItem/useItem/
 * breakItem) are NOT rows here -- they are the four bases above plus a flat
 * array in struct stats_profile.
 *
 * NOTE on the count: design section 2 says "26 ids"; the id list design 1.4
 * actually tabulates (and the list this brief repeats) has 23 entries, which is
 * also what Beta's StatList registers. The table is the authority and
 * CW_STAT_COUNT is DERIVED from it below, so no consumer can inherit the prose
 * miscount. */
#define CW_STAT_ROWS                                                           \
	CW_STAT(1000, "startGame", SIMPLE, CLIENT)                                 \
	CW_STAT(1001, "createWorld", SIMPLE, CLIENT)                               \
	CW_STAT(1002, "loadWorld", SIMPLE, CLIENT)                                 \
	CW_STAT(1003, "joinMultiplayer", SIMPLE, CLIENT)                           \
	CW_STAT(1004, "leaveGame", SIMPLE, CLIENT)                                 \
	CW_STAT(1100, "playOneMinute", TIME, CLIENT)                               \
	CW_STAT(2000, "walkOneCm", DISTANCE, CLIENT)                               \
	CW_STAT(2001, "swimOneCm", DISTANCE, CLIENT)                               \
	CW_STAT(2002, "fallOneCm", DISTANCE, CLIENT)                               \
	CW_STAT(2003, "climbOneCm", DISTANCE, CLIENT)                              \
	CW_STAT(2004, "flyOneCm", DISTANCE, CLIENT)                                \
	CW_STAT(2005, "diveOneCm", DISTANCE, CLIENT)                               \
	CW_STAT(2006, "minecartOneCm", DISTANCE, CLIENT)                           \
	CW_STAT(2007, "boatOneCm", DISTANCE, CLIENT)                               \
	CW_STAT(2008, "pigOneCm", DISTANCE, CLIENT)                                \
	CW_STAT(2010, "jump", SIMPLE, CLIENT)                                      \
	CW_STAT(2011, "drop", SIMPLE, CLIENT)                                      \
	CW_STAT(2020, "damageDealt", SIMPLE, SERVER)                               \
	CW_STAT(2021, "damageTaken", SIMPLE, SERVER)                               \
	CW_STAT(2022, "deaths", SIMPLE, SERVER)                                    \
	CW_STAT(2023, "mobKills", SIMPLE, SERVER)                                  \
	CW_STAT(2024, "playerKills", SIMPLE, SERVER)                               \
	CW_STAT(2025, "fishCaught", SIMPLE, SERVER)

/* ---- Achievements [design 1.1] ------------------------------------------
 * Registration order 0..15 (AchievementList.<clinit>); the index IS the Beta
 * registration index, so CW_ACH_BASE + i is the stat id. `parent` is the index
 * of the prerequisite or -1 for none. `icon` is the Beta item/block id the
 * grid draws. `special` = setSpecial() (special frame); `independent` =
 * field_27088_g, true for openInventory alone (the only achievement a
 * multiplayer client counts locally, design 1.2 point 4).
 *
 * `column`/`row` are Beta's displayColumn/displayRow, the remaining two
 * constructor arguments of `new Achievement(id, name, displayColumn,
 * displayRow, icon, parent)`. They live HERE, in the one table, rather than in
 * a parallel array in the screen layer: a seventeenth row then cannot take
 * openInventory's coordinates by accident (R1 lead item 3). Both are SIGNED
 * and both go negative. */
#define CW_ACH_ROWS                                                            \
	CW_ACH(0, "openInventory", -1, 340, 0, 1, 0, 0)                            \
	CW_ACH(1, "mineWood", 0, 17, 0, 0, 2, 1)                                   \
	CW_ACH(2, "buildWorkBench", 1, 58, 0, 0, 4, -1)                            \
	CW_ACH(3, "buildPickaxe", 2, 270, 0, 0, 4, 2)                              \
	CW_ACH(4, "buildFurnace", 3, 61, 0, 0, 3, 4)                               \
	CW_ACH(5, "acquireIron", 4, 265, 0, 0, 1, 4)                               \
	CW_ACH(6, "buildHoe", 2, 290, 0, 0, 2, -3)                                 \
	CW_ACH(7, "makeBread", 6, 297, 0, 0, -1, -3)                               \
	CW_ACH(8, "bakeCake", 6, 354, 0, 0, 0, -5)                                 \
	CW_ACH(9, "buildBetterPickaxe", 3, 274, 0, 0, 6, 2)                        \
	CW_ACH(10, "cookFish", 4, 350, 0, 0, 2, 6)                                 \
	CW_ACH(11, "onARail", 5, 66, 1, 0, 2, 3)                                   \
	CW_ACH(12, "buildSword", 2, 268, 0, 0, 6, -1)                              \
	CW_ACH(13, "killEnemy", 12, 352, 0, 0, 8, -1)                              \
	CW_ACH(14, "killCow", 12, 334, 0, 0, 7, -3)                                \
	CW_ACH(15, "flyPig", 14, 329, 1, 0, 8, -4)

/* ---- Canonical block aliasing [design 2] ---------------------------------
 * StatList.replaceAllSimilarBlocks aliases the array entry, so the two block
 * ids share ONE counter. Applied at count time by stat_canonical_block() (the
 * mine array) and stat_alias_block_in() (the three item arrays), so a saved
 * file only ever holds canonical ids.
 *
 * ROW ORDER AND DIRECTION ARE THE DECOMPILE'S, NOT THE DESIGN PROSE'S (B3).
 * Each row is one `replaceSimilarBlocks(array, i, j)` call of
 * StatList.replaceAllSimilarBlocks (StatList.java:141-152) written
 * `CW_BLOCK_ALIAS(i, j)`, and the fold direction is that method's own
 * (StatList.java:154-166):
 *
 *	 if(a[i] != null && a[j] == null) { a[j] = a[i]; }   // j takes i's counter
 *	 else                             { a[i] = a[j]; }   // i takes j's counter
 *
 * so in the ordinary case -- BOTH ids registered in that array -- the counter
 * is `j`'s and `i` folds onto it. The `else` also covers a[i] == null, where
 * both sides end up on a[j] (null included). The IF branch is the inversion
 * the craft array actually takes; CW_BLOCK_CRAFT_INVERTED_ROWS below.
 *
 * The water row was (8, 9) in B1/F1 and is now (9, 8): the call is
 * `replaceSimilarBlocks(astatbase, Block.waterStill.blockID,
 * Block.waterMoving.blockID)` (StatList.java:143), i.e. i = STILL (9),
 * j = MOVING (8), so still water folds onto MOVING water, not the other way
 * round. In the mine array both ids are disableStats() so the direction is
 * unobservable, which is how the prose reading survived B1; in useItem both
 * are registered and the counter is id 8's. The lava row is the no-op pair
 * Beta itself codes and is kept as coded rather than silently dropped. */
#define CW_BLOCK_ALIAS_ROWS                                                    \
	CW_BLOCK_ALIAS(9, 8)	/* still water    -> moving water */               \
	CW_BLOCK_ALIAS(11, 11)	/* lava still     -> itself (Beta's no-op) */      \
	CW_BLOCK_ALIAS(91, 86)	/* pumpkin lantern-> pumpkin */                    \
	CW_BLOCK_ALIAS(62, 61)	/* lit furnace    -> furnace */                    \
	CW_BLOCK_ALIAS(74, 73)	/* glowing r-ore  -> redstone ore */               \
	CW_BLOCK_ALIAS(94, 93)	/* active repeater-> idle repeater */              \
	CW_BLOCK_ALIAS(76, 75)	/* active r-torch -> idle torch */                 \
	CW_BLOCK_ALIAS(40, 39)	/* red mushroom   -> brown mushroom */             \
	CW_BLOCK_ALIAS(43, 44)	/* double slab    -> slab */                       \
	CW_BLOCK_ALIAS(2, 3)	/* grass          -> dirt */                       \
	CW_BLOCK_ALIAS(60, 3)	/* farmland       -> dirt */

/* Which of the four Beta arrays a fold is being asked about. The alias table
 * above is shared, but the DIRECTION a pair folds is a property of the array,
 * because replaceSimilarBlocks branches on which side is registered in THAT
 * array [design 2, B3]. */
enum cw_stat_array {
	CW_STAT_ARRAY_MINE = 0,	 /* StatList.func_25153_a, getEnableStats gate */
	CW_STAT_ARRAY_CRAFT = 1, /* StatList.func_25157_c, recipe outputs only  */
	CW_STAT_ARRAY_USE = 2,	 /* StatList.func_25155_a, every item id        */
	CW_STAT_ARRAY_BREAK = 3, /* StatList.func_25149_b, damagable items only */
	CW_STAT_ARRAY_COUNT
};

/* ---- The craftItem inversions [StatList.func_25157_c, B3] ----------------
 * craftItem (field_25158_z) is the ONE array that is not registered
 * id-by-id: func_25157_c collects the itemIDs of every CraftingManager recipe
 * output and every FurnaceRecipes smelting output into a HashSet and
 * registers only those (StatList.java:44-75), THEN calls
 * replaceAllSimilarBlocks. So for a pair whose SOURCE is a recipe output and
 * whose TARGET is not, `a[i] != null && a[j] == null` holds and Beta takes
 * the inverting branch: the TARGET takes the SOURCE's counter, and the shared
 * counter id is the SOURCE's.
 *
 * Exactly two of the eleven pairs are in that position, both verified against
 * the recipe lists:
 *
 *   (91 -> 86) jack-o-lantern / pumpkin. `CraftingManager.java:131` outputs
 *	 `new ItemStack(Block.pumpkinLantern, ...)`; no recipe and no smelting
 *	 recipe outputs Block.pumpkin (86). Shared counter = 91.
 *   (76 -> 75) active / idle redstone torch. `CraftingManager.java:170`
 *	 outputs `new ItemStack(Block.torchRedstoneActive, ...)`; nothing outputs
 *	 Block.torchRedstoneIdle (75). Shared counter = 76.
 *
 * The other nine pairs are NOT inverted: (62,61) furnace, (43,44) slab --
 * target registered (`RecipesCrafting.java:23`, `CraftingManager.java:70-79`),
 * source not, so the `else` branch folds source onto target as usual; and
 * (9,8) water, (11,11) lava, (74,73) redstone ore, (94,93) repeater (the
 * repeater RECIPE outputs Item.redstoneRepeater 356, not the block), (40,39)
 * mushrooms, (2,3) grass/dirt, (60,3) farmland/dirt -- neither side is a
 * recipe output, so Beta's fold is a no-op on two nulls and the direction is
 * unobservable. With the F3 gates those seven pairs have no craftItem counter
 * on EITHER side and stat_alias_block_in() answers CW_STAT_BLOCK_NONE for
 * them, which is what "two nulls" means.
 *
 * Written as its own row set, not as a second alias table, so the pairs and
 * their direction stay in ONE place: a row here must also be a
 * CW_BLOCK_ALIAS_ROWS row, and the _Static_assert in achievements_state.c
 * pins that. */
/* F3 NOTE: these two rows are no longer what DRIVES the fold. With the
 * registration gates implemented (CW_BLOCK_CRAFT_OUTPUT_ROWS below), the
 * inverting branch falls straight out of `a[i] != null && a[j] == null`, so
 * stat_alias_block_in() computes it instead of consulting a second table. The
 * rows stay as the cited EXPECTATION -- test_stats_counters asserts the
 * derived fold reproduces exactly these two inversions and no others -- so a
 * wrong output row cannot silently move a shared counter. */
#define CW_BLOCK_CRAFT_INVERT_ROWS                                             \
	CW_BLOCK_CRAFT_INVERT(91, 86) /* pumpkin takes the lantern's counter  */   \
	CW_BLOCK_CRAFT_INVERT(76, 75) /* idle torch takes the active's        */

/* ---- Beta's block-id range [Block.<clinit>, Block.java:759-854] ----------
 * `Block.blocksList` is populated for 1 (stone) .. 96 (trapdoor) with NO
 * gaps; index 0 is air and 97..255 are null. ALL FOUR registration loops
 * require a non-null entry there -- func_25153_a tests `Block.blocksList[j]
 * != null` (StatList.java:86) and the other three test `Item.itemsList[l] !=
 * null` (StatList.java:70, 108, 134), which Block.java:863-869 fills with an
 * ItemBlock for exactly the non-null blocks. So a block id outside this range
 * has NO statistic in any of the four arrays.
 *
 * CaveWii registers blocks Beta does not (97 tree2d, 200 iron chest). LEAD
 * RULING (F3): a net-new block is outside the Beta table and gets NO
 * statistic -- not a counter of its own and not a fold onto someone else's. A
 * later card may add CaveWii-owned counter ids on top; it may not quietly
 * borrow Beta's. */
#define CW_BLOCK_BETA_FIRST 1
#define CW_BLOCK_BETA_LAST 96

/* ---- craftItem's registration set [StatList.func_25157_c, F3] ------------
 * func_25157_c (StatList.java:42-79) collects the itemID of every
 * CraftingManager recipe output (the loop at :50-53) and every FurnaceRecipes
 * smelting output (:56-59) into a HashSet and registers ONLY those. These are
 * the 39 members of that set that are block ids; every other block id has no
 * craftItem counter at all, which is why the golden's craft column reads "-"
 * for 213 of the 256 rows (39 registered + the four ids a fold reaches:
 * 86, 62, 75, 43).
 *
 * Derived by enumerating the decompile, not from memory: `Block.` outputs in
 * CraftingManager.<init>, the four RecipesCrafting rows, RecipesIngots'
 * storage-block loop (RecipesIngots.java:29-41 -- its FIRST addRecipe emits
 * the block), RecipesDyes' dyed cloth, and the two block outputs of
 * FurnaceRecipes.<init>. RecipesTools / RecipesWeapons / RecipesFood /
 * RecipesArmor output no block at all.
 *
 * B3's derivation header listed 38 of these: it dropped 67 (cobblestone
 * stairs, CraftingManager.java:158). The golden is regenerated from THIS
 * list. */
#define CW_BLOCK_CRAFT_OUTPUT_ROWS                                             \
	CW_BLOCK_CRAFT_OUTPUT(1)  /* stone       FurnaceRecipes.java:31 (cobble) */\
	CW_BLOCK_CRAFT_OUTPUT(5)  /* planks           CraftingManager.java:104  */ \
	CW_BLOCK_CRAFT_OUTPUT(20) /* glass       FurnaceRecipes.java:28 (sand)  */ \
	CW_BLOCK_CRAFT_OUTPUT(22) /* lapis block      RecipesIngots.java:24,35  */ \
	CW_BLOCK_CRAFT_OUTPUT(23) /* dispenser        CraftingManager.java:194  */ \
	CW_BLOCK_CRAFT_OUTPUT(24) /* sandstone        RecipesCrafting.java:29   */ \
	CW_BLOCK_CRAFT_OUTPUT(25) /* note block       CraftingManager.java:46   */ \
	CW_BLOCK_CRAFT_OUTPUT(27) /* powered rail     CraftingManager.java:122  */ \
	CW_BLOCK_CRAFT_OUTPUT(28) /* detector rail    CraftingManager.java:125  */ \
	CW_BLOCK_CRAFT_OUTPUT(29) /* sticky piston    CraftingManager.java:201  */ \
	CW_BLOCK_CRAFT_OUTPUT(33) /* piston           CraftingManager.java:197  */ \
	CW_BLOCK_CRAFT_OUTPUT(35) /* wool      CraftingManager:64 RecipesDyes:23*/ \
	CW_BLOCK_CRAFT_OUTPUT(41) /* gold block       RecipesIngots.java:18,35  */ \
	CW_BLOCK_CRAFT_OUTPUT(42) /* iron block       RecipesIngots.java:20,35  */ \
	CW_BLOCK_CRAFT_OUTPUT(44) /* slab             CraftingManager.java:70-79*/ \
	CW_BLOCK_CRAFT_OUTPUT(45) /* bricks           CraftingManager.java:58   */ \
	CW_BLOCK_CRAFT_OUTPUT(46) /* TNT              CraftingManager.java:67   */ \
	CW_BLOCK_CRAFT_OUTPUT(47) /* bookshelf        CraftingManager.java:49   */ \
	CW_BLOCK_CRAFT_OUTPUT(50) /* torch          CraftingManager.java:110,113*/ \
	CW_BLOCK_CRAFT_OUTPUT(53) /* wooden stairs    CraftingManager.java:152  */ \
	CW_BLOCK_CRAFT_OUTPUT(54) /* chest            RecipesCrafting.java:20   */ \
	CW_BLOCK_CRAFT_OUTPUT(57) /* diamond block    RecipesIngots.java:22,35  */ \
	CW_BLOCK_CRAFT_OUTPUT(58) /* workbench        RecipesCrafting.java:26   */ \
	CW_BLOCK_CRAFT_OUTPUT(61) /* furnace (idle)   RecipesCrafting.java:23   */ \
	CW_BLOCK_CRAFT_OUTPUT(65) /* ladder           CraftingManager.java:82   */ \
	CW_BLOCK_CRAFT_OUTPUT(66) /* rail             CraftingManager.java:119  */ \
	CW_BLOCK_CRAFT_OUTPUT(67) /* cobble stairs    CraftingManager.java:158  */ \
	CW_BLOCK_CRAFT_OUTPUT(69) /* lever            CraftingManager.java:167  */ \
	CW_BLOCK_CRAFT_OUTPUT(70) /* stone plate      CraftingManager.java:188  */ \
	CW_BLOCK_CRAFT_OUTPUT(72) /* wooden plate     CraftingManager.java:191  */ \
	CW_BLOCK_CRAFT_OUTPUT(76) /* r-torch (active) CraftingManager.java:170  */ \
	CW_BLOCK_CRAFT_OUTPUT(77) /* stone button     CraftingManager.java:185  */ \
	CW_BLOCK_CRAFT_OUTPUT(80) /* snow block       CraftingManager.java:52   */ \
	CW_BLOCK_CRAFT_OUTPUT(82) /* clay block       CraftingManager.java:55   */ \
	CW_BLOCK_CRAFT_OUTPUT(84) /* jukebox          CraftingManager.java:43   */ \
	CW_BLOCK_CRAFT_OUTPUT(85) /* fence            CraftingManager.java:40   */ \
	CW_BLOCK_CRAFT_OUTPUT(89) /* glowstone        CraftingManager.java:61   */ \
	CW_BLOCK_CRAFT_OUTPUT(91) /* jack-o-lantern   CraftingManager.java:131  */ \
	CW_BLOCK_CRAFT_OUTPUT(96) /* trapdoor         CraftingManager.java:88   */

/* Blocks Beta registers WITHOUT a mine counter: Block.<clinit> calls
 * disableStats() on them, so StatList.func_25153_a never creates the
 * StatBase and EntityPlayerSP.addStat returns early. Leaves are in the set
 * even though BlockLeaves.harvestBlock calls addStat [design 2]. */
#define CW_BLOCK_NO_STAT_ROWS                                                  \
	CW_BLOCK_NO_STAT(7)	 /* bedrock */                                         \
	CW_BLOCK_NO_STAT(8)	 /* water (moving) */                                  \
	CW_BLOCK_NO_STAT(9)	 /* water (still) */                                   \
	CW_BLOCK_NO_STAT(10) /* lava (moving) */                                   \
	CW_BLOCK_NO_STAT(11) /* lava (still) */                                    \
	CW_BLOCK_NO_STAT(18) /* leaves */                                          \
	CW_BLOCK_NO_STAT(26) /* bed */                                             \
	CW_BLOCK_NO_STAT(51) /* fire */                                            \
	CW_BLOCK_NO_STAT(52) /* mob spawner */                                     \
	CW_BLOCK_NO_STAT(55) /* redstone wire */                                   \
	CW_BLOCK_NO_STAT(59) /* crops */                                           \
	CW_BLOCK_NO_STAT(63) /* sign post */                                       \
	CW_BLOCK_NO_STAT(64) /* wooden door */                                     \
	CW_BLOCK_NO_STAT(68) /* wall sign */                                       \
	CW_BLOCK_NO_STAT(71) /* iron door */                                       \
	CW_BLOCK_NO_STAT(83) /* reeds */                                           \
	CW_BLOCK_NO_STAT(92) /* cake */                                            \
	CW_BLOCK_NO_STAT(93) /* repeater (idle) */                                 \
	CW_BLOCK_NO_STAT(94) /* repeater (active) */                               \
	CW_BLOCK_NO_STAT(96) /* trapdoor */

/* ---- craftItem ITEM registration set [StatList.func_25157_c, R2 MED 8] ---
 * The same HashSet as CW_BLOCK_CRAFT_OUTPUT_ROWS, for the ids at or above 256:
 * the itemID of every CraftingManager recipe output (StatList.java:50-53) and
 * every FurnaceRecipes smelting output (:56-59), registered at :75 behind
 * `Item.itemsList[id] != null`. An item id that is not an output has NO
 * craftItem counter, exactly as an uncraftable block id has none -- which is
 * what the module used to get wrong: item ids were ungated entirely, so
 * "crafting" an apple counted (R2 MED 8).
 *
 * Enumerated from the decompile (McBetaCpp/srcorigjava), never from memory.
 * Item ids are `256 + i` for `new Item(i)` (Item.java:32, `shiftedIndex =
 * 256 + i`). The four loop-driven recipe classes contribute every cell of
 * their tables: RecipesTools (20 tools + shears), RecipesWeapons (5 swords +
 * bow + arrow), RecipesArmor (20 pieces), RecipesIngots (the ingot/gem
 * "uncraft" half of each storage-block pair). The hand-derived golden
 * tests/golden/achievements/b4_item_registration.txt carries the per-id
 * citation and test_stats_counters pins this table to it. */
#define CW_ITEM_CRAFT_OUTPUT_ROWS                                              \
	CW_ITEM_CRAFT_OUTPUT(256) /* iron shovel     RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(257) /* iron pickaxe    RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(258) /* iron axe        RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(259) /* flint and steel CraftingManager.java:146   */ \
	CW_ITEM_CRAFT_OUTPUT(261) /* bow             RecipesWeapons.java:40     */ \
	CW_ITEM_CRAFT_OUTPUT(262) /* arrow           RecipesWeapons.java:43     */ \
	CW_ITEM_CRAFT_OUTPUT(263) /* coal (charcoal) FurnaceRecipes.java:33     */ \
	CW_ITEM_CRAFT_OUTPUT(264) /* diamond      RecipesIngots:38 Furnace:26   */ \
	CW_ITEM_CRAFT_OUTPUT(265) /* iron ingot   RecipesIngots:38 Furnace:24   */ \
	CW_ITEM_CRAFT_OUTPUT(266) /* gold ingot   RecipesIngots:38 Furnace:25   */ \
	CW_ITEM_CRAFT_OUTPUT(267) /* iron sword      RecipesWeapons.java:33     */ \
	CW_ITEM_CRAFT_OUTPUT(268) /* wooden sword    RecipesWeapons.java:33     */ \
	CW_ITEM_CRAFT_OUTPUT(269) /* wooden shovel   RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(270) /* wooden pickaxe  RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(271) /* wooden axe      RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(272) /* stone sword     RecipesWeapons.java:33     */ \
	CW_ITEM_CRAFT_OUTPUT(273) /* stone shovel    RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(274) /* stone pickaxe   RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(275) /* stone axe       RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(276) /* diamond sword   RecipesWeapons.java:33     */ \
	CW_ITEM_CRAFT_OUTPUT(277) /* diamond shovel  RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(278) /* diamond pickaxe RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(279) /* diamond axe     RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(280) /* stick           CraftingManager.java:107   */ \
	CW_ITEM_CRAFT_OUTPUT(281) /* bowl            CraftingManager.java:116   */ \
	CW_ITEM_CRAFT_OUTPUT(282) /* mushroom stew   RecipesFood.java:20,23     */ \
	CW_ITEM_CRAFT_OUTPUT(283) /* gold sword      RecipesWeapons.java:33     */ \
	CW_ITEM_CRAFT_OUTPUT(284) /* gold shovel     RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(285) /* gold pickaxe    RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(286) /* gold axe        RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(290) /* wooden hoe      RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(291) /* stone hoe       RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(292) /* iron hoe        RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(293) /* diamond hoe     RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(294) /* gold hoe        RecipesTools.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(297) /* bread           CraftingManager.java:149   */ \
	CW_ITEM_CRAFT_OUTPUT(298) /* leather helmet  RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(299) /* leather chest   RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(300) /* leather legs    RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(301) /* leather boots   RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(302) /* chain helmet    RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(303) /* chain chest     RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(304) /* chain legs      RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(305) /* chain boots     RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(306) /* iron helmet     RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(307) /* iron chest      RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(308) /* iron legs       RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(309) /* iron boots      RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(310) /* diamond helmet  RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(311) /* diamond chest   RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(312) /* diamond legs    RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(313) /* diamond boots   RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(314) /* gold helmet     RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(315) /* gold chest      RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(316) /* gold legs       RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(317) /* gold boots      RecipesArmor.java:39       */ \
	CW_ITEM_CRAFT_OUTPUT(320) /* cooked pork     FurnaceRecipes.java:28     */ \
	CW_ITEM_CRAFT_OUTPUT(321) /* painting        CraftingManager.java:161   */ \
	CW_ITEM_CRAFT_OUTPUT(322) /* golden apple    CraftingManager.java:164   */ \
	CW_ITEM_CRAFT_OUTPUT(323) /* sign            CraftingManager.java:94    */ \
	CW_ITEM_CRAFT_OUTPUT(324) /* wooden door     CraftingManager.java:85    */ \
	CW_ITEM_CRAFT_OUTPUT(325) /* bucket          CraftingManager.java:143   */ \
	CW_ITEM_CRAFT_OUTPUT(328) /* minecart        CraftingManager.java:128   */ \
	CW_ITEM_CRAFT_OUTPUT(330) /* iron door       CraftingManager.java:91    */ \
	CW_ITEM_CRAFT_OUTPUT(333) /* boat            CraftingManager.java:140   */ \
	CW_ITEM_CRAFT_OUTPUT(336) /* brick (item)    FurnaceRecipes.java:31     */ \
	CW_ITEM_CRAFT_OUTPUT(339) /* paper           CraftingManager.java:34    */ \
	CW_ITEM_CRAFT_OUTPUT(340) /* book            CraftingManager.java:37    */ \
	CW_ITEM_CRAFT_OUTPUT(342) /* chest minecart  CraftingManager.java:134   */ \
	CW_ITEM_CRAFT_OUTPUT(343) /* furnace minecrt CraftingManager.java:137   */ \
	CW_ITEM_CRAFT_OUTPUT(345) /* compass         CraftingManager.java:179   */ \
	CW_ITEM_CRAFT_OUTPUT(346) /* fishing rod     CraftingManager.java:155   */ \
	CW_ITEM_CRAFT_OUTPUT(347) /* clock           CraftingManager.java:176   */ \
	CW_ITEM_CRAFT_OUTPUT(350) /* cooked fish     FurnaceRecipes.java:29     */ \
	CW_ITEM_CRAFT_OUTPUT(351) /* dye  RecipesIngots:38 Dyes:28 Furnace:32   */ \
	CW_ITEM_CRAFT_OUTPUT(353) /* sugar           CraftingManager.java:101   */ \
	CW_ITEM_CRAFT_OUTPUT(354) /* cake            CraftingManager.java:97    */ \
	CW_ITEM_CRAFT_OUTPUT(355) /* bed             CraftingManager.java:204   */ \
	CW_ITEM_CRAFT_OUTPUT(356) /* repeater (item) CraftingManager.java:173   */ \
	CW_ITEM_CRAFT_OUTPUT(357) /* cookie          RecipesFood.java:26        */ \
	CW_ITEM_CRAFT_OUTPUT(358) /* map             CraftingManager.java:182   */ \
	CW_ITEM_CRAFT_OUTPUT(359) /* shears          RecipesTools.java:46       */

/* ---- breakItem registration set [StatList.func_25149_b, R2 MED 8] --------
 * func_25149_b registers `Item.itemsList[l] != null && ...isDamagable()`
 * (StatList.java:134), and isDamagable() is `maxDamage > 0 && !hasSubtypes`
 * (Item.java:115-118). Only these constructors ever set a non-zero maxDamage:
 *
 *   ItemTool.java:22      setMaxDamage(enumtoolmaterial.getMaxUses()) -- the
 *                         ItemSpade / ItemPickaxe / ItemAxe subclasses
 *   ItemSword.java:19     setMaxDamage(enumtoolmaterial.getMaxUses())
 *   ItemHoe.java:19       setMaxDamage(enumtoolmaterial.getMaxUses())
 *   ItemArmor.java:21     setMaxDamage(maxDamageArray[l] * 3 << j)
 *   ItemFishingRod.java:19    setMaxDamage(64)
 *   ItemFlintAndSteel.java:20 setMaxDamage(64)
 *   ItemShears.java:19        setMaxDamage(238)
 *
 * (EnumToolMaterial maxUses: WOOD 59, STONE 131, IRON 250, EMERALD 1561,
 * GOLD 32 -- all > 0.) Everything else keeps Item.java:28 `maxDamage = 0`; the
 * three subtype items that call setMaxDamage(0) explicitly -- ItemCoal,
 * ItemDye, ItemCloth -- are excluded twice over. The BOW is NOT damagable in
 * Beta 1.7.3: ItemBow sets only maxStackSize (ItemBow.java:16-20). Blocks
 * contribute nothing at all (see achievements_state.c). */
#define CW_ITEM_DAMAGABLE_ROWS                                                 \
	CW_ITEM_DAMAGABLE(256) /* iron shovel      ItemTool.java:22   IRON 250  */ \
	CW_ITEM_DAMAGABLE(257) /* iron pickaxe     ItemTool.java:22   IRON 250  */ \
	CW_ITEM_DAMAGABLE(258) /* iron axe         ItemTool.java:22   IRON 250  */ \
	CW_ITEM_DAMAGABLE(259) /* flint and steel  ItemFlintAndSteel:20      64 */ \
	CW_ITEM_DAMAGABLE(267) /* iron sword       ItemSword.java:19  IRON 250  */ \
	CW_ITEM_DAMAGABLE(268) /* wooden sword     ItemSword.java:19  WOOD 59   */ \
	CW_ITEM_DAMAGABLE(269) /* wooden shovel    ItemTool.java:22   WOOD 59   */ \
	CW_ITEM_DAMAGABLE(270) /* wooden pickaxe   ItemTool.java:22   WOOD 59   */ \
	CW_ITEM_DAMAGABLE(271) /* wooden axe       ItemTool.java:22   WOOD 59   */ \
	CW_ITEM_DAMAGABLE(272) /* stone sword      ItemSword.java:19  STONE 131 */ \
	CW_ITEM_DAMAGABLE(273) /* stone shovel     ItemTool.java:22   STONE 131 */ \
	CW_ITEM_DAMAGABLE(274) /* stone pickaxe    ItemTool.java:22   STONE 131 */ \
	CW_ITEM_DAMAGABLE(275) /* stone axe        ItemTool.java:22   STONE 131 */ \
	CW_ITEM_DAMAGABLE(276) /* diamond sword    ItemSword.java:19  EMER 1561 */ \
	CW_ITEM_DAMAGABLE(277) /* diamond shovel   ItemTool.java:22   EMER 1561 */ \
	CW_ITEM_DAMAGABLE(278) /* diamond pickaxe  ItemTool.java:22   EMER 1561 */ \
	CW_ITEM_DAMAGABLE(279) /* diamond axe      ItemTool.java:22   EMER 1561 */ \
	CW_ITEM_DAMAGABLE(283) /* gold sword       ItemSword.java:19  GOLD 32   */ \
	CW_ITEM_DAMAGABLE(284) /* gold shovel      ItemTool.java:22   GOLD 32   */ \
	CW_ITEM_DAMAGABLE(285) /* gold pickaxe     ItemTool.java:22   GOLD 32   */ \
	CW_ITEM_DAMAGABLE(286) /* gold axe         ItemTool.java:22   GOLD 32   */ \
	CW_ITEM_DAMAGABLE(290) /* wooden hoe       ItemHoe.java:19    WOOD 59   */ \
	CW_ITEM_DAMAGABLE(291) /* stone hoe        ItemHoe.java:19    STONE 131 */ \
	CW_ITEM_DAMAGABLE(292) /* iron hoe         ItemHoe.java:19    IRON 250  */ \
	CW_ITEM_DAMAGABLE(293) /* diamond hoe      ItemHoe.java:19    EMER 1561 */ \
	CW_ITEM_DAMAGABLE(294) /* gold hoe         ItemHoe.java:19    GOLD 32   */ \
	CW_ITEM_DAMAGABLE(298) /* leather helmet   ItemArmor.java:21  11*3<<0   */ \
	CW_ITEM_DAMAGABLE(299) /* leather chest    ItemArmor.java:21  16*3<<0   */ \
	CW_ITEM_DAMAGABLE(300) /* leather legs     ItemArmor.java:21  15*3<<0   */ \
	CW_ITEM_DAMAGABLE(301) /* leather boots    ItemArmor.java:21  13*3<<0   */ \
	CW_ITEM_DAMAGABLE(302) /* chain helmet     ItemArmor.java:21  11*3<<1   */ \
	CW_ITEM_DAMAGABLE(303) /* chain chest      ItemArmor.java:21  16*3<<1   */ \
	CW_ITEM_DAMAGABLE(304) /* chain legs       ItemArmor.java:21  15*3<<1   */ \
	CW_ITEM_DAMAGABLE(305) /* chain boots      ItemArmor.java:21  13*3<<1   */ \
	CW_ITEM_DAMAGABLE(306) /* iron helmet      ItemArmor.java:21  11*3<<2   */ \
	CW_ITEM_DAMAGABLE(307) /* iron chest       ItemArmor.java:21  16*3<<2   */ \
	CW_ITEM_DAMAGABLE(308) /* iron legs        ItemArmor.java:21  15*3<<2   */ \
	CW_ITEM_DAMAGABLE(309) /* iron boots       ItemArmor.java:21  13*3<<2   */ \
	CW_ITEM_DAMAGABLE(310) /* diamond helmet   ItemArmor.java:21  11*3<<3   */ \
	CW_ITEM_DAMAGABLE(311) /* diamond chest    ItemArmor.java:21  16*3<<3   */ \
	CW_ITEM_DAMAGABLE(312) /* diamond legs     ItemArmor.java:21  15*3<<3   */ \
	CW_ITEM_DAMAGABLE(313) /* diamond boots    ItemArmor.java:21  13*3<<3   */ \
	CW_ITEM_DAMAGABLE(314) /* gold helmet      ItemArmor.java:21  11*3<<1   */ \
	CW_ITEM_DAMAGABLE(315) /* gold chest       ItemArmor.java:21  16*3<<1   */ \
	CW_ITEM_DAMAGABLE(316) /* gold legs        ItemArmor.java:21  15*3<<1   */ \
	CW_ITEM_DAMAGABLE(317) /* gold boots       ItemArmor.java:21  13*3<<1   */ \
	CW_ITEM_DAMAGABLE(346) /* fishing rod      ItemFishingRod:19        64  */ \
	CW_ITEM_DAMAGABLE(359) /* shears           ItemShears.java:19      238  */

/* The first ITEM id: Beta `shiftedIndex = 256 + i` (Item.java:32). Below it an
 * id is a block and the four block tables above decide; at or above it the two
 * item tables do. */
#define CW_ITEM_FIRST 256

/* ---- Event ordering class [design 5.2 v4, R2 HIGH 1] ---------------------
 * Coalescing may never REORDER or APPROXIMATE an event that can affect an
 * achievement or a notice. Every kind in game_event.h is therefore classified
 * exactly once, HERE, and the consumer's table is built from these rows so a
 * new kind cannot be added without a class (the _Static_assert in
 * achievements.c pins the row count against GE_KIND_COUNT).
 *
 *   ORDERED  -- it can change an achievement or a notice, so it must be
 *               applied in emit order. Past a full main ring it goes to the
 *               ordered overflow ring; it NEVER reaches the coalescer.
 *   FOLDABLE -- a pure counter whose order is irrelevant; past a full main
 *               ring it folds into the per-slot coalescer.
 *
 * A ROW IS THE STRICTEST CASE OF ITS KIND, NOT THE WHOLE ANSWER (R2 HIGH 1).
 * v3 read these rows as the final word, so EVERY pickup, EVERY craft take and
 * EVERY kill was ORDERED -- and the 64-events-per-player-per-tick envelope
 * says nothing about how many of those 64 are pickups. Two players collecting
 * a dozen item entities a tick across a ten-second stall produce thousands of
 * ORDERED events and hit ACH_REJECT_OVERFLOW INSIDE the supported envelope.
 * The fix: the consumer classifies at EMIT TIME from the row PLUS the payload,
 * and only the achievement-relevant payloads are ordered:
 *
 *   GE_ITEM_PICKED_UP  ORDERED only for wood 17 (mineWood) and leather 334
 *                      (killCow). Beta has NO items-picked-up statistic, so
 *                      every other pickup is a NO-OP and the consumer drops it
 *                      before it ever queues (counted in ach_status.
 *                      noop_events, which is not a rejection: nothing was
 *                      lost, there was nothing to keep).
 *   GE_CRAFT_TAKEN     ORDERED only for the eight SlotCrafting achievement ids
 *                      (58, 270, 61, 290, 297, 354, 274, 268); every other
 *                      take is a pure craftItem[] counter and FOLDS.
 *   GE_FURNACE_TAKEN   ORDERED only for iron 265 and cooked fish 350.
 *   GE_KILLED          ORDERED only for the EntityMob set killEnemy reads;
 *                      every other kill is a pure mobKills counter and FOLDS.
 *   GE_RIDE            ORDERED only when sub == GE_MOUNT_MINECART (the onARail
 *                      origin / squared-1000 rule). A boat or pig ride is a
 *                      pure distance counter.
 *   GE_REMOTE_STAT     ORDERED only when its stat_id is an achievement id
 *                      (CW_ACH_BASE + i): that goes through the parent check
 *                      and the toast. Every other id is a pure counter.
 *
 * Each of those is listed ORDERED below -- the strictest of its cases -- and
 * the refinement only ever moves a payload from the overflow ring to the
 * coalescer (or, for a no-op pickup, out of both), never the other way.
 *
 * THE PER-CLASS ORDERED ENVELOPE that now sizes the ring (design 5.2 v4): per
 * player per second, 20 GE_TICK + 20 GE_RIDE{minecart} + human-rate discrete
 * facts (achievement pickups, achievement crafts, hostile kills, inventory
 * opens, pig falls -- bounded by hands on a controller, budgeted at 12/s) =
 * 52/s; two players across a 10 s drain stall = 1,040.
 * CW_ACH_OVERFLOW_CAPACITY is 2048, twice that.
 *
 * GE_TICK is ORDERED (R2 MED 4): it carries the mount kind and is the faithful
 * rail-origin reset (EntityPlayer.java:124-127 nulls
 * startMinecartRidingCoordinate on every non-riding tick, right after
 * minutesPlayedStat). It is emitted exactly once per player per tick, which is
 * what keeps it inside the envelope above.
 *
 * GE_MOVED is FOLDABLE again. v3 made it ORDERED because it carried the
 * ride-origin reset; with that reset moved to its faithful site on the tick, a
 * movement event is a pure distance counter with nothing order-sensitive left
 * in it, and a sum of centimetres folds exactly.
 */
enum cw_event_class {
	CW_EVENT_CLASS_ORDERED = 0,
	CW_EVENT_CLASS_FOLDABLE = 1,
};

#define CW_EVENT_CLASS_ROWS                                                    \
	CW_EVENT_CLASS(GE_BLOCK_HARVESTED, FOLDABLE)  /* mineBlock[] counter    */ \
	CW_EVENT_CLASS(GE_CRAFT_TAKEN, ORDERED)		  /* the 8 achievement ids  */ \
	CW_EVENT_CLASS(GE_FURNACE_TAKEN, ORDERED)	  /* 265 iron, 350 fish     */ \
	CW_EVENT_CLASS(GE_ITEM_PICKED_UP, ORDERED)	  /* 17 wood, 334 leather   */ \
	CW_EVENT_CLASS(GE_ITEM_USED, FOLDABLE)		  /* useItem[] counter      */ \
	CW_EVENT_CLASS(GE_ITEM_BROKEN, FOLDABLE)	  /* breakItem[] counter    */ \
	CW_EVENT_CLASS(GE_KILLED, ORDERED)			  /* the hostile set only   */ \
	CW_EVENT_CLASS(GE_KILLED_PLAYER, FOLDABLE)	  /* playerKills counter    */ \
	CW_EVENT_CLASS(GE_DAMAGE_DEALT, FOLDABLE)                                  \
	CW_EVENT_CLASS(GE_DAMAGE_TAKEN, FOLDABLE)                                  \
	CW_EVENT_CLASS(GE_DIED, FOLDABLE)                                          \
	CW_EVENT_CLASS(GE_JUMPED, FOLDABLE)                                        \
	CW_EVENT_CLASS(GE_DROPPED, FOLDABLE)                                       \
	CW_EVENT_CLASS(GE_INVENTORY_OPENED, ORDERED)  /* openInventory + hint   */ \
	CW_EVENT_CLASS(GE_MOVED, FOLDABLE)			  /* walk/swim/... counter  */ \
	CW_EVENT_CLASS(GE_FELL, FOLDABLE)			  /* fallOneCm counter      */ \
	CW_EVENT_CLASS(GE_RIDE, ORDERED)			  /* minecart only          */ \
	CW_EVENT_CLASS(GE_RIDE_END, ORDERED)		  /* resets the ride origin */ \
	CW_EVENT_CLASS(GE_FISH_CAUGHT, FOLDABLE)                                   \
	CW_EVENT_CLASS(GE_PIG_FELL_WITH_RIDER, ORDERED) /* flyPig, per fall     */ \
	CW_EVENT_CLASS(GE_GAME_STARTED, FOLDABLE)                                  \
	CW_EVENT_CLASS(GE_GAME_LEFT, FOLDABLE)                                     \
	CW_EVENT_CLASS(GE_MP_JOINED, FOLDABLE)                                     \
	CW_EVENT_CLASS(GE_TICK, ORDERED)	  /* the rail reset, EntityPlayer   */ \
										  /* .java:124-127                  */ \
	CW_EVENT_CLASS(GE_REMOTE_STAT, ORDERED)		  /* achievement ids only   */

/* ---- Derived counts ------------------------------------------------------
 * Derived from the rows, never written twice: adding a row updates every
 * array bound and every test loop at once. */
enum {
	CW_STAT_COUNT = 0
#define CW_STAT(id, key, type, owner) +1
	CW_STAT_ROWS
#undef CW_STAT
	,
	CW_ACH_COUNT = 0
#define CW_ACH(i, key, parent, icon, special, independent, column, row) +1
	CW_ACH_ROWS
#undef CW_ACH
	,
	CW_BLOCK_ALIAS_COUNT = 0
#define CW_BLOCK_ALIAS(from, to) +1
	CW_BLOCK_ALIAS_ROWS
#undef CW_BLOCK_ALIAS
	,
	CW_BLOCK_CRAFT_INVERT_COUNT = 0
#define CW_BLOCK_CRAFT_INVERT(from, to) +1
	CW_BLOCK_CRAFT_INVERT_ROWS
#undef CW_BLOCK_CRAFT_INVERT
	,
	CW_BLOCK_CRAFT_OUTPUT_COUNT = 0
#define CW_BLOCK_CRAFT_OUTPUT(id) +1
	CW_BLOCK_CRAFT_OUTPUT_ROWS
#undef CW_BLOCK_CRAFT_OUTPUT
	,
	CW_BLOCK_NO_STAT_COUNT = 0
#define CW_BLOCK_NO_STAT(id) +1
	CW_BLOCK_NO_STAT_ROWS
#undef CW_BLOCK_NO_STAT
	,
	CW_ITEM_CRAFT_OUTPUT_COUNT = 0
#define CW_ITEM_CRAFT_OUTPUT(id) +1
	CW_ITEM_CRAFT_OUTPUT_ROWS
#undef CW_ITEM_CRAFT_OUTPUT
	,
	CW_ITEM_DAMAGABLE_COUNT = 0
#define CW_ITEM_DAMAGABLE(id) +1
	CW_ITEM_DAMAGABLE_ROWS
#undef CW_ITEM_DAMAGABLE
	,
	/* One row per game_event kind; achievements.c asserts this equals
	 * GE_KIND_COUNT, so a new kind cannot ship without a class. */
	CW_EVENT_CLASS_COUNT = 0
#define CW_EVENT_CLASS(kind, cls) +1
	CW_EVENT_CLASS_ROWS
#undef CW_EVENT_CLASS
};

/* The mine array is one entry per Beta block id (source/block/blocks.h:
 * `struct block* blocks[256]`). */
#define CW_STAT_BLOCK_COUNT 256

#endif /* CW_STATS_TABLE_H */
