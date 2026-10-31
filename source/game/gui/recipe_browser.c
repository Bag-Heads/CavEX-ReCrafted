/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
/* Console Workbench / Pocket Ledger recipe guide -- see recipe_browser.h. */
#include <stdio.h>
#include <string.h>

#include "recipe_browser.h"
#include "../../network/inventory_crafting.h"
#include "container_compact.h"
#include "hud_survival_row.h"
#include "../game_state.h"
#include "../../config.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../block/blocks_data.h"
#include "../../item/items.h"
#include "../../item/recipe.h"
#include "../../platform/gfx.h"
#include "../../platform/input_glyph.h"
#include "../../platform/input_owner.h"

#define CATALOG_NO_ALT RECIPE_CATALOG_NO_ALT

/* ---------------------------------------------------------- the catalog -- */

/* Approved catalog (docs/ui_design/approved/menus.html #recipe-catalog,
 * recipe.c sha256 62171042ac17355e70c878cab13e02fb3505948b8d2f4e68049d86ae9c046b15).
 * Entries are flat registry indices in the recipe_craft.h numbering, so a
 * selection made here names the same registration on the server with no
 * translation table. tests/test_recipe_ui.c re-derives the counts, the
 * exclusions and the stew merge from the live registry. */
static const struct recipe_catalog_entry catalog_entries[RECIPE_CATALOG_ENTRIES] = {
	{72,CATALOG_NO_ALT}, {51,CATALOG_NO_ALT}, {52,CATALOG_NO_ALT}, {53,CATALOG_NO_ALT}, {50,CATALOG_NO_ALT}, {22,CATALOG_NO_ALT},
	{23,CATALOG_NO_ALT}, {43,CATALOG_NO_ALT}, {135,CATALOG_NO_ALT}, {134,CATALOG_NO_ALT}, {133,CATALOG_NO_ALT}, {132,CATALOG_NO_ALT},
	{131,CATALOG_NO_ALT}, {130,CATALOG_NO_ALT}, {129,CATALOG_NO_ALT}, {128,CATALOG_NO_ALT}, {127,CATALOG_NO_ALT}, {126,CATALOG_NO_ALT},
	{125,CATALOG_NO_ALT}, {124,CATALOG_NO_ALT}, {123,CATALOG_NO_ALT}, {122,CATALOG_NO_ALT}, {121,CATALOG_NO_ALT}, {120,CATALOG_NO_ALT},
	{38,CATALOG_NO_ALT}, {41,CATALOG_NO_ALT}, {39,CATALOG_NO_ALT}, {40,CATALOG_NO_ALT}, {29,CATALOG_NO_ALT}, {30,CATALOG_NO_ALT},
	{33,CATALOG_NO_ALT}, {31,CATALOG_NO_ALT}, {12,CATALOG_NO_ALT}, {37,CATALOG_NO_ALT}, {7,CATALOG_NO_ALT}, {5,CATALOG_NO_ALT},
	{35,CATALOG_NO_ALT}, {56,CATALOG_NO_ALT}, {42,CATALOG_NO_ALT}, {57,CATALOG_NO_ALT}, {10,CATALOG_NO_ALT}, {58,CATALOG_NO_ALT},
	{32,CATALOG_NO_ALT}, {8,CATALOG_NO_ALT}, {76,CATALOG_NO_ALT}, {81,CATALOG_NO_ALT}, {91,CATALOG_NO_ALT}, {96,CATALOG_NO_ALT},
	{86,CATALOG_NO_ALT}, {77,CATALOG_NO_ALT}, {82,CATALOG_NO_ALT}, {92,CATALOG_NO_ALT}, {97,CATALOG_NO_ALT}, {87,CATALOG_NO_ALT},
	{78,CATALOG_NO_ALT}, {83,CATALOG_NO_ALT}, {93,CATALOG_NO_ALT}, {98,CATALOG_NO_ALT}, {88,CATALOG_NO_ALT}, {79,CATALOG_NO_ALT},
	{84,CATALOG_NO_ALT}, {94,CATALOG_NO_ALT}, {99,CATALOG_NO_ALT}, {89,CATALOG_NO_ALT}, {36,CATALOG_NO_ALT}, {17,CATALOG_NO_ALT},
	{74,CATALOG_NO_ALT}, {44,CATALOG_NO_ALT}, {26,CATALOG_NO_ALT}, {25,CATALOG_NO_ALT}, {27,CATALOG_NO_ALT}, {75,CATALOG_NO_ALT},
	{80,CATALOG_NO_ALT}, {90,CATALOG_NO_ALT}, {95,CATALOG_NO_ALT}, {85,CATALOG_NO_ALT}, {0,CATALOG_NO_ALT}, {45,CATALOG_NO_ALT},
	{100,CATALOG_NO_ALT}, {108,CATALOG_NO_ALT}, {116,CATALOG_NO_ALT}, {112,CATALOG_NO_ALT}, {101,CATALOG_NO_ALT}, {109,CATALOG_NO_ALT},
	{117,CATALOG_NO_ALT}, {113,CATALOG_NO_ALT}, {102,CATALOG_NO_ALT}, {110,CATALOG_NO_ALT}, {118,CATALOG_NO_ALT}, {114,CATALOG_NO_ALT},
	{103,CATALOG_NO_ALT}, {111,CATALOG_NO_ALT}, {119,CATALOG_NO_ALT}, {115,CATALOG_NO_ALT}, {60,CATALOG_NO_ALT}, {61,CATALOG_NO_ALT},
	{59,CATALOG_NO_ALT}, {65,CATALOG_NO_ALT}, {66,CATALOG_NO_ALT}, {64,CATALOG_NO_ALT}, {62,CATALOG_NO_ALT}, {63,CATALOG_NO_ALT},
	{28,CATALOG_NO_ALT}, {9,CATALOG_NO_ALT}, {11,CATALOG_NO_ALT}, {19,CATALOG_NO_ALT}, {20,CATALOG_NO_ALT}, {21,CATALOG_NO_ALT},
	{13,CATALOG_NO_ALT}, {14,CATALOG_NO_ALT}, {15,CATALOG_NO_ALT}, {16,CATALOG_NO_ALT}, {54,CATALOG_NO_ALT}, {46,47},
	{73,CATALOG_NO_ALT}, {18,CATALOG_NO_ALT}, {24,CATALOG_NO_ALT}, {71,CATALOG_NO_ALT}, {34,CATALOG_NO_ALT}, {55,CATALOG_NO_ALT},
	{48,CATALOG_NO_ALT}, {49,CATALOG_NO_ALT}, {137,CATALOG_NO_ALT}, {147,CATALOG_NO_ALT}, {146,CATALOG_NO_ALT}, {143,CATALOG_NO_ALT},
	{144,CATALOG_NO_ALT}, {142,CATALOG_NO_ALT}, {139,CATALOG_NO_ALT}, {141,CATALOG_NO_ALT}, {136,CATALOG_NO_ALT}, {145,CATALOG_NO_ALT},
	{148,CATALOG_NO_ALT}, {149,CATALOG_NO_ALT}, {150,CATALOG_NO_ALT}, {140,CATALOG_NO_ALT}, {138,CATALOG_NO_ALT}, {2,CATALOG_NO_ALT},
	{1,CATALOG_NO_ALT}, {3,CATALOG_NO_ALT}, {4,CATALOG_NO_ALT}, {68,CATALOG_NO_ALT}, {67,CATALOG_NO_ALT}, {69,CATALOG_NO_ALT},
	{70,CATALOG_NO_ALT},
};

static const struct recipe_catalog_family catalog_families[RECIPE_CATALOG_FAMILIES] = {
	{"Planks", 0, 1}, {"Slabs", 1, 4}, {"Stairs", 5, 2}, {"Wool", 7, 17},
	{"Sandstone", 24, 1}, {"Bricks", 25, 1}, {"Snow", 26, 1}, {"Clay", 27, 1},
	{"Fences", 28, 1}, {"Doors", 29, 2}, {"Trapdoors", 31, 1}, {"Ladders", 32, 1},
	{"Workbench", 33, 1}, {"Furnace", 34, 1}, {"Chests", 35, 1}, {"Bed", 36, 1},
	{"Torches", 37, 1}, {"Glowstone", 38, 1}, {"Jack o'lantern", 39, 1},
	{"Bookshelf", 40, 1}, {"Painting", 41, 1}, {"Sign", 42, 1}, {"Jukebox", 43, 1},
	{"Pickaxes", 44, 5}, {"Shovels", 49, 5}, {"Axes", 54, 5}, {"Hoes", 59, 5},
	{"Shears", 64, 1}, {"Bucket", 65, 1}, {"Fishing rod", 66, 1},
	{"Flint and steel", 67, 1}, {"Compass", 68, 1}, {"Clock", 69, 1}, {"Map", 70, 1},
	{"Swords", 71, 5}, {"Bow", 76, 1}, {"Arrows", 77, 1}, {"Helmets", 78, 4},
	{"Chestplates", 82, 4}, {"Leggings", 86, 4}, {"Boots", 90, 4},
	{"Redstone torch", 94, 1}, {"Repeater", 95, 1}, {"Lever", 96, 1},
	{"Button", 97, 1}, {"Pressure plates", 98, 2}, {"Pistons", 100, 2},
	{"Dispenser", 102, 1}, {"Note block", 103, 1}, {"TNT", 104, 1},
	{"Rails", 105, 3}, {"Minecarts", 108, 3}, {"Boat", 111, 1},
	{"Bread", 112, 1}, {"Stew", 113, 1}, {"Cookies", 114, 1}, {"Cake", 115, 1},
	{"Golden apple", 116, 1}, {"Sugar", 117, 1}, {"Bowls", 118, 1},
	{"Sticks", 119, 1}, {"Paper", 120, 1}, {"Books", 121, 1}, {"Dyes", 122, 15},
	{"Resource blocks", 137, 4}, {"Unpacking", 141, 4},
};

static const struct recipe_catalog_category catalog_categories[RECIPE_CATALOG_CATEGORIES] = {
	{"Building", 0, 12}, {"Home", 12, 11}, {"Tools", 23, 11}, {"Combat", 34, 7},
	{"Redstone", 41, 9}, {"Travel", 50, 3}, {"Food", 53, 7}, {"Materials", 60, 6},
};

/* Deliberate availability policy (crafting-chests.md): the iron chest and all
 * four chainmail registrations stay out of the menu. Chainmail remains valid
 * inventory/chest content; this is not a claim about Beta mob drops. */
static const uint16_t catalog_excluded[] = {6, 104, 105, 106, 107};

unsigned recipe_catalog_category_count(void) {
	return RECIPE_CATALOG_CATEGORIES;
}

const char* recipe_catalog_category_name(unsigned category) {
	return category < RECIPE_CATALOG_CATEGORIES
		? catalog_categories[category].name : "";
}

unsigned recipe_catalog_family_count(unsigned category) {
	return category < RECIPE_CATALOG_CATEGORIES
		? catalog_categories[category].count : 0;
}

static const struct recipe_catalog_family* family_of(unsigned category,
	unsigned family) {
	if(category >= RECIPE_CATALOG_CATEGORIES
	   || family >= catalog_categories[category].count) return NULL;
	return catalog_families + catalog_categories[category].first + family;
}

const char* recipe_catalog_family_name(unsigned category, unsigned family) {
	const struct recipe_catalog_family* f = family_of(category, family);
	return f ? f->name : "";
}

unsigned recipe_catalog_variant_count(unsigned category, unsigned family) {
	const struct recipe_catalog_family* f = family_of(category, family);
	return f ? f->count : 0;
}

bool recipe_catalog_lookup(unsigned category, unsigned family, unsigned variant,
	struct recipe_catalog_entry* out) {
	const struct recipe_catalog_family* f = family_of(category, family);
	if(!f || variant >= f->count) return false;
	if(out) *out = catalog_entries[f->first + variant];
	return true;
}

bool recipe_catalog_excluded(uint16_t recipe) {
	for(size_t k = 0; k < sizeof(catalog_excluded) / sizeof(catalog_excluded[0]); k++)
		if(catalog_excluded[k] == recipe) return true;
	return false;
}

/* ------------------------------------------------- registry decode/names -- */

void recipe_browser_name(struct item_data item, bool exact, char* name, size_t capacity) {
	/* Beta ItemDye.dyeColorNames order (740c583). Wool metadata uses the
	 * reverse order. Names keep otherwise identical recipe variants distinct. */
	static const char* colors[] = {"Black", "Red", "Green", "Brown", "Blue",
		"Purple", "Cyan", "Light gray", "Gray", "Pink", "Lime", "Yellow",
		"Light blue", "Magenta", "Orange", "White"};
	struct item* def = item_get(&item);
	if(!exact) {
		snprintf(name, capacity, "%s", def ? def->name : "Unknown");
		return;
	}
	if(item.id == BLOCK_WOOL && item.durability < 16)
		snprintf(name, capacity, "%s wool", colors[15 - item.durability]);
	else if(item.id == ITEM_DYE && item.durability < 16) {
		const char* special = item.durability == 0 ? "Ink sac"
			: item.durability == 3 ? "Cocoa beans" : item.durability == 4 ? "Lapis lazuli"
			: item.durability == 15 ? "Bone meal" : NULL;
		if(special) snprintf(name, capacity, "%s", special);
		else snprintf(name, capacity, "%s dye", colors[item.durability]);
	} else if(item.id == ITEM_COAL && item.durability == 1)
		snprintf(name, capacity, "Charcoal");
	else snprintf(name, capacity, "%s", def ? def->name : "Unknown");
}

bool recipe_browser_entry_at(uint16_t index, struct recipe_browser_entry* e) {
	*e = (struct recipe_browser_entry){0};
	size_t shaped = array_recipe_size(recipes_crafting);
	size_t k_index = index;
	if(k_index < shaped) {
		const struct recipe* r = array_recipe_cget(recipes_crafting, k_index);
		e->result = r->result; e->width = (unsigned)r->width; e->height = (unsigned)r->height;
		for(size_t k = 0; k < r->width * r->height; k++) {
			size_t cell = k / r->width * 3 + k % r->width;
			e->cells[cell] = r->shape[k].item;
			e->exact[cell] = r->shape[k].match_durability;
		}
	} else {
		k_index -= shaped;
		if(k_index >= array_recipe_shapeless_size(recipes_crafting_shapeless)) return false;
		const struct recipe_shapeless* r
			= array_recipe_shapeless_cget(recipes_crafting_shapeless, k_index);
		e->result = r->result; e->shapeless = true;
		e->width = 3; e->height = (unsigned)((r->count + 2) / 3);
		for(size_t k = 0; k < r->count; k++) {
			e->cells[k] = r->ingredients[k].item;
			e->exact[k] = r->ingredients[k].match_durability;
		}
	}
	/* Registry ingredient counts describe one occupied grid cell, not a stack
	 * recipe. Normalize the preview to the one item consumed from each cell. */
	for(unsigned k = 0; k < 9; k++) if(e->cells[k].id) e->cells[k].count = 1;
	return true;
}

static unsigned clamp_family(const struct recipe_browser* b) {
	unsigned f = b->family[b->category < RECIPE_CATALOG_CATEGORIES ? b->category : 0];
	unsigned count = recipe_catalog_family_count(b->category);
	return count && f < count ? f : 0;
}

static unsigned clamp_variant(const struct recipe_browser* b, unsigned family) {
	unsigned flat = catalog_categories[b->category].first + family;
	unsigned v = b->variant[flat];
	unsigned count = catalog_families[flat].count;
	return v < count ? v : 0;
}

static bool selected_entry(const struct recipe_browser* b,
	struct recipe_catalog_entry* out) {
	if(b->category >= RECIPE_CATALOG_CATEGORIES) return false;
	unsigned family = clamp_family(b);
	return recipe_catalog_lookup(b->category, family, clamp_variant(b, family), out);
}

uint16_t recipe_browser_selected_index(const struct recipe_browser* b) {
	struct recipe_catalog_entry entry;
	/* Always the MAIN registration of a merged entry: it is the one Beta's
	 * CraftingManager resolves first, so it is the one the server can honour. */
	return selected_entry(b, &entry) ? entry.recipe : RECIPE_CRAFT_INDEX_NONE;
}

bool recipe_browser_selected(const struct recipe_browser* b, unsigned arrangement,
	struct recipe_browser_entry* out) {
	struct recipe_catalog_entry entry;
	if(!selected_entry(b, &entry)) return false;
	uint16_t index = entry.recipe;
	if(arrangement && entry.alternate != RECIPE_CATALOG_NO_ALT)
		index = entry.alternate;
	return recipe_browser_entry_at(index, out);
}

bool recipe_browser_fits(const struct recipe_browser_entry* e, unsigned grid) {
	if(grid != 2 && grid != 3) return false;
	if(!e->shapeless) return e->width <= grid && e->height <= grid;
	unsigned count = 0;
	for(unsigned k = 0; k < 9; k++) count += e->cells[k].id != 0;
	return count <= grid * grid;
}

size_t recipe_browser_stock(const struct recipe_browser_entry* e,
	const struct inventory* inv, size_t base, struct recipe_browser_stock stock[9]) {
	size_t count = 0;
	memset(stock, 0, sizeof(*stock) * 9);
	for(unsigned k = 0; k < 9; k++) {
		if(!e->cells[k].id) continue;
		size_t j = 0;
		while(j < count && !(stock[j].item.id == e->cells[k].id
			&& stock[j].exact == e->exact[k]
			&& (!e->exact[k] || stock[j].item.durability == e->cells[k].durability))) j++;
		if(j == count) {
			stock[count].item = e->cells[k];
			stock[count++].exact = e->exact[k];
		}
		stock[j].need++;
	}
	/* Reserve only what each exact requirement needs. Surplus remains available
	 * to wildcard rows. This is display accounting, never an inventory mutation. */
	if(inv && (base > inv->capacity || inv->capacity - base < 36)) inv = NULL;
	unsigned reserved[36] = {0};
	for(unsigned pass = 0; pass < 2; pass++) for(size_t j = 0; j < count; j++) {
		if(stock[j].exact != (pass == 0)) continue;
		unsigned remaining = stock[j].need;
		for(size_t k = 0; inv && k < 36; k++) {
			struct item_data item;
			if(!inventory_get_slot((struct inventory*)inv, base + k, &item)
			   || item.id != stock[j].item.id
			   || (stock[j].exact && item.durability != stock[j].item.durability)) continue;
			unsigned available = item.count > reserved[k] ? item.count - reserved[k] : 0;
			stock[j].have += available;
			unsigned take = available < remaining ? available : remaining;
			reserved[k] += take; remaining -= take;
		}
	}
	return count;
}

/* ------------------------------------------------------------- lifecycle -- */

void recipe_browser_open(struct recipe_browser* b, unsigned grid) {
	b->active = true;
 b->close_status[0]=0;
	b->availability_busy=false;
	b->availability_reason=RECIPE_CRAFT_BUSY;
	b->availability_index=RECIPE_CRAFT_INDEX_NONE;
	b->suspended = false;
	b->details = false;
	b->blocked = false;
	b->pending = false;
	b->close_after_craft = false;
	b->has_reason = false;
	b->detail_page = 0;
	b->detail_shape = 0;
	b->detail_timer = 0.0F;
	b->grid = (uint8_t)(grid == 2 ? 2 : 3);
	b->focus = (struct gutil_menu_focus){.initialized=true};
	b->footer_selected = 0; /* Craft is the ring's action. */
	b->footer_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	b->repeat = (struct gutil_menu_repeat){0};
	if(b->category >= RECIPE_CATALOG_CATEGORIES) b->category = 0;
	/* Read once per open, never per frame: the alternation is a presentation
	 * preference, and the config tree is a JSON walk. */
	b->reduced_motion = config_read_int(&gstate.config_user,
		"gui.reduced_motion", 0) != 0;
}

void recipe_browser_refresh_availability(struct recipe_browser* b,struct window_container* wc,bool session_busy) {
 b->availability_index=recipe_browser_selected_index(b);
 b->availability_reason=wc?inventory_crafting_one_batch_preflight(windowc_get_latest(wc),wc->type,b->availability_index):RECIPE_CRAFT_INVALID_REQUEST;
 b->availability_busy=session_busy||(wc&&(windowc_actions_pending(wc)||wc->quick_pending||wc->correction_pending||wc->transaction_reply_pending));
 if(b->availability_busy)b->availability_reason=RECIPE_CRAFT_BUSY;
}

void recipe_browser_craft_reply(struct recipe_browser* b,
	enum recipe_craft_reason reason) {
	if(!b) return;
	b->pending = false;
	b->reason = reason;
	/* A completed craft says nothing; only a rejection needs a line, and it is
	 * shown silently (sound-events.md: failed actions are silent). */
	b->has_reason = reason != RECIPE_CRAFT_OK;
}

/* ------------------------------------------------------------------ step -- */

/* The guide's prompts are the shared bottom hint row (controller UI
 * redesign 2026-10-31, part B): no glyph is drawn inside the panel. This is
 * the row's top in the 640x480 / 640x240 design box (geometry seam). */
static int footer_y(unsigned grid,int h) {
 (void)grid;
 return h>=480 ? 480-GUTIL_HINT_BOTTOM_FULL-GUTIL_HINT_STRIP_H : 240-GUTIL_HINT_BOTTOM_SPLIT-GUTIL_HINT_STRIP_H;
}
/* What a footer cell DOES, independent of where it sits in the row: the
 * pointer and the painter share the row, and the step code dispatches on
 * this, never on a cell's position. */
enum recipe_footer_action { RECIPE_FOOTER_CRAFT, RECIPE_FOOTER_DETAILS,
 RECIPE_FOOTER_MANUAL, RECIPE_FOOTER_CLOSE, RECIPE_FOOTER_PAGE, RECIPE_FOOTER_LEGEND };
/* An ACTION cell of the row (the one legend cell -- Category -- names the
 * L+R pair but is not a pointer target). `cell` indexes row->cells.
 * Sol review 10-31 item 2: "Inventory" (the page swap back) IS an action
 * cell: the callers' page_swap test reads it through
 * recipe_browser_row_page_swap before the step runs. */
struct recipe_footer_item { int cell; enum recipe_footer_action action; };
/* WIRING-PLAN B2: personal Crafting page Inventory | Category (L+R) |
 * Craft xN | Details | Close; the workbench guide Category | Craft xN |
 * Details | Manual Grid | Close (its quick = Manual Grid behaviour stays).
 * Details: Craft | (Manual Grid) | Back, plus Inventory on the page.
 * "Inventory" is the PAGE_SWAP binding and "Category" the PREVIOUS+PAGE
 * pair; an unbound role is left out by the row helper. Returns the number of
 * action cells. */
static size_t recipe_footer(const struct recipe_browser* b,
 const struct container_glyphs* glyphs, unsigned count,int x,int y,int width,int height,
 struct gutil_hint_row* row, struct recipe_footer_item out[4], char craft[32]) {
 if(count>1)snprintf(craft,32,"Craft x%u",count);else snprintf(craft,32,"Craft");
 struct gutil_hint_item items[6];enum recipe_footer_action acts[6];int n=0;
 bool page=b->grid==2;
 if(page){items[n]=(struct gutil_hint_item){.roles={IB_GUI_PAGE_SWAP},.label="Inventory",.priority=2};acts[n++]=RECIPE_FOOTER_PAGE;}
 if(!b->details){items[n]=(struct gutil_hint_item){.roles={IB_GUI_PREVIOUS,IB_GUI_PAGE},.role_count=2,.label="Category",.priority=1};acts[n++]=RECIPE_FOOTER_LEGEND;}
 items[n]=(struct gutil_hint_item){.roles={IB_GUI_CLICK},.label=craft,.priority=6};acts[n++]=RECIPE_FOOTER_CRAFT;
 if(!b->details){items[n]=(struct gutil_hint_item){.roles={IB_GUI_SPLIT},.label="Details",.priority=5};acts[n++]=RECIPE_FOOTER_DETAILS;}
 if(!page){items[n]=(struct gutil_hint_item){.roles={IB_GUI_QUICK_MOVE},.label="Manual Grid",.priority=3};acts[n++]=RECIPE_FOOTER_MANUAL;}
 items[n]=(struct gutil_hint_item){.roles={IB_INVENTORY},.label=b->details?"Back":"Close",.keep=true};acts[n++]=RECIPE_FOOTER_CLOSE;
 struct input_player_frame frame;container_compact_glyph_frame(glyphs,&frame);
 if(!gutil_hint_row_layout(&frame,x,y,width,height,height<480,GUTIL_HINT_ALIGN_LEFT,items,n,row))return 0;
 /* Action order: Craft first (footer_selected 0 is the default A target),
  * the Inventory page cell LAST although it leads the drawn row. */
 size_t m=0;
 for(int k=0;k<row->count&&m<4;k++)if(acts[row->cells[k].item]!=RECIPE_FOOTER_LEGEND&&acts[row->cells[k].item]!=RECIPE_FOOTER_PAGE)
  out[m++]=(struct recipe_footer_item){k,acts[row->cells[k].item]};
 for(int k=0;k<row->count&&m<4;k++)if(acts[row->cells[k].item]==RECIPE_FOOTER_PAGE)
  out[m++]=(struct recipe_footer_item){k,RECIPE_FOOTER_PAGE};
 return m;
}
/* Measurement seam for the tests: the DRAWN row (viewport-local), from the
 * same helper the painter and the pointer use. `page_x` is the Inventory
 * legend cell's x, or -1; xs/widths are the ACTION cells. */
size_t recipe_browser_footer_probe(const struct recipe_browser* b,
 const struct container_glyphs* glyphs,unsigned count,int* page_x,int* page_w,
 int xs[4],int widths[4]);
size_t recipe_browser_footer_probe(const struct recipe_browser* b,
 const struct container_glyphs* glyphs,unsigned count,int* page_x,int* page_w,
 int xs[4],int widths[4]) {
 struct gutil_hint_row row;struct recipe_footer_item items[4];char craft[32];
 size_t n=recipe_footer(b,glyphs,count,0,0,640,480,&row,items,craft);
 *page_x=-1;*page_w=0;
 for(int k=0;k<row.count;k++)if(!strcmp(row.cells[k].label,"Inventory")){*page_x=row.cells[k].rect.x;*page_w=row.cells[k].rect.width;}
 for(size_t k=0;k<n;k++){xs[k]=row.cells[items[k].cell].rect.x;widths[k]=row.cells[items[k].cell].rect.width;}
 return n;
}
static int recipe_footer_hit(const struct gutil_hint_row* row,const struct recipe_footer_item* items,size_t n,float x,float y) {
 int cell=-1;
 for(int k=0;k<row->count;k++)if(gutil_button_hit(&row->cells[k].rect,x,y))cell=k;
 for(size_t k=0;k<n;k++)if(items[k].cell==cell)return (int)k;
 return -1;
}
bool recipe_browser_row_page_swap(const struct recipe_browser* b,
 const struct container_glyphs* glyphs,const struct container_input* in,int width,int height) {
 if(!b||!glyphs||!in||!b->active||b->grid!=2||b->suspended||in->suspended||!in->pick
  ||!in->pointer_valid||in->up||in->down||in->left||in->right)return false;
 if(in->pointer_x<0||in->pointer_x>=width||in->pointer_y<0||in->pointer_y>=height)return false;
 struct recipe_browser_entry entry={0};recipe_browser_selected(b,0,&entry);
 struct recipe_footer_item items[4];char craft[32];struct gutil_hint_row row;
 size_t n=recipe_footer(b,glyphs,entry.result.count,0,0,width,height,&row,items,craft);
 int hit=recipe_footer_hit(&row,items,n,in->pointer_x,in->pointer_y);
 return hit>=0&&items[hit].action==RECIPE_FOOTER_PAGE;
}


#define CATEGORY_TAB_X 80
#define CATEGORY_TAB_STEP 36
static const uint16_t category_icons[RECIPE_CATALOG_CATEGORIES]={
 BLOCK_BRICKS,ITEM_BED,ITEM_IRON_PICKAXE,ITEM_IRON_SWORD,
 ITEM_REDSTONE,ITEM_MINECART,ITEM_BREAD,ITEM_IRON};
void recipe_browser_geometry(int height,unsigned family,unsigned count,
 unsigned variant,unsigned variants,struct recipe_browser_geometry* g) {
 recipe_browser_geometry_grid(3,height,family,count,variant,variants,g);
}
void recipe_browser_geometry_grid(unsigned grid,int height,unsigned family,unsigned count,
 unsigned variant,unsigned variants,struct recipe_browser_geometry* g) {
 *g=(struct recipe_browser_geometry){0};
 bool full=height>=480,page=grid==2;
 g->panel_y=full ? (page?32:24) : (page?8:16);g->footer_y=footer_y(grid,height);
 /* 2026-10-31 part B: the prompts left the panel for the bottom hint row,
  * so the panel ends above it -- full screen also above the gameplay hearts
  * (y 363); the content's lowest row (inventory grid / costs) still fits. */
 g->panel_h=full ? 320 : page ? 184 : 176;
 /* Split Crafting page: the header row (y 20) pushes tabs/strip down by
  * 28 px and the name row up against the strip (approved v1-6). */
 bool split_page=page&&!full;
 g->category_y=g->panel_y+(split_page?34:8);
 g->family_y=g->panel_y+(split_page?78:58);
 g->name_y=g->panel_y+(split_page?126:116);
 /* T13 fix r1: the split page has 2 px between rows, so its chevrons hug
  * the strip; elsewhere the frozen approved offsets stay. */
 g->variant_up_y=g->family_y-(split_page?10:16);
 g->variant_down_y=g->family_y+38;
 g->band_top=g->family_y-(split_page?10:18);
 g->band_bottom=g->family_y+(split_page?46:54);
 g->visible=count<7 ? count : 7;
 if(!g->visible)return;
 g->first=family>g->visible/2 ? family-g->visible/2 : 0;
 if(g->first+g->visible>count)g->first=count-g->visible;
 char counter[24];snprintf(counter,sizeof counter,"%u/%u",variant+1,variants);
 int extra=variants>1 ? gutil_font_width(counter,16)+6 : 0;
 int total=(int)g->visible*56+((int)g->visible-1)*6+extra;
 int at=72+(496-total)/2;
 for(unsigned k=0;k<g->visible;k++) {
  g->tile_x[k]=at+10;g->cell_width[k]=56+(k+g->first==family ? extra : 0);
  at+=g->cell_width[k]+6;
 }
}
void recipe_browser_pattern_cells(const struct recipe_browser_entry* e,unsigned grid,
 bool blocked,struct item_data out[9]) {
 memset(out,0,sizeof(*out)*9);
 if(!e||blocked||(grid!=2&&grid!=3))return;
 if(grid==3){memcpy(out,e->cells,sizeof(*out)*9);return;}
 if(e->shapeless){unsigned at=0;for(unsigned k=0;k<9 && at<4;k++)if(e->cells[k].id)out[at++]=e->cells[k];}
 else {out[0]=e->cells[0];out[1]=e->cells[1];out[2]=e->cells[3];out[3]=e->cells[4];}
}

enum recipe_browser_action recipe_browser_step(struct recipe_browser* b,
 const struct container_input* in,int width,int height,float dt) {
 struct container_glyphs glyphs;container_compact_active_glyphs(&glyphs);
 return recipe_browser_step_owned(b,in,&glyphs,width,height,dt);
}
enum recipe_browser_action recipe_browser_step_owned(struct recipe_browser* b,
 const struct container_input* in,const struct container_glyphs* glyphs,int width,int height,float dt) {
	if(in->suspended) {
		b->suspended = true;
		b->focus.pointer_owner = false;
		b->repeat = (struct gutil_menu_repeat){0};
		return RECIPE_BROWSER_NONE;
	}
	/* Recovery consumes the first reconnected frame: a button still held from
	 * before the loss must not act. */
	if(b->suspended) { b->suspended = false; return RECIPE_BROWSER_NONE; }
	if(b->category >= RECIPE_CATALOG_CATEGORIES) b->category = 0;

	int direction = in->left ? -2 : in->right ? 2 : in->up ? -1 : in->down ? 1 : 0;
	bool pointer = in->pointer_valid && in->pointer_x >= 0 && in->pointer_x < width
		&& in->pointer_y >= 0 && in->pointer_y < height;
	bool owns = gutil_menu_pointer_focus(&b->focus, pointer,
		in->pointer_x, in->pointer_y, direction != 0);
	b->pointer_x = in->pointer_x;
	b->pointer_y = in->pointer_y;
	int px = (int)in->pointer_x - (width - 640) / 2;
	int py = (int)in->pointer_y - (height - (height >= 480 ? 480 : 240)) / 2;
	struct recipe_browser_entry footer_entry={0};recipe_browser_selected(b,0,&footer_entry);
	struct recipe_footer_item actions[4];char craft_label[32];struct gutil_hint_row footer_row;
	size_t action_count=recipe_footer(b,glyphs,footer_entry.result.count,0,0,width,height,&footer_row,actions,craft_label);
	int footer_hit=pointer?recipe_footer_hit(&footer_row,actions,action_count,in->pointer_x,in->pointer_y):-1;
	bool navigating=direction!=0||in->previous||in->page;
	if(navigating){b->footer_selected=0;b->footer_origin=GUTIL_MENU_ORIGIN_DIRECTIONAL;}
	/* Over blank the pointer keeps the last REAL target (Fable G4 review 1):
	 * pointing the Remote away then fires it under rule (3), as in
	 * screen_pause.c / container_menu.c. */
	else if(owns){if(footer_hit>=0)b->footer_selected=footer_hit;
		b->footer_origin=GUTIL_MENU_ORIGIN_POINTER;}
	struct gutil_menu_decision footer_decision=gutil_menu_activate_decide(
		(struct gutil_menu_activate_state){.pressed=in->pick,.navigated=navigating,
		 .pointer_valid=pointer,.selection_origin=b->footer_origin,
		 .hovered=footer_hit,.hovered_enabled=footer_hit>=0,
		 .selected=b->footer_selected,.selected_enabled=b->footer_selected>=0});
	b->footer_selected=footer_decision.selected;
	b->footer_origin=footer_decision.selection_origin;
	int footer_action=footer_decision.action==GUTIL_MENU_ACTIVATE
		&&footer_decision.target>=0&&(size_t)footer_decision.target<action_count?
		(int)actions[footer_decision.target].action:-1;

 if(b->close_after_craft && !b->pending){b->close_after_craft=false;return RECIPE_BROWSER_CLOSE;}
 /* The Inventory cell is the page swap, consumed by the caller's page_swap
  * test (recipe_browser_row_page_swap) before this step; here it is inert. */
 if(footer_action==RECIPE_FOOTER_PAGE)return RECIPE_BROWSER_NONE;
 if(b->pending) {
  if(in->close || footer_action==RECIPE_FOOTER_CLOSE)b->close_after_craft=true;
  b->repeat=(struct gutil_menu_repeat){0};
  return RECIPE_BROWSER_NONE;
 }

	if(b->details) {
		/* The merged stew entry keeps its main pattern still and alternates the
		 * two arrangements HERE, once a second. A reduced-motion preference
		 * pins it to the first arrangement. */
		struct recipe_catalog_entry entry;
		bool merged = selected_entry(b, &entry)
			&& entry.alternate != RECIPE_CATALOG_NO_ALT;
		if(merged && !b->reduced_motion) {
			b->detail_timer += dt;
			while(b->detail_timer >= 1.0F) {
				b->detail_timer -= 1.0F;
				b->detail_shape ^= 1;
			}
		} else {
			b->detail_timer = 0.0F;
			b->detail_shape = 0;
		}
		if(in->close || footer_action==RECIPE_FOOTER_CLOSE) {
			b->details=false;b->footer_selected=0;
			b->footer_origin=GUTIL_MENU_ORIGIN_DEFAULT;
			return RECIPE_BROWSER_NONE;
		}
		if(b->pending)return RECIPE_BROWSER_NONE;
		if(in->quick_move || footer_action==RECIPE_FOOTER_MANUAL)return RECIPE_BROWSER_MANUAL;
		if(footer_action==RECIPE_FOOTER_CRAFT)goto craft;
		return RECIPE_BROWSER_NONE;
	}

	if(in->close) return RECIPE_BROWSER_CLOSE;

	unsigned families = recipe_catalog_family_count(b->category);
	unsigned family = clamp_family(b);
	unsigned flat_family = catalog_categories[b->category].first + family;
	unsigned variants = catalog_families[flat_family].count;
	unsigned variant = clamp_variant(b, family);

	/* Minus/Plus or L/R: category. Left/right: family. Up/down: variant. */
	if(in->previous != in->page) {
		b->category=(uint8_t)((b->category+(in->previous?RECIPE_CATALOG_CATEGORIES-1:1))%RECIPE_CATALOG_CATEGORIES);
		b->has_reason = false;
		return RECIPE_BROWSER_NONE;
	}

	int step = gutil_menu_repeat_step(&b->repeat, direction, dt);
	if(step == -2 && family) family--;
	if(step == 2 && family + 1 < families) family++;
	if(step == -2 || step == 2) {
		b->family[b->category] = (uint8_t)family;
		flat_family = catalog_categories[b->category].first + family;
		variants = catalog_families[flat_family].count;
		variant = clamp_variant(b, family);
		b->has_reason = false;
	}
	if(step == -1 && variant) variant--;
	if(step == 1 && variant + 1 < variants) variant++;
	if(step == -1 || step == 1) b->has_reason = false;
	b->family[b->category] = (uint8_t)family;
	b->variant[flat_family] = (uint8_t)variant;
 if(step)return RECIPE_BROWSER_NONE;

 if(owns && pointer) {
  struct recipe_browser_geometry g;
  recipe_browser_geometry_grid(b->grid,height,family,families,variant,variants,&g);
  if(in->pick && py>=g.category_y && py<g.category_y+32) {
   if(px>=80 && px<368 && (px-80)%36<32)b->category=(uint8_t)((px-80)/36);
   else if(px>=44&&px<76)b->category=(uint8_t)((b->category+7)%8);
   else if(px>=372&&px<404)b->category=(uint8_t)((b->category+1)%8);
   b->has_reason=false;return RECIPE_BROWSER_NONE;
  }
  if(in->pick && py>=g.band_top&&py<g.band_bottom) {
   if(px>=44&&px<72){if(family){b->family[b->category]--;b->has_reason=false;}return RECIPE_BROWSER_NONE;}
   if(px>=568&&px<596){if(family+1<families){b->family[b->category]++;b->has_reason=false;}return RECIPE_BROWSER_NONE;}
  }
  for(unsigned k=0;k<g.visible;k++)if(px>=g.tile_x[k]&&px<g.tile_x[k]+36&&py>=g.family_y&&py<g.family_y+36){
   if(b->family[b->category]!=g.first+k){b->family[b->category]=(uint8_t)(g.first+k);b->has_reason=false;}return RECIPE_BROWSER_NONE;
  }
  int tx=g.tile_x[family-g.first];
  if(in->pick&&variants>1&&px>=tx&&px<tx+36) {
   if(py>=g.band_top&&py<g.family_y){if(variant){b->variant[flat_family]--;b->has_reason=false;}return RECIPE_BROWSER_NONE;}
   if(py>=g.family_y+36&&py<g.band_bottom){if(variant+1<variants){b->variant[flat_family]++;b->has_reason=false;}return RECIPE_BROWSER_NONE;}
  }
  if(in->pick&&footer_action<0)return RECIPE_BROWSER_NONE;
 }
	if(footer_action==RECIPE_FOOTER_CRAFT)goto craft;
	if(footer_action==RECIPE_FOOTER_DETAILS){b->details=true;b->footer_selected=0;
		b->footer_origin=GUTIL_MENU_ORIGIN_DEFAULT;return RECIPE_BROWSER_NONE;}
	if(footer_action==RECIPE_FOOTER_MANUAL)return RECIPE_BROWSER_MANUAL;
	if(footer_action==RECIPE_FOOTER_CLOSE)return RECIPE_BROWSER_CLOSE;

	if(in->split) { b->details = true; b->detail_page = 0;
		b->footer_selected=0;b->footer_origin=GUTIL_MENU_ORIGIN_DEFAULT;
		return RECIPE_BROWSER_NONE; }
	if(in->quick_move) return RECIPE_BROWSER_MANUAL;
	return RECIPE_BROWSER_NONE;
craft:;
	/* A crafts one batch -- never the Manual Grid. A recipe the station cannot
	 * hold is browsable but not executable ("Needs crafting table"). */
	struct recipe_browser_entry e;
	if(b->has_reason || b->availability_busy || b->availability_index!=recipe_browser_selected_index(b) || b->availability_reason!=RECIPE_CRAFT_OK || !recipe_browser_selected(b, 0, &e) || !recipe_browser_fits(&e, b->grid))
		return RECIPE_BROWSER_NONE;
	return RECIPE_BROWSER_CRAFT;
}

/* ---------------------------------------------------------------- render -- */
static void rect(int x,int y,int w,int h,uint8_t gray,uint8_t alpha) {
 gfx_texture(false);gutil_texquad_col(x,y,0,0,0,0,w,h,gray,gray,gray,alpha);gfx_texture(true);
}
static void text_fit(int x,int y,const char* text,int width) {
 char copy[128];snprintf(copy,sizeof copy,"%s",text);
 if(gutil_font_width(copy,16)>width){size_t n=strlen(copy);int dots=gutil_font_width("...",16);
  while(n&&gutil_font_width(copy,16)>width-dots)copy[--n]='\0';
  if(n+3<sizeof copy)memcpy(copy+n,"...",4);
 }
 gutil_text_rgb(x,y,copy,16,false,64,64,64);
}
void recipe_ui_panel(int x,int y,int w,int h) {
 gfx_bind_texture(&texture_gui_crafting);
 for(int dy=6;dy<h-6;dy+=32)for(int dx=6;dx<w-6;dx+=32){
  int dw=w-6-dx<32?w-6-dx:32,dh=h-6-dy<32?h-6-dy:32;
  gutil_texquad(x+dx,y+dy,150,62,(dw+1)/2,(dh+1)/2,dw,dh);
 }
 gutil_texquad(x,y,0,0,3,3,6,6);
 gutil_texquad(x+6,y,3,0,170,3,w-12,6);
 gutil_texquad(x+w-6,y,173,0,3,3,6,6);
 gutil_texquad(x,y+6,0,3,3,160,6,h-12);
 gutil_texquad(x+w-6,y+6,173,3,3,160,6,h-12);
 gutil_texquad(x,y+h-6,0,163,3,3,6,6);
 gutil_texquad(x+6,y+h-6,3,163,170,3,w-12,6);
 gutil_texquad(x+w-6,y+h-6,173,163,3,3,6,6);
}
/* Scaled recipe illustrations omit stack/durability overlays; the adjacent
 * ledger gives exact counts. Normal inventory slots retain gutil_draw_item. */
static void illustration(struct item_data item,int x,int y,int size) {
 struct item* it=item_get(&item);if(!it||!item.id)return;
 mat4 model;glm_translate_make(model,(vec3){x,y,0});
 float scale=(float)size/(16*GFX_GUI_SCALE);glm_scale(model,(vec3){scale,scale,1});
 gfx_depth_range(.1F,.2F);gfx_write_buffers(true,true,true);
 it->renderItem(it,&item,model,true,R_ITEM_ENV_INVENTORY);
 gfx_write_buffers(true,false,false);gfx_depth_range(0,1);
}
static void slot(struct item_data item,int x,int y,bool output) {
 int size=output?52:36;gfx_bind_texture(&texture_gui_crafting);
 gutil_texquad(x,y,output?119:29,output?30:16,output?26:18,output?26:18,size,size);
 if(item.id)gutil_draw_item(&item,x+(size-32)/2,y+(size-32)/2,1);
}
void recipe_ui_chevron(int x,int y,int dx,int dy,bool enabled) {
 for(int k=-3;k<=3;k++){int a=k<0?-k:k;
  int px=dx?x+(dx>0?2*(3-a):2*a):x+6+k*2;
  int py=dy?y+(dy>0?2*(3-a):2*a):y+6+k*2;
  rect(px,py,2,2,enabled?64:160,255);
 }
}
static void pattern(const struct recipe_browser_entry* e,unsigned grid,int x,int y) {
 bool blocked=!recipe_browser_fits(e,grid);struct item_data cells[9];
 recipe_browser_pattern_cells(e,grid,blocked,cells);
 int pad=grid==2?18:0;
 for(unsigned k=0;k<grid*grid;k++)slot(cells[k],x+pad+(int)(k%grid)*36,y+pad+(int)(k/grid)*36,false);
 gfx_bind_texture(&texture_gui_crafting);gutil_texquad(x+112,y+39,90,35,22,15,44,30);
 slot(blocked?(struct item_data){0}:e->result,x+164,y+28,true);
}
static void costs(const struct recipe_browser_stock* stock,size_t n,int x,int y,bool full) {
 gutil_text_rgb(x,full?y:y+8,"Have/Need",16,false,64,64,64);
 int at=full?x:x+gutil_font_width("Have/Need",16)+12,row=y+18;
 for(size_t k=0;k<n;k++) {
  bool missing=stock[k].have<stock[k].need;
  char text[24];snprintf(text,sizeof text,"%u/%u%s",stock[k].have,stock[k].need,missing?"!":"");
  int width=21+gutil_font_width(text,16);
  if(full&&at!=x&&at+width>x+216){at=x;row+=18;}
  int yy=full?row:y+7;
  illustration(stock[k].item,at,yy,18);
  gutil_text_rgb(at+21,yy+1,text,16,false,missing?170:64,missing?0:64,missing?0:64);
  at+=width+10;
 }
}
static const char* reason_text(enum recipe_craft_reason reason) {
	switch(reason) {
		case RECIPE_CRAFT_NO_CAPACITY: return "No room for the result";
		case RECIPE_CRAFT_MISSING_INGREDIENTS: return "Missing materials";
		case RECIPE_CRAFT_RECIPE_TOO_LARGE: return "Needs crafting table";
		case RECIPE_CRAFT_CREATE_FAILED: return "Could not create item";
		case RECIPE_CRAFT_NOT_ONE_BATCH: return "Craft refused";
		case RECIPE_CRAFT_BUSY: return "Clear the grid first";
		case RECIPE_CRAFT_TIMED_OUT:
		case RECIPE_CRAFT_STALE_SESSION: return "No reply; try again";
		default: return "Craft refused";
	}
}


static void text_right(int right,int y,const char* text,bool missing) {
 gutil_text_rgb(right-gutil_font_width(text,16),y,text,16,false,missing?170:64,missing?0:64,missing?0:64);
}
static bool required_item(struct item_data item,const struct recipe_browser_stock* stock,size_t n) {
 for(size_t k=0;k<n;k++)if(item.id==stock[k].item.id&&(!stock[k].exact||item.durability==stock[k].item.durability))return true;
 return false;
}

void recipe_browser_render(const struct recipe_browser* b,const struct inventory* inv,size_t base,
 int x,int y,int width,int height,const struct container_glyphs* glyphs) {
 if(width<640||height<240||b->category>=RECIPE_CATALOG_CATEGORIES)return;
 bool full=height>=480;int bx=x+(width-640)/2,by=y+(height-(full?480:240))/2;
 unsigned families=recipe_catalog_family_count(b->category),family=clamp_family(b);
 unsigned flat=catalog_categories[b->category].first+family;
 unsigned variants=catalog_families[flat].count,variant=clamp_variant(b,family);
 struct recipe_browser_geometry g;recipe_browser_geometry_grid(b->grid,height,family,families,variant,variants,&g);
 int py=by+g.panel_y;
 rect(x,y,width,height,0,full?150:180);recipe_ui_panel(bx+32,py,576,g.panel_h);
 /* T13: the personal guide is the inventory's Crafting PAGE. */
 int header_w=0;
 if(b->grid==2) {
  if(full)header_w=container_page_header(bx+320-container_page_header_width(true)/2,by+10,true,true);
  else header_w=container_page_header(bx+44,by+20,true,false);
 }
 struct recipe_browser_entry e={0};bool valid=recipe_browser_selected(b,b->details?b->detail_shape:0,&e);
 struct recipe_browser_stock stock[9];size_t n=valid?recipe_browser_stock(&e,inv,base,stock):0;
 enum recipe_craft_reason available=b->availability_busy?RECIPE_CRAFT_BUSY:recipe_browser_selected_index(b)==b->availability_index?b->availability_reason:inventory_crafting_one_batch_preflight(inv,b->grid==2?WINDOW_TYPE_INVENTORY:WINDOW_TYPE_WORKBENCH,recipe_browser_selected_index(b));
 if(b->details&&valid) {
  /* Split Crafting page: the header owns row 20, so the name follows it. */
  int name_x=b->grid==2&&!full?bx+44+header_w+12:bx+44;
  char name[80];recipe_browser_name(e.result,true,name,sizeof name);text_fit(name_x,py+12,name,full?552:bx+480-name_x);
  pattern(&e,b->grid,bx+44,py+(full?98:34));
  int item_h=full?24:20,head=full?18:0,body_h=full?240:112;
  int my=py+32+(body_h-(head+(int)n*item_h))/2;
  if(full){text_fit(bx+272,my,"Ingredients",180);text_right(bx+538,my,"Have",false);text_right(bx+596,my,"Need",false);my+=18;}
  else {text_right(bx+538,py+12,"Have",false);text_right(bx+596,py+12,"Need",false);}
  for(size_t k=0;k<n;k++) {
   int yy=my+(int)k*item_h,ty=yy+(item_h-16)/2;
   illustration(stock[k].item,bx+272,yy,item_h);
   recipe_browser_name(stock[k].item,stock[k].exact,name,sizeof name);text_fit(bx+302,ty,name,178);
   bool missing=stock[k].have<stock[k].need;
   char q[24];snprintf(q,sizeof q,"%u%s",stock[k].have,missing?"!":"");text_right(bx+538,ty,q,missing);
   snprintf(q,sizeof q,"%u",stock[k].need);text_right(bx+596,ty,q,false);
  }
  struct recipe_catalog_entry selected;bool merged=selected_entry(b,&selected)&&selected.alternate!=RECIPE_CATALOG_NO_ALT;
  const char* status=b->pending?(b->close_after_craft?"Closing after craft...":"Crafting..."):
   !recipe_browser_fits(&e,b->grid)?"Needs crafting table":b->has_reason?reason_text(b->reason):
   available!=RECIPE_CRAFT_OK?reason_text(available):merged?"Either order":e.result.id==ITEM_CAKE?"Returns 3 empty buckets":NULL;
  if(b->close_status[0])status=b->close_status;
  if(status)text_fit(bx+44,py+(full?284:148),status,552);

 } else {
  int cy=by+g.category_y;
  /* Maintainer 2026-10-31 (Q2): plain arrows on the old -/+ pointer
   * targets; the Category L+R prompt is in the bottom row. */
  {struct gutil_button_bounds a={bx+44,cy,32,32};container_menu_arrow(&a,"<");}
  for(unsigned k=0;k<RECIPE_CATALOG_CATEGORIES;k++) {
   int tx=bx+80+(int)k*36,ty=cy+(k==b->category?0:4),th=k==b->category?32:28;
   rect(tx,ty,32,th,k==b->category?198:139,255);
   rect(tx,ty,32,2,255,255);rect(tx,ty,2,th,255,255);
   rect(tx+30,ty+2,2,th-2,112,255);
   if(k!=b->category)rect(tx+2,ty+th-2,30,2,112,255);
   illustration((struct item_data){.id=category_icons[k],.count=1},tx+3,cy+3,26);

  }
  {struct gutil_button_bounds a={bx+372,cy,32,32};container_menu_arrow(&a,">");}
  char category[64];snprintf(category,sizeof category,b->grid==2?"%s 2x2":"%s",recipe_catalog_category_name(b->category));
  text_fit(bx+408,cy+8,category,188);
  int fy=by+g.family_y;
  recipe_ui_chevron(bx+49,fy+11,-1,0,family>0);recipe_ui_chevron(bx+573,fy+11,1,0,family+1<families);
  for(unsigned k=0;k<g.visible;k++) {
   unsigned f=g.first+k;struct recipe_catalog_entry ce;struct recipe_browser_entry row={0};
   bool ok=recipe_catalog_lookup(b->category,f,f==family?variant:0,&ce)&&recipe_browser_entry_at(ce.recipe,&row);
   int tx=bx+g.tile_x[k];slot(row.result,tx,fy,false);
   if(f==family){struct gutil_button_bounds q={tx,fy,36,36};gutil_menu_focus_border(&q);
    if(variants>1){char counter[24];snprintf(counter,sizeof counter,"%u/%u",variant+1,variants);
     text_fit(tx+46,fy+8,counter,g.cell_width[k]-56);
     recipe_ui_chevron(tx+11,by+g.variant_up_y,0,-1,variant>0);recipe_ui_chevron(tx+11,by+g.variant_down_y,0,1,variant+1<variants);
    }
   }
   if(ok&&inventory_crafting_recipe_eligibility(inv,b->grid==2?WINDOW_TYPE_INVENTORY:WINDOW_TYPE_WORKBENCH,ce.recipe)!=RECIPE_CRAFT_OK){
    rect(tx+28,fy+28,10,12,124,255);gutil_text_rgb(tx+29,fy+27,"!",12,false,255,255,255);
   }
  }
  if(valid) {
   char name[80],label[96];recipe_browser_name(e.result,true,name,sizeof name);
   if(e.result.count>1)snprintf(label,sizeof label,"%s x%u",name,e.result.count);else snprintf(label,sizeof label,"%s",name);
   text_fit(bx+44,by+g.name_y,b->close_status[0]?b->close_status:label,b->close_status[0]?552:300);
   const char* notice=b->pending?(b->close_after_craft?"Closing after craft...":"Crafting..."):!recipe_browser_fits(&e,b->grid)?"Needs crafting table":b->has_reason?reason_text(b->reason):available!=RECIPE_CRAFT_OK?reason_text(available):NULL;
   if(notice&&!b->close_status[0])text_fit(bx+356,by+g.name_y,notice,240);
   if(full){
    pattern(&e,b->grid,bx+44,py+140);costs(stock,n,bx+44,py+248,true);
    text_fit(bx+272,py+138,"Inventory",324);
    for(unsigned k=0;k<36;k++){struct item_data item={0};if(inv)inventory_get_slot((struct inventory*)inv,base+k,&item);
     int sx=bx+272+(int)(k%9)*36,sy=py+158+(int)(k/9)*36+(k>=27?6:0);
     slot(item,sx,sy,false);
     if(required_item(item,stock,n))rect(sx+3,sy+3,30,30,255,18);
    }
   }else costs(stock,n,bx+44,py+136,false);
  }

 }
 struct recipe_footer_item actions[4];char craft[32];struct gutil_hint_row footer_row;
 size_t count=recipe_footer(b,glyphs,valid?e.result.count:0,x,y,width,height,&footer_row,actions,craft);
 gutil_hint_row_draw(&footer_row);
 /* Craft unavailable: its cell is dimmed (the reason is the status line). */
 for(size_t k=0;k<count;k++)if(actions[k].action==RECIPE_FOOTER_CRAFT&&(b->pending||b->has_reason||available!=RECIPE_CRAFT_OK)){
  const struct gutil_button_bounds* r=&footer_row.cells[actions[k].cell].rect;rect(r->x,r->y,r->width,r->height,0,120);}
 if(b->footer_origin!=GUTIL_MENU_ORIGIN_DEFAULT&&b->footer_selected>=0
    &&(size_t)b->footer_selected<count){
  struct gutil_button_bounds q=footer_row.cells[actions[b->footer_selected].cell].rect;gutil_menu_focus_border(&q);}
 if(full)hud_menu_draw_owned(&gstate.players[0],width,height); /* B3 (10-31) */
 if(b->blocked)text_fit(bx+44,by+g.name_y,"Clear grid and carried stack for recipes",552);
 if(b->suspended)text_fit(bx+44,by+g.name_y,"Reconnect controller",552);
 if(b->focus.pointer_owner&&b->focus.pointer_valid){gfx_bind_texture(&texture_pointer);
  gutil_texquad_rt_any(x+b->pointer_x,y+b->pointer_y,0,0,0,256,256,48,48);
 }
}
