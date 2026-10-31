"""Compile the shipping main-loop stats routing, fallback draw and auto-exit gate.

Only platform rendering/input are adapted by test_achievements_r2.c. Persistence,
dialog state, owner state and the hw_run readiness function are production code.
"""
from pathlib import Path
import re
import sys

root, output = map(Path, sys.argv[1:])
main = (root / "source/main.c").read_text(encoding="utf-8")


def between(start, end):
    return main.split(start, 1)[1].split(end, 1)[0]


callback = "static void retained_stats_answer" + between(
    "static void retained_stats_answer", "static void local_pause_stats_saved")
poll = "  /* An init failure" + between(
    "  /* An init failure", "\t\tthread_main_pacing_checkpoint_at(THREAD_PACING_RPC);")
update = "  bool stats_modal[2]" + between(
    "  bool stats_modal[2]", "  if(stats_modal[0] &&")
draw = next(line for line in main.splitlines() if "screen_stats_save_render(0," in line)
draw_gate = next((line for line in main.splitlines() if "bool draw_viewports =" in line), "")
viewport_draw = next(line for line in main.splitlines() if "screen_stats_save_render(viewport," in line)
gate_start = "if(!achievement_quit_canceled" if "if(!achievement_quit_canceled" in main else "if(!hw_save_quit_blocks_exit"
gate = gate_start + between(gate_start, "{\n")
quit_call = re.search(r"if\(!(?P<call>screen_stats_save_quit\([^\n]*?\))\) \{", main)["call"]
output.write_text(callback + "\nstatic void main_stats_poll(void) {\n"
    "bool achievement_ready = true;\n" + poll + "\n}\n"
    "static void main_stats_update(void) {\nunsigned view_count=1;\n"
    "struct {int y, height;} view_rect[2]={{0,480},{240,240}};\n"
    "bool menu_consumed[2]={false,false};\n" + update + "\n(void)menu_consumed;\n}\n"
    "static void main_stats_draw(bool render_world) {\n" + draw_gate + "\n" + draw + "\n}\n"
    "static void main_stats_viewport_draw(unsigned viewport, int view_height) {\n"
    + viewport_draw + "\n}\n"
    "static bool main_auto_exit_gate(bool achievement_quit_canceled) {\n"
    "struct hw_run_config hw_run={.enabled=true,.auto_exit=true};\n"
    "bool hw_save_quit_blocks_exit=false, hw_profile_done_sent=true;\n"
    "ptime_t hw_profile_done_at=time_get(), now=time_add_ms(hw_profile_done_at,60000);\n"
    + gate + "return true;\nreturn false;\n}\n"
    "static bool main_stats_quit(bool enabled, bool auto_exit) {\n"
    "struct hw_run_config hw_run={.enabled=enabled,.auto_exit=auto_exit};\nreturn " + quit_call + ";\n}\n"
    "#define RUN_QUIT(unattended) main_stats_quit(unattended, unattended)\n", encoding="utf-8")
