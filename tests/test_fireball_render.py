"""Exercise the production entity draw loop with fireball render recorders."""
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
static unsigned ordinary, fireballs, thrown, flames;
volatile uint32_t g_wd_draw_site, g_wd_draw_id, g_wd_draw_aux;
void entity_fireball_render(struct entity* e, struct camera* c, float td) {
    assert(e->data.arrow.fireball && td == .5F);
    fireballs++;
}
void entity_thrown_render(struct entity* e, struct camera* c, float td) {
    assert(!e->data.arrow.fireball && td == .5F);
    assert(e->data.arrow.item_id == ITEM_EGG || e->data.arrow.item_id == ITEM_SNOW_BALL);
    thrown++;
}
void render_entity_fire_draw(const struct entity* e, const struct camera* c, float td) {
    assert(e->on_fire && td == .5F);
    if(e->data.arrow.fireball) {
        assert(e->width == 1.0F && e->height == 1.0F);
        assert(e->y_offset == 0 && e->y_size == 0);
    }
    flames++;
}
static void draw(struct entity* e, mat4 view, float td) {
    assert(td == .5F);
    ordinary++;
}
FUNCTION
static void check(dict_entity_t dict, struct entity* e, struct camera* c,
                  float x, bool visible) {
    e->pos[0]=x; e->pos[1]=0; e->pos[2]=0;
    struct entity before=*e;
    ordinary=fireballs=thrown=flames=0;
    entities_client_render(dict,c,.5F);
    assert(fireballs == (unsigned)(visible && e->data.arrow.fireball));
    bool is_thrown = !e->data.arrow.fireball
        && (e->data.arrow.item_id == ITEM_EGG || e->data.arrow.item_id == ITEM_SNOW_BALL);
    assert(thrown == (unsigned)(visible && is_thrown));
    assert(ordinary == (unsigned)(visible && !e->data.arrow.fireball && !is_thrown));
    assert(flames == (unsigned)(visible && e->data.arrow.fireball));
    assert(memcmp(e,&before,sizeof before)==0);
}
int main(void) {
    dict_entity_t dict; dict_entity_init(dict);
    struct entity e={.id=7,.type=ENTITY_ARROW,.render=draw};
    struct camera c={0};
    dict_entity_set_at(dict,e.id,&e);
    /* [740c583 EntityFireball.isInRangeToRenderDist]: 1 * 4 * 64. */
    e.data.arrow.fireball=true;
    e.data.arrow.item_id=332;
    check(dict,&e,&c,31.9F,true);
    check(dict,&e,&c,32.0F,true);
    check(dict,&e,&c,48.0F,true);
    check(dict,&e,&c,63.9F,true);
    check(dict,&e,&c,255.9F,true);
    check(dict,&e,&c,256.0F,false);
    c.x=300;
    check(dict,&e,&c,48.0F,true);
    check(dict,&e,&c,0.0F,false);
    c.x=0;
    /* Beta RenderManager routes snowballs and eggs to RenderSnowball. */
    e.data.arrow.fireball=false;
    e.data.arrow.item_id=ITEM_SNOW_BALL;
    check(dict,&e,&c,31.9F,true);
    check(dict,&e,&c,32.0F,false);
    e.data.arrow.item_id=ITEM_EGG;
    check(dict,&e,&c,31.9F,true);
    /* Real arrows retain their flight-oriented render path. */
    e.data.arrow.item_id=ITEM_ARROW;
    check(dict,&e,&c,31.9F,true);
    e.data.arrow.item_id=ITEM_FISHING_ROD;
    check(dict,&e,&c,31.9F,true);
    e.render=NULL;
    check(dict,&e,&c,0.0F,false);
    dict_entity_clear(dict);
    puts("PASS: Beta fireball, thrown icon and ordinary arrow draw dispatch");
}
'''.replace("FUNCTION", source[start:end])
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / "fireball_render.c"
    exe = c.with_suffix(".exe")
    c.write_text(code)
    subprocess.run(["C:/msys64/ucrt64/bin/gcc.exe", "-DPLATFORM_PC", "-DCGLM_ALL_UNALIGNED",
                    "-UNDEBUG", "-I" + str(root / "source"), "-I" + str(root / "third_party"),
                    str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
