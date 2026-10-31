/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* REL-11a: the PURE phase driver for the first half of the controller-only
 * survival journey (R11): mine a real block, craft through the real inventory
 * UI, store + retrieve a chest stack, fuel + cook + retrieve furnace output.
 *
 * Same shape as mp_pause_menu_core.h: no gfx/game_state/server dependency. The
 * (future, REL-11-adapter-a) impure caller fills `struct sj_obs` from
 * AUTHORITATIVE committed state each frame and turns `struct sj_out` into input
 * through the existing hooks (manifest: tmp/jobs/release-plan/REL-11a/
 * MANIFEST.md). The driver never mutates the world or an inventory: it only
 * requests controls and judges observations. A labelled prebuilt fixture
 * (SJ_FIXTURE_NAME) arranges the starting materials and terrain.
 *
 * Success is never "an action was submitted". An ACT phase succeeds only when
 * the server ACCEPTED a reply after the submit, no optimistic action is still
 * pending, AND the committed counts moved the way the phase demands. A reply
 * that was accepted but left the committed state unchanged is its own failure
 * (SJ_FAIL_UNCOMMITTED), distinct from no reply at all (SJ_FAIL_TIMEOUT) and
 * from a rejected transaction (SJ_FAIL_REJECTED).
 *
 * Input convention (autopilot_script_buttons / menu_autopilot): an action edge
 * is requested on ONE frame only and is always followed by at least one
 * neutral frame; edges are never retried (a lost edge fails by name rather than
 * risking a duplicate transaction). Mining is the one held LEVEL. */
#ifndef GAME_SURVIVAL_JOURNEY_CORE_H
#define GAME_SURVIVAL_JOURNEY_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define SJ_FIXTURE_NAME "sj_fixture_v1"

/* Fixture cells that are solid stone (id 1), relative to the player cell P.
 * The chest sits at P+(1,0,0); the cell above it, P+(1,1,0), must stay open:
 * BlockChest.blockActivated refuses to open a chest with a normal cube above
 * it (BlockChest.java:161). The enclosure around that cell keeps drops in
 * reach. See MANIFEST.md. */
struct sj_cell { int8_t x, y, z; };
static const struct sj_cell sj_fixture_stone[] = {
	{0,-1,0}, {0,-1,1}, {1,0,1}, {-1,0,1}, {0,0,2}, {0,0,-1},
	{-1,1,0}, {1,1,1}, {-1,1,1}, {0,1,2}, {0,1,-1},
	{2,1,0}, {1,1,-1}, {1,2,0}, {0,2,0}, {0,2,1}};
static const struct sj_cell sj_fixture_chest = {1, 0, 0};

/* True when no fixture stone sits directly above the chest. */
static inline bool sj_fixture_chest_openable(void) {
	for(unsigned i = 0; i < sizeof(sj_fixture_stone) / sizeof(sj_fixture_stone[0]); i++) {
		const struct sj_cell* c = &sj_fixture_stone[i];
		if(c->x == sj_fixture_chest.x && c->y == sj_fixture_chest.y + 1
		   && c->z == sj_fixture_chest.z) return false;
	}
	return true;
}

enum sj_screen { SJ_SCR_OTHER, SJ_SCR_INGAME, SJ_SCR_INVENTORY, SJ_SCR_CHEST,
	SJ_SCR_FURNACE };
enum sj_target { SJ_TGT_NONE, SJ_TGT_LOG, SJ_TGT_CHEST, SJ_TGT_FURNACE };
/* Abstract controls; the adapter maps each to existing input hooks. */
enum sj_act { SJ_ACT_NONE, SJ_ACT_MINE, SJ_ACT_OPEN_INVENTORY, SJ_ACT_USE,
	SJ_ACT_CLOSE, SJ_ACT_CRAFT_PLANKS, SJ_ACT_STORE, SJ_ACT_TAKE,
	SJ_ACT_LOAD_INPUT, SJ_ACT_LOAD_FUEL, SJ_ACT_TAKE_OUTPUT };
enum sj_kind { SJ_K_FIXTURE, SJ_K_HOLD, SJ_K_OPEN, SJ_K_ACT, SJ_K_WORK,
	SJ_K_CLOSE, SJ_K_END };
enum sj_event { SJ_EV_NONE, SJ_EV_MINED, SJ_EV_CRAFTED, SJ_EV_CHEST_ROUNDTRIP,
	SJ_EV_FURNACE_RETRIEVED, SJ_EV_DONE };
enum sj_fail { SJ_FAIL_NONE, SJ_FAIL_TIMEOUT, SJ_FAIL_REJECTED,
	SJ_FAIL_UNCOMMITTED, SJ_FAIL_MISSING_SCREEN, SJ_FAIL_MISSING_FIXTURE,
	SJ_FAIL_NO_PROGRESS, SJ_FAIL_SCREEN_LOST, SJ_FAIL_NOT_COLLECTED };

/* What the adapter must observe for a phase: `have` = the player's committed
 * main+hotbar count of item:meta; `stored` = the open container's committed
 * count of the same item in `slot` (-1 = summed over the container). Slot 1
 * (furnace fuel) reports fuel count + 1 while the furnace burns, because an
 * idle furnace with smeltable input eats the fuel item on its next tick
 * (TileEntityFurnace.java:114-121), so the bare slot count would drop back. */
struct sj_spec {
	const char* name;
	enum sj_kind kind;
	enum sj_act act;
	enum sj_screen screen;
	enum sj_target target;
	uint16_t item, meta;
	int8_t slot, d_have, d_stored;
	uint32_t budget_ms;
	enum sj_event event;
};

#define SJ_NO_PROGRESS_MS 4000u
/* After the mined block is gone the drop must reach the inventory within this
 * window. Beta gives a block drop a 10-tick (500 ms) pickup delay
 * (Block.java:344 dropBlockAsItem_do: delayBeforeCanPickup = 10; checked at
 * EntityItem.java:113), so 2000 ms is 4x that delay. The mine button is
 * released as soon as the block is gone: holding attack would start digging
 * whatever is behind it. */
#define SJ_PICKUP_MS 2000u

/* Item ids: Beta 1.7.3 (BlockLog.java:15-16 idDropped -> wood 17, CraftingManager.java:47
 * 1 wood -> 4 planks 5, FurnaceRecipes.java:21 cobblestone 4 -> stone 1,
 * TileEntityFurnace.java:180 coal 263 fuels 1600 ticks; cook = 200 ticks,
 * TileEntityFurnace.java:126-128). Furnace slots: 0 input, 1 fuel, 2 output. */
static const struct sj_spec sj_specs[] = {
	{"fixture", SJ_K_FIXTURE, SJ_ACT_NONE, SJ_SCR_INGAME, SJ_TGT_NONE, 0, 0, -1, 0, 0, 30000u, SJ_EV_NONE},
	{"mine", SJ_K_HOLD, SJ_ACT_MINE, SJ_SCR_INGAME, SJ_TGT_LOG, 17, 0, -1, 1, 0, 15000u, SJ_EV_MINED},
	{"inv_open", SJ_K_OPEN, SJ_ACT_OPEN_INVENTORY, SJ_SCR_INVENTORY, SJ_TGT_NONE, 0, 0, -1, 0, 0, 3000u, SJ_EV_NONE},
	{"craft", SJ_K_ACT, SJ_ACT_CRAFT_PLANKS, SJ_SCR_INVENTORY, SJ_TGT_NONE, 5, 0, -1, 4, 0, 5000u, SJ_EV_CRAFTED},
	{"inv_close", SJ_K_CLOSE, SJ_ACT_CLOSE, SJ_SCR_INGAME, SJ_TGT_NONE, 0, 0, -1, 0, 0, 3000u, SJ_EV_NONE},
	{"chest_open", SJ_K_OPEN, SJ_ACT_USE, SJ_SCR_CHEST, SJ_TGT_CHEST, 0, 0, -1, 0, 0, 3000u, SJ_EV_NONE},
	{"chest_store", SJ_K_ACT, SJ_ACT_STORE, SJ_SCR_CHEST, SJ_TGT_CHEST, 5, 0, -1, -4, 4, 5000u, SJ_EV_NONE},
	{"chest_take", SJ_K_ACT, SJ_ACT_TAKE, SJ_SCR_CHEST, SJ_TGT_CHEST, 5, 0, -1, 4, -4, 5000u, SJ_EV_CHEST_ROUNDTRIP},
	{"chest_close", SJ_K_CLOSE, SJ_ACT_CLOSE, SJ_SCR_INGAME, SJ_TGT_NONE, 0, 0, -1, 0, 0, 3000u, SJ_EV_NONE},
	{"furnace_open", SJ_K_OPEN, SJ_ACT_USE, SJ_SCR_FURNACE, SJ_TGT_FURNACE, 0, 0, -1, 0, 0, 3000u, SJ_EV_NONE},
	{"furnace_input", SJ_K_ACT, SJ_ACT_LOAD_INPUT, SJ_SCR_FURNACE, SJ_TGT_FURNACE, 4, 0, 0, -1, 1, 5000u, SJ_EV_NONE},
	{"furnace_fuel", SJ_K_ACT, SJ_ACT_LOAD_FUEL, SJ_SCR_FURNACE, SJ_TGT_FURNACE, 263, 0, 1, -1, 1, 5000u, SJ_EV_NONE},
	{"furnace_cook", SJ_K_WORK, SJ_ACT_NONE, SJ_SCR_FURNACE, SJ_TGT_FURNACE, 1, 0, 2, 0, 1, 30000u, SJ_EV_NONE},
	{"furnace_take", SJ_K_ACT, SJ_ACT_TAKE_OUTPUT, SJ_SCR_FURNACE, SJ_TGT_FURNACE, 1, 0, 2, 1, -1, 5000u, SJ_EV_FURNACE_RETRIEVED},
	{"furnace_close", SJ_K_CLOSE, SJ_ACT_CLOSE, SJ_SCR_INGAME, SJ_TGT_NONE, 0, 0, -1, 0, 0, 3000u, SJ_EV_NONE},
	{"done", SJ_K_END, SJ_ACT_NONE, SJ_SCR_OTHER, SJ_TGT_NONE, 0, 0, -1, 0, 0, 0u, SJ_EV_DONE},
};
#define SJ_PHASE_COUNT ((unsigned)(sizeof(sj_specs) / sizeof(sj_specs[0])))
#define SJ_PHASE_DONE (SJ_PHASE_COUNT - 1u)

struct sj_obs {
	uint32_t ms;
	bool fixture_ok;      /* the labelled fixture's identity matched */
	enum sj_screen screen;
	bool target_present;  /* mine: the fixture block is still at its cell */
	uint32_t progress;    /* mine: dig progress; cook: furnace cook time */
	uint16_t have, stored; /* per the current phase's sj_spec */
	bool pending;         /* optimistic window actions outstanding */
	uint32_t accepted, rejected; /* cumulative server transaction replies */
};
struct sj_out {
	enum sj_act act;
	bool edge;            /* one-frame press; false + MINE = held level */
	enum sj_target target;
	enum sj_event event;
	enum sj_fail fail;
	char reason[48];
};
struct sj_state {
	unsigned phase;
	bool entered, submitted, edge_prev, failed, gone;
	uint32_t gone_ms, phase_ms, progress_ms, base_progress, base_acc, base_rej;
	uint16_t base_have, base_stored;
};

static inline const char* sj_fail_name(enum sj_fail f) {
	static const char* const n[] = {"none", "timeout", "rejected",
		"uncommitted", "missing_screen", "missing_fixture", "no_progress",
		"screen_lost", "not_collected"};
	return (unsigned)f < sizeof(n) / sizeof(n[0]) ? n[f] : "unknown";
}

static inline void sj_fail(struct sj_state* s, struct sj_out* out,
						   enum sj_fail f) {
	s->failed = true; out->fail = f; out->act = SJ_ACT_NONE; out->edge = false;
	snprintf(out->reason, sizeof(out->reason), "%s:%s", sj_fail_name(f),
			 sj_specs[s->phase].name);
}

static inline bool sj_moved(int16_t d, uint16_t base, uint16_t now) {
	return d == 0 ? now == base : (int32_t)now - (int32_t)base == d;
}

static inline void sj_tick(struct sj_state* s, const struct sj_obs* o,
						   struct sj_out* out) {
	*out = (struct sj_out){0};
	if(s->failed || s->phase >= SJ_PHASE_DONE) { s->edge_prev = false; return; }
	const struct sj_spec* sp = &sj_specs[s->phase];
	out->target = sp->target;
	if(!s->entered) {
		s->entered = true; s->submitted = false; s->gone = false;
		s->phase_ms = s->progress_ms = o->ms;
		s->base_progress = o->progress; s->base_have = o->have;
		s->base_stored = o->stored; s->base_acc = o->accepted;
		s->base_rej = o->rejected;
	}
	uint32_t age = o->ms - s->phase_ms;
	bool advance = false;
	if(o->progress != s->base_progress) {
		s->base_progress = o->progress; s->progress_ms = o->ms;
	}
	switch(sp->kind) {
	case SJ_K_FIXTURE:
		if(o->fixture_ok && o->screen == SJ_SCR_INGAME) advance = true;
		else if(age >= sp->budget_ms) sj_fail(s, out, SJ_FAIL_MISSING_FIXTURE);
		break;
	case SJ_K_HOLD:
		if(o->screen != SJ_SCR_INGAME) { sj_fail(s, out, SJ_FAIL_SCREEN_LOST); break; }
		if(!o->target_present) {
			/* Broken: wait (button released) for the drop to be collected. */
			if(!s->gone) { s->gone = true; s->gone_ms = o->ms; }
			if(sj_moved(sp->d_have, s->base_have, o->have)) advance = true;
			else if(o->ms - s->gone_ms >= SJ_PICKUP_MS)
				sj_fail(s, out, SJ_FAIL_NOT_COLLECTED);
			break;
		}
		if(age >= sp->budget_ms) sj_fail(s, out, SJ_FAIL_TIMEOUT);
		else if(o->ms - s->progress_ms >= SJ_NO_PROGRESS_MS)
			sj_fail(s, out, SJ_FAIL_NO_PROGRESS);
		else out->act = sp->act;
		break;
	case SJ_K_OPEN:
	case SJ_K_CLOSE:
		/* The destination must be reached BY our press: already being there
		 * before it means something else moved the screen. */
		if(o->screen == sp->screen) {
			if(s->submitted) advance = true;
			else sj_fail(s, out, SJ_FAIL_SCREEN_LOST);
		} else if(s->submitted && age >= sp->budget_ms)
			sj_fail(s, out, sp->kind == SJ_K_OPEN ? SJ_FAIL_MISSING_SCREEN
												  : SJ_FAIL_TIMEOUT);
		if(!s->submitted && !s->failed && !s->edge_prev) {
			out->act = sp->act; out->edge = true; s->submitted = true;
			s->phase_ms = o->ms;
		}
		break;
	case SJ_K_ACT:
	case SJ_K_WORK: {
		if(o->screen != sp->screen) { sj_fail(s, out, SJ_FAIL_SCREEN_LOST); break; }
		if(o->rejected != s->base_rej) { sj_fail(s, out, SJ_FAIL_REJECTED); break; }
		if(sp->kind == SJ_K_ACT && !s->submitted) {
			if(!s->edge_prev) {
				out->act = sp->act; out->edge = true; s->submitted = true;
				s->phase_ms = o->ms;
			}
			break;
		}
		bool replied = sp->kind == SJ_K_WORK || o->accepted != s->base_acc;
		bool moved = sj_moved(sp->d_have, s->base_have, o->have)
			&& sj_moved(sp->d_stored, s->base_stored, o->stored);
		if(replied && !o->pending && moved) { advance = true; break; }
		if(age >= sp->budget_ms)
			sj_fail(s, out, sp->kind == SJ_K_ACT && replied
								? SJ_FAIL_UNCOMMITTED : SJ_FAIL_TIMEOUT);
		else if(sp->kind == SJ_K_WORK && o->ms - s->progress_ms >= SJ_NO_PROGRESS_MS)
			sj_fail(s, out, SJ_FAIL_NO_PROGRESS);
		break;
	}
	default: break;
	}
	if(advance) {
		out->event = sp->event;
		s->phase++; s->entered = false;
		if(s->phase == SJ_PHASE_DONE) out->event = sp->event ? sp->event : SJ_EV_DONE;
	}
	s->edge_prev = out->edge;
}

static inline bool sj_done(const struct sj_state* s) {
	return !s->failed && s->phase == SJ_PHASE_DONE;
}


/* ===== REL-11b: second half - fight, genuine death, Respawn, portal round
 * trip, Save-and-quit + reopen, then compare committed state. Pure like the
 * first half; the live adapter is unwritten and unproven.
 * Beta rules this judges (lines checked against src_original/net/minecraft/src):
 *  - death: health <= 0 -> deathTime climbs and the entity dies after 20 ticks
 *    (EntityLiving.java:155-157); the Respawn button calls respawnPlayer()
 *    (GuiGameOver.java:23-25 -> EntityPlayerSP.java:165-166 -> Minecraft.respawn).
 *  - portal: dwell grows 0.0125 per tick, travel fires at >= 1.0, i.e. the 81st float add (~4 s)
 *    (EntityPlayerSP.java:53-59).
 *  - save: level.dat + player written by SaveHandler.saveWorldInfoAndPlayer
 *    (SaveHandler.java:109); furnace BurnTime/CookTime persist as shorts
 *    (TileEntityFurnace.java:62-63); player Pos persists as doubles
 *    (Entity.java:751).
 * Expected-death tagging: a death is expected ONLY inside the SJB_DEATH phase
 * and only once. Any other death (other phase, or a second one) fails by name
 * as accidental_death, so this step can never waive deaths elsewhere. `deaths`
 * must be cumulative for the whole run (it must not reset on world reload). */
enum sjb_phase { SJB_FIGHT, SJB_DEATH, SJB_RESPAWN, SJB_PORTAL_OUT,
	SJB_PORTAL_BACK, SJB_SAVE_QUIT, SJB_REOPEN, SJB_VERIFY, SJB_DONE };
enum sjb_screen { SJB_SCR_OTHER, SJB_SCR_PLAY, SJB_SCR_DEATH, SJB_SCR_TITLE };
enum sjb_act { SJB_ACT_NONE, SJB_ACT_ATTACK, SJB_ACT_RESPAWN, SJB_ACT_WALK_PORTAL,
	SJB_ACT_SAVE_QUIT, SJB_ACT_LOAD_WORLD };
enum sjb_fail { SJB_FAIL_NONE, SJB_FAIL_TIMEOUT, SJB_FAIL_SCREEN_LOST,
	SJB_FAIL_ACCIDENTAL_DEATH, SJB_FAIL_WRONG_WORLD, SJB_FAIL_RELOAD,
	SJB_FAIL_MISMATCH };
/* Authoritative committed state compared across Save-and-quit/reopen. */
struct sjb_snap {
	uint16_t inv, chest, furn_in, furn_fuel, furn_out, furn_cook, furn_burn;
	int8_t dim;
	float x, y, z;
};
struct sjb_obs {
	uint32_t ms;
	enum sjb_screen screen;
	int8_t dim;           /* 0 Overworld, -1 Nether */
	uint32_t world_id;    /* world identity; must never change in a run */
	uint32_t hits, deaths; /* cumulative: our hits on the mob / player deaths */
	int16_t hp;
	bool saved, loaded, load_error; /* level.dat written / world reopened */
	struct sjb_snap snap;
};
struct sjb_out {
	enum sjb_act act;
	bool edge;
	bool done;
	enum sjb_fail fail;
	char reason[48];
};
struct sjb_state {
	unsigned phase;
	bool init, entered, submitted, edge_prev, failed, died;
	uint32_t phase_ms, base_hits, deaths_seen, world_id;
	struct sjb_snap pre;
};
static const char* const sjb_names[] = {"fight", "death", "respawn",
	"portal_out", "portal_back", "save_quit", "reopen", "verify", "done"};
/* Portal budget 20 s covers the ~4 s dwell (81 float adds of 0.0125F) plus walking. */
static const uint32_t sjb_budget_ms[] = {20000u, 30000u, 5000u, 20000u, 20000u,
	15000u, 30000u, 3000u, 0u};

static inline const char* sjb_snap_diff(const struct sjb_snap* a,
										const struct sjb_snap* b) {
	if(a->inv != b->inv) return "item_mismatch:inventory";
	if(a->chest != b->chest) return "item_mismatch:chest";
	if(a->furn_in != b->furn_in) return "item_mismatch:furnace_input";
	if(a->furn_fuel != b->furn_fuel) return "item_mismatch:furnace_fuel";
	if(a->furn_out != b->furn_out) return "item_mismatch:furnace_output";
	if(a->furn_cook != b->furn_cook) return "item_mismatch:furnace_cook";
	if(a->furn_burn != b->furn_burn) return "item_mismatch:furnace_burn";
	if(a->dim != b->dim) return "player_mismatch:dimension";
	if(a->x != b->x || a->y != b->y || a->z != b->z) return "player_mismatch:position";
	return NULL;
}

static inline void sjb_fail(struct sjb_state* s, struct sjb_out* out,
							enum sjb_fail f, const char* why) {
	static const char* const n[] = {"none", "timeout", "screen_lost",
		"accidental_death", "wrong_world", "reload_failed", "mismatch"};
	s->failed = true; out->fail = f; out->act = SJB_ACT_NONE; out->edge = false;
	if(why) snprintf(out->reason, sizeof(out->reason), "%s", why);
	else snprintf(out->reason, sizeof(out->reason), "%s:%s", n[f], sjb_names[s->phase]);
}

/* One edge-submit helper: request `act` once, on a frame after a neutral one. */
static inline void sjb_press(struct sjb_state* s, const struct sjb_obs* o,
							 struct sjb_out* out, enum sjb_act act) {
	if(s->edge_prev) return;
	out->act = act; out->edge = true; s->submitted = true; s->phase_ms = o->ms;
}

static inline void sjb_tick(struct sjb_state* s, const struct sjb_obs* o,
							struct sjb_out* out) {
	*out = (struct sjb_out){0};
	if(s->failed || s->phase >= SJB_DONE) { s->edge_prev = false; return; }
	if(!s->init) { s->init = true; s->deaths_seen = o->deaths; s->world_id = o->world_id; }
	if(!s->entered) {
		s->entered = true; s->submitted = false;
		s->phase_ms = o->ms; s->base_hits = o->hits;
	}
	uint32_t age = o->ms - s->phase_ms;
	bool late = age >= sjb_budget_ms[s->phase];
	if(o->deaths != s->deaths_seen) {
		bool ok = s->phase == SJB_DEATH && !s->died && o->deaths == s->deaths_seen + 1u;
		s->deaths_seen = o->deaths;
		if(!ok) { sjb_fail(s, out, SJB_FAIL_ACCIDENTAL_DEATH, NULL); return; }
		s->died = true;
	}
	if(o->screen == SJB_SCR_PLAY && o->world_id != s->world_id) {
		sjb_fail(s, out, SJB_FAIL_WRONG_WORLD, NULL); return;
	}
	bool advance = false;
	switch(s->phase) {
	case SJB_FIGHT:
		if(o->screen != SJB_SCR_PLAY) sjb_fail(s, out, SJB_FAIL_SCREEN_LOST, NULL);
		else if(o->hits > s->base_hits) advance = true;
		else if(late) sjb_fail(s, out, SJB_FAIL_TIMEOUT, NULL);
		else out->act = SJB_ACT_ATTACK;
		break;
	case SJB_DEATH: /* we never cause the death; we only wait for the real one */
		if(s->died && o->screen == SJB_SCR_DEATH) advance = true;
		else if(late) sjb_fail(s, out, SJB_FAIL_TIMEOUT, NULL);
		break;
	case SJB_RESPAWN:
		if(!s->submitted) {
			if(o->screen != SJB_SCR_DEATH) sjb_fail(s, out, SJB_FAIL_SCREEN_LOST, NULL);
			else sjb_press(s, o, out, SJB_ACT_RESPAWN);
		} else if(o->screen == SJB_SCR_PLAY && o->hp > 0) advance = true;
		else if(late) sjb_fail(s, out, SJB_FAIL_TIMEOUT, NULL);
		break;
	case SJB_PORTAL_OUT:
	case SJB_PORTAL_BACK: {
		int8_t want = s->phase == SJB_PORTAL_OUT ? -1 : 0;
		if(o->screen != SJB_SCR_PLAY) sjb_fail(s, out, SJB_FAIL_SCREEN_LOST, NULL);
		else if(o->dim != 0 && o->dim != -1) sjb_fail(s, out, SJB_FAIL_WRONG_WORLD, NULL);
		else if(o->dim == want) advance = true;
		else if(late) sjb_fail(s, out, SJB_FAIL_TIMEOUT, NULL);
		else out->act = SJB_ACT_WALK_PORTAL;
		break;
	}
	case SJB_SAVE_QUIT:
		if(!s->submitted) {
			if(o->screen != SJB_SCR_PLAY) sjb_fail(s, out, SJB_FAIL_SCREEN_LOST, NULL);
			else if(!s->edge_prev) {
				s->pre = o->snap; /* the state that must survive the reload */
				sjb_press(s, o, out, SJB_ACT_SAVE_QUIT);
			}
		} else if(o->screen == SJB_SCR_TITLE && o->saved) advance = true;
		else if(late) sjb_fail(s, out, SJB_FAIL_TIMEOUT, NULL);
		break;
	case SJB_REOPEN:
		if(!s->submitted) {
			if(o->screen != SJB_SCR_TITLE) sjb_fail(s, out, SJB_FAIL_SCREEN_LOST, NULL);
			else sjb_press(s, o, out, SJB_ACT_LOAD_WORLD);
		} else if(o->screen == SJB_SCR_PLAY && o->loaded) advance = true;
		else if(o->load_error || late) sjb_fail(s, out, SJB_FAIL_RELOAD, NULL);
		break;
	case SJB_VERIFY: {
		const char* d = sjb_snap_diff(&s->pre, &o->snap);
		if(d) sjb_fail(s, out, SJB_FAIL_MISMATCH, d); else advance = true;
		break;
	}
	}
	if(advance) {
		s->phase++; s->entered = false;
		if(s->phase == SJB_DONE) out->done = true;
	}
	s->edge_prev = out->edge;
}

static inline bool sjb_done(const struct sjb_state* s) {
	return !s->failed && s->phase == SJB_DONE;
}

#endif
