"""Check actual biped draw transforms against Beta's Y-down -> Y-up mapping.

Compile the production biped draw through its six body pieces, replacing only
GPU submission with matrix capture. The raw-pose golden test alone cannot catch
coordinate conversion errors at the renderer boundary.
"""
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
out = Path('tmp/jobs/player-visuals-0930')
out.mkdir(parents=True, exist_ok=True)
source = (root / 'source/graphics/render_entity.c').read_text(encoding='utf-8')
start = source.index('static void render_entity_biped(')
end = source.index('    /* --- Held item', start)
draw = source[start:end] + '\n}\n'
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "graphics/render_entity_anchor.h"
#include "graphics/render_biped_swing_pose.h"
typedef struct { uint8_t u,v,w,h; } UVRect;
struct tex_gfx { void *data; } texture_creeper;
static int dl, piece;
static float limb_swing_amount;
static mat4 current, matrices[6];
static void gfx_bind_texture(struct tex_gfx *p) {(void)p;}
static void gfx_matrix_modelview(mat4 m) { glm_mat4_copy(m,current); }
static void displaylist_reset(int *p) {(void)p;}
static void displaylist_render_immediate(int *p,int n) {(void)p;(void)n;}
static void render_entity_create_cube(int x,int y,int z,int w,int h,int d,
 const UVRect *uv,const int *rot) {
 (void)x;(void)y;(void)z;(void)w;(void)h;(void)d;(void)uv;(void)rot;
 assert(piece<6);glm_mat4_copy(current,matrices[piece++]);
}
DRAW
static void close3(vec3 got,vec3 want) {
 for(int i=0;i<3;i++) if(fabsf(got[i]-want[i])>1e-5f) {
  fprintf(stderr,"axis %d: got %f, expected %f\n",i,got[i],want[i]);assert(0);
 }
}
static void point(int part,vec3 local,vec3 expected,mat4 body) {
 vec3 got,want;glm_mat4_mulv3(matrices[part],local,1,got);
 glm_mat4_mulv3(body,expected,1,want);close3(got,want);
}
int main(void) {
 /* Reference: Beta 1.7.3 ModelBiped.setRotationAngles isSneak block;
  * ModelRenderer.render applies T(rotationPoint) Rz Ry Rx to local boxes.
  * CaveWii maps each Beta point (x,y,z) to (x+8,24-y,z+8) pixels.
  * Check actual rendered points at every facing, plus pitch, repeatedly. */
 for(int cycle=0;cycle<2;cycle++) for(int yaw=-180;yaw<=180;yaw+=45)
 for(int pitch=-45;pitch<=45;pitch+=45) for(int sneak=0;sneak<2;sneak++) {
  mat4 view=GLM_MAT4_IDENTITY_INIT,body;
  render_entity_body_mv(view,(vec3){.5f,1.125f,.5f},yaw,body);
  piece=0;
  render_entity_biped(view,yaw,yaw,pitch,0,&texture_creeper,false,false,
                      0,sneak,0,false,NULL);
  assert(piece==6);
  float a=sneak?.5f:0, arm=sneak?.4f:0;
  /* Torso's lower centre: reflected Beta R_x(+0.5)*(0,12,0). */
  point(1,(vec3){.5f,.75f,.5f},
        (vec3){.5f,(24-12*cosf(a))/16,(8+12*sinf(a))/16},body);
  /* Neck anchor moves DOWN one pixel, even with zero head pitch. */
  point(0,(vec3){.5f,1.5f,.5f},
        (vec3){.5f,(24-(sneak?1:0))/16.0f,.5f},body);
  for(int leg=0;leg<2;leg++)
   point(2+leg,(vec3){.125f,.75f,.125f},
         (vec3){(6+4*leg)/16.0f,(sneak?15:12)/16.0f,
                (sneak?12:8)/16.0f},body);
  /* Existing arm box/pivot, with Beta's positive sneak pitch reflected.
   * Its bottom centre must move backward (+Z), not forward through chest. */
  for(int arm_i=0;arm_i<2;arm_i++)
   point(4+arm_i,(vec3){.125f,0,.125f},
         (vec3){(2+12*arm_i)/16.0f,(24-12*cosf(arm))/16,
                (8+12*sinf(arm))/16},body);
 }
 puts("PASS: actual biped draws bend forward, lift hips and lower head; all facings/pitches and repeat cycles");
}
'''.replace('DRAW', draw)
c = out / 'biped-render-regression.c'
c.write_text(harness, encoding='utf-8')
exe = out / 'biped-render-regression.exe'
subprocess.run(['C:/msys64/ucrt64/bin/gcc.exe', '-UNDEBUG', '-DCGLM_ALL_UNALIGNED',
                '-I'+str(root/'source'), str(c), '-o', str(exe), '-lm'], check=True)
subprocess.run([str(exe.resolve())], check=True)
