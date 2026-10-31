"""Record real model draw inputs; pin opt-off bytes to a frozen pre-W01 source.

GFX/displaylist calls are recording doubles, not a GPU/readability test. The
model code is compiled intact. Baseline source is retained under support/.
"""
import argparse
import ctypes
import filecmp
import os
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--cc', default='cc')
parser.add_argument('--candidate-source', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    ctypes.windll.kernel32.SetErrorMode(3)

code = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/displaylist.h"
#include "platform/gfx.h"
#include "platform/texture.h"
#include "graphics/render_entity.h"
#include "graphics/render_item.h"
#include "item/items.h"
#include "entity/mob_windup_cue.h"
#include "entity/entity.h"
#include "entity/combat.h"
#include "game/game_state.h"
#include "graphics/render_entity_anchor.h"
#include "light_brightness.h"
TEXTURES
struct item* items[ITEMS_MAX];
struct game_state gstate;
static mat4 matrices[64];
static unsigned nmat;
static unsigned char tape[1000000];
static size_t ntape;
static mat4 current;
static void record(const void* p, size_t n) {
    assert(ntape+n <= sizeof tape); memcpy(tape+ntape,p,n); ntape+=n;
}
static void tag(unsigned t) { record(&t,sizeof t); }
void displaylist_init(struct displaylist* l,size_t n,size_t size) {(void)l;(void)n;(void)size;}
void displaylist_reset(struct displaylist* l) {(void)l;tag(1);}
void displaylist_pos(struct displaylist* l,int16_t x,int16_t y,int16_t z) {
    (void)l;int16_t v[3]={x,y,z};tag(2);record(v,sizeof v);
}
void displaylist_color(struct displaylist* l,uint8_t i) {(void)l;tag(3);record(&i,1);}
void displaylist_texcoord(struct displaylist* l,uint8_t u,uint8_t v) {
    (void)l;uint8_t uv[2]={u,v};tag(4);record(uv,2);
}
void displaylist_render_immediate(struct displaylist* l,uint16_t n) {(void)l;tag(5);record(&n,2);}
void gfx_matrix_modelview(mat4 mv) {
    assert(nmat<64);glm_mat4_copy(mv,matrices[nmat++]);glm_mat4_copy(mv,current);
    tag(6);record(mv,sizeof(mat4));
}
void gfx_bind_texture(struct tex_gfx* tex) {
    unsigned id=0;
TEXTURE_IDS
    tag(7);record(&id,sizeof id);
}
void gfx_lighting(bool b) {tag(8);record(&b,sizeof b);}
void gfx_alpha_test(bool b) {tag(9);record(&b,sizeof b);}
void gfx_blending(enum gfx_blend b) {tag(10);record(&b,sizeof b);}
void gfx_cull_func(enum cull_func f) {tag(11);record(&f,sizeof f);}
void gfx_entity_modulate(float r,float g,float b) {float v[3]={r,g,b};tag(12);record(v,sizeof v);}
void gfx_entity_tint(float r,float g,float b,float a) {float v[4]={r,g,b,a};tag(17);record(v,sizeof v);}
void gfx_draw_quads(size_t n,const int16_t* p,const uint8_t* c,const uint16_t* uv) {
    tag(13);record(&n,sizeof n);record(p,n*6);record(c,n*4);record(uv,n*4);
}
void gfx_draw_quads_flt(size_t n,const float* p,const uint8_t* c,const float* uv) {
    tag(14);record(&n,sizeof n);record(p,n*12);record(c,n*4);record(uv,n*8);
}
void render_item_flat(struct item* item,struct item_data* stack,mat4 mv,bool ui,enum render_item_env env) {
    (void)item;(void)ui;(void)env;tag(15);record(&stack->id,2);record(mv,sizeof(mat4));
}
void render_item_update_light(uint8_t l) {tag(16);record(&l,1);}
bool entity_get_block(struct entity* e,w_coord_t x,w_coord_t y,w_coord_t z,struct block_data* b) {
    (void)e;(void)x;(void)y;(void)z;*b=(struct block_data){.torch_light=10,.sky_light=12};return true;
}
void entity_shadow(struct entity* e,struct AABB* a,mat4 view) {(void)e;(void)a;(void)view;}
int underwater_overlay_sample_y(float p,float h,float o) {(void)h;(void)o;return (int)p;}
float light_brightness_current(uint8_t light) {(void)light;return 1;}
#ifdef CANDIDATE
DISPATCH_FUNCTION
#endif
static void reset(void) {nmat=0;ntape=0;}
static void draw(int kind,mat4 view,int frame,float degrees,bool explicit_cue) {
#ifdef CANDIDATE
    if(explicit_cue) {
        switch(kind) {
        case 0:render_entity_zombie_cue(view,21,-17,frame,degrees);break;
        case 1:render_entity_pigzombie_cue(view,21,-17,frame,degrees);break;
        case 2:render_entity_spider_cue(view,21,-17,frame,degrees);break;
        case 3:render_entity_wolf_cue(view,21,-17,frame,0,8,false,0,0,0,0,.25F,degrees);break;
        }
        return;
    }
#else
    (void)degrees;(void)explicit_cue;
#endif
    switch(kind) {
    case 0:render_entity_zombie(view,21,-17,frame);break;
    case 1:render_entity_pigzombie(view,21,-17,frame);break;
    case 2:render_entity_spider(view,21,-17,frame);break;
    case 3:render_entity_wolf(view,21,-17,frame,0,8,false,0,0,0,0,.25F);break;
    }
}
static void pose_check(int kind,mat4 view,int frame,float degrees) {
#ifdef CANDIDATE
    reset();draw(kind,view,frame,0,true);
    mat4 idle[64];memcpy(idle,matrices,sizeof idle);unsigned count=nmat;
    reset();draw(kind,view,frame,degrees,true);assert(nmat==count);
    /* Matrix call order: bipeds head/body/legs/arms, spider head/neck/body/8 legs,
     * wolf head/ear/ear/snout then body/mane/legs/tail. Final identity also stays. */
    unsigned changes=0;
    for(unsigned i=0;i<count;i++) {
        bool want=kind<=1?(i==4||i==5)
            :kind==2?(i==9||i==10):i<4;
        bool changed=memcmp(idle[i],matrices[i],sizeof(mat4))!=0;
        assert(changed==want);changes+=changed;
    }
    assert(changes==(kind<=2?2:4));
    /* Independent local-space rotation delta, after inverse baseline: prove
     * signed angle and that the selected parts really move upward. */
    unsigned selected=kind<=1?4:kind==2?9:0;
    mat4 inv,delta;glm_mat4_inv(idle[selected],inv);
    glm_mat4_mul(inv,matrices[selected],delta);
    float rad=glm_rad(degrees);
    if(kind==2) {
        assert(fabsf(delta[0][0]+delta[1][1]+delta[2][2]-(1+2*cosf(rad)))<.00001F);
        vec3 tip={-15.0F/16.0F,0,0},old,new;
        glm_mat4_mulv3(idle[selected],tip,1,old);
        glm_mat4_mulv3(matrices[selected],tip,1,new);
        assert(new[1]>old[1]);
    } else {
        assert(fabsf(delta[1][1]-cosf(rad))<.00001F);
        assert(fabsf(delta[1][2]-sinf(rad))<.00001F);
    }
    /* Return to idle after an active draw; no global pose leakage. */
    reset();draw(kind,view,frame,0,true);
    assert(memcmp(idle,matrices,count*sizeof(mat4))==0);
#else
    (void)kind;(void)view;(void)frame;(void)degrees;
#endif
}
static void dispatch_check(mat4 views[2]) {
#ifdef CANDIDATE
    gstate.config_user.root=json_value_init_object();assert(gstate.config_user.root);
    JSON_Object* config=json_value_get_object(gstate.config_user.root);
    const int species[]={MONSTER_ZOMBIE,MONSTER_PIG_ZOMBIE,MONSTER_SPIDER,MONSTER_WOLF};
    for(int k=0;k<4;k++)for(int vp=0;vp<2;vp++) {
        struct entity e={.type=ENTITY_MONSTER,.health=20,.height=1.8F};
        e.data.monster.id=species[k];e.data.monster.windup_cue=(struct mob_windup_cue){1,2};
        struct entity before=e;
        assert(json_object_dotset_boolean(config,"ui.mob_attack_windup_cue",0)==JSONSuccess);
        reset();entity_render(&e,views[vp],.5F);
        static unsigned char off[1000000];size_t n=ntape;memcpy(off,tape,n);
        assert(memcmp(&e,&before,sizeof e)==0);
        assert(json_object_dotset_boolean(config,"ui.mob_attack_windup_cue",1)==JSONSuccess);
        reset();entity_render(&e,views[vp],.5F);
        assert(ntape==n&&memcmp(off,tape,n)!=0);
        static unsigned char active[1000000];memcpy(active,tape,n);
        reset();entity_render(&e,views[1-vp],.5F);
        reset();entity_render(&e,views[vp],.5F);
        assert(ntape==n&&memcmp(active,tape,n)==0);
        assert(memcmp(&e,&before,sizeof e)==0);
        e.data.monster.windup_cue.version=0;
        reset();entity_render(&e,views[vp],.5F);
        assert(ntape==n&&memcmp(off,tape,n)==0);
    }
    config_destroy(&gstate.config_user);
#else
    (void)views;
#endif
}
int main(int argc,char** argv) {
    assert(argc==2);FILE* out=fopen(argv[1],"wb");assert(out);
    struct item sword={0};sword.tool.type=TOOL_TYPE_SWORD;
    sword.renderItem=render_item_flat;items[ITEM_GOLD_SWORD]=&sword;
    INIT_TEXTURES
    render_entity_init();render_entity_update_light(172);
    mat4 views[2];glm_mat4_identity(views[0]);glm_translate_make(views[1],(vec3){7,-3,9});
    for(int kind=0;kind<4;kind++)for(int frame=0;frame<60;frame++)
        for(int amt=0;amt<3;amt++)for(int viewport=0;viewport<2;viewport++) {
            render_entity_set_limb_swing(amt*.5F);
            reset();draw(kind,views[viewport],frame,0,false);
            static unsigned char legacy[1000000];size_t size=ntape;memcpy(legacy,tape,size);
            reset();draw(kind,views[viewport],frame,0,true);
            assert(size==ntape&&memcmp(legacy,tape,size)==0);
            assert(fwrite(tape,1,ntape,out)==ntape);
            float full=kind<2?35:kind==2?25:20;
            pose_check(kind,views[viewport],frame,full*.5F);
            pose_check(kind,views[viewport],frame,full);
            /* Draw viewport 2 then 1: advancing animation during either draw
             * would change the recorded matrices on the second pass. */
            reset();draw(kind,views[1-viewport],frame,full,true);
            reset();draw(kind,views[viewport],frame,0,true);
            assert(size==ntape&&memcmp(legacy,tape,size)==0);
        }
    dispatch_check(views);
    assert(fclose(out)==0);puts("PASS real model matrices/vertices/UV/state, signed half/full pose, real entity dispatch, two viewport order");
}
'''
texture_header=(root/'source/platform/texture.h').read_text()
textures=re.findall(r'extern struct tex_gfx (texture_\w+);', texture_header)
code=code.replace('TEXTURES','\n'.join(f'struct tex_gfx {x};' for x in textures),1)
code=code.replace('TEXTURE_IDS','\n'.join(f'    if(tex==&{x})id={i};' for i,x in enumerate(textures,1)))
code=code.replace('INIT_TEXTURES','\n'.join(f'    {x}.data=(void*)1;' for x in textures))
monster=(root/'source/entity/entity_monster.c').read_text(encoding='utf-8')
begin=monster.index('static void entity_render(')
end=monster.index('\n\n\n// Raycast',begin)
code=code.replace('DISPATCH_FUNCTION',monster[begin:end])

with tempfile.TemporaryDirectory(prefix='cw-windup-render-') as temp:
    d=Path(temp)
    main=d/'main.c';main.write_text(code)
    for name, source in [('baseline',root/'tests/support/windup_render_baseline.c'),
                         ('candidate',args.candidate_source or root/'source/graphics/render_entity.c')]:
        exe=d/f'{name}.exe'
        cmd=[args.cc,'-O2','-std=c99','-DPLATFORM_PC','-UNDEBUG','-ffp-contract=off',
             f'-I{root/"source"}',
             f'-I{root/"source/graphics"}',f'-I{root/"third_party"}',
             str(main),str(source),str(root/'source/entity/monsters_object.c'),
             str(root/'source/config.c'),str(root/'source/parson/parson.c'),
             str(root/'source/block/aabb.c'),
             '-lm','-o',str(exe)]
        if name=='candidate':cmd.insert(1,'-DCANDIDATE')
        subprocess.run(cmd,check=True)
        subprocess.run([str(exe),str(d/f'{name}.bin')],check=True)
    assert filecmp.cmp(d/'baseline.bin',d/'candidate.bin',shallow=False), 'toggle-off draw inputs changed'
print('PASS toggle-off byte-exact against pre-W01 renderer cd687b6a8')
