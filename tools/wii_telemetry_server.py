#!/usr/bin/env python3
"""Collect CaveWii real-Wii UDP telemetry and classify a hardware proof run."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from collections import defaultdict, deque
from pathlib import Path
from analyze_storage_levers import StorageEvidence, render_storage
from g9_cpu_evidence import G9CpuEvidence, render_g9_cpu, g9_pacing_record
from g9_capture_evidence import G9CaptureEvidence, render_g9_capture


# scenario = the G2 positive gate: a PASS must prove the profile's stressor
# actually ENGAGED, not merely that the console survived the duration.
#   far_ring  -> world loaded AND far ring active (far_view_distance >= 1)
#   far_walk  -> far_ring AND horizontal displacement >= min_walk_blocks
#   redstone  -> world loaded AND the notify-dispatch total ("ndisp") grew past
#                min_ndisp (a running clock dispatches thousands/2min; an idle
#                superflat stays near the rig's build-time constant)
#   menus     -> world loaded AND the menu_autopilot script's monotonic
#                high-water-mark step ("map_max") reached min_map_step on a
#                beat carrying complete np_entered/skipped/focus/flags/rows
#                evidence, graded for observed entries and justified skips --
#                title, texture-packs, select-world, create-world's OSK,
#                pause, and options (from pause) all round-tripped for real.
#                The sign-OSK step is DELIBERATELY excluded from this gate
#                (source/game/menu_autopilot_script.h/.c: it is the first
#                hw_run integration of the aim+place-pulse mechanism and is
#                soft/best-effort by design) -- see map_sign in the PASS/FAIL
#                artifact for whether it was also reached, as bonus evidence.
#   mp_walk   -> world loaded (mp_connect_smoke's own discriminator: the
#                profile arms no autoload, so a loaded world can ONLY be the
#                remote install succeeding) AND horizontal displacement >=
#                min_walk_blocks, same displacement tracking as far_walk but
#                WITHOUT requiring the far ring (mp profiles leave
#                far_view_distance at the server-driven platform default, so
#                fring is never active) -- proves the autopilot walk actually
#                engaged post-connect, driving continuous chunk-stream/
#                install load for the >3s main-loop stall hunt.
#   mp_reconnect -> world loaded (same mp_connect_smoke discriminator) AND
#                the LAST heartbeat's mrs_cycles reached the profile's
#                min_cycles AND no heartbeat ever reported mrs_wedge true --
#                proves N full connect/disconnect round-trips actually
#                completed, not merely that a connection was ever made once
#                (source/game/mp_reconnect_soak_core.h). A wedge (a phase
#                that exceeded its timeout) is a hard FAIL even if enough
#                cycles were seen before it happened -- "no wedge" is a PASS
#                requirement on its own, not something more cycles offset.
# ---- the menus_dwell STEP CONTRACT --------------------------------------
# min_map_step is an ORDINAL out of menu_autopilot_script.c's `enum { ... }`
# step table: the value menu_ap_script_step_required_for_pass() returns
# (STEP_NP_LEG_DONE). Inserting a step anywhere above it shifts it, and a
# stale value here is a silently LOOSER `>=` gate -- a run that never finished
# the script still passes. That has now happened twice (39 -> 47 -> 48 while
# the real ordinal was already higher), so the number is VERSIONED like the
# chat-chord contract above: every superseded value is named here with the
# reason it is spent, and a profile still carrying one is REFUSED instead of
# quietly grading a run against it. Recompute by hand from the enum -- never
# "the old number plus however many steps you added" -- and pin it with
# tests/test_menus_dwell_contract.c, which reads this file and the C function.
MENUS_DWELL_STEP_SPENT = {
    39: ("the pre-2026-09-02 ordinal: it predates STEP_CWORLD_MODE_FAILED, so "
         "a run graded against it could stop 8 steps early and still pass"),
    47: ("drifted stale before the 2026-09-02 fix and was never corrected in "
         "place; superseded by 48, which was itself already stale"),
    48: ("the 2026-09-02 value, left behind by the 2026-09-11 inventory and "
         "container legs (+49 steps) and again by the b1-chat-opener-0919/16 "
         "inventory HOLD leg (+3): it lets a run that quit at the sign leg "
         "pass the menus gate outright"),
    100: ("the b1-chat-opener-0919 value: it predates the pointer-SUPPRESSED "
          "front-end leg of push-0921 (steps 107..138: Select World Create / "
          "Rename / Delete, Texture Packs, Multiplayer Direct / Add / Edit / "
          "name editor, all by direction + activate + Close with the pointer "
          "unavailable), so a run graded against it proves nothing about "
          "button-only reachability"),
    138: ("the push-0921/A11..A14 value, left behind by A15's STEP_NP_SUB_TO_"
          "STRIP_DOWN (+1): the sub-leg walk now enters an action strip with "
          "ONE GUI RIGHT off the current row and keeps A14's DOWN converge as "
          "a named fallback step, so a run graded against 138 could stop one "
          "step short of the leg's terminal ordinal and still pass"),
    139: ("the T03/T04 value, left behind by T15 (+20: Tutorial row visit, "
          "Texture packs one-click pick, Create World Details/name OSK/seed "
          "keypad sub-legs, Inventory/Crafting tap + tab); a run graded "
          "against 139 stops 20 steps short of the no-pointer leg's terminal "
          "ordinal"),
    159: ("the T16 value, left behind by hw1030i's Texture Packs scroll "
          "(+2: STEP_NP_TEXPACKS_SCROLL / _SCROLL_DWELL, D-pad DOWN to the "
          "last pack row and a capture dwell there); a run graded against "
          "159 stops 2 steps short of the no-pointer leg's terminal ordinal"),
}
# Hand-counted hw1030i: title/packs 0..23, Select/Create 24..48, world/sign
# 49..58, inventory 59..76, containers 77..107, pause/quit 108..126,
# no-pointer 127..161, STEP_DONE 162, STEP_COUNT 163. The gate is
# STEP_NP_LEG_DONE, not DONE.
MENUS_DWELL_STEP_CONTRACT = 161

# ---- the WALKER PROOF fields (push-0921 A11, review A9-3) ----------------
# min_map_step is a HIGH-WATER MARK: the no-pointer leg reaches its terminal
# step whether it ENTERED all seven of its targets or named all seven as
# skips, so the ordinal alone certifies nothing about button-only
# reachability. Every heartbeat now also carries np_entered / np_skipped /
# np_focus (bitmasks over the table below), np_flags and np_rows -- see
# source/game/menu_autopilot_script.h, which is where the bit numbering is
# DEFINED; this is the mirror, pinned by the selftest below.
NP_SUB = {
    "create": 1 << 0,   # Select World -> Create New World
    "rename": 1 << 1,   # Select World -> Rename        (needs a world row)
    "delete": 1 << 2,   # Select World -> Delete        (needs a world row)
    "direct": 1 << 3,   # Multiplayer  -> Direct Connect
    "add": 1 << 4,      # Multiplayer  -> Add Server
    "edit": 1 << 5,     # Multiplayer  -> Edit          (needs a server row)
    "name": 1 << 6,     # Multiplayer  -> player-name editor
}
# The OSK-backed targets: their Close is owned by a real keyboard wherever
# input_text_active() can be true. Expected to be skipped on PC, and a DEFECT
# on the console, where input_text_active() is a hard false.
NP_OSK_BACKED = (NP_SUB["rename"] | NP_SUB["direct"] | NP_SUB["add"]
                 | NP_SUB["edit"] | NP_SUB["name"])
# Always-enabled targets: no list row gates them, so a console run that did
# not ENTER every one of these has not proven what this profile exists for.
NP_REQUIRED_ENTERED = (NP_SUB["create"] | NP_SUB["direct"] | NP_SUB["add"]
                       | NP_SUB["name"])
# Row-gated targets, and which count has to be zero for a skip to be the
# screen behaving correctly rather than a walker that could not reach them.
NP_ROW_GATED = {
    "rename": NP_SUB["rename"],
    "delete": NP_SUB["delete"],
}
NP_FLAG_NAMES = {
    1 << 0: "the Multiplayer focus/view observers were never registered",
    1 << 1: "an OSK-backed screen was reached but not entered",
    1 << 2: "the world count changed across the delete confirm or the rename editor",
    1 << 3: "the delete confirmation did not open on its safe answer",
    1 << 4: "the leg abandoned",
    # push-0921 A14. `debug.menu_ap_start_np` starts the walker AT the
    # no-pointer leg so the leg can be exercised against the real screens in
    # the PC app (the PC app cannot otherwise reach it -- it parks in the
    # create-world OSK). Such a run skipped everything before the leg, so its
    # map_max is a high-water mark of the leg alone: it must never grade as a
    # console PASS, and this bit -- raised by the walker from its very first
    # tick -- is how the tape says so.
    1 << 5: "the PC entry lever started the walker at the no-pointer leg, "
            "so nothing before the leg ran",
    # push-0921 A17 (review A16-3). The walk enters an action strip with ONE
    # GUI RIGHT off the current row; A14's DOWN converge survives as the
    # fallback for a card whose screens predate that entry. The fallback keeps
    # the leg ALIVE on such a card -- which is why it exists -- but the run
    # must not then CERTIFY the entry: max_step is identical either way (the
    # fallback step is numbered below the clamp) and the only other difference
    # is a press count no heartbeat carries. Refused like every other flag.
    1 << 6: "a sub-leg reached the action strip by the DOWN-converge fallback, "
            "so the RIGHT entry this card is meant to certify did not take",
}

from split_offer_piston_evidence import SplitOfferPistonEvidence, PROFILE as OFFER_PISTON_PROFILE
from save_reload_evidence import SaveReloadEvidence, PROFILE as SAVE_RELOAD_PROFILE

VEHICLE_SMOKE_PROFILE = "host_vehicle_smoke_8m"
PROFILES = {
    OFFER_PISTON_PROFILE: {"duration": 120, "min_hb": 10, "expect_dump": False, "scenario": None},
    VEHICLE_SMOKE_PROFILE: {"duration": 480, "min_hb": 10,
                            "expect_dump": False, "scenario": None},
    # duration 100, not 60 (2026-08-19 quiesce recalibration): the window
    # starts at CONNECT (~28s before beat 1: stage + boot + fixture load),
    # and since df84d49 the game goes SILENT at profile_done, so the
    # trailing beats that used to pad this gate no longer exist -- a 60s
    # window deterministically tops out at ~32 beats (measured 31/32 across
    # two real runs). 100s fits 50+ with margin; min_hb unchanged, the gate
    # only got MORE observed data. Stage-side --cw-duration is 100 to match.
    "heartbeat_selftest": {"duration": 100, "min_hb": 50, "expect_dump": False,
                           "scenario": None},
    # duration 150, not 120 (2026-08-18 hardware calibration): the collection
    # window starts at CONNECT, and this fixture world spends ~28s loading
    # before heartbeat 1 -- a 120s window tops out at ~92 beats, below
    # min_hb=100 (measured 92/91/92 across three real runs). 150s fits the
    # full 100+ with margin; min_hb is unchanged, so the gate only got MORE
    # observed data. Stage-side --cw-duration is 150 to outlive this window.
    "far_idle_2m": {"duration": 150, "min_hb": 100, "expect_dump": False,
                    "scenario": "far_ring"},
    "far_walk_6m": {"duration": 360, "min_hb": 300, "expect_dump": False,
                    "scenario": "far_walk", "min_walk_blocks": 32.0},
    "perf_stress_5m": {"duration": 300, "min_hb": 240, "expect_dump": False,
                       "scenario": "far_ring"},
    "watchdog_inject": {"duration": 30, "min_hb": 5, "expect_dump": True,
                        "scenario": None},
    # 150 for the same connect-window/load-lag reason as far_idle_2m above.
    "redstone_clock_2m": {"duration": 150, "min_hb": 100, "expect_dump": False,
                          "scenario": "redstone", "min_ndisp": 200},
    # motion-proof-0927 (lane C, mob motion units S2): the mob motion fixture's
    # three recorded checks (TNT lift, Beta dry recurrence, path step jump)
    # arrive as mm_* heartbeat keys; the gate is _note_mob_motion_evidence.
    # 150 for the same connect-window/load-lag reason as far_idle_2m.
    "mob_motion_2m": {"duration": 150, "min_hb": 100, "expect_dump": False,
                      "scenario": "mob_motion"},
    # colcap-1006 (ASTRA-CC-R5 item 2): the column-pool T1 proof on the Wii
    # (source/game/pool_selftest.c). Its own events carry the verdict
    # (PoolSelftestEvidence); the self-test completes the run (170 s cap in a
    # 180 s device window). Two runs, one per --cw-pool-placement, are then
    # compared with --gx-compare.
    "pool_selftest_3m": {"duration": 210, "min_hb": 30, "expect_dump": False,
                         "scenario": "pool_selftest"},
    # SPEC R12 (tools/save_reload_evidence.py): fresh seeded world, fixture,
    # real pause Save and quit, named reload of the same folder, hashes equal.
    # The driver completes the run (170 s cap in a 180 s device window); 210
    # covers the connect-window lag like pool_selftest_3m.
    SAVE_RELOAD_PROFILE: {"duration": 210, "min_hb": 30, "expect_dump": False,
                          "scenario": "save_reload"},
    "menus_dwell": {"duration": 240, "min_hb": 200, "expect_dump": False,
                    "scenario": "menus",
                    "min_map_step": MENUS_DWELL_STEP_CONTRACT},
    # menus_dwell_creative (2026-09-02, --cw-cworld-mode creative /
    # HW_RUN_PROFILE_MENUS_DWELL_CREATIVE, source/game/hw_run.h): the SAME
    # script/duration/min_map_step as menus_dwell above -- the only delta is
    # require_creative, which adds one more scenario-gate condition below
    # (self.creative_seen): the run must have observed the heartbeat's
    # `creative` field true at least once in-world, proving the knob
    # actually landed a Creative world instead of silently creating
    # Survival like every other menus_dwell run would (the exact failure
    # mode staged_menus_dwell_20260902_063552 hit by accident).
    "menus_dwell_creative": {"duration": 240, "min_hb": 200, "expect_dump": False,
                             "scenario": "menus",
                             "min_map_step": MENUS_DWELL_STEP_CONTRACT,
                             "require_creative": True},
    # S5.3: the Wii joins a LAN mc173 server hands-free. The profile arms NO
    # autoload and boots into the connect screen, so world_loaded_seen (the
    # generic scenario gate below) can ONLY come from the remote world
    # install -- connect + login + chunk stream all succeeded. min_hb 60 of
    # a 120s window leaves room for the ~25s measured DHCP settle before
    # heartbeats start flowing from a loaded world.
    "mp_connect_smoke": {"duration": 120, "min_hb": 60, "expect_dump": False,
                         "scenario": "mp_connect"},
    # mp_lan_walk_10m (S5.3-era hunt profile, PROJECT_MEMORY): the maintainer's
    # 2026-08-30 field session fired the watchdog banner (a real >3s main-loop
    # stall) under walking/chunk churn that mp_connect_smoke's short idle-at-
    # spawn window never reproduces. Arms mp_connect_smoke's connect flow
    # (world_loaded_seen can ONLY come from the remote install -- same
    # discriminator) PLUS far_walk_6m's forced autopilot walk, for the whole
    # 600s window. Un-padded like far_walk_6m/mp_connect_smoke themselves
    # (duration matches hw_run.c's HW_RUN_PROFILE_MP_LAN_WALK_10M default and
    # stage_wii_dol.ps1's --cw-duration exactly). min_hb=480: the ~25-30s
    # measured DHCP/connect/login/chunk-stream settle (mp_connect_smoke's own
    # figure) leaves ~570s of walk-under-load at ~1 heartbeat/s; 480 keeps
    # margin below that without weakening the gate to the default of 1.
    "mp_lan_walk_10m": {"duration": 600, "min_hb": 480, "expect_dump": False,
                        "scenario": "mp_walk", "min_walk_blocks": 32.0},
    # mp_reconnect_soak (S5.3 hunt plumbing, docs/mp_s5_wii_transport_design_
    # 2026-08-26.md 7b.3's "reconnect-cycle soak" -- the design doc's own
    # "100 connect/disconnect cycles" S4 gate, ported to hardware at a
    # smaller count first). Repeated connect/dwell/disconnect/reconnect
    # cycles catch the LWP-thread-bookkeeping and MEM2-margin accumulation
    # risks that only surface ACROSS reconnects, never a single session.
    # min_hb=400 of a 480s window: heartbeats emit ~1/s regardless of MP
    # connect state (main.c's emit_hw_heartbeat is gated on hw_run.enabled
    # only, not on world_loaded), so this leaves ~80s of margin for the
    # first cycle's DHCP/connect settle plus normal scheduling jitter --
    # the same ~83% ratio far_walk_6m/mp_lan_walk_10m already use.
    # min_cycles=6 matches source/game/mp_reconnect_soak_core.h's
    # MP_RECONNECT_SOAK_CYCLES_TARGET; keep both in step if that ever moves.
    "mp_reconnect_soak": {"duration": 480, "min_hb": 400, "expect_dump": False,
                          "scenario": "mp_reconnect", "min_cycles": 6},
    # mp_chat_login_3m (2026-09-17, b1-chat-keyboard-0917): the scripted OSK
    # chat-login proof -- a virtual Nunchuk types `/login hunter22` on the real
    # console and the LAN server acknowledges it. Contract
    # tmp/jobs/b1-chat-keyboard-0917/b1c-contract-v1-ruled.md (sections 3, 5, 6
    # and the section 8 rulings).
    #   * scenario "mp_connect": the base run is mp_connect_smoke (hw_run.c's
    #     mp_autoconnect with NO autoload), so a loaded world can only be the
    #     remote install succeeding -- the same discriminator that profile uses,
    #     and the only heartbeat-side gate this profile wants (it deliberately
    #     stands still, so no displacement gate can apply).
    #   * duration 180 matches the contract's total cap (section 3: the
    #     per-phase budgets sum past it, so the TOTAL is binding).
    #   * min_hb 60 is a LIVENESS DEFAULT only, for a run that produced no
    #     marker at all. The driver ends the run the moment P6 verifies, which
    #     on a healthy run happens well inside 180 s, so a fixed floor would
    #     FAIL honest fast runs (the split_recovery_6m lesson, 2026-09-17).
    #     The binding floor is ChatLoginEvidence.heartbeat_floor()'s CADENCE
    #     rule over the driver's own clock; the marker contract plus the
    #     server receipt are what actually certify the run.
    "mp_chat_login_3m": {"duration": 180, "min_hb": 60, "expect_dump": False,
                         "scenario": "mp_connect"},
    # mp_chat_chord_1m (2026-09-19, b1-chat-opener-0919, Astra ruling 2 D2):
    # the SEPARATE debug-chord leg. Same mp_connect base and the same
    # marker-is-the-verdict rule as the profile above, at 60 s. It exists so a
    # chord claim can never be read out of a login run: the two profiles have
    # different event names, different marker sets and their own contracts.
    "mp_chat_chord_1m": {"duration": 60, "min_hb": 20, "expect_dump": False,
                         "scenario": "mp_connect"},
    # mp_play_10m (2026-09-21, push-0921 lane B, Astra gate 1): the unattended
    # LAN PLAY proof of release blocker D1 -- join, typed login, >= 600 000 ms
    # of measured play with server-matched digs / places / inventory transfers,
    # a clean disconnect through the pause menu, a rejoin through the menus,
    # server-owned state verified. Contract docs/mp_play_profile.md.
    #   * scenario "mp_connect", NOT "mp_walk": the base run is mp_connect_smoke
    #     (no autoload), so world_loaded_seen can only be the remote install
    #     succeeding -- the same discriminator every mp_* profile uses. The
    #     displacement claim deliberately does NOT ride on the heartbeat-side
    #     walk gate: Astra answer B2 defines it as SERVER-observed distance with
    #     teleports excluded, which is a fixture-receipt measurement
    #     (MpPlayEvidence.displacement), and a second client-side gate would
    #     both double-count and fail honest runs -- this profile's walk is
    #     interrupted by a build leg roughly every 20 s (MPP_WALK_LEG_MS).
    #   * duration 1260 is THIS TABLE'S RUN TIMEOUT -- the collector's own
    #     wall clock (RunState._tick, "duration %ds reached"), the moment it
    #     stops listening and classifies a driver-owned run that never sent
    #     profile_done. It is NOT the heartbeat floor (that is min_hb below /
    #     MpPlayEvidence.heartbeat_floor()) and it is not display-only.
    #     1260 = hw_run.c's cfg->duration_seconds for this profile, which is
    #     deliberately 60 s LONGER than mp_play_core.h's MPP_TOTAL_CAP_MS
    #     (1 200 000 ms) so the driver's own named failure fires INSIDE the
    #     window instead of being cut off with no verdict (hw_run.c:490-517).
    #     A collector timeout shorter than the device's clock would throw that
    #     verdict away, so the two must stay equal. Reconciled 2026-09-21
    #     (push-0921/B6): this table and the two ps1 tables said 1200 while
    #     the device profile said 1260. A legitimate run (~13 min) reaches
    #     neither; the driver owns completion (profile_done).
    #   * min_hb 480 is the TABLE default for a run that produced no marker at
    #     all. The binding floor is MpPlayEvidence.heartbeat_floor()'s cadence
    #     over the run's OWN measured clock -- see that method. 480 is 0.8
    #     beats/s across the 600 000 ms measured floor, the same ratio
    #     mp_lan_walk_10m uses for its own 600 s window.
    "mp_play_10m": {"duration": 1260, "min_hb": 480, "expect_dump": False,
                    "scenario": "mp_connect"},
    "mp_pause_menu_3m": {"duration": 180, "min_hb": 5,
                          "expect_dump": False, "scenario": "mp_connect"},
    # First-run hard gate: the whole hands-free fixture/reopen/verify scenario
    # must finish inside 300s. The migration's own smig_ms is retained exactly
    # and may not exceed that window; a timeout is an SD-I/O finding, not a
    # threshold-calibration excuse.
    # world_copy_convert (ui-build-1026/T16a): the "Back up & convert" proof
    # route. The in-run gate only requires the walker's wcc_end; the VERDICT
    # is the offline --world-copy-verdict (device tape + card record), which
    # fails closed -- see WORLD_COPY_MARKERS and T16a-RIG.md.
    "world_copy_convert": {"duration": 300, "min_hb": 5, "expect_dump": False,
                           "scenario": "world_copy_convert"},
    "save_migrate": {"duration": 300, "min_hb": 5, "expect_dump": False,
                     "scenario": "save_migrate", "min_regions": 8,
                     "min_signs": 32},
    # keepup_walk_6m (2026-09-04, deterministic keep-up profile): SAME
    # duration/min_hb/scenario/gate as far_walk_6m -- this profile exists so
    # two BUILDS can be A/B'd on tris/chunks_meshed under the same chunk-load
    # pressure envelope, which requires an identical pass/fail bar, not a
    # looser one. The only thing that changes is the walk driver itself
    # (source/game/autopilot_keepup.h): a fixed heading with a bounded,
    # deterministic sidestep-only stuck escape, instead of far_walk_6m's
    # never-resetting curving-yaw drift (autopilot.c mode 0) -- see that
    # header's top comment for the RCA of far_walk_6m's measured 24/67/82
    # block run-to-run displacement spread (PROJECT_MEMORY, 2026-09-04).
    "split_anchor_6m": {"duration": 360, "min_hb": 300, "expect_dump": False,
                        "scenario": "split_anchor"},
    # split_play_2m (2026-09-07, lane/guest-body): the first profile with a
    # REAL second gameplay body instead of split_anchor_6m's synthetic +X
    # anchor. Same anchor scenario/evidence shape and the same readiness
    # gate; a shorter 120 s measured window because the guest Join
    # handshake happens INSIDE it. Keep in step with hw_run.c.
    "split_play_2m": {"duration": 120, "min_hb": 100, "expect_dump": False,
                      "scenario": "split_anchor"},
    # split_play_8m (2026-09-10, Astra ruling): the CERTIFYING guest-body
    # profile -- six measured minutes on the readiness epoch (like
    # split_anchor_6m) after a 120 s installed-readiness budget. The 2m
    # profile is the smoke: readiness by 90 s and >= 30 s measured inside
    # its 120 s run clock. Keep in step with hw_run.c.
    # min_hb 330 (was 400): the window is 360 s on the READINESS epoch, so a
    # fast readiness (32 s in stg25) gives ~392 heartbeats; the floor must not
    # punish a fast load. split_anchor_6m uses 300 for the same reason.
    "split_play_8m": {"duration": 360, "min_hb": 330, "expect_dump": False,
                      "scenario": "split_anchor"},
    # split_actions_6m (2026-09-14, mp-split-0912/68 S1-d): the per-owner
    # ACTION proof. Identical window, scenario, readiness budget and safety
    # checks to split_play_8m -- the difference is entirely in what it
    # REQUIRES at the end: the guest body's accepted dig/place/hotbar/use
    # tallies must clear their floors while the host's are all zero. Astra
    # gate-3 asked for a profile that FAILS when the expected per-owner
    # effects are absent, and a shared-with-split_play_8m threshold set would
    # not do that. Keep in step with hw_run.c.
    "split_actions_6m": {"duration": 360, "min_hb": 330, "expect_dump": False,
                         "scenario": "split_anchor"},
    # split_recovery_6m (2026-09-17, robot-recovery-0917): the scripted
    # split-screen RECOVERY proxy -- controller loss/recovery, leave/rejoin and
    # save-and-quit/reload, contract docs/split_recovery_profile.md. It shares
    # the guest-body base (split_guest + superflat + force_overworld) and
    # therefore the readiness/guest-admitted gates, but NOT split_actions_6m's
    # thresholds:
    #   * scenario "split_recovery", not "split_anchor": the contract (section
    #     1, "walk: none") deliberately keeps BOTH bodies still so the position
    #     baselines mean something, so the split_anchor displacement gate would
    #     be an unprovable, permanently-failing check here. The scenario is
    #     still non-None, so the generic "world never reported loaded" gate
    #     applies.
    #   * min_hb 60, not 330: this profile is NOT duration-certifying. The
    #     driver sends profile_done the moment P4 verifies (contract section
    #     6), which on a healthy run happens well inside the 360 s window, so a
    #     six-minute heartbeat floor would FAIL honest fast runs. 60 is a
    #     liveness floor -- "the console was alive for a minute" -- and the
    #     marker contract below is what actually certifies the run.
    # duration 360 matches the contract's harness table; completion comes from
    # profile_done (SPLIT_PROFILES membership disables the generic duration
    # completion in tick()), never from the clock.
    "split_recovery_6m": {"duration": 360, "min_hb": 60, "expect_dump": False,
                          "scenario": "split_recovery"},
    "split_portal_quit_4m": {"duration": 240, "min_hb": 20,
                               "expect_dump": False,
                               "scenario": "split_recovery"},
    # split_dims_6m (splitdims-profile-0926): the host stands, the guest walks
    # through a portal (no phase driver, no still-body baselines), so neither
    # the split_anchor displacement gate nor the recovery contract applies;
    # the certifying evidence is SplitDimsEvidence + the anchor/TPS gates.
    "split_dims_6m": {"duration": 360, "min_hb": 300,
                      "expect_dump": False, "scenario": "split_dims"},
    "keepup_walk_6m": {"duration": 360, "min_hb": 300, "expect_dump": False,
                       "scenario": "far_walk", "min_walk_blocks": 32.0},
    # colcap-1006 (ASTRA-CC-R5 item 3): split_anchor_6m's walk with the
    # synthetic anchor separated / reunited / separated again on the
    # readiness clock; RegrowEvidence certifies drain AND warmed 7/8-slab
    # regrowth and reports the allocator CPU/lock cost ("pac").
    "split_regrow_8m": {"duration": 360, "min_hb": 300, "expect_dump": False,
                        "scenario": "split_regrow"},
}


# TPS measurement starts at first world_loaded + this post-load settle and
# continues to profile completion (including later menu/reconnect work).
# Load/DHCP allowances above are pre-load, not additional TPS settle periods.
# Keep HW_RUN_TPS_SETTLE_MS in source/game/hw_run.h synchronized.
# Every profile that carries the two-anchor evidence shape. The certifying
# ones measure six minutes on the readiness epoch; the 2m smoke has its own
# completion check (AnchorEvidence.complete_error).
SPLIT_PROFILES = ("split_anchor_6m", "split_play_2m", "split_play_8m",
                  "split_actions_6m", "split_recovery_6m",
                  "split_portal_quit_4m", "split_dims_6m", "split_regrow_8m")
# The ANCHOR observer (two residency anchors, readiness latch, measured window,
# per-frame evidence) is what the device emits for the split PERFORMANCE/
# residency profiles. split_recovery_6m is a FUNCTIONAL profile: the device arms
# no anchor observer for it and its heartbeats carry no anchor_* fields (real
# Wii, 878619c_09.17 run 0917_091521: 35 of 35 markers ok, verdict "missing
# anchor evidence"). Its certifying evidence is the marker contract alone, so
# every anchor rule is keyed on ANCHOR_PROFILES; SPLIT_PROFILES still governs
# completion-by-profile_done and the TPS settle exemption.
# split_regrow_8m (colcap-1006 R5 item 3) moves its synthetic anchor on
# purpose (separate / reunite / separate), so the anchor completion rules do
# not apply; RegrowEvidence grades it (the same pool reconstruction inside).
ANCHOR_PROFILES = tuple(p for p in SPLIT_PROFILES
                        if p not in ("split_recovery_6m", "split_portal_quit_4m",
                                     "split_regrow_8m"))
# TPS/performance-certifying members. split_recovery_6m is deliberately NOT
# here: it proves state machines, not throughput.
SPLIT_CERTIFYING = ("split_anchor_6m", "split_play_8m", "split_actions_6m",
                   "split_dims_6m")
# Guest-body profiles: two REAL bodies, so both anchors must reach installed
# readiness even though anchor_sep (the synthetic offset) is 0.
SPLIT_GUEST = ("split_play_2m", "split_play_8m", "split_actions_6m",
               "split_recovery_6m", "split_portal_quit_4m", "split_dims_6m")
# Installed-readiness budget in heartbeats (~1 s each) per profile.
# split_recovery_6m takes the same 120 s as its guest-body siblings: its P0
# phase timeout is 90 s from world-loaded (contract section 8) and installed
# readiness (three settled anchor heartbeats) can only lag that.
SPLIT_READY_BUDGET = {"split_play_2m": 90, "split_play_8m": 120,
                      "split_actions_6m": 120, "split_recovery_6m": 120,
                      "split_portal_quit_4m": 120, "split_dims_6m": 120}
# S1-d (mp-split-0912/68). The ONE profile that judges per-owner AGENCY rather
# than residency, and the wire order of the two act arrays the device emits.
SPLIT_ACTION_PROFILE = "split_actions_6m"
SPLIT_ACTION_NAMES = ("dig", "place", "attack", "use", "hotbar", "drop")
# What the scripted guest sequence (main.c SPLIT_ACTION_SCRIPT) must have
# achieved by the end of the measured window. One cycle of the script is five
# steps of 40 client ticks = 10 s, so a six-minute window offers ~36 cycles --
# these floors are a small multiple of ONE cycle, not a performance bar: they
# exist to separate "the guest acted" from "the guest never acted", which is
# the failure the profile is built to catch.
#
# ATTACK is deliberately absent. It is REPORTED in the summary but never
# required: no mob is guaranteed to be within reach of a scripted walker, and
# a threshold that depends on the world rolling a creeper would fail honest
# runs. Nothing else in this table is world-dependent -- dig/place/hotbar/use
# are all self-supplied by the script.
SPLIT_ACTION_FLOORS = {"dig": 3, "place": 3, "hotbar": 3, "use": 1}

# --- split_recovery_6m: the marker contract (docs/split_recovery_profile.md) --
# The ONE profile whose verdict is a MARKER SEQUENCE rather than a counter or a
# duration: the driver narrates the three split-screen transitions phase by
# phase and the collector re-derives, from those markers alone, that each
# production state machine really did what the contract says. Section numbers
# below refer to that document.
SPLIT_RECOVERY_PROFILE = "split_recovery_6m"
# Section 0 / Astra ruling item 8: the wording of any PASS this profile
# produces is fixed, because a scripted loss is not a physical unplug.
SPLIT_RECOVERY_PASS_LABEL = ("scripted real-Wii proxy PASS; physical unplug "
                             "still owed (checklist items 3-4)")
# Section 5, in emission order: (phase, marker, times it must appear).
# The tuple is BOTH the mandatory set and the canonical order -- a marker's
# index here is its ordinal, and the delivered ordinals must never decrease.
# `baseline` and `pre_save_baseline` appear twice ("one per actor", section 5),
# `block_placed`/`blocks_verified` three times (i = 0, 1, 2).
SPLIT_RECOVERY_MARKERS = (
    ("p0", "guest_admitted", 1),
    ("p0", "block_placed", 3),
    ("p0", "baseline", 2),
    ("p1", "loss_injected", 1),
    ("p1", "no_leave", 1),
    ("p1", "ticks_advanced", 1),
    ("p1", "collapse_begin", 1),
    ("p1", "collapse_done", 1),
    ("p1", "no_auto_resume", 1),
    ("p1", "resumed", 1),
    ("p1", "view_restored", 1),
    ("p1", "inventory_match", 1),
    ("p2", "leave_requested", 1),
    ("p2", "leave_acked", 1),
    ("p2", "guest_retired", 1),
    ("p2", "host_only_layout", 1),
    ("p2", "rejoined", 1),
    ("p2", "inventory_match", 1),
    ("p3", "pre_save_baseline", 2),
    ("p3", "exit_requested", 1),
    ("p3", "world_reset", 1),
    ("p3", "title_entered", 1),
    ("p4", "autoload_armed", 1),
    ("p4", "world_loaded", 1),
    ("p4", "host_verified", 1),
    ("p4", "blocks_verified", 3),
    ("p4", "guest_readmitted", 1),
    ("p4", "guest_verified", 1),
    ("run", "ok", 1),
)
SPLIT_RECOVERY_EXPECTED = {(p, m): n for p, m, n in SPLIT_RECOVERY_MARKERS}
SPLIT_RECOVERY_ORDINAL = {(p, m): i
                          for i, (p, m, _n) in enumerate(SPLIT_RECOVERY_MARKERS)}
# Markers that may only be emitted AFTER another one. Each entry is an
# assertion the contract makes in prose, so its absence is CONTRADICTORY
# evidence (a state machine that skipped a step) rather than a mere reorder:
#   p3 title_entered  -- section 5 P3, "Ordering is an assertion, not a note"
#   every p4 marker   -- section 5 P4 entry: "world_reset observed AND
#                        title_entered observed, in that order"
#   the rest          -- each phase's own request/observation pairing
SPLIT_RECOVERY_PREREQ = {
    ("p1", "resumed"): ("p1", "collapse_done"),
    ("p1", "view_restored"): ("p1", "resumed"),
    ("p2", "leave_acked"): ("p2", "leave_requested"),
    ("p2", "rejoined"): ("p2", "guest_retired"),
    ("p3", "world_reset"): ("p3", "exit_requested"),
    ("p3", "title_entered"): ("p3", "world_reset"),
}
for _p, _m, _n in SPLIT_RECOVERY_MARKERS:
    if _p == "p4":
        SPLIT_RECOVERY_PREREQ.setdefault((_p, _m), ("p3", "title_entered"))
# Section 5: "corr is this marker's own seq" for the two REQUEST markers, and
# section 4: every other corr is either 0 or the seq of the request it answers.
SPLIT_RECOVERY_SELF_CORR = (("p2", "leave_requested"), ("p3", "exit_requested"))
SPLIT_RECOVERY_ANSWERS = {("p2", "leave_acked"): ("p2", "leave_requested"),
                          ("p3", "world_reset"): ("p3", "exit_requested")}
# Section 5 P1 "Timing tolerance": SPLIT_LAYOUT_GRACE_SECONDS 5.0 and
# SPLIT_LAYOUT_TRANSITION_SECONDS 0.35 observed at a 20 fps floor (10 frames of
# 50 ms). Section 7 makes a reading outside these windows contradictory.
# Lower bounds allow ONE 20 fps frame of sampling quantization (real Wii run
# 0917_092937 read a correct 350 ms ease as 349 ms); the driver applies the
# exact per-window frame time, the collector cannot see it on a passing marker.
SPLIT_RECOVERY_GRACE_MS = (4950, 5500)
SPLIT_RECOVERY_TRANSITION_MS = (300, 850)

# --- mp_chat_login_3m: the OSK chat-login marker + receipt contract ----------
# tmp/jobs/b1-chat-keyboard-0917/b1c-contract-v1-ruled.md, sections 3 (markers),
# 5 (this evidence class), 6 (the server receipt) and 8 (the lead's rulings).
# The SECOND marker-graded profile after split_recovery_6m: the scripted driver
# narrates one `/login hunter22` submission phase by phase, and the collector
# re-derives from those markers ALONE -- plus the server's own receipt -- that
# the command really travelled console -> LAN -> server. Nothing here comes
# from the clock or from a counter that a merely-alive console would produce.
CHAT_LOGIN_PROFILE = "mp_chat_login_3m"
# Section 0 / ruling 7 fixes what a PASS here is worth, and ruling 6 requires
# the two things this profile does NOT exercise to ride along in the label.
CHAT_LOGIN_PASS_LABEL = (
    "scripted real-Wii OSK command delivery on LAN: PASS; three-controller, "
    "ownership/reconnect, human timing, TV readability and BetaMC gates stay "
    "open, and the IR pointer and the PC text queue remain OWED -- B1 is not "
    "Done")
# Section 2/3: the draft the route types, and the argument the server hashes.
CHAT_LOGIN_DRAFT = "/login hunter22"
CHAT_LOGIN_DRAFT_LEN = 15
CHAT_LOGIN_SECRET = b"hunter22"
CHAT_LOGIN_SECRET_LEN = 8
# Section 3, in emission order: (phase, marker, times it must appear). The
# tuple is BOTH the mandatory set and the canonical order, exactly as
# SPLIT_RECOVERY_MARKERS is. The two `trace` members are the open-ended ones
# (None = "one or more"): section 3 emits one batch per <=16 rendered frames
# and the result markers close their own legs, so trace batches INTERLEAVE with
# them and are exempt from the marker-ORDER check -- their PHASE order is still
# enforced, which is what keeps p3.trace inside P3 (section 9 ruling 6).
# p4.key is 53: one marker per REQUESTED press in P4 -- 37 directional + 1
# GUI_PAGE + 15 clicks (section 9 ruling 1 corrects section 5's "52"; the page
# press is a press).
CHAT_LOGIN_MARKERS = (
    ("p0", "session", 1),
    ("p1", "pad_bound", 1),
    ("p1", "chord_refused", 1),
    ("p1", "chat_open", 1),
    ("p2", "trace", None),
    ("p2", "hold_result", 1),
    ("p2", "tap_result", 1),
    ("p2", "page_hold", 1),
    ("p3", "trace", None),
    ("p3", "frozen", 1),
    ("p3", "rearmed", 1),
    ("p4", "origin", 1),
    ("p4", "key", 53),
    ("p4", "draft", 1),
    ("p5", "sent_requested", 1),
    ("p5", "composer_cleared", 1),
    ("p5", "lines_sent", 1),
    ("p6", "ack", 1),
    ("run", "ok", 1),
)
CHAT_LOGIN_EXPECTED = {(p, m): n for p, m, n in CHAT_LOGIN_MARKERS}
CHAT_LOGIN_ORDINAL = {(p, m): i for i, (p, m, _n) in enumerate(CHAT_LOGIN_MARKERS)}
CHAT_LOGIN_KEY_PRESSES = CHAT_LOGIN_EXPECTED[("p4", "key")]
# --- contract 2 (b1-chat-opener-0919, Astra ruling 2 D2) ---------------------
# The opener moved off D-pad Up (maintainer console report 2026-09-19), so P1's
# refusal leg no longer asks for the Up+Left CHORD: it holds the Wii opener
# (Remote button 2) past its 500 ms tap window and the gate refuses THAT. Same
# leg, same section 11 M4 fields, different physical claim -- and Astra's
# ruling is blunt about it: "an 800 ms refusal must never be presented as
# hardware chord proof". So the marker is RENAMED rather than redefined:
#
#     contract 1 (absent `contract` key):  p1.chord_refused  -- Up+Left chord
#     contract 2 (`p0.session.contract`):  p1.hold_refused   -- overlong hold
#
# p0.session's `contract` integer is the version key, and the two marker sets
# are mutually exclusive: a contract-2 run emitting chord_refused, or an old
# run directory emitting hold_refused, is rejected as a marker that is not in
# its own contract. Old run directories keep passing under contract 1, exactly
# as they were graded when they were recorded.
# The opener tap window the Wii gesture enforces (INPUT_CHAT_TAP_MS). Contract
# 2's refusal leg has to have held the opener PAST it for its refusal to mean
# anything, so the leg publishes the hold it OBSERVED and the collector checks
# it rather than trusting the driver (review finding 2).
CHAT_LOGIN_TAP_MS = 500
CHAT_LOGIN_CONTRACT_DEFAULT = 1
CHAT_LOGIN_CONTRACT_CURRENT = 2
CHAT_LOGIN_REFUSAL_MARKER = {1: "chord_refused", 2: "hold_refused"}
CHAT_LOGIN_REFUSAL_MARKERS = frozenset(CHAT_LOGIN_REFUSAL_MARKER.values())
CHAT_LOGIN_MARKERS_V2 = tuple(
    (p, CHAT_LOGIN_REFUSAL_MARKER[2], n) if (p, m) == ("p1", "chord_refused")
    else (p, m, n) for p, m, n in CHAT_LOGIN_MARKERS)
CHAT_LOGIN_EXPECTED_V2 = {(p, m): n for p, m, n in CHAT_LOGIN_MARKERS_V2}
CHAT_LOGIN_ORDINAL_V2 = {(p, m): i
                         for i, (p, m, _n) in enumerate(CHAT_LOGIN_MARKERS_V2)}
CHAT_LOGIN_CONTRACTS = {1: (CHAT_LOGIN_EXPECTED, CHAT_LOGIN_ORDINAL),
                        2: (CHAT_LOGIN_EXPECTED_V2, CHAT_LOGIN_ORDINAL_V2)}

# --- mp_chat_chord_1m: the debug-chord marker contract -----------------------
# Astra ruling 2 D2. Its OWN profile, event name and contract, because the
# login profile's P1 leg now refuses an over-long HOLD and "an 800 ms refusal
# must never be presented as hardware chord proof". What this profile claims,
# and nothing more (contract 104): from a KNOWN overlay state (forced off), the
# chord pressed in the RULED order (hold Minus, then press Plus) made the
# overlay ACTION fire, the WRONG order (Plus first) did NOT, neither order
# opened chat or moved the hotbar, after a neutral re-arm the ordinary opener
# still opened the composer -- which was closed again with the overlay in the
# state that ONE legal toggle implies -- and a SECOND correct-order chord then
# fired exactly once and put the overlay back OFF.
CHAT_CHORD_PROFILE = "mp_chat_chord_1m"
CHAT_CHORD_EVENT = "chat_chord"
# Review finding 4: the chord contract series is DISJOINT from the login one
# (1 = the old Up+Left run, 2 = the over-long-hold run), because both profiles
# write a `contract` integer into a PASS.json and a bare 1 would have been
# ambiguous between them. Chord contracts start at 101 (102 today), and both summaries
# report the PROFILE beside the number so the pair is what identifies a
# contract, never the number alone.
# 101 -> 102 (brief 07 A, Astra ruling 3 D2): F3 became specifically "hold
# Minus, THEN press Plus", so p2.chord order 0 INVERTED -- it used to assert
# that the Plus-first order fired too, and now asserts that it must not. 101
# never produced a hardware run, but a number whose meaning flipped is SPENT,
# not reusable: an old stream graded by these tables would have its claim read
# backwards. The contract also gained the hotbar slot readings, because Minus
# and Plus are the hotbar taps in the shipped CavEX scheme.
# 102 -> 103 (brief 12 D-1): 102 is spent for a HARDER reason than 101 -- it
# could never pass on a console at all. It kept 101's "p3.closed.overlay must
# be false" after making order 0 a negative leg, so with exactly ONE legal
# toggle the run always ended with F3 on. The real-Wii run
# mp_chat_chord_1m_db01e56_0919_173900 failed on precisely that check while
# every PRODUCT assertion held. 103 adds the RESTORE leg (p4.chord_restore)
# and re-aims p3.closed at the state one toggle really implies.
CHAT_CHORD_SPENT = {
    101: ("the meaning of p2.chord order 0 inverted (it required a fire and "
          "now forbids one), so an old stream would read its claim backwards"),
    102: ("unpassable on hardware: it demanded p3.closed.overlay=false after "
          "the single legal toggle had turned the overlay ON, which is how "
          "mp_chat_chord_1m_db01e56_0919_173900 failed with every product "
          "assertion holding; 103 adds the p4.chord_restore leg instead"),
    103: ("p2.chord gained a `fires` COUNT beside its bool, so a 103 stream "
          "carries strictly less evidence than these tables grade and the "
          "count would have to be invented; it never ran on hardware either "
          "(its E-2 gate refused the profile before P1 on every card), so "
          "like 101 and 102 it is spent rather than superseded"),
}
# 103 -> 104 (hostile review 17, M-1/M-2). P4 had already argued that a bool
# cannot carry "fired exactly once" -- three toggles end the overlay ON again
# -- while P2 was still grading the RULED chord with one. A double fire
# therefore passed p2 and was rejected four legs later at p3.closed with "the
# ruled chord fired but the debug overlay is off": a COUNTING defect wearing a
# product defect's diagnosis, on the wrong leg, which is the same class of
# failure that made 102 unpassable. p2.chord now carries `fires` and is graded
# on it exactly as p4.chord_restore is -- and its `overlay` reading, which the
# marker always carried and this grader never read, is cross-checked against
# that count, so "the action fired and the product toggled nothing" is refused
# HERE by name instead of surfacing indirectly two legs later.
#
# 104 ALSO carries C-1's restaging of the E-2 binding gate. That gate reads the
# LIVE binding table, which the device can only scan from inside an input poll
# that reaches player 0 -- and nobody owns player 0 until P1 binds the virtual
# pad, so a gate in P0 could never have a reading and refused every card,
# naming the very config that was staged. p0.session therefore no longer
# carries `chord_bound` (it cannot know it) and the gate is its own marker,
# p1.chord_bound, published on the first frame after the bind. 104 was created
# inside this same unmerged series and has never run anywhere, so the moved
# refusal is folded into it rather than spending another number.
CHAT_CHORD_CONTRACT_CURRENT = 104
CHAT_CHORD_CONTRACTS = (104,)
CHAT_CHORD_MARKERS = (
    ("p0", "session", 1),
    # Before pad_bound: the gate runs on the first frame the bind has produced
    # a reading, and the rest of P1 waits for its verdict.
    ("p1", "chord_bound", 1),
    ("p1", "pad_bound", 1),
    ("p2", "chord", 2),
    ("p3", "opened", 1),
    ("p3", "closed", 1),
    ("p4", "chord_restore", 1),
    ("run", "ok", 1),
)
CHAT_CHORD_EXPECTED = {(p, m): n for p, m, n in CHAT_CHORD_MARKERS}
CHAT_CHORD_ORDINAL = {(p, m): i for i, (p, m, _n) in enumerate(CHAT_CHORD_MARKERS)}
CHAT_CHORD_ORDERS = (0, 1)
# The virtual pad always presents INPUT_CAT_NUNCHUK, so a scripted run
# exercises the REMOTE family, where the chord does NOT share a button with the
# opener (2). The Classic shared-button case is covered by
# tests/test_input_wii_adapter.c and stays owed to physical acceptance -- this
# label says so rather than letting a PASS imply it.
CHAT_CHORD_PASS_LABEL = (
    "scripted real-Wii debug-chord leg on the Remote family: PASS; the ruled "
    "order (hold Minus, then press Plus) toggled the overlay, the Plus-first "
    "order did not, neither opened chat or moved the hotbar, the opener "
    "still worked after a neutral re-arm, and a second correct-order chord put "
    "the overlay back OFF so the run left the console as it found it. The "
    "CLASSIC shared-button case (Plus "
    "is both the opener and a chord half there) is NOT exercised by this "
    "profile and stays owed to physical acceptance")
CHAT_LOGIN_TRACE_MARKERS = (("p2", "trace"), ("p3", "trace"))
# The marker that closes each trace leg, and the leg's name in the artifact.
CHAT_LOGIN_LEG_CLOSERS = {("p2", "hold_result"): "hold",
                          ("p2", "tap_result"): "tap",
                          ("p2", "page_hold"): "page",
                          ("p3", "rearmed"): "reconnect"}
# Section 11 M1: the step count each leg's result marker reports, cross-checked
# against the steps counted in that leg's own trace rows. "steps" is every step
# in the leg; "extra_steps" is the page leg's REPEAT steps (the leg opens with
# the held direction's own edge step, which is not "extra"). The reconnect leg
# reports no count -- section 11 L3 re-derives its claim from the rows instead.
CHAT_LOGIN_LEG_STEP_FIELD = {"hold": "steps", "tap": "steps",
                             "page": "extra_steps", "reconnect": None}
# Section 12: every trace batch names the leg it belongs to, because the device
# restarts `t` at the first press of EACH leg -- without the tag a new leg's
# clock is indistinguishable from a clock running backwards (the finding that
# failed the first full PC replay). The legs a phase may carry:
CHAT_LOGIN_PHASE_LEGS = {"p2": ("hold", "tap", "page"), "p3": ("reconnect",)}
# Section 5's phase order, with section 9 ruling 6 applied: there is NO
# run.start marker, so the first marker of a run is p0.session and the `run`
# phase only ever ends one.
CHAT_LOGIN_PHASES = ("p0", "p1", "p2", "p3", "p4", "p5", "p6")
# Section 9 ruling 5: the req/cp/cd mask bits, identical everywhere, in
# OSK_CHAT_ARM_LEFT..PREVIOUS order (widget_osk.h). `req` carries a trailing
# `^` for a request PRESS edge this frame and `v` for a request RELEASE edge.
# Section 13 ruling 1: all three masks are written in HEX (either case), so
# PAGE is "40", DOWN|PAGE is "48" -- reading them as decimal mis-decodes every
# bit above 9 (it read the page tap as DOWN|BACK on the first PC replay).
CHAT_LOGIN_BITS = ("LEFT", "RIGHT", "UP", "DOWN", "CLICK", "BACK", "PAGE",
                   "PREVIOUS")
CHAT_LOGIN_DIRECTIONS = 0b1111  # LEFT | RIGHT | UP | DOWN
CHAT_LOGIN_REQ_EDGES = "^v"
# widget_osk.h: OSK_CHAT_REPEAT_DELAY_S 0.25F / OSK_CHAT_REPEAT_RATE_S 0.08F.
CHAT_LOGIN_REPEAT_DELAY_MS = 250
CHAT_LOGIN_REPEAT_RATE_MS = 80
# Section 5: "the collector additionally allows one 20 fps frame, as
# split_recovery section 5 does" -- on TOP of the device's own max_frame_ms.
CHAT_LOGIN_SLOW_FRAME_MS = 50
# Section 11 H1: max_frame_ms is measured PER LEG and used for that leg only.
# A leg whose worst frame exceeds this cannot say anything about a 250/80 ms
# cadence -- it is INCONCLUSIVE, not tolerated, and fails the run. (A run-wide
# maximum may ride on run.ok as `max_frame_ms_run`; it is never a tolerance.)
CHAT_LOGIN_FRAME_STALL_MS = 250
# chat_composer_model.h CHAT_HISTORY_LINES / chat_ring.h CHAT_RING_CAPACITY.
CHAT_LOGIN_RING_CAPACITY = 50
# widget_osk.h: OSK_CHAT_ARM_COUNT 12 -- every chat button blocked (the
# composer's "frozen" claim in P3 is exactly this mask, all bits set).
CHAT_LOGIN_ARM_MASK_FULL = (1 << 12) - 1
# Section 10 ruling 2: p0.session names the platform the driver ran on, and
# two P1 assertions are PLATFORM_WII-only (the chord gate lives in the Wii
# input path; the PC seam binds a {KEYBOARD,0} device -- ruling 5).
CHAT_LOGIN_PLATFORMS = ("wii", "pc")
CHAT_LOGIN_PC_CHANNEL = 0
# Section 10 ruling 7: until the third seam lands and a real-Wii run passes,
# a PC run may not wear the real-Wii PASS wording.
CHAT_LOGIN_PC_PASS_LABEL = (
    "scripted OSK command delivery proved on PC against the real fixture; "
    "HARDWARE-UNCONFIRMED -- this run makes no real-Wii claim")
# Section 10 ruling 1: the controller-recovery modal must never open for
# player 0 during the run. The device fails its phase with this exact diag,
# which the generic ok:false path then surfaces verbatim.
CHAT_LOGIN_RECOVERY_MODAL_DIAG = "recovery modal"
# input.h `enum input_category`: WIIMOTE 0, NUNCHUK 1, ... Section 1.4 rules
# the virtual pad is presented as NUNCHUK (a bare Remote is force-disconnected
# by input_owner_latch, so WIIMOTE is illegal here), on WPAD channel 3
# (ruling 2).
CHAT_LOGIN_CATEGORY_NUNCHUK = 1
CHAT_LOGIN_PAD_CHANNEL = 3
# input_owner.h `enum input_device_kind`: NONE 0, KEYBOARD 1, WPAD 2,
# GAMECUBE 3. Section 11 L1: p1.pad_bound reports the OBSERVED binding
# (input_player_device(0, ...)) and the collector compares kind + channel (+
# the owner category on wii) against the platform's expectation.
CHAT_LOGIN_KIND_KEYBOARD = 1
CHAT_LOGIN_KIND_WPAD = 2
CHAT_LOGIN_BINDING = {"wii": (CHAT_LOGIN_KIND_WPAD, CHAT_LOGIN_PAD_CHANNEL),
                      "pc": (CHAT_LOGIN_KIND_KEYBOARD, 0)}
# widget_osk.h OSK_PAGE_LOWER -- section 2's canonical origin (LOWER, 1, 0).
CHAT_LOGIN_PAGE_LOWER = 0
# Section 6: one line, in a log the rig creates fresh for THIS run.
CHAT_LOGIN_RECEIPT_NAME = "mc173-diag.log"
CHAT_LOGIN_RECEIPT_MARK = "[login-gate]"
# The fixture logs the receipt through mc173's tracing logger, so the line
# arrives with an ISO timestamp, a level and ANSI colour around them (measured
# on pc_chat_login_20260917_214444). The RECEIPT itself is the tail of the
# line and is matched exactly; the prefix is tolerated, never parsed.
CHAT_LOGIN_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
CHAT_LOGIN_RECEIPT_RE = re.compile(
    r"\[login-gate\] (\S+) /login len=(\d+) sha256=([0-9a-fA-F]+)$")


def chat_login_username(run_id: str) -> str:
    """Section 6 + section 9 ruling 7: the receipt is bound to THIS run by the
    player name the rig passes -- "cw" plus the first six hex characters of
    sha256(<run id string>), lowercase (8 chars, inside Beta's 16). The rig
    computes it (tools/stage_wii_dol.ps1, tools/run_wii_hardware.ps1), passes
    `--cw-mp-username`, the device echoes it as p0.session's `username`, and
    the collector requires the receipt's player to equal BOTH."""
    return "cw" + hashlib.sha256(str(run_id).encode("utf-8")).hexdigest()[:6]


# --- mp_play_10m: the unattended LAN PLAY contract ---------------------------
# docs/mp_play_profile.md is the authority; these constants mirror the device's
# own (source/game/mp_play_core.h) and the fixture's
# (_persist/mc173-diag/mc173-server/src/receipt.rs). A run proves: join, a typed
# login inside the server's deadline, >= 600 000 ms of MEASURED play whose digs
# / places / inventory transfers are each matched 1:1 to a SERVER receipt, a
# clean disconnect through the production pause menu, a rejoin through the
# production menus, and server-owned position / dimension / inventory unchanged
# across it. Nothing here is derived from the clock or from a counter a merely
# alive console would produce.
MP_PLAY_PROFILE = "mp_play_10m"
# hw1029b/G1: main.c's edge-triggered per-packet-kind counter row.
MP_KINDS_EVENT = "mp_kinds"
MP_KINDS_KEYS = ("mp_rx_bed", "mp_tx_leavebed", "mp_rx_attach",
                 "mp_rx_blockact", "mp_rx_auxsfx", "mp_rx_selfstatus",
                 "mp_rx_meta", "mp_tx_useitem")


def note_mp_kinds(acc: dict, event: dict) -> None:
    """Fold one mp_kinds row into acc as the max seen per key. Non-integer or
    missing values are skipped (recorded-only evidence never fails a run)."""
    for key in MP_KINDS_KEYS:
        value = event.get(key)
        if type(value) is int and value >= 0:
            acc[key] = max(acc.get(key, 0), value)
# REL-15: the open pause menu must dwell 180 s on a vanilla server, three
# times its 1200-tick read timeout (CFR NetworkManager.java:214-215). The
# device driver owns completion (cap MPM_TOTAL_MS 300 s, mp_pause_menu_core.h),
# so the staged --duration 180 can never end this collection; only
# profile_done or this cap, counted from the first heartbeat, can.
MP_PAUSE_MENU_HOLD_MS = 180000
MP_PAUSE_MENU_CAP_S = 360
_MP_SESSION_FIELDS = ("mp_login", "mp_end", "mp_gen", "mp_reconn", "mp_rx_kb")
MP_PLAY_EVENT = "mp_play"
# mp_play_core.h MPP_CONTRACT / MPP_MEASURED_PLAY_MS / MPP_TOTAL_CAP_MS. The
# device STATES these in its `session` marker and the collector refuses a
# stream that was built against different ones -- a .dol compiled with a
# shorter measured floor would otherwise be graded by this file's floor and
# pass a run it never ran.
MP_PLAY_CONTRACT_CURRENT = 1
MP_PLAY_CONTRACTS = (1,)
MP_PLAY_MEASURED_FLOOR_MS = 600000
MP_PLAY_TOTAL_CAP_MS = 1200000
# MPP_LOGIN_DEADLINE_MS / MPP_REJOIN_BUDGET_MS (Astra answer B2: the rejoin's
# 90 s run from initiating reconnect through verified gameplay, and BOTH logins
# must independently meet the server deadline).
MP_PLAY_LOGIN_DEADLINE_MS = 60000
MP_PLAY_REJOIN_BUDGET_MS = 90000
# MPP_FLOOR_* -- and every one of them counts MATCHED actions only (Astra
# Lane B item 4: duplicate, stale, rejected or unrelated updates earn no
# credit). Four inventory moves means four COMPLETED stack transfers.
MP_PLAY_FLOOR_DIGS = 8
MP_PLAY_FLOOR_PLACES = 8
MP_PLAY_FLOOR_INV_MOVES = 4
# Astra answer B2: displacement is the maximum HORIZONTAL server-observed
# distance from the initial join point, excluding teleports.
MP_PLAY_MIN_DISPLACEMENT = 64.0
# (mp_play_core.h's MPP_TELEPORT_BLOCKS threshold is deliberately NOT mirrored
# here: the device applies it to decide whether to REPORT a correction, while
# the collector reads the fixture's own `tp=1` flag -- the server's statement
# that it set the position -- and never re-derives the device's judgement.)
# MPP_POS_TOL_H / MPP_POS_TOL_V: mc173 saves the last position packet it
# processed, so the rejoin lands within a block of where the leave happened.
MP_PLAY_POS_TOL_H = 1.0
MP_PLAY_POS_TOL_V = 1.5
# MPP_INV_PARTS: a snapshot is one head marker plus three `inv` markers of 12
# core slots each (36 occupied slots do not fit one marker body).
MP_PLAY_INV_PARTS = 3
MP_PLAY_GENERATIONS = (1, 2)
# THE CANONICAL INVENTORY FORM, in one place: `slot:item:count:damage` in
# PROTOCOL window-0 numbering, ascending in the fixture's own emission order
# (armor 5..8, main 9..35, hotbar 36..44), with an empty slot -- and every
# armor slot, which the device's own snapshot cannot see -- written as item 0.
# receipt.rs's inv_list() produces exactly this string; the device's snapshot
# markers are canonicalised INTO it before any comparison, so the two sides are
# compared as one string built the same way twice.
MP_PLAY_SNAPSHOT_SLOTS = (tuple(range(5, 9)) + tuple(range(9, 36))
                          + tuple(range(36, 45)))
# The marker order, as the device emits it (mp_play_core.h's phase machine).
# The second element is the GENERATION the marker must carry, or 0 when the
# marker's generation is not part of its identity (it is still read and
# checked, it just cannot name two different slots in this table).
MP_PLAY_PHASE_ORDER = (
    ("session", 0),
    ("ready", 1), ("login_begin", 1), ("login_ok", 1),
    ("p2_begin", 0), ("p2_end", 0),
    ("snapshot_pre", 0), ("disconnected", 0), ("rejoin_begin", 0),
    ("ready", 2), ("login_begin", 2), ("login_ok", 2),
    ("snapshot_post", 0), ("verified", 0), ("post_action", 0), ("ok", 0),
)
MP_PLAY_PHASE_ORDINAL = {key: i for i, key in enumerate(MP_PLAY_PHASE_ORDER)}
# REL-14b-0: mp_play arms the shared login machine ACTION-ONLY, so each
# generation emits exactly these and types nothing (vanilla never answers
# /login, CFR NetServerHandler.java:469-474). Any other leg is a typed login.
MP_PLAY_LOGIN_ORDER = (("p0", "session"), ("p1", "pad_bound"),
                       ("p1", "actions_ready"))
MP_PLAY_LOGIN_TABLES = ({key: 1 for key in MP_PLAY_LOGIN_ORDER},
                        {key: i for i, key in enumerate(MP_PLAY_LOGIN_ORDER)})
# REL-14b-ev2a: the pinned vanilla server's own log. Every record is COMPLETE
# (CFR ConsoleLogFormatter.java:24-43, "yyyy-MM-dd HH:mm:ss [LEVEL] message")
# and its message must START with the record's own text: a player's chat is
# logged "<name> text" (NetServerHandler.java:442), "/me" as "* name"
# (:452) and "/tell" with a colour code (:462), so chat never binds.
MP_PLAY_VANILLA_LOG = "server.log"
MP_PLAY_VANILLA_MANIFEST = (Path(__file__).resolve().parent / "mp_fixture"
                            / "vanilla-b173.json")
MP_PLAY_VANILLA_RECORDS = tuple(
    (kind, re.compile(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d \[%s\] %s$"
                      % (level, body))) for kind, level, body in (
        # server/MinecraftServer.java:113 and :130
        ("start", "INFO", r"Starting minecraft server version (.*)"),
        ("port", "INFO", r"Starting Minecraft server on \S*:(\d+)"),
        # NetLoginHandler.java:117 (getUserAndIPString = name [/host:port])
        ("login", "INFO", r"(\w+) \[/([^\]]+):\d+\] logged in with entity "
                          r"id (-?\d+) at \([^()]*\)"),
        ("lost", "INFO", r"(\w+) lost connection: (\S+)"),  # NSH.java:404
        # NetServerHandler.java:213/236/253/268/424, then :469/:473
        ("flag", "WARNING", r"(\w+) (?:had an illegal stance: .*|moved too "
                            r"quickly!|moved wrongly!|was kicked for floating"
                            r" too long!|tried to set an invalid carried item)"),
        ("flag", "INFO", r"(\w+) (?:issued server command|tried command): .*")))
# Markers emitted INSIDE measured play, in whatever order the world offered
# them: they are exempt from the phase order (like chat_login's trace batches)
# and are instead confined to the window between p2_begin and p2_end.
MP_PLAY_PLAY_MARKERS = frozenset({"dig", "pickup", "place", "inv_move",
                                  "reserve_kept", "build_skipped",
                                  "inv_skipped",
                                  # c4488f36: a walk leg that made less than
                                  # MPP_LEG_MIN_PROGRESS turns the next leg 90
                                  # degrees and says so. Narration of the leg,
                                  # like build_skipped -- it counts nothing and
                                  # certifies nothing, but the device emits it
                                  # inside measured play, so a contract that
                                  # does not list it REFUSES THE WHOLE TAPE
                                  # ("unexpected marker 'heading_turned'",
                                  # PC run pc_mp_play_20260921_130624).
                                  "heading_turned",
                                  # 8bf54845, same class again (push-0921/B12).
                                  # plant_cleared: the place target had tall
                                  # grass or a flower standing on it and the
                                  # driver broke it first, which is what a
                                  # player does. pickup_missed: one dug item
                                  # did not reach the inventory -- on a REMOTE
                                  # server the drop is the server's physics, so
                                  # an own-column dig makes a pickup likely,
                                  # not certain (run pc_mp_play_20260921_162144
                                  # lost 1 of 4 with the block provably broken
                                  # under the feet). Both are narration:
                                  # tallied and REPORTED, graded by nothing.
                                  # A run that loses MOST of its drops is a
                                  # different thing and the device fails it by
                                  # name -- see the generic `fail` rule.
                                  "plant_cleared", "pickup_missed",
                                  # REL-14b-walker-escape (341cc6b28) and its
                                  # hw1021e follow-up: the walker pillars out
                                  # of a pit (escape_step), digs a wall cell
                                  # for a block to pillar with (escape_mine),
                                  # or says why it could not (escape_failed,
                                  # followed by the device `fail`, or by
                                  # walk_done_stuck when the floors were met).
                                  # Narration: the escape's digs and places
                                  # are NOT the build leg's and count toward
                                  # no floor. Unlisted, a successful escape
                                  # would refuse the whole tape.
                                  "escape_step", "escape_mine",
                                  "escape_failed",
                                  # 5893b4de0: the walker re-centres in its
                                  # column before the pillar jumps (nudges, x,
                                  # y, z). Optional narration, only emitted
                                  # when it nudged; graded by nothing.
                                  "escape_centred",
                                  # hw1021f: a walker stuck AFTER every
                                  # walk-phase floor was met ends the walk as
                                  # success and stands until p2_end. It earns
                                  # nothing: every floor is still graded from
                                  # the server's receipts, and the marker's own
                                  # claim is checked against the tape (see
                                  # _note_play).
                                  "walk_done_stuck"})
# A continuous latch, so it may land in any in-world phase.
MP_PLAY_FREE_MARKERS = MP_PLAY_PLAY_MARKERS | {"teleport"}
# The fixture's receipt line, after the tracing prefix/ANSI is stripped:
#   [mp-play] n=<nonce> g=<gen> ev=<event> k=v ...
# `death` carries no field at all, so the tail is optional.
MP_PLAY_RECEIPT_MARK = "[mp-play]"
MP_PLAY_RECEIPT_RE = re.compile(
    r"\[mp-play\] n=(\S+) g=(\d+) ev=([A-Za-z_]+)(?: (.*))?$")
# The item-entity receipts (fixture 5f6d406b, push-0921/B13). NARRATION on the
# server side, exactly as plant_cleared / pickup_missed are on the device side:
# they are read and reported, they carry no floor and they change no verdict.
# They exist to answer ONE question the older receipt set could not -- when the
# device says pickup_missed, WHERE did the dropped item go -- and the three
# cases have three different owners, so the answer decides who fixes it.
MP_PLAY_ITEM_EVENTS = frozenset({"item_spawn", "item_collect", "item_gone"})
# mc173 removes an item in exactly three places (a pickup, y < -64, and the
# 6000-tick lifetime), so `reason` is drawn from this set. Unknown is kept as a
# value rather than rejected: a fixture that grows a fourth removal path should
# show up in the report, not refuse the tape.
MP_PLAY_ITEM_GONE_REASONS = ("despawn", "void", "unknown")
# An item the fixture cannot attribute to a player (it was not dig-spawned, or
# it outlived the connection) is logged with this nonce and gen 0. It is NOT a
# receipt "belonging to another run" -- the foreign-nonce rule below has to
# know the difference, or a run whose item simply despawned would be refused
# outright with "the log belongs to another run".
MP_PLAY_UNATTRIBUTED_NONCE = "-"
# Astra gate 1's landing label, verbatim: what a PASS of this profile is worth
# and, just as importantly, what it is NOT.
MP_PLAY_PASS_LABEL = (
    "D1 unattended LAN candidate: mc173-diag fixture coverage; BetaMC and "
    "physical-controller acceptance pending; plugin-auth compatibility "
    "unproven")
# Same rule as mp_chat_login_3m (section 10 ruling 7): a PC run proves the
# contract against the real fixture and makes NO console claim.
MP_PLAY_PC_PASS_LABEL = (
    "mp_play_10m contract proved on PC against the real mc173-diag fixture; "
    "HARDWARE-UNCONFIRMED -- this run makes no real-Wii claim")
# B-a (Astra Lane B item 2): the cumulative load-ready BACKSTOP counter,
# exported on every heartbeat. "No backstop" may only pass on an OBSERVED
# normal release, so the counter must be present and zero from the first
# heartbeat to the last -- including across the rejoin. A MISSING field fails:
# an unprovable claim must not certify (the same rule `fring` already carries).
# Scoped to the two profiles whose contract asks for it; no other profile's
# verdict logic is touched.
LW_TIMEOUT_ADMIT_PROFILES = (MP_PLAY_PROFILE, "mp_lan_walk_10m")

# REL-MP-GATE (2026-10-18): hw1018_e677b79_8_mp_lan_walk_10m PASSed on
# "duration 600s reached" although its session ended at heartbeat 31
# (mp_end_reason latched, world_loaded false on 565/593 beats). On these
# profiles the session must stay up for the run: once the world has loaded,
# or once the login window has passed, a heartbeat with a non-empty
# mp_end_reason or world_loaded false FAILs the run as mp_session_ended. The
# first such heartbeat is latched; later recovery does not clear it.
# The window is 60 beats (~60 s): mp_connect_smoke's measured DHCP/connect/
# login settle is ~25-30 s and every retained mp tape loads by beat 3-4.
# mp_play_10m is deliberately NOT gated here (maintainer narrowing
# 2026-10-18): it disconnects and rejoins on purpose and is judged by its own
# MpPlayEvidence collector.
MP_SESSION_GATE_PROFILES = ("mp_connect_smoke", "mp_lan_walk_10m")
MP_SESSION_LOGIN_WINDOW_HB = 60

# REL-MP-JOINGATE (2026-10-22): vanilla joins of 36-48 s went unnoticed
# because the MP profiles only checked "did it connect". The initial join time
# is the FIRST heartbeat with load_screen_ms > 0 (main.c latches the load
# screen's on-screen time when it closes). Over the FAIL limit the run FAILs;
# over the WARN limit it passes with "join_slow_warning". A missing value on
# these profiles is recorded explicitly (join_ms null, join_ms_missing true)
# and FAILs: every retained MP tape carries it.
MP_JOIN_GATE_PROFILES = ("mp_connect_smoke", "mp_lan_walk_10m",
                         "mp_pause_menu_3m", "mp_play_10m")
MP_JOIN_WARN_MS = 10000
MP_JOIN_FAIL_MS = 15000


def mp_play_canonical_inventory(slots: dict) -> str:
    """`slots` maps a PROTOCOL slot number to (item, count, damage). Returns the
    canonical `slot:item:count:damage` list of MP_PLAY_SNAPSHOT_SLOTS -- the one
    string form both sides of a snapshot comparison are built into."""
    out = []
    for slot in MP_PLAY_SNAPSHOT_SLOTS:
        item, count, damage = slots.get(slot, (0, 0, 0))
        if int(item) <= 0:
            item, count, damage = 0, 0, 0
        out.append("%d:%d:%d:%d" % (slot, int(item), int(count), int(damage)))
    return ",".join(out)


for _name, _profile in PROFILES.items():
    _profile['tps_settle_ms'] = None if _name in SPLIT_PROFILES else 5000


# colcap-1006 (ASTRA-CC-R3 item 2): the physical column pool's shape
# (mem2.h MEM2_POOL_COLUMN_BOOT_SLABS / MEM2_POOL_COLUMN_SLABS /
# MEM2_POOL_SLAB_SLOTS), used to validate grow/shrink events and the "cpv"
# heartbeat block.
COLUMN_POOL_BOOT_SLABS = 4
COLUMN_POOL_SLABS_MAX = 9
COLUMN_POOL_SLAB_SLOTS = 32
COLUMN_POOL_EVENTS = ("column_pool_grow", "column_pool_shrink")
# One column slab's carve request (mem2.c: lead + 32 * (82,176 + 256)).
COLUMN_POOL_SLAB_BYTES = 256 + COLUMN_POOL_SLAB_SLOTS * (82176 + 256)
COLUMN_POOL_SLAB_REPORT_MAX = 16  # HW_POOL_SLAB_REPORT_MAX
_POOL_HB_INTS = ("column_pool_used", "column_pool_peak", "column_pool_slabs",
                 "column_pool_overflow", "column_pool_carve_fail",
                 "budget_resident", "budget_live_backings", "budget_reserved",
                 "budget_prepeak", "budget_peak", "budget_waits", "budget_phase")
# More distinct violations than this are counted, not listed: a message that
# embeds a per-heartbeat value must not grow the verdict without bound.
ANCHOR_MAX_ERRORS = 32


def _u32(v):
    return type(v) is int and 0 <= v <= 0xffffffff


COLUMN_POOL_CPV = 3            # current heartbeat schema ("cpe" added)
COLUMN_POOL_SHRINK_HEADROOM = 16  # MEM2_POOL_COLUMN_SHRINK_HEADROOM
COLUMN_POOL_PENDING_MAX = 256
# R4 point 7 "dlo": display-list bytes by owner (hw_telemetry.h).
DLO_FIELDS = ("view0_bytes", "view0_lists", "view1_bytes", "view1_lists",
              "pinned_bytes", "pinned_lists", "completed_bytes",
              "completed_lists", "age_lt5s_bytes", "age_lt30s_bytes",
              "age_ge30s_bytes", "own_alloc_lists")
MEM2_GUARD_BYTES = 256
# R5 item 3 "pac" (mem2.h MEM2_COST_FIELDS order).
PAC_FIELDS = ("col_alloc_n", "col_alloc_us", "col_alloc_max_us",
              "col_free_n", "col_free_us", "col_free_max_us",
              "lock_wait_n", "lock_wait_us", "lock_wait_max_us")
# The current-schema pool block a PC build emits on an anchor heartbeat (no
# MEM2 pool: zero slabs, empty occupancy, no events). Synthetic selftest
# heartbeats carry it, because missing evidence now fails certification.
PC_POOL_EVIDENCE = dict(
    cpv=3, column_pool_used=0, column_pool_peak=0, column_pool_slabs=0,
    column_pool_overflow=0, column_pool_carve_fail=0, budget_valid=True,
    budget_wii=False, budget_resident=0, budget_live_backings=0,
    budget_reserved=0, budget_prepeak=0, budget_peak=0, budget_waits=0,
    budget_phase=0, cps=[], dll=0, dlb=0, cpe=[0, 0, 0, 0])
COLUMN_POOL_HISTORY_MAX = 4096


OVERLAY_SPILL_KEYS = ("fallback_live", "fallback_total", "spill", "mem2_last_spill_bytes")


class OverlaySpillEvidence:
    """SPLIT-SPILL (2026-10-14): the device's change-driven "overlay_spill"
    observer record (hw_telemetry.h struct hw_overlay_spill) -- column-view
    private-image fallbacks live/total, the carve spill count, and the
    guarded size of the most recent spilled carve request. DIAGNOSTIC ONLY:
    nothing here feeds a gate, a PASS/FAIL or a failure class; the spill gate
    stays on the heartbeat's own "spill" counter. A malformed record is
    counted and ignored, never a FAIL. The size is a size class (80.25 KiB =
    82,176 B is an overlay private image), not a tenant identity."""

    def __init__(self) -> None:
        self.records = 0
        self.malformed = 0
        self.last: dict | None = None
        self.max_fallback_live: int | None = None
        self.spill_sizes: dict[int, int] = {}

    def note(self, event: dict) -> None:
        values = {k: event.get(k) for k in OVERLAY_SPILL_KEYS}
        tick = event.get("tick")
        if (event.get("v") != 1 or type(tick) is not int
                or any(type(v) is not int or v < 0 or v > 0xFFFFFFFF
                       for v in values.values())):
            self.malformed += 1
            return
        self.records += 1
        prev = self.last
        self.last = {"tick": tick, **values}
        live = values["fallback_live"]
        self.max_fallback_live = live if self.max_fallback_live is None else max(self.max_fallback_live, live)
        size = values["mem2_last_spill_bytes"]
        # Count each spill-count rise against the size it left behind.
        prev_spill = prev["spill"] if prev else 0
        if size and values["spill"] > prev_spill:
            self.spill_sizes[size] = self.spill_sizes.get(size, 0) + (values["spill"] - prev_spill)

    def summary(self) -> dict:
        return {"graded": False, "records": self.records, "malformed": self.malformed,
                "last": self.last, "max_fallback_live": self.max_fallback_live,
                "spills_by_last_request_bytes": {str(k): v for k, v in sorted(self.spill_sizes.items())}}


def render_overlay_spill(summary: dict | None) -> str:
    if not summary or (not summary.get("records") and not summary.get("malformed")):
        return ""  # healthy runs emit no record; keep their summary unchanged
    last = summary.get("last") or {}
    return (f"- Overlay spill (diagnostic, not graded): records={summary.get('records')} "
            f"malformed={summary.get('malformed')}, fallback live/total="
            f"{last.get('fallback_live')}/{last.get('fallback_total')} "
            f"(max live {summary.get('max_fallback_live')}), spill={last.get('spill')}, "
            f"last spill request={last.get('mem2_last_spill_bytes')} B, "
            f"spills by last request size={summary.get('spills_by_last_request_bytes')}\n")


class ColumnPoolEvidence:
    """colcap-1006 (ASTRA-CC-R3 items 2-3, R4 points 9-10): the physical
    column pool, reconstructed from ordered grow/shrink events and reconciled
    against sequence-stamped heartbeat snapshots.

    ORDER. The device queues events under its allocator lock, numbering them
    there, and publishes oldest-first; seq is gap-free on a current build, so
    a gap is a LOST event. The heartbeat rides a priority lane that can
    overtake queued events, so each "cpv":3 snapshot carries the seq of the
    last event queued before it ("cpe"[0]); a snapshot is checked only once
    the events up to that seq have arrived (pending until then; unresolved at
    the end = missing evidence).

    IDENTITY. Slabs are identified by the order they were carved (boot slabs
    0..boot-1, then one id per successful grow). A hand-back removes the id
    at its index, so per-slab minima stay attached to the same physical slab
    across index shifts and regrowth.

    HISTORICAL. A tape recorded before the current schema (replay only) is
    graded for what it carries and labelled non-certifying; a live run, or
    a replay of a current-schema tape, must carry current evidence on every
    heartbeat."""

    def __init__(self, profile, report, historical=False):
        self.profile = profile
        self.report = report            # AnchorEvidence's violation sink
        self.historical = historical
        self.gate = profile != "split_dims_6m"  # dims grades pool keys itself
        # events
        self.events = 0
        self.last_seq = None
        self.slabs = None
        self.ids = None
        self.next_id = 0
        self.boot = None
        self.history = {}               # seq -> (slabs, ids tuple, shrinks)
        self.shrinks = 0
        self.grows_ok = 0
        self.grows_failed = 0
        self.shrunk_bytes = 0
        self.seq_gaps = 0
        self.high_water = 0
        self.grow_min_largest = None
        # heartbeats
        self.hb = 0
        self.hb_schema = set()
        self.hb_last = None
        self.hb_slabs_max = 0
        self.hb_peak_max = 0
        self.hb_overflow = 0
        self.hb_carve_fail = 0
        self.hb_cpe_last = None         # [seq, shrinks, shrink_fail, lost]
        self.pending = []
        self.resolved = 0
        # occupancy (resolved samples only)
        self.occ_samples = 0
        self.occ_identity = None        # "carve-order" or "index (historical)"
        self.occ_min = {}               # slab id -> min live columns
        self.occ_empty_nonboot_max = 0
        self.occ_reclaimable_max = 0
        self.occ_reclaimable_hb = 0
        self.occ_reclaimable_measured_hb = 0
        self.occ_at_carve_min = None
        self.occ_carve_min_kb = None
        self.dl_bytes_max = 0
        self.dl_lists_max = 0
        # R4 point 7: display-list owner census ("dlo").
        self.dlo_samples = 0
        self.dlo_max = {}
        self.dlo_at_carve_min = None
        self.dlo_carve_min_kb = None
        self.dlo_at_dlb_max = None
        # R5 item 3: allocator CPU/lock cost ("pac", per-heartbeat deltas).
        self.pac_samples = 0
        self.pac_totals = [0] * len(PAC_FIELDS)
        self.pac_max = {}

    # -- events ------------------------------------------------------------
    def _set_base(self, slabs):
        self.slabs = slabs
        self.boot = slabs
        self.ids = list(range(slabs))
        self.next_id = slabs
        self.high_water = max(self.high_water, slabs)
        self.history[0] = (slabs, tuple(self.ids), 0)

    def note_event(self, ev):
        kind = ev.get("event")
        seq, before, after = ev.get("seq"), ev.get("slabs_before"), ev.get("slabs_after")
        ok = (kind in COLUMN_POOL_EVENTS and _u32(seq) and seq > 0
              and _u32(before) and _u32(after)
              and before <= COLUMN_POOL_SLABS_MAX and after <= COLUMN_POOL_SLABS_MAX)
        if ok and kind == "column_pool_grow":
            free, largest = ev.get("free_bytes_before"), ev.get("largest_allocatable_bytes_before")
            valid, success = ev.get("before_valid"), ev.get("success")
            ok = (_u32(ev.get("requested_bytes")) and ev["requested_bytes"] > 0
                  and type(valid) is bool and type(success) is bool
                  and _u32(free) and _u32(largest) and largest <= free
                  and (valid or (free == 0 and largest == 0))
                  and after == (before + 1 if success else before))
        elif ok:
            slab = ev.get("slab")
            ok = (_u32(ev.get("released_bytes")) and ev["released_bytes"] > 0
                  and _u32(slab) and slab < before
                  and slab >= COLUMN_POOL_BOOT_SLABS
                  and after == before - 1 and after >= COLUMN_POOL_BOOT_SLABS)
        if not ok:
            self.report("malformed column pool event")
            return
        expected = (self.last_seq if self.last_seq is not None
                    else (None if self.historical else 0))
        if expected is not None:
            if seq <= expected:
                self.report("column pool event sequence regressed")
                return
            if seq != expected + 1:
                self.seq_gaps += 1
                if not self.historical:
                    self.report("column pool event lost (sequence gap)")
        self.last_seq = seq
        if self.slabs is None:
            base = self.history.get(0)
            if base is not None:
                # A seq-0 heartbeat fixed the boot count first (validated
                # against the boot slabs there).
                self.slabs, self.ids = base[0], list(base[1])
                self.boot = base[0]
                self.next_id = base[0]
            else:
                # R5 (Sol R4 Major 1): the chain starts from the boot slabs
                # mem2_init carves, never from whatever the first event says.
                if before != COLUMN_POOL_BOOT_SLABS:
                    self.report("column pool event chain does not start from the %d "
                                "boot slabs (first slabs_before %d)"
                                % (COLUMN_POOL_BOOT_SLABS, before))
                self._set_base(COLUMN_POOL_BOOT_SLABS)
        if before != self.slabs:
            self.report("column pool event chain broken (a grow/shrink event was lost)")
            # Resynchronise on the device's own count; the violation stands.
            self.ids = self.ids[:before] + [None] * max(0, before - len(self.ids))
            self.slabs = before
        self.events += 1
        if kind == "column_pool_shrink":
            if ev["slab"] < len(self.ids):
                del self.ids[ev["slab"]]
            self.shrinks += 1
            self.shrunk_bytes += ev["released_bytes"]
        elif ev["success"]:
            self.ids.append(self.next_id)
            self.next_id += 1
            self.grows_ok += 1
            if ev["before_valid"]:
                largest = ev["largest_allocatable_bytes_before"]
                self.grow_min_largest = (largest if self.grow_min_largest is None
                                         else min(largest, self.grow_min_largest))
        else:
            self.grows_failed += 1
            self.report("column pool slab carve failed (heap had no room for a slab)")
        self.slabs = after
        self.high_water = max(self.high_water, before, after)
        if len(self.history) < COLUMN_POOL_HISTORY_MAX:
            self.history[seq] = (after, tuple(self.ids), self.shrinks)
        else:
            self.report("column pool event history overflow")
        if self.high_water > SPLIT_MAX_SLABS and self.gate:
            self.report(f"physical column pool exceeded {SPLIT_MAX_SLABS} slabs (grow events)")
        self._resolve()

    # -- heartbeats ----------------------------------------------------------
    def note_heartbeat(self, hb):
        cpv = hb.get("cpv")
        if cpv is None:
            if self.historical:
                return
            self.report("column pool evidence disappeared mid-run" if self.hb
                        else "missing current-schema column pool evidence")
            return
        if cpv not in (1, 2, COLUMN_POOL_CPV):
            self.report("missing or malformed column pool evidence")
            return
        if cpv != COLUMN_POOL_CPV and not self.historical:
            self.report("outdated column pool evidence schema (cpv %s, need %d)"
                        % (cpv, COLUMN_POOL_CPV))
            return
        if (any(not _u32(hb.get(k)) for k in _POOL_HB_INTS)
                or any(type(hb.get(k)) is not bool for k in ("budget_valid", "budget_wii"))):
            self.report("missing or malformed column pool evidence")
            return
        used, peak, slabs = hb["column_pool_used"], hb["column_pool_peak"], hb["column_pool_slabs"]
        overflow, carve_fail = hb["column_pool_overflow"], hb["column_pool_carve_fail"]
        if (used > peak or slabs > COLUMN_POOL_SLABS_MAX
                or used > slabs * COLUMN_POOL_SLAB_SLOTS or carve_fail > overflow
                or peak < self.hb_peak_max or overflow < self.hb_overflow
                or carve_fail < self.hb_carve_fail):
            self.report("inconsistent or regressed column pool evidence")
            return
        cps = dll = dlb = cpe = None
        if cpv >= 2:
            cps, dll, dlb = hb.get("cps"), hb.get("dll"), hb.get("dlb")
            if (not isinstance(cps, list) or not _u32(dll) or not _u32(dlb)
                    or any(type(v) is not int or not 0 <= v <= COLUMN_POOL_SLAB_SLOTS
                           for v in cps)):
                self.report("missing or malformed per-slab occupancy evidence")
                return
            if (len(cps) != min(slabs, COLUMN_POOL_SLAB_REPORT_MAX)
                    or sum(cps) != used):
                self.report("per-slab occupancy disagrees with the column pool counters")
                return
        if cpv >= 3 and "dlo" in hb:
            self._note_dlo(hb, dlb)
        if cpv >= 3 and "pac" in hb:
            self._note_pac(hb)
        if cpv >= 3:
            cpe = hb.get("cpe")
            if not isinstance(cpe, list) or len(cpe) != 4 or any(not _u32(v) for v in cpe):
                self.report("missing or malformed column pool event stamp")
                return
            if self.hb_cpe_last is not None and any(
                    a < b for a, b in zip(cpe, self.hb_cpe_last)):
                self.report("column pool event stamp regressed")
                return
            self.hb_cpe_last = list(cpe)
        self.hb += 1
        self.hb_schema.add(cpv)
        self.hb_last = {k: hb[k] for k in _POOL_HB_INTS + ("budget_valid", "budget_wii")}
        self.hb_slabs_max = max(self.hb_slabs_max, slabs)
        self.hb_peak_max = peak
        self.hb_overflow = overflow
        self.hb_carve_fail = carve_fail
        if dlb is not None:
            self.dl_bytes_max = max(self.dl_bytes_max, dlb)
            self.dl_lists_max = max(self.dl_lists_max, dll)
        if self.gate:
            if not hb["budget_valid"]:
                self.report("transfer budget snapshot was not coherent")
            if overflow or carve_fail:
                self.report("physical column pool overflow or carve failure")
            if slabs > SPLIT_MAX_SLABS or peak > SPLIT_COLUMN_CAP:
                self.report(f"physical column pool exceeded {SPLIT_MAX_SLABS} slabs / "
                            f"{SPLIT_COLUMN_CAP} columns")
        if cpe is not None:
            if cpe[2]:
                self.report("column slab hand-back refused by the heap (shrink_fail)")
            if cpe[3]:
                self.report("column pool events lost on the device (queue overflow)")
            sample = (cpe[0], slabs, cpe[1], cps, used, hb.get("anchor_measuring") is True,
                      hb.get("mem2_carve_free_kb"), hb.get("tick"), dll, dlb)
            if cpe[0] == 0:
                # R5 (Sol R4 Major 1): before any event the pool is EXACTLY
                # the boot slabs (a Wii build carves 4 at mem2_init; a PC
                # build has no pool), with no hand-back. Anything else --
                # e.g. eight slabs at seq 0 with no grow event -- is evidence
                # the event stream cannot explain, never a new baseline.
                boot = COLUMN_POOL_BOOT_SLABS if hb["budget_wii"] else 0
                if slabs != boot or cpe[1] != 0 or cpe[2] != 0:
                    self.report("sequence-zero column pool state is not the %d boot "
                                "slabs (%d slabs, %d hand-backs, no events)"
                                % (boot, slabs, cpe[1]))
                    return
                if 0 not in self.history and self.slabs is None:
                    self.history[0] = (slabs, tuple(range(slabs)), 0)
            if hb["budget_wii"] and slabs < COLUMN_POOL_BOOT_SLABS:
                self.report("column pool below its %d boot slabs" % COLUMN_POOL_BOOT_SLABS)
                return
            if len(self.pending) >= COLUMN_POOL_PENDING_MAX:
                self.report("column pool snapshots outran their events (pending overflow)")
                return
            self.pending.append(sample)
            self._resolve()
        elif cps is not None:
            # Historical (cpv 2): no seq stamp. Identity is the index, which
            # is exact only while nothing was handed back.
            self._occupancy(None, slabs, cps, used, hb.get("anchor_measuring") is True,
                            hb.get("mem2_carve_free_kb"), hb.get("tick"), dll, dlb)

    def _note_pac(self, hb):
        """R5 item 3: Broadway allocator cost since the previous heartbeat:
        column-pool allocs/frees (count, total us, max us) and contended
        mem2-lock waits. Measurement; malformed is a violation."""
        pac = hb["pac"]
        if (not isinstance(pac, list) or len(pac) != len(PAC_FIELDS)
                or any(not _u32(v) for v in pac)
                or any(pac[i + 2] > pac[i + 1] for i in (0, 3, 6))):
            self.report("malformed allocator cost (pac)")
            return
        self.pac_samples += 1
        for i, v in enumerate(pac):
            if PAC_FIELDS[i].endswith("_max_us"):
                self.pac_max[PAC_FIELDS[i]] = max(self.pac_max.get(PAC_FIELDS[i], 0), v)
            else:
                self.pac_totals[i] += v

    def pac_summary(self):
        if not self.pac_samples:
            return None
        t = dict(zip(PAC_FIELDS, self.pac_totals))
        out = {"heartbeats": self.pac_samples}
        for name in ("col_alloc", "col_free", "lock_wait"):
            n, us = t[name + "_n"], t[name + "_us"]
            out[name] = {"count": n, "total_us": us,
                         "avg_us": round(us / n, 2) if n else None,
                         "max_us": self.pac_max.get(name + "_max_us", 0)}
        return out

    def _note_dlo(self, hb, dlb):
        """R4 point 7: who holds the display-list bytes. Measurement only.
        The remainder dlb - (installed + pinned + completed) is what no named
        owner accounts for (the worker's in-progress job, other owners); it
        is reported, never hidden, and may read slightly negative because
        the device samples the counters a few microseconds apart."""
        dlo = hb["dlo"]
        if (not isinstance(dlo, list) or len(dlo) != len(DLO_FIELDS)
                or any(not _u32(v) for v in dlo)):
            self.report("malformed display-list owner census")
            return
        row = dict(zip(DLO_FIELDS, dlo))
        named = (row["view0_bytes"] + row["view1_bytes"] + row["pinned_bytes"]
                 + row["completed_bytes"])
        row["remainder_bytes"] = dlb - named
        row["guard_bytes"] = row["own_alloc_lists"] * MEM2_GUARD_BYTES
        row["dlb"] = dlb
        row["tick"] = hb.get("tick")
        self.dlo_samples += 1
        for k, v in row.items():
            if k != "tick" and type(v) is int:
                self.dlo_max[k] = max(self.dlo_max.get(k, v), v)
        if self.dlo_at_dlb_max is None or dlb > self.dlo_at_dlb_max["dlb"]:
            self.dlo_at_dlb_max = row
        carve = hb.get("mem2_carve_free_kb")
        if (hb.get("anchor_measuring") is True and type(carve) is int
                and carve >= 0 and (self.dlo_carve_min_kb is None
                                    or carve < self.dlo_carve_min_kb)):
            self.dlo_carve_min_kb = carve
            self.dlo_at_carve_min = dict(row, carve_free_kb=carve)

    def _resolve(self):
        keep = []
        for sample in self.pending:
            seq, slabs, shrinks, cps = sample[0], sample[1], sample[2], sample[3]
            state = self.history.get(seq)
            if state is None:
                keep.append(sample)
                continue
            self.resolved += 1
            if state[0] != slabs or state[2] != shrinks:
                self.report("heartbeat pool snapshot disagrees with the ordered events")
                continue
            self._occupancy(state[1], *sample[1:2], cps, *sample[4:])
        self.pending = keep

    def _occupancy(self, ids, slabs, cps, used, measuring, carve, tick, dll, dlb):
        self.occ_samples += 1
        if ids is None:
            ids = tuple(range(len(cps)))
            self.occ_identity = self.occ_identity or "index (historical; no hand-backs seen)"
            if self.shrinks:
                self.occ_identity = "index (historical; UNRELIABLE after a hand-back)"
        else:
            self.occ_identity = "carve order (stable across hand-back/regrow)"
        boot = self.boot if self.boot is not None else COLUMN_POOL_BOOT_SLABS
        for slab_id, live in zip(ids, cps):
            key = "?" if slab_id is None else slab_id
            self.occ_min[key] = min(self.occ_min.get(key, live), live)
        empty = sum(1 for slab_id, live in zip(ids, cps)
                    if live == 0 and slab_id is not None and slab_id >= boot)
        # What the device would actually hand back: empty non-boot slabs, but
        # only while >= 16 free slots remain afterwards (the shrink rule).
        reclaim = 0
        while (reclaim < empty and (slabs - reclaim - 1) * COLUMN_POOL_SLAB_SLOTS
               - used >= COLUMN_POOL_SHRINK_HEADROOM):
            reclaim += 1
        self.occ_empty_nonboot_max = max(self.occ_empty_nonboot_max, empty)
        self.occ_reclaimable_max = max(self.occ_reclaimable_max, reclaim)
        if reclaim:
            self.occ_reclaimable_hb += 1
            if measuring:
                self.occ_reclaimable_measured_hb += 1
        if (measuring and type(carve) is int and carve >= 0
                and (self.occ_carve_min_kb is None or carve < self.occ_carve_min_kb)):
            self.occ_carve_min_kb = carve
            self.occ_at_carve_min = {"tick": tick, "carve_free_kb": carve,
                                     "cps": list(cps), "slab_ids": list(ids),
                                     "empty_nonboot": empty, "reclaimable": reclaim,
                                     "dll": dll, "dlb": dlb}

    def finish(self):
        """Run end: a snapshot whose events never arrived is missing evidence,
        and so is a run that never carried one current-schema snapshot."""
        if self.historical:
            return
        if self.pending:
            self.report("column pool events missing for heartbeat-stamped seq %d"
                        % self.pending[0][0])
        if self.hb == 0:
            self.report("no current-schema column pool evidence in the run")

    def certifying(self):
        """R5 (Sol R4 Major 1): only current-schema, seq-reconciled evidence
        may certify. A historical (pre-cpv3) tape is graded for what it
        carries but can never yield PASS.json."""
        return not self.historical and self.hb > 0

    def summary(self):
        high = max(self.high_water, self.hb_slabs_max)
        occ = None
        if self.occ_samples:
            occ = {
                "samples": self.occ_samples,
                "slab_identity": self.occ_identity,
                "slab_min_live": {str(k): v for k, v in sorted(
                    self.occ_min.items(), key=lambda kv: (str(type(kv[0])), kv[0]))},
                "empty_nonboot_slabs_max": self.occ_empty_nonboot_max,
                "reclaimable_slabs_max": self.occ_reclaimable_max,
                "reclaimable_bytes_max": self.occ_reclaimable_max * COLUMN_POOL_SLAB_BYTES,
                "reclaimable_rule": "empty non-boot slab AND >= %d free slots after"
                                    % COLUMN_POOL_SHRINK_HEADROOM,
                "heartbeats_reclaimable": self.occ_reclaimable_hb,
                "measured_heartbeats_reclaimable": self.occ_reclaimable_measured_hb,
                "at_measured_carve_min": self.occ_at_carve_min,
                "dl_bytes_max": self.dl_bytes_max,
                "dl_lists_max": self.dl_lists_max,
            }
        return {
            "events": self.events,
            "evidence": ("historical (pre-cpv%d tape; NOT certifying)" % COLUMN_POOL_CPV
                         if self.historical else
                         "current (cpv %d, seq-reconciled)" % COLUMN_POOL_CPV
                         if self.hb else "unavailable"),
            "schemas_seen": sorted(self.hb_schema),
            "slabs_boot": self.boot,
            "slabs_reconstructed": self.slabs,
            "slabs_high_water": high or None,
            "grows_ok": self.grows_ok,
            "grows_failed": self.grows_failed,
            "shrinks": self.shrinks,
            "shrunk_bytes": self.shrunk_bytes,
            "seq_gaps": self.seq_gaps,
            "grow_min_largest_allocatable_bytes": self.grow_min_largest,
            "heartbeats": self.hb,
            "snapshots_reconciled": self.resolved,
            "snapshots_pending": len(self.pending),
            "hb_slabs_max": self.hb_slabs_max if self.hb else None,
            "hb_peak_max": self.hb_peak_max if self.hb else None,
            "overflow_cumulative": self.hb_overflow if self.hb else None,
            "carve_fail_cumulative": self.hb_carve_fail if self.hb else None,
            "event_stamp_last": self.hb_cpe_last,
            "last": self.hb_last,
            "occupancy": occ,
            "allocator_cost": self.pac_summary(),
            "dl_owners": ({"samples": self.dlo_samples, "max": self.dlo_max,
                           "at_measured_carve_min": self.dlo_at_carve_min,
                           "at_dlb_max": self.dlo_at_dlb_max}
                          if self.dlo_samples else None),
        }


REGROW_PROFILE = "split_regrow_8m"
REGROW_PHASES = ("separated", "reunited", "reseparated")
REGROW_REUNITE_MS = 90000
REGROW_RESEPARATE_MS = 180000
REGROW_CARVE_FLOOR_KB = 2246
REGROW_MEM1_FLOOR_KB = 4096


class RegrowEvidence:
    """colcap-1006 (ASTRA-CC-R5 item 3): split_regrow_8m.

    The device moves the synthetic second anchor on the readiness clock:
    separated [0, 90 s) -> reunited [90, 180 s) -> reseparated to the end,
    announcing each transition ("anchor_cycle", with the pool slab count).
    PASS needs, on current-schema seq-reconciled pool evidence (the same
    ColumnPoolEvidence the anchor profiles use, gates on):
      * both transitions, in order, on time, accepted by the server;
      * DRAIN while reunited: at least one slab hand-back and the pool down
        to <= 6 slabs;
      * WARMED REGROWTH while reseparated: successful demand carves back to
        >= 7 slabs (after the mesh churn of the first two phases), and no
        failed carve anywhere;
      * no OOM / spill / truncation in the measured window, the measured
        carve minimum >= 2246 KiB and MEM1 >= 4096 KiB;
      * the allocator cost ("pac") on every measured heartbeat."""

    def __init__(self):
        self.errors = []
        self.pool = ColumnPoolEvidence(REGROW_PROFILE, self._err)
        self.cycle = []
        self.phase = "separated"
        self.measuring = False
        self.events = []        # (phase, kind, seq, slabs_after, success, largest, requested)
        self.hb_slabs = {p: [] for p in REGROW_PHASES}
        self.carve_min = None
        self.mem1_min = None
        self.run_faults = None
        self.last_ms = 0
        self.pac_missing = 0
        self.measured_hb = 0
        self.dlb_at_regrow = None

    def _err(self, msg):
        if msg not in self.errors:
            self.errors.append(msg)

    def note_cycle(self, ev):
        phase, ms = ev.get("phase"), ev.get("anchor_ms")
        want = REGROW_PHASES[len(self.cycle) + 1] if len(self.cycle) < 2 else None
        if phase != want or not _u32(ms) or ev.get("ok") is not True:
            self._err("anchor cycle transition out of order or refused: %s" % json.dumps(ev, sort_keys=True))
            return
        need = REGROW_REUNITE_MS if phase == "reunited" else REGROW_RESEPARATE_MS
        if not need <= ms < need + 5000:
            self._err("anchor cycle %s at %d ms (expected %d)" % (phase, ms, need))
        self.cycle.append({"phase": phase, "anchor_ms": ms, "offset": ev.get("offset"),
                           "slabs": ev.get("slabs"), "used": ev.get("used")})
        self.phase = phase

    def note_event(self, ev):
        self.pool.note_event(ev)
        kind = ev.get("event")
        self.events.append({"phase": self.phase, "kind": kind, "seq": ev.get("seq"),
                            "slabs_after": ev.get("slabs_after"),
                            "success": ev.get("success"),
                            "largest_before": ev.get("largest_allocatable_bytes_before"),
                            "free_before": ev.get("free_bytes_before"),
                            "requested": ev.get("requested_bytes")})

    def note_heartbeat(self, hb):
        self.pool.note_heartbeat(hb)
        if hb.get("anchor_measuring") is not True:
            if self.measuring:
                self._err("measurement latch regressed")
            return
        self.measuring = True
        self.measured_hb += 1
        ms = hb.get("anchor_ms")
        if _u32(ms):
            self.last_ms = max(self.last_ms, ms)
        slabs = hb.get("column_pool_slabs")
        if _u32(slabs):
            self.hb_slabs[self.phase].append(slabs)
        for key, attr in (("mem2_carve_free_kb", "carve_min"), ("mem1_free_kb", "mem1_min")):
            v = hb.get(key)
            if type(v) is int and v >= 0:
                cur = getattr(self, attr)
                setattr(self, attr, v if cur is None else min(cur, v))
        faults = hb.get("anchor_run_faults")
        if isinstance(faults, list) and len(faults) == 3 and all(_u32(v) for v in faults):
            # Cumulative device counters: keep the worst seen, and a value
            # going DOWN is itself a violation (a reset hides a fault).
            if self.run_faults is not None and any(
                    a < b for a, b in zip(faults, self.run_faults)):
                self._err("anchor_run_faults regressed")
            self.run_faults = (list(faults) if self.run_faults is None else
                               [max(a, b) for a, b in zip(faults, self.run_faults)])
        else:
            self._err("missing or malformed anchor_run_faults")
        if "pac" not in hb:
            self.pac_missing += 1
        if (self.phase == "reseparated" and self.dlb_at_regrow is None
                and _u32(slabs) and slabs >= 7):
            self.dlb_at_regrow = hb.get("dlb")

    def _phase_events(self, phase, kind):
        return [e for e in self.events if e["phase"] == phase and e["kind"] == kind]

    def complete_error(self):
        self.pool.finish()
        errs = list(self.errors)
        if not self.pool.certifying():
            errs.append("no current-schema pool evidence")
        if [c["phase"] for c in self.cycle] != ["reunited", "reseparated"]:
            errs.append("anchor cycle incomplete (%s)" % [c["phase"] for c in self.cycle])
        shrinks = self._phase_events("reunited", "column_pool_shrink")
        reunited = self.hb_slabs["reunited"]
        if not shrinks or not reunited or min(reunited) > 6:
            errs.append("no drain while reunited (hand-backs %d, min slabs %s)"
                        % (len(shrinks), min(reunited) if reunited else None))
        grows = self._phase_events("reseparated", "column_pool_grow")
        if not any(g["success"] and _u32(g["slabs_after"]) and g["slabs_after"] >= 7
                   for g in grows):
            errs.append("no warmed regrowth to >= 7 slabs after reseparating")
        if any(e["kind"] == "column_pool_grow" and e["success"] is False for e in self.events):
            errs.append("a column slab carve failed")
        if self.run_faults is None or any(self.run_faults):
            errs.append("OOM/spill/truncation in the measured window (%s)" % self.run_faults)
        if self.carve_min is None or self.carve_min < REGROW_CARVE_FLOOR_KB:
            errs.append("carve minimum %s KiB < %d" % (self.carve_min, REGROW_CARVE_FLOOR_KB))
        if self.mem1_min is None or self.mem1_min < REGROW_MEM1_FLOOR_KB:
            errs.append("MEM1 minimum %s KiB < %d" % (self.mem1_min, REGROW_MEM1_FLOOR_KB))
        if self.pac_missing or not self.pool.pac_samples:
            errs.append("allocator cost (pac) missing on %d measured heartbeats" % self.pac_missing)
        if self.last_ms < 300000:
            errs.append("measured window too short (%d ms)" % self.last_ms)
        return "; ".join(errs[:8]) if errs else None

    def summary(self):
        regrow = [e for e in self._phase_events("reseparated", "column_pool_grow")]
        return {
            "cycle": self.cycle,
            "slabs_by_phase": {p: ({"min": min(v), "max": max(v)} if v else None)
                               for p, v in self.hb_slabs.items()},
            "drain_handbacks": len(self._phase_events("reunited", "column_pool_shrink")),
            "regrow_events": regrow,
            "regrow_reached_slabs": max((e["slabs_after"] for e in regrow
                                         if e["success"] and _u32(e["slabs_after"])), default=None),
            "regrow_min_largest_allocatable_bytes": min(
                (e["largest_before"] for e in regrow if _u32(e["largest_before"])), default=None),
            "dlb_at_regrow": self.dlb_at_regrow,
            "carve_min_kb": self.carve_min,
            "mem1_min_kb": self.mem1_min,
            "run_faults_oom_spill_trunc": self.run_faults,
            "measured_ms": self.last_ms,
            "allocator_cost": self.pool.pac_summary(),
            "pool": self.pool.summary(),
            "errors": list(self.errors),
        }


POOL_SELFTEST_PROFILE = "pool_selftest_3m"
POOL_SELFTEST_BALLAST = 48
POOL_SELFTEST_ZERO = ("lowest_bad", "pattern_bad", "owner_bad", "addr_bad",
                      "used_bad", "canary_delta", "diag_bad")
POOL_SELFTEST_U32 = ("ops", "allocs", "frees", "grows", "shrinks", "shrink_fail",
                     "slabs_start", "slabs_peak", "lowest_checks",
                     "pattern_checks", "alloc_n", "alloc_us", "alloc_us_max",
                     "free_n", "free_us", "free_us_max", "ballast") + POOL_SELFTEST_ZERO
GX_SCENE_HEX = ("gx", "gx_set", "pos", "content", "content_set")


class PoolSelftestEvidence:
    """colcap-1006 (ASTRA-CC-R5 item 2): pool_selftest_3m grading.

    Three events, in this order, exactly once each:
      pool_selftest phase=churn  the boot churn through the REAL column pool:
          ok, zero violations of every kind, the forced hand-back refusal
          kept its slab (shrink_fail >= 1) and the kept slab was reused, the
          pool grew 4 -> 7 and ended with the 48-object ballast;
      gx_scene_hash              three identical settled frames (stable),
          non-empty (lists, chunks, content chunks), well-formed hashes;
      pool_selftest phase=final  the ballast unchanged after the whole run,
          no canary/validator hit, status ok, the game made column takes.
    All three must name the same placement. A run that PASSes here proves
    the heap/failure path on this build and placement; GX EQUIVALENCE needs
    the cross-run comparison (gx_compare)."""

    def __init__(self):
        self.churn = None
        self.scene = None
        self.final = None
        self.errors = []

    def _err(self, msg):
        if msg not in self.errors:
            self.errors.append(msg)

    def note(self, ev):
        kind = ev.get("event")
        if kind == "pool_selftest":
            phase = ev.get("phase")
            if phase == "churn":
                if self.churn is not None:
                    self._err("duplicate pool_selftest churn event")
                self.churn = ev
                self._check_churn(ev)
            elif phase == "final":
                if self.final is not None:
                    self._err("duplicate pool_selftest final event")
                if self.churn is None or self.scene is None:
                    self._err("pool_selftest final before churn/scene")
                self.final = ev
                self._check_final(ev)
            else:
                self._err("malformed pool_selftest event (phase)")
        elif kind == "gx_scene_hash":
            if self.scene is not None:
                self._err("duplicate gx_scene_hash event")
            if self.churn is None:
                self._err("gx_scene_hash before the churn event")
            self.scene = ev
            self._check_scene(ev)

    def _placement(self, ev):
        place = ev.get("placement")
        if place not in ("lowest", "rotating"):
            self._err("malformed placement")
        elif self.churn is not None and place != self.churn.get("placement"):
            self._err("placement changed between pool_selftest events")
        return place

    def _check_churn(self, ev):
        if any(not _u32(ev.get(k)) for k in POOL_SELFTEST_U32) or type(ev.get("ok")) is not bool:
            self._err("malformed pool_selftest churn event")
            return
        place = self._placement(ev)
        if not ev["ok"]:
            self._err("boot churn failed on the device: %s" % ev.get("reason"))
        for k in POOL_SELFTEST_ZERO:
            if ev[k]:
                self._err("boot churn %s=%d" % (k, ev[k]))
        if ev.get("heap_valid") is not True:
            self._err("carve heap walk invalid after the churn")
        if ev["shrink_fail"] < 1 or ev.get("refusal_kept_slab") is not True:
            self._err("forced hand-back refusal not exercised or slab not kept")
        if ev.get("kept_slab_reused") is not True:
            self._err("refused (kept) slab was not reused")
        if ev["slabs_start"] != COLUMN_POOL_BOOT_SLABS or ev["slabs_peak"] < 7:
            self._err("churn did not grow the pool 4 -> 7 (start %d peak %d)"
                      % (ev["slabs_start"], ev["slabs_peak"]))
        if ev["grows"] < 3 or ev["shrinks"] < 3:
            self._err("churn did not exercise grow AND hand-back (grows %d shrinks %d)"
                      % (ev["grows"], ev["shrinks"]))
        if place == "lowest" and ev["lowest_checks"] < ev["allocs"]:
            self._err("lowest-first placement not checked on every take")
        if ev["ballast"] != POOL_SELFTEST_BALLAST:
            self._err("ballast is %d objects, need %d" % (ev["ballast"], POOL_SELFTEST_BALLAST))
        if not ev["alloc_n"] or not ev["free_n"] or ev["pattern_checks"] < ev["frees"]:
            self._err("churn did not measure/verify its operations")

    def _check_scene(self, ev):
        self._placement(ev)
        if (any(not isinstance(ev.get(k), str) or len(ev[k]) != 16
                or any(c not in "0123456789abcdef" for c in ev[k]) for k in GX_SCENE_HEX)
                or any(not _u32(ev.get(k)) for k in ("lists", "bytes", "chunks",
                                                      "content_chunks", "attempts", "ptakes"))
                or not isinstance(ev.get("pdigest"), str) or len(ev["pdigest"]) != 8
                or not isinstance(ev.get("cam"), list) or len(ev["cam"]) != 5):
            self._err("malformed gx_scene_hash event")
            return
        if ev.get("stable") is not True:
            self._err("GX scene hash never stable over 3 consecutive settled frames")
        if not ev["lists"] or not ev["chunks"] or not ev["content_chunks"]:
            self._err("GX scene hash covered no chunk display lists")
        if not ev["ptakes"]:
            self._err("the game made no column takes after the churn")

    def _check_final(self, ev):
        if (any(not _u32(ev.get(k)) for k in ("ballast", "ballast_bad", "canary_delta",
                                               "diag_bad", "ptakes"))
                or type(ev.get("ok")) is not bool):
            self._err("malformed pool_selftest final event")
            return
        self._placement(ev)
        if not ev["ok"] or ev.get("status") != "ok":
            self._err("self-test final status %s" % ev.get("status"))
        if ev["ballast"] != POOL_SELFTEST_BALLAST:
            self._err("final ballast count %d" % ev["ballast"])
        for k in ("ballast_bad", "canary_delta", "diag_bad"):
            if ev[k]:
                self._err("final %s=%d (ballast/guard damaged during the run)" % (k, ev[k]))

    def complete_error(self):
        if self.errors:
            return "; ".join(self.errors[:8])
        if self.churn is None:
            return "no pool_selftest churn event (the .dol never ran the self-test)"
        if self.scene is None:
            return "no gx_scene_hash event (the scene never settled)"
        if self.final is None:
            return "no pool_selftest final event"
        return None

    def summary(self):
        c = self.churn or {}
        return {
            "placement": c.get("placement"),
            "churn": self.churn,
            "scene": self.scene,
            "final": self.final,
            "alloc_us_avg": (round(c["alloc_us"] / c["alloc_n"], 2)
                             if _u32(c.get("alloc_us")) and c.get("alloc_n") else None),
            "free_us_avg": (round(c["free_us"] / c["free_n"], 2)
                            if _u32(c.get("free_us")) and c.get("free_n") else None),
            "errors": list(self.errors),
            "verdict_scope": ("heap/failure path proven for this build and "
                              "placement; ordered-GX equivalence needs --gx-compare "
                              "of a rotating and a lowest run"),
        }


def _pool_selftest_record(run_dir):
    """The certified pool_selftest summary of one run dir: PASS.json only."""
    path = Path(run_dir) / "PASS.json"
    if not path.is_file():
        return None, "no certifying PASS.json in %s" % run_dir
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("status") != "PASS" or data.get("profile") != POOL_SELFTEST_PROFILE:
        return None, "%s is not a pool_selftest_3m PASS" % path
    rec = data.get("pool_selftest") or {}
    if not rec.get("scene") or not rec.get("final"):
        return None, "%s carries no pool_selftest scene" % path
    return rec, None


def gx_compare(dir_a, dir_b):
    """colcap-1006 (ASTRA-CC-R5 item 2b): the ordered GX equivalence verdict
    across two pool_selftest_3m runs of the SAME build, one per placement.
    PASS only when both runs certified, placements and placement digests
    differ, the rendered content and chunk order are identical, and the
    ordered display-list hash is identical. A content or order difference
    cannot blame placement: INCONCLUSIVE (re-run), never PASS."""
    recs = []
    for d in (dir_a, dir_b):
        rec, why = _pool_selftest_record(d)
        if rec is None:
            return {"status": "FAIL", "reason": why}
        recs.append(rec)
    a, b = recs
    sa, sb = a["scene"], b["scene"]
    out = {"runs": [str(dir_a), str(dir_b)],
           "placements": [a["placement"], b["placement"]],
           "build_ids": [a.get("build_id"), b.get("build_id")],
           "gx": [sa["gx"], sb["gx"]], "gx_set": [sa["gx_set"], sb["gx_set"]],
           "pos": [sa["pos"], sb["pos"]], "content": [sa["content"], sb["content"]],
           "lists": [sa["lists"], sb["lists"]], "bytes": [sa["bytes"], sb["bytes"]],
           "pdigest": [sa["pdigest"], sb["pdigest"]], "cam": [sa["cam"], sb["cam"]]}

    def verdict(status, reason):
        out["status"], out["reason"] = status, reason
        return out
    if not a.get("build_id") or a.get("build_id") != b.get("build_id"):
        return verdict("FAIL", "the two runs are not the same build")
    if sorted([a["placement"], b["placement"]]) != ["lowest", "rotating"]:
        return verdict("FAIL", "need one lowest and one rotating run")
    if sa["pdigest"] == sb["pdigest"]:
        return verdict("INCONCLUSIVE", "placement digests equal: columns did not "
                       "land differently, nothing was compared")
    if sa["cam"] != sb["cam"]:
        return verdict("INCONCLUSIVE", "camera differs between runs")
    if sa["content"] != sb["content"] or sa["content_set"] != sb["content_set"]:
        return verdict("INCONCLUSIVE", "rendered world content differs between runs "
                       "(random ticks/fluids); re-run")
    if sa["pos"] != sb["pos"]:
        if sa["gx_set"] != sb["gx_set"]:
            return verdict("FAIL", "same content, different per-chunk display-list "
                           "bytes under the other placement")
        return verdict("INCONCLUSIVE", "same content and per-chunk lists, different "
                       "render order between runs; re-run")
    if sa["gx"] != sb["gx"] or sa["lists"] != sb["lists"] or sa["bytes"] != sb["bytes"]:
        return verdict("FAIL", "same content and order, different ordered GX "
                       "submissions under the other placement")
    return verdict("PASS", "identical ordered GX display-list submissions under "
                   "both placements (content, order and bytes)")


class AnchorEvidence:
    """Raw per-frame samples; loading and the post-readiness window stay separate.

    colcap-1006 (ASTRA-CC-R3 item 2): every gate violation is ACCUMULATED.
    `self.error = msg` appends msg (once) to `self.errors` instead of
    overwriting the previous verdict, so a run that both spills in the
    measured window AND drops below the carve reserve reports both; reading
    `self.error` returns them joined in the order they first fired."""

    @property
    def error(self):
        if not self.errors:
            return None
        text = "; ".join(self.errors)
        if self.errors_overflow:
            # R4 point 10 (Sol R3 Minor): an honest count, not a retained set
            # -- repeats of an unretained message are not distinguished.
            text += ("; (+%d further violation reports beyond the %d retained; "
                     "repeats not distinguished)" % (self.errors_overflow,
                                                     ANCHOR_MAX_ERRORS))
        return text

    @error.setter
    def error(self, message):
        if message is None:
            self.errors = []
            self.errors_overflow = 0
        elif message not in self.errors:
            if len(self.errors) < ANCHOR_MAX_ERRORS:
                self.errors.append(message)
            else:
                self.errors_overflow += 1

    def _report(self, message):
        self.error = message

    def __init__(self, profile="split_anchor_6m"):
        self.profile = profile
        self.frames = []
        self.last_total = 0
        self.previous_frame_hb = None
        self.coverage_gaps = []
        self.errors = []
        self.errors_overflow = 0
        # colcap-1006 (R3 items 2-3, R4 points 9-10): the physical column
        # pool, reconstructed from ordered events and reconciled against
        # seq-stamped snapshots (ColumnPoolEvidence).
        self.pool = ColumnPoolEvidence(profile, self._report)
        self.first_measuring_hb = None
        self.split_reach = 0
        self.server_radius = 0
        self.first_tick = self.first_ms = None
        self.last_tick = self.last_ms = None
        self.oldest_max = [0, 0]
        self.far_max = [0, 0]
        self.last = {}
        self.fault_baseline = None
        self.loading_spill_max = 0
        # S1-d: last seen per-owner accepted-action tallies, [host, guest].
        # None until the first heartbeat carrying them.
        self.actions = [None, None]

    def note_actions(self, hb):
        """S1-d: the per-owner agency evidence, checked every heartbeat.

        Three separate claims, each fatal on its own:
          * the act0/act1 arrays are present and well formed (a missing array
            is not "zero actions", it is a build that cannot answer the
            question this profile exists to ask);
          * neither owner's tally ever goes DOWN -- counters are monotonic
            server-side, so a decrease means either a wrapped counter or a
            guest body that left and rejoined mid-run, and in both cases the
            end-of-run totals no longer describe one continuous session;
          * the HOST performed no action at all. The whole point of the run is
            that the guest acts on its own actor; a nonzero act0 means either
            the harness drove the wrong body or an owner-routing bug sent the
            guest's requests to the host, which is exactly the defect this
            profile is the detector for.
        Per-owner health is checked here too, because the profile's claim is
        about two LIVE bodies: -1 (no body) and 0 (dead) are both failures
        once the measured window has started."""
        for owner in (0, 1):
            row = hb.get("act%d" % owner)
            if (not isinstance(row, list)
                    or len(row) != len(SPLIT_ACTION_NAMES)
                    or any(type(v) is not int or not 0 <= v <= 0xffffffff
                           for v in row)):
                self.error = "missing or malformed per-owner action evidence"
                return
            previous = self.actions[owner]
            if previous is not None and any(a < b for a, b in zip(row, previous)):
                self.error = ("per-owner action counters regressed for owner "
                              "%d (%s -> %s)" % (owner, previous, row))
                return
            self.actions[owner] = list(row)
        if any(self.actions[0]):
            self.error = ("the host performed actions during a guest-action "
                          "run (act0=%s)" % self.describe_actions(0))
            return
        health = hb.get("act_health")
        if (not isinstance(health, list) or len(health) != 2
                or any(type(v) is not int for v in health)):
            self.error = "missing or malformed per-owner health evidence"
            return
        if hb.get("anchor_measuring") and any(v <= 0 for v in health):
            self.error = ("an owner was not alive during the measured window "
                          "(act_health=%s; -1 = no body, 0 = dead)" % health)

    def describe_actions(self, owner):
        row = self.actions[owner]
        if row is None:
            return None
        return dict(zip(SPLIT_ACTION_NAMES, row))

    def note_pool_event(self, ev):
        self.pool.note_event(ev)

    def pool_summary(self):
        return self.pool.summary()

    def note(self, hb):
        self.last = hb
        if self.profile == SPLIT_ACTION_PROFILE:
            self.note_actions(hb)
        self.pool.note_heartbeat(hb)
        if self.first_tick is not None and hb.get("anchor_measuring") is not True:
            self.error = "measurement latch regressed"
        if hb.get("anchor_measuring") and self.first_measuring_hb is None:
            self.first_measuring_hb = hb.get("tick")
        if type(hb.get("server_radius")) is int and hb["server_radius"]:
            if self.server_radius and hb["server_radius"] != self.server_radius and hb.get("anchor_measuring"):
                self.error = "server radius changed during the measured window (%d -> %d)" % (
                    self.server_radius, hb["server_radius"])
            self.server_radius = hb["server_radius"]
        if type(hb.get("split_reach")) is int and hb["split_reach"]:
            if self.split_reach and hb["split_reach"] != self.split_reach:
                # A reduced-reach PASS certifies ONE presentation (Astra B).
                self.error = "presentation changed during the run (split_reach %d -> %d)" % (
                    self.split_reach, hb["split_reach"])
            self.split_reach = hb["split_reach"]
        budget = SPLIT_READY_BUDGET.get(self.profile)
        if (budget is not None and not hb.get("anchor_measuring")
                and type(hb.get("tick")) is int and hb["tick"] > budget):
            self.error = f"installed readiness not reached within the {budget} s budget"
        keys = ("anchor_ms", "anchor_frame_total", "anchor_frame_dropped", "server_time",
                "tick_backlog", "anchor_load_carve_min", "anchor_run_carve_min")
        if any(type(hb.get(k)) is not int for k in keys):
            self.error = "missing anchor evidence"
            return
        for k in ("anchor_near_owed", "anchor_oldest", "anchor_missing",
                  "anchor_mesh_pending", "anchor_far_missing", "anchor_far_max"):
            if not isinstance(hb.get(k), list) or len(hb[k]) != 2 or any(
                    type(v) is not int or v < 0 for v in hb[k]):
                self.error = "invalid anchor debt evidence"
                return
        self.note_frames(hb)
        load, run = hb.get("anchor_load_faults"), hb.get("anchor_run_faults")
        if any(not isinstance(a, list) or len(a) != 3 or any(
                type(v) is not int or not 0 <= v <= 0xffffffff for v in a)
               for a in (load, run)):
            self.error = "missing loading/window fault counters"
            return
        self.loading_spill_max = max(self.loading_spill_max, load[1])
        if load[0] or load[2]:
            self.error = "allocation failure or truncation during loading"
        if hb.get("anchor_measuring"):
            totals = [hb.get(k) for k in ("oom", "spill", "mesh_trunc")]
            if any(type(v) is not int or not 0 <= v <= 0xffffffff for v in totals):
                self.error = "missing allocation failure, spill or truncation counters"
                return
            if self.fault_baseline is None:
                self.fault_baseline = load[:]
            if load != self.fault_baseline or any(
                    (v - base) & 0xffffffff != delta
                    for v, base, delta in zip(totals, load, run)):
                self.error = "anchor fault boundary changed or counters disagree"
            if any(run):
                self.error = "allocation failure, spill or truncation in measured window"
            if (hb.get("near_radius"), hb.get("far_view_distance"), hb.get("far_storage")) != (2, 6, 0):
                self.error = "split anchor hardware protocol requires near2/far6 voxel"
            if self.first_tick is None:
                # A guest-body profile has TWO real bodies even with the
                # synthetic separation at 0 (Astra ruling 2026-09-10).
                count = 2 if (hb.get("anchor_sep", 0)
                              or self.profile in SPLIT_GUEST) else 1
                pending = hb.get("anchor_far_mesh_pending")
                if (hb.get("anchor_ready") is not True
                        or hb.get("anchor_ready_streak") != 3
                        or not isinstance(pending, list) or len(pending) != 2
                        or any(type(v) is not int or v != 0 for v in pending[:count])
                        or any(hb.get(k) != 0 for k in
                               ("near_missing", "near_mesh_pending", "far_missing"))
                        or any(any(hb[k][:count]) for k in
                               ("anchor_missing", "anchor_mesh_pending", "anchor_far_missing"))):
                    self.error = "measurement began before stable installed readiness"
                self.first_tick, self.first_ms = hb["server_time"], hb["anchor_ms"]
            self.last_tick, self.last_ms = hb["server_time"], hb["anchor_ms"]
            for p in range(2):
                self.oldest_max[p] = max(self.oldest_max[p], hb["anchor_oldest"][p])
                self.far_max[p] = max(self.far_max[p], hb["anchor_far_max"][p])
            carve_floor = SPLIT_CARVE_FLOOR_KB
            if hb["anchor_run_carve_min"] < carve_floor:
                self.error = f"dedicated carve below {carve_floor} KiB reserve"

    def note_frames(self, hb):
        frames = hb.get("anchor_frame_us")
        if not isinstance(frames, list) or any(type(v) is not int or v <= 0 for v in frames):
            self.error = "invalid anchor frame evidence"
            return
        total = hb.get("anchor_frame_total")
        if type(total) is not int or type(hb.get("anchor_frame_dropped")) is not int:
            self.error = "invalid anchor frame sequence"
            return
        missing = total - self.last_total - len(frames)
        previous = self.previous_frame_hb
        zero_counters = ("anchor_frame_dropped", "oom", "spill", "mesh_trunc")
        device_counters = ("tel_drop", "tel_fail", "tel_wto", "tel_abd",
                           "tel_hb_repl")
        no_device_fault = (previous is not None
                           and all(type(side.get(k)) is int and side[k] == 0
                                   for side in (previous, hb) for k in zero_counters)
                           and all(type(previous.get(k)) is int
                                   and type(hb.get(k)) is int
                                   and hb[k] == previous[k]
                                   for k in device_counters))
        no_anchor_fault = all(
            isinstance(side.get(k), list) and len(side[k]) == 3
            and all(type(v) is int and v == 0 for v in side[k])
            for side in (previous, hb)
            for k in ("anchor_load_faults", "anchor_run_faults")
        ) if previous is not None else False
        if (self.profile == "split_play_8m" and self.coverage_gaps
                and (not no_device_fault or not no_anchor_fault)):
            self.error = "device drop or fault after telemetry coverage gap"
        same_boundary = (previous is not None
                         and previous.get("anchor_measuring") is True
                         and hb.get("anchor_measuring") is True
                         and all(previous.get(k) == hb.get(k) for k in
                                 ("server_radius", "split_reach", "near_radius",
                                  "far_view_distance", "far_storage")))
        eligible = (self.profile == "split_play_8m" and missing > 0
                    and len(self.coverage_gaps) == 0 and no_device_fault
                    and no_anchor_fault and same_boundary
                    and type(previous.get("tick")) is int
                    and type(hb.get("tick")) is int
                    and hb["tick"] - previous["tick"] == 2
                    and total >= self.last_total)
        if eligible:
            self.coverage_gaps.append({"missing_tick": previous["tick"] + 1,
                                       "missing_frames": missing,
                                       "interval_ticks": [previous["tick"], hb["tick"]],
                                       "interval_ms": [previous.get("anchor_ms"),
                                                       hb.get("anchor_ms")]})
        elif hb["anchor_frame_dropped"] or missing != 0:
            self.error = "missing per-frame samples"
        self.last_total = total
        self.previous_frame_hb = hb
        self.frames.extend(frames)

    def complete_error(self):
        self.pool.finish()
        if self.error:
            return self.error
        if not self.last.get("anchor_measuring"):
            return "installed readiness never reached (no measured window)"
        if self.profile == "split_play_2m":
            # The 120 s smoke: readiness by 90 s of the run clock and at
            # least 30 s measured after it; the safety checks (OOM /
            # truncation / window spills / reserve) already ran per beat.
            if self.last.get("anchor_ms", 0) < 30000:
                return "need at least 30 measured seconds after installed readiness"
        elif self.profile == SPLIT_RECOVERY_PROFILE:
            # No measured-duration floor, on purpose. This profile's
            # completion is the MARKER CONTRACT (docs/split_recovery_profile.md
            # sections 1 and 6): the generic duration completion is disabled
            # device-side, the driver ends the run the moment P4 verifies, and
            # its own hard cap is 355 s -- so a six-minute measured window is
            # unreachable by construction and demanding one would make the
            # profile unpassable. The per-beat safety checks above (readiness
            # budget, OOM/spill/truncation, carve reserve, near2/far6) still
            # apply exactly as they do to every other split profile; what
            # certifies the run is RecoveryEvidence.complete_error() below.
            pass
        elif self.last.get("anchor_ms", 0) < 360000:
            return "need six measured minutes after installed readiness"
        # Completion is checked on unresolved debt at the end, including
        # attempted/backed-off work. T=20 only changes selection priority.
        if max(self.last.get("anchor_oldest", [0, 0])) > 1200:
            return "unresolved residency debt exceeded completion bound (1200 ticks)"
        if not self.frames:
            return "missing per-frame evidence"
        if self.profile == SPLIT_ACTION_PROFILE:
            guest = self.describe_actions(1)
            if guest is None:
                return "no per-owner action evidence was ever received"
            for name, floor in sorted(SPLIT_ACTION_FLOORS.items()):
                if guest[name] < floor:
                    return ("guest %s actions %d < %d required (act1=%s) -- "
                            "the scripted guest effects are absent"
                            % (name, guest[name], floor, guest))
            if any(self.actions[0]):
                return ("the host was not action-idle (act0=%s)"
                        % self.describe_actions(0))
        return None

    def summary(self):
        ordered = sorted(self.frames)
        # Percentile of instantaneous FPS, calculated from every frame interval.
        fps = sorted(1e6 / v for v in ordered)
        def percentile(p):
            return fps[min(len(fps)-1, int((len(fps)-1)*p))] if fps else None
        dt = (self.last_ms - self.first_ms) if self.first_ms is not None else 0
        missing_frames = sum(g["missing_frames"] for g in self.coverage_gaps)
        return {"error": self.error, "errors": list(self.errors),
                "column_pool": self.pool_summary(), "frames": len(fps),
                "coverage_gaps": self.coverage_gaps,
                "inconclusive_windows": len(self.coverage_gaps),
                "missing_ticks": [g["missing_tick"] for g in self.coverage_gaps],
                "missing_frames": missing_frames,
                "frame_coverage": (len(fps) / (len(fps) + missing_frames)
                                   if fps or missing_frames else None),
                "first_measuring_hb": self.first_measuring_hb,
                # Presentation protocol: 0 = full rings (the pre-2026-09-10
                # meaning), N = split render reach N chunks (T2, reduced).
                "split_render_reach": self.split_reach,
                "server_radius": self.server_radius,
                "presentation": ("split_r%d_reach%d_faroff" % (self.server_radius, self.split_reach))
                                if self.split_reach and self.server_radius else
                                ("split_render_reach_%d" % self.split_reach)
                                if self.split_reach else "full_rings",
                "fps_p01": percentile(.01), "fps_p05": percentile(.05),
                "fps_p50": percentile(.5), "fps_p95": percentile(.95),
                "actual_tps": ((self.last_tick-self.first_tick) & 0xffffffff)*1000/dt if dt > 0 else None,
                "oldest_max_ticks": self.oldest_max, "far_missing_max": self.far_max,
                "far_missing_end": self.last.get("anchor_far_missing"),
                "load_carve_min_kib": self.last.get("anchor_load_carve_min"),
                "measured_carve_min_kib": self.last.get("anchor_run_carve_min"),
                "load_faults_oom_spill_trunc": self.last.get("anchor_load_faults"),
                "measured_faults_oom_spill_trunc": self.last.get("anchor_run_faults"),
                "warnings": ([f"Loading spill={self.loading_spill_max}: warning only; "
                              "loading OOM/truncation fail whole-run certification."]
                             if self.loading_spill_max else []),
                "deaths": self.last.get("anchor_deaths"),
                # S1-d: reported for every split profile that carries them
                # (None elsewhere), so a PASS.json states the agency evidence
                # rather than only the residency evidence. attack rides here
                # as a REPORTED figure with no threshold behind it.
                "host_actions": self.describe_actions(0),
                "guest_actions": self.describe_actions(1),
                "action_floors": (SPLIT_ACTION_FLOORS
                                  if self.profile == SPLIT_ACTION_PROFILE
                                  else None),
                "last_death_cause": self.last.get("anchor_death_cause")}


PORTAL_QUIT_PROFILE = "split_portal_quit_4m"
PORTAL_QUIT_EVENT = "split_portal_quit"
PORTAL_QUIT_SWEEP = (0, 1, 2, 3, 5)


class PortalQuitEvidence:
    """Ordered checklist-6b tape, independent of profile_done's claim."""

    def __init__(self, run_id: str, remaining_ticks: int | None = None):
        self.run_id = run_id
        self.remaining_ticks = remaining_ticks
        self.rows: list[dict] = []
        self.error: str | None = None
        self.profile_done: str | None = None
        self.recovery_blocks: dict[int, tuple] = {}
        self.server_rows: list[dict] = []

    def note_server(self, row: dict) -> None:
        if row.get("run_id") != self.run_id:
            self.error = "stale portal quit server run_id"
            return
        if (row.get("marker") not in ("unload_seen", "unload_dropped_at_barrier")
                or row.get("switch") not in ("not_begun", "begun", "barrier")
                or type(row.get("seq")) is not int or row["seq"] < 1
                or type(row.get("up_ms")) is not int or row["up_ms"] < 0
                or type(row.get("tick")) is not int or row["tick"] < 0
                or type(row.get("switch_begin_up_ms")) is not int
                or row["switch_begin_up_ms"] < 0
                or (self.server_rows and row["seq"] <= self.server_rows[-1]["seq"])):
            self.error = "malformed portal quit server marker"
            return
        self.server_rows.append(row)

    def note_recovery(self, row: dict) -> None:
        if row.get("run_id") != self.run_id:
            self.error = "stale recovery block_placed run_id"
            return
        if row.get("phase") != "p0" or row.get("marker") != "block_placed":
            return
        if self.rows:
            self.error = "recovery block_placed after portal leg began"
            return
        i = row.get("i")
        if (type(i) is not int or i not in range(3)
                or i in self.recovery_blocks or row.get("ok") is not True
                or any(type(row.get(k)) is not int
                       for k in ("x", "y", "z", "id", "meta"))
                or row["id"] <= 0):
            self.error = "invalid recovery block_placed"
            return
        self.recovery_blocks[i] = tuple(row[k] for k in
                                         ("x", "y", "z", "id", "meta"))

    def note(self, row: dict) -> None:
        if self.error:
            return
        if row.get("run_id") != self.run_id:
            self.error = "stale portal quit run_id"
            return
        name = row.get("marker")
        seq = row.get("seq")
        up_ms = row.get("up_ms")
        tick = row.get("tick")
        n = row.get("remaining_ticks")
        if (type(seq) is not int or seq != len(self.rows) + 1
                or type(up_ms) is not int or up_ms < 0
                or type(tick) is not int or tick < 0
                or type(n) is not int or n not in PORTAL_QUIT_SWEEP):
            self.error = "malformed portal quit marker payload or seq"
            return
        if self.remaining_ticks is None:
            self.remaining_ticks = n
        if n != self.remaining_ticks:
            self.error = "portal quit N changed mid-run"
            return
        if self.rows and (up_ms < self.rows[-1]["up_ms"]
                          or tick < self.rows[-1]["tick"]):
            self.error = "portal quit clock reversed"
            return
        if row.get("ok") is not True:
            self.error = str(row.get("reason") or name or "portal quit failure")
            return
        if name == "title_entered" and not any(
                r["marker"] == "save_settled" for r in self.rows):
            self.error = "title_before_save_settled"
            return
        if name == "guest_rejoined" and (
                row.get("guest_inventory_match") is not True
                or row.get("guest_phase") != "ACTIVE"):
            self.error = "guest_inventory_lost or guest stuck LEAVING"
            return
        if name == "reload_verified" and not all(
                row.get(key) is True for key in (
                    "host_inventory_match", "blocks_match",
                    "position_match", "region_intact")):
            self.error = "reload state torn or changed"
            return
        self.rows.append(row)

    def note_profile_done(self, status: str) -> None:
        self.profile_done = status

    def heartbeat_floor(self, default: int) -> int:
        if not self.rows:
            return default
        return max(default, int(0.8 * self.rows[-1]["up_ms"] / 1000.0))

    def complete_error(self) -> str | None:
        if self.error:
            return self.error
        names = [row["marker"] for row in self.rows]
        if "unload_replied_late" in names:
            return "unload_replied_late"
        if any(r["marker"] == "unload_dropped_at_barrier"
               for r in self.server_rows):
            return "unload_dropped_silently"
        if names[:2] != ["portal_lit", "dwell_trigger"]:
            return "missing or out-of-order portal/dwell/quit markers"
        if ("quit_requested" not in names
                or ("race_switch_first" in names and
                    names.index("quit_requested") > names.index("race_switch_first"))):
            return "missing or out-of-order portal/dwell/quit markers"
        if "confirm_sent" not in names or "switch_begin" not in names:
            return "race_not_reached"
        confirm = self.rows[names.index("confirm_sent")]
        switch = self.rows[names.index("switch_begin")]
        if (names.index("confirm_sent") >= names.index("switch_begin")
                or switch["tick"] <= confirm["tick"]
                or switch["up_ms"] <= confirm["up_ms"]):
            return "race_not_reached"
        if not any(r["marker"] == "unload_seen" and r["switch"] == "begun"
                   and confirm["up_ms"] < r["switch_begin_up_ms"] <= r["up_ms"]
                   for r in self.server_rows):
            return "race_not_reached"
        if set(self.recovery_blocks) != {0, 1, 2}:
            return "missing recovery block_placed markers"
        if "race_switch_first" not in names:
            return "missing race winner"
        if (self.profile_done is not None
                and not any(name in names for name in
                            ("save_settled", "save_refused"))):
            return "unload_dropped_silently"
        winner = self.rows[names.index("race_switch_first")]
        if any(winner.get(k) != v for k, v in (
                ("confirm_tick", confirm["tick"]),
                ("confirm_up_ms", confirm["up_ms"]),
                ("switch_tick", switch["tick"]),
                ("switch_up_ms", switch["up_ms"]))):
            return "race_not_reached: outcome chronology mismatch"
        tail = names[names.index("race_switch_first") + 1:]
        if tail[:1] == ["save_refused"]:
            tail = tail[1:]
            if tail[:1] != ["retry_requested"]:
                return "switch refusal without retry"
            tail = tail[1:]
        if tail != ["save_settled", "title_entered", "reload_verified",
                    "guest_rejoined", "run_ok", "run_summary"]:
            return "missing or out-of-order save/reload/guest markers"
        if self.profile_done != "ok":
            return "portal quit profile_done was not ok"
        if (self.rows[-1].get("winning_leg") != self.remaining_ticks
                or self.rows[-2].get("winning_leg") != self.remaining_ticks):
            return "missing winning_leg summary"
        return None

    def summary(self) -> dict:
        return {"error": self.error, "markers": len(self.rows),
                "remaining_ticks": self.remaining_ticks,
                "winner": next((r["marker"] for r in self.rows
                                if r["marker"].startswith("race_")), None)}


class RecoveryEvidence:
    """split_recovery_6m's verdict: the marker stream of the contract, checked
    as a sequence (docs/split_recovery_profile.md sections 4-7).

    Modelled on AnchorEvidence: every rejection latches the FIRST reason and
    stops, so a stream that goes wrong early reports why it went wrong rather
    than the pile of consequences. The claims, each fatal on its own:

      * every mandatory marker of section 5, exactly as many times as the
        contract says, in the contract's order and with the phases in the order
        p0 < p1 < p2 < p3 < p4, ending with run.ok as the LAST marker;
      * `seq` a gapless 1..N run -- section 4 promises the driver retries a
        blocked emit with the SAME seq, so the collector may assume no gaps and
        a gap is lost evidence, not a lost marker;
      * every marker ok:true. An ok:false marker (or the `fail` marker of
        section 6) surfaces its phase and its diag VERBATIM: that string is the
        only device-side diagnosis a console run produces;
      * no contradictory evidence -- a marker whose contract precondition was
        never observed (p3.title_entered before p3.world_reset, p1.resumed
        before p1.collapse_done, any p4 marker before p3.title_entered), a
        layout timing outside section 5's window, an inventory comparison that
        disagrees with the baseline it names, or a corr that answers nothing;
      * no stale-run evidence -- a marker from another run_id, a guest-session
        marker under the wrong `gen`, or a P3/P4 marker under the wrong
        `epoch`. Section 7 calls all three "stale", because each one means the
        marker describes a session other than the one being validated;
      * exactly one profile_done, with status "ok" (section 6).

    A generic duration-based completion can never satisfy this: nothing here is
    derived from the clock."""

    # Guest-session markers whose `gen` must still be the session established
    # by the marker that opened it (section 2, "markers that are tied to a
    # guest session carry it as gen so the collector can reject stale
    # evidence"), and the two that open a NEW session and must NOT reuse one.
    GEN_SAME = (("p1", "no_leave"), ("p2", "leave_requested"),
                ("p2", "leave_acked"))
    GEN_NEW = (("p2", "rejoined"), ("p4", "guest_readmitted"))

    def __init__(self, run_id: str = ""):
        self.run_id = run_id or ""
        self.error: str | None = None
        self.markers = 0
        self.count: dict[tuple[str, str], int] = {}
        self.group: dict[tuple[str, str], list] = {}
        self.seq_of: dict[tuple[str, str], int] = {}
        self.last_seq = 0
        self.last_ordinal = -1
        self.last_key: tuple[str, str] | None = None
        self.finished = False
        self.profile_done: str | None = None
        self.profile_done_count = 0
        # Correlation state carried across phases.
        self.guest_gen = None
        self.exit_epoch = None
        # inv_hash baselines the later comparisons are judged against
        # (section 3.1): actor -> hash.
        self.baseline_inv: dict[int, int] = {}
        self.pre_save_inv: dict[int, int] = {}
        self.last_ms = 0

    def heartbeat_floor(self, default: int) -> int:
        """This profile ends when its LAST assertion passes, not on a clock
        (real Wii, 2026-09-17: run.ok at 34.2 s with 36 one-per-second
        heartbeats, which a fixed floor of 60 FAILED although every marker was
        green). The telemetry-health question is CADENCE while the run lasted:
        at least 0.8 heartbeats per second of the driver's own clock, never
        fewer than 10. With no marker at all the table default applies."""
        if self.last_ms <= 0:
            return default
        return max(10, int(0.8 * self.last_ms / 1000.0))

    def note_profile_done(self, status) -> None:
        self.profile_done_count += 1
        self.profile_done = status
        if self.profile_done_count > 1 and not self.error:
            self.error = ("more than one profile_done for a single run "
                          "(section 6 sends exactly one)")

    def note(self, event: dict) -> None:
        if self.error:
            return
        if self.finished:
            self.error = ("markers continued after run.ok (seq %r) -- run.ok is "
                          "the last marker of a run" % (event.get("seq"),))
            return
        if self.run_id and event.get("run_id") != self.run_id:
            self.error = ("stale-run evidence: marker run_id=%r is not the run "
                          "being validated (%r)"
                          % (event.get("run_id"), self.run_id))
            return
        phase, marker = event.get("phase"), event.get("marker")
        seq, corr, ok = event.get("seq"), event.get("corr"), event.get("ok")
        if (not isinstance(phase, str) or not isinstance(marker, str)
                or type(seq) is not int or type(corr) is not int
                or type(ok) is not bool or type(event.get("ms")) is not int):
            self.error = ("malformed split_recovery marker: %s"
                          % json.dumps(event, sort_keys=True)[:120])
            return
        self.last_ms = max(self.last_ms, event["ms"])
        if seq <= self.last_seq:
            self.error = ("seq not strictly increasing: %s.%s carries seq=%d "
                          "after seq=%d" % (phase, marker, seq, self.last_seq))
            return
        if seq != self.last_seq + 1:
            self.error = ("seq gap before %s.%s: seq=%d follows seq=%d, but "
                          "section 4 promises a gapless 1..N run"
                          % (phase, marker, seq, self.last_seq))
            return
        self.last_seq = seq
        self.markers += 1
        # Failure markers first: they carry the only device-side diagnosis.
        if phase == "run" and marker == "timeout":
            self.error = ("run.timeout: %s"
                          % (event.get("diag") or "total timeout"))
            return
        if marker == "fail" or ok is not True:
            self.error = ("phase failure %s.%s: %s"
                          % (phase, marker, event.get("diag")))
            return
        key = (phase, marker)
        ordinal = SPLIT_RECOVERY_ORDINAL.get(key)
        if ordinal is None:
            self.error = ("unexpected marker %s.%s: not a marker of the "
                          "contract (section 5)" % (phase, marker))
            return
        prereq = SPLIT_RECOVERY_PREREQ.get(key)
        if prereq and not self.count.get(prereq):
            self.error = ("contradictory evidence: %s.%s before %s.%s"
                          % (phase, marker, prereq[0], prereq[1]))
            return
        if ordinal < self.last_ordinal:
            self.error = ("reordered markers: %s.%s arrived after %s.%s"
                          % (phase, marker, self.last_key[0], self.last_key[1]))
            return
        expected = SPLIT_RECOVERY_EXPECTED[key]
        seen = self.count.get(key, 0) + 1
        if seen > expected:
            self.error = ("duplicate marker %s.%s: seen %d times, the contract "
                          "says %d" % (phase, marker, seen, expected))
            return
        if expected > 1 and not self._note_group(key, expected, event):
            return
        if not self._note_correlation(key, seq, corr, event):
            return
        self.count[key] = seen
        self.last_ordinal = ordinal
        self.last_key = key
        self.seq_of.setdefault(key, seq)
        if key == ("run", "ok"):
            self.finished = True

    def _note_group(self, key, expected, event) -> bool:
        """block_placed/blocks_verified carry `i`, the two baselines carry
        `actor`: the group must arrive complete and in increasing index order,
        so a repeated index is a duplicate and a skipped one is a reorder."""
        phase, marker = key
        field = "actor" if marker.endswith("baseline") else "i"
        index = event.get(field)
        seen = self.group.setdefault(key, [])
        if index in seen:
            self.error = ("duplicate marker %s.%s %s=%r"
                          % (phase, marker, field, index))
            return False
        if type(index) is not int or index != len(seen):
            self.error = ("reordered markers: %s.%s %s=%r, expected %s=%d of %d"
                          % (phase, marker, field, index, field, len(seen),
                             expected))
            return False
        seen.append(index)
        if key in (("p0", "baseline"), ("p3", "pre_save_baseline")):
            # An EMPTY inventory hashes equal to itself forever, so every later
            # inventory_match would prove nothing (review 2026-09-17): the
            # contract has both actors dig and pick up before the baseline.
            used = event.get("inv_used")
            if type(used) is not int or used < 1:
                self.error = ("contradictory evidence: %s.%s actor %r inv_used=%r"
                              " -- an empty baseline proves no inventory round"
                              " trip" % (phase, marker, index, used))
                return False
        if key == ("p0", "baseline"):
            self.baseline_inv[index] = event.get("inv_hash")
            # Two equal baselines make every later per-owner comparison blind
            # to a host/guest inventory swap (first full PC pass, 2026-09-17:
            # both actors held two dirt). The driver's plan makes them differ.
            if index == 1 and self.baseline_inv.get(0) == self.baseline_inv[1]:
                self.error = ("contradictory evidence: p0.baseline actor 0 and "
                              "actor 1 share inv_hash=%r -- an ownership swap "
                              "would be invisible" % (self.baseline_inv[1],))
                return False
        elif key == ("p3", "pre_save_baseline"):
            self.pre_save_inv[index] = event.get("inv_hash")
        return True

    def _note_correlation(self, key, seq, corr, event) -> bool:
        """corr/gen/epoch (section 4) plus section 7's value contradictions."""
        phase, marker = key
        if key in SPLIT_RECOVERY_SELF_CORR:
            if corr != seq:
                self.error = ("contradictory evidence: %s.%s corr=%d must be "
                              "its own seq %d" % (phase, marker, corr, seq))
                return False
        elif corr and not 0 < corr < seq:
            self.error = ("contradictory evidence: %s.%s corr=%d answers no "
                          "earlier marker (its own seq is %d)"
                          % (phase, marker, corr, seq))
            return False
        answers = SPLIT_RECOVERY_ANSWERS.get(key)
        if answers and corr != self.seq_of.get(answers):
            self.error = ("contradictory evidence: %s.%s corr=%d does not "
                          "answer %s.%s (seq %r)"
                          % (phase, marker, corr, answers[0], answers[1],
                             self.seq_of.get(answers)))
            return False
        gen = event.get("gen")
        if key == ("p0", "guest_admitted"):
            if type(gen) is not int:
                self.error = ("malformed split_recovery marker: %s.%s carries "
                              "no integer gen" % (phase, marker))
                return False
            self.guest_gen = gen
        elif key in self.GEN_SAME:
            if gen != self.guest_gen:
                self.error = ("stale-run evidence: %s.%s carries gen=%r, but "
                              "this guest session is gen=%r"
                              % (phase, marker, gen, self.guest_gen))
                return False
        elif key in self.GEN_NEW:
            if type(gen) is not int or gen == self.guest_gen:
                self.error = ("stale-run evidence: %s.%s must report a NEW "
                              "guest generation, got gen=%r (previous %r)"
                              % (phase, marker, gen, self.guest_gen))
                return False
            self.guest_gen = gen
        epoch = event.get("epoch")
        if key == ("p3", "exit_requested"):
            if type(epoch) is not int:
                self.error = ("malformed split_recovery marker: %s.%s carries "
                              "no integer epoch" % (phase, marker))
                return False
            self.exit_epoch = epoch
        elif key == ("p3", "world_reset"):
            if epoch != self.exit_epoch:
                self.error = ("stale-run evidence: %s.%s carries epoch=%r, but "
                              "p3.exit_requested was epoch=%r"
                              % (phase, marker, epoch, self.exit_epoch))
                return False
        elif key == ("p4", "world_loaded"):
            if type(epoch) is not int or epoch == self.exit_epoch:
                self.error = ("stale-run evidence: %s.%s must report the NEW "
                              "input epoch, got epoch=%r (pre-reset %r)"
                              % (phase, marker, epoch, self.exit_epoch))
                return False
        return self._note_values(key, event)

    def _note_values(self, key, event) -> bool:
        """Section 7's contradictory READINGS: a marker that is present, in
        order and ok:true can still carry a value that disproves its own
        claim."""
        phase, marker = key
        window = None
        if key == ("p1", "collapse_begin"):
            window, field = SPLIT_RECOVERY_GRACE_MS, "grace_ms"
        elif key == ("p1", "collapse_done"):
            window, field = SPLIT_RECOVERY_TRANSITION_MS, "transition_ms"
        if window is not None:
            value = event.get(field)
            if type(value) is not int or not window[0] <= value <= window[1]:
                self.error = ("contradictory evidence: %s.%s %s=%r outside the "
                              "contract window [%d, %d]"
                              % (phase, marker, field, value, *window))
                return False
        if marker in ("inventory_match", "host_verified", "guest_verified"):
            got, want = event.get("inv_hash"), event.get("want")
            if type(got) is not int or got != want:
                self.error = ("contradictory evidence: %s.%s inv_hash=%r does "
                              "not equal want=%r" % (phase, marker, got, want))
                return False
            # ... and `want` must be the baseline the contract names, not
            # whichever hash the driver happened to keep (section 5: the guest
            # comparisons are against the P0 baseline, the host's against the
            # P3 pre-save baseline).
            source, actor = ((self.pre_save_inv, 0)
                             if marker == "host_verified"
                             else (self.baseline_inv, 1))
            if want != source.get(actor):
                self.error = ("contradictory evidence: %s.%s want=%r is not "
                              "the recorded baseline for actor %d (%r)"
                              % (phase, marker, want, actor, source.get(actor)))
                return False
        return True

    def complete_error(self) -> str | None:
        if self.error:
            return self.error
        if not self.markers:
            return ("no split_recovery markers were received -- the scripted "
                    "recovery driver never ran")
        for (phase, marker), expected in SPLIT_RECOVERY_EXPECTED.items():
            seen = self.count.get((phase, marker), 0)
            if seen < expected:
                return ("missing marker %s.%s: %d of the %d the contract "
                        "requires" % (phase, marker, seen, expected))
        if not self.finished or self.last_key != ("run", "ok"):
            return "run.ok is not the last marker of the run"
        if self.profile_done != "ok":
            return ("profile_done %s: the contract (section 6) ends a passing "
                    "run with exactly \"ok\""
                    % ("was never received"
                       if self.profile_done_count == 0
                       else "status=%r" % (self.profile_done,)))
        return None

    def summary(self) -> dict:
        return {"error": self.error, "markers": self.markers,
                "last_seq": self.last_seq,
                "phases": {phase: sum(n for (p, _m), n in self.count.items()
                                      if p == phase)
                           for phase in ("p0", "p1", "p2", "p3", "p4", "run")},
                "run_ok": self.finished,
                "profile_done": self.profile_done,
                "guest_generation": self.guest_gen,
                "exit_epoch": self.exit_epoch,
                "baseline_inv_hash": self.baseline_inv,
                "pre_save_inv_hash": self.pre_save_inv,
                # Stated on every artifact, PASS or FAIL: a PASS here is a
                # PROXY (section 0 / Astra ruling item 8).
                "proof_scope": SPLIT_RECOVERY_PASS_LABEL}


class ChatChordEvidence:
    """mp_chat_chord_1m's verdict: the marker contract IS the verdict, exactly
    as it is for split_recovery_6m and mp_chat_login_3m. Modelled on
    ChatLoginEvidence but far smaller -- no traces, no route, no server
    receipt: this profile claims one thing and claims it twice (both press
    orders), so the rejections are the readings that could disprove it.

    Every rejection latches the FIRST reason and stops:
      * no markers at all, or run.ok absent, or markers after run.ok;
      * a missing/duplicate marker, seq not a gapless 1..N, markers out of
        order, or a marker outside this contract;
      * `ok:false` anywhere -- the device's own diag is the diagnosis;
      * a p0.session that does not name a known contract or platform, or that
        did NOT start from a forced-off overlay;
      * a p1.chord_bound that carries a non-bool `bound`, or that reports
        bound=False on "wii" with ok:true -- on a console the chord IS the
        profile, so a card whose live binding table forms no F3 chord must be
        refused by that gate rather than carried past it (contract 104, C-1:
        the gate moved off p0.session, which could never have read the table
        because nobody owns player 0 until P1 binds the pad);
      * a p2.chord whose `chat_opened` is true (on EITHER platform), whose
        `slot_before`/`slot_after` differ (a chord must not move the hotbar),
        whose order-0 `fired` is true (the WRONG press order must toggle
        nothing, on either platform), or whose order-1 `fired` is not true on
        "wii" (the PC config binds no chord at all, so a PC run legitimately
        records false and still asserts "no chat, no slot move");
      * a p2.chord whose `fires` count disagrees with its own `fired` bool --
        in particular a RULED order that fired more than once, which a bool
        cannot tell from one toggle and which ends the overlay where it
        started (contract 104, M-1; p4.chord_restore is graded the same way);
      * a p2.chord whose `overlay` reading disagrees with that count: the leg
        starts from a forced-OFF overlay, so "the action fired" and "the
        product toggled" must be the same answer, and a stream where they are
        not is refused HERE rather than surfacing two legs later as something
        else (M-2);
      * the two p2.chord markers not carrying press orders 0 then 1;
      * p3.opened.opened not true -- the opener must still work after the
        neutral re-arm;
      * a p3.closed whose `ruled_fired` disagrees with what p2.chord order 1
        reported, or whose `overlay` is not that same value: ONE legal toggle
        has been spent by then, so the overlay is on exactly when the ruled
        order fired (contract 102 demanded "off" here and could therefore
        never pass on hardware -- see CHAT_CHORD_SPENT);
      * a p4.chord_restore that opened chat, moved the hotbar, did not fire on
        "wii", fired a number of times other than once, or left `overlay`
        true: the restore leg is what puts the console back as it was."""

    def __init__(self, run_id: str = ""):
        self.run_id = run_id or ""
        self.error: str | None = None
        self.contract = CHAT_CHORD_CONTRACT_CURRENT
        self.markers = 0
        self.count: dict[tuple[str, str], int] = {}
        self.last_seq = 0
        self.last_ordinal = -1
        self.last_key: tuple[str, str] | None = None
        self.finished = False
        self.profile_done: str | None = None
        self.profile_done_count = 0
        self.done_before_ok = False
        self.last_ms = 0
        self.platform: str | None = None
        self.chord_bound: bool | None = None
        self.overlay_at_start: bool | None = None
        self.orders: list = []
        self.fired: list = []
        self.slots: list = []
        self.opened: bool | None = None
        # The overlay right after the opener leg (ON when the single legal
        # toggle landed) and after the RESTORE leg put it back.
        self.overlay_after_chord: bool | None = None
        self.overlay_at_end: bool | None = None
        self.restored: bool | None = None

    def heartbeat_floor(self, default: int) -> int:
        """This profile ends when its last assertion passes, so the
        telemetry-health question is CADENCE over the driver's own clock
        (0.8 heartbeats per second of it), never a fixed count.

        NOT ChatLoginEvidence's "never fewer than 10": the login lasts a
        minute, this driver is done in about six seconds. Real Wii,
        2026-09-19, build 0c5a7d8_09.19, the first hardware run of contract
        104: every marker green, driver clock 5821 ms, 7 one-per-second
        heartbeats -- FAILED "only 7 heartbeats; need 10" by the copied
        minimum, which no honest run of this profile can ever reach. The fixed
        part only has to show the stream is alive: 3 beats."""
        if self.last_ms <= 0:
            return default
        return max(3, int(0.8 * self.last_ms / 1000.0))

    def note_profile_done(self, status) -> None:
        self.profile_done_count += 1
        self.profile_done = status
        self.done_before_ok = not self.finished
        if self.profile_done_count > 1 and not self.error:
            self.error = ("more than one profile_done for a single run "
                          "(the driver sends exactly one, always)")

    def note(self, event: dict) -> None:
        if self.error:
            return
        if self.finished:
            self.error = ("markers continued after run.ok (seq %r) -- run.ok "
                          "is the last marker of a run" % (event.get("seq"),))
            return
        if self.run_id and event.get("run_id") != self.run_id:
            self.error = ("stale-run evidence: marker run_id=%r is not the run "
                          "being validated (%r)"
                          % (event.get("run_id"), self.run_id))
            return
        phase, marker = event.get("phase"), event.get("marker")
        seq, ok = event.get("seq"), event.get("ok")
        if (not isinstance(phase, str) or not isinstance(marker, str)
                or type(seq) is not int or type(ok) is not bool
                or type(event.get("ms")) is not int):
            self.error = ("malformed chat_chord marker: %s"
                          % json.dumps(event, sort_keys=True)[:120])
            return
        self.last_ms = max(self.last_ms, event["ms"])
        if seq != self.last_seq + 1:
            self.error = ("seq gap before %s.%s: seq=%d follows seq=%d, but "
                          "the contract promises a gapless 1..N run"
                          % (phase, marker, seq, self.last_seq))
            return
        self.last_seq = seq
        self.markers += 1
        if phase == "run" and marker == "timeout":
            self.error = ("run.timeout: %s"
                          % (event.get("diag") or "total timeout"))
            return
        if marker == "fail" or ok is not True:
            self.error = ("phase failure %s.%s: %s"
                          % (phase, marker, event.get("diag")))
            return
        key = (phase, marker)
        ordinal = CHAT_CHORD_ORDINAL.get(key)
        if ordinal is None:
            self.error = ("unexpected marker %s.%s: not a marker of the "
                          "mp_chat_chord_1m contract" % (phase, marker))
            return
        if ordinal < self.last_ordinal:
            self.error = ("reordered markers: %s.%s arrived after %s.%s"
                          % (phase, marker, self.last_key[0],
                             self.last_key[1]))
            return
        seen = self.count.get(key, 0) + 1
        if seen > CHAT_CHORD_EXPECTED[key]:
            self.error = ("duplicate marker %s.%s: seen %d times, the contract "
                          "says %d"
                          % (phase, marker, seen, CHAT_CHORD_EXPECTED[key]))
            return
        if not self._note_values(key, event):
            return
        self.count[key] = seen
        self.last_ordinal = ordinal
        self.last_key = key
        if key == ("run", "ok"):
            self.finished = True

    def _note_values(self, key, event) -> bool:
        if key == ("p0", "session"):
            contract = event.get("contract")
            if contract not in CHAT_CHORD_CONTRACTS:
                spent = CHAT_CHORD_SPENT.get(contract)
                self.error = ("malformed p0.session: contract=%r is not one of "
                              "%s%s"
                              % (contract, list(CHAT_CHORD_CONTRACTS),
                                 (" -- that contract is SPENT: %s" % spent)
                                 if spent else ""))
                return False
            self.contract = contract
            platform = event.get("platform")
            if platform not in CHAT_LOGIN_PLATFORMS:
                self.error = ("malformed p0.session: platform=%r is not one of "
                              "%s" % (platform, "/".join(CHAT_LOGIN_PLATFORMS)))
                return False
            self.platform = platform
            # The whole point of the leg: it starts from a KNOWN state, so a
            # run that began with the overlay already on proves nothing about
            # what the chord did to it.
            overlay = event.get("overlay")
            if overlay is not False:
                self.error = ("contradictory evidence: p0.session overlay=%r "
                              "-- the run must start from a forced-OFF debug "
                              "overlay" % (overlay,))
                return False
            self.overlay_at_start = overlay
        elif key == ("p1", "chord_bound"):
            # Contract 104: the E-2 gate's own marker. A REFUSAL arrives as
            # ok:false and is latched by note() before this runs, so what
            # reaches here is always the passing form -- and on a console the
            # only passing form is `bound: true`, because the chord is the
            # whole profile there. A PC run legitimately records false (its
            # config binds a single key, not a chord) and goes on to run the
            # legs that do not need one.
            bound = event.get("bound")
            if type(bound) is not bool:
                self.error = ("malformed p1.chord_bound: bound=%r is not a "
                              "bool" % (bound,))
                return False
            if self.platform == "wii" and bound is not True:
                self.error = ("contradictory evidence: p1.chord_bound "
                              "bound=False on wii with ok:true -- a console "
                              "run whose config binds no F3 chord must be "
                              "REFUSED by this gate, not carried past it")
                return False
            self.chord_bound = bound
        elif key == ("p1", "pad_bound"):
            if event.get("bound") is not True:
                self.error = ("contradictory evidence: p1.pad_bound bound=%r "
                              "-- player 0 never took the virtual pad"
                              % (event.get("bound"),))
                return False
            want_kind, want_channel = CHAT_LOGIN_BINDING[self.platform]
            got = (event.get("kind"), event.get("channel"))
            if got != (want_kind, want_channel):
                self.error = ("contradictory evidence: p1.pad_bound observed "
                              "(kind,channel)=%r on platform %r, which binds %r"
                              % (got, self.platform,
                                 (want_kind, want_channel)))
                return False
        elif key == ("p2", "chord"):
            order, fired = event.get("order"), event.get("fired")
            fires, overlay = event.get("fires"), event.get("overlay")
            opened = event.get("chat_opened")
            before, after = event.get("slot_before"), event.get("slot_after")
            if order not in CHAT_CHORD_ORDERS or order in self.orders:
                self.error = ("malformed p2.chord: order=%r -- the contract "
                              "presses BOTH orders %s, each once"
                              % (order, list(CHAT_CHORD_ORDERS)))
                return False
            if opened is not False:
                self.error = ("contradictory evidence: p2.chord order=%r "
                              "chat_opened=%r -- a chord must never open the "
                              "composer, on any platform" % (order, opened))
                return False
            if type(fired) is not bool:
                self.error = ("malformed p2.chord: fired=%r is not a bool"
                              % (fired,))
                return False
            if type(before) is not int or type(after) is not int:
                self.error = ("malformed p2.chord: slot_before=%r "
                              "slot_after=%r -- both must be ints (-1 where "
                              "the hotbar was not observable)"
                              % (before, after))
                return False
            # Minus and Plus carry the hotbar taps as well as the chord, so
            # "the chord did not move a slot" is half of what this leg buys.
            if before != after:
                self.error = ("contradictory evidence: p2.chord order=%r "
                              "slot_before=%d slot_after=%d -- a chord must "
                              "never move the hotbar" % (order, before, after))
                return False
            if self.platform == "wii" and before < 0:
                self.error = ("contradictory evidence: p2.chord order=%r "
                              "slot_before=-1 on wii -- the hotbar was never "
                              "observable, so \"the chord did not move it\" "
                              "is unproven" % (order,))
                return False
            # THE ORDER RULE (Astra ruling 3 D2). Order 1 is "hold Minus, then
            # press Plus" and must toggle the overlay -- but the chord is a
            # CONFIGURED binding (9008/212213/409410 on the Wii, a single key
            # on PC), so only a console run can assert the fire. Order 0 is the
            # wrong order and must toggle NOTHING anywhere, which is the
            # assertion that makes this leg worth running at all.
            if order == 0 and fired is not False:
                self.error = ("contradictory evidence: p2.chord order=0 "
                              "fired=True -- the wrong press order (Plus "
                              "first) must never toggle the overlay")
                return False
            if order == 1 and self.platform == "wii" and fired is not True:
                self.error = ("contradictory evidence: p2.chord order=1 "
                              "fired=%r on wii -- the ruled order (hold "
                              "Minus, then press Plus) must reach "
                              "IB_DEBUG_OVERLAY" % (fired,))
                return False
            # Contract 104, M-1: the COUNT, graded exactly as
            # p4.chord_restore's already is. "Fired once" and "fired three
            # times" are the same bool and a different console -- three
            # toggles leave the overlay where one found it.
            if type(fires) is not int:
                self.error = ("malformed p2.chord: fires=%r is not an int"
                              % (fires,))
                return False
            if fires != (1 if fired else 0):
                self.error = ("contradictory evidence: p2.chord order=%r "
                              "fires=%d %s"
                              % (order, fires,
                                 "-- the ruled chord must fire EXACTLY once"
                                 if fires > 1
                                 else "contradicts fired=%r" % (fired,)))
                return False
            # M-2: the marker has always carried what the PRODUCT did
            # (gstate.debug_overlay at the end of the leg) and this grader
            # never read it, so a stream where the action fired and nothing
            # toggled -- or where the overlay moved without a fire -- was
            # accepted here and only caught later, on another leg, as a
            # contradiction that reads like a different defect. The leg starts
            # from a forced-OFF overlay (p0.session), so the reading IS the
            # count: ON exactly when this leg fired once.
            if type(overlay) is not bool:
                self.error = ("malformed p2.chord: overlay=%r is not a bool"
                              % (overlay,))
                return False
            if overlay is not (fires == 1):
                self.error = ("contradictory evidence: p2.chord order=%r "
                              "overlay=%r but the leg fired %d time(s) from a "
                              "forced-OFF overlay -- the action and the "
                              "product disagree" % (order, overlay, fires))
                return False
            self.orders.append(order)
            self.fired.append(fired)
            self.slots.append([before, after])
        elif key == ("p3", "opened"):
            if event.get("opened") is not True:
                self.error = ("contradictory evidence: p3.opened opened=%r -- "
                              "the opener must still work after the chord "
                              "neutral re-arm" % (event.get("opened"),))
                return False
            self.opened = True
        elif key == ("p3", "closed"):
            # Contract 103. ONE legal toggle has been spent by now, so the
            # honest reading is "the overlay is on exactly when the ruled
            # order fired" -- and the driver must say which of the two it
            # believes, so the marker carries `ruled_fired` and it is
            # cross-checked against what p2.chord order 1 actually reported.
            overlay, ruled = event.get("overlay"), event.get("ruled_fired")
            # By ORDER, never by position: a stream that pressed the orders
            # the wrong way round is caught by complete_error()'s own rule,
            # and must not be mis-reported as a contradiction here.
            device = (self.fired[self.orders.index(1)]
                      if 1 in self.orders else None)
            if type(ruled) is not bool:
                self.error = ("malformed p3.closed: ruled_fired=%r is not a "
                              "bool" % (ruled,))
                return False
            # `device is None` means order 1 never arrived at all, which
            # complete_error() reports as the missing marker it is.
            if device is not None and ruled is not device:
                self.error = ("contradictory evidence: p3.closed "
                              "ruled_fired=%r contradicts p2.chord order=1 "
                              "fired=%r" % (ruled, device))
                return False
            if overlay is not ruled:
                self.error = ("contradictory evidence: p3.closed overlay=%r "
                              "but the ruled chord %s -- one legal toggle "
                              "leaves the overlay ON, and p4.chord_restore is "
                              "what puts it back"
                              % (overlay,
                                 "fired" if ruled else "did not fire"))
                return False
            self.overlay_after_chord = overlay
        elif key == ("p4", "chord_restore"):
            fired, fires = event.get("fired"), event.get("fires")
            opened, overlay = event.get("chat_opened"), event.get("overlay")
            before, after = event.get("slot_before"), event.get("slot_after")
            if type(fired) is not bool or type(fires) is not int:
                self.error = ("malformed p4.chord_restore: fired=%r fires=%r"
                              % (fired, fires))
                return False
            if opened is not False:
                self.error = ("contradictory evidence: p4.chord_restore "
                              "chat_opened=%r -- a chord must never open the "
                              "composer, on any platform" % (opened,))
                return False
            if type(before) is not int or type(after) is not int:
                self.error = ("malformed p4.chord_restore: slot_before=%r "
                              "slot_after=%r -- both must be ints (-1 where "
                              "the hotbar was not observable)"
                              % (before, after))
                return False
            if before != after:
                self.error = ("contradictory evidence: p4.chord_restore "
                              "slot_before=%d slot_after=%d -- a chord must "
                              "never move the hotbar" % (before, after))
                return False
            if self.platform == "wii" and before < 0:
                self.error = ("contradictory evidence: p4.chord_restore "
                              "slot_before=-1 on wii -- the hotbar was never "
                              "observable, so \"the chord did not move it\" "
                              "is unproven")
                return False
            # "Fired exactly once" is the whole point: three toggles end ON
            # again, so a bool alone could not carry the claim.
            if fires != (1 if fired else 0):
                self.error = ("contradictory evidence: p4.chord_restore "
                              "fires=%d %s"
                              % (fires,
                                 "-- the restoring chord must fire EXACTLY "
                                 "once" if fires > 1
                                 else "contradicts fired=%r" % (fired,)))
                return False
            if self.platform == "wii" and fired is not True:
                self.error = ("contradictory evidence: p4.chord_restore "
                              "fired=%r on wii -- a second correct-order "
                              "chord must reach IB_DEBUG_OVERLAY and put the "
                              "overlay back" % (fired,))
                return False
            if overlay is not False:
                self.error = ("contradictory evidence: p4.chord_restore "
                              "overlay=%r -- the run must leave the debug "
                              "overlay OFF" % (overlay,))
                return False
            self.restored = fired
            self.overlay_at_end = overlay
        return True

    def complete_error(self) -> str | None:
        if not self.run_id:
            return ("run id missing: this profile evidence is bound to the "
                    "run id, so an unidentified run cannot pass")
        if self.error:
            return self.error
        if not self.markers:
            return ("no chat_chord markers were received -- the scripted "
                    "chord driver never ran")
        for (phase, marker), expected in CHAT_CHORD_EXPECTED.items():
            seen = self.count.get((phase, marker), 0)
            if seen < expected:
                return ("missing marker %s.%s: seen %d of %d"
                        % (phase, marker, seen, expected))
        if not self.finished:
            return "run.ok never arrived -- the driver never completed the run"
        if self.orders != list(CHAT_CHORD_ORDERS):
            return ("the two p2.chord markers carry press orders %r, not %r"
                    % (self.orders, list(CHAT_CHORD_ORDERS)))
        if self.profile_done is None:
            return "profile_done never arrived"
        if self.profile_done != "ok":
            return ("profile_done reported %r, not ok" % (self.profile_done,))
        if self.done_before_ok:
            return ("profile_done arrived before run.ok -- the driver "
                    "completes the run only after its last assertion")
        return None

    def pass_label(self) -> str:
        if self.platform != "wii":
            return CHAT_CHORD_PASS_LABEL.replace("scripted real-Wii",
                                                 "scripted PC (NOT hardware)")
        return CHAT_CHORD_PASS_LABEL

    def summary(self) -> dict:
        device = self.complete_error()
        return {"error": self.error, "markers": self.markers,
                # The contract is the PAIR (profile, number) -- finding 4.
                "profile": CHAT_CHORD_PROFILE, "contract_event": CHAT_CHORD_EVENT,
                "contract": self.contract, "platform": self.platform,
                "chord_bound": self.chord_bound,
                "overlay_at_start": self.overlay_at_start,
                "overlay_after_chord": self.overlay_after_chord,
                "overlay_at_end": self.overlay_at_end,
                "restored": self.restored,
                "orders": self.orders, "fired": self.fired,
                "slots": self.slots, "opener_reopened": self.opened,
                "run_ok": self.finished, "profile_done": self.profile_done,
                "device_complete": device is None, "device_error": device}


class ChatLoginEvidence:
    """mp_chat_login_3m's verdict: section 5 of the B1c contract, checked as a
    marker SEQUENCE plus the server's own receipt (section 6).

    Modelled on RecoveryEvidence -- every rejection latches the FIRST reason and
    stops, so a stream that goes wrong early reports why rather than the pile of
    consequences. Section 5's rejections, each fatal on its own:

      * no `chat_login` markers at all, or `run.ok` absent;
      * a missing marker: `p4.key` exactly 53 times (37 directional presses +
        the GUI_PAGE press + 15 clicks, section 9 ruling 1), `p2.trace` and
        `p3.trace` at least once each, every other marker exactly once;
      * `seq` not a gapless 1..N (section 3 promises a refused emit retries the
        SAME seq, so a gap is lost evidence);
      * phases out of order (run, p0..p6, run);
      * `ok:false` anywhere -- which can never sit beside a `profile_done "ok"`,
        since the phase's own diag is the only device-side diagnosis there is;
      * `p4.draft.text != "/login hunter22"` or `len != 15`;
      * `p5.lines_sent.after != before + 1`;
      * a cadence violation derived from the `p2.trace` tuples (see
        `_close_leg`), under section 3's one-frame tolerance;
      * a `p6.ack` whose `ring_index` was already occupied at send time (a
        STALE ack) or whose `corr` is not the send's `seq`;
      * a marker from another run_id.

    DEVICE completion (everything above) is reported separately from the OVERALL
    verdict: a run PASSES only when the marker contract AND `receipt_error()`
    both pass, so a console that types perfectly into a server that never saw
    the command is an honest device-PASS / overall-FAIL.

    Section 9's round-2 rulings fix what section 3 left in prose:
      * `p5.sent_requested` carries the send-time chat-ring snapshot as
        `ring_head` + `ring_count` (ruling 3), which is what makes "the ack line
        was NOT in the snapshot" checkable;
      * a trace tuple's `rem` is the value the step decision saw that frame --
        post-decrement, pre-reload -- so a stepping frame carries 0 and "never a
        step while `rem > 0`" is production's own condition (ruling 4);
      * the req/cp/cd masks are decoded (ruling 5: bit0 LEFT .. bit7 PREVIOUS,
        `^`/`v` request edges), so the release-attribution rule is checkable as
        written: a step is only production-correct while `cd` still holds the
        direction, and a consumed directional press must step in its own frame;
      * every trace batch names its `leg` (section 12), because the device
        restarts `t` at each leg's first press: rows are segmented by that tag,
        `t` is monotonic within a leg only, and each leg is judged by its own
        result marker;
      * the username binding is sha256-derived and echoed by the device
        (ruling 7), so the receipt, the marker stream and the rig's own
        derivation must all name the same player.

    Section 10's round-3 rulings make the two platform-specific claims explicit:
    p0.session names the platform, the chord refusal is required on "wii" and
    accepted either way on "pc" (the gate is PLATFORM_WII only), p1.pad_bound
    carries channel 3 on "wii" and 0 on "pc" (the {KEYBOARD,0} seam),
    p3.frozen's rearm_mask is REPORTED and never scored (it is not observable
    from outside osk_chat_input_tick), and a run whose controller-recovery
    modal opened fails through the ordinary ok:false path with the device's own
    "recovery modal" diag. A "pc" run may not wear the real-Wii PASS wording
    (ruling 7)."""

    # Section 3: "corr (uint32: the seq of the request this observation
    # answers, else 0)", and P5's send request correlates to ITSELF.
    SELF_CORR = (("p5", "sent_requested"),)
    ANSWERS = {("p6", "ack"): ("p5", "sent_requested")}

    def __init__(self, run_id: str = ""):
        self.run_id = run_id or ""
        self.error: str | None = None
        # The marker contract this run is graded by. A run that does not NAME
        # one in p0.session is an old run directory, and is graded by the
        # contract it was recorded under (1).
        self.contract = CHAT_LOGIN_CONTRACT_DEFAULT
        self.expected, self.ordinal = CHAT_LOGIN_CONTRACTS[self.contract]
        self.markers = 0
        self.count: dict[tuple[str, str], int] = {}
        self.seq_of: dict[tuple[str, str], int] = {}
        self.last_seq = 0
        self.last_ordinal = -1
        self.last_phase_rank = -1
        self.last_key: tuple[str, str] | None = None
        self.finished = False
        self.profile_done: str | None = None
        self.profile_done_count = 0
        self.done_before_ok = False
        self.done_at_marker = 0
        self.last_ms = 0
        # The per-frame trace rows, SEGMENTED BY LEG (section 12): each batch
        # names its leg, rows accumulate per leg, and the leg's result marker
        # closes it. `open_leg` keeps two legs from interleaving.
        self.leg_rows: dict[str, list[dict]] = {}
        self.open_leg: str | None = None
        self.closed_legs: list[str] = []
        self.legs: list[dict] = []
        self.trace_batches = 0
        self.trace_frames = 0
        # Section 11 H1: the run-wide worst frame is REPORTED (from run.ok's
        # optional `max_frame_ms_run`) and is never a tolerance -- each leg
        # carries its own, in self.legs.
        self.max_frame_ms_run: int | None = None
        # Section 3's release attribution, applied verbatim: REPORTED, never
        # scored as a game failure -- the gap between the requested release and
        # the consumed release is driver/input delivery delay.
        self.release_lag_ms = None
        # P0: the run nonce the device echoes (section 9 ruling 7) and the
        # platform it ran on (section 10 ruling 2).
        self.session_username: str | None = None
        self.platform: str | None = None
        # Reported, never scored (section 10 ruling 3).
        self.frozen_rearm_mask = None
        # Contract 2: the OBSERVED hold the refusal leg measured.
        self.hold_ms = None
        # P4/P5/P6 values the verdict and the artifact quote.
        self.key_index = -1
        self.key_draft_len = 0
        self.draft_text: str | None = None
        self.draft_len: int | None = None
        self.lines_before = None
        self.lines_after = None
        self.ring_head = None
        self.ring_count = None
        self.ack_ring_index = None
        self.ack_wait_ms = None
        self.receipt: dict | None = None

    def heartbeat_floor(self, default: int) -> int:
        """This profile ends when its LAST assertion passes, not on a clock:
        the driver sends profile_done the moment P6 verifies, so a fixed
        heartbeat floor would FAIL an honest fast run (the split_recovery_6m
        finding, 2026-09-17). The telemetry-health question is CADENCE while
        the run lasted: at least 0.8 heartbeats per second of the driver's own
        clock, never fewer than 10. With no marker at all the table default
        applies."""
        if self.last_ms <= 0:
            return default
        return max(10, int(0.8 * self.last_ms / 1000.0))

    def note_profile_done(self, status) -> None:
        self.profile_done_count += 1
        self.profile_done = status
        # Section 3 completes with run.ok FIRST and profile_done("ok") only
        # then, so WHEN the completion arrived is evidence in itself (judged in
        # complete_error, after the structural checks, so a stream that is also
        # missing markers reports the missing markers rather than this).
        self.done_before_ok = not self.finished
        self.done_at_marker = self.markers
        if self.profile_done_count > 1 and not self.error:
            self.error = ("more than one profile_done for a single run "
                          "(section 3 sends exactly one, always)")

    def note(self, event: dict) -> None:
        if self.error:
            return
        if self.finished:
            self.error = ("markers continued after run.ok (seq %r) -- run.ok is "
                          "the last marker of a run" % (event.get("seq"),))
            return
        if self.run_id and event.get("run_id") != self.run_id:
            self.error = ("stale-run evidence: marker run_id=%r is not the run "
                          "being validated (%r)"
                          % (event.get("run_id"), self.run_id))
            return
        phase, marker = event.get("phase"), event.get("marker")
        seq, corr, ok = event.get("seq"), event.get("corr"), event.get("ok")
        if (not isinstance(phase, str) or not isinstance(marker, str)
                or type(seq) is not int or type(corr) is not int
                or type(ok) is not bool or type(event.get("ms")) is not int):
            self.error = ("malformed chat_login marker: %s"
                          % json.dumps(event, sort_keys=True)[:120])
            return
        self.last_ms = max(self.last_ms, event["ms"])
        if seq <= self.last_seq:
            self.error = ("seq not strictly increasing: %s.%s carries seq=%d "
                          "after seq=%d" % (phase, marker, seq, self.last_seq))
            return
        if seq != self.last_seq + 1:
            self.error = ("seq gap before %s.%s: seq=%d follows seq=%d, but "
                          "section 3 promises a gapless 1..N run"
                          % (phase, marker, seq, self.last_seq))
            return
        self.last_seq = seq
        self.markers += 1
        # Failure markers first: they carry the only device-side diagnosis.
        if phase == "run" and marker == "timeout":
            self.error = ("run.timeout: %s"
                          % (event.get("diag") or "total timeout"))
            return
        if marker == "fail" or ok is not True:
            self.error = ("phase failure %s.%s: %s"
                          % (phase, marker, event.get("diag")))
            return
        key = (phase, marker)
        ordinal = self.ordinal.get(key)
        if ordinal is None:
            self.error = ("unexpected marker %s.%s: not a marker of "
                          "contract %d (section 3)"
                          % (phase, marker, self.contract))
            return
        rank = self._phase_rank(phase)
        if rank < self.last_phase_rank:
            self.error = ("phases out of order: %s.%s arrived after phase %s "
                          "(the contract order is run,p0..p6,run)"
                          % (phase, marker, self.last_key[0]))
            return
        # Trace batches are the markers that interleave with their own phase's
        # result markers (one batch per <=16 rendered frames), so only their
        # PHASE order is checked, never their ordinal -- which is also what
        # confines p3.trace to P3 (section 9 ruling 6).
        if key not in CHAT_LOGIN_TRACE_MARKERS and ordinal < self.last_ordinal:
            self.error = ("reordered markers: %s.%s arrived after %s.%s"
                          % (phase, marker, self.last_key[0], self.last_key[1]))
            return
        expected = self.expected[key]
        seen = self.count.get(key, 0) + 1
        if expected is not None and seen > expected:
            self.error = ("duplicate marker %s.%s: seen %d times, the contract "
                          "says %d" % (phase, marker, seen, expected))
            return
        if not self._note_correlation(key, seq, corr):
            return
        if not self._note_values(key, event):
            return
        self.count[key] = seen
        self.last_phase_rank = rank
        if key not in CHAT_LOGIN_TRACE_MARKERS:
            self.last_ordinal = ordinal
        self.last_key = key
        self.seq_of.setdefault(key, seq)
        if key == ("run", "ok"):
            self.finished = True

    def _phase_rank(self, phase: str) -> int:
        # Section 9 ruling 6: no run.start marker, so the run phase is always
        # the LAST one -- a run.* marker before p0.session is out of order.
        if phase == "run":
            return len(CHAT_LOGIN_PHASES)
        return CHAT_LOGIN_PHASES.index(phase)

    def _note_correlation(self, key, seq, corr) -> bool:
        phase, marker = key
        if key in self.SELF_CORR:
            if corr != seq:
                self.error = ("contradictory evidence: %s.%s corr=%d must be "
                              "its own seq %d" % (phase, marker, corr, seq))
                return False
        elif corr and not 0 < corr < seq:
            self.error = ("contradictory evidence: %s.%s corr=%d answers no "
                          "earlier marker (its own seq is %d)"
                          % (phase, marker, corr, seq))
            return False
        answers = self.ANSWERS.get(key)
        if answers and corr != self.seq_of.get(answers):
            self.error = ("contradictory evidence: %s.%s corr=%d does not "
                          "answer %s.%s (seq %r)"
                          % (phase, marker, corr, answers[0], answers[1],
                             self.seq_of.get(answers)))
            return False
        return True

    def _leg_frame_ms(self, key, event) -> int | None:
        """Section 11 H1: the cadence tolerance base is the leg's OWN worst
        frame, reported on that leg's result marker and used for that leg only
        -- never a run-wide maximum, which would let one stall anywhere in the
        run widen every other leg's window. A leg whose worst frame exceeds
        250 ms is INCONCLUSIVE about a 250/80 ms cadence and fails the run."""
        phase, marker = key
        value = event.get("max_frame_ms")
        if type(value) is not int or value <= 0:
            self.error = ("malformed %s.%s: max_frame_ms=%r -- this leg's "
                          "cadence tolerance has no base (section 11 H1)"
                          % (phase, marker, value))
            return None
        if value > CHAT_LOGIN_FRAME_STALL_MS:
            self.error = ("frame stall in trace leg %s: max_frame_ms=%d ms is "
                          "over the %d ms bound, so the leg is INCONCLUSIVE "
                          "about a %d/%d ms cadence -- not tolerated, failed"
                          % (CHAT_LOGIN_LEG_CLOSERS[key], value,
                             CHAT_LOGIN_FRAME_STALL_MS,
                             CHAT_LOGIN_REPEAT_DELAY_MS,
                             CHAT_LOGIN_REPEAT_RATE_MS))
            return None
        return value

    @staticmethod
    def _cadence_window(want: int, frame_ms: int) -> tuple[int, int]:
        """Section 3's "+-max_frame_ms" plus section 5's extra 20 fps frame --
        applied ASYMMETRICALLY, and that asymmetry is deliberate: the repeat is
        a countdown that only steps once `remaining <= 0`, so sampling can make
        a step LATE (frame quantization, a slow frame, a late observation) but
        never EARLY. The low edge therefore gets the device's own frame
        quantization only; the collector's extra slow-frame allowance is a LATE
        allowance. A symmetric window would accept a 40 ms repeat of an 80 ms
        cadence -- i.e. a production timer running at double rate -- as
        "within tolerance", which is the exact defect this check exists for.

        `frame_ms` is THIS LEG's worst frame (section 11 H1)."""
        return (want - frame_ms, want + frame_ms + CHAT_LOGIN_SLOW_FRAME_MS)

    def _note_trace(self, event) -> bool:
        """One trace batch (p2 or p3, same encoding -- section 9 ruling 6):
        `frames` is <=16 "t:req:cp:cd:rep:rem:r:c" tuples, one per RENDERED
        frame (section 3), `n` their count and `first_t` the first t. The masks
        are decoded per ruling 5 (bit0 LEFT .. bit7 PREVIOUS; a trailing `^`/`v`
        on req marks a request press/release edge). Structural rules are checked
        per record; the step CADENCE is checked when the leg closes, because its
        tolerance needs the device's max_frame_ms from p2.hold_result."""
        phase = event.get("phase")
        allowed = CHAT_LOGIN_PHASE_LEGS.get(phase, ())
        leg = event.get("leg")
        # Section 12, fail closed: an untagged or unknown leg is exactly the
        # ambiguity the tag exists to remove, so it is never guessed.
        if not isinstance(leg, str) or leg not in allowed:
            self.error = ("malformed %s.trace batch: leg=%r is not one of %s "
                          "(section 12) -- an untagged batch cannot be told "
                          "from a clock running backwards"
                          % (phase, leg, "/".join(allowed) or "(none)"))
            return False
        if leg in self.closed_legs:
            self.error = ("trace batch for leg %r arrived after its own result "
                          "marker already closed it (section 12)" % (leg,))
            return False
        if self.open_leg is not None and self.open_leg != leg:
            self.error = ("trace legs interleaved: a %r batch arrived while leg "
                          "%r was still open -- the legs run one at a time, "
                          "each closed by its own result marker (section 12)"
                          % (leg, self.open_leg))
            return False
        self.open_leg = leg
        frames, n, first_t = (event.get("frames"), event.get("n"),
                              event.get("first_t"))
        if (not isinstance(frames, str) or type(n) is not int
                or type(first_t) is not int):
            self.error = ("malformed p2.trace batch: frames/n/first_t missing "
                          "or not of section 3's types")
            return False
        tuples = [item for item in frames.split(";") if item]
        if not tuples or len(tuples) != n or n > 16:
            self.error = ("p2.trace batch carries %d tuples for n=%d (section 3 "
                          "allows 1..16 per marker)" % (len(tuples), n))
            return False
        records = []
        for raw in tuples:
            parts = raw.split(":")
            if len(parts) != 8:
                self.error = ("malformed trace tuple %r -- section 3 encodes "
                              "t:req:cp:cd:rep:rem:r:c" % raw[:48])
                return False
            req, edge = parts[1], ""
            if req[-1:] in tuple(CHAT_LOGIN_REQ_EDGES):
                req, edge = req[:-1], req[-1]
            try:
                # Section 13 ruling 1: the three MASKS are hex, as the device
                # emits them (docs section 3); the clock, the repeat decision,
                # the countdown and the cell are plain decimal.
                t, rep, rem, row, col = (int(parts[i] or 0) for i in
                                         (0, 4, 5, 6, 7))
                req_mask, cp, cd = (int(part or "0", 16) for part in
                                    (req, parts[2], parts[3]))
            except ValueError:
                self.error = ("malformed trace tuple %r -- t/rep/rem/r/c are "
                              "decimal and req/cp/cd are HEX masks, req "
                              "optionally followed by ^ or v (sections 9.5 "
                              "and 13)" % raw[:48])
                return False
            records.append({"t": t, "req": req_mask, "edge": edge, "cp": cp,
                            "cd": cd, "rep": rep, "rem": rem, "row": row,
                            "col": col})
        if records[0]["t"] != first_t:
            self.error = ("trace batch first_t=%d does not match its first "
                          "tuple (t=%d)" % (first_t, records[0]["t"]))
            return False
        rows = self.leg_rows.setdefault(leg, [])
        for record in records:
            if not self._note_trace_frame(leg, rows, record):
                return False
        self.trace_batches += 1
        self.trace_frames += len(records)
        return True

    def _note_trace_frame(self, leg, rows, record) -> bool:
        """Section 3's per-frame rules, none of which needs a tolerance. `t` is
        monotonic WITHIN A LEG only (section 12: the device restarts it at each
        leg's first press), so a batch may never straddle a leg boundary."""
        previous = rows[-1] if rows else None
        t, rep = record["t"], record["rep"]
        if rep not in (0, 1, 2):
            self.error = ("p2.trace frame t=%d carries rep=%d (section 3: 0 "
                          "none, 1 fresh-edge step, 2 repeat step)" % (t, rep))
            return False
        if previous is not None:
            if t < previous["t"]:
                self.error = ("trace frame t=%d went backwards inside leg %r "
                              "(previous t=%d) -- a batch may not straddle a "
                              "leg boundary (section 12)"
                              % (t, leg, previous["t"]))
                return False
            if t == previous["t"] and rep and previous["rep"]:
                self.error = ("cadence violation: two steps in one frame "
                              "(t=%d) -- section 3 forbids a catch-up burst"
                              % t)
                return False
        if rep and record["rem"] > 0:
            self.error = ("cadence violation: a step at t=%d while the repeat "
                          "timer still had rem=%d ms -- section 3 forbids a "
                          "step while rem > 0" % (t, record["rem"]))
            return False
        if record["edge"] and record["edge"] not in CHAT_LOGIN_REQ_EDGES:
            self.error = ("malformed trace tuple at t=%d: req edge %r is not "
                          "^ or v" % (t, record["edge"]))
            return False
        # Section 3's release attribution, decoded (section 9 ruling 5): a step
        # in a frame whose `cd` still holds the direction is production,
        # correct; a step in a frame where `cd` has already cleared implicates
        # PRODUCTION handling. The requested-to-consumed release gap is the
        # driver's delivery delay, reported as release_lag_ms and never scored.
        if rep and not record["cd"] & CHAT_LOGIN_DIRECTIONS:
            self.error = ("cadence violation: a step at t=%d while no direction "
                          "was held (cd=%s) -- production kept moving after it "
                          "consumed the release"
                          % (t, self.describe_mask(record["cd"])))
            return False
        if rep == 1 and not record["cp"] & CHAT_LOGIN_DIRECTIONS:
            self.error = ("cadence violation: a fresh-edge step at t=%d with no "
                          "consumed directional press that frame (cp=%s)"
                          % (t, self.describe_mask(record["cp"])))
            return False
        if record["cp"] & CHAT_LOGIN_DIRECTIONS and rep != 1:
            # Section 3's "an immediate step on the edge frame": the frame in
            # which production CONSUMED a directional press must step, as a
            # fresh edge. A consumed NON-directional press (leg c's GUI_PAGE
            # tap, cp=0x40) moves no cell and is not covered.
            #
            # This rule was briefly scoped to the leg's opening press because
            # the first PC replay read the page tap as cp=DOWN|BACK -- that was
            # the RADIX bug (section 13 ruling 1: the masks are hex, so 0x40 is
            # PAGE alone, not 40 decimal). With hex parsing the real run does
            # not trip it, so the full rule stands.
            self.error = ("cadence violation: production consumed %s at t=%d "
                          "with rep=%d -- section 3 requires an immediate "
                          "fresh-edge step on the edge frame"
                          % (self.describe_mask(record["cp"]), t, rep))
            return False
        rows.append(record)
        return True

    @staticmethod
    def describe_mask(mask: int) -> str:
        """A req/cp/cd mask in the contract's own names (section 9 ruling 5)."""
        names = [name for i, name in enumerate(CHAT_LOGIN_BITS)
                 if mask & (1 << i)]
        return "|".join(names) if names else "0"

    def _close_leg(self, name, event, frame_ms) -> bool:
        """A result marker closes its leg: cross-check what the marker CLAIMS
        against what the leg's own rows show (section 11 M1/M2/L3), then check
        the step cadence those rows recorded (section 3's "first repeat at
        250 ms, later repeats at 80 ms") against THIS leg's frame time.

        The cadence chain is per HELD PRESS: a fresh-edge step (rep 1) reloads
        the timer and therefore starts a new chain, so the P3 leg -- one press,
        a disconnect, then a re-armed press -- is not read as a monstrously late
        repeat."""
        # Section 12: the rows this marker judges are the ones tagged with its
        # own leg -- never "whatever arrived since the last closer".
        records = self.leg_rows.pop(name, [])
        self.closed_legs.append(name)
        if self.open_leg == name:
            self.open_leg = None
        steps = [record for record in records if record["rep"]]
        repeats = [record for record in steps if record["rep"] == 2]
        # Section 11 M2, in Astra's words: a step in a frame whose `cd` still
        # holds the direction is production-correct and is never counted
        # against the game; only a step AFTER the consumed release counts.
        after_release = [record for record in steps
                         if not record["cd"] & CHAT_LOGIN_DIRECTIONS]
        field = CHAT_LOGIN_LEG_STEP_FIELD[name]
        leg = {"leg": name, "frames": len(records), "steps": len(steps),
               "repeats": len(repeats), "max_frame_ms": frame_ms,
               "reported_steps": event.get(field) if field else None,
               "steps_after_consumed_release": len(after_release),
               # Reported, never scored: the requested -> consumed release gap
               # is driver/input delivery delay (section 11 M2).
               "observed_release_lag_ms": self._release_lag(records),
               "reported_release_lag_ms": event.get("release_lag_ms")}
        self.legs.append(leg)
        if not records:
            self.error = ("%s.%s closed a leg with no trace frames -- section 3 "
                          "emits one record per rendered frame"
                          % (event.get("phase"), event.get("marker")))
            return False
        # M1: the marker's own step count must be what its rows show. One
        # record per rendered frame is the contract, so a disagreement means
        # either the count or the trace is wrong -- both are disqualifying.
        if field is not None:
            counted = len(repeats) if field == "extra_steps" else len(steps)
            if event.get(field) != counted:
                self.error = ("contradictory evidence: %s.%s %s=%r, but its "
                              "trace rows show %d (section 11 M1)"
                              % (event.get("phase"), event.get("marker"), field,
                                 event.get(field), counted))
                return False
        if ("after_release_steps" in event
                and event["after_release_steps"] != len(after_release)):
            self.error = ("contradictory evidence: %s.%s after_release_steps=%r,"
                          " but its trace rows show %d step(s) after the "
                          "CONSUMED release (section 11 M2)"
                          % (event.get("phase"), event.get("marker"),
                             event["after_release_steps"], len(after_release)))
            return False
        if name == "reconnect" and not self._check_rearm(records):
            return False
        previous, chain = None, 0
        for step in steps:
            if step["rep"] == 1:
                previous, chain = step["t"], 0
                continue
            if previous is None:
                self.error = ("cadence violation in leg %s: a repeat step at "
                              "t=%d with no press edge before it" % (name,
                                                                     step["t"]))
                return False
            chain += 1
            delta = step["t"] - previous
            want = (CHAT_LOGIN_REPEAT_DELAY_MS if chain == 1
                    else CHAT_LOGIN_REPEAT_RATE_MS)
            low, high = self._cadence_window(want, frame_ms)
            if not low <= delta <= high:
                self.error = ("cadence violation in leg %s: %s repeat %d after "
                              "%d ms, outside the contract window [%d, %d] for "
                              "%d ms (this leg's max_frame_ms=%d)"
                              % (name, "first" if chain == 1 else "later",
                                 chain, delta, low, high, want, frame_ms))
                return False
            previous = step["t"]
        return True

    def _check_rearm(self, records) -> bool:
        """Section 11 L3: p3.rearmed's claim, re-derived from the rows -- the
        first step after the owner went away may only follow an OBSERVED
        neutral frame (cd == 0), and it must be a FRESH edge (the re-arm
        swallows the still-held press until it has been seen neutral,
        widget_osk.c:757-771)."""
        gone = next((i for i, record in enumerate(records)
                     if record["cd"] == 0), None)
        after = next((i for i, record in enumerate(records)
                      if record["rep"] and gone is not None and i > gone), None)
        if gone is None or after is None:
            self.error = ("contradictory evidence: p3.rearmed -- the trace rows "
                          "show no disconnect (cd == 0) followed by a re-armed "
                          "step (section 11 L3)")
            return False
        if records[after - 1]["cd"] != 0:
            self.error = ("contradictory evidence: p3.rearmed -- the first "
                          "post-reconnect step at t=%d was not preceded by a "
                          "neutral frame (cd=%s), so the re-arm never blocked "
                          "it (section 11 L3)"
                          % (records[after]["t"],
                             self.describe_mask(records[after - 1]["cd"])))
            return False
        if records[after]["rep"] != 1:
            self.error = ("contradictory evidence: p3.rearmed -- the first "
                          "post-reconnect step at t=%d is rep=%d, not a fresh "
                          "edge (section 11 L3)"
                          % (records[after]["t"], records[after]["rep"]))
            return False
        return True

    @staticmethod
    def _release_lag(records) -> int | None:
        """REPORTED, never scored (section 3's release attribution): the gap
        between the frame that REQUESTED a direction release (req's `v` edge)
        and the frame where production had CONSUMED it (cd's directions
        cleared). That gap is driver/input delivery delay."""
        asked = None
        for record in records:
            if record["edge"] == "v" and asked is None:
                asked = record["t"]
            elif asked is not None and not record["cd"] & CHAT_LOGIN_DIRECTIONS:
                return record["t"] - asked
        return None

    def _note_values(self, key, event) -> bool:
        """The readings a marker carries that can disprove its own claim. Only
        section 5's rejection set is scored; everything else this profile
        reports (positions, glyph counts, release_lag_ms) rides into the
        artifact untouched."""
        phase, marker = key
        if key == ("p0", "session"):
            # Section 9 ruling 7: the device echoes the run nonce the rig
            # computed, and the collector re-derives it independently -- a
            # username that is not THIS run's cannot bind a receipt to it.
            name, want = event.get("username"), chat_login_username(self.run_id)
            if (not isinstance(name, str) or len(name) != 8
                    or not name.startswith("cw")):
                self.error = ("malformed p0.session: username=%r is not the "
                              "contract's \"cw\" + 6 hex characters" % (name,))
                return False
            if self.run_id and name != want:
                self.error = ("stale-run evidence: p0.session username=%r is "
                              "not this run's derivation %r (sha256 of the run "
                              "id)" % (name, want))
                return False
            self.session_username = name
            # Section 10 ruling 2: the platform decides which P1 assertions
            # apply, so a run that does not name it cannot be graded.
            platform = event.get("platform")
            if platform not in CHAT_LOGIN_PLATFORMS:
                self.error = ("malformed p0.session: platform=%r is not one of "
                              "%s (section 10 ruling 2)"
                              % (platform, "/".join(CHAT_LOGIN_PLATFORMS)))
                return False
            self.platform = platform
            # The marker contract this run was RECORDED under, so an old run
            # directory and a run of this build can never be confused (Astra
            # ruling 2 D2). Absent = 1, the contract that called P1's refusal
            # leg a CHORD refusal; 2 renames it to the overlong-hold refusal
            # the opener change actually produces. The table swap below is
            # what makes the two marker sets mutually exclusive.
            contract = event.get("contract", CHAT_LOGIN_CONTRACT_DEFAULT)
            if type(contract) is not int or contract not in CHAT_LOGIN_CONTRACTS:
                self.error = ("malformed p0.session: contract=%r is not one of "
                              "%s" % (contract,
                                      sorted(CHAT_LOGIN_CONTRACTS)))
                return False
            self.contract = contract
            self.expected, self.ordinal = CHAT_LOGIN_CONTRACTS[contract]
        elif key == ("p1", "pad_bound"):
            if event.get("bound") is not True:
                self.error = ("contradictory evidence: p1.pad_bound bound=%r "
                              "-- player 0 never took the virtual pad"
                              % (event.get("bound"),))
                return False
            # Section 10 ruling 5 + section 11 L1: p1.pad_bound reports the
            # OBSERVED binding (input_player_device(0, ...)), so the collector
            # compares the whole identity against the platform's expectation --
            # WPAD channel 3 on the console, the {KEYBOARD,0} seam on PC. A
            # bound-but-wrong device would otherwise pass as "bound".
            want_kind, want_channel = CHAT_LOGIN_BINDING[self.platform]
            got = (event.get("kind"), event.get("channel"))
            if got != (want_kind, want_channel):
                self.error = ("contradictory evidence: p1.pad_bound observed "
                              "(kind,channel)=%r on platform %r, which binds "
                              "%r (section 11 L1)"
                              % (got, self.platform,
                                 (want_kind, want_channel)))
                return False
            # The NUNCHUK identity is a WII rule (section 1.4: input_owner_latch
            # force-disconnects a bare Remote, so that family is illegal there).
            # On PC the bound device is a keyboard and its category is reported,
            # not scored.
            if (self.platform == "wii"
                    and event.get("category") != CHAT_LOGIN_CATEGORY_NUNCHUK):
                self.error = ("contradictory evidence: p1.pad_bound category=%r,"
                              " section 1.4 presents the pad as INPUT_CAT_"
                              "NUNCHUK (%d); a bare Remote is illegal"
                              % (event.get("category"),
                                 CHAT_LOGIN_CATEGORY_NUNCHUK))
                return False
        elif phase == "p1" and marker in CHAT_LOGIN_REFUSAL_MARKERS:
            # P1's REFUSAL leg. The marker's name says which physical claim it
            # makes -- contract 1's chord_refused is the Up+Left D-pad chord,
            # contract 2's hold_refused is an opener held past its 500 ms tap
            # window (b1-chat-opener-0919). The contract tables above already
            # reject the wrong name for the run's contract, so the readings
            # below are common to both and the wording follows the name.
            refused = event.get("refused")
            if type(refused) is not bool:
                self.error = ("malformed p1.%s: refused=%r is not a bool"
                              % (marker, refused))
                return False
            # Section 10 ruling 2: the opener gate is PLATFORM_WII only
            # (input.c's input_chat_open_edge), so a PC run legitimately reports
            # false -- the leg is still emitted, and still says what happened.
            if self.platform == "wii" and refused is not True:
                self.error = ("contradictory evidence: p1.%s refused=%r on wii "
                              "-- the gated press must NOT open the composer"
                              % (marker, refused))
                return False
            # Section 11 M4: refused = chat_pressed && !screen_changed, so a
            # "refusal" that never actually pressed chat proves nothing. On
            # wii the press is REQUIRED; on pc the gate does not exist.
            pressed, changed = (event.get("chat_pressed"),
                                event.get("screen_changed"))
            if self.platform == "wii" and pressed is not True:
                self.error = ("contradictory evidence: p1.%s chat_pressed=%r "
                              "-- the leg must actually have pressed chat for "
                              "its refusal to mean anything (section 11 M4)"
                              % (marker, pressed))
                return False
            # Contract 2 only: the leg publishes the hold it OBSERVED (from
            # the first frame the owner frame really showed IB_CHAT down), so
            # a refusal that never actually outlasted the tap window cannot
            # pass as one.
            if self.contract >= 2:
                held = event.get("hold_ms")
                if type(held) is not int:
                    self.error = ("malformed p1.%s: hold_ms=%r is not an int "
                                  "-- contract 2 publishes the OBSERVED hold"
                                  % (marker, held))
                    return False
                self.hold_ms = held
                if self.platform == "wii" and held <= CHAT_LOGIN_TAP_MS:
                    self.error = ("contradictory evidence: p1.%s hold_ms=%d -- "
                                  "the opener was not held past the %d ms tap "
                                  "window, so a refusal proves nothing"
                                  % (marker, held, CHAT_LOGIN_TAP_MS))
                    return False
            if (type(pressed) is bool and type(changed) is bool
                    and refused != (pressed and not changed)):
                self.error = ("contradictory evidence: p1.%s refused=%r but "
                              "chat_pressed=%r screen_changed=%r -- section 11 "
                              "M4 defines refused as chat_pressed && "
                              "!screen_changed"
                              % (marker, refused, pressed, changed))
                return False
        elif key == ("p1", "chat_open"):
            if event.get("draft_len") != 0:
                self.error = ("contradictory evidence: p1.chat_open draft_len="
                              "%r -- the composer opens empty"
                              % (event.get("draft_len"),))
                return False
        elif key in CHAT_LOGIN_TRACE_MARKERS:
            return self._note_trace(event)
        elif key == ("p2", "hold_result"):
            frame_ms = self._leg_frame_ms(key, event)
            if frame_ms is None:
                return False
            self.release_lag_ms = event.get("release_lag_ms")
            steps = event.get("steps")
            if type(steps) is not int or steps < 6:
                self.error = ("contradictory evidence: p2.hold_result steps=%r,"
                              " the held leg must cross at least 6 cells"
                              % (steps,))
                return False
            low, high = self._cadence_window(CHAT_LOGIN_REPEAT_DELAY_MS,
                                             frame_ms)
            first = event.get("first_repeat_ms")
            if type(first) is not int or not low <= first <= high:
                self.error = ("cadence violation: p2.hold_result "
                              "first_repeat_ms=%r outside [%d, %d]"
                              % (first, low, high))
                return False
            # after_release_steps and release_lag_ms are judged against the
            # leg's own rows inside _close_leg (section 11 M1/M2).
            return self._close_leg(CHAT_LOGIN_LEG_CLOSERS[key], event, frame_ms)
        elif key == ("p2", "tap_result"):
            frame_ms = self._leg_frame_ms(key, event)
            if frame_ms is None:
                return False
            if event.get("steps") != 1:
                self.error = ("contradictory evidence: p2.tap_result steps=%r "
                              "-- a press+release inside the 250 ms delay moves "
                              "exactly once" % (event.get("steps"),))
                return False
            return self._close_leg(CHAT_LOGIN_LEG_CLOSERS[key], event, frame_ms)
        elif key == ("p2", "page_hold"):
            frame_ms = self._leg_frame_ms(key, event)
            if frame_ms is None:
                return False
            if event.get("extra_steps") != 0:
                self.error = ("contradictory evidence: p2.page_hold "
                              "extra_steps=%r -- a page change resets the "
                              "repeat timer" % (event.get("extra_steps"),))
                return False
            if event.get("page_before") == event.get("page_after"):
                self.error = ("contradictory evidence: p2.page_hold page_before"
                              "=%r equals page_after -- the page never changed"
                              % (event.get("page_before"),))
                return False
            low, high = self._cadence_window(CHAT_LOGIN_REPEAT_DELAY_MS,
                                             frame_ms)
            remaining = event.get("remaining_ms")
            if type(remaining) is not int or not low <= remaining <= high:
                self.error = ("cadence violation: p2.page_hold remaining_ms=%r "
                              "outside [%d, %d] -- the page change must reload "
                              "the full delay" % (remaining, low, high))
                return False
            return self._close_leg(CHAT_LOGIN_LEG_CLOSERS[key], event, frame_ms)
        elif key == ("p3", "frozen"):
            if event.get("moves_while_down") != 0:
                self.error = ("contradictory evidence: p3.frozen "
                              "moves_while_down=%r -- a disconnected owner must "
                              "not move the focus"
                              % (event.get("moves_while_down"),))
                return False
            # Section 10 ruling 3: "full" is NOT observable from outside
            # osk_chat_input_tick (it sets and clears the mask in one call), so
            # the marker carries the true readback and the collector REPORTS it
            # instead of scoring it. The frozen claim is carried by
            # moves_while_down above and by the device's own ok gate on
            # unchanged focus/draft.
            self.frozen_rearm_mask = event.get("rearm_mask")
        elif key == ("p3", "rearmed"):
            frame_ms = self._leg_frame_ms(key, event)
            if frame_ms is None:
                return False
            blocked = event.get("frames_blocked")
            if type(blocked) is not int or blocked < 1:
                self.error = ("contradictory evidence: p3.rearmed "
                              "frames_blocked=%r -- the first move may only "
                              "follow a neutral frame" % (blocked,))
                return False
            # P3's own trace leg (section 9 ruling 6) ends here: the reconnect
            # sequence is one held press across a disconnect and a re-arm, and
            # _close_leg re-derives that claim from the rows (section 11 L3).
            return self._close_leg(CHAT_LOGIN_LEG_CLOSERS[key], event, frame_ms)
        elif key == ("p4", "origin"):
            got = (event.get("row"), event.get("col"), event.get("page"))
            if got != (1, 0, CHAT_LOGIN_PAGE_LOWER):
                self.error = ("contradictory evidence: p4.origin (row,col,page)"
                              "=%r is not section 2's canonical origin "
                              "(1, 0, LOWER=%d)" % (got, CHAT_LOGIN_PAGE_LOWER))
                return False
        elif key == ("p4", "key"):
            index = event.get("i")
            if index != self.key_index + 1:
                self.error = ("reordered markers: p4.key i=%r, expected i=%d of "
                              "%d" % (index, self.key_index + 1,
                                      CHAT_LOGIN_KEY_PRESSES))
                return False
            self.key_index = index
            got = (event.get("row"), event.get("col"), event.get("page"))
            want = (event.get("want_row"), event.get("want_col"),
                    event.get("want_page"))
            if got != want:
                self.error = ("contradictory evidence: p4.key i=%d landed on "
                              "(row,col,page)=%r, wanted %r"
                              % (index, got, want))
                return False
            length = event.get("draft_len")
            if type(length) is not int or length < self.key_draft_len:
                self.error = ("contradictory evidence: p4.key i=%d draft_len=%r"
                              " shrank from %d -- this route never deletes"
                              % (index, length, self.key_draft_len))
                return False
            self.key_draft_len = length
        elif key == ("p4", "draft"):
            self.draft_text, self.draft_len = (event.get("text"),
                                               event.get("len"))
            if self.draft_text != CHAT_LOGIN_DRAFT:
                self.error = ("the composed draft is %r, not the contract's %r"
                              % (self.draft_text, CHAT_LOGIN_DRAFT))
                return False
            if self.draft_len != CHAT_LOGIN_DRAFT_LEN:
                self.error = ("the composed draft reports len=%r, not the "
                              "contract's %d bytes"
                              % (self.draft_len, CHAT_LOGIN_DRAFT_LEN))
                return False
        elif key == ("p5", "sent_requested"):
            self.lines_before = event.get("lines_before")
            self.ring_head, self.ring_count = (event.get("ring_head"),
                                               event.get("ring_count"))
            if (type(self.ring_head) is not int
                    or type(self.ring_count) is not int
                    or not 0 <= self.ring_head < CHAT_LOGIN_RING_CAPACITY
                    or not 0 <= self.ring_count <= CHAT_LOGIN_RING_CAPACITY):
                self.error = ("malformed p5.sent_requested: the send-time chat "
                              "ring snapshot (ring_head=%r, ring_count=%r) is "
                              "missing, so a stale p6.ack could not be detected"
                              % (self.ring_head, self.ring_count))
                return False
        elif key == ("p5", "composer_cleared"):
            # Section 11 L2: draft_len is REPORTED, not scored -- the composer
            # is closed by then, so its buffer length is not an observation
            # about the send. `open` and the screen ARE scored: those two are
            # what "the send closed the composer" means.
            if event.get("open") is not False:
                self.error = ("contradictory evidence: p5.composer_cleared "
                              "open=%r -- the send closes the composer"
                              % (event.get("open"),))
                return False
            if event.get("screen_ingame") is not True:
                self.error = ("contradictory evidence: p5.composer_cleared "
                              "screen_ingame=%r -- the send returns to the "
                              "world" % (event.get("screen_ingame"),))
                return False
        elif key == ("p5", "lines_sent"):
            before, after = event.get("before"), event.get("after")
            self.lines_before, self.lines_after = before, after
            if type(before) is not int or type(after) is not int:
                self.error = ("malformed p5.lines_sent: before=%r after=%r"
                              % (before, after))
                return False
            if after != before + 1:
                self.error = ("p5.lines_sent after=%d is not before+1 (%d) -- "
                              "the run must submit exactly one line"
                              % (after, before))
                return False
        elif key == ("p6", "ack"):
            index = event.get("ring_index")
            self.ack_ring_index, self.ack_wait_ms = index, event.get("wait_ms")
            if type(index) is not int or not 0 <= index < CHAT_LOGIN_RING_CAPACITY:
                self.error = ("malformed p6.ack: ring_index=%r is outside the "
                              "chat ring (0..%d)"
                              % (index, CHAT_LOGIN_RING_CAPACITY - 1))
                return False
            if type(self.ring_head) is not int:
                self.error = ("contradictory evidence: p6.ack arrived with no "
                              "send-time ring snapshot to judge it against")
                return False
            occupied = ((index - self.ring_head) % CHAT_LOGIN_RING_CAPACITY
                        < self.ring_count)
            if occupied:
                self.error = ("stale ack: p6.ack ring_index=%d was already "
                              "occupied at send time (head=%d, count=%d) -- the "
                              "reply must be a line the send did not already "
                              "see" % (index, self.ring_head, self.ring_count))
                return False
        elif key == ("run", "ok"):
            # Section 11 H1: a run-wide worst frame may ride here. It is
            # recorded for the artifact and never used as a tolerance.
            self.max_frame_ms_run = event.get("max_frame_ms_run")
        return True

    def complete_error(self) -> str | None:
        """The DEVICE side of the verdict: section 5's marker contract alone.
        The server receipt is judged separately (receipt_error)."""
        # Section 11 L6: with no run id there is nothing to bind the markers or
        # the receipt TO, and the device-echoed name can never stand in for the
        # collector's own derivation (it is the thing being checked). Fail
        # closed rather than grade an unattributable run.
        if not self.run_id:
            return ("run id missing: this profile's evidence is bound to the "
                    "run id (marker run_id, and the sha256 username the "
                    "receipt must name), so an unidentified run cannot pass")
        if self.error:
            return self.error
        if not self.markers:
            return ("no chat_login markers were received -- the scripted "
                    "chat-login driver never ran")
        for (phase, marker), expected in self.expected.items():
            seen = self.count.get((phase, marker), 0)
            if expected is None:
                if not seen:
                    return ("missing marker %s.%s: section 3 emits at least one "
                            "batch" % (phase, marker))
            elif seen < expected:
                return ("missing marker %s.%s: %d of the %d the contract "
                        "requires" % (phase, marker, seen, expected))
        if self.leg_rows:
            return ("trace frames were never closed by a result marker: %s"
                    % ", ".join("leg %r (%d frames)" % (leg, len(rows))
                                for leg, rows in sorted(self.leg_rows.items())))
        if not self.finished or self.last_key != ("run", "ok"):
            return "run.ok is not the last marker of the run"
        if self.profile_done == "ok" and self.done_before_ok:
            return ("premature success: profile_done \"ok\" arrived before "
                    "run.ok (after %d of %d markers)"
                    % (self.done_at_marker, self.markers))
        if self.profile_done != "ok":
            return ("profile_done %s: the contract (section 3) ends a passing "
                    "run with exactly \"ok\""
                    % ("was never received" if self.profile_done_count == 0
                       else "status=%r" % (self.profile_done,)))
        return None

    def receipt_error(self, run_dir) -> str | None:
        """Section 6: the run dir's mc173-diag.log must hold EXACTLY ONE
        `[login-gate] <player> /login len=8 sha256=<hex>` line, whose player is
        this run's username and whose hash is SHA-256 over the raw argument
        bytes. No plaintext credential is logged (section 8 ruling 4), so the
        hash is the only proof the server saw the bytes the console typed.

        Section 9 ruling 7 closes the binding on BOTH sides: the player must
        equal the username the DEVICE echoed in p0.session -- which note()
        already proved equal to the collector's own sha256 derivation for this
        run -- so a receipt can only certify the run it was written for."""
        path = Path(run_dir) / CHAT_LOGIN_RECEIPT_NAME
        # Section 11 L6: the expected player is the collector's OWN derivation
        # from the run id. The device-echoed name is checked against it in
        # note(), so it may confirm the binding but never supply it -- with no
        # run id there is no expectation at all, and the run fails closed.
        if not self.run_id:
            self.receipt = {"path": CHAT_LOGIN_RECEIPT_NAME, "ok": False,
                            "error": "run id missing"}
            return ("run id missing: the server receipt can only be bound to a "
                    "run by the username derived from its run id")
        want_player = chat_login_username(self.run_id)
        want_sha = hashlib.sha256(CHAT_LOGIN_SECRET).hexdigest()
        self.receipt = {"path": CHAT_LOGIN_RECEIPT_NAME, "lines": 0,
                        "want_player": want_player, "want_sha256": want_sha,
                        "session_username": self.session_username,
                        "derived_username": chat_login_username(self.run_id),
                        "player": None, "len": None, "sha256": None,
                        "ok": False}
        if not path.is_file():
            return ("server receipt %s is missing from the run directory -- "
                    "section 6 requires a fresh log carrying exactly one "
                    "[login-gate] line" % CHAT_LOGIN_RECEIPT_NAME)
        text = CHAT_LOGIN_ANSI_RE.sub(
            "", path.read_text(encoding="utf-8-sig", errors="replace"))
        gate = [line.strip() for line in text.splitlines()
                if CHAT_LOGIN_RECEIPT_MARK in line]
        self.receipt["lines"] = len(gate)
        if len(gate) != 1:
            return ("server receipt carries %d [login-gate] lines -- section 6 "
                    "requires exactly one per run" % len(gate))
        match = CHAT_LOGIN_RECEIPT_RE.search(gate[0])
        if not match:
            return ("server receipt line does not match section 6's syntax "
                    "([login-gate] <player> /login len=<n> sha256=<hex>): %r"
                    % gate[0][:96])
        player, length, sha = match.group(1), int(match.group(2)), match.group(3).lower()
        self.receipt.update(player=player, len=length, sha256=sha)
        if len(player) != 8 or not player.startswith("cw"):
            return ("server receipt player %r is not a run nonce (\"cw\" plus "
                    "6 hex characters, section 9 ruling 7)" % (player,))
        if player != want_player:
            return ("server receipt player %r is not this run's username %r "
                    "(device-echoed %r, derived %r) -- the receipt belongs to "
                    "another run"
                    % (player, want_player, self.session_username,
                       chat_login_username(self.run_id)))
        if length != CHAT_LOGIN_SECRET_LEN:
            return ("server receipt reports len=%d; section 6 requires len=%d "
                    "over the raw argument bytes"
                    % (length, CHAT_LOGIN_SECRET_LEN))
        if sha != want_sha:
            return ("server receipt sha256=%s does not hash the argument the "
                    "console typed (%s)" % (sha, want_sha))
        self.receipt["ok"] = True
        return None

    def pass_label(self) -> str:
        """What a PASS of THIS run is worth. Section 0/ruling 7 fixes the
        real-Wii wording; section 10 ruling 7 forbids a PC run from wearing
        it until the third seam lands and a console run passes."""
        return (CHAT_LOGIN_PC_PASS_LABEL if self.platform == "pc"
                else CHAT_LOGIN_PASS_LABEL)

    def summary(self) -> dict:
        device = self.complete_error()
        return {"error": self.error, "markers": self.markers,
                "last_seq": self.last_seq,
                "phases": {phase: sum(n for (p, _m), n in self.count.items()
                                      if p == phase)
                           for phase in ("p0", "p1", "p2", "p3", "p4", "p5",
                                         "p6", "run")},
                "run_ok": self.finished,
                "profile_done": self.profile_done,
                "username": self.session_username,
                "platform": self.platform,
                # Which marker contract graded this run (Astra ruling 2 D2).
                # Reported as the PAIR (profile, number): the chord profile
                # numbers its own contracts from 101 so the two series can
                # never be confused in a PASS.json (review finding 4).
                "profile": CHAT_LOGIN_PROFILE, "contract_event": "chat_login",
                "contract": self.contract,
                # Reported, never scored (section 10 ruling 3).
                "frozen_rearm_mask": self.frozen_rearm_mask,
                "hold_ms": self.hold_ms,
                # Device completion is its OWN reported field (section 5): a
                # console that did everything asked still needs the server's
                # receipt before the run PASSES.
                "device_complete": device is None,
                "device_error": device,
                "draft": self.draft_text, "draft_len": self.draft_len,
                "keys": self.key_index + 1,
                "trace_batches": self.trace_batches,
                "trace_frames": self.trace_frames,
                "legs": self.legs,
                # Reported, never a tolerance (section 11 H1). Per-leg values
                # ride in "legs" above, and only those gate the cadence.
                "max_frame_ms_run": self.max_frame_ms_run,
                # Section 3's release attribution: reported, never scored.
                "release_lag_ms": self.release_lag_ms,
                "lines_sent": [self.lines_before, self.lines_after],
                "ack_ring_index": self.ack_ring_index,
                "ack_wait_ms": self.ack_wait_ms,
                "send_ring": [self.ring_head, self.ring_count],
                "receipt": self.receipt,
                "proof_scope": self.pass_label()}


class MpPlayActionLogin(ChatLoginEvidence):
    """REL-14b-0: one generation of mp_play's ACTION-ONLY login, graded by
    ChatLoginEvidence's own rules (gapless seq, run nonce, the platform's pad
    incl. the Wii Nunchuk category) against MP_PLAY_LOGIN_ORDER only."""

    def __init__(self, run_id: str = ""):
        super().__init__(run_id)
        self.expected, self.ordinal = MP_PLAY_LOGIN_TABLES

    def _note_values(self, key, event) -> bool:
        ok = super()._note_values(key, event)
        self.expected, self.ordinal = MP_PLAY_LOGIN_TABLES  # p0 swaps them
        if ok and key == MP_PLAY_LOGIN_ORDER[-1] and (
                event.get("typed_login") is not False):
            self.error = ("contradictory evidence: p1.actions_ready "
                          "typed_login=%r, the action-only login types nothing"
                          % (event.get("typed_login"),))
            return False
        return ok


class MpPlayEvidence:
    """mp_play_10m's verdict (docs/mp_play_profile.md): a marker SEQUENCE from
    the device, plus the mc173-diag fixture's own receipts, with every counted
    action matched 1:1 to a receipt by (nonce, generation, operation identity).

    Modelled on ChatLoginEvidence -- the first rejection latches and stops, so a
    stream that goes wrong early reports WHY rather than the pile of
    consequences -- and split the same way into two halves that are judged and
    reported separately:

      * `complete_error()` is the DEVICE half: the marker set, their order, a
        gapless seq, the readings a marker carries that can disprove its own
        claim (timeout_admits, since_connect_ms, measured_ms, verified.ok), and
        the shared login sub-stream (`note_login`).
      * `receipt_error(run_dir)` is the SERVER half: the fixture log. It parses
        every `[mp-play]` receipt, matches each counted device marker to
        exactly one of them, applies the floors and the spread rule to the
        MATCHED actions only, measures the walked displacement from the server's
        own `pos` receipts, and compares the two snapshots.

    A console that did everything asked in front of a server that saw none of it
    is therefore an honest device-PASS / overall-FAIL, exactly as
    mp_chat_login_3m already is.

    WHAT EARNS NOTHING (Astra Lane B item 4): a receipt is consumed ONCE, and a
    duplicate, stale (wrong generation), rejected or unrelated receipt matches
    nothing. Device markers left unmatched are counted and REPORTED -- they are
    not fatal in themselves, but they cannot carry a floor, so "the floors were
    met only by unmatched markers" fails by name.

    WHAT IS NEVER INFERRED: no respawn, teleport-assisted displacement or
    fixture restoration may conceal a failure (Astra Lane B item 5). A `death`
    receipt, a `health h=0` receipt or a device `fail` marker fails the run at
    once; a `tp=1` position is REMOVED from the walked displacement rather than
    counted towards it."""

    def __init__(self, run_id: str = ""):
        self.run_id = run_id or ""
        self.error: str | None = None
        self.markers = 0
        self.last_seq = 0
        self.last_ordinal = -1
        self.last_key: tuple[str, int] | None = None
        self.count: dict[tuple[str, int], int] = {}
        self.finished = False
        self.profile_done: str | None = None
        self.profile_done_count = 0
        self.done_before_ok = False
        self.done_at_marker = 0
        # p0 of the run: what the device says it is and what it is bound to.
        self.contract: int | None = None
        self.platform: str | None = None
        self.nonce: str | None = None
        self.measured_floor_ms: int | None = None
        self.total_cap_ms: int | None = None
        # Per generation readings the verdict quotes.
        self.timeout_admits: dict[int, int] = {}
        self.since_connect_ms: dict[int, int] = {}
        # Measured play.
        self.measuring = False
        self.p2_t0_ms: int | None = None
        self.measured_ms: int | None = None
        self.p2_end: dict | None = None
        self.teleports = 0
        # The counted actions, in emission order, each as a dict carrying the
        # identity the receipt has to agree with.
        self.digs: list[dict] = []
        self.places: list[dict] = []
        self.inv_moves: list[dict] = []
        self.picks = 0
        self.reserve_kept = 0
        self.build_skipped = 0
        self.inv_skipped = 0
        # push-0921/B12 narration tallies (8bf54845). Reported, graded by
        # nothing -- see MP_PLAY_PLAY_MARKERS for why each is narration.
        self.plants_cleared = 0
        self.pickups_missed = 0
        self.pickup_missed_rows = []
        # Pit-escape narration (escape_step / escape_mine / escape_failed).
        self.escape_steps = 0
        self.escape_mines = 0
        self.escape_failed = 0
        self.escape_centred = 0
        # hw1021f: the walk ended as success at this stuck position (or None).
        self.walk_done_stuck: dict | None = None
        # Filled by classify_pickup_misses() during receipt_error.
        self.pickup_miss_cases = []
        # c4488f36: how many walk legs turned 90 degrees for lack
        # of progress. Reported, never graded -- the displacement
        # floor is what judges the walk.
        self.heading_turns = 0
        # Snapshots: {"snapshot_pre"|"snapshot_post": {...}} with the inventory
        # already canonicalised.
        self.snapshots: dict[str, dict] = {}
        self._open_snapshot: str | None = None
        self._snapshot_parts = 0
        self.disconnected_ms: int | None = None
        self.rejoin_ms: int | None = None
        self.verified: dict | None = None
        self.post_action: dict | None = None
        self.seq_total: int | None = None
        # The shared login sub-machine's own markers (event chat_login), once
        # per generation -- see note_login().
        self.login: dict[int, ChatLoginEvidence] = {}
        self.login_gen = 1
        self.login_error_text: str | None = None
        # Filled by receipt_error().
        self.receipt: dict | None = None
        self.matched = {"dig": 0, "place": 0, "inv_move": 0}
        self.unmatched: list[str] = []
        self.displacement: float | None = None

    # -- the device half ---------------------------------------------------

    def heartbeat_floor(self, default: int) -> int:
        """Like mp_chat_login_3m's, and for the same reason: the driver ends the
        run on its last assertion, not on a clock, so a fixed count would FAIL
        an honest run that finished early (the split_recovery_6m lesson). The
        telemetry-health question is CADENCE -- 0.8 heartbeats per second of the
        run's OWN measured play. Unlike the login profile this clock has a hard
        FLOOR (600 000 ms), so the derived number can never fall below the
        table's 480; a longer run simply has to show more beats. With no
        p2_end at all the table default applies, and the marker contract is
        what fails such a run."""
        if not self.measured_ms:
            return default
        return max(10, int(0.8 * self.measured_ms / 1000.0))

    def note_profile_done(self, status) -> None:
        self.profile_done_count += 1
        self.profile_done = status
        # The contract ends with `ok` FIRST and profile_done("ok") only then, so
        # WHEN the completion arrived is evidence in itself.
        self.done_before_ok = not self.finished
        self.done_at_marker = self.markers
        if self.profile_done_count > 1 and not self.error:
            self.error = ("more than one profile_done for a single run (the "
                          "driver sends exactly one, always)")

    def note(self, event: dict) -> None:
        """One `mp_play` marker line (mp_play_map.h's mpp_map_marker_line)."""
        if self.error:
            return
        if self.finished:
            self.error = ("markers continued after `ok` (seq %r) -- `ok` is the "
                          "last marker of a run" % (event.get("seq"),))
            return
        if self.run_id and event.get("run_id") != self.run_id:
            self.error = ("stale-run evidence: marker run_id=%r is not the run "
                          "being validated (%r)"
                          % (event.get("run_id"), self.run_id))
            return
        marker, seq = event.get("marker"), event.get("seq")
        if not isinstance(marker, str) or type(seq) is not int:
            self.error = ("malformed mp_play marker: %s"
                          % json.dumps(event, sort_keys=True)[:120])
            return
        if seq <= self.last_seq:
            self.error = ("seq not strictly increasing: %s carries seq=%d after "
                          "seq=%d" % (marker, seq, self.last_seq))
            return
        if seq != self.last_seq + 1:
            self.error = ("seq gap before %s: seq=%d follows seq=%d, but a "
                          "refused emit retries the SAME seq, so the run is "
                          "gapless 1..N" % (marker, seq, self.last_seq))
            return
        self.last_seq = seq
        self.markers += 1
        # The failure marker first: it carries the only device-side diagnosis
        # there is, and the queue reserves its last slot for exactly this.
        if marker == "fail":
            self.error = ("device fail marker in phase %r (gen %r) after "
                          "%r ms of measured play: %s"
                          % (event.get("phase"), event.get("gen"),
                             event.get("measured_ms"), event.get("diag")))
            return
        if marker == "inv":
            self._note_inv(event)
            return
        if marker in MP_PLAY_FREE_MARKERS:
            self._note_play(marker, event)
            return
        self._note_phase(marker, seq, event)

    def _gen_of(self, marker, event):
        """The generation a marker carries. Only the three markers emitted once
        per generation put it in their KEY; for the rest it is read and checked
        where it means something, but it never names two table slots."""
        gen = event.get("gen")
        if marker in ("ready", "login_begin", "login_ok"):
            if gen not in MP_PLAY_GENERATIONS:
                self.error = ("malformed %s: gen=%r is not one of %s -- this "
                              "marker is emitted once per generation and its "
                              "generation is part of its identity"
                              % (marker, gen, list(MP_PLAY_GENERATIONS)))
                return None
            return gen
        return 0

    def _note_phase(self, marker, seq, event) -> None:
        gen = self._gen_of(marker, event)
        if gen is None:
            return
        key = (marker, gen)
        ordinal = MP_PLAY_PHASE_ORDINAL.get(key)
        if ordinal is None:
            self.error = ("unexpected marker %r: not a marker of the mp_play_10m "
                          "contract" % (marker,))
            return
        if ordinal < self.last_ordinal:
            self.error = ("reordered markers: %s arrived after %s"
                          % (self._name(key), self._name(self.last_key)))
            return
        seen = self.count.get(key, 0) + 1
        if seen > 1:
            self.error = ("duplicate marker %s: seen %d times, the contract "
                          "emits it once" % (self._name(key), seen))
            return
        if self._open_snapshot is not None:
            self.error = ("snapshot %s was cut short: %d of its %d `inv` parts "
                          "arrived before %s"
                          % (self._open_snapshot, self._snapshot_parts,
                             MP_PLAY_INV_PARTS, self._name(key)))
            return
        if not self._note_values(marker, gen, seq, event):
            return
        self.count[key] = seen
        self.last_ordinal = ordinal
        self.last_key = key
        if marker == "ok":
            self.finished = True

    @staticmethod
    def _name(key) -> str:
        if not key:
            return "(nothing)"
        marker, gen = key
        return "%s(gen %d)" % (marker, gen) if gen else marker

    def _note_values(self, marker, gen, seq, event) -> bool:
        """The readings that can disprove a marker's own claim. Everything else
        the profile reports (positions, cycle counts, why a build leg was
        skipped) rides into the artifact untouched."""
        if marker == "session":
            # The device STATES the contract it was built against; a stream
            # built against a different one cannot be graded by this file.
            contract = event.get("contract")
            if contract not in MP_PLAY_CONTRACTS:
                self.error = ("malformed session: contract=%r is not one of %s"
                              % (contract, list(MP_PLAY_CONTRACTS)))
                return False
            self.contract = contract
            floor, cap = (event.get("measured_floor_ms"),
                          event.get("total_cap_ms"))
            if floor != MP_PLAY_MEASURED_FLOOR_MS or cap != MP_PLAY_TOTAL_CAP_MS:
                self.error = ("contradictory evidence: session declares "
                              "measured_floor_ms=%r total_cap_ms=%r, but "
                              "contract %d is %d / %d -- this .dol was built "
                              "against a different contract"
                              % (floor, cap, contract,
                                 MP_PLAY_MEASURED_FLOOR_MS,
                                 MP_PLAY_TOTAL_CAP_MS))
                return False
            self.measured_floor_ms, self.total_cap_ms = floor, cap
            platform = event.get("platform")
            if platform not in CHAT_LOGIN_PLATFORMS:
                self.error = ("malformed session: platform=%r is not one of %s"
                              % (platform, "/".join(CHAT_LOGIN_PLATFORMS)))
                return False
            self.platform = platform
            # The run nonce IS the login username, and the collector re-derives
            # it rather than trusting the echo -- a nonce that is not this
            # run's cannot bind a receipt to it (the mp_chat_login_3m rule,
            # unchanged).
            nonce, want = event.get("nonce"), chat_login_username(self.run_id)
            if (not isinstance(nonce, str) or len(nonce) != 8
                    or not nonce.startswith("cw")):
                self.error = ("malformed session: nonce=%r is not the "
                              "contract's \"cw\" + 6 hex characters" % (nonce,))
                return False
            if self.run_id and nonce != want:
                self.error = ("stale-run evidence: session nonce=%r is not this "
                              "run's derivation %r (sha256 of the run id)"
                              % (nonce, want))
                return False
            self.nonce = nonce
            if seq != 1:
                self.error = ("session carries seq=%d -- it is the first marker "
                              "of a run" % (seq,))
                return False
        elif marker == "ready":
            admits = event.get("timeout_admits")
            if type(admits) is not int:
                self.error = ("malformed ready(gen %d): timeout_admits=%r is "
                              "not an int, so \"no backstop\" is unprovable"
                              % (gen, admits))
                return False
            self.timeout_admits[gen] = admits
            if admits != 0:
                # Astra Lane B item 2: "no backstop" needs an OBSERVED normal
                # release. A world the 30 s timeout admitted is not one.
                self.error = ("ready(gen %d) timeout_admits=%d: the load screen "
                              "was released by the backstop, not by an observed "
                              "normal release" % (gen, admits))
                return False
        elif marker == "login_ok":
            since = event.get("since_connect_ms")
            if type(since) is not int:
                self.error = ("malformed login_ok(gen %d): since_connect_ms=%r "
                              "is not an int" % (gen, since))
                return False
            self.since_connect_ms[gen] = since
            if since >= MP_PLAY_LOGIN_DEADLINE_MS:
                self.error = ("login_ok(gen %d) since_connect_ms=%d: the login "
                              "did not make the server's %d ms deadline (both "
                              "generations must meet it independently)"
                              % (gen, since, MP_PLAY_LOGIN_DEADLINE_MS))
                return False
        elif marker == "p2_begin":
            t0 = event.get("t0_ms")
            if type(t0) is not int:
                self.error = ("malformed p2_begin: t0_ms=%r is not an int"
                              % (t0,))
                return False
            self.p2_t0_ms = t0
            self.measuring = True
        elif marker == "p2_end":
            measured = event.get("measured_ms")
            if type(measured) is not int:
                self.error = ("malformed p2_end: measured_ms=%r is not an int"
                              % (measured,))
                return False
            self.measuring = False
            self.measured_ms = measured
            self.p2_end = {k: event.get(k) for k in
                           ("measured_ms", "cycles", "digs", "picks", "places",
                            "inv_moves", "build_skipped", "teleports")}
            if measured < MP_PLAY_MEASURED_FLOOR_MS:
                # Astra Lane B item 5: measured play is P2 ONLY -- loading,
                # login, reconnect and settling are excluded, so this number is
                # the only one the floor may be applied to.
                self.error = ("measured play was %d ms, below the contract's "
                              "%d ms floor (loading, login, reconnect and "
                              "settling are excluded by construction)"
                              % (measured, MP_PLAY_MEASURED_FLOOR_MS))
                return False
            # The device's own tallies must agree with the markers it emitted:
            # a disagreement means either the count or the marker stream is
            # wrong, and both are disqualifying.
            for field, got in (("digs", len(self.digs)),
                               ("places", len(self.places)),
                               ("inv_moves", len(self.inv_moves)),
                               ("picks", self.picks),
                               ("build_skipped", self.build_skipped)):
                if event.get(field) != got:
                    self.error = ("contradictory evidence: p2_end %s=%r, but "
                                  "the run emitted %d such marker(s)"
                                  % (field, event.get(field), got))
                    return False
            if event.get("teleports") != self.teleports:
                self.error = ("contradictory evidence: p2_end teleports=%r, but "
                              "%d teleport marker(s) were emitted"
                              % (event.get("teleports"), self.teleports))
                return False
        elif marker in ("snapshot_pre", "snapshot_post"):
            want_gen = 1 if marker == "snapshot_pre" else 2
            if event.get("gen") != want_gen:
                self.error = ("contradictory evidence: %s carries gen=%r, not "
                              "%d" % (marker, event.get("gen"), want_gen))
                return False
            if event.get("parts") != MP_PLAY_INV_PARTS:
                self.error = ("malformed %s: parts=%r, the contract splits an "
                              "inventory into %d" % (marker, event.get("parts"),
                                                     MP_PLAY_INV_PARTS))
                return False
            for axis in ("x", "y", "z"):
                if not isinstance(event.get(axis), (int, float)):
                    self.error = ("malformed %s: %s=%r is not a number"
                                  % (marker, axis, event.get(axis)))
                    return False
            if type(event.get("dim")) is not int:
                self.error = ("malformed %s: dim=%r is not an int"
                              % (marker, event.get("dim")))
                return False
            self.snapshots[marker] = {
                "gen": want_gen, "x": float(event["x"]), "y": float(event["y"]),
                "z": float(event["z"]), "dim": event["dim"], "slots": {}}
            self._open_snapshot = marker
            self._snapshot_parts = 0
        elif marker == "disconnected":
            if event.get("gen") != 1:
                self.error = ("contradictory evidence: disconnected carries "
                              "gen=%r, not 1" % (event.get("gen"),))
                return False
            self.disconnected_ms = event.get("ms")
        elif marker == "rejoin_begin":
            if event.get("gen") != 2:
                self.error = ("contradictory evidence: rejoin_begin carries "
                              "gen=%r, not 2" % (event.get("gen"),))
                return False
            # Everything after this marker belongs to the second connection,
            # including the shared login machine's second run: this is the one
            # place the two chat_login sub-streams are told apart, because they
            # are one continuous seq numbering with no generation of their own.
            self.login_gen = 2
        elif marker == "verified":
            ok, rejoin = event.get("ok"), event.get("rejoin_ms")
            if type(ok) is not bool or type(rejoin) is not int:
                self.error = ("malformed verified: ok=%r rejoin_ms=%r"
                              % (ok, rejoin))
                return False
            self.rejoin_ms = rejoin
            self.verified = {"ok": ok, "rejoin_ms": rejoin,
                             "diag": event.get("diag")}
            if not ok:
                self.error = ("verified ok=false: the server-owned state did "
                              "not survive the rejoin -- %s"
                              % (event.get("diag") or "no diagnosis"))
                return False
            if rejoin > MP_PLAY_REJOIN_BUDGET_MS:
                # Astra answer B2: the budget runs from initiating reconnect
                # through authenticated, loaded, state-verified gameplay.
                self.error = ("rejoin took %d ms, past the contract's %d ms "
                              "budget (reconnect -> verified gameplay)"
                              % (rejoin, MP_PLAY_REJOIN_BUDGET_MS))
                return False
        elif marker == "post_action":
            # REL-14b: gen 2's whole-stack move, graded on its receipts
            rec = {k: event.get(k) for k in (
                "gen", "n", "window", "action_pick", "action_put", "from_slot",
                "to_slot", "item", "count", "damage", "measured_ms")}
            bad = ["%s=%r" % kv for kv in rec.items() if type(kv[1]) is not int]
            if bad:
                self.error = "malformed post_action: " + ", ".join(bad)
                return False
            for wrong, why in (
                    (rec["gen"] != 2, "carries gen=%d, not 2" % rec["gen"]),
                    (rec["action_pick"] == rec["action_put"], "reports one "
                     "action number (%d) for both clicks" % rec["action_put"]),
                    (rec["n"] != len(self.inv_moves),
                     "n=%d, but generation 1 made %d inventory moves"
                     % (rec["n"], len(self.inv_moves)))):
                if wrong:
                    self.error = "contradictory evidence: post_action " + why
                    return False
            self.post_action = dict(rec, matched=False)
        elif marker == "ok":
            total = event.get("seq_total")
            self.seq_total = total
            if total != seq:
                self.error = ("contradictory evidence: ok seq_total=%r, but it "
                              "is itself marker seq=%d of a gapless run"
                              % (total, seq))
                return False
        return True

    def _note_inv(self, event) -> None:
        """One inventory PART of the snapshot that is currently open."""
        head = self._open_snapshot
        if head is None:
            self.error = ("an `inv` part arrived with no open snapshot -- the "
                          "parts follow their own snapshot_pre / snapshot_post "
                          "head")
            return
        if event.get("of") != head:
            self.error = ("`inv` part claims of=%r while %s is the open "
                          "snapshot" % (event.get("of"), head))
            return
        if event.get("gen") != self.snapshots[head]["gen"]:
            self.error = ("`inv` part of %s carries gen=%r, not %d"
                          % (head, event.get("gen"),
                             self.snapshots[head]["gen"]))
            return
        if event.get("part") != self._snapshot_parts:
            self.error = ("`inv` part of %s arrived out of order: part=%r, "
                          "expected %d" % (head, event.get("part"),
                                           self._snapshot_parts))
            return
        rows = event.get("slots")
        if not isinstance(rows, list):
            self.error = ("malformed `inv` part %d of %s: slots=%r is not a "
                          "list" % (self._snapshot_parts, head, rows))
            return
        slots = self.snapshots[head]["slots"]
        for row in rows:
            if (not isinstance(row, list) or len(row) != 4
                    or any(type(v) is not int for v in row)):
                self.error = ("malformed `inv` entry %r in part %d of %s -- the "
                              "contract writes [slot,item,count,damage]"
                              % (row, self._snapshot_parts, head))
                return
            slot, item, count, damage = row
            if slot in slots:
                self.error = ("`inv` part %d of %s repeats slot %d"
                              % (self._snapshot_parts, head, slot))
                return
            slots[slot] = (item, count, damage)
        self._snapshot_parts += 1
        if self._snapshot_parts >= MP_PLAY_INV_PARTS:
            self.snapshots[head]["canonical"] = mp_play_canonical_inventory(
                slots)
            self._open_snapshot = None

    def _note_play(self, marker, event) -> None:
        """A marker emitted inside measured play (or, for `teleport`, any
        in-world phase). These are exempt from the phase ORDER -- the world
        decides when a usable column turns up -- but never from their window."""
        if marker != "teleport" and not self.measuring:
            self.error = ("%s arrived outside measured play: the window is "
                          "p2_begin..p2_end, and only what happens inside it "
                          "counts" % (marker,))
            return
        if marker == "teleport":
            self.teleports += 1
            return
        if self.walk_done_stuck is not None and marker in (
                "dig", "pickup", "place", "inv_move", "build_skipped",
                "heading_turned", "plant_cleared", "pickup_missed",
                "inv_skipped", "reserve_kept", "escape_step", "escape_mine",
                "escape_failed", "escape_centred",
                "walk_done_stuck"):
            self.error = ("%s after walk_done_stuck: the walk had ended, the "
                          "walker may only stand until p2_end" % (marker,))
            return
        if marker == "walk_done_stuck":
            # The device's claim that every walk-phase floor was met when the
            # walker got stuck. Checked against the tape it emitted: the counts
            # must be the markers' and at the floors. The server-side floors
            # (matched actions, spread, displacement, measured play) are graded
            # by receipt_error exactly as for any other run.
            row = {k: event.get(k) for k in
                   ("x", "y", "z", "measured_ms", "digs", "places",
                    "inv_moves", "disp")}
            if any(type(row[k]) not in (int, float)
                   for k in ("x", "y", "z", "disp")) or any(
                       type(row[k]) is not int for k in
                       ("measured_ms", "digs", "places", "inv_moves")):
                self.error = ("malformed walk_done_stuck: %r -- it must name "
                              "the stuck position and the counts" % (row,))
                return
            for field, got, floor in (
                    ("digs", len(self.digs), MP_PLAY_FLOOR_DIGS),
                    ("places", len(self.places), MP_PLAY_FLOOR_PLACES),
                    ("inv_moves", len(self.inv_moves),
                     MP_PLAY_FLOOR_INV_MOVES)):
                if row[field] != got or got < floor:
                    self.error = ("walk_done_stuck with %s=%r (tape has %d, "
                                  "floor %d): a stuck walker ends the walk "
                                  "only with every floor met"
                                  % (field, row[field], got, floor))
                    return
            if row["disp"] < MP_PLAY_MIN_DISPLACEMENT:
                self.error = ("walk_done_stuck with disp=%r below the %.0f "
                              "block floor" % (row["disp"],
                                               MP_PLAY_MIN_DISPLACEMENT))
                return
            self.walk_done_stuck = row
            return
        if marker == "reserve_kept":
            self.reserve_kept += 1
            return
        if marker == "build_skipped":
            self.build_skipped += 1
            return
        if marker == "heading_turned":
            # c4488f36's walk-leg narration, classed with build_skipped and
            # NOT with the counted actions below: it carries cycle and
            # heading_deg, no measured_ms, because it is not an action that has
            # to land in a third -- it reports that the NEXT leg will face
            # somewhere else. Counting it would put a non-action in the spread
            # rule; demanding measured_ms of it fails an honest run (PC run
            # pc_mp_play_20260921_131211).
            self.heading_turns += 1
            return
        if marker == "inv_skipped":
            self.inv_skipped += 1
            return
        if marker == "plant_cleared":
            # 8bf54845's place-leg narration, classed with build_skipped: the
            # crosshair settled on a plant standing where the block was to go,
            # and the driver broke it first. It carries n/x/y/z and NO
            # measured_ms -- it is not an action that has to land in a third of
            # measured play, it reports why the place that follows took longer.
            self.plants_cleared += 1
            return
        if marker in ("escape_step", "escape_mine", "escape_failed",
                      "escape_centred"):
            # The pit escape's narration: counted and REPORTED, graded by
            # nothing. Its digs and places never stand in for the build leg's
            # (they carry no measured_ms and are not in the action tables).
            if marker == "escape_step":
                self.escape_steps += 1
            elif marker == "escape_mine":
                self.escape_mines += 1
            elif marker == "escape_centred":
                self.escape_centred += 1
            else:
                self.escape_failed += 1
            return
        if marker == "pickup_missed":
            # Kept, not just counted (push-0921/B13): the cell is what ties the
            # miss to the fixture's item_spawn receipt, and that pairing is the
            # whole point of the item receipts.
            self.pickup_missed_rows.append(
                {k: event.get(k) for k in ("n", "x", "y", "z", "measured_ms")})
            # Same class. It DOES carry measured_ms (the core prints one), but
            # it is still not a counted action: a miss is the absence of a
            # pickup, so feeding it to the spread rule would let a run satisfy
            # a third with things that never happened. Reported so a reader can
            # see the collect path's real hit rate.
            self.pickups_missed += 1
            return
        if marker == "pickup":
            self.picks += 1
            return
        measured = event.get("measured_ms")
        if type(measured) is not int:
            self.error = ("malformed %s: measured_ms=%r is not an int, so the "
                          "action cannot be placed in measured play"
                          % (marker, measured))
            return
        if marker == "dig":
            record = {"n": event.get("n"), "x": event.get("x"),
                      "y": event.get("y"), "z": event.get("z"),
                      "old_id": event.get("old_id"), "measured_ms": measured}
            bad = [k for k in ("n", "x", "y", "z", "old_id")
                   if type(record[k]) is not int]
            table = self.digs
        elif marker == "place":
            record = {"n": event.get("n"), "x": event.get("x"),
                      "y": event.get("y"), "z": event.get("z"),
                      "id": event.get("id"), "measured_ms": measured}
            bad = [k for k in ("n", "x", "y", "z", "id")
                   if type(record[k]) is not int]
            table = self.places
        else:
            record = {k: event.get(k) for k in
                      ("n", "window", "action_pick", "action_put", "from_slot",
                       "to_slot", "item", "count", "damage")}
            record["measured_ms"] = measured
            bad = [k for k in record if type(record[k]) is not int]
            table = self.inv_moves
        if bad:
            self.error = ("malformed %s: %s is/are not int"
                          % (marker, ", ".join("%s=%r" % (k, record[k])
                                               for k in sorted(bad))))
            return
        if record["n"] != len(table) + 1:
            self.error = ("reordered markers: %s n=%d, expected n=%d (the "
                          "device numbers its counted actions from 1)"
                          % (marker, record["n"], len(table) + 1))
            return
        if marker == "inv_move" and record["action_pick"] == record["action_put"]:
            self.error = ("contradictory evidence: inv_move n=%d reports one "
                          "action number (%d) for both clicks -- a move is TWO "
                          "accepted clicks" % (record["n"],
                                               record["action_pick"]))
            return
        record["matched"] = False
        table.append(record)

    # -- the shared login sub-stream ---------------------------------------

    def note_login(self, event: dict) -> None:
        """One `chat_login` marker of the shared login machine, CALLED rather
        than copied (Astra answer B3), in REL-14b-0's ACTION-ONLY mode: one
        MpPlayActionLogin per generation, each re-observing p0.session and
        p1.pad_bound before p1.actions_ready, on one seq continued from gen 1
        (hence the seeding). heartbeat_floor() and profile_done belong to the
        mp_play driver, and no run.ok is emitted."""
        gen = self.login_gen
        evidence = self.login.get(gen)
        if evidence is None:
            evidence = MpPlayActionLogin(self.run_id)
            if gen > 1:
                first = self.login.get(1)
                if first is not None:
                    evidence.contract = first.contract
                    evidence.expected, evidence.ordinal = (first.expected,
                                                           first.ordinal)
                    evidence.platform = first.platform
                    evidence.session_username = first.session_username
                    evidence.last_seq = first.last_seq
            self.login[gen] = evidence
        evidence.note(event)

    def login_error(self) -> str | None:
        """Both logins, graded by ChatLoginEvidence's rules where they apply."""
        for gen in MP_PLAY_GENERATIONS:
            evidence = self.login.get(gen)
            if evidence is None:
                return ("no chat_login markers for generation %d -- the shared "
                        "login machine narrates every login it runs" % gen)
            if evidence.error:
                return "generation %d login: %s" % (gen, evidence.error)
            if evidence.platform != self.platform:  # pad_bound followed it
                return ("generation %d login: p0.session platform=%r, but the "
                        "run is %r -- every generation logs in on the run's "
                        "own platform and pad" % (gen, evidence.platform,
                                                  self.platform))
            for key, expected in evidence.expected.items():
                seen = evidence.count.get(key, 0)
                if expected is None:
                    if not seen:
                        return ("generation %d login: missing marker %s.%s"
                                % (gen, key[0], key[1]))
                elif seen < expected:
                    return ("generation %d login: missing marker %s.%s (%d of "
                            "the %d the contract requires)"
                            % (gen, key[0], key[1], seen, expected))
        first, second = self.login[1], self.login[2]
        if second.last_seq <= first.last_seq:
            return ("generation 2 login restarted its seq at %d: the shared "
                    "machine continues one numbering across both logins"
                    % (second.last_seq,))
        return None

    def complete_error(self) -> str | None:
        """The DEVICE side of the verdict: the marker contract alone. The
        fixture's receipts are judged separately (receipt_error)."""
        if not self.run_id:
            return ("run id missing: this profile's evidence is bound to the "
                    "run id (marker run_id, and the sha256 nonce every receipt "
                    "must name), so an unidentified run cannot pass")
        if self.error:
            return self.error
        if not self.markers:
            return ("no mp_play markers were received -- the scripted play "
                    "driver never ran")
        for key in MP_PLAY_PHASE_ORDER:
            if not self.count.get(key):
                return "missing phase marker %s" % self._name(key)
        if self._open_snapshot is not None:
            return ("snapshot %s was never completed: %d of its %d `inv` parts "
                    "arrived" % (self._open_snapshot, self._snapshot_parts,
                                 MP_PLAY_INV_PARTS))
        if not self.finished or self.last_key != ("ok", 0):
            return "`ok` is not the last marker of the run"
        problem = self.login_error()
        if problem:
            return problem
        if self.profile_done == "ok" and self.done_before_ok:
            return ("premature success: profile_done \"ok\" arrived before the "
                    "`ok` marker (after %d of %d markers)"
                    % (self.done_at_marker, self.markers))
        if self.profile_done != "ok":
            return ("profile_done %s: the contract ends a passing run with "
                    "exactly \"ok\""
                    % ("was never received" if self.profile_done_count == 0
                       else "status=%r" % (self.profile_done,)))
        return None

    # -- the server half ---------------------------------------------------

    @staticmethod
    def _receipt_fields(tail: str) -> dict:
        fields = {}
        for token in (tail or "").split():
            key, _, value = token.partition("=")
            if key:
                fields[key] = value
        return fields

    def _read_receipts(self, path) -> list[dict]:
        """Every receipt line of the fixture log, in order. `[login-gate]` lines
        are carried in the SAME list under the synthetic event `login_cmd`
        (design section 7 / Astra answer B1: the pre-existing login-gate line IS
        the login_cmd receipt) so that a login receipt can be attributed to the
        generation whose `login_accept` it follows -- it carries no `g=` of its
        own, and position in the one stdout stream is the binding."""
        rows = []
        text = CHAT_LOGIN_ANSI_RE.sub(
            "", path.read_text(encoding="utf-8-sig", errors="replace"))
        for raw in text.splitlines():
            line = raw.strip()
            if CHAT_LOGIN_RECEIPT_MARK in line:
                match = CHAT_LOGIN_RECEIPT_RE.search(line)
                rows.append({"ev": "login_cmd", "gen": None,
                             "player": match.group(1) if match else None,
                             "len": int(match.group(2)) if match else None,
                             "sha256": (match.group(3).lower() if match
                                        else None),
                             "raw": line})
                continue
            if MP_PLAY_RECEIPT_MARK not in line:
                continue
            match = MP_PLAY_RECEIPT_RE.search(line)
            if not match:
                rows.append({"ev": None, "raw": line})
                continue
            rows.append({"n": match.group(1), "gen": int(match.group(2)),
                         "ev": match.group(3),
                         "f": self._receipt_fields(match.group(4)),
                         "raw": line, "used": False})
        return rows

    def receipt_error(self, run_dir) -> str | None:
        """The SERVER half: the fixture's own receipts, and every rule that
        needs them. Ordered so the first thing a reader is told is the most
        basic thing that went wrong."""
        path = Path(run_dir) / CHAT_LOGIN_RECEIPT_NAME
        self.receipt = {"path": CHAT_LOGIN_RECEIPT_NAME, "ok": False,
                        "lines": 0, "nonce": self.nonce,
                        "derived_nonce": (chat_login_username(self.run_id)
                                          if self.run_id else None),
                        "login_cmd": {}, "disconnect": None,
                        "displacement": None, "matched": dict(self.matched),
                        "unmatched": []}
        if not self.run_id:
            self.receipt["error"] = "run id missing"
            return ("run id missing: a fixture receipt can only be bound to a "
                    "run by the nonce derived from its run id")
        vanilla = Path(run_dir) / MP_PLAY_VANILLA_LOG
        if vanilla.is_file():
            self.receipt["path"] = MP_PLAY_VANILLA_LOG
            return (self._vanilla_error(vanilla) if not path.is_file() else
                    "two receipt sources in the run directory (%s and %s): a "
                    "run is graded against ONE server"
                    % (MP_PLAY_VANILLA_LOG, CHAT_LOGIN_RECEIPT_NAME))
        if not path.is_file():
            return ("fixture receipt log %s is missing from the run directory "
                    "-- the contract needs the mc173-diag stdout of THIS run"
                    % CHAT_LOGIN_RECEIPT_NAME)
        want_nonce = chat_login_username(self.run_id)
        rows = self._read_receipts(path)
        self.receipt["lines"] = len(rows)
        malformed = next((r for r in rows if r.get("ev") is None), None)
        if malformed:
            return ("fixture receipt line does not match the contract's syntax "
                    "([mp-play] n=<nonce> g=<gen> ev=<event> k=v ...): %r"
                    % malformed["raw"][:96])
        play = [r for r in rows if r["ev"] != "login_cmd"]
        # UNATTRIBUTED item receipts are exempt from the foreign-nonce rule and
        # nothing else is (push-0921/B13). An item the fixture cannot tie to a
        # player is logged n=- g=0; without this exemption a run whose dropped
        # item merely despawned would be refused as "the log belongs to another
        # run", which is both wrong and the opposite of informative. The
        # exemption is deliberately narrow: only the nonce `-`, only on the
        # item events, and a receipt naming a DIFFERENT REAL player still
        # refuses the tape exactly as before -- that rule is what stops one
        # run's log being graded as another's.
        foreign = next((r for r in play
                        if r["n"] != want_nonce
                        and not (r["n"] == MP_PLAY_UNATTRIBUTED_NONCE
                                 and r["ev"] in MP_PLAY_ITEM_EVENTS)), None)
        if foreign:
            return ("fixture receipt names player %r, not this run's nonce %r "
                    "-- the log belongs to another run"
                    % (foreign["n"], want_nonce))
        problem = self._check_login_receipts(rows, want_nonce)
        if problem:
            return problem
        # BEFORE the safety and matching rules, and deliberately unconditional:
        # a run that ends early still carries misses worth explaining, and this
        # decides nothing -- it only says who owns each one (push-0921/B13).
        self.classify_pickup_misses(play)
        problem = self._check_safety_receipts(play)
        if problem:
            return problem
        problem = self._match_actions(play)
        if problem:
            return problem
        problem = self._check_floors_and_spread()
        if problem:
            return problem
        problem = self._check_displacement(play)
        if problem:
            return problem
        problem = self._check_disconnect(play)
        if problem:
            return problem
        problem = self._check_snapshots(play)
        if problem:
            return problem
        self.receipt["ok"] = True
        return None

    def _check_login_receipts(self, rows, want_nonce) -> str | None:
        """One `login_accept` per generation and NO `[login-gate]` line: since
        REL-14b-0 the device types no /login at all."""
        accepts = {}
        for row in rows:
            if row["ev"] == "login_accept":
                accepts[row["gen"]] = accepts.get(row["gen"], 0) + 1
            elif row["ev"] == "login_cmd":
                return ("the fixture logged a typed /login (%r) in an "
                        "action-only mp_play run" % row["raw"][-72:])
        for generation in MP_PLAY_GENERATIONS:
            if accepts.get(generation, 0) != 1:
                return ("the fixture logged %d login_accept receipts for "
                        "generation %d; the contract has exactly one per "
                        "generation" % (accepts.get(generation, 0), generation))
        return None

    def _vanilla_error(self, path) -> str:
        """REL-14b-ev2a: the pinned vanilla server.log binds the run: one Beta
        1.7.3 start on the manifest's port, only this run's nonce, two ordered
        logins from one client host with fresh entity ids, generation 1 left
        by the client's own quit (disconnect.quitting, CFR NetServerHandler.
        java:494-495), generation 2 at most that quit; any other loss (:404),
        a second record for one session, a warning or a typed command fails.
        Vanilla writes no per-action receipt, so until the action/state
        witness exists (REL-14b-ev2b) this FAILS CLOSED."""
        nonce = chat_login_username(self.run_id)
        try:
            port = json.loads(MP_PLAY_VANILLA_MANIFEST.read_text(
                encoding="utf-8"))["properties"]["server_port"]
        except (OSError, ValueError, KeyError, TypeError) as exc:
            return "vanilla fixture manifest unreadable: %r" % (exc,)
        records = []  # (kind, groups, line), complete records only
        for line in path.read_text(encoding="utf-8",
                                   errors="replace").splitlines():
            for kind, pattern in MP_PLAY_VANILLA_RECORDS:
                match = pattern.fullmatch(line)
                if match:
                    records.append((kind, match.groups(), line))
                    break
        starts = [g[0] for kind, g, _ in records if kind == "start"]
        if starts != ["Beta 1.7.3"]:
            return ("stale or foreign server.log: %d server starts (%s), the "
                    "run needs exactly one fresh Beta 1.7.3 start"
                    % (len(starts), starts))
        listen = [g[0] for kind, g, _ in records if kind == "port"]
        if listen != [str(port)]:
            return ("endpoint mismatch: the server listened on %s, not the "
                    "pinned fixture port %s" % (listen, port))
        sessions = []  # [entity id, client host, lost-connection reason]
        for kind, g, line in records:
            if kind in ("start", "port") or (g[0] != nonce and kind != "login"):
                continue
            if kind == "login" and g[0] != nonce:
                return ("foreign evidence: another player %r logged in; this "
                        "run's nonce is %r" % (g[0], nonce))
            if kind == "login":
                sessions.append([g[2], g[1], None])
            elif kind == "lost":
                if not sessions or sessions[-1][2] is not None:
                    return ("duplicate or unbound disconnect record: %r -- a "
                            "session ends at most once, after its login"
                            % line[-96:])
                sessions[-1][2] = g[1]
            else:
                return ("the server warned about this player or logged a typed "
                        "/login or command: %r" % line[-96:])
        if len(sessions) != 2:
            return ("%s logged in %d times; the contract has exactly two "
                    "sessions, one per generation" % (nonce, len(sessions)))
        if sessions[0][0] == sessions[1][0]:
            return ("stale generation: entity id %s twice, but every vanilla "
                    "login spawns a new player entity" % sessions[0][0])
        if sessions[0][1] != sessions[1][1]:
            return ("endpoint mismatch: generation 2 joined from %s, "
                    "generation 1 from %s" % (sessions[1][1], sessions[0][1]))
        for gen, (_, _, reason) in enumerate(sessions, 1):
            if reason != "disconnect.quitting" and (gen == 1 or reason):
                return ("premature disconnect: generation %d lost connection "
                        "%r, not the client's own quit (CFR NetServerHandler."
                        "java:404 vs :495)" % (gen, reason))
        return ("vanilla server-side action/state witness not implemented: "
                "both sessions are bound, but the digs, places, inventory moves "
                "and post-reconnect action need an independent witness "
                "(REL-14b-ev2b) -- failing closed")

    def classify_misses_from_run_dir(self, run_dir) -> None:
        """Classify the misses WITHOUT needing the verdict path to have reached
        receipt_error (push-0921/B13).

        It does not, in the case that matters most: a `profile_done` failure
        fails the run at the collector's top level, so the mp_play gate -- and
        with it receipt_error -- never runs. Run pc_mp_play_20260921_170107
        ended that way with nine item_spawn, nine item_collect and thirty
        item_gone receipts sitting in its log and not one miss explained. The
        explanation is exactly as useful for a run that died early, so it is
        made independent of the verdict rather than a side effect of it."""
        if self.pickup_miss_cases or not self.pickup_missed_rows:
            return
        path = Path(run_dir) / CHAT_LOGIN_RECEIPT_NAME
        if not path.is_file():
            return
        rows = self._read_receipts(path)
        play = [r for r in rows
                if r.get("ev") not in (None, "login_cmd")]
        self.classify_pickup_misses(play)

    def classify_pickup_misses(self, play) -> None:
        """WHERE DID THE DROPPED ITEM GO? One line per device `pickup_missed`,
        answering with the fixture's own item receipts (push-0921/B13).

        This GRADES NOTHING and can never change a verdict -- the device already
        decided the run's fate (one miss is narration, most of them fail by
        name). What it decides is WHO OWNS the defect, and the three cases have
        three different owners:

          no_spawn         the block broke and no item entity ever existed.
                           The loot path, fixture side.
          drifted          it spawned and the world removed it (despawn after
                           6000 ticks, or below y=-64). Physics: the drop
                           landed somewhere the body never went.
          still_in_world   it spawned, was never collected and was never
                           removed -- it is lying there at run's end. Also
                           physics, and the one the core could answer by
                           stepping toward the drop.
          handed_over      it spawned AND the server collected it for THIS
                           player, into a named slot -- so the server sent a
                           WindowSetItem the client never showed. THAT IS A
                           CAVEWII DEFECT, in the set-slot apply path, and the
                           slot number says exactly where to look.

        Matched by CELL, not by time: the receipts carry no clock, but the
        device's miss names the dug cell and the fixture's spawn carries the
        drop's position, and a dig empties exactly one cell. Each spawn is
        consumed once so two misses in the same cell cannot share one item."""
        self.pickup_miss_cases = []
        if not self.pickup_missed_rows:
            return
        spawns = [r for r in play if r["ev"] == "item_spawn"]
        collects = {r["f"].get("id"): r for r in play
                    if r["ev"] == "item_collect"}
        gones = {r["f"].get("id"): r for r in play if r["ev"] == "item_gone"}
        positions = [r for r in play if r["ev"] == "pos"]

        def fnum(row, key):
            try:
                return float(row["f"].get(key))
            except (TypeError, ValueError):
                return None

        for miss in self.pickup_missed_rows:
            cell = (miss.get("x"), miss.get("y"), miss.get("z"))
            case = {"miss": miss, "cell": cell}
            hit = None
            for row in spawns:
                if row.get("used_by_miss"):
                    continue
                sx, sy, sz = (fnum(row, "x"), fnum(row, "y"), fnum(row, "z"))
                if None in (sx, sy, sz) or None in cell:
                    continue
                # The drop starts inside the cell that was emptied; mc173 gives
                # it a small random velocity, so allow the block it was born in
                # plus one, rather than demanding an exact floor match.
                if (abs(math.floor(sx) - cell[0]) <= 1
                        and abs(math.floor(sy) - cell[1]) <= 1
                        and abs(math.floor(sz) - cell[2]) <= 1):
                    hit = row
                    row["used_by_miss"] = True
                    break
            if hit is None:
                case["case"] = "no_spawn"
                case["owner"] = "fixture loot path"
                self.pickup_miss_cases.append(case)
                continue
            item_id = hit["f"].get("id")
            case["item_entity"] = item_id
            case["spawn"] = {k: hit["f"].get(k)
                             for k in ("item", "count", "x", "y", "z",
                                       "vx", "vy", "vz")}
            if item_id in collects:
                case["case"] = "handed_over"
                case["owner"] = "CAVEWII -- the set-slot apply path"
                case["slot"] = collects[item_id]["f"].get("slot")
                case["collect"] = dict(collects[item_id]["f"])
            elif item_id in gones:
                case["case"] = "drifted"
                case["owner"] = "physics / the drop's own travel"
                case["reason"] = gones[item_id]["f"].get("reason")
            else:
                case["case"] = "still_in_world"
                case["owner"] = "physics / the drop's own travel"
                # The player's position nearest the spawn IN LOG ORDER. The
                # receipts carry no clock and `pos` is sampled every 5 s, so
                # this is the first sample after the drop was born -- roughly
                # spawn + 2 s on average, and stated that way rather than
                # claimed as exact.
                idx = play.index(hit)
                nxt = next((r for r in play[idx:] if r["ev"] == "pos"), None)
                if nxt is None and positions:
                    nxt = positions[-1]
                if nxt is not None:
                    px, py, pz = (fnum(nxt, "x"), fnum(nxt, "y"),
                                  fnum(nxt, "z"))
                    sx, sy, sz = (fnum(hit, "x"), fnum(hit, "y"),
                                  fnum(hit, "z"))
                    case["player_after"] = {"x": px, "y": py, "z": pz}
                    if None not in (px, py, pz, sx, sy, sz):
                        case["distance"] = round(
                            math.sqrt((px - sx) ** 2 + (py - sy) ** 2
                                      + (pz - sz) ** 2), 2)
            self.pickup_miss_cases.append(case)

    @staticmethod
    def _check_safety_receipts(play) -> str | None:
        """Astra Lane B item 5: sampled evidence cannot prove "never died", so
        the SERVER's own death / health receipts are read directly."""
        for row in play:
            if row["ev"] == "death":
                return ("the server logged a death receipt for generation %d -- "
                        "this profile arms no respawn and a death ends the run"
                        % row["gen"])
            if row["ev"] == "slot_state" and not all(
                    str(row["f"].get(k)).lstrip("-").isdigit() for k in
                    ("window", "slot", "item", "count", "damage")):
                return "malformed slot_state receipt: %r" % row["f"]
            if row["ev"] == "health":
                try:
                    health = int(row["f"].get("h"))
                except (TypeError, ValueError):
                    return ("malformed health receipt: h=%r"
                            % row["f"].get("h"))
                if health <= 0:
                    return ("the server logged health h=%d for generation %d -- "
                            "the player died" % (health, row["gen"]))
        return None

    def _match_actions(self, play) -> str | None:
        """Astra Lane B item 4, applied literally: 1:1 by (nonce, generation,
        operation identity), and a receipt is consumed ONCE. A duplicate, stale
        or unrelated receipt matches nothing; a rejected click fails by name.

        All counted play happens in generation 1 -- P2 is one continuous
        session -- so a receipt from generation 2 can never carry a play action,
        which is exactly what makes "receipt from the wrong generation" a
        detectable defect rather than a silent re-credit."""
        for record in self.digs:
            want = (1, record["x"], record["y"], record["z"], record["old_id"])
            row = self._take(play, "dig", want, self._dig_identity)
            if row is None:
                self.unmatched.append(
                    "dig n=%d at (%d,%d,%d) old_id=%d"
                    % (record["n"], record["x"], record["y"], record["z"],
                       record["old_id"]))
                continue
            record["matched"] = True
            self.matched["dig"] += 1
        for record in self.places:
            want = (1, record["x"], record["y"], record["z"], record["id"])
            row = self._take(play, "place", want, self._place_identity)
            if row is None:
                self.unmatched.append(
                    "place n=%d at (%d,%d,%d) id=%d"
                    % (record["n"], record["x"], record["y"], record["z"],
                       record["id"]))
                continue
            record["matched"] = True
            self.matched["place"] += 1
        for record in self.inv_moves:
            problem = self._match_inv_move(play, record)
            if problem:
                return problem
        if self.post_action:
            problem = self._match_inv_move(play, self.post_action, gen=2)
            if problem:  # its messages name it, not generation 1's n-th move
                return problem.replace(
                    "inv_move n=%d" % self.post_action["n"], "post_action", 1)
        self.receipt["matched"] = dict(self.matched)
        self.receipt["unmatched"] = list(self.unmatched)
        return None

    @staticmethod
    def _dig_identity(row):
        f = row["f"]
        try:
            return (row["gen"], int(f["x"]), int(f["y"]), int(f["z"]),
                    int(f["old_id"]))
        except (KeyError, TypeError, ValueError):
            return None

    @staticmethod
    def _place_identity(row):
        f = row["f"]
        try:
            return (row["gen"], int(f["x"]), int(f["y"]), int(f["z"]),
                    int(f["id"]))
        except (KeyError, TypeError, ValueError):
            return None

    @staticmethod
    def _take(play, ev, want, identity):
        for row in play:
            if row["used"] or row["ev"] != ev:
                continue
            if identity(row) == want:
                row["used"] = True
                return row
        return None

    def _match_inv_move(self, play, record, gen=1) -> str | None:
        """One move = TWO accepted clicks (Astra Lane B item 4), and the
        RESULTING slot state is verified -- not merely that two clicks were
        accepted. The slot_state receipts that follow the SECOND click are the
        server's own statement of where the stack ended up."""
        if record["item"] <= 0 or record["count"] <= 0:  # Beta accepts no-ops
            return ("inv_move n=%d claims item %d x%d: not a real transfer"
                    % (record["n"], record["item"], record["count"]))
        pick =self._take_click(play, gen, record["window"],
                                record["action_pick"], record["from_slot"])
        put = self._take_click(play, gen, record["window"],
                               record["action_put"], record["to_slot"])
        for name, row, action in (("pick-up", pick, record["action_pick"]),
                                  ("put-down", put, record["action_put"])):
            # every claimed click, counted BEFORE consumption: exactly one
            same = sum(r["ev"] == "click" and r["gen"] == gen and r["f"].get(
                "action") == str(action) for r in play)
            if same != 1 or row is None:
                return ("inv_move n=%d %s click: %d generation %d receipt(s) "
                        "name action %d; exactly one must, with its window, "
                        "slot, button 0 and shift 0"
                        % (record["n"], name, same, gen, action))
            if row["f"].get("accepted") != "true":
                return ("inv_move n=%d was credited to a REJECTED %s click "
                        "(window %s action %d, accepted=%s) -- a rejected "
                        "click earns nothing"
                        % (record["n"], name, row["f"].get("window"), action,
                           row["f"].get("accepted")))
        if next(r for r in play if r is pick or r is put) is put:
            return ("inv_move n=%d put-down click precedes its pick-up"
                    % record["n"])
        states = self._slot_states_after(play, pick)
        want = (record["item"], record["count"], record["damage"])
        src, cur = states.get(record["from_slot"]), states.get(-1)
        if (src, cur) != ((0, 0, 0), want):  # CFR Container.java:138
            return ("inv_move n=%d pick-up click left slot %d=%r, cursor=%r"
                    % (record["n"], record["from_slot"], src, cur))
        states = self._slot_states_after(play, put)
        got = states.get(record["to_slot"])
        if got is None:
            return ("inv_move n=%d has no slot_state receipt for its "
                    "destination slot %d after the put-down click, so the "
                    "server never confirmed where the stack landed"
                    % (record["n"], record["to_slot"]))
        if got != want:
            return ("inv_move n=%d claims slot %d holds item %d x%d dmg %d, but "
                    "the server's slot_state receipt says item %d x%d dmg %d"
                    % (record["n"], record["to_slot"], want[0], want[1],
                       want[2], got[0], got[1], got[2]))
        cursor = states.get(-1)
        if cursor is None:  # CFR Container.java:133: a whole stack empties it
            return ("inv_move n=%d has no empty-cursor slot_state receipt after "
                    "the put-down click" % record["n"])
        if cursor[0] > 0:
            return ("inv_move n=%d left the cursor holding item %d x%d after "
                    "the put-down click -- a move is a COMPLETED transfer"
                    % (record["n"], cursor[0], cursor[1]))
        record["matched"] = True
        self.matched["inv_move"] += gen == 1
        return None

    @staticmethod
    def _take_click(play, gen, window, action, slot):
        # The caller's generation only; a left click matches window, action,
        # slot, button 0, shift 0 (CFR NetServerHandler.java:534-543).
        for row in play:
            if row["used"] or row["ev"] != "click" or row["gen"] != gen:
                continue
            try:
                if (int(row["f"]["window"]) == window
                        and int(row["f"]["action"]) == action and [int(row[
                            "f"][k]) for k in ("slot", "button", "shift")]
                        == [slot, 0, 0]):
                    row["used"] = True
                    return row
            except (KeyError, TypeError, ValueError):
                continue
        return None

    @staticmethod
    def _slot_states_after(play, click) -> dict:
        """The slot_state receipts between `click` and the next click receipt --
        the ones the server wrote FOR that click."""
        states = {}
        # By IDENTITY, not by value: two click receipts can be textually equal,
        # and list.index() would then hand back the wrong one's slot states.
        index = next(i for i, row in enumerate(play) if row is click)
        for row in play[index + 1:]:
            if row["ev"] == "click" or row["gen"] != click["gen"]:
                break
            # exact window (CFR NetServerHandler.java:533)
            if (row["ev"], row["f"].get("window")) != ("slot_state",
                                                       click["f"]["window"]):
                continue
            try:
                states[int(row["f"]["slot"])] = (int(row["f"]["item"]),
                                                 int(row["f"]["count"]),
                                                 int(row["f"]["damage"]))
            except (KeyError, TypeError, ValueError):
                continue
        return states

    def _third_of(self, measured_ms: int) -> int:
        return min(2, max(0, measured_ms * 3 // max(1, self.measured_ms or 1)))

    def _check_floors_and_spread(self) -> str | None:
        """The floors count MATCHED actions only, and the spread rule is applied
        to the same set: an action the server never confirmed cannot carry
        either. Thirds are computed from the markers' own `measured_ms`."""
        for name, matched, total, floor in (
                ("dig", self.matched["dig"], len(self.digs),
                 MP_PLAY_FLOOR_DIGS),
                ("place", self.matched["place"], len(self.places),
                 MP_PLAY_FLOOR_PLACES),
                ("inv_move", self.matched["inv_move"], len(self.inv_moves),
                 MP_PLAY_FLOOR_INV_MOVES)):
            if matched < floor:
                return ("action floor not met: %d matched %s action(s) of the "
                        "%d the device claimed; the contract needs %d matched "
                        "to a server receipt (unmatched markers earn nothing)"
                        % (matched, name, total, floor))
        for name, table in (("dig", self.digs), ("place", self.places)):
            thirds = {self._third_of(r["measured_ms"])
                      for r in table if r["matched"]}
            missing = sorted({0, 1, 2} - thirds)
            if missing:
                return ("spread rule: no matched %s in third %s of measured "
                        "play -- the contract needs at least one matched dig "
                        "and one matched place in EACH third"
                        % (name, ", ".join(str(t + 1) for t in missing)))
        moves = {self._third_of(r["measured_ms"])
                 for r in self.inv_moves if r["matched"]}
        if len(moves) < 2:
            return ("spread rule: matched inventory moves fall in %d third(s) "
                    "of measured play; the contract needs at least two"
                    % (len(moves),))
        return None

    def _check_displacement(self, play) -> str | None:
        """Astra answer B2: the maximum HORIZONTAL distance from the initial
        join point, measured from the SERVER's own `pos` receipts, with
        teleports excluded.

        "Excluded" means the jump is REMOVED from the walked path, not merely
        that the jumping sample is skipped: a teleport shifts every later
        position, so the offset it introduced is subtracted from all of them.
        A run cannot buy displacement with a server correction.

        The whole delta of a `tp=1` sample is attributed to the server, so that
        sample earns no walked distance at all -- the player may well have
        walked part of the five seconds it covers, but nothing in the receipt
        says how much, and this profile does not guess in its own favour. The
        cost is at most one sampling interval of honest walking per teleport."""
        origin = None
        offset_x = offset_z = 0.0
        previous = None
        best = None
        for row in play:
            if row["ev"] != "pos" or row["gen"] != 1:
                continue
            try:
                x, z = float(row["f"]["x"]), float(row["f"]["z"])
            except (KeyError, TypeError, ValueError):
                return "malformed pos receipt: %r" % row["raw"][:96]
            if row["f"].get("tp") == "1" and previous is not None:
                offset_x += x - previous[0]
                offset_z += z - previous[1]
            previous = (x, z)
            walked = (x - offset_x, z - offset_z)
            if origin is None:
                origin = walked
            distance = ((walked[0] - origin[0]) ** 2
                        + (walked[1] - origin[1]) ** 2) ** 0.5
            best = distance if best is None else max(best, distance)
        if origin is None:
            return ("the fixture logged no generation 1 `pos` receipt, so the "
                    "walked displacement cannot be measured")
        self.displacement = best or 0.0
        self.receipt["displacement"] = self.displacement
        if self.displacement < MP_PLAY_MIN_DISPLACEMENT:
            return ("walked displacement was %.1f blocks (server-observed, "
                    "teleports removed); the contract needs >= %.0f"
                    % (self.displacement, MP_PLAY_MIN_DISPLACEMENT))
        return None

    def _check_disconnect(self, play) -> str | None:
        leaves = [row for row in play if row["ev"] == "disconnect"]
        first = next((row for row in leaves if row["gen"] == 1), None)
        if first is None:
            return ("the fixture logged no disconnect receipt for generation 1 "
                    "-- the run must leave through the production pause menu")
        reason = first["f"].get("reason")
        self.receipt["disconnect"] = {"reason": reason,
                                      "detail": first["f"].get("detail")}
        if reason != "quit":
            return ("generation 1 left the server with reason=%r (detail %r): "
                    "only a client-sent Disconnect (quit) is the production "
                    "pause-menu leave this profile claims"
                    % (reason, first["f"].get("detail")))
        return None

    def _check_snapshots(self, play) -> str | None:
        """Astra Lane B item 6: server-owned position, dimension and inventory
        before the disconnect, compared after the rejoin -- exact item / count /
        damage, declared position tolerance, dimension equal. Both the FIXTURE's
        pair and the DEVICE's own pair are compared, so a device that lied about
        what it saw and a server that lost the state are separate findings.

        Which fixture snapshot is which: the fixture writes one on every
        disconnect AND right after every login, so generation 1's DISCONNECT
        snapshot is its LAST one and generation 2's LOGIN snapshot is its
        FIRST."""
        snaps = [row for row in play if row["ev"] == "snapshot"]
        gen1 = [row for row in snaps if row["gen"] == 1]
        gen2 = [row for row in snaps if row["gen"] == 2]
        if not gen1:
            return ("the fixture logged no generation 1 snapshot receipt, so "
                    "there is no server-owned state to compare across the "
                    "rejoin")
        if not gen2:
            return ("the fixture logged no generation 2 snapshot receipt, so "
                    "the rejoined state was never observed by the server")
        before = self._fixture_snapshot(gen1[-1])
        after = self._fixture_snapshot(gen2[0])
        for name, snap in (("generation 1 disconnect", before),
                           ("generation 2 login", after)):
            if isinstance(snap, str):
                return "malformed %s snapshot receipt: %s" % (name, snap)
        self.receipt["snapshot_before"] = before
        self.receipt["snapshot_after"] = after
        if not any(entry.split(":")[1] != "0"
                   for entry in before["canonical"].split(",")):
            # The RESERVE rule exists so this compare means something: a fresh
            # player who placed everything it dug would leave with an empty
            # inventory, and "empty == empty" proves nothing about the server
            # keeping it (design section 7, draft 2b).
            return ("the pre-disconnect inventory was EMPTY: the reserve rule "
                    "makes a non-empty inventory mandatory, because an "
                    "empty-equals-empty compare proves nothing about the "
                    "server keeping the player's stacks")
        problem = self._compare_snapshots(
            "the server's own state across the rejoin", before, after)
        if problem:
            return problem
        for device_key, fixture, label in (
                ("snapshot_pre", before, "generation 1 disconnect"),
                ("snapshot_post", after, "generation 2 login")):
            # This half is graded even when the DEVICE half already failed, so
            # that an artifact always states both -- which means it can be
            # reached by a stream that never emitted the marker, or emitted its
            # head and not its parts. Say so rather than raising: the device
            # half is reporting the same absence in its own words.
            device = self.snapshots.get(device_key)
            if device is None or "canonical" not in device:
                return ("the device emitted no complete %s marker, so its own "
                        "view of the %s state cannot be compared with the "
                        "server's" % (device_key, label))
            problem = self._compare_snapshots(
                "the device's %s and the fixture's %s snapshot"
                % (device_key, label), device, fixture)
            if problem:
                return problem
        return None

    @staticmethod
    def _fixture_snapshot(row):
        f = row["f"]
        try:
            snap = {"x": float(f["x"]), "y": float(f["y"]), "z": float(f["z"]),
                    "dim": int(f["dim"])}
        except (KeyError, TypeError, ValueError):
            return "x/y/z/dim missing or not numeric"
        raw = (f.get("inv") or "").strip("[]")
        slots = {}
        for entry in raw.split(","):
            if not entry:
                continue
            parts = entry.split(":")
            if len(parts) != 4:
                return "inv entry %r is not slot:item:count:damage" % entry
            try:
                slot, item, count, damage = (int(p) for p in parts)
            except ValueError:
                return "inv entry %r is not numeric" % entry
            slots[slot] = (item, count, damage)
        snap["canonical"] = mp_play_canonical_inventory(slots)
        if f.get("inv_hash"):
            want = hashlib.sha256(raw.encode("utf-8")).hexdigest()
            if f["inv_hash"].lower() != want:
                return ("inv_hash %s does not hash the inv list it accompanies"
                        % f["inv_hash"])
        return snap

    @staticmethod
    def _compare_snapshots(what, before, after) -> str | None:
        if before["dim"] != after["dim"]:
            return ("%s disagree on dimension: %d vs %d"
                    % (what, before["dim"], after["dim"]))
        dx, dz = after["x"] - before["x"], after["z"] - before["z"]
        dy = after["y"] - before["y"]
        horizontal = (dx * dx + dz * dz) ** 0.5
        if horizontal > MP_PLAY_POS_TOL_H or abs(dy) > MP_PLAY_POS_TOL_V:
            return ("%s disagree on position: %.2f blocks horizontally and "
                    "%.2f vertically, past the %.1f / %.1f tolerance"
                    % (what, horizontal, dy, MP_PLAY_POS_TOL_H,
                       MP_PLAY_POS_TOL_V))
        if before["canonical"] != after["canonical"]:
            first = next((i for i, (a, b) in
                          enumerate(zip(before["canonical"].split(","),
                                        after["canonical"].split(",")))
                          if a != b), None)
            detail = "length"
            if first is not None:
                detail = ("slot entry %s vs %s"
                          % (before["canonical"].split(",")[first],
                             after["canonical"].split(",")[first]))
            return ("%s disagree on the inventory (%s) -- the comparison is "
                    "exact on item, count and damage" % (what, detail))
        return None

    def pass_label(self) -> str:
        return (MP_PLAY_PC_PASS_LABEL if self.platform == "pc"
                else MP_PLAY_PASS_LABEL)

    def summary(self) -> dict:
        device = self.complete_error()
        return {
            "error": self.error, "markers": self.markers,
            "last_seq": self.last_seq, "run_ok": self.finished,
            "profile": MP_PLAY_PROFILE, "contract_event": MP_PLAY_EVENT,
            "contract": self.contract, "platform": self.platform,
            "nonce": self.nonce, "profile_done": self.profile_done,
            "device_complete": device is None, "device_error": device,
            "login_error": self.login_error() if self.login else None,
            "login_generations": {
                gen: {"markers": ev.markers, "last_seq": ev.last_seq,
                      "run_ok": ev.finished,
                      # Reported, never graded -- and for generation 2 it spans
                      # the play phase by design (see note_login).
                      "max_frame_ms_run": ev.max_frame_ms_run}
                for gen, ev in sorted(self.login.items())},
            "timeout_admits": self.timeout_admits,
            "since_connect_ms": self.since_connect_ms,
            "measured_ms": self.measured_ms, "p2_end": self.p2_end,
            "actions": {"dig": len(self.digs), "place": len(self.places),
                        "inv_move": len(self.inv_moves), "pickup": self.picks},
            "matched": dict(self.matched), "unmatched": list(self.unmatched),
            "reserve_kept": self.reserve_kept,
            "build_skipped": self.build_skipped,
            "plants_cleared": self.plants_cleared,
            "pickups_missed": self.pickups_missed,
            "heading_turns": self.heading_turns,
            "escape_steps": self.escape_steps,
            "escape_mines": self.escape_mines,
            "escape_failed": self.escape_failed,
            "escape_centred": self.escape_centred,
            "walk_done_stuck": self.walk_done_stuck,
            "inv_skipped": self.inv_skipped, "teleports": self.teleports,
            "displacement": self.displacement,
            "disconnected_ms": self.disconnected_ms,
            "rejoin_ms": self.rejoin_ms, "verified": self.verified,
            "post_action": self.post_action,
            "receipt": self.receipt, "proof_scope": self.pass_label()}


def build_stamp_hash(stamp: str) -> str:
    """Comparable identity of a CW_BUILD_ID: the git-hash part (incl. the '+'
    dirty marker) before the '_MM.dd' suffix. The date is baked at BUILD time
    while the expected stamp is recomputed at COLLECT time, so full equality
    spuriously FAILed any build that crossed midnight before its run."""
    stamp = stamp or ""
    i = stamp.rfind("_")
    return stamp[:i] if i > 0 else stamp


def build_stamps_match(actual: str, expected: str) -> bool:
    """Hash-only comparison -- EXCEPT for degraded stamps: when git failed at
    build time the hash part is 'nogit' (or empty), whose date suffix is the
    ONLY distinguishing bit; hash-only compare would accept a days-stale
    nogit .dol. Degraded stamps fall back to full equality."""
    a = build_stamp_hash(actual)
    e = build_stamp_hash(expected)
    if a in ("", "nogit") or e in ("", "nogit"):
        return (actual or "") == (expected or "")
    return a == e

WD_MAIN_NAMES = [
    "IDLE",
    "CLIN",
    "CTICK",
    "PRERENDER",
    "LIGHTING",
    "LOD",
    "BUILD_CHUNKS",
    "SCRN_UPD",
    "GFX_FLIP",
    "MESH_RECV",
    "RENDER",
    "GFX_FINISH",
    "GFX_DRAWDONE",
    "GFX_FBPUSH",
    "GFX_SEMWAIT",
    # Batch E item 1 / C1 (2026-08-23/09-02) + T-b (2026-09-03): these were
    # missing here even before HEARTBEAT was appended C-side (source/platform/
    # wii/watchdog.c's MAIN_NM), so wd_main_name silently read "?" for any
    # shutdown-phase hang report -- mirrored now to match that array exactly.
    "SHUT_SRV",
    "SHUT_AUD",
    "SHUT_TEL",
    "SHUT_WPAD",
    "SHUT_WD",
    "HEARTBEAT",
]
WD_SRV_NAMES = [
    "IDLE",
    "RPC",
    "SPAWN",
    "RANDOM_TICK",
    "WORLD_TICK",
    "EVICT_NEAR",
    "EVICT_FAR",
    "NEAR_SCAN",
    "ENSURE_POP",
    "CHUNK_SEND",
    "SLOT_WAIT",
    "RPC_SEND",
    # APPENDED 2026-09-08 (server-loop wedge instrumentation). Mirrors the
    # WDS_IDLE_SAMPLE/WDS_IDLE_SLEEP/WDS_FAR_EXTRACT/WDS_FAR_POPULATE/
    # WDS_REGION_IO appended to enum wd_phase_srv (source/platform/watchdog.h).
    "IDLE_SAMPLE",
    "IDLE_SLEEP",
    "FAR_EXTRACT",
    "FAR_POPULATE",
    "REGION_IO",
    # APPENDED 2026-09-11 C-side (WDS_SAVE, WDS_SAVE_SELECT, WDS_EVICT_SELECT)
    # but NEVER mirrored here until lava-aabb-0924/H1 (2026-09-24): every
    # autosave-window dump (wd_srv=17 SAVE) decoded as "?" in the collector.
    "SAVE",
    "SAVE_SELECT",
    "EVICT_SELECT",
    # APPENDED 2026-09-24 (lava-aabb-0924/H1): honest far-select / tail /
    # epilogue phases (source/platform/watchdog.h).
    "FAR_SELECT",
    "TAIL",
    "EPILOGUE",
]
WD_WORKER_NAMES = ["IDLE", "RECV", "BUILD", "SEND"]
# Telemetry-sender stages (2026-09-08 wedge lane). Mirrors
# enum hw_telemetry_sender_stage in source/platform/hw_telemetry.h -- the wire
# carries the ORDINAL, so entries may be appended there but never reordered,
# and this table must stay index-aligned with it.
TEL_STAGE_NAMES = {
    0: "idle",
    1: "poll",
    2: "send",
    3: "accept",
    4: "close",
    5: "ring_wait",
    6: "other",
}


def phase_name(names: list[str], value: object) -> str:
    try:
        index = int(value)
    except (TypeError, ValueError):
        return "?"
    if 0 <= index < len(names):
        return names[index]
    return "?"


def release_frame_error(hb: dict, complete: bool = False) -> str | None:
    """Fail closed on missing cumulative frame evidence, not sampled FPS.

    The game admits frames to these counters only after a 10 s post-load
    settle (HW_FRAME_EVIDENCE_SETTLE_S, maintainer 2026-10-31 option 2), so
    the running max cannot see the first-frame spike after world load. The
    rules below are unchanged; the five-minute floor still holds for a
    6-minute walk (360 s - 10 s > 300 s).
    """
    fields = ("perf_frames", "perf_max_us", "perf_over_30ms",
              "perf_gpu_over_16600", "perf_gpu_over_8000", "perf_invalid",
              "perf_duration_us")
    if any(type(hb.get(k)) is not int or hb[k] < 0 for k in fields):
        return "missing or invalid per-frame evidence"
    n = hb["perf_frames"]
    # Maintainer 2026-10-31 option 2 (second ruling): no per-frame 55 FPS
    # cap. Isolated 19-21 ms frames while walking are not felt; the hard
    # per-frame limit is the 30 ms rule below, plus the GPU p90/median rules.
    if hb["perf_invalid"]:
        return "invalid frame timing"
    if hb["perf_max_us"] > 30_000:
        return "gameplay frame exceeded 30 ms"
    if hb["perf_over_30ms"]:
        return "gameplay frame exceeded 30 ms"
    if any(hb[k] > n for k in fields[2:5]):
        return "inconsistent frame counters"
    if n and (not hb["perf_max_us"] or not hb["perf_duration_us"]
              or hb["perf_duration_us"] > n * hb["perf_max_us"]):
        return "inconsistent frame duration"
    if complete:
        if n < 16500 or hb["perf_duration_us"] < 300_000_000:
            return "need at least five minutes of admitted gameplay frames"
        if hb["perf_gpu_over_16600"] * 10 > n:
            return "GPU p90 exceeds 16.6 ms"
        if hb["perf_gpu_over_8000"] * 2 > n:
            return "GPU median exceeds 8 ms"
    return None


RELEASE_FRAME_PROFILES = ("keepup_walk_6m", "far_walk_6m")


# REL-16-collector (SPEC R16): soak_idle_play grading on the PC side, the same
# rules as source/game/soak_idle_play_core.h (REL-16a CONTRACT.md) applied to
# the heartbeat tape on the device uptime clock (up_ms). Limits come ONLY from
# the approved measured baseline; a missing or zero limit fails closed. The
# first failure is sticky; a trailing partial segment is not graded.
SOAK_IP_MIN_DURATION_MS = 10800 * 1000
SOAK_IP_HB_MAX_GAP_MS = 5000          # existing heartbeat stale timeout
SOAK_IP_LIMITS = ("idle_s", "play_s", "warmup_s", "min_play_blocks",
                  "min_play_chunks", "max_drop_kb")
SOAK_IP_FIELDS = ("seq", "up_ms", "soak_process_id", "soak_world_id",
                  "moved_blocks", "chunks_loaded", "mem1_free_kb",
                  "mem2_carve_free_kb", "oom")


class SoakIdlePlayEvidence:
    def __init__(self, limits: dict | None):
        lim = limits or {}
        self.lim = {k: lim.get(k) for k in SOAK_IP_LIMITS}
        self.fail = None
        if any(type(v) is not int or v <= 0 for v in self.lim.values()):
            self.fail = "unapproved_limits"
        self.prev = self.seg_start = self.start_ms = None
        self.idle_segments = self.play_segments = self.leak_checks = 0
        self.base = None  # (mem1, carve) at the first idle end after warmup

    def _set_fail(self, cause: str) -> None:
        self.fail = self.fail or cause

    def _play(self, up_ms: int) -> bool:
        cycle = (self.lim["idle_s"] + self.lim["play_s"]) * 1000
        return (up_ms - self.start_ms) % cycle >= self.lim["idle_s"] * 1000

    def _close_segment(self) -> None:
        p, s = self.prev, self.seg_start
        if self._play(p["up_ms"]):
            self.play_segments += 1
            if (p["moved_blocks"] - s["moved_blocks"] < self.lim["min_play_blocks"]
                    or p["chunks_loaded"] - s["chunks_loaded"] < self.lim["min_play_chunks"]):
                self._set_fail("no_progress")
            return
        # Idle must be observed, not inferred from the clock: from its first to
        # its last sample (boundary intervals excluded, as for play) no
        # movement and no chunk loads, else a stuck driver passes as idle.
        if (p["moved_blocks"] != s["moved_blocks"]
                or p["chunks_loaded"] != s["chunks_loaded"]):
            self._set_fail("idle_not_idle")
            return
        self.idle_segments += 1
        if p["up_ms"] - self.start_ms < self.lim["warmup_s"] * 1000:
            return
        mem = (p["mem1_free_kb"], p["mem2_carve_free_kb"])
        if self.base is None:
            self.base = mem
            return
        self.leak_checks += 1
        if any(b > m + self.lim["max_drop_kb"] for b, m in zip(self.base, mem)):
            self._set_fail("leak")

    def note(self, event: dict) -> None:
        if self.fail:
            return
        kind = event.get("event")
        if kind in ("fault", "crash_mailbox"):
            return self._set_fail("crash")
        if kind == "watchdog_dump":
            return self._set_fail("watchdog")
        if kind != "heartbeat":
            return
        if any(type(event.get(k)) is not int for k in SOAK_IP_FIELDS):
            return self._set_fail("bad_heartbeat")
        s, p = event, self.prev
        if p is None:
            self.start_ms = s["up_ms"]
        elif (s["soak_process_id"] != p["soak_process_id"]
              or s["soak_world_id"] != p["soak_world_id"]
              or s["up_ms"] <= p["up_ms"] or s["seq"] <= p["seq"]
              or s["moved_blocks"] < p["moved_blocks"]
              or s["chunks_loaded"] < p["chunks_loaded"]):
            return self._set_fail("process_reset")
        elif s["up_ms"] - p["up_ms"] > SOAK_IP_HB_MAX_GAP_MS:
            return self._set_fail("telemetry_gap")
        if (s["oom"] or s["mem1_free_kb"] < 4096          # release-frame floors
                or s["mem2_carve_free_kb"] < 2048):
            return self._set_fail("memory_floor")
        if p is None:
            self.seg_start = s
        elif self._play(s["up_ms"]) != self._play(p["up_ms"]):
            self._close_segment()
            self.seg_start = s  # first sample inside the new phase
        self.prev = s

    def verdict(self) -> str:
        if self.fail:
            return self.fail
        if not self.idle_segments or not self.play_segments:
            return "missing_phase"
        if self.prev["up_ms"] - self.start_ms < SOAK_IP_MIN_DURATION_MS:
            return "short"
        if self.base is None or not self.leak_checks:
            return "no_memory_history"
        return "pass"


class MpPauseMenuEvidence:
    """Device snapshots plus the 1 Hz heartbeat stream for pause parity."""
    ORDER = ("pause_open", "pause_hold_forward", "pause_hold_end",
             "pause_close", "walk_after", "done")

    def __init__(self, run_id: str):
        self.run_id = run_id
        self.rows: list[dict] = []
        self.beats: list[tuple[int, int]] = []
        self.sessions: list[tuple[int, dict]] = []
        self.hold_beats = 0
        self.last_beat: int | None = None
        self.error: str | None = None
        self.done_status: str | None = None

    def note(self, event: dict) -> None:
        if self.error:
            return
        marker = event.get("marker")
        if marker == "fail":
            self.error = (f"mp_pause_menu driver failure: {event.get('reason')}"
                          if event.get("run_id") == self.run_id and
                          isinstance(event.get("reason"), str) and event["reason"]
                          else "mp_pause_menu failure missing reason or run_id")
            return
        if marker in ("site_unverified", "apex_short"):
            self.error = marker if event.get("run_id") == self.run_id else "stale-run pause outcome"
            return
        i = len(self.rows)
        if (event.get("run_id") != self.run_id or i >= len(self.ORDER)
                or marker != self.ORDER[i] or event.get("seq") != i + 1):
            self.error = f"mp_pause_menu marker order/seq invalid at {marker!r}"
            return
        for key in ("ms", "tick", "x", "y", "z"):
            if type(event.get(key)) not in (int, float):
                self.error = f"mp_pause_menu {marker}: missing {key}"
                return
        if marker in ("pause_open", "pause_close", "walk_after") and (type(event.get("up_ms")) is not int
                or event["up_ms"] < 0):
            self.error = f"mp_pause_menu {marker}: missing up_ms"
            return
        if marker == "pause_open" and event.get("screen") != "pause":
            self.error = "pause_open did not observe the pause screen"
            return
        if marker == "pause_open" and event.get("login") is not True:
            self.error = "pause_open did not follow the shared login"
            return
        if marker == "pause_close" and event.get("screen") != "ingame":
            self.error = "pause_close did not return to gameplay"
            return
        if marker == "done" and (event.get("screen") != "title" or
                                  event.get("remote") is not False):
            self.error = "mp_pause_menu did not leave the remote session"
            return
        if marker in ("pause_hold_forward", "pause_hold_end") and event.get("forward") is not True:
            self.error = f"{marker}: forward input was not sustained"
            return
        if marker != "done" and event.get("remote") is not True:
            self.error = f"{marker}: remote session absent"
            return
        self.rows.append(event)

    def heartbeat(self, event: dict) -> None:
        tick = event.get("tick")
        if type(tick) is not int:
            self.error = "mp_pause_menu heartbeat missing sequence tick"
        else:
            if self.last_beat is not None and tick <= self.last_beat:
                self.error = "mp_pause_menu heartbeat sequence invalid"
            self.last_beat = tick
            stamp = event.get("up_ms")
            if type(stamp) is not int or stamp < 0:
                self.error = "heartbeat_no_uptime"
            else:
                self.beats.append((tick, stamp))
                self.sessions.append((stamp, {k: event.get(k) for k in _MP_SESSION_FIELDS}))
            if len(self.rows) == 2:
                self.hold_beats += 1
        gap_ms = event.get("wd_hb_gap_max_ms")
        if type(gap_ms) is not int:
            self.error = "mp_pause_menu heartbeat missing device gap gauge"

    def complete_error(self) -> str | None:
        if self.error:
            return self.error
        if len(self.rows) != len(self.ORDER):
            return f"missing mp_pause_menu marker {self.ORDER[len(self.rows)]}"
        if self.done_status != "ok":
            return f"mp_pause_menu profile_done status={self.done_status!r}"
        opening, start, end, closing, walking, done = self.rows
        # Markers and beats use the same monotonic device uptime clock.
        window_start = max(0, opening["up_ms"] - 5000)
        window_end = walking["up_ms"]
        window_beats = [(seq, ms) for seq, ms in self.beats
                        if window_start <= ms <= window_end]
        minimum = max(2, (window_end - window_start) // 1000 - 1)
        if len(window_beats) < minimum:
            return ("mp_pause_menu heartbeat window empty "
                    f"({len(window_beats)} heartbeats between up_ms "
                    f"{window_start} and {window_end}, expected >= {minimum})")
        for (seq0, ms0), (seq1, ms1) in zip(window_beats, window_beats[1:]):
            gap = ms1 - ms0
            if gap > 2000:
                return (f"mp_pause_menu heartbeat seq {seq0} -> {seq1} "
                        f"up_ms {ms0} -> {ms1} gap {gap} ms > 2000 ms")
        if window_beats[0][1] > opening["up_ms"]:
            return ("mp_pause_menu heartbeat hold coverage missing "
                    f"(first pair starts at up_ms {window_beats[0][1]}, "
                    f"after pause_open up_ms {opening['up_ms']})")
        if window_beats[-1][1] < closing["up_ms"]:
            return ("mp_pause_menu heartbeat hold coverage missing "
                    f"(last pair ends at up_ms {window_beats[-1][1]}, "
                    f"before pause_close up_ms {closing['up_ms']})")
        problem = self.session_error(opening["up_ms"], walking["up_ms"])
        if problem:
            return problem
        if not (opening["ms"] <= start["ms"] and
                end["ms"] - start["ms"] >= MP_PAUSE_MENU_HOLD_MS and
                end["ms"] <= closing["ms"] <= walking["ms"] <= done["ms"]):
            return f"pause hold did not last {MP_PAUSE_MENU_HOLD_MS} ms in marker order"
        if closing["tick"] - opening["tick"] < 40:
            return "pause menu stopped client ticks (<40 advanced)"
        if opening["y"] - closing["y"] < 0.5:
            return "pause menu body did not fall >=0.5 block"
        if math.hypot(closing["x"] - opening["x"],
                      closing["z"] - opening["z"]) > 0.15:
            return "held forward moved body horizontally in pause menu"
        if math.hypot(walking["x"] - closing["x"],
                      walking["z"] - closing["z"]) < 1.0:
            return "forward after pause close moved <1 block"
        if len(self.beats) < 2:
            return "pause hold lacked continuous heartbeats"
        return None

    def session_error(self, start: int, end: int) -> str | None:
        """One live, logged-in connect generation from the menu opening to
        the post-menu walk, with received bytes advancing: a kick, timeout
        or reconnect hidden behind otherwise complete markers fails here."""
        rows = [(ms, f) for ms, f in self.sessions if start <= ms <= end]
        if not rows:
            return "mp_pause_menu no heartbeat inside the menu window"
        first = rows[0][1]
        for ms, f in rows:
            if any(type(f[k]) is not int for k in _MP_SESSION_FIELDS):
                return f"mp_pause_menu heartbeat at up_ms {ms} missing mp session fields"
            if f["mp_login"] != 1 or f["mp_end"] != 0:
                return (f"mp_pause_menu session lost at up_ms {ms} "
                        f"(mp_login {f['mp_login']}, mp_end {f['mp_end']})")
            for k in ("mp_gen", "mp_reconn"):
                if f[k] != first[k]:
                    return (f"mp_pause_menu session changed at up_ms {ms} "
                            f"({k} {first[k]} -> {f[k]})")
        if rows[-1][1]["mp_rx_kb"] <= first["mp_rx_kb"]:
            return ("mp_pause_menu no network progress across the menu window "
                    f"(mp_rx_kb {first['mp_rx_kb']} -> {rows[-1][1]['mp_rx_kb']})")
        return None

    def summary(self) -> dict:
        return {"markers": [r.get("marker") for r in self.rows],
                "hold_heartbeats": self.hold_beats,
                "error": self.complete_error()}


# Device sender counters a missing heartbeat can be attributed to, in the
# order a verdict should name them (2026-09-26, telreliable).
_GAP_COUNTERS = ("tel_fail", "tel_wto", "tel_hb_repl", "tel_drop", "tel_abd")


def heartbeat_gap(prev: dict, hb: dict) -> dict | None:
    """Name the device-side cause of a heartbeat tick gap, or None.

    Real-Wii split_play_8m (G4, 2026-09-26) lost one whole heartbeat three
    times and the first diagnosis blamed UDP fragmentation -- but staged runs
    use the TCP pull stream, and the very next heartbeat carried the answer:
    tel_wto 0->1 with tel_xmax_us=255065 (the sender's per-line deadline
    abandoned the heartbeat mid-line after one 255 ms IOS net_send). The
    counters are cumulative, so the heartbeat AFTER a gap says what the
    sender did during it. This helper only attributes; it grades nothing.

    Returns None when either side lacks an int tick or the ticks are
    consecutive (or went backwards/reset -- not a gap this can explain)."""
    if not prev or not hb:
        return None
    t0, t1 = prev.get("tick"), hb.get("tick")
    if type(t0) is not int or type(t1) is not int or t1 - t0 <= 1:
        return None
    deltas = {}
    for key in _GAP_COUNTERS:
        a, b = prev.get(key), hb.get(key)
        if type(a) is int and type(b) is int and b >= a:
            deltas[key] = b - a
        else:
            deltas[key] = None
    if deltas["tel_fail"]:
        cause = "device_send_failure"
    elif deltas["tel_wto"]:
        cause = "device_send_deadline"
    elif deltas["tel_hb_repl"]:
        cause = "device_heartbeat_superseded"
    elif deltas["tel_drop"] or deltas["tel_abd"]:
        cause = "device_ring_or_nonblock_drop"
    else:
        cause = "unattributed"
    return {"missing_ticks": list(range(t0 + 1, t1)),
            "interval_ticks": [t0, t1], "cause": cause,
            "deltas": deltas,
            "tel_xmax_us": hb.get("tel_xmax_us"),
            "tel_pmax_us": hb.get("tel_pmax_us")}


# split_dims_6m (lane B, splitdims-profile-0926): the semantic evidence for
# independent split dimensions. The device (main.c split_dims_fill) publishes
# what each split VIEW shows on every heartbeat: dim_v0 (player 0), dim_v1
# (the guest; -128 = no guest view), views_live, dims_active and the guest
# walk's portal step dims_phase. A PASS needs ALL of:
#   * every heartbeat carries the fields, typed, and dims_active agrees with
#     them (a device that claims divergence it does not show is rejected);
#   * each view shows the dimension the SERVER simulates its body in
#     (srv_dim_host / srv_dim_guest, Fable F1) on every measured heartbeat
#     with both views live, except while the guest is crossing ("hold") --
#     a view bound to a Nether payload while the body is simulated in the
#     Overworld can never count as divergence;
#   * the host's view shows the Overworld on every measured heartbeat;
#   * both views live on every measured heartbeat, except while the guest is
#     crossing (dims_phase "hold": a dimension change re-admits the body);
#   * the views really diverged -- guest view in the Nether (-1) while the
#     host view is in the Overworld -- for at least DIMS_MIN_ACTIVE_HB
#     measured heartbeats and still on the last one (no bounce back);
#   * the guest walked at least DIMS_MIN_NETHER_WALK blocks while diverged;
#   * same-dimension separation is RECORDED, never graded: the maintainer's
#     2026-09-24 ruling makes 256 blocks the free, unclamped envelope, so a
#     run beyond it is noted as outside the certified envelope, not failed;
#     cross-dimension rows are independent worlds and are not measured.
# A run whose views never diverged is dims_not_active: FAIL, never PASS --
# the state of every run until guest portal travel lands (A2).
#
# Host switch FIRST (splitdims-hostswitch-0927, lane A's A2-T): before the
# guest crosses, the host goes to the Nether and back (main.c
# split_dims_host_step, host_phase wait/away/back/done/timeout). A PASS also
# needs ALL of:
#   * the host fields typed on every heartbeat (host_phase, host_commits,
#     slot1_cols, rel_sent, rel_rx, rel_slot, wslot_v0, wslot_v1), and the
#     client never applied more releases than the server sent;
#   * at least two host transfer COMMITs (out and back);
#   * each view bound to its OWN client world slot whenever the two views
#     show different dimensions (a view drawing the other player's world is
#     not two worlds);
#   * after the second COMMIT, the vacated world (slot 1) drains to zero
#     resident columns, and a release is applied at or after that drain --
#     never before it;
#   * the host view may leave the Overworld only while the trip is on
#     (host_phase wait/away), and the server may lead the host view by at
#     most DIMS_MAX_HOST_TRANSIT_HB heartbeats across a transfer;
#   * no missing heartbeat (split_dims_heartbeat_missing, Collector).
DIMS_NETHER, DIMS_OVERWORLD, DIMS_NO_VIEW = -1, 0, -128
DIMS_MIN_ACTIVE_HB = 60
DIMS_MIN_NETHER_WALK = 16.0
DIMS_MAX_SEPARATION = 256.0
# SERVER_TRANSFER_RESIDENT_COLUMN_CAP, server_transfer.h
SPLIT_COLUMN_CAP = 256
SPLIT_MAX_SLABS = 8
SPLIT_CARVE_FLOOR_KB = 2246   # both split_dims carve floors (Astra CC-R2 item 4)
DIMS_PHASES = ("clear", "host_trip", "pane_wait", "seek", "hold", "across",
               "timeout")
DIMS_HOST_PHASES = ("wait", "away", "back", "done", "timeout")
DIMS_HOST_TRIP = ("wait", "away")
DIMS_MIN_HOST_COMMITS = 2
DIMS_MAX_HOST_TRANSIT_HB = 5
_DIMS_HOST_INTS = ("host_commits", "slot1_cols", "rel_sent", "rel_rx",
                   "rel_slot", "wslot_v0", "wslot_v1")
_DIMS_BUDGET_INTS = ("budget_resident", "budget_live_backings", "budget_reserved",
                     "budget_prepeak", "budget_peak", "budget_waits", "budget_phase",
                     "transfer_request", "guest_commits", "transfer_actor", "transfer_state",
                     "column_pool_used", "column_pool_peak", "column_pool_slabs",
                     "column_pool_overflow", "column_pool_carve_fail")


class SplitDimsEvidence:
    def __init__(self):
        self.error = None
        self.error_class = None
        self.rows = 0
        self.measured = 0
        self.active = 0
        self.first_active_tick = None
        self.last_measured_active = None
        self.nether_walk = 0.0
        self.last_active_pos = None
        self.max_same_dim_separation = 0.0
        self.last_phase = None
        self.views_lost = 0
        # host switch
        self.max_host_commits = 0
        self.back_tick = None      # first heartbeat with >= 2 host COMMITs
        self.drain_tick = None     # first zero-column slot 1 after back_tick
        self.release_tick = None   # first applied release after drain_tick
        self.prev_rel_rx = None
        self.host_transit_run = 0
        self.last_host_phase = None
        self.budget_enforced = False
        self.budget_peak = 0
        self.prebudget_peak = 0
        self.budget_waits = 0
        self.guest_commits = 0
        self.column_pool_peak = 0
        self.column_pool_slabs = 0
        self.budget_min_carve_kb = None
        self.budget_wii = None
        self.carve_largest_min = None      # min over run, valid snapshots only
        self.carve_largest_seen = False
        self.pool_grow_count = 0

    def note_pool_grow(self, ev):
        self.pool_grow_count += 1
        if ev.get("success") is False:
            self._set("column_pool_grow failed: " + json.dumps(ev, sort_keys=True),
                      "split_dims_column_pool_grow_failed")

    def _note_carve_contiguity(self, hb):
        valid = hb.get("mem2_carve_largest_valid")
        size = hb.get("mem2_carve_largest_allocatable_bytes")
        if valid is True and type(size) is int and size >= 0:
            self.carve_largest_seen = True
            self.carve_largest_min = (size if self.carve_largest_min is None
                                      else min(size, self.carve_largest_min))

    def _note_budget(self, hb):
        if (type(hb.get("budget_valid")) is not bool or type(hb.get("budget_wii")) is not bool
                or any(type(hb.get(k)) is not int or hb[k] < 0
                       for k in _DIMS_BUDGET_INTS)
                or any(type(hb.get(k)) is not bool for k in
                       ("transfer_held", "transfer_source_draining", "transfer_restoring"))):
            self._set("missing or mistyped transfer budget evidence", "split_dims_budget_missing")
            return
        if not hb["budget_valid"]:
            self._set("transfer budget snapshot was not coherent", "split_dims_budget_invalid")
            return
        phase = hb["budget_phase"]
        total = hb["budget_live_backings"] + hb["budget_reserved"]
        if (phase not in (0, 1, 2) or (self.budget_enforced and phase != 2)
                or hb["budget_resident"] > hb["budget_live_backings"]
                or hb["budget_peak"] < self.budget_peak
                or hb["budget_prepeak"] < self.prebudget_peak
                or (phase == 1 and hb["budget_prepeak"] < total)
                or (self.budget_wii is not None and hb["budget_wii"] != self.budget_wii)):
            self._set("inconsistent or reset transfer budget evidence", "split_dims_budget_invalid")
            return
        self.budget_wii = hb["budget_wii"]
        self._note_carve_contiguity(hb)
        self.prebudget_peak = max(self.prebudget_peak, hb["budget_prepeak"])
        self.budget_waits = max(self.budget_waits, hb["budget_waits"])
        self.guest_commits = max(self.guest_commits, hb["guest_commits"])
        self.column_pool_peak = max(self.column_pool_peak, hb["column_pool_peak"])
        self.column_pool_slabs = max(self.column_pool_slabs, hb["column_pool_slabs"])
        if phase == 2:
            self.budget_enforced = True
            self.budget_peak = hb["budget_peak"]
            if total > SPLIT_COLUMN_CAP or self.budget_peak > SPLIT_COLUMN_CAP or self.budget_peak < total:
                self._set(f"transfer budget exceeded/inconsistent: live+reserved={total}, "
                          f"peak={self.budget_peak}", "split_dims_budget_exceeded")
            # On PC all pool fields are zero. Real Wii pool high-water counters
            # retain peaks between heartbeats, including client-held backing.
            if (self.column_pool_peak > SPLIT_COLUMN_CAP or self.column_pool_slabs > SPLIT_MAX_SLABS
                    or hb["column_pool_used"] > hb["column_pool_peak"]
                    or hb["column_pool_overflow"] or hb["column_pool_carve_fail"]):
                self._set(f"physical column pool exceeded the {SPLIT_MAX_SLABS}-slab / {SPLIT_COLUMN_CAP}-column transfer budget",
                          "split_dims_column_pool_exceeded")
            if self.budget_wii:
                if hb["column_pool_slabs"] < 4:
                    self._set("Wii budget evidence lacks its physical column pool",
                              "split_dims_budget_missing")
                carve = hb.get("mem2_carve_free_kb")
                if type(carve) is not int or carve < 0:
                    self._set("Wii budget evidence lacks carve headroom",
                              "split_dims_budget_missing")
                else:
                    self.budget_min_carve_kb = (carve if self.budget_min_carve_kb is None
                                              else min(carve, self.budget_min_carve_kb))
                    if carve < SPLIT_CARVE_FLOOR_KB:
                        self._set(f"transfer consumed the required {SPLIT_CARVE_FLOOR_KB} KiB carve reserve",
                                  "split_dims_budget_headroom")
        elif hb["host_commits"] or hb["guest_commits"]:
            self._set("actor committed before transfer budget enforcement",
                      "split_dims_budget_unenforced")

    def _set(self, message, failure_class):
        if self.error is None:
            self.error, self.error_class = message, failure_class

    def note(self, hb: dict) -> None:
        self.rows += 1
        v0, v1, live = hb.get("dim_v0"), hb.get("dim_v1"), hb.get("views_live")
        active, phase = hb.get("dims_active"), hb.get("dims_phase")
        s0, s1 = hb.get("srv_dim_host"), hb.get("srv_dim_guest")
        if (type(v0) is not int or type(v1) is not int or live not in (1, 2)
                or type(live) is not int or type(active) is not bool
                or phase not in DIMS_PHASES or type(s0) is not int
                or type(s1) is not int):
            self._set(f"heartbeat tick {hb.get('tick')} lacks typed split "
                      "dimension fields", "split_dims_fields_missing")
            return
        host_phase = hb.get("host_phase")
        if (host_phase not in DIMS_HOST_PHASES
                or any(type(hb.get(k)) is not int for k in _DIMS_HOST_INTS)):
            self._set(f"heartbeat tick {hb.get('tick')} lacks typed host-switch "
                      "fields", "split_dims_fields_missing")
            return
        if hb["rel_rx"] > hb["rel_sent"]:
            self._set(f"heartbeat tick {hb.get('tick')}: client applied "
                      f"{hb['rel_rx']} world releases, server sent "
                      f"{hb['rel_sent']}", "split_dims_inconsistent")
            return
        self._note_host_switch(hb)
        self._note_budget(hb)
        if active != (live == 2 and v1 != DIMS_NO_VIEW and v1 != v0):
            self._set(f"heartbeat tick {hb.get('tick')}: dims_active={active} "
                      f"disagrees with v0={v0} v1={v1} views_live={live}",
                      "split_dims_inconsistent")
            return
        self.last_phase = phase
        if hb.get("anchor_measuring") is not True:
            return
        self.measured += 1
        on_trip = host_phase in DIMS_HOST_TRIP
        if v0 != DIMS_OVERWORLD and not on_trip:
            self._set(f"host view left the Overworld (dim {v0}) at tick "
                      f"{hb.get('tick')} (host phase {host_phase})",
                      "split_dims_host_left_overworld")
        if live != 2 and phase != "hold":
            self.views_lost += 1
            self._set(f"guest view not live at tick {hb.get('tick')} "
                      f"(phase {phase})", "split_dims_view_lost")
        if live == 2 and phase != "hold" and hb["wslot_v1"] < 0:
            self._set(f"live guest has no bound world at tick {hb.get('tick')}",
                      "split_dims_guest_unbound")
        if live == 2 and v0 != v1 and (hb["wslot_v0"] < 0 or hb["wslot_v1"] < 0
                                       or hb["wslot_v0"] == hb["wslot_v1"]):
            self._set(f"views show dimensions ({v0}, {v1}) from world slots "
                      f"({hb['wslot_v0']}, {hb['wslot_v1']}) at tick "
                      f"{hb.get('tick')}: not one world per view",
                      "split_dims_views_share_world")
        # Across a host transfer the server commits before the client's view
        # follows: a short lead is tolerated while the trip is on.
        host_transit = on_trip and v0 != s0
        self.host_transit_run = self.host_transit_run + 1 if host_transit else 0
        if self.host_transit_run > DIMS_MAX_HOST_TRANSIT_HB:
            self._set(f"host view dim {v0} lagged server body dim {s0} for "
                      f"{self.host_transit_run} heartbeats at tick "
                      f"{hb.get('tick')}", "split_dims_view_body_disagree")
            return
        if live == 2 and phase != "hold" and ((v0 != s0 and not host_transit)
                                              or v1 != s1):
            self._set(f"view/body dimension disagree at tick {hb.get('tick')}: "
                      f"views ({v0}, {v1}) vs server bodies ({s0}, {s1})",
                      "split_dims_view_body_disagree")
            return
        diverged = active and v0 == DIMS_OVERWORLD and v1 == DIMS_NETHER
        self.last_measured_active = diverged
        gx, gz = hb.get("gx"), hb.get("gz")
        pos = ((float(gx), float(gz)) if type(gx) in (int, float)
               and type(gz) in (int, float) else None)
        if diverged:
            self.active += 1
            if self.first_active_tick is None:
                self.first_active_tick = hb.get("tick")
            if pos is not None and self.last_active_pos is not None:
                self.nether_walk += math.hypot(pos[0] - self.last_active_pos[0],
                                               pos[1] - self.last_active_pos[1])
            self.last_active_pos = pos
        else:
            self.last_active_pos = None
        if live == 2 and v0 == v1 and pos is not None:
            x, z = hb.get("x"), hb.get("z")
            if type(x) in (int, float) and type(z) in (int, float):
                sep = math.hypot(pos[0] - x, pos[1] - z)
                self.max_same_dim_separation = max(self.max_same_dim_separation, sep)

    def _note_host_switch(self, hb: dict) -> None:
        tick = hb.get("tick")
        self.last_host_phase = hb["host_phase"]
        self.max_host_commits = max(self.max_host_commits, hb["host_commits"])
        if self.back_tick is None and hb["host_commits"] >= DIMS_MIN_HOST_COMMITS:
            self.back_tick = tick
        if (self.back_tick is not None and self.drain_tick is None
                and hb["slot1_cols"] == 0):
            self.drain_tick = tick
        rel = hb["rel_rx"]
        if self.prev_rel_rx is not None and rel > self.prev_rel_rx:
            if self.drain_tick is None:
                self._set(f"a world release was applied at tick {tick} before "
                          f"the vacated world drained (slot1 "
                          f"{hb['slot1_cols']} columns, host commits "
                          f"{hb['host_commits']})",
                          "split_dims_release_before_drain")
            elif self.release_tick is None:
                self.release_tick = tick
        self.prev_rel_rx = rel

    def verdict(self):
        """(message, failure_class) of the first unmet requirement, or None."""
        # A device whose views contradict its own fields or the server's
        # bodies is named before "never active": its divergence count is not
        # evidence either way.
        if self.error is not None and self.error_class in (
                "split_dims_fields_missing", "split_dims_inconsistent",
                "split_dims_view_body_disagree", "split_dims_guest_unbound"):
            return self.error, self.error_class
        if self.error is not None and self.error_class.startswith((
                "split_dims_budget_", "split_dims_column_pool_")):
            return self.error, self.error_class
        if self.active == 0:
            return (f"split dimensions never became active: the two views never "
                    f"showed different dimensions (last guest portal step "
                    f"{self.last_phase})", "dims_not_active")
        if self.error is not None:
            return self.error, self.error_class
        if self.max_host_commits < DIMS_MIN_HOST_COMMITS:
            return (f"host transferred {self.max_host_commits} time(s); need "
                    f"{DIMS_MIN_HOST_COMMITS} (Nether and back; last host "
                    f"phase {self.last_host_phase})",
                    "split_dims_host_commits_short")
        if not self.budget_enforced or self.guest_commits < 1:
            return ("no enforced-budget guest COMMIT was recorded",
                    "split_dims_guest_commit_missing")
        if self.drain_tick is None:
            return (f"the vacated world never drained to 0 resident columns "
                    f"after the host came back (tick {self.back_tick})",
                    "split_dims_no_drain")
        if self.release_tick is None:
            return (f"no world release was applied after the drain (tick "
                    f"{self.drain_tick})", "split_dims_release_missing")
        if self.active < DIMS_MIN_ACTIVE_HB:
            return (f"views diverged on only {self.active} measured heartbeats; "
                    f"need {DIMS_MIN_ACTIVE_HB}", "split_dims_active_too_short")
        if not self.last_measured_active:
            return ("views no longer diverged at the end of the measured window",
                    "split_dims_not_sustained")
        if self.nether_walk < DIMS_MIN_NETHER_WALK:
            return (f"guest walked {self.nether_walk:.1f} blocks in the Nether; "
                    f"need {DIMS_MIN_NETHER_WALK:.0f}", "split_dims_guest_not_walking")
        return None

    def summary(self) -> dict:
        verdict = self.verdict()
        return {"state": ("dims_not_active" if verdict and verdict[1] == "dims_not_active"
                          else "dims_active" if self.active else "unknown"),
                "difficulty": "difficulty forced to Peaceful by profile",
                "heartbeats": self.rows, "measured": self.measured,
                "active_measured": self.active,
                "first_active_tick": self.first_active_tick,
                "nether_walk_blocks": round(self.nether_walk, 2),
                "host_commits": self.max_host_commits,
                "guest_commits": self.guest_commits,
                "budget_enforced": self.budget_enforced,
                "budget_peak": self.budget_peak,
                "prebudget_peak": self.prebudget_peak,
                "budget_waits": self.budget_waits,
                "column_pool_peak": self.column_pool_peak,
                "column_pool_slabs": self.column_pool_slabs,
                "budget_min_carve_kb": self.budget_min_carve_kb,
                "carve_largest_allocatable_min_bytes": (
                    self.carve_largest_min if self.carve_largest_seen
                    else "unavailable"),
                "column_pool_grow_events": self.pool_grow_count,
                "budget_platform": "wii" if self.budget_wii else "pc" if self.budget_wii is False else "unknown",
                "hardware_budget": ("graded" if self.budget_wii and self.budget_enforced
                                    else "not_graded"),
                "host_back_tick": self.back_tick,
                "drain_tick": self.drain_tick,
                "release_tick": self.release_tick,
                "last_host_phase": self.last_host_phase,
                "max_same_dim_separation": round(self.max_same_dim_separation, 2),
                # Recorded, never graded (maintainer ruling 2026-09-24: free
                # separation, no clamp).
                "separation_note": (f"same-dimension separation reached "
                                    f"{self.max_same_dim_separation:.1f} blocks, "
                                    f"outside the {DIMS_MAX_SEPARATION:.0f}-block "
                                    "certified envelope"
                                    if self.max_same_dim_separation
                                    > DIMS_MAX_SEPARATION else None),
                "last_phase": self.last_phase,
                "error": verdict[0] if verdict else None,
                "failure_class": verdict[1] if verdict else None}


# Frozen CONTRACT-v1.2, vehicle-smoke-1004. These schemas are deliberately
# closed: owner-specific records may not borrow fields from another owner.
VS_KINDS = ("boat", "pig", "cart")
VS_A = dict(slot="slot", generation="g64", wire_id="u32", input_epoch="u32")
VS_W = dict(slot="slot", generation="positive", dimension="dimension")
VS_R = dict(id="positive", kind=VS_KINDS, world=VS_W)
VS_S = dict(actor=VS_A, world=VS_W, pos=["f", 3], health="i32", ready="b",
            confirmed="b", mount_count="u32", riding_boat="b", riding_cart="b",
            steer=["f", 2], inv_hash="u32", inv_used="u32")
VS_C = dict(present="b", actor=VS_A, bound=VS_W, pos=["f", 3], ready="b",
            mount_count="u32", ride_pending="b", ride_target="u32",
            ride_input_epoch="u32", ride_request_seq="u32",
            ride_desired_occupied="b", ride_send_dirty="b",
            ride_exit_lift_pending="b", ride_exit_repin_eligible="b",
            steer=["f", 2], inv_hash="u32", inv_used="u32")
VS_C0 = dict(present="b", generation="g64", pending_ticket="g64")
VS_V = dict(target=VS_R, alive="b", pos=["f", 3], occupied="b", rider=VS_A,
            ride_epoch="u32", control_seq="u32", positive_steps="u32",
            negative_steps="u32", pending_throttle="i32", saddled="b", steer=["f", 2])
VS_VC = {
    "boat": dict(target=VS_R, alive="b", pos=["f", 3], occupied="b",
                 occupant_id="u32", steer=["f", 2]),
    "pig": dict(target=VS_R, alive="b", pos=["f", 3], pig_ridden="b",
                pig_rider_id="u32", saddled="b"),
    "cart": dict(target=VS_R, alive="b", pos=["f", 3], occupied="b",
                 occupant_id="u32", cart_type="u32", ride_epoch="u32",
                 control_ready="b", control_dirty="b", control_seq="u32",
                 positive_steps="u32", negative_steps="u32", pending_throttle="i32",
                 replay_throttle="i32"),
}
VS_Q = dict(ticket="g64", generation="g64", world=VS_W)
VS_ACTION = ("saddle", "mount", "dismount", "switch", "guest_ui", "guest_use",
             "guest_cart_intent", "guest_cart_control")
VS_ENVELOPE = dict(schema="u32", run_id="id", build_id="id", profile="id",
                   kind=VS_KINDS, seq="positive",
                   phase=("p0", "p1", "p2", "p3", "p4", "p5", "done", "failed"),
                   op_id="u32", ms="u32", server_update="u32", active_update="u32",
                   event="event", origin=("production", "fixture", "synthetic"),
                   ok="b", reason="text")


class VehicleSmokeError(ValueError):
    pass


def vehicle_json_loads(line: str) -> dict:
    """Used by the live parser AND replay; duplicates must survive neither."""
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise VehicleSmokeError("vehicle_schema: duplicate JSON key " + key)
            result[key] = value
        return result
    def constant(value):
        raise VehicleSmokeError("vehicle_schema: nonfinite JSON constant " + value)
    return json.loads(line, object_pairs_hook=pairs, parse_constant=constant)


class VehicleSmokeEvidence:
    """Functional gate only. Synthetic rows never qualify Wii calibration.

    Capture times from different owners are not ordered by wire arrival. New
    authority serials are unique; comparisons use integers, never G64 strings.
    """
    CEILINGS = (100000, 60000, 100000, 60000, 130000, 20000)
    SWITCHES = {"boat": ("boat", "pig", "cart", "boat"),
                "pig": ("pig", "boat", "cart", "pig"),
                "cart": ("cart", "boat", "pig", "cart")}

    def __init__(self, run_id: str, kind: str = "", build_id: str = "",
                 transfer_mode: str = ""):
        self.run_id, self.kind, self.build_id = run_id, kind, build_id
        self.transfer_mode = transfer_mode
        self.error = None
        self.rows = []
        self.seen = {}
        self.serials = set()
        self.serial_records = {}
        self.phase = -1
        self.phase_open = False
        self.phase_start = 0
        self.run_start = None
        self.run_ok = None
        self.done_count = 0
        self.retries = 0
        self.claims = {}
        self.switch_pairs = []
        self.operations = set()
        self.edge_sequences = {}
        # Existing GUI policy (main.c/screen_death.c), overridden from the
        # frozen effective configuration when collecting a hardware packet.
        self.death_hold_respawn_ms = 750
        self.main_ms = 0

    @staticmethod
    def require(condition, reason):
        if not condition:
            raise VehicleSmokeError(reason)

    @classmethod
    def validate(cls, value, spec, path):
        fail = lambda: cls.require(False, "vehicle_schema: " + path)
        if isinstance(spec, dict):
            if type(value) is not dict or set(value) != set(spec):
                missing = set(spec) - set(value) if type(value) is dict else set(spec)
                extra = set(value) - set(spec) if type(value) is dict else set()
                cls.require(False, f"vehicle_schema: {path} missing={sorted(missing)} unknown={sorted(extra)}")
            for key, sub in spec.items():
                cls.validate(value[key], sub, path + "." + key)
        elif isinstance(spec, tuple):
            if type(value) is not str or value not in spec:
                fail()
        elif isinstance(spec, list):
            if type(value) is not list or len(value) != spec[1]:
                fail()
            for i, sub in enumerate(value):
                cls.validate(sub, spec[0], f"{path}[{i}]")
        elif spec in ("u32", "positive", "slot", "dimension", "i32"):
            lo, hi = {"u32": (0, 0xffffffff), "positive": (1, 0xffffffff),
                      "slot": (0, 1), "dimension": (-1, 0),
                      "i32": (-0x80000000, 0x7fffffff)}[spec]
            if type(value) is not int or not lo <= value <= hi:
                fail()
        elif spec == "b":
            if type(value) is not bool:
                fail()
        elif spec == "f":
            try:
                finite = type(value) in (int, float) and math.isfinite(value)
            except OverflowError:
                finite = False
            if not finite:
                fail()
        elif spec == "g64":
            if (type(value) is not str or not re.fullmatch(r"0|[1-9][0-9]*", value)
                    or len(value) > 20 or int(value) > 0xffffffffffffffff):
                fail()
        elif spec in ("id", "text", "event"):
            pattern = {"id": r"[A-Za-z0-9_.+-]{1,63}",
                       "text": r"[A-Za-z0-9_.:-]{0,48}",
                       "event": r"vehicle_smoke\.[a-z_]+"}[spec]
            if type(value) is not str or not re.fullmatch(pattern, value):
                fail()
        else:
            raise AssertionError(spec)

    @staticmethod
    def payload(row):
        name = row.get("event", "").removeprefix("vehicle_smoke.")
        stage, site = row.get("stage"), row.get("site")
        schemas = {
            "run_start": dict(seed="g64", difficulty="u32", game_mode=("survival",),
                              superflat="b", split="b", world_payloads="b", world="id",
                              transfer_mode=("contact", "fixture_request"), deadline_ms="u32"),
            "phase_begin": dict(phase_index="u32", pending_markers="u32", open_ops="u32"),
            "phase_end": dict(phase_index="u32", pending_markers="u32", open_ops="u32"),
            "rig_ready": dict(fixture_hash="u32", targets=[VS_R, 3], host=VS_S,
                              guest=VS_S, water_ok="b", support_ok="b", rails_ok="b",
                              ray_ok="b", saddle_consumed="b", pig_ai_updates="u32"),
            "action_attempt": dict(action=VS_ACTION, channel=("host_buttons", "guest_frame", "owned_rpc"),
                                   source=VS_A, target=VS_R,
                                   button=("IB_ACTION1", "IB_ACTION2", "IB_JUMP", "none"),
                                   edge_seq="u32", held_ms="u32", request_seq="u32",
                                   desired_occupied="b", reach_m="f", los="b", camera_hit="u32"),
            "ui_action": dict(action=("save_exit", "respawn", "guest_leave", "guest_join", "accept", "confirm", "gameplay"),
                              player="slot", screen=("ingame", "pause", "death", "title", "loading", "none"),
                              button=("IB_HOME", "IB_GUI_UP", "IB_GUI_DOWN", "IB_GUI_CLICK", "none"),
                              held_ms="u32", edge_seq="u32", delivered="b", input_epoch="u32"),
            "dismount": dict(host=VS_S, old=VS_V, serial="g64"),
            "switch": dict(**{"from": VS_KINDS, "to": VS_KINDS}, host=VS_S, old=VS_V,
                           new=VS_V, release_serial="g64", mount_serial="g64"),
            "checkpoint": dict(stage=("begin", "durable"), checkpoint_id="positive", host=VS_S,
                               guest=VS_S, vehicle=VS_V, pose_bits=["u32", 3], fixture_hash="u32",
                               save_ok="b", level_ok="b", chunks_ok="b", regions_synced="b"),
            "world_reset": dict(world_reset_count="u32", local_entity="u32", checkpoint_id="positive",
                                source_world=VS_W, world_loaded="b"),
            "title": dict(world_loaded="b", world_reset_count="u32", checkpoint_id="positive",
                          screen=("title",), autoload_consumed="b"),
            "transfer_begin": dict(transfer_id="positive", actor=VS_A, src=VS_W, dst=VS_W,
                                   vehicle=VS_V, mode=("contact", "fixture_request"), state=("requested",), serial="g64"),
            "transfer_release": dict(transfer_id="positive", actor=VS_A, src=VS_W, dst=VS_W,
                                     host=VS_S, old=VS_V, release_serial="g64"),
            "transfer_commit": dict(transfer_id="positive", actor=VS_A, src=VS_W, dst=VS_W,
                                    host=VS_S, source_vehicle=VS_V, destination_mount_count="u32",
                                    commit_serial="g64", transition_epoch="u32", state=("committed",)),
            "pending": dict(stage=("armed", "blocked", "retained"),
                            cause=("synthetic_admission", "global_pause", "transfer", "loading", "status_backpressure"),
                            tuple=VS_Q, result=("none", "installed"), paused="b", loading="b",
                            transfer_open="b", refusal_armed="b", pending="b", serial="g64"),
            "pause": dict(tuple=VS_Q, paused="b", loading="b", pending="b", refusal_armed="b", serial="g64"),
            "resume": dict(tuple=VS_Q, paused="b", loading="b", pending="b", refusal_armed="b", serial="g64"),
            "admission_clear": dict(tuple=VS_Q, refusal_armed="b", pending="b", transfer_open="b",
                                    transfer_id="positive", loading="b", serial="g64"),
            "barrier_release": dict(tuple=VS_Q, transfer_id="positive", pending="b", paused="b",
                                    transfer_open="b", loading="b", host_ack_pending="b", serial="g64"),
            "installed": dict(tuple=VS_Q, installed_count="u32", result=("installed",), pending="b",
                              serial="g64", paused="b", loading="b", transfer_open="b"),
            "run_ok": dict(completed_phases="u32", pending_markers="u32", open_ops="u32",
                           overflow_count="u32", hook_mask="u32"),
            "fail": dict(code="text", failed_phase=VS_ENVELOPE["phase"], overflow_count="u32", hook_mask="u32"),
        }
        if name == "action_result":
            base = dict(action=VS_ACTION, source=VS_A, target=VS_R,
                        site=("ingress", "authority", "client"),
                        status=("sent", "applied", "refused", "client_noop"), request_seq="u32", ride_epoch="u32")
            extra = {"authority": dict(serial="g64", host=VS_S, vehicle=VS_V),
                     "client": dict(host=VS_C, vehicle="vc"),
                     "ingress": dict(rpc=("ENTITY_ATTACK", "ENTITY_USE", "MINECART_RIDE_INTENT", "MINECART_CONTROL"))}
            return {**base, **extra.get(site, {})}
        if name == "ride_sample":
            return dict(stage=("server", "client"), target=VS_R, window_id="positive", samples="u32",
                        eligible_updates="u32", tick_calls="u32", missing_calls="u32", duplicate_calls="u32",
                        displacement_m="f", host=VS_S if stage == "server" else VS_C,
                        vehicle=VS_V if stage == "server" else "vc", seat_error_max_m="f",
                        follow_count="u32", request_seq="u32")
        if name == "isolation":
            server = row.get("owner") == "server"
            return dict(stage=("before", "after"), owner=("server", "client"), attempt_op="positive",
                        host=VS_S if server else VS_C, guest=VS_S if server else "c0",
                        vehicle=VS_V if server else "vc", guest_pose_delta_max_m="f",
                        guest_follow_count="u32", unexpected_attach_count="u32",
                        **({} if server else dict(unexpected_route_count="u32")))
        if name == "reload":
            extra = {
                "disk": dict(world=VS_W, pose_bits=["u32", 3], disk_reopen="b", player_read_ok="b", fallback_used="b", override_used="b"),
                "inventory": dict(world=VS_W, read_ok="b", inv_hash="u32", inv_used="u32"),
                "entities": dict(host=VS_S, guest=VS_S, fixture_hash="u32", saddle="b", targets=[VS_R, 3], ambiguous_matches="u32"),
                "client_ready": dict(host=VS_C, guest=VS_C, input_ack_epoch="positive"),
            }
            return dict(stage=tuple(extra), world_name="id", reload_count="u32", checkpoint_id="positive", **extra.get(stage, {}))
        if name == "death":
            return dict(stage=("server", "ui"), **(dict(before_host=VS_S, host=VS_S, vehicle=VS_V,
                        cause="text", deaths_before="u32", deaths_after="u32") if stage == "server" else dict(host=VS_C, screen=("death",))))
        if name == "respawn":
            return dict(stage=("released", "requested", "ack", "fresh", "client_ready"),
                        old_input_epoch="u32", new_input_epoch="u32", respawn_epoch="g64",
                        **(dict(host=VS_C, ready="b") if stage == "client_ready" else
                           dict(host=VS_S, old=VS_V, serial="g64", health="i32", ack="b")))
        if name == "transfer_ack":
            return dict(stage=("server", "client"), transfer_id="positive", actor=VS_A, dst=VS_W,
                        input_ack_epoch="positive", ready="b",
                        **(dict(source_vehicle=VS_V, destination_mount_count="u32") if stage == "server" else dict(host=VS_C)))
        if name == "gameplay_ready":
            return dict(stage=("server", "client"), tuple=VS_Q, guest=VS_S if stage == "server" else VS_C,
                        confirmed="b", ready_count="u32", installed_count="u32")
        if name not in schemas:
            raise VehicleSmokeError("vehicle_event: unknown event " + name)
        return schemas[name]

    @classmethod
    def validate_record(cls, row):
        cls.require(type(row) is dict, "vehicle_schema: record must be object")
        cls.validate(row.get("event"), "event", "record.event")
        spec = {**VS_ENVELOPE, **cls.payload(row)}
        for key, sub in list(spec.items()):
            if sub == "vc":
                value = row.get(key)
                cls.require(type(value) is dict and type(value.get("target")) is dict, "vehicle_schema: " + key)
                kind = value["target"].get("kind")
                cls.require(kind in VS_VC, "vehicle_schema: client vehicle kind")
                spec[key] = VS_VC[kind]
            elif sub == "c0":
                value = row.get(key)
                cls.require(type(value) is dict, "vehicle_schema: " + key)
                spec[key] = VS_C0 if value.get("present") is False else VS_C
        cls.validate(row, spec, "record")
        # Identity defaults have meaning; they cannot masquerade as a guest.
        def objects(value):
            if isinstance(value, dict):
                if set(value) == set(VS_A):
                    cls.require((value["slot"] == 0 and value["generation"] == "0" and value["wire_id"] == 0)
                                or (value["slot"] == 1 and int(value["generation"]) > 0), "vehicle_actor_identity")
                    # The local guest's current owned PLAYER_POS protocol has
                    # no input handoff epoch. Its native zero is independent
                    # of both its lifetime and the host's epoch allocator.
                    cls.require(value["slot"] == 0 or value["input_epoch"] == 0,
                                "vehicle_guest_input_epoch")
                if set(value) == set(VS_V):
                    k = value["target"]["kind"]
                    cls.require(k == "cart" or all(value[f] == 0 for f in
                                ("ride_epoch", "control_seq", "positive_steps", "negative_steps", "pending_throttle")), "vehicle_noncart_fields")
                    cls.require(k == "pig" or not value["saddled"], "vehicle_nonpig_saddle")
                if set(value) == set(VS_C):
                    cls.require(value["present"], "vehicle_client_absent")
                if set(value) == set(VS_C0):
                    cls.require(not value["present"] and int(value["generation"]) > 0
                                and int(value["pending_ticket"]) > 0, "vehicle_pending_body")
                if set(value) == set(VS_VC["cart"]):
                    cls.require(value["cart_type"] == 0, "vehicle_cart_type")
                for child in value.values():
                    objects(child)
            elif isinstance(value, list):
                for child in value:
                    objects(child)
        objects(row)

    @staticmethod
    def actor_equal(a, b):
        return all(a[f] == b[f] for f in ("slot", "generation", "wire_id"))

    @staticmethod
    def released(v):
        return (v["alive"] and not v["occupied"] and v["steer"] == [0, 0]
                and (v["target"]["kind"] != "cart" or
                     (v["ride_epoch"] == 0 and v["pending_throttle"] == 0)))

    @classmethod
    def server_mounted(cls, s, v):
        k = v["target"]["kind"]
        return (s["ready"] and v["alive"] and v["occupied"]
                and cls.actor_equal(s["actor"], v["rider"])
                and v["target"]["world"] == s["world"] and s["mount_count"] == 1
                and s["riding_boat"] == (k == "boat") and s["riding_cart"] == (k == "cart")
                and (k != "pig" or v["saddled"]))

    @classmethod
    def server_detached(cls, s, v=None):
        return (s["mount_count"] == 0 and not s["riding_boat"] and not s["riding_cart"]
                and s["steer"] == [0, 0] and (v is None or cls.released(v)))

    @staticmethod
    def prediction_clear(c):
        return not any(c[f] for f in ("ride_pending", "ride_send_dirty",
                                     "ride_exit_lift_pending", "ride_exit_repin_eligible"))

    @staticmethod
    def client_released(v):
        k = v["target"]["kind"]
        return v["alive"] and ({
            "boat": lambda: not v["occupied"] and v["steer"] == [0, 0],
            "pig": lambda: not v["pig_ridden"],
            "cart": lambda: not v["occupied"] and v["ride_epoch"] == 0
                and not v["control_ready"] and not v["control_dirty"]
                and v["pending_throttle"] == v["replay_throttle"] == 0,
        }[k]())

    @classmethod
    def client_detached(cls, c, v=None):
        return (c.get("present") and c["ready"] and c["mount_count"] == 0
                and c["steer"] == [0, 0] and cls.prediction_clear(c)
                and (v is None or cls.client_released(v)))

    @classmethod
    def client_mounted(cls, c, v, request_seq=0, ride_epoch=0):
        k = v["target"]["kind"]
        common = (c["present"] and c["ready"] and v["alive"]
                  and v["target"]["world"] == c["bound"] and c["mount_count"] == 1
                  and cls.prediction_clear(c))
        if k == "pig":
            return common and v["pig_ridden"] and v["pig_rider_id"] == c["actor"]["wire_id"] and v["saddled"]
        common = common and v["occupied"] and v["occupant_id"] == c["actor"]["wire_id"]
        if k == "boat":
            return common
        return (common and v["cart_type"] == 0 and v["control_ready"] and v["ride_epoch"] > 0
                and v["ride_epoch"] == ride_epoch and request_seq > 0
                and c["ride_target"] == v["target"]["id"] and c["ride_desired_occupied"]
                and c["ride_input_epoch"] == c["actor"]["input_epoch"]
                and c["ride_request_seq"] == request_seq)

    def _note(self, row):
        self.validate_record(row)
        self.require(len(json.dumps(row, separators=(",", ":"), ensure_ascii=False).encode("utf-8")) + 1 <= 2303,
                     "vehicle_line_oversize")
        self.require(row["schema"] == 1 and row["profile"] == VEHICLE_SMOKE_PROFILE, "vehicle_schema_version")
        self.require(row["run_id"] == self.run_id, "vehicle_stale_run")
        self.require(row["kind"] == self.kind, "vehicle_kind_mismatch")
        self.require(bool(self.build_id) and row["build_id"] == self.build_id, "vehicle_build_mismatch")
        seq = row["seq"]
        if seq in self.seen:
            self.require(row == self.seen[seq], "vehicle_conflicting_duplicate")
            self.retries += 1
            return
        self.require(seq == len(self.seen) + 1, "vehicle_seq_gap")
        self.require(not self.done_count, "vehicle_event_after_done")
        self.require(not self.run_ok, "vehicle_event_after_run_ok")
        self.require(row["ms"] <= 480000, "vehicle_deadline")
        self.require(all(row[f] < 0xffffffff for f in ("seq", "server_update", "active_update")), "vehicle_counter_wrap")
        self.require(row.get("overflow_count", 0) == 0, "vehicle_overflow")
        self.require(row.get("hook_mask", 0) == 0, "vehicle_hook_armed")
        if row["event"] == "vehicle_smoke.fail":
            self.require(False, "vehicle_producer_fail: " + row["code"] + " phase=" + row["failed_phase"])
        self.require(row["ok"], "vehicle_terminal_false")
        name = row["event"].split(".")[1]
        if name == "ui_action" and row["player"] == 1:
            self.require(row["input_epoch"] == 0, "vehicle_guest_input_epoch")
        if name in ("run_start", "phase_begin", "phase_end", "action_attempt", "ui_action", "title", "run_ok", "fail"):
            self.require(row["ms"] >= self.main_ms, "vehicle_main_time_order")
            self.main_ms = row["ms"]
        expected_origin = ("fixture" if name in ("run_start", "rig_ready")
                           or (name == "death" and row["stage"] == "server")
                           or (name == "transfer_begin" and row["mode"] == "fixture_request")
                           else "synthetic" if name in ("action_attempt", "ui_action", "admission_clear")
                           or (name == "pending" and row["cause"] == "synthetic_admission")
                           else "production")
        self.require(row["origin"] == expected_origin, "vehicle_origin")
        self.require(name != "fail", "vehicle_producer_fail")
        if name == "isolation":
            self.require(row["unexpected_attach_count"] == row["guest_follow_count"] == 0
                         and 0 <= row["guest_pose_delta_max_m"] <= .05, "vehicle_isolation_violation")
            self.require(row.get("unexpected_route_count", 0) == 0, "vehicle_unexpected_route")
            self.require(row["stage"] != "before" or row["guest_pose_delta_max_m"] == 0,
                         "vehicle_isolation_reset")
        if name == "ride_sample":
            # Missing + duplicate calls can cancel in the total. Both per-update
            # census counters remain mandatory, independently of follow_count.
            self.require(row["tick_calls"] == row["eligible_updates"]
                         and row["missing_calls"] == row["duplicate_calls"] == 0,
                         "vehicle_ride_tick_census")
        if name == "run_start":
            self.require(not self.rows and row["phase"] == "p0" and row["op_id"] == 0, "vehicle_run_start_order")
            self.require(row["seed"] == "12345" and row["difficulty"] == 0 and row["superflat"]
                         and row["split"] and row["world_payloads"] and row["world"] == "cw_vehicle_rig"
                         and row["deadline_ms"] == 480000, "vehicle_fixture_config")
            self.require(not self.transfer_mode or row["transfer_mode"] == self.transfer_mode, "vehicle_transfer_mode")
            self.transfer_mode = row["transfer_mode"]
            self.run_start = row
        elif name == "phase_begin":
            i = row["phase_index"]
            self.require(self.run_start and not self.phase_open and i == self.phase + 1 and i < 6
                         and row["phase"] == f"p{i}" and row["op_id"] == 0, "vehicle_phase_order")
            self.require(row["pending_markers"] == row["open_ops"] == 0, "vehicle_phase_undrained")
            self.phase, self.phase_open, self.phase_start = i, True, row["ms"]
        elif name == "phase_end":
            self.require(self.phase_open and row["phase_index"] == self.phase
                         and row["phase"] == f"p{self.phase}" and row["op_id"] == 0, "vehicle_phase_order")
            self.require(row["pending_markers"] == row["open_ops"] == 0, "vehicle_phase_undrained")
            self.require(0 <= row["ms"] - self.phase_start <= self.CEILINGS[self.phase], "vehicle_phase_deadline")
            self.check_phase(self.phase)
            self.phase_open = False
        elif name == "run_ok":
            self.require(self.phase == 5 and not self.phase_open and row["phase"] == "done"
                         and row["op_id"] == 0, "vehicle_run_ok_order")
            self.require(row["completed_phases"] == 6 and row["pending_markers"] == row["open_ops"] ==
                         row["overflow_count"] == row["hook_mask"] == 0, "vehicle_run_undrained")
            self.run_ok = row
        else:
            self.require(self.phase_open and row["phase"] == f"p{self.phase}", "vehicle_phase_late_event")
            self.require(row["op_id"] > 0, "vehicle_operation_missing")
            if row["op_id"] not in self.operations:
                self.require(not self.operations or row["op_id"] > max(self.operations), "vehicle_operation_order")
                self.operations.add(row["op_id"])
            if name in ("action_attempt", "ui_action") and row["button"] != "none":
                actor = row["source"] if name == "action_attempt" else {"slot": row["player"], "generation": "ui"}
                key = (actor["slot"], actor["generation"])
                self.require(row["edge_seq"] > self.edge_sequences.get(key, 0), "vehicle_edge_sequence")
                self.edge_sequences[key] = row["edge_seq"]
        # References may repeat. New transaction serials must never alias.
        keys = [k for k in ("serial", "commit_serial") if k in row]
        if name == "transfer_release":
            keys.append("release_serial")
        for key in keys:
            value = int(row[key])
            self.require(value > 0, "vehicle_serial_duplicate")
            if value in self.serials:
                previous = self.serial_records[value]
                # Two receipts can reference one explicit release transaction;
                # they may not allocate two transitions with the same serial.
                names = {previous["event"], row["event"]}
                result = row if name == "action_result" else previous
                released = previous if name == "action_result" else row
                self.require(names == {"vehicle_smoke.dismount", "vehicle_smoke.action_result"}
                             and result.get("site") == "authority" and result.get("action") == "dismount"
                             and previous["op_id"] == row["op_id"]
                             and released["old"]["target"] == result["target"], "vehicle_serial_duplicate")
            else:
                self.serial_records[value] = row
            self.serials.add(value)
        self.seen[seq] = row
        self.rows.append(row)

    def note(self, row):
        if self.error:
            return
        try:
            self._note(row)
        except (VehicleSmokeError, KeyError, TypeError, ValueError) as exc:
            self.error = str(exc)

    def note_profile_done(self, status):
        if self.error:
            return
        self.done_count += 1
        if self.done_count != 1:
            self.error = "vehicle_extra_completion"
        elif status != "ok":
            self.error = "vehicle_profile_done_status"
        elif self.run_ok is None:
            self.error = "vehicle_profile_done_before_drain"

    def rows_for(self, name, phase=None, **fields):
        return [r for r in self.rows if r["event"] == "vehicle_smoke." + name
                and (phase is None or r["phase"] == f"p{phase}")
                and all(r.get(k) == v for k, v in fields.items())]

    def one(self, name, phase=None, **fields):
        rows = self.rows_for(name, phase, **fields)
        self.require(len(rows) == 1, "vehicle_missing_or_extra: " + name + " " + str(fields))
        return rows[0]

    def ui(self, action, phase, player, screen=None):
        r = self.one("ui_action", phase, action=action, player=player)
        self.require(r["delivered"] and (r["input_epoch"] > 0 if player == 0 else r["input_epoch"] == 0),
                     "vehicle_ui_delivery: " + action)
        if screen is not None:
            self.require(r["screen"] == screen and r["button"] != "none"
                         and r["edge_seq"] > 0, "vehicle_ui_input: " + action)
            if action == "respawn":
                self.require(r["held_ms"] >= self.death_hold_respawn_ms, "vehicle_ui_hold: respawn")
        return r

    def operation(self, op, phase, action=None):
        a = self.one("action_attempt", phase, op_id=op)
        if action is not None:
            self.require(a["action"] == action, "vehicle_action_kind")
        self.require(0 <= a["reach_m"] <= 6 and a["los"] and a["camera_hit"] == a["target"]["id"], "vehicle_action_reach")
        if a["channel"] != "owned_rpc":
            self.require(a["button"] != "none" and a["edge_seq"] > 0, "vehicle_action_button")
        results = self.rows_for("action_result", phase, op_id=op)
        self.require(bool(results), "vehicle_action_result_missing")
        sites = set()
        for r in results:
            self.require(r["site"] not in sites, "vehicle_action_result_duplicate")
            sites.add(r["site"])
            self.require(r["action"] == a["action"] and r["source"] == a["source"]
                         and r["target"] == a["target"] and r["request_seq"] == a["request_seq"]
                         and r["seq"] > a["seq"], "vehicle_action_correlation")
            if r["site"] != "ingress":
                self.require(r["vehicle"]["target"] == a["target"], "vehicle_action_target")
        return a, results

    def positive(self, op, phase, action="mount"):
        a, results = self.operation(op, phase, action)
        self.require(a["source"]["slot"] == 0 and a["channel"] == "host_buttons"
                     and a["button"] == "IB_ACTION2" and a["desired_occupied"], "vehicle_positive_input")
        s = next((r for r in results if r["site"] == "authority"), None)
        c = next((r for r in results if r["site"] == "client"), None)
        self.require(s is not None and c is not None and s["status"] == c["status"] == "applied", "vehicle_positive_results")
        self.require(self.server_mounted(s["host"], s["vehicle"]), "vehicle_server_mount")
        self.require(self.client_mounted(c["host"], c["vehicle"], a["request_seq"], s["ride_epoch"]), "vehicle_client_mount")
        self.require(s["vehicle"]["ride_epoch"] == s["ride_epoch"] and c["ride_epoch"] == s["ride_epoch"], "vehicle_mount_epoch")
        self.require(self.actor_equal(s["host"]["actor"], a["source"])
                     and s["host"]["actor"]["input_epoch"] == a["source"]["input_epoch"]
                     and c["host"]["actor"] == a["source"], "vehicle_mount_actor")
        return a, s, c

    def remount(self, phase, after):
        attempts = [a for a in self.rows_for("action_attempt", phase, action="mount")
                    if a["target"]["kind"] == self.kind and a["seq"] > after]
        self.require(len(attempts) == 1, "vehicle_boundary_remount")
        return self.positive(attempts[0]["op_id"], phase)

    @staticmethod
    def inventory(s):
        return s["inv_hash"], s["inv_used"]

    @staticmethod
    def relation(v):
        return {k: v[k] for k in v if k != "pos"}

    def isolation(self, op, phase, first, last, negative=False, lifecycle=False):
        for owner in ("server", "client"):
            before = self.one("isolation", phase, owner=owner, stage="before", attempt_op=op)
            after = self.one("isolation", phase, owner=owner, stage="after", attempt_op=op)
            self.require(before["op_id"] == after["op_id"] == op and before["seq"] < first
                         and after["seq"] > last, "vehicle_isolation_bracket")
            for r in (before, after):
                g = r["guest"]
                self.require(r["guest_follow_count"] == r["unexpected_attach_count"] == 0
                             and 0 <= r["guest_pose_delta_max_m"] <= .05, "vehicle_isolation_violation")
                if owner == "client":
                    self.require(r["unexpected_route_count"] == 0, "vehicle_unexpected_route")
                    if g["present"]:
                        detached = self.client_detached(g)
                        if phase == 4 and r["stage"] == "before" and not negative:
                            detached = g["mount_count"] == 0 and g["steer"] == [0, 0] and self.prediction_clear(g)
                        self.require(detached and g["actor"]["slot"] == 1, "vehicle_guest_client_attached")
                    else:
                        self.require(phase == 4 and bool(self.rows_for("ui_action", 4, action="guest_leave")), "vehicle_c0_outside_readmission")
                else:
                    self.require(self.server_detached(g) and (g["ready"] or (phase == 4 and r["stage"] == "before" and not negative)) and g["actor"]["slot"] == 1,
                                 "vehicle_guest_server_attached")
            b, a = before["guest"], after["guest"]
            readmission = (phase == 4 and any(before["seq"] < r["seq"] < after["seq"]
                          for r in self.rows_for("ui_action", 4, action="guest_leave")))
            if readmission:
                ready = self.one("gameplay_ready", 4, stage="server" if owner == "server" else "client")
                self.require(a["actor"] == ready["guest"]["actor"]
                             and self.inventory(a) == self.inventory(ready["guest"]), "vehicle_isolation_readmission")
            if not lifecycle and not readmission and (owner == "server" or (b["present"] and a["present"])):
                worldkey = "world" if owner == "server" else "bound"
                self.require(b["actor"] == a["actor"] and b[worldkey] == a[worldkey]
                             and self.inventory(b) == self.inventory(a), "vehicle_guest_identity_changed")
                self.require(after["guest_pose_delta_max_m"] + 1e-6 >= math.dist(b["pos"], a["pos"]),
                             "vehicle_isolation_accumulation")
            if negative:
                self.require(after["active_update"] - before["active_update"] >= 10, "vehicle_negative_window")
                self.require(before["vehicle"]["target"] == after["vehicle"]["target"]
                             and self.relation(before["vehicle"]) == self.relation(after["vehicle"]), "vehicle_negative_host_relation")
                h1, h2 = before["host"], after["host"]
                self.require(h1["actor"] == h2["actor"] and h1["mount_count"] == h2["mount_count"]
                             and h1["steer"] == h2["steer"] == [0, 0], "vehicle_negative_host_changed")
                if owner == "server":
                    self.require(h1["world"] == h2["world"] and h1["riding_boat"] == h2["riding_boat"]
                                 and h1["riding_cart"] == h2["riding_cart"], "vehicle_negative_host_changed")
                else:
                    self.require(h1["bound"] == h2["bound"] and self.prediction_clear(h1)
                                 and self.prediction_clear(h2), "vehicle_negative_host_prediction")

    def ride(self, s, c, minimum=100):
        self.require(s["target"] == c["target"] == s["vehicle"]["target"] == c["vehicle"]["target"]
                     and s["window_id"] == c["window_id"] and s["request_seq"] == c["request_seq"], "vehicle_ride_correlation")
        self.require(self.server_mounted(s["host"], s["vehicle"]), "vehicle_ride_server_mount")
        self.require(self.client_mounted(c["host"], c["vehicle"], s["request_seq"], s["vehicle"]["ride_epoch"]), "vehicle_ride_client_mount")
        self.require(s["host"]["actor"] == c["host"]["actor"], "vehicle_ride_actor_epoch")
        for r in (s, c):
            self.require(r["eligible_updates"] >= minimum and r["tick_calls"] == r["eligible_updates"]
                         and r["missing_calls"] == r["duplicate_calls"] == 0, "vehicle_ride_tick_census")
        self.require(c["samples"] >= minimum and c["follow_count"] >= minimum
                     and 0 <= c["seat_error_max_m"] <= .001, "vehicle_ride_follow")
        if minimum == 100:
            self.require(s["displacement_m"] >= .25 and c["displacement_m"] >= .25, "vehicle_ride_motion")
        if s["target"]["kind"] == "cart":
            mounts = [r for r in self.rows_for("action_result", site="authority", status="applied")
                      if r["target"] == s["target"] and r["request_seq"] == s["request_seq"]
                      and r["action"] in ("mount", "switch") and r["seq"] < s["seq"]]
            self.require(bool(mounts), "vehicle_ride_cart_request")
            start = mounts[-1]
            self.require(start["ride_epoch"] == s["vehicle"]["ride_epoch"] == c["vehicle"]["ride_epoch"] > 0,
                         "vehicle_ride_cart_epoch")
            if minimum == 100:
                for r in (s, c):
                    self.require(r["vehicle"]["control_seq"] > start["vehicle"]["control_seq"]
                                 and r["vehicle"]["positive_steps"] > start["vehicle"]["positive_steps"], "vehicle_ride_cart_control")

    def check_phase(self, phase):
        # Every emitted operation/bracket is accounted for, including extras
        # that would otherwise hide outside the decisive positive examples.
        for a in self.rows_for("action_attempt", phase):
            self.operation(a["op_id"], phase)
            if a["source"]["slot"] == 0 and a["action"] in ("mount", "switch"):
                self.positive(a["op_id"], phase, a["action"])
        for r in self.rows_for("action_result", phase):
            self.one("action_attempt", phase, op_id=r["op_id"])
        if phase == 1:
            windows = {}
            for r in self.rows_for("ride_sample", phase):
                key = (r["window_id"], json.dumps(r["target"], sort_keys=True))
                windows.setdefault(key, []).append(r)
            for rows in windows.values():
                self.require(len(rows) == 2 and {r["stage"] for r in rows} == {"server", "client"},
                             "vehicle_ride_summary_count")
                s = next(r for r in rows if r["stage"] == "server")
                c = next(r for r in rows if r["stage"] == "client")
                self.require(s["op_id"] == c["op_id"], "vehicle_ride_correlation")
                self.ride(s, c, 100 if s["target"]["kind"] == self.kind else 1)
        for owner in ("server", "client"):
            opened = None
            for r in self.rows_for("isolation", phase, owner=owner):
                if r["stage"] == "before":
                    self.require(opened is None, "vehicle_isolation_overlap")
                    opened = r["attempt_op"]
                else:
                    self.require(opened == r["attempt_op"], "vehicle_isolation_unclosed")
                    opened = None
            self.require(opened is None, "vehicle_isolation_unclosed")
        if phase == 0:
            rig = self.one("rig_ready", 0)
            self.require(all(rig[k] for k in ("water_ok", "support_ok", "rails_ok", "ray_ok", "saddle_consumed"))
                         and rig["pig_ai_updates"] > 0, "vehicle_rig_geometry")
            self.require([r["kind"] for r in rig["targets"]] == list(VS_KINDS)
                         and len({r["id"] for r in rig["targets"]}) == 3, "vehicle_rig_targets")
            h, g = rig["host"], rig["guest"]
            self.require(h["actor"]["slot"] == 0 and g["actor"]["slot"] == 1
                         and h["ready"] and g["ready"] and g["confirmed"]
                         and h["actor"]["input_epoch"] > 0 and h["world"] == g["world"]
                         and all(r["world"] == h["world"] for r in rig["targets"]), "vehicle_rig_readiness")
            self.require(h["inv_used"] > 0 and g["inv_used"] > 0 and h["inv_hash"] != g["inv_hash"], "vehicle_rig_inventory")
            self.claims["fixture"] = True
        elif phase == 1:
            primary = [r for r in self.rows_for("ride_sample", 1, stage="server") if r["target"]["kind"] == self.kind]
            self.require(bool(primary), "vehicle_ride_missing")
            for s in primary:
                c = self.one("ride_sample", 1, stage="client", window_id=s["window_id"], target=s["target"])
                self.ride(s, c)
            explicit = [r for r in self.rows_for("action_attempt", 1, action="dismount")
                        if r["target"]["kind"] == self.kind]
            self.require(len(explicit) == 1, "vehicle_explicit_dismount")
            d = self.one("dismount", 1, op_id=explicit[0]["op_id"])
            a, rs = self.operation(d["op_id"], 1, "dismount")
            self.require(a["target"]["kind"] == self.kind and a["source"]["slot"] == 0
                         and a["channel"] == "host_buttons" and not a["desired_occupied"]
                         and a["button"] == ("IB_JUMP" if self.kind == "cart" else "IB_ACTION2"), "vehicle_dismount_input")
            self.require(d["old"]["target"] == a["target"] and self.server_detached(d["host"], d["old"]), "vehicle_dismount_server")
            sr = [r for r in rs if r["site"] == "authority"]
            self.require(len(sr) == 1 and sr[0]["status"] == "applied"
                         and self.server_detached(sr[0]["host"], sr[0]["vehicle"]), "vehicle_dismount_result")
            sc = [r for r in rs if r["site"] == "client"]
            self.require(len(sc) == 1 and sc[0]["status"] == "applied"
                         and self.client_detached(sc[0]["host"], sc[0]["vehicle"]), "vehicle_dismount_client")
            remounts = [r for r in self.rows_for("action_attempt", 1, action="mount")
                        if r["target"]["kind"] == self.kind and r["seq"] > sc[0]["seq"]]
            self.require(bool(remounts), "vehicle_dismount_remount")
            self.positive(remounts[0]["op_id"], 1)
            switches = self.rows_for("switch", 1)
            sequence = self.SWITCHES[self.kind]
            self.require([(r["from"], r["to"]) for r in switches] == list(zip(sequence, sequence[1:])), "vehicle_switch_coverage")
            for switch in switches:
                a, s, c = self.positive(switch["op_id"], 1, "switch")
                release = int(switch["release_serial"])
                self.require(switch["old"]["target"]["kind"] == switch["from"]
                             and switch["new"]["target"] == a["target"] and a["target"]["kind"] == switch["to"]
                             and self.released(switch["old"]) and self.server_mounted(switch["host"], switch["new"])
                             and release < int(switch["mount_serial"]), "vehicle_switch_release")
                # Mount result and switch are two observations of the SAME transition.
                self.require(int(s["serial"]) == int(switch["mount_serial"]), "vehicle_switch_mount_serial")
                releases = self.rows_for("dismount", 1, op_id=switch["op_id"])
                self.require(len(releases) == 1 and int(releases[0]["serial"]) == release
                             and releases[0]["old"]["target"] == switch["old"]["target"]
                             and self.server_detached(releases[0]["host"], releases[0]["old"]), "vehicle_switch_release_serial")
                witness = releases[0]
                self.require(a["seq"] < witness["seq"] < min(s["seq"], switch["seq"]),
                             "vehicle_switch_release_order")
                self.require(witness["host"]["actor"] == a["source"]
                             and witness["host"]["world"] == a["target"]["world"]
                             and witness["old"]["target"]["world"] == witness["host"]["world"],
                             "vehicle_switch_release_identity")
                prior_mounts = [r for r in self.rows_for("action_result", 1, site="authority", status="applied")
                                if r["action"] in ("mount", "switch") and r["seq"] < a["seq"]]
                self.require(bool(prior_mounts) and prior_mounts[-1]["target"] == witness["old"]["target"]
                             and prior_mounts[-1]["host"]["actor"] == a["source"],
                             "vehicle_switch_old_relation")
                follow = [r for r in self.rows_for("ride_sample", 1, stage="client", target=a["target"])
                          if r["seq"] > c["seq"] and r["follow_count"] >= 1]
                self.require(bool(follow), "vehicle_switch_follow")
                for r in follow:
                    self.require(self.client_mounted(r["host"], r["vehicle"], a["request_seq"], s["ride_epoch"]), "vehicle_switch_client_census")
            self.switch_pairs = [list(pair) for pair in zip(sequence, sequence[1:])]
            self.check_negatives()
            self.claims.update(ride=True, dismount=True, switch=True, p2_isolation=True)
        elif phase == 2:
            self.check_reload()
            self.claims["save_reload"] = True
        elif phase == 3:
            self.check_respawn()
            self.claims["death_respawn"] = True
        elif phase == 4:
            self.check_transfer()
            self.claims.update(transfer=True, progress=True)
        elif phase == 5:
            tb = self.one("transfer_begin", 4)
            server_final = self.one("ride_sample", 5, stage="server")
            client_final = self.one("ride_sample", 5, stage="client")
            self.require(server_final["host"]["actor"] == client_final["host"]["actor"],
                         "vehicle_final_actor_epoch")
            self.require(all(server_final[f] == client_final[f] for f in ("op_id", "window_id", "request_seq")),
                         "vehicle_ride_correlation")
            for owner, stage in (("server", "server"), ("client", "client")):
                final = self.one("ride_sample", 5, stage=stage)
                self.require(final["vehicle"]["target"] == tb["vehicle"]["target"], "vehicle_final_source")
                detached = (self.server_detached(final["host"], final["vehicle"]) if owner == "server"
                            else self.client_detached(final["host"], final["vehicle"]))
                worldkey = "world" if owner == "server" else "bound"
                self.require(detached and final["host"][worldkey] == tb["dst"]
                             and final["eligible_updates"] > 0 and final["tick_calls"] == final["eligible_updates"]
                             and final["missing_calls"] == final["duplicate_calls"] == 0, "vehicle_final_detached")
            self.claims["final_detached"] = True
        transitions = sorted(self.serial_records.values(),
                             key=lambda r: int(r.get("serial", r.get("commit_serial", r.get("release_serial")))))
        for a, b in zip(transitions, transitions[1:]):
            self.require(a["ms"] <= b["ms"] and a["server_update"] <= b["server_update"]
                         and a["active_update"] <= b["active_update"], "vehicle_authority_serial_order")

    def check_negatives(self):
        coverage = set()
        for a in [r for r in self.rows_for("action_attempt", 1) if r["source"]["slot"] == 1]:
            a, results = self.operation(a["op_id"], 1)
            k = a["target"]["kind"]
            self.require(a["action"] in ("guest_ui", "guest_use", "guest_cart_intent", "guest_cart_control"), "vehicle_negative_action")
            before = self.one("isolation", 1, owner="server", stage="before", attempt_op=a["op_id"])
            client = self.one("isolation", 1, owner="client", stage="before", attempt_op=a["op_id"])
            self.require(a["source"] == before["guest"]["actor"] == client["guest"]["actor"]
                         and before["guest"]["world"] == client["guest"]["bound"] == a["target"]["world"]
                         and before["vehicle"]["target"] == client["vehicle"]["target"] == a["target"], "vehicle_negative_source")
            occupied = before["vehicle"]["occupied"]
            key = (k, occupied, a["action"])
            self.require(key not in coverage, "vehicle_negative_duplicate")
            coverage.add(key)
            if a["action"] == "guest_ui":
                self.require(a["channel"] == "guest_frame" and a["button"] == "IB_ACTION2", "vehicle_negative_guest_frame")
                required = "client" if k == "cart" else "authority"
                r = next((r for r in results if r["site"] == required), None)
                self.require(r is not None and r["status"] == ("client_noop" if k == "cart" else "refused"), "vehicle_negative_refusal")
            else:
                self.require(a["channel"] == "owned_rpc" and a["button"] == "none", "vehicle_negative_rpc")
                site = "ingress" if a["action"] in ("guest_cart_intent", "guest_cart_control") else "authority"
                r = next((r for r in results if r["site"] == site), None)
                self.require(r is not None and r["status"] == "refused", "vehicle_negative_refusal")
                rpc = {"guest_use": "ENTITY_USE", "guest_cart_intent": "MINECART_RIDE_INTENT", "guest_cart_control": "MINECART_CONTROL"}[a["action"]]
                ingress = next((r for r in results if r["site"] == "ingress"), None)
                self.require(ingress is not None and ingress["rpc"] == rpc, "vehicle_negative_ingress")
            self.require(all(r["status"] in ("refused", "client_noop", "sent") for r in results), "vehicle_negative_applied")
            self.isolation(a["op_id"], 1, a["seq"], max(r["seq"] for r in results), negative=True)
            if occupied:
                self.require(self.server_mounted(before["host"], before["vehicle"]), "vehicle_negative_occupied_control")
            else:
                self.require(self.released(before["vehicle"]), "vehicle_negative_empty_control")
                positives = [p for p in self.rows_for("action_attempt", 1)
                             if p["action"] in ("mount", "switch") and p["target"] == a["target"]
                             and p["source"]["slot"] == 0]
                self.require(bool(positives), "vehicle_negative_positive_missing")
                self.positive(positives[0]["op_id"], 1, positives[0]["action"])
        expected = {(k, state, action) for k in VS_KINDS for state in (False, True)
                    for action in (("guest_ui", "guest_use", "guest_cart_intent", "guest_cart_control")
                                   if k == "cart" else ("guest_ui", "guest_use"))}
        self.require(coverage == expected, "vehicle_negative_coverage")

    def check_reload(self):
        ui = self.ui("save_exit", 2, 0, "pause")
        begin, durable = (self.one("checkpoint", 2, stage=stage) for stage in ("begin", "durable"))
        reset, title = self.one("world_reset", 2), self.one("title", 2)
        disk, inventory, entities, client = (self.one("reload", 2, stage=stage)
                                            for stage in ("disk", "inventory", "entities", "client_ready"))
        ordered = (ui, begin, durable, reset, title, disk, inventory, entities, client)
        self.require(all(a["seq"] < b["seq"] for a, b in zip(ordered, ordered[1:])), "vehicle_reload_order")
        self.require(all(a["seq"] > client["seq"] for a in self.rows_for("action_attempt", 2)), "vehicle_reload_action_before_comparison")
        cp = begin["checkpoint_id"]
        self.require(all(r["checkpoint_id"] == cp for r in ordered[1:]), "vehicle_checkpoint_identity")
        self.require(begin["vehicle"]["target"]["kind"] == self.kind
                     and self.server_mounted(begin["host"], begin["vehicle"])
                     and self.server_mounted(durable["host"], durable["vehicle"])
                     and all(begin["vehicle"][f] == durable["vehicle"][f]
                             for f in ("target", "occupied", "rider", "ride_epoch")), "vehicle_checkpoint_mount")
        flags = ("save_ok", "level_ok", "chunks_ok", "regions_synced")
        self.require(not any(begin[f] for f in flags) and all(durable[f] for f in flags), "vehicle_checkpoint_durable")
        self.require(begin["pose_bits"] == durable["pose_bits"] == disk["pose_bits"]
                     and disk["disk_reopen"] and disk["player_read_ok"] and not disk["fallback_used"]
                     and not disk["override_used"] and inventory["read_ok"], "vehicle_reload_disk_pose")
        for r in (begin, durable):
            try:
                bits = [struct.unpack("<I", struct.pack("<f", float(p)))[0] for p in r["host"]["pos"]]
            except (OverflowError, struct.error):
                bits = None
            self.require(bits == r["pose_bits"], "vehicle_checkpoint_pose_bits")
        self.require(reset["world_reset_count"] == title["world_reset_count"] == 1
                     and reset["local_entity"] == 0 and not reset["world_loaded"]
                     and not title["world_loaded"] and not title["autoload_consumed"]
                     and reset["source_world"] == begin["host"]["world"], "vehicle_reload_reset")
        self.require(all(r["world_name"] == "cw_vehicle_rig" and r["reload_count"] == 1
                         for r in (disk, inventory, entities, client)), "vehicle_reload_count")
        oldw, neww = begin["host"]["world"], disk["world"]
        self.require(neww != oldw and neww["generation"] > oldw["generation"]
                     and inventory["world"] == neww and entities["host"]["world"] == entities["guest"]["world"] == neww
                     and client["host"]["bound"] == client["guest"]["bound"] == neww, "vehicle_reload_world")
        self.require(self.inventory(begin["host"]) == self.inventory(durable["host"]) ==
                     (inventory["inv_hash"], inventory["inv_used"]) == self.inventory(entities["host"]) == self.inventory(client["host"])
                     and self.inventory(begin["guest"]) == self.inventory(durable["guest"]) ==
                     self.inventory(entities["guest"]) == self.inventory(client["guest"])
                     and begin["host"]["inv_used"] > 0 and begin["guest"]["inv_used"] > 0, "vehicle_reload_inventory")
        self.require(begin["fixture_hash"] == durable["fixture_hash"] == entities["fixture_hash"] == self.one("rig_ready", 0)["fixture_hash"]
                     and entities["saddle"] and entities["ambiguous_matches"] == 0
                     and [r["kind"] for r in entities["targets"]] == list(VS_KINDS)
                     and len({r["id"] for r in entities["targets"]}) == 3
                     and all(r["world"] == neww for r in entities["targets"]), "vehicle_reload_fixture")
        self.require(all(self.server_detached(entities[p]) and entities[p]["ready"] for p in ("host", "guest"))
                     and all(self.client_detached(client[p]) for p in ("host", "guest")), "vehicle_reload_detached")
        self.require(client["input_ack_epoch"] == client["host"]["actor"]["input_epoch"] > begin["host"]["actor"]["input_epoch"]
                     and entities["host"]["actor"] == client["host"]["actor"]
                     and self.actor_equal(begin["host"]["actor"], client["host"]["actor"]), "vehicle_reload_ack")
        self.require(entities["guest"]["actor"] == client["guest"]["actor"], "vehicle_reload_guest_identity")
        _, mount, settled = self.remount(2, client["seq"])
        self.require(mount["target"] in entities["targets"], "vehicle_reload_remount_target")
        self.isolation(ui["op_id"], 2, ui["seq"], settled["seq"], lifecycle=True)

    def check_respawn(self):
        death, ui = self.one("death", 3, stage="server"), self.one("death", 3, stage="ui")
        inputrow = self.ui("respawn", 3, 0, "death")
        self.require(inputrow["ms"] - ui["ms"] >= max(400, inputrow["held_ms"]), "vehicle_ui_grace: respawn")
        stages = [self.one("respawn", 3, stage=stage) for stage in ("released", "requested", "ack", "fresh", "client_ready")]
        released, requested, ack, fresh, client = stages
        ordered = [death, ui, inputrow] + stages
        self.require(all(a["seq"] < b["seq"] for a, b in zip(ordered, ordered[1:])), "vehicle_respawn_order")
        self.require(self.server_mounted(death["before_host"], death["vehicle"])
                     and death["vehicle"]["target"]["kind"] == self.kind and death["host"]["health"] <= 0
                     and death["before_host"]["health"] > 0
                     and death["before_host"]["actor"] == death["host"]["actor"] == ui["host"]["actor"]
                     and death["before_host"]["world"] == death["host"]["world"] == ui["host"]["bound"]
                     and death["deaths_after"] == death["deaths_before"] + 1 and bool(death["cause"]), "vehicle_death_commit")
        oldepoch = death["host"]["actor"]["input_epoch"]
        newepoch = fresh["new_input_epoch"]
        self.require(oldepoch > 0 and inputrow["input_epoch"] == oldepoch and newepoch == oldepoch
                     and all(r["old_input_epoch"] == oldepoch for r in stages)
                     and all(r["new_input_epoch"] == r["host"]["actor"]["input_epoch"] == oldepoch for r in stages),
                     "vehicle_respawn_epoch")
        epoch = requested["respawn_epoch"]
        self.require(released["respawn_epoch"] == "0" and int(epoch) > 0
                     and all(r["respawn_epoch"] == epoch for r in (ack, fresh, client)),
                     "vehicle_respawn_handshake_epoch")
        self.require(all(r["op_id"] == inputrow["op_id"] == death["op_id"] for r in stages),
                     "vehicle_respawn_operation")
        self.require(int(released["serial"]) < int(requested["serial"]) < int(ack["serial"]) < int(fresh["serial"]), "vehicle_respawn_serial_order")
        for r in (released, requested, ack, fresh):
            self.require(r["old"]["target"] == death["vehicle"]["target"]
                         and self.server_detached(r["host"], r["old"]), "vehicle_respawn_release")
        self.require(fresh["health"] == fresh["host"]["health"] == 160
                     and ack["ack"] and not any(r["ack"] for r in (released, requested, fresh))
                     and client["ready"] and self.client_detached(client["host"])
                     and client["host"]["actor"]["input_epoch"] == newepoch, "vehicle_respawn_ack")
        self.require(all(r["health"] == r["host"]["health"] for r in stages[:-1]),
                     "vehicle_respawn_health_snapshot")
        self.require(all(self.actor_equal(death["host"]["actor"], r["host"]["actor"])
                         and r["host"]["world"] == death["host"]["world"] for r in stages[:-1])
                     and self.actor_equal(fresh["host"]["actor"], client["host"]["actor"])
                     and client["host"]["bound"] == fresh["host"]["world"], "vehicle_respawn_identity")
        _, _, settled = self.remount(3, client["seq"])
        self.isolation(death["op_id"], 3, death["seq"], settled["seq"])

    def check_transfer_ack(self, begin, commit, server, client):
        # v1.2: the independent transfer counter is never an input epoch.
        # A larger ACK alone is insufficient: certification also requires the
        # source-bound real-handler proof checked at completion/admission.
        for r in (commit, server, client):
            self.require(r["transfer_id"] == begin["transfer_id"] and r["op_id"] == begin["op_id"]
                         and self.actor_equal(r["actor"], begin["actor"])
                         and r["dst"] == begin["dst"], "vehicle_transfer_identity")
        floor = commit["host"]["actor"]["input_epoch"]
        epoch = server["input_ack_epoch"]
        self.require(commit["transition_epoch"] > 0 and commit["actor"] == commit["host"]["actor"]
                     and floor >= begin["actor"]["input_epoch"] > 0, "vehicle_transfer_commit_epoch")
        self.require(server["ready"] and client["ready"] and epoch > floor
                     and epoch == client["input_ack_epoch"] == server["actor"]["input_epoch"]
                     == client["actor"]["input_epoch"] == client["host"]["actor"]["input_epoch"]
                     and client["host"]["actor"] == client["actor"]
                     and client["host"]["bound"] == begin["dst"] and self.client_detached(client["host"]),
                     "vehicle_transfer_ack")
        self.require(commit["seq"] < server["seq"] < client["seq"]
                     and server["ms"] >= commit["ms"] and server["server_update"] >= commit["server_update"],
                     "vehicle_transfer_observation_order")

    def check_transfer(self):
        leave = self.ui("guest_leave", 4, 1)
        join = self.ui("guest_join", 4, 1)
        accept = self.ui("accept", 4, 1)
        confirm = self.ui("confirm", 4, 1)
        gameplay = self.ui("gameplay", 4, 1)
        self.require(leave["seq"] < join["seq"] < accept["seq"] < confirm["seq"] < gameplay["seq"], "vehicle_readmission_order")
        begin, release, commit = (self.one(name, 4) for name in ("transfer_begin", "transfer_release", "transfer_commit"))
        sa, ca = (self.one("transfer_ack", 4, stage=stage) for stage in ("server", "client"))
        # transfer_begin has no S: the last actual settled mount plus V proves it.
        mounted = [r for r in self.rows_for("action_result", site="authority", status="applied", action="mount")
                   if r["seq"] < begin["seq"] and r["target"] == begin["vehicle"]["target"]]
        self.require(bool(mounted) and self.server_mounted(mounted[-1]["host"], begin["vehicle"])
                     and begin["actor"] == mounted[-1]["host"]["actor"]
                     and begin["vehicle"]["target"]["kind"] == self.kind and begin["mode"] == self.transfer_mode, "vehicle_transfer_mounted")
        self.require(begin["src"] == begin["vehicle"]["target"]["world"] and begin["src"]["dimension"] == 0
                     and begin["dst"]["dimension"] == -1 and begin["src"] != begin["dst"], "vehicle_transfer_worlds")
        for r in (release, commit, sa, ca):
            self.require(r["transfer_id"] == begin["transfer_id"] and self.actor_equal(r["actor"], begin["actor"])
                         and r["dst"] == begin["dst"], "vehicle_transfer_identity")
        self.require(release["src"] == commit["src"] == begin["src"]
                     and int(begin["serial"]) < int(release["release_serial"]) < int(commit["commit_serial"]), "vehicle_transfer_serial_order")
        self.require(release["host"]["world"] == begin["src"]
                     and release["host"]["actor"] == begin["actor"]
                     and self.server_detached(release["host"], release["old"])
                     and release["old"]["target"] == begin["vehicle"]["target"], "vehicle_transfer_release")
        self.require(commit["host"]["world"] == begin["dst"] and self.server_detached(commit["host"], commit["source_vehicle"])
                     and commit["destination_mount_count"] == sa["destination_mount_count"] == 0
                     and self.released(sa["source_vehicle"])
                     and commit["source_vehicle"]["target"] == sa["source_vehicle"]["target"] == begin["vehicle"]["target"], "vehicle_transfer_source_survival")
        self.check_transfer_ack(begin, commit, sa, ca)
        self.require(begin["seq"] < release["seq"] < commit["seq"] < sa["seq"] < ca["seq"], "vehicle_transfer_observation_order")
        self.isolation(begin["op_id"], 4, begin["seq"], ca["seq"])
        armed = self.one("pending", 4, stage="armed", cause="synthetic_admission")
        pause, resume = self.one("pause", 4), self.one("resume", 4)
        # Frozen v1.2 table: SL's stable pause hold is a retained observation.
        paused = self.one("pending", 4, stage="retained", cause="global_pause")
        clear = self.one("admission_clear", 4)
        release_barrier = self.one("barrier_release", 4)
        installed = self.one("installed", 4)
        rs, rc = (self.one("gameplay_ready", 4, stage=stage) for stage in ("server", "client"))
        q = armed["tuple"]
        old_guest = self.one("isolation", 4, owner="server", stage="before", attempt_op=begin["op_id"])["guest"]
        self.require(int(q["generation"]) > int(old_guest["actor"]["generation"]), "vehicle_readmission_generation")
        self.require(int(q["ticket"]) > 0 and int(q["generation"]) > 0
                     and all(r["tuple"] == q for name in ("pending", "pause", "resume", "admission_clear", "barrier_release", "installed", "gameplay_ready")
                             for r in self.rows_for(name, 4)), "vehicle_pending_tuple")
        self.require(armed["pending"] and armed["refusal_armed"] and armed["result"] == "none"
                     and pause["paused"] and pause["pending"] and pause["refusal_armed"]
                     and paused["paused"] and paused["pending"] and paused["refusal_armed"]
                     and paused["result"] == "none"
                     and not resume["paused"] and resume["pending"] and resume["refusal_armed"]
                     and resume["ms"] - pause["ms"] >= 2000
                     and pause["active_update"] == paused["active_update"] == resume["active_update"], "vehicle_pause_retention")
        retained = [r for r in self.rows_for("pending", 4, stage="retained")
                    if r["cause"] in ("transfer", "loading") and not r["refusal_armed"] and r["transfer_open"]]
        self.require(bool(retained), "vehicle_natural_retention_missing")
        self.require(clear["pending"] and clear["transfer_open"] and not clear["refusal_armed"]
                     and clear["transfer_id"] == release_barrier["transfer_id"] == begin["transfer_id"], "vehicle_admission_clear")
        self.require(all(r["pending"] and r["result"] == "none" and not r["paused"] for r in retained), "vehicle_natural_retention_state")
        self.require(release_barrier["pending"] and not release_barrier["paused"] and not release_barrier["transfer_open"]
                     and installed["installed_count"] == 1 and not installed["pending"]
                     and not installed["paused"] and not installed["transfer_open"], "vehicle_retry_install")
        serial_chain = [armed, pause, paused, resume, clear, retained[-1], release_barrier, installed]
        self.require(all(int(a["serial"]) < int(b["serial"]) for a, b in zip(serial_chain, serial_chain[1:])), "vehicle_progress_serial_order")
        self.require(int(begin["serial"]) < int(clear["serial"]) < int(commit["commit_serial"]) < int(release_barrier["serial"]), "vehicle_progress_transfer_order")
        self.require(release_barrier["active_update"] == retained[-1]["active_update"] + 1, "vehicle_first_retry_pass")
        # CONFIRM and the owned gameplay request precede SG's successful
        # gameplay-ready response, then the client's actual ready/bound receipt.
        self.require(accept["seq"] < armed["seq"] and installed["seq"] < confirm["seq"]
                     and confirm["seq"] < gameplay["seq"] < rs["seq"] < rc["seq"], "vehicle_progress_delivery_order")
        for r in (installed, rs, rc):
            # O24: the deadline is wall time. active_update counts unpaused
            # server retry passes (vehicle_smoke_probe.c:1231-1234); their
            # rate follows the host loop speed (Wii loading screen ~45/s, PC
            # ~37/s), so a pass-count bound fails a 5 s Wii load that the
            # 15000 ms bound accepts. Order is still required (>= 0).
            self.require(r["active_update"] >= release_barrier["active_update"]
                         and 0 <= r["ms"] - release_barrier["ms"] <= 15000, "vehicle_progress_deadline")
        for r in (rs, rc):
            guest = r["guest"]
            worldkey = "world" if r["stage"] == "server" else "bound"
            detached = self.server_detached(guest) if r["stage"] == "server" else self.client_detached(guest)
            self.require(r["ready_count"] == r["installed_count"] == 1 and r["confirmed"]
                         and (r["stage"] != "server" or guest["confirmed"])
                         and guest["ready"] and detached and guest["actor"]["slot"] == 1
                         and guest["actor"]["generation"] == q["generation"] and guest[worldkey] == q["world"], "vehicle_progress_ready")
        self.require(self.inventory(rs["guest"]) == self.inventory(rc["guest"])
                    and rs["guest"]["inv_used"] > 0 and rs["guest"]["actor"] == rc["guest"]["actor"], "vehicle_progress_guest_identity")
        for r in self.rows_for("isolation", 4, owner="client"):
            if not r["guest"]["present"]:
                self.require(r["guest"]["generation"] == q["generation"]
                             and r["guest"]["pending_ticket"] == q["ticket"], "vehicle_pending_body_tuple")

    def heartbeat_floor(self, unused=10):
        return max(10, 4 * self.run_ok["ms"] // 5000) if self.run_ok else 10

    def complete_error(self):
        if self.error:
            return self.error
        if self.run_ok is None:
            return "vehicle_missing_run_ok"
        if self.done_count != 1:
            return "vehicle_missing_profile_done"
        return None

    def summary(self):
        return dict(contract="CONTRACT-v1.2", kind=self.kind, run_id=self.run_id,
                    build_id=self.build_id, transfer_mode=self.transfer_mode,
                    claims=dict(self.claims), switch_pairs=self.switch_pairs,
                    complete=self.complete_error() is None, error=self.complete_error(),
                    run_ok_ms=self.run_ok["ms"] if self.run_ok else None,
                    heartbeat_floor=self.heartbeat_floor(), unique_records=len(self.seen),
                    identical_retries=self.retries, profile_done_count=self.done_count,
                    certification="calibration_required", limits=["Peaceful functional fixture",
                    "Normal survival, cargo, rollback, guest-positive riding, physical controllers unproven",
                    "split_dims >=19 TPS remains required",
                    "portal contact/dwell unproven" if self.transfer_mode == "fixture_request" else "contact observed"])

def vehicle_manifest(path, kind, build_id="", dol_path=""):
    """Frozen packet format documented in docs/vehicle_smoke_profile.md."""
    p = Path(path)
    manifest = vehicle_json_loads(p.read_text(encoding="utf-8-sig"))
    need = VehicleSmokeEvidence.require
    need(type(manifest) is dict, "vehicle_manifest_schema")
    need(manifest.get("contract") in ("CONTRACT-v1.2", "CONTRACT-v1.3"), "vehicle_manifest_contract")  # O18: v1.3 = v1.2 wire + fixture amendment
    need(bool(build_id) and manifest.get("build_id") == build_id, "vehicle_manifest_build")
    for field in ("release_sha256", "collector_sha256", "source_sha256"):
        need(isinstance(manifest.get(field), str) and re.fullmatch(r"[0-9a-f]{64}", manifest[field]), "vehicle_manifest_" + field)
    need(manifest["collector_sha256"] == hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "vehicle_manifest_collector_changed")
    need(type(manifest.get("kinds")) is dict and set(manifest["kinds"]) == set(VS_KINDS), "vehicle_manifest_kinds")
    for k in VS_KINDS:
        config = manifest["kinds"][k]
        need(type(config) is dict, "vehicle_manifest_config_schema")
        need(config.get("transfer_mode") in ("contact", "fixture_request"), "vehicle_manifest_mode")
        need(config.get("geometry_proven") is True, "vehicle_manifest_geometry_unproven")
        need(isinstance(config.get("config_sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", config["config_sha256"]), "vehicle_manifest_config_hash")
        config_path = Path(config.get("config_path", ""))
        if not config_path.is_absolute():
            config_path = p.parent / config_path
        need(config_path.is_file() and hashlib.sha256(config_path.read_bytes()).hexdigest() == config["config_sha256"], "vehicle_manifest_config_changed")
    need(kind in VS_KINDS, "vehicle_manifest_kind")
    if dol_path:
        need(hashlib.sha256(Path(dol_path).read_bytes()).hexdigest() == manifest["release_sha256"], "vehicle_manifest_release_changed")
    return manifest, hashlib.sha256(p.read_bytes()).hexdigest()


def vehicle_calibration_error(manifest, base):
    """Provenance is a reviewed assertion, never inferred from synthetic rows.

    Retained authentic bytes and an independent negative report are required.
    The lead still authenticates provenance; this is not a hardware authenticator.
    """
    try:
        c = manifest.get("calibration", {})
        need = VehicleSmokeEvidence.require
        need(type(c) is dict, "vehicle_calibration_required")
        need(c.get("provenance") == "real_wii" and c.get("contract") == "CONTRACT-v1.2"
             and c.get("reviewed") is True, "vehicle_calibration_required")
        for name in ("tape", "negative_report"):
            path = Path(c.get(name, ""))
            if not path.is_absolute():
                path = Path(base) / path
            need(path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == c.get(name + "_sha256"), "vehicle_calibration_" + name)
            if name == "negative_report":
                report = vehicle_json_loads(path.read_text(encoding="utf-8-sig"))
                need(type(report) is dict, "vehicle_calibration_negative_replay")
                need(report.get("contract") == "CONTRACT-v1.2" and report.get("status") == "PASS"
                     and report.get("parser") == "ingest_telemetry_line"
                     and report.get("provenance") == "independent_real_wii_negative_replay"
                     and report.get("tape_sha256") == c.get("tape_sha256")
                     and report.get("collector_sha256") == manifest["collector_sha256"]
                     and type(report.get("cases")) is list and len(report["cases"]) > 0
                     and all(type(case) is dict and case.get("rejected") is True and bool(case.get("reason")) for case in report["cases"]),
                     "vehicle_calibration_negative_replay")
        return None
    except (ValueError, TypeError, OSError, KeyError) as exc:
        return str(exc)


# These are coverage obligations on the REAL production handler, not more
# snapshots purporting to prove that an earlier accepted ACK did not exist.
VS_ACK_PROOF_CASES = ("exact_pending_non_retry", "larger_non_pending_rejected",
                      "stale_rejected", "zero_rejected", "retry_not_emitted",
                      "first_accepted_only", "wrong_transfer_rejected", "before_commit_not_emitted")


def vehicle_transfer_ack_proof_error(manifest, base):
    """Require reviewed, hash/source-bound production-handler evidence per kind."""
    try:
        need = VehicleSmokeEvidence.require
        binding = manifest.get("transfer_ack_proof", {})
        need(type(binding) is dict, "vehicle_transfer_ack_handler_proof_required")
        path = Path(binding.get("path", ""))
        if not path.is_absolute():
            path = Path(base) / path
        need(path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == binding.get("sha256"),
             "vehicle_transfer_ack_handler_proof_required")
        report = vehicle_json_loads(path.read_text(encoding="utf-8-sig"))
        need(type(report) is dict and report.get("contract") == "CONTRACT-v1.2"
             and report.get("provenance") == "production_real_handler_tests"
             and report.get("reviewed") is True and report.get("status") == "PASS"
             and report.get("source_sha256") == manifest.get("source_sha256"),
             "vehicle_transfer_ack_handler_proof_provenance")
        kinds = report.get("kinds", {})
        need(type(kinds) is dict and set(kinds) == set(VS_KINDS), "vehicle_transfer_ack_handler_proof_kinds")
        for proof in kinds.values():
            need(type(proof) is dict and type(proof.get("cases")) is dict
                 and all(proof["cases"].get(case) is True for case in VS_ACK_PROOF_CASES),
                 "vehicle_transfer_ack_handler_proof_coverage")
            ack = proof.get("first_accepted_ack", {})
            need(type(ack) is dict and type(ack.get("pending_epoch")) is int
                 and 0 < ack["pending_epoch"] <= 4294967295
                 and type(ack.get("ack_epoch")) is int and ack["ack_epoch"] == ack["pending_epoch"]
                 and ack.get("retry") is False and ack.get("accepted") is True
                 and type(ack.get("accepted_index")) is int and ack["accepted_index"] == 1
                 and type(ack.get("emitted_count")) is int and ack["emitted_count"] == 1,
                 "vehicle_transfer_ack_handler_proof_exact_first")
        return None
    except (ValueError, TypeError, OSError, KeyError) as exc:
        return str(exc)


def vehicle_is_pc_replay_artifact(recorded):
    # Admission follows provenance/status, never a filename. Even partial
    # relabeling of a diagnostic result cannot make it certification evidence.
    return (bool(recorded.get("vehicle_pc_replay"))
            or str(recorded.get("status", "")).startswith("PC_REPLAY")
            or recorded.get("evidence_provenance") == "pc_diagnostic_replay"
            or "pc_replay" in str(recorded.get("vehicle_certification", ""))
            or "pc_replay" in str(recorded.get("certification_scope", ""))
            or recorded.get("noncertifying") is True
            or recorded.get("source_tape") == "source-tape.ndjson"
            or "source_tape_sha256" in recorded)


def vehicle_pc_tape_lines(raw):
    parts = raw.split(b"\n")
    return [part + b"\n" for part in parts[:-1]] + ([parts[-1]] if parts[-1] else [])


def vehicle_pc_replay_args_error(args, profile):
    if not getattr(args, "replay", "") or profile != VEHICLE_SMOKE_PROFILE:
        return "vehicle_pc_replay requires offline host_vehicle_smoke_8m replay"
    forbidden = ("tcp_connect_host", "vehicle_dol", "vehicle_check_manifest",
                 "vehicle_validate_pass", "vehicle_aggregate", "selftest", "release_gate",
                 "require_tnt_evidence", "require_wii_budget")
    if any(getattr(args, key, False) for key in forbidden):
        return "vehicle_pc_replay forbids live/device collection, staging and certification side effects"
    return None


def vehicle_validate_packet(pass_paths, manifest_path, kind="", build_id=""):
    """Replays all raw rows through the live gate; returns an aggregate matrix."""
    manifest, digest = vehicle_manifest(manifest_path, kind or VS_KINDS[0], build_id)
    problem = vehicle_calibration_error(manifest, Path(manifest_path).parent)
    VehicleSmokeEvidence.require(problem is None, problem or "vehicle_calibration_required")
    problem = vehicle_transfer_ack_proof_error(manifest, Path(manifest_path).parent)
    VehicleSmokeEvidence.require(problem is None, problem or "vehicle_transfer_ack_handler_proof_required")
    summaries = {}
    run_ids = set()
    for path in pass_paths:
        p = Path(path)
        recorded = vehicle_json_loads(p.read_text(encoding="utf-8-sig"))
        need = VehicleSmokeEvidence.require
        need(type(recorded) is dict, "vehicle_packet_schema")
        need(not vehicle_is_pc_replay_artifact(recorded), "vehicle_packet_pc_replay")
        need(recorded.get("status") == "PASS" and recorded.get("profile") == VEHICLE_SMOKE_PROFILE, "vehicle_packet_pass")
        summary = recorded.get("vehicle_smoke", {})
        k = summary.get("kind")
        need(k in VS_KINDS and k not in summaries and (not kind or k == kind), "vehicle_packet_kind")
        need(recorded.get("vehicle_manifest_sha256") == digest, "vehicle_packet_manifest_mismatch")
        run_id = recorded.get("run_id")
        need(bool(run_id) and run_id not in run_ids, "vehicle_packet_run_id")
        tape = p.parent / "vehicle-wire.ndjson"
        need(tape.is_file(), "vehicle_packet_raw_tape_missing")
        need(tape.read_bytes().endswith(b"\n"), "vehicle_packet_tape_unterminated")
        need(recorded.get("vehicle_tape_sha256") == hashlib.sha256(tape.read_bytes()).hexdigest(), "vehicle_packet_tape_changed")
        with tempfile.TemporaryDirectory() as td:
            c = Collector(_selftest_args(Path(td), run_id, VEHICLE_SMOKE_PROFILE,
                expected_build=build_id, cw_vehicle_kind=k, vehicle_manifest=str(Path(manifest_path).resolve())))
            for line in tape.read_text(encoding="utf-8").splitlines(keepends=True):
                if ingest_telemetry_line(c, line):
                    break
            if not c.fail_reason:
                c._finish_normal("validator vehicle replay")
            need(bool(c.pass_reason) and not c.fail_reason, "vehicle_packet_replay: " + str(c.fail_reason))
            need(c.vehicle_smoke.summary() == summary, "vehicle_packet_summary_mismatch")
            need(recorded.get("heartbeats") == c.hb_count, "vehicle_packet_heartbeat_count")
            need(recorded.get("last_heartbeat") == c.last_heartbeat_event
                 and summary["build_id"] == build_id, "vehicle_packet_heartbeat_provenance")
            need(recorded.get("vehicle_certification") == "calibrated_functional", "vehicle_packet_certification")
        summaries[k] = summary
        run_ids.add(run_id)
    pairs = {tuple(pair) for summary in summaries.values() for pair in summary["switch_pairs"]}
    if not kind:
        VehicleSmokeEvidence.require(set(summaries) == set(VS_KINDS), "vehicle_packet_three_kinds_required")
        VehicleSmokeEvidence.require(pairs == {(a, b) for a in VS_KINDS for b in VS_KINDS if a != b}, "vehicle_packet_switch_coverage")
    return dict(manifest_sha256=digest, kinds=summaries,
                coverage={a: {b: a != b and (a, b) in pairs for b in VS_KINDS} for a in VS_KINDS},
                certification="reviewed_calibration_and_replayed_functional_evidence")



# G9 R6 (ASTRA-G9-REVIEW Required 6): the heartbeat's positional "lq" array --
# the server pending-light queue per world slot and the light stop / flood
# ceiling / stack drop counters (source/platform/hw_telemetry.h, field
# comment above lq_peak). Order is the wire order.
LQ_FIELDS = ("peak_b0", "peak_b1", "seeds_pk0", "seeds_pk1", "drain_us0",
             "drain_us1", "drain_pops0", "drain_pops1", "fault", "ceil",
             "oom", "fceil", "stk_drop")
# Any non-zero value of these FAILs the run: a light stop, a drain ceiling, a
# drain frontier OOM, any flood's 500k ceiling, or a dropped stack push.
LQ_FAIL_FIELDS = ("fault", "ceil", "oom", "fceil", "stk_drop")

# G9 R6 v2 (ASTRA-G9-REVIEW-2 item 5): the 26-entry layout of the same "lq"
# key (source/platform/hw_telemetry.h, HW_TELEMETRY_LQ_N): saturating uint16;
# memory in KiB rounded up. The 13-entry v1 layout above stays accepted for
# explicitly older artifacts, never under --require-g9-evidence.
LQ2_FIELDS = ("seed_kib0", "seed_kib1", "undo_kib0", "undo_kib1",
              "pin_kib0", "pin_kib1", "front_kib0", "front_kib1",
              "ckpt_kib0", "ckpt_kib1", "seeds_pk0", "seeds_pk1",
              "drain_us0", "drain_us1", "drain_cells0", "drain_cells1",
              "debt0", "debt1", "tick_us", "ckpt_us", "fault", "ceil", "oom",
              "ckpt_fail", "fceil", "stk_drop")
# Any non-zero value FAILs: pending debt right after a prescribed drain, a
# light stop, a drain ceiling / frontier OOM, a checkpoint snapshot OOM, any
# flood ceiling, a dropped stack push.
LQ2_FAIL_FIELDS = ("debt0", "debt1", "fault", "ceil", "oom", "ckpt_fail",
                   "fceil", "stk_drop")
LQ2_MAX = 0xffff
# LIGHT-CLIENT-LOSSLESS: the heartbeat "clq" array (source/platform/hw_telemetry.h
# hw_heartbeat.clq), the CLIENT render relight split from lq's process-wide
# fceil/stk_drop. Absent = all zero.
CLQ_FIELDS = ("cfceil", "coom", "cpark", "crep")
CLQ_FAIL_FIELDS = ("cfceil", "coom")
# nether-dark/1: the heartbeat "blc" array (source/platform/hw_telemetry.h
# hw_heartbeat.blc), a client vs server block-light census of player 0's 3x3
# near columns, sampled at most every 5 s. Recorded/printed only, never judged.
BLC_FIELDS = ("dim", "cols", "shared", "client_lit", "server_lit", "age_s", "us")


def format_blc(hb: dict) -> str:
    """' blc=dim:-1 cols:9 shared:9 client_lit:N server_lit:M age:Ks us:U', or ''
    when the heartbeat carries no (or a malformed) census."""
    blc = hb.get("blc")
    if (type(blc) is not list or len(blc) != len(BLC_FIELDS)
            or any(type(v) is not int for v in blc)):
        return "" if blc is None else f" blc=malformed:{blc!r}"
    d = dict(zip(BLC_FIELDS, blc))
    return (f" blc=dim:{d['dim']} cols:{d['cols']} shared:{d['shared']}"
            f" client_lit:{d['client_lit']} server_lit:{d['server_lit']}"
            f" age:{d['age_s']}s us:{d['us']}")

def g9_drain_records(event):
    """Paired elapsed attribution; never infer scheduler CPU from residual."""
    if event.get("event") == "g9_drain_diag":
        return [dict(event, measurement="isolated_elapsed")]
    a = event.get("lqd")
    if not isinstance(a, list) or len(a) != 22 or event.get("lqd_v") != 3:
        return []  # v1/v2 retain their historical meaning
    if any(type(v) is not int or not 0 <= v <= 0xffffffff for v in a):
        return []
    out = []
    for k in range(2):
        if not a[6+k]:
            continue
        out.append(dict(world=k, pops=a[2+k], us=a[6+k], yields=a[k],
            yield_us=a[8+k], lock_us=a[10+k], recovery_us=a[4+k],
            measurement="normal_elapsed", tick=event.get("tick"),
            run_id=event.get("run_id")))
    return out


# ci-1101/37: the longest >= 10 ms server-drain mutex wait of one heartbeat
# window, emitted only when one happened (hw1101c: 71,206 us inside the server
# tick's light drain, previously with no way to name the lock). Absent on every
# quiet heartbeat and on every older tape, so the key is optional: a missing or
# malformed value is simply "no attribution", never a gate failure. The array
# is [wait_us, mutex, holder_id, holder_tag, stripe, name]; mutex is the
# contended mutex's address low 32 bits, holder_id/holder_tag identify the
# thread that held it (0 = free/unknown), stripe is the thread_stripe_register
# index (0 = unregistered, name then ""). Evidence only -- no verdict reads it.
LQD_WAIT_FIELDS = ("wait_us", "mutex", "holder_id", "holder_tag", "stripe", "name")

def parse_lqd_wait(event: dict) -> dict | None:
    """The drain long-wait attribution, or None when this heartbeat carries
    none (the common case) or a malformed value. Never raises."""
    a = event.get("lqd_w")
    if (type(a) is not list or len(a) != len(LQD_WAIT_FIELDS)
            or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in a[:5])
            or type(a[5]) is not str):
        return None
    d = dict(zip(LQD_WAIT_FIELDS, a))
    return d if d["wait_us"] > 0 else None

def format_lqd_wait(event: dict) -> str:
    d = parse_lqd_wait(event)
    if d is None:
        return ""
    return (f" lqd_w=us:{d['wait_us']} mutex:0x{d['mutex']:08x}"
            f" holder:{d['holder_id']}/tag{d['holder_tag']}"
            f" stripe:{d['stripe']} name:{d['name']!r}")


# R24: allow a full join/drain after the device's gameplay duration. 180 s
# exceeds the recorded 35.646 s IO completion latency by five times, while
# bounding the disclosed permanent-IO-error join risk. Not a timing exclusion.
CHECKPOINT_SHUTDOWN_WAIT_SECONDS = 180
def parse_heartbeat_fps_window(event: dict) -> float | None:
    """Producer window rate (wf), absent on pre-L225 tapes.

    Never synthesize it from receive times or assume consecutive received
    heartbeats cover consecutive windows. Keep the legacy fps schema/gates.
    """
    rate = event.get("wf")
    if (type(rate) not in (int, float) or not 0 <= rate <= sys.float_info.max
            or not math.isfinite(rate)):
        return None
    return rate


class Collector:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.vehicle_pc_replay = bool(getattr(args, "vehicle_pc_replay", False))
        self.vehicle_pc_source_path = None
        self.vehicle_pc_source_sha256 = None
        self.vehicle_pc_source_lines = []
        self.vehicle_pc_source_cursor = 0
        self.vehicle_pc_ingesting = False
        if self.vehicle_pc_replay:
            problem = vehicle_pc_replay_args_error(args, args.profile)
            if problem:
                raise ValueError(problem)
            src = Path(args.replay).resolve()
            target = Path(args.out).resolve()
            if target.exists() or target.is_relative_to(src):
                raise ValueError("vehicle_pc_replay output must be a new isolated directory outside the source run")
            self.vehicle_pc_source_path = src / "telemetry.ndjson"
            source_bytes = self.vehicle_pc_source_path.read_bytes()
            self.vehicle_pc_source_sha256 = hashlib.sha256(source_bytes).hexdigest()
            # Split only at LF; do not normalize CRLF, UTF-8, whitespace or EOF.
            self.vehicle_pc_source_lines = vehicle_pc_tape_lines(source_bytes)
            # No decoded/re-serialized derivative is the source evidence.
            target.mkdir(parents=True, exist_ok=False)
            with (target / "source-tape.ndjson").open("xb") as f:
                f.write(source_bytes)
        self.profile = PROFILES.get(args.profile, PROFILES["far_idle_2m"])
        # Staged runs append this suffix before passing the RunId to the collector.
        # Older direct-run IDs put _sN before additional suffixes.
        seeds = [] if args.run_id.endswith("_unseeded") else re.findall(
            r"(?:^|_)s([+-]?\d+)(?=_|$)", args.run_id)
        self.seed = int(seeds[-1]) if seeds else None
        # lava-aabb-0924/H4 (review fix 10): a chunk save that FAILED during the
        # run must never hide behind a PASS -- warn the moment save_fail_n rises
        # and carry every rise into PASS.json / FAIL.json ("save_fail_warnings").
        # H5 (Opus H4 nit): the counter counts from BOOT, so it starts at 0
        # here and a failure before the first heartbeat warns on that beat.
        self.save_fail_seen = 0
        self.save_fail_warnings: list[str] = []
        # world_copy_convert (T16a): the wcc_* observer lines, as received.
        self.wcc_rows: list[dict] = []
        self.wcc_device_verdict = None
        # G9 R6: element-wise maxima of the heartbeat "lq" array (None until a
        # heartbeat carries it -- an older .dol never does). lq_version is the
        # layout (1: 13 uint32, 2: 26 uint16); one run never mixes them.
        self.lq_max: list[int] | None = None
        self.lq_version = 0
        self.require_g9_evidence = bool(getattr(args, "require_g9_evidence", False))
        self.g9_cpu = G9CpuEvidence()
        self.g9_capture = G9CaptureEvidence()
        self.service_rows = []
        self.service_errors = []
        self.service_metrics = {}
        self.service_final = set()
        self.service_sequence = 0
        self.service_input_unassigned = 0
        self.service_teardown_unobserved = 0
        self.service_live_previous = None
        self.service_live_ms = 0
        self.service_at_us = 0
        self.service_identity = None
        self.service_context = []
        self.service_states = {}
        self.service_input = {}
        self.service_audio = []
        self.out = Path(args.out)
        self.out.mkdir(parents=True, exist_ok=True)
        self.telemetry_path = self.out / "telemetry.ndjson"
        self.latest_path = self.out / "latest.json"
        self.summary_path = self.out / "summary.md"
        self.dump_path = self.out / "watchdog_dump.txt"
        self.started = time.monotonic()
        self.first_hb: float | None = None
        self.last_hb: float | None = None
        self.hb_count = 0
        # Heartbeat tick gaps with their device-side attribution (see
        # heartbeat_gap). Evidence only -- no verdict reads this list.
        self.telemetry_gaps: list[dict] = []
        self.done = False
        self.tps_pending = False
        self.tps_records = 0
        self.tps_profile_done = False
        self.tps_end: dict | None = None
        self.shutdown_required = False
        self.shutdown_rows: list[dict] = []
        self.shutdown_done: dict | None = None
        self.shutdown_errors: list[str] = []
        self.shutdown_last_counters: dict | None = None
        self.release_gate = bool(getattr(args, "release_gate", False))
        self.require_tnt_evidence = bool(getattr(args, "require_tnt_evidence", False))
        self.tnt_terminal: tuple[int, ...] | None = None
        # mob_motion_2m verdict (terminal payload once st >= 2) and the
        # mob-move cost counters summed over every beat that carried them
        # (Fable S2 F2; read on perf_stress_5m, never a gate).
        self.mm_terminal: tuple[int, ...] | None = None
        self.mm_cost = {"beats": 0, "moves": 0, "move_us": 0,
                        "env_walks": 0, "env_ents": 0, "max_move_us": 0,
                        "max_moves": 0}
        self.release_evidence_error: str | None = None
        self.previous_perf: dict = {}
        self.fail_reason: str | None = None
        self.failure_class: str | None = None
        self.pass_reason: str | None = None
        self.latest: dict = {}
        self.last_heartbeat_event: dict = {}
        self.death_telemetry = {"version": 1, "valid": False, "session": None,
                                "host_deaths": None, "host_cause": None,
                                "guest_deaths": None, "guest_cause": None}
        self.death_previous = None
        self.perf_survival_guard = {"version": None, "valid": False,
                                   "mode": "diagnostic refill plus lethal-debit guard",
                                   "requested": None, "effective": None}
        self.perf_guard_context = None
        self.perf_guard_previous = None
        self.perf_guard_coverage = {
            "effective_rows": 0, "excluded_rows": 0,
            "barrier_rows": {name: 0 for name in
                             ("pre-arm", "loading", "pause", "ACK", "residency",
                              "transfer", "terminal", "world hold")},
            "basis": "heartbeat samples; excluded samples grant no guarded coverage",
        }
        # mp_lan_walk_10m: paired, same-generation REMOTE_PLAY heartbeats.
        self.mp_move_previous: dict | None = None
        self.mp_move_cadence = {
            "totals": {"ticks": 0, "samples": 0, "rpcdrop": 0},
            "pairs": 0, "worst_pair": None,
        }
        self.max_rt_us = 0
        self.max_wt_us = 0
        # ci-1101/37: the largest drain long-wait attribution seen this run
        # (see parse_lqd_wait), or None. Evidence only.
        self.lqd_wait = None
        self.max_oom = 0
        self.max_trunc = 0
        self.world_loaded_seen = False
        # REL-MP-GATE: the first heartbeat that showed the session gone, as
        # (heartbeat number, mp_end_reason, world_loaded); None while it held.
        self.mp_session_end: tuple | None = None
        # REL-MP-JOINGATE: first heartbeat load_screen_ms > 0 (initial join).
        self.join_ms: int | None = None
        self.max_fvd: int | None = None
        self.max_fring: int | None = None
        self.max_ndisp = 0
        self.max_map_step = 0
        # push-0921 A11: the walker's own proof of what it entered/skipped --
        # the UNION of every heartbeat that carried the fields (A16-4), not the
        # last one. None = a tape that never carried them at all.
        self.np_proof: dict | None = None
        # THE TERMINAL EVIDENCE (push-0921 A23, review A20-1): the highest
        # map_max reported BY A HEARTBEAT THAT ALSO CARRIED THE WALKER PROOF.
        # The menus verdict requires this -- not the run-wide max_map_step --
        # to reach the leg's terminal ordinal. None = no such beat.
        self.np_proof_step_max: int | None = None
        # menus-dwell-walker-1026 (hw1026c): the walker's one-shot named
        # abandonment line ({"event":"menu_ap_fail"}), if it sent one.
        self.menu_ap_fail: dict | None = None
        # Heartbeats the producer replaced with its minimal overflow
        # substitute (hw_telemetry.c's `"overflow":true` line). They carry no
        # map_max and no np_* keys, so they are evidence of nothing; counted
        # only so a refusal can say so instead of blaming loss (A20-5).
        self.overflow_beats = 0
        self.map_sign_reached = False
        # menus_dwell_creative scenario evidence (2026-09-02): OR-across-
        # heartbeats, mirroring map_sign_reached above -- true the moment ANY
        # heartbeat's `creative` field reads true, and sticky afterward, so a
        # profile that only requires "was it creative at least once" (not
        # "is it creative on the last heartbeat before profile_done") is
        # provable even if the very last beat raced profile_done.
        self.creative_seen = False
        self.max_smig_state = 0
        self.max_smig_ms = 0
        self.max_smig_regions = 0
        self.max_smig_chunks = 0
        self.max_smig_signs = 0
        self.max_smig_expected = 0
        self.max_smig_verified = 0
        # mp_reconnect_soak scenario evidence: mrs_cycles is monotonic across
        # the run (source/game/mp_reconnect_soak_core.h never decrements it),
        # so tracking the running max is equivalent to reading the last
        # heartbeat's value but survives a truncated/reordered tail. wedge
        # is sticky C-side once set, but OR-across-heartbeats here is the
        # same result and needs no ordering assumption either.
        self.max_mrs_cycles = 0
        self.mrs_wedge_seen = False
        # Running max of wd_hb_gap_max_ms across every heartbeat this run
        # (2026-09 stall-hunt follow-up): before this, a PASS carried NO
        # peak-gap evidence at all -- only a FAIL's failure_context showed
        # the LAST heartbeat's gap reading. A clean run left no quantitative
        # trace of how close it came to the watchdog's ~3s trip, which the
        # >3s-stall hunt profiles (mp_lan_walk_10m, mp_reconnect_soak) exist
        # to surface. wd_hb_gap_max_ms is itself a running max on the C side
        # (watchdog_heartbeat_gap_max_ms), so max()-ing it here across
        # heartbeats is safe (monotonic non-decreasing input).
        self.max_hb_gap_ms = 0
        # Far-ring keep-up probe (2026-09-03 reach-sweep gate 4): far_missing_max
        # and far_settle_ms are ALREADY running maxima computed device-side
        # (source/game/far_ring_keepup.h), so tracking the collector's own max
        # of each across heartbeats is defense-in-depth against a truncated/
        # reordered tail, not the primary source of truth -- same reasoning as
        # max_hb_gap_ms above. srv_gen_us/srv_gen_cols are single-tick samples
        # (like srv_rt_us/srv_wt_us -> max_rt_us/max_wt_us above), so their max
        # here IS the primary peak-cost evidence. srv_gen_backlog similarly.
        self.max_far_missing = 0
        self.max_far_missing_reported = 0
        self.max_far_settle_ms = 0
        # residency-1005/2 round 3: compact per-heartbeat "rza" records for the
        # per-actor residency gate (residency_gate_eval).
        # residency-1005/4 (items 9-11): every heartbeat after the first
        # rza-bearing one is recorded -- valid, invalid or missing -- and
        # graded with the SAME required legs / induced option as the tape CLI.
        self.rz_records = []
        self.residency_expect_fault = getattr(args, "residency_expect_fault", "") or ""
        self.require_residency = bool(getattr(args, "require_residency", False) or self.residency_expect_fault)
        self.residency_allow_induced = bool(getattr(args, "residency_allow_induced", False) or self.residency_expect_fault)
        self.residency_required_legs = residency_required_legs(
            args.profile, getattr(args, "residency_require_moving", "") or "")
        self.max_srv_gen_us = 0
        self.max_srv_gen_cols = 0
        self.max_srv_gen_backlog = 0
        # P2-c telemetry (2026-09-03 round-2 frame-drop root-cause doc, "#3
        # Instrumentation gap"): ms_mesh_worker_max is ALREADY a device-side
        # running max (chunk_mesher.c's mesher_build_ms_max), so tracking the
        # collector's own max here is defense-in-depth against a truncated/
        # reordered tail -- same reasoning as max_far_settle_ms above.
        # mem2_lock_wait is a cumulative (monotonic non-decreasing) count, so
        # its own max across heartbeats is the run's peak -- same treatment
        # as max_srv_gen_backlog.
        self.max_ms_mesh_worker = 0.0
        self.max_mem2_lock_wait = 0
        # T-a (2026-09-03, Round 3 (3) "walk wedge" telemetry self-quiesce
        # fix): tel_fail is monotonic non-decreasing device-side (hw_
        # telemetry.c's hw_telemetry_send_fail_classify never decrements
        # fail_count), so its own running max here IS the run's final
        # cumulative count -- same treatment as max_mem2_lock_wait above.
        # tel_quiesced_seen is OR-across-heartbeats (sticky once true,
        # mirrors mrs_wedge_seen/map_sign_reached above): the collector's own
        # process is what goes silent once the device quiesces, so the LAST
        # delivered heartbeat is the only place this can ever be observed.
        # frame_counter is monotonic (main.c's hw_frame_counter only
        # increments), so its max is the highest proof-of-life value any
        # heartbeat carried -- cross-check this against the SD boot trace's
        # own frame_alive lines when a run froze with telemetry still
        # reporting frame_counter climbing (a real socket-only failure)
        # vs. frozen (a real main-thread hang).
        self.max_tel_fail = 0
        self.tel_quiesced_seen = False
        # Fix round brief finding #3 (2026-09-07): both fields ride only on
        # carve_census rows (--cw-carve-census armed), so they stay at 0 on
        # any run that never emits one -- same "0 means never observed"
        # convention as the srv_gen_*/mem2_lock_wait maxes above.
        self.max_tel_drop = 0
        self.max_staged_skipped = 0
        # Sender self-telemetry (2026-09-07 starvation fix). tel_drop now also
        # rides the HEARTBEAT, which owns the ring's priority lane -- the
        # carve_census row that used to be its only carrier is itself a
        # droppable observer record, so a saturated ring hid its own losses.
        # min_tel_it/min_tel_sends are running MINIMA across the heartbeat
        # windows: they answer "was the sender scheduled at all this second?",
        # the question no host-side instrument on this rig can answer.
        # SCALE NOTE (2026-09-08): the sender's idle wait became a signalled
        # condvar park with a 10 ms ceiling, so a healthy idle min_tel_it is
        # ~100 (plus one pass per record), not the ~1000 the old 1 ms poll
        # produced. 0 still means "never scheduled"; only the healthy floor
        # moved, so runs either side of that change are not comparable here.
        # max_tel_qmax pinned at 5 means the ring stood full; a non-zero
        # max_tel_hb_repl means a heartbeat was superseded before it could be
        # sent, i.e. the priority lane itself was starving.
        self.min_tel_it = None
        self.min_tel_sends = None
        self.max_tel_smax_us = 0
        self.max_tel_qmax = 0
        self.max_tel_hb_repl = 0
        # Bounded sends (2026-09-08). max_tel_wto rides the HEARTBEAT: lines
        # abandoned because the socket did not become writable inside the
        # per-line deadline. It is NOT a failure count -- a healthy run with a
        # busy IOS may show a few -- but a large one beside a live heartbeat
        # is exactly the signature of the 2026-09-07 stream deaths, now
        # survived instead of fatal. max_loading_suppressed rides
        # carve_census: census sets whose eleven carve_tenant records were
        # deliberately withheld during the loading phase, so a thinned reload
        # can never be mistaken for a lost stream.
        self.max_tel_wto = 0
        # SENDER STAGE (2026-09-08 wedge lane). The 2026-09-07 deaths showed
        # tel_it/tel_sends at 0 for 4-5 s with tel_wto climbing: the sender
        # was parked in SOME call and nothing said which. tel_stg names it
        # (enum hw_telemetry_sender_stage) and tel_stg_us says for how long,
        # both measured at heartbeat-compose time -- and the heartbeat is the
        # one record with a priority lane, so it still arrives mid-wedge.
        # worst_tel_stage is the stage seen with the LARGEST tel_stg_us, kept
        # together as a pair because either half alone is unreadable.
        self.max_tel_stg_us = 0
        self.worst_tel_stage = None
        self.tel_stages_seen = {}
        # Longest SINGLE net_poll and net_send, windowed per heartbeat.
        # max_tel_smax_us times a whole record and cannot separate the two
        # IOS calls; these can, which is the point of the lane.
        self.max_tel_pmax_us = 0
        self.max_tel_xmax_us = 0
        # Lines abandoned by the opt-in --cw-tel-nonblock experiment. Stays 0
        # on every run that does not arm it. Distinct from max_tel_wto, which
        # counts the bounded-send DEADLINE's abandonments.
        self.max_tel_abd = 0
        # Last raw transport results the sender published (see the heartbeat
        # field docs): net_poll return, its revents bitmask, net_send return.
        # These are LIVE gauges, so the collector keeps the LAST value seen
        # rather than a max -- a max over a signed errno would be meaningless.
        self.last_tel_pret = None
        self.last_tel_prev = None
        self.last_tel_sret = None
        self.max_loading_suppressed = 0
        self.anchor = AnchorEvidence(args.profile)
        self.first_guest_zero_heartbeat = None
        self.guest_deaths = 0
        self.route_reversals = 0
        self.route_retrace_len = None
        self.route_oscillation_reason = None
        # Measured split guest path, sampled by the one-second heartbeat.
        self.guest_progress = deque()
        self.guest_total_distance_walked = 0.0
        self.min_window_displacement = None
        self.guest_stall_window = None
        # split_recovery_6m's marker stream (docs/split_recovery_profile.md).
        # Constructed for every profile so write_result/_finish_normal never
        # have to guard on its existence; only split_recovery_6m feeds it.
        self.recovery = RecoveryEvidence(args.run_id)
        self.offer_piston = SplitOfferPistonEvidence(args.run_id)
        self.offer_piston_completion_at = None
        self.vehicle_smoke = VehicleSmokeEvidence(
            args.run_id, getattr(args, "cw_vehicle_kind", ""), args.expected_build,
            getattr(args, "vehicle_transfer_mode", ""))
        self.vehicle_completion_at = None
        self.vehicle_manifest_data = None
        self.vehicle_manifest_sha256 = None
        self.vehicle_manifest_path = getattr(args, "vehicle_manifest", "")
        if args.profile == VEHICLE_SMOKE_PROFILE and self.vehicle_manifest_path:
            try:
                self.vehicle_manifest_data, self.vehicle_manifest_sha256 = vehicle_manifest(
                    self.vehicle_manifest_path, self.vehicle_smoke.kind, args.expected_build)
                self.vehicle_smoke.transfer_mode = self.vehicle_manifest_data["kinds"][self.vehicle_smoke.kind]["transfer_mode"]
                config = self.vehicle_manifest_data["kinds"][self.vehicle_smoke.kind]
                path = Path(config["config_path"])
                if not path.is_absolute():
                    path = Path(self.vehicle_manifest_path).parent / path
                effective = vehicle_json_loads(path.read_text(encoding="utf-8-sig"))
                VehicleSmokeEvidence.require(type(effective) is dict and type(effective.get("gui", {})) is dict, "vehicle_manifest_config_schema")
                hold = effective.get("gui", {}).get("death_hold_respawn_ms", 750)
                VehicleSmokeEvidence.require(type(hold) is int, "vehicle_manifest_ui_policy")
                self.vehicle_smoke.death_hold_respawn_ms = max(0, min(10000, hold))
                shutil.copyfile(self.vehicle_manifest_path, self.out / "vehicle-manifest.json")
            except (ValueError, OSError, TypeError, KeyError) as exc:
                self.fail(str(exc), "vehicle_manifest_failed")
        self.portal_quit = PortalQuitEvidence(args.run_id)
        # Profile foundation: preserve future dimension/actor fields verbatim
        # for replay, without making them a passing semantic claim yet.
        self.split_dims_rows: list[dict] = []
        self.split_dims = SplitDimsEvidence()
        self.pool_selftest = PoolSelftestEvidence()
        self.save_reload = SaveReloadEvidence(args.run_id)
        self.regrow = RegrowEvidence()
        # mp_chat_login_3m's marker stream + server receipt (the B1c contract).
        # Constructed for every profile for the same reason as `recovery` above
        # -- write_result/_finish_normal never have to guard on its existence;
        # only mp_chat_login_3m feeds it.
        self.chat_login = ChatLoginEvidence(args.run_id)
        # mp_chat_chord_1m's own evidence: a different event name and a
        # different contract, so a chord claim can never ride in a login run.
        self.chat_chord = ChatChordEvidence(args.run_id)
        # mp_play_10m's marker stream + the mc173-diag fixture receipts
        # (docs/mp_play_profile.md). Constructed for every profile for the same
        # reason as the three above -- write_result/_finish_normal never have to
        # guard on its existence; only mp_play_10m feeds it.
        self.mp_play = MpPlayEvidence(args.run_id)
        self.mp_pause_menu = MpPauseMenuEvidence(args.run_id)
        # hw1029b/G1: per-packet-kind wire counters (main.c "mp_kinds" rows,
        # server_remote.h enum server_remote_kind). RECORDED ONLY, never
        # graded: the mc173-diag fixture has no path that sends 0x11/0x27, so
        # no profile can require them yet. Max per key across the run (the
        # device counters restart at 0 on every connect generation).
        self.mp_kinds = {}
        # Checks this run could not grade, and why -- named in the artifact so a
        # PASS states its own scope. Only ever populated by a rule that KNOWS
        # the evidence cannot exist on this platform (see _finish_normal's PC
        # branch); an absent reading that merely might exist stays a FAIL.
        self.not_graded: list[str] = []
        self.storage = StorageEvidence()
        self.overlay_spill = OverlaySpillEvidence()  # SPLIT-SPILL: diagnostic only
        self.max_frame_counter = 0
        # T-c (2026-09-03, Round 3 (2)(a) MEM2 gauge fix): running MINIMUM
        # (the worst/lowest headroom observed), not max -- -1 (no carve, or
        # PC) is excluded from tracking so it can never masquerade as "0 KB
        # free, about to spill".
        self.min_mem2_carve_free_kb: int | None = None
        self.walk_min_x: float | None = None
        self.walk_max_x: float | None = None
        self.walk_min_z: float | None = None
        self.walk_max_z: float | None = None
        self.dump_last_seen: float | None = None
        self.dumps: dict[tuple[str, str, int], list[str | None]] = defaultdict(list)

    def write_event(self, event: dict) -> None:
        with self.telemetry_path.open("a", encoding="utf-8") as f:
            f.write(json.dumps(event, separators=(",", ":")) + "\n")
        self.latest = event
        self.latest_path.write_text(json.dumps(event, indent=2), encoding="utf-8")

    def note_light_queue(self, hb: dict) -> str | None:
        """G9 R6: record the light-queue maxima; return a FAIL reason on a
        malformed array, a layout change mid-run, or any light stop /
        ceiling / OOM / drop / post-drain debt. Without --require-g9-evidence
        a heartbeat lacking "lq" (a .dol predating it) is ignored and the
        13-entry v1 layout is still judged by its own rules (explicitly older
        artifacts); with it, every heartbeat must carry the 26-entry v2."""
        problem = self.note_client_light(hb)
        if problem:
            return problem
        lq = hb.get("lq")
        if lq is None:
            if self.require_g9_evidence:
                return f"G9 evidence missing: heartbeat without lq at tick {hb.get('tick')}"
            return None
        if type(lq) is not list or len(lq) not in (len(LQ_FIELDS), len(LQ2_FIELDS)):
            return f"malformed heartbeat lq array: {lq!r}"
        version = 2 if len(lq) == len(LQ2_FIELDS) else 1
        limit = LQ2_MAX if version == 2 else 0xffffffff
        if any(type(v) is not int or not 0 <= v <= limit for v in lq):
            return f"malformed heartbeat lq array: {lq!r}"
        if version == 1 and self.require_g9_evidence:
            return "G9 evidence: an older 13-entry lq array where v2 is required"
        if self.lq_version and version != self.lq_version:
            return f"malformed heartbeat lq array: layout v{version} after v{self.lq_version}"
        self.lq_version = version
        if self.lq_max is None:
            self.lq_max = list(lq)
        else:
            self.lq_max = [max(a, b) for a, b in zip(self.lq_max, lq)]
        names = LQ2_FIELDS if version == 2 else LQ_FIELDS
        fails = LQ2_FAIL_FIELDS if version == 2 else LQ_FAIL_FIELDS
        bad = [f"{name}={lq[names.index(name)]}" for name in fails
               if lq[names.index(name)] != 0]
        if bad:
            return ("G9 light queue (server queue / process-wide counters): "
                    + " ".join(bad) + f" at tick {hb.get('tick')}"
                    " (post-drain debt, a light stop, drain ceiling/OOM, checkpoint"
                    " OOM, server flood ceiling or stack drop)")
        return None

    def note_client_light(self, hb: dict) -> str | None:
        """LIGHT-CLIENT-LOSSLESS: the CLIENT render relight's own counters,
        "clq":[cfceil, coom, cpark, crep], split from lq[24]/lq[25] so a client
        fault is attributed to the client relight, never the server queue.
        Emitted only when non-zero, so absence means all zero. cfceil (a flood
        parked at the 500k ceiling) and coom (a failed client grow) FAIL the
        run even though the work was kept and repaired: either one means the
        cascade or the heap pressure is back. cpark/crep are recorded."""
        clq = hb.get("clq")
        if clq is None:
            return None
        if (type(clq) is not list or len(clq) != len(CLQ_FIELDS)
                or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in clq)):
            return f"malformed heartbeat clq array: {clq!r}"
        prev = getattr(self, "clq_max", None)
        self.clq_max = list(clq) if prev is None else [max(a, b) for a, b in zip(prev, clq)]
        self.clq_last = list(clq)
        bad = [f"{name}={clq[CLQ_FIELDS.index(name)]}" for name in CLQ_FAIL_FIELDS
               if clq[CLQ_FIELDS.index(name)] != 0]
        if bad:
            return ("client render relight (not the server queue): " + " ".join(bad)
                    + f" cpark={clq[2]} crep={clq[3]} at tick {hb.get('tick')}"
                    " (a client flood parked at the 500k ceiling, or a client"
                    " frontier grow failed; work kept as column repairs)")
        return None

    def light_queue_summary(self) -> dict | None:
        if self.lq_max is None:
            return None
        names = LQ2_FIELDS if self.lq_version == 2 else LQ_FIELDS
        out = dict(zip(names, self.lq_max))
        out["layout"] = self.lq_version
        clq_max = getattr(self, "clq_max", None)
        if clq_max is not None:  # LIGHT-CLIENT-LOSSLESS: client relight, split
            out["client"] = dict(zip(CLQ_FIELDS, clq_max))
            out["client"]["cpark_last"] = self.clq_last[2]
        return out

    def menu_ap_fail_text(self) -> str:
        """The walker's named abandonment, for a menus_dwell FAIL reason.

        map_fail is 1 + the step ordinal that gave up (np/walk/cworld
        failed step when one was named, else the running step; stalled =
        the walker's 30 s no-progress guard). Empty when none arrived."""
        f = self.menu_ap_fail
        if not f:
            return ""
        try:
            at = int(f.get("map_fail", 0) or 0) - 1
        except (TypeError, ValueError):
            at = -1
        return (f"; walker ABANDONED at step {at} (menu_ap_fail: "
                f"step={f.get('step')} sub={f.get('sub')} "
                f"stalled={f.get('stalled')} "
                f"np_failed_step={f.get('np_failed_step')} "
                f"np_failed_sub={f.get('np_failed_sub')} "
                f"walk_failed_step={f.get('walk_failed_step')} "
                f"cworld_failed_step={f.get('cworld_failed_step')} "
                f"cworld_sub={f.get('cworld_sub')} "
                f"screen={f.get('screen')})")

    def note_save_failures(self, hb: dict) -> None:
        """Warn (never fail) when the heartbeat's chunk-save failure counter rises."""
        try:
            n = int(hb.get("save_fail_n"))
        except (TypeError, ValueError):
            return
        if n > self.save_fail_seen:
            line = (f"save_fail_n rose {self.save_fail_seen}->{n} at tick "
                    f"{hb.get('tick')} (stage={hb.get('save_fail_stage')} "
                    f"sub={hb.get('save_fail_sub')} idx_fb={hb.get('save_idx_fb')}): "
                    "warning only; a chunk save failed during this run")
            self.save_fail_warnings.append(line)
            print(f"[WARN] {line}", flush=True)
        self.save_fail_seen = n

    def _note_tnt_evidence(self, beat: dict) -> str | None:
        keys = ("tnt_valid", "tnt_st", "tnt_bits", "tnt_primed_left",
                "tnt_carved", "tnt_pad", "tnt_gen", "tnt_tick")
        if any(type(beat.get(key)) is not int or not 0 <= beat[key] <= 0xffffffff
               for key in keys):
            return "TNT snapshot has missing, non-integer or out-of-range fields"
        valid, st, bits = (beat[key] for key in keys[:3])
        left, carved, pad, gen, tick = (beat[key] for key in keys[3:])
        if valid not in (0, 1) or st > 4 or bits > 7 or carved > pad:
            return "TNT snapshot has invalid state, bits or counts"
        if valid == 0:
            return None  # bounded take unavailable; never a pending transition
        if st == 0 and (bits or left or carved or pad or tick):
            return "inactive TNT snapshot has nonzero payload"
        if st == 1 and (bits or left or carved or pad or tick == 0):
            return "pending TNT snapshot has inconsistent payload"
        if st == 2 and (bits != 7 or left != 0 or carved < 8 or tick == 0):
            return "TNT PASS snapshot lacks all three checks or valid counts"
        if st == 3 and (bits == 7 or tick == 0):
            return "TNT FAIL snapshot contradicts its check bits"
        if st == 4 and (bits or left or carved or pad or tick == 0):
            return "TNT SKIP snapshot has inconsistent payload"
        if self.tnt_terminal is not None:
            if st < 2:
                return "TNT terminal regressed to inactive or pending"
            if (gen, st, bits, left, carved, pad, tick) != self.tnt_terminal:
                return "TNT terminal payload or generation changed"
        elif st >= 2:
            self.tnt_terminal = (gen, st, bits, left, carved, pad, tick)
        if st in (3, 4):
            return "TNT fixture reported FAIL or SKIP"
        return None

    MM_KEYS = ("mm_valid", "mm_st", "mm_bits", "mm_recur_ok", "mm_recur_bad",
               "mm_rise_milli", "mm_step_milli", "mm_gen", "mm_tick")
    MM_COST_KEYS = ("mm_moves", "mm_move_us", "mm_env_walks", "mm_env_ents")

    def _note_mob_motion_evidence(self, beat: dict) -> str | None:
        """mob_motion_2m: validate one beat's fixture snapshot. The keys are
        absent until the rig publishes (about 5 s after load); the finish gate
        requires the terminal PASS. Mirrors _note_tnt_evidence."""
        if not any(key in beat for key in self.MM_KEYS):
            if self.mm_terminal is not None:
                return "mob motion evidence vanished after its terminal verdict"
            return None
        if any(type(beat.get(key)) is not int or not 0 <= beat[key] <= 0xffffffff
               for key in self.MM_KEYS):
            return "mob motion snapshot has missing, non-integer or out-of-range fields"
        (valid, st, bits, rok, rbad, rise, step, gen, tick) = (
            beat[key] for key in self.MM_KEYS)
        if valid != 1 or st > 4 or bits > 7:
            return "mob motion snapshot has invalid valid/state/bits"
        counts = (rok, rbad, rise, step)
        if st == 0:
            return "mob motion snapshot published as inactive"
        if st == 1 and (bits or any(counts) or tick == 0):
            return "pending mob motion snapshot has inconsistent payload"
        if st == 2:
            if bits != 7:
                return f"mob motion PASS lacks a check: bits={bits}"
            if rbad != 0:
                return (f"mob motion PASS with {rbad} recurrence miss(es): a clean "
                        f"airborne tick left Beta's (vy-0.08)*0.98F integrator")
            if rok < 8:
                return f"mob motion PASS with only {rok} clean recurrence ticks (< 8)"
            if rise < 500:
                return f"mob motion PASS with TNT lift {rise} mblocks (< 500)"
            if step != 1000:
                return f"mob motion PASS with step landing {step} mblocks (!= 1000)"
            if tick < 400:
                return f"mob motion PASS before the verdict tick (tick={tick})"
        if st == 3 and bits == 7:
            return "mob motion FAIL snapshot contradicts its check bits"
        if st == 4 and (bits or any(counts)):
            return "mob motion SKIP snapshot has inconsistent payload"
        payload = (gen, st, bits, rok, rbad, rise, step, tick)
        if self.mm_terminal is not None:
            if payload != self.mm_terminal:
                return "mob motion terminal payload or generation changed"
        elif st >= 2:
            self.mm_terminal = payload
        if st == 3:
            return (f"mob motion fixture reported FAIL: bits={bits} "
                    f"(lift={bits & 1} recur={(bits >> 1) & 1} step={(bits >> 2) & 1}) "
                    f"recur ok={rok} bad={rbad} rise={rise} step={step}")
        if st == 4:
            return "mob motion fixture reported SKIP (spawn or TNT prime failed)"
        return None

    def _note_mob_motion_cost(self, beat: dict) -> None:
        """Fable S2 F2: sum the mob-move cost window of every beat that carries
        it (absent keys = no mob moved, or a .dol without the counters)."""
        if "mm_moves" not in beat:
            return
        vals = [beat.get(key) for key in self.MM_COST_KEYS]
        if any(type(v) is not int or v < 0 for v in vals):
            return
        moves, us, walks, ents = vals
        c = self.mm_cost
        c["beats"] += 1
        c["moves"] += moves
        c["move_us"] += us
        c["env_walks"] += walks
        c["env_ents"] += ents
        c["max_move_us"] = max(c["max_move_us"], us)
        c["max_moves"] = max(c["max_moves"], moves)

    def note_g9_drain(self, event: dict) -> None:
        records = g9_drain_records(event)
        if records:
            with (self.out / "g9_drain_records.ndjson").open("a", encoding="utf-8") as f:
                for record in records:
                    f.write(json.dumps(record, separators=(",", ":")) + "\n")

    def note_service_observer(self, row):
        """Cumulative poll-drain observer; never grants a hardware gate."""
        self.service_rows.append(row)
        try:
            def uint(v, limit=0xffffffff):
                if type(v) is not int or not 0 <= v <= limit:
                    raise ValueError("schema unsigned integer")
                return v
            for key in ("seq", "lane", "final", "config_id", "resolution_us", "storage"):
                uint(row[key])
            unassigned = uint(row.get("input_unassigned", 0))
            if unassigned < self.service_input_unassigned:
                raise ValueError("unassigned input census regression")
            self.service_input_unassigned = unassigned
            teardown = uint(row.get("teardown_unobserved", 0))
            if teardown < self.service_teardown_unobserved:
                raise ValueError("unobserved teardown census regression")
            self.service_teardown_unobserved = teardown
            uint(row["at_us"], 0xffffffffffffffff)
            uint(row["origin_us"], 0xffffffffffffffff)
            if (row["v"] != 2 or row.get("input_boundary") != "poll_drain"
                    or row["lane"] > 3 or row["final"] > 1
                    or row["clock"] != "monotonic_us" or row["resolution_us"] != 1
                    or row["reserve"] != "time_head_estimate" or row["storage"] > 16384
                    or row["presentation"] not in ("vi_retrace", "render_submit_proxy")
                    or not row["config_id"] or not row["storage"]
                    or row["at_us"] < row["origin_us"]
                    or not isinstance(row["build"], str) or not row["build"]):
                raise ValueError("schema header")
            identity = (row["run_id"], row["build"], row["config_id"], row["origin_us"],
                        row["clock"], row["presentation"], row["storage"])
            if row["run_id"] != self.args.run_id:
                raise ValueError("identity run")
            if self.service_identity is not None and identity != self.service_identity:
                raise ValueError("identity changed")
            self.service_identity = identity
            if row["seq"] != self.service_sequence + 1:
                self.service_errors.append("sequence gap/duplicate")
            self.service_sequence = row["seq"]
            if row["at_us"] < self.service_at_us:
                raise ValueError("clock regression")
            self.service_at_us = row["at_us"]
            if row["lane"] in self.service_final:
                raise ValueError("data after final lane")
            loss = row["loss"]
            if not isinstance(loss, list) or len(loss) != 4:
                raise ValueError("schema loss")
            for key, value in zip(("missing", "overflow", "dropped", "counter_overflow"), loss):
                if uint(value):
                    self.service_errors.append(f"{key}={value}")
            for key, width in (("metrics", 24), ("context", 20), ("states", 3), ("input", 7)):
                if not isinstance(row[key], list):
                    raise ValueError("schema " + key)
                for record in row[key]:
                    if not isinstance(record, list) or len(record) != width:
                        raise ValueError("schema " + key + " width")
                    for v in record:
                        uint(v)
            if not isinstance(row["audio"], list) or len(row["audio"]) not in (0, 14):
                raise ValueError("schema audio")
            for v in row["audio"]:
                uint(v)
            for m in row["metrics"]:
                mid, count = m[:2]
                expected_lane = 2 if mid == 38 else 3 if mid == 7 else 1 if mid >= 8 else 0
                if mid >= 39 or expected_lane != row["lane"] or sum(m[16:]) != count or m[4] > m[5]:
                    raise ValueError("schema metric census")
                old = self.service_metrics.get(mid)
                if old and (count < old[1] or any(a < b for a, b in zip(m[16:], old[16:]))):
                    raise ValueError("schema cumulative metric regression")
                self.service_metrics[mid] = m
            for r in row["states"]:
                if r[0] >= 5:
                    raise ValueError("schema state")
                old = self.service_states.get(r[0])
                if old and any(a < b for a, b in zip(r[1:], old)):
                    raise ValueError("schema cumulative state regression")
                self.service_states[r[0]] = r[1:]
            for r in row["input"]:
                if r[0] >= 9:
                    raise ValueError("schema device")
                old = self.service_input.get(r[0])
                if old and any(a < b for a, b in zip(r[1:], old)):
                    raise ValueError("schema cumulative input regression")
                self.service_input[r[0]] = r[1:]
            if row["audio"]:
                self.service_audio = row["audio"]
            self.service_context.extend(row["context"])
            if row["final"]:
                end = uint(row["tick_end_us"], 0xffffffffffffffff)
                completed = uint(row["completed_ticks"])
                if ((completed and not row["origin_us"] <= end <= row["at_us"])
                        or (not completed and end)):
                    raise ValueError("schema completed tick boundary")
                previous = next((r for r in self.service_rows[:-1] if r.get("final")), None)
                if previous and (end, completed) != (previous.get("tick_end_us"), previous.get("completed_ticks")):
                    raise ValueError("completed tick boundary changed")
                if row["lane"] in self.service_final:
                    raise ValueError("duplicate final lane")
                self.service_final.add(row["lane"])
                if row["lane"] == 0 and (len(row["states"]) != 5
                        or len(row["input"]) != 9 or len(row["audio"]) != 14):
                    raise ValueError("missing final state/input/audio census")
        except (KeyError, ValueError, TypeError) as exc:
            self.service_errors.append("schema: " + str(exc))

    def service_observer_report(self):
        names = ["frame_gap", "poll_gap", "poll_duration", "poll_drain_consume_age",
                 "reused_consume_age", "audio_service_gap", "audio_reserve_estimate",
                 "poll_drain_presentation_age", "tick_us", "rt_us", "wt_us", "boost_us"]
        metrics = {}
        for mid, m in self.service_metrics.items():
            name = names[mid] if mid < 12 else f"phase_{mid-12}" if mid < 35 else f"wait_{mid-35}" if mid < 38 else "boost_capped_us"
            metrics[name] = {"count": m[1], "total_us": m[2] | (m[3] << 32),
                             "min_us": m[4], "max_us": m[5], "bins": m[16:],
                             "maximum": dict(zip(("at_lo", "at_hi", "frame", "tick", "sample",
                                                   "device", "owner", "action", "state", "begin_lo"), m[6:16]))}
            metrics[name]["maximum"]["at_us"] = m[6] | (m[7] << 32)
            if mid == 6:
                metrics[name]["minimum"] = metrics[name].pop("maximum")
        errors = list(dict.fromkeys(self.service_errors))
        if self.service_final != {0, 1, 2, 3}:
            errors.append("missing final flush lanes")
        final_tick = next((r for r in self.service_rows if r.get("final") and r.get("lane") == 1
                           and type(r.get("tick_end_us")) is int
                           and type(r.get("completed_ticks")) is int), {})
        tick_interval = {"begin_us": final_tick.get("origin_us"),
                         "end_us": final_tick.get("tick_end_us"),
                         "completed_ticks": final_tick.get("completed_ticks"),
                         "partial_tick_included": False}
        tick = self.service_metrics.get(8)
        if tick and (tick[1] != tick_interval["completed_ticks"]
                     or (tick[6] | (tick[7] << 32)) > (tick_interval["end_us"] or 0)):
            errors.append("completed tick interval mismatch")
        for name in ("frame_gap", "poll_gap", "poll_duration", "poll_drain_consume_age",
                     "poll_drain_presentation_age", "audio_service_gap", "audio_reserve_estimate", "tick_us", "rt_us", "wt_us"):
            if name not in metrics:
                errors.append("missing observations: " + name)
        if set(self.service_states) != set(range(5)) or set(self.service_input) != set(range(9)) or len(self.service_audio) != 14:
            errors.append("missing final state/input/audio census")
        # Zero slots are expected for states this profile never entered.
        # Cross-check actual metric/context observations so erasing a census
        # count cannot turn an observed state into an absent one.
        observed_states = {0} | {state for state, values in self.service_states.items() if any(values)}
        observed_states.update(m[14] for m in self.service_metrics.values())
        observed_states.update(r[3] for r in self.service_context)
        observed_states.update(r[6] for r in self.service_context if r[0] == 1)
        for state in sorted(observed_states):
            values = self.service_states.get(state, [0, 0])
            if not values[0] or (state != 4 and not values[1]):
                errors.append(f"missing state observations: {state}")
        if not any(v[0] and v[5] for v in self.service_input.values()):
            errors.append("missing fresh consumed input observations")
        if any(v[5] and not v[0] for v in self.service_input.values()):
            errors.append("consumed device without fresh input observations")
        if len(self.service_audio) == 14 and not (self.service_audio[10] | self.service_audio[11]):
            errors.append("missing active audio duration")
        # Correlate complete intervals, rather than accepting active_us from
        # unrelated music earlier/later. State-changing backend edges include
        # initial prime, stop, track stop and shutdown; decode/exhaustion rows
        # explicitly have d!=0 and are not state transitions.
        contexts = sorted(self.service_context, key=lambda r: r[1] | (r[2] << 32))
        audio_edges = [(r[1] | (r[2] << 32), bool(r[8])) for r in contexts
                       if r[0] == 6 and r[19] == 0]
        def active_across(begin, end):
            before = [active for at, active in audio_edges if at <= begin]
            return bool(before and before[-1] and end >= begin
                        and not any(not active for at, active in audio_edges if begin < at <= end))
        caps = [r for r in contexts if r[0] == 4]
        checkpoints = {}
        for r in contexts:
            if r[0] & 0x80000000:
                errors.append("missing correlated context")
            if r[0] == 5:
                checkpoints.setdefault((r[6], r[7]), []).append(r)
        if not caps or not checkpoints:
            errors.append("missing cap/checkpoint context")
        for r in caps:
            end = r[1] | (r[2] << 32)
            begin = end - r[19]
            if (begin < 0 or (begin & 0xffffffff) != r[8] or not r[18]
                    or not active_across(begin, end)):
                errors.append("music inactive/unobserved across cap interval")
        for key, events in checkpoints.items():
            begin, end = events[0], events[-1]
            if begin[19] != 0 or end[19] != 2:
                errors.append(f"incomplete checkpoint context: {key}")
            if (any(not r[18] for r in events)
                    or not active_across(begin[1] | (begin[2] << 32), end[1] | (end[2] << 32))):
                errors.append("music inactive/unobserved across checkpoint interval")
        return {"version": 2, "rows": len(self.service_rows), "identity": self.service_identity,
                "metrics": metrics, "states": self.service_states, "input": self.service_input,
                "tick_interval": tick_interval,
                "audio": self.service_audio, "context": self.service_context, "errors": errors,
                "audio_fields": ["service", "initial_prime", "backend_underrun", "backend_stop", "reprime",
                                 "decode_underrun", "reset_initial_or_track_change", "rate_change",
                                 "normal_track_stop", "shutdown_stop",
                                 "active_us_lo", "active_us_hi", "inactive_observations", "estimated_exhaustion_intervals"],
                "action_bits": {1: "camera/look", 2: "movement consumer", 4: "button consumer (press)", 8: "button consumer (release)"},
                "state_names": ["startup", "loading", "gameplay", "menu", "shutdown"],
                "context_fields": ["kind", "at_lo", "at_hi", "state", "frame", "tick", "a", "b", "c",
                                   "latest_frame_at_lo", "latest_frame", "poll_end_lo", "sample",
                                   "receipt_lo", "consume_lo", "device_owner_action", "audio_at_lo",
                                   "reserve_us", "audio_active", "d"],
                "context_kinds": {1: "state", 2: "worst_tick/components", 3: "boost", 4: "actual_cap",
                                  5: "checkpoint", 6: "audio", 8: "first_display"},
                "context_missing_bit": 0x80000000,
                "estimated_audio_exhaustion": "elapsed active interval exceeds prior estimated reserve; distinct from backend underrun",
                "final_seen": self.service_final == {0, 1, 2, 3}, "coverage_complete": not errors,
                "presentation_proven": False, "hardware_verified": False,
                "coverage_scope": "received poll-drain rows and observed burst intervals only; post-profile teardown unproven; not hardware acceptance",
                "presentation_boundary": self.service_identity[5] if self.service_identity else None,
                "input_boundary": "poll_drain",
                "input_unassigned_consumers": self.service_input_unassigned,
                "teardown_unobserved_calls": self.service_teardown_unobserved,
                "receipt_latency_proven": False,
                "receipt_semantics": "driver poll drain/sample time; report arrival and physical button actuation unmeasured",
                "unsupported_writer_loss": "non-main/non-server phase/wait writers set a sticky missing flag (lower bound); MAIN rejects counted exactly outside explicit shutdown; shutdown MAIN calls are unobserved, not tick samples",
                "decode_underrun_timestamps": "heartbeat detection proxy; counts from real decode producer",
                "bin_upper_us": [1000, 5000, 10000, 20000, 50000, 100000, 250000, None],
                "server_gate_us": 50000, "overhead_subtracted": False}

    def note_service_exposure_heartbeat(self, event):
        """Conservative consecutive admitted/alive heartbeat intervals.

        Exclude loading, pause, ACK, residency, transfer and teardown using the
        actual guard v2 predicate. Missing/discontinuous intervals earn no time;
        poll-drain input evidence still cannot prove physical receipt latency.
        """
        context = self.perf_guard_context
        guard = context and context.get("summary")
        death = event.get("d1")
        eligible = bool(guard and guard[0] == 2 and guard[2] and guard[3]
                        and context["heartbeat_tick"] == event.get("tick")
                        and not context.get("barriers") and death and death[0]
                        and death[1] == death[3] == 0
                        and type(event.get("player_health")) is int and event["player_health"] > 0
                        and event.get("world_loaded") is True
                        and event.get("loading_screen") is False)
        tick, at = event.get("tick"), event.get("up_ms")
        valid = type(tick) is int and type(at) is int and death and len(death) == 5
        previous = self.service_live_previous
        if valid and eligible and previous and previous[3] and death[0] == previous[2]:
            gap = at - previous[1]
            if tick == previous[0] + 1 and 0 < gap <= self.args.stale_timeout * 1000:
                self.service_live_ms += gap
        self.service_live_previous = (tick, at, death[0], eligible) if valid else None

    def service_exposure_report(self):
        report = self.service_observer_report()
        scheduled = {}
        for row in report["context"]:
            if row[0] == 5 and row[8] > 0 and row[8] % 600 == 0:
                scheduled.setdefault((row[6], row[7], row[8]), []).append(row[19])
        # Different tokens retrying one logical checkpoint do not buy exposure.
        durable = {(gen, tick) for (gen, token, tick), states in scheduled.items()
                   if states[0] == 0 and states[-1] == 2}
        observed_gameplay_us = report["states"].get(2, [0, 0])[1]
        live_ms = min(self.service_live_ms, observed_gameplay_us // 1000)
        errors = list(report["errors"])
        if self.args.profile != "perf_stress_5m":
            errors.append("service diagnostic requires the unchanged stress fixture")
        if live_ms < 300000:
            errors.append(f"admitted/alive gameplay {live_ms}ms < 300000ms")
        if len(durable) < 8:
            errors.append(f"ordinary scheduled durable checkpoints {len(durable)} < 8")
        return {"required": bool(getattr(self.args, "require_service_exposure", False)),
                "admitted_alive_ms": live_ms, "ordinary_durable_checkpoints": len(durable),
                "checkpoint_cadence_ticks": 600, "errors": errors,
                "complete": not errors, "hardware_acceptance": False}

    def write_g9_capture(self) -> None:
        report = self.g9_capture.report()
        (self.out / "g9_capture.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        (self.out / "g9_capture_table.md").write_text(render_g9_capture(report), encoding="utf-8")

    def write_g9_cpu(self) -> None:
        report = self.g9_cpu.report()
        (self.out / "g9_cpu.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        (self.out / "g9_cpu_table.md").write_text(render_g9_cpu(report), encoding="utf-8")

    def shutdown_error(self) -> str | None:
        missing = []
        if self.shutdown_done is None:
            missing.append("profile_done")
        if not any("checkpoint_shutdown_us" in row for row in self.shutdown_rows):
            missing.append("checkpoint_shutdown_us")
        errors = self.shutdown_errors + (["missing " + ", ".join(missing)] if missing else [])
        return "; ".join(errors) if errors else None

    def finish_shutdown_if_ready(self) -> None:
        if (self.shutdown_done is not None
                and any("checkpoint_shutdown_us" in row for row in self.shutdown_rows)
                and not self.tps_pending):
            problem = self.shutdown_error()
            if problem:
                self.fail(problem, "checkpoint_shutdown_failed")
            else:
                self._finish_normal("profile_done received; checkpoint shutdown drained")

    def process(self, event: dict) -> None:
        # L225: wf is count / device-monotonic-window FPS. The two-character
        # wire key replaces fps to retain the fixed sender-slot spare floor.
        # Normalize a copy for existing consumers/gates; old tapes pass through.
        if isinstance(event, dict) and event.get("event") == "heartbeat" and "wf" in event:
            event = {**event, "fps": event["wf"]}
        if self.vehicle_pc_replay and not self.vehicle_pc_ingesting:
            self.fail("vehicle_pc_replay_requires_verbatim_parser", "vehicle_smoke_evidence_failed")
            return
        self.bad_lines_consecutive = 0
        if (isinstance(event, dict) and self.args.profile == WORLD_COPY_PROFILE
                and event.get("event") in WORLD_COPY_MARKERS):
            self.wcc_rows.append(dict(event))
        if self.args.profile == OFFER_PISTON_PROFILE:
            if self.fail_reason:
                return
            name = event.get("event", "") if isinstance(event, dict) else ""
            if not isinstance(name, str):
                self.fail("event_schema", "split_offer_piston_evidence_failed")
                return
            # A heartbeat fault waits up to 5 s for the device's tel_drop_names
            # attribution so the failure names the refused producers; the run
            # fails either way.
            pending = getattr(self, "offer_piston_fault_pending", None)
            if pending is not None:
                if name == "tel_drop_names":
                    self.write_event(event)
                    self.fail("heartbeat_fault: dropped " + json.dumps(event.get("names"), sort_keys=True),
                              "split_offer_piston_evidence_failed")
                    return
                if time.monotonic() - pending > 5.0 or name == "profile_done":
                    self.fail("heartbeat_fault (no tel_drop_names within 5 s)",
                              "split_offer_piston_evidence_failed")
                    return
            if name.startswith("split_offer_piston"):
                self.write_event(event)
                self.offer_piston.note(event)
                if self.offer_piston.error:
                    self.fail(self.offer_piston.error, "split_offer_piston_evidence_failed")
                return
            if name == "profile_done":
                self.write_event(event)
                if event.get("run_id") != self.args.run_id:
                    self.fail("run_profile_identity", "split_offer_piston_evidence_failed")
                    return
                self.offer_piston.note_profile_done(event.get("status"))
                if self.offer_piston.error:
                    self.fail(self.offer_piston.error, "split_offer_piston_evidence_failed")
                else:
                    self.offer_piston_completion_at = time.monotonic()
                return
            if name == "heartbeat":
                if (event.get("run_id") != self.args.run_id or event.get("profile") != OFFER_PISTON_PROFILE
                        or not self.args.expected_build or event.get("build_id") != self.args.expected_build):
                    self.fail("heartbeat_identity", "split_offer_piston_evidence_failed")
                    return
                if any(event.get(k, 0) != 0 for k in ("oom", "tel_fail", "save_fail_n")):
                    self.fail("heartbeat_fault", "split_offer_piston_evidence_failed")
                    return
                if event.get("tel_drop", 0) != 0 and getattr(self, "offer_piston_fault_pending", None) is None:
                    self.offer_piston_fault_pending = time.monotonic()
            if name in ("FAULT", "HANG", "fault", "hang", "dump", "dump_begin", "dump_chunk", "dump_end"):
                self.fail("fault_or_dump", "split_offer_piston_evidence_failed")
                return
        if self.args.profile == VEHICLE_SMOKE_PROFILE:
            if self.fail_reason:
                return
            name = event.get("event", "") if isinstance(event, dict) else ""
            if not isinstance(event, dict):
                self.fail("vehicle_schema: top-level object required", "vehicle_smoke_evidence_failed")
                return
            if not isinstance(name, str) or not name:
                self.fail("vehicle_schema: event must be string", "vehicle_smoke_evidence_failed")
                return
            if (name.lower() == "heartbeat" or ("tick" in event and "fps" in event)) and name != "heartbeat":
                self.fail("vehicle_heartbeat_schema: malformed heartbeat event name", "vehicle_smoke_evidence_failed")
                return
            if name.startswith("vehicle_smoke") or any(k in event for k in ("schema", "op_id", "origin")):
                self.write_event(event)
                self.vehicle_smoke.note(event)
                if self.vehicle_smoke.error:
                    self.fail(self.vehicle_smoke.error, "vehicle_smoke_evidence_failed")
                return
            if name == "profile_done":
                self.write_event(event)
                if event.get("run_id") != self.args.run_id:
                    self.fail("vehicle_stale_run", "vehicle_smoke_evidence_failed")
                    return
                self.vehicle_smoke.note_profile_done(event.get("status"))
                if self.vehicle_smoke.error:
                    self.fail(self.vehicle_smoke.error, "vehicle_smoke_evidence_failed")
                else:
                    self.vehicle_completion_at = time.monotonic()
                return  # Read through quiescence/EOF, including duplicate completions.
            if name == "heartbeat":
                try:
                    VehicleSmokeEvidence.validate(event.get("tick"), "u32", "heartbeat.tick")
                    VehicleSmokeEvidence.validate(event.get("fps"), "f", "heartbeat.fps")
                    VehicleSmokeEvidence.require(event["fps"] >= 0, "vehicle_heartbeat_schema: negative fps")
                    for key in ("oom", "tel_drop", "tel_fail", "save_fail_n"):
                        VehicleSmokeEvidence.validate(event.get(key, 0), "u32", "heartbeat." + key)
                except VehicleSmokeError as exc:
                    self.fail("vehicle_heartbeat_schema: " + str(exc), "vehicle_smoke_evidence_failed")
                    return
                if (event.get("run_id") != self.args.run_id or event.get("profile") != VEHICLE_SMOKE_PROFILE
                        or not self.args.expected_build or event.get("build_id") != self.args.expected_build):
                    self.fail("vehicle_heartbeat_identity", "vehicle_smoke_evidence_failed")
                    return
                if any(event.get(k, 0) != 0 for k in ("oom", "tel_drop", "tel_fail", "save_fail_n")):
                    self.fail("vehicle_heartbeat_fault", "vehicle_smoke_evidence_failed")
                    return
            if name in ("FAULT", "HANG", "fault", "hang", "dump", "dump_begin", "dump_chunk", "dump_end", "watchdog_dump", "fault_dump"):
                self.fail("vehicle_fault_or_dump", "vehicle_smoke_evidence_failed")
                return
        # Alive is diagnostic only: never refresh heartbeat deadlines or PASS.
        # Its 64-byte wire budget omits run_id; the TCP connection supplies scope.
        if event.get("event") == "alive":
            with (self.out / "alive.ndjson").open("a", encoding="utf-8") as f:
                f.write(json.dumps(event) + "\n")
            return
        if (self.require_tnt_evidence and event.get("event") == "heartbeat"
                and (not self.args.run_id or not self.args.expected_build
                     or event.get("run_id") != self.args.run_id
                     or event.get("profile") != self.args.profile
                     or not isinstance(event.get("build_id"), str)
                     or not event.get("build_id")
                     or not build_stamps_match(event["build_id"],
                                               self.args.expected_build))):
            self.write_event(event)
            self.fail("TNT heartbeat run/profile/build identity missing or mismatched",
                      "tnt_evidence_failed")
            return
        if self.args.run_id and event.get("run_id") != self.args.run_id:
            if self.args.profile == "split_dims_6m":
                self.fail("stale split dimensions run_id", "split_dims_stale_run")
            return
        if self.done and self.args.profile == "split_play_8m":
            return  # A measured P2 death can never be overwritten by profile_done.
        self.write_event(event)
        kind = event.get("event")
        if kind == "overlay_spill":
            self.overlay_spill.note(event)  # never graded; falls through unchanged
        if self.args.profile == "perf_stress_5m":
            if kind == "region_journal":
                rio = event.get("rio")
                if isinstance(rio, list) and rio and rio[0] == 1:
                    # Match main.c's emission condition: deferred IO only.
                    self.shutdown_required = True
                if "ckio" in event:
                    self.shutdown_last_counters = dict(event)
                for key in ("checkpoint_final_drain_us", "checkpoint_shutdown_us"):
                    if key in event:
                        self.shutdown_required = True
                        self.shutdown_rows.append(dict(event))
                        if type(event[key]) is not int or event[key] < 0:
                            self.shutdown_errors.append(f"invalid {key}")
                        if type(event.get("ok")) is not int or event["ok"] != 1:
                            self.shutdown_errors.append(f"{key} ok={event.get('ok')!r}")
                        self.finish_shutdown_if_ready()
                        return
            if kind == "profile_done" and self.shutdown_required:
                if self.shutdown_done is not None:
                    self.shutdown_errors.append("duplicate profile_done")
                self.shutdown_done = dict(event)
                if event.get("status") != "ok":
                    self.shutdown_errors.append(f"profile_done status={event.get('status')!r}")
                # Retain failure status AND drain timing, including reordered
                # UDP delivery; neither duration nor an earlier zero backlog
                # supplies lifecycle completion evidence.
                self.finish_shutdown_if_ready()
                return
        if kind in COLUMN_POOL_EVENTS and self.args.profile in ANCHOR_PROFILES:
            # colcap-1006 (ASTRA-CC-R3 item 2): every anchor profile now
            # reconstructs the column pool from its grow/shrink events.
            self.anchor.note_pool_event(event)
        if kind == "column_pool_grow" and self.args.profile == "split_dims_6m":
            self.split_dims.note_pool_grow(event)
            if self.split_dims.error_class == "split_dims_column_pool_grow_failed":
                self.fail(self.split_dims.error, self.split_dims.error_class)
            return
        if kind in COLUMN_POOL_EVENTS and self.args.profile == REGROW_PROFILE:
            self.regrow.note_event(event)
        if kind == "anchor_cycle":
            if self.args.profile == REGROW_PROFILE:
                self.regrow.note_cycle(event)
            return
        if kind in COLUMN_POOL_EVENTS:
            return
        if kind in ("save_reload", "save_reload_phase"):
            if self.args.profile == SAVE_RELOAD_PROFILE:
                self.save_reload.note(event)
            return
        if kind in ("pool_selftest", "gx_scene_hash"):
            if self.args.profile == POOL_SELFTEST_PROFILE:
                self.pool_selftest.note(event)
            return
        if kind in ("g9_capture", "g9_capture_seal"):
            self.g9_capture.note(event)
            if kind == "g9_capture_seal":
                self.write_g9_capture()
            return
        if kind == "service_observer":
            self.note_service_observer(event)
            return
        if kind == "g9_cpu":
            self.g9_cpu.note(event)
            # Raw rows are already retained in telemetry.ndjson. Materialize
            # tables on span ends, rather than rewrite a growing report for
            # every sample batch while the TCP reader services heartbeats.
            rows = event.get("rows")
            if isinstance(rows, list) and any(
                    isinstance(r, list) and len(r) == 24 and r[2] == 3 for r in rows):
                self.write_g9_cpu()
            return
        if kind == "menu_ap_fail" and self.menu_ap_fail is None:
            self.menu_ap_fail = dict(event)
        if kind in ("heartbeat", "g9_drain_diag"):
            self.note_g9_drain(event)
        if kind == "heartbeat":
            self.g9_cpu.note(event)
            if "g9p" in event or "g9p_v" in event:
                record = g9_pacing_record(event)
                if record is not None:
                    with (self.out / "g9_pacing_records.ndjson").open("a", encoding="utf-8") as f:
                        f.write(json.dumps(record) + "\n")
        if kind == "heartbeat":
            self.note_save_failures(event)
            # Recorded before any profile-specific early return so the live
            # tape the residency gate sees is exactly the tape CLI's.
            rz_rec = residency_tape_record(event)
            if rz_rec is not None:
                self.rz_records.append(rz_rec)
        if self.args.profile == "split_play_8m":
            if kind == "guest_death":
                self.guest_deaths += 1
                return
            if kind == "guest_respawn":
                return
            if kind == "guest_route_reversed":
                self.route_reversals += 1
                self.route_retrace_len = event.get("retrace_len")
                return
            if kind == "guest_route_oscillating":
                self.route_oscillation_reason = str(event.get("reason") or "unknown")
                if not self.done:
                    self.fail("scripted guest route oscillating: " + self.route_oscillation_reason,
                              "guest_route_oscillating")
                return
            if kind == "guest_route_blocked":
                if not self.done:
                    self.fail("scripted guest route blocked after bounded water escape",
                              "guest_route_blocked")
                return
        if kind == "tps_tick":
            self.tps_pending = True
            self.tps_records += 1
            return
        if kind == "tps_summary" and "seq" not in event:
            return  # Legacy census accompanies exact ticks; do not double-count.
        if kind == "tps_window" or kind == "tps_summary":
            self.tps_pending = True
            if kind == "tps_summary":
                count = event.get("count")
                if type(count) is not int or count <= 0:
                    self.fail("invalid TPS summary count", "tps_stream_invalid")
                    return
                self.tps_records += count
            return
        if kind == "tps_end":
            if (type(event.get("records")) is not int
                    or event["records"] != self.tps_records
                    or type(event.get("stream_valid")) is not bool):
                self.fail("TPS seal does not match received aggregates", "tps_stream_invalid")
                return
            self.tps_pending = False
            self.tps_end = dict(event)
            if not event["stream_valid"]:
                # watchdog_inject deliberately quiesces before its HANG dump.
                # Preserve the partial seal, but certify only the expected dump
                # path; ending collection here would truncate that evidence.
                if not self.profile["expect_dump"]:
                    self.fail(f"partial TPS window: {event.get('reason', 'unspecified')}", "tps_partial")
            elif self.shutdown_required:
                self.finish_shutdown_if_ready()
            elif self.tps_profile_done and not self.profile["expect_dump"]:
                self._finish_normal("profile_done received; TPS seal drained")
            return
        if kind == "anchor_frames" and self.args.profile in ANCHOR_PROFILES:
            self.anchor.note_frames(event)
            return
        if kind == "split_recovery":
            # The scripted recovery driver's narration. Only its own profile
            # judges it; on any other profile the record is still written to
            # telemetry.ndjson above and otherwise ignored.
            if self.args.profile == SPLIT_RECOVERY_PROFILE:
                self.recovery.note(event)
            elif self.args.profile == PORTAL_QUIT_PROFILE:
                self.portal_quit.note_recovery(event)
            return
        if kind == PORTAL_QUIT_EVENT:
            if self.args.profile == PORTAL_QUIT_PROFILE:
                self.portal_quit.note(event)
            return
        if kind == "split_portal_quit_server":
            if self.args.profile == PORTAL_QUIT_PROFILE:
                self.portal_quit.note_server(event)
            return
        if kind == CHAT_CHORD_EVENT:
            # The scripted chord driver's narration. Same convention as the
            # chat_login rows below: only its own profile judges it.
            if self.args.profile == CHAT_CHORD_PROFILE:
                self.chat_chord.note(event)
            return
        if kind == MP_KINDS_EVENT:
            note_mp_kinds(self.mp_kinds, event)
            return
        if kind == MP_PLAY_EVENT:
            # The scripted play driver's narration (docs/mp_play_profile.md).
            # Same convention as split_recovery below: only its own profile
            # judges it.
            if self.args.profile == MP_PLAY_PROFILE:
                self.mp_play.note(event)
            return
        if kind == "mp_pause_menu":
            if self.args.profile == "mp_pause_menu_3m":
                self.mp_pause_menu.note(event)
            return
        if kind == "chat_login":
            # The scripted chat-login driver's narration (B1c contract section
            # 3). Same convention as split_recovery above: only its own profile
            # judges it; elsewhere the record is written to telemetry.ndjson and
            # otherwise ignored.
            if self.args.profile == CHAT_LOGIN_PROFILE:
                self.chat_login.note(event)
            # mp_play_10m CALLS the same login machine (Astra answer B3), twice,
            # so the very same rows are its login evidence -- graded by
            # ChatLoginEvidence's own rules, per generation, in its action-only
            # mode (MpPlayEvidence.note_login, REL-14b-0).
            if self.args.profile == MP_PLAY_PROFILE:
                self.mp_play.note_login(event)
            return
        if kind == "carve_census":
            # Fix round brief finding #3 (2026-09-07): tel_drop (observer-ring
            # drops) and staged_skipped (a whole census set skipped because
            # the previous one was still mid-drain) only ever rode on the
            # carve_census row itself -- invisible to write_result()/PASS.json
            # on every profile that doesn't grep the raw NDJSON by hand. Track
            # each as a running max, same treatment as max_tel_fail: both
            # counters are monotonic non-decreasing device-side.
            self.max_tel_drop = max(
                self.max_tel_drop, int(event.get("tel_drop", 0) or 0))
            self.max_staged_skipped = max(
                self.max_staged_skipped,
                int(event.get("staged_skipped", 0) or 0))
            self.max_loading_suppressed = max(
                self.max_loading_suppressed,
                int(event.get("loading_suppressed", 0) or 0))
            return
        now = time.monotonic()

        build = event.get("build_id")
        if (
            self.args.expected_build
            and build
            and not build_stamps_match(build, self.args.expected_build)
        ):
            self.fail(
                f"build stamp mismatch: expected {self.args.expected_build}, got {build}",
                "build_mismatch",
            )
            return

        if kind == "perf_survival_guard":
            problem = self.note_perf_guard_context(event)
            if problem:
                self.fail(*problem)
            return
        if kind == "heartbeat":
            death_problem = self.note_death_telemetry(event)
            if death_problem:
                self.fail(*death_problem)
                return
            guard_problem = self.note_perf_guard(event)
            if guard_problem:
                self.fail(*guard_problem)
                return
            self.note_service_exposure_heartbeat(event)
            problem = self.note_light_queue(event)
            if problem:
                self.fail(problem, "light_queue_fault")
                return
            if self.require_tnt_evidence:
                problem = self._note_tnt_evidence(event)
                if problem:
                    self.fail(problem, "tnt_evidence_failed")
                    return
            self._note_mob_motion_cost(event)
            if self.profile.get("scenario") == "mob_motion":
                problem = self._note_mob_motion_evidence(event)
                if problem:
                    self.fail(problem, "mob_motion_evidence_failed")
                    return
            if self.args.profile == "split_dims_6m":
                if event.get("guest") and (type(event.get("gy")) not in
                                           (int, float) or not math.isfinite(event["gy"])):
                    self.fail("split dimensions guest heartbeat lacks finite gy",
                              "split_dims_guest_y_missing")
                    return
                if getattr(self.args, "require_wii_budget", False) and event.get("budget_wii") is not True:
                    self.fail("staged Wii proof requires compiled Wii budget evidence",
                              "split_dims_budget_platform")
                    return
                self.split_dims_rows.append({k: v for k, v in event.items()
                                             if k.startswith(("dim_", "budget_", "transfer_",
                                                              "column_pool_", "srv_dim_"))
                                             or k in _DIMS_HOST_INTS or k in
                                             ("tick", "guest", "gx", "gy", "gz",
                                              "guest_head_block", "guest_air",
                                              "views_live", "dims_active",
                                              "dims_phase", "host_phase", "guest_commits",
                                              "mem1_free_kb", "mem2_carve_free_kb", "db", "ds")})
                self.split_dims.note(event)
                if (self.split_dims.error_class in ("split_dims_fields_missing",
                                                   "split_dims_inconsistent",
                                                   "split_dims_guest_unbound")
                        or (self.split_dims.error_class or "").startswith((
                            "split_dims_budget_", "split_dims_column_pool_"))):
                    self.fail(self.split_dims.error, self.split_dims.error_class)
                    return
            if self.args.profile == "mp_lan_walk_10m":
                self._note_mp_move_cadence(event)
                if self.done:
                    return
            if self.args.profile == "mp_pause_menu_3m":
                self.mp_pause_menu.heartbeat(event)
            self.storage.note(event)
            if (self.args.profile == "split_play_8m"
                    and event.get("anchor_measuring") is True
                    and isinstance(event.get("act_health"), list)
                    and len(event["act_health"]) == 2
                    and type(event["act_health"][1]) is int
                    and event["act_health"][1] == 0
                    and self.first_guest_zero_heartbeat is None):
                self.first_guest_zero_heartbeat = dict(event)
                self.guest_deaths = max(self.guest_deaths, 1)
                self.fail("P2 health 0 in measured window at heartbeat tick %s"
                          % event.get("tick"), "guest_died_measured")
                return
            if (self.args.profile == "split_play_8m"
                    and event.get("anchor_measuring") is True):
                self.note_guest_progress(event)
                if self.done:
                    return
            if self.args.profile in ANCHOR_PROFILES:
                self.anchor.note(event)
            if self.args.profile == REGROW_PROFILE:
                self.regrow.note_heartbeat(event)
            # Retro 1 corrective action (2026-09-13): five real-Wii
            # mp_lan_walk_10m runs graded PASS while the player died at spawn
            # behind the loading screen (player_health 160 -> 0 by tick 12);
            # the autopilot respawned and kept walking, and the `deaths`
            # heartbeat field is a server_local counter that reads 0 in MP
            # (no server_local player exists client-side), so nothing caught
            # it. Judge by player_health itself: any mp_* profile heartbeat
            # in the first 60 ticks reporting health exactly 0 is a dead-at-
            # spawn run, full stop -- `== 0` also naturally excludes both the
            # -1 no-body sentinel (main.c's hb.player_health when
            # gstate.players[0].local_player is NULL) and a missing field
            # (event.get returns None, and None == 0 is False).
            if (self.args.profile.startswith("mp_")
                    and type(event.get("tick")) is int and event["tick"] <= 60
                    and type(event.get("player_health")) is int
                    and event["player_health"] == 0):
                self.fail(
                    f"player died within the first 60 s (health 0 at tick {event['tick']})",
                    "player_died_early",
                )
                return
            # B-a (Astra Lane B item 2), mp_play_10m and mp_lan_walk_10m ONLY:
            # the cumulative load-ready BACKSTOP counter must be exported and
            # must read zero from the FIRST heartbeat to the LAST, rejoin
            # included -- "no backstop" may only pass on an observed normal
            # release, and a counter that is merely absent proves nothing. Every
            # other profile's verdict logic is untouched by this block.
            if self.args.profile in LW_TIMEOUT_ADMIT_PROFILES:
                admits = event.get("lw_to_admits")
                if type(admits) is not int:
                    self.fail(
                        "heartbeat at tick "
                        f"{event.get('tick')} carries no integer lw_to_admits "
                        "(lworld_ready_timeout_admits): this profile's "
                        '"no backstop" claim is unprovable without it',
                        "lw_timeout_admits_missing",
                    )
                    return
                if admits != 0:
                    self.fail(
                        f"load-ready backstop admitted a world: lw_to_admits="
                        f"{admits} at tick {event.get('tick')}; the counter "
                        "must be 0 from the first heartbeat to the last",
                        "lw_timeout_admits_nonzero",
                    )
                    return
            if self.release_gate:
                problem = release_frame_error(event)
                cumulative = ("perf_frames", "perf_duration_us", "perf_max_us",
                              "perf_over_30ms", "perf_gpu_over_16600",
                              "perf_gpu_over_8000", "perf_invalid")
                if not problem and any(event[k] < self.previous_perf.get(k, 0)
                                       for k in cumulative):
                    problem = "per-frame counters regressed during run"
                if not problem and event["perf_frames"]:
                    for key, floor in (("mem1_free_kb", 4096),
                                       ("mem2_carve_free_kb", 2048)):
                        if type(event.get(key)) is not int or event[key] < floor:
                            problem = f"{key} missing or below release floor"
                    if event.get("oom") != 0 or event.get("mesh_trunc") != 0:
                        problem = "allocation failure or mesh truncation"
                self.release_evidence_error = self.release_evidence_error or problem
                self.previous_perf = {k: event.get(k, 0) for k in cumulative}
            if self.first_hb is None:
                self.first_hb = now
            self.last_hb = now
            self.hb_count += 1
            gap = heartbeat_gap(self.last_heartbeat_event, event)
            if gap is not None:
                self.telemetry_gaps.append(gap)
            self.last_heartbeat_event = dict(event)
            self.max_rt_us = max(self.max_rt_us, int(event.get("srv_rt_us", 0) or 0))
            self.max_wt_us = max(self.max_wt_us, int(event.get("srv_wt_us", 0) or 0))
            # ASTRA-G9-R5 point 9: srv_rt_us/srv_wt_us are the LAST tick's
            # sample; the lqd v2 array (hw_telemetry.h) carries the per-window
            # MAXIMA at 14/15, so every tick of the window is graded by the
            # perf_stress_5m budget, not only the one that preceded the beat
            # (g9_1 caught one of five 1.1-1.7 s checkpoint ticks). A v1
            # 14-entry array (older tapes) has no maxima: the sample stands.
            lqd = event.get("lqd")
            if isinstance(lqd, list) and len(lqd) >= 16:
                if type(lqd[14]) is int:
                    self.max_rt_us = max(self.max_rt_us, lqd[14])
                if type(lqd[15]) is int:
                    self.max_wt_us = max(self.max_wt_us, lqd[15])
            # ci-1101/37: keep the worst drain long-wait attribution seen when
            # a heartbeat carries it, so a FAIL names the contended lock and its
            # holder instead of only the 71 ms figure. Evidence only.
            wait = parse_lqd_wait(event)
            if wait is not None and (self.lqd_wait is None
                                     or wait["wait_us"] > self.lqd_wait["wait_us"]):
                self.lqd_wait = wait
            self.max_oom = max(self.max_oom, int(event.get("oom", 0) or 0))
            self.max_trunc = max(self.max_trunc, int(event.get("mesh_trunc", 0) or 0))
            self.max_ndisp = max(self.max_ndisp, int(event.get("ndisp", 0) or 0))
            self.max_map_step = max(self.max_map_step, int(event.get("map_max", 0) or 0))
            # Walker proof (A11) -- THE UNION OF EVERY BEAT, not the last one
            # that carried the keys (review A16-4). The three masks and the
            # flags are strictly MONOTONIC in the walker (`|=` only, cleared
            # nowhere but menu_ap_script_reset), so OR-ing across every beat is
            # IDENTICAL to the last beat under no loss and strictly safer under
            # loss: last-writer-wins graded a failure raised at second 200,
            # whose beats never arrived, against second 199's clean snapshot --
            # np_flags 0, np_entered already complete, map_max already past the
            # contract -- and PASSED a leg that failed. This rig has a recorded
            # line-drop trap on a loaded host, so that is a real tape, not a
            # forged one.
            #
            # np_rows takes the MAX for the same reason. It is a one-shot latch
            # (the leg sets it at the end of its Select World dwell) and it is
            # never negative on the wire -- menu_autopilot.c:410-415 reports an
            # unlatched -1 as 0 -- so max == last under no loss, while under
            # loss max cannot fall back to the pre-latch 0 that would justify a
            # row-gated Rename/Delete skip the screen never made.
            #
            # A tape whose beats lack the keys ENTIRELY leaves this None, which
            # the menus verdict REFUSES by name rather than reading as zero;
            # a tape whose proof never reached the leg's TERMINAL STEP is
            # refused separately, below, via np_proof_step_max.
            if all(key in event for key in
                   ("np_entered", "np_skipped", "np_focus", "np_flags", "np_rows")):
                beat = {
                    "np_entered": int(event.get("np_entered", 0) or 0),
                    "np_skipped": int(event.get("np_skipped", 0) or 0),
                    "np_focus": int(event.get("np_focus", 0) or 0),
                    "np_flags": int(event.get("np_flags", 0) or 0),
                    "np_rows": int(event.get("np_rows", 0) or 0),
                }
                if self.np_proof is None:
                    self.np_proof = beat
                else:
                    for key in ("np_entered", "np_skipped", "np_focus",
                                "np_flags"):
                        self.np_proof[key] |= beat[key]
                    self.np_proof["np_rows"] = max(self.np_proof["np_rows"],
                                                   beat["np_rows"])
                # THE TERMINAL EVIDENCE (A23, review A20-1). map_max and the
                # np_* masks are formatted from the SAME walker snapshot into
                # the SAME line, and an over-long heartbeat is dropped WHOLE
                # by the producer rather than shortened -- so a beat that
                # reports the terminal ordinal necessarily reports the walker's
                # final masks and flags, and loss can never separate them.
                # Tracked per beat rather than run-wide so the run-wide
                # max_map_step (which any beat, including one from a .dol with
                # no walker proof at all, can raise) cannot stand in for it.
                self.np_proof_step_max = max(
                    self.np_proof_step_max
                    if self.np_proof_step_max is not None else 0,
                    int(event.get("map_max", 0) or 0))
            if event.get("overflow"):
                self.overflow_beats += 1
            self.max_smig_state = max(
                self.max_smig_state, int(event.get("smig_state", 0) or 0))
            self.max_smig_ms = max(
                self.max_smig_ms, int(event.get("smig_ms", 0) or 0))
            self.max_smig_regions = max(
                self.max_smig_regions, int(event.get("smig_regions", 0) or 0))
            self.max_smig_chunks = max(
                self.max_smig_chunks, int(event.get("smig_chunks", 0) or 0))
            self.max_smig_signs = max(
                self.max_smig_signs, int(event.get("smig_signs", 0) or 0))
            self.max_smig_expected = max(
                self.max_smig_expected, int(event.get("smig_expected", 0) or 0))
            self.max_smig_verified = max(
                self.max_smig_verified, int(event.get("smig_verified", 0) or 0))
            self.max_mrs_cycles = max(
                self.max_mrs_cycles, int(event.get("mrs_cycles", 0) or 0))
            if bool(event.get("mrs_wedge")):
                self.mrs_wedge_seen = True
            self.max_hb_gap_ms = max(
                self.max_hb_gap_ms, int(event.get("wd_hb_gap_max_ms", 0) or 0))
            # Far-ring keep-up probe (gate 4): see the __init__ comment for why
            # both the collector's own max AND the device-reported running max
            # are tracked.
            self.max_far_missing = max(
                self.max_far_missing, int(event.get("far_missing", 0) or 0))
            self.max_far_missing_reported = max(
                self.max_far_missing_reported,
                int(event.get("far_missing_max", 0) or 0))
            self.max_far_settle_ms = max(
                self.max_far_settle_ms, int(event.get("far_settle_ms", 0) or 0))
            self.max_srv_gen_us = max(
                self.max_srv_gen_us, int(event.get("srv_gen_us", 0) or 0))
            self.max_srv_gen_cols = max(
                self.max_srv_gen_cols, int(event.get("srv_gen_cols", 0) or 0))
            self.max_srv_gen_backlog = max(
                self.max_srv_gen_backlog,
                int(event.get("srv_gen_backlog", 0) or 0))
            # P2-c telemetry (see the __init__ comment above).
            self.max_ms_mesh_worker = max(
                self.max_ms_mesh_worker,
                float(event.get("ms_mesh_worker_max", 0) or 0))
            self.max_mem2_lock_wait = max(
                self.max_mem2_lock_wait,
                int(event.get("mem2_lock_wait", 0) or 0))
            # T-a/T-b/T-c telemetry-robustness fields (see the __init__
            # comment for the tracking rationale for each).
            self.max_tel_fail = max(
                self.max_tel_fail, int(event.get("tel_fail", 0) or 0))
            if bool(event.get("tel_quiesced")):
                self.tel_quiesced_seen = True
            self.max_frame_counter = max(
                self.max_frame_counter, int(event.get("frame_counter", 0) or 0))
            # Sender self-telemetry (see the __init__ comment).
            self.max_tel_drop = max(
                self.max_tel_drop, int(event.get("tel_drop", 0) or 0))
            self.max_tel_smax_us = max(
                self.max_tel_smax_us, int(event.get("tel_smax_us", 0) or 0))
            self.max_tel_qmax = max(
                self.max_tel_qmax, int(event.get("tel_qmax", 0) or 0))
            self.max_tel_hb_repl = max(
                self.max_tel_hb_repl, int(event.get("tel_hb_repl", 0) or 0))
            self.max_tel_wto = max(
                self.max_tel_wto, int(event.get("tel_wto", 0) or 0))
            # Sender stage (see the __init__ comment). tel_stg/tel_stg_us are
            # read as a PAIR: the stage is only meaningful beside its dwell.
            if event.get("tel_stg") is not None:
                stage = TEL_STAGE_NAMES.get(
                    int(event.get("tel_stg") or 0), "unknown")
                stg_us = int(event.get("tel_stg_us", 0) or 0)
                self.tel_stages_seen[stage] = (
                    self.tel_stages_seen.get(stage, 0) + 1)
                if stg_us >= self.max_tel_stg_us:
                    self.max_tel_stg_us = stg_us
                    self.worst_tel_stage = stage
            self.max_tel_pmax_us = max(
                self.max_tel_pmax_us, int(event.get("tel_pmax_us", 0) or 0))
            self.max_tel_xmax_us = max(
                self.max_tel_xmax_us, int(event.get("tel_xmax_us", 0) or 0))
            self.max_tel_abd = max(
                self.max_tel_abd, int(event.get("tel_abd", 0) or 0))
            for key, attr in (("tel_pret", "last_tel_pret"),
                              ("tel_prev", "last_tel_prev"),
                              ("tel_sret", "last_tel_sret")):
                if event.get(key) is not None:
                    setattr(self, attr, int(event.get(key) or 0))
            if event.get("tel_it") is not None:
                tel_it = int(event.get("tel_it") or 0)
                self.min_tel_it = (tel_it if self.min_tel_it is None
                                   else min(self.min_tel_it, tel_it))
            if event.get("tel_sends") is not None:
                tel_sends = int(event.get("tel_sends") or 0)
                self.min_tel_sends = (tel_sends if self.min_tel_sends is None
                                      else min(self.min_tel_sends, tel_sends))
            carve_kb = event.get("mem2_carve_free_kb")
            if carve_kb is not None:
                try:
                    carve_kb = int(carve_kb)
                    if carve_kb >= 0:
                        self.min_mem2_carve_free_kb = (
                            carve_kb if self.min_mem2_carve_free_kb is None
                            else min(self.min_mem2_carve_free_kb, carve_kb))
                except (TypeError, ValueError):
                    pass
            if bool(event.get("map_sign")):
                self.map_sign_reached = True
            if bool(event.get("creative")):
                self.creative_seen = True
            fvd = event.get("far_view_distance")
            if fvd is not None:
                try:
                    fvd = int(fvd)
                    self.max_fvd = fvd if self.max_fvd is None else max(self.max_fvd, fvd)
                except (TypeError, ValueError):
                    pass
            fring = event.get("fring")
            if fring is not None:
                try:
                    fring = int(fring)
                    self.max_fring = (
                        fring if self.max_fring is None else max(self.max_fring, fring)
                    )
                except (TypeError, ValueError):
                    pass
            if self.join_ms is None:
                try:
                    lsm = int(event.get("load_screen_ms") or 0)
                except (TypeError, ValueError):
                    lsm = 0
                if lsm > 0:
                    self.join_ms = lsm
            if (self.args.profile in MP_SESSION_GATE_PROFILES
                    and self.mp_session_end is None):
                reason = str(event.get("mp_end_reason") or "")
                loaded = bool(event.get("world_loaded"))
                if self._mp_session_violated(reason, loaded):
                    self.mp_session_end = (self.hb_count, reason, loaded)
            if bool(event.get("world_loaded")):
                self.world_loaded_seen = True
                # Track walk displacement only once the world is loaded -- the
                # pre-load camera sits at (0,0,0) and would fake movement.
                try:
                    x = float(event.get("x"))
                    z = float(event.get("z"))
                except (TypeError, ValueError):
                    x = z = None
                if x is not None:
                    self.walk_min_x = x if self.walk_min_x is None else min(self.walk_min_x, x)
                    self.walk_max_x = x if self.walk_max_x is None else max(self.walk_max_x, x)
                    self.walk_min_z = z if self.walk_min_z is None else min(self.walk_min_z, z)
                    self.walk_max_z = z if self.walk_max_z is None else max(self.walk_max_z, z)
            print(
                f"[hb {self.hb_count:04d}] fps={event.get('fps')} "
                f"rt={event.get('srv_rt_us')}us wt={event.get('srv_wt_us')}us "
                f"mem1={event.get('mem1_free_kb')}K mem2={event.get('mem2_free_kb')}K "
                f"cflood={event.get('cflood')} ndisp={event.get('ndisp')}"
                f"{format_blc(event)}"
                f"{format_lqd_wait(event)}"
                f"{' ssw=' + str(event['ssw']) if 'ssw' in event else ''}",
                flush=True,
            )
            return

        if kind == "crash_mailbox":
            self._append_dump_chunk(event)
            return

        if kind in ("watchdog_dump", "fault"):
            key = self._append_dump_chunk(event)
            self.dump_last_seen = now
            if not self.profile["expect_dump"]:
                self.fail(f"{kind} received: {event.get('cause', '?')}", kind)
            elif event.get("last") is True:
                # The PASS must prove the INJECTED-HANG detection path ran:
                # a 'fault' dump means the run CRASHED (a different failure,
                # not the watchdog hang detector) -- fail it explicitly.
                if kind == "fault":
                    self.fail(
                        f"fault dump received (crash, not the injected hang): "
                        f"{event.get('cause', '?')}",
                        "unexpected_fault",
                    )
                    return
                # The run must have been demonstrably alive before wedging.
                if self.hb_count < int(self.profile["min_hb"]):
                    self.fail(
                        f"dump completed but only {self.hb_count} heartbeats "
                        f"preceded it; need {self.profile['min_hb']}",
                        "too_few_heartbeats",
                    )
                    return
                parts = self.dumps[key]
                # A hole (None) means a chunk never arrived (UDP loss, or the
                # head evicted from the device's pre-connect ring): truncated
                # evidence must not certify the dump path.
                if any(p is None for p in parts):
                    missing = [i for i, p in enumerate(parts) if p is None]
                    self.fail(
                        f"{kind} completed with missing chunk seq(s) {missing}",
                        "incomplete_dump",
                    )
                    return
                text = "".join(parts)
                # A completed-but-EMPTY dump proves only that the chunk framing
                # works, not that the watchdog captured anything -- fail it.
                if text.strip():
                    self.pass_(f"expected {kind} received ({len(text)} bytes)")
                else:
                    self.fail(f"{kind} completed with empty text", "empty_dump")
            return

        if kind == "profile_done" and not self.profile["expect_dump"]:
            status = event.get("status")
            if self.args.profile == SPLIT_RECOVERY_PROFILE:
                # Recorded before the generic check below, which treats a
                # MISSING status as "ok": the recovery contract (section 6)
                # requires the literal "ok", so its own gate must see what
                # actually arrived.
                self.recovery.note_profile_done(status)
            if self.args.profile == PORTAL_QUIT_PROFILE:
                self.portal_quit.note_profile_done(status)
            if self.args.profile == SAVE_RELOAD_PROFILE:
                self.save_reload.note_profile_done(status)
            if self.args.profile == CHAT_LOGIN_PROFILE:
                # Same reason as the recovery hook above: the chat-login
                # contract requires the literal "ok" AFTER run.ok, so its own
                # gate must see exactly what arrived, and when.
                self.chat_login.note_profile_done(status)
            if self.args.profile == CHAT_CHORD_PROFILE:
                self.chat_chord.note_profile_done(status)
            if self.args.profile == MP_PLAY_PROFILE:
                # Same reason again: the contract ends with the `ok` marker and
                # THEN profile_done "ok", so its own gate must see exactly what
                # arrived and when.
                self.mp_play.note_profile_done(status)
            if self.args.profile == "mp_pause_menu_3m":
                self.mp_pause_menu.done_status = status
                if status in ("site_unverified", "apex_short"):
                    self.fail(status, "mp_pause_menu_site_outcome")
                    return
                if self.mp_pause_menu.error:
                    self.fail(self.mp_pause_menu.error, "mp_pause_menu_driver_failed")
                    return
            if status and status != "ok" and self.args.profile == MP_PLAY_PROFILE:
                # Grade the fixture receipts BEFORE the early fail so matched/
                # unmatched in the artifact are real. The verdict stays FAIL.
                self.mp_play.receipt_error(self.out)
            if status and status != "ok":
                self.fail(f"profile_done status={status}", "profile_done_error")
                return
            if self.tps_pending:
                # UDP can deliver profile_done before the sender's final seal.
                self.tps_profile_done = True
            else:
                self._finish_normal("profile_done received")

    def _append_dump_chunk(self, event: dict) -> tuple[str, str, int]:
        kind = str(event.get("event"))
        cause = str(event.get("cause") or "unknown")
        key = (kind, cause, int(event.get("dump_id", 0) or 0))
        seq = int(event.get("seq", 0) or 0)
        parts = self.dumps[key]
        while len(parts) <= seq:
            parts.append(None)
        parts[seq] = str(event.get("text", ""))
        text = "".join(p or "" for p in parts)
        path = self._dump_path_for(kind, cause)
        path.write_text(text, encoding="utf-8", errors="replace")
        print(f"[dump] {event.get('event')} {event.get('cause')} seq={seq}", flush=True)
        return key

    def _dump_path_for(self, kind: str, cause: str) -> Path:
        if kind != "crash_mailbox":
            return self.dump_path
        # Two mailbox paths can share a BASENAME. The watchdog's app-dir FALLBACK report
        # (sd:/apps/CavEX/hang_report.txt) carries the same file name as the primary
        # (sd:/hang_report.txt), so a basename-only artifact name let one silently
        # overwrite the other -- and the fallback exists only on the boot where the
        # primary write FAILED, i.e. exactly when both matter. Qualify the name with its
        # parent directory; a device-root path has no parent name and therefore keeps the
        # historical artifact name unchanged.
        path = Path(cause)
        name = path.name or "unknown"
        parent = path.parent.name
        if parent:
            name = f"{parent}_{name}"
        safe = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in name)
        safe = safe.strip("._") or "unknown"
        return self.out / f"crash_mailbox_{safe}"

    def tick(self) -> None:
        now = time.monotonic()
        if self.args.profile == OFFER_PISTON_PROFILE:
            if self.fail_reason:
                return
            pending = getattr(self, "offer_piston_fault_pending", None)
            if pending is not None:
                # A telemetry loss blocks every completion path; the deadline is
                # enforced here even when no further event arrives.
                if now - pending > 5.0 or self.offer_piston_completion_at is not None:
                    self.fail("heartbeat_fault (telemetry dropped; no attribution within 5 s)",
                              "split_offer_piston_evidence_failed")
                return
            if self.offer_piston_completion_at is not None and now-self.offer_piston_completion_at >= 1:
                self._finish_normal("offered guest piston and 30 s admitted split play")
                return
            if now-self.started > 120+self.args.first_heartbeat_timeout:
                self.fail("profile_done_missing", "split_offer_piston_evidence_failed")
                return
        if self.args.profile == VEHICLE_SMOKE_PROFILE:
            if self.fail_reason:
                return
            if self.vehicle_completion_at is not None and now - self.vehicle_completion_at >= 1:
                self._finish_normal("profile_done received; vehicle observations drained")
                return
            if now - self.started > 480 + self.args.first_heartbeat_timeout:
                self.fail("vehicle completion deadline expired", "vehicle_deadline")
                return
        if self.first_hb is None and now - self.started > self.args.first_heartbeat_timeout:
            cls = "booted_no_heartbeat" if self.latest else "no_telemetry"
            self.fail(
                f"no first heartbeat within {self.args.first_heartbeat_timeout}s",
                cls,
            )
            return
        if self.profile["expect_dump"]:
            # watchdog_inject wedges the main thread ON PURPOSE, so a stale
            # heartbeat is the expected prelude to the dump (watchdog fires at
            # >3s gap, then streams chunks). The old unconditional stale fail
            # raced the dump and could kill the run mid-chunks. Fail only when
            # the post-stall dump window closes with no COMPLETE dump (the
            # last=true chunk passes in process()).
            ref = self.dump_last_seen if self.dump_last_seen is not None else self.last_hb
            window = self.args.stale_timeout + self.args.dump_wait
            if ref is not None and now - ref > window:
                self.fail(
                    f"expected dump: no dump activity within {window}s of last "
                    f"heartbeat/chunk",
                    "no_dump_after_stale",
                )
                return
            # Self-termination when the inject never wedges: healthy heartbeats
            # keep refreshing ref forever, so without this an un-armed inject
            # spins until the wrapper's hard kill -- losing the FAIL artifact.
            duration = self.args.duration or self.profile["duration"]
            if now - self.started >= duration + window:
                self.fail(
                    f"no dump within duration {duration}s + {window}s window "
                    f"(inject never wedged?)",
                    "no_dump_in_window",
                )
            return
        duration = self.args.duration or self.profile["duration"]
        if self.shutdown_required and now - self.started >= duration:
            # MAIN stops heartbeats while joining/draining. Preserve the
            # normal stale gate during gameplay, then use this bounded tail
            # deadline (first HB accounts for host/device boot clock offset).
            deadline = (self.first_hb if self.first_hb is not None else self.started) + duration + CHECKPOINT_SHUTDOWN_WAIT_SECONDS
            if now >= deadline:
                problem = self.shutdown_error() or "missing tps_end"
                self.fail(f"checkpoint shutdown timeout after {CHECKPOINT_SHUTDOWN_WAIT_SECONDS}s: {problem}",
                          "checkpoint_shutdown_timeout")
            return
        if self.last_hb is not None and now - self.last_hb > self.args.stale_timeout:
            # T-a (2026-09-03, Round 3 (3) "walk wedge" fix): a stale
            # heartbeat used to be indistinguishable from a real main-thread
            # freeze -- exactly the failure the walk wedge produced (one
            # -EAGAIN silenced telemetry for the rest of a healthy run). The
            # LAST heartbeat this collector actually received already tells
            # the difference: tel_fail > 0 there means the DEVICE itself saw
            # send trouble before going quiet (self-quiesced or its socket
            # died), which is real evidence pointing at the SOCKET, not
            # necessarily the game -- a distinct failure_class so a human
            # (or a later automated retry) does not have to open PASS/
            # FAIL.json's last_hearteat by hand to tell "telemetry dropped"
            # apart from "the console genuinely hung".
            tel_fail = int(self.last_heartbeat_event.get("tel_fail", 0) or 0)
            if tel_fail > 0:
                self.fail(
                    f"heartbeat stale for >{self.args.stale_timeout}s after "
                    f"{self.hb_count} heartbeats (last heartbeat already showed "
                    f"tel_fail={tel_fail}, tel_last_errno="
                    f"{self.last_heartbeat_event.get('tel_last_errno')}, "
                    f"tel_quiesced={self.last_heartbeat_event.get('tel_quiesced')}"
                    f" -- telemetry itself likely dropped/quiesced, not "
                    f"necessarily a game freeze)",
                    "heartbeat_stale_telemetry_dropped",
                )
            else:
                self.fail(
                    f"heartbeat stale for >{self.args.stale_timeout}s after "
                    f"{self.hb_count} heartbeats",
                    "heartbeat_stale_after_start",
                )
            return
        if self.args.profile in (VEHICLE_SMOKE_PROFILE, OFFER_PISTON_PROFILE):
            return  # Duration never supplies evidence or successful completion.
        if self.args.profile == "mp_pause_menu_3m":
            if now - (self.first_hb or self.started) > MP_PAUSE_MENU_CAP_S:
                self.fail("mp_pause_menu completion deadline expired "
                          f"({MP_PAUSE_MENU_CAP_S}s after first heartbeat, no profile_done)",
                          "mp_pause_menu_deadline")
            return
        if self.args.profile in SPLIT_PROFILES:
            # Completion comes from profile_done after the device's readiness
            # clock. Loading time must never consume the measurement window.
            if now - self.started > 1200:
                self.fail("anchor readiness/completion timeout", "anchor_incomplete")
            return
        duration = self.args.duration or self.profile["duration"]
        if now - self.started >= duration:
            # Collector startup precedes the device's duration clock by boot
            # and connect time. Keep receiving an observed TPS window through
            # its seal; the normal stale timeout and this bounded grace still
            # detect a missing tail. A device-side margin alone cannot fix the
            # two clocks' offset.
            if self.tps_pending:
                if now - self.started >= duration + self.args.stale_timeout + 45:
                    self.fail("missing tps_end after duration", "tps_incomplete")
            elif not self.release_gate:
                self._finish_normal(f"duration {duration}s reached")
            elif now - self.started >= duration + self.args.stale_timeout + 45:
                self.fail("missing final frame evidence/profile_done",
                          "release_performance_incomplete")

    def note_guest_progress(self, beat: dict) -> None:
        """Grade each measured 60 s window by horizontal path length.

        Missing/non-finite coordinates or time are missing evidence; a repeated
        or decreasing tick is present but invalid evidence and is reported as
        guest_progress_invalid.
        """
        ms, tick = beat.get("anchor_ms"), beat.get("tick")
        gx, gz = beat.get("gx"), beat.get("gz")
        if (type(ms) is not int or type(tick) is not int
                or type(gx) not in (int, float) or not math.isfinite(gx)
                or type(gz) not in (int, float) or not math.isfinite(gz)):
            self.fail("measured heartbeat lacks guest progress coordinates/time/tick",
                      "guest_progress_missing")
            return
        path = self.guest_progress
        if path:
            previous_ms, previous_tick, px, pz, _ = path[-1]
            if ms <= previous_ms:
                self.fail("guest progress clock did not advance", "guest_progress_invalid")
                return
            if tick <= previous_tick:
                self.fail("guest progress tick did not increase",
                          "guest_progress_invalid")
                return
            self.guest_total_distance_walked += math.hypot(gx - px, gz - pz)
        path.append((ms, tick, gx, gz, self.guest_total_distance_walked))
        cutoff = ms - 60000
        if cutoff < path[0][0]:
            return
        while len(path) > 1 and path[1][0] <= cutoff:
            path.popleft()
        start = path[0]
        start_distance = start[4]
        if len(path) > 1 and start[0] < cutoff:
            after = path[1]
            fraction = (cutoff - start[0]) / (after[0] - start[0])
            start_distance += fraction * (after[4] - start[4])
        displacement = self.guest_total_distance_walked - start_distance
        self.min_window_displacement = (displacement if self.min_window_displacement is None
                                        else min(self.min_window_displacement, displacement))
        if displacement < 8.0:
            self.guest_stall_window = dict(window_start_tick=start[1],
                                           window_end_tick=tick,
                                           displacement=displacement)
            self.fail("guest_stalled: 60 s path %.2f blocks from tick %s to %s"
                      % (displacement, start[1], tick), "guest_stalled")

    def walk_displacement(self) -> float | None:
        if self.walk_min_x is None or self.walk_min_z is None:
            return None
        return max(self.walk_max_x - self.walk_min_x,
                   self.walk_max_z - self.walk_min_z)

    def mp_play_pc_no_heartbeats(self) -> bool:
        """True for an mp_play_10m run the DEVICE stamped `platform:"pc"` that
        produced no heartbeat at all -- the one case where the heartbeat-derived
        gates have nothing to grade rather than something missing.

        On PC there IS no heartbeat transport for this profile:
        hw_telemetry_emit_heartbeat (source/platform/hw_telemetry.c:2784-2800)
        writes stdout only when the heartbeat carries an ARMED ANCHOR, and
        mp_play_10m is not an anchor profile, so a PC tape carries markers and
        profile_done and nothing else. The honest answer is "not graded", not a
        faked beat and not a loosened floor.

        Deliberately narrow, and fail-closed on both edges:
          * the platform comes from the device's own p0/session marker, so a
            tape with no session marker reads None and is graded as a console
            run (mandatory gates), exactly like "wii";
          * a run that DID produce heartbeats is graded on them whatever it
            says its platform is -- evidence that exists is always graded."""
        return (self.args.profile == MP_PLAY_PROFILE
                and self.mp_play.platform == "pc"
                and self.hb_count == 0)

    def _mp_session_violated(self, reason: str, loaded: bool) -> bool:
        """REL-MP-GATE: whether this heartbeat shows the session gone where
        the profile claims it is up (after load or past the login window).
        The current heartbeat's own loaded state counts: a terminal reason on
        the first loaded beat ends the login too (world_loaded_seen is only
        updated after this check runs)."""
        dead = bool(reason) or not loaded
        login_over = (self.world_loaded_seen or loaded
                      or self.hb_count > MP_SESSION_LOGIN_WINDOW_HB)
        return dead and login_over

    def _finish_normal(self, reason: str) -> None:
        if self.vehicle_pc_replay:
            if (self.vehicle_pc_source_cursor != len(self.vehicle_pc_source_lines)
                    or hashlib.sha256(self.vehicle_pc_source_path.read_bytes()).hexdigest() != self.vehicle_pc_source_sha256):
                self.fail("vehicle_pc_replay_evidence_changed_or_incomplete", "vehicle_smoke_evidence_failed")
                return
        pc_ungraded = self.mp_play_pc_no_heartbeats()
        vehicle_pc_no_hb = self.vehicle_pc_replay and self.hb_count == 0
        if vehicle_pc_no_hb:
            self.not_graded = ["heartbeat floor: NOT GRADED ON PC REPLAY (no heartbeat transport); NONCERTIFYING"]
        if pc_ungraded:
            # Named in the artifact and on the replay's stdout so a PC PASS can
            # never be read as covering them. Both are restored in full the
            # moment the same build runs on a console.
            self.not_graded = [
                "heartbeat floor (min_hb / MpPlayEvidence.heartbeat_floor): "
                "NOT GRADED ON PC (no heartbeat transport)",
                "lw_to_admits heartbeat gate (lworld_ready_timeout_admits == 0 "
                "from first beat to last): NOT GRADED ON PC "
                "(no heartbeat transport)",
                "scenario gate world_loaded_seen: NOT GRADED ON PC (no "
                "heartbeat transport); the driver's own ready(gen 1) marker is "
                "what observed the remote world install",
            ]
        if self.args.profile == OFFER_PISTON_PROFILE:
            if getattr(self, "offer_piston_fault_pending", None) is not None:
                self.fail("heartbeat_fault (telemetry dropped before completion)",
                          "split_offer_piston_evidence_failed")
                return
            problem = self.offer_piston.complete_error()
            if problem:
                self.fail(problem, "split_offer_piston_evidence_failed")
                return
        min_hb = int(self.profile["min_hb"])
        if self.args.profile == VEHICLE_SMOKE_PROFILE:
            problem = self.vehicle_smoke.complete_error()
            if problem:
                self.fail(problem, "vehicle_smoke_evidence_failed")
                return
            min_hb = self.vehicle_smoke.heartbeat_floor()
            if not self.vehicle_manifest_data:
                self.fail("vehicle_manifest_required", "vehicle_manifest_failed")
                return
            problem = vehicle_calibration_error(self.vehicle_manifest_data, Path(self.vehicle_manifest_path).parent)
            if problem:
                self.fail(problem, "vehicle_calibration_required")
                return
            problem = vehicle_transfer_ack_proof_error(self.vehicle_manifest_data, Path(self.vehicle_manifest_path).parent)
            if problem:
                self.fail(problem, "vehicle_transfer_ack_handler_proof_required")
                return
        if self.args.profile == PORTAL_QUIT_PROFILE:
            min_hb = self.portal_quit.heartbeat_floor(min_hb)
        elif self.args.profile == SPLIT_RECOVERY_PROFILE:
            min_hb = self.recovery.heartbeat_floor(min_hb)
        if self.args.profile == CHAT_LOGIN_PROFILE:
            # Same cadence rule, same reason: this profile ends on its last
            # assertion, not on the clock, so the floor is beats-per-second of
            # the driver's own run, not a fixed count.
            min_hb = self.chat_login.heartbeat_floor(min_hb)
        if self.args.profile == CHAT_CHORD_PROFILE:
            min_hb = self.chat_chord.heartbeat_floor(min_hb)
        if self.args.profile == MP_PLAY_PROFILE:
            # The cadence over the run's OWN measured play, which has a hard
            # 600 000 ms floor -- so this can only ever raise the table's 480.
            min_hb = self.mp_play.heartbeat_floor(min_hb)
        if not pc_ungraded and not vehicle_pc_no_hb and self.hb_count < min_hb:
            self.fail(
                f"only {self.hb_count} heartbeats; need {min_hb}",
                "too_few_heartbeats",
            )
            return
        if self.mp_session_end is not None:
            beat, reason, loaded = self.mp_session_end
            self.fail(
                f"mp session ended at heartbeat {beat} of {self.hb_count}: "
                f"mp_end_reason={reason!r} world_loaded={loaded}",
                "mp_session_ended",
            )
            return
        if self.args.profile in MP_JOIN_GATE_PROFILES:
            if self.join_ms is None and pc_ungraded:
                pass  # PC run: no heartbeats, no load screen to time
            elif self.join_ms is None:
                self.fail("join time missing: no heartbeat carried "
                          "load_screen_ms > 0", "mp_join_ms_missing")
                return
            if self.join_ms is not None and self.join_ms > MP_JOIN_FAIL_MS:
                self.fail(f"join too slow: {self.join_ms} ms "
                          f"(limit {MP_JOIN_FAIL_MS})", "mp_join_too_slow")
                return
        if self.args.profile == "split_dims_6m":
            # The semantic claim first: a run whose two views never showed two
            # dimensions is dims_not_active whatever else it measured.
            verdict = self.split_dims.verdict()
            if verdict:
                self.fail(*verdict)
                return
            if self.telemetry_gaps:
                gap = self.telemetry_gaps[0]
                self.fail(f"split dimensions lost {len(self.telemetry_gaps)} "
                          f"heartbeat interval(s); first ticks "
                          f"{gap['missing_ticks']} ({gap['cause']})",
                          "split_dims_heartbeat_missing")
                return
            if self.max_oom:
                self.fail(f"split dimensions allocation failure: oom={self.max_oom}",
                          "split_dims_oom")
                return
        if (self.args.profile == "mp_lan_walk_10m"
                and self.mp_move_cadence["pairs"] == 0):
            self.fail("movement cadence: no consecutive live heartbeats in one session",
                      "mp_move_cadence_missing")
            return
        # G2 scenario gates: a PASS must prove the profile's stressor engaged.
        # Without these, "redstone_clock_2m PASS" only proved a superflat idled
        # for 120s (the documented false-PASS class).
        scenario = self.profile.get("scenario")
        # world_copy_convert never enters a world by design (convert-only;
        # Play world is never pressed) -- its own gate below inverts this.
        if (scenario is not None and scenario != "world_copy_convert"
                and not self.world_loaded_seen and not pc_ungraded):
            # world_loaded_seen is read off a HEARTBEAT (see note_heartbeat), so
            # on a PC mp_play tape it can only ever be False. The stressor is
            # still proved, and more directly: this profile arms no autoload, so
            # its ready(gen 1) marker IS the driver observing the remote world
            # install -- and the marker contract below refuses a run without it.
            # Sol round-3 review: a walker that abandons BEFORE the world
            # loads (a step-0 stall, a create-world refusal) ends here, so
            # its named menu_ap_fail reason rides on this refusal too.
            self.fail(
                "scenario gate: world never reported loaded"
                f"{self.menu_ap_fail_text()}",
                "scenario_not_engaged",
            )
            return
        if scenario in ("far_ring", "far_walk") and (
            self.max_fring is None or self.max_fring < 1
        ):
            # fring = far_vd - MAX_VIEW_DISTANCE from the device; the raw
            # far_view_distance CANNOT express "ring off" (main.c clamps it to
            # >= MAX_VIEW_DISTANCE even when disabled), so gating on it would
            # be permanently satisfied. fring absent (old .dol) also fails --
            # an unprovable scenario must not certify.
            self.fail(
                f"scenario gate: far ring never active "
                f"(max fring={self.max_fring}, far_view_distance={self.max_fvd})",
                "scenario_not_engaged",
            )
            return
        if scenario in ("far_walk", "split_anchor"):
            need = float(self.profile.get("min_walk_blocks", 32.0))
            disp = self.walk_displacement()
            if disp is None or disp < need:
                self.fail(
                    f"scenario gate: autopilot walk displaced "
                    f"{disp if disp is not None else 0:.1f} blocks; need >= {need} "
                    f"(seed={self.seed if self.seed is not None else 'unseeded'})",
                    "scenario_not_engaged",
                )
                return
        if scenario == "mp_walk":
            # Same displacement proof as far_walk, but deliberately WITHOUT
            # the far-ring requirement above: mp profiles leave
            # far_view_distance at the server-driven platform default, so
            # fring is never active and gating on it would be an unprovable,
            # permanently-failing check for this profile.
            need = float(self.profile.get("min_walk_blocks", 32.0))
            disp = self.walk_displacement()
            if disp is None or disp < need:
                self.fail(
                    f"scenario gate: mp autopilot walk displaced "
                    f"{disp if disp is not None else 0:.1f} blocks; need >= {need} "
                    f"(seed={self.seed if self.seed is not None else 'unseeded'})",
                    "scenario_not_engaged",
                )
                return
        if scenario == "split_regrow":
            problem = self.regrow.complete_error()
            if problem:
                self.fail("split regrow: " + problem, "split_regrow_failed")
                return
        if scenario == "save_reload":
            problem = self.save_reload.complete_error()
            if problem:
                self.fail("save/reload: " + problem, "save_reload_failed")
                return
        if scenario == "pool_selftest":
            problem = self.pool_selftest.complete_error()
            if problem:
                self.fail("pool self-test: " + problem, "pool_selftest_failed")
                return
        if scenario == "mob_motion":
            if self.mm_terminal is None or self.mm_terminal[1] != 2:
                self.fail(
                    "scenario gate: no terminal mob motion PASS on the heartbeat "
                    "(the fixture never published, or the .dol predates the "
                    "mm_* heartbeat keys)",
                    "mob_motion_evidence_failed",
                )
                return
        if scenario == "redstone":
            need_ndisp = int(self.profile.get("min_ndisp", 200))
            if self.max_ndisp < need_ndisp:
                self.fail(
                    f"scenario gate: notify dispatches ndisp={self.max_ndisp} "
                    f"< {need_ndisp} (clock never ran, or the .dol predates the "
                    f"ndisp heartbeat field)",
                    "scenario_not_engaged",
                )
                return
        if scenario == "menus":
            # Fallback mirrors PROFILES["menus_dwell"]["min_map_step"] above --
            # see MENUS_DWELL_STEP_CONTRACT before changing either number.
            need_step = int(self.profile.get("min_map_step",
                                             MENUS_DWELL_STEP_CONTRACT))
            spent = MENUS_DWELL_STEP_SPENT.get(need_step)
            if spent:
                # Refused BY NAME, never silently honoured: a spent ordinal
                # grades the run against a script shape that no longer exists.
                self.fail(
                    f"menus_dwell step contract: min_map_step={need_step} is "
                    f"SPENT ({spent}); the current contract is "
                    f"{MENUS_DWELL_STEP_CONTRACT} "
                    f"(menu_ap_script_step_required_for_pass)",
                    "scenario_not_engaged",
                )
                return
            # THE TERMINAL STATE OF THE WALKER (push-0921 A23, review A20-1).
            # A PASS has to prove the leg FINISHED, not merely that nothing bad
            # was observed while it ran. The proof is the leg's own terminal
            # ordinal -- need_step IS menu_ap_script_step_required_for_pass()
            # == STEP_NP_LEG_DONE, the step that sets np_leg_done and falls
            # into STEP_DONE -- reported by a heartbeat that ALSO carried the
            # walker proof.
            #
            # WHY IT CANNOT BE FORGED BY LOSS. Loss deletes whole beats; it
            # never fabricates one, and it never shortens one (an over-long
            # heartbeat is dropped whole by the producer, hw_telemetry.c's
            # overflow substitute, which carries no np_* keys and no map_max).
            # A device emits map_max == need_step only after executing
            # STEP_NP_LEG_DONE, which is reachable solely through the whole
            # leg, and it emits the np_* masks from the same snapshot in the
            # same line. So one such beat is positive evidence of completion,
            # and beats lost before OR after it subtract nothing: the masks are
            # monotonic, and nothing the verdict reads can change after the
            # script has finished.
            #
            # WHY NOT THE TAIL-WINDOW RULE THE REVIEW SUGGESTED (last beat's
            # tick within a window of the run's expected end, or a cadence-
            # derived min_hb): measured over the 44 recorded menus_dwell runs
            # in tmp/wii_runs, an honest 240 s window produced anywhere from
            # 139 to 238 heartbeats -- the cadence is not a constant, it tracks
            # what the screens cost -- so any count/tick-derived end gate would
            # refuse honest runs. The walker's own terminal step is the
            # instrument that actually answers the question.
            if self.np_proof is not None and (self.np_proof_step_max or 0) < need_step:
                overflow = (f"; {self.overflow_beats} heartbeat(s) were the "
                            "producer's minimal overflow substitute, which "
                            "carries no walker proof"
                            if self.overflow_beats else "")
                self.fail(
                    "menus_dwell walker proof never reached the leg's "
                    f"terminal step {need_step}: the highest map_max on a "
                    "heartbeat that carried np_entered/np_skipped/np_flags was "
                    f"{self.np_proof_step_max}, and the run's high-water "
                    f"map_max was {self.max_map_step}. The END of the leg was "
                    "never observed -- either it never finished (a failed leg "
                    "abandons into a step numbered BELOW this one) or the tape "
                    f"stops before it did{overflow}"
                    f"{self.menu_ap_fail_text()}",
                    "walker_leg_unfinished",
                )
                return
            if self.max_map_step < need_step:
                self.fail(
                    f"scenario gate: menu_autopilot max step "
                    f"map_max={self.max_map_step} < {need_step} (title/"
                    f"texture-packs/select-world/create-world/pause/options "
                    f"did not all round-trip, or the .dol predates the "
                    f"map_max heartbeat field){self.menu_ap_fail_text()}",
                    "scenario_not_engaged",
                )
                return
            # ---- WALKER PROOF (push-0921 A11, review A9-3) --------------
            # map_max alone is a high-water mark: the leg reaches its terminal
            # step whether it ENTERED all seven targets or named all seven as
            # skips. What it actually did is in np_entered / np_skipped /
            # np_flags, and the menus_dwell step-contract verdict requires them.
            proof = self.np_proof
            if proof is None:
                self.fail(
                    "menus_dwell walker proof fields missing: no heartbeat "
                    f"carried np_entered/np_skipped/np_flags, so map_max="
                    f"{self.max_map_step} is a high-water mark with nothing "
                    "behind it (a .dol predating the contract-"
                    f"{MENUS_DWELL_STEP_CONTRACT} walker-proof telemetry)",
                    "walker_proof_missing",
                )
                return
            # (A16-4's second half -- "refuse a tape whose FINAL heartbeat
            # lacks the keys" -- is GONE, deliberately, review A20-1. Loss
            # removes whole beats, so the last beat that ARRIVES still carries
            # the keys: the rule could only ever fire for the producer's
            # overflow substitute (whose diagnosis it then got wrong, A20-5)
            # and for a mid-run binary change, while punishing the single
            # trailing heartbeat this rig really does lose. Both of its honest
            # cases are covered, and named better, by the terminal-step rule
            # above: a mid-run swap leaves the proof short of the terminal
            # ordinal, and an unfinished leg never reaches it.)
            entered = proof["np_entered"]
            skipped = proof["np_skipped"]
            focus = proof["np_focus"]
            flags = proof["np_flags"]
            rows = proof["np_rows"]
            # "on platform wii" is UNCONDITIONAL here, and deliberately not
            # keyed on a platform field: this collector only ever grades tapes
            # that a .dol produced, and hw_telemetry.c's whole network send is
            # PLATFORM_WII-gated -- a PC build emits no heartbeats at all, so
            # there is no non-wii tape to exempt. A Dolphin run is the same
            # .dol and the same input_text_active() hard false. Inventing a
            # platform key to carry a constant would only create a way to
            # spoof the exemption.
            if flags:
                # Every bit is named, including one this table does not know
                # (push-0921 A23, review A20-6): the refusal was already right,
                # but "np_flags=0x80 -- " with nothing after the dash left the
                # reader to guess. tests/test_menus_dwell_contract.c is what
                # keeps the table itself from going stale; this is what the
                # tape says on the day it has.
                parts = [text for bit, text in sorted(NP_FLAG_NAMES.items())
                         if flags & bit]
                for bit in range(8):
                    value = 1 << bit
                    if flags & value and value not in NP_FLAG_NAMES:
                        parts.append(
                            f"unnamed bit 0x{value:02x} (no entry in this "
                            "collector's NP_FLAG_NAMES -- a walker flag was "
                            "added to menu_autopilot_script.h and the mirror "
                            "was not updated)")
                named = "; ".join(parts)
                self.fail(
                    f"scenario gate: walker np_flags=0x{flags:02x} -- {named}",
                    "walker_proof_flags",
                )
                return
            missing = NP_REQUIRED_ENTERED & ~entered
            if missing:
                names = ", ".join(sorted(
                    n for n, bit in NP_SUB.items() if missing & bit))
                self.fail(
                    f"scenario gate: the walker never entered {names} "
                    f"(np_entered=0x{entered:02x}); these have no list row "
                    "gating them, so not entering one is unreachability, not "
                    "an empty list",
                    "walker_target_not_entered",
                )
                return
            # THE RING WAS SEEN ON EVERY TARGET IT ENTERED (push-0921 A23,
            # review A20-4). np_focus is the walker's "the OBSERVED focus IS
            # this target" mark, and until now it was parsed, OR-ed and graded
            # by nothing -- so a tape could claim all seven entries with the
            # focus mask flat zero and still certify the card.
            #
            # The rule is the DEVICE's own invariant, not a new demand:
            # np_entered is marked only at STEP_NP_SUB_WAIT_OUT, which is
            # reachable only through STEP_NP_SUB_ARM, which marks
            # np_focus_reached (menu_autopilot_script.c:1634, :1712). So
            # entered SUBSET focus holds per beat on any real device, and
            # therefore across the union too (a union of subsets is a subset).
            # An entered bit without its focus bit is a tape no walker can
            # produce -- a .dol whose SEEK/ARM wiring has drifted, or a forged
            # line -- and neither may certify.
            #
            # SKIPS are deliberately not graded here: a row-gated Rename /
            # Delete / Edit is skipped at the SEEK exhaustion, i.e. with the
            # target never under the ring, so its focus bit is legitimately
            # clear (an OSK-backed skip DOES set it, and that difference is
            # the point of the field -- see menu_autopilot_script.h).
            blind = entered & ~focus
            if blind:
                names = ", ".join(sorted(
                    n for n, bit in NP_SUB.items() if blind & bit))
                self.fail(
                    f"scenario gate: the walker reports having ENTERED {names} "
                    "without ever observing the ring on them "
                    f"(np_entered=0x{entered:02x}, np_focus=0x{focus:02x}) -- "
                    "the walk marks np_focus at STEP_NP_SUB_ARM and np_entered "
                    "only after it, so this tape did not come from a walk that "
                    "saw its targets",
                    "walker_focus_not_observed",
                )
                return
            # Rename and Delete are DISABLED by the screen with no world row,
            # and Edit with no saved server. A skip is then the screen being
            # correct -- but only if the tape SHOWS the list was empty, which
            # for the world list is np_rows (the count the leg latched).
            for name, bit in NP_ROW_GATED.items():
                if entered & bit:
                    continue
                if not (skipped & bit):
                    self.fail(
                        f"scenario gate: the walker neither entered nor "
                        f"skipped {name} (np_entered=0x{entered:02x}, "
                        f"np_skipped=0x{skipped:02x})",
                        "walker_target_not_entered",
                    )
                    return
                if rows != 0:
                    self.fail(
                        f"scenario gate: the walker skipped {name} as "
                        f"row-gated, but the tape reports np_rows={rows} -- a "
                        "non-empty world list does not disable it",
                        "walker_skip_unjustified",
                    )
                    return
            # Edit is gated on a SAVED SERVER count, which no production
            # accessor exposes (see the A11 return: screen_mps_test_server_
            # count is #ifdef CAVEWII_CLIENT_INTERFACE_TEST, and adding one is
            # not this lane's file). So Edit is accepted as entered OR skipped
            # and NOT cross-checked against a count -- stated here rather than
            # silently treated as proven.
            if not (entered & NP_SUB["edit"]) and not (skipped & NP_SUB["edit"]):
                self.fail(
                    "scenario gate: the walker neither entered nor skipped "
                    f"edit (np_entered=0x{entered:02x}, "
                    f"np_skipped=0x{skipped:02x})",
                    "walker_target_not_entered",
                )
                return
            # "np_skipped carries no OSK-backed bit on wii" is ALREADY
            # enforced, twice and more precisely, so it is not repeated as a
            # bare bit test -- which would fire on a legitimately row-gated
            # Rename or Edit (both are OSK-backed AND row-gated, and an empty
            # list disables them for reasons that have nothing to do with a
            # keyboard). What actually has to be refused is a skip CAUSED by
            # the keyboard owning the Close role, and:
            #   * np_flags bit 1 is set by the walker exactly when that cause
            #     fires, and any flag bit is refused above -- that is the
            #     cause, named, rather than inferred from a bit pattern;
            #   * Direct Connect, Add and the name editor are OSK-backed and
            #     have no row gating at all, so NP_REQUIRED_ENTERED above
            #     already refuses any skip of them;
            #   * Rename and Edit are covered by their row-gate checks, which
            #     demand the tape SHOW the list was empty.
            # A bare bit test would add no refusal those three miss, and would
            # add a false one.
            # menus_dwell_creative-only (2026-09-02): the whole point of the
            # --cw-cworld-mode creative knob is a world that IS creative, not
            # merely a script that round-tripped the menus -- a PASS that
            # never saw creative=True would certify the exact silent-failure
            # class this profile exists to catch on purpose instead of by
            # accident (staged_menus_dwell_20260902_063552).
            if self.profile.get("require_creative") and not self.creative_seen:
                self.fail(
                    "scenario gate: --cw-cworld-mode creative knob did not "
                    "take -- no heartbeat ever reported creative=true "
                    "in-world",
                    "creative_not_observed",
                )
                return
        if scenario == "world_copy_convert":
            # Review round 1 (Sol B2): the in-run gate grades the WHOLE device
            # half -- every wcc_* marker, the wcc_card listing and the variant
            # semantics (recover's strict no-partial check included). Even a
            # clean device half is only PENDING_CARD (write_result): PASS.json
            # is written by --world-copy-verdict once the card hashes agree.
            if self.world_loaded_seen:
                self.fail("scenario gate: world_copy_convert loaded a world "
                          "(the route must never press Play world)",
                          "wcc_world_loaded")
                return
            ev, _ = world_copy_events(self.wcc_rows)
            variant = (ev.get("wcc_begin", {}).get("variant")
                       or ev.get("wcc_end", {}).get("variant"))
            dv = world_copy_verdict(self.wcc_rows, None, variant,
                                    device_only=True)
            self.wcc_device_verdict = dv
            if dv["status"] != "PASS":
                self.fail("scenario gate: world_copy_convert device evidence: "
                          + "; ".join(dv["reasons"]),
                          "wcc_device_evidence")
                return
        if scenario == "save_migrate":
            min_regions = int(self.profile.get("min_regions", 8))
            min_signs = int(self.profile.get("min_signs", 32))
            if self.max_smig_state != 4:
                self.fail(
                    f"scenario gate: save migration state={self.max_smig_state}, "
                    "need verified state 4",
                    "scenario_not_engaged",
                )
                return
            if not 0 < self.max_smig_ms <= 300000:
                self.fail(
                    f"scenario gate: migration elapsed {self.max_smig_ms}ms "
                    "outside (0,300000]",
                    "migration_duration",
                )
                return
            if self.max_smig_regions < min_regions:
                self.fail(
                    f"scenario gate: scanned {self.max_smig_regions} regions; "
                    f"need >= {min_regions}",
                    "scenario_not_engaged",
                )
                return
            expected = self.max_smig_expected
            if expected < min_signs or expected % 4 != 0:
                self.fail(
                    f"scenario gate: expected-sign count {expected} invalid; "
                    f"need >= {min_signs} in groups of four",
                    "scenario_not_engaged",
                )
                return
            if (self.max_smig_chunks < expected // 4
                    or self.max_smig_signs != expected
                    or self.max_smig_verified != expected):
                self.fail(
                    "scenario gate: save migration evidence mismatch "
                    f"chunks={self.max_smig_chunks} rewritten={self.max_smig_signs} "
                    f"verified={self.max_smig_verified} expected={expected}",
                    "scenario_not_engaged",
                )
                return
        if scenario == "mp_reconnect":
            need_cycles = int(self.profile.get("min_cycles", 6))
            if self.mrs_wedge_seen:
                self.fail(
                    f"scenario gate: mp_reconnect_soak wedged (a phase "
                    f"exceeded its timeout) after {self.max_mrs_cycles}/"
                    f"{need_cycles} cycles -- no wedge is a hard requirement",
                    "scenario_not_engaged",
                )
                return
            if self.max_mrs_cycles < need_cycles:
                self.fail(
                    f"scenario gate: mp_reconnect_soak completed only "
                    f"{self.max_mrs_cycles}/{need_cycles} connect/disconnect "
                    f"cycles",
                    "scenario_not_engaged",
                )
                return
        if self.args.profile == "perf_stress_5m":
            if self.max_oom > 0 or self.max_trunc > 0:
                self.fail(
                    f"perf stress saw oom={self.max_oom} trunc={self.max_trunc}",
                    "perf_resource_failure",
                )
                return
            if max(self.max_rt_us, self.max_wt_us) > self.args.max_server_us:
                self.fail(
                    f"server tick exceeded {self.args.max_server_us}us "
                    f"(rt={self.max_rt_us}, wt={self.max_wt_us})",
                    "perf_server_budget_exceeded",
                )
                return
            if self.shutdown_required:
                problem = self.shutdown_error()
                if problem:
                    self.fail(problem, "checkpoint_shutdown_failed")
                    return
        if self.release_gate:
            problem = self.release_evidence_error or release_frame_error(
                self.last_heartbeat_event, complete=True)
            if self.args.profile not in RELEASE_FRAME_PROFILES:
                problem = "release performance gate requires a six-minute walk profile"
            if problem:
                self.fail(problem, "release_performance_failed")
                return
        if self.args.profile in ANCHOR_PROFILES:
            problem = self.anchor.complete_error()
            if problem:
                self.fail(problem, "anchor_feasibility_failed")
                return
        if self.args.profile == "split_dims_6m":
            actual_tps = self.anchor.summary()["actual_tps"]
            if actual_tps is None or actual_tps < 19:
                self.fail(f"split dimensions TPS {actual_tps} below 19",
                          "split_dims_tps_failed")
                return
        if self.args.profile == SPLIT_RECOVERY_PROFILE:
            # The marker contract IS the verdict here (see RecoveryEvidence):
            # heartbeat evidence alone, however clean, never certifies that the
            # three recovery transitions happened.
            problem = self.recovery.complete_error()
            if problem:
                self.fail(problem, "split_recovery_evidence_failed")
                return
            # Section 0 / Astra ruling item 8: a scripted loss is not a
            # physical unplug, and the PASS must say so in these words.
            reason = f"{reason} -- {SPLIT_RECOVERY_PASS_LABEL}"
        if self.args.profile == PORTAL_QUIT_PROFILE:
            problem = self.portal_quit.complete_error()
            if problem:
                self.fail(problem, "split_portal_quit_evidence_failed")
                return
            reason = f"{reason} -- scripted split portal quit, N={self.portal_quit.remaining_ticks}"
        if self.args.profile == CHAT_CHORD_PROFILE:
            # The marker contract IS the verdict here: heartbeat evidence,
            # however clean, never certifies that a chord fired, that it left
            # the composer shut, or that the opener still worked afterwards.
            problem = self.chat_chord.complete_error()
            if problem:
                self.fail(problem, "chat_chord_evidence_failed")
                return
            reason = f"{reason} -- {self.chat_chord.pass_label()}"
        if self.args.profile == CHAT_LOGIN_PROFILE:
            # Two gates, judged and REPORTED separately (B1c section 5): the
            # device's marker contract, then the server's own receipt. Neither
            # heartbeat evidence nor a clean marker stream alone certifies that
            # the command reached the server.
            device = self.chat_login.complete_error()
            # Graded even when the device side already failed, so the artifact
            # always states BOTH halves rather than only the first one to trip.
            receipt = self.chat_login.receipt_error(self.out)
            if device:
                self.fail(device, "chat_login_evidence_failed")
                return
            if receipt:
                self.fail(receipt, "chat_login_receipt_failed")
                return
            # Section 0 / ruling 7 fix the wording of this PASS, and section 10
            # ruling 7 keeps a PC run from claiming the console one.
            reason = f"{reason} -- {self.chat_login.pass_label()}"
        if self.args.profile == MP_PLAY_PROFILE:
            # Two gates, judged and REPORTED separately, exactly as
            # mp_chat_login_3m's are: the device's marker contract, then the
            # fixture's own receipts (which is where every counted action is
            # matched 1:1). Heartbeat evidence certifies neither.
            device = self.mp_play.complete_error()
            # Graded even when the device half already failed, so the artifact
            # always states BOTH halves rather than only the first to trip.
            receipt = self.mp_play.receipt_error(self.out)
            if device:
                self.fail(device, "mp_play_evidence_failed")
                return
            if receipt:
                self.fail(receipt, "mp_play_receipt_failed")
                return
            # Astra gate 1's landing label, and a PC run may not wear it.
            reason = f"{reason} -- {self.mp_play.pass_label()}"
        if self.args.profile == "mp_pause_menu_3m":
            problem = self.mp_pause_menu.complete_error()
            if problem:
                self.fail(problem, "mp_pause_menu_evidence_failed")
                return
            reason = (f"{reason} -- remote pause held {MP_PAUSE_MENU_HOLD_MS} ms on one live "
                      "session, advanced ticks and fall, held forward suppressed")
        self.pass_(reason)

    def note_bad_line(self, exc: Exception) -> bool:

        if self.args.profile == OFFER_PISTON_PROFILE:
            self.fail("malformed_line: " + str(exc), "split_offer_piston_evidence_failed")
            return True
        if self.args.profile == VEHICLE_SMOKE_PROFILE:
            self.fail("vehicle_malformed_line: " + str(exc), "vehicle_smoke_evidence_failed")
            return True

        """One malformed NDJSON line is a dropped/partial send (the Wii sender drops a

        line it could not fully queue, by design); skip it and count it. Only three

        CONSECUTIVE malformed lines mean the stream is really corrupt -> fail.

        Returns True when the run has been failed."""

        self.bad_lines = getattr(self, 'bad_lines', 0) + 1
        # residency-1005/4 item 9: an undecodable line stays in the residency
        # tape as missing evidence (the gate ignores it before rza begins).
        self.rz_records.append({"kind": "unparseable", "error": str(exc)})

        self.bad_lines_consecutive = getattr(self, 'bad_lines_consecutive', 0) + 1

        print(f"[collect] malformed telemetry line skipped ({self.bad_lines} total, "

              f"{self.bad_lines_consecutive} consecutive): {exc}", flush=True)

        if self.bad_lines_consecutive >= 3:

            self.fail(f"invalid JSON line x{self.bad_lines_consecutive} consecutive: {exc}", "invalid_json")

            return True

        return False


    @staticmethod
    def perf_guard_summary_problem(r):
        if (not isinstance(r, list) or len(r) != 10
                or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in r)
                or r[0] not in (1, 2) or r[2] > 1 or r[3] > 1):
            return "corrupt g1 survival guard summary", "survival_guard_corrupt"
        if (r[3] > r[2] or r[6] > r[7] or r[8] != max(r[7]-4, 0)
                or (not r[2] and any(r[i] for i in (3, 4, 6, 7, 8, 9)))):
            return "contradictory guard state/counters", "survival_guard_contradictory"
        return None

    def note_perf_guard_context(self, event):
        r = event.get("record")
        if (not isinstance(r, list) or len(r) not in (46, 47)
                or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in r)
                or type(event.get("clock_hz")) is not int
                or event["clock_hz"] not in (1000000, 60750000)
                or type(event.get("heartbeat_tick")) is not int
                or not 0 <= event["heartbeat_tick"] <= 0xffffffff):
            return "corrupt guard event context/common clock", "survival_guard_corrupt"
        problem = self.perf_guard_summary_problem(r[:10])
        if problem:
            return problem
        if len(r) != (47 if r[0] == 2 else 46):
            return "guard version/record width mismatch", "survival_guard_corrupt"
        barriers = r[46] if r[0] == 2 else 0
        if barriers & ~0x3ff:
            return "unknown guard barrier bits", "survival_guard_corrupt"
        if barriers and r[3]:
            return "effective guard contradicts server barriers", "survival_guard_contradictory"
        n = min(r[7], 4)
        previous_stamp = None
        events = []
        for sequence in range(r[7]-n, r[7]):
            i = 10 + (sequence % 4)*9
            e = r[i:i+9]
            tick, hi, lo, cause, before, raw, post, applied, prevented = e
            stamp = (hi << 32) | lo
            if (cause > 8 or cause == 7 or not 1 <= before <= 160
                    or applied != min(post, before-1) or applied > post
                    or prevented > r[6] or (post >= before and not prevented)
                    or (previous_stamp is not None and stamp < previous_stamp)):
                return "guard debit/context contradicts cap or clock", "survival_guard_contradictory"
            previous_stamp = stamp
            events.append({"sequence": sequence+1, "tick": tick, "clock": stamp,
                           "cause": cause, "health_before": before,
                           "raw_incoming": raw, "post_mitigation": post,
                           "applied_debit": applied, "prevented_lethal": prevented})
        for slot in range(n, 4) if r[7] < 4 else ():
            if any(r[10+slot*9:10+(slot+1)*9]):
                return "unused guard context is nonzero", "survival_guard_contradictory"
        self.perf_guard_context = {"summary": r[:10], "clock_hz": event["clock_hz"],
                                   "heartbeat_tick": event["heartbeat_tick"], "events": events,
                                   "barriers": barriers}
        return None

    def note_perf_guard(self, event):
        marker = event.get("g1")
        frames = event.get("perf_frames", 0)
        required = (self.args.profile == "perf_stress_5m" and "d1" in event
                    and (event.get("player_health") != -1
                         or (type(frames) is int and frames > 0)))
        if marker is not None and (type(marker) is not int or marker != 1):
            return "corrupt g1 survival guard marker", "survival_guard_corrupt"
        context = self.perf_guard_context
        matching = context and context["heartbeat_tick"] == event.get("tick")
        expected = (self.require_g9_evidence or marker == 1
                    or (context and context["summary"][2])
                    or (self.perf_guard_previous and self.perf_guard_previous[2]))
        # Default-OFF controls emit no observer. Once requested, absence or a
        # late/stale observer cannot silently turn a guarded run into a control.
        if required and expected and not matching:
            return "missing heartbeat-matched bounded guard context", "survival_guard_missing"
        if not matching:
            if required and not expected:
                self.perf_survival_guard.update(requested=False, effective=False,
                    session=event["d1"][0], valid=True, coverage=self.perf_guard_coverage)
            return None
        if required and expected and marker != 1:
            return "G9 guarded fixture requires g1=1", "survival_guard_missing"
        r = context["summary"]
        problem = self.perf_guard_summary_problem(r)
        if problem:
            return problem
        version, session, requested, effective, refills, minimum, prevented, count, dropped, overflow = r
        self.perf_survival_guard = {
            "version": version, "session": session or None, "requested": bool(requested),
            "effective": bool(effective), "refills": refills,
            "minimum_health": minimum if session else None,
            "prevented_lethal": prevented, "events": count, "context_dropped": dropped,
            "overflow": overflow, "valid": bool(session),
            "mode": "diagnostic refill plus lethal-debit guard; not Beta gameplay",
            "coverage": self.perf_guard_coverage,
        }
        if not required:
            return None
        if session != event["d1"][0]:
            return "guard/death session identities disagree", "survival_guard_contradictory"
        if requested and marker != 1:
            return "requested guard is missing g1 marker", "survival_guard_missing"
        if requested != (marker == 1):
            return "guard marker contradicts requested state", "survival_guard_contradictory"
        barriers = context["barriers"]
        # Legacy v1 has no server barrier observation: only the main loading
        # fields can excuse it. v2 records the exact guard predicate's reasons.
        loading = event.get("loading_screen") is True or event.get("world_loaded") is False
        excluded = barriers & 0xff or loading
        if requested and (not 1 <= minimum <= 160 or overflow or barriers & 0x300
                          or (barriers & 1 and self.perf_guard_coverage["effective_rows"])
                          or (not effective and not excluded)):
            return "requested guard unavailable, dead minimum or overflow", "survival_guard_invalid"
        previous = self.perf_guard_previous
        if previous and (session != previous[1] or requested != previous[2]
                         or any(r[i] < previous[i] for i in (4, 6, 7, 8, 9))
                         or (requested and minimum > previous[5])):
            return "guard session/counters/minimum regressed", "survival_guard_contradictory"
        if requested:
            self.perf_survival_guard.update(context)
            coverage = self.perf_guard_coverage
            if excluded:
                coverage["excluded_rows"] += 1
                for bit, name in enumerate(coverage["barrier_rows"]):
                    if barriers & (1 << bit) or (name == "loading" and loading):
                        coverage["barrier_rows"][name] += 1
            elif effective:
                coverage["effective_rows"] += 1
        self.perf_guard_previous = r.copy()
        return None

    def note_death_telemetry(self, event):
        """Grade live stress survival from the session-valid real producer.

        d1 is [session, host deaths, host cause, guest deaths, guest cause].
        Session zero explicitly means unavailable; cause IDs are the append-only
        server_player_damage_cause enum. Legacy rows remain byte-for-byte in
        the receipt, but absent evidence cannot certify live service.
        """
        record = event.get("d1")
        health = event.get("player_health")
        stress = self.args.profile == "perf_stress_5m"
        frames = event.get("perf_frames", 0)
        required = stress and (health != -1 or (type(frames) is int and frames > 0))
        self.death_telemetry = {"version": 1, "valid": False, "session": None,
                                "host_deaths": None, "host_cause": None,
                                "guest_deaths": None, "guest_cause": None}
        if record is None:
            # The legacy hardware contradiction fails even without d1.
            if required and type(health) is int and health <= 0 and health != -1:
                return "primary player committed dead health; live service invalid", "player_died"
            if required:
                return "missing d1 authoritative death snapshot", "death_telemetry_missing"
            return None
        if (not isinstance(record, list) or len(record) != 5
                or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in record)
                or record[2] > 8 or record[4] > 8):
            return "corrupt d1 death snapshot", "death_telemetry_corrupt"
        session, host, host_cause, guest, guest_cause = record
        causes = ("unknown", "fall", "drown", "lava", "fire", "cactus",
                  "combat", "void", "suffocate")
        self.death_telemetry = {"version": 1, "valid": bool(session),
                                "session": session or None,
                                "host_deaths": host if session else None,
                                "host_cause": (causes[host_cause] if host else "none") if session else None,
                                "guest_deaths": guest if session else None,
                                "guest_cause": (causes[guest_cause] if guest else "none") if session else None}
        if not session:
            if required:
                return "local death snapshot unavailable", "death_telemetry_unavailable"
            return None
        if ((host == 0 and host_cause != 0) or (guest == 0 and guest_cause != 0)
                or ("deaths" in event and (type(event["deaths"]) is not int or event["deaths"] != host))
                or ("death_cause" in event and event["death_cause"] != self.death_telemetry["host_cause"])
                or ("anchor_deaths" in event and event["anchor_deaths"] != host)):
            return "death snapshot contradicts counts/cause/legacy fields", "death_telemetry_contradictory"
        if stress and (host or guest):
            return "authoritative committed death invalidates live service", "player_died"
        if not required:
            return None
        if type(health) is not int or health <= 0:
            return "living primary body unavailable or dead", "player_died" if type(health) is int else "death_telemetry_unavailable"
        if self.death_previous and session != self.death_previous[0]:
            return "session identity changed during live coverage", "death_telemetry_session_changed"
        if host or guest:
            return "authoritative committed death invalidates live service", "player_died"
        self.death_previous = record.copy()
        return None

    def fail(self, reason: str, failure_class: str = "unclassified") -> None:
        self.fail_reason = reason
        self.failure_class = failure_class
        self.done = True

    def _note_mp_move_cadence(self, event: dict) -> None:
        # REMOTE_PLAY is enum server_remote_state value 4. A reconnect or a
        # non-playing beat ends the pair; neither crosses a session boundary.
        if event.get("mp_state") != 4:
            self.mp_move_previous = None
            return
        for key in ("mp_gen", "mp_move_ticks", "mp_move_samples"):
            if type(event.get(key)) is not int:
                self.fail(f"movement cadence: live heartbeat missing integer {key}",
                          "mp_move_cadence_missing")
                return
        previous = self.mp_move_previous
        self.mp_move_previous = dict(event)
        if previous is None or previous["mp_gen"] != event["mp_gen"]:
            return
        pair = {
            "seq": [previous.get("seq"), event.get("seq")],
            "up_ms": [previous.get("up_ms"), event.get("up_ms")],
            "d_ticks": event["mp_move_ticks"] - previous["mp_move_ticks"],
            "d_samples": event["mp_move_samples"] - previous["mp_move_samples"],
            # Older heartbeat producers have no rpcdrop key. Zero is the
            # conservative budget: a lost sample cannot be excused by it.
            "d_rpcdrop": (int(event.get("rpcdrop", 0) or 0)
                          - int(previous.get("rpcdrop", 0) or 0)),
        }
        totals = self.mp_move_cadence["totals"]
        for name, key in (("ticks", "d_ticks"), ("samples", "d_samples"),
                          ("rpcdrop", "d_rpcdrop")):
            totals[name] += pair[key]
        self.mp_move_cadence["pairs"] += 1
        worst = self.mp_move_cadence["worst_pair"]
        if worst is None or (
                abs(pair["d_ticks"] - pair["d_samples"]), pair["d_ticks"]) > (
                abs(worst["d_ticks"] - worst["d_samples"]), worst["d_ticks"]):
            self.mp_move_cadence["worst_pair"] = pair
        dt, ds, dr = pair["d_ticks"], pair["d_samples"], pair["d_rpcdrop"]
        if dt < 0 or ds < 0 or dr < 0 or ds > dt or dt - ds > dr:
            self.fail(
                "movement cadence pair "
                f"seq={pair['seq'][0]}->{pair['seq'][1]} "
                f"up_ms={pair['up_ms'][0]}->{pair['up_ms'][1]} "
                f"d_ticks={dt} d_samples={ds} d_rpcdrop={dr}",
                "mp_move_cadence_failed")

    def pass_(self, reason: str) -> None:
        # Both normal completion and expected watchdog dumps arrive here.
        if getattr(self.args, "require_service_exposure", False):
            exposure = self.service_exposure_report()
            if not exposure["complete"]:
                self.fail("service diagnostic: " + "; ".join(exposure["errors"]),
                          "service_exposure_failed")
                return
        if (self.args.profile == "perf_stress_5m"
                and (self.require_g9_evidence or self.perf_survival_guard.get("requested"))
                and not self.perf_guard_coverage["effective_rows"]):
            self.fail("requested guard never effective after admission",
                      "survival_guard_invalid")
            return
        # A dump cannot certify an incomplete TNT (or mob motion) fixture run.
        if self.require_g9_evidence and (self.lq_max is None or self.lq_version != 2):
            self.fail("G9 evidence missing: no v2 light-queue array in this run",
                      "light_queue_evidence_missing")
            return
        if (self.profile.get("scenario") == "save_reload"
                and self.save_reload.complete_error()):
            self.fail("save/reload: " + self.save_reload.complete_error(),
                      "save_reload_failed")
            return
        if (self.profile.get("scenario") == "pool_selftest"
                and self.pool_selftest.complete_error()):
            self.fail("pool self-test: " + self.pool_selftest.complete_error(),
                      "pool_selftest_failed")
            return
        if (self.profile.get("scenario") == "mob_motion"
                and (self.mm_terminal is None or self.mm_terminal[1] != 2)):
            self.fail("no terminal mob motion PASS on the heartbeat",
                      "mob_motion_evidence_failed")
            return
        if self.require_tnt_evidence:
            beat = self.last_heartbeat_event
            payload = tuple(beat.get(key) for key in
                            ("tnt_gen", "tnt_st", "tnt_bits", "tnt_primed_left",
                             "tnt_carved", "tnt_pad", "tnt_tick"))
            if (beat.get("tnt_valid") != 1 or beat.get("tnt_st") != 2
                    or self.tnt_terminal is None
                    or payload != self.tnt_terminal):
                self.fail("latest TNT heartbeat does not carry this run's terminal PASS",
                          "tnt_evidence_failed")
                return
        self.pass_reason = reason
        self.done = True

    def failure_context(self) -> dict:
        latest = self.latest or {}
        hb = self.last_heartbeat_event or latest
        ctx = {
            "latest_event": latest.get("event", ""),
            "context_source_event": hb.get("event", ""),
            "latest_build": latest.get("build_id", ""),
            "latest_profile": latest.get("profile", ""),
            "last_heartbeat_build": hb.get("build_id", ""),
            "last_heartbeat_profile": hb.get("profile", ""),
            "latest_tick": hb.get("tick", None),
            "world_loaded": hb.get("world_loaded", None),
            # menus_dwell hardware-risk probe (2026-09-02): was the world just
            # CREATED (or the current world) creative? See hw_telemetry.h's
            # `creative` field comment for why this is gstate.config_creative,
            # not server_local.creative_mode.
            "creative": hb.get("creative", None),
            "position": {
                "x": hb.get("x", None),
                "y": hb.get("y", None),
                "z": hb.get("z", None),
            },
            "fps": hb.get("fps", None),
            "fps_window": parse_heartbeat_fps_window(hb),
            "mem1_free_kb": hb.get("mem1_free_kb", None),
            "mem2_free_kb": hb.get("mem2_free_kb", None),
            "srv_rt_us": hb.get("srv_rt_us", None),
            "srv_wt_us": hb.get("srv_wt_us", None),
            # ci-1101/37: which lock stalled the drain and who held it.
            "lqd_wait": self.lqd_wait,
            "cflood": hb.get("cflood", None),
            "ndisp": hb.get("ndisp", None),
            "max_ndisp": self.max_ndisp,
            "mm_terminal": (dict(zip(("gen", "st", "bits", "recur_ok",
                                      "recur_bad", "rise_milli",
                                      "step_milli", "tick"),
                                     self.mm_terminal))
                            if self.mm_terminal else None),
            "mm_cost": dict(self.mm_cost),
            "map_step": hb.get("map_step", None),
            "max_map_step": self.max_map_step,
            "np_proof": self.np_proof,
            # The terminal evidence itself (A23, review A20-1): the
            # highest map_max seen on a beat that carried the proof. A
            # PASS says "the leg reached its terminal step, observed",
            # so the claim can be re-read instead of trusted.
            "np_proof_step_max": self.np_proof_step_max,
            "overflow_beats": self.overflow_beats,
            "map_sign_reached": self.map_sign_reached,
            "save_migrate_ms": self.max_smig_ms,
            "save_migrate_regions": self.max_smig_regions,
            "save_migrate_chunks": self.max_smig_chunks,
            "save_migrate_signs": self.max_smig_signs,
            "save_migrate_expected": self.max_smig_expected,
            "save_migrate_verified": self.max_smig_verified,
            "max_far_view_distance": self.max_fvd,
            "max_fring": self.max_fring,
            "world_loaded_seen": self.world_loaded_seen,
            "walk_displacement": self.walk_displacement(),
            "max_mrs_cycles": self.max_mrs_cycles,
            "mrs_wedge_seen": self.mrs_wedge_seen,
            "max_hb_gap_ms": self.max_hb_gap_ms,
            # Far-ring keep-up probe (2026-09-03 reach-sweep gate 4): "does
            # worldgen keep up while walking". far_missing/far_settle_ms are
            # the last heartbeat's raw values; the max_* pair are running
            # maxima across the whole run (see __init__ comment).
            "far_missing": hb.get("far_missing", None),
            "max_far_missing": self.max_far_missing,
            "far_missing_max_reported": hb.get("far_missing_max", None),
            "max_far_missing_reported": self.max_far_missing_reported,
            "far_settle_ms": hb.get("far_settle_ms", None),
            "max_far_settle_ms": self.max_far_settle_ms,
            "residency_gate": self.residency_verdict(),
            "srv_gen_us": hb.get("srv_gen_us", None),
            "max_srv_gen_us": self.max_srv_gen_us,
            "srv_gen_cols": hb.get("srv_gen_cols", None),
            "max_srv_gen_cols": self.max_srv_gen_cols,
            "srv_gen_backlog": hb.get("srv_gen_backlog", None),
            "max_srv_gen_backlog": self.max_srv_gen_backlog,
            "near_evict_defer": hb.get("near_evict_defer", None),
            "ms_bfs": hb.get("ms_bfs", None),
            "ms_light": hb.get("ms_light", None),
            "ms_lod": hb.get("ms_lod", None),
            "ms_build": hb.get("ms_build", None),
            "ms_bfs_max": hb.get("ms_bfs_max", None),
            "ms_light_max": hb.get("ms_light_max", None),
            "ms_lod_max": hb.get("ms_lod_max", None),
            "ms_build_max": hb.get("ms_build_max", None),
            # P2-c telemetry (2026-09-03 round-2 frame-drop root-cause doc):
            # the mesher WORKER's own build wall-clock, previously F3-only.
            "ms_mesh_worker": hb.get("ms_mesh_worker", None),
            "ms_mesh_worker_max": hb.get("ms_mesh_worker_max", None),
            "max_ms_mesh_worker": self.max_ms_mesh_worker,
            "mem2_lock_wait": hb.get("mem2_lock_wait", None),
            "max_mem2_lock_wait": self.max_mem2_lock_wait,
            "wd_main": hb.get("wd_main", None),
            "wd_main_name": phase_name(WD_MAIN_NAMES, hb.get("wd_main")),
            "wd_srv": hb.get("wd_srv", None),
            "wd_srv_name": phase_name(WD_SRV_NAMES, hb.get("wd_srv")),
            "wd_worker": hb.get("wd_worker", None),
            "wd_worker_name": phase_name(WD_WORKER_NAMES, hb.get("wd_worker")),
            "wd_hb_gap_max_ms": hb.get("wd_hb_gap_max_ms", None),
            # T-a/T-b/T-c telemetry-robustness fields: raw last-heartbeat
            # values alongside the collector's own running trackers, same
            # pairing convention as far_missing/max_far_missing above.
            "tel_fail": hb.get("tel_fail", None),
            "max_tel_fail": self.max_tel_fail,
            "tel_last_errno": hb.get("tel_last_errno", None),
            "tel_quiesced": hb.get("tel_quiesced", None),
            "tel_quiesced_seen": self.tel_quiesced_seen,
            "frame_counter": hb.get("frame_counter", None),
            "max_frame_counter": self.max_frame_counter,
            "mem2_carve_free_kb": hb.get("mem2_carve_free_kb", None),
            "min_mem2_carve_free_kb": self.min_mem2_carve_free_kb,
        }
        if self.last_hb is not None:
            ctx["seconds_since_last_heartbeat"] = round(
                max(0.0, time.monotonic() - self.last_hb), 3
            )
        return ctx

    def residency_verdict(self) -> dict:
        """The residency gate over the live tape, with the SAME required legs
        and induced option residency_gate_tape uses (closure item 11)."""
        return residency_verdict_for_profile(
            self.rz_records, self.args.profile,
            getattr(self.args, "residency_require_moving", "") or "",
            self.residency_allow_induced, self.residency_expect_fault)

    def write_result(self) -> int:
        # Include trailing loss and incomplete spans even when the run FAILs.
        if self.g9_capture.windows or self.g9_capture.errors or self.g9_capture.seal:
            self.write_g9_capture()
        if self.g9_cpu.hz is not None or self.g9_cpu.pacing:
            self.write_g9_cpu()
        if self.vehicle_pc_replay:
            if self.pass_reason and self.vehicle_pc_source_cursor != len(self.vehicle_pc_source_lines):
                self.pass_reason = None
                self.fail("vehicle_pc_replay_evidence_changed_or_incomplete", "vehicle_smoke_evidence_failed")
            if any((self.out / name).exists() for name in ("PASS.json", "FAIL.json", "PC_REPLAY.json")):
                raise ValueError("vehicle_pc_replay output isolation violated; certification artifacts must remain untouched")
            if hashlib.sha256(self.vehicle_pc_source_path.read_bytes()).hexdigest() != self.vehicle_pc_source_sha256:
                self.pass_reason = None
                self.fail("vehicle_pc_replay_source_tape_changed", "vehicle_smoke_evidence_failed")
        result = {
            "run_id": self.args.run_id,
            "profile": self.args.profile,
            "seed": self.seed,
            # Always retain collector wall time, including watchdog/timeout
            # failures where the synchronous migration never returned and
            # therefore could not publish a final smig_ms value.
            "elapsed_seconds": round(
                max(0.0, time.monotonic() - self.started), 3),
            "heartbeats": self.hb_count,
            # H4 review fix (10): every rise of the heartbeat's chunk-save
            # failure counter, so a PASS states that saves failed during it.
            "save_fail_warnings": list(self.save_fail_warnings),
            **({"join_ms": self.join_ms,
                "join_ms_missing": self.join_ms is None,
                **({"join_slow_warning": f"join {self.join_ms} ms over "
                    f"{MP_JOIN_WARN_MS} ms warn limit (fail limit "
                    f"{MP_JOIN_FAIL_MS})"}
                   if self.join_ms is not None
                   and MP_JOIN_WARN_MS < self.join_ms <= MP_JOIN_FAIL_MS else {})}
               if self.args.profile in MP_JOIN_GATE_PROFILES else {}),
            # G9 R6: the light-queue maxima over the run (None = no "lq").
            "light_queue": self.light_queue_summary(),
            "g9_cpu": self.g9_cpu.report(),
            "g9_capture": self.g9_capture.report(),
            "service_observer": self.service_observer_report(),
            "service_exposure": (self.service_exposure_report()
                                 if getattr(self.args, "require_service_exposure", False) else None),
            "telemetry_gaps": list(self.telemetry_gaps),
            "max_rt_us": self.max_rt_us,
            "max_wt_us": self.max_wt_us,
            "max_oom": self.max_oom,
            "max_trunc": self.max_trunc,
            # Scenario-gate evidence: shows WHAT a PASS proved, not just that
            # the console survived.
            "scenario": self.profile.get("scenario"),
            "world_loaded_seen": self.world_loaded_seen,
            "max_far_view_distance": self.max_fvd,
            "max_fring": self.max_fring,
            "max_ndisp": self.max_ndisp,
            "mm_terminal": (dict(zip(("gen", "st", "bits", "recur_ok",
                                      "recur_bad", "rise_milli",
                                      "step_milli", "tick"),
                                     self.mm_terminal))
                            if self.mm_terminal else None),
            "mm_cost": dict(self.mm_cost),
            "max_map_step": self.max_map_step,
            "np_proof": self.np_proof,
            # The terminal evidence itself (A23, review A20-1): the
            # highest map_max seen on a beat that carried the proof. A
            # PASS says "the leg reached its terminal step, observed",
            # so the claim can be re-read instead of trusted.
            "np_proof_step_max": self.np_proof_step_max,
            "overflow_beats": self.overflow_beats,
            "map_sign_reached": self.map_sign_reached,
            "creative_seen": self.creative_seen,
            "save_migrate_state": self.max_smig_state,
            "save_migrate_ms": self.max_smig_ms,
            "save_migrate_regions": self.max_smig_regions,
            "save_migrate_chunks": self.max_smig_chunks,
            "save_migrate_signs": self.max_smig_signs,
            "save_migrate_expected": self.max_smig_expected,
            "save_migrate_verified": self.max_smig_verified,
            "walk_displacement": self.walk_displacement(),
            "max_mrs_cycles": self.max_mrs_cycles,
            "mrs_wedge_seen": self.mrs_wedge_seen,
            # Peak heartbeat gap observed this run, whether it PASSed or
            # FAILed -- see the __init__ comment: this is the quantitative
            # trace of how close the run came to the watchdog's ~3s trip,
            # now recorded even on a clean PASS.
            "max_hb_gap_ms": self.max_hb_gap_ms,
            # Far-ring keep-up probe (2026-09-03 reach-sweep gate 4): recorded
            # on every run (PASS or FAIL) so the runner can gate on "does
            # worldgen keep up while walking" the same way it already gates
            # on max_fring/world_loaded_seen -- see the __init__ comment for
            # the max_far_missing vs max_far_missing_reported distinction.
            "max_far_missing": self.max_far_missing,
            "max_far_missing_reported": self.max_far_missing_reported,
            "max_far_settle_ms": self.max_far_settle_ms,
            "residency_gate": self.residency_verdict(),
            "max_srv_gen_us": self.max_srv_gen_us,
            "max_srv_gen_cols": self.max_srv_gen_cols,
            "max_srv_gen_backlog": self.max_srv_gen_backlog,
            # P2-c telemetry (2026-09-03 round-2 frame-drop root-cause doc,
            # "#3 Instrumentation gap"): recorded on every run (PASS or FAIL),
            # same treatment as the srv_gen_* trio above.
            "max_ms_mesh_worker": self.max_ms_mesh_worker,
            "max_mem2_lock_wait": self.max_mem2_lock_wait,
            # T-a/T-b/T-c telemetry-robustness fields (recorded on every run,
            # PASS or FAIL, same treatment as the srv_gen_*/mem2_lock_wait
            # pairs above -- see the __init__ comment for each field's
            # tracking rationale).
            "max_tel_fail": self.max_tel_fail,
            "tel_quiesced_seen": self.tel_quiesced_seen,
            "max_frame_counter": self.max_frame_counter,
            "min_mem2_carve_free_kb": self.min_mem2_carve_free_kb,
            # Fix round brief finding #3 (2026-09-07): recorded on every run,
            # PASS or FAIL, same treatment as max_tel_fail above -- see the
            # __init__ comment for why both stay 0 without --cw-carve-census.
            "max_tel_drop": self.max_tel_drop,
            "max_staged_skipped": self.max_staged_skipped,
            "min_tel_it": self.min_tel_it,
            "min_tel_sends": self.min_tel_sends,
            "max_tel_smax_us": self.max_tel_smax_us,
            "max_tel_qmax": self.max_tel_qmax,
            "max_tel_hb_repl": self.max_tel_hb_repl,
            "max_tel_wto": self.max_tel_wto,
            "max_tel_stg_us": self.max_tel_stg_us,
            "worst_tel_stage": self.worst_tel_stage,
            "tel_stages_seen": self.tel_stages_seen,
            "max_tel_pmax_us": self.max_tel_pmax_us,
            "max_tel_xmax_us": self.max_tel_xmax_us,
            "max_tel_abd": self.max_tel_abd,
            "last_tel_pret": self.last_tel_pret,
            "last_tel_prev": self.last_tel_prev,
            "last_tel_sret": self.last_tel_sret,
            "max_loading_suppressed": self.max_loading_suppressed,
            "storage": self.storage.summary(),
            "overlay_spill": self.overlay_spill.summary(),
            "vehicle_smoke": (self.vehicle_smoke.summary()
                              if self.args.profile == VEHICLE_SMOKE_PROFILE else None),
            "vehicle_manifest_sha256": self.vehicle_manifest_sha256,
            "vehicle_pc_replay": self.vehicle_pc_replay,
            "split_offer_piston": (self.offer_piston.summary() if self.args.profile == OFFER_PISTON_PROFILE else None),
            "vehicle_certification": ("pc_replay_noncertifying" if self.vehicle_pc_replay else "calibrated_functional" if self.args.profile == VEHICLE_SMOKE_PROFILE
                                      and self.pass_reason and not self.fail_reason else
                                      "calibration_required" if self.args.profile == VEHICLE_SMOKE_PROFILE else None),
            "vehicle_tape_sha256": (hashlib.sha256((self.out / "vehicle-wire.ndjson").read_bytes()).hexdigest()
                                    if self.args.profile == VEHICLE_SMOKE_PROFILE and (self.out / "vehicle-wire.ndjson").is_file() else None),
            "tps_end": self.tps_end,
            "anchor": self.anchor.summary() if self.args.profile in ANCHOR_PROFILES else None,
            "regrow": (self.regrow.summary()
                       if self.args.profile == REGROW_PROFILE else None),
            "save_reload": (self.save_reload.summary()
                            if self.args.profile == SAVE_RELOAD_PROFILE else None),
            "pool_selftest": (dict(self.pool_selftest.summary(),
                                   build_id=(self.last_heartbeat_event or {}).get("build_id"))
                              if self.args.profile == POOL_SELFTEST_PROFILE else None),
            # The recovery marker evidence, on PASS and FAIL alike, so an
            # artifact states WHICH transitions were proven -- None on every
            # other profile, same convention as "anchor" above.
            "split_recovery": (self.recovery.summary()
                               if self.args.profile == SPLIT_RECOVERY_PROFILE
                               else None),
            "split_portal_quit": (self.portal_quit.summary()
                                  if self.args.profile == PORTAL_QUIT_PROFILE
                                  else None),
            "split_dimensions_telemetry": (self.split_dims_rows
                                            if self.args.profile == "split_dims_6m"
                                            else None),
            "split_dimensions": (self.split_dims.summary()
                                 if self.args.profile == "split_dims_6m"
                                 else None),
            # The chat-login marker evidence and the server receipt, on PASS and
            # FAIL alike, so an artifact states WHAT the console typed, whether
            # the DEVICE side completed, and whether the server acknowledged it
            # -- None on every other profile, same convention as the two above.
            "chat_chord": (self.chat_chord.summary()
                           if self.args.profile == CHAT_CHORD_PROFILE
                           else None),
            "chat_login": (self.chat_login.summary()
                           if self.args.profile == CHAT_LOGIN_PROFILE
                           else None),
            # The play marker evidence, what each counted action matched (and
            # what it did not), the walked displacement and both snapshots --
            # on PASS and FAIL alike, None on every other profile.
            "mp_play": (self.mp_play.summary()
                        if self.args.profile == MP_PLAY_PROFILE else None),
            # hw1029b/G1: recorded, not graded (see self.mp_kinds). None when
            # the device never emitted an mp_kinds row.
            "mp_kinds": dict(self.mp_kinds) or None,
            "mp_move_cadence": (self.mp_move_cadence
                                if self.args.profile == "mp_lan_walk_10m" else None),
            "mp_pause_menu": (self.mp_pause_menu.summary()
                              if self.args.profile == "mp_pause_menu_3m" else None),
            # Checks this run could not grade at all, verbatim. Empty on every
            # console run; on a PC mp_play_10m run it names the heartbeat-
            # derived gates, which have no transport there (see
            # mp_play_pc_no_heartbeats).
            "not_graded": list(self.not_graded),
            "latest": self.latest,
            "last_heartbeat": self.last_heartbeat_event,
            "death_telemetry": self.death_telemetry,
            "perf_survival_guard": self.perf_survival_guard,
            "checkpoint_shutdown": ({
                "required": self.shutdown_required,
                "wait_bound_seconds": CHECKPOINT_SHUTDOWN_WAIT_SECONDS,
                "profile_done": self.shutdown_done,
                "rows": self.shutdown_rows,
                "failures": self.shutdown_errors,
                "last_counters": self.shutdown_last_counters,
                # main.c checks final ckio internally but sends only ok and
                # timing. The periodic arrays must not masquerade as final.
                "counters_are_final": False,
                "device_reconciled": self.shutdown_required and self.shutdown_error() is None,
            } if self.args.profile == "perf_stress_5m" else None),
            "first_guest_zero_heartbeat": self.first_guest_zero_heartbeat,
            "guest_deaths": self.guest_deaths,
            "route_reversals": self.route_reversals,
            "route_retrace_len": self.route_retrace_len,
            "route_oscillation_reason": self.route_oscillation_reason,
            "min_window_displacement": (self.min_window_displacement
                                        if self.args.profile == "split_play_8m" else None),
            "guest_total_distance_walked": (self.guest_total_distance_walked
                                            if self.args.profile == "split_play_8m" else None),
            **(self.guest_stall_window or {}),
            "release_performance_pass": bool(self.release_gate and self.pass_reason
                                             and not self.fail_reason),
            "certification_scope": ("offered_piston_scripted_functional" if self.args.profile == OFFER_PISTON_PROFILE else "vehicle_smoke_pc_replay_noncertifying" if self.vehicle_pc_replay else "vehicle_smoke_peaceful_functional" if self.args.profile == VEHICLE_SMOKE_PROFILE
                                    else "split_portal_quit_scripted_proxy" if self.args.profile == PORTAL_QUIT_PROFILE
                                    else "split_recovery_scripted_proxy" if self.args.profile == SPLIT_RECOVERY_PROFILE
                                    else "mp_play_unattended_lan_session" if self.args.profile == MP_PLAY_PROFILE
                                    else "chat_login_scripted_osk_delivery" if self.args.profile == CHAT_LOGIN_PROFILE
                                    else "split_anchor_feasibility" if self.args.profile in SPLIT_PROFILES else "gameplay_frame_performance" if self.release_gate
                                    else "profile_stability_only"),
        }
        if self.require_tnt_evidence:
            result["tnt"] = {
                "required": True,
                "terminal": self.tnt_terminal,
                "latest": {key: self.last_heartbeat_event.get(key)
                           for key in ("tnt_valid", "tnt_st", "tnt_bits",
                                       "tnt_primed_left", "tnt_carved", "tnt_pad",
                                       "tnt_gen", "tnt_tick")},
            }
        if self.require_residency:
            rz_gate = self.residency_verdict()
            result["residency_gate"] = rz_gate
            if rz_gate["status"] != "PASS" and not self.fail_reason:
                self.pass_reason = None
                self.fail_reason = ("residency gate " + rz_gate["status"] + ": "
                                    + "; ".join(rz_gate["reasons"][:4]))
                self.failure_class = "residency_gate_failed"
        result["telemetry_traces"] = read_telemetry_traces(self.out)
        inconclusive = (self.args.profile == "split_play_8m" and self.pass_reason
                        and not self.fail_reason and self.anchor.coverage_gaps)
        if self.vehicle_pc_replay:
            passed = bool(self.pass_reason and not self.fail_reason)
            result["contract"] = "CONTRACT-v1.2"
            result["source_tape_sha256"] = self.vehicle_pc_source_sha256
            result["source_tape"] = "source-tape.ndjson"
            result["evidence_provenance"] = "pc_diagnostic_replay"
            result["noncertifying"] = True
            result["status"] = "PC_REPLAY_PASS" if passed else "PC_REPLAY_FAIL"
            result["reason"] = self.fail_reason or self.pass_reason or "collector ended without pass condition"
            if not passed:
                result["failure_class"] = self.failure_class or "unclassified"
                result["failure_context"] = self.failure_context()
            with (self.out / "PC_REPLAY.json").open("x", encoding="utf-8") as f:
                f.write(json.dumps(result, indent=2))
            rc = 0 if passed else 1
        elif inconclusive:
            result["status"] = "INCONCLUSIVE"
            result["reason"] = ("telemetry coverage gap: missing frame times cannot "
                                "certify split anchor performance")
            result["failure_class"] = "anchor_telemetry_coverage_gap"
            (self.out / "FAIL.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
            rc = 1
        elif (self.pass_reason and not self.fail_reason
              and self.args.profile in ANCHOR_PROFILES
              and not self.anchor.pool.certifying()):
            # R5 (Sol R4 Major 1): historical or absent pool evidence is
            # graded, but NEVER written as a certifying PASS.json.
            result["status"] = "NONCERTIFYING_PASS"
            result["noncertifying"] = True
            result["reason"] = (self.pass_reason + " -- NOT certifying: column pool "
                                "evidence is " + self.anchor.pool_summary()["evidence"])
            (self.out / "NONCERTIFYING.json").write_text(json.dumps(result, indent=2),
                                                         encoding="utf-8")
            rc = 1
        elif (self.pass_reason and not self.fail_reason
              and self.args.profile == WORLD_COPY_PROFILE):
            # T16a review round 1 (Sol B2): NOT a PASS. The device half is
            # complete; PASS.json is written only by --world-copy-verdict,
            # which requires this file AND the card hashes.
            result["status"] = "PENDING_CARD"
            result["reason"] = (self.pass_reason + " -- device evidence only; "
                                "PASS.json needs --world-copy-verdict with the "
                                "card record")
            result["world_copy_device"] = self.wcc_device_verdict
            (self.out / WORLD_COPY_DEVICE_FILE).write_text(
                json.dumps(result, indent=2), encoding="utf-8")
            rc = 1
        elif self.pass_reason and not self.fail_reason:
            result["status"] = "PASS"
            result["reason"] = self.pass_reason
            (self.out / "PASS.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
            rc = 0
        else:
            result["status"] = "FAIL"
            result["reason"] = self.fail_reason or "collector ended without pass condition"
            result["failure_class"] = self.failure_class or "unclassified"
            result["failure_context"] = self.failure_context()
            (self.out / "FAIL.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
            rc = 1
        self.summary_path.write_text(render_summary(result), encoding="utf-8")
        for trace in result["telemetry_traces"]:
            print(f"[telemetry trace: {trace['path']}]\n{trace['text']}", flush=True)
        print(f"[{result['status']}] {result['reason']}", flush=True)
        return rc


def read_telemetry_traces(out: Path) -> list[dict]:
    traces = []
    for name in ("cw_tel_trace.txt", "stage_mailbox/previous_cw_tel_trace.txt"):
        path = out / name
        if path.is_file():
            with path.open("r", encoding="utf-8", errors="replace") as f:
                traces.append({"path": name, "text": f.read(4096)})
    return traces


def render_telemetry_gaps(gaps) -> str:
    """One summary line per missing-heartbeat gap, naming its device cause."""
    if not gaps:
        return ""
    return "".join(
        f"- Heartbeat gap: ticks {g.get('missing_ticks')} missing, "
        f"cause={g.get('cause')}, deltas={g.get('deltas')}, "
        f"tel_xmax_us={g.get('tel_xmax_us')}\n" for g in gaps)



# ---- residency-1005/4: the per-actor residency gate (wire schema v2) -------
# Closure items 9-11 (RESIDENCY-CLOSURE-LIST.md). A heartbeat carries, only
# once any residency activity exists:
#   "rzv": 2                         schema version
#   "rzt": <u32>                     server tick (near_send_seq) at publish
#   "rza": [[22 x u32 host], [22 x u32 guest]]   (server_local.h
#                                    enum server_local_rza_field; FULL u32,
#                                    no saturation)
#   "rzi": [[actor, slot, gen, epoch, attempt], ...]  0..4 induced marks
# Thresholds are Astra's: per actor pending <= 6 (total <= 12), every
# continuous refusal and requested-footprint service delay <= 40 server
# ticks, recovery (and every terminated attempt) <= 200 ticks, zero uninduced
# failures, nothing unresolved at completion, and every REQUIRED moving leg
# (actor[:world_slot]) needs ONE (actor, slot, world_gen, body_epoch) segment
# that logged >= 32 blocks of admitted ordinary movement. Healthy zero-demand
# segments qualify; observed demand must have attributable service. Veto
# attempts are REPORTED, never failures.
# Fail closed: malformed residency evidence is FAIL, missing evidence is
# UNPROVEN, and neither is ever coerced, dropped or graded PASS.
RZ_SCHEMA_VERSION = 2
# Tracks HW_TELEMETRY_HEARTBEAT_LINE_SIZE (source/platform/hw_telemetry.h):
# 12288 since mp-split-0912/54 S-A7, 13312 since residency-1005/4 (the v2
# residency keys). The UDP recvfrom cap and the selftest bound use it -- a
# UDP read cannot reassemble a datagram truncated by recvfrom.
# M3: retained pool/receipt and G9 drain/pacing payloads, measured together:
# 13,795 bytes at the widest, plus the 200-byte spare floor. CW_TEL_LINE
# and the C formatter must use the same capacity.
HEARTBEAT_LINE_SIZE = 13995
(RZA_WORLD_SLOT, RZA_WORLD_GEN, RZA_BODY_EPOCH, RZA_ATTEMPT, RZA_STATE,
 RZA_STREAK_AGE, RZA_STATE_AGE, RZA_LAST_ATTEMPT_TICKS, RZA_LAST_ATTEMPT_RESULT,
 RZA_PENDING, RZA_OLDEST_REQ_AGE, RZA_REQUESTS, RZA_LOADED,
 RZA_SERVICE_DELAY_MAX, RZA_LATENCY_MAX, RZA_RECOVERY_EXITS_OK,
 RZA_RECOVERY_FAILURES, RZA_RECOVERY_TICKS_MAX, RZA_RETRIES, RZA_NOTICES,
 RZA_VETO_ATTEMPTS, RZA_MOVED_DM) = range(22)
RZA_FIELDS = 22
RZI_FIELDS, RZI_MAX = 5, 4
# "rzf": session bitmask of the runtime fault arm's phases (closure item 12).
# Session-sticky: a bit that clears is malformed. An induced exception needs
# the arm to have been armed, entered, terminal, noticed and disarmed.
RZF_ARMED, RZF_ENTERED, RZF_TERMINAL, RZF_NOTICE = 1 << 0, 1 << 1, 1 << 2, 1 << 3
RZF_RETRY, RZF_DISARMED, RZF_REFUSED = 1 << 4, 1 << 5, 1 << 6
RZF_KNOWN_BITS = (1 << 7) - 1
RZF_INDUCED_REQUIRED = RZF_ARMED | RZF_ENTERED | RZF_TERMINAL | RZF_NOTICE | RZF_DISARMED
RZ_STATE_OK, RZ_STATE_REFUSING, RZ_STATE_RECOVERING = 0, 1, 2
RZ_STATE_FAILED, RZ_STATE_WAKE_HOLD = 3, 4
RZ_RESULT_FAILED = 2
RZ_SLOT_ABSENT = 255
RZ_U32_MAX = 0xFFFFFFFF
RZ_LIMIT_PENDING, RZ_LIMIT_PENDING_TOTAL = 6, 12
RZ_LIMIT_DELAY_TICKS, RZ_LIMIT_RECOVERY_TICKS = 40, 200
RZ_MIN_PROGRESS = 32.0                       # blocks
RZ_MIN_MOVED_DM = int(RZ_MIN_PROGRESS * 10)  # MOVED_DM is 0.1-block units
# Session-monotonic per-actor fields: a decrease is MALFORMED evidence (a
# counter reset would otherwise erase an earlier failure between samples).
RZA_SESSION_MONOTONIC = (RZA_BODY_EPOCH, RZA_ATTEMPT) + tuple(range(RZA_REQUESTS, RZA_FIELDS))
# Live fields that must read 0 on an absent (slot 255) row.
RZA_LIVE_FIELDS = (RZA_STATE, RZA_STREAK_AGE, RZA_STATE_AGE, RZA_PENDING, RZA_OLDEST_REQ_AGE)
RZ_ACTOR_NAMES = ("host", "guest")
# Default required moving legs per profile (closure item 11), used when
# --residency-require-moving is not given. (actor, world_slot or None=any).
#  * far_walk_6m / keepup_walk_6m / host_vehicle_smoke_8m: the host walks/rides.
#  * split_anchor_6m: the guest is a SYNTHETIC anchor, not a moving body --
#    only the host is required (a present guest is still safety-gated).
#  * split_play_8m / split_recovery_6m: two actual actors, both move.
#  * split_dims_6m: host:0,host:1 per the R4 brief. JUDGEMENT CALL, recorded:
#    the profile's guest walks through a portal (>= DIMS_MIN_NETHER_WALK = 16
#    blocks in the Nether) and the host makes a scripted portal trip
#    (main.c SPLIT_DIMS_HOST_AWAY/BACK); whether each host world leg reaches
#    32 blocks of admitted movement is NOT yet measured on hardware. If it
#    does not, the gate FAILs that leg honestly -- override with
#    -ResidencyRequireMoving (e.g. guest:1) only with a stated reason.
#  * idle profiles: no movement; unknown profiles need an explicit policy.
RZ_PROFILE_REQUIRED_LEGS = {
    "far_walk_6m": "host",
    "keepup_walk_6m": "host",
    "host_vehicle_smoke_8m": "host",
    "split_anchor_6m": "host",
    "split_play_8m": "host,guest",
    "split_recovery_6m": "host,guest",
    "split_dims_6m": "host:0,host:1",
    "heartbeat_selftest": "none",
    "redstone_clock_2m": "none",
    "far_idle_2m": "none",
    "perf_stress_5m": "none",
}


def parse_residency_legs(text: str) -> list:
    """`host`, `guest`, `host:0`, `guest:1`, ... comma list, or `none`."""
    text = (text or "").strip()
    if text.lower() == "none":
        return []
    legs = []
    for token in text.split(","):
        token = token.strip()
        m = re.fullmatch(r"(host|guest)(?::([0-3]))?", token)
        if not m:
            raise ValueError(f"bad residency moving leg {token!r} "
                             "(expected host|guest[:0-3], comma separated, or none)")
        leg = (RZ_ACTOR_NAMES.index(m.group(1)),
               int(m.group(2)) if m.group(2) is not None else None)
        if leg not in legs:
            legs.append(leg)
    if not legs:
        raise ValueError("empty residency moving-leg list (use none)")
    return legs


def residency_required_legs(profile: str, require_moving: str = "") -> list:
    # Overrides may add a more specific leg, but cannot waive a profile's
    # required coverage (in particular, `none` cannot certify a moving run).
    legs = parse_residency_legs(RZ_PROFILE_REQUIRED_LEGS.get(profile, "none"))
    for leg in parse_residency_legs(require_moving) if require_moving else []:
        if leg not in legs:
            legs.append(leg)
    return legs


def parse_residency_fault(text: str):
    if not text:
        return None
    match = re.fullmatch(r"(host|guest)@([1-9][0-9]*)", text)
    if not match or int(match[2]) > 3600:
        raise ValueError("expected residency fault host@seconds|guest@seconds (1..3600)")
    return RZ_ACTOR_NAMES.index(match[1]), int(match[2])


def residency_verdict_for_profile(records, profile, require_moving="",
                                  allow_induced=False, expect_fault=""):
    verdict = residency_gate_eval(records, allow_induced or bool(expect_fault),
                                  residency_required_legs(profile, require_moving),
                                  expect_fault)
    verdict.update(profile=profile, moving_override=require_moving,
                   expect_fault=expect_fault)
    if profile not in RZ_PROFILE_REQUIRED_LEGS and not require_moving:
        verdict["codes"] = sorted(set(verdict["codes"]) | {"missing_leg_policy"})
        verdict["reasons"].append(f"{profile}: explicit residency moving-leg policy required")
        if verdict["status"] == "PASS":
            verdict["status"] = "UNPROVEN"
    return verdict


def residency_leg_name(leg) -> str:
    actor, slot = leg
    return RZ_ACTOR_NAMES[actor] + ("" if slot is None else f":{slot}")


def _rz_is_u32(v) -> bool:
    return type(v) is int and 0 <= v <= RZ_U32_MAX


def _rz_check_u32(v, where: str, errors: list) -> None:
    if v is None:
        errors.append(("null_value", f"{where} is null"))
    elif type(v) is not int:
        errors.append(("malformed_rza", f"{where} is not an integer ({type(v).__name__})"))
    elif not 0 <= v <= RZ_U32_MAX:
        errors.append(("malformed_rza", f"{where} = {v} outside u32"))


def residency_tape_record(event):
    """The per-heartbeat record the residency gate needs.

    None for a non-heartbeat event. A heartbeat WITHOUT any rz key yields an
    "absent" record (fine before residency evidence starts, missing evidence
    after it). A heartbeat carrying any of rza/rzv/rzt/rzi is validated in
    full; every problem stays in the record as an error -- never coerced to
    0, never dropped."""
    if not isinstance(event, dict) or event.get("event") != "heartbeat":
        return None
    tick = event.get("tick")
    if not any(k in event for k in ("rza", "rzv", "rzt", "rzi", "rzf")):
        return {"kind": "absent", "tick": tick, "overflow": event.get("overflow") is True}
    errors = []
    rec = {"kind": "rz", "tick": tick, "rza": None, "rzt": None, "rzi": [], "rzf": 0,
           "errors": errors, "rze": None}
    if "rze" in event:
        rze = event["rze"]
        n = len(errors)
        if not (isinstance(rze, list) and len(rze) == 2
                and all(isinstance(r, list) and len(r) == 8 for r in rze)):
            errors.append(("malformed_rze", "rze must contain two eight-value receipts"))
        else:
            for a, row in enumerate(rze):
                for k, value in enumerate(row):
                    _rz_check_u32(value, f"rze[{a}][{k}]", errors)
            if len(errors) == n:
                rec["rze"] = [list(r) for r in rze]
    if "rzf" in event:
        n = len(errors)
        _rz_check_u32(event["rzf"], "rzf", errors)
        if len(errors) == n:
            if event["rzf"] & ~RZF_KNOWN_BITS:
                errors.append(("malformed_rzf", f"rzf {event['rzf']:#x} has unknown phase bits"))
            else:
                rec["rzf"] = event["rzf"]
    if "rzv" not in event:
        errors.append(("version_mismatch", "residency heartbeat lacks rzv"))
    elif event["rzv"] is None:
        errors.append(("null_value", "rzv is null"))
    elif type(event["rzv"]) is not int or event["rzv"] != RZ_SCHEMA_VERSION:
        errors.append(("version_mismatch",
                       f"rzv {event['rzv']!r} != {RZ_SCHEMA_VERSION}"))
    if "rzt" not in event:
        errors.append(("malformed_rza", "residency heartbeat lacks rzt"))
    else:
        n = len(errors)
        _rz_check_u32(event["rzt"], "rzt", errors)
        if len(errors) == n:
            rec["rzt"] = event["rzt"]
    rza = event.get("rza")
    if "rza" not in event:
        errors.append(("malformed_rza", "residency heartbeat lacks rza"))
    elif rza is None:
        errors.append(("null_value", "rza is null"))
    elif not (isinstance(rza, list) and len(rza) == 2
              and all(isinstance(r, list) for r in rza)):
        errors.append(("malformed_rza", "rza is not two rows"))
    elif any(len(r) != RZA_FIELDS for r in rza):
        errors.append(("malformed_rza",
                       f"rza row lengths {[len(r) for r in rza]} != {RZA_FIELDS}"))
    else:
        n = len(errors)
        for a, row in enumerate(rza):
            for i, v in enumerate(row):
                _rz_check_u32(v, f"rza[{RZ_ACTOR_NAMES[a]}][{i}]", errors)
        if len(errors) == n:
            for a, row in enumerate(rza):
                who = RZ_ACTOR_NAMES[a]
                slot = row[RZA_WORLD_SLOT]
                if slot != RZ_SLOT_ABSENT and not 0 <= slot <= 3:
                    errors.append(("invalid_identity", f"{who}: world slot {slot}"))
                elif slot != RZ_SLOT_ABSENT and row[RZA_WORLD_GEN] == 0:
                    errors.append(("invalid_identity", f"{who}: present in slot {slot} with world_gen 0"))
                elif slot == RZ_SLOT_ABSENT and any(row[i] for i in RZA_LIVE_FIELDS):
                    errors.append(("invalid_identity", f"{who}: absent row with live state/age/pending"))
                if row[RZA_STATE] > RZ_STATE_WAKE_HOLD:
                    errors.append(("malformed_rza", f"{who}: state {row[RZA_STATE]}"))
                if row[RZA_LAST_ATTEMPT_RESULT] > RZ_RESULT_FAILED:
                    errors.append(("malformed_rza", f"{who}: last attempt result {row[RZA_LAST_ATTEMPT_RESULT]}"))
            if len(errors) == n:
                rec["rza"] = [list(r) for r in rza]
    if "rzi" in event:
        rzi = event["rzi"]
        if not (isinstance(rzi, list) and len(rzi) <= RZI_MAX
                and all(isinstance(m, list) and len(m) == RZI_FIELDS for m in rzi)):
            errors.append(("malformed_rzi", f"rzi {rzi!r} is not 0..{RZI_MAX} marks of {RZI_FIELDS}"))
        else:
            n = len(errors)
            for k, mark in enumerate(rzi):
                for i, v in enumerate(mark):
                    _rz_check_u32(v, f"rzi[{k}][{i}]", errors)
            if len(errors) == n:
                for mark in rzi:
                    if mark[0] > 1 or not 0 <= mark[1] <= 3 or mark[2] == 0:
                        errors.append(("malformed_rzi", f"rzi mark {mark} has an invalid identity"))
                rec["rzi"] = [tuple(m) for m in rzi]
    for key in ("x", "z", "gx", "gz"):
        if key in event:
            v = event[key]
            if type(v) not in (int, float) or not math.isfinite(v):
                errors.append(("nonfinite_position", f"{key} = {v!r}"))
    return rec


def _rz_unresolved(row) -> bool:
    # FAILED (state 3) is unresolved even when both age fields read 0.
    return bool(row[RZA_STATE] != RZ_STATE_OK or row[RZA_STREAK_AGE] or row[RZA_STATE_AGE])


def residency_gate_eval(records, allow_induced_failures=False, required_legs=None,
                       expect_fault=""):
    """PASS / FAIL / UNPROVEN over residency_tape_record() records.

    records may also hold {"kind": "unparseable"} entries for lines the
    ingest could not decode. required_legs: [(actor, slot or None)], default
    host. FAIL beats UNPROVEN beats PASS."""
    if required_legs is None:
        required_legs = [(0, None)]
    expected_fault = parse_residency_fault(expect_fault)
    fails, unproven = [], []

    def fail(code, msg):
        if (code, msg) not in fails:
            fails.append((code, msg))

    def unprove(code, msg):
        if (code, msg) not in unproven:
            unproven.append((code, msg))

    seq = [r for r in records if r is not None]
    first = next((i for i, r in enumerate(seq) if r.get("kind") == "rz"), None)
    if first is None:
        return {"status": "FAIL" if expected_fault else "UNPROVEN",
                "reasons": ["no rza residency evidence in the tape"]
                    + (["expected induced episode not exercised"] if expected_fault else []),
                "codes": ["no_evidence"] + (["induced_not_exercised"] if expected_fault else []),
                "actors": [],
                "required_legs": [residency_leg_name(l) for l in required_legs]}
    later_rz = [i for i, r in enumerate(seq) if r.get("kind") == "rz"]
    last_rz = later_rz[-1]
    valid = []          # (seq index, record) of fully valid rz records
    for i in range(first, len(seq)):
        r = seq[i]
        kind = r.get("kind")
        if kind == "absent":
            what = "an overflow substitute" if r.get("overflow") else "a heartbeat"
            if i < last_rz:
                unprove("missing_middle", f"{what} at tick {r.get('tick')} lacks residency evidence "
                        "after rza began (evidence gap)")
            else:
                unprove("missing_final", f"{what} at tick {r.get('tick')} lacks residency evidence "
                        "after rza began (no final evidence)")
        elif kind == "unparseable":
            unprove("unparseable_line", f"undecodable telemetry line after rza began: {r.get('error')}")
        elif kind == "rz":
            if r["errors"]:
                for code, msg in r["errors"]:
                    fail(code, f"tick {r.get('tick')}: {msg}")
            else:
                valid.append((i, r))
    # Cross-record ordering / monotonicity over the valid rows.
    marks_seen = []        # [(valid index first seen, mark)]
    prev = None
    for vi, (_, r) in enumerate(valid):
        if prev is not None:
            if r["rzt"] < prev["rzt"]:
                fail("out_of_order", f"rzt went backwards {prev['rzt']} -> {r['rzt']}")
            for a in range(2):
                for f_ in RZA_SESSION_MONOTONIC:
                    if r["rza"][a][f_] < prev["rza"][a][f_]:
                        fail("counter_decrease",
                             f"{RZ_ACTOR_NAMES[a]}: session field {f_} decreased "
                             f"{prev['rza'][a][f_]} -> {r['rza'][a][f_]} at rzt {r['rzt']}")
            if prev["rze"] is not None:
                if r["rze"] is None:
                    fail("missing_rze", "retained attempt receipts disappeared")
                else:
                    for a in range(2):
                        old, new = prev["rze"][a], r["rze"][a]
                        delta = r["rza"][a][RZA_RECOVERY_FAILURES] - prev["rza"][a][RZA_RECOVERY_FAILURES]
                        if old[3] and delta == 0:
                            if (new[:5] != old[:5]
                                    or any(old[k] and old[k] != new[k] for k in (5, 6, 7))):
                                fail("mutated_rze", "terminal/retry/completion receipt was rewritten")
            lost = [m for _, m in marks_seen if m not in r["rzi"]]
            if lost:
                fail("malformed_rzi", f"induced mark(s) {lost} vanished at rzt {r['rzt']}")
            if prev["rzf"] & ~r["rzf"]:
                fail("malformed_rzf", f"session fault-phase bits cleared {prev['rzf']:#x} -> "
                     f"{r['rzf']:#x} at rzt {r['rzt']}")
        for m in r["rzi"]:
            if all(m != seen for _, seen in marks_seen):
                marks_seen.append((vi, m))
        prev = r
    rows_all = [r for _, r in valid]
    rzf_session = 0
    for r in rows_all:
        rzf_session |= r["rzf"]
    total_pending_max = max((sum(r["rza"][a][RZA_PENDING] for a in range(2)) for r in rows_all), default=0)
    if total_pending_max > RZ_LIMIT_PENDING_TOTAL:
        fail("pending_limit", f"total pending {total_pending_max} > {RZ_LIMIT_PENDING_TOTAL}")
    actors, exempt = [], []
    used_marks = set()
    for a, name in enumerate(RZ_ACTOR_NAMES):
        R = [r["rza"][a] for r in rows_all]
        present_idx = [i for i, row in enumerate(R) if row[RZA_WORLD_SLOT] != RZ_SLOT_ABSENT]
        info = {"actor": name, "present": bool(present_idx)}
        actors.append(info)
        if not R:
            continue
        # -- failure transitions (every row, not only the last) and the
        #    scoped induced exception.
        windows = []
        failures = []
        for i, row in enumerate(R):
            d = row[RZA_RECOVERY_FAILURES] - (R[i - 1][RZA_RECOVERY_FAILURES] if i else 0)
            if d > 0:
                failures.append((i, d))
        for i, d in failures:
            row = R[i]
            ident = (a, row[RZA_WORLD_SLOT], row[RZA_WORLD_GEN], row[RZA_BODY_EPOCH], row[RZA_ATTEMPT])
            where = (f"{name}: recovery failure at rzt {rows_all[i]['rzt']} "
                     f"(slot {ident[1]}, gen {ident[2]}, epoch {ident[3]}, attempt {ident[4]})")
            if not allow_induced_failures:
                fail("uninduced_failure", where + " (no induced exception allowed)")
                continue
            receipt = rows_all[i]["rze"]
            receipt = receipt[a] if receipt is not None else None
            terminal_ticks = row[RZA_LAST_ATTEMPT_TICKS]
            terminal_source = "observed_failed_row"
            if receipt is not None:
                ident = (a,) + tuple(receipt[:4])
                terminal_ticks = receipt[4]
                terminal_source = "retained_receipt"
            elif row[RZA_LAST_ATTEMPT_RESULT] != RZ_RESULT_FAILED:
                # Legacy v2 producer: rzf's terminal bit is set only by the
                # marked bounded failure, and persists across the owner retry.
                # Do NOT mislabel the last SUCCESS duration as failure timing.
                prior = R[i - 1] if i else None
                if (prior is not None and prior[RZA_STATE] == RZ_STATE_RECOVERING
                        and tuple(prior[:3]) == tuple(row[:3])
                        and row[RZA_ATTEMPT] == prior[RZA_ATTEMPT] + 1
                        and row[RZA_RETRIES] == prior[RZA_RETRIES] + 1
                        and prior[RZA_STATE_AGE] <= RZ_LIMIT_RECOVERY_TICKS
                        and prior[RZA_STATE_AGE] + rows_all[i]["rzt"]
                            - rows_all[i - 1]["rzt"] >= RZ_LIMIT_RECOVERY_TICKS):
                    ident = (a,) + tuple(prior[:4])
                    terminal_ticks = None
                    terminal_source = "legacy_bounded_terminal_phase"
            mark = next((m for vi, m in marks_seen if vi <= i and m == ident), None)
            if (d != 1 or mark is None or mark in used_marks
                    or row[RZA_WORLD_SLOT] == RZ_SLOT_ABSENT
                    or tuple(row[:3]) != ident[1:4]
                    or (receipt is None and terminal_ticks is not None
                        and row[RZA_LAST_ATTEMPT_RESULT] != RZ_RESULT_FAILED)) :
                marks = [m for vi, m in marks_seen if vi <= i]
                fail("uninduced_failure", where + f" matches no unused induced mark (marks {marks}, "
                     f"failures +{d}, last result {row[RZA_LAST_ATTEMPT_RESULT]})")
                continue
            used_marks.add(mark)
            ok = True
            if rows_all[i]["rzf"] & RZF_INDUCED_REQUIRED != RZF_INDUCED_REQUIRED:
                fail("induced_fault_phases", where + f": failure-row fault phases rzf {rows_all[i]['rzf']:#x} "
                     f"lack armed|entered|terminal|notice|disarmed ({RZF_INDUCED_REQUIRED:#x})")
                ok = False
            if terminal_ticks is not None and terminal_ticks > RZ_LIMIT_RECOVERY_TICKS:
                fail("induced_late_terminal", where + f": terminated after "
                     f"{terminal_ticks} ticks > {RZ_LIMIT_RECOVERY_TICKS}")
                ok = False
            start = next((j for j in range(i + 1)
                          if (a,) + tuple(R[j][:4]) == ident), None)
            if start is None:
                fail("induced_missing_entry", where + ": no observed marked attempt entry")
                continue
            noticed = any(R[j][RZA_NOTICES] > (R[j - 1][RZA_NOTICES] if j else 0)
                          for j in range(start, i + 1))
            if not noticed:
                fail("induced_no_notice", where + ": no residency notice reached the pane "
                     "by the failure sample")
                ok = False
            retry_ok = None
            retry_at = None
            body_key = tuple(row[k] for k in (RZA_WORLD_SLOT, RZA_WORLD_GEN, RZA_BODY_EPOCH))
            episode_end = next((k for k in range(i + 1, len(R))
                                if tuple(R[k][f] for f in (RZA_WORLD_SLOT, RZA_WORLD_GEN,
                                                           RZA_BODY_EPOCH)) != body_key), len(R))
            for k in range(i, len(R)):
                if k >= episode_end:
                    break
                if k and R[k][RZA_RETRIES] > R[k - 1][RZA_RETRIES]:
                    retry_at = k
                    # A single heartbeat can coalesce terminal failure,
                    # owner retry and success, while the pane still reports
                    # REFUSING until its next sample. Bind the success to a
                    # fresh exit increment, then require a settled sample of
                    # that same body/attempt with no intervening failure.
                    for completed in range(k, episode_end):
                        c = R[completed]
                        if (c[RZA_RECOVERY_EXITS_OK] <= R[completed - 1][RZA_RECOVERY_EXITS_OK]
                                or c[RZA_ATTEMPT] != ident[4] + 1
                                or c[RZA_LAST_ATTEMPT_RESULT] != 1):
                            continue
                        retry_ok = next((m for m in range(completed, episode_end)
                                         if R[m][RZA_ATTEMPT] == ident[4] + 1
                                         and rows_all[m]["rzt"] - rows_all[completed]["rzt"] <= RZ_LIMIT_DELAY_TICKS
                                         and R[m][RZA_RECOVERY_FAILURES] == c[RZA_RECOVERY_FAILURES]
                                         and R[m][RZA_RETRIES] == c[RZA_RETRIES]
                                         and R[m][RZA_RECOVERY_EXITS_OK] == c[RZA_RECOVERY_EXITS_OK]
                                         and R[m][RZA_LAST_ATTEMPT_RESULT] == 1
                                         and not _rz_unresolved(R[m]) and R[m][RZA_PENDING] == 0), None)
                        if retry_ok is not None:
                            break
                    if retry_ok is not None:
                        break
            if retry_ok is None:
                fail("induced_no_retry", where + ": no later owner retry followed by a "
                     "successful fresh recovery")
                ok = False
            end = retry_ok if retry_ok is not None else i
            if retry_ok is not None:
                end_receipt = rows_all[end]["rze"]
                if receipt is not None and (end_receipt is None
                        or end_receipt[a][:5] != receipt[:5]
                        or end_receipt[a][5:7] != [ident[4] + 1, ident[4] + 1]
                        or end_receipt[a][7] != R[end][RZA_LAST_ATTEMPT_TICKS]):
                    fail("induced_receipt_mismatch", where + ": retry/completion receipt mismatch")
                    ok = False
                if (not rows_all[retry_at]["rzf"] & RZF_DISARMED
                        or not rows_all[retry_at]["rzf"] & RZF_RETRY):
                    fail("induced_fault_phases", where + ": retry lacks disarm/retry phase evidence")
                    ok = False
                # The first observed attempt row already includes its entry
                # request (R7 guest90 has exactly one). Count that increment
                # only across a known same-body predecessor with a live
                # recovery request; never borrow demand across an identity
                # boundary or count an older request as this episode's work.
                request_base = start
                if (start > 0 and tuple(R[start - 1][:3]) == body_key
                        and R[start - 1][RZA_ATTEMPT] != ident[4]
                        and R[start][RZA_STATE] == RZ_STATE_RECOVERING
                        and R[start][RZA_PENDING] > 0):
                    request_base = start - 1
                if (R[end][RZA_REQUESTS] <= R[request_base][RZA_REQUESTS]
                        or R[end][RZA_LOADED] <= R[start][RZA_LOADED]):
                    fail("induced_no_service", where + ": no requests and loads attributable to fault/retry episode")
                    ok = False
                if not any(R[m][RZA_MOVED_DM] > R[end][RZA_MOVED_DM]
                           for m in range(end + 1, episode_end)):
                    fail("induced_no_movement", where + ": no resumed movement after successful retry")
                    ok = False
            if ok:
                windows.append((start, end))
                exempt.append({"actor": name, "mark": list(mark),
                               "rzt_window": [rows_all[start]["rzt"], rows_all[end]["rzt"]],
                               "attempt_ticks": terminal_ticks, "terminal_source": terminal_source})

        def exempt_row(i):
            return any(s <= i <= e for s, e in windows)
        # -- per-row limits. Session maxima are judged where they RISE, so an
        #    exempt induced episode's value does not fail every later row.
        for i, row in enumerate(R):
            rzt = rows_all[i]["rzt"]
            if row[RZA_PENDING] > RZ_LIMIT_PENDING:
                fail("pending_limit", f"{name}: pending {row[RZA_PENDING]} > {RZ_LIMIT_PENDING} at rzt {rzt}")
            if (row[RZA_STATE] in (RZ_STATE_RECOVERING, RZ_STATE_WAKE_HOLD)
                    and row[RZA_STATE_AGE] > RZ_LIMIT_RECOVERY_TICKS):
                fail("recovery_limit", f"{name}: recovery/hold age {row[RZA_STATE_AGE]} > "
                     f"{RZ_LIMIT_RECOVERY_TICKS} at rzt {rzt}")
            if row[RZA_LAST_ATTEMPT_TICKS] > RZ_LIMIT_RECOVERY_TICKS:
                fail("attempt_duration_limit", f"{name}: terminated attempt took "
                     f"{row[RZA_LAST_ATTEMPT_TICKS]} > {RZ_LIMIT_RECOVERY_TICKS} ticks")
            if row[RZA_RECOVERY_TICKS_MAX] > RZ_LIMIT_RECOVERY_TICKS:
                fail("recovery_limit", f"{name}: successful recovery {row[RZA_RECOVERY_TICKS_MAX]} > "
                     f"{RZ_LIMIT_RECOVERY_TICKS} ticks")
            if exempt_row(i):
                continue
            if row[RZA_STREAK_AGE] > RZ_LIMIT_DELAY_TICKS:
                fail("refusal_limit", f"{name}: continuous refusal {row[RZA_STREAK_AGE]} ticks > "
                     f"{RZ_LIMIT_DELAY_TICKS} at rzt {rzt}")
            if row[RZA_OLDEST_REQ_AGE] > RZ_LIMIT_DELAY_TICKS:
                fail("service_delay_limit", f"{name}: request service delay {row[RZA_OLDEST_REQ_AGE]} "
                     f"ticks > {RZ_LIMIT_DELAY_TICKS} at rzt {rzt}")
            for f_, code, what in ((RZA_LATENCY_MAX, "refusal_limit", "closed refusal streak"),
                                   (RZA_SERVICE_DELAY_MAX, "service_delay_limit", "request service delay")):
                rose = i == 0 or row[f_] > R[i - 1][f_]
                if rose and row[f_] > RZ_LIMIT_DELAY_TICKS:
                    fail(code, f"{name}: {what} max {row[f_]} ticks > {RZ_LIMIT_DELAY_TICKS} at rzt {rzt}")
        # -- disappearance while unresolved, and completion.
        for i in range(1, len(R)):
            if (R[i - 1][RZA_WORLD_SLOT] != RZ_SLOT_ABSENT and _rz_unresolved(R[i - 1])
                    and R[i][RZA_WORLD_SLOT] == RZ_SLOT_ABSENT):
                fail("actor_disappeared_unresolved",
                     f"{name}: body vanished at rzt {rows_all[i]['rzt']} while state "
                     f"{R[i - 1][RZA_STATE]} was unresolved")
        last = R[-1]
        if _rz_unresolved(last):
            fail("unresolved_at_completion",
                 f"{name}: state {last[RZA_STATE]} (streak age {last[RZA_STREAK_AGE]}, "
                 f"state age {last[RZA_STATE_AGE]}) unresolved at completion")
        if last[RZA_PENDING] or last[RZA_OLDEST_REQ_AGE]:
            fail("pending_at_completion", f"{name}: pending requests at completion")
        # -- segments: (slot, world_gen, body_epoch) contiguous runs of
        #    present rows; deltas are never inherited across a boundary.
        segments, cur = [], None
        for i, row in enumerate(R):
            key = (row[RZA_WORLD_SLOT], row[RZA_WORLD_GEN], row[RZA_BODY_EPOCH])
            if row[RZA_WORLD_SLOT] == RZ_SLOT_ABSENT:
                cur = None
                continue
            if cur is None or cur["key"] != key or valid[i][0] != valid[i - 1][0] + 1:
                cur = {"key": key, "first": i, "last": i}
                segments.append(cur)
            else:
                cur["last"] = i
        seg_info = []
        for s in segments:
            f0, f1 = R[s["first"]], R[s["last"]]
            requests = f1[RZA_REQUESTS] - f0[RZA_REQUESTS]
            loaded = f1[RZA_LOADED] - f0[RZA_LOADED]
            activity_fields = (RZA_ATTEMPT, RZA_LAST_ATTEMPT_TICKS,
                               RZA_LAST_ATTEMPT_RESULT) + tuple(range(RZA_REQUESTS, RZA_VETO_ATTEMPTS))
            active = (any(_rz_unresolved(R[j]) or R[j][RZA_PENDING] or R[j][RZA_OLDEST_REQ_AGE]
                          for j in range(s["first"], s["last"] + 1))
                      or any(R[j][f] > R[j - 1][f] for f in activity_fields
                             for j in range(s["first"] + 1, s["last"] + 1)))
            induced_here = any(start <= s["last"] and end >= s["first"]
                               for start, end in windows)
            gate_state = "DEMAND_OPEN" if active else "NO_DEMAND"
            # A session counter first rising at an identity boundary cannot
            # be credited to either segment. It is also not proof of healthy
            # zero demand. Keep this sampling ambiguity non-certifying.
            boundary_activity = (s["first"] > 0 and any(
                f0[f] > R[s["first"] - 1][f] for f in activity_fields))
            if not active and boundary_activity:
                gate_state = "DEMAND_OPEN"
                unprove("boundary_activity_unattributed",
                        f"{name}: demand/recovery activity first observed at identity boundary")
            if induced_here:
                gate_state = "INDUCED_RECOVERED"
            elif any(m[0] == a and tuple(m[1:4]) == s["key"] for _, m in marks_seen):
                gate_state = "INDUCED_OPEN"
            elif active and requests > 0 and loaded > 0 and not _rz_unresolved(f1) \
                    and not f1[RZA_PENDING] and not f1[RZA_OLDEST_REQ_AGE]:
                gate_state = "DEMAND_SERVED"
            if active and not induced_here and (requests <= 0 or loaded <= 0):
                fail("demand_unserved", f"{name}: slot {s['key'][0]} gen {s['key'][1]} "
                     f"epoch {s['key'][2]} demand lacks same-segment service "
                     f"(requests {requests}, loaded {loaded})")
            if s["last"] + 1 < len(R) and _rz_unresolved(f1):
                fail("identity_changed_unresolved", f"{name}: unresolved state at segment boundary")
            if f1[RZA_PENDING] or f1[RZA_OLDEST_REQ_AGE]:
                fail("pending_at_completion", f"{name}: segment ends with pending requests")
            seg_info.append({"world_slot": s["key"][0], "world_gen": s["key"][1],
                             "body_epoch": s["key"][2],
                             "rzt": [rows_all[s["first"]]["rzt"], rows_all[s["last"]]["rzt"]],
                             "gate_state": gate_state,
                             "requests": requests, "loaded": loaded,
                             "moved_dm": f1[RZA_MOVED_DM] - f0[RZA_MOVED_DM],
                             "moved_blocks": (f1[RZA_MOVED_DM] - f0[RZA_MOVED_DM]) / 10.0})
        info.update({
            "world_slots": sorted({R[i][RZA_WORLD_SLOT] for i in present_idx}),
            "pending_max": max(r[RZA_PENDING] for r in R),
            "streak_age_max": max(max(r[RZA_STREAK_AGE] for r in R), last[RZA_LATENCY_MAX]),
            "service_delay_max": max(max(r[RZA_OLDEST_REQ_AGE] for r in R), last[RZA_SERVICE_DELAY_MAX]),
            "recovery_ticks_max": last[RZA_RECOVERY_TICKS_MAX],
            "recovery_ok": last[RZA_RECOVERY_EXITS_OK],
            "recovery_failures": last[RZA_RECOVERY_FAILURES],
            "retries": last[RZA_RETRIES], "notices": last[RZA_NOTICES],
            "requests": last[RZA_REQUESTS], "loaded": last[RZA_LOADED],
            "veto_attempts": last[RZA_VETO_ATTEMPTS],
            "moved_blocks": last[RZA_MOVED_DM] / 10.0,
            "unresolved_at_end": _rz_unresolved(last),
            "segments": seg_info})
    if any(mark not in used_marks for _, mark in marks_seen):
        fail("induced_not_exercised", "induced mark without its terminal failure")
    if expected_fault:
        actor, _ = expected_fault
        if (rzf_session & RZF_REFUSED or len(marks_seen) != 1 or len(exempt) != 1
                or exempt[0]["actor"] != RZ_ACTOR_NAMES[actor]):
            fail("induced_not_exercised", f"expected {expect_fault}: no single fully proven matching fault episode")
    # -- movement remains separate from demand: streaming that keeps up
    # needs no request. No segment can erase another actor/segment's failure.
    for leg in required_legs:
        a, slot = leg
        lname = residency_leg_name(leg)
        segs = [s for s in actors[a].get("segments", [])
                if slot is None or s["world_slot"] == slot]
        if not segs:
            fail("missing_required_actor", f"{lname}: no residency evidence for a required moving leg")
            continue
        if not any(s["moved_dm"] >= RZ_MIN_MOVED_DM for s in segs):
            best = max(segs, key=lambda s: (s["moved_blocks"], s["loaded"], s["requests"]))
            fail("no_moving_segment",
                 f"{lname}: no single world/body segment moved "
                 f">= {RZ_MIN_PROGRESS:g} blocks (best: requests {best['requests']}, loaded "
                 f"{best['loaded']}, moved {best['moved_blocks']:g} in slot {best['world_slot']} "
                 f"gen {best['world_gen']} epoch {best['body_epoch']})")
    status = "FAIL" if fails else "UNPROVEN" if unproven else "PASS"
    both = fails + unproven
    return {"status": status, "reasons": [m for _, m in both],
            "codes": sorted({c for c, _ in both}), "actors": actors,
            "required_legs": [residency_leg_name(l) for l in required_legs],
            "allow_induced": bool(allow_induced_failures),
            "induced_exempt": exempt, "rzf": rzf_session,
            "total_pending_max": total_pending_max,
            "rows": len(valid)}


def residency_parse_tape_lines(text: str):
    """Raw NDJSON -> (events, records). An undecodable line is kept as an
    "unparseable" record (fail closed), never skipped or fatal."""
    events, records = [], []
    for n, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except (json.JSONDecodeError, ValueError) as exc:
            records.append({"kind": "unparseable", "error": f"line {n}: {exc}"})
            events.append(None)
            continue
        events.append(event)
        rec = residency_tape_record(event)
        if rec is not None:
            records.append(rec)
    return events, records


def residency_tape_profile(events, profile: str, profile_explicit: bool) -> str:
    """An explicit --profile wins; otherwise the profile every heartbeat names
    (if one known profile), else the parser default."""
    if profile_explicit:
        return profile
    names = {e.get("profile") for e in events
             if isinstance(e, dict) and e.get("event") == "heartbeat"}
    if len(names) == 1:
        (only,) = names
        if isinstance(only, str) and only:
            return only
    # A tape with no identifiable policy must not silently inherit the
    # parser's idle default (which could waive its movement requirement).
    return "unidentified_profile"


def residency_gate_tape(path: str, negatives: bool, profile: str = "far_idle_2m",
                        require_moving: str = "", allow_induced: bool = False,
                        profile_explicit: bool = True, expect_fault: str = "") -> int:
    """Evaluate an ndjson heartbeat tape (exit 0 only on PASS); with
    negatives, also prove that each single-fault mutation of the SAME tape
    FAILs or is UNPROVEN (a gate that cannot fail is no gate)."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    events, recs = residency_parse_tape_lines(text)
    profile = residency_tape_profile(events, profile, profile_explicit)
    legs = residency_required_legs(profile, require_moving)
    verdict = residency_verdict_for_profile(recs, profile, require_moving, allow_induced, expect_fault)
    print(json.dumps(verdict, indent=2))
    if verdict["status"] != "PASS":
        print(f"[{verdict['status']}] residency tape: " + "; ".join(verdict["reasons"][:6]), flush=True)
        return 1
    if not negatives:
        return 0
    import copy

    hb = [i for i, e in enumerate(events)
          if isinstance(e, dict) and e.get("event") == "heartbeat" and "rza" in e]
    if len(hb) < 3:
        print("[FAIL] negatives need >= 3 residency heartbeats", flush=True)
        return 1
    mid, last = hb[len(hb) // 2], hb[-1]

    def mutated(fn):
        m = copy.deepcopy(events)
        fn(m)
        recs_m = [r for r in (residency_tape_record(e) for e in m if e is not None) if r]
        return residency_verdict_for_profile(recs_m, profile, require_moving, allow_induced, expect_fault)["status"]

    def present_actor(m, i):
        return 1 if m[i]["rza"][1][RZA_WORLD_SLOT] != RZ_SLOT_ABSENT else 0

    def put(m, i, field, value, a=None):
        a = present_actor(m, i) if a is None else a
        m[i]["rza"][a][field] = value

    def every(m, a, field, value):
        for i in hb:
            m[i]["rza"][a][field] = value

    def bump_from(m, start, a, field, by):
        for i in hb:
            if i >= start:
                m[i]["rza"][a][field] += by

    cases = {
        "missing_evidence": lambda m: [m[i].pop(k, None) for i in hb for k in ("rza", "rzv", "rzt", "rzi", "rzf")],
        "missing_middle": lambda m: [m[mid].pop(k, None) for k in ("rza", "rzv", "rzt", "rzi", "rzf")],
        "missing_final": lambda m: [m[last].pop(k, None) for k in ("rza", "rzv", "rzt", "rzi", "rzf")],
        "version_mismatch": lambda m: m[mid].__setitem__("rzv", 1),
        "malformed_rza": lambda m: m[mid]["rza"][0].pop(),
        "null_counter": lambda m: put(m, mid, RZA_REQUESTS, None, 0),
        "rzt_backwards": lambda m: m[hb[0]].__setitem__("rzt", m[last]["rzt"] + 1),
        "streak_41": lambda m: (put(m, mid, RZA_STATE, RZ_STATE_REFUSING), put(m, mid, RZA_STREAK_AGE, 41)),
        "service_delay_41": lambda m: (put(m, mid, RZA_PENDING, 1), put(m, mid, RZA_OLDEST_REQ_AGE, 41)),
        "pending_7": lambda m: put(m, mid, RZA_PENDING, 7),
        "recovery_201": lambda m: (put(m, mid, RZA_STATE, RZ_STATE_RECOVERING), put(m, mid, RZA_STATE_AGE, 201)),
        "uninduced_failure": lambda m: bump_from(m, mid, present_actor(m, mid), RZA_RECOVERY_FAILURES, 1),
        "failure_then_reset": lambda m: put(m, mid, RZA_RECOVERY_FAILURES, m[mid]["rza"][0][RZA_RECOVERY_FAILURES] + 1, 0),
        "unresolved_at_end": lambda m: (put(m, last, RZA_STATE, RZ_STATE_REFUSING), put(m, last, RZA_STREAK_AGE, 3)),
        "failed_zero_ages": lambda m: put(m, last, RZA_STATE, RZ_STATE_FAILED),
    }
    for leg in legs:
        a = leg[0]
        lname = residency_leg_name(leg)
        cases[f"{lname}_demand_no_loads"] = lambda m, a=a: (
            every(m, a, RZA_LOADED, 0), bump_from(m, mid, a, RZA_REQUESTS, 1))
        cases[f"{lname}_no_progress"] = lambda m, a=a: every(m, a, RZA_MOVED_DM, 0)
        cases[f"{lname}_absent"] = lambda m, a=a: [
            m[i]["rza"].__setitem__(a, [RZ_SLOT_ABSENT] + [0] * (RZA_FIELDS - 1)) for i in hb]
    bad = []
    for name, fn in cases.items():
        st = mutated(fn)
        print(f"negative {name}: {st}", flush=True)
        if st == "PASS":
            bad.append(name)
    if bad:
        print(f"[FAIL] residency gate PASSED mutated tapes: {bad}", flush=True)
        return 1
    print("[PASS] authentic tape PASSES; every single-fault mutation FAILS/UNPROVEN", flush=True)
    return 0


def render_summary(result: dict) -> str:
    trace_text = "".join(
        f"\nTelemetry trace ({t['path']}; check embedded run id):\n```text\n{t['text']}\n```\n"
        for t in result.get("telemetry_traces", []))
    latest = result.get("latest") or {}
    last_hb = result.get("last_heartbeat") or {}
    anchor = result.get("anchor") or {}
    anchor_warnings = "".join(f"- {warning}\n" for warning in anchor.get("warnings", []))
    coverage_line = ""
    if result.get("profile") == "split_play_8m":
        coverage_line = (
            f"- Telemetry coverage gap: inconclusive windows={anchor.get('inconclusive_windows')}, "
            f"missing ticks={anchor.get('missing_ticks')}, "
            f"missing frames={anchor.get('missing_frames')}, "
            f"received frames={anchor.get('frames')}, "
            f"frame coverage={anchor.get('frame_coverage')}, "
            f"intervals={anchor.get('coverage_gaps')}\n")
    failure_class = result.get("failure_class")
    failure_line = f"- Failure class: `{failure_class}`\n" if failure_class else ""
    ctx = result.get("failure_context") or {}
    phase_line = ""
    last_line = ""
    if ctx:
        phase_line = (
            f"- Last watchdog phases: MAIN={ctx.get('wd_main_name')}({ctx.get('wd_main')}) "
            f"SRV={ctx.get('wd_srv_name')}({ctx.get('wd_srv')}) "
            f"WRK={ctx.get('wd_worker_name')}({ctx.get('wd_worker')})\n"
        )
        timing_line = (
            f"- Last main timings: bfs={ctx.get('ms_bfs')}ms "
            f"light={ctx.get('ms_light')}ms lod={ctx.get('ms_lod')}ms "
            f"build={ctx.get('ms_build')}ms max=("
            f"{ctx.get('ms_bfs_max')}, {ctx.get('ms_light_max')}, "
            f"{ctx.get('ms_lod_max')}, {ctx.get('ms_build_max')}) "
            f"hb_gap_max={ctx.get('wd_hb_gap_max_ms')}ms\n"
        )
        pos = ctx.get("position") or {}
        last_line = (
            f"- Last telemetry: event=`{ctx.get('latest_event')}` "
            f"tick=`{ctx.get('latest_tick')}` world_loaded=`{ctx.get('world_loaded')}` "
            f"creative=`{ctx.get('creative')}` "
            f"pos=({pos.get('x')}, {pos.get('y')}, {pos.get('z')}) "
            f"near_evict_defer={ctx.get('near_evict_defer')}\n"
        )
    return (
        f"# Wii Hardware Run {result['status']}\n\n"
        f"{trace_text}"
        f"- Run id: `{result.get('run_id')}`\n"
        f"- Profile: `{result.get('profile')}`\n"
        f"- Reason: {result.get('reason')}\n"
        f"{failure_line}"
        f"{phase_line}"
        f"{timing_line if ctx else ''}"
        f"{last_line}"
        f"- Heartbeats: {result.get('heartbeats')}\n"
        f"- Max server rt/wt: {result.get('max_rt_us')}us / {result.get('max_wt_us')}us\n"
        f"- Max OOM/trunc: {result.get('max_oom')} / {result.get('max_trunc')}\n"
        f"{coverage_line}"
        f"{anchor_warnings}"
        f"{render_storage(result.get('storage'))}"
        f"{render_overlay_spill(result.get('overlay_spill'))}"
        f"- Max mesher worker build: {result.get('max_ms_mesh_worker')}ms, "
        f"max mem2 lock-wait: {result.get('max_mem2_lock_wait')}\n"
        f"- Far-ring keep-up (gate 4): missing max={result.get('max_far_missing')} "
        f"(device-reported {result.get('max_far_missing_reported')}), "
        f"settle max={result.get('max_far_settle_ms')}ms, "
        f"srv gen max={result.get('max_srv_gen_us')}us/"
        f"{result.get('max_srv_gen_cols')}cols, "
        f"backlog max={result.get('max_srv_gen_backlog')}\n"
        f"- Telemetry robustness: tel_fail max={result.get('max_tel_fail')}, "
        f"quiesced this run={result.get('tel_quiesced_seen')}, "
        f"frame_counter max={result.get('max_frame_counter')}, "
        f"mem2 carve free min={result.get('min_mem2_carve_free_kb')}KB, "
        f"tel_drop max={result.get('max_tel_drop')}, "
        f"staged_skipped max={result.get('max_staged_skipped')}\n"
        # Sender self-telemetry (2026-09-07 starvation fix): the only
        # host-visible proof of whether the sender LWP was SCHEDULED.
        f"- Telemetry sender: loop iters/s min={result.get('min_tel_it')}, "
        f"sends/s min={result.get('min_tel_sends')}, "
        f"single send max={result.get('max_tel_smax_us')}us, "
        f"ring occupancy max={result.get('max_tel_qmax')}/5, "
        f"heartbeats superseded={result.get('max_tel_hb_repl')}\n"
        # Sender stage (2026-09-08): the line that answers "WHERE was it
        # stuck?" rather than only "it was stuck". A large dwell beside
        # stage=send/poll names an IOS call; beside stage=idle it means the
        # sender was alive but starved of records -- a different fault.
        f"- Telemetry stage: worst dwell={result.get('max_tel_stg_us')}us "
        f"in stage={result.get('worst_tel_stage')}, "
        f"stages seen={result.get('tel_stages_seen')}, "
        f"single poll max={result.get('max_tel_pmax_us')}us, "
        f"single send max={result.get('max_tel_xmax_us')}us, "
        f"nonblock abandoned={result.get('max_tel_abd')}, "
        f"last poll ret/revents={result.get('last_tel_pret')}/"
        f"{result.get('last_tel_prev')}, "
        f"last send ret={result.get('last_tel_sret')}\n"
        f"{render_telemetry_gaps(result.get('telemetry_gaps'))}"
        f"- Latest build: `{latest.get('build_id') or last_hb.get('build_id', '')}`\n"
        f"- Latest fps: `{latest.get('fps') if latest.get('fps') is not None else last_hb.get('fps', '')}`\n"
    )


def run(args: argparse.Namespace) -> int:
    if getattr(args, "vehicle_pc_replay", False):
        raise ValueError("vehicle_pc_replay forbids live/device collection")
    if args.tcp_connect_host:
        return run_tcp_pull(args)

    c = Collector(args)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # 2026-09-26: a loaded host (full ctest) dropped single heartbeat datagrams
    # in split_play runs (device tel_drop=0, anchor_frame_dropped=0; see
    # tmp/jobs/split-dimensions-plan-0924/out/SPLIT-DROP-DIAG-0926.md). The OS
    # default receive buffer (~64 KB on Windows) overflows while this
    # single-threaded loop parses a burst; 8 MB absorbs several seconds.
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
    except OSError as exc:
        print(f"[listen] SO_RCVBUF raise failed ({exc}); using OS default", flush=True)
    sock.bind((args.bind, args.port))
    sock.settimeout(0.25)
    print(f"[listen] udp://{args.bind}:{args.port} -> {args.out}", flush=True)
    try:
        while not c.done:
            try:
                # Match the enlarged production heartbeat capacity; a UDP
                # read cannot reassemble a datagram truncated by recvfrom.
                # Tracks HW_TELEMETRY_HEARTBEAT_LINE_SIZE -- see
                # HEARTBEAT_LINE_SIZE; raise both together.
                data, _addr = sock.recvfrom(HEARTBEAT_LINE_SIZE)
            except socket.timeout:
                c.tick()
                continue
            try:
                line = data.decode("utf-8", "strict" if args.profile in (VEHICLE_SMOKE_PROFILE, OFFER_PISTON_PROFILE) else "replace")
                if args.profile == VEHICLE_SMOKE_PROFILE:
                    ingest_telemetry_line(c, line)
                    continue
                event = json.loads(line)
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                if c.note_bad_line(exc):
                    break
                continue
            c.process(event)
            if c.shutdown_required and not c.done:
                c.tick()  # Enforce the tail bound even on a busy UDP stream.
    finally:
        sock.close()
    return c.write_result()


def ingest_telemetry_line(c: Collector, line: str) -> bool:
    """Parse one live TCP telemetry line and feed it to the collector.

    Return true only when the malformed-line budget says the caller must stop.
    """
    # Death evidence keeps the exact stress wire row, including duplicate keys
    # or malformed input, alongside the existing normalized event journal.
    # Replay source/historical receipts are never edited.
    if c.args.profile == "perf_stress_5m":
        with (c.out / "death-wire.ndjson").open("a", encoding="utf-8", newline="") as f:
            f.write(line if line.endswith("\n") else line + "\n")
    if c.args.profile == OFFER_PISTON_PROFILE:
        with (c.out / "split-offer-piston-wire.ndjson").open("a", encoding="utf-8", newline="") as f:
            f.write(line if line.endswith("\n") else line+"\n")
        if not line.strip() and c.offer_piston.done:
            return False
        try:
            event = vehicle_json_loads(line)
            if not isinstance(event, dict):
                raise ValueError("object required")
        except (ValueError, TypeError) as exc:
            return c.note_bad_line(exc)
        c.process(event)
        return False
    if c.args.profile == VEHICLE_SMOKE_PROFILE:
        if c.vehicle_pc_replay:
            cursor = c.vehicle_pc_source_cursor
            if (cursor >= len(c.vehicle_pc_source_lines)
                    or line.encode("utf-8") != c.vehicle_pc_source_lines[cursor]):
                return c.note_bad_line(VehicleSmokeError("vehicle_pc_replay_evidence_changed"))
            c.vehicle_pc_source_cursor += 1
            if not line.endswith("\n"):
                return c.note_bad_line(VehicleSmokeError("vehicle_replay_unterminated_line"))
        else:
            with (c.out / "vehicle-wire.ndjson").open("a", encoding="utf-8", newline="") as f:
                f.write(line if line.endswith("\n") else line + "\n")
        if not line.strip():
            # HW7 (2026-10-05): the Wii stream ends with one bare newline
            # AFTER profile_done. Tolerate blank lines only once profile_done
            # has been seen; before that they still fail closed.
            if getattr(c, "_vs_profile_done_seen", False):
                return False
            return c.note_bad_line(VehicleSmokeError("empty line"))
        if '"event":"profile_done"' in line.replace(" ", ""):
            c._vs_profile_done_seen = True
        try:
            event = vehicle_json_loads(line)
            if not isinstance(event, dict):
                raise VehicleSmokeError("top-level object required")
            if str(event.get("event", "")).startswith("vehicle_smoke") and len(line.encode("utf-8")) + (0 if line.endswith("\n") else 1) > 2303:
                raise VehicleSmokeError("vehicle_line_oversize")
        except (ValueError, TypeError) as exc:
            return c.note_bad_line(exc)
        c.vehicle_pc_ingesting = True
        try:
            c.process(event)
        finally:
            c.vehicle_pc_ingesting = False
        return bool(c.fail_reason)
    line = line.strip()
    if not line:
        return False
    try:
        event = json.loads(line)
    except json.JSONDecodeError as exc:
        return c.note_bad_line(exc)
    c.process(event)
    return False


def run_tcp_pull(args: argparse.Namespace) -> int:
    if getattr(args, "vehicle_pc_replay", False):
        raise ValueError("vehicle_pc_replay forbids live/device collection")
    c = Collector(args)
    deadline = time.monotonic() + args.connect_timeout
    sock: socket.socket | None = None
    last_error: OSError | None = None
    print(
        f"[connect] tcp://{args.tcp_connect_host}:{args.tcp_connect_port} -> {args.out}",
        flush=True,
    )
    while sock is None:
        try:
            sock = socket.create_connection(
                (args.tcp_connect_host, args.tcp_connect_port), timeout=1.0
            )
            sock.settimeout(0.25)
            c.started = time.monotonic()
            break
        except OSError as exc:
            last_error = exc
            if time.monotonic() >= deadline:
                c.fail(
                    f"could not connect to {args.tcp_connect_host}:"
                    f"{args.tcp_connect_port}: {last_error}",
                    "telemetry_connect_failed",
                )
                break
            time.sleep(0.5)

    if sock is not None:
        pending = ""
        with sock:
            while not c.done:
                try:
                    data = sock.recv(4096)
                except socket.timeout:
                    c.tick()
                    continue
                except OSError:
                    # Abrupt peer reset: the game aborts its telemetry socket
                    # at profile end (RST -> ConnectionResetError) while the
                    # console keeps running. Without this, the exception
                    # escaped past write_result() and the run ended
                    # verdict-less (observed 2026-08-08:
                    # staged_far_walk_6m_20260808_154025). Treat as close.
                    data = b""
                if not data:
                    if c.args.profile == OFFER_PISTON_PROFILE and c.offer_piston_completion_at is not None and not c.fail_reason:
                        if pending.strip():
                            c.note_bad_line(ValueError("unterminated observer tail"))
                        else:
                            c._finish_normal("profile_done received; offered piston socket quiesced")
                    elif c.args.profile == VEHICLE_SMOKE_PROFILE and c.vehicle_completion_at is not None and not c.fail_reason:
                        if pending:
                            c.note_bad_line(VehicleSmokeError("unterminated trailing line"))
                        else:
                            c._finish_normal("profile_done received; vehicle socket quiesced")
                    c.tick()
                    if not c.done:
                        if c.first_hb is None:
                            cls = "socket_closed_before_heartbeat"
                        else:
                            cls = "socket_closed_after_heartbeat"
                        c.fail("TCP telemetry socket closed", cls)
                    break
                try:
                    pending += data.decode("utf-8", "strict" if args.profile in (VEHICLE_SMOKE_PROFILE, OFFER_PISTON_PROFILE) else "replace")
                except UnicodeDecodeError as exc:
                    c.note_bad_line(exc)
                    break
                if args.profile in (VEHICLE_SMOKE_PROFILE, OFFER_PISTON_PROFILE) and "\n" not in pending and len(pending.encode("utf-8")) > HEARTBEAT_LINE_SIZE:
                    c.note_bad_line(VehicleSmokeError("unbounded unterminated line"))
                    break
                while "\n" in pending:
                    line, pending = pending.split("\n", 1)
                    if ingest_telemetry_line(c, line):
                        break
                if c.shutdown_required and not c.done:
                    c.tick()  # A busy/unterminated stream must not evade timeout.
    return c.write_result()


def replay_run_dir(args: argparse.Namespace) -> int:
    """`--replay <run dir>`: re-derive a verdict from an ALREADY RECORDED run,
    offline -- no console, no server, no socket.

    This is the same entry point the live collector uses (Collector.process ->
    _finish_normal -> the profile's own gates), fed from the run's
    telemetry.ndjson instead of from the wire, so a replay can never grade a
    tape by rules the live run did not apply. It is what the lead runs on the
    first real PC-app run before any console time is spent on the profile, and
    it is how every negative case in tests/test_mp_play_evidence.py is proved.

    The run directory is READ-ONLY here: the collector writes its own
    telemetry.ndjson/latest.json as it ingests, so it is pointed at a temporary
    directory and the fixture receipt log is copied in beside it. Nothing in
    `<run dir>` is created, appended to or overwritten -- replaying a run never
    disturbs the artifact that recorded it."""
    src = Path(args.replay)
    pc_replay = bool(getattr(args, "vehicle_pc_replay", False))
    if pc_replay:
        problem = vehicle_pc_replay_args_error(args, args.profile_override or args.profile)
        if problem:
            print("[replay] " + problem, flush=True)
            return 2
    tape = src / "telemetry.ndjson"
    if not pc_replay and (src / "vehicle-wire.ndjson").is_file():
        tape = src / "vehicle-wire.ndjson"
    if not tape.is_file():
        print(f"[replay] {tape} does not exist", flush=True)
        return 2
    recorded: dict = {}
    for name in (() if pc_replay else ("PASS.json", "FAIL.json")):
        path = src / name
        if path.is_file():
            try:
                recorded = json.loads(path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                recorded = {}
            break
    run_id = args.run_id or recorded.get("run_id") or ""
    lines = []
    rows = []
    decode_error = None
    if pc_replay:
        try:
            lines = [line.decode("utf-8", "strict") for line in vehicle_pc_tape_lines(tape.read_bytes())]
        except UnicodeDecodeError as exc:
            decode_error = exc
    else:
        with tape.open("r", encoding="utf-8", errors="replace") as f:
            lines = list(f)
    for line in lines:
        # Hints select identity only. The live parser consumes EVERY raw line;
        # malformed JSON is never dropped from grading or retained PC evidence.
        try:
            hint = json.loads(line)
            if isinstance(hint, dict):
                rows.append(hint)
        except json.JSONDecodeError:
            pass
    if not run_id:
        run_id = next((str(row.get("run_id")) for row in rows
                       if row.get("run_id")), "")
    # What the tape is graded as, most authoritative first: an explicit
    # override, the recorded artifact, the device's own heartbeats (every
    # heartbeat names its profile), then the CLI default.
    profile = (args.profile_override or recorded.get("profile")
               or next((str(row.get("profile")) for row in rows
                        if row.get("event") == "heartbeat"
                        and row.get("profile")), "")
               or args.profile)
    if profile not in PROFILES:
        print(f"[replay] unknown profile {profile!r}", flush=True)
        return 2
    if pc_replay and profile != VEHICLE_SMOKE_PROFILE:
        print("[replay] --vehicle-pc-replay requires host_vehicle_smoke_8m", flush=True)
        return 2
    replay_out = getattr(args, "vehicle_replay_out", "")
    if replay_out:
        target = Path(replay_out)
        if (profile != VEHICLE_SMOKE_PROFILE or (not pc_replay and not (src / "vehicle-wire.ndjson").is_file())
                or target.exists() or target.resolve().is_relative_to(src.resolve())):
            print("[replay] vehicle output requires a raw vehicle tape and a new directory outside the original run", flush=True)
            return 2
    print(f"[replay] {src} profile={profile} run_id={run_id} "
          f"rows={len(rows)}", flush=True)
    with tempfile.TemporaryDirectory() as td:
        out = Path(replay_out) if replay_out else Path(td) / "replay"
        if not pc_replay:
            out.mkdir(parents=True, exist_ok=True)
        for name in (() if pc_replay else (CHAT_LOGIN_RECEIPT_NAME,)):
            if (src / name).is_file():
                shutil.copyfile(src / name, out / name)
        collector = Collector(_selftest_args(
            out, run_id, profile,
            expected_build=((args.expected_build or (recorded.get("vehicle_smoke") or {}).get("build_id", ""))
                            if profile == VEHICLE_SMOKE_PROFILE else
                            (recorded.get("build_id") or args.expected_build)
                            if getattr(args, "require_tnt_evidence", False) else ""),
            require_tnt_evidence=getattr(args, "require_tnt_evidence", False),
            require_residency=getattr(args, "require_residency", False),
            residency_require_moving=getattr(args, "residency_require_moving", ""),
            residency_allow_induced=getattr(args, "residency_allow_induced", False),
            residency_expect_fault=getattr(args, "residency_expect_fault", ""),
            cw_vehicle_kind=getattr(args, "cw_vehicle_kind", "") or (recorded.get("vehicle_smoke") or {}).get("kind", ""),
            vehicle_transfer_mode=getattr(args, "vehicle_transfer_mode", ""),
            vehicle_pc_replay=pc_replay, replay=str(src),
            vehicle_manifest=getattr(args, "vehicle_manifest", ""),
            duration=0))
        if profile in ANCHOR_PROFILES:
            # colcap-1006 (R4 point 9): a tape recorded before the current
            # pool schema is graded for what it carries and labelled
            # non-certifying; a current-schema tape gets the full rules.
            collector.anchor.pool.historical = not any(
                row.get("event") == "heartbeat"
                and row.get("cpv") == COLUMN_POOL_CPV for row in rows)
        if decode_error:
            collector.note_bad_line(decode_error)
        for line in lines:
            if collector.done and profile != VEHICLE_SMOKE_PROFILE:
                break
            if profile == VEHICLE_SMOKE_PROFILE and not line.endswith("\n"):
                collector.note_bad_line(VehicleSmokeError("vehicle_replay_unterminated_line"))
                break
            if ingest_telemetry_line(collector, line):
                break
        if profile == VEHICLE_SMOKE_PROFILE and not collector.fail_reason:
            collector._finish_normal(f"offline vehicle replay of {src}")
        if not collector.done:
            # A tape whose profile_done never arrived: grade what there is
            # rather than reporting nothing, exactly as a live run that reached
            # its duration would.
            collector._finish_normal(f"offline replay of {src}")
        # Residency finalization is shared with live collection, including
        # required FAIL/UNPROVEN propagation. Output stays in the replay's
        # isolated directory; the original artifacts are never rewritten.
        if not pc_replay and (collector.require_residency or replay_out):
            collector.write_result()
        if collector.require_residency:
            print("[replay] residency: " + json.dumps(collector.residency_verdict(), sort_keys=True), flush=True)
        verdict = "PASS" if (collector.pass_reason
                             and not collector.fail_reason) else "FAIL"
        if (verdict == "PASS" and profile in ANCHOR_PROFILES
                and not collector.anchor.pool.certifying()):
            # R5 (Sol R4 Major 1): a historical/incomplete tape is graded
            # for what it carries, never reported as a certifying PASS.
            verdict = "NONCERTIFYING_PASS"
        reason = collector.fail_reason or collector.pass_reason or (
            "replay ended without a pass condition")
        label = ("PC_REPLAY_" + verdict + " (NONCERTIFYING)" if pc_replay else
                 verdict + " (pool evidence: "
                 + collector.anchor.pool_summary()["evidence"] + ")"
                 if verdict == "NONCERTIFYING_PASS" else verdict)
        print(f"[replay] {label}: {reason}", flush=True)
        for note in collector.not_graded:
            print(f"[replay] {note}", flush=True)
        if collector.failure_class:
            print(f"[replay] failure class: {collector.failure_class}",
                  flush=True)
        if replay_out and pc_replay:
            collector.write_result()
        if profile in ANCHOR_PROFILES:
            # colcap-1006 (ASTRA-CC-R3 item 2): every accumulated violation
            # and the reconstructed physical column pool, one line each.
            for i, err in enumerate(collector.anchor.errors, 1):
                print(f"[replay] anchor violation {i}: {err}", flush=True)
            print("[replay] column_pool: "
                  + json.dumps(collector.anchor.pool_summary(), sort_keys=True),
                  flush=True)
        if profile == MP_PLAY_PROFILE:
            summary = collector.mp_play.summary()
            print("[replay] device_complete=%s matched=%s unmatched=%d "
                  "measured_ms=%s displacement=%s"
                  % (summary["device_complete"], summary["matched"],
                     len(summary["unmatched"]), summary["measured_ms"],
                     summary["displacement"]), flush=True)
            # The narration tallies (push-0921/B12), on their own line because
            # they grade nothing: pickups_missed is the collect path's real hit
            # rate against this run's own digs, and plants_cleared says how much
            # of the place budget went on breaking tall grass first. A reader
            # comparing two runs needs both, and neither belongs in the verdict.
            print("[replay] narration: plants_cleared=%s pickups_missed=%s "
                  "build_skipped=%s reserve_kept=%s"
                  % (summary["plants_cleared"], summary["pickups_missed"],
                     summary["build_skipped"], summary["reserve_kept"]),
                  flush=True)
            # A run that failed at profile_done never reached the mp_play gate,
            # so receipt_error -- and the classification inside it -- did not
            # run. Do it here; it is idempotent.
            collector.mp_play.classify_misses_from_run_dir(collector.out)
            # WHO OWNS EACH MISS (push-0921/B13). One line per pickup_missed,
            # naming the case and the owner, so nobody has to read two logs
            # side by side to find out whether the drop was ours to lose.
            for case in collector.mp_play.pickup_miss_cases:
                cell = case["cell"]
                head = ("[replay] miss n=%s cell=(%s,%s,%s) -> %s [%s]"
                        % (case["miss"].get("n"), cell[0], cell[1], cell[2],
                           case["case"], case["owner"]))
                if case["case"] == "handed_over":
                    head += (" slot=%s item=%s count=%s -- the server HANDED "
                             "IT OVER and the client never showed it"
                             % (case.get("slot"),
                                case.get("collect", {}).get("item"),
                                case.get("collect", {}).get("count")))
                elif case["case"] == "drifted":
                    head += " reason=%s" % case.get("reason")
                elif case["case"] == "still_in_world":
                    spawn = case.get("spawn", {})
                    head += (" spawn=(%s,%s,%s) vel=(%s,%s,%s) player_after="
                             "%s distance=%s"
                             % (spawn.get("x"), spawn.get("y"), spawn.get("z"),
                                spawn.get("vx"), spawn.get("vy"),
                                spawn.get("vz"),
                                case.get("player_after"),
                                case.get("distance")))
                print(head, flush=True)
            for row in summary["unmatched"]:
                print(f"[replay] unmatched: {row}", flush=True)
            if summary["device_error"]:
                print(f"[replay] device: {summary['device_error']}", flush=True)
        return 0 if verdict == "PASS" else 1


def _selftest_args(out: Path, run_id: str, profile: str, **overrides) -> argparse.Namespace:
    base = dict(
        out=out,
        run_id=run_id,
        profile=profile,
        expected_build="abc_06.27",
        first_heartbeat_timeout=1,
        stale_timeout=5,
        dump_wait=30,
        duration=60,
        max_server_us=50000,
    )
    base.update(overrides)
    return argparse.Namespace(**base)


def anchor_selftest():
    # Ingest the actual C formatter's combined worst-case record over UDP.
    exe_name = "test_hw_telemetry.exe" if os.name == "nt" else "test_hw_telemetry"
    exe = Path.cwd() / exe_name
    if not exe.exists():
        exe = Path(__file__).resolve().parents[1] / "build-verify" / exe_name
    formatted = subprocess.run([str(exe), "--worst-heartbeat-only"],
                               capture_output=True, text=True, check=True)
    wire = next(line for line in formatted.stdout.splitlines()
                if line.startswith('{"event":"heartbeat"') and '"cpu_valid"' in line)
    # Upper bound tracks HW_TELEMETRY_HEARTBEAT_LINE_SIZE (HEARTBEAT_LINE_SIZE,
    # 13312 since residency-1005/4); the C side pins the exact worst case.
    assert 4096 < len(wire.encode()) < HEARTBEAT_LINE_SIZE
    event = json.loads(wire)
    assert len(event["anchor_frame_us"]) == 256
    assert event["cpu_packed_us"] == 0xffffffff
    # colcap-1006 (ASTRA-CC-R3 items 2-3): the anchor pool block at its widest
    # round-trips intact (no truncation of the line's tail blocks).
    assert event["cpv"] == COLUMN_POOL_CPV and event["column_pool_used"] == 0xffffffff
    assert event["cpe"] == [0xffffffff] * 4
    assert event["cps"] == [255] * 16
    assert event["dll"] == 0xffffffff and event["dlb"] == 0xffffffff
    assert all(k in event for k in ('oom', 'spill', 'mesh_trunc', 'near_radius',
        'far_view_distance', 'far_storage', 'near_missing', 'near_mesh_pending',
        'far_missing'))
    # 2026-09-08 server-loop wedge instrumentation lane (lane/carve-observer-
    # boot): the six new server-loop fields round-trip through the C
    # formatter (worst-case UINT32_MAX/INT_MIN, matching test_hw_telemetry.c)
    # and are readable here exactly as produced -- proves the collector does
    # not drop or truncate them, distinct from the wd_srv phase-name table
    # extension checked below.
    assert event["srv_loop_it"] == 0xffffffff
    assert event["srv_ticks_pend"] == 0xffffffff
    assert event["srv_elapsed_ms"] == -2147483648
    assert event["srv_partial_m"] == -2147483648
    assert event["srv_sleep_ms"] == -2147483648
    assert event["srv_far_us_max"] == -2147483648
    # The appended WDS_* phase names (watchdog.h order, mirrored in
    # WD_SRV_NAMES above): 12..16 = IDLE_SAMPLE/IDLE_SLEEP/FAR_EXTRACT/
    # FAR_POPULATE/REGION_IO.
    assert phase_name(WD_SRV_NAMES, 12) == "IDLE_SAMPLE"
    assert phase_name(WD_SRV_NAMES, 13) == "IDLE_SLEEP"
    assert phase_name(WD_SRV_NAMES, 14) == "FAR_EXTRACT"
    assert phase_name(WD_SRV_NAMES, 15) == "FAR_POPULATE"
    assert phase_name(WD_SRV_NAMES, 16) == "REGION_IO"
    # lava-aabb-0924/H1: the three 2026-09-11 phases were missing here (a
    # SAVE-window dump decoded as "?"), plus the three honest phases it added.
    assert phase_name(WD_SRV_NAMES, 17) == "SAVE"
    assert phase_name(WD_SRV_NAMES, 18) == "SAVE_SELECT"
    assert phase_name(WD_SRV_NAMES, 19) == "EVICT_SELECT"
    assert phase_name(WD_SRV_NAMES, 20) == "FAR_SELECT"
    assert phase_name(WD_SRV_NAMES, 21) == "TAIL"
    assert phase_name(WD_SRV_NAMES, 22) == "EPILOGUE"
    assert phase_name(WD_SRV_NAMES, 23) == "?"
    with tempfile.TemporaryDirectory() as td:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        proc = subprocess.Popen(
            [sys.executable, __file__, "--bind", "127.0.0.1", "--port", str(port),
             "--out", td, "--run-id", event["run_id"], "--profile", "split_anchor_6m"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            assert "[listen]" in proc.stdout.readline()
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                sender.sendto(wire.encode(), ("127.0.0.1", port))
                done = dict(event="profile_done", run_id=event["run_id"], status="ok")
                sender.sendto(json.dumps(done).encode(), ("127.0.0.1", port))
            output, _ = proc.communicate(timeout=10)
            assert proc.returncode == 1, output  # maximum counters are deliberately invalid evidence
            recorded = Path(td, "telemetry.ndjson").read_text().splitlines()
            assert json.loads(recorded[0]) == event
            assert json.loads(Path(td, "FAIL.json").read_text())["failure_class"] != "invalid_json"
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
    a = AnchorEvidence()
    base = dict(**PC_POOL_EVIDENCE, near_radius=2, far_view_distance=6, far_storage=0,
                anchor_ms=0, anchor_frame_total=0, anchor_frame_dropped=0,
                server_time=0, tick_backlog=0, anchor_load_carve_min=1000,
                anchor_run_carve_min=-1, anchor_near_owed=[1,1],
                anchor_oldest=[0,0], anchor_missing=[0,0], anchor_mesh_pending=[0,0],
                anchor_far_missing=[0,0], anchor_far_max=[0,0], anchor_frame_us=[],
                anchor_ready_streak=3, anchor_far_mesh_pending=[0,0],
                near_missing=0, near_mesh_pending=0, far_missing=0,
                anchor_load_faults=[0,30,0], anchor_run_faults=[0,0,0])
    a.note(base)
    assert a.error is None  # loading minimum is not the measured minimum
    assert a.complete_error()
    for t in range(361):
        a.note({**base, "anchor_measuring": True, "anchor_ready": True,
                "anchor_ms": t*1000, "server_time": t*20,
                "anchor_frame_total": t+1, "anchor_frame_us": [16667],
                "anchor_run_carve_min": 2246, "oom": 0,
                "spill": 30, "mesh_trunc": 0})
    assert a.complete_error() is None
    assert "warning only" in a.summary()["warnings"][0]
    assert "Loading spill=30: warning only" in render_summary(
        {"status": "PASS", "anchor": a.summary()})
    failed = AnchorEvidence()
    for t in range(361):
        failed.note({**base, "anchor_measuring": True, "anchor_ready": True,
                     "anchor_ms": t*1000, "server_time": t*20,
                     "anchor_oldest": [t*20, 0], "anchor_near_owed": [1, 0],
                     "anchor_frame_total": t+1, "anchor_frame_us": [16667],
                     "anchor_run_carve_min": 2246, "oom": 0, "spill": 30,
                     "mesh_trunc": 0})
    assert "completion bound" in failed.complete_error()
    failed.note({**failed.last, "anchor_oldest": [0, 0],
                 "anchor_frame_total": 362, "anchor_frame_us": [16667]})
    assert failed.complete_error() is None  # resolved historical debt is complete
    for slot, key in enumerate(("oom", "spill", "mesh_trunc")):
        bad = AnchorEvidence()
        bad.note({**a.last, "anchor_frame_total": 1, "anchor_frame_us": [16667]})
        assert bad.error is None
        counters = [0, 0, 0]
        counters[slot] = 1
        bad.note({**bad.last, "anchor_ready": False, "anchor_frame_total": 2,
                  "anchor_frame_us": [16667], key: bad.last[key] + 1,
                  "anchor_run_faults": counters})
        assert "measured window" in bad.complete_error()
    for missing_key in ("near_missing", "near_mesh_pending", "far_missing"):
        bad = AnchorEvidence()
        bad.note({**a.last, "anchor_frame_total": 1, "anchor_frame_us": [16667],
                  missing_key: 1})
        assert "before stable" in bad.complete_error()
    for slot in range(2):
        for key in ("anchor_missing", "anchor_mesh_pending", "anchor_far_missing",
                    "anchor_far_mesh_pending"):
            bad = AnchorEvidence()
            pending = [0, 0]
            pending[slot] = 1
            bad.note({**a.last, "anchor_sep": 32, key: pending,
                      "anchor_frame_total": 1, "anchor_frame_us": [16667]})
            assert "before stable" in bad.complete_error()
    for slot, key in ((0, "oom"), (2, "mesh_trunc")):
        loaded = AnchorEvidence()
        faults = [0, 30, 0]
        faults[slot] = 1
        loaded.note({**base, "anchor_load_faults": faults})
        assert "during loading" in loaded.complete_error()
        loaded.note({**a.last, "anchor_frame_total": 1, "anchor_frame_us": [16667],
                     key: 1, "anchor_load_faults": faults})
        assert "during loading" in loaded.complete_error()
        assert loaded.summary()["measured_faults_oom_spill_trunc"] == [0, 0, 0]
    loaded = AnchorEvidence()
    loaded.note({**a.last, "anchor_frame_total": 1, "anchor_frame_us": [16667]})
    loaded.note({**loaded.last, "anchor_frame_total": 2, "anchor_frame_us": [16667],
                 "oom": 3, "anchor_load_faults": [3, 30, 4]})
    assert "boundary changed" in loaded.complete_error()
    regressed = AnchorEvidence()
    regressed.note({**a.last, "anchor_frame_total": 1, "anchor_frame_us": [16667]})
    regressed.note({**regressed.last, "anchor_measuring": False,
                    "anchor_frame_total": 2, "anchor_frame_us": [16667]})
    assert "latch regressed" in regressed.complete_error()
    print("PASS loading OOM/truncation fail whole-run certification; loading spill "
          "is warning only; every in-window fault remains fatal after readiness loss")
    print("PASS persistent post-readiness failure exceeds completion bound")
    a.note_frames(dict(anchor_frame_total=363, anchor_frame_dropped=0, anchor_frame_us=[16667,16667]))
    assert a.error is None and a.last_total == 363
    assert a.summary()["actual_tps"] == 20
    assert 59 < a.summary()["fps_p01"] < 60
    broken = AnchorEvidence()
    broken.note({**base, "anchor_frame_total": 4})
    assert broken.error == "missing per-frame samples"
    a.note({**a.last, "anchor_frame_total": 364, "anchor_frame_us": [50000],
            "anchor_run_carve_min": 2245})
    assert a.complete_error()


def split_action_selftest():
    """S1-d (mp-split-0912/68): split_actions_6m must PASS a run whose guest
    really acted and FAIL every way the per-owner effects can be absent.

    Astra gate-3 is explicit that the profile has to fail when the expected
    per-owner effects are missing, so this drives BOTH directions over the same
    360-beat synthetic run: one where the scripted guest tallies clear their
    floors, and one differing only in the act1 array. Every failing variant
    below is a way a real broken build would present -- the guest silently
    dropped (zeros), the guest's requests routed to the host (act0 nonzero), a
    build that does not emit the evidence at all, a body that left and rejoined
    mid-window (regression), and an owner that was not alive."""
    base = dict(**PC_POOL_EVIDENCE, near_radius=2, far_view_distance=6, far_storage=0,
                anchor_ms=0, anchor_frame_total=0, anchor_frame_dropped=0,
                server_time=0, tick_backlog=0, anchor_load_carve_min=1000,
                anchor_run_carve_min=2246, anchor_near_owed=[0, 0],
                anchor_oldest=[0, 0], anchor_missing=[0, 0],
                anchor_mesh_pending=[0, 0], anchor_far_missing=[0, 0],
                anchor_far_max=[0, 0], anchor_frame_us=[],
                anchor_ready_streak=3, anchor_far_mesh_pending=[0, 0],
                near_missing=0, near_mesh_pending=0, far_missing=0,
                anchor_load_faults=[0, 0, 0], anchor_run_faults=[0, 0, 0],
                oom=0, spill=0, mesh_trunc=0,
                anchor_measuring=True, anchor_ready=True,
                act0=[0, 0, 0, 0, 0, 0], act_health=[160, 160])

    def guest_row(dig=0, place=0, attack=0, use=0, hotbar=0, drop=0):
        return [dig, place, attack, use, hotbar, drop]

    def run(profile="split_actions_6m", guest=None, per_beat=None):
        """360 measured seconds, one heartbeat each. `guest` is the FINAL act1
        row; the tally ramps toward it so the monotonic check sees real
        growth rather than a step. `per_beat` may rewrite any beat."""
        a = AnchorEvidence(profile)
        for t in range(361):
            beat = {**base, "anchor_ms": t * 1000, "server_time": t * 20,
                    "anchor_frame_total": t + 1, "anchor_frame_us": [16667]}
            if guest is not None:
                ramp = min(1.0, t / 300.0)
                beat["act1"] = [int(v * ramp) for v in guest]
            if per_beat:
                beat = per_beat(t, beat)
            a.note(beat)
        return a

    acted = guest_row(dig=36, place=34, attack=2, use=36, hotbar=72)
    good = run(guest=acted)
    assert good.complete_error() is None, good.complete_error()
    assert good.summary()["guest_actions"]["dig"] == 36
    assert good.summary()["host_actions"] == dict(
        zip(SPLIT_ACTION_NAMES, [0] * 6))
    assert good.summary()["action_floors"] == SPLIT_ACTION_FLOORS

    # The FAIL direction Astra asked for: the guest never acted.
    silent = run(guest=guest_row())
    problem = silent.complete_error()
    assert problem and "effects are absent" in problem, problem
    assert "dig" in problem

    # ATTACK is reported, never required: a run with zero attacks still passes.
    no_attack = run(guest=guest_row(dig=3, place=3, use=1, hotbar=3))
    assert no_attack.complete_error() is None
    assert no_attack.summary()["guest_actions"]["attack"] == 0

    # One below each floor fails on exactly that verb.
    for name, floor in SPLIT_ACTION_FLOORS.items():
        row = dict(zip(SPLIT_ACTION_NAMES, [0] * 6))
        row.update({k: v for k, v in SPLIT_ACTION_FLOORS.items()})
        row[name] = floor - 1
        short = run(guest=[row[k] for k in SPLIT_ACTION_NAMES])
        problem = short.complete_error()
        assert problem and problem.startswith("guest %s actions" % name), problem

    # The host acted: either the harness drove the wrong body, or owner
    # routing sent the guest's requests to the host.
    def host_acts(t, beat):
        if t > 100:
            beat["act0"] = [0, 0, 0, 0, 1, 0]
        return beat
    busy = run(guest=acted, per_beat=host_acts)
    assert "host performed actions" in busy.complete_error()

    # A build that cannot answer the question at all.
    for missing in ("act0", "act1", "act_health"):
        def drop_key(t, beat, key=missing):
            beat = dict(beat)
            beat.pop(key, None)
            return beat
        blind = run(guest=acted, per_beat=drop_key)
        problem = blind.complete_error()
        assert problem and "per-owner" in problem, (missing, problem)
    for broken in ([1, 2, 3], "six", [0] * 6 + [1], [0, 0, 0, 0, 0, -1]):
        def bad_row(t, beat, value=broken):
            beat = dict(beat)
            beat["act1"] = value
            return beat
        assert "per-owner action evidence" in run(
            guest=acted, per_beat=bad_row).complete_error()

    # A guest that left and rejoined restarts its (per-body) tally at zero;
    # the end-of-run totals then describe two sessions, not one.
    def rejoin(t, beat):
        if t > 200:
            beat["act1"] = [0, 0, 0, 0, 0, 0]
        return beat
    assert "regressed" in run(guest=acted, per_beat=rejoin).complete_error()

    # Both owners must be ALIVE through the measured window: -1 is "no body",
    # 0 is dead, and the T1 health floor applies to the guest as well as the
    # host (Retro 1 corrective action, generalized per owner).
    for health in ([160, 0], [160, -1], [0, 160], [-1, 160]):
        def sick(t, beat, value=health):
            beat = dict(beat)
            if t > 50:
                beat["act_health"] = value
            return beat
        problem = run(guest=acted, per_beat=sick).complete_error()
        assert problem and "not alive" in problem, (health, problem)

    # None of this touches the sibling profiles: split_play_8m carries the
    # same window and safety checks and still certifies a run with no action
    # evidence at all, exactly as it did before this profile existed.
    def strip_actions(t, beat):
        beat = dict(beat)
        for key in ("act0", "act1", "act_health"):
            beat.pop(key, None)
        return beat
    sibling = run(profile="split_play_8m", per_beat=strip_actions)
    assert sibling.complete_error() is None
    assert sibling.summary()["guest_actions"] is None
    assert sibling.summary()["action_floors"] is None

    # And the gate really is wired into the collector verdict, not just the
    # evidence class: the same two runs through _finish_normal.
    with tempfile.TemporaryDirectory() as td:
        for name, evidence, expect_pass in (("actions_pass", good, True),
                                            ("actions_fail", silent, False)):
            c = Collector(_selftest_args(Path(td) / name, name,
                                         "split_actions_6m"))
            c.anchor = evidence
            c.hb_count = int(c.profile["min_hb"])
            c.world_loaded_seen = True
            c.walk_min_x = c.walk_min_z = 0.0
            c.walk_max_x = c.walk_max_z = 64.0
            c._finish_normal("selftest")
            assert c.done
            if expect_pass:
                assert c.pass_reason and not c.fail_reason, c.fail_reason
            else:
                assert c.failure_class == "anchor_feasibility_failed"
                assert "effects are absent" in c.fail_reason
    print("PASS split_actions_6m certifies a guest that acted and FAILS absent "
          "per-owner effects (silent guest, short of any floor, host not idle, "
          "missing/ malformed evidence, mid-run rejoin, owner not alive); "
          "attack reported only; split_play_8m unchanged")


def split_recovery_selftest():
    """split_recovery_6m must PASS the contract's marker stream and FAIL every
    way that stream can be wrong (docs/split_recovery_profile.md section 7).

    The valid stream is built ONCE below; every rejection case is that same
    stream with exactly one thing broken, so a case can never pass for a reason
    the valid stream does not also have. Each broken stream is a way a real
    driver or a real console would present: a phase that never reported, a
    marker emitted twice, two observations that arrived out of order, a
    counter that went backwards, an assertion the device itself failed, a state
    machine that skipped a step, and evidence left over from an earlier run."""
    def stream(run_id="rec", skip=(), repeat=None):
        """The one valid stream, section 5, in its emission order."""
        rows = []
        seq = 0

        def emit(phase, marker, corr=0, **fields):
            nonlocal seq
            if (phase, marker) in skip:
                return 0
            last = 0
            for _ in range(2 if (phase, marker) == repeat else 1):
                seq += 1
                row = dict(event="split_recovery", seq=seq, phase=phase,
                           marker=marker, ms=seq * 100,
                           corr=seq if corr == "self" else corr, ok=True,
                           run_id=run_id)
                row.update(fields)
                rows.append(row)
                last = seq
            return last

        emit("p0", "guest_admitted", gen=7, entity=11, x=8.5, y=65.6, z=8.5)
        for i in range(3):
            emit("p0", "block_placed", i=i, x=10 + i, y=65, z=10, id=3, meta=0)
        for actor, inv in ((0, 0xaa), (1, 0xab)):
            emit("p0", "baseline", actor=actor, inv_hash=inv, inv_used=3,
                 x=8.5, y=65.6, z=8.5)
        emit("p1", "loss_injected", reason=2, act=1)
        emit("p1", "no_leave", local_phase=4, gen=7, ready=True)
        emit("p1", "ticks_advanced", ticks=40)
        emit("p1", "collapse_begin", grace_ms=5040, frames=101, boundary=0.5)
        emit("p1", "collapse_done", transition_ms=400, frames=8, boundary=1.0)
        emit("p1", "no_auto_resume", frames=10, rec_phase=2)
        emit("p1", "resumed", act=3)
        emit("p1", "view_restored", boundary=0.5, restore_ms=360)
        emit("p1", "inventory_match", actor=1, inv_hash=0xab, want=0xab)
        leave = emit("p2", "leave_requested", corr="self", gen=7, page=0,
                     selected=1)
        emit("p2", "leave_acked", corr=leave, gen=7)
        emit("p2", "guest_retired", ready=False)
        emit("p2", "host_only_layout", views=1, boundary=1.0)
        emit("p2", "rejoined", gen=8, entity=12)
        emit("p2", "inventory_match", actor=1, inv_hash=0xab, want=0xab)
        for actor, inv in ((0, 0xac), (1, 0xab)):
            emit("p3", "pre_save_baseline", actor=actor, inv_hash=inv,
                 inv_used=4, x=8.5, y=65.6, z=8.5)
        exit_seq = emit("p3", "exit_requested", corr="self", epoch=3, page=1)
        emit("p3", "world_reset", corr=exit_seq, epoch=3)
        emit("p3", "title_entered", after_reset=True)
        emit("p4", "autoload_armed", world="cw_recovery_rig")
        emit("p4", "world_loaded", epoch=4, x=8.5, y=65.6, z=8.5)
        emit("p4", "host_verified", inv_hash=0xac, want=0xac, dx=0.0, dy=0.12,
             dz=0.0, tol_xz=0.05, tol_y=0.5)
        for i in range(3):
            emit("p4", "blocks_verified", i=i, x=10 + i, y=65, z=10, id=3,
                 meta=0)
        emit("p4", "guest_readmitted", gen=9, entity=13)
        emit("p4", "guest_verified", inv_hash=0xab, want=0xab)
        emit("run", "ok")
        return rows

    def index_of(rows, key):
        return next(i for i, r in enumerate(rows)
                    if (r["phase"], r["marker"]) == key)

    def patched(key, **fields):
        """The valid stream with one marker rewritten."""
        rows = stream()
        rows[index_of(rows, key)].update(fields)
        return rows

    def reordered(first, second):
        """The two markers delivered the other way round. Only their CONTENT
        swaps: seq stays gapless and each corr stays with its slot, so the
        stream is wrong in exactly one way -- the order."""
        rows = stream()
        i, j = index_of(rows, first), index_of(rows, second)
        fixed = ("event", "seq", "ms", "corr", "run_id")
        content = [{k: v for k, v in rows[n].items() if k not in fixed}
                   for n in (i, j)]
        rows[i] = {**{k: rows[i][k] for k in fixed}, **content[1]}
        rows[j] = {**{k: rows[j][k] for k in fixed}, **content[0]}
        return rows

    def verdict(rows, run_id="rec", done="ok"):
        evidence = RecoveryEvidence(run_id)
        for row in rows:
            evidence.note(row)
        if done is not None:
            evidence.note_profile_done(done)
        return evidence.complete_error()

    assert verdict(stream()) is None, verdict(stream())
    # The heartbeat floor is a CADENCE over the run's own length, never a fixed
    # count: a 34.2 s run needs 27 beats, a marker-less run keeps the default.
    cadence = RecoveryEvidence("rec")
    assert cadence.heartbeat_floor(60) == 60
    cadence.note(dict(event="split_recovery", seq=1, phase="p0",
                      marker="guest_admitted", ms=34224, corr=0, ok=True,
                      gen=1, entity=1, x=0.5, y=6.62, z=0.5, run_id="rec"))
    assert cadence.heartbeat_floor(60) == 27, cadence.heartbeat_floor(60)
    # Nothing in the verdict comes from the clock: an empty run is not a
    # "short" one, it is a driver that never ran.
    assert "no split_recovery markers" in verdict([])

    # Every rejection of section 7, each with its own reason string.
    cases = [
        ("missing marker p1.ticks_advanced",
         verdict(stream(skip={("p1", "ticks_advanced")}))),
        ("duplicate marker p1.resumed",
         verdict(stream(repeat=("p1", "resumed")))),
        ("duplicate marker p0.block_placed i=0",
         verdict(stream(repeat=("p0", "block_placed")))),
        ("reordered markers: p1.no_leave arrived after p1.ticks_advanced",
         verdict(reordered(("p1", "no_leave"), ("p1", "ticks_advanced")))),
        ("seq not strictly increasing",
         verdict(patched(("p1", "resumed"), seq=2))),
        ("p0.baseline actor 0 and actor 1 share inv_hash=171",
         verdict(patched(("p0", "baseline"), inv_hash=0xab))),
        ("p0.baseline actor 0 inv_used=0 -- an empty baseline",
         verdict(patched(("p0", "baseline"), inv_used=0))),
        ("p3.pre_save_baseline actor 0 inv_used=None -- an empty baseline",
         verdict(patched(("p3", "pre_save_baseline"), inv_used=None))),
        ("seq gap", verdict(patched(("p1", "resumed"), seq=99))),
        ("phase failure p1.collapse_done: collapse took 9000 ms",
         verdict(patched(("p1", "collapse_done"), ok=False,
                         diag="collapse took 9000 ms"))),
        ("phase failure p3.fail: title before WORLD_RESET(0)",
         verdict(patched(("p3", "world_reset"), marker="fail", ok=False,
                         diag="title before WORLD_RESET(0)"))),
        ("contradictory evidence: p3.title_entered before p3.world_reset",
         verdict(reordered(("p3", "world_reset"), ("p3", "title_entered")))),
        ("contradictory evidence: p1.resumed before p1.collapse_done",
         verdict(stream(skip={("p1", "collapse_done")}))),
        ("contradictory evidence: p4.autoload_armed before p3.title_entered",
         verdict(stream(skip={("p3", "title_entered")}))),
        ("contradictory evidence: p1.collapse_begin grace_ms=6000 outside",
         verdict(patched(("p1", "collapse_begin"), grace_ms=6000))),
        ("contradictory evidence: p1.collapse_done transition_ms=900 outside",
         verdict(patched(("p1", "collapse_done"), transition_ms=900))),
        ("contradictory evidence: p2.inventory_match inv_hash=153 does not "
         "equal want=171",
         verdict(patched(("p2", "inventory_match"), inv_hash=0x99))),
        ("is not the recorded baseline for actor 1",
         verdict(patched(("p4", "guest_verified"), inv_hash=0x55, want=0x55))),
        ("does not answer p3.exit_requested",
         verdict(patched(("p3", "world_reset"), corr=0))),
        ("must be its own seq",
         verdict(patched(("p2", "leave_requested"), corr=1))),
        ("stale-run evidence: marker run_id='another_run'",
         verdict(patched(("p2", "rejoined"), run_id="another_run"))),
        ("stale-run evidence: p1.no_leave carries gen=99",
         verdict(patched(("p1", "no_leave"), gen=99))),
        ("stale-run evidence: p2.rejoined must report a NEW guest generation",
         verdict(patched(("p2", "rejoined"), gen=7))),
        ("stale-run evidence: p3.world_reset carries epoch=9",
         verdict(patched(("p3", "world_reset"), epoch=9))),
        ("stale-run evidence: p4.world_loaded must report the NEW input epoch",
         verdict(patched(("p4", "world_loaded"), epoch=3))),
        ("run.timeout: total timeout",
         verdict(patched(("run", "ok"), marker="timeout", ok=False,
                         diag="total timeout"))),
        ("unexpected marker p1.resumed_soon",
         verdict(patched(("p1", "resumed"), marker="resumed_soon"))),
        ("markers continued after run.ok",
         verdict(stream() + [dict(event="split_recovery", seq=99, phase="p4",
                                  marker="guest_verified", ms=1, corr=0,
                                  ok=True, run_id="rec")])),
        # Section 7's "run.ok absent": the run stopped without declaring
        # itself passed, which is a MISSING marker, not a late one (a late
        # run.ok is impossible -- nothing may follow it, see the case above).
        ("missing marker run.ok", verdict(stream(skip={("run", "ok")}))),
        ("profile_done status='p3:title before WORLD_RESET(0)'",
         verdict(stream(), done="p3:title before WORLD_RESET(0)")),
        ("profile_done was never received", verdict(stream(), done=None)),
    ]
    for expected, actual in cases:
        assert actual and expected in actual, (expected, actual)
    assert len({actual for _expected, actual in cases}) == len(cases)

    # The profile's shape: a guest-body split profile that is deliberately
    # NEITHER duration-certifying nor performance-certifying (contract
    # section 0: it proves state machines, not throughput).
    assert PROFILES[SPLIT_RECOVERY_PROFILE]["tps_settle_ms"] is None
    assert SPLIT_RECOVERY_PROFILE in SPLIT_PROFILES
    assert SPLIT_RECOVERY_PROFILE in SPLIT_GUEST
    assert SPLIT_READY_BUDGET[SPLIT_RECOVERY_PROFILE] == 120
    assert SPLIT_RECOVERY_PROFILE not in SPLIT_CERTIFYING
    assert SPLIT_RECOVERY_PROFILE not in RELEASE_FRAME_PROFILES

    def anchor_evidence(profile=SPLIT_RECOVERY_PROFILE, beats=5):
        """Installed readiness plus a few measured beats -- all this profile's
        350 s run can ever produce."""
        base = dict(**PC_POOL_EVIDENCE, near_radius=2, far_view_distance=6, far_storage=0,
                    anchor_ms=0, anchor_frame_total=0, anchor_frame_dropped=0,
                    server_time=0, tick_backlog=0, anchor_load_carve_min=1000,
                    anchor_run_carve_min=2246, anchor_near_owed=[0, 0],
                    anchor_oldest=[0, 0], anchor_missing=[0, 0],
                    anchor_mesh_pending=[0, 0], anchor_far_missing=[0, 0],
                    anchor_far_max=[0, 0], anchor_frame_us=[],
                    anchor_ready_streak=3, anchor_far_mesh_pending=[0, 0],
                    near_missing=0, near_mesh_pending=0, far_missing=0,
                    anchor_load_faults=[0, 0, 0], anchor_run_faults=[0, 0, 0],
                    oom=0, spill=0, mesh_trunc=0,
                    anchor_measuring=True, anchor_ready=True)
        a = AnchorEvidence(profile)
        for t in range(beats):
            a.note({**base, "anchor_ms": t * 1000, "server_time": t * 20,
                    "anchor_frame_total": t + 1, "anchor_frame_us": [16667]})
        return a

    # The measured-window rule moved for THIS profile only: five measured
    # seconds are complete here (the marker contract certifies), and still a
    # failure for the six-minute siblings.
    assert anchor_evidence().complete_error() is None
    assert "six measured minutes" in anchor_evidence("split_play_8m").complete_error()

    # And the gate is wired into the collector verdict, not just the evidence
    # class: the same streams through Collector.process/_finish_normal.
    with tempfile.TemporaryDirectory() as td:
        def collector(name):
            c = Collector(_selftest_args(Path(td) / name, name,
                                         SPLIT_RECOVERY_PROFILE))
            c.anchor = anchor_evidence()
            c.hb_count = int(c.profile["min_hb"])
            c.world_loaded_seen = True
            return c

        broken = patched(("p1", "collapse_done"), ok=False,
                         diag="collapse took 9000 ms")
        for name, rows, expect_pass in (("recovery_pass", stream(), True),
                                        ("recovery_fail", broken, False)):
            c = collector(name)
            for row in rows:
                c.process({**row, "run_id": name})
            c.process(dict(event="profile_done", run_id=name, status="ok"))
            assert c.done
            if expect_pass:
                assert c.pass_reason and not c.fail_reason, c.fail_reason
                assert c.write_result() == 0
                proof = json.loads((c.out / "PASS.json").read_text())
                # Astra ruling item 8: the PASS says what it is worth.
                assert SPLIT_RECOVERY_PASS_LABEL in proof["reason"], proof["reason"]
                assert proof["certification_scope"] == "split_recovery_scripted_proxy"
                assert proof["split_recovery"]["run_ok"] is True
                assert proof["split_recovery"]["markers"] == len(rows)
            else:
                assert c.failure_class == "split_recovery_evidence_failed"
                assert "collapse took 9000 ms" in c.fail_reason
                assert c.write_result() == 1
                assert SPLIT_RECOVERY_PASS_LABEL not in c.fail_reason

        # A clean console that simply never ran the driver: heartbeat evidence
        # alone must NEVER certify this profile.
        silent = collector("recovery_silent")
        silent.process(dict(event="profile_done", run_id="recovery_silent",
                            status="ok"))
        assert silent.failure_class == "split_recovery_evidence_failed"
        assert "no split_recovery markers" in silent.fail_reason

        # Nor may the clock: the generic duration completion is off for every
        # split profile, and this one has no other clock-driven pass.
        timed = collector("recovery_duration")
        timed.process(dict(event="heartbeat", run_id="recovery_duration",
                           tick=1))
        timed.started = time.monotonic() - 361
        timed.tick()
        assert not timed.done, "duration alone must never certify a recovery run"
    print("PASS split_recovery_6m certifies the contract's full marker "
          "sequence and FAILS missing/duplicate/reordered markers, a broken "
          "seq, any ok:false (phase + diag verbatim), contradictory evidence, "
          "stale run_id/gen/epoch, run.timeout and a non-ok profile_done; "
          "duration alone never passes it and the PASS states the proxy scope")


def mp_play_selftest():
    """mp_play_10m's own rules, the ones that need no tape.

    The FULL contract -- a valid two-generation run and one negative per rule --
    is tests/test_mp_play_evidence.py, which replays synthesized tapes through
    the real Collector. This selftest covers what that suite would otherwise
    have to assume: the profile's table shape, the canonical inventory form the
    two sides of every snapshot comparison are built into, the receipt syntax
    the fixture actually emits, and the heartbeat gate, which lives in the
    Collector rather than in the evidence class."""
    profile = PROFILES[MP_PLAY_PROFILE]
    # 1260 = hw_run.c's cfg->duration_seconds for this profile, 60 s past
    # mp_play_core.h's MPP_TOTAL_CAP_MS (reconciled 2026-09-21, push-0921/B6).
    assert profile["duration"] == 1260 and profile["min_hb"] == 480
    assert profile["scenario"] == "mp_connect" and not profile["expect_dump"]
    assert profile["tps_settle_ms"] == 5000
    for table in (SPLIT_PROFILES, ANCHOR_PROFILES, SPLIT_CERTIFYING,
                  SPLIT_GUEST, RELEASE_FRAME_PROFILES):
        assert MP_PLAY_PROFILE not in table
    # The canonical form: 40 protocol slots, armor and empties as item 0, in
    # the fixture's own order -- so the device's sparse list and the fixture's
    # dense one become the same string.
    canonical = mp_play_canonical_inventory({36: (3, 12, 0), 9: (4, 1, 2)})
    entries = canonical.split(",")
    assert len(entries) == 40
    assert entries[0] == "5:0:0:0" and entries[3] == "8:0:0:0"
    assert entries[4] == "9:4:1:2" and entries[31] == "36:3:12:0"
    assert entries[39] == "44:0:0:0"
    # An item <= 0 is empty whatever count/damage ride with it, so a device
    # that reported "slot 20 holds -1 x7" cannot differ from an empty slot.
    assert (mp_play_canonical_inventory({20: (-1, 7, 3)})
            == mp_play_canonical_inventory({}))
    # receipt.rs's own line_frame() unit test, verbatim, plus a fieldless event.
    match = MP_PLAY_RECEIPT_RE.search("[mp-play] n=cw1a2b3c g=2 ev=health h=19")
    assert match and match.group(1) == "cw1a2b3c" and match.group(2) == "2"
    assert match.group(3) == "health" and match.group(4) == "h=19"
    match = MP_PLAY_RECEIPT_RE.search("[mp-play] n=cw1a2b3c g=2 ev=death")
    assert match and match.group(3) == "death" and match.group(4) is None
    # A marker-less run says so, rather than passing on heartbeat evidence.
    empty = MpPlayEvidence("0921_000001")
    assert "no mp_play markers" in empty.complete_error()
    assert MpPlayEvidence("").complete_error().startswith("run id missing")
    # seq is gapless 1..N, and a `fail` marker carries the only device-side
    # diagnosis there is.
    gap = MpPlayEvidence("r")
    gap.note(dict(event=MP_PLAY_EVENT, seq=1, marker="teleport", run_id="r"))
    gap.note(dict(event=MP_PLAY_EVENT, seq=3, marker="teleport", run_id="r"))
    assert "seq gap before teleport" in gap.error
    failed = MpPlayEvidence("r")
    failed.note(dict(event=MP_PLAY_EVENT, seq=1, marker="fail", run_id="r",
                     phase="walk", gen=1, diag="total timeout",
                     measured_ms=412000))
    assert "total timeout" in failed.error and "walk" in failed.error
    # The B-a heartbeat gate, on the two profiles that carry it and on no
    # other. A missing field fails; a non-zero counter fails; and an old
    # far_walk_6m tape (which carries neither) is graded exactly as before.
    with tempfile.TemporaryDirectory() as td:
        for name, field, expect in (
                ("mp_play_10m", None, "lw_timeout_admits_missing"),
                ("mp_play_10m", 1, "lw_timeout_admits_nonzero"),
                ("mp_play_10m", 0, None),
                ("mp_lan_walk_10m", None, "lw_timeout_admits_missing"),
                ("mp_lan_walk_10m", 0, None),
                ("far_walk_6m", None, None)):
            out = Path(td) / ("%s_%s" % (name, field))
            collector = Collector(_selftest_args(out, "hb", name,
                                                 expected_build=""))
            beat = dict(event="heartbeat", run_id="hb", tick=120,
                        world_loaded=True)
            if field is not None:
                beat["lw_to_admits"] = field
            collector.process(beat)
            assert collector.failure_class == expect, (name, field,
                                                       collector.failure_class)
    # The PC branch of the heartbeat-derived gates (push-0921/B6). A real PC run
    # of this profile emits NO heartbeat at all -- hw_telemetry_emit_heartbeat
    # writes stdout only for an ARMED ANCHOR profile -- so the heartbeat floor,
    # the lw_to_admits gate and the world_loaded scenario gate have nothing to
    # grade. They are reported as NOT GRADED on a device-stamped "pc" run and
    # stay MANDATORY everywhere else. Both branches, from the same zero-beat
    # tape, so only the platform differs:
    with tempfile.TemporaryDirectory() as td:
        for platform, want_class, want_notes in (("pc", "mp_play_evidence_failed", 3),
                                                 ("wii", "too_few_heartbeats", 0),
                                                 (None, "too_few_heartbeats", 0)):
            collector = Collector(_selftest_args(
                Path(td) / ("plat_%s" % platform), "r", MP_PLAY_PROFILE,
                expected_build=""))
            if platform is not None:
                collector.process(dict(
                    event=MP_PLAY_EVENT, seq=1, marker="session", run_id="r",
                    contract=1, measured_floor_ms=MP_PLAY_MEASURED_FLOOR_MS,
                    total_cap_ms=MP_PLAY_TOTAL_CAP_MS, platform=platform,
                    nonce=chat_login_username("r")))
            assert collector.hb_count == 0
            collector._finish_normal("selftest")
            assert collector.failure_class == want_class, (
                platform, collector.failure_class, collector.fail_reason)
            assert len(collector.not_graded) == want_notes, (
                platform, collector.not_graded)
            for note in collector.not_graded:
                assert "NOT GRADED ON PC (no heartbeat transport)" in note, note
            # The PC branch skips the two heartbeat-derived gates and NOTHING
            # else: the marker contract still refuses this one-marker tape.
            if platform == "pc":
                assert "measured play" in collector.fail_reason \
                    or "marker" in collector.fail_reason, collector.fail_reason
    # The snapshot comparison: the declared tolerance, and an exact inventory.
    before = {"x": 10.0, "y": 64.0, "z": -3.0, "dim": 0,
              "canonical": mp_play_canonical_inventory({36: (3, 2, 0)})}
    after = dict(before, x=10.9)
    assert MpPlayEvidence._compare_snapshots("t", before, after) is None
    assert "position" in MpPlayEvidence._compare_snapshots(
        "t", before, dict(before, x=11.2))
    assert "dimension" in MpPlayEvidence._compare_snapshots(
        "t", before, dict(before, dim=-1))
    assert "inventory" in MpPlayEvidence._compare_snapshots(
        "t", before, dict(before,
                          canonical=mp_play_canonical_inventory({36: (3, 1, 0)})))


def tps_seal_selftest():
    """The host duration expires before the device duration; await its seal."""
    with tempfile.TemporaryDirectory() as td:
        def collector(name, profile="heartbeat_selftest"):
            c = Collector(_selftest_args(Path(td) / name, name, profile))
            c.started = time.monotonic() - 61
            c.process(dict(event="heartbeat", run_id=name, tick=50))
            c.hb_count = 50  # Isolate completion gating from the minimum-HB gate.
            return c

        def start(c):
            c.process(dict(event="tps_window", run_id=c.args.run_id,
                           kind="single_anchor", settle_ms=5000))
            c.process(dict(event="tps_summary", run_id=c.args.run_id,
                           seq=1, count=7))

        def seal(c, **changes):
            event = dict(event="tps_end", run_id=c.args.run_id, records=7,
                         measured_ns=1_000_000_000, stream_valid=True)
            c.process(event | changes)

        c = collector("delayed")
        start(c)
        c.tick()
        assert not c.done, "host duration must not close a pending TPS stream"
        seal(c)
        c.tick()
        assert c.done and c.pass_reason
        assert json.loads(c.telemetry_path.read_text().splitlines()[-1])["event"] == "tps_end"

        c = collector("done_first")
        start(c)
        c.process(dict(event="profile_done", run_id=c.args.run_id, status="ok"))
        assert not c.done
        seal(c)
        assert c.done and c.pass_reason

        c = collector("partial")
        start(c)
        seal(c, stream_valid=False, reason="early_abort")
        assert c.done and c.failure_class == "tps_partial"
        assert "early_abort" in c.fail_reason

        c = collector("watchdog_partial", "watchdog_inject")
        start(c)
        # An out-of-order completion packet cannot replace the expected dump.
        c.process(dict(event="profile_done", run_id=c.args.run_id, status="ok"))
        seal(c, stream_valid=False, reason="watchdog_inject")
        c.tick()
        assert not c.done and c.tps_end["stream_valid"] is False
        c.process(dict(event="watchdog_dump", run_id=c.args.run_id, cause="HANG",
                       dump_id=1, seq=0, last=False, text="HANG evidence\n"))
        assert not c.done
        c.process(dict(event="watchdog_dump", run_id=c.args.run_id, cause="HANG",
                       dump_id=1, seq=1, last=True, text="complete\n"))
        assert c.done and c.pass_reason.startswith("expected watchdog_dump")
        assert c.write_result() == 0
        passed = json.loads((c.out / "PASS.json").read_text())
        assert passed["tps_end"]["stream_valid"] is False
        assert passed["tps_end"]["reason"] == "watchdog_inject"
        tape = [json.loads(line) for line in c.telemetry_path.read_text().splitlines()]
        assert [e["event"] for e in tape][-3:] == ["tps_end", "watchdog_dump", "watchdog_dump"]

        c = collector("lost_summary")
        start(c)
        seal(c, records=8)
        assert c.done and c.failure_class == "tps_stream_invalid"

        c = collector("no_seal")
        start(c)
        c.started = time.monotonic() - 111
        c.tick()
        assert c.done and c.failure_class == "tps_incomplete"

        c = collector("no_observer")
        c.tick()
        assert c.done and c.pass_reason

        # push-0921/C1: split_play_8m, the certifying guest-body profile. Its
        # window runs on the READINESS epoch device-side (hw_run_tps_measuring
        # -- split_play_8m and split_actions_6m were missing from that list,
        # so they measured from five seconds after world load instead), and
        # this profile completes on profile_done, never on the host duration
        # clock. Both halves have to survive that: a sealed stream must reach
        # PASS.json as a real tps_end, and a run that carries NO TPS stream at
        # all must still certify -- a missing seal has never been a failure
        # here and this is not the card that changes that contract.
        def split_anchor_evidence(profile="split_play_8m", beats=361):
            base = dict(**PC_POOL_EVIDENCE, near_radius=2, far_view_distance=6, far_storage=0,
                        anchor_ms=0, anchor_frame_total=0,
                        anchor_frame_dropped=0, server_time=0, tick_backlog=0,
                        anchor_load_carve_min=1000, anchor_run_carve_min=2246,
                        anchor_near_owed=[0, 0], anchor_oldest=[0, 0],
                        anchor_missing=[0, 0], anchor_mesh_pending=[0, 0],
                        anchor_far_missing=[0, 0], anchor_far_max=[0, 0],
                        anchor_frame_us=[], anchor_ready_streak=3,
                        anchor_far_mesh_pending=[0, 0], near_missing=0,
                        near_mesh_pending=0, far_missing=0,
                        anchor_load_faults=[0, 0, 0],
                        anchor_run_faults=[0, 0, 0], oom=0, spill=0,
                        mesh_trunc=0, anchor_measuring=True, anchor_ready=True)
            a = AnchorEvidence(profile)
            for t in range(beats):
                a.note({**base, "anchor_ms": t * 1000, "server_time": t * 20,
                        "anchor_frame_total": t + 1, "anchor_frame_us": [16667]})
            return a

        def split_collector(name):
            c = Collector(_selftest_args(Path(td) / name, name,
                                         "split_play_8m"))
            c.anchor = split_anchor_evidence()
            assert c.anchor.complete_error() is None
            c.hb_count = int(c.profile["min_hb"])
            c.world_loaded_seen = True
            c.walk_min_x = c.walk_min_z = 0.0
            c.walk_max_x = c.walk_max_z = 64.0
            return c

        c = split_collector("split8_sealed")
        c.process(dict(event="tps_window", run_id=c.args.run_id,
                       kind="single_anchor", settle_ms=5000))
        c.process(dict(event="tps_summary", run_id=c.args.run_id, seq=1,
                       count=7))
        c.process(dict(event="profile_done", run_id=c.args.run_id, status="ok"))
        assert not c.done, "completion must wait for the readiness window's seal"
        c.process(dict(event="tps_end", run_id=c.args.run_id, records=7,
                       measured_ns=360_000_000_000, stream_valid=True))
        assert c.done and c.pass_reason and not c.fail_reason, c.fail_reason
        assert c.write_result() == 0
        passed = json.loads((c.out / "PASS.json").read_text())
        assert passed["tps_end"]["stream_valid"] is True
        assert passed["tps_end"]["measured_ns"] == 360_000_000_000

        reversed_route = split_collector("split8_reversed")
        reversed_route.process(dict(event="guest_route_reversed",
                                    retrace_len=16,
                                    run_id=reversed_route.args.run_id,
                                    reason="shore_beyond_budget", steps=8,
                                    ticks=69, x=0.5, y=64.03, z=-132.0))
        reversed_route.process(dict(event="tps_window", run_id=reversed_route.args.run_id,
                                    kind="single_anchor", settle_ms=5000))
        reversed_route.process(dict(event="tps_summary", run_id=reversed_route.args.run_id,
                                    seq=1, count=7))
        reversed_route.process(dict(event="profile_done",
                                    run_id=reversed_route.args.run_id, status="ok"))
        assert not reversed_route.done
        reversed_route.process(dict(event="tps_end", run_id=reversed_route.args.run_id,
                                    records=7, measured_ns=360_000_000_000,
                                    stream_valid=True))
        assert reversed_route.done and reversed_route.write_result() == 0
        assert json.loads((reversed_route.out / "PASS.json").read_text())["route_reversals"] == 1
        assert json.loads((reversed_route.out / "PASS.json").read_text())["route_retrace_len"] == 16
        blocked_route = split_collector("split8_blocked")
        blocked_route.process(dict(event="guest_route_blocked",
                                   run_id=blocked_route.args.run_id,
                                   reason="no_heading", steps=8, ticks=69,
                                   x=0.5, y=64.03, z=-132.0))
        assert blocked_route.done and blocked_route.failure_class == "guest_route_blocked"
        assert blocked_route.write_result() == 1

        oscillating = split_collector("split8_oscillating")
        oscillating.process(dict(event="guest_route_oscillating",
                                 run_id=oscillating.args.run_id,
                                 reason="insufficient_progress", displacement=3.0,
                                 ticks=400, x=0.5, y=64.03, z=-5.0, up_ms=1234))
        assert oscillating.done and oscillating.failure_class == "guest_route_oscillating"
        assert oscillating.write_result() == 1
        failed = json.loads((oscillating.out / "FAIL.json").read_text())
        assert failed["reason"] == "scripted guest route oscillating: insufficient_progress"
        assert failed["route_oscillation_reason"] == "insufficient_progress"

        # The same profile with no TPS observer armed at all: unchanged, and
        # the artifact says so with a null rather than a fabricated seal.
        c = split_collector("split8_no_tps")
        c.process(dict(event="profile_done", run_id=c.args.run_id, status="ok"))
        assert c.done and c.pass_reason and not c.fail_reason, c.fail_reason
        assert c.tps_end is None
        assert c.write_result() == 0
        assert json.loads((c.out / "PASS.json").read_text())["tps_end"] is None
    print("PASS duration collection waits for TPS seal; reordered profile_done waits; "
          "normal partial/lost/missing seal fail explicitly; watchdog partial retains "
          "complete HANG dump; non-TPS duration unchanged; split_play_8m carries a "
          "sealed readiness-window tps_end and still certifies without one")


def split_guest_health_selftest():
    with tempfile.TemporaryDirectory() as td:
        c = Collector(_selftest_args(Path(td), "dead_guest", "split_play_8m"))
        for tick, health in ((89, 112), (94, 0), (95, 160)):
            c.process(dict(event="heartbeat", run_id="dead_guest",
                           profile="split_play_8m", tick=tick,
                           anchor_ms=(tick - 89) * 1000,
                           gx=(tick - 89) * 0.3, gz=0.0,
                           anchor_measuring=True, act_health=[160, health]))
            if tick == 94:
                assert c.done and c.failure_class == "guest_died_measured"
        c.process(dict(event="guest_death", run_id="dead_guest"))
        c.process(dict(event="guest_respawn", run_id="dead_guest"))
        c.process(dict(event="profile_done", run_id="dead_guest", status="ok"))
        assert c.failure_class == "guest_died_measured"
        assert c.write_result() == 1
        verdict = json.loads((Path(td) / "FAIL.json").read_text())
        assert verdict["first_guest_zero_heartbeat"]["tick"] == 94
        assert verdict["guest_deaths"] == 1
    print("PASS split_play_8m first measured P2 zero remains FAIL after respawn")


def split_dims_selftest():
    profile = PROFILES["split_dims_6m"]
    assert profile["duration"] == 360 and profile["min_hb"] == 300
    assert "split_dims_6m" in ANCHOR_PROFILES
    assert "split_dims_6m" in SPLIT_CERTIFYING
    with tempfile.TemporaryDirectory() as td:
        c = Collector(_selftest_args(Path(td), "dims", "split_dims_6m"))
        c.process(dict(event="heartbeat", run_id="stale", guest=True, tick=1, gy=9.0))
        assert c.done and c.failure_class == "split_dims_stale_run"
        assert not c.split_dims_rows
        missing = Collector(_selftest_args(Path(td) / "missing", "dims", "split_dims_6m"))
        missing.process(dict(event="heartbeat", run_id="dims", guest=True, tick=2))
        assert missing.done and missing.failure_class == "split_dims_guest_y_missing"
        host = dict(host_phase="wait", host_commits=0, slot1_cols=-1,
                    rel_sent=0, rel_rx=0, rel_slot=-1, wslot_v0=0, wslot_v1=0,
                    **{key: 0 for key in _DIMS_BUDGET_INTS},
                    budget_valid=True, budget_wii=False, transfer_held=False,
                    transfer_source_draining=False, transfer_restoring=False)
        good = Collector(_selftest_args(Path(td) / "good", "dims", "split_dims_6m"))
        good.process(dict(event="heartbeat", run_id="dims", guest=True,
                          tick=1, gy=64.25, dim_v0=0, dim_v1=0, views_live=2,
                          dims_active=False, dims_phase="clear",
                          srv_dim_host=0, srv_dim_guest=0, **host))
        assert not good.done, good.fail_reason
        # A device without the host-switch fields is rejected at once.
        nohost = Collector(_selftest_args(Path(td) / "nohost", "dims", "split_dims_6m"))
        nohost.process(dict(event="heartbeat", run_id="dims", guest=True,
                            tick=1, gy=64.25, dim_v0=0, dim_v1=0, views_live=2,
                            dims_active=False, dims_phase="seek",
                            srv_dim_host=0, srv_dim_guest=0))
        assert nohost.done and nohost.failure_class == "split_dims_fields_missing"
        # A client that applied a release the server never sent is a liar.
        liar = Collector(_selftest_args(Path(td) / "liar", "dims", "split_dims_6m"))
        liar.process(dict(event="heartbeat", run_id="dims", guest=True,
                          tick=1, gy=64.25, dim_v0=0, dim_v1=0, views_live=2,
                          dims_active=False, dims_phase="host_trip",
                          srv_dim_host=0, srv_dim_guest=0,
                          **dict(host, rel_rx=1)))
        assert liar.done and liar.failure_class == (
            "split_dims_inconsistent")
        assert good.split_dims_rows[0]["gy"] == 64.25
        assert good.split_dims_rows[0]["dim_v1"] == 0
    print("PASS split_dims_6m registry, stale run, missing gy, ungraded fields, "
          "host-switch fields")


def selftest() -> int:
    assert format_blc({}) == ""
    assert format_blc({"blc": [-1, 9, 9, 120, 4800, 2, 1500]}) == (
        " blc=dim:-1 cols:9 shared:9 client_lit:120 server_lit:4800 age:2s us:1500")
    assert format_blc({"blc": [1, 2]}).startswith(" blc=malformed:")
    # hw1029b/G1: mp_kinds rows fold to the max per key; junk is skipped.
    kinds = {}
    note_mp_kinds(kinds, {"event": "mp_kinds", "mp_rx_bed": 2, "mp_rx_meta": 9})
    note_mp_kinds(kinds, {"event": "mp_kinds", "mp_rx_bed": 0, "mp_rx_meta": 3,
                          "mp_tx_useitem": 1, "mp_rx_attach": "x"})
    assert kinds == {"mp_rx_bed": 2, "mp_rx_meta": 9, "mp_tx_useitem": 1}, kinds
    from analyze_storage_levers import FIELDS
    storage = StorageEvidence()
    storage.note({'event': 'boot'})
    assert storage.summary()['overlay_256_margin_pages'] is None
    for pages, peak in [(10, 181), (3, 181), (200, 200)]:
        storage.note(dict.fromkeys(FIELDS, 0) | dict(event='heartbeat',
                     op=pages, oph=peak, opc=8192, dln=3, dls=1024, dlp=96))
    summary = storage.summary()
    assert summary['ranges']['op'] == {'min': 3, 'peak': 200}
    assert summary['overlay_256_margin_pages'] == 56
    assert 'dls (capacity slack bytes (includes pad)): min=1024 peak=1024' in render_storage(summary)
    storage.note({'event': 'heartbeat'})
    assert not storage.summary()['complete']
    assert storage.summary()['overlay_256_margin_pages'] is None
    storage.note(dict.fromkeys(FIELDS, 0) | dict(event='heartbeat', oph=257))
    assert storage.summary()['ranges']['oph']['peak'] == 257
    assert storage.summary()['recovery_samples'] == 0
    assert 'Recovery telemetry: 0/5' in render_storage(storage.summary())
    storage.note(dict.fromkeys(FIELDS, 0) | dict(event='heartbeat', oph=257,
                 ocy=2, oex=1, owu=31000))
    assert storage.summary()['recovery_samples'] == 1
    assert storage.summary()['ranges']['oex'] == {'min': 1, 'peak': 1}
    assert 'owu (maximum completed recovery wait us): min=31000 peak=31000' in render_storage(storage.summary())
    storage.note(dict.fromkeys(FIELDS, 0) | dict(event='heartbeat', ocy=3,
                 oex=2, owu=-1))
    assert storage.summary()['recovery_samples'] == 1  # malformed is not evidence
    assert 'dlc (compaction enabled): unavailable' in render_storage(storage.summary())
    for enabled in (0, 1, True, -1, 2):
        storage.note(dict.fromkeys(FIELDS, 0) | dict(event='heartbeat', dlc=enabled))
    assert storage.summary()['ranges']['dlc'] == {'min': 0, 'peak': 1}
    assert 'dlc (compaction enabled): min=0 peak=1' in render_storage(storage.summary())
    split_guest_health_selftest()
    split_dims_selftest()
    anchor_selftest()
    # lava-aabb-0924/H4/H5: the chunk-save failure WARN. A first heartbeat
    # already carrying save_fail_n=1 warns (the counter counts from boot), a
    # flat counter never warns, a rise warns once per rise, and the rises
    # ride into the result payload.
    with tempfile.TemporaryDirectory() as td:
        c = Collector(_selftest_args(Path(td) / "sf", "sf", "far_idle_2m"))
        assert c.save_fail_seen == 0 and c.save_fail_warnings == []
        c.note_save_failures({"event": "heartbeat", "tick": 1, "save_fail_n": 1,
                              "save_fail_stage": 7, "save_fail_sub": 6, "save_idx_fb": 0})
        assert len(c.save_fail_warnings) == 1 and "0->1" in c.save_fail_warnings[0]
        assert "stage=7" in c.save_fail_warnings[0] and "sub=6" in c.save_fail_warnings[0]
        c.note_save_failures({"event": "heartbeat", "tick": 2, "save_fail_n": 1})
        assert len(c.save_fail_warnings) == 1
        c.note_save_failures({"event": "heartbeat", "tick": 3, "save_fail_n": 3})
        assert len(c.save_fail_warnings) == 2 and "1->3" in c.save_fail_warnings[1]
        c.note_save_failures({"event": "heartbeat", "tick": 4})  # missing key: ignored
        c.note_save_failures({"event": "heartbeat", "tick": 5, "save_fail_n": "x"})
        assert len(c.save_fail_warnings) == 2
    # SPLIT-SPILL: the overlay_spill record is retained and summarized but
    # never graded -- even maximal / malformed values leave no failure.
    ov = OverlaySpillEvidence()
    assert ov.summary()["records"] == 0 and render_overlay_spill(ov.summary()) == ""
    rec = {"event": "overlay_spill", "v": 1, "tick": 10, "fallback_live": 2,
           "fallback_total": 9, "spill": 3, "mem2_last_spill_bytes": 82176}
    ov.note(rec)
    ov.note({**rec, "tick": 11, "fallback_live": 1, "spill": 5,
             "mem2_last_spill_bytes": 71000})
    ov.note({**rec, "tick": 12, "fallback_live": 1, "spill": 5,
             "mem2_last_spill_bytes": 71000, "fallback_total": 10})
    for bad in ({**rec, "v": 2}, {**rec, "spill": -1}, {**rec, "fallback_live": "1"},
                {**rec, "mem2_last_spill_bytes": 2 ** 32}, {**rec, "tick": None}):
        ov.note(bad)
    sm = ov.summary()
    assert sm["graded"] is False and sm["records"] == 3 and sm["malformed"] == 5
    assert sm["max_fallback_live"] == 2 and sm["last"]["fallback_total"] == 10
    assert sm["spills_by_last_request_bytes"] == {"71000": 2, "82176": 3}
    text = render_overlay_spill(sm)
    assert "not graded" in text and "last spill request=71000 B" in text
    with tempfile.TemporaryDirectory() as td:
        c = Collector(_selftest_args(Path(td) / "ov", "ov", "split_play_8m"))
        wide = {"event": "overlay_spill", "v": 1, "tick": 2 ** 32 - 1,
                **{k: 2 ** 32 - 1 for k in OVERLAY_SPILL_KEYS}, "run_id": "ov"}
        line = json.dumps(wide, separators=(",", ":"))
        assert len(line.encode()) + 1 <= 2303
        assert not ingest_telemetry_line(c, line)
        assert not ingest_telemetry_line(c, json.dumps({**wide, "v": "x"}))
        assert c.fail_reason is None and c.failure_class is None
        assert c.overlay_spill.summary()["records"] == 1
        assert c.overlay_spill.summary()["malformed"] == 1
        recorded = Path(td, "ov", "telemetry.ndjson").read_text().splitlines()
        assert json.loads(recorded[0])["event"] == "overlay_spill"
    # G9 R6: the "lq" light-queue array. Absent = ignored (older .dol);
    # malformed = FAIL; maxima are element-wise; any light stop, drain
    # ceiling/OOM, flood ceiling or stack drop FAILs.
    with tempfile.TemporaryDirectory() as td:
        c = Collector(_selftest_args(Path(td) / "lq", "lq", "far_idle_2m"))
        assert c.note_light_queue({"event": "heartbeat", "tick": 1}) is None
        assert c.lq_max is None
        ok = [1416, 0, 21, 0, 3800, 0, 9000, 0, 0, 0, 0, 0, 0]
        assert c.note_light_queue({"event": "heartbeat", "tick": 2, "lq": ok}) is None
        more = [2048, 64, 9, 1, 100, 5, 20000, 7, 0, 0, 0, 0, 0]
        assert c.note_light_queue({"event": "heartbeat", "tick": 3, "lq": more}) is None
        assert c.lq_max == [2048, 64, 21, 1, 3800, 5, 20000, 7, 0, 0, 0, 0, 0]
        for k, name in enumerate(LQ_FIELDS):
            if name not in LQ_FAIL_FIELDS:
                continue
            bad = list(ok)
            bad[k] = 1
            r = c.note_light_queue({"event": "heartbeat", "tick": 4, "lq": bad})
            assert r and f"{name}=1" in r, (name, r)
        assert c.note_light_queue({"event": "heartbeat", "tick": 5, "lq": ok[:12]})
        assert c.note_light_queue({"event": "heartbeat", "tick": 6, "lq": ok[:12] + ["0"]})
        assert c.note_light_queue({"event": "heartbeat", "tick": 7, "lq": ok[:12] + [-1]})
        assert c.note_light_queue({"event": "heartbeat", "tick": 8, "lq": ok[:12] + [2 ** 32]})
        assert c.light_queue_summary()["layout"] == 1
        # a v1 run cannot switch layout mid-run
        assert "layout" in c.note_light_queue(
            {"event": "heartbeat", "tick": 9, "lq": [0] * len(LQ2_FIELDS)})
    # G9 R6 v2 (ASTRA-G9-REVIEW-2 item 5): the 26-entry layout; maxima per
    # element; every FAIL field (post-drain debt per slot, stops, ceilings,
    # OOM, checkpoint OOM, flood ceiling, stack drop) FAILs alone; uint16
    # bounds; --require-g9-evidence rejects a missing array, the v1 layout
    # and a run that ends without any v2 heartbeat.
    assert len(LQ2_FIELDS) == 26 and len(set(LQ2_FIELDS)) == 26
    with tempfile.TemporaryDirectory() as td:
        c = Collector(_selftest_args(Path(td) / "lq2", "lq2", "far_idle_2m"))
        ok2 = [1, 0, 2, 0, 1, 0, 64, 0, 288, 0, 21, 0, 3800, 0, 29878, 0, 0, 0,
               4100, 9000, 0, 0, 0, 0, 0, 0]
        assert c.note_light_queue({"event": "heartbeat", "tick": 1, "lq": ok2}) is None
        more2 = list(ok2)
        more2[6] = 320
        more2[13] = 77
        assert c.note_light_queue({"event": "heartbeat", "tick": 2, "lq": more2}) is None
        assert c.lq_max[6] == 320 and c.lq_max[13] == 77 and c.lq_max[12] == 3800
        summ = c.light_queue_summary()
        assert summ["layout"] == 2 and summ["front_kib0"] == 320
        for k, name in enumerate(LQ2_FIELDS):
            bad = list(ok2)
            bad[k] = 1 if bad[k] == 0 else bad[k]
            r = c.note_light_queue({"event": "heartbeat", "tick": 3, "lq": bad})
            if name in LQ2_FAIL_FIELDS:
                assert r and f"{name}=1" in r, (name, r)
            else:
                assert r is None, (name, r)
        assert c.note_light_queue({"event": "heartbeat", "tick": 4, "lq": ok2[:25] + [65536]})
        assert c.note_light_queue({"event": "heartbeat", "tick": 5, "lq": ok2[:25] + [True]})
        assert c.note_light_queue({"event": "heartbeat", "tick": 6, "lq": ok2 + [0]})
        assert c.note_light_queue({"event": "heartbeat", "tick": 7, "lq": "0,0"})
        # without the flag: a heartbeat lacking lq is an older artifact
        assert c.note_light_queue({"event": "heartbeat", "tick": 8}) is None
    with tempfile.TemporaryDirectory() as td:
        c = Collector(_selftest_args(Path(td) / "lq2r", "lq2r", "far_idle_2m",
                                     require_g9_evidence=True))
        assert "missing" in c.note_light_queue({"event": "heartbeat", "tick": 1})
        assert "older" in c.note_light_queue(
            {"event": "heartbeat", "tick": 2, "lq": [0] * len(LQ_FIELDS)})
        assert c.lq_max is None
        c.pass_("complete")
        assert not c.done or c.pass_reason != "complete"
        c2 = Collector(_selftest_args(Path(td) / "lq2s", "lq2s", "far_idle_2m",
                                      require_g9_evidence=True))
        assert c2.note_light_queue({"event": "heartbeat", "tick": 1,
                                    "lq": [0] * len(LQ2_FIELDS)}) is None
    split_action_selftest()
    split_recovery_selftest()
    mp_play_selftest()
    tps_seal_selftest()
    # Unsupported certification must fail BEFORE connecting or writing a run.
    # Previously the CLI accepted this and spent five minutes collecting a
    # profile that _finish_normal could never certify.
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "unsupported_release"
        rejected = subprocess.run(
            [sys.executable, __file__, "--profile", "perf_stress_5m",
             "--release-gate", "--out", str(out),
             "--tcp-connect-host", "192.0.2.1", "--connect-timeout", "1"],
            capture_output=True, text=True, timeout=10)
        assert rejected.returncode == 2, rejected.stderr
        assert "--release-gate requires" in rejected.stderr
        assert not out.exists()
        print("PASS unsupported release profile rejected before connection/artifacts")
    good = dict(perf_frames=18000, perf_duration_us=300006000,
                perf_max_us=16667, perf_over_30ms=0, perf_gpu_over_16600=0,
                perf_gpu_over_8000=0, perf_invalid=0)
    assert release_frame_error(good, True) is None
    # 10-31 ruling: an isolated 20.5 ms frame (faad2a0 far_walk) passes.
    assert release_frame_error({**good, "perf_max_us": 20539}, True) is None
    for bad in ({}, {**good, "perf_max_us": 50000},
                {**good, "perf_over_30ms": 1},
                {**good, "perf_gpu_over_16600": 1801},
                {**good, "perf_gpu_over_8000": 9001},
                {**good, "perf_duration_us": 1000},
                {**good, "perf_frames": True}):
        assert release_frame_error(bad, True) is not None
    with tempfile.TemporaryDirectory() as td:
        for defect in ("none", "missing", "hitch", "memory", "regressed"):
            rc = Collector(_selftest_args(Path(td) / ("release_" + defect),
                           "release", "keepup_walk_6m", release_gate=True))
            rc.profile = {**rc.profile, "min_hb": 2}
            for i in range(2):
                event = {**good, "event": "heartbeat", "run_id": "release",
                         "profile": "keepup_walk_6m", "build_id": "abc_06.27",
                         "world_loaded": True, "fring": 1, "x": i * 64, "z": 0,
                         "mem1_free_kb": 5000, "mem2_carve_free_kb": 3000,
                         "oom": 0, "mesh_trunc": 0}
                if defect == "missing":
                    event.pop("perf_max_us")
                if defect == "hitch" and i == 0:
                    event["perf_max_us"] = 50000
                if defect == "memory":
                    event["mem1_free_kb"] = 1000
                if defect == "regressed" and i == 1:
                    event["perf_frames"] -= 1
                rc.process(event)
            rc.process({"event": "profile_done", "run_id": "release",
                        "profile": "keepup_walk_6m", "status": "ok"})
            assert (rc.write_result() == 0) == (defect == "none")
            if defect == "none":
                proof = json.loads((rc.out / "PASS.json").read_text())
                assert proof["release_performance_pass"] is True
        # 2026-09-07 carve-observer stream death, collector half. The Wii
        # sender abandons a line it could not fully queue and resyncs the
        # framing with a leading LF, so an ambiguous prefix reaches us as ONE
        # malformed NDJSON line. Isolated bad lines must be skipped, never
        # end the run; only three CONSECUTIVE ones mean the stream is really
        # corrupt. This is what lets the sender treat -ENOBUFS/-EAGAIN as
        # back-pressure instead of a fatal error.
        frame_dir = Path(td) / "framing"
        fr = Collector(_selftest_args(frame_dir, "fr", "heartbeat_selftest"))
        err = json.JSONDecodeError("x", "x", 0)
        for _ in range(20):
            assert not fr.note_bad_line(err)     # isolated prefix, skipped
            assert not fr.note_bad_line(err)     # two in a row is still fine
            fr.process({"event": "heartbeat", "run_id": "fr"})  # resync worked
        assert fr.bad_lines == 40 and fr.bad_lines_consecutive == 0
        assert not fr.done and fr.failure_class is None
        assert not fr.note_bad_line(err)
        assert not fr.note_bad_line(err)
        assert fr.note_bad_line(err)             # three in a row: really corrupt
        assert fr.done and fr.failure_class == "invalid_json"
        pass_dir = Path(td) / "pass"
        fail_dir = Path(td) / "fail"
        args = _selftest_args(pass_dir, "self", "heartbeat_selftest")
        c = Collector(args)
        c.process({"event": "boot", "run_id": "self", "profile": "heartbeat_selftest", "build_id": "abc_06.27"})
        c.process({
            "event": "crash_mailbox",
            "run_id": "self",
            "profile": "heartbeat_selftest",
            "cause": "../hang report.txt",
            "dump_id": 1,
            "seq": 0,
            "last": False,
            "text": "old ",
        })
        c.process({
            "event": "crash_mailbox",
            "run_id": "self",
            "profile": "heartbeat_selftest",
            "cause": "../hang report.txt",
            "dump_id": 1,
            "seq": 1,
            "last": True,
            "text": "hang",
        })
        mailbox = pass_dir / "crash_mailbox_hang_report.txt"
        assert mailbox.read_text(encoding="utf-8") == "old hang"
        # The watchdog's app-dir FALLBACK report shares the primary's BASENAME, and it is
        # replayed only on the boot where the primary write failed -- so if the two landed
        # on one artifact, the run that most needs the evidence would keep just one of
        # them. Its parent directory has to make the name distinct.
        c.process({
            "event": "crash_mailbox",
            "run_id": "self",
            "profile": "heartbeat_selftest",
            "cause": "sd:/apps/CavEX/hang_report.txt",
            "dump_id": 3,
            "seq": 0,
            "last": True,
            "text": "fallback",
        })
        fallback = pass_dir / "crash_mailbox_CavEX_hang_report.txt"
        assert fallback.read_text(encoding="utf-8") == "fallback"
        assert mailbox.read_text(encoding="utf-8") == "old hang", (
            "the app-dir fallback report overwrote the primary mailbox artifact"
        )
        for i in range(50):
            c.process({
                "event": "heartbeat",
                "run_id": "self",
                "profile": "heartbeat_selftest",
                "build_id": "abc_06.27",
                "srv_rt_us": i,
                "srv_wt_us": i + 1,
                "oom": 0,
                "mesh_trunc": 0,
                "fps": 60,
            })
        c.process({
            "event": "profile_done",
            "run_id": "self",
            "profile": "heartbeat_selftest",
            "status": "ok",
        })
        assert c.write_result() == 0
        passed = json.loads((pass_dir / "PASS.json").read_text(encoding="utf-8"))
        assert passed["reason"] == "profile_done received"
        assert passed["latest"]["event"] == "profile_done"
        assert passed["last_heartbeat"]["event"] == "heartbeat"
        assert passed['storage']['heartbeats'] > 0
        assert passed['storage']['samples'] == 0
        assert passed["last_heartbeat"]["build_id"] == "abc_06.27"
        assert passed["last_heartbeat"]["profile"] == "heartbeat_selftest"
        pass_summary = (pass_dir / "summary.md").read_text(encoding="utf-8")
        assert "Latest build: `abc_06.27`" in pass_summary
        assert "Latest fps: `60`" in pass_summary

        fail_args = _selftest_args(fail_dir, "freeze", "redstone_clock_2m",
                                   duration=120)
        fc = Collector(fail_args)
        fc.process({
            "event": "heartbeat",
            "run_id": "freeze",
            "profile": "redstone_clock_2m",
            "build_id": "abc_06.27",
            "tick": 77,
            "fps": 59.0,
            "srv_rt_us": 1200,
            "srv_wt_us": 3400,
            "mem1_free_kb": 12345,
            "mem2_free_kb": 54321,
            "x": 1.25,
            "y": 64.0,
            "z": -2.5,
            "cflood": 4096,
            "near_evict_defer": 6,
            "ms_bfs": 1.5,
            "ms_light": 2.5,
            "ms_lod": 3.5,
            "ms_build": 4.5,
            "ms_bfs_max": 11.5,
            "ms_light_max": 12.5,
            "ms_lod_max": 13.5,
            "ms_build_max": 14.5,
            "wd_main": 4,
            "wd_srv": 8,
            "wd_worker": 2,
            "wd_hb_gap_max_ms": 2750,
            "world_loaded": True,
            "creative": False,
        })
        fc.fail("TCP telemetry socket closed", "socket_closed_after_heartbeat")
        assert fc.write_result() == 1
        fail = json.loads((fail_dir / "FAIL.json").read_text(encoding="utf-8"))
        ctx = fail["failure_context"]
        assert fail["failure_class"] == "socket_closed_after_heartbeat"
        assert ctx["context_source_event"] == "heartbeat"
        assert ctx["last_heartbeat_build"] == "abc_06.27"
        assert ctx["last_heartbeat_profile"] == "redstone_clock_2m"
        assert ctx["latest_tick"] == 77
        assert ctx["wd_main_name"] == "LIGHTING"
        assert ctx["wd_srv_name"] == "ENSURE_POP"
        assert ctx["wd_worker_name"] == "BUILD"
        assert ctx["wd_hb_gap_max_ms"] == 2750
        assert ctx["near_evict_defer"] == 6
        assert ctx["ms_lod"] == 3.5
        assert ctx["ms_build_max"] == 14.5
        assert ctx["creative"] is False
        summary = (fail_dir / "summary.md").read_text(encoding="utf-8")
        assert "Last watchdog phases: MAIN=LIGHTING(4)" in summary
        assert "Last main timings: bfs=1.5ms light=2.5ms lod=3.5ms build=4.5ms" in summary
        assert "creative=`False`" in summary
        assert "near_evict_defer=6" in summary
        assert "hb_gap_max=2750ms" in summary

        # --- build-stamp: hash-only comparison (cross-midnight must NOT fail) ---
        assert build_stamp_hash("abc+_06.27") == "abc+"
        assert build_stamp_hash("unstamped") == "unstamped"
        assert build_stamps_match("abc_06.28", "abc_06.27")
        assert not build_stamps_match("def_06.27", "abc_06.27")
        # Degraded 'nogit' stamps: the date is the only distinguishing bit, so
        # they must compare by FULL string (a stale nogit .dol must not pass).
        assert not build_stamps_match("nogit_06.20", "nogit_07.02")
        assert build_stamps_match("nogit_07.02", "nogit_07.02")
        mid = Collector(_selftest_args(Path(td) / "mid", "mid", "heartbeat_selftest"))
        mid.process({"event": "heartbeat", "run_id": "mid",
                     "profile": "heartbeat_selftest", "build_id": "abc_06.28",
                     "fps": 60})
        assert mid.hb_count == 1 and not mid.done, "midnight date drift must not fail"
        mid.process({"event": "heartbeat", "run_id": "mid",
                     "profile": "heartbeat_selftest", "build_id": "def_06.27",
                     "fps": 60})
        assert mid.done and mid.failure_class == "build_mismatch"

        # --- redstone_clock_2m scenario gate: idle superflat must NOT pass ---
        def _rs_collector(name: str) -> Collector:
            return Collector(_selftest_args(Path(td) / name, name,
                                            "redstone_clock_2m", duration=120))

        rs_idle = _rs_collector("rs_idle")
        for i in range(100):
            rs_idle.process({"event": "heartbeat", "run_id": "rs_idle",
                             "profile": "redstone_clock_2m",
                             "build_id": "abc_06.27", "world_loaded": True,
                             "ndisp": 50, "fps": 60})
        rs_idle.process({"event": "profile_done", "run_id": "rs_idle",
                         "profile": "redstone_clock_2m", "status": "ok"})
        assert rs_idle.write_result() == 1
        rs_idle_fail = json.loads(
            ((Path(td) / "rs_idle") / "FAIL.json").read_text(encoding="utf-8"))
        assert rs_idle_fail["failure_class"] == "scenario_not_engaged"

        rs_live = _rs_collector("rs_live")
        for i in range(100):
            rs_live.process({"event": "heartbeat", "run_id": "rs_live",
                             "profile": "redstone_clock_2m",
                             "build_id": "abc_06.27", "world_loaded": True,
                             "ndisp": 100 + i * 60, "fps": 60})
        rs_live.process({"event": "profile_done", "run_id": "rs_live",
                         "profile": "redstone_clock_2m", "status": "ok"})
        assert rs_live.write_result() == 0
        rs_pass = json.loads(
            ((Path(td) / "rs_live") / "PASS.json").read_text(encoding="utf-8"))
        assert rs_pass["max_ndisp"] == 100 + 99 * 60
        assert rs_pass["scenario"] == "redstone"

        # --- save_migrate: only the completed production reopen plus all four
        # physical walls in every default fixture region may pass. ---
        sm_dir = Path(td) / "save_migrate"
        sm = Collector(_selftest_args(sm_dir, "sm", "save_migrate", duration=300))
        for i in range(5):
            sm.process({"event": "heartbeat", "run_id": "sm",
                        "profile": "save_migrate", "build_id": "abc_06.27",
                        "world_loaded": True, "smig_state": 4,
                        "smig_ms": 47000, "smig_regions": 12,
                        "smig_chunks": 40, "smig_signs": 32,
                        "smig_expected": 32, "smig_verified": 32,
                        "fps": 60})
        sm.process({"event": "profile_done", "run_id": "sm",
                    "profile": "save_migrate", "status": "ok"})
        assert sm.write_result() == 0
        sm_pass = json.loads((sm_dir / "PASS.json").read_text(encoding="utf-8"))
        assert sm_pass["save_migrate_state"] == 4
        assert sm_pass["save_migrate_ms"] == 47000
        assert sm_pass["save_migrate_verified"] == 32
        assert sm_pass["elapsed_seconds"] >= 0

        sm_bad_dir = Path(td) / "save_migrate_bad"
        sm_bad = Collector(_selftest_args(
            sm_bad_dir, "sm_bad", "save_migrate", duration=300))
        for i in range(5):
            sm_bad.process({"event": "heartbeat", "run_id": "sm_bad",
                            "profile": "save_migrate", "build_id": "abc_06.27",
                            "world_loaded": True, "smig_state": 4,
                            "smig_ms": 47000, "smig_regions": 12,
                            "smig_chunks": 40, "smig_signs": 32,
                            "smig_expected": 32, "smig_verified": 31,
                            "fps": 60})
        sm_bad.process({"event": "profile_done", "run_id": "sm_bad",
                        "profile": "save_migrate", "status": "ok"})
        assert sm_bad.write_result() == 1
        sm_fail = json.loads(
            (sm_bad_dir / "FAIL.json").read_text(encoding="utf-8"))
        assert sm_fail["failure_class"] == "scenario_not_engaged"

        # --- far_walk_6m scenario gate: standing still must NOT pass ---
        def _walk_collector(name: str) -> Collector:
            return Collector(_selftest_args(Path(td) / name, name,
                                            "far_walk_6m", duration=360))

        walk_still = _walk_collector("walk_still")
        for i in range(300):
            walk_still.process({"event": "heartbeat", "run_id": "walk_still",
                                "profile": "far_walk_6m",
                                "build_id": "abc_06.27", "world_loaded": True,
                                "far_view_distance": 6, "fring": 1,
                                "x": 8.0, "y": 70.0, "z": 8.0, "fps": 60})
        walk_still.process({"event": "profile_done", "run_id": "walk_still",
                            "profile": "far_walk_6m", "status": "ok"})
        assert walk_still.write_result() == 1
        walk_fail = json.loads(
            ((Path(td) / "walk_still") / "FAIL.json").read_text(encoding="utf-8"))
        assert walk_fail["failure_class"] == "scenario_not_engaged"

        walk_move = _walk_collector("walk_move")
        for i in range(300):
            walk_move.process({"event": "heartbeat", "run_id": "walk_move",
                               "profile": "far_walk_6m",
                               "build_id": "abc_06.27", "world_loaded": True,
                               "far_view_distance": 6, "fring": 1,
                               "x": 8.0 + i * 0.5, "y": 70.0, "z": 8.0,
                               "fps": 60})
        walk_move.process({"event": "profile_done", "run_id": "walk_move",
                           "profile": "far_walk_6m", "status": "ok"})
        assert walk_move.write_result() == 0

        # --- mp_lan_walk_10m scenario gate: the same displacement proof as
        # far_walk_6m, but WITHOUT the far-ring requirement -- mp profiles
        # leave far_view_distance/fring unset (server-driven default), so
        # this gate must pass on displacement + world_loaded alone. Standing
        # still must still NOT pass.
        def _mp_walk_collector(name: str) -> Collector:
            return Collector(_selftest_args(Path(td) / name, name,
                                            "mp_lan_walk_10m", duration=600))

        # Both streams below carry lw_to_admits 0 on EVERY heartbeat, because
        # since B-a (Astra Lane B item 2) this profile's heartbeats must: the
        # load-ready backstop counter is required, and required to read zero.
        # That is a deliberate tightening of mp_lan_walk_10m's own rule, so the
        # displacement gate these two cases exist to prove is still what they
        # prove rather than being masked by the new one. The counter's own
        # cases -- missing, and non-zero -- are mp_play_selftest's.
        mp_walk_still = _mp_walk_collector("mp_walk_still")
        for i in range(480):
            mp_walk_still.process({"event": "heartbeat", "run_id": "mp_walk_still",
                                   "profile": "mp_lan_walk_10m", "load_screen_ms": 3000,
                                   "build_id": "abc_06.27", "world_loaded": True,
                                   "lw_to_admits": 0,
                                   "mp_state": 4, "mp_gen": 1,
                                   "mp_move_ticks": i, "mp_move_samples": i,
                                   "x": 8.0, "y": 70.0, "z": 8.0, "fps": 60})
        mp_walk_still.process({"event": "profile_done", "run_id": "mp_walk_still",
                               "profile": "mp_lan_walk_10m", "status": "ok"})
        assert mp_walk_still.write_result() == 1
        mp_walk_fail = json.loads(
            ((Path(td) / "mp_walk_still") / "FAIL.json").read_text(encoding="utf-8"))
        assert mp_walk_fail["failure_class"] == "scenario_not_engaged"

        # No far_view_distance/fring field in this heartbeat stream at all --
        # proves the mp_walk gate does not depend on the far-ring fields
        # far_walk's gate requires.
        mp_walk_move = _mp_walk_collector("mp_walk_move")
        for i in range(480):
            mp_walk_move.process({"event": "heartbeat", "run_id": "mp_walk_move",
                                  "profile": "mp_lan_walk_10m", "load_screen_ms": 3000,
                                  "build_id": "abc_06.27", "world_loaded": True,
                                  "lw_to_admits": 0,
                                  "mp_state": 4, "mp_gen": 1,
                                  "mp_move_ticks": i, "mp_move_samples": i,
                                  "x": 8.0 + i * 0.5, "y": 70.0, "z": 8.0,
                                  "fps": 60})
        mp_walk_move.process({"event": "profile_done", "run_id": "mp_walk_move",
                              "profile": "mp_lan_walk_10m", "status": "ok"})
        assert mp_walk_move.write_result() == 0
        mp_walk_pass = json.loads(
            ((Path(td) / "mp_walk_move") / "PASS.json").read_text(encoding="utf-8"))
        assert mp_walk_pass["max_fring"] is None

        # --- mp_reconnect_soak scenario gate: too few cycles must NOT pass,
        # a wedge must NOT pass even with enough cycles seen before it, and
        # reaching min_cycles with no wedge must pass. ---
        def _mrs_collector(name: str) -> Collector:
            return Collector(_selftest_args(Path(td) / name, name,
                                            "mp_reconnect_soak", duration=480))

        mrs_short = _mrs_collector("mrs_short")
        for i in range(400):
            mrs_short.process({"event": "heartbeat", "run_id": "mrs_short",
                               "profile": "mp_reconnect_soak",
                               "build_id": "abc_06.27", "world_loaded": True,
                               "mrs_cycles": min(i // 60, 5), "mrs_target": 6,
                               "mrs_wedge": False, "fps": 60})
        mrs_short.process({"event": "profile_done", "run_id": "mrs_short",
                           "profile": "mp_reconnect_soak", "status": "ok"})
        assert mrs_short.write_result() == 1
        mrs_short_fail = json.loads(
            ((Path(td) / "mrs_short") / "FAIL.json").read_text(encoding="utf-8"))
        assert mrs_short_fail["failure_class"] == "scenario_not_engaged"
        assert "5/6" in mrs_short_fail["reason"]

        # A wedge must fail even though enough cycles were already seen
        # before it fired -- "no wedge" is its own hard requirement, not
        # offset by cycle count.
        mrs_wedged = _mrs_collector("mrs_wedged")
        for i in range(400):
            mrs_wedged.process({
                "event": "heartbeat", "run_id": "mrs_wedged",
                "profile": "mp_reconnect_soak", "build_id": "abc_06.27",
                "world_loaded": True, "mrs_cycles": 6, "mrs_target": 6,
                "mrs_wedge": i >= 390, "fps": 60})
        mrs_wedged.process({"event": "profile_done", "run_id": "mrs_wedged",
                            "profile": "mp_reconnect_soak", "status": "ok"})
        assert mrs_wedged.write_result() == 1
        mrs_wedged_fail = json.loads(
            ((Path(td) / "mrs_wedged") / "FAIL.json").read_text(encoding="utf-8"))
        assert mrs_wedged_fail["failure_class"] == "scenario_not_engaged"
        assert "wedged" in mrs_wedged_fail["reason"]

        mrs_ok = _mrs_collector("mrs_ok")
        for i in range(400):
            mrs_ok.process({"event": "heartbeat", "run_id": "mrs_ok",
                            "profile": "mp_reconnect_soak",
                            "build_id": "abc_06.27", "world_loaded": True,
                            "mrs_cycles": min(i // 60, 6), "mrs_target": 6,
                            "mrs_wedge": False,
                            "wd_hb_gap_max_ms": 200 + i, "fps": 60})
        mrs_ok.process({"event": "profile_done", "run_id": "mrs_ok",
                        "profile": "mp_reconnect_soak", "status": "ok"})
        assert mrs_ok.write_result() == 0
        mrs_ok_pass = json.loads(
            ((Path(td) / "mrs_ok") / "PASS.json").read_text(encoding="utf-8"))
        assert mrs_ok_pass["max_mrs_cycles"] == 6
        assert mrs_ok_pass["mrs_wedge_seen"] is False
        # max_hb_gap_ms (2026-09 stall-hunt follow-up): a PASS must now carry
        # the peak gap seen across the whole run, not just a FAIL's context.
        assert mrs_ok_pass["max_hb_gap_ms"] == 200 + 399

        # --- Retro 1 corrective action (2026-09-13): mp_* profiles must FAIL
        # on player_health==0 in the first 60 ticks -- the collector must
        # judge by player_health itself, not the server_local `deaths`
        # counter (which reads 0 in MP). Four cases: the real defect (a),
        # the tick>60 boundary (b), a non-mp profile (c), and the -1 no-body
        # sentinel (d). ---
        mp_health_dir = Path(td) / "mp_health_early_death"
        mp_health = Collector(_selftest_args(mp_health_dir, "mp_health_early_death",
                                             "mp_lan_walk_10m", duration=600))
        for tick, health in ((1, 160), (5, 160), (10, 0)):
            mp_health.process({"event": "heartbeat", "run_id": "mp_health_early_death",
                               "profile": "mp_lan_walk_10m", "load_screen_ms": 3000, "build_id": "abc_06.27",
                               "world_loaded": True, "lw_to_admits": 0,
                               "tick": tick, "player_health": health,
                               "x": 8.0, "y": 70.0, "z": 8.0, "fps": 60})
        assert mp_health.done and mp_health.failure_class == "player_died_early"
        assert mp_health.fail_reason == "player died within the first 60 s (health 0 at tick 10)"
        print("PASS (a) mp_lan_walk_10m health 160->0 at tick 10 FAILs with the exact message")

        mp_late_dir = Path(td) / "mp_health_late_death"
        mp_late = Collector(_selftest_args(mp_late_dir, "mp_health_late_death",
                                           "mp_lan_walk_10m", duration=600))
        mp_late.process({"event": "heartbeat", "run_id": "mp_health_late_death",
                         "profile": "mp_lan_walk_10m", "load_screen_ms": 3000, "build_id": "abc_06.27",
                         "world_loaded": True, "lw_to_admits": 0,
                         "tick": 61, "player_health": 0,
                         "x": 8.0, "y": 70.0, "z": 8.0, "fps": 60})
        assert not mp_late.done, "tick 61 death must not trip the first-60s health floor"
        print("PASS (b) mp_lan_walk_10m health 0 at tick 61 is NOT the early-death error")

        nonmp_dir = Path(td) / "nonmp_health_zero"
        nonmp = Collector(_selftest_args(nonmp_dir, "nonmp_health_zero", "far_idle_2m"))
        nonmp.process({"event": "heartbeat", "run_id": "nonmp_health_zero",
                       "profile": "far_idle_2m", "build_id": "abc_06.27",
                       "world_loaded": True, "tick": 10, "player_health": 0,
                       "fring": 1, "far_view_distance": 6, "fps": 60})
        assert not nonmp.done, "a non-mp profile must not trip the mp early-death gate"
        print("PASS (c) non-mp profile (far_idle_2m) health 0 at tick 10 is NOT the early-death error")

        sentinel_dir = Path(td) / "mp_health_sentinel"
        sentinel = Collector(_selftest_args(sentinel_dir, "mp_health_sentinel",
                                            "mp_lan_walk_10m", duration=600))
        sentinel.process({"event": "heartbeat", "run_id": "mp_health_sentinel",
                          "profile": "mp_lan_walk_10m", "load_screen_ms": 3000, "build_id": "abc_06.27",
                          "world_loaded": True, "lw_to_admits": 0,
                          "tick": 0, "player_health": -1,
                          "x": 8.0, "y": 70.0, "z": 8.0, "fps": 60})
        assert not sentinel.done, "the -1 no-body sentinel must not trip the health floor"
        print("PASS (d) mp_lan_walk_10m health -1 sentinel at tick 0 is NOT the early-death error")

        # --- far_idle_2m: the far ring must have been ACTIVE (fring >= 1).
        # ring OFF on a real Wii .dol reports far_view_distance=5 (clamped, NOT
        # -1) with fring=0 -- the raw fvd can never express "off", which is why
        # the gate reads fring.
        idle_off = Collector(_selftest_args(Path(td) / "idle_off", "idle_off",
                                            "far_idle_2m", duration=120))
        for i in range(100):
            idle_off.process({"event": "heartbeat", "run_id": "idle_off",
                              "profile": "far_idle_2m", "build_id": "abc_06.27",
                              "world_loaded": True, "far_view_distance": 5,
                              "fring": 0, "fps": 60})
        idle_off.process({"event": "profile_done", "run_id": "idle_off",
                          "profile": "far_idle_2m", "status": "ok"})
        assert idle_off.write_result() == 1
        idle_fail = json.loads(
            ((Path(td) / "idle_off") / "FAIL.json").read_text(encoding="utf-8"))
        assert idle_fail["failure_class"] == "scenario_not_engaged"

        idle_on = Collector(_selftest_args(Path(td) / "idle_on", "idle_on",
                                           "far_idle_2m", duration=120))
        for i in range(100):
            # Far-ring keep-up probe (2026-09-03 gate 4): a walk-in transient
            # (far_missing/far_missing_max spike, then settle) plus a
            # steady-state server gen sample, folded into ONE heartbeat
            # stream so the collector's max-tracking is exercised end to end
            # (device -> heartbeat JSON -> Collector.process -> PASS.json).
            idle_on.process({"event": "heartbeat", "run_id": "idle_on",
                             "profile": "far_idle_2m", "build_id": "abc_06.27",
                             "world_loaded": True, "far_view_distance": 6,
                             "fring": 1, "fps": 60,
                             "far_missing": 12 if i < 5 else 0,
                             "far_missing_max": 12,
                             "far_settle_ms": 4200 if i >= 5 else 0,
                             "srv_gen_us": 800 + i, "srv_gen_cols": 2,
                             "srv_gen_backlog": max(0, 12 - i)})
        idle_on.process({"event": "profile_done", "run_id": "idle_on",
                         "profile": "far_idle_2m", "status": "ok"})
        assert idle_on.write_result() == 0
        idle_pass = json.loads(
            ((Path(td) / "idle_on") / "PASS.json").read_text(encoding="utf-8"))
        assert idle_pass["max_fring"] == 1
        # Both the collector's own max-of-instantaneous-far_missing AND the
        # device-reported running max must reach the same peak here.
        assert idle_pass["max_far_missing"] == 12
        assert idle_pass["max_far_missing_reported"] == 12
        assert idle_pass["max_far_settle_ms"] == 4200
        assert idle_pass["max_srv_gen_us"] == 800 + 99
        assert idle_pass["max_srv_gen_cols"] == 2
        assert idle_pass["max_srv_gen_backlog"] == 12

        # --- watchdog_inject: stale heartbeats must WAIT for the dump, an
        # empty dump must fail, and a complete dump must pass ---
        wd_pass = Collector(_selftest_args(Path(td) / "wd_pass", "wd", "watchdog_inject"))
        for i in range(6):
            wd_pass.process({"event": "heartbeat", "run_id": "wd",
                             "profile": "watchdog_inject",
                             "build_id": "abc_06.27", "fps": 60})
        # Heartbeats went stale 20s ago (past stale_timeout=5): with a dump
        # expected, tick() must NOT fail inside the dump-wait window.
        wd_pass.last_hb = time.monotonic() - 20
        wd_pass.tick()
        assert not wd_pass.done, "stale heartbeat must not fail an expect_dump run in the dump window"
        wd_pass.process({"event": "watchdog_dump", "run_id": "wd",
                         "profile": "watchdog_inject", "cause": "HANG",
                         "dump_id": 1, "seq": 0, "last": False,
                         "text": "phase rows "})
        wd_pass.process({"event": "watchdog_dump", "run_id": "wd",
                         "profile": "watchdog_inject", "cause": "HANG",
                         "dump_id": 1, "seq": 1, "last": True,
                         "text": "PC=8012 LR=8010"})
        assert wd_pass.write_result() == 0

        def _wd_heartbeats(c: Collector, run_id: str, count: int = 6) -> None:
            for i in range(count):
                c.process({"event": "heartbeat", "run_id": run_id,
                           "profile": "watchdog_inject",
                           "build_id": "abc_06.27", "fps": 60})

        wd_empty = Collector(_selftest_args(Path(td) / "wd_empty", "wde", "watchdog_inject"))
        _wd_heartbeats(wd_empty, "wde")
        wd_empty.process({"event": "watchdog_dump", "run_id": "wde",
                          "profile": "watchdog_inject", "cause": "HANG",
                          "dump_id": 1, "seq": 0, "last": True, "text": ""})
        assert wd_empty.done and wd_empty.failure_class == "empty_dump"

        # A dump with too few preceding heartbeats must not certify.
        wd_early = Collector(_selftest_args(Path(td) / "wd_early", "wdy", "watchdog_inject"))
        _wd_heartbeats(wd_early, "wdy", count=2)
        wd_early.process({"event": "watchdog_dump", "run_id": "wdy",
                          "profile": "watchdog_inject", "cause": "HANG",
                          "dump_id": 1, "seq": 0, "last": True, "text": "evidence"})
        assert wd_early.done and wd_early.failure_class == "too_few_heartbeats"

        # A FAULT dump = the run crashed; it must not certify the hang detector.
        wd_fault = Collector(_selftest_args(Path(td) / "wd_fault", "wdf", "watchdog_inject"))
        _wd_heartbeats(wd_fault, "wdf")
        wd_fault.process({"event": "fault", "run_id": "wdf",
                          "profile": "watchdog_inject", "cause": "DSI",
                          "dump_id": 1, "seq": 0, "last": True,
                          "text": "PC=8012 LR=8010"})
        assert wd_fault.done and wd_fault.failure_class == "unexpected_fault"

        # A dump with a missing middle chunk is truncated evidence -> fail.
        wd_holes = Collector(_selftest_args(Path(td) / "wd_holes", "wdh", "watchdog_inject"))
        _wd_heartbeats(wd_holes, "wdh")
        wd_holes.process({"event": "watchdog_dump", "run_id": "wdh",
                          "profile": "watchdog_inject", "cause": "HANG",
                          "dump_id": 1, "seq": 2, "last": True,
                          "text": "tail only"})
        assert wd_holes.done and wd_holes.failure_class == "incomplete_dump"

        wd_none = Collector(_selftest_args(Path(td) / "wd_none", "wdn", "watchdog_inject"))
        wd_none.process({"event": "heartbeat", "run_id": "wdn",
                         "profile": "watchdog_inject", "build_id": "abc_06.27",
                         "fps": 60})
        # Past stale_timeout + dump_wait with no dump activity at all -> fail.
        wd_none.last_hb = time.monotonic() - 60
        wd_none.tick()
        assert wd_none.done and wd_none.failure_class == "no_dump_after_stale"

        # Healthy heartbeats but the inject never wedges: the collector must
        # self-terminate at duration + window instead of spinning forever.
        wd_alive = Collector(_selftest_args(Path(td) / "wd_alive", "wda",
                                            "watchdog_inject", duration=30))
        _wd_heartbeats(wd_alive, "wda")
        wd_alive.started = time.monotonic() - 100  # past 30 + (5 + 30)
        wd_alive.tick()
        assert wd_alive.done and wd_alive.failure_class == "no_dump_in_window"

        # --- T-a (2026-09-03, Round 3 (3) "walk wedge" fix) collector
        # classification: a stale heartbeat whose LAST delivered heartbeat
        # already showed tel_fail > 0 must FAIL under a DIFFERENT
        # failure_class than a stale heartbeat with tel_fail == 0 -- the
        # whole point being that a human (or an automated retry) reading
        # FAIL.json's failure_class alone can tell "telemetry itself
        # dropped/quiesced" apart from "the console genuinely hung", without
        # opening failure_context by hand. ---
        stale_healthy = Collector(_selftest_args(
            Path(td) / "stale_healthy", "sh", "heartbeat_selftest"))
        stale_healthy.process({
            "event": "heartbeat", "run_id": "sh", "profile": "heartbeat_selftest",
            "build_id": "abc_06.27", "fps": 60, "tel_fail": 0,
        })
        stale_healthy.last_hb = time.monotonic() - 60
        stale_healthy.tick()
        assert stale_healthy.done
        assert stale_healthy.failure_class == "heartbeat_stale_after_start", (
            stale_healthy.failure_class
        )

        stale_dropped = Collector(_selftest_args(
            Path(td) / "stale_dropped", "sd", "heartbeat_selftest"))
        stale_dropped.process({
            "event": "heartbeat", "run_id": "sd", "profile": "heartbeat_selftest",
            "build_id": "abc_06.27", "fps": 60,
            "tel_fail": 2, "tel_last_errno": -6, "tel_quiesced": True,
        })
        stale_dropped.last_hb = time.monotonic() - 60
        stale_dropped.tick()
        assert stale_dropped.done
        assert stale_dropped.failure_class == "heartbeat_stale_telemetry_dropped", (
            stale_dropped.failure_class
        )
        assert "tel_fail=2" in stale_dropped.fail_reason
        assert "tel_last_errno=-6" in stale_dropped.fail_reason
        # write_result()'s PASS/FAIL.json + failure_context() surfacing --
        # a human (or a re-run script) must be able to read these straight
        # off the FAIL.json without re-deriving them from the raw NDJSON.
        stale_dropped.write_result()
        fail_json = json.loads(
            (Path(td) / "stale_dropped" / "FAIL.json").read_text(encoding="utf-8"))
        assert fail_json["failure_class"] == "heartbeat_stale_telemetry_dropped"
        assert fail_json["max_tel_fail"] == 2
        assert fail_json["tel_quiesced_seen"] is True
        assert fail_json["failure_context"]["tel_fail"] == 2
        assert fail_json["failure_context"]["tel_last_errno"] == -6
        assert fail_json["failure_context"]["tel_quiesced"] is True

        # T-c: the running MINIMUM carve-free reading is tracked (not max),
        # and a -1 (no carve / PC) sample must never be treated as "0 KB
        # free, about to spill".
        carve_min = Collector(_selftest_args(
            Path(td) / "carve_min", "cm", "heartbeat_selftest"))
        carve_min.process({"event": "heartbeat", "run_id": "cm",
                           "profile": "heartbeat_selftest", "build_id": "abc_06.27",
                           "mem2_carve_free_kb": -1})
        assert carve_min.min_mem2_carve_free_kb is None
        carve_min.process({"event": "heartbeat", "run_id": "cm",
                           "profile": "heartbeat_selftest", "build_id": "abc_06.27",
                           "mem2_carve_free_kb": 4096})
        carve_min.process({"event": "heartbeat", "run_id": "cm",
                           "profile": "heartbeat_selftest", "build_id": "abc_06.27",
                           "mem2_carve_free_kb": 512})
        carve_min.process({"event": "heartbeat", "run_id": "cm",
                           "profile": "heartbeat_selftest", "build_id": "abc_06.27",
                           "mem2_carve_free_kb": -1})
        assert carve_min.min_mem2_carve_free_kb == 512, carve_min.min_mem2_carve_free_kb

        # Fix round brief finding #3 (2026-09-07): tel_drop/staged_skipped
        # ride only on carve_census rows, invisible to write_result() before
        # this fix. Both are monotonic non-decreasing device-side counters,
        # so the collector's own running max across several carve_census
        # rows (out of order or not) must equal the highest value seen.
        tel_drop = Collector(_selftest_args(
            Path(td) / "tel_drop", "td", "heartbeat_selftest"))
        tel_drop.process({"event": "carve_census", "run_id": "td",
                          "profile": "heartbeat_selftest",
                          "tel_drop": 3, "staged_skipped": 0})
        tel_drop.process({"event": "carve_census", "run_id": "td",
                          "profile": "heartbeat_selftest",
                          "tel_drop": 7, "staged_skipped": 2})
        tel_drop.process({"event": "carve_census", "run_id": "td",
                          "profile": "heartbeat_selftest",
                          "tel_drop": 5, "staged_skipped": 1})
        assert tel_drop.max_tel_drop == 7
        assert tel_drop.max_staged_skipped == 2
        tel_drop.process({"event": "heartbeat", "run_id": "td",
                          "profile": "heartbeat_selftest", "build_id": "abc_06.27",
                          "fps": 60})
        tel_drop.write_result()  # PASS or FAIL either way; both carry the field
        result_json = (tel_drop.out / "PASS.json"
                       if (tel_drop.out / "PASS.json").exists()
                       else tel_drop.out / "FAIL.json")
        result_text = result_json.read_text()
        assert '"max_tel_drop": 7' in result_text
        assert '"max_staged_skipped": 2' in result_text
        # A run that never arms --cw-carve-census must show the "never
        # observed" 0, not a KeyError or a stale value from another run.
        no_carve = Collector(_selftest_args(
            Path(td) / "no_carve", "nc", "heartbeat_selftest"))
        assert no_carve.max_tel_drop == 0 and no_carve.max_staged_skipped == 0

        # --- menus_dwell_creative scenario gate (2026-09-02): the
        # --cw-cworld-mode creative knob's proof. A run that round-trips the
        # whole menu script (map_max reaches min_map_step) but NEVER once
        # reports creative=true must FAIL distinctly -- that is exactly the
        # silent-failure shape this profile exists to catch on purpose
        # instead of the knob quietly not taking. ---
        def _menus_collector(name: str, profile: str) -> Collector:
            return Collector(_selftest_args(Path(td) / name, name, profile,
                                            duration=240))

        menus_creative_never = _menus_collector(
            "menus_creative_never", "menus_dwell_creative")
        for i in range(200):
            menus_creative_never.process({
                "event": "heartbeat", "run_id": "menus_creative_never",
                "profile": "menus_dwell_creative", "build_id": "abc_06.27",
                "world_loaded": True, "map_max": MENUS_DWELL_STEP_CONTRACT, "creative": False,
                "np_entered": 0x7f, "np_skipped": 0, "np_focus": 0x7f,
                "np_flags": 0, "np_rows": 3,
                "fps": 60,
            })
        menus_creative_never.process({
            "event": "profile_done", "run_id": "menus_creative_never",
            "profile": "menus_dwell_creative", "status": "ok",
        })
        assert menus_creative_never.write_result() == 1
        menus_creative_never_fail = json.loads(
            ((Path(td) / "menus_creative_never") / "FAIL.json")
            .read_text(encoding="utf-8"))
        assert menus_creative_never_fail["failure_class"] == "creative_not_observed"

        # Survival's default target (require_creative unset) must NOT be
        # held to this bar -- creative=False throughout is the byte-
        # identical, unchanged menus_dwell shape and must still PASS.
        menus_survival_default = _menus_collector(
            "menus_survival_default", "menus_dwell")
        for i in range(200):
            menus_survival_default.process({
                "event": "heartbeat", "run_id": "menus_survival_default",
                "profile": "menus_dwell", "build_id": "abc_06.27",
                "world_loaded": True, "map_max": MENUS_DWELL_STEP_CONTRACT, "creative": False,
                "np_entered": 0x7f, "np_skipped": 0, "np_focus": 0x7f,
                "np_flags": 0, "np_rows": 3,
                "fps": 60,
            })
        menus_survival_default.process({
            "event": "profile_done", "run_id": "menus_survival_default",
            "profile": "menus_dwell", "status": "ok",
        })
        assert menus_survival_default.write_result() == 0

        # The knob DID take: creative=true on at least one heartbeat (not
        # necessarily every one -- the world is created creative partway
        # through the script, before that no heartbeat can report it yet)
        # must PASS, and the PASS.json must carry creative_seen=true as the
        # visible proof.
        menus_creative_pass = _menus_collector(
            "menus_creative_pass", "menus_dwell_creative")
        for i in range(200):
            menus_creative_pass.process({
                "event": "heartbeat", "run_id": "menus_creative_pass",
                "profile": "menus_dwell_creative", "build_id": "abc_06.27",
                "world_loaded": True, "map_max": MENUS_DWELL_STEP_CONTRACT,
                "creative": i >= 100,  # world created creative partway in
                "np_entered": 0x7f, "np_skipped": 0, "np_focus": 0x7f,
                "np_flags": 0, "np_rows": 3,
                "fps": 60,
            })
        menus_creative_pass.process({
            "event": "profile_done", "run_id": "menus_creative_pass",
            "profile": "menus_dwell_creative", "status": "ok",
        })
        assert menus_creative_pass.write_result() == 0
        menus_creative_pass_result = json.loads(
            ((Path(td) / "menus_creative_pass") / "PASS.json")
            .read_text(encoding="utf-8"))
        assert menus_creative_pass_result["creative_seen"] is True

        # --- the WALKER PROOF gate (push-0921 A11, review A9-3): map_max is a
        # high-water mark the leg reaches whether it ENTERED its seven targets
        # or named all seven as skips, so the step contract additionally grades
        # np_entered / np_skipped / np_flags. Every refusal below is named. ---
        def _menus_tape(name: str, beat: dict, profile: str = "menus_dwell",
                        *, min_map_step: int | None = None,
                        abandonment: dict | None = None):
            c = _menus_collector(name, profile)
            if min_map_step is not None:
                c.profile = dict(c.profile, min_map_step=min_map_step)
            for _ in range(200):
                hb = {"event": "heartbeat", "run_id": name, "profile": profile,
                      "build_id": "abc_06.27", "world_loaded": True,
                      "map_max": MENUS_DWELL_STEP_CONTRACT, "creative": False,
                      "fps": 60}
                hb.update(beat)
                c.process(hb)
            if abandonment is not None:
                c.process(dict(abandonment, event="menu_ap_fail", run_id=name))
            c.process({"event": "profile_done", "run_id": name,
                       "profile": profile, "status": "ok"})
            return c

        def _menus_fail_class(name: str, beat: dict) -> str:
            c = _menus_tape(name, beat)
            assert c.write_result() == 1, name
            return json.loads(((Path(td) / name) / "FAIL.json")
                              .read_text(encoding="utf-8"))["failure_class"]

        CLEAN_PROOF = {"np_entered": 0x7f, "np_skipped": 0, "np_focus": 0x7f,
                       "np_flags": 0, "np_rows": 3}

        # A tape with NO np_* fields at all is REFUSED BY NAME, never read as
        # zero: that is a .dol predating the walker-proof telemetry, and
        # grading it would certify a high-water mark with nothing behind it.
        assert _menus_fail_class("menus_no_proof", {}) == "walker_proof_missing"

        # A terminal high-water mark with only SOME proof keys cannot supply
        # the missing evidence by defaulting it to zero (notably skipped).
        for key in CLEAN_PROOF:
            partial = dict(CLEAN_PROOF)
            del partial[key]
            assert _menus_fail_class("menus_missing_" + key, partial) \
                == "walker_proof_missing"

        assert set(MENUS_DWELL_STEP_SPENT) == {39, 47, 48, 100, 138, 139, 159}
        assert MENUS_DWELL_STEP_CONTRACT == 161
        for ordinal in MENUS_DWELL_STEP_SPENT:
            name = "menus_spent_" + str(ordinal)
            spent_c = _menus_tape(name, CLEAN_PROOF, min_map_step=ordinal)
            assert spent_c.write_result() == 1
            spent_fail = json.loads(
                ((Path(td) / name) / "FAIL.json").read_text(encoding="utf-8"))
            assert spent_fail["failure_class"] == "scenario_not_engaged"
            assert f"min_map_step={ordinal} is SPENT" in spent_fail["reason"]

        # Any np_flags bit is a refusal, each named in the reason text.
        for bit, klass in ((1 << 0, "walker_proof_flags"),
                           (1 << 1, "walker_proof_flags"),
                           (1 << 2, "walker_proof_flags"),
                           (1 << 3, "walker_proof_flags"),
                           (1 << 4, "walker_proof_flags"),
                           (1 << 5, "walker_proof_flags"),
                           (1 << 6, "walker_proof_flags")):
            beat = dict(CLEAN_PROOF, np_flags=bit)
            assert _menus_fail_class(f"menus_flag_{bit}", beat) == klass
        # push-0921 A23 (review A20-6): a bit with NO entry in NP_FLAG_NAMES is
        # still refused -- but the tape has to be able to say WHICH bit, or the
        # reason reads "np_flags=0x80 -- " and the next reader has to guess.
        unnamed = _menus_tape("menus_flag_unnamed",
                              dict(CLEAN_PROOF, np_flags=1 << 7))
        assert unnamed.write_result() == 1
        unnamed_reason = json.loads(
            ((Path(td) / "menus_flag_unnamed") / "FAIL.json")
            .read_text(encoding="utf-8"))["reason"]
        assert "unnamed bit 0x80" in unnamed_reason
        assert not unnamed_reason.rstrip().endswith("--")
        # A named bit and an unnamed one together name both.
        mixed = _menus_tape("menus_flag_mixed",
                            dict(CLEAN_PROOF, np_flags=(1 << 3) | (1 << 7)))
        assert mixed.write_result() == 1
        mixed_reason = json.loads(((Path(td) / "menus_flag_mixed") / "FAIL.json")
                                  .read_text(encoding="utf-8"))["reason"]
        assert "safe answer" in mixed_reason and "unnamed bit 0x80" in mixed_reason

        flagged = _menus_tape("menus_flag_named", dict(CLEAN_PROOF, np_flags=1 << 3))
        assert flagged.write_result() == 1
        assert "did not open on its safe answer" in json.loads(
            ((Path(td) / "menus_flag_named") / "FAIL.json")
            .read_text(encoding="utf-8"))["reason"]

        # push-0921 A14: the PC ENTRY LEVER. The tape is otherwise PERFECT --
        # every target entered, no skips, a plausible row count, the contract
        # ordinal reached -- and it is still refused, by name, because the run
        # started at the leg and nothing before it ran. This is the tape shape
        # a developer's PC-lever run would produce if it ever reached this
        # collector, and the case exists so that it can only ever be refused.
        lever = _menus_tape("menus_entry_lever",
                            dict(CLEAN_PROOF, np_flags=1 << 5))
        assert lever.write_result() == 1
        lever_fail = json.loads(((Path(td) / "menus_entry_lever") / "FAIL.json")
                                .read_text(encoding="utf-8"))
        assert lever_fail["failure_class"] == "walker_proof_flags"
        assert "entry lever" in lever_fail["reason"]
        assert "nothing before the leg ran" in lever_fail["reason"]

        # push-0921 A17 (review A16-3): the DOWN-CONVERGE FALLBACK. Same shape
        # as the lever above -- a tape that is otherwise perfect, refused by
        # name, because the card's screens did not honour the RIGHT entry this
        # profile exists to certify and the walk had to cross the list instead.
        fallback = _menus_tape("menus_strip_fallback",
                               dict(CLEAN_PROOF, np_flags=1 << 6))
        assert fallback.write_result() == 1
        fallback_fail = json.loads(
            ((Path(td) / "menus_strip_fallback") / "FAIL.json")
            .read_text(encoding="utf-8"))
        assert fallback_fail["failure_class"] == "walker_proof_flags"
        assert "DOWN-converge fallback" in fallback_fail["reason"]
        assert "RIGHT entry" in fallback_fail["reason"]

        # --- push-0921 A17 (review A16-4): THE PROOF IS THE UNION OF THE
        # BEATS. Every field the gate reads is monotonic in the walker, so a
        # tape whose later beats are clean while an earlier one carried a flag
        # can only be loss (or duplication) -- and used to PASS, because the
        # collector graded the LAST beat that carried the keys. ---
        def _menus_tape_beats(name: str, beats: list, profile: str = "menus_dwell"):
            """One heartbeat per entry of `beats`; an entry of None carries no
            np_* keys at all (a beat from before the fields existed, or the
            shape a partial tape leaves behind)."""
            c = _menus_collector(name, profile)
            for beat in beats:
                hb = {"event": "heartbeat", "run_id": name, "profile": profile,
                      "build_id": "abc_06.27", "world_loaded": True,
                      "map_max": MENUS_DWELL_STEP_CONTRACT, "creative": False,
                      "fps": 60}
                if beat is not None:
                    hb.update(beat)
                c.process(hb)
            c.process({"event": "profile_done", "run_id": name,
                       "profile": profile, "status": "ok"})
            return c

        # (a) A LATE FAILURE whose last beats are lost. The leg raised
        # leg_failed at beat 100; beats 150+ are an earlier, clean snapshot
        # (what last-writer-wins grades when the failing beats never arrive).
        # The union sees the flag and REFUSES; before A17 this tape PASSED.
        late_failure = ([dict(CLEAN_PROOF)] * 100
                        + [dict(CLEAN_PROOF, np_flags=1 << 4)] * 50
                        + [dict(CLEAN_PROOF)] * 50)
        late = _menus_tape_beats("menus_late_failure", late_failure)
        assert late.write_result() == 1
        late_fail = json.loads(((Path(td) / "menus_late_failure") / "FAIL.json")
                               .read_text(encoding="utf-8"))
        assert late_fail["failure_class"] == "walker_proof_flags"
        assert "the leg abandoned" in late_fail["reason"]

        # ... and the same for the row count, which takes the MAX for exactly
        # this reason: a pre-latch 0 in a later beat must not resurrect the
        # "empty list" justification for a Rename / Delete skip that a beat
        # reporting 650 worlds has already refuted.
        row_gated_skip = dict(
            CLEAN_PROOF,
            np_entered=0x7f & ~(NP_SUB["rename"] | NP_SUB["delete"]),
            np_skipped=NP_SUB["rename"] | NP_SUB["delete"])
        rows_regress = ([dict(row_gated_skip, np_rows=650)] * 100
                        + [dict(row_gated_skip, np_rows=0)] * 100)
        rows_c = _menus_tape_beats("menus_rows_regress", rows_regress)
        assert rows_c.write_result() == 1
        assert json.loads(((Path(td) / "menus_rows_regress") / "FAIL.json")
                          .read_text(encoding="utf-8"))["failure_class"] \
            == "walker_skip_unjustified"

        # (b) An HONEST tape with beats lost in the MIDDLE still PASSES: the
        # union is identical to the last beat under no loss, and a gap changes
        # nothing about what the surviving beats prove.
        with_gap = ([dict(CLEAN_PROOF)] * 90 + [None] * 20
                    + [dict(CLEAN_PROOF)] * 90)
        assert _menus_tape_beats("menus_gap_middle", with_gap).write_result() == 0

        # --- push-0921 A23 (review A20-1): A PASS MUST PROVE THE LEG
        # FINISHED. The evidence is the walker's own TERMINAL STEP ordinal
        # (menu_ap_script_step_required_for_pass() == STEP_NP_LEG_DONE ==
        # MENUS_DWELL_STEP_CONTRACT) carried BY A HEARTBEAT THAT ALSO CARRIES
        # THE WALKER PROOF -- see the verdict's own comment for why loss
        # cannot forge it. ---

        # (d) THE REVIEWER'S CASE, as the DEVICE produces it: the leg is still
        # walking when the tape stops (STEP_NP_FAILED is numbered BELOW the
        # pass ordinal, so a leg that fails late never emits the terminal
        # ordinal at all -- menu_autopilot_script.c's step enum). Every beat
        # is clean, every required target is entered, the row count is
        # plausible; the ONE thing missing is the end of the leg, and that is
        # what the refusal has to say.
        unfinished = [dict(CLEAN_PROOF,
                           map_max=min(MENUS_DWELL_STEP_CONTRACT - 1,
                                       i + 1))
                      for i in range(200)]
        unfinished_c = _menus_tape_beats("menus_leg_unfinished", unfinished)
        assert unfinished_c.write_result() == 1
        unfinished_fail = json.loads(
            ((Path(td) / "menus_leg_unfinished") / "FAIL.json")
            .read_text(encoding="utf-8"))
        assert unfinished_fail["failure_class"] == "walker_leg_unfinished"
        assert "terminal step" in unfinished_fail["reason"]
        assert str(MENUS_DWELL_STEP_CONTRACT) in unfinished_fail["reason"]
        assert "ABANDONED" not in unfinished_fail["reason"]

        # (d2) hw1026c: the walker NAMES its abandonment in one
        # {"event":"menu_ap_fail"} line (map_fail = 1 + the step that gave
        # up), and the refusal must carry it -- that run's tape said only
        # map_step 107 / np_flags 0x10, and the dead step never left the Wii.
        named_c = _menus_collector("menus_leg_named", "menus_dwell")
        for beat in unfinished:
            hb = {"event": "heartbeat", "run_id": "menus_leg_named",
                  "profile": "menus_dwell", "build_id": "abc_06.27",
                  "world_loaded": True, "creative": False, "fps": 60}
            hb.update(beat)
            named_c.process(hb)
        named_c.process({"event": "menu_ap_fail", "run_id": "menus_leg_named",
                         "map_fail": 123, "step": 107, "max": 135, "sub": 3,
                         "stalled": False, "np_failed_step": 122,
                         "np_failed_sub": 3, "walk_failed_step": 0,
                         "cworld_failed_step": 0, "screen": 14})
        named_c.process({"event": "profile_done", "run_id": "menus_leg_named",
                         "profile": "menus_dwell", "status": "ok"})
        assert named_c.write_result() == 1
        named_fail = json.loads(
            ((Path(td) / "menus_leg_named") / "FAIL.json")
            .read_text(encoding="utf-8"))
        assert named_fail["failure_class"] == "walker_leg_unfinished"
        assert "ABANDONED at step 122" in named_fail["reason"]
        assert "np_failed_sub=3" in named_fail["reason"]

        cworld_named = _menus_tape("menus_cworld_named", dict(
            CLEAN_PROOF, map_max=40, map_fail=41),
            abandonment={"map_fail": 41, "cworld_failed_step": 40,
                         "cworld_sub": 2})
        assert cworld_named.write_result() == 1
        cworld_fail = json.loads(
            ((Path(td) / "menus_cworld_named") / "FAIL.json")
            .read_text(encoding="utf-8"))
        assert "cworld_failed_step=40" in cworld_fail["reason"]
        assert "cworld_sub=2" in cworld_fail["reason"]

        # (d3) Sol round-3 review: an abandonment BEFORE the world loads
        # (here the stall guard at step 0) refuses on "world never reported
        # loaded" -- and must still carry the walker's named reason.
        early_c = _menus_collector("menus_early_abandon", "menus_dwell")
        for i in range(200):
            early_c.process({"event": "heartbeat",
                             "run_id": "menus_early_abandon",
                             "profile": "menus_dwell",
                             "build_id": "abc_06.27", "world_loaded": False,
                             "map_step": 0, "map_max": 0, "creative": False,
                             "fps": 60})
        early_c.process({"event": "menu_ap_fail",
                         "run_id": "menus_early_abandon", "map_fail": 1,
                         "step": 0, "max": 0, "sub": 0, "stalled": True,
                         "np_failed_step": 0, "np_failed_sub": -1,
                         "walk_failed_step": 0, "cworld_failed_step": 0,
                         "screen": 0})
        early_c.process({"event": "profile_done",
                         "run_id": "menus_early_abandon",
                         "profile": "menus_dwell", "status": "ok"})
        assert early_c.write_result() == 1
        early_fail = json.loads(
            ((Path(td) / "menus_early_abandon") / "FAIL.json")
            .read_text(encoding="utf-8"))
        assert "world never reported loaded" in early_fail["reason"], early_fail["reason"]
        assert "ABANDONED at step 0" in early_fail["reason"]
        assert "stalled=True" in early_fail["reason"]

        # (e) THE LOSS TOLERANCE. An honest run loses single heartbeats on a
        # loaded host (the recorded line-drop trap), including the very last
        # one. Once ONE proof-carrying beat has reported the terminal step the
        # leg is over -- STEP_NP_LEG_DONE sets np_leg_done and falls into
        # STEP_DONE, which ends the script -- so nothing the verdict reads can
        # change afterwards and trailing loss hides nothing. Measured on the
        # last complete real run (tmp/wii_runs/menus_dwell_reach_s12345_d0_
        # voxel_0912_023352): 163 of its 237 beats carried the terminal
        # ordinal, first at beat 74, so requiring one of them costs an honest
        # run nothing.
        honest = []
        for i in range(240):
            if i in (40, 97, 150, 239):  # single beats lost, incl. the last
                continue
            honest.append(dict(CLEAN_PROOF,
                               map_max=(MENUS_DWELL_STEP_CONTRACT if i >= 230
                                        else min(MENUS_DWELL_STEP_CONTRACT - 1,
                                                 i))))
        assert _menus_tape_beats("menus_single_losses",
                                 honest).write_result() == 0

        # (f) ... and a run that finishes and then keeps beating to the end of
        # the window (the ordinary shape) passes too.
        kept_beating = ([dict(CLEAN_PROOF, map_max=min(
                            MENUS_DWELL_STEP_CONTRACT - 1, i))
                         for i in range(200)]
                        + [dict(CLEAN_PROOF)] * 40)
        assert _menus_tape_beats("menus_kept_beating",
                                 kept_beating).write_result() == 0

        # (g) The terminal ordinal must come from a beat that CARRIES THE
        # PROOF, not from any beat at all: a tape whose terminal beats have no
        # np_* keys (a .dol swapped mid-run -- the case the old
        # walker_proof_stale rule was reaching for) proves nothing about what
        # the walker did at the end.
        swapped = ([dict(CLEAN_PROOF, map_max=MENUS_DWELL_STEP_CONTRACT - 1)]
                   * 150 + [None] * 50)
        swapped_c = _menus_tape_beats("menus_proof_swapped", swapped)
        assert swapped_c.write_result() == 1
        assert json.loads(((Path(td) / "menus_proof_swapped") / "FAIL.json")
                          .read_text(encoding="utf-8"))["failure_class"] \
            == "walker_leg_unfinished"

        # (h) THE PRODUCER'S OVERFLOW SUBSTITUTE (review A20-5). When a
        # heartbeat would not fit its slot, hw_telemetry.c sends a minimal
        # well-formed line instead -- `"overflow":true`, no map_max, no np_*.
        # It used to trip walker_proof_stale, whose reason blamed "telemetry
        # loss, or a .dol change mid-run", i.e. named two causes and not the
        # real one. After a finished leg it is now simply not evidence, and the
        # run passes; where the leg IS short the refusal names it.
        overflow_beat = {"overflow": True}
        after_done = [dict(CLEAN_PROOF)] * 200 + [overflow_beat]
        assert _menus_tape_beats("menus_overflow_after_done",
                                 after_done).write_result() == 0
        short_with_overflow = (
            [dict(CLEAN_PROOF, map_max=MENUS_DWELL_STEP_CONTRACT - 1)] * 200
            + [overflow_beat] * 3)
        short_c = _menus_tape_beats("menus_overflow_short", short_with_overflow)
        assert short_c.write_result() == 1
        short_fail = json.loads(
            ((Path(td) / "menus_overflow_short") / "FAIL.json")
            .read_text(encoding="utf-8"))
        assert short_fail["failure_class"] == "walker_leg_unfinished"
        assert "overflow substitute" in short_fail["reason"]
        assert "3 heartbeat(s)" in short_fail["reason"]

        # (c) A tape that carried the keys and then STOPPED while the walker
        # was ALREADY at its terminal step is no longer refused (it was, as
        # `walker_proof_stale`): the leg had finished, the beats after it are
        # an idle title screen, and refusing them punished exactly the honest
        # trailing loss (e) is about.
        tail_lost = [dict(CLEAN_PROOF)] * 150 + [None] * 50
        assert _menus_tape_beats("menus_tail_lost", tail_lost).write_result() == 0

        # An always-enabled target that was never entered is unreachability,
        # not an empty list -- no row gates Create, Direct Connect, Add or the
        # name editor.
        for missing in ("create", "direct", "add", "name"):
            beat = dict(CLEAN_PROOF,
                        np_entered=0x7f & ~NP_SUB[missing],
                        np_skipped=NP_SUB[missing])
            assert _menus_fail_class(f"menus_missing_{missing}", beat)                 == "walker_target_not_entered"

        # Rename/Delete row-gated: a skip is the SCREEN being correct only if
        # the tape shows the world list was empty (np_rows == 0).
        empty_list = dict(CLEAN_PROOF,
                          np_entered=0x7f & ~(NP_SUB["rename"] | NP_SUB["delete"]),
                          np_skipped=NP_SUB["rename"] | NP_SUB["delete"],
                          np_rows=0)
        assert _menus_tape("menus_empty_list", empty_list).write_result() == 0
        assert _menus_fail_class("menus_skip_unjustified",
                                 dict(empty_list, np_rows=3))             == "walker_skip_unjustified"
        # Neither entered nor skipped is always a refusal.
        assert _menus_fail_class(
            "menus_delete_absent",
            dict(CLEAN_PROOF, np_entered=0x7f & ~NP_SUB["delete"]))             == "walker_target_not_entered"

        # --- push-0921 A23 (review A20-4): np_focus is GRADED. "The ring
        # OBSERVABLY rested on the target" (hw_telemetry.h) was parsed and
        # OR-ed and then read by nothing, so a tape could claim seven entries
        # with the focus mask flat zero and still PASS. The device invariant
        # is np_entered SUBSET np_focus -- np_entered is marked only at
        # STEP_NP_SUB_WAIT_OUT, reachable only through STEP_NP_SUB_ARM, which
        # marks np_focus_reached (menu_autopilot_script.c) -- and it survives
        # both loss (the two masks ride the same line) and the union (a union
        # of subsets is a subset). ---
        assert _menus_fail_class("menus_focus_blind",
                                 dict(CLEAN_PROOF, np_focus=0))             == "walker_focus_not_observed"
        focus_blind = _menus_tape("menus_focus_named", dict(CLEAN_PROOF,
                                                            np_focus=0))
        assert focus_blind.write_result() == 1
        focus_reason = json.loads(
            ((Path(td) / "menus_focus_named") / "FAIL.json")
            .read_text(encoding="utf-8"))["reason"]
        for name in NP_SUB:
            assert name in focus_reason, name
        # One target entered with its focus bit missing is the same refusal,
        # naming just that target.
        assert _menus_fail_class(
            "menus_focus_partial",
            dict(CLEAN_PROOF, np_focus=0x7f & ~NP_SUB["create"]))             == "walker_focus_not_observed"
        # ... and a SKIP is not graded this way: a row-gated Rename/Delete is
        # skipped at the SEEK exhaustion, i.e. with the target never under the
        # ring, so its focus bit is legitimately clear.
        assert _menus_tape_beats(
            "menus_focus_skip_ok",
            [dict(empty_list, np_focus=0x7f & ~(NP_SUB["rename"]
                                                | NP_SUB["delete"]))] * 200
        ).write_result() == 0

        # Edit is accepted entered OR skipped (no production accessor exposes
        # the saved-server count, so there is no count to cross-check it
        # against -- said out loud in the verdict rather than assumed proven).
        edit_skipped = dict(CLEAN_PROOF,
                            np_entered=0x7f & ~NP_SUB["edit"],
                            np_skipped=NP_SUB["edit"])
        assert _menus_tape("menus_edit_skipped", edit_skipped).write_result() == 0

        # A skip CAUSED by the keyboard owning the Close role is refused by
        # its np_flags bit, which names the cause -- not by a bare OSK bit
        # test, which would also refuse a legitimately row-gated Rename. Both
        # halves of that are pinned: the keyboard cause is refused even when
        # the bit pattern looks perfectly legal (an empty world list, Rename
        # skipped, np_rows 0 -- accepted above), and an OSK-backed target with
        # no row gating is refused by NP_REQUIRED_ENTERED.
        assert _menus_fail_class(
            "menus_osk_cause", dict(empty_list, np_flags=1 << 1))             == "walker_proof_flags"
        assert _menus_fail_class(
            "menus_osk_direct",
            dict(CLEAN_PROOF, np_entered=0x7f & ~NP_SUB["direct"],
                 np_skipped=NP_SUB["direct"])) == "walker_target_not_entered"

        # The clean console run PASSES and carries its proof into PASS.json,
        # so the evidence can be re-read rather than trusted.
        clean = _menus_tape("menus_proof_pass", CLEAN_PROOF)
        assert clean.write_result() == 0
        clean_result = json.loads(((Path(td) / "menus_proof_pass") / "PASS.json")
                                  .read_text(encoding="utf-8"))
        assert clean_result["np_proof"]["np_entered"] == 0x7f
        assert clean_result["np_proof"]["np_flags"] == 0
        # ... including the TERMINAL EVIDENCE itself (A23, review A20-1), so
        # a PASS can be quoted as "the leg reached step 159, observed on a
        # proof-carrying beat" rather than taken on trust.
        assert clean_result["np_proof_step_max"] == MENUS_DWELL_STEP_CONTRACT
        assert clean_result["overflow_beats"] == 0

        # THE MIRROR: the bit table above is a COPY of the walker's own
        # (source/game/menu_autopilot_script.h's enum menu_ap_sub). Seven
        # distinct single bits in the documented order is what keeps the copy
        # honest from this side; tests/test_menus_dwell_contract.c holds the
        # C side.
        assert list(NP_SUB) == ["create", "rename", "delete", "direct", "add",
                                "edit", "name"]
        assert [NP_SUB[k] for k in NP_SUB] == [1 << i for i in range(7)]
    return 0


# ---- ui-build-1026/T16a: world_copy_convert evidence ---------------------
# The Wii proof route for "Back up & convert" (source/game/menu_autopilot_
# script.c, menu_ap_copy_*). Two halves, and BOTH must be present:
#   1. the device tape: one `{"event":"wcc_*"}` observer line per marker the
#      walker latched (menu_ap_copy_format_marker is the byte source; tests/
#      test_world_copy_profile.c pins it);
#   2. the card record the rig writes AFTER the run over the stage server
#      (CWFETCH1 of every fixture/backup file, hashed on the PC, and the
#      backup's level.dat) -- see tmp/jobs/ui-build-1026/T16a-RIG.md.
#   The worlds-root LISTING (dot names, .world-copy-N.owner journals and
#   each final folder's relative /.incomplete marker included) is taken ON THE
#   DEVICE as the wcc_card marker (lead decision 2026-10-27), so it is part of
#   the tape, not the card record.
# world_copy_verdict() FAILS CLOSED: a missing marker, a missing card field,
# an unreadable file or an unknown variant is a FAIL that names it, never a
# skip. It is graded offline with --world-copy-verdict; the profile's own
# registration (hw_run.c + the five ValidateSets + PROFILES) is owed by the
# product hunks in T16a-RIG.md, so it is deliberately NOT in PROFILES yet
# (tests/test_profile_registration.py would rightly call that half-registered).
WORLD_COPY_PROFILE = "world_copy_convert"
WORLD_COPY_FIXTURE = "cw_wcc_fixture"
# enum menu_ap_copy_marker order (menu_autopilot_script.h).
WORLD_COPY_MARKERS = ("wcc_begin", "wcc_list_pre", "wcc_ask", "wcc_volume",
                      "wcc_copy", "wcc_result", "wcc_list_post", "wcc_card",
                      "wcc_end")
WORLD_COPY_VARIANTS = ("ok", "low_space", "write_error", "interrupt", "recover")
# menu_ap_copy_required_markers(): INTERRUPT never reaches a result screen.
WORLD_COPY_REQUIRED = {v: WORLD_COPY_MARKERS for v in WORLD_COPY_VARIANTS}
WORLD_COPY_REQUIRED["interrupt"] = tuple(
    m for m in WORLD_COPY_MARKERS if m not in ("wcc_result", "wcc_list_post"))
# Mirrored values (menu_autopilot_script.h enum menu_ap_cb_state / cb_fail,
# world_copy.h enum world_copy_error, server_interface.h svin_convert_outcome).
WCC_CB_COPYING, WCC_CB_ERROR, WCC_CB_DONE = 2, 3, 6
WCC_FAIL_COPY = 3
WCC_WC_IO_ERROR, WCC_WC_LOW_SPACE = 6, 10
WCC_CONVERT_SUCCEEDED = 1
WCC_JOURNAL_PREFIX = ".world-copy-"
WCC_MARKER_SUFFIX = "/.incomplete"
# The in-run collector's device-half artifact (status PENDING_CARD).
WORLD_COPY_DEVICE_FILE = "DEVICE_EVIDENCE.json"


def _nbt_read(data: bytes):
    """Minimal big-endian NBT reader (Beta level.dat): returns (name, value)
    of the root tag. Compounds are dicts {name: (tag_id, value)}, so a
    comparison sees tag TYPES as well as values. Raises ValueError on any
    malformed or trailing byte."""
    pos = 0

    def take(n):
        nonlocal pos
        if pos + n > len(data):
            raise ValueError("truncated NBT")
        out = data[pos:pos + n]
        pos += n
        return out

    def string():
        (n,) = struct.unpack(">H", take(2))
        return take(n).decode("utf-8")

    def payload(tag, depth):
        if depth > 32:
            raise ValueError("NBT nested too deep")
        fmt = {1: ">b", 2: ">h", 3: ">i", 4: ">q", 5: ">f", 6: ">d"}
        if tag in fmt:
            size = struct.calcsize(fmt[tag])
            return struct.unpack(fmt[tag], take(size))[0]
        if tag == 7:
            (n,) = struct.unpack(">i", take(4))
            if n < 0:
                raise ValueError("negative NBT array")
            return bytes(take(n))
        if tag == 8:
            return string()
        if tag == 9:
            elem = take(1)[0]
            (n,) = struct.unpack(">i", take(4))
            if n < 0:
                raise ValueError("negative NBT list")
            return (elem, [payload(elem, depth + 1) for _ in range(n)])
        if tag == 10:
            out = {}
            while True:
                sub = take(1)[0]
                if sub == 0:
                    return out
                name = string()
                if name in out:
                    raise ValueError("duplicate NBT key %r" % name)
                out[name] = (sub, payload(sub, depth + 1))
        if tag == 11:
            (n,) = struct.unpack(">i", take(4))
            if n < 0:
                raise ValueError("negative NBT array")
            return list(struct.unpack(">%di" % n, take(4 * n)))
        raise ValueError("unknown NBT tag %d" % tag)

    root = take(1)[0]
    if root != 10:
        raise ValueError("NBT root is not a compound")
    name = string()
    value = payload(10, 0)
    if pos != len(data):
        raise ValueError("trailing bytes after NBT root")
    return name, value


def _level_dat_tree(path: Path):
    import gzip
    return _nbt_read(gzip.decompress(Path(path).read_bytes()))


def _level_name_problems(source: Path, backup: Path) -> list:
    """backup == source except Data.LevelName, which must be the source's
    name + " (backup)" (world_copy.h: 'The copy's existing LevelName gains
    " (backup)"')."""
    try:
        s_name, s_root = _level_dat_tree(source)
        b_name, b_root = _level_dat_tree(backup)
    except (OSError, ValueError, EOFError) as exc:
        return ["level.dat unreadable: %s" % exc]
    out = []
    if s_name != b_name:
        out.append("level.dat root name differs")
    s_data = s_root.get("Data", (None, None))
    b_data = b_root.get("Data", (None, None))
    if s_data[0] != 10 or b_data[0] != 10:
        return out + ["level.dat has no Data compound"]
    s_level = s_data[1].get("LevelName")
    b_level = b_data[1].get("LevelName")
    if not s_level or s_level[0] != 8 or not b_level or b_level[0] != 8:
        return out + ["level.dat LevelName missing or not a string"]
    if b_level[1] != s_level[1] + " (backup)":
        out.append("backup LevelName %r is not %r" % (b_level[1],
                                                       s_level[1] + " (backup)"))
    s_rest = dict(s_data[1])
    b_rest = dict(b_data[1])
    s_rest.pop("LevelName")
    b_rest.pop("LevelName")
    if s_rest != b_rest:
        keys = sorted(k for k in set(s_rest) | set(b_rest)
                      if s_rest.get(k) != b_rest.get(k))
        out.append("level.dat differs beyond LevelName: %s" % ", ".join(keys))
    if {k: v for k, v in s_root.items() if k != "Data"} != \
            {k: v for k, v in b_root.items() if k != "Data"}:
        out.append("level.dat root differs outside Data")
    return out


def world_copy_events(rows) -> tuple:
    """First wcc_* line of each kind, plus the names seen twice (a second,
    DIFFERENT copy of a marker is a FAIL: the walker latches each once)."""
    seen, conflicts = {}, []
    for row in rows:
        if not isinstance(row, dict):
            continue
        ev = row.get("event")
        if ev not in WORLD_COPY_MARKERS:
            continue
        if ev in seen and seen[ev] != row:
            conflicts.append(ev)
        seen.setdefault(ev, row)
    return seen, conflicts


def _manifest_problems(card: dict, key: str) -> tuple:
    value = card.get(key)
    if not isinstance(value, dict) or not value or not all(
            isinstance(k, str) and isinstance(v, str)
            and re.fullmatch(r"[0-9a-f]{64}", v) for k, v in value.items()):
        return None, ["card %s missing or not {rel_path: sha256}" % key]
    return value, []


def world_copy_verdict(rows, card, variant: str, card_dir: Path = None,
                       device_only: bool = False) -> dict:
    """Grade one world_copy_convert run. `rows` is the parsed device tape,
    `card` the rig's post-run card record (dict) or None. device_only=True
    grades the device half alone (the in-run gate; never a PASS by itself).
    Returns
    {"status": "PASS"|"FAIL", "reasons": [...], "variant": ..., ...}."""
    reasons = []
    if variant not in WORLD_COPY_VARIANTS:
        found = {r.get("event") for r in rows if isinstance(r, dict)}
        return {"status": "FAIL", "variant": variant,
                "reasons": ["unknown variant %r" % variant]
                + ["missing marker %s" % m for m in ("wcc_begin", "wcc_end")
                   if m not in found]}
    ev, conflicts = world_copy_events(rows)
    for name in conflicts:
        reasons.append("marker %s sent twice with different content" % name)
    missing = [m for m in WORLD_COPY_REQUIRED[variant] if m not in ev]
    for name in missing:
        reasons.append("missing marker %s" % name)

    def field(marker, key, kind):
        row = ev.get(marker)
        if row is None:
            return None
        value = row.get(key)
        ok = isinstance(value, bool) if kind is bool else (
            isinstance(value, kind) and not isinstance(value, bool))
        if not ok:
            reasons.append("%s.%s missing or not %s" % (marker, key, kind.__name__))
            return None
        return value

    def expect(cond, why):
        if not cond:
            reasons.append(why)

    fixture = WORLD_COPY_FIXTURE
    if "wcc_begin" in ev:
        expect(field("wcc_begin", "variant", str) == variant,
               "wcc_begin.variant is %r, expected %r"
               % (ev["wcc_begin"].get("variant"), variant))
        fixture = field("wcc_begin", "fixture", str) or fixture
        expect(fixture == WORLD_COPY_FIXTURE,
               "wcc_begin.fixture %r is not the walker-only fixture %r"
               % (fixture, WORLD_COPY_FIXTURE))
    if "wcc_end" in ev:
        expect(field("wcc_end", "ok", bool) is True,
               "walker ended ok=false (step %r, failed_step %r)"
               % (ev["wcc_end"].get("step"), ev["wcc_end"].get("failed_step")))
        expect(field("wcc_end", "variant", str) == variant,
               "wcc_end.variant disagrees with %r" % variant)
    if "wcc_list_pre" in ev:
        row = field("wcc_list_pre", "fixture_row", int)
        expect(row is not None and row >= 0,
               "fixture row absent from the list before the run")
        expect(field("wcc_list_pre", "needs_convert", bool) is True,
               "fixture row did not carry Must be converted")
        expect(field("wcc_list_pre", "partial_visible", bool) is False,
               "a partial world was visible in the list before any press"
               + (" (recovery failed)" if variant == "recover" else ""))
        expect(field("wcc_list_pre", "backup_row", int) == -1,
               "a '(backup)' row already existed before the run")
    if "wcc_ask" in ev:
        expect(field("wcc_ask", "safe_default", bool) is True,
               "the ask did not open with Cancel focused")
    free = req = None
    if "wcc_volume" in ev:
        free = field("wcc_volume", "free_bytes", int)
        cluster = field("wcc_volume", "cluster_bytes", int)
        req = field("wcc_volume", "required_bytes", int)
        expect(free is not None and free > 0, "free_bytes not reported")
        expect(cluster is not None and cluster >= 512
               and cluster & (cluster - 1) == 0,
               "cluster_bytes %r is not a power of two >= 512" % cluster)
        expect(req is not None and req > 0, "required_bytes not reported")
    if "wcc_copy" in ev:
        expect(field("wcc_copy", "monotonic", bool) is True,
               "copy progress went backwards")
        if variant != "low_space":
            expect(field("wcc_copy", "saw_copying", bool) is True,
                   "copy progress was never observed")
        expect(field("wcc_copy", "interrupted", bool) is (variant == "interrupt"),
               "wcc_copy.interrupted does not match the variant")
        if variant == "interrupt":
            pct = field("wcc_copy", "pct_max", int)
            expect(pct is not None and 0 < pct < 100,
                   "interruption was not mid-copy (pct_max %r)" % pct)
        if variant == "write_error":
            # hw1028b: "Cleaning up..." is shown only while cleanup spans a
            # frame. The rig's fault (2nd write = the .incomplete marker)
            # fails with a near-empty tree whose whole cleanup fits inside
            # the failing world_copy_step, so the state is never observable
            # (hw1028a tape: saw_cleaning=false, fail=3 not 4, card clean).
            # What must hold either way is that cleanup RAN: the device's
            # cleanup_done (copy started, no cleanup_error, destination
            # absent on disk) is required; saw_cleaning must be a bool but
            # may be false. The card listing below still has to be clean.
            field("wcc_copy", "saw_cleaning", bool)
            expect(field("wcc_copy", "cleanup_done", bool) is True,
                   "write error: cleanup was not proven to have run "
                   "(wcc_copy.cleanup_done is not true)")
    result = ev.get("wcc_result")
    if result is not None:
        state = field("wcc_result", "state", int)
        fail = field("wcc_result", "fail", int)
        err = field("wcc_result", "copy_error", int)
        outcome = field("wcc_result", "outcome", int)
        published = field("wcc_result", "backup_published", bool)
        if variant in ("ok", "recover"):
            expect(state == WCC_CB_DONE, "result state %r is not Done" % state)
            expect(outcome == WCC_CONVERT_SUCCEEDED,
                   "convert outcome %r is not SUCCEEDED" % outcome)
            expect(published is True, "backup was not published")
            if free is not None and req is not None:
                expect(free >= req, "free_bytes %d < required_bytes %d on a "
                       "successful copy" % (free, req))
        else:
            want = WCC_WC_LOW_SPACE if variant == "low_space" else WCC_WC_IO_ERROR
            expect(state == WCC_CB_ERROR, "result state %r is not the backup "
                   "error screen" % state)
            expect(fail == WCC_FAIL_COPY and err == want,
                   "backup error fail=%r copy_error=%r, expected %d/%d"
                   % (fail, err, WCC_FAIL_COPY, want))
            expect(published is False, "a failed backup reported published")
            expect(outcome == 0, "a failed backup ran the convert")
            if variant == "low_space" and free is not None and req is not None:
                expect(free < req, "low_space run had free_bytes %d >= "
                       "required %d (injection not in force)" % (free, req))
    if "wcc_list_post" in ev:
        expect(field("wcc_list_post", "partial_visible", bool) is False,
               "a partial world is visible in the list after the run")
        row = field("wcc_list_post", "fixture_row", int)
        expect(row is not None and row >= 0,
               "fixture row missing from the list after the run")
        backup_row = field("wcc_list_post", "backup_row", int)
        needs = field("wcc_list_post", "needs_convert", bool)
        if variant in ("ok", "recover"):
            expect(backup_row is not None and backup_row >= 0,
                   "the '(backup)' world is not in the list")
            expect(needs is False, "fixture still tagged after convert")
        else:
            expect(backup_row == -1, "a failed backup left a '(backup)' row")
            expect(needs is True, "fixture lost its tag after a failed backup")

    # ---- the device's own listing of the per-run worlds root (wcc_card) --
    listing = None
    dev = ev.get("wcc_card")
    if dev is not None:
        names = dev.get("names")
        if dev.get("root_ok") is not True:
            reasons.append("wcc_card: the worlds root could not be listed")
        elif dev.get("truncated") is not False:
            reasons.append("wcc_card: listing truncated (too many or too "
                           "long names)")
        elif not isinstance(names, list) or not all(
                isinstance(n, str) for n in names):
            reasons.append("wcc_card.names missing or not a name list")
        else:
            listing = names
    backup_name = fixture + " (backup)"
    if listing is not None:
        expect(fixture in listing, "fixture missing from the post-run card")
        leftovers = sorted(n for n in listing
                           if n.startswith(WCC_JOURNAL_PREFIX)
                           or n.endswith(WCC_MARKER_SUFFIX)
                           or (n.startswith(fixture + " (backup")
                               and n != backup_name))
        if variant == "interrupt":
            marker = backup_name + WCC_MARKER_SUFFIX
            expect(backup_name in listing and marker in listing,
                   "interrupt left no marked final folder on the card "
                   "(the copy finished or never started)")
            expect(any(re.fullmatch(r"\.world-copy-[1-9][0-9]*\.owner", n)
                       for n in listing),
                   "interrupt left no ownership journal on the card")
            expect(all(n == marker or re.fullmatch(
                r"\.world-copy-[1-9][0-9]*\.owner", n) for n in leftovers),
                   "interrupt left unexpected partial copy entries")
        else:
            expect(not leftovers, "partial copy left on the card: %s"
                   % ", ".join(leftovers))
        if variant in ("low_space", "write_error"):
            expect(backup_name not in listing,
                   "a failed backup left %r on the card" % backup_name)
        if variant in ("ok", "recover"):
            expect(backup_name in listing,
                   "%r missing from the post-run card" % backup_name)

    # ---- the card half (taken by the rig over the stage server) ----------
    if device_only:
        pass
    elif not isinstance(card, dict):
        reasons.append("card record missing (wcc_card.json): no source/backup "
                       "hashes, cannot grade")
    else:
        expect(card.get("fixture") == fixture,
               "card fixture %r is not %r" % (card.get("fixture"), fixture))
        expect(card.get("variant") == variant,
               "card variant %r is not %r" % (card.get("variant"), variant))
        source, why = _manifest_problems(card, "source_manifest")
        reasons += why
        post, why = _manifest_problems(card, "post_source_manifest")
        reasons += why
        if source is not None:
            expect("level.dat" in source, "source manifest has no level.dat")
        if variant in ("low_space", "write_error", "interrupt") \
                and source is not None and post is not None:
            expect(post == source, "fixture changed on the card although no "
                   "convert ran (byte-identical required)")
        if variant in ("ok", "recover") and source is not None:
            backup, why = _manifest_problems(card, "backup_manifest")
            reasons += why
            if backup is not None:
                expect(sorted(backup) == sorted(source),
                       "backup file set differs from the source: %s"
                       % ", ".join(sorted(set(backup) ^ set(source))))
                diff = sorted(k for k in source if k in backup
                              and k != "level.dat" and backup[k] != source[k])
                expect(not diff, "backup differs from source in %s"
                       % ", ".join(diff))
                expect(backup.get("level.dat") != source.get("level.dat"),
                       "backup level.dat is byte-identical: LevelName was "
                       "not given ' (backup)'")
                base = Path(card_dir) if card_dir else Path(".")
                src_dat = card.get("source_level_dat")
                bak_dat = card.get("backup_level_dat")
                if not isinstance(src_dat, str) or not isinstance(bak_dat, str):
                    reasons.append("card source_level_dat/backup_level_dat "
                                   "paths missing")
                else:
                    sp, bp = base / src_dat, base / bak_dat
                    for p in (sp, bp):
                        if not p.is_file():
                            reasons.append("level.dat file missing: %s" % p)
                    if sp.is_file() and bp.is_file():
                        got = {"source": hashlib.sha256(sp.read_bytes()).hexdigest(),
                               "backup": hashlib.sha256(bp.read_bytes()).hexdigest()}
                        expect(got["source"] == source.get("level.dat"),
                               "source_level_dat file does not hash to the "
                               "manifest's level.dat")
                        expect(got["backup"] == backup.get("level.dat"),
                               "backup_level_dat file does not hash to the "
                               "backup manifest's level.dat")
                        reasons += _level_name_problems(sp, bp)
    return {
        "status": "FAIL" if reasons else "PASS",
        "profile": WORLD_COPY_PROFILE,
        "variant": variant,
        "reasons": reasons,
        "markers": [m for m in WORLD_COPY_MARKERS if m in ev],
        "free_bytes": free,
        "required_bytes": req,
        "convert_outcome": result.get("outcome") if result else None,
    }


def world_copy_verdict_files(tape: str, card: str, variant: str,
                             out: str = "") -> int:
    """CLI half: read the tape + card record, write wcc_verdict.json.

    With `out` = the run directory this is the ONLY writer of a
    world_copy_convert PASS.json: it needs the in-run collector's
    DEVICE_EVIDENCE.json (status PENDING_CARD, this profile) AND a full PASS
    verdict; otherwise it writes FAIL.json naming every reason. An existing
    PASS.json/FAIL.json is never overwritten."""
    rows, reasons = [], []
    try:
        for line in Path(tape).read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except ValueError:
                continue  # torn line: the markers it held are then missing
    except OSError as exc:
        reasons.append("tape unreadable: %s" % exc)
    card_obj = None
    try:
        card_obj = json.loads(Path(card).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        reasons.append("card record unreadable: %s" % exc)
    verdict = world_copy_verdict(rows, card_obj, variant,
                                 Path(card).parent if card else None)
    device = None
    if out:
        try:
            device = json.loads((Path(out) / WORLD_COPY_DEVICE_FILE).read_text(
                encoding="utf-8"))
        except (OSError, ValueError) as exc:
            reasons.append("device evidence missing: %s (%s)"
                           % (WORLD_COPY_DEVICE_FILE, exc))
        if device is not None and (device.get("profile") != WORLD_COPY_PROFILE
                                   or device.get("status") != "PENDING_CARD"):
            reasons.append("%s is not a world_copy_convert PENDING_CARD record"
                           % WORLD_COPY_DEVICE_FILE)
            device = None
    if reasons:
        verdict["reasons"] = reasons + verdict["reasons"]
        verdict["status"] = "FAIL"
    text = json.dumps(verdict, indent=2, sort_keys=True)
    if out:
        Path(out).mkdir(parents=True, exist_ok=True)
        (Path(out) / "wcc_verdict.json").write_text(text, encoding="utf-8")
        existing = [n for n in ("PASS.json", "FAIL.json")
                    if (Path(out) / n).exists()]
        if existing:
            print("[world-copy] refusing to overwrite %s" % existing[0],
                  flush=True)
            return 1
        result = dict(device or {"profile": WORLD_COPY_PROFILE})
        result["world_copy_verdict"] = verdict
        if verdict["status"] == "PASS":
            result["status"] = "PASS"
            result["reason"] = ("world_copy_convert %s: device + card evidence "
                                "complete" % variant)
            (Path(out) / "PASS.json").write_text(json.dumps(result, indent=2),
                                                 encoding="utf-8")
        else:
            result["status"] = "FAIL"
            result["reason"] = "; ".join(verdict["reasons"])
            result["failure_class"] = "wcc_evidence"
            (Path(out) / "FAIL.json").write_text(json.dumps(result, indent=2),
                                                 encoding="utf-8")
    print(text, flush=True)
    print("[world-copy] %s: %s" % (verdict["status"], "; ".join(
        verdict["reasons"]) or "all evidence present"), flush=True)
    return 0 if verdict["status"] == "PASS" else 1


def world_copy_manifest(world: Path) -> dict:
    """{rel_path: sha256} of a world directory, '/'-separated (the shape the
    stage server's CWSAVES1 hash mode reports)."""
    out = {}
    for p in sorted(Path(world).rglob("*")):
        if p.is_file():
            out[p.relative_to(world).as_posix()] = \
                hashlib.sha256(p.read_bytes()).hexdigest()
    return out


def world_copy_selftest() -> None:
    """Fixture telemetry (tests/fixtures/world_copy_convert/*.ndjson) against
    the REAL Beta import fixture world: each variant PASSES with its tape and
    a matching card, and every evidence removal FAILS by name."""
    import copy
    import gzip
    root = Path(__file__).resolve().parents[1]
    fx = root / "tests" / "fixtures" / "world_copy_convert"
    world = root / "tests" / "fixtures" / "beta_import" / "world"
    source = world_copy_manifest(world)
    backup_dat = fx / "backup_level.dat"
    backup = dict(source, **{"level.dat":
                             hashlib.sha256(backup_dat.read_bytes()).hexdigest()})

    def card(variant):
        c = {"fixture": WORLD_COPY_FIXTURE, "variant": variant,
             "source_manifest": source, "post_source_manifest": dict(source),
             "source_level_dat": os.path.relpath(world / "level.dat", fx),
             "backup_level_dat": "backup_level.dat"}
        if variant in ("ok", "recover"):
            c["backup_manifest"] = dict(backup)
            c["post_source_manifest"] = dict(source, **{"level.dat": "0" * 64})
        return c

    def tape(variant):
        return [json.loads(line) for line in
                (fx / ("%s.ndjson" % variant)).read_text(
                    encoding="utf-8").splitlines() if line.strip()]

    for variant in WORLD_COPY_VARIANTS:
        rows, c = tape(variant), card(variant)
        v = world_copy_verdict(rows, c, variant, fx)
        assert v["status"] == "PASS", (variant, v["reasons"])
        # fail closed: every required marker, removed alone, names itself
        for marker in WORLD_COPY_REQUIRED[variant]:
            cut = [r for r in rows if r.get("event") != marker]
            v = world_copy_verdict(cut, c, variant, fx)
            assert v["status"] == "FAIL", (variant, marker)
            assert "missing marker %s" % marker in v["reasons"], v["reasons"]
        assert world_copy_verdict([], c, variant, fx)["status"] == "FAIL"
        v = world_copy_verdict(rows, None, variant, fx)
        assert v["status"] == "FAIL" and "card record missing" in v["reasons"][0]
        for key in ("source_manifest", "post_source_manifest"):
            c2 = copy.deepcopy(c)
            del c2[key]
            assert world_copy_verdict(rows, c2, variant, fx)["status"] == "FAIL", key
        # the tape of one variant never passes as another
        other = WORLD_COPY_VARIANTS[(WORLD_COPY_VARIANTS.index(variant) + 1)
                                    % len(WORLD_COPY_VARIANTS)]
        assert world_copy_verdict(rows, card(other), other, fx)["status"] == "FAIL"

    def mutate(variant, marker, key, value):
        rows = tape(variant)
        for r in rows:
            if r["event"] == marker:
                r[key] = value
        return world_copy_verdict(rows, card(variant), variant, fx)

    assert mutate("ok", "wcc_ask", "safe_default", False)["status"] == "FAIL"
    assert mutate("ok", "wcc_volume", "free_bytes", 1)["status"] == "FAIL"
    assert mutate("ok", "wcc_volume", "cluster_bytes", 3000)["status"] == "FAIL"
    assert mutate("ok", "wcc_copy", "monotonic", False)["status"] == "FAIL"
    assert mutate("ok", "wcc_result", "outcome", 8)["status"] == "FAIL"
    assert mutate("ok", "wcc_list_post", "partial_visible", True)["status"] == "FAIL"
    assert mutate("ok", "wcc_end", "ok", False)["status"] == "FAIL"
    assert mutate("ok", "wcc_begin", "fixture", "New World")["status"] == "FAIL"
    assert mutate("recover", "wcc_list_pre", "partial_visible", True)["status"] == "FAIL"
    assert mutate("low_space", "wcc_volume", "free_bytes", 10 ** 12)["status"] == "FAIL"
    assert mutate("write_error", "wcc_result", "copy_error", 10)["status"] == "FAIL"
    # hw1028b: a cleanup that finished inside the failing step (never
    # shown) still PASSes on cleanup_done; no proof of cleanup never does.
    assert mutate("write_error", "wcc_copy", "saw_cleaning", False)["status"] == "PASS"
    assert mutate("write_error", "wcc_copy", "saw_cleaning", "no")["status"] == "FAIL"
    assert mutate("write_error", "wcc_copy", "cleanup_done", False)["status"] == "FAIL"
    assert mutate("write_error", "wcc_copy", "cleanup_done", 1)["status"] == "FAIL"
    rows = [{k: v for k, v in r.items() if k != "cleanup_done"}
            for r in tape("write_error")]
    v = world_copy_verdict(rows, card("write_error"), "write_error", fx)
    assert v["status"] == "FAIL" and any(
        "cleanup_done" in why for why in v["reasons"]), v["reasons"]
    assert mutate("interrupt", "wcc_copy", "pct_max", 100)["status"] == "FAIL"
    assert mutate("ok", "wcc_ask", "safe_default", "yes")["status"] == "FAIL"
    # a second, different copy of a marker
    rows = tape("ok") + [{"event": "wcc_ask", "safe_default": False}]
    assert world_copy_verdict(rows, card("ok"), "ok", fx)["status"] == "FAIL"

    def card_mut(variant, fn):
        c = card(variant)
        fn(c)
        return world_copy_verdict(tape(variant), c, variant, fx)

    region = next(k for k in source if k.endswith(".mcr"))
    assert card_mut("ok", lambda c: c["backup_manifest"].__setitem__(
        region, "f" * 64))["status"] == "FAIL"
    assert card_mut("ok", lambda c: c["backup_manifest"].pop(region))["status"] == "FAIL"
    assert card_mut("ok", lambda c: c["backup_manifest"].__setitem__(
        "level.dat", source["level.dat"]))["status"] == "FAIL"
    assert card_mut("ok", lambda c: c.__setitem__(
        "backup_level_dat", "missing.dat"))["status"] == "FAIL"
    assert card_mut("low_space", lambda c: c["post_source_manifest"].__setitem__(
        region, "e" * 64))["status"] == "FAIL"
    # the device listing (wcc_card) is strict in both directions
    fx_only = [WORLD_COPY_FIXTURE]
    assert mutate("ok", "wcc_card", "names", fx_only + [
        WORLD_COPY_FIXTURE + " (backup)",
        WORLD_COPY_FIXTURE + " (backup)/.incomplete"])["status"] == "FAIL"
    assert mutate("ok", "wcc_card", "names", fx_only)["status"] == "FAIL"
    assert mutate("write_error", "wcc_card", "names", fx_only + [
        WORLD_COPY_FIXTURE + " (backup)"])["status"] == "FAIL"
    assert mutate("interrupt", "wcc_card", "names", fx_only)["status"] == "FAIL"
    for names in (
            fx_only + [WORLD_COPY_FIXTURE + " (backup)", ".world-copy-1.owner"],
            fx_only + [WORLD_COPY_FIXTURE + " (backup)",
                       WORLD_COPY_FIXTURE + " (backup)/.incomplete"],
            fx_only + [WORLD_COPY_FIXTURE + " (backup)",
                       ".world-copy-1.tmp", ".world-copy-1.owner"]):
        assert mutate("interrupt", "wcc_card", "names", names)["status"] == "FAIL"
    for variant in ("ok", "recover", "low_space", "write_error"):
        names = next(r["names"] for r in tape(variant)
                     if r["event"] == "wcc_card")
        assert mutate(variant, "wcc_card", "names", names + [
            WORLD_COPY_FIXTURE + " (backup)/.incomplete"])["status"] == "FAIL"

    assert mutate("ok", "wcc_card", "truncated", True)["status"] == "FAIL"
    assert mutate("ok", "wcc_card", "root_ok", False)["status"] == "FAIL"
    # A marked final folder must remain hidden after an interruption.
    # recover must FAIL if it appears, both as a
    # visible row and as a pre-existing backup row -- never be waived.
    assert mutate("recover", "wcc_list_pre", "backup_row", 1)["status"] == "FAIL"
    assert mutate("recover", "wcc_card", "names", fx_only + [
        WORLD_COPY_FIXTURE + " (backup)", WORLD_COPY_FIXTURE + " (backup 2)"]
        )["status"] == "FAIL"

    # level.dat: equal except LevelName, which gains " (backup)"
    src = world / "level.dat"
    assert _level_name_problems(src, backup_dat) == []
    assert _level_name_problems(src, src)  # no " (backup)"
    with tempfile.TemporaryDirectory() as td:
        name, rootv = _nbt_read(gzip.decompress(backup_dat.read_bytes()))
        data = dict(rootv["Data"][1])
        data["SpawnY"] = (3, data["SpawnY"][1] + 1)
        bad = Path(td) / "bad.dat"
        bad.write_bytes(gzip.compress(_nbt_write(name, {"Data": (10, data)})))
        probs = _level_name_problems(src, bad)
        assert probs and "SpawnY" in probs[-1], probs
        # CLI: missing card -> FAIL exit 1, verdict file written
        rc = world_copy_verdict_files(str(fx / "ok.ndjson"),
                                      str(Path(td) / "absent.json"), "ok", td)
        assert rc == 1
        assert json.loads((Path(td) / "wcc_verdict.json").read_text())["status"] == "FAIL"
        good = Path(td) / "card.json"
        c = card("ok")
        c["source_level_dat"] = str(world / "level.dat")
        c["backup_level_dat"] = str(backup_dat)
        good.write_text(json.dumps(c), encoding="utf-8")
        # grade-only (no run dir): prints the verdict, writes no PASS.json
        assert world_copy_verdict_files(str(fx / "ok.ndjson"), str(good),
                                        "ok", "") == 0
        assert world_copy_verdict_files(str(Path(td) / "no.ndjson"), str(good),
                                        "ok", "") == 1
        assert not (Path(td) / "PASS.json").exists()

    # Review round 1 (Sol B2): the in-run collector never writes PASS.json for
    # this profile, and only the full verdict does.
    def run_collector(name, rows):
        d = Path(tempfile.mkdtemp())
        c = Collector(_selftest_args(d / name, name, WORLD_COPY_PROFILE,
                                     duration=300))
        for i in range(6):
            c.process({"event": "heartbeat", "run_id": name,
                       "profile": WORLD_COPY_PROFILE, "build_id": "abc_06.27",
                       "fps": 60})
        for r in rows:
            c.process(dict(r, run_id=name))
        c.process({"event": "profile_done", "run_id": name,
                   "profile": WORLD_COPY_PROFILE, "status": "ok"})
        rc = c.write_result()
        return rc, d / name

    for variant in WORLD_COPY_VARIANTS:
        rc, d = run_collector("wcc_" + variant, tape(variant))
        assert rc == 1 and not (d / "PASS.json").exists(), variant
        dev = json.loads((d / WORLD_COPY_DEVICE_FILE).read_text())
        assert dev["status"] == "PENDING_CARD", (variant, dev.get("reason"))
    # every device piece is required in-run: each marker removed alone -> FAIL
    for marker in WORLD_COPY_REQUIRED["ok"]:
        rc, d = run_collector("wcc_cut", [r for r in tape("ok")
                                          if r.get("event") != marker])
        assert rc == 1 and not (d / "PASS.json").exists()
        assert not (d / WORLD_COPY_DEVICE_FILE).exists()
        fail = json.loads((d / "FAIL.json").read_text())
        assert "missing marker %s" % marker in fail["reason"], fail["reason"]
    # recover with the marked final folder listed is a named in-run FAIL,
    # never PENDING_CARD
    bad = tape("recover")
    for r in bad:
        if r["event"] == "wcc_list_pre":
            r.update(partial_visible=True, backup_row=1)
    rc, d = run_collector("wcc_recover_bad", bad)
    assert rc == 1 and not (d / "PASS.json").exists()
    fail = json.loads((d / "FAIL.json").read_text())
    assert "partial world was visible" in fail["reason"], fail["reason"]
    # a route REFUSED for a non-isolated worlds root (Sol B1): its three
    # markers are a named in-run FAIL, never PENDING_CARD or PASS
    refused = [{"event": "wcc_begin", "variant": "ok",
                "fixture": WORLD_COPY_FIXTURE},
               {"event": "wcc_card", "root_ok": False, "truncated": False,
                "names": []},
               {"event": "wcc_end", "variant": "ok", "ok": False,
                "step": 15, "failed_step": 15}]
    rc, d = run_collector("wcc_refused", refused)
    assert rc == 1 and not (d / "PASS.json").exists()
    assert not (d / WORLD_COPY_DEVICE_FILE).exists()
    fail = json.loads((d / "FAIL.json").read_text())
    assert "walker ended ok=false (step 15, failed_step 15)" in fail["reason"]
    assert "the worlds root could not be listed" in fail["reason"]
    # the full verdict promotes the device record to PASS.json ...
    rc, d = run_collector("wcc_full", tape("ok"))
    good_card = d / "wcc_card.json"
    c = card("ok")
    c["source_level_dat"] = str(world / "level.dat")
    c["backup_level_dat"] = str(backup_dat)
    good_card.write_text(json.dumps(c), encoding="utf-8")
    assert world_copy_verdict_files(str(fx / "ok.ndjson"), str(good_card),
                                    "ok", str(d)) == 0
    passed = json.loads((d / "PASS.json").read_text())
    assert passed["status"] == "PASS" and passed["run_id"] == "wcc_full"
    assert passed["world_copy_verdict"]["status"] == "PASS"
    # ... never twice, and never without the device record or the card
    assert world_copy_verdict_files(str(fx / "ok.ndjson"), str(good_card),
                                    "ok", str(d)) == 1
    rc, d = run_collector("wcc_nocard", tape("ok"))
    assert world_copy_verdict_files(str(fx / "ok.ndjson"),
                                    str(d / "absent.json"), "ok", str(d)) == 1
    assert not (d / "PASS.json").exists() and (d / "FAIL.json").exists()
    d = Path(tempfile.mkdtemp())
    assert world_copy_verdict_files(str(fx / "ok.ndjson"), str(good_card),
                                    "ok", str(d)) == 1
    assert "device evidence missing" in json.loads(
        (d / "FAIL.json").read_text())["reason"]


def _nbt_write(name: str, compound: dict) -> bytes:
    """Inverse of _nbt_read for the tag types a level.dat carries (selftest
    and the backup_level.dat fixture generator)."""
    def s(text):
        raw = text.encode("utf-8")
        return struct.pack(">H", len(raw)) + raw

    def payload(tag, value):
        fmt = {1: ">b", 2: ">h", 3: ">i", 4: ">q", 5: ">f", 6: ">d"}
        if tag in fmt:
            return struct.pack(fmt[tag], value)
        if tag == 7:
            return struct.pack(">i", len(value)) + value
        if tag == 8:
            return s(value)
        if tag == 9:
            elem, items = value
            return bytes([elem]) + struct.pack(">i", len(items)) + b"".join(
                payload(elem, v) for v in items)
        if tag == 10:
            return b"".join(bytes([t]) + s(k) + payload(t, v)
                            for k, (t, v) in value.items()) + b"\x00"
        if tag == 11:
            return struct.pack(">i", len(value)) + struct.pack(">%di" % len(value), *value)
        raise ValueError(tag)

    return b"\x0a" + s(name) + payload(10, compound)



def build_arg_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=39017)
    ap.add_argument("--out", required=False, default="tmp/wii_runs/manual")
    ap.add_argument("--run-id", default="")
    # default None -> far_idle_2m after parsing (parse_collector_args), so
    # the residency tape CLI can tell an explicit --profile from the default.
    ap.add_argument("--profile", default=None, choices=sorted(PROFILES))
    ap.add_argument("--expected-build", default="")
    ap.add_argument("--cw-vehicle-kind", default="", choices=[""] + list(VS_KINDS))
    ap.add_argument("--vehicle-transfer-mode", default="", choices=["", "contact", "fixture_request"])
    ap.add_argument("--vehicle-manifest", default="")
    ap.add_argument("--vehicle-check-manifest", action="store_true")
    ap.add_argument("--vehicle-dol", default="")
    ap.add_argument("--vehicle-validate-pass", nargs="+", default=[])
    ap.add_argument("--vehicle-aggregate", action="store_true")
    ap.add_argument("--vehicle-replay-out", default="",
                    help="with --replay: retain a regraded vehicle verdict/tape in a new directory outside the original run")
    ap.add_argument("--vehicle-pc-replay", action="store_true",
                    help="offline vehicle replay only: waive the heartbeat floor only when no beats exist; never emit Wii PASS evidence")
    ap.add_argument("--tcp-connect-host", default="")
    ap.add_argument("--tcp-connect-port", type=int, default=39017)
    ap.add_argument("--connect-timeout", type=int, default=90)
    ap.add_argument("--duration", type=int, default=0)
    ap.add_argument("--first-heartbeat-timeout", type=int, default=45)
    ap.add_argument("--stale-timeout", type=int, default=5)
    # expect_dump profiles: how long past the stale window to wait for the
    # watchdog dump to finish streaming before declaring the run dumpless.
    ap.add_argument("--dump-wait", type=int, default=45)
    ap.add_argument("--max-server-us", type=int, default=50000)
    ap.add_argument("--release-gate", action="store_true",
                    help="require cumulative gameplay frame and memory evidence; not visual/splitscreen certification")
    ap.add_argument("--require-tnt-evidence", action="store_true",
                    help="require a run-bound terminal TNT chain PASS on the latest heartbeat")
    ap.add_argument("--require-wii-budget", action="store_true",
                    help="require compiled Wii physical-budget evidence for split_dims_6m")
    ap.add_argument("--require-residency", action="store_true",
                    help="residency-1005/2: FAIL unless the per-actor rza residency gate PASSes")
    ap.add_argument("--residency-allow-induced", action="store_true",
                    help="residency gate: accept ONE failure per matching rzi induced mark "
                         "(actor/world/body/attempt), only with notice, <=200-tick "
                         "termination, fault-phase evidence and a successful fresh retry")
    ap.add_argument("--residency-expect-fault", default="", type=_residency_fault_arg,
                    help="require an exercised host@seconds|guest@seconds fault and fresh retry; "
                         "implies --require-residency and --residency-allow-induced")
    ap.add_argument("--residency-require-moving", default="",
                    type=_residency_legs_arg,
                    help="residency gate: required moving legs, comma list of "
                         "host|guest[:slot] or none (default: derived from --profile)")
    ap.add_argument("--residency-gate-tape", default="",
                    help="evaluate an ndjson heartbeat tape with the residency gate and exit")
    ap.add_argument("--residency-gate-negatives", action="store_true",
                    help="with --residency-gate-tape: also require every single-fault mutation to FAIL")
    ap.add_argument("--require-g9-evidence", action="store_true",
                    help="require the v2 G9 light-queue array on every heartbeat (no older artifacts)")
    ap.add_argument("--require-service-exposure", action="store_true",
                    help="guarded stress diagnostic: >=300 admitted/alive seconds, >=8 ordinary 600-tick checkpoints, complete observer/input/active-music burst coverage; no hardware acceptance")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--world-copy-selftest", action="store_true",
                    help="test world-copy grading against the private original-Beta save fixture")
    # ui-build-1026/T16a: offline world_copy_convert verdict (device tape +
    # the rig's post-run card record); fails closed on any missing evidence.
    ap.add_argument("--world-copy-verdict", default="", metavar="TAPE",
                    help="grade a world_copy_convert telemetry.ndjson tape")
    ap.add_argument("--world-copy-card", default="", metavar="CARD_JSON",
                    help="with --world-copy-verdict: the rig's wcc_card.json")
    ap.add_argument("--world-copy-variant", default="",
                    choices=[""] + list(WORLD_COPY_VARIANTS),
                    help="with --world-copy-verdict: the variant the run armed")
    ap.add_argument("--gx-compare", nargs=2, metavar=("RUN_A", "RUN_B"), default=None,
                    help="colcap-1006 R5 item 2b: ordered GX equivalence of two "
                         "pool_selftest_3m PASS run dirs (one per placement)")
    ap.add_argument("--gx-compare-out", default="",
                    help="with --gx-compare: write the verdict JSON here")
    # Offline replay of an ALREADY RECORDED run directory (see replay_run_dir):
    # verdict + reasons out, nothing in the directory touched. Extends THIS
    # entry point on purpose -- a second script would be a second set of rules.
    ap.add_argument("--replay", default="",
                    help="re-derive the verdict of a recorded run directory "
                         "offline (no console, no server); the directory is "
                         "read-only")
    ap.add_argument("--profile-override", default="", choices=[""] + sorted(PROFILES),
                    help="with --replay: grade the tape as this profile "
                         "instead of the one its PASS/FAIL artifact recorded")
    return ap


def _residency_legs_arg(text: str) -> str:
    try:
        parse_residency_legs(text) if text else None
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc))
    return text


def _residency_fault_arg(text: str) -> str:
    try:
        parse_residency_fault(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc))
    return text


def parse_collector_args(argv=None) -> argparse.Namespace:
    args = build_arg_parser().parse_args(argv)
    args.profile_explicit = args.profile is not None
    if args.profile is None:
        args.profile = "far_idle_2m"
    if args.residency_expect_fault:
        args.require_residency = args.residency_allow_induced = True
    return args


def main() -> int:
    args = parse_collector_args()
    ap = build_arg_parser()
    if args.vehicle_pc_replay:
        problem = vehicle_pc_replay_args_error(args, args.profile_override or args.profile)
        if problem:
            ap.error("--vehicle-pc-replay requires --replay; " + problem)
    if args.residency_gate_tape:
        return residency_gate_tape(args.residency_gate_tape, args.residency_gate_negatives,
                                   args.profile, args.residency_require_moving,
                                   args.residency_allow_induced, args.profile_explicit,
                                   args.residency_expect_fault)
    if args.world_copy_verdict:
        if not args.world_copy_card or not args.world_copy_variant:
            ap.error("--world-copy-verdict requires --world-copy-card and "
                     "--world-copy-variant")
        return world_copy_verdict_files(args.world_copy_verdict,
                                        args.world_copy_card,
                                        args.world_copy_variant,
                                        args.out if args.out != "tmp/wii_runs/manual" else "")
    if args.selftest:
        return selftest()
    if args.world_copy_selftest:
        world_copy_selftest()
        return 0
    if args.vehicle_check_manifest or args.vehicle_validate_pass:
        try:
            if args.vehicle_check_manifest:
                vehicle_manifest(args.vehicle_manifest, args.cw_vehicle_kind, args.expected_build, args.vehicle_dol)
                print("PASS frozen vehicle manifest/release preflight")
            else:
                packet = vehicle_validate_packet(args.vehicle_validate_pass, args.vehicle_manifest,
                    "" if args.vehicle_aggregate else args.cw_vehicle_kind, args.expected_build)
                print(json.dumps(packet, sort_keys=True))
            return 0
        except (ValueError, OSError, KeyError, TypeError) as exc:
            print("FAIL vehicle packet: " + str(exc))
            return 1
    if args.gx_compare:
        verdict = gx_compare(*args.gx_compare)
        text = json.dumps(verdict, indent=2)
        if args.gx_compare_out:
            Path(args.gx_compare_out).write_text(text, encoding="utf-8")
        print(text, flush=True)
        print("[gx-compare] %s: %s" % (verdict["status"], verdict["reason"]), flush=True)
        return 0 if verdict["status"] == "PASS" else 1
    if args.replay:
        return replay_run_dir(args)
    if (args.profile == VEHICLE_SMOKE_PROFILE) != bool(args.cw_vehicle_kind):
        ap.error("--cw-vehicle-kind boat|pig|cart is required only for host_vehicle_smoke_8m")
    if args.release_gate and args.profile not in RELEASE_FRAME_PROFILES:
        ap.error("--release-gate requires keepup_walk_6m or far_walk_6m; "
                 "other profiles collect stability evidence without this flag")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
