/* Frozen from bf92f3e33 lighting_flood: independent G9 R6 oracle.
 * Do not edit the relaxation, forced seeds, side order, writes or stops.
 * Reserve/yield/ceiling hooks only connect to the shared failure/pace seam. */
#ifndef G9_FROZEN_FLOOD_H
#define G9_FROZEN_FLOOD_H
#include "lighting.h"
struct g9_frozen_entry { w_coord_t x, y, z; bool forced; };
static inline int8_t g9_frozen_max(int8_t a, int8_t b) { return a > b ? a : b; }
static const int8_t G9_FROZEN_SIDE[SIDE_MAX][3] = {
 [SIDE_TOP]={0,1,0}, [SIDE_BOTTOM]={0,-1,0}, [SIDE_LEFT]={-1,0,0},
 [SIDE_RIGHT]={1,0,0}, [SIDE_FRONT]={0,0,-1}, [SIDE_BACK]={0,0,1}
};
static size_t g9_frozen_flood(
	struct stack* queue, bool ignore_sky_light, size_t max_pops,
	size_t ceiling, enum lighting_drain_result* status,
	struct lighting_drain_stats* st,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	bool (*get_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t* sky, uint8_t* torch),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user) {
	size_t pops = 0;
	if(status)
		*status = LIGHTING_DRAIN_DONE;
	while(!stack_empty(queue)) {
		// Per-call WORK BUDGET (max_pops > 0): suspend the flood with its BFS frontier
		// left INTACT in the queue, so the caller can resume it next frame. This is the
		// client/main-thread relight's freeze fix -- world_update_lighting runs the flood
		// on the render thread, which (unlike the server's yield) cannot advance the
		// main-loop watchdog heartbeat mid-flood, so an unbudgeted multi-second surface
		// skylight flood froze the core (watchdog row1=WDM_LIGHTING). Pacing it is
		// BYTE-EXACT: the queue carries the exact frontier across frames, so the same
		// cells are popped in the same order, reaching the same fixed point -- only the
		// settle RATE changes (proven in tests/test_lighting_client_budget_equiv.c).
		// max_pops == 0 (server + tests) = unbudgeted, byte-identical to before.
		if(max_pops && pops >= max_pops)
			break;
		// Work ceiling: bail on a pathological flood instead of freezing the core /
		// OOM-churning the queue. pops is counted UNCONDITIONALLY here (the old
		// `++pops` lived inside the yield `&&`, so it never counted on the client
		// thread where s_flood_yield is NULL -- exactly the thread that ran away).
		if(++pops > ceiling) {
			lighting_drain_ceiling_hit();
			if(status) { /* lossless: frontier intact, caller stops */
				*status = LIGHTING_DRAIN_CEILING;
				pops--; /* nothing was popped */
			}
			break;
		}
		if(status && !lighting_drain_reserve(queue, stack_size(queue) + 5)) {
			*status = LIGHTING_DRAIN_OOM; /* frontier intact, nothing dropped */
			pops--;
			break;
		}

		// I07 drain telemetry: the frontier peak, read before the pop (one
		// compare per pop; st is NULL on the client / legacy paths).
		if(st && stack_size(queue) > st->depth_peak)
			st->depth_peak = (uint32_t)stack_size(queue);

		struct g9_frozen_entry current;
		stack_pop(queue, &current);

		// A2: hand the core back to render every LIGHTING_FLOOD_YIELD_EVERY pops so a
		// long flood doesn't freeze the frame. Pure pacing -- the queue state is
		// untouched across the yield, so the fixed point is identical.
		if((pops % 256) == 0)
			lighting_drain_yield();

		uint8_t column_height;
		struct block_data old;
		bool old_exists = get_block(user, current.x, current.y, current.z, &old,
									&column_height);
		// A queued cell can be UNLOADED before it is popped when the flood is paced
		// across frames (the client budget): death->respawn evicts the death area, and
		// walking evicts far chunks, between the push and the resume. get_block then
		// returns false WITHOUT writing `old`, so the old assert(old_exists) (a no-op
		// under NDEBUG) let the flood run on garbage -> world_light_set_light deref'd a
		// NULL chunk -> DSI crash. Skip the evicted cell: its column is gone, and a
		// future re-install arrives with final server light (same rule as the deferred-
		// entry stale-column drop in world_update_lighting). For the SYNCHRONOUS server +
		// test floods nothing evicts mid-flood, so old_exists is always true here and
		// this is byte-identical (the branch is never taken).
		if(!old_exists)
			continue;

		uint8_t old_light = (old.torch_light << 4) | old.sky_light;
		uint8_t new_light_sky = 0, new_light_torch = 0;

		if(!ignore_sky_light && current.y >= column_height)
			new_light_sky = 0xF;

		if(blocks[old.type])
			new_light_torch = blocks[old.type]->luminance;

		// I07 lane 3 (T1): the neighbour-existence answers of the read loop
		// are kept for the push loop below. Nothing between the two loops
		// can change them (set_light writes light into a resident chunk; no
		// chunk is loaded or evicted inside a pop), so the push loop asks
		// get_block again only when the read loop did not run (an opaque
		// cell). Same pushes in the same order; six fewer resolves per
		// written see-through cell.
		bool neighbour_known = false;
		uint8_t neighbour_exists = 0;

		if(!blocks[old.type] || blocks[old.type]->can_see_through) {
			neighbour_known = true;
			const int8_t opacity = blocks[old.type] ?
				g9_frozen_max(blocks[old.type]->opacity, 1) :
				1;
			for(enum side s = 0; s < SIDE_MAX; s++) {
				const int x = G9_FROZEN_SIDE[s][0], y = G9_FROZEN_SIDE[s][1],
						  z = G9_FROZEN_SIDE[s][2];

				uint8_t other_sky, other_torch;
				bool other_exists;
				if(get_light) {
					other_exists = get_light(user, current.x + x, current.y + y,
											 current.z + z, &other_sky,
											 &other_torch);
				} else {
					struct block_data other;
					other_exists = get_block(user, current.x + x, current.y + y,
											 current.z + z, &other, NULL);
					other_sky = other.sky_light;
					other_torch = other.torch_light;
				}

				if(other_exists) {
					neighbour_exists |= (uint8_t)(1u << s);

					if(!ignore_sky_light)
						new_light_sky = g9_frozen_max(
							g9_frozen_max((int8_t)other_sky - opacity, 0),
							new_light_sky);
					new_light_torch
						= g9_frozen_max(g9_frozen_max((int8_t)other_torch - opacity, 0),
								 new_light_torch);
				}
			}
		}

		uint8_t new_light = (new_light_torch << 4) | new_light_sky;

		if(old_light != new_light || current.forced) {
			set_light(user, current.x, current.y, current.z, new_light);
			if(st)
				st->writes++;

			for(enum side s = 0; s < SIDE_MAX; s++) {
				const int x = G9_FROZEN_SIDE[s][0], y = G9_FROZEN_SIDE[s][1],
						  z = G9_FROZEN_SIDE[s][2];

				const bool exists = neighbour_known ?
					(neighbour_exists & (1u << s)) != 0 :
					get_block(user, current.x + x, current.y + y,
							  current.z + z, NULL, NULL);
				if(exists) {
					if(st)
						st->pushes++;
					stack_push(queue,
							   &(struct g9_frozen_entry) {
								   .x = current.x + x,
								   .y = current.y + y,
								   .z = current.z + z,
								   .forced = false,
							   });
				}
			}
		}
	}
	return pops;
}

static enum lighting_drain_result g9_frozen_kernel(
	struct stack* q, bool ignore_sky_light, size_t ceiling, size_t* pops,
	struct lighting_drain_stats* st,
	bool (*get_block)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk, uint8_t* height),
	bool (*get_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t* sky, uint8_t* torch),
	void (*set_light)(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
					  uint8_t light),
	void* user) {
 enum lighting_drain_result r = LIGHTING_DRAIN_DONE;
 if(st) *st = (struct lighting_drain_stats){0};
 *pops = g9_frozen_flood(q, ignore_sky_light, 0, ceiling, &r, st,
                       get_block, get_light, set_light, user);
 return r;
}
#endif
