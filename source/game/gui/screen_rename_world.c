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

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <m-lib/m-string.h>

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../network/level_archive.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/input_owner.h"
#include "frontend_presentation.h"
#include "../game_state.h"
#include "screen.h"
#include "widget_osk.h"

/* Beta GuiRenameWorld [740c583]: title "Rename World" at h/4-60+20
 * (0xFFFFFF), "Enter world name" label (0xA0A0A0) at (w/2-100, 47), ONE
 * field (w/2-100, y=60, 200x20, max 32) prefilled with the world's
 * CURRENT LevelName and focused, Rename (h/4+96+12) + Cancel (h/4+120+12)
 * buttons. Rename.enabled = getText().TRIM().length() > 0 -- note the
 * trim, unlike the create dialog's raw length. actionPerformed(0) calls
 * renameWorld(folder, text.trim()): the SAVED name is TRIMMED and the
 * FOLDER does not change (level.dat's LevelName is rewritten in place).
 * '\r' presses Rename; Cancel returns to the parent.
 *
 * ADAPTED (carve-out #2): the approved frontend keyboard supplies real
 * middle insertion and caret movement. OSK cells and physical typing share
 * the frontend mutator. Native length, trim, folder and save semantics stay
 * above; Cancel and physical ESC return without saving. */

#define RWORLD_NAME_MAX 32 /* [740c583 GuiRenameWorld.initGui] */

static char rename_folder_path[512]; /* the world DIRECTORY (unchanged) */
static char name_buf[RWORLD_NAME_MAX + 4];
static char prefill_buf[RWORLD_NAME_MAX + 4];
static struct osk_frontend osk;
static bool pointer_available;
static float pointer_x, pointer_y, pointer_angle;
static bool rename_latch;
static bool rename_failed;

void screen_rename_world_setup(const char* directory_path,
							   const char* current_name) {
	assert(directory_path && current_name);
	snprintf(rename_folder_path, sizeof(rename_folder_path), "%s",
			 directory_path);
	/* prefill = the CURRENT LevelName [740c583 GuiRenameWorld.initGui ->
	 * getWorldInfo().getWorldName()]; the caller passes the resolved row
	 * name. Truncated to the field max like any typed content. */
	snprintf(prefill_buf, sizeof(prefill_buf), "%.32s", current_name);
}

/* The rename operation [740c583 ISaveFormat.renameWorld]: rewrite
 * level.dat's LevelName IN PLACE (trimmed); the folder never changes.
 * Exposed for the env test. */
bool screen_rworld_apply(const char* directory_path, const char* new_name) {
	assert(directory_path && new_name);

	char trimmed[64];
	screen_cworld_java_trim(new_name, trimmed, sizeof(trimmed));
	if(!trimmed[0])
		return false; /* the enable rule already guards; belt-and-braces */

	string_t dir;
	string_init_set_str(dir, directory_path);
	struct level_archive la;
	if(!level_archive_create(&la, dir)) {
		string_clear(dir);
		return false;
	}
	string_clear(dir);

	bool ok = level_archive_write(&la, LEVEL_NAME, trimmed)
		&& level_archive_save(&la);
	level_archive_discard(&la);
	return ok;
}

static void screen_rworld_reset(struct screen* s, int width, int height) {
	(void)s;
	(void)width;
	(void)height;
	assert(rename_folder_path[0]); /* setup() must precede screen_set */
	input_pointer_enable(true);
	input_text_enable(true);

	snprintf(name_buf, sizeof(name_buf), "%s", prefill_buf);
	osk_frontend_reset(&osk, name_buf, sizeof(name_buf), RWORLD_NAME_MAX, 0, false);
	rename_latch = false;
	pointer_available = false;
	rename_failed = false;
}

static void rworld_leave(struct screen* to) {
	input_text_enable(false);
	rename_folder_path[0] = 0;
	screen_set(to);
}

static bool rworld_enabled(void) {
	/* Rename.enabled = trim(text).length > 0 [740c583 GuiRenameWorld
	 * .keyTyped] */
	char trimmed[64];
	screen_cworld_java_trim(name_buf, trimmed, sizeof(trimmed));
	return trimmed[0] != 0;
}

static void rworld_try_rename(void) {
	if(rename_latch || !rworld_enabled())
		return;
	rename_latch = true;
	if(screen_rworld_apply(rename_folder_path, name_buf)) {
		rworld_leave(&screen_select_world);
	} else {
		fprintf(stderr, "[rworld] rename failed for %s\n",
				rename_folder_path);
		rename_latch = false; /* stay, retryable */
		rename_failed = true;
	}
}

/* The frontend keyboard owns all visible action rectangles; Done retains
 * this screen's real trim/save operation, Cancel changes neither name nor folder. */
static void screen_rworld_update(struct screen* s, float dt) {
	(void)s; (void)dt;
	pointer_available = input_pointer(&pointer_x,&pointer_y,&pointer_angle);
	struct input_player_frame scratch;
	enum osk_event ev=osk_frontend_update_owned(&osk,gfx_width(),gfx_height(),
		osk_frontend_input_frame(&scratch),true,pointer_available,pointer_x,pointer_y,true,rworld_enabled());
	if(ev==OSK_EV_DONE) rworld_try_rename();
	else if(ev==OSK_EV_CANCEL) rworld_leave(&screen_select_world);
	else if(ev==OSK_EV_CHANGED) rename_failed=false;
	/* OSK_EV_NEXT is inert: Rename has only one field. */
}
static int rw_d(int value,int height) {
	return (int)((float)value*(height>0?height:480)/480.0F+.5F);
}
static int rw_x(int value,int width,int height) {
	int ox=(width-rw_d(640,height))/2;
	return (ox>0?ox:0)+rw_d(value,height);
}
/* A bare Remote cannot own gameplay input, so it has no player frame.
 * Keep this editor's presentation useful before assignment as well. Resolve
 * the Remote's configured bindings; never advertise a Nunchuk-only button. */
static void rworld_prompt_frame(struct input_player_frame* prompts) {
	input_prompt_frame(0, prompts);
	if(prompts->category != INPUT_CAT_NONE || prompts->glyphs[IB_GUI_CLICK] >= 0)
		return;
	prompts->category = INPUT_CAT_WIIMOTE;
	static const struct { enum input_button role; const char* key; } bindings[] = {
		{IB_GUI_CLICK, "input.gui_click"},
		{IB_GUI_PREVIOUS, "input.gui_previous"},
		{IB_GUI_PAGE, "input.gui_page"},
		{IB_GUI_CLICK_ALT, "input.gui_click_alt"},
		{IB_INVENTORY, "input.inventory"},
		{IB_GUI_QUICK_MOVE, "input.gui_quick_move"},
	};
	for(size_t k = 0; k < sizeof(bindings) / sizeof(bindings[0]); k++) {
		int keys[INPUT_BINDINGS_PER_ACTION];
		size_t count = INPUT_BINDINGS_PER_ACTION;
		if(config_read_int_array(&gstate.config_user, bindings[k].key, keys, &count))
			prompts->glyphs[bindings[k].role]
				= input_glyph_binding(prompts->category, keys, count);
	}
}

static void screen_rworld_render2D(struct screen* s,int width,int height) {
	(void)s;
	if(!fep_backdrop(width,height,90)) gutil_bg();
	struct fep_rect panel=fep_rect(rw_x(20,width,height),rw_d(16,height),rw_d(600,height),rw_d(448,height));
	fep_paper(panel); fep_bevel(panel);
	gutil_text_rgb(rw_x(32,width,height),rw_d(28,height),"Rename world",rw_d(27,height),false,53,53,53);
	char count[32]; snprintf(count,sizeof(count),"%u / %u",(unsigned)strlen(name_buf),RWORLD_NAME_MAX);
	int count_font=rw_d(14,height);
	gutil_text_rgb(rw_x(608,width,height)-gutil_font_width(count,count_font),rw_d(37,height),count,count_font,false,53,53,53);
	struct fep_rect field=fep_rect(rw_x(32,width,height),rw_d(76,height),rw_d(576,height),rw_d(52,height));
	fep_fill(field,0x19,0x1A,0x17,255);
	fep_outline(field,rw_d(1,height),85,85,85,255);
	fep_fill(fep_rect(field.x,field.y+field.h-rw_d(2,height),field.w,rw_d(2,height)),184,188,178,255);
	int pad=rw_d(16,height),font=rw_d(20,height),caret_w=rw_d(2,height);
	size_t start=osk_frontend_reveal(&osk,font,field.w-2*pad-caret_w);
	int text_x=field.x+pad,text_y=field.y+(field.h-font)/2;
	gfx_scissor(true,(uint32_t)text_x,(uint32_t)field.y,(uint32_t)(field.w-2*pad),(uint32_t)field.h);
	gutil_text_rgb(text_x,text_y,name_buf+start,font,false,244,244,237);
	int caret_x=text_x;
	for(size_t i=start;i<osk.caret;i++) { char ch[2]={name_buf[i],0}; caret_x+=gutil_font_width(ch,font); }
	fep_fill(fep_rect(caret_x,text_y,caret_w,font),255,255,160,255);
	gfx_scissor(false,0,0,0,0);
	if(rename_failed)
		gutil_text_rgb(rw_x(32,width,height),rw_d(136,height),"Could not rename this world. Try again.",rw_d(14,height),false,121,44,37);
	struct input_player_frame prompts; rworld_prompt_frame(&prompts);
	static const enum input_button roles[] = {IB_GUI_PREVIOUS, IB_GUI_PAGE,
		IB_GUI_CLICK_ALT, IB_INVENTORY, IB_GUI_QUICK_MOVE};
	for(size_t k = 0; k < sizeof(roles) / sizeof(roles[0]); k++)
		osk.prompt_glyphs[k] = prompts.glyphs[roles[k]];
	osk_frontend_render(&osk,width,height,rworld_enabled());
	osk_frontend_hint_row(&osk,&prompts,width,height);
	if(pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(pointer_x,pointer_y,glm_rad(pointer_angle),0,0,256,256,48*GFX_GUI_SCALE,48*GFX_GUI_SCALE);
	}
}

struct screen screen_rename_world = {
	.reset = screen_rworld_reset,
	.update = screen_rworld_update,
	.render2D = screen_rworld_render2D,
	.render3D = NULL,
	.render_world = false,
};
