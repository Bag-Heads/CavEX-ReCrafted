/* Copyright (c) 2026 CaveWii_v2 contributors */
/* docs/achievements_design.md section 9, row `test_achievements_seam`, as
 * amended by the v3 text of section 5.2 (Astra call 2, brief F2):
 *
 *   ... compares the module against a single-threaded reference model on the
 *   named scenarios (threshold crossing then return, mixed qualifying rail
 *   ticks, dismount/remount, child-before-parent vs parent-before-child) and
 *   on seeded random scripts, each with the main ring forced full, comparing
 *   achievement counts, notice outcomes and general counters -- not one
 *   script.
 *
 * WHAT IS BEING PROVED. Past a full main ring the module splits an event two
 * ways: ORDERED kinds (anything that can change an achievement or a notice)
 * queue in the ordered overflow ring and are applied in emit order; FOLDABLE
 * kinds (pure counters) fold into the per-slot coalescer and are applied last.
 * The claim is that this split is INVISIBLE: for the same script, the module
 * produces exactly what a straight single-threaded walk of design 1.1/1.2/1.4
 * produces -- the same sixteen achievement counts, the same toast sequence,
 * the same value in every general and per-id counter -- whether the ring was
 * full or empty when the script ran.
 *
 * THE REFERENCE MODEL (struct ref_model) is written from the design tables and
 * never calls the module: no ring, no overflow ring, no coalescer, no locking,
 * no fold struct. It walks the script in order and applies the acceptance
 * matrix of 5.2, the unlock semantics of 1.2 and the counter rules of 1.4 by
 * hand.
 *
 * AND IT NO LONGER BORROWS THE MODULE'S REGISTRATION/ALIAS CODE (R2 HIGH 2).
 * It used to call stat_canonical_block() / stat_alias_block() -- the
 * replaceAllSimilarBlocks replay AND the per-array registration gates -- so a
 * wrong gate or a wrong fold direction was computed identically on both sides
 * and the comparison could not see it. The reference now reads the two
 * hand-derived goldens at start-up,
 *
 *     tests/golden/achievements/b3_alias_table.txt      (256 x 4 block cells)
 *     tests/golden/achievements/b4_item_registration.txt (the item id lists)
 *
 * and answers "which counter does this id land on in this array" from THOSE
 * tables alone. The goldens are derived from the decompile by hand and pinned
 * against the module separately (test_stats_counters), so the two sides now
 * agree only if both agree with Beta.
 *
 * The pure id-TABLE lookups of the B1 state layer are still shared
 * (stat_general_index, ach_parent, ach_is_independent): those are the X-macro
 * rows of stats_table.h read back, not a rule the module computes, and a
 * second hand copy of the 23 general ids would prove nothing.
 *
 * THE HARNESS runs every script TWICE -- once with the main ring deliberately
 * filled to capacity with foldable filler under a stalled drain, once with it
 * empty -- and both runs must equal the same reference. The script is emitted
 * from PRODUCER THREADS, one per owner slot: the two slots hold independent
 * state, so their interleaving cannot change either outcome, while within a
 * slot ring order is program order (design 5.2 "Causal order"). That is what
 * makes a single-threaded reference the right oracle for a threaded producer.
 *
 * The random scripts also pick each slot's BIND MODE per script (R2 LOW 14),
 * so the 5.2 matrix is exercised in all four slot/mode assignments rather than
 * always LOCAL-on-0 / REMOTE-on-1. The named scenarios keep a fixed pair on
 * purpose: their events are authored with the authority that mode accepts, and
 * randomising them would turn every scenario into a rejection sweep.
 *
 * NOTICES. GuiAchievement has one toast slot and a later unlock replaces the
 * one on screen [design 1.3], so after a drain only the LAST toast is
 * readable. The sequence is captured instead through
 * achievements_debug_notice_hook(), which fires once per toast queued, in
 * queue order, and is compared per slot against the reference's.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/test_fixture_dir.h"

#include "entity/monsters_object.h"
#include "game/achievements/achievements.h"
#include "platform/thread.h"

static char s_dir[128];

#define GEN_P0 11u
#define GEN_P1 22u

/* Two owner slots, one per half of the 5.2 matrix, exactly as B2's envelope
 * test has always run them. */
#define SLOT_LOCAL 0
#define SLOT_REMOTE 1

static void make_dir(const char* dir) {
#ifdef _WIN32
	(void)mkdir(dir);
#else
	(void)mkdir(dir, 0777);
#endif
}

static uint64_t generation_for(uint8_t slot) {
	return (slot == 0) ? GEN_P0 : GEN_P1;
}

static const struct stats_profile* prof(uint8_t slot) {
	return achievements_debug_profile(slot);
}

static struct ach_status status_of(uint8_t slot) {
	struct ach_status st;
	memset(&st, 0, sizeof(st));
	assert(achievements_debug_status(slot, &st));
	return st;
}

static uint32_t sat32(uint64_t v) {
	return (v > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)v;
}

/* ====================================================================== */
/* 1. THE SINGLE-THREADED REFERENCE MODEL                                 */
/* ====================================================================== */

/* ---- THE GOLDENS THE REFERENCE READS (R2 HIGH 2) -----------------------
 *
 * Loaded once, before any comparison: the 256 x 4 block cell table and the two
 * item id lists. From here on the reference answers "which counter does a
 * count for `id` land on in this array" from these arrays and NOTHING else --
 * no stat_canonical_block(), no stat_alias_block(), no stat_alias_block_in().
 */
static uint16_t g_gold_block[CW_STAT_ARRAY_COUNT][CW_STAT_BLOCK_COUNT];
static bool g_gold_item_craft[ITEMS_MAX];
static bool g_gold_item_break[ITEMS_MAX];

/* ctest runs this from the build directory and the brief forbids a CMake
 * change, so the golden is found by walking up from wherever we started. */
static FILE* open_repo_file(const char* rel) {
	static const char* const prefix[]
		= {"", "../", "../../", "../../../", "../../../../"};
	char path[256];
	for(size_t i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++) {
		FILE* f;
		snprintf(path, sizeof(path), "%s%s", prefix[i], rel);
		f = fopen(path, "rb");
		if(f)
			return f;
	}
	return NULL;
}

static uint16_t gold_cell(const char* tok) {
	if(tok[0] == '-' && tok[1] == '\0')
		return CW_STAT_BLOCK_NONE;
	return (uint16_t)atoi(tok);
}

static void ref_load_goldens(void) {
	char line[256];
	int rows = 0;
	int craft_rows = 0, break_rows = 0;
	FILE* f = open_repo_file("tests/golden/achievements/b3_alias_table.txt");
	assert(f);
	for(int a = 0; a < CW_STAT_ARRAY_COUNT; a++)
		for(int b = 0; b < CW_STAT_BLOCK_COUNT; b++)
			g_gold_block[a][b] = CW_STAT_BLOCK_NONE;
	while(fgets(line, sizeof(line), f)) {
		char c_id[16], c_m[16], c_c[16], c_u[16], c_b[16];
		int id;
		if(line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;
		assert(sscanf(line, "%15s %15s %15s %15s %15s", c_id, c_m, c_c, c_u,
					  c_b)
			   == 5);
		id = atoi(c_id);
		assert(id >= 0 && id < CW_STAT_BLOCK_COUNT);
		g_gold_block[CW_STAT_ARRAY_MINE][id] = gold_cell(c_m);
		g_gold_block[CW_STAT_ARRAY_CRAFT][id] = gold_cell(c_c);
		g_gold_block[CW_STAT_ARRAY_USE][id] = gold_cell(c_u);
		g_gold_block[CW_STAT_ARRAY_BREAK][id] = gold_cell(c_b);
		rows++;
	}
	fclose(f);
	assert(rows == CW_STAT_BLOCK_COUNT);

	f = open_repo_file("tests/golden/achievements/b4_item_registration.txt");
	assert(f);
	memset(g_gold_item_craft, 0, sizeof(g_gold_item_craft));
	memset(g_gold_item_break, 0, sizeof(g_gold_item_break));
	while(fgets(line, sizeof(line), f)) {
		char tag[16];
		int id;
		if(line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;
		assert(sscanf(line, "%15s %d", tag, &id) == 2);
		assert(id >= CW_STAT_BLOCK_COUNT && id < ITEMS_MAX);
		if(strcmp(tag, "craft") == 0) {
			g_gold_item_craft[id] = true;
			craft_rows++;
		} else {
			assert(strcmp(tag, "break") == 0);
			g_gold_item_break[id] = true;
			break_rows++;
		}
	}
	fclose(f);
	assert(craft_rows == 82 && break_rows == 48);
	printf("reference model loaded %d block rows, %d craft + %d break item "
		   "rows from the goldens (no module gate/alias code)\n",
		   rows, craft_rows, break_rows);
}

/* The counter id a count for `id` lands on in `array`, or CW_STAT_BLOCK_NONE
 * when Beta registers none -- straight out of the goldens. Below 256 the block
 * table decides (registration AND the replaceAllSimilarBlocks fold are already
 * baked into its cells); at or above it the two item lists do, and no item
 * ever folds because no alias row names one. */
static uint16_t ref_counter_in(enum cw_stat_array array, uint32_t id) {
	if(id < (uint32_t)CW_STAT_BLOCK_COUNT)
		return g_gold_block[array][id];
	if(id >= (uint32_t)ITEMS_MAX)
		return CW_STAT_BLOCK_NONE;
	switch(array) {
		case CW_STAT_ARRAY_MINE: return CW_STAT_BLOCK_NONE;
		case CW_STAT_ARRAY_CRAFT:
			return g_gold_item_craft[id] ? (uint16_t)id : CW_STAT_BLOCK_NONE;
		case CW_STAT_ARRAY_USE: return (uint16_t)id;
		case CW_STAT_ARRAY_BREAK:
			return g_gold_item_break[id] ? (uint16_t)id : CW_STAT_BLOCK_NONE;
		default: return CW_STAT_BLOCK_NONE;
	}
}

/* Beta's field_27088_g split for the general rows, read straight out of the
 * one id table [design 1.4 "Beta owner"]. */
static const uint8_t REF_STAT_OWNER[CW_STAT_COUNT] = {
#define CW_STAT(id, key, type, owner) (uint8_t)CW_STAT_OWNER_##owner,
	CW_STAT_ROWS
#undef CW_STAT
};

/* design 1.4 rows 2000/2001/2005/2003/2004 and 2006/2007/2008. */
static const uint32_t REF_MOVE_STAT[GE_MOVE_KIND_COUNT]
	= {2000u, 2001u, 2005u, 2003u, 2004u};
static const uint32_t REF_MOUNT_STAT[GE_MOUNT_KIND_COUNT]
	= {2006u, 2007u, 2008u};

/* One owner's whole expected state. Counters are uint64 so the model never
 * saturates earlier than the module does; the column is fixed per slot by the
 * bind mode [design 6], so one number per counter plus that column is a
 * complete description of the pair the module must hold. */
struct ref_model {
	uint64_t general[CW_STAT_COUNT];
	uint64_t mine[CW_STAT_BLOCK_COUNT];
	uint64_t craft[ITEMS_MAX];
	uint64_t use[ITEMS_MAX];
	uint64_t brk[ITEMS_MAX];
	uint64_t ach[CW_ACH_COUNT];
	uint8_t mode;	/* enum ach_bind_mode */
	uint8_t column; /* enum stats_column implied by `mode` */
	/* EntityPlayer.startMinecartRidingCoordinate [design 1.1 row 11]. */
	bool ride_valid;
	int32_t rx, ry, rz;
	bool hint_active;
	int16_t notice[CW_ACH_COUNT]; /* toasts, in queue order */
	int notice_n;
	uint32_t notices_queued;
	int16_t notice_current;
};

static struct ref_model g_ref[CW_ACH_SLOT_COUNT];

static void ref_reset(struct ref_model* m, enum ach_bind_mode mode) {
	memset(m, 0, sizeof(*m));
	m->mode = (uint8_t)mode;
	m->column = (uint8_t)((mode == ACH_MODE_REMOTE_SERVER)
							  ? STATS_COLUMN_REMOTE
							  : STATS_COLUMN_LOCAL);
	/* Beta shows the openInventory hint for as long as the achievement is
	 * locked; a fresh profile has it locked [design 1.1 row 0]. */
	m->hint_active = true;
	m->notice_current = -1;
}

static void ref_general(struct ref_model* m, uint32_t stat_id, uint64_t v) {
	int i = stat_general_index(stat_id);
	assert(i >= 0);
	m->general[i] += v;
}

/* design 1.2 point 2: unlocked == the totals map holds a count > 0. */
static bool ref_unlocked(const struct ref_model* m, int i) {
	return i >= 0 && i < CW_ACH_COUNT && m->ach[i] > 0u;
}

/* design 1.2 point 1: no parent, or the parent is already unlocked. */
static bool ref_can_unlock(const struct ref_model* m, int i) {
	if(i < 0 || i >= CW_ACH_COUNT)
		return false;
	int parent = ach_parent(i);
	return parent < 0 || ref_unlocked(m, parent);
}

/* EntityPlayerSP.addStat for an achievement [design 1.2]: a locked parent
 * DROPS the event entirely; otherwise the toast is queued on the 0 -> 1
 * transition only, and the count increments on every repeat. */
static void ref_award(struct ref_model* m, int i, uint64_t increment) {
	if(i < 0 || i >= CW_ACH_COUNT)
		return;
	if(!ref_can_unlock(m, i))
		return;
	if(increment == 0u)
		return;
	if(!ref_unlocked(m, i)) {
		assert(m->notice_n < CW_ACH_COUNT);
		m->notice[m->notice_n++] = (int16_t)i;
		m->notices_queued++;
		m->notice_current = (int16_t)i;
	}
	m->ach[i] += increment;
	if(i == 0)
		m->hint_active = !ref_unlocked(m, 0);
}

/* design 1.4 "Beta owner", read through the 5.3 site map: which side of the
 * engine is allowed to observe this kind. */
static int ref_kind_owner(uint8_t kind) {
	switch(kind) {
		case GE_JUMPED:
		case GE_DROPPED:
		case GE_INVENTORY_OPENED:
		case GE_MOVED:
		case GE_FELL:
		case GE_RIDE:
		case GE_RIDE_END:
		case GE_GAME_STARTED:
		case GE_GAME_LEFT:
		case GE_MP_JOINED:
		case GE_TICK: return CW_STAT_OWNER_CLIENT;
		default: return CW_STAT_OWNER_SERVER;
	}
}

static int ref_ach_of_stat_id(uint32_t id) {
	if(id < CW_ACH_BASE || id >= CW_ACH_BASE + (uint32_t)CW_ACH_COUNT)
		return -1;
	return (int)(id - CW_ACH_BASE);
}

/* The class of a whole Beta stat id, for Packet 200. False means this build
 * has no counter for it at all. */
static bool ref_stat_owner_of_id(uint32_t id, int* owner) {
	int a = ref_ach_of_stat_id(id);
	if(a >= 0) {
		*owner = ach_is_independent(a) ? CW_STAT_OWNER_CLIENT
									   : CW_STAT_OWNER_SERVER;
		return true;
	}
	int g = stat_general_index(id);
	if(g >= 0) {
		*owner = REF_STAT_OWNER[g];
		return true;
	}
	/* A per-id array counter exists only where Beta's registration loop for
	 * THAT array made a StatBase -- blocks and items alike (F3 + R2 MED 8).
	 * `false` is "this build has no counter for that id", which the module
	 * counts as ACH_REJECT_UNKNOWN_STAT. */
	if(id >= CW_STAT_MINE_BASE
	   && id < CW_STAT_MINE_BASE + (uint32_t)CW_STAT_BLOCK_COUNT) {
		if(ref_counter_in(CW_STAT_ARRAY_MINE, id - CW_STAT_MINE_BASE)
		   == CW_STAT_BLOCK_NONE)
			return false;
		*owner = CW_STAT_OWNER_SERVER;
		return true;
	}
	{
		enum cw_stat_array which;
		uint32_t base;
		if(id >= CW_STAT_CRAFT_BASE && id < CW_STAT_CRAFT_BASE + ITEMS_MAX) {
			which = CW_STAT_ARRAY_CRAFT;
			base = CW_STAT_CRAFT_BASE;
		} else if(id >= CW_STAT_USE_BASE && id < CW_STAT_USE_BASE + ITEMS_MAX) {
			which = CW_STAT_ARRAY_USE;
			base = CW_STAT_USE_BASE;
		} else if(id >= CW_STAT_BREAK_BASE
				  && id < CW_STAT_BREAK_BASE + ITEMS_MAX) {
			which = CW_STAT_ARRAY_BREAK;
			base = CW_STAT_BREAK_BASE;
		} else {
			return false;
		}
		if(ref_counter_in(which, id - base) == CW_STAT_BLOCK_NONE)
			return false;
		*owner = CW_STAT_OWNER_SERVER;
		return true;
	}
}

/* The acceptance matrix of design 5.2, one branch per row of the table. */
static bool ref_matrix(uint8_t mode, uint8_t authority, int owner) {
	if(mode == (uint8_t)ACH_MODE_LOCAL_WORLD) {
		if(authority == GE_AUTH_PACKET200)
			return false;
		return (authority == GE_AUTH_SERVER && owner == CW_STAT_OWNER_SERVER)
			|| (authority == GE_AUTH_CLIENT && owner == CW_STAT_OWNER_CLIENT);
	}
	if(authority == GE_AUTH_SERVER)
		return false;
	return (authority == GE_AUTH_CLIENT && owner == CW_STAT_OWNER_CLIENT)
		|| (authority == GE_AUTH_PACKET200 && owner == CW_STAT_OWNER_SERVER);
}

/* SlotCrafting.onPickupFromSlot's else-if chain [design 1.1]. */
static int ref_craft_ach(uint32_t item_id) {
	switch(item_id) {
		case 58: return 2;	/* buildWorkBench */
		case 270: return 3;	/* buildPickaxe */
		case 61: return 4;	/* buildFurnace */
		case 290: return 6;	/* buildHoe */
		case 297: return 7;	/* makeBread */
		case 354: return 8;	/* bakeCake */
		case 274: return 9;	/* buildBetterPickaxe */
		case 268: return 12; /* buildSword */
		default: return -1;
	}
}

/* EntityPlayer.onKillEntity: EntityMob victims only [design 1.1 row 13]. */
static bool ref_hostile(uint8_t sub) {
	switch(sub) {
		case MONSTER_ZOMBIE:
		case MONSTER_SKELETON:
		case MONSTER_CREEPER:
		case MONSTER_SPIDER:
		case MONSTER_PIG_ZOMBIE:
		case MONSTER_GIANT: return true;
		default: return false;
	}
}

/* A Packet 200 statistic for a non-achievement id [design 1.2 point 4]. */
static void ref_remote_counter(struct ref_model* m, uint32_t id,
							   uint64_t amount) {
	int g = stat_general_index(id);
	uint16_t cell;
	if(g >= 0) {
		m->general[g] += amount;
		return;
	}
	if(id >= CW_STAT_MINE_BASE
	   && id < CW_STAT_MINE_BASE + (uint32_t)CW_STAT_BLOCK_COUNT) {
		cell = ref_counter_in(CW_STAT_ARRAY_MINE, id - CW_STAT_MINE_BASE);
		if(cell != CW_STAT_BLOCK_NONE)
			m->mine[cell] += amount;
		return;
	}
	/* Each array folds in ITS OWN direction and behind ITS OWN gate: a remote
	 * craftItem is not a useItem (the pre-F4 reference sent all three through
	 * the use table, R2 HIGH 3). */
	if(id >= CW_STAT_CRAFT_BASE && id < CW_STAT_CRAFT_BASE + ITEMS_MAX) {
		cell = ref_counter_in(CW_STAT_ARRAY_CRAFT, id - CW_STAT_CRAFT_BASE);
		if(cell != CW_STAT_BLOCK_NONE)
			m->craft[cell] += amount;
		return;
	}
	if(id >= CW_STAT_USE_BASE && id < CW_STAT_USE_BASE + ITEMS_MAX) {
		cell = ref_counter_in(CW_STAT_ARRAY_USE, id - CW_STAT_USE_BASE);
		if(cell != CW_STAT_BLOCK_NONE)
			m->use[cell] += amount;
		return;
	}
	if(id >= CW_STAT_BREAK_BASE && id < CW_STAT_BREAK_BASE + ITEMS_MAX) {
		cell = ref_counter_in(CW_STAT_ARRAY_BREAK, id - CW_STAT_BREAK_BASE);
		if(cell != CW_STAT_BLOCK_NONE)
			m->brk[cell] += amount;
	}
}

/* EntityPlayer.addMountedMovementStat's rail rule [design 1.1 row 11]: the
 * origin is set on the first mounted move after a reset and NOT tested on that
 * same event; every later one compares the squared integer block distance
 * against 1000. */
static void ref_rail(struct ref_model* m, int32_t x, int32_t y, int32_t z) {
	if(!m->ride_valid) {
		m->ride_valid = true;
		m->rx = x;
		m->ry = y;
		m->rz = z;
		return;
	}
	int64_t dx = (int64_t)m->rx - (int64_t)x;
	int64_t dy = (int64_t)m->ry - (int64_t)y;
	int64_t dz = (int64_t)m->rz - (int64_t)z;
	if(dx * dx + dy * dy + dz * dz >= 1000)
		ref_award(m, 11, 1u);
}

/* The whole consumer, written straight from the design tables. */
static void ref_apply(struct ref_model* m, const struct game_event* ev) {
	int owner;
	if(ev->kind == GE_REMOTE_STAT) {
		if(!ref_stat_owner_of_id((uint32_t)ev->u.remote.stat_id, &owner))
			return; /* this build has no counter for that id */
	} else {
		owner = ref_kind_owner(ev->kind);
	}
	if(!ref_matrix(m->mode, ev->authority, owner))
		return;

	switch(ev->kind) {
		case GE_BLOCK_HARVESTED: {
			uint16_t b = ref_counter_in(CW_STAT_ARRAY_MINE, ev->u.item.id);
			if(b != CW_STAT_BLOCK_NONE)
				m->mine[b] += 1u;
			break;
		}
		case GE_CRAFT_TAKEN: {
			/* ItemStack.onCrafting counts craftItem += stackSize whether or
			 * not the achievement fires -- but only where Beta registered a
			 * craftItem counter, which is the recipe/smelting output set for
			 * blocks AND items (F3 + R2 MED 8). */
			uint16_t c = ref_counter_in(CW_STAT_ARRAY_CRAFT, ev->u.item.id);
			if(c != CW_STAT_BLOCK_NONE)
				m->craft[c] += ev->u.item.count;
			ref_award(m, ref_craft_ach(ev->u.item.id), 1u);
			break;
		}
		case GE_FURNACE_TAKEN: {
			/* SlotFurnace's take is a craftItem count too, under the same
			 * gate. */
			uint16_t c = ref_counter_in(CW_STAT_ARRAY_CRAFT, ev->u.item.id);
			if(c != CW_STAT_BLOCK_NONE)
				m->craft[c] += ev->u.item.count;
			/* SlotFurnace checks both outputs independently. */
			if(ev->u.item.id == 265)
				ref_award(m, 5, 1u); /* acquireIron */
			if(ev->u.item.id == 350)
				ref_award(m, 10, 1u); /* cookFish */
			break;
		}
		case GE_ITEM_PICKED_UP:
			/* Beta has no "items picked up" counter: the pickup is only ever
			 * the trigger of mineWood or killCow [design 1.1 rows 1, 14]. */
			if(ev->u.item.id == 17)
				ref_award(m, 1, 1u);
			else if(ev->u.item.id == 334)
				ref_award(m, 14, 1u);
			break;
		case GE_ITEM_USED: {
			uint16_t c = ref_counter_in(CW_STAT_ARRAY_USE, ev->u.item.id);
			if(c != CW_STAT_BLOCK_NONE)
				m->use[c] += 1u;
			break;
		}
		case GE_ITEM_BROKEN: {
			/* breakItem is `isDamagable()` only: no block id has one, and
			 * neither does an item that cannot be depleted (R2 MED 8). */
			uint16_t c = ref_counter_in(CW_STAT_ARRAY_BREAK, ev->u.item.id);
			if(c != CW_STAT_BLOCK_NONE)
				m->brk[c] += 1u;
			break;
		}
		case GE_KILLED:
			ref_general(m, 2023u, 1u); /* mobKills */
			if(ref_hostile(ev->sub))
				ref_award(m, 13, 1u); /* killEnemy */
			break;
		case GE_KILLED_PLAYER: ref_general(m, 2024u, 1u); break;
		case GE_DAMAGE_DEALT:
			ref_general(m, 2020u, (uint64_t)ev->u.n.amount);
			break;
		case GE_DAMAGE_TAKEN:
			ref_general(m, 2021u, (uint64_t)ev->u.n.amount);
			break;
		case GE_DIED: ref_general(m, 2022u, 1u); break;
		case GE_FISH_CAUGHT: ref_general(m, 2025u, 1u); break;
		case GE_PIG_FELL_WITH_RIDER:
			/* EntityPig.fall: `f > 5F` on the FLOAT, one addStat per
			 * qualifying fall [design 1.1 row 15, R2 MED 5]. */
			if(ev->u.fall.f > 5.0F)
				ref_award(m, 15, 1u);
			break;
		case GE_JUMPED: ref_general(m, 2010u, 1u); break;
		case GE_DROPPED: ref_general(m, 2011u, 1u); break;
		case GE_INVENTORY_OPENED: ref_award(m, 0, 1u); break;
		case GE_MOVED:
			/* A pure distance counter: the ride-origin reset lives on the
			 * TICK, where Beta does it (R2 MED 4). */
			if(ev->sub < GE_MOVE_KIND_COUNT)
				ref_general(m, REF_MOVE_STAT[ev->sub],
							(uint64_t)ev->u.move.cm);
			break;
		case GE_FELL: ref_general(m, 2002u, (uint64_t)ev->u.move.cm); break;
		case GE_RIDE:
			if(ev->sub < GE_MOUNT_KIND_COUNT)
				ref_general(m, REF_MOUNT_STAT[ev->sub],
							(uint64_t)ev->u.ride.cm);
			if(ev->sub == GE_MOUNT_MINECART)
				ref_rail(m, ev->u.ride.x, ev->u.ride.y, ev->u.ride.z);
			break;
		case GE_RIDE_END:
			m->ride_valid = false;
			m->rx = m->ry = m->rz = 0;
			break;
		case GE_GAME_STARTED:
			ref_general(m, 1000u, 1u);
			ref_general(m, (ev->sub == GE_WORLD_NEW) ? 1001u : 1002u, 1u);
			break;
		case GE_GAME_LEFT: ref_general(m, 1004u, 1u); break;
		case GE_MP_JOINED: ref_general(m, 1003u, 1u); break;
		case GE_TICK:
			/* EntityPlayer.onUpdate: minutesPlayedStat, then
			 * `if(ridingEntity == null) startMinecartRidingCoordinate = null;`
			 * (EntityPlayer.java:123-127). */
			ref_general(m, 1100u, 1u);
			if(ev->sub == (uint8_t)GE_TICK_NOT_RIDING) {
				m->ride_valid = false;
				m->rx = m->ry = m->rz = 0;
			}
			m->hint_active = !ref_unlocked(m, 0);
			break;
		case GE_REMOTE_STAT: {
			int a = ref_ach_of_stat_id((uint32_t)ev->u.remote.stat_id);
			if(a >= 0)
				ref_award(m, a, (uint64_t)ev->u.remote.amount);
			else
				ref_remote_counter(m, (uint32_t)ev->u.remote.stat_id,
								   (uint64_t)ev->u.remote.amount);
			break;
		}
		default: break;
	}
}

/* ====================================================================== */
/* 2. NOTICE CAPTURE                                                      */
/* ====================================================================== */

#define CAP_MAX 256
static struct {
	uint8_t slot;
	int16_t ach;
} g_cap[CAP_MAX];
static int g_cap_n;

static void cap_hook(uint8_t slot, int16_t ach_id) {
	assert(g_cap_n < CAP_MAX);
	g_cap[g_cap_n].slot = slot;
	g_cap[g_cap_n].ach = ach_id;
	g_cap_n++;
}

/* ====================================================================== */
/* 3. SCRIPTS                                                             */
/* ====================================================================== */

#define SCRIPT_MAX 2048

struct script {
	struct game_event ev[SCRIPT_MAX];
	int n;
};

static struct script g_script;
static struct script g_sub[CW_ACH_SLOT_COUNT];
static int g_slot_arg[CW_ACH_SLOT_COUNT] = {0, 1};

static struct game_event ev_kind(uint8_t kind, uint8_t authority, uint8_t sub) {
	struct game_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.kind = kind;
	ev.authority = authority;
	ev.sub = sub;
	return ev;
}

static struct game_event ev_item(uint8_t kind, uint8_t authority, uint16_t id,
								 uint16_t count) {
	struct game_event ev = ev_kind(kind, authority, 0);
	ev.u.item.id = id;
	ev.u.item.count = count;
	return ev;
}

static struct game_event ev_move(uint8_t kind, uint8_t authority, uint8_t sub,
								 int32_t cm) {
	struct game_event ev = ev_kind(kind, authority, sub);
	ev.u.move.cm = cm;
	return ev;
}

static struct game_event ev_ride(uint8_t sub, int32_t x, int32_t y, int32_t z,
								 int32_t cm) {
	struct game_event ev = ev_kind(GE_RIDE, GE_AUTH_CLIENT, sub);
	ev.u.ride.x = x;
	ev.u.ride.y = y;
	ev.u.ride.z = z;
	ev.u.ride.cm = cm;
	return ev;
}

static struct game_event ev_n(uint8_t kind, uint8_t authority, int32_t amount) {
	struct game_event ev = ev_kind(kind, authority, 0);
	ev.u.n.amount = amount;
	return ev;
}

/* GE_PIG_FELL_WITH_RIDER carries the float fall distance (R2 MED 5). */
static struct game_event ev_pig(float f) {
	struct game_event ev = ev_kind(GE_PIG_FELL_WITH_RIDER, GE_AUTH_SERVER, 0);
	ev.u.fall.f = f;
	return ev;
}

/* GE_TICK carries the mount kind; 0 is "not riding", which is Beta's
 * rail-origin reset (EntityPlayer.java:124-127, R2 MED 4). */
static struct game_event ev_tick(uint8_t mount_sub) {
	return ev_kind(GE_TICK, GE_AUTH_CLIENT, mount_sub);
}

static struct game_event ev_remote(int32_t stat_id, int32_t amount) {
	struct game_event ev = ev_kind(GE_REMOTE_STAT, GE_AUTH_PACKET200, 0);
	ev.u.remote.stat_id = stat_id;
	ev.u.remote.amount = amount;
	return ev;
}

static void sc_add(struct script* sc, uint8_t slot, struct game_event ev) {
	assert(sc->n < SCRIPT_MAX);
	ev.who.slot = slot;
	ev.who.generation = generation_for(slot);
	sc->ev[sc->n++] = ev;
}

/* openInventory -> mineWood -> buildWorkBench -> buildPickaxe -> buildFurnace
 * -> acquireIron, i.e. everything onARail (11) descends from [design 1.1]. */
static void sc_rail_spine(struct script* sc, uint8_t slot) {
	sc_add(sc, slot, ev_kind(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0));
	sc_add(sc, slot, ev_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 17, 1));
	sc_add(sc, slot, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 58, 1));
	sc_add(sc, slot, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 270, 1));
	sc_add(sc, slot, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 61, 1));
	sc_add(sc, slot, ev_item(GE_FURNACE_TAKEN, GE_AUTH_SERVER, 265, 1));
}

/* The design 5.2 v4 emit-time class, re-derived here for ONE purpose: to
 * budget how many ORDERED events a random script may contain, because the
 * ordered ring is finite and case (vii) owns what happens past it. It is NOT
 * part of the oracle -- the reference model never needs to know a class, which
 * is exactly why the split has to be invisible. Getting it wrong can only cost
 * a false ACH_REJECT_OVERFLOW, which compare_slot() fails on. */
static int harness_class(const struct game_event* ev) {
	switch(ev->kind) {
		case GE_ITEM_PICKED_UP:
			return (ev->u.item.id == 17u || ev->u.item.id == 334u) ? 1 : -1;
		case GE_CRAFT_TAKEN: return ref_craft_ach(ev->u.item.id) >= 0 ? 1 : 0;
		case GE_FURNACE_TAKEN:
			return (ev->u.item.id == 265u || ev->u.item.id == 350u) ? 1 : 0;
		case GE_KILLED: return ref_hostile(ev->sub) ? 1 : 0;
		case GE_RIDE: return ev->sub == GE_MOUNT_MINECART ? 1 : 0;
		case GE_REMOTE_STAT:
			return ref_ach_of_stat_id((uint32_t)ev->u.remote.stat_id) >= 0 ? 1
																		   : 0;
		case GE_INVENTORY_OPENED:
		case GE_RIDE_END:
		case GE_PIG_FELL_WITH_RIDER:
		case GE_TICK: return 1;
		default: return 0;
	}
}

/* ====================================================================== */
/* 4. THE HARNESS                                                         */
/* ====================================================================== */

static unsigned g_run;
static unsigned long g_counters_compared;
static unsigned long g_notices_compared;

/* One producer thread per owner slot [design 5.2 "Causal order"]. */
static void* emit_sub_script(void* arg) {
	int slot = *(const int*)arg;
	const struct script* sc = &g_sub[slot];
	for(int i = 0; i < sc->n; i++)
		game_event_emit(&sc->ev[i]);
	return NULL;
}

struct run_ctx {
	const char* label;
	bool full;
	uint32_t seed;
};

static void fail_ctx(const struct run_ctx* c, uint8_t slot, const char* what) {
	printf("FAIL %s [ring %s, seed %u, slot %u]: %s\n", c->label,
		   c->full ? "FORCED FULL" : "empty", (unsigned)c->seed,
		   (unsigned)slot, what);
	fflush(stdout);
}

#define REF_CHECK(ctx, slot, cond, what)                                       \
	do {                                                                       \
		if(!(cond)) {                                                          \
			fail_ctx((ctx), (slot), (what));                                   \
			assert(cond);                                                      \
		}                                                                      \
	} while(0)

/* Every counter the profile has, the sixteen achievement counts, the hint and
 * the whole toast sequence -- nothing skipped, so a value that moved when it
 * should not have fails as loudly as one that did not move. */
static void compare_slot(const struct run_ctx* ctx, uint8_t slot) {
	const struct stats_profile* p = prof(slot);
	const struct ref_model* m = &g_ref[slot];
	bool local = (m->column == (uint8_t)STATS_COLUMN_LOCAL);

#define PAIR_EQ(pairptr, want)                                                 \
	do {                                                                       \
		const struct stats_pair* _p = (pairptr);                               \
		uint32_t _w = sat32(want);                                             \
		REF_CHECK(ctx, slot,                                                   \
				  (local ? (_p->local == _w && _p->remote == 0u)               \
						 : (_p->remote == _w && _p->local == 0u)),             \
				  #pairptr);                                                   \
		g_counters_compared++;                                                 \
	} while(0)

	for(int i = 0; i < CW_STAT_COUNT; i++)
		PAIR_EQ(&p->general[i], m->general[i]);
	for(int i = 0; i < CW_STAT_BLOCK_COUNT; i++)
		PAIR_EQ(&p->mine[i], m->mine[i]);
	for(int i = 0; i < ITEMS_MAX; i++) {
		PAIR_EQ(&p->craft[i], m->craft[i]);
		PAIR_EQ(&p->use[i], m->use[i]);
		PAIR_EQ(&p->brk[i], m->brk[i]);
	}
	for(int i = 0; i < CW_ACH_COUNT; i++) {
		PAIR_EQ(&p->ach[i], m->ach[i]);
		REF_CHECK(ctx, slot, ach_unlocked(p, i) == ref_unlocked(m, i),
				  "achievement unlocked state");
	}
#undef PAIR_EQ

	/* The toast sequence, in queue order, for THIS slot only. */
	int k = 0;
	for(int i = 0; i < g_cap_n; i++) {
		if(g_cap[i].slot != slot)
			continue;
		REF_CHECK(ctx, slot, k < m->notice_n, "more toasts than the reference");
		REF_CHECK(ctx, slot, g_cap[i].ach == m->notice[k],
				  "toast sequence diverged");
		k++;
		g_notices_compared++;
	}
	REF_CHECK(ctx, slot, k == m->notice_n, "fewer toasts than the reference");

	struct ach_notice n;
	memset(&n, 0, sizeof(n));
	(void)achievements_debug_notice(slot, &n);
	REF_CHECK(ctx, slot, n.ach_id == m->notice_current, "the toast on screen");
	REF_CHECK(ctx, slot, n.hint_active == m->hint_active,
			  "the openInventory hint");

	struct ach_status st = status_of(slot);
	REF_CHECK(ctx, slot, st.notices_queued == m->notices_queued,
			  "notices_queued");
	REF_CHECK(ctx, slot, !st.progress_incomplete, "progress_incomplete set");
	REF_CHECK(ctx, slot, st.rejected[ACH_REJECT_OVERFLOW] == 0u,
			  "the ordered overflow ring rejected an event");
	REF_CHECK(ctx, slot, st.overflow_used == 0u,
			  "the overflow ring was not fully drained");
	REF_CHECK(ctx, slot, st.overflow_peak <= CW_ACH_OVERFLOW_CAPACITY,
			  "overflow_peak past the ring");
	REF_CHECK(ctx, slot, st.coalesce_peak <= CW_ACH_COALESCE_CAPACITY,
			  "coalesce_peak past the table");
}

/* Run one script against the module and against the reference, and compare.
 *
 * `full` fills the main ring to capacity FIRST, with a foldable kind, under a
 * stalled drain, so every script event has to take the overflow-ring /
 * coalescer split. The filler is part of the script's expected state, so the
 * reference sees it too. */
static void run_script(const struct script* sc, bool full, const char* label,
					   uint32_t seed, enum ach_bind_mode mode0,
					   enum ach_bind_mode mode1) {
	struct run_ctx ctx = {label, full, seed};
	char id0[CW_PROFILE_ID_MAX + 1], id1[CW_PROFILE_ID_MAX + 1];
	g_run++;
	snprintf(id0, sizeof(id0), "sa%u", g_run);
	snprintf(id1, sizeof(id1), "sb%u", g_run);

	assert(achievements_init(s_dir));
	assert(achievements_bind(SLOT_LOCAL, GEN_P0, id0, mode0) == ACH_BIND_OK);
	assert(achievements_bind(SLOT_REMOTE, GEN_P1, id1, mode1) == ACH_BIND_OK);
	ref_reset(&g_ref[SLOT_LOCAL], mode0);
	ref_reset(&g_ref[SLOT_REMOTE], mode1);

	g_cap_n = 0;
	achievements_debug_notice_hook(cap_hook);

	if(full) {
		/* GE_JUMPED is FOLDABLE, unlocks nothing, and is CLIENT-observed and
		 * client-owned -- so it is accepted in BOTH bind modes and the filler
		 * cannot itself disturb an ordering or a mode. */
		struct game_event filler
			= ev_kind(GE_JUMPED, GE_AUTH_CLIENT, 0);
		filler.who.slot = SLOT_LOCAL;
		filler.who.generation = GEN_P0;
		for(int i = 0; i < CW_ACH_RING_CAPACITY; i++) {
			game_event_emit(&filler);
			ref_apply(&g_ref[SLOT_LOCAL], &filler);
		}
	}

	/* Split by owner: one producer thread per slot. */
	g_sub[0].n = g_sub[1].n = 0;
	for(int i = 0; i < sc->n; i++) {
		uint8_t slot = sc->ev[i].who.slot;
		assert(slot < CW_ACH_SLOT_COUNT);
		assert(g_sub[slot].n < SCRIPT_MAX);
		g_sub[slot].ev[g_sub[slot].n++] = sc->ev[i];
		ref_apply(&g_ref[slot], &sc->ev[i]);
	}

	struct thread th0, th1;
	assert(thread_create(&th0, emit_sub_script, &g_slot_arg[0], 64));
	assert(thread_create(&th1, emit_sub_script, &g_slot_arg[1], 64));
	thread_join(&th0);
	thread_join(&th1);

	if(full) {
		/* The split actually happened: nothing drained in between. */
		struct ach_status a = status_of(SLOT_LOCAL);
		struct ach_status b = status_of(SLOT_REMOTE);
		REF_CHECK(&ctx, 0, a.overflow_peak + b.overflow_peak > 0u
							   || a.ring_folds + b.ring_folds > 0u,
				  "the ring was not actually full");
	}

	achievements_tick();
	compare_slot(&ctx, SLOT_LOCAL);
	compare_slot(&ctx, SLOT_REMOTE);
	achievements_debug_notice_hook(NULL);
}

/* Both ring states must give the same answer as the one reference.
 *
 * "empty" means the ring starts empty, not that it never fills: a script
 * longer than CW_ACH_RING_CAPACITY splits on its own, at a different point
 * than the forced-full run does. Two different split points, one reference --
 * which is the property being proved. */
static void run_both_modes(const struct script* sc, const char* label,
						   uint32_t seed, enum ach_bind_mode mode0,
						   enum ach_bind_mode mode1) {
	run_script(sc, false, label, seed, mode0, mode1);
	run_script(sc, true, label, seed, mode0, mode1);
}

/* The named scenarios keep the fixed pair they were authored against: their
 * events carry the authority THAT mode accepts, so randomising the mode would
 * turn a rail scenario into a rejection sweep. The random scripts randomise it
 * per slot per script instead (R2 LOW 14). */
static void run_both(const struct script* sc, const char* label,
					 uint32_t seed) {
	run_both_modes(sc, label, seed, ACH_MODE_LOCAL_WORLD,
				   ACH_MODE_REMOTE_SERVER);
}

/* ====================================================================== */
/* 5. THE NAMED SCENARIOS                                                 */
/* ====================================================================== */

/* (i) threshold crossing, then a return inside it, then more beyond. */
static void scenario_rail_cross_and_return(void) {
	struct script* sc = &g_script;
	sc->n = 0;
	sc_rail_spine(sc, SLOT_LOCAL);
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 0, 0, 0, 5)); /* origin */
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 32, 0, 0, 5)); /* 1024 */
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 5, 0, 0, 5));	 /* 25   */
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 0, 0, 0, 5));	 /* 0    */
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 40, 0, 0, 5)); /* 1600 */
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 33, 0, 0, 5)); /* 1089 */
	run_both(sc, "(i) rail threshold crossing then return", 0u);
	assert(stats_counter_total(&prof(SLOT_LOCAL)->ach[11]) == 3u);
	printf("PASS (i) rail threshold crossing then return -- onARail = 3 of 6 "
		   "ride ticks, identical with the ring empty and forced full\n");
}

/* (ii) qualifying and non-qualifying rail ticks interleaved. */
static void scenario_rail_mixed(void) {
	struct script* sc = &g_script;
	sc->n = 0;
	sc_rail_spine(sc, SLOT_LOCAL);
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 0, 0, 0, 3)); /* origin */
	int expect = 0;
	for(int i = 0; i < 40; i++) {
		int32_t x = (i % 2) ? 4 : 31; /* 16 < 1000, 961 < 1000 ... */
		int32_t y = (i % 2) ? 0 : 20; /* 961 + 400 = 1361 >= 1000   */
		sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, x, y, 0, 3));
		if((int64_t)x * x + (int64_t)y * y >= 1000)
			expect++;
	}
	run_both(sc, "(ii) mixed qualifying/non-qualifying rail ticks", 0u);
	assert(stats_counter_total(&prof(SLOT_LOCAL)->ach[11])
		   == (uint32_t)expect);
	printf("PASS (ii) mixed qualifying/non-qualifying rail ticks -- onARail = "
		   "%d of 40 interleaved ticks, both ring states\n",
		   expect);
}

/* (iii) dismount, a new origin, and crossing again -- plus the NON-RIDING TICK
 * reset, which is where Beta actually clears startMinecartRidingCoordinate
 * (EntityPlayer.java:124-127) and is why GE_TICK is the ORDERED kind now and
 * GE_MOVED is not (R2 MED 4). */
static void scenario_dismount_remount(void) {
	struct script* sc = &g_script;
	sc->n = 0;
	sc_rail_spine(sc, SLOT_LOCAL);
	/* ride 1: cross. */
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 0, 0, 0, 4));
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 32, 0, 0, 4)); /* award */
	/* dismount, then ride 2 from a FAR AWAY origin: without the reset the
	 * first tick of ride 2 would be 100^2 from the old origin and award. */
	sc_add(sc, SLOT_LOCAL, ev_kind(GE_RIDE_END, GE_AUTH_CLIENT,
								   GE_MOUNT_MINECART));
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 100, 0, 0, 4));
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 105, 0, 0, 4)); /* 25 */
	/* Walk off. The MOVE is now just a distance counter; the reset comes from
	 * the non-riding tick that Beta emits whether or not the player moved --
	 * so this pair also proves the move alone no longer resets anything. */
	sc_add(sc, SLOT_LOCAL, ev_move(GE_MOVED, GE_AUTH_CLIENT, GE_MOVE_WALK, 7));
	sc_add(sc, SLOT_LOCAL, ev_tick(GE_TICK_NOT_RIDING));
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 200, 0, 0, 4));
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 205, 0, 0, 4)); /* 25 */
	sc_add(sc, SLOT_LOCAL, ev_ride(GE_MOUNT_MINECART, 240, 0, 0, 4)); /* aw */
	run_both(sc, "(iii) dismount / remount / walk off", 0u);
	assert(stats_counter_total(&prof(SLOT_LOCAL)->ach[11]) == 2u);
	printf("PASS (iii) dismount/remount -- onARail = 2 of 7 ride ticks across "
		   "3 origins (GE_RIDE_END and a non-riding GE_TICK reset in order)"
		   "\n");
}

/* (iv) the prerequisite order, both ways round, for three parent edges. */
static void scenario_parent_order(void) {
	struct script* sc = &g_script;
	int r = 0;

	/* a) mineWood(1) -> buildWorkBench(2), parent first. */
	sc->n = 0;
	sc_add(sc, SLOT_LOCAL, ev_kind(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 17, 1));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 58, 1));
	run_both(sc, "(iv-a) pickup wood then craft workbench", 0u);
	assert(ach_unlocked(prof(SLOT_LOCAL), 2));
	r++; /* wood->bench */

	/* b) the same pair, child first: SlotCrafting's addStat is DROPPED. */
	sc->n = 0;
	sc_add(sc, SLOT_LOCAL, ev_kind(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 58, 1));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 17, 1));
	run_both(sc, "(iv-b) craft workbench then pickup wood", 0u);
	assert(!ach_unlocked(prof(SLOT_LOCAL), 2));
	assert(ach_unlocked(prof(SLOT_LOCAL), 1));
	/* The craftItem counter is NOT dropped with the achievement. */
	assert(stats_counter_total(&prof(SLOT_LOCAL)->craft[58]) == 1u);
	r++; /* bench->wood */

	/* c) buildWorkBench(2) -> buildPickaxe(3), parent first. */
	sc->n = 0;
	sc_add(sc, SLOT_LOCAL, ev_kind(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 17, 1));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 58, 1));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 270, 1));
	run_both(sc, "(iv-c) craft workbench then pickaxe", 0u);
	assert(ach_unlocked(prof(SLOT_LOCAL), 3));
	r++; /* bench->pick */

	/* d) pickaxe before workbench. */
	sc->n = 0;
	sc_add(sc, SLOT_LOCAL, ev_kind(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 17, 1));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 270, 1));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 58, 1));
	run_both(sc, "(iv-d) craft pickaxe then workbench", 0u);
	assert(!ach_unlocked(prof(SLOT_LOCAL), 3));
	assert(ach_unlocked(prof(SLOT_LOCAL), 2));
	r++; /* pick->bench */

	/* e) buildSword(12) -> killEnemy(13), parent first. */
	sc->n = 0;
	sc_add(sc, SLOT_LOCAL, ev_kind(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 17, 1));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 58, 1));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 268, 1));
	sc_add(sc, SLOT_LOCAL,
		   ev_kind(GE_KILLED, GE_AUTH_SERVER, MONSTER_ZOMBIE));
	run_both(sc, "(iv-e) craft sword then kill a hostile", 0u);
	assert(ach_unlocked(prof(SLOT_LOCAL), 13));
	r++; /* sword->kill */

	/* f) the kill first: dropped, but mobKills still counts [design 1.2]. */
	sc->n = 0;
	sc_add(sc, SLOT_LOCAL, ev_kind(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 17, 1));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 58, 1));
	sc_add(sc, SLOT_LOCAL,
		   ev_kind(GE_KILLED, GE_AUTH_SERVER, MONSTER_ZOMBIE));
	sc_add(sc, SLOT_LOCAL, ev_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 268, 1));
	run_both(sc, "(iv-f) kill a hostile then craft the sword", 0u);
	assert(!ach_unlocked(prof(SLOT_LOCAL), 13));
	{
		int mk = stat_general_index(2023u);
		assert(stats_counter_total(&prof(SLOT_LOCAL)->general[mk]) == 1u);
	}
	r++; /* kill->sword */

	printf("PASS (iv) child-before-parent vs parent-before-child -- %d edge "
		   "orderings x 2 ring states, %s\n",
		   r,
		   "every unlock/drop and every counter equal to the reference");
}

/* ---- (v) seeded random scripts ---------------------------------------- */

#define RAND_SCRIPTS 200
#define RAND_EVENTS 2000
/* Every script event lands past a full main ring in the forced-full run, so
 * the ORDERED share must stay inside the overflow ring. Beyond this the module
 * is right to reject loudly (case (vii) proves it does), which is not what
 * these scripts are for. */
#define RAND_ORDERED_BUDGET 1200

static uint32_t rnd32(uint32_t* s) {
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

static uint32_t rnd_below(uint32_t* s, uint32_t n) {
	return rnd32(s) % n;
}

/* ---- THE POOLS (R2 HIGH 3) ---------------------------------------------
 *
 * The old pools contained only ids every array registers, so the half of the
 * sweep that exercises the F3/F4 registration gates could not fail: a wrong
 * gate never met an id it would have gated. Every pool below deliberately
 * mixes the four outcomes -- registered, gated to nothing, folded onto another
 * id, and outside Beta entirely -- in the array that actually decides:
 *
 *   2, 60   grass / farmland: a mine+use counter that FOLDS onto dirt 3, and
 *           NO craft counter at all (neither side is a recipe output).
 *   62      lit furnace: folds onto 61 in craft, has no mine counter.
 *   91, 86  jack-o-lantern / pumpkin: the craft fold INVERTS (the source is
 *           the output), so the shared counter is 91.
 *   76, 75  the redstone torch pair: the other inverting craft fold.
 *   43, 44  double slab / slab: folds the ordinary way.
 *   8, 9    the water pair: disableStats in mine, registered in use, and the
 *           (9,8) direction B3 corrected.
 *   18      leaves: disableStats, so no mine counter although Beta's
 *           harvestBlock calls addStat.
 *   97, 200 CaveWii-only blocks: no Beta block, so NO statistic anywhere.
 *   260     apple, 280 stick: an item that is not a recipe output and one
 *           that is -- the R2 MED 8 craft gate, on items.
 *   256..   iron shovel / flint-and-steel / rod / shears: damagable, so they
 *           have a breakItem counter where 260, 280 and every block do not.
 */
static const uint16_t RND_BLOCK[] = {1,  2,  3,  17, 18, 60, 62, 91, 76,
									 43, 44, 86, 75, 8,  9,  61, 97, 200};
static const uint16_t RND_CRAFT_ID[] = {
	/* the eight SlotCrafting achievement ids -- the ORDERED payloads */
	58, 270, 61, 290, 297, 354, 274, 268,
	/* and payloads that are pure counters, on both sides of every gate */
	5, 44, 43, 91, 86, 76, 75, 62, 2, 60, 8, 9, 18, 97, 200, 260, 280, 351,
	359};
static const uint16_t RND_USE_ID[] = {270, 274, 290, 2,  3,   60, 62, 64,
									  92,  8,   9,   18, 97,  200, 260, 359};
static const uint16_t RND_BREAK_ID[] = {256, 259, 270, 274, 290, 346, 359,
										260, 280, 262, 2,   3,   60,  97,
										200};
static const uint16_t RND_FURNACE[] = {265, 350, 263, 20, 15, 349, 336};
static const uint16_t RND_PICKUP[] = {17, 334, 1, 4, 262};
/* Bounded on purpose: a coalescer key is (kind, sub, authority, id), and the
 * table holds 512 of them. These deliberately include ids this build has NO
 * counter for, which the module answers with ACH_REJECT_UNKNOWN_STAT and the
 * reference with "apply nothing". */
static const int32_t RND_REMOTE_COUNTER[] = {
	2020,
	2021,
	2022,
	2023,
	2024,
	2025,
	(int32_t)(CW_STAT_MINE_BASE + 1),
	(int32_t)(CW_STAT_MINE_BASE + 3),
	(int32_t)(CW_STAT_MINE_BASE + 2),   /* folds onto 3      */
	(int32_t)(CW_STAT_MINE_BASE + 18),  /* disableStats      */
	(int32_t)(CW_STAT_MINE_BASE + 97),  /* no Beta block     */
	(int32_t)(CW_STAT_MINE_BASE + 200), /* no Beta block     */
	(int32_t)(CW_STAT_CRAFT_BASE + 58),
	(int32_t)(CW_STAT_CRAFT_BASE + 62),  /* folds onto 61    */
	(int32_t)(CW_STAT_CRAFT_BASE + 86),  /* INVERTS onto 91  */
	(int32_t)(CW_STAT_CRAFT_BASE + 3),   /* uncraftable      */
	(int32_t)(CW_STAT_CRAFT_BASE + 260), /* apple: not an output */
	(int32_t)(CW_STAT_CRAFT_BASE + 280),
	(int32_t)(CW_STAT_USE_BASE + 270),
	(int32_t)(CW_STAT_USE_BASE + 2), /* folds onto 3         */
	(int32_t)(CW_STAT_USE_BASE + 97),
	(int32_t)(CW_STAT_BREAK_BASE + 274),
	(int32_t)(CW_STAT_BREAK_BASE + 2),   /* no block breaks   */
	(int32_t)(CW_STAT_BREAK_BASE + 260), /* not damagable     */
	0x7000000 /* an id this build has no counter for */};

#define PICK(st, arr) ((arr)[rnd_below((st), (uint32_t)(sizeof(arr) / sizeof((arr)[0])))])

/* Build one well-formed event for `slot`, which is bound in `mode`. Picks
 * 0..9 are kinds that can never be ORDERED, so `allow_ordered == false`
 * restricts the generator to them once the ordered budget is spent; the
 * caller classifies what actually came out with harness_class(). Authorities
 * are the correct ones for the slot's bind mode most of the time and
 * deliberately wrong 1 time in 32, which exercises the rejection rows of the
 * 5.2 matrix under the ring split -- the reference applies the same matrix, so
 * a rejection is compared too. */
static struct game_event gen_event(uint32_t* st, enum ach_bind_mode mode,
								   bool allow_ordered) {
	bool remote_mode = (mode == ACH_MODE_REMOTE_SERVER);
	bool flip = (rnd_below(st, 32u) == 0u);
	uint8_t client = flip ? GE_AUTH_SERVER : GE_AUTH_CLIENT;
	uint8_t server = flip ? GE_AUTH_CLIENT : GE_AUTH_SERVER;
	uint32_t pick = rnd_below(st, allow_ordered ? 18u : 10u);
	switch(pick) {
		/* ---- never ORDERED ------------------------------------------ */
		case 0:
			if(remote_mode)
				return ev_remote(PICK(st, RND_REMOTE_COUNTER),
								 (int32_t)rnd_below(st, 5u) + 1);
			return ev_item(GE_BLOCK_HARVESTED, server, PICK(st, RND_BLOCK), 1);
		case 1:
			if(remote_mode)
				return ev_remote(PICK(st, RND_REMOTE_COUNTER),
								 (int32_t)rnd_below(st, 9u) + 1);
			return ev_item(GE_ITEM_USED, server, PICK(st, RND_USE_ID), 1);
		case 2:
			if(remote_mode)
				return ev_remote(PICK(st, RND_REMOTE_COUNTER), 1);
			return ev_item(GE_ITEM_BROKEN, server, PICK(st, RND_BREAK_ID), 1);
		case 3:
			if(remote_mode)
				return ev_kind(GE_GAME_STARTED, client,
							   (uint8_t)rnd_below(st, 2u));
			return ev_n(GE_DAMAGE_DEALT, server, (int32_t)rnd_below(st, 7u));
		case 4:
			if(remote_mode)
				return ev_kind(GE_GAME_LEFT, client, 0);
			return ev_n(GE_DAMAGE_TAKEN, server, (int32_t)rnd_below(st, 5u));
		case 5:
			if(remote_mode)
				return ev_kind(GE_MP_JOINED, client, 0);
			return ev_kind(GE_DIED, server, 0);
		case 6: return ev_kind(GE_JUMPED, client, 0);
		case 7: return ev_kind(GE_DROPPED, client, 0);
		case 8:
			return ev_move(GE_FELL, client, 0,
						   (int32_t)rnd_below(st, 700u) + 200);
		case 9:
			/* FOLDABLE again: a movement is a pure distance counter now that
			 * the rail-origin reset lives on the tick (R2 MED 4). */
			return ev_move(GE_MOVED, client,
						   (uint8_t)rnd_below(st, GE_MOVE_KIND_COUNT),
						   (int32_t)rnd_below(st, 30u) + 1);
		/* ---- may be ORDERED ----------------------------------------- */
		case 10: return ev_kind(GE_INVENTORY_OPENED, client, 0);
		case 11:
			/* The tick, with a mount kind: 0 resets the rail origin, a riding
			 * value does not. Both appear, so the reset site is swept. */
			return ev_tick((uint8_t)(rnd_below(st, 4u) == 0u
										 ? GE_TICK_RIDING_BASE
											 + GE_MOUNT_MINECART
										 : GE_TICK_NOT_RIDING));
		case 12: {
			int32_t x = (int32_t)rnd_below(st, 81u) - 40;
			int32_t y = (int32_t)rnd_below(st, 21u) - 10;
			int32_t z = (int32_t)rnd_below(st, 81u) - 40;
			return ev_ride(GE_MOUNT_MINECART, x, y, z,
						   (int32_t)rnd_below(st, 20u) + 1);
		}
		case 13: return ev_kind(GE_RIDE_END, client, GE_MOUNT_MINECART);
		case 14:
			/* A boat or pig ride is a pure distance counter: FOLDABLE. */
			return ev_ride((uint8_t)(GE_MOUNT_BOAT + rnd_below(st, 2u)), 0, 0,
						   0, (int32_t)rnd_below(st, 25u) + 1);
		case 15:
			if(remote_mode)
				return ev_remote(
					(int32_t)(CW_ACH_BASE + rnd_below(st, CW_ACH_COUNT)), 1);
			return ev_item(GE_ITEM_PICKED_UP, server, PICK(st, RND_PICKUP), 1);
		case 16:
			if(remote_mode)
				return ev_remote(
					(int32_t)(CW_ACH_BASE + rnd_below(st, CW_ACH_COUNT)), 1);
			return ev_item(GE_CRAFT_TAKEN, server, PICK(st, RND_CRAFT_ID),
						   (uint16_t)(rnd_below(st, 3u) + 1));
		default:
			if(remote_mode)
				return ev_remote(
					(int32_t)(CW_ACH_BASE + rnd_below(st, CW_ACH_COUNT)), 1);
			if(rnd_below(st, 3u) == 0u)
				/* Straddles Beta's `f > 5F` boundary, including the values in
				 * (5.0, 5.005] that the old x100 integer predicate missed. */
				return ev_pig(4.5F + (float)rnd_below(st, 1200u) * 0.001F);
			if(rnd_below(st, 2u) == 0u)
				return ev_kind(GE_KILLED, server,
							   (uint8_t)rnd_below(st, MONSTER_COUNT));
			return ev_item(GE_FURNACE_TAKEN, server, PICK(st, RND_FURNACE),
						   (uint16_t)(rnd_below(st, 2u) + 1));
	}
}

static void scenario_random_scripts(void) {
	unsigned long ordered_total = 0;
	unsigned long noop_total = 0;
	int mode_mix[2][2] = {{0, 0}, {0, 0}};
	for(int s = 0; s < RAND_SCRIPTS; s++) {
		uint32_t seed = 0x5eed0000u + (uint32_t)s * 2654435761u;
		if(seed == 0u)
			seed = 1u;
		uint32_t st = seed;
		struct script* sc = &g_script;
		enum ach_bind_mode modes[CW_ACH_SLOT_COUNT];
		int budget = RAND_ORDERED_BUDGET;
		sc->n = 0;
		/* R2 LOW 14: the slot/mode assignment is part of the script, so all
		 * four combinations (and therefore both halves of the 5.2 matrix on
		 * either slot) are swept instead of slot 0 always being LOCAL. */
		for(int i = 0; i < CW_ACH_SLOT_COUNT; i++)
			modes[i] = (rnd_below(&st, 2u) == 0u) ? ACH_MODE_LOCAL_WORLD
												  : ACH_MODE_REMOTE_SERVER;
		mode_mix[modes[0] == ACH_MODE_REMOTE_SERVER]
				[modes[1] == ACH_MODE_REMOTE_SERVER]++;
		while(sc->n < RAND_EVENTS) {
			uint8_t slot = (uint8_t)rnd_below(&st, CW_ACH_SLOT_COUNT);
			struct game_event ev = gen_event(&st, modes[slot], budget > 0);
			int cls = harness_class(&ev);
			if(cls == 1) {
				budget--;
				ordered_total++;
			} else if(cls < 0) {
				noop_total++;
			}
			sc_add(sc, slot, ev);
		}
		run_both_modes(sc, "(v) seeded random script", seed, modes[0],
					   modes[1]);
	}
	printf("PASS (v) seeded random scripts -- %d scripts x %d events x 2 ring "
		   "states, mode mix LL/LR/RL/RR = %d/%d/%d/%d, %lu ordered and %lu "
		   "no-op events, %lu counters and %lu toasts compared against the "
		   "golden-driven reference\n",
		   RAND_SCRIPTS, RAND_EVENTS, mode_mix[0][0], mode_mix[0][1],
		   mode_mix[1][0], mode_mix[1][1], ordered_total, noop_total,
		   g_counters_compared, g_notices_compared);
}

/* ====================================================================== */
/* 6. (vi) THE FULL STRESS ENVELOPE                                       */
/* ====================================================================== */

/* design 5.2's supported envelope: <= 64 events per player per server tick,
 * two players, 20 TPS, a 10 s drain stall = 25,600 events.
 *
 * WHAT CHANGED WITH v3. The ORDERED share of that traffic is what the overflow
 * ring has to hold, and design 5.2 sizes the ring for the ordered subset at
 * Beta's own rate: one movement (or one minecart tick -- addMovementStat
 * returns early while riding, so they are mutually exclusive) per player per
 * tick, plus human-rate discrete facts. The producers below emit exactly that
 * shape -- 1 ordered client event per player per tick and 1 ordered discrete
 * event every other tick -- and fill the remaining 62 per player per tick with
 * the pure counters a tick really does produce. A script that instead emitted
 * a dozen achievement-bearing events per tick for ten seconds straight is
 * OUTSIDE the envelope the ring is sized for, and the module's right answer
 * there is the loud ACH_REJECT_OVERFLOW that case (vii) proves.
 *
 * Slot 0 runs in a LOCAL world (server + client producers) and slot 1 on a
 * REMOTE server (client + network producers), so all three threads and both
 * halves of the 5.2 matrix are live at once. */

#define SEAM_TPS 20
#define SEAM_SECONDS 10
#define SEAM_TICKS (SEAM_TPS * SEAM_SECONDS)	 /* 200 ticks           */
#define SEAM_PER_TICK_PER_PLAYER 64				 /* design 5.2 envelope */
#define SEAM_HALF (SEAM_PER_TICK_PER_PLAYER / 2) /* 32 per producer     */

#define REF_SERVER 0
#define REF_CLIENT 1
#define REF_NETWORK 2
#define REF_MAIN 3
#define REF_PRODUCERS 4

/* Per-producer counter tables. Sums, so the interleaving of the three threads
 * cannot change them -- and the seed below unlocks every achievement the burst
 * can trigger, so no first-unlock (and therefore no toast, the one outcome
 * that WOULD be order-dependent across threads) happens during the burst. */
struct ref {
	uint64_t general[CW_STAT_COUNT];
	uint64_t mine[CW_STAT_BLOCK_COUNT];
	uint64_t ach[CW_ACH_COUNT];
	uint64_t craft[ITEMS_MAX];
	uint64_t use[ITEMS_MAX];
};

static struct ref g_sref[REF_PRODUCERS][CW_ACH_SLOT_COUNT];
static struct ref g_ssum[CW_ACH_SLOT_COUNT];

static void sref_general(struct ref* r, uint32_t stat_id, uint64_t v) {
	int i = stat_general_index(stat_id);
	assert(i >= 0);
	r->general[i] += v;
}

static struct game_event mk(uint8_t kind, uint8_t authority, uint8_t slot) {
	struct game_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.kind = kind;
	ev.authority = authority;
	ev.who.slot = slot;
	ev.who.generation = generation_for(slot);
	return ev;
}

/* Server-decided facts for the local-world player (slot 0). */
static void* producer_server(void* arg) {
	(void)arg;
	struct ref* r = &g_sref[REF_SERVER][0];
	for(int t = 0; t < SEAM_TICKS; t++) {
		for(int k = 0; k < SEAM_HALF; k++) {
			struct game_event ev;
			if(k == 0 && (t % 2) == 0) {
				/* The ONE ordered, achievement-bearing fact of this tick. */
				switch((t / 2) % 4) {
					case 0:
						ev = mk(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 0);
						ev.u.item.id = 17;
						ev.u.item.count = 3;
						game_event_emit(&ev);
						r->ach[1] += 1u; /* mineWood, one per pickup */
						break;
					case 1:
						ev = mk(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 0);
						ev.u.item.id = 58;
						ev.u.item.count = 2;
						game_event_emit(&ev);
						r->craft[58] += 2u; /* craftItem += stackSize */
						r->ach[2] += 1u;	/* buildWorkBench */
						break;
					case 2:
						ev = mk(GE_KILLED, GE_AUTH_SERVER, 0);
						ev.sub = MONSTER_ZOMBIE;
						game_event_emit(&ev);
						sref_general(r, 2023u, 1u); /* mobKills  */
						r->ach[13] += 1u;			/* killEnemy */
						break;
					default:
						ev = mk(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 0);
						ev.u.item.id = 334;
						ev.u.item.count = 1;
						game_event_emit(&ev);
						r->ach[14] += 1u; /* killCow */
						break;
				}
				continue;
			}
			switch(k % 6) {
				case 0:
				case 1: {
					/* grass folds onto the dirt counter [design 2]. */
					uint16_t block = (k % 2) ? 2u : 17u;
					ev = mk(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 0);
					ev.u.item.id = block;
					ev.u.item.count = 1;
					game_event_emit(&ev);
					r->mine[ref_counter_in(CW_STAT_ARRAY_MINE, block)] += 1u;
					break;
				}
				case 2:
					ev = mk(GE_ITEM_USED, GE_AUTH_SERVER, 0);
					ev.u.item.id = 270;
					ev.u.item.count = 1;
					game_event_emit(&ev);
					r->use[270] += 1u;
					break;
				case 3:
					ev = mk(GE_DAMAGE_DEALT, GE_AUTH_SERVER, 0);
					ev.u.n.amount = 3;
					game_event_emit(&ev);
					sref_general(r, 2020u, 3u);
					break;
				case 4:
					ev = mk(GE_DAMAGE_TAKEN, GE_AUTH_SERVER, 0);
					ev.u.n.amount = 1;
					game_event_emit(&ev);
					sref_general(r, 2021u, 1u);
					break;
				default:
					ev = mk(GE_FISH_CAUGHT, GE_AUTH_SERVER, 0);
					game_event_emit(&ev);
					sref_general(r, 2025u, 1u);
					break;
			}
		}
	}
	return NULL;
}

/* Client-decided facts, for BOTH players: the one producer Beta runs in local
 * and remote play alike [design 5.2 "Producer"]. The ORDERED share is ONE
 * GE_TICK per player per tick -- exactly the rate design 5.2 v4 sizes the ring
 * from, and the event that carries the rail-origin reset (R2 MED 4). Movement
 * is a pure counter again and folds with the rest. */
static void* producer_client(void* arg) {
	(void)arg;
	for(int t = 0; t < SEAM_TICKS; t++) {
		for(uint8_t slot = 0; slot < CW_ACH_SLOT_COUNT; slot++) {
			struct ref* r = &g_sref[REF_CLIENT][slot];
			for(int k = 0; k < SEAM_HALF; k++) {
				struct game_event ev;
				if(k == 0) {
					/* THE one ordered client event of this player's tick. */
					ev = mk(GE_TICK, GE_AUTH_CLIENT, slot);
					ev.sub = GE_TICK_NOT_RIDING;
					game_event_emit(&ev);
					sref_general(r, 1100u, 1u);
					continue;
				}
				switch(k % 4) {
					case 0:
						ev = mk(GE_DROPPED, GE_AUTH_CLIENT, slot);
						game_event_emit(&ev);
						sref_general(r, 2011u, 1u);
						break;
					case 1:
						ev = mk(GE_MOVED, GE_AUTH_CLIENT, slot);
						ev.sub = GE_MOVE_WALK;
						ev.u.move.cm = 7;
						game_event_emit(&ev);
						sref_general(r, 2000u, 7u);
						break;
					case 2:
						ev = mk(GE_JUMPED, GE_AUTH_CLIENT, slot);
						game_event_emit(&ev);
						sref_general(r, 2010u, 1u);
						break;
					default:
						ev = mk(GE_FELL, GE_AUTH_CLIENT, slot);
						ev.u.move.cm = 250;
						game_event_emit(&ev);
						sref_general(r, 2002u, 250u);
						break;
				}
			}
		}
	}
	return NULL;
}

/* The remote client's Packet 200 decoder, for the remote-server player. */
static void* producer_network(void* arg) {
	(void)arg;
	struct ref* r = &g_sref[REF_NETWORK][1];
	for(int t = 0; t < SEAM_TICKS; t++) {
		for(int k = 0; k < SEAM_HALF; k++) {
			struct game_event ev = mk(GE_REMOTE_STAT, GE_AUTH_PACKET200, 1);
			if(k == 0 && (t % 2) == 0) {
				/* An achievement over Packet 200 goes through the same
				 * addStat path, incremented by the packet's amount: ORDERED. */
				ev.u.remote.stat_id = (int32_t)(CW_ACH_BASE + 1);
				ev.u.remote.amount = 1;
				r->ach[1] += 1u;
				game_event_emit(&ev);
				continue;
			}
			switch(k % 3) {
				case 0:
					ev.u.remote.stat_id = 2022; /* deaths */
					ev.u.remote.amount = 1;
					sref_general(r, 2022u, 1u);
					break;
				case 1:
					ev.u.remote.stat_id = 2025; /* fishCaught */
					ev.u.remote.amount = 2;
					sref_general(r, 2025u, 2u);
					break;
				default:
					ev.u.remote.stat_id = (int32_t)(CW_STAT_MINE_BASE + 3);
					ev.u.remote.amount = 5;
					r->mine[3] += 5u;
					break;
			}
			game_event_emit(&ev);
		}
	}
	return NULL;
}

static void sum_srefs(void) {
	memset(g_ssum, 0, sizeof(g_ssum));
	for(int th = 0; th < REF_PRODUCERS; th++)
		for(int s = 0; s < CW_ACH_SLOT_COUNT; s++) {
			for(int i = 0; i < CW_STAT_COUNT; i++)
				g_ssum[s].general[i] += g_sref[th][s].general[i];
			for(int i = 0; i < CW_STAT_BLOCK_COUNT; i++)
				g_ssum[s].mine[i] += g_sref[th][s].mine[i];
			for(int i = 0; i < CW_ACH_COUNT; i++)
				g_ssum[s].ach[i] += g_sref[th][s].ach[i];
			for(int i = 0; i < ITEMS_MAX; i++) {
				g_ssum[s].craft[i] += g_sref[th][s].craft[i];
				g_ssum[s].use[i] += g_sref[th][s].use[i];
			}
		}
}

static void compare_stress_slot(uint8_t slot) {
	const struct stats_profile* p = prof(slot);
	const struct ref* r = &g_ssum[slot];
	for(int i = 0; i < CW_STAT_COUNT; i++)
		assert(stats_counter_total(&p->general[i]) == sat32(r->general[i]));
	for(int i = 0; i < CW_STAT_BLOCK_COUNT; i++)
		assert(stats_counter_total(&p->mine[i]) == sat32(r->mine[i]));
	for(int i = 0; i < CW_ACH_COUNT; i++)
		assert(stats_counter_total(&p->ach[i]) == sat32(r->ach[i]));
	for(int i = 0; i < ITEMS_MAX; i++) {
		assert(stats_counter_total(&p->craft[i]) == sat32(r->craft[i]));
		assert(stats_counter_total(&p->use[i]) == sat32(r->use[i]));
		assert(stats_counter_total(&p->brk[i]) == 0u);
	}
	/* Nothing in the burst is a pickup Beta keeps no statistic for. */
	assert(status_of(slot).noop_events == 0u);
}

/* Everything the burst can trigger is unlocked here, on the main thread,
 * BEFORE the producers start -- so the only cross-thread parent edge design
 * 5.2 records as an accepted limitation (client openInventory -> server
 * pickup) cannot be raced, and no toast is queued during the burst. */
static int seed_prerequisites(void) {
	struct ref* r0 = &g_sref[REF_MAIN][0];
	struct ref* r1 = &g_sref[REF_MAIN][1];
	int n = 0;

	struct game_event ev = mk(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0);
	game_event_emit(&ev);
	r0->ach[0] += 1u;
	n++;
	ev = mk(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 0);
	ev.u.item.id = 17;
	ev.u.item.count = 1;
	game_event_emit(&ev);
	r0->ach[1] += 1u;
	n++;
	ev = mk(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 0);
	ev.u.item.count = 1;
	ev.u.item.id = 58;
	game_event_emit(&ev);
	r0->craft[58] += 1u;
	r0->ach[2] += 1u;
	n++;
	ev.u.item.id = 268; /* buildSword, the parent of killEnemy and killCow */
	game_event_emit(&ev);
	r0->craft[268] += 1u;
	r0->ach[12] += 1u;
	n++;
	ev = mk(GE_KILLED, GE_AUTH_SERVER, 0);
	ev.sub = MONSTER_ZOMBIE;
	game_event_emit(&ev);
	sref_general(r0, 2023u, 1u);
	r0->ach[13] += 1u;
	n++;
	ev = mk(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 0);
	ev.u.item.id = 334;
	ev.u.item.count = 1;
	game_event_emit(&ev);
	r0->ach[14] += 1u;
	n++;

	/* openInventory is Beta's one client-owned achievement, so the remote
	 * player earns it locally even on a remote server; mineWood then arrives
	 * from the server as Packet 200. */
	ev = mk(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 1);
	game_event_emit(&ev);
	r1->ach[0] += 1u;
	n++;
	ev = mk(GE_REMOTE_STAT, GE_AUTH_PACKET200, 1);
	ev.u.remote.stat_id = (int32_t)(CW_ACH_BASE + 1);
	ev.u.remote.amount = 1;
	game_event_emit(&ev);
	r1->ach[1] += 1u;
	n++;

	achievements_tick();
	assert(ach_unlocked(prof(0), 13) && ach_unlocked(prof(0), 14)
		   && ach_unlocked(prof(1), 1));
	return n;
}

static void scenario_stress_envelope(void) {
	memset(g_sref, 0, sizeof(g_sref));
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, GEN_P0, "seamlocal", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	assert(achievements_bind(1, GEN_P1, "seamremote", ACH_MODE_REMOTE_SERVER)
		   == ACH_BIND_OK);
	int seeds = seed_prerequisites();
	uint32_t queued0 = status_of(0).notices_queued;
	uint32_t queued1 = status_of(1).notices_queued;

	g_cap_n = 0;
	achievements_debug_notice_hook(cap_hook);

	struct thread th_server, th_client, th_network;
	assert(thread_create(&th_server, producer_server, NULL, 64));
	assert(thread_create(&th_client, producer_client, NULL, 64));
	assert(thread_create(&th_network, producer_network, NULL, 64));
	thread_join(&th_server);
	thread_join(&th_client);
	thread_join(&th_network);

	/* The drain was stalled for the whole burst, so the main ring is long
	 * since full: the ordered events went to the overflow ring and the pure
	 * counters folded. */
	struct ach_status mid0 = status_of(0);
	struct ach_status mid1 = status_of(1);
	assert(mid0.ring_folds + mid1.ring_folds > 0u);
	assert(mid0.overflow_used + mid1.overflow_used > 0u);
	uint32_t peak = (mid0.overflow_peak > mid1.overflow_peak)
		? mid0.overflow_peak
		: mid1.overflow_peak;
	achievements_tick();
	achievements_debug_notice_hook(NULL);

	sum_srefs();
	compare_stress_slot(0);
	compare_stress_slot(1);

	struct ach_status st0 = status_of(0);
	struct ach_status st1 = status_of(1);
	assert(!st0.progress_incomplete && !st1.progress_incomplete);
	assert(prof(0)->progress_incomplete == 0
		   && prof(1)->progress_incomplete == 0);
	assert(st0.coalesce_peak <= CW_ACH_COALESCE_CAPACITY);
	assert(st1.coalesce_peak <= CW_ACH_COALESCE_CAPACITY);
	assert(st0.overflow_peak <= CW_ACH_OVERFLOW_CAPACITY);
	assert(st1.overflow_peak <= CW_ACH_OVERFLOW_CAPACITY);
	assert(st0.overflow_used == 0u && st1.overflow_used == 0u);
	for(int r = 0; r < ACH_REJECT_COUNT; r++) {
		assert(st0.rejected[r] == 0u);
		assert(st1.rejected[r] == 0u);
	}
	assert(achievements_debug_bad_slot_drops() == 0u);
	/* Every achievement the burst touches was already unlocked by the seed,
	 * so the repeats increment counts and queue no toast at all -- the one
	 * outcome cross-thread interleaving could otherwise reorder. */
	assert(g_cap_n == 0);
	assert(st0.notices_queued == queued0 && st1.notices_queued == queued1);

	uint32_t burst = (uint32_t)SEAM_TICKS * SEAM_PER_TICK_PER_PLAYER
		* CW_ACH_SLOT_COUNT;
	assert(burst == 25600u);
	assert(st0.accepted + st1.accepted == burst + (uint32_t)seeds);

	printf("PASS (vi) stress envelope -- %u events over 3 threads (%u folded "
		   "as counters; ordered ring peak %u per slot, %u across both, of "
		   "%u), every achievement count, counter and notice identical to the "
		   "reference\n",
		   (unsigned)(st0.accepted + st1.accepted),
		   (unsigned)(st0.ring_folds + st1.ring_folds), (unsigned)peak,
		   (unsigned)(mid0.overflow_peak + mid1.overflow_peak),
		   (unsigned)CW_ACH_OVERFLOW_CAPACITY);
}

/* ====================================================================== */
/* 7. (vii) PAST THE ENVELOPE: LOUD, NEVER A SILENT FOLD                  */
/* ====================================================================== */

static void scenario_overflow_is_loud(void) {
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, GEN_P0, "seamloud", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	/* Fill the main ring with a FOLDABLE kind ... */
	for(int i = 0; i < CW_ACH_RING_CAPACITY; i++) {
		struct game_event ev = mk(GE_JUMPED, GE_AUTH_CLIENT, 0);
		game_event_emit(&ev);
	}
	assert(status_of(0).ring_folds == 0u);
	/* ... then the ordered overflow ring, exactly to capacity ... */
	for(int i = 0; i < CW_ACH_OVERFLOW_CAPACITY; i++) {
		struct game_event ev = mk(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0);
		game_event_emit(&ev);
	}
	struct ach_status full = status_of(0);
	assert(full.overflow_used == (uint32_t)CW_ACH_OVERFLOW_CAPACITY);
	assert(full.ring_folds == 0u); /* an ORDERED kind NEVER folds */
	assert(full.rejected[ACH_REJECT_OVERFLOW] == 0u);
	assert(!full.progress_incomplete);
	/* ... and one more is REJECTED and counted, not approximated. */
	for(int i = 0; i < 7; i++) {
		struct game_event ev = mk(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0);
		game_event_emit(&ev);
	}
	struct ach_status over = status_of(0);
	assert(over.rejected[ACH_REJECT_OVERFLOW] == 7u);
	assert(over.ring_folds == 0u);
	assert(over.progress_incomplete);
	assert(prof(0)->progress_incomplete == 1);

	achievements_tick();
	/* The 1024 that DID queue were all applied, in order. */
	assert(stats_counter_total(&prof(0)->ach[0])
		   == (uint32_t)CW_ACH_OVERFLOW_CAPACITY);
	assert(status_of(0).overflow_used == 0u);
	printf("PASS (vii) past the envelope: %d ordered events queued exactly, "
		   "7 rejected as ACH_REJECT_OVERFLOW with progress_incomplete set -- "
		   "never a silent fold\n",
		   CW_ACH_OVERFLOW_CAPACITY);
}

/* ====================================================================== */

/* Within ONE producer thread, ring order is program order, so a parent
 * emitted before its child is always applied first [design 5.2 "Causal
 * order"]. */
static void* ordering_producer(void* arg) {
	(void)arg;
	for(int i = 0; i < 64; i++) {
		struct game_event ev = mk(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 0);
		ev.u.item.count = 1;
		ev.u.item.id = 58; /* buildWorkBench,     parent mineWood       */
		game_event_emit(&ev);
		ev.u.item.id = 270; /* buildPickaxe,       parent buildWorkBench */
		game_event_emit(&ev);
		ev.u.item.id = 274; /* buildBetterPickaxe, parent buildPickaxe   */
		game_event_emit(&ev);
	}
	return NULL;
}

static void test_ordering_within_a_thread(void) {
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, GEN_P0, "seamorder", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	struct game_event ev = mk(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0);
	game_event_emit(&ev);
	ev = mk(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 0);
	ev.u.item.id = 17;
	ev.u.item.count = 1;
	game_event_emit(&ev);
	achievements_tick();
	assert(ach_unlocked(prof(0), 1));

	struct thread th;
	assert(thread_create(&th, ordering_producer, NULL, 64));
	thread_join(&th);
	/* 192 events, well inside the ring: this case is about ORDER, not the
	 * split. */
	assert(status_of(0).ring_folds == 0u);
	assert(status_of(0).overflow_used == 0u);
	achievements_tick();

	assert(ach_unlocked(prof(0), 2) && ach_unlocked(prof(0), 3)
		   && ach_unlocked(prof(0), 9));
	assert(stats_counter_total(&prof(0)->ach[2]) == 64u);
	assert(stats_counter_total(&prof(0)->ach[3]) == 64u);
	assert(stats_counter_total(&prof(0)->ach[9]) == 64u);
	assert(status_of(0).ach_dropped_parent_locked == 0u);
	puts("PASS ring order is program order within one producer thread");
}

int main(void) {
	test_fixture_dir("test_achievements_seam_tmp", s_dir, sizeof(s_dir));
	make_dir(s_dir);
	/* The reference model's ONLY source of registration/alias truth (R2
	 * HIGH 2): two hand-derived goldens, read before anything runs. */
	ref_load_goldens();

	test_ordering_within_a_thread();
	scenario_rail_cross_and_return();
	scenario_rail_mixed();
	scenario_dismount_remount();
	scenario_parent_order();
	scenario_random_scripts();
	scenario_stress_envelope();
	scenario_overflow_is_loud();

	achievements_shutdown(); /* release the module block before the leak check */
	test_fixture_dir_cleanup(s_dir);
	puts("PASS test_achievements_seam");
	return 0;
}
