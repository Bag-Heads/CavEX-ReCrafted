"""Exercise the production local-body draw function with real types and a renderer recorder."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "source/entity/entity_remote_player.c").read_text()
start = source.index("void entity_local_player_render_other(")
end = source.index("\nvoid entity_remote_player(", start)
camera_source = (root / "source/game/camera.c").read_text()
camera_start = camera_source.index("void camera_bob_client_tick(")
camera_end = camera_source.index("\nvoid camera_get_ray", camera_start)
main_source = (root / "source/main.c").read_text(encoding="utf-8")
feed_start = main_source.index("camera_bob_client_tick(&gstate.players[0].camera,")
feed_end = main_source.index("if(gstate.autopilot.active", feed_start)
code = r'''
#include <assert.h>
#include <math.h>
#include <string.h>
#include "game/game_state.h"
#include "entity/combat.h"
#include "network/server_interface.h"
#include "graphics/render_entity.h"
#include "platform/gfx.h"
struct game_state gstate;
static struct inventory inv;
static unsigned draws, tint_calls;
static bool seated, sneaking;
static mat4 drawn;
static float pitch, yaw, amplitude;
static int walk_frame;
struct inventory* windowc_get_latest(struct window_container* wc) { return &inv; }
bool inventory_get_hotbar_item(struct inventory* i, struct item_data* item) {
 assert(i==&inv); item->id=5; return true;
}
bool inventory_get_slot(struct inventory* i,size_t slot,struct item_data* item) {
 assert(i==&inv);item->id=slot-INVENTORY_SLOT_ARMOR+300;return true;
}
bool entity_get_block(struct entity* e,w_coord_t x,w_coord_t y,w_coord_t z,struct block_data* b) {
 memset(b,0,sizeof *b);b->sky_light=15;return true;
}
ptime_t time_get(void) { return (ptime_t){0}; }
float time_diff_s(ptime_t a,ptime_t b) { return 1; }
void render_entity_update_light(uint8_t light) { assert(light==15); }
void render_entity_set_limb_swing(float value) { amplitude=value; }
void gfx_entity_tint(float r,float g,float b,float a) { tint_calls++; }
void render_entity_player(mat4 view,float head,float body,float tilt,int frame,int held,
 bool sneak,float swing,bool riding,const uint16_t armor[4]) {
 draws++;glm_mat4_copy(view,drawn);yaw=head;pitch=tilt;seated=riding;sneaking=sneak;walk_frame=frame;
 assert(head==body && held==5 && swing==0);
 for(unsigned i=0;i<4;i++)assert(armor[i]==300+i);
}
FUNCTION
CAMERA_FUNCTION
static void tick_owners(void) { OWNER_FEED }
int main(void) {
 dict_entity_init(gstate.entities);
 struct entity e={.id=7,.health=MAX_PLAYER_HEALTH};
 struct client_session* owner=&gstate.players[1];
 mat4 view=GLM_MAT4_IDENTITY_INIT;
 entity_local_player_render_other(NULL,view,.5F);assert(draws==0);
 owner->local_player=&e;
 entity_local_player_render_other(owner,view,.5F);assert(draws==0);
 struct window_container wc={0};owner->windows[WINDOWC_INVENTORY]=&wc;
 e.pos_old[0]=2;e.pos[0]=4;e.pos_old[1]=e.pos[1]=PLAYER_EYE_HEIGHT;
 owner->camera.ry=GLM_PI_2;owner->camera.rx=GLM_PI_2;
 struct entity original=e;
 entity_local_player_render_other(owner,view,.5F);
 assert(draws==1 && fabsf(drawn[3][0]-3)<.0001F && fabsf(drawn[3][1])<.0001F);
 assert(fabsf(pitch)<.0001F && fabsf(yaw-90)<.0001F && !seated && !sneaking);
 assert(memcmp(&e,&original,sizeof e)==0); /* Rendering never moves the body. */
 int initial_frame=walk_frame;
 for(unsigned tick=0;tick<10;tick++)tick_owners();
 entity_local_player_render_other(owner,view,.5F);
 assert(walk_frame!=initial_frame && owner->camera.bob.dwm>0);
 assert(gstate.players[0].camera.bob.dwm==0); /* P2 walking cannot animate P1. */
 draws--; /* Keep the independent mount assertions below unchanged. */
 struct entity cart={.type=ENTITY_MINECART};cart.data.minecart.occupied=true;
 cart.data.minecart.occupant_id=e.id;dict_entity_set_at(gstate.entities,20,&cart);
 owner->player_sync.state_sneaking=true;e.hurt=true;
 entity_local_player_render_other(owner,view,.5F);
 assert(draws==2 && seated && sneaking && tint_calls==2);
 assert(fabsf(drawn[3][1]+.125F)<.0001F);
 cart.data.minecart.occupant_id=8;
 e.pos_old[0]=e.pos[0];
 entity_local_player_render_other(owner,view,.5F);assert(!seated && amplitude==0);
 /* Actual health deltas, not the network-only hurt flag, drive feedback. */
 e.hurt=false;owner->player_sync.state_sneaking=false;
 unsigned before=tint_calls;
 e.health-=8;tick_owners();
 entity_local_player_render_other(owner,view,.5F);
 assert(tint_calls==before+2 && owner->camera.hurt.hurt_time==10);
 for(unsigned i=0;i<10;i++)tick_owners();
 before=tint_calls;entity_local_player_render_other(owner,view,.5F);
 assert(tint_calls==before);
 e.health=0;
 for(unsigned i=0;i<8;i++)tick_owners();
 entity_local_player_render_other(owner,view,0);
 float expected_cos=cosf(sqrtf(7.0F/20*1.6F)*GLM_PI_2);
 assert(fabsf(drawn[1][1]-expected_cos)<.0001F && amplitude==0);
 unsigned corpse_draws=draws;
 for(unsigned i=8;i<21;i++)tick_owners();
 entity_local_player_render_other(owner,view,0);assert(draws==corpse_draws);
 e.health=MAX_PLAYER_HEALTH;tick_owners();
 entity_local_player_render_other(owner,view,0);
 assert(draws==corpse_draws+1 && fabsf(drawn[1][1]-1)<.0001F);
 dict_entity_clear(gstate.entities);
}
'''.replace("CAMERA_FUNCTION", camera_source[camera_start:camera_end]).replace("OWNER_FEED", main_source[feed_start:feed_end]).replace("FUNCTION", source[start:end])
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / "local_draw.c"
    exe = c.with_suffix(".exe")
    c.write_text(code)
    subprocess.run(["C:/msys64/ucrt64/bin/gcc.exe", "-DPLATFORM_PC", "-DCGLM_ALL_UNALIGNED",
                    "-UNDEBUG", "-I" + str(root / "source"), "-I" + str(root / "third_party"),
                    str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: owner walking, damage tint expiry, Beta death roll, corpse retirement, respawn reset, equipment and mounts")
