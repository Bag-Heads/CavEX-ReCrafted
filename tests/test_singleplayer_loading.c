/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include "game/gui/screen_load_world.h"
#include "game/gui/loading_indicator.h"
#include "game/game_state.h"
#include "network/client_world_payload.h"
#include "network/level_archive.h"
#include "graphics/texture_atlas.h"
#include "platform/texture.h"
#include "game/gui/loading_tips.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Capture the actual production status renderer without a GPU. */
struct quad { int x,y,w,h; unsigned rgb; };
static struct quad quads[16];
static unsigned quad_count, text_count;
static char texts[3][256];
static int text_y[3], text_size[3];
static int viewport_width=640, text_x[3];
/* Fake the existing main-thread census, not the progress/gate arithmetic. */
struct game_state gstate;
static struct world host_terrain;
static int missing, pending, radius=1, scanned_radius, scanned_cx, scanned_cz;
static const char* archive_name;
static unsigned archive_opens, archive_discards, census_calls, import_calls;
static bool converting;
/* Optional PC preview command stream, rasterized with each real pack by
 * the job script. Normal tests retain their fixed headless font metrics. */
static bool preview;
static uint8_t preview_font_widths[256];
struct tex_gfx texture_terrain, texture_menu_background, texture_nether_background;
static struct tex_gfx* bound_texture;
static unsigned terrain_quads, menu_quads, dirt_calls, scene_quads;
static struct tex_gfx* scene_texture = &texture_nether_background;
void tex_gfx_clear(struct tex_gfx* tex) { (void)tex; assert(!"portal freed a resident texture"); }
void gfx_wait_idle(void) { assert(!"portal waited for GPU"); }
/* This focused executable links the production episode owner too. */
#include "../source/game/gui/loading_tips.c"

static uint8_t netherrack_cell = 103;
void gfx_bind_texture(struct tex_gfx* tex) { bound_texture=tex; }
uint8_t tex_atlas_lookup(enum tex_atlas_entry entry) {
	assert(entry==TEXAT_NETHERRACK); return netherrack_cell;
}
void gutil_bg(void) { dirt_calls++; }
void gfx_draw_quads_flt(size_t count,const float* v,const uint8_t* c,const float* uv) {
	assert(count==4);
	if(preview && bound_texture==scene_texture) {
		printf("{\"kind\":\"scene\",\"shade\":%u}\n",c[0]); return;
	}
	if(preview) {
		printf("{\"kind\":\"tile\",\"rect\":[%.0f,%.0f,%.0f,%.0f],\"uv\":[%g,%g,%g,%g],\"shade\":%u}\n",
			v[0],v[1],v[6]-v[0],v[7]-v[1],uv[0],uv[1],uv[4],uv[5],c[0]);
		return;
	}
	if(bound_texture==&texture_terrain) {
		terrain_quads++;
		assert(v[0]>=0 && v[1]>=0 && v[6]<=854 && v[7]<=480);
		assert(c[0]==c[1] && c[1]==c[2] && c[3]==255);
		assert(c[0]==64 || c[0]==96);
		assert(uv[0]==(netherrack_cell%16)/16.0f);
		assert(uv[1]==(netherrack_cell/16)/16.0f);
		assert(uv[4]<=uv[0]+1/16.0f && uv[5]<=uv[1]+1/16.0f);
	} else if(bound_texture==scene_texture) { scene_quads++; }
	else { assert(bound_texture==&texture_menu_background); menu_quads++; }
}
static void portal_scene_lifecycle(void) {
 loading_tips_reset();
 texture_nether_background=(struct tex_gfx){.data=(void*)1,.width=1024,.height=576};
 loading_reason_begin(0,PORTAL_IN);
 lworld_draw_loading_backdrop(PORTAL_IN,640,480,0,1); assert(scene_quads==1);
 loading_reason_begin(1,PORTAL_OUT);
 loading_reason_end(0);
 lworld_draw_loading_backdrop(PORTAL_OUT,640,240,0,1); assert(scene_quads==2);
 assert(loading_reason_get(1)==PORTAL_OUT);
 loading_reason_end(1); loading_tips_reset();
 assert(texture_nether_background.data==(void*)1);
 texture_nether_background=(struct tex_gfx){0};
 quad_count=0;
}
static void portal_backdrops(void) {
	/* No Nether bake/image exists: both portal directions must immediately
	 * use the active atlas. Normal reasons retain the existing landscape. */
	texture_terrain.data=(void*)1;
	texture_terrain.width=texture_terrain.height=256;
	texture_menu_background.data=(void*)1;
	texture_menu_background.width=1024; texture_menu_background.height=576;
	for(int reason=NEW_WORLD;reason<=UNKNOWN;reason++) {
		terrain_quads=menu_quads=0; quad_count=0;
		lworld_draw_loading_backdrop(reason,640,480,0,1);
		bool portal=reason==PORTAL_IN || reason==PORTAL_OUT;
		assert(terrain_quads==(portal?80u:0u));
		assert(menu_quads==(portal?0u:1u));
	}
	/* Atlas remap and HD pack dimensions cannot change the normalized cell. */
	netherrack_cell=19; texture_terrain.width=texture_terrain.height=1024;
	terrain_quads=0;
	lworld_draw_loading_backdrop(PORTAL_OUT,854,240,0,2);
	assert(terrain_quads==56);
	/* Default/unknown legacy style cannot disable the portal fallback. */
	for(int style=0;style<=3;style+=3) {
		terrain_quads=menu_quads=quad_count=0;
		lworld_draw_loading_backdrop(PORTAL_IN,640,480,0,style);
		assert(terrain_quads==80 && !menu_quads);
	}
	texture_terrain.data=NULL; menu_quads=quad_count=0;
	lworld_draw_loading_backdrop(PORTAL_IN,640,480,0,1);
	assert(menu_quads==1);
	texture_menu_background.data=NULL;
	lworld_draw_loading_backdrop(PORTAL_OUT,640,480,0,1);
	assert(dirt_calls==1);
	netherrack_cell=103;
}
static uint32_t import_percent;
bool server_local_import_progress(uint32_t* out) {
	import_calls++; *out=import_percent; return converting;
}
bool clin_world_payloads_enabled(void) { return true; }
unsigned clin_world_payload_primary_slot(void) { return 1; }
bool clin_world_payload_of_slot(unsigned slot,struct clin_world_payload* out) {
	assert(slot==1); out->terrain=&host_terrain; return true;
}
int world_server_view_radius(void) { return radius; }
int world_lod_near_radius(void) { return 1; }
int config_read_int(struct config* c,const char* key,int fallback) {
	(void)c; (void)key; return fallback;
}
void world_near_ring_status(struct world* w,w_coord_t cx,w_coord_t cz,int r,
	int* out_missing,int* out_pending) {
	assert(w==&host_terrain); /* host may occupy the secondary world slot */
	census_calls++;
	scanned_radius=r; scanned_cx=cx; scanned_cz=cz;
	if(out_missing) *out_missing=missing;
	if(out_pending) *out_pending=pending;
}
bool level_archive_create(struct level_archive* la,string_t path) {
	(void)la; assert(!strcmp(string_get_cstr(path),"saves/folder-id"));
	archive_opens++; return archive_name!=NULL;
}
bool level_archive_read(struct level_archive* la,struct level_archive_tag tag,
	void* out,size_t length) {
	(void)la; assert(!strcmp(tag.name,".Data.LevelName") && tag.type==TAG_STRING);
	snprintf(out,length,"%s",archive_name); return true;
}
void level_archive_discard(struct level_archive* la) { (void)la; archive_discards++; }
void gfx_texture(bool enable) { (void)enable; }
int gutil_font_width(const char* s,int size) {
	if(!preview) return (int)strlen(s)*size/2;
	int width=0;
	for(;*s;s++) width+=(preview_font_widths[(unsigned char)*s]+1)*size/8;
	return width;
}
void gutil_texquad_col(int x,int y,int tx,int ty,int sx,int sy,int w,int h,
	uint8_t r,uint8_t g,uint8_t b,uint8_t a) {
	if(!preview && bound_texture==&texture_terrain) {
		/* hw1102c: the netherrack fallback uses gutil_bg's padded-atlas
		 * texcoords (the float-UV path tiled the whole atlas on Wii). */
		terrain_quads++;
		assert(x>=0 && y>=0 && x+w<=854 && y+h<=480);
		assert(r==g && g==b && a==255 && (r==64 || r==96));
		assert(tx==(netherrack_cell % 14) * 18 + 3); /* TEX_OFFSET(TEXTURE_X()) */
		assert(ty==(netherrack_cell / 14) * 18 + 3); /* TEX_OFFSET(TEXTURE_Y()) */
		assert(sx>0 && sx<=16 && sy>0 && sy<=16);
		return;
	}
	(void)tx; (void)ty; (void)sx; (void)sy;
	if(preview) {
		printf("{\"kind\":\"solid\",\"rect\":[%d,%d,%d,%d],\"rgba\":[%u,%u,%u,%u]}\n",x,y,w,h,r,g,b,a);
		return;
	}
	assert((a==255 || a==72) && quad_count<16);
	quads[quad_count++]=(struct quad){x,y,w,h,(unsigned)r<<16|(unsigned)g<<8|b};
}
void gutil_text_rgb(int x,int y,const char* s,int size,bool shadow,
	uint8_t r,uint8_t g,uint8_t b) {
	if(preview) {
		printf("{\"kind\":\"text\",\"x\":%d,\"y\":%d,\"size\":%d,\"text\":\"%s\"}\n",x,y,size,s);
		return;
	}
	assert(text_count<3 && shadow && r==255 && g==255 && b==255);
	assert(x==(viewport_width-gutil_font_width(s,size))/2);
	snprintf(texts[text_count],sizeof texts[0],"%s",s);
	text_x[text_count]=x; text_y[text_count]=y; text_size[text_count++]=size;
}
static void clear(void) { quad_count=text_count=0; }
static void remote_unchanged(void) {
	for(int split=0;split<2;split++) for(int tick=0;tick<40;tick++) {
		int height=split?240:480;
		float elapsed=tick*0.17f;
		clear();
		/* Same seam used by the actual remote terrain renderer. Arbitrary
		 * local arguments must have no effect on any remote drawing. */
		lworld_draw_loading_status(true,640,height,elapsed,"Connecting",
			"LOCAL NAME","LOCAL STAGE",72);
		assert(text_count==1 && !strcmp(texts[0],"Connecting"));
		assert(text_y[0]==(split?24:170) && text_size[0]==22);
		struct loading_indicator old;
		loading_indicator_layout(640,height,elapsed,&old);
		assert(quad_count==2*LOADING_INDICATOR_SQUARES);
		for(int k=0;k<LOADING_INDICATOR_SQUARES;k++) {
			const struct loading_indicator_square* q=&old.squares[k];
			int b=LOADING_INDICATOR_BORDER;
			struct quad border=quads[2*k], fill=quads[2*k+1];
			assert(border.x==q->x-b && border.y==q->y-b);
			assert(border.w==q->size+2*b && border.h==q->size+2*b);
			assert(border.rgb==LOADING_INDICATOR_BORDER_RGB);
			assert(fill.x==q->x && fill.y==q->y && fill.w==q->size && fill.h==q->size);
			assert(fill.rgb==(k==old.lit?LOADING_INDICATOR_LIT_RGB:LOADING_INDICATOR_UNLIT_RGB));
		}
	}
}
static void measured_progress(void) {
	struct lworld_loading_progress p={.percent=-1};
	/* A backwards/unknown sample must keep the actual previous fill. */
	uint32_t samples[]={0,1,42,41,UINT32_MAX,101,42,99,100,0,UINT32_MAX};
	int expected[]={0,1,42,42,42,42,42,99,100,100,100};
	int last=-1;
	for(unsigned k=0;k<sizeof samples/sizeof samples[0];k++) {
		lworld_loading_progress_update(&p,samples[k]);
		assert(p.percent==expected[k]);
		if(p.percent>=0) { assert(p.percent>=last && p.percent<=100); last=p.percent; }
		clear();
		lworld_draw_loading_status(false,640,480,10,"ignored","New World",
			"Generating level",p.percent);
		assert(text_count==3 && !strcmp(texts[0],"Building terrain"));
		assert(!strcmp(texts[1],"New World"));
		assert(text_y[0]==150 && text_size[0]==24 && text_y[1]==186 && text_y[2]==248);
		assert(quads[0].x==118 && quads[0].y==220 && quads[0].w==404 && quads[0].h==16);
		/* Beta LoadingScreenRenderer.setLoadingProgress: track 8421504,
		 * fill 8454016, track quad precedes proportional fill quad. */
		assert(quads[1].x==120 && quads[1].y==222 && quads[1].w==400 && quads[1].h==12);
		assert(quads[1].rgb==0x808080);
		if(p.percent>=0) {
			/* fep_solid intentionally omits zero-width quads. */
			assert(quad_count==(p.percent?3u:2u));
			if(p.percent) {
				assert(quads[2].w==p.percent*4 && quads[2].h==12);
				assert(quads[2].rgb==0x80FF80);
			}
			assert(strchr(texts[2],'%'));
		} else { assert(quad_count==2 && !strchr(texts[2],'%')); }
	}
	p=(struct lworld_loading_progress){.percent=-1};
	lworld_loading_progress_update(&p,0);
	assert(p.percent==0); /* a fresh episode may begin at zero */
	p=(struct lworld_loading_progress){.percent=-1};
	lworld_loading_progress_update(&p,UINT32_MAX);
	assert(p.percent==-1); /* unknown before the first measurement stays unknown */
}

static void normal_load_gate(void) {
	/* Use the actual name lookup and admission episode, as loading_notice's
	 * gate_send_load does before it queues SRPC_LOAD_WORLD. */
	archive_name="A saved world name";
	lworld_set_world_path("saves/folder-id");
	assert(archive_opens==1 && archive_discards==1);
	lworld_reset_presentation();
	gstate.players[0].camera.x=160;
	gstate.players[0].camera.z=-64;
	missing=9; pending=0;
	assert(lworld_local_loading_percent()==0 && !lworld_prepare_presentation());
	/* The server's position/load marker is a separate, real gate prerequisite.
	 * Mesh work may finish first, but this must never read 100 prematurely. */
	missing=0;
	assert(lworld_local_loading_percent()==99 && !lworld_prepare_presentation());
	lworld_reset_presentation();
	gstate.world_loaded=true;
	struct lworld_loading_progress p={.percent=-1};
	for(int left=9*COLUMN_HEIGHT;left>=0;left--) {
		missing=left/COLUMN_HEIGHT; pending=left%COLUMN_HEIGHT;
		uint32_t percent=lworld_local_loading_percent();
		assert(percent==(uint32_t)(9*COLUMN_HEIGHT-left)*100/(9*COLUMN_HEIGHT));
		assert(scanned_radius==1 && scanned_cx==10 && scanned_cz==-4);
		bool ready=lworld_prepare_presentation();
		assert(ready==(left==0) && (percent==100)==ready);
		lworld_loading_progress_update(&p,percent);
		assert(p.percent==(int)percent);
		assert(!strcmp(lworld_world_name(),archive_name));
		clear();
		lworld_draw_loading_status(false,640,480,0,"ignored",lworld_world_name(),
			"Loading level",p.percent);
		assert(!strcmp(texts[1],archive_name));
	}
	/* Prepared admission latches across a later re-dirty, exactly as the gate. */
	pending=1;
	assert(lworld_prepare_presentation() && lworld_local_loading_percent()==100);
	/* A different radius uses the same denominator as the gate. */
	lworld_reset_presentation(); radius=2; missing=1; pending=0;
	assert(lworld_local_loading_percent()==96 && !lworld_prepare_presentation());
	assert(scanned_radius==2);
	missing=0; pending=1;
	assert(lworld_local_loading_percent()==99 && !lworld_prepare_presentation());
	pending=0;
	assert(lworld_local_loading_percent()==100 && lworld_prepare_presentation());
	/* A missing LevelName falls back to the path basename, never the old name. */
	archive_name=NULL;
	lworld_set_world_path("saves/folder-id");
	lworld_reset_presentation();
	assert(!strcmp(lworld_world_name(),"folder-id"));
	lworld_set_world_path(NULL);
	assert(!lworld_world_name()[0]);
}

static void owner_progress_and_stage_switch(void) {
	const char* stage;
	lworld_reset_presentation();
	gstate.world_loaded=true; radius=1; missing=pending=0;
	assert(lworld_prepare_presentation()); /* host's gate is latched ready */
	assert(lworld_local_loading_status(0,false,&stage)==100);
	for(int pass=0;pass<2;pass++) {
		unsigned scans=census_calls, imports=import_calls;
		int guest_percent=lworld_local_loading_status(1,false,&stage);
		assert(guest_percent==-1);
		assert(census_calls==scans && import_calls==imports);
		clear();
		lworld_draw_loading_status(false,640,240,0,"ignored","Host world",stage,guest_percent);
		assert(text_count==3 && !strchr(texts[2],'%') && quad_count==2);
		assert(!strcmp(texts[2],"Loading level..."));
		/* Host's latched 100 and subsequent rendering never leak to the guest. */
		missing=9;
		assert(lworld_local_loading_status(0,false,&stage)==100);
	}
	lworld_reset_presentation();
	gstate.world_switch_pending=true; /* this load's reset has arrived */
	missing=9; converting=true; import_percent=90;
	assert(lworld_local_loading_status(0,false,&stage)==90);
	assert(!strcmp(stage,"Converting world"));
	converting=false;
	assert(lworld_local_loading_status(0,false,&stage)==0);
	assert(!strcmp(stage,"Loading level")); /* fresh terrain-stage fill */
	missing=8;
	assert(lworld_local_loading_status(0,false,&stage)==11);
	assert(lworld_local_loading_status(1,false,&stage)==-1);
	missing=9;
	assert(lworld_local_loading_status(0,false,&stage)==11); /* own high-water */
	lworld_reset_presentation();
	gstate.world_switch_pending=false;
}

/* stale-99-1030 (round2 bug 06): a new load opened at "Loading level 99%"
 * because the census still saw the PREVIOUS scene's complete ring. Until
 * this load's world switch starts, the bar reads 0; then the real census. */
static void stale_ring_reads_zero(void) {
	const char* stage;
	lworld_reset_presentation();
	gstate.world_loaded=false; gstate.world_switch_pending=false;
	radius=1; missing=0; pending=0; /* the old scene: every column present */
	assert(lworld_local_loading_percent()==99); /* the raw census */
	for(int f=0;f<5;f++)
		assert(lworld_local_loading_status(0,false,&stage)==0 && "stale 99% shown");
	gstate.world_switch_pending=true; /* WORLD_RESET: the new world streams */
	missing=9;
	assert(lworld_local_loading_status(0,false,&stage)==0);
	missing=4;
	assert(lworld_local_loading_status(0,false,&stage)==55);
	gstate.world_switch_pending=false; /* stays this episode's census after */
	missing=2;
	assert(lworld_local_loading_status(0,false,&stage)==77);
	lworld_reset_presentation();
	missing=0;
	assert(lworld_local_loading_status(0,false,&stage)==0 && "the next load starts at 0 again");
	lworld_reset_presentation();
	/* Fallback 1: no switch flag, but the ring centre moves to the spawn. */
	gstate.world_switch_pending=false;
	gstate.players[0].camera.x=160; gstate.players[0].camera.z=-64;
	missing=0;
	assert(lworld_local_loading_status(0,false,&stage)==0);
	gstate.players[0].camera.x+=64; /* 4 chunks away: the new spawn */
	missing=4;
	assert(lworld_local_loading_status(0,false,&stage)==55 && "centre moved");
	gstate.players[0].camera.x-=64;
	lworld_reset_presentation();
	/* Fallback 2: neither signal -- bounded by the reading count. */
	missing=4;
	int got=0;
	for(int f=0;f<200&&!got;f++)
		got=lworld_local_loading_status(0,false,&stage);
	assert(got==55 && "the zeroing is bounded");
	lworld_reset_presentation();
	puts("PASS stale-99: the bar starts at 0 for every load until that load's world switch, a spawn move, or 120 readings");
}

static void split_name_and_tip_bounds(void) {
	char long_name[256];
	memset(long_name,'W',sizeof long_name-1); long_name[sizeof long_name-1]=0;
	for(int width=320;width<=854;width+=106) {
		viewport_width=width;
		clear();
		lworld_draw_loading_status(false,width,240,0,"ignored",long_name,
			"Loading level",99);
		assert(text_count==3 && strstr(texts[1],"..."));
		assert(text_x[1]>=16);
		assert(text_x[1]+gutil_font_width(texts[1],text_size[1])<=width-16);
		assert(text_y[0]+text_size[0]+2<=text_y[1]);
		assert(text_y[1]+text_size[1]+2<=quads[0].y);
		assert(quads[0].y+quads[0].h<=text_y[2]);
		/* loading_tips_layout rejects split tips above y=128. Include the
		 * bitmap shadow in the status bounds, not just the text baseline. */
		assert(text_y[2]+text_size[2]+2<=128);
	}
	viewport_width=640;
}
static void portal_titles(void) {
	/* Minecraft.usePortal: entering -1 / leaving for 0. Same approved art. */
	const char* headings[] = {"Entering the Nether", "Leaving the Nether"};
	for(int remote=0;remote<2;remote++) for(int split=0;split<2;split++)
		for(int direction=0;direction<2;direction++) {
			clear();
			lworld_draw_loading_status(remote,640,split?240:480,0,
				headings[direction],"World","Loading level",42);
			assert(!strcmp(texts[0],headings[direction]));
		}
}
int main(int argc,char** argv) {
	if(argc==5 && !strcmp(argv[1],"--preview")) {
		FILE* f=fopen(argv[3],"rb"); assert(f);
		assert(fread(preview_font_widths,1,256,f)==256); fclose(f);
		preview=true;
		texture_terrain.data=(void*)1;
		texture_terrain.width=texture_terrain.height=256;
		int direction=atoi(argv[4]);
		if(atoi(argv[2])==3) {
		 texture_nether_background=(struct tex_gfx){.data=(void*)1,.width=1024,.height=576};
		}
		lworld_draw_loading_backdrop(direction?PORTAL_OUT:PORTAL_IN,640,480,0,atoi(argv[2])==3?1:atoi(argv[2]));
		lworld_draw_loading_status(false,640,480,0,
			direction?"Leaving the Nether":"Entering the Nether","World","Loading level",42);
		return 0;
	}
	portal_scene_lifecycle();
	portal_backdrops();
	portal_titles();
	remote_unchanged();
	measured_progress();
	normal_load_gate();
	owner_progress_and_stage_switch();
	stale_ring_reads_zero();
	split_name_and_tip_bounds();
	puts("PASS singleplayer_loading: gate-aligned progress, monotone fill, name lookup, split bounds, unchanged remote");
	return 0;
}
