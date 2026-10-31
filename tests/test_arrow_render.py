"""Record the real arrow draw path against Beta RenderArrow's six-quad mesh.

Local true arrows also record Beta previous/current rotation and Z-axis embed
shake before the X45 roll. Missing/remote samples retain the explicit fallback.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--cc", default="cc")
parser.add_argument("--source-root", type=Path)
parser.add_argument("--include-root", type=Path)
parser.add_argument("--out", type=Path)
parser.add_argument("--entity-header", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source_root = args.source_root or root
include_root = args.include_root or root
source = (source_root / "source/entity/entity_arrow.c").read_text(encoding="utf-8")
start = source.index("static void entity_render(")
end = source.index("\n/* [740c583 RenderSnowball", start)
dispatch = (include_root / "source/entity/entity.c").read_text(encoding="utf-8")
ds = dispatch.index("void entities_client_render(")
de = dispatch.index("\nbool entity_aabb_intersect_ray(", ds)

code = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "entity/entity.h"
#include "game/camera.h"
#include "graphics/render_item.h"
#include "graphics/render_entity_fire.h"
#include "platform/gfx.h"
#include "platform/texture.h"
#include "platform/watchdog.h"
#include "worldgen/beta_math.h"
struct tex_gfx texture_items, texture_terrain, texture_arrow;
volatile uint32_t g_wd_draw_site, g_wd_draw_id, g_wd_draw_aux;
static unsigned draws, fallback, thrown, fireballs, flames, binds, lighting, matrices;
static float seen_xyz[72], seen_uv[48];
static mat4 seen_mv;
static uint16_t expected_item;
static struct item icon;
static bool blocks_available=true;
static int expected_x=11;
static struct tex_gfx* current_texture;
static void fallback_draw(struct item* it, struct item_data* data, mat4 mv,
                          bool ui, enum render_item_env env) {
    assert(data->id==expected_item && !ui && env==R_ITEM_ENV_ENTITY);
    fallback++;
}
struct item* item_get(struct item_data* data) {
    assert(data->id==expected_item); icon.renderItem=fallback_draw; return &icon;
}
bool entity_get_block(struct entity* e, w_coord_t x, w_coord_t y, w_coord_t z,
                      struct block_data* blk) {
    assert(x==expected_x && y==2 && z==3);
    if(!blocks_available)return false;
    blk->torch_light=11; blk->sky_light=2; return true;
}
void render_item_update_light(uint8_t light) { assert(light==178); }
float gfx_lookup_light(uint8_t light) {
    assert(light==(blocks_available?178:0));return .75f;
}
void gfx_matrix_modelview(mat4 mv) {
    if(matrices++==0)glm_mat4_copy(mv,seen_mv);
    else assert(memcmp(mv,GLM_MAT4_IDENTITY,sizeof(mat4))==0);
}
void gfx_bind_texture(struct tex_gfx* texture) {
    assert(texture==(binds++==0?&texture_arrow:&texture_terrain));
    current_texture=texture;
}
void gfx_lighting(bool enable) {assert(enable==(lighting++!=0));}
void gfx_draw_quads_flt(size_t count,const float* xyz,const uint8_t* rgba,
                        const float* uv) {
    assert(count==24 && draws++==0 && current_texture==&texture_arrow);
    memcpy(seen_xyz,xyz,sizeof seen_xyz);memcpy(seen_uv,uv,sizeof seen_uv);
    for(unsigned k=0;k<count;k++) {
        assert(rgba[k*4]==191 && rgba[k*4+1]==191 && rgba[k*4+2]==191);
        assert(rgba[k*4+3]==255);
    }
}
void entity_thrown_render(struct entity* e,struct camera* c,float td) {
    assert(e->data.arrow.item_id==ITEM_EGG || e->data.arrow.item_id==ITEM_SNOW_BALL);
    thrown++;
}
void entity_fireball_render(struct entity* e,struct camera* c,float td) {fireballs++;}
void render_entity_fire_draw(const struct entity* e,const struct camera* c,float td) {flames++;}
FUNCTION
DISPATCH
static bool near(float a,float b) {return fabsf(a-b)<0.00001f;}
static void reset(void) {draws=fallback=thrown=fireballs=flames=binds=lighting=matrices=0;}
/* Independent Java/OpenGL order; compare world/camera-space results after
 * reversing each vertex/UV pair for CaveWii's clockwise front faces. */
static void mesh_check(struct entity* e,struct camera* c,float td) {
    assert(draws==1 && fallback==0 && binds==2 && lighting==2 && matrices==2);
    static const float beta_caps[24]={
        -7,-2,-2, -7,-2,2, -7,2,2, -7,2,-2,
        -7,2,-2, -7,2,2, -7,-2,2, -7,-2,-2
    };
    static const float beta_face[12]={-8,-2,0, 8,-2,0, 8,2,0, -8,2,0};
    static const float cap_uv[8]={0,5.0f/32, 5.0f/32,5.0f/32,
                                5.0f/32,10.0f/32, 0,10.0f/32};
    static const float face_uv[8]={0,0, .5f,0, .5f,5.0f/32, 0,5.0f/32};
    vec3 pos,delta;glm_vec3_lerp(e->pos_old,e->pos,td,pos);
    glm_vec3_sub(e->pos,e->pos_old,delta);
    if(glm_vec3_norm2(delta)<glm_pow2(.0001f))glm_vec3_copy(e->vel,delta);
    float yaw=atan2f(delta[0],delta[2]);
    float horizontal=sqrtf(delta[0]*delta[0]+delta[2]*delta[2]);
    float pitch=atan2f(delta[1],horizontal>.001f?horizontal:.001f);
    if(e->data.arrow.visual.valid) {
        yaw=glm_rad(e->data.arrow.visual.previous[0]
            +(e->data.arrow.visual.rotation[0]-e->data.arrow.visual.previous[0])*td);
        pitch=glm_rad(e->data.arrow.visual.previous[1]
            +(e->data.arrow.visual.rotation[1]-e->data.arrow.visual.previous[1])*td);
    }
    mat4 expected;glm_translate_make(expected,pos);
    glm_rotate_y(expected,yaw-glm_rad(90),expected);
    glm_rotate_z(expected,pitch,expected);
    float shake=(float)e->data.arrow.visual.shake_ticks-td;
    if(e->data.arrow.visual.valid && shake>0)
        glm_rotate_z(expected,glm_rad(-mh_sin(shake*3)*shake),expected);
    glm_rotate_x(expected,glm_rad(45),expected);
    glm_scale_uni(expected,.05625f);glm_translate(expected,(vec3){-4,0,0});
    glm_mat4_mul(c->view,expected,expected);
    for(unsigned q=0;q<6;q++)for(unsigned k=0;k<4;k++) {
        unsigned beta=3-k;vec3 original;
        if(q<2)memcpy(original,beta_caps+q*12+beta*3,sizeof original);
        else {
            memcpy(original,beta_face+beta*3,sizeof original);
            /* Beta rotates90degrees BEFORE each of its four shaft draws. */
            unsigned quarter=q-1;float y=original[1];
            original[1]=quarter==2?-y:quarter==4?y:0;
            original[2]=quarter==1?y:quarter==3?-y:0;
        }
        vec3 want,got;glm_mat4_mulv3(expected,original,1,want);
        glm_mat4_mulv3(seen_mv,seen_xyz+q*12+k*3,1,got);
        for(unsigned axis=0;axis<3;axis++)assert(near(got[axis],want[axis]));
        const float* uv=q<2?cap_uv:face_uv;
        assert(seen_uv[q*8+k*2]==uv[beta*2]);
        assert(seen_uv[q*8+k*2+1]==uv[beta*2+1]);
    }
    assert(draws==1 && fallback==0 && binds==2 && lighting==2 && matrices==2);
    assert(current_texture==&texture_terrain);
}
int main(void) {
    struct entity e={.id=7,.type=ENTITY_ARROW,.render=entity_render};
    struct camera c={0};glm_mat4_identity(c.view);
    e.pos_old[0]=10;e.pos[0]=12;e.pos_old[1]=e.pos[1]=2;
    e.pos_old[2]=e.pos[2]=3;e.vel[0]=2;e.vel[1]=1;e.vel[2]=3;
    texture_arrow.data=(uint8_t*)"test";expected_item=ITEM_ARROW;
    for(unsigned run=0;run<4;run++) {
        reset();e.data.arrow.item_id=run==0?0:ITEM_ARROW;
        if(run==1)glm_rotate_y(c.view,glm_rad(37),c.view);
        if(run==2)glm_vec3_copy(e.pos,e.pos_old);
        if(run==3)blocks_available=false;
        expected_x=run<2?11:12;
        struct entity before=e;entity_render(&e,c.view,run==2?0:0.5f);
        assert(memcmp(&e,&before,sizeof e)==0);
        mesh_check(&e,&c,run==2?0:0.5f);
    }
    /* Independent orientation sample disagrees with position/velocity:
     * interpolation and embed shake must use the authoritative rotation. */
    blocks_available=true;e.pos_old[0]=10;expected_x=11;
    e.data.arrow.visual=(struct arrow_visual_state){
        .previous={170,25},.rotation={190,-15},.shake_ticks=7,.valid=true};
    for(unsigned camera=0;camera<2;camera++)for(unsigned part=0;part<3;part++) {
        if(camera==1 && part==0)glm_rotate_x(c.view,glm_rad(29),c.view);
        float td=part*.5f;expected_x=10+part;reset();
        struct entity before=e;entity_render(&e,c.view,td);
        assert(memcmp(&e,&before,sizeof e)==0);mesh_check(&e,&c,td);
    }
    reset();expected_x=(int)floorf(e.pos_old[0]+(e.pos[0]-e.pos_old[0])*.5f);
    e.data.arrow.visual.previous[0]=e.data.arrow.visual.previous[1]=0;
    entity_render(&e,c.view,.5f);
    assert(draws==0 && fallback==0 && binds==0 && lighting==0 && matrices==0);
    e.data.arrow.visual.valid=false;expected_x=11;
    texture_arrow.data=NULL;reset();entity_render(&e,c.view,.5f);
    assert(fallback==1 && draws==0 && binds==0 && lighting==0 && matrices==0);
    /* Production dispatch must keep each other projectile on its own route. */
    texture_arrow.data=(uint8_t*)"test";
    dict_entity_t dict;dict_entity_init(dict);dict_entity_set_at(dict,e.id,&e);
    uint16_t items[]={ITEM_ARROW,ITEM_EGG,ITEM_SNOW_BALL,ITEM_FISHING_ROD,ITEM_SNOW_BALL};
    for(unsigned k=0;k<5;k++) {
        reset();expected_item=items[k];e.data.arrow.item_id=items[k];
        e.data.arrow.fireball=k==4;struct entity before=e;
        entities_client_render(dict,&c,.5f);
        assert(draws==(k==0));assert(thrown==(k==1||k==2));
        assert(fallback==(k==3));assert(fireballs==(k==4));assert(flames==(k==4));
        assert(memcmp(&e,&before,sizeof e)==0);
    }
    dict_entity_clear(dict);
    puts("PASS: actual Beta arrow mesh/UV/current direction/light/state/fallback/dispatch");
}
'''.replace("FUNCTION", source[start:end]).replace("DISPATCH", dispatch[ds:de])

def run(directory):
    directory.mkdir(parents=True, exist_ok=True)
    c = directory / "arrow_render.c"
    exe = directory / "arrow_render.exe"
    c.write_text(code, encoding="utf-8")
    subprocess.run([args.cc, "-DPLATFORM_PC", "-DCGLM_ALL_UNALIGNED", "-UNDEBUG",
                    "-I"+str(include_root/"source"), "-I"+str(include_root/"third_party"),
                    *([] if not args.entity_header else ["-iquote",str(include_root/"source/entity"),"-iquote",str(include_root/"source"),"-include",str(args.entity_header)]),
                    str(c), str(include_root/"source/worldgen/beta_math.c"), "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

if args.out:
    run(args.out)
else:
    with tempfile.TemporaryDirectory() as temp:
        run(Path(temp))
