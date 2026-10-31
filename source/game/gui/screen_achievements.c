/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../block/blocks.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/render_block.h"
#include "../../graphics/render_item.h"
#include "../../graphics/texture_atlas.h"
#include "../../item/items.h"
#include "../../platform/gfx.h"
#include "../../platform/input_glyph.h"
#include "../../platform/menu_sound.h"
#include "../../platform/time.h"
#include "../achievements/achievements_api.h"
#include "screen_achievements.h"

static struct achievements_board boards[CW_ACH_SLOT_COUNT];
static bool gray_refresh = true;
static const int branches[4][6]
	= {{0, 1, 2}, {3, 4, 5, 9, 10, 11}, {6, 7, 8}, {12, 13, 14, 15}};
static const int branch_count[4] = {3, 6, 3, 4};
static const char* branch_names[] = {"Basics", "Mining", "Farming", "Combat"};
static const char* names[]
	= {"Taking Inventory",	 "Getting Wood",   "Benchmarking",
	   "Time to Mine!",		 "Hot Topic",	   "Acquire Hardware",
	   "Time to Farm!",		 "Bake Bread",	   "The Lie",
	   "Getting an Upgrade", "Delicious Fish", "On A Rail",
	   "Time to Strike!",	 "Monster Hunter", "Cow Tipper",
	   "When Pigs Fly"};
/* Approved controller-readable copy. The trigger/parent/icon comes ONLY
 * from achievements_get; these strings do not implement unlock logic. */
static const char* instructions[]
	= {"Open your inventory.",
	   "Pick up a log.",
	   "Craft a workbench.",
	   "Craft a wooden pickaxe.",
	   "Craft a furnace.",
	   "Smelt an iron ingot.",
	   "Craft a wooden hoe.",
	   "Bake bread.",
	   "Craft a cake.",
	   "Craft a better pickaxe.",
	   "Cook a fish.",
	   "Ride a minecart at least 1 km from where you started.",
	   "Craft a wooden sword.",
	   "Kill a monster.",
	   "Pick up leather.",
	   "Ride a pig off a cliff."};

struct achievements_board* screen_achievements_owner(unsigned owner) {
	return owner < CW_ACH_SLOT_COUNT ? &boards[owner] : NULL;
}
int screen_achievements_tab_count(int tab) {
	return tab >= 0 && tab < 4 ? branch_count[tab] : 0;
}
int screen_achievements_tab_id(int tab, int row) {
	return row >= 0 && row < screen_achievements_tab_count(tab) ?
		branches[tab][row] :
		-1;
}
int screen_achievements_tab_of(int id) {
	for(int t = 0; t < 4; t++)
		for(int i = 0; i < branch_count[t]; i++)
			if(branches[t][i] == id)
				return t;
	return -1;
}
const char* screen_achievements_tab_name(int tab) {
	return tab >= 0 && tab < 4 ? branch_names[tab] : "";
}
const char* screen_achievements_name(int id) {
	return id >= 0 && id < 16 ? names[id] : "";
}
int screen_achievements_ribbon_first(int id) {
	return id < 3 ? 0 : id > 12 ? 9 : id - 3;
}

void screen_achievements_read(unsigned owner, struct achievements_data* d) {
	if(!d)
		return;
	*d = (struct achievements_data) {.state = ACHIEVEMENTS_UNAVAILABLE,
									 .earned = -1};
	struct ach_status st;
	bool status
		= owner < CW_ACH_SLOT_COUNT && achievements_status((uint8_t)owner, &st);
	enum ach_init_status init = achievements_init_status();
	const char* error = "Achievements unavailable for this player.";
	if(init != ACH_INIT_OK && init != ACH_INIT_NEVER_RUN) {
		d->state = ACHIEVEMENTS_ERROR;
		error = "Achievements could not load. Back or Retry.";
	} else if(status && st.closing) {
		d->state = ACHIEVEMENTS_LOADING;
		error = "Loading achievements...";
	} else if(status && st.bound) {
		if(st.load == STATS_LOAD_CORRUPT
		   || st.load == STATS_LOAD_UNSUPPORTED_VERSION
		   || st.load == STATS_LOAD_BAD_ARG) {
			d->state = ACHIEVEMENTS_ERROR;
			error = st.load == STATS_LOAD_UNSUPPORTED_VERSION ?
				"Progress needs a newer version. Back or Retry." :
				"Progress could not be read. Back or Retry.";
		} else {
			d->state = ACHIEVEMENTS_READY;
			d->earned = 0;
			for(int id = 0; id < 16; id++) {
				struct ach_desc row;
				if(!achievements_get((uint8_t)owner, id, &row)
				   || row.parent < -1 || row.parent >= 16) {
					d->state = ACHIEVEMENTS_ERROR;
					d->earned = -1;
					error = "Progress query failed. Back or Retry.";
					break;
				}
				d->rows[id] = (struct achievements_row) {
					row.icon_item, row.parent, row.special, row.unlocked,
					row.can_unlock};
				d->earned += row.unlocked;
			}
			if(d->state == ACHIEVEMENTS_READY) {
				error = st.progress_incomplete ?
					"Some progress was lost." :
					st.last_save_error != STATS_SAVE_OK ?
					"Progress could not be saved." :
					"";
				/* Preserve the known count, but surface module warnings instead
				 * of silently rendering an apparently healthy progress board.
				 */
				if(*error)
					d->state = ACHIEVEMENTS_ERROR;
			}
		}
	}
	snprintf(d->message, sizeof d->message, "%s", error);
}
enum achievement_visual_state
screen_achievements_state(const struct achievements_data* d, int id) {
	if(!d || d->state != ACHIEVEMENTS_READY || id < 0 || id >= 16)
		return ACHIEVEMENT_UNKNOWN;
	return d->rows[id].unlocked ? ACHIEVEMENT_EARNED :
		d->rows[id].can_unlock	? ACHIEVEMENT_AVAILABLE :
								  ACHIEVEMENT_LOCKED;
}

void screen_achievements_open(struct achievements_board* b, unsigned owner) {
	if(!b || owner >= CW_ACH_SLOT_COUNT)
		return;
	struct ach_status st;
	if(achievements_status((uint8_t)owner, &st) && st.bound) {
		if(b->profile[0] && strcmp(b->profile, st.profile_id))
			b->visited = false;
		snprintf(b->profile, sizeof b->profile, "%s", st.profile_id);
	}
	if(!b->visited || b->owner != owner || b->selected < 0 || b->selected >= 16)
		b->selected = 0;
	gray_refresh = true;
	b->owner = (uint8_t)owner;
	b->visited = b->open = true;
	b->depth = 0;
	b->detail = false;
	b->pointer_available = false;
	b->pointer_target = -1;
	b->focus = (struct gutil_menu_focus) {.initialized = true};
	b->origin = GUTIL_MENU_ORIGIN_DEFAULT;
	audio_menu_event(MENU_SOUND_SECTION, time_get());
}

void screen_achievements_layout(struct achievements_layout* l, int x, int y,
								int w, int h) {
	if(!l)
		return;
	*l = (struct achievements_layout) {.split = h < 480, .view = {x, y, w, h}};
	int pw = w >= 854 ? 720 : 576, px = x + (w - pw) / 2;
	int lw = w >= 854 ? 340 : 300;
	l->panel = (struct menu_rect) {px, y + (l->split ? 12 : 24), pw,
								   l->split ? 216 : 432};
	l->header = (struct menu_rect) {px + 16, y + (l->split ? 12 : 40), pw - 32,
									l->split ? 24 : 28};
	l->legend
		= (struct menu_rect) {px + 16, y + (l->split ? 180 : 404), pw - 32, 44};
	/* Footer positions reserve an independently hittable Requirement and Back.
	 */
	l->requirement = (struct menu_rect) {
		x + (w >= 854 ? (l->split ? 351 : 361) : 254), l->legend.y, 184, 44};
	l->back = (struct menu_rect) {
		x + (w >= 854 ? (l->split ? 682 : 691) : (l->split ? 503 : 512)),
		l->legend.y, 80, 44};
	if(l->split) {
		int size = w >= 854 ? 48 : 44, pitch = w >= 854 ? 60 : 54;
		int start = x + (w >= 854 ? 223 : 136);
		for(int i = 0; i < 7; i++)
			l->tiles[i]
				= (struct menu_rect) {start + i * pitch, y + 44, size, size};
		l->arrows[0]
			= (struct menu_rect) {start - 52, y + 44 + (size - 44) / 2, 44, 44};
		l->arrows[1] = (struct menu_rect) {start + 6 * pitch + size + 8,
										   y + 44 + (size - 44) / 2, 44, 44};
		l->title = (struct menu_rect) {px + 16, y + 96, pw - 32, 24};
		l->howto = (struct menu_rect) {px + 16, y + 120, pw - 32, 40};
		l->
			requires
		= (struct menu_rect) {px + 16, y + 160, pw - 32, 20};
	} else {
		l->chips[0] = (struct menu_rect) {px + 16, y + 78, 32, 32};
		l->chips[1] = (struct menu_rect) {px + pw - 48, y + 78, 32, 32};
		int tw = (pw - 112 - 24) / 4;
		for(int i = 0; i < 4; i++)
			l->tabs[i]
				= (struct menu_rect) {px + 56 + i * (tw + 8), y + 76, tw, 36};
		l->list_header = (struct menu_rect) {px + 16, y + 120, lw, 16};
		for(int i = 0; i < 6; i++)
			l->rows[i] = (struct menu_rect) {px + 16, y + 144 + i * 44, lw, 40};
		l->detail
			= (struct menu_rect) {px + lw + 32, y + 144, pw - lw - 48, 260};
		l->title = (struct menu_rect) {l->detail.x, y + 144, l->detail.w, 48};
		l->howto = (struct menu_rect) {l->detail.x, y + 204, l->detail.w, 60};
		l->
			requires
		= (struct menu_rect) {l->detail.x, y + 236, l->detail.w, 60};
	}
}

struct achievements_glyphs
screen_achievements_glyphs(const struct input_player_frame* f) {
	struct achievements_glyphs g = {-1, -1, -1, -1, -1};
	if(f)
		g = (struct achievements_glyphs) {
			f->glyphs[IB_GUI_UP], f->glyphs[IB_GUI_CLICK],
			f->glyphs[IB_INVENTORY], f->glyphs[IB_GUI_PREVIOUS],
			f->glyphs[IB_GUI_PAGE]};
	return g;
}
/* The centred hint row in the board's view (controller UI redesign 10-31,
 * A1 helper): Browse / Requirement (Retry) / Back. Its Requirement and Back
 * cells are also the pointer targets, so what is hit is what is drawn. */
enum { ACH_ROW_BROWSE, ACH_ROW_MID, ACH_ROW_BACK };
static bool achievements_row(const struct input_player_frame* f,
							 const struct achievements_layout* l, bool ready,
							 struct gutil_hint_row* row) {
	const struct gutil_hint_item items[] = {
		{{IB_GUI_UP}, 1, "Browse", 0, false},
		{{IB_GUI_CLICK}, 1, ready ? "Requirement" : "Retry", 1, false},
		{{IB_INVENTORY}, 1, "Back", 2, true}};
	if(!f) {
		memset(row, 0, sizeof *row);
		return false;
	}
	return gutil_hint_row_layout(f, l->view.x, l->view.y, l->view.w,
								 l->view.h, l->split, GUTIL_HINT_ALIGN_CENTRE,
								 ready ? items : items + 1, ready ? 3 : 2, row);
}
static struct menu_rect achievements_row_cell(const struct gutil_hint_row* row,
											  int item) {
	for(int k = 0; k < row->count; k++)
		if(row->cells[k].item == item) {
			struct gutil_button_bounds r = row->cells[k].rect;
			return (struct menu_rect) {r.x, r.y, r.width, r.height};
		}
	return (struct menu_rect) {0, 0, 0, 0};
}
static bool hit(struct menu_rect r, float x, float y) {
	struct gutil_button_bounds bounds = {r.x, r.y, r.w, r.h};
	return r.w > 0 && gutil_button_hit(&bounds, x, y);
}
static void select_id(struct achievements_board* b, int id,
					  enum menu_sound_event cue) {
	if(id < 0 || id >= 16 || b->selected == id)
		return;
	b->selected = id;
	b->detail = false;
	audio_menu_event(cue, time_get());
}
static void requirement(struct achievements_board* b,
						const struct achievements_data* d) {
	if(d->state != ACHIEVEMENTS_READY || b->depth >= 16)
		return;
	int parent = d->rows[b->selected].parent;
	if(parent < 0 || parent >= 16)
		return;
	b->stack[b->depth] = b->selected;
	b->stack_detail[b->depth++] = b->detail;
	select_id(b, parent, MENU_SOUND_SECTION);
}
enum {
	TARGET_REQUIREMENT = 16,
	TARGET_BACK,
	TARGET_PREV,
	TARGET_NEXT,
	TARGET_TAB0,
	TARGET_RETRY = 24
};

enum achievements_action
screen_achievements_update_owned(struct achievements_board* b,
								 const struct input_player_frame* f, int x,
								 int y, int w, int h) {
	if(!b || !b->open || !f || !f->connected || b->selected < 0
	   || b->selected >= 16)
		return ACHIEVEMENTS_NONE;
	/* Resume outranks the child stack and never awards or changes pause state.
	 */
	if(f->pressed[IB_HOME]) {
		b->open = false;
		return ACHIEVEMENTS_RESUME;
	}
	if(f->pressed[IB_INVENTORY] || f->pressed[IB_GUI_CLICK_ALT]) {
		audio_menu_event(MENU_SOUND_BACK, time_get());
		if(b->depth) {
			b->selected = b->stack[--b->depth];
			b->detail = b->stack_detail[b->depth];
			return ACHIEVEMENTS_NONE;
		}
		b->open = false;
		return ACHIEVEMENTS_BACK;
	}
	struct achievements_layout l;
	screen_achievements_layout(&l, x, y, w, h);
	struct achievements_data d;
	screen_achievements_read(b->owner, &d);
	{
		/* Pointer targets = the hint row's drawn cells (10-31). */
		bool ready = d.state == ACHIEVEMENTS_READY;
		struct gutil_hint_row hint_row;
		(void)achievements_row(f, &l, ready, &hint_row);
		l.requirement = achievements_row_cell(&hint_row, ready ? ACH_ROW_MID : 0);
		l.back = achievements_row_cell(&hint_row, ready ? ACH_ROW_BACK : 1);
	}
	int tab = screen_achievements_tab_of(b->selected), row = 0;
	while(row < branch_count[tab] && branches[tab][row] != b->selected)
		row++;
	bool direction = f->pressed[IB_GUI_UP] || f->pressed[IB_GUI_DOWN]
		|| f->pressed[IB_GUI_LEFT] || f->pressed[IB_GUI_RIGHT]
		|| f->pressed[IB_GUI_PREVIOUS] || f->pressed[IB_GUI_PAGE];
	float px, py, a;
	b->pointer_available = input_owner_pointer(f, x, y, w, h, &px, &py, &a);
	/* Store absolute coordinates: shared renderer and hit rectangles have the
	 * viewport offset already applied, including the second player's 240px. */
	px += x;
	py += y;
	b->pointer_x = px;
	b->pointer_y = py;
	b->pointer_angle = a;
	bool pointer_owns = gutil_menu_pointer_focus(
		&b->focus, b->pointer_available, px, py, direction);
	int target = -1;
	if(b->pointer_available) {
		if(d.state == ACHIEVEMENTS_READY) {
			if(l.split) {
				int first = screen_achievements_ribbon_first(b->selected);
				for(int i = 0; i < 7; i++)
					if(hit(l.tiles[i], px, py))
						target = first + i;
				for(int i = 0; i < 2; i++)
					if(hit(l.arrows[i], px, py))
						target = i ? TARGET_NEXT : TARGET_PREV;
			} else {
				for(int i = 0; i < branch_count[tab]; i++)
					if(hit(l.rows[i], px, py))
						target = branches[tab][i];
				for(int i = 0; i < 4; i++)
					if(hit(l.tabs[i], px, py))
						target = TARGET_TAB0 + i;
				for(int i = 0; i < 2; i++)
					if(hit(l.chips[i], px, py))
						target = i ? TARGET_NEXT : TARGET_PREV;
				if(hit(l.detail, px, py))
					target = TARGET_REQUIREMENT;
			}
			if(hit(l.requirement, px, py))
				target = TARGET_REQUIREMENT;
		} else if(hit(l.requirement, px, py))
			target = TARGET_RETRY;
		if(hit(l.back, px, py))
			target = TARGET_BACK;
	}
	b->pointer_target = target;
	if(pointer_owns && target >= 0) {
		b->origin = GUTIL_MENU_ORIGIN_POINTER;
		if(target < 16)
			select_id(b, target, MENU_SOUND_FOCUS);
		b->detail = target == TARGET_REQUIREMENT;
	}
	if(direction) {
		b->origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
		if(d.state == ACHIEVEMENTS_READY) {
			if(f->pressed[IB_GUI_PREVIOUS] && tab > 0)
				select_id(b, branches[tab - 1][0], MENU_SOUND_SECTION);
			else if(f->pressed[IB_GUI_PAGE] && tab < 3)
				select_id(b, branches[tab + 1][0], MENU_SOUND_SECTION);
			else if(l.split) {
				if(f->pressed[IB_GUI_LEFT])
					select_id(b, b->selected - 1, MENU_SOUND_FOCUS);
				else if(f->pressed[IB_GUI_RIGHT])
					select_id(b, b->selected + 1, MENU_SOUND_FOCUS);
			} else {
				if(f->pressed[IB_GUI_LEFT])
					b->detail = false;
				else if(f->pressed[IB_GUI_RIGHT])
					b->detail = true;
				else if(f->pressed[IB_GUI_UP] && row > 0)
					select_id(b, branches[tab][row - 1], MENU_SOUND_FOCUS);
				else if(f->pressed[IB_GUI_DOWN] && row + 1 < branch_count[tab])
					select_id(b, branches[tab][row + 1], MENU_SOUND_FOCUS);
			}
		}
	}
	if(!f->pressed[IB_GUI_CLICK] || direction)
		return ACHIEVEMENTS_NONE;
	/* A fresh click is deliberate intent, even before the IR has moved. Like
	 * ordinary menus, its first press focuses a different row; it must never
	 * follow the remembered row's parent while pointing at another one. */
	if(b->pointer_available && b->origin == GUTIL_MENU_ORIGIN_DEFAULT
	   && target >= 0 && target < 16 && target != b->selected) {
		select_id(b, target, MENU_SOUND_FOCUS);
		b->origin = GUTIL_MENU_ORIGIN_POINTER;
		return ACHIEVEMENTS_NONE;
	}
	int chosen = b->detail ? TARGET_REQUIREMENT : b->selected;
	/* Pointer owns an action only after deliberate movement. A stationary IR
	 * sprite cannot replace the directional requirement/selection. */
	if(b->pointer_available && b->origin != GUTIL_MENU_ORIGIN_DIRECTIONAL)
		chosen = target;
	if(chosen == TARGET_BACK) {
		struct input_player_frame back = *f;
		memset(back.pressed, 0, sizeof back.pressed);
		back.pressed[IB_INVENTORY] = true;
		return screen_achievements_update_owned(b, &back, x, y, w, h);
	}
	if(d.state != ACHIEVEMENTS_READY) {
  if(achievements_slot_save_failed(b->owner)) achievements_request_save(b->owner);
  return ACHIEVEMENTS_NONE;
 }
	if(chosen == TARGET_PREV || chosen == TARGET_NEXT) {
		int dir = chosen == TARGET_PREV ? -1 : 1;
		if(l.split)
			select_id(b, b->selected + dir, MENU_SOUND_FOCUS);
		else if(tab + dir >= 0 && tab + dir < 4)
			select_id(b, branches[tab + dir][0], MENU_SOUND_SECTION);
	} else if(chosen >= TARGET_TAB0 && chosen < TARGET_TAB0 + 4)
		select_id(b, branches[chosen - TARGET_TAB0][0], MENU_SOUND_SECTION);
	else if(chosen >= 0)
		requirement(b, &d);
	return ACHIEVEMENTS_NONE;
}

static void solid(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b,
				  uint8_t a) {
	gfx_texture(false);
	gutil_texquad_col(x, y, 0, 0, 0, 0, w, h, r, g, b, a);
	gfx_texture(true);
}
static void panel(struct menu_rect r) {
	solid(r.x - 2, r.y - 2, r.w + 4, r.h + 4, 17, 17, 17, 255);
	gfx_bind_texture(&texture_gui_crafting);
	for(int y = 0; y < r.h; y += 32)
		for(int x = 0; x < r.w; x += 32) {
			int w = r.w - x < 32 ? r.w - x : 32,
				h = r.h - y < 32 ? r.h - y : 32;
			gutil_texquad_col(r.x + x, r.y + y, 150, 62, (w + 1) / 2,
							  (h + 1) / 2, w, h, 255, 255, 255, 255);
		}
	solid(r.x, r.y, r.w, 2, 255, 255, 255, 153);
	solid(r.x, r.y, 2, r.h, 255, 255, 255, 153);
	solid(r.x, r.y + r.h - 2, r.w, 2, 85, 85, 85, 255);
	solid(r.x + r.w - 2, r.y, 2, r.h, 85, 85, 85, 255);
}
static void button(struct menu_rect r, bool focus) {
	gfx_bind_texture(&texture_gui_widgets);
	int half = r.w / 4, ty = focus ? 86 : 66;
	gutil_texquad_col(r.x, r.y, 0, ty, half, 20, r.w / 2, r.h, 255, 255, 255,
					  255);
	gutil_texquad_col(r.x + r.w / 2, r.y, 200 - half, ty, half, 20,
					  r.w - r.w / 2, r.h, 255, 255, 255, 255);
}
/* Wrap complete words at the approved font sizes. No truncation/ellipsis.
 * The caller reserves enough lines for every catalog string. */
static int copy_text(int x, int y, int width, const char* str, int size,
					 bool shadow, uint8_t r, uint8_t g, uint8_t b) {
	char line[192];
	size_t n = 0;
	int lines = 0;
	while(*str) {
		while(*str == ' ')
			str++;
		const char* end = str;
		while(*end && *end != ' ')
			end++;
		size_t len = (size_t)(end - str);
		if(!len)
			break;
		char trial[192];
		snprintf(trial, sizeof trial, "%s%.*s", n ? " " : "", (int)len, str);
		if(n
		   && gutil_font_width(line, size) + gutil_font_width(trial, size)
			   > width) {
			gutil_text_rgb(x, y + lines * 20, line, size, shadow, r, g, b);
			lines++;
			n = 0;
		}
		if(n)
			line[n++] = ' ';
		if(len >= sizeof line - n)
			return lines; /* all fixed catalog strings fit */
		memcpy(line + n, str, len);
		n += len;
		line[n] = 0;
		str = end;
	}
	if(n) {
		gutil_text_rgb(x, y + lines * 20, line, size, shadow, r, g, b);
		lines++;
	}
	return lines;
}
/* Private derived textures preserve native UVs, alpha, filtering and geometry.
 * No shared pack texture is changed. Reopening invalidates both copies;
 * ordinary pack switching leaves this screen first. The cache is bounded to two
 * atlases, lives on the heap and is replaced only after the GPU has finished
 * reading it. On GX every encoded mip texel is transformed, retaining the
 * original alpha bits and mip layout instead of flattening the active pack's
 * resolution. */
static struct {
	struct tex_gfx image;
	const void* source;
	size_t bytes;
} gray_atlas[2];
static uint8_t gray_luma(unsigned r, unsigned g, unsigned b) {
	/* CSS grayscale(1) followed by contrast(1.5), from the approved art. */
	float luma = (r * 2126 + g * 7152 + b * 722) / 10000.0F;
	float contrast = luma * 1.5F - 63.75F;
	return contrast < 0 ? 0 : contrast > 255 ? 255 : (uint8_t)(contrast + .5F);
}
static struct tex_gfx* gray_texture(struct tex_gfx* src, unsigned which) {
	if(!src->data || !src->data_bytes)
		return NULL;
	if(gray_refresh) {
		gfx_wait_idle();
		for(unsigned i = 0; i < 2; i++) {
			tex_gfx_clear(&gray_atlas[i].image);
			gray_atlas[i].source = NULL;
		}
		gray_refresh = false;
	}
	if(gray_atlas[which].source == src->data
	   && gray_atlas[which].bytes == src->data_bytes
	   && gray_atlas[which].image.width == src->width
	   && gray_atlas[which].image.height == src->height)
		return &gray_atlas[which].image;
	uint8_t* data = tex_cache_texel_alloc(src->data_bytes);
	if(!data)
		return NULL;
	memcpy(data, src->data, src->data_bytes);
#ifdef PLATFORM_WII
	/* Both approved icon atlases are RGB5A3. Work directly on every encoded
	 * texel, including mip levels, without relying on tex_gfx_lookup (which
	 * supports only I8/IA4 on GX). Keep all alpha bits exactly unchanged. */
	if(src->fmt != TEX_FMT_RGBA16) {
		tex_cache_texel_free(data);
		return NULL;
	}
	for(size_t i = 0; i + 1 < src->data_bytes; i += 2) {
		uint16_t v = (uint16_t)((data[i] << 8) | data[i + 1]);
		uint16_t out;
		if(v & 0x8000) {
			unsigned rr = (v >> 10) & 31, gg = (v >> 5) & 31, bb = v & 31;
			unsigned q = gray_luma((rr << 3) | (rr >> 2), (gg << 3) | (gg >> 2),
								   (bb << 3) | (bb >> 2))
				>> 3;
			out = 0x8000 | (q << 10) | (q << 5) | q;
		} else {
			unsigned q = gray_luma(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17,
								   (v & 15) * 17)
				>> 4;
			out = (v & 0x7000) | (q << 8) | (q << 4) | q;
		}
		data[i] = (uint8_t)(out >> 8);
		data[i + 1] = (uint8_t)out;
	}
#else
	for(size_t i = 0; i + 3 < src->data_bytes; i += 4)
		data[i] = data[i + 1] = data[i + 2]
			= gray_luma(data[i], data[i + 1], data[i + 2]);
#endif
	gfx_wait_idle();
	if(!tex_gfx_restore(&gray_atlas[which].image, data, src->data_bytes,
						src->width, src->height, src->fmt, src->lods,
						src->linear, which == 1))
		return NULL; /* restore owns/frees data even on failure */
	gray_atlas[which].source = src->data;
	gray_atlas[which].bytes = src->data_bytes;
	return &gray_atlas[which].image;
}
struct icon_vertex {
	float x, y, z, u, v;
};
/* Clip in the caller's GUI coordinate space, preserving its viewport/scissor.
 * This is needed for the approved enlarged locked art inside a small slot. */
static void icon_quad(struct icon_vertex q[4], const struct menu_rect* clip,
					  uint8_t shade, uint8_t alpha) {
	struct icon_vertex a[12], b[12];
	memcpy(a, q, sizeof(struct icon_vertex) * 4);
	int count = 4;
	if(clip)
		for(int edge = 0; edge < 4; edge++) {
			float limit = edge == 0 ? clip->x :
				edge == 1			? clip->x + clip->w :
				edge == 2			? clip->y :
									  clip->y + clip->h;
			int out = 0;
			for(int i = 0; i < count; i++) {
				struct icon_vertex p = a[i], n = a[(i + 1) % count];
				float pv = edge < 2 ? p.x : p.y, nv = edge < 2 ? n.x : n.y;
				bool pi = edge == 0 || edge == 2 ? pv >= limit : pv <= limit;
				bool ni = edge == 0 || edge == 2 ? nv >= limit : nv <= limit;
				if(pi)
					b[out++] = p;
				if(pi != ni) {
					float t = (limit - pv) / (nv - pv);
					b[out++] = (struct icon_vertex) {
						p.x + (n.x - p.x) * t, p.y + (n.y - p.y) * t,
						p.z + (n.z - p.z) * t, p.u + (n.u - p.u) * t,
						p.v + (n.v - p.v) * t};
				}
			}
			count = out;
			memcpy(a, b, sizeof(struct icon_vertex) * count);
		}
	/* A clipped quad has at most eight vertices. Triangle fans are emitted
	 * as quads with a repeated final vertex on both GL and GX. */
	for(int i = 1; i + 1 < count; i++) {
		int indices[4] = {0, i, i + 1, i + 1};
		float pos[12], uv[8];
		uint8_t color[16];
		for(int k = 0; k < 4; k++) {
			struct icon_vertex v = a[indices[k]];
			pos[k * 3] = v.x;
			pos[k * 3 + 1] = v.y;
			pos[k * 3 + 2] = v.z;
			uv[k * 2] = v.u;
			uv[k * 2 + 1] = v.v;
			color[k * 4] = color[k * 4 + 1] = color[k * 4 + 2] = shade;
			color[k * 4 + 3] = alpha;
		}
		gfx_draw_quads_flt(4, pos, color, uv);
	}
}
static void gray_cube(struct item* it, struct item_data* stack, mat4 base,
					  uint8_t alpha, const struct menu_rect* clip) {
	/* The catalog's three block icons are full cubes. Match the inventory
	 * renderer's matrix, face UV directions, registry metadata and light LUT.
	 * Native capture compares all 16 catalog silhouettes at 16/24/32px. */
	static const vec3 origins[6]
		= {{0, 1, 0}, {0, 0, 1}, {0, 1, 0}, {1, 1, 1}, {1, 1, 0}, {0, 1, 1}};
	static const vec3 us[6]
		= {{1, 0, 0}, {1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {-1, 0, 0}, {1, 0, 0}};
	static const vec3 vs[6] = {{0, 0, 1},  {0, 0, -1}, {0, -1, 0},
							   {0, -1, 0}, {0, -1, 0}, {0, -1, 0}};
	static const uint8_t lights[6] = {0xff, 0xcc, 0xdd, 0xdd, 0xee, 0xee};
	struct block_data data = {.type = stack->id,
							  .metadata = it->render_data.block.has_default ?
								  it->render_data.block.default_metadata :
								  stack->durability,
							  .sky_light = 15,
							  .torch_light = 15};
	struct block_info info = {.block = &data};
	mat4 model, mv;
	glm_translate_make(model, (vec3) {6, 6, -16});
	glm_scale(model, (vec3) {20, 20, -20});
	glm_translate(model, (vec3) {.5F, .5F, .5F});
	glm_rotate_z(model, glm_rad(180), model);
	glm_rotate_x(model, glm_rad(-30), model);
	glm_rotate_y(model,
				 glm_rad((it->render_data.block.has_default ?
							  it->render_data.block.default_rotation * 90.0F :
							  0)
						 - 45),
				 model);
	glm_translate(model, (vec3) {-.5F, -.5F, -.5F});
	glm_mat4_mul(base, model, mv);
	/* The inventory matrix reverses winding. Cull hidden faces so the
	 *
	 * approved group opacity is applied once, rather than blending through

	 * * the back of the cube. GUI mode normally leaves culling disabled. */
	gfx_cull_func(MODE_FRONT);
	for(int face = 0; face < SIDE_MAX; face++) {
		uint8_t tile
			= blocks[stack->id]->getTextureIndex(&info, (enum side)face);
		float tx = TEX_OFFSET(TEXTURE_X(tile)) / 256.0F;
		float ty = TEX_OFFSET(TEXTURE_Y(tile)) / 256.0F;
		static const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
		struct icon_vertex q[4];
		for(int k = 0; k < 4; k++) {
			vec4 p = {0, 0, 0, 1}, out;
			for(int axis = 0; axis < 3; axis++)
				p[axis] = origins[face][axis] + us[face][axis] * corners[k][0]
					+ vs[face][axis] * corners[k][1];
			glm_mat4_mulv(mv, p, out);
			q[k] = (struct icon_vertex) {out[0], out[1], out[2],
										 tx + corners[k][0] / 16,
										 ty + corners[k][1] / 16};
		}
		icon_quad(q, clip, (uint8_t)(gfx_lookup_light(lights[face]) * 255),
				  alpha);
	}
	gfx_cull_func(MODE_NONE);
}
static void icon_art(const struct achievements_data* d, int id, int x, int y,
					 int size, uint8_t alpha, const struct menu_rect* clip) {
	if(id < 0 || id >= 16 || d->state != ACHIEVEMENTS_READY)
		return;
	struct item_data stack = {.id = d->rows[id].icon_item, .count = 1};
	if(screen_achievements_state(d, id) != ACHIEVEMENT_LOCKED) {
		gutil_draw_item_sized(&stack, x, y, size, 1);
		return;
	}
	struct item* it = item_get(&stack);
	if(!it)
		return;
	struct tex_gfx* gray
		= gray_texture(stack.id >= 256 ? &texture_items : &texture_terrain,
					   stack.id >= 256 ? 0 : 1);
	if(!gray)
		return;
	gfx_bind_texture(gray);
	gfx_depth_range(.1F, .2F);
	gfx_write_buffers(true, true, true);
	gfx_texture(true);
	gfx_lighting(false);
	mat4 base;
	glm_translate_make(base, (vec3) {x, y, 0});
	glm_scale(base, (vec3) {size / 32.0F, size / 32.0F, 1});
#ifdef GFX_3D_ELEMENTS
	if(stack.id < 256 && it->renderItem == render_item_block)
		gray_cube(it, &stack, base, alpha, clip);
	else
#endif
	{
		unsigned tx, ty;
		if(stack.id >= 256) {
			tx = it->render_data.item.texture_x * 16;
			ty = it->render_data.item.texture_y * 16;
		} else {
			struct block_data data = {.type = stack.id,
									  .metadata = stack.durability,
									  .sky_light = 15,
									  .torch_light = 15};
			struct block_info info = {.block = &data};
			uint8_t tile = blocks[stack.id]->getTextureIndex(&info, SIDE_TOP);
			tx = TEX_OFFSET(TEXTURE_X(tile));
			ty = TEX_OFFSET(TEXTURE_Y(tile));
		}
		struct icon_vertex q[4]
			= {{x, y, -2, tx / 256.0F, ty / 256.0F},
			   {x + size, y, -2, (tx + 16) / 256.0F, ty / 256.0F},
			   {x + size, y + size, -2, (tx + 16) / 256.0F, (ty + 16) / 256.0F},
			   {x, y + size, -2, tx / 256.0F, (ty + 16) / 256.0F}};
		icon_quad(q, clip, 255, alpha);
	}
	gfx_matrix_modelview(GLM_MAT4_IDENTITY);
	gfx_write_buffers(true, false, false);
	gfx_depth_range(0, 1);
}
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void screen_achievements_test_icon(uint16_t item, bool locked, int x, int y,
								   int size) {
	struct achievements_data d = {.state = ACHIEVEMENTS_READY};
	d.rows[0].icon_item = item;
	d.rows[0].unlocked = !locked;
	icon_art(&d, 0, x, y, size, 255, NULL);
}
#endif
static const char* state_word(enum achievement_visual_state state) {
	return state == ACHIEVEMENT_EARNED ? "Earned" :
		state == ACHIEVEMENT_AVAILABLE ? "Available" :
		state == ACHIEVEMENT_LOCKED	   ? "Locked" :
										 "Unavailable";
}
static void float_rect(float x, float y, float w, float h, uint8_t r, uint8_t g,
					   uint8_t b) {
	gfx_texture(false);
	gfx_draw_quads_flt(
		4, (float[]) {x, y, -2, x + w, y, -2, x + w, y + h, -2, x, y + h, -2},
		(uint8_t[]) {r, g, b, 255, r, g, b, 255, r, g, b, 255, r, g, b, 255},
		(float[8]) {0});
	gfx_texture(true);
}
static void lock_rect(float x, float y, float w, float h, bool shadow) {
	float_rect(x, y, w, h, shadow ? 85 : 200, shadow ? 89 : 201,
			   shadow ? 76 : 184);
}
static void padlock(struct menu_rect r) {
	/* Prototype border-box 26x22, origin (13,4), scale .75. */
	float x = r.x + r.w * .5F - 9.75F, y = r.y + r.h * .5F - 3;
	lock_rect(x + 1.5F, y + 1.5F, 19.5F, 16.5F, true);
	lock_rect(x, y, 19.5F, 2.25F, false);
	lock_rect(x, y + 14.25F, 19.5F, 2.25F, false);
	lock_rect(x, y + 2.25F, 2.25F, 12, false);
	lock_rect(x + 17.25F, y + 2.25F, 2.25F, 12, false);
	lock_rect(x + 6, y - 8.25F, 10.5F, 9, true);
	lock_rect(x + 4.5F, y - 8.25F, 10.5F, 2.25F, false);
	lock_rect(x + 4.5F, y - 6, 2.25F, 6.75F, false);
	lock_rect(x + 12.75F, y - 6, 2.25F, 6.75F, false);
	lock_rect(x + 8.25F, y + 6, 3, 5.25F, false);
}
static void icon(const struct achievements_data* d, int id, int x, int y,
				 int size) {
	if(id < 0 || id >= 16 || d->state != ACHIEVEMENTS_READY)
		return;
	struct item_data stack = {.id = d->rows[id].icon_item, .count = 1};
	gutil_draw_item_sized(&stack, x, y, size, 1);
}
static void locked_art(const struct achievements_data* d, int id,
					   struct menu_rect slot_rect) {
	/* The original art is 32px; CSS centers it, then translates -4px and
	 * scales 1.25 around its center. Clip without changing parent scissor. */
	int x = slot_rect.x + (slot_rect.w - 32) / 2 - 8;
	int y = slot_rect.y + (slot_rect.h - 32) / 2 - 8;
	icon_art(d, id, x, y, 40, 89, &slot_rect);
	padlock(slot_rect);
}
static void badge(int x, int y, enum achievement_visual_state state) {
	if(state != ACHIEVEMENT_EARNED)
		return;
	/* Green check only: locked slots have the dominant centered padlock. */
	for(int i = 0; i < 4; i++)
		solid(x + 2 + i, y + 7 + i, 2, 2, 44, 117, 36, 255);
	for(int i = 0; i < 7; i++)
		solid(x + 5 + i, y + 10 - i, 2, 2, 44, 117, 36, 255);
}
static void slot(struct menu_rect r, bool special, bool locked, bool focus) {
	if(special) {
		solid(r.x - 2, r.y - 2, r.w + 4, r.h + 4, 96, 70, 24, 255);
		solid(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 255, 222, 92, 255);
	}
	gfx_bind_texture(&texture_gui_crafting);
	if(locked)
		solid(r.x, r.y, r.w, r.h, 133, 133, 123, 255);
	gfx_bind_texture(&texture_gui_crafting);
	gutil_texquad(r.x, r.y, 29, 16, 18, 18, r.w, r.h);
	if(focus) {
		solid(r.x - 2, r.y - 2, r.w + 4, 2, 248, 244, 209, 255);
		solid(r.x - 2, r.y + r.h, r.w + 4, 2, 248, 244, 209, 255);
		solid(r.x - 2, r.y, 2, r.h, 248, 244, 209, 255);
		solid(r.x + r.w, r.y, 2, r.h, 248, 244, 209, 255);
	}
}
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static struct gutil_hint_row s_test_row;
const struct gutil_hint_row* screen_achievements_test_row(void) {
	return &s_test_row;
}
#endif
static void footer(const struct achievements_board* b,
				   const struct input_player_frame* f,
				   const struct achievements_layout* l, bool ready) {
	(void)b;
	struct gutil_hint_row row;
	if(achievements_row(f, l, ready, &row))
		gutil_hint_row_draw(&row);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	s_test_row = row;
#endif
}
static void relation(const struct achievements_data* d, int id,
					 struct menu_rect r, bool split, bool focus) {
	int parent = d->rows[id].parent;
	if(parent < 0) {
		gutil_text_rgb(r.x, r.y, "First achievement", 16, false, 64, 64, 64);
		return;
	}
	if(!split) {
		gutil_text_rgb(r.x, r.y, "Requires:", 16, false, 64, 64, 64);
		icon(d, parent, r.x, r.y + 24, 16);
		copy_text(r.x + 24, r.y + 24, r.w - 24, names[parent], 16, false,
				  focus ? 32 : 64, focus ? 32 : 64, 64);
	} else {
		int at = r.x;
		gutil_text_rgb(at, r.y, "Requires:", 16, false, 64, 64, 64);
		at += gutil_font_width("Requires:", 16) + 8;
		icon(d, parent, at, r.y, 16);
		gutil_text_rgb(at + 22, r.y, names[parent], 16, false, 64, 64, 64);
	}
}

void screen_achievements_render_owned(const struct achievements_board* b,
									  const struct input_player_frame* f, int x,
									  int y, int w, int h) {
	if(!b || !b->open || b->selected < 0 || b->selected >= 16 || w <= 0
	   || h <= 0)
		return;
	struct achievements_layout l;
	screen_achievements_layout(&l, x, y, w, h);
	struct achievements_data d;
	screen_achievements_read(b->owner, &d);
	/* Layout stays within the supplied view. The caller's per-view scissor
	 * also clips the rotated IR cursor at its edges, as in main's menu painter.
	 * Do not clear or replace that caller-owned viewport/scissor state. */
	solid(x, y, w, h, 0, 0, 0, 180);
	panel(l.panel);
	int title_x = l.split ? l.header.x :
							x + (w - gutil_font_width("Achievements", 20)) / 2;
	gutil_text_rgb(title_x, l.header.y + 2, "Achievements", 20, false, 64, 64,
				   64);
	char count[40];
	if(d.earned < 0) {
		/* The approved unknown count uses an em dash. The pack font is a byte
		 * atlas, so draw that one punctuation mark with the existing primitive.
		 */
		snprintf(count, sizeof count, l.split ? "Player %u   " : "",
				 b->owner + 1);
		int prefix = gutil_font_width(count, 16);
		int at = l.header.x + l.header.w - prefix - 16
			- gutil_font_width("/16", 16);
		gutil_text_rgb(at, l.header.y + 3, count, 16, false, 64, 64, 64);
		solid(at + prefix, l.header.y + 11, 12, 2, 64, 64, 64, 255);
		gutil_text_rgb(at + prefix + 16, l.header.y + 3, "/16", 16, false, 64,
					   64, 64);
	} else {
		if(l.split)
			snprintf(count, sizeof count, "Player %u   %d/16", b->owner + 1,
					 d.earned);
		else
			snprintf(count, sizeof count, "%d/16", d.earned);
		gutil_text_rgb(l.header.x + l.header.w - gutil_font_width(count, 16),
					   l.header.y + 3, count, 16, false, 64, 64, 64);
	}
	if(d.state != ACHIEVEMENTS_READY) {
		copy_text(l.panel.x + 24, l.panel.y + (l.split ? 84 : 160),
				  l.panel.w - 48, d.message, 16, false, 64, 64, 64);
		footer(b, f, &l, false);
	} else if(l.split) {
		int first = screen_achievements_ribbon_first(b->selected);
		for(int i = 0; i < 7; i++) {
			int id = first + i;
			struct menu_rect r = l.tiles[i];
			enum achievement_visual_state st
				= screen_achievements_state(&d, id);
			slot(r, d.rows[id].special, st == ACHIEVEMENT_LOCKED,
				 b->selected == id);
			if(st == ACHIEVEMENT_LOCKED)
				locked_art(&d, id, r);
			else
				icon(&d, id, r.x + (r.w - 32) / 2, r.y + (r.h - 32) / 2, 32);
			badge(r.x + r.w - 15, r.y + r.h - 15, st);
		}
		for(int i = 0; i < 2; i++) {
			struct menu_rect r = l.arrows[i];
			gfx_bind_texture(&texture_gui_crafting);
			gutil_texquad_col(r.x + 10, r.y + 14, i ? 89 : 113, 35,
							  i ? 24 : -24, 16, 24, 16, 255, 255, 255,
							  (i ? b->selected < 15 : b->selected > 0) ? 255 :
																		 85);
		}
		enum achievement_visual_state st
			= screen_achievements_state(&d, b->selected);
		const char* word = state_word(st);
		int tw = gutil_font_width(names[b->selected], 20);
		int at = l.title.x;
		gutil_text_rgb(at, l.title.y, names[b->selected], 20, false, 64, 64,
					   64);
		solid(at + tw + 8, l.title.y + 10, 4, 4, 64, 64, 64, 255);
		gutil_text_rgb(at + tw + 20, l.title.y + 2, word, 16, false,
					   st == ACHIEVEMENT_EARNED		   ? 44 :
						   st == ACHIEVEMENT_AVAILABLE ? 64 :
														 85,
					   st == ACHIEVEMENT_EARNED		   ? 117 :
						   st == ACHIEVEMENT_AVAILABLE ? 64 :
														 85,
					   st == ACHIEVEMENT_EARNED		   ? 36 :
						   st == ACHIEVEMENT_AVAILABLE ? 64 :
														 85);
		copy_text(l.howto.x, l.howto.y, l.howto.w, instructions[b->selected],
				  16, false, 64, 64, 64);
		relation(&d, b->selected, l.requires, true, false);
		footer(b, f, &l, true);
	} else {
		int tab = screen_achievements_tab_of(b->selected);
		/* The tab chips are plain "<" ">" click targets (10-31, Q2): the
		 * page roles are not drawn inside the panel. */
		for(int i = 0; i < 2; i++) {
			slot(l.chips[i], false, false, false);
			const char* arrow = i ? ">" : "<";
			gutil_text_rgb(l.chips[i].x
							   + (l.chips[i].w - gutil_font_width(arrow, 16)) / 2,
						   l.chips[i].y + (l.chips[i].h - 16) / 2, arrow, 16,
						   false, 64, 64, 64);
		}
		for(int t = 0; t < 4; t++) {
			struct menu_rect r = l.tabs[t];
			uint8_t bg = t == tab ? 198 : 139;
			solid(r.x, r.y, r.w, r.h, bg, bg, bg, 255);
			solid(r.x, r.y, r.w, 2, 255, 255, 255, 255);
			solid(r.x, r.y, 2, r.h, 255, 255, 255, 255);
			solid(r.x + r.w - 2, r.y, 2, r.h, 112, 112, 112, 255);
			if(t != tab)
				solid(r.x, r.y + r.h - 2, r.w, 2, 112, 112, 112, 255);
			int at
				= r.x + (r.w - 30 - gutil_font_width(branch_names[t], 16)) / 2;
			icon(&d, branches[t][0], at, r.y + 6, 24);
			gutil_text_rgb(at + 30, r.y + 10, branch_names[t], 16, false, 64,
						   64, 64);
		}
		int earned = 0;
		for(int i = 0; i < branch_count[tab]; i++)
			earned += d.rows[branches[tab][i]].unlocked;
		char label[48];
		snprintf(label, sizeof label, "%s %d/%d", branch_names[tab], earned,
				 branch_count[tab]);
		gutil_text_rgb(l.list_header.x, l.list_header.y, label, 16, false, 64,
					   64, 64);
		for(int i = 0; i < branch_count[tab]; i++) {
			int id = branches[tab][i];
			struct menu_rect r = l.rows[i];
			enum achievement_visual_state st
				= screen_achievements_state(&d, id);
			button(r, b->selected == id && !b->detail);
			if(d.rows[id].special || st == ACHIEVEMENT_LOCKED)
				slot((struct menu_rect) {r.x + 6, r.y + 4, 32, 32},
					 d.rows[id].special, st == ACHIEVEMENT_LOCKED, false);
			if(st == ACHIEVEMENT_LOCKED)
				locked_art(&d, id,
						   (struct menu_rect) {r.x + 6, r.y + 4, 32, 32});
			else
				icon(&d, id, r.x + 6, r.y + 4, 32);
			copy_text(
				r.x + 52,
				r.y + (gutil_font_width(names[id], 16) > r.w - 82 ? 0 : 10),
				r.w - 82, names[id], 16, true, 255, 255, 255);
			badge(r.x + r.w - 22, r.y + 13, st);
		}
		int id = b->selected;
		enum achievement_visual_state st = screen_achievements_state(&d, id);
		int lines = copy_text(l.title.x, l.title.y, l.title.w, names[id], 20,
							  false, 64, 64, 64);
		gutil_text_rgb(l.detail.x, l.detail.y + lines * 20 + 8, state_word(st),
					   16, false,
					   st == ACHIEVEMENT_EARNED		   ? 44 :
						   st == ACHIEVEMENT_AVAILABLE ? 64 :
														 85,
					   st == ACHIEVEMENT_EARNED		   ? 117 :
						   st == ACHIEVEMENT_AVAILABLE ? 64 :
														 85,
					   st == ACHIEVEMENT_EARNED		   ? 36 :
						   st == ACHIEVEMENT_AVAILABLE ? 64 :
														 85);
		int desc_y = l.detail.y + lines * 20 + 40;
		int desc_lines = copy_text(l.detail.x, desc_y, l.detail.w,
								   instructions[id], 16, false, 64, 64, 64);
		struct menu_rect req
			= {l.detail.x, desc_y + desc_lines * 20 + 12, l.detail.w, 60};
		relation(&d, id, req, false, b->detail);
		int kids_y = req.y + (d.rows[id].parent < 0 ? 32 : 64);
		char kids[192] = "Leads to:";
		bool any = false;
		for(int i = 0; i < 16; i++)
			if(d.rows[i].parent == id) {
				size_t used = strlen(kids);
				snprintf(kids + used, sizeof kids - used, "%s%s",
						 any ? ", " : " ", names[i]);
				any = true;
			}
		if(any)
			copy_text(l.detail.x, kids_y, l.detail.w, kids, 16, false, 64, 64,
					  64);
		footer(b, f, &l, true);
	}
	if(b->pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(b->pointer_x, b->pointer_y,
							 glm_rad(b->pointer_angle), 0, 0, 256, 256, 48, 48);
	}
}

void screen_achievements_return_pause(void) {
	boards[0].open = false;
	screen_pause_restore_achievements();
}
static void reset(struct screen* s, int w, int h) {
	(void)s;
	(void)w;
	(void)h;
	screen_achievements_open(&boards[0], 0);
	input_pointer_enable(true);
}
static void update(struct screen* s, float dt) {
	(void)s;
	(void)dt;
	struct input_player_frame f;
	input_prompt_frame(0, &f);
	/* Primary aliases include the existing PC/agent input bridge. */
	for(int i = 0; i < IB_COUNT; i++)
		f.pressed[i] = input_pressed((enum input_button)i);
	float px = 0, py = 0, a = 0;
	f.pointer_valid = input_pointer(&px, &py, &a);
	f.pointer_normalized = false;
	f.pointer_x = px;
	f.pointer_y = py;
	f.pointer_angle = a;
	f.connected = true;
	enum achievements_action result = screen_achievements_update_owned(
		&boards[0], &f, 0, 0, gfx_width(), gfx_height());
	if(result == ACHIEVEMENTS_BACK)
		screen_achievements_return_pause();
	else if(result == ACHIEVEMENTS_RESUME && !screen_pause_resume_game())
		boards[0].open = true;
}
static void render(struct screen* s, int w, int h) {
	(void)s;
	struct input_player_frame f;
	input_prompt_frame(0, &f);
	screen_achievements_render_owned(&boards[0], &f, 0, 0, w, h);
}
struct screen screen_achievements = {
	.reset = reset, .update = update, .render2D = render, .render_world = true};
