/* next-gaps-0924/R1b golden: the hurt/death wash colour and alpha.
 *
 * Beta 1.7.3 RenderLiving.doRenderLiving (RenderLiving.java:64-75):
 *   b = the entity's getEntityBrightness(partialTick);
 *   if(... || hurtTime > 0 || deathTime > 0) {
 *     glDisable(TEXTURE_2D); glDisable(ALPHA_TEST); glEnable(BLEND);
 *     glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthFunc(GL_EQUAL);
 *     if(hurtTime > 0 || deathTime > 0) { glColor4f(b, 0.0F, 0.0F, 0.4F); model.render(...); }
 * so every model pixel becomes 0.4 * (b, 0, 0) + 0.6 * dst. b is the SAME
 * brightness RenderManager lit the model with; CaveWii's model is lit by the
 * lightmap entry of its packed light byte, read back by light_brightness_current.
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "entity/combat.h"
#include "light_brightness.h"

static int failures;
static void close_to(float got, float want, const char* what) {
	if(fabsf(got - want) > 1e-6f) {
		printf("FAIL %s: got %.6f want %.6f\n", what, (double)got, (double)want);
		failures++;
	}
}

static void check_wash(float brightness, const char* label) {
	struct hurt_wash w = hurt_wash_for_brightness(brightness);
	char what[96];
	snprintf(what, sizeof what, "%s wash R (= brightness)", label);
	close_to(w.r, brightness, what);
	snprintf(what, sizeof what, "%s wash G", label);
	close_to(w.g, 0.0f, what);
	snprintf(what, sizeof what, "%s wash B", label);
	close_to(w.b, 0.0f, what);
	snprintf(what, sizeof what, "%s wash alpha", label);
	close_to(w.amount, 0.4f, what);
}

int main(void) {
	/* 1. Wash colour/alpha for the two brief cases: full-bright and dark. */
	check_wash(1.0f, "full-bright 1.0");
	check_wash(0.2f, "dark 0.2");

	/* 2. The brightness the renderer passes is the model's own lightmap entry.
	      Before any lightmap is built there is nothing drawn; the default is 1. */
	close_to(light_brightness_current(0), 1.0f, "unbuilt lightmap default");
	static const float suns[] = {1.0f, 0.5f, 0.25f, 0.0f};
	for(unsigned n = 0; n < sizeof suns / sizeof suns[0]; n++) {
		for(int dim = 0; dim < 2; dim++) {
			const float* lookup = dim ? light_lookup_nether : light_lookup_overworld;
			float lm[256];
			gfx_build_lightmap(lm, suns[n], 0.0f, 0.0f, lookup);
			for(int packed = 0; packed < 256; packed++) {
				if(light_brightness_current((uint8_t)packed) != lm[packed]) {
					printf("FAIL lightmap readback sun=%.2f dim=%d packed=%d: %.4f != %.4f\n",
						   (double)suns[n], dim, packed,
						   (double)light_brightness_current((uint8_t)packed),
						   (double)lm[packed]);
					failures++;
				}
			}
		}
	}
	/* Night overworld, sky 15 / torch 0: level 15 - 11 = 4 -> 0.129 (dark wash). */
	float lm[256];
	gfx_build_lightmap(lm, 0.0f, 0.0f, 0.0f, light_lookup_overworld);
	float night = light_brightness_current((uint8_t)((0 << 4) | 15));
	close_to(night, 0.129f, "night sky-15 brightness");
	check_wash(night, "night sky-15");
	gfx_build_lightmap(lm, 1.0f, 0.0f, 0.0f, light_lookup_overworld);
	check_wash(light_brightness_current((uint8_t)((0 << 4) | 15)), "noon sky-15");

	/* 3. gfx_entity_tint lerps (1-t)*dst + t*tint. With t = alpha that is exactly
	      Beta's SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend of (b,0,0,0.4) over dst. */
	static const float dsts[][3] = {{1, 1, 1}, {0.5f, 0.25f, 0.75f}, {0, 0, 0}, {0.129f, 0.2f, 0.9f}};
	static const float bs[] = {1.0f, 0.2f, 0.05f};
	for(unsigned i = 0; i < sizeof dsts / sizeof dsts[0]; i++) {
		for(unsigned j = 0; j < sizeof bs / sizeof bs[0]; j++) {
			struct hurt_wash w = hurt_wash_for_brightness(bs[j]);
			const float src[3] = {w.r, w.g, w.b};
			const float beta_src[3] = {bs[j], 0.0f, 0.0f};
			for(int c = 0; c < 3; c++) {
				float lerp = (1.0f - w.amount) * dsts[i][c] + w.amount * src[c];
				float beta = 0.4f * beta_src[c] + (1.0f - 0.4f) * dsts[i][c];
				close_to(lerp, beta, "lerp == Beta SRC_ALPHA/ONE_MINUS_SRC_ALPHA");
			}
		}
	}
	/* GX quantizes the amount into KCOLOR0.a as (uint8_t)(t*255+0.5): 0.4 -> 102,
	   and 102/255 is 0.4 -- no rounding drift in the lerp factor. The tint red byte
	   (uint8_t)(b*255+0.5) equals the model's vertex gray roundf(b*255) for every
	   lightmap brightness (wii/gfx.c gfx_update_light). */
	{
		struct hurt_wash w = hurt_wash_for_brightness(1.0f);
		uint8_t t8 = (uint8_t)(w.amount * 255.0f + 0.5f);
		if(t8 != 102) {
			printf("FAIL GX alpha byte %u want 102\n", t8);
			failures++;
		}
		for(int dim = 0; dim < 2; dim++)
			for(int l = 0; l < 16; l++) {
				float b = (dim ? light_lookup_nether : light_lookup_overworld)[l];
				struct hurt_wash wl = hurt_wash_for_brightness(b);
				uint8_t red = (uint8_t)(wl.r * 255.0f + 0.5f);
				uint8_t gray = (uint8_t)roundf(b * 255.0f);
				if(red != gray) {
					printf("FAIL GX red byte %u != vertex gray %u (dim %d level %d)\n",
						   red, gray, dim, l);
					failures++;
				}
			}
	}

	if(failures) {
		printf("test_hurt_wash_parity: %d FAILED\n", failures);
		return 1;
	}
	puts("test_hurt_wash_parity: PASS");
	return 0;
}
