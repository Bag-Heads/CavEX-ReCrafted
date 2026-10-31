"""The production sky projection must retain centred celestial quads at split reach."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "source/graphics/gfx_util.c").read_text(encoding="utf-8")
start = source.index("\tmat4 sky_projection;", source.index("void gutil_sky_box_owned"))
end = source.index("\n\tgfx_alpha_test", start)
code = r'''
#include <assert.h>
#include <string.h>
#include "game/game_state.h"
struct game_state gstate;
static mat4 captured;
static void gfx_matrix_projection(mat4 p, bool perspective) {
 assert(perspective); glm_mat4_copy(p,captured);
}
static void sky(const struct camera* camera) { PRODUCTION }
static bool visible(mat4 projection, float x,float z) {
 vec4 p={x,0,z,1}, clip;
 glm_mat4_mulv(projection,p,clip);
 return clip[2]<=clip[3] && clip[2]>=-clip[3];
}
int main(void) {
 for(unsigned aspect=0;aspect<2;aspect++) {
  struct camera camera={0};
  gstate.config.render_distance=96;
  glm_perspective(glm_rad(70),aspect?3.34F:1.67F,.075F,96,camera.projection);
  mat4 original;glm_mat4_copy(camera.projection,original);
  assert(!visible(original,0,-100)); /* Centred sun: reproduce the defect. */
  assert(visible(original,60,-80)); /* Same distance, off-axis: visible. */
  sky(&camera);
  assert(visible(captured,0,-100));
  assert(visible(captured,30,-109));
  assert(memcmp(original,camera.projection,sizeof original)==0);
  assert(captured[0][0]==original[0][0] && captured[1][1]==original[1][1]);
  gstate.config.render_distance=544;
  glm_perspective(glm_rad(70),1.67F,.075F,544,camera.projection);
  sky(&camera);
  assert(memcmp(captured,camera.projection,sizeof captured)==0);
 }
}
'''.replace("PRODUCTION",source[start:end])
assert "gfx_matrix_projection((vec4*)camera->projection, true);" in source[end:source.index("void gutil_block_selection",end)]
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / "sky.c"
    c.write_text(code)
    exe = c.with_suffix(".exe")
    subprocess.run(["C:/msys64/ucrt64/bin/gcc.exe", "-DPLATFORM_PC", "-DCGLM_ALL_UNALIGNED",
                    "-UNDEBUG", "-I"+str(root/"source"), "-I"+str(root/"third_party"),
                    str(c), "-o", str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("PASS: centred sky clipping reproduced at 96 blocks; sky-only extension preserves FOV and terrain projection")
