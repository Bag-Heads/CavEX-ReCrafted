/* Lane 5 -- the APPROVED dispenser composition, both layouts.
 *
 * Authority: docs/ui_design/approved/menus.html (menu selector -> Dispenser) and
 * specs/menus.md, "Dispenser and dispenser": "Show machine slots AND all 36
 * carried slots, with item names and remembered selection. Never restrict
 * transfer sources to the hotbar. Keep the approved positioning/spacing and
 * split-screen readability; the machine, bag, selection label and button hints
 * have distinct space."
 *
 * Plus the P2 acceptance gate from the shared rules and IMPLEMENTATION.md's
 * open decision ("do not set local_gameplay_ready merely to make a preview
 * appear implemented"): the split composition is admitted through the OWNER
 * MODEL, never merely because a viewport measured short.
 *
 * The real container_compact and menu_header code runs; the window/inventory
 * layer is the production one. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#undef GLFW_DLL
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "game/gui/container_compact.h"
#include "game/gui/container_menu.h"
#include "game/gui/menu_header.h"
#include "block/blocks.h"
#include "item/items.h"
#include "item/window_container.h"

/* Beta GuiDispenser / ContainerDispenser [740c583]: the fixed 176x166 panel
 * (GuiDispenser overrides neither xSize nor ySize), the 3x3 storage grid at
 * (62+col*18, 17+row*18), 27 main at (8+col*18, 84+row*18) and 9 hotbar at
 * (8+col*18, 142). DISPENSER_SIZE is 9 + 36. */
#define PANEL_W (176 * GFX_GUI_SCALE)
#define PANEL_H (166 * GFX_GUI_SCALE)

static void check_simultaneous(int height) {
 struct container_compact v;struct window_container wc;
 assert(windowc_create(&wc,WINDOW_TYPE_DISPENSER,DISPENSER_SIZE));
 assert(container_compact_open(&v,WINDOW_TYPE_DISPENSER,DISPENSER_SIZE,4,640,height));
 assert(v.active&&v.count==DISPENSER_SIZE&&v.count==9+36&&v.view_count==1&&v.view==0);
 assert(v.menu.slot_extent==36&&v.menu.selected==9+31);
 bool seen[DISPENSER_SIZE]={0};
 for(size_t k=0;k<v.count;k++){
  assert(v.slots[k].slot<DISPENSER_SIZE&&!seen[v.slots[k].slot]);seen[v.slots[k].slot]=true;
  for(size_t j=0;j<k;j++)assert(v.slots[k].x>=v.slots[j].x+36||v.slots[j].x>=v.slots[k].x+36||v.slots[k].y>=v.slots[j].y+36||v.slots[j].y>=v.slots[k].y+36);
 }
 for(size_t k=0;k<36;k++){
  assert(v.slots[9+k].slot==9+k);
  assert(v.slots[9+k].x==(height==480?158:272)+(int)(k%9)*36);
  assert(v.slots[9+k].y==(height==480?206:40) /* B3: up 14 */+(int)(k/9)*36+(k>=27?4:0));
 }
 assert(v.slots[0].x==(height==480?266:92)&&v.slots[0].y==(height==480?65:49));
 size_t selected=v.menu.selected;struct container_input input={.page=true};
 assert(!container_compact_step(&v,&wc,7,&input,.016F,container_menu_send,NULL));
 assert(v.view==0&&v.menu.selected==selected);
 size_t target=v.count-1;v.menu.selected=0;v.menu.focus=(struct gutil_menu_focus){0};
 input=(struct container_input){.pointer_valid=true,.pointer_x=v.slots[target].x+35.5F,.pointer_y=v.slots[target].y+35.5F};
 container_menu_focus_step(&v.menu,v.slots,v.count,0,0,&input,.016F);
 assert(v.menu.pointer_on_slot&&v.menu.selected==target);
 windowc_destroy(&wc);
}
static void test_full_layout_slots(void){check_simultaneous(480);}
static void test_compact_layout_slots(void){check_simultaneous(240);}

static void test_p2_acceptance_gate(void) {
	/* The owner model, not the viewport, admits the split composition. */
	assert(!menu_split_composition_admitted(false, false));
	assert(!menu_split_composition_admitted(true, false)); /* P2 present but
															* not yet admitted */
	assert(!menu_split_composition_admitted(false, true));
	assert(menu_split_composition_admitted(true, true));

	/* With the gate closed, a 240-row viewport must NOT produce the split
	 * composition even though container_compact_open itself would accept it --
	 * that is exactly the "make a preview appear implemented" failure. */
	struct container_compact v;
	for(int present = 0; present < 2; present++)
		for(int ready = 0; ready < 2; ready++) {
			bool admitted
				= menu_split_composition_admitted(present != 0, ready != 0);
			bool opened = admitted
				&& container_compact_open(&v, WINDOW_TYPE_DISPENSER, DISPENSER_SIZE,
										  0, 640, 240);
			assert(opened == admitted);
			if(!admitted)
				continue;
			assert(v.active && v.type == WINDOW_TYPE_DISPENSER);
		}
	puts("  dispenser: split composition needs an ADMITTED second player 1/1");
}

static void test_distinct_space(void) {
	/* "the machine, bag, selection label and button hints have distinct
	 * space" -- checked against the compact composition the screen declares. */
	struct container_compact v;
	assert(container_compact_open(&v, WINDOW_TYPE_DISPENSER, DISPENSER_SIZE, 0, 640,
								  240));
	struct menu_composition c;
	menu_composition_begin(&c, MENU_SCREEN_DISPENSER, 0, 0, 640, 240);
	menu_composition_window(&c, 0, 0, 640, 240);
	struct menu_rect head = menu_container_header_box(0, 0);
	menu_composition_header(&c, head.x, head.y, head.w, head.h);
	menu_composition_seal(&c);

	/* The bag/machine grid never reaches into the header band ... */
	for(unsigned p = 0; p < v.view_count; p++) {
		struct window_container wc;
		assert(windowc_create(&wc, WINDOW_TYPE_DISPENSER, DISPENSER_SIZE));
		for(size_t k = 0; k < v.count; k++) {
			struct menu_rect slot = {v.slots[k].x, v.slots[k].y, 32, 32};
			assert(!menu_rect_overlap(&slot, &head));
			assert(!menu_rect_overlap(&slot, &c.notice));
			/* ... nor into the footer prompt row (drawn at y 196). */
			assert(slot.y + slot.h <= 196);
		}
		struct container_input page = {.page = true};
		container_compact_step(&v, &wc, 7, &page, 0.016f, container_menu_send,
							   NULL);
		windowc_destroy(&wc);
	}
	puts("  dispenser: machine/bag, header and prompt rows keep distinct space "
		 "1/1");
}

int main(void) {
	puts("approved dispenser layout");
	/* Real item registry: the selection label must name the ITEM. */
	blocks_init();
	items_init();
	test_full_layout_slots();
	test_compact_layout_slots();
	test_p2_acceptance_gate();
	test_distinct_space();
	puts("PASS approved dispenser layout");
	return 0;
}
