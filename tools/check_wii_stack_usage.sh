#!/usr/bin/env bash
# check_wii_stack_usage.sh -- gate against the 2026-08-26 boot-death class: a
# stack-local so big it blows libogc's 128KB main-thread stack (main() hit a
# 188KB frame before the fix in source/main.c made the server_local `static`).
# GCC's -fstack-usage prints every function's worst-case frame size straight
# from the compiler, so this is the cheapest possible net-cast: build the DEV
# (non-LTO) Wii variant with it on, and FAIL if any first-party function's
# frame exceeds a threshold and isn't an explicitly-justified exception.
#
# Why the DEV variant: -fstack-usage and LTO don't mix usefully -- with LTO the
# .su files are written per-GIMPLE-object at compile time, before the LTRANS
# link-time codegen that actually decides final frame layout, so the numbers
# would be misleading. `make dev` (CW_DEV=1) is LTO-free and lands objects in
# build-dev/, so this never touches build/ or the release CaveWii_v2.dol.
#
# Usage:
#   bash tools/check_wii_stack_usage.sh              # build dev with -fstack-usage, then gate
#   bash tools/check_wii_stack_usage.sh --scan-only  # gate the .su files already in build-dev/
#
# The build needs the patched libogc sidecar: the Makefile's sidecar gate
# (Makefile:40-48) accepts LIBOGC_LIB ONLY as a make command-line assignment,
# so this script forwards it. Sources, in order: LIBOGC_LIB (env), then
# CW_LIBOGC_LIB (the tools/cwbuild.ps1 variable). A caller that already puts
# LIBOGC_LIB= in MAKEFLAGS is left alone, and CW_ALLOW_STOCK_LIBOGC=1 (CI
# compile gate only) builds against stock. Anything else fails before
# build-dev/ is touched. Pinned by tests/test_check_wii_stack_usage.sh.
set -euo pipefail

SCAN_ONLY=0
for arg in "$@"; do
  case "$arg" in
    --scan-only) SCAN_ONLY=1 ;;
    *) echo "[stack-check] FAIL: unknown argument: $arg (usage: check_wii_stack_usage.sh [--scan-only])"; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO"

THRESHOLD_BYTES=16384
ALLOWLIST="$REPO/tools/wii_stack_allowlist.txt"
BUILD_DIR="$REPO/build-dev"

if [ "$SCAN_ONLY" = 1 ]; then
  [ -d "$BUILD_DIR" ] || { echo "[stack-check] FAIL: --scan-only but no build dir: $BUILD_DIR"; exit 1; }
  echo "[stack-check] --scan-only: gating the existing .su files under $BUILD_DIR (no build)"
  mkdir -p "$REPO/.buildtmp"
else

# --- locate devkitPPC -------------------------------------------------------
# CI (devkitpro/devkitppc container) already exports DEVKITPRO=/opt/devkitpro;
# this Windows host needs the MSYS path spelled out explicitly (nothing sets it
# by default). Precedence: explicit DEVKITPPC env > DEVKITPRO env > this host's
# known MSYS install > the generic Linux/container default.
if [ -z "${DEVKITPPC:-}" ]; then
  if [ -n "${DEVKITPRO:-}" ]; then
    DEVKITPPC="$DEVKITPRO/devkitPPC"
  elif [ -d /c/msys64/opt/devkitpro/devkitPPC ]; then
    DEVKITPPC=/c/msys64/opt/devkitpro/devkitPPC
  else
    DEVKITPPC=/opt/devkitpro/devkitPPC
  fi
fi
DEVKITPRO="${DEVKITPRO:-$(dirname "$DEVKITPPC")}"
export DEVKITPPC DEVKITPRO

[ -d "$DEVKITPPC" ] || { echo "[stack-check] FAIL: DEVKITPPC not found: $DEVKITPPC"; exit 1; }

export PATH="$DEVKITPPC/bin:$PATH"
[ -d /c/msys64/usr/bin ] && export PATH="/c/msys64/usr/bin:$PATH"
[ -d /usr/bin ] && export PATH="/usr/bin:$PATH"

# --- libogc sidecar (see Usage above) ----------------------------------------
# Resolved before ANY destructive step, so a refusal never costs build-dev/.
LIBOGC_ARG=()
SIDECAR="${LIBOGC_LIB:-${CW_LIBOGC_LIB:-}}"
if [ -n "$SIDECAR" ]; then
  LIBOGC_ARG=("LIBOGC_LIB=$SIDECAR")
  echo "[stack-check] libogc: LIBOGC_LIB=$SIDECAR (make command line)"
elif [[ " ${MAKEFLAGS:-} " == *" LIBOGC_LIB="* ]]; then
  echo "[stack-check] libogc: LIBOGC_LIB supplied by the caller's MAKEFLAGS"
elif [ "${CW_ALLOW_STOCK_LIBOGC:-}" = 1 ]; then
  echo "[stack-check] libogc: STOCK (CW_ALLOW_STOCK_LIBOGC=1 -- compile gate only)"
else
  echo "[stack-check] FAIL: no libogc sidecar. Set LIBOGC_LIB=<sidecar stage-lib> (or CW_LIBOGC_LIB);"
  echo "[stack-check]       the Makefile refuses a stock link otherwise (docs/wii_libogc_kernel_heap_sidecar.md)."
  echo "[stack-check]       Or run with --scan-only against an existing build-dev/."
  exit 1
fi

MAKE="${MAKE:-}"
if [ -z "$MAKE" ]; then
  if [ -x /c/msys64/usr/bin/make ]; then
    MAKE=/c/msys64/usr/bin/make
  else
    MAKE=make
  fi
fi
command -v "$MAKE" >/dev/null 2>&1 || { echo "[stack-check] FAIL: make not found: $MAKE"; exit 1; }

# --- cc-wrap.sh: temp-trap-safe gcc wrapper ---------------------------------
# Regenerated identically to .agents/skills/build-wii/scripts/build-verify.sh's
# copy (it's gitignored -- a fresh clone/CI checkout won't have it). Pins
# TMP/TEMP/TMPDIR off C:\Windows, which mid-link denies temp-file writes and
# fails the build silently on this host (see CLAUDE.md's Wii silent-failure
# trap note).
WRAP="$REPO/.buildtmp/cc-wrap.sh"
if [ ! -f "$WRAP" ]; then
  mkdir -p "$REPO/.buildtmp"
  cat > "$WRAP" <<EOF
#!/bin/sh
D='$REPO/.buildtmp'
export TMP="\$D"; export TEMP="\$D"; export TMPDIR="\$D"
exec powerpc-eabi-gcc "\$@"
EOF
  echo "[stack-check] regenerated $WRAP"
fi

# --- build -------------------------------------------------------------------
# Fresh build-dev only: a stale .su from a previous invocation with different
# flags would silently under- or over-report. NEVER touches build/ or the
# release CaveWii_v2.dol (design constraint -- this script must be safe to run
# next to a hardware-proof release build in progress).
#
# Also remove the dev .elf/.dol/.map at repo root (the Makefile's $(OUTPUT)
# lives outside build-dev/, per the release-artifact-naming block up top).
# Leaving a stale CaveWii_v2-dev.elf in place while build-dev/ is empty makes
# GNU Make's implicit-rule search decide the link is already "up to date"
# without ever recompiling anything -- observed on this host: a rerun after
# build-dev/ was cleared produced zero .su files because make skipped the
# whole rebuild. Deleting the dev artifacts too closes that hole.
rm -rf "$BUILD_DIR"
rm -f "$REPO"/CaveWii_v2-dev.elf "$REPO"/CaveWii_v2-dev.dol "$REPO"/CaveWii_v2-dev.elf.map
# ...and the CW_ALLOW_STOCK_LIBOGC=1 (-stock) spellings the Makefile emits under CI.
rm -f "$REPO"/CaveWii_v2-dev-stock.elf "$REPO"/CaveWii_v2-dev-stock.dol "$REPO"/CaveWii_v2-dev-stock.elf.map

echo "[stack-check] building dev variant with -fstack-usage ..."
"$MAKE" dev DEVKITPRO="$DEVKITPRO" DEVKITPPC="$DEVKITPPC" \
  ${LIBOGC_ARG[@]+"${LIBOGC_ARG[@]}"} \
  CC="sh $WRAP" EXTRA_DEFS=-fstack-usage -j"${STACK_CHECK_JOBS:-10}"
fi # SCAN_ONLY

# --- parse .su files -----------------------------------------------------
# Every SOURCES entry the Makefile compiles lands a .su next to its .o in
# build-dev/ (prebuilt libs like libogc/wiiuse never emit one, so this is
# already scoped to first-party-compiled code with zero extra filtering).
# .su line shape: "file:line:col:function<TAB>bytes<TAB>qualifier". Only the
# byte count matters -- static/bss usage is a different report entirely.
mapfile -t SU_FILES < <(find "$BUILD_DIR" -name '*.su' 2>/dev/null | sort)
if [ "${#SU_FILES[@]}" -eq 0 ]; then
  echo "[stack-check] FAIL: no .su files produced under $BUILD_DIR -- build didn't run with -fstack-usage"
  exit 1
fi

REPORT="$REPO/.buildtmp/stack_usage_report.txt"
: > "$REPORT"
for f in "${SU_FILES[@]}"; do
  # awk splits on tab; field 1 = "file:line:col:function", field 2 = bytes.
  awk -F'\t' 'NF>=2 { print $1 "\t" $2 }' "$f" >> "$REPORT"
done

TOTAL_FUNCS=$(wc -l < "$REPORT" | tr -d ' ')
echo "[stack-check] parsed $TOTAL_FUNCS function frames from ${#SU_FILES[@]} .su files"

echo
echo "=== top-10 stack frames (FYI, every run) ==="
# Sort to a file rather than piping straight into `head`: with pipefail set,
# `sort | head -10` has `sort` receive SIGPIPE (exit 141) the instant `head`
# closes its stdin after 10 lines, and `set -e` then kills the whole script --
# hit on this host with a 2803-line report. Writing the sorted file first
# means nothing closes the pipe out from under `sort`.
SORTED="$REPO/.buildtmp/stack_usage_sorted.txt"
sort -t $'\t' -k2 -rn "$REPORT" > "$SORTED"
head -10 "$SORTED" | while IFS=$'\t' read -r loc bytes; do
  printf '  %8s bytes  %s\n' "$bytes" "$loc"
done
echo

# --- confirm the fix: main() must now be small ------------------------------
# (awk, not `grep -P`: this host's grep 3.0 rejects -P outside a UTF-8 locale.)
MAIN_LINE=$(awk -F'\t' '$1 ~ /\/main\.c:[0-9]+:[0-9]+:main$/ { print; exit }' "$REPORT")
if [ -z "$MAIN_LINE" ]; then
  echo "[stack-check] WARN: could not find main()'s frame in the report (grep miss -- check .su format)"
else
  MAIN_BYTES=$(printf '%s\n' "$MAIN_LINE" | cut -f2)
  echo "[stack-check] main() frame: $MAIN_BYTES bytes (fix comment claims ~1,280-1,576 B; libogc main stack is 128KB)"
fi

# --- load the allowlist ------------------------------------------------------
# One function name per line; '#' comments allowed. Every entry MUST carry a
# justification comment (same line, after the name) -- an allowlist entry with
# no stated reason is exactly the silent-drift this check exists to prevent.
declare -A ALLOWED
if [ -f "$ALLOWLIST" ]; then
  while IFS= read -r line; do
    trimmed="${line#"${line%%[![:space:]]*}"}"
    [ -z "$trimmed" ] && continue
    [ "${trimmed:0:1}" = "#" ] && continue
    name="${trimmed%%#*}"
    name="${name%"${name##*[![:space:]]}"}"
    comment="${trimmed#*#}"
    [ -z "$name" ] && continue
    if [ "$trimmed" = "$name" ]; then
      echo "[stack-check] FAIL: allowlist entry '$name' has no justification comment (see $ALLOWLIST)"
      exit 1
    fi
    [ -z "${comment// /}" ] && {
      echo "[stack-check] FAIL: allowlist entry '$name' has an empty justification comment"
      exit 1
    }
    ALLOWED["$name"]=1
  done < "$ALLOWLIST"
fi

# --- apply the threshold -----------------------------------------------------
OFFENDERS=()
ALLOWED_HITS=()
while IFS=$'\t' read -r loc bytes; do
  [ -z "${loc:-}" ] && continue
  fn="${loc##*:}"
  if [ "$bytes" -gt "$THRESHOLD_BYTES" ]; then
    if [ -n "${ALLOWED[$fn]:-}" ]; then
      ALLOWED_HITS+=("$bytes bytes  $loc  (allowlisted)")
    else
      OFFENDERS+=("$bytes bytes  $loc")
    fi
  fi
done < "$REPORT"

if [ "${#ALLOWED_HITS[@]}" -gt 0 ]; then
  echo "=== allowlisted frames over ${THRESHOLD_BYTES}B (documented exceptions) ==="
  printf '  %s\n' "${ALLOWED_HITS[@]}"
  echo
fi

if [ "${#OFFENDERS[@]}" -gt 0 ]; then
  echo "=== FAIL: frames over ${THRESHOLD_BYTES}B with no allowlist entry ==="
  printf '  %s\n' "${OFFENDERS[@]}"
  echo
  echo "[stack-check] Add each to $ALLOWLIST with a justification, or fix the frame."
  echo "[stack-check] VERDICT: FAIL"
  exit 1
fi

echo "[stack-check] VERDICT: PASS -- no un-allowlisted frame exceeds ${THRESHOLD_BYTES}B"
exit 0
