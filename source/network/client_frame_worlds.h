#ifndef CLIENT_FRAME_WORLDS_H
#define CLIENT_FRAME_WORLDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../platform/time.h"
#include "../world.h"
#include "client_world_payload.h"

/* Split-dims B2 main.c wiring (B2-MAINC-PLAN.md H7/H9/H10/H11): the per-frame
 * WORLD passes main.c runs, factored out so they are testable and so the
 * flag-OFF path is one reviewed place. Every function reduces, with the
 * world-bound payload flag OFF, to the exact call main.c made before B2 on
 * &gstate.world (argued per function and pinned by test_b2_frame_worlds T1).
 *
 * With the flag ON a frame may hold TWO live payloads: each view draws the
 * payload its context shows (clin_world_view_enter); each live payload gets
 * its own lighting / LOD / mesh dispatch; EVERY payload storage gets its GPU
 * hand-off each frame (so no pin outlives one flip -- test_b2_gpu_lifetime's
 * contract). Owner (main) thread only. */

struct camera;

/* Raw owner state for the transfer-only split continuation policy. Passing the
 * actual reset/ready/episode bits keeps initial loading and guest joining on
 * their established paths. */
struct clin_frame_loading_state {
	bool payloads, remote, paused, failed;
	bool host_transfer_episode, host_pending, host_loading_screen;
	bool host_ingame_screen, host_death_screen, host_loaded, host_active;
	bool host_established;
	bool guest_transfer_episode, guest_held, guest_bound, guest_attached;
	bool guest_active, guest_established, guest_respawn_loading;
	bool guest_view_bound;
};
struct clin_frame_loading_policy {
	bool keep_split;
	bool draw_view[2];
	bool guest_transfer_loading;
	bool host_continues, guest_continues;
	bool death_view_allowed[2];
};
struct clin_frame_loading_policy clin_frame_loading_policy(
	const struct clin_frame_loading_state* state);

struct clin_frame_worlds {
	unsigned view_count;
	/* Payloads to tick / light / LOD / build: player 0's first. */
	unsigned n_live;
	unsigned live_slot[2];
	struct world* live[2];
	/* Every payload STORAGE (flag OFF: gstate.world only; ON: both), for the
	 * GPU hand-off and teardown. */
	unsigned n_storage;
	struct world* storage[2];
	bool drew[2]; /* storage[i] took this frame's references */
	/* The slot / terrain / entity store each view draws, resolved ONCE per
	 * frame here; the draw loop re-enters by view_slot so it draws exactly
	 * what was pre-rendered (Fable LOW-4). */
	unsigned view_slot[2];
	struct world* view_terrain[2];
	struct dict_entity_s* view_entities[2];
	bool draw_view[2]; /* owner-local loading may hide only that owner's world */
	/* Both views draw the same terrain (always true with one view or with
	 * the flag OFF): the two-view union render path applies. */
	bool shared;
	/* A respawn destination can need meshes before its body/view handoff.
	 * This camera affects only LOD/build work, never draw lists or GPU pins. */
	struct camera* respawn_camera;
	unsigned respawn_live_index;
};

void clin_frame_worlds_begin(struct clin_frame_worlds* f, unsigned view_count);
struct client_session;
bool clin_frame_prepare_respawn(struct clin_frame_worlds*,
	struct client_session*, struct camera*);

/* H7: pre-render. Shared: the pre-B2 code on that terrain (single view ->
 * world_pre_render; two -> world_pre_render_views, clearing on failure and
 * turning *render_world off). Separate payloads: world_pre_render per view on
 * its own terrain with its own camera. */
void clin_frame_pre_render(struct clin_frame_worlds* f,
						   struct camera* const* cameras, bool* render_world);
/* The else-branch / recovery clear, on every storage. */
void clin_frame_pre_render_clear(struct clin_frame_worlds* f);

/* H9a: client relight of every live payload. */
void clin_frame_lighting(const struct clin_frame_worlds* f);
/* H9b: LOD. One live payload: the pre-B2 call (lod_view_count == 1 ->
 * world_update_lod, else world_update_lod_views with lod_cameras). Two: each
 * payload with only the cameras of the views that show it (none: skipped). */
void clin_frame_lod(const struct clin_frame_worlds* f,
					struct camera* const* lod_cameras, unsigned lod_view_count);
/* H9c: mesh dispatch. Payload i is centred on centers[i] (chunk index) with
 * the band-2 policy of world_build_chunks. One payload: the whole `tokens`
 * budget (main.c: CHUNK_MESHER_QLENGTH), as before B2. Two: a second payload
 * builds only while a view shows it; while its near ring has meshing
 * pending, tokens/2 is reserved for it and player 0's unused share rolls
 * over (Fable MED-3); the total never exceeds `tokens`. Returns the FIRST
 * payload's remaining tokens (the settle observer's value); its pre-call
 * build_frame is written to *first_frame_before when non-NULL. */
size_t clin_frame_build(const struct clin_frame_worlds* f, size_t tokens,
						const w_coord_t centers[2][2], int radius,
						uint32_t* first_frame_before,
						int* first_near_mesh_pending);
/* H10: GPU hand-off for every storage: the pre-B2 single call
 * world_render_completed(&gstate.world, render_world) when OFF. */
void clin_frame_render_completed(const struct clin_frame_worlds* f,
								 bool render_world);

/* H1/H11: per-view environment. daytime_primary is main.c's own (pre-B2)
 * daytime; with the flag OFF it is returned unchanged and the weather is the
 * gstate fields (the scope's clock snapshot of them), so the sky/fog/light
 * inputs are bit-identical. ON: each view's own clock (daytime_prev while
 * paused, like main.c's daytime) and dimension. */
struct clin_view_env {
	float daytime;
	bool sky;               /* overworld: sky box, weather, clouds */
	float rain_strength;    /* sky ? clock rain : 0 */
	float thunder_strength; /* sky ? clock thunder : 0 */
	int dimension;
};
void clin_frame_view_env(const struct clin_view_world* scope,
						 float daytime_primary, bool paused, ptime_t now,
						 float daytime_prev, struct clin_view_env* out);
/* The pre-B2 daytime expression (main.c "if(!gstate.paused) daytime = ...")
 * for any clock. */
float clin_frame_daytime(uint64_t world_time, ptime_t world_time_start,
						 ptime_t now);

/* H14: before main.c's world_finish_local_teardown(&gstate.world): wait for
 * the GP (gfx_wait_idle), then release the second storage's frame
 * references. No-op with the flag OFF. */
void clin_frame_teardown_extra(void);

#endif
