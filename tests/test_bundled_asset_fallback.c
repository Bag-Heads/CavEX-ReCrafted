/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * ci-1101/18: bundled-only art (title logo, controller sheets, pointer) and
 * the boot credits must load on a Wii that has ONLY the apps/CavEX/assets
 * copy (the release zip ships both sd:/assets and sd:/apps/CavEX/assets; a
 * player who copied only apps/ got no logo, no subtitle and garbled glyphs).
 *
 *   1. the Wii root list is {"sd:/assets", "assets"} in that order (relative
 *      "assets" = the app folder the launcher started us in);
 *   2. the PC root list is just {"assets"} (behaviour unchanged);
 *   3. the REAL resolver (PLATFORM_PC build of texture.c) still reads a
 *      bundled-only file from its bundled root when the selected pack has
 *      its own (ignored) copy -- the fallback did not loosen the policy.
 */
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

int main(void) {
	const char* roots[4] = {0};
	size_t n = tex_bundled_roots_for(true, roots, 4);
	assert(n == 2 && "Wii: sd:/assets then the app-folder assets/");
	assert(strcmp(roots[0], "sd:/assets") == 0);
	assert(strcmp(roots[1], "assets") == 0);
	n = tex_bundled_roots_for(false, roots, 4);
	assert(n == 1 && strcmp(roots[0], "assets") == 0 && "PC: bundled assets/ only");
	/* A one-slot buffer still yields the first (preferred) root. */
	n = tex_bundled_roots_for(true, roots, 1);
	assert(n == 1 && strcmp(roots[0], "sd:/assets") == 0);
	/* The platform entry point agrees with the PC list in this build. */
	n = tex_bundled_roots(roots, 4);
	assert(n == 1 && strcmp(roots[0], "assets") == 0);
	puts("PASS bundled roots: Wii {sd:/assets, assets}; PC {assets}");

	/* 3. the real resolver keeps ignoring a pack copy of a bundled-only file */
	assert(config_create(&gstate.config_user, "config_pc.json"));
	char fixture[128], gui[160], path[192];
	test_fixture_dir("test_bundled_asset_fallback", fixture, sizeof(fixture));
#ifdef _WIN32
	assert(!_mkdir(fixture));
	snprintf(gui, sizeof(gui), "%s/gui", fixture);
	assert(!_mkdir(gui));
#else
	assert(!mkdir(fixture, 0755));
	snprintf(gui, sizeof(gui), "%s/gui", fixture);
	assert(!mkdir(gui, 0755));
#endif
	static unsigned char pixels[64 * 64 * 4];
	memset(pixels, 0x7F, sizeof(pixels));
	snprintf(path, sizeof(path), "%s/gui/title-logo.png", fixture);
	assert(!lodepng_encode32_file(path, pixels, 64, 64));
	assert(config_set_string(&gstate.config_user, "paths.texturepack", fixture));
	unsigned char* expected = NULL;
	unsigned ew, eh;
	assert(!lodepng_decode32_file(&expected, &ew, &eh, "assets/gui/title-logo.png"));
	size_t w, h;
	unsigned char* actual = tex_read("gui/title-logo.png", &w, &h);
	assert(actual && w == ew && h == eh && "bundled logo not read from assets/");
	assert(!memcmp(actual, expected, w * h * 4) && "pack copy of the logo leaked");
	free(actual);
	free(expected);
	test_fixture_dir_cleanup(fixture);
	config_destroy(&gstate.config_user);
	puts("PASS bundled-only title logo still comes from the bundled root");
	return 0;
}
