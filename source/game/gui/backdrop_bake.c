/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../../block/blocks_data.h"
#include "../../cglm/cglm.h"
#include "../../network/server_world.h" /* struct server_chunk + column arrays */
#include "../../platform/mem2.h"
#include "../../platform/time.h"
#include "../../util.h"
#include "../../worldgen/populate.h"
#include "../../worldgen/worldgen.h"
#include "backdrop_bake.h"

/* Storage-free scene adapter (FEASIBILITY blocker 3). The integrated server
 * world cannot host the scene without touching storage (its create removes a
 * journal, its loads go through the region archive, its destroy persists), so
 * the bake owns its columns here and drives the SAME generator entry points
 * the server uses: worldgen_generate_chunk_provider (terrain + per-column
 * light), worldgen_populate over each 2x2 quadrant through a wgen_world view,
 * then worldgen_lighting(reset_torch=false) per column (the server's
 * post-population rebuild). Nothing here opens a file.
 *
 * Differences from a live server world, all render-side (Bucket B) and
 * recorded for the task-4 visual comparison, none a simulation change:
 *   - tick_fluid_immediate is NULL (populate.h documents NULL as the
 *     offline-view contract): a WorldGenLiquids spring is placed as its source
 *     block but does not run its immediate updateTick, so a visible spring
 *     shows as one source block instead of a short fall;
 *   - set_chest / set_spawner are NULL (RNG still consumed, nothing stored);
 *   - quadrants populate in a fixed row-major order rather than the
 *     server's player-driven order, so cross-quadrant overlap resolves in a
 *     fixed (deterministic) order;
 *   - cross-column light is settled by the client's lossless light repair
 *     (world_light_repair_mark -> fixed point) in the host, not by the
 *     server's scheduled Beta light queue. */

#define BB_COL_IDX(lx, y, lz) \
	((size_t)(y) + ((size_t)(lz) + (size_t)(lx) * CHUNK_SIZE) * WORLD_HEIGHT)

static const struct backdrop_bake_recipe s_default_recipe = {
	.seed = 42,
	.eye = {0.5F, 68.62F, 64.5F},
	.yaw_deg = 270.0F,
	.pitch_deg = 94.0F,
	.world_time = 6000,
	.fov_deg = 70.0F,
	.frame_w = 640,
	.frame_h = 480,
	.render_distance = 11 * 16.0F + 32.0F,
	.fog_distance = 11 * 16.0F,
	.fog_clip = 1.13F,
	.lod_near_radius = 2,
	.view_radius = 10,
	.crop_x0 = 0,
	.crop_y0 = 96,
	.crop_x1 = 640,
	.crop_y1 = 456,
};

const struct backdrop_bake_recipe* backdrop_bake_default_recipe(void) {
	return &s_default_recipe;
}

static unsigned s_fail_alloc_after;

void backdrop_bake_test_fail_alloc_after(unsigned n) {
	s_fail_alloc_after = n;
}

static bool bb_alloc_allowed(void) {
	if(!s_fail_alloc_after) return true;
	if(--s_fail_alloc_after == 0) return false;
	return true;
}

struct bb_column {
	struct server_chunk sc; /* only the five array pointers are used */
	uint8_t* tint;          /* grass_i[256] then grass_j[256] */
	bool member;            /* in the terrain set T */
	bool visible;           /* in the frustum set V */
	bool generated;
	bool installed;
};

struct backdrop_bake {
	struct backdrop_bake_host host;
	struct backdrop_bake_recipe recipe;
	enum backdrop_bake_state state;
	enum backdrop_bake_error error;
	bool cancel_requested;
	bool scene_open;
	struct worldgen* wg;

	int min_cx, min_cz, span_x, span_z; /* bounding box of T */
	struct bb_column* cols;             /* span_x * span_z */
	size_t cols_bytes;
	unsigned cursor;                    /* per-phase linear cursor */
	/* quadrant origins (bounding box of Q, flagged) */
	int qmin_cx, qmin_cz, qspan_x, qspan_z;
	bool* quad;
	size_t quad_bytes;

	uint8_t* out;      /* BACKDROP_BAKE_OUT_W x _H x 4 */
	size_t out_row;    /* next output row the sink must produce */
	size_t src_crop[4];/* x0, y0, w, h in source pixels */
	bool sink_geom_set;
	bool sink_failed;

	struct backdrop_bake_report rep;
};

static void bb_account(struct backdrop_bake* b, long delta) {
	if(delta < 0) b->rep.owned_bytes -= (size_t)(-delta);
	else b->rep.owned_bytes += (size_t)delta;
	if(b->rep.owned_bytes > b->rep.owned_peak_bytes)
		b->rep.owned_peak_bytes = b->rep.owned_bytes;
}

static struct bb_column* bb_col(struct backdrop_bake* b, int cx, int cz) {
	int x = cx - b->min_cx, z = cz - b->min_cz;
	if(x < 0 || z < 0 || x >= b->span_x || z >= b->span_z) return NULL;
	struct bb_column* c = &b->cols[(size_t)x * b->span_z + z];
	return c->member ? c : NULL;
}

static struct bb_column* bb_col_gen(struct backdrop_bake* b, int cx, int cz) {
	struct bb_column* c = bb_col(b, cx, cz);
	return c && c->generated ? c : NULL;
}

#define BB_COLUMN_BYTES (SERVER_CHUNK_IDS_BYTES + 3 * SERVER_CHUNK_NIBBLE_BYTES \
	+ SERVER_CHUNK_HEIGHTMAP_BYTES)

static void bb_column_release(struct backdrop_bake* b, struct bb_column* c) {
	if(c->generated) {
		server_chunk_arrays_free(&c->sc);
		c->generated = false;
		bb_account(b, -(long)BB_COLUMN_BYTES);
	}
	if(c->tint) {
		free(c->tint);
		c->tint = NULL;
		bb_account(b, -512);
	}
}

static void bb_release_all(struct backdrop_bake* b) {
	if(b->scene_open && b->host.scene_end) {
		b->host.scene_end(b->host.user);
	}
	b->scene_open = false;
	if(b->cols) {
		for(size_t i = 0; i < (size_t)b->span_x * b->span_z; i++)
			bb_column_release(b, &b->cols[i]);
		free(b->cols);
		b->cols = NULL;
		bb_account(b, -(long)b->cols_bytes);
	}
	if(b->quad) {
		free(b->quad);
		b->quad = NULL;
		bb_account(b, -(long)b->quad_bytes);
	}
	if(b->out) {
		mem2_free_sized(b->out, (size_t)BACKDROP_BAKE_OUT_W * BACKDROP_BAKE_OUT_H * 4);
		b->out = NULL;
		bb_account(b, -(long)((size_t)BACKDROP_BAKE_OUT_W * BACKDROP_BAKE_OUT_H * 4));
	}
	if(b->wg) {
		worldgen_destroy(b->wg);
		b->wg = NULL;
	}
}

static enum backdrop_bake_state bb_fail(struct backdrop_bake* b,
	enum backdrop_bake_error e) {
	bb_release_all(b);
	b->error = e;
	b->state = e == BACKDROP_BAKE_ERR_CANCELLED ? BACKDROP_BAKE_CANCELLED
												: BACKDROP_BAKE_FAILED;
	return b->state;
}

/* ---- the populate world view over bake-owned columns ---- */

struct bb_view {
	struct backdrop_bake* b;
	int px, pz; /* quadrant origin chunk */
	struct wgen_world* self;
};

static struct server_chunk* bbv_chunk(struct bb_view* v, int x, int z) {
	struct bb_column* c = bb_col_gen(v->b, WCOORD_CHUNK_OFFSET(x),
		WCOORD_CHUNK_OFFSET(z));
	return c ? &c->sc : NULL;
}

static uint8_t bbv_get(void* ctx, int x, int y, int z) {
	if(y < 0 || y >= WORLD_HEIGHT) return 0;
	struct server_chunk* sc = bbv_chunk(ctx, x, z);
	return sc ? sc->ids[BB_COL_IDX(W2C_COORD(x), y, W2C_COORD(z))] : 0;
}

static uint8_t bbv_get_meta(void* ctx, int x, int y, int z) {
	if(y < 0 || y >= WORLD_HEIGHT) return 0;
	struct server_chunk* sc = bbv_chunk(ctx, x, z);
	return sc ? nibble_read(sc->metadata,
		BB_COL_IDX(W2C_COORD(x), y, W2C_COORD(z))) : 0;
}

/* Chunk.setBlockID[WithMetadata] semantics as quad_set_kind_body applies
 * them to the raw arrays: unchanged id (+meta for the metadata API) is a
 * no-op, the old block's leaves/log removal callback runs, metadata is
 * stored after. Light is rebuilt per column once population is complete. */
static void bbv_set_kind(struct bb_view* v, int x, int y, int z, uint8_t id,
	uint8_t meta, bool plain) {
	if(y < 0 || y >= WORLD_HEIGHT) return;
	struct server_chunk* sc = bbv_chunk(v, x, z);
	if(!sc) return;
	size_t idx = BB_COL_IDX(W2C_COORD(x), y, W2C_COORD(z));
	uint8_t old = sc->ids[idx];
	uint8_t old_meta = nibble_read(sc->metadata, idx);
	if(old == id && (plain || old_meta == meta)) return;
	sc->ids[idx] = id;
	if(old == BLOCK_LEAVES)
		wgen_leaves_on_removal(v->self, v->px, v->pz, x, y, z);
	if(old == BLOCK_LOG)
		wgen_log_on_removal(v->self, v->px, v->pz, x, y, z);
	nibble_write(sc->metadata, idx, plain ? 0 : meta);
}

static void bbv_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	bbv_set_kind(ctx, x, y, z, id, meta, false);
}

static void bbv_set_plain(void* ctx, int x, int y, int z, uint8_t id) {
	bbv_set_kind(ctx, x, y, z, id, 0, true);
}

static void bbv_set_meta(void* ctx, int x, int y, int z, uint8_t meta) {
	if(y < 0 || y >= WORLD_HEIGHT) return;
	struct server_chunk* sc = bbv_chunk(ctx, x, z);
	if(sc) nibble_write(sc->metadata,
		BB_COL_IDX(W2C_COORD(x), y, W2C_COORD(z)), meta);
}

static int bbv_height(void* ctx, int x, int z) {
	struct server_chunk* sc = bbv_chunk(ctx, x, z);
	return sc ? sc->heightmap[W2C_COORD(x) + W2C_COORD(z) * CHUNK_SIZE] : 0;
}

static int bbv_skylight(void* ctx, int x, int y, int z) {
	if(y >= WORLD_HEIGHT) return 15;
	if(y < 0) return 0;
	struct server_chunk* sc = bbv_chunk(ctx, x, z);
	return sc ? nibble_read(sc->lighting_sky,
		BB_COL_IDX(W2C_COORD(x), y, W2C_COORD(z))) : 0;
}

static int bbv_full_light(void* ctx, int x, int y, int z) {
	if(y < 0 || y >= WORLD_HEIGHT) return 0;
	struct server_chunk* sc = bbv_chunk(ctx, x, z);
	if(!sc) return 0;
	size_t idx = BB_COL_IDX(W2C_COORD(x), y, W2C_COORD(z));
	int s = nibble_read(sc->lighting_sky, idx);
	int t = nibble_read(sc->lighting_torch, idx);
	return s > t ? s : t;
}

static bool bbv_chunks_exist(void* ctx, int x1, int y1, int z1, int x2, int y2,
	int z2) {
	struct bb_view* v = ctx;
	if(y2 < 0 || y1 >= WORLD_HEIGHT) return false;
	for(int cx = WCOORD_CHUNK_OFFSET(x1); cx <= WCOORD_CHUNK_OFFSET(x2); cx++)
		for(int cz = WCOORD_CHUNK_OFFSET(z1); cz <= WCOORD_CHUNK_OFFSET(z2); cz++)
			if(!bb_col_gen(v->b, cx, cz)) return false;
	return true;
}

/* ---- scene extent ---- */

static void bb_camera(const struct backdrop_bake_recipe* r, mat4 view,
	mat4 proj) {
	/* camera_update_viewport's exact projection/look-at construction. */
	float rx = glm_rad(r->yaw_deg), ry = glm_rad(r->pitch_deg);
	glm_perspective(glm_rad(r->fov_deg), (float)r->frame_w / (float)r->frame_h,
		0.075F, r->render_distance, proj);
	glm_lookat((vec3) {r->eye[0], r->eye[1], r->eye[2]},
		(vec3) {r->eye[0] + sinf(rx) * sinf(ry), r->eye[1] + cosf(ry),
			r->eye[2] + cosf(rx) * sinf(ry)},
		(vec3) {0, 1, 0}, view);
}

static bool bb_column_in_view(const struct backdrop_bake_recipe* r,
	vec4 planes[6], int cx, int cz) {
	/* world.c FOG_DIST_LESS(c, cam, fog_clip): the renderer never draws a
	 * chunk whose centre is beyond fog_clip * fog_distance. */
	float dx = cx * CHUNK_SIZE + CHUNK_SIZE / 2 - r->eye[0];
	float dz = cz * CHUNK_SIZE + CHUNK_SIZE / 2 - r->eye[2];
	float reach = r->fog_clip * r->fog_distance;
	if(dx * dx + dz * dz > reach * reach) return false;
	vec3 box[2] = {{cx * CHUNK_SIZE, 0, cz * CHUNK_SIZE},
		{cx * CHUNK_SIZE + CHUNK_SIZE, WORLD_HEIGHT, cz * CHUNK_SIZE + CHUNK_SIZE}};
	return glm_aabb_frustum(box, planes);
}

static bool bb_plan(struct backdrop_bake* b) {
	const struct backdrop_bake_recipe* r = &b->recipe;
	mat4 view, proj, vp;
	vec4 planes[6];
	bb_camera(r, view, proj);
	glm_mat4_mul(proj, view, vp);
	glm_frustum_planes(vp, planes);

	int ecx = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(r->eye[0]));
	int ecz = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(r->eye[2]));
	int R = r->view_radius;
	/* V within [ecx-R, ecx+R]; quadrants q in V-1..V; T in q..q+1, so the
	 * terrain box is V's box grown by one chunk each side. */
	b->min_cx = ecx - R - 1;
	b->min_cz = ecz - R - 1;
	b->span_x = 2 * R + 3;
	b->span_z = 2 * R + 3;
	b->qmin_cx = ecx - R - 1;
	b->qmin_cz = ecz - R - 1;
	b->qspan_x = 2 * R + 2;
	b->qspan_z = 2 * R + 2;

	size_t n = (size_t)b->span_x * b->span_z;
	size_t nq = (size_t)b->qspan_x * b->qspan_z;
	if(!bb_alloc_allowed()) return false;
	b->cols = calloc(n, sizeof(*b->cols));
	if(!b->cols) return false;
	b->cols_bytes = n * sizeof(*b->cols);
	bb_account(b, (long)b->cols_bytes);
	if(!bb_alloc_allowed()) return false;
	b->quad = calloc(nq, sizeof(*b->quad));
	if(!b->quad) return false;
	b->quad_bytes = nq * sizeof(*b->quad);
	bb_account(b, (long)b->quad_bytes);

	for(int cx = ecx - R; cx <= ecx + R; cx++)
		for(int cz = ecz - R; cz <= ecz + R; cz++) {
			if(!bb_column_in_view(r, planes, cx, cz)) continue;
			b->cols[(size_t)(cx - b->min_cx) * b->span_z + (cz - b->min_cz)].visible
				= true;
			b->rep.columns_visible++;
			for(int qx = cx - 1; qx <= cx; qx++)
				for(int qz = cz - 1; qz <= cz; qz++)
					b->quad[(size_t)(qx - b->qmin_cx) * b->qspan_z
						+ (qz - b->qmin_cz)] = true;
		}
	for(size_t i = 0; i < nq; i++) {
		if(!b->quad[i]) continue;
		b->rep.quads_total++;
		int qx = b->qmin_cx + (int)(i / b->qspan_z);
		int qz = b->qmin_cz + (int)(i % b->qspan_z);
		for(int dx = 0; dx < 2; dx++)
			for(int dz = 0; dz < 2; dz++)
				b->cols[(size_t)(qx + dx - b->min_cx) * b->span_z
					+ (qz + dz - b->min_cz)].member = true;
	}
	for(size_t i = 0; i < n; i++)
		if(b->cols[i].member) b->rep.columns_total++;
	return b->rep.columns_visible > 0;
}

static bool bb_recipe_valid(const struct backdrop_bake_recipe* r) {
	return r && r->frame_w > 0 && r->frame_h > 0 && r->fov_deg > 0
		&& r->render_distance > 0 && r->fog_distance > 0 && r->fog_clip > 0
		&& r->view_radius >= 0 && r->view_radius <= 16
		&& r->crop_x0 >= 0 && r->crop_y0 >= 0 && r->crop_x1 > r->crop_x0
		&& r->crop_y1 > r->crop_y0 && r->crop_x1 <= r->frame_w
		&& r->crop_y1 <= r->frame_h;
}

struct backdrop_bake* backdrop_bake_create(const struct backdrop_bake_host* host,
	const struct backdrop_bake_recipe* recipe, enum backdrop_bake_error* err) {
	enum backdrop_bake_error dummy;
	if(!err) err = &dummy;
	*err = BACKDROP_BAKE_OK;
	if(!recipe) recipe = &s_default_recipe;
	if(!host || !host->scene_begin || !host->install_column
	   || !host->settle_step || !host->render_capture || !host->scene_end
	   || !bb_recipe_valid(recipe)) {
		*err = BACKDROP_BAKE_ERR_SCENE;
		return NULL;
	}
	if(host->world_alive && host->world_alive(host->user)) {
		*err = BACKDROP_BAKE_ERR_WORLD_ALIVE;
		return NULL;
	}
	if(!bb_alloc_allowed()) {
		*err = BACKDROP_BAKE_ERR_ALLOC;
		return NULL;
	}
	struct backdrop_bake* b = calloc(1, sizeof(*b));
	if(!b) {
		*err = BACKDROP_BAKE_ERR_ALLOC;
		return NULL;
	}
	b->host = *host;
	b->recipe = *recipe;
	b->state = BACKDROP_BAKE_TERRAIN;
	if(!bb_plan(b)) {
		bb_release_all(b);
		free(b);
		*err = BACKDROP_BAKE_ERR_ALLOC;
		return NULL;
	}
	b->wg = worldgen_create(recipe->seed, WORLD_DIM_OVERWORLD);
	if(!b->wg) {
		bb_release_all(b);
		free(b);
		*err = BACKDROP_BAKE_ERR_ALLOC;
		return NULL;
	}
	return b;
}

/* ---- phases ---- */

static size_t bb_ncols(const struct backdrop_bake* b) {
	return (size_t)b->span_x * b->span_z;
}

/* One column: true = progress (or skipped non-member), false = failed. */
static bool bb_terrain_one(struct backdrop_bake* b, size_t i) {
	struct bb_column* c = &b->cols[i];
	if(!c->member) return true;
	int cx = b->min_cx + (int)(i / b->span_z);
	int cz = b->min_cz + (int)(i % b->span_z);
	if(!bb_alloc_allowed()) {
		b->error = BACKDROP_BAKE_ERR_ALLOC;
		return false;
	}
	memset(&c->sc, 0, sizeof(c->sc));
	if(!worldgen_generate_chunk_provider(b->wg, &c->sc, cx, cz, NULL)) {
		server_chunk_arrays_free(&c->sc);
		b->error = BACKDROP_BAKE_ERR_GENERATE;
		return false;
	}
	c->generated = true;
	b->rep.columns_generated++;
	bb_account(b, (long)BB_COLUMN_BYTES);
	return true;
}

static void bb_populate_one(struct backdrop_bake* b, size_t i) {
	if(!b->quad[i]) return;
	int qx = b->qmin_cx + (int)(i / b->qspan_z);
	int qz = b->qmin_cz + (int)(i % b->qspan_z);
	struct bb_view v = {.b = b, .px = qx, .pz = qz};
	struct wgen_world view = {
		.ctx = &v,
		.get = bbv_get,
		.set = bbv_set,
		.height = bbv_height,
		.skylight = bbv_skylight,
		.get_meta = bbv_get_meta,
		.set_plain = bbv_set_plain,
		.full_light = bbv_full_light,
		.set_meta = bbv_set_meta,
		.chunks_exist = bbv_chunks_exist,
	};
	v.self = &view;
	/* server_world.c's populate call: corner biome at +16,+16, tree density
	 * at the block origin. */
	enum biome corner = worldgen_biome_at(b->wg, qx * 16 + 16, qz * 16 + 16);
	double density = worldgen_tree_density(b->wg, qx * 16, qz * 16);
	worldgen_populate(b->recipe.seed, qx, qz, &view, corner, density);
	b->rep.quads_done++;
}

static bool bb_install_one(struct backdrop_bake* b, size_t i) {
	struct bb_column* c = &b->cols[i];
	if(!c->member) return true;
	int cx = b->min_cx + (int)(i / b->span_z);
	int cz = b->min_cz + (int)(i % b->span_z);
	worldgen_lighting(&c->sc, false, WORLD_DIM_OVERWORLD);
	if(!bb_alloc_allowed()) {
		b->error = BACKDROP_BAKE_ERR_ALLOC;
		return false;
	}
	c->tint = malloc(512);
	if(!c->tint) {
		b->error = BACKDROP_BAKE_ERR_ALLOC;
		return false;
	}
	bb_account(b, 512);
	worldgen_climate(b->wg, cx, cz, c->tint, c->tint + 256);
	if(!b->host.install_column(b->host.user, cx, cz, c->sc.ids,
		   c->sc.metadata, c->sc.lighting_sky, c->sc.lighting_torch,
		   c->sc.heightmap, c->tint, c->tint + 256, c->visible)) {
		b->error = BACKDROP_BAKE_ERR_INSTALL;
		return false;
	}
	c->installed = true;
	b->rep.columns_installed++;
	/* Population is complete and the host copied the column: drop the
	 * bake-owned payload now so the peak is max(T payload, host copy)
	 * rather than their sum. */
	bb_column_release(b, c);
	return true;
}

bool backdrop_bake_world_alive(const struct backdrop_bake_world_facts* f) {
	return !f || f->remote_provider || f->server_owns_world
		|| f->client_sections > 0 || (!f->at_title && !f->loading_frontend);
}

size_t backdrop_bake_nearest_src(size_t o, size_t out_len, size_t crop0,
	size_t crop_len) {
	assert(out_len);
	return crop0 + ((2 * o + 1) * crop_len) / (2 * out_len);
}

static bool bb_sink(void* user, const uint8_t* rgba, size_t y0, size_t rows,
	size_t width, size_t height) {
	struct backdrop_bake* b = user;
	if(b->cancel_requested) return false;
	if(!rgba || !width || !height || y0 + rows > height) {
		b->sink_failed = true;
		return false;
	}
	if(!b->sink_geom_set) {
		/* Reference crop scaled to the delivered frame (a PAL EFB or a PC
		 * window that is not 640x480 still yields the same aperture). */
		const struct backdrop_bake_recipe* r = &b->recipe;
		size_t x0 = (size_t)r->crop_x0 * width / r->frame_w;
		size_t x1 = (size_t)r->crop_x1 * width / r->frame_w;
		size_t sy0 = (size_t)r->crop_y0 * height / r->frame_h;
		size_t sy1 = (size_t)r->crop_y1 * height / r->frame_h;
		if(x1 <= x0 || sy1 <= sy0) {
			b->sink_failed = true;
			return false;
		}
		b->src_crop[0] = x0;
		b->src_crop[1] = sy0;
		b->src_crop[2] = x1 - x0;
		b->src_crop[3] = sy1 - sy0;
		b->sink_geom_set = true;
	}
	while(b->out_row < BACKDROP_BAKE_OUT_H) {
		size_t sy = backdrop_bake_nearest_src(b->out_row, BACKDROP_BAKE_OUT_H,
			b->src_crop[1], b->src_crop[3]);
		if(sy < y0) {
			/* rows must arrive in order; a skipped band is a partial frame */
			b->sink_failed = true;
			return false;
		}
		if(sy >= y0 + rows) break;
		const uint8_t* src = rgba + (sy - y0) * width * 4;
		uint8_t* dst = b->out + b->out_row * BACKDROP_BAKE_OUT_W * 4;
		for(size_t ox = 0; ox < BACKDROP_BAKE_OUT_W; ox++) {
			size_t sx = backdrop_bake_nearest_src(ox, BACKDROP_BAKE_OUT_W,
				b->src_crop[0], b->src_crop[2]);
			dst[ox * 4 + 0] = src[sx * 4 + 0];
			dst[ox * 4 + 1] = src[sx * 4 + 1];
			dst[ox * 4 + 2] = src[sx * 4 + 2];
			dst[ox * 4 + 3] = 255; /* opaque backdrop; EFB alpha is not colour */
		}
		b->out_row++;
	}
	return true;
}

static uint32_t bb_elapsed_us(ptime_t start) {
	uint64_t us = time_diff_us(start, time_get());
	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

enum backdrop_bake_state backdrop_bake_step(struct backdrop_bake* b,
	uint32_t budget_us) {
	if(!b) return BACKDROP_BAKE_FAILED;
	if(b->state == BACKDROP_BAKE_DONE || b->state == BACKDROP_BAKE_FAILED
	   || b->state == BACKDROP_BAKE_CANCELLED)
		return b->state;
	if(b->cancel_requested)
		return bb_fail(b, BACKDROP_BAKE_ERR_CANCELLED);
	/* A world appearing mid-bake (should not happen: the caller only steps
	 * at the title) ends the bake before it can touch shared state. */
	if(b->host.world_alive && b->host.world_alive(b->host.user))
		return bb_fail(b, BACKDROP_BAKE_ERR_WORLD_ALIVE);

	ptime_t start = time_get();
	enum backdrop_bake_state phase = b->state;
	bool first = true;
	while(first || bb_elapsed_us(start) < budget_us) {
		first = false;
		if(b->state == BACKDROP_BAKE_TERRAIN) {
			if(b->cursor >= bb_ncols(b)) {
				b->state = BACKDROP_BAKE_POPULATE;
				b->cursor = 0;
				break;
			}
			if(!bb_terrain_one(b, b->cursor++))
				return bb_fail(b, b->error);
		} else if(b->state == BACKDROP_BAKE_POPULATE) {
			size_t nq = (size_t)b->qspan_x * b->qspan_z;
			if(b->cursor >= nq) {
				if(!b->host.scene_begin(b->host.user, &b->recipe))
					return bb_fail(b, BACKDROP_BAKE_ERR_SCENE);
				b->scene_open = true;
				b->state = BACKDROP_BAKE_INSTALL;
				b->cursor = 0;
				break;
			}
			bb_populate_one(b, b->cursor++);
		} else if(b->state == BACKDROP_BAKE_INSTALL) {
			if(b->cursor >= bb_ncols(b)) {
				if(b->rep.columns_installed != b->rep.columns_total)
					return bb_fail(b, BACKDROP_BAKE_ERR_INSTALL);
				b->state = BACKDROP_BAKE_SETTLE;
				b->cursor = 0;
				break;
			}
			if(!bb_install_one(b, b->cursor++))
				return bb_fail(b, b->error);
		} else if(b->state == BACKDROP_BAKE_SETTLE) {
			uint32_t left = budget_us - (budget_us < bb_elapsed_us(start)
				? budget_us : bb_elapsed_us(start));
			enum backdrop_bake_settle s = b->host.settle_step(b->host.user,
				left ? left : 1);
			b->rep.settle_steps++;
			if(s == BACKDROP_BAKE_SETTLE_FAILED)
				return bb_fail(b, BACKDROP_BAKE_ERR_SETTLE);
			if(s == BACKDROP_BAKE_SETTLE_READY) {
				b->state = BACKDROP_BAKE_CAPTURE;
				break;
			}
			if(b->rep.settle_steps >= BACKDROP_BAKE_SETTLE_STEP_LIMIT)
				return bb_fail(b, BACKDROP_BAKE_ERR_NOT_READY);
			break; /* one settle slice per step: the host frame must turn */
		} else if(b->state == BACKDROP_BAKE_CAPTURE) {
			size_t out_bytes = (size_t)BACKDROP_BAKE_OUT_W * BACKDROP_BAKE_OUT_H * 4;
			if(!bb_alloc_allowed())
				return bb_fail(b, BACKDROP_BAKE_ERR_ALLOC);
			b->out = mem2_alloc(out_bytes);
			if(!b->out)
				return bb_fail(b, BACKDROP_BAKE_ERR_ALLOC);
			bb_account(b, (long)out_bytes);
			b->out_row = 0;
			b->sink_geom_set = false;
			b->sink_failed = false;
			bool ok = b->host.render_capture(b->host.user, bb_sink, b);
			if(b->cancel_requested)
				return bb_fail(b, BACKDROP_BAKE_ERR_CANCELLED);
			if(!ok || b->sink_failed || b->out_row != BACKDROP_BAKE_OUT_H)
				return bb_fail(b, BACKDROP_BAKE_ERR_CAPTURE);
			/* Success: end the scene now; only the pixels stay owned. */
			b->host.scene_end(b->host.user);
			b->scene_open = false;
			uint8_t* out = b->out;
			b->out = NULL; /* keep the pixels across the release below */
			bb_release_all(b);
			b->out = out;
			b->state = BACKDROP_BAKE_DONE;
			break;
		}
	}
	uint32_t spent = bb_elapsed_us(start);
	if(phase <= BACKDROP_BAKE_CAPTURE) b->rep.phase_us[phase] += spent;
	if(spent > b->rep.step_max_us) b->rep.step_max_us = spent;
	return b->state;
}

void backdrop_bake_cancel(struct backdrop_bake* b) {
	if(b) b->cancel_requested = true;
}

enum backdrop_bake_state backdrop_bake_state(const struct backdrop_bake* b) {
	return b ? b->state : BACKDROP_BAKE_FAILED;
}

enum backdrop_bake_error backdrop_bake_error(const struct backdrop_bake* b) {
	return b ? b->error : BACKDROP_BAKE_ERR_SCENE;
}

void backdrop_bake_get_report(const struct backdrop_bake* b,
	struct backdrop_bake_report* out) {
	if(!out) return;
	if(!b) {
		memset(out, 0, sizeof(*out));
		return;
	}
	*out = b->rep;
	out->state = b->state;
	out->error = b->error;
}

uint8_t* backdrop_bake_take_pixels(struct backdrop_bake* b) {
	if(!b || b->state != BACKDROP_BAKE_DONE || !b->out) return NULL;
	uint8_t* p = b->out;
	b->out = NULL;
	bb_account(b, -(long)((size_t)BACKDROP_BAKE_OUT_W * BACKDROP_BAKE_OUT_H * 4));
	return p;
}

void backdrop_bake_pixels_free(uint8_t* pixels) {
	if(pixels)
		mem2_free_sized(pixels, (size_t)BACKDROP_BAKE_OUT_W * BACKDROP_BAKE_OUT_H * 4);
}

void backdrop_bake_destroy(struct backdrop_bake* b) {
	if(!b) return;
	bb_release_all(b);
	free(b);
}

/* ---- process-wide slot ---- */

static struct backdrop_bake_host s_host;
static bool s_host_set;
static struct backdrop_bake* s_active;

void backdrop_bake_register_host(const struct backdrop_bake_host* host) {
	s_host_set = host != NULL;
	if(host) s_host = *host;
}

bool backdrop_bake_request(enum backdrop_bake_error* err) {
	enum backdrop_bake_error dummy;
	if(!err) err = &dummy;
	if(s_active || !s_host_set) {
		*err = BACKDROP_BAKE_ERR_SCENE;
		return false;
	}
	s_active = backdrop_bake_create(&s_host, NULL, err);
	return s_active != NULL;
}

struct backdrop_bake* backdrop_bake_active(void) {
	return s_active;
}

void backdrop_bake_service(uint32_t budget_us) {
	if(s_active) (void)backdrop_bake_step(s_active, budget_us);
}

void backdrop_bake_release_active(void) {
	backdrop_bake_destroy(s_active);
	s_active = NULL;
}
