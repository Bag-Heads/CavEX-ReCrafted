/*
	Lane 4 -- crafting + inventory screens.

	Six named tests, one per requirement of the approved design
	(docs/ui_design/approved/specs/crafting-chests.md + specs/menus.md):

	  test_recipe_catalog_145    the approved catalog, walked end to end
	  test_recipe_bindings       A / Details / Manual Grid / Back / categories
	  test_console_workbench     the single-player full composition
	  test_personal_2x2_ledger   Inventory -> Recipes, personal 2x2 limits
	  test_recipe_craft_flow     request -> pending -> reply, and retirement
	  test_recipe_have_need      authoritative Have/Need + the stew alternation

	No recipe, container rule or Beta value is changed here: the guide only
	NAMES a registration and the server-authoritative transaction executes it.
*/
#include <assert.h>
#include "graphics/gui_util.h"
#include <stdio.h>
#include <string.h>

#undef GLFW_DLL
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "block/blocks.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "game/gui/container_compact.h"
#include "game/gui/container_menu.h"
#include "game/gui/recipe_browser.h"
#include "game/gui/screen.h"
#include "item/inventory.h"
#include "item/items.h"
#include "item/recipe.h"
#include "network/client_interface.h"
#include "network/inventory_crafting.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "graphics/gfx_settings.h"
#include "platform/gfx.h"
#include "platform/input.h"
#include "platform/time.h"
#include "platform/menu_sound.h"
#include "game/gui/sign_osk_model.h"
#include "game/gui/menu_header.h"

/* ------------------------------------------------------------- fixtures -- */

extern GLFWwindow* window;
static bool keys[GLFW_KEY_LAST + 1];
static double px = -1, py = -1;
static GLFWcharfun sign_char_callback;static GLFWkeyfun sign_key_callback;
static unsigned sign_select_cues,sign_back_cues;static bool observe_sign_cues;
GLFWcharfun glfwSetCharCallback(GLFWwindow* w,GLFWcharfun cb){(void)w;GLFWcharfun old=sign_char_callback;sign_char_callback=cb;return old;}
GLFWkeyfun glfwSetKeyCallback(GLFWwindow* w,GLFWkeyfun cb){(void)w;GLFWkeyfun old=sign_key_callback;sign_key_callback=cb;return old;}
void __wrap_audio_menu_event(enum menu_sound_event cue,ptime_t now){(void)now;if(observe_sign_cues){sign_select_cues+=cue==MENU_SOUND_SELECT;sign_back_cues+=cue==MENU_SOUND_BACK;}}


int glfwGetKey(GLFWwindow* w, int key) {
	(void)w;
	assert(key >= 0 && key <= GLFW_KEY_LAST);
	return keys[key] ? GLFW_PRESS : GLFW_RELEASE;
}
int glfwGetMouseButton(GLFWwindow* w, int button) {
	(void)w; (void)button;
	return GLFW_RELEASE;
}
void glfwSetInputMode(GLFWwindow* w, int mode, int value) {
	(void)w; (void)mode; (void)value;
}
void glfwSetCursorPos(GLFWwindow* w, double x, double y) {
	(void)w; px = x; py = y;
}
void glfwGetCursorPos(GLFWwindow* w, double* x, double* y) {
	(void)w; *x = px; *y = py;
}
#ifdef _WIN32
int (*__imp_glfwGetKey)(GLFWwindow*, int) = glfwGetKey;
int (*__imp_glfwGetMouseButton)(GLFWwindow*, int) = glfwGetMouseButton;
void (*__imp_glfwSetInputMode)(GLFWwindow*, int, int) = glfwSetInputMode;
void (*__imp_glfwSetCursorPos)(GLFWwindow*, double, double) = glfwSetCursorPos;
void (*__imp_glfwGetCursorPos)(GLFWwindow*, double*, double*) = glfwGetCursorPos;
GLFWcharfun (*__imp_glfwSetCharCallback)(GLFWwindow*,GLFWcharfun)=glfwSetCharCallback;
GLFWkeyfun (*__imp_glfwSetKeyCallback)(GLFWwindow*,GLFWkeyfun)=glfwSetKeyCallback;
#endif

struct capture {
	unsigned clicks, closes, drops, crafts, signs;
 uint16_t sign_actions[64];
	struct server_rpc last;
};
static void receive(struct server_rpc* call, void* user) {
	struct capture* out = user;
	out->last = *call;
	out->clicks += call->type == SRPC_WINDOW_CLICK;
	out->closes += call->type == SRPC_WINDOW_CLOSE;
	out->drops += call->type == SRPC_WINDOW_DROP;
	out->crafts += call->type == SRPC_RECIPE_CRAFT;
 if(call->type==SRPC_SIGN_SET){assert(out->signs<64);out->sign_actions[out->signs++]=call->payload.sign_set.action_id;}
}
static struct capture drain(void) {
	struct capture out = {0};
	svin_process_messages(receive, &out, false);
	return out;
}
static void frame(struct screen* screen, const char* pressed, float dt) {
	memset(keys, 0, sizeof(keys));
	for(const char* p = pressed; *p; p++) keys[(unsigned char)*p] = true;
	input_poll();
	screen->update(screen, dt);
}
static void press(struct screen* screen, const char* pressed) {
	frame(screen, "", 0.016F);
	frame(screen, pressed, 0.016F);
}

/* The three registry keys the guide needs, discovered rather than hard-coded. */
static uint16_t recipe_making(uint16_t id) {
	for(size_t n = 0; n < array_recipe_size(recipes_crafting); n++)
		if(array_recipe_cget(recipes_crafting, n)->result.id == id) return (uint16_t)n;
	assert(!"recipe missing");
	return RECIPE_CRAFT_INDEX_NONE;
}

/* Put one of everything a recipe needs into the player section. */
static void stock_for(struct inventory* inv, size_t base,
	const struct recipe_browser_entry* e) {
	struct recipe_browser_stock stock[9];
	size_t count = recipe_browser_stock(e, NULL, 0, stock);
	assert(count <= 9);
	for(size_t k = 0; k < count; k++) {
		struct item_data item = stock[k].item;
		item.count = (uint8_t)stock[k].need;
		inv->items[base + k] = item;
	}
}

/* ------------------------------------------------- 1. the approved catalog -- */

/* (result id, metadata, count) of the registration each of the 145 approved
 * catalog entries names, taken verbatim from the approved manifest
 * (docs/ui_design/approved/menus.html #recipe-catalog) and re-ordered to
 * follow the shipping catalog table. If a flat index ever drifts from the
 * registration it is supposed to name, this table -- not a screenshot -- is
 * what catches it. */
static const struct catalog_expect { uint16_t id, meta, count; } catalog_expect[145] = {
	{5,0,4}, {44,0,3}, {44,1,3}, {44,2,3}, {44,3,3}, {53,0,4},
	{67,0,4}, {35,0,1}, {35,0,1}, {35,1,1}, {35,2,1}, {35,3,1},
	{35,4,1}, {35,5,1}, {35,6,1}, {35,7,1}, {35,8,1}, {35,9,1},
	{35,10,1}, {35,11,1}, {35,12,1}, {35,13,1}, {35,14,1}, {35,15,1},
	{24,0,1}, {45,0,1}, {80,0,1}, {82,0,1}, {85,0,2}, {324,0,1},
	{330,0,1}, {96,0,2}, {65,0,2}, {58,0,1}, {61,0,1}, {54,0,1},
	{355,0,1}, {50,0,4}, {89,0,1}, {91,0,1}, {47,0,1}, {321,0,1},
	{323,0,1}, {84,0,1}, {270,0,1}, {274,0,1}, {257,0,1}, {278,0,1},
	{285,0,1}, {269,0,1}, {273,0,1}, {256,0,1}, {277,0,1}, {284,0,1},
	{271,0,1}, {275,0,1}, {258,0,1}, {279,0,1}, {286,0,1}, {290,0,1},
	{291,0,1}, {292,0,1}, {293,0,1}, {294,0,1}, {359,0,1}, {325,0,1},
	{346,0,1}, {259,0,1}, {345,0,1}, {347,0,1}, {358,0,1}, {268,0,1},
	{272,0,1}, {267,0,1}, {276,0,1}, {283,0,1}, {261,0,1}, {262,0,4},
	{298,0,1}, {306,0,1}, {310,0,1}, {314,0,1}, {299,0,1}, {307,0,1},
	{311,0,1}, {315,0,1}, {300,0,1}, {308,0,1}, {312,0,1}, {316,0,1},
	{301,0,1}, {309,0,1}, {313,0,1}, {317,0,1}, {76,0,1}, {356,0,1},
	{69,0,1}, {77,0,1}, {72,0,1}, {70,0,1}, {33,0,1}, {29,0,1},
	{23,0,1}, {25,0,1}, {46,0,1}, {66,0,16}, {27,0,6}, {28,0,6},
	{328,0,1}, {342,0,1}, {343,0,1}, {333,0,1}, {297,0,1}, {282,0,1},
	{357,0,8}, {354,0,1}, {322,0,1}, {353,0,1}, {281,0,4}, {280,0,4},
	{339,0,3}, {340,0,1}, {351,1,2}, {351,5,2}, {351,6,2}, {351,7,2},
	{351,7,3}, {351,8,2}, {351,9,2}, {351,10,2}, {351,11,2}, {351,12,2},
	{351,13,2}, {351,13,3}, {351,13,4}, {351,14,2}, {351,15,3}, {42,0,1},
	{41,0,1}, {57,0,1}, {22,0,1}, {265,0,9}, {266,0,9}, {264,0,9},
	{351,4,9},
};

static void test_recipe_catalog_145(void) {
	/* The catalog is built for THIS registry. Prove the shape first, so a
	 * registry change fails loudly instead of dropping an entry out of reach. */
	size_t shaped = array_recipe_size(recipes_crafting);
	size_t shapeless = array_recipe_shapeless_size(recipes_crafting_shapeless);
	assert(shaped == 120 && shapeless == 31);
	assert(shaped + shapeless == RECIPE_CATALOG_REGISTRATIONS);
	assert(inventory_crafting_recipe_count() == RECIPE_CATALOG_REGISTRATIONS);
	assert(recipe_catalog_category_count() == RECIPE_CATALOG_CATEGORIES);

	unsigned families = 0, entries = 0;
	for(unsigned c = 0; c < RECIPE_CATALOG_CATEGORIES; c++) {
		unsigned fc = recipe_catalog_family_count(c);
		assert(fc > 0 && recipe_catalog_category_name(c)[0]);
		families += fc;
		for(unsigned f = 0; f < fc; f++) {
			unsigned vc = recipe_catalog_variant_count(c, f);
			assert(vc > 0 && recipe_catalog_family_name(c, f)[0]);
			entries += vc;
		}
	}
	assert(families == RECIPE_CATALOG_FAMILIES);
	assert(entries == RECIPE_CATALOG_ENTRIES);

	/* Walk every entry through NAVIGATION -- category, then family, then
	 * variant -- exactly as a player reaches it, and account for every
	 * registration in the build. */
	unsigned seen[RECIPE_CATALOG_REGISTRATIONS];
	memset(seen, 0, sizeof(seen));
	unsigned reached = 0, merged = 0;
	struct recipe_browser browser = {0};
	recipe_browser_open(&browser, 3);
	for(unsigned c = 0; c < RECIPE_CATALOG_CATEGORIES; c++) {
		browser.category = (uint8_t)c;
		for(unsigned f = 0; f < recipe_catalog_family_count(c); f++) {
			browser.family[c] = (uint8_t)f;
			for(unsigned v = 0; v < recipe_catalog_variant_count(c, f); v++) {
				struct recipe_catalog_entry entry;
				assert(recipe_catalog_lookup(c, f, v, &entry));
				assert(entry.recipe < RECIPE_CATALOG_REGISTRATIONS);
				assert(!recipe_catalog_excluded(entry.recipe));
				assert(!seen[entry.recipe]++);
				browser.variant[0] = 0; /* selection is per family, set below */
				reached++;
				if(entry.alternate != RECIPE_CATALOG_NO_ALT) {
					merged++;
					assert(entry.alternate < RECIPE_CATALOG_REGISTRATIONS);
					assert(!seen[entry.alternate]++);
					/* Both arrangements really are the same dish. */
					struct recipe_browser_entry a, b;
					assert(recipe_browser_entry_at(entry.recipe, &a));
					assert(recipe_browser_entry_at(entry.alternate, &b));
					assert(a.result.id == ITEM_MUSHROOM_STEW);
					assert(b.result.id == a.result.id && b.result.count == a.result.count);
					assert(memcmp(a.cells, b.cells, sizeof(a.cells)) != 0);
				}
				/* The entry names the registration the approved catalog says. */
				struct recipe_browser_entry e;
				assert(recipe_browser_entry_at(entry.recipe, &e));
				const struct catalog_expect* want = catalog_expect + (reached - 1);
				assert(e.result.id == want->id);
				assert(e.result.durability == want->meta);
				assert(e.result.count == want->count);
				struct recipe_browser_stock presentation[9];
				assert(recipe_browser_stock(&e,NULL,0,presentation)<=4);
				/* Every entry fits the workbench it is offered from. */
				assert(recipe_browser_fits(&e, 3));
			}
		}
	}
	assert(reached == RECIPE_CATALOG_ENTRIES);
	assert(merged == 1); /* only the two stew arrangements share an entry */
	assert(reached + merged == RECIPE_CATALOG_REPRESENTED);

	/* Everything not represented is deliberately excluded, and it is exactly
	 * the iron chest plus the four chainmail pieces -- nothing else fell out. */
	const uint16_t chain[] = {ITEM_CHAIN_HELMET, ITEM_CHAIN_CHESTPLATE,
		ITEM_CHAIN_LEGGINGS, ITEM_CHAIN_BOOTS};
	unsigned excluded = 0, chain_seen = 0, iron_chest_seen = 0;
	for(uint16_t k = 0; k < RECIPE_CATALOG_REGISTRATIONS; k++) {
		if(seen[k]) { assert(!recipe_catalog_excluded(k)); continue; }
		assert(recipe_catalog_excluded(k));
		excluded++;
		struct recipe_browser_entry e;
		assert(recipe_browser_entry_at(k, &e));
		if(e.result.id == BLOCK_IRON_CHEST) iron_chest_seen++;
		for(unsigned j = 0; j < 4; j++) chain_seen += e.result.id == chain[j];
	}
	assert(excluded == 5 && iron_chest_seen == 1 && chain_seen == 4);

	/* Reachable is not enough: every entry must be CRAFTABLE through the
	 * authoritative transaction. An index shadowed by an earlier registration
	 * would answer UNKNOWN_RECIPE, and the menu entry would be a dead end. */
	for(unsigned c = 0; c < RECIPE_CATALOG_CATEGORIES; c++)
		for(unsigned f = 0; f < recipe_catalog_family_count(c); f++)
			for(unsigned v = 0; v < recipe_catalog_variant_count(c, f); v++) {
				struct recipe_catalog_entry entry;
				struct recipe_browser_entry e;
				assert(recipe_catalog_lookup(c, f, v, &entry));
				assert(recipe_browser_entry_at(entry.recipe, &e));
				struct inventory inv;
				assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
				stock_for(&inv, CRAFTING_SLOT_MAIN, &e);
				enum recipe_craft_reason reason = inventory_crafting_one_batch(&inv,
					WINDOW_TYPE_WORKBENCH, entry.recipe, NULL, NULL);
				assert(reason == RECIPE_CRAFT_OK);
				inventory_destroy(&inv, NULL);
			}

	puts("PASS test_recipe_catalog_145: 145 reachable entries over 146 "
		 "registrations (stew merged), 66 families in 8 categories, iron chest "
		 "and four chainmail excluded, every entry crafts through the "
		 "authoritative transaction");
}

/* --------------------------------------------------------- 2. bindings --- */

static void test_recipe_presentation_geometry(void) {
 struct recipe_browser_geometry full,compact,plain;
 recipe_browser_geometry(480,3,12,0,17,&full);
 recipe_browser_geometry(240,3,12,0,17,&compact);
 recipe_browser_geometry(480,3,12,0,1,&plain);
 assert(full.panel_y==24 && full.footer_y==428 && full.category_y==32 && full.family_y==82);
 assert(compact.panel_y==16 && compact.footer_y==196 && compact.category_y==24 && compact.family_y==74);
 assert(full.panel_y+full.panel_h<=full.footer_y && compact.panel_y+compact.panel_h<=compact.footer_y);
 assert(full.visible==7 && full.first==0);
 assert(full.cell_width[3]==56+gutil_font_width("1/17",16)+6 && plain.cell_width[3]==56);
 for(unsigned k=1;k<full.visible;k++)assert(full.tile_x[k]==full.tile_x[k-1]+full.cell_width[k-1]+6);
 struct recipe_browser_entry e={0};struct item_data out[9];
 e.cells[0].id=1;e.cells[1].id=2;e.cells[3].id=3;e.cells[4].id=4;
 recipe_browser_pattern_cells(&e,2,false,out);
 for(unsigned k=0;k<4;k++)assert(out[k].id==k+1);
 for(unsigned k=4;k<9;k++)assert(!out[k].id);
 e=(struct recipe_browser_entry){.shapeless=true};
 e.cells[0].id=1;e.cells[2].id=2;e.cells[5].id=3;e.cells[8].id=4;
 recipe_browser_pattern_cells(&e,2,false,out);
 for(unsigned k=0;k<4;k++)assert(out[k].id==k+1);
 recipe_browser_pattern_cells(&e,2,true,out);for(unsigned k=0;k<9;k++)assert(!out[k].id);
 struct recipe_browser b={0};recipe_browser_open(&b,3);
 struct recipe_browser_geometry hit;recipe_browser_geometry(480,0,recipe_catalog_family_count(0),0,1,&hit);
 struct container_input in={.pointer_valid=true,.pointer_x=hit.tile_x[3]+1,.pointer_y=hit.family_y+1,.pick=true};
 in.pick=false;in.pointer_x-=20;recipe_browser_step(&b,&in,640,480,.016F);
 in.pick=true;in.pointer_x+=20;
 assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
 assert(b.family[0]==3);
 recipe_browser_geometry(480,3,recipe_catalog_family_count(0),0,recipe_catalog_variant_count(0,3),&hit);
 in=(struct container_input){.pointer_valid=true,.pointer_x=hit.tile_x[3]+1,.pointer_y=hit.family_y+37,.pick=true};
 assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);assert(b.variant[3]==1);
 b=(struct recipe_browser){0};recipe_browser_open(&b,3);
 in=(struct container_input){.right=true};recipe_browser_step(&b,&in,640,480,.016F);
 in=(struct container_input){.pointer_valid=true,.pointer_x=600,.pointer_y=200};
 recipe_browser_step(&b,&in,640,480,.016F);in.pointer_x=620;in.pick=true;
 assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
 puts("PASS recipe presentation: complete four-cost catalog, shared ribbon hitboxes, personal 2x2, blank pointer inert");
}
size_t recipe_browser_footer_probe(const struct recipe_browser* b,
 const struct container_glyphs* glyphs,unsigned count,int* page_x,int* page_w,
 int xs[4],int widths[4]);
static void test_recipe_footer_last_input_wins(void) {
	struct recipe_browser b={0};recipe_browser_open(&b,3);
	b.availability_index=recipe_browser_selected_index(&b);
	b.availability_reason=RECIPE_CRAFT_OK;
	/* The bottom hint row's Close cell (action 3), from the drawn layout. */
	struct container_glyphs glyphs;container_compact_active_glyphs(&glyphs);
	int page_x,page_w,xs[4],widths[4];
	struct recipe_browser_entry first={0};recipe_browser_selected(&b,0,&first);
	assert(recipe_browser_footer_probe(&b,&glyphs,first.result.count,&page_x,&page_w,xs,widths)==4);
	float close_x=xs[3]+widths[3]/2.0F,close_y=480-GUTIL_HINT_BOTTOM_FULL-GUTIL_HINT_STRIP_H/2;
	struct container_input in={.pointer_valid=true,.pointer_x=close_x,.pointer_y=close_y};
	assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
	in.pick=true;
	assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
	assert(b.footer_selected==3); /* first mismatched press only moves focus */
	in.pick=false;
	in.down=true;
	assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
	in.down=false;in.pick=true;
	assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_CRAFT);
	in.pick=false;in.pointer_x=20;in.pointer_y=250;
	assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
	assert(b.footer_selected==0); /* blank keeps the last real target */
	in.pick=true;
	assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
	in.pick=false;in.pointer_x=close_x-4;in.pointer_y=close_y;
	assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
	in.pick=true;
	assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_CLOSE);
	puts("PASS recipe last input wins: ring Craft under resting Close; moved sprite Close");
}

/* Fable G4 review item 1: a sprite that owns focus over BLANK keeps the last
 * real footer target, so pointing the Remote away and pressing A crafts
 * (rule (3)). Before the fix the blank hover armed NONE and A did nothing. */
static void test_recipe_blank_hover_then_away_crafts(void) {
	struct recipe_browser b={0};recipe_browser_open(&b,3);
	b.availability_index=recipe_browser_selected_index(&b);
	b.availability_reason=RECIPE_CRAFT_OK;
	struct container_input in={.pointer_valid=true,.pointer_x=20,.pointer_y=250};
	assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
	in.pointer_x=40; /* purposeful motion over blank */
	assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
	assert(b.focus.pointer_owner);
	assert(b.footer_selected==0 && "a blank hover discarded the Craft target");
	in=(struct container_input){0}; /* Remote pointed away */
	assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
	in.pick=true;
	assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_CRAFT
		&& "A after a blank hover and pointer loss did not craft");
	puts("PASS recipe blank hover then pointer away: A crafts the kept footer target");
}

static void test_recipe_bindings(void) {
	/* The three controller families, through the real prompt/glyph resolver. */
	const char* families[] = {"wiimote", "classic", "gamecube"};
	const int expect[3][8] = {
		/* QUICK, CLICK, SPLIT, DROP, INVENTORY, CLICK_ALT, PREVIOUS, PAGE */
		{8, 0, 1, 3, 2, 1, 6, 5},
		{13, 10, 11, 6, 12, 11, 22, 23},
		{34, 32, 33, 36, 35, 33, 37, 38},
	};
	const enum input_button actions[] = {IB_GUI_QUICK_MOVE, IB_GUI_CLICK,
		IB_GUI_SPLIT, IB_GUI_DROP, IB_INVENTORY, IB_GUI_CLICK_ALT,
		IB_GUI_PREVIOUS, IB_GUI_PAGE};
	for(unsigned d = 0; d < 3; d++) {
		assert(config_set_string(&gstate.config_user, "debug.container_glyphs", families[d]));
		for(unsigned k = 0; k < 8; k++) {
			int symbol, glyph;
			enum input_category category;
			assert(input_symbol(actions[k], &symbol, &glyph, &category));
			assert(glyph == expect[d][k]);
		}
		for(int direction=IB_GUI_UP;direction<=IB_GUI_RIGHT;direction++){int symbol,glyph;enum input_category category;assert(input_symbol(direction,&symbol,&glyph,&category));assert(glyph==24);}
		/* Same abstract actions, same guide behaviour, on every family. */
		struct recipe_browser b = {0};
		recipe_browser_open(&b, 3);
		b.availability_index=recipe_browser_selected_index(&b);
		b.availability_reason=RECIPE_CRAFT_OK; /* input-only test: supplied preflight verdict */
		struct container_input in = {0};
		in.pick = true;
		assert(recipe_browser_step(&b, &in, 640, 480, 0.016F) == RECIPE_BROWSER_CRAFT);
		in = (struct container_input){.split = true};
		assert(recipe_browser_step(&b, &in, 640, 480, 0.016F) == RECIPE_BROWSER_NONE);
		assert(b.details);
		in = (struct container_input){.pick = true};
		b.availability_busy=true;
		assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
		b.availability_busy=false;
		b.availability_index=RECIPE_CRAFT_INDEX_NONE;
		assert(recipe_browser_step(&b,&in,640,480,.016F)==RECIPE_BROWSER_NONE);
		b.availability_index=recipe_browser_selected_index(&b);
		assert(recipe_browser_step(&b, &in, 640, 480, 0.016F) == RECIPE_BROWSER_CRAFT);
		in = (struct container_input){.quick_move = true};
		assert(recipe_browser_step(&b, &in, 640, 480, 0.016F) == RECIPE_BROWSER_MANUAL);
		in = (struct container_input){.close = true};
		assert(recipe_browser_step(&b, &in, 640, 480, 0.016F) == RECIPE_BROWSER_NONE);
		assert(!b.details); /* Back leaves Details first, then closes. */
		assert(recipe_browser_step(&b, &in, 640, 480, 0.016F) == RECIPE_BROWSER_CLOSE);
		in = (struct container_input){.quick_move = true};
		assert(recipe_browser_step(&b, &in, 640, 480, 0.016F) == RECIPE_BROWSER_MANUAL);
	}
	assert(config_set_string(&gstate.config_user, "debug.container_glyphs", "auto"));

	/* Categories on Minus/Plus (or L/R); families left/right; variants up/down.
	 * Wool is family 3 of Building with 17 variants -- a real counter. */
	struct recipe_browser b = {0};
	recipe_browser_open(&b, 3);
	struct container_input in = {.page = true};
	assert(recipe_browser_step(&b, &in, 640, 480, 0.016F) == RECIPE_BROWSER_NONE);
	assert(b.category == 1);
	in = (struct container_input){.previous = true};
	recipe_browser_step(&b, &in, 640, 480, 0.016F);
	assert(b.category == 0);
	/* Both at once is not a category change. */
	in = (struct container_input){.page = true, .previous = true};
	recipe_browser_step(&b, &in, 640, 480, 0.016F);
	assert(b.category == 0);

	for(unsigned k = 0; k < 3; k++) {
		in = (struct container_input){0};
		recipe_browser_step(&b, &in, 640, 480, 0.016F);
		in = (struct container_input){.right = true};
		recipe_browser_step(&b, &in, 640, 480, 0.016F);
	}
	assert(b.family[0] == 3);
	unsigned wool = recipe_catalog_variant_count(0, 3);
	assert(wool == 17);
	for(unsigned k = 0; k < 4; k++) {
		in = (struct container_input){0};
		recipe_browser_step(&b, &in, 640, 480, 0.016F);
		in = (struct container_input){.down = true};
		recipe_browser_step(&b, &in, 640, 480, 0.016F);
	}
	struct recipe_catalog_entry fourth;
	assert(recipe_catalog_lookup(0, 3, 4, &fourth));
	assert(recipe_browser_selected_index(&b) == fourth.recipe);
	/* Clamped at both ends: no wrap into another family or category. */
	for(unsigned k = 0; k < 40; k++) {
		in = (struct container_input){0};
		recipe_browser_step(&b, &in, 640, 480, 0.016F);
		in = (struct container_input){.up = true};
		recipe_browser_step(&b, &in, 640, 480, 0.016F);
	}
	struct recipe_catalog_entry first;
	assert(recipe_catalog_lookup(0, 3, 0, &first));
	assert(recipe_browser_selected_index(&b) == first.recipe);
	assert(b.family[0] == 3 && b.category == 0);

	puts("PASS test_recipe_bindings: A crafts (never Manual Grid), C/X Details, "
		 "Z/Y Manual Grid, B returns then closes, Minus/Plus categories, "
		 "left/right families, up/down variants -- all three controller families");
}

/* -------------------------------------------------- 3. the two stations --- */

static struct window_container* workbench;
static struct window_container* inventory_window;

static void clear_window(struct window_container* wc) {
	windowc_discard_pending_actions(wc);
	struct inventory* inv = windowc_get_latest(wc);
	for(size_t k = 0; k < wc->slot_count; k++) inv->items[k] = (struct item_data){0};
	inv->picked_item = (struct item_data){0};
}

static void test_console_workbench(void) {
	clear_window(workbench);
	px = py = -1;
	screen_set(&screen_crafting);
	struct recipe_browser* b = &gstate.players[0].recipes;
	/* Single player at 640x480: the guide IS the crafting presentation, and it
	 * is the workbench station (nine input cells). */
	assert(b->active && b->grid == 3 && !b->details);
	assert(gfx_width() == 640 && gfx_height() == 480);

	/* A crafts -- it must never be the Manual Grid entry. */
	struct inventory* inv = windowc_get_latest(workbench);
	struct recipe_browser_entry e;
	assert(recipe_browser_selected(b, 0, &e));
	stock_for(inv, CRAFTING_SLOT_MAIN, &e);
	press(&screen_crafting, " ");
	struct capture c = drain();
	assert(c.crafts == 1 && !c.clicks && !c.closes && !c.drops);
	assert(b->active && b->pending);
	assert(c.last.payload.recipe_craft.window == WINDOWC_CRAFTING);
	assert(c.last.payload.recipe_craft.recipe_index == recipe_browser_selected_index(b));

	/* Pending locks THIS window's guide input only. */
	uint8_t before = b->category;
	press(&screen_crafting, "F");
	assert(b->category == before && b->pending);
	recipe_browser_craft_reply(b, RECIPE_CRAFT_OK);
	gstate.players[0].craft_pending = false;
	press(&screen_crafting, "F");
	assert(b->category == before + 1);

	uint16_t saved_recipe=recipe_browser_selected_index(b);
	press(&screen_crafting,"T");
	assert(!b->active&&gstate.players[0].manual_grid.active&&gstate.players[0].manual_grid.grid==3);
	press(&screen_crafting,"E");c=drain();
	assert(!c.closes&&!c.clicks&&!c.drops&&!c.crafts);
	assert(b->active&&!gstate.players[0].manual_grid.active&&recipe_browser_selected_index(b)==saved_recipe);
	press(&screen_crafting,"E");c=drain();
	assert(c.closes==1&&!c.clicks&&!c.drops&&!c.crafts);
	/* A blocked safe close must explain the held-stack failure and leave
	 * Manual Grid available to make room, without submitting a close. */
	clear_window(workbench);
	screen_set(&screen_crafting);
	inv = windowc_get_latest(workbench);
	for(size_t k=CRAFTING_SLOT_MAIN;k<CRAFTING_SIZE;k++)
		inv->items[k]=(struct item_data){.id=1,.count=64};
	inv->picked_item=(struct item_data){.id=3,.count=3};
	press(&screen_crafting,"E");c=drain();
	assert(!c.closes && !c.clicks && b->active && b->close_status[0]);
	assert(inv->picked_item.id==3 && inv->picked_item.count==3);
	press(&screen_crafting,"T");c=drain();
	assert(gstate.players[0].manual_grid.active && !b->close_status[0] && !c.closes);
	puts("PASS test_console_workbench: full 640x480 guide is the workbench "
		 "station, A crafts one batch through the transaction, pending locks "
		 "the window, Manual Grid and Back use the existing owners");
}

static void test_personal_2x2_ledger(void) {
	clear_window(inventory_window);
	clear_window(workbench);
	px = py = -1;
	struct recipe_browser* b = &gstate.players[0].recipes;
	screen_set(&screen_inventory);
	assert(!b->active); /* the inventory is still the inventory */

	/* T12: Inventory -> Crafting is a PAGE_SWAP tap (PC default C), fired on
	 * release. Previous/Next stay plain section jumps on the Inventory page. */
	px = py = -1; /* screen_set re-centres the cursor; keep it off-panel */
	press(&screen_inventory, "F");
	struct capture sect = drain();
	assert(!sect.clicks && !b->active);
	press(&screen_inventory, "C");
	assert(!b->active);
	frame(&screen_inventory, "", 0.016F);
	assert(b->active && b->grid == 2);

	/* Personal station: a 3x3 recipe is browsable but not executable. */
	struct recipe_browser_entry e;
	unsigned found_large = 0;
	for(unsigned c = 0; c < RECIPE_CATALOG_CATEGORIES && !found_large; c++)
		for(unsigned f = 0; f < recipe_catalog_family_count(c); f++) {
			struct recipe_catalog_entry entry;
			assert(recipe_catalog_lookup(c, f, 0, &entry));
			assert(recipe_browser_entry_at(entry.recipe, &e));
			if(recipe_browser_fits(&e, 2)) continue;
			b->category = (uint8_t)c;
			b->family[c] = (uint8_t)f;
			found_large = 1;
			break;
		}
	assert(found_large);
	struct capture c2 = drain();
	press(&screen_inventory, " ");
	c2 = drain();
	assert(!c2.crafts && !c2.clicks); /* "Needs crafting table", not a craft */

	/* A crafting-table ITEM is correctly craftable in the personal grid. */
	uint16_t table = recipe_making(BLOCK_WORKBENCH);
	unsigned placed = 0;
	for(unsigned c = 0; c < RECIPE_CATALOG_CATEGORIES && !placed; c++)
		for(unsigned f = 0; f < recipe_catalog_family_count(c) && !placed; f++)
			for(unsigned v = 0; v < recipe_catalog_variant_count(c, f); v++) {
				struct recipe_catalog_entry entry;
				assert(recipe_catalog_lookup(c, f, v, &entry));
				if(entry.recipe != table) continue;
				assert(v == 0); /* the workbench family has one variant */
				b->category = (uint8_t)c;
				b->family[c] = (uint8_t)f;
				placed = 1;
				break;
			}
	assert(placed);
	assert(recipe_browser_selected(b, 0, &e) && recipe_browser_fits(&e, 2));
	stock_for(windowc_get_latest(inventory_window), INVENTORY_SLOT_MAIN, &e);
	press(&screen_inventory, " ");
	c2 = drain();
	assert(c2.crafts == 1);
	assert(c2.last.payload.recipe_craft.window == WINDOWC_INVENTORY);
	assert(c2.last.payload.recipe_craft.recipe_index == table);
	recipe_browser_craft_reply(b, RECIPE_CRAFT_OK);
	gstate.players[0].craft_pending = false;

	/* A second tap returns to Inventory; Close at the root of the Crafting
	 * page closes the window (T12: Back one level, close at root). */
	press(&screen_inventory, "C");
	frame(&screen_inventory, "", 0.016F);
	assert(!b->active);
	c2 = drain();
	assert(!c2.closes && !c2.clicks);
	press(&screen_inventory, "C");
	frame(&screen_inventory, "", 0.016F);
	assert(b->active && b->grid == 2);
	press(&screen_inventory, "E");
	c2 = drain();
	assert(c2.closes == 1);
	puts("PASS test_personal_2x2_ledger: a page tap opens the personal 2x2 "
		 "Crafting page, 3x3 recipes browse but cannot be executed, the crafting "
		 "table crafts from four planks, a tap returns and Close closes");
}

/* ------------------------------------------------------- 4. craft flow --- */

static void test_recipe_craft_flow(void) {
	clear_window(workbench);
	px = py = -1;
	screen_set(&screen_crafting);
	struct client_session* session = &gstate.players[0];
	struct recipe_browser* b = &session->recipes;
	assert(b->active && !b->pending);

	/* Missing stock is inert. A valid mirror enables the request, but a
	 * server-side race can still reject it authoritatively. */
	press(&screen_crafting," ");
	assert(!drain().crafts&&!b->pending);
	assert(b->availability_reason==RECIPE_CRAFT_MISSING_INGREDIENTS);
	struct recipe_browser_entry selected;assert(recipe_browser_selected(b,0,&selected));
	stock_for(windowc_get_latest(workbench),CRAFTING_SLOT_MAIN,&selected);
	press(&screen_crafting, " ");
	struct capture c = drain();
	assert(c.crafts == 1 && b->pending && session->craft_pending);
	uint8_t was = b->category;
	press(&screen_crafting, "F");
	assert(b->category == was); /* input locked for THIS window while pending */

	struct client_rpc reply = {.type = CRPC_RECIPE_CRAFT};
	reply.payload.recipe_craft.ticket = session->craft_pending_ticket;
	reply.payload.recipe_craft.player = 0;
	reply.payload.recipe_craft.window = WINDOWC_CRAFTING;
	reply.payload.recipe_craft.reason = RECIPE_CRAFT_MISSING_INGREDIENTS;
	assert(client_session_window_reply(session, &reply));
	assert(!session->craft_pending);
	press(&screen_crafting, "");
	assert(!b->pending && b->has_reason);
	assert(b->reason == RECIPE_CRAFT_MISSING_INGREDIENTS);
	press(&screen_crafting, "F");
	assert(b->category == was + 1 && !b->has_reason);

	/* A reply that does not echo all three ownership fields is inert, and the
	 * stale/expired pending is retired locally instead of hanging forever. */
	assert(recipe_browser_selected(b,0,&selected));
	stock_for(windowc_get_latest(workbench),CRAFTING_SLOT_MAIN,&selected);
	press(&screen_crafting, " ");
	c = drain();
	assert(c.crafts == 1 && b->pending);
	struct client_rpc wrong = reply;
	wrong.payload.recipe_craft.ticket = session->craft_pending_ticket;
	wrong.payload.recipe_craft.window = WINDOWC_INVENTORY;
	client_session_window_reply(session, &wrong);
	assert(session->craft_pending && b->pending);
	session->visible_window = -1; /* the window stopped being the visible one */
	client_session_recipe_craft_tick(session, time_get());
	assert(!session->craft_pending && session->craft_reason == RECIPE_CRAFT_TIMED_OUT);
	session->visible_window = WINDOWC_CRAFTING;
	press(&screen_crafting, "");
	assert(!b->pending && b->has_reason && b->reason == RECIPE_CRAFT_TIMED_OUT);

	/* A displayed rejection disables Craft until navigation clears it. */
	press(&screen_crafting, " ");
	c=drain();assert(c.crafts==0&&!b->pending);
	press(&screen_crafting, "F");
	press(&screen_crafting, "G");
	/* Back queues a close after the authoritative reply or timeout. */
	press(&screen_crafting, " ");
	drain();
	assert(b->pending);
	press(&screen_crafting, "E");
	c = drain();
	assert(c.closes == 0 && b->pending && b->close_after_craft);
	session->craft_pending = false;
	session->craft_reason = RECIPE_CRAFT_TIMED_OUT;
	press(&screen_crafting, "");
	c = drain();
	assert(c.closes == 1);
	puts("PASS test_recipe_craft_flow: A -> request -> pending (input locked for "
		 "that window) -> reply installs the reason silently; a foreign reply is "
		 "inert and an unanswerable request retires itself");
}

/* --------------------------------------------------- 5. Have / Need ------ */

static void test_recipe_have_need(void) {
	/* Preserve the draft guide's exact metadata labels during integration. */
	char name[64];
	recipe_browser_name((struct item_data){.id=BLOCK_WOOL,.durability=4},true,name,sizeof(name));
	assert(strcmp(name,"Yellow wool")==0);
	recipe_browser_name((struct item_data){.id=BLOCK_WOOL,.durability=4},false,name,sizeof(name));
	assert(strcmp(name,"Wool")==0);
	recipe_browser_name((struct item_data){.id=ITEM_DYE,.durability=15},true,name,sizeof(name));
	assert(strcmp(name,"Bone meal")==0);
	recipe_browser_name((struct item_data){.id=ITEM_COAL,.durability=1},true,name,sizeof(name));
	assert(strcmp(name,"Charcoal")==0);
	/* Have/Need is read from the AUTHORITATIVE inventory every time, so it is
	 * correct again as soon as the server's slot updates land -- there is no
	 * predicted count to drift. */
	uint16_t table = recipe_making(BLOCK_WORKBENCH);
	struct recipe_browser_entry e;
	assert(recipe_browser_entry_at(table, &e));
	struct recipe_browser_stock stock[9];

	struct inventory inv;
	assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
	assert(recipe_browser_stock(&e, &inv, CRAFTING_SLOT_MAIN, stock) == 1);
	assert(stock[0].need == 4 && stock[0].have == 0);

	inv.items[CRAFTING_SLOT_MAIN] = (struct item_data){.id = BLOCK_PLANKS, .count = 9};
	assert(recipe_browser_stock(&e, &inv, CRAFTING_SLOT_MAIN, stock) == 1);
	assert(stock[0].have == 9 && stock[0].need == 4);
	struct item_data before[CRAFTING_SIZE];
	memcpy(before, inv.items, sizeof(before));

	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH, table, NULL, NULL)
		== RECIPE_CRAFT_OK);
	assert(memcmp(before, inv.items, sizeof(before)) != 0);
	assert(recipe_browser_stock(&e, &inv, CRAFTING_SLOT_MAIN, stock) == 1);
	assert(stock[0].have == 5); /* re-read, not predicted */
	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH, table, NULL, NULL)
		== RECIPE_CRAFT_OK);
	assert(recipe_browser_stock(&e, &inv, CRAFTING_SLOT_MAIN, stock) == 1);
	assert(stock[0].have == 1);
	/* Accounting alone never touches the inventory. */
	memcpy(before, inv.items, sizeof(before));
	recipe_browser_stock(&e, &inv, CRAFTING_SLOT_MAIN, stock);
	assert(memcmp(before, inv.items, sizeof(before)) == 0);
	inventory_destroy(&inv, NULL);

	/* Exact metadata is reserved before wildcards, so one stack cannot satisfy
	 * two incompatible requirements at once. */
	struct inventory wool;
	assert(inventory_create(&wool, NULL, NULL, NULL, 46, 0, 0, 0));
	struct recipe_browser_entry mixed = {.width = 3, .height = 1};
	mixed.cells[0] = mixed.cells[1] = mixed.cells[2]
		= (struct item_data){.id = BLOCK_WOOL, .count = 1};
	mixed.exact[2] = true;
	mixed.cells[2].durability = 4;
	wool.items[10] = (struct item_data){.id = BLOCK_WOOL, .durability = 4, .count = 1};
	assert(recipe_browser_stock(&mixed, &wool, 10, stock) == 2);
	assert(stock[0].need == 2 && stock[0].have == 0);
	assert(stock[1].need == 1 && stock[1].have == 1);
	/* Another wool color can satisfy a wildcard without spending exact stock.
	 * Manual inputs and the cursor still do not count as available stock. */
	wool.items[45]=(struct item_data){.id=BLOCK_WOOL,.durability=2,.count=1};
	wool.items[1]=wool.picked_item=(struct item_data){.id=BLOCK_WOOL,.count=64};
	struct item_data wool_before[46];memcpy(wool_before,wool.items,sizeof(wool_before));
	assert(recipe_browser_stock(&mixed,&wool,10,stock)==2);
	assert(stock[0].need==2&&stock[0].have==1);
	assert(stock[1].need==1&&stock[1].have==1);
	assert(memcmp(wool_before,wool.items,sizeof(wool_before))==0&&wool.picked_item.count==64);
	assert(recipe_browser_stock(&mixed,&wool,SIZE_MAX,stock)==2);
	assert(stock[0].have==0&&stock[1].have==0);
	inventory_destroy(&wool, NULL);

	/* The merged stew entry: main pattern still, Details alternates the two
	 * arrangements, and a reduced-motion preference keeps it static. */
	struct recipe_browser b = {0};
	recipe_browser_open(&b, 3);
	unsigned cat = RECIPE_CATALOG_CATEGORIES, fam = 0;
	for(unsigned c = 0; c < RECIPE_CATALOG_CATEGORIES && cat == RECIPE_CATALOG_CATEGORIES; c++)
		for(unsigned f = 0; f < recipe_catalog_family_count(c); f++) {
			struct recipe_catalog_entry entry;
			if(recipe_catalog_lookup(c, f, 0, &entry)
			   && entry.alternate != RECIPE_CATALOG_NO_ALT) { cat = c; fam = f; break; }
		}
	assert(cat < RECIPE_CATALOG_CATEGORIES);
	b.category = (uint8_t)cat;
	b.family[cat] = (uint8_t)fam;
	struct recipe_browser_entry main_shape, alt_shape;
	assert(recipe_browser_selected(&b, 0, &main_shape));
	assert(recipe_browser_selected(&b, 1, &alt_shape));
	assert(main_shape.result.id == ITEM_MUSHROOM_STEW);
	assert(memcmp(main_shape.cells, alt_shape.cells, sizeof(main_shape.cells)) != 0);

	struct container_input in = {.split = true};
	recipe_browser_step(&b, &in, 640, 480, 0.016F);
	assert(b.details && b.detail_shape == 0);
	in = (struct container_input){0};
	for(unsigned k = 0; k < 70; k++) recipe_browser_step(&b, &in, 640, 480, 0.016F);
	assert(b.detail_shape == 1); /* it alternates while Details is open */
	b.reduced_motion = true;
	b.detail_shape = 0;
	for(unsigned k = 0; k < 200; k++) recipe_browser_step(&b, &in, 640, 480, 0.016F);
	assert(b.detail_shape == 0); /* reduced motion keeps it static */

	puts("PASS test_recipe_have_need: Have/Need is re-read from the "
		 "authoritative inventory after every craft, exact metadata reserved "
		 "before wildcards, stew Details alternates with a static "
		 "reduced-motion fallback");
}


static struct capture sign_drain_all(void){struct capture c={0};for(;;){svin_process_messages(receive,&c,false);if(!svin_pending_count())break;svin_rpc_flush_pending();}return c;}
static void sign_type(char ch){assert(sign_char_callback);sign_char_callback(window,(unsigned char)ch);frame(&screen_sign,"",.016F);}
static void sign_special(int key){assert(sign_key_callback);sign_key_callback(window,key,0,GLFW_PRESS,0);frame(&screen_sign,"",.016F);}
static void sign_ack(struct window_container* wc,const struct capture* c,bool accept){for(unsigned k=0;k<c->signs;k++)windowc_action_apply_result(wc,c->sign_actions[k],accept);}
static void sign_reset_cues(void){observe_sign_cues=true;sign_select_cues=sign_back_cues=0;}
static void test_sign_real_transaction(void){
 struct window_container wc;assert(windowc_create(&wc,WINDOW_TYPE_SIGN,SIGN_OSK_SIZE));
 for(size_t k=0;k<SIGN_OSK_SIZE;k++)inventory_set_slot(windowc_get_latest(&wc),k,(struct item_data){.id=1,.count=' '});
 gstate.players[0].windows[WINDOWC_SIGN]=&wc;gstate.players[0].visible_window=WINDOWC_SIGN;screen_sign_set_windowc(WINDOWC_SIGN);px=py=-1;
 screen_set(&screen_sign);
 for(int k=0;k<2;k++){struct menu_composition c;struct menu_rect face,keyboard;
 screen_sign_test_layout(640,k?240:480,&c,&face,&keyboard);
 assert(c.window.x==(k?32:44)&&c.window.y==(k?8:44)&&c.window.w==(k?576:552)&&c.window.h==(k?188:352));
 assert(face.x==(k?44:203)&&face.y==(k?76:112)&&face.w==(k?194:234)&&face.h==104);
 assert(keyboard.x==(k?266:44)&&keyboard.y==(k?47:232)&&keyboard.w==(k?330:552)&&keyboard.h==(k?133:156));
 assert(menu_composition_notice_conflict(&c)==c.action_count);}
 /* A queued keyboard B wins over the same physical binding, and holding the
  * key cannot delete it on the following frame. A fresh B edge backspaces. */
 sign_char_callback(window,'B');frame(&screen_sign,"B",.016F);
 struct capture c=sign_drain_all();assert(c.signs==1&&!c.closes);
 assert(c.last.payload.sign_set.ch=='B'&&!screen_sign_test_discard_open());
 frame(&screen_sign,"B",.016F);assert(!sign_drain_all().signs);
 sign_ack(&wc,&c,true);press(&screen_sign,"B");c=sign_drain_all();
 assert(c.signs==1&&!c.closes&&c.last.payload.sign_set.ch==' ');
 assert(!screen_sign_test_discard_open());sign_ack(&wc,&c,true);
 sign_type('A');c=sign_drain_all();assert(c.signs==1&&!c.closes);
 sign_reset_cues();sign_special(GLFW_KEY_ENTER);assert(!sign_drain_all().closes&&gstate.players[0].current_screen==&screen_sign&&!sign_select_cues);
 sign_ack(&wc,&c,true);frame(&screen_sign,"",.016F);assert(sign_drain_all().closes==1&&sign_select_cues==1&&gstate.players[0].current_screen==&screen_ingame);
 screen_set(&screen_sign);sign_type('B');c=sign_drain_all();assert(c.signs==1);sign_reset_cues();sign_special(GLFW_KEY_ENTER);
 sign_ack(&wc,&c,false);frame(&screen_sign,"",.016F);assert(!sign_drain_all().closes&&gstate.players[0].current_screen==&screen_sign&&!sign_select_cues);
 /* Settle correction journal before a separate close-retry case. */
 windowc_discard_pending_actions(&wc);wc.correction_pending=false;
 screen_set(&screen_sign);sign_type('C');c=sign_drain_all();sign_ack(&wc,&c,true);
 struct server_rpc filler={.type=SRPC_WINDOW_CLICK};for(int k=0;k<16;k++)assert(svin_rpc_send(&filler));
 for(int k=0;k<3;k++){struct server_rpc close={.type=SRPC_WINDOW_CLOSE,.payload.window_close.window=(uint8_t)k};assert(svin_rpc_send(&close));}
 sign_reset_cues();sign_special(GLFW_KEY_ENTER);frame(&screen_sign,"",.016F);assert(gstate.players[0].current_screen==&screen_sign&&!sign_select_cues);
 (void)sign_drain_all();frame(&screen_sign,"",.016F);assert(sign_drain_all().closes==1&&sign_select_cues==1);

 screen_set(&screen_sign);
 uint8_t opened[SIGN_OSK_SIZE];for(size_t k=0;k<SIGN_OSK_SIZE;k++)opened[k]=windowc_get_latest(&wc)->items[k].count;
 sign_type('X');sign_type('Y');c=sign_drain_all();assert(c.signs==2);sign_ack(&wc,&c,true);
 /* Re-present this same authoritative editor in compact and then full layout. */
 assert(local_menu_host_open(&gstate.players[0],0,WINDOWC_SIGN,640,240));
 assert(gstate.players[0].menu_host.mode==LOCAL_MENU_SIGN);
 assert(!memcmp(gstate.players[0].sign.opened_cells,opened,sizeof opened));
 local_menu_host_reset(&gstate.players[0]);screen_set(&screen_sign);
 assert(!memcmp(gstate.players[0].sign.opened_cells,opened,sizeof opened));
 sign_special(GLFW_KEY_ESCAPE);assert(screen_sign_test_discard_open());
 sign_special(GLFW_KEY_TAB); /* keyboard selection moves to Discard */
 for(int k=0;k<79;k++)assert(svin_rpc_send(&filler)); /* one action-reserve slot remains */
 sign_reset_cues();sign_special(GLFW_KEY_ENTER);frame(&screen_sign,"",.016F);
 struct capture first=sign_drain_all();assert(first.signs==1&&!first.closes&&!sign_select_cues&&!sign_back_cues);
 sign_ack(&wc,&first,true);frame(&screen_sign,"",.016F);struct capture second=sign_drain_all();assert(second.signs==1&&!second.closes);
 sign_ack(&wc,&second,true);frame(&screen_sign,"",.016F);assert(sign_drain_all().closes==1&&sign_back_cues==1&&!sign_select_cues);
 for(size_t k=0;k<SIGN_OSK_SIZE;k++)assert(windowc_get_latest(&wc)->items[k].count==opened[k]);
 /* A typed Space must not also click Q under a stationary pointer. */
 screen_set(&screen_sign);px=45;py=275;frame(&screen_sign,"",.016F);px=65;frame(&screen_sign,"",.016F);
 sign_char_callback(window,' ');frame(&screen_sign," ",.016F);c=sign_drain_all();
 assert(c.signs==0); /* trailing space matches the sign's blank cell, no extra Q */
 sign_special(GLFW_KEY_ESCAPE);frame(&screen_sign,"",.016F);(void)sign_drain_all();
 observe_sign_cues=false;gstate.players[0].windows[WINDOWC_SIGN]=NULL;windowc_destroy(&wc);
 puts("PASS shipping sign screen: approved full/compact layout, ACK-gated Done, rejection and close retry");
}

static bool host_capture(void* user,struct server_rpc* rpc) { receive(rpc,user);return true; }
static void test_two_owner_menu_host(void) {
 struct client_session peers[2]={0};struct window_container wc[2][2];
 struct container_glyphs glyphs[2];struct capture calls[2]={0};
 struct container_input idle={0},pick={.pick=true};
 for(unsigned k=0;k<2;k++) {
  assert(windowc_create(&wc[k][0],WINDOW_TYPE_INVENTORY,INVENTORY_SIZE));
  assert(windowc_create(&wc[k][1],WINDOW_TYPE_WORKBENCH,CRAFTING_SIZE));
  peers[k].windows[0]=&wc[k][0];peers[k].windows[1]=&wc[k][1];peers[k].visible_window=1;
  struct input_player_frame frame={.connected=true,.category=k?INPUT_CAT_GAMECUBE:INPUT_CAT_NUNCHUK};
  for(int b=0;b<IB_COUNT;b++)frame.glyphs[b]=-1;
  frame.glyphs[IB_GUI_CLICK]=k?32:0;frame.glyphs[IB_GUI_CLICK_ALT]=k?33:1;
  frame.glyphs[IB_GUI_QUICK_MOVE]=k?34:8;frame.glyphs[IB_GUI_SPLIT]=k?33:1;
  frame.glyphs[IB_INVENTORY]=k?35:2;
  container_compact_player_glyphs(&glyphs[k],&frame);
  assert(local_menu_host_open(&peers[k],k,1,640,240));
 }
 struct client_session untouched;memcpy(&untouched,&peers[1],sizeof untouched);
 struct container_input right={.right=true};
 local_menu_host_update(&peers[0],&right,&glyphs[0],.016F,host_capture,&calls[0]);
 assert(!memcmp(&untouched,&peers[1],sizeof untouched));
 assert(recipe_browser_selected_index(&peers[0].recipes)!=recipe_browser_selected_index(&peers[1].recipes));
 for(unsigned k=0;k<2;k++) {
  struct recipe_browser_entry entry;assert(recipe_browser_selected(&peers[k].recipes,0,&entry));
  stock_for(windowc_get_latest(&wc[k][1]),CRAFTING_SLOT_MAIN,&entry);
  local_menu_host_update(&peers[k],&pick,&glyphs[k],.016F,host_capture,&calls[k]);
  assert(calls[k].crafts==1&&calls[k].last.payload.recipe_craft.player==k&&peers[k].recipes.pending);
 }
 assert(peers[0].craft_pending_ticket!=peers[1].craft_pending_ticket);
 struct client_rpc reply={.type=CRPC_RECIPE_CRAFT,.payload.recipe_craft={.ticket=peers[0].craft_pending_ticket,.player=0,.window=1,.reason=RECIPE_CRAFT_OK}};
 client_session_window_reply(&peers[1],&reply);assert(peers[1].craft_pending);
 client_session_window_reply(&peers[0],&reply);
 local_menu_host_update(&peers[0],&idle,&glyphs[0],.016F,host_capture,&calls[0]);
 assert(!peers[0].recipes.pending&&peers[1].recipes.pending);
 memcpy(&untouched,&peers[1],sizeof untouched);
 uint16_t retained=recipe_browser_selected_index(&peers[0].recipes);
 struct container_input manual={.quick_move=true},next={.page=true},back={.close=true};
 local_menu_host_update(&peers[0],&manual,&glyphs[0],.016F,host_capture,&calls[0]);
 assert(peers[0].menu_host.mode==LOCAL_MENU_MANUAL&&peers[0].manual_grid.owner_index==0);
 local_menu_host_update(&peers[0],&next,&glyphs[0],.016F,host_capture,&calls[0]);assert(peers[0].manual_grid.page==1);
 local_menu_host_update(&peers[0],&back,&glyphs[0],.016F,host_capture,&calls[0]);
 assert(peers[0].menu_host.mode==LOCAL_MENU_RECIPES&&recipe_browser_selected_index(&peers[0].recipes)==retained);
 assert(!memcmp(&untouched,&peers[1],sizeof untouched));
 /* A held stack requires an acknowledged return before workbench close; idle
  * updates must continue the return without needing a second Back press. */
 struct inventory* inv=windowc_get_latest(&wc[0][1]);inventory_clear(inv);inv->picked_item=(struct item_data){.id=BLOCK_DIRT,.count=3};
 local_menu_host_update(&peers[0],&back,&glyphs[0],.016F,host_capture,&calls[0]);
 assert(calls[0].clicks==1&&!calls[0].closes&&local_menu_host_active(&peers[0]));
 windowc_action_apply_result(&wc[0][1],calls[0].last.payload.window_click.action_id,true);
 local_menu_host_update(&peers[0],&idle,&glyphs[0],.016F,host_capture,&calls[0]);
 assert(calls[0].closes==1&&!local_menu_host_active(&peers[0]));
 assert(!memcmp(&untouched,&peers[1],sizeof untouched));
 /* A full inventory must leave Manual Grid available to resolve the held
  * stack; the host must not endlessly retry or trap recipe input. */
 assert(local_menu_host_open(&peers[0],0,1,640,240));
 inv=windowc_get_latest(&wc[0][1]);inventory_clear(inv);
 for(unsigned k=CRAFTING_SLOT_MAIN;k<CRAFTING_SIZE;k++)inv->items[k]=(struct item_data){.id=BLOCK_STONE,.count=64};
 inv->picked_item=(struct item_data){.id=BLOCK_DIRT,.count=3};
 local_menu_host_update(&peers[0],&back,&glyphs[0],.016F,host_capture,&calls[0]);
 assert(peers[0].container.menu.return_blocked&&!peers[0].container.menu.close_requested);
 local_menu_host_update(&peers[0],&manual,&glyphs[0],.016F,host_capture,&calls[0]);
 assert(peers[0].menu_host.mode==LOCAL_MENU_MANUAL&&calls[0].closes==1);
 /* T12: Inventory <-> Crafting is a PAGE_SWAP tap only. A reconnect frame
  * cannot swap, and -/+ or the header arrows stay section jumps. */
 assert(local_menu_host_open(&peers[0],0,0,640,240));
 struct container_input lost={.suspended=true,.page_swap=true},prev={.previous=true},swap={.page_swap=true};
 local_menu_host_update(&peers[0],&lost,&glyphs[0],.016F,host_capture,&calls[0]);
 local_menu_host_update(&peers[0],&swap,&glyphs[0],.016F,host_capture,&calls[0]);assert(peers[0].menu_host.mode==LOCAL_MENU_CONTAINER);
 local_menu_host_update(&peers[0],&idle,&glyphs[0],.016F,host_capture,&calls[0]);
 local_menu_host_update(&peers[0],&prev,&glyphs[0],.016F,host_capture,&calls[0]);assert(peers[0].menu_host.mode==LOCAL_MENU_CONTAINER);
 local_menu_host_update(&peers[0],&swap,&glyphs[0],.016F,host_capture,&calls[0]);assert(peers[0].menu_host.mode==LOCAL_MENU_RECIPES);
 assert(local_menu_host_open(&peers[0],0,0,640,240));
 struct container_input pointer={.pointer_valid=true,.pointer_x=100,.pointer_y=28};
 local_menu_host_update(&peers[0],&pointer,&glyphs[0],.016F,host_capture,&calls[0]);
 pointer.pointer_x=54;pointer.pick=true;
 local_menu_host_update(&peers[0],&pointer,&glyphs[0],.016F,host_capture,&calls[0]);
 assert(peers[0].menu_host.mode==LOCAL_MENU_CONTAINER);
 for(unsigned k=0;k<2;k++) {
  for(unsigned j=0;j<2;j++){peers[k].windows[j]=NULL;windowc_destroy(&wc[k][j]);}
  client_session_clear_windows(&peers[k]);assert(!local_menu_host_active(&peers[k]));
 }
 puts("PASS two-owner host: independent recipe selection/tickets/replies/manual pages, acknowledged close and reconnect guard");
}

static void test_owner_frame_lifecycle(void) {
 struct client_session peer={0};assert(client_session_bind_local(&peer,51));
 struct client_rpc open={.type=CRPC_OPEN_WINDOW,.payload.window_open={.window=0,.type=WINDOW_TYPE_INVENTORY,.slot_count=INVENTORY_SIZE}};
 assert(client_session_local_reply(&peer,51,&open));peer.visible_window=-1;
 struct input_player_frame frame={.connected=true,.category=INPUT_CAT_GAMECUBE};
 for(int b=0;b<IB_COUNT;b++)frame.glyphs[b]=-1;
 frame.glyphs[IB_GUI_CLICK]=32;frame.glyphs[IB_GUI_CLICK_ALT]=33;
 frame.glyphs[IB_GUI_QUICK_MOVE]=34;frame.glyphs[IB_GUI_SPLIT]=33;
 frame.glyphs[IB_INVENTORY]=35;
 struct capture calls={0};
 /* b1-chat-opener-0919/07 C: the guest's GAMEPLAY OPEN is its own role --
  * local_menu_host.c reads frame->pressed[IB_OPEN_INVENTORY] -- while the
  * Close inside the window stays on IB_INVENTORY (container_menu_read_player
  * .close). This frame is synthetic, so no binding-layer fallback stands
  * behind it and the two bits are driven separately, which pins the split
  * from BOTH sides: the Close role alone must not open. */
 frame.pressed[IB_INVENTORY]=true;
 assert(!local_menu_host_frame(&peer,1,&frame,0,240,640,240,.016F,host_capture,&calls));
 assert(!local_menu_host_active(&peer)&&!calls.closes);
 frame.pressed[IB_INVENTORY]=false;frame.pressed[IB_OPEN_INVENTORY]=true;
 assert(local_menu_host_frame(&peer,1,&frame,0,240,640,240,.016F,host_capture,&calls));
 assert(local_menu_host_active(&peer)&&!calls.closes);
 assert(local_menu_host_frame(&peer,1,NULL,0,240,640,240,.016F,host_capture,&calls));
 assert(local_menu_host_active(&peer)&&!calls.closes);
 frame.pressed[IB_OPEN_INVENTORY]=false;
 assert(local_menu_host_frame(&peer,1,&frame,0,240,640,240,.016F,host_capture,&calls));
 frame.pressed[IB_INVENTORY]=true;
 assert(local_menu_host_frame(&peer,1,&frame,0,240,640,240,.016F,host_capture,&calls));
 assert(!local_menu_host_active(&peer)&&calls.closes==1&&peer.visible_window==-1);
 frame.pressed[IB_INVENTORY]=false;
 assert(!local_menu_host_frame(&peer,1,&frame,0,240,640,240,.016F,host_capture,&calls));
 open.payload.window_open.window=1;open.payload.window_open.type=WINDOW_TYPE_WORKBENCH;open.payload.window_open.slot_count=CRAFTING_SIZE;
 assert(client_session_local_reply(&peer,51,&open));
 assert(local_menu_host_frame(&peer,1,&frame,0,240,640,240,.016F,host_capture,&calls));
 assert(peer.menu_host.mode==LOCAL_MENU_RECIPES);
 struct client_rpc reopen={.type=CRPC_REOPEN_WINDOW,.payload.window_reopen={.window=1,.type=WINDOW_TYPE_WORKBENCH,.slot_count=CRAFTING_SIZE}};
 assert(client_session_local_reply(&peer,51,&reopen));assert(!local_menu_host_active(&peer));
 assert(local_menu_host_frame(&peer,1,&frame,0,240,640,240,.016F,host_capture,&calls));
 peer.deferred_reopen_window=1;
 struct client_rpc close={.type=CRPC_WINDOW_CLOSE_REMOTE,.payload.window_close_remote={.window=1}};
 assert(client_session_local_reply(&peer,51,&close));
 assert(!peer.windows[1]&&peer.windows[0]&&!local_menu_host_active(&peer)&&!peer.local_reply_failed);
 assert(peer.deferred_reopen_window==-1);
 assert(!local_menu_host_frame(&peer,1,&frame,0,240,640,240,.016F,host_capture,&calls));
 close.payload.window_close_remote.window=0;assert(client_session_local_reply(&peer,51,&close));assert(peer.windows[0]);
 client_session_clear_windows(&peer);
 puts("PASS owner frame lifecycle: consume open/close, no reopen loop, authoritative replacement and remote close");
}

static void test_split_presentation_rebase(void) {
 struct client_session saved[2]={gstate.players[0],gstate.players[1]};
 struct window_container wc;assert(windowc_create(&wc,WINDOW_TYPE_INVENTORY,INVENTORY_SIZE));
 struct entity bodies[2]={0};bodies[0].data.local_player.capture_input=true;
 gstate.players[0]=(struct client_session){.current_screen=&screen_inventory,.local_player=&bodies[0],.visible_window=-1};
 gstate.players[0].windows[0]=&wc;
 gstate.players[1]=(struct client_session){.local_player=&bodies[1],.local_bound=true,.local_gameplay_ready=false};
 struct inventory* inv=windowc_get_latest(&wc);inv->picked_item=(struct item_data){.id=BLOCK_DIRT,.count=3};
 assert(!screen_split_container_base(0,640,240));assert(bodies[0].data.local_player.capture_input);
 gstate.players[1].local_gameplay_ready=true;gstate.world_switch_pending=true;
 assert(!screen_split_container_base(0,640,240));gstate.world_switch_pending=false;
 uint32_t generation=wc.generation;
 assert(screen_split_container_base(0,640,240));
 assert(gstate.players[0].current_screen==&screen_ingame&&!bodies[0].data.local_player.capture_input);
 assert(gstate.players[0].menu_host.window_identity==&wc&&gstate.players[0].menu_host.split_owned);
 assert(gstate.players[0].visible_window==-1&&inv->picked_item.count==3&&wc.generation==generation);
 local_menu_host_reset(&gstate.players[0]);gstate.players[1].local_gameplay_ready=false;
 assert(clin_show_window(0));
 assert(gstate.players[0].current_screen==&screen_inventory&&!bodies[0].data.local_player.capture_input);
 assert(gstate.players[0].windows[0]==&wc&&inv->picked_item.count==3&&wc.generation==generation);
 windowc_destroy(&wc);gstate.players[0]=saved[0];gstate.players[1]=saved[1];
 puts("PASS presentation rebase: admission-gated, same held stack/window/generation, capture remains menu-owned");
}

/* ------------------------------------------------------------------ main -- */

static void test_compact_notice_bounds(void) {
 const enum window_type types[]={WINDOW_TYPE_INVENTORY,WINDOW_TYPE_CHEST,WINDOW_TYPE_FURNACE,WINDOW_TYPE_DISPENSER};
 const size_t counts[]={INVENTORY_SIZE,63,FURNACE_SIZE,DISPENSER_SIZE};
 for(unsigned k=0;k<4;k++) {
  struct window_container wc;assert(windowc_create(&wc,types[k],counts[k]));
  struct container_compact v;assert(container_compact_open(&v,types[k],counts[k],0,640,240));
  struct gutil_button_bounds r;container_compact_notice_bounds(&v,&wc,1000,&r);
  assert(r.y==16&&r.height==24&&r.x>=44&&r.x+r.width<=596&&r.width>0);
  for(size_t j=0;j<v.count;j++)assert(r.y+r.height<=v.slots[j].y);
  if(types[k]==WINDOW_TYPE_INVENTORY)assert(r.x>=container_compact_inventory_next_x(false)+24+8);
  if(container_menu_sort_supported(&wc)) {
   struct gutil_button_bounds entry;container_menu_sort_layout(&v.menu,640,240,&wc);
   container_menu_sort_entry_bounds(&v.menu,&wc,0,0,&entry);
   assert(r.x+r.width+8<=entry.x);
   v.menu.sort_open=true;container_compact_notice_bounds(&v,&wc,1000,&r);
   assert(r.x>=44+gutil_font_width("Sort",16)+8&&r.x+r.width==596);
  }
  windowc_destroy(&wc);
 }
 puts("PASS compact notices: header controls, Sort entry and all machine slots remain unobscured");
}

/* Two independent editors drive the same production transaction core. */
struct owned_sign_capture { struct server_rpc last; unsigned writes,closes; bool reject; };
static bool owned_sign_send(void* user,struct server_rpc* rpc) {
 struct owned_sign_capture* out=user;
 if(out->reject)return false;
 out->last=*rpc;out->writes+=rpc->type==SRPC_SIGN_SET;out->closes+=rpc->type==SRPC_WINDOW_CLOSE;return true;
}
static bool owned_sign_keys(struct client_session* s,unsigned owner,struct owned_sign_capture* out,int first,int second) {
 struct input_player_frame f={.connected=true,.category=INPUT_CAT_GAMECUBE};
 if(first>=0)f.down[first]=f.pressed[first]=true;
 if(second>=0)f.down[second]=f.pressed[second]=true;
 return sign_editor_update(s,owner,&f,0,owner?240:0,640,240,.016F,owned_sign_send,out);
}
static bool owned_sign_key(struct client_session* s,unsigned owner,struct owned_sign_capture* out,int key) {
 return owned_sign_keys(s,owner,out,key,-1);
}
static bool owned_sign_pointer_ab(struct client_session* s,unsigned owner,struct owned_sign_capture* out) {
 struct input_player_frame f={.connected=true,.category=INPUT_CAT_GAMECUBE,.pointer_valid=true};
 int dw=32+4+gutil_font_width("Done",16),cw=32+4+gutil_font_width("Cancel",16);
 int done_x=(640-dw-cw-18)/2;
 f.pointer_x=(float)(done_x-20);f.pointer_y=201;
 (void)sign_editor_update(s,owner,&f,0,owner?240:0,640,240,.016F,owned_sign_send,out);
 f.pointer_x=(float)(done_x+1);
 f.down[IB_GUI_CLICK]=f.pressed[IB_GUI_CLICK]=true;
 f.down[IB_GUI_CLICK_ALT]=f.pressed[IB_GUI_CLICK_ALT]=true;
 return sign_editor_update(s,owner,&f,0,owner?240:0,640,240,.016F,owned_sign_send,out);
}
static void test_owned_sign_editors(void) {
 struct client_session peers[2]={0};struct window_container wc[2];
 struct owned_sign_capture out[2]={0};
 for(unsigned k=0;k<2;k++){
  assert(windowc_create(&wc[k],WINDOW_TYPE_SIGN,SIGN_OSK_SIZE));
  struct inventory* inv=windowc_get_latest(&wc[k]);
  for(unsigned j=0;j<SIGN_OSK_SIZE;j++)inv->items[j]=(struct item_data){.id=1,.count=' '};
  peers[k].windows[WINDOWC_SIGN]=&wc[k];peers[k].local_generation=k+100;
  assert(sign_editor_open(&peers[k],WINDOWC_SIGN));
 }
 struct game_state before=gstate;
 /* Select Q and W independently; the owner's frame must outrank host keys. */
 keys[GLFW_KEY_B]=keys[GLFW_KEY_F]=keys[GLFW_KEY_SPACE]=true;


 owned_sign_key(&peers[1],1,&out[1],IB_GUI_RIGHT);
 owned_sign_key(&peers[0],0,&out[0],IB_GUI_CLICK);
 owned_sign_key(&peers[1],1,&out[1],IB_GUI_CLICK);
 assert(out[0].writes==1&&out[1].writes==1&&!out[0].closes&&!out[1].closes);
 assert(out[0].last.payload.sign_set.ch=='Q'&&out[1].last.payload.sign_set.ch=='W');
 assert(!strcmp(peers[0].sign.shadow_line,"Q")&&!strcmp(peers[1].sign.shadow_line,"W"));
 assert(!memcmp(&gstate,&before,sizeof gstate));
 /* B backspaces in the owner-specific editor; it cannot cancel or finish. */
 windowc_action_apply_result(&wc[0],out[0].last.payload.sign_set.action_id,true);
 windowc_action_apply_result(&wc[1],out[1].last.payload.sign_set.action_id,true);
 owned_sign_pointer_ab(&peers[0],0,&out[0]);
 assert(out[0].writes==2&&!out[0].closes&&!peers[0].sign.discard_prompt);
 assert(!peers[0].sign.shadow_line[0]&&peers[0].sign.pending_exit==SIGN_EXIT_NONE);
 /* Done uses Quick Move and waits for that owner's save ACK. */
 owned_sign_key(&peers[0],0,&out[0],IB_GUI_QUICK_MOVE);
 assert(!out[0].closes);assert(!owned_sign_key(&peers[0],0,&out[0],-1));
 windowc_action_apply_result(&wc[0],out[0].last.payload.sign_set.action_id,true);
 assert(owned_sign_key(&peers[0],0,&out[0],-1));assert(out[0].closes==1);
 assert(owned_sign_key(&peers[0],0,&out[0],-1)&&out[0].closes==1);
 assert(!peers[1].sign.closed&&!strcmp(peers[1].sign.shadow_line,"W"));
 /* Cancel/discard restores the opened cells and waits for its transaction. */
 owned_sign_key(&peers[1],1,&out[1],IB_INVENTORY);assert(peers[1].sign.discard_prompt);
 owned_sign_key(&peers[1],1,&out[1],IB_GUI_RIGHT);
 owned_sign_key(&peers[1],1,&out[1],IB_GUI_CLICK);
 owned_sign_key(&peers[1],1,&out[1],-1);assert(out[1].writes==2&&!out[1].closes);
 assert(out[1].last.payload.sign_set.ch==' ');
 owned_sign_key(&peers[1],1,&out[1],-1);assert(!out[1].closes);
 windowc_action_apply_result(&wc[1],out[1].last.payload.sign_set.action_id,true);
 assert(owned_sign_key(&peers[1],1,&out[1],-1)&&out[1].closes==1);
 /* Full outbound queue cancels prediction and keeps the text retryable. */
 assert(sign_editor_open(&peers[1],WINDOWC_SIGN));out[1]=(struct owned_sign_capture){.reject=true};
owned_sign_key(&peers[1],1,&out[1],IB_GUI_CLICK);
 assert(!out[1].writes&&!windowc_actions_pending(&wc[1])&&!peers[1].sign.shadow_line[0]);
 out[1].reject=false;owned_sign_key(&peers[1],1,&out[1],IB_GUI_CLICK);
 assert(out[1].writes==1);
 /* A server rejection reconciles just this editor, with no success close. */
 windowc_action_apply_result(&wc[1],out[1].last.payload.sign_set.action_id,false);
 owned_sign_key(&peers[1],1,&out[1],-1);
 assert(!peers[1].sign.shadow_line[0]&&!peers[1].sign.closed&&!out[1].closes);
 assert(!peers[0].sign.shadow_line[0]&&peers[0].sign.closed);
 /* The physical keyboard queue belongs to the primary text editor. */
 input_text_enable(true);assert(sign_char_callback);sign_char_callback(window,'Z');
 owned_sign_key(&peers[1],1,&out[1],-1);
 char queued=0;assert(input_text_poll(&queued)&&queued=='Z');input_text_enable(false);
 /* Replaced provider/window cannot reuse the previous editor. */
 peers[1].local_generation++;unsigned sent=out[1].writes;
 assert(!owned_sign_key(&peers[1],1,&out[1],IB_GUI_CLICK)&&out[1].writes==sent);
 peers[1].local_generation--;peers[1].windows[WINDOWC_SIGN]=&wc[0];
 assert(!owned_sign_key(&peers[1],1,&out[1],IB_GUI_CLICK)&&out[1].writes==sent);
 memset(keys,0,sizeof keys);
 for(unsigned k=0;k<2;k++)windowc_destroy(&wc[k]);
 puts("PASS owned sign editors: independent text/input, commit/discard ACK barriers, queue rejection, stale owner/window; P1 unchanged");
}
/* Astra D2-destructive ruling B (tmp/jobs/next-gaps-0924/astra-D2-destructive.raw)
 * on the sign editor's DISCARD confirmation (Back = keep editing, Discard =
 * throw the edit away). Both mismatch directions x stationary / reappearing
 * sprite: the first A only moves focus under the sprite, holding A does
 * nothing, and the next FRESH A answers the hovered button. */
static void test_sign_discard_reconciles_focus(void) {
	static const struct { int sprite, ring; bool reappear; } rows[] = {
		{0, 1, false}, {0, 1, true}, {1, 0, false}, {1, 0, true}};
	const float bx[2] = {640 / 2 - 146 + 10, 640 / 2 - 146 + 152 + 10};
	const float by = 240 / 2 + 10 + 10;
	for(size_t k = 0; k < sizeof rows / sizeof rows[0]; k++) {
		struct client_session s = {.local_generation = 1};
		struct window_container wc;
		struct owned_sign_capture out = {0};
		assert(windowc_create(&wc, WINDOW_TYPE_SIGN, SIGN_OSK_SIZE));
		for(unsigned c = 0; c < SIGN_OSK_SIZE; c++)
			windowc_get_latest(&wc)->items[c]
				= (struct item_data) {.id = 1, .count = ' '};
		s.windows[WINDOWC_SIGN] = &wc;
		assert(sign_editor_open(&s, WINDOWC_SIGN));
		owned_sign_key(&s, 0, &out, IB_GUI_RIGHT);
		owned_sign_key(&s, 0, &out, IB_GUI_CLICK);
		assert(out.writes == 1);
		owned_sign_key(&s, 0, &out, -1);
		owned_sign_key(&s, 0, &out, IB_INVENTORY);
		assert(s.sign.discard_prompt && s.sign.discard_selected == 0);
		int x = rows[k].sprite, y = rows[k].ring;
		struct input_player_frame f = {.connected = true,
			.category = INPUT_CAT_GAMECUBE, .pointer_valid = true,
			.pointer_x = bx[x], .pointer_y = by};
		sign_editor_update(&s, 0, &f, 0, 0, 640, 240, .016F, owned_sign_send, &out);
		f.pressed[y ? IB_GUI_RIGHT : IB_GUI_LEFT] = true;
		sign_editor_update(&s, 0, &f, 0, 0, 640, 240, .016F, owned_sign_send, &out);
		f.pressed[IB_GUI_RIGHT] = f.pressed[IB_GUI_LEFT] = false;
		assert(s.sign.discard_prompt && s.sign.discard_selected == y);
		if(rows[k].reappear) {
			f.pointer_valid = false;
			sign_editor_update(&s, 0, &f, 0, 0, 640, 240, .016F, owned_sign_send, &out);
			f.pointer_valid = true;
			sign_editor_update(&s, 0, &f, 0, 0, 640, 240, .016F, owned_sign_send, &out);
			assert(s.sign.discard_selected == y
				   && "a reacquired stationary sprite took focus by itself");
		}
		f.down[IB_GUI_CLICK] = f.pressed[IB_GUI_CLICK] = true;
		sign_editor_update(&s, 0, &f, 0, 0, 640, 240, .016F, owned_sign_send, &out);
		f.pressed[IB_GUI_CLICK] = false;
		assert(s.sign.discard_prompt && s.sign.discard_selected == x
			   && s.sign.pending_exit == SIGN_EXIT_NONE
			   && "a destructive confirm acted on a sprite/ring mismatch "
				  "(ruling B)");
		sign_editor_update(&s, 0, &f, 0, 0, 640, 240, .016F, owned_sign_send, &out);
		sign_editor_update(&s, 0, &f, 0, 0, 640, 240, .016F, owned_sign_send, &out);
		assert(s.sign.discard_prompt && s.sign.pending_exit == SIGN_EXIT_NONE
			   && "holding A answered the discard confirmation");
		f.down[IB_GUI_CLICK] = false;
		f.released[IB_GUI_CLICK] = true;
		sign_editor_update(&s, 0, &f, 0, 0, 640, 240, .016F, owned_sign_send, &out);
		f.released[IB_GUI_CLICK] = false;
		f.down[IB_GUI_CLICK] = f.pressed[IB_GUI_CLICK] = true;
		sign_editor_update(&s, 0, &f, 0, 0, 640, 240, .016F, owned_sign_send, &out);
		if(x == 1)
			assert(s.sign.pending_exit == SIGN_EXIT_DISCARD
				   && "the reconciled press did not answer Discard");
		else
			assert(!s.sign.discard_prompt
				   && s.sign.pending_exit == SIGN_EXIT_NONE
				   && "the reconciled press did not answer Back");
		windowc_destroy(&wc);
	}
	puts("PASS ruling B on the sign discard confirm: both directions, "
		 "stationary and reappearing sprite, hold inert, fresh A answers");
}

void screen_sign_test_controls(unsigned player, const struct input_player_frame* frame,
	int width, int height, struct gutil_button_bounds out[4]);
static void test_sign_pointer_activation_order(void) {
	struct client_session s={.local_generation=1};
	struct window_container wc;
	struct owned_sign_capture out={0};
	assert(windowc_create(&wc,WINDOW_TYPE_SIGN,SIGN_OSK_SIZE));
	for(unsigned k=0;k<SIGN_OSK_SIZE;k++)
		windowc_get_latest(&wc)->items[k]=(struct item_data){.id=1,.count=' '};
	s.windows[WINDOWC_SIGN]=&wc;
	assert(sign_editor_open(&s,WINDOWC_SIGN));
	/* 10-31: Done/Cancel are the editor's centred hint-row cells now; take
	 * their rects from the drawn layout. */
	struct input_player_frame f={.connected=true,.category=INPUT_CAT_GAMECUBE,
		.pointer_valid=true};
	struct gutil_button_bounds controls[4];
	screen_sign_test_controls(0,&f,640,240,controls);
	assert(controls[2].width>0&&controls[3].width>0);
	f.pointer_x=controls[2].x+1;f.pointer_y=controls[2].y+controls[2].height/2;
	sign_editor_update(&s,0,&f,0,0,640,240,.016F,owned_sign_send,&out);
	f.pressed[IB_GUI_CLICK]=true;
	sign_editor_update(&s,0,&f,0,0,640,240,.016F,owned_sign_send,&out);
	assert(!out.writes&&!s.sign.discard_prompt);
	f.pressed[IB_GUI_CLICK]=false;
	f.pressed[IB_GUI_RIGHT]=true;
	sign_editor_update(&s,0,&f,0,0,640,240,.016F,owned_sign_send,&out);
	f.pressed[IB_GUI_RIGHT]=false;f.pressed[IB_GUI_CLICK]=true;
	sign_editor_update(&s,0,&f,0,0,640,240,.016F,owned_sign_send,&out);
	assert(out.writes==1&&out.last.payload.sign_set.ch=='W'&&!out.closes);
	f.pressed[IB_GUI_CLICK]=false;f.pointer_x=controls[3].x+1;f.pointer_y=controls[3].y+controls[3].height/2;
	sign_editor_update(&s,0,&f,0,0,640,240,.016F,owned_sign_send,&out);
	f.pressed[IB_GUI_CLICK]=true;
	sign_editor_update(&s,0,&f,0,0,640,240,.016F,owned_sign_send,&out);
	assert(s.sign.discard_prompt&&out.writes==1);
	windowc_destroy(&wc);
	puts("PASS sign last input wins: ring key under resting Done; moved sprite Cancel");
}

/* Fable G4 review item 1: a sprite that owns focus over BLANK keeps the last
 * real target, so pointing the Remote away gives A back to the keyboard
 * (rule (3)). Before the fix the blank hover armed NONE, A did NOTHING and
 * the control layer consumed it: the OSK never typed. */
static void test_sign_blank_hover_then_away_types(void) {
	struct client_session s={.local_generation=1};
	struct window_container wc;
	struct owned_sign_capture out={0};
	assert(windowc_create(&wc,WINDOW_TYPE_SIGN,SIGN_OSK_SIZE));
	for(unsigned k=0;k<SIGN_OSK_SIZE;k++)
		windowc_get_latest(&wc)->items[k]=(struct item_data){.id=1,.count=' '};
	s.windows[WINDOWC_SIGN]=&wc;
	assert(sign_editor_open(&s,WINDOWC_SIGN));
	struct input_player_frame f={.connected=true,.category=INPUT_CAT_GAMECUBE,
		.pointer_valid=true,.pointer_x=4,.pointer_y=4}; /* outside the window */
	sign_editor_update(&s,0,&f,0,0,640,240,.016F,owned_sign_send,&out);
	f.pointer_x=24; /* purposeful motion: the sprite owns focus over blank */
	sign_editor_update(&s,0,&f,0,0,640,240,.016F,owned_sign_send,&out);
	assert(s.sign.sign_focus.pointer_owner);
	assert(s.sign.action_target==4 && "a blank hover discarded the keyboard target");
	f.pointer_valid=false; /* Remote pointed away */
	sign_editor_update(&s,0,&f,0,0,640,240,.016F,owned_sign_send,&out);
	f.pressed[IB_GUI_CLICK]=true;
	sign_editor_update(&s,0,&f,0,0,640,240,.016F,owned_sign_send,&out);
	assert(out.writes==1&&out.last.payload.sign_set.ch=='Q'&&!out.closes
		&& "A after a blank hover and pointer loss did not type");
	windowc_destroy(&wc);
	puts("PASS sign blank hover then pointer away: A types the keyboard target");
}

int main(void) {
	blocks_init();
	items_init();
	recipe_init();
	test_recipe_catalog_145();
	test_recipe_have_need();

	assert(svin_init());
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	gstate.world_loaded = true;
	gstate.config_user.root = json_parse_string(
		"{\"input\":{\"inventory\":[69],\"gui_click\":[32],\"gui_click_alt\":[66],"
		"\"gui_up\":[87],\"gui_down\":[83],\"gui_left\":[65],\"gui_right\":[68]}}");
	assert(gstate.config_user.root);
	window = (GLFWwindow*)1;
	input_init();
	test_recipe_bindings();
 test_recipe_presentation_geometry();
 test_recipe_footer_last_input_wins();
 test_recipe_blank_hover_then_away_crafts();
 test_compact_notice_bounds();
 test_two_owner_menu_host();
 test_owner_frame_lifecycle();
 test_split_presentation_rebase();

	struct window_container wc[2];
	assert(windowc_create(&wc[0], WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
	assert(windowc_create(&wc[1], WINDOW_TYPE_WORKBENCH, CRAFTING_SIZE));
	gstate.players[0].windows[WINDOWC_INVENTORY] = inventory_window = &wc[0];
	gstate.players[0].windows[WINDOWC_CRAFTING] = workbench = &wc[1];
	gstate.players[0].visible_window = WINDOWC_CRAFTING;
	screen_crafting_set_windowc(WINDOWC_CRAFTING);

	test_console_workbench();
	test_recipe_craft_flow();
	gstate.players[0].visible_window = -1;
	test_personal_2x2_ledger();
 test_sign_real_transaction();
 test_owned_sign_editors();
 test_sign_pointer_activation_order();
 test_sign_discard_reconciles_focus();
 test_sign_blank_hover_then_away_types();

	windowc_destroy(&wc[0]);
	windowc_destroy(&wc[1]);
	gstate.players[0].windows[WINDOWC_INVENTORY] = NULL;
	gstate.players[0].windows[WINDOWC_CRAFTING] = NULL;
 struct client_session peers[2]={0};
 peers[0].recipes.active=peers[1].recipes.active=true;
 peers[0].manual_grid.active=peers[1].manual_grid.active=true;
 peers[0].container.active=peers[1].container.active=true;
 client_session_clear_windows(&peers[0]);
 assert(!peers[0].recipes.active&&!peers[0].manual_grid.active&&!peers[0].container.active);
 assert(peers[1].recipes.active&&peers[1].manual_grid.active&&peers[1].container.active);
	world_destroy(&gstate.world);
	config_destroy(&gstate.config_user);
	window = NULL;
	puts("test_recipe_ui: PASS");
	return 0;
}
