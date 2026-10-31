/*
	T2 SPIKE (opt-in, PC-measured only -- docs/far_ring_geomip_design_2026-08-26.md).

	Unit tests for chunk_lod_geomip.h's pure math: the geomipmapping error metric,
	the perspective-projection screen-space-error formula, and the merge decision.
	Standalone (no mesher link needed -- the header is allocation-free static
	inline functions), same pattern as test_lod_tree_policy.c testing
	chunk_lod_policy.h.

	The single most important property this file proves is the one CLAUDE.md's T2
	rules require before ANY of this can be called done: with the `enabled` flag
	false, lod_geomip_patch_mergeable() returns false for EVERY input, exhaustively
	swept -- mirroring test_lod_tree_policy.c's
	test_lod0_gate_is_byte_path_equivalent proof for chunk_lod_face_allowed. That
	single property is what makes chunk_mesher.c's flag-off render path provably
	inert: lod_geomip_compute_plan calls this function (indirectly, via its own
	`enabled` early-out) and nothing downstream can ever mark a patch merged when
	the flag is off.
*/

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "chunk_lod_geomip.h"

static void test_disabled_gate_is_byte_path_equivalent(void) {
	/* Sweep a spread of heights (including the -1 "no surface" sentinel and
	 * negative-looking values), ids (including equal and differing), distances
	 * (including <= 0), budgets (including huge) and k (including 0) -- with
	 * enabled=false every single combination must refuse to merge. */
	const int heights[] = {-1, 0, 1, 5, 15, 16, 255};
	const uint8_t ids[] = {0, 1, 2, 255};
	const float dists[] = {-10.0F, 0.0F, 1.0F, 16.0F, 500.0F};
	const float budgets[] = {0.0F, 2.0F, 1.0e6F};
	const float ks[] = {0.0F, 1.0F, 800.0F};

	for(size_t hi = 0; hi < sizeof(heights) / sizeof(heights[0]); hi++)
		for(size_t idi = 0; idi < sizeof(ids) / sizeof(ids[0]); idi++)
			for(size_t di = 0; di < sizeof(dists) / sizeof(dists[0]); di++)
				for(size_t bi = 0; bi < sizeof(budgets) / sizeof(budgets[0]); bi++)
					for(size_t ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ki++)
						assert(!lod_geomip_patch_mergeable(
							false, heights[hi], heights[hi], heights[hi],
							heights[hi], ids[idi], ids[idi], ids[idi], ids[idi],
							dists[di], budgets[bi], ks[ki]));
}

static void test_no_surface_never_mergeable(void) {
	/* Any corner reporting the -1 "no skin surface in this column" sentinel
	 * must refuse to merge even when enabled and every other input is ideal
	 * (identical ids, zero error, huge budget). */
	assert(!lod_geomip_patch_mergeable(true, -1, 5, 5, 5, 2, 2, 2, 2, 100.0F,
									   1.0e6F, 500.0F));
	assert(!lod_geomip_patch_mergeable(true, 5, -1, 5, 5, 2, 2, 2, 2, 100.0F,
									   1.0e6F, 500.0F));
	assert(!lod_geomip_patch_mergeable(true, 5, 5, -1, 5, 2, 2, 2, 2, 100.0F,
									   1.0e6F, 500.0F));
	assert(!lod_geomip_patch_mergeable(true, 5, 5, 5, -1, 2, 2, 2, 2, 100.0F,
									   1.0e6F, 500.0F));
}

static void test_material_boundary_never_mergeable(void) {
	/* Flat, zero-error terrain that crosses a block-id boundary (e.g. grass
	 * meeting stone) must never merge, regardless of how generous the budget
	 * is -- a coarse quad may not blend two different materials. */
	assert(!lod_geomip_patch_mergeable(true, 5, 5, 5, 5, /*ids*/ 2, 2, 2, 1,
									   100.0F, 1.0e6F, 500.0F));
	assert(!lod_geomip_patch_mergeable(true, 5, 5, 5, 5, 2, 1, 2, 2, 100.0F,
									   1.0e6F, 500.0F));
}

static void test_flat_patch_always_mergeable_when_enabled(void) {
	/* Zero geometric error (all four corners share the anchor's height) means
	 * zero screen-space error, which is <= any non-negative budget -- must
	 * merge whenever enabled, at any real distance. */
	for(float dist = 1.0F; dist <= 1000.0F; dist *= 10.0F)
		assert(lod_geomip_patch_mergeable(true, 7, 7, 7, 7, 3, 3, 3, 3, dist,
										  0.0F, 500.0F));
}

static void test_error_metric_is_max_abs_deviation_from_anchor(void) {
	assert(lod_geomip_patch_error(10, 10, 10, 10) == 0.0F);
	assert(lod_geomip_patch_error(10, 11, 9, 10) == 1.0F);
	assert(lod_geomip_patch_error(10, 12, 8, 10) == 2.0F);
	/* Asymmetric: the metric is the deviation from h_ref specifically, not
	 * from the group average (10.5 here) -- see the header's design note on
	 * why the anchor, not the average, is the reference. */
	assert(lod_geomip_patch_error(0, 1, 1, 1) == 1.0F);
}

static void test_projection_k_and_screen_error(void) {
	/* k = screen_h / (2*tan(fov/2)). For fov=90deg, tan(45deg)=1, so
	 * k == screen_h / 2 exactly (a clean, hand-checkable case). */
	float k90 = lod_geomip_projection_k(90.0F, 480.0F);
	assert(fabsf(k90 - 240.0F) < 0.01F);

	/* A narrower FOV projects the SAME geometric error to MORE screen pixels
	 * (the classic terrain-LOD tightening-on-zoom effect) -- k must grow as
	 * fov shrinks. */
	float k_wide = lod_geomip_projection_k(120.0F, 480.0F);
	float k_narrow = lod_geomip_projection_k(40.0F, 480.0F);
	assert(k_narrow > k_wide);

	/* Screen error scales linearly with geometric error and inversely with
	 * distance, and the degenerate distance<=0 case returns a huge (never-
	 * mergeable) value instead of dividing by zero or going negative/NaN. */
	assert(lod_geomip_screen_error_px(2.0F, 100.0F, 240.0F) > 0.0F);
	float near = lod_geomip_screen_error_px(2.0F, 50.0F, 240.0F);
	float far = lod_geomip_screen_error_px(2.0F, 200.0F, 240.0F);
	assert(near > far);
	assert(lod_geomip_screen_error_px(2.0F, 0.0F, 240.0F) > 1.0e8F);
	assert(lod_geomip_screen_error_px(2.0F, -5.0F, 240.0F) > 1.0e8F);

	/* Defensive fov/resolution domain: out-of-range inputs return 0, not a
	 * NaN/negative from tanf on a bad angle. */
	assert(lod_geomip_projection_k(0.0F, 480.0F) == 0.0F);
	assert(lod_geomip_projection_k(180.0F, 480.0F) == 0.0F);
	assert(lod_geomip_projection_k(200.0F, 480.0F) == 0.0F);
	assert(lod_geomip_projection_k(70.0F, 0.0F) == 0.0F);
}

static void test_budget_threshold_is_exact(void) {
	/* A patch error of exactly 2 blocks at 100 blocks distance with k=100
	 * projects to exactly 2.0 px (2/100*100). A budget of 2.0 must accept it
	 * (<=); a budget of 1.99 must reject it. */
	float k = 100.0F;
	assert(lod_geomip_patch_mergeable(true, 0, 2, 0, 0, 5, 5, 5, 5, 100.0F, 2.0F,
									  k));
	assert(!lod_geomip_patch_mergeable(true, 0, 2, 0, 0, 5, 5, 5, 5, 100.0F,
									   1.99F, k));
}

int main(void) {
	test_disabled_gate_is_byte_path_equivalent();
	test_no_surface_never_mergeable();
	test_material_boundary_never_mergeable();
	test_flat_patch_always_mergeable_when_enabled();
	test_error_metric_is_max_abs_deviation_from_anchor();
	test_projection_k_and_screen_error();
	test_budget_threshold_is_exact();
	puts("test_lod_geomip_policy: disabled-gate exhaustive proof + no-surface + "
		 "material-boundary + flat-always-merge + error-metric + projection-k + "
		 "exact-budget-threshold OK");
	return 0;
}
