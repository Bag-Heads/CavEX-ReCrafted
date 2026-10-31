#include "../block/block_leaves.h"
#include "render_piston_moving.h"

#include <assert.h>
#include <string.h>

#include "../block/blocks.h"
#include "../block/piston_render.h"
#include "../game/game_state.h"
#include "../network/piston_moving_transport.h"
#include "../platform/displaylist.h"
#include "../platform/gfx.h"
#include "../platform/texture.h"
#include "../world.h"
#include "biome_color.h"
#include "gfx_settings.h"
#include "render_block.h"

static struct displaylist s_moving_list;

static struct block_data render_get_block(
	const struct piston_moving_render_access* access,
	w_coord_t x, w_coord_t y, w_coord_t z) {
	struct block_data out = {.type = BLOCK_AIR};
	if(access && access->get_block)
		access->get_block(access->ctx, x, y, z, &out);
	return out;
}

static bool render_get_moving(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
							  struct piston_moving_state* out) {
	const struct piston_moving_render_access* access = ctx;
	return access && access->get_moving
		&& access->get_moving(access->ctx, x, y, z, out);
}

static uint8_t render_ao_average(const struct block_data sample[4]) {
	static const int shade[5] = {0, 5, 3, 1, 0};
	int sky = 0, torch = 0, count = 0;
	for(int i = 0; i < 4; i++) {
		struct block* b = blocks[sample[i].type];
		if(!b || (b->can_see_through && !b->ignore_lighting)) {
			sky += sample[i].sky_light;
			torch += sample[i].torch_light;
			count++;
		}
	}
	if(count == 0)
		return 0;
	sky = (sky + count - 1) / count - shade[count];
	torch = (torch + count - 1) / count - shade[count];
	if(sky < 0) sky = 0;
	if(torch < 0) torch = 0;
	return (uint8_t)((torch << 4) | sky);
}

static uint8_t render_vertex_sample(
	const struct piston_moving_render_access* access,
	w_coord_t x, w_coord_t y, w_coord_t z, int qx, int qy, int qz, int axis) {
	struct block_data s[4];
	if(axis == 0) {
		s[0] = render_get_block(access, x + qx, y + qy - 1, z + qz);
		s[1] = render_get_block(access, x + qx - 1, y + qy - 1, z + qz);
		s[2] = render_get_block(access, x + qx, y + qy - 1, z + qz - 1);
		s[3] = render_get_block(access, x + qx - 1, y + qy - 1, z + qz - 1);
	} else if(axis == 1) {
		s[0] = render_get_block(access, x + qx - 1, y + qy, z + qz);
		s[1] = render_get_block(access, x + qx - 1, y + qy - 1, z + qz);
		s[2] = render_get_block(access, x + qx - 1, y + qy, z + qz - 1);
		s[3] = render_get_block(access, x + qx - 1, y + qy - 1, z + qz - 1);
	} else {
		s[0] = render_get_block(access, x + qx, y + qy, z + qz - 1);
		s[1] = render_get_block(access, x + qx - 1, y + qy, z + qz - 1);
		s[2] = render_get_block(access, x + qx, y + qy - 1, z + qz - 1);
		s[3] = render_get_block(access, x + qx - 1, y + qy - 1, z + qz - 1);
	}
#ifdef SETTINGS_FANCY_AO
	return render_ao_average(s);
#else
	return (uint8_t)((s[0].torch_light << 4) | s[0].sky_light);
#endif
}

static void render_vertex_lights(
	const struct piston_moving_render_access* access,
	w_coord_t x, w_coord_t y, w_coord_t z, uint8_t out[24]) {
	static const uint8_t q[24][4] = {
		{0,0,0,0},{1,0,0,0},{1,0,1,0},{0,0,1,0},
		{0,2,0,0},{1,2,0,0},{1,2,1,0},{0,2,1,0},
		{0,0,0,1},{0,1,0,1},{0,1,1,1},{0,0,1,1},
		{2,0,0,1},{2,1,0,1},{2,1,1,1},{2,0,1,1},
		{0,0,0,2},{1,0,0,2},{1,1,0,2},{0,1,0,2},
		{0,0,2,2},{1,0,2,2},{1,1,2,2},{0,1,2,2},
	};
	for(int i = 0; i < 24; i++)
		out[i] = render_vertex_sample(
			access, x, y, z, q[i][0], q[i][1], q[i][2], q[i][3]);
}

static void render_prepare_info(
	const struct piston_moving_render_access* access,
	const struct piston_moving_client_entry* entry, uint8_t id, uint8_t meta,
	struct block_data* data, struct block_data neighbours[6],
	struct block_info* info, struct block_info neighbour_info[6],
	uint8_t light[24]) {
	struct block_data marker = render_get_block(access, entry->x, entry->y, entry->z);
	*data = (struct block_data) {
		.type = id,
		.metadata = (uint8_t)(meta & 0x0F),
		.sky_light = marker.sky_light,
		.torch_light = marker.torch_light,
	};
	*info = (struct block_info) {
		.block = data,
		.neighbours = neighbours,
		.x = entry->x,
		.y = entry->y,
		.z = entry->z,
		.get_block = access ? access->get_block : NULL,
		.get_block_ctx = access ? access->ctx : NULL,
		.get_piston_moving = render_get_moving,
		.get_piston_moving_ctx = (void*)access,
	};
	for(int side = 0; side < SIDE_MAX; side++) {
		int dx, dy, dz;
		blocks_side_offset((enum side)side, &dx, &dy, &dz);
		neighbours[side]
			= render_get_block(access, entry->x + dx, entry->y + dy, entry->z + dz);
		neighbour_info[side] = (struct block_info) {
			.block = &neighbours[side],
			.x = entry->x + dx,
			.y = entry->y + dy,
			.z = entry->z + dz,
			.get_block = access ? access->get_block : NULL,
			.get_block_ctx = access ? access->ctx : NULL,
			.get_piston_moving = render_get_moving,
			.get_piston_moving_ctx = (void*)access,
		};
	}
	render_vertex_lights(access, entry->x, entry->y, entry->z, light);
}

static uint32_t render_face_tint(
	const struct piston_moving_render_access* access, const struct block* block,
	w_coord_t x, w_coord_t z, enum side side) {
	if(!block || block->biome_tint == BIOME_TINT_NONE
	   || (block->biome_tint == BIOME_TINT_GRASS && side != SIDE_TOP)
	   || !access || !access->get_tint)
		return 0xFFFFFFu;
	return access->get_tint(access->ctx, x, z, block->biome_tint);
}

static uint32_t render_world_tint(void* ctx, w_coord_t x, w_coord_t z,
								  enum biome_tint tint);

/* DC-GRASS G2: is the moving grass block's biome colour AVAILABLE? Decided
 * independently of the RGB value (a real colormap entry may be pure white
 * and then stays white), with the chunk mesher's rule: the pack's grass
 * colormap is loaded AND the column has a section tint. For the production
 * world accessor the section tint is looked up directly; a caller-supplied
 * accessor (unit fixtures) owns its tint data, so only the colormap gates it. */
static bool render_grass_color_available(
	const struct piston_moving_render_access* access, w_coord_t x, w_coord_t z) {
	if(!biome_grass_colormap_loaded() || !access || !access->get_tint)
		return false;
	if(access->get_tint == render_world_tint) {
		uint8_t gi, gj;
		return access->ctx
			&& world_get_section_tint_at((struct world*)access->ctx, x, z, &gi,
										 &gj);
	}
	return true;
}

/* The moving LEAF block's foliage availability, with the mesher's rule
 * (foliage colormap loaded AND the column has a section tint). */
static bool render_foliage_color_available(
	const struct piston_moving_render_access* access, w_coord_t x, w_coord_t z) {
	if(!biome_foliage_colormap_loaded() || !access || !access->get_tint)
		return false;
	if(access->get_tint == render_world_tint) {
		uint8_t gi, gj;
		return access->ctx
			&& world_get_section_tint_at((struct world*)access->ctx, x, z, &gi,
										 &gj);
	}
	return true;
}

/* DC-GRASS G2: a moving grass block's colour, applied ONCE (the grass atlas
 * cells are raw now): the biome colour (BlockGrass.java:23-28) when
 * available -- whatever its RGB -- else the CaveWii unavailable-data colour
 * BIOME_GRASS_FALLBACK, the same rule as the chunk mesher. */
static uint32_t render_grass_color(
	const struct piston_moving_render_access* access, w_coord_t x, w_coord_t z) {
	if(render_grass_color_available(access, x, z))
		return access->get_tint(access->ctx, x, z, BIOME_TINT_GRASS);
	return BIOME_GRASS_FALLBACK;
}

static size_t render_emit_stored(
	struct displaylist* list, const struct piston_moving_client_entry* entry,
	uint8_t id, uint8_t meta, const struct piston_moving_render_access* access,
	bool count_only) {
	if(!blocks[id])
		return 0;
	struct block_data data, neighbours[6];
	struct block_info info, neighbour_info[6];
	uint8_t light[24];
	render_prepare_info(
		access, entry, id, meta, &data, neighbours, &info, neighbour_info, light);
	struct block* block = blocks[id];
	if(list && !count_only)
		displaylist_set_tinted(list, block->biome_tint != BIOME_TINT_NONE);
	bool grass = id == BLOCK_GRASS;
	uint32_t grass_col = grass ? render_grass_color(access, entry->x, entry->z) : 0;
	/* Leaves: the mesher's colour rule (block_leaves_face_tint), so a pushed
	 * spruce/birch leaf keeps its fixed colour and oak its biome colour. */
	bool leaves = id == BLOCK_LEAVES;
	uint32_t leaf_col = 0xFFFFFFu;
	if(leaves) {
		bool known = render_foliage_color_available(access, entry->x, entry->z);
		bool raw_oak;
		leaf_col = block_leaves_face_tint(
			meta, known,
			known ? access->get_tint(access->ctx, entry->x, entry->z,
									 BIOME_TINT_FOLIAGE)
				  : 0xFFFFFFu,
			&raw_oak);
		if(raw_oak)
			info.texture_override = block_leaves_raw_oak_texture;
	}
	size_t quads = 0;
	for(int side = 0; side < SIDE_MAX; side++) {
		if(list && !count_only) {
			uint32_t tint = leaves ? leaf_col
				: !grass ? render_face_tint(access, block, entry->x, entry->z,
											(enum side)side)
				: side == SIDE_TOP ? grass_col
								   : 0xFFFFFFu;
			displaylist_set_tint(
				list, (uint8_t)(tint >> 16), (uint8_t)(tint >> 8), (uint8_t)tint);
		}
		if(block->renderBlock)
			quads += block->renderBlock(
				list, &info, (enum side)side, &neighbour_info[side], light, count_only);
		if(block->renderBlockAlways)
			quads += block->renderBlockAlways(
				list, &info, (enum side)side, &neighbour_info[side], light, count_only);
		/* DC-GRASS G2: Beta's piston renderer draws the moving block through
		 * the standard RenderBlocks path, so grass gets the fancy-grass side
		 * overlay (RenderBlocks.java:2276-2281 flat, 1954-1970 AO): over a
		 * plain tile-3 side (not snowy 68, BlockGrass.java:12-21), the same
		 * face again with tile 38 x the grass colour, right after its base in
		 * this one list (drawn in order, equal depth). Count-only and emit
		 * both add 1 quad. render_draw_batch gives this batch the Beta alpha
		 * cut (RENDER_GRASS_OVERLAY_ALPHA_REF). */
		if(grass && side != SIDE_TOP && side != SIDE_BOTTOM
		   && block->getTextureIndex(&info, (enum side)side)
			   == tex_atlas_lookup(TEXAT_GRASS_SIDE)) {
			if(list && !count_only)
				displaylist_set_tint(list, (uint8_t)(grass_col >> 16),
									 (uint8_t)(grass_col >> 8),
									 (uint8_t)grass_col);
			quads += render_block_full_tex(
				list, &info, (enum side)side, light,
				tex_atlas_lookup(TEXAT_GRASS_SIDE_OVERLAY), count_only);
		}
	}
	return quads;
}

static size_t render_emit_head(
	struct displaylist* list, const struct piston_moving_client_entry* entry,
	uint8_t meta, bool full_rod, const struct piston_moving_render_access* access,
	bool count_only) {
	struct block_data data, neighbours[6];
	struct block_info info, neighbour_info[6];
	uint8_t light[24];
	render_prepare_info(access, entry, BLOCK_PISTON_HEAD, meta, &data,
		neighbours, &info, neighbour_info, light);
	if(list && !count_only)
		displaylist_set_tinted(list, false);
	size_t quads = 0;
	for(int side = 0; side < SIDE_MAX; side++)
		quads += piston_render_head_variant(
			list, &info, (enum side)side, light, full_rod, count_only);
	return quads;
}

/* Matrix-free variant of the render_visit branch logic, kept solely for the
 * quad-count unit tests -- the production path applies per-piece matrices and
 * calls render_emit_head/render_emit_stored directly. */
#ifdef CAVEWII_PISTON_RENDER_TEST
static size_t render_emit_entry(
	struct displaylist* list, const struct piston_moving_client_entry* entry,
	float partial_tick, const struct piston_moving_render_access* access,
	bool count_only) {
	struct piston_moving_render_plan plan;
	piston_moving_make_render_plan(&entry->state, partial_tick, &plan);
	if(plan.branch == PISTON_MOVING_RENDER_NONE)
		return 0;
	if(plan.branch == PISTON_MOVING_RENDER_HEAD_SHORT)
		return render_emit_head(list, entry, entry->state.stored_meta,
			false, access, count_only);
	if(plan.branch == PISTON_MOVING_RENDER_RETRACT_SOURCE) {
		uint8_t head_meta = (uint8_t)(entry->state.facing
			| (entry->state.stored_id == BLOCK_PISTON_STICKY ? 8 : 0));
		size_t quads = render_emit_head(
			list, entry, head_meta, plan.full_rod, access, count_only);
		return quads + render_emit_stored(
			list, entry, entry->state.stored_id,
			(uint8_t)(entry->state.facing | 8), access, count_only);
	}
	return render_emit_stored(list, entry, entry->state.stored_id,
		entry->state.stored_meta, access, count_only);
}
#endif /* CAVEWII_PISTON_RENDER_TEST */

static bool render_world_get(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
							 struct block_data* out) {
	if(!ctx || !out || y < 0 || y >= WORLD_HEIGHT)
		return false;
	*out = world_get_block((struct world*)ctx, x, y, z);
	return true;
}

static bool render_world_moving(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
								struct piston_moving_state* out) {
	(void)ctx;
	return client_piston_moving_query(x, y, z, out);
}

static uint32_t render_world_tint(void* ctx, w_coord_t x, w_coord_t z,
								  enum biome_tint tint) {
	struct world* world = ctx;
	struct world_section* section = dict_wsection_get(
		world->sections, SECTION_TO_ID(WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z)));
	if(!section)
		return 0xFFFFFFu;
	size_t index = (size_t)W2C_COORD(x) * CHUNK_SIZE + W2C_COORD(z);
	return tint == BIOME_TINT_GRASS
		? biome_grass_color(section->grass_i[index], section->grass_j[index])
		: biome_foliage_color(section->grass_i[index], section->grass_j[index]);
}

struct render_visit_context {
	mat4 view;
	float partial_tick;
	struct piston_moving_render_access access;
};

void render_piston_moving_model_offset(w_coord_t x, w_coord_t y, w_coord_t z,
									   const float motion[3], float out[3]) {
	assert(motion && out);
	out[0] = (float)(WCOORD_CHUNK_OFFSET(x) * CHUNK_SIZE) + motion[0];
	out[1] = (float)(WCOORD_CHUNK_OFFSET(y) * CHUNK_SIZE) + motion[1];
	out[2] = (float)(WCOORD_CHUNK_OFFSET(z) * CHUNK_SIZE) + motion[2];
}

static void render_draw_batch(struct render_visit_context* ctx,
							  const struct piston_moving_client_entry* entry,
							  bool head, uint8_t id, uint8_t meta, bool full_rod,
							  const float translation[3]) {
	displaylist_reset(&s_moving_list);
	size_t quads = head
		? render_emit_head(&s_moving_list, entry, meta, full_rod, &ctx->access, false)
		: render_emit_stored(&s_moving_list, entry, id, meta, &ctx->access, false);
	if(quads == 0 || s_moving_list.truncated)
		return;
	mat4 model, model_view;
	float offset[3];
	render_piston_moving_model_offset(
		entry->x, entry->y, entry->z, translation, offset);
	glm_translate_make(model, (vec3) {offset[0], offset[1], offset[2]});
	glm_mat4_mul(ctx->view, model, model_view);
	gfx_matrix_modelview(model_view);
	gfx_tint(s_moving_list.tinted);
	/* DC-GRASS G2: the moving-grass batch carries its tile-38 overlay; give it
	 * Beta's alpha cut (a >= 26 on both backends, see render_block.h), then
	 * restore render_piston_moving_all's gfx_alpha_test(true). Grass only --
	 * every other moving block keeps today's alpha state. */
	bool grass_batch = !head && id == BLOCK_GRASS;
	if(grass_batch)
		gfx_alpha_test_ref(true, RENDER_GRASS_OVERLAY_ALPHA_REF);
	displaylist_render_immediate(&s_moving_list, (uint16_t)(quads * 4));
	if(grass_batch)
		gfx_alpha_test(true);
	gfx_tint(false);
}

static void render_visit(const struct piston_moving_client_entry* entry, void* user) {
	struct render_visit_context* ctx = user;
	struct piston_moving_render_plan plan;
	piston_moving_make_render_plan(&entry->state, ctx->partial_tick, &plan);
	if(plan.branch == PISTON_MOVING_RENDER_NONE)
		return;
	if(plan.branch == PISTON_MOVING_RENDER_HEAD_SHORT) {
		render_draw_batch(ctx, entry, true, 0, entry->state.stored_meta,
			false, plan.translation);
	} else if(plan.branch == PISTON_MOVING_RENDER_RETRACT_SOURCE) {
		uint8_t head_meta = (uint8_t)(entry->state.facing
			| (entry->state.stored_id == BLOCK_PISTON_STICKY ? 8 : 0));
		render_draw_batch(ctx, entry, true, 0, head_meta,
			plan.full_rod, plan.translation);
		const float stationary[3] = {0.0F, 0.0F, 0.0F};
		render_draw_batch(ctx, entry, false, entry->state.stored_id,
			(uint8_t)(entry->state.facing | 8), true, stationary);
	} else {
		render_draw_batch(ctx, entry, false, entry->state.stored_id,
			entry->state.stored_meta, true, plan.translation);
	}
}

void render_piston_moving_init(void) {
	/* 96 vertices covers the 64-vertex retract composite with room for the largest ordinary shape. */
	displaylist_init(&s_moving_list, 96, 12);
}

void render_piston_moving_all(mat4 view, float partial_tick) {
	assert(view);
	struct render_visit_context ctx = {
		.view = GLM_MAT4_IDENTITY_INIT,
		.partial_tick = partial_tick,
		.access = {
			.get_block = render_world_get,
			.get_moving = render_world_moving,
			.get_tint = render_world_tint,
			.ctx = world_view_current(),
		},
	};
	glm_mat4_copy(view, ctx.view);
	gfx_bind_texture(&texture_terrain);
	gfx_blending(MODE_BLEND);
	gfx_alpha_test(true);
	gfx_cull_func(MODE_NONE);
	gfx_lighting(true);
	client_piston_moving_visit(render_visit, &ctx);
	gfx_tint(false);
	gfx_matrix_modelview(GLM_MAT4_IDENTITY);
	gfx_cull_func(MODE_BACK);
	gfx_blending(MODE_OFF);
}

#ifdef CAVEWII_PISTON_RENDER_TEST
/* DC-GRASS G2 test seam: the PRODUCTION world accessor render_piston_moving_all
 * builds (same callbacks), bound to `world`, so moving-grass colour
 * availability is tested through the real section-tint lookup. Declared by
 * its test (render_piston_moving.h is outside the G2 scope). */
void render_piston_moving_test_world_access(
	struct world* world, struct piston_moving_render_access* out);
void render_piston_moving_test_world_access(
	struct world* world, struct piston_moving_render_access* out) {
	assert(out);
	*out = (struct piston_moving_render_access) {
		.get_block = render_world_get,
		.get_moving = render_world_moving,
		.get_tint = render_world_tint,
		.ctx = world,
	};
}

size_t render_piston_moving_test_emit(
	struct displaylist* list, const struct piston_moving_client_entry* entry,
	float partial_tick, const struct piston_moving_render_access* access,
	bool count_only) {
	return render_emit_entry(list, entry, partial_tick, access, count_only);
}
#endif
