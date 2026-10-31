#include "client_frame_worlds.h"
#include "client_interface.h"
#include "../game/client_world_context.h"

#include "../daytime.h"
#include "../game/game_state.h"

#include <assert.h>
#include <math.h>

struct clin_frame_loading_policy clin_frame_loading_policy(
	const struct clin_frame_loading_state* s) {
	bool allowed=s->payloads&&!s->remote&&!s->paused&&!s->failed;
	bool host_loading=allowed&&s->host_transfer_episode&&s->host_pending
		&&s->host_loading_screen
		&&((s->guest_established
			&&(s->guest_view_bound||s->guest_respawn_loading))
			||(s->guest_transfer_episode&&s->guest_bound
			&&s->guest_attached));
	bool guest_loading=allowed&&s->guest_transfer_episode&&s->guest_bound
		&&s->guest_attached&&(s->guest_held||!s->guest_active)
		&&(host_loading
			||((s->host_ingame_screen||s->host_death_screen)
				&&s->host_loaded&&!s->host_pending&&s->host_established));
	bool host_continues=guest_loading&&!host_loading;
	bool guest_continues=host_loading&&!guest_loading;
	bool global_death_ready=s->host_loaded&&!s->host_pending;
	/* Both can load at once: keep both panes but expose neither owner to input. */
	return (struct clin_frame_loading_policy) {
		.keep_split = host_loading || guest_loading,
		/* A guest respawn can briefly have no bound terrain while the host
		 * transfers. Keep its death/respawn pane without drawing the host world
		 * through the guest's fallback view. */
		.draw_view = {!host_loading,
			!guest_loading && !(host_loading&&s->guest_respawn_loading
				&&!s->guest_view_bound)},
		.guest_transfer_loading = guest_loading,
		.host_continues = host_continues,
		.guest_continues = guest_continues,
		.death_view_allowed = {global_death_ready||host_continues,
			global_death_ready||guest_continues},
	};
}

void clin_frame_worlds_begin(struct clin_frame_worlds* f, unsigned view_count) {
	assert(f && view_count >= 1 && view_count <= 2);
	*f = (struct clin_frame_worlds) {.view_count = view_count};
	f->draw_view[0] = f->draw_view[1] = true;
	f->n_live = clin_world_live_slots(f->live_slot);
	for(unsigned i = 0; i < f->n_live; i++) {
		struct clin_world_payload p;
		bool ok = clin_world_payload_of_slot(f->live_slot[i], &p);
		assert(ok);
		(void)ok;
		f->live[i] = p.terrain;
	}
	/* Storages: flag OFF both slots map to gstate storage -> one entry. */
	f->n_storage = 0;
	for(unsigned s = 0; s < 2; s++) {
		struct clin_world_payload p;
		if(!clin_world_payload_of_slot(s, &p))
			continue;
		bool dup = false;
		for(unsigned i = 0; i < f->n_storage; i++)
			dup |= f->storage[i] == p.terrain;
		if(!dup)
			f->storage[f->n_storage++] = p.terrain;
	}
	for(unsigned v = 0; v < 2; v++) {
		struct clin_view_world scope;
		bool ok = clin_world_view_enter(v, &scope);
		assert(ok);
		(void)ok;
		f->view_slot[v] = scope.slot;
		f->view_terrain[v] = scope.terrain;
		f->view_entities[v] = scope.entities;
		clin_world_view_leave(&scope);
	}
	f->shared = view_count == 1 || f->view_terrain[0] == f->view_terrain[1];
}

bool clin_frame_prepare_respawn(struct clin_frame_worlds* f,
	struct client_session* guest, struct camera* camera) {
	if(!f || !guest || !camera || !clin_world_payloads_enabled()
	   || !client_session_local_respawn_hidden_camera(guest, camera))
		return false;
	unsigned slot = guest->local_respawn_world_slot;
	const struct client_world_context* ctx = slot < CLIENT_CTX_SLOT_COUNT
		? client_ctx_registry_current(clin_world_contexts(), slot) : NULL;
	if(!ctx || ctx->retiring || !guest->local_respawn_world_generation
	   || ctx->server_generation != guest->local_respawn_world_generation
	   || ctx->dimension != guest->local_respawn_dimension
	   || !clin_world_payload_ready(slot))
		return false;
	for(unsigned i = 0; i < f->n_live; i++) {
		if(f->live_slot[i] != slot) continue;
		f->respawn_live_index = i;
		f->respawn_camera = camera;
		return true;
	}
	return false;
}

static void mark_drew(struct clin_frame_worlds* f, const struct world* w) {
	for(unsigned i = 0; i < f->n_storage; i++)
		if(f->storage[i] == w)
			f->drew[i] = true;
}

void clin_frame_pre_render(struct clin_frame_worlds* f,
						   struct camera* const* cameras, bool* render_world) {
	/* A masked storage can still receive build tokens. Drop its previous
	 * frame's scratch list before the builder can walk stale chunk links. */
	for(unsigned i = 0; i < f->n_storage; i++) {
		bool drawable = false;
		for(unsigned v = 0; v < f->view_count; v++)
			drawable |= f->draw_view[v] && f->view_terrain[v] == f->storage[i];
		if(!drawable) world_pre_render_clear(f->storage[i]);
	}
	if(f->shared) {
		/* Pre-B2 main.c, verbatim, on the (single) drawn terrain. */
		struct world* w = f->view_terrain[0];
		if(f->view_count == 1 || !f->draw_view[0] || !f->draw_view[1]) {
			unsigned v = f->view_count == 2 && !f->draw_view[0] ? 1 : 0;
			if(!f->draw_view[v]) return;
			world_pre_render(w, cameras[v], cameras[v]->view);
		}
		else if(!world_pre_render_views(w, cameras,
										f->view_count)) {
			/* Keep both owners and retry next frame; do not silently shrink
			 * a view. */
			world_pre_render_clear(w);
			*render_world = false;
			return;
		}
		mark_drew(f, w);
		return;
	}
	/* Two payloads: each world sees only its own view's camera. */
	for(unsigned v = 0; v < f->view_count; v++) {
		if(!f->draw_view[v]) continue;
		world_pre_render(f->view_terrain[v], cameras[v], cameras[v]->view);
		mark_drew(f, f->view_terrain[v]);
	}
}

void clin_frame_pre_render_clear(struct clin_frame_worlds* f) {
	for(unsigned i = 0; i < f->n_storage; i++) {
		world_pre_render_clear(f->storage[i]);
		f->drew[i] = false;
	}
}

void clin_frame_lighting(const struct clin_frame_worlds* f) {
	for(unsigned i = 0; i < f->n_live; i++)
		world_update_lighting(f->live[i]);
}

void clin_frame_lod(const struct clin_frame_worlds* f,
					struct camera* const* lod_cameras, unsigned lod_view_count) {
	if(f->n_live == 1 && !f->respawn_camera) {
		/* Pre-B2 main.c, verbatim. */
		if(lod_view_count == 1) world_update_lod(f->live[0]);
		else world_update_lod_views(f->live[0], lod_cameras, lod_view_count);
		return;
	}
	for(unsigned i = 0; i < f->n_live; i++) {
		struct camera* mine[2];
		size_t n = 0;
		for(unsigned v = 0; v < lod_view_count && v < 2; v++) {
			/* The dead guest's hidden camera belongs to the respawn world,
			 * even while its visible frozen view still belongs to the source. */
			if(v == 1 && f->respawn_camera) continue;
			if(f->view_terrain[v] == f->live[i])
				mine[n++] = lod_cameras[v];
		}
		if(f->respawn_camera && f->respawn_live_index == i)
			mine[n++] = f->respawn_camera;
		if(n)
			world_update_lod_views(f->live[i], mine, n);
	}
}

static bool frame_viewed(const struct clin_frame_worlds* f,
						 const struct world* w) {
	for(unsigned v = 0; v < f->view_count && v < 2; v++)
		if(f->view_terrain[v] == w)
			return true;
	return false;
}

size_t clin_frame_build(const struct clin_frame_worlds* f, size_t tokens,
						const w_coord_t centers[2][2], int radius,
						uint32_t* first_frame_before,
						int* first_near_mesh_pending) {
	int pending[2] = {0, 0};
	bool build[2] = {false, false};
	for(unsigned i = 0; i < f->n_live; i++) {
		/* Payload 0 (player 0's world) always builds, exactly as before B2.
		 * A second live payload builds only while a view shows it (Fable B2
		 * MED-3: an unviewed world is not meshed, and is never centred on
		 * another world's camera). */
		bool respawn = f->respawn_camera && f->respawn_live_index == i;
		build[i] = i == 0 || frame_viewed(f, f->live[i]) || respawn;
		if(!build[i])
			continue;
		w_coord_t cx = respawn
			? WCOORD_CHUNK_OFFSET((w_coord_t)floorf(f->respawn_camera->x))
			: centers[i][0];
		w_coord_t cz = respawn
			? WCOORD_CHUNK_OFFSET((w_coord_t)floorf(f->respawn_camera->z))
			: centers[i][1];
		world_near_ring_status(f->live[i], cx, cz, radius, NULL, &pending[i]);
	}
	if(first_frame_before) *first_frame_before = f->live[0]->build_frame;
	if(first_near_mesh_pending) *first_near_mesh_pending = pending[0];
	/* Budget (MED-3): with a second VIEWED world that still has near-ring
	 * meshing pending, half the tokens are reserved for it, so a moving
	 * player 0 can never starve the guest's on-screen world; player 0's
	 * unused share rolls over to it. The global bound is unchanged: the
	 * second builder gets exactly tokens minus what the first consumed. One
	 * payload (always the case with the flag OFF): the whole budget, as
	 * before. */
	size_t reserve = f->n_live == 2 && build[1] && pending[1] > 0
		? tokens / 2 : 0;
	size_t first_budget = tokens - reserve;
	size_t first_remaining = world_build_chunks(
		f->live[0], first_budget,
		world_band2_allow_far(pending[0], f->live[0]->build_frame));
	if(f->n_live == 2 && build[1])
		(void)world_build_chunks(
			f->live[1], first_remaining + reserve,
			world_band2_allow_far(pending[1], f->live[1]->build_frame));
	return first_remaining;
}

void clin_frame_render_completed(const struct clin_frame_worlds* f,
								 bool render_world) {
	if(f->n_storage == 1) {
		/* Preserve the original handoff when both views draw. A masked shared
		 * frame has a new GPU reader only if pre-render visited its survivor;
		 * when both owners load, drain the prior lease without repinning the old
		 * render list. */
		bool masked = f->view_count == 2
			&& (!f->draw_view[0] || !f->draw_view[1]);
		world_render_completed(f->storage[0],
			render_world && (!masked || f->drew[0]));
		return;
	}
	/* Every storage, every frame: last frame's pin drains even for a
	 * payload that was not drawn (or was released) this frame. */
	for(unsigned i = 0; i < f->n_storage; i++)
		world_render_completed(f->storage[i], render_world && f->drew[i]);
}

float clin_frame_daytime(uint64_t world_time, ptime_t world_time_start,
						 ptime_t now) {
	return (float)((world_time + time_diff_ms(world_time_start, now)
					   / DAY_TICK_MS)
				   % DAY_LENGTH_TICKS)
		/ (float)DAY_LENGTH_TICKS;
}

void clin_frame_view_env(const struct clin_view_world* scope,
						 float daytime_primary, bool paused, ptime_t now,
						 float daytime_prev, struct clin_view_env* out) {
	/* Flag ON: every view reads ITS world's clock (main.c's daytime comes
	 * from the gstate fields = slot 0's clock, which need not be player 0's
	 * world any more -- R2-3). */
	float daytime = daytime_primary;
	if(clin_world_payloads_enabled())
		daytime = paused ? daytime_prev
						 : clin_frame_daytime(scope->clock.world_time,
											  scope->clock.world_time_start, now);
	bool sky = scope->dimension == WORLD_DIM_OVERWORLD;
	*out = (struct clin_view_env) {
		.daytime = daytime,
		.sky = sky,
		.rain_strength = sky ? scope->clock.rain_strength : 0.0F,
		.thunder_strength = sky ? scope->clock.thunder_strength : 0.0F,
		.dimension = scope->dimension,
	};
}

void clin_frame_teardown_extra(void) {
	if(!clin_world_payloads_enabled())
		return;
	struct clin_world_payload p;
	if(clin_world_payload_of_slot(1, &p) && p.terrain != &gstate.world) {
		/* Fable B2 HIGH-1: the GP may still be reading last frame's slot-1
		 * display lists (this runs before the frame's flip); wait for it
		 * before the hand-off unpins -- and so may free -- them, exactly as
		 * world_finish_local_teardown orders it for gstate.world. */
		world_gpu_wait_idle();
		world_pre_render_clear(p.terrain);
		world_render_completed(p.terrain, false);
	}
}
