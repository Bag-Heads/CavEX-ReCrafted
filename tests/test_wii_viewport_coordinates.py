"""Compile the real Wii viewport/scissor functions against a recording GX sink."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "source/platform/wii/gfx.c").read_text()


def function(name):
    start = source.index("void " + name + "(")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
struct mode { unsigned fbWidth, efbHeight; } mode, *screenMode=&mode;
static int logical_width;
static int gfx_width(void) { return logical_width; }
static int gfx_height(void) { return 480; }
static float vp_x,vp_y,vp_w,vp_h, viewport[6];
static uint32_t scissor[4];
static void GX_SetViewport(float x,float y,float w,float h,float n,float f) {
 viewport[0]=x;viewport[1]=y;viewport[2]=w;viewport[3]=h;viewport[4]=n;viewport[5]=f;
}
static void GX_SetScissor(uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
 scissor[0]=x;scissor[1]=y;scissor[2]=w;scissor[3]=h;
}
FUNCTIONS
int main(void) {
 for(int wide=0;wide<2;wide++) for(int pal=0;pal<2;pal++) {
  logical_width=wide?802:640;mode=(struct mode){640,pal?528:480};
  gfx_set_viewport(0,0,logical_width,480);
  assert(fabsf(viewport[2]-640)<.001F && fabsf(viewport[3]-mode.efbHeight)<.001F);
  for(unsigned boundary=1;boundary<480;boundary++) {
   gfx_set_viewport(0,boundary,logical_width,480-boundary);
   float y=viewport[1],h=viewport[3];gfx_depth_range(.1F,.8F);
   assert(viewport[1]==y && viewport[3]==h && viewport[2]>639.99F);
   assert(fabsf(y+h-mode.efbHeight)<.001F);
   gfx_scissor(true,0,0,logical_width,boundary);
   unsigned top_height=scissor[3]; assert(scissor[2]==640);
   gfx_scissor(true,0,boundary,logical_width,480-boundary);
   assert(scissor[0]==0 && scissor[1]==top_height && scissor[2]==640);
   assert(top_height+scissor[3]==mode.efbHeight);
  }
 }
}
'''.replace("FUNCTIONS", "\n".join(function(n) for n in
                                    ("gfx_set_viewport", "gfx_depth_range", "gfx_scissor")))
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / "viewport.c"
    exe = c.with_suffix(".exe")
    c.write_text(code)
    subprocess.run(["C:/msys64/ucrt64/bin/gcc.exe", "-UNDEBUG", "-Wall", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: real Wii viewport/depth/scissor, 4:3/widescreen and 480/528 EFB; every split boundary has exact coverage")
