#ifndef CHUNK_LOD_GEOMIP_H
#define CHUNK_LOD_GEOMIP_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/*
	T2 SPIKE (opt-in, PC-measured only; hardware-unconfirmed -- see
	docs/far_ring_geomip_design_2026-08-26.md for the full design, the
	measurement plan, and every simplification called out below).

	Geomipmapping error-metric core (de Boer 2000) adapted to the far-ring skin's
	per-column heightmap. Allocation-free static inline predicates, same pattern
	as chunk_lod_policy.h: the tiny decisions live here so the deterministic
	regression fixture (tests/test_lod_geomip_policy.c) can exercise the exact
	math the worker uses without linking the threaded mesher.

	A "patch" is a fixed 2x2 group of adjacent skin columns. One corner (the
	"anchor", the group's lowest-x,lowest-z column) supplies BOTH the reference
	height used below AND the geometry actually emitted for the whole patch when
	merged -- this keeps the merged quad's anchor corner exactly watertight
	against the UNCHANGED per-column silhouette skirt pass (chunk_mesher.c's
	lod_emit_skirts), which keys off that same column's real height and is not
	touched by this spike. The error metric therefore measures deviation from
	the anchor height (h_ref), not the group average: h_ref is what is actually
	drawn, so the bound is on the true rendered deviation, not a proxy that could
	understate it.
*/

/* Max abs deviation (in blocks) of the other three corners from the anchor
 * (h_ref). Callers must not merge if any input is the "no surface" sentinel
 * (-1); this function assumes all four are valid, real column heights -- see
 * lod_geomip_patch_mergeable, which checks that before calling this. */
static inline float lod_geomip_patch_error(int h_ref, int h1, int h2, int h3) {
	float e = 0.0F, d;
	d = fabsf((float)(h1 - h_ref));
	if(d > e)
		e = d;
	d = fabsf((float)(h2 - h_ref));
	if(d > e)
		e = d;
	d = fabsf((float)(h3 - h_ref));
	if(d > e)
		e = d;
	return e;
}

/* The perspective-projection constant (de Boer S2.2 / Lindstrom et al. 1996):
 * screen_px = (error_blocks / distance_blocks) * k,  k = screen_h / (2*tan(fov/2)).
 * Depends only on FOV + screen height, so callers compute it ONCE per session
 * (whenever those change) rather than once per patch -- see
 * world_lod_geomip_projection_k() in world.c. fov_deg <= 0 or >= 180 is
 * defensive (never true for this project's FOV values) and returns 0, which
 * makes every screen error 0 px -- i.e. every patch passes the budget. Callers
 * should never rely on that corner case; it exists so this function never
 * divides by zero or calls tanf on an out-of-domain angle. */
static inline float lod_geomip_projection_k(float fov_deg, float screen_height_px) {
	if(fov_deg <= 0.0F || fov_deg >= 180.0F || screen_height_px <= 0.0F)
		return 0.0F;
	float fov_rad = fov_deg * (float)(3.14159265358979323846 / 180.0);
	float t = tanf(fov_rad * 0.5F);
	return t > 0.0F ? screen_height_px / (2.0F * t) : 0.0F;
}

/* Project a geometric error (blocks) at `distance_blocks` to screen-space pixels
 * using a precomputed `k` (see lod_geomip_projection_k). distance_blocks <= 0 is
 * defensive (camera exactly on/behind the patch) -- returns a huge error so the
 * caller always falls back to the fine level instead of dividing by zero. */
static inline float lod_geomip_screen_error_px(float error_blocks,
												float distance_blocks, float k) {
	if(distance_blocks <= 0.0F)
		return 1.0e9F;
	return (error_blocks / distance_blocks) * k;
}

/* Full merge decision for one 2x2 patch. `enabled` is the opt-in flag
 * ("lod.geomip", default off) threaded all the way down from config -- when
 * false this ALWAYS returns false, for ANY other input, full stop. That single
 * property is what makes the flag-off render path byte-identical to before this
 * spike: test_lod_geomip_policy.c's test_disabled_gate_is_byte_path_equivalent
 * exhaustively proves it the same way test_lod_tree_policy.c's
 * test_lod0_gate_is_byte_path_equivalent proves chunk_lod_face_allowed's LOD0
 * gate.
 *
 * When enabled, mergeable iff: every corner has a real surface (h_ref/h1/h2/h3
 * all >= 0 -- the -1 sentinel means "no skin surface in that column", never
 * mergeable); all four corners share the exact same top block id (a material
 * boundary -- e.g. grass meeting stone -- always keeps its own quads, never
 * blended); and the projected screen-space error of collapsing the patch to the
 * anchor's height stays within `error_budget_px`.
 *
 * Biome-tint uniformity (grass/foliage colour index) and per-block metadata
 * (snow layers, crop growth stage, log orientation, ...) are NOT checked here
 * -- that data isn't visible to this header (id is the bare block type byte).
 * Callers with tinted or metadata-sensitive terrain MUST add those as an extra
 * precondition before accepting a merge (chunk_mesher.c's
 * lod_geomip_compute_plan does exactly that). */
static inline bool lod_geomip_patch_mergeable(bool enabled, int h_ref, int h1,
											   int h2, int h3, uint8_t id_ref,
											   uint8_t id1, uint8_t id2,
											   uint8_t id3, float distance_blocks,
											   float error_budget_px, float k) {
	if(!enabled)
		return false;
	if(h_ref < 0 || h1 < 0 || h2 < 0 || h3 < 0)
		return false;
	if(id_ref != id1 || id_ref != id2 || id_ref != id3)
		return false;
	float e = lod_geomip_patch_error(h_ref, h1, h2, h3);
	float px = lod_geomip_screen_error_px(e, distance_blocks, k);
	return px <= error_budget_px;
}

#endif
