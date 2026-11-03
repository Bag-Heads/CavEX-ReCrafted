/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../graphics/gui_util.h"
#include "hud_survival_row.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/render_model.h"
#include "../../platform/gfx.h"
#include "../../platform/menu_sound.h"
#include "../../platform/input_glyph.h"
#include "../../platform/input_owner.h"
#include "../../platform/texture.h"
#include "../game_state.h"
#include "container_compact.h"
#include "local_menu_host.h"
#include "menu_header.h"
#include "screen.h"

/* ONE authority for "is this the compact/split composition": menu_header.c,
 * which the header/notice composition and its golden also use. */
enum container_layout_mode container_layout_for_height(int height) {
	return menu_screen_layout_for(640, height) == MENU_SCREEN_FULL ?
		CONTAINER_LAYOUT_FULL :
		CONTAINER_LAYOUT_COMPACT;
}

static void add_slot(struct container_compact* v, int x, int y, size_t slot) {
	assert(v->count < sizeof(v->slots) / sizeof(v->slots[0]));
	v->slots[v->count++] = (struct container_slot) {
		x + (v->width - 640) / 2, y + (v->height - 240) / 2, slot, 0,
	};
}

static void fill_slots(struct container_compact* v) {
 v->count=0;
 if(v->type==WINDOW_TYPE_INVENTORY){
  for(size_t k=0;k<36;k++)add_slot(v,272+(k%9)*36,40+(k/9)*36+(k>=27?4:0),v->main_base+k);
  /* ui-build-1026/T13, approved V1 split page (design-round-1026e
   * v1-5-split-inventory): armor, portrait, the REAL 2x2 craftMatrix and its
   * output share ONE page in the 44..268 column on 26 px cells, so the
   * Crafting page (PAGE_SWAP) is the only other page. */
  for(size_t k=0;k<4;k++){add_slot(v,44,46+(int)k*26,INVENTORY_SLOT_ARMOR+k);v->slots[v->count-1].extent=26;}
  for(size_t k=0;k<4;k++){add_slot(v,138+(int)(k%2)*26,64+(int)(k/2)*26,INVENTORY_SLOT_CRAFTING+k);v->slots[v->count-1].extent=26;}
  add_slot(v,220,76,INVENTORY_SLOT_OUTPUT);v->slots[v->count-1].extent=26;
  return;
 }
 if(v->type==WINDOW_TYPE_FURNACE||v->type==WINDOW_TYPE_DISPENSER) {
  bool full=v->height>=480;int ox=(v->width-640)/2,oy=(v->height-(full?480:240))/2;
  /* B3 (10-31): full-screen panels sit 14 px higher (44 -> 30) so they end
  * above the gameplay hearts that hud_menu_draw_owned keeps on screen. */
  int bx=ox+(full?158:272),by=oy+(full?206:40);
  if(v->type==WINDOW_TYPE_FURNACE) {
   int mx=ox+(full?248:74),my=oy+(full?71:55);
   v->slots[v->count++]=(struct container_slot){mx,my,FURNACE_SLOT_INPUT,0};
   v->slots[v->count++]=(struct container_slot){mx,my+60,FURNACE_SLOT_FUEL,0};
   v->slots[v->count++]=(struct container_slot){mx+108,my+30,FURNACE_SLOT_OUTPUT,0};
  } else {
   int mx=ox+(full?266:92),my=oy+(full?65:49);
   for(size_t k=0;k<9;k++)v->slots[v->count++]=(struct container_slot){mx+(int)(k%3)*36,my+(int)(k/3)*36,k,0};
  }
  for(size_t k=0;k<36;k++)v->slots[v->count++]=(struct container_slot){
   bx+(int)(k%9)*36,by+(int)(k/9)*36+(k>=27?4:0),v->main_base+k,0};
  return;
 }
	/* chest.html:23-24,152: 324px grid at panel +(12,32), hotbar +112. */
	if(v->type == WINDOW_TYPE_CHEST) {
		size_t first = v->view ? (v->view - 1) * 27 : v->main_base;
		for(size_t k = 0; k < 27; k++)
			add_slot(v, 46 + (k % 9) * 36, 42 + (k / 9) * 36, first + k);
		for(size_t k = 0; k < 9; k++)
			add_slot(v, 46 + k * 36, 154, v->main_base + 27 + k);
		return;
	}
	if(v->view == 0) {
		for(size_t k = 0; k < 27; k++)
			add_slot(v, 160 + (k % 9) * 36, 44 + (k / 9) * 36, v->main_base + k);
	} else if(v->storage_count) {
		size_t first = (v->view - 1) * 27;
		int columns = v->type == WINDOW_TYPE_DISPENSER ? 3 : 9;
		int left = columns == 3 ? 268 : 160;
		for(size_t k = first; k < v->storage_count && k < first + 27; k++)
			add_slot(v, left + ((k - first) % columns) * 36,
				44 + ((k - first) / columns) * 36, k);
	} else if(v->type == WINDOW_TYPE_INVENTORY) {
		for(size_t k = 0; k < 4; k++)
			add_slot(v, 172 + k * 36, 44, INVENTORY_SLOT_ARMOR + k);
		for(size_t k = 0; k < 4; k++)
			add_slot(v, 232 + (k % 2) * 36, 80 + (k / 2) * 36, INVENTORY_SLOT_CRAFTING + k);
		add_slot(v, 412, 98, INVENTORY_SLOT_OUTPUT);
	} else if(v->type == WINDOW_TYPE_WORKBENCH) {
		for(size_t k = 0; k < 9; k++)
			add_slot(v, 196 + (k % 3) * 36, 44 + (k / 3) * 36, CRAFTING_SLOT_INPUT + k);
		add_slot(v, 412, 80, CRAFTING_SLOT_OUTPUT);
	} else if(v->type == WINDOW_TYPE_FURNACE) {
		add_slot(v, 232, 44, FURNACE_SLOT_INPUT);
		add_slot(v, 232, 116, FURNACE_SLOT_FUEL);
		add_slot(v, 412, 80, FURNACE_SLOT_OUTPUT);
	}
	for(size_t k = 0; k < 9; k++)
		add_slot(v, 160 + k * 36, 156, v->main_base + 27 + k);
}

bool container_compact_open(struct container_compact* v,
	enum window_type type, size_t slot_count, size_t hotbar,
	int width, int height) {
	*v = (struct container_compact) {0};
 bool machine=type==WINDOW_TYPE_FURNACE||type==WINDOW_TYPE_DISPENSER;
 if(width<640||height<240||(!machine&&container_layout_for_height(height)!=CONTAINER_LAYOUT_COMPACT))return false;
	size_t main_base, storage = 0;
	switch(type) {
		case WINDOW_TYPE_INVENTORY: main_base = INVENTORY_SLOT_MAIN; break;
		case WINDOW_TYPE_WORKBENCH: main_base = CRAFTING_SLOT_MAIN; break;
		case WINDOW_TYPE_FURNACE: main_base = FURNACE_SLOT_MAIN; break;
		case WINDOW_TYPE_DISPENSER:
			main_base = storage = DISPENSER_SIZE_STORAGE; break;
		case WINDOW_TYPE_CHEST:
			if(slot_count != 63 && slot_count != 90) return false;
			main_base = storage = slot_count - 36; break;
		case WINDOW_TYPE_IRON_CHEST:
			main_base = storage = IRON_CHEST_SIZE_STORAGE; break;
		default: return false;
	}
	if(slot_count != main_base + 36)
		return false;
	v->active = true;
	v->type = type;
	v->width = width; v->height = height;
	v->main_base = main_base; v->storage_count = storage;
	v->view_count = storage ? 1 + (storage + 26) / 27
		: type == WINDOW_TYPE_INVENTORY ? 1 : 2;
	v->view = type == WINDOW_TYPE_INVENTORY ? 0 : 1;
	v->selection[0] = 27 + (hotbar < 9 ? hotbar : 0);
	fill_slots(v);
 if(machine){
  v->view_count=1;v->view=0;v->menu.slot_extent=36;
  v->selection[0]=(type==WINDOW_TYPE_FURNACE?3:9)+27+(hotbar<9?hotbar:0);
 }
 if(type==WINDOW_TYPE_INVENTORY)v->menu.slot_extent=36;
 v->menu.selected=v->selection[v->view];
	return true;
}

/* T12 fix r1: put directional focus on one window slot, switching to the
 * compact view that shows it (the personal 2x2 grid lives on view 1). */
bool container_compact_focus_slot(struct container_compact* v, size_t slot) {
	if(!v || !v->active) return false;
	unsigned original = v->view;
	v->selection[v->view] = v->menu.selected;
	for(unsigned n = 0; n < v->view_count; n++) {
		v->view = (original + n) % v->view_count;
		fill_slots(v);
		for(size_t k = 0; k < v->count; k++) {
			if(v->slots[k].slot != slot) continue;
			v->menu.selected = k;
			v->menu.selection_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
			v->menu.focus = (struct gutil_menu_focus) {.initialized = true};
			v->menu.repeat = (struct gutil_menu_repeat) {0};
			v->menu.pointer_on_slot = false;
			return true;
		}
	}
	v->view = original;
	fill_slots(v);
	v->menu.selected = v->selection[v->view] < v->count ? v->selection[v->view] : 0;
	return false;
}

static const char* chest_title(const struct container_compact* v) {
	return !v->view ? "Inventory" : v->view == 2 ? "Chest 2"
		: v->storage_count == 54 ? "Chest 1" : "Chest";
}

void container_compact_notice_bounds(const struct container_compact* v,
 const struct window_container* wc, int desired, struct gutil_button_bounds* out) {
 *out=(struct gutil_button_bounds){0};
 if(!v||!wc||!v->active||v->height>=480||desired<=0)return;
 int ox=(v->width-640)/2,oy=(v->height-240)/2;
 int left=ox+44,right=ox+596;
 if(v->menu.sort_open)left+=gutil_font_width("Sort",16)+8;
 else if(v->type==WINDOW_TYPE_INVENTORY)left=ox+44+container_page_header_width(false)+8;
 else if(v->type==WINDOW_TYPE_CHEST) {
  struct gutil_button_bounds header[4];
  container_chest_header_rects(ox+32,oy+8,576,chest_title(v),header);
  left=header[2].x+header[2].width+8;
 } else if(v->type==WINDOW_TYPE_FURNACE||v->type==WINDOW_TYPE_DISPENSER)
  left+=gutil_font_width(v->type==WINDOW_TYPE_FURNACE?"Furnace":"Dispenser",16)+8;
 else {struct menu_rect next=menu_container_next_box(ox,oy);left=next.x+next.w+8;}
 if(!v->menu.sort_open&&!v->menu.sort_disabled&&container_menu_sort_supported(wc)) {
  struct container_menu layout=v->menu;struct gutil_button_bounds entry;
  container_menu_sort_layout(&layout,v->width,v->height,wc);
  container_menu_sort_entry_bounds(&layout,wc,0,0,&entry);right=entry.x-8;
 }
 int available=right>left?right-left:0,w=desired<available?desired:available;
 *out=(struct gutil_button_bounds){right-w,oy+16,w,24};
}

#define glyph_frame container_compact_glyph_frame
static enum container_row_kind compact_row_kind(const struct container_compact* v);

bool container_compact_step(struct container_compact* v,
	struct window_container* wc, uint8_t window,
	const struct container_input* input, float dt,
	container_send_fn send, void* user) {
	if(!v->active || !wc || wc->type != v->type
	   || wc->slot_count != v->main_base + 36)
		return false;
	struct container_input local = *input;
	container_menu_sort_layout(&v->menu, v->width, v->height, wc);
	local.pointer_valid = local.pointer_valid && local.pointer_x >= 0
		&& local.pointer_x < v->width && local.pointer_y >= 0
		&& local.pointer_y < v->height;
	/* The bottom hint row is a pointer target (WIRING-PLAN Q2), resolved
	 * from the same layout the painter uses, with the OWNER's glyphs. */
	if(!v->menu.sort_open && (v->menu.focus.pointer_owner || !v->menu.focus.initialized)) {
		unsigned owner = 0;
		if(v->menu.sort_owner && local_menu_host_active(v->menu.sort_owner))
			owner = v->menu.sort_owner->menu_host.player;
		struct container_glyphs g;
		container_compact_owner_glyphs(owner, &g);
		struct input_player_frame frame;
		glyph_frame(&g, &frame);
		container_row_click(compact_row_kind(v),
			inventory_get_picked_item(windowc_get_latest(wc), NULL), v->view_count > 1,
			&frame, 0, 0, v->width, v->height, &local);
	}
	/* The labelled previous/next controls also have IR/mouse hit targets. */
	if(v->view_count > 1 && !v->menu.sort_open && local.pick && local.pointer_valid
	   && (v->menu.focus.pointer_owner || !v->menu.focus.initialized)
	   && !local.up && !local.down && !local.left && !local.right) {
		float px = local.pointer_x - (v->width - 640) / 2;
		float py = local.pointer_y - (v->height - 240) / 2;
		if(v->type == WINDOW_TYPE_CHEST) {
			struct gutil_button_bounds r[4];
			container_chest_header_rects(32, 8, 576, chest_title(v), r);
			local.previous |= gutil_button_hit(&r[0], px, py);
			local.page |= gutil_button_hit(&r[2], px, py);
		} else if(py >= 16 && py < 48) {
			/* Hit targets follow the shared header boxes, so the pointer can
			 * never activate Next through the reserved notice slot. */
			struct menu_rect prev = menu_container_prev_box(0, 0);
			struct menu_rect next = menu_container_next_box(0, 0);
			local.previous |= px >= prev.x && px < prev.x + prev.w;
			bool sort_hit = container_menu_sort_supported(wc)
				&& px >= 596 - gutil_font_width("Sort", 16) - 12 && px < 596
				&& py < 40;
			local.page |= px >= next.x && px < next.x + next.w && !sort_hit;
		}
	}
	if(v->view_count > 1 && !v->menu.sort_open && (local.page || local.previous) && !local.close && !local.suspended
	   && !v->menu.close_requested) {
		windowc_sort_interrupt(wc);
		v->menu.sort_focus = false;
		v->selection[v->view] = v->menu.selected;
		if(local.page != local.previous) audio_menu_event(MENU_SOUND_SECTION, time_get());
		if(local.page != local.previous)
			v->view = (v->view + (local.previous ? v->view_count - 1 : 1)) % v->view_count;
		fill_slots(v);
		v->menu.selected = v->selection[v->view] < v->count ? v->selection[v->view] : 0;
		v->menu.selection_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
		v->menu.focus = (struct gutil_menu_focus) {.initialized = true};
		v->menu.repeat = (struct gutil_menu_repeat) {0};
		v->menu.pointer_on_slot = false;
		return false; /* Switching views never also clicks, drops or transfers. */
	}
	/* T13: the one-page split inventory keeps Previous/Next as the SAME
	 * section jump the full inventory uses (container_menu_update), never a
	 * page change -- PAGE_SWAP is the only way to Crafting. */
	if(v->type==WINDOW_TYPE_INVENTORY&&v->view_count==1) {
		if(!v->menu.sort_open&&(local.page||local.previous)&&!local.close)windowc_sort_interrupt(wc);
		if(!local.suspended&&!v->menu.close_requested
		   &&container_menu_section_step(&v->menu,v->slots,v->count,v->main_base,&local))
			return false;
	}
	if(v->view_count==1)local.page=local.previous=false;
	return container_menu_step(&v->menu, v->slots, v->count, 0, 0,
		wc, window, &local, dt, send, user);
}

/* Sol review 10-31 item 1: the split inventory's "Crafting" row cell is a
 * page swap, but local_menu_host_update tests page_swap BEFORE
 * container_compact_step resolves the row click, so the click was eaten.
 * This pure query answers "does this frame's pointer press hit that cell",
 * from the same layout the painter and the step use, for the host to OR into
 * its page_swap test. */
bool container_compact_row_page_swap(const struct container_compact* v,
	const struct window_container* wc, const struct container_glyphs* glyphs,
	const struct container_input* in) {
	if(!v || !v->active || !wc || !glyphs || !in || v->type != WINDOW_TYPE_INVENTORY
	   || v->menu.sort_open || !(v->menu.focus.pointer_owner || !v->menu.focus.initialized))
		return false;
	struct container_input local = *in;
	local.pointer_valid = local.pointer_valid && local.pointer_x >= 0
		&& local.pointer_x < v->width && local.pointer_y >= 0 && local.pointer_y < v->height;
	struct input_player_frame frame;
	glyph_frame(glyphs, &frame);
	return container_row_click(CONTAINER_ROW_INVENTORY,
		inventory_get_picked_item(windowc_get_latest((struct window_container*)wc), NULL),
		false, &frame, 0, 0, v->width, v->height, &local) == CONTAINER_ROW_PAGE_SWAP;
}

void container_compact_player_glyphs(struct container_glyphs* glyphs,
	const struct input_player_frame* frame) {
	const enum input_button actions[] = {IB_GUI_QUICK_MOVE, IB_GUI_CLICK,
		IB_GUI_SPLIT, IB_GUI_DROP, IB_INVENTORY, IB_GUI_PREVIOUS, IB_GUI_PAGE,
		IB_GUI_PAGE_SWAP};
	_Static_assert(sizeof(actions) / sizeof(actions[0]) == CONTAINER_PROMPT_COUNT,
		"one input role per container prompt");
	for(int k = 0; k < CONTAINER_PROMPT_COUNT; k++)
		/* The owner retains its last known controller through connection loss. */
		glyphs->symbols[k] = frame ? frame->glyphs[actions[k]] : -1;
}

void container_compact_owner_glyphs(unsigned player,
	struct container_glyphs* glyphs) {
	struct input_player_frame frame;
	input_prompt_frame(player, &frame);
	container_compact_player_glyphs(glyphs, &frame);
}

/* The global (player 1) container screens. Split panes go through
 * local_menu_host with their owner's frame (container_compact_owner_glyphs /
 * container_compact_player_glyphs). */
void container_compact_active_glyphs(struct container_glyphs* glyphs) {
	container_compact_owner_glyphs(0, glyphs);
}

/* The owner's resolved glyphs as a prompt frame for the shared hint row. */
void container_compact_glyph_frame(const struct container_glyphs* glyphs,
	struct input_player_frame* prompts) {
	*prompts = (struct input_player_frame) {0};
	for(int b = 0; b < IB_COUNT; b++) prompts->glyphs[b] = -1;
	prompts->glyphs[IB_GUI_CLICK] = glyphs->symbols[CONTAINER_PROMPT_PICK];
	prompts->glyphs[IB_INVENTORY] = glyphs->symbols[CONTAINER_PROMPT_BACK];
	prompts->glyphs[IB_GUI_SPLIT] = glyphs->symbols[CONTAINER_PROMPT_SPLIT];
	prompts->glyphs[IB_GUI_QUICK_MOVE] = glyphs->symbols[CONTAINER_PROMPT_QUICK];
	prompts->glyphs[IB_GUI_DROP] = glyphs->symbols[CONTAINER_PROMPT_DROP];
	prompts->glyphs[IB_GUI_PREVIOUS] = glyphs->symbols[CONTAINER_PROMPT_PREVIOUS];
	prompts->glyphs[IB_GUI_PAGE] = glyphs->symbols[CONTAINER_PROMPT_NEXT];
	prompts->glyphs[IB_GUI_PAGE_SWAP] = glyphs->symbols[CONTAINER_PROMPT_PAGE_SWAP];
}

/* The row's kind for this compact view. Chests and the paged generic views
 * (workbench) show the Section pair only when there is a section to change. */
static enum container_row_kind compact_row_kind(const struct container_compact* v) {
	if(v->type == WINDOW_TYPE_INVENTORY) return CONTAINER_ROW_INVENTORY;
	if(v->type == WINDOW_TYPE_FURNACE || v->type == WINDOW_TYPE_DISPENSER)
		return CONTAINER_ROW_MACHINE;
	return CONTAINER_ROW_STORAGE;
}

static void rect(int x, int y, int width, int height,
	uint8_t gray, uint8_t alpha) {
	gfx_texture(false);
	gutil_texquad_col(x, y, 0, 0, 0, 0, width, height, gray, gray, gray, alpha);
	gfx_texture(true);
}

static void sidebar(int x, int y, const char* text) {
	int bottom = y + 114;
	while(*text) {
		char line[128];
		size_t n = 0, word = 0;
		while(text[n] && n < sizeof(line) - 1) {
			line[n] = text[n]; line[n + 1] = '\0';
			if(gutil_font_width(line, 16) > 112) break;
			if(text[n] == ' ') word = n;
			n++;
		}
		if(text[n] && word) n = word;
		if(!n) n = 1;
		line[n] = '\0';
		gutil_text(x, y, line, 16, true);
		text += n;
		while(*text == ' ') text++;
		y += 18;
		if(y > bottom) break;
	}
}

/* Approved machine layouts retain the work area and all 36 carried slots. */
static void machine_panel(int x,int y,int w,int h) {
 rect(x-2,y-2,w+4,h+4,17,255);
 gfx_bind_texture(&texture_gui_crafting);
 for(int yy=0;yy<h;yy+=32)for(int xx=0;xx<w;xx+=32){
  int dw=w-xx<32?w-xx:32,dh=h-yy<32?h-yy:32;
  gutil_texquad(x+xx,y+yy,150,62,(dw+1)/2,(dh+1)/2,dw,dh);
 }
 rect(x,y,w,3,255,153);rect(x,y,3,h,255,153);
 rect(x,y+h-3,w,3,85,255);rect(x+w-3,y,3,h,85,255);
}

/* Ellipsize in place so a long item name (either pack font) stays inside its
 * column instead of running under the slots. `label` has room for "...". */
static void container_fit_label(char* label,int width) {
 if(gutil_font_width(label,14)<=width)return;
 size_t n=strlen(label);int dots=gutil_font_width("...",14);
 while(n&&gutil_font_width(label,14)>width-dots)label[--n]=0;
 memcpy(label+n,"...",4);
}

/* T13 page header (design-round-1026e render_cavex.py pagehdr, V1 with the
 * maintainer's 2026-10-27 change: no lead glyph). Light: active page yellow
 * 255,255,160 + shadow + 2 px underline, other page and bar grey 150. Ink:
 * active ink 64 on a pale plate + underline, other page and bar grey 128. */
static void rect_rgb(int x,int y,int w,int h,uint8_t r,uint8_t g,uint8_t b,uint8_t a) {
 gfx_texture(false);gutil_texquad_col(x,y,0,0,0,0,w,h,r,g,b,a);gfx_texture(true);
}
int container_page_header_width(bool light) {
 int gap=light?10:6;
 return gutil_font_width("Inventory",16)+gap+gutil_font_width("|",16)+gap
  +gutil_font_width("Crafting",16);
}
int container_page_header_y(bool full) {
 return full?24:20;
}
int container_page_header(int x,int y,bool crafting,bool light) {
 static const char* names[2]={"Inventory","Crafting"};
 int gap=light?10:6,at=x;
 uint8_t dim=light?150:128;
 for(int k=0;k<2;k++) {
  int w=gutil_font_width(names[k],16);bool active=(k==1)==crafting;
  if(active&&!light)rect(at-3,y-3,w+6,22,255,110);
  if(active&&light)gutil_text_rgb(at,y,names[k],16,true,255,255,160);
  else if(active)gutil_text_rgb(at,y,names[k],16,false,64,64,64);
  else gutil_text_rgb(at,y,names[k],16,light,dim,dim,dim);
  if(active)rect_rgb(at,y+18,w,2,light?255:64,light?255:64,light?160:64,255);
  at+=w;
  if(!k){at+=gap;gutil_text_rgb(at,y,"|",16,light,dim,dim,dim);at+=gutil_font_width("|",16)+gap;}
 }
 return at-x;
}

void container_inventory_look(const struct container_compact* v,float* pitch,float* yaw) {
 bool full=v->height>=480;
 float head_x=(v->width-640)/2+(full?228+40:74+29);
 float head_y=(v->height-(full?480:240))/2+(full?77+20:46+19);
 float target_x=head_x,target_y=head_y;
 if(v->menu.pointer_available&&v->menu.focus.pointer_owner){
  target_x=v->menu.pointer_x;target_y=v->menu.pointer_y;
 } else if(v->menu.selected<v->count){
  const struct container_slot* slot=&v->slots[v->menu.selected];
  float half=container_slot_extent(&v->menu,slot)*0.5F;
  target_x=slot->x+half;target_y=slot->y+half;
 }
 /* Original CavEX inventory response: half yaw and one-third pitch. */
 *pitch=atan2f(target_y-head_y,192.0F)*0.66F*0.5F;
 *yaw=atan2f(target_x-head_x,192.0F)*0.5F;
}

/* Full: Beta's 80x112 box. Split (T13 V1): 58x104 next to the 26 px armor
 * column, the model scaled by height so the whole figure stays inside. */
static void inventory_portrait(const struct container_compact* v,struct inventory* inv,int x,int y,int w,int h) {
 rect(x,y,w,h,0,255);
#ifdef GFX_3D_ELEMENTS
 float pitch,yaw;container_inventory_look(v,&pitch,&yaw);
 float scale=3.25F*(float)h/112.0F;
 mat4 view;glm_translate_make(view,(vec3){x+w/2,y+h*50/112,0});
 glm_scale(view,(vec3){scale,-scale,1});
 glm_rotate_x(view,pitch,view);glm_rotate_y(view,yaw,view);
 glm_translate(view,(vec3){0,10,0});
 struct item_data held,helmet,chest,legs,boots;
 gfx_write_buffers(true,true,true);
 render_model_player(view,glm_deg(pitch),glm_deg(yaw),0,0,
  inventory_get_hotbar_item(inv,&held)?&held:NULL,
  inventory_get_slot(inv,INVENTORY_SLOT_ARMOR,&helmet)?&helmet:NULL,
  inventory_get_slot(inv,INVENTORY_SLOT_ARMOR+1,&chest)?&chest:NULL,
  inventory_get_slot(inv,INVENTORY_SLOT_ARMOR+2,&legs)?&legs:NULL,
  inventory_get_slot(inv,INVENTORY_SLOT_ARMOR+3,&boots)?&boots:NULL);
 gfx_write_buffers(true,false,false);gfx_matrix_modelview(GLM_MAT4_IDENTITY);
#endif
}

void container_inventory_render(const struct container_compact* v,struct window_container* wc,
 int x,int y,const struct input_player_frame* prompts) {
 bool full=v->height>=480;
 int ox=x+(v->width-640)/2,oy=y+(v->height-(full?480:240))/2;
 struct inventory* inv=windowc_get_latest(wc);bool held=inventory_get_picked_item(inv,NULL);
 rect(x,y,v->width,v->height,0,180);
 /* ci-1101/07: the full panel starts at 46 (B3 had 30) so the header above
  * it is TV-safe (y 24..44); it ends at 361 so machine_panel's 2 px outer
  * border (rect y-2 .. y+h+2) stops at 363, the hearts row, without touching
  * it (Sol R2) -- its top 16 px held nothing (the armour column starts at 63). */
 machine_panel(ox+(full?134:32),oy+(full?46:8),full?372:576,full?315:188);
 /* T13 approved V1: the page header replaces the in-panel title. Full: centred
  * over the washed world above the panel; split: inside the panel at 44,20. */
 if(full)container_page_header(ox+320-container_page_header_width(true)/2,oy+container_page_header_y(true),false,true);
 else container_page_header(ox+44,oy+container_page_header_y(false),false,false);
 if(full)inventory_portrait(v,inv,ox+228,oy+63,80,112);
 else inventory_portrait(v,inv,ox+74,oy+46,58,104);
 gfx_bind_texture(&texture_gui_crafting);
 if(full)gutil_texquad(ox+386,oy+95,90,35,22,15,28,20);
 else gutil_texquad(ox+194,oy+80,90,35,22,15,22,16);
 /* T12: no "Recipes" button; PAGE_SWAP opens the Crafting page. */
 for(size_t k=0;k<v->count;k++){
  int sx=x+v->slots[k].x,sy=y+v->slots[k].y,extent=container_slot_extent(&v->menu,&v->slots[k]);
  gfx_bind_texture(&texture_gui_crafting);gutil_texquad(sx,sy,29,16,18,18,extent,extent);
  struct item_data item;
  if((k!=v->menu.selected||!held||(v->menu.pointer_available&&v->menu.pointer_on_slot))&&inventory_get_slot(inv,v->slots[k].slot,&item))
   gutil_draw_item_sized(&item,sx+2,sy+2,extent-4,1);
 }
 const struct container_slot* sel=&v->slots[v->menu.selected];int extent=container_slot_extent(&v->menu,sel);
 struct gutil_button_bounds bounds={x+sel->x,y+sel->y,extent,extent};
 if(!v->menu.sort_focus)gutil_menu_focus_border(&bounds);
 struct item_data item;
 if(inventory_get_picked_item(inv,&item)){
  if(v->menu.pointer_available&&v->menu.pointer_on_slot)gutil_draw_item(&item,x+v->menu.pointer_x-16,y+v->menu.pointer_y-16,0);
  else gutil_draw_item_sized(&item,bounds.x+2,bounds.y+2,extent-4,0);
 }
 char label[128];container_menu_label(&v->menu,wc,sel->slot,label,sizeof label);
 char* wear=full?NULL:strstr(label," - Wear ");
 int lw=full?324:204;
 if(wear){*wear=0;wear+=3;container_fit_label(wear,lw);gutil_text_rgb(ox+44+(204-gutil_font_width(wear,14))/2,oy+174,wear,14,false,64,64,64);}
 container_fit_label(label,lw);
 gutil_text_rgb(ox+(full?158:44)+(lw-gutil_font_width(label,14))/2,oy+(full?179:156),label,14,false,64,64,64);
 container_row_draw(CONTAINER_ROW_INVENTORY,held,false,prompts,x,y,v->width,v->height);
 container_menu_sort_render(&v->menu,wc,x,y,v->width,v->height,prompts);
}
static void machine_render(const struct container_compact* v,struct window_container* wc,
 int x,int y,const struct input_player_frame* prompts) {
 bool full=v->height>=480;int ox=x+(v->width-640)/2,oy=y+(v->height-(full?480:240))/2;
 int px=ox+(full?134:32),py=oy+(full?30:8),ww=full?324:204; /* B3: 44 -> 30 */
 machine_panel(px,py,full?372:576,full?331:188);
 gutil_text_rgb(px+12,py+8,v->type==WINDOW_TYPE_FURNACE?"Furnace":"Dispenser",16,false,64,64,64);
 struct inventory* inv=windowc_get_latest(wc);bool held=inventory_get_picked_item(inv,NULL);
 for(size_t k=0;k<v->count;k++) {
  int sx=x+v->slots[k].x,sy=y+v->slots[k].y;
  gfx_bind_texture(&texture_gui_crafting);gutil_texquad(sx,sy,29,16,18,18,36,36);
  struct item_data item;
  if((k!=v->menu.selected||!held||(v->menu.pointer_available&&v->menu.pointer_on_slot))&&inventory_get_slot(inv,v->slots[k].slot,&item))
   gutil_draw_item(&item,sx+2,sy+2,1);
 }
 if(v->type==WINDOW_TYPE_FURNACE) {
  int fx=x+v->slots[0].x+8,fy=y+v->slots[0].y+38;
  int ax=x+v->slots[0].x+50,ay=y+v->slots[0].y+33;
  gfx_bind_texture(&texture_gui_furnace);
  gutil_texquad(fx,fy,56,36,14,14,20,20);
  gutil_texquad(ax,ay,79,34,22,15,44,30);
  if(wc->furnace_burn_time>0) {
   int burn=wc->furnace_current_item_burn?wc->furnace_current_item_burn:200;
   int flame=wc->furnace_burn_time*12/burn;if(flame>12)flame=12;
   int dh=(flame+2)*20/14;
   gutil_texquad(fx,fy+20-dh,176,12-flame,14,flame+2,20,dh);
  }
  int progress=wc->furnace_cook_time*24/200;if(progress>24)progress=24;
  gutil_texquad(ax,ay,176,14,progress+1,16,(progress+1)*44/25,30);
 }
 const struct container_slot* sel=&v->slots[v->menu.selected];
 struct gutil_button_bounds bounds={x+sel->x,y+sel->y,36,36};
 gutil_menu_focus_border(&bounds);
 struct item_data item;
 if(inventory_get_picked_item(inv,&item)) {
  int ix=bounds.x+2,iy=bounds.y+2;
  if(v->menu.pointer_available&&v->menu.pointer_on_slot){ix=x+v->menu.pointer_x-16;iy=y+v->menu.pointer_y-16;}
  gutil_draw_item(&item,ix,iy,0);
 }
 char label[128];container_menu_label(&v->menu,wc,sel->slot,label,sizeof label);
 if(gutil_font_width(label,14)>ww){size_t n=strlen(label);int dots=gutil_font_width("...",14);
  while(n&&gutil_font_width(label,14)>ww-dots)label[--n]='\0';
  if(n+3<sizeof label)memcpy(label+n,"...",4);
 }
 int lx=ox+(full?158:44)+(ww-gutil_font_width(label,14))/2;
 gutil_text_rgb(lx,oy+(full?177:161),label,14,false,64,64,64);
 container_row_draw(CONTAINER_ROW_MACHINE,held,false,prompts,x,y,v->width,v->height);
}

void container_compact_render(const struct container_compact* v,
	struct window_container* wc, int x, int y,
	const struct container_glyphs* glyphs) {
	if(!v->active || !wc || wc->type != v->type
	   || wc->slot_count != v->main_base + 36)
		return;
	struct inventory* inv = windowc_get_latest(wc);
	int bx = x + (v->width - 640) / 2;
	int by = y + (v->height - 240) / 2;
	/* Match the full native container: dim the live world around an opaque
	 * inventory panel. Never replace the viewport with a solid background. */
	struct input_player_frame prompts;
	glyph_frame(glyphs, &prompts);
	if(v->type==WINDOW_TYPE_INVENTORY){container_inventory_render(v,wc,x,y,&prompts);return;}
	rect(x,y,v->width,v->height,0,180);
	if(v->type==WINDOW_TYPE_FURNACE||v->type==WINDOW_TYPE_DISPENSER){machine_render(v,wc,x,y,&prompts);return;}
	if(v->menu.sort_open) {
		container_menu_sort_render(&v->menu, wc, x, y, v->width, v->height, &prompts);
		return;
	}
	if(v->type == WINDOW_TYPE_CHEST) {
		container_chest_panel(bx + 32, by + 8, 576, 188);
		container_chest_header_paged(bx + 32, by + 8, 576, chest_title(v), v->view_count > 1, &prompts);
		bool held = inventory_get_picked_item(inv, NULL);
		for(size_t k = 0; k < v->count; k++) {
			int sx = x + v->slots[k].x, sy = y + v->slots[k].y;
			container_chest_slot(sx, sy);
			struct item_data item;
			if((k != v->menu.selected || !held)
			   && inventory_get_slot(inv, v->slots[k].slot, &item))
				gutil_draw_item(&item, sx, sy, 1);
		}
		const struct container_slot* selected = &v->slots[v->menu.selected];
		struct gutil_button_bounds bounds = {x + selected->x, y + selected->y, 32, 32};
		if(!v->menu.sort_focus) gutil_menu_focus_border(&bounds);
		struct item_data item;
		if(inventory_get_picked_item(inv, &item)) gutil_draw_item(&item, bounds.x, bounds.y, 0);
		char label[128];
		container_menu_label(&v->menu, wc, selected->slot, label, sizeof(label));
		container_chest_info(bx + 380, by + 40, 216, 148, label);
		container_menu_sort_render(&v->menu, wc, x, y, v->width, v->height, &prompts);
		container_row_draw(CONTAINER_ROW_STORAGE, held, v->view_count > 1, &prompts,
			x, y, v->width, v->height);
		return;
	}
	rect(bx + 152, by + 40, 336, 152, 192, 255);
	const char* auxiliary = v->type == WINDOW_TYPE_INVENTORY ? "Craft / Armor"
		: v->type == WINDOW_TYPE_WORKBENCH ? "Crafting"
		: v->type == WINDOW_TYPE_FURNACE ? "Furnace"
		: v->type == WINDOW_TYPE_DISPENSER ? "Dispenser"
		: v->view_count == 3 ? "Chest 1" : "Chest";
	const char* names[] = {"Inventory", auxiliary, "Chest 2"};
	int widths[3], total = 0;
	for(unsigned k = 0; k < v->view_count; k++) {
		widths[k] = gutil_font_width(names[k], 16) + 24;
		total += widths[k];
	}
	/* Approved compact header: Prev / tab row / Next all live LEFT of the
	 * reserved MENU_HEADER_NOTICE_W slot, so a controller notice composing
	 * into this header (menu_header.h) can never cover one of them and their
	 * positions never shift when one arrives or expires. The tab row is
	 * centred inside its own fixed box rather than across the whole 640 px,
	 * which is what used to push a wide label under the Next prompt. */
	struct menu_rect tabs = menu_container_tab_box(bx, by);
	struct menu_rect prev_box = menu_container_prev_box(bx, by);
	struct menu_rect next_box = menu_container_next_box(bx, by);
	int tx = tabs.x + (tabs.w - total) / 2;
	if(tx < tabs.x)
		tx = tabs.x;
	for(unsigned k = 0; k < v->view_count; k++) {
		if(tx + widths[k] > tabs.x + tabs.w)
			break; /* clipped to the box; never drawn over a prompt */
		if(k == v->view) rect(tx, tabs.y, widths[k], tabs.h, 80, 255);
		gutil_text(tx + 12, tabs.y + 3, names[k], 16, true);
		tx += widths[k];
	}
	/* Maintainer 2026-10-31 (Q2): plain arrows on the same pointer targets;
	 * the Section L+R prompt is in the bottom row. */
	struct gutil_button_bounds prev_arrow = {prev_box.x, prev_box.y, 32, prev_box.h};
	struct gutil_button_bounds next_arrow = {next_box.x, next_box.y, 32, next_box.h};
	container_menu_arrow(&prev_arrow, "<");
	container_menu_arrow(&next_arrow, ">");
	bool held = inventory_get_picked_item(inv, NULL);
	for(size_t k = 0; k < v->count; k++) {
		int sx = x + v->slots[k].x, sy = y + v->slots[k].y;
		rect(sx - 2, sy - 2, 36, 36, 64, 255);
		rect(sx, sy, 34, 34, 240, 255);
		rect(sx, sy, 32, 32, 139, 255);
		struct item_data item;
		if((k != v->menu.selected || !held)
		   && inventory_get_slot(inv, v->slots[k].slot, &item))
			gutil_draw_item(&item, sx, sy, 1);
	}
	if(v->view && v->type == WINDOW_TYPE_FURNACE) {
		gfx_bind_texture(&texture_gui_furnace);
		if(wc->furnace_burn_time > 0) {
			int burn = wc->furnace_current_item_burn ? wc->furnace_current_item_burn : 200;
			int flame = wc->furnace_burn_time * 12 / burn;
			if(flame > 12) flame = 12;
			gutil_texquad(bx + 232, by + 80 + (12 - flame) * 2,
				176, 12 - flame, 14, flame + 2, 28, (flame + 2) * 2);
		}
		int arrow = wc->furnace_cook_time * 24 / 200;
		if(arrow > 24) arrow = 24;
		gutil_texquad(bx + 324, by + 80, 176, 14, arrow + 1, 16, (arrow + 1) * 2, 32);
	} else if(v->view && !v->storage_count) {
		gutil_text(bx + 352, by + 88, "\2478>", 16, false);
	}
	const struct container_slot* selected = &v->slots[v->menu.selected];
	struct gutil_button_bounds bounds = {x + selected->x, y + selected->y, 32, 32};
	if(!v->menu.sort_focus) gutil_menu_focus_border(&bounds);
	struct item_data item;
	if(inventory_get_picked_item(inv, &item))
		gutil_draw_item(&item, bounds.x, bounds.y, 0);
	char label[128];
	container_menu_label(&v->menu, wc, selected->slot, label, sizeof(label));
	sidebar(bx + 24, by + 60, label);
	if(!held) {
		const char* destination = v->storage_count
			? (selected->slot < v->main_base ? "To inventory" : "To chest")
			: selected->slot < v->main_base || selected->slot >= v->main_base + 27
				? "To inventory" : "To hotbar";
		if(!container_menu_sort_supported(wc)) sidebar(bx + 512, by + 60, destination);
	} else {
		if(!container_menu_sort_supported(wc)) sidebar(bx + 512, by + 60, "Switch tabs to place");
	}
	container_menu_sort_render(&v->menu, wc, x, y, v->width, v->height, &prompts);
	container_row_draw(compact_row_kind(v), held, v->view_count > 1, &prompts,
		x, y, v->width, v->height);
}

bool container_compact_screen_open(struct container_compact* v,
	uint8_t window, int width, int height) {
	struct window_container* wc = gstate.players[0].windows[window];
	v->active = false;
	if(!wc)
		return false;
	return container_compact_open(v, wc->type, wc->slot_count,
		inventory_get_hotbar(windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY])),
		width, height);
}

void container_compact_screen_update(struct container_compact* v,
	uint8_t window, float dt) {
	v->menu.sort_owner = &gstate.players[0];
	struct container_input input;
	container_menu_read_input(&input, 0, 0, v->width, v->height);
	if(container_compact_step(v, gstate.players[0].windows[window], window, &input, dt,
		container_menu_send, NULL))
		screen_set(&screen_ingame);
}

/* See the header: the split page header's right edge - 32, so the notice
 * reservation (this + 24 + 8) starts 8 px after the header. One header for
 * both pages, so the page no longer changes it. */
int container_compact_inventory_next_x(bool crafting_view) {
	(void)crafting_view;
	return 44 + container_page_header_width(false) - 24;
}

void container_compact_screen_render(struct container_compact* v, uint8_t window) {
	struct container_glyphs glyphs;
	container_compact_active_glyphs(&glyphs);
	container_compact_render(v, gstate.players[0].windows[window], 0, 0, &glyphs);
	/* B3 (10-31): a full-screen container keeps the hearts + hotbar exactly
	 * where gameplay draws them (the panels end above the hearts, the row
	 * sits under the hotbar). The 54-slot chest's panel reaches the bottom,
	 * so it shows no HUD (Q7). */
	if(v->active && v->height >= 480
	   && !(v->type == WINDOW_TYPE_CHEST && v->storage_count > 27))
		hud_menu_draw_owned(&gstate.players[0], v->width, v->height);
}

static void manual_grid_slots(struct manual_grid* v) {
 bool full=v->height>=480;int ox=(v->width-640)/2,oy=(v->height-(full?480:240))/2;
 int gy=full?142:62,pad=v->grid==2?18:0;v->count=0;
 for(unsigned k=0;k<v->grid*v->grid;k++)v->slots[v->count++]=(struct container_slot){ox+44+pad+(int)(k%v->grid)*36,oy+gy+pad+(int)(k/v->grid)*36,k+1,36};
 v->slots[v->count++]=(struct container_slot){ox+208,oy+(full?170:90),0,52};
 size_t base=v->grid==2?INVENTORY_SLOT_MAIN:CRAFTING_SLOT_MAIN;
 unsigned main=full?27:v->page?9:18,first=full?0:v->page*18;
 for(unsigned k=0;k<main;k++)v->slots[v->count++]=(struct container_slot){ox+272+(int)(k%9)*36,oy+(full?134:72)+(int)(k/9)*36,base+first+k,36};
 for(unsigned k=0;k<9;k++)v->slots[v->count++]=(struct container_slot){ox+272+(int)k*36,oy+(full?246:148),base+27+k,36};
 if(v->menu.selected>=v->count)v->menu.selected=0;
}
void manual_grid_open(struct manual_grid* v,unsigned grid,int width,int height) {
 *v=(struct manual_grid){.grid=grid==2?2:3,.width=width,.height=height,.active=true,
  .menu={.slot_extent=36,.return_only=true,.sort_disabled=true,.focus={.initialized=true}}};
 manual_grid_slots(v);
}
static void manual_page_bounds(const struct manual_grid* v,struct gutil_button_bounds out[2]) {
 int right=(v->width-640)/2+596,y=(v->height-240)/2+50;
 int count=gutil_font_width("1/2",16);
 out[0]=(struct gutil_button_bounds){right-56-count,y,20,20};
 out[1]=(struct gutil_button_bounds){right-20,y,20,20};
}
/* The Manual Grid row (Astra ruling 6 Q1, 2026-09-19), now drawn by the
 * shared bottom hint row: EMPTY cursor Pick | Split | Quick Move | Recipes;
 * HOLDING Place | One | Drop | Recipes -- the third cell SWAPS, its Quick
 * Move BINDING stays live (manual_grid_step never filters it). Drop's glyph
 * is the MENU drop role, so a remap moves it and an unbound Drop is left
 * out. */

/* Measurement seam for the tests: the DRAWN row's cells (viewport-local),
 * from the same layout the painter and the pointer use. */
size_t manual_grid_footer_probe(const struct manual_grid* v,
 const struct container_glyphs* g,bool held,
 struct gutil_button_bounds bounds[4],int symbols[4],const char* labels[4]);
size_t manual_grid_footer_probe(const struct manual_grid* v,
 const struct container_glyphs* g,bool held,
 struct gutil_button_bounds bounds[4],int symbols[4],const char* labels[4]) {
 struct input_player_frame f;glyph_frame(g,&f);
 struct gutil_hint_row row;enum container_row_action actions[CONTAINER_ROW_ITEMS];
 int n=container_row_layout(CONTAINER_ROW_MANUAL,held,false,&f,0,0,v->width,v->height,&row,actions);
 if(n>4)n=4;
 for(int k=0;k<n;k++){bounds[k]=row.cells[k].rect;symbols[k]=row.cells[k].glyph[0];labels[k]=row.cells[k].label;}
 return (size_t)n;
}
bool manual_grid_step(struct manual_grid* v,struct window_container* wc,uint8_t window,
 const struct container_input* input,const struct container_glyphs* glyphs,float dt,container_send_fn send,void* user) {
 if(!v->active||!wc)return false;
 struct container_input in=*input;
 /* The hit targets follow the VISIBLE row: while the cursor holds a stack the
  * third cell reads Drop, so clicking it drops and can never quick-move. */
 bool held=inventory_get_picked_item(windowc_get_latest(wc),NULL);
 struct input_player_frame frame;glyph_frame(glyphs,&frame);
 bool owns=gutil_menu_pointer_focus(&v->menu.focus,in.pointer_valid,in.pointer_x,in.pointer_y,in.up||in.down||in.left||in.right);
 if(!in.suspended&&!v->menu.suspended) {
  /* A row click sets EXACTLY the action of the cell it hit (Drop on the
   * holding row, Quick Move on the empty one, Recipes = close) and clears
   * the others; with no cell hit the button bindings are untouched, which is
   * why the Quick Move binding keeps working after its prompt leaves. */
  if(owns)container_row_click(CONTAINER_ROW_MANUAL,held,false,&frame,0,0,v->width,v->height,&in);
  if(in.close||v->menu.close_requested){
   if(container_menu_step(&v->menu,v->slots,v->count,0,0,wc,window,&in,dt,send,user)){v->active=false;return true;}
   return false;
  }
  if(v->height<480) {
   int page=-1;struct gutil_button_bounds arrows[2];manual_page_bounds(v,arrows);
   if(in.previous&&!in.page&&v->page>0)page=(int)v->page-1;
   if(in.page&&!in.previous&&v->page<1)page=(int)v->page+1;
   if(in.pick&&owns)for(int k=0;k<2;k++)if((unsigned)k!=v->page&&gutil_button_hit(&arrows[k],in.pointer_x,in.pointer_y))page=k;
   if(page>=0){v->selection[v->page]=v->menu.selected;v->page=(unsigned)page;v->menu.selected=v->selection[v->page];v->menu.selection_origin=GUTIL_MENU_ORIGIN_DIRECTIONAL;manual_grid_slots(v);v->menu.focus=(struct gutil_menu_focus){.initialized=true};audio_menu_event(MENU_SOUND_SECTION,time_get());return false;}
  }
 }
 in.close=false;in.previous=in.page=false;
 container_menu_step(&v->menu,v->slots,v->count,0,0,wc,window,&in,dt,send,user);
 return false;
}
void manual_grid_render(const struct manual_grid* v,struct window_container* wc,int x,int y,
 const struct container_glyphs* glyphs) {
 if(!v->active||!wc)return;
 bool full=v->height>=480;int ox=x+(v->width-640)/2,oy=y+(v->height-(full?480:240))/2;
 rect(x,y,v->width,v->height,0,full?150:180);recipe_ui_panel(ox+32,oy+(full?24:16),576,full?320:176);
 char title[32];snprintf(title,sizeof title,full?"Manual Grid":"Manual Grid - P%u",v->owner_index+1);
 gutil_text_rgb(ox+44,oy+(full?36:28),title,16,false,64,64,64);
 gutil_text_rgb(ox+272,oy+(full?110:50),"Inventory",16,false,64,64,64);
 if(!full){struct gutil_button_bounds a[2];manual_page_bounds(v,a);char page[8];snprintf(page,sizeof page,"%u/2",v->page+1);
  recipe_ui_chevron(x+a[0].x+6,y+a[0].y+3,-1,0,v->page>0);recipe_ui_chevron(x+a[1].x+6,y+a[1].y+3,1,0,v->page<1);
  gutil_text_rgb(x+a[0].x+28,y+a[0].y,page,16,false,64,64,64);
  if(v->page)for(int k=0;k<9;k++){gfx_bind_texture(&texture_gui_crafting);gutil_texquad(ox+272+k*36,oy+108,29,16,18,18,36,36);}
 }
 struct inventory* inv=windowc_get_latest(wc);
 if(v->menu.close_requested||v->menu.return_blocked||v->menu.send_failed){char status[128];container_menu_label(&v->menu,wc,v->slots[v->menu.selected].slot,status,sizeof status);gutil_text_rgb(ox+(full?210:272),oy+(full?36:28),status,16,false,64,64,64);}
 for(size_t k=0;k<v->count;k++) {
  const struct container_slot* cell=&v->slots[k];int sz=cell->extent,sx=x+cell->x,sy=y+cell->y;bool output=cell->slot==0;
  gfx_bind_texture(&texture_gui_crafting);gutil_texquad(sx,sy,output?119:29,output?30:16,output?26:18,output?26:18,sz,sz);
  struct item_data item;if((k!=v->menu.selected||!inventory_get_picked_item(inv,NULL)||(v->menu.pointer_available&&v->menu.pointer_on_slot))&&inventory_get_slot(inv,cell->slot,&item))gutil_draw_item(&item,sx+(sz-32)/2,sy+(sz-32)/2,1);
 }
 gfx_bind_texture(&texture_gui_crafting);gutil_texquad(ox+158,oy+(full?181:101),90,35,22,15,44,30);
 if(v->menu.selected<v->count){const struct container_slot* cell=&v->slots[v->menu.selected];struct gutil_button_bounds b={x+cell->x,y+cell->y,cell->extent,cell->extent};gutil_menu_focus_border(&b);
  struct item_data held;if(inventory_get_picked_item(inv,&held))gutil_draw_item(&held,v->menu.pointer_on_slot?x+v->menu.pointer_x-16:b.x+(cell->extent-32)/2,v->menu.pointer_on_slot?y+v->menu.pointer_y-16:b.y+(cell->extent-32)/2,0);
 }
 struct input_player_frame frame;glyph_frame(glyphs,&frame);
 container_row_draw(CONTAINER_ROW_MANUAL,inventory_get_picked_item(inv,NULL),false,&frame,x,y,v->width,v->height);
 /* B3 (10-31): hearts + hotbar at the gameplay positions */
 if(full)hud_menu_draw_owned(&gstate.players[0],v->width,v->height);
 if(v->menu.focus.pointer_owner&&v->menu.pointer_available){gfx_bind_texture(&texture_pointer);gutil_texquad_rt_any(x+v->menu.pointer_x,y+v->menu.pointer_y,0,0,0,256,256,48,48);}
}
