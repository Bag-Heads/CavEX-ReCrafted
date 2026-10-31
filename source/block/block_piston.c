/*
	This file is part of CaveWii_v2 (a CavEX/fCavEX derivative).

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CaveWii_v2 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CaveWii_v2.  If not, see <http://www.gnu.org/licenses/>.
*/

// Block-table entries for the Beta 1.7.3 pistons (ids 29 sticky / 33 base / 34 head / 36 moving). The
// faithful SIMULATION lives in redstone_piston.c (the pure decision + move materialization) +
// piston_mobility.c + redstone_piston_server.c (the production hooks: the synchronous move dispatch +
// the moving-TE store + the T+2 settle driver). This file binds those onto the four block ids and
// supplies the render-facing fields. The settled base/head geometry and collision boxes are transcribed
// from BlockPistonBase / BlockPistonExtension + RenderBlocks (740c583). The moving-TE slide animation is
// still a separate client-protocol concern: id 36 does not carry its stored block to the client today.

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../item/items.h"
#include "../network/server_local.h" // s->player (placement orientation)
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "blocks.h"
#include "blocks_data.h"
#include "piston_moving.h"
#include "piston_render.h"
#include "piston_texture.h"			// piston_face_texat (the directional face textures)
#include "redstone_piston.h"		// piston_determine_orientation, RS_PISTON_*
#include "redstone_piston_server.h" // piston_server_place / _neighbour_change / piston_moving_stored

// --- Beta settled piston geometry -------------------------------------------------------------------
static enum block_material getMaterial(struct block_info* this) {
	(void)this;
	return MATERIAL_STONE; // Beta Material.piston; CaveWii has no piston material (stone is closest)
}

struct piston_bounds {
	float x1, y1, z1;
	float x2, y2, z2;
};

// Beta BlockPistonBase.setBlockBoundsBasedOnState, cases 0..5. These are the selection/render bounds
// for an EXTENDED base. Its getCollidingBoundingBoxes deliberately resets to a full cube, so entity
// collision remains full below.
static const struct piston_bounds PISTON_BASE_EXTENDED[6] = {
	{0.0F, 0.25F, 0.0F, 1.0F, 1.0F, 1.0F},
	{0.0F, 0.0F, 0.0F, 1.0F, 0.75F, 1.0F},
	{0.0F, 0.0F, 0.25F, 1.0F, 1.0F, 1.0F},
	{0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.75F},
	{0.25F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F},
	{0.0F, 0.0F, 0.0F, 0.75F, 1.0F, 1.0F},
};

// Beta BlockPistonExtension.setBlockBoundsBasedOnState / the first box in
// getCollidingBoundingBoxes: the 0.25-thick pushing plate.
static const struct piston_bounds PISTON_HEAD_PLATE[6] = {
	{0.0F, 0.0F, 0.0F, 1.0F, 0.25F, 1.0F},
	{0.0F, 0.75F, 0.0F, 1.0F, 1.0F, 1.0F},
	{0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.25F},
	{0.0F, 0.0F, 0.75F, 1.0F, 1.0F, 1.0F},
	{0.0F, 0.0F, 0.0F, 0.25F, 1.0F, 1.0F},
	{0.75F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F},
};

// Beta BlockPistonExtension.getCollidingBoundingBoxes, second setBlockBounds in cases 0..5.
// The direction-dependent cross sections look asymmetric, but they are the exact 740c583 values.
static const struct piston_bounds PISTON_HEAD_COLLISION_ROD[6] = {
	{0.375F, 0.25F, 0.375F, 0.625F, 1.0F, 0.625F},
	{0.375F, 0.0F, 0.375F, 0.625F, 0.75F, 0.625F},
	{0.25F, 0.375F, 0.25F, 0.75F, 0.625F, 1.0F},
	{0.25F, 0.375F, 0.0F, 0.75F, 0.625F, 0.75F},
	{0.375F, 0.25F, 0.25F, 0.625F, 0.75F, 1.0F},
	{0.0F, 0.375F, 0.25F, 0.75F, 0.625F, 0.75F},
};

// Beta RenderBlocks.func_31080_c(..., renderFullRod=true), cases 0..5. Unlike collision, the visual
// rod is a consistent 0.25-square shaft and extends 0.25 into the shortened base cell, closing the gap
// between the base's 0.75-depth body and the head plate. Only the four LATERAL faces are rendered.
static const struct piston_bounds PISTON_HEAD_RENDER_ROD[6] = {
	{0.375F, 0.25F, 0.375F, 0.625F, 1.25F, 0.625F},
	{0.375F, -0.25F, 0.375F, 0.625F, 0.75F, 0.625F},
	{0.375F, 0.375F, 0.25F, 0.625F, 0.625F, 1.25F},
	{0.375F, 0.375F, -0.25F, 0.625F, 0.625F, 0.75F},
	{0.25F, 0.375F, 0.375F, 1.25F, 0.625F, 0.625F},
	{-0.25F, 0.375F, 0.375F, 0.75F, 0.625F, 0.625F},
};

// RenderBlocks.func_31080_c(..., renderFullRod=false): the rod stays plate-anchored and is exactly
// half a block long. It is not a scaled copy around the full rod's centre.
static const struct piston_bounds PISTON_HEAD_SHORT_ROD[6] = {
	{0.375F, 0.25F, 0.375F, 0.625F, 0.75F, 0.625F},
	{0.375F, 0.25F, 0.375F, 0.625F, 0.75F, 0.625F},
	{0.375F, 0.375F, 0.25F, 0.625F, 0.625F, 0.75F},
	{0.375F, 0.375F, 0.25F, 0.625F, 0.625F, 0.75F},
	{0.25F, 0.375F, 0.375F, 0.75F, 0.625F, 0.625F},
	{0.25F, 0.375F, 0.375F, 0.75F, 0.625F, 0.625F},
};

static const struct piston_bounds PISTON_FULL_BOUNDS
	= {0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F};

static void piston_set_box(struct AABB* out, const struct piston_bounds* b) {
	out->x1 = b->x1;
	out->y1 = b->y1;
	out->z1 = b->z1;
	out->x2 = b->x2;
	out->y2 = b->y2;
	out->z2 = b->z2;
}

static size_t getBoundingBoxBase(struct block_info* this, bool entity, struct AABB* out) {
	int meta = (this && this->block) ? (this->block->metadata & 0x0F) : 0;
	int facing = piston_orientation(meta);
	const struct piston_bounds* b = &PISTON_FULL_BOUNDS;
	if(!entity && piston_is_powered(meta) && facing >= 0 && facing < 6)
		b = &PISTON_BASE_EXTENDED[facing];
	if(out)
		piston_set_box(out, b);
	return 1;
}

static size_t getBoundingBoxHead(struct block_info* this, bool entity, struct AABB* out) {
	int facing = (this && this->block) ? piston_orientation(this->block->metadata & 0x0F) : 7;
	if(facing < 0 || facing >= 6) {
		if(out)
			piston_set_box(out, &PISTON_FULL_BOUNDS);
		return 1;
	}
	if(out) {
		piston_set_box(out, &PISTON_HEAD_PLATE[facing]);
		if(entity)
			piston_set_box(out + 1, &PISTON_HEAD_COLLISION_ROD[facing]);
	}
	return entity ? 2 : 1;
}

static bool piston_moving_singular_box(struct block_info* moving,
								   const struct piston_moving_state* state,
								   bool entity, struct AABB* out) {
	if(!moving || !moving->block || !state || state->stored_id == BLOCK_AIR
	   || state->stored_id == BLOCK_PISTON_MOVING || !blocks[state->stored_id])
		return false;

	struct block_data stored = {
		.type = state->stored_id,
		.metadata = (uint8_t)(state->stored_meta & 0x0F),
	};
	struct block_info info = *moving;
	info.block = &stored;

	/*
		BlockPistonMoving calls storedBlock.getCollisionBoundingBoxFromPool: one singular box, NOT
		addCollidingBoundingBoxesToList. These overrides differ from Cave's ordinary multi-box path.
		[740c583 BlockPistonMoving.getAxisAlignedBB; BlockStairs; BlockFarmland]
	*/
	if(entity && (state->stored_id == BLOCK_PISTON_HEAD
				   || state->stored_id == BLOCK_WOODEN_STAIRS
				   || state->stored_id == BLOCK_STONE_STAIRS
				   || state->stored_id == BLOCK_FARMLAND)) {
		piston_set_box(out, &PISTON_FULL_BOUNDS);
		return true;
	}

	struct block* b = blocks[state->stored_id];
	size_t count = b->getBoundingBox(&info, entity, NULL);
	if(count == 0)
		return false; /* rails, plants, wire, and other null-collision blocks */
	struct AABB boxes[count];
	b->getBoundingBox(&info, entity, boxes);
	*out = boxes[0];
	return true;
}

static size_t getBoundingBoxMoving(struct block_info* this, bool entity, struct AABB* out) {
	if(!this || !this->get_piston_moving)
		return 0; /* orphan TileEntityPiston -> null collision/selection */
	struct piston_moving_state state;
	if(!this->get_piston_moving(this->get_piston_moving_ctx,
								this->x, this->y, this->z, &state))
		return 0;
	if(!out)
		return piston_moving_singular_box(this, &state, entity, &(struct AABB) {0}) ? 1 : 0;
	if(!piston_moving_singular_box(this, &state, entity, out))
		return 0;
	float shift = piston_moving_collision_shift(&state);
	int dx, dy, dz;
	piston_moving_facing_offset(state.facing, &dx, &dy, &dz);
	aabb_translate(out, -(float)dx * shift, -(float)dy * shift, -(float)dz * shift);
	return 1;
}
static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	(void)this;
	(void)side;
	(void)it;
	// Beta isOpaqueCube()==false (a piston never fully occludes -- the head/groove); be conservative and
	// occlude nothing so a neighbour face is never wrongly culled against the partial piston geometry.
	return face_occlusion_empty();
}
// Beta BlockPistonBase.getBlockTextureFromSideAndMetadata + BlockPistonExtension: the directional face
// textures (front plate / back / sides, + the inner face when extended). The selection logic is the pure
// golden-tested piston_face_texat; this wraps it with the atlas lookup.
static uint8_t getTexturePiston(struct block_info* this, enum side side) {
	return tex_atlas_lookup(piston_face_texat(this->block->type, this->block->metadata, side));
}

// Standalone settled-piston cuboid emitter. Keeping it here avoids coupling piston correctness to the
// shared render_block.c hot path. Its winding, per-corner light indices, and side dimming mirror
// render_block_side_adv_v2; UV span follows the rendered sub-cuboid's dimensions (Beta's piston renderer
// likewise uses 4/12/16-texel spans for the 0.25 plate / shaft / full faces).
static const uint8_t PISTON_LIGHT_BOTTOM[16]
	= {0, 0, 0, 0, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12};
static const uint8_t PISTON_LIGHT_SIDE_X[16]
	= {0, 0, 0, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13};
static const uint8_t PISTON_LIGHT_SIDE_Z[16]
	= {0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};

static uint8_t piston_dim_light(uint8_t light, const uint8_t* table) {
	return table ? (uint8_t)((table[light >> 4] << 4) | table[light & 0x0F]) : light;
}

static int16_t piston_render_coord(int block_coord, float local) {
	return (int16_t)(W2C_COORD(block_coord) * 256 + (int)(local * 256.0F));
}

static int piston_beta_side(enum side side) {
	static const int map[SIDE_MAX] = {
		[SIDE_TOP] = 1, [SIDE_BOTTOM] = 0, [SIDE_LEFT] = 4,
		[SIDE_RIGHT] = 5, [SIDE_FRONT] = 2, [SIDE_BACK] = 3,
	};
	return (side >= SIDE_TOP && side < SIDE_MAX) ? map[side] : 7;
}

static int piston_bound_texel(float value) {
	return (int)(value * 16.0F + 0.5F);
}

// RenderBlocks.func_31074_b / func_31080_c set one of six face-rotation fields before rendering
// the shortened base or head plate. Rows are piston facing; columns are Beta face 0..5. Values are
// the exact field_3108x modes consumed by renderBottom/Top/East/West/North/SouthFace.
static const uint8_t PISTON_UV_ROTATION[6][6] = {
	{0, 0, 3, 3, 3, 3}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 2, 1},
	{3, 3, 0, 0, 1, 2}, {1, 2, 1, 2, 0, 0}, {2, 1, 2, 1, 0, 0},
};

struct piston_uv_vars {
	int ua, va, ub, vb, uc, vc, ud, vd;
};

// Finish the temporary RenderBlocks variables var12/16 (ua/va), var14/18 (ub/vb),
// var20/24 (uc/vc), and var22/26 (ud/vd). Modes 1/2 below are the two reassignment
// patterns repeated verbatim by all six Java face methods; mode 3 is their default pattern.
static struct piston_uv_vars piston_uv_finish(int style, int ua, int va, int ub, int vb) {
	struct piston_uv_vars out = {.ua = ua, .va = va, .ub = ub, .vb = vb};
	if(style == 1) {
		out.uc = ua; out.vc = va;
		out.ud = ub; out.vd = vb;
		int old_va = va;
		out.va = vb;
		out.vb = old_va;
	} else if(style == 2) {
		out.uc = ub; out.vc = vb;
		out.ud = ua; out.vd = va;
		int old_ua = ua;
		out.ua = ub;
		out.ub = old_ua;
	} else {
		out.uc = ub; out.vc = va;
		out.ud = ua; out.vd = vb;
	}
	return out;
}

static void piston_standard_face_uv(const struct piston_bounds* bounds, int facing, int beta_side,
									uint8_t uv[4][2]) {
	int x1 = piston_bound_texel(bounds->x1), y1 = piston_bound_texel(bounds->y1);
	int z1 = piston_bound_texel(bounds->z1), x2 = piston_bound_texel(bounds->x2);
	int y2 = piston_bound_texel(bounds->y2), z2 = piston_bound_texel(bounds->z2);
	int rotation = PISTON_UV_ROTATION[facing][beta_side];
	int ua = 0, va = 0, ub = 16, vb = 16, style = 3;

	// Integer-texel form of RenderBlocks' six face methods. Bounds used here are all inside
	// [0,1], so the Java out-of-range fallback is irrelevant (the rod uses its own helper below).
	switch(beta_side) {
		case 0: // renderBottomFace, field_31082_l
			ua = x1; va = z1; ub = x2; vb = z2;
			if(rotation == 1) {
				ua = 16 - z2; va = x1; ub = 16 - z1; vb = x2; style = 2;
			} else if(rotation == 2) {
				ua = z1; va = 16 - x2; ub = z2; vb = 16 - x1; style = 1;
			} else if(rotation == 3) {
				ua = 16 - x1; va = 16 - z1; ub = 16 - x2; vb = 16 - z2;
			}
			break;
		case 1: // renderTopFace, field_31083_k
			ua = x1; va = z1; ub = x2; vb = z2;
			if(rotation == 1) {
				ua = z1; va = 16 - x2; ub = z2; vb = 16 - x1; style = 1;
			} else if(rotation == 2) {
				ua = 16 - z2; va = x1; ub = 16 - z1; vb = x2; style = 2;
			} else if(rotation == 3) {
				ua = 16 - x1; va = 16 - z1; ub = 16 - x2; vb = 16 - z2;
			}
			break;
		case 2: // renderEastFace (-Z), field_31087_g
			ua = x1; va = 16 - y2; ub = x2; vb = 16 - y1;
			if(rotation == 1) {
				ua = 16 - y2; va = x2; ub = 16 - y1; vb = x1; style = 2;
			} else if(rotation == 2) {
				ua = y1; va = 16 - x1; ub = y2; vb = 16 - x2; style = 1;
			} else if(rotation == 3) {
				ua = 16 - x1; va = y2; ub = 16 - x2; vb = y1;
			}
			break;
		case 3: // renderWestFace (+Z), field_31086_h
			ua = x1; va = 16 - y2; ub = x2; vb = 16 - y1;
			if(rotation == 1) {
				ua = y1; va = 16 - x2; ub = y2; vb = 16 - x1; style = 1;
			} else if(rotation == 2) {
				ua = 16 - y2; va = x1; ub = 16 - y1; vb = x2; style = 2;
			} else if(rotation == 3) {
				ua = 16 - x1; va = y2; ub = 16 - x2; vb = y1;
			}
			break;
		case 4: // renderNorthFace (-X), field_31084_j
			ua = z1; va = 16 - y2; ub = z2; vb = 16 - y1;
			if(rotation == 1) {
				ua = y1; va = 16 - z2; ub = y2; vb = 16 - z1; style = 1;
			} else if(rotation == 2) {
				ua = 16 - y2; va = z1; ub = 16 - y1; vb = z2; style = 2;
			} else if(rotation == 3) {
				ua = 16 - z1; va = y2; ub = 16 - z2; vb = y1;
			}
			break;
		case 5: // renderSouthFace (+X), field_31085_i
			ua = z1; va = 16 - y2; ub = z2; vb = 16 - y1;
			if(rotation == 1) {
				ua = 16 - y2; va = z2; ub = 16 - y1; vb = z1; style = 2;
			} else if(rotation == 2) {
				ua = y1; va = 16 - z1; ub = y2; vb = 16 - z2; style = 1;
			} else if(rotation == 3) {
				ua = 16 - z1; va = y2; ub = 16 - z2; vb = y1;
			}
			break;
		default: break;
	}

	struct piston_uv_vars p = piston_uv_finish(style, ua, va, ub, vb);
#define SET_UV(I, U, V) do { uv[(I)][0] = (uint8_t)(U); uv[(I)][1] = (uint8_t)(V); } while(0)
	switch(beta_side) {
		case 0:
			SET_UV(0, p.ua, p.va); SET_UV(1, p.ud, p.vd);
			SET_UV(2, p.ub, p.vb); SET_UV(3, p.uc, p.vc);
			break;
		case 1:
			SET_UV(0, p.ua, p.va); SET_UV(1, p.uc, p.vc);
			SET_UV(2, p.ub, p.vb); SET_UV(3, p.ud, p.vd);
			break;
		case 2:
		case 5:
			SET_UV(0, p.ub, p.vb); SET_UV(1, p.ud, p.vd);
			SET_UV(2, p.ua, p.va); SET_UV(3, p.uc, p.vc);
			break;
		case 3:
		case 4:
			SET_UV(0, p.ud, p.vd); SET_UV(1, p.ua, p.va);
			SET_UV(2, p.uc, p.vc); SET_UV(3, p.ub, p.vb);
			break;
		default:
			for(int i = 0; i < 4; i++) SET_UV(i, 0, 0);
			break;
	}
#undef SET_UV
}

static bool piston_rod_face_uv(int facing, int beta_side, bool full_rod,
							   uint8_t uv[4][2]) {
	static const uint8_t pattern[3][4][2] = {
		{{0, 0}, {16, 0}, {16, 4}, {0, 4}},
		{{0, 4}, {0, 0}, {16, 0}, {16, 4}},
		{{16, 4}, {16, 0}, {0, 0}, {0, 4}},
	};
	int which = -1;
	switch(facing / 2) {
		case 0: // func_31076_a: Y-axis rod
			if(beta_side == 4 || beta_side == 3) which = 0;
			if(beta_side == 5 || beta_side == 2) which = 1;
			break;
		case 1: // func_31081_b: Z-axis rod
			if(beta_side == 5 || beta_side == 0) which = 0;
			if(beta_side == 4) which = 1;
			if(beta_side == 1) which = 2;
			break;
		case 2: // func_31077_c: X-axis rod
			if(beta_side == 1 || beta_side == 2) which = 0;
			if(beta_side == 0 || beta_side == 3) which = 1;
			break;
		default: break;
	}
	if(which < 0)
		return false;
	uint8_t span = full_rod ? 16 : 8;
	for(int i = 0; i < 4; i++) {
		uv[i][0] = pattern[which][i][0] == 16 ? span : pattern[which][i][0];
		uv[i][1] = pattern[which][i][1];
	}
	return true;
}

static size_t piston_render_box_face(struct displaylist* d, struct block_info* this,
								   const struct piston_bounds* b, enum side side,
								   int facing, const uint8_t uv_override[4][2],
								   const uint8_t* vertex_light, const uint8_t* dim_override,
								   bool count_only) {
	if(side < SIDE_TOP || side >= SIDE_MAX || !this || !this->block || !b
	   || facing < 0 || facing >= 6 || !blocks[this->block->type]
	   || !blocks[this->block->type]->getTextureIndex)
		return 0;
	if(count_only)
		return 1;
	if(!d || !vertex_light)
		return 0;

	int16_t x1 = piston_render_coord(this->x, b->x1);
	int16_t y1 = piston_render_coord(this->y, b->y1);
	int16_t z1 = piston_render_coord(this->z, b->z1);
	int16_t x2 = piston_render_coord(this->x, b->x2);
	int16_t y2 = piston_render_coord(this->y, b->y2);
	int16_t z2 = piston_render_coord(this->z, b->z2);
	// Honor block_info::texture_override the same way render_block.c's
	// block_texture_index() does (not callable from here -- static to that
	// file): the crack overlay (render_block_cracks) sets it on ITS
	// stack-local block_info instead of swapping blocks[]->getTextureIndex
	// globally, so any direct blocks[]->getTextureIndex(this, ...) call
	// bypasses digging cracks on piston faces without this check.
	uint8_t texture = this->texture_override
		? this->texture_override(this, side)
		: blocks[this->block->type]->getTextureIndex(this, side);
	uint8_t u0 = TEX_OFFSET(TEXTURE_X(texture));
	uint8_t v0 = TEX_OFFSET(TEXTURE_Y(texture));
	uint8_t uv[4][2];
	if(uv_override)
		for(int i = 0; i < 4; i++) {
			uv[i][0] = uv_override[i][0];
			uv[i][1] = uv_override[i][1];
		}
	else
		piston_standard_face_uv(b, facing, piston_beta_side(side), uv);
	const uint8_t* dim;

	switch(side) {
		case SIDE_LEFT:
		case SIDE_RIGHT:
			dim = PISTON_LIGHT_SIDE_X;
			break;
		case SIDE_TOP:
		case SIDE_BOTTOM:
			dim = side == SIDE_BOTTOM ? PISTON_LIGHT_BOTTOM : NULL;
			break;
		case SIDE_FRONT:
		case SIDE_BACK:
			dim = PISTON_LIGHT_SIDE_Z;
			break;
		default: return 0;
	}
	if(dim_override)
		dim = dim_override;
#define PISTON_VERTEX(PX, PY, PZ, LI, I) \
	displaylist_vertex(d, (PX), (PY), (PZ), piston_dim_light(vertex_light[(LI)], dim), \
		(uint8_t)(u0 + uv[(I)][0]), (uint8_t)(v0 + uv[(I)][1]))
	switch(side) {
		case SIDE_LEFT:
			PISTON_VERTEX(x1, y1, z1, 8, 0);
			PISTON_VERTEX(x1, y2, z1, 9, 1);
			PISTON_VERTEX(x1, y2, z2, 10, 2);
			PISTON_VERTEX(x1, y1, z2, 11, 3);
			break;
		case SIDE_RIGHT:
			PISTON_VERTEX(x2, y1, z1, 12, 0);
			PISTON_VERTEX(x2, y1, z2, 15, 1);
			PISTON_VERTEX(x2, y2, z2, 14, 2);
			PISTON_VERTEX(x2, y2, z1, 13, 3);
			break;
		case SIDE_TOP:
			PISTON_VERTEX(x1, y2, z1, 4, 0);
			PISTON_VERTEX(x2, y2, z1, 5, 1);
			PISTON_VERTEX(x2, y2, z2, 6, 2);
			PISTON_VERTEX(x1, y2, z2, 7, 3);
			break;
		case SIDE_BOTTOM:
			PISTON_VERTEX(x1, y1, z1, 0, 0);
			PISTON_VERTEX(x1, y1, z2, 3, 1);
			PISTON_VERTEX(x2, y1, z2, 2, 2);
			PISTON_VERTEX(x2, y1, z1, 1, 3);
			break;
		case SIDE_FRONT:
			PISTON_VERTEX(x1, y1, z1, 16, 0);
			PISTON_VERTEX(x2, y1, z1, 17, 1);
			PISTON_VERTEX(x2, y2, z1, 18, 2);
			PISTON_VERTEX(x1, y2, z1, 19, 3);
			break;
		case SIDE_BACK:
			PISTON_VERTEX(x1, y1, z2, 20, 0);
			PISTON_VERTEX(x1, y2, z2, 23, 1);
			PISTON_VERTEX(x2, y2, z2, 22, 2);
			PISTON_VERTEX(x2, y1, z2, 21, 3);
			break;
		default: break;
	}
#undef PISTON_VERTEX
	return 1;
}

static size_t renderPistonBase(struct displaylist* d, struct block_info* this,
							  enum side side, struct block_info* it,
							  uint8_t* vertex_light, bool count_only) {
	int meta = (this && this->block) ? (this->block->metadata & 0x0F) : 0;
	int facing = piston_orientation(meta);
	if(facing < 0 || facing >= 6)
		return render_block_full(d, this, side, it, vertex_light, count_only);
	const struct piston_bounds* bounds = piston_is_powered(meta)
		? &PISTON_BASE_EXTENDED[facing] : &PISTON_FULL_BOUNDS;
	return piston_render_box_face(
		d, this, bounds, side, facing, NULL, vertex_light, NULL, count_only);
}

size_t piston_render_head_variant(struct displaylist* d, struct block_info* this,
								  enum side side, uint8_t vertex_light[24],
								  bool full_rod, bool count_only) {
	int facing = (this && this->block) ? piston_orientation(this->block->metadata & 0x0F) : 7;
	if(facing < 0 || facing >= 6)
		return render_block_full(d, this, side, NULL, vertex_light, count_only);
	size_t quads = piston_render_box_face(
		d, this, &PISTON_HEAD_PLATE[facing], side, facing, NULL, vertex_light, NULL, count_only);
	static const int opposite[6] = {1, 0, 3, 2, 5, 4};
	int beta_side = piston_beta_side(side);
	if(beta_side != facing && beta_side != opposite[facing]) {
		uint8_t rod_uv[4][2];
		if(!piston_rod_face_uv(facing, beta_side, full_rod, rod_uv))
			return quads;
		// RenderBlocks.func_31080_c cases 4/5 shade both Z faces at 0.6, unlike a normal
		// cuboid's 0.8 Z faces. Other rod axes already match the standard side tables.
		const uint8_t* rod_dim = (facing == 4 || facing == 5)
			&& (side == SIDE_FRONT || side == SIDE_BACK) ? PISTON_LIGHT_SIDE_X : NULL;
		uint8_t rod_light[24];
		uint8_t centre_light = this && this->block
			? (uint8_t)((this->block->torch_light << 4) | this->block->sky_light) : 0;
		for(int i = 0; i < 24; i++)
			rod_light[i] = centre_light;
		quads += piston_render_box_face(
			d, this, full_rod ? &PISTON_HEAD_RENDER_ROD[facing]
								 : &PISTON_HEAD_SHORT_ROD[facing],
			side, facing, rod_uv, rod_light, rod_dim, count_only);
	}
	return quads;
}

static size_t renderPistonHead(struct displaylist* d, struct block_info* this,
							  enum side side, struct block_info* it,
							  uint8_t* vertex_light, bool count_only) {
	(void)it;
	return piston_render_head_variant(d, this, side, vertex_light, true, count_only);
}

// Place a piston facing away from / based on the placer (Beta determineOrientation), then updatePiston
// State. sticky is taken from the item id (the block-item being placed: 29 vs 33).
static bool onItemPlacePiston(struct server_local* s, struct server_player* actor, struct item_data* it, struct block_info* where,
							  struct block_info* on, enum side on_side) {
	(void)on;
	(void)on_side;
	bool sticky = (it && it->id == RS_PISTON_STICKY_ID);
	int facing = piston_determine_orientation(actor->x, actor->y, actor->z, 1.62F, actor->rx,
											  where->x, where->y, where->z);
	return piston_server_place(s, actor, where, facing, sticky);
}

static size_t drop_self_base(struct block_info* this, struct item_data* it, struct java_random* g,
							 struct server_local* s) {
	(void)g;
	(void)s;
	if(it) {
		it->id = RS_PISTON_BASE_ID;
		it->durability = 0;
		it->count = 1;
	}
	return 1;
}
static size_t drop_self_sticky(struct block_info* this, struct item_data* it, struct java_random* g,
							   struct server_local* s) {
	(void)g;
	(void)s;
	if(it) {
		it->id = RS_PISTON_STICKY_ID;
		it->durability = 0;
		it->count = 1;
	}
	return 1;
}

struct block block_piston = {
	.name = "Piston",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBoxBase,
	.getMaterial = getMaterial,
	.getTextureIndex = getTexturePiston,
	.getDroppedItem = drop_self_base,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	// SYNCHRONOUS move: the base self-reacts to a neighbour power change (Beta onNeighborBlockChange ->
	// updatePistonState). NO onWorldTick (the move is inline; the settle runs on the separate
	// piston_moving_te_tick driver) -- so the engine's constant auto-schedule never arms (RAIL 4).
	.onNeighbourBlockChange = piston_server_neighbour_change,
	.transparent = false,
	.renderBlock = renderPistonBase,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	// BlockPistonBase isOpaqueCube/renderAsNormalBlock are false; Block's
	// constructor therefore gives it lightOpacity 0 (Beta 1.7.3 decompile).
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 750, // Beta setHardness(0.5F); Cave scale is 1500 per hardness unit
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = onItemPlacePiston,
		// Beta RenderBlocks.renderBlockOnInventory FORCES metadata=1 for render type 16 (the piston):
		// Render type 16 is forced to metadata 1 (orientation 1). So the inventory/held icon faces UP (+Y) and the
		// front pushing plate is the VISIBLE top face -- 107 (normal) / 106 (sticky), so the two stacks
		// differ. Without this the icon defaulted to metadata 0 (facing -Y): the plate hid on the bottom
		// and base/sticky rendered identically and "headless". Icon-only -- in-world placement is
		// unaffected (onItemPlacePiston derives facing from the player's look, not the item metadata).
		.render_data.block.has_default = true,
		.render_data.block.default_metadata = 1,
		.render_data.block.default_rotation = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

struct block block_piston_sticky = {
	.name = "Sticky Piston",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBoxBase,
	.getMaterial = getMaterial,
	.getTextureIndex = getTexturePiston,
	.getDroppedItem = drop_self_sticky,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	.onNeighbourBlockChange = piston_server_neighbour_change,
	.transparent = false,
	.renderBlock = renderPistonBase,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	// Sticky pistons share BlockPistonBase's non-normal, zero-opacity shape.
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 750,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = onItemPlacePiston,
		// Beta RenderBlocks.renderBlockOnInventory FORCES metadata=1 for render type 16 (the piston):
		// Render type 16 is forced to metadata 1 (orientation 1). So the inventory/held icon faces UP (+Y) and the
		// front pushing plate is the VISIBLE top face -- 107 (normal) / 106 (sticky), so the two stacks
		// differ. Without this the icon defaulted to metadata 0 (facing -Y): the plate hid on the bottom
		// and base/sticky rendered identically and "headless". Icon-only -- in-world placement is
		// unaffected (onItemPlacePiston derives facing from the player's look, not the item metadata).
		.render_data.block.has_default = true,
		.render_data.block.default_metadata = 1,
		.render_data.block.default_rotation = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

// --- the head / extension (id 34) -------------------------------------------------------------------
// Beta BlockPistonExtension: hardness 0.5, drops NOTHING itself (quantityDropped==0). Two faithful
// behaviours (Phase 5): onBlockRemoval (mining the head drops + clears the still-powered base behind it --
// piston_head_on_removal, called from the dig path) and onNeighborBlockChange (a baseless head self-removes
// -- piston_head_neighbour_change, bound below).
static size_t drop_nothing(struct block_info* this, struct item_data* it, struct java_random* g,
						   struct server_local* s) {
	(void)this;
	(void)it;
	(void)g;
	(void)s;
	return 0;
}
static void piston_head_removed(
	struct server_local* s, struct block_info* info) {
	piston_head_on_removal(
		s, info->x, info->y, info->z, info->block->metadata);
}
struct block block_piston_head = {
	.name = "Piston Head",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBoxHead,
	.getMaterial = getMaterial,
	.getTextureIndex = getTexturePiston,
	.getDroppedItem = drop_nothing,
	.onBlockRemoval = piston_head_removed,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	// Beta BlockPistonExtension.onNeighborBlockChange: a head whose base is gone self-removes (orphan
	// cleanup); otherwise it forwards the update to the base. (onBlockRemoval is the dig-path hook.)
	.onNeighbourBlockChange = piston_head_neighbour_change,
	.transparent = false,
	.renderBlock = renderPistonHead,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	// BlockPistonExtension isOpaqueCube/renderAsNormalBlock are false in Beta.
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 750,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = block_place_default, // not normally placeable; satisfies the registry assert
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

// --- the moving block (id 36) -- a statically render-less, unbreakable TE marker ----------------------
// Beta BlockPistonMoving.getRenderType == -1: ordinary chunk meshing emits nothing here; the separate
// TileEntityPiston renderer draws the stored block in flight. Collision is likewise supplied dynamically
// from the persisted moving-TE state (an orphan has no box), never by a placeholder full cube.
static size_t renderNothing(struct displaylist* d, struct block_info* this, enum side side,
							struct block_info* it, uint8_t* light, bool count) {
	(void)d;
	(void)this;
	(void)side;
	(void)it;
	(void)light;
	(void)count;
	return 0; // the moving block emits no geometry (Beta render type -1)
}
static enum block_material getMaterialMoving(struct block_info* this) {
	(void)this;
	return MATERIAL_STONE;
}
static uint8_t getTextureMoving(struct block_info* this, enum side side) {
	(void)this;
	(void)side;
	return tex_atlas_lookup(TEXAT_STONE); // static marker path never samples it; satisfies registry assert
}
// Beta BlockPistonMoving.blockActivated: right-clicking an ORPHANED moving block (one whose TileEntity
// is gone) clears it to air. Normal save/reload persists the TileEntityPiston; an orphan instead means a
// corrupt/stale voxel-sidecar mismatch or an interrupted ownership path. A still-live moving block (a slot
// exists) returns false so the held item's use/placement path still runs. [740c583:
// BlockPistonMoving.blockActivated]
static bool onRightClickMoving(struct server_local* s, struct server_player* actor, struct item_data* it,
							   struct block_info* where, struct block_info* on,
							   enum side on_side) {
	(void)it;
	(void)where;
	(void)on_side;
	if(piston_moving_stored(s, on->x, on->y, on->z, NULL))
		return false;
	server_world_set_block(
		block_world(s), on->x, on->y, on->z, (struct block_data) {.type = BLOCK_AIR});
	return true;
}

// Beta BlockPistonMoving.onBlockRemoval: clearPistonTileEntity on the live moving TE (bug-hunt D1 --
// without this, an explosion-carved id-36 cell left its TE to settle later, ghost-writing the stored
// block into the crater). [740c583: BlockPistonMoving.onBlockRemoval]
static void piston_moving_removed(struct server_local* s, struct block_info* info) {
	piston_moving_on_removal(s, info->x, info->y, info->z);
}

// Drop the stored block when the moving cell is broken (Beta dropBlockAsItemWithChance via the store).
static size_t drop_moving(struct block_info* this, struct item_data* it, struct java_random* g,
						  struct server_local* s) {
	struct item_data stored;
	if(!s || !piston_moving_stored(s, this->x, this->y, this->z, &stored))
		return 0;
	if(stored.id >= 256 || !blocks[stored.id] || !blocks[stored.id]->getDroppedItem)
		return 0;
	struct block_data data = {
		.type = (uint8_t)stored.id,
		.metadata = (uint8_t)(stored.durability & 0x0F),
	};
	struct block_info info = *this;
	info.block = &data;
	// BlockPistonMoving.dropBlockAsItemWithChance delegates to the STORED block, so stone drops
	// cobblestone, crops use their own quantity rules, and metadata-sensitive drops stay intact.
	return blocks[stored.id]->getDroppedItem(&info, it, g, s);
}
struct block block_piston_moving = {
	.name = "Moving Piston",
	.getSideMask = getSideMask, // occludes nothing
	.getBoundingBox = getBoundingBoxMoving,
	.getMaterial = getMaterialMoving,
	.getTextureIndex = getTextureMoving,
	.getDroppedItem = drop_moving,
	.onBlockRemoval = piston_moving_removed,
	.onRandomTick = NULL,
	.onRightClickHandled = onRightClickMoving, // orphan=true; live TE=false so item placement falls through
	.transparent = true,
	.renderBlock = renderNothing,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true, // not opaque (Beta isOpaqueCube false) -- no light occlusion, no face cull
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = -1, // Beta setHardness(-1.0F): unbreakable (ps_get_hardness reads this sentinel)
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = block_place_default, // never placeable; satisfies the registry assert
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
