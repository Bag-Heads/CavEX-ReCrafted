/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * GLYPH/1 (Astra pack-0926 item 1): controller glyphs can never come from a
 * texture pack, and the join glyph belongs to a DETECTED device.
 *
 * Part A drives the REAL texture resolver (texture.c): hostile pack copies of
 * all three controller-art sheets -- stale-size, blank and recoloured -- are
 * ignored in favour of the bundled bytes, ordinary pack art still wins, and
 * the Pack Helper's generation map keeps all three bundled_only.
 *
 * Part B drives the REAL prompt/notice painters (gui_util.c) over a headless
 * GUI-quad observer: the title's "Press A to join" and the Join card draw the
 * offered device's bundled A cell (Remote/Nunchuk 0, Classic 10, GameCube 32)
 * for all three families, draw no glyph and no icon spacing without a
 * detected eligible device, never infer a Remote from "unassigned", pick one
 * device when several are present, hand the offer over when it lapses, never
 * offer a device either owner holds, and suppress an actionable card whose
 * device is gone. */
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "game/game_state.h"
#include "graphics/gfx_settings.h"
#include "graphics/gui_util.h"
#include "graphics/texture_atlas.h"
#include "item/items.h"
#include "lodepng/lodepng.h"
#include "platform/gfx.h"
#include "platform/input_glyph.h"
#include "platform/input_inventory.h"
#include "platform/texture.h"
#include "support/test_fixture_dir.h"

struct game_state gstate;

/* ---- link-only doubles for the GUI draw path ---------------------------- */
static struct input_inventory live_inventory;
const struct input_inventory* input_devices(void) {
	return &live_inventory;
}
void input_prompt_frame(unsigned player, struct input_player_frame* frame) {
	(void)player;
	(void)frame;
	assert(false);
}
int gfx_width(void) {
	return 640;
}
int gfx_height(void) {
	return 480;
}
static bool texture_enabled = true;
void gfx_blending(enum gfx_blend mode) { (void)mode; } /* hint strip / card A set MODE_BLEND */
void gfx_texture(bool enabled) {
	texture_enabled = enabled;
}
uint8_t tex_atlas_lookup(enum tex_atlas_entry entry) {
	(void)entry;
	assert(false);
	return 0;
}
/* tex_gfx_lookup is texture.c's own (linked for the resolver). */
struct item* item_get(struct item_data* item) {
	(void)item;
	assert(false);
	return NULL;
}
void gfx_depth_range(float near, float far) {
	(void)near;
	(void)far;
	assert(false);
}
void gfx_write_buffers(bool color, bool depth, bool test) {
	(void)color;
	(void)depth;
	(void)test;
	assert(false);
}

/* The observer: every textured quad, by bound sheet. */
static struct tex_gfx* bound;
static int cells[8], cell_x[8], cell_count;
static int font_quads, first_font_x, picture_quads, solid_quads;
static bool pack_sheet_bound;

void gfx_bind_texture(struct tex_gfx* texture) {
	bound = texture;
}
void gfx_draw_quads(size_t count, const int16_t* vertices,
					const uint8_t* colors, const uint16_t* uv) {
	(void)colors;
	assert(count == 4);
	if(!texture_enabled) {
		solid_quads++;
		return;
	}
	if(bound == &texture_controls || bound == &texture_approved_controls) {
		assert(cell_count < 8);
		assert(uv[2] - uv[0] == 32 && uv[5] - uv[1] == 32);
		cell_x[cell_count] = vertices[0];
		cells[cell_count] = uv[0] / 32 + uv[1] / 32 * 8;
		/* Approved cells always come from the approved sheet when loaded. */
		assert(!texture_approved_controls.data || cells[cell_count] >= 42
			   || bound == &texture_approved_controls);
		cell_count++;
	} else if(bound == &texture_controller_pictures) {
		picture_quads++;
	} else if(bound == &texture_font) {
		if(!font_quads)
			first_font_x = vertices[0];
		font_quads++;
	} else {
		pack_sheet_bound = true; /* any other sheet is a failure below */
	}
}
static void clear_draw(void) {
	cell_count = font_quads = picture_quads = solid_quads = 0;
	first_font_x = -1;
	pack_sheet_bound = false;
}

/* ---- Part A: the resolver ----------------------------------------------- */
static const char* const sheets[3] = {
	"controls.png", "gui/approved-controls.png", "gui/controller-pictures.png"};

static void check_bundled(const char* name) {
	char path[512];
	snprintf(path, sizeof(path), "assets/%s", name);
	unsigned char* expected = NULL;
	unsigned ew, eh;
	assert(!lodepng_decode32_file(&expected, &ew, &eh, path));
	size_t w, h;
	unsigned char* actual = tex_read_gfx(name, &w, &h, TEX_FMT_RGBA32);
	assert(actual && w == ew && h == eh);
	assert(!memcmp(actual, expected, w * h * 4));
	free(actual);
	free(expected);
}

/* Hash the decoded image that the real GPU loader consumes, not PNG encoding. */
static uint64_t image_hash(const unsigned char* pixels, size_t bytes) {
	uint64_t hash = UINT64_C(14695981039346656037);
	for(size_t k = 0; k < bytes; k++)
		hash = (hash ^ pixels[k]) * UINT64_C(1099511628211);
	return hash;
}

static void observe_pointer(int tag, const char* filename) {
	if(strcmp(filename, "pointer.png") == 0) {
		/* Cache dependencies must follow the bundled source, even for a
		 * managed pack whose own files are covered only by its manifest. */
		assert(tag == 2);
		assert(!strcmp(tex_root_for_tag(tag), "assets"));
	}
}

static void check_pointer_and_font(const char* pack) {
	assert(config_set_string(&gstate.config_user, "paths.texturepack", pack));
	unsigned char* hand = NULL;
	unsigned ew, eh;
	assert(!lodepng_decode32_file(&hand, &ew, &eh,
								 "packs/beta-base/pointer.png"));
	size_t w, h;
	tex_set_read_observer(observe_pointer);
	unsigned char* actual = tex_read_gfx("pointer.png", &w, &h, TEX_FMT_RGBA16);
	tex_set_read_observer(NULL);
	assert(actual && w == ew && h == eh);
	uint64_t expected_hash = image_hash(hand, ew * eh * 4);
	uint64_t actual_hash = image_hash(actual, w * h * 4);
	printf("pointer %s: hand=%016llx loaded=%016llx\n", pack,
		   (unsigned long long)expected_hash, (unsigned long long)actual_hash);
	fflush(stdout);
	assert(actual_hash == expected_hash);
	assert(!memcmp(actual, hand, w * h * 4));
	free(actual);
	free(hand);

	/* The active pack's font still wins, with the usual bundled fallback. */
	char path[512];
	snprintf(path, sizeof(path), "%s/font/default.png", pack);
	unsigned char* font = NULL;
	if(lodepng_decode32_file(&font, &ew, &eh, path))
		assert(!lodepng_decode32_file(&font, &ew, &eh,
									 "assets/font/default.png"));
	actual = tex_read_gfx("font/default.png", &w, &h, TEX_FMT_I8);
	assert(actual && w == ew && h == eh);
	assert(!memcmp(actual, font, w * h * 4));
	free(actual);
	free(font);
}

static void test_pack_pointers(void) {
	check_pointer_and_font("assets");
	const char* roots[] = {"packs", "texturepacks"};
	for(size_t k = 0; k < sizeof(roots) / sizeof(roots[0]); k++) {
		DIR* dir = opendir(roots[k]);
		/* User-installed packs are optional in a clean checkout. The tracked
		 * packs/ base and all bundled controller sheets are still required. */
		if(!dir && !strcmp(roots[k], "texturepacks") && errno == ENOENT)
			continue;
		assert(dir);
		struct dirent* entry;
		while((entry = readdir(dir))) {
			if(entry->d_name[0] == '.')
				continue;
			char path[512];
			snprintf(path, sizeof(path), "%s/%s", roots[k], entry->d_name);
			struct stat st;
			assert(!stat(path, &st));
			if(S_ISDIR(st.st_mode))
				check_pointer_and_font(path);
		}
		closedir(dir);
	}
}

static void make_dir(const char* path) {
#ifdef _WIN32
	assert(!_mkdir(path));
#else
	assert(!mkdir(path, 0755));
#endif
}

static char* slurp(const char* path) {
	FILE* f = fopen(path, "rb");
	assert(f);
	static char buf[2][1 << 18];
	static int which;
	char* out = buf[which++ & 1];
	size_t n = fread(out, 1, sizeof(buf[0]) - 1, f);
	fclose(f);
	assert(n > 0 && n < sizeof(buf[0]) - 1);
	out[n] = '\0';
	return out;
}

/* Pack requirement/generation policy. The Pack Helper's conversion map is the
 * generation list: each controller sheet must be "bundled_only" and not
 * required, so a prepared pack never ships or regenerates it. */
static void check_pack_policy(void) {
	const char* map = slurp("tools/pack_helper/conversion_map.json");
	for(int k = 0; k < 3; k++) {
		char key[64];
		snprintf(key, sizeof(key), "\"%s\": {", sheets[k]);
		const char* entry = strstr(map, key);
		assert(entry);
		const char* eol = strchr(entry, '\n');
		assert(eol);
		size_t len = (size_t)(eol - entry);
		char line[512];
		assert(len < sizeof(line));
		memcpy(line, entry, len);
		line[len] = '\0';
		assert(strstr(line, "\"strategy\": \"bundled_only\""));
		assert(strstr(line, "\"required\": false"));
		assert(strstr(line, "\"fallback\": \"bundled\""));
	}
	for(int k = 0; k < 3; k++)
		assert(tex_bundled_only(sheets[k]));
	assert(!tex_bundled_only("font/default.png"));
	assert(tex_bundled_only("pointer.png"));
	assert(!tex_bundled_only("gui/menu-background.png"));
	assert(!tex_bundled_only(NULL));
	puts("PASS policy: the Pack Helper map marks all three controller sheets "
		 "bundled_only / not required; pointer is also bundled-only "
		 "in the resolver");
}

static void test_resolver(int argc, char** argv) {
	char fixture[128], gui[160], path[192];
	test_fixture_dir("test_controller_glyph_pack_proof", fixture,
					 sizeof(fixture));
	make_dir(fixture);
	snprintf(gui, sizeof(gui), "%s/gui", fixture);
	make_dir(gui);
	assert(config_set_string(&gstate.config_user, "paths.texturepack", fixture));
	static unsigned char pixels[256 * 256 * 4];
	/* Hostile replacement 1: valid but obsolete half-height sheets. */
	memset(pixels, 0xFF, sizeof(pixels));
	/* A future pack may ship an arbitrary pointer and its own font. */
	snprintf(path, sizeof(path), "%s/pointer.png", fixture);
	assert(!lodepng_encode32_file(path, pixels, 256, 256));
	snprintf(path, sizeof(path), "%s/font", fixture);
	make_dir(path);
	snprintf(path, sizeof(path), "%s/font/default.png", fixture);
	assert(!lodepng_encode32_file(path, pixels, 256, 256));
	check_pointer_and_font(fixture);
	for(int k = 0; k < 3; k++) {
		snprintf(path, sizeof(path), "%s/%s", fixture, sheets[k]);
		assert(!lodepng_encode32_file(path, pixels, 256, 128));
	}
	for(int k = 0; k < 3; k++)
		check_bundled(sheets[k]);
	/* 2: correctly sized, fully transparent (would erase every glyph). */
	memset(pixels, 0, sizeof(pixels));
	for(int k = 0; k < 3; k++) {
		snprintf(path, sizeof(path), "%s/%s", fixture, sheets[k]);
		assert(!lodepng_encode32_file(path, pixels, 256, 256));
		check_bundled(sheets[k]);
	}
	/* 3: correctly sized, opaque "redrawn" art (the Earth & Ash case). */
	for(size_t i = 0; i < sizeof(pixels); i += 4) {
		pixels[i] = 0x7C;
		pixels[i + 1] = 0x3A;
		pixels[i + 2] = (uint8_t)(i >> 10);
		pixels[i + 3] = 0xFF;
	}
	for(int k = 0; k < 3; k++) {
		snprintf(path, sizeof(path), "%s/%s", fixture, sheets[k]);
		assert(!lodepng_encode32_file(path, pixels, 256, 256));
		check_bundled(sheets[k]);
		assert(!remove(path));
		check_bundled(sheets[k]); /* and absent from the pack */
	}
	/* Ordinary pack art keeps pack-first resolution. */
	memset(pixels, 0xA5, 8 * 8 * 4);
	snprintf(path, sizeof(path), "%s/pack-art.png", fixture);
	assert(!lodepng_encode32_file(path, pixels, 8, 8));
	size_t w, h;
	unsigned char* art = tex_read_gfx("pack-art.png", &w, &h, TEX_FMT_RGBA16);
	assert(art && w == 8 && h == 8 && !memcmp(art, pixels, 8 * 8 * 4));
	free(art);
	puts("PASS resolver: stale, blank, redrawn and missing pack copies of "
		 "controls / approved-controls / controller-pictures all resolve to "
		 "the bundled bytes; ordinary pack art still resolves from the pack");
	/* Optional real packs (Beta, Earth & Ash): switch repeatedly. */
	for(int round = 0; round < 2; round++)
		for(int i = 1; i < argc; i++) {
			assert(config_set_string(&gstate.config_user, "paths.texturepack",
									 argv[i]));
			for(int k = 0; k < 3; k++)
				check_bundled(sheets[k]);
			printf("PASS resolver: all three controller sheets bundled for %s\n",
				   argv[i]);
		}
	assert(config_set_string(&gstate.config_user, "paths.texturepack", "assets"));
	test_fixture_dir_cleanup(fixture);
}

/* ---- Part B: the painters ------------------------------------------------ */
#define ICON (16 * GFX_GUI_SCALE)

static struct input_device_status* dev(struct input_inventory* inv,
									   enum input_device_kind kind,
									   unsigned channel) {
	struct input_device_status* s = (struct input_device_status*)
		input_inventory_device(inv, (struct input_device) {kind, channel});
	assert(s);
	return s;
}
static void publish(struct input_inventory* inv, enum input_device_kind kind,
					unsigned channel, enum input_category family,
					int assigned_player) {
	struct input_device_status* s = dev(inv, kind, channel);
	s->initialized = true;
	s->published = s->observed = (struct input_identity) {true, family};
	s->published_usable = family == INPUT_CAT_NUNCHUK
		|| family == INPUT_CAT_CLASSIC_CONTROLLER
		|| family == INPUT_CAT_GAMECUBE;
	s->published_assigned = assigned_player >= 0;
	s->published_player = assigned_player >= 0 ? (unsigned)assigned_player : 0;
}
static void offer(struct input_inventory* inv, enum input_device_kind kind,
				  unsigned channel) {
	inv->join.state = INPUT_JOIN_OFFERED;
	inv->join.device = (struct input_device) {kind, channel};
}

/* Player 1 holds a Remote + Nunchuk (or, when the device under test is a
 * Wii Remote, a GameCube pad) on the LAST channel of the other kind. */
static void p1_holds(enum input_device_kind under_test) {
	if(under_test == INPUT_DEVICE_GAMECUBE)
		publish(&live_inventory, INPUT_DEVICE_WPAD, 3, INPUT_CAT_NUNCHUK, 0);
	else
		publish(&live_inventory, INPUT_DEVICE_GAMECUBE, 3, INPUT_CAT_GAMECUBE,
				0);
}

/* Draws the title's join prompt through every production entry point and
 * returns the one A cell drawn (-1 = none), asserting the three agree. */
static int title_join_cell(void) {
	struct input_player_frame frame = {.category = INPUT_CAT_NONE,
									   .join_prompt = true};
	for(int b = 0; b < IB_COUNT; b++)
		frame.glyphs[b] = -1;
	const int x = 40, y = 400;
	int text_w = gutil_font_width("Press A to join", 8 * GFX_GUI_SCALE);

	clear_draw();
	gutil_menu_hint_owned(&frame, x, y, IB_GUI_CLICK, "Select");
	assert(!pack_sheet_bound && picture_quads == 0 && font_quads > 0);
	int cell = cell_count ? cells[0] : -1;
	assert(cell_count <= 1);
	int width = gutil_menu_hint_width_owned(&frame, IB_GUI_CLICK, "Select");
	int text_x = first_font_x;
	if(cell < 0) {
		/* No glyph, no icon spacing: the text starts at the prompt origin. */
		assert(width == text_w);
		assert(text_x == x || text_x == x + GFX_GUI_SCALE); /* face/shadow */
	} else {
		assert(cell_x[0] == x);
		assert(width == ICON + GUTIL_MENU_HINT_GAP + text_w);
	}
	/* Non-confirm hints stay hidden under the join prompt. */
	clear_draw();
	gutil_menu_hint_owned(&frame, x, y, IB_HOME, "Quit");
	assert(cell_count == 0 && font_quads == 0);
	assert(gutil_menu_hint_width_owned(&frame, IB_HOME, "Quit") == 0);

	/* The compact control-icon form draws the same cell. */
	clear_draw();
	int icon_w = gutil_control_icon_owned(&frame, 480, x, IB_GUI_CLICK,
										  "Confirm");
	assert(!pack_sheet_bound && cell_count == (cell >= 0));
	if(cell >= 0)
		assert(cells[0] == cell && cell_x[0] == x);
	int small = 5 * GFX_GUI_SCALE;
	assert(icon_w == (cell >= 0 ? ICON + small / 2 : 0) + small / 2
		   + gutil_font_width("Press A to join", small));
	assert(gutil_join_offer_cell(input_devices()) == cell);
	return cell;
}

/* Draws player 1's newest notice card; returns the confirm cell drawn
 * (-1 none) and reports whether any card was drawn at all. */
static int card_confirm_cell(bool* drawn, int* cancel_cell) {
	clear_draw();
	gutil_connection_notices(0, false, 0, 240, 640, 240);
	assert(!pack_sheet_bound);
	*drawn = solid_quads > 0;
	*cancel_cell = -1;
	if(cell_count == 2) {
		*cancel_cell = cells[1];
		return cells[0];
	}
	return cell_count == 1 ? cells[0] : -1;
}
static void set_notice(enum input_device_kind kind, unsigned channel,
					   enum controller_event event, enum input_category family) {
	memset(live_inventory.notices, 0, sizeof(live_inventory.notices));
	live_inventory.notices[0] = (struct input_connection_notice) {
		.expires = time_add_ms(time_get(), INPUT_NOTICE_MS),
		.kind = (uint8_t)kind, .channel = (uint8_t)channel, .player = 0,
		.event = (uint8_t)event, .category = (uint8_t)family,
		.active = 1, .connected = 1};
}

static void test_painters(void) {
	static unsigned char sheet[256 * 256 * 4];
	texture_approved_controls.data = sheet; /* bound, never sampled here */
	texture_controller_pictures.data = sheet;

	/* 1. No device at all: text only, and no card. */
	memset(&live_inventory, 0, sizeof(live_inventory));
	assert(title_join_cell() == -1);
	bool drawn;
	int cancel;
	assert(card_confirm_cell(&drawn, &cancel) == -1 && !drawn);
	puts("PASS absent: no detected device -> \"Press A to join\" text only, "
		 "no glyph, no icon spacing, no card");

	/* 2. Each family, on every channel, via the inventory's join offer; the
	 * Join card for the SAME device draws the SAME cell (+1 for Cancel). */
	static const struct {
		enum input_device_kind kind;
		enum input_category family;
		int cell;
		const char* name;
	} families[3] = {
		{INPUT_DEVICE_WPAD, INPUT_CAT_NUNCHUK, 0, "Remote+Nunchuk"},
		{INPUT_DEVICE_WPAD, INPUT_CAT_CLASSIC_CONTROLLER, 10, "Classic"},
		{INPUT_DEVICE_GAMECUBE, INPUT_CAT_GAMECUBE, 32, "GameCube"},
	};
	for(int f = 0; f < 3; f++)
		for(unsigned ch = 0; ch < 4; ch++) {
			memset(&live_inventory, 0, sizeof(live_inventory));
			publish(&live_inventory, families[f].kind, ch, families[f].family,
					-1);
			offer(&live_inventory, families[f].kind, ch);
			assert(title_join_cell() == families[f].cell);
			/* First-player state: nobody holds a controller, so the card
			 * is "New controller" with NO Join P2 action. */
			set_notice(families[f].kind, ch, CONTROLLER_EVENT_DETECTED,
					   families[f].family);
			assert(card_confirm_cell(&drawn, &cancel) == -1 && drawn
				   && picture_quads == 1);
			set_notice(families[f].kind, ch, CONTROLLER_EVENT_JOIN_CANCELLED,
					   families[f].family);
			assert(card_confirm_cell(&drawn, &cancel) == -1 && drawn);
			/* P2-offer state: player 1 holds another controller. */
			p1_holds(families[f].kind);
			assert(title_join_cell() == families[f].cell);
			set_notice(families[f].kind, ch, CONTROLLER_EVENT_DETECTED,
					   families[f].family);
			assert(card_confirm_cell(&drawn, &cancel) == families[f].cell
				   && drawn && picture_quads == 1);
			set_notice(families[f].kind, ch, CONTROLLER_EVENT_JOIN_OFFERED,
					   families[f].family);
			assert(card_confirm_cell(&drawn, &cancel) == families[f].cell + 1
				   && drawn); /* "Joining..." carries Cancel only */
			set_notice(families[f].kind, ch, CONTROLLER_EVENT_JOIN_FAILED,
					   families[f].family);
			assert(card_confirm_cell(&drawn, &cancel) == families[f].cell
				   && cancel == families[f].cell + 1);
			printf("PASS family %-14s ch%u: title + Join card draw bundled "
				   "A cell %d\n", families[f].name, ch, families[f].cell);
		}

	/* 3. Never infer a Remote: a bare Remote (no attachment) and an unknown
	 * family are not eligible, even offered and unassigned. */
	memset(&live_inventory, 0, sizeof(live_inventory));
	publish(&live_inventory, INPUT_DEVICE_WPAD, 1, INPUT_CAT_WIIMOTE, -1);
	offer(&live_inventory, INPUT_DEVICE_WPAD, 1);
	assert(title_join_cell() == -1);
	publish(&live_inventory, INPUT_DEVICE_WPAD, 1, INPUT_CAT_NONE, -1);
	assert(title_join_cell() == -1);
	/* A disconnected entry with a stale family is no device either. */
	publish(&live_inventory, INPUT_DEVICE_WPAD, 1, INPUT_CAT_NUNCHUK, -1);
	dev(&live_inventory, INPUT_DEVICE_WPAD, 1)->published.connected = false;
	assert(title_join_cell() == -1);
	puts("PASS no inference: bare Remote, unknown family and a disconnected "
		 "device draw no glyph");

	/* 4. Multiple devices: the inventory's offer wins over port order. */
	memset(&live_inventory, 0, sizeof(live_inventory));
	publish(&live_inventory, INPUT_DEVICE_WPAD, 0, INPUT_CAT_NUNCHUK, -1);
	publish(&live_inventory, INPUT_DEVICE_WPAD, 2, INPUT_CAT_CLASSIC_CONTROLLER,
			-1);
	publish(&live_inventory, INPUT_DEVICE_GAMECUBE, 1, INPUT_CAT_GAMECUBE, -1);
	offer(&live_inventory, INPUT_DEVICE_WPAD, 2);
	assert(title_join_cell() == 10);
	offer(&live_inventory, INPUT_DEVICE_GAMECUBE, 1);
	assert(title_join_cell() == 32);
	/* With no live offer, port order: Remote channels first. */
	live_inventory.join.state = INPUT_JOIN_IDLE;
	assert(title_join_cell() == 0);
	puts("PASS multiple: the offered device's cell; port order only without "
		 "an offer");

	/* 5. Handover: the offered device leaves (the inventory drops the offer)
	 * or is assigned; the prompt follows the remaining eligible device. */
	offer(&live_inventory, INPUT_DEVICE_WPAD, 2);
	dev(&live_inventory, INPUT_DEVICE_WPAD, 2)->published.connected = false;
	live_inventory.join.state = INPUT_JOIN_IDLE;
	assert(title_join_cell() == 0);
	dev(&live_inventory, INPUT_DEVICE_WPAD, 0)->published.connected = false;
	assert(title_join_cell() == 32);
	offer(&live_inventory, INPUT_DEVICE_WPAD, 0); /* stale offer, gone */
	assert(title_join_cell() == 32);
	dev(&live_inventory, INPUT_DEVICE_GAMECUBE, 1)->published.connected = false;
	assert(title_join_cell() == -1);
	/* The actionable card of a device that left is suppressed outright
	 * (player 1 held, so the Join P2 action would otherwise stand). */
	p1_holds(INPUT_DEVICE_WPAD);
	set_notice(INPUT_DEVICE_WPAD, 2, CONTROLLER_EVENT_DETECTED,
			   INPUT_CAT_CLASSIC_CONTROLLER);
	assert(card_confirm_cell(&drawn, &cancel) == -1 && !drawn);
	set_notice(INPUT_DEVICE_WPAD, 2, CONTROLLER_EVENT_JOIN_FAILED,
			   INPUT_CAT_CLASSIC_CONTROLLER);
	assert(card_confirm_cell(&drawn, &cancel) == -1 && !drawn);
	/* An informational card (no action) still shows without its device. */
	set_notice(INPUT_DEVICE_WPAD, 2, CONTROLLER_EVENT_DISCONNECTED,
			   INPUT_CAT_CLASSIC_CONTROLLER);
	live_inventory.notices[0].connected = 0;
	live_inventory.notices[0].previous_category = INPUT_CAT_CLASSIC_CONTROLLER;
	assert(card_confirm_cell(&drawn, &cancel) == -1 && drawn);
	puts("PASS handover: the prompt follows the remaining eligible device; "
		 "an actionable card without its device is suppressed");

	/* 6. Both owners: a device either player holds is never offered. */
	for(int owner = 0; owner < 2; owner++) {
		memset(&live_inventory, 0, sizeof(live_inventory));
		publish(&live_inventory, INPUT_DEVICE_WPAD, 3,
				INPUT_CAT_CLASSIC_CONTROLLER, owner);
		offer(&live_inventory, INPUT_DEVICE_WPAD, 3);
		assert(title_join_cell() == -1);
		publish(&live_inventory, INPUT_DEVICE_GAMECUBE, 0, INPUT_CAT_GAMECUBE,
				1 - owner);
		assert(title_join_cell() == -1);
		publish(&live_inventory, INPUT_DEVICE_WPAD, 1, INPUT_CAT_NUNCHUK, -1);
		assert(title_join_cell() == 0);
	}
	puts("PASS owners: devices held by player 1 or player 2 are never offered");

	/* 7. The capture override feeds the same painters and is removable. */
	static struct input_inventory proof;
	memset(&proof, 0, sizeof(proof));
	publish(&proof, INPUT_DEVICE_GAMECUBE, 2, INPUT_CAT_GAMECUBE, -1);
	offer(&proof, INPUT_DEVICE_GAMECUBE, 2);
	memset(&live_inventory, 0, sizeof(live_inventory));
	gutil_join_inventory_override(&proof);
	clear_draw();
	struct input_player_frame frame = {.join_prompt = true};
	gutil_menu_hint_owned(&frame, 0, 0, IB_GUI_CLICK, "Select");
	assert(cell_count == 1 && cells[0] == 32);
	gutil_join_inventory_override(NULL);
	assert(title_join_cell() == -1);
	puts("PASS override: presentation inventory reaches the same painters and "
		 "clears back to the live inventory");

	texture_approved_controls.data = NULL;
	texture_controller_pictures.data = NULL;
}

int main(int argc, char** argv) {
	assert(config_create(&gstate.config_user, "config_pc.json"));
	test_pack_pointers();
	check_pack_policy();
	test_resolver(argc, argv);
	test_painters();
	config_destroy(&gstate.config_user);
	puts("PASS test_controller_glyph_pack_proof");
	return 0;
}
