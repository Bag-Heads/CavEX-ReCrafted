/* A texture-pack folder whose name has spaces ("My Pack") loads like any
 * other: listed, selectable (value compare, config save + reload), its own
 * art read through the real resolver (texture.c), raw-jar layout detected,
 * audio eligibility and the manifest display name read. ingame-import-1030 follow-up: the importer
 * now writes "Beta-1.7.3" (rig cannot stage spaced names), but players name
 * their own packs "My Pack", so the PC path handling must hold for them. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "game/game_state.h"
#include "lodepng/lodepng.h"
#include "platform/texture.h"
#include "support/test_fixture_dir.h"
#include "texturepacks.h"

#ifdef _WIN32
#include <direct.h>
#define mkdir1(p) _mkdir(p)
#else
#define mkdir1(p) mkdir((p), 0755)
#endif

struct game_state gstate;

static void png(const char* dir, const char* rel, unsigned w, unsigned h,
				uint8_t fill) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, rel);
	unsigned char* px = malloc((size_t)w * h * 4);
	memset(px, fill, (size_t)w * h * 4);
	assert(!lodepng_encode32_file(path, px, w, h));
	free(px);
}

static void text(const char* dir, const char* rel, const char* body) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, rel);
	FILE* f = fopen(path, "wb");
	assert(f);
	fputs(body, f);
	fclose(f);
}

static void expect_fill(const char* name, unsigned w, unsigned h,
						uint8_t fill) {
	size_t gw = 0, gh = 0;
	unsigned char* px = tex_read_gfx(name, &gw, &gh, TEX_FMT_RGBA32);
	assert(px && gw == w && gh == h);
	for(size_t i = 0; i < (size_t)w * h * 4; i++)
		if(px[i] != fill) {
			fprintf(stderr, "%s: byte %zu = 0x%02x, want 0x%02x\n", name,
					i, px[i], fill);
			abort();
		}
	free(px);
}

int main(void) {
	assert(config_create(&gstate.config_user, "config_pc.json"));
	char fixture[128], packs[192], pack[256], sub[320], cfg[320];
	test_fixture_dir("test_pack_path_spaces", fixture, sizeof(fixture));
	assert(!mkdir1(fixture));
	snprintf(packs, sizeof(packs), "%s/texturepacks", fixture);
	assert(!mkdir1(packs));
	snprintf(pack, sizeof(pack), "%s/My Pack", packs);
	assert(!mkdir1(pack));
	snprintf(sub, sizeof(sub), "%s/gui", pack);
	assert(!mkdir1(sub));
	snprintf(sub, sizeof(sub), "%s/sound", pack);
	assert(!mkdir1(sub));
	snprintf(sub, sizeof(sub), "%s/sound/random", pack);
	assert(!mkdir1(sub));

	png(pack, "pack-art.png", 8, 8, 0x5A);
	png(pack, "gui/container.png", 256, 256, 0xA5); /* raw jar chest name */
	text(pack, "sound/random/click.wav", "RIFF....WAVE");
	text(pack, ".cavewii-pack.json",
		 "{\n  \"files\": [],\n  \"name\": \"My Pack (shown)\",\n"
		 "  \"source\": {\"name\": \"inner\"}\n}\n");

	/* Listed under its real name. */
	string_t names[8];
	size_t n = texturepacks_list(packs, names, 8);
	bool listed = false;
	for(size_t i = 0; i < n; i++) {
		listed = listed || !strcmp(string_get_cstr(names[i]), "My Pack");
		string_clear(names[i]);
	}
	assert(listed);

	/* Value compare (relative vs SD-anchored), eligibility, display name. */
	assert(texturepacks_value_eq("texturepacks/My Pack",
								 "sd:/texturepacks/My Pack"));
	assert(!texturepacks_value_eq("texturepacks/My Pack",
								  "texturepacks/My"));
	assert(texturepacks_is_raw_jar(pack));
	assert(texturepacks_has_audio(pack));
	char shown[64] = "";
	assert(texturepacks_display_name(pack, shown, sizeof(shown)));
	assert(!strcmp(shown, "My Pack (shown)")); /* top-level name, not source's */
	assert(!texturepacks_display_name(packs, shown, sizeof(shown)));

	/* Selected via config, saved and re-read: the spaced value survives. */
	assert(config_set_string(&gstate.config_user, "paths.texturepack", pack));
	snprintf(cfg, sizeof(cfg), "%s/config.json", fixture);
	assert(config_save(&gstate.config_user, cfg));
	struct config reread;
	assert(config_create(&reread, cfg));
	assert(!strcmp(config_read_string(&reread, "paths.texturepack", ""), pack));

	/* The real resolver (pack first, bundled fallback) reads the pack's own
	 * files from the spaced folder. (The chest <- container.png alias is
	 * applied by tex_init's raw-jar path, which keys off
	 * texturepacks_is_raw_jar / tex_beta_raw_jar_layout, checked above.) */
	expect_fill("pack-art.png", 8, 8, 0x5A);
	expect_fill("gui/container.png", 256, 256, 0xA5);

	test_fixture_dir_cleanup(fixture);
	puts("PASS: pack folder \"My Pack\" lists, selects, saves, resolves art, "
		 "raw-jar layout, audio eligibility and manifest display name");
	return 0;
}
