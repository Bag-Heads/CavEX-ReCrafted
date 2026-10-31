/*
	Creative HUD hides hearts/armor/air (maintainer ruling 2026-09-03,
	docs/creative_mode.md "Creative HUD -- no hearts/armor/air").

	screen_ingame_render2D's real call site (source/game/gui/screen_ingame.c)
	is exactly:

		if(!gstate.config_creative)
			hud_survival_row_draw_owned(owner, hud_left, heart_row_y);

	This is a FOCUSED link test (build_conventions.md): it compiles
	hud_survival_row.c (the extracted hearts+armor+air row, verbatim from the
	pre-split screen_ingame_render2D block) and the real java_random.c (the
	heart-jitter rand must stay bit-exact) directly, with every leaf
	gfx/inventory symbol it calls mocked to record a trace instead of
	drawing. It does NOT link the rest of screen_ingame.c (chat/particle/
	lighting/mem2 -- unrelated to this row and not worth the link-isolation
	cost), so a static text check at the end pins the actual call site's
	source text, closing the gap between "this function draws nothing" and
	"the real game never calls it in creative".

	Cases:
	  1. QUIET (full health, no armor, dry): golden quad-count + exact
	     positions -- the survival EQUIVALENCE anchor. This is the common
	     HUD state and matches the pre-extraction code byte-for-byte (the
	     function body was moved, not rewritten).
	  2. Mixed health (full/half/empty hearts) -- overlay UV selection.
	  3. Armor: 0 -> no row; >0 -> exactly 10 quads, right-aligned.
	  4. Air: dry -> no bubbles; submerged with partial oxygen -> correct
	     full/popping counts and the row's y offset above the hearts.
	  5. Hurt blink -- white container variant + prev-health ghost overlay.
	  6. THE GATE: gstate.config_creative true -> zero draw calls, zero
	     binds; false -> the same golden trace as case 1. Uses the real
	     hud_survival_row_draw() and the real gstate.config_creative field,
	     the same two things screen_ingame.c's gate uses.
	  7. Static source-text pin on screen_ingame.c's actual call site.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "entity/entity.h" // struct entity (gstate.players[0].local_player)
#include "game/game_state.h"
#include "game/gui/hud_survival_row.h"
#include "graphics/gfx_settings.h" // GFX_GUI_SCALE
#include "item/window_container.h" // WINDOWC_INVENTORY, struct window_container
#include "network/server_interface.h" // MAX_PLAYER_HEALTH / HEALTH_PER_HEART
#include "network/server_local_state.h" // OXYGEN_THRESHOLD / MAX_OXYGEN
#include "platform/gfx.h"
#include "platform/texture.h"
#include "game/hud_hurt.h"
#include "worldgen/java_random.h"

// ---- trace of gutil_texquad calls: hud_survival_row.c's only draw primitive ----
struct quad_call {
	int x, y, tx, ty, sx, sy, w, h;
};
#define MAX_QUADS 256
static struct quad_call g_quads[MAX_QUADS];
static int g_nquads;
static int g_binds;
static struct tex_gfx* g_last_bind;

void gutil_texquad(int x, int y, int tx, int ty, int sx, int sy, int width,
				   int height) {
	assert(g_nquads < MAX_QUADS);
	g_quads[g_nquads++]
		= (struct quad_call) {x, y, tx, ty, sx, sy, width, height};
}
void gfx_bind_texture(struct tex_gfx* t) {
	g_binds++;
	g_last_bind = t;
}

static int g_armor_value;
static struct window_container g_owner_windows[2];
static struct inventory g_owner_inventory[2];
static int g_owner_armor[2];
int armor_get_total_value(const struct inventory* inventory) {
	for(int k = 0; k < 2; k++)
		if(inventory == &g_owner_inventory[k]) return g_owner_armor[k];
	return g_armor_value;
}
struct inventory* windowc_get_latest(struct window_container* wc) {
	for(int k = 0; k < 2; k++)
		if(wc == &g_owner_windows[k]) return &g_owner_inventory[k];
	return NULL;
}

// ---- link-only globals: definitions this focused build must supply itself ----
struct game_state gstate;
struct tex_gfx texture_icons;

static struct entity g_player;

static void reset_all(int health) {
	memset(g_quads, 0, sizeof g_quads);
	g_nquads = 0;
	g_binds = 0;
	g_last_bind = NULL;
	g_armor_value = 0;
	memset(&gstate, 0, sizeof gstate);
	memset(&g_player, 0, sizeof g_player);
	g_player.health = (short)health;
	gstate.players[0].local_player = &g_player;
	gstate.players[0].windows[WINDOWC_INVENTORY] = NULL;
	gstate.players[0].in_water = false;
	gstate.players[0].oxygen = MAX_OXYGEN;
}

#define HUD_LEFT 100
#define HEART_Y 400
#define SC GFX_GUI_SCALE

static void expect_quads(const char* who, int want) {
	if(g_nquads != want) {
		fprintf(stderr, "FAIL %s: %d quads, expected %d\n", who, g_nquads,
				want);
		assert(0);
	}
}

// ---- 1: QUIET, full health, no armor, dry -- the survival equivalence golden ----
static void case_quiet_full_health(void) {
	reset_all(MAX_PLAYER_HEALTH);
	hud_survival_row_draw(HUD_LEFT, HEART_Y);

	assert(g_binds == 1 && g_last_bind == &texture_icons);
	// 10 hearts * 2 quads (container + full-heart overlay), no armor, no air.
	expect_quads("quiet", (MAX_PLAYER_HEALTH / HEALTH_PER_HEART) * 2);

	for(int k = 0; k < MAX_PLAYER_HEALTH / HEALTH_PER_HEART; k++) {
		struct quad_call container = g_quads[k * 2];
		struct quad_call heart = g_quads[k * 2 + 1];
		int hx = HUD_LEFT + k * 8 * SC;
		assert(container.x == hx && container.y == HEART_Y);
		assert(container.tx == 16 && container.ty == 0); // not blinking
		assert(container.sx == 9 && container.sy == 9);
		assert(container.w == 9 * SC && container.h == 9 * SC);
		assert(heart.x == hx && heart.y == HEART_Y);
		assert(heart.tx == 52 && heart.ty == 0); // full heart
	}
	printf("  [1] quiet full-health golden: OK (%d quads)\n", g_nquads);
}

// ---- 2: mixed health -- full / half / empty hearts ----
static void case_mixed_health(void) {
	// 4 full hearts + 1 half heart + 5 empty (out of 10), no jitter (>32).
	reset_all(4 * HEALTH_PER_HEART + HEALTH_PER_HEART / 2);
	hud_survival_row_draw(HUD_LEFT, HEART_Y);
	assert(!(gstate.players[0].local_player->health <= 2 * HEALTH_PER_HEART));

	int q = 0;
	for(int k = 0; k < MAX_PLAYER_HEALTH / HEALTH_PER_HEART; k++) {
		assert(g_quads[q].tx == 16); // container always drawn
		q++;
		int heart_hp = gstate.players[0].local_player->health - k * HEALTH_PER_HEART;
		if(heart_hp >= HEALTH_PER_HEART) {
			assert(g_quads[q].tx == 52); // full
			q++;
		} else if(heart_hp >= HEALTH_PER_HEART / 2) {
			assert(g_quads[q].tx == 61); // half
			q++;
		} // else: no overlay quad for this heart
	}
	expect_quads("mixed", q);
	printf("  [2] mixed health overlay selection: OK (%d quads)\n", g_nquads);
}

// ---- 3: armor row gating ----
static void case_armor(void) {
	reset_all(MAX_PLAYER_HEALTH);
	g_armor_value = 0;
	hud_survival_row_draw(HUD_LEFT, HEART_Y);
	expect_quads("armor=0", (MAX_PLAYER_HEALTH / HEALTH_PER_HEART) * 2);

	reset_all(MAX_PLAYER_HEALTH);
	g_armor_value = 15; // > 0 -> exactly 10 armor quads, right-aligned
	hud_survival_row_draw(HUD_LEFT, HEART_Y);
	int hearts_q = (MAX_PLAYER_HEALTH / HEALTH_PER_HEART) * 2;
	expect_quads("armor=15", hearts_q + 10);
	for(int k = 0; k < 10; k++) {
		struct quad_call a = g_quads[hearts_q + k];
		int ax = HUD_LEFT + 182 * SC - (9 + k * 8) * SC;
		int au = (k * 2 + 1 < 15) ? 34 : ((k * 2 + 1 == 15) ? 25 : 16);
		assert(a.x == ax && a.y == HEART_Y);
		assert(a.tx == au && a.ty == 9);
	}
	printf("  [3] armor row gating + layout: OK\n");
}

// ---- 4: air bubble row ----
static void case_air(void) {
	reset_all(MAX_PLAYER_HEALTH);
	gstate.players[0].in_water = false;
	gstate.players[0].oxygen = 150; // partial, but dry -> no bubbles at all
	hud_survival_row_draw(HUD_LEFT, HEART_Y);
	expect_quads("dry", (MAX_PLAYER_HEALTH / HEALTH_PER_HEART) * 2);

	reset_all(MAX_PLAYER_HEALTH);
	gstate.players[0].in_water = true;
	gstate.players[0].oxygen = 32; // low -> full=1, total=2: exercises the POPPING bubble too
	hud_survival_row_draw(HUD_LEFT, HEART_Y);
	int hearts_q = (MAX_PLAYER_HEALTH / HEALTH_PER_HEART) * 2;
	int full = (32 - 2) * 10 + 299;
	full /= 300;
	int total = 32 * 10 + 299;
	total /= 300;
	assert(full == 1 && total == 2); // sanity: this case must hit the full<total branch
	expect_quads("wet oxygen=32", hearts_q + total);
	for(int k = 0; k < total; k++) {
		struct quad_call b = g_quads[hearts_q + k];
		assert(b.x == HUD_LEFT + k * 8 * SC);
		assert(b.y == HEART_Y - 9 * SC);
		assert(b.tx == ((k < full) ? 16 : 25) && b.ty == 18);
	}
	printf("  [4] air bubble row (dry=silent, wet=%d/%d bubbles): OK\n", full,
		   total);

	/* SOL-REVIEW item 4 (10-31): the menu variant (with_air=false) draws the
	 * hearts but NEVER the bubbles, even wet and low on air; with_air=true is
	 * byte-for-byte the gameplay row. */
	reset_all(MAX_PLAYER_HEALTH);
	gstate.players[0].in_water = true;
	gstate.players[0].oxygen = 32;
	hud_survival_row_draw_owned_ex(&gstate.players[0], HUD_LEFT, HEART_Y, false);
	expect_quads("menu wet oxygen=32 (no bubbles)", hearts_q);
	for(int k = 0; k < g_nquads; k++)
		assert(g_quads[k].ty != 18 && "a menu must not draw an air bubble");
	reset_all(MAX_PLAYER_HEALTH);
	gstate.players[0].in_water = true;
	gstate.players[0].oxygen = 32;
	hud_survival_row_draw_owned_ex(&gstate.players[0], HUD_LEFT, HEART_Y, true);
	expect_quads("gameplay wet oxygen=32 via _ex", hearts_q + total);
	printf("  [4b] menu row hides air bubbles (with_air=false): OK\n");
}

// ---- 5: hurt blink + prev-health ghost ----
static void case_hurt_blink(void) {
	reset_all(4 * HEALTH_PER_HEART); // 4 hearts current
	// hearts_life=15 -> hurt_blink true (15>=10 && (15/3)%2==1==5%2)
	gstate.players[0].camera.hurt.hearts_life = 15;
	gstate.players[0].camera.hurt.prev_health = 6 * HEALTH_PER_HEART; // was higher
	gstate.players[0].camera.hurt.update_counter = 0; // no jitter row-shift confound
	hud_survival_row_draw(HUD_LEFT, HEART_Y);

	int q = 0;
	for(int k = 0; k < MAX_PLAYER_HEALTH / HEALTH_PER_HEART; k++) {
		assert(g_quads[q].tx == 25); // white/blink container variant
		q++;
		int prev_hp = 6 * HEALTH_PER_HEART - k * HEALTH_PER_HEART;
		if(prev_hp >= HEALTH_PER_HEART) {
			assert(g_quads[q].tx == 70); // full ghost
			q++;
		} else if(prev_hp >= HEALTH_PER_HEART / 2) {
			assert(g_quads[q].tx == 79); // half ghost
			q++;
		}
		int heart_hp = 4 * HEALTH_PER_HEART - k * HEALTH_PER_HEART;
		if(heart_hp >= HEALTH_PER_HEART) {
			assert(g_quads[q].tx == 52);
			q++;
		} else if(heart_hp >= HEALTH_PER_HEART / 2) {
			assert(g_quads[q].tx == 61);
			q++;
		}
	}
	expect_quads("hurt blink", q);
	printf("  [5] hurt blink + prev-health ghost overlay: OK (%d quads)\n",
		   g_nquads);
}

// ---- 6: THE GATE -- the exact boolean + function screen_ingame.c calls ----
static void case_creative_gate(void) {
	reset_all(MAX_PLAYER_HEALTH);
	gstate.config_creative = true;
	if(!gstate.config_creative)
		hud_survival_row_draw_owned(&gstate.players[0], HUD_LEFT, HEART_Y);
	assert(g_nquads == 0 && g_binds == 0
		   && "creative must draw NO heart/armor/air quads");

	reset_all(MAX_PLAYER_HEALTH);
	gstate.config_creative = false;
	if(!gstate.config_creative)
		hud_survival_row_draw_owned(&gstate.players[0], HUD_LEFT, HEART_Y);
	// identical to case 1's golden trace -- survival is untouched by the gate.
	expect_quads("gate survival", (MAX_PLAYER_HEALTH / HEALTH_PER_HEART) * 2);
	assert(g_binds == 1);
	printf("  [6] creative gate: creative=0 quads, survival=%d quads "
		   "(unchanged)\n",
		   g_nquads);
}

// ---- 7: pin the real call site's source text ----
static char* read_source(const char* path) {
	FILE* f = fopen(path, "rb");
	assert(f && "source file not found -- is the test's WORKING_DIRECTORY the repo root?");
	assert(fseek(f, 0, SEEK_END) == 0);
	long size = ftell(f);
	assert(size > 0 && fseek(f, 0, SEEK_SET) == 0);
	char* buf = malloc((size_t)size + 1);
	assert(buf);
	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

static void case_call_site_pin(void) {
	const char* path = "source/game/gui/screen_ingame.c";
	char* src = read_source(path);
	const char* function = strstr(src, "void screen_ingame_render2D_owned(");
	const char* gate = function ? strstr(function, "if(!gstate.config_creative)") : NULL;
	const char* call = gate ? gate + strlen("if(!gstate.config_creative)") : NULL;
	if(call) call += strspn(call, " \t\r\n");
	const char* expected = "hud_survival_row_draw_owned(owner, hud_left, heart_row_y);";
	if(!call || strncmp(call, expected, strlen(expected))) {
		fprintf(stderr,
			"FAIL %s: the owned HUD draw must immediately follow the creative-mode guard\n",
			path);
		assert(0);
	}
	/* SOL-REVIEW item 4 (10-31): the menu HUD draws the row WITHOUT bubbles. */
	const char* menu = strstr(src, "void hud_menu_draw_owned(");
	const char* menu_call = menu
		? strstr(menu, "hud_survival_row_draw_owned_ex(owner, hud_left,") : NULL;
	assert(menu && menu_call && strstr(menu_call, "false);")
		   && "hud_menu_draw_owned must draw the row WITHOUT air bubbles");
	assert(!strstr(menu, "hud_survival_row_draw_owned(owner")
		   && "hud_menu_draw_owned must not use the gameplay (bubbles) row");
	free(src);
	printf("  [7] screen_ingame.c call-site pin (gameplay + menu): OK\n");
}


#include "support/hud_survival_row_reference.h"

static void clear_trace(void) {
	memset(g_quads, 0, sizeof(g_quads));
	g_nquads = g_binds = 0;
	g_last_bind = NULL;
}

static void case_owned_equivalence(void) {
	const int airs[] = {-20, 0, 1, 2, 3, 32, 272, 273, 300};
	struct quad_call expected[MAX_QUADS];
	unsigned cases = 0;
	for(int hp = 0; hp <= MAX_PLAYER_HEALTH; hp += HEALTH_PER_HEART / 2)
	for(int life = 0; life <= 20; life++)
	for(size_t air = 0; air < sizeof(airs)/sizeof(*airs); air++)
	for(int wet = 0; wet < 2; wet++) {
		reset_all(hp);
		g_armor_value = (hp + life) % 21;
		gstate.players[0].in_water = wet != 0;
		gstate.players[0].oxygen = airs[air];
		gstate.players[0].camera.hurt.hearts_life = life;
		gstate.players[0].camera.hurt.prev_health = MAX_PLAYER_HEALTH - hp;
		gstate.players[0].camera.hurt.update_counter = cases * 197u;
		hud_survival_row_reference(HUD_LEFT, HEART_Y);
		int count = g_nquads;
		memcpy(expected, g_quads, sizeof(expected));
		clear_trace();
		hud_survival_row_draw_owned(&gstate.players[0], HUD_LEFT, HEART_Y);
		assert(g_binds == 1 && g_last_bind == &texture_icons);
		assert(count == g_nquads && !memcmp(expected, g_quads, sizeof(expected)));
		cases++;
	}
	printf("  [8] owned HUD byte-exact against frozen renderer: %u cases OK\n", cases);
}

static void case_two_owners(void) {
	reset_all(MAX_PLAYER_HEALTH);
	struct entity guest_body = {0};
	guest_body.health = HEALTH_PER_HEART;
	struct client_session guest = {0};
	guest.local_player = &guest_body;
	guest.in_water = true;
	guest.oxygen = 32;
	guest.camera.hurt.hearts_life = 15;
	guest.camera.hurt.prev_health = 3 * HEALTH_PER_HEART;
	guest.camera.hurt.update_counter = 123;
	guest.windows[WINDOWC_INVENTORY] = &g_owner_windows[1];
	gstate.players[0].windows[WINDOWC_INVENTORY] = &g_owner_windows[0];
	g_owner_armor[0] = 0;
	g_owner_armor[1] = 15;
	struct client_session primary_before, guest_before;
	memcpy(&primary_before, &gstate.players[0], sizeof(primary_before));
	memcpy(&guest_before, &guest, sizeof(guest_before));
	hud_survival_row_draw_owned(&guest, 4, 160);
	assert(g_nquads == 26); /* 10 containers, 3 ghosts, 1 heart, 10 armor, 2 air. */
	assert(g_quads[0].tx == 25 && g_quads[14].ty == 9);
	struct quad_call guest_trace[MAX_QUADS];
	memcpy(guest_trace, g_quads, sizeof(guest_trace));
	clear_trace();
	hud_survival_row_draw(HUD_LEFT, HEART_Y);
	assert(g_nquads == 20 && g_quads[0].tx == 16);
	clear_trace();
	hud_survival_row_draw_owned(&guest, 4, 160);
	assert(g_nquads == 26 && !memcmp(guest_trace, g_quads, sizeof(guest_trace)));
	assert(!memcmp(&primary_before, &gstate.players[0], sizeof(primary_before)));
	assert(!memcmp(&guest_before, &guest, sizeof(guest_before)));
	puts("  [9] alternating owners: health, armor, air, blink and jitter independent");
}

int main(void) {
	case_quiet_full_health();
	case_mixed_health();
	case_armor();
	case_air();
	case_hurt_blink();
	case_creative_gate();
	case_call_site_pin();
	case_owned_equivalence();
	case_two_owners();
	printf("test_hud_survival_row: PASS\n");
	return 0;
}
