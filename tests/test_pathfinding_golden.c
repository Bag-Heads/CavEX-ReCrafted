/*
	FH-13: golden-first tests for the Beta 1.7.3 A* pathfinding core
	(source/entity/pathfinding.{c,h}), authored against the decompile BEFORE the
	engine wiring so the port has an independent oracle. Three layers, in
	increasing order of what they exercise:

	  1. PathPoint.func_22329_a hash values -- hand-computed bit arithmetic,
	     including negative coordinates (the sign-bit special case).
	  2. The Path binary min-heap (sortBack/sortForward) -- hand-traced push/
	     pop/change-distance sequences, INCLUDING a tie (two equal keys), so the
	     exact array shape (not just "pops in sorted order") is pinned. A
	     logically-correct-but-differently-shaped heap would diverge on the tie.
	  3. Pathfinder.createEntityPathTo end to end on tiny synthetic worlds (a
	     world-free predicate, mirroring tests/test_mob_los.c's style) --
	     straight corridor, an L-bend forced by solid walls, a 1-block gap that
	     must be routed AROUND (Beta's getSafePoint 4-fall-cap rejects stepping
	     into an unbounded pit), a 2-high ceiling constraint (dim_y respected),
	     and a fully sealed pocket (no path -- Beta's null).

	Every synthetic world below is deliberately built as a FORCED 1-wide tunnel
	(solid everywhere except the one intended route) so the expected waypoint
	list is unambiguous by construction -- no tie-break judgement calls are
	needed to hand-derive the expected path (the heap tie-break behaviour is
	separately, directly pinned in section 2).

	Beta source: 740c583, tmp/decompile_Path{finder,Entity,Point}.java,
	tmp/decompile_Path.java (see source/entity/pathfinding.h for the per-function
	citations).
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "entity/pathfinding.h"

// ================================================================================
// 1. PathPoint hash -- hand-computed, see the derivation in the FH-13 lane notes.
//    hash = (y&255) | ((x&32767)<<8) | ((z&32767)<<24) | (x<0?MIN_VALUE:0) |
//           (z<0?0x8000:0), all in 32-bit (Java int-overflow-wrapping) arithmetic.
// ================================================================================
static void test_hash_values(void) {
	// (0,0,0): every term zero.
	assert(path_point_hash(0, 0, 0) == 0);

	// (1,2,3): all-positive, bit-disjoint fields (y in bits0-7, x in bits8-22,
	// z in bits24-25) -- y=2, x-term=1<<8=256, z-term=3<<24=50331648.
	assert(path_point_hash(1, 2, 3) == 50331906);

	// (-1,0,0): x negative. x&32767=32767, <<8 = 0x7FFF00 = 8388352; plus the
	// negative-x sign bit 0x80000000 (2147483648) -- disjoint (bit31 vs bits8-22)
	// so OR==ADD here. Sum as uint32 = 2155872000 = 0x807FFF00; reinterpreted as
	// signed int32 (subtract 2^32) = -2139095296.
	assert(path_point_hash(-1, 0, 0) == -2139095296);

	// (0,0,-1): z negative. (z&32767)<<24, truncated to 32 bits, keeps only the
	// LOW 8 BITS of the masked z shifted to the top byte (bits 8-14 of the
	// pre-shift value are pushed past bit31 and lost) -- i.e. equals
	// (z&0xFF)<<24. For z=-1, z&0xFF=0xFF -> 0xFF000000 = 4278190080. Plus the
	// negative-z sign bit 0x8000=32768 (disjoint: bits24-31 vs bit15, OR==ADD).
	// Sum = 4278222848 = 0xFF008000; as signed int32 = -16744448.
	assert(path_point_hash(0, 0, -1) == -16744448);
}

// ================================================================================
// 2. Path heap: hand-traced sortBack/sortForward sequences.
// ================================================================================

static struct path_point make_point(int32_t x, float d) {
	struct path_point p;
	path_point_init(&p, x, 0, 0); // x used only as a distinguishing id here
	p.distance_to_target = d;
	return p;
}

// Push A(5), B(2), C(7): a small tie-free trace covering ordinary sortBack
// bubbling, then push a FOURTH point D(1) that must bubble past BOTH B and A to
// reach the root, exercising a two-level sortBack walk. Hand trace (0-indexed
// array, parent(i) = (i-1)>>1):
//   push A(5): [A]
//   push B(2): parent(1)=A(5); 2<5 -> swap up -> [B,A]
//   push C(7): parent(2)=B(2); 7>=2 -> no swap -> [B,A,C]
//   push D(1): parent(3)=A(5); 1<5 -> swap, i=1; parent(1)=B(2); 1<2 -> swap,
//              i=0 -> [D,B,C,A]
static void test_heap_push_trace(void) {
	struct path_point a = make_point(1, 5.0f);
	struct path_point b = make_point(2, 2.0f);
	struct path_point c = make_point(3, 7.0f);
	struct path_point d = make_point(4, 1.0f);

	struct path_heap h;
	path_heap_clear(&h);
	assert(path_heap_add_point(&h, &a));
	assert(h.count == 1 && h.items[0] == &a);

	assert(path_heap_add_point(&h, &b));
	assert(h.count == 2 && h.items[0] == &b && h.items[1] == &a);

	assert(path_heap_add_point(&h, &c));
	assert(h.count == 3 && h.items[0] == &b && h.items[1] == &a
		   && h.items[2] == &c);

	assert(path_heap_add_point(&h, &d));
	assert(h.count == 4 && h.items[0] == &d && h.items[1] == &b
		   && h.items[2] == &c && h.items[3] == &a);

	// heap_index mirrors the array position for every resident point.
	assert(d.heap_index == 0 && b.heap_index == 1 && c.heap_index == 2
		   && a.heap_index == 3);
}

// Push P1(2), P2(2) [TIE with P1], P3(1). Hand trace:
//   push P1(2): [P1]
//   push P2(2): parent(1)=P1(2); 2>=2 (tie) -> BREAK, no swap -> [P1,P2]
//   push P3(1): parent(2)=P1(2); 1<2 -> swap, i=0 -> [P3,P2,P1]
// Then dequeue all three and check the exact pop order: P3, P1, P2 -- note P1
// pops BEFORE P2 despite the tie, which only follows from the exact array shape
// above (P1 ends at index2, P2 stays at index1; dequeue1 moves index2's
// occupant (P1) into the vacated root and it survives sortForward because its
// only child slot is empty at that point) -- not from insertion order in
// general (see the change-distance trace below, where a tie resolves the OTHER
// way after a mutation).
static void test_heap_tie_break_trace(void) {
	struct path_point p1 = make_point(1, 2.0f);
	struct path_point p2 = make_point(2, 2.0f);
	struct path_point p3 = make_point(3, 1.0f);

	struct path_heap h;
	path_heap_clear(&h);
	path_heap_add_point(&h, &p1);
	path_heap_add_point(&h, &p2);
	assert(h.items[0] == &p1 && h.items[1] == &p2);
	path_heap_add_point(&h, &p3);
	assert(h.count == 3 && h.items[0] == &p3 && h.items[1] == &p2
		   && h.items[2] == &p1);

	struct path_point* order[3];
	order[0] = path_heap_dequeue(&h);
	order[1] = path_heap_dequeue(&h);
	order[2] = path_heap_dequeue(&h);
	assert(path_heap_is_empty(&h));
	assert(order[0] == &p3 && order[1] == &p1 && order[2] == &p2);
	// Every dequeued point is marked unassigned again.
	assert(p1.heap_index < 0 && p2.heap_index < 0 && p3.heap_index < 0);
}

// changeDistance: push A(5), B(2), C(7) [C never bubbles, parent B(2)<=7], then
// DECREASE A from 5 to 1 (should sortBack A past B to the root). Hand trace:
//   after pushes: [B(2), A(5), C(7)]
//   change A: 5->1 (decrease) -> sortBack(A.heap_index=1): parent(1)=B(2);
//             1<2 -> swap, i=0 -> [A(1), B(2), C(7)]
// Then dequeue all three: expect A, B, C (ascending, no tie this time).
static void test_heap_change_distance_trace(void) {
	struct path_point a = make_point(1, 5.0f);
	struct path_point b = make_point(2, 2.0f);
	struct path_point c = make_point(3, 7.0f);

	struct path_heap h;
	path_heap_clear(&h);
	path_heap_add_point(&h, &a);
	path_heap_add_point(&h, &b);
	path_heap_add_point(&h, &c);
	assert(h.items[0] == &b && h.items[1] == &a && h.items[2] == &c);

	path_heap_change_distance(&h, &a, 1.0f);
	assert(h.items[0] == &a && h.items[1] == &b && h.items[2] == &c);
	assert(a.distance_to_target == 1.0f);

	assert(path_heap_dequeue(&h) == &a);
	assert(path_heap_dequeue(&h) == &b);
	assert(path_heap_dequeue(&h) == &c);
	assert(path_heap_is_empty(&h));
}

// ================================================================================
// 3. Pathfinder.createEntityPathTo end to end on tiny synthetic worlds.
// ================================================================================

// Standard biped test mob (matches the engine's zombie-ish box_w=0.6/box_h=1.8):
// dim_x = floor(0.6+1.0) = 1, dim_y = floor(1.8+1.0) = 2, dim_z = 1.
#define TEST_MOB_WIDTH 0.6f
#define TEST_MOB_HEIGHT 1.8f

static bool in_xz_list(int32_t x, int32_t z, const int (*list)[2], size_t n) {
	for(size_t i = 0; i < n; i++)
		if(list[i][0] == x && list[i][1] == z)
			return true;
	return false;
}

static void assert_path_equals(const struct path_entity* p, const int32_t (*expect)[3],
								int32_t n) {
	assert(p->length == n);
	for(int32_t i = 0; i < n; i++) {
		assert(p->points[i].x == expect[i][0]);
		assert(p->points[i].y == expect[i][1]);
		assert(p->points[i].z == expect[i][2]);
	}
}

// --- 3a. Straight corridor: no obstacles, floor at y=0, open air y=1,2. ------
static enum pathfinder_cell_kind corridor_query(int32_t x, int32_t y, int32_t z,
												void* ctx) {
	(void)ctx;
	(void)x;
	(void)z;
	if(y == 0)
		return PATH_CELL_SOLID; // floor
	return PATH_CELL_PASSABLE; // open sky everywhere else
}

static void test_search_straight_corridor(void) {
	struct pathfinder pf;
	struct path_entity path;
	struct pathfinder_stats stats = {0};

	bool ok = pathfinder_create_path_to_xyz(&pf, corridor_query, NULL, 0.5f, 1.0f,
											0.5f, TEST_MOB_WIDTH, TEST_MOB_HEIGHT, 5,
											1, 0, 16.0f, &path, &stats);
	assert(ok);
	static const int32_t expect[][3]
		= {{0, 1, 0}, {1, 1, 0}, {2, 1, 0}, {3, 1, 0}, {4, 1, 0}, {5, 1, 0}};
	assert_path_equals(&path, expect, 6);
	assert(stats.nodes_opened >= 6); // at least the 6 waypoints were minted
}

// --- 3b. L-bend forced around a solid wall (a true 1-wide tunnel: everything
// off the intended route is solid, so the route is unambiguous by construction).
// ------------------------------------------------------------------------------
static const int L_CORRIDOR[][2]
	= {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, 3}, {2, 3}, {3, 3}};

static enum pathfinder_cell_kind lbend_query(int32_t x, int32_t y, int32_t z,
											 void* ctx) {
	(void)ctx;
	if(y == 0)
		return PATH_CELL_SOLID; // floor
	if(y == 1 || y == 2)
		return in_xz_list(x, z, L_CORRIDOR, 7) ? PATH_CELL_PASSABLE
												: PATH_CELL_SOLID;
	return PATH_CELL_SOLID; // ceiling: keeps the tunnel a true 1-tall-pair
}

static void test_search_l_bend(void) {
	struct pathfinder pf;
	struct path_entity path;

	bool ok = pathfinder_create_path_to_xyz(&pf, lbend_query, NULL, 0.5f, 1.0f,
											0.5f, TEST_MOB_WIDTH, TEST_MOB_HEIGHT, 3,
											1, 3, 16.0f, &path, NULL);
	assert(ok);
	static const int32_t expect[][3] = {{0, 1, 0}, {0, 1, 1}, {0, 1, 2}, {0, 1, 3},
										{1, 1, 3}, {2, 1, 3}, {3, 1, 3}};
	assert_path_equals(&path, expect, 7);
}

// --- 3c. 1-block gap: a bottomless pit at (2,*,0) rejects the direct route
// (Pathfinder.getSafePoint's 4-fall cap), forcing the detour via z=1. Floor
// moved to y=10 (away from Beta's own y>0 loop-bottom special case at y=0 -- see
// the FH-13 lane notes) so the fall-cap, not the world-bottom edge case, is what
// rejects the candidate.
// ------------------------------------------------------------------------------
static const int GAP_CORRIDOR[][2]
	= {{0, 0}, {1, 0}, {1, 1}, {2, 1}, {3, 1}, {3, 0}, {4, 0}, {5, 0}};

static enum pathfinder_cell_kind gap_query(int32_t x, int32_t y, int32_t z,
										   void* ctx) {
	(void)ctx;
	if(x == 2 && z == 0) {
		// The pit column: no floor anywhere in the probed range (open air from
		// well below the corridor's feet level up through its head level), so
		// the fall-through descent never finds ground within the 3-block cap.
		return (y >= -5 && y <= 12) ? PATH_CELL_PASSABLE : PATH_CELL_SOLID;
	}
	if(y == 10)
		return PATH_CELL_SOLID; // floor
	if(y == 11 || y == 12)
		return in_xz_list(x, z, GAP_CORRIDOR, 8) ? PATH_CELL_PASSABLE
												  : PATH_CELL_SOLID;
	return PATH_CELL_SOLID;
}

static void test_search_gap_routes_around(void) {
	struct pathfinder pf;
	struct path_entity path;

	bool ok = pathfinder_create_path_to_xyz(&pf, gap_query, NULL, 0.5f, 11.0f,
											0.5f, TEST_MOB_WIDTH, TEST_MOB_HEIGHT, 5,
											11, 0, 16.0f, &path, NULL);
	assert(ok);
	static const int32_t expect[][3] = {{0, 11, 0}, {1, 11, 0}, {1, 11, 1},
										{2, 11, 1}, {3, 11, 1}, {3, 11, 0},
										{4, 11, 0}, {5, 11, 0}};
	assert_path_equals(&path, expect, 8);
	// The pit column itself must never appear as a waypoint.
	for(int32_t i = 0; i < path.length; i++)
		assert(!(path.points[i].x == 2 && path.points[i].z == 0));
}

// --- 3d. 2-high ceiling constraint: a pinch cell where y=11 is open but y=12
// (the biped's head cell, dim_y=2) is solid rejects the direct route entirely
// (getVerticalOffset scans BOTH cells and returns 0 on the first solid hit),
// forcing the detour via a fully 2-high side passage.
// ------------------------------------------------------------------------------
static const int CEIL_CORRIDOR[][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 2}, {1, 3},
										{0, 3}};

static enum pathfinder_cell_kind ceiling_query(int32_t x, int32_t y, int32_t z,
												void* ctx) {
	(void)ctx;
	if(y == 10)
		return PATH_CELL_SOLID; // floor
	if(x == 0 && z == 2) {
		// The low-ceiling pinch: standable at y=11, but the head cell y=12 is
		// solid rock -- a dim_y=2 mob cannot occupy this column at all.
		return (y == 11) ? PATH_CELL_PASSABLE : PATH_CELL_SOLID;
	}
	if(y == 11 || y == 12)
		return in_xz_list(x, z, CEIL_CORRIDOR, 6) ? PATH_CELL_PASSABLE
												   : PATH_CELL_SOLID;
	return PATH_CELL_SOLID;
}

static void test_search_ceiling_constraint(void) {
	struct pathfinder pf;
	struct path_entity path;

	bool ok = pathfinder_create_path_to_xyz(&pf, ceiling_query, NULL, 0.5f, 11.0f,
											0.5f, TEST_MOB_WIDTH, TEST_MOB_HEIGHT, 0,
											11, 3, 16.0f, &path, NULL);
	assert(ok);
	static const int32_t expect[][3]
		= {{0, 11, 0}, {0, 11, 1}, {1, 11, 1}, {1, 11, 2}, {1, 11, 3}, {0, 11, 3}};
	assert_path_equals(&path, expect, 6);
	// The pinch cell (0,11,2) -- passable at foot level but capped by the low
	// ceiling -- must never appear as a waypoint.
	for(int32_t i = 0; i < path.length; i++)
		assert(!(path.points[i].x == 0 && path.points[i].z == 2));
}

// --- 3e. Fully sealed pocket: no path at all (Beta's null). -------------------
static enum pathfinder_cell_kind sealed_query(int32_t x, int32_t y, int32_t z,
											  void* ctx) {
	(void)ctx;
	if(y == 10)
		return PATH_CELL_SOLID; // floor under the pocket too (can't fall out)
	if(x == 0 && z == 0 && (y == 11 || y == 12))
		return PATH_CELL_PASSABLE; // the sealed 1x2x1 pocket itself
	return PATH_CELL_SOLID; // every neighbour in every direction: sealed
}

static void test_search_unreachable_returns_no_path(void) {
	struct pathfinder pf;
	struct path_entity path;
	path.length = 99; // poison -- must be cleared to 0 on failure

	bool ok = pathfinder_create_path_to_xyz(&pf, sealed_query, NULL, 0.5f, 11.0f,
											0.5f, TEST_MOB_WIDTH, TEST_MOB_HEIGHT,
											10, 11, 0, 16.0f, &path, NULL);
	assert(!ok);
	assert(path.length == 0);
}

// --- PathEntity waypoint cursor -------------------------------------------------
static void test_path_entity_cursor(void) {
	struct path_entity p;
	path_entity_clear(&p);
	p.points[0] = (struct path_waypoint) {0, 1, 0};
	p.points[1] = (struct path_waypoint) {1, 1, 0};
	p.length = 2;

	assert(!path_entity_is_finished(&p));
	float x, y, z;
	// width=0.6 -> (int)(0.6+1.0F)=1 -> offset 0.5 (ordinary block-centre aim).
	path_entity_get_position(&p, 0.6f, &x, &y, &z);
	assert(x == 0.5f && y == 1.0f && z == 0.5f);

	path_entity_increment(&p);
	assert(!path_entity_is_finished(&p));
	path_entity_get_position(&p, 0.6f, &x, &y, &z);
	assert(x == 1.5f && y == 1.0f && z == 0.5f);

	path_entity_increment(&p);
	assert(path_entity_is_finished(&p));

	// Wide mob (spider-ish width=1.4): (int)(1.4+1.0F)=2 -> offset 1.0, the
	// documented Beta quirk (aims at the far corner, not the block centre).
	p.index = 0;
	path_entity_get_position(&p, 1.4f, &x, &y, &z);
	assert(x == 1.0f && y == 1.0f && z == 1.0f);
}

int main(void) {
	test_hash_values();
	test_heap_push_trace();
	test_heap_tie_break_trace();
	test_heap_change_distance_trace();
	test_search_straight_corridor();
	test_search_l_bend();
	test_search_gap_routes_around();
	test_search_ceiling_constraint();
	test_search_unreachable_returns_no_path();
	test_path_entity_cursor();
	printf("test_pathfinding_golden: PathPoint hash, Path heap (incl. a hand-"
		   "traced tie), and Pathfinder.createEntityPathTo (corridor, L-bend, "
		   "gap-routes-around, ceiling constraint, sealed/unreachable) all "
		   "pinned against hand-derived expected values\n");
	return 0;
}
