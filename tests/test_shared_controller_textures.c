#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "game/game_state.h"
#include "platform/texture.h"
#include "lodepng/lodepng.h"
#include "support/test_fixture_dir.h"

struct game_state gstate;

static void check_shared(const char* name) {
	char path[512];
	snprintf(path, sizeof(path), "assets/%s", name);
	unsigned char* expected = NULL;
	unsigned ew, eh;
	assert(!lodepng_decode32_file(&expected, &ew, &eh, path));
	size_t w, h;
	unsigned char* actual = tex_read_gfx(name, &w, &h, TEX_FMT_RGBA16);
	assert(actual && w == ew && h == eh);
	assert(!memcmp(actual, expected, w * h * 4));
	free(actual);
	free(expected);
}

int main(int argc, char** argv) {
	assert(config_create(&gstate.config_user, "config_pc.json"));
	char fixture[128], gui[160], path[192];
	test_fixture_dir("test_shared_controller_textures", fixture, sizeof(fixture));
#ifdef _WIN32
	assert(!_mkdir(fixture));
	snprintf(gui, sizeof(gui), "%s/gui", fixture);
	assert(!_mkdir(gui));
#else
	assert(!mkdir(fixture, 0755));
	snprintf(gui, sizeof(gui), "%s/gui", fixture);
	assert(!mkdir(gui, 0755));
#endif
	/* Deliberately valid but obsolete half-height button sheets. */
	static unsigned char pixels[256 * 256 * 4];
	memset(pixels, 0xFF, sizeof(pixels));
	const char* names[] = {"controls.png", "gui/approved-controls.png"};
	for(size_t i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s/%s", fixture, names[i]);
		assert(!lodepng_encode32_file(path, pixels, 256, 128));
	}
	snprintf(path, sizeof(path), "%s/pack-art.png", fixture);
	assert(!lodepng_encode32_file(path, pixels, 8, 8));
	assert(config_set_string(&gstate.config_user, "paths.texturepack", fixture));
	for(size_t i = 0; i < 2; i++)
		check_shared(names[i]);
	/* A correctly sized blank replacement must not erase the icons either. */
	memset(pixels, 0, sizeof(pixels));
	for(size_t i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s/%s", fixture, names[i]);
		assert(!lodepng_encode32_file(path, pixels, 256, 256));
		check_shared(names[i]);
		assert(!remove(path));
		check_shared(names[i]);
	}
	memset(pixels, 0xFF, sizeof(pixels));
	size_t w, h;
	unsigned char* art = tex_read_gfx("pack-art.png", &w, &h, TEX_FMT_RGBA16);
	assert(art && w == 8 && h == 8 && !memcmp(art, pixels, 8 * 8 * 4));
	free(art);
	puts("PASS: stale, blank and missing pack button sheets retain bundled pixels; ordinary pack artwork retained");
	/* Optional real pack paths also exercise switching repeatedly. */
	for(int round = 0; round < 2; round++)
		for(int i = 1; i < argc; i++) {
			assert(config_set_string(&gstate.config_user, "paths.texturepack", argv[i]));
			for(size_t j = 0; j < 2; j++)
				check_shared(names[j]);
			printf("PASS: both controller sheets byte-identical for %s\n", argv[i]);
		}
	test_fixture_dir_cleanup(fixture);
	config_destroy(&gstate.config_user);
	return 0;
}
