/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "graphics/gui_util.h"
#include "graphics/texture_atlas.h"
#include "item/items.h"
#include "lodepng/lodepng.h"
#include "platform/gfx.h"
#include "platform/input_glyph.h"
#include "platform/input_inventory.h"

struct tex_gfx texture_controller_pictures, texture_approved_controls;
struct tex_gfx texture_controls, texture_font, texture_terrain,
	texture_gui_widgets;
/* Unrelated GUI paths must stay unused in this headless draw test. */
static struct input_inventory inventory;
const struct input_inventory* input_devices(void) {
	return &inventory;
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
static bool texture_enabled=true;
void gfx_blending(enum gfx_blend mode) { (void)mode; } /* hint strip / card A set MODE_BLEND */
void gfx_texture(bool enabled) { texture_enabled=enabled; }
uint8_t tex_atlas_lookup(enum tex_atlas_entry entry) {
	(void)entry;
	assert(false);
	return 0;
}
void tex_gfx_lookup(struct tex_gfx* tex, int x, int y, uint8_t* color) {
	(void)tex;
	(void)x;
	(void)y;
	(void)color;
	assert(false);
}
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
static struct tex_gfx* bound;
static int cells[4], positions[4], cell_count, plus_count;
static bool check_notice_bounds;
static int text_quads, picture_quads;
static int picture_uv[4], picture_w, picture_h, solid_quads, notice_font;

void gfx_bind_texture(struct tex_gfx* texture) {
	bound = texture;
}
void gfx_draw_quads(size_t count, const int16_t* vertices,
					const uint8_t* colors, const uint16_t* uv) {
	(void)colors;
	assert(count == 4);
	if(check_notice_bounds) {
		for(int k = 0; k < 4; k++) {
			/* the card sits at (8,248) in the P2 viewport; card A (10-31)
			 * draws its 2 px dark edge just outside it, still in P2 */
			assert(vertices[k * 3] >= 6 && vertices[k * 3] < 640);
			assert(vertices[k * 3 + 1] >= 246 && vertices[k * 3 + 1] < 480);
		}
	}
	if(!texture_enabled){solid_quads++;return;}
	if(bound==&texture_font&&check_notice_bounds){text_quads++;assert(vertices[7]-vertices[1]==notice_font);}
	if(bound == &texture_controller_pictures) {
		picture_quads++;picture_w=vertices[3]-vertices[0];picture_h=vertices[7]-vertices[1];
		picture_uv[0] = uv[0]; picture_uv[1] = uv[1];
		picture_uv[2] = uv[2] - uv[0]; picture_uv[3] = uv[5] - uv[1];
		for(int k = 0; k < 16; k++) assert(colors[k] == 255);
		return;
	}
	if(bound == &texture_controls || bound == &texture_approved_controls) {
		assert(cell_count < 4);
		cells[cell_count] = uv[0] / 32 + uv[1] / 32 * 8;
		positions[cell_count++] = vertices[0];
		assert(uv[2] - uv[0] == 32 && uv[5] - uv[1] == 32);
	} else {
		assert(bound == &texture_font);
		if(uv[0] == ('+' % 16) * 16 && uv[1] == ('+' / 16) * 16)
			plus_count++;
	}
}

static void check_ink(const unsigned char* atlas, int cell) {
	assert(cell >= 0 && cell < INPUT_GLYPH_CELLS);
	unsigned ink = 0;
	for(int y = 0; y < 32; y++)
		for(int x = 0; x < 32; x++)
			ink += atlas[((cell / 8 * 32 + y) * 256 + cell % 8 * 32 + x) * 4
						 + 3]
				!= 0;
	assert(ink > 0);
}

/* Opaque pixels in a w-cell-wide run of gui/approved-controls.png; the old
 * empty-slot marker has 9, real art hundreds. */
static unsigned approved_ink(const unsigned char* atlas, int cell, int cells) {
	unsigned ink = 0;
	for(int y = 0; y < 32; y++)
		for(int x = 0; x < 32 * cells; x++)
			ink += atlas[((cell / 8 * 32 + y) * 256 + cell % 8 * 32 + x) * 4
						 + 3]
				!= 0;
	return ink;
}

/* PC keyboard/mouse glyphs (maintainer pick ctl1031-pc-glyphs, style A). */
static void test_pc_glyphs(void) {
	static const char* const keys[]
		= {"input.gui_split",		"input.gui_drop",
		   "input.gui_page",		"input.player_forward",
		   "input.player_backward", "input.player_left",
		   "input.player_right",	"input.player_jump",
		   "input.player_sneak",	"input.item_action_left",
		   "input.item_action_right", "input.scroll_left",
		   "input.scroll_right",	"input.inventory",
		   "input.open_menu",		"input.gui_up",
		   "input.gui_down",		"input.gui_left",
		   "input.gui_right",		"input.gui_click",
		   "input.gui_click_alt",	"input.screenshot",
		   "input.debug_overlay",	"input.player_drop",
		   "input.gui_previous",	"input.gui_quick_move"};
	struct config pc;
	assert(config_create(&pc, "config_pc.json"));
	unsigned char* atlas = NULL;
	unsigned width, height;
	assert(!lodepng_decode32_file(&atlas, &width, &height,
								  "assets/gui/approved-controls.png"));
	assert(width == 256 && height == 256);
	assert(approved_ink(atlas, INPUT_GLYPH_CELL_KEY_1U, 1) > 100);
	assert(approved_ink(atlas, INPUT_GLYPH_CELL_KEY_WIDE, 2) > 200);
	assert(approved_ink(atlas, INPUT_GLYPH_CELL_MOUSE_LEFT, 1) > 100);
	assert(approved_ink(atlas, INPUT_GLYPH_CELL_MOUSE_RIGHT, 1) > 100);
	assert(approved_ink(atlas, INPUT_GLYPH_CELL_MOUSE_WHEEL, 1) > 100);
	/* The wide cap is ONE 64-px picture: cells 45 and 46 share a row. */
	assert(INPUT_GLYPH_CELL_KEY_WIDE % 8 <= 6);

	size_t bound = 0;
	for(size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
		int codes[INPUT_BINDINGS_PER_ACTION];
		size_t n = INPUT_BINDINGS_PER_ACTION;
		assert(config_read_int_array(&pc, keys[k], codes, &n));
		for(size_t j = 0; j < n; j++, bound++) {
			int code = input_glyph_key(INPUT_CAT_NONE, codes[j]);
			int modifier, button;
			assert(code >= INPUT_GLYPH_PC_KEY_BASE && code < INPUT_GLYPH_PC_END);
			assert(input_glyph_binding(INPUT_CAT_NONE, &codes[j], 1) == code);
			assert(input_glyph_cells(code, &modifier, &button));
			assert(modifier == -1);
			bool wide = true;
			const char* label = input_glyph_label_for_code(code, &wide);
			if(codes[j] >= 1000) {
				assert(!label && !wide);
				assert(button
					   == (codes[j] == 1000 ? INPUT_GLYPH_CELL_MOUSE_LEFT
											: INPUT_GLYPH_CELL_MOUSE_RIGHT));
			} else {
				assert(label && *label);
				assert(wide == (strlen(label) > 1));
				assert(button
					   == (wide ? INPUT_GLYPH_CELL_KEY_WIDE
								: INPUT_GLYPH_CELL_KEY_1U));
			}
		}
	}
	assert(bound >= 20);
	free(atlas);

	/* Single characters on the 1u cap, longer names on the wide cap. */
	static const struct {
		int glfw;
		const char* label;
		bool wide;
	} names[] = {{65, "A", false},	   {90, "Z", false},	{48, "0", false},
				 {57, "9", false},	   {32, "Space", true}, {340, "Shift", true},
				 {344, "Shift", true}, {257, "Enter", true}, {256, "Esc", true},
				 {258, "Tab", true},   {341, "Ctrl", true}, {342, "Alt", true},
				 {290, "F1", true},	   {292, "F3", true},	{301, "F12", true},
				 {265, "Up", true},	   {264, "Down", true}, {263, "Left", true},
				 {262, "Right", true}};
	for(size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
		bool wide = !names[k].wide;
		int code = input_glyph_key(INPUT_CAT_NONE, names[k].glfw);
		const char* label = input_glyph_label_for_code(code, &wide);
		assert(label && !strcmp(label, names[k].label));
		assert(wide == names[k].wide);
		int modifier, button;
		assert(input_glyph_cells(code, &modifier, &button) && modifier == -1);
		assert(button
			   == (names[k].wide ? INPUT_GLYPH_CELL_KEY_WIDE
								 : INPUT_GLYPH_CELL_KEY_1U));
	}
	/* Mouse: wheel press (GLFW button 2) is the wheel picture, side buttons
	 * have no art and keep the generic MSE cell; no overlay text on any. */
	int modifier, button;
	assert(input_glyph_key(INPUT_CAT_NONE, 1002) == INPUT_GLYPH_PC_MOUSE_WHEEL);
	assert(input_glyph_cells(INPUT_GLYPH_PC_MOUSE_WHEEL, &modifier, &button)
		   && button == INPUT_GLYPH_CELL_MOUSE_WHEEL);
	assert(!input_glyph_label_for_code(INPUT_GLYPH_PC_MOUSE_WHEEL, NULL));
	assert(input_glyph_cells(input_glyph_key(INPUT_CAT_NONE, 1004), &modifier,
							 &button)
		   && button == 43);
	/* An unnamed GLFW key (161 WORLD_2) keeps the generic KEY cell; codes
	 * outside both GLFW ranges stay unbound. */
	assert(input_glyph_key(INPUT_CAT_NONE, 161) == 42);
	assert(input_glyph_key(INPUT_CAT_NONE, 31) == -1);
	assert(input_glyph_key(INPUT_CAT_NONE, 1008) == -1);
	/* PC codes never alias a cell or a chord, and NULL outputs are refused. */
	assert(INPUT_GLYPH_PC_KEY_BASE == INPUT_GLYPH_CELLS * (INPUT_GLYPH_CELLS + 1));
	assert(!input_glyph_cells(INPUT_GLYPH_PC_KEY_BASE + 65, NULL, &button));
	assert(!input_glyph_cells(INPUT_GLYPH_PC_END, &modifier, &button));
	/* A PC chord (Shift+A) shows its press key alone. */
	assert(input_glyph_binding(INPUT_CAT_NONE, (int[]) {340065}, 1)
		   == INPUT_GLYPH_PC_KEY_BASE + 65);
	/* Classic shoulder caps share the label path; lettered cells have none. */
	static const struct {
		int code;
		const char* label;
	} classic[] = {{208, "ZL"}, {209, "ZR"}, {210, "L"}, {211, "R"}};
	for(size_t k = 0; k < 4; k++) {
		int cell = input_glyph_key(INPUT_CAT_CLASSIC_CONTROLLER, classic[k].code);
		bool wide = true;
		const char* label = input_glyph_label_for_code(cell, &wide);
		assert(label && !strcmp(label, classic[k].label) && !wide);
	}
	for(int cell = 0; cell < INPUT_GLYPH_CELLS; cell++)
		if(cell != 14 && cell != 15 && cell != 22 && cell != 23)
			assert(!input_glyph_label_for_code(cell, NULL));
	assert(!input_glyph_label_for_code(-1, NULL));
	assert(!input_glyph_label_for_code((14 + 1) * INPUT_GLYPH_CELLS + 15, NULL));
	config_destroy(&pc);
	printf("PASS pc glyphs: %zu config_pc.json bindings resolve to cells 44-49 "
		   "with labels; 1-char keys 1u, long names wide; Classic L/R/ZL/ZR "
		   "labels\n",
		   bound);
}

int main(void) {
	test_pc_glyphs();
	struct config config;
	assert(config_create(&config, "config_wii.json"));
	/* Every action used by the converted menu/HUD/container draw sites. */
	const struct {
		enum input_button action;
		const char* key;
	} used[] = {{IB_GUI_UP, "input.gui_up"},
				{IB_GUI_CLICK, "input.gui_click"},
				{IB_GUI_CLICK_ALT, "input.gui_click_alt"},
				{IB_HOME, "input.open_menu"},
				{IB_INVENTORY, "input.inventory"},
				{IB_JUMP, "input.player_jump"},
				{IB_ACTION1, "input.item_action_left"},
				{IB_ACTION2, "input.item_action_right"},
				{IB_GUI_QUICK_MOVE, "input.gui_quick_move"},
				{IB_GUI_SPLIT, "input.gui_split"},
				{IB_GUI_DROP, "input.gui_drop"},
				{IB_GUI_PREVIOUS, "input.gui_previous"},
				{IB_GUI_PAGE, "input.gui_page"}};
	unsigned char* atlas = NULL;
	unsigned width, height;
	assert(
		!lodepng_decode32_file(&atlas, &width, &height, "assets/controls.png"));
	assert(width == 256 && height == 256);
	int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION] = {{0}};
	size_t lengths[IB_COUNT] = {0};
	for(size_t k = 0; k < sizeof(used) / sizeof(used[0]); k++) {
		lengths[used[k].action] = INPUT_BINDINGS_PER_ACTION;
		assert(config_read_int_array(&config, used[k].key, map[used[k].action],
									 &lengths[used[k].action]));
	}
	const int click[] = {4, 204, 404}, split[] = {5, 205, 405};
	const int inventory_keys[] = {6, 206, 407}, quick[] = {100, 207, 406};
	assert(lengths[IB_GUI_CLICK] == 3
		&& !memcmp(map[IB_GUI_CLICK], click, sizeof(click)));
	assert(lengths[IB_GUI_CLICK_ALT] == 3
		&& !memcmp(map[IB_GUI_CLICK_ALT], split, sizeof(split)));
	assert(lengths[IB_GUI_SPLIT] == 3
		&& !memcmp(map[IB_GUI_SPLIT], split, sizeof(split)));
	assert(lengths[IB_INVENTORY] == 3
		&& !memcmp(map[IB_INVENTORY], inventory_keys, sizeof(inventory_keys)));
	assert(lengths[IB_GUI_QUICK_MOVE] == 3
		&& !memcmp(map[IB_GUI_QUICK_MOVE], quick, sizeof(quick)));
	for(enum input_category family = INPUT_CAT_NUNCHUK;
		family <= INPUT_CAT_GAMECUBE; family++) {
		struct input_player_frame frame = {.category = family};
		input_glyph_frame(&frame, map, lengths);
		for(size_t k = 0; k < sizeof(used) / sizeof(used[0]); k++) {
			int modifier, button;
			assert(input_glyph_cells(frame.glyphs[used[k].action], &modifier,
									 &button));
			check_ink(atlas, button);
			if(modifier >= 0)
				check_ink(atlas, modifier);
		}
	}
	puts("PASS glyphs: every shipped family/action used by converted screens "
		 "resolves to an inked production atlas cell");

	struct input_player_frame p1 = {.category = INPUT_CAT_CLASSIC_CONTROLLER};
	struct input_player_frame p2 = {.category = INPUT_CAT_GAMECUBE};
	map[IB_GUI_CLICK][0] = 208204; /* Classic ZL + A */
	map[IB_GUI_CLICK][1] = 408404; /* GC Z + A */
	lengths[IB_GUI_CLICK] = 2;
	input_glyph_frame(&p1, map, lengths);
	input_glyph_frame(&p2, map, lengths);
	int modifier, button;
	assert(input_glyph_cells(p1.glyphs[IB_GUI_CLICK], &modifier, &button));
	assert(modifier == 14 && button == 10);
	assert(input_glyph_cells(p2.glyphs[IB_GUI_CLICK], &modifier, &button));
	assert(modifier == 36 && button == 32);
	struct input_player_frame saved = p1;
	map[IB_GUI_CLICK][1] = 409405;
	input_glyph_frame(&p2, map, lengths);
	assert(!memcmp(&saved, &p1, sizeof(p1)));
	cell_count = plus_count = 0;
	gutil_control_icon_owned(&p1, 240, 8, IB_GUI_CLICK, "Select");
	assert(cell_count == 2 && plus_count == 2); /* plus shadow + face */
	assert(cells[0] == 14 && cells[1] == 10 && positions[0] == 8
		   && positions[1] == 56);
	cell_count = plus_count = 0;
	gutil_menu_hint_owned(&p2, 8, 8, IB_GUI_CLICK, "Select");
	assert(cell_count == 2 && plus_count == 2 && cells[0] == 37
		   && cells[1] == 33);
	assert(input_glyph_binding(INPUT_CAT_GAMECUBE, (int[]) {208404}, 1) == -1);
	assert(input_glyph_binding(INPUT_CAT_WIIMOTE, (int[]) {4}, 1) == 0);
	assert(input_glyph_binding(INPUT_CAT_WIIMOTE, (int[]) {100}, 1) == -1);
	assert(input_glyph_dpad_direction_cell(INPUT_CAT_WIIMOTE, 0) == 25);
	assert(!input_glyph_cells(-1, &modifier, &button));
	assert(!input_glyph_cells(4160, &modifier, &button));
	assert(input_glyph_binding(INPUT_CAT_NONE, (int[]) {1000}, 1)
		   == INPUT_GLYPH_PC_MOUSE_LEFT);
	puts("PASS glyphs: both chord cells and plus reach the real GUI draw path; "
		 "P1/P2 bindings isolated; mixed-family chord rejected");
	/* Real notice text path, routed to the physical lower viewport. */
	ptime_t now = time_get();
	for(size_t k = 0; k < INPUT_NOTICE_COUNT; k++)
		inventory.notices[k] = (struct input_connection_notice) {
			.active = true,
			.player = 1,
			.assigned = true,
			.kind = INPUT_DEVICE_WPAD, .channel = 2,
			.connected = true, .category = INPUT_CAT_CLASSIC_CONTROLLER,
			.event = CONTROLLER_EVENT_CONNECTED,
			.expires = time_add_ms(now, INPUT_NOTICE_MS)};
	check_notice_bounds = true;notice_font=16;solid_quads=picture_quads=0;texture_controller_pictures.data=atlas;
	text_quads = 0;
	gutil_connection_notices(0, true, 0, 0, 640, 240);
	assert(text_quads == 0);
	gutil_connection_notices(1, true, 0, 240, 640, 240);
	assert(text_quads > 0&&solid_quads==6&&picture_quads==1&&picture_w==40&&picture_h==24); /* card A: edge+body+4 bevel */
 solid_quads=picture_quads=0;notice_font=12;
 gutil_connection_notices_in_header(1,true,8,248,600,24);
 assert(solid_quads==0&&picture_quads==1&&picture_w==32&&picture_h==20);
 solid_quads=picture_quads=0;
 gutil_connection_notices_mini(1,true,24,248,592,16);
 assert(solid_quads==5&&picture_quads==1&&picture_w==26&&picture_h==16);
 memset(inventory.notices,0,sizeof inventory.notices);
 inventory.notices[0]=(struct input_connection_notice){.active=true,.kind=INPUT_DEVICE_WPAD,.channel=0,.event=CONTROLLER_EVENT_JOIN_FAILED,.expires=time_add_ms(now,INPUT_NOTICE_MS)};
 const enum input_category families[]={INPUT_CAT_NUNCHUK,INPUT_CAT_CLASSIC_CONTROLLER,INPUT_CAT_GAMECUBE};
 const int buttons[]={0,10,32};
 notice_font=16; /* card A (10-31): 16 px text, edge+body+4 bevel quads */
 inventory.devices[0].published.connected=true; /* the card's device is here (GLYPH/1) */
 for(size_t k=0;k<3;k++) {
  inventory.devices[0].published.category=families[k];cell_count=0;solid_quads=picture_quads=0;
  gutil_connection_notices(0,false,0,240,640,240);
  assert(cell_count==2&&cells[0]==buttons[k]&&cells[1]==buttons[k]+1&&picture_quads==1&&solid_quads==6);
 }
	check_notice_bounds = false;
	puts("PASS notice drawing: all live transition text stays inside P2 "
		 "viewport; no draw in P1");
	/* Drive the real painter: pictures select the approved source rectangles,
	 * controller glyphs select the approved atlas, PC symbols retain fallback. */
	texture_controller_pictures.data = atlas;
	const int picture_cases[][5] = {
		{CONTROLLER_PICTURE_REMOTE, 132, 76, 40, 24},
		{CONTROLLER_PICTURE_NUNCHUK, 180, 76, 40, 24},
		{CONTROLLER_PICTURE_GAMECUBE, 132, 108, 40, 24},
		{CONTROLLER_PICTURE_CLASSIC, 180, 108, 40, 24}};
	for(size_t k = 0; k < 4; k++) {
		picture_quads = 0;
		gutil_controller_picture(24, 8, 40, 24, picture_cases[k][0]);
		assert(picture_quads == 1);
		assert(!memcmp(picture_uv, picture_cases[k] + 1, sizeof(picture_uv)));
	}
	texture_controller_pictures.data = NULL;
	picture_quads = 0;
	gutil_controller_picture(24, 8, 40, 24, CONTROLLER_PICTURE_REMOTE);
	assert(picture_quads == 0);
	texture_approved_controls.data = atlas;
	cell_count = 0;
	gutil_prompt_glyph(0, 0, 24, 32);
	assert(bound == &texture_approved_controls && cells[0] == 32);
	cell_count = 0;
	gutil_prompt_glyph(0, 0, 24, 43);
	assert(bound == &texture_controls && cells[0] == 43);
	texture_approved_controls.data = NULL;
	puts("PASS approved art: exact controller source regions, original colors, "
		 "approved button atlas and PC fallback through production painters");

	/* ---- DIRECTIONAL D-pad CELLS (build spec 15 A.1, brief 26 item 1b) ----
	 * input_glyph_key answers cell 24 -- the WHOLE D-pad -- for all four
	 * directions of every family, which is why the retired ruling-8 chat hint
	 * had to spell the direction in WORDS. The H-C2 socket draws one cell and
	 * no word, so it needs the direction itself: 25 Up, 26 Down, 27 Left,
	 * 28 Right, in the Up/Down/Left/Right code order every family uses
	 * (Remote 0-3, Classic 200-203, GameCube 400-403).
	 *
	 * ADDITIVE: the existing table is asserted UNCHANGED right below, so the
	 * ordinary prompt path (menu hints, footers, control icons) still gets the
	 * shared cell 24 it has always drawn. */
	{
		static const struct {
			const char* what;
			enum input_category family;
			int base;
		} dpads[] = {
			{"Wii Remote + Nunchuk", INPUT_CAT_NUNCHUK, 0},
			{"Classic Controller", INPUT_CAT_CLASSIC_CONTROLLER, 200},
			{"GameCube", INPUT_CAT_GAMECUBE, 400},
		};
		static const char* const names[4] = {"Up", "Down", "Left", "Right"};
		unsigned char* approved = NULL;
		unsigned aw, ah;
		assert(!lodepng_decode32_file(&approved, &aw, &ah,
									  "assets/gui/approved-controls.png"));
		assert(aw == 256 && ah == 256);
		for(size_t f = 0; f < sizeof(dpads) / sizeof(dpads[0]); f++) {
			for(int d = 0; d < 4; d++) {
				int code = dpads[f].base + d;
				int cell = input_glyph_dpad_direction_cell(dpads[f].family, code);
				assert(cell == 25 + d
					   && "the D-pad direction cell is not 25/26/27/28");
				/* The ordinary resolver is untouched: still the shared cell. */
				assert(input_glyph_key(dpads[f].family, code) == 24
					   && "input_glyph_key's D-pad mapping must not change");
				/* And the cell is real approved artwork, not an empty slot. */
				check_ink(approved, cell);
				printf("  MEASURE dpad-cell %-21s %-5s code %3d -> cell %d "
					   "(src %d,%d 32x32)\n", dpads[f].what, names[d], code,
					   cell, (cell % 8) * 32, (cell / 8) * 32);
			}
			/* One past the D-pad block is NOT a direction. */
			assert(input_glyph_dpad_direction_cell(dpads[f].family,
												   dpads[f].base + 4) < 0);
		}
		free(approved);
		/* -1 everywhere else: a code the family cannot press, the other
		 * families' D-pad code spaces, the stick/keyboard namespaces, the
		 * PC keyboard family. */
		assert(input_glyph_dpad_direction_cell(INPUT_CAT_NUNCHUK, 200) < 0);
		assert(input_glyph_dpad_direction_cell(INPUT_CAT_NUNCHUK, 400) < 0);
		assert(input_glyph_dpad_direction_cell(INPUT_CAT_CLASSIC_CONTROLLER, 0)
			   < 0);
		assert(input_glyph_dpad_direction_cell(INPUT_CAT_CLASSIC_CONTROLLER, 400)
			   < 0);
		assert(input_glyph_dpad_direction_cell(INPUT_CAT_GAMECUBE, 0) < 0);
		assert(input_glyph_dpad_direction_cell(INPUT_CAT_GAMECUBE, 200) < 0);
		assert(input_glyph_dpad_direction_cell(INPUT_CAT_NUNCHUK, 900) < 0);
		assert(input_glyph_dpad_direction_cell(INPUT_CAT_WIIMOTE, 0) == 25
			   && "the bare Remote uses the same D-pad artwork");
		assert(input_glyph_dpad_direction_cell(INPUT_CAT_NONE, 84) < 0);
		assert(input_glyph_dpad_direction_cell(INPUT_CAT_NONE, 0) < 0);
		assert(input_glyph_dpad_direction_cell(INPUT_CAT_NUNCHUK, -1) < 0);
		puts("PASS D-pad direction cells: Remote 0-3 / Classic 200-203 / "
			 "GameCube 400-403 resolve 25 Up, 26 Down, 27 Left, 28 Right on the "
			 "approved sheet; every other code, family and namespace answers "
			 "-1, and input_glyph_key's shared cell 24 is unchanged");
	}
	free(atlas);
	config_destroy(&config);
	return 0;
}
