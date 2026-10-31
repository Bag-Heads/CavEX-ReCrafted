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

#include "../../graphics/gfx_util.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../graphics/texture_atlas.h"
#include "../../network/server_interface.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/menu_sound.h"
#include "../../platform/time.h"
#include "../game_state.h"
#include "menu_header.h"
#include "screen.h"
#include "sign_osk_model.h"
#include "widget_osk.h"
#include "../../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)

#include <stdio.h>
#include <string.h>

/* APPROVED SIGN EDITOR -- docs/ui_design/approved/menus.html (menu selector ->
 * Sign entry) and specs/menus.md:
 *
 *   "Sign has four lines, at most 15 characters each. The visible keyboard
 *    modes are ABC, abc, 123 and #+=, covering all 95 printable ASCII
 *    characters. Keep stable key rows and Space/Delete placement, with a
 *    reserved 52 px count area so line controls do not shift near the limit.
 *    Clean or reverted Cancel closes directly. Changed text uses discard
 *    confirmation; Back preserves the edit; Discard restores saved content.
 *    Done saves through the real sign transaction. Show one bounded sign
 *    face, not a tiled plank sheet."
 *
 * The four lines, the Beta 15-character ceiling and the four keyboard pages
 * were already true (widget_osk.h; test_osk_env sweeps every printable ASCII
 * character for reachability). This lane adds the four that were missing:
 *   1. the DISCARD workflow, including the real restore transaction;
 *   2. the reserved character-count area (MENU_SIGN_COUNT_RESERVE_W);
 *   3. one BOUNDED sign face instead of a single plank tile stretched 8x4
 *      across the whole panel;
 *   4. the approved cue map (below), which the screen had none of.
 *
 * SOUND -- specs/sound-events.md, "Sign editing": "Typing cue after an
 * inserted/deleted character; scroll when changing selected line; existing
 * confirmation after successfully saving changed text. Unchanged Done and
 * clean Cancel use Back. A cancelled discard prompt uses Back; opening it uses
 * category. Confirmed discard closes with Back, never the successful-save cue.
 * Full lines and deletion of an empty line are silent." A full line and a
 * delete on an empty line both leave the shadow buffer byte-identical, so the
 * single "did the owned buffer actually change" test below is exactly the
 * approved silence rule -- no separate boundary special-case, which is what
 * would have drifted.
 *
 * SIMULATION UNTOUCHED: every character still goes through the same
 * windowc_new_sign_set_action + SRPC_SIGN_SET pair, in the same order, and
 * Done still closes through SRPC_WINDOW_CLOSE (which server_remote turns into
 * Beta's whole-sign Packet130UpdateSign). Discard is not a local undo: it
 * REPLAYS the opened content through that same authoritative path. */

#define SIGN_FACE_W 128
#define SIGN_FACE_H 64
#define SIGN_COLS SIGN_OSK_COLS
#define SIGN_LINES SIGN_OSK_LINES

/* All mutable state belongs to the supplied session. No active-player swap. */
struct sign_context {
 struct client_session* session;
 struct sign_editor* edit;
 const struct input_player_frame* frame;
 unsigned player;
 int width,height;
 bool primary_input;
 container_send_fn send;
 void* user;
};
static bool sign_pressed(const struct sign_context* c,enum input_button key) {
 return c->primary_input ? input_pressed(key) : c->frame&&c->frame->connected&&c->frame->pressed[key];
}
static bool sign_typing(const struct sign_context* c) { return c->primary_input&&input_text_active(); }
static bool sign_poll(const struct sign_context* c,char* ch) { return c->primary_input&&input_text_poll(ch); }
static bool sign_pointer(const struct sign_context* c,float* x,float* y,float* angle) {
 if(c->primary_input)return input_pointer(x,y,angle);
 return input_owner_pointer(c->frame,0,0,c->width,c->height,x,y,angle);
}
void screen_sign_set_windowc(uint8_t container) { gstate.players[0].sign.sign_container=container; }

/* ---- layout ------------------------------------------------------------- */

/* The approved order, top to bottom: line controls (the header the notice
 * composes into), the bounded sign face, then the keyboard. The pre-approval
 * native screen had the keyboard ABOVE a centred face, which no approved
 * still shows. */
struct sign_layout {
	struct menu_composition composition;
	int scale;
	struct menu_rect face;
	struct menu_rect keyboard;
 struct menu_rect count;
	int cell_w, cell_h; /* one character cell on the face */
};

static void sign_layout(const struct sign_context* c,struct sign_layout* l,int width,int height){
 menu_composition_begin(&l->composition,MENU_SCREEN_SIGN,0,0,width,height);
 bool full=l->composition.layout==MENU_SCREEN_FULL;int ox=(width-640)/2,oy=(height-(full?480:240))/2;
 l->scale=1;l->cell_w=0;l->cell_h=22;
 menu_composition_window(&l->composition,ox+(full?44:32),oy+(full?44:8),full?552:576,full?352:188);
 struct menu_rect w=l->composition.window;menu_composition_header(&l->composition,w.x+12,w.y+8,w.w-24,24);
 l->face=(struct menu_rect){ox+(full?203:44),oy+(full?112:76),full?234:194,104};
 l->keyboard=(struct menu_rect){ox+(full?44:266),oy+(full?232:47),full?552:330,full?156:133};
 l->count=(struct menu_rect){l->face.x+l->face.w-52,l->face.y-28,52,28};
 menu_composition_action(&l->composition,l->face.x+8,l->face.y+8,l->face.w-16,88);
 menu_composition_action(&l->composition,l->keyboard.x,l->keyboard.y,l->keyboard.w,l->keyboard.h);
 menu_composition_action(&l->composition,l->face.x,l->face.y-28,l->face.w-52,28);
 menu_composition_header_notice(&l->composition,gutil_connection_notice_width(c->player,!c->primary_input,true),gutil_font_width("Edit sign",16));
 menu_composition_seal(&l->composition);
}
/* The editor's centred A1 hint row in its view (controller UI redesign
 * 10-31, Q6): Type / Line / Done / Cancel. Its Done and Cancel cells ARE the
 * pointer targets (drawn == hit); Line Prev/Next stay "<" ">" arrows. */
enum { SIGN_ROW_TYPE, SIGN_ROW_LINE, SIGN_ROW_DONE, SIGN_ROW_CANCEL };
static bool sign_hint_row(const struct sign_layout* l,const struct input_player_frame* owner,struct gutil_hint_row* row){
 static const struct gutil_hint_item items[]={
  {{IB_GUI_CLICK},1,"Type",1,false},{{IB_GUI_PREVIOUS,IB_GUI_PAGE},2,"Line",0,false},
  {{IB_GUI_QUICK_MOVE},1,"Done",2,false},{{IB_INVENTORY},1,"Cancel",3,true}};
 struct menu_rect v=l->composition.view;
 return gutil_hint_row_layout(owner,v.x,v.y,v.w,v.h,l->composition.layout!=MENU_SCREEN_FULL,GUTIL_HINT_ALIGN_CENTRE,items,4,row);
}
static void sign_controls(const struct sign_layout* l,const struct input_player_frame* owner,struct gutil_button_bounds out[4]){
 int gx=l->face.x+(l->face.w-52-96)/2;
 out[0]=(struct gutil_button_bounds){gx,l->face.y-26,24,24};out[1]=(struct gutil_button_bounds){gx+72,l->face.y-26,24,24};
 out[2]=out[3]=(struct gutil_button_bounds){0,0,0,0};
 struct gutil_hint_row row;
 if(sign_hint_row(l,owner,&row))for(int k=0;k<row.count;k++){
  if(row.cells[k].item==SIGN_ROW_DONE)out[2]=row.cells[k].rect;
  if(row.cells[k].item==SIGN_ROW_CANCEL)out[3]=row.cells[k].rect;
 }
}

/* ---- window <-> shadow -------------------------------------------------- */

static void screen_sign_read_cells(const struct sign_context* c,uint8_t cells[SIGN_OSK_SIZE]) {
	struct inventory* inv
		= windowc_get_latest(c->session->windows[c->edit->sign_container]);
	for(size_t slot = 0; slot < SIGN_OSK_SIZE; slot++) {
		struct item_data chr;
		cells[slot] = inventory_get_slot(inv, slot, &chr) ? chr.count : ' ';
	}
}

static void screen_sign_sync_shadow(const struct sign_context* c) {
	struct window_container* wc = c->session->windows[c->edit->sign_container];
	uint8_t cells[SIGN_OSK_SIZE];
	screen_sign_read_cells(c,cells);

	size_t len = sign_osk_shadow_from_cells(cells, c->edit->osk_line, c->edit->shadow_line);

	// [maintainer ruling 2026-08-17]: the OSK's typing gate is Beta's OWN
	// 15-char ceiling, not the grid's 16-column width -- SIGN_COLS still
	// sizes the physical cell grid everywhere else in this file. Passing
	// SIGN_COLS here (both 16) was the bug: it let osk_feed_char's Beta
	// max-rule [740c583 GuiTextField.textboxKeyTyped: `length < max || max
	// == 0`] accept a 16th character GuiEditSign itself would refuse.
	int page=c->edit->osk.page;osk_sign_reset(&c->edit->osk,c->edit->shadow_line,sizeof c->edit->shadow_line,SIGN_BETA_MAX_CHARS);
 if(page>=0&&page<OSK_PAGES)c->edit->osk.page=page;
	c->edit->selected_slot = c->edit->osk_line * SIGN_COLS
		+ (len < SIGN_COLS ? len : SIGN_COLS - 1);
	c->edit->observed_rejection_serial = wc->rejection_serial;
}

static void screen_sign_select_line(const struct sign_context* c,size_t line) {
	c->edit->osk_line = line & (SIGN_LINES - 1);
	screen_sign_sync_shadow(c);
}


/* Send ONE authoritative character write. Shared by ordinary typing and by
 * Discard's restore, so there is exactly one sign-transaction path. */
static bool screen_sign_send_cell(const struct sign_context* c,size_t slot, uint8_t ch) {
	struct window_container* wc = c->session->windows[c->edit->sign_container];
	struct inventory* backing = windowc_get_latest(wc);
	uint16_t action_id;
	if(!windowc_new_sign_set_action(wc, &action_id, slot, (char)ch))
		return false;
	struct server_rpc rpc = {
		.type = SRPC_SIGN_SET,
		.payload.sign_set.window = c->edit->sign_container,
		.payload.sign_set.slot = slot,
		.payload.sign_set.ch = (char)ch,
		.payload.sign_set.action_id = action_id,
		/* server_local ignores these; server_remote needs them to
		 * compose Beta's whole-sign Packet130UpdateSign on close. */
		.payload.sign_set.x = backing ? backing->x : 0,
		.payload.sign_set.y = backing ? backing->y : 0,
		.payload.sign_set.z = backing ? backing->z : 0,
	};
	if(!c->send(c->user,&rpc)) {
		windowc_cancel_unsent(wc, action_id);
		return false;
	}
	windowc_get_latest(wc)->revision.sent = true;
	return true;
}

static bool screen_sign_apply_shadow(const struct sign_context* c) {
	size_t len = strlen(c->edit->shadow_line);
	uint8_t cells[SIGN_OSK_SIZE];
	struct sign_osk_edit edits[SIGN_OSK_COLS];
	screen_sign_read_cells(c,cells);
	size_t edit_count
		= sign_osk_diff_cells(cells, c->edit->osk_line, c->edit->shadow_line, edits);

	for(size_t k = 0; k < edit_count; k++) {
		if(!screen_sign_send_cell(c,edits[k].slot, (uint8_t)edits[k].ch)) {
			screen_sign_sync_shadow(c);
			return false;
		}
	}

	c->edit->selected_slot = c->edit->osk_line * SIGN_COLS
		+ (len < SIGN_COLS ? len : SIGN_COLS - 1);
	return true;
}

/* ---- discard workflow --------------------------------------------------- */

/* "Clean or reverted Cancel closes directly": both are the same test -- the
 * live content equals the content this editor opened with. A player who typed
 * a character and deleted it again is REVERTED, not changed, so no prompt. */
static bool screen_sign_changed(const struct sign_context* c) {
	uint8_t cells[SIGN_OSK_SIZE];
	screen_sign_read_cells(c,cells);
	return memcmp(cells, c->edit->opened_cells, sizeof(cells)) != 0;
}

/* "Discard restores saved content" -- through the real transaction, not a
 * local revert of the predicted window. */
static bool screen_sign_restore_saved(const struct sign_context* c){
 uint8_t cells[SIGN_OSK_SIZE];screen_sign_read_cells(c,cells);
 for(size_t slot=0;slot<SIGN_OSK_SIZE;slot++)if(cells[slot]!=c->edit->opened_cells[slot])
  if(!screen_sign_send_cell(c,slot,c->edit->opened_cells[slot]))return false;
 return true;
}

static void sign_begin_exit(const struct sign_context* c,enum sign_exit_kind kind,enum menu_sound_event cue){
 c->edit->pending_exit=kind;c->edit->exit_cue=cue;c->edit->exit_rejection=c->session->windows[c->edit->sign_container]->rejection_serial;
}
static bool sign_exit_tick(const struct sign_context* c){
 if(c->edit->pending_exit==SIGN_EXIT_NONE)return false;
 char ignored;while(sign_poll(c,&ignored)){}
 struct window_container* wc=c->session->windows[c->edit->sign_container];
 if(wc->rejection_serial!=c->edit->exit_rejection){c->edit->pending_exit=SIGN_EXIT_NONE;screen_sign_sync_shadow(c);return true;}
 if(windowc_actions_pending(wc))return true;
 if(c->edit->pending_exit==SIGN_EXIT_DISCARD&&screen_sign_changed(c)){
  if(!screen_sign_restore_saved(c))return true; /* unsent remainder retries after outstanding ACKs */
  return true; /* restore ACKs precede close */
 }
 struct server_rpc close={.type=SRPC_WINDOW_CLOSE,.payload.window_close.window=c->edit->sign_container};
 if(!c->send(c->user,&close))return true;
 c->edit->pending_exit=SIGN_EXIT_NONE;c->edit->discard_prompt=false;
 audio_menu_event(c->edit->exit_cue,time_get());c->edit->closed=true;return true;
}

/* Cancel: clean/reverted closes directly with Back; changed opens the prompt
 * with the category cue. */
static void screen_sign_cancel(const struct sign_context* c) {
	if(!screen_sign_changed(c)) {
		sign_begin_exit(c,SIGN_EXIT_CANCEL,MENU_SOUND_BACK);
		return;
	}
	c->edit->discard_prompt = true;
	c->edit->discard_selected = 0;
	c->edit->discard_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	c->edit->action_target = 4;
	c->edit->action_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	audio_menu_event(MENU_SOUND_SECTION, time_get());
}

/* Done: "existing confirmation after successfully saving changed text.
 * Unchanged Done ... use Back." */
static void screen_sign_done(const struct sign_context* c){
 sign_begin_exit(c,SIGN_EXIT_DONE,screen_sign_changed(c)?MENU_SOUND_SELECT:MENU_SOUND_BACK);
}

static void screen_sign_reset_context(const struct sign_context* c,struct screen* s, int width, int height) {
	(void)s;
	(void)width;
	(void)height;


	if(c->session->local_player)
		c->session->local_player->data.local_player.capture_input = false;

	c->edit->closed=false;
 c->edit->window_identity=c->session->windows[c->edit->sign_container];
 c->edit->local_generation=c->session->local_generation;

	c->edit->pointer_available = false;
	c->edit->discard_prompt = false;
	c->edit->discard_selected = 0;
	c->edit->discard_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	c->edit->action_target = 4;
	c->edit->action_origin = GUTIL_MENU_ORIGIN_DEFAULT;

	c->edit->osk_line=0;c->edit->pending_exit=SIGN_EXIT_NONE;c->edit->osk.page=OSK_PAGE_UPPER;c->edit->sign_focus=(struct gutil_menu_focus){.initialized=true};
	screen_sign_sync_shadow(c);
	screen_sign_read_cells(c,c->edit->opened_cells);
}

static struct gutil_button_bounds sign_discard_button(int width,int height,int index){
 return (struct gutil_button_bounds){width/2-146+index*152,height/2+10,140,44};
}
static void screen_sign_update_discard(const struct sign_context* c){
 bool back=false,confirm=false,tab=false,queued=false,typing=sign_typing(c);
 char ch;while(sign_poll(c,&ch)){queued=true;back|=ch==27;confirm|=ch==13||ch==10;tab|=ch==9;}
 bool prev=!typing&&(sign_pressed(c,IB_GUI_UP)||sign_pressed(c,IB_GUI_LEFT)),next=!typing&&(sign_pressed(c,IB_GUI_DOWN)||sign_pressed(c,IB_GUI_RIGHT));
 int before=c->edit->discard_selected;if(tab)c->edit->discard_selected^=1;if(prev)c->edit->discard_selected=0;if(next)c->edit->discard_selected=1;
 bool navigating=prev||next||tab;
 if(navigating)c->edit->discard_origin=GUTIL_MENU_ORIGIN_DIRECTIONAL;
 c->edit->pointer_available=sign_pointer(c,&c->edit->pointer_x,&c->edit->pointer_y,&c->edit->pointer_angle);
 bool pointer=gutil_menu_pointer_focus(&c->edit->sign_focus,c->edit->pointer_available,c->edit->pointer_x,c->edit->pointer_y,navigating);
 int target=-1;if(c->edit->pointer_available)for(int k=0;k<2;k++){struct gutil_button_bounds q=sign_discard_button(c->width,c->height,k);if(gutil_button_hit(&q,c->edit->pointer_x,c->edit->pointer_y)){target=k;if(pointer){c->edit->discard_selected=k;c->edit->discard_origin=GUTIL_MENU_ORIGIN_POINTER;}break;}}
 if(before!=c->edit->discard_selected)audio_menu_event(MENU_SOUND_FOCUS,time_get());
 if(!typing)back|=sign_pressed(c,IB_INVENTORY)||sign_pressed(c,IB_HOME);
 if(back){c->edit->discard_prompt=false;audio_menu_event(MENU_SOUND_BACK,time_get());return;}
 bool click=!queued&&sign_pressed(c,IB_GUI_CLICK)
  &&(c->edit->pointer_available||!typing);
 struct gutil_menu_decision decision=gutil_menu_activate_decide(
  (struct gutil_menu_activate_state){.pressed=click,.navigated=navigating,
   .pointer_valid=c->edit->pointer_available,.selection_origin=c->edit->discard_origin,
   .hovered=target,.hovered_enabled=target>=0,
   .selected=c->edit->discard_selected,.selected_enabled=true,
   .selected_destructive=c->edit->discard_selected==1,
   /* Discard throws the edit away: a destructive confirmation (Astra
    * D2-destructive ruling B) -- a sprite/ring mismatch only moves focus. */
   .destructive_dialog=true});
 c->edit->discard_selected=decision.selected;c->edit->discard_origin=decision.selection_origin;
 confirm|=decision.action==GUTIL_MENU_ACTIVATE;
 if(!confirm)return;
 if(c->edit->discard_selected==0){c->edit->discard_prompt=false;audio_menu_event(MENU_SOUND_BACK,time_get());return;}
 sign_begin_exit(c,SIGN_EXIT_DISCARD,MENU_SOUND_BACK);
}

static void screen_sign_update_context(const struct sign_context* c,struct screen* s,float dt){
 (void)s;(void)dt;struct window_container* wc=c->session->windows[c->edit->sign_container];
 if(c->edit->observed_rejection_serial!=wc->rejection_serial)screen_sign_sync_shadow(c);
 if(sign_exit_tick(c))return;
 if(c->edit->discard_prompt){screen_sign_update_discard(c);return;}
 struct sign_layout l;sign_layout(c,&l,c->width,c->height);
 bool compact=l.composition.layout==MENU_SCREEN_COMPACT,typing=sign_typing(c);
 struct input_player_frame owner;owner=*c->frame;struct gutil_button_bounds controls[4];sign_controls(&l,&owner,controls);
 bool nav=!typing&&(sign_pressed(c,IB_GUI_LEFT)||sign_pressed(c,IB_GUI_RIGHT)||sign_pressed(c,IB_GUI_UP)||sign_pressed(c,IB_GUI_DOWN)||sign_pressed(c,IB_GUI_PREVIOUS)||sign_pressed(c,IB_GUI_PAGE));
 c->edit->pointer_available=sign_pointer(c,&c->edit->pointer_x,&c->edit->pointer_y,&c->edit->pointer_angle);
 bool pointer=gutil_menu_pointer_focus(&c->edit->sign_focus,c->edit->pointer_available,c->edit->pointer_x,c->edit->pointer_y,nav);
 if(nav){c->edit->action_target=4;c->edit->action_origin=GUTIL_MENU_ORIGIN_DIRECTIONAL;}
 bool backspace=sign_pressed(c,IB_GUI_CLICK_ALT);
 bool click=sign_pressed(c,IB_GUI_CLICK)&&!backspace,done=false,cancel=false;size_t line_before=c->edit->osk_line;
 bool prev=!typing&&sign_pressed(c,IB_GUI_PREVIOUS),next=!typing&&sign_pressed(c,IB_GUI_PAGE);
 if(!typing){done=sign_pressed(c,IB_GUI_QUICK_MOVE);cancel=sign_pressed(c,IB_INVENTORY)||sign_pressed(c,IB_HOME);}
 bool pprev=false,pnext=false,pdone=false,pcancel=false;int pline=-1,hovered=-1;
 if(c->edit->pointer_available){
  for(int k=0;k<4;k++)if(gutil_button_hit(&controls[k],c->edit->pointer_x,c->edit->pointer_y)){hovered=k;break;}
  if(hovered<0)for(size_t line=0;line<SIGN_LINES;line++){struct gutil_button_bounds row={l.face.x+8,l.face.y+8+(int)line*22,l.face.w-16,22};if(gutil_button_hit(&row,c->edit->pointer_x,c->edit->pointer_y)){pline=(int)line;break;}}
  if(hovered<0&&pline<0){struct gutil_button_bounds keys={l.keyboard.x,l.keyboard.y,l.keyboard.w,l.keyboard.h};if(gutil_button_hit(&keys,c->edit->pointer_x,c->edit->pointer_y))hovered=4;}
 }
 /* Over blank (or a face row) the pointer keeps the last REAL target (Fable
  * G4 review 1): pointing away gives A back to it under rule (3). */
 if(pointer){if(hovered!=GUTIL_MENU_TARGET_NONE)c->edit->action_target=hovered;c->edit->action_origin=GUTIL_MENU_ORIGIN_POINTER;}
 struct gutil_menu_decision decision=gutil_menu_activate_decide(
  (struct gutil_menu_activate_state){.pressed=click,.navigated=nav,
   .pointer_valid=c->edit->pointer_available,.selection_origin=c->edit->action_origin,
   .hovered=hovered,.hovered_enabled=hovered>=0,.selected=c->edit->action_target,
   .selected_enabled=c->edit->action_target>=0});
 c->edit->action_target=decision.selected;c->edit->action_origin=decision.selection_origin;
 if(decision.action==GUTIL_MENU_ACTIVATE){
  pprev=decision.target==0;pnext=decision.target==1;
  pdone=decision.target==2;pcancel=decision.target==3;
 }
 if(!pointer||!click)pline=-1;
 char before[SIGN_COLS+1];memcpy(before,c->edit->shadow_line,sizeof before);int page_before=c->edit->osk.page;bool queued=false;
 bool control=(click&&(decision.action!=GUTIL_MENU_ACTIVATE||decision.target!=4))||pline>=0;
 enum osk_event ev=osk_sign_update_owned(&c->edit->osk,l.keyboard.x,l.keyboard.y,l.keyboard.w,compact,pointer,c->edit->pointer_x,c->edit->pointer_y,!done&&!cancel&&!control,&queued,c->primary_input?NULL:c->frame,c->primary_input);
 /* Commit this line before rebinding the shadow to a different line. */
 if(strcmp(before,c->edit->shadow_line)!=0){if(screen_sign_apply_shadow(c))audio_menu_event(MENU_SOUND_TYPE,time_get());}
 else if(c->edit->osk.page!=page_before)audio_menu_event(MENU_SOUND_SECTION,time_get());
 if(!queued){prev|=pprev;next|=pnext;done|=pdone;cancel|=pcancel;if(pline>=0&&(size_t)pline!=c->edit->osk_line)screen_sign_select_line(c,(size_t)pline);}
 if(prev&&c->edit->osk_line>0)screen_sign_select_line(c,c->edit->osk_line-1);
 if(next&&c->edit->osk_line+1<SIGN_LINES)screen_sign_select_line(c,c->edit->osk_line+1);
 if(ev==OSK_EV_NEXT&&c->edit->osk_line+1<SIGN_LINES)screen_sign_select_line(c,c->edit->osk_line+1);
 if(c->edit->osk_line!=line_before)audio_menu_event(MENU_SOUND_FOCUS,time_get());
 if(cancel||ev==OSK_EV_CANCEL){screen_sign_cancel(c);return;}
 if(done||ev==OSK_EV_DONE){screen_sign_done(c);return;}
}

/* ---- rendering ---------------------------------------------------------- */

static void sign_rect(int x, int y, int w, int h, uint8_t r, uint8_t g,
					  uint8_t b, uint8_t a) {
	gfx_texture(false);
	gutil_texquad_col(x, y, 0, 0, 0, 0, w, h, r, g, b, a);
	gfx_texture(true);
}

/* The approved flat board uses the pack's median plank color. Coordinates
 * map the same 48x24 pixel artwork into the full or compact face. */
static void sign_board_rect(const struct menu_rect* face,int x,int y,int w,int h,unsigned percent) {
 unsigned rgb[3];for(int k=0;k<3;k++){rgb[k]=(texture_sign_color[k]*percent+50)/100;if(rgb[k]>255)rgb[k]=255;}
 int left=x*face->w/48,top=y*face->h/24;
 sign_rect(face->x+left,face->y+top,(x+w)*face->w/48-left,(y+h)*face->h/24-top,rgb[0],rgb[1],rgb[2],255);
}
static void sign_draw_face(const struct menu_rect* face) {
 sign_board_rect(face,0,0,48,24,100);
 sign_board_rect(face,0,23,48,1,65);sign_board_rect(face,47,1,1,22,65);
 sign_board_rect(face,0,0,48,1,112);sign_board_rect(face,0,1,1,22,112);
 static const int grain[6][3]={{3,4,14},{29,4,13},{13,9,29},{2,14,18},{27,17,17},{9,21,20}};
 for(int k=0;k<6;k++)sign_board_rect(face,grain[k][0],grain[k][1],grain[k][2],1,92);
}

static void sign_draw_discard_prompt(const struct sign_context* c,int width,int height);

void screen_menu_panel_draw(const struct menu_rect* q){
 gfx_bind_texture(&texture_gui_crafting);
 for(int y=6;y<q->h-6;y+=32)for(int x=6;x<q->w-6;x+=32){int w=q->w-6-x<32?q->w-6-x:32,h=q->h-6-y<32?q->h-6-y:32;gutil_texquad(q->x+x,q->y+y,150,62,(w+1)/2,(h+1)/2,w,h);}
 gutil_texquad(q->x,q->y,0,0,3,3,6,6);gutil_texquad(q->x+6,q->y,3,0,170,3,q->w-12,6);
 gutil_texquad(q->x+q->w-6,q->y,173,0,3,3,6,6);gutil_texquad(q->x,q->y+6,0,3,3,160,6,q->h-12);
 gutil_texquad(q->x+q->w-6,q->y+6,173,3,3,160,6,q->h-12);gutil_texquad(q->x,q->y+q->h-6,0,163,3,3,6,6);
 gutil_texquad(q->x+6,q->y+q->h-6,3,163,170,3,q->w-12,6);gutil_texquad(q->x+q->w-6,q->y+q->h-6,173,163,3,3,6,6);
}
static void sign_draw_discard_prompt(const struct sign_context* c,int width,int height){
 sign_rect(0,0,width,height,0,0,0,153);
 struct menu_rect panel={width/2-230,height/2-70,460,140};screen_menu_panel_draw(&panel);
 const char* title="Discard sign changes?",*detail="Unsaved text will be lost.";
 gutil_text_rgb((width-gutil_font_width(title,18))/2,panel.y+19,title,18,false,64,64,64);
 gutil_text_rgb((width-gutil_font_width(detail,14))/2,panel.y+51,detail,14,false,64,64,64);
 struct input_player_frame owner;owner=*c->frame;
 for(int k=0;k<2;k++){struct gutil_button_bounds q=sign_discard_button(width,height,k);int ty=c->edit->discard_selected==k?86:66;
  gfx_bind_texture(&texture_gui_widgets);gutil_texquad(q.x,q.y,0,ty,35,20,70,44);gutil_texquad(q.x+70,q.y,165,ty,35,20,70,44);
  const char* label=k?"Discard":"Back";
  gutil_text(q.x+(q.width-gutil_font_width(label,16))/2,q.y+14,label,16,true);
 }
 /* No glyph inside the buttons (10-31): the centred A1 row names them. */
 static const struct gutil_hint_item items[]={
  {{IB_INVENTORY},1,"Back",1,true},{{IB_GUI_CLICK},1,"Discard",0,false}};
 struct gutil_hint_row row;
 if(gutil_hint_row_layout(&owner,0,0,width,height,height<480,GUTIL_HINT_ALIGN_CENTRE,items,2,&row))
  gutil_hint_row_draw(&row);
}

static void screen_sign_render2D_context(const struct sign_context* c,struct screen* s,int width,int height){
 (void)s;sign_rect(0,0,width,height,0,0,0,180);
 struct sign_layout l;sign_layout(c,&l,width,height);bool compact=l.composition.layout==MENU_SCREEN_COMPACT;
 screen_menu_panel_draw(&l.composition.window);gutil_text_rgb(l.composition.header.x,l.composition.header.y+4,"Edit sign",16,false,64,64,64);
 struct input_player_frame owner;owner=*c->frame;struct gutil_button_bounds controls[4];sign_controls(&l,&owner,controls);
 /* Line Prev/Next: plain "<" ">" click targets (10-31, Q2). */
 gutil_text_rgb(controls[0].x+(controls[0].width-gutil_font_width("<",16))/2,controls[0].y+4,"<",16,false,64,64,64);
 gutil_text_rgb(controls[0].x+controls[0].width+6,controls[0].y+5,"Line",14,false,64,64,64);
 gutil_text_rgb(controls[1].x+(controls[1].width-gutil_font_width(">",16))/2,controls[1].y+4,">",16,false,64,64,64);
 if(strlen(c->edit->shadow_line)>=12){char count[16];snprintf(count,sizeof count,"%u/15",(unsigned)strlen(c->edit->shadow_line));gutil_text_rgb(l.count.x+l.count.w-gutil_font_width(count,12),l.count.y+8,count,12,false,64,64,64);}
 sign_draw_face(&l.face);uint8_t cells[SIGN_OSK_SIZE];screen_sign_read_cells(c,cells);
 for(size_t line=0;line<SIGN_LINES;line++){int ly=l.face.y+8+(int)line*22;
 if(line==c->edit->osk_line)sign_rect(l.face.x+8,ly,l.face.w-16,22,255,255,255,85);
 char text[SIGN_COLS+1];sign_osk_shadow_from_cells(cells,line,text);gutil_text_rgb(l.face.x+(l.face.w-gutil_font_width(text,14))/2,ly+4,text,14,false,32,27,20);}
 osk_sign_render(&c->edit->osk,l.keyboard.x,l.keyboard.y,l.keyboard.w,compact);
 {struct gutil_hint_row row;if(sign_hint_row(&l,&owner,&row))gutil_hint_row_draw(&row);}
 if(!c->edit->discard_prompt&&c->edit->action_target>=0&&c->edit->action_target<4)
  gutil_menu_focus_border(&controls[c->edit->action_target]);
 if(c->primary_input)screen_notice_compose(l.composition.notice.x,l.composition.notice.y,l.composition.notice.w,l.composition.notice.h,l.composition.notice_in_header);
 else gutil_connection_notices_in_header(c->player,true,l.composition.notice.x,l.composition.notice.y,l.composition.notice.w,l.composition.notice.h);
 if(c->edit->discard_prompt)sign_draw_discard_prompt(c,width,height);
 if(c->edit->pointer_available){gfx_bind_texture(&texture_pointer);gutil_texquad_rt_any(c->edit->pointer_x,c->edit->pointer_y,glm_rad(c->edit->pointer_angle),0,0,256,256,48,48);}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
bool screen_sign_test_discard_open(void) {
	return gstate.players[0].sign.discard_prompt;
}
void screen_sign_test_layout(int width, int height, struct menu_composition* out,
							 struct menu_rect* face, struct menu_rect* keyboard) {
	struct sign_layout layout;
	struct sign_context context={.session=&gstate.players[0],.edit=&gstate.players[0].sign,.primary_input=true};
	sign_layout(&context,&layout, width, height);
	if(out)
		*out = layout.composition;
	if(face)
		*face = layout.face;
	if(keyboard)
		*keyboard = layout.keyboard;
}
/* The four pointer targets (Line <, Line >, Done, Cancel) of `player`'s owned
 * editor in a width x height view -- Done/Cancel are the hint-row cells. */
void screen_sign_test_controls(unsigned player, const struct input_player_frame* frame,
							   int width, int height, struct gutil_button_bounds out[4]) {
	struct sign_layout layout;
	struct sign_context context={.player=player,.frame=frame,.width=width,.height=height};
	sign_layout(&context,&layout,width,height);
	sign_controls(&layout,frame,out);
}
#endif

static bool sign_primary_send(void* user,struct server_rpc* rpc) { (void)user;return svin_rpc_send(rpc); }
static struct sign_context sign_primary_context(struct input_player_frame* frame,int width,int height) {
 input_prompt_frame(0,frame);
 return (struct sign_context){.session=&gstate.players[0],.edit=&gstate.players[0].sign,.frame=frame,.width=width,.height=height,.primary_input=true,.send=sign_primary_send};
}
static bool sign_window_valid(struct client_session* s) {
 struct window_container* wc=s?s->windows[s->sign.sign_container]:NULL;
 return wc&&wc==s->sign.window_identity&&s->local_generation==s->sign.local_generation
  &&wc->type==WINDOW_TYPE_SIGN&&wc->slot_count==SIGN_OSK_SIZE;
}
bool sign_editor_open(struct client_session* s,uint8_t window) {
 if(!s||!s->windows[window]||s->windows[window]->type!=WINDOW_TYPE_SIGN||s->windows[window]->slot_count!=SIGN_OSK_SIZE)return false;
 s->sign=(struct sign_editor){.sign_container=window};
 struct sign_context c={.session=s,.edit=&s->sign};
 screen_sign_reset_context(&c,NULL,0,0);return true;
}
bool sign_editor_update(struct client_session* s,unsigned player,const struct input_player_frame* frame,int x,int y,int width,int height,float dt,container_send_fn send,void* user) {
 if(!sign_window_valid(s)||!frame||!frame->connected||!send||player>=CLIENT_LOCAL_PLAYERS||width<640||height<240||s->local_reply_failed)return false;
 if(s->sign.closed)return true;
 struct input_player_frame local=*frame;
 local.pointer_valid=input_owner_pointer(frame,x,y,width,height,&local.pointer_x,&local.pointer_y,&local.pointer_angle);
 local.pointer_normalized=false;
 struct sign_context c={.session=s,.edit=&s->sign,.frame=&local,.player=player,.width=width,.height=height,.send=send,.user=user};
 screen_sign_update_context(&c,NULL,dt);return s->sign.closed;
}
void sign_editor_render(struct client_session* s,unsigned player,const struct input_player_frame* frame,int width,int height) {
 if(!sign_window_valid(s)||!frame||player>=CLIENT_LOCAL_PLAYERS||width<640||height<240)return;
 struct sign_context c={.session=s,.edit=&s->sign,.frame=frame,.player=player,.width=width,.height=height};
 screen_sign_render2D_context(&c,NULL,width,height);
}
static void screen_sign_reset(struct screen* screen,int width,int height) {
 input_pointer_enable(true);input_text_enable(true);screen->render3D=screen_ingame.render3D;
 struct input_player_frame frame;struct sign_context c=sign_primary_context(&frame,width,height);
 if(!sign_window_valid(c.session)||c.edit->closed)screen_sign_reset_context(&c,screen,width,height);
}
static void screen_sign_update(struct screen* screen,float dt) {
 struct input_player_frame frame;struct sign_context c=sign_primary_context(&frame,gfx_width(),gfx_height());
 if(!sign_window_valid(c.session))return;
 screen_sign_update_context(&c,screen,dt);
 if(c.edit->closed){input_text_enable(false);screen_set(&screen_ingame);}
}
static void screen_sign_render2D(struct screen* screen,int width,int height) {
 struct input_player_frame frame;struct sign_context c=sign_primary_context(&frame,width,height);
 if(sign_window_valid(c.session))screen_sign_render2D_context(&c,screen,width,height);
}

struct screen screen_sign = {
	.reset = screen_sign_reset,
	.update = screen_sign_update,
	.render2D = screen_sign_render2D,
	.render3D = NULL,
	.render_world = true,
};
