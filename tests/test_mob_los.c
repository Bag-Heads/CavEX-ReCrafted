/*
	Pins the Beta 1.7.3 mob line-of-sight core (audit #13): the pure voxel DDA in
	source/entity/los.h that backs EntityLiving.canEntityBeSeen / World.rayTraceBlocks.
	A mob must NOT acquire / shoot / prime / swing through a solid wall, so the
	traversal has to (a) report a clear line when nothing intervenes, (b) report
	BLOCKED when an opaque cell sits strictly between the eyes, (c) EXCLUDE the start
	and end cells (a mob standing in grass or a player in a doorway is not falsely
	occluded by its own block), and (d) NOT leak through a thin diagonal wall (the
	whole reason for an incremental boundary-crossing DDA over a fixed-step sampler).

	This binds the world-free core directly with an in-test grid predicate -- no mock
	world -- mirroring tests/test_creeper_fuse.c's pure-helper style. The world wiring
	(server_world_get_block + the blocks[] can_see_through flag) lives in
	entity_monster.c::mob_los_cell_opaque and is not exercised here.

	These pin the GAMEPLAY-EQUIVALENT invariants the gate depends on (solid strictly
	between -> blocked, open line -> clear, no thin/diagonal leak, both eye-cells
	excluded), NOT Beta's exact corner tie-break cell selection (see los.h: the
	Amanatides-Woo walk may visit a different-but-coincident cell at an exact grazing
	tie without changing the blocked/clear result).

	Beta source: World.func_28105_a L702-851 (incremental boundary-crossing ray walk;
	the stop cell returns clear before its block test -- los.h additionally excludes
	the start cell, a documented deviation); EntityLiving.canEntityBeSeen L71-73.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/los.h"

// --- A tiny finite voxel grid for the predicate ------------------------------
// A list of opaque cells; is_opaque() returns true iff (x,y,z) is in the list.
struct grid {
	const int (*solid)[3];
	int count;
};

static bool grid_is_opaque(int x, int y, int z, void* ctx) {
	const struct grid* g = (const struct grid*)ctx;
	for(int i = 0; i < g->count; i++)
		if(g->solid[i][0] == x && g->solid[i][1] == y && g->solid[i][2] == z)
			return true;
	return false;
}

static bool blocked(struct grid* g, float ax, float ay, float az, float bx,
					float by, float bz) {
	return los_segment_blocked(ax, ay, az, bx, by, bz, grid_is_opaque, g);
}

// (1) Empty world: any line is CLEAR (visible). Includes axis-aligned, diagonal,
// and long lines -- none touch a solid cell because there are none.
static void test_clear_line(void) {
	struct grid g = {.solid = NULL, .count = 0};
	assert(!blocked(&g, 0.5f, 1.5f, 0.5f, 10.5f, 1.5f, 0.5f));   // +X straight
	assert(!blocked(&g, 0.5f, 1.5f, 0.5f, 0.5f, 1.5f, 10.5f));   // +Z straight
	assert(!blocked(&g, 0.5f, 1.5f, 0.5f, 10.5f, 8.5f, 10.5f));  // full diagonal
	assert(!blocked(&g, 5.5f, 2.5f, 5.5f, -4.5f, 2.5f, -4.5f));  // negative dir
}

// (2) A solid cell STRICTLY BETWEEN the eyes blocks the line. Wall at x=5; eye to
// eye crosses it -> blocked. Move the wall off the line -> clear again.
static void test_wall_between(void) {
	static const int wall[][3] = {{5, 1, 0}};
	struct grid g = {.solid = wall, .count = 1};
	// Mob at x=0.5, player at x=10.5, same y/z column -> ray passes through x=5.
	assert(blocked(&g, 0.5f, 1.5f, 0.5f, 10.5f, 1.5f, 0.5f));
	// Same endpoints but the only solid cell is one row up (y=3) -> not on the line.
	static const int off[][3] = {{5, 3, 0}};
	struct grid g2 = {.solid = off, .count = 1};
	assert(!blocked(&g2, 0.5f, 1.5f, 0.5f, 10.5f, 1.5f, 0.5f));
}

// (3) The START and END cells are EXCLUDED as blockers. A mob standing inside a
// solid (its own occupied cell) or a player standing in a solid doorway block must
// still be mutually visible if nothing lies strictly between.
static void test_endpoints_excluded(void) {
	// Solid AT the start cell (0,1,0) only -> start excluded -> clear.
	static const int at_start[][3] = {{0, 1, 0}};
	struct grid gs = {.solid = at_start, .count = 1};
	assert(!blocked(&gs, 0.5f, 1.5f, 0.5f, 6.5f, 1.5f, 0.5f));

	// Solid AT the end cell (6,1,0) only -> end excluded -> clear.
	static const int at_end[][3] = {{6, 1, 0}};
	struct grid ge = {.solid = at_end, .count = 1};
	assert(!blocked(&ge, 0.5f, 1.5f, 0.5f, 6.5f, 1.5f, 0.5f));

	// Solid at BOTH endpoints, nothing between -> still clear (both excluded).
	static const int both[][3] = {{0, 1, 0}, {6, 1, 0}};
	struct grid gb = {.solid = both, .count = 2};
	assert(!blocked(&gb, 0.5f, 1.5f, 0.5f, 6.5f, 1.5f, 0.5f));

	// Sanity: add a cell BETWEEN the endpoint-solids -> now blocked (proves the
	// endpoint exclusion didn't just disable the whole test).
	static const int both_plus[][3] = {{0, 1, 0}, {3, 1, 0}, {6, 1, 0}};
	struct grid gbp = {.solid = both_plus, .count = 3};
	assert(blocked(&gbp, 0.5f, 1.5f, 0.5f, 6.5f, 1.5f, 0.5f));
}

// (4) THE load-bearing case: a thin diagonal wall must NOT be leaked through. Two
// adjacent solid cells form an L that seals the shared face/corner the diagonal ray
// crosses; an incremental boundary-crossing DDA visits one of them (it cannot hop
// the shared face), so the line is BLOCKED. A naive fixed-step sampler would step
// over the corner and falsely report clear -- this asserts we do NOT.
static void test_diagonal_thin_wall(void) {
	// Ray from (0.5,0.5) to (2.5,2.5) on the XY plane crosses the corner shared by
	// cells (1,1) and (1,2)/(2,1). Seal both cells the diagonal grazes at the corner.
	static const int corner[][3] = {{1, 1, 0}, {1, 2, 0}, {2, 1, 0}};
	struct grid g = {.solid = corner, .count = 3};
	assert(blocked(&g, 0.5f, 0.5f, 0.5f, 2.5f, 2.5f, 0.5f));

	// A single cell forming a one-thick diagonal barrier across the path is hit too:
	// ray (0.5,0.5)->(3.5,3.5) passes through (1,1),(2,2) -> a solid (2,2) blocks.
	static const int mid[][3] = {{2, 2, 0}};
	struct grid g2 = {.solid = mid, .count = 1};
	assert(blocked(&g2, 0.5f, 0.5f, 0.5f, 3.5f, 3.5f, 0.5f));
}

// (5) Degenerate near-zero-length segment (point-blank, same cell or same column):
// finite, trivially VISIBLE, no NaN / no infinite loop.
static void test_point_blank(void) {
	static const int anywhere[][3] = {{5, 5, 5}};
	struct grid g = {.solid = anywhere, .count = 1};
	// Identical endpoints -> same cell -> clear.
	assert(!blocked(&g, 3.5f, 1.5f, 3.5f, 3.5f, 1.5f, 3.5f));
	// Sub-block separation within one cell -> still same cell -> clear.
	assert(!blocked(&g, 3.2f, 1.1f, 3.8f, 3.6f, 1.4f, 3.3f));
	// Pure vertical (same XZ column), short -> clear when nothing between.
	assert(!blocked(&g, 3.5f, 1.5f, 3.5f, 3.5f, 2.9f, 3.5f));
}

// (6) Pure vertical occlusion: a solid ceiling cell between a low mob and a high
// player blocks. Exercises the single-active-axis (dx=dz=0) traversal path.
static void test_vertical_block(void) {
	static const int ceil_[][3] = {{3, 4, 3}};
	struct grid g = {.solid = ceil_, .count = 1};
	// Mob foot-eye at y=1.5, player eye at y=7.5, same column -> crosses y=4.
	assert(blocked(&g, 3.5f, 1.5f, 3.5f, 3.5f, 7.5f, 3.5f));
	// Without the ceiling cell -> clear.
	struct grid g0 = {.solid = NULL, .count = 0};
	assert(!blocked(&g0, 3.5f, 1.5f, 3.5f, 3.5f, 7.5f, 3.5f));
}

// (7) The Beta-faithful per-block LOS-occlusion RULE (los_block_occludes), the same
// decision the server's mob_los_cell_opaque forwards its blocks[] flags to. Pins that
// a full-footprint solid the mesher flags see-through (SLAB / STAIRS / FARMLAND,
// opacity 15, place_ignore false) DOES occlude -- the slab/stair-wall through-shoot
// gap the audit closed -- while FLUIDS (lava: opacity 15 but place_ignore) and thin
// decorations (opacity 0..1, see-through) do NOT, and every non-see-through solid
// does. Also pins the one documented deviation: glass/leaves (see-through, low
// opacity) stay passable here though Beta's full AABB would block them.
static void test_block_occlusion_rule(void) {
	// (can_see_through, opacity, place_ignore) drawn from the real block registry.
	// --- BLOCKERS ---
	assert(los_block_occludes(false, 15, false)); // stone / full solid
	assert(los_block_occludes(false, 0, false));  // any non-see-through solid
	assert(los_block_occludes(true, 15, false));  // SLAB  (see-through, full footprint)
	assert(los_block_occludes(true, 15, false));  // STAIRS (same flags as slab)
	assert(los_block_occludes(true, 15, false));  // FARMLAND (tilled-soil full footprint)
	// --- PASS-THROUGH ---
	assert(!los_block_occludes(true, 15, true));  // LAVA  (fluid: opacity 15 but place_ignore)
	assert(!los_block_occludes(true, 3, true));   // WATER (fluid)
	assert(!los_block_occludes(true, 0, true));   // FIRE  (place_ignore, opacity 0)
	assert(!los_block_occludes(true, 0, false));  // TORCH / flower / wire / fence / ladder
	assert(!los_block_occludes(true, 1, false));  // COBWEB (opacity 1, thin)
	assert(!los_block_occludes(true, 0, false));  // GLASS  (documented deviation: passes)
	assert(!los_block_occludes(true, 1, false));  // LEAVES (documented deviation: passes)
}

/* CFR Beta server World.java:562-594 (Jad World.java:841-875): an
 * axis whose cell already equals the destination is disabled. A negative
 * crossing at t=1 must not leave that cell while another axis finishes.
 * Existing endpoint/opacity deviations remain the caller's contract. */
static void test_integer_endpoints(void) {
	for(int axis = 0; axis < 3; axis++) {
		for(int sign = -1; sign <= 1; sign += 2) {
			float a[3] = {0.0F, 0.0F, 0.0F};
			float b[3] = {0.0F, 0.0F, 0.0F};
			const int wall[1][3] = {{axis == 0 ? sign : 0,
				axis == 1 ? sign : 0, axis == 2 ? sign : 0}};
			a[axis] = (float)(3 * sign);
			b[axis] = 0.0F;
			struct grid g = {.solid = wall, .count = 1};
			assert(blocked(&g, a[0], a[1], a[2], b[0], b[1], b[2]));
			/* Reverse direction: integer source, axis-parallel. */
			assert(blocked(&g, b[0], b[1], b[2], a[0], a[1], a[2]));
			const int beyond_axis[1][3] = {{axis == 0 ? -2 * sign : 0,
				axis == 1 ? -2 * sign : 0, axis == 2 ? -2 * sign : 0}};
			g.solid = beyond_axis; // beyond either segment
			assert(!blocked(&g, a[0], a[1], a[2], b[0], b[1], b[2]));
			assert(!blocked(&g, b[0], b[1], b[2], a[0], a[1], a[2]));
		}
	}
	/* X wins the endpoint tie, then walks out of x=0 in the old DDA.
	 * A wall at x=-1 lies beyond the segment, and must never be visited. */
	static const int beyond[][3] = {{-1, 0, 0}};
	struct grid g = {.solid = beyond, .count = 1};
	assert(!blocked(&g, 2.5F, 0.5F, 0.5F, 0.0F, 1.0F, 0.5F));
	/* Same boundary hazard on Y, and a whole-block X/Z target. */
	static const int beyond_y[][3] = {{0, -1, 0}};
	g.solid = beyond_y;
	assert(!blocked(&g, 0.5F, 2.5F, 0.5F, 0.5F, 0.0F, 1.0F));
	g.solid = beyond;
	assert(!blocked(&g, 2.5F, 0.5F, 0.5F, 0.0F, 0.5F, 1.0F));
	/* A real intervening voxel remains a blocker. */
	static const int between[][3] = {{1, 0, 0}};
	g.solid = between;
	assert(blocked(&g, 2.5F, 0.5F, 0.5F, 0.0F, 1.0F, 0.5F));
	puts("  LOS integer endpoints: axis-parallel +/-XYZ, "
		 "boundary ties and walls OK");
}

int main(void) {
	test_integer_endpoints();
	test_clear_line();
	test_wall_between();
	test_endpoints_excluded();
	test_diagonal_thin_wall();
	test_point_blank();
	test_vertical_block();
	test_block_occlusion_rule();
	printf("test_mob_los: canEntityBeSeen DDA pinned (clear visible, wall blocks, "
		   "start/end excluded, diagonal thin-wall not leaked, point-blank ok, "
		   "slab/stair/farmland occlude + fluids/decorations pass)\n");
	return 0;
}
