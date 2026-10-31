/*
	Pure Beta 1.7.3 chest-window layout -- see chest_layout.h for the decompile citations and
	the two traps (the 167-vs-168 centering height, and the deliberate 1px player-row offset
	that must NOT be propagated to the other container screens).
*/

#include "chest_layout.h"

// GuiChest.java:15-18:
//     base height 222;
//     minus 108 = 114;
//     rows = the inventory size / 9;
//     ySize = 114 + rows * 18.
int chest_gui_height(int rows) {
	return 114 + rows * 18;
}

// GuiChest.java:32-33 -- drawGuiContainerBackgroundLayer:
//     drawTexturedModalRect(var3, var4,                          0,   0, xSize, rows*18+17);
//     drawTexturedModalRect(var3, var4 + rows*18 + 17,           0, 126, xSize, 96);
// The second rect's SOURCE v is a fixed 126 regardless of rows: that is what skips the
// unused chest rows on a small chest and consumes them on a large one.
int chest_gui_bg_rects(int rows, struct chest_gui_rect out[2]) {
	const int chest_h = rows * 18 + CHEST_GUI_TOP_BORDER;
	out[0] = (struct chest_gui_rect) {
		.src_u = 0,
		.src_v = 0,
		.w = CHEST_GUI_WIDTH,
		.h = chest_h,
		.dst_x = 0,
		.dst_y = 0,
	};
	out[1] = (struct chest_gui_rect) {
		.src_u = 0,
		.src_v = CHEST_GUI_PANEL_SRC_V,
		.w = CHEST_GUI_WIDTH,
		.h = CHEST_GUI_PANEL_H,
		.dst_x = 0,
		.dst_y = chest_h,
	};
	return 2;
}

// ContainerChest.java:10 -- `int var3 = (this.numRows - 4) * 18;` shifts the whole player
// block: -18 for a 3-row chest, 0 for 4 rows, +36 for a 6-row double chest.
static int chest_player_shift(int rows) {
	return (rows - 4) * 18;
}

// ContainerChest.java:16 -- addSlot(new Slot(lower, index, 8 + col*18, 18 + row*18))
void chest_gui_storage_slot(int rows, int index, int* x, int* y) {
	(void)rows; // storage always starts at the top; only the player block shifts
	if(x)
		*x = 8 + (index % 9) * 18;
	if(y)
		*y = 18 + (index / 9) * 18;
}

// ContainerChest.java:22 -- addSlot(new Slot(player, ..., 8 + col*18, 103 + row*18 + var3))
void chest_gui_main_slot(int rows, int index, int* x, int* y) {
	if(x)
		*x = 8 + (index % 9) * 18;
	if(y)
		*y = 103 + (index / 9) * 18 + chest_player_shift(rows);
}

// ContainerChest.java:27 -- addSlot(new Slot(player, index, 8 + index*18, 161 + var3))
void chest_gui_hotbar_slot(int rows, int index, int* x, int* y) {
	if(x)
		*x = 8 + index * 18;
	if(y)
		*y = 161 + chest_player_shift(rows);
}

// ContainerChest adds the slots storage-first, so the bases fall straight out of the row
// count -- see the note in chest_layout.h for why this is what made the double chest free.
int chest_gui_storage_count(int rows) {
	return rows * 9;
}
int chest_gui_main_base(int rows) {
	return chest_gui_storage_count(rows);
}
int chest_gui_hotbar_base(int rows) {
	return chest_gui_storage_count(rows) + CHEST_GUI_MAIN_SLOTS;
}

// GuiChest.java:23 -- drawString(upper.getInvName(), 8, this.ySize - 96 + 2, 4210752).
// For a 3-row chest that is 168 - 96 + 2 = 74.
int chest_gui_inventory_label_y(int rows) {
	return chest_gui_height(rows) - CHEST_GUI_PANEL_H + 2;
}
