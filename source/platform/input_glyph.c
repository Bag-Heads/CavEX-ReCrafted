/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include "input_glyph.h"

#include <string.h>

/* GLFW 3 key codes (glfw3.h GLFW_KEY_*) to the name drawn on the cap. Names
 * stay ASCII so every pack font can draw them; arrows are words because the
 * Beta font has no arrow glyphs. */
static const char* pc_key_name(int key) {
	static const char* const digits[]
		= {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
	static const char* const letters[]
		= {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
		   "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
	static const char* const fkeys[]
		= {"F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",  "F8",  "F9",
		   "F10", "F11", "F12", "F13", "F14", "F15", "F16", "F17", "F18",
		   "F19", "F20", "F21", "F22", "F23", "F24", "F25"};
	static const char* const keypad[]
		= {"Num0", "Num1", "Num2", "Num3", "Num4",
		   "Num5", "Num6", "Num7", "Num8", "Num9"};
	if(key >= 48 && key <= 57)
		return digits[key - 48];
	if(key >= 65 && key <= 90)
		return letters[key - 65];
	if(key >= 290 && key <= 314)
		return fkeys[key - 290];
	if(key >= 320 && key <= 329)
		return keypad[key - 320];
	switch(key) {
		case 32: return "Space";
		case 39: return "'";
		case 44: return ",";
		case 45: return "-";
		case 46: return ".";
		case 47: return "/";
		case 59: return ";";
		case 61: return "=";
		case 91: return "[";
		case 92: return "\\";
		case 93: return "]";
		case 96: return "`";
		case 256: return "Esc";
		case 257: return "Enter";
		case 258: return "Tab";
		case 259: return "Bksp";
		case 260: return "Ins";
		case 261: return "Del";
		case 262: return "Right";
		case 263: return "Left";
		case 264: return "Down";
		case 265: return "Up";
		case 266: return "PgUp";
		case 267: return "PgDn";
		case 268: return "Home";
		case 269: return "End";
		case 280: return "Caps";
		case 281: return "ScrLk";
		case 282: return "NumLk";
		case 283: return "PrtSc";
		case 284: return "Pause";
		case 330: return "Num.";
		case 331: return "Num/";
		case 332: return "Num*";
		case 333: return "Num-";
		case 334: return "Num+";
		case 335: return "Enter";
		case 336: return "Num=";
		case 340: return "Shift";
		case 341: return "Ctrl";
		case 342: return "Alt";
		case 343: return "Super";
		case 344: return "Shift";
		case 345: return "Ctrl";
		case 346: return "Alt";
		case 347: return "Super";
		case 348: return "Menu";
		default: return NULL;
	}
}

/* Atlas cell of a PC glyph code; -1 if the code is not one. */
static int pc_code_cell(int code) {
	if(code < INPUT_GLYPH_PC_KEY_BASE || code >= INPUT_GLYPH_PC_END)
		return -1;
	int key = code - INPUT_GLYPH_PC_KEY_BASE;
	if(key >= 1000) {
		switch(code) {
			case INPUT_GLYPH_PC_MOUSE_LEFT: return INPUT_GLYPH_CELL_MOUSE_LEFT;
			case INPUT_GLYPH_PC_MOUSE_RIGHT: return INPUT_GLYPH_CELL_MOUSE_RIGHT;
			case INPUT_GLYPH_PC_MOUSE_WHEEL: return INPUT_GLYPH_CELL_MOUSE_WHEEL;
			default: return 43; /* side buttons: no art, generic MSE cell */
		}
	}
	const char* name = key >= 32 && key <= 348 ? pc_key_name(key) : NULL;
	if(!name)
		return -1;
	return strlen(name) > 1 ? INPUT_GLYPH_CELL_KEY_WIDE : INPUT_GLYPH_CELL_KEY_1U;
}

const char* input_glyph_label_for_code(int code, bool* wide) {
	if(wide)
		*wide = false;
	int cell = pc_code_cell(code);
	if(cell == INPUT_GLYPH_CELL_KEY_1U || cell == INPUT_GLYPH_CELL_KEY_WIDE) {
		if(wide)
			*wide = cell == INPUT_GLYPH_CELL_KEY_WIDE;
		return pc_key_name(code - INPUT_GLYPH_PC_KEY_BASE);
	}
	switch(code) { /* Classic shoulder caps (approved-controls 10-31) */
		case 22: return "L";
		case 23: return "R";
		case 14: return "ZL";
		case 15: return "ZR";
		default: return NULL;
	}
}

int input_glyph_key(enum input_category category, int key) {
	if(category == INPUT_CAT_NONE) {
		/* GLFW keyboard/mouse namespace, kept independent of Wii key codes.
		 * A named key or a mouse button becomes a PC glyph code (cap + name);
		 * a GLFW key with no name in the table keeps the generic KEY cell 42
		 * rather than a blank cap with nothing on it. */
		if(key >= 32 && key <= 348)
			return pc_key_name(key) ? INPUT_GLYPH_PC_KEY_BASE + key : 42;
		return key >= 1000 && key <= 1007 ? INPUT_GLYPH_PC_KEY_BASE + key : -1;
	}
	if(!input_owner_accepts(category, key))
		return -1;
	if(key >= 900) {
		static const int sticks[] = {17, 18, 19, 40, 41};
		return sticks[(key - 900) / 10];
	}
	if(key >= 400) {
		static const int gc[]
			= {24, 24, 24, 24, 32, 33, 34, 35, 36, 37, 38, 39};
		return gc[key - 400];
	}
	if(key >= 200) {
		static const int classic[]
			= {24, 24, 24, 24, 10, 11, 12, 13, 14, 15, 22, 23, 5, 6, 4};
		return classic[key - 200];
	}
	if(key >= 100)
		return key == 100 ? 8 : 9;
	static const int remote[] = {24, 24, 24, 24, 0, 1, 2, 3, 5, 6, 4};
	return remote[key];
}

int input_glyph_binding(enum input_category category, const int* keys,
						size_t count) {
	for(size_t k = 0; keys && k < count && k < INPUT_BINDINGS_PER_ACTION; k++) {
		int key = keys[k];
		if(key >= 1008) {
			int modifier = input_glyph_key(category, key / 1000);
			int button = input_glyph_key(category, key % 1000);
			/* The chord packing holds two plain cells only. A PC glyph code
			 * does not fit it; no shipped PC binding is a chord, so such a
			 * chord shows its PRESS key alone rather than a wrong pair. */
			if(modifier >= INPUT_GLYPH_CELLS || button >= INPUT_GLYPH_CELLS) {
				if(button >= 0)
					return button;
				continue;
			}
			if(modifier >= 0 && button >= 0)
				return (modifier + 1) * INPUT_GLYPH_CELLS + button;
		} else {
			int cell = input_glyph_key(category, key);
			if(cell >= 0)
				return cell;
		}
	}
	return -1;
}

/* See the header for why this exists next to input_glyph_key rather than
 * inside it: the shared cell 24 is the right answer for an ordinary prompt and
 * the wrong one for a prompt that draws a single cell and no direction word.
 * Nothing above changes; this is a second, read-only view of the SAME four
 * codes per family, and the caller decides which view it wants. */
int input_glyph_dpad_direction_cell(enum input_category family, int code) {
	/* The same family gate input_glyph_key applies, for the same reason: a
	 * code its family cannot press is not that family's D-pad. */
	if(!input_owner_accepts(family, code))
		return -1;
	int direction;
	if(family == INPUT_CAT_GAMECUBE)
		direction = code - 400;
	else if(family == INPUT_CAT_CLASSIC_CONTROLLER)
		direction = code - 200;
	else if(family == INPUT_CAT_NUNCHUK || family == INPUT_CAT_WIIMOTE)
		direction = code;
	else
		return -1;
	/* 0 Up, 1 Down, 2 Left, 3 Right -- the order every family's four D-pad
	 * codes are laid out in (docs/controls_sheet.md's binding tables). */
	return direction >= 0 && direction <= 3 ? 25 + direction : -1;
}

bool input_glyph_cells(int glyph, int* modifier, int* button) {
	int pc = pc_code_cell(glyph);
	if(pc >= 0) {
		if(!modifier || !button)
			return false;
		*modifier = -1;
		*button = pc;
		return true;
	}
	if(glyph < 0 || glyph >= INPUT_GLYPH_CELLS * (INPUT_GLYPH_CELLS + 1)
	   || !modifier || !button)
		return false;
	*modifier = glyph / INPUT_GLYPH_CELLS - 1;
	*button = glyph % INPUT_GLYPH_CELLS;
	return true;
}

void input_glyph_frame(struct input_player_frame* frame,
					   int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION],
					   const size_t* lengths) {
	for(int b = 0; b < IB_COUNT; b++)
		frame->glyphs[b]
			= input_glyph_binding(frame->category, map[b], lengths[b]);
}
