/*
	aim-snap-1005/A01: soft-snap melee assist (controller playability
	carve-out; docs/deviations.md "Soft-snap melee assist";
	tmp/jobs/aim-help-committee/DECISION.md).

	attack_pick() replaced main.c's inline per-pane crosshair pick
	(raycast_entity at 4.5, own body ignored, else the 4.5 block ray). These
	cases pin the committee's limits:

	  (1) OFF is byte-identical to an independent model of Beta
	      EntityRenderer.getMouseOver (critique-1028: 3.0 entity reach clamped
	      to the block hit, canBeCollidedWith only, 0.1 border) over a
	      deterministic sweep of poses / entity layouts / block fields.
	  (2) cone edges: nothing at Off (0 deg); 2.9 deg in / 3.1 deg out at Low
	      (3 deg); 5.9 in / 6.1 out at Normal (6 deg).
	  (3) reach: aim point at 4.49 assisted, at 4.51 not (same 4.5 reach).
	  (4) occlusion: a block on the snap line, an intervening entity (remote
	      player, passive cow, item) on the snap line -> no assist.
	  (5) precedence: a direct entity hit and a targeted block both win; the
	      assist fields stay clear.
	  (6) crowd determinism: smallest angle, then distance, then entity id --
	      independent of dictionary insertion order.
	  (7) dead (death_time > 0) and destroyed (delay_destroy == 0) targets.
	  (8) exclusions: players, passive animals, squid, tamed wolves (even
	      angry), calm wild wolves, calm pigmen, items, minecarts, boats,
	      paintings, projectiles, TNT, falling sand.
	  (9) angry neutrals: an angry wild wolf and an angry pigman ARE assisted.
	 (10) remote server: never active, at any level.
	 (11) split-screen: pane A's pick reads only pane A's camera -- moving
	      pane B's camera onto the target changes nothing for A.
 (13) Beta getMouseOver for every entity (critique-1028): 3.0 reach on
	      the 0.1-grown box, the block hit clamps it (no hits through walls),
	      items/arrows/eggs/snowballs/bobbers never picked, fireball 1.0
	      border, destroyed skipped, remote sessions identical.
 (12) local split PvP (critique-1028): the OTHER pane's local body (type
	      ENTITY_LOCAL_PLAYER, the host's id 0) is a direct target and an
	      occluder when passed as `peer`; NULL peer (online / single player)
	      keeps the legacy skip; self/absent/dead peers never count.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/aabb.h"
#include "cglm/cglm.h"
#include "entity/monsters_object.h"
#include "game/attack_assist.h"
#include "game/camera.h"

static int failures;
#define CHECK(cond, ...) do { \
	if(!(cond)) { \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		failures++; \
	} \
} while(0)

/* ---- entity fixtures -------------------------------------------------- */
#define MAX_E 64
static struct entity ents[MAX_E];
static struct AABB boxes[MAX_E];
static int n_ents;
static dict_entity_t dict;

static size_t fixture_box(const struct entity* e, struct AABB* out) {
	*out = boxes[e - ents];
	return 1;
}

static void scene_reset(void) {
	dict_entity_reset(dict);
	memset(ents, 0, sizeof(ents));
	memset(boxes, 0, sizeof(boxes));
	n_ents = 0;
}

static struct entity* add_entity(uint32_t id, enum entity_type type,
								 int monster_id, struct AABB box) {
	assert(n_ents < MAX_E);
	struct entity* e = &ents[n_ents];
	boxes[n_ents] = box;
	n_ents++;
	e->id = id;
	e->type = type;
	e->delay_destroy = -1;
	e->getBoundingBox = fixture_box;
	if(type == ENTITY_MONSTER) {
		e->data.monster.id = monster_id;
		e->data.monster.death_time = 0;
	}
	dict_entity_set_at(dict, id, e);
	return e;
}

static struct entity* add_mob(uint32_t id, int monster_id, struct AABB box) {
	return add_entity(id, ENTITY_MONSTER, monster_id, box);
}

/* The eye. Level look along +z (rx = 0, ry = pi/2). x sits at 0.95 so the
 * centre ray stays in block column x = 0 while a ray a few degrees to the
 * right crosses into column x = 1 within the first block. */
#define EYE_X 0.95F
#define EYE_Y 0.5F
#define EYE_Z 0.0F

static struct camera cam_at(float x, float y, float z, float rx, float ry) {
	struct camera c;
	memset(&c, 0, sizeof(c));
	c.x = x;
	c.y = y;
	c.z = z;
	c.rx = rx;
	c.ry = ry;
	return c;
}

static struct camera eye(void) {
	return cam_at(EYE_X, EYE_Y, EYE_Z, 0.0F, GLM_PI_2f);
}

/* A mob box whose near edge sits `deg` degrees right (side +1) or left (-1)
 * of the look ray at depth `dist`: 0.6 wide, 1.8 tall, zero depth in z so the
 * angle to its nearest point is exactly the angle to that edge. */
static struct AABB box_at(float deg, float dist, int side) {
	float off = dist * tanf(glm_rad(deg));
	struct AABB b;
	if(side > 0) {
		b.x1 = EYE_X + off;
		b.x2 = EYE_X + off + 0.6F;
	} else {
		b.x1 = EYE_X - off - 0.6F;
		b.x2 = EYE_X - off;
	}
	b.y1 = EYE_Y - 0.9F;
	b.y2 = EYE_Y + 0.9F;
	b.z1 = EYE_Z + dist;
	b.z2 = EYE_Z + dist; /* zero depth: the near edge IS the aim point */
	return b;
}

static struct AABB box_xyz(float x1, float y1, float z1, float x2, float y2,
						   float z2) {
	return (struct AABB) {x1, y1, z1, x2, y2, z2};
}

/* ---- block fields ------------------------------------------------------ */
struct field {
	bool use_hash;
	int n_solid;
	int solid[8][3];
};

static bool solid_hash(int x, int y, int z) {
	uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u
		^ (uint32_t)z * 83492791u;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return (h % 7u) == 0u;
}

static bool field_cell(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					   struct ray* r, enum side* s) {
	struct field* f = user;
	bool solid = false;
	if(f->use_hash)
		solid = solid_hash(x, y, z);
	for(int i = 0; i < f->n_solid; i++)
		if(f->solid[i][0] == x && f->solid[i][1] == y && f->solid[i][2] == z)
			solid = true;
	if(!solid)
		return false;
	struct AABB unit = {x, y, z, x + 1, y + 1, z + 1};
	return aabb_intersection_ray(&unit, r, s);
}

static struct field empty_field = {0};

static struct camera_ray_result pick(const struct camera* c, int level,
									 bool remote, struct field* f,
									 const struct entity* self) {
	struct camera_ray_result r;
	memset(&r, 0, sizeof(r));
	attack_pick(&(struct attack_pick_input) {
					.entities = &dict,
					.self = self,
					.camera = c,
					.cell = field_cell,
					.cell_user = f ? f : &empty_field,
					.assist_level = level,
					.remote_session = remote,
				},
				&r);
	return r;
}

static struct camera_ray_result pick_peer_in(const struct camera* c,
											 int level,
											 const struct entity* self,
											 const struct entity* peer,
											 struct field* f) {
	struct camera_ray_result r;
	memset(&r, 0, sizeof(r));
	attack_pick(&(struct attack_pick_input) {
					.entities = &dict,
					.self = self,
					.camera = c,
					.cell = field_cell,
					.cell_user = f,
					.assist_level = level,
					.remote_session = false,
					.peer = peer,
				},
				&r);
	return r;
}

static struct camera_ray_result pick_peer(const struct camera* c, int level,
										  const struct entity* self,
										  const struct entity* peer) {
	struct camera_ray_result r;
	memset(&r, 0, sizeof(r));
	attack_pick(&(struct attack_pick_input) {
					.entities = &dict,
					.self = self,
					.camera = c,
					.cell = field_cell,
					.cell_user = &empty_field,
					.assist_level = level,
					.remote_session = false,
					.peer = peer,
				},
				&r);
	return r;
}

static bool assisted(struct camera_ray_result r, uint32_t id) {
	return !r.hit && !r.entity_hit && r.assist_hit && r.assist_entity_id == id;
}

static bool nothing(struct camera_ray_result r) {
	return !r.hit && !r.entity_hit && !r.assist_hit && r.assist_entity_id == 0;
}

/* ---- (1) OFF == Beta EntityRenderer.getMouseOver, byte for byte ------- */
/* critique-1028: an independent transcription of b1.7.3
 * EntityRenderer.getMouseOver (EntityRenderer.java:80-131) over the fixture
 * types, written from the decompile rather than from attack_assist.c:
 * the 4.0 block ray first (PlayerControllerSP/MP getBlockReachDistance);
 * entity reach min(block hit, 3.0); only
 * canBeCollidedWith types (here: monsters, remote players, carts, boats --
 * NOT items); boxes grown by 0.1; eye inside = distance 0; nearest wins.
 * The pre-critique pick (raycast_entity at 4.5, items included, no wall
 * clamp) is no longer the reference -- that was the bug. */
static bool ref_collidable(enum entity_type t) {
	return t == ENTITY_MONSTER || t == ENTITY_REMOTE_PLAYER
		|| t == ENTITY_MINECART || t == ENTITY_BOAT;
}

static void beta_ref_pick(const struct entity* self, const struct camera* c,
						  struct field* f, struct camera_ray_result* out) {
	vec3 o, d;
	camera_get_ray(c, o, d);
	out->assist_hit = false;
	out->assist_entity_id = 0;
	out->entity_hit = false;
	out->entity_id = 0;
	camera_ray_walk(c->x, c->y, c->z,
					c->x + sinf(c->rx) * sinf(c->ry) * 4.0F,
					c->y + cosf(c->ry) * 4.0F,
					c->z + cosf(c->rx) * sinf(c->ry) * 4.0F, field_cell, f,
					out);
	const struct entity* best = NULL;
	float best_t = 0.0F;
	for(int i = 0; i < n_ents; i++) {
		const struct entity* e = &ents[i];
		if(e == self || !ref_collidable(e->type))
			continue;
		struct AABB b = boxes[i];
		b.x1 -= 0.1F; b.y1 -= 0.1F; b.z1 -= 0.1F;
		b.x2 += 0.1F; b.y2 += 0.1F; b.z2 += 0.1F;
		float t;
		if(o[0] > b.x1 && o[0] < b.x2 && o[1] > b.y1 && o[1] < b.y2
		   && o[2] > b.z1 && o[2] < b.z2) {
			t = 0.0F;
		} else {
			float lo = 0.0F, hi = 3.0F;
			const float bl[3] = {b.x1, b.y1, b.z1}, bh[3] = {b.x2, b.y2, b.z2};
			bool miss = false;
			for(int a = 0; a < 3 && !miss; a++) {
				if(fabsf(d[a]) < 1e-12F) {
					miss = o[a] < bl[a] || o[a] > bh[a];
					continue;
				}
				float t1 = (bl[a] - o[a]) / d[a], t2 = (bh[a] - o[a]) / d[a];
				lo = fmaxf(lo, fminf(t1, t2));
				hi = fminf(hi, fmaxf(t1, t2));
				miss = lo > hi;
			}
			if(miss)
				continue;
			t = lo;
		}
		/* dict iteration order != array order: break exact ties by id so
		 * the comparison never depends on it (none occur in this sweep). */
		if(!best || t < best_t || (t == best_t && e->id < best->id)) {
			best = e;
			best_t = t;
		}
	}
	if(!best)
		return;
	struct camera_ray_result blk = {0};
	camera_ray_walk(o[0], o[1], o[2], o[0] + d[0] * best_t,
					o[1] + d[1] * best_t, o[2] + d[2] * best_t, field_cell, f,
					&blk);
	if(blk.hit)
		return; /* nearer block: Beta's reach clamp */
	out->entity_hit = true;
	out->entity_id = best->id;
	out->hit = false;
}

static uint32_t lcg(uint32_t* s) {
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}
static float frand(uint32_t* s, float lo, float hi) {
	return lo + (hi - lo) * (float)(lcg(s) & 0xFFFFu) / 65535.0F;
}

static void test_off_equivalence(void) {
	uint32_t seed = 0xA55A1005u;
	struct field f = {.use_hash = true};
	int compared = 0;
	for(int layout = 0; layout < 40; layout++) {
		scene_reset();
		int count = 1 + (int)(lcg(&seed) % 12u);
		for(int i = 0; i < count; i++) {
			float x = frand(&seed, -4, 4), y = frand(&seed, -2, 3),
				  z = frand(&seed, -4, 4);
			enum entity_type types[] = {ENTITY_MONSTER, ENTITY_ITEM,
				ENTITY_REMOTE_PLAYER, ENTITY_MINECART, ENTITY_BOAT};
			enum entity_type t = types[lcg(&seed) % 5u];
			int mid = (int)(lcg(&seed) % MONSTER_COUNT);
			add_entity(100 + (uint32_t)i, t, mid,
					   box_xyz(x, y, z, x + 0.6F, y + 1.8F, z + 0.6F));
		}
		/* An own body that would otherwise be hit: the viewer is excluded. */
		struct entity* self = add_entity(99, ENTITY_REMOTE_PLAYER, 0,
			box_xyz(-0.3F, -1.0F, -0.3F, 0.3F, 0.8F, 0.3F));
		for(int pose = 0; pose < 50; pose++) {
			struct camera c = cam_at(frand(&seed, -1, 1), frand(&seed, 0, 1),
									 frand(&seed, -1, 1),
									 frand(&seed, -GLM_PIf, GLM_PIf),
									 frand(&seed, 0.05F, GLM_PIf - 0.05F));
			struct camera_ray_result a, b;
			/* Same stale pattern in both: fields neither writes (x/y/z/side
			 * on a block miss) must stay untouched in both. */
			memset(&a, 0xA7, sizeof(a));
			memset(&b, 0xA7, sizeof(b));
			beta_ref_pick(self, &c, &f, &a);
			attack_pick(&(struct attack_pick_input) {
							.entities = &dict, .self = self, .camera = &c,
							.cell = field_cell, .cell_user = &f,
							.assist_level = ATTACK_ASSIST_OFF,
							.remote_session = false},
						&b);
			CHECK(memcmp(&a, &b, sizeof(a)) == 0,
				  "OFF pick differs from the Beta model (layout %d pose %d)", layout,
				  pose);
			compared++;
		}
	}
	CHECK(compared == 2000, "compared %d", compared);
}

/* ---- (2) cone edges, (3) reach ---------------------------------------- */
static void test_cone_and_reach(void) {
	struct camera c = eye();
	CHECK(attack_assist_half_angle_deg(ATTACK_ASSIST_OFF, false) == 0.0F,
		  "off angle");
	CHECK(attack_assist_half_angle_deg(ATTACK_ASSIST_LOW, false) == 3.0F,
		  "low angle");
	CHECK(attack_assist_half_angle_deg(ATTACK_ASSIST_NORMAL, false) == 6.0F,
		  "normal angle");
	CHECK(attack_assist_half_angle_deg(7, false) == 0.0F, "bad level");
	CHECK(attack_assist_half_angle_deg(-1, false) == 0.0F, "neg level");

	const struct {
		float deg;
		int level;
		bool expect;
	} rows[] = {
		{4.0F, ATTACK_ASSIST_OFF, false},  {2.9F, ATTACK_ASSIST_OFF, false},
		{2.9F, ATTACK_ASSIST_LOW, true},   {3.1F, ATTACK_ASSIST_LOW, false},
		{5.9F, ATTACK_ASSIST_NORMAL, true}, {6.1F, ATTACK_ASSIST_NORMAL, false},
		{2.9F, ATTACK_ASSIST_NORMAL, true},
	};
	for(size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		scene_reset();
		add_mob(7, MONSTER_ZOMBIE, box_at(rows[i].deg, 2.5F, +1));
		struct camera_ray_result r = pick(&c, rows[i].level, false, NULL, NULL);
		if(rows[i].expect)
			CHECK(assisted(r, 7), "%.1f deg level %d should assist",
				  rows[i].deg, rows[i].level);
		else
			CHECK(nothing(r), "%.1f deg level %d should not assist",
				  rows[i].deg, rows[i].level);
	}
	/* Left side is symmetric. */
	scene_reset();
	add_mob(8, MONSTER_SKELETON, box_at(5.0F, 2.5F, -1));
	CHECK(assisted(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 8),
		  "left side");

	/* Reach: the aim point (the box's near edge) inside vs outside the 3.0
	 * entity reach (critique-1028; Beta getMouseOver). The edge is 4 deg
	 * off-axis (clear of the direct pick's 0.1 border), so distance =
	 * depth / cos(4 deg). */
	float in_depth = 2.99F * cosf(glm_rad(4.0F));
	float out_depth = 3.01F * cosf(glm_rad(4.0F));
	scene_reset();
	add_mob(9, MONSTER_ZOMBIE, box_at(4.0F, in_depth, +1));
	CHECK(assisted(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 9),
		  "2.99 inside reach");
	scene_reset();
	add_mob(9, MONSTER_ZOMBIE, box_at(4.0F, out_depth, +1));
	CHECK(nothing(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL)),
		  "3.01 outside reach");
	/* Behind the eye never counts. */
	scene_reset();
	add_mob(10, MONSTER_ZOMBIE, box_xyz(EYE_X + 0.05F, 0, -2.0F,
										EYE_X + 0.65F, 1.8F, -1.9F));
	CHECK(nothing(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL)),
		  "behind the eye");
}

/* ---- (4) occlusion ----------------------------------------------------- */
static void test_occlusion(void) {
	struct camera c = eye();
	/* The 4-deg snap line from x=0.95 crosses x=1 at z~0.7: block (1,0,2)
	 * sits on it, the centre ray (column x=0) stays clear. */
	struct field wall = {.n_solid = 1, .solid = {{1, 0, 2}}};
	scene_reset();
	add_mob(20, MONSTER_ZOMBIE, box_at(4.0F, 2.8F, +1));
	CHECK(assisted(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 20),
		  "clear line assists");
	struct camera_ray_result r = pick(&c, ATTACK_ASSIST_NORMAL, false, &wall,
									  NULL);
	CHECK(!r.hit, "centre ray must be clear of the wall");
	CHECK(nothing(r), "block on the snap line occludes");

	/* Intervening entities on the snap line (not on the centre ray). */
	const enum entity_type blockers[] = {ENTITY_REMOTE_PLAYER, ENTITY_MONSTER,
		ENTITY_ITEM};
	for(size_t i = 0; i < 3; i++) {
		scene_reset();
		add_mob(21, MONSTER_ZOMBIE, box_at(4.0F, 2.8F, +1));
		/* Snap line at depth 2: x = 0.95 + 2*tan(4 deg) = 1.09. */
		add_entity(22, blockers[i], MONSTER_COW,
				   box_xyz(1.06F, 0.0F, 1.9F, 1.3F, 1.0F, 2.1F));
		r = pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL);
		CHECK(!r.entity_hit, "blocker %zu must not be on the centre ray", i);
		/* A dropped item is not canBeCollidedWith: Beta's corrected ray
		 * passes through it, so it no longer occludes (critique-1028). */
		if(blockers[i] == ENTITY_ITEM)
			CHECK(assisted(r, 21), "an item never occludes");
		else
			CHECK(nothing(r), "intervening entity type %d occludes",
				  blockers[i]);
	}
	/* An excluded entity BEHIND the target does not occlude. */
	scene_reset();
	add_mob(23, MONSTER_ZOMBIE, box_at(4.0F, 2.0F, +1));
	add_entity(24, ENTITY_REMOTE_PLAYER, 0, box_at(4.0F, 3.5F, +1));
	CHECK(assisted(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 23),
		  "player behind the target");
}

/* ---- (5) precedence ---------------------------------------------------- */
static void test_precedence(void) {
	struct camera c = eye();
	/* Direct hit (a cow dead ahead) beats a hostile in the cone. */
	scene_reset();
	add_entity(30, ENTITY_MONSTER, MONSTER_COW,
			   box_xyz(EYE_X - 0.3F, 0, 2.0F, EYE_X + 0.3F, 1.4F, 2.6F));
	add_mob(31, MONSTER_ZOMBIE, box_at(4.0F, 2.0F, +1));
	struct camera_ray_result r = pick(&c, ATTACK_ASSIST_NORMAL, false, NULL,
									  NULL);
	CHECK(r.entity_hit && r.entity_id == 30 && !r.assist_hit
			  && r.assist_entity_id == 0,
		  "direct entity hit wins");
	/* A targeted block beats a hostile in the cone (mining untouched). */
	struct field ahead = {.n_solid = 1, .solid = {{0, 0, 3}}};
	scene_reset();
	add_mob(32, MONSTER_ZOMBIE, box_at(4.0F, 2.0F, +1));
	r = pick(&c, ATTACK_ASSIST_NORMAL, false, &ahead, NULL);
	CHECK(r.hit && r.x == 0 && r.y == 0 && r.z == 3 && !r.entity_hit
			  && !r.assist_hit && r.assist_entity_id == 0,
		  "block target wins");
	/* The own body is never a target, direct or assisted. */
	scene_reset();
	struct entity* self = add_mob(33, MONSTER_ZOMBIE, box_at(4.0F, 2.0F, +1));
	CHECK(nothing(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, self)),
		  "own body excluded");
}

/* ---- (6) crowd determinism -------------------------------------------- */
static void test_crowd(void) {
	struct camera c = eye();
	/* Smallest angle wins, even when farther. */
	scene_reset();
	add_mob(40, MONSTER_ZOMBIE, box_at(5.0F, 2.0F, +1));
	add_mob(41, MONSTER_ZOMBIE, box_at(3.0F, 2.6F, -1));
	CHECK(assisted(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 41),
		  "smallest angle");
	/* Equal angle (same 0.1-degree ranking bucket): nearer wins. */
	scene_reset();
	add_mob(42, MONSTER_ZOMBIE, box_at(4.0F, 2.8F, +1));
	add_mob(43, MONSTER_ZOMBIE, box_at(4.0F, 2.0F, -1));
	CHECK(assisted(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 43),
		  "equal angle -> nearer");
	/* Equal angle AND distance: exact mirror images about an eye at x = 0.5
	 * (every coordinate binary-exact, so the floats tie bit for bit). The
	 * lower id wins whichever side it is on and whatever the insertion
	 * order. */
	struct camera mid = cam_at(0.5F, 0.5F, 0.0F, 0.0F, GLM_PI_2f);
	const struct AABB right = box_xyz(0.625F, -0.4F, 2.0F, 1.25F, 1.4F, 2.0F);
	const struct AABB left = box_xyz(-0.25F, -0.4F, 2.0F, 0.375F, 1.4F, 2.0F);
	for(int order = 0; order < 4; order++) {
		scene_reset();
		uint32_t id_right = (order & 1) ? 44 : 45;
		uint32_t id_left = (order & 1) ? 45 : 44;
		if(order & 2) {
			add_mob(id_right, MONSTER_ZOMBIE, right);
			add_mob(id_left, MONSTER_ZOMBIE, left);
		} else {
			add_mob(id_left, MONSTER_ZOMBIE, left);
			add_mob(id_right, MONSTER_ZOMBIE, right);
		}
		CHECK(assisted(pick(&mid, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 44),
			  "exact tie -> lower id (order %d)", order);
	}
	/* Ineligible nearer candidates do not shadow the ranking. */
	scene_reset();
	add_entity(46, ENTITY_MONSTER, MONSTER_PIG, box_at(3.5F, 2.5F, -1));
	add_mob(47, MONSTER_CREEPER, box_at(4.5F, 2.5F, +1));
	CHECK(assisted(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 47),
		  "pig skipped, creeper chosen");
}

/* ---- (7) dead / destroyed ---------------------------------------------- */
static void test_dead(void) {
	struct camera c = eye();
	scene_reset();
	struct entity* z = add_mob(50, MONSTER_ZOMBIE, box_at(4.0F, 2.0F, +1));
	z->data.monster.death_time = 1;
	CHECK(nothing(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL)),
		  "dying mob");
	CHECK(!attack_assist_eligible(z), "dying mob ineligible");
	z->data.monster.death_time = 0;
	z->delay_destroy = 0;
	CHECK(nothing(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL)),
		  "destroyed mob");
	z->delay_destroy = -1;
	CHECK(assisted(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 50),
		  "alive again");
	/* Despawned = gone from the view dictionary. */
	dict_entity_erase(dict, 50);
	CHECK(nothing(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL)),
		  "despawned");
	/* An id-0 fixture is never a target. */
	scene_reset();
	struct entity* zero = add_mob(0, MONSTER_ZOMBIE, box_at(4.0F, 2.0F, +1));
	CHECK(!attack_assist_eligible(zero), "id 0");
}

/* ---- (8) exclusions, (9) angry neutrals ------------------------------- */
static void test_types(void) {
	struct camera c = eye();
	const int hostile[] = {MONSTER_ZOMBIE, MONSTER_CREEPER, MONSTER_SKELETON,
		MONSTER_SPIDER, MONSTER_SLIME_SMALL, MONSTER_SLIME_MEDIUM,
		MONSTER_SLIME_BIG, MONSTER_GHAST, MONSTER_GIANT};
	for(size_t i = 0; i < sizeof(hostile) / sizeof(hostile[0]); i++) {
		scene_reset();
		add_mob(60, hostile[i], box_at(4.0F, 2.0F, +1));
		CHECK(assisted(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 60),
			  "hostile %d included", hostile[i]);
	}
	const int passive[] = {MONSTER_PIG, MONSTER_COW, MONSTER_CHICKEN,
		MONSTER_SHEEP, MONSTER_SQUID};
	for(size_t i = 0; i < sizeof(passive) / sizeof(passive[0]); i++) {
		scene_reset();
		add_mob(61, passive[i], box_at(4.0F, 2.0F, +1));
		CHECK(nothing(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL)),
			  "passive %d excluded", passive[i]);
	}
	const enum entity_type others[] = {ENTITY_LOCAL_PLAYER,
		ENTITY_REMOTE_PLAYER, ENTITY_ITEM, ENTITY_MINECART, ENTITY_BOAT,
		ENTITY_PAINTING, ENTITY_ARROW, ENTITY_PRIMED_TNT, ENTITY_EGG,
		ENTITY_SNOWBALL, ENTITY_FIREBALL, ENTITY_FISH, ENTITY_FALLING_SAND};
	for(size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
		struct entity probe;
		memset(&probe, 0, sizeof(probe));
		probe.id = 62;
		probe.type = others[i];
		probe.delay_destroy = -1;
		CHECK(!attack_assist_eligible(&probe), "type %d excluded", others[i]);
		if(others[i] == ENTITY_PAINTING || others[i] == ENTITY_LOCAL_PLAYER)
			continue; /* painting geometry is not a fixture box; local
						 players are skipped by the raycast itself */
		scene_reset();
		add_entity(62, others[i], 0, box_at(4.0F, 2.0F, +1));
		struct camera_ray_result r = pick(&c, ATTACK_ASSIST_NORMAL, false,
										  NULL, NULL);
		/* A fireball's 1.0 collision border puts this 0.14-off box on the
		 * DIRECT ray (Beta EntityFireball.getCollisionBorderSize); it is a
		 * direct target, never an assisted one. */
		if(others[i] == ENTITY_FIREBALL)
			CHECK(r.entity_hit && r.entity_id == 62 && !r.assist_hit,
				  "fireball picked directly");
		else
			CHECK(nothing(r), "type %d excluded in the pick", others[i]);
	}
	/* Wolves: only angry AND wild. */
	const struct {
		uint8_t flags;
		bool expect;
	} wolves[] = {
		{0, false},
		{WOLF_FLAG_ANGRY, true},
		{WOLF_FLAG_TAMED, false},
		{WOLF_FLAG_TAMED | WOLF_FLAG_ANGRY, false},
		{WOLF_FLAG_TAMED | WOLF_FLAG_SITTING, false},
	};
	for(size_t i = 0; i < sizeof(wolves) / sizeof(wolves[0]); i++) {
		scene_reset();
		struct entity* w = add_mob(63, MONSTER_WOLF, box_at(4.0F, 2.0F, +1));
		w->data.monster.wolf_flags = wolves[i].flags;
		struct camera_ray_result r = pick(&c, ATTACK_ASSIST_NORMAL, false,
										  NULL, NULL);
		CHECK(wolves[i].expect ? assisted(r, 63) : nothing(r),
			  "wolf flags 0x%x", wolves[i].flags);
	}
	/* Pigmen: only angry (client mirror of anger_level > 0). */
	scene_reset();
	struct entity* pz = add_mob(64, MONSTER_PIG_ZOMBIE, box_at(4.0F, 2.0F, +1));
	pz->data.monster.pig_zombie_angry = false;
	CHECK(nothing(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL)),
		  "calm pigman excluded");
	pz->data.monster.pig_zombie_angry = true;
	CHECK(assisted(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 64),
		  "angry pigman included");
}

/* ---- crosshair tint ----------------------------------------------------- */
static void test_tint(void) {
	struct camera c = eye();
	scene_reset();
	add_mob(90, MONSTER_ZOMBIE, box_at(4.0F, 2.0F, +1));
	struct camera_ray_result r = pick(&c, ATTACK_ASSIST_NORMAL, false, NULL,
									  NULL);
	CHECK(attack_assist_crosshair_tinted(&r), "tinted when assisted");
	r = pick(&c, ATTACK_ASSIST_OFF, false, NULL, NULL);
	CHECK(!attack_assist_crosshair_tinted(&r), "not tinted when Off");
	/* A direct hit never tints, even if stale assist fields were set. */
	struct camera_ray_result direct = {.entity_hit = true, .entity_id = 90,
		.assist_hit = true, .assist_entity_id = 90};
	CHECK(!attack_assist_crosshair_tinted(&direct), "direct hit untinted");
	struct camera_ray_result block = {.hit = true, .assist_hit = true,
		.assist_entity_id = 90};
	CHECK(!attack_assist_crosshair_tinted(&block), "block target untinted");
	CHECK(!attack_assist_crosshair_tinted(NULL), "null untinted");
}

/* ---- (10) remote ------------------------------------------------------- */
static void test_remote(void) {
	struct camera c = eye();
	scene_reset();
	add_mob(70, MONSTER_ZOMBIE, box_at(4.0F, 2.0F, +1));
	for(int level = 0; level < ATTACK_ASSIST_LEVELS; level++) {
		CHECK(attack_assist_half_angle_deg(level, true) == 0.0F,
			  "remote angle level %d", level);
		CHECK(nothing(pick(&c, level, true, NULL, NULL)),
			  "remote never assists (level %d)", level);
	}
	CHECK(assisted(pick(&c, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 70),
		  "same scene assists locally");
}

/* ---- (11) split-screen ------------------------------------------------- */
static void test_split(void) {
	struct camera a = eye();
	/* Pane B stands elsewhere, looking the other way. */
	struct camera b = cam_at(10.5F, 0.5F, 10.5F, GLM_PIf, GLM_PI_2f);
	scene_reset();
	add_mob(80, MONSTER_ZOMBIE, box_at(4.0F, 2.5F, +1));
	struct camera_ray_result ra = pick(&a, ATTACK_ASSIST_NORMAL, false, NULL,
									   NULL);
	struct camera_ray_result rb = pick(&b, ATTACK_ASSIST_NORMAL, false, NULL,
									   NULL);
	CHECK(assisted(ra, 80), "pane A sees its target");
	CHECK(nothing(rb), "pane B does not");
	/* Move pane B right onto the target and turn pane A away: each pane's
	 * pick follows only its own camera. */
	struct camera b2 = cam_at(EYE_X, EYE_Y, EYE_Z, 0.0F, GLM_PI_2f);
	struct camera_ray_result ra2 = pick(&a, ATTACK_ASSIST_NORMAL, false, NULL,
										NULL);
	CHECK(memcmp(&ra, &ra2, sizeof(ra)) == 0, "A unchanged by B moving");
	CHECK(assisted(pick(&b2, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 80),
		  "B at A's pose assists independently");
	struct camera a_away = cam_at(EYE_X, EYE_Y, EYE_Z, GLM_PIf, GLM_PI_2f);
	CHECK(nothing(pick(&a_away, ATTACK_ASSIST_NORMAL, false, NULL, NULL)),
		  "A turned away");
	CHECK(assisted(pick(&b2, ATTACK_ASSIST_NORMAL, false, NULL, NULL), 80),
		  "B unaffected by A turning");
}

/* ---- (12) local split PvP --------------------------------------------- */
static struct entity* add_local_body(uint32_t id, struct AABB box) {
	struct entity* e = add_entity(id, ENTITY_LOCAL_PLAYER, 0, box);
	e->health = 20;
	return e;
}

static void test_split_peer(void) {
	struct camera c = eye();
	const struct AABB ahead = box_xyz(EYE_X - 0.3F, -0.9F, 2.0F, EYE_X + 0.3F,
									  0.9F, 2.6F);
	/* Both pane bodies: P1 (host, id 0) is this pane's self, P2 ahead. */
	for(int dir = 0; dir < 2; dir++) {
		scene_reset();
		uint32_t self_id = dir ? 4242 : 0, peer_id = dir ? 0 : 4242;
		struct entity* self = add_local_body(self_id,
			box_xyz(EYE_X - 0.3F, -1.1F, -0.3F, EYE_X + 0.3F, 0.7F, 0.3F));
		struct entity* peer = add_local_body(peer_id, ahead);
		struct camera_ray_result r = pick_peer(&c, ATTACK_ASSIST_OFF, self,
											   peer);
		CHECK(r.entity_hit && r.entity_id == peer_id && !r.hit,
			  "P%d hits P%d (peer id %u)", dir + 1, 2 - dir, peer_id);
		/* Online / single player: no peer -> the legacy skip, unchanged. */
		r = pick_peer(&c, ATTACK_ASSIST_OFF, self, NULL);
		CHECK(!r.entity_hit && !r.hit, "NULL peer keeps the legacy skip");
		/* The own body passed as peer is still never a target. */
		r = pick_peer(&c, ATTACK_ASSIST_OFF, self, self);
		CHECK(!r.entity_hit, "self as peer ignored");
		/* Dead peer: the server refuses it; not a target. */
		peer->health = 0;
		r = pick_peer(&c, ATTACK_ASSIST_OFF, self, peer);
		CHECK(!r.entity_hit, "dead peer ignored");
		peer->health = 20;
		/* A peer outside this pane's entity view (another world) is ignored. */
		dict_entity_erase(dict, peer_id);
		r = pick_peer(&c, ATTACK_ASSIST_OFF, self, peer);
		CHECK(!r.entity_hit, "peer not in this view ignored");
	}
	/* Nearer wins both ways: peer in front of a mob, mob in front of peer. */
	scene_reset();
	struct entity* peer = add_local_body(0, ahead);
	add_mob(5, MONSTER_ZOMBIE, box_xyz(EYE_X - 0.3F, -0.9F, 2.7F, EYE_X + 0.3F,
									   0.9F, 4.1F));
	struct camera_ray_result r = pick_peer(&c, ATTACK_ASSIST_NORMAL, NULL, peer);
	CHECK(r.entity_hit && r.entity_id == 0 && !r.assist_hit,
		  "peer in front of a mob blocks the hit");
	r = pick_peer(&c, ATTACK_ASSIST_NORMAL, NULL, NULL);
	CHECK(r.entity_hit && r.entity_id == 5, "without peer the mob is hit");
	scene_reset();
	peer = add_local_body(4242, box_xyz(EYE_X - 0.3F, -0.9F, 2.7F,
										EYE_X + 0.3F, 0.9F, 4.1F));
	add_mob(6, MONSTER_ZOMBIE, ahead);
	r = pick_peer(&c, ATTACK_ASSIST_OFF, NULL, peer);
	CHECK(r.entity_hit && r.entity_id == 6, "mob in front of the peer wins");
	/* Out of reach (grown box from 3.1 > 3.0): no hit. */
	scene_reset();
	peer = add_local_body(4242, box_xyz(EYE_X - 0.3F, -0.9F, 3.2F,
										EYE_X + 0.3F, 0.9F, 5.2F));
	r = pick_peer(&c, ATTACK_ASSIST_OFF, NULL, peer);
	CHECK(!r.entity_hit, "peer beyond 3.0 reach");
	/* The peer occludes the assist snap line (same blocker box as (4)). */
	scene_reset();
	add_mob(21, MONSTER_ZOMBIE, box_at(4.0F, 2.8F, +1));
	peer = add_local_body(4242, box_xyz(1.06F, 0.0F, 1.9F, 1.3F, 1.0F, 2.1F));
	r = pick_peer(&c, ATTACK_ASSIST_NORMAL, NULL, NULL);
	CHECK(assisted(r, 21), "assist without peer");
	r = pick_peer(&c, ATTACK_ASSIST_NORMAL, NULL, peer);
	CHECK(!r.entity_hit, "peer off the centre ray");
	CHECK(nothing(r), "peer on the snap line occludes the assist");
	/* Block vs peer (Beta getMouseOver: block ray first, entity reach
	 * clamped to the block hit). The eye column is x=0, row y=0. */
	scene_reset();
	peer = add_local_body(4242, ahead); /* z 2.0..2.6 */
	struct field in_front = {.n_solid = 1, .solid = {{0, 0, 1}}};
	r = pick_peer_in(&c, ATTACK_ASSIST_OFF, NULL, peer, &in_front);
	CHECK(r.hit && !r.entity_hit && r.z == 1,
		  "peer behind the aimed block: the block stays targeted");
	/* Peer box (grown by 0.1) wholly inside the aimed block's cell z=1. */
	scene_reset();
	struct entity* sunk = add_local_body(4242, box_xyz(EYE_X - 0.3F, -0.9F,
		1.2F, EYE_X + 0.3F, 0.9F, 1.8F));
	r = pick_peer_in(&c, ATTACK_ASSIST_OFF, NULL, sunk, &in_front);
	CHECK(r.hit && !r.entity_hit && r.z == 1,
		  "peer inside the aimed block: the block stays targeted");
	/* Beta compares distances, not cells: a box poking 0.1 out in front of
	 * the block face (grown z 1.9 < face 2.0) IS nearer and is picked. */
	scene_reset();
	peer = add_local_body(4242, ahead);
	struct field around = {.n_solid = 1, .solid = {{0, 0, 2}}};
	r = pick_peer_in(&c, ATTACK_ASSIST_OFF, NULL, peer, &around);
	CHECK(r.entity_hit && r.entity_id == 4242,
		  "peer border in front of the block face is nearer");
	struct field behind = {.n_solid = 1, .solid = {{0, 0, 3}}};
	r = pick_peer_in(&c, ATTACK_ASSIST_OFF, NULL, peer, &behind);
	CHECK(r.entity_hit && r.entity_id == 4242 && !r.hit,
		  "peer in front of a block is the target");
	/* The peer is never itself an assist target (players excluded). */
	scene_reset();
	peer = add_local_body(4242, box_at(4.0F, 2.0F, +1));
	CHECK(nothing(pick_peer(&c, ATTACK_ASSIST_NORMAL, NULL, peer)),
		  "peer never assisted");
}

/* ---- (13) Beta getMouseOver rules for every entity (critique-1028) ----- */
static void test_beta_mouse_over(void) {
	struct camera c = eye();
	/* Reach 3.0 on the grown (0.1) box: near face at 3.05 -> grown 2.95 in;
	 * near face at 3.15 -> grown 3.05 out. */
	scene_reset();
	add_mob(100, MONSTER_ZOMBIE, box_xyz(EYE_X - 0.3F, -0.9F, 3.05F,
										 EYE_X + 0.3F, 0.9F, 3.65F));
	struct camera_ray_result r = pick(&c, ATTACK_ASSIST_OFF, false, NULL, NULL);
	CHECK(r.entity_hit && r.entity_id == 100, "mob at 2.95 (grown) in reach");
	scene_reset();
	add_mob(100, MONSTER_ZOMBIE, box_xyz(EYE_X - 0.3F, -0.9F, 3.15F,
										 EYE_X + 0.3F, 0.9F, 3.75F));
	r = pick(&c, ATTACK_ASSIST_OFF, false, NULL, NULL);
	CHECK(!r.entity_hit && !r.hit, "mob at 3.05 (grown) out of reach");
	/* The 0.1 collision border: a mob whose raw box misses the ray by 0.05
	 * is still picked. */
	scene_reset();
	add_mob(101, MONSTER_ZOMBIE, box_xyz(EYE_X + 0.05F, -0.9F, 2.0F,
										 EYE_X + 0.65F, 0.9F, 2.6F));
	r = pick(&c, ATTACK_ASSIST_OFF, false, NULL, NULL);
	CHECK(r.entity_hit && r.entity_id == 101, "0.1 border picks a near miss");
	/* No hitting through walls: a mob behind the aimed block -> the block. */
	struct field wall = {.n_solid = 1, .solid = {{0, 0, 1}}};
	scene_reset();
	add_mob(102, MONSTER_ZOMBIE, box_xyz(EYE_X - 0.3F, -0.9F, 2.0F,
										 EYE_X + 0.3F, 0.9F, 2.6F));
	r = pick(&c, ATTACK_ASSIST_OFF, false, &wall, NULL);
	CHECK(r.hit && !r.entity_hit && r.z == 1, "mob behind a wall: block");
	/* ... and in front of the block the mob is unchanged. */
	struct field far_wall = {.n_solid = 1, .solid = {{0, 0, 3}}};
	r = pick(&c, ATTACK_ASSIST_OFF, false, &far_wall, NULL);
	CHECK(r.entity_hit && r.entity_id == 102 && !r.hit, "mob before a block");
	/* Dropped items (and plain arrows) are never picked and never stop
	 * mining: the block behind them stays targeted. */
	const enum entity_type never[] = {ENTITY_ITEM, ENTITY_ARROW, ENTITY_EGG,
		ENTITY_SNOWBALL, ENTITY_FISH, ENTITY_LOCAL_PLAYER};
	for(size_t i = 0; i < sizeof(never) / sizeof(never[0]); i++) {
		scene_reset();
		add_entity(103, never[i], 0, box_xyz(EYE_X - 0.3F, -0.3F, 1.2F,
											 EYE_X + 0.3F, 0.8F, 1.8F));
		r = pick(&c, ATTACK_ASSIST_OFF, false, &far_wall, NULL);
		CHECK(r.hit && !r.entity_hit && r.z == 3,
			  "type %d in front of a block: the block is mined", never[i]);
		r = pick(&c, ATTACK_ASSIST_OFF, false, NULL, NULL);
		CHECK(!r.entity_hit, "type %d never picked", never[i]);
	}
	/* Collidable non-mobs stay pickable (Beta canBeCollidedWith true). */
	const enum entity_type always[] = {ENTITY_MINECART, ENTITY_BOAT,
		ENTITY_PRIMED_TNT, ENTITY_FALLING_SAND, ENTITY_REMOTE_PLAYER};
	for(size_t i = 0; i < sizeof(always) / sizeof(always[0]); i++) {
		scene_reset();
		add_entity(104, always[i], 0, box_xyz(EYE_X - 0.3F, -0.3F, 1.2F,
											  EYE_X + 0.3F, 0.8F, 1.8F));
		r = pick(&c, ATTACK_ASSIST_OFF, false, &far_wall, NULL);
		CHECK(r.entity_hit && r.entity_id == 104, "type %d picked",
			  always[i]);
	}
	/* A fireball (client: arrow display with the fireball flag) has a 1.0
	 * border: 0.5 off the ray is still a hit; a plain arrow there is not. */
	scene_reset();
	struct entity* fb = add_entity(105, ENTITY_ARROW, 0,
		box_xyz(EYE_X + 0.5F, -0.25F, 1.5F, EYE_X + 1.0F, 0.25F, 2.0F));
	fb->data.arrow.fireball = true;
	r = pick(&c, ATTACK_ASSIST_OFF, false, NULL, NULL);
	CHECK(r.entity_hit && r.entity_id == 105, "fireball 1.0 border");
	fb->data.arrow.fireball = false;
	r = pick(&c, ATTACK_ASSIST_OFF, false, NULL, NULL);
	CHECK(!r.entity_hit, "plain arrow not picked");
	/* Destroyed (isDead) entities are skipped. */
	scene_reset();
	struct entity* gone = add_mob(106, MONSTER_ZOMBIE,
		box_xyz(EYE_X - 0.3F, -0.9F, 2.0F, EYE_X + 0.3F, 0.9F, 2.6F));
	gone->delay_destroy = 0;
	r = pick(&c, ATTACK_ASSIST_OFF, false, NULL, NULL);
	CHECK(!r.entity_hit, "destroyed entity skipped");
	/* Block reach 4.0 (Beta getBlockReachDistance, PlayerControllerSP.java
	 * :109-111 / PlayerControllerMP.java:118-120): a face 3.95 away is
	 * targeted, 4.05 away is not. */
	{
		/* (`gone` above is still destroyed, so it cannot shadow the wall.) */
		struct field reach_wall = {.n_solid = 1, .solid = {{0, 0, 4}}};
		struct camera near = cam_at(EYE_X, EYE_Y, 0.05F, 0.0F, GLM_PI_2f);
		struct camera far = cam_at(EYE_X, EYE_Y, -0.05F, 0.0F, GLM_PI_2f);
		r = pick(&near, ATTACK_ASSIST_OFF, false, &reach_wall, NULL);
		CHECK(r.hit && r.z == 4, "block face at 3.95 in reach");
		r = pick(&far, ATTACK_ASSIST_OFF, false, &reach_wall, NULL);
		CHECK(!r.hit, "block face at 4.05 out of reach");
		r = pick(&far, ATTACK_ASSIST_OFF, true, &reach_wall, NULL);
		CHECK(!r.hit, "remote: block face at 4.05 out of reach");
	}
	/* Online (remote session) uses the same direct pick. */
	gone->delay_destroy = -1;
	r = pick(&c, ATTACK_ASSIST_OFF, true, &wall, NULL);
	CHECK(r.hit && !r.entity_hit, "remote: no hitting through walls");
	r = pick(&c, ATTACK_ASSIST_OFF, true, NULL, NULL);
	CHECK(r.entity_hit && r.entity_id == 106, "remote: mob picked");
}

int main(void) {
	dict_entity_init(dict);
	test_off_equivalence();
	test_cone_and_reach();
	test_occlusion();
	test_precedence();
	test_crowd();
	test_dead();
	test_types();
	test_remote();
	test_split();
	test_split_peer();
	test_beta_mouse_over();
	test_tint();
	dict_entity_clear(dict);
	printf("%s test_attack_assist (%d failure%s)\n", failures ? "FAIL" : "PASS",
		   failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
