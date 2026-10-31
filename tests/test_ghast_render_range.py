"""Run the real entity draw loop with renderer recorders, without a graphics context."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "source/entity/entity.c").read_text(encoding="utf-8")
start = source.index("void entities_client_render(")
end = source.index("\nbool entity_aabb_intersect_ray(", start)
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "entity/entity.h"
#include "game/camera.h"
#include "platform/watchdog.h"
#include "graphics/render_entity_fire.h"
static unsigned draws, fires;
volatile uint32_t g_wd_draw_site, g_wd_draw_id, g_wd_draw_aux;
void render_entity_fire_draw(const struct entity* e, const struct camera* c, float td) {
    fires++;
}
void entity_fireball_render(struct entity* e, struct camera* c, float td) {
    assert(0 && "ordinary ghast draw must not take the fireball path");
}
void entity_thrown_render(struct entity* e, struct camera* c, float td) {
    assert(0 && "ordinary ghast draw must not take the thrown icon path");
}
static void draw(struct entity* e, mat4 view, float td) {
    assert(td == .5F); draws++;
}
FUNCTION
static void check(dict_entity_t dict, struct entity* e, struct camera* c,
                  float x, float y, float z, bool visible) {
    e->pos[0]=x; e->pos[1]=y; e->pos[2]=z;
    struct entity before=*e;
    draws=fires=0;
    entities_client_render(dict,c,.5F);
    assert(draws == (unsigned)visible);
    assert(fires == (unsigned)(visible && e->on_fire));
    assert(memcmp(e,&before,sizeof before)==0);
}
int main(void) {
    dict_entity_t dict; dict_entity_init(dict);
    struct entity e={.id=7,.type=ENTITY_MONSTER,.render=draw};
    struct camera c={0};
    dict_entity_set_at(dict,e.id,&e);
    /* Beta EntityGhast: 4x4 bounds; Entity.isInRangeToRenderDist:
       average edge length * 64 = 256 blocks. */
    for(unsigned kind=0;kind<2;kind++) {
        e.type=ENTITY_MONSTER;
        e.data.monster.id=MONSTER_GHAST;
        e.on_fire=kind;
        check(dict,&e,&c,31,0,0,true);
        check(dict,&e,&c,32,0,0,true);
        check(dict,&e,&c,48,0,0,true);
        check(dict,&e,&c,63.99F,0,0,true);
        check(dict,&e,&c,80,0,0,true);
        check(dict,&e,&c,0,64,0,true);
        check(dict,&e,&c,40,30,40,true);
        check(dict,&e,&c,255.9F,0,0,true);
        check(dict,&e,&c,256,0,0,false);
        check(dict,&e,&c,200,0,200,false);
        /* Each split-screen camera computes its own distance. */
        c.x=300;
        check(dict,&e,&c,48,0,0,true);
        check(dict,&e,&c,0,0,0,false);
        c.x=0;
    }
    e.on_fire=false; e.type=ENTITY_MONSTER; e.data.monster.id=MONSTER_ZOMBIE;
    check(dict,&e,&c,31.9F,0,0,true);
    check(dict,&e,&c,32,0,0,false);
    e.type=ENTITY_REMOTE_PLAYER;
    check(dict,&e,&c,63.9F,0,0,true);
    check(dict,&e,&c,64,0,0,false);
    e.render=NULL;
    check(dict,&e,&c,0,0,0,false);
    dict_entity_clear(dict);
    puts("PASS: ghast draw range, boundaries, camera ownership, unchanged ordinary entities");
}
'''.replace("FUNCTION", source[start:end])
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / "ghast_render.c"
    exe = c.with_suffix(".exe")
    c.write_text(code)
    subprocess.run(["C:/msys64/ucrt64/bin/gcc.exe", "-DPLATFORM_PC", "-DCGLM_ALL_UNALIGNED",
                    "-UNDEBUG", "-I" + str(root / "source"), "-I" + str(root / "third_party"),
                    str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
