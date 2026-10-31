/*
	Copyright (c) 2023 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../../platform/input_owner.h"
#include <assert.h>
#include <math.h>
#include <string.h>

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "widget_osk.h"

/* Cell geometry in logical units. Grid cells are uniform; the function row
 * has per-key widths that sum to the same total. */
#define OSK_CELL_W 13
#define OSK_CELL_H 13
#define OSK_TOTAL_W (OSK_COLS * OSK_CELL_W)

/* '\0' cells are non-selectable blanks (design doc: the symbols page does
 * not fill every slot). */
static const char osk_pages[OSK_PAGES][OSK_ROWS][OSK_COLS] = {
	{
		{'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
		{'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
		{'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '-'},
		{'z', 'x', 'c', 'v', 'b', 'n', 'm', '_', '.', '\''},
	},
	{
		{'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
		{'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'},
		{'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', '-'},
		{'Z', 'X', 'C', 'V', 'B', 'N', 'M', '_', '.', '\''},
	},
	{
		{'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'},
		{'`', '~', '[', ']', '{', '}', '<', '>', '\\', '|'},
		{';', ':', '"', '/', '?', '=', '+', ',', 0, 0},
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	},
	/* OSK_PAGE_NUM -- the seed page (widget_osk.h). Digits and '-' only:
	 * exactly Java 6 Long.parseLong's alphabet [740c583 GuiCreateWorld
	 * .actionPerformed]. Deliberately sparse; the blank rows are skipped by
	 * osk_step_row and never drawn, so this renders as a compact keypad. */
	{
		{'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
		{'-', 0, 0, 0, 0, 0, 0, 0, 0, 0},
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	},
};

enum osk_fn {
	OSK_FN_SHIFT = 0,
	OSK_FN_SYM,
	OSK_FN_SPACE,
	OSK_FN_BACK,
	OSK_FN_NEXT,
	OSK_FN_DONE,
};

static const struct {
	const char* label;
	int width; /* logical units; must sum to OSK_TOTAL_W */
} osk_fn_keys[OSK_FN_KEYS] = {
	[OSK_FN_SHIFT] = {"Shift", 26}, [OSK_FN_SYM] = {"Sym", 20},
	[OSK_FN_SPACE] = {"Space", 34}, [OSK_FN_BACK] = {"<-", 16},
	[OSK_FN_NEXT] = {"Next", 20},   [OSK_FN_DONE] = {"OK", 14},
};

static bool osk_cell_selectable(const struct osk_state* o, int row, int col);
static void osk_step_col(struct osk_state* o, int dir);

void osk_reset_page(struct osk_state* o, char* buf, size_t buf_cap,
					int beta_max, int page) {
	assert(o && buf && buf_cap > 0 && beta_max >= 0);
	assert(page >= 0 && page < OSK_PAGES);
	o->buf = buf;
	o->buf_cap = buf_cap;
	o->beta_max = beta_max;
	o->page = page;
	o->row = 1;
	o->col = 0;
	/* Row 1 col 0 is a letter on the lower/upper pages but may be blank on a
	 * sparse page; park on the first selectable cell instead of leaving the
	 * cursor somewhere osk_activate would ignore. */
	if(!osk_cell_selectable(o, o->row, o->col))
		osk_step_col(o, 1);
}

void osk_reset(struct osk_state* o, char* buf, size_t buf_cap, int beta_max) {
	osk_reset_page(o, buf, buf_cap, beta_max, OSK_PAGE_LOWER);
}

enum osk_event osk_feed_char(struct osk_state* o, char c) {
	assert(o && o->buf);
	size_t len = strlen(o->buf);

	/* [740c583 GuiTextField.textboxKeyTyped]: backspace (keycode 14 there)
	 * removes the LAST char only -- Beta text fields are append-only, no
	 * cursor movement. */
	if(c == '\b') {
		if(len == 0)
			return OSK_EV_NONE;
		o->buf[len - 1] = 0;
		return OSK_EV_CHANGED;
	}

	/* '\t' -> selectNextField, '\r' -> confirm [740c583 GuiTextField
	 * .textboxKeyTyped / GuiCreateWorld.keyTyped], ESC -> close [740c583
	 * GuiScreen.keyTyped key 1]. The embedder acts. */
	if(c == '\t')
		return OSK_EV_NEXT;
	if(c == '\r' || c == '\n')
		return OSK_EV_DONE;
	if(c == 27)
		return OSK_EV_CANCEL;

	/* Beta's filter is membership in font.txt's printable set; ours is
	 * printable ASCII (the pack font's coverage). */
	if(c < 32 || c > 126)
		return OSK_EV_NONE;

	if(len + 1 >= o->buf_cap)
		return OSK_EV_NONE;

	/* The Beta max rule: `length < max || max == 0`
	 * [740c583 GuiTextField.textboxKeyTyped]. */
	if(o->beta_max > 0 && (int)len >= o->beta_max)
		return OSK_EV_NONE;

	o->buf[len] = c;
	o->buf[len + 1] = 0;
	return OSK_EV_CHANGED;
}

static bool osk_cell_selectable(const struct osk_state* o, int row, int col) {
	if(row == OSK_ROWS)
		return col >= 0 && col < OSK_FN_KEYS;
	return row >= 0 && row < OSK_ROWS && col >= 0 && col < OSK_COLS
		&& osk_pages[o->page][row][col] != 0;
}

/* Function-row x extent (logical units, relative to the widget origin). */
static void osk_fn_extent(int idx, int* x0, int* w) {
	int x = 0;
	for(int k = 0; k < idx; k++)
		x += osk_fn_keys[k].width;
	*x0 = x;
	*w = osk_fn_keys[idx].width;
}

/* Step left/right along the current row, skipping blanks, wrapping. A row
 * with no selectable cell cannot be current (page 2 row 3 is all blanks --
 * vertical movement skips it below). */
static void osk_step_col(struct osk_state* o, int dir) {
	int cols = (o->row == OSK_ROWS) ? OSK_FN_KEYS : OSK_COLS;
	int c = o->col;
	for(int k = 0; k < cols; k++) {
		c = (c + dir + cols) % cols;
		if(osk_cell_selectable(o, o->row, c)) {
			o->col = c;
			return;
		}
	}
}

/* Step up/down, mapping the column proportionally between the 10-wide grid
 * and the 6-wide function row, then snapping to the nearest selectable
 * cell in the new row (skipping fully-blank rows). */
static void osk_step_row(struct osk_state* o, int dir) {
	int rows_total = OSK_ROWS + 1;
	int r = o->row;
	for(int tries = 0; tries < rows_total; tries++) {
		r = (r + dir + rows_total) % rows_total;
		int cols = (r == OSK_ROWS) ? OSK_FN_KEYS : OSK_COLS;
		int old_cols = (o->row == OSK_ROWS) ? OSK_FN_KEYS : OSK_COLS;
		int c = o->col * cols / old_cols;
		if(c >= cols)
			c = cols - 1;
		/* nearest selectable in row r */
		for(int off = 0; off < cols; off++) {
			int cl = c - off;
			int cr = c + off;
			if(cl >= 0 && osk_cell_selectable(o, r, cl) == true) {
				o->row = r, o->col = cl;
				return;
			}
			if(cr < cols && osk_cell_selectable(o, r, cr)) {
				o->row = r, o->col = cr;
				return;
			}
		}
		/* row entirely blank: keep walking */
	}
}

static enum osk_event osk_activate(struct osk_state* o) {
	if(o->row < OSK_ROWS) {
		char c = osk_pages[o->page][o->row][o->col];
		return c ? osk_feed_char(o, c) : OSK_EV_NONE;
	}

	switch(o->col) {
		case OSK_FN_SHIFT:
			o->page = (o->page == 1) ? 0 : 1;
			return OSK_EV_NONE;
		case OSK_FN_SYM:
			o->page = (o->page == 2) ? 0 : 2;
			/* the symbols page has blank cells; make sure the cursor is
			 * still on a selectable one next time it enters the grid */
			return OSK_EV_NONE;
		case OSK_FN_SPACE: return osk_feed_char(o, ' ');
		case OSK_FN_BACK: return osk_feed_char(o, '\b');
		case OSK_FN_NEXT: return osk_feed_char(o, '\t');
		case OSK_FN_DONE: return osk_feed_char(o, '\r');
		default: return OSK_EV_NONE;
	}
}

enum osk_event osk_update_scaled(struct osk_state* o, int ox, int oy, int scale,
								 bool ptr, float px, float py,
								 bool allow_click) {
	assert(o && o->buf);
	int s = scale > 0 ? scale : 1;
	enum osk_event ev = OSK_EV_NONE;

	/* PC physical keyboard: the faithful Beta input path, same mutator. */
	char qc;
	while(input_text_poll(&qc)) {
		enum osk_event fe = osk_feed_char(o, qc);
		if(fe != OSK_EV_NONE)
			ev = fe; /* the LAST meaningful event of the frame wins */
	}

	/* Pointer hover overrides the d-pad cell (screen_sign precedence). */
	if(ptr) {
		float lx = (px - (float)ox) / (float)s;
		float ly = (py - (float)oy) / (float)s;
		if(lx >= 0 && lx < OSK_TOTAL_W && ly >= 0
		   && ly < (OSK_ROWS + 1) * OSK_CELL_H) {
			int r = (int)(ly / OSK_CELL_H);
			if(r < OSK_ROWS) {
				int c = (int)(lx / OSK_CELL_W);
				if(osk_cell_selectable(o, r, c))
					o->row = r, o->col = c;
			} else {
				int x = 0;
				for(int k = 0; k < OSK_FN_KEYS; k++) {
					if(lx >= x && lx < x + osk_fn_keys[k].width) {
						o->row = OSK_ROWS, o->col = k;
						break;
					}
					x += osk_fn_keys[k].width;
				}
			}
		}
	}

	/* While a REAL keyboard owns the screen (PC), the letter-aliased d-pad
	 * IB actions are suppressed -- typing 'w' must not move the cell
	 * cursor (input.h: a focused Beta text screen owns the whole
	 * keyboard). Mouse clicks (IB_GUI_CLICK/_ALT ride the 1000+ mouse
	 * codes on PC) stay live. */
	bool typing = input_text_active();
	if(!typing) {
		if(input_pressed(IB_GUI_LEFT))
			osk_step_col(o, -1);
		if(input_pressed(IB_GUI_RIGHT))
			osk_step_col(o, 1);
		if(input_pressed(IB_GUI_UP))
			osk_step_row(o, -1);
		if(input_pressed(IB_GUI_DOWN))
			osk_step_row(o, 1);
	}

	/* After a page switch the cursor may sit on a blank symbol cell. */
	if(!osk_cell_selectable(o, o->row, o->col))
		osk_step_col(o, 1);

	if(allow_click && input_pressed(IB_GUI_CLICK)) {
		enum osk_event ae = osk_activate(o);
		if(ae != OSK_EV_NONE)
			ev = ae;
	}

	/* Backspace accelerator from anywhere on the widget. */
	if(input_pressed(IB_GUI_CLICK_ALT)) {
		enum osk_event be = osk_feed_char(o, '\b');
		if(be != OSK_EV_NONE)
			ev = be;
	}

	return ev;
}

enum osk_event osk_update(struct osk_state* o, int ox, int oy, bool ptr,
						  float px, float py, bool allow_click) {
	return osk_update_scaled(o, ox, oy, GFX_GUI_SCALE, ptr, px, py,
							 allow_click);
}

/* One bordered cell in the GuiTextField palette [740c583 GuiTextField
 * .drawTextBox]: 0xA0A0A0 border, black fill; selected = white border,
 * hover-yellow label (the GuiButton hover color). */
static void osk_draw_cell(int x, int y, int w, int h, int s, const char* label,
						  bool selected, bool page_lit) {
	gfx_texture(false);
	if(selected)
		gutil_texquad_col(x - s, y - s, 0, 0, 0, 0, w + 2 * s, h + 2 * s, 0xFF,
						  0xFF, 0xFF, 255);
	else
		gutil_texquad_col(x - s, y - s, 0, 0, 0, 0, w + 2 * s, h + 2 * s, 0xA0,
						  0xA0, 0xA0, 255);
	gutil_texquad_col(x, y, 0, 0, 0, 0, w, h, 0, 0, 0, 255);
	gfx_texture(true);

	int tw = gutil_font_width(label, 8 * s);
	uint8_t cr = 0xE0, cg = 0xE0, cb = 0xE0;
	if(selected)
		cr = 0xFF, cg = 0xFF, cb = 0xE0;
	else if(page_lit)
		cr = 0xFF, cg = 0xFF, cb = 0x00;
	gutil_text_rgb(x + (w - tw) / 2, y + (h - 8 * s) / 2, label, 8 * s, false,
				   cr, cg, cb);
}

void osk_render_scaled(const struct osk_state* o, int ox, int oy, int scale) {
	assert(o);
	int s = scale > 0 ? scale : 1;

	for(int r = 0; r < OSK_ROWS; r++) {
		for(int c = 0; c < OSK_COLS; c++) {
			char ch = osk_pages[o->page][r][c];
			if(!ch)
				continue;
			char label[2] = {ch, 0};
			osk_draw_cell(ox + c * OSK_CELL_W * s, oy + r * OSK_CELL_H * s,
						  OSK_CELL_W * s - 2 * s, OSK_CELL_H * s - 2 * s, s,
						  label, o->row == r && o->col == c, false);
		}
	}

	for(int k = 0; k < OSK_FN_KEYS; k++) {
		int fx, fw;
		osk_fn_extent(k, &fx, &fw);
		bool lit = (k == OSK_FN_SHIFT && o->page == 1)
			|| (k == OSK_FN_SYM && o->page == 2);
		osk_draw_cell(ox + fx * s, oy + OSK_ROWS * OSK_CELL_H * s,
					  fw * s - 2 * s, OSK_CELL_H * s - 2 * s, s,
					  osk_fn_keys[k].label, o->row == OSK_ROWS && o->col == k,
					  lit);
	}
}

void osk_render(const struct osk_state* o, int ox, int oy) {
	osk_render_scaled(o, ox, oy, GFX_GUI_SCALE);
}

int osk_width(void) {
	return OSK_TOTAL_W;
}

int osk_height(void) {
	return (OSK_ROWS + 1) * OSK_CELL_H;
}

char osk_page_char(int page, int row, int col) {
	if(page < 0 || page >= OSK_PAGES || row < 0 || row >= OSK_ROWS || col < 0
	   || col >= OSK_COLS)
		return 0;
	return osk_pages[page][row][col];
}

static const char* const sign_chars[OSK_PAGES]={
 [OSK_PAGE_LOWER]="qwertyuiopasdfghjkl;zxcvbnm,./",
 [OSK_PAGE_UPPER]="QWERTYUIOPASDFGHJKL;ZXCVBNM,./",
 [OSK_PAGE_SYMBOLS]="\"#$%&'*<=>@[\\]^_`{|}~",
 [OSK_PAGE_NUM]="1234567890.,!?-+/:;()",
};
static const int sign_modes[]={OSK_PAGE_UPPER,OSK_PAGE_LOWER,OSK_PAGE_NUM,OSK_PAGE_SYMBOLS};
static const char* const sign_mode_labels[]={"ABC","abc","123","#+="};
static int sign_row_count(const struct osk_state* o,int row){
 if(row==0)return 4;
 if(row==4)return 2;
 int n=(int)strlen(sign_chars[o->page])-(row-1)*10;
 return n<=0?0:n<10?n:10;
}
void osk_sign_reset(struct osk_state* o,char* buf,size_t cap,int beta_max){
 assert(o&&buf&&cap);o->buf=buf;o->buf_cap=cap;o->beta_max=beta_max;
 o->page=OSK_PAGE_UPPER;o->row=1;o->col=0;
}
char osk_sign_page_char(int page,int row,int col){
 if(page<0||page>=OSK_PAGES||row<1||row>3||col<0||col>=10)return 0;
 size_t at=(size_t)(row-1)*10+col;
 return at<strlen(sign_chars[page])?sign_chars[page][at]:0;
}
bool osk_sign_key_bounds(int ox,int oy,int width,bool compact,int row,int col,struct gutil_button_bounds* out){
 if(!out||row<0||row>4)return false;
 int cols=row==0?4:row==4?2:10;if(col<0||col>=cols)return false;
 int gap=compact?2:4,rh=compact?25:28,usable=width-(cols-1)*gap;
 int x0=ox+col*gap+col*usable/cols,x1=ox+col*gap+(col+1)*usable/cols;
 *out=(struct gutil_button_bounds){x0,oy+row*(rh+gap),x1-x0,rh};return true;
}
static void sign_step_col(struct osk_state* o,int dir){int n=sign_row_count(o,o->row);if(n)o->col=(o->col+dir+n)%n;}
static void sign_step_row(struct osk_state* o,int dir){
 int old=sign_row_count(o,o->row),row=o->row;
 do row=(row+dir+5)%5;while(!sign_row_count(o,row));
 int n=sign_row_count(o,row);o->col=old?o->col*n/old:0;if(o->col>=n)o->col=n-1;o->row=row;
}
static enum osk_event sign_activate(struct osk_state* o){
 if(o->row==0){o->page=sign_modes[o->col];return OSK_EV_NONE;}
 if(o->row==4)return osk_feed_char(o,o->col?'\b':' ');
 char ch=osk_sign_page_char(o->page,o->row,o->col);return ch?osk_feed_char(o,ch):OSK_EV_NONE;
}
enum osk_event osk_sign_update_owned(struct osk_state* o,int ox,int oy,int width,bool compact,bool ptr,float px,float py,bool allow_click,bool* text_queued,const struct input_player_frame* owner,bool keyboard){
 assert(o&&o->buf);enum osk_event ev=OSK_EV_NONE;char ch;bool queued=false;
 while(keyboard&&input_text_poll(&ch)){queued=true;enum osk_event e=osk_feed_char(o,ch);if(e)ev=e;}
 if(text_queued)*text_queued=queued;
 bool pointer_key=false;
 if(ptr)for(int row=0;row<5;row++)for(int col=0;col<sign_row_count(o,row);col++){
  struct gutil_button_bounds q;osk_sign_key_bounds(ox,oy,width,compact,row,col,&q);
  if(gutil_button_hit(&q,px,py)){o->row=row;o->col=col;pointer_key=true;row=5;break;}
 }
 if(!(keyboard&&input_text_active())){
  if((owner ? owner->connected&&owner->pressed[IB_GUI_LEFT] : input_pressed(IB_GUI_LEFT)))sign_step_col(o,-1);
  if((owner ? owner->connected&&owner->pressed[IB_GUI_RIGHT] : input_pressed(IB_GUI_RIGHT)))sign_step_col(o,1);
  if((owner ? owner->connected&&owner->pressed[IB_GUI_UP] : input_pressed(IB_GUI_UP)))sign_step_row(o,-1);
  if((owner ? owner->connected&&owner->pressed[IB_GUI_DOWN] : input_pressed(IB_GUI_DOWN)))sign_step_row(o,1);
 }
 /* PC key characters are already delivered by the text queue. Mouse clicks
  * still activate visible keys, but the keyboard's bound Space must not. */
 /* B keeps its text-editor meaning even when focus is on another key. It has
  * priority over A so a combined controller report performs one deletion. */
 if(allow_click&&!queued&&(owner ? owner->connected&&owner->pressed[IB_GUI_CLICK_ALT] : input_pressed(IB_GUI_CLICK_ALT)))
  return osk_feed_char(o,'\b');
 if(allow_click&&!queued&&(owner ? owner->connected&&owner->pressed[IB_GUI_CLICK] : input_pressed(IB_GUI_CLICK))&&(!ptr||pointer_key)&&(!(keyboard&&input_text_active())||ptr)){
  enum osk_event e=sign_activate(o);if(e)ev=e;
 }
 return ev;
}
enum osk_event osk_sign_update(struct osk_state* o,int x,int y,int width,bool compact,bool ptr,float px,float py,bool click,bool* queued) {
 return osk_sign_update_owned(o,x,y,width,compact,ptr,px,py,click,queued,NULL,true);
}
static void sign_button(const struct gutil_button_bounds* q,const char* label,bool hot){
 int half=q->width/2;gfx_bind_texture(&texture_gui_widgets);
 gutil_texquad(q->x,q->y,0,hot?86:66,100,20,half,q->height);
 gutil_texquad(q->x+half,q->y,100,hot?86:66,100,20,q->width-half,q->height);
 gutil_text_rgb(q->x+(q->width-gutil_font_width(label,14))/2,q->y+(q->height-14)/2,label,14,true,hot?255:224,hot?255:224,224);
}
void osk_sign_render(const struct osk_state* o,int ox,int oy,int width,bool compact){
 for(int row=0;row<5;row++)for(int col=0;col<sign_row_count(o,row);col++){
  struct gutil_button_bounds q;char one[2]={0};const char* label;
  osk_sign_key_bounds(ox,oy,width,compact,row,col,&q);
  if(row==0)label=sign_mode_labels[col];else if(row==4)label=col?"Backspace":"Space";
  else {one[0]=osk_sign_page_char(o->page,row,col);label=one;}
  sign_button(&q,label,(row==0&&sign_modes[col]==o->page)||(o->row==row&&o->col==col));
 }
}

/* ======================================================================== */
/* Chat profile -- docs/ui_design/chat-keyboard/SPEC.md, "Keyboard grid and
 * page states". CHAT-SCOPED: everything above this line, including every
 * osk_sign_* entry point, its tables and its geometry, is untouched. The chat
 * profile reuses sign_chars through osk_sign_page_char and mutates only
 * through osk_feed_char -- "a chat presentation/navigation profile sharing
 * existing character lookup/mutation, not a second text editor". */

/* SPEC rect table. The tab row: "Letters (56,204,172,32), Numbers
 * (234,204,172,32), Symbols (412,204,172,32); gaps 6 pixels." */
#define CHAT_KB_LEFT 56
#define CHAT_TAB_Y 204
#define CHAT_TAB_W 172
#define CHAT_TAB_H 32
#define CHAT_TAB_PITCH 178 /* 172 + the 6-px gap */
/* "Three character rows, ten columns. Row tops: 244,288,332. Height 40,
 * vertical gap 4. Horizontal gap 4. Available cell width sum=492." */
#define CHAT_ROW_Y 244
#define CHAT_ROW_PITCH 44 /* 40 + the 4-px vertical gap */
#define CHAT_KEY_H 40
#define CHAT_COL_GAP 4
#define CHAT_CELL_SPAN 492
/* "Utility row: case (56,380,84,40), Space (144,380,220,40), Backspace
 * (368,380,216,40)." */
#define CHAT_UTIL_Y 380
static const int chat_util_x[OSK_CHAT_UTILS] = {56, 144, 368};
static const int chat_util_w[OSK_CHAT_UTILS] = {84, 220, 216};
static const char* const chat_tab_labels[OSK_CHAT_TABS]
	= {"Letters", "Numbers", "Symbols"};
/* Tab order is Letters, Numbers, Symbols; Letters resolves through the
 * remembered case. */
static const int chat_tab_pages[OSK_CHAT_TABS]
	= {OSK_PAGE_LOWER, OSK_PAGE_NUM, OSK_PAGE_SYMBOLS};

int osk_chat_tab_of_page(int page) {
	if(page == OSK_PAGE_NUM)
		return OSK_CHAT_TAB_NUMBERS;
	if(page == OSK_PAGE_SYMBOLS)
		return OSK_CHAT_TAB_SYMBOLS;
	return OSK_CHAT_TAB_LETTERS; /* LOWER and UPPER are one tab */
}

int osk_chat_page_of_tab(int tab, bool upper) {
	if(tab < 0 || tab >= OSK_CHAT_TABS)
		return OSK_PAGE_LOWER;
	if(tab == OSK_CHAT_TAB_LETTERS)
		return upper ? OSK_PAGE_UPPER : OSK_PAGE_LOWER;
	return chat_tab_pages[tab];
}

bool osk_chat_case_enabled(int page) {
	return osk_chat_tab_of_page(page) == OSK_CHAT_TAB_LETTERS;
}

const char* osk_chat_tab_label(int tab) {
	return (tab >= 0 && tab < OSK_CHAT_TABS) ? chat_tab_labels[tab] : "";
}

/* "Case label ABC while lower, abc while upper" -- the label is the case the
 * key would switch TO. */
const char* osk_chat_case_label(bool upper) {
	return upper ? "abc" : "ABC";
}

int osk_chat_row_count(int page, int row) {
	if(page < 0 || page >= OSK_PAGES)
		return 0;
	if(row == OSK_CHAT_ROW_TABS)
		return OSK_CHAT_TABS;
	if(row == OSK_CHAT_ROW_UTILITY)
		return OSK_CHAT_UTILS;
	if(row < OSK_CHAT_GRID_ROW_FIRST || row > OSK_CHAT_GRID_ROW_LAST)
		return 0;
	int n = (int)strlen(sign_chars[page]) - (row - 1) * 10;
	return n <= 0 ? 0 : n < 10 ? n : 10;
}

bool osk_chat_selectable(int page, int row, int col) {
	int n = osk_chat_row_count(page, row);
	if(col < 0 || col >= n)
		return false;
	/* "Empty cells have no button and cannot receive focus" and the case key
	 * is skipped while disabled. */
	if(row == OSK_CHAT_ROW_UTILITY && col == OSK_CHAT_UTIL_CASE)
		return osk_chat_case_enabled(page);
	return true;
}

bool osk_chat_key_bounds(int ox, int row, int col,
						 struct gutil_button_bounds* out) {
	if(!out || row < 0 || row >= OSK_CHAT_ROWS || col < 0)
		return false;
	if(row == OSK_CHAT_ROW_TABS) {
		if(col >= OSK_CHAT_TABS)
			return false;
		*out = (struct gutil_button_bounds) {
			ox + CHAT_KB_LEFT + col * CHAT_TAB_PITCH, CHAT_TAB_Y, CHAT_TAB_W,
			CHAT_TAB_H};
		return true;
	}
	if(row == OSK_CHAT_ROW_UTILITY) {
		if(col >= OSK_CHAT_UTILS)
			return false;
		*out = (struct gutil_button_bounds) {ox + chat_util_x[col], CHAT_UTIL_Y,
											 chat_util_w[col], CHAT_KEY_H};
		return true;
	}
	if(col >= OSK_COLS)
		return false;
	/* "use left=56+4*c+floor(492*c/10), right=56+4*c+floor(492*(c+1)/10)" --
	 * widths 49,49,49,49,50,49,49,49,49,50. */
	int left = CHAT_KB_LEFT + CHAT_COL_GAP * col + CHAT_CELL_SPAN * col / 10;
	int right
		= CHAT_KB_LEFT + CHAT_COL_GAP * col + CHAT_CELL_SPAN * (col + 1) / 10;
	*out = (struct gutil_button_bounds) {
		ox + left, CHAT_ROW_Y + (row - OSK_CHAT_GRID_ROW_FIRST) * CHAT_ROW_PITCH,
		right - left, CHAT_KEY_H};
	return true;
}

/* Horizontal wrap, skipping cells that cannot take focus (the existing
 * behaviour, extended by the disabled case key). */
static void chat_step_col(struct osk_state* o, int dir) {
	int n = osk_chat_row_count(o->page, o->row);
	if(n <= 0)
		return;
	int c = o->col;
	for(int k = 0; k < n; k++) {
		c = (c + dir + n) % n;
		if(osk_chat_selectable(o->page, o->row, c)) {
			o->col = c;
			return;
		}
	}
}

/* Proportional vertical-neighbour selection (sign_step_row's rule), snapping
 * to the nearest selectable column and skipping fully blank rows. */
static void chat_step_row(struct osk_state* o, int dir) {
	int old = osk_chat_row_count(o->page, o->row);
	int r = o->row;
	for(int tries = 0; tries < OSK_CHAT_ROWS; tries++) {
		r = (r + dir + OSK_CHAT_ROWS) % OSK_CHAT_ROWS;
		int n = osk_chat_row_count(o->page, r);
		if(n <= 0)
			continue;
		int c = old > 0 ? o->col * n / old : 0;
		if(c >= n)
			c = n - 1;
		for(int off = 0; off < n; off++) {
			if(c - off >= 0 && osk_chat_selectable(o->page, r, c - off)) {
				o->row = r, o->col = c - off;
				return;
			}
			if(c + off < n && osk_chat_selectable(o->page, r, c + off)) {
				o->row = r, o->col = c + off;
				return;
			}
		}
	}
}

/* "Page shortcut changes preserve focus row/column where valid, otherwise
 * clamp to the nearest existing column, skip empty rows toward utility." */
static void chat_clamp_focus(struct osk_state* o) {
	if(o->row < 0 || o->row >= OSK_CHAT_ROWS)
		o->row = OSK_CHAT_GRID_ROW_FIRST;
	if(osk_chat_selectable(o->page, o->row, o->col))
		return;
	for(int row = o->row; row < OSK_CHAT_ROWS; row++) {
		int n = osk_chat_row_count(o->page, row);
		if(n <= 0)
			continue;
		int c = o->col < n ? o->col : n - 1;
		for(int off = 0; off < n; off++) {
			if(c - off >= 0 && osk_chat_selectable(o->page, row, c - off)) {
				o->row = row, o->col = c - off;
				return;
			}
			if(c + off < n && osk_chat_selectable(o->page, row, c + off)) {
				o->row = row, o->col = c + off;
				return;
			}
		}
	}
	/* Unreachable with the shipped pages (the utility row always has Space). */
	o->row = OSK_CHAT_ROW_UTILITY, o->col = OSK_CHAT_UTIL_SPACE;
}

static void chat_set_tab(struct osk_state* o, struct osk_chat_view* v, int tab) {
	o->page = osk_chat_page_of_tab(tab, v->upper);
	chat_clamp_focus(o);
}

static enum osk_event chat_activate(struct osk_state* o,
									struct osk_chat_view* v, bool* rejected) {
	if(o->row == OSK_CHAT_ROW_TABS) {
		chat_set_tab(o, v, o->col);
		return OSK_EV_NONE;
	}
	char ch = 0;
	if(o->row == OSK_CHAT_ROW_UTILITY) {
		switch(o->col) {
			case OSK_CHAT_UTIL_CASE:
				if(!osk_chat_case_enabled(o->page))
					return OSK_EV_NONE;
				v->upper = !v->upper;
				o->page = osk_chat_page_of_tab(OSK_CHAT_TAB_LETTERS, v->upper);
				chat_clamp_focus(o);
				return OSK_EV_NONE;
			case OSK_CHAT_UTIL_SPACE: ch = ' '; break;
			case OSK_CHAT_UTIL_BACK: return osk_feed_char(o, '\b');
			default: return OSK_EV_NONE;
		}
	} else {
		ch = osk_sign_page_char(o->page, o->row, o->col);
	}
	if(!ch)
		return OSK_EV_NONE;
	enum osk_event e = osk_feed_char(o, ch);
	/* "At cap, text-producing keys (including Space) reject insertion with
	 * unchanged buffer and flash the counter yellow for 120 ms." */
	if(e == OSK_EV_NONE && rejected)
		*rejected = true;
	return e;
}

/* The chat profile is OWNER-FRAME ONLY -- it never reads the merged global
 * mask. That is the B1b owner-isolation requirement ("in a split-screen session
 * only the owning player's frame drives the composer"), and it keeps this
 * widget's link surface free of input_held, which the frozen sign test
 * (tests/test_sign_approved_flow.c, which links widget_osk.c standalone) does
 * not stub. screen_chat.c synthesises player 0's frame from the merged mask on
 * a PC with no paired controller, so there is exactly one input path. */
static bool chat_edge_raw(const struct input_player_frame* owner,
						  enum input_button b) {
	return owner && owner->connected && owner->pressed[b];
}

static bool chat_down(const struct input_player_frame* owner,
					  enum input_button b) {
	return owner && owner->connected && owner->down[b];
}

/* ---- B1b input hygiene: the neutral re-arm ------------------------------
 * SPEC: "Disconnect freezes edits; preserve draft/focus and require neutral
 * rearming before fresh actions." The same rule covers the press that OPENED
 * the composer and the press that closed it: a button that was already down
 * when the composer took over does nothing until it has been released, so a
 * held Send cannot re-fire and the opening press cannot type. CHAT-SCOPED:
 * osk_sign_* keeps its plain edge behaviour. */
static const enum input_button chat_arm_button[OSK_CHAT_ARM_COUNT] = {
	[OSK_CHAT_ARM_LEFT] = IB_GUI_LEFT,
	[OSK_CHAT_ARM_RIGHT] = IB_GUI_RIGHT,
	[OSK_CHAT_ARM_UP] = IB_GUI_UP,
	[OSK_CHAT_ARM_DOWN] = IB_GUI_DOWN,
	[OSK_CHAT_ARM_CLICK] = IB_GUI_CLICK,
	[OSK_CHAT_ARM_BACK] = IB_GUI_CLICK_ALT,
	[OSK_CHAT_ARM_PAGE] = IB_GUI_PAGE,
	[OSK_CHAT_ARM_PREVIOUS] = IB_GUI_PREVIOUS,
	[OSK_CHAT_ARM_SEND] = IB_GUI_QUICK_MOVE,
	[OSK_CHAT_ARM_CANCEL] = IB_INVENTORY,
	[OSK_CHAT_ARM_HOME] = IB_HOME,
	[OSK_CHAT_ARM_CHAT] = IB_CHAT,
};

void osk_chat_input_reset(struct osk_chat_view* view) {
	assert(view);
	view->repeat = (struct gutil_menu_repeat) {0};
	/* Block everything: the reset runs on the frame the opening press is still
	 * down, and the caller has no cheap way to know which button that was. */
	view->rearm_mask = (1u << OSK_CHAT_ARM_COUNT) - 1u;
	view->owner_was_disconnected = false;
}

/* Clear the block on every chat button that is currently neutral, and re-block
 * everything across a disconnect so a reconnecting pad cannot act on a button
 * it was already holding. Idempotent within a frame: the screen calls it once
 * before reading Send/Cancel, and osk_chat_update_owned calls it again. */
void osk_chat_input_tick(struct osk_chat_view* v,
						 const struct input_player_frame* owner) {
	assert(v);
	bool connected = owner ? owner->connected : true;
	if(connected && v->owner_was_disconnected)
		v->rearm_mask = (1u << OSK_CHAT_ARM_COUNT) - 1u;
	v->owner_was_disconnected = !connected;
	if(!connected) {
		v->repeat = (struct gutil_menu_repeat) {0};
		return;
	}
	for(int k = 0; k < OSK_CHAT_ARM_COUNT; k++)
		if(!chat_down(owner, chat_arm_button[k]))
			v->rearm_mask &= ~(1u << k);
}

/* A press edge that has passed the re-arm. */
static bool chat_edge(struct osk_chat_view* v,
					  const struct input_player_frame* owner, int arm) {
	if(v->rearm_mask & (1u << arm))
		return false;
	/* The character queue owns keyboard aliases across frame boundaries,
	 * not just on the frame that happened to contain queued text. */
	if(input_text_active()
	   && (arm == OSK_CHAT_ARM_CLICK || arm == OSK_CHAT_ARM_BACK))
		return owner && owner->connected
			&& input_text_mouse_pressed(chat_arm_button[arm]);
	return chat_edge_raw(owner, chat_arm_button[arm]);
}

static bool chat_held(struct osk_chat_view* v,
					  const struct input_player_frame* owner, int arm) {
	if(v->rearm_mask & (1u << arm))
		return false;
	return chat_down(owner, chat_arm_button[arm]);
}

bool osk_chat_send_edge(struct osk_chat_view* view,
						const struct input_player_frame* owner) {
	assert(view);
	return chat_edge(view, owner, OSK_CHAT_ARM_SEND);
}

bool osk_chat_cancel_edge(struct osk_chat_view* view,
						  const struct input_player_frame* owner) {
	assert(view);
	return chat_edge(view, owner, OSK_CHAT_ARM_CANCEL)
		|| chat_edge(view, owner, OSK_CHAT_ARM_HOME);
}

bool osk_chat_click_edge(struct osk_chat_view* view,
						 const struct input_player_frame* owner) {
	assert(view);
	return chat_edge(view, owner, OSK_CHAT_ARM_CLICK);
}

bool osk_chat_back_edge(struct osk_chat_view* view,
						const struct input_player_frame* owner) {
	assert(view);
	return chat_edge(view, owner, OSK_CHAT_ARM_BACK);
}

/* ---- B1b held-direction repeat (CHAT ONLY) ------------------------------
 * gutil_menu_repeat_step's shape with SPEC's cadence. A fresh press always
 * moves immediately and restarts the delay; a direction change does the same;
 * a neutral frame clears everything; and a late frame reloads the interval
 * instead of draining it, so a stalled frame cannot emit a burst. */
/* `seen` reports the countdown THIS decision consulted -- after the decrement
 * and before any reload -- so a caller can publish production's own number
 * instead of re-deriving one (struct osk_chat_result::repeat_seen_s). A release
 * consulted nothing (0); a fresh press or a direction change consulted nothing
 * either, and reports what was left before the delay was loaded. */
static int chat_repeat_step(struct gutil_menu_repeat* r, int dir, float dt,
							bool fresh, float* seen) {
	if(!dir) {
		*r = (struct gutil_menu_repeat) {0};
		if(seen)
			*seen = 0.0F;
		return 0;
	}
	if(fresh || dir != r->direction) {
		if(seen)
			*seen = r->remaining;
		r->direction = dir;
		r->remaining = OSK_CHAT_REPEAT_DELAY_S;
		return dir;
	}
	if(isfinite(dt) && dt > 0.0F)
		r->remaining -= dt;
	if(seen)
		*seen = r->remaining;
	if(r->remaining > 0.0F)
		return 0;
	r->remaining = OSK_CHAT_REPEAT_RATE_S;
	return dir;
}

void osk_chat_update_owned(struct osk_state* o, struct osk_chat_view* view,
						   float dt, bool ptr, float px, float py,
						   bool allow_click,
						   const struct input_player_frame* owner,
						   bool keyboard, struct osk_chat_result* out) {
	assert(o && o->buf && view && out);
	*out = (struct osk_chat_result) {0};
	out->event = OSK_EV_NONE;
	out->press_row = out->press_col = -1;
	/* No step decision runs on a text-owned frame; report the live countdown
	 * so the field is never stale from a previous frame. */
	out->repeat_seen_s = view->repeat.remaining;
	int was_row = o->row, was_col = o->col, was_page = o->page;
	osk_chat_input_tick(view, owner);
	char ch;
	/* The opener's own key must not type: while IB_CHAT has not been seen up
	 * since the composer opened, drain the queue and DISCARD it (GLFW key
	 * repeat for a held 'T' would otherwise land in the draft on PC). */
	bool swallow = (view->rearm_mask & (1u << OSK_CHAT_ARM_CHAT)) != 0;
	while(keyboard && input_text_poll(&ch)) {
		if(swallow)
			continue;
		out->text_queued = true;
		size_t before = strlen(o->buf);
		enum osk_event e = osk_feed_char(o, ch);
		if(e)
			out->event = e;
		else if(ch >= 32 && ch <= 126 && before >= (size_t)o->beta_max
				&& o->beta_max > 0)
			out->rejected = true;
		/* Events are applied IN ORDER and the first confirm/cancel ends the
		 * frame: a queue of ['
','a'] must send, not type an 'a' after the
		 * send, and [27,'x'] must cancel with the draft untouched. */
		if(e == OSK_EV_DONE || e == OSK_EV_CANCEL)
			break;
	}
	/* Pointer hover overrides the cursor (the existing precedence rule). */
	bool pointer_key = false;
	if(ptr) {
		for(int row = 0; row < OSK_CHAT_ROWS && !pointer_key; row++) {
			for(int col = 0; col < osk_chat_row_count(o->page, row); col++) {
				struct gutil_button_bounds q;
				if(!osk_chat_selectable(o->page, row, col)
				   || !osk_chat_key_bounds(view->ox, row, col, &q))
					continue;
				if(gutil_button_hit(&q, px, py)) {
					o->row = row, o->col = col;
					pointer_key = true;
					break;
				}
			}
		}
	}
	bool from_repeat = false;
	if(!(keyboard && input_text_active())) {
		/* One active direction at a time, chosen deterministically: a fresh
		 * press always wins (so pressing Up while Left is held goes up), then
		 * the direction already repeating if it is still held, then the
		 * highest-priority held direction. A diagonal or a conflicting
		 * left+right report therefore has one defined outcome. */
		static const int arms[4] = {OSK_CHAT_ARM_LEFT, OSK_CHAT_ARM_RIGHT,
									OSK_CHAT_ARM_UP, OSK_CHAT_ARM_DOWN};
		int dir = 0;
		bool fresh = false;
		for(int k = 0; k < 4; k++)
			if(chat_edge(view, owner, arms[k])) {
				dir = k + 1;
				fresh = true;
				break;
			}
		if(!dir) {
			for(int k = 0; k < 4; k++)
				if(view->repeat.direction == k + 1
				   && chat_held(view, owner, arms[k])) {
					dir = k + 1;
					break;
				}
		}
		if(!dir) {
			for(int k = 0; k < 4; k++)
				if(chat_held(view, owner, arms[k])) {
					dir = k + 1;
					break;
				}
		}
		int step
			= chat_repeat_step(&view->repeat, dir, dt, fresh, &out->repeat_seen_s);
		if(step) {
			from_repeat = !fresh;
			switch(step) {
				case 1: chat_step_col(o, -1); break;
				case 2: chat_step_col(o, 1); break;
				case 3: chat_step_row(o, -1); break;
				default: chat_step_row(o, 1); break;
			}
		}
		/* The EXISTING page actions, consumed while chat owns input (SPEC:
		 * "consume existing IB_GUI_PREVIOUS and IB_GUI_PAGE while chat owns
		 * input, without modifying input mappings"). Press-edge only, and they
		 * "stop at ends, do not repeat". */
		int tab = osk_chat_tab_of_page(o->page);
		if(chat_edge(view, owner, OSK_CHAT_ARM_PREVIOUS) && tab > 0)
			chat_set_tab(o, view, tab - 1);
		else if(chat_edge(view, owner, OSK_CHAT_ARM_PAGE)
				&& tab < OSK_CHAT_TABS - 1)
			chat_set_tab(o, view, tab + 1);
	}
	chat_clamp_focus(o);
	/* B keeps its text-editor meaning from any key and outranks A. */
	if(allow_click && !out->text_queued
	   && chat_edge(view, owner, OSK_CHAT_ARM_BACK)) {
		enum osk_event e = osk_feed_char(o, '\b');
		if(e)
			out->event = e;
		out->activated = true;
		out->press_row = OSK_CHAT_ROW_UTILITY;
		out->press_col = OSK_CHAT_UTIL_BACK;
	} else if(allow_click && !out->text_queued
			  && chat_edge(view, owner, OSK_CHAT_ARM_CLICK)
			  && (!ptr || pointer_key)
			  && (!(keyboard && input_text_active()) || ptr)) {
		out->press_row = o->row;
		out->press_col = o->col;
		out->activated = true;
		enum osk_event e = chat_activate(o, view, &out->rejected);
		if(e)
			out->event = e;
	}
	/* SPEC "Motion": FOCUS fires on the first actual move of a fresh press or
	 * a new pointer target, never on a repeated held step, and never when the
	 * cursor could not move (a one-cell row, or the disabled case key). */
	out->moved = o->row != was_row || o->col != was_col;
	out->moved_repeat = out->moved && from_repeat;
	out->page_changed = o->page != was_page;
	if(out->page_changed) {
		/* SPEC: "release/disconnect/page change resets the timer" -- the
		 * TIMER, not the active direction. Zeroing the direction made a
		 * still-held direction look brand new on the very next frame, so
		 * tapping Page while holding a direction walked the cursor one extra
		 * cell with no new press. Reloading the delay keeps the hold owned by
		 * the same direction and simply restarts its wait. */
		view->repeat.remaining = OSK_CHAT_REPEAT_DELAY_S;
	}
}
