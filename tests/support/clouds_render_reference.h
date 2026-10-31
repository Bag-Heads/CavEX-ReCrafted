/* Frozen pre-owned renderer. Only name and clock parameter differ. */
static void clouds_reference(mat4 view_matrix, float brightness, ptime_t now) {
	assert(view_matrix);

	float cloud_pos = (gstate.world_time
					   + time_diff_s(gstate.world_time_start, now)
						   * 1000.0F / DAY_TICK_MS)
		* 12.0F / 400.0F;

	vec3 shift = {roundf(gstate.players[0].camera.x / 12.0F) * 12.0F
					  - (cloud_pos - roundf(cloud_pos / 12.0F) * 12.0F),
				  108.5F, roundf(gstate.players[0].camera.z / 12.0F) * 12.0F};

	mat4 model_view;
	glm_translate_to(view_matrix, shift, model_view);
	gfx_matrix_modelview(model_view);

	gfx_fog(true);
	gfx_fog_pos(shift[0] - gstate.players[0].camera.x, shift[2] - gstate.players[0].camera.z,
				gstate.config.fog_distance * 1.5F);
	gfx_texture(false);
	gfx_blending(MODE_BLEND);
	gfx_lighting(false);
	gfx_cull_func(MODE_NONE);
	gfx_write_buffers(false, true, true);

	int ox = roundf(gstate.players[0].camera.x / 12.0F) + roundf(cloud_pos / 12.0F);
	int oy = roundf(gstate.players[0].camera.z / 12.0F);

	uint8_t shade[6] = {
		roundf(brightness * 1.0F * 255),  roundf(brightness * 0.75F * 255),
		roundf(brightness * 0.84F * 255), roundf(brightness * 0.84F * 255),
		roundf(brightness * 0.92F * 255), roundf(brightness * 0.92F * 255),
	};

	// FACE-CULLING (2026-06-13 cloud reduction, wf_906f4fa3): the old code emitted
	// an unconditional 24-vert 6-face solid cube for EVERY opaque cell of a 33x33
	// grid, TWICE = up to ~52k immediate verts/frame (the GP-saturation freeze's
	// dominant producer). Beta's own renderCloudsFancy culls per-face -- interior
	// cells draw only top+bottom; side faces only at exposed edges. Match that:
	// precompute the opacity grid ONCE (was a texture lookup per cell PER pass),
	// then per cell emit top+bottom (always) + each side ONLY where the abutting
	// cell is transparent. Removes the occluded interior side faces -- a faithful
	// cut (the dropped faces were never visible), not a shortcut. ~2-3x fewer
	// verts; the silhouette is identical (only shared interior faces vanish).
	#define CLOUD_GRID 33 // x,y in [-16,16] inclusive
	uint8_t op[CLOUD_GRID * CLOUD_GRID];
	for(int y = -16; y <= 16; y++)
		for(int x = -16; x <= 16; x++) {
			uint8_t c[4];
			tex_gfx_lookup(&texture_clouds, ox + x, oy + y, c);
			op[(y + 16) * CLOUD_GRID + (x + 16)] = c[3] >= 128;
		}
	// Out-of-grid neighbours read NOT opaque, so the field perimeter keeps its
	// side faces (an off-bitmap edge must never be culled).
	#define CLOUD_OP(X, Y)                                                       \
		((X) >= -16 && (X) <= 16 && (Y) >= -16 && (Y) <= 16                       \
		 && op[((Y) + 16) * CLOUD_GRID + ((X) + 16)])

	// Each face: 4 verts encoded (xb,yb,zb) selecting min/box per axis
	// (X=min[0]/box[0], Y=min[2]/box[2] the 0..4 thickness, Z=min[1]/box[1]);
	// shade index; (dx,dy) of the neighbour cell that occludes it (8,8 = never).
	static const struct {
		uint8_t v[4][3];
		uint8_t shade;
		int8_t dx, dy;
	} FACES[6] = {
		{{{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}}, 0, 8, 8},  // top
		{{{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}}, 1, 8, 8},  // bottom
		{{{0, 1, 0}, {0, 0, 0}, {1, 0, 0}, {1, 1, 0}}, 2, 0, -1}, // -Z (back)
		{{{0, 1, 1}, {1, 1, 1}, {1, 0, 1}, {0, 0, 1}}, 3, 0, 1},  // +Z (front)
		{{{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}}, 4, 1, 0},  // +X (right)
		{{{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}}, 5, -1, 0}, // -X (left)
	};

	for(int k = 0; k < 2; k++) {
		if(k == 1)
			gfx_write_buffers(true, false, true);

		for(int y = -16; y <= 16; y++) {
			for(int x = -16; x <= 16; x++) {
				if(!CLOUD_OP(x, y))
					continue;

				int16_t ax[2] = {x * 12, x * 12 + 12};   // min[0], box[0]
				int16_t ay[2] = {0, 4};                  // min[2], box[2]
				int16_t az[2] = {y * 12, y * 12 + 12};   // min[1], box[1]

				int16_t verts[6 * 4 * 3];
				uint8_t cols[6 * 4 * 4];
				uint16_t tex[6 * 4 * 2] = {0}; // texturing off; dead but VTXFMT2-shaped
				int nv = 0;

				for(int f = 0; f < 6; f++) {
					if(FACES[f].dx != 8
					   && CLOUD_OP(x + FACES[f].dx, y + FACES[f].dy))
						continue; // shared interior face -- occluded, skip
					uint8_t s = shade[FACES[f].shade];
					for(int i = 0; i < 4; i++) {
						verts[nv * 3 + 0] = ax[FACES[f].v[i][0]];
						verts[nv * 3 + 1] = ay[FACES[f].v[i][1]];
						verts[nv * 3 + 2] = az[FACES[f].v[i][2]];
						cols[nv * 4 + 0] = s;
						cols[nv * 4 + 1] = s;
						cols[nv * 4 + 2] = s;
						cols[nv * 4 + 3] = 0xBF;
						nv++;
					}
				}

				if(nv)
					gfx_draw_quads(nv, verts, cols, tex);
			}
		}
	}
	#undef CLOUD_OP
	#undef CLOUD_GRID

	gfx_write_buffers(true, true, true);
	gfx_blending(MODE_OFF);
	gfx_texture(true);
	gfx_cull_func(MODE_BACK);
	gfx_fog(false);
	// Mandatory restore (gutil_sky_box precedent, 95159f6, same file): the
	// gfx_lighting(false) at this function's top was never paired with a
	// restore, leaving CLR0 in DIRECT for whatever draws next. gutil_clouds
	// is called exactly once per frame (main.c, overworld only) and is
	// IMMEDIATELY followed by gfx_mode_gui(), which itself unconditionally
	// re-asserts gfx_lighting(false) -- so this omission produces no
	// observable symptom in the current call graph (verified: the only
	// caller has no draw call between gutil_clouds and gfx_mode_gui). That is
	// exactly the accidental-ordering dependency gutil_sky_box had before
	// 95159f6 fixed it: correctness rode on what happens to run next, not on
	// this function's own contract. Restore explicitly so a future caller
	// (e.g. a debug/HUD draw inserted between clouds and gfx_mode_gui) can't
	// silently reintroduce the CLR0-desync hang class. Restored last, matching
	// the established idiom (gutil_sky_box's own tail, render_weather.c).
	gfx_lighting(true);
}

