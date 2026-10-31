/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef PLATFORM_INPUT_GLYPH_H
#define PLATFORM_INPUT_GLYPH_H

#include "input_owner.h"

#define INPUT_GLYPH_CELLS 64
/* A single cell keeps its historical index. A chord packs (modifier + 1)
 * above the low six bits. -1 remains unbound. This is an atlas encoding,
 * not another device taxonomy; input_category still selects the family. */
/* PC keyboard/mouse glyph CODES (maintainer pick ctl1031-pc-glyphs, style A).
 * They sit above every cell/chord code (0 .. CELLS*(CELLS+1)-1), so a code is
 * never mistaken for a chord. input_glyph_key(INPUT_CAT_NONE, k) answers
 * INPUT_GLYPH_PC_KEY_BASE + k for a GLFW key (32..348) or mouse button
 * (1000..1007), and input_glyph_cells() decodes such a code to its atlas cell
 * (modifier -1): a blank 1u cap, the blank WIDE cap (64 px, cells 45-46), or
 * a mouse picture. The key NAME is not in the art: input_glyph_label_for_code
 * gives the text to draw on the cap in the active pack font. */
#define INPUT_GLYPH_PC_KEY_BASE (INPUT_GLYPH_CELLS * (INPUT_GLYPH_CELLS + 1))
#define INPUT_GLYPH_PC_MOUSE_LEFT (INPUT_GLYPH_PC_KEY_BASE + 1000)
#define INPUT_GLYPH_PC_MOUSE_RIGHT (INPUT_GLYPH_PC_KEY_BASE + 1001)
/* Mouse wheel: no binding code exists for it; GLFW button 2 (1002, the wheel
 * press) resolves here, and a scroll hint may name it directly. */
#define INPUT_GLYPH_PC_MOUSE_WHEEL (INPUT_GLYPH_PC_KEY_BASE + 1002)
#define INPUT_GLYPH_PC_END (INPUT_GLYPH_PC_KEY_BASE + 1008)
/* Atlas cells in gui/approved-controls.png. */
#define INPUT_GLYPH_CELL_KEY_1U 44
#define INPUT_GLYPH_CELL_KEY_WIDE 45 /* 64x32: cells 45 AND 46 */
#define INPUT_GLYPH_CELL_MOUSE_LEFT 47
#define INPUT_GLYPH_CELL_MOUSE_RIGHT 48
#define INPUT_GLYPH_CELL_MOUSE_WHEEL 49
int input_glyph_key(enum input_category category, int key);
/* Text drawn on a blank cap for a SINGLE glyph code (not a packed chord; for a
 * chord pass each half from input_glyph_cells, which equal their codes below
 * 64): a PC key name ("A", "Space", "F3", ...), or the Classic shoulder
 * letters for cells 22/23/14/15 ("L"/"R"/"ZL"/"ZR"). NULL means no overlay
 * (mouse pictures, every lettered cell). *wide (may be NULL) is set true only
 * for the 64-px wide cap. The caller drops to the 1x font when the text is
 * wider than the face minus 10 px. */
const char* input_glyph_label_for_code(int code, bool* wide);
int input_glyph_binding(enum input_category category, const int* keys,
						size_t count);
bool input_glyph_cells(int glyph, int* modifier, int* button);
/* The four D-pad DIRECTIONS as their own official cells: 25 Up, 26 Down,
 * 27 Left, 28 Right. READ-ONLY and purely additive -- input_glyph_key's table
 * is unchanged, so every ordinary prompt (menu hints, footers, control icons)
 * keeps drawing the shared whole-D-pad cell 24 it always has.
 *
 * WHY a second mapping: cell 24 is the WHOLE D-pad, so a prompt that resolves
 * a single binding through input_glyph_key cannot say which way to push, and
 * the retired ruling-8 chat hint had to spell the direction in words next to
 * it. A prompt that draws ONE cell and no word -- the H-C2 chat socket -- asks
 * for the direction itself instead.
 *
 * `family` selects the code space exactly as input_glyph_key does: Remote
 * (INPUT_CAT_NUNCHUK) 0-3, Classic 200-203, GameCube 400-403, in Up/Down/
 * Left/Right order. Any other code, a code that family cannot press, the PC
 * keyboard family and INPUT_CAT_WIIMOTE (which input_glyph_key itself refuses,
 * resolving no glyph at all) answer -1. */
int input_glyph_dpad_direction_cell(enum input_category family, int code);
void input_glyph_frame(struct input_player_frame* frame,
					   int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION],
					   const size_t* lengths);
/* Explicit player; absent guests never inherit the host's bindings. */
void input_prompt_frame(unsigned player, struct input_player_frame* frame);

#endif
