/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
/* Native recipe guide (Console Workbench / Pocket Ledger).
 *
 * Presentation and navigation only: no recipe is executed here. Crafting goes
 * through the server-authoritative one-batch transaction (recipe_craft.h,
 * network/inventory_crafting.h); this module only names WHICH registration the
 * player selected and reports the reply reason back to the player.
 *
 * Design authority: docs/ui_design/approved/specs/crafting-chests.md and
 * specs/menus.md, with docs/ui_design/approved/menus.html as the visual
 * authority. The catalog below is the approved one: 145 reachable entries
 * representing 146 registrations (the two mushroom-stew arrangements share one
 * entry), across eight categories and 66 families, with the iron chest and the
 * four chainmail registrations deliberately excluded. */
#ifndef RECIPE_BROWSER_H
#define RECIPE_BROWSER_H

#include "container_menu.h"
#include "../../item/item_types.h"
#include "../../network/recipe_craft.h"

struct inventory;
struct container_glyphs;

/* Approved catalog shape. These are hard counts, not upper bounds: the catalog
 * table is generated from the approved manifest for one exact recipe.c
 * (sha256 62171042...c9046b15) and tests/test_recipe_ui.c re-derives all three
 * from the live registry, so a registry change fails loudly instead of
 * silently dropping an entry out of reach. */
#define RECIPE_CATALOG_CATEGORIES 8
#define RECIPE_CATALOG_FAMILIES 66
#define RECIPE_CATALOG_ENTRIES 145
/* Registrations this build must hold for the catalog above to be complete. */
#define RECIPE_CATALOG_REGISTRATIONS 151
/* Entries covered by the catalog, i.e. registrations minus the exclusions. */
#define RECIPE_CATALOG_REPRESENTED 146
#define RECIPE_CATALOG_NO_ALT RECIPE_CRAFT_INDEX_NONE

/* One reachable menu entry. `recipe` is the flat registry index in the
 * recipe_craft.h numbering -- exactly what the craft request carries.
 * `alternate` is the SECOND registration merged into this one entry (only the
 * stew pair uses it) or RECIPE_CATALOG_NO_ALT. The merged entry keeps its main
 * pattern still; Details alternates the two arrangements. */
struct recipe_catalog_entry {
	uint16_t recipe, alternate;
};
struct recipe_catalog_family {
	const char* name;
	uint16_t first, count;
};
struct recipe_catalog_category {
	const char* name;
	uint16_t first, count;
};

unsigned recipe_catalog_category_count(void);
const char* recipe_catalog_category_name(unsigned category);
unsigned recipe_catalog_family_count(unsigned category);
const char* recipe_catalog_family_name(unsigned category, unsigned family);
unsigned recipe_catalog_variant_count(unsigned category, unsigned family);
bool recipe_catalog_lookup(unsigned category, unsigned family, unsigned variant,
	struct recipe_catalog_entry* out);
/* True for a registration the approved availability policy keeps out of the
 * menu (iron chest, the four chainmail pieces). Never a claim about Beta. */
bool recipe_catalog_excluded(uint16_t recipe);

struct recipe_browser_entry {
	struct item_data result, cells[9];
	bool exact[9], shapeless;
	unsigned width, height;
};

struct recipe_browser_stock {
	struct item_data item;
	bool exact;
	unsigned need, have;
};

/* Resolved approved layout shared by drawing and pointer navigation. */
struct recipe_browser_geometry {
 int panel_y,footer_y,family_y,category_y;
 int panel_h,name_y; /* T13: panel height and the selected-name row */
 /* T13 fix r1: top y of the variant up/down chevrons (8 px tall) and the
  * family-row pointer band [band_top, band_bottom) they and the strip share.
  * Every row -- header, tabs, up chevron, strip, down chevron, name -- is
  * disjoint at every size, so each pointer target has exactly one owner. */
 int variant_up_y,variant_down_y,band_top,band_bottom;
 unsigned first,visible;
 int tile_x[7],cell_width[7];
};
/* Workbench (3x3) geometry; the frozen approved layout. */
void recipe_browser_geometry(int height,unsigned family,unsigned count,
 unsigned variant,unsigned variants,struct recipe_browser_geometry* out);
/* ui-build-1026/T13: the personal (2x2) guide is the Crafting PAGE of the
 * inventory, so it makes room for the "Inventory | Crafting" page header
 * (design-round-1026e v1-2 / v1-6): full panel 32..384 with the header above
 * it at y 10, footer 344; split panel 8..224 with the header inside at 44,20
 * (to y 40), tabs 42..74, up chevron 76..84, strip 86..122, down chevron
 * 124..132, name 134..150, Have/Need 152, footer 184. grid 3 =
 * recipe_browser_geometry. */
void recipe_browser_geometry_grid(unsigned grid,int height,unsigned family,unsigned count,
 unsigned variant,unsigned variants,struct recipe_browser_geometry* out);
void recipe_browser_pattern_cells(const struct recipe_browser_entry* entry,unsigned grid,
 bool blocked,struct item_data out[9]);

/* One instance per player; selection never depends on ingredient availability.
 * Every field is a fixed-size struct member: the guide allocates nothing, per
 * frame or otherwise. */
struct recipe_browser {
 char close_status[64]; /* Owner host: recoverable held-stack close status. */
	struct gutil_menu_focus focus;
	int footer_selected;
	int footer_origin; /* enum gutil_menu_origin */
	struct gutil_menu_repeat repeat;
	/* Remembered selection: the family per category, the variant per family. */
	uint8_t category;
	uint8_t family[RECIPE_CATALOG_CATEGORIES];
	uint8_t variant[RECIPE_CATALOG_FAMILIES];
	uint8_t grid;			/* 2 = Pocket Ledger / personal, 3 = workbench */
	uint8_t detail_page;
	uint8_t detail_shape;	/* which merged arrangement Details is showing */
	float detail_timer;
	bool active, details, suspended, blocked;
	bool reduced_motion;	/* honours gui.reduced_motion: no stew alternation */
	/* One craft request in flight for THIS window. Input that could change the
	 * selection or start a second craft is locked while it is set. */
	bool pending;
	bool close_after_craft;
	uint32_t pending_serial;
	enum recipe_craft_reason reason;
	bool has_reason;
	bool availability_busy;
	enum recipe_craft_reason availability_reason;
	uint16_t availability_index;
	float pointer_x, pointer_y;
};

/* Decode one registration (flat recipe_craft.h index) into a preview. */
bool recipe_browser_entry_at(uint16_t recipe, struct recipe_browser_entry* out);
/* The selected entry's preview. `arrangement` picks between a merged entry's
 * two registrations (0 = main, 1 = alternate); a plain entry ignores it. */
bool recipe_browser_selected(const struct recipe_browser* browser,
	unsigned arrangement, struct recipe_browser_entry* out);
/* The flat index the craft request must carry for the current selection. */
uint16_t recipe_browser_selected_index(const struct recipe_browser* browser);

void recipe_browser_name(struct item_data item, bool exact, char* name, size_t capacity);
bool recipe_browser_fits(const struct recipe_browser_entry* entry, unsigned grid);
/* Only ordinary inventory/hotbar slots count. Exact metadata is reserved before
 * wildcards, so a single stack cannot satisfy two incompatible requirements.
 * Read-only accounting of the AUTHORITATIVE inventory -- never a prediction,
 * so Have/Need is correct again the moment the server's slot updates land. */
size_t recipe_browser_stock(const struct recipe_browser_entry* entry,
	const struct inventory* inventory, size_t main_base,
	struct recipe_browser_stock stock[9]);
/* `grid` is 3 for the Console Workbench and 2 for the personal Pocket Ledger.
 * `compact` selects the 640x240 split composition. */
void recipe_browser_open(struct recipe_browser* browser, unsigned grid);
void recipe_browser_refresh_availability(struct recipe_browser*,struct window_container*, bool session_busy);

enum recipe_browser_action {
	RECIPE_BROWSER_NONE,
	/* A: craft exactly one batch of the selected entry through the
	 * server-authoritative transaction. Never emitted while pending, and never
	 * for an entry that does not fit this station's grid. */
	RECIPE_BROWSER_CRAFT,
	RECIPE_BROWSER_MANUAL,
	RECIPE_BROWSER_CLOSE,
};
/* No RPC and no slot write happen here; the caller owns both. */
/* Explicit owner glyphs keep pointer footer hit widths aligned with rendering. */
enum recipe_browser_action recipe_browser_step_owned(struct recipe_browser*,
 const struct container_input*,const struct container_glyphs*,int width,int height,float dt);
enum recipe_browser_action recipe_browser_step(struct recipe_browser* browser,
	const struct container_input* input, int width, int height, float dt);
/* Does `input`'s pointer press hit the personal Crafting page's "Inventory"
 * row cell (the page swap back)? Pure; the caller ORs it into its page_swap
 * test, which runs before the step (Sol review 10-31 item 2). */
bool recipe_browser_row_page_swap(const struct recipe_browser*,
	const struct container_glyphs*, const struct container_input*, int width, int height);
/* Install the terminal reply for the in-flight craft. Clears pending; keeps the
 * reason for the silent rejection line. */
void recipe_browser_craft_reply(struct recipe_browser* browser,
	enum recipe_craft_reason reason);
void recipe_browser_render(const struct recipe_browser* browser,
	const struct inventory* inventory, size_t main_base,
	int x, int y, int width, int height, const struct container_glyphs* glyphs);

void recipe_ui_chevron(int x,int y,int dx,int dy,bool enabled);
void recipe_ui_panel(int x,int y,int width,int height);

#endif
