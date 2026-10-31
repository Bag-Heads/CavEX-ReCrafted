"""Record the production egg/snowball renderer against Beta RenderSnowball geometry."""
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "source/entity/entity_arrow.c").read_text(encoding="utf-8")
start = source.index("void entity_thrown_render(")
end = source.index("\n/* [740c583 RenderFireball", start)

code = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "entity/entity.h"
#include "game/camera.h"
#include "item/items.h"
#include "platform/gfx.h"
#include "platform/texture.h"
struct tex_gfx texture_items, texture_terrain;
static struct item snowball = {.render_data.item={.texture_x=14,.texture_y=0}};
static struct item egg = {.render_data.item={.texture_x=2,.texture_y=3}};
static float xyz_seen[12], uv_seen[8];
static unsigned draw_count, matrix_count, light_count, bind_count;
static uint16_t expected_item;
struct item* item_get(struct item_data* data) {
    assert(data->id == expected_item);
    return data->id == ITEM_EGG ? &egg : &snowball;
}
bool entity_get_block(struct entity* e, w_coord_t x, w_coord_t y, w_coord_t z,
                      struct block_data* blk) {
    assert(x == 11 && y == 2 && z == 3);
    blk->torch_light=15; blk->sky_light=0;
    return true;
}
float gfx_lookup_light(uint8_t packed) {assert(packed==240); return .75f;}
void gfx_matrix_modelview(mat4 matrix) {
    assert(matrix[0][0]==1 && matrix[1][1]==1 && matrix[2][2]==1);
    assert(matrix[3][0]==0 && matrix[3][1]==0 && matrix[3][2]==0);
    matrix_count++;
}
void gfx_bind_texture(struct tex_gfx* texture) {
    assert(texture == (bind_count==0 ? &texture_items : &texture_terrain));
    bind_count++;
}
void gfx_lighting(bool enable) {
    assert(enable == (light_count!=0));
    light_count++;
}
void gfx_draw_quads_flt(size_t n, const float* xyz, const uint8_t* rgba,
                        const float* uv) {
    assert(n==4 && draw_count++==0);
    memcpy(xyz_seen,xyz,sizeof xyz_seen);
    memcpy(uv_seen,uv,sizeof uv_seen);
    for(int i=0;i<4;i++) {
        assert(rgba[i*4]==191 && rgba[i*4+1]==191 && rgba[i*4+2]==191);
        assert(rgba[i*4+3]==255);
    }
}
FUNCTION
static bool near(float a,float b) {return fabsf(a-b)<0.0001f;}
int main(void) {
    struct entity e={0}; struct camera c={0};
    e.pos_old[0]=10; e.pos[0]=12;
    e.pos_old[1]=e.pos[1]=2; e.pos_old[2]=e.pos[2]=3;
    for(unsigned kind=0;kind<3;kind++) {
        expected_item = kind==1 ? ITEM_EGG : ITEM_SNOW_BALL;
        e.data.arrow.item_id=expected_item;
        glm_translate_make(c.view,(vec3){kind==1 ? -6 : -4,0,0});
        if(kind==2)
            glm_rotate_y(c.view, glm_rad(90.0f), c.view);
        struct entity before=e;
        draw_count=matrix_count=light_count=bind_count=0;
        entity_thrown_render(&e,&c,.5f);
        assert(memcmp(&e,&before,sizeof e)==0);
        assert(draw_count==1 && matrix_count==2 && light_count==2 && bind_count==2);
        vec3 center;
        glm_mat4_mulv3(c.view,(vec3){11,2,3},1.0f,center);
        if(kind==2)
            assert(!near(center[0],7.0f));
        /* [740c583 RenderSnowball.doRender]: 0.5 scale and local
           x[-0.5,0.5], y[-0.25,0.75], about interpolated position. */
        assert(near(xyz_seen[0],center[0]+.25f)
               && near(xyz_seen[3],center[0]-.25f));
        assert(near(xyz_seen[1],center[1]-.125f)
               && near(xyz_seen[7],center[1]+.375f));
        assert(near(xyz_seen[2],center[2])
               && near(xyz_seen[5],center[2]));
        /* GL_CW/GX_CULL_BACK: front face remains clockwise in camera space. */
        float cross=(xyz_seen[3]-xyz_seen[0])*(xyz_seen[7]-xyz_seen[1])
                   -(xyz_seen[4]-xyz_seen[1])*(xyz_seen[6]-xyz_seen[0]);
        assert(cross<0);
        float u0=kind==1 ? 2.0f/16 : 14.0f/16;
        float u1=kind==1 ? 3.0f/16 : 15.0f/16;
        float v0=kind==1 ? 3.0f/16 : 0.0f;
        float v1=kind==1 ? 4.0f/16 : 1.0f/16;
        assert(near(uv_seen[0],u1) && near(uv_seen[2],u0));
        assert(near(uv_seen[1],v1) && near(uv_seen[5],v0));
    }
    puts("PASS: Beta egg/snowball billboard geometry, UV, light and GX restore");
}
'''.replace("FUNCTION", source[start:end])

parser = argparse.ArgumentParser()
parser.add_argument("--cc", default="cc")
args = parser.parse_args()
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / "thrown_billboard.c"
    exe = c.with_suffix(".exe")
    c.write_text(code)
    subprocess.run([args.cc, "-DPLATFORM_PC", "-DCGLM_ALL_UNALIGNED",
                    "-UNDEBUG", "-I" + str(root / "source"),
                    "-I" + str(root / "third_party"), str(c), "-lm",
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
