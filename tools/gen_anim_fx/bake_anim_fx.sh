#!/usr/bin/env bash
#
# bake_anim_fx.sh -- bake authentic Beta 1.7.3 procedural fire, lava, and
# portal filmstrips into a CaveWii pack's anim.png.
#
# Fire and lava are transparent blocks, so they render from anim.png via the
# world.c matrix_anim scroll over a vertical filmstrip. Beta generates these
# textures procedurally. This tool runs the faithful C ports offline and
# composites them into the cells sampled by fire, lava, and portal rendering.
#
# SCOPE, stated precisely (the old claim "every other tile remains byte-identical"
# was FALSE): composite_anim_fx.py's paste_cell writes an 18px block at col*18+2,
# so each target column ALSO overwrites the first 2px of the next column's gutter
# at (col+1)*18+0..+1. Targets are cols {0,7} portal, {2,9} lava, {3,10} fire,
# {6,13} fire+16 (Beta's second TextureFlamesFX, 2026-09-24), so the bleed lands
# in the gutters of cols 1, 4, 7, 8 and 11 (col 13's block ends at x=253) -- and
# cols 4/11 are the
# authentic Beta ICE composite from make_beta_pack.ps1. The bleed is HARMLESS
# because the sampled region of a cell is col*18+2..+19 (16px content at +3 with a
# 1px border), which the bleed never reaches -- but it is a real byte difference,
# and tests/test_anim_pack_art.c pins that the ice CONTENT survives it. Run this
# AFTER the ice composite, never before.
#
# WATER STILL (cols 1 & 8) and WATER FLOW (cols 5 & 12) are BOTH baked from the
# ports as of 2026-07-31 (water_fx = TextureWaterFX, water_flow_fx =
# TextureWaterFlowFX). Before that the flow tile was fork placeholder art with a
# grey blue-channel (75..91 where Beta pins 255), so every SIDE face of every
# water block was grey under a correct blue top -- the visible "seam" that got
# the correct still bake reverted on 07-30. Both RNGs are documented non-Beta
# LCGs (the algorithms are the faithful FX classes; the seed streams are not
# parity, and Beta's own use Math.random()).
#
# Usage: tools/gen_anim_fx/bake_anim_fx.sh <target-anim.png> [base-anim.png] [--ramp-dir <dir>]
# A ramp directory must contain lava.ramp, flame.ramp (shared by both flames),
# water.ramp, water_flow.ramp and portal.ramp, each exactly 1024 raw RGBA bytes.
# Only RGB is replaced; Beta alpha, motion, frame cadence and layout are retained.
#
# Requires a C compiler and Python 3 with Pillow.
# Override CC or PYTHON when those executables are not on the default PATH.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
GFX="$REPO/source/graphics"
CC="${CC:-cc}"
PYTHON="${PYTHON:-python3}"

usage() {
  echo "usage: bake_anim_fx.sh <target-anim.png> [base-anim.png] [--ramp-dir <dir>]"
}

POSITIONAL=()
RAMP_DIR=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --ramp-dir)
      if [ "$#" -lt 2 ] || [ -z "$2" ] || [ -n "$RAMP_DIR" ]; then
        usage >&2
        exit 1
      fi
      RAMP_DIR="$2"
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    --*) usage >&2; exit 1 ;;
    *) POSITIONAL+=("$1"); shift ;;
  esac
done
if [ "${#POSITIONAL[@]}" -lt 1 ] || [ "${#POSITIONAL[@]}" -gt 2 ]; then
  usage >&2
  exit 1
fi
TARGET="${POSITIONAL[0]}"
BASE="${POSITIONAL[1]:-}"
FX_ARGS=()
if [ -n "$RAMP_DIR" ]; then
  # fx_gen runs in WORK; resolve a caller-relative directory before changing cwd.
  RAMP_DIR="$(cd -- "$RAMP_DIR" && pwd)"
  FX_ARGS=(--ramp-dir "$RAMP_DIR")
fi
if [ -z "$BASE" ]; then
  if [ -f "$TARGET" ]; then
    BASE="$TARGET"
  else
    BASE="$REPO/assets/anim.png"
  fi
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

"$CC" -O2 -ffp-contract=off -I "$GFX" -I "$REPO/source" \
  "$HERE/fx_gen.c" \
  "$GFX/lava_fx.c" "$GFX/flame_fx.c" "$GFX/portal_fx.c" "$GFX/water_fx.c" \
  "$GFX/water_flow_fx.c" \
  "$REPO/source/worldgen/beta_math.c" \
  "$REPO/source/worldgen/java_random.c" \
  -lm -o "$WORK/fx_gen"

(cd "$WORK" && ./fx_gen "${FX_ARGS[@]}")
"$PYTHON" "$HERE/composite_anim_fx.py" "$WORK" "$BASE" "$TARGET"
echo "[bake_anim_fx] baked fire+fire16+lava+portal+water filmstrips into $TARGET (base: $BASE)"
