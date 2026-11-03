/* Approved Wii UI geometry as assertions (docs/ui_design/approved/menus.html +
 * chests.html; the Astra ui1/ui2/ui3 tables, docs/p2_astra_ui*_2026-09-11.md).
 * Coordinates are 640x480 design pixels; split values are local to a 640x240
 * viewport. Same recording harness as test_sort_presentation (every leaf gfx
 * call is --wrap'ed into a trace); the gameplay HUD rows, which live in
 * screen_ingame.c's big render function, are pinned by source text at their
 * call site exactly like test_hud_survival_row does. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "game/game_state.h"
#include "graphics/gui_util.h"
#include "graphics/gfx_settings.h"
#include "game/gui/screen.h"
#include "game/gui/container_menu.h"
#include "game/gui/container_compact.h"
#include "game/gui/recipe_browser.h"
size_t recipe_browser_footer_probe(const struct recipe_browser* b,
	const struct container_glyphs* glyphs, unsigned count, int* page_x, int* page_w,
	int xs[4], int widths[4]);
#include "game/gui/local_pause.h"
#include "game/gui/local_menu_host.h"
#include "platform/controller_events.h"
#include "game/gui/menu_header.h"
#include "platform/menu_sound.h"
#include "platform/audio.h"
#include "platform/input_glyph.h"
#include "platform/texture.h"
#include "platform/gfx.h"
#include "item/items.h"
#include "block/blocks.h"
#include "game/gui/chat_hud.h"
#include "game/gui/chat_composer_model.h"
#include "game/gui/chat_composer.h"
#include "network/server_interface.h"
#include "platform/input.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "lodepng/lodepng.h"
#include "platform/input_inventory.h"

/* ---- recording harness (mirrors test_sort_presentation.c) ---------------- */
static char calls[131072];
static size_t used;
static bool recording, texture_enabled = true;
static struct tex_gfx* bound_texture;
static bool chat_recording, alpha_test_enabled = true;
static enum gfx_blend blend_mode = MODE_OFF;
static unsigned chat_backdrops, chat_strips, chat_history_strips, chat_drafts,
	chat_counters;
static struct input_player_frame chat_owner;
/* Refused-enqueue case (check_chat_refused_send): the rendered `Retry` label
 * and the 2-px focus-yellow input outline SPEC's error state draws, the menu
 * cues the screen raises, and a scripted owner mask. All of it is inert until
 * the case arms it, so the existing checks above run exactly as before. */
static unsigned chat_retry_labels, chat_retry_outline_quads;
/* ---- the HUD pass's BLEND-STATE bracket (review 28 H1 + M3) -------------
 * THE CONTRACT, read out of both backends rather than assumed: the 2D pass is
 * entered by gfx_mode_gui_viewport, which sets MODE_BLEND and alpha_test(true)
 * -- source/platform/pc/gfx.c:412-413 and source/platform/wii/gfx.c:871-872 --
 * once per viewport (source/main.c's render loop, right before the 2D draws).
 * So MODE_BLEND is the state EVERY HUD draw is handed, and a block that
 * brackets its own quads has to hand that same state back; MODE_OFF is not a
 * restore, it is a different state that every later draw then inherits.
 *
 * These slots record the mode IN FORCE at the three HUD draws whose pixels the
 * mode actually changes: the hotbar BODY quad (a genuine Beta pack's bar
 * interior is translucent), the SNEAK rail texel, and Beta's sleep-fade wash
 * (a pure alpha ramp). They are filled by __wrap_gfx_draw_quads while
 * `hud_recording` is armed -- the text trace stays OFF for a whole HUD frame,
 * which would overflow `calls`. */
static bool hud_recording;
static int hud_blend_hotbar, hud_blend_rail, hud_blend_sleep;
static unsigned hud_hotbar_quads, hud_rail_quads, hud_sleep_quads,
	hud_socket_quads;
static bool chat_driving;
static bool chat_btn_down[IB_COUNT], chat_btn_edge[IB_COUNT];
static unsigned chat_cue_counts[MENU_SOUND_BATTERY_ATTENTION + 1];
void __real_input_prompt_frame(unsigned player, struct input_player_frame* frame);
void __wrap_input_prompt_frame(unsigned player, struct input_player_frame* frame) {
	if(chat_recording) *frame = chat_owner;
	else __real_input_prompt_frame(player, frame);
}
void __wrap_gfx_blending(enum gfx_blend mode) { blend_mode = mode; }
void __wrap_gfx_alpha_test(bool enabled) { alpha_test_enabled = enabled; }
void __real_gutil_text_rgb(int x, int y, const char* s, int size, bool shadow,
						 uint8_t r, uint8_t g, uint8_t b);
void __wrap_gutil_text_rgb(int x, int y, const char* s, int size, bool shadow,
						 uint8_t r, uint8_t g, uint8_t b) {
	/* The draft now starts at the SPEC's (64,172) and is budgeted against a
	 * FIXED 416-px clip that ends at x=480, not against Send.left
	 * (docs/ui_design/chat-keyboard/SPEC.md: "fixed 416-pixel draft clip
	 * ending 480; suffix scroll includes underscore budget"). */
	if(chat_recording && x == 64 && y == 172) {
		assert(strncmp(s, "> ", 2) == 0);
		assert(x + gutil_font_width(s, size) <= 64 + 416);
		assert(size == 16);
		assert(r == 224 && g == 224 && b == 224);
		chat_drafts++;
	}
	/* The counter is right-aligned to x=576 inside its reserved 88-px box and
	 * always reads n/100. */
	if(chat_recording && y == 172 && x != 64 && strchr(s, '/')) {
		assert(x >= 488 && x + gutil_font_width(s, size) == 576);
		assert(size == 16);
		chat_counters++;
	}
	/* SPEC "Error/refused enqueue": `Retry` REPLACES the counter inside that
	 * same 88-px allocation, so it is right-aligned to 576 too and no n/100 is
	 * drawn on the frames it shows. */
	if(chat_recording && y == 172 && strcmp(s, "Retry") == 0) {
		assert(x >= 488 && x + gutil_font_width(s, size) == 576);
		assert(size == 16);
		chat_retry_labels++;
	}
	__real_gutil_text_rgb(x, y, s, size, shadow, r, g, b);
}
static void record(const char* fmt, ...) {
	if(!recording) return;
	va_list ap; va_start(ap, fmt);
	int n = vsnprintf(calls + used, sizeof(calls) - used, fmt, ap); va_end(ap);
	assert(n >= 0 && (size_t)n < sizeof(calls) - used); used += (size_t)n;
}
void __wrap_gfx_bind_texture(struct tex_gfx* t) {
	bound_texture = t;
	const char* name = t == &texture_gui_crafting ? "crafting" : t == &texture_gui_furnace ? "furnace"
		: t == &texture_gui_chest ? "chest" : t == &texture_gui_widgets ? "widgets"
		: t == &texture_icons ? "icons" : t == &texture_controls ? "controls"
		: t == &texture_approved_controls ? "approved" : t == &texture_font ? "font"
		: t == &texture_pointer ? "pointer" : t == &texture_controller_pictures ? "pictures" : "other";
	record("bind %s\n", name);
}
void __wrap_gfx_texture(bool enabled) { texture_enabled = enabled; record("texture %d\n", enabled); }
/* Width of a 4-vertex quad from its corner span. */
static int chat_history_strip_w(const int16_t* v) {
	int lo = v[0], hi = v[0];
	for(int i = 1; i < 4; i++) {
		if(v[i * 3] < lo) lo = v[i * 3];
		if(v[i * 3] > hi) hi = v[i * 3];
	}
	return hi - lo;
}
void __wrap_gfx_draw_quads(size_t n, const int16_t* v, const uint8_t* c, const uint16_t* uv) {
	if(hud_recording && n == 4) {
		/* Each of the four is identified by what it IS, never by draw order:
		 * the hotbar body is gui.png (0,0) stretched 182 units wide, the rail
		 * is the single texel (1,23), the sleep wash is the untextured
		 * 0x101020 colour quad, and the socket FRAME is gui.png (0,22) at the
		 * socket's own x (the hotbar's selected-slot frame shares that UV but
		 * never that column). */
		int width = v[3] - v[0];
		if(texture_enabled && bound_texture == &texture_gui_widgets
		   && uv[0] == 0 && uv[1] == 0 && width == 182 * GFX_GUI_SCALE) {
			hud_blend_hotbar = blend_mode;
			hud_hotbar_quads++;
		} else if(texture_enabled && bound_texture == &texture_gui_widgets
				  && uv[0] == 1 && uv[1] == 23) {
			hud_blend_rail = blend_mode;
			hud_rail_quads++;
		} else if(!texture_enabled && c[0] == 0x10 && c[1] == 0x10
				  && c[2] == 0x20) {
			hud_blend_sleep = blend_mode;
			hud_sleep_quads++;
		} else if(texture_enabled && bound_texture == &texture_gui_widgets
				  && uv[0] == 0 && uv[1] == 22 && v[0] == 640 - 128) {
			hud_socket_quads++;
		}
	}
	if(chat_recording && !texture_enabled && n == 4) {
		/* Exercise the real renderer with no history and with nonempty history:
		 * the latter used to leave the subsequent draft in MODE_OFF. Only the
		 * three TRANSLUCENT draws are checked for blend state -- the keyboard's
		 * own opaque chrome (tab underline, pressed inset, focus border) is
		 * drawn untextured too and legitimately runs with blending off. */
		if(v[0] == 0 && v[1] == 0) {
			assert(blend_mode == MODE_BLEND && !alpha_test_enabled);
			assert(c[3] == 140); chat_backdrops++;
		} else if(c[0] == 255 && c[1] == 255 && c[2] == 224 && c[3] == 255) {
			/* SPEC's refused-enqueue INPUT OUTLINE: four opaque focus-yellow
			 * 2-px edges on the draft inset's own rect. Opaque and untextured,
			 * so it is not one of the three translucent draws below -- and it
			 * is the only focus-yellow chrome the composer draws (the focus
			 * border and the tab underline are pure white). */
			assert(chat_history_strip_w(v) == 528 || chat_history_strip_w(v) == 2);
			chat_retry_outline_quads++;
		} else if(v[0] == 56 && v[1] == 164) {
			/* the SPEC draft inset (56,164,528,32) */
			assert(blend_mode == MODE_BLEND && !alpha_test_enabled);
			assert(c[3] == 128); chat_strips++;
		} else if(c[3] == 127) {
			assert(blend_mode == MODE_BLEND && !alpha_test_enabled);
			/* open history band: x=44, strips 552 wide, never 640 */
			assert(v[0] == 44 && chat_history_strip_w(v) == 552);
			chat_history_strips++;
		}
	}
	record("quads %zu", n);
	for(size_t i = 0; i < n; i++)
		record(" | %d,%d,%d %u,%u,%u,%u %u,%u", v[i * 3], v[i * 3 + 1], v[i * 3 + 2],
			   c[i * 4], c[i * 4 + 1], c[i * 4 + 2], c[i * 4 + 3], uv[i * 2], uv[i * 2 + 1]);
	record("\n");
}
void __wrap_gutil_draw_item(struct item_data* item, int x, int y, int layer) {
	record("item %u %u %u %d %d %d\n", item->id, item->count, item->durability, x, y, layer);
}
int __wrap_gfx_width(void) { return 640; }
int __wrap_gfx_height(void) { return 480; }
void __wrap_input_pointer_enable(bool enable) { (void)enable; }
/* With no device owning player 0 (this harness, and a PC with no paired pad)
 * screen_chat_update presents the MERGED mask as player 0's own frame, so
 * these two wraps are the production input path for the refusal case. */
bool __wrap_input_held(enum input_button b) { return chat_driving && chat_btn_down[b]; }
bool __wrap_input_pressed(enum input_button b) { return chat_driving && chat_btn_edge[b]; }
static bool chat_pointer_valid;
static float chat_pointer_x, chat_pointer_y;
bool __wrap_input_pointer(float* x, float* y, float* angle) {
	*x = chat_pointer_x; *y = chat_pointer_y; *angle = 0;
	return chat_driving && chat_pointer_valid;
}
static const int font_widths[256] = {5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,4,1,4,5,5,5,5,2,4,4,4,5,1,5,1,5,5,5,5,5,5,5,5,5,5,5,1,1,4,5,4,5,6,5,5,5,5,5,5,5,5,3,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,3,5,3,5,5,2,5,5,5,5,5,4,5,5,1,5,4,2,5,5,5,5,5,5,5,3,5,5,5,5,5,5,4,1,4,6,5};
void __wrap_tex_gfx_lookup(struct tex_gfx* t, int x, int y, uint8_t* rgba) {
	assert(t == &texture_font);
	int k = x / 8 + (y / 8) * 16;
	memset(rgba, x % 8 < font_widths[k] ? 255 : 0, 4);
}
void __wrap_audio_menu_event(enum menu_sound_event e, ptime_t now) {
	(void)now;
	if((size_t)e < sizeof(chat_cue_counts) / sizeof(chat_cue_counts[0]))
		chat_cue_counts[e]++;
}

/* Does the trace hold a 4-vertex quad whose top-left is (x, y)? */
static bool quad_at(int x, int y) {
	char needle[48];
	snprintf(needle, sizeof(needle), "quads 4 | %d,%d,", x, y);
	return strstr(calls, needle) != NULL;
}

/* ---- 1. compact (split) inventory: header Next rule + crafting work area --- */
static char* read_source(const char* path) {
	FILE* fp = fopen(path, "rb");
	assert(fp && "run from the repository root (WORKING_DIRECTORY)");
	fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
	char* src = malloc((size_t)n + 1); assert(src && fread(src, 1, (size_t)n, fp) == (size_t)n);
	src[n] = 0; fclose(fp); return src;
}
/* Pin each production anchor, then exercise every sheet cell in those slots. */
static void check_controller_picture_slots(void) {
 const struct {const char* file;const char* anchor;const char* draw;const char* label;int x,y,h,gap;} sites[]={
  {"screen_pause.c","int sx=width/2-(full?146:105);","sx+(full?120-pw:0)","sx+128,status.y+25",174,348,64,8},
  {"screen_pause.c","int sx=width/2-(full?146:105);","sx+(full?120-pw:0)","sx+46,status.y+5",215,176,24,6},
  /* local_pause.c: picture + name centred as ONE group since abea2e610
   * (maintainer 10-31); the fixed-slot model below no longer applies and
   * test_local_pause / pause_approved pin that layout. */
  /* local_menu_host.c / screen.c recovery: the A4 disconnect card
   * (check_disconnect_card pins its picture and text) */
 };
 const int pads[]={CONTROLLER_PICTURE_REMOTE,CONTROLLER_PICTURE_NUNCHUK,CONTROLLER_PICTURE_GAMECUBE,CONTROLLER_PICTURE_CLASSIC};
 const int widths[]={24,56,96,120};
 const int large_x[]={4,36,100,4},large_y[]={4,4,4,76};
 const int mini_x[]={132,180,132,180},mini_y[]={76,76,108,108};
 void* saved=texture_controller_pictures.data;
 texture_controller_pictures.data=(void*)1;
 for(size_t s=0;s<sizeof sites/sizeof sites[0];s++) {
  char path[128];snprintf(path,sizeof path,"source/game/gui/%s",sites[s].file);
  char* src=read_source(path);
  assert(strstr(src,sites[s].anchor)&&strstr(src,sites[s].draw)&&strstr(src,sites[s].label));free(src);
  int slot_w=sites[s].h==64?120:40,label_x=sites[s].x+slot_w+sites[s].gap;
  for(int k=0;k<4;k++) {
   int w=gutil_controller_picture_width(pads[k],sites[s].h);
   assert(w==(sites[s].h==64?widths[k]:40));
   int x=sites[s].x+slot_w-w;
   assert(x>=sites[s].x&&x+w<=sites[s].x+slot_w&&x+w+sites[s].gap==label_x);
   used=0;calls[0]=0;recording=true;
   gutil_controller_picture(x,sites[s].y,w,sites[s].h,pads[k]);
   recording=false;
   char expected[128];int tx=sites[s].h==64?large_x[k]:mini_x[k],ty=sites[s].h==64?large_y[k]:mini_y[k];
   snprintf(expected,sizeof expected,"quads 4 | %d,%d,-2 255,255,255,255 %d,%d",x,sites[s].y,tx,ty);
   assert(strstr(calls,"bind pictures")&&strstr(calls,expected));
  }
 }
 texture_controller_pictures.data=saved;
 assert(gutil_controller_picture_width(CONTROLLER_PICTURE_NONE,64)==0);
 /* The full row must clear BOTH actual prompt implementations. */
 struct menu_composition c;menu_composition_begin(&c,MENU_SCREEN_PAUSE,0,0,640,480);
 struct menu_rect status=menu_pause_status(&c,5);
 assert(status.y==348&&status.h==64);
 char* src=read_source("source/graphics/gui_util.c");
 int factor=0;const char* y=strstr(src,"int y = height - scale * ");assert(y);
 assert(sscanf(y,"int y = height - scale * %d /",&factor)==1);
 int divisor=0;assert(sscanf(strchr(y,'/')+1,"%d",&divisor)==1);
 int prompt_y=480-(16*GFX_GUI_SCALE)*factor/divisor;
 assert(prompt_y-(status.y+status.h)>=15);free(src);
 src=read_source("source/game/gui/local_pause.c");
 int inset=0;y=strstr(src,"int fy=full?y+h-");
 if(y) {
  assert(sscanf(y,"int fy=full?y+h-%d",&inset)==1);
  assert(480-inset-(status.y+status.h)>=15);
 } else {
  /* controller UI 10-31: the local pause footer is the shared hint row */
  assert(strstr(src,"gutil_hint_row_layout(f,x,y,w,h,!full,GUTIL_HINT_ALIGN_CENTRE,"));
  assert(480-GUTIL_HINT_BOTTOM_FULL-GUTIL_HINT_STRIP_H-(status.y+status.h)>=15);
 }
 free(src);
 puts("PASS controller slots: all four pads at every picture call site, FINAL cells, fixed label x and full-pause prompt clearance");
}
static void check_compact_inventory(void) {
	/* ui-build-1026/T13 approved V1 (design-round-1026e): ONE page header
	 * "Inventory | Crafting" with the active page marked and NO glyph beside
	 * it (maintainer 2026-10-27: the page-swap hint is legend-only). Split:
	 * ink, 6 px gaps, at (44,20), inside the 226 px left column; full: light,
	 * 10 px gaps, centred over the panel. */
	int inv_w = gutil_font_width("Inventory", 16), craft_w = gutil_font_width("Crafting", 16);
	int bar_w = gutil_font_width("|", 16);
	assert(inv_w > 0 && craft_w > 0 && bar_w > 0);
	assert(container_page_header_width(false) == inv_w + 6 + bar_w + 6 + craft_w);
	assert(container_page_header_width(true) == inv_w + 10 + bar_w + 10 + craft_w);
	assert(44 + container_page_header_width(false) <= 44 + 226);
	assert(container_page_header_width(true) <= 372);
	/* Compact notices start 8 px after the header (contract kept by name). */
	assert(container_compact_inventory_next_x(false) + 24 + 8 == 44 + container_page_header_width(false) + 8);
	assert(container_compact_inventory_next_x(true) == container_compact_inventory_next_x(false));
	printf("  MEASURE page header: split %d px (room 226), full %d px\n",
		container_page_header_width(false), container_page_header_width(true));
	/* 26 px split work area (v1-5): armor 44,46+26k; portrait 74,46 58x104;
	 * 2x2 138+26c,64+26r; arrow 194,80 22x16; output 220,76. Pinned at the
	 * layout/draw sites (the inventory render needs the player portrait,
	 * which this leaf-wrapped harness does not host). */
	char* src = read_source("source/game/gui/container_compact.c");
	assert(strstr(src, "add_slot(v,44,46+(int)k*26,INVENTORY_SLOT_ARMOR+k);v->slots[v->count-1].extent=26;"));
	assert(strstr(src, "add_slot(v,138+(int)(k%2)*26,64+(int)(k/2)*26,INVENTORY_SLOT_CRAFTING+k);v->slots[v->count-1].extent=26;"));
	assert(strstr(src, "add_slot(v,220,76,INVENTORY_SLOT_OUTPUT);v->slots[v->count-1].extent=26;"));
	assert(strstr(src, "else inventory_portrait(v,inv,ox+74,oy+46,58,104);"));
	assert(strstr(src, "else gutil_texquad(ox+194,oy+80,90,35,22,15,22,16);"));
	assert(strstr(src, "if(full)container_page_header(ox+320-container_page_header_width(true)/2,oy+container_page_header_y(true),false,true);"));
	assert(strstr(src, "else container_page_header(ox+44,oy+container_page_header_y(false),false,false);"));
	/* ci-1101/07 (+ Sol R2): the full panel starts under the TV-safe header;
	 * machine_panel draws a 2 px outer border beyond its rect (rect(x-2,y-2,
	 * w+4,h+4)), so the DRAWN panel incl. border must end at or above the
	 * hearts row (363): 46 + 315 + 2 == 363 (last drawn row 362). */
	assert(strstr(src, "machine_panel(ox+(full?134:32),oy+(full?46:8),full?372:576,full?315:188);"));
	assert(strstr(src, "rect(x-2,y-2,w+4,h+4,17,255);") && "machine_panel border expansion changed; re-pin");
	{
		const int border = 2, panel_y = 46, panel_h = 315;
		int hearts_y = screen_ingame_hotbar_top(480, GFX_GUI_SCALE) - 10 * GFX_GUI_SCALE;
		assert(hearts_y == 363);
		assert(panel_y + panel_h + border <= hearts_y && "inventory panel border touches the hearts");
		assert(strstr(src, "machine_panel(px,py,full?372:576,full?331:188);"));
		assert(30 + 331 + 2 <= hearts_y && "machine panel border touches the hearts");
		for(int machine = 0; machine < 2; machine++) {
			struct container_compact panel;
			assert(container_compact_open(&panel,
				machine ? WINDOW_TYPE_DISPENSER : WINDOW_TYPE_FURNACE,
				machine ? 45 : 39, 0, 640, 480));
			for(size_t k = 0; k < panel.count; k++)
				assert(panel.slots[k].y + 36 <= 30 + 331);
		}
		/* the inventory hotbar slot row (off_y 30 + 288 .. +36) stays inside */
		assert(30 + 288 + 36 <= panel_y + panel_h);
		/* the Crafting page panel (recipe_ui_panel draws inside its rect) too */
		struct recipe_browser_geometry pg;
		recipe_browser_geometry_grid(2, 480, 0, 12, 0, 1, &pg);
		assert(pg.panel_y + pg.panel_h <= hearts_y);
	}
	/* No in-panel title, no header Previous/Next glyphs, no Recipes button. */
	assert(!strstr(src, "gutil_text_rgb(ox+146,oy+56,\"Inventory\""));
	assert(!strstr(src, "\"Recipes\",14,true"));
	assert(!strstr(src, "gutil_prompt_glyph(ox+44,oy+16,24,prompts->glyphs[IB_GUI_PREVIOUS]);"));
	assert(strstr(src, "left=ox+44+container_page_header_width(false)+8;"));
	free(src);
	char* host = read_source("source/game/gui/local_menu_host.c");
	assert(!strstr(host, "in->pointer_x>=ox+110&&in->pointer_x<ox+182"));
	assert(!strstr(host, "recipe_entry("));
	free(host);
	/* The Crafting page draws the same header (recipe_browser.c, grid 2). */
	char* rb = read_source("source/game/gui/recipe_browser.c");
	assert(strstr(rb, "if(full)header_w=container_page_header(bx+320-container_page_header_width(true)/2,by+container_page_header_y(true),true,true);"));
	assert(strstr(rb, "else header_w=container_page_header(bx+44,by+container_page_header_y(false),true,false);"));
	free(rb);
	/* ci-1101/07 (real-Wii softlock-repro-faad2a0 capture): the title row was
	 * clipped off the top of the screen -- B3 had the full header at y 10,
	 * inside the SDTV overscan band. The whole header rect (split ink plate
	 * y-3 .. underline y+20) must lie inside the pane AND, full screen, in
	 * the TV-safe band (top >= 24, like the bottom hint row's 24). Checked
	 * for 640/802 x 480 and both split halves (x 240). */
	for(int W = 640; W <= 802; W += 162) for(int H = 240; H <= 480; H += 240) {
		bool full = H >= 480;
		int oy = (H - (full ? 480 : 240)) / 2;
		int y = oy + container_page_header_y(full);
		int top = full ? y : y - 3, bottom = y + CONTAINER_PAGE_HEADER_H;
		int hx = full ? (W - 640) / 2 + 320 - container_page_header_width(true) / 2
					  : (W - 640) / 2 + 44;
		int hw = container_page_header_width(!full ? false : true);
		assert(top >= 0 && bottom <= H && "page header outside its pane");
		assert(hx >= 0 && hx + hw <= W && "page header outside its pane (x)");
		if(full) {
			assert(top >= GUTIL_HINT_BOTTOM_FULL && "full page header in the overscan band");
			/* and it stays above the panel it titles (border at panel-2) */
			assert(bottom <= 46 - 2);
			struct recipe_browser_geometry pg;
			recipe_browser_geometry_grid(2, H, 0, 12, 0, 1, &pg);
			assert(bottom <= pg.panel_y - 2);
		}
	}
	printf("  MEASURE page header y: full %d (h %d), split %d\n",
		container_page_header_y(true), CONTAINER_PAGE_HEADER_H, container_page_header_y(false));
	struct recipe_browser_geometry full, split, bench;
	recipe_browser_geometry_grid(2, 480, 0, 12, 0, 1, &full);
	recipe_browser_geometry_grid(2, 240, 0, 12, 0, 1, &split);
	recipe_browser_geometry(240, 0, 12, 0, 1, &bench);
	/* 2026-10-31 part B: the footer is the bottom hint row (strip top
	 * 428 / 196); the panels end above it (full: above the hearts, B3). */
	/* ci-1101/07: full page panel 46..362 (was 32..352), under the TV-safe
	 * header; its lowest row (inventory hotbar, panel+272..+308) still fits. */
	assert(full.panel_y == 46 && full.panel_h == 316 && full.category_y == 54
		&& full.family_y == 104 && full.name_y == 162 && full.footer_y == 428);
	assert(46 + 308 <= 46 + 316);
	assert(split.panel_y == 8 && split.panel_h == 184 && split.category_y == 42
		&& split.family_y == 86 && split.name_y == 134 && split.footer_y == 196);
	/* T13 fix r1 (Sol review): at EVERY size the vertical rows -- page header
	 * (plate y-3 .. underline y+20), tabs (32), up chevron (8), strip (36),
	 * down chevron (8), name (16), Have/Need (split: 152, 18), footer (32) --
	 * are disjoint and in order, and the pointer bands are disjoint, so a
	 * click has exactly one owner. Rows span the panel width, so disjoint y
	 * ranges mean no rectangle overlap. */
	for(int k = 0; k < 3; k++) {
		const struct recipe_browser_geometry* g = k == 0 ? &split : k == 1 ? &full : &bench;
		bool page = k < 2, compact = k != 1;
		int header_top = page && compact ? 20 - 3 : -1000, header_bottom = page && compact ? 20 + 20 : -1000;
		struct { int top, bottom; } rows[] = {
			{header_top, header_bottom},
			{g->category_y, g->category_y + 32},
			{g->variant_up_y, g->variant_up_y + 8},
			{g->family_y, g->family_y + 36},
			{g->variant_down_y, g->variant_down_y + 8},
			{g->name_y, g->name_y + 16},
			{compact ? g->panel_y + 136 + 7 : g->panel_y + 140, compact ? g->panel_y + 136 + 7 + 18 : g->panel_y + 140 + 104},
			{g->footer_y, g->footer_y + GUTIL_HINT_STRIP_H},
		};
		for(size_t r = 1; r < sizeof(rows) / sizeof(rows[0]); r++)
			assert(rows[r - 1].bottom <= rows[r].top && "recipe rows overlap");
		assert(g->panel_y <= g->category_y && g->panel_y + g->panel_h <= g->footer_y);
		assert(compact || g->panel_y + g->panel_h <= 363); /* above the full-screen hearts */
		/* Pointer bands: tabs | up band | strip | down band, no shared pixel. */
		assert(g->category_y + 32 <= g->band_top);
		assert(g->band_top <= g->variant_up_y && g->variant_up_y + 8 <= g->family_y);
		assert(g->family_y + 36 <= g->variant_down_y && g->variant_down_y + 8 <= g->band_bottom);
		assert(g->band_bottom <= g->name_y);
	}
	/* The workbench guide is not a page: its rows are unchanged; only the
	 * panel now ends above the bottom hint row (part B 10-31: 208 -> 176). */
	assert(bench.panel_y == 16 && bench.panel_h == 176 && bench.category_y == 24
		&& bench.family_y == 74 && bench.name_y == 132 && bench.footer_y == 196
		&& bench.variant_up_y == 58 && bench.variant_down_y == 112
		&& bench.band_top == 56 && bench.band_bottom == 128);
	puts("PASS compact inventory: V1 page header (no glyph) in both layouts, 26 px split work area, crafting page header/geometry, workbench unchanged");
}

/* ---- 2. split pause composition ------------------------------------------ */
static void check_pause_composition(void) {
	struct menu_composition c;
	for(int width = 640; width <= 854; width += 214) {
		int pw = width == 640 ? 576 : 720;
		for(int rows = 2; rows <= 4; rows += 2) {
			local_pause_compose(&c, 0, 0, width, 240, 80, rows);
			assert(c.window.x == (width - pw) / 2 && c.window.y == 12
				&& c.window.w == pw && c.window.h == 216);
			assert(c.action_count == (size_t)rows && c.notice_in_header);
			for(size_t k = 0; k < c.action_count; k++)
				assert(menu_rect_contains(&c.view, &c.actions[k]));
			assert(176 + 25 + 14 <= c.window.y + c.window.h);
			assert(c.window.y + c.window.h <= c.view.y + c.view.h);
			assert(208 + 32 <= c.view.y + c.view.h);
		}
	}
	local_pause_compose(&c, 0, 0, 640, 480, 80, 4);
	assert(c.window.w == 360 && c.window.h == 332);
	assert(c.header.y == 56 && c.header.h == 40);
	assert(c.header.y + c.header.h + 8 <= c.actions[0].y);
	struct menu_rect status = menu_pause_status(&c, 4);
	assert(status.h == 64 && status.y + status.h <= c.window.y + c.window.h);
	for(size_t k = 0; k < c.action_count; k++)
		assert(menu_rect_contains(&c.view, &c.actions[k]));
	puts("PASS pause composition: approved four-cell grid, two-row children, expanded recovery status");
}


/* ---- 3. footer pitch and chord width ------------------------------------- */
static void check_footer_widths(void) {
	struct input_player_frame f = {.connected = true};
	for(int b = 0; b < IB_COUNT; b++) f.glyphs[b] = 0;
	int icon = 16 * GFX_GUI_SCALE, text = 8 * GFX_GUI_SCALE;
	/* ordinary hint: 4 px gap (chest footers, Sort, options, sign) */
	assert(gutil_menu_hint_width_owned(&f, IB_GUI_CLICK, "Pick") == icon + 4 + gutil_font_width("Pick", text));
	/* approved inventory/machine footers: 3 px, Close 6 px */
	assert(gutil_footer_hint_gap(IB_GUI_CLICK) == 3 && gutil_footer_hint_gap(IB_INVENTORY) == 6);
	assert(gutil_menu_hint_width_gap(&f, IB_INVENTORY, "Close", 6) == icon + 6 + gutil_font_width("Close", text));
	/* a chord draws modifier + "+" + button = 80 px at the 32 px icon */
	int chord = ((3 + 1) << 6) | 0, modifier, button;
	assert(input_glyph_cells(chord, &modifier, &button) && modifier == 3 && button == 0);
	f.glyphs[IB_GUI_SPLIT] = chord;
	assert(gutil_menu_hint_width_gap(&f, IB_GUI_SPLIT, "Split", 3) == 80 + 3 + gutil_font_width("Split", text));
	puts("PASS footer pitch: 4 px ordinary, 3/6 px approved footers, chords 80 px");
}

/* ---- 3b. the Inventory DROP prompt -------------------------------------- */

/* Astra menu review 2026-09-19 MUST item 4: with the CavEX layout the Wii
 * Remote has NO gameplay drop (`input.player_drop` is an explicit empty array
 * in config_wii.json), so discarding an item DEPENDS on the menu -- and the
 * full / compact Inventory footer never said where it lives. The prompt shows
 * ONLY while the cursor holds a stack, carries the resolved glyph of the MENU
 * drop-held-stack role (IB_GUI_DROP), and must fit beside the prompts already
 * there in the SMALLEST layout. The widths are PRINTED as well as asserted,
 * because the fit is what the card asked to be measured.
 *
 * The seam calls the shipping helper, so this measures the drawn row rather
 * than a copy of its arithmetic.
 *
 * The Manual Grid half of the same card could not be solved this way -- its
 * footer box is 552 px and its four prompts already measure 474 px, so a
 * FIFTH 84 px prompt needs 558 px before any spacing. That was reported to
 * the lead rather than solved by abbreviating or dropping a prompt, and Astra
 * ruling 6 Q1 answered it: while the cursor holds a stack the Manual Grid row
 * SWAPS rather than grows -- Quick Move's prompt leaves and Drop takes its
 * place, so the row stays four wide. check_manual_grid_footer below. */
/* The three shipped families, resolved through the REAL glyph resolver from
 * the REAL config -- never a table of atlas cells written down here. */
static const char* family_name(enum input_category c) {
	return c == INPUT_CAT_NUNCHUK ? "Remote+Nunchuk"
		: c == INPUT_CAT_CLASSIC_CONTROLLER ? "Classic" : "GameCube";
}
static void shipped_frame(struct config* cfg, enum input_category category,
						  struct input_player_frame* out) {
	static const struct { enum input_button b; const char* key; } roles[] = {
		{IB_GUI_CLICK, "input.gui_click"}, {IB_GUI_SPLIT, "input.gui_split"},
		{IB_GUI_QUICK_MOVE, "input.gui_quick_move"},
		{IB_INVENTORY, "input.inventory"}, {IB_GUI_DROP, "input.gui_drop"},
	};
	*out = (struct input_player_frame) {.connected = true, .category = category};
	for(int b = 0; b < IB_COUNT; b++) out->glyphs[b] = -1;
	for(size_t k = 0; k < sizeof(roles) / sizeof(roles[0]); k++) {
		int codes[INPUT_BINDINGS_PER_ACTION];
		size_t n = INPUT_BINDINGS_PER_ACTION;
		assert(config_read_int_array(cfg, roles[k].key, codes, &n) && n);
		out->glyphs[roles[k].b] = input_glyph_binding(category, codes, n);
		assert(out->glyphs[roles[k].b] >= 0 && "a footer role lost its glyph");
	}
	/* PAGE_SWAP ships as the Wii code default in input.c (no config key):
	 * C / ZL / C-stick left / right, pinned against that source below. */
	static const int page_swap[] = {101, 208, 942, 943};
	out->glyphs[IB_GUI_PAGE_SWAP] = input_glyph_binding(category, page_swap, 4);
	assert(out->glyphs[IB_GUI_PAGE_SWAP] >= 0 && "the Page prompt lost its glyph");
}

/* Part B (controller UI 2026-10-31): the container prompts are ONE bottom
 * row laid out by the shared gutil_hint_row helper. The probe is the layout
 * the painter and the pointer both use (container_row_layout). */
static void check_row_cells(const struct gutil_hint_row* row, int width, int height) {
	int margin = gutil_hint_row_margin(width);
	int bottom = height - (height < 480 ? GUTIL_HINT_BOTTOM_SPLIT : GUTIL_HINT_BOTTOM_FULL);
	assert(row->strip.width > 0 && row->strip.y + row->strip.height == bottom);
	assert(row->strip.x == margin && "the container row is not bottom-left");
	assert(row->strip.x + row->strip.width <= width - margin);
	for(int k = 0; k < row->count; k++) {
		const struct gutil_button_bounds* r = &row->cells[k].rect;
		assert(r->x >= row->strip.x && r->x + r->width <= row->strip.x + row->strip.width);
		if(k) assert(r->x >= row->cells[k - 1].rect.x + row->cells[k - 1].rect.width
			&& "two row cells overlap");
	}
}
static int row_find(const struct gutil_hint_row* row, const char* label) {
	for(int k = 0; k < row->count; k++)
		if(!strcmp(row->cells[k].label, label)) return k;
	return -1;
}

/* B3 (10-31, Wii feedback on 018b4ba25): every container menu's bottom row
 * stays clear of the hearts and the hotbar hud_menu_draw_owned draws at the
 * gameplay positions -- inventory, chest, crafting (workbench + personal
 * page), furnace, dispenser at 640/802, full and split -- and the full-screen
 * panels (their slots) end above the hearts row. */
static bool rects_overlap(int ax, int ay, int aw, int ah, int bx, int by,
						  int bw, int bh);
static void check_menu_hud_clearance(void) {
	struct config cfg;
	assert(config_create(&cfg, "config_wii.json"));
	static const enum input_category families[3] = {INPUT_CAT_NUNCHUK,
		INPUT_CAT_CLASSIC_CONTROLLER, INPUT_CAT_GAMECUBE};
	static const enum container_row_kind kinds[3] = {CONTAINER_ROW_INVENTORY,
		CONTAINER_ROW_STORAGE, CONTAINER_ROW_MACHINE};
	int checked = 0;
	for(int f = 0; f < 3; f++) {
		struct input_player_frame frame;
		shipped_frame(&cfg, families[f], &frame);
		struct container_glyphs g;
		container_compact_player_glyphs(&g, &frame);
		for(int W = 640; W <= 802; W += 162) for(int H = 240; H <= 480; H += 240) {
			struct screen_ingame_hud_layout L;
			screen_ingame_hud_layout(W, H, &L);
			struct gutil_button_bounds rows[8];
			int n = 0;
			for(int k = 0; k < 3; k++) for(int held = 0; held < 2; held++) {
				struct gutil_hint_row row;
				enum container_row_action actions[CONTAINER_ROW_ITEMS];
				container_row_layout(kinds[k], held, k == 1, &frame, 0, 0, W, H,
									 &row, actions);
				assert(row.count > 0);
				rows[n++ & 7] = row.strip;
			}
			for(unsigned grid = 2; grid <= 3; grid++) {
				struct recipe_browser rb = {.grid = grid};
				int page_x, page_w, xs[4], widths[4];
				size_t m = recipe_browser_footer_probe(&rb, &g, 4, &page_x,
					&page_w, xs, widths);
				assert(m > 0); /* the probe lays out at 640x480 */
				struct gutil_button_bounds strip = {xs[0], 480
					- GUTIL_HINT_BOTTOM_FULL - GUTIL_HINT_STRIP_H, 1,
					GUTIL_HINT_STRIP_H};
				if(H == 480) rows[n++ & 7] = strip;
			}
			for(int k = 0; k < n && k < 8; k++) {
				const struct gutil_button_bounds* r = &rows[k];
				assert(!rects_overlap(r->x, r->y, r->width, r->height,
					L.hearts.x, L.hearts.y, L.hearts.width, L.hearts.height)
					&& "a menu row overlaps the hearts");
				assert(!rects_overlap(r->x, r->y, r->width, r->height,
					L.hotbar.x, L.hotbar.y, L.hotbar.width, L.hotbar.height - 1)
					&& "a menu row overlaps the hotbar");
				checked++;
			}
		}
	}
	/* full-screen machine panels (compact full layout): every slot ends
	 * above the hearts row (bar - 20); the full single chest is the legacy
	 * 24 + 332 = 356 panel, pinned in screen_chest.c below */
	static const enum window_type types[2] = {WINDOW_TYPE_FURNACE,
		WINDOW_TYPE_DISPENSER};
	static const size_t counts[2] = {39, 45};
	for(int W = 640; W <= 802; W += 162)
		for(int t = 0; t < 2; t++) {
			struct container_compact v;
			assert(container_compact_open(&v, types[t], counts[t], 0, W, 480));
			int hearts_y = screen_ingame_hotbar_top(480, GFX_GUI_SCALE) - 10 * GFX_GUI_SCALE;
			for(size_t k = 0; k < v.count; k++)
				assert(v.slots[k].y + 36 <= hearts_y && "a panel slot under the hearts");
		}
	char* src = read_source("source/game/gui/screen_inventory.c");
	assert(strstr(src, "(height-480)/2+30") && !strstr(src, "height-36"));
	free(src);
	src = read_source("source/game/gui/screen_chest.c");
	assert(!strstr(src, "chest_external_hud") && strstr(src, "hud_menu_draw_owned("));
	free(src);
	src = read_source("source/game/gui/recipe_browser.c");
	assert(!strstr(src, "recipe_ui_hud") && strstr(src, "hud_menu_draw_owned("));
	free(src);
	config_destroy(&cfg);
	printf("PASS B3 menu HUD: %d container/recipe rows clear of the gameplay "
		   "hearts + hotbar (640/802, full/split); full panels end above the "
		   "hearts; old h-36 / recipe_ui_hud / chest_external_hud gone\n", checked);
}

/* Wii feedback 10-31 (split_play_2m): a pane whose disconnect card is up
 * draws ONLY the card over the world -- the row, the hotbar body, items,
 * selection, crosshair and hearts are all behind one card_up gate. */
/* Q7 (maintainer 2026-10-31) + Sol review item 5: the full-screen 90-slot
 * chest is paged into the 3-row panel, which ends above the hearts band. */
static void check_full_chest_paged(void) {
	/* Full screen (Q7 + Sol review item 5): the one 3-row chest panel
	 * (24..344) and its hotbar row (y 302..334, counts to 336) end above
	 * the hearts band (345) and far above the row strip (428). */
	struct screen_ingame_hud_layout F;
	screen_ingame_hud_layout(640, 480, &F);
	char* src = read_source("source/game/gui/screen_chest.c");
	assert(strstr(src, "#define GUI_HEIGHT 160") && strstr(src, "#define CHEST_SHOWN_ROWS")
		   && strstr(src, "(height - 480) / 2 + 24") && !strstr(src, "214 : 160"));
	assert(strstr(src, "menu.page_count = chest_half_count();")
		   && strstr(src, "if(menu.page != shown_half) chest_fill_slots();"));
	free(src);
	assert(24 + 160 * GFX_GUI_SCALE <= F.hearts.y && "the full chest panel reaches the hearts");
	assert(24 + (85 + 3 * 18) * GFX_GUI_SCALE + 32 + 2 <= F.hearts.y && "chest hotbar counts under the hearts");
	assert(24 + 160 * GFX_GUI_SCALE <= F.row.y);
	src = read_source("source/game/gui/container_menu.c");
	assert(strstr(src, "bool paged = menu->page_count > 1;")
		   && strstr(src, "wc->type == WINDOW_TYPE_CHEST && paged && !menu->sort_open")
		   && "the unpaged chest hit-tests invisible header rects");
	free(src);
	puts("PASS Q7 full chest: one 3-row panel paged for 90 slots, above the hearts and the row; "
		 "the unpaged chest hit-tests no header rect");
}

static void check_card_hides_pane_hud(void) {
	assert(!screen_ingame_card_hides_hud(false, false));
	assert(screen_ingame_card_hides_hud(true, false));
	assert(screen_ingame_card_hides_hud(false, true));
	char* src = read_source("source/game/gui/screen_ingame.c");
	const char* fn = strstr(src, "void screen_ingame_render2D_owned(");
	assert(fn);
	const char* gate = strstr(fn, "bool card_up = screen_ingame_card_hides_hud(");
	/* first gate: the hint row block; second gate: the hotbar body onward */
	const char* row = strstr(fn, "if(!card_up) {");
	const char* bar = row ? strstr(row + 1, "if(!card_up) {") : NULL;
	assert(row && strstr(row, "struct screen_ingame_hint_ctx hc") < bar);
	const char* end = strstr(fn, "} /* !card_up */");
	const char* hearts = strstr(fn, "hud_survival_row_draw_owned(owner,");
	const char* items = strstr(fn, "k + INVENTORY_SLOT_HOTBAR, &item))");
	assert(gate && row && bar && end && hearts && items);
	assert(gate < row && row < bar && bar < items && items < hearts && hearts < end
		   && "a HUD piece escaped the card_up gate");
	free(src);
	puts("PASS card hides pane HUD: row, hotbar, items, crosshair and hearts "
		 "all gated while the pane's disconnect card is up");
}

static void check_drop_prompt(void) {
	struct config cfg;
	assert(config_create(&cfg, "config_wii.json"));
	char* input_src = read_source("source/platform/input.c");
	assert(strstr(input_src, "static const int page_swap[] = {101, 208, 942, 943};"));
	free(input_src);
	static const enum input_category families[3] = {INPUT_CAT_NUNCHUK,
		INPUT_CAT_CLASSIC_CONTROLLER, INPUT_CAT_GAMECUBE};
	static const int widths_[2] = {640, 802};
	for(int f = 0; f < 3; f++) {
		struct input_player_frame frame;
		shipped_frame(&cfg, families[f], &frame);
		for(int split = 0; split < 2; split++) for(int w = 0; w < 2; w++) {
			int width = widths_[w], height = split ? 240 : 480;
			struct gutil_hint_row row;
			enum container_row_action actions[CONTAINER_ROW_ITEMS];
			/* EMPTY Inventory: Crafting | Pick | Split | Close, no Drop. */
			int n = container_row_layout(CONTAINER_ROW_INVENTORY, false, false, &frame,
				0, 0, width, height, &row, actions);
			assert(n == 4 && row.fits);
			assert(!strcmp(row.cells[0].label, "Crafting")
				&& row.cells[0].glyph[0] == frame.glyphs[IB_GUI_PAGE_SWAP]);
			assert(!strcmp(row.cells[1].label, "Pick") && !strcmp(row.cells[2].label, "Split")
				&& !strcmp(row.cells[3].label, "Close"));
			assert(row_find(&row, "Drop") < 0 && "Drop is shown with an EMPTY cursor");
			check_row_cells(&row, width, height);
			/* HOLDING: Drop joins with the resolved MENU drop glyph. */
			n = container_row_layout(CONTAINER_ROW_INVENTORY, true, false, &frame,
				0, 0, width, height, &row, actions);
			int drop = row_find(&row, "Drop");
			assert(drop >= 0 && row.cells[drop].glyph[0] == frame.glyphs[IB_GUI_DROP]);
			assert(row_find(&row, "Close") >= 0);
			check_row_cells(&row, width, height);
			printf("  MEASURE inventory row %-14s %s w=%d: %d cells, strip %d..%d gap %d\n",
				family_name(families[f]), split ? "split " : "single", width, n,
				row.strip.x, row.strip.x + row.strip.width, row.gap);
			/* Machines and storage: Close is always kept, every cell inside. */
			for(int kind = CONTAINER_ROW_STORAGE; kind <= CONTAINER_ROW_MACHINE; kind++)
				for(int held = 0; held < 2; held++) {
					container_row_layout((enum container_row_kind)kind, held != 0,
						kind == CONTAINER_ROW_STORAGE, &frame, 0, 0, width, height, &row, actions);
					assert(row_find(&row, "Close") >= 0);
					assert((row_find(&row, "Drop") >= 0) == (held != 0) || !row.fits);
					check_row_cells(&row, width, height);
				}
		}
		/* A REMAP moves the prompt's glyph with the binding. */
		int remapped = input_glyph_key(families[f],
			families[f] == INPUT_CAT_NUNCHUK ? 5
			: families[f] == INPUT_CAT_CLASSIC_CONTROLLER ? 205 : 405);
		assert(remapped >= 0 && remapped != frame.glyphs[IB_GUI_DROP]);
		frame.glyphs[IB_GUI_DROP] = remapped;
		struct gutil_hint_row row;
		enum container_row_action actions[CONTAINER_ROW_ITEMS];
		container_row_layout(CONTAINER_ROW_INVENTORY, true, false, &frame, 0, 0, 640, 480, &row, actions);
		assert(row.cells[row_find(&row, "Drop")].glyph[0] == remapped
			&& "the Drop prompt kept a stale glyph");
		/* A chord-bound Drop (the widest the config format makes) still
		 * leaves a row inside the safe edges with Close in it. */
		int chord_code = families[f] == INPUT_CAT_NUNCHUK ? 9008
			: families[f] == INPUT_CAT_CLASSIC_CONTROLLER ? 212213 : 409410;
		int chord = input_glyph_binding(families[f], &chord_code, 1);
		int mod = -1, button = -1;
		assert(chord >= 0 && input_glyph_cells(chord, &mod, &button) && mod >= 0);
		frame.glyphs[IB_GUI_DROP] = chord;
		for(int split = 0; split < 2; split++) {
			container_row_layout(CONTAINER_ROW_INVENTORY, true, false, &frame, 0, 0, 640,
				split ? 240 : 480, &row, actions);
			assert(row_find(&row, "Close") >= 0);
			check_row_cells(&row, 640, split ? 240 : 480);
		}
	}
	/* T13: the personal Crafting page row is led by the Page legend on every
	 * shipped family, browse ("Craft x4" worst case) and Details, inside 44..596. */
	for(int f = 0; f < 3; f++) {
		struct input_player_frame frame;
		shipped_frame(&cfg, families[f], &frame);
		struct container_glyphs g;
		container_compact_player_glyphs(&g, &frame);
		struct recipe_browser rb = {.grid = 2};
		for(int details = 0; details < 2; details++) {
			rb.details = details != 0;
			int page_x, page_w, xs[4], widths[4];
			size_t n = recipe_browser_footer_probe(&rb, &g, 4, &page_x, &page_w, xs, widths);
			assert(n == (details ? 3u : 4u)); /* no Manual Grid prompt on the page */
			/* Bottom-left hint row (2026-10-31): the Inventory (PAGE_SWAP)
			 * cell leads at the TV-safe edge -- an ACTION cell since Sol
			 * review 10-31 item 2 -- the others follow, all inside. */
			assert(page_x == gutil_hint_row_margin(640) + GUTIL_HINT_PAD && "the Inventory cell did not lead the row");
			assert(xs[n - 1] == page_x && widths[n - 1] == page_w && "the Inventory cell is not an action cell");
			assert(xs[0] >= page_x + page_w + GUTIL_HINT_GAP_TIGHT); /* Craft stays action 0 */
			for(size_t k = 1; k + 1 < n; k++) assert(xs[k] >= xs[k - 1] + widths[k - 1] + GUTIL_HINT_GAP_TIGHT);
			assert(xs[n - 2] + widths[n - 2] <= 640 - gutil_hint_row_margin(640));
			printf("  MEASURE crafting %-14s %s page=%d first=%d end=%d\n", family_name(families[f]),
				details ? "details" : "browse ", page_w, xs[0], xs[n - 1] + widths[n - 1]);
		}
		rb.grid = 3; rb.details = false; /* the workbench guide is not a page */
		int page_x, page_w, xs[4], widths[4];
		recipe_browser_footer_probe(&rb, &g, 4, &page_x, &page_w, xs, widths);
		assert(page_x == -1 && xs[0] >= gutil_hint_row_margin(640));
	}
	config_destroy(&cfg);
	puts("PASS Drop prompt: shown only while the cursor holds a stack, glyph "
		 "resolved from the binding (and follows a remap), the held Inventory "
		 "footer fits single and split on all three families");
}

/* ---- 3c. the MANUAL GRID row, empty vs holding (Astra ruling 6 Q1) ------
 * Empty cursor: Pick | Split | Quick Move | Recipes. Holding: Place | One |
 * Drop | Recipes -- Quick Move's PROMPT leaves (its binding does not; see
 * test_container_menu.c), and Drop's glyph is the MENU drop role's. Drawn by
 * the shared bottom hint row since the 2026-10-31 controller UI redesign, so
 * the old 552 px in-panel gap rule is retired. */
extern size_t manual_grid_footer_probe(const struct manual_grid* v,
	const struct container_glyphs* g, bool held,
	struct gutil_button_bounds bounds[4], int symbols[4],
	const char* labels[4]);

static void check_manual_grid_footer(void) {
	struct config cfg;
	assert(config_create(&cfg, "config_wii.json"));
	static const enum input_category families[3] = {INPUT_CAT_NUNCHUK,
		INPUT_CAT_CLASSIC_CONTROLLER, INPUT_CAT_GAMECUBE};
	static const char* const empty_copy[4] = {"Pick", "Split", "Quick Move", "Recipes"};
	static const char* const held_copy[4] = {"Place", "One", "Drop", "Recipes"};
	for(int f = 0; f < 3; f++) {
		struct input_player_frame frame;
		shipped_frame(&cfg, families[f], &frame);
		struct container_glyphs glyphs;
		container_compact_player_glyphs(&glyphs, &frame);
		for(int split = 0; split < 2; split++) for(int width = 640; width <= 802; width += 162) {
			int height = split ? 240 : 480;
			int bottom = height - (split ? GUTIL_HINT_BOTTOM_SPLIT : GUTIL_HINT_BOTTOM_FULL);
			struct manual_grid v;
			manual_grid_open(&v, 3, width, height);
			struct gutil_button_bounds b[4];
			int symbols[4];
			const char* labels[4];
			for(int held = 0; held < 2; held++) {
				assert(manual_grid_footer_probe(&v, &glyphs, held != 0, b, symbols, labels) == 4);
				for(int k = 0; k < 4; k++) {
					assert(!strcmp(labels[k], held ? held_copy[k] : empty_copy[k]));
					assert(b[k].y + b[k].height == bottom && b[k].height == GUTIL_HINT_STRIP_H);
					if(k) assert(b[k].x >= b[k - 1].x + b[k - 1].width);
				}
				assert(b[0].x >= gutil_hint_row_margin(width));
				assert(b[3].x + b[3].width <= width - gutil_hint_row_margin(width));
				assert(symbols[0] == frame.glyphs[IB_GUI_CLICK]);
				assert(symbols[1] == frame.glyphs[IB_GUI_SPLIT]);
				assert(symbols[3] == frame.glyphs[IB_INVENTORY]);
				assert(symbols[2] == frame.glyphs[held ? IB_GUI_DROP : IB_GUI_QUICK_MOVE]);
			}
		}
		/* A REMAP moves the Drop prompt's glyph with the binding. */
		int remapped = input_glyph_key(families[f],
			families[f] == INPUT_CAT_NUNCHUK ? 5
			: families[f] == INPUT_CAT_CLASSIC_CONTROLLER ? 205 : 405);
		frame.glyphs[IB_GUI_DROP] = remapped;
		struct container_glyphs remap_glyphs;
		container_compact_player_glyphs(&remap_glyphs, &frame);
		struct manual_grid rv;
		manual_grid_open(&rv, 3, 640, 480);
		struct gutil_button_bounds rb[4];
		int rsymbols[4];
		const char* rlabels[4];
		assert(manual_grid_footer_probe(&rv, &remap_glyphs, true, rb, rsymbols, rlabels) == 4);
		assert(rsymbols[2] == remapped && "the Manual Grid Drop prompt kept a stale glyph");
	}
	config_destroy(&cfg);
	puts("PASS Manual Grid row: empty Pick|Split|Quick Move|Recipes, holding swaps in a "
		 "binding-resolved Drop, bottom-left inside the safe edges on all three families");
}

/* ---- 3d. H-S1, the SNEAK TOP RAIL (Astra build spec 15 A.2/A.3/A.4;
 * maintainer pick 2026-09-21 "and Hs 1") ---------------------------------
 *
 * The sneak TOGGLE (carve-out #2: the Wii pads have no finger free to hold a
 * sneak while the thumb walks) leaves the player latched into a crouch with
 * nothing on screen to say so -- the 0.08-block camera dip and the movement
 * slowdown do not read while standing still. A.2 answers with a rail along
 * the hotbar's own top border plus a label beside it:
 *
 *   rail (B+4, T, 356, 4), opaque, the PACK's selection colour, where
 *   B = floor((W-364)/2) and T = H-62; label "SNEAK" 16 px white with the
 *   native (2,2) shadow at (B-94, H-52), NO backing. One rule for single and
 *   split, resolved against that viewport's own width and height.
 *
 * RETIRES ruling 6/7's black box entirely: the 106x32 plain-black-at-192
 * backdrop, the centred single-view placement (bottom at hotbar top - 36,
 * pinned at (267,314)), the SPLIT SPECIAL CASE (viewport-local x=8, bottom
 * 8 px above the hotbar, pinned at (8,138)) and the HUD split predicate the
 * draw took to choose between them. What replaces all of it is one
 * viewport-local rule with no `split` argument at all.
 *
 * The geometry lives in a pure helper (the loading_notice_boot_layout
 * pattern) so the rectangles are assertable rather than eyeballed. */
extern void screen_ingame_sneak_rail_layout(int width, int height, int scale,
	int text_w, struct gutil_button_bounds* rail,
	struct gutil_button_bounds* text);
/* `modal` (review 28 M1) is the shared screen_ingame_hud_modal_active answer:
 * every modal that owns this viewport -- both recovery machines and the
 * single-view local pause -- hides the rail as well as the socket. */
extern bool screen_ingame_sneak_marker_visible(bool latched, bool config_on,
	bool capture, bool toast_visible, bool modal);
extern bool screen_ingame_hud_modal_active(bool owner_recovery,
	bool primary_recovery, bool local_pause);
extern bool screen_ingame_sneak_marker_config(struct config* c);

/* Do two half-open rectangles share a pixel? */
static bool rects_overlap(int ax, int ay, int aw, int ah, int bx, int by,
						  int bw, int bh) {
	return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* One rectangle against the spec's own numbers, printing BOTH on a miss so a
 * failure names the drift instead of just asserting false. */
static void check_rect(const char* what, const struct gutil_button_bounds* got,
					   int x, int y, int w, int h) {
	if(got->x != x || got->y != y || got->width != w || got->height != h) {
		printf("  FAIL %s = (%d,%d %dx%d), the spec says (%d,%d %dx%d)\n", what,
			   got->x, got->y, got->width, got->height, x, y, w, h);
		assert(false && "a HUD corner-block rectangle left the spec");
	}
}

/* The H-C2 socket's layout, declared here so the H-S1 rail can be measured
 * against it (section 4b below holds the socket's own tests). */
extern int screen_ingame_chat_socket_layout(
	const struct input_player_frame* input, int width, int height, int code,
	bool is_tap, struct gutil_button_bounds* body,
	struct gutil_button_bounds* frame, struct gutil_button_bounds* glyph,
	struct gutil_button_bounds* tap, struct gutil_button_bounds* label,
	int cells[2], const char** label_text);

static void check_sneak_rail_geometry(void) {
	static const struct { int w, h; bool split; const char* what; } views[] = {
		{640, 480, false, "4:3 single"}, {854, 480, false, "16:9 single"},
		{640, 240, true, "4:3 split"}, {854, 240, true, "16:9 split"},
	};
	for(int s = 1; s <= 3; s++) {
		for(size_t v = 0; v < sizeof(views) / sizeof(views[0]); v++) {
			int W = views[v].w, H = views[v].h;
			int text_w = gutil_font_width("SNEAK", 16);
			struct gutil_button_bounds rail, text;
			screen_ingame_sneak_rail_layout(W, H, s, text_w, &rail, &text);

			/* The hotbar anchors are recomputed here from the values
			 * test_hud_source_pin pins at the draw site, so the helper is
			 * checked against the HUD rather than against itself. A.2's
			 * B = floor((W-364)/2) and T = H-62 at the shipped scale 2. */
			int hotbar_left = (W - 182 * s) / 2, hotbar_top = screen_ingame_hotbar_top(H, s);
			if(s == GFX_GUI_SCALE)
				/* Controller UI A2 (10-31, Q1): the bar sits 1 px above the
				 * shared hint row's strip: T = 383 of 480, 151 of 240. */
				assert(hotbar_left == (W - 364) / 2
					   && hotbar_top == (H == 480 ? 383 : 151)
					   && "B and T are not the (A2 10-31) hotbar's own anchors");

			/* A.2: the rail rides the hotbar's own top border -- source
			 * pixels x = 2..179, y = 0..1 of the 182x22 bar -- so it is
			 * 178 units wide and 2 units tall, 2 units in from each end. */
			assert(rail.x == hotbar_left + 2 * s);
			assert(rail.y == hotbar_top);
			assert(rail.width == 178 * s && rail.height == 2 * s);
			/* "It leaves 4 px at each outer end" (at the shipped scale). */
			assert(rail.x - hotbar_left == (hotbar_left + 182 * s)
					   - (rail.x + rail.width)
				   && "the rail is not centred in the bar");
			/* Inside the hotbar's own rectangle, never outside it. */
			assert(rail.x >= hotbar_left && rail.y >= hotbar_top
				   && rail.x + rail.width <= hotbar_left + 182 * s
				   && rail.y + rail.height <= hotbar_top + 22 * s
				   && "the rail left the hotbar it rides");

			/* The label (maintainer 2026-10-30, "the sneak to the left of
			 * the hotbar like we had it"): 16 px text whose RIGHT edge is
			 * 8 px before the bar's left edge, centred on the bar's own
			 * 22-unit height. PIXELS, like ruling 7 V4's two 8s. No backing,
			 * so there is only ONE label rectangle to place. */
			assert(text.x + text.width == hotbar_left - 8
				   && "the SNEAK label must sit immediately left of the bar");
			assert(text.y == hotbar_top + (22 * s - 16) / 2);
			assert(text.y >= hotbar_top
				   && text.y + text.height <= hotbar_top + 22 * s
				   && "the SNEAK label must stay inside the bar's own row");
			assert(text.width == text_w && text.height == 16);

			/* Clearances, as RECTANGLES. The survival block's TOP is the air
			 * row, drawn at heart_row_y - 9 (hud_survival_row.c); heart_row_y
			 * is bar_y - 11, and the block is drawn from hud_left across the
			 * hotbar's own 182 units. The crosshair box is the aiming area:
			 * centre +- 16 px. The label sits BESIDE the bar, so the test is
			 * rectangle overlap, not a row-vs-row distance. */
			int survival_top = (hotbar_top - 10 * s) - 9 * s;
			bool room_beside = text.x >= 0 && text.x + text.width <= hotbar_left;
			if(s == GFX_GUI_SCALE)
				assert(room_beside
					   && "the SNEAK label must fit beside the hotbar");
			assert(!rects_overlap(text.x, text.y, text.width, text.height,
								  hotbar_left, hotbar_top, 182 * s, 22 * s)
				   && "the SNEAK label overlaps the hotbar");
			assert(!rects_overlap(text.x, text.y, text.width, text.height,
								  rail.x, rail.y, rail.width, rail.height)
				   && "the SNEAK label overlaps the rail");
			assert(!rects_overlap(text.x, text.y, text.width, text.height,
								  hotbar_left, survival_top, 182 * s,
								  hotbar_top - survival_top)
				   && "the SNEAK label overlaps the hearts/armour/air row");
			assert(!rects_overlap(text.x, text.y, text.width, text.height,
								  W / 2 - 16, H / 2 - 16, 32, 32)
				   && "the SNEAK label sits in the aiming area");
			/* A2 (10-31): the hint-row strip is a fixed 28 px, no longer
			 * 16*s*8/5, so a hypothetical 3x HUD in a 240 split pane puts
			 * the 66 px bar (top 129) over the pane centre; the shipped
			 * scale (2) and every single view stay clear. */
			if(s <= GFX_GUI_SCALE || !views[v].split)
				assert(!rects_overlap(rail.x, rail.y, rail.width, rail.height,
									  W / 2 - 16, H / 2 - 16, 32, 32)
					   && "the rail sits in the aiming area");
			/* Inside the viewport it is clipped to, at the shipped scale. */
			if(s == GFX_GUI_SCALE)
				assert(text.x >= 0 && text.y >= 0
					   && text.y + text.height <= H
					   && rail.x >= 0 && rail.x + rail.width <= W
					   && "the rail or its label left the viewport");
			printf("  MEASURE sneak-rail scale=%d %-11s %-6s rail=(%d,%d %dx%d) "
				   "text=(%d,%d %dx%d) ends: left=%d right=%d  beside-hotbar=%d"
				   "%s\n",
				   s, views[v].what, views[v].split ? "SPLIT" : "single",
				   rail.x, rail.y, rail.width, rail.height, text.x, text.y,
				   text.width, text.height, rail.x - hotbar_left,
				   (hotbar_left + 182 * s) - (rail.x + rail.width),
				   hotbar_left - (text.x + text.width),
				   room_beside ? "" : "  [no room beside the bar]");
		}
	}
	/* A.3's literals, spelled out so a formula that drifts cannot re-derive
	 * its way past them, in BOTH viewport heights -- "at 640x240, subtract
	 * 240 from every y". ONE rule: no split argument exists any more, so the
	 * two viewports of a 2-player split necessarily resolve the same
	 * rectangles as the single view at their own height. */
	{
		struct gutil_button_bounds rail, text;
		int text_w = gutil_font_width("SNEAK", 16);
		screen_ingame_sneak_rail_layout(640, 480, 2, text_w, &rail, &text);
		/* Controller UI A2 (10-31, WIRING-PLAN Q1): the hotbar sits 1 px
		 * above the shared hint row's strip (428): top 383 (split 151). */
		check_rect("single-view rail", &rail, 142, 383, 356, 4);
		check_rect("single-view SNEAK label", &text, 130 - text_w, 397,
				   text_w, 16);
		/* The hotbar occupies [138,502)x[383,427), the rail
		 * [142,498)x[383,387), and SNEAK ends at x = 130. */
		assert(rail.x == 142 && rail.x + rail.width == 498);
		assert(rail.y == 383 && rail.y + rail.height == 387);
		assert(text.x + text.width == 130);
		struct gutil_button_bounds top_rail, top_text, bot_rail, bot_text;
		screen_ingame_sneak_rail_layout(640, 240, 2, text_w, &top_rail,
										&top_text);
		screen_ingame_sneak_rail_layout(640, 240, 2, text_w, &bot_rail,
										&bot_text);
		assert(!memcmp(&top_rail, &bot_rail, sizeof(top_rail))
			   && !memcmp(&top_text, &bot_text, sizeof(top_text))
			   && "one rule: both split viewports must resolve identically");
		check_rect("split rail", &top_rail, 142, 151, 356, 4);
		check_rect("split SNEAK label", &top_text, 130 - text_w, 165, text_w,
				   16);
		printf("  MEASURE sneak-rail literals single rail=(142,383 356x4) "
			   "text=(%d,397 %dx16); split rail=(142,151 356x4) "
			   "text=(%d,165 %dx16)\n", 130 - text_w, text_w, 130 - text_w,
			   text_w);
	}
	/* THE SELECTED-SLOT FRAME DRAWS OVER THE RAIL (A.2), which is only
	 * meaningful if the two actually meet: both the FIRST and the NINTH slot
	 * frames overlap it, and the rail is neither shortened around them nor
	 * painted over them. Measured here; the paint ORDER is pinned at the
	 * draw site in check_sneak_rail_source_pin. */
	for(int split = 0; split < 2; split++) {
		int W = 640, H = split ? 240 : 480;
		const int s = GFX_GUI_SCALE;
		int hotbar_left = (W - 182 * s) / 2, hotbar_top = screen_ingame_hotbar_top(H, s);
		struct gutil_button_bounds rail, text;
		screen_ingame_sneak_rail_layout(W, H, s, gutil_font_width("SNEAK", 16),
										&rail, &text);
		for(int slot = 0; slot < 9; slot += 8) {
			int sel_x = hotbar_left - 2 + 20 * s * slot;
			assert(rects_overlap(rail.x, rail.y, rail.width, rail.height, sel_x,
								 hotbar_top - s, 24 * s, 24 * s)
				   && "the rail must run UNDER the selection frame, not around "
					  "it");
			printf("  MEASURE sneak-rail slot-%d selection frame (%d,%d %dx%d) "
				   "%-6s covers the rail\n", slot + 1, sel_x, hotbar_top - s,
				   24 * s, 24 * s, split ? "SPLIT" : "single");
		}
	}
	/* Clear of the H-C2 socket in both viewports: the two corner blocks are
	 * the pair the maintainer picked, so they are measured against each
	 * other, not just against the HUD. */
	for(int split = 0; split < 2; split++) {
		int W = 640, H = split ? 240 : 480;
		struct input_player_frame frame
			= {.connected = true, .category = INPUT_CAT_NUNCHUK};
		struct gutil_button_bounds body, frm, glyph, tap, label, rail, text;
		for(int b = 0; b < IB_COUNT; b++)
			frame.glyphs[b] = -1;
		frame.glyphs[IB_CHAT] = input_glyph_key(INPUT_CAT_NUNCHUK, 7);
		(void)screen_ingame_chat_socket_layout(&frame, W, H, 7, true, &body,
			&frm, &glyph, &tap, &label, NULL, NULL);
		screen_ingame_sneak_rail_layout(W, H, GFX_GUI_SCALE,
			gutil_font_width("SNEAK", 16), &rail, &text);
		const struct gutil_button_bounds* socket[3] = {&frm, &tap, &label};
		for(int p = 0; p < 3; p++) {
			assert(!rects_overlap(rail.x, rail.y, rail.width, rail.height,
								  socket[p]->x, socket[p]->y,
								  socket[p]->width, socket[p]->height)
				   && "the rail overlaps the chat socket");
			assert(!rects_overlap(text.x, text.y, text.width, text.height,
								  socket[p]->x, socket[p]->y,
								  socket[p]->width, socket[p]->height)
				   && "the SNEAK label overlaps the chat socket");
		}
	}
	puts("PASS H-S1 sneak rail geometry: GUI scales 1-3, 4:3 and 16:9 -- ONE "
		 "viewport-local rule (the split special case is gone), rail "
		 "(B+4,T,356,4) riding the hotbar's own top border with 4 px at each "
		 "end, label SNEAK 16 px ending 8 px left of the bar and centred on "
		 "it, no backing, literals (142,383 356x4)/(130-w,397) single and "
		 "(142,151)/(130-w,165) split, both "
		 "split viewports identical, the slot-1 and slot-9 selection frames "
		 "both meeting the rail, and clear of the hearts/armour/air row, the "
		 "crosshair box and the H-C2 socket");
}

static void check_sneak_marker_rules(void) {
	/* Ruling item 5 + item 8 + the capture lever (brief G-2 e): the marker
	 * shows exactly while THAT player's toggle is latched, the config key
	 * hides ONLY the marker, and the capture lever forces the DRAW without
	 * touching any sneak state. Config false wins over the lever: `false`
	 * means the marker is off. */
	assert(screen_ingame_sneak_marker_visible(true, true, false, false, false));
	assert(!screen_ingame_sneak_marker_visible(false, true, false, false, false));
	assert(!screen_ingame_sneak_marker_visible(true, false, false, false, false));
	assert(screen_ingame_sneak_marker_visible(false, true, true, false, false));
	assert(screen_ingame_sneak_marker_visible(true, true, true, false, false));
	assert(!screen_ingame_sneak_marker_visible(false, false, true, false, false));

	/* Ruling 7 V5: while THAT player's transient bed toast is visible the
	 * marker is NOT drawn -- 0 px of it, not a moved or dimmed one -- and it
	 * comes back BY ITSELF when the toast ends if the toggle is still
	 * latched. Nothing about the toast changes, and the suppression is not a
	 * latch of its own: the same inputs, one frame later, answer differently
	 * only because the toast's own visibility did. The capture lever obeys
	 * the same rule, so a rig frame can never photograph the pair
	 * overlapping. */
	assert(!screen_ingame_sneak_marker_visible(true, true, false, true, false)
		   && "the bed toast must suppress a latched marker");
	assert(screen_ingame_sneak_marker_visible(true, true, false, false, false)
		   && "the marker must return when the toast ends");
	assert(!screen_ingame_sneak_marker_visible(false, true, true, true, false)
		   && "the capture lever must obey the bed toast too");
	assert(screen_ingame_sneak_marker_visible(false, true, true, false, false));
	assert(!screen_ingame_sneak_marker_visible(true, true, true, true, false));
	assert(!screen_ingame_sneak_marker_visible(false, true, false, true, false));

	/* ui.sneak_toggle_indicator: JSON true/false, 1/0 -- maintainer
	 * 2026-10-30 makes it an option that is ON by default (left of the
	 * hotbar), so an ABSENT key means TRUE, and so does a config that has no
	 * JSON root at all. `false` hides it. */
	assert(screen_ingame_sneak_marker_config(NULL)
		   && "a config that cannot be read must default to ON");
	struct config cfg;
	assert(config_create(&cfg, "config_wii.json"));
	JSON_Object* root = json_object(cfg.root);
	assert(root);
	assert(json_object_dothas_value(root, "ui.sneak_toggle_indicator")
		   && "config_wii.json must state the key explicitly");
	assert(screen_ingame_sneak_marker_config(&cfg)
		   && "config_wii.json must state the key as TRUE (default on)");
	assert(json_object_dotset_boolean(root, "ui.sneak_toggle_indicator", 0)
		   == JSONSuccess);
	assert(!screen_ingame_sneak_marker_config(&cfg));
	assert(json_object_dotset_boolean(root, "ui.sneak_toggle_indicator", 1)
		   == JSONSuccess);
	assert(screen_ingame_sneak_marker_config(&cfg)
		   && "an explicit true must still show the marker");
	assert(json_object_dotset_number(root, "ui.sneak_toggle_indicator", 0)
		   == JSONSuccess);
	assert(!screen_ingame_sneak_marker_config(&cfg));
	assert(json_object_dotset_number(root, "ui.sneak_toggle_indicator", 1)
		   == JSONSuccess);
	assert(screen_ingame_sneak_marker_config(&cfg));
	assert(json_object_dotremove(root, "ui.sneak_toggle_indicator")
		   == JSONSuccess);
	assert(screen_ingame_sneak_marker_config(&cfg)
		   && "an absent ui.sneak_toggle_indicator must default to TRUE");
	config_destroy(&cfg);

	/* The presets carry the key explicitly too (brief G-2 c), and ruling 7
	 * (D) has them say FALSE with the shipped config -- the developer preset
	 * is an exact diff of config_wii.json with one key changed
	 * (tests/test_controls_sheet.py), so the two cannot drift apart. */
	static const char* const presets[] = {
		"tools/presets/config_wii_developer.json",
		"tools/presets/config_wii_pointer_look.json",
	};
	for(size_t k = 0; k < sizeof(presets) / sizeof(presets[0]); k++) {
		struct config p;
		assert(config_create(&p, presets[k]));
		JSON_Object* proot = json_object(p.root);
		assert(proot && json_object_dothas_value(proot,
												 "ui.sneak_toggle_indicator"));
		assert(screen_ingame_sneak_marker_config(&p)
			   && "both presets must state the key as TRUE");
		config_destroy(&p);
	}
	puts("PASS sneak marker rules: latched-only, suppressed while that "
		 "player's bed toast is up and back by itself afterwards (the capture "
		 "lever too), ui.sneak_toggle_indicator true/false/1/0, absent and "
		 "unreadable both ON (default on), and the shipped config plus both "
		 "presets state the key as true");
}

/* The rail's DRAW, pinned at its call site.
 *
 * RETIRES ruling 6/7's pins: that the draw emitted a plain black quad at
 * alpha 192 ("0, 0, 0, 192"), that it took the HUD's own split predicate
 * `gstate.players[1].local_player != NULL`, and that it ran BEFORE the chat
 * block and the F3 overlay so both painted over it. A.3 moves the rail INSIDE
 * the hotbar pass instead -- after the bar body, before the item icons and
 * the selected-slot frame -- so the frame paints over it at slots 1 and 9 and
 * no later hotbar draw can erase it, and A.2 removes the backing box
 * altogether. What is kept unchanged: the SNEAK word, gutil_text's white, no
 * border, no sound and no rumble, and the bed-toast term read from the
 * toast's own two facts. */
static void check_sneak_rail_source_pin(void) {
	char* src = read_source("source/game/gui/screen_ingame.c");
	const char* body = strstr(src, "static void sneak_rail_draw(");
	assert(body && "the rail draw helper is gone or renamed");
	/* The helper's closing brace: a `}` in column 0. Found without assuming
	 * the file's line endings -- read_source reads in binary, so a CRLF
	 * working copy would otherwise silently swallow the rest of the file
	 * into this window and make every check below meaningless. */
	const char* body_end = body;
	while((body_end = strchr(body_end + 1, '}'))
		  && !(body_end[-1] == '\n' || body_end[-1] == '\r'))
		;
	assert(body_end && "the rail draw helper has no closing brace");
	size_t n = (size_t)(body_end - body);
	char* only = malloc(n + 1);
	assert(only);
	memcpy(only, body, n);
	only[n] = '\0';
	assert(strstr(only, "\"SNEAK\"") && "the rail stopped saying SNEAK");
	/* A.2: the rail is ONE gui.png texel -- (1,23), the top-left bright pixel
	 * of the selection frame -- stretched over the whole rail, so the colour
	 * is the pack's own @selection by construction. No CPU pixel inspection
	 * and no texture-loader change anywhere in the helper. */
	assert(strstr(only, "1, 23, 1, 1")
		   && "the rail is not the single gui.png texel (1,23)");
	assert(!strstr(only, "tex_gfx_lookup") && !strstr(only, "tex_atlas_lookup")
		   && "A.2: the rail must not inspect texture pixels");
	/* "Keep no backing box": no untextured fill, no colour quad, no border. */
	assert(!strstr(only, "gutil_texquad_col") && !strstr(only, "gfx_texture(")
		   && "A.2: the SNEAK backing box must stay removed");
	assert(strstr(only, "gutil_text(text.x") && !strstr(only, "gutil_text_rgb")
		   && "the label must be gutil_text's white with its native shadow");
	assert(!strstr(only, "audio_") && !strstr(only, "menu_sound")
		   && "ruling item 9: the rail raises no sound");
	assert(!strstr(only, "rumble") && !strstr(only, "WPAD_Rumble")
		   && "ruling item 9: the rail raises no rumble");
	/* ONE rule: the draw takes no split argument any more. */
	assert(!strstr(only, "split")
		   && "A.2/A.5: the split special case must be gone");
	free(only);

	const char* fn = strstr(src, "void screen_ingame_render2D_owned(");
	assert(fn);
	/* A.3: the rail lives INSIDE the hotbar pass -- after the bar body quad,
	 * before the item icons and before the selected-slot frame, so the frame
	 * paints OVER it at slots 1 and 9 and no later hotbar draw erases it. */
	const char* bar = strstr(fn, "int bar_y = screen_ingame_hotbar_top(height, GFX_GUI_SCALE);");
	assert(bar && "the hotbar pass is gone or renamed");
	const char* bar_body = strstr(bar, "182, 22, 182 * GFX_GUI_SCALE");
	const char* call = strstr(fn, "sneak_rail_draw(");
	const char* items = strstr(fn, "gutil_draw_item(");
	const char* selection = strstr(fn, "bar_y - GFX_GUI_SCALE, 0, 22, 24, 24,");
	assert(bar_body && call && items && selection);
	assert(bar_body < call
		   && "the rail must be drawn AFTER the hotbar body");
	assert(call < items && "the rail must be drawn BEFORE the item icons");
	assert(call < selection
		   && "the selected-slot frame must paint OVER the rail");
	/* The rail's own anchors come from the hotbar pass's `width`/`height`, so
	 * one viewport-local rule serves single and split alike. */
	{
		const char* stop = strchr(call, ';');
		assert(stop);
		char* args = malloc((size_t)(stop - call) + 1);
		assert(args);
		memcpy(args, call, (size_t)(stop - call));
		args[stop - call] = '\0';
		assert(strstr(args, "width") && strstr(args, "height")
			   && !strstr(args, "gstate.players[1]")
			   && "the rail takes the viewport, not the HUD split predicate");
		free(args);
	}
	/* Ruling 7 V5: the suppression term is the TOAST'S OWN two facts -- the
	 * message text and its deadline -- read at the draw site, not a private
	 * copy or a second latch that could disagree with the toast inside one
	 * frame. Pinned here, and the toast's own draw is left untouched. */
	{
		const char* tv = strstr(fn, "bool toast_visible");
		const char* vis = strstr(fn, "screen_ingame_sneak_marker_visible(");
		assert(tv && vis && tv < vis
			   && "the toast term must be read before the marker gate");
		const char* stop = strchr(tv, ';');
		assert(stop);
		char* term = malloc((size_t)(stop - tv) + 1);
		assert(term);
		memcpy(term, tv, (size_t)(stop - tv));
		term[stop - tv] = '\0';
		assert(strstr(term, "owner->hud_message[0]")
			   && strstr(term, "owner->hud_message_until")
			   && "the toast term must read the toast's own two facts");
		free(term);
		stop = strchr(vis, ';');
		assert(stop);
		char* args = malloc((size_t)(stop - vis) + 1);
		assert(args);
		memcpy(args, vis, (size_t)(stop - vis));
		args[stop - vis] = '\0';
		assert(strstr(args, "toast_visible")
			   && "the marker gate must take the bed-toast term");
		free(args);
	}
	/* Rail and label appear and disappear TOGETHER: one gate, one draw. */
	{
		const char* vis = strstr(fn, "screen_ingame_sneak_marker_visible(");
		assert(vis && call > vis
			   && "the rail draw must sit under the visibility gate");
		assert(strstr(vis, "sneak_rail_draw(") == call
			   && "the gate must guard exactly one rail draw");
	}
	free(src);
	puts("PASS H-S1 rail draw pinned: the single gui.png texel (1,23) with no "
		 "pixel inspection, no backing box, no border, no sound, no rumble and "
		 "no split argument; drawn inside the hotbar pass after the bar body "
		 "and before the item icons and the selected-slot frame, under one "
		 "gate with its label, and suppressed from the bed toast's own text + "
		 "deadline");
}

/* ---- 4a. the restored CavEX hint row: context rules (maintainer ruling
 * 2026-10-29, reverting b70f38003's removal) ------------------------------ */
static void expect_row(const struct screen_ingame_hint_ctx* c,
					   const char* const* want_labels, const int* want_buttons,
					   int want_n, const char* what) {
	struct screen_ingame_hint h[SCREEN_INGAME_HINT_MAX];
	int n = screen_ingame_hint_row(c, h);
	bool ok = n == want_n;
	for(int k = 0; ok && k < n; k++)
		ok = h[k].button == want_buttons[k]
			&& !strcmp(h[k].label, want_labels[k]);
	if(!ok) {
		printf("  FAIL hint row %s: got", what);
		for(int k = 0; k < n; k++) printf(" [%d %s]", h[k].button, h[k].label);
		printf("\n");
		assert(false && "hint row content/order drifted");
	}
}

/* ---- A1 (controller UI 10-31): the shared hint row ---------------------- */
void gutil_test_set_font_widths(const uint8_t widths[256]);
/* A pack font's metrics, scanned exactly as gutil_reset_font scans them. */
/* Ink rows (top, bottom inclusive, in 8ths of a cell) of each letter. */
static int font_ink_top[256], font_ink_bottom[256];
static bool hint_load_font(const char* path, uint8_t widths[256]) {
	unsigned char* px = NULL;
	unsigned w = 0, h = 0;
	FILE* probe = fopen(path, "rb");
	if(!probe) return false; /* an optional local pack (never committed) */
	fclose(probe);
	assert(lodepng_decode32_file(&px, &w, &h, path) == 0);
	assert(w >= 128 && h >= 128 && w % 16 == 0 && h % 16 == 0);
	unsigned cw = w / 16, ch = h / 16;
	for(unsigned cell = 0; cell < 256; cell++) {
		unsigned ink = 0;
		for(unsigned x = 0; x < cw; x++) {
			bool present = false;
			for(unsigned y = 0; y < ch && !present; y++) {
				const unsigned char* p = px + 4 * ((cell / 16 * ch + y) * w
												   + cell % 16 * cw + x);
				present = p[0] || p[1] || p[2];
			}
			if(!present) break;
			ink++;
		}
		widths[cell] = (uint8_t)(ink * 8 / cw);
		font_ink_top[cell] = 8; font_ink_bottom[cell] = -1;
		for(unsigned y = 0; y < ch; y++)
			for(unsigned x = 0; x < cw; x++) {
				const unsigned char* p = px + 4 * ((cell / 16 * ch + y) * w
												   + cell % 16 * cw + x);
				if(p[0] || p[1] || p[2]) {
					int row = (int)(y * 8 / ch);
					if(row < font_ink_top[cell]) font_ink_top[cell] = row;
					if(row > font_ink_bottom[cell]) font_ink_bottom[cell] = row;
				}
			}
	}
	widths[' '] = 4;
	free(px);
	return true;
}
/* Invariants every laid-out row keeps. */
static void hint_row_invariants(const struct gutil_hint_row* r,
	const struct gutil_hint_item* items, int x, int y, int width, int height,
	bool split, enum gutil_hint_align align) {
	int margin = gutil_hint_row_margin(width);
	assert(r->count > 0 && r->fits);
	assert(r->strip.height == 28);
	assert(r->strip.y + r->strip.height == y + height - (split ? 16 : 24));
	assert(r->strip.x >= x + margin);
	assert(r->strip.x + r->strip.width <= x + width - margin);
	if(align == GUTIL_HINT_ALIGN_LEFT) assert(r->strip.x == x + margin);
	else assert(abs((r->strip.x - x) - (x + width - r->strip.x - r->strip.width)) <= 1);
	assert(r->gap == 14 || r->gap == 8);
	/* one centre line: glyph centre == text centre (7-row letters, 16 px) */
	assert(r->glyph_y + 12 == r->text_y + 7);
	assert(r->glyph_y + 12 == r->strip.y + 14);
	int at = r->strip.x + 8;
	for(int k = 0; k < r->count; k++) {
		const struct gutil_hint_cell* c = &r->cells[k];
		assert(c->glyph_count >= 1 && c->glyph_count <= 2);
		assert(c->rect.x == at && c->glyph_x[0] == at);
		assert(c->rect.y == r->strip.y && c->rect.height == 28);
		if(c->glyph_count == 2)
			assert(c->glyph_x[1] == c->glyph_x[0] + c->glyph_w[0] + 4);
		int last = c->glyph_count - 1;
		assert(c->label_x == c->glyph_x[last] + c->glyph_w[last]
			+ gutil_hint_label_gap(c->glyph[last]));
		/* labels are never truncated */
		assert(c->label == items[c->item].label);
		assert(c->label_w == gutil_font_width(items[c->item].label, 16));
		assert(c->rect.x + c->rect.width == c->label_x + c->label_w);
		/* the pointer rect is the drawn rect */
		assert(gutil_hint_row_hit(r, c->rect.x + c->rect.width / 2.0F,
								  c->rect.y + 14.0F) == c->item);
		if(k + 1 < r->count)
			assert(gutil_hint_row_hit(r, c->rect.x + c->rect.width + r->gap / 2.0F,
									  c->rect.y + 14.0F) == -1);
		at = c->rect.x + c->rect.width + r->gap;
	}
	assert(at - r->gap + 8 == r->strip.x + r->strip.width);
	for(int k = 0; k < r->count; k++)
		if(k) assert(r->cells[k].item > r->cells[k - 1].item);
}
static void check_hint_row_helper(void) {
	static const char* fonts[] = {"assets/font/default.png",
		"packs/isabella-ii/font/default.png",
		"texturepacks/beta-1.7.3/font/default.png"};
	/* the approved cells per family (screens.py PADS, final_screens.py keycaps) */
	enum { PICK, SPLIT, QUICK, PREV, NEXT, SWAP, CLOSE, DROP, ROLES };
	static const enum input_button role[ROLES] = {IB_GUI_CLICK, IB_GUI_SPLIT,
		IB_GUI_QUICK_MOVE, IB_GUI_PREVIOUS, IB_GUI_PAGE, IB_GUI_PAGE_SWAP,
		IB_INVENTORY, IB_GUI_DROP};
	static const int cells[3][ROLES] = {
		{0, 1, 8, 6, 5, 9, 2, 3},       /* Wii Remote + Nunchuk */
		{10, 11, 13, 22, 23, 14, 12, 6}, /* Classic (keycaps L R ZL) */
		{32, 33, 34, 37, 38, 41, 35, 36}}; /* GameCube */
	/* the plan's widest rows (B2), Close last and kept */
	const struct gutil_hint_item chest[] = {
		{{role[PICK]}, 1, "Pick", 4, false}, {{role[SPLIT]}, 1, "Split", 3, false},
		{{role[QUICK]}, 1, "Quick Move", 2, false},
		{{role[PREV], role[NEXT]}, 2, "Section", 1, false},
		{{role[CLOSE]}, 1, "Close", 0, true}};
	const struct gutil_hint_item craft[] = {
		{{role[SWAP]}, 1, "Inventory", 1, false},
		{{role[PREV], role[NEXT]}, 2, "Category", 2, false},
		{{role[PICK]}, 1, "Craft x64", 4, false},
		{{role[SPLIT]}, 1, "Details", 3, false},
		{{role[CLOSE]}, 1, "Close", 0, true}};
	const struct gutil_hint_item inventory[] = {
		{{role[SWAP]}, 1, "Crafting", 1, false},
		{{role[PICK]}, 1, "Pick", 4, false}, {{role[SPLIT]}, 1, "Split", 3, false},
		{{role[DROP]}, 1, "Drop", 2, false}, {{role[CLOSE]}, 1, "Close", 0, true}};
	const struct { const struct gutil_hint_item* items; int n; } rows[] = {
		{chest, 5}, {craft, 5}, {inventory, 5}};
	uint8_t widths[256];
	int checked = 0;
	for(int f = 0; f < 3; f++) {
		if(!hint_load_font(fonts[f], widths)) {
			assert(f == 2); /* only the Beta pack may be absent */
			continue;
		}
		gutil_test_set_font_widths(widths);
		for(int fam = 0; fam < 3; fam++) {
			struct input_player_frame in = {.connected = true};
			for(int b = 0; b < IB_COUNT; b++) in.glyphs[b] = -1;
			for(int k = 0; k < ROLES; k++) in.glyphs[role[k]] = cells[fam][k];
			for(int width = 640; width <= 802; width += 162)
				for(int split = 0; split < 2; split++)
					for(int al = 0; al < 2; al++)
						for(size_t q = 0; q < sizeof rows / sizeof rows[0]; q++) {
							int h = split ? 240 : 480, y = split ? 240 : 0;
							struct gutil_hint_row r;
							assert(gutil_hint_row_layout(&in, 0, y, width, h,
								split, (enum gutil_hint_align)al, rows[q].items,
								rows[q].n, &r));
							hint_row_invariants(&r, rows[q].items, 0, y, width,
								h, split, (enum gutil_hint_align)al);
							/* the plan's real rows fit whole at 16:9; at 4:3 the
							 * crafting row (5 items, an L+R pair) overflows even at
							 * gap 8 in every font and drops its lowest-priority item */
							if(width == 802 || q != 1) assert(r.count == rows[q].n);
							else assert(r.count == rows[q].n - 1 && r.cells[0].item == 1);
							assert(r.cells[r.count - 1].item == rows[q].n - 1);
							if(!split && h == 480)
								assert(r.strip.y == 428); /* Q1: 428-456 */
							if(split)
								assert(r.strip.y - y == 196); /* Q1: 196-224 */
							checked++;
						}
		}
	}
	/* overflow, in order: the gap shrinks to 8 first, then items drop,
	 * lowest priority first (ties: rightmost), Close never */
	struct input_player_frame in = {.connected = true};
	for(int b = 0; b < IB_COUNT; b++) in.glyphs[b] = -1;
	for(int k = 0; k < ROLES; k++) in.glyphs[role[k]] = cells[0][k];
	int base = 0;
	struct gutil_hint_row r;
	assert(gutil_hint_row_layout(&in, 0, 0, 640, 480, false,
		GUTIL_HINT_ALIGN_LEFT, chest, 5, &r) && r.gap == 14);
	for(int k = 0; k < r.count; k++) base += r.cells[k].rect.width;
	/* a frame just too narrow for gap 14 but wide enough for gap 8 */
	int need8 = base + 4 * 8 + 16;
	int width8 = 0;
	for(int w = 200; w <= 1000 && !width8; w++)
		if(w - 2 * gutil_hint_row_margin(w) >= need8) width8 = w;
	assert(width8 && width8 - 2 * gutil_hint_row_margin(width8) < base + 4 * 14 + 16);
	assert(gutil_hint_row_layout(&in, 0, 0, width8, 480, false,
		GUTIL_HINT_ALIGN_LEFT, chest, 5, &r));
	assert(r.count == 5 && r.gap == 8 && r.fits);
	hint_row_invariants(&r, chest, 0, 0, width8, 480, false, GUTIL_HINT_ALIGN_LEFT);
	/* one px narrower: the lowest-priority droppable item (Section, 1) goes */
	assert(gutil_hint_row_layout(&in, 0, 0, width8 - 1, 480, false,
		GUTIL_HINT_ALIGN_LEFT, chest, 5, &r));
	assert(r.fits && r.count == 4);
	for(int k = 0; k < r.count; k++) assert(r.cells[k].item != 3);
	assert(r.cells[r.count - 1].item == 4); /* Close kept */
	hint_row_invariants(&r, chest, 0, 0, width8 - 1, 480, false, GUTIL_HINT_ALIGN_LEFT);
	/* ties drop the rightmost droppable first */
	const struct gutil_hint_item tie[] = {
		{{role[PICK]}, 1, "Pick", 0, false}, {{role[SPLIT]}, 1, "Split", 0, false},
		{{role[QUICK]}, 1, "Quick Move", 0, false},
		{{role[CLOSE]}, 1, "Close", 0, true}};
	assert(gutil_hint_row_layout(&in, 0, 0, 240, 480, false,
		GUTIL_HINT_ALIGN_CENTRE, tie, 4, &r));
	assert(r.fits && r.cells[r.count - 1].item == 3);
	for(int k = 0; k + 1 < r.count; k++) assert(r.cells[k].item == k);
	assert(r.count < 4);
	hint_row_invariants(&r, tie, 0, 0, 240, 480, false, GUTIL_HINT_ALIGN_CENTRE);
	/* only kept items left and still too wide: laid out whole, fits false */
	assert(gutil_hint_row_layout(&in, 0, 0, 60, 480, false,
		GUTIL_HINT_ALIGN_LEFT, tie, 4, &r));
	assert(!r.fits && r.count == 1 && r.cells[0].item == 3 && r.gap == 8);
	assert(r.cells[0].label_w == gutil_font_width("Close", 16));
	/* glyphs come only from the owner's frame: an unbound role is left out */
	in.glyphs[role[QUICK]] = -1;
	assert(gutil_hint_row_layout(&in, 0, 0, 802, 480, false,
		GUTIL_HINT_ALIGN_LEFT, chest, 5, &r) && r.count == 4);
	for(int k = 0; k < r.count; k++) assert(r.cells[k].item != 2);
	in.glyphs[role[QUICK]] = cells[0][QUICK];
	/* a pair with one unbound half draws the bound half alone */
	in.glyphs[role[PREV]] = -1;
	assert(gutil_hint_row_layout(&in, 0, 0, 802, 480, false,
		GUTIL_HINT_ALIGN_LEFT, chest, 5, &r) && r.count == 5);
	assert(r.cells[3].glyph_count == 1 && r.cells[3].glyph[0] == cells[0][NEXT]);
	in.glyphs[role[PREV]] = cells[0][PREV];
	/* a chord draws at gutil_prompt_glyph's width: 24 + 12 + 24 */
	in.glyphs[role[SPLIT]] = ((3 + 1) << 6) | 1;
	assert(gutil_hint_row_layout(&in, 0, 0, 802, 480, false,
		GUTIL_HINT_ALIGN_LEFT, chest, 5, &r));
	assert(r.cells[1].glyph_w[0] == 60);
	hint_row_invariants(&r, chest, 0, 0, 802, 480, false, GUTIL_HINT_ALIGN_LEFT);
	in.glyphs[role[SPLIT]] = cells[0][SPLIT];
	/* no items / no frame: nothing */
	assert(!gutil_hint_row_layout(&in, 0, 0, 802, 480, false,
		GUTIL_HINT_ALIGN_LEFT, chest, 0, &r) && r.count == 0);
	assert(!gutil_hint_row_layout(NULL, 0, 0, 802, 480, false,
		GUTIL_HINT_ALIGN_LEFT, chest, 5, &r) && r.strip.width == 0);
	/* the draw paints exactly the layout: strip, then each glyph */
	assert(gutil_hint_row_layout(&in, 0, 0, 640, 480, false,
		GUTIL_HINT_ALIGN_LEFT, chest, 5, &r));
	used = 0; calls[0] = '\0'; recording = true;
	gutil_hint_row_draw(&r);
	recording = false;
	/* 22978b7fd (maintainer 10-31): no backing strip quad; glyphs only. */
	assert(!quad_at(r.strip.x, r.strip.y) && "the hint row must not draw a strip");
	for(int k = 0; k < r.count; k++)
		for(int g = 0; g < r.cells[k].glyph_count; g++)
			assert(quad_at(r.cells[k].glyph_x[g], r.glyph_y));
	gutil_reset_font(&texture_font);
	printf("PASS hint row helper: %d layouts (3 fonts x 3 families x 802/640 x"
		   " full/split x left/centre x 3 rows), overflow order, draw == layout\n",
		   checked);
}

/* Classic shoulder keycaps (maintainer 10-31): blank caps 22/23/14/15,
 * letters from the ACTIVE pack font, inside the face x 4-28, y 6-22 at 32. */
static void check_classic_keycap_letters(void) {
	static const char* fonts[] = {"assets/font/default.png",
		"packs/isabella-ii/font/default.png",
		"texturepacks/beta-1.7.3/font/default.png"};
	static const int cells[4] = {22, 23, 14, 15};
	static const char* want[4] = {"L", "R", "ZL", "ZR"};
	assert(!gutil_keycap_label(0) && !gutil_keycap_label(10)
		   && !gutil_keycap_label(32));
	uint8_t widths[256];
	int fonts_used = 0;
	for(int f = 0; f < 3; f++) {
		if(!hint_load_font(fonts[f], widths)) { assert(f == 2); continue; }
		gutil_test_set_font_widths(widths);
		fonts_used++;
		for(int k = 0; k < 4; k++) {
			assert(!strcmp(gutil_keycap_label(cells[k]), want[k]));
			for(int size = 16; size <= 32; size += 8) {
				int x, y, w, h;
				assert(gutil_keycap_label_rect(cells[k], size, &x, &y, &w, &h));
				/* the REAL ink: columns from the measured widths, rows from
				 * the pack's pixels (a pack may draw taller letters) */
				int scale = size / 2, top = 8, bottom = -1;
				for(const char* c = want[k]; *c; c++) {
					unsigned char u = (unsigned char)*c;
					if(font_ink_top[u] < top) top = font_ink_top[u];
					if(font_ink_bottom[u] > bottom) bottom = font_ink_bottom[u];
				}
				int ink_y0 = y + top * scale / 8;
				int ink_y1 = y + (bottom + 1) * scale / 8;
				assert(w > 0 && bottom >= top);
				assert(x >= 4 * size / 32 && x + w <= 28 * size / 32);
				assert(ink_y0 >= 6 * size / 32 && ink_y1 <= 22 * size / 32);
				/* centred: left and right margins differ by at most 1 px */
				assert(abs(x - (size - x - w)) <= 1);
			}
		}
		/* drawn through the normal text path, at the rect, (20,20,20) x gray */
		uint8_t* saved = texture_approved_controls.data;
		static uint8_t fake_texels[4];
		texture_approved_controls.data = fake_texels;
		int x, y, w, h;
		assert(gutil_keycap_label_rect(14, 24, &x, &y, &w, &h));
		used = 0; calls[0] = '\0'; recording = true;
		gutil_prompt_glyph(100, 200, 24, 14);
		recording = false;
		char needle[64];
		snprintf(needle, sizeof needle, "quads 4 | %d,%d,",
				 100 + x, 200 + y);
		/* z is whatever layer the caller left (not part of the rule) */
		assert(strstr(calls, needle) && strstr(strstr(calls, needle), " 20,20,20,255 ")
			   && strstr(calls, "bind font"));
		used = 0; calls[0] = '\0'; recording = true;
		gutil_prompt_glyph_gray(100, 200, 24, 14, 128);
		recording = false;
		snprintf(needle, sizeof needle, "| %d,%d,", 100 + x, 200 + y);
		assert(strstr(calls, needle) && strstr(strstr(calls, needle), " 10,10,10,255 "));
		/* a non-keycap cell draws no letters */
		used = 0; calls[0] = '\0'; recording = true;
		gutil_prompt_glyph(100, 200, 24, 10);
		recording = false;
		assert(!strstr(calls, "bind font"));
		/* the fallback controls.png keeps its own art: no overlay */
		texture_approved_controls.data = NULL;
		used = 0; calls[0] = '\0'; recording = true;
		gutil_prompt_glyph(100, 200, 24, 14);
		recording = false;
		assert(strstr(calls, "bind controls") && !strstr(calls, "bind font"));
		texture_approved_controls.data = saved;
	}
	assert(fonts_used >= 2);
	gutil_reset_font(&texture_font);
	printf("PASS Classic keycap letters: L R ZL ZR inside the face at 16/24/32"
		   " in %d pack fonts, (20,20,20) x gray, none on the fallback atlas\n",
		   fonts_used);
}

/* A3 (10-31): notice card A -- every child (picture/battery, text, glyphs,
 * action labels) on the card's ONE centre line, card 32 tall at (24,16). */
struct gutil_test_notice_draw { int x, y, w, h; char text[48];
	int children; struct { int x, y, w, h; } child[8]; };
extern struct gutil_test_notice_draw gutil_test_last_notice;
/* PC glyphs (10-31, ctl1031-pc-glyphs style A): PC codes draw the blank
 * 1u / wide cap with the key NAME (input_glyph_label_for_code) or a mouse
 * cell; the wide cap is 2x wide everywhere it is measured; without the
 * approved atlas they fall back to controls.png's KEY 42 / MSE 43 cells with
 * no label. */
static void check_pc_cap_glyphs(void) {
	int key_w = INPUT_GLYPH_PC_KEY_BASE + 'W', space = INPUT_GLYPH_PC_KEY_BASE + 32;
	int f3 = INPUT_GLYPH_PC_KEY_BASE + 292, lmb = INPUT_GLYPH_PC_MOUSE_LEFT;
	bool wide = false;
	assert(!strcmp(input_glyph_label_for_code(space, &wide), "Space") && wide);
	assert(!strcmp(input_glyph_label_for_code(key_w, &wide), "W") && !wide);
	uint8_t* saved = texture_approved_controls.data;
	static uint8_t fake_texels[4];
	texture_approved_controls.data = fake_texels;
	assert(gutil_prompt_glyph_width(key_w, 24) == 24);
	assert(gutil_prompt_glyph_width(space, 24) == 48);
	assert(gutil_prompt_glyph_width(lmb, 24) == 24);
	/* drawn width == measured width; the wide cap samples 64x32 (cells 45+46) */
	used = 0; calls[0] = '\0'; recording = true;
	assert(gutil_prompt_glyph(100, 200, 24, space) == 48);
	recording = false;
	assert(strstr(calls, "bind approved") && strstr(calls, "bind font"));
	char needle[64];
	snprintf(needle, sizeof needle, "| 148,200,");
	assert(strstr(calls, needle) && "the wide cap is not 48 px wide");
	/* the name sits inside the cap face, centred */
	static const int codes[] = {'W', 32, 292, 340, 257, 266};
	for(size_t k = 0; k < sizeof codes / sizeof codes[0]; k++) {
		int code = INPUT_GLYPH_PC_KEY_BASE + codes[k];
		const char* label = input_glyph_label_for_code(code, &wide);
		int cap = wide ? 48 : 24, x, y, w, h, scale;
		assert(gutil_pc_cap_label_rect(label, cap, 24, &x, &y, &w, &h, &scale));
		assert(scale == 12 || scale == 6);
		assert(x >= 0 && x + w <= cap && abs(x - (cap - x - w)) <= 1);
		assert(y >= 0 && y + h <= 24);
	}
	/* a mouse cell draws no name */
	used = 0; calls[0] = '\0'; recording = true;
	assert(gutil_prompt_glyph(100, 200, 24, lmb) == 24);
	recording = false;
	assert(!strstr(calls, "bind font"));
	/* a hint row measures the wide cap at 48 */
	struct input_player_frame in = {.connected = true};
	for(int b = 0; b < IB_COUNT; b++) in.glyphs[b] = -1;
	in.glyphs[IB_JUMP] = space; in.glyphs[IB_INVENTORY] = key_w;
	struct gutil_hint_item items[2] = {{{IB_JUMP}, 1, "Jump", 1, false},
		{{IB_INVENTORY}, 1, "Back", 0, true}};
	struct gutil_hint_row row;
	assert(gutil_hint_row_layout(&in, 0, 0, 640, 480, false, GUTIL_HINT_ALIGN_LEFT,
								 items, 2, &row) && row.count == 2);
	assert(row.cells[0].rect.width == 48 + gutil_hint_label_gap(space)
		   + gutil_font_width("Jump", GUTIL_HINT_TEXT));
	/* the fallback atlas: generic KEY / MSE cells, no name, 1x wide */
	texture_approved_controls.data = NULL;
	assert(gutil_prompt_glyph_width(space, 24) == 24);
	used = 0; calls[0] = '\0'; recording = true;
	assert(gutil_prompt_glyph(100, 200, 24, f3) == 24);
	recording = false;
	assert(strstr(calls, "bind controls") && !strstr(calls, "bind font"));
	snprintf(needle, sizeof needle, " %d,%d |", (42 % 8) * 32, (42 / 8) * 32);
	assert(strstr(calls, needle) && "fallback key cap is not cell 42");
	used = 0; calls[0] = '\0'; recording = true;
	gutil_prompt_glyph(100, 200, 24, lmb);
	recording = false;
	snprintf(needle, sizeof needle, " %d,%d |", (43 % 8) * 32, (43 / 8) * 32);
	assert(strstr(calls, needle) && "fallback mouse is not cell 43");
	texture_approved_controls.data = saved;
	puts("PASS PC glyphs: named caps (1u 24 / wide 48 at 24 px) with the key "
		 "name inside the face, mouse cells unlabelled, rows measure the wide "
		 "cap, fallback atlas draws KEY 42 / MSE 43 with no name");
}

static void check_notice_card_centre(void) {
	static struct input_inventory inv;
	memset(&inv, 0, sizeof inv);
	struct input_device p1 = {INPUT_DEVICE_WPAD, 0}, p2 = {INPUT_DEVICE_WPAD, 1};
	struct input_device_status* d1
		= (struct input_device_status*)input_inventory_device(&inv, p1);
	struct input_device_status* d2
		= (struct input_device_status*)input_inventory_device(&inv, p2);
	assert(d1 && d2 && d1 != d2);
	d1->published = (struct input_identity) {true, INPUT_CAT_NUNCHUK};
	d1->published_assigned = true; d1->published_player = 0;
	d2->published = (struct input_identity) {true, INPUT_CAT_CLASSIC_CONTROLLER};
	static const enum controller_event events[] = {CONTROLLER_EVENT_DISCONNECTED,
		CONTROLLER_EVENT_CONNECTED, CONTROLLER_EVENT_RECONNECTED,
		CONTROLLER_EVENT_LOW_BATTERY, CONTROLLER_EVENT_ATTACHMENT_MISSING,
		CONTROLLER_EVENT_DETECTED, CONTROLLER_EVENT_JOIN_FAILED,
		CONTROLLER_EVENT_ATTACHMENT_CHANGED};
	gutil_join_inventory_override(&inv);
	int drawn = 0, with_actions = 0, with_picture = 0;
	for(size_t e = 0; e < sizeof events / sizeof events[0]; e++) {
		memset(inv.notices, 0, sizeof inv.notices);
		inv.notices[0] = (struct input_connection_notice) {
			.expires = time_add_ms(time_get(), 60000), .kind = INPUT_DEVICE_WPAD,
			.channel = 1, .player = 1, .event = (uint8_t)events[e],
			.category = INPUT_CAT_CLASSIC_CONTROLLER,
			.previous_category = INPUT_CAT_CLASSIC_CONTROLLER, .active = 1,
			.connected = events[e] != CONTROLLER_EVENT_DISCONNECTED};
		for(int split = 0; split < 2; split++) {
			memset(&gutil_test_last_notice, 0, sizeof gutil_test_last_notice);
			int W = 802, H = split ? 240 : 480;
			gutil_connection_notices(1, split, 16, 8, W - 32, H - 8);
			const struct gutil_test_notice_draw* n = &gutil_test_last_notice;
			if(!n->w) continue;
			drawn++;
			assert(n->x == GUTIL_NOTICE_ANCHOR_X && n->y == GUTIL_NOTICE_ANCHOR_Y);
			assert(n->h == GUTIL_NOTICE_CARD_H);
			assert(n->children >= 1);
			int cy2 = 2 * n->y + n->h; /* doubled centre, exact for odd heights */
			for(int k = 0; k < n->children; k++) {
				int c2 = 2 * n->child[k].y + n->child[k].h;
				assert(abs(c2 - cy2) <= 2 && "a card child is off the centre line");
				assert(n->child[k].x >= n->x && n->child[k].x + n->child[k].w
					   <= n->x + n->w);
				assert(n->child[k].y >= n->y && n->child[k].y + n->child[k].h
					   <= n->y + n->h);
			}
			with_actions += n->children >= 4;
			with_picture += n->children >= 2;
		}
	}
	gutil_join_inventory_override(NULL);
	assert(drawn >= 10 && with_actions >= 2 && with_picture >= 4);
	printf("PASS notice card A: %d cards, every child within 1 px of the 32 px "
		   "card's centre line, at (24,16), full and split\n", drawn);
}

/* A4 (10-31): the disconnect card -- one pure layout for both paths; the
 * Resume hit rectangle is the drawn rectangle, and the card stays small. */
static void check_disconnect_card(void) {
	struct controller_recovery_card lost = {"Controller disconnected",
		"Press 1+2 on the Wii Remote.", "Game paused", "",
		CONTROLLER_PICTURE_NUNCHUK};
	struct controller_recovery_card ready = {"Controller reconnected",
		"Release buttons and centre the stick.", "", "Resume",
		CONTROLLER_PICTURE_NONE};
	struct controller_recovery_card split_lost = {"Controller disconnected",
		"Plug the controller back in.", "World continues", "",
		CONTROLLER_PICTURE_GAMECUBE};
	int chord = ((3 + 1) << 6) | 0;
	for(int W = 640; W <= 802; W += 162)
		for(int split = 0; split < 2; split++) {
			int H = split ? 240 : 480;
			const struct controller_recovery_card* cards[3]
				= {&lost, &ready, &split_lost};
			for(int k = 0; k < 3; k++)
				for(int g = 0; g < 2; g++) {
					bool resume = cards[k]->action[0] != 0;
					int glyph = g ? chord : 0;
					struct recovery_card_layout l;
					recovery_card_layout(0, 0, W, H, split, cards[k], resume,
										 glyph, &l);
					/* centred, inside the pane with its 2 px edge, small */
					assert(abs(l.card.x - (W - l.card.x - l.card.width)) <= 1);
					assert(abs(l.card.y - (H - l.card.y - l.card.height)) <= 1);
					assert(l.card.x >= 2 && l.card.y >= 2);
					assert(l.card.height <= (split ? 140 : 160));
					assert(l.card.width <= W - 64);
					if(!split && !resume) assert(l.card.height == 92);
					/* the picture (64 full / 48 split) sits inside the card,
					 * left of the text, on the body's centre line */
					if(cards[k]->picture != CONTROLLER_PICTURE_NONE) {
						assert(l.picture.width > 0 && l.picture.height == (split ? 48 : 64));
						assert(l.picture.x > l.card.x && l.picture.y > l.card.y
							   && l.picture.y + l.picture.height < l.card.y + l.card.height);
						assert(l.picture.x + l.picture.width + 16 == l.text_x);
					}
					/* the rows share one pitch and stay inside the card */
					for(int r = 0; r < l.rows; r++) {
						assert(l.row_y[r] >= l.card.y && l.text_x > l.card.x);
						assert(l.text_x + gutil_font_width(r == 0 ? cards[k]->title
							: r == 1 ? cards[k]->detail : cards[k]->status, 16)
							<= l.card.x + l.card.width);
						if(r) assert(l.row_y[r] - l.row_y[r - 1] == 20);
					}
					if(!resume) { assert(!l.has_button && l.button.width == 0); continue; }
					/* hit rect == drawn rect: draw, then find the button quad */
					assert(l.has_button);
					assert(l.button.x >= l.card.x && l.button.x + l.button.width
						   <= l.card.x + l.card.width);
					assert(l.button.y + l.button.height <= l.card.y + l.card.height);
					/* (was check_recovery_card_spacing on the old 460 px
					 * panel) the last text row's letters clear Resume */
					assert(l.row_y[l.rows - 1] + 14 + 8 <= l.button.y);
					assert(l.glyph_y + 12 == l.label_y + 7); /* one centre line */
					used = 0; calls[0] = '\0'; recording = true;
					recovery_card_draw(&l, cards[k], glyph, false);
					recording = false;
					assert(quad_at(l.button.x, l.button.y));
					assert(quad_at(l.card.x, l.card.y));
					assert(gutil_button_hit(&l.button, l.button.x + 1.0F,
											l.button.y + 1.0F));
					assert(!gutil_button_hit(&l.button, l.button.x - 1.0F,
											 l.button.y));
				}
		}
	/* both paths call the one layout; the old fixed recovery buttons are gone */
	char* a = read_source("source/game/gui/screen.c");
	char* b = read_source("source/game/gui/local_menu_host.c");
	assert(strstr(a, "recovery_card_layout(0,0,width,height,false,"));
	assert(strstr(b, "recovery_card_layout(0,0,width,height,true,"));
	assert(!strstr(a, "width/2-70,height/2+39") && !strstr(b, "width/2-70,height/2+52"));
	free(a); free(b);
	puts("PASS disconnect card A: one centred layout (92 px lost card full screen), "
		 "Resume hit rect == drawn rect, single and split, 802/640");
}

/* A5 (10-31): coordinates + FPS under the notice card area in every pane,
 * clear of the card (and its edge) and of the sneak marker. */
static void check_coords_fps_rect(void) {
	int cw = gutil_font_width("X: -30000000  Y: 127  Z: -30000000", 16);
	int fw = gutil_font_width("60 FPS", 16);
	for(int W = 640; W <= 802; W += 162)
		for(int H = 240; H <= 480; H += 240)
			for(int mask = 1; mask < 4; mask++) {
				struct gutil_button_bounds r[2];
				int n = screen_ingame_coords_layout(mask & 1, cw, mask & 2, fw, r);
				assert(n == (mask == 3 ? 2 : 1));
				assert(r[0].x == 24 && r[0].y == 52);
				if(n == 2) assert(r[1].y == r[0].y + 18 && r[1].x == 24);
				for(int k = 0; k < n; k++) {
					/* below the card (16..48) and its 2 px edge */
					assert(r[k].y >= GUTIL_NOTICE_ANCHOR_Y + GUTIL_NOTICE_CARD_H + 2);
					assert(r[k].x + r[k].width <= W && r[k].y + r[k].height <= H);
					struct gutil_button_bounds rail, text;
					screen_ingame_sneak_rail_layout(W, H, GFX_GUI_SCALE,
						gutil_font_width("SNEAK", 16), &rail, &text);
					assert(!rects_overlap(r[k].x, r[k].y, r[k].width, r[k].height,
						text.x, text.y, text.width, text.height));
					assert(!rects_overlap(r[k].x, r[k].y, r[k].width, r[k].height,
						rail.x, rail.y, rail.width, rail.height));
					struct screen_ingame_hud_layout L;
					screen_ingame_hud_layout(W, H, &L);
					assert(!rects_overlap(r[k].x, r[k].y, r[k].width, r[k].height,
						L.hearts.x, L.hearts.y, L.hearts.width, L.hearts.height));
				}
			}
	/* the F3 block position: floor of the feet */
	int bp[3];
	screen_ingame_block_pos((const float[3]) {-0.5F, 64.5F + 1.62F /* PLAYER_EYE_HEIGHT */, 12.99F}, bp);
	assert(bp[0] == -1 && bp[1] == 64 && bp[2] == 12);
	/* config: true/false and 1/0, absent = true */
	struct config cfg = {0};
	cfg.root = json_value_init_object();
	JSON_Object* root = json_object(cfg.root);
	assert(screen_ingame_ui_flag(&cfg, "ui.show_coordinates"));
	json_object_dotset_boolean(root, "ui.show_coordinates", 0);
	assert(!screen_ingame_ui_flag(&cfg, "ui.show_coordinates"));
	json_object_dotset_number(root, "ui.show_fps", 0);
	assert(!screen_ingame_ui_flag(&cfg, "ui.show_fps"));
	json_object_dotset_number(root, "ui.show_fps", 1);
	assert(screen_ingame_ui_flag(&cfg, "ui.show_fps"));
	json_value_free(cfg.root);
	/* shipped explicitly in config_wii.json and both presets */
	static const char* files[] = {"config_wii.json",
		"tools/presets/config_wii_developer.json",
		"tools/presets/config_wii_pointer_look.json"};
	for(int f = 0; f < 3; f++) {
		char* src = read_source(files[f]);
		assert(strstr(src, "\"show_coordinates\": true") && strstr(src, "\"show_fps\": true"));
		free(src);
	}
	char* src = read_source("source/game/gui/screen_ingame.c");
	assert(strstr(src, "bool hud_shown = !gstate.debug_overlay && !hud_modal"));
	free(src);
	puts("PASS coords + FPS: (24,52)/(24,70) in every pane, clear of the notice "
		 "card, the sneak marker and the hearts; F3 rounding; ui.show_* flags");
}

static void check_hint_row_rules(void) {
#define ROW(ctx, what, ...) do { \
	const struct { int b; const char* l; } r_[] = {__VA_ARGS__}; \
	const char* l_[SCREEN_INGAME_HINT_MAX]; int b_[SCREEN_INGAME_HINT_MAX]; \
	int n_ = (int)(sizeof r_ / sizeof r_[0]); \
	for(int i_ = 0; i_ < n_; i_++) { l_[i_] = r_[i_].l; b_[i_] = r_[i_].b; } \
	expect_row(&(ctx), l_, b_, n_, what); } while(0)
	struct screen_ingame_hint_ctx c = {0};
	/* Nothing targeted, empty hand: CavEX's Inventory, Jump, Punch, Pause. */
	ROW(c, "nothing", {IB_OPEN_INVENTORY, "Inventory"}, {IB_JUMP, "Jump"},
		{IB_ACTION1, "Punch"}, {IB_HOME, "Pause"});
	/* Nothing targeted + food: the eat fix's "Eat" (A2), after Punch. */
	c.held = SCREEN_INGAME_HELD_FOOD;
	ROW(c, "nothing+food", {IB_OPEN_INVENTORY, "Inventory"}, {IB_JUMP, "Jump"},
		{IB_ACTION1, "Punch"}, {IB_ACTION2, "Eat"}, {IB_HOME, "Pause"});
	c.held = SCREEN_INGAME_HELD_AIR_USE;
	ROW(c, "nothing+air-use", {IB_OPEN_INVENTORY, "Inventory"}, {IB_JUMP, "Jump"},
		{IB_ACTION1, "Punch"}, {IB_ACTION2, "Use"}, {IB_HOME, "Pause"});
	c.held = SCREEN_INGAME_HELD_BLOCK; /* a block has no open-air use */
	ROW(c, "nothing+block", {IB_OPEN_INVENTORY, "Inventory"}, {IB_JUMP, "Jump"},
		{IB_ACTION1, "Punch"}, {IB_HOME, "Pause"});
	c.held = SCREEN_INGAME_HELD_OTHER;
	ROW(c, "nothing+stick", {IB_OPEN_INVENTORY, "Inventory"}, {IB_JUMP, "Jump"},
		{IB_ACTION1, "Punch"}, {IB_HOME, "Pause"});
	/* A plain block: the held item's Place/Use, then Mine (CavEX order). */
	c = (struct screen_ingame_hint_ctx) {.target = SCREEN_INGAME_HINT_BLOCK,
		.held = SCREEN_INGAME_HELD_BLOCK};
	ROW(c, "block+held block", {IB_OPEN_INVENTORY, "Inventory"}, {IB_JUMP, "Jump"},
		{IB_ACTION2, "Place"}, {IB_ACTION1, "Mine"}, {IB_HOME, "Pause"});
	c.held = SCREEN_INGAME_HELD_FOOD;
	ROW(c, "block+held food", {IB_OPEN_INVENTORY, "Inventory"}, {IB_JUMP, "Jump"},
		{IB_ACTION2, "Use"}, {IB_ACTION1, "Mine"}, {IB_HOME, "Pause"});
	c.held = SCREEN_INGAME_HELD_NONE;
	ROW(c, "block+empty hand", {IB_OPEN_INVENTORY, "Inventory"}, {IB_JUMP, "Jump"},
		{IB_ACTION1, "Mine"}, {IB_HOME, "Pause"});
	/* A usable block (onRightClick): "Use" wins over the held item. */
	c.block_usable = true;
	c.held = SCREEN_INGAME_HELD_BLOCK;
	ROW(c, "usable block", {IB_OPEN_INVENTORY, "Inventory"}, {IB_JUMP, "Jump"},
		{IB_ACTION2, "Use"}, {IB_ACTION1, "Mine"}, {IB_HOME, "Pause"});
	/* An entity: its own left/right texts, nothing else in between. */
	c = (struct screen_ingame_hint_ctx) {.target = SCREEN_INGAME_HINT_ENTITY,
		.entity_left = "Attack", .entity_right = "Ride",
		.held = SCREEN_INGAME_HELD_FOOD};
	ROW(c, "entity", {IB_OPEN_INVENTORY, "Inventory"}, {IB_JUMP, "Jump"},
		{IB_ACTION1, "Attack"}, {IB_ACTION2, "Ride"}, {IB_HOME, "Pause"});
	c.entity_right = NULL;
	ROW(c, "entity left only", {IB_OPEN_INVENTORY, "Inventory"}, {IB_JUMP, "Jump"},
		{IB_ACTION1, "Attack"}, {IB_HOME, "Pause"});
#undef ROW
	/* Per split pane, per device: each pane draws the row from the frame it
	 * is handed (main.c builds it with input_prompt_frame(viewport, ...)),
	 * through the shared hint row laid out from `input` (A2 10-31) -- never
	 * player 0's. */
	{
		char* main_src = read_source("source/main.c");
		const char* pf = strstr(main_src, "input_prompt_frame(viewport, &prompt);");
		const char* call = pf ? strstr(pf, "screen_ingame_render2D_owned(owner, &prompt,") : NULL;
		assert(pf && call && call - pf < 200
			   && "each split pane must render with its OWN prompt frame");
		free(main_src);
		char* src = read_source("source/game/gui/screen_ingame.c");
		const char* fn = strstr(src, "void screen_ingame_render2D_owned(");
		const char* row = fn ? strstr(fn, "screen_ingame_hint_row(&hc, hints)") : NULL;
		/* 1653f0c74: the gameplay row yields to the MP chat socket through
		 * screen_ingame_gameplay_hint_layout, which lays out from the SAME
		 * pane `input` (pinned in the helper itself). */
		assert(row && strstr(row, "screen_ingame_gameplay_hint_layout(input, width, height,")
			   && "the row must draw from the pane's own `input`");
		const char* helper = strstr(src, "bool screen_ingame_gameplay_hint_layout(");
		assert(helper && helper < fn
			   && strstr(helper, "gutil_hint_row_layout(input, 0, 0, width, height,")
			   && "the gameplay hint helper must lay out from the pane's own `input`");
		free(src);
	}
	puts("PASS CavEX hint row rules: nothing/food/air-use/block/usable block/"
		 "entity content + order, per-pane device frame");
}

/* ---- 4a'. hint-audit-1030: every hint-row button == that pad's binding --
 * The 2026-10-30 real-Wii still showed "(1) Inventory": the row named the menu
 * CLOSE role. This walks every row variant for all three shipped families and
 * asserts each label's glyph is exactly the glyph of the button
 * docs/controls_sheet.md's machine block gives that action on that pad, with
 * the glyph resolved the way the game does it (input_glyph_frame over the
 * config_wii.json bindings). Any drift -- a wrong role in the row, a config
 * edit, or a sheet edit -- fails here and names family, label and both cells. */
/* The sheet's JSON has a duplicate key on purpose-free prose (player_drop
 * twice under unbound_by_design), which strict parsers refuse; the codes are
 * read textually from the family's own "roles" section instead. */
static size_t hint_sheet_codes(const char* block, const char* fam,
							   const char* role, int* out, size_t cap) {
	static const char* const order[] = {"\"wii_remote_nunchuk\": {",
		"\"classic_controller\": {", "\"gamecube\": {", "\"shared_codes\""};
	char want[64];
	snprintf(want, sizeof want, "\"%s\": {", fam);
	const char* start = strstr(block, want);
	assert(start && "family missing from the sheet");
	const char* end = NULL;
	for(size_t i = 0; i < 4; i++) {
		const char* e = strstr(start + 1, order[i]);
		if(e && (!end || e < end)) end = e;
	}
	assert(end);
	char key[96];
	snprintf(key, sizeof key, "\"%s\": {\"codes\": [", role);
	const char* at = strstr(start, key);
	assert(at && at < end && "role missing from the family's sheet section");
	at += strlen(key);
	size_t n = 0;
	while(*at && *at != ']' && n < cap) {
		char* next;
		long v = strtol(at, &next, 10);
		if(next == at) { at++; continue; }
		out[n++] = (int)v;
		at = next;
	}
	return n;
}
static const char* hint_sheet_role(const char* label, int button) {
	if(!strcmp(label, "Inventory")) return "open_inventory";
	if(!strcmp(label, "Jump")) return "player_jump";
	if(!strcmp(label, "Pause")) return "open_menu";
	if(!strcmp(label, "Punch") || !strcmp(label, "Mine")) return "item_action_left";
	if(!strcmp(label, "Place") || !strcmp(label, "Use") || !strcmp(label, "Eat"))
		return "item_action_right";
	/* entity texts: whichever click the row put them on */
	return button == IB_ACTION1 ? "item_action_left"
		: button == IB_ACTION2 ? "item_action_right" : NULL;
}

static void check_hint_row_bindings(void) {
	/* the sheet's machine block */
	char* sheet = read_source("docs/controls_sheet.md");
	char* open = strstr(sheet, "## Machine-readable binding block");
	open = open ? strstr(open, "```json\n") : NULL;
	assert(open && "controls sheet machine block missing");
	open += 8;
	char* close = strstr(open, "\n```");
	assert(close);
	*close = '\0';
	const char* block = open;
	/* the shipped bindings, read the way input.c reads them */
	struct config cfg;
	assert(config_create(&cfg, "config_wii.json"));
	static const struct { enum input_button b; const char* key; } keys[] = {
		{IB_OPEN_INVENTORY, "input.open_inventory"},
		{IB_JUMP, "input.player_jump"},
		{IB_ACTION1, "input.item_action_left"},
		{IB_ACTION2, "input.item_action_right"},
		{IB_HOME, "input.open_menu"}};
	int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION] = {{0}};
	size_t lengths[IB_COUNT] = {0};
	for(size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
		lengths[keys[k].b] = INPUT_BINDINGS_PER_ACTION;
		assert(config_read_int_array(&cfg, keys[k].key, map[keys[k].b],
									 &lengths[keys[k].b]));
	}
	/* every action the row can name is in the shared table and bound here */
	for(int a = 0; a < SCREEN_INGAME_ACT_COUNT; a++) {
		bool found = false;
		for(size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
			found |= keys[k].b == screen_ingame_action_role[a];
		assert(found && "a hint action's role has no shipped binding key here");
	}
	static const struct { enum input_category cat; const char* fam; } fams[] = {
		{INPUT_CAT_NUNCHUK, "wii_remote_nunchuk"},
		{INPUT_CAT_CLASSIC_CONTROLLER, "classic_controller"},
		{INPUT_CAT_GAMECUBE, "gamecube"}};
	static const struct screen_ingame_hint_ctx ctxs[] = {
		{.target = SCREEN_INGAME_HINT_NONE, .held = SCREEN_INGAME_HELD_FOOD},
		{.target = SCREEN_INGAME_HINT_NONE, .held = SCREEN_INGAME_HELD_AIR_USE},
		{.target = SCREEN_INGAME_HINT_BLOCK, .held = SCREEN_INGAME_HELD_BLOCK},
		{.target = SCREEN_INGAME_HINT_BLOCK, .block_usable = true},
		{.target = SCREEN_INGAME_HINT_ENTITY, .entity_left = "Attack",
		 .entity_right = "Ride"}};
	int checked = 0;
	for(size_t f = 0; f < sizeof fams / sizeof fams[0]; f++) {
		struct input_player_frame frame = {.category = fams[f].cat};
		input_glyph_frame(&frame, map, lengths);
		for(size_t c = 0; c < sizeof ctxs / sizeof ctxs[0]; c++) {
			struct screen_ingame_hint h[SCREEN_INGAME_HINT_MAX];
			int n = screen_ingame_hint_row(&ctxs[c], h);
			for(int k = 0; k < n; k++) {
				const char* role = hint_sheet_role(h[k].label, h[k].button);
				assert(role && "hint label with no sheet action");
				int sheet_codes[INPUT_BINDINGS_PER_ACTION];
				size_t sc = hint_sheet_codes(block, fams[f].fam, role,
					sheet_codes, INPUT_BINDINGS_PER_ACTION);
				assert(sc > 0 && "the sheet leaves a hinted action unbound");
				int want = input_glyph_binding(fams[f].cat, sheet_codes, sc);
				int got = frame.glyphs[h[k].button];
				if(want < 0 || got != want) {
					printf("  FAIL hint row %s: \"%s\" draws cell %d, the sheet's "
						   "%s is cell %d\n", fams[f].fam, h[k].label, got,
						   role, want);
					assert(false && "hint-row button != the pad's binding");
				}
				checked++;
			}
		}
	}
	/* the reported bug, by name: the Remote's Inventory is A, not 1 */
	{
		struct input_player_frame frame = {.category = INPUT_CAT_NUNCHUK};
		input_glyph_frame(&frame, map, lengths);
		struct screen_ingame_hint h[SCREEN_INGAME_HINT_MAX];
		struct screen_ingame_hint_ctx c0 = {0};
		assert(screen_ingame_hint_row(&c0, h) > 0
			   && !strcmp(h[0].label, "Inventory"));
		assert(frame.glyphs[h[0].button] == input_glyph_key(INPUT_CAT_NUNCHUK, 4)
			   && "Wii Remote Inventory must be A (code 4)");
		assert(frame.glyphs[h[0].button] != input_glyph_key(INPUT_CAT_NUNCHUK, 6)
			   && "Wii Remote Inventory drew Button 1 again");
	}
	config_destroy(&cfg);
	free(sheet);
	printf("PASS hint row == controls sheet: %d label/button pairs across "
		   "Remote+Nunchuk, Classic, GameCube match config_wii.json and "
		   "docs/controls_sheet.md\n", checked);
}

/* Lead ruling 2026-10-29: the online Leave Bed prompt clears the restored
 * hotbar, the hearts/armour/air block and the bottom hint row, single and
 * split, with and without a glyph. */
static void check_leave_bed_clearance(void) {
	static const struct { int w, h; } views[] = {
		{640, 480}, {854, 480}, {640, 240}, {854, 240}};
	const int s = GFX_GUI_SCALE;
	for(size_t v = 0; v < sizeof views / sizeof views[0]; v++) {
		int W = views[v].w, H = views[v].h;
		for(int gw = 0; gw <= 80; gw += 32) {
			struct gutil_button_bounds cell, label;
			int lw = gutil_font_width("Leave Bed", 16);
			screen_ingame_leave_bed_layout(W, H, gw, lw, &cell, &label);
			int top = screen_ingame_hotbar_top(H, s), left = (W - 182 * s) / 2;
			int survival_top = (top - 10 * s) - 9 * s;
			struct gutil_button_bounds band = screen_ingame_row_band(W, H);
			int row_y = band.y;
			const struct gutil_button_bounds* parts[2] = {&cell, &label};
			/* Q5 (10-31): 24 px glyph, the label on its centre line */
			assert(cell.height == 24 && cell.y + 12 == label.y + 7);
			for(int p = 0; p < 2; p++) {
				const struct gutil_button_bounds* r = parts[p];
				if(!r->width) continue;
				assert(r->y + r->height <= top - 40
					   && "Leave Bed must sit 40 px above the hotbar top");
				assert(!rects_overlap(r->x, r->y, r->width, r->height, left,
					top - s, 182 * s, 24 * s) && "Leave Bed over the hotbar");
				assert(!rects_overlap(r->x, r->y, r->width, r->height, left,
					survival_top, 182 * s, top - survival_top)
					   && "Leave Bed over the hearts/armour/air row");
				assert(!rects_overlap(r->x, r->y, r->width, r->height, 0,
					row_y, W, band.height) && "Leave Bed over the hint row");
				assert(r->y >= 0 && r->x >= 0 && r->x + r->width <= W);
			}
			/* The SNEAK label (left of the hotbar, maintainer 2026-10-30)
			 * against the same three neighbours: Leave Bed, the hint row
			 * and the hearts/armour/air block. */
			{
				struct gutil_button_bounds rail, sneak;
				screen_ingame_sneak_rail_layout(W, H, s,
					gutil_font_width("SNEAK", 16), &rail, &sneak);
				for(int p = 0; p < 2; p++) {
					const struct gutil_button_bounds* r = parts[p];
					if(!r->width) continue;
					assert(!rects_overlap(sneak.x, sneak.y, sneak.width,
						sneak.height, r->x, r->y, r->width, r->height)
						   && "the SNEAK label overlaps Leave Bed");
				}
				assert(!rects_overlap(sneak.x, sneak.y, sneak.width,
					sneak.height, 0, row_y, W, band.height)
					   && "the SNEAK label overlaps the hint row");
				assert(sneak.y + sneak.height <= row_y
					   && "the SNEAK label must sit above the hint row");
				assert(!rects_overlap(sneak.x, sneak.y, sneak.width,
					sneak.height, left, survival_top, 182 * s,
					top - survival_top)
					   && "the SNEAK label overlaps the hearts/armour/air row");
				assert(sneak.x >= 0 && "the SNEAK label left the viewport");
			}
			/* centred */
			int lft = gw ? cell.x : label.x, rgt = label.x + label.width;
			assert(abs(lft - (W - rgt)) <= 1 && "Leave Bed must be centred");
		}
	}
	puts("PASS Leave Bed prompt: 40 px above the CavEX hotbar, clear of the "
		 "hotbar, hearts and hint row, single + split");
}

/* ---- 4. gameplay HUD rows: pinned at the call site (screen_ingame.c) ----- */
static void check_hud_source_pin(void) {
	char* src = read_source("source/game/gui/screen_ingame.c");
	const char* fn = strstr(src, "void screen_ingame_render2D_owned(");
	assert(fn);
	/* Controller UI A2 (10-31, WIRING-PLAN Q1): the row strip is 428-456 of
	 * 480 (196-224 of 240) and the hotbar sits 1 px above it: 383 / 151;
	 * hearts/armor 10 units above it = 363 / 131. */
	assert(strstr(fn, "int bar_y = screen_ingame_hotbar_top(height, GFX_GUI_SCALE);"));
	assert(screen_ingame_hotbar_top(480, 2) == 383
		   && screen_ingame_hotbar_top(240, 2) == 151);
	assert(screen_ingame_hotbar_top(480, 2) - 10 * 2 == 363
		   && screen_ingame_hotbar_top(240, 2) - 10 * 2 == 131);
	for(int W = 640; W <= 802; W += 162) {
		struct gutil_button_bounds full = screen_ingame_row_band(W, 480);
		struct gutil_button_bounds half = screen_ingame_row_band(W, 240);
		assert(full.y == 428 && full.height == 28 && half.y == 196
			   && half.height == 28);
		assert(full.x == (W == 802 ? 41 : 32) && full.x + full.width == W - full.x);
		/* the hotbar's bottom edge stays above the strip */
		assert(screen_ingame_hotbar_top(480, 2) + 44 < full.y);
		assert(screen_ingame_hotbar_top(240, 2) + 44 < half.y);
		/* the exposed HUD rectangles (Q3) never overlap each other */
		for(int H = 240; H <= 480; H += 240) {
			struct screen_ingame_hud_layout L;
			screen_ingame_hud_layout(W, H, &L);
			assert(L.hotbar.y == screen_ingame_hotbar_top(H, 2) - 2);
			assert(L.hearts.y + L.hearts.height <= L.hotbar.y);
			/* Q1 as approved: the 24-unit selection frame reaches 1 px
			 * into the strip's top row (bar body itself clears it) */
			assert(L.hotbar.y + L.hotbar.height == L.row.y + 1);
			assert(L.hearts.y == screen_ingame_hotbar_top(H, 2) - 38);
		}
	}
	/* items +6, selection -2, health/armor row -22 -- menus.html:399/400 */
	assert(strstr(fn, "bar_y + 3 * GFX_GUI_SCALE, 0);"));
	assert(strstr(fn, "bar_y - GFX_GUI_SCALE, 0, 22, 24, 24,"));
	assert(strstr(fn, "int heart_row_y = bar_y - 10 * GFX_GUI_SCALE;"));
	assert(strstr(fn, "hud_survival_row_draw_owned(owner, hud_left, heart_row_y);"));
	/* The CavEX bottom hint row is back, always on, drawn from this pane's
	 * own frame through gutil_control_icon_owned. */
	/* 1653f0c74: items + LEFT layout now live in screen_ingame_gameplay_hint_layout. */
	assert(strstr(fn, "screen_ingame_hint_row(&hc, hints)")
		   && strstr(src, "screen_ingame_hint_items(hints, count, items);")
		   && strstr(src, "GUTIL_HINT_ALIGN_LEFT, items, count, row))")
		   && strstr(fn, "gutil_hint_row_draw(&row);")
		   && "the restored CavEX hint row is missing");
	free(src);
	/* A2: menus draw hearts + hotbar through hud_menu_draw_owned at exactly
	 * the gameplay anchors and UVs above (placement 1, 10-31). */
	char* hud = read_source("source/game/gui/screen_ingame.c");
	const char* menu = strstr(hud, "void hud_menu_draw_owned(");
	assert(menu);
	assert(strstr(menu, "int bar_y = screen_ingame_hotbar_top(height, GFX_GUI_SCALE);"));
	assert(strstr(menu, "int hud_left = (width - 182 * GFX_GUI_SCALE) / 2;"));
	assert(strstr(menu, "gutil_texquad(hud_left, bar_y, 0, 0, 182, 22, 182 * GFX_GUI_SCALE,"));
	assert(strstr(menu, "bar_y + 3 * GFX_GUI_SCALE, 0);"));
	assert(strstr(menu, "bar_y - GFX_GUI_SCALE, 0, 22, 24, 24,"));
	assert(strstr(menu, "if(!gstate.config_creative)"));
	/* SOL-REVIEW item 4 (10-31): the menu row never draws oxygen bubbles. */
	assert(strstr(menu, "bar_y - 10 * GFX_GUI_SCALE, false);"));
	free(hud);
	puts("PASS gameplay HUD rows pinned: bar_y 383/151 on the 428/196 hint-row strip, items +6, selection -2, hearts 363/131, shared hint row on");
}

/* ---- 4b. the in-game H-C2 CHAT SOCKET (Astra build spec 15 A.1/A.3/A.4;
 * maintainer pick 2026-09-21 "I like Hc 2") -------------------------------
 *
 * RETIRES ruling 8's single-line hint and everything the old check_chat_hint
 * pinned: the 16 px glyph CELL, the 8 px glyph-to-text gap, the 8 px right/
 * bottom inset, the measured single-row box (114 px Remote at x 518..632,
 * 126 px widest remap at x 506..632) and the DIRECTION WORDS ("Up Chat",
 * "Right Chat") that made a remap the widest case. What replaces them: a
 * fixed corner socket -- gui.png body + selection frame -- holding ONE
 * complete official 32x32 cell, with the four D-pad DIRECTIONS drawn as their
 * own cells (25/26/27/28, input_glyph_dpad_direction_cell) instead of the
 * shared cell 24 plus a word. Every single-button binding is now the SAME
 * size, so there is no "widest remap" left to measure.
 *
 * The screen's own layout helper is the measuring instrument and its draw
 * helper is traced, so what is asserted is what is drawn. Kinds: 0 unbound
 * (the menu route), 1 one complete cell, 2 the modifier + button chord. */
extern int screen_ingame_chat_socket_layout(
	const struct input_player_frame* input, int width, int height, int code,
	bool is_tap, struct gutil_button_bounds* body,
	struct gutil_button_bounds* frame, struct gutil_button_bounds* glyph,
	struct gutil_button_bounds* tap, struct gutil_button_bounds* label,
	int cells[2], const char** label_text);
extern void screen_ingame_chat_socket_draw(
	const struct input_player_frame* input, int width, int height, int code,
	bool is_tap);
extern bool screen_ingame_chat_socket_visible(bool remote_session,
	bool owner_can_chat, bool recovery_modal, bool join_prompt, bool capture);
extern bool screen_ingame_chat_socket_owner(unsigned slot);
extern bool screen_ingame_chat_socket_capture(int capture,
	enum input_category* category, int* code, bool* is_tap, int* glyph);

static void chat_socket_frame(enum input_category category, int code,
							  struct input_player_frame* out) {
	*out = (struct input_player_frame) {.connected = true, .category = category};
	for(int b = 0; b < IB_COUNT; b++) out->glyphs[b] = -1;
	/* The REAL glyph resolver, never a table of atlas cells written here. */
	out->glyphs[IB_CHAT] = input_glyph_key(category, code);
}

/* The same frame with an explicitly ENCODED glyph -- a chord built by
 * input_glyph_binding, or the -1 of an unbound action. Still no hand-written
 * atlas cell: the encoding comes from the production resolver either way. */
static void chat_socket_frame_glyph(enum input_category category, int glyph,
									struct input_player_frame* out) {
	*out = (struct input_player_frame) {.connected = true, .category = category};
	for(int b = 0; b < IB_COUNT; b++) out->glyphs[b] = -1;
	out->glyphs[IB_CHAT] = glyph;
}

/* Every socket rectangle as LITERALS, in both viewport heights: "at 640x240,
 * subtract 240 from every y". PLACEMENT PL-B (maintainer 2026-09-21, "B for
 * the (2) and Tap chat"), which supersedes spec 15 A.1's positions: the
 * socket tucks against the hotbar's right end at hotbar height and the
 * label sits BESIDE it, left-aligned at x = W-72. `chat` is the two-line
 * position (under "Tap"); `chat_alone` is the vertically-centred one a
 * non-gesture binding gets. */
static const struct {
	int h;
	int body[4], frame[4], glyph[4], chord[4];
	int tap[2], chat[2], chat_alone[2], menu[2];
} socket_views[2] = {
	{480, {514, 410, 44, 44}, {512, 408, 48, 48}, {520, 416, 32, 32},
	 {516, 424, 40, 16}, {568, 412}, {568, 434}, {568, 424}, {512, 432}},
	{240, {514, 170, 44, 44}, {512, 168, 48, 48}, {520, 176, 32, 32},
	 {516, 184, 40, 16}, {568, 172}, {568, 194}, {568, 184}, {512, 192}},
};

static void check_chat_socket(void) {
	/* The CHORD encoding comes from the production resolver: Classic Minus
	 * (210) + Plus (211) -> cells 22 and 23, packed (22+1)*64+23. */
	static const int classic_chord_key = 210 * 1000 + 211;
	int chord_glyph = input_glyph_binding(INPUT_CAT_CLASSIC_CONTROLLER,
										  &classic_chord_key, 1);
	int chord_mod, chord_btn;
	assert(input_glyph_cells(chord_glyph, &chord_mod, &chord_btn)
		   && chord_mod == 22 && chord_btn == 23);

	static const struct {
		const char* what;
		enum input_category category;
		int code;
		bool is_tap;
		int cell; /* the cell the socket actually draws */
	} cases[] = {
		/* the three SHIPPED defaults (input.c's chat[] fallback). A.1's
		 * binding table: Remote default tap 2 = cell 3; Classic and GameCube
		 * D-pad Up = cell 25 and NO direction word. */
		{"Remote default tap 2", INPUT_CAT_NUNCHUK, 7, true, 3},
		{"Classic D-pad Up", INPUT_CAT_CLASSIC_CONTROLLER, 200, false, 25},
		{"GameCube D-pad Up", INPUT_CAT_GAMECUBE, 400, false, 25},
		/* the other three directions, on both D-pad families */
		{"Classic D-pad Down remap", INPUT_CAT_CLASSIC_CONTROLLER, 201, false, 26},
		{"Classic D-pad Left remap", INPUT_CAT_CLASSIC_CONTROLLER, 202, false, 27},
		{"Classic D-pad Right remap", INPUT_CAT_CLASSIC_CONTROLLER, 203, false, 28},
		{"GameCube D-pad Right remap", INPUT_CAT_GAMECUBE, 403, false, 28},
		/* a remap to an ORDINARY button: its own approved cell, unchanged */
		{"Remote remap to 1", INPUT_CAT_NUNCHUK, 6, false, 2},
		/* an explicitly configured Classic Plus: the release gesture, so Tap */
		{"Classic Plus (tap)", INPUT_CAT_CLASSIC_CONTROLLER, 212, true, 5},
		/* PC: one keyboard family, no gesture, the SAME geometry rule; since
		 * the PC glyphs (4d5cd8bb5) a named key is the blank 1u cap 44 */
		{"PC keyboard T", INPUT_CAT_NONE, 84, false, 44},
	};
	for(size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
		struct input_player_frame frame;
		chat_socket_frame(cases[k].category, cases[k].code, &frame);
		assert(frame.glyphs[IB_CHAT] >= 0 && "the socket case lost its glyph");
		for(int split = 0; split < 2; split++) {
			int W = 640, H = socket_views[split].h;
			struct gutil_button_bounds body, frm, glyph, tap, label;
			int cells[2];
			const char* lab = NULL;
			int kind = screen_ingame_chat_socket_layout(&frame, W, H,
				cases[k].code, cases[k].is_tap, &body, &frm, &glyph, &tap,
				&label, cells, &lab);
			assert(kind == 1 && "an ordinary binding must be a SINGLE cell");
			assert(cells[0] == -1 && "a single binding has no modifier");
			assert(cells[1] == cases[k].cell
				   && "the socket drew the wrong official cell");
			assert(!strcmp(lab, "Chat") && "the socket's label is not Chat");
			check_rect("socket body", &body, socket_views[split].body[0],
					   socket_views[split].body[1], socket_views[split].body[2],
					   socket_views[split].body[3]);
			check_rect("socket frame", &frm, socket_views[split].frame[0],
					   socket_views[split].frame[1],
					   socket_views[split].frame[2],
					   socket_views[split].frame[3]);
			check_rect("binding glyph", &glyph, socket_views[split].glyph[0],
					   socket_views[split].glyph[1],
					   socket_views[split].glyph[2],
					   socket_views[split].glyph[3]);
			/* PL-B: "Chat" sits under "Tap" when the opener is a gesture and
			 * is vertically centred on the socket when it is alone. */
			check_rect("Chat label", &label,
					   cases[k].is_tap ? socket_views[split].chat[0]
									   : socket_views[split].chat_alone[0],
					   cases[k].is_tap ? socket_views[split].chat[1]
									   : socket_views[split].chat_alone[1],
					   gutil_font_width("Chat", 16), 16);
			/* "Tap" ONLY when the resolved opener runs through the gesture
			 * rule; a zero-width rectangle is the "not drawn" answer. */
			if(cases[k].is_tap)
				check_rect("Tap label", &tap, socket_views[split].tap[0],
						   socket_views[split].tap[1],
						   gutil_font_width("Tap", 16), 16);
			else
				assert(tap.width == 0 && tap.height == 0
					   && "Tap was drawn for a plain press edge");
			/* PL-B's two envelope rules, for every mark of this case
			 * INCLUDING the text shadow: nothing passes y = H-24, and no
			 * label passes x = W-16. */
			const struct gutil_button_bounds* marks[5]
				= {&frm, &body, &glyph, &tap, &label};
			for(int m = 0; m < 5; m++) {
				if(!marks[m]->width)
					continue;
				int shadow = m >= 3 ? 2 : 0; /* the two labels only */
				assert(marks[m]->y + marks[m]->height + shadow <= H - 24
					   && "a socket mark passes PL-B's y = H-24 floor");
				assert(marks[m]->x + marks[m]->width + shadow <= W - 16
					   && "a socket mark passes PL-B's x = W-16 edge");
			}
			printf("  MEASURE chat-socket %-27s %-6s cell %2d  frame=(%d,%d "
				   "%dx%d) glyph=(%d,%d %dx%d) tap=(%d,%d %dx%d) "
				   "chat=(%d,%d %dx%d) right edge %d (limit %d), lowest %d "
				   "(floor %d)\n",
				   cases[k].what, split ? "SPLIT" : "single", cells[1], frm.x,
				   frm.y, frm.width, frm.height, glyph.x, glyph.y, glyph.width,
				   glyph.height, tap.x, tap.y, tap.width, tap.height, label.x,
				   label.y, label.width, label.height,
				   label.x + label.width + 2, W - 16,
				   label.y + label.height + 2 > frm.y + frm.height
					   ? label.y + label.height + 2
					   : frm.y + frm.height,
				   H - 24);
		}
	}
	/* A.1's CHORD row: the same socket, the existing modifier + "+" + button
	 * composition at size 16 -- total 40 px -- at its own anchor, and the SAME
	 * Tap/Chat positions. Directional cells never apply to a chord. */
	for(int split = 0; split < 2; split++) {
		int W = 640, H = socket_views[split].h;
		struct input_player_frame frame;
		struct gutil_button_bounds body, frm, glyph, tap, label;
		int cells[2];
		const char* lab = NULL;
		chat_socket_frame_glyph(INPUT_CAT_CLASSIC_CONTROLLER, chord_glyph,
								&frame);
		int kind = screen_ingame_chat_socket_layout(&frame, W, H, 211, true,
			&body, &frm, &glyph, &tap, &label, cells, &lab);
		assert(kind == 2 && "the chord case is not a chord");
		assert(cells[0] == chord_mod && cells[1] == chord_btn
			   && "the modifier must never be silently omitted");
		check_rect("chord socket body", &body, socket_views[split].body[0],
				   socket_views[split].body[1], socket_views[split].body[2],
				   socket_views[split].body[3]);
		check_rect("chord socket frame", &frm, socket_views[split].frame[0],
				   socket_views[split].frame[1], socket_views[split].frame[2],
				   socket_views[split].frame[3]);
		check_rect("chord composition", &glyph, socket_views[split].chord[0],
				   socket_views[split].chord[1], socket_views[split].chord[2],
				   socket_views[split].chord[3]);
		check_rect("chord Tap label", &tap, socket_views[split].tap[0],
				   socket_views[split].tap[1], gutil_font_width("Tap", 16), 16);
		check_rect("chord Chat label", &label, socket_views[split].chat[0],
				   socket_views[split].chat[1], gutil_font_width("Chat", 16),
				   16);
		/* PL-B: "labels as above" -- a chord keeps the same Tap/Chat
		 * positions the single-cell case uses. */
		assert(tap.x == label.x && "PL-B: both labels share one left edge");
		assert(glyph.y + glyph.height <= frm.y + frm.height
			   && glyph.x + glyph.width <= frm.x + frm.width
			   && glyph.x >= frm.x && glyph.y >= frm.y
			   && "the chord composition must stay inside the socket frame");
		assert(label.y + label.height + 2 <= H - 24
			   && label.x + label.width + 2 <= W - 16
			   && "the chord case passes PL-B's envelope");
		/* The production measure of that composition is 40 px at size 16. */
		recording = true;
		used = 0;
		int drawn = gutil_prompt_glyph(glyph.x, glyph.y, glyph.height,
									   chord_glyph);
		recording = false;
		assert(drawn == glyph.width && drawn == 40
			   && "the chord composition is not 40 px at size 16");
		printf("  MEASURE chat-socket %-27s %-6s chord %d+%d composition="
			   "(%d,%d %dx%d)\n", "Classic Minus + Plus chord",
			   split ? "SPLIT" : "single", chord_mod, chord_btn, glyph.x,
			   glyph.y, glyph.width, glyph.height);
	}
	/* An explicitly unbound chat action has no route to advertise. */
	for(int split = 0; split < 2; split++) {
		int W = 640, H = socket_views[split].h;
		struct input_player_frame frame;
		struct gutil_button_bounds body, frm, glyph, tap, label;
		int cells[2];
		const char* lab = NULL;
		chat_socket_frame_glyph(INPUT_CAT_NUNCHUK, -1, &frame);
		int kind = screen_ingame_chat_socket_layout(&frame, W, H, 7, true,
			&body, &frm, &glyph, &tap, &label, cells, &lab);
		assert(kind == 0 && "an unbound action must draw no socket");
		assert(cells[0] == -1 && cells[1] == -1);
		assert(!body.width && !frm.width && !glyph.width && !tap.width
			   && !label.width && "the unbound action drew chat geometry");
		assert(!strcmp(lab, ""));
		/* A NULL frame (nobody owns this viewport yet) is also hidden. */
		assert(screen_ingame_chat_socket_layout(NULL, W, H, 7, true, &body,
			&frm, &glyph, &tap, &label, cells, &lab) == 0);
		assert(!strcmp(lab, "") && !label.width);
	}
	/* A.3's CLEARANCES, at both viewport heights. The hotbar quad is 182x22
	 * units at bar_y; the SELECTION frame is 24x24 units starting 2 px left of
	 * the bar and 1 unit above it, so its widest reach is the NINTH slot --
	 * spec A.3's 504, and the 16 px the spec leaves before the socket. The
	 * survival block (hearts / armour / air) reaches 20 units above the bar
	 * from hud_left. The crosshair box is the aiming area, centre +- 16 px. */
	for(int split = 0; split < 2; split++) {
		int W = 640, H = socket_views[split].h;
		const int s = GFX_GUI_SCALE;
		int hotbar_left = (W - 182 * s) / 2, hotbar_top = screen_ingame_hotbar_top(H, s);
		int sel_x = hotbar_left - 2 + 20 * s * 8;
		int survival_top = (hotbar_top - 10 * s) - 9 * s;
		struct input_player_frame frame;
		struct gutil_button_bounds body, frm, glyph, tap, label;
		int cells[2];
		/* The widest case is now the same as every other: a D-pad direction
		 * draws its own cell, so there is no wording left to grow. */
		chat_socket_frame(INPUT_CAT_GAMECUBE, 403, &frame);
		(void)screen_ingame_chat_socket_layout(&frame, W, H, 403, true, &body,
			&frm, &glyph, &tap, &label, cells, NULL);
		const struct gutil_button_bounds* parts[4] = {&frm, &glyph, &tap,
													  &label};
		static const char* const part_names[4] = {"frame", "glyph", "Tap",
												  "Chat"};
		for(int p = 0; p < 4; p++) {
			const struct gutil_button_bounds* r = parts[p];
			assert(r->x >= 0 && r->y >= 0 && r->x + r->width <= W
				   && r->y + r->height <= H
				   && "a socket part left the viewport");
			assert(!rects_overlap(r->x, r->y, r->width, r->height, hotbar_left,
								  hotbar_top, 182 * s, 22 * s)
				   && "the socket overlaps the hotbar");
			assert(!rects_overlap(r->x, r->y, r->width, r->height, sel_x,
								  hotbar_top - s, 24 * s, 24 * s)
				   && "the socket overlaps the slot-9 selection frame");
			assert(!rects_overlap(r->x, r->y, r->width, r->height, hotbar_left,
								  survival_top, 182 * s,
								  hotbar_top - survival_top)
				   && "the socket overlaps the hearts/armour/air rows");
			assert(!rects_overlap(r->x, r->y, r->width, r->height, W / 2 - 16,
								  H / 2 - 16, 32, 32)
				   && "the socket sits in the aiming area");
			(void)part_names[p];
		}
		/* PL-B's own number: the slot-9 selection frame ends at 504 and the
		 * socket frame starts at 512 -- exactly 8 px of clear, which is what
		 * "tucks against the hotbar" means. */
		assert(sel_x + 24 * s == 504);
		assert(frm.x - (sel_x + 24 * s) == 8
			   && "PL-B's 8 px selection-frame clearance is gone");
		printf("  MEASURE chat-socket clearances %-6s slot-9 frame ends %d, "
			   "socket frame starts %d (clear %d), hotbar ends %d, socket "
			   "bottom %d (floor %d)\n",
			   split ? "SPLIT" : "single", sel_x + 24 * s, frm.x,
			   frm.x - (sel_x + 24 * s), hotbar_left + 182 * s,
			   frm.y + frm.height, H - 24);
	}
	/* WHAT IS DRAWN, traced through the production draw helper, so the
	 * rectangles above are the ones that reach the GPU. A.1: body, then
	 * frame, then glyph, then text; the complete 32x32 source cell into a
	 * 32x32 destination (no scaling, no trimming); white vertex colour (no
	 * tint); and gutil_text's standard 2 px 0x3F3F3F shadow on both labels. */
	{
		struct input_player_frame frame;
		struct gutil_button_bounds body, frm, glyph, tap, label;
		int cells[2];
		chat_socket_frame(INPUT_CAT_CLASSIC_CONTROLLER, 200, &frame);
		(void)screen_ingame_chat_socket_layout(&frame, 640, 480, 200, true,
			&body, &frm, &glyph, &tap, &label, cells, NULL);
		assert(cells[1] == 25);
		recording = true;
		used = 0;
		screen_ingame_chat_socket_draw(&frame, 640, 480, 200, true);
		recording = false;
		char needle[256];
		/* gui.png (0,0,22,22) -> the 44x44 body */
		snprintf(needle, sizeof needle,
				 "quads 4 | %d,%d,-2 255,255,255,255 0,0 | %d,%d,-2 "
				 "255,255,255,255 22,0",
				 body.x, body.y, body.x + body.width, body.y);
		const char* body_at = strstr(calls, needle);
		assert(body_at && "the socket body is not gui.png (0,0,22,22)");
		/* gui.png (0,22,24,24) -> the 48x48 frame */
		snprintf(needle, sizeof needle,
				 "quads 4 | %d,%d,-2 255,255,255,255 0,22 | %d,%d,-2 "
				 "255,255,255,255 24,22",
				 frm.x, frm.y, frm.x + frm.width, frm.y);
		const char* frame_at = strstr(calls, needle);
		assert(frame_at && "the socket frame is not gui.png (0,22,24,24)");
		/* the complete 32x32 cell 25, drawn 32x32 and untinted */
		snprintf(needle, sizeof needle,
				 "quads 4 | %d,%d,-2 255,255,255,255 %d,%d | %d,%d,-2 "
				 "255,255,255,255 %d,%d",
				 glyph.x, glyph.y, (25 % 8) * 32, (25 / 8) * 32,
				 glyph.x + 32, glyph.y, (25 % 8) * 32 + 32, (25 / 8) * 32);
		const char* glyph_at = strstr(calls, needle);
		assert(glyph_at && "the binding glyph is not the complete 32x32 cell");
		/* A.1's paint order: body, frame, glyph, text. */
		assert(body_at < frame_at && frame_at < glyph_at
			   && "A.1's paint order (body, frame, glyph) is wrong");
		/* both labels at 16 px with the native (2,2) 0x3F3F3F shadow */
		snprintf(needle, sizeof needle, "quads 4 | %d,%d,-2 63,63,63,255 %d,%d",
				 tap.x + 2, tap.y + 2, ('T' % 16) * 16, ('T' / 16) * 16);
		assert(strstr(calls, needle) && "Tap lost its 2 px 0x3F3F3F shadow");
		snprintf(needle, sizeof needle, "quads 4 | %d,%d,-1 255,255,255,255 "
				 "%d,%d | %d,%d,-1 255,255,255,255 %d,%d",
				 label.x, label.y, ('C' % 16) * 16, ('C' / 16) * 16,
				 label.x + 16, label.y, ('C' % 16) * 16 + 16, ('C' / 16) * 16);
		assert(strstr(calls, needle) && "the Chat label is not 16 px white");
		snprintf(needle, sizeof needle, "quads 4 | %d,%d,-2 63,63,63,255 %d,%d",
				 label.x + 2, label.y + 2, ('C' % 16) * 16, ('C' / 16) * 16);
		assert(strstr(calls, needle) && "Chat lost its 2 px 0x3F3F3F shadow");
		/* An unbound action draws nothing. */
		chat_socket_frame_glyph(INPUT_CAT_NUNCHUK, -1, &frame);
		(void)screen_ingame_chat_socket_layout(&frame, 640, 480, 7, true, &body,
			&frm, &glyph, &tap, &label, cells, NULL);
		recording = true;
		used = 0;
		screen_ingame_chat_socket_draw(&frame, 640, 480, 7, true);
		recording = false;
		assert(used == 0 && "an unbound action drew chat artwork or text");
	}
	/* PC IS UNCHANGED WHERE THE SPEC SAYS SO. The directional substitution is
	 * scoped to THIS prompt: a PC keyboard code is not a D-pad direction of
	 * its family, so the socket draws the resolver's own cell -- and PC takes
	 * the same rectangles as every other family, with no platform-specific
	 * geometry. (A.1: "Scope directional substitution to this prompt.") */
	{
		struct input_player_frame pc, wii;
		struct gutil_button_bounds pc_glyph, wii_glyph, unused;
		int pc_cells[2], wii_cells[2];
		chat_socket_frame(INPUT_CAT_NONE, 84, &pc);
		chat_socket_frame(INPUT_CAT_NUNCHUK, 6, &wii);
		assert(screen_ingame_chat_socket_layout(&pc, 640, 480, 84, false,
			&unused, &unused, &pc_glyph, &unused, &unused, pc_cells, NULL) == 1);
		assert(screen_ingame_chat_socket_layout(&wii, 640, 480, 6, false,
			&unused, &unused, &wii_glyph, &unused, &unused, wii_cells, NULL)
			   == 1);
		assert(!memcmp(&pc_glyph, &wii_glyph, sizeof(pc_glyph))
			   && "PC must take the same geometry rule as every family");
		/* the resolver now answers a PC glyph CODE (4d5cd8bb5); its cell */
		int pc_mod, pc_cell;
		assert(input_glyph_cells(input_glyph_key(INPUT_CAT_NONE, 84), &pc_mod,
								 &pc_cell));
		assert(pc_cells[1] == pc_cell && "the PC cell is not the resolver's own");
		/* A PC keyboard code that happens to collide numerically with a Wii
		 * D-pad code still gets the resolver's cell, never a direction. */
		struct input_player_frame pc_low;
		chat_socket_frame(INPUT_CAT_NONE, 32, &pc_low);
		assert(screen_ingame_chat_socket_layout(&pc_low, 640, 480, 0, false,
			&unused, &unused, &unused, &unused, &unused, pc_cells, NULL) == 1);
		assert(input_glyph_cells(input_glyph_key(INPUT_CAT_NONE, 32), &pc_mod,
								 &pc_cell) && pc_cells[1] == pc_cell);
	}
	puts("PASS H-C2 chat socket (placement PL-B): rectangles as literals in "
		 "both viewports -- frame (0,22,24,24)x2 at (W-128,H-72), body "
		 "(0,0,22,22)x2 at frame+(2,2), one complete 32x32 cell at "
		 "frame+(8,8), chord 40 px at frame+(4,16), Tap (W-72,H-68), Chat "
		 "(W-72,H-46) under it or (W-72,H-56) alone, unbound hidden; "
		 "directional cells 25/26/27/28 replace cell 24 + a "
		 "direction word; 8 px clear of the slot-9 selection frame, every "
		 "mark inside y <= H-24 and x <= W-16 with its shadow, clear of the "
		 "hotbar, survival rows and crosshair box; PC unchanged");
}

/* ---- 4b-ii. the socket's VISIBILITY (spec A.4) and its CAPTURE LEVER ----
 *
 * A.4 H-C2: a remote network session only, per owner and viewport, persistent
 * throughout eligible gameplay EVEN AFTER the composer is first opened, and
 * hidden under disconnect/recovery modals and join prompts. Menus, the pause
 * screen and the composer hide it by CONSTRUCTION -- those screens own the
 * frame instead of screen_ingame_render2D_owned, exactly as they already do
 * for the hotbar and the hearts -- and this engine has no separate
 * hidden-HUD toggle for them to be distinguished from.
 *
 * RETIRED with ruling 8's hint: the `chat_hint_done` gate (the hint used to
 * disappear the first time the composer opened; A.5 makes the socket
 * persistent, and screen_ingame.c no longer reads the flag at all) and the
 * `owner == &gstate.players[0]` IDENTITY test, replaced by the chat ROUTE's
 * own capability -- which resolves to the same owner today, because the
 * composer is a global screen only the host can open.
 *
 * ALSO RETIRED: the old pin that the lever "must not rewrite the resolved
 * glyph". Brief 26 reverses it -- a PC capture has to show the REMOTE cells
 * it is asked for -- so the lever now substitutes the encoded glyph too, and
 * the pin below requires exactly that, inside the capture branch only. */
extern bool screen_ingame_chat_socket_capture(int capture,
	enum input_category* category, int* code, bool* is_tap, int* glyph);

static void check_chat_socket_rules(void) {
	/* remote session + an owner with the chat route, nothing suppressing. */
	assert(screen_ingame_chat_socket_visible(true, true, false, false, false));
	/* single player: no socket, whatever else is true. */
	assert(!screen_ingame_chat_socket_visible(false, true, false, false, false));
	/* an owner with no chat route never gets one, in either arm. */
	assert(!screen_ingame_chat_socket_visible(true, false, false, false, false));
	assert(!screen_ingame_chat_socket_visible(false, false, false, false, true));
	/* the capture lever forces the DRAW off-line -- and is still subject to
	 * the two suppressions, so a rig frame can never photograph the socket
	 * under a recovery modal or a join prompt. */
	assert(screen_ingame_chat_socket_visible(false, true, false, false, true));
	assert(!screen_ingame_chat_socket_visible(true, true, true, false, false));
	assert(!screen_ingame_chat_socket_visible(true, true, false, true, false));
	assert(!screen_ingame_chat_socket_visible(false, true, true, false, true));
	assert(!screen_ingame_chat_socket_visible(false, true, false, true, true));
	/* Non-interactive by construction: the layout produces rectangles and the
	 * draw produces quads -- there is no hit test, focus or slot anywhere in
	 * the socket's source. */
	{
		char* src = read_source("source/game/gui/screen_ingame.c");
		const char* body = strstr(src, "void screen_ingame_chat_socket_draw(");
		assert(body && "the socket draw helper is gone or renamed");
		const char* stop = strstr(body, "\n}");
		assert(stop);
		char* only = malloc((size_t)(stop - body) + 1);
		assert(only);
		memcpy(only, body, (size_t)(stop - body));
		only[stop - body] = '\0';
		assert(!strstr(only, "gutil_button_hit") && !strstr(only, "menu_focus")
			   && !strstr(only, "input_pointer") && !strstr(only, "audio_")
			   && !strstr(only, "rumble")
			   && "A.4: the socket is non-interactive and silent");
		free(only);
		/* A.5: screen_ingame.c must not READ the retired first-use gate any
		 * more -- the socket is persistent throughout eligible gameplay. The
		 * field itself (game_state.h) and its two writers (screen_chat.c,
		 * main.c) are outside this brief's owned paths, so they stay; what is
		 * pinned here is that nothing in this file consults them. */
		assert(!strstr(src, "gstate.chat_hint_done")
			   && "A.5: the chat_hint_done gate must be gone");
		free(src);
	}
	/* The chat ROUTE's owner: the composer is a global screen, opened from
	 * the host's action pass and the host's pause menu, so the host is the
	 * only local owner that can use it. */
	assert(screen_ingame_chat_socket_owner(0));
	assert(!screen_ingame_chat_socket_owner(1));
	assert(!screen_ingame_chat_socket_owner(7));
	/* L4: pin the ROUTE, not today's answer. `!owner(1)` alone would still
	 * pass if a route became owner-local and the socket silently stopped
	 * following the capability it claims to follow, so source-pin the two
	 * routes the helper's own comment names -- the way
	 * check_sneak_rail_source_pin pins the hotbar anchors.
	 *   route 1: screen_ingame_update_slot's opener edge, whose whole
	 *            global-screen block is gated on `slot == 0`. */
	{
		char* src = read_source("source/game/gui/screen_ingame.c");
		const char* gate = strstr(src, "Global-screen edges: HOST only");
		assert(gate && "the opener's host-only block is gone or renamed");
		const char* brace = strstr(gate, "if(slot == 0) {");
		assert(brace && brace - gate < 400
			   && "route 1 no longer gates the opener edge on slot == 0");
		const char* opener = strstr(brace, "input_chat_open_edge(");
		assert(opener && "the chat opener edge left the host-only block");
		/* it is INSIDE that block: no `\n\t}` closes it first. */
		const char* close = strstr(brace, "\n\t}");
		assert(close && opener < close
			   && "L4: the chat opener edge escaped the slot == 0 gate");
		/* and the helper is the ONE statement of this rule, so a future
		 * owner-local route has to come through it. */
		const char* fn = strstr(src, "bool screen_ingame_chat_socket_owner(unsigned slot) {");
		assert(fn);
		const char* body_end = strstr(fn, "\n}");
		assert(body_end && strstr(fn, "return slot == 0;") < body_end
			   && "the socket owner rule is no longer one statement");
		free(src);
		char* pause_src = read_source("source/game/gui/screen_pause.c");
		assert(!strstr(pause_src, "screen_set(&screen_chat)")
			   && "the retired pause Chat route reappeared");
		free(pause_src);
	}
	puts("PASS H-C2 visibility: remote session + the chat route's own owner, "
		 "persistent after the composer is first opened (the chat_hint_done "
		 "gate is gone), hidden under recovery modals and join prompts, the "
		 "capture lever exempt from neither, non-interactive and silent");
}

static void check_chat_socket_capture(void) {
	enum input_category category = INPUT_CAT_NONE;
	int code = -12345, glyph = -12345;
	bool is_tap = true;
	/* OFF: absent (0) and anything the lever does not define, and NOTHING is
	 * written to the caller's wording. */
	static const int off[5] = {0, -1, 4, 99, 2026};
	for(int k = 0; k < 5; k++) {
		category = INPUT_CAT_CLASSIC_CONTROLLER;
		code = -12345;
		glyph = -12345;
		is_tap = true;
		assert(!screen_ingame_chat_socket_capture(off[k], &category, &code,
												  &is_tap, &glyph)
			   && "the capture lever armed on a value it does not define");
		assert(category == INPUT_CAT_CLASSIC_CONTROLLER && code == -12345
			   && is_tap && glyph == -12345
			   && "an unarmed lever wrote to the wording");
	}
	/* Each armed value is measured through the SHIPPED socket layout, so the
	 * lever is proven to reproduce a case the tests above already pin. */
	static const struct {
		int lever;
		const char* what;
		enum input_category category;
		int code;
		bool is_tap;
		int kind;
		int cell;
	} armed[3] = {
		{1, "Remote default, tap 2", INPUT_CAT_NUNCHUK, 7, true, 1, 3},
		{2, "D-pad Right remap", INPUT_CAT_GAMECUBE, 403, false, 1, 28},
		{3, "Classic Minus + Plus chord", INPUT_CAT_CLASSIC_CONTROLLER, 211,
		 false, 2, 23},
	};
	for(int k = 0; k < 3; k++) {
		assert(screen_ingame_chat_socket_capture(armed[k].lever, &category,
												 &code, &is_tap, &glyph));
		assert(category == armed[k].category && code == armed[k].code
			   && is_tap == armed[k].is_tap
			   && "the lever is not the wording it promises");
		assert(glyph >= 0 && "the lever must supply the REMOTE cell to draw");
		struct input_player_frame frame;
		struct gutil_button_bounds g, unused;
		int cells[2];
		chat_socket_frame_glyph(category, glyph, &frame);
		int kind = screen_ingame_chat_socket_layout(&frame, 640, 480, code,
			is_tap, &unused, &unused, &g, &unused, &unused, cells, NULL);
		assert(kind == armed[k].kind && cells[1] == armed[k].cell
			   && "the lever does not reproduce the case it names");
		printf("  MEASURE chat-socket lever %d %-27s cell %2d kind %d "
			   "glyph=(%d,%d %dx%d)\n", armed[k].lever, armed[k].what,
			   cells[1], kind, g.x, g.y, g.width, g.height);
	}
	/* The CALL SITE. The lever is resolved before the draw, substitutes the
	 * family, the code, the tap semantics AND the encoded glyph -- brief 26
	 * requires the REMOTE cells on a PC frame -- and nothing else: it opens
	 * no screen, latches nothing, and writes no game state. */
	{
		char* src = read_source("source/game/gui/screen_ingame.c");
		const char* fn = strstr(src, "void screen_ingame_render2D_owned(");
		assert(fn);
		const char* call = strstr(fn, "screen_ingame_chat_socket_capture(");
		assert(call && "the capture lever is not read at the draw site");
		const char* draw = strstr(fn, "screen_ingame_chat_socket_draw(");
		assert(draw && call < draw
			   && "the lever must be resolved before the draw");
		size_t window = (size_t)(draw - call);
		char* body = malloc(window + 1);
		assert(body);
		memcpy(body, call, window);
		body[window] = '\0';
		assert(strstr(body, "glyphs[IB_CHAT] =")
			   && "brief 26: the lever must substitute the REMOTE cell");
		assert(!strstr(body, "screen_set(")
			   && !strstr(body, "input_chat_open_edge")
			   && !strstr(body, "input_sneak")
			   && "the lever must be draw-only");
		free(body);
		assert(strstr(src, "\"debug.chat_hint_capture\"")
			   && "the lever's config key is gone");
		free(src);
	}
	puts("PASS H-C2 capture lever: debug.chat_hint_capture 1 = the shipped "
		 "Remote default (cell 3, Tap), 2 = a D-pad Right remap (cell 28, no "
		 "direction word), 3 = a chord (40 px composition); absent or any "
		 "other value off with nothing written; it substitutes the REMOTE "
		 "cell so a PC frame shows what it is asked for, and opens nothing");
}

/* ---- 4b-iii. the HUD pass's BLEND-STATE bracket (review 28 H1 + M3) -----
 *
 * THE CONTRACT (both backends, read not assumed): gfx_mode_gui_viewport sets
 * MODE_BLEND + alpha_test(true) -- source/platform/pc/gfx.c:412-413,
 * source/platform/wii/gfx.c:871-872 -- once per viewport before the 2D draws.
 * MODE_BLEND is therefore what every HUD draw is handed, and a block that
 * brackets its own quads MUST restore MODE_BLEND. MODE_OFF is not a restore:
 * it silently disables blending for everything drawn after it in the pass.
 *
 * The proof below is behavioural, not a source pin: it runs the PRODUCTION
 * screen_ingame_render2D_owned twice -- once in a remote session with the
 * socket on screen, once with no socket -- and requires the three
 * blend-sensitive HUD draws to see the SAME mode in both, i.e. the socket is
 * state-NEUTRAL. Before the fix the remote frame painted the hotbar body, the
 * rail and the sleep wash with blending OFF. */
extern unsigned screen_ingame_test_socket_capture_reads(void);

static struct window_container hud_inventory;
static struct input_player_frame hud_input;

/* Enough real game state for the HUD's own draws; everything it needs is a
 * plain field read except the inventory window, which the hotbar pass really
 * does resolve through windowc_get_latest. */
static void hud_rig_init(void) {
	assert(windowc_create(&hud_inventory, WINDOW_TYPE_INVENTORY,
						  INVENTORY_SIZE));
	gstate.players[0].windows[WINDOWC_INVENTORY] = &hud_inventory;
	gstate.players[0].hud_message[0] = 0;
	gstate.players[0].camera_hit.hit = false;
	gstate.players[0].camera_hit.entity_hit = false;
	gstate.players[0].menu_host.recovery.phase = CONTROLLER_RECOVERY_NONE;
	gstate.players[1].menu_host.recovery.phase = CONTROLLER_RECOVERY_NONE;
	gstate.players[1].local_gameplay_ready = false;
	gstate.players[1].local_player = NULL;
	gstate.debug_overlay = false;
	memset(&gstate.chat_ring, 0, sizeof(gstate.chat_ring));
	texture_gui_widgets.width = texture_gui_widgets.height = 256;
	texture_icons.width = texture_icons.height = 256;
	/* The capture rig's own levers (pc-captures-26's NOTES), as the config the
	 * HUD actually reads. debug.chat_hint_capture is set to -1 on purpose: a
	 * LEGAL value of the lever's domain that means "off", and the exact value
	 * review 28 M2 says defeats the socket-capture cache. */
	JSON_Object* root = json_object(gstate.config_user.root);
	assert(root);
	assert(json_object_dotset_boolean(root, "ui.sneak_toggle_indicator", 1)
		   == JSONSuccess);
	assert(json_object_dotset_number(root, "debug.sneak_indicator_capture", 1)
		   == JSONSuccess);
	assert(json_object_dotset_number(root, "debug.chat_hint_capture", -1)
		   == JSONSuccess);
	/* An owner whose chat binding resolves through the production resolver, so
	 * the socket draws its frame rather than an unbound blank. */
	chat_socket_frame(INPUT_CAT_NUNCHUK, 7, &hud_input);
	assert(hud_input.glyphs[IB_CHAT] >= 0);
	hud_input.join_prompt = false;
}

/* One production HUD frame, entered at exactly the state the real 2D pass
 * hands it. The text trace stays off -- a whole HUD frame would overflow
 * `calls`; the four draws under test are picked out by identity instead. */
static void hud_render_once(bool remote_session, int sleep_timer) {
	gstate.remote_session = remote_session;
	gstate.players[0].sleep_timer = sleep_timer;
	hud_blend_hotbar = hud_blend_rail = hud_blend_sleep = -1;
	hud_hotbar_quads = hud_rail_quads = hud_sleep_quads = hud_socket_quads = 0;
	gfx_blending(MODE_BLEND);
	gfx_alpha_test(true);
	hud_recording = true;
	screen_ingame_render2D_owned(&gstate.players[0], &hud_input, 640, 480);
	hud_recording = false;
}

static void check_hud_blend_state(void) {
	hud_rig_init();
	/* (1) The socket ALONE: it must hand back the mode it was given. */
	{
		struct input_player_frame frame;
		chat_socket_frame(INPUT_CAT_CLASSIC_CONTROLLER, 200, &frame);
		gfx_blending(MODE_BLEND);
		gfx_alpha_test(true);
		screen_ingame_chat_socket_draw(&frame, 640, 480, 200, true);
		assert(blend_mode == MODE_BLEND
			   && "H1: the socket must restore the 2D pass's MODE_BLEND");
		assert(alpha_test_enabled
			   && "the socket must leave the alpha test as it found it");
	}
	/* (2) chat_hud_draw's own bracket, which mis-restored the same way
	 * whenever a line happened to be faded in (the pre-existing half of H1). */
	{
		struct chat_ring_state ring;
		memset(&ring, 0, sizeof(ring));
		chat_ring_push(&ring, "Chat");
		gfx_blending(MODE_BLEND);
		gfx_alpha_test(true);
		chat_hud_render(&ring, 4, 300, false, 10);
		assert(blend_mode == MODE_BLEND
			   && "H1: chat_hud_draw must restore the pass's MODE_BLEND");
		assert(alpha_test_enabled);
	}
	/* (3) The whole HUD frame, with and without the socket on screen. */
	hud_render_once(true, 50);
	int socket_hotbar = hud_blend_hotbar, socket_rail = hud_blend_rail,
		socket_sleep = hud_blend_sleep;
	assert(hud_socket_quads == 1 && "the remote frame drew no socket");
	assert(hud_hotbar_quads == 1 && hud_rail_quads == 1 && hud_sleep_quads == 1
		   && "the HUD frame did not draw the three blend-sensitive marks");
	assert(blend_mode == MODE_BLEND
		   && "the HUD pass must end in the mode it started in");
	hud_render_once(false, 50);
	assert(hud_socket_quads == 0 && "the single-player frame drew a socket");
	assert(hud_hotbar_quads == 1 && hud_rail_quads == 1 && hud_sleep_quads == 1);
	assert(blend_mode == MODE_BLEND);
	printf("  MEASURE hud-blend hotbar %d/%d rail %d/%d sleep %d/%d "
		   "(socket / no socket; %d = MODE_BLEND, %d = MODE_OFF)\n",
		   socket_hotbar, hud_blend_hotbar, socket_rail, hud_blend_rail,
		   socket_sleep, hud_blend_sleep, (int)MODE_BLEND, (int)MODE_OFF);
	assert(socket_hotbar == hud_blend_hotbar
		   && "H1: the socket changed the hotbar body's blend mode");
	assert(socket_rail == hud_blend_rail
		   && "H1: the socket changed the sneak rail's blend mode");
	assert(socket_sleep == hud_blend_sleep
		   && "H1: the socket changed the sleep fade's blend mode");
	assert(hud_blend_hotbar == MODE_BLEND && hud_blend_rail == MODE_BLEND
		   && hud_blend_sleep == MODE_BLEND
		   && "every HUD draw must run in the pass's own MODE_BLEND");
	gstate.remote_session = false;
	gstate.players[0].sleep_timer = 0;
	puts("PASS HUD blend state: the 2D pass is entered at MODE_BLEND (both "
		 "backends) and every block hands it back -- the chat socket and "
		 "chat_hud_draw restore MODE_BLEND, and the hotbar body, the SNEAK "
		 "rail and Beta's sleep fade see the same mode with the socket on "
		 "screen as without it");
}

/* ---- 4b-iv. M2: the socket-capture cache must read the config ONCE -------
 * `debug.chat_hint_capture`'s value domain includes negatives (the lever's
 * own off-table is {0, -1, 4, 99, 2026}), so "-1 = not read yet" was a
 * sentinel INSIDE the cached value's domain: a config that sets -1 re-ran a
 * parson dot-path lookup every frame, per viewport, on the Wii main thread --
 * exactly the cost the cache exists to avoid. hud_rig_init sets the key to -1,
 * so the frames already drawn above are the pathological case. */
static void check_socket_capture_cache(void) {
	unsigned before = screen_ingame_test_socket_capture_reads();
	assert(before == 1
		   && "M2: the socket capture lever was read more than once");
	for(int k = 0; k < 5; k++)
		hud_render_once(k % 2 == 0, 0);
	assert(screen_ingame_test_socket_capture_reads() == 1
		   && "M2: a negative lever value re-reads the config every frame");
	gstate.remote_session = false;
	printf("  MEASURE socket-capture cache: %u config read(s) across %d HUD "
		   "frames with debug.chat_hint_capture = -1\n",
		   screen_ingame_test_socket_capture_reads(), 7);
	puts("PASS M2 socket capture cache: a legal NEGATIVE lever value is "
		 "cached after exactly one config read, so no parson dot-path lookup "
		 "runs per frame per viewport");
}

/* ---- 4b-v. the MODALS that own the viewport (review 28 H2 + M1) ---------
 * Spec A.3: "Suppression rules apply before drawing; a modal must not merely
 * cover a still-visible hint." A.4: hidden under disconnect/recovery modals.
 *
 * THE OVER-HUD MODALS THAT EXIST, and what each one does to this HUD:
 *  1. the PRIMARY controller-recovery modal -- screen_controller_recovery_
 *     render, called from the END of screen_ingame_render2D_owned itself for
 *     players[0]; a 153-alpha full-viewport wash plus a 460 px panel. In
 *     SINGLE view this is THE recovery modal, and the per-owner
 *     menu_host.recovery state the gate used to read is a DIFFERENT machine
 *     (main.c only runs it for a split/menu-host frame). SUPPRESSED.
 *  2. the per-owner menu-host recovery modal (local_menu_host_recovery_
 *     render, split view). SUPPRESSED (it already was).
 *  3. the LOCAL pause -- local_pause_render, drawn by main.c AFTER render2D
 *     in single view; a viewport-wide 180-alpha wash plus a panel. In SPLIT
 *     it replaces the HUD instead, so the term costs nothing there.
 *     SUPPRESSED.
 *  4. the global screens (pause, inventory, crafting, the chat composer, the
 *     title/world-select/connect screens) -- NOT modals over this HUD: they
 *     are current_screen, so screen_ingame_render2D_owned is never called.
 *     Hidden by construction, as the source comments say.
 *  5. the DEATH screen -- in single view it is the global screen_death (4);
 *     in split, main.c's own branch draws screen_death_owner_render INSTEAD
 *     of the HUD. Hidden by construction either way; not a modal over it.
 *  6. the loading / terrain hold -- there is no such overlay: the world load
 *     runs under the world-select / connect global screens (4), and once
 *     screen_ingame is current the HUD simply draws over an empty world.
 *     Nothing to suppress.
 *  7. the connection notices -- gutil_connection_notices, drawn INSIDE this
 *     function as a top-of-viewport card. A notice, not a modal: it owns no
 *     input, dims nothing, and does not reach the socket's or the rail's
 *     rows. NOT suppressed.
 *  8. the split "P%u: ... - world continues" strip and the in-water tint --
 *     neither owns the view (the first is split-only chrome outside every
 *     viewport, the second is a world-pass tint). NOT suppressed. */
static void check_hud_modal_suppression(void) {
	char* src = read_source("source/game/gui/screen_ingame.c");
	const char* fn = strstr(src, "void screen_ingame_render2D_owned(");
	assert(fn);
	/* The modal facts are taken ONCE, before either gate, and both gates take
	 * the same answer -- so the socket and the rail can never disagree about
	 * which modal is up. */
	const char* modal = strstr(fn, "screen_ingame_hud_modal_active(");
	assert(modal && "H2/M1: the HUD does not compute its modal state");
	const char* stop = strchr(modal, ';');
	assert(stop);
	size_t n = (size_t)(stop - modal);
	char* args = malloc(n + 1);
	assert(args);
	memcpy(args, modal, n);
	args[n] = '\0';
	/* (1) the PRIMARY recovery modal, the one single view actually draws --
	 * read through its exported accessor, at the same owner the draw at the
	 * end of this function is gated on. */
	assert(strstr(args, "screen_controller_recovery_state()")
		   && "H2: the single-view recovery modal is not a suppressor");
	/* (2) the per-owner menu-host recovery state, unchanged. */
	assert(strstr(args, "menu_host.recovery.phase")
		   && "the per-owner recovery modal stopped suppressing");
	/* (3) the LOCAL pause, which main.c draws OVER this HUD in single view. */
	assert(strstr(args, "local_pause_active(&owner->pause)")
		   && "M1: the single-view local pause is not a suppressor");
	free(args);
	/* Both gates take it, and the socket's gate no longer reads the per-slot
	 * recovery phase on its own. */
	const char* socket_gate = strstr(fn, "screen_ingame_chat_socket_visible(");
	const char* rail_gate = strstr(fn, "screen_ingame_sneak_marker_visible(");
	assert(socket_gate && rail_gate);
	assert(modal < socket_gate && modal < rail_gate
		   && "the modal state must be resolved before both gates");
	for(int g = 0; g < 2; g++) {
		const char* gate = g ? rail_gate : socket_gate;
		const char* gstop = strchr(gate, ';');
		assert(gstop);
		size_t gn = (size_t)(gstop - gate);
		char* gargs = malloc(gn + 1);
		assert(gargs);
		memcpy(gargs, gate, gn);
		gargs[gn] = '\0';
		assert(strstr(gargs, "hud_modal")
			   && "both gates must take the SAME modal answer");
		free(gargs);
	}
	/* The claim the old comments made -- that pause and the menus hide these
	 * blocks "by construction" -- is true only for the GLOBAL screens, and
	 * must not be asserted of the local pause any more. */
	assert(!strstr(fn, "those screens own the frame instead of this function")
		   || strstr(fn, "local pause")
		   || strstr(fn, "local_pause")
		   || !strstr(fn, "local"));
	free(src);
	/* BEHAVIOURAL, through the production HUD: with the local pause open --
	 * which main.c draws OVER this HUD in single view -- neither block may
	 * reach the GPU at all. (The primary recovery modal's own state is a
	 * file-static of screen.c driven by the real input machine, which this
	 * leaf-wrapped harness does not host; it is pinned at the call site
	 * above and tabled through the pure predicate below.) */
	assert(!local_pause_active(&gstate.players[0].pause));
	hud_render_once(true, 0);
	assert(hud_socket_quads == 1 && hud_rail_quads == 1
		   && "the unsuppressed frame must draw both blocks");
	gstate.players[0].pause.open = true;
	assert(local_pause_active(&gstate.players[0].pause));
	hud_render_once(true, 0);
	assert(hud_socket_quads == 0
		   && "M1: the local pause must hide the chat socket");
	assert(hud_rail_quads == 0
		   && "M1: the local pause must hide the sneak rail");
	assert(hud_hotbar_quads == 1
		   && "only the two new blocks are suppressed, not the HUD");
	gstate.players[0].pause.open = false;
	gstate.remote_session = false;
	/* THE TABLE: every modal x {socket, rail} -> hidden; no modal -> shown. */
	static const struct {
		const char* what;
		bool owner_recovery, primary_recovery, local_pause;
	} modals[4] = {
		{"nothing", false, false, false},
		{"the per-owner menu-host recovery modal", true, false, false},
		{"the PRIMARY controller-recovery modal", false, true, false},
		{"the local pause", false, false, true},
	};
	for(int k = 0; k < 4; k++) {
		bool up = screen_ingame_hud_modal_active(modals[k].owner_recovery,
												 modals[k].primary_recovery,
												 modals[k].local_pause);
		assert(up == (k != 0) && "the modal predicate missed a modal");
		/* socket: eligible in every other way, so only the modal decides. */
		assert(screen_ingame_chat_socket_visible(true, true, up, false, false)
			   == !up && "a modal must hide the chat socket");
		/* A.4: the capture lever is exempt from no suppression. */
		assert(screen_ingame_chat_socket_visible(false, true, up, false, true)
			   == !up && "a modal must hide a RIG socket too");
		/* rail: latched and configured on, so only the modal decides. */
		assert(screen_ingame_sneak_marker_visible(true, true, false, false, up)
			   == !up && "a modal must hide the sneak rail");
		assert(screen_ingame_sneak_marker_visible(false, true, true, false, up)
			   == !up && "a modal must hide a RIG rail too");
		printf("  MEASURE hud-modal %-38s socket %s, rail %s\n", modals[k].what,
			   up ? "hidden" : "shown", up ? "hidden" : "shown");
	}
	/* All three at once is still just "a modal is up", and the join prompt and
	 * the bed toast keep their own independent veto. */
	assert(screen_ingame_hud_modal_active(true, true, true));
	assert(!screen_ingame_chat_socket_visible(true, true, false, true, false));
	assert(!screen_ingame_sneak_marker_visible(true, true, false, true, false));
	puts("PASS H2/M1 modal suppression: the per-owner menu-host recovery "
		 "modal, the PRIMARY single-view controller-recovery modal and the "
		 "local pause each hide both the chat socket and the sneak rail (the "
		 "capture levers included); the global screens, the death screen and "
		 "the connection notices are not modals over this HUD");
}

/* ---- 4b-vi. the CHAT HISTORY CLAMP (build spec 15 A.3) ------------------
 * The closed HUD's black strip is Beta's fixed 320-LOGICAL one, i.e. 640 px
 * at GUI scale 2 -- the full viewport width -- so the history block does
 * reach the socket's column and needs a vertical ceiling rather than a
 * horizontal one. A.3: during an eligible multiplayer session the BOTTOM of
 * the rendered history bounds, shadow and backing included, is
 * min(existing bottom, H-88) per viewport -- 392 single, 152 per split
 * viewport -- and "any existing higher position" is never lowered.
 *
 * The bottom is derived from the RENDERER, not restated: chat_hud_bottom is
 * checked against the lowest pixel chat_hud_render actually emits. */
extern int chat_hud_bottom(int base_y);
extern int chat_hud_clamp_base_y(int base_y, int bottom_limit);

/* The lowest y any quad in the current trace reaches. */
static int trace_lowest_y(void) {
	int lowest = -1;
	for(const char* p = calls; (p = strstr(p, " | ")); p += 3) {
		int x, y, z;
		if(sscanf(p + 3, "%d,%d,%d", &x, &y, &z) == 3 && y > lowest)
			lowest = y;
	}
	return lowest;
}

static void check_chat_history_clamp(void) {
	/* chat_hud_bottom, measured against the real closed-HUD renderer. */
	static const int heights[2] = {480, 240};
	for(int split = 0; split < 2; split++) {
		int H = heights[split];
		int base = H - 74 * GFX_GUI_SCALE; /* the shipped anchor, H-148 */
		struct chat_ring_state ring;
		memset(&ring, 0, sizeof(ring));
		chat_ring_push(&ring, "Chat");
		texture_gui_widgets.width = texture_gui_widgets.height = 256;
		recording = true;
		used = 0;
		chat_hud_render(&ring, 2 * GFX_GUI_SCALE, base, false, 10);
		recording = false;
		int drawn_bottom = trace_lowest_y();
		assert(drawn_bottom > 0 && "the chat HUD drew nothing to measure");
		assert(chat_hud_bottom(base) == drawn_bottom
			   && "chat_hud_bottom does not match what the HUD draws");
		/* L1: the lowest mark is gutil_text's hard SHADOW, whose offset is
		 * `scale / 8` (gui_util.c gutil_text_rgba), not a fixed 2 px --
		 * chat_hud_draw passes scale = 8 * GFX_GUI_SCALE, so the offset is
		 * GFX_GUI_SCALE and the block's depth is 9 * GFX_GUI_SCALE. */
		assert(chat_hud_bottom(base) - base == 9 * GFX_GUI_SCALE
			   && "the history block's depth is not 9 * GUI scale");
		/* A.3's ceiling, and the fact that the SHIPPED anchor already clears
		 * it -- so the clamp is a guard, not a move. */
		int limit = H - 88;
		assert(limit == (split ? 152 : 392) && "A.3's per-viewport ceiling");
		assert(chat_hud_bottom(base) <= limit);
		assert(chat_hud_clamp_base_y(base, limit) == base
			   && "A.3: a higher existing anchor must never be lowered");
		printf("  MEASURE chat-history %-6s anchor %d, drawn bottom %d "
			   "(shadow included), ceiling %d, slack %d px -- unchanged\n",
			   split ? "SPLIT" : "single", base, drawn_bottom, limit,
			   limit - drawn_bottom);
		/* An anchor that WOULD pass the ceiling is lifted by exactly the
		 * overrun, and no further; clamping twice changes nothing. */
		int low = H - 20;
		int clamped = chat_hud_clamp_base_y(low, limit);
		assert(chat_hud_bottom(low) > limit);
		assert(chat_hud_bottom(clamped) == limit && clamped == low
				   - (chat_hud_bottom(low) - limit)
			   && "the clamp must lift by exactly the overrun");
		assert(chat_hud_clamp_base_y(clamped, limit) == clamped);
		/* Equal is not over. */
		assert(chat_hud_clamp_base_y(clamped, limit) == clamped);
	}
	/* L1 CONTINUED -- THE SWEEP. GFX_GUI_SCALE is a compile-time constant, so
	 * chat_hud_bottom itself cannot be evaluated at another scale; what CAN be
	 * swept is the renderer it models. Measure gutil_text's real shadow extent
	 * at the three GUI scales' text sizes and require it to be `scale / 8`
	 * every time -- which is the term chat_hud_bottom now carries, and is why
	 * the retired hard-coded `+ 2` was right only at scale 2. */
	for(int s = 1; s <= 3; s++) {
		recording = true;
		used = 0;
		gutil_text_rgba(0, 100, "Chat", 8 * s, true, 0xFF, 0xFF, 0xFF, 0xFF);
		recording = false;
		int bottom = trace_lowest_y();
		assert(bottom == 100 + 8 * s + s
			   && "gutil_text's shadow offset is scale / 8, not a fixed 2 px");
		printf("  MEASURE chat-history shadow at GUI scale %d: text %d px, "
			   "shadow offset %d px, block depth %d px\n",
			   s, 8 * s, s, bottom - 100);
	}
	{
		char* src = read_source("source/game/gui/chat_hud.c");
		const char* fn = strstr(src, "int chat_hud_bottom(int base_y)");
		assert(fn);
		const char* stop = strstr(fn, "\n}");
		assert(stop);
		size_t n = (size_t)(stop - fn);
		char* only = malloc(n + 1);
		assert(only);
		memcpy(only, fn, n);
		only[n] = '\0';
		assert(!strstr(only, "+ 2")
			   && "L1: the shadow extent must not be a hard-coded 2 px");
		assert(strstr(only, "GFX_GUI_SCALE")
			   && "L1: the shadow extent must be derived from the GUI scale");
		free(only);
		free(src);
		src = read_source("source/graphics/gui_util.c");
		assert(strstr(src, "x + scale / 8, y + scale / 8, -2")
			   && "gutil_text's shadow offset is no longer scale / 8");
		free(src);
	}
	/* SINGLE PLAYER AND THE COMPOSER ARE BYTE-IDENTICAL. Two facts, both
	 * pinned at their source: the clamp lives entirely in the CALLER, so
	 * chat_hud_render / chat_hud_render_open cannot have changed behaviour
	 * for anybody; and the in-game call applies it only under the eligible-
	 * multiplayer-session gate, which single player never satisfies. */
	{
		char* src = read_source("source/game/gui/chat_hud.c");
		const char* draw = strstr(src, "static void chat_hud_draw(");
		assert(draw);
		const char* stop = strstr(draw, "\nvoid chat_hud_render(");
		assert(stop);
		char* only = malloc((size_t)(stop - draw) + 1);
		assert(only);
		memcpy(only, draw, (size_t)(stop - draw));
		only[stop - draw] = '\0';
		assert(!strstr(only, "clamp") && !strstr(only, "chat_hud_bottom")
			   && "the clamp must stay OUT of the renderer");
		free(only);
		assert(strstr(src, "chat_hud_render_open(const struct chat_ring_state*")
			   && "the composer entry point is gone");
		free(src);

		src = read_source("source/game/gui/screen_ingame.c");
		const char* fn = strstr(src, "void screen_ingame_render2D_owned(");
		assert(fn);
		const char* call = strstr(fn, "chat_hud_clamp_base_y(");
		assert(call && "the in-game HUD does not clamp its history");
		const char* gate = strstr(fn, "if(gstate.remote_session");
		assert(gate && gate < call && call - gate < 400
			   && "the clamp must sit under the eligible-session gate");
		const char* render = strstr(fn, "chat_hud_render(&gstate.chat_ring");
		assert(render && call < render
			   && "the clamp must be applied before the render");
		free(src);
	}
	puts("PASS chat history clamp: the rendered bottom (strip + gutil_text's "
		 "scale/8 shadow, swept at GUI scales 1-3) is derived from the "
		 "renderer itself, A.3's ceiling is 392 single / "
		 "152 per split viewport, the shipped H-148 anchor already clears it "
		 "by 42 px in both and is returned unchanged, a lower anchor is "
		 "lifted by exactly its overrun, and the clamp lives in the caller "
		 "under the eligible-session gate so the composer and single player "
		 "are untouched");
}

extern void screen_chat_test_layout(int, int, struct menu_composition*,
	struct menu_rect*, struct menu_rect*, int*);
extern void screen_chat_test_hints(int, int, struct chat_geometry*,
	struct gutil_button_bounds*, struct gutil_button_bounds*,
	struct gutil_button_bounds*, bool*);
static void check_chat_renderer(void) {
	const int glyphs[][2] = {{8,2}, {13,12}, {34,35}, {42,42}, {4*64+8,4*64+2}};
	struct chat_composer* chat = &gstate.players[0].chat;
	memset(chat, 0, sizeof(*chat));
	osk_sign_reset(&chat->osk, chat->buf, sizeof(chat->buf), CHAT_COMPOSER_CAP);
	texture_controls.width = texture_controls.height = 256;
	texture_gui_widgets.width = texture_gui_widgets.height = 256;
	chat_recording = true;
	for(size_t family = 0; family < sizeof(glyphs)/sizeof(glyphs[0]); family++) {
		memset(&chat_owner, 0, sizeof(chat_owner)); chat_owner.connected = true;
		chat_owner.glyphs[IB_GUI_QUICK_MOVE] = glyphs[family][0];
		chat_owner.glyphs[IB_INVENTORY] = glyphs[family][1];
		struct menu_composition comp; struct menu_rect input, keys; int lines;
		screen_chat_test_layout(640, 480, &comp, &input, &keys, &lines);
		/* docs/ui_design/chat-keyboard/SPEC.md rect table: the composer's own
		 * notice band above the panel, and the keyboard envelope that now
		 * carries the tabs, the character grid AND the utility row. */
		assert(!comp.notice_in_header);
		assert(comp.notice.x == 44 && comp.notice.y == 24 && comp.notice.w == 552
			   && comp.notice.h == 28);
		/* The reserved notice may not cover a declared control (the header
		 * Send/Cancel targets are declared for exactly this check). */
		assert(menu_composition_notice_conflict(&comp) == comp.action_count);
		assert(keys.x == 56 && keys.y == 204 && keys.w == 528 && keys.h == 216);
		assert(keys.y + keys.h == 420 && "last utility row ends 36 px clear of the 456 title-safe bottom");
		assert(input.x == keys.x && input.y == 164 && input.w == keys.w && input.h == 32);
		assert(lines == 3);
		struct chat_geometry geo; struct gutil_button_bounds send, cancel, type_hint;
		bool type_shown;
		screen_chat_test_hints(640, 480, &geo, &send, &cancel, &type_hint, &type_shown);
		/* 1653f0c74 (WIRING-PLAN Q1/Q6, maintainer picks 10-31): Send and
		 * Cancel are cells of the CENTRED bottom hint row -- the 28 px strip
		 * ending 24 px above the 480 frame (y 428..456), inside the 5 % side
		 * margins -- Send left of Cancel, one row gap (14, or 8 on overflow)
		 * apart. The header Send/Cancel at y132 and the "Type" hint at
		 * (152,132) are retired: no glyph is drawn inside the panel. */
		assert(send.width > 24 && cancel.width > 24 && "Send/Cancel never omitted");
		assert(send.y == 428 && cancel.y == 428 && send.height == 28 && cancel.height == 28);
		assert(cancel.x == send.x + send.width + 14 || cancel.x == send.x + send.width + 8);
		assert(send.x >= 32 + 8 && cancel.x + cancel.width <= 608 - 8);
		assert(!type_shown && type_hint.width == 0 && type_hint.height == 0
			   && "the in-panel Type hint is retired");
		assert(geo.panel.y + geo.panel.h == 432);
		for(int history = 0; history < 2; history++) for(int length = 0; length <= 100; length += 100) {
			memset(&gstate.chat_ring, 0, sizeof(gstate.chat_ring));
			if(history) chat_ring_push(&gstate.chat_ring, "History present");
			memset(chat->buf, 'W', length); chat->buf[length] = 0;
			chat_backdrops = chat_strips = chat_history_strips = chat_drafts = 0;
			chat_counters = 0;
			/* Run twice to catch state left behind by the first frame. */
			for(int frame = 0; frame < 2; frame++) {
				screen_chat.render2D(&screen_chat, 640, 480);
				assert(blend_mode == MODE_OFF && alpha_test_enabled && texture_enabled);
			}
			assert(chat_backdrops == 2 && chat_strips == 2 && chat_drafts == 2);
			assert(chat_counters == 2);
			assert(chat_history_strips == (unsigned)history * 2);
		}
	}
	/* Screen-level rules the review called untested. They live inside
	 * screen_chat_update, which this leaf-wrapped harness does not drive (it
	 * hosts the renderer only), so they are pinned at the call site exactly as
	 * the gameplay HUD rows are above. */
	char* chat_src = read_source("source/game/gui/screen_chat.c");
	/* SPEC "Motion": ONE cue per update, Type and Section outranking Focus. */
	const char* cue = strstr(chat_src, "if(strcmp(before, c->buf) != 0)");
	assert(cue && "the chat sound-priority chain moved");
	assert(strstr(cue, "audio_menu_event(MENU_SOUND_TYPE"));
	const char* sect = strstr(cue, "else if(c->osk.page != page_before)");
	assert(sect && strstr(sect, "audio_menu_event(MENU_SOUND_SECTION"));
	const char* foc = strstr(sect, "else if(focus_moved)");
	assert(foc && strstr(foc, "audio_menu_event(MENU_SOUND_FOCUS")
		   && "FOCUS must stay the LAST branch: Type and Section outrank it");
	assert(strstr(chat_src, "focus_moved = kb.moved && !kb.moved_repeat;")
		   && "a held-repeat step must not raise the focus cue");
	/* The composer runs off player 0's frame; with no device owning player 0
	 * the merged mask is presented AS that frame, so the widget never reads a
	 * global and split-screen isolation holds. */
	assert(strstr(chat_src, "const struct input_player_frame* owner_in = input_player_frame(0);"));
	assert(strstr(chat_src, "merged.pressed[b] = input_pressed((enum input_button)b);"));
	assert(strstr(chat_src, "merged.down[b] = input_held((enum input_button)b);"));
	/* The controller mode is sampled ONCE, in reset, through the core-owned
	 * flag (never by calling the app-only bridge, which compiles out of this
	 * pool), and it is what decides PC text mode. */
	const char* reset = strstr(chat_src, "static void screen_chat_reset(");
	assert(reset);
	const char* sample = strstr(reset, "c->controller_mode = chat_controller_mode_requested();");
	assert(sample && "the controller mode is no longer sampled at open");
	assert(strstr(sample, "input_text_enable(!c->controller_mode);"));
	assert(!strstr(strstr(chat_src, "static void screen_chat_update("),
				   "controller_mode = ")
		   && "the mode must not be re-sampled during the session");
	free(chat_src);
	chat_recording = false;
	puts("PASS chat screen rules pinned: one cue per update (Type/Section > Focus), "
		 "owner-frame input with the merged-mask fallback, controller mode sampled once");
	puts("PASS actual chat renderer: 40 frames, SPEC panel 44,120,552,312 / keyboard 56,204,528,216, counter and draft clip, empty/populated history, 0/100 chars, 5 glyph sets, blend state");
}

/* ---- 5. refused enqueue, at the SCREEN level (Astra ruling 3, items 12-13) -
 *
 * SPEC's "Error/refused enqueue" is `svin_rpc_send` returning false WHILE THE
 * REMOTE SESSION IS STILL ALIVE. No PC rig can stage that: taking the server
 * away ends the session and `screen_chat_update` closes the composer on
 * `!gstate.remote_session` before any send can be refused (measured in
 * tmp/jobs/b1-chat-keyboard-0917/captures/README.md). So the refusal comes
 * from the test-only seam at the composer's single enqueue point
 * (screen_chat.c's chat_enqueue, CAVEWII_TESTING), and everything else on
 * this path is production: the real screen_chat reset/update/render2D, the
 * real owner-frame input, the real key mutator, the real svin transport.
 *
 * What is asserted, one claim at a time:
 *   retained draft, retained key focus, composer stays open (no screen_set),
 *   `Retry` + the focus-yellow input outline for 1200 ms and NOT after,
 *   no success cue on the refusal, and exactly ONE submission when a later
 *   send goes through -- counted at the seam AND drained off the transport. */
static void chat_frame(enum input_button b, float dt) {
	memset(chat_btn_down, 0, sizeof(chat_btn_down));
	memset(chat_btn_edge, 0, sizeof(chat_btn_edge));
	if((int)b < IB_COUNT) chat_btn_down[b] = chat_btn_edge[b] = true;
	screen_chat.update(&screen_chat, dt);
}
static unsigned chat_retry_render(void) {
	chat_retry_labels = chat_retry_outline_quads = chat_counters = 0;
	chat_drafts = chat_backdrops = chat_strips = 0;
	screen_chat.render2D(&screen_chat, 640, 480);
	assert(chat_drafts == 1 && "the draft is drawn on every composer frame");
	return chat_retry_labels;
}
static unsigned chat_sends_seen;
static char chat_last_text[SRPC_CHAT_TEXT_CAP + 1];
static void chat_receive(struct server_rpc* call, void* user) {
	(void)user;
	if(call->type != SRPC_CHAT_SEND) return;
	chat_sends_seen++;
	snprintf(chat_last_text, sizeof(chat_last_text), "%s",
			 call->payload.chat_send.text);
}
static void check_chat_refused_send(void) {
	assert(svin_init());
	struct chat_composer* c = &gstate.players[0].chat;
	memset(c, 0, sizeof(*c));
	memset(&gstate.chat_ring, 0, sizeof(gstate.chat_ring));
	memset(&chat_owner, 0, sizeof(chat_owner));
	chat_owner.connected = true;
	gstate.remote_session = true;
	gstate.players[0].current_screen = NULL;
	chat_recording = chat_driving = true;
	screen_chat_test_enqueue_reset();
	screen_chat.reset(&screen_chat, 640, 480);
	/* The draft, through the production mutator the keyboard itself calls. */
	for(const char* p = "/login hunter22"; *p; p++) osk_feed_char(&c->osk, *p);
	assert(strcmp(c->buf, "/login hunter22") == 0);
	/* One neutral frame satisfies the composer's open-time re-arm, then a real
	 * directional press moves the cursor off the opening cell so "focus
	 * retained" is an observable claim rather than a tautology. */
	chat_frame((enum input_button)IB_COUNT, 0.016F);
	chat_frame(IB_GUI_RIGHT, 0.016F);
	int row = c->osk.row, col = c->osk.col;
	assert(row == OSK_CHAT_GRID_ROW_FIRST && col == 1);

	/* --- the refusal ---------------------------------------------------- */
	screen_chat_test_refuse_sends(1);
	memset(chat_cue_counts, 0, sizeof(chat_cue_counts));
	chat_frame((enum input_button)IB_COUNT, 0.016F);
	chat_frame(IB_GUI_QUICK_MOVE, 0.016F); /* Send */
	assert(screen_chat_test_enqueue_count() == 0 && "nothing reached the transport");
	assert(!c->closed && gstate.players[0].current_screen == NULL
		   && "a refused send must not close the composer");
	assert(c->retry_armed);
	assert(strcmp(c->buf, "/login hunter22") == 0 && "the draft is retained");
	assert(c->osk.row == row && c->osk.col == col && "the key focus is retained");
	assert(chat_cue_counts[MENU_SOUND_SELECT] == 0
		   && chat_cue_counts[MENU_SOUND_BACK] == 0
		   && "no success/close cue may be claimed for a refused send");
	/* The screen keeps running: another update neither closes nor re-sends. */
	chat_frame((enum input_button)IB_COUNT, 0.016F);
	assert(!c->closed && gstate.players[0].current_screen == NULL);
	assert(screen_chat_test_enqueue_count() == 0);

	/* --- the 1200 ms Retry / outline window ----------------------------- */
	assert(chat_retry_render() == 1 && chat_retry_outline_quads == 4
		   && chat_counters == 0
		   && "Retry replaces n/100 and the input is outlined");
	thread_msleep(600);
	assert(time_diff_ms(c->retry_at, time_get()) < CHAT_RETRY_MS);
	assert(chat_retry_render() == 1 && chat_retry_outline_quads == 4
		   && "the state holds for the whole window, it is not a one-frame blip");
	while(time_diff_ms(c->retry_at, time_get()) < CHAT_RETRY_MS)
		thread_msleep(50);
	assert(chat_retry_render() == 0 && chat_retry_outline_quads == 0
		   && chat_counters == 1
		   && "after CHAT_RETRY_MS the counter is back and the outline is gone");
	assert(c->retry_armed && strcmp(c->buf, "/login hunter22") == 0
		   && "the WINDOW expires, the draft does not");

	/* --- the later send: EXACTLY ONE submission ------------------------- */
	memset(chat_cue_counts, 0, sizeof(chat_cue_counts));
	chat_sends_seen = 0; chat_last_text[0] = 0;
	chat_frame((enum input_button)IB_COUNT, 0.016F);
	chat_frame(IB_GUI_QUICK_MOVE, 0.016F);
	assert(screen_chat_test_enqueue_count() == 1 && "one enqueue, not two");
	assert(c->closed && chat_cue_counts[MENU_SOUND_SELECT] == 1);
	svin_process_messages(chat_receive, NULL, false);
	assert(chat_sends_seen == 1 && "exactly one SRPC_CHAT_SEND on the transport");
	assert(strcmp(chat_last_text, "/login hunter22") == 0);

	chat_recording = chat_driving = false;
	gstate.remote_session = false;
	screen_chat_test_enqueue_reset();
	puts("PASS chat refused send (test-only enqueue seam): draft and key focus "
		 "retained, composer stays open, no success cue, Retry + 2-px outline "
		 "for 1200 ms then cleared, later send = exactly ONE SRPC_CHAT_SEND "
		 "\"/login hunter22\"");
}

static void check_chat_pointer_focus(void) {
	chat_recording = chat_driving = true;
	gstate.remote_session = true;
	screen_chat.reset(&screen_chat, 640, 480);
	struct chat_composer* c = &gstate.players[0].chat;
	for(const char* p = "safe"; *p; p++) osk_feed_char(&c->osk, *p);
	chat_pointer_valid = true;
	chat_pointer_x = 240; chat_pointer_y = 264; /* resting over r */
	chat_frame((enum input_button)IB_COUNT, .016F);
	assert(c->osk.col == 0 && !c->focus.pointer_owner);
	chat_frame(IB_GUI_RIGHT, .016F);
	assert(c->osk.col == 1 && !c->focus.pointer_owner);
	/* Held direction continues while the stationary pointer stays over r. */
	memset(chat_btn_edge, 0, sizeof(chat_btn_edge));
	screen_chat.update(&screen_chat, .30F);
	assert(c->osk.col == 2 && !c->focus.pointer_owner);
	chat_frame((enum input_button)IB_COUNT, .016F);
	chat_pointer_x += 3; /* below the 8-pixel purposeful-motion threshold */
	chat_frame((enum input_button)IB_COUNT, .016F);
	assert(c->osk.col == 2 && !c->focus.pointer_owner);
	chat_pointer_x = 290;
	chat_frame((enum input_button)IB_COUNT, .016F);
	assert(c->osk.col == 4 && c->focus.pointer_owner);
	chat_frame(IB_GUI_LEFT, .016F);
	assert(c->osk.col == 3 && !c->focus.pointer_owner);
	chat_pointer_valid = false;
	chat_frame((enum input_button)IB_COUNT, .016F);
	assert(c->osk.col == 3 && strcmp(c->buf, "safe") == 0);
	chat_pointer_valid = true; chat_pointer_x = 500;
	chat_frame((enum input_button)IB_COUNT, .016F);
	assert(c->osk.col == 3 && !c->focus.pointer_owner);
	chat_pointer_valid = false;
	chat_recording = chat_driving = false;
	gstate.remote_session = false;
	puts("PASS actual chat focus: resting/jitter/lost/reacquired pointer preserves directional focus; purposeful motion takes over");
}
static void check_chat_header_activation_order(void) {
	struct chat_geometry geo;
	struct gutil_button_bounds send,cancel,type_hint;
	bool type_shown=false;
	/* 1653f0c74: Send/Cancel are hint-row cells laid out from the owner's
	 * bound glyphs, so the frame must be the recorded owner WITH glyphs. */
	chat_recording=chat_driving=true;
	memset(&chat_owner,0,sizeof(chat_owner)); chat_owner.connected=true;
	chat_owner.glyphs[IB_GUI_QUICK_MOVE]=8; chat_owner.glyphs[IB_INVENTORY]=2;
	screen_chat_test_hints(640,480,&geo,&send,&cancel,&type_hint,&type_shown);
	assert(send.width>0&&cancel.width>0);
	gstate.remote_session=true;
	screen_chat.reset(&screen_chat,640,480);
	struct chat_composer* c=&gstate.players[0].chat;
	chat_pointer_valid=true;
	chat_pointer_x=send.x+send.width/2;
	chat_pointer_y=send.y+send.height/2;
	chat_frame((enum input_button)IB_COUNT,.016F);
	chat_frame(IB_GUI_CLICK,.016F);
	assert(!c->closed&&c->buf[0]=='\0'); /* first mismatch only focuses Send */
	chat_frame((enum input_button)IB_COUNT,.016F);
	chat_frame(IB_GUI_RIGHT,.016F);
	chat_frame((enum input_button)IB_COUNT,.016F);
	chat_frame(IB_GUI_CLICK,.016F);
	assert(!c->closed&&strcmp(c->buf,"w")==0);
	chat_frame((enum input_button)IB_COUNT,.016F);
	chat_pointer_x=cancel.x+cancel.width/2;
	chat_pointer_y=cancel.y+cancel.height/2;
	chat_frame((enum input_button)IB_COUNT,.016F);
	chat_frame(IB_GUI_CLICK,.016F);
	assert(c->closed);
	chat_pointer_valid=false;
	chat_recording=chat_driving=false;
	gstate.remote_session=false;
	puts("PASS chat last input wins: ring key under resting Send; moved sprite Cancel");
}


/* Fable G4 review item 1: a pointer that owns focus over BLANK keeps the last
 * real target. Pointing the Remote away then gives A to the keyboard (rule
 * (3)) -- before the fix the blank hover armed NONE, A did NOTHING, and the
 * header consumed it, so the OSK never typed. */
static void check_chat_blank_hover_then_away_types(void) {
	chat_recording=chat_driving=true;
	gstate.remote_session=true;
	screen_chat.reset(&screen_chat,640,480);
	struct chat_composer* c=&gstate.players[0].chat;
	chat_pointer_valid=true;
	chat_pointer_x=4;chat_pointer_y=4; /* blank: no header control, no key */
	chat_frame((enum input_button)IB_COUNT,.016F);
	chat_pointer_x=44; /* purposeful motion: the sprite owns focus over blank */
	chat_frame((enum input_button)IB_COUNT,.016F);
	assert(c->focus.pointer_owner);
	assert(c->menu_target==2 && "a blank hover discarded the keyboard target");
	chat_pointer_valid=false; /* Remote pointed away */
	chat_frame((enum input_button)IB_COUNT,.016F);
	chat_frame(IB_GUI_CLICK,.016F);
	assert(!c->closed&&strcmp(c->buf,"q")==0
		&& "A after a blank hover and pointer loss did not type");
	chat_recording=chat_driving=false;
	gstate.remote_session=false;
	puts("PASS chat blank hover then pointer away: A types the keyboard target");
}

static void check_weather_overlay_row(void) {
	char* src = read_source("source/game/gui/screen_ingame.c");
	const char* wx = strstr(
		src, "\"spawner: %dus %dn  wx r%.2f b%d s%d f%d w%d\"");
	assert(wx);
	assert(strstr(wx, "\"wx r%.2f b%d s%d f%d w%dc\"")
		&& "client fallback must be marked with trailing c");
	const char* next_row = strstr(wx, "// Row 12:");
	assert(next_row);
	const char* draw = strstr(wx, "gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * ");
	int wx_row = -1;
	assert(draw && draw < next_row);
	assert(sscanf(draw, "gutil_text(4, 4 + (GFX_GUI_SCALE * 8 + 1) * %d", &wx_row) == 1);
	assert(wx_row == 11 && "weather must render on row 11");
	const char* guard = strstr(wx, "if(4 + gutil_font_width(str, GFX_GUI_SCALE * 8) > width - 4)");
	const char* clamp = strstr(wx, "while(str[0] && 4 + gutil_font_width(str, GFX_GUI_SCALE * 8) > width - 4)");
	assert(guard && guard < next_row && clamp && clamp < next_row);
	const char* weather = "spawner: 999us 99n  wx r1.00 b1 s1 f600 w1";
	const int scale = 2, size = scale * 8, pitch = size + 1;
	const int hotbar_x = (640 - 182 * scale) / 2;
	const int hotbar_y = screen_ingame_hotbar_top(480, scale);
	const int heart_y = hotbar_y - 10 * scale;
	const struct { const char* name; int x, y, w, h; } hud[] = {
		{"hotbar", hotbar_x, hotbar_y, 182 * scale, 22 * scale},
		{"hearts", hotbar_x, heart_y, 81 * scale, 11 * scale},
		{"air", hotbar_x + 101 * scale, heart_y, 81 * scale, 9 * scale},
		{"armor", hotbar_x, heart_y - 9 * scale, 81 * scale, 9 * scale},
		{"bottom band", 0, hotbar_y, 640, 480 - hotbar_y},
	};
	const int wx_width = gutil_font_width(weather, size);
	printf("  F3 row 11 maximal width %d\n", wx_width);
	fflush(stdout); /* c54 */
	assert(wx_width == 468 && "weather row maximum changed; re-audit 640px fit");
	assert(4 + wx_width <= 636 && "maximal spawner and wx values must fit 640px");
	for(size_t i = 0; i < sizeof(hud) / sizeof(hud[0]); ++i)
		assert(!rects_overlap(4, 4 + pitch * wx_row, wx_width, size,
				hud[i].x, hud[i].y, hud[i].w, hud[i].h)
			   && "weather row intersects survival HUD");
	/* Existing HEAD layout: region io is row 22 and path is row 23.
	 * Their survival-HUD intersections are expected and reported here until
	 * the separate layout repair card handles them. */
	const struct { int row; const char* name; const char* text; } lower[] = {
		{22, "region io", "region io: fopen 999 fread 999 bytes 999KB nbt-full 99"},
		{23, "path", "path: 99 srch/tk, 999 nodes (peak 999)"},
	};
	for(size_t r = 0; r < sizeof(lower) / sizeof(lower[0]); ++r) {
		int overlaps = 0;
		for(size_t i = 0; i < sizeof(hud) / sizeof(hud[0]); ++i)
			if(rects_overlap(4, 4 + pitch * lower[r].row,
				gutil_font_width(lower[r].text, size), size,
				hud[i].x, hud[i].y, hud[i].w, hud[i].h)) {
				printf("  EXPECTED overlap: row %d %s vs %s\n",
					lower[r].row, lower[r].name, hud[i].name);
				++overlaps;
			}
		assert(overlaps && "known lower-row overlap changed");
	}
	free(src);
}

/* Record the old single-player composition before exercising split menus. */
static void check_creative_picker_geometry(void) {
	used = 0; calls[0] = 0; recording = true;
	screen_creative_picker.render2D(&screen_creative_picker, 640, 480);
	recording = false;
	uint64_t hash = UINT64_C(14695981039346656037);
	for(size_t i = 0; i < used; i++) {
		hash ^= (unsigned char)calls[i];
		hash *= UINT64_C(1099511628211);
	}
	/* Captured before ci-1101/35 production edits (12169-byte draw tape). */
	assert(used == 12169 && hash == UINT64_C(1212067025779354361));
	printf("creative SP trace: %llu bytes, hash %llu\n",
		(unsigned long long)used, (unsigned long long)hash);
	struct client_session host = {0}, guest = {0};
	struct container_glyphs glyphs = {0};
	gstate.config_creative = true;
	assert(local_menu_host_open_creative(&guest, 1, 640, 240));
	used = 0; calls[0] = 0; recording = true;
	local_menu_host_render(&host, &glyphs);
	assert(used == 0); /* P1 retains main's world/HUD branch. */
	hud_recording = true; hud_hotbar_quads = 0;
	local_menu_host_render(&guest, &glyphs);
	hud_recording = false; recording = false;
	assert(used > 0 && hud_hotbar_quads == 0);
	unsigned entries = 0;
	for(const char* at = calls; (at = strstr(at, "item ")); at += 5) {
		unsigned id, count, damage; int x, y, layer;
		assert(sscanf(at, "item %u %u %u %d %d %d", &id, &count,
			&damage, &x, &y, &layer) == 6);
		assert(x >= 0 && x + 32 <= 640 && y >= 32 && y + 32 <= 112);
		entries++;
	}
	assert(entries == 18); /* readable 9x2 composition, not a squeezed 9x5 */
	for(const char* at = calls; (at = strstr(at, " | ")); at += 3) {
		int x, y, z;
		assert(sscanf(at + 3, "%d,%d,%d", &x, &y, &z) == 3);
		assert(x >= 0 && x <= 640 && y >= 0 && y <= 240);
	}
	struct input_inventory notices = {0};
	notices.notices[0] = (struct input_connection_notice){
		.expires = time_add_ms(time_get(), 60000), .kind = INPUT_DEVICE_WPAD,
		.channel = 1, .player = 1, .event = CONTROLLER_EVENT_LOW_BATTERY,
		.category = INPUT_CAT_NUNCHUK, .active = 1, .connected = true};
	gutil_join_inventory_override(&notices);
	memset(&gutil_test_last_notice, 0, sizeof gutil_test_last_notice);
	local_menu_host_render(&guest, &glyphs);
	assert(gutil_test_last_notice.w > 0 && gutil_test_last_notice.x >= 140);
	assert(gutil_test_last_notice.y >= 8
		&& gutil_test_last_notice.y + gutil_test_last_notice.h <= 32);
	gutil_join_inventory_override(NULL);
	local_menu_host_reset(&guest);
	gstate.config_creative = false;
	puts("PASS creative picker: SP tape unchanged; P2 compact drawing stays inside its pane without HUD");

}

int main(void) {
	/* Draw recorder: represent a loaded fallback sheet; graphics calls are wrapped. */
	static uint8_t controls_texel[4];
	texture_controls.data = controls_texel;
	gstate.config_user.root = json_value_init_object();
	texture_font.width = texture_font.height = 128; gutil_reset_font(&texture_font);
	assert(gutil_font_width("Sort", 16) == 44);
	blocks_init(); items_init();
	struct entity hud_body = {0}; hud_body.health = 160;
	gstate.players[0].local_player = &hud_body;
	check_creative_picker_geometry();
	check_compact_inventory();
	check_pause_composition();
	check_controller_picture_slots();
	check_footer_widths();
	check_drop_prompt();
	check_manual_grid_footer();
	check_sneak_rail_geometry();
	check_sneak_marker_rules();
	check_sneak_rail_source_pin();
	check_leave_bed_clearance();
	check_hint_row_helper();
	check_classic_keycap_letters();
	check_notice_card_centre();
	check_pc_cap_glyphs();
	check_menu_hud_clearance();
	check_full_chest_paged();
	check_card_hides_pane_hud();
	check_disconnect_card();
	check_coords_fps_rect();
	check_hint_row_rules();
	check_hint_row_bindings();
	check_hud_source_pin();
	check_weather_overlay_row();
	check_chat_socket();
	check_chat_socket_rules();
	check_chat_socket_capture();
	check_hud_blend_state();
	check_hud_modal_suppression();
	check_socket_capture_cache();
	check_chat_history_clamp();
	check_chat_renderer();
	check_chat_refused_send();
	check_chat_pointer_focus();
	check_chat_header_activation_order();
	check_chat_blank_hover_then_away_types();
	puts("PASS test_ui_geometry");
	return 0;
}
