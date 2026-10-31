/*
	Covers two render_block.c fixes to the block-cracks (dig progress) overlay:

	(1) render_block_cracks USED TO swap the GLOBAL blocks[type]->getTextureIndex
	    vtable slot for the duration of the crack render, then restore it. That
	    slot is also read directly by the mesher thread (chunk_mesher.c's
	    lb->getTextureIndex(&local_info, SIDE_TOP)) -- a real cross-thread data
	    race on live gameplay state, not just a style nit. The fix adds a
	    per-call `texture_override` field to struct block_info (blocks_data.h)
	    and a single choke-point helper, block_texture_index() (render_block.c),
	    that every renderBlock/renderBlockAlways texture lookup in the file now
	    goes through; render_block_cracks sets `.texture_override` on its own
	    STACK-LOCAL block_info instead of touching blocks[] at all.

	    This file cannot link the real engine (render_block_cracks pulls in
	    gstate/inventory/world/GX), so -- matching the established convention in
	    test_displaylist_vertex_equiv.c / test_vertex_light_equiv.c -- it carries
	    self-contained mirrors of the OLD (vtable-swap) and NEW (field-override)
	    dispatch logic and proves:
	      a) identical output for every (block type, side) query in a crack-
	         render sequence -- i.e. the emitted crack quads (whose only
	         per-call-varying input is the picked texture index; geometry is
	         untouched by this fix) are byte-identical to before.
	      b) the shared vtable slot is observably mutated, mid-call, by the OLD
	         approach (the precondition that made the mesher-thread race
	         possible) and is proven, exhaustively and deterministically (no
	         timing dependency), to be NEVER written by the NEW approach.

	(2) block_cracks_texture divided by `delay / 10` with no floor: any
	    tool_dig_delay_ms() < 10ms (real in this engine -- e.g. block_redstone_
	    wire.c's comment: hardness=50 -> tool_dig_delay_ms returns exactly
	    50ms, and softer/faster-harvest materials go lower) truncated the
	    divisor to 0 and divided by zero. The fix floors the divisor at 1. This
	    mirrors the exact fixed formula and checks a battery of delays,
	    including several < 10ms, with ASan/UBSan-clean integer division and no
	    behaviour change for delay >= 10 (divisor was already >= 1 there).

	If you change block_texture_index()/render_block_cracks() in render_block.c
	or the divisor floor in block_cracks_texture(), change the mirrors below.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Part 1: texture_override dispatch (mirrors struct block_info::texture_override
 * + render_block.c's block_texture_index() + render_block_cracks()).
 * ------------------------------------------------------------------------- */

enum side { SIDE_TOP = 0, SIDE_BOTTOM, SIDE_LEFT, SIDE_RIGHT, SIDE_FRONT,
			SIDE_BACK, SIDE_MAX };

struct m_block_info; /* fwd */

typedef uint8_t (*m_tex_fn)(struct m_block_info*, enum side);

/* Mirrors struct block { ...; getTextureIndex; ... } -- just the one field
   this test exercises. */
struct m_block {
	m_tex_fn getTextureIndex;
};

/* Mirrors struct block_info, specifically the new texture_override field. */
struct m_block_info {
	int type;
	m_tex_fn texture_override; /* NULL unless the crack overlay set it */
};

enum { M_BLOCK_TYPES = 3 };
static struct m_block g_blocks[M_BLOCK_TYPES];

/* Each real block type's "real" getTextureIndex: deterministic, distinct per
   (type, side) so any mix-up between types/sides is caught. */
static uint8_t real_tex_type0(struct m_block_info* i, enum side s) {
	(void)i;
	return (uint8_t)(10 + s);
}
static uint8_t real_tex_type1(struct m_block_info* i, enum side s) {
	(void)i;
	return (uint8_t)(50 + s);
}
static uint8_t real_tex_type2(struct m_block_info* i, enum side s) {
	(void)i;
	return (uint8_t)(90 + s);
}
static const m_tex_fn REAL_TEX[M_BLOCK_TYPES]
	= {real_tex_type0, real_tex_type1, real_tex_type2};

/* The crack overlay's own texture function (mirrors block_cracks_texture's
   RETURN CHANNEL only -- a fixed sentinel standing in for "whatever break-tile
   index the digging progress computed"). */
static uint8_t crack_tex(struct m_block_info* i, enum side s) {
	(void)i;
	return (uint8_t)(200 + s);
}

/* NEW dispatch: mirrors render_block.c's block_texture_index() EXACTLY. */
static uint8_t new_texture_index(struct m_block_info* info, enum side side) {
	if(info->texture_override)
		return info->texture_override(info, side);
	return g_blocks[info->type].getTextureIndex(info, side);
}

/* Records every value observed in g_blocks[type].getTextureIndex across a
   whole OLD-style crack render, so we can prove (a) it visibly changes to
   crack_tex for the bracket's duration and (b) it is restored after. */
struct probe_log {
	m_tex_fn seen[SIDE_MAX + 2];
	int n;
};
static void probe_record(struct probe_log* log, m_tex_fn v) {
	assert(log->n < (int)(sizeof(log->seen) / sizeof(log->seen[0])));
	log->seen[log->n++] = v;
}

/* OLD dispatch: mirrors the removed swap-based render_block_cracks -- save,
   overwrite the GLOBAL slot, render every side, restore -- with a probe
   sampling the shared slot at each step (this is exactly what the mesher
   thread's lb->getTextureIndex(...) reads concurrently in the real code). */
static uint8_t old_crack_render(int type, uint8_t out_tex[SIDE_MAX],
								struct probe_log* log) {
	m_tex_fn tmp = g_blocks[type].getTextureIndex;
	g_blocks[type].getTextureIndex = crack_tex; /* the removed mutation */
	probe_record(log, g_blocks[type].getTextureIndex);

	struct m_block_info info = {.type = type, .texture_override = NULL};
	for(int k = 0; k < SIDE_MAX; k++) {
		probe_record(log, g_blocks[type].getTextureIndex);
		out_tex[k] = g_blocks[type].getTextureIndex(&info, (enum side)k);
	}

	g_blocks[type].getTextureIndex = tmp; /* the removed restore */
	probe_record(log, g_blocks[type].getTextureIndex);
	return 0;
}

/* NEW dispatch: mirrors the fixed render_block_cracks -- a STACK-LOCAL
   block_info carries the override; blocks[] (g_blocks here) is read-only
   throughout. Probes the shared slot at each step exactly like the OLD path
   above, over the identical (type, side) sequence. */
static void new_crack_render(int type, uint8_t out_tex[SIDE_MAX],
							 struct probe_log* log) {
	struct m_block_info info = {.type = type, .texture_override = crack_tex};
	probe_record(log, g_blocks[type].getTextureIndex);
	for(int k = 0; k < SIDE_MAX; k++) {
		probe_record(log, g_blocks[type].getTextureIndex);
		out_tex[k] = new_texture_index(&info, (enum side)k);
	}
	probe_record(log, g_blocks[type].getTextureIndex);
}

static void test_override_dispatch(void) {
	for(int t = 0; t < M_BLOCK_TYPES; t++)
		g_blocks[t].getTextureIndex = REAL_TEX[t];

	for(int type = 0; type < M_BLOCK_TYPES; type++) {
		uint8_t old_out[SIDE_MAX], new_out[SIDE_MAX];
		struct probe_log old_log = {.n = 0}, new_log = {.n = 0};

		old_crack_render(type, old_out, &old_log);
		new_crack_render(type, new_out, &new_log);

		/* (a) Emitted crack texture sequence is byte-identical old vs new --
		   the fix does not change WHAT gets drawn, only HOW it's threaded
		   through. */
		assert(memcmp(old_out, new_out, sizeof(old_out)) == 0);
		for(int k = 0; k < SIDE_MAX; k++)
			assert(old_out[k] == (uint8_t)(200 + k)); /* == crack_tex(side) */

		/* (b) The OLD approach visibly mutates the shared slot: every probe
		   taken INSIDE the bracket (indices 0..SIDE_MAX, i.e. all but the
		   final post-restore sample) reads crack_tex -- proving the window a
		   concurrent mesher-thread read could observe the WRONG function
		   existed and was non-instantaneous. */
		assert(old_log.n == SIDE_MAX + 2);
		for(int i = 0; i <= SIDE_MAX; i++)
			assert(old_log.seen[i] == crack_tex);
		assert(old_log.seen[SIDE_MAX + 1] == REAL_TEX[type]); /* restored after */

		/* (c) The NEW approach NEVER mutates the shared slot -- exhaustively,
		   at every single sample point across the whole render, not just
		   net-zero before/after. This is the direct fix for the mesher-thread
		   race: g_blocks[] (blocks[] in the real engine) is read-only. */
		assert(new_log.n == SIDE_MAX + 2);
		for(int i = 0; i < new_log.n; i++)
			assert(new_log.seen[i] == REAL_TEX[type]);
	}

	/* (d) Normal (non-crack) rendering is untouched: texture_override == NULL
	   must fall straight through to the real per-type function, for every
	   type/side -- the ~30-call-site conversion to block_texture_index()
	   changes no output when no override is set. */
	for(int type = 0; type < M_BLOCK_TYPES; type++) {
		struct m_block_info info = {.type = type, .texture_override = NULL};
		for(int k = 0; k < SIDE_MAX; k++) {
			uint8_t got = new_texture_index(&info, (enum side)k);
			uint8_t want = g_blocks[type].getTextureIndex(&info, (enum side)k);
			assert(got == want);
		}
	}

	printf("test_block_cracks_texture_override: dispatch: crack quads "
		   "byte-identical old vs new; OLD mutates blocks[] mid-render (%d "
		   "probes, all crack_tex), NEW never does (%d probes, all real); "
		   "normal (non-crack) rendering unaffected across %d types x %d "
		   "sides\n",
		   SIDE_MAX + 1, SIDE_MAX + 2, M_BLOCK_TYPES, SIDE_MAX);
}

/* ---------------------------------------------------------------------------
 * Part 2: block_cracks_texture's divide-by-zero guard.
 * Mirrors the fixed formula exactly:
 *     int divisor = delay / 10; if(divisor < 1) divisor = 1;
 *     int dt = elapsed_ms / divisor; if(dt >= 9) dt = 9;
 * ------------------------------------------------------------------------- */

static int crack_stage(int delay_ms, int elapsed_ms) {
	int divisor = delay_ms / 10;
	if(divisor < 1)
		divisor = 1;
	int dt = elapsed_ms / divisor;
	if(dt >= 9)
		dt = 9;
	return dt;
}

static void test_divide_guard(void) {
	/* Sub-10ms delays used to truncate delay/10 to 0 -> SIGFPE (integer
	   divide by zero). Every one of these must now just compute, not crash;
	   the floor means elapsed_ms is divided by 1 (every ms is its own stage,
	   clamped at the real code's ceiling of 9 -- TEXAT_BREAK_0..+9, 10
	   stages). */
	static const int tiny_delays[] = {0, 1, 5, 9};
	for(size_t i = 0; i < sizeof(tiny_delays) / sizeof(tiny_delays[0]); i++) {
		int d = tiny_delays[i];
		assert(crack_stage(d, 0) == 0);
		assert(crack_stage(d, 3) == 3);
		assert(crack_stage(d, 8) == 8);   /* 8/1=8, clamp not yet active */
		assert(crack_stage(d, 9) == 9);   /* 9/1=9, clamp boundary (dt>=9) */
		assert(crack_stage(d, 500) == 9); /* deep into clamp */
	}

	/* delay == 10 is the boundary the bug report names: divisor was already
	   exactly 1 pre-fix (10/10==1), so the floor is a strict no-op there --
	   confirms the fix changes nothing for delay >= 10 (identical to the
	   tiny-delay divisor of 1 above, just no longer reached via truncation). */
	assert(crack_stage(10, 0) == 0);
	assert(crack_stage(10, 8) == 8);
	assert(crack_stage(10, 9) == 9);
	assert(crack_stage(10, 90) == 9); /* clamped */

	/* A representative real hardness delay well above the boundary (e.g. the
	   documented hardness=50 -> 50ms case) is completely unaffected by the
	   floor (divisor = 50/10 = 5, never hits the floor). */
	assert(crack_stage(50, 0) == 0);
	assert(crack_stage(50, 12) == 2);  /* 12/5=2 */
	assert(crack_stage(50, 44) == 8);  /* 44/5=8 (8.8 truncated), clamp inactive */
	assert(crack_stage(50, 45) == 9);  /* 45/5=9, clamp boundary */
	assert(crack_stage(50, 1000) == 9); /* clamp active */

	printf("test_block_cracks_texture_override: divide guard: delay in "
		   "{0,1,5,9,10,50} all compute without a divide-by-zero; delay>=10 "
		   "behaviour unchanged\n");
}

int main(void) {
	test_override_dispatch();
	test_divide_guard();
	printf("test_block_cracks_texture_override: all assertions passed\n");
	return 0;
}
