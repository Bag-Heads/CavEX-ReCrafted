/*
	Beta 1.7.3 chest-window LAYOUT, extracted as pure functions so it can be golden-tested
	without a GL context (handoff 2026-07-29 P2).

	WHY THIS EXISTS. screen_chest.c drew the window background as ONE 176x167 blit, i.e. it
	blitted source rows v=0..167 contiguously. That is only valid for a fork-authored
	single-chest sheet. Beta's gui/container.png is a SIX-row DOUBLE-chest sheet, so against
	the real Beta pack that single blit paints all six chest rows plus the top 42px of the
	player panel -- four fully-empty cell rows above the items, which is exactly the
	"roughly 4-5 rows of empty cells" seen on hardware.

	Beta draws it as TWO rects, skipping the middle of the sheet for a small chest
	(GuiChest.java:26-34):
	    drawTexturedModalRect(x, y,                    0,   0, xSize, rows*18 + 17);
	    drawTexturedModalRect(x, y + rows*18 + 17,     0, 126, xSize, 96);
	For rows=3 that is src v=0..70 then src v=126..221 -- source rows 71..125 (the extra
	three chest rows) are SKIPPED. For rows=6 the first rect grows to 125 and nothing is
	skipped, which is why the same sheet serves both sizes.

	⚠ THE 167-vs-168 TRAP. The drawn background is 71+96 = 167 rows tall, but ySize is 168
	(GuiChest.java:15-18: 222 minus 108, plus rows*18 = 114 +
	rows*18). ySize is the CENTERING height only -- the two blit heights stay 71 and 96. Do
	not "simplify" chest_gui_height() into the sum of the rects.

	Slot origins are ContainerChest.java, which shifts the player block by
	a shift of (numRows - 4) * 18 -- i.e. -18 for a small chest, +36 for a large one:
	    :16  storage      (8 + c*18, 18 + r*18)
	    :22  player main  (8 + c*18, 103 + r*18 + shift)
	    :27  hotbar       (8 + c*18, 161 + shift)
	For rows=3 that is storage 18/36/54, main 85/103/121, hotbar 143 -- each ONE pixel below
	the corresponding screen_inventory.c row. That 1px is a genuine Beta quirk, not an error:
	GuiChest blits the player panel from source v=126 to dest y=71 (offset -55), so
	container.png's baked bevel rows 139/157/175/197 land at dest 84/102/120/142 as BORDERS,
	putting the cell INTERIORS at 85/103/121/143. Do NOT propagate the +1 to
	screen_inventory / screen_crafting / screen_furnace: those are already Beta-correct at
	84/102/120/142 against their own sheets.

	Decompile: 740c583:1.7.3-LTS GuiChest, GuiContainer, ContainerChest.
*/

#ifndef GUI_CHEST_LAYOUT_H
#define GUI_CHEST_LAYOUT_H

#include <stddef.h>

// Beta GuiContainer.java:8 -- every container window is 176 wide.
#define CHEST_GUI_WIDTH 176
// The source row the player-inventory panel starts at inside container.png
// (GuiChest.java:33 second rect) and its height.
#define CHEST_GUI_PANEL_SRC_V 126
#define CHEST_GUI_PANEL_H 96
// GuiChest.java:32 -- the chest block is rows*18 plus a 17px top border.
#define CHEST_GUI_TOP_BORDER 17

struct chest_gui_rect {
	int src_u, src_v; // source rect in the sheet
	int w, h;
	int dst_x, dst_y; // destination offset from the window origin
};

// ySize: 114 + rows*18 (GuiChest.java:15-18). 168 for a 3-row chest, 222 for a 6-row one.
// CENTERING ONLY -- it is one pixel taller than the background actually drawn.
int chest_gui_height(int rows);

// The TWO background rects Beta draws, in order. Returns the count (always 2).
int chest_gui_bg_rects(int rows, struct chest_gui_rect out[2]);

// Slot origins in GUI space (before GFX_GUI_SCALE). `index` is within each group.
void chest_gui_storage_slot(int rows, int index, int* x, int* y);
void chest_gui_main_slot(int rows, int index, int* x, int* y);
void chest_gui_hotbar_slot(int rows, int index, int* x, int* y);

/* WIRE SLOT BASES. A chest window is laid out storage-first: slots 0..S-1 are the
   container, then the player's 27 main slots, then the 9 hotbar slots -- Beta's
   ContainerChest adds them in exactly that order (:16 lower inventory, :22 player main,
   :27 hotbar). One formula covers both sizes:
       rows=3 -> 0 / 27 / 54   (== CHEST_SLOT_STORAGE / _MAIN / _HOTBAR)
       rows=6 -> 0 / 54 / 81   (== the double-chest layout inventory_double_chest.c uses)
   which is why routing the double chest to this screen needed no wire change at all --
   only the window TYPE, so the client picks GuiChest instead of the fork's iron-chest GUI. */
#define CHEST_GUI_MAIN_SLOTS 27
#define CHEST_GUI_HOTBAR_SLOTS 9
int chest_gui_storage_count(int rows);
int chest_gui_main_base(int rows);
int chest_gui_hotbar_base(int rows);

// GuiChest.java:22-23 -- the window draws TWO labels, both at x=8: the chest name at y=6 and
// the player-inventory name at `ySize - 96 + 2`.
#define CHEST_GUI_TITLE_X 8
#define CHEST_GUI_TITLE_Y 6
int chest_gui_inventory_label_y(int rows);

#endif
