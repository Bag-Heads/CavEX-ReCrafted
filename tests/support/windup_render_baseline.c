/*
	Copyright (c) 2022 ByteBit/xtreme8000

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
/* entity.c
   -----------------
   Draws the entities as a simple 3D cubes instead of a billboard.
*/

#include <assert.h>
#include <string.h>

#include "../block/blocks.h"
#include "../platform/displaylist.h"
#include "../platform/gfx.h"
#include "gui_util.h"
#include "gfx_settings.h"
#include "../cglm/types.h"
#include "../cglm/cglm.h"
#include "render_item.h"
#include "render_block.h"
#include "render_entity_anchor.h" // M2 head fix: shared body/head composition
#include "render_biped_swing_pose.h" // MP visual-parity pass items 2+3
#include "../entity/combat.h" // emissive-tint flash decision lives here (audit #17f);
                              // the swell/hurt tint is now applied by the caller
#include "../item/items.h" // direct include (was transitive via blocks.h pre-split)
#include "../platform/texture.h" // animation-parity pass item 5: the 10 armor/<tier>_<layer>.png globals


static struct displaylist dl;
static uint8_t vertex_light[24], vertex_light_inv[24];

// Beta limbSwingAmount for the walk animation: 0 = legs at rest, 1 = full stride.
// entity_render sets this per mob from its (lerped) speed BEFORE calling a model
// fn, mirroring render_entity_update_light. Scales the biped/spider/creeper swing
// so a stopped mob settles its limbs instead of freezing mid-stride.
static float limb_swing_amount = 1.0f;
void render_entity_set_limb_swing(float amt) {
    limb_swing_amount = amt < 0.0f ? 0.0f : (amt > 1.0f ? 1.0f : amt);
}

typedef struct {
    uint8_t u, v, w, h;
} UVRect;

/*
    Helper function to compute the four UV corners (in order BL, BR, TR, TL)
    and then rotate them by one of {0, 90, 180, -90} degrees.

    Input:
      u0, v0, u1, v1 : original rectangle corners in texture space
      rotDegrees     : must be one of {0, 90, 180, -90}

    Output:
      outUVs[8] = { uBL, vBL, uBR, vBR, uTR, vTR, uTL, vTL }
      after applying the specified rotation.
*/
static void render_entity_fill_rotated_UVs(
    uint8_t u0, uint8_t v0,
    uint8_t u1, uint8_t v1,
    int rotDegrees,
    uint8_t outUVs[8]
) {
    // Original corners in this order:
    //   0 = bottom-left  (u0, v0)
    //   1 = bottom-right (u1, v0)
    //   2 = top-right    (u1, v1)
    //   3 = top-left     (u0, v1)
    uint8_t uvCorners[4][2] = {
        { u0, v0 },  // 0
        { u1, v0 },  // 1
        { u1, v1 },  // 2
        { u0, v1 }   // 3
    };

    int rotIdx;
    switch (rotDegrees) {
        case   0: rotIdx = 0; break;
        case  90: rotIdx = 1; break;
        case 180: rotIdx = 2; break;
        case -90: rotIdx = 3; break;
        default:  rotIdx = 0; break;  // fallback to no rotation if invalid
    }

    // After rotation, the new order of corners (BL, BR, TR, TL) is:
    //   newIndex(i) = (i + rotIdx) % 4
    for (int i = 0; i < 4; i++) {
        int src = (i + rotIdx) & 3;  // mod 4
        outUVs[i * 2 + 0] = uvCorners[src][0];
        outUVs[i * 2 + 1] = uvCorners[src][1];
    }
}

/*
    render_entity_create_cube:
    ======================
    Draws an axis-aligned box from (x0,y0,z0) to (x0+Sx, y0+Sy, z0+Sz),
    using per-vertex lighting from vertex_light[0], and sampling six independent
    UV rectangles—one per face—in this order:

      0 = bottom, 1 = top, 2 = north (+Z), 3 = south (−Z),
      4 = west  (−X), 5 = east  (+X)

    Additionally, each face i can rotate its UVs by faceRotations[i] degrees,
    where faceRotations[i] ∈ {0, 90, 180, -90}.

    faceUVs:       array of 6 UVRect structs (u, v, w, h), one per face.
    faceRotations: array of 6 ints, each 0/90/180/−90.
*/
// Emits one cube's 24 vertices. The box extents are passed ALREADY scaled to 1/16-px
// units (x0..z1). Split out from render_entity_create_cube so both the integer-px and the
// sub-pixel-float entry points share this exact emission body (no duplication / drift).
static void render_entity_emit_cube(
    int x0, int y0, int z0, int x1, int y1, int z1,
    const UVRect faceUVs[6],
    const int faceRotations[6]
) {
    const int sw = 256 / 64;
    const int sh = 256 / 32;
    const int UV_MAX = 255;

    // bottom face UV
    uint8_t ub0 = faceUVs[0].u * sw;
    uint8_t vb0 = faceUVs[0].v * sh;
    int    ub1_i = faceUVs[0].u * sw + faceUVs[0].w * sw;
    int    vb1_i = faceUVs[0].v * sh + faceUVs[0].h * sh;
    if (ub1_i > UV_MAX) ub1_i = UV_MAX;
    if (vb1_i > UV_MAX) vb1_i = UV_MAX;
    uint8_t ub1 = (uint8_t)ub1_i;
    uint8_t vb1 = (uint8_t)vb1_i;

    // top face UV
    uint8_t ut0 = faceUVs[1].u * sw;
    uint8_t vt0 = faceUVs[1].v * sh;
    int    ut1_i = faceUVs[1].u * sw + faceUVs[1].w * sw;
    int    vt1_i = faceUVs[1].v * sh + faceUVs[1].h * sh;
    if (ut1_i > UV_MAX) ut1_i = UV_MAX;
    if (vt1_i > UV_MAX) vt1_i = UV_MAX;
    uint8_t ut1 = (uint8_t)ut1_i;
    uint8_t vt1 = (uint8_t)vt1_i;

    // north face UV
    uint8_t un0 = faceUVs[2].u * sw;
    uint8_t vn0 = faceUVs[2].v * sh;
    int    un1_i = faceUVs[2].u * sw + faceUVs[2].w * sw;
    int    vn1_i = faceUVs[2].v * sh + faceUVs[2].h * sh;
    if (un1_i > UV_MAX) un1_i = UV_MAX;
    if (vn1_i > UV_MAX) vn1_i = UV_MAX;
    uint8_t un1 = (uint8_t)un1_i;
    uint8_t vn1 = (uint8_t)vn1_i;

    // south face UV
    uint8_t us0 = faceUVs[3].u * sw;
    uint8_t vs0 = faceUVs[3].v * sh;
    int    us1_i = faceUVs[3].u * sw + faceUVs[3].w * sw;
    int    vs1_i = faceUVs[3].v * sh + faceUVs[3].h * sh;
    if (us1_i > UV_MAX) us1_i = UV_MAX;
    if (vs1_i > UV_MAX) vs1_i = UV_MAX;
    uint8_t us1 = (uint8_t)us1_i;
    uint8_t vs1 = (uint8_t)vs1_i;

    // west face UV
    uint8_t uw0 = faceUVs[4].u * sw;
    uint8_t vw0 = faceUVs[4].v * sh;
    int    uw1_i = faceUVs[4].u * sw + faceUVs[4].w * sw;
    int    vw1_i = faceUVs[4].v * sh + faceUVs[4].h * sh;
    if (uw1_i > UV_MAX) uw1_i = UV_MAX;
    if (vw1_i > UV_MAX) vw1_i = UV_MAX;
    uint8_t uw1 = (uint8_t)uw1_i;
    uint8_t vw1 = (uint8_t)vw1_i;

    // east face UV
    uint8_t ue0 = faceUVs[5].u * sw;
    uint8_t ve0 = faceUVs[5].v * sh;
    int    ue1_i = faceUVs[5].u * sw + faceUVs[5].w * sw;
    int    ve1_i = faceUVs[5].v * sh + faceUVs[5].h * sh;
    if (ue1_i > UV_MAX) ue1_i = UV_MAX;
    if (ve1_i > UV_MAX) ve1_i = UV_MAX;
    uint8_t ue1 = (uint8_t)ue1_i;
    uint8_t ve1 = (uint8_t)ve1_i;

    uint8_t uniform_light = vertex_light[0];
    uint8_t l0, l1;
    uint8_t uv4[8];

    {
        int vx0 = x1, vy0 = y0, vz0 = z0;
        int vx1 = x0, vy1 = y0, vz1 = z0;
        int vx2 = x0, vy2 = y0, vz2 = z1;
        int vx3 = x1, vy3 = y0, vz3 = z1;
        l0 = uniform_light;
        render_entity_fill_rotated_UVs(ub0, vb0, ub1, vb1, faceRotations[0], uv4);
        displaylist_pos(&dl, vx0, vy0, vz0);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[0], uv4[1]);
        displaylist_pos(&dl, vx1, vy1, vz1);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[2], uv4[3]);
        displaylist_pos(&dl, vx2, vy2, vz2);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[4], uv4[5]);
        displaylist_pos(&dl, vx3, vy3, vz3);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[6], uv4[7]);
    }

    {
        int vx0 = x1, vy0 = y1, vz0 = z1;
        int vx1 = x0, vy1 = y1, vz1 = z1;
        int vx2 = x0, vy2 = y1, vz2 = z0;
        int vx3 = x1, vy3 = y1, vz3 = z0;
        l0 = uniform_light;
        render_entity_fill_rotated_UVs(ut0, vt0, ut1, vt1, faceRotations[1], uv4);
        displaylist_pos(&dl, vx0, vy0, vz0);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[0], uv4[1]);
        displaylist_pos(&dl, vx1, vy1, vz1);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[2], uv4[3]);
        displaylist_pos(&dl, vx2, vy2, vz2);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[4], uv4[5]);
        displaylist_pos(&dl, vx3, vy3, vz3);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[6], uv4[7]);
    }

    {
        int vx0 = x0, vy0 = y0, vz0 = z1;
        int vx1 = x0, vy1 = y1, vz1 = z1;
        int vx2 = x1, vy2 = y1, vz2 = z1;
        int vx3 = x1, vy3 = y0, vz3 = z1;
        l0 = uniform_light;
        l1 = uniform_light;
        render_entity_fill_rotated_UVs(un0, vn0, un1, vn1, faceRotations[2], uv4);
        displaylist_pos(&dl, vx0, vy0, vz0);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[0], uv4[1]);
        displaylist_pos(&dl, vx1, vy1, vz1);
        displaylist_color(&dl, l1);
        displaylist_texcoord(&dl, uv4[2], uv4[3]);
        displaylist_pos(&dl, vx2, vy2, vz2);
        displaylist_color(&dl, l1);
        displaylist_texcoord(&dl, uv4[4], uv4[5]);
        displaylist_pos(&dl, vx3, vy3, vz3);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[6], uv4[7]);
    }

    {
        int vx0 = x1, vy0 = y0, vz0 = z0;
        int vx1 = x1, vy1 = y1, vz1 = z0;
        int vx2 = x0, vy2 = y1, vz2 = z0;
        int vx3 = x0, vy3 = y0, vz3 = z0;
        l0 = uniform_light;
        l1 = uniform_light;
        render_entity_fill_rotated_UVs(us0, vs0, us1, vs1, faceRotations[3], uv4);
        displaylist_pos(&dl, vx0, vy0, vz0);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[0], uv4[1]);
        displaylist_pos(&dl, vx1, vy1, vz1);
        displaylist_color(&dl, l1);
        displaylist_texcoord(&dl, uv4[2], uv4[3]);
        displaylist_pos(&dl, vx2, vy2, vz2);
        displaylist_color(&dl, l1);
        displaylist_texcoord(&dl, uv4[4], uv4[5]);
        displaylist_pos(&dl, vx3, vy3, vz3);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[6], uv4[7]);
    }

    {
        int vx0 = x0, vy0 = y0, vz0 = z0;
        int vx1 = x0, vy1 = y1, vz1 = z0;
        int vx2 = x0, vy2 = y1, vz2 = z1;
        int vx3 = x0, vy3 = y0, vz3 = z1;
        l0 = uniform_light;
        l1 = uniform_light;
        render_entity_fill_rotated_UVs(uw0, vw0, uw1, vw1, faceRotations[4], uv4);
        displaylist_pos(&dl, vx0, vy0, vz0);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[0], uv4[1]);
        displaylist_pos(&dl, vx1, vy1, vz1);
        displaylist_color(&dl, l1);
        displaylist_texcoord(&dl, uv4[2], uv4[3]);
        displaylist_pos(&dl, vx2, vy2, vz2);
        displaylist_color(&dl, l1);
        displaylist_texcoord(&dl, uv4[4], uv4[5]);
        displaylist_pos(&dl, vx3, vy3, vz3);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[6], uv4[7]);
    }

    {
        int vx0 = x1, vy0 = y0, vz0 = z1;
        int vx1 = x1, vy1 = y1, vz1 = z1;
        int vx2 = x1, vy2 = y1, vz2 = z0;
        int vx3 = x1, vy3 = y0, vz3 = z0;
        l0 = uniform_light;
        l1 = uniform_light;
        render_entity_fill_rotated_UVs(ue0, ve0, ue1, ve1, faceRotations[5], uv4);
        displaylist_pos(&dl, vx0, vy0, vz0);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[0], uv4[1]);
        displaylist_pos(&dl, vx1, vy1, vz1);
        displaylist_color(&dl, l1);
        displaylist_texcoord(&dl, uv4[2], uv4[3]);
        displaylist_pos(&dl, vx2, vy2, vz2);
        displaylist_color(&dl, l1);
        displaylist_texcoord(&dl, uv4[4], uv4[5]);
        displaylist_pos(&dl, vx3, vy3, vz3);
        displaylist_color(&dl, l0);
        displaylist_texcoord(&dl, uv4[6], uv4[7]);
    }
}

// Integer-px cube (1-px model grid). The historical entry point; every bare model uses it.
static void render_entity_create_cube(
    int x0_px, int y0_px, int z0_px,
    int sx_px, int sy_px, int sz_px,
    const UVRect faceUVs[6],
    const int faceRotations[6]
) {
    render_entity_emit_cube(x0_px * 16, y0_px * 16, z0_px * 16,
                            (x0_px + sx_px) * 16, (y0_px + sy_px) * 16, (z0_px + sz_px) * 16,
                            faceUVs, faceRotations);
}

// Sub-pixel cube (1/16-px grid via roundf). Beta ModelRenderer.addBox's fractional INFLATE grows
// only the GEOMETRY (never the UV), so the wool overlay's 7.2 / 11.5 / 19.5 / 9.5-px boxes are
// drawn byte-faithfully here instead of rounded to whole px. For INTEGER-valued args it emits
// vertices BIT-IDENTICAL to render_entity_create_cube (since x_px*16 == lroundf(x_px*16.0f)).
static void render_entity_create_cube_float(
    float x0_px, float y0_px, float z0_px,
    float sx_px, float sy_px, float sz_px,
    const UVRect faceUVs[6],
    const int faceRotations[6]
) {
    render_entity_emit_cube(
        (int)lroundf(x0_px * 16.0f), (int)lroundf(y0_px * 16.0f), (int)lroundf(z0_px * 16.0f),
        (int)lroundf((x0_px + sx_px) * 16.0f), (int)lroundf((y0_px + sy_px) * 16.0f),
        (int)lroundf((z0_px + sz_px) * 16.0f), faceUVs, faceRotations);
}


void render_entity_update_light(uint8_t light) {
    memset(vertex_light,     light,  sizeof(vertex_light));
    memset(vertex_light_inv, ~light, sizeof(vertex_light_inv));
}


// minecart
//
// render_entity_minecart_init() DELETED (2026-07-31, adversarial review): it had
// no callers anywhere, and calling it would have been a bug rather than a fix --
// it re-ran displaylist_init on the SAME file-static `dl` that
// render_entity_init (the real initializer, called from main.c) already owns.
// displaylist_init sets l->data = NULL without freeing, so the second init would
// have leaked the ~20 KB MEM2 block the first one allocated. A dead function
// that only does damage when someone "helpfully" wires it up.

void render_entity_minecart(mat4 view) {
    assert(view);

    mat4 model, mv;
    glm_mat4_identity(model);
    glm_translate_make(model, (vec3){-0.5F, 0.0F,-0.5F });
    glm_mat4_mul(view, model, mv);
   // glm_rotate_y(mv, GLM_PI_2, mv);
    gfx_matrix_modelview(mv);

    gfx_bind_texture(&texture_minecart);
    displaylist_reset(&dl);


    //setup the texture coordinates for each face

    UVRect bottomSideUV[6] = {
        {  2,  12, 20, 16 }, // bottom face
		{  25, 13, 18, 14 }, // top face
        {  0,  12, 2, 16 }, // north face (+Z)
        {  22, 12, 2, 16 }, // south face (−Z)
        {  2,  10, 20, 2 }, // west face  (−X)
        {  22, 10, 20, 2 }  // east face  (+X)
    };
    int bottomSideDirection[6] = {-90,0,0,0,-90,-90};

    UVRect rightSideUV[6] = {	// is actually back
        {  2, 0, 16, 2 },
        {  2, 0, 16, 2 },
        {  2, 2, 16, 8 },
        { 20, 2, 16, 8 },
        { 18, 2, 2,  8 },
        {  0, 2, 2,  8 }
    };
    int rightSideDirection[6] = {-90,180,-90,-90,90,90};

    UVRect leftSideUV[6] = {		// is actually front
            {  2, 0, 16, 2 },
            {  2, 0, 16, 2 },
            { 20, 2, 16, 8 },
            {  2, 2, 16, 8 },
            {  0, 2, 2,  8 },
			{ 18, 2, 2,  8 }
    };
    int leftSideDirection[6] = {-90,0,-90,-90,90,90};

    UVRect backSideUV[6] = {		// is actually right // kant van de fakkels
            {  2, 0, 16, 2 },
            {  2, 0, 16, 2 },
            { 18, 2, 2,  8 },
            {  0, 2, 2,  8 },
            { 20, 2, 16, 8 },
            {  2, 2, 16, 8 }

    };
    int backSideDirection[6] = {90,-90,0,0,-90,-90};


    UVRect frontSideUV[6] = {		// is actually left
            {  2, 0, 16, 2 },
            {  2, 0, 16, 2 },
            { 18, 2, 2,  8 },
            {  0, 2, 2,  8 },
            {  2, 2, 16, 8 },
            { 20, 2, 16, 8 },

    };
    int frontSideDirection[6] =  {90,90,0,0,-90,-90};





// generate the cubes
    render_entity_create_cube(
        0, 0, 0,
        16, 2, 20,
		bottomSideUV,
		bottomSideDirection
    );

    render_entity_create_cube(
		0, 2, 0,
       16, 8, 2,
	    rightSideUV,
		rightSideDirection
    );

    render_entity_create_cube(
        0, 2, 18,
		16, 8, 2,
        leftSideUV,
		leftSideDirection
    );

    render_entity_create_cube(
        0, 2, 2,
        2, 8, 16,
		backSideUV,
		backSideDirection
    );

    render_entity_create_cube(
        14, 2, 2,
        2, 8, 16,
		frontSideUV,
		frontSideDirection
    );

    displaylist_render_immediate(&dl, 24*5);

    gfx_lighting(true);
    gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}


void render_entity_init(void) {
    displaylist_init(&dl, 320, 64);
    memset(vertex_light,     0x0F, sizeof(vertex_light));
    memset(vertex_light_inv, 0xFF, sizeof(vertex_light_inv));
}

// creeper
void render_entity_creeper(mat4 view, float headYawDeg, float bodyYawDeg, int frame, int fuse, float td){
	assert(view);

	// Beta swell flash, PER-INSTANCE and driven by THIS creeper's networked fuse
	// phase -- NOT a function-static clock shared by every creeper on screen (which
	// ramped ~Nx too fast with N swelling and shared one phase). RenderCreeper
	// getCreeperFlash = (timeSinceIgnited + partialTick) / (fuseTime - 2); Beta
	// toggles the whitening overlay as that ramps 0->1, so the flash accelerates
	// toward detonation.
	//
	// audit #17f: the white swell wash is now a TRUE white emissive overlay applied by
	// the CALLER (entity_render) via the FIFO-safe gfx_entity_tint, replacing the old
	// render_entity_update_light(0xFF) max-light hack -- that could only brighten the
	// grayscale light index, not reach the RGB channel, so it never actually whitened.
	// The tint (and its mandatory reset-to-0) is bracketed around the whole per-mob
	// draw in entity_render so the red hurt-flash and the white swell share one code
	// path and one reset; this function just draws the model. `fuse`/`td` are still
	// accepted for signature stability (the flash amount is derived from them upstream).
	(void)fuse;
	(void)td;

    float walkAngle = sinf((float)(frame % 60) / 60.0f * 2.0f * M_PI) * (30.0f * (M_PI / 180.0f)) * limb_swing_amount;

    // Body matrix (M2 head fix: the shared composition, render_entity_anchor.h;
    // pivot already on the model axis -- the refactor is output-identical for
    // the creeper, pinned by test_head_anchor).
    mat4 body_mv;
    vec3 bodyPivot = {
        8.0f / 16.0f,
        (6.0f + 12.0f / 2.0f) / 16.0f,
        8.0f / 16.0f
    };
    render_entity_body_mv(view, bodyPivot, bodyYawDeg, body_mv);

    gfx_bind_texture(&texture_creeper);

    // Head: rides body_mv with its yaw DELTA about the Beta neck rp(0,6,0)
    // -> v2 (8,18,8) (on the rotation axis -> provably unchanged output).
    displaylist_reset(&dl);
    {
        vec3 neck = { 8.0f / 16.0f, 18.0f / 16.0f, 8.0f / 16.0f };
        mat4 head_mv;
        render_entity_head_mv(body_mv, neck, headYawDeg, bodyYawDeg, 0.0f, head_mv);
        gfx_matrix_modelview(head_mv);
    }

	static const UVRect creeperHeadUVs[6] = {
		{16,  0, 8, 8}, {8,   0, 8, 8},
		{0,   8, 8, 8}, {8,   8, 8, 8},
		{16,  8, 8, 8}, {24,  8, 8, 8}
	};
	int creeperHeadDirection[6] = {-90, -90, -90, -90, -90, -90};

	render_entity_create_cube(
		4, 18, 4,
		8,  8,  8,
		creeperHeadUVs,
		creeperHeadDirection
	);
	displaylist_render_immediate(&dl, 24);

	// Body
    displaylist_reset(&dl);
    gfx_matrix_modelview(body_mv);

    static const UVRect creeperBodyUVs[6] = {
        {20, 16, 8, 4}, {28, 16, 8, 4},
        {32, 20, 8, 12}, {20, 20, 8, 12},
        {28, 20, 4, 12}, {16, 20, 4, 12}
    };
    int creeperBodyDirection[6] = {0, 0, -90, -90, -90, -90};

    render_entity_create_cube(
        4, 6, 6,
        8, 12, 4,
        creeperBodyUVs,
        creeperBodyDirection
    );
    displaylist_render_immediate(&dl, 24);

    // Legs
    static const UVRect creeperLegUVs[6] = {
        {8, 16, 4, 4}, {4, 16, 4, 4},
        {12, 20, 4, 6}, {4, 20, 4, 6},
        {8, 20, 4, 6}, {0, 20, 4, 6}
    };
    static const int creeperLegDirection[6] = {0, 0, -90, -90, -90, -90};

    const int width = 4, height = 6, depth = 4;
    int legPositions[4][3] = {
        { 4, 0,  2 }, // front-left
        { 8, 0,  2 }, // front-right
        { 4, 0, 10 }, // back-left
        { 8, 0, 10 }  // back-right
    };
    int legAnimDir[4] = { +1, -1, -1, +1 };

    for (int i = 0; i < 4; i++) {
        displaylist_reset(&dl);

        bool isFront = (i < 2);
        vec3 pivot = {
            (width / 2.0f) / 16.0f,
            height / 16.0f,
            (isFront ? depth : 0.0f) / 16.0f
        };

        mat4 leg_model;
        glm_mat4_identity(leg_model);
        glm_translate_make(leg_model, (vec3){
            legPositions[i][0] / 16.0f,
            legPositions[i][1] / 16.0f,
            legPositions[i][2] / 16.0f
        });
        glm_translate(leg_model, pivot);
        glm_rotate_x(leg_model, legAnimDir[i] * walkAngle, leg_model);
        glm_translate(leg_model, (vec3){ -pivot[0], -pivot[1], -pivot[2] });

        mat4 leg_mv;
        glm_mat4_mul(body_mv, leg_model, leg_mv);
        gfx_matrix_modelview(leg_mv);

        render_entity_create_cube(
            0, 0, 0,
            width, height, depth,
            creeperLegUVs, creeperLegDirection
        );
        displaylist_render_immediate(&dl, 24);
    }

    gfx_lighting(true);
    gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}

// Shared Beta 1.7.3 ModelBiped (head 8x8x8, body 8x12x4, two arms + two legs
// 4x12x4) on the 64x32 sheet. BOTH the zombie and the skeleton ARE this biped
// (Beta 1.7.3 has no dedicated ModelSkeleton -- the thin 2-wide arms are 1.8+);
// they differ only by texture, so the geometry/UVs/pose live here once. The
// distinguishing pose is the arms held straight out forward (-90deg), swinging
// opposite the same-side leg -- correct for the zombie's reaching arms and the
// skeleton's bow-holding arms. The body below is the original render_entity_zombie
// verbatim (only the bound texture is now a parameter), so the zombie's rendered
// vertices are byte-identical by construction.
// arms_forward selects the Beta arm POSE (not a cosmetic tweak -- see
// render_entity_player's CRITICAL FAITHFULNESS TRAP note below): true is
// ModelZombie's arms-reaching-forward override (zombie/skeleton), false is
// plain ModelBiped's arms-at-the-sides pose (player). Both share the exact
// same swing term (limbAnim[]); only the resting base differs.
static void render_entity_biped(mat4 view, float headYawDeg, float bodyYawDeg,
                                float headPitchDeg,
                                int frame, struct tex_gfx* tex, bool thin,
                                bool arms_forward, int held_item,
                                bool is_sneak, float swing_progress,
                                bool is_riding, const uint16_t armor_ids[4]) {
    assert(view);

    float walkAngle = sinf((float)(frame % 60) / 60.0f * 2.0f * M_PI)
                      * (30.0f * (M_PI / 180.0f)) * limb_swing_amount;

    // MP visual-parity pass items 2+3: every OTHER biped caller (zombie/
    // skeleton/pigzombie) passes is_sneak=false, swing_progress=0.0f, for
    // which every pose field below is EXACTLY zero/baseline (provable: see
    // render_biped_swing_pose.h's header + test_mp_visual_parity_pose.c) --
    // so their rendered vertices are unchanged by this addition.
    struct biped_swing_pose pose
        = render_biped_compute_swing_pose(swing_progress, is_sneak,
                                          glm_rad(headPitchDeg));
    // ModelBiped uses Y down; these boxes use (x+8, 24-y, z+8).
    // Reflect translations in Y and rotations about X/Z at this boundary.
    // Keep the reference pose helper in Beta coordinates for its golden tests.
    pose.body_rot_x = -pose.body_rot_x;
    pose.right_arm_rot_x_delta = -pose.right_arm_rot_x_delta;
    pose.left_arm_rot_x_delta = -pose.left_arm_rot_x_delta;
    pose.right_arm_rot_z = -pose.right_arm_rot_z;
    pose.leg_point_y_px_delta = -pose.leg_point_y_px_delta;
    pose.head_point_y_px_delta = -pose.head_point_y_px_delta;

    // Body matrix (M2 head fix: shared composition; pivot already on the
    // model axis -- output-identical for the bipeds, pinned by
    // test_head_anchor's matrix-identity case).
    mat4 body_mv;
    vec3 bodyPivot = {
        8.0f / 16.0f,
        (12.0f + 12.0f / 2.0f) / 16.0f,
        8.0f / 16.0f
    };
    render_entity_body_mv(view, bodyPivot, bodyYawDeg, body_mv);

    // Per-mob biped skin from the active pack; fall back to the creeper texture if
    // the pack omits it (documented visual degradation, not a silent stub).
    gfx_bind_texture(tex->data ? tex : &texture_creeper);

    // Shared Beta neck point rp(0,0,0) -> v2 (8,24,8), px/16 -- [740c583
    // ModelBiped ctor] bipedHead AND bipedBody share this SAME raw
    // rotationPoint, so it is also the correct pivot for the body's own
    // extra swing/sneak rotation below (bipedBody.rotateAngleX/Y), not just
    // the head's yaw delta.
    vec3 neck = { 8.0f / 16.0f, 24.0f / 16.0f, 8.0f / 16.0f };

    // --- Head (8x8x8): rides body_mv with its yaw DELTA about the Beta neck
    // (on the axis -> provably unchanged output at pose.head_point_y_px_delta
    // == 0, the non-sneak case). head_neck applies ONLY the sneak Y-shift
    // [740c583 ModelBiped.setRotationAngles: `bipedHead.rotationPointY =
    // 1.0F` while sneaking] -- a property of the HEAD piece alone, distinct
    // from bipedBody's own (untouched) rotationPoint. ---
    displaylist_reset(&dl);
    // Animation-parity pass item 5: head_mv is hoisted out of its old inner
    // block scope so the helmet armor overlay pass below can reuse it
    // verbatim (same matrix, same pivot) instead of recomputing it.
    mat4 head_mv;
    {
        // The mesh is authored at the standing neck. Translate the whole
        // piece before rotating it; changing only its pivot cancels at rest.
        mat4 head_body_mv;
        glm_mat4_copy(body_mv, head_body_mv);
        if(pose.head_point_y_px_delta != 0.0f)
            glm_translate(head_body_mv,
                          (vec3){0, pose.head_point_y_px_delta / 16.0f, 0});
        render_entity_head_mv(head_body_mv, neck, headYawDeg, bodyYawDeg,
                              headPitchDeg, head_mv);
        gfx_matrix_modelview(head_mv);
    }
    static const UVRect headUV[6] = {
        {16, 0, 8, 8}, {8, 0, 8, 8},
        {0, 8, 8, 8},  {8, 8, 8, 8},
        {16, 8, 8, 8}, {24, 8, 8, 8}
    };
    int headDir[6] = {-90, -90, -90, -90, -90, -90};
    render_entity_create_cube(4, 24, 4, 8, 8, 8, headUV, headDir);
    displaylist_render_immediate(&dl, 24);

    // --- Body (8x12x4) ---
    displaylist_reset(&dl);
    // bipedBody's OWN rotateAngleY/X (swing twist + sneak pitch), applied
    // about the SAME neck-point axis, riding body_mv -- [740c583
    // ModelRenderer.render: T(rotationPoint) then Rz, Ry, Rx in that order;
    // rotationPoint is (0,0,0) so ONLY the rotation matters, no extra
    // translate needed beyond the shared neck pivot dance below]. At
    // pose.body_rot_y == pose.body_rot_x == 0 (every non-swinging,
    // non-sneaking case) this collapses to body_mv unchanged (both
    // glm_rotate_y/x are true identity ops on a zero angle).
    mat4 body_pose_mv;
    glm_mat4_copy(body_mv, body_pose_mv);
    if(pose.body_rot_y != 0.0f || pose.body_rot_x != 0.0f) {
        glm_translate(body_pose_mv, neck);
        glm_rotate_y(body_pose_mv, pose.body_rot_y, body_pose_mv);
        glm_rotate_x(body_pose_mv, pose.body_rot_x, body_pose_mv);
        glm_translate(body_pose_mv, (vec3) {-neck[0], -neck[1], -neck[2]});
    }
    gfx_matrix_modelview(body_pose_mv);
    static const UVRect bodyUV[6] = {
        {20, 16, 8, 4}, {28, 16, 8, 4},
        {32, 20, 8, 12}, {20, 20, 8, 12},
        {28, 20, 4, 12}, {16, 20, 4, 12}
    };
    int bodyDir[6] = {0, 0, -90, -90, -90, -90};
    render_entity_create_cube(4, 12, 6, 8, 12, 4, bodyUV, bodyDir);
    displaylist_render_immediate(&dl, 24);

    // --- Arms + legs ---
    // Standard ModelBiped limbs are 4x12x4. Beta 1.7.3 ALSO ships
    // ModelSkeleton (ModelSkeleton.java, extends ModelZombie): 2x12x2 arms @
    // texOffs(40,16) and 2x12x2 legs @ texOffs(0,16), same shoulder/hip
    // rotation points [740c583: ModelSkeleton.java]. (The old comment at
    // render_entity_skeleton claiming thin limbs are 1.8+ was WRONG -- the
    // class is right there in the b1.7.3 pack.) `thin` selects that geometry:
    // boxes shrink to 2px centred on the same pivots (fat x0 0/12 + 1 -> thin
    // 1/13; legs 4/8 + 1 -> 5/9; depth 6 + 1 -> 7), UV rects re-derived with
    // the same face order as the 4-wide set (w=d=2).
    static const UVRect legUV[6] = {
        {8, 16, 4, 4}, {4, 16, 4, 4},
        {12, 20, 4, 12}, {4, 20, 4, 12},
        {8, 20, 4, 12}, {0, 20, 4, 12}
    };
    static const UVRect armUV[6] = {
        {48, 16, 4, 4}, {44, 16, 4, 4},
        {52, 20, 4, 12}, {44, 20, 4, 12},
        {48, 20, 4, 12}, {40, 20, 4, 12}
    };
    static const UVRect legUVthin[6] = {
        {4, 16, 2, 2}, {2, 16, 2, 2},
        {6, 18, 2, 12}, {2, 18, 2, 12},
        {4, 18, 2, 12}, {0, 18, 2, 12}
    };
    static const UVRect armUVthin[6] = {
        {44, 16, 2, 2}, {42, 16, 2, 2},
        {46, 18, 2, 12}, {42, 18, 2, 12},
        {44, 18, 2, 12}, {40, 18, 2, 12}
    };
    static const int limbDir[6] = {0, 0, -90, -90, -90, -90};

    const int limbW = thin ? 2 : 4; // box width == depth
    // ModelZombie's arms-forward override (zombie/skeleton) vs plain
    // ModelBiped's arms-at-the-sides rest pose (player) -- see the
    // arms_forward parameter doc above and render_entity_player's trap note.
    const float armBase = arms_forward ? +glm_rad(90.0f) : 0.0f; // FORWARD (-Z, toward the face) or hanging
    const UVRect* limbUV[4]; // R leg, L leg, R arm, L arm
    limbUV[0] = limbUV[1] = thin ? legUVthin : legUV;
    limbUV[2] = limbUV[3] = thin ? armUVthin : armUV;
    // Index order throughout this loop: 0 R leg, 1 L leg, 2 R arm, 3 L arm
    // (matching limbUV/limbAnim above). X0/Y0/Z0 are now float PIXEL
    // placements (were `const int`) so the swing/sneak deltas below can be
    // added in the SAME unit before the shared /16.0f conversion; every
    // delta is exactly 0.0f for is_sneak=false/swing_progress=0.0f (see
    // render_biped_swing_pose.h), so this is output-identical for every
    // other biped caller.
    float limbX0[4] = {thin ? 5.0f : 4.0f, thin ? 9.0f : 8.0f,
                       (thin ? 1.0f : 0.0f) + pose.right_arm_point_x_px
                           - (-5.0f), // MP item 2/3: delta vs. Beta's own
                                      // -5.0F static baseline (ModelBiped ctor)
                       (thin ? 13.0f : 12.0f) + pose.left_arm_point_x_px
                           - 5.0f}; // delta vs. Beta's own +5.0F baseline
    float limbY0[4] = {0.0f + pose.leg_point_y_px_delta,
                       0.0f + pose.leg_point_y_px_delta, 12.0f,
                       12.0f}; // legs at hip level, arms at shoulder
    float limbZ0[4]
        = {(thin ? 7.0f : 6.0f) + pose.leg_point_z_px_delta,
          (thin ? 7.0f : 6.0f) + pose.leg_point_z_px_delta,
          (thin ? 7.0f : 6.0f) + pose.right_arm_point_z_px - 0.0f,
          (thin ? 7.0f : 6.0f) + pose.left_arm_point_z_px - 0.0f};
    // Animation-parity pass item 4: fold in ModelBiped's isRiding pose --
    // see render_biped_swing_pose.h's render_biped_riding_pose() for the
    // full [740c583] citation and the "output-identical application order"
    // proof. Every field here is exactly the pre-pass value when
    // is_riding is false (the SAME "no other caller's output changes"
    // guarantee the swing/sneak pose above already carries).
    struct biped_riding_pose ride = render_biped_riding_pose();
    const float limbBaseX[4] = {
        is_riding ? ride.leg_rot_x : 0.0f, is_riding ? ride.leg_rot_x : 0.0f,
        armBase, armBase
    };
    // rotateAngleX total per limb: walk swing (limbAnim) + the swing-
    // progress/sneak delta from the pose (arms only; legs get 0.0f) + the
    // riding delta (arms only -- legs get their OWN absolute limbBaseX
    // above instead, with limbAnim zeroed below so walk-swing does not
    // stack onto the riding pose).
    const float limbRotXDelta[4]
        = {0.0f, 0.0f,
          pose.right_arm_rot_x_delta + (is_riding ? ride.arm_rot_x_delta : 0.0f),
          pose.left_arm_rot_x_delta + (is_riding ? ride.arm_rot_x_delta : 0.0f)};
    const float limbAnim[4] = {
        is_riding ? 0.0f : +walkAngle, is_riding ? 0.0f : -walkAngle,
        -walkAngle, +walkAngle
    };
    // rotateAngleY/Z: arms only in Beta's baseline ModelBiped, EXCEPT the
    // riding pose above which is the only thing that ever sets a leg's
    // rotateAngleY; [740c583] Z-then-Y-then-X GL call order
    // [ModelRenderer.render].
    const float limbRotY[4] = {
        is_riding ? ride.right_leg_rot_y : 0.0f,
        is_riding ? ride.left_leg_rot_y : 0.0f,
        pose.right_arm_rot_y, pose.left_arm_rot_y
    };
    const float limbRotZ[4] = {0.0f, 0.0f, pose.right_arm_rot_z, 0.0f};

    // Animation-parity pass item 5: captured for the armor overlay pass
    // below (helmet/chest reuse head_mv/body_pose_mv directly; legs/boots
    // need both leg matrices, chest also needs both arm matrices -- only
    // the right arm was captured before this pass, for the held item).
    mat4 right_arm_mv, left_arm_mv, right_leg_mv, left_leg_mv;
    for(int i = 0; i < 4; i++) {
        displaylist_reset(&dl);
        vec3 pivot = {
            ((float)limbW / 2.0f) / 16.0f,
            12.0f / 16.0f, // top of the 12px limb -> hip (legs) / shoulder (arms)
            ((float)limbW / 2.0f) / 16.0f
        };
        mat4 limb_model;
        glm_mat4_identity(limb_model);
        glm_translate_make(limb_model, (vec3){
            limbX0[i] / 16.0f, limbY0[i] / 16.0f, limbZ0[i] / 16.0f
        });
        glm_translate(limb_model, pivot);
        // [740c583 ModelRenderer.render]: Z, then Y, then X -- each
        // conditional on a nonzero angle, mirroring Beta's own
        // nonzero-Z-angle guards (T1: a true no-op for
        // legs and every non-swinging/non-sneaking arm).
        if(limbRotZ[i] != 0.0f)
            glm_rotate_z(limb_model, limbRotZ[i], limb_model);
        if(limbRotY[i] != 0.0f)
            glm_rotate_y(limb_model, limbRotY[i], limb_model);
        glm_rotate_x(limb_model, limbBaseX[i] + limbAnim[i] + limbRotXDelta[i],
                    limb_model);
        glm_translate(limb_model, (vec3){-pivot[0], -pivot[1], -pivot[2]});
        mat4 limb_mv;
        glm_mat4_mul(body_mv, limb_model, limb_mv);
        // Index order (see above): 0 R leg, 1 L leg, 2 R arm, 3 L arm.
        switch(i) {
        case 0: glm_mat4_copy(limb_mv, right_leg_mv); break;
        case 1: glm_mat4_copy(limb_mv, left_leg_mv); break;
        case 2: glm_mat4_copy(limb_mv, right_arm_mv); break;
        case 3: glm_mat4_copy(limb_mv, left_arm_mv); break;
        }
        gfx_matrix_modelview(limb_mv);
        render_entity_create_cube(0, 0, 0, limbW, 12, limbW, limbUV[i],
                                  limbDir);
        displaylist_render_immediate(&dl, 24);
    }

    /* --- Held item (Beta RenderBiped.renderEquippedItems [740c583:
     * RenderBiped.java]) -- what makes a pig zombie carry its gold sword and
     * a skeleton its bow (EntityPigZombie/EntitySkeleton defaultHeldItem;
     * plain zombies hold nothing). Beta's chain: bipedRightArm.postRender,
     * translate(-0.0625, 0.4375, 0.0625), then per item kind -- isFull3D
     * (swords/tools): +0.1875 down, scale(0.625, -0.625, 0.625), Rx(-100),
     * Ry(45); flat items (bow): translate(0.25, 0.1875, -0.1875),
     * scale(0.375), Rz(60), Rx(-90), Rz(20) -- then ItemRenderer draws the
     * icon in its [0..1]^2 space. v2 re-expresses that chain in OUR
     * right-arm frame (Y-up, origin at the limb box corner, hand at the box
     * bottom): the arm already carries the 90-degree forward pose, so Beta's
     * Rx(-100)/Rx(-90) decompose as "stand the icon back up out of the arm's
     * pitch" (-90) plus the residual tilt; the Y-flip in Beta's scale undoes
     * its Y-down model space and dropss out entirely here. Angles/anchors are
     * presentation-tuned to Beta's on-screen pose (render-layer; the icon
     * art, scales 0.625/0.375, and the 45-degree blade yaw are Beta's
     * numbers). render_item_flat(R_ITEM_ENV_MOB_HAND) restores the lit GX
     * descriptor exactly like the dropped-item env, so the follow-on
     * entity/shadow draws stay FIFO-safe. */
    /* Bounds gate BEFORE the registry probe: the mob callers pass
     * compile-time constants (ITEM_BOW, ITEM_GOLD_SWORD), but
     * render_entity_player forwards a NETWORK-supplied id, and
     * items[held_item] itself is the deref -- without `< ITEMS_MAX` a wire
     * id like 65535 reads ~253KB past the 2304-entry registry
     * (adversarial-review CRITICAL). Unknown-but-in-range ids fall out on
     * the NULL registry slot and draw nothing. */
    if(held_item > 0 && held_item < ITEMS_MAX && items[held_item]) {
        struct item_data stack
            = (struct item_data) {.id = held_item, .durability = 0, .count = 1};
        mat4 hand_model, hand_mv;
        glm_mat4_identity(hand_model);
        // Hand point: box-centre X/Z, just below the wrist.
        glm_translate_make(hand_model,
                           (vec3) {((float)limbW / 2.0f) / 16.0f, 1.0f / 16.0f,
                                   ((float)limbW / 2.0f) / 16.0f});
        // Beta Item.isFull3D == the ItemTool/ItemSword/ItemHoe overrides; in
        // the v2 registry that is exactly "has a tool type".
        if(items[held_item]->tool.type != TOOL_TYPE_ANY) {
            // Sword: upright out of the fist, blade tipped 10 forward past
            // vertical (Beta Rx(-100) = arm's -90 + 10), plane yawed 45.
            glm_rotate_x(hand_model, glm_rad(-100.0f), hand_model);
            glm_rotate_y(hand_model, glm_rad(45.0f), hand_model);
            glm_scale_uni(hand_model, 0.625f);
        } else {
            // Bow: across the fist -- stood up out of the arm pitch, then
            // Beta's roll pair (60 + 20) about the blade axis.
            glm_rotate_x(hand_model, glm_rad(-90.0f), hand_model);
            glm_rotate_z(hand_model, glm_rad(80.0f), hand_model);
            glm_scale_uni(hand_model, 0.375f);
        }
        // Icon anchor: centre the [0..1]^2 icon on the fist, handle corner
        // overlapping it (Beta anchors the icon corner and offsets; one
        // centring translate is the v2-frame equivalent).
        glm_translate(hand_model, (vec3) {-0.5f, -0.15f, -1.0f / 32.0f});
        glm_mat4_mul(right_arm_mv, hand_model, hand_mv);
        items[held_item]->renderItem(items[held_item], &stack, hand_mv, false,
                                     R_ITEM_ENV_MOB_HAND);
    }

    /* Animation-parity pass (2026-09-03) item 5: RenderPlayer's armor-layer
     * pass [740c583 RenderPlayer.setArmorModel + doRenderLiving's four
     * render passes, numbered zero through three]. Beta walks armorType
     * 0..3 (helmet, chest, legs, boots) in that order, each pass picking a
     * texture "/armor/<tier>_<layer>.png" (layer 2 only for legs, else
     * layer 1) and ONE of two shared armor models selected by showModel
     * flags: modelArmorChestplate (inflate 1.0) for helmet/chest/boots,
     * modelArmor (inflate 0.5) for legs -- boots therefore reuse the SAME
     * leg geometry as leggings, just a different texture+inflate+model,
     * drawn AFTER (so a close-fitting legging peeks out beneath a looser
     * boot exactly as real Beta renders it). NULL armor_ids (every
     * non-player biped caller) skips this whole block -- zero output
     * change for zombie/skeleton/pigzombie/giant.
     *
     * SIMPLIFICATION (documented): Beta's armor models ALSO show
     * bipedHeadwear (a second, slightly-larger "hat layer" box) alongside
     * bipedHead for the helmet pass -- a cosmetic brim/overlay detail with
     * no simulation weight. Omitted here; the helmet still renders as the
     * correct material/texture on the correct geometry. */
    if(armor_ids) {
        // Tier -> {layer1, layer2} texture pair. Restated locally rather
        // than sharing render_model.c's own copy of this table (this file's
        // established precedent for small per-file constant tables -- see
        // REMOTE_PLAYER_YAW_SMOOTH_FACTOR's comment in entity_remote_player.c).
        static struct tex_gfx* const armor_tex[][2] = {
            [ARMOR_TIER_CHAIN] = {&texture_armor_chain1, &texture_armor_chain2},
            [ARMOR_TIER_CLOTH] = {&texture_armor_cloth1, &texture_armor_cloth2},
            [ARMOR_TIER_GOLD] = {&texture_armor_gold1, &texture_armor_gold2},
            [ARMOR_TIER_IRON] = {&texture_armor_iron1, &texture_armor_iron2},
            [ARMOR_TIER_DIAMOND]
            = {&texture_armor_diamond1, &texture_armor_diamond2},
        };
        static const enum armor_type pass_order[4]
            = {ARMOR_TYPE_HELMET, ARMOR_TYPE_CHESTPLATE, ARMOR_TYPE_LEGGINGS,
              ARMOR_TYPE_BOOTS};
        for(int pass = 0; pass < 4; pass++) {
            enum armor_type at = pass_order[pass];
            int id = armor_ids[at];
            if(id <= 0 || id >= ITEMS_MAX || !items[id]
               || !items[id]->armor.is_armor || items[id]->armor.type != at)
                continue; // empty slot, or a wire id that isn't this slot's armor
            enum armor_tier tier = items[id]->armor.tier;
            if(tier < 0
               || (size_t) tier >= sizeof(armor_tex) / sizeof(armor_tex[0])
               || !armor_tex[tier][0])
                continue; // ARMOR_TIER_BLOCK (pumpkin-style) has no cloth layer here
            bool is_legs = at == ARMOR_TYPE_LEGGINGS;
            struct tex_gfx* atex = armor_tex[tier][is_legs ? 1 : 0];
            if(!atex->data)
                continue;
            // modelArmor (legs) inflate 0.5px; modelArmorChestplate
            // (everything else, boots included) inflate 1.0px -- Beta's own
            // ModelBiped(float) constructor units, the SAME 1/16-block "px"
            // scale render_entity_create_cube_float already divides by.
            float inflate = is_legs ? 0.5f : 1.0f;
            gfx_bind_texture(atex);
            switch(at) {
            case ARMOR_TYPE_HELMET:
                displaylist_reset(&dl);
                gfx_matrix_modelview(head_mv);
                render_entity_create_cube_float(4.0f - inflate, 24.0f - inflate,
                                                4.0f - inflate, 8.0f + 2.0f * inflate,
                                                8.0f + 2.0f * inflate,
                                                8.0f + 2.0f * inflate, headUV, headDir);
                displaylist_render_immediate(&dl, 24);
                break;
            case ARMOR_TYPE_CHESTPLATE:
                displaylist_reset(&dl);
                gfx_matrix_modelview(body_pose_mv);
                render_entity_create_cube_float(4.0f - inflate, 12.0f - inflate,
                                                6.0f - inflate, 8.0f + 2.0f * inflate,
                                                12.0f + 2.0f * inflate,
                                                4.0f + 2.0f * inflate, bodyUV, bodyDir);
                displaylist_render_immediate(&dl, 24);
                displaylist_reset(&dl);
                gfx_matrix_modelview(right_arm_mv);
                render_entity_create_cube_float(-inflate, -inflate, -inflate,
                                                (float) limbW + 2.0f * inflate,
                                                12.0f + 2.0f * inflate,
                                                (float) limbW + 2.0f * inflate,
                                                limbUV[2], limbDir);
                displaylist_render_immediate(&dl, 24);
                displaylist_reset(&dl);
                gfx_matrix_modelview(left_arm_mv);
                render_entity_create_cube_float(-inflate, -inflate, -inflate,
                                                (float) limbW + 2.0f * inflate,
                                                12.0f + 2.0f * inflate,
                                                (float) limbW + 2.0f * inflate,
                                                limbUV[3], limbDir);
                displaylist_render_immediate(&dl, 24);
                break;
            case ARMOR_TYPE_LEGGINGS:
            case ARMOR_TYPE_BOOTS:
                displaylist_reset(&dl);
                gfx_matrix_modelview(right_leg_mv);
                render_entity_create_cube_float(-inflate, -inflate, -inflate,
                                                (float) limbW + 2.0f * inflate,
                                                12.0f + 2.0f * inflate,
                                                (float) limbW + 2.0f * inflate,
                                                limbUV[0], limbDir);
                displaylist_render_immediate(&dl, 24);
                displaylist_reset(&dl);
                gfx_matrix_modelview(left_leg_mv);
                render_entity_create_cube_float(-inflate, -inflate, -inflate,
                                                (float) limbW + 2.0f * inflate,
                                                12.0f + 2.0f * inflate,
                                                (float) limbW + 2.0f * inflate,
                                                limbUV[1], limbDir);
                displaylist_render_immediate(&dl, 24);
                break;
            }
        }
        // Restore the skin/mob texture binding: a caller after this one
        // (or the next entity's own first bind) must not inherit the last
        // armor texture left bound here.
        gfx_bind_texture(tex->data ? tex : &texture_creeper);
    }

    gfx_lighting(true);
    gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}

// zombie -- standard Beta 1.7.3 ModelBiped, textured from mob/zombie.png. Public
// signature + output unchanged; the body now lives in render_entity_biped.
// EntityZombie has no getHeldItem override -> empty hands [740c583].
void render_entity_zombie(mat4 view, float headYawDeg, float bodyYawDeg, int frame) {
    render_entity_biped(view, headYawDeg, bodyYawDeg, 0.0f, frame, &texture_zombie,
                        false, true, 0, false, 0.0f, false, NULL);
}

// player (MP remote-player render follow-up) -- Beta 1.7.3 RenderPlayer over
// ModelBiped: same shared geometry as the zombie/skeleton above, textured
// with the pack's mob/char.png (texture_mob_char) instead of a monster skin.
// CRITICAL FAITHFULNESS TRAP: ModelZombie (which render_entity_biped's body
// is transcribed from) holds BOTH arms FIXED forward (armBase = +90deg about
// X) and swings them opposite the same-side leg -- correct for a zombie's
// reaching arms / a
// skeleton's bow-holding arms, but NOT for the player. ModelBiped (the
// player's own, undecorated model) hangs the arms straight DOWN at the sides
// and swings them with plain rotateAngleX = cos(limbSwing*0.6662)*1.4*
// limbSwingAmount [740c583: ModelBiped.setRotationAngles -- no forward
// rotationPointZ offset on bipedLeftArm/bipedRightArm, unlike ModelZombie's
// override]. render_entity_biped's `arms_forward` parameter selects between
// these two arm poses (false here == player); `false` is also physically
// correct for the biped's HELD ITEM anchor, which rides the right arm's
// matrix (right_arm_mv) and must sit in the hand at rest, not out in front of
// the face. EntityPlayer's held item otherwise uses the identical
// RenderBiped.renderEquippedItems path as every other biped.
void render_entity_player(mat4 view, float headYawDeg, float bodyYawDeg,
						  float headPitchDeg, int frame, int held_item,
						  bool is_sneak, float swing_progress,
						  bool is_riding, const uint16_t armor_ids[4]) {
	render_entity_biped(view, headYawDeg, bodyYawDeg, headPitchDeg, frame,
						&texture_mob_char, false, false, held_item, is_sneak,
						swing_progress, is_riding, armor_ids);
}

// skeleton -- Beta 1.7.3 RenderBiped over ModelSkeleton: THIN 2x12x2 limbs
// (arms texOffs 40,16 / legs 0,16, same pivots) + the BOW defaultHeldItem
// [740c583: RenderManager.java:34, ModelSkeleton.java, EntitySkeleton
// defaultHeldItem]. (This renderer previously claimed thin limbs were 1.8+ --
// wrong: ModelSkeleton is in the b1.7.3 pack.)
void render_entity_skeleton(mat4 view, float headYawDeg, float bodyYawDeg, int frame) {
    render_entity_biped(view, headYawDeg, bodyYawDeg, 0.0f, frame, &texture_skeleton,
                        true, true, ITEM_BOW, false, 0.0f, false, NULL);
}

// pig zombie (batch 1b) -- Beta 1.7.3 EntityPigZombie renders the zombie biped
// with /mob/pigzombie.png and the GOLD SWORD defaultHeldItem [740c583:
// EntityPigZombie.java:8/89 -- the held-item deferral is repaid].
void render_entity_pigzombie(mat4 view, float headYawDeg, float bodyYawDeg,
                             int frame) {
    render_entity_biped(view, headYawDeg, bodyYawDeg, 0.0f, frame,
                        &texture_pigzombie, false, true, ITEM_GOLD_SWORD,
                        false, 0.0f, false, NULL);
}

// ghast (batch 1b) -- Beta 1.7.3 ModelGhast: one 16x16x16 body @ UV(0,0) with
// 9 hanging 2xLx2 tentacles @ UV(0,0), lengths FIXED by Random(1660):
// nextInt(7)+8 x9 = {8,13,9,11,11,10,12,9,12} (precomputed -- the exact bytes
// Beta derives every construction). Tentacle sway rotateAngleX =
// 0.2*sin(t*0.3 + i) + 0.4. RenderGhast rest scale (8+1)/2 = 4.5 about the
// feet; the attack squash-stretch needs the un-networked counter phase and is
// a documented cosmetic simplification (the fire-texture swap conveys the
// state). Structure cloned from render_entity_squid: per-cube BASE-RESET, no
// head joint.
void render_entity_ghast(mat4 view, float headYawDeg, float bodyYawDeg,
                         int frame, bool attacking) {
    assert(view);
    (void)headYawDeg; // no separate head; the body yaws as one

    static const int tent_len[9] = {8, 13, 9, 11, 11, 10, 12, 9, 12};

    mat4 model, body_mv;
    glm_mat4_identity(model);
    glm_translate_make(model, (vec3){ -0.5f, 0.0f, -0.5f });
    vec3 pivot = { 0.5f, 0.0f, 0.5f };
    glm_translate(model, pivot);
    glm_rotate_y(model, glm_rad(bodyYawDeg + 180.0f), model);
    glm_translate(model, (vec3){ -pivot[0], -pivot[1], -pivot[2] });
    // RenderGhast rest scale about the feet (idle var4=1 -> (8+1)/2 = 4.5).
    glm_translate(model, pivot);
    glm_scale(model, (vec3){4.5f, 4.5f, 4.5f});
    glm_translate(model, (vec3){ -pivot[0], -pivot[1], -pivot[2] });
    glm_mat4_mul(view, model, body_mv);
    gfx_matrix_modelview(body_mv);

    // Fallback chain: fire variant -> base ghast -> creeper.
    struct tex_gfx* base
        = texture_ghast.data ? &texture_ghast : &texture_creeper;
    gfx_bind_texture(attacking && texture_ghast_fire.data ? &texture_ghast_fire
                                                          : base);

    // Body 16x16x16 @ (0,0): Beta addBox(-8,-8,-8) rp_y 24-16=8 -> down-y
    // 0..16 -> v2 x/z 0..16, y 8..24 (the squid's 24-map).
    static const UVRect bodyUV[6] = {
        {32, 0, 16, 16}, {16, 0, 16, 16},
        { 0, 16, 16, 16}, {16, 16, 16, 16},
        {32, 16, 16, 16}, {48, 16, 16, 16}
    };
    static const int ghastDir[6] = {0, 0, -90, -90, -90, -90};
    displaylist_reset(&dl);
    render_entity_create_cube(0, 8, 0, 16, 16, 16, bodyUV, ghastDir);
    displaylist_render_immediate(&dl, 24);

    // Nine tentacles on the ModelGhast 3x3 grid: x = ((i%3 - (i/3%2)*0.5 +
    // 0.25)/2*2 - 1)*5, z = (i/3 - 1)*5 (rp offsets), joints at rp_y 31-16=15
    // -> v2 y 24-15 = 9, boxes hanging down. Sway 0.2*sin(t*0.3+i)+0.4.
    for(int i = 0; i < 9; i++) {
        float bx = (((float)(i % 3) - (float)(i / 3 % 2) * 0.5f + 0.25f)
                        / 2.0f * 2.0f
                    - 1.0f)
            * 5.0f;
        float bz = ((float)(i / 3) / 2.0f * 2.0f - 1.0f) * 5.0f;
        int len = tent_len[i];
        float sway = 0.2f * sinf((float)frame * 0.3f + (float)i) + 0.4f;
        vec3 joint = { (bx + 8.0f) / 16.0f, 9.0f / 16.0f,
                       (bz + 8.0f) / 16.0f };
        mat4 tent_model, tent_mv;
        glm_mat4_identity(tent_model);
        glm_translate_make(tent_model, joint);
        // Beta rotateAngleX in down-y model space -> negated in v2 up-y space
        // (the squid tentacle sign convention).
        glm_rotate_x(tent_model, -sway, tent_model);
        glm_mat4_mul(body_mv, tent_model, tent_mv);
        gfx_matrix_modelview(tent_mv);
        UVRect tentUV[6] = {
            {4, 0, 2, 2}, {2, 0, 2, 2},
            {0, 2, 2, (uint8_t)len}, {2, 2, 2, (uint8_t)len},
            {4, 2, 2, (uint8_t)len}, {6, 2, 2, (uint8_t)len}
        };
        displaylist_reset(&dl); // BASE-RESET rule, per tentacle
        render_entity_create_cube(-1, -len, -1, 2, len, 2, tentUV, ghastDir);
        displaylist_render_immediate(&dl, 24);
    }

    gfx_lighting(true);
    gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}

// spider -- Beta 1.7.3 ModelSpider (ModelSpider.java): head 8x8x8, neck 6x6x6,
// body 10x8x12, 8 legs 16x2x2 that fan down (rotateAngleZ) + splay fwd/back
// (rotateAngleY) + walk-swing. Beta's Y-down model maps to v2's feet-origin Y-up
// exactly as the zombie maps ModelBiped, with the spider's rotPointY=15.
void render_entity_spider(mat4 view, float headYawDeg, float bodyYawDeg, int frame) {
    assert(view);

    // Beta walk driver t = limbSwing*0.6662; v2 has no per-tick limbSwing, so
    // drive it from `frame` over the same 60-frame cycle the zombie/creeper use,
    // with a constant moderate gait (limbSwingAmount=1.0, animation-only).
    const float t   = (float)(frame % 60) / 60.0f * 2.0f * (float)M_PI;
    const float amt = limb_swing_amount; // 0 when stopped -> legs settle (Beta)

    // Body matrix: yaw about the MODEL ORIGIN AXIS x=z=8px (M2 head fix --
    // the old z=17 abdomen-centre pivot put the spider's head 6px off at the
    // worst facings; Beta rotates the whole model about its origin).
    mat4 body_mv;
    vec3 bodyPivot = {
        8.0f / 16.0f,
        (5.0f + 8.0f / 2.0f) / 16.0f,
        8.0f / 16.0f
    };
    render_entity_body_mv(view, bodyPivot, bodyYawDeg, body_mv);

    // Beta spider.png; fall back to creeper if a pack omits it (mirrors zombie).
    gfx_bind_texture(texture_spider.data ? &texture_spider : &texture_creeper);

    // --- Head (8x8x8): rides body_mv with its yaw DELTA about the Beta neck
    // rp(0,15,-3) -> v2 (8,9,5) (M2 head fix; the single recenter + the +180
    // both live in body_mv now). ---
    displaylist_reset(&dl);
    {
        vec3 neck = { 8.0f / 16.0f, 9.0f / 16.0f, 5.0f / 16.0f };
        mat4 head_mv;
        render_entity_head_mv(body_mv, neck, headYawDeg, bodyYawDeg, 0.0f, head_mv);
        gfx_matrix_modelview(head_mv);
    }
    static const UVRect spiderHeadUV[6] = {
        {48, 4, 8, 8}, {40, 4, 8, 8},
        {32,12, 8, 8}, {40,12, 8, 8},
        {48,12, 8, 8}, {56,12, 8, 8}
    };
    static const int spiderHeadDir[6] = {-90,-90,-90,-90,-90,-90};
    // CORRECTED z0: Beta (-4,-4,-8)+(8,8,8) @rp(0,15,-3) -> v2 z0 = -3+(-8)+8 = -3
    render_entity_create_cube(4, 5, -3, 8, 8, 8, spiderHeadUV, spiderHeadDir);
    displaylist_render_immediate(&dl, 24);

    // --- Glowing eyes (Beta 1.7.3 RenderSpider emissive eye overlay) ------------
    // Beta draws the eyes as a 2nd pass over the head using a SEPARATE overlay texture
    // mob/spider_eyes.png that is TRANSPARENT everywhere except the glowing eye pixels,
    // ignoring world light so the eyes glow in the dark while the body stays world-lit.
    // We do exactly that: bind texture_spider_eyes and draw the head FRONT (-Z) face
    // from it at FULL BRIGHT with ALPHA-TEST on, so only the opaque eye texels survive
    // and the rest of the face is cut away.
    //
    // (Earlier this redrew an opaque 8x5 sub-rect of the HEAD texture itself -- which
    // lit up the whole upper FACE, not just the eyes, because that region of spider.png
    // is solid skin with no transparency. The overlay's alpha is what isolates the
    // eyes; there is no way to do it from the opaque head texture alone.)
    //
    // FULL-BRIGHT MECHANISM (FIFO-safe): the brightness of THIS quad comes ENTIRELY
    // from the per-vertex CLR0 light INDEX 0xFF written by displaylist_color(0xFF) on
    // each of the 4 verts -- that is the byte the immediate renderer actually consumes
    // (Wii: GX_Color1x8(MEM_U8(base,6)); PC: vertex attribute 3). It does NOT read the
    // file-static vertex_light[] global, so we deliberately do NOT call
    // render_entity_update_light here at all: the global stays at the caller's world
    // light, the neck/body/legs that follow (render_entity_create_cube reads
    // vertex_light[0]) stay world-lit automatically, and there is NOTHING to save or
    // restore. Not dirtying the global is the strongest possible guarantee that no
    // full-bright state can leak to the rest of this spider or to the next entity --
    // it removes the only coupling point rather than relying on a paired restore. The
    // pass reuses the existing CLR0 INDEX8 light path + the standard alpha-test cutout
    // (no new TEV stage / vertex format / blend) so it cannot cause the #17f CLR0-desync
    // FIFO hang.
    //
    // The eye quad rides the head modelview (still the active state here -- we do NOT
    // call gfx_matrix_modelview in between) so the eyes track the head yaw. Guarded on
    // texture_spider_eyes.data: a pack without the overlay simply gets NO glow --
    // strictly better than the old face-wide glow (the body still draws; it fell back
    // to texture_creeper at the bind above if even spider.png is absent).
    //
    // SIMPLIFICATION vs Beta: Beta modulates eye alpha by ambient brightness
    // ((1-getEntityBrightness)*0.5) so the eyes fade in as it darkens; we draw them
    // always full-bright. The visible Beta behavior (eyes glow in the dark, blend into
    // the lit head texture in daylight) is preserved; only the day/night alpha ramp is
    // approximated. Documented per CLAUDE.md as a deliberate, justified simplification.
    if(texture_spider_eyes.data) {
        // The eye overlay shares spider.png's 64x32 UV layout, so the head FRONT (-Z)
        // face is the same window {40,12,8,8}. Draw that WHOLE face from the overlay;
        // the texture's transparency isolates the eye pixels (no manual row trim -- the
        // alpha does it, correctly, for every eye texel including the lower rows).
        static const UVRect spiderEyeUV = {40, 12, 8, 8};

        // The eye quad is coplanar with the head's -Z front face (both at box z0=-3).
        // To avoid z-fighting, nudge it a hair PROUD of the face toward the camera
        // (more negative z, the outward -Z normal direction). The cube builder works
        // in fixed-point box units (1 px = 16 units); 4 units ~= 0.25 px ~= 0.016
        // block -- far below one texel, invisible, but enough to win the depth test.
        const int EYE_PROUD = 4; // fixed-point units (16 = 1 Beta px)

        // Head front-face box extents in fixed-point (mirror render_entity_create_cube
        // for the head box render_entity_create_cube(4,5,-3, 8,8,8, ...)):
        //   x in [4*16 .. 12*16], y in [5*16 .. 13*16], front plane z = -3*16.
        // The eye quad now covers the FULL front face (u=40,v=12,w=8,h=8): the whole
        // face width (x0..x1) and all 8 rows. The south face puts texture-top (v=12)
        // at box y1 (top) and texture-bottom (v=20) at y0, so v=[12..20] -> y=[5..13].
        const int ex0 = 4  * 16;   // left   (x0)
        const int ex1 = 12 * 16;   // right  (x1)
        const int ey0 = 5  * 16;   // bottom (y for v=20)
        const int ey1 = 13 * 16;   // top    (y for v=12)
        const int ez  = -3 * 16 - EYE_PROUD; // front plane, nudged outward (-Z)

        // Eye texcoords: same sw=4 (256/64) / sh=8 (256/32) scale the cube builder
        // uses, fed through render_entity_fill_rotated_UVs with the SAME -90 rotation
        // as the head front face (spiderHeadDir[3]) so the eye texels orient exactly
        // like the underlying head.
        const int sw = 256 / 64, sh = 256 / 32;
        uint8_t eu0 = spiderEyeUV.u * sw;
        uint8_t ev0 = spiderEyeUV.v * sh;
        uint8_t eu1 = (uint8_t)(spiderEyeUV.u * sw + spiderEyeUV.w * sw);
        uint8_t ev1 = (uint8_t)(spiderEyeUV.v * sh + spiderEyeUV.h * sh);
        uint8_t euv[8];
        render_entity_fill_rotated_UVs(eu0, ev0, eu1, ev1, -90, euv);

        // One quad on the front (-Z) face, winding copied EXACTLY from the south-face
        // block of render_entity_create_cube (so it faces -Z outward and is not
        // back-face culled): v0=(x1,y0), v1=(x1,y1), v2=(x0,y1), v3=(x0,y0) at z=ez.
        // The per-vertex displaylist_color(EYE_FULL_BRIGHT) on all 4 verts is the SOLE
        // source of the eyes' full brightness (see the FULL-BRIGHT MECHANISM note
        // above): the immediate renderer reads this byte directly, so the global
        // vertex_light[] is untouched and the world-lit parts that follow need no
        // restore.
        const uint8_t EYE_FULL_BRIGHT = 0xFF; // CLR0 INDEX8 = (torch15<<4)|sky15

        // Bind the emissive overlay + assert the alpha-test cutout so the overlay's
        // transparent (non-eye) texels are discarded -- only the eye pixels draw.
        gfx_bind_texture(&texture_spider_eyes);
        gfx_alpha_test(true);

        displaylist_reset(&dl);
        displaylist_pos(&dl, ex1, ey0, ez);
        displaylist_color(&dl, EYE_FULL_BRIGHT);
        displaylist_texcoord(&dl, euv[0], euv[1]);
        displaylist_pos(&dl, ex1, ey1, ez);
        displaylist_color(&dl, EYE_FULL_BRIGHT);
        displaylist_texcoord(&dl, euv[2], euv[3]);
        displaylist_pos(&dl, ex0, ey1, ez);
        displaylist_color(&dl, EYE_FULL_BRIGHT);
        displaylist_texcoord(&dl, euv[4], euv[5]);
        displaylist_pos(&dl, ex0, ey0, ez);
        displaylist_color(&dl, EYE_FULL_BRIGHT);
        displaylist_texcoord(&dl, euv[6], euv[7]);
        displaylist_render_immediate(&dl, 4);

        // Rebind the body skin for the neck/body/legs that follow. Alpha-test stays
        // on (the entity pass inherits it on; the opaque body is unaffected -- all its
        // texels are alpha=255 -- and the next transparent pass sets its own state).
        // This pass never wrote vertex_light[], so the world-lit parts need no restore.
        gfx_bind_texture(texture_spider.data ? &texture_spider : &texture_creeper);
    }

    // --- Neck/thorax (6x6x6) @rp(0,15,0) ---
    displaylist_reset(&dl);
    gfx_matrix_modelview(body_mv);
    static const UVRect spiderNeckUV[6] = {
        {12, 0, 6, 6}, { 6, 0, 6, 6},
        { 0, 6, 6, 6}, { 6, 6, 6, 6},
        {12, 6, 6, 6}, {18, 6, 6, 6}
    };
    static const int spiderNeckDir[6] = {-90,-90,-90,-90,-90,-90};
    // Beta (-3,-3,-3)+(6,6,6) @rp(0,15,0): x0=5, y0=24-(15-3+6)=6, z0=0+(-3)+8=5
    render_entity_create_cube(5, 6, 5, 6, 6, 6, spiderNeckUV, spiderNeckDir);
    displaylist_render_immediate(&dl, 24);

    // --- Abdomen/body (10x8x12) @rp(0,15,9) ---
    displaylist_reset(&dl);
    gfx_matrix_modelview(body_mv);
    // UV faces must match the abdomen's 10(W)x8(H)x12(D) extents: north/south(+/-Z)=10x8,
    // west/east(+/-X)=12x8, top/bottom=10x12. The old north {0,24,12,8} squashed 12 texels
    // onto the 10px +Z face and east {34,24,10,8} stretched 10 over the 12px-deep +X face;
    // corrected to the canonical Beta ModelSpider net (tex offset 0,12) -- only north, west,
    // east change; south/top/bottom were already size-correct (mob-model audit, 2026-06-05).
    static const UVRect spiderBodyUV[6] = {
        {22,12,10,12}, {12,12,10,12},
        {34,24,10, 8}, {12,24,10, 8},
        { 0,24,12, 8}, {22,24,12, 8}
    };
    static const int spiderBodyDir[6] = {0,0,-90,-90,-90,-90};
    // Beta (-5,-4,-6)+(10,8,12) @rp(0,15,9): x0=3, y0=24-(15-4+8)=5, z0=9+(-6)+8=11
    render_entity_create_cube(3, 5, 11, 10, 8, 12, spiderBodyUV, spiderBodyDir);
    displaylist_render_immediate(&dl, 24);

    // --- 8 legs (each 16x2x2), Beta fixed fan + walk swing ---
    const float var7 = (float)M_PI * 0.25f;   // 45 deg base fan
    const float var9 = (float)M_PI * 0.125f;  // 22.5 deg base splay

    const float legZbase[8] = {
        -var7,        +var7,
        -var7*0.74f,  +var7*0.74f,
        -var7*0.74f,  +var7*0.74f,
        -var7,        +var7
    };
    const float legYbase[8] = {
        +var9*2.0f,  -var9*2.0f,
        +var9*1.0f,  -var9*1.0f,
        -var9*1.0f,  +var9*1.0f,
        -var9*2.0f,  +var9*2.0f
    };
    const float swYpair[4] = {
        -(cosf(t*2.0f + 0.0f)             * 0.4f) * amt, // Y pair 0 -> legs 1,2
        -(cosf(t*2.0f + (float)M_PI)      * 0.4f) * amt, // Y pair 1 -> legs 3,4
        -(cosf(t*2.0f + (float)M_PI*0.5f) * 0.4f) * amt, // Y pair 2 -> legs 5,6
        -(cosf(t*2.0f + (float)M_PI*1.5f) * 0.4f) * amt  // Y pair 3 -> legs 7,8
    };
    const float swZpair[4] = {
        fabsf(sinf(t + 0.0f)             * 0.4f) * amt, // Z pair 0 -> legs 1,2
        fabsf(sinf(t + (float)M_PI)      * 0.4f) * amt, // Z pair 1 -> legs 3,4
        fabsf(sinf(t + (float)M_PI*0.5f) * 0.4f) * amt, // Z pair 2 -> legs 5,6
        fabsf(sinf(t + (float)M_PI*1.5f) * 0.4f) * amt  // Z pair 3 -> legs 7,8
    };
    // Leg is 16(W)x2(H)x2(D): north/south(+/-Z) long sides = 16x2, west/east(+/-X) end caps
    // = 2x2. The old north {18,2,2,2} put a 2x2 cap-window on the 16px side and east
    // {38,2,16,2} put a 16-wide strip on the 2px cap; corrected so sizes match the faces.
    static const UVRect spiderLegUV[6] = {
        {36, 0,16, 2}, {20, 0,16, 2},
        {38, 2,16, 2}, {20, 2,16, 2},
        {18, 2, 2, 2}, {36, 2, 2, 2}
    };
    static const int spiderLegDir[6] = {0,0,-90,-90,-90,-90};
    // joint Y=9 (rotPointY 15 -> v2 y-centre 9), joint X = rotPointX+8
    // (left=-4->4, right=+4->12), joint Z = rotPointZ+8 (pair {2,2,1,1,0,0,-1,-1}).
    const int legJointZ[8] = {10,10, 9,9, 8,8, 7,7};
    for(int i = 0; i < 8; i++) {
        // Rewind the buffer write-index to base for THIS leg. displaylist_render_immediate
        // (at the end of the loop) ALWAYS reads its 24 verts from slot 0 and never advances
        // l->index, so without this reset every leg's create_cube (which appends at the
        // running index 24,48,...) would be ignored and render_immediate would redraw the
        // ABDOMEN's stale verts 0..23 under the leg matrix -- all 8 "legs" become abdomen
        // copies (the cause of "spider body looks off"). The creeper/pig/biped leg loops all
        // reset per-iteration the same way (mob-model audit, 2026-06-05).
        displaylist_reset(&dl);
        bool  left   = (i % 2 == 0);  // legs 1,3,5,7 (0-based even)
        int   pair   = i / 2;
        float swSign = left ? +1.0f : -1.0f;
        float rotZ = legZbase[i] + swSign * swZpair[pair];
        float rotY = legYbase[i] + swSign * swYpair[pair];

        float jointX = left ? 4.0f : 12.0f;
        float jointY = 9.0f;
        float jointZ = (float)legJointZ[i];

        // Beta ModelSpider faithful, mirroring the proven pig pattern: translate the matrix
        // TO the joint, rotate ABOUT the joint (Beta order rotateZ then rotateY), and draw the
        // box JOINT-RELATIVE (Beta addBox). EXACTLY ONE recenter total -- supplied by body_mv.
        // The earlier code had TWO bugs that each broke attachment (spider-leg-attach audit
        // 2026-06-05): (a) an extra glm_translate_make(-0.5,0,-0.5) DOUBLE-recentered the legs
        // (body_mv already carries the single Y-down->Y-up recenter), pulling them -8px x/-8px z;
        // (b) the T(+joint)*R*T(-joint) "pivot sandwich" has net-zero translation, so the cube
        // (drawn at ABSOLUTE -16..0 / 0..16, near the model origin) was never moved to the joint.
        // Both are gone now: translate_make to the joint is the matrix base, no inverse-joint
        // translate, and the cube uses Beta's joint-relative addBox. Traced inner ends land flush
        // on the thorax x-faces (x=+/-3.5) and outer ends splay out+down to the ground.
        mat4 leg_model;
        glm_mat4_identity(leg_model);
        glm_translate_make(leg_model, (vec3){ jointX/16.0f, jointY/16.0f, jointZ/16.0f });
        glm_rotate_z(leg_model, -rotZ, leg_model); // Z fan: negated for the Y-down->Y-up flip
        glm_rotate_y(leg_model,  rotY, leg_model); // Y splay: sign KEPT (z mapping has no flip)
        mat4 leg_mv;
        glm_mat4_mul(body_mv, leg_model, leg_mv);  // body_mv = the ONE recenter
        gfx_matrix_modelview(leg_mv);
        // Beta addBox drawn joint-relative: LEFT spiderLeg1 addBox(-15,-1,-1,16,2,2) (inner end
        // x=+1 tucks INTO the thorax), RIGHT spiderLeg2 addBox(-1,-1,-1,16,2,2) (mirror).
        if(left)
            render_entity_create_cube(-15, -1, -1, 16, 2, 2, spiderLegUV, spiderLegDir);
        else
            render_entity_create_cube( -1, -1, -1, 16, 2, 2, spiderLegUV, spiderLegDir);
        displaylist_render_immediate(&dl, 24);
    }

    gfx_lighting(true);
    gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}

// Beta 1.7.3 ModelPig (ModelQuadruped + snout): a horizontal body (the Beta 10x16x8 box
// rendered with rotateAngleX=90 -> 10 wide x 8 tall x 16 long), an 8^3 head with a 4x3x1
// snout at the front, and four 4x6x4 legs that walk-swing. Built on the creeper's proven
// (FIFO-safe) matrix/displaylist structure: body yaw + the +180 model-front offset, a
// head submatrix for headYaw, and per-leg X-rotation walk animation. Standard signature so
// the entity_render dispatch can call it like every other mob.
void render_entity_pig(mat4 view, float headYawDeg, float bodyYawDeg, int frame) {
    assert(view);

    float walkAngle = sinf((float)(frame % 60) / 60.0f * 2.0f * M_PI)
                      * (30.0f * (M_PI / 180.0f)) * limb_swing_amount;

    // Body matrix: yaw about the MODEL ORIGIN AXIS x=z=8px (M2 head fix --
    // Beta rotates the whole model about its origin, never a box centre; the
    // old z=10 box-centre pivot was half the pig's 12px head drift).
    mat4 body_mv;
    vec3 bodyPivot = { 8.0f / 16.0f, (6.0f + 8.0f / 2.0f) / 16.0f,
                       8.0f / 16.0f };
    render_entity_body_mv(view, bodyPivot, bodyYawDeg, body_mv);

    // pig.png missing (legal-asset pack not installed) -> fall back to the creeper skin
    // (mis-skinned but visible) rather than an unbound-texture draw.
    gfx_bind_texture(texture_pig.data ? &texture_pig : &texture_creeper);

    // Head (8x8x8) at the FRONT (protruding -Z), riding body_mv with only its
    // yaw DELTA about the Beta neck rp(0,12,-6) -> v2 (8,12,2) (M2 head fix).
    displaylist_reset(&dl);
    {
        vec3 neck = { 8.0f / 16.0f, 12.0f / 16.0f, 2.0f / 16.0f };
        mat4 head_mv;
        render_entity_head_mv(body_mv, neck, headYawDeg, bodyYawDeg, 0.0f, head_mv);
        gfx_matrix_modelview(head_mv);
    }
    static const UVRect headUV[6] = {
        {16, 0, 8, 8}, { 8, 0, 8, 8},
        { 0, 8, 8, 8}, { 8, 8, 8, 8},
        {16, 8, 8, 8}, {24, 8, 8, 8}
    };
    static const int headDir[6] = {0, 0, -90, -90, -90, -90};
    // Beta ModelQuadruped head addBox(-4,-4,-8)@rp(0,12,-6) on the 24-map:
    // y 8..16 (the old y0=6 was the 22-map's 2px sink), z -6..2.
    render_entity_create_cube(4, 8, -6, 8, 8, 8, headUV, headDir);
    displaylist_render_immediate(&dl, 24);
    // NO SNOUT: b1.7.3 ModelPig is a bare ModelQuadruped(6) -- the protruding
    // snout box is 1.8+/release-era contamination (decompile-verified
    // 2026-06-11; the face is PAINTED on the head front texture). Removed.

    // Body (10 wide x 8 tall x 16 long, horizontal). PI/2-bake map (the closed
    // form, chicken-proven): x0=rp_x+bx0+8, y0=(24-rp_y)+bz0, z0=rp_z+by0+8,
    // dims (w,d,h). Beta addBox(-5,-10,-7,10,16,8)@rp(0,11,2): (3,6,0). The
    // old z0=2 was a fudge constant -- the body sat 2px behind the map, hidden
    // by the pig's 8-deep head sinking into the chest (24-map body re-bake).
    displaylist_reset(&dl);
    gfx_matrix_modelview(body_mv);
    // Rebuilt on the COW's hardware-proven row order + dirs (hit on console
    // 2026-08-04: the right body side sampled {18,16}, the EMPTY gutter
    // between pig.png's leg and body blocks -> transparent face). Beta
    // ModelQuadruped body texOffs(28,8) addBox dims (10,16,8) on the standard
    // 64x32 pig.png: front(36,16) back(54,16) bottom(46,8) top(36,8)
    // right(28,16) left(46,16); PI/2 bake puts front/back on the horizontal
    // faces, bottom/top on the z-ends, right/left on the x-sides -- exactly
    // the cow's proven slotting.
    static const UVRect bodyUV[6] = {
        {36, 16, 10, 16}, {54, 16, 10, 16},
        {46,  8, 10,  8}, {36,  8, 10,  8},
        {28, 16,  8, 16}, {46, 16,  8, 16}
    };
    static const int bodyDir[6] = {0, 0, 0, 0, 90, -90};
    render_entity_create_cube(3, 6, 0, 10, 8, 16, bodyUV, bodyDir);
    displaylist_render_immediate(&dl, 24);

    // Four legs (4x6x4), walk-swinging about the hip (y = 6).
    static const UVRect legUV[6] = {
        { 8, 16, 4, 4}, {4, 16, 4, 4},
        {12, 20, 4, 6}, {4, 20, 4, 6},
        { 8, 20, 4, 6}, {0, 20, 4, 6}
    };
    static const int legDir[6] = {0, 0, -90, -90, -90, -90};
    const int lw = 4, lh = 6, ld = 4;
    // legPos = (rp_x+8, 0, rp_z+8) - boxHalf(2). ModelPig is a bare
    // ModelQuadruped: leg rps +/-3 x, -5/7 z -> front z 1 (the old 3 was the
    // same +2 fudge as the body; Beta's back legs poke 1px past the rump).
    int legPos[4][3] = {
        {3, 0, 1},  // front-left
        {9, 0, 1},  // front-right
        {3, 0, 13}, // back-left
        {9, 0, 13}  // back-right
    };
    int legAnimDir[4] = {+1, -1, -1, +1};
    for(int i = 0; i < 4; i++) {
        displaylist_reset(&dl);
        vec3 pivot = { (lw / 2.0f) / 16.0f, lh / 16.0f, (ld / 2.0f) / 16.0f };
        mat4 leg_model;
        glm_mat4_identity(leg_model);
        glm_translate_make(leg_model, (vec3){ legPos[i][0] / 16.0f,
                                              legPos[i][1] / 16.0f,
                                              legPos[i][2] / 16.0f });
        glm_translate(leg_model, pivot);
        glm_rotate_x(leg_model, legAnimDir[i] * walkAngle, leg_model);
        glm_translate(leg_model, (vec3){ -pivot[0], -pivot[1], -pivot[2] });
        mat4 leg_mv;
        glm_mat4_mul(body_mv, leg_model, leg_mv);
        gfx_matrix_modelview(leg_mv);
        render_entity_create_cube(0, 0, 0, lw, lh, ld, legUV, legDir);
        displaylist_render_immediate(&dl, 24);
    }

    gfx_lighting(true);
    gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}

/* Beta 1.7.3 ModelWolf. The boxes, rotation points and sitting rotations are
 * transcribed from the decompile; the 24-pixel Y-up map is the same conversion
 * used by the quadruped renderers above. [740c583: ModelWolf, RenderWolf] */
static void wolf_box_uv(UVRect uv[6], int u, int v, int w, int h, int d) {
	uv[0] = (UVRect) {u + d, v, w, d};
	uv[1] = (UVRect) {u + d + w, v, w, d};
	uv[2] = (UVRect) {u + d, v + d, w, h};
	uv[3] = (UVRect) {u + d + w + d, v + d, w, h};
	uv[4] = (UVRect) {u, v + d, d, h};
	uv[5] = (UVRect) {u + d + w, v + d, d, h};
}

static void wolf_draw_box(mat4 root, float px, float py, float pz,
						  float bx, float by, float bz, int w, int h, int d,
						  float rx, float ry, float rz, int u, int v) {
	mat4 local, mv;
	glm_mat4_identity(local);
	glm_translate_make(local, (vec3) {(px + 8.0F) / 16.0F,
		(24.0F - py) / 16.0F, (pz + 8.0F) / 16.0F});
	/* Java model space is Y-down, so X/Z rotations reverse under the map. */
	glm_rotate_z(local, -rz, local);
	glm_rotate_y(local, ry, local);
	glm_rotate_x(local, -rx, local);
	glm_mat4_mul(root, local, mv);
	gfx_matrix_modelview(mv);
	UVRect uv[6];
	static const int dirs[6] = {0, 0, -90, -90, -90, -90};
	wolf_box_uv(uv, u, v, w, h, d);
	displaylist_reset(&dl);
	render_entity_create_cube((int)bx, (int)(-by - h), (int)bz,
		w, h, d, uv, dirs);
	displaylist_render_immediate(&dl, 24);
}

void render_entity_wolf(mat4 view, float headYawDeg, float bodyYawDeg, int frame,
						uint8_t flags, int watched_health, bool shaking,
						float previous_shake, float current_shake,
						float previous_interest, float current_interest,
						float partial) {
	assert(view);
	mat4 root;
	render_entity_body_mv(view, (vec3) {0.5F, 0.75F, 0.5F}, bodyYawDeg, root);
	struct tex_gfx* skin = wolf_flag_get(flags, WOLF_FLAG_TAMED)
		? &texture_wolf_tame
		: (wolf_flag_get(flags, WOLF_FLAG_ANGRY)
			? &texture_wolf_angry : &texture_wolf);
	gfx_bind_texture(skin->data ? skin : &texture_creeper);

	if(shaking) {
		float shade = wolf_shading_while_shaking(previous_shake,
			current_shake, partial);
		gfx_entity_modulate(shade, shade, shade);
	}
	bool sitting = wolf_flag_get(flags, WOLF_FLAG_SITTING);
	bool angry = wolf_flag_get(flags, WOLF_FLAG_ANGRY);
	float walk = (float)frame * 0.6662F;
	float stride = 1.4F * limb_swing_amount;
	float head_z = wolf_interested_angle(previous_interest, current_interest,
		partial) + wolf_shake_angle(previous_shake, current_shake, partial, 0.0F);
	float mane_z = wolf_shake_angle(previous_shake, current_shake, partial, -0.08F);
	float body_z = wolf_shake_angle(previous_shake, current_shake, partial, -0.16F);
	float tail_z = wolf_shake_angle(previous_shake, current_shake, partial, -0.20F);
	float head_y = glm_rad(headYawDeg - bodyYawDeg);

	/* Head, ears and snout share the interested/shake roll. */
	wolf_draw_box(root, -1, 13.5F, -7, -3, -3, -2, 6, 6, 4,
		0, head_y, head_z, 0, 0);
	wolf_draw_box(root, -1, 13.5F, -7, -3, -5, 0, 2, 2, 1,
		0, head_y, head_z, 16, 14);
	wolf_draw_box(root, -1, 13.5F, -7, 1, -5, 0, 2, 2, 1,
		0, head_y, head_z, 16, 14);
	wolf_draw_box(root, -0.5F, 13.5F, -7, -2, 0, -5, 3, 3, 4,
		0, head_y, head_z, 0, 10);

	float body_x = sitting ? 0.7853982F : 1.5707964F;
	float body_py = sitting ? 18.0F : 14.0F;
	float body_pz = sitting ? 0.0F : 2.0F;
	wolf_draw_box(root, 0, body_py, body_pz, -4, -2, -3, 6, 9, 6,
		body_x, 0, body_z, 18, 14);
	wolf_draw_box(root, -1, sitting ? 16.0F : 14.0F, -3,
		-4, -3, -3, 8, 6, 7, sitting ? 1.2566371F : 1.5707964F,
		0, mane_z, 21, 0);

	float leg_rx[4], leg_px[4] = {-2.5F, 0.5F, -2.5F, 0.5F};
	float leg_py[4], leg_pz[4];
	if(sitting) {
		leg_py[0] = leg_py[1] = 22.0F;
		leg_pz[0] = leg_pz[1] = 2.0F;
		leg_rx[0] = leg_rx[1] = 4.712389F;
		leg_py[2] = leg_py[3] = 17.0F;
		leg_pz[2] = leg_pz[3] = -4.0F;
		leg_rx[2] = leg_rx[3] = 5.811947F;
	} else {
		leg_py[0] = leg_py[1] = leg_py[2] = leg_py[3] = 16.0F;
		leg_pz[0] = leg_pz[1] = 7.0F;
		leg_pz[2] = leg_pz[3] = -4.0F;
		leg_rx[0] = leg_rx[3] = cosf(walk) * stride;
		leg_rx[1] = leg_rx[2] = cosf(walk + 3.1415927F) * stride;
	}
	for(int i = 0; i < 4; i++)
		wolf_draw_box(root, leg_px[i], leg_py[i], leg_pz[i],
			-1, 0, -1, 2, 8, 2, leg_rx[i], 0, 0, 0, 18);

	float tail_y = angry ? 0.0F : cosf(walk) * stride;
	wolf_draw_box(root, -1, sitting ? 21.0F : 12.0F,
		sitting ? 6.0F : 8.0F, -1, 0, -1, 2, 8, 2,
		wolf_tail_angle(flags, watched_health), tail_y, tail_z, 9, 18);
	if(shaking)
		gfx_entity_modulate(1.0F, 1.0F, 1.0F);
	gfx_lighting(true);
	gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}

// Beta 1.7.3 ModelCow (ModelQuadruped + horns): an 8x8x6 head with two 1x3x1 horns, a horizontal
// body (Beta 12x18x10 + rotateAngleX=PI/2 -> baked to 12 wide x 10 tall x 18 long), and four 4x12x4
// legs that walk-swing. Built on the pig's proven FIFO-safe structure: per-cube displaylist_reset,
// body yaw + the +180 model-front offset, a head submatrix for headYaw, and per-leg X-rotation walk.
// cow.png is absent from the legal pack -> creeper fallback; UVs are 64x32-extent-correct for when a
// real cow.png is overlaid. (cow/chicken render design, verified 2026-06-05.)
void render_entity_cow(mat4 view, float headYawDeg, float bodyYawDeg, int frame) {
    assert(view);

    float walkAngle = sinf((float)(frame % 60) / 60.0f * 2.0f * M_PI)
                      * (30.0f * (M_PI / 180.0f)) * limb_swing_amount;

    // Body matrix: yaw about the MODEL ORIGIN AXIS x=z=8px (M2 head fix --
    // the old z=11 box-centre pivot was most of the cow's 16px head drift).
    mat4 body_mv;
    vec3 bodyPivot = {
        8.0f / 16.0f,
        (12.0f + 10.0f / 2.0f) / 16.0f,
        8.0f / 16.0f
    };
    render_entity_body_mv(view, bodyPivot, bodyYawDeg, body_mv);

    gfx_bind_texture(texture_cow.data ? &texture_cow : &texture_creeper);

    // --- Head (8x8x6): rides body_mv with its yaw DELTA about the Beta neck
    // rp(0,4,-8) -> v2 (8,20,0) (M2 head fix). The 2026-06-05 "22-map" this
    // head was calibrated on was itself the 2px sink -- the chicken proved
    // the 24-map (v2_y = 24 - betaYdown) is this codebase's true constant,
    // and the decompile re-verification confirmed every quadruped rp. ---
    displaylist_reset(&dl);
    {
        vec3 neck = { 8.0f / 16.0f, 20.0f / 16.0f, 0.0f / 16.0f };
        mat4 head_mv;
        render_entity_head_mv(body_mv, neck, headYawDeg, bodyYawDeg, 0.0f, head_mv);
        gfx_matrix_modelview(head_mv);
    }
    static const UVRect cowHeadUV[6] = {
        {14, 0, 8, 6}, { 6, 0, 8, 6},
        {20, 6, 8, 8}, { 6, 6, 8, 8},
        { 0, 6, 6, 8}, {14, 6, 6, 8}
    };
    static const int cowHeadDir[6] = {0, 0, -90, -90, -90, -90};
    // Beta addBox(-4,-4,-6)@rp(0,4,-8) on the 24-map: y 16..24 (head top 2px
    // proud of the back line, exactly Beta), z -6..0.
    render_entity_create_cube(4, 16, -6, 8, 8, 6, cowHeadUV, cowHeadDir);
    displaylist_render_immediate(&dl, 24);

    // --- Two horns (1x3x1), ride the head matrix (head_mv stays active; reset only). ---
    static const UVRect cowHornUV[6] = {
        {24, 0, 1, 1}, {23, 0, 1, 1},
        {25, 1, 1, 3}, {23, 1, 1, 3},
        {22, 1, 1, 3}, {24, 1, 1, 3}
    };
    static const int cowHornDir[6] = {0, 0, -90, -90, -90, -90};
    // Beta horn1/horn2 addBox y(-5..-2)@rp(0,3,-7) on the 24-map: y 23..26,
    // raised with the head (+2 vs the 22-map bake); x 4..5 / 11..12, z -3..-2.
    displaylist_reset(&dl);
    render_entity_create_cube(4, 23, -3, 1, 3, 1, cowHornUV, cowHornDir);  // left horn
    displaylist_render_immediate(&dl, 24);
    displaylist_reset(&dl);
    render_entity_create_cube(11, 23, -3, 1, 3, 1, cowHornUV, cowHornDir); // right horn (mirror about x=8)
    displaylist_render_immediate(&dl, 24);

    // --- Body (12 wide x 10 tall x 18 long; Beta PI/2 baked into the swapped dims).
    // PI/2-bake map: x0=rp_x+bx0+8, y0=(24-rp_y)+bz0, z0=rp_z+by0+8, dims (w,d,h).
    // Beta addBox(-6,-10,-7,12,18,10)@rp(0,5,2): (2,12,0) -- front face FLUSH with the
    // head's rear plane (head z -6..0), exactly Beta. The old z0=2 fudge left a 2px
    // see-through neck gap once the head rose to the 24-map (24-map body re-bake). ---
    displaylist_reset(&dl);
    gfx_matrix_modelview(body_mv);
    static const UVRect cowBodyUV[6] = {
        {28, 14, 12, 18}, {50, 14, 12, 18},
        {40,  4, 12, 10}, {28,  4, 12, 10},
        {18, 14, 10, 18}, {40, 14, 10, 18}
    };
    static const int cowBodyDir[6] = {0, 0, 0, 0, 90, -90};
    render_entity_create_cube(2, 12, 0, 12, 10, 18, cowBodyUV, cowBodyDir);
    displaylist_render_immediate(&dl, 24);

    // --- Udders (Beta 4x6x2 + rotateAngleX=PI/2 -> baked 4 wide x 2 tall x 6 long), hang below
    // the belly between the hind legs. Beta addBox(-2,-3,0,4,6,2)@rp(0,14,6) on the PI/2-bake
    // map: (6,10,11) -- the old z0=13 carried the body's +2 fudge (24-map body re-bake). ---
    displaylist_reset(&dl);
    gfx_matrix_modelview(body_mv);
    static const UVRect cowUdderUV[6] = {
        {54, 2, 4, 6}, {60, 2, 4, 6},
        {58, 0, 4, 2}, {54, 0, 4, 2},
        {52, 2, 2, 6}, {58, 2, 2, 6}
    };
    static const int cowUdderDir[6] = {0, 0, 0, 0, 90, -90};
    render_entity_create_cube(6, 10, 11, 4, 2, 6, cowUdderUV, cowUdderDir);
    displaylist_render_immediate(&dl, 24);

    // --- Four legs (4x12x4), walk-swinging about the hip (y=12). Proven pig leg pattern. ---
    static const UVRect cowLegUV[6] = {
        { 8, 16, 4, 4}, { 4, 16, 4, 4},
        {12, 20, 4, 12}, { 4, 20, 4, 12},
        { 0, 20, 4, 12}, { 8, 20, 4, 12}
    };
    static const int cowLegDir[6] = {0, 0, -90, -90, -90, -90};
    const int lw = 4, lh = 12, ld = 4;
    // COW leg positions (Beta cow leg rps -4/+4 x, -6/+7 z -> legPos = rp+8 - boxHalf(2)). The cow
    // is 12 wide (vs the pig's 10) so the hips are wider (x 2/10, not 3/9), and the front legs sit
    // further forward (z 0, not 3). These were the PIG's positions before -- the cow-model-fix audit.
    int legPos[4][3] = {
        {2, 0, 0},   // front-left
        {10, 0, 0},  // front-right
        {2, 0, 13},  // back-left
        {10, 0, 13}  // back-right
    };
    int legAnimDir[4] = {+1, -1, -1, +1};
    for(int i = 0; i < 4; i++) {
        displaylist_reset(&dl);
        vec3 pivot = { (lw / 2.0f) / 16.0f, lh / 16.0f, (ld / 2.0f) / 16.0f };
        mat4 leg_model;
        glm_mat4_identity(leg_model);
        glm_translate_make(leg_model, (vec3){ legPos[i][0] / 16.0f,
                                              legPos[i][1] / 16.0f,
                                              legPos[i][2] / 16.0f });
        glm_translate(leg_model, pivot);
        glm_rotate_x(leg_model, legAnimDir[i] * walkAngle, leg_model);
        glm_translate(leg_model, (vec3){ -pivot[0], -pivot[1], -pivot[2] });
        mat4 leg_mv;
        glm_mat4_mul(body_mv, leg_model, leg_mv);
        gfx_matrix_modelview(leg_mv);
        render_entity_create_cube(0, 0, 0, lw, lh, ld, cowLegUV, cowLegDir);
        displaylist_render_immediate(&dl, 24);
    }

    gfx_lighting(true);
    gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}

// Beta 1.7.3 ModelChicken (extends ModelBase): a 4x6x3 head with a 4x2x2 bill + 2x2x2 chin/wattle,
// a horizontal 6x6x8 body (Beta 6x8x6 + rotateAngleX=PI/2 baked), two 3x5x3 legs that alternate-walk,
// and two 1x4x6 wings that Z-flap. Pig-faithful FIFO-safe structure: per-cube displaylist_reset, body
// yaw + the +180 offset, a head submatrix (head+bill+chin ride it), per-leg X-rotation walk + per-wing
// Z-rotation flap. chicken.png absent -> creeper fallback. (cow/chicken render design, verified 2026-06-05.)
void render_entity_chicken(mat4 view, float headYawDeg, float bodyYawDeg, int frame,
                           float wingDestPos) {
    assert(view);

    float walkAngle = sinf((float)(frame % 60) / 60.0f * 2.0f * M_PI)
                      * (30.0f * (M_PI / 180.0f)) * limb_swing_amount;
    // Beta ModelChicken wing flap: rightWing.rotateAngleZ = (sin(wingRotation)+1)*destPos,
    // leftWing = -that. destPos is ~0 on the ground (EntityChicken folds the wings flat) and
    // ramps to ~1 when airborne; the caller derives it from the chicken's fall speed. wingRotation
    // accumulates ~+2/tick in Beta when airborne, so drive it from `frame`*2. Result: a grounded
    // chicken's wings are STILL (Beta-faithful -- the earlier constant sine flap was the bug), and
    // they flap (rotateAngleZ swings 0..2 rad) only as it falls/flutters down. (chicken wing fix.)
    float wingRot = (float)frame * 2.0f;
    float flapAngle = (sinf(wingRot) + 1.0f) * wingDestPos;

    // Body matrix: pivot (8,8,8) already sits on the model origin axis (M2
    // head fix: shared composition, render_entity_anchor.h).
    mat4 body_mv;
    vec3 bodyPivot = { 8.0f / 16.0f, 8.0f / 16.0f, 8.0f / 16.0f };
    render_entity_body_mv(view, bodyPivot, bodyYawDeg, body_mv);

    gfx_bind_texture(texture_chicken.data ? &texture_chicken : &texture_creeper);

    // --- Head (4x6x3): rides body_mv with its yaw DELTA about the Beta neck
    // rp(0,15,-4) -> v2 (8,9,4) (M2 head fix; this was the one quadruped-ish
    // head already on the 24-map -- the constant that proved the map -- but
    // its off-axis neck still drifted up to 8px under the old absolute
    // composition). Head + bill + chin all ride head_mv. ---
    mat4 head_mv;
    {
        vec3 neck = { 8.0f / 16.0f, 9.0f / 16.0f, 4.0f / 16.0f };
        render_entity_head_mv(body_mv, neck, headYawDeg, bodyYawDeg, 0.0f, head_mv);
    }
    displaylist_reset(&dl);
    gfx_matrix_modelview(head_mv);
    static const UVRect chkHeadUV[6] = {
        { 7, 0, 4, 3}, { 3, 0, 4, 3},
        {10, 3, 4, 6}, { 3, 3, 4, 6},
        { 0, 3, 3, 6}, { 7, 3, 3, 6}
    };
    static const int chkHeadDir[6] = {0, 0, -90, -90, -90, -90};
    render_entity_create_cube(6, 9, 2, 4, 6, 3, chkHeadUV, chkHeadDir);
    displaylist_render_immediate(&dl, 24);

    // --- Bill (4x2x2) + chin/wattle (2x2x2), both ride head_mv (no modelview re-call). ---
    static const UVRect chkBillUV[6] = {
        {20, 0, 4, 2}, {16, 0, 4, 2},
        {22, 2, 4, 2}, {16, 2, 4, 2},
        {14, 2, 2, 2}, {20, 2, 2, 2}
    };
    static const int chkBillDir[6] = {0, 0, -90, -90, -90, -90};
    displaylist_reset(&dl);
    render_entity_create_cube(6, 11, 0, 4, 2, 2, chkBillUV, chkBillDir);
    displaylist_render_immediate(&dl, 24);
    static const UVRect chkChinUV[6] = {
        {18, 4, 2, 2}, {16, 4, 2, 2},
        {20, 6, 2, 2}, {16, 6, 2, 2},
        {14, 6, 2, 2}, {18, 6, 2, 2}
    };
    static const int chkChinDir[6] = {0, 0, -90, -90, -90, -90};
    displaylist_reset(&dl);
    render_entity_create_cube(7, 9, 1, 2, 2, 2, chkChinUV, chkChinDir);
    displaylist_render_immediate(&dl, 24);

    // --- Body (6 wide x 6 tall x 8 long; Beta PI/2 baked into swapped dims). ---
    displaylist_reset(&dl);
    gfx_matrix_modelview(body_mv);
    static const UVRect chkBodyUV[6] = {
        { 6, 15, 6, 8}, {18, 15, 6, 8},
        {12,  9, 6, 6}, { 6,  9, 6, 6},
        { 0, 15, 6, 8}, {12, 15, 6, 8}
    };
    static const int chkBodyDir[6] = {0, 0, 0, 0, 90, -90};
    render_entity_create_cube(5, 5, 4, 6, 6, 8, chkBodyUV, chkBodyDir);
    displaylist_render_immediate(&dl, 24);

    // --- Two legs (3x5x3), alternating walk about the hip (y=5). Pig leg pattern. ---
    static const UVRect chkLegUV[6] = {
        {32, 0, 3, 3}, {29, 0, 3, 3},
        {35, 3, 3, 5}, {29, 3, 3, 5},
        {26, 3, 3, 5}, {32, 3, 3, 5}
    };
    static const int chkLegDir[6] = {0, 0, -90, -90, -90, -90};
    const int clw = 3, clh = 5, cld = 3;
    int chkLegPos[2][3] = { {5, 0, 6}, {8, 0, 6} }; // right, left (touch at centreline x=8)
    float chkLegAnim[2] = { +1.0f, -1.0f };         // opposite phase (Beta +PI)
    for(int i = 0; i < 2; i++) {
        displaylist_reset(&dl);
        vec3 pivot = { (clw / 2.0f) / 16.0f, clh / 16.0f, (cld / 2.0f) / 16.0f };
        mat4 leg_model;
        glm_mat4_identity(leg_model);
        glm_translate_make(leg_model, (vec3){ chkLegPos[i][0] / 16.0f,
                                              chkLegPos[i][1] / 16.0f,
                                              chkLegPos[i][2] / 16.0f });
        glm_translate(leg_model, pivot);
        glm_rotate_x(leg_model, chkLegAnim[i] * walkAngle, leg_model);
        glm_translate(leg_model, (vec3){ -pivot[0], -pivot[1], -pivot[2] });
        mat4 leg_mv;
        glm_mat4_mul(body_mv, leg_model, leg_mv);
        gfx_matrix_modelview(leg_mv);
        render_entity_create_cube(0, 0, 0, clw, clh, cld, chkLegUV, chkLegDir);
        displaylist_render_immediate(&dl, 24);
    }

    // --- Two wings (1x4x6), Z-flapping about the wing root. Pig pattern (cube at origin). ---
    static const UVRect chkWingUV[6] = {
        {31, 13, 1, 6}, {30, 13, 1, 6},
        {37, 19, 1, 4}, {30, 19, 1, 4},
        {24, 19, 6, 4}, {31, 19, 6, 4}
    };
    static const int chkWingDir[6] = {0, 0, -90, -90, -90, -90};
    const int ww = 1, wh = 4, wd = 6;
    int wingPos[2][3] = { {4, 7, 5}, {11, 7, 5} };  // right, left (cube origin; mirror about x=8)
    // Wing root = Beta rotationPoint (rp) RELATIVE to the cube origin: right rp(4,11,8)-pos(4,7,5)=(0,4,3);
    // left rp(12,11,8)-pos(11,7,5)=(1,4,3). The wing Z-flaps about this root (inner-top edge).
    vec3 wingRoot[2] = { { 0.0f / 16.0f, 4.0f / 16.0f, 3.0f / 16.0f },
                         { 1.0f / 16.0f, 4.0f / 16.0f, 3.0f / 16.0f } };
    float wingAnim[2] = { +1.0f, -1.0f };           // right +flap, left -flap (Beta leftWing = -var3)
    for(int i = 0; i < 2; i++) {
        displaylist_reset(&dl);
        mat4 wing_model;
        glm_mat4_identity(wing_model);
        glm_translate_make(wing_model, (vec3){ wingPos[i][0] / 16.0f,
                                               wingPos[i][1] / 16.0f,
                                               wingPos[i][2] / 16.0f });
        glm_translate(wing_model, wingRoot[i]);
        glm_rotate_z(wing_model, wingAnim[i] * flapAngle, wing_model);
        glm_translate(wing_model, (vec3){ -wingRoot[i][0], -wingRoot[i][1], -wingRoot[i][2] });
        mat4 wing_mv;
        glm_mat4_mul(body_mv, wing_model, wing_mv);
        gfx_matrix_modelview(wing_mv);
        render_entity_create_cube(0, 0, 0, ww, wh, wd, chkWingUV, chkWingDir);
        displaylist_render_immediate(&dl, 24);
    }

    gfx_lighting(true);
    gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}

// Beta 1.7.3 fleeceColorTable[16] RGB (index 0..15). White=identity; the four greys (0/7/8/15)
// are NEUTRAL (R==G==B) so the wool render uses the Beta-exact tint-toward-black path; the rest
// are chromatic. Client-only (needs cglm vec3) -> kept file-local here rather than in the shared
// monsters_object.h. Mirrors the int-only sheep_fleece_hash/color in that header.
static void sheep_fleece_rgb(int idx, vec3 out) {
    static const float TABLE[16][3] = {
        {1.0f,1.0f,1.0f}, {0.95f,0.7f,0.2f}, {0.9f,0.5f,0.85f}, {0.6f,0.7f,0.95f},
        {0.9f,0.9f,0.2f}, {0.5f,0.8f,0.1f},  {0.95f,0.7f,0.8f}, {0.3f,0.3f,0.3f},
        {0.6f,0.6f,0.6f}, {0.3f,0.6f,0.7f},  {0.7f,0.4f,0.9f},  {0.2f,0.4f,0.8f},
        {0.5f,0.4f,0.3f}, {0.4f,0.5f,0.2f},  {0.8f,0.3f,0.3f},  {0.1f,0.1f,0.1f}
    };
    if(idx < 0 || idx > 15) idx = 0;
    out[0] = TABLE[idx][0]; out[1] = TABLE[idx][1]; out[2] = TABLE[idx][2];
}

// Beta 1.7.3 RenderSheep = ModelSheep2 (BARE body, sheep.png) + ModelSheep1 (WOOL overlay,
// sheep_fur.png, colour-tinted). Both extend ModelQuadruped super(12,0) -- the SAME base as the
// proven render_entity_cow, so every Beta->v2 map below is the cow's verified map re-applied to
// the sheep's rp/box. PASS 1 draws the bare body untinted; PASS 2 draws the inflated wool overlay
// on the SAME head/body/leg matrices, bracketed by gfx_entity_tint. The bare model has 0 inflate
// (exact); only the wool overlay rounds the fractional inflate to integer px (flagged below).
// GEOMETRY: BODY uses the PI/2-bake closed form (x0=rp_x+bx0+8, y0=(24-rp_y)+bz0,
// z0=rp_z+by0+8, dims (w,d,h)) -- derived from rotX=+PI/2 (y'=-z, z'=y) + the verified frame
// map (x+8, 24-y_down, z+8); it reproduces the chicken/cow/pig bodies exactly. (The old
// "(22-rp_y)-d/2 / (rp_z+9)-h/2" centre fudge was the 24-map body re-bake's target: it left
// the sheep body 2px high + 3px back, and the cow/pig bodies 2px back.)
void render_entity_sheep(mat4 view, float headYawDeg, float bodyYawDeg, int frame,
                         int fleeceColor, bool sheared) {
    assert(view);

    float walkAngle = sinf((float)(frame % 60) / 60.0f * 2.0f * M_PI)
                      * (30.0f * (M_PI / 180.0f)) * limb_swing_amount;

    // ---- Shared body matrix (body + all 4 legs ride this): yaw about the
    // MODEL ORIGIN AXIS x=z=8px (M2 head fix -- the old z=11 box-centre pivot
    // was most of the sheep's 16px head drift). ----
    mat4 body_mv;
    vec3 bodyPivot = {
        8.0f / 16.0f,
        (14.0f + 6.0f / 2.0f) / 16.0f,   // 17/16
        8.0f / 16.0f
    };
    render_entity_body_mv(view, bodyPivot, bodyYawDeg, body_mv);

    // ---- Shared head submatrix (bare + wool head ride this): the head rides
    // body_mv with its yaw DELTA about the Beta neck rp(0,6,-8) -> v2
    // (8,18,0) on the 24-map (M2 head fix; the old 16 was the 22-map's 2px
    // sink). The JOINT, not the head's y-centre -- Beta rotates about the
    // rotationPoint. ----
    mat4 head_mv;
    {
        vec3 neck = { 8.0f / 16.0f, 18.0f / 16.0f, 0.0f / 16.0f };
        render_entity_head_mv(body_mv, neck, headYawDeg, bodyYawDeg, 0.0f, head_mv);
    }

    // Shared leg placement (bare 4x12x4 legs drawn at origin; the per-leg matrix translates here).
    // legPos = (rp_x+8, 0, rp_z+8) - boxHalf(2): rp +/-3 -> x{3,9}; z back 7->13, front -5->1.
    const int legPos[4][3] = {
        {3, 0, 1},   // front-left  (FL)
        {9, 0, 1},   // front-right (FR)
        {3, 0, 13},  // back-left   (BL)
        {9, 0, 13}   // back-right  (BR)
    };
    const int legAnimDir[4] = {+1, -1, -1, +1};

    // =====================================================================================
    // PASS 1 -- BARE ModelSheep2 (sheep.png), UNTINTED. Fallback to creeper if sheep.png absent.
    // =====================================================================================
    gfx_bind_texture(texture_sheep.data ? &texture_sheep : &texture_creeper);

    // --- BARE HEAD (6x6x8). Beta addBox(-3,-4,-6,6,6,8,0)@rp(0,6,-8) -> x0=5,y0=14,z0=-6. ---
    static const UVRect sheepHeadUV[6] = {
        {14, 0, 6, 8}, { 8, 0, 6, 8},   // bottom, top  (W x D = 6 x 8)
        {22, 8, 6, 6}, { 8, 8, 6, 6},   // north,  south(W x H = 6 x 6)
        { 0, 8, 8, 6}, {14, 8, 8, 6}    // west,   east (D x H = 8 x 6)
    };
    static const int sheepHeadDir[6] = {0, 0, -90, -90, -90, -90};
    displaylist_reset(&dl);
    gfx_matrix_modelview(head_mv);
    // Beta addBox(-3,-4,-6)@rp(0,6,-8) on the 24-map: y 16..22 (the old 14
    // was the 22-map's 2px sink), z -6..2.
    render_entity_create_cube(5, 16, -6, 6, 6, 8, sheepHeadUV, sheepHeadDir);
    displaylist_render_immediate(&dl, 24);

    // --- BARE BODY (8 wide x 6 tall x 16 long; Beta 8x16x6 PI/2-baked, dims swap H<->D).
    //     Beta addBox(-4,-10,-7,8,16,6)@rp(0,5,2) on the PI/2-bake map: (4,12,0) -- body top
    //     y 18 under the head (16..22), front face flush at z=0 like the cow (24-map body
    //     re-bake; the old (4,14,3) centre fudge floated it 2px high + 3px back). ---
    static const UVRect sheepBodyUV[6] = {
        {34, 14, 8, 16}, {48, 14, 8, 16}, // bottom, top  (sx x sz = 8 x 16)
        {42,  8, 8,  6}, {34,  8, 8,  6}, // north,  south(sx x sy = 8 x 6)
        {28, 14, 6, 16}, {42, 14, 6, 16}  // west,   east (sy x sz = 6 x 16, swapped by dir)
    };
    static const int sheepBodyDir[6] = {0, 0, 0, 0, 90, -90};
    displaylist_reset(&dl);
    gfx_matrix_modelview(body_mv);
    render_entity_create_cube(4, 12, 0, 8, 6, 16, sheepBodyUV, sheepBodyDir);
    displaylist_render_immediate(&dl, 24);

    // --- BARE LEGS x4 (4x12x4), walk-swinging about the hip (cube-y 12). cow leg UV verbatim
    //     (same 4x12x4 box @ texOffset(0,16)). ---
    static const UVRect sheepLegUV[6] = {
        { 8, 16, 4, 4}, { 4, 16, 4, 4},
        {12, 20, 4, 12}, { 4, 20, 4, 12},
        { 0, 20, 4, 12}, { 8, 20, 4, 12}
    };
    static const int sheepLegDir[6] = {0, 0, -90, -90, -90, -90};
    for(int i = 0; i < 4; i++) {
        displaylist_reset(&dl);
        vec3 pivot = { (4 / 2.0f) / 16.0f, 12.0f / 16.0f, (4 / 2.0f) / 16.0f }; // hip {2,12,2}/16
        mat4 leg_model, leg_mv;
        glm_translate_make(leg_model, (vec3){ legPos[i][0] / 16.0f, legPos[i][1] / 16.0f,
                                              legPos[i][2] / 16.0f });
        glm_translate(leg_model, pivot);
        glm_rotate_x(leg_model, legAnimDir[i] * walkAngle, leg_model);
        glm_translate(leg_model, (vec3){ -pivot[0], -pivot[1], -pivot[2] });
        glm_mat4_mul(body_mv, leg_model, leg_mv);
        gfx_matrix_modelview(leg_mv);
        render_entity_create_cube(0, 0, 0, 4, 12, 4, sheepLegUV, sheepLegDir);
        displaylist_render_immediate(&dl, 24);
    }

    // =====================================================================================
    // PASS 2 -- WOOL ModelSheep1 (sheep_fur.png), COLOUR-TINTED. The inflated wool boxes ride the
    // IDENTICAL head_mv / body_mv / per-leg matrices, so the wool tracks the bare model's gait.
    // SHEARED: Beta RenderSheep.shouldRenderPass returns false for a sheared sheep -- the ENTIRE
    // fur overlay (head + body + legs) is skipped; only the bare ModelSheep2 remains. No partial
    // wool cap in b1.7.3. [740c583: RenderSheep.setWoolColorAndRender `!getSheared()`]
    // =====================================================================================
    if(sheared) {
        gfx_lighting(true);
        gfx_matrix_modelview(GLM_MAT4_IDENTITY);
        return;
    }
    // --- WOOL TINT = Beta-EXACT per-channel multiply by the fleece colour. Beta RenderSheep does
    //     glColor3f(fr,fg,fb) and the wool texture is MODULATED (src*colour per channel); gfx_entity_
    //     modulate reproduces that exactly for ALL 16 fleece colours (white f=1 -> identity -> raw
    //     white wool; gray {.3,.3,.3} -> wool*0.3; pink/brown -> true per-channel multiply -- the
    //     earlier scalar-lerp could only match the neutral R==G==B subset). FIRST clear any inherited
    //     hurt/swell LERP tint (the BARE pass already flashed; the wool layer shows pure fleece, a
    //     documented cosmetic deviation), THEN apply the fleece multiply. ---
    vec3 frgb;
    sheep_fleece_rgb(fleeceColor, frgb);
    gfx_entity_tint(0.0f, 0.0f, 0.0f, 0.0f);        // drop the inherited hurt-flash lerp
    gfx_entity_modulate(frgb[0], frgb[1], frgb[2]); // wool * fleece, per channel (Beta-exact)

    gfx_bind_texture(texture_sheep_fur.data ? &texture_sheep_fur : &texture_creeper);

    // --- WOOL HEAD (Beta addBox(-3,-4,-4,6,6,6,0.6)@rp(0,6,-8) -- a forward-shifted box). Inflated
    //     7.2^3, rounded CONCENTRIC about centre (8,17,-1) to 8x8x8 -> (4,13,-5). Rides head_mv.
    //     ROUNDING FLAG: 7.2->8 (~0.4px/side loose shell). +1px forward-Z is FAITHFUL (Beta wool
    //     head is shifted forward), not a rounding error. UV: Beta addBox computes texcoords from
    //     the BASE box dims (6x6x6 @ texOffset 0,0) -- the +0.6 INFLATE grows only the geometry,
    //     never the UV -- so the 6x6x6 net stretches over the rounded 8^3 geometry (Beta-faithful;
    //     using the inflated dims here is what overflowed/mis-sampled the body). ---
    static const UVRect woolHeadUV[6] = {
        {12, 0, 6, 6}, { 6, 0, 6, 6}, // bottom, top  (w x d = 6 x 6)
        {18, 6, 6, 6}, { 6, 6, 6, 6}, // north,  south(w x h = 6 x 6)
        { 0, 6, 6, 6}, {12, 6, 6, 6}  // west,   east (d x h = 6 x 6)
    };
    static const int woolHeadDir[6] = {0, 0, -90, -90, -90, -90};
    displaylist_reset(&dl);
    gfx_matrix_modelview(head_mv);
    // Beta box(-3,-4,-4, 6,6,6, 0.6) @ rp(0,6,-8) -> inflated 7.2^3 origin (-3.6,-4.6,-4.6). Head
    // frame map (x0=rp_x+ox+8, y0=22-(rp_y+oy+sy), z0=rp_z+oz+8): (4.4, 13.4, -4.6) size 7.2^3.
    // Concentric with the bare head (centre x8,y17) + the faithful +1px forward-Z (z-centre -1).
    // Wool head raised with the bare head to the 24-map: Beta inflate-0.6 box
    // down-y 1.4..8.6 -> v2 y 15.4..22.6 (was 13.4 on the 22-map).
    render_entity_create_cube_float(4.4f, 15.4f, -4.6f, 7.2f, 7.2f, 7.2f, woolHeadUV, woolHeadDir);
    displaylist_render_immediate(&dl, 24);

    // --- WOOL BODY (Beta addBox(-4,-10,-7,8,16,6,1.75)@rp(0,5,2) PI/2). Inflated 11.5x19.5x9.5,
    //     baked H<->D -> 11.5(w) x 9.5(h) x 19.5(d), CONCENTRIC about the bare body centre (8,15,8):
    //     x0=8-11.5/2=2.25, y0=15-9.5/2=10.25, z0=8-19.5/2=-1.75. Float-exact (was rounded 12x10x20),
    //     re-baked with the bare body (the old (2.25,12.25,1.25) rode the bare body's old fudge).
    //     Envelopes the bare body on every face (no inside-out clip). Rides body_mv. UV: SAME
    //     texOffset(28,8) + SAME base dims (8x16x6) as the bare body, so the wool body net is
    //     IDENTICAL to sheepBodyUV (only the texture FILE differs). The base 8x6x16 net stretches
    //     over the inflated geometry, Beta-faithful. (The earlier inflated-DIM UV overflowed 64x32
    //     and mis-sampled -> tan stripes; the UV must always be the base-dim net.) ---
    static const UVRect woolBodyUV[6] = {
        {34, 14, 8, 16}, {48, 14, 8, 16},
        {42,  8, 8,  6}, {34,  8, 8,  6},
        {28, 14, 6, 16}, {42, 14, 6, 16}
    };
    static const int woolBodyDir[6] = {0, 0, 0, 0, 90, -90};
    displaylist_reset(&dl);
    gfx_matrix_modelview(body_mv);
    render_entity_create_cube_float(2.25f, 10.25f, -1.75f, 11.5f, 9.5f, 19.5f, woolBodyUV, woolBodyDir);
    displaylist_render_immediate(&dl, 24);

    // --- WOOL LEGS x4 (Beta OVERRIDDEN addBox(-2,0,-2,4,6,4,0.5) -- stubby). Inflated 5x7x5 and,
    //     CRUCIALLY, CONCENTRIC with the bare 4x12x4 leg (Beta both centre on the leg axis): bare
    //     leg cube x[0,4]/z[0,4] (centre 2); the inflated wool is x[-0.5,4.5]/z[-0.5,4.5] (centre 2),
    //     covering the upper leg y[5.5,12.5] (Beta wool box y[0,6] inflated -> v2 y = 12 - betaY).
    //     The swing pivot is the HIP {2,12,2}/16 -- the EXACT SAME axis the bare leg rotates about
    //     (lw/2=2, lh=12), so the wool co-swings in LOCKSTEP with no shear. (Earlier the cube sat at
    //     x0/z0=0 and the pivot at 2.5 -- the wool's own half-size leaking in -- so it was 0.5px
    //     off-centre AND rotated about a different axis -> sheared off the bare leg at full stride.)
    // UV: Beta base dims 4x6x4 @ texOffset(0,16) (the +0.5 inflate grows only geometry); the
    // 4x6x4 net stretches over the 5x7x5 geometry.
    static const UVRect woolLegUV[6] = {
        { 8, 16, 4, 4}, { 4, 16, 4, 4}, // bottom, top  (w x d = 4 x 4)
        {12, 20, 4, 6}, { 4, 20, 4, 6}, // north,  south(w x h = 4 x 6)
        { 0, 20, 4, 6}, { 8, 20, 4, 6}  // west,   east (d x h = 4 x 6)
    };
    static const int woolLegDir[6] = {0, 0, -90, -90, -90, -90};
    for(int i = 0; i < 4; i++) {
        displaylist_reset(&dl);
        vec3 pivot = { 2.0f / 16.0f, 12.0f / 16.0f, 2.0f / 16.0f }; // hip -- MUST match bare leg pivot
        mat4 leg_model, leg_mv;
        glm_translate_make(leg_model, (vec3){ legPos[i][0] / 16.0f, legPos[i][1] / 16.0f,
                                              legPos[i][2] / 16.0f });
        glm_translate(leg_model, pivot);
        glm_rotate_x(leg_model, legAnimDir[i] * walkAngle, leg_model);
        glm_translate(leg_model, (vec3){ -pivot[0], -pivot[1], -pivot[2] });
        glm_mat4_mul(body_mv, leg_model, leg_mv);
        gfx_matrix_modelview(leg_mv);
        // concentric with the bare leg (origin -0.5,-0.5) + the upper-leg y-lift baked into y0=5.5
        render_entity_create_cube_float(-0.5f, 5.5f, -0.5f, 5.0f, 7.0f, 5.0f, woolLegUV, woolLegDir);
        displaylist_render_immediate(&dl, 24);
    }

    // --- MANDATORY reset (GX FIFO safety; harmless double-reset vs entity_render's own reset).
    //     Undo the fleece multiply (back to identity 1,1,1) AND clear any tint -- so the next
    //     entity/world/GUI draw is neither modulated nor lerped. NOTE: a hurt sheep flashes red on
    //     the BARE pass but the wool pass shows pure fleece (documented cosmetic deviation). ---
    gfx_entity_modulate(1.0f, 1.0f, 1.0f);
    gfx_entity_tint(0.0f, 0.0f, 0.0f, 0.0f);

    gfx_lighting(true);
    gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}

// --- Beta 1.7.3 SLIME (ModelSlime + RenderSlime, decompile-verified 2026-06-11) ---
// Two models in Beta: ModelSlime(16) = the opaque INNER core (6x6x6 @ tex 0,16)
// with eyes (2x2x2 @ 32,0 and 32,4) and mouth (1x1x1 @ 32,8); ModelSlime(0) =
// the 8x8x8 outer GEL (@ 0,0) drawn as render-pass 0 with GL_BLEND
// SRC_ALPHA/ONE_MINUS_SRC_ALPHA. Draw order here: opaque parts FIRST, gel LAST
// inside a gfx_blending(MODE_BLEND)+gfx_alpha_test(false) bracket with the
// MANDATORY restore (the entity_shadow precedent -- on GX a leaked blend state
// risks a FIFO hang). The eyes/mouth sit at fractional px (the -3.25/-3.5
// Beta offsets) between the core front (z=5) and the shell front (z=4), so
// they use the float cube builder -- rounding would z-fight one or the other.
// The CALLER scales the shared model matrix by the Beta size (1/2/4) about the
// feet; this fn draws the size-1 model. ModelSlime has NO head joint and an
// empty setRotationAngles -- body yaw only, on-axis pivot (headYawDeg unused).
// Squish squash-and-stretch (RenderSlime.scaleSlime) is DEFERRED: it needs the
// server's hop/land squish field networked -- documented, not silently dropped.
void render_entity_slime(mat4 view, float headYawDeg, float bodyYawDeg, int frame) {
    assert(view);
    (void)headYawDeg; // no head joint on the slime model
    (void)frame;      // no walk gait either (it hops bodily)

    mat4 model, body_mv;
    glm_mat4_identity(model);
    glm_translate_make(model, (vec3){ -0.5f, 0.0f, -0.5f });
    vec3 pivot = { 0.5f, 0.0f, 0.5f }; // on-axis: yaw about the model centre
    glm_translate(model, pivot);
    glm_rotate_y(model, glm_rad(bodyYawDeg + 180.0f), model);
    glm_translate(model, (vec3){ -pivot[0], -pivot[1], -pivot[2] });
    glm_mat4_mul(view, model, body_mv);
    gfx_matrix_modelview(body_mv);

    // slime.png is ABSENT until the legal pack lands -> creeper-skin fallback
    // (mis-skinned + opaque gel, but visible). A real Beta pack overlays it.
    gfx_bind_texture(texture_slime.data ? &texture_slime : &texture_creeper);

    // Inner core 6x6x6 @ tex (0,16). Beta addBox(-3,17,-3): v2 24-map -> y 1..7.
    static const UVRect coreUV[6] = {
        {12, 16, 6, 6}, { 6, 16, 6, 6},
        { 0, 22, 6, 6}, { 6, 22, 6, 6},
        {12, 22, 6, 6}, {18, 22, 6, 6}
    };
    static const int slimeDir[6] = {0, 0, -90, -90, -90, -90};
    displaylist_reset(&dl);
    render_entity_create_cube(5, 1, 5, 6, 6, 6, coreUV, slimeDir);
    displaylist_render_immediate(&dl, 24);

    // Right eye 2x2x2 @ (32,0): Beta addBox(-3.25,18,-3.5) -> x 4.75, y 4, z 4.5.
    static const UVRect eyeRUV[6] = {
        {36, 0, 2, 2}, {34, 0, 2, 2},
        {32, 2, 2, 2}, {34, 2, 2, 2},
        {36, 2, 2, 2}, {38, 2, 2, 2}
    };
    displaylist_reset(&dl); // BASE-RESET rule: every cube rewinds to slot 0
    render_entity_create_cube_float(4.75f, 4.0f, 4.5f, 2.0f, 2.0f, 2.0f, eyeRUV,
                                    slimeDir);
    displaylist_render_immediate(&dl, 24);

    // Left eye 2x2x2 @ (32,4): Beta addBox(1.25,18,-3.5) -> x 9.25.
    static const UVRect eyeLUV[6] = {
        {36, 4, 2, 2}, {34, 4, 2, 2},
        {32, 6, 2, 2}, {34, 6, 2, 2},
        {36, 6, 2, 2}, {38, 6, 2, 2}
    };
    displaylist_reset(&dl);
    render_entity_create_cube_float(9.25f, 4.0f, 4.5f, 2.0f, 2.0f, 2.0f, eyeLUV,
                                    slimeDir);
    displaylist_render_immediate(&dl, 24);

    // Mouth 1x1x1 @ (32,8): Beta addBox(0,21,-3.5) -> x 8, y 2, z 4.5.
    static const UVRect mouthUV[6] = {
        {34, 8, 1, 1}, {33, 8, 1, 1},
        {32, 9, 1, 1}, {33, 9, 1, 1},
        {34, 9, 1, 1}, {35, 9, 1, 1}
    };
    displaylist_reset(&dl);
    render_entity_create_cube_float(8.0f, 2.0f, 4.5f, 1.0f, 1.0f, 1.0f, mouthUV,
                                    slimeDir);
    displaylist_render_immediate(&dl, 24);

    // Outer gel 8x8x8 @ (0,0), LAST, blended (Beta render-pass 0:
    // SRC_ALPHA/ONE_MINUS_SRC_ALPHA). alpha_test OFF so the gel's partial
    // alpha is blended, not cut. MANDATORY restore right after -- never leak
    // blend state into the next entity/world draw (GX FIFO discipline).
    static const UVRect gelUV[6] = {
        {16, 0, 8, 8}, { 8, 0, 8, 8},
        { 0, 8, 8, 8}, { 8, 8, 8, 8},
        {16, 8, 8, 8}, {24, 8, 8, 8}
    };
    gfx_blending(MODE_BLEND);
    gfx_alpha_test(false);
    displaylist_reset(&dl);
    render_entity_create_cube(4, 0, 4, 8, 8, 8, gelUV, slimeDir);
    displaylist_render_immediate(&dl, 24);
    gfx_blending(MODE_OFF);
    gfx_alpha_test(true);

    gfx_lighting(true);
    gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}

// --- Beta 1.7.3 SQUID (ModelSquid + RenderSquid, decompile-verified 2026-06-11) ---
// Body 12x16x12 @ tex (0,0) and EIGHT tentacles 2x18x2 @ tex (48,0) hanging
// from a radius-5px ring under the body, each pre-yawed i*-2PI/8 + PI/2 (so
// its swing axis points radially) and swung about X by the tentacle angle.
// Beta drives that angle from the entity's tentacleAngle pulse; the server's
// exact phase is NOT networked, so the swing derives from the shared walk
// frame + limb_swing (documented approximation -- same wave family, free).
// Beta's whole-model pitch/roll (RenderSquid rotateCorpse) is DEFERRED with
// it: the squid renders upright. Placement: body down-y 0..16 -> v2 y 8..24
// (24-map); tentacle joints at down-y 15 -> v2 y 9, boxes hanging 18px down
// (below the feet underwater -- faithful overhang, the bbox is collision-only).
void render_entity_squid(mat4 view, float headYawDeg, float bodyYawDeg, int frame) {
    assert(view);
    (void)headYawDeg; // no separate head; the body yaws as one

    // Tentacle wave: |sin| * PI/4 amplitude (the Beta tentacleAngle range),
    // scaled by limb_swing so a still squid relaxes straight.
    float tentacleAngle = fabsf(sinf((float)(frame % 60) / 60.0f * 2.0f * M_PI))
                          * (float)M_PI * 0.25f * limb_swing_amount;

    mat4 model, body_mv;
    glm_mat4_identity(model);
    glm_translate_make(model, (vec3){ -0.5f, 0.0f, -0.5f });
    vec3 pivot = { 0.5f, 0.0f, 0.5f };
    glm_translate(model, pivot);
    glm_rotate_y(model, glm_rad(bodyYawDeg + 180.0f), model);
    glm_translate(model, (vec3){ -pivot[0], -pivot[1], -pivot[2] });
    glm_mat4_mul(view, model, body_mv);
    gfx_matrix_modelview(body_mv);

    gfx_bind_texture(texture_squid.data ? &texture_squid : &texture_creeper);

    // Body 12x16x12 @ (0,0): Beta addBox(-6,-8,-6) rp_y 8 -> down-y 0..16 ->
    // v2 x/z 2..14, y 8..24.
    static const UVRect bodyUV[6] = {
        {24, 0, 12, 12}, {12, 0, 12, 12},
        { 0, 12, 12, 16}, {12, 12, 12, 16},
        {24, 12, 12, 16}, {36, 12, 12, 16}
    };
    static const int squidDir[6] = {0, 0, -90, -90, -90, -90};
    displaylist_reset(&dl);
    render_entity_create_cube(2, 8, 2, 12, 16, 12, bodyUV, squidDir);
    displaylist_render_immediate(&dl, 24);

    // Eight tentacles 2x18x2 @ (48,0) on the radius-5 ring, joints at v2 y 9,
    // boxes hanging downward (create_cube(-1,-18,-1) under the joint matrix).
    // Per-tentacle: T(joint) * R_y(i*-2PI/8 + PI/2) * R_x(-tentacleAngle) --
    // the X-rotation sign is flipped vs Beta's down-y model space so the
    // tentacles flare OUTWARD (the spider-leg joint-matrix pattern). Every
    // cube gets its own displaylist_reset (eight tentacles is EXACTLY the
    // spider-legs BASE-replay failure mode).
    static const UVRect tentUV[6] = {
        {52, 0, 2, 2}, {50, 0, 2, 2},
        {48, 2, 2, 18}, {50, 2, 2, 18},
        {52, 2, 2, 18}, {54, 2, 2, 18}
    };
    for(int i = 0; i < 8; i++) {
        float ring = (float)i * 2.0f * (float)M_PI / 8.0f;
        vec3 joint = { (8.0f + cosf(ring) * 5.0f) / 16.0f,
                       9.0f / 16.0f,
                       (8.0f + sinf(ring) * 5.0f) / 16.0f };
        float tyaw = (float)i * -2.0f * (float)M_PI / 8.0f + (float)M_PI * 0.5f;
        mat4 tent_model, tent_mv;
        glm_mat4_identity(tent_model);
        glm_translate_make(tent_model, joint);
        glm_rotate_y(tent_model, tyaw, tent_model);
        glm_rotate_x(tent_model, -tentacleAngle, tent_model);
        glm_mat4_mul(body_mv, tent_model, tent_mv);
        gfx_matrix_modelview(tent_mv);
        displaylist_reset(&dl); // BASE-RESET rule, per tentacle
        render_entity_create_cube(-1, -18, -1, 2, 18, 2, tentUV, squidDir);
        displaylist_render_immediate(&dl, 24);
    }

    gfx_lighting(true);
    gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}
