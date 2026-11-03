/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited). See autopilot.h.
*/

#include "autopilot.h"

#include <cglm/cglm.h>
#include <string.h>

#include "autopilot_keepup.h"
#include "autopilot_unstick.h"
#include "game_state.h"

/* Probes follow the host client body through a world-payload view switch.
 * Before a body is bound, retain the legacy primary-world fallback. */
static struct world* autopilot_world(void) {
    struct entity* body = gstate.players[0].local_player;
    return body && !body->on_server && body->world
        ? body->world : &gstate.world;
}

bool autopilot_portal_geometry_sample(const struct entity* body,
	struct autopilot_portal_geometry* out) {
	if(!body || body->on_server || !body->world || !out)
		return false;
	struct world* w = body->world;
	memset(out, 0, sizeof(*out));
	out->x = (int)floorf(body->pos[0]);
	out->z = (int)floorf(body->pos[2]);
	float feet = body->pos[1] - 1.62F;
	out->feet_y = (int)floorf(feet + 0.01F);
	out->portal_axis = '-';
	for(int level = 0; level < 3; level++) {
		int y = out->feet_y + level - 1;
		for(int dz = -2; dz <= 2; dz++) {
			char* row = out->cells[level][dz + 2];
			for(int dx = -2; dx <= 2; dx++) {
				int x = out->x + dx, z = out->z + dz;
				if(!world_find_chunk(w, x, y, z)) {
					row[dx + 2] = '?';
					continue;
				}
				uint8_t block = world_get_block(w, x, y, z).type;
				row[dx + 2] = block == BLOCK_PORTAL ? 'P'
					: block == BLOCK_OBSIDIAN ? 'O'
					: block == BLOCK_AIR ? '.' : '#';
			}
			row[5] = '\0';
		}
	}
	for(int dz = 0; dz < 5; dz++)
		for(int dx = 0; dx < 5; dx++) {
			if(out->cells[1][dz][dx] != 'P')
				continue;
			if(dx < 4 && out->cells[1][dz][dx + 1] == 'P')
				out->portal_axis = 'X';
			if(dz < 4 && out->cells[1][dz + 1][dx] == 'P')
				out->portal_axis = 'Z';
		}
	struct AABB box = {body->pos[0] - 0.299F, feet + 0.01F,
		body->pos[2] - 0.299F, body->pos[0] + 0.299F,
		feet + 1.799F, body->pos[2] + 0.299F};
	out->body_collision = world_aabb_intersection(w, &box);
	return true;
}

bool autopilot_portal_return_anchor(const struct entity* body,
	struct autopilot_portal_return_anchor* out) {
	struct autopilot_portal_geometry g;
	if(!out || !autopilot_portal_geometry_sample(body, &g)
	   || g.body_collision || g.portal_axis == '-')
		return false;
	float best = 1000.0F;
	bool found = false;
	for(int dz = 0; dz < 5; dz++)
		for(int dx = 0; dx < 5; dx++) {
			int nx = dx + (g.portal_axis == 'X');
			int nz = dz + (g.portal_axis == 'Z');
			if(nx >= 5 || nz >= 5 || g.cells[1][dz][dx] != 'P'
			   || g.cells[2][dz][dx] != 'P'
			   || g.cells[1][nz][nx] != 'P'
			   || g.cells[2][nz][nx] != 'P')
				continue;
			float mx = g.x + (dx + nx) * 0.5F - 1.5F;
			float mz = g.z + (dz + nz) * 0.5F - 1.5F;
			float score = (mx - body->pos[0]) * (mx - body->pos[0])
				+ (mz - body->pos[2]) * (mz - body->pos[2]);
			if(score >= best)
				continue;
			best = score;
			out->world = body->world;
			out->feet_y = g.feet_y;
			out->x[0] = g.x + dx - 2;
			out->z[0] = g.z + dz - 2;
			out->x[1] = g.x + nx - 2;
			out->z[1] = g.z + nz - 2;
			found = true;
		}
	return found;
}

/* A conservative lower bound: the server begins counting its 1200 Nether
 * updates no later than the client sees COMMIT1. Stalled server ticks cannot
 * satisfy this predicate, regardless of PC/Wii wall-clock speed. */
bool autopilot_portal_return_rearmed(uint32_t observed_tick,
	uint32_t current_tick) {
	return (uint32_t)(current_tick - observed_tick) >= 1200u;
}

/* The runtime script seam's requested walk heading, defined with the rest of
 * the seam at the bottom of this file. Declared here because mode 2 in
 * autopilot_tick reads it, and it returns exactly 0.0F -- mode 2's previous
 * literal -- whenever no scripted driver is asserting the walker, which is
 * every config/profile consumer. */
static float ap_script_walk_yaw(void);
/* Candidate preference for the terrain-safe route probe, defined with the rest
 * of the seam at the bottom of this file. Returns the literal table index --
 * the preference the probe has always used -- for every config consumer. */
static unsigned ap_script_route_rank(const int dirs[][2], unsigned n,
									 unsigned i);
/* "A scripted driver owns the autopilot this frame", defined with the rest of
 * the seam at the bottom of this file. FALSE for every config/profile consumer
 * -- nothing but a scripted driver ever calls autopilot_script_walk/_aim -- so
 * the one branch below that consults it is byte-identical for them. */
static bool ap_script_owns(void);
/* "A scripted driver is asserting the WALK this frame", defined with the rest
 * of the seam at the bottom of this file. Narrower than ap_script_owns() on
 * purpose: the route probe below is the walker's, so only a walk assertion may
 * change it. False for every config/profile consumer. */
static bool ap_script_walking(void);

static bool autopilot_water(struct block_data b) {
	return b.type == BLOCK_WATER_FLOW || b.type == BLOCK_WATER_STILL;
}

static bool autopilot_route_hazard(struct block_data b) {
	return b.type == BLOCK_LAVA_STILL || b.type == BLOCK_LAVA_FLOW
		|| b.type == BLOCK_FIRE || b.type == BLOCK_CACTUS;
}

static bool s_script_portal_walk, s_script_portal_safe;
/* autopilot_walk_pitch_set_deg: 0 = the fixed walk look (every profile). */
static int s_walk_pitch_deg;
static float ap_walk_pitch(void) {
	return s_walk_pitch_deg ? (float)s_walk_pitch_deg * GLM_PI / 180.0F
							: GLM_PI * 0.5F + 0.45F;
}
static bool s_script_portal_sneak;
static struct autopilot_portal_return_anchor s_script_portal_anchor;
static int s_script_portal_target = -1;

static bool autopilot_portal_step(const struct entity* body,
	const struct autopilot_portal_return_anchor* anchor,
	struct autopilot_portal_return_step* out, int target) {
	if(!body || body->on_server || !body->world || !anchor || !out
	   || body->world != anchor->world)
		return false;
	struct world* w = body->world;
	float feet = body->pos[1] - 1.62F;
	if(fabsf(feet - anchor->feet_y) > 0.25F)
		return false;
	int chosen = -1;
	float best = 1000.0F;
	for(int i = 0; i < 2; i++) {
		int x = anchor->x[i], z = anchor->z[i], y = anchor->feet_y;
		if(!world_find_chunk(w, x, y - 1, z)
		   || !world_find_chunk(w, x, y, z)
		   || !world_find_chunk(w, x, y + 1, z)
		   || world_get_block(w, x, y, z).type != BLOCK_PORTAL
		   || world_get_block(w, x, y + 1, z).type != BLOCK_PORTAL)
			return false;
		float dx = x + 0.5F - body->pos[0];
		float dz = z + 0.5F - body->pos[2];
		float d2 = dx * dx + dz * dz;
		if((target < 0 && d2 < best) || target == i) { best = d2; chosen = i; }
	}
	if(chosen < 0 || best > 9.0F)
		return false; /* never chase a displaced body across the Nether */
	out->inside = false;
	for(int i = 0; i < 2; i++)
		if((int)floorf(body->pos[0]) == anchor->x[i]
		   && (int)floorf(body->pos[2]) == anchor->z[i])
			out->inside = true;
	out->target_x = anchor->x[chosen] + 0.5F;
	out->target_z = anchor->z[chosen] + 0.5F;
	float dx = out->target_x - body->pos[0];
	float dz = out->target_z - body->pos[2];
	if(target >= 0)
		out->inside = best < 0.01F;
	out->heading_deg = out->inside ? 0
		: (int)lroundf(atan2f(dx, dz) * 180.0F / GLM_PI);
	/* The 0.6-wide, 1.8-high swept body and a solid, dry footing must remain
	 * installed and safe at every 0.2-block sample. Unknown terrain refuses. */
	int samples = (int)ceilf(sqrtf(best) * 5.0F);
	if(samples < 1)
		samples = 1;
	for(int i = 0; i <= samples; i++) {
		float t = (float)i / (float)samples;
		float x = body->pos[0] + dx * t;
		float z = body->pos[2] + dz * t;
		int min_x = (int)floorf(x - 0.4F), max_x = (int)floorf(x + 0.4F);
		int min_z = (int)floorf(z - 0.4F), max_z = (int)floorf(z + 0.4F);
		for(int bx = min_x; bx <= max_x; bx++)
			for(int bz = min_z; bz <= max_z; bz++)
				for(int by = anchor->feet_y - 1; by <= anchor->feet_y + 1; by++) {
					if(!world_find_chunk(w, bx, by, bz))
						return false;
					struct block_data b = world_get_block(w, bx, by, bz);
					if(autopilot_route_hazard(b) || autopilot_water(b))
						return false;
				}
		/* Half a sample spacing on each side covers the whole inter-sample
		 * sweep, including a diagonal grazing the corner of a solid voxel. */
		struct AABB box = {x - 0.4F, feet + 0.01F, z - 0.4F,
			x + 0.4F, feet + 1.799F, z + 0.4F};
		struct AABB floor = {x - 0.2F, feet - 0.1F, z - 0.2F,
			x + 0.2F, feet, z + 0.2F};
		if(world_aabb_intersection(w, &box)
		   || !world_aabb_intersection(w, &floor))
			return false;
	}
	return true;
}

bool autopilot_portal_return_step(const struct entity* body,
	const struct autopilot_portal_return_anchor* anchor,
	struct autopilot_portal_return_step* out) {
	return autopilot_portal_step(body, anchor, out, -1);
}

/* Explicit endpoint on the same checked pane; used only during cooldown. */
void autopilot_script_portal_patrol(const struct autopilot_portal_return_anchor* a,
	unsigned endpoint) {
	autopilot_script_portal_walk(true, a);
	s_script_portal_target = endpoint < 2 ? (int)endpoint : -1;
}

/* Would a body standing at (x, height, z) be IN a fluid -- either resting on
 * one or wading through one? The footing cell is height-1 and the body occupies
 * height and height+1 (1.8 blocks tall). Lava is covered by the hazard sweep
 * already; naming it here too costs nothing and means this predicate reads as
 * "a fluid" rather than "water, and trust that lava was handled elsewhere".
 *
 * Only ever consulted while a scripted driver asserts the walk -- see the call
 * site in autopilot_route_landing for why the config walkers must keep their
 * swimming behaviour. */
static bool autopilot_route_wet_column(int x, int height, int z) {
	for(int y = height - 1; y <= height + 1; y++) {
		struct block_data b = world_get_block(autopilot_world(), x, y, z);
		if(autopilot_water(b) || b.type == BLOCK_LAVA_STILL
		   || b.type == BLOCK_LAVA_FLOW)
			return true;
	}
	return false;
}

/* Lava/fire have no solid collision box. Sweep the player's full width in XZ,
 * testing segment/slab overlap so even a brief diagonal corner crossing is
 * covered between the solid-body probes. Include the climb/drop height range
 * and supporting cells: a lava surface cannot serve as safe footing. */
static bool autopilot_route_sweep_hazard(struct entity* e, int x, int z,
	float height) {
	float feet = e->pos[1] - 1.62F;
	float start[2] = {e->pos[0], e->pos[2]};
	float end[2] = {x + 0.5F, z + 0.5F};
	int bottom = (int)floorf(fminf(feet, height) - 0.1F);
	int top = (int)floorf(fmaxf(feet, height) + 1.799F);
	int lo[2], hi[2];
	for(unsigned axis = 0; axis < 2; axis++) {
		lo[axis] = (int)floorf(fminf(start[axis], end[axis]) - 0.3F);
		hi[axis] = (int)floorf(fmaxf(start[axis], end[axis]) + 0.3F);
	}
	for(int bx = lo[0]; bx <= hi[0]; bx++) {
		for(int bz = lo[1]; bz <= hi[1]; bz++) {
			int cell[2] = {bx, bz};
			float enter = 0.0F, leave = 1.0F;
			for(unsigned axis = 0; axis < 2; axis++) {
				float min = cell[axis] - 0.3F, max = cell[axis] + 1.3F;
				float delta = end[axis] - start[axis];
				if(delta == 0.0F) {
					if(start[axis] < min || start[axis] > max)
						leave = -1.0F;
				} else {
					float a = (min - start[axis]) / delta;
					float b = (max - start[axis]) / delta;
					enter = fmaxf(enter, fminf(a, b));
					leave = fminf(leave, fmaxf(a, b));
				}
			}
			if(enter > leave)
				continue;
			for(int by = bottom; by <= top; by++)
				if(autopilot_route_hazard(
					   world_get_block(autopilot_world(), bx, by, bz)))
					return true;
		}
	}
	return false;
}

static void autopilot_route_visit(struct autopilot_route_state* r, int x, int z) {
	r->recent_x[r->next] = x;
	r->recent_z[r->next] = z;
	r->next = (r->next + 1) % AUTOPILOT_ROUTE_HISTORY;
	if(r->count < AUTOPILOT_ROUTE_HISTORY)
		r->count++;
}

/* A one-cell controller waypoint needs a known landing, headroom and a clear
 * swept body. At most a one-block climb or two-block drop; no digging, snapping or physics
 * override. Check the raised path too, so a low canopy rejects a jump. */
static bool autopilot_route_landing(struct entity* e, int x, int z, float* y) {
	float feet = e->pos[1] - 1.62F;
	int base = (int)floorf(feet + 0.05F);
	/* Is the body standing somewhere DRY right now? Only consulted by the
	 * scripted-walker fluid rule below, and it is what keeps that rule from
	 * becoming a trap -- see the call site.
	 *
	 * SHORT-CIRCUITED on the seam assertion on purpose: this costs three
	 * world_get_block reads, and this function runs once per candidate, eight
	 * candidates per probe. For every config/profile consumer (far_walk_6m, the
	 * split profiles -- all of them on a single Broadway core) that would be 24
	 * block reads per probe bought for a rule they never evaluate. With the &&
	 * they pay one bool read instead. */
	bool body_dry = ap_script_walking()
		&& !autopilot_route_wet_column((int)floorf(e->pos[0]), base,
									   (int)floorf(e->pos[2]));
	/* THE SCRIPTED WALKER ONLY TAKES STEPS IT CAN TAKE BACK (push-0921/B16).
	 * This probe climbs ONE block and drops TWO, so a walker can enter ground
	 * it can never leave. For the config walkers that is a recorded baseline
	 * and stays exactly as it is. A scripted driver has a ten-minute proof to
	 * finish: PC runs pc_mp_play_20260921_185536 (a cliff pocket) and _194515
	 * (a forest pit, y 71 -> 67) both died of it with nothing dug nearby. Under
	 * the seam a drop is accepted only if the climb back exists -- one block --
	 * which makes every step reversible and the reachable ground closed. */
	int lowest = ap_script_walking() ? base - 1 : base - 2;
	for(int height = base + 1; height >= lowest; height--) {
		if(!world_find_chunk(autopilot_world(), x, height - 1, z))
			continue; /* Unknown cells have a stone collision fallback. */
		if(autopilot_route_sweep_hazard(e, x, z, height))
			continue;
		/* THE SCRIPTED WALKER STAYS DRY (push-0921/B11). The arm below accepts
		 * a column whose footing is WATER, which is right for the config
		 * walkers -- a far_walk_6m run that refused to swim would pin itself on
		 * the first shore -- and wrong for a scripted driver, which has to
		 * stand on solid ground to dig and place. Two runs were lost to it: one
		 * spent 250 s floating in a lake skipping every build leg "in water"
		 * and then died on "crosshair never on the dig cell"; the one before it
		 * dug FROM water and mc173 refused the break twelve times.
		 *
		 * Rejecting the candidate (rather than re-ranking it) is what makes the
		 * rule hold: with every candidate wet the probe ends with no target,
		 * autopilot_probe_route brakes on !has_target, and the core's own
		 * no-progress rule turns the heading 90 degrees on the next leg -- the
		 * escape the two lost runs never had.
		 *
		 * Gated on the WALK assertion, so every config/profile consumer keeps
		 * its exact trajectory (proven in test_autopilot_death_recovery's
		 * off-the-seam case, on the identical flooded world).
		 *
		 * AND GATED ON THE BODY BEING DRY, which is not decoration: without it
		 * the rule is a TRAP that is worse than the behaviour it replaces. A
		 * body dropped in a pond has eight wet neighbours, so every candidate
		 * is rejected, the probe brakes, and FORWARD is gated on !brake -- it
		 * bobs in place forever. Measured: 600 ticks in a 5x5 pond with the
		 * shore three cells away, brake=1 has_target=0, never moved. "Let the
		 * no-progress rule turn the heading" cannot save it either, because
		 * from inside the water EVERY heading is wet. The vertical escape does
		 * survive on its own (route_jump follows gstate.autopilot.water, which
		 * is why the body surfaces), but surfacing is not getting out.
		 *
		 * So: a DRY body may not step into a fluid -- that is the whole point,
		 * and at a shore it still brakes and lets the heading turn, because the
		 * land behind it is dry and normally supplies a candidate anyway. A body
		 * already standing in one may move through it, which is the only way
		 * out. */
		if(body_dry && autopilot_route_wet_column(x, height, z))
			continue; /* body_dry already implies the walk assertion */
		struct AABB floor = {x + 0.25F, height - 0.1F, z + 0.25F,
			x + 0.75F, (float)height, z + 0.75F};
		if(!world_aabb_intersection(autopilot_world(), &floor)
		   && !autopilot_water(world_get_block(autopilot_world(), x, height-1, z)))
			continue;
		bool clear = true, embedded = false;
		for(int step = 0; step <= 4; step++) {
			float t = step * 0.25F;
			float px = e->pos[0] + (x + 0.5F - e->pos[0]) * t;
			float pz = e->pos[2] + (z + 0.5F - e->pos[2]) * t;
			float py = step == 4 ? height : fmaxf(feet, height);
			/* Ignore sub-milliblock contact roundoff at the already clipped
			 * player edge (e.g. 24.3f - 0.3f). The physics box is unchanged. */
			struct AABB body = {px - 0.299F, py + 0.01F, pz - 0.299F,
				px + 0.299F, py + 1.799F, pz + 0.299F};
			if(step == 0 && height <= feet)
				embedded = world_aabb_intersection(autopilot_world(), &body);
			/* A populated tree can enclose the already admitted spawn. Its
			 * current overlap is not evidence that every exit is blocked.
			 * Require a free destination; ordinary collision still resolves
			 * the movement out of the embedded body, with no position write. */
			if((!embedded || step == 4)
			   && world_aabb_intersection(autopilot_world(), &body)) {
				clear = false;
				break;
			}
		}
		if(clear) {
			*y = height;
			return true;
		}
	}
	return false;
}

static void autopilot_probe_route(struct entity* e) {
	struct autopilot_route_state* r = &gstate.autopilot.route;
	float feet = e->pos[1] - 1.62F;
	gstate.autopilot.water = autopilot_water(world_get_block(autopilot_world(),
		(w_coord_t)floorf(e->pos[0]), (w_coord_t)floorf(feet + 0.5F),
		(w_coord_t)floorf(e->pos[2])));
	float dx = r->x - e->pos[0], dz = r->z - e->pos[2];
	if(r->has_target && ((dx*dx + dz*dz < 0.12F
		&& ((e->on_ground && fabsf(feet - r->y) < 0.3F) || gstate.autopilot.water)) || ++r->ticks >= 40
		|| dx*dx + dz*dz > 16.0F)) {
		autopilot_route_visit(r, (int)floorf(r->x), (int)floorf(r->z));
		r->has_target = false;
	}
	if(!r->has_target) {
		int x = (int)floorf(e->pos[0]), z = (int)floorf(e->pos[2]);
		autopilot_route_visit(r, x, z);
		/* Fixed +Z preference, then detours and retreat. Recent-cell cost
		 * prevents both a reverse/forward loop and a repeated unsafe ledge. */
		static const int dirs[8][2] = {{0,1},{-1,1},{1,1},{-1,0},{1,0},
			{-1,-1},{1,-1},{0,-1}};
		unsigned best = UINT32_MAX;
		for(unsigned i = 0; i < 8; i++) {
			int nx = x + dirs[i][0], nz = z + dirs[i][1];
			float height;
			if(!autopilot_route_landing(e, nx, nz, &height))
				continue;
			/* The table's own order for every config consumer; while the
			 * runtime seam owns the walk, the candidate's alignment with the
			 * heading the caller asked for. Same 0..7 domain either way, so
			 * the recent-cell penalty below keeps its exact weight. */
			unsigned cost = ap_script_route_rank(dirs, 8u, i);
			for(unsigned age = 0; age < r->count; age++) {
				unsigned k = (r->next + AUTOPILOT_ROUTE_HISTORY - 1 - age)
					% AUTOPILOT_ROUTE_HISTORY;
				if(r->recent_x[k] == nx && r->recent_z[k] == nz) {
					cost += 8 * (AUTOPILOT_ROUTE_HISTORY - age);
					break;
				}
			}
			if(cost < best) {
				best = cost;
				r->x = nx + 0.5F; r->y = height; r->z = nz + 0.5F;
				r->has_target = true;
			}
		}
		r->ticks = 0;
	}
	dx = r->x - e->pos[0]; dz = r->z - e->pos[2];
	/* Only the live scripted walker crosses a lower waypoint while its body
	 * still rests on the higher edge. Ordinary config walkers retain their
	 * frozen brake here; after the script becomes airborne it brakes above
	 * the verified landing and lets gravity settle it. */
	gstate.autopilot.brake = !r->has_target
		|| (dx*dx + dz*dz < 0.12F && feet > r->y + 0.3F
			&& (!ap_script_walking() || !e->on_ground));
	gstate.autopilot.route_jump = gstate.autopilot.water;
	if(r->has_target) {
		gstate.autopilot.yaw = atan2f(r->x - e->pos[0], r->z - e->pos[2]);
		gstate.autopilot.route_jump |= r->y > feet + 0.1F;
	}
	/* Publish heading with this tick's buttons, before walking physics. Camera
	 * attachment runs only once after a rendered frame's catch-up ticks. */
	e->orient[0] = gstate.autopilot.yaw;
	e->orient[1] = gstate.autopilot.pitch;
}

// One scripted tick: angle the camera down to frame the terrain surface (and the cheap LOD
// skin/skirt mesh), and slowly drift the yaw so the player walks a wide arc -- continuously
// entering NEW chunks (streaming/evict churn) while the view sweeps the surrounding terrain.
void autopilot_tick(void) {
	gstate.autopilot.tick++;

	// Aim mode (debug.autopilot=aim): hold the EXACT look authored from config at init
	// (main.c). Do not drift; force no buttons (see autopilot_force_button) so synthetic
	// input drives move/mine/place while the camera stays locked on the target.
	if(gstate.autopilot.mode == 1) {
		// Optional deterministic block-place: raise a pulse on a schedule. screen_ingame
		// consumes it (autopilot_take_place_pulse) once a block is under the crosshair, so
		// placement needs NO input -- immune to Dolphin Nunchuk/focus flakiness.
		if(gstate.autopilot.place_enabled) {
			int t = gstate.autopilot.tick;
			const int settle = 40; // ~2 s after the world starts ticking
			bool fire = (gstate.autopilot.place_period > 0)
				? (t >= settle && (t - settle) % gstate.autopilot.place_period == 0)
				: (t == settle); // period <= 0 -> a single one-shot place
			// Latch ONE pending place (never accumulate). If a screen switch starves the
			// consumer -- the inventory screen keeps ticks running without pausing -- an
			// unbounded counter would drain as a BURST of places on return; the latch caps it.
			if(fire && gstate.autopilot.pending_place < 1)
				gstate.autopilot.pending_place = 1;
		}
		return;
	}

	int t = gstate.autopilot.tick;

	// Death-screen tracking for autopilot_force_button's Respawn press below (see
	// game_state.h's death_screen_ticks comment for the root cause this fixes).
	// Runs every walk-mode tick regardless of yaw/pitch/unstick state -- entities
	// (and this function) keep ticking on the death screen exactly as they did
	// before the corpse froze in place, so death_screen_ticks tracks real elapsed
	// ticks on that screen with no dependency on anything else below.
	if(gstate.players[0].current_screen == &screen_death)
		gstate.autopilot.death_screen_ticks++;
	else
		gstate.autopilot.death_screen_ticks = 0;

	// Keep-up mode (HW_RUN_PROFILE_KEEPUP_WALK_6M, hw_run.c): a FIXED
	// heading that never drifts with tick, unlike mode 0's t*0.005F curve
	// below -- see autopilot_keepup.h's top comment for why that curve
	// (a constant-curvature ~43-block-radius circle on flat ground) is
	// itself far_walk_6m's dominant source of run-to-run displacement
	// variance. Only a bounded, non-escalating sidestep (never a random or
	// accumulating turn) ever perturbs the heading here. Returns before
	// mode 0's curving-yaw code below so that code path -- and every
	// existing consumer of it (far_walk_6m first) -- stays byte-identical.
	if(gstate.autopilot.mode == 2) {
		gstate.autopilot.pitch = ap_walk_pitch();
		if(s_script_portal_walk) {
			struct autopilot_portal_return_step route;
			s_script_portal_safe = autopilot_portal_step(
				gstate.players[0].local_player, &s_script_portal_anchor,
				&route, s_script_portal_target) && !route.inside;
			s_script_portal_sneak = false;
			if(s_script_portal_safe) {
				struct entity* body = gstate.players[0].local_player;
				gstate.autopilot.yaw
					= route.heading_deg * GLM_PI / 180.0F;
				/* Like the ordinary route probe, publish this tick's heading
				 * before physics, including catch-up ticks between renders. */
				body->orient[0] = gstate.autopilot.yaw;
				body->orient[1] = gstate.autopilot.pitch;
				/* A full-speed step can skip the 0.1-block endpoint window.
				 * Ordinary sneak input brakes the approach without changing
				 * physics, endpoint tolerance, leg count or cooldown deadline. */
				float dx = route.target_x - body->pos[0];
				float dz = route.target_z - body->pos[2];
				s_script_portal_sneak = s_script_portal_target >= 0
					&& dx * dx + dz * dz < 0.25F;
			}
			return; /* exact checked heading, no waypoint/rerank/sidestep */
		}
		// Fixed heading -- never a function of t. 0.0F for every config/profile
		// consumer (keepup_walk_6m), so their trajectory is byte-identical; the
		// runtime seam's requested heading while a scripted driver owns the
		// walker (push-0921/B7, autopilot.h's autopilot_script_walk comment).
		gstate.autopilot.yaw = ap_script_walk_yaw();
		if(gstate.autopilot.terrain_safe && gstate.players[0].local_player) {
			autopilot_probe_route(gstate.players[0].local_player);
			return;
		}
		if(gstate.players[0].local_player) {
			autopilot_keepup_tick(&gstate.autopilot.keepup, t,
								   gstate.players[0].local_player->pos[0],
								   gstate.players[0].local_player->pos[2]);
		}
		gstate.autopilot.yaw += gstate.autopilot.keepup.yaw_offset;
		return;
	}

	// GLM_PI/2 = look level; larger = look down. ~25 deg below level surfaces the ground.
	gstate.autopilot.pitch = ap_walk_pitch();
	// Slow yaw drift (~0.1 rad/s at 20Hz): a wide curving walk that keeps reaching new chunks.
	gstate.autopilot.yaw = (float)t * 0.005F;

	// Opt-in stuck-detector (autopilot_unstick.h; armed ONLY by
	// HW_RUN_PROFILE_MP_LAN_WALK_10M -- see main.c's reset call and
	// hw_run.c's profile case). gstate.autopilot.unstick.armed is false for
	// every other profile/config path, so autopilot_unstick_tick returns
	// immediately without touching *any* field of the state struct, and
	// yaw_offset stays exactly 0.0F -- the `+=` below is then a true
	// no-op (adding 0.0F changes nothing), so far_walk_6m and every other
	// consumer of this function keep their established, byte-identical
	// yaw trajectory.
	if(gstate.autopilot.unstick.armed && gstate.players[0].local_player) {
		autopilot_unstick_tick(&gstate.autopilot.unstick, t,
							   gstate.players[0].local_player->pos[0],
							   gstate.players[0].local_player->pos[2]);
	}
	gstate.autopilot.yaw += gstate.autopilot.unstick.yaw_offset;
}

// Force only the movement buttons. NEVER force a screen-opening button (inventory/pause/place)
// -- a screen switch clears capture_input and would freeze the walk. Unforced buttons return
// false here so input_held resolves them normally (= unpressed in a headless run).
/* host_vehicle_smoke_8m: the script's button LEVELS (see autopilot.h). */
static bool s_script_buttons_armed;
static bool s_script_button_level[IB_COUNT];

static bool ap_script_button_owned(enum input_button b) {
	/* O06: BACKWARD is the cart brake (the rider throttle follows the
	 * current motion, so BACKWARD decelerates a moving cart). */
	return b == IB_ACTION1 || b == IB_ACTION2 || b == IB_JUMP
		|| b == IB_FORWARD || b == IB_BACKWARD || b == IB_GUI_CLICK
		/* Offered-piston fixture: ordinary sideways creep before Join. */
		|| b == IB_RIGHT || b == IB_LEFT || b == IB_SNEAK
		/* REL-11-seam: survival-journey inventory/GUI presses. Claimed only
		 * while the script is armed (autopilot_force_button gates on
		 * s_script_buttons_armed), then read as one rising edge per press. */
		|| b == IB_OPEN_INVENTORY || b == IB_INVENTORY
		|| b == IB_GUI_QUICK_MOVE;
}

void autopilot_script_buttons(bool armed, const bool levels[IB_COUNT]) {
	s_script_buttons_armed = armed;
	for(int b = 0; b < IB_COUNT; b++)
		s_script_button_level[b] = armed && levels && levels[b]
			&& ap_script_button_owned((enum input_button)b);
}

bool autopilot_script_buttons_armed(void) {
	return s_script_buttons_armed;
}

bool autopilot_force_button(enum input_button b, bool* out) {
	/* Before the aim mode's early return: the vehicle smoke script drives
	 * the host's in-game ACTION1/ACTION2/JUMP/FORWARD and the death screen's
	 * held click through the real input path. Unarmed: skipped. */
	if(s_script_buttons_armed && b >= 0 && b < IB_COUNT
	   && ap_script_button_owned(b)) {
		*out = s_script_button_level[b];
		return true;
	}
	// Aim mode forces nothing -> input_held resolves every button from real (synthetic) input.
	if(gstate.autopilot.mode == 1)
		return false;
	if(s_script_portal_walk) {
		if(b == IB_FORWARD) { *out = s_script_portal_safe; return true; }
		if(b == IB_SNEAK) { *out = s_script_portal_sneak; return true; }
		if(b == IB_JUMP) { *out = false; return true; }
	}

	// Death recovery: script the SAME held-click a human uses on screen_death's
	// Respawn button (game_state.h's death_screen_ticks comment has the full root
	// cause). This is the one screen-driving exception to the "never force a
	// screen-opening button" rule below -- it does not OPEN a screen (death opens
	// itself, from fall/drown/combat damage the walk driver doesn't control), it
	// ESCAPES one that would otherwise freeze the walk forever (screen_death.c
	// clears capture_input, so the FORWARD/JUMP forcing further down can never
	// reach movement again once dead). `selected` defaults to 0 == Respawn on
	// entry (screen_death_reset) and nothing here ever presses IB_GUI_UP/DOWN, so
	// the target never changes.
	//
	// screen_death.c requires the click to be seen RELEASED at least once since
	// entry before a press can arm ("fresh-press latch" -- so a button already
	// held from mid-combat can't insta-confirm) -- forcing held=true from tick 0
	// would violate that and NEVER register. But ONE release-then-hold is not
	// enough either: the single edge it produces (tick 1 -> 2, ~100 ms in) falls
	// inside screen_death's entry grace, which discards it, and the level behind
	// it never seeds a hold. So the script RE-PRESSES on a period: released for
	// one tick at tick 1 and at every multiple of AUTOPILOT_DEATH_CLICK_RETRY_-
	// TICKS after it, held otherwise. autopilot.h derives that period from the
	// real grace + hold constants; screen_death's own entry-grace (real time,
	// DEATH_GRACE_S) and hold-to-confirm duration (gui.death_hold_respawn_ms) run
	// unmodified underneath.
	if(b == IB_GUI_CLICK && gstate.players[0].current_screen == &screen_death) {
		/* NO AUTOMATIC RESPAWN UNDER THE SCRIPT SEAM (push-0921/B9; Astra
		 * gate 1 Lane B item 5, docs/mp_play_profile.md section 3: "No
		 * automatic respawn is armed for this profile"). The paragraph above
		 * is written for the CONFIG walker, whose whole job is to keep
		 * walking; a scripted driver is the opposite case -- a death is a
		 * RESULT it must report, and recovering from one would quietly carry
		 * on a run whose contract says it ended. The aim assertion is already
		 * covered by mode 1's early return at the top of this function, but
		 * the walk assertion is not: autopilot_script_walk sets active = true
		 * and mode = 2, which lands squarely in this branch.
		 *
		 * Returning false UNCLAIMS the button rather than forcing it low, so
		 * input_held resolves it normally -- and in an unattended scripted run
		 * that means the driver's own virtual pad, which never asks for
		 * GUI_CLICK outside its inventory phases (those require the inventory
		 * screen to be up, so they cannot be live here). Only this branch is
		 * gated: FORWARD, JUMP and SNEAK below stay claimed, because driving
		 * exactly those is what the seam exists for.
		 *
		 * Byte-identical for every config/profile consumer -- far_walk_6m,
		 * mp_lan_walk_10m, the split profiles and this file's own
		 * test_autopilot_death_recovery cases -- because none of them calls
		 * the seam, so ap_script_owns() is false and this `if` never fires. */
		if(ap_script_owns())
			return false;
		*out = autopilot_death_click_held(
			gstate.autopilot.death_screen_ticks);
		return true;
	}

	int t = gstate.autopilot.tick;

	if(b == IB_FORWARD) {
		*out = (t > 20)
			&& !(gstate.autopilot.terrain_safe && gstate.autopilot.brake);
		return true;
	}
	if(b == IB_JUMP) {
		bool pulse = (t > 20) && (t % 12 == 0); // hop 1-block rises (auto-step does the rest)
		// Keep-up mode ONLY (mode 2): the escape ladder's rung 1 holds jump
		// for its whole duration instead of relying on the every-12th-tick
		// pulse landing on the right phase. Gated on mode == 2 so mode 0's
		// forced-button trajectory (far_walk_6m and every other consumer)
		// stays byte-identical -- keepup.jump_hold is false for them anyway
		// (autopilot_keepup_tick returns immediately while disarmed), but the
		// explicit mode gate makes that independent of the state struct.
		if(gstate.autopilot.mode == 2 && gstate.autopilot.keepup.jump_hold)
			pulse = true;
		if(gstate.autopilot.terrain_safe) {
			pulse = gstate.autopilot.route_jump;
			if(gstate.autopilot.brake && !gstate.autopilot.water)
				pulse = false;
		}
		*out = pulse;
		return true;
	}
	if(b == IB_SNEAK && gstate.autopilot.terrain_safe) {
		struct entity* e = gstate.players[0].local_player;
		/* Let a verified one-block descent leave its upper ledge; sneaking
		 * while grounded would clamp that step forever. Slow only in air. */
		*out = gstate.autopilot.brake || (e && !e->on_ground && gstate.autopilot.route.has_target
			&& gstate.autopilot.route.y < e->pos[1] - 1.62F - 0.1F);
		return true;
	}
	return false;
}

// Consume one pending aim-autopilot place pulse (raised by autopilot_tick). Returns true at
// most once per pulse; screen_ingame ORs it into the block-place edge so a place fires with no
// input. Safe (false) when autopilot is inactive or no pulse is pending.
bool autopilot_take_place_pulse(void) {
	if(gstate.autopilot.active && gstate.autopilot.mode == 1
	   && gstate.autopilot.pending_place > 0) {
		gstate.autopilot.pending_place--;
		return true;
	}
	return false;
}

void autopilot_walk_pitch_set_deg(int pitch_deg) {
	s_walk_pitch_deg = pitch_deg;
}

void autopilot_aim_set_deg(int yaw_deg, int pitch_deg) {
	if(!gstate.autopilot.active || gstate.autopilot.mode != 1)
		return;
	gstate.autopilot.yaw = (float)yaw_deg * GLM_PI / 180.0F;
	gstate.autopilot.pitch = (float)pitch_deg * GLM_PI / 180.0F;
}

void autopilot_request_place(void) {
	if(!gstate.autopilot.active || gstate.autopilot.mode != 1)
		return;
	gstate.autopilot.pending_place = 1;
}

/* ---- the scripted-driver runtime seam (push-0921/B4b) --------------------
 * See autopilot.h for what this is and why the config path cannot serve it.
 * All of it is dead -- every field below stays false/0 and nothing above
 * reads any of it -- until a caller asserts one of the two. */

static bool s_script_walk;	/* the caller wants the walker this frame   */
static bool s_script_aim;	/* the caller wants the aim lock this frame */
static bool s_script_held;	/* the pre-seam autopilot state is saved    */
static bool s_saved_active, s_saved_unstick_armed, s_saved_keepup_armed;
static bool s_saved_terrain_safe;
static int s_saved_mode;
static float s_saved_yaw, s_saved_pitch;
/* The script's requested heading, in RADIANS, engine convention. Only ever
 * read through ap_script_walk_yaw() below, and only while s_script_walk. */
static float s_script_walk_yaw;

/* Remember what the config/profile path had set, ONCE, on the first assertion.
 * Not on every assertion: the seam's own writes would then be saved over the
 * real state and the release would restore the seam instead of the config. */
static void ap_script_save(void) {
	if(s_script_held)
		return;
	s_saved_active = gstate.autopilot.active;
	s_saved_mode = gstate.autopilot.mode;
	s_saved_unstick_armed = gstate.autopilot.unstick.armed;
	s_saved_keepup_armed = gstate.autopilot.keepup.armed;
	s_saved_terrain_safe = gstate.autopilot.terrain_safe;
	s_saved_yaw = gstate.autopilot.yaw;
	s_saved_pitch = gstate.autopilot.pitch;
	s_script_held = true;
}

/* Is the seam in force? True from the first assertion until BOTH are released
 * and ap_script_apply has handed the pre-seam state back -- i.e. exactly while
 * the autopilot state the rest of this file reads is the script's and not the
 * config's. See autopilot_force_button's death branch for the one decision
 * that turns on it. */
static bool ap_script_owns(void) {
	return s_script_held;
}

/* The WALK assertion alone. autopilot_route_landing's fluid rule turns on this
 * rather than on ap_script_owns() because the route probe belongs to the
 * walker: an aim-only assertion must not reshape it. */
static bool ap_script_walking(void) {
	return s_script_walk;
}

/* Mode 2's fixed heading: the script's while it owns the walker, and the exact
 * literal mode 2 always used otherwise. */
static float ap_script_walk_yaw(void) {
	return s_script_walk ? s_script_walk_yaw : 0.0F;
}

/* How well one route candidate points where the script wants to go. */
static float ap_dir_alignment(const int dir[2], float hx, float hz) {
	float len = sqrtf((float)(dir[0] * dir[0] + dir[1] * dir[1]));
	if(len <= 0.0F)
		return -2.0F;
	return ((float)dir[0] * hx + (float)dir[1] * hz) / len;
}

/* THE ROUTE PROBE'S CANDIDATE PREFERENCE.
 *
 * Off the seam this is the identity -- the probe's `dirs` table is ordered
 * +Z first and `cost = i` has always been that order, so every config consumer
 * (split_anchor_6m, split_play_2m/8m, split_actions_6m) keeps its exact
 * trajectory.
 *
 * ON the seam the walk has a HEADING the caller chose and turns 90 degrees
 * itself after a leg without progress, and a hard-coded +Z preference would
 * quietly ignore both -- the probe would walk north whatever the core asked
 * for, which is the reason this seam did not turn terrain_safe on at first.
 * Ranking the candidates by alignment with the requested heading keeps the
 * probe's hazard avoidance (it still only ever proposes a landing it verified)
 * while making the heading mean what it says.
 *
 * Rank = how many candidates are strictly better aligned, with the table index
 * as the tie-break, so the result is a permutation of 0..n-1: the same domain
 * `cost = i` had, which is what keeps the recent-cell penalty's 8*age weight
 * calibrated. */
static unsigned ap_script_route_rank(const int dirs[][2], unsigned n,
									 unsigned i) {
	if(!s_script_walk)
		return i;
	float hx = sinf(s_script_walk_yaw), hz = cosf(s_script_walk_yaw);
	float mine = ap_dir_alignment(dirs[i], hx, hz);
	unsigned rank = 0;
	for(unsigned k = 0; k < n; k++) {
		float other = ap_dir_alignment(dirs[k], hx, hz);
		if(other > mine || (other == mine && k < i))
			rank++;
	}
	return rank;
}

static void ap_script_apply(void) {
	if(!s_script_held)
		return;
	if(s_script_walk) {
		/* Mode 2 is the FIXED-HEADING walker (autopilot_keepup.h): the tick
		 * holds ap_script_walk_yaw() and adds only the escape ladder's bounded
		 * sidestep, and autopilot_force_button forces FORWARD/JUMP. It forces
		 * NOTHING else, so the driver's own pad still carries attack / use /
		 * the GUI roles on the very same frames. See autopilot.h for why this
		 * is mode 2 and not mode 0's curving yaw. */
		gstate.autopilot.active = true;
		gstate.autopilot.mode = 2;
		/* HAZARD AVOIDANCE ON. Without it a fixed heading walks straight off
		 * whatever is in front of it: PC run pc_mp_play_20260921_124707 DIED
		 * of fall damage 12.6 s into measured play, and the two probe runs
		 * before it arrived at the first build leg on 4 of 20 health. The
		 * probe only ever proposes a landing it has verified, and with
		 * ap_script_route_rank it proposes them in the order the requested
		 * heading asks for, so the heading is still honoured. */
		/* Keep the pre-physics tick slot in main.c. The direct branch above
		 * validates and sets input there before this tick's player movement. */
		gstate.autopilot.terrain_safe = true;
	} else if(s_script_aim) {
		gstate.autopilot.active = true;
		gstate.autopilot.mode = 1;
	} else {
		/* Both released: hand the pre-seam state back untouched. */
		gstate.autopilot.active = s_saved_active;
		gstate.autopilot.mode = s_saved_mode;
		gstate.autopilot.yaw = s_saved_yaw;
		gstate.autopilot.pitch = s_saved_pitch;
		autopilot_unstick_reset(&gstate.autopilot.unstick,
								s_saved_unstick_armed);
		autopilot_keepup_reset(&gstate.autopilot.keepup, s_saved_keepup_armed);
		gstate.autopilot.terrain_safe = s_saved_terrain_safe;
		s_script_held = false;
	}
}

void autopilot_script_walk(bool on, int heading_deg) {
	/* The heading is taken on EVERY call, including the frames where the
	 * assertion itself does not change -- the caller's core re-states it every
	 * frame and turns it 90 degrees between legs, and a heading latched only
	 * on the rising edge would silently hold the previous leg's. Normalized by
	 * the caller (mpp_map_engine_yaw_deg); fmodf here so this function is
	 * still correct for any int a future caller hands it. */
	s_script_walk_yaw = fmodf((float)heading_deg, 360.0F) * GLM_PI / 180.0F;
	/* No apply needed when the assertion itself did not change: mode 2 reads
	 * ap_script_walk_yaw() fresh on every tick, so the new heading is in force
	 * from the next tick without touching gstate here. */
	if(on == s_script_walk)
		return;
	if(on) {
		ap_script_save();
		s_script_walk = true;
		/* Arm the escape ladder WITH the walker, and re-seed it on every fresh
		 * leg: autopilot_keepup_tick's first tick after a reset only seeds the
		 * baseline, so a leg that begins where the last one ended starts its
		 * probe window honestly instead of inheriting the previous leg's.
		 *
		 * KEEPUP, not UNSTICK (push-0921/B7): mode 2 returns before
		 * autopilot_tick ever reaches the unstick block, so arming the unstick
		 * detector here would be state nothing reads. The saved value is
		 * restored on release either way, so a config that armed it keeps it. */
		autopilot_keepup_reset(&gstate.autopilot.keepup, true);
	} else {
		s_script_walk = false;
		/* Leaving walk mode: the ladder must not keep accumulating against a
		 * body the driver is deliberately holding still to dig. */
		autopilot_keepup_reset(&gstate.autopilot.keepup, false);
	}
	ap_script_apply();
}

void autopilot_script_portal_walk(bool on,
	const struct autopilot_portal_return_anchor* anchor) {
	s_script_portal_target = -1;
	if(on && anchor) {
		s_script_portal_anchor = *anchor;
		s_script_portal_walk = true;
		s_script_portal_safe = false; /* next tick must validate before motion */
		autopilot_script_walk(true, 0);
		ap_script_apply();
	} else {
		s_script_portal_walk = s_script_portal_safe = false;
		autopilot_script_walk(false, 0);
	}
}

void autopilot_script_look(bool on, float yaw_rad, float pitch_polar_rad) {
	autopilot_script_aim(on, 0, 0);
	if(on && !s_script_walk && gstate.autopilot.active
	   && gstate.autopilot.mode == 1) {
		gstate.autopilot.yaw = yaw_rad;
		gstate.autopilot.pitch = pitch_polar_rad;
	}
}

void autopilot_script_aim(bool on, int yaw_deg, int pitch_down_deg) {
	if(on)
		ap_script_save();
	s_script_aim = on;
	ap_script_apply();
	/* AFTER apply, because autopilot_aim_set_deg refuses unless mode is
	 * already 1 -- and only when the aim is the mode actually in force, since
	 * a walk assertion owns yaw/pitch and would overwrite this on its next
	 * tick anyway. The caller's "positive is down from level" becomes the
	 * camera's polar degrees (90 = level, larger = down). */
	if(on && !s_script_walk)
		autopilot_aim_set_deg(yaw_deg, 90 + pitch_down_deg);
}
