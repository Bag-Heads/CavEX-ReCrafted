/* REL-11a: survival_journey_core.h against a reacting fake world.
 *
 * The fake applies each requested control one frame later (like the real input
 * latch), raises `pending` for the frame an action lands, and can inject one
 * fault. The test proves the four success events (mined, crafted, chest round
 * trip, furnace output retrieved) fire ONLY on accepted + settled + committed
 * deltas, and that each failure class is named: rejected, accepted-without-
 * effect (uncommitted), no reply (timeout), missing screen, missing fixture,
 * no progress, screen lost, mined drop not collected. Mining is modelled the
 * Beta way: breaking the log spawns a drop that is collected only after its
 * 10-tick pickup delay (Block.java:344), never on the break frame. Pure: no engine source is linked. The live adapter
 * that fills sj_obs from real state is REL-11-adapter-a and is unproven here. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/survival_journey_core.h"

enum fault { F_NONE, F_REJECT_CRAFT, F_STORE_NO_EFFECT, F_TAKE_NO_REPLY,
	F_CHEST_NEVER_OPENS, F_NO_FIXTURE, F_DIG_STUCK, F_COOK_STUCK,
	F_BLOCK_GONE_NO_DROP, F_CHEST_CLOSED_EXTERNALLY, F_PENDING_STUCK,
	F_INV_ALREADY_OPEN, F_SLOW_PICKUP, F_DROP_STRANDED };

struct fw {
	enum fault fault;
	enum sj_screen scr;
	bool log_present, pending, pending_stuck, drop_live;
	unsigned dig, cook, burn, drop_age;
	uint16_t logs, planks, cobble, coal, stone, chest, fin, ffuel, fout;
	uint32_t acc, rej;
};

static uint16_t* fw_count(struct fw* w, uint16_t item) {
	switch(item) {
	case 17: return &w->logs;
	case 5: return &w->planks;
	case 4: return &w->cobble;
	case 263: return &w->coal;
	case 1: return &w->stone;
	default: return NULL;
	}
}

static void fw_apply(struct fw* w, const struct sj_out* out) {
	w->pending = w->pending_stuck;
	/* One frame = one 50 ms tick. A live drop is collected once its pickup
	 * delay has run out (10 ticks, Block.java:344; 30 for the slow case);
	 * a stranded drop (outside the player's expand(1,0,1) pickup box,
	 * EntityPlayer.java:197) never is. */
	if(w->drop_live && w->fault != F_DROP_STRANDED
	   && ++w->drop_age >= (w->fault == F_SLOW_PICKUP ? 30u : 10u)) {
		w->drop_live = false; w->logs++;
	}
	if(out->act == SJ_ACT_MINE && w->log_present) {
		if(w->fault != F_DIG_STUCK) w->dig++;
		if(w->dig >= 60u) {
			w->log_present = false; w->dig = 0;
			if(w->fault != F_BLOCK_GONE_NO_DROP) { w->drop_live = true; w->drop_age = 0; }
		}
	}
	if(out->edge) {
		switch(out->act) {
		case SJ_ACT_OPEN_INVENTORY: w->scr = SJ_SCR_INVENTORY; break;
		case SJ_ACT_USE:
			if(out->target == SJ_TGT_CHEST && w->fault != F_CHEST_NEVER_OPENS)
				w->scr = SJ_SCR_CHEST;
			if(out->target == SJ_TGT_FURNACE) w->scr = SJ_SCR_FURNACE;
			break;
		case SJ_ACT_CLOSE: w->scr = SJ_SCR_INGAME; break;
		case SJ_ACT_CRAFT_PLANKS:
			if(w->fault == F_REJECT_CRAFT) { w->rej++; break; }
			w->logs--; w->planks += 4; w->acc++; w->pending = true; break;
		case SJ_ACT_STORE:
			w->acc++; w->pending = true;
			if(w->fault == F_PENDING_STUCK) w->pending_stuck = true;
			if(w->fault != F_STORE_NO_EFFECT) { w->planks -= 4; w->chest += 4; }
			if(w->fault == F_CHEST_CLOSED_EXTERNALLY) w->scr = SJ_SCR_INGAME;
			break;
		case SJ_ACT_TAKE:
			if(w->fault == F_TAKE_NO_REPLY) break;
			w->planks += 4; w->chest -= 4; w->acc++; w->pending = true; break;
		case SJ_ACT_LOAD_INPUT:
			w->cobble--; w->fin++; w->acc++; w->pending = true; break;
		case SJ_ACT_LOAD_FUEL:
			w->coal--; w->ffuel++; w->acc++; w->pending = true; break;
		case SJ_ACT_TAKE_OUTPUT:
			w->fout--; w->stone++; w->acc++; w->pending = true; break;
		default: break;
		}
	}
	/* Beta TileEntityFurnace.updateEntity: an idle furnace with smeltable
	 * input consumes one fuel item at once (coal = 1600 ticks), then cooks
	 * one step per tick, 200 steps per item. */
	if(w->burn) w->burn--;
	if(!w->burn && w->fin && w->ffuel) { w->ffuel--; w->burn = 1600u; }
	if(w->burn && w->fin && w->fault != F_COOK_STUCK && ++w->cook >= 200u) {
		w->cook = 0; w->fin--; w->fout++;
	}
}

static void fw_obs(struct fw* w, const struct sj_state* s, uint32_t ms,
				   struct sj_obs* o) {
	const struct sj_spec* sp = &sj_specs[s->phase < SJ_PHASE_COUNT ? s->phase : SJ_PHASE_DONE];
	uint16_t* have = fw_count(w, sp->item);
	*o = (struct sj_obs){.ms = ms, .fixture_ok = w->fault != F_NO_FIXTURE,
		.screen = w->scr, .target_present = w->log_present,
		.pending = w->pending, .accepted = w->acc, .rejected = w->rej};
	o->have = have ? *have : 0;
	if(sp->screen == SJ_SCR_CHEST) o->stored = w->chest;
	if(sp->screen == SJ_SCR_FURNACE)
		o->stored = sp->slot == 0 ? w->fin : sp->slot == 1 ? (uint16_t)(w->ffuel + (w->burn ? 1 : 0))
				  : sp->slot == 2 ? w->fout : 0;
	o->progress = s->phase == 1u ? w->dig : sp->kind == SJ_K_WORK ? w->cook : 0;
}

struct run {
	enum sj_event events[8];
	unsigned n_events, edges[SJ_PHASE_COUNT];
	enum sj_fail fail;
	char reason[48];
	uint32_t break_ms, mined_ms;
	unsigned mine_after_break;
	struct sj_state s;
	struct fw w;
};

static void run(enum fault fault, struct run* r) {
	memset(r, 0, sizeof(*r));
	r->w = (struct fw){.fault = fault, .scr = SJ_SCR_INGAME, .log_present = true,
		.cobble = 8, .coal = 1};
	struct sj_out out = {0};
	bool prev_edge = false;
	for(uint32_t ms = 1000; ms < 400000u; ms += 50u) {
		fw_apply(&r->w, &out);
		if(fault == F_INV_ALREADY_OPEN && r->s.phase == 2u) r->w.scr = SJ_SCR_INVENTORY;
		struct sj_obs o;
		unsigned phase = r->s.phase;
		fw_obs(&r->w, &r->s, ms, &o);
		sj_tick(&r->s, &o, &out);
		if(phase == 1u && !o.target_present) {
			if(!r->break_ms) r->break_ms = ms;
			if(out.act == SJ_ACT_MINE) r->mine_after_break++;
		}
		if(out.event == SJ_EV_MINED) r->mined_ms = ms;
		assert(!(prev_edge && out.edge)); /* a neutral frame between edges */
		prev_edge = out.edge;
		if(out.edge) r->edges[phase]++;
		if(out.event) { assert(r->n_events < 8u); r->events[r->n_events++] = out.event; }
		if(out.fail) {
			assert(r->s.failed && out.act == SJ_ACT_NONE && !out.edge);
			r->fail = out.fail;
			snprintf(r->reason, sizeof(r->reason), "%s", out.reason);
			return;
		}
		if(sj_done(&r->s)) return;
	}
	assert(!"journey never finished");
}

static bool has_event(const struct run* r, enum sj_event e) {
	for(unsigned i = 0; i < r->n_events; i++) if(r->events[i] == e) return true;
	return false;
}

static void expect_fail(enum fault fault, enum sj_fail want, const char* reason,
						enum sj_event absent) {
	struct run r;
	run(fault, &r);
	assert(!sj_done(&r.s));
	if(r.fail != want || strcmp(r.reason, reason) != 0) {
		fprintf(stderr, "fault %d: got %s want %s\n", (int)fault, r.reason, reason);
		assert(0);
	}
	assert(!has_event(&r, absent));
	/* A finished driver stays inert. */
	struct sj_obs o = {.ms = 999999u};
	struct sj_out out;
	sj_tick(&r.s, &o, &out);
	assert(out.act == SJ_ACT_NONE && !out.event && !out.fail);
}

/* ===== REL-11b: second half (fight, death, respawn, portal, save/reload) ===== */
enum bf { B_NONE, B_NO_HIT, B_EARLY_DEATH, B_NO_DEATH, B_NO_RESPAWN, B_PORTAL_DEATH,
	B_SECOND_DEATH, B_STUCK_NETHER, B_BACK_WRONG_DIM, B_BACK_WRONG_WORLD,
	B_NO_SAVE, B_RELOAD_FAIL, B_ITEM_LOSS, B_COOK_LOST, B_POS_DRIFT };
struct bw {
	enum bf f;
	enum sjb_screen scr;
	int8_t dim;
	uint32_t world_id, hits, deaths, dwell, frames, trips, kill_at;
	int16_t hp;
	bool saved, loaded, load_error;
	struct sjb_snap snap;
};
static void bw_apply(struct bw* w, const struct sjb_out* out) {
	w->frames++;
	if(w->f == B_EARLY_DEATH && w->frames == 3u) { w->deaths++; w->scr = SJB_SCR_DEATH; w->hp = 0; }
	if(out->act == SJB_ACT_ATTACK && w->scr == SJB_SCR_PLAY && w->f != B_NO_HIT && w->f != B_EARLY_DEATH) {
		if(++w->hits == 1u) w->kill_at = w->frames + 20u; /* mob keeps hitting us */
	}
	if(w->kill_at && w->frames == w->kill_at && w->f != B_NO_DEATH && w->f != B_EARLY_DEATH) {
		w->deaths += w->f == B_SECOND_DEATH ? 2u : 1u; w->hp = 0; w->scr = SJB_SCR_DEATH;
	}
	w->dwell = out->act == SJB_ACT_WALK_PORTAL ? w->dwell + 1u : 0u;
	if(w->dwell >= 81u) { /* EntityPlayerSP.java:53-59: 81 float adds of 0.0125F */
		w->dwell = 0;
		if(++w->trips == 1u) w->dim = -1;
		else if(w->f == B_PORTAL_DEATH) { w->deaths++; w->hp = 0; w->scr = SJB_SCR_DEATH; }
		else if(w->f == B_BACK_WRONG_DIM) w->dim = 1;
		else if(w->f != B_STUCK_NETHER) { w->dim = 0; if(w->f == B_BACK_WRONG_WORLD) w->world_id++; }
	}
	if(!out->edge) return;
	switch(out->act) {
	case SJB_ACT_RESPAWN: if(w->f != B_NO_RESPAWN) { w->scr = SJB_SCR_PLAY; w->hp = 20; } break;
	case SJB_ACT_SAVE_QUIT: w->scr = SJB_SCR_TITLE; w->saved = w->f != B_NO_SAVE; break;
	case SJB_ACT_LOAD_WORLD:
		if(w->f == B_RELOAD_FAIL) { w->load_error = true; break; }
		w->scr = SJB_SCR_PLAY; w->loaded = true;
		if(w->f == B_ITEM_LOSS) w->snap.inv--;
		if(w->f == B_COOK_LOST) w->snap.furn_cook = 0;
		if(w->f == B_POS_DRIFT) w->snap.x += 0.5f;
		break;
	default: break;
	}
}
static bool bw_run(enum bf f, struct sjb_state* s, struct sjb_out* last, unsigned edges[]) {
	struct bw w = {.f = f, .scr = SJB_SCR_PLAY, .hp = 20, .world_id = 7,
		.snap = {30, 4, 1, 0, 0, 57, 1000, 0, 10.5f, 64.0f, -3.25f}};
	memset(s, 0, sizeof(*s));
	struct sjb_out out = {0};
	bool pe = false;
	for(uint32_t ms = 1000; ms < 600000u; ms += 50u) {
		bw_apply(&w, &out);
		w.snap.dim = w.dim;
		struct sjb_obs o = {.ms = ms, .screen = w.scr, .dim = w.dim, .world_id = w.world_id,
			.hits = w.hits, .deaths = w.deaths, .hp = w.hp, .saved = w.saved,
			.loaded = w.loaded, .load_error = w.load_error, .snap = w.snap};
		unsigned ph = s->phase;
		sjb_tick(s, &o, &out);
		assert(!(pe && out.edge)); /* a neutral frame between edges */
		pe = out.edge;
		if(out.edge) edges[ph]++;
		if(out.fail) { *last = out; return false; }
		if(sjb_done(s)) { *last = out; return true; }
	}
	assert(!"second half never finished");
	return false;
}
static void bexpect(enum bf f, const char* reason) {
	struct sjb_state s; struct sjb_out out; unsigned e[SJB_DONE + 1] = {0};
	bool ok = bw_run(f, &s, &out, e);
	if(ok || strcmp(out.reason, reason)) {
		fprintf(stderr, "fault %d: got '%s' want '%s'\n", (int)f, out.reason, reason);
		assert(0);
	}
	assert(s.failed && !sjb_done(&s));
	struct sjb_obs o = {.ms = 9999999u}; struct sjb_out z;
	sjb_tick(&s, &o, &z); /* a failed driver stays inert */
	assert(z.act == SJB_ACT_NONE && !z.fail && !z.done);
}
static void test_second_half(void) {
	struct sjb_state s; struct sjb_out out; unsigned e[SJB_DONE + 1] = {0};
	assert(bw_run(B_NONE, &s, &out, e) && out.done);
	/* exactly one edge each for respawn, save_quit, reopen; none elsewhere */
	for(unsigned i = 0; i <= SJB_DONE; i++)
		assert(e[i] == (i == SJB_RESPAWN || i == SJB_SAVE_QUIT || i == SJB_REOPEN));
	bexpect(B_NO_HIT, "timeout:fight");
	bexpect(B_NO_DEATH, "timeout:death");
	bexpect(B_NO_RESPAWN, "timeout:respawn");
	bexpect(B_STUCK_NETHER, "timeout:portal_back");
	bexpect(B_NO_SAVE, "timeout:save_quit");
	/* intentional-death tag must not waive any other death */
	bexpect(B_EARLY_DEATH, "accidental_death:fight");
	bexpect(B_PORTAL_DEATH, "accidental_death:portal_back");
	bexpect(B_SECOND_DEATH, "accidental_death:death");
	/* wrong world / wrong dimension on the way back */
	bexpect(B_BACK_WRONG_DIM, "wrong_world:portal_back");
	bexpect(B_BACK_WRONG_WORLD, "wrong_world:portal_back");
	bexpect(B_RELOAD_FAIL, "reload_failed:reopen");
	bexpect(B_ITEM_LOSS, "item_mismatch:inventory");
	bexpect(B_COOK_LOST, "item_mismatch:furnace_cook");
	bexpect(B_POS_DRIFT, "player_mismatch:position");
}

int main(void) {
	/* Fixture validity: the chest must be openable (no normal cube above it,
	 * BlockChest.java:161), and the validator must catch the 10-18 defect. */
	assert(sj_fixture_chest_openable());
	for(unsigned i = 0; i < sizeof(sj_fixture_stone) / sizeof(sj_fixture_stone[0]); i++)
		assert(!(sj_fixture_stone[i].x == 1 && sj_fixture_stone[i].y == 1 && sj_fixture_stone[i].z == 0));
	/* The table: names unique, the four success events in journey order. */
	static const enum sj_event order[] = {SJ_EV_MINED, SJ_EV_CRAFTED,
		SJ_EV_CHEST_ROUNDTRIP, SJ_EV_FURNACE_RETRIEVED};
	unsigned k = 0;
	for(unsigned i = 0; i < SJ_PHASE_COUNT; i++) {
		for(unsigned j = 0; j < i; j++) assert(strcmp(sj_specs[i].name, sj_specs[j].name));
		if(sj_specs[i].event && sj_specs[i].event != SJ_EV_DONE)
			assert(k < 4u && sj_specs[i].event == order[k++]);
	}
	assert(k == 4u && sj_specs[SJ_PHASE_DONE].kind == SJ_K_END);

	/* Happy path: all four events, then DONE, with real committed results. */
	struct run r;
	run(F_NONE, &r);
	assert(sj_done(&r.s) && r.fail == SJ_FAIL_NONE);
	assert(r.n_events == 5u);
	for(unsigned i = 0; i < 4u; i++) assert(r.events[i] == order[i]);
	assert(r.events[4] == SJ_EV_DONE);
	assert(r.w.logs == 0 && r.w.planks == 4 && r.w.chest == 0);
	assert(r.w.cobble == 7 && r.w.coal == 0 && r.w.stone == 1 && r.w.fout == 0);
	/* MINED waits for the drop's 10-tick pickup delay, not the break, and the
	 * dig button is released the moment the block is gone. */
	assert(r.break_ms && r.mined_ms >= r.break_ms + 10u * 50u);
	assert(r.mine_after_break == 0 && !r.w.drop_live);
	/* Delayed collection (30 ticks, inside SJ_PICKUP_MS) still succeeds, only
	 * on the collection frame. */
	{
		struct run sl;
		run(F_SLOW_PICKUP, &sl);
		assert(sj_done(&sl.s) && sl.fail == SJ_FAIL_NONE);
		assert(sl.events[0] == SJ_EV_MINED);
		assert(sl.mined_ms >= sl.break_ms + 30u * 50u);
		assert(sl.mined_ms < sl.break_ms + SJ_PICKUP_MS);
		assert(sl.mine_after_break == 0);
	}
	/* Exactly one edge per OPEN/ACT/CLOSE phase; none while mining/cooking. */
	for(unsigned i = 0; i < SJ_PHASE_COUNT; i++) {
		enum sj_kind kd = sj_specs[i].kind;
		bool one = kd == SJ_K_OPEN || kd == SJ_K_ACT || kd == SJ_K_CLOSE;
		assert(r.edges[i] == (one ? 1u : 0u));
	}

	/* Submitted but rejected by the server: not a craft. */
	expect_fail(F_REJECT_CRAFT, SJ_FAIL_REJECTED, "rejected:craft", SJ_EV_CRAFTED);
	/* Accepted reply, committed state unchanged: not a store. */
	expect_fail(F_STORE_NO_EFFECT, SJ_FAIL_UNCOMMITTED, "uncommitted:chest_store",
				SJ_EV_CHEST_ROUNDTRIP);
	/* Accepted with the delta but never settled (pending): not a store. */
	expect_fail(F_PENDING_STUCK, SJ_FAIL_UNCOMMITTED, "uncommitted:chest_store",
				SJ_EV_CHEST_ROUNDTRIP);
	/* Submitted, no reply at all: a plain timeout, distinct from the above. */
	expect_fail(F_TAKE_NO_REPLY, SJ_FAIL_TIMEOUT, "timeout:chest_take",
				SJ_EV_CHEST_ROUNDTRIP);
	/* The press went out but the container screen never appeared. */
	expect_fail(F_CHEST_NEVER_OPENS, SJ_FAIL_MISSING_SCREEN,
				"missing_screen:chest_open", SJ_EV_CHEST_ROUNDTRIP);
	/* A screen reached without our press is not our result. */
	expect_fail(F_INV_ALREADY_OPEN, SJ_FAIL_SCREEN_LOST, "screen_lost:inv_open",
				SJ_EV_CRAFTED);
	/* The container closed under the action. */
	expect_fail(F_CHEST_CLOSED_EXTERNALLY, SJ_FAIL_SCREEN_LOST,
				"screen_lost:chest_store", SJ_EV_CHEST_ROUNDTRIP);
	/* No labelled fixture: nothing is pressed at all. */
	{
		struct run nf;
		run(F_NO_FIXTURE, &nf);
		assert(nf.fail == SJ_FAIL_MISSING_FIXTURE);
		assert(!strcmp(nf.reason, "missing_fixture:fixture"));
		for(unsigned i = 0; i < SJ_PHASE_COUNT; i++) assert(nf.edges[i] == 0);
		assert(nf.w.dig == 0 && nf.n_events == 0);
	}
	/* Holding the dig button without dig progress. */
	expect_fail(F_DIG_STUCK, SJ_FAIL_NO_PROGRESS, "no_progress:mine", SJ_EV_MINED);
	/* Block gone but no drop committed to the inventory: not mined. */
	expect_fail(F_BLOCK_GONE_NO_DROP, SJ_FAIL_NOT_COLLECTED, "not_collected:mine",
				SJ_EV_MINED);
	/* Drop spawned but stranded out of pickup range: not mined, and the
	 * driver never walks or re-digs to fetch it. */
	{
		struct run st;
		run(F_DROP_STRANDED, &st);
		assert(st.fail == SJ_FAIL_NOT_COLLECTED);
		assert(!strcmp(st.reason, "not_collected:mine"));
		assert(!has_event(&st, SJ_EV_MINED) && st.w.drop_live && st.w.logs == 0);
		assert(st.mine_after_break == 0);
	}
	/* Loaded and fuelled furnace that never cooks. */
	expect_fail(F_COOK_STUCK, SJ_FAIL_NO_PROGRESS, "no_progress:furnace_cook",
				SJ_EV_FURNACE_RETRIEVED);

	test_second_half();

	printf("test_survival_journey_core: PASS\n");
	return 0;
}
