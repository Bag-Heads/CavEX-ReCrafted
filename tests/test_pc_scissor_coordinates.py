"""Compile the real PC gfx_scissor against a recording GL sink.

CaveWii hands gfx_scissor a TOP-origin rectangle (the same convention the Wii
backend takes, y growing downward from the top of the screen), but glScissor
takes a BOTTOM-origin one. The contract pinned here is the flip:

    glScissor(x, gfx_height() - y - height, width, height)

so the menu rect (0, 94, 640, 218) must reach GL as (0, 168, 640, 218).
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "source/platform/pc/gfx.c").read_text()


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
#ifdef NDEBUG
#error "this test must be compiled with asserts enabled"
#endif
#define GL_SCISSOR_TEST 0x0C11
static int enabled, enable_calls, disable_calls, scissor_calls;
static int scissor[4];
static void glEnable(int cap) { assert(cap == GL_SCISSOR_TEST); enabled = 1; enable_calls++; }
static void glDisable(int cap) { assert(cap == GL_SCISSOR_TEST); enabled = 0; disable_calls++; }
static void glScissor(int x, int y, int w, int h) {
 scissor[0]=x;scissor[1]=y;scissor[2]=w;scissor[3]=h;scissor_calls++;
}
static int gfx_width(void) { return 640; }
static int gfx_height(void) { return 480; }
FUNCTIONS
static long checks;
static void expect(int cond) { checks++; assert(cond); }
int main(void) {
 /* disable forwards to glDisable and emits no rectangle */
 gfx_scissor(false, 0, 0, 640, 480);
 expect(enabled == 0 && disable_calls == 1 && scissor_calls == 0);

 /* full screen is its own mirror image under the flip */
 gfx_scissor(true, 0, 0, 640, 480);
 expect(enabled == 1 && enable_calls == 1 && scissor_calls == 1);
 expect(scissor[0] == 0 && scissor[1] == 0 && scissor[2] == 640 && scissor[3] == 480);

 /* every split boundary: the TOP view must land high in GL space, the
    BOTTOM view at y=0, and the two must tile the screen exactly once */
 for(int boundary = 1; boundary < 480; boundary++) {
  gfx_scissor(true, 0, 0, 640, boundary);
  int top_y = scissor[1], top_h = scissor[3];
  expect(scissor[0] == 0 && scissor[2] == 640 && top_h == boundary);
  expect(top_y == 480 - boundary);
  expect(top_y + top_h == 480);

  gfx_scissor(true, 0, boundary, 640, 480 - boundary);
  expect(scissor[0] == 0 && scissor[2] == 640 && scissor[3] == 480 - boundary);
  expect(scissor[1] == 0);
  expect(scissor[1] + scissor[3] == top_y);
 }

 /* the approved menu rect, spelled out */
 gfx_scissor(true, 0, 94, 640, 218);
 expect(scissor[0] == 0 && scissor[1] == 168 && scissor[2] == 640 && scissor[3] == 218);

 /* Check every intended case ran; -UNDEBUG and the compile-time guard above
    independently ensure that assertions are enabled. Six checks per split. */
 if(checks != 3 + 479L * 6 + 1) return 1;
 return 0;
}
'''.replace("FUNCTIONS", function("gfx_scissor"))

gcc = "C:/msys64/ucrt64/bin/gcc.exe"
if not Path(gcc).exists():
    gcc = shutil.which("gcc") or shutil.which("cc")
    assert gcc, "no C compiler found"

with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / "scissor.c"
    exe = c.with_suffix(".exe")
    c.write_text(code)
    subprocess.run([gcc, "-UNDEBUG", "-Wall", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: real PC gfx_scissor flips top-origin rects to GL bottom-origin; "
      "enable/disable, full screen, every split boundary, menu rect 0,94 -> 0,168")
