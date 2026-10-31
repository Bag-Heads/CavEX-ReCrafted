/* Native container input/presentation. Server inventory rules live elsewhere. */
#ifndef CONTAINER_MENU_H
#define CONTAINER_MENU_H

#include <stddef.h>
#include <stdint.h>

#include "../../graphics/menu_focus.h"

struct window_container;
struct client_session;

struct container_slot {
	int x, y; /* panel-relative physical pixels */
	size_t slot; /* authoritative window slot id */
	uint8_t extent; /* zero inherits the menu size */
};

/* Depth of the commit-cue ring below. Ordinary clicks are press edges, so at
 * most one transfer is issued per frame; four in flight covers the deepest
 * pipelining a player can reach inside the dispatcher's own 90 ms item-cue
 * repeat guard, which would collapse anything deeper into one audible cue
 * anyway. Overflow drops the OLDEST arm and counts it in item_cue_dropped. */
#define CONTAINER_MENU_CUE_PENDING 4

struct container_menu_cue {
	uint32_t serial;	 /* wc->rejection_serial the click was sent under */
	uint32_t generation; /* wc->generation the click was sent under */
	uint16_t action;	 /* that click's own window action id */
	uint8_t cue;		 /* enum menu_sound_event this transfer owes */
};

struct container_menu {
	int slot_extent; /* zero retains legacy 32px hit targets */
	struct client_session* sort_owner;
	bool sort_focus, sort_open, sort_compact;
	bool return_only; /* acknowledged cursor return without closing the window */
	bool sort_disabled; /* presenter has no Sort control */
	unsigned sort_operation;
	int sort_x, sort_y;
	struct gutil_menu_focus focus;
	int selection_origin; /* enum gutil_menu_origin; keep gui_util out of this header */
	int pointer_hit; /* raw hovered slot index, even while the ring owns focus */
	struct gutil_menu_repeat repeat;
	size_t selected;
	bool pointer_available, pointer_on_slot, send_failed;
	bool close_requested, return_blocked, suspended;
	/* close_force: Close was pressed while the window was blocked (pending
	 * click/correction/ack), so it closes at once as Beta does
	 * (EntityClientPlayerMP.java:132-135). server_wait: the last refused
	 * action was refused because the server still owes a correction, not
	 * because a send failed ("Waiting for server..." label). */
	bool close_force, server_wait;
	uint32_t close_rejection_serial;
	/* Commit-timed transfer cues (sound-events.md: "Actual transfer/save
	 * success hooks must come from authoritative completed operations").
	 * A sent user transfer click that predicted a real mutation arms ONE ring
	 * entry carrying its own action id, the rejection serial AND the window
	 * generation it was sent under, plus which approved cue it owes:
	 * MENU_SOUND_ITEM for an ordinary transfer, MENU_SOUND_SELECT for the
	 * crafting RESULT slot, whose completed craft keeps the approved
	 * confirmation cue ("Keep the approved crafting cues and recipes
	 * unchanged", sound-events.md; "completed successful craft/sort",
	 * crafting-chests.md). container_menu_step plays each entry only once
	 * THAT action's authoritative accept lands.
	 *
	 * A RING, not a single slot: nothing gates ordinary (non quick-move)
	 * click pipelining, so a player can have several unacknowledged transfers
	 * in flight; a single arm was overwritten by the next click and the first
	 * accept then played nothing. Entries are armed and resolved strictly
	 * FIFO -- action ids increase and the server acks in order.
	 * Never a wire field, never presentation. */
	struct container_menu_cue item_cue[CONTAINER_MENU_CUE_PENDING];
	uint8_t item_cue_head, item_cue_count;
	/* Arms discarded because more than CONTAINER_MENU_CUE_PENDING transfers
	 * were in flight at once. Counted, never silently ignored. */
	uint16_t item_cue_dropped;
	/* One-frame flag: the sort panel's own Back control closed the panel, so
	 * the panel-change cue below is the Back cue, not the section cue
	 * ("Back on successful return/resume"; "A cancelled discard prompt uses
	 * Back ... opening it uses category"). */
	bool sort_back;
	unsigned return_steps;
	size_t section_selection[3];
	float pointer_x, pointer_y, pointer_angle;
	/* Q7 (maintainer 2026-10-31): the full-screen 90-slot chest shows ONE
	 * 27-slot half at a time, paged like the split chest. page_count > 1 makes
	 * the header's "<" ">" arrows pointer targets and turns Section L/R into
	 * a half swap of `page` (the owning screen refills its slots from it);
	 * page_count <= 1 (every other window) keeps Section L/R as the focus
	 * section jump and hit-tests NO header rect (the unpaged chest draws
	 * none). */
	unsigned page, page_count;
};

/* Parked 2026-10-25 (maintainer): always false in release, so no screen draws
 * or focuses Sort. See container_menu.c (CONTAINER_MENU_SORT_UI). */
bool container_menu_sort_supported(const struct window_container*);
struct input_player_frame;
void container_menu_sort_layout(struct container_menu*, int width, int height,
	const struct window_container*);
void container_menu_sort_render(const struct container_menu*, struct window_container*,
	int x, int y, int width, int height, const struct input_player_frame*);

struct gutil_button_bounds;
void container_menu_sort_entry_bounds(const struct container_menu*,
 const struct window_container*, int x, int y, struct gutil_button_bounds*);
/* Frozen chest-only presentation; rect order: Prev, title, Next, Sort. */
void container_chest_panel(int x, int y, int width, int height);
void container_chest_header_rects(int x, int y, int width, const char* title,
	struct gutil_button_bounds* out);
/* Title, plus plain "<" ">" pointer arrows at rects 0/2 when `paged`. */
void container_chest_header_paged(int x, int y, int width, const char* title,
	bool paged, const struct input_player_frame*);
/* The full-screen chest's header title: "Chest" for the 63-slot window,
 * "Chest 1" / "Chest 2" for the paged 90-slot one (menu->page). The painter
 * and the pointer hit-test both use it, so the arrows sit where they are
 * drawn. */
const char* container_chest_title(const struct container_menu*,
	const struct window_container*);
/* Player 1's full-viewport container screens: draws `window`'s bottom row. */
void container_menu_row(uint8_t window);
void container_menu_arrow(const struct gutil_button_bounds* rect, const char* arrow);
void container_chest_slot(int x, int y);
/* `bag` 0: the full-screen white label column; > 0: the ink label centred in
 * a `bag` px tall column (the compact chest's info column). */
void container_chest_info(int x, int y, int width, int bag, const char* text);

int container_slot_extent(const struct container_menu*, const struct container_slot*);

struct window_container;
struct server_rpc;
struct input_player_frame;
/* One player's frame: directions are held; ordinary actions are press edges.
 * Pointer coordinates are local to that player's viewport;
 * the caller rejects pointers outside it before setting pointer_valid. */
struct container_input {
	bool suspended;
	bool up, down, left, right;
	bool close, pick, split, drop, page, previous, quick_move;
	bool page_pressed, page_held, page_released, page_swap, page_hold;
	unsigned page_held_ms; /* menu-only gesture; HOLD is not wired to Sort */
	bool pointer_valid;
	float pointer_x, pointer_y, pointer_angle;
};
typedef bool (*container_send_fn)(void* user, struct server_rpc* rpc);

void container_menu_read_input(struct container_input* input,
	int x, int y, int width, int height);
void container_menu_read_player(struct container_input* input,
	const struct input_player_frame* frame, int x, int y, int width, int height);
void container_menu_read_player_owned(struct container_input* input, unsigned player,
	const struct input_player_frame* frame, int x, int y, int width, int height);
bool container_menu_send(void* user, struct server_rpc* rpc);
/* Returns true only when a close request was sent. No global screen change. */
bool container_menu_step(struct container_menu* menu,
	const struct container_slot* slots, size_t count, int x, int y,
	struct window_container* wc, uint8_t window,
	const struct container_input* input, float dt,
	container_send_fn send, void* user);
void container_menu_focus_step(struct container_menu* menu,
	const struct container_slot* slots, size_t count, int x, int y,
	const struct container_input* input, float dt);
bool container_menu_section_step(struct container_menu* menu,
	const struct container_slot* slots, size_t count, size_t main_base,
	const struct container_input* input);
void container_menu_label(const struct container_menu* menu,
	struct window_container* wc, size_t slot, char* label, size_t capacity);

/* ---- The container hint row (controller UI redesign 2026-10-31, part B) ----
 * Every container screen shows its button prompts in ONE bottom-left row,
 * drawn by the shared gutil_hint_row helper (gui_util.h), never inside the
 * panel. The row is also a pointer target (WIRING-PLAN Q2): clicking a cell
 * performs that cell's action on the focused slot. The layout is resolved by
 * the same function for the painter and the pointer, so a hit rectangle is
 * exactly what is drawn. Frame = the whole viewport or one split pane. */
enum container_row_kind {
	CONTAINER_ROW_INVENTORY, /* Crafting / Pick / Split / Close (+ Drop) */
	CONTAINER_ROW_STORAGE,   /* Pick / Split / Quick Move / Section / Close (+ Drop) */
	CONTAINER_ROW_MACHINE,   /* Pick / Split / Quick Move / Close (+ Drop) */
	CONTAINER_ROW_MANUAL,    /* Pick / Split / Quick Move or Drop / Recipes */
};
enum container_row_action {
	CONTAINER_ROW_PICK, CONTAINER_ROW_SPLIT, CONTAINER_ROW_QUICK,
	CONTAINER_ROW_DROP, CONTAINER_ROW_CLOSE, CONTAINER_ROW_PAGE_SWAP,
	CONTAINER_ROW_SECTION,
};
#define CONTAINER_ROW_ITEMS 6
enum container_row_kind container_row_kind_for(const struct window_container* wc);
struct gutil_hint_row;
/* Lays the row out for `kind` (`held`: the cursor holds a stack; `paged`:
 * the Section L+R pair is shown). Fills `row` and, for each laid-out item
 * index, `actions[item]`. Returns the cell count. */
int container_row_layout(enum container_row_kind kind, bool held, bool paged,
	const struct input_player_frame* frame, int x, int y, int width, int height,
	struct gutil_hint_row* row, enum container_row_action actions[CONTAINER_ROW_ITEMS]);
void container_row_draw(enum container_row_kind kind, bool held, bool paged,
	const struct input_player_frame* frame, int x, int y, int width, int height);
/* A pointer press on a row cell becomes exactly that cell's action on the
 * focused slot (the pointer is consumed so no slot under it is clicked).
 * Returns the action taken, or -1 (input untouched). */
int container_row_click(enum container_row_kind kind, bool held, bool paged,
	const struct input_player_frame* frame, int x, int y, int width, int height,
	struct container_input* input);

// Directions: -1 up, +1 down, -2 left, +2 right. No wrap; empty slots count.
size_t container_menu_neighbor(const struct container_slot* slots, size_t count,
							   size_t selected, int direction);

/* READ-ONLY OBSERVATION (push-0921/B4b, Astra gate 1 Lane B item 3: "authorize
 * only the minimal observation seams"). Where DIRECTIONAL focus currently
 * sits, as a window-0 slot number -- main 9..35, hotbar 36..44, which is the
 * numbering the protocol and mc173-diag's receipts both use.
 *
 * Returns -1, meaning "not on one of the player's own 36 stacks", when:
 *   - the focus is on the craft output, a crafting slot or an armor slot
 *     (window slots 0..8): they are not the player's carried inventory and an
 *     inventory-move script must never treat one as its source or target;
 *   - the POINTER owns focus (gutil_menu_pointer_focus). `selected` is then
 *     the cursor's answer, not the directional one the caller is walking, and
 *     reporting it would let a script believe a keypress landed;
 *   - the menu is suspended, sorting or closing, or `selected` is out of
 *     range.
 *
 * Valid for the PLAYER-INVENTORY window. A chest's own slots use a different
 * numbering (its container first, then the player's 36), so a chest menu would
 * be answered in that window's numbers; screen_inventory.c is the only caller.
 * Observes only -- changes nothing, and nothing in the game branches on it. */
int container_menu_focus_player_slot(const struct container_menu* menu,
									 const struct container_slot* slots,
									 size_t count);
void container_menu_update(struct container_menu* menu,
	const struct container_slot* slots, size_t count, int x, int y,
	uint8_t window, float dt);
void container_menu_item_name(const struct container_menu* menu, uint8_t window,
	size_t slot, int width);

#endif
