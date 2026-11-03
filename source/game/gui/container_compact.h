/* Viewport-local manual containers. No global player, window or cursor owner. */
#ifndef CONTAINER_COMPACT_H
#define CONTAINER_COMPACT_H

#include "../../item/window_container.h"
#include "container_menu.h"

enum container_layout_mode { CONTAINER_LAYOUT_FULL, CONTAINER_LAYOUT_COMPACT };
enum container_layout_mode container_layout_for_height(int height);

/* Fixed views: all 36 player slots, or up to 27 container slots + hotbar.
 * Allocate one instance per player. Focus is remembered separately per tab. */
struct container_compact {
	struct container_menu menu;
	struct container_slot slots[45];
	size_t count, main_base, storage_count;
	unsigned view, view_count;
	size_t selection[3];
	int width, height;
	enum window_type type;
	bool active;
};

struct container_glyphs;
/* Does the input's pointer press hit the split inventory row's "Crafting" cell
 * (the page swap)? For the host's page_swap test, which runs before the
 * step resolves row clicks (Sol review 10-31 item 1). */
bool container_compact_row_page_swap(const struct container_compact*,
	const struct window_container*, const struct container_glyphs*,
	const struct container_input*);

/* The caller supplies glyphs resolved for THIS player/controller. Negative
 * glyphs hide an unbound action's symbol. Order matches the enum below. */
enum container_prompt { CONTAINER_PROMPT_QUICK, CONTAINER_PROMPT_PICK,
	CONTAINER_PROMPT_SPLIT, CONTAINER_PROMPT_DROP, CONTAINER_PROMPT_BACK,
	CONTAINER_PROMPT_PREVIOUS, CONTAINER_PROMPT_NEXT, CONTAINER_PROMPT_PAGE_SWAP,
	CONTAINER_PROMPT_COUNT };
struct container_glyphs { int symbols[CONTAINER_PROMPT_COUNT]; };
/* The glyphs as a prompt frame (unlisted roles -1) for the shared hint row. */
void container_compact_glyph_frame(const struct container_glyphs* glyphs,
	struct input_player_frame* frame);
void container_compact_active_glyphs(struct container_glyphs* glyphs);
/* critique-1028: the same, from `player`'s OWN prompt frame (a split pane
 * must never borrow player 1's controller family). */
void container_compact_owner_glyphs(unsigned player,
	struct container_glyphs* glyphs);
/* ui-build-1026/T13 (approved V1, design-round-1026e): the personal
 * inventory's page header "Inventory | Crafting", active page marked. It
 * carries NO glyph -- the page-swap hint lives only in the bottom legend
 * (maintainer 2026-10-27). `light` = drawn over the washed world (full
 * layout), otherwise ink on the paper panel (split). Returns drawn width. */
int container_page_header_width(bool light);
int container_page_header(int x, int y, bool crafting, bool light);
/* ci-1101/07: the header's y inside its 480 (full) / 240 (split) pane. Full
 * screen it sits in the TV-safe band -- top >= 24, the same margin the
 * bottom hint row keeps (GUTIL_HINT_BOTTOM_FULL); B3 had moved it to 10 and
 * the real Wii lost the top half of the glyphs to overscan. The header is
 * CONTAINER_PAGE_HEADER_H tall (16 px glyphs + the 2 px underline at y+18);
 * the split ink plate starts 3 px above y. */
#define CONTAINER_PAGE_HEADER_H 20
int container_page_header_y(bool full);
/* Kept for the compact notice contract: the notice reservation starts at
 * this + 24 + 8, i.e. 8 px after the split page header's right edge. The
 * T12/T13 header has no Previous/Next glyphs any more. */
int container_compact_inventory_next_x(bool crafting_view);
void container_compact_player_glyphs(struct container_glyphs* glyphs,
	const struct input_player_frame* frame);

bool container_compact_open(struct container_compact* view,
	enum window_type type, size_t slot_count, size_t hotbar,
	int width, int height);
/* Directional focus on `slot`, switching to the view that shows it. */
bool container_compact_focus_slot(struct container_compact* view, size_t slot);
/* Returns true when close was sent. Input is viewport-local and already
 * assigned to the player. The caller owns screen transitions and transport. */
bool container_compact_step(struct container_compact* view,
	struct window_container* wc, uint8_t window,
	const struct container_input* input, float dt,
	container_send_fn send, void* user);
/* Draw in framebuffer coordinates. All content stays inside the supplied
 * view rectangle; no viewport/scissor state is changed behind the caller. */
void container_compact_render(const struct container_compact* view,
	struct window_container* wc, int x, int y,
	const struct container_glyphs* glyphs);

/* Notice bounds share the actual header controls and Sort entry. */
void container_compact_notice_bounds(const struct container_compact*,
 const struct window_container*, int desired_width, struct gutil_button_bounds*);

void container_inventory_render(const struct container_compact*,struct window_container*,
 int x,int y,const struct input_player_frame*);
/* CavEX portrait rotation in radians, from this owner's viewport-local focus. */
void container_inventory_look(const struct container_compact*,float* pitch,float* yaw);

/* Adapter for the current single-player screen loop. The split-screen host
 * should use open/step/render above with its own input, window and glyphs. */
bool container_compact_screen_open(struct container_compact* view,
	uint8_t window, int width, int height);
void container_compact_screen_update(struct container_compact* view,
	uint8_t window, float dt);
void container_compact_screen_render(struct container_compact* view,
	uint8_t window);

/* Approved Manual Grid: one instance per owner, same authoritative slot RPCs. */
struct manual_grid {
 struct container_menu menu;
 struct container_slot slots[46];
 size_t count, selection[2];
 unsigned grid, page, owner_index;
 int width,height;
 bool active;
};
void manual_grid_open(struct manual_grid*,unsigned grid,int width,int height);
/* True means return to the retained recipe, never close the window. */
bool manual_grid_step(struct manual_grid*,struct window_container*,uint8_t window,
 const struct container_input*,const struct container_glyphs*,float dt,container_send_fn,void*);
void manual_grid_render(const struct manual_grid*,struct window_container*,int x,int y,
 const struct container_glyphs*);

#endif
