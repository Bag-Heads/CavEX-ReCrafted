"""Exercise production guest position publication and its local render flag."""
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[1]
source = (root/'source/network/client_interface.c').read_text(encoding='utf-8')
start = source.index('const struct input_player_frame* guest_input = input_player_frame(1);')
end = source.index('\n\t}\n}', start)
body = source[start:end]
out = Path('tmp/jobs/player-visuals-0930'); out.mkdir(parents=True,exist_ok=True)
harness = r'''
#include <assert.h>
#include <stdio.h>
#include "game/game_state.h"
#include "network/server_interface.h"
#include "platform/input_owner.h"
#include "entity/player_ysize.h"
struct game_state gstate;
static struct input_player_frame frame;
static bool accept;
static struct server_rpc sent;
const struct input_player_frame* input_player_frame(unsigned slot) {
 assert(slot==1);return &frame;
}
ptime_t time_get(void) {return (ptime_t){.tv_sec=5};}
bool svin_rpc_send_from(struct server_rpc_source owner,struct server_rpc* call) {
 assert(owner.slot==1&&owner.generation==7);sent=*call;return accept;
}
static void publish(void) {BODY}
int main(void) {
 struct entity player={0};gstate.players[1].local_player=&player;
 gstate.players[1].local_generation=7;
 gstate.players[0].player_sync.state_sneaking=true;
 for(int cycle=0;cycle<3;cycle++) {
  frame.down[IB_SNEAK]=true;accept=false;publish();
  assert(!gstate.players[1].player_sync.state_sneaking);
  accept=true;publish();assert(sent.payload.player_pos.sneaking);
  assert(gstate.players[1].player_sync.state_sneaking);
  frame.down[IB_SNEAK]=false;accept=false;publish();
  assert(gstate.players[1].player_sync.state_sneaking);
  accept=true;publish();assert(!sent.payload.player_pos.sneaking);
  assert(!gstate.players[1].player_sync.state_sneaking);
  assert(gstate.players[0].player_sync.state_sneaking);
 }
 puts("PASS: P2 accepted sneak/release reaches local render state; rejected sends and P1 remain unchanged, three cycles");
}
'''.replace('BODY',body)
c=out/'guest-sneak.c';c.write_text(harness,encoding='utf-8');exe=out/'guest-sneak.exe'
subprocess.run(['C:/msys64/ucrt64/bin/gcc.exe','-UNDEBUG','-DPLATFORM_PC','-DCGLM_ALL_UNALIGNED',
 '-I'+str(root/'source'),'-I'+str(root/'third_party'),str(c),'-o',str(exe),'-lm'],check=True)
subprocess.run([str(exe.resolve())],check=True)


