#!/usr/bin/env python3
"""Header hygiene gate (docs/build_architecture.md step 7).

Two checks over every first-party header under source/:

  1. SELF-CONTAINED: the header compiles as the sole content of a TU
     (gcc -fsyntax-only, the PC profile defines). A header that only
     compiles when its includer happens to have included something else
     first is a hidden coupling and blocks any interface split.
  2. CYCLES: strongly-connected components of the quoted-include graph.
     A cycle means "these headers are one interface pretending to be
     several" -- exactly what step 7 exists to unwind.

Existing debt is recorded in tools/header_hygiene_baseline.txt; the gate
fails only on NEW debt (a non-baselined failing header, or an SCC line not
present in the baseline -- note a GROWN cycle produces a new line and so
fails). Shrinking debt prints as an improvement with a hint to refresh.

Usage:
  python tools/check_header_hygiene.py            # gate against baseline
  python tools/check_header_hygiene.py --report   # full human report
  python tools/check_header_hygiene.py --baseline # rewrite the baseline
"""
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tools", "header_hygiene_baseline.txt")
DEFINES = ["-DPLATFORM_PC", "-DCGLM_ALL_UNALIGNED"]
# Wii-only headers never compile under the PC profile; they are gated by the
# Wii Makefile build instead. Vendored libraries under source/ (cglm, cNBT,
# lodepng, parson) are upstream code, not ours to lint -- same status as
# third_party/.
EXCLUDE_PREFIXES = (
    "source/platform/wii/",
    "source/cglm/",
    "source/cNBT/",
    "source/lodepng/",
    "source/parson/",
)

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)

# Step 8 stratum gate: each named stratum's .c/.h files may quote-include
# only their own stratum plus the FROZEN external edge set measured when the
# stratum was declared. A new external edge fails configure/CI -- widen the
# list only with a justification comment (that's a dependency-direction
# decision, not a convenience).
# The sim stratum (step 8b) spans two directories; block/ and item/ grant
# each other wholesale (trailing-"/" entries are whole-stratum grants).
# Its external edge set was harvested by the checker itself 2026-08-27 --
# 38 headers, honestly including the UP-stratum render/server edges Beta's
# handler idiom bakes into block/item code. Those are counted, frozen
# DIRECTION DEBT for future cleanup, not an endorsement.
_SIM_EDGES = {
    "source/cglm/cglm.h",
    "source/cglm/types.h",
    "source/daytime.h",
    "source/entity/combat.h",
    "source/entity/entity.h",
    "source/entity/entity_falling_sand.h",
    "source/entity/entity_minecart.h",
    "source/entity/entity_monster.h",
    "source/entity/entity_painting.h",
    "source/entity/entity_primed_tnt.h",
    "source/game/game_state.h",
    "source/graphics/gfx_settings.h",
    "source/graphics/render_block.h",
    "source/graphics/render_item.h",
    "source/graphics/texture_atlas.h",
    "source/network/client_interface.h",
    "source/network/inventory_logic.h",
    "source/network/map_data.h",
    "source/network/map_data_archive.h",
    "source/network/map_idcounts.h",
    "source/network/server_dispenser.h",
    "source/network/server_furnace.h",
    "source/network/server_interface.h",
    "source/network/server_jukebox.h",
    "source/network/server_local.h",
    "source/network/server_local_state.h",
    "source/network/server_noteblock.h",
    "source/network/server_spawner.h",
    "source/network/server_weather.h",
    "source/network/server_world.h",
    "source/particle.h",
    "source/platform/audio.h",
    "source/platform/displaylist.h",
    "source/platform/thread.h",
    "source/util.h",
    "source/world.h",
    "source/worldgen/jmath.h",
    "source/worldgen/tree_runtime.h",
}

# The world stratum (step 8c) is a FILE set: its TUs live loose at
# source/ root. Members and allowed sets share one rule: an entry
# ending in "/" matches by prefix, otherwise exactly.
_WORLD_MEMBERS = {
    "source/column_view.c", "source/column_view.h", # shared presentation storage
    "source/chunk.c", "source/chunk.h", "source/chunk_defs.h",
    "source/chunk_lod_geomip.h", "source/chunk_lod_policy.h",
    "source/chunk_mesher.c", "source/chunk_mesher.h",
    "source/light_brightness.c", "source/light_brightness.h",
    "source/lighting.c", "source/lighting.h",
    "source/stack.c", "source/stack.h",
    "source/world.c", "source/world.h", "source/world_redraw.c",
    "source/world_visibility.c", "source/world_visibility.h",
}

_ENTITY_EDGES = {
        "source/block/block_slipperiness.h",  # 9139f9a7/5ec2d8b1/cbeb3751: Beta ground friction helper used by entity movement.
        "source/block/aabb.h",
        "source/block/block_pressure_plate.h",  # plate arrival trigger (2026-09-02 lane/pressure-plates)
        "source/block/block_rail.h",
        "source/block/block_sand.h",
        "source/block/blocks.h",
        "source/block/blocks_data.h",
        "source/block/redstone_piston_server.h",
        "source/cglm/cglm.h",
        # 2026-09-02 harvest: entity_monster.c's undead sun-burn outer gate
        # (711c1d6) calls the byte-pinned daytime_is_daytime_ticks. daytime.h
        # is a leaf header every other stratum (sim/world/server/client)
        # already grants; same class as light_brightness.h below.
        "source/daytime.h",
        "source/game/camera.h",
        "source/game/client_session.h",  # 3f330096: remote player resolves its owner session.
        "source/game/difficulty.h",  # 8d624d72: arrow damage reads Beta difficulty.
        "source/game/game_state.h",
        "source/graphics/gfx_settings.h",
        "source/graphics/render_block.h",
        "source/graphics/render_entity.h",
        "source/graphics/render_entity_anchor.h",  # c224f3e8: monster/remote player render anchors.
        # 2026-09-24 (remote-mob-fire merge d400a930, b77724f9): entities_client_render
        # draws Beta Render.renderEntityOnFire after e->render -- the same
        # client-render edge class as render_entity.h / render_item.h above.
        "source/graphics/render_entity_fire.h",
        "source/graphics/render_item.h",
        "source/item/inventory.h",
        "source/item/item_types.h",
        "source/item/items.h",
        "source/item/tool.h",
        "source/light_brightness.h",
        "source/network/client_interface.h",
        "source/network/piston_moving_transport.h",
        "source/network/server_interface.h",
        "source/network/server_local.h",
        "source/network/server_local_state.h",
        "source/network/server_weather.h",
        "source/network/server_world.h",
        "source/particle.h",
        "source/platform/audio.h",
        "source/platform/displaylist.h",
        "source/platform/gfx.h",
        "source/platform/input.h",
        # Owned client walking reads immutable per-player button frames.
        "source/platform/input_owner.h",
        "source/platform/texture.h",
        "source/platform/thread.h",
        "source/platform/watchdog.h",
        "source/util.h",
        "source/world.h",
        "source/worldgen/beta_math.h",
        "source/worldgen/biome.h",
        "source/worldgen/java_random.h",
    }

_SERVER_EDGES = {
        "source/carve_observer.h",  # 56efceeb: server world inline allocation tags need carve constants.
        "source/game/difficulty.h",  # 8d624d72/111d0bc4: local/weather/world difficulty rules.
        "source/game/menu_dwell_demo.h",  # 8c6a7be3: local server drives menu dwell demo.
        "source/game/respawn_clearance.h",  # d1e09a89: local server validates respawn clearance.
        "source/game/gui/chat_hud.h",  # f6fed050: client interface updates chat HUD.
        "source/game/gui/screen_load_world.h",  # d75443fb: remote server drives load screen.
        "source/platform/hw_telemetry.h",  # e11065a9: local server reports hardware telemetry.
        "source/platform/input_owner.h",  # b9c87d16: client interface routes owner input.
        "source/block/aabb.h",  # 27ed0e7b: server world collision boxes.
        "source/block/redstone_server.h",  # 4e397379: server world applies redstone rules.
        "source/entity/actor_ident.h",  # 35a306e4/27ed0e7b: actor identity is passed by value.
        "source/cNBT/buffer.h",  # fd34ee32: server world embeds a persistence buffer.
        "source/item/inventory_click.h",  # ece063c7: inventory transaction click semantics.
        "source/item/item_nbt.h",  # 50cd2117: archive and tile import preserve item NBT.
        "source/tps_observer.h", # optional read-only CPU observations, no engine includes
        "source/column_view.h", # published backing write barriers; no render dependency
        # Slice 1 passive column census callbacks; no ownership transfer.
        "source/column_observer.h",
        "source/block/block_pressure_plate.h",  # plate arrival trigger from server_world.c (2026-09-02)
        "source/block/blocks.h",
        "source/block/blocks_data.h",
        "source/block/dispenser_logic.h",
        "source/block/fluid_block.h",
        "source/block/fluid_spread.h",
        "source/block/furnace_logic.h",
        "source/block/furnace_recipes.h",
        "source/block/jukebox_logic.h",
        "source/block/noteblock_logic.h",
        "source/block/piston_moving.h",
        "source/block/redstone_logic.h",
        "source/block/redstone_piston_server.h",
        "source/cNBT/list.h",
        "source/cNBT/nbt.h",
        "source/cglm/cglm.h",
        "source/chunk.h",
        "source/daytime.h",
        "source/entity/armor.h",
        "source/entity/combat.h",
        "source/entity/entity.h",
        "source/entity/entity_boat.h",
        "source/entity/entity_falling_sand.h",
        "source/entity/entity_fish.h",
        "source/entity/entity_minecart.h",
        "source/entity/entity_monster.h",
        "source/entity/entity_painting.h",
        "source/entity/entity_primed_tnt.h",
        "source/entity/entity_wolf.h",
        "source/entity/los.h",
        "source/entity/mob_motion_probe.h",  # 2026-09-28 G8: server hardware sampler feeds pure entity motion checks; no world/render dependency.
        "source/entity/monsters_object.h",
        "source/entity/pathfinding.h",
        "source/game/game_state.h",
        "source/game/sign_text_store.h",
        "source/item/inventory.h",
        "source/item/item_types.h",
        "source/item/items.h",
        "source/item/items/item_map.h",
        "source/item/recipe.h",
        "source/item/window_container.h",
        "source/lighting.h",
        "source/particle.h",
        "source/platform/audio.h",
        # 2026-09-01 harvest: guard-canary instrumentation
        # (client_interface.c, server_remote_internal.h) -- normal
        # server->platform direction, same status as watchdog.h.
        "source/platform/cw_canary.h",
        # 2026-09-01 harvest: S3 music/jukebox voice (2ceb4ec) -- the client
        # interface drives record playback; same direction as audio.h.
        "source/platform/audio_music.h",
        "source/platform/input.h",
        "source/platform/mem2.h",
        "source/platform/net_socket.h",
        "source/platform/thread.h",
        "source/platform/time.h",
        "source/platform/watchdog.h",
        "source/profile.h",
        "source/util.h",
        "source/world.h",
        "source/worldgen/decorate.h",
        "source/worldgen/java_random.h",
        "source/worldgen/jmath.h",
        "source/worldgen/populate.h",
        "source/worldgen/worldgen.h",
    }

_CLIENT_EDGES = {
        "source/block/blocks.h",
        "source/block/blocks_data.h",
        "source/block/piston_moving.h",
        "source/block/piston_render.h",
        "source/block/redstone_power.h",
        "source/cglm/cglm.h",
        "source/cglm/types.h",
        "source/chunk.h",
        "source/chunk_mesher.h",
        "source/config.h",
        "source/daytime.h",
        "source/entity/armor.h",
        "source/entity/combat.h",
        "source/entity/entity.h",
        "source/entity/monsters_object.h",
        "source/entity/pathfinding.h",
        "source/item/inventory.h",
        "source/item/item_types.h",
        "source/item/items.h",
        "source/item/window_container.h",
        "source/light_brightness.h",
        "source/lighting.h",
        "source/lodepng/lodepng.h",
        "source/network/level_archive.h",
        "source/network/mp_codec.h",
        "source/network/mp_session.h",
        "source/network/piston_moving_transport.h",
        "source/network/server_interface.h",
        "source/network/server_local.h",
        "source/network/server_local_state.h",
        "source/network/server_world.h",
        "source/network/tile_entities_nbt.h",
        "source/particle.h",
        "source/profile.h",
        "source/stack.h",
        "source/texturepacks.h",
        "source/util.h",
        "source/world.h",
        "source/worldgen/beta_math.h",
        "source/worldgen/biome.h",
        "source/worldgen/java_random.h",
        "source/worldgen/worldgen.h",
    }

STRATA = {
    "source/block/": _SIM_EDGES | {
        "source/item/",
        "source/network/server_local_guest.h",  # eb0ccbda: piston server routes the bound guest actor.
    },
    "source/item/": _SIM_EDGES | {
        "source/block/",
        "source/cNBT/nbt.h",  # 50cd2117: item NBT API names nbt_node typedef.
        "source/platform/time.h",  # 20febec8: container sort session embeds ptime_t deadlines.
    },
    # world stratum edges harvested by the checker 2026-08-27 (step 8c):
    # sim value types + a handful of platform/render leaves the mesher and
    # client-lighting genuinely touch. render_block.h + game_state.h are
    # counted direction debt, same status as the sim stratum's.
    frozenset(_WORLD_MEMBERS): {
        "source/settle_observer.h", # passive stage callbacks; leaf types only, no engine dependency
        "source/tps_observer.h", # optional mesher-thread CPU registration
        # Slice 1 passive column/mesh lifetime observation.
        "source/column_observer.h",
        "source/arena.h",
        "source/block/aabb.h",
        "source/block/blocks.h",
        "source/block/blocks_data.h",
        "source/cglm/cglm.h",
        "source/daytime.h",
        "source/game/game_state.h",
        "source/graphics/biome_color.h",
        "source/graphics/gfx_settings.h",
        "source/graphics/render_block.h",
        "source/network/piston_moving_transport.h",
        # Slice 2 column ownership: chunk.c releases the server-owned backing
        # lease at final shell destruction. No server residency API is called.
        "source/network/server_world.h",
        "source/platform/displaylist.h",
        "source/platform/gfx.h",
        "source/platform/mem2.h",
        "source/platform/thread.h",
        "source/platform/time.h",
        "source/platform/watchdog.h",
        "source/util.h",
    },
    # entity/network/client edge sets harvested by the checker 2026-08-27
    # (steps 8d-8f, "go all"): frozen reality incl. counted direction debt.
    "source/entity/": _ENTITY_EDGES,
    "source/network/": _SERVER_EDGES | {
        "source/settle_observer.h", # passive ready/RPC stage callbacks, no simulation input
        "source/game/client_world_context.h",  # 2026-09-25 B1 context registry: client_interface routes RPCs by world context
        # 2026-09-26 (fire/dispenser parity 879a79c6): client_interface spawns Beta
        # SFX 2000's directed smoke puffs (RenderGlobal:1396) and looks up the
        # smoke particle tile. Same up-stratum client-render debt class as the
        # sim stratum's texture_atlas.h grant; not an endorsement.
        "source/graphics/texture_atlas.h",
    },
    # 2026-09-01 harvest (per-stratum, NOT via _CLIENT_EDGES, to keep the
    # grants minimal): held-map render (648e431) reads the client's map
    # mirror + item_map palette from render_item.c; the MP S5 screens
    # (load_world teardown, mp_connect, mp_setup) drive client_interface /
    # server_remote directly. Counted direction debt, same status as the
    # sim stratum's UP edges -- future accessor-seam work, not endorsement.
    "source/graphics/": _CLIENT_EDGES | {
        "source/platform/", "source/game/",
        "source/item/items/item_map.h",
        "source/network/client_interface.h",
    },
    "source/platform/": _CLIENT_EDGES | {"source/graphics/", "source/game/",
        "source/network/region_archive.h",  # 3c6bedef: Wii watchdog reports archive state.
        "source/settle_observer.h", # worker allocation wait + existing sender drain; leaf API
        "source/tps_observer.h",
        "source/carve_observer.h"}, # dormant allocator/display-list census
    "source/game/": _CLIENT_EDGES | {
        "source/item/recipe.h",  # 52d72d38: recipe browser enumerates registrations.
        "source/network/inventory_crafting.h",  # 52d72d38: recipe browser checks craft availability.
        "source/item/inventory_click.h",  # 9ab8e577: container menu performs inventory clicks.
        "source/item/item_nbt.h",  # ea0de5b3: in-game screen displays preserved item NBT.
        "source/block/aabb.h",  # e72d2805: MP play map's inline support-box check.
        "source/chunk_defs.h",  # d1e09a89: respawn clearance inline uses WORLD_HEIGHT.
        "source/entity/player_ysize.h",  # 87c9029d: camera embeds presentation y-size state.
        "source/item/inventory_sort.h",  # 20febec8: client session drives inventory sorting.
        "source/network/recipe_craft.h",  # 7949911f/52d72d38: client session and guide store reason enum.
        "source/graphics/", "source/platform/",
        "source/network/client_interface.h",
        "source/network/server_remote.h",
        "source/network/client_world_payload.h",  # a7dc5803: loading screen judges player 0's payload terrain (B2 A2-T F2).
    },
    "source/worldgen/": {
        "source/tps_observer.h", # optional initial-light CPU scope, no simulation input
        # measured 2026-08-27 (step 8a): worldgen writes blocks/items into
        # the server world and allocates from MEM2; nothing else.
        "source/block/blocks.h",
        "source/block/blocks_data.h",
        "source/item/items.h",
        "source/item/item_types.h",
        "source/network/server_local.h",
        "source/network/server_local_state.h",
        "source/network/server_world.h",
        "source/platform/mem2.h",
        "source/util.h",
        "source/world.h",
    },
}


def find_headers():
    out = []
    for dirpath, _dirs, files in os.walk(os.path.join(ROOT, "source")):
        for f in files:
            if f.endswith(".h"):
                rel = os.path.relpath(os.path.join(dirpath, f), ROOT)
                rel = rel.replace(os.sep, "/")
                if not rel.startswith(EXCLUDE_PREFIXES):
                    out.append(rel)
    return sorted(out)


def compiler():
    return os.environ.get("CC", "gcc")


def check_self_contained(header):
    """Compile `#include "header"` alone; return (header, error-or-None)."""
    with tempfile.NamedTemporaryFile(
            "w", suffix=".c", delete=False, dir=tempfile.gettempdir()) as tu:
        # include via a path relative to source/ so the header's own quoted
        # includes resolve exactly as they do for real TUs
        rel = header[len("source/"):]
        tu.write('#include "%s"\nint cw_hdr_probe_%s;\n'
                 % (rel, re.sub(r"\W", "_", rel)))
        path = tu.name
    try:
        cp = subprocess.run(
            [compiler(), "-fsyntax-only", "-std=gnu99",
             "-I" + os.path.join(ROOT, "source"),
             "-I" + os.path.join(ROOT, "third_party")] + DEFINES + [path],
            capture_output=True, text=True, timeout=120)
        if cp.returncode != 0:
            first = (cp.stderr.strip().splitlines() or ["<no output>"])[0]
            return header, first
        return header, None
    finally:
        os.unlink(path)


def build_include_graph(headers):
    hset = set(headers)
    graph = {h: set() for h in headers}
    for h in headers:
        try:
            text = open(os.path.join(ROOT, h), encoding="utf-8",
                        errors="replace").read()
        except OSError:
            continue
        for inc in INCLUDE_RE.findall(text):
            # resolve against the includer's dir, then against source/
            cands = [
                os.path.normpath(os.path.join(os.path.dirname(h), inc)),
                os.path.normpath(os.path.join("source", inc)),
            ]
            for c in cands:
                c = c.replace(os.sep, "/")
                if c in hset:
                    graph[h].add(c)
                    break
    return graph


def tarjan_sccs(graph):
    """Iterative Tarjan; returns SCCs with >1 member (or a self-loop)."""
    index, low, onstack = {}, {}, set()
    stack, sccs, counter = [], [], [0]
    for start in graph:
        if start in index:
            continue
        work = [(start, iter(sorted(graph[start])))]
        index[start] = low[start] = counter[0]
        counter[0] += 1
        stack.append(start)
        onstack.add(start)
        while work:
            node, it = work[-1]
            advanced = False
            for nxt in it:
                if nxt not in index:
                    index[nxt] = low[nxt] = counter[0]
                    counter[0] += 1
                    stack.append(nxt)
                    onstack.add(nxt)
                    work.append((nxt, iter(sorted(graph[nxt]))))
                    advanced = True
                    break
                elif nxt in onstack:
                    low[node] = min(low[node], index[nxt])
            if advanced:
                continue
            work.pop()
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[node])
            if low[node] == index[node]:
                comp = []
                while True:
                    w = stack.pop()
                    onstack.discard(w)
                    comp.append(w)
                    if w == node:
                        break
                if len(comp) > 1 or node in graph[node]:
                    sccs.append(sorted(comp))
    return sccs


def scc_lines(sccs):
    return sorted("CYCLE %d: %s" % (len(c), " ".join(c)) for c in sccs)


def _stratum_files(key):
    """Enumerate a stratum's member .c/.h files. A str key is a directory
    prefix; a frozenset key is an explicit file set."""
    if isinstance(key, str):
        for dirpath, _dirs, files in os.walk(os.path.join(ROOT, key)):
            for f in files:
                if f.endswith(".c") or f.endswith(".h"):
                    rel = os.path.relpath(os.path.join(dirpath, f), ROOT)
                    yield rel.replace(os.sep, "/")
    else:
        for rel in sorted(key):
            if os.path.exists(os.path.join(ROOT, rel)):
                yield rel


def _in_stratum(resolved, key):
    if isinstance(key, str):
        return resolved.startswith(key)
    return resolved in key


def _stratum_name(key):
    return key if isinstance(key, str) else "world-file-set"


def check_strata():
    """Return violation strings for cross-stratum includes not in the
    frozen edge set. Scans BOTH .c and .h files of each stratum."""
    out = []
    for key, allowed in STRATA.items():
        for rel in _stratum_files(key):
            text = open(os.path.join(ROOT, rel), encoding="utf-8",
                        errors="replace").read()
            for inc in INCLUDE_RE.findall(text):
                cands = [
                    os.path.normpath(os.path.join(
                        os.path.dirname(rel), inc)).replace(os.sep, "/"),
                    os.path.normpath(os.path.join(
                        "source", inc)).replace(os.sep, "/"),
                ]
                resolved = next((c for c in cands if os.path.exists(
                    os.path.join(ROOT, c))), None)
                if resolved is None or not resolved.startswith("source/"):
                    continue
                if _in_stratum(resolved, key) or resolved in allowed:
                    continue
                # allowed entries ending in "/" are whole-stratum grants
                # (e.g. block/ and item/ are one sim stratum in two dirs)
                if any(a.endswith("/") and resolved.startswith(a)
                       for a in allowed):
                    continue
                out.append("STRATUM %s: %s includes %s (not in the "
                           "frozen edge set)" % (_stratum_name(key), rel,
                                                 resolved))
    return sorted(out)


def load_baseline():
    failing, cycles = set(), set()
    if not os.path.exists(BASELINE):
        return failing, cycles
    for line in open(BASELINE, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("NOT_SELF_CONTAINED "):
            failing.add(line.split(" ", 1)[1])
        elif line.startswith("CYCLE "):
            cycles.add(line)
    return failing, cycles


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "check"
    headers = find_headers()
    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as ex:
        results = list(ex.map(check_self_contained, headers))
    failing = {h: err for h, err in results if err is not None}
    graph = build_include_graph(headers)
    cycles = scc_lines(tarjan_sccs(graph))

    if mode == "--report":
        print("headers checked: %d   not self-contained: %d   cycles: %d"
              % (len(headers), len(failing), len(cycles)))
        for h in sorted(failing):
            print("NOT_SELF_CONTAINED %s\n    %s" % (h, failing[h]))
        for c in cycles:
            print(c)
        return 0

    if mode == "--baseline":
        with open(BASELINE, "w", encoding="utf-8", newline="\n") as f:
            f.write("# Header-hygiene DEBT baseline (step 7). Every line is\n"
                    "# existing debt the gate tolerates; new debt fails the\n"
                    "# build. Shrink this file, never grow it -- regenerate\n"
                    "# with --baseline ONLY after removing debt, and diff the\n"
                    "# result in the same commit.\n")
            for h in sorted(failing):
                f.write("NOT_SELF_CONTAINED %s\n" % h)
            for c in cycles:
                f.write(c + "\n")
        print("baseline written: %d failing headers, %d cycles"
              % (len(failing), len(cycles)))
        return 0

    strata_viol = check_strata()
    base_fail, base_cycles = load_baseline()
    new_fail = sorted(set(failing) - base_fail)
    new_cycles = sorted(set(cycles) - base_cycles)
    fixed_fail = sorted(base_fail - set(failing))
    fixed_cycles = sorted(base_cycles - set(cycles))
    ok = True
    for v in strata_viol:
        print("HEADER-HYGIENE FAIL: " + v)
        ok = False
    for h in new_fail:
        print("HEADER-HYGIENE FAIL: %s is not self-contained (new debt)\n"
              "    %s" % (h, failing[h]))
        ok = False
    for c in new_cycles:
        print("HEADER-HYGIENE FAIL: new/grown include cycle:\n    %s" % c)
        ok = False
    if fixed_fail or fixed_cycles:
        print("header-hygiene: %d baseline entries no longer fail -- debt "
              "shrank; refresh the baseline in your commit (--baseline)"
              % (len(fixed_fail) + len(fixed_cycles)))
    print("header-hygiene: %d headers, %d/%d failing (baseline %d), "
          "%d/%d cycles (baseline %d) -- %s"
          % (len(headers), len(failing), len(headers), len(base_fail),
             len(cycles), len(cycles), len(base_cycles),
             "OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
