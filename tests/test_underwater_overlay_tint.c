/* Beta 1.7.3: ItemRenderer.renderWarpedTextureOverlay and
 * Entity.getEntityBrightness -> World.getLightBrightness. */
#include "../source/light_brightness.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Caller wiring guard (Fable G1 review, LOW): the goldens above prove the
 * shared helper; these prove each render caller passes the right arguments
 * INSIDE its render function. Whitespace-normalised source match, read from
 * the source root (WORKING_DIRECTORY). */
static char* slurp_norm(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f) {
		fprintf(stderr, "cannot open %s (WORKING_DIRECTORY = repo root?)\n", path);
		assert(0);
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* raw = malloc((size_t)n + 1);
	char* out = malloc((size_t)n + 1);
	assert(raw && out && fread(raw, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	size_t k = 0;
	for(long i = 0; i < n; i++) {
		if(isspace((unsigned char)raw[i])) {
			if(k && out[k - 1] != ' ')
				out[k++] = ' ';
		} else {
			out[k++] = raw[i];
		}
	}
	out[k] = 0;
	free(raw);
	return out;
}

/* `call` must appear inside `fn`'s body: after its definition and before the
 * next top-level definition (a normalised "} <type> " head). */
static void check_wiring(const char* path, const char* fn, const char* call,
						 const char* label) {
	char* src = slurp_norm(path);
	const char* start = strstr(src, fn);
	assert(start && "render function not found");
	const char* hit = strstr(start, call);
	const char* next_fn = NULL;
	static const char* const heads[] = {"} static ", "} void ", "} bool ",
		"} int ", "} size_t ", "} struct "};
	for(size_t i = 0; i < sizeof heads / sizeof heads[0]; i++) {
		const char* h = strstr(start + strlen(fn), heads[i]);
		if(h && (!next_fn || h < next_fn))
			next_fn = h;
	}
	if(!hit || (next_fn && hit > next_fn)) {
		fprintf(stderr, "%s: %s does not call %s\n", label, fn, call);
		assert(0);
	}
	printf("  wiring %-14s %s\n", label, call);
	free(src);
}

static void check(float pos_y, float height, float y_offset,
			  int expected_y, int sky, int torch, int sky_subtract,
			  int expected_rgb) {
	assert(underwater_overlay_sample_y(pos_y, height, y_offset)
		   == expected_y);
	struct underwater_overlay_color color = underwater_overlay_tint(
		sky, torch, sky_subtract, light_lookup_overworld);
	assert(color.r == expected_rgb);
	assert(color.g == expected_rgb && color.b == expected_rgb);
	assert(color.a == 0x80);
}

int main(void) {
	/* [Beta Entity.java:587-591] Mob/remote feet, local split-partner eye.
	 * These straddle the next block's floor boundary, including negative Y. */
	assert(underwater_overlay_sample_y(63.811F, 1.8F, 0.0F) == 64);
	assert(underwater_overlay_sample_y(63.813F, 1.8F, 0.0F) == 65);
	assert(underwater_overlay_sample_y(-1.190F, 1.8F, 0.0F) == -1);
	assert(underwater_overlay_sample_y(64.62F, 1.8F, 1.62F) == 64);
	assert(underwater_overlay_sample_y(65.62F, 1.8F, 1.62F) == 65);
	assert(underwater_overlay_sample_y(64.50F, 0.9F, 0.0F) == 65);
	assert(underwater_overlay_sample_y(64.40F, 0.9F, 0.0F) == 64);
	/* Fable U1 review: the LOCAL player's pos[1] is the EYE (EntityPlayer.java:44
	 * yOffset = 1.62F), so the wiring must subtract 1.62F. Flooded 2-high tunnel:
	 * feet 60.0 -> eye 61.62, Beta samples floor(60 + 1.8*0.66) = floor(61.188) = 61
	 * (the water cell), never 62 (the stone ceiling the old y_offset=0 wiring hit). */
	assert(underwater_overlay_sample_y_local_player(61.62F, 1.8F) == 61);
	assert(underwater_overlay_sample_y_local_player(64.62F, 1.8F) == 64);
	assert(underwater_overlay_sample_y(61.62F, 1.8F, 0.0F) == 62); /* old bug shape */
	/* 0.66 of the 1.8-block player box: feet at 63, sample at 64. */
	check(64.62F, 1.8F, 1.62F, 64, 15, 0, 0, 255);
	check(64.62F, 1.8F, 1.62F, 64, 0, 0, 0, 13);
	check(64.62F, 1.8F, 1.62F, 64, 0, 14, 0, 201);
	check(64.62F, 1.8F, 1.62F, 64, 15, 0, 11, 33);
	check(-0.38F, 1.8F, 1.62F, -1, 0, 0, 0, 13);
	/* Entity.java:587-591 per caller: mob and remote player pos[1] = feet
	 * (y_offset 0 in the engine); the split partner's pos[1] = eye, 1.62F. */
	check_wiring("source/entity/entity_monster.c",
		"static void entity_render(struct entity* e, mat4 view, float td) {",
		"underwater_overlay_sample_y(e->pos[1], e->height, e->y_offset)", "mob");
	check_wiring("source/entity/entity_remote_player.c",
		"entity_remote_player_render(",
		"underwater_overlay_sample_y( e->pos[1], e->height, e->y_offset)",
		"remote player");
	check_wiring("source/entity/entity_remote_player.c",
		"void entity_local_player_render_other(",
		"underwater_overlay_sample_y(e->pos[1], e->height, PLAYER_EYE_HEIGHT)",
		"split partner");
	puts("PASS underwater overlay Beta brightness golden");
	return 0;
}
