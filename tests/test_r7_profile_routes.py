"""Execute main.c's dimensions driver and recovery walk arbitration verbatim.

R7 dims tape lines 250, 455, 480: away -> timeout with zero host movement,
then COMMIT2 arrives after the route has already abandoned the guest portal.
Clock, publication, terrain and physics are explicit doubles here; real portal
terrain checks run in test_autopilot_route_rank and saved-Join handlers in
test_player_residency_admission lifecycle_saved_join. No GX boot is needed.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef uint64_t ptime_t;
typedef int w_coord_t;
enum { WORLD_DIM_OVERWORLD=0, WORLD_DIM_NETHER=-1, BLOCK_PORTAL=90 };
struct world { int dimension; } worlds[2];
struct entity { float pos[3]; int health; struct world *world; } bodies[2];
struct session { struct entity *local_player; };
struct { struct session players[2]; struct world world; bool world_switch_pending; } gstate;
struct client_world_context { uint32_t server_generation; } contexts[2]={{1},{2}};
struct autopilot_portal_return_anchor { struct world *world; int x[2],z[2]; };
struct autopilot_portal_return_step { bool inside; float target_x,target_z; };
struct clin_world_payload { struct world *terrain; };
struct block_data { int type; };
struct hw_split_dims_evidence {
 unsigned host_commits,rel_rx; int dim_v0,dim_v1,wslot_v0,slot1_cols,rel_slot;
 bool dims_active;
} evidence;
static int host_slot, patrol=-1;
static bool walking;
static struct { bool host_walk_valid,drive_host_walk; } s_split_recovery_act;
uint32_t g_srv_sim_ticks;
static int32_t time_diff_ms(ptime_t a,ptime_t b){return (int32_t)(b-a);}
static int clin_world_view_slot(unsigned v){return v ? 0 : host_slot;}
static void *clin_world_contexts(void){return 0;}
static const struct client_world_context *client_ctx_registry_current(void *p,unsigned s){return &contexts[s];}
static struct world *view_terrain(unsigned v){return v ? &worlds[0] : &worlds[host_slot];}
static bool client_session_host_gameplay_active(struct session *s){return true;}
static bool clin_world_payload_of_view(unsigned v,struct clin_world_payload *p){p->terrain=view_terrain(v);return true;}
static struct block_data world_get_block(struct world *w,int x,int y,int z){return (struct block_data){z==5 ? BLOCK_PORTAL : 0};}
static void split_dims_fill(struct hw_split_dims_evidence *d){*d=evidence;}
static void autopilot_script_walk(bool on,int heading){walking=on;}
static void autopilot_script_portal_walk(bool on,const struct autopilot_portal_return_anchor *a){walking=on;patrol=-1;}
static void autopilot_script_portal_patrol(const struct autopilot_portal_return_anchor *a,unsigned end){walking=true;patrol=(int)end;}
static bool autopilot_portal_return_rearmed(uint32_t a,uint32_t b){return b-a>=1200;}
static bool autopilot_portal_return_anchor(struct entity *e,struct autopilot_portal_return_anchor *a){*a=(struct autopilot_portal_return_anchor){.world=e->world,.x={0,1},.z={0,0}};return true;}
static bool autopilot_portal_return_step(struct entity *e,const struct autopilot_portal_return_anchor *a,struct autopilot_portal_return_step *r){*r=(struct autopilot_portal_return_step){.inside=true};return true;}
/* DRIVER */
static void apply_other_driver(void) {
/* ARBITRATION */
}
static ptime_t now;
static float moved;
static void frame(void) {
 now+=50; g_srv_sim_ticks++;
 /* Ordinary input double; no motion unless the production driver owns it. */
 if(walking) {
  float old=bodies[0].pos[0];
  if(host_slot==1 && patrol>=0) {
   float target=patrol+0.5F, dx=target-old;
   bodies[0].pos[0]+=fmaxf(-0.1F,fminf(0.1F,dx));
  } else if(host_slot==0) bodies[0].pos[0]+=0.2F;
  moved+=fabsf(bodies[0].pos[0]-old);
 }
 split_dims_portal_step(now);
 apply_other_driver(); /* recovery is unarmed in split_dims_6m */
}
int main(void) {
 bodies[0]=(struct entity){.pos={1,5.62F,0.5F},.health=160,.world=&worlds[1]};
 bodies[1]=(struct entity){.pos={0.5F,5.62F,2.5F},.health=160,.world=&worlds[0]};
 gstate.players[0].local_player=&bodies[0];gstate.players[1].local_player=&bodies[1];
 host_slot=1;s_split_dims_armed=s_split_dims_started=true;
 evidence=(struct hw_split_dims_evidence){.host_commits=1,.dim_v0=-1,.dim_v1=0,.wslot_v0=1,.slot1_cols=81,.dims_active=true};
 split_dims_enter(SPLIT_DIMS_HOST_TRIP,now);
 split_dims_host_enter(SPLIT_DIMS_HOST_AWAY,now);
 for(unsigned i=0;i<1201;i++)frame();
 assert(s_split_dims_host_phase==SPLIT_DIMS_HOST_AWAY);
 /* R11: require route margin, not a near-threshold 32-block pass. */
 assert(s_split_dims_patrol_legs>=61 && moved>=48);
 assert(s_split_dims_return_entered);
 evidence.host_commits=2;evidence.dim_v0=0;evidence.dims_active=false;
 host_slot=0;bodies[0].world=&worlds[0];frame();
 assert(s_split_dims_host_phase==SPLIT_DIMS_HOST_BACK);
 evidence.slot1_cols=0;evidence.rel_rx=1;frame();
 assert(s_split_dims_host_phase==SPLIT_DIMS_HOST_DONE);
 frame();assert(s_split_dims_phase==SPLIT_DIMS_SEEK && !split_dims_walk_released());
 bodies[1].pos[2]=5.5F;frame();
 assert(s_split_dims_phase==SPLIT_DIMS_HOLD && split_dims_walk_released());
 evidence.dim_v1=-1;evidence.dims_active=true;frame();
 assert(s_split_dims_phase==SPLIT_DIMS_ACROSS);
 moved=0;
 for(unsigned i=0;i<250;i++)frame();
 assert(s_split_dims_slot0_walk_done && s_split_dims_phase==SPLIT_DIMS_ACROSS && moved>=40);
 /* A stalled pane retains its original deadline and releases no false leg. */
 evidence.dims_active=false;split_dims_enter(SPLIT_DIMS_HOLD,now);
 now+=SPLIT_DIMS_HOLD_MS;split_dims_portal_step(now);
 assert(s_split_dims_phase==SPLIT_DIMS_TIMEOUT);
 puts("PASS R11 main driver: 61 Nether legs, >=48 blocks, drain/release, guest portal, 40-block host leg; unchanged timeout");
}
'''


def run(mutate=False):
    source = (ROOT / "source/main.c").read_text(encoding="utf-8")
    driver = source[source.index("#define SPLIT_DIMS_NO_VIEW"):source.index("/* colcap-1006 (ASTRA-CC-R3 item 2)")]
    a = driver.index("static void split_dims_host_geometry_log(")
    b = driver.index("/* The profile's return", a)
    driver = driver[:a] + "static void split_dims_host_geometry_log(unsigned ms) {}\n" + driver[b:]
    # Exact consuming statement, including its guard when present. Deleting the
    # guard recreates the R7 cross-profile cancellation and must turn this RED.
    end = source.index("autopilot_script_walk(s_split_recovery_act.drive_host_walk, 180);")
    start = source.rfind("\n", 0, end)
    guard = source.rfind("if(s_split_recovery_act.host_walk_valid)", 0, end)
    if guard >= 0 and source[guard:end].count("\n") == 1:
        start = guard
    arbitration = source[start:source.index(";", end)+1]
    if mutate:
        arbitration = arbitration.replace("if(s_split_recovery_act.host_walk_valid)", "")
    cc = shutil.which("gcc") or ("C:/msys64/ucrt64/bin/gcc.exe" if os.name == "nt" else "cc")
    with tempfile.TemporaryDirectory(prefix="r7_routes_") as td:
        c = Path(td)/"routes.c"
        c.write_text(HARNESS.replace("/* DRIVER */", driver).replace("/* ARBITRATION */", arbitration))
        exe = Path(td)/"routes.exe"
        subprocess.run([cc,"-std=c99","-UNDEBUG",str(c),"-lm","-o",str(exe)], check=True, timeout=60)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
        if mutate:
            assert result.returncode != 0, "unarmed-driver cancellation was not caught"
        else:
            assert result.returncode == 0, result.stdout + result.stderr
            print(result.stdout.strip())


if __name__ == "__main__":
    run()
    run(mutate=True)

