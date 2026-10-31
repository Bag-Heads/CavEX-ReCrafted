"""A dead hardware heartbeat cannot establish live-survival coverage."""
import contextlib
import hashlib
import io
import json
import re
from pathlib import Path
import tempfile
import subprocess
import sys

from test_perf_shutdown_collector import collector, TAPE_HASHES

ROOT = Path(__file__).resolve().parents[1]

def g9_send_cases(telemetry_exe):
    # Verbatim ce9ec71_10.10, w_g9b_s12345/telemetry.ndjson lines 8,29,53.
    # The actual collector rejects tick2 before its FIFO context can arrive.
    # .bin preserves the tape's CRLF bytes under the repo's LF normalization.
    raw = (ROOT / "tests/fixtures/g9_send_w_g9b.ndjson.bin").read_bytes()
    assert hashlib.sha256(raw).hexdigest() == (
        "42cd8cc1fe6f096e3c921069affc98d5e9c28e816785971bbffc2076b538a215")
    check(raw.decode().splitlines(keepends=True), "survival_guard_missing",
          "verbatim G9 priority inversion closes collector", expected_build="ce9ec71_10.10",
          require_guard=True)
    # Corrected queue/drain output goes through the real parser and gates.
    # This is a disclosed host transport model, never hardware certification.
    output = subprocess.check_output([str(telemetry_exe), "--g9-send-wire"], text=True)
    rows = [line + "\n" for line in output.splitlines() if line.startswith("{")]
    assert [json.loads(row)["event"] for row in rows] == [
        "perf_survival_guard", "heartbeat", "perf_survival_guard", "heartbeat"]
    check(rows, None, "real sender guard/heartbeat pairs accepted (noncertifying)",
          expected_build=None, require_guard=True)
    missing = rows[1:]
    check(missing, "survival_guard_missing", "transport cannot omit context",
          expected_build=None, require_guard=True)
    stale = json.loads(rows[0])
    stale["heartbeat_tick"] -= 1
    check([json.dumps(stale) + "\n", rows[1]], "survival_guard_missing",
          "transport cannot reuse stale context", expected_build=None, require_guard=True)

# Verbatim ce98952_10.09 hardware, v_r24a_s12345/telemetry.ndjson line 302.
# Full tape SHA: 3d46187cdd5df6b5a86371de3ff3e89dfd3323d0fbbb5fb9da844ddc33e4a4c3.
HARDWARE_ROW_SHA256 = '7cca0530c63ae24db1b03e801cc3bad8fb00f05355bc68f91e59fdffa7b01fc7'
HARDWARE_ROW = b'{"event":"heartbeat","run_id":"v_r24a_s12345","profile":"perf_stress_5m","build_id":"ce98952_10.09","up_ms":844958853096,"tick":27,"fps":14.45,"gpu_ms":0.0,"vsync_ms":62.7,"ms_bfs":0.44,"ms_light":0.0,"ms_lod":0.17,"ms_build":0.2,"ms_recv":0.0,"ms_render":0.28,"ms_tick":9.95,"ms_bfs_max":37.81,"ms_light_max":26.11,"ms_lod_max":0.22,"ms_build_max":53.7,"chunks":185,"mesh_q":8,"mesh_stale":18,"mesh_trunc":0,"estk":0,"near_evict_defer":10,"srv_rt_us":978,"srv_wt_us":175,"heap_used_kb":8270,"heap_free_kb":538,"hblk":1157,"htop_kb":3,"mem1_free_kb":6108,"mem2_free_kb":1371,"spill":0,"oom":0,"dlbad":0,"dlcall":0,"wpad":-1,"wksp":146224,"wkspT":262128,"wkspU":115912,"wkspB":1,"x":0.5,"y":84.62,"z":0.5,"rx_deg":90.0,"ry_deg":62.0,"near_radius":2,"far_view_distance":6,"fring":1,"far_missing":12,"far_missing_max":48,"far_settle_ms":0,"srv_gen_us":10348,"srv_gen_cols":1,"srv_gen_backlog":12,"srv_loop_it":15,"srv_elapsed_ms":434,"srv_ticks_pend":1,"srv_partial_m":579,"srv_sleep_ms":1,"srv_far_us_max":10408,"cw_set_n":6,"lw_to_admits":0,"cflood":47071,"ndisp":2186,"lq":[64,0,288,0,32,0,512,0,0,0,398,0,26808,0,65535,0,0,0,26808,0,0,0,0,0,0,0],"lqd_v":3,"lqd":[273,0,69925,0,0,0,18271,0,2,0,0,0,3,0,1056,18497,0,0,0,0,0,5023],"wd_main":20,"wd_srv":22,"wd_worker":2,"wd_hb_gap_max_ms":242,"world_loaded":true,"map_step":0,"map_max":0,"np_entered":0,"np_skipped":0,"np_focus":0,"np_flags":0,"np_rows":0,"map_sign":false,"smig_state":0,"smig_ms":0,"smig_regions":0,"smig_chunks":0,"smig_signs":0,"smig_expected":0,"smig_verified":0,"smig_chests":0,"smig_chests_expected":0,"smig_chests_verified":0,"smig_spawners":0,"smig_spawners_expected":0,"smig_spawners_verified":0,"save_fail_n":0,"save_fail_stage":0,"save_fail_sub":0,"save_skip_n":0,"save_idx_fb":0,"mp_state":0,"mp_gen":0,"mp_login":0,"mp_ret_kb":0,"mp_ret_n":0,"mp_ready":0,"mp_backlog":0,"mp_evt_q":0,"mp_evict":0,"mp_alloc_fail":0,"mp_reconn":0,"wl_pending":0,"wl_failed":0,"mp_rx_kb":0,"mp_end":0,"mp_end_phase":0,"mp_end_rx":0,"mp_end_reason":"","mp_dec_phase":0,"mp_dec_pkt":0,"mp_dec_have":0,"mp_dec_need":0,"mp_move_ticks":0,"mp_move_samples":0,"rpcdrop":0,"mp_adm_radius":0,"mp_resident_cols":0,"mp_backed_cols":0,"mp_backing_kb":0,"mp_backing_peak_kb":0,"mp_demotions":0,"mp_compactions":0,"mp_backing_refusals":0,"mp_ready_pending":0,"music_key":4,"music_ms":18858,"music_underruns":1,"music_starts":222,"music_rejects":0,"music_dur":85,"mrs_cycles":0,"mrs_target":0,"mrs_wedge":false,"creative":false,"dlcalls":233,"tris":29490,"ms_mesh_worker":63.96,"ms_mesh_worker_max":612.83,"mem2_lock_wait":0,"tel_fail":0,"tel_last_errno":0,"tel_quiesced":false,"frame_counter":1349,"mem2_carve_free_kb":16023,"chunks_meshed":1014,"near_missing":0,"near_mesh_pending":75,"near_missing_max":316,"near_settle_ms":0,"edit_to_visible_frames":739,"load_screen_ms":12846,"player_health":0,"loading_screen":false,"perf_frames":517,"perf_max_us":83411,"perf_over_30ms":208,"perf_gpu_over_16600":30,"perf_gpu_over_8000":30,"perf_invalid":0,"perf_duration_us":12628538,"far_storage":0,"vi_ready_max_us":72198,"vi_ready_over_8000":125,"gpu_poll_late_max_us":53912,"gpu_poll_late_over_8000":30,"server_time":186,"sd_trace_max_us":0,"sd_trace_over_30ms":0,"tel_drop":5,"tel_it":108,"tel_sends":5,"tel_smax_us":4516,"tel_qmax":4,"tel_hb_repl":0,"tel_wto":0,"tel_stg":0,"tel_stg_us":8000,"tel_pmax_us":1523,"tel_xmax_us":1625,"tel_abd":0,"tel_pret":1,"tel_prev":8,"tel_sret":1,"srv_us":[283269,5833693,39709,761444,58995,7552219,460,203823,2680,1249396,64326],"guard_scan_us":[2635,658],"world_us":[2348592,2217653,1567704,598165,1167442,1392364,383884,167333,0,0,0,166298,2292,0],"worst_parts_us":[0,1,261,459,2,165,192,2,76452,18,6,5787,2,35,71,4,0,0,0,4],"split_live":0,"split_pending":0,"split_toggle_ms":0,"split_last_ok":-1,"deaths":0,"death_cause":"none","tnt_valid":1,"tnt_st":0,"tnt_bits":0,"tnt_primed_left":0,"tnt_carved":0,"tnt_pad":0,"tnt_gen":2,"tnt_tick":0,"mm_moves":270,"mm_move_us":4941,"mm_env_walks":270,"mm_env_ents":18669,"rzv":2,"rzt":2483,"rzf":0,"rza":[[0,1,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],[255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]],"rze":[[0,0,0,0,0,0,0,0],[0,0,0,0,0,0,0,0]],"cpu_clock_step_ns":906,"cpu_valid":1,"cpu_ticks":2483,"cpu_srv_us":[285036,5834347,41327,762813,60177,7552248,562,203839,2777,1249245,66076],"cpu_nested_us":[0,2350538,2218172,179932,1568170,523500,1168026,1179119,80455,214122,384015,171061,0,0,0,166467,2398,0,0,0,0,105],"cpu_waits":[0,0,0,0,0,0],"cpu_main_us":33728022,"cpu_mesher_us":0,"cpu_frames":1349,"cpu_preemptions_available":false,"cpu_packed_us":287372,"cpu_clock_mode":3,"item_nbt_exhaustions":0,"op":137,"oph":256,"opc":256,"opf":18,"oer":0,"orc":0,"ocy":0,"oex":0,"owu":0,"db":121,"ds":121,"dln":2069,"dls":0,"dlp":0,"dlbp":470,"dlbo":0,"dlbf":0,"dlbm":3,"dlbs":0,"dlbc":4197,"dlbx":0,"srv_ph":[0,36799,0,19424,473422,37,112,829,0,160,0,5036,1558,36200,104513,0,0,0,0,145,121,584,337036,0],"srv_lean":7,"tclk":[3,1237,23,1,0,0,15,20,15,15,10,25,0,0],"server_radius":5,"mem2_carve_largest_valid":true,"mem2_carve_largest_allocatable_bytes":14640800}\r\n'


# ce98952_10.09 v_r24a_s12345/telemetry.ndjson; tape SHA in TAPE_HASHES.
LOADING_ROWS = [(29, 'a051727b0191d44789e09d7ba3e2a16950b94a5b75d6cd185672f99e2b6e46fc', b'{"event":"heartbeat","run_id":"v_r24a_s12345","profile":"perf_stress_5m","build_id":"ce98952_10.09","up_ms":844958826795,"tick":1,"fps":59.84,"gpu_ms":1.62,"vsync_ms":14.79,"ms_bfs":0.0,"ms_light":0.0,"ms_lod":0.01,"ms_build":0.02,"ms_recv":0.0,"ms_render":0.0,"ms_tick":0.0,"ms_bfs_max":0.0,"ms_light_max":0.0,"ms_lod_max":0.01,"ms_build_max":6.16,"chunks":0,"mesh_q":8,"mesh_stale":0,"mesh_trunc":0,"estk":0,"near_evict_defer":0,"srv_rt_us":0,"srv_wt_us":0,"heap_used_kb":5424,"heap_free_kb":76,"hblk":65,"htop_kb":73,"mem1_free_kb":9416,"mem2_free_kb":1371,"spill":0,"oom":0,"dlbad":0,"dlcall":0,"wpad":-1,"wksp":146224,"wkspT":262128,"wkspU":115912,"wkspB":1,"x":0.0,"y":0.0,"z":0.0,"rx_deg":0.0,"ry_deg":0.5,"near_radius":2,"far_view_distance":6,"fring":1,"far_missing":0,"far_missing_max":0,"far_settle_ms":0,"srv_gen_us":0,"srv_gen_cols":0,"srv_gen_backlog":0,"srv_loop_it":0,"srv_elapsed_ms":0,"srv_ticks_pend":0,"srv_partial_m":0,"srv_sleep_ms":1,"srv_far_us_max":0,"cw_set_n":6,"lw_to_admits":0,"cflood":0,"ndisp":0,"lq":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"lqd_v":3,"lqd":[0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0],"wd_main":20,"wd_srv":8,"wd_worker":2,"wd_hb_gap_max_ms":242,"world_loaded":false,"map_step":0,"map_max":0,"np_entered":0,"np_skipped":0,"np_focus":0,"np_flags":0,"np_rows":0,"map_sign":false,"smig_state":0,"smig_ms":0,"smig_regions":0,"smig_chunks":0,"smig_signs":0,"smig_expected":0,"smig_verified":0,"smig_chests":0,"smig_chests_expected":0,"smig_chests_verified":0,"smig_spawners":0,"smig_spawners_expected":0,"smig_spawners_verified":0,"save_fail_n":0,"save_fail_stage":0,"save_fail_sub":0,"save_skip_n":0,"save_idx_fb":0,"mp_state":0,"mp_gen":0,"mp_login":0,"mp_ret_kb":0,"mp_ret_n":0,"mp_ready":0,"mp_backlog":0,"mp_evt_q":0,"mp_evict":0,"mp_alloc_fail":0,"mp_reconn":0,"wl_pending":1,"wl_failed":0,"mp_rx_kb":0,"mp_end":0,"mp_end_phase":0,"mp_end_rx":0,"mp_end_reason":"","mp_dec_phase":0,"mp_dec_pkt":0,"mp_dec_have":0,"mp_dec_need":0,"mp_move_ticks":0,"mp_move_samples":0,"rpcdrop":0,"mp_adm_radius":0,"mp_resident_cols":0,"mp_backed_cols":0,"mp_backing_kb":0,"mp_backing_peak_kb":0,"mp_demotions":0,"mp_compactions":0,"mp_backing_refusals":0,"mp_ready_pending":0,"music_key":0,"music_ms":0,"music_underruns":0,"music_starts":0,"music_rejects":0,"music_dur":0,"mrs_cycles":0,"mrs_target":0,"mrs_wedge":false,"creative":false,"dlcalls":0,"tris":0,"ms_mesh_worker":0.0,"ms_mesh_worker_max":0.0,"mem2_lock_wait":0,"tel_fail":0,"tel_last_errno":0,"tel_quiesced":false,"frame_counter":40,"mem2_carve_free_kb":23890,"chunks_meshed":0,"near_missing":0,"near_mesh_pending":0,"near_missing_max":0,"near_settle_ms":0,"edit_to_visible_frames":0,"load_screen_ms":0,"player_health":160,"loading_screen":true,"perf_frames":0,"perf_max_us":0,"perf_over_30ms":0,"perf_gpu_over_16600":0,"perf_gpu_over_8000":0,"perf_invalid":0,"perf_duration_us":0,"far_storage":0,"vi_ready_max_us":0,"vi_ready_over_8000":0,"gpu_poll_late_max_us":0,"gpu_poll_late_over_8000":0,"server_time":0,"sd_trace_max_us":0,"sd_trace_over_30ms":0,"tel_drop":2,"tel_it":102,"tel_sends":27,"tel_smax_us":358266,"tel_qmax":4,"tel_hb_repl":0,"tel_wto":0,"tel_stg":0,"tel_stg_us":9000,"tel_pmax_us":257,"tel_xmax_us":1543,"tel_abd":0,"tel_pret":1,"tel_prev":8,"tel_sret":1,"srv_us":[245092,73,117,22,241,347539,13,6502,84,0,52],"guard_scan_us":[2635,658],"world_us":[222364,211626,118167,36969,80595,87983,0,4823,0,0,0,0,0,0],"worst_parts_us":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"split_live":0,"split_pending":0,"split_toggle_ms":0,"split_last_ok":-1,"deaths":0,"death_cause":"none","tnt_valid":1,"tnt_st":0,"tnt_bits":0,"tnt_primed_left":0,"tnt_carved":0,"tnt_pad":0,"tnt_gen":2,"tnt_tick":0,"rzv":2,"rzt":188,"rzf":0,"rza":[[0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],[255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]],"rze":[[0,0,0,0,0,0,0,0],[0,0,0,0,0,0,0,0]],"cpu_clock_step_ns":906,"cpu_valid":1,"cpu_ticks":187,"cpu_srv_us":[245178,71,109,24,219,347540,16,6502,87,0,59],"cpu_nested_us":[0,222498,211678,17240,118198,34486,80636,80563,2578,7471,0,5138,0,0,0,0,0,0,0,0,0,0],"cpu_waits":[0,0,0,0,0,0],"cpu_main_us":7435637,"cpu_mesher_us":0,"cpu_frames":40,"cpu_preemptions_available":false,"cpu_packed_us":9034,"cpu_clock_mode":3,"item_nbt_exhaustions":0,"op":0,"oph":0,"opc":256,"opf":0,"oer":0,"orc":0,"ocy":0,"oex":0,"owu":0,"db":18,"ds":4,"dln":0,"dls":0,"dlp":0,"dlbm":3,"dlbs":0,"dlbc":0,"dlbx":0,"srv_ph":[0,239815,0,0,0,1,0,278,345090,6502,11,83,0,391298,0,0,7445,0,0,11,0,0,1299,0],"server_radius":5,"mem2_carve_largest_valid":true,"mem2_carve_largest_allocatable_bytes":24463552}\r\n'), (54, 'd843e686affc456b58db2cfa8f80ce09c537a446324b0605ae98e41410dfb27a', b'{"event":"heartbeat","run_id":"v_r24a_s12345","profile":"perf_stress_5m","build_id":"ce98952_10.09","up_ms":844958827811,"tick":2,"fps":59.95,"gpu_ms":1.42,"vsync_ms":14.93,"ms_bfs":0.0,"ms_light":0.0,"ms_lod":0.02,"ms_build":0.05,"ms_recv":0.0,"ms_render":0.0,"ms_tick":0.0,"ms_bfs_max":0.0,"ms_light_max":0.0,"ms_lod_max":0.03,"ms_build_max":6.16,"chunks":0,"mesh_q":8,"mesh_stale":0,"mesh_trunc":0,"estk":0,"near_evict_defer":0,"srv_rt_us":0,"srv_wt_us":0,"heap_used_kb":5534,"heap_free_kb":90,"hblk":156,"htop_kb":85,"mem1_free_kb":9292,"mem2_free_kb":1371,"spill":0,"oom":0,"dlbad":0,"dlcall":0,"wpad":-1,"wksp":146224,"wkspT":262128,"wkspU":115912,"wkspB":1,"x":0.0,"y":0.0,"z":0.0,"rx_deg":0.0,"ry_deg":0.5,"near_radius":2,"far_view_distance":6,"fring":1,"far_missing":0,"far_missing_max":0,"far_settle_ms":0,"srv_gen_us":0,"srv_gen_cols":0,"srv_gen_backlog":0,"srv_loop_it":189,"srv_elapsed_ms":0,"srv_ticks_pend":0,"srv_partial_m":0,"srv_sleep_ms":1,"srv_far_us_max":0,"cw_set_n":6,"lw_to_admits":0,"cflood":0,"ndisp":0,"lq":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"lqd_v":3,"lqd":[0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0],"wd_main":20,"wd_srv":9,"wd_worker":2,"wd_hb_gap_max_ms":242,"world_loaded":false,"map_step":0,"map_max":0,"np_entered":0,"np_skipped":0,"np_focus":0,"np_flags":0,"np_rows":0,"map_sign":false,"smig_state":0,"smig_ms":0,"smig_regions":0,"smig_chunks":0,"smig_signs":0,"smig_expected":0,"smig_verified":0,"smig_chests":0,"smig_chests_expected":0,"smig_chests_verified":0,"smig_spawners":0,"smig_spawners_expected":0,"smig_spawners_verified":0,"save_fail_n":0,"save_fail_stage":0,"save_fail_sub":0,"save_skip_n":0,"save_idx_fb":0,"mp_state":0,"mp_gen":0,"mp_login":0,"mp_ret_kb":0,"mp_ret_n":0,"mp_ready":0,"mp_backlog":0,"mp_evt_q":0,"mp_evict":0,"mp_alloc_fail":0,"mp_reconn":0,"wl_pending":1,"wl_failed":0,"mp_rx_kb":0,"mp_end":0,"mp_end_phase":0,"mp_end_rx":0,"mp_end_reason":"","mp_dec_phase":0,"mp_dec_pkt":0,"mp_dec_have":0,"mp_dec_need":0,"mp_move_ticks":0,"mp_move_samples":0,"rpcdrop":0,"mp_adm_radius":0,"mp_resident_cols":0,"mp_backed_cols":0,"mp_backing_kb":0,"mp_backing_peak_kb":0,"mp_demotions":0,"mp_compactions":0,"mp_backing_refusals":0,"mp_ready_pending":0,"music_key":0,"music_ms":0,"music_underruns":0,"music_starts":0,"music_rejects":0,"music_dur":0,"mrs_cycles":0,"mrs_target":0,"mrs_wedge":false,"creative":false,"dlcalls":0,"tris":0,"ms_mesh_worker":282.45,"ms_mesh_worker_max":332.54,"mem2_lock_wait":0,"tel_fail":0,"tel_last_errno":0,"tel_quiesced":false,"frame_counter":101,"mem2_carve_free_kb":23867,"chunks_meshed":3,"near_missing":0,"near_mesh_pending":0,"near_missing_max":0,"near_settle_ms":0,"edit_to_visible_frames":0,"load_screen_ms":0,"player_health":160,"loading_screen":true,"perf_frames":0,"perf_max_us":0,"perf_over_30ms":0,"perf_gpu_over_16600":0,"perf_gpu_over_8000":0,"perf_invalid":0,"perf_duration_us":0,"far_storage":0,"vi_ready_max_us":0,"vi_ready_over_8000":0,"gpu_poll_late_max_us":0,"gpu_poll_late_over_8000":0,"server_time":0,"sd_trace_max_us":0,"sd_trace_over_30ms":0,"tel_drop":3,"tel_it":137,"tel_sends":25,"tel_smax_us":4435,"tel_qmax":4,"tel_hb_repl":0,"tel_wto":0,"tel_stg":0,"tel_stg_us":8000,"tel_pmax_us":243,"tel_xmax_us":1757,"tel_abd":0,"tel_pret":1,"tel_prev":8,"tel_sret":1,"srv_us":[245562,260,456,74,982,1323356,61,24296,332,0,217],"guard_scan_us":[2635,658],"world_us":[529527,506369,313708,109374,220646,262816,0,20657,0,0,0,0,0,0],"worst_parts_us":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"split_live":0,"split_pending":0,"split_toggle_ms":0,"split_last_ok":-1,"deaths":0,"death_cause":"none","tnt_valid":1,"tnt_st":0,"tnt_bits":0,"tnt_primed_left":0,"tnt_carved":0,"tnt_pad":0,"tnt_gen":2,"tnt_tick":0,"rzv":2,"rzt":214,"rzf":0,"rza":[[0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],[255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]],"rze":[[0,0,0,0,0,0,0,0],[0,0,0,0,0,0,0,0]],"cpu_clock_step_ns":906,"cpu_valid":1,"cpu_ticks":213,"cpu_srv_us":[245662,270,462,90,966,1266251,71,24298,343,0,240],"cpu_nested_us":[0,515969,493400,39300,304042,95936,207451,220619,9722,30652,0,20242,0,0,0,0,0,0,0,0,0,0],"cpu_waits":[0,0,0,0,0,0],"cpu_main_us":8453309,"cpu_mesher_us":0,"cpu_frames":101,"cpu_preemptions_available":false,"cpu_packed_us":34131,"cpu_clock_mode":3,"item_nbt_exhaustions":0,"op":0,"oph":2,"opc":256,"opf":0,"oer":0,"orc":0,"ocy":0,"oex":0,"owu":0,"db":41,"ds":16,"dln":18,"dls":0,"dlp":0,"dlbp":3,"dlbo":0,"dlbf":0,"dlbm":3,"dlbs":0,"dlbc":21,"dlbx":0,"srv_ph":[0,669,0,0,0,0,0,864,973267,17795,39,246,0,28596,0,0,2423,0,0,27,0,0,261,0],"tclk":[0,0,0,0,0,0,366,394,0,0,0,1,0,0],"server_radius":5,"mem2_carve_largest_valid":true,"mem2_carve_largest_allocatable_bytes":24440448}\r\n')]

def check(rows, expected, label, expected_build="ce98952_10.09", require_guard=False):
    with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
        out = Path(td)
        c = collector.Collector(collector._selftest_args(
            out, next((json.loads(row)["run_id"] for row in rows
                       if "run_id" in json.loads(row)), "guard-test"), "perf_stress_5m", duration=300,
            expected_build=expected_build))
        c.require_g9_evidence = require_guard
        consumed = []
        for row in rows:
            if c.done: break  # The real TCP collector stops at its first verdict.
            collector.ingest_telemetry_line(c, row)
            consumed.append(row)
        if expected is None:
            assert not c.done and not c.fail_reason, (label, c.fail_reason)
        else:
            assert c.done and c.failure_class == expected, (
                label, c.failure_class, c.fail_reason)
            assert c.write_result() != 0
            assert not (out / "PASS.json").exists()
            report = json.loads((out / "FAIL.json").read_text())
            assert report["failure_class"] == expected
        assert (out / "death-wire.ndjson").read_bytes() == "".join(consumed).encode("utf-8")
        manifest_guard = c.perf_survival_guard.copy()
    print("PASS", label)
    return manifest_guard


def guard_cases():
    # Explicit schema mutations of the SHA-pinned real-Wii row above. These
    # exercise rejection, never manufacture a hardware PASS or replace its tape.
    live = dict(json.loads(HARDWARE_ROW), player_health=160, d1=[31, 0, 0, 0, 0])
    wire = lambda e: json.dumps(e, separators=(",", ":")) + "\n"
    summary = [1, 31, 1, 1, 0, 1, 1, 1, 0, 0]
    context = [0, 0, 1, 1, 160, 168, 168, 159, 1] + [0]*27
    observer = dict(event="perf_survival_guard", run_id=live["run_id"], clock_hz=60750000,
                    heartbeat_tick=live["tick"], record=summary + context)
    check([wire(live)], "survival_guard_missing", "G9 both guard signals missing", require_guard=True)
    check([wire(dict(live, g1=1))], "survival_guard_missing", "guard missing context")
    for label, value, expected in (
        ("short", summary[:2], "survival_guard_corrupt"),
        ("boolean", [True] + summary[1:], "survival_guard_corrupt"),
        ("contradictory", [1, 31, 0, 1] + summary[4:], "survival_guard_contradictory"),
        ("minimum zero", summary[:5]+[0]+summary[6:], "survival_guard_invalid"),
        ("overflow", summary[:9]+[1], "survival_guard_invalid"),
        ("session", [1, 32]+summary[2:], "survival_guard_contradictory"),
    ):
        mutated = dict(observer, record=value + context)
        check([wire(mutated), wire(dict(live, g1=1))], expected, "guard " + label)
    check([wire(observer), wire(dict(live, g1=1))], None,
          "guard context schema (noncertifying mutation)")
    check([wire(observer), wire(live)], "survival_guard_missing", "guard missing marker")
    check([wire(dict(observer, heartbeat_tick=live["tick"]-1)), wire(dict(live, g1=1))],
          "survival_guard_missing", "guard stale context")
    for marker in (True, [], 0, 2):
        check([wire(dict(live, g1=marker))], "survival_guard_corrupt", f"guard corrupt marker {marker!r}")
    off = dict(observer, record=[1, 31, 0, 0, 0, 160, 0, 0, 0, 0] + [0]*36)
    manifest = check([wire(off), wire(live)], None, "OFF manifest requested/effective/version/session")
    assert manifest["version"] == 1 and manifest["session"] == 31
    assert manifest["requested"] is False and manifest["effective"] is False
    check([wire(off), wire(live)], "survival_guard_missing", "G9 explicitly OFF cannot certify guarded fixture", require_guard=True)
    later = dict(live, tick=live["tick"]+1, g1=1)
    for label, second in (("counter rollback", [1,31,1,1,0,1,0,0,0,0]+[0]*36),
                          ("minimum increase", summary[:5]+[160]+summary[6:]+context)):
        next_observer = dict(observer, heartbeat_tick=later["tick"], record=second)
        check([wire(observer), wire(dict(live, g1=1)), wire(next_observer), wire(later)],
              "survival_guard_contradictory", "guard "+label)
    corrupt = dict(observer, record=summary + context[:7]+[160]+context[8:])
    check([wire(corrupt)], "survival_guard_contradictory", "guard debit contradicts cap")


def guard_interval_cases():
    # Embedded VERBATIM hardware loading rows (line/SHA recorded below).
    # Only d1/g1 are appended to replay the newly versioned producer contract;
    # health/loading/world_loaded/perf_frames retain their original values.
    wire = lambda e: json.dumps(e, separators=(",", ":")) + "\n"
    summary = [2, 31, 1, 0, 0, 160, 0, 0, 0, 0]
    def pair(row, effective=0, barriers=0):
        event = json.loads(row)
        s = summary.copy(); s[3] = effective
        context = dict(event="perf_survival_guard", run_id=event["run_id"],
                       clock_hz=60750000, heartbeat_tick=event["tick"],
                       record=s + [0]*36 + [barriers])
        beat = row.rstrip()[:-1] + ',"d1":[31,0,0,0,0],"g1":1}\n'
        return [wire(context), beat]
    for line, sha, row in LOADING_ROWS:
        assert hashlib.sha256(row).hexdigest() == sha
        e = json.loads(row)
        assert e["player_health"] == 160 and e["loading_screen"] and not e["world_loaded"]
        check(pair(row.decode(), barriers=3), None, f"verbatim loading line {line} + guard v2", require_guard=True)
    live = dict(json.loads(HARDWARE_ROW), player_health=160, tick=50,
                up_ms=json.loads(HARDWARE_ROW)["up_ms"]+50000)
    ready = pair(wire(live), effective=1)
    prearm = pair(wire(dict(live, tick=49, perf_frames=0)), barriers=1)
    check(prearm + ready, None, "pre-arm then admitted effective", require_guard=True)
    for label, bit in (("loading", 2), ("pause", 4), ("ACK", 8), ("residency", 16),
                       ("transfer", 32), ("terminal", 64), ("world hold", 128)):
        held = pair(wire(dict(live, tick=51)), barriers=bit)
        resumed = pair(wire(dict(live, tick=52)), effective=1)
        manifest = check(ready + held + resumed, None, "effective/hold/resume " + label, require_guard=True)
        assert manifest["coverage"]["effective_rows"] == 2
        assert manifest["coverage"]["excluded_rows"] == 1
        assert manifest["coverage"]["barrier_rows"][label] == 1
        dead = dict(live, tick=51, d1=[31,1,7,0,0], deaths=1, death_cause="void")
        check(ready + [held[0], wire(dead)], "player_died", "death during hold " + label, require_guard=True)
        unavailable = dict(live, tick=51, d1=[0,4294967295,0,4294967295,0])
        check(ready + [held[0], wire(unavailable)], "death_telemetry_unavailable",
              "unavailable survival during hold " + label, require_guard=True)
    check(ready + pair(wire(dict(live, tick=51))), "survival_guard_invalid",
          "effective lost while eligible", require_guard=True)
    check(ready + pair(wire(dict(live, tick=51)), barriers=1), "survival_guard_invalid",
          "armed interval cannot revert to pre-arm", require_guard=True)
    check(pair(wire(live), effective=1, barriers=4), "survival_guard_contradictory",
          "effective contradicts hold", require_guard=True)
    check(pair(wire(live), barriers=256), "survival_guard_invalid",
          "scope mismatch cannot excuse missing guard", require_guard=True)
    check(pair(wire(live), barriers=1024), "survival_guard_corrupt",
          "unknown barrier bit", require_guard=True)
    old_guard = dict(json.loads(ready[0]), heartbeat_tick=49)
    check([wire(old_guard), wire(dict(live, d1=[31,0,0,0,0]))], "survival_guard_missing",
          "observed request cannot silently turn OFF without matched context")
    dead_loading = json.loads(LOADING_ROWS[0][2]); dead_loading.update(
        d1=[31,1,7,0,0], deaths=1, death_cause="void")
    check([wire(dead_loading)], "player_died", "committed death during pre-arm/loading", require_guard=True)
    check([wire(dict(dead_loading, player_health=-1))], "player_died",
          "committed death with no body before admission", require_guard=True)
    off = dict(live, d1=[31,0,0,0,0])
    manifest = check([wire(off)], None, "OFF control has no guard context/marker")
    assert manifest["requested"] is False and manifest["effective"] is False
    check([wire(off)], "survival_guard_missing", "guarded run cannot silently become OFF", require_guard=True)
    unavailable = dict(live, player_health=-1, perf_frames=0,
                       d1=[0,4294967295,0,4294967295,0])
    context = dict(json.loads(ready[0]), record=[2,0,1,0,0,4294967295,0,0,0,0]+[0]*37)
    manifest = check([wire(context), wire(unavailable)], None,
                     "pre-body unavailable guard report has no sentinel minimum")
    assert manifest["minimum_health"] is None and manifest["session"] is None
    # Actual completion entry point: no effective sample must never become PASS.
    with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
        c = collector.Collector(collector._selftest_args(Path(td), live["run_id"],
            "perf_stress_5m", duration=300, expected_build="ce98952_10.09"))
        c.require_g9_evidence = True
        for row in prearm: collector.ingest_telemetry_line(c, row)
        assert not c.done, c.fail_reason
        c.pass_("test completion")
        assert c.failure_class == "survival_guard_invalid", c.fail_reason
    print("PASS never effective after admission/completion")


def main():
    if "--dg-red" in sys.argv:
        guard_interval_cases()
        return
    if "--guard-red" in sys.argv:
        guard_cases()
        return
    run_id = "v_r24a_s12345"
    row = HARDWARE_ROW
    assert hashlib.sha256(row).hexdigest() == HARDWARE_ROW_SHA256
    tape = ROOT / "tmp/wii_runs" / run_id / "telemetry.ndjson"
    if tape.exists():
        raw = tape.read_bytes()
        assert hashlib.sha256(raw).hexdigest() == TAPE_HASHES[run_id]
        assert raw.splitlines(keepends=True)[301] == row
    event = json.loads(row)
    assert event["deaths"] == 0 and event["death_cause"] == "none"
    # Verbatim ce98952_10.09 real-Wii row, SHA-pinned tape above; locate its
    # exact line for the evidence packet. All cases below mutate THAT row.
    print("hardware fixture line 302 (embedded verbatim)")
    cases = [("verbatim dead heartbeat", [row.decode()], "player_died")]
    live = dict(event, player_health=160, d1=[31, 0, 0, 0, 0])
    wire = lambda e: json.dumps(e, separators=(",", ":")) + "\n"
    off_wire = lambda e: wire(dict(event="perf_survival_guard", run_id=e["run_id"],
        clock_hz=60750000, heartbeat_tick=e["tick"],
        record=[1, e["d1"][0], 0, 0, 0, 160, 0, 0, 0, 0] + [0]*36))
    cases.append(("valid local snapshot with explicit OFF state", [off_wire(live), wire(live)], None))
    for label, value, expected in (
        ("missing", None, "death_telemetry_missing"),
        ("invalid", [0, 0, 0, 0, 0], "death_telemetry_unavailable"),
        ("short", [31, 0], "death_telemetry_corrupt"),
        ("boolean", [True, 0, 0, 0, 0], "death_telemetry_corrupt"),
        ("negative", [31, -1, 0, 0, 0], "death_telemetry_corrupt"),
        ("overflow", [31, 2**32, 0, 0, 0], "death_telemetry_corrupt"),
        ("bad cause", [31, 0, 9, 0, 0], "death_telemetry_corrupt"),
        ("cause without death", [31, 0, 1, 0, 0], "death_telemetry_contradictory"),
        ("host death", [31, 1, 1, 0, 0], "player_died"),
        ("guest death", [31, 0, 0, 1, 6], "player_died"),
    ):
        mutated = dict(live)
        if value is None:
            del mutated["d1"]
        else:
            mutated["d1"] = value
        if label == "host death":
            mutated.update(deaths=1, death_cause="fall")
        cases.append((label, [wire(mutated)], expected))
    cases.append(("legacy contradiction", [wire(dict(live, deaths=1))],
                  "death_telemetry_contradictory"))
    for health in (None, True, "160"):
        cases.append((f"corrupt health {health!r}", [wire(dict(live, player_health=health))],
                      "death_telemetry_unavailable"))
    missing_health = dict(live)
    del missing_health["player_health"]
    cases.append(("missing health", [wire(missing_health)], "death_telemetry_unavailable"))
    cases.append(("missed dead heartbeat", [off_wire(live), wire(live), wire(dict(
        live, d1=[31, 1, 6, 0, 0], deaths=1, death_cause="combat"))], "player_died"))
    cases.append(("session changes during coverage", [off_wire(live), wire(live), wire(dict(
        live, d1=[32, 0, 0, 0, 0]))], "death_telemetry_session_changed"))
    failures = []
    for label, rows, expected in cases:
        try:
            check(rows, expected, label)
        except AssertionError as exc:
            failures.append((label, str(exc)))
    assert not failures, failures
    guard_cases()
    guard_interval_cases()
    if len(sys.argv) > 1:
        from g9_x_regression import death_cases
        death_cases(collector, Path(sys.argv[1]))
        # Same producer -> mapping -> formatter as main, linked to real engine.
        output = subprocess.check_output([sys.argv[1]], text=True)
        produced = next(line + "\n" for line in output.splitlines() if line.startswith("{"))
        actual = json.loads(produced)
        assert actual["d1"][1:] == [1, 3, 1, 4]
        assert actual["player_health"] <= 0
        check([produced], "player_died", "real C producer/heartbeat/parser", expected_build=None)
        inventory = subprocess.check_output(["ctest", "--test-dir", str(Path(sys.argv[1]).parent),
            "-N", "-V", "-R", "^test_perf_stress_survival_guard$"], text=True)
        command = re.search(r"Test command: (.+)", inventory)
        assert command, "ctest inventory has no guard producer command"
        guard_exe = Path(command.group(1).strip().strip('"'))
        assert guard_exe.exists(), "real guard producer executable absent"
        telemetry_inventory = subprocess.check_output(["ctest", "--test-dir", str(Path(sys.argv[1]).parent),
            "-N", "-V", "-R", "^test_hw_telemetry$"], text=True)
        telemetry_command = re.search(r"Test command: (.+)", telemetry_inventory)
        assert telemetry_command, "ctest inventory has no telemetry sender command"
        g9_send_cases(Path(telemetry_command.group(1).strip().strip('"')))
        output = subprocess.check_output([str(guard_exe)], text=True)
        rows = [line + "\n" for line in output.splitlines() if line.startswith("{")]
        context_row = next(row for row in rows if json.loads(row).get("event") == "perf_survival_guard")
        heartbeat_row = next(row for row in rows if json.loads(row).get("g1") == 1)
        check([context_row, heartbeat_row], None, "real guarded producer/observer-envelope/heartbeat/parser (noncertifying)", expected_build=None)


if __name__ == "__main__":
    # R18: these hardware tapes hold a private rig IP and stay out of the
    # public export (docs/release-allowlist.json); skip (77) where absent.
    _r18_tapes = ("g9_g9_1_s12345.ndjson.bin", "g9_x_x_g9a_s12345.ndjson.bin",
                  "g9_x_x_g9b_s12345.ndjson.bin", "g9_y_y_g9a_s12345.ndjson.bin",
                  "g9_y_y_g9b_s12345.ndjson.bin", "g9_x_provenance.json", "g9_y_provenance.json")
    _r18_root = __import__("pathlib").Path(__file__).resolve().parent / "fixtures"
    if not all((_r18_root / _t).is_file() for _t in _r18_tapes):
        print("SKIP: private G9 hardware tapes are not in this tree (public export)")
        raise SystemExit(77)
    main()
