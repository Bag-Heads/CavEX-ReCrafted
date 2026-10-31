/* Native container UX: real input latch, screen updates and window revisions.
 * The renderer/GLFW device is absent; the server still owns slot permissions. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#undef GLFW_DLL
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "block/blocks.h"
#include "game/game_state.h"
#include "game/gui/container_menu.h"
#include "game/gui/container_compact.h"
#include "game/gui/screen.h"
#include "platform/controller_events.h"
#include "graphics/gui_util.h"
#include "item/items.h"
#include "network/server_interface.h"
#include "network/inventory_logic.h"
#include "network/server_local_state.h"
#include "network/server_local.h"
#include "network/client_interface.h"
#include "item/recipe.h"
#include "platform/input_glyph.h"
#include "platform/input_owner.h"
#include "game/gui/local_menu_host.h"
#include "game/gui/recipe_browser.h"

static void test_quick_move_rules(void) {
	struct inventory inv;
	struct server_player player = {0};
	set_inv_slot_t changes;
	set_inv_slot_init(changes);
	assert(inventory_create(&inv, NULL, NULL, NULL, 90, 0, 0, 0));
	inv.logic = &inventory_logic_double_chest;
	inv.items[0] = (struct item_data) {.id = 1, .count = 20};
	inv.items[89] = (struct item_data) {.id = 1, .count = 60};
	assert(!inventory_quick_move(&inv, &player, 0, NULL).id);
	assert(inv.items[0].count == 20 && inv.items[89].count == 60);
	assert(inventory_quick_move(&inv, &player, 0, changes).id);
	assert(!inv.items[0].id && inv.items[89].count == 64 && inv.items[88].count == 16);
	assert(!inv.picked_item.id);
	assert(inventory_quick_move(&inv, &player, 88, changes).id);
	assert(inv.items[0].count == 16 && !inv.items[88].id);
	/* Full destination: preserve the source; partial room: move only what fits. */
	for(size_t k = 54; k < 90; k++) inv.items[k] = (struct item_data) {.id = 3, .count = 64};
	assert(inventory_quick_move(&inv, &player, 0, changes).id);
	inv.items[89] = (struct item_data) {.id = 1, .count = 63};
	/* Comparison is tested at the server boundary, after mutation. */
	assert(inventory_quick_move(&inv, &player, 0, changes).id);
	assert(inv.items[0].count == 15 && inv.items[89].count == 64);
	assert(inventory_quick_move(&inv, &player, 0, changes).id);
	assert(!inventory_quick_move(&inv, &player, 255, changes).id);
	inv.invalidated = true;
	assert(!inventory_quick_move(&inv, &player, 0, changes).id);
	inv.invalidated = false;
	inv.logic = NULL; inventory_destroy(&inv, NULL);

	/* ContainerPlayer: partial output transfers still consume one recipe. */
	assert(inventory_create(&inv, NULL, NULL, NULL, 45, 0, 0, 0));
	inv.logic = &inventory_logic_player;
	inv.items[1] = (struct item_data) {.id = BLOCK_LOG, .count = 2};
	inv.logic->post_action(&inv, &player, 1, false, true, changes);
	assert(inv.items[0].id == BLOCK_PLANKS && inv.items[0].count == 4);
	for(size_t k = 9; k < 45; k++) inv.items[k] = (struct item_data) {.id = 3, .count = 64};
	inv.items[44] = (struct item_data) {.id = BLOCK_PLANKS, .count = 62};
	assert(inventory_quick_move(&inv, &player, 0, changes).id);
	assert(inv.items[1].count == 1 && inv.items[44].count == 64);
	inv.items[43] = (struct item_data) {0};
	assert(inventory_quick_move(&inv, &player, 0, changes).id);
	assert(!inv.items[1].id && inv.items[44].count == 64 && inv.items[43].count == 4);
	assert(!inv.picked_item.id && !inv.items[0].id);
	/* No automatic armor equip. Normal inventory transfers use the hotbar. */
	inventory_clear(&inv);
	inv.items[9] = (struct item_data) {.id = ITEM_IRON_HELMET, .count = 1};
	assert(inventory_quick_move(&inv, &player, 9, changes).id);
	assert(!inv.items[5].id && inv.items[36].id == ITEM_IRON_HELMET);
	inv.picked_item = (struct item_data) {.id = 1, .count = 1};
	assert(inventory_quick_move(&inv, &player, 36, changes).id);
	assert(inv.picked_item.count == 1);
	inv.logic = NULL; inventory_destroy(&inv, NULL);

	/* Beta furnace main -> hotbar, never into the output/fuel/input slots. */
	assert(inventory_create(&inv, NULL, NULL, NULL, 39, 0, 0, 0));
	inv.logic = &inventory_logic_furnace;
	inv.items[3] = (struct item_data) {.id = ITEM_COAL, .count = 16};
	assert(inventory_quick_move(&inv, &player, 3, changes).id);
	assert(inv.items[30].count == 16 && !inv.items[0].id && !inv.items[1].id && !inv.items[2].id);
	inv.logic = NULL; inventory_destroy(&inv, NULL);
	set_inv_slot_clear(changes);
	puts("PASS quick move: transfer order, partial/full stacks, stale requests, whole crafting result, armor/furnace restrictions");
}

static void test_quick_cake(void) {
	struct inventory inv;
	struct server_player actor = {0};
	set_inv_slot_t changes;
	set_inv_slot_init(changes);
	assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
	inv.logic = &inventory_logic_crafting;
	const uint16_t recipe[] = {ITEM_MILK_BUCKET, ITEM_MILK_BUCKET, ITEM_MILK_BUCKET,
		ITEM_SUGAR, ITEM_EGG, ITEM_SUGAR, ITEM_WHEAT, ITEM_WHEAT, ITEM_WHEAT};
	for(size_t k = 0; k < 9; k++) inv.items[1 + k] = (struct item_data) {.id = recipe[k], .count = 1};
	inv.logic->post_action(&inv, &actor, 1, false, true, changes);
	assert(inv.items[0].id == ITEM_CAKE);
	assert(inventory_quick_move(&inv, &actor, 0, changes).id);
	assert(inv.items[45].id == ITEM_CAKE && inv.items[45].count == 1);
	for(size_t k = 1; k <= 3; k++) assert(inv.items[k].id == ITEM_BUCKET && inv.items[k].count == 1);
	for(size_t k = 4; k <= 9; k++) assert(!inv.items[k].id);
	assert(!inv.items[0].id && !inv.picked_item.id);
	inv.logic = NULL; inventory_destroy(&inv, NULL);
	set_inv_slot_clear(changes);
	puts("PASS quick craft: real cake recipe consumed once; all three buckets stay in the crafting grid");
}

extern GLFWwindow* window;
static bool keys[GLFW_KEY_LAST + 1];
static bool mouse_buttons[GLFW_MOUSE_BUTTON_LAST + 1];
static double px = -1, py = -1;

int glfwGetKey(GLFWwindow* w, int key) {
	(void)w;
	assert(key >= 0 && key <= GLFW_KEY_LAST);
	return keys[key] ? GLFW_PRESS : GLFW_RELEASE;
}
int glfwGetMouseButton(GLFWwindow* w, int button) {
	(void)w;
	assert(button >= 0 && button <= GLFW_MOUSE_BUTTON_LAST);
	return mouse_buttons[button] ? GLFW_PRESS : GLFW_RELEASE;
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
#endif

struct capture {
	unsigned clicks, closes, drops, pauses;
	struct server_rpc last;
};
static void receive(struct server_rpc* call, void* user) {
	struct capture* out = user;
	out->last = *call;
	out->clicks += call->type == SRPC_WINDOW_CLICK;
	out->closes += call->type == SRPC_WINDOW_CLOSE;
	out->drops += call->type == SRPC_WINDOW_DROP;
	out->pauses += call->type == SRPC_TOGGLE_PAUSE;
}
static struct capture drain(void) {
	struct capture out = {0};
	svin_process_messages(receive, &out, false);
	return out;
}
static void frame(struct screen* screen, const char* pressed, float dt) {
	memset(keys, 0, sizeof(keys));
	for(const char* p = pressed; *p; p++)
		keys[(unsigned char)*p] = true;
	input_poll();
	screen->update(screen, dt);
}
static void press(struct screen* screen, const char* pressed) {
	frame(screen, "", 0.016F);
	frame(screen, pressed, 0.016F);
}

static void test_bindings(void) {
	/* These keys were intentionally omitted from the loaded legacy config. */
	memset(keys, 0, sizeof(keys));
	input_poll();
	keys['R'] = true;
	input_poll();
	assert(input_pressed(IB_GUI_SPLIT));
	assert(!input_pressed(IB_GUI_CLICK_ALT));
	struct container_input input;
	container_menu_read_input(&input, 0, 0, 640, 480);
	assert(input.split && !input.close);
	int symbol, glyph;
	enum input_category category;
	assert(input_symbol(IB_GUI_SPLIT, &symbol, &glyph, &category));
	assert(glyph == INPUT_GLYPH_PC_KEY_BASE + 'R'); /* R cap, as the real binding */
	JSON_Object* root = json_object(gstate.config_user.root);
	assert(json_object_dotset_value(root, "input.gui_split", json_value_init_array()) == JSONSuccess);
	assert(json_object_dotset_value(root, "input.gui_click_alt", json_parse_string("[1001]")) == JSONSuccess);
	memset(keys, 0, sizeof(keys)); memset(mouse_buttons, 0, sizeof(mouse_buttons)); input_poll();
	mouse_buttons[GLFW_MOUSE_BUTTON_RIGHT] = true; input_poll();
	container_menu_read_input(&input, 0, 0, 640, 480);
	assert(input.split && !input.close); /* Beta PC right-click remains split. */
	assert(json_object_dotset_value(root, "input.gui_click_alt", json_parse_string("[66]")) == JSONSuccess);
	memset(mouse_buttons, 0, sizeof(mouse_buttons)); input_poll();
	memset(keys, 0, sizeof(keys)); input_poll();
	keys['R'] = true; input_poll();
	assert(!input_pressed(IB_GUI_SPLIT));
	assert(!input_symbol(IB_GUI_SPLIT, &symbol, &glyph, &category));
	assert(json_object_dotremove(root, "input.gui_split") == JSONSuccess);
	assert(json_object_dotset_value(root, "input.gui_quick_move", json_parse_string("[69]")) == JSONSuccess);
	memset(keys, 0, sizeof(keys)); input_poll();
	keys['E'] = true; input_poll();
	container_menu_read_input(&input, 0, 0, 640, 480);
	assert(input.quick_move && input.close); /* Close is never suppressed by Quick Move. */
	assert(json_object_dotremove(root, "input.gui_quick_move") == JSONSuccess);
	assert(config_set_string(&gstate.config_user, "debug.container_glyphs", "wiimote"));
	assert(input_symbol(IB_INVENTORY, &symbol, &glyph, &category));
	assert(glyph == 2 && category == INPUT_CAT_NUNCHUK);
	assert(input_symbol(IB_HOME, &symbol, &glyph, &category) && glyph == 4);
	assert(config_set_string(&gstate.config_user, "debug.container_glyphs", "classic"));
	assert(input_symbol(IB_GUI_QUICK_MOVE, &symbol, &glyph, &category));
	assert(glyph == 13 && category == INPUT_CAT_CLASSIC_CONTROLLER);
	assert(input_symbol(IB_GUI_SPLIT, &symbol, &glyph, &category) && glyph == 11);
	assert(input_symbol(IB_INVENTORY, &symbol, &glyph, &category) && glyph == 12);
	assert(input_symbol(IB_GUI_DROP, &symbol, &glyph, &category) && glyph == 6);
	assert(input_symbol(IB_GUI_PREVIOUS, &symbol, &glyph, &category) && glyph == 22);
	assert(input_symbol(IB_GUI_PAGE, &symbol, &glyph, &category) && glyph == 23);
	assert(config_set_string(&gstate.config_user, "debug.container_glyphs", "auto"));
	puts("PASS container bindings: legacy fallback, matching glyph, explicit disable");
}

static void test_neighbors(void) {
	struct container_slot slots[45];
	for(int k = 0; k < 45; k++)
		slots[k] = (struct container_slot) {(k % 9) * 36, (k / 9) * 36, k, 0};
	for(size_t k = 0; k < 45; k++) {
		assert(container_menu_neighbor(slots, 45, k, -2) == (k % 9 ? k - 1 : k));
		assert(container_menu_neighbor(slots, 45, k, 2) == (k % 9 < 8 ? k + 1 : k));
		assert(container_menu_neighbor(slots, 45, k, -1) == (k >= 9 ? k - 9 : k));
		assert(container_menu_neighbor(slots, 45, k, 1) == (k < 36 ? k + 9 : k));
	}
	puts("PASS container navigation: every empty grid slot reachable, clamped edges");
}

static void enter_manual_container(struct screen* screen){
 if(screen!=&screen_crafting)return;
 assert(gstate.players[0].recipes.active);press(screen,"T");assert(!gstate.players[0].recipes.active);
}
static void test_screens(void) {
	struct screen* screens[] = {&screen_inventory, &screen_crafting,
		&screen_furnace, &screen_chest, &screen_dispenser, &screen_iron_chest};
	const int ids[] = {WINDOWC_INVENTORY, WINDOWC_CRAFTING, WINDOWC_FURNACE,
		WINDOWC_CHEST, WINDOWC_DISPENSER, WINDOWC_IRON_CHEST};
	const size_t hotbars[] = {36, 37, 30, 54, 36, 81};
	const size_t mains[] = {9, 10, 3, 27, 9, 54};
	for(size_t k = 0; k < 6; k++) {
		struct screen* screen = screens[k];
		struct window_container* wc = gstate.players[0].windows[ids[k]];
		screen_set(screen);
  enter_manual_container(screen);
		px = py = -1;
		/* Empty targets still emit the selected authoritative slot id. */
		press(screen, "D "); /* R-B consumes simultaneous navigation + A */
		struct capture c = drain();
		assert(!c.clicks);
		press(screen, " ");
		c = drain();
		assert(c.clicks == 1 && c.last.payload.window_click.slot == (k==1?2:hotbars[k]+1));
		windowc_discard_pending_actions(wc);

		/* A stationary pointer must not override directional focus. */
		screen_set(screen);
  enter_manual_container(screen);
		px = k == 5 ? 44 : 168;
		py = k == 5 ? 226 : 236; /* B3: inventory/machine panels up 14 (was 250) */
		/* Frozen chest.html:10,14,16,18: first player row at y=190. */
		if(k == 3) py = 198;
		if(k == 1){px=270;py=152;frame(screen,"",.016F);px=290;}
		frame(screen, "", 0.016F);
		press(screen, "D ");
		c = drain();
		assert(!c.clicks);
		press(screen, " ");
		c = drain();
		assert(c.clicks == 1 && c.last.payload.window_click.slot == mains[k] + 1);
		windowc_discard_pending_actions(wc);
		px += 2;
		press(screen, " ");
		c = drain();
		assert(c.clicks == 1 && c.last.payload.window_click.slot == mains[k] + 1);
		windowc_discard_pending_actions(wc);

		/* Split routes to the existing right-click action, not to Back. */
		px = py = -1;
		struct inventory* inv = windowc_get_latest(wc);
		inventory_set_slot(inv, mains[k] + 1,
			(struct item_data) {.id = 1, .count = 9});
		press(screen, "R");
		c = drain();
		assert(c.clicks == 1 && c.closes == 0 && c.last.payload.window_click.right_click);
		assert(windowc_get_latest(wc)->picked_item.count == 5);
		assert(windowc_get_latest(wc)->items[mains[k] + 1].count == 4);
		windowc_discard_pending_actions(wc);

		/* Outside clicks no longer implicitly drop. Explicit Drop sends one
		 * request and predicts the cursor deletion before either provider ACKs. */
		inv = windowc_get_latest(wc);
		inventory_set_picked_item(inv, (struct item_data) {.id = 1, .count = 9});
		px = py = 10;
		frame(screen, "", 0.016F); /* reacquire */
		px = 30; /* deliberate movement takes pointer ownership */
		press(screen, " ");
		c = drain();
		assert(!c.clicks && !c.drops);
		press(screen, "Q");
		c = drain();
		assert(c.drops == 1 && c.last.payload.window_drop.window == ids[k]);
		assert(!c.last.payload.window_drop.right && !windowc_get_latest(wc)->picked_item.id);
		windowc_action_apply_result(wc, c.last.payload.window_drop.action_id, true);
		inv = windowc_get_latest(wc);
		frame(screen, "Q", 1.0F);
		c = drain(); assert(!c.drops);
		inventory_set_picked_item(inv, (struct item_data){.id = 1, .count = 9});
		/* Inventory owns the frame. Containers return the cursor before
		 * sending Close; the PLAYER inventory closes as Beta does: one Close,
		 * the window owner drops the cursor (Container.java:154-160). */
		press(screen, "ERQ ");
		c = drain();
		if(k == 0) {
			assert(c.closes == 1 && !c.clicks && !c.drops);
			inventory_clear(windowc_get_latest(wc));
			continue;
		}
		assert(!c.closes && c.clicks == 1 && !c.drops);
		assert(inv->picked_item.count == 9);
		windowc_action_apply_result(wc, c.last.payload.window_click.action_id, true);
		frame(screen, "", .016F);
		c = drain();
		if(k==1){assert(!c.closes&&gstate.players[0].recipes.active&&!gstate.players[0].manual_grid.active);press(screen,"E");c=drain();}
		assert(c.closes == 1 && !c.drops);
		inventory_clear(windowc_get_latest(wc));
	}
	puts("PASS all six screens: empty slots, focus priority, Split, explicit Drop, Back");
}

/* Q7 (maintainer 2026-10-31): the full-screen 90-slot chest shows ONE 27-slot
 * half at a time in the 3-row panel (off 146,24): storage cells at
 * x 160+36c, y 58+36r. Section Next/Prev (F/G) and the header's "<" ">"
 * arrows swap halves; the focus keeps its cell. */
static void test_full_chest(void) {
	struct window_container* wc = gstate.players[0].windows[WINDOWC_CHEST];
	windowc_destroy(wc);
	assert(windowc_create(wc, WINDOW_TYPE_CHEST, 90));
	screen_set(&screen_chest);
	px = py = -1;
	struct capture c = drain();
	assert(!c.clicks && !c.closes && !c.drops);
	for(unsigned half = 0; half < 2; half++) {
		/* Every cell of the shown half addresses that half's storage slot. */
		for(size_t k = 0; k < 27; k++) {
			px = 168 + (k % 9) * 36; py = 74 + (k / 9) * 36;
			frame(&screen_chest, "", .016F);
			px += 10; /* deliberate pointer reacquisition on the first cell */
			press(&screen_chest, " ");
			c = drain();
			assert(c.clicks == 1 && c.last.payload.window_click.slot == half * 27 + k);
			windowc_discard_pending_actions(wc);
		}
		/* The row under the storage is the player's first main row (54). */
		px = 168; py = 74 + 3 * 36 + 24;
		frame(&screen_chest, "", .016F); px += 10;
		press(&screen_chest, " ");
		c = drain();
		assert(c.clicks == 1 && c.last.payload.window_click.slot == 54);
		windowc_discard_pending_actions(wc);
		/* Section Next (F) swaps the half; it never clicks, drops or closes. */
		px = py = -1;
		press(&screen_chest, "F");
		c = drain();
		assert(!c.clicks && !c.closes && !c.drops);
	}
	/* Two swaps are a round trip; Previous (G) is the other direction. */
	px = 178; py = 74; frame(&screen_chest, "", .016F); px = 168; frame(&screen_chest, "", .016F);
	press(&screen_chest, " "); c = drain();
	assert(c.clicks == 1 && c.last.payload.window_click.slot == 0);
	windowc_discard_pending_actions(wc);
	px = py = -1; press(&screen_chest, "G"); c = drain(); assert(!c.clicks);
	px = 178; py = 74; frame(&screen_chest, "", .016F); px = 168; frame(&screen_chest, "", .016F);
	press(&screen_chest, " "); c = drain();
	assert(c.clicks == 1 && c.last.payload.window_click.slot == 27);
	windowc_discard_pending_actions(wc);
	/* The header's plain ">" arrow is a pointer target (Q2): it swaps the
	 * half from the rect the painter draws it in, title "Chest 2" now. */
	struct gutil_button_bounds r[4];
	container_chest_header_rects(146, 24, 348, "Chest 2", r);
	px = r[2].x + r[2].width / 2.0; py = r[2].y + r[2].height / 2.0;
	frame(&screen_chest, "", .016F); px += 1; frame(&screen_chest, "", .016F);
	press(&screen_chest, " "); c = drain();
	assert(!c.clicks && !c.closes && !c.drops);
	px = 178; py = 74; frame(&screen_chest, "", .016F); px = 168; frame(&screen_chest, "", .016F);
	press(&screen_chest, " "); c = drain();
	assert(c.clicks == 1 && c.last.payload.window_click.slot == 0 && "the > arrow did not swap back to half 1");
	windowc_discard_pending_actions(wc);

	/* The UNPAGED 63-slot chest draws no header arrows, so a click on the
	 * rect where the paged chest's "<" sits must NOT jump the focus section
	 * (the bug: container_menu_update hit-tested the invisible rects). */
	windowc_destroy(wc);
	assert(windowc_create(wc, WINDOW_TYPE_CHEST, 63));
	screen_set(&screen_chest);
	px = py = -1; frame(&screen_chest, "", .016F);
	container_chest_header_rects(146, 24, 348, "Chest", r);
	px = r[0].x + r[0].width / 2.0; py = r[0].y + r[0].height / 2.0;
	frame(&screen_chest, "", .016F); px += 1; frame(&screen_chest, "", .016F);
	press(&screen_chest, " "); drain();
	windowc_discard_pending_actions(wc);
	px = py = -1; frame(&screen_chest, "", .016F);
	press(&screen_chest, " "); c = drain();
	assert(c.clicks == 1 && c.last.payload.window_click.slot == 54
		   && "the invisible header rect jumped the focus to the storage section");
	windowc_discard_pending_actions(wc);
	/* The later round-trip test expects the double chest window again. */
	windowc_destroy(wc);
	assert(windowc_create(wc, WINDOW_TYPE_CHEST, 90));
	puts("PASS full chest: 90 slots as two 27-slot halves (F/G and the > arrow swap, "
		 "focus keeps its cell); the unpaged chest hit-tests no header rect");
}

static bool capture_send(void* user, struct server_rpc* rpc) {
	receive(rpc, user);
	return true;
}

static void test_quick_move_ui(void) {
	struct window_container wc;
	assert(windowc_create(&wc, WINDOW_TYPE_CHEST, 90));
	struct inventory* inv = windowc_get_latest(&wc);
	inv->items[0] = (struct item_data) {.id = 1, .count = 12};
	struct container_menu menu = {0};
	const struct container_slot slots[] = {{160, 44, 0, 0}};
	struct container_input quick = {.quick_move = true, .pick = true};
	struct capture calls = {0};
	container_menu_step(&menu, slots, 1, 0, 0, &wc, 3, &quick, .016F, capture_send, &calls);
	assert(calls.clicks == 1 && calls.last.payload.window_click.quick_move);
	assert(calls.last.payload.window_click.expected.count == 12 && wc.quick_pending);
	assert(!windowc_get_latest(&wc)->items[0].id);
	assert(windowc_get_latest(&wc)->items[89].count == 12);
	assert(!windowc_get_latest(&wc)->picked_item.id);
	/* A slot update while the request is pending must not replay it as Pick. */
	windowc_slot_change(&wc, 0, (struct item_data) {.id = 1, .count = 5});
	assert(!windowc_get_latest(&wc)->items[0].id);
	assert(windowc_get_latest(&wc)->items[89].count == 5);
	assert(!windowc_get_latest(&wc)->picked_item.id);
	container_menu_step(&menu, slots, 1, 0, 0, &wc, 3, &quick, .016F, capture_send, &calls);
	assert(calls.clicks == 1);
	windowc_action_apply_result(&wc, calls.last.payload.window_click.action_id, false);
	assert(!wc.quick_pending && windowc_get_latest(&wc)->items[0].count == 5);
	windowc_destroy(&wc);
	puts("PASS quick move UI: one request, predicted transfer/replay, rejection keeps authoritative contents");
}
static void test_slot_activation_order(void) {
	struct window_container wc;
	assert(windowc_create(&wc,WINDOW_TYPE_CHEST,90));
	struct inventory* inv=windowc_get_latest(&wc);
	inv->items[0]=(struct item_data){.id=BLOCK_DIRT,.count=1};
	inv->items[1]=(struct item_data){.id=BLOCK_STONE,.count=1};
	const struct container_slot slots[]={{0,0,0,0},{40,0,1,0}};
	struct container_menu menu={.selected=1,.focus={.initialized=true}};
	struct capture calls={0};
	struct container_input in={.pointer_valid=true,.pointer_x=1,.pointer_y=1};
	container_menu_step(&menu,slots,2,0,0,&wc,3,&in,.016F,capture_send,&calls);
	in.pick=true;
	container_menu_step(&menu,slots,2,0,0,&wc,3,&in,.016F,capture_send,&calls);
	assert(!calls.clicks&&menu.selected==0); /* focus-only on first A */
	in.pick=false;in.right=true;
	container_menu_step(&menu,slots,2,0,0,&wc,3,&in,.016F,capture_send,&calls);
	in.right=false;in.pick=true;
	container_menu_step(&menu,slots,2,0,0,&wc,3,&in,.016F,capture_send,&calls);
	assert(calls.clicks==1&&calls.last.payload.window_click.slot==1);
	in.pick=false;in.pointer_x=20;
	container_menu_step(&menu,slots,2,0,0,&wc,3,&in,.016F,capture_send,&calls);
	assert(menu.selected==0);
	in.pick=true;
	container_menu_step(&menu,slots,2,0,0,&wc,3,&in,.016F,capture_send,&calls);
	assert(calls.clicks==2&&calls.last.payload.window_click.slot==0);
	windowc_destroy(&wc);
	puts("PASS container last input wins: focus-only, ring slot, moved sprite slot");
}

static void apply_reply(const struct client_rpc* rpc, void* user) {
	(void)user;
	struct client_rpc copy = *rpc;
	clin_process_direct(&copy);
}

static void test_quick_move_round_trip(void) {
	static struct server_local server;
	struct inventory authoritative;
	assert(inventory_create(&authoritative, NULL, NULL, NULL, 90, 0, 0, 0));
	authoritative.logic = &inventory_logic_double_chest;
	server.player.active_inventory = &authoritative;
	server.player.has_pos = server.player.finished_loading = true;
	struct window_container* wc = gstate.players[0].windows[WINDOWC_CHEST];
	struct window_container* saved = gstate.players[0].windows[WINDOWC_IRON_CHEST];
	gstate.players[0].windows[WINDOWC_IRON_CHEST] = wc;
	windowc_discard_pending_actions(wc);
	inventory_clear(windowc_get_latest(wc));
	authoritative.items[75] = (struct item_data) {.id = ITEM_COAL, .count = 64};
	windowc_slot_change(wc, 75, authoritative.items[75]);
	assert(clin_init());
	clin_test_set_rpc_observer(apply_reply, NULL);
	struct container_menu menu = {0};
	struct container_slot target = {160, 44, 75, 0};
	struct container_input split = {.split = true}, pick = {.pick = true}, quick = {.quick_move = true};
	struct capture calls = {0};
	container_menu_step(&menu, &target, 1, 0, 0, wc, WINDOWC_IRON_CHEST, &split, .016F, capture_send, &calls);
	server_local_test_process_rpc(&server, &calls.last);
	clin_drain_discard();
	assert(authoritative.picked_item.count == 32 && authoritative.items[75].count == 32);
	assert(windowc_get_latest(wc)->picked_item.count == 32);
	target.slot = 27;
	container_menu_step(&menu, &target, 1, 0, 0, wc, WINDOWC_IRON_CHEST, &pick, .016F, capture_send, &calls);
	server_local_test_process_rpc(&server, &calls.last);
	clin_drain_discard();
	assert(authoritative.items[27].count == 32 && !authoritative.picked_item.id);
	assert(windowc_get_latest(wc)->items[27].count == 32);
	container_menu_step(&menu, &target, 1, 0, 0, wc, WINDOWC_IRON_CHEST, &quick, .016F, capture_send, &calls);
	server_local_test_process_rpc(&server, &calls.last);
	clin_drain_discard();
	assert(authoritative.items[75].count == 64 && !authoritative.items[27].id);
	assert(windowc_get_latest(wc)->items[75].count == 64 && !windowc_get_latest(wc)->items[27].id);
	/* Large-chest storage must never be mirrored into player main slots. */
	struct client_rpc update = {.type = CRPC_INVENTORY_SLOT,
		.payload.inventory_slot = {.window = WINDOWC_IRON_CHEST, .slot = 27,
			.item = {.id = ITEM_COAL, .count = 7}}};
	struct inventory* player = windowc_get_latest(gstate.players[0].windows[0]);
	struct item_data before = player->items[9];
	clin_process_direct(&update);
	assert(player->items[9].id == before.id && player->items[9].count == before.count);
	update.payload.inventory_slot.slot = 54;
	clin_process_direct(&update);
	assert(player->items[9].id == ITEM_COAL && player->items[9].count == 7);
	clin_test_set_rpc_observer(NULL, NULL);
	gstate.players[0].windows[WINDOWC_IRON_CHEST] = saved;
	authoritative.logic = NULL;
	inventory_destroy(&authoritative, NULL);
	inventory_clear(windowc_get_latest(wc));
	inventory_clear(player);
	puts("PASS live RPC round trip: split 64 into 32+32, place in Chest 2, quick-merge to 64, correct large-chest player mirror");
}

static void assert_compact_slots(const struct container_compact* v,
	bool seen[90], size_t capacity) {
	bool reachable[90] = {0};
	reachable[0] = true;
	for(size_t pass = 0; pass < v->count; pass++) {
		for(size_t k = 0; k < v->count; k++) {
			if(!reachable[k]) continue;
			const int directions[] = {-2, -1, 1, 2};
			for(size_t d = 0; d < 4; d++)
				reachable[container_menu_neighbor(v->slots, v->count, k, directions[d])] = true;
		}
	}
	for(size_t k = 0; k < v->count; k++) {
		const struct container_slot* a = &v->slots[k];
		assert(a->slot < capacity && reachable[k]);
		int ae=container_slot_extent(&v->menu,a);
  assert(ae==26||ae==28||ae==32||ae==36); /* 26 = T13 split inventory work area */
  assert(a->x >= 32 && a->x + ae <= 608);
		assert(a->y >= 40 && a->y + ae <= 188);
		seen[a->slot] = true;
		for(size_t j = 0; j < k; j++) {
			const struct container_slot* b = &v->slots[j];
			assert(a->slot != b->slot);
			int be=container_slot_extent(&v->menu,b);
   assert(a->x+ae<=b->x||b->x+be<=a->x||a->y+ae<=b->y||b->y+be<=a->y);
		}
	}
}

static void test_compact_layouts(void) {
	assert(container_layout_for_height(480) == CONTAINER_LAYOUT_FULL);
	assert(container_layout_for_height(240) == CONTAINER_LAYOUT_COMPACT);
	const enum window_type types[] = {WINDOW_TYPE_INVENTORY, WINDOW_TYPE_WORKBENCH,
		WINDOW_TYPE_FURNACE, WINDOW_TYPE_DISPENSER, WINDOW_TYPE_CHEST,
		WINDOW_TYPE_CHEST, WINDOW_TYPE_IRON_CHEST};
	const size_t sizes[] = {45, 46, 39, 45, 63, 90, 90};
	for(size_t k = 0; k < 7; k++) {
		struct container_compact v;
		struct window_container wc;
		assert(windowc_create(&wc, types[k], sizes[k]));
		bool machine=types[k]==WINDOW_TYPE_FURNACE||types[k]==WINDOW_TYPE_DISPENSER;
  assert(container_compact_open(&v, types[k], sizes[k], 0, 640, 480)==machine);
		assert(!container_compact_open(&v, types[k], sizes[k], 0, 640, 120));
		assert(container_compact_open(&v, types[k], sizes[k], 0, 640, 240));
		bool seen[90] = {0};
		struct capture calls = {0};
		struct container_input page = {.page = true, .pick = true, .drop = true};
		unsigned initial = v.view;
		for(unsigned p = 0; p < v.view_count; p++) {
			assert_compact_slots(&v, seen, sizes[k]);
			if(v.type==WINDOW_TYPE_INVENTORY){
    /* T13 approved V1: ONE split page -- 36 carried slots + armor, real 2x2
     * and output on 26 px cells (v1-5-split-inventory). */
    assert(v.view_count==1&&v.count==45);
    for(size_t j=0;j<36;j++){assert(v.slots[j].slot==v.main_base+j);assert(v.slots[j].x==272+(int)(j%9)*36);assert(v.slots[j].y==40+(int)(j/9)*36+(j>=27?4:0));}
    for(size_t j=36;j<v.count;j++)assert(v.slots[j].extent==26);
    for(size_t j=0;j<4;j++){
     assert(v.slots[36+j].slot==INVENTORY_SLOT_ARMOR+j&&v.slots[36+j].x==44&&v.slots[36+j].y==46+(int)j*26);
     assert(v.slots[40+j].slot==INVENTORY_SLOT_CRAFTING+j&&v.slots[40+j].x==138+(int)(j%2)*26&&v.slots[40+j].y==64+(int)(j/2)*26);
    }
    assert(v.slots[44].slot==INVENTORY_SLOT_OUTPUT&&v.slots[44].x==220&&v.slots[44].y==76);
   }else if(machine){
    size_t work=v.type==WINDOW_TYPE_FURNACE?3:9;assert(v.view_count==1&&v.count==work+36);
    for(size_t j=0;j<36;j++)assert(v.slots[work+j].slot==v.main_base+j);
   }else if(v.view == 0) {
    assert(v.count == 36);
				for(size_t j = 0; j < 36; j++) assert(v.slots[j].slot == v.main_base + j);
			}
			v.menu.selected = v.count - 1; /* Tab meaning never depends on focus. */
			struct container_input nav=page;if(machine)nav.pick=nav.drop=false;
   assert(!container_compact_step(&v, &wc, 7, &nav, .016F, capture_send, &calls));
			assert(v.slots[v.type==WINDOW_TYPE_INVENTORY?27:v.count-9].slot == v.main_base + 27);
		}
		assert(v.view == initial);
		struct container_input previous = {.previous = true};
		container_compact_step(&v, &wc, 7, &previous, .016F, capture_send, &calls);
		assert(v.view == (initial + v.view_count - 1) % v.view_count);
		struct container_input nav=page;if(machine)nav.pick=nav.drop=false;
  container_compact_step(&v, &wc, 7, &nav, .016F, capture_send, &calls);
		if(v.type==WINDOW_TYPE_INVENTORY) {
			/* Next is the full inventory's section jump (craft/armor -> main ->
			 * hotbar), never a page and never a click. */
			assert(v.view == initial && v.slots[v.menu.selected].slot >= v.main_base
				&& v.slots[v.menu.selected].slot < v.main_base + 27);
			for(size_t slot = 0; slot < sizes[k]; slot++) assert(seen[slot]);
			assert(!calls.clicks && !calls.drops && !calls.closes);
			windowc_destroy(&wc);
			continue; /* no header Previous/Next pointer targets any more */
		}
		assert(v.view == initial && v.menu.selected == v.count - 1);
		v.menu.focus.pointer_owner = true;
		struct container_input pointer_next = {.pointer_valid = true,
			.pointer_x = 400, .pointer_y = 24, .pick = true};
		/* Approved header: Next follows the measured title + 8 px (Astra ui1),
		 * the same rectangle the renderer and the pointer test use. */
		if(v.type == WINDOW_TYPE_CHEST) {
			/* Frozen Prev/title/Next, not the superseded right-side Next tab. */
			/* This input-only fixture has no loaded font; the presentation
			 * golden independently checks the real Beta pixel advances. */
			pointer_next.pointer_x = 76
				+ gutil_font_width(v.storage_count == 54 ? "Chest 1" : "Chest", 16) + 12;
		}
		container_compact_step(&v, &wc, 7, &pointer_next, .016F, capture_send, &calls);
		assert(v.view == (initial + 1) % v.view_count);
		for(size_t slot = 0; slot < sizes[k]; slot++) assert(seen[slot]);
		assert(!calls.clicks && !calls.drops && !calls.closes);
		windowc_destroy(&wc);
	}
	puts("PASS compact layouts: every slot reachable across pages, no overlap, mixed 28/32/36px slots");
}

static void test_two_view_owners(void) {
	struct container_compact a, b;
	struct window_container wa, wb;
	assert(windowc_create(&wa, WINDOW_TYPE_CHEST, 90));
	assert(windowc_create(&wb, WINDOW_TYPE_CHEST, 90));
	assert(container_compact_open(&a, wa.type, wa.slot_count, 0, 640, 240));
	assert(container_compact_open(&b, wb.type, wb.slot_count, 0, 640, 240));
	inventory_set_slot(windowc_get_latest(&wa), 81, (struct item_data) {.id = 1, .count = 9});
	inventory_set_slot(windowc_get_latest(&wb), 81, (struct item_data) {.id = 3, .count = 7});
	struct capture ca = {0}, cb = {0};
	struct container_input split = {.split = true};
	a.menu.selected = b.menu.selected = 27;
	container_compact_step(&a, &wa, 11, &split, .016F, capture_send, &ca);
	assert(windowc_get_latest(&wa)->picked_item.count == 5);
	assert(!inventory_get_picked_item(windowc_get_latest(&wb), NULL));
	container_compact_step(&b, &wb, 12, &split, .016F, capture_send, &cb);
	assert(windowc_get_latest(&wb)->picked_item.count == 4);
	assert(windowc_get_latest(&wa)->picked_item.id == 1);
	assert(windowc_get_latest(&wb)->picked_item.id == 3);
	assert(ca.last.payload.window_click.window == 11);
	assert(cb.last.payload.window_click.window == 12);
	struct container_compact b_before = b;
	/* Next always means the next labelled view, even over the hotbar. */
	struct container_input page = {.page = true, .pointer_valid = true,
		.pointer_x = 168, .pointer_y = 164};
	container_compact_step(&a, &wa, 11, &page, .016F, capture_send, &ca);
	assert(a.view == 2 && a.slots[0].slot == 27);
	assert(memcmp(&b, &b_before, sizeof(b)) == 0);
	assert(ca.clicks == 1 && cb.clicks == 1);
	assert(windowc_get_latest(&wa)->picked_item.count == 5);
	/* Full-framebuffer lower-half pointer coordinates must be translated first. */
	struct container_input outside = {.pointer_valid = true, .pointer_x = 48, .pointer_y = 320};
	container_compact_step(&a, &wa, 11, &outside, .016F, capture_send, &ca);
	assert(!a.menu.pointer_available);
	struct container_input close = {.close = true, .page = true, .pick = true};
	assert(!container_compact_step(&a, &wa, 11, &close, .016F, capture_send, &ca));
	assert(ca.closes == 0 && ca.clicks == 1 && cb.closes == 0);
	windowc_action_apply_result(&wa, ca.last.payload.window_click.action_id, true);
	struct container_input idle = {0};
	assert(!container_compact_step(&a, &wa, 11, &idle, .016F, capture_send, &ca));
	assert(ca.clicks == 2 && ca.last.payload.window_click.window == 11);
	windowc_action_apply_result(&wa, ca.last.payload.window_click.action_id, true);
	assert(container_compact_step(&a, &wa, 11, &idle, .016F, capture_send, &ca));
	assert(ca.closes == 1 && cb.closes == 0 && !ca.drops);
	windowc_destroy(&wa); windowc_destroy(&wb);
	puts("PASS two view owners: independent pages/focus/cursors, explicit window transport, Back priority");
}

static void test_owned_controller_menus(void) {
	const enum input_category families[] = {INPUT_CAT_NUNCHUK,
		INPUT_CAT_CLASSIC_CONTROLLER, INPUT_CAT_GAMECUBE};
	for(size_t k = 0; k < sizeof(families) / sizeof(families[0]); k++) {
		struct input_player_frame frame = {.connected = true, .category = families[k]};
		struct container_input input;
		frame.pressed[IB_GUI_SPLIT] = frame.pressed[IB_GUI_CLICK_ALT] = true;
		container_menu_read_player(&input, &frame, 0, 0, 640, 240);
		assert(input.split && !input.close && !input.quick_move);
		frame.pressed[IB_GUI_SPLIT] = frame.pressed[IB_GUI_CLICK_ALT] = false;
		frame.pressed[IB_GUI_QUICK_MOVE] = true;
		container_menu_read_player(&input, &frame, 0, 0, 640, 240);
		assert(input.quick_move && !input.close && !input.split);
		frame.pressed[IB_INVENTORY] = true;
		container_menu_read_player(&input, &frame, 0, 0, 640, 240);
		assert(input.close && input.quick_move && !input.split);
	}
	struct input_owner owners[2] = {0};
	assert(input_owner_bind(owners, 2, 0, (struct input_device){INPUT_DEVICE_WPAD, 0}));
	assert(input_owner_bind(owners, 2, 1, (struct input_device){INPUT_DEVICE_GAMECUBE, 0}));
	struct input_player_frame remote = {.connected = true, .category = INPUT_CAT_NUNCHUK};
	struct input_player_frame pad = {.connected = true, .category = INPUT_CAT_GAMECUBE};
	remote.glyphs[IB_GUI_QUICK_MOVE] = 8; remote.glyphs[IB_GUI_SPLIT] = 1;
	pad.glyphs[IB_GUI_QUICK_MOVE] = 34; pad.glyphs[IB_GUI_SPLIT] = 33;
	remote.glyphs[IB_INVENTORY] = 2; pad.glyphs[IB_INVENTORY] = 35;
	input_owner_latch(&owners[0], &remote); input_owner_latch(&owners[1], &pad);
	struct container_glyphs ga, gb;
	container_compact_player_glyphs(&ga, &owners[0].frame);
	container_compact_player_glyphs(&gb, &owners[1].frame);
	assert(ga.symbols[CONTAINER_PROMPT_QUICK] == 8 && gb.symbols[CONTAINER_PROMPT_QUICK] == 34);
	assert(ga.symbols[CONTAINER_PROMPT_SPLIT] == 1 && gb.symbols[CONTAINER_PROMPT_SPLIT] == 33);
	assert(ga.symbols[CONTAINER_PROMPT_BACK] == 2 && gb.symbols[CONTAINER_PROMPT_BACK] == 35);
	struct container_compact a, b;
	struct window_container wa, wb;
	assert(windowc_create(&wa, WINDOW_TYPE_CHEST, 90));
	assert(windowc_create(&wb, WINDOW_TYPE_CHEST, 90));
	assert(container_compact_open(&a, wa.type, wa.slot_count, 0, 640, 240));
	assert(container_compact_open(&b, wb.type, wb.slot_count, 0, 640, 240));
	inventory_set_slot(windowc_get_latest(&wa), 0, (struct item_data){.id = 1, .count = 9});
	inventory_set_slot(windowc_get_latest(&wb), 0, (struct item_data){.id = 3, .count = 7});
	struct capture ca = {0}, cb = {0};
	struct container_input ia, ib;
	remote.down[IB_GUI_SPLIT] = true;
	pad.down[IB_GUI_QUICK_MOVE] = true;
	input_owner_latch(&owners[0], &remote); input_owner_latch(&owners[1], &pad);
	container_menu_read_player(&ia, &owners[0].frame, 0, 0, 640, 240);
	container_menu_read_player(&ib, &owners[1].frame, 0, 240, 640, 240);
	assert(ia.split && !ia.quick_move && ib.quick_move && !ib.close && !ib.split);
	container_compact_step(&a, &wa, 11, &ia, .016F, capture_send, &ca);
	container_compact_step(&b, &wb, 12, &ib, .016F, capture_send, &cb);
	assert(windowc_get_latest(&wa)->picked_item.count == 5);
	assert(!inventory_get_picked_item(windowc_get_latest(&wb), NULL));
	assert(ca.last.type == SRPC_WINDOW_CLICK && ca.last.payload.window_click.window == 11);
	assert(cb.last.type == SRPC_WINDOW_CLICK && cb.last.payload.window_click.window == 12);
	assert(cb.last.payload.window_click.quick_move && cb.last.payload.window_click.slot == 0);
	struct container_compact before_b = b;
	remote.down[IB_GUI_SPLIT] = false; remote.down[IB_GUI_PAGE] = true;
	input_owner_latch(&owners[0], &remote);
	container_menu_read_player(&ia, &owners[0].frame, 0, 0, 640, 240);
	container_compact_step(&a, &wa, 11, &ia, .016F, capture_send, &ca);
	assert(a.view == 2 && !memcmp(&b, &before_b, sizeof(b)));
	assert(windowc_get_latest(&wa)->picked_item.count == 5);
	remote.connected = false;
	remote.down[IB_GUI_DROP] = remote.down[IB_GUI_CLICK_ALT] = true;
	input_owner_latch(&owners[0], &remote);
	container_compact_player_glyphs(&ga, &owners[0].frame);
	assert(ga.symbols[CONTAINER_PROMPT_QUICK] == 8
		&& ga.symbols[CONTAINER_PROMPT_SPLIT] == 1);
	container_menu_read_player(&ia, &owners[0].frame, 0, 0, 640, 240);
	assert(!container_compact_step(&a, &wa, 11, &ia, .016F, capture_send, &ca));
	remote.connected = true;
	input_owner_latch(&owners[0], &remote);
	container_menu_read_player(&ia, &owners[0].frame, 0, 0, 640, 240);
	assert(!container_compact_step(&a, &wa, 11, &ia, .016F, capture_send, &ca));
	assert(!ca.drops && !ca.closes && windowc_get_latest(&wa)->picked_item.count == 5);
	/* IR spans either viewport; a mouse cannot address the other half. */
	remote.pointer_valid = remote.pointer_normalized = true;
	remote.pointer_x = .25F; remote.pointer_y = .75F;
	input_owner_latch(&owners[0], &remote);
	container_menu_read_player(&ia, &owners[0].frame, 0, 240, 640, 240);
	assert(ia.pointer_valid && ia.pointer_x == 160 && ia.pointer_y == 180);
	remote.pointer_normalized = false; remote.pointer_x = 160; remote.pointer_y = 420;
	input_owner_latch(&owners[0], &remote);
	container_menu_read_player(&ia, &owners[0].frame, 0, 0, 640, 240);
	assert(!ia.pointer_valid);
	container_menu_read_player(&ia, NULL, 0, 0, 640, 240);
	assert(!ia.pick && !ia.drop && !ia.close && !ia.pointer_valid);
	container_compact_player_glyphs(&ga, NULL);
	for(int k = 0; k < CONTAINER_PROMPT_COUNT; k++) assert(ga.symbols[k] == -1);
	windowc_destroy(&wa); windowc_destroy(&wb);
	puts("PASS owned menus: Wii split + GC Quick Move route separately; tabs, carried stack and reconnect isolated");
}

static void test_safe_back_and_sections(void) {
	struct window_container wc;
	assert(windowc_create(&wc, WINDOW_TYPE_CHEST, 90));
	struct container_slot slots[90];
	for(size_t k = 0; k < 90; k++) slots[k] = (struct container_slot){0, 0, k, 0};
	struct container_menu menu = {.selected = 12};
	struct container_input next = {.page = true}, prev = {.previous = true};
	assert(container_menu_section_step(&menu, slots, 90, 54, &next) && menu.selected == 54);
	menu.selected = 65;
	assert(container_menu_section_step(&menu, slots, 90, 54, &next) && menu.selected == 81);
	menu.selected = 88;
	assert(container_menu_section_step(&menu, slots, 90, 54, &next) && menu.selected == 12);
	assert(container_menu_section_step(&menu, slots, 90, 54, &prev) && menu.selected == 88);
	assert(container_menu_section_step(&menu, slots, 90, 54, &prev) && menu.selected == 65);
	struct inventory* inv = windowc_get_latest(&wc);
	/* Close wins over a simultaneous Quick Move and waits for an already-sent
	 * transfer to be acknowledged before closing. It must not issue a second
	 * transfer while the close request is pending. */
	inv->items[65] = (struct item_data){.id = 1, .count = 9};
	struct capture calls = {0};
	struct container_input quick = {.quick_move = true};
	assert(!container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &quick, .016F,
		capture_send, &calls));
	assert(calls.clicks == 1 && calls.last.payload.window_click.quick_move
		&& wc.quick_pending && !calls.closes);
	uint16_t quick_action = calls.last.payload.window_click.action_id;
	struct container_input close_quick = {.close = true, .quick_move = true};
	assert(!container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &close_quick,
		.016F, capture_send, &calls));
	assert(menu.close_requested && calls.clicks == 1 && !calls.closes);
	assert(!container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &close_quick,
		.016F, capture_send, &calls));
	assert(calls.clicks == 1 && !calls.closes);
	windowc_action_apply_result(&wc, quick_action, true);
	struct container_input idle = {0};
	assert(container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &idle, .016F,
		capture_send, &calls));
	assert(calls.clicks == 1 && calls.closes == 1 && !calls.drops);

	menu = (struct container_menu){.selected = 65};
	calls = (struct capture){0};
	inv = windowc_get_latest(&wc);
	inventory_clear(inv);
	for(size_t k = 54; k < 90; k++) inv->items[k] = (struct item_data){.id = 3, .count = 64};
	inv->items[54] = (struct item_data){.id = 1, .count = 60};
	inv->picked_item = (struct item_data){.id = 1, .count = 9};
	struct container_input back = {.close = true, .drop = true, .pick = true,
		.quick_move = true};
	assert(!container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &back, .016F, capture_send, &calls));
	assert(menu.return_blocked && !calls.clicks && !calls.closes && !calls.drops);
	assert(inv->items[54].count == 60 && inv->picked_item.count == 9);
	inv->items[89] = (struct item_data){0};
	assert(!container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &back, .016F, capture_send, &calls));
	assert(calls.clicks == 1 && calls.last.payload.window_click.slot == 54
		&& !calls.last.payload.window_click.quick_move && !calls.drops);
	for(int i = 0; i < 5; i++)
		assert(!container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &idle, .016F, capture_send, &calls));
	assert(calls.clicks == 1 && !calls.closes);
	windowc_action_apply_result(&wc, calls.last.payload.window_click.action_id, true);
	assert(!container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &idle, .016F, capture_send, &calls));
	assert(calls.clicks == 2 && calls.last.payload.window_click.slot == 89
		&& !calls.last.payload.window_click.quick_move);
	windowc_action_apply_result(&wc, calls.last.payload.window_click.action_id, true);
	assert(container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &idle, .016F, capture_send, &calls));
	assert(calls.closes == 1 && !calls.drops);
	inv = windowc_get_latest(&wc);
	assert(inv->items[54].count == 64 && inv->items[89].count == 5 && !inv->picked_item.id);
	inv->picked_item = (struct item_data){.id = 1, .count = 2};
	container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &back, .016F, capture_send, &calls);
	windowc_action_apply_result(&wc, calls.last.payload.window_click.action_id, false);
	container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &idle, .016F, capture_send, &calls);
	assert(!menu.close_requested && menu.send_failed && calls.closes == 1);
	assert(windowc_get_latest(&wc)->picked_item.count == 2);
	container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &back, .016F, capture_send, &calls);
	struct container_input lost = {.suspended = true};
	container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &lost, .016F, capture_send, &calls);
	assert(menu.suspended && !menu.close_requested && calls.closes == 1 && !calls.drops);
	windowc_action_apply_result(&wc, calls.last.payload.window_click.action_id, true);
	container_menu_step(&menu, slots, 90, 0, 0, &wc, 3, &idle, .016F, capture_send, &calls);
	assert(calls.closes == 1);
	windowc_destroy(&wc);
	puts("PASS safe Back: Close beats Quick, waits pending ACK, capacity preflight, acknowledged returns, rejection/disconnect, section focus memory");
}

/* critique-1028 (betamc.org: inventory stuck, Close dead). Ordered capture so
 * the owed Packet106 ack is proven to precede the Packet101 close. */
struct ordered_capture { struct server_rpc rpc[8]; unsigned count; bool fail_ack; };
static bool ordered_send(void* user, struct server_rpc* rpc) {
	struct ordered_capture* out = user;
	if(out->fail_ack && rpc->type == SRPC_WINDOW_TRANSACTION) return false;
	assert(out->count < 8);
	out->rpc[out->count++] = *rpc;
	return true;
}

static void close_test_window(struct window_container* wc, enum window_type type,
	size_t count, struct container_slot* slots) {
	assert(windowc_create(wc, type, count));
	for(size_t k = 0; k < count; k++) slots[k] = (struct container_slot){0, 0, k, 0};
}

static void test_close_never_waits(void) {
	bool was_remote = gstate.remote_session;
	struct window_container wc;
	struct container_slot slots[63];
	struct container_input pick = {.pick = true}, close = {.close = true}, idle = {0};
	char label[128];

	/* Local authority (review M1): the acknowledged close is unchanged -- a
	 * pending correction still holds Close and no packet is sent. */
	gstate.remote_session = false;
	close_test_window(&wc, WINDOW_TYPE_INVENTORY, 45, slots);
	wc.correction_pending = true;
	struct container_menu menu = {.selected = 36};
	struct ordered_capture out = {0};
	assert(!container_menu_step(&menu, slots, 45, 0, 0, &wc, WINDOWC_INVENTORY, &close,
		.016F, ordered_send, &out));
	assert(!out.count && menu.close_requested);
	windowc_destroy(&wc);

	gstate.remote_session = true;
	/* A sent click the server never answered (NetServerHandler.java:499 drops
	 * it silently), plus a rejection still owing its correction and ack. */
	close_test_window(&wc, WINDOW_TYPE_INVENTORY, 45, slots);
	windowc_get_latest(&wc)->items[36] = (struct item_data){.id = 1, .count = 9};
	uint16_t action;
	assert(windowc_new_action(&wc, &action, false, 36));
	windowc_get_latest(&wc)->revision.sent = true;
	assert(windowc_get_latest(&wc)->picked_item.count == 9); /* predicted pickup */
	wc.correction_pending = wc.transaction_reply_pending = true;
	wc.transaction_reply_action = 7;

	/* Clicks are refused while the correction is owed: a wait, not a send failure. */
	menu = (struct container_menu){.selected = 36};
	assert(!container_menu_step(&menu, slots, 45, 0, 0, &wc, WINDOWC_INVENTORY, &pick,
		.016F, ordered_send, &out));
	container_menu_label(&menu, &wc, 36, label, sizeof label);
	assert(!out.count && menu.send_failed && menu.server_wait);
	assert(!strcmp(label, "Waiting for server..."));

	/* An ack that cannot be queued blocks the close honestly (never dropped). */
	out.fail_ack = true;
	assert(!container_menu_step(&menu, slots, 45, 0, 0, &wc, WINDOWC_INVENTORY, &close,
		.016F, ordered_send, &out));
	assert(!out.count && menu.send_failed && wc.transaction_reply_pending);
	container_menu_label(&menu, &wc, 36, label, sizeof label);
	assert(!strcmp(label, "Action not sent. Try again."));

	/* One Close press: ack first, then Packet101, on the same step. */
	out = (struct ordered_capture){0};
	assert(container_menu_step(&menu, slots, 45, 0, 0, &wc, WINDOWC_INVENTORY, &close,
		.016F, ordered_send, &out));
	assert(out.count == 2);
	assert(out.rpc[0].type == SRPC_WINDOW_TRANSACTION
		&& out.rpc[0].payload.window_transaction.window == WINDOWC_INVENTORY
		&& out.rpc[0].payload.window_transaction.action_id == 7
		&& out.rpc[0].payload.window_transaction.accepted);
	assert(out.rpc[1].type == SRPC_WINDOW_CLOSE
		&& out.rpc[1].payload.window_close.window == WINDOWC_INVENTORY);
	assert(!menu.close_requested && !menu.send_failed && !wc.transaction_reply_pending);
	/* Review H2: Beta's client keeps the click and clears its cursor on close
	 * (EntityClientPlayerMP.java:134); the server drops the held stack with no
	 * cursor packet (server Container.java:185-190). */
	assert(!windowc_actions_pending(&wc));
	assert(!windowc_get_latest(&wc)->picked_item.id && !windowc_get_latest(&wc)->items[36].id);
	/* A late accept of that click changes nothing (no slot update follows an
	 * accept, NetServerHandler.java:502-506); the stack never comes back. */
	windowc_action_apply_result(&wc, action, true);
	assert(!windowc_get_latest(&wc)->picked_item.id && !windowc_get_latest(&wc)->items[36].id);
	windowc_destroy(&wc);

	/* A rejection arriving after Close was pressed cannot cancel it. */
	close_test_window(&wc, WINDOW_TYPE_CHEST, 63, slots);
	windowc_get_latest(&wc)->picked_item = (struct item_data){.id = 1, .count = 3};
	menu = (struct container_menu){.selected = 0};
	out = (struct ordered_capture){0};
	assert(!container_menu_step(&menu, slots, 63, 0, 0, &wc, 3, &close,
		.016F, ordered_send, &out));
	assert(out.count == 1 && out.rpc[0].type == SRPC_WINDOW_CLICK && menu.close_requested);
	/* ...while its own return click is merely in flight, it still waits. */
	assert(!container_menu_step(&menu, slots, 63, 0, 0, &wc, 3, &idle,
		.016F, ordered_send, &out));
	assert(out.count == 1);
	windowc_action_apply_result(&wc, out.rpc[0].payload.window_click.action_id, false);
	assert(container_menu_step(&menu, slots, 63, 0, 0, &wc, 3, &idle,
		.016F, ordered_send, &out));
	assert(out.count == 2 && out.rpc[1].type == SRPC_WINDOW_CLOSE && !menu.send_failed);
	assert(!windowc_get_latest(&wc)->picked_item.id);
	windowc_destroy(&wc);

	/* Review H1: the ~5 s no-progress deadline COMMITS an unanswered click as
	 * Beta's client holds it; it holds while an ack is owed, restarts on any
	 * reply, and never guesses past a pending correction. */
	for(int late_accept = 0; late_accept < 2; late_accept++) {
		close_test_window(&wc, WINDOW_TYPE_INVENTORY, 45, slots);
		windowc_get_latest(&wc)->items[36] = (struct item_data){.id = 1, .count = 9};
		assert(windowc_new_action(&wc, &action, false, 36));
		windowc_get_latest(&wc)->revision.sent = true;
		ptime_t t0 = time_get();
		assert(!windowc_expire_unanswered(&wc, t0)); /* arms */
		assert(!windowc_expire_unanswered(&wc, time_add_ms(t0, WINDOWC_REPLY_TIMEOUT_MS - 1)));
		wc.transaction_reply_pending = true;
		assert(!windowc_expire_unanswered(&wc, time_add_ms(t0, WINDOWC_REPLY_TIMEOUT_MS * 3)));
		wc.transaction_reply_pending = false;
		ptime_t t1 = time_add_ms(t0, WINDOWC_REPLY_TIMEOUT_MS * 3);
		assert(!windowc_expire_unanswered(&wc, t1)); /* re-arms after the held ack */
		windowc_action_apply_result(&wc, 999, true); /* unknown id: still progress */
		assert(!windowc_expire_unanswered(&wc, time_add_ms(t1, WINDOWC_REPLY_TIMEOUT_MS)));
		assert(!windowc_expire_unanswered(&wc, time_add_ms(t1, WINDOWC_REPLY_TIMEOUT_MS * 2 - 1)));
		assert(windowc_expire_unanswered(&wc, time_add_ms(t1, WINDOWC_REPLY_TIMEOUT_MS * 2)));
		struct inventory* inv = windowc_get_latest(&wc);
		assert(!windowc_actions_pending(&wc) && !wc.quick_pending);
		assert(inv->picked_item.count == 9 && !inv->items[36].id); /* kept, not rolled back */
		uint32_t serial = wc.rejection_serial;
		windowc_action_apply_result(&wc, action, late_accept);
		inv = windowc_get_latest(&wc);
		if(late_accept) {
			/* Late accept: no slot update follows; the committed state stands. */
			assert(wc.rejection_serial == serial);
			assert(inv->picked_item.count == 9 && !inv->items[36].id);
		} else {
			/* Late reject: recorded, and the server's Packet104 + Packet103(-1,-1)
			 * replace the state (EntityPlayerMP.java:322-325). */
			assert(wc.rejection_serial == serial + 1);
			wc.correction_pending = true;
			windowc_slot_change(&wc, 36, (struct item_data){.id = 1, .count = 9});
			windowc_slot_change(&wc, SPECIAL_SLOT_PICKED_ITEM, (struct item_data){0});
			inv = windowc_get_latest(&wc);
			assert(inv->items[36].count == 9 && !inv->picked_item.id);
		}
		windowc_destroy(&wc);
	}
	close_test_window(&wc, WINDOW_TYPE_INVENTORY, 45, slots);
	wc.correction_pending = true;
	ptime_t t2 = time_get();
	for(int k = 0; k < 4; k++)
		assert(!windowc_expire_unanswered(&wc, time_add_ms(t2, WINDOWC_REPLY_TIMEOUT_MS * 3 * k)));
	assert(wc.correction_pending); /* clicks stay blocked until the 0x67 lands */
	windowc_destroy(&wc);
	gstate.remote_session = was_remote;
	puts("PASS close never waits (remote): ack precedes Packet101 on one press, cursor cleared, "
		"rejection cannot cancel, local close unchanged, 5 s deadline commits, late accept/reject");
}

static void test_session_replies(void) {
	struct client_session players[2] = {0};
	struct capture requests[2] = {0};
	for(unsigned p = 0; p < 2; p++) {
		const uint8_t ids[] = {0, 17, 18};
		const enum window_type types[] = {WINDOW_TYPE_INVENTORY, WINDOW_TYPE_CHEST, WINDOW_TYPE_FURNACE};
		const size_t sizes[] = {45, 90, 39};
		for(unsigned k = 0; k < 3; k++) {
			players[p].windows[ids[k]] = malloc(sizeof(struct window_container));
			assert(players[p].windows[ids[k]]);
			assert(windowc_create(players[p].windows[ids[k]], types[k], sizes[k]));
		}
		players[p].visible_window = 17;
		players[p].camera.x = 10 + p;
		assert(container_compact_open(&players[p].container, WINDOW_TYPE_CHEST, 90, 0, 640, 240));
		struct client_rpc item = {.type = CRPC_INVENTORY_SLOT,
			.payload.inventory_slot = {.window = 17, .slot = 0,
				.item = {.id = p ? 3 : 1, .count = p ? 7 : 9}}};
		assert(client_session_window_reply(&players[p], &item));
		struct container_input quick = {.quick_move = true};
		container_compact_step(&players[p].container, players[p].windows[17], 17,
			&quick, .016F, capture_send, &requests[p]);
		assert(players[p].windows[17]->quick_pending);
	}
	/* Equal window/action ids are normal across players. Only the intended
	 * owner may consume the reply or clear its pending transaction. */
	assert(requests[0].last.payload.window_click.action_id == requests[1].last.payload.window_click.action_id);
	assert(requests[0].last.payload.window_click.expected.count == 9);
	assert(requests[1].last.payload.window_click.expected.count == 7);
	struct client_rpc ack = {.type = CRPC_WINDOW_TRANSACTION,
		.payload.window_transaction = {.window = 17, .accepted = true,
			.action_id = requests[0].last.payload.window_click.action_id}};
	assert(client_session_window_reply(&players[0], &ack));
	assert(!players[0].windows[17]->quick_pending && players[1].windows[17]->quick_pending);
	ack.payload.window_transaction.accepted = false;
	assert(client_session_window_reply(&players[1], &ack));
	assert(!players[1].windows[17]->quick_pending);
	struct client_rpc item = {.type = CRPC_INVENTORY_SLOT,
		.payload.inventory_slot = {.window = 17, .slot = 81, .item = {.id = 4, .count = 23}}};
	assert(client_session_window_reply(&players[1], &item));
	assert(windowc_get_latest(players[1].windows[0])->items[36].count == 23);
	assert(!windowc_get_latest(players[0].windows[0])->items[36].id);
	item.payload.inventory_slot.slot = SPECIAL_SLOT_PICKED_ITEM;
	assert(client_session_window_reply(&players[1], &item));
	assert(windowc_get_latest(players[1].windows[17])->picked_item.count == 23);
	assert(!windowc_get_latest(players[0].windows[17])->picked_item.id);
	assert(!windowc_get_latest(players[1].windows[0])->picked_item.id);
	item.payload.inventory_slot.slot = 254; /* malformed non-cursor slot */
	assert(client_session_window_reply(&players[1], &item));
	struct client_rpc progress = {.type = CRPC_WINDOW_PROGRESS,
		.payload.window_progress = {.window = 18, .cook_time = 77, .burn_time = 88,
			.current_item_burn_time = 99}};
	assert(client_session_window_reply(&players[1], &progress));
	assert(players[1].windows[18]->furnace_cook_time == 77);
	assert(players[0].windows[18]->furnace_cook_time == 0);
	struct client_rpc batch = {.type = CRPC_WINDOW_ITEMS,
		.payload.window_items = {.window = 18, .count = UINT16_MAX}};
	batch.payload.window_items.items[30] = (struct item_data){.id = 1, .count = 12};
	assert(client_session_window_reply(&players[0], &batch));
	assert(windowc_get_latest(players[0].windows[0])->items[36].count == 12);
	assert(windowc_get_latest(players[1].windows[0])->items[36].count == 23);
	struct client_rpc hotbar = {.type = CRPC_PLAYER_HOTBAR_SLOT, .payload.player_hotbar_slot.slot = 8};
	assert(client_session_window_reply(&players[1], &hotbar));
	assert(windowc_get_latest(players[1].windows[0])->hotbar_slot == 8);
	assert(windowc_get_latest(players[0].windows[0])->hotbar_slot == 0);
	hotbar.payload.player_hotbar_slot.slot = 255;
	assert(client_session_window_reply(&players[1], &hotbar));
	assert(windowc_get_latest(players[1].windows[0])->hotbar_slot == 8);
	struct client_rpc world = {.type = CRPC_WORLD_RESET};
	assert(!client_session_window_reply(&players[1], &world));
	client_session_clear_windows(&players[0]);
	client_session_clear_windows(&players[0]);
	for(unsigned k = 0; k < 256; k++) assert(!players[0].windows[k]);
	assert(players[0].visible_window == -1 && players[0].camera.x == 10);
	assert(players[1].visible_window == 17 && players[1].container.active);
	assert(windowc_get_latest(players[1].windows[17])->picked_item.count == 23);
	client_session_clear_windows(&players[1]);
	puts("PASS player sessions: equal window/action ids, inventory mirrors, replies and teardown stay with their owner");
}

static void test_compact_screen_adapter(void) {
	/* The regular screen chooses compact only for a short viewport. */
	screen_set(&screen_inventory);
	screen_inventory.reset(&screen_inventory, 640, 240);
	px = py = -1;
	press(&screen_inventory, "F");
	press(&screen_inventory, " ");
	struct capture c = drain();
	/* T13: the split inventory is ONE page, so Next is the same section jump
	 * as full (hotbar -> armor/craft), not a page change. */
	assert(c.clicks == 1 && c.last.payload.window_click.slot == 5);
	windowc_discard_pending_actions(gstate.players[0].windows[WINDOWC_INVENTORY]);
	screen_set(&screen_inventory); /* restore normal 640x480 */
	press(&screen_inventory, "F");
	press(&screen_inventory, " ");
	c = drain();
	assert(c.clicks == 1 && c.last.payload.window_click.slot == 5);
	windowc_discard_pending_actions(gstate.players[0].windows[WINDOWC_INVENTORY]);
	puts("PASS viewport adapter: compact and full both jump sections without hiding slots");
}

static bool enqueue_click(void* user, struct server_rpc* rpc) {
	(void)user;
	return svin_rpc_send(rpc);
}

static void test_unsent_revision(void) {
	struct window_container wc;
	assert(windowc_create(&wc, WINDOW_TYPE_WORKBENCH, 46));
	struct container_menu menu = {0};
	const struct container_slot slot = {160,44,10,0};
	struct container_input pick = {.pick = true};
	windowc_slot_change(&wc, 10, (struct item_data){.id=1,.count=9});
	struct window_container* saved = gstate.players[0].windows[1];
	gstate.players[0].windows[1] = &wc;
	/* Retain the actual initial base 0. A whole uint16 range of failed
	 * enqueues must not consume action 1 or alias that base on the next ACK. */
	assert(windowc_get_latest(&wc)->revision.action_id == 0);
	struct server_rpc filler = {.type=SRPC_WINDOW_CLICK};
	unsigned queued = 0;
	while(svin_rpc_send(&filler)) assert(++queued < 4096);
	for(unsigned i = 0; i < 65535; i++) {
		container_menu_step(&menu, &slot, 1, 0, 0, &wc, 1, &pick, .016F, enqueue_click, NULL);
		assert(menu.send_failed);
		assert(windowc_get_latest(&wc)->items[10].count == 9);
		assert(!windowc_get_latest(&wc)->picked_item.id);
		assert(wc.next_action_id == 1 && windowc_revision_count(&wc) == 1);
		assert(windowc_get_latest(&wc)->revision.action_id == 0);
	}
	(void)drain();
	while(svin_pending_count()) { svin_rpc_flush_pending(); (void)drain(); }
	/* Send A through the real transport, then fail B against its full queue. */
	container_menu_step(&menu, &slot, 1, 0, 0, &wc, 1, &pick, .016F, enqueue_click, NULL);
	uint16_t a = windowc_get_latest(&wc)->revision.action_id;
	assert(a == 1 && windowc_revision_count(&wc) == 2);
	assert(windowc_get_latest(&wc)->revision.sent);
	windowc_cancel_unsent(&wc, a);
	assert(wc.next_action_id == 2 && windowc_revision_count(&wc) == 2);
	struct capture sent = drain();
	assert(sent.clicks == 1 && sent.last.payload.window_click.action_id == a);
	queued = 0;
	while(svin_rpc_send(&filler)) assert(++queued < 4096);
	container_menu_step(&menu, &slot, 1, 0, 0, &wc, 1, &pick, .016F, enqueue_click, NULL);
	assert(menu.send_failed && windowc_revision_count(&wc) == 2);
	assert(windowc_get_latest(&wc)->revision.action_id == a && wc.next_action_id == 2);
	struct client_rpc ack = {.type=CRPC_WINDOW_TRANSACTION,
		.payload.window_transaction={.window=1,.action_id=a,.accepted=true}};
	clin_rpc_send(&ack); clin_update_drain();
	assert(!windowc_actions_pending(&wc) && wc.next_action_id == 2);
	assert(!windowc_get_latest(&wc)->items[10].id && windowc_get_latest(&wc)->picked_item.count == 9);
	(void)drain();
	while(svin_pending_count()) { svin_rpc_flush_pending(); (void)drain(); }
	/* Rejection/resync in THIS history cannot reclaim either sent ID. */
	container_menu_step(&menu, &slot, 1, 0, 0, &wc, 1, &pick, .016F, enqueue_click, NULL);
	sent = drain();
	assert(sent.clicks == 1 && sent.last.payload.window_click.action_id == 2);
	ack.payload.window_transaction.action_id = 2;
	ack.payload.window_transaction.accepted = false;
	clin_rpc_send(&ack); clin_update_drain();
	assert(wc.correction_pending && wc.next_action_id == 3);
	uint16_t blocked;
	assert(!windowc_new_action(&wc, &blocked, false, 10));
	sent = drain();
	assert(sent.last.type == SRPC_WINDOW_TRANSACTION);
	assert(sent.last.payload.window_transaction.action_id == 2);
	struct client_rpc resync = {.type=CRPC_WINDOW_ITEMS,
		.payload.window_items={.window=1,.count=46}};
	clin_rpc_send(&resync);
	struct client_rpc cursor = {.type=CRPC_INVENTORY_SLOT,
		.payload.inventory_slot={.window=1,.slot=SPECIAL_SLOT_PICKED_ITEM,
			.item={.id=1,.count=9}}};
	clin_rpc_send(&cursor); clin_update_drain();
	assert(!wc.correction_pending && !wc.transaction_reply_pending && wc.next_action_id == 3);
	container_menu_step(&menu, &slot, 1, 0, 0, &wc, 1, &pick, .016F, enqueue_click, NULL);
	sent = drain();
	assert(sent.clicks == 1 && sent.last.payload.window_click.action_id == 3);
	ack.payload.window_transaction.action_id = 3;
	ack.payload.window_transaction.accepted = true;
	clin_rpc_send(&ack); clin_update_drain();
	assert(!windowc_actions_pending(&wc) && wc.next_action_id == 4);
	/* The wrap predecessor is really enqueued, observed and ACKed too. */
	wc.next_action_id = UINT16_MAX;
	container_menu_step(&menu, &slot, 1, 0, 0, &wc, 1, &pick, .016F, enqueue_click, NULL);
	sent = drain();
	assert(sent.clicks == 1 && sent.last.payload.window_click.action_id == UINT16_MAX);
	uint16_t unsent;
	assert(windowc_new_action(&wc, &unsent, false, 10) && unsent == 0);
	windowc_cancel_unsent(&wc, UINT16_MAX); // non-LIFO cannot reclaim sent predecessor
	assert(windowc_get_latest(&wc)->revision.action_id == 0);
	windowc_cancel_unsent(&wc, unsent);
	assert(wc.next_action_id == 0 && windowc_get_latest(&wc)->revision.action_id == UINT16_MAX);
	ack.payload.window_transaction.action_id = UINT16_MAX;
	clin_rpc_send(&ack); clin_update_drain();
	assert(!windowc_actions_pending(&wc) && wc.next_action_id == 0);
	container_menu_step(&menu, &slot, 1, 0, 0, &wc, 1, &pick, .016F, enqueue_click, NULL);
	sent = drain();
	assert(sent.clicks == 1 && sent.last.payload.window_click.action_id == 0);
	ack.payload.window_transaction.action_id = 0;
	clin_rpc_send(&ack); clin_update_drain();
	assert(!windowc_actions_pending(&wc) && wc.next_action_id == 1);
	queued = 0;
	while(svin_rpc_send(&filler)) assert(++queued < 4096);
	windowc_slot_change(&wc, SPECIAL_SLOT_PICKED_ITEM, (struct item_data){0});
	windowc_slot_change(&wc, 0, (struct item_data){.id=358,.count=1});
	for(size_t i = 1; i <= 9; i++) windowc_slot_change(&wc, i,
		(struct item_data){.id=i == 5 ? 345 : 339,.count=1});
	const struct container_slot output = {160,44,0,0};
	for(unsigned quick = 0; quick < 2; quick++) {
		for(unsigned wrap = 0; wrap < 2; wrap++) {
			wc.map_counter = wrap ? UINT16_MAX : 0;
			struct container_input craft = {.pick=true,.quick_move=quick};
			container_menu_step(&menu, &output, 1, 0, 0, &wc, 1, &craft, .016F, enqueue_click, NULL);
			assert(menu.send_failed && !windowc_actions_pending(&wc) && !wc.quick_pending);
			assert(wc.map_counter == (wrap ? UINT16_MAX : 0));
			assert(windowc_get_latest(&wc)->items[5].count == 1);
			assert(!windowc_get_latest(&wc)->picked_item.id);
		}
	}
	(void)drain();
	while(svin_pending_count()) { svin_rpc_flush_pending(); (void)drain(); }
	gstate.players[0].windows[1] = saved;
	windowc_destroy(&wc);
	puts("PASS base 0 + 65535 cancellations; real send/ACK/wrap/rejection/resync; enqueue A, saturated B, ACK A preserves A; unsent normal/quick map craft restores counter including wrap");
}

/* REL-11-seam: accept_serial counts accepted replies only; rejections, unknown
 * accepted ids and pending-action discards (position corrections) leave it. */
static void test_accept_serial(void) {
	struct window_container wc;
	assert(windowc_create(&wc, WINDOW_TYPE_CHEST, 63));
	assert(wc.accept_serial == 0);
	uint16_t a, b, c;
	assert(windowc_new_action(&wc, &a, false, 0));
	assert(windowc_new_action(&wc, &b, false, 0));
	windowc_action_apply_result(&wc, a, true);
	assert(wc.accept_serial == 1 && wc.rejection_serial == 0);
	windowc_action_apply_result(&wc, 0x7ff0, true); /* untracked id: ignored */
	assert(wc.accept_serial == 1);
	windowc_action_apply_result(&wc, b, false);
	assert(wc.accept_serial == 1 && wc.rejection_serial == 1);
	assert(windowc_new_action(&wc, &c, false, 0));
	windowc_discard_pending_actions(&wc); /* correction handoff */
	assert(wc.accept_serial == 1);
	assert(windowc_new_action(&wc, &c, false, 0));
	windowc_action_apply_result(&wc, c, true);
	assert(wc.accept_serial == 2);
	windowc_destroy(&wc);
	puts("PASS accept_serial: bumps on accepted replies only, survives rejection and correction");
}

static void test_failed_send(void) {
	struct window_container* wc = gstate.players[0].windows[WINDOWC_INVENTORY];
	struct inventory* inv = windowc_get_latest(wc);
	inventory_set_slot(inv, 36, (struct item_data) {.id = 1, .count = 9});
	screen_set(&screen_inventory);
	px = py = -1;
	struct server_rpc filler = {.type = SRPC_WINDOW_CLICK};
	unsigned queued = 0;
	while(svin_rpc_send(&filler)) {
		assert(++queued < 4096);
	}
	press(&screen_inventory, " ");
	assert(windowc_revision_count(wc) == 1);
	assert(!inventory_get_picked_item(windowc_get_latest(wc), NULL));
	assert(windowc_get_latest(wc)->items[36].count == 9);
	(void)drain();
	while(svin_pending_count()) { svin_rpc_flush_pending(); (void)drain(); }
	puts("PASS full request queue: rejected click rolls back its client revision");
}

static void test_primary_controller_recovery(void) {
	input_pointer_enable(false); /* Keyboard test; Wii driver coverage is separate. */
	struct entity player = {0};
	gstate.players[0].local_player = &player;
	gstate.paused = false;
	assert(input_player_bind(0, (struct input_device){INPUT_DEVICE_KEYBOARD, 0}));
	memset(keys, 0, sizeof(keys)); input_poll();
	struct screen* original = gstate.players[0].current_screen;
	window = NULL; input_poll();
	screen_controller_recovery();
	assert(gstate.paused && gstate.players[0].current_screen == original);
	struct capture c = drain(); assert(c.pauses == 1);
	screen_controller_recovery();
	c = drain(); assert(!c.pauses && !c.clicks && !c.drops && !c.closes);
	struct container_input in;
	container_menu_read_input(&in, 0, 0, 640, 480);
	assert(in.suspended && !in.close && !in.drop);
	/* The controller comes back. Approved rule: STAY paused -- reconnection
	 * requires neutral input and then a fresh Resume action. The base tree
	 * resumed right here, which is what this assertion now forbids. */
	window = (GLFWwindow*)1; input_poll();
	screen_controller_recovery();
	assert(gstate.paused && gstate.players[0].current_screen == original);
	c = drain(); assert(!c.pauses);
	assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_READY);
	/* Idling at the open gate never resumes on its own. */
	for(unsigned f = 0; f < 8; f++) {
		input_poll(); screen_controller_recovery();
		c = drain(); assert(!c.pauses && !c.clicks && !c.drops && !c.closes);
	}
	assert(gstate.paused);
	/* A fresh deliberate Resume press, and only then. */
	keys[32] = true; input_poll();
	assert(input_player_frame(0)->pressed[IB_GUI_CLICK]);
	screen_controller_recovery();
	assert(!gstate.paused && gstate.players[0].current_screen == original);
	c = drain(); assert(c.pauses == 1);
	/* ...and the menu retained underneath never sees that press. */
	assert(!input_player_frame(0)->pressed[IB_GUI_CLICK]
		   && !input_player_frame(0)->down[IB_GUI_CLICK]);
	container_menu_read_input(&in, 0, 0, 640, 480);
	assert(!in.pick && !in.close && !in.drop);
	keys[32] = false; input_poll();
	assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_NONE);
	input_player_unbind(0);
	gstate.players[0].local_player = NULL;
	screen_controller_recovery();
	puts("PASS primary recovery: pause on loss, STAYS paused through the "
		 "reconnect, resumes only on a fresh press the menu never sees");
}


static void test_manual_grid_owned(void) {
 struct container_glyphs glyphs;container_compact_active_glyphs(&glyphs);
 for(unsigned grid=2;grid<=3;grid++)for(int height=240;height<=480;height+=240) {
  struct window_container wc;size_t capacity=grid==2?45:46,base=capacity-36;
  assert(windowc_create(&wc,grid==2?WINDOW_TYPE_INVENTORY:WINDOW_TYPE_WORKBENCH,capacity));
  struct manual_grid v;manual_grid_open(&v,grid,640,height);
  assert(v.count==grid*grid+1+(height==480?36:27));
  bool seen[46]={0};for(size_t k=0;k<v.count;k++){assert(v.slots[k].slot<capacity&&!seen[v.slots[k].slot]);seen[v.slots[k].slot]=true;}
  assert(v.slots[0].x==(grid==2?62:44));assert(v.slots[0].y==(height==480?142:62)+(grid==2?18:0));
  assert(v.slots[grid*grid].slot==0&&v.slots[grid*grid].extent==52);
  for(size_t k=base;k<base+(height==480?36:18);k++)assert(seen[k]);
  if(grid==2)for(int k=5;k<9;k++)assert(!seen[k]);
  struct capture calls={0};struct container_input idle={0};
  if(height==240) {
   v.menu.selected=2;struct container_input page={.page=true,.pick=true};
   assert(!manual_grid_step(&v,&wc,7,&page,&glyphs,.016F,capture_send,&calls));
   assert(v.page==1&&v.count==grid*grid+19&&!calls.clicks&&!calls.closes);
   struct container_input blank={.pointer_valid=true,.pointer_x=270,.pointer_y=120};
   manual_grid_step(&v,&wc,7,&blank,&glyphs,.016F,capture_send,&calls);
   blank.pointer_x=290;blank.pick=true;
   manual_grid_step(&v,&wc,7,&blank,&glyphs,.016F,capture_send,&calls);
   assert(!calls.clicks&&!calls.closes&&!v.menu.sort_open&&!v.menu.sort_focus);
   page.pick=false;manual_grid_step(&v,&wc,7,&page,&glyphs,.016F,capture_send,&calls);assert(v.page==1&&!calls.clicks);
   page=(struct container_input){.previous=true};
   manual_grid_step(&v,&wc,7,&page,&glyphs,.016F,capture_send,&calls);assert(v.page==0&&v.menu.selected==2);
   manual_grid_step(&v,&wc,7,&page,&glyphs,.016F,capture_send,&calls);assert(v.page==0&&!calls.clicks);
  }
  /* A stale pointer over the Recipes footer cannot override controller focus. */
  v.menu.focus=(struct gutil_menu_focus){.initialized=true,.pointer_valid=true,.x=560,.y=height==480?350:200};
  struct container_input select={.pick=true,.pointer_valid=true,.pointer_x=560,.pointer_y=height==480?350:200};
  /* DOWN on BOTH heights (Fable G4 review item 5): without it the 240 pass
   * never makes the ring directional and so never tests this rule. */
  {struct container_input direction=select;direction.pick=false;direction.down=true;
   manual_grid_step(&v,&wc,7,&direction,&glyphs,.016F,capture_send,&calls);
   assert(v.menu.selection_origin==GUTIL_MENU_ORIGIN_DIRECTIONAL);}
  manual_grid_step(&v,&wc,7,&select,&glyphs,.016F,capture_send,&calls);
  assert(v.active&&calls.clicks==1&&!calls.closes);windowc_discard_pending_actions(&wc);calls=(struct capture){0};
  /* Return needs two acknowledged clicks; it must never close the window. */
  struct inventory* inv=windowc_get_latest(&wc);inventory_clear(inv);
  for(size_t k=base;k<capacity;k++)inv->items[k]=(struct item_data){.id=3,.count=64};
  inv->items[base]=(struct item_data){.id=1,.count=60};inv->picked_item=(struct item_data){.id=1,.count=9};
  struct container_input back={.close=true,.pick=true,.drop=true};
  assert(!manual_grid_step(&v,&wc,7,&back,&glyphs,.016F,capture_send,&calls));assert(v.active&&v.menu.return_blocked&&!calls.clicks);
  inv->items[capacity-1]=(struct item_data){0};
  assert(!manual_grid_step(&v,&wc,7,&back,&glyphs,.016F,capture_send,&calls));assert(calls.clicks==1&&v.active&&!calls.closes);
  assert(!manual_grid_step(&v,&wc,7,&idle,&glyphs,.016F,capture_send,&calls));assert(calls.clicks==1);
  windowc_action_apply_result(&wc,calls.last.payload.window_click.action_id,true);
  assert(!manual_grid_step(&v,&wc,7,&idle,&glyphs,.016F,capture_send,&calls));assert(calls.clicks==2);
  windowc_action_apply_result(&wc,calls.last.payload.window_click.action_id,true);
  assert(manual_grid_step(&v,&wc,7,&idle,&glyphs,.016F,capture_send,&calls));assert(!v.active&&!calls.closes&&!calls.drops);
  inv=windowc_get_latest(&wc);assert(!inv->picked_item.id&&inv->items[base].count==64&&inv->items[capacity-1].count==5);
  windowc_destroy(&wc);
 }
 puts("PASS Manual Grid: exact slots, 2x2/3x3, paged blanks, pointer ownership, acknowledged cursor return without close");
}

/* The Manual Grid footer's POINTER hit targets follow the VISIBLE row
 * (Astra ruling 6 Q1). Empty cursor, cell 2 reads "Quick Move"; holding a
 * stack, the same cell reads "Drop" -- and must then DROP, never quick-move.
 * The geometry comes from the shipping helper itself, so what is clicked is
 * what is drawn.
 *
 * Quick Move's BINDING is not touched by its prompt leaving the row: the
 * footer only overrides actions when a cell was actually hit. Proven below
 * by pressing the binding with no pointer over the row -- the engine's own
 * rule (container_menu.c refuses a quick move while the cursor holds a
 * stack, unchanged Beta behaviour) makes the held case a no-op, so the
 * binding is shown live on the same manual grid once the cursor is empty. */
extern size_t manual_grid_footer_probe(const struct manual_grid* v,
	const struct container_glyphs* g, bool held,
	struct gutil_button_bounds bounds[4], int symbols[4],
	const char* labels[4]);

static void manual_grid_footer_click(struct manual_grid* v,
									 struct window_container* wc,
									 const struct container_glyphs* glyphs,
									 bool held, int cell,
									 struct capture* calls) {
	struct gutil_button_bounds b[4];
	int symbols[4];
	const char* labels[4];
	assert(manual_grid_footer_probe(v, glyphs, held, b, symbols, labels) == 4);
	float px = (float)(b[cell].x + b[cell].width / 2);
	float py = (float)(b[cell].y + b[cell].height / 2);
	/* The pointer must OWN the focus for a footer click to count at all
	 * (gutil_menu_pointer_focus: a stale pointer that has not moved at least
	 * 4 * GFX_GUI_SCALE never takes it -- that is the property
	 * test_manual_grid_owned pins from the other side). So: one frame that
	 * parks the pointer 20 px left of the cell, then the frame that moves it
	 * onto the cell and clicks. */
	v->menu.focus = (struct gutil_menu_focus) {.initialized = true};
	struct container_input park = {.pointer_valid = true,
		.pointer_x = px - 20.0F, .pointer_y = py};
	manual_grid_step(v, wc, 7, &park, glyphs, .016F, capture_send, calls);
	struct container_input in = {.pick = true, .pointer_valid = true,
		.pointer_x = px, .pointer_y = py};
	manual_grid_step(v, wc, 7, &in, glyphs, .016F, capture_send, calls);
}

static void test_manual_grid_footer_actions(void) {
	struct container_glyphs glyphs;
	container_compact_active_glyphs(&glyphs);
	struct window_container wc;
	assert(windowc_create(&wc, WINDOW_TYPE_WORKBENCH, 46));
	struct inventory* inv = windowc_get_latest(&wc);
	inventory_clear(inv);
	for(size_t k = 10; k < 46; k++)
		inv->items[k] = (struct item_data) {.id = 3, .count = 64};
	struct manual_grid v;
	manual_grid_open(&v, 3, 640, 480);
	v.menu.selected = 10; /* the first carried main slot, not a grid cell */

	/* ---- EMPTY cursor: cell 2 is Quick Move, and nothing is dropped ---- */
	struct capture calls = {0};
	manual_grid_footer_click(&v, &wc, &glyphs, false, 2, &calls);
	assert(!calls.drops && "the empty row's third cell dropped a stack");
	assert(calls.clicks == 1 && calls.last.payload.window_click.quick_move
		   && "the empty row's third cell did not quick-move");
	windowc_discard_pending_actions(&wc);

	/* ---- HOLDING: the same cell is Drop -- drop, never quick-move ----- */
	calls = (struct capture) {0};
	inv = windowc_get_latest(&wc);
	inv->picked_item = (struct item_data) {.id = 1, .count = 9};
	manual_grid_footer_click(&v, &wc, &glyphs, true, 2, &calls);
	assert(calls.drops == 1 && "the holding row's third cell did not drop");
	assert(!calls.clicks
		   && "the holding row's third cell quick-moved instead of dropping");
	assert(calls.last.type == SRPC_WINDOW_DROP);
	windowc_discard_pending_actions(&wc);

	/* ---- the Quick Move BINDING is still live -------------------------
	 * Pressed with the pointer nowhere near the footer, so no cell is hit
	 * and manual_grid_step overrides nothing. While the cursor holds a
	 * stack container_menu.c refuses the transfer itself (Beta), so the
	 * observable proof that the binding was never filtered out is that the
	 * very same press transfers once the cursor is put down -- no reopen,
	 * no rebind, no re-arm in between. */
	calls = (struct capture) {0};
	v.menu.focus = (struct gutil_menu_focus) {.initialized = true};
	struct container_input quick = {.quick_move = true};
	manual_grid_step(&v, &wc, 7, &quick, &glyphs, .016F, capture_send, &calls);
	assert(!calls.drops && !calls.closes
		   && "a held-cursor quick move was re-routed to another action");
	windowc_get_latest(&wc)->picked_item = (struct item_data) {0};
	manual_grid_step(&v, &wc, 7, &quick, &glyphs, .016F, capture_send, &calls);
	assert(calls.clicks == 1 && calls.last.payload.window_click.quick_move
		   && "the Quick Move binding died with its prompt");
	windowc_discard_pending_actions(&wc);
	windowc_destroy(&wc);
	puts("PASS Manual Grid footer actions: the third cell follows the visible "
		 "row (Quick Move when empty, Drop when holding) and the Quick Move "
		 "binding outlives its prompt");
}

static void test_inventory_portrait_look(void) {
 struct container_compact a={.width=640,.height=480,.count=1};
 a.menu.pointer_available=true;a.menu.focus.pointer_owner=true;
 a.menu.pointer_x=268;a.menu.pointer_y=97;
 float pitch,yaw;
 container_inventory_look(&a,&pitch,&yaw);
 assert(pitch==0&&yaw==0);
 a.menu.pointer_x=460;a.menu.pointer_y=289;
 container_inventory_look(&a,&pitch,&yaw);
 assert(fabsf(yaw-0.39269908F)<0.000001F);
 assert(fabsf(pitch-0.25918139F)<0.000001F);
 a.menu.pointer_x=76;a.menu.pointer_y=-95;
 container_inventory_look(&a,&pitch,&yaw);
 assert(yaw<0&&pitch<0);
 /* Keyboard/stick focus wins over a stale pointer and looks at the slot. */
 a.menu.focus.pointer_owner=false;
 a.slots[0]=(struct container_slot){.x=400,.y=200};
 container_inventory_look(&a,&pitch,&yaw);
 assert(yaw>0&&pitch>0);
 a.menu.pointer_available=false;a.menu.focus.pointer_owner=true;
 float fallback_pitch,fallback_yaw;
 container_inventory_look(&a,&fallback_pitch,&fallback_yaw);
 assert(pitch==fallback_pitch&&yaw==fallback_yaw);
 /* Compact and centered widescreen views use their own local coordinates. */
 struct container_compact b={.width=800,.height=240};
 b.menu.pointer_available=true;b.menu.focus.pointer_owner=true;
 /* T13 split portrait: 58x104 at (74,46) -> head (74+29, 46+19). */
 b.menu.pointer_x=80+74+29;b.menu.pointer_y=46+19;
 container_inventory_look(&b,&pitch,&yaw);
 assert(pitch==0&&yaw==0);
 b.menu.pointer_x=52;
 container_inventory_look(&b,&pitch,&yaw);
 assert(yaw<0);
 container_inventory_look(&a,&pitch,&yaw);
 assert(yaw==fallback_yaw&&pitch==fallback_pitch);
}

/* ui-build-1026/T12: the personal inventory has two pages. A tap of
 * PAGE_SWAP (PC default C) swaps Inventory <-> Crafting with NO RPC: the
 * window, the cursor stack and the four real 2x2 craftMatrix slots are the
 * same objects on both pages (Beta ContainerPlayer.java:13 -- one container
 * for the whole GUI). -/+ stay section jumps on Inventory and category tabs
 * on Crafting. Close from the Crafting page is the window close (held stack
 * returned first), which is where Beta empties the grid
 * (ContainerPlayer.java:48-57). */
static void page_tap(struct screen* screen) {
	press(screen, "C");        /* pressed + held: no swap yet */
	frame(screen, "", 0.016F); /* release before 0.8 s: swap */
}
static void test_inventory_two_pages(void) {
	struct window_container* wc = gstate.players[0].windows[WINDOWC_INVENTORY];
	struct recipe_browser* b = &gstate.players[0].recipes;
	inventory_clear(windowc_get_latest(wc));
	windowc_discard_pending_actions(wc);
	px = py = -1;
	screen_set(&screen_inventory);
	drain();
	struct inventory* inv = windowc_get_latest(wc);
	const struct item_data grid[4] = {{.id = 5, .count = 2}, {0},
		{.id = 4, .count = 1}, {.id = 5, .count = 9}};
	for(int k = 0; k < 4; k++) inv->items[INVENTORY_SLOT_CRAFTING + k] = grid[k];
	inventory_set_picked_item(inv, (struct item_data) {.id = 3, .count = 7});
	assert(!b->active);
	frame(&screen_inventory, "", 0.016F); /* neutral after open */

	/* A held page key is not a tap: nothing happens until release. */
	press(&screen_inventory, "C");
	assert(!b->active);
	frame(&screen_inventory, "", 0.016F);
	struct capture c = drain();
	assert(b->active && b->grid == 2);
	assert(!c.clicks && !c.closes && !c.drops);
	inv = windowc_get_latest(wc);
	for(int k = 0; k < 4; k++)
		assert(inv->items[INVENTORY_SLOT_CRAFTING + k].id == grid[k].id
			&& inv->items[INVENTORY_SLOT_CRAFTING + k].count == grid[k].count);
	assert(inv->picked_item.id == 3 && inv->picked_item.count == 7);
	assert(screen_inventory_focus_player_slot() == -1); /* no slot focus on Crafting */

	/* Next is a crafting TAB here, never a page change. */
	uint8_t category = b->category;
	press(&screen_inventory, "F");
	assert(b->active && b->category == (category + 1) % RECIPE_CATALOG_CATEGORIES);

	/* Swap back: same window, same grid, same held stack, still no RPC. */
	page_tap(&screen_inventory);
	c = drain();
	assert(!b->active && !c.clicks && !c.closes && !c.drops);
	inv = windowc_get_latest(wc);
	for(int k = 0; k < 4; k++)
		assert(inv->items[INVENTORY_SLOT_CRAFTING + k].count == grid[k].count);
	assert(inv->picked_item.count == 7);
	/* Previous/Next on Inventory are section jumps; they never open Crafting,
	 * and DOWN below the grid no longer focuses a "Recipes" entry. */
	press(&screen_inventory, "F");
	press(&screen_inventory, "G");
	press(&screen_inventory, "S");
	assert(!b->active);
	c = drain();
	assert(!c.closes);

	/* Manual Grid shortcut (T) on the Crafting page: no popup -- it goes to
	 * the page holding the REAL 2x2 slots with focus on the first one. */
	page_tap(&screen_inventory);
	assert(b->active);
	press(&screen_inventory, "T");
	assert(!b->active && !gstate.players[0].manual_grid.active);
	c = drain();
	assert(!c.clicks && !c.closes);
	inventory_set_picked_item(windowc_get_latest(wc), (struct item_data) {0});
	press(&screen_inventory, " ");
	c = drain();
	assert(c.clicks == 1 && c.last.payload.window_click.slot == INVENTORY_SLOT_CRAFTING);
	windowc_discard_pending_actions(wc);

	/* Full storage + held stack + 2x2 contents: both pages close the same
	 * Beta way -- one Close, no return clicks, never blocked. The window
	 * owner drops cursor + craftMatrix (ContainerPlayer.java:48-57). */
	for(int page = 0; page < 2; page++) {
		inv = windowc_get_latest(wc);
		inventory_clear(inv);
		for(size_t k = INVENTORY_SLOT_MAIN; k < INVENTORY_SIZE; k++)
			inv->items[k] = (struct item_data) {.id = 1, .count = 64};
		for(int k = 0; k < 4; k++) inv->items[INVENTORY_SLOT_CRAFTING + k] = grid[k];
		inventory_set_picked_item(inv, (struct item_data) {.id = 3, .count = 7});
		screen_set(&screen_inventory);
		px = py = -1;
		frame(&screen_inventory, "", 0.016F);
		if(page) { page_tap(&screen_inventory); assert(b->active); }
		drain();
		press(&screen_inventory, "E");
		c = drain();
		assert(c.closes == 1 && !c.clicks && !c.drops);
		assert(c.last.payload.window_close.window == WINDOWC_INVENTORY);
		frame(&screen_inventory, "", 0.016F);
		assert(!drain().closes);
	}
	inventory_clear(windowc_get_latest(wc));
	windowc_discard_pending_actions(wc);
	b->active = false;

	/* Split-screen owner host: same two pages, same rules, per owner. */
	struct client_session peer = {0};
	struct window_container pw;
	assert(windowc_create(&pw, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
	peer.windows[WINDOWC_INVENTORY] = &pw;
	peer.visible_window = -1;
	struct container_glyphs glyphs;
	container_compact_active_glyphs(&glyphs);
	assert(local_menu_host_open(&peer, 1, WINDOWC_INVENTORY, 640, 240));
	inv = windowc_get_latest(&pw);
	for(int k = 0; k < 4; k++) inv->items[INVENTORY_SLOT_CRAFTING + k] = grid[k];
	inv->picked_item = (struct item_data) {.id = 3, .count = 7};
	struct capture calls = {0};
	struct container_input idle = {0}, swap = {.page_swap = true},
		prev = {.previous = true}, next = {.page = true}, back = {.close = true};
	/* The section cycle at either end stays inside Inventory (old wrap gone). */
	local_menu_host_update(&peer, &prev, &glyphs, .016F, capture_send, &calls);
	local_menu_host_update(&peer, &next, &glyphs, .016F, capture_send, &calls);
	local_menu_host_update(&peer, &next, &glyphs, .016F, capture_send, &calls);
	assert(peer.menu_host.mode == LOCAL_MENU_CONTAINER);
	local_menu_host_update(&peer, &swap, &glyphs, .016F, capture_send, &calls);
	assert(peer.menu_host.mode == LOCAL_MENU_RECIPES && peer.recipes.active
		&& peer.recipes.grid == 2);
	category = peer.recipes.category;
	local_menu_host_update(&peer, &prev, &glyphs, .016F, capture_send, &calls);
	assert(peer.menu_host.mode == LOCAL_MENU_RECIPES && peer.recipes.category
		== (category + RECIPE_CATALOG_CATEGORIES - 1) % RECIPE_CATALOG_CATEGORIES);
	local_menu_host_update(&peer, &swap, &glyphs, .016F, capture_send, &calls);
	assert(peer.menu_host.mode == LOCAL_MENU_CONTAINER && !peer.recipes.active);
	assert(!calls.clicks && !calls.closes && !calls.drops);
	inv = windowc_get_latest(&pw);
	for(int k = 0; k < 4; k++)
		assert(inv->items[INVENTORY_SLOT_CRAFTING + k].count == grid[k].count);
	assert(inv->picked_item.count == 7);
	/* A suspended frame can never swap. */
	struct container_input lost = {.suspended = true, .page_swap = true};
	local_menu_host_update(&peer, &lost, &glyphs, .016F, capture_send, &calls);
	local_menu_host_update(&peer, &idle, &glyphs, .016F, capture_send, &calls);
	assert(peer.menu_host.mode == LOCAL_MENU_CONTAINER);
	/* Manual Grid shortcut: back to the page with the real 2x2, focused. */
	local_menu_host_update(&peer, &swap, &glyphs, .016F, capture_send, &calls);
	assert(peer.menu_host.mode == LOCAL_MENU_RECIPES);
	struct container_input manual = {.quick_move = true};
	local_menu_host_update(&peer, &manual, &glyphs, .016F, capture_send, &calls);
	assert(peer.menu_host.mode == LOCAL_MENU_CONTAINER && !peer.manual_grid.active);
	assert(peer.container.slots[peer.container.menu.selected].slot == INVENTORY_SLOT_CRAFTING);
	assert(!calls.clicks && !calls.closes);
	/* Full storage + held stack: close from the Crafting page is one Close,
	 * never blocked, no return clicks (same as the Inventory page). */
	inv = windowc_get_latest(&pw);
	for(size_t k = INVENTORY_SLOT_MAIN; k < INVENTORY_SIZE; k++)
		inv->items[k] = (struct item_data) {.id = 1, .count = 64};
	local_menu_host_update(&peer, &swap, &glyphs, .016F, capture_send, &calls);
	assert(peer.menu_host.mode == LOCAL_MENU_RECIPES);
	local_menu_host_update(&peer, &back, &glyphs, .016F, capture_send, &calls);
	assert(calls.closes == 1 && !calls.clicks && !local_menu_host_active(&peer));
	/* And from the Inventory page. */
	assert(local_menu_host_open(&peer, 1, WINDOWC_INVENTORY, 640, 240));
	inv = windowc_get_latest(&pw);
	inv->picked_item = (struct item_data) {.id = 3, .count = 7};
	local_menu_host_update(&peer, &back, &glyphs, .016F, capture_send, &calls);
	assert(calls.closes == 2 && !calls.clicks && !local_menu_host_active(&peer));
	peer.windows[WINDOWC_INVENTORY] = NULL;
	windowc_destroy(&pw);
	puts("PASS inventory two pages: tap swaps Inventory/Crafting with no RPC, grid and held "
		 "stack kept, -/+ never change page, Manual Grid focuses the real 2x2, Beta close from both pages "
		 "even with full storage (full + split)");
}

/* ui-build-1026/T13: in split screen each owner has its OWN page and focus.
 * One player's page swap, tab or slot move never touches the other's, and
 * each owner's return path (Crafting -> Inventory) lands on its own last
 * focus. The Page legend glyph is resolved per owner from its own frame. */
static void test_two_owner_pages(void) {
	struct client_session p1 = {0}, p2 = {0};
	struct window_container w1, w2;
	assert(windowc_create(&w1, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
	assert(windowc_create(&w2, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
	p1.windows[WINDOWC_INVENTORY] = &w1; p2.windows[WINDOWC_INVENTORY] = &w2;
	p1.visible_window = p2.visible_window = -1;
	assert(local_menu_host_open(&p1, 0, WINDOWC_INVENTORY, 640, 240));
	assert(local_menu_host_open(&p2, 1, WINDOWC_INVENTORY, 640, 240));
	struct input_player_frame remote = {.connected = true, .category = INPUT_CAT_NUNCHUK};
	struct input_player_frame pad = {.connected = true, .category = INPUT_CAT_GAMECUBE};
	for(int k = 0; k < IB_COUNT; k++) remote.glyphs[k] = pad.glyphs[k] = -1;
	remote.glyphs[IB_GUI_PAGE_SWAP] = 9; pad.glyphs[IB_GUI_PAGE_SWAP] = 41;
	struct container_glyphs g1, g2;
	container_compact_player_glyphs(&g1, &remote);
	container_compact_player_glyphs(&g2, &pad);
	assert(g1.symbols[CONTAINER_PROMPT_PAGE_SWAP] == 9 && g2.symbols[CONTAINER_PROMPT_PAGE_SWAP] == 41);
	struct capture calls = {0};
	struct container_input idle = {0}, swap = {.page_swap = true}, right = {.right = true},
		next = {.page = true};
	/* P1 moves focus, then swaps to Crafting and changes tab. */
	local_menu_host_update(&p1, &right, &g1, .016F, capture_send, &calls);
	local_menu_host_update(&p1, &idle, &g1, .016F, capture_send, &calls);
	size_t p1_focus = p1.container.menu.selected, p2_focus = p2.container.menu.selected;
	assert(p1_focus != p2_focus);
	local_menu_host_update(&p1, &swap, &g1, .016F, capture_send, &calls);
	assert(p1.menu_host.mode == LOCAL_MENU_RECIPES && p1.recipes.active);
	uint8_t tab = p1.recipes.category;
	local_menu_host_update(&p1, &next, &g1, .016F, capture_send, &calls);
	assert(p1.recipes.category == (tab + 1) % RECIPE_CATALOG_CATEGORIES);
	/* P2 is untouched: still Inventory, same focus, no crafting state. */
	assert(p2.menu_host.mode == LOCAL_MENU_CONTAINER && !p2.recipes.active
		&& p2.container.menu.selected == p2_focus);
	/* P2 pages on its own while P1 stays on Crafting. */
	local_menu_host_update(&p2, &swap, &g2, .016F, capture_send, &calls);
	assert(p2.menu_host.mode == LOCAL_MENU_RECIPES && p1.menu_host.mode == LOCAL_MENU_RECIPES);
	assert(p2.recipes.category == 0 && p1.recipes.category == (tab + 1) % RECIPE_CATALOG_CATEGORIES);
	/* Return paths: each lands on its own Inventory page and own focus. */
	local_menu_host_update(&p1, &swap, &g1, .016F, capture_send, &calls);
	assert(p1.menu_host.mode == LOCAL_MENU_CONTAINER && p1.container.menu.selected == p1_focus);
	assert(p2.menu_host.mode == LOCAL_MENU_RECIPES);
	local_menu_host_update(&p2, &swap, &g2, .016F, capture_send, &calls);
	assert(p2.menu_host.mode == LOCAL_MENU_CONTAINER && p2.container.menu.selected == p2_focus);
	/* Swapping back to Crafting remembers each owner's own tab. */
	local_menu_host_update(&p1, &swap, &g1, .016F, capture_send, &calls);
	assert(p1.recipes.category == (tab + 1) % RECIPE_CATALOG_CATEGORIES);
	assert(!calls.clicks && !calls.closes && !calls.drops);
	local_menu_host_reset(&p1); local_menu_host_reset(&p2);
	p1.windows[WINDOWC_INVENTORY] = p2.windows[WINDOWC_INVENTORY] = NULL;
	windowc_destroy(&w1); windowc_destroy(&w2);
	puts("PASS two owner pages: P1/P2 own page, tab and focus; return paths land on each owner's focus; per-owner Page glyph");
}

/* Sol review 10-31 items 1 + 2: the bottom row's "Crafting" (Inventory page)
 * and "Inventory" (Crafting page) cells are page swaps when CLICKED, through
 * local_menu_host_update, whose page_swap test runs before the step. */
static void test_split_row_page_swap_clicks(void) {
	struct client_session p1 = {0};
	struct window_container w1;
	assert(windowc_create(&w1, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
	p1.windows[WINDOWC_INVENTORY] = &w1; p1.visible_window = -1;
	assert(local_menu_host_open(&p1, 0, WINDOWC_INVENTORY, 640, 240));
	struct input_player_frame remote = {.connected = true, .category = INPUT_CAT_NUNCHUK};
	for(int k = 0; k < IB_COUNT; k++) remote.glyphs[k] = -1;
	remote.glyphs[IB_GUI_PAGE_SWAP] = 9; remote.glyphs[IB_GUI_CLICK] = 0;
	remote.glyphs[IB_GUI_SPLIT] = 1; remote.glyphs[IB_INVENTORY] = 2;
	struct container_glyphs g; container_compact_player_glyphs(&g, &remote);
	struct input_player_frame frame; container_compact_glyph_frame(&g, &frame);
	struct gutil_hint_row row; enum container_row_action actions[CONTAINER_ROW_ITEMS];
	int n = container_row_layout(CONTAINER_ROW_INVENTORY, false, false, &frame, 0, 0, 640, 240, &row, actions);
	int cell = -1;
	for(int k = 0; k < n; k++) if(actions[k] == CONTAINER_ROW_PAGE_SWAP) cell = k;
	assert(cell >= 0 && "the split inventory row has no Crafting cell");
	struct gutil_button_bounds cr = row.cells[cell].rect;
	struct capture calls = {0};
	struct container_input hover = {.pointer_valid = true, .pointer_x = cr.x + cr.width / 2.0F,
		.pointer_y = cr.y + cr.height / 2.0F};
	local_menu_host_update(&p1, &hover, &g, .016F, capture_send, &calls);
	struct container_input click = hover; click.pick = true;
	assert(!container_compact_row_page_swap(&p1.container, &w1, &g, &hover));
	assert(container_compact_row_page_swap(&p1.container, &w1, &g, &click));
	local_menu_host_update(&p1, &click, &g, .016F, capture_send, &calls);
	assert(p1.menu_host.mode == LOCAL_MENU_RECIPES && p1.recipes.active
		   && "item 1: the Crafting cell click was eaten");
	assert(!calls.clicks && !calls.closes && !calls.drops);
	/* Crafting page: the row's Inventory cell leads the row (TV-safe edge +
	 * pad); a click on it is the swap back. */
	struct container_input idle = {0};
	local_menu_host_update(&p1, &idle, &g, .016F, capture_send, &calls);
	float ix = gutil_hint_row_margin(640) + GUTIL_HINT_PAD + 4.0F;
	float iy = 240 - GUTIL_HINT_BOTTOM_SPLIT - GUTIL_HINT_STRIP_H / 2.0F;
	struct container_input ihover = {.pointer_valid = true, .pointer_x = ix, .pointer_y = iy};
	struct container_input iclick = ihover; iclick.pick = true;
	local_menu_host_update(&p1, &ihover, &g, .016F, capture_send, &calls); /* pointer takes focus */
	assert(!recipe_browser_row_page_swap(&p1.recipes, &g, &ihover, 640, 240));
	assert(recipe_browser_row_page_swap(&p1.recipes, &g, &iclick, 640, 240));
	local_menu_host_update(&p1, &ihover, &g, .016F, capture_send, &calls);
	local_menu_host_update(&p1, &iclick, &g, .016F, capture_send, &calls);
	assert(p1.menu_host.mode == LOCAL_MENU_CONTAINER && !p1.recipes.active
		   && "item 2: the Inventory cell is not a clickable action");
	assert(!calls.clicks && !calls.closes && !calls.drops);
	/* A click elsewhere on the strip is not a swap. */
	struct container_input blank = {.pointer_valid = true, .pointer_x = 600, .pointer_y = iy, .pick = true};
	assert(!container_compact_row_page_swap(&p1.container, &w1, &g, &blank));
	local_menu_host_reset(&p1);
	p1.windows[WINDOWC_INVENTORY] = NULL;
	windowc_destroy(&w1);
	puts("PASS split row page-swap clicks: Crafting cell opens the page, Inventory cell returns (host test reads both)");
}

/* The same for player 1's FULL Crafting page (screen_inventory): the row's
 * Inventory cell click returns to the Inventory page. */
static void test_full_crafting_row_inventory_click(void) {
	struct recipe_browser* b = &gstate.players[0].recipes;
	px = py = -1;
	screen_set(&screen_inventory);
	drain();
	frame(&screen_inventory, "", 0.016F);
	page_tap(&screen_inventory);
	assert(b->active && b->grid == 2);
	px = gutil_hint_row_margin(640) + GUTIL_HINT_PAD + 4;
	py = 480 - GUTIL_HINT_BOTTOM_FULL - GUTIL_HINT_STRIP_H / 2;
	frame(&screen_inventory, "", 0.016F); px += 1; frame(&screen_inventory, "", 0.016F);
	press(&screen_inventory, " ");
	struct capture c = drain();
	assert(!b->active && "item 2 (full): the Inventory cell click did not return to Inventory");
	assert(!c.clicks && !c.closes && !c.drops);
	px = py = -1;
	puts("PASS full Crafting page: the row's Inventory cell click returns to Inventory");
}

/* T13 fix r1 (Sol review): on the split Crafting page the variant chevrons
 * sit between the tabs and the strip without overlapping either, and a click
 * on each target reaches exactly that target -- the tab row never takes a
 * click aimed at the up chevron. */
static void test_split_crafting_hit_targets(void) {
	unsigned family = 0, variants = 0;
	for(unsigned f = 0; f < recipe_catalog_family_count(0); f++)
		if(recipe_catalog_variant_count(0, f) > 1) { family = f; variants = recipe_catalog_variant_count(0, f); break; }
	assert(variants > 1);
	struct recipe_browser b = {0};
	recipe_browser_open(&b, 2);
	b.category = 0; b.family[0] = (uint8_t)family;
	unsigned flat = 0; /* category 0 starts at flat family 0 */
	b.variant[flat + family] = 1;
	struct recipe_browser_geometry g;
	recipe_browser_geometry_grid(2, 240, family, recipe_catalog_family_count(0), 1, variants, &g);
	int tx = g.tile_x[family - g.first] + 12;
	struct container_input hover = {.pointer_valid = true, .pointer_x = tx - 20, .pointer_y = g.variant_up_y + 4};
	struct container_input up = {.pointer_valid = true, .pointer_x = tx, .pointer_y = g.variant_up_y + 4, .pick = true};
	recipe_browser_step(&b, &hover, 640, 240, .016F);
	assert(recipe_browser_step(&b, &up, 640, 240, .016F) == RECIPE_BROWSER_NONE);
	assert(b.category == 0 && b.variant[flat + family] == 0 && "the up chevron click went elsewhere");
	struct container_input down = {.pointer_valid = true, .pointer_x = tx, .pointer_y = g.variant_down_y + 4, .pick = true};
	hover.pointer_y = g.variant_down_y + 4;
	recipe_browser_step(&b, &hover, 640, 240, .016F);
	assert(recipe_browser_step(&b, &down, 640, 240, .016F) == RECIPE_BROWSER_NONE);
	assert(b.category == 0 && b.variant[flat + family] == 1 && "the down chevron click went elsewhere");
	/* The bottom pixel row of the tabs is still a tab, never the chevron. */
	struct container_input tab = {.pointer_valid = true, .pointer_x = 80 + 36 * 2 + 4, .pointer_y = g.category_y + 31, .pick = true};
	hover.pointer_x = tab.pointer_x - 20; hover.pointer_y = tab.pointer_y;
	recipe_browser_step(&b, &hover, 640, 240, .016F);
	assert(recipe_browser_step(&b, &tab, 640, 240, .016F) == RECIPE_BROWSER_NONE);
	assert(b.category == 2);
	puts("PASS split crafting hit targets: tabs, up/down chevrons each own their clicks");
}

int main(void) {
	test_inventory_portrait_look();
	blocks_init(); items_init();
	recipe_init();
	test_quick_move_rules();
	test_quick_cake();
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
	struct window_container wc[6];
	const int ids[] = {WINDOWC_INVENTORY, WINDOWC_CRAFTING, WINDOWC_FURNACE,
		WINDOWC_CHEST, WINDOWC_DISPENSER, WINDOWC_IRON_CHEST};
	const enum window_type types[] = {WINDOW_TYPE_INVENTORY, WINDOW_TYPE_WORKBENCH,
		WINDOW_TYPE_FURNACE, WINDOW_TYPE_CHEST, WINDOW_TYPE_DISPENSER, WINDOW_TYPE_IRON_CHEST};
	const size_t sizes[] = {45, 46, 39, 63, 45, 90};
	for(int k = 0; k < 6; k++) {
		assert(windowc_create(&wc[k], types[k], sizes[k]));
		gstate.players[0].windows[ids[k]] = &wc[k];
	}
	screen_crafting_set_windowc(WINDOWC_CRAFTING);
	screen_furnace_set_windowc(WINDOWC_FURNACE);
	screen_chest_set_windowc(WINDOWC_CHEST);
	screen_dispenser_set_windowc(WINDOWC_DISPENSER);
	screen_iron_chest_set_windowc(WINDOWC_IRON_CHEST);
	test_bindings(); test_neighbors(); test_screens();
	test_manual_grid_owned();
	test_manual_grid_footer_actions();
	test_quick_move_ui();
	test_slot_activation_order();
	test_safe_back_and_sections();
	test_close_never_waits();
	test_full_chest(); test_quick_move_round_trip(); test_compact_layouts(); test_two_view_owners();
	test_split_row_page_swap_clicks(); test_full_crafting_row_inventory_click();
	test_owned_controller_menus();
	test_session_replies();
	test_compact_screen_adapter(); test_failed_send(); test_accept_serial(); test_unsent_revision();
	test_primary_controller_recovery();
	test_inventory_two_pages();
	test_two_owner_pages();
	test_split_crafting_hit_targets();
	for(int k = 0; k < 6; k++) {
		windowc_destroy(&wc[k]);
		gstate.players[0].windows[ids[k]] = NULL;
	}
	world_destroy(&gstate.world);
	config_destroy(&gstate.config_user);
	window = NULL;
	puts("test_container_menu: PASS");
	return 0;
}
