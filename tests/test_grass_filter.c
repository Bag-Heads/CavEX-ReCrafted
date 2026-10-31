/* design-crit-1010 DC-GRASS G3: PC terrain atlas sampler policy.
 *
 * Beta 1.7.3 client RenderEngine.java:20 `useMipmaps = false` and
 * RenderEngine.java:168-169 set GL_NEAREST for both MIN and MAG on every
 * setupTexture (terrain.png included). So the PC terrain atlas must be
 * sampled NEAREST/NEAREST from level 0 only -- on a fresh load, on a
 * pack-cache restore, and on a pack reload (re-entrant load).
 *
 * Production-linked: the real source/platform/pc/texture.c is compiled into
 * this test, and the GL 1.1 entry points it calls are replaced by a recorder,
 * so the assertions read the actual sampler/upload state texture.c requested.
 * Non-terrain textures (particles atlas, plain linear/nearest loads) must keep
 * their existing filters. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/platform/pc/texture.c"

struct tex_gfx texture_terrain;
struct tex_gfx texture_particles;

/* ---- GL recorder ------------------------------------------------------ */
#define REC_MAX_TEX 64
struct rec_tex {
	GLint min_filter, mag_filter, base_level, max_level;
	unsigned levels_uploaded; /* bit n = glTexImage2D level n */
	int live;
};
static struct rec_tex rec[REC_MAX_TEX];
static GLuint rec_next = 1, rec_bound = 0;

void GLAPIENTRY glGenTextures(GLsizei n, GLuint* ids) {
	for(GLsizei i = 0; i < n; i++) {
		assert(rec_next < REC_MAX_TEX);
		ids[i] = rec_next++;
		memset(&rec[ids[i]], 0, sizeof(rec[0]));
		rec[ids[i]].min_filter = -1;
		rec[ids[i]].mag_filter = -1;
		rec[ids[i]].base_level = 0; /* GL defaults */
		rec[ids[i]].max_level = 1000;
		rec[ids[i]].live = 1;
	}
}
void GLAPIENTRY glDeleteTextures(GLsizei n, const GLuint* ids) {
	for(GLsizei i = 0; i < n; i++)
		if(ids[i] < REC_MAX_TEX)
			rec[ids[i]].live = 0;
}
void GLAPIENTRY glBindTexture(GLenum target, GLuint id) {
	assert(target == GL_TEXTURE_2D);
	rec_bound = id;
}
void GLAPIENTRY glTexImage2D(GLenum target, GLint level, GLint ifmt,
							 GLsizei w, GLsizei h, GLint border, GLenum fmt,
							 GLenum type, const void* px) {
	(void)ifmt, (void)w, (void)h, (void)border, (void)fmt, (void)type, (void)px;
	assert(target == GL_TEXTURE_2D && rec_bound);
	rec[rec_bound].levels_uploaded |= 1u << level;
}
void GLAPIENTRY glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w,
								GLsizei h, GLenum f, GLenum ty, const void* p) {
	(void)t, (void)l, (void)x, (void)y, (void)w, (void)h, (void)f, (void)ty,
		(void)p;
}
void GLAPIENTRY glTexParameteri(GLenum target, GLenum pname, GLint v) {
	assert(target == GL_TEXTURE_2D && rec_bound);
	struct rec_tex* r = &rec[rec_bound];
	switch(pname) {
		case GL_TEXTURE_MIN_FILTER: r->min_filter = v; break;
		case GL_TEXTURE_MAG_FILTER: r->mag_filter = v; break;
		case GL_TEXTURE_BASE_LEVEL: r->base_level = v; break;
		case GL_TEXTURE_MAX_LEVEL: r->max_level = v; break;
		default: break;
	}
}
void GLAPIENTRY glPixelStorei(GLenum p, GLint v) {
	(void)p, (void)v;
}
GLenum GLAPIENTRY glGetError(void) {
	return GL_NO_ERROR;
}

/* ---- helpers ---------------------------------------------------------- */
static uint8_t* make_img(size_t side) {
	uint8_t* img = malloc(side * side * 4);
	assert(img);
	for(size_t i = 0; i < side * side * 4; i++)
		img[i] = (uint8_t)(i * 31u);
	return img;
}

static bool load_atlas(struct tex_gfx* t, size_t side) {
	struct tex_atlas_grid grid;
	assert(tex_atlas_grid_init(&grid, side, side));
	return tex_gfx_load_atlas(t, make_img(side), side, side, TEX_FMT_RGBA16,
							  &grid);
}

static bool restore_atlas(struct tex_gfx* t, size_t side) {
	return tex_gfx_restore(t, make_img(side), side * side * 4, side, side,
						   TEX_FMT_RGBA16, 1, true, true);
}

static int failures = 0;
#define CHECK(cond, ...)                                                       \
	do {                                                                       \
		if(!(cond)) {                                                          \
			printf("FAIL: " __VA_ARGS__);                                      \
			printf("\n");                                                      \
			failures++;                                                        \
		}                                                                      \
	} while(0)

static void expect_beta_nearest(const struct tex_gfx* t, const char* path) {
	int before = failures;
	CHECK(t->id && rec[t->id].live, "%s: terrain has a live GL texture", path);
	const struct rec_tex* r = &rec[t->id];
	CHECK(r->min_filter == GL_NEAREST, "%s: terrain MIN 0x%x, want GL_NEAREST",
		  path, (unsigned)r->min_filter);
	CHECK(r->mag_filter == GL_NEAREST, "%s: terrain MAG 0x%x, want GL_NEAREST",
		  path, (unsigned)r->mag_filter);
	CHECK(r->base_level == 0, "%s: terrain BASE_LEVEL %d, want 0", path,
		  r->base_level);
	CHECK(r->max_level == 0, "%s: terrain MAX_LEVEL %d, want 0", path,
		  r->max_level);
	CHECK(r->levels_uploaded == 1u,
		  "%s: terrain uploaded level mask 0x%x, want level 0 only", path,
		  r->levels_uploaded);
	if(failures == before)
		printf("PASS: %s terrain = NEAREST/NEAREST, level 0 only\n", path);
}

static void expect_particles_unchanged(const struct tex_gfx* t,
									   const char* path) {
	/* Today's atlas policy for a 256px non-terrain atlas: 4 LODs (0..3),
	 * trilinear min, linear mag. Unchanged by G3. */
	const struct rec_tex* r = &rec[t->id];
	int before = failures;
	CHECK(r->min_filter == GL_LINEAR_MIPMAP_LINEAR,
		  "%s: particles MIN 0x%x changed", path, (unsigned)r->min_filter);
	CHECK(r->mag_filter == GL_LINEAR, "%s: particles MAG 0x%x changed", path,
		  (unsigned)r->mag_filter);
	CHECK(r->base_level == 0 && r->max_level == 3,
		  "%s: particles levels %d..%d changed", path, r->base_level,
		  r->max_level);
	CHECK(r->levels_uploaded == 0xFu, "%s: particles level mask 0x%x changed",
		  path, r->levels_uploaded);
	if(failures == before)
		printf("PASS: %s particles atlas keeps trilinear/linear, 4 LODs\n",
			   path);
}

int main(void) {
	/* Fresh load (platform/texture.c tex_init -> tex_gfx_load_atlas). */
	assert(load_atlas(&texture_terrain, 256));
	expect_beta_nearest(&texture_terrain, "fresh load");

	/* Pack reload: re-entrant load replaces the GL object (512px pack). */
	GLuint old_id = texture_terrain.id;
	assert(load_atlas(&texture_terrain, 512));
	CHECK(texture_terrain.id != old_id && !rec[old_id].live,
		  "pack reload: old terrain GL object replaced");
	expect_beta_nearest(&texture_terrain, "pack reload");

	/* Pack-cache restore (texture_cache.c -> tex_gfx_restore, atlas=true). */
	assert(restore_atlas(&texture_terrain, 256));
	expect_beta_nearest(&texture_terrain, "cache restore");
	/* Cache record flags/lods stay as before so the cache format is unchanged. */
	CHECK(texture_terrain.lods == 1 && texture_terrain.linear,
		  "terrain cache metadata (lods=1, linear=1) unchanged");

	/* Non-terrain: particles atlas keeps today's filtering on load + restore. */
	assert(load_atlas(&texture_particles, 256));
	expect_particles_unchanged(&texture_particles, "fresh load");
	assert(restore_atlas(&texture_particles, 256));
	expect_particles_unchanged(&texture_particles, "cache restore");

	/* Non-terrain plain loads (GUI/font/items/entities) keep their flag. */
	struct tex_gfx lin = {0}, near = {0};
	assert(tex_gfx_load(&lin, make_img(16), 16, 16, TEX_FMT_RGBA16, true));
	assert(tex_gfx_load(&near, make_img(16), 16, 16, TEX_FMT_RGBA16, false));
	CHECK(rec[lin.id].min_filter == GL_LINEAR
			  && rec[lin.id].mag_filter == GL_LINEAR,
		  "linear plain texture keeps GL_LINEAR");
	CHECK(rec[near.id].min_filter == GL_NEAREST
			  && rec[near.id].mag_filter == GL_NEAREST,
		  "nearest plain texture keeps GL_NEAREST");

	tex_gfx_clear(&lin);
	tex_gfx_clear(&near);
	tex_gfx_clear(&texture_terrain);
	tex_gfx_clear(&texture_particles);

	if(failures) {
		printf("test_grass_filter: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_grass_filter: PASS\n");
	return 0;
}
