"""Exercise main's actual viewport setup/menu/cleanup across P2 departure."""
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
source = (root/'source/main.c').read_text(encoding='utf-8')
out = Path('tmp/jobs/player-visuals-0930')
out.mkdir(parents=True, exist_ok=True)
start = source.index('// Own the viewport even') if '// Own the viewport even' in source else source.index('if(view_count > 1 || view_height != gfx_height()) {')
setup = source[start:source.index('if(render_view && clin_world_payloads_enabled())', start)]
start = source.index('int menu_y = view_y + (view_height - 240)/2;')
menu = source[start:source.index('\n\t\t\t\t}', start)]
start = source.index('} /* viewports */') + len('} /* viewports */')
cleanup = source[start:source.index('for(unsigned p = 0; p < 2 && view_count == 2;', start)] + '}\n'
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
static int x,y,w,h;
static bool clipped;
static int gfx_width(void){return 802;}
static int gfx_height(void){return 480;}
static void gfx_set_viewport(int a,int b,int c,int d){x=a;y=b;w=c;h=d;}
static void gfx_scissor(bool on,int a,int b,int c,int d){clipped=on;(void)a;(void)b;(void)c;(void)d;}
static void gfx_mode_gui(void){}
static void gfx_mode_gui_viewport(int a,int b){(void)a;(void)b;}
static void draw(int view_count,int view_height,bool centered_menu) {
 int view_y=0;
 struct {int height;} view_rect[1]={{view_height}};
 SETUP
 assert(x==0&&y==0&&w==802&&h==view_height);
 if(centered_menu) { MENU }
 CLEANUP
 assert(x==0&&y==0&&w==802&&h==480&&!clipped);
}
int main(void) {
 gfx_set_viewport(0,0,802,480);
 for(int cycle=0;cycle<3;cycle++) {
  draw(2,240,true); /* joined, menu in host pane */
  draw(1,360,true); /* guest departs, host pane expands */
  draw(1,480,true); /* menu survives into solo */
  draw(1,480,false); /* close menu: gameplay must own the full viewport */
  gfx_set_viewport(0,120,802,240); /* dirty menu viewport entering a frame */
  draw(1,480,false);
 }
 puts("PASS: split -> solo menu -> solo gameplay restores full viewport and scissor, repeated three times");
}
'''.replace('SETUP', setup).replace('MENU', menu).replace('CLEANUP', cleanup)
c = out/'viewport-regression.c'
c.write_text(harness, encoding='utf-8')
exe = out/'viewport-regression.exe'
subprocess.run(['C:/msys64/ucrt64/bin/gcc.exe','-UNDEBUG',str(c),'-o',str(exe)],check=True)
subprocess.run([str(exe.resolve())],check=True)
