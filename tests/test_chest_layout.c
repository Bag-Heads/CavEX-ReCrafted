/*
	Golden test for the Beta 1.7.3 chest window (handoff 2026-07-29 P2).

	SYMPTOM on hardware: the chest window showed roughly 4-5 rows of empty cells above the
	items, and the items sat ~12px off their cells. The maintainer guessed "it might think
	it's a double chest".

	THAT HYPOTHESIS IS DISPROVEN for the window LOGIC -- block_chest.c opens a 27-slot
	WINDOWC_CHEST for a lone chest and screen_chest.c emits exactly CHEST_SIZE_STORAGE = 27
	storage slots, so the window really is 3 rows. Only the DRAWING was wrong, three ways:

	  (a) the background was ONE 176x167 blit. Beta's gui/container.png is a SIX-row
	      DOUBLE-chest sheet, so blitting v=0..167 contiguously paints all six chest rows
	      plus the top 42px of the player panel. Beta draws TWO rects and SKIPS source rows
	      71..125 for a small chest (GuiChest.java:32-33).
	  (c) slot rows were 2px/1px above Beta's (ContainerChest.java:16/22/27).
	  (d) GUI_HEIGHT was 167; Beta's GuiContainer default is 166, and GuiChest overrides it
	      to 114 + rows*18 = 168.
	  (e) the title sat at x=28 instead of 8, and the second "Inventory" label was missing.

	The asset half (b) is pinned by check_asset_geometry below: the BUNDLED
	assets/gui/chest.png was authored to the FORK's slot coordinates (cell rows at
	v=16/34/52, 84/102/120, 142 -- exactly the pre-fix code) and is only 166 rows tall, so
	applying Beta's two-rect draw to it would sample ~56 rows of pure transparency. Fixing
	the code alone breaks the default pack; the two must land together.

	Decompile: 740c583:1.7.3-LTS GuiChest, GuiContainer, ContainerChest.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/gui/chest_layout.h"
#include "item/inventory.h"
#include "lodepng/lodepng.h"

#define ROWS_SMALL (CHEST_SIZE_STORAGE / 9) // 3
#define ROWS_LARGE 6                        // Beta's double chest, 54 storage slots

// --- (1) the centering height, including the 167-vs-168 trap ------------------------
static void check_height(void) {
	assert(chest_gui_height(ROWS_SMALL) == 168 && "GuiChest.java:18 -> 114 + 3*18");
	assert(chest_gui_height(ROWS_LARGE) == 222 && "114 + 6*18 -- Beta's double chest");
	// THE TRAP: the DRAWN background is one pixel shorter than the centering height.
	struct chest_gui_rect r[2];
	chest_gui_bg_rects(ROWS_SMALL, r);
	assert(r[0].h + r[1].h == 167
		   && "the two rects draw 167 rows while ySize is 168 -- do not collapse one into "
			  "the other");
	printf("  [1] height 168/222, drawn background 167 (the ySize trap)\n");
}

// --- (2) the two background rects, and the SKIPPED source band ----------------------
static void check_bg_rects(void) {
	struct chest_gui_rect r[2];
	assert(chest_gui_bg_rects(ROWS_SMALL, r) == 2);
	// GuiChest.java:32 -- src (0,0), height rows*18+17 = 71.
	assert(r[0].src_u == 0 && r[0].src_v == 0);
	assert(r[0].w == 176 && r[0].h == 71);
	assert(r[0].dst_x == 0 && r[0].dst_y == 0);
	// GuiChest.java:33 -- src v is a FIXED 126, height 96, drawn directly below the first.
	assert(r[1].src_u == 0 && r[1].src_v == 126);
	assert(r[1].w == 176 && r[1].h == 96);
	assert(r[1].dst_x == 0 && r[1].dst_y == 71);

	// THE WHOLE POINT: source rows 71..125 are SKIPPED for a small chest. Those are the
	// three extra chest rows of the double-chest sheet -- the empty rows seen on hardware.
	assert(r[0].src_v + r[0].h == 71 && r[1].src_v == 126
		   && "the gap between the two source rects is the unused chest block");

	// A 6-row chest consumes the whole sheet: nothing is skipped.
	assert(chest_gui_bg_rects(ROWS_LARGE, r) == 2);
	assert(r[0].h == 125 && r[1].src_v == 126 && r[1].dst_y == 125
		   && "a double chest's first rect must run right up to the panel");
	printf("  [2] two rects; rows 71..125 skipped for a small chest, consumed for a large\n");
}

// --- (3) slot origins straight out of ContainerChest ---------------------------------
static void check_slot_origins(void) {
	int x, y;
	// Storage: (8 + col*18, 18 + row*18) -> rows 18/36/54. Was 16/34/52 (2px high).
	const int want_storage_y[3] = {18, 36, 54};
	for(int i = 0; i < CHEST_SIZE_STORAGE; i++) {
		chest_gui_storage_slot(ROWS_SMALL, i, &x, &y);
		assert(x == 8 + (i % 9) * 18);
		assert(y == want_storage_y[i / 9]);
	}
	// Player main: 103 + row*18 + (rows-4)*18 -> 85/103/121. Was 84/102/120 (1px high).
	const int want_main_y[3] = {85, 103, 121};
	for(int i = 0; i < INVENTORY_SIZE_MAIN; i++) {
		chest_gui_main_slot(ROWS_SMALL, i, &x, &y);
		assert(x == 8 + (i % 9) * 18);
		assert(y == want_main_y[i / 9]);
	}
	// Hotbar: 161 + (rows-4)*18 -> 143. Was 142.
	for(int i = 0; i < INVENTORY_SIZE_HOTBAR; i++) {
		chest_gui_hotbar_slot(ROWS_SMALL, i, &x, &y);
		assert(x == 8 + i * 18 && y == 143);
	}
	// The double chest shifts the player block DOWN by 36 instead of up by 18.
	chest_gui_storage_slot(ROWS_LARGE, 0, &x, &y);
	assert(y == 18 && "storage always starts at the top");
	chest_gui_storage_slot(ROWS_LARGE, 45, &x, &y);
	assert(y == 108 && "the sixth storage row");
	chest_gui_main_slot(ROWS_LARGE, 0, &x, &y);
	assert(y == 139);
	chest_gui_hotbar_slot(ROWS_LARGE, 0, &x, &y);
	assert(y == 197);
	printf("  [3] slot origins: storage 18/36/54, main 85/103/121, hotbar 143 (small); "
		   "139/197 (large)\n");
}

// --- (4) the two labels ---------------------------------------------------------------
static void check_labels(void) {
	assert(CHEST_GUI_TITLE_X == 8 && "the title was indented to x=28; Beta draws it at 8");
	assert(CHEST_GUI_TITLE_Y == 6);
	// GuiChest.java:23 -- ySize - 96 + 2. 168-96+2 = 74 for a small chest.
	assert(chest_gui_inventory_label_y(ROWS_SMALL) == 74);
	assert(chest_gui_inventory_label_y(ROWS_LARGE) == 128);
	printf("  [4] title at x=8,y=6; the second \"Inventory\" label at y=74\n");
}

// --- (5) NEGATIVE CONTROL: the sibling screens must NOT be harmonised ------------------
// The chest's player rows sit ONE pixel below screen_inventory's because GuiChest blits the
// panel from source v=126 to dest y=71 (offset -55), landing container.png's baked bevels
// at 84/102/120/142 as BORDERS with interiors at 85/103/121/143. screen_inventory draws its
// own sheet with no such offset and is already Beta-correct. A blanket sed that "fixes" all
// four screens to the same numbers is the failure this pins.
static char* read_source(const char* path) {
	FILE* f = fopen(path, "rb");
	assert(f && "source file not found -- is WORKING_DIRECTORY the repo root?");
	assert(fseek(f, 0, SEEK_END) == 0);
	long size = ftell(f);
	assert(size > 0 && fseek(f, 0, SEEK_SET) == 0);
	char* buf = malloc((size_t)size + 1);
	assert(buf);
	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

static void check_siblings_not_harmonised(void) {
	const struct {
		const char* path;
		const char* keep;
	} sib[] = {
		/* Approved inventory now has its own 36px composition, tested in test_container_menu. */
		{"source/game/gui/screen_crafting.c", "84 + "},
		{"source/game/gui/screen_furnace.c", "84 + "},
	};
	for(size_t i = 0; i < sizeof(sib) / sizeof(sib[0]); i++) {
		char* t = read_source(sib[i].path);
		assert(strstr(t, sib[i].keep)
			   && "a sibling container screen lost its Beta-correct 84/102/120 player rows "
				  "-- the chest's +1 is a GuiChest-specific blit offset, do not propagate "
				  "it");
		// ...and they must be 166, not the chest's 168 (GuiContainer.java:9 ySize = 166).
		assert(strstr(t, "#define GUI_HEIGHT 166")
			   && "sibling GUI_HEIGHT should be Beta's 166");
		free(t);
	}
	// The fork-only iron chest is explicitly out of scope (300x190).
	char* iron = read_source("source/game/gui/screen_iron_chest.c");
	assert(strstr(iron, "#define GUI_HEIGHT 190")
		   && "screen_iron_chest is fork-only and must keep its own 300x190 geometry");
	free(iron);
	printf("  [5] sibling screens keep 84/102/120 and GUI_HEIGHT 166; iron chest untouched\n");
}

// --- (5b) P2(f): ONE screen serves both chest sizes -----------------------------------
// Beta wraps an adjacent pair in an InventoryLargeChest and hands it to the SAME
// displayGUIChest (BlockChest.java:199-211); GuiChest.java:17 then derives inventoryRows
// from the container size. CaveWii used to send the double chest to the FORK's iron-chest
// screen instead -- which carries its own "HACK: hardcoded slot counts" comment and whose
// gui/iron_chest.png is fork-only, so a Beta pack silently got fork art for every double
// chest. These pin the unified path.
static void check_slot_bases_unify_both_sizes(void) {
	// Small chest: the bases must reproduce the existing CHEST_SLOT_* constants exactly,
	// or every click in a lone chest would address the wrong slot.
	assert(chest_gui_storage_count(ROWS_SMALL) == CHEST_SIZE_STORAGE);
	assert(chest_gui_main_base(ROWS_SMALL) == CHEST_SLOT_MAIN);
	assert(chest_gui_hotbar_base(ROWS_SMALL) == CHEST_SLOT_HOTBAR);
	assert(CHEST_SLOT_STORAGE == 0);

	// Double chest: must reproduce the layout inventory_double_chest.c already reads and
	// writes on the server (storage 0..53, main 54..80, hotbar 81..89). That equality is
	// the whole reason routing the double chest here needed NO wire change.
	assert(chest_gui_storage_count(ROWS_LARGE) == IRON_CHEST_SIZE_STORAGE);
	assert(chest_gui_main_base(ROWS_LARGE) == IRON_CHEST_SLOT_MAIN);
	assert(chest_gui_hotbar_base(ROWS_LARGE) == IRON_CHEST_SLOT_HOTBAR);

	// The window totals the two sizes imply.
	assert(chest_gui_hotbar_base(ROWS_SMALL) + 9 == CHEST_SIZE);
	assert(chest_gui_hotbar_base(ROWS_LARGE) + 9 == IRON_CHEST_SIZE);
	printf("  [5b] slot bases: 0/27/54 small, 0/54/81 large -- one formula, both sizes\n");
}

static void check_double_chest_routes_to_the_beta_gui(void) {
	char* t = read_source("source/block/block_chest.c");
	const char* open = strstr(t, "server_local_open_linked_inventory");
	assert(open && "the double-chest open call is gone");
	const char* end = strchr(open, ';');
	assert(end);
	size_t n = (size_t)(end - open);
	char* call = malloc(n + 1);
	assert(call);
	memcpy(call, open, n);
	call[n] = '\0';

	assert(strstr(call, "WINDOW_TYPE_CHEST")
		   && "the double chest must open as WINDOW_TYPE_CHEST so the client picks the Beta "
			  "GuiChest, not the fork's iron-chest screen");
	assert(!strstr(call, "WINDOW_TYPE_IRON_CHEST")
		   && "the double chest is back on the fork's iron-chest GUI");

	// ⚠ SAVE COMPAT: the routing INDEX must stay WINDOWC_IRON_CHEST. It is written into
	// persisted death_drop_provenance records (death_drop_journal.h `window` byte, written
	// by inventory_double_chest.c), so renumbering it to WINDOWC_CHEST would silently
	// invalidate every existing death-drop journal. Only the TYPE selects the screen.
	assert(strstr(call, "WINDOWC_IRON_CHEST")
		   && "the double chest's routing INDEX was changed -- that byte is persisted in "
			  "death-drop journals; only the window TYPE may change");
	free(call);
	free(t);

	// THREE places in server_local.c used to encode the double chest's window type -- the
	// OPEN contract whitelist (which would have REFUSED the open outright), the REOPEN
	// mapping (which would have flipped a recovered double chest back onto the fork's GUI
	// after a client window-allocation failure), and the routing index. Two of the three
	// were missed on the first pass. They are now ONE registry row (window_registry.h),
	// which is what both paths read, so this greps the row instead of the dispatch chains.
	// test_window_registry pins the full row; this keeps the double-chest-specific reason
	// attached to the chest layout it belongs to.
	char* reg = read_source("source/network/window_registry.h");
	const char* row = strstr(reg, "X(double_chest,");
	assert(row && "the double-chest registry row is gone");
	const char* row_end = strstr(row, "WINDOW_KEY_BLOCK)");
	assert(row_end && "the double-chest registry row is truncated");
	size_t cn = (size_t)(row_end - row);
	char* cbuf = malloc(cn + 1);
	assert(cbuf);
	memcpy(cbuf, row, cn);
	cbuf[cn] = '\0';
	assert(strstr(cbuf, "WINDOW_TYPE_CHEST") && !strstr(cbuf, "WINDOW_TYPE_IRON_CHEST")
		   && "the double chest's registry row still says WINDOW_TYPE_IRON_CHEST -- with "
			  "block_chest.c sending WINDOW_TYPE_CHEST the OPEN contract would REFUSE it, "
			  "and REOPEN would flip it onto the fork's GUI");
	assert(strstr(cbuf, "WINDOWC_IRON_CHEST")
		   && "the double chest's routing INDEX was changed in the registry -- that byte is "
			  "persisted in death-drop journals; only the window TYPE may change");
	free(cbuf);
	free(reg);

	// The fork's iron chest is a real, separate net-new container -- its screen stays.
	char* iron = read_source("source/game/gui/screen_iron_chest.c");
	assert(strstr(iron, "IRON_CHEST_SIZE")
		   && "screen_iron_chest is still the GUI for the actual (net-new) iron chest");
	free(iron);
	printf("  [5c] double chest opens WINDOW_TYPE_CHEST, keeps its persisted window index\n");
}

// --- (6) the ASSET has Beta's geometry, or the two-rect draw samples nothing ----------
// This is the half that cannot be fixed in code. Asserts on BYTES, not on file existence --
// the pack-builder lesson from P0(a): a gate that proves a path exists proves nothing about
// what is in it.
struct sheet {
	unsigned char* rgba;
	unsigned w, h;
};

static bool sheet_load(struct sheet* s, const char* path) {
	return lodepng_decode32_file(&s->rgba, &s->w, &s->h, path) == 0;
}

static int lum_at(const struct sheet* s, unsigned x, unsigned y) {
	const unsigned char* p = s->rgba + ((size_t)y * s->w + x) * 4;
	return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
}

static int alpha_at(const struct sheet* s, unsigned x, unsigned y) {
	return s->rgba[((size_t)y * s->w + x) * 4 + 3];
}

// A 1px cell bevel: markedly darker than the row above AND below, across the window width.
static bool is_bevel_row(const struct sheet* s, unsigned v) {
	if(v == 0 || v + 1 >= s->h)
		return false;
	long here = 0, above = 0, below = 0;
	for(unsigned u = 9; u < 167; u++) {
		here += lum_at(s, u, v);
		above += lum_at(s, u, v - 1);
		below += lum_at(s, u, v + 1);
	}
	const long n = 167 - 9;
	return here < above - 8 * n && here < below - 8 * n;
}

static void check_asset_geometry(const char* path, const char* label) {
	struct sheet sh;
	if(!sheet_load(&sh, path)) {
		printf("  [6] %s absent -- skipped\n", label);
		return;
	}
	assert(sh.w >= 176 && sh.h >= 222 && "a container sheet must hold 176x222");

	// SIX chest cell bevels at v = 17 + 18r. The bundled sheet had only three (16/34/52) and
	// at the wrong offset, which is why the Beta draw path could not be used against it.
	for(int r = 0; r < 6; r++) {
		unsigned v = (unsigned)(17 + 18 * r);
		if(!is_bevel_row(&sh, v)) {
			fprintf(stderr, "%s: no 1px cell bevel at v=%u (chest row %d)\n", label, v, r);
			assert(0 && "chest sheet lacks Beta's six-row cell geometry");
		}
	}
	// The player panel's own bevels, in the v=126..221 band the second rect samples.
	const unsigned panel[] = {139, 157, 175, 197};
	for(size_t i = 0; i < sizeof(panel) / sizeof(panel[0]); i++) {
		if(!is_bevel_row(&sh, panel[i])) {
			fprintf(stderr, "%s: no 1px cell bevel at v=%u (player panel)\n", label,
					panel[i]);
			assert(0 && "chest sheet lacks the player-panel row geometry");
		}
	}

	// THE SINGLE ASSERTION THAT CATCHES THE OLD FILE ON ITS OWN: the band the second rect
	// samples must not be transparent. The bundled sheet was alpha 0 from v=166 down.
	int opaque = 0;
	for(unsigned v = 166; v <= 221; v++)
		for(unsigned u = 0; u < 176; u++)
			if(alpha_at(&sh, u, v) > 0)
				opaque++;
	assert(opaque > 0
		   && "v=166..221 is fully transparent -- Beta's second background rect samples "
			  "v=126..221, so this window would draw ~56 rows of nothing");

	// Columns at u = 7 + 18c, c=0..8 (the X axis the bundled sheet already had right).
	for(int c = 0; c < 9; c++) {
		unsigned u = (unsigned)(7 + 18 * c);
		long here = 0, left = 0, right = 0;
		for(unsigned v = 20; v < 110; v++) {
			here += lum_at(&sh, u, v);
			left += lum_at(&sh, u - 1, v);
			right += lum_at(&sh, u + 1, v);
		}
		const long n = 110 - 20;
		if(!(here < left - 8 * n && here < right - 8 * n)) {
			fprintf(stderr, "%s: no 1px cell bevel at u=%u (column %d)\n", label, u, c);
			assert(0 && "chest sheet lacks the column cell geometry");
		}
	}
	free(sh.rgba);
	printf("  [6] %s: 6 chest rows + panel rows + columns, v=166..221 populated\n", label);
}

int main(void) {
	printf("test_chest_layout: Beta 1.7.3 chest window (P2)\n");
	check_height();
	check_bg_rects();
	check_slot_origins();
	check_labels();
	check_siblings_not_harmonised();
	check_slot_bases_unify_both_sizes();
	check_double_chest_routes_to_the_beta_gui();
	check_asset_geometry("assets/gui/chest.png", "assets/gui/chest.png");
	// The authentic pack is gitignored (Mojang IP) and absent in CI -- opportunistic.
	check_asset_geometry("texturepacks/beta-1.7.3/gui/chest.png",
						 "texturepacks/beta-1.7.3/gui/chest.png");
	printf("test_chest_layout: PASS\n");
	return 0;
}
