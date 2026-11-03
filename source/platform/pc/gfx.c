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

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <assert.h>
#include <m-lib/m-string.h>
#include <malloc.h>
#include <math.h>
#include <string.h>

#include "../../game/game_state.h"
#include "../../graphics/texture_atlas.h"
#include "../../light_brightness.h"
#include "../../graphics/gfx_settings.h"
#include "../../lodepng/lodepng.h"
#include "../../util.h"
#include "../gfx.h"
#include "../input.h"
#include "../texture.h"

// CR-07 (docs/audit_2026-08-20_code_review.md): this used to only PRINT the
// info log on a compile failure and then delete the shader object -- the
// caller had no way to know compilation failed, and went on to attach the
// now-deleted shader to a program and link anyway. Returns false (and names
// the failing shader in the message, per the audit) so the caller can refuse
// to stand up a broken program instead of continuing with undefined output.
static bool shader_error(GLuint shader, const char* name) {
	GLint is_compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &is_compiled);

	if(!is_compiled) {
		GLint length = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);

		char log[length > 0 ? length : 1];
		if(length > 0)
			glGetShaderInfoLog(shader, length, &length, log);
		fprintf(stderr, "[gfx] shader compile failed (%s):\n%s\n", name,
				length > 0 ? log : "(no info log)");

		glDeleteShader(shader);
		return false;
	}
	return true;
}

// CR-07: returns false (and leaves *out_program untouched) on ANY failure --
// a shader that failed to compile, or a program that failed to link -- so
// gfx_setup() can abort startup instead of glUseProgram()-ing a broken or
// unlinked program and rendering undefined output while claiming success.
static bool create_shader(const char* vertex, const char* fragment,
						  GLuint* out_program) {
	GLuint v = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(v, 1, (const GLchar* const*)&vertex, NULL);
	glCompileShader(v);
	bool v_ok = shader_error(v, "vertex.shader");

	GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(f, 1, (const GLchar* const*)&fragment, NULL);
	glCompileShader(f);
	bool f_ok = shader_error(f, "fragment.shader");

	if(!v_ok || !f_ok) {
		// shader_error() already deleted whichever shader(s) failed to
		// compile; clean up the surviving one too rather than linking a
		// program against a compiled-but-orphaned shader object.
		if(v_ok)
			glDeleteShader(v);
		if(f_ok)
			glDeleteShader(f);
		return false;
	}

	GLuint program = glCreateProgram();
	glAttachShader(program, v);
	glAttachShader(program, f);

	glBindAttribLocation(program, 0, "a_pos");
	glBindAttribLocation(program, 1, "a_color");
	glBindAttribLocation(program, 2, "a_texcoord");
	glBindAttribLocation(program, 3, "a_light");

	glLinkProgram(program);

	// CR-07: linking was never checked -- a bad varying/attribute mismatch
	// (or a driver rejecting the combination) used to leave `shader_prog` set
	// to an unlinked program that GL happily "uses" and draws nothing/garbage
	// with, while gfx_setup() still returned true.
	GLint link_status = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &link_status);
	if(!link_status) {
		GLint length = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);

		char log[length > 0 ? length : 1];
		if(length > 0)
			glGetProgramInfoLog(program, length, &length, log);
		fprintf(stderr, "[gfx] shader program link failed:\n%s\n",
				length > 0 ? log : "(no info log)");

		glDeleteProgram(program);
		glDeleteShader(v);
		glDeleteShader(f);
		return false;
	}

	*out_program = program;
	return true;
}

static int window_width = GFX_PC_WINDOW_WIDTH;
static int window_height = GFX_PC_WINDOW_HEIGHT;
GLFWwindow* window;

int gfx_width() {
	return window_width;
}

int gfx_height() {
	return window_height;
}

static void framebuffer_size_callback(GLFWwindow* window, int width,
									  int height) {
	window_width = width;
	window_height = height;
	glViewport(0, 0, gfx_width(), gfx_height());
}

static void scroll_callback(GLFWwindow* window, double xoffset,
							double yoffset) {
	// TODO: buttons are not released
	if(glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED) {
		if(yoffset > 0) {
			// input_set_status(IB_SCROLL_LEFT, GLFW_PRESS);
		} else if(yoffset < 0) {
			// input_set_status(IB_SCROLL_RIGHT, GLFW_PRESS);
		}
	}
}

static GLuint shader_prog;

// Engine shaders ship in resources/. A texture pack MAY override them, so try
// the active pack dir first, then fall back to resources/ — mirroring the
// texture loader's assets/ fallback, so a texture pack need not bundle shaders.
// Without this, selecting any pack other than one containing the shaders (e.g.
// an authentic-textures pack) aborted gfx_setup on the shader-load assert.
static void* gfx_read_shader(const char* name) {
	const char* pack
		= config_read_path(&gstate.config_user, "paths.texturepack", "assets");
	string_t p;
	string_init(p);
	string_printf(p, "%s/%s", pack, name);
	void* data = file_read(string_get_cstr(p));
	if(!data) {
		string_printf(p, "resources/%s", name);
		data = file_read(string_get_cstr(p));
	}
	string_clear(p);
	return data;
}

bool gfx_setup() {
	if(!glfwInit()) {
		fprintf(stderr, "[gfx] glfwInit failed\n");
		return false;
	}

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	// Lock the window to the Wii's native 640x480 render resolution: the render
	// target tracks the window size (framebuffer_size_callback), so a resizable
	// window let it be blown up to 1080p and over-render. Non-resizable keeps
	// the PC dev build representative of actual Wii output.
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	/* Unattended PC feasibility runs still execute the real OpenGL mesh path.
	 */
	const char* headless = getenv("CAVEWII_HEADLESS");
	if(headless && strcmp(headless, "1") == 0)
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	/* Explicit PC proof viewport; normal play retains native 640x480. */
	if(config_read_int(&gstate.config_user, "debug.capture_height", 480) == 240)
		window_height = 240;
	/* The player-facing name; GAME_NAME stays the internal build marker. */
	window = glfwCreateWindow(window_width, window_height, "CavEX: ReCrafted",
							  NULL, NULL);
	if(!window) {
		fprintf(stderr, "[gfx] glfwCreateWindow failed\n");
		glfwTerminate();
		return false;
	}
	glfwMakeContextCurrent(window);

	glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
	// glfwSetScrollCallback(window, scroll_callback);

	if(glfwRawMouseMotionSupported())
		glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);

	glfwSetInputMode(window, GLFW_STICKY_KEYS, GLFW_TRUE);

	if(glewInit()) {
		fprintf(stderr, "[gfx] could not load extended OpenGL functions\n");
		glfwDestroyWindow(window);
		window = NULL;
		glfwTerminate();
		return false;
	}

	printf("Vendor: %s\n", glGetString(GL_VENDOR));
	printf("Renderer: %s\n", glGetString(GL_RENDERER));
	printf("Version: %s\n", glGetString(GL_VERSION));

	void* vertex = gfx_read_shader("vertex.shader");
	assert(vertex);
	// CR-07 (docs/audit_2026-08-20_code_review.md): assert() alone is
	// compiled out under NDEBUG in the app build (see CMakeLists' `-UNDEBUG`
	// note, which applies only to TEST targets), so a missing/short-read
	// shader file used to fall straight through into create_shader() with a
	// NULL vertex source. Keep the assert (fast signal in debug/test builds)
	// but do not rely on it -- fail startup for real here.
	if(!vertex) {
		fprintf(stderr,
				"[gfx] failed to read vertex shader (vertex.shader)\n");
		return false;
	}
	void* fragment = gfx_read_shader("fragment.shader");
	assert(fragment);
	if(!fragment) {
		fprintf(stderr,
				"[gfx] failed to read fragment shader (fragment.shader)\n");
		free(vertex);
		return false;
	}

	GLuint program = 0;
	bool shader_ok = create_shader(vertex, fragment, &program);
	free(vertex);
	free(fragment);
	if(!shader_ok) {
		// create_shader()/shader_error() already logged the specific compile
		// or link failure above; refuse to continue startup with no usable
		// program rather than gfx_setup() reporting success anyway.
		fprintf(stderr, "[gfx] shader program setup failed; aborting startup\n");
		return false;
	}
	shader_prog = program;
	glUseProgram(shader_prog);

	// Default a_color (attribute 1) to white so lit geometry that doesn't bind a
	// per-vertex colour (normal untinted blocks, item icons) leaves the new
	// lighting*a_color tint multiply a no-op. Tinted chunk lists bind their own
	// a_color array; immediate draws set + clear it around their own draw.
	glVertexAttrib4f(1, 1.0f, 1.0f, 1.0f, 1.0f);

	// Default the audit #17f emissive-tint uniforms OFF so all geometry sharing this
	// program (blocks, items, GUI) is untinted until an entity draw opts in. amount=0
	// makes the fragment-shader mix() a no-op; colour defaults to white but is unused
	// while amount==0. Mirrors the a_color=white default above.
	glUniform3f(glGetUniformLocation(shader_prog, "tint_color"), 1.0f, 1.0f, 1.0f);
	glUniform1f(glGetUniformLocation(shader_prog, "tint_amount"), 0.0f);
	// Per-channel multiply overlay (fleece colour) defaults to identity (1,1,1) -> no-op.
	glUniform3f(glGetUniformLocation(shader_prog, "tint_modulate"), 1.0f, 1.0f, 1.0f);

	gfx_clear_buffers(255, 255, 255);
	gfx_texture(true);
	gfx_alpha_test(true);

	glCullFace(GL_BACK);
	glFrontFace(GL_CW);
	#ifdef GFX_WIREFRAME
	glPolygonMode(GL_FRONT, GL_LINE);
	glPolygonMode(GL_BACK, GL_LINE);
	#endif
	gfx_cull_func(MODE_BACK);

	gfx_depth_func(MODE_LEQUAL);

	glViewport(0, 0, gfx_width(), gfx_height());

	if(!tex_init())
		return false;
	gfx_bind_texture(&texture_terrain);

	glUniform1i(glGetUniformLocation(shader_prog, "tex"), 0);
	return true;
}

static float colors[256];

void gfx_update_light(float daytime, float rain_strength, float thunder_strength,
					  const float* light_lookup) {
	assert(daytime > -GLM_FLT_EPSILON && daytime < 1.0F + GLM_FLT_EPSILON
		   && light_lookup);
	assert(rain_strength >= 0.0F && rain_strength <= 1.0F);
	assert(thunder_strength >= 0.0F && thunder_strength <= 1.0F);

	// Beta-faithful lightmap (shared with wii/gfx.c so the two cannot drift, and
	// directly pinned by tests/test_daylight_curve.c): integer sky-subtract +
	// max(blockLight, skyLight - subtract) + single lightBrightnessTable lookup. The
	// old `max(table[torch], table[sky]*daytime)` continuous multiply read too bright
	// at dawn/dusk and crushed moonlit night to pure black. Rain/thunder add to the
	// sky-subtract, so a storm dims the whole world (Beta calculateSkylightSubtracted).
	gfx_build_lightmap(colors, daytime, rain_strength, thunder_strength,
					   light_lookup);

	glUniform1fv(glGetUniformLocation(shader_prog, "lighting"), 256, colors);
}

float gfx_lookup_light(uint8_t light) {
	return colors[light];
}

void gfx_clear_buffers(uint8_t r, uint8_t g, uint8_t b) {
	glClearColor(r / 255.0F, g / 255.0F, b / 255.0F, 1.0F);
}

void gfx_finish(bool vsync) {
	glfwSwapBuffers(window);
	gfx_write_buffers(true, true, true);
	glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
	glfwPollEvents();
}

struct gfx_wait_timing gfx_frame_wait_timing(bool enabled) {
	(void)enabled;
	return (struct gfx_wait_timing){0};
}

void gfx_flip_buffers(float* gpu_wait, float* vsync_wait) {
	*gpu_wait = 0;
	*vsync_wait = 0;
}

void gfx_wait_idle(void) {
	if(window) glFinish();
}

bool gfx_can_present(void) {
	return window != NULL;
}

void gfx_wait_vsync(void) {
}

void gfx_bind_texture(struct tex_gfx* tex) {
	tex_gfx_bind(tex, 0);
}

void gfx_copy_framebuffer(uint8_t* dest, size_t* width, size_t* height) {
	assert(width && height);

	*width = gfx_width();
	*height = gfx_height();

	if(!dest)
		return;

	void* tmp = malloc(*width * 4);

	if(!tmp)
		return;

	glReadPixels(0, 0, *width, *height, GL_RGBA, GL_UNSIGNED_BYTE, dest);

	// flip image
	for(size_t y = 0; y < *height / 2; y++) {
		memcpy(tmp, dest + y * (*width) * 4, *width * 4);
		memcpy(dest + y * (*width) * 4, dest + (*height - 1 - y) * (*width) * 4,
			   *width * 4);
		memcpy(dest + (*height - 1 - y) * (*width) * 4, tmp, *width * 4);
	}

	free(tmp);
}

bool gfx_capture_frame_rows(gfx_capture_rows_fn sink, void* user) {
	enum { BAND_ROWS = 32 };
	size_t w = (size_t)gfx_width(), h = (size_t)gfx_height();
	bool ok = sink && w > 0 && h > 0;
	uint8_t* band = ok ? malloc(w * BAND_ROWS * 4) : NULL;
	uint8_t* row = ok ? malloc(w * 4) : NULL;
	if(!band || !row) ok = false;
	if(ok) {
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		for(size_t y0 = 0; ok && y0 < h; y0 += BAND_ROWS) {
			size_t rows = h - y0 < BAND_ROWS ? h - y0 : BAND_ROWS;
			/* GL is bottom-origin: this band's lowest row is h - y0 - rows. */
			glReadPixels(0, (GLint)(h - y0 - rows), (GLsizei)w, (GLsizei)rows,
						 GL_RGBA, GL_UNSIGNED_BYTE, band);
			if(glGetError() != GL_NO_ERROR) {
				ok = false;
				break;
			}
			for(size_t r = 0; r < rows / 2; r++) {
				memcpy(row, band + r * w * 4, w * 4);
				memcpy(band + r * w * 4, band + (rows - 1 - r) * w * 4, w * 4);
				memcpy(band + (rows - 1 - r) * w * 4, row, w * 4);
			}
			ok = sink(user, band, y0, rows, w, h);
		}
	}
	free(row);
	free(band);
	/* Hand the frame back clean: no scissor, every buffer writable. */
	gfx_scissor(false, 0, 0, 0, 0);
	gfx_write_buffers(true, true, true);
	glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
	return ok;
}

void gfx_mode_world() {
	gfx_write_buffers(true, true, true);
	gfx_matrix_texture(false, NULL);
}

void gfx_mode_gui() {
	gfx_mode_gui_viewport(gfx_width(), gfx_height());
}

void gfx_mode_gui_viewport(int width, int height) {
	gfx_fog(false);

	mat4 proj;
	glm_ortho(0, width, height, 0, -256, 256, proj);
	gfx_matrix_projection(proj, false);
	gfx_matrix_modelview(GLM_MAT4_IDENTITY);

	gfx_lighting(false);
	gfx_blending(MODE_BLEND);
	gfx_alpha_test(true);
	gfx_write_buffers(true, false, false);
}

void gfx_matrix_projection(mat4 proj, bool is_perspective) {
	assert(proj);
	glUniformMatrix4fv(glGetUniformLocation(shader_prog, "proj"), 1, GL_FALSE,
					   (float*)proj);
}

void gfx_matrix_modelview(mat4 mv) {
	assert(mv);
	glUniformMatrix4fv(glGetUniformLocation(shader_prog, "mv"), 1, GL_FALSE,
					   (float*)mv);
}

void gfx_matrix_texture(bool enable, mat4 tex) {
	if(enable) {
		assert(tex);
		glUniformMatrix4fv(glGetUniformLocation(shader_prog, "texm"), 1,
						   GL_FALSE, (float*)tex);
	} else {
		glUniformMatrix4fv(glGetUniformLocation(shader_prog, "texm"), 1,
						   GL_FALSE, (float*)GLM_MAT4_IDENTITY);
	}
}

void gfx_fog_color(uint8_t r, uint8_t g, uint8_t b) {
	glUniform3f(glGetUniformLocation(shader_prog, "fog_color"), r / 255.0F,
				g / 255.0F, b / 255.0F);
}

void gfx_fog_pos(float dx, float dz, float distance) {
	assert(distance > 0);

	glUniform2f(glGetUniformLocation(shader_prog, "fog_delta"), dx, dz);
	glUniform1f(glGetUniformLocation(shader_prog, "fog_distance"), distance);
}

void gfx_fog(bool enable) {
	glUniform1i(glGetUniformLocation(shader_prog, "enable_fog"), enable);
}

void gfx_tint(bool enable) {
	// PC applies the biome tint per display list (a_color attribute + the
	// lighting*a_color shader multiply), so there is no global state to toggle.
	(void)enable;
}

// audit #17f: emissive colour-tint overlay for entity draws. Sets the fragment
// shader's tint uniforms; the shader lerps the lit/textured colour toward
// (r,g,b) by `amount`. amount==0 leaves the model exactly as today (the mix() is a
// no-op), and render_entity.c calls this with amount 0 right after each tinted mob
// so no other entity/world/GUI draw is tinted. r,g,b are 0..1.
//
// DIRTY-GUARD: amount is clamped to [0,1]; when it resolves to 0 (the common
// untinted path -- every non-flashing mob, plus the mandatory reset call after an
// already-untinted draw) and the uniform is ALREADY 0, early-out and upload nothing.
// This mirrors the GX backend's gfx_entity_tint_active guard so the untinted path is
// a true no-op on both backends rather than re-uploading an unchanged uniform per mob.
void gfx_entity_tint(float r, float g, float b, float amount) {
	static float tint_amount_prev = 0.0f; // matches the gfx_setup default (off)
	if(amount < 0.0f)
		amount = 0.0f;
	if(amount > 1.0f)
		amount = 1.0f;
	if(amount == 0.0f && tint_amount_prev == 0.0f)
		return; // already untinted -> nothing to upload
	tint_amount_prev = amount;
	glUniform3f(glGetUniformLocation(shader_prog, "tint_color"), r, g, b);
	glUniform1f(glGetUniformLocation(shader_prog, "tint_amount"), amount);
}

// Per-channel emissive MULTIPLY for entity draws (Beta RenderSheep's glColor3f fleece tint:
// the wool is multiplied per channel by the fleece colour -> byte-exact for ALL 16 colours,
// where the tint_amount LERP can only reproduce the grayscale/neutral subset). (1,1,1) is the
// identity/reset; render_entity.c brackets the fleece wool draw with gfx_entity_modulate(fleece)
// ... gfx_entity_modulate(1,1,1). Dirty-guarded like gfx_entity_tint so the identity path
// uploads nothing. r,g,b are 0..1.
void gfx_entity_modulate(float r, float g, float b) {
	static float pr = 1.0f, pg = 1.0f, pb = 1.0f; // matches the gfx_setup default (identity)
	if(r < 0.0f) r = 0.0f;
	if(r > 1.0f) r = 1.0f;
	if(g < 0.0f) g = 0.0f;
	if(g > 1.0f) g = 1.0f;
	if(b < 0.0f) b = 0.0f;
	if(b > 1.0f) b = 1.0f;
	if(r == pr && g == pg && b == pb)
		return; // already at this modulate (incl. the common identity reset) -> nothing to upload
	pr = r; pg = g; pb = b;
	glUniform3f(glGetUniformLocation(shader_prog, "tint_modulate"), r, g, b);
}

void gfx_blending(enum gfx_blend mode) {
	switch(mode) {
		case MODE_BLEND:
			glDisable(GL_COLOR_LOGIC_OP);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			break;
		case MODE_BLEND2:
			glDisable(GL_COLOR_LOGIC_OP);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			break;
		case MODE_BLEND3:
			glDisable(GL_COLOR_LOGIC_OP);
			glEnable(GL_BLEND);
			glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
			break;
		case MODE_INVERT:
			glDisable(GL_BLEND);
			glEnable(GL_COLOR_LOGIC_OP);
			glLogicOp(GL_INVERT);
			break;
		case MODE_OFF:
			glDisable(GL_COLOR_LOGIC_OP);
			glDisable(GL_BLEND);
			break;
	}
}

// The fixed threshold every OTHER caller expects (matches fragment.shader's
// pre-render-slice literal `< 0.0625`, and wii/gfx.c's GX_GEQUAL/16 == 16/255
// ~= 0.0625). Re-asserted on every gfx_alpha_test(bool) call (not just once
// at setup) so a prior gfx_alpha_test_ref() call can never leak its ref into
// an unrelated later draw -- alpha_ref is a shader UNIFORM, so it persists
// across draws until explicitly rewritten.
#define GFX_ALPHA_REF_DEFAULT 0.0625f

void gfx_alpha_test(bool enable) {
	glUniform1i(glGetUniformLocation(shader_prog, "enable_alpha"), enable);
	glUniform1f(glGetUniformLocation(shader_prog, "alpha_ref"),
				GFX_ALPHA_REF_DEFAULT);
}

void gfx_alpha_test_ref(bool enable, float ref) {
	glUniform1i(glGetUniformLocation(shader_prog, "enable_alpha"), enable);
	glUniform1f(glGetUniformLocation(shader_prog, "alpha_ref"), ref);
}

void gfx_write_buffers(bool color, bool depth, bool depth_test) {
	glColorMask(color, color, color, color);
	glDepthMask(depth);

	if(depth_test) {
		glEnable(GL_DEPTH_TEST);
	} else {
		glDisable(GL_DEPTH_TEST);
	}
}

void gfx_set_viewport(float x, float y, float w, float h) {
	// GL keeps depth range separate from the viewport, so no caching needed
	// here (the Wii backend caches -- GX folds both into GX_SetViewport).
	glViewport((GLint)x, (GLint)(gfx_height() - y - h), (GLsizei)w, (GLsizei)h);
}

void gfx_depth_range(float near, float far) {
	glDepthRange(near, far);
}

void gfx_depth_func(enum depth_func func) {
	switch(func) {
		case MODE_LEQUAL: glDepthFunc(GL_LEQUAL); break;
		case MODE_EQUAL: glDepthFunc(GL_EQUAL); break;
	}
}

void gfx_texture(bool enable) {
	glUniform1i(glGetUniformLocation(shader_prog, "enable_texture"), enable);
}

void gfx_lighting(bool enable) {
	glUniform1i(glGetUniformLocation(shader_prog, "enable_lighting"), enable);
}

void gfx_cull_func(enum cull_func func) {
	if(func != MODE_NONE) {
		glEnable(GL_CULL_FACE);
	} else {
		glDisable(GL_CULL_FACE);
	}

	switch(func) {
		case MODE_FRONT: glCullFace(GL_FRONT); break;
		case MODE_BACK: glCullFace(GL_BACK); break;
		default:
	}
}

// gfx_scissor takes an absolute TOP-LEFT-origin framebuffer rectangle (x right,
// y DOWN from the top of the screen) -- the convention the Wii backend already
// implements and every caller already passes (widget_slotlist.c, main.c's split
// viewports). GL's glScissor is BOTTOM-origin, so this backend does the flip;
// before, y was forwarded raw and main.c compensated behind an #ifdef
// PLATFORM_PC, which left every other PC caller clipping the mirrored band.
// No viewport offset is applied: the rect is in framebuffer pixels, not
// viewport-relative ones, on both backends.
void gfx_scissor(bool enable, uint32_t x, uint32_t y, uint32_t width,
				 uint32_t height) {
	if(!enable) {
		glDisable(GL_SCISSOR_TEST);
		return;
	}

	uint32_t fb_w = (uint32_t)gfx_width();
	uint32_t fb_h = (uint32_t)gfx_height();

	// Clamp into the framebuffer in UNSIGNED space, extent last: a rect that
	// starts past an edge collapses to zero extent instead of wrapping the
	// `fb - origin` subtraction to ~4 billion, and the flip below can never
	// go negative. Cast to the signed GL types only AFTER clamping.
	uint32_t cx = x > fb_w ? fb_w : x;
	uint32_t cy = y > fb_h ? fb_h : y;
	uint32_t cw = width > fb_w - cx ? fb_w - cx : width;
	uint32_t ch = height > fb_h - cy ? fb_h - cy : height;

	glEnable(GL_SCISSOR_TEST);
	glScissor((int)cx, (int)(fb_h - cy - ch), (int)cw, (int)ch);
}

void gfx_draw_lines(size_t vertex_count, const int16_t* vertices,
					const uint8_t* colors) {
	assert(vertices && colors);
	glLineWidth(2.0F);

	assert(vertex_count < 256);

	float tmp[vertex_count * 3];
	for(size_t k = 0; k < vertex_count * 3; k++)
		tmp[k] = vertices[k] / 256.0F;

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, tmp);
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, colors);

	glDrawArrays(GL_LINES, 0, vertex_count);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
}

void gfx_draw_quads(size_t vertex_count, const int16_t* vertices,
					const uint8_t* colors, const uint16_t* texcoords) {
	assert(vertices && colors && texcoords);

	assert(vertex_count < 256);

	// texcoords hold 2 components/vertex (the attrib below is GL_FLOAT, 2). The old
	// vertex_count*3 sizing + loop bound read vertex_count u16 PAST the end of every
	// texcoord buffer (a heap/stack over-read = UB); GL only ever read the first 2*vc
	// valid slots, so output was unaffected and headless ASan (no GL window) missed it.
	// Match the Wii backend's k*2 indexing.
	float tmp[vertex_count * 2];
	for(size_t k = 0; k < vertex_count * 2; k++)
		tmp[k] = texcoords[k] / 256.0F;

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);

	glVertexAttribPointer(0, 3, GL_SHORT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, colors);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, tmp);

	glDrawArrays(GL_QUADS, 0, vertex_count);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
}

void gfx_draw_quads_flt(size_t vertex_count, const float* vertices,
						const uint8_t* colors, const float* texcoords) {
	assert(vertices && colors && texcoords);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);

	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, colors);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, texcoords);

	glDrawArrays(GL_QUADS, 0, vertex_count);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
}
