"""Run real death UI and host gameplay gate across a loading-screen replacement."""
from pathlib import Path
import subprocess,sys
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[1]
out=Path('tmp/jobs/respawn-movement-0930');out.mkdir(parents=True,exist_ok=True)
main=(root/'source/main.c').read_text(encoding='utf-8')
wired='screen_death_host_respawn_poll();' in main
if wired:
 assert main.index('screen_death_host_respawn_poll();')>main.index('\t\tclin_update();')
 assert main.index('screen_death_host_respawn_poll();')<main.index('bool host_entities_active=game_state_client_entities_active')
session=(root/'source/game/client_session.c').read_text(encoding='utf-8')
gate=session[session.index('bool client_session_host_gameplay_active('):session.index('bool client_session_local_gameplay_active(')]
test=(root/'tests/test_death_commit_timing.c').read_text(encoding='utf-8')
extra=r"""
GATE
static void test_loading_replaces_death_ui(void) {
 enter_death();
 held[IB_GUI_CLICK]=false;step(.5F);hold_for(800);
 assert(respawn_rpcs==1 && gstate.players[0].death.active);
 /* Real screen dispatch stops calling death.update when loading replaces it. */
 current=&screen_load_world;
 gstate.players[0].current_screen=current;
 gstate.world_loaded=false;
 gstate.world_switch_pending=false;
 local_player.health=20;
 POLL
 assert(gstate.players[0].death.active && !client_session_host_gameplay_active(&gstate.players[0]));
 assert(cue_count==0);
 /* The new body and epoch settle while that screen is no longer dispatched. */
 gstate.world_loaded=true;gstate.world_switch_pending=false;
 current=&screen_ingame;gstate.players[0].current_screen=current;
 POLL
 assert(!gstate.players[0].death.active && "respawn left host physics/camera gated by abandoned death UI");
 assert(client_session_host_gameplay_active(&gstate.players[0]));
 assert(cue_count==1);
 POLL
 assert(cue_count==1);
 puts("PASS: loading replacement retires death gate; live host physics/camera admitted once");
}
"""
extra=extra.replace('GATE',gate).replace('POLL','screen_death_host_respawn_poll();' if wired else '/* Installed main has no screen-independent respawn poll. */')
test=test.replace('int main(void) {',extra+'\nint main(void) {\n test_loading_replaces_death_ui(); test_loading_replaces_death_ui();')
c=out/'regression.c';c.write_text(test,encoding='utf-8')
exe=out/'regression.exe'
subprocess.run(['C:/msys64/ucrt64/bin/gcc.exe','-UNDEBUG','-DPLATFORM_PC','-DCAVEWII_CLIENT_INTERFACE_TEST','-DCGLM_ALL_UNALIGNED','-I'+str(root/'source'),'-I'+str(root/'third_party'),str(c),str(root/'source/game/gui/screen_death.c'),str(root/'source/game/gui/menu_header.c'),str(root/'source/platform/time.c'),'-o',str(exe),'-lm'],check=True)
subprocess.run([str(exe.resolve())],check=True)
