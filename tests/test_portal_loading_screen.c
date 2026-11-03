/* ci-1101/56: Beta Minecraft.usePortal / NetClientHandler.handleRespawn.
 * Exercise the real client RPC consumer; presentation adds no readiness gate. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "block/blocks.h"
#include "item/items.h"
#include "game/game_state.h"
#include "game/gui/loading_tips.h"
#include "game/gui/screen.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "network/server_interface.h"

static void reset(int dimension) {
 clin_process(&(struct client_rpc){.type=CRPC_WORLD_RESET,
  .payload.world_reset={.dimension=dimension,.local_entity=77}});
}
static void hold(unsigned owner, unsigned id, bool begin) {
 clin_process(&(struct client_rpc){.type=CRPC_TRANSFER_HOLD,
  .payload.transfer_hold={.actor_slot=owner,.actor_generation=owner?9:0,
   .request_id=id,.begin=begin}});
}
static void dismiss(void) {
 screen_set(&screen_title);
 assert(loading_reason_get(0)==UNKNOWN);
 gstate.players[0].current_screen=&screen_ingame;
}
int main(void) {
 blocks_init(); items_init(); assert(svin_init()); assert(clin_init());
 memset(&gstate,0,sizeof gstate); world_create(&gstate.world);
 dict_entity_init(gstate.entities); gstate.players[0].visible_window=-1;
 assert(clin_world_payloads_set_enabled(true));
 loading_tips_reset(); reset(WORLD_DIM_OVERWORLD);
 assert(loading_reason_get(0)==UNKNOWN); /* initial load is not a portal */
 gstate.players[0].current_screen=&screen_ingame;
 clin_process(&(struct client_rpc){.type=CRPC_BEGIN_WORLD_SWITCH});
 assert(loading_reason_get(0)==PORTAL_IN);
 assert(!strcmp(loading_reason_heading(loading_reason_get(0)),"Entering the Nether"));
 reset(WORLD_DIM_NETHER);
 assert(loading_reason_get(0)==PORTAL_IN);
 dismiss(); assert(loading_reason_get(0)==UNKNOWN);
 clin_process(&(struct client_rpc){.type=CRPC_BEGIN_WORLD_SWITCH});
 assert(loading_reason_get(0)==PORTAL_OUT);
 reset(WORLD_DIM_OVERWORLD); dismiss();
 gstate.remote_session=true;
 reset(WORLD_DIM_NETHER); assert(loading_reason_get(0)==PORTAL_IN);
 dismiss();
 reset(WORLD_DIM_OVERWORLD); assert(loading_reason_get(0)==PORTAL_OUT);
 dismiss();
 reset(WORLD_DIM_OVERWORLD); assert(loading_reason_get(0)==UNKNOWN);
 gstate.remote_session=false;
 /* A split guest's hold must not change the host's episode. */
 gstate.players[1].local_bound=true; gstate.players[1].local_generation=9;
 loading_reason_begin(0,LOAD_SAVE);
 hold(1,1,true);
 assert(loading_reason_get(1)==PORTAL_IN);
 assert(loading_reason_get(0)==LOAD_SAVE);
 hold(1,1,false);
 gstate.players[1].local_phase=CLIENT_GUEST_ATTACHED;
 gstate.players[1].local_gameplay_ready=true;
 gstate.players[1].local_player=gstate.players[0].local_player;
 clin_update_drain();
 assert(loading_reason_get(1)==UNKNOWN); /* existing guest readiness */
 gstate.world.dimension=WORLD_DIM_NETHER;
 hold(1,2,true);
 assert(loading_reason_get(1)==PORTAL_OUT);
 assert(loading_reason_get(0)==LOAD_SAVE);
 hold(1,2,false);
 gstate.players[1].local_player=NULL;
 gstate.players[1].local_bound=false; /* cancellation / disconnect */
 clin_update_drain();
 assert(loading_reason_get(1)==UNKNOWN);
 assert(loading_reason_get(0)==LOAD_SAVE);
 puts("PASS portal_loading_screen: local in/out, MP changed/same dimension, split isolation and cancellation");
 return 0;
}
