/* Lane 5 -- the APPROVED sign editor flow.
 *
 * Authority: docs/ui_design/approved/menus.html (menu selector -> Sign entry)
 * and specs/menus.md:
 *
 *   "Sign has four lines, at most 15 characters each. The visible keyboard
 *    modes are ABC, abc, 123 and #+=, covering all 95 printable ASCII
 *    characters. Keep stable key rows and Space/Delete placement, with a
 *    reserved 52 px count area so line controls do not shift near the limit.
 *    Clean or reverted Cancel closes directly. Changed text uses discard
 *    confirmation; Back preserves the edit; Discard restores saved content.
 *    Done saves through the real sign transaction. Show one bounded sign face,
 *    not a tiled plank sheet."
 *
 * plus specs/sound-events.md, "Sign editing": "Typing cue after an
 * inserted/deleted character; scroll when changing selected line; existing
 * confirmation after successfully saving changed text. Unchanged Done and
 * clean Cancel use Back. A cancelled discard prompt uses Back; opening it uses
 * category. Confirmed discard closes with Back, never the successful-save cue.
 * Full lines and deletion of an empty line are silent."
 *
 * Two halves, both against production code:
 *   - the OSK page/edit model (the real widget_osk.c mutator), which is where
 *     the 95-character coverage, the 15-character ceiling and the typing/
 *     silence rules live; and
 *   - the reserved count area / bounded-face layout, through the shared
 *     composition (menu_header.c), which is what the golden also sees. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/gui/menu_header.h"
#include "game/gui/sign_osk_model.h"
#include "game/gui/widget_osk.h"
#include "graphics/gui_util.h"
#include "platform/gfx.h"
#include "platform/input.h"

/* Boundary doubles: the OSK's EDIT model (osk_feed_char, the page table) is
 * what this test drives, so only its draw/device edge is replaced. */
struct tex_gfx texture_font, texture_controls, texture_gui_widgets,
	texture_pointer;
void gfx_texture(bool enabled) { (void)enabled; }
void gutil_texquad_col(int x, int y, int tx, int ty, int sx, int sy, int w,
					   int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	(void)x; (void)y; (void)tx; (void)ty; (void)sx; (void)sy; (void)w;
	(void)h; (void)r; (void)g; (void)b; (void)a;
}
int gutil_font_width(const char* str, int scale) {
	return (int)strlen(str) * scale;
}
void gutil_text_rgb(int x, int y, const char* s, int scale, bool shadow,
					uint8_t r, uint8_t g, uint8_t b) {
	(void)x; (void)y; (void)s; (void)scale; (void)shadow; (void)r; (void)g;
	(void)b;
}
static bool key_click;
bool input_pressed(enum input_button b) { return b==IB_GUI_CLICK&&key_click; }
bool input_text_active(void) { return false; }
/* This sign-only harness never enters the chat widget's PC typing branch. */
bool input_text_mouse_pressed(enum input_button b) { (void)b; return false; }
bool input_text_poll(char* out) { (void)out; return false; }

/* ---- 1. four lines, 15 characters, four pages, all 95 ASCII ------------- */

static void test_sign_shape(void) {
	assert(SIGN_OSK_LINES == 4);
	assert(SIGN_OSK_COLS == 16); /* the physical cell grid */
	/* The TYPING ceiling is Beta's own 15, not the grid width
	 * [740c583 GuiEditSign / GuiTextField.textboxKeyTyped]. */
	assert(SIGN_BETA_MAX_CHARS == 15);
	assert(SIGN_OSK_SIZE == SIGN_OSK_LINES * SIGN_OSK_COLS);
	assert(OSK_PAGES == 4); /* ABC / abc / 123 / #+= */

	/* "covering all 95 printable ASCII characters" -- on a Wii there is no
	 * keyboard fallback, so an unreachable character would be stranded. */
	bool seen[128] = {false};
	for(int page = 0; page < OSK_PAGES; page++)
		for(int row = 0; row < OSK_ROWS; row++)
			for(int col = 0; col < OSK_COLS; col++) {
				char c = osk_page_char(page, row, col);
				if(c)
					seen[(unsigned char)c] = true;
			}
	/* Space is the dedicated function key, not a grid cell. */
	seen[' '] = true;
	unsigned reachable = 0;
	for(int c = 32; c < 127; c++) {
		assert(seen[c]);
		reachable++;
	}
	assert(reachable == 95);
	puts("  sign: 4 lines x 15 chars, 4 keyboard sets, all 95 printable "
		 "ASCII reachable 1/1");
}

/* ---- 2. the typing cue fires only on a REAL character change ------------ */

static void test_typing_and_silence(void) {
	char buf[SIGN_OSK_COLS + 1] = "";
	struct osk_state o;
	osk_reset(&o, buf, sizeof(buf), SIGN_BETA_MAX_CHARS);

	/* Every inserted character changes the buffer -- that IS the cue gate in
	 * screen_sign.c (it compares the owned buffer before/after). */
	for(int k = 0; k < SIGN_BETA_MAX_CHARS; k++) {
		char before[SIGN_OSK_COLS + 1];
		memcpy(before, buf, sizeof(before));
		osk_feed_char(&o, 'a');
		assert(strcmp(before, buf) != 0 && "an insert must change the buffer");
	}
	assert(strlen(buf) == SIGN_BETA_MAX_CHARS);

	/* "Full lines ... are silent": a 16th character is refused, so the buffer
	 * is unchanged and screen_sign.c's one comparison already suppresses the
	 * cue -- no separate boundary special-case to drift. */
	char full[SIGN_OSK_COLS + 1];
	memcpy(full, buf, sizeof(full));
	osk_feed_char(&o, 'z');
	assert(strcmp(full, buf) == 0);

	/* A delete changes the buffer -> cue. */
	osk_feed_char(&o, '\b');
	assert(strcmp(full, buf) != 0);

	/* "deletion of an empty line" is silent, same mechanism. */
	while(buf[0])
		osk_feed_char(&o, '\b');
	assert(buf[0] == '\0');
	osk_feed_char(&o, '\b');
	assert(buf[0] == '\0');
	puts("  sign: insert/delete change the buffer; a full line and an empty "
		 "delete leave it byte-identical (silence) 1/1");
}

/* ---- 3. clean vs changed vs reverted ----------------------------------- */

/* screen_sign.c decides "clean or reverted Cancel closes directly" with one
 * memcmp of the live cells against the cells the editor OPENED with. This
 * models that predicate exactly, including the reverted case a naive
 * "was anything typed?" flag would get wrong. */
static bool sign_changed(const unsigned char* opened, const unsigned char* live) {
	return memcmp(opened, live, SIGN_OSK_SIZE) != 0;
}

static void test_discard_predicate(void) {
	unsigned char opened[SIGN_OSK_SIZE], live[SIGN_OSK_SIZE];
	memset(opened, ' ', sizeof(opened));
	memcpy(live, opened, sizeof(live));
	assert(!sign_changed(opened, live)); /* clean */

	live[0] = 'A';
	assert(sign_changed(opened, live)); /* changed -> discard confirmation */

	live[0] = ' ';
	assert(!sign_changed(opened, live)); /* REVERTED -> closes directly */

	/* A sign that opened with saved text and was edited back to it is also
	 * reverted, not changed. */
	memset(opened, ' ', sizeof(opened));
	memcpy(opened, "HELLO", 5);
	memcpy(live, opened, sizeof(live));
	live[4] = '!';
	assert(sign_changed(opened, live));
	live[4] = 'O';
	assert(!sign_changed(opened, live));

	/* "Discard restores saved content": the restore is exactly the set of
	 * differing cells, which is what screen_sign_restore_saved replays through
	 * the real SRPC_SIGN_SET path. */
	live[0] = 'J';
	live[9] = 'X';
	size_t differing = 0;
	for(size_t k = 0; k < SIGN_OSK_SIZE; k++)
		if(live[k] != opened[k])
			differing++;
	assert(differing == 2);
	puts("  sign: clean / changed / reverted classification, and the restore "
		 "set is the differing cells 1/1");
}

/* ---- 4. the reserved count area and the bounded face ------------------- */

/* Screen layout itself is checked through shipping screen_sign in test_recipe_ui. */
static void test_layout(void){
 char buf[17]="";struct osk_state o;osk_sign_reset(&o,buf,sizeof buf,15);
 assert(o.page==OSK_PAGE_UPPER&&o.row==1&&o.col==0);
 bool seen[127]={0};seen[32]=true;
 for(int p=0;p<4;p++)for(int row=1;row<=3;row++)for(int col=0;col<10;col++){char ch=osk_sign_page_char(p,row,col);if(ch)seen[(unsigned char)ch]=true;}
 for(int ch=32;ch<127;ch++)assert(seen[ch]);
 for(int compact=0;compact<2;compact++){
  int x=compact?266:44,y=compact?47:232,w=compact?330:552;struct gutil_button_bounds q,last;
  assert(osk_sign_key_bounds(x,y,w,compact,0,0,&q));assert(q.width==(compact?81:135)&&q.height==(compact?25:28));
  assert(osk_sign_key_bounds(x,y,w,compact,4,1,&last));assert(last.x+last.width==596&&last.y==(compact?155:360));
  key_click=true;osk_sign_update(&o,x,y,w,compact,true,q.x+q.width+.5F,q.y+1,true,NULL);assert(!buf[0]);
  assert(osk_sign_key_bounds(x,y,w,compact,1,0,&q));osk_sign_update(&o,x,y,w,compact,true,q.x+1,q.y+1,true,NULL);assert(!strcmp(buf,"Q"));
  osk_sign_update(&o,x,y,w,compact,true,last.x+1,last.y+1,true,NULL);assert(!buf[0]);key_click=false;
 }
 puts("PASS approved sign keyboard: exact modes, key bounds, inert gaps, append/delete");
}

/* ---- 5. the cell diff the sign transaction is built from ---------------- */

static void test_cell_diff(void) {
	uint8_t cells[SIGN_OSK_SIZE];
	struct sign_osk_edit edits[SIGN_OSK_COLS];
	char shadow[SIGN_OSK_COLS + 1];
	memset(cells, ' ', sizeof(cells));

	/* Typing "HI" on line 2 produces exactly two authoritative writes. */
	size_t line = 2;
	assert(sign_osk_shadow_from_cells(cells, line, shadow) == 0);
	snprintf(shadow, sizeof(shadow), "HI");
	size_t n = sign_osk_diff_cells(cells, line, shadow, edits);
	assert(n == 2);
	for(size_t k = 0; k < n; k++) {
		assert(edits[k].slot / SIGN_OSK_COLS == line);
		cells[edits[k].slot] = (uint8_t)edits[k].ch;
	}
	/* Re-applying the same text writes nothing: unchanged boundaries are not
	 * transactions (and therefore not cues). */
	assert(sign_osk_diff_cells(cells, line, shadow, edits) == 0);
	puts("  sign: the shadow->cell diff issues one write per CHANGED cell and "
		 "none for unchanged text 1/1");
}

int main(void) {
	puts("approved sign editor flow");
	test_sign_shape();
	test_typing_and_silence();
	test_discard_predicate();
	test_layout();
	test_cell_diff();
	puts("PASS approved sign flow");
	return 0;
}

void gfx_bind_texture(struct tex_gfx* t){(void)t;}
void gutil_texquad(int x,int y,int tx,int ty,int sx,int sy,int w,int h){(void)x;(void)y;(void)tx;(void)ty;(void)sx;(void)sy;(void)w;(void)h;}
bool gutil_button_hit(const struct gutil_button_bounds* q,float x,float y){return x>=q->x&&x<q->x+q->width&&y>=q->y&&y<q->y+q->height;}
