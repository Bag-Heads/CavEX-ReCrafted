#!/usr/bin/env bash
# tools/build_libogc_sidecar.sh
#
# Rebuilds libogc v3.1.0 as a SIDECAR (the devkitPro toolchain install is
# NEVER touched) with the lwp KERNEL_HEAP workspace raised 64KB -> 256KB, and
# assembles a stage-lib/ directory that mirrors devkitPro's
# $DEVKITPRO/libogc/lib/wii layout with the patched libogc.a (and its
# catnip-built siblings) swapped in. Point the Wii .dol build at it with:
#
#   make DEVKITPPC=/opt/devkitpro/devkitPPC DEVKITPRO=/opt/devkitpro \
#        LIBOGC_LIB=<out-dir>/stage-lib
#
# or, through the build-wii skill's md5-gated wrapper:
#
#   .claude/skills/build-wii/scripts/build-verify.sh LIBOGC_LIB=<out-dir>/stage-lib
#
# WHY: see third_party/libogc-kernel-heap/README.md. Short version: wiiuse
# needs 18,432 contiguous bytes from libogc's lwp kernel workspace at Wiimote
# attach; the stock 64KB KERNEL_HEAP (libogc/system.c) left only ~4,932B free
# once this game's own boot allocations landed, so the attach silently failed
# (io_wii.c:275) and the Wiimote was permanently inert. 256KB was hardware-
# measured to leave ~146,196B free in one contiguous block (128KB was tried
# first and measured INSUFFICIENT -- see the README).
#
# This is a STANDING build step until upstream libogc makes KERNEL_HEAP
# configurable (it is presently a hardcoded #define, not a Kconfig/env knob).
#
# EXPERIMENTAL, HARDWARE-UNCONFIRMED (patch 0003): this sidecar also carries a
# libogc/wiisd.c __sd0_initio() cold-boot-retry patch, targeting this rig's
# 8/8 pre-FAT cold-boot deaths (PROJECT_MEMORY 2026-08-25) and matching
# devkitPro sourceforge bug #94 (cards failing the unconditional
# __sdio_resetcard() call right after Wii power-up). Unlike the KERNEL_HEAP
# patch above, this one is NOT yet certified on real hardware -- see
# third_party/libogc-kernel-heap/README.md's dedicated section before relying
# on it, and do not report it as a fix until a cold-boot PASS exists.
#
# ---------------------------------------------------------------------------
# Hard-won traps this script bakes in (2026-08-23 debugging marathon):
#   1. MSYS2's own `cmake` is required. Windows-native cmake refuses the
#      catnip build (it validates the whole toolset tree, not just the
#      selected preset, and chokes on path/tool assumptions Windows-native
#      cmake does not satisfy).
#   2. The build MUST run inside `MSYSTEM=MSYS bash -l ...`, with DEVKITPRO
#      exported INSIDE that shell and "/c/Program Files/Git/cmd" appended to
#      PATH there. Environment variables set outside that shell (e.g. from a
#      plain git-bash / this script's own top-level shell) do NOT cross into
#      it, and catnip's version detection shells out to `git`, which is not
#      normally on a bare MSYS PATH.
#   3. The `cube` (GameCube) preset -- and its DEFAULT-preset membership --
#      must be dropped from Catnip.cmake before configuring. This host has no
#      GameCube toolset installed, and catnip validates the WHOLE tree before
#      building anything, so a `cube` preset with no toolset aborts the
#      entire build, including the `wii` preset we actually want.
#   4. The build must happen at a SHORT path. Long paths exceed
#      CMAKE_OBJECT_PATH_MAX and `ar` fails with "No such file or directory"
#      deep into the build. The default OUT_DIR below is deliberately short.
#
# Plus two verification disciplines (verify OUTPUT, never trust exit code):
#   - ALWAYS rm -rf/re-clone before building. catnip's incremental depfiles
#     break on a fresh/relocated tree with a "multiple target patterns" make
#     error (a Windows drive-colon artifact baked into compiler_depend.make).
#   - objdump-gate the 0x40000 (256KB) literal feeding __lwp_wkspace_init AND
#     confirm the built .a's mtime actually advanced past this run's start --
#     a green `catnip` exit code that silently did nothing is exactly the
#     "stale .dol" trap this project has hit before, one level down the stack.
#
# ---------------------------------------------------------------------------
# Usage:
#   tools/build_libogc_sidecar.sh [OUT_DIR] [LIBOGC_SRC] [LIBOGC_REF]
#
#   OUT_DIR     Output/build directory (also becomes the source checkout).
#               Default: <repo>/../logc256kr (sibling of the checkout)
#               MUST be short (trap 4) and MUST NOT be inside this repo.
#   LIBOGC_SRC  A local libogc git repo (any branch/state; only its committed
#               v3.1.0 tag is used -- a dirty working tree there is fine and
#               is never read) OR a git URL.
#               Default: <repo>/../_libogc_audit (sibling of the checkout)
#               Fallback if that sibling doesn't exist on this machine: this
#               script clones straight from the devkitPro upstream instead --
#                 https://github.com/devkitPro/libogc  (tag v3.1.0)
#   LIBOGC_REF  Git tag/ref to check out. Default: v3.1.0
#
# Env overrides:
#   DEVKITPRO_HOST_PATH  Where the OFFICIAL devkitPro install lives, for the
#                         two .a files (libfat.a, libgxflux.a) that are NOT
#                         part of the libogc source tree catnip builds, and
#                         are copied into stage-lib unmodified.
#                         Default: /c/msys64/opt/devkitpro
#
# Exit codes:
#   0 = built, staged, both objdump gates PASSED, mtime-advance PASSED.
#   2 = bad arguments / missing prerequisite tool.
#   3 = LIBOGC_SRC not usable (not a repo, ref not found).
#   4 = OUT_DIR exists and is not a prior product of this script (refusing
#       to blow it away -- remove it yourself if that's really intended).
#   5 = a patch (0001/0002/0003/0004/0005) failed to apply, or its post-patch
#       sanity grep came back empty.
#   6 = the MSYS/catnip build failed.
#   7 = an objdump gate FAILED: the KERNEL_HEAP gate (0x40000 literal not
#       found feeding __lwp_wkspace_init) or the EXPERIMENTAL sd0_initio
#       retry gate (sd0_get_init_attempts symbol absent, or its compiled
#       trip-count evidence absent near the SEND_OP_COND call site).
#   8 = mtime-advance check FAILED (the built .a is not newer than this run
#       -- the build silently did nothing).
#   9 = stage-lib assembly failed (an expected .a is missing).
set -u -o pipefail

# The CI container uses the same pinned source and patch set, but a native
# Linux catnip invocation rather than the Windows/MSYS2 host wrapper below.
if [ "${1:-}" = --container ]; then
  shift
  exec bash "$(dirname "${BASH_SOURCE[0]}")/build_libogc_sidecar_container.sh" "$@"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" || { echo "[sidecar] FAIL: cannot resolve script dir"; exit 2; }
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)" || { echo "[sidecar] FAIL: cannot resolve repo root"; exit 2; }
PATCH_DIR="$REPO_ROOT/third_party/libogc-kernel-heap"
PATCH_KERNEL_HEAP="$PATCH_DIR/0001-lwp-kernel-heap-64k-to-256k.patch"
PATCH_CATNIP_CUBE="$PATCH_DIR/0002-catnip-drop-cube-preset-host-has-no-gamecube-toolset.patch"
PATCH_SD0_RETRY="$PATCH_DIR/0003-wiisd-sd0-initio-cold-boot-retry.patch"
PATCH_FPU_OWNER="$PATCH_DIR/0004-tuxedo-clear-lazy-fpu-owner-on-thread-exit.patch"
PATCH_SD0_ALIGN="$PATCH_DIR/0005-wiisd-sd0-functions-align64.patch"

OUT_DIR="${1:-$REPO_ROOT/../logc256kr}"
LIBOGC_SRC="${2:-$REPO_ROOT/../_libogc_audit}"
LIBOGC_REF="${3:-v3.1.0}"
DEVKITPRO_HOST_PATH="${DEVKITPRO_HOST_PATH:-/c/msys64/opt/devkitpro}"
UPSTREAM_FALLBACK="https://github.com/devkitPro/libogc"
MSYS_BASH="/c/msys64/usr/bin/bash"
MARKER=".sidecar-build-marker"

log() { printf '[sidecar] %s\n' "$*"; }
fail() { printf '[sidecar] FAIL: %s\n' "$*" >&2; exit "$1"; }

# ---------------------------------------------------------------------------
# 0. Prerequisites
# ---------------------------------------------------------------------------
for TOOL in git objdump mkdir cp rm stat date; do
  command -v "$TOOL" >/dev/null 2>&1 || fail 2 "required tool not on PATH: $TOOL"
done
OBJDUMP="$DEVKITPRO_HOST_PATH/devkitPPC/bin/powerpc-eabi-objdump.exe"
[ -x "$OBJDUMP" ] || fail 2 "devkitPPC objdump not found/executable: $OBJDUMP"
[ -x "$MSYS_BASH" ] || fail 2 "MSYS2 bash not found: $MSYS_BASH (trap 1/2 needs the MSYS2 install, not plain git-bash)"
[ -f "$PATCH_KERNEL_HEAP" ] || fail 2 "missing patch: $PATCH_KERNEL_HEAP"
[ -f "$PATCH_CATNIP_CUBE" ] || fail 2 "missing patch: $PATCH_CATNIP_CUBE"
[ -f "$PATCH_SD0_RETRY" ] || fail 2 "missing patch: $PATCH_SD0_RETRY"
[ -f "$PATCH_FPU_OWNER" ] || fail 2 "missing patch: $PATCH_FPU_OWNER"
[ -f "$PATCH_SD0_ALIGN" ] || fail 2 "missing patch: $PATCH_SD0_ALIGN"

case "$OUT_DIR" in
  /|/c|/c/|"$REPO_ROOT"|"$REPO_ROOT"/*) fail 2 "refusing OUT_DIR=$OUT_DIR (unsafe or inside the repo)" ;;
esac

log "OUT_DIR     = $OUT_DIR"
log "LIBOGC_SRC  = $LIBOGC_SRC"
log "LIBOGC_REF  = $LIBOGC_REF"

# ---------------------------------------------------------------------------
# 1. Resolve the source: a local repo, or the documented upstream fallback.
# ---------------------------------------------------------------------------
CLONE_FROM="$LIBOGC_SRC"
CLONE_LOCAL_FLAG="--local"
if [ ! -d "$LIBOGC_SRC/.git" ]; then
  log "LIBOGC_SRC not found locally ($LIBOGC_SRC) -- falling back to upstream: $UPSTREAM_FALLBACK"
  CLONE_FROM="$UPSTREAM_FALLBACK"
  CLONE_LOCAL_FLAG=""
fi

# ---------------------------------------------------------------------------
# 2. OUT_DIR: refuse to blow away anything that isn't our own prior output.
# ---------------------------------------------------------------------------
if [ -e "$OUT_DIR" ]; then
  if [ -f "$OUT_DIR/$MARKER" ]; then
    log "OUT_DIR exists and carries our marker -- removing for a fresh build"
    rm -rf "$OUT_DIR" || fail 4 "could not remove existing OUT_DIR: $OUT_DIR"
  else
    fail 4 "OUT_DIR exists and is NOT a prior product of this script (no $MARKER found): $OUT_DIR -- remove it manually if that is intended"
  fi
fi

# ---------------------------------------------------------------------------
# 3. Fresh clone at the pinned ref. Cloning NEVER copies a source repo's
#    uncommitted/dirty working-tree state (only committed history) -- this is
#    what makes it safe to default LIBOGC_SRC at _libogc_audit even though
#    that checkout currently sits on kernel-heap-128k with an uncommitted
#    128KB experiment: only the committed v3.1.0 TAG is ever read.
# ---------------------------------------------------------------------------
log "cloning $CLONE_FROM @ $LIBOGC_REF -> $OUT_DIR"
git clone $CLONE_LOCAL_FLAG --branch "$LIBOGC_REF" --single-branch "$CLONE_FROM" "$OUT_DIR" \
  || fail 3 "git clone failed (bad LIBOGC_SRC, or $LIBOGC_REF is not a ref in it)"

RESOLVED_DESC="$(git -C "$OUT_DIR" describe --tags 2>/dev/null || true)"
RESOLVED_HASH="$(git -C "$OUT_DIR" rev-parse HEAD 2>/dev/null || true)"
[ "$RESOLVED_DESC" = "$LIBOGC_REF" ] || fail 3 "cloned tree is not exactly $LIBOGC_REF (got: ${RESOLVED_DESC:-<none>})"
log "resolved: $RESOLVED_DESC ($RESOLVED_HASH)"

# ---------------------------------------------------------------------------
# 4. Apply the patches.
# ---------------------------------------------------------------------------
log "applying 0001 (KERNEL_HEAP 64k -> 256k)"
git -C "$OUT_DIR" apply --whitespace=fix "$PATCH_KERNEL_HEAP" || fail 5 "0001 patch failed to apply"
log "applying 0002 (drop cube preset -- trap 3)"
git -C "$OUT_DIR" apply --whitespace=fix "$PATCH_CATNIP_CUBE" || fail 5 "0002 patch failed to apply"
log "applying 0003 (sd0_initio cold-boot retry -- EXPERIMENTAL, hardware-unconfirmed)"
git -C "$OUT_DIR" apply --whitespace=fix "$PATCH_SD0_RETRY" || fail 5 "0003 patch failed to apply"
log "applying 0004 (tuxedo clear lazy-FPU owner on thread exit -- S5.3 kernel UAF fix)"
git -C "$OUT_DIR" apply --whitespace=fix "$PATCH_FPU_OWNER" || fail 5 "0004 patch failed to apply"
log "applying 0005 (wiisd __sd0_functions ATTRIBUTE_ALIGN 32 -> 64 -- EXPERIMENTAL, hardware-unconfirmed)"
git -C "$OUT_DIR" apply --whitespace=fix "$PATCH_SD0_ALIGN" || fail 5 "0005 patch failed to apply"

APPLIED_HEAP="$(grep -c '#define KERNEL_HEAP.*256\*1024' "$OUT_DIR/libogc/system.c" || true)"
[ "$APPLIED_HEAP" = "1" ] || fail 5 "post-patch sanity check failed: KERNEL_HEAP is not (256*1024) in $OUT_DIR/libogc/system.c"

APPLIED_SD0_TRIES="$(grep -c 'tries = 20;' "$OUT_DIR/libogc/wiisd.c" || true)"
[ "$APPLIED_SD0_TRIES" = "1" ] || fail 5 "post-patch sanity check failed: widened retry count (tries = 20;) not found in $OUT_DIR/libogc/wiisd.c"
APPLIED_SD0_ACCESSOR="$(grep -c 'sd0_get_init_attempts' "$OUT_DIR/libogc/wiisd.c" || true)"
[ "$APPLIED_SD0_ACCESSOR" -ge "1" ] || fail 5 "post-patch sanity check failed: sd0_get_init_attempts accessor not found in $OUT_DIR/libogc/wiisd.c"

APPLIED_FPU_OWNER="$(grep -c 'PPCMfspr(SPRG3) == (u32)self' "$OUT_DIR/tuxedo/thread.c" || true)"
[ "$APPLIED_FPU_OWNER" = "1" ] || fail 5 "post-patch sanity check failed: SPRG3 owner clear not found in $OUT_DIR/tuxedo/thread.c"

APPLIED_SD0_ALIGN="$(grep -c '__sd0_functions\[64\] ATTRIBUTE_ALIGN(64)' "$OUT_DIR/libogc/wiisd.c" || true)"
[ "$APPLIED_SD0_ALIGN" = "1" ] || fail 5 "post-patch sanity check failed: __sd0_functions[64] ATTRIBUTE_ALIGN(64) not found in $OUT_DIR/libogc/wiisd.c"

# ---------------------------------------------------------------------------
# 5. Build inside a real MSYS2 login shell (traps 1, 2, 4; discipline: always
#    rm -rf build first even though this is a fresh clone, so the script
#    stays correct if OUT_DIR handling ever changes to reuse a checkout).
# ---------------------------------------------------------------------------
BUILD_START_EPOCH="$(date +%s)"
BUILD_LOG="$OUT_DIR/../$(basename "$OUT_DIR")_sidecar_build.log"
OUT_DIR_MSYS="$OUT_DIR"   # same /c/... form resolves under MSYS2 as under git-bash on this box

MSYS_CMD='set -e
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC="$DEVKITPRO/devkitPPC"
export PATH="$PATH:/c/Program Files/Git/cmd"
cd '"'$OUT_DIR_MSYS'"'
rm -rf build
catnip'

log "building via MSYSTEM=MSYS bash -lc (log: $BUILD_LOG)"
if ! MSYSTEM=MSYS "$MSYS_BASH" -lc "$MSYS_CMD" > "$BUILD_LOG" 2>&1; then
  log "----- last 60 lines of $BUILD_LOG -----"
  tail -n 60 "$BUILD_LOG" >&2
  fail 6 "MSYS catnip build failed -- see $BUILD_LOG"
fi
log "catnip build reported success"

BUILT_LIBOGC_A="$OUT_DIR/build/libogc.wii/libogc/libogc.a"
[ -f "$BUILT_LIBOGC_A" ] || fail 6 "catnip reported success but $BUILT_LIBOGC_A does not exist"

# ---------------------------------------------------------------------------
# 6. objdump gate: `lis r3,4` (0x00040000 = 256KB) must feed the
#    __lwp_wkspace_init(KERNEL_HEAP) call site inside SYS_Init. FAIL LOUDLY,
#    do not just check the exit code of catnip.
# ---------------------------------------------------------------------------
log "objdump gate: verifying 0x40000 feeds __lwp_wkspace_init"
DUMP="$("$OBJDUMP" -dr "$BUILT_LIBOGC_A" 2>&1)" || fail 7 "objdump failed to run on $BUILT_LIBOGC_A"
CALL_LINE="$(printf '%s\n' "$DUMP" | grep -n 'R_PPC_REL24[[:space:]]*__lwp_wkspace_init' | head -1 | cut -d: -f1)"
if [ -z "$CALL_LINE" ]; then
  fail 7 "objdump gate FAILED: no relocation to __lwp_wkspace_init found in $BUILT_LIBOGC_A"
fi
CONTEXT="$(printf '%s\n' "$DUMP" | sed -n "$((CALL_LINE > 40 ? CALL_LINE - 40 : 1)),${CALL_LINE}p")"
GATE_LINE="$(printf '%s\n' "$CONTEXT" | grep -E 'lis[[:space:]]+r3,4$' | tail -1)"
if [ -z "$GATE_LINE" ]; then
  log "----- disassembly context around the __lwp_wkspace_init call -----"
  printf '%s\n' "$CONTEXT" >&2
  fail 7 "objdump gate FAILED: no 'lis r3,4' (0x40000/256KB) feeding the __lwp_wkspace_init call -- KERNEL_HEAP patch did NOT make it into this .a"
fi
log "objdump gate PASSED: $GATE_LINE"

# ---------------------------------------------------------------------------
# 6b. objdump gate (EXPERIMENTAL, hardware-unconfirmed: patch 0003's
#     sd0_initio cold-boot retry). Two checks, most-reliable first, because
#     the naive "grep for the literal 20" gate does NOT work here -- verified
#     empirically: GCC compiles `s32 tries = 20; while(tries-- > 0) {...}` as
#     a do-while with the FIRST iteration unconditional, so the trip-count
#     literal it actually loads is tries-1 = 19 (0x13), never 20. (Likewise
#     the stock/pre-patch code's `tries = 10` compiles to a loaded literal of
#     9.) __sd0_initio itself also fully inlines into sdio_Startup at this
#     optimization level (confirmed: no standalone <__sd0_initio>: symbol),
#     so a function-symbol-scoped search would find nothing either. Both
#     details are compiler/optimization-level artifacts, not something to
#     "fix" in the patch -- the checks below are written to survive either.
#
#     (i) PRIMARY, compiler-idiom-independent: sd0_get_init_attempts() is
#         entirely new code (patch 0003 adds it; nothing else could produce
#         it), so its presence as its own symbol in libogc.a is airtight
#         proof the patched libogc/wiisd.c was actually compiled into this
#         archive -- unaffected by inlining or register allocation.
#     (ii) SECONDARY, reinforcing: within the wiisd.o member's disassembly,
#         anchor on the unmistakable SEND_OP_COND argument constant
#         0x40300000 (its upper 16 bits show as `lis rX,16432`) and look in
#         the preceding instruction window for the `li rX,19` trip-count
#         load described above. If a future toolchain stops inlining
#         __sd0_initio, this same anchor-and-window search still finds it,
#         now inside its own <__sd0_initio>: symbol instead of sdio_Startup's.
# ---------------------------------------------------------------------------
log "objdump gate (experimental, i): verifying sd0_get_init_attempts() accessor symbol exists"
# NOTE: deliberately `grep -c` (reads to EOF) into a captured variable, not
# `grep -q` in a checked pipeline -- `grep -q` exits the instant it finds a
# match, and on a multi-MB objdump `$DUMP` that early exit can SIGPIPE the
# upstream `printf` before it finishes writing, which `set -o pipefail`
# (active at the top of this script) then reports as pipeline failure even
# though the match WAS found. Reproduced empirically while authoring this
# gate. `grep -c`/`grep -n` (used throughout this script, including the
# KERNEL_HEAP gate above) never early-exits, so they don't have this trap.
SD0_ACCESSOR_COUNT="$(printf '%s\n' "$DUMP" | grep -c '<sd0_get_init_attempts>:' || true)"
if [ -z "$SD0_ACCESSOR_COUNT" ] || [ "$SD0_ACCESSOR_COUNT" = "0" ]; then
  fail 7 "objdump gate FAILED (experimental): sd0_get_init_attempts symbol not found in $BUILT_LIBOGC_A -- patch 0003 did NOT make it into this .a"
fi
log "objdump gate PASSED (experimental, i): sd0_get_init_attempts symbol present"

log "objdump gate (experimental, ii): verifying widened ACMD41 retry trip-count (li rX,19, i.e. tries=20-1) near the SEND_OP_COND (0x40300000) call site"
WIISD_START="$(printf '%s\n' "$DUMP" | grep -n '^wiisd\.o:' | head -1 | cut -d: -f1)"
if [ -z "$WIISD_START" ]; then
  fail 7 "objdump gate FAILED (experimental): wiisd.o member not found in the disassembly of $BUILT_LIBOGC_A"
fi
WIISD_END="$(printf '%s\n' "$DUMP" | awk -v s="$WIISD_START" 'NR>s && /^[A-Za-z0-9_.]+\.o:[[:space:]]*file format/{print NR; exit}')"
[ -n "$WIISD_END" ] || WIISD_END="$(printf '%s\n' "$DUMP" | wc -l)"
WIISD_BODY="$(printf '%s\n' "$DUMP" | sed -n "${WIISD_START},${WIISD_END}p")"
ANCHOR_LINE="$(printf '%s\n' "$WIISD_BODY" | grep -n 'lis[[:space:]]*r[0-9]*,16432' | head -1 | cut -d: -f1)"
if [ -z "$ANCHOR_LINE" ]; then
  fail 7 "objdump gate FAILED (experimental): SEND_OP_COND 0x40300000 argument constant (lis rX,16432) not found inside wiisd.o -- cannot locate the retry loop to verify"
fi
ANCHOR_CONTEXT="$(printf '%s\n' "$WIISD_BODY" | sed -n "$((ANCHOR_LINE > 60 ? ANCHOR_LINE - 60 : 1)),${ANCHOR_LINE}p")"
SD0_RETRY_LINE="$(printf '%s\n' "$ANCHOR_CONTEXT" | grep -E 'li[[:space:]]+r[0-9]+,19$' | tail -1)"
if [ -z "$SD0_RETRY_LINE" ]; then
  log "----- disassembly context around the SEND_OP_COND call site (experimental gate) -----"
  printf '%s\n' "$ANCHOR_CONTEXT" >&2
  fail 7 "objdump gate FAILED (experimental): no 'li rX,19' (the compiled trip-count for tries=20) found near the SEND_OP_COND call site -- the widened retry loop did NOT make it into this .a (a stock/pre-patch build shows 'li rX,9' at the equivalent spot)"
fi
log "objdump gate PASSED (experimental, ii): $SD0_RETRY_LINE"

# ---------------------------------------------------------------------------
# 7. mtime-advance check: the built .a must be newer than this run's start.
# ---------------------------------------------------------------------------
A_MTIME="$(stat -c %Y "$BUILT_LIBOGC_A" 2>/dev/null || stat -f %m "$BUILT_LIBOGC_A")"
if [ -z "$A_MTIME" ] || [ "$A_MTIME" -lt "$BUILD_START_EPOCH" ]; then
  fail 8 "mtime-advance check FAILED: $BUILT_LIBOGC_A mtime ($A_MTIME) is not newer than build start ($BUILD_START_EPOCH) -- the build did nothing"
fi
log "mtime-advance check PASSED: $BUILT_LIBOGC_A mtime $A_MTIME >= build start $BUILD_START_EPOCH"

# ---------------------------------------------------------------------------
# 8. Assemble stage-lib: official devkitPro lib/wii/*.a as the baseline
#    (covers libfat.a and libgxflux.a, which are NOT part of the libogc
#    source tree catnip builds -- verified by md5 against the official
#    install), with every catnip-built sibling swapped for our patched copy.
# ---------------------------------------------------------------------------
OFFICIAL_LIB_DIR="$DEVKITPRO_HOST_PATH/libogc/lib/wii"
[ -d "$OFFICIAL_LIB_DIR" ] || fail 9 "official devkitPro lib dir not found: $OFFICIAL_LIB_DIR"

STAGE_DIR="$OUT_DIR/stage-lib"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"
cp "$OFFICIAL_LIB_DIR"/*.a "$STAGE_DIR/" || fail 9 "could not seed stage-lib from $OFFICIAL_LIB_DIR"

# pkg-dir:artifact-name pairs, exactly matching catnip's build/libogc.wii/*/*.a layout
BUILT_PAIRS="
libaesnd:libaesnd.a
libasnd:libasnd.a
libdb:libdb.a
libdi:libdi.a
libiso9660:libiso9660.a
libmad:libmad.a
libmodplay:libmodplay.a
libogc:libogc.a
libtinysmb:libtinysmb.a
libwiikeyboard:libwiikeyboard.a
lwbt:libbte.a
wiiuse:libwiiuse.a
"
REBUILT_COUNT=0
for PAIR in $BUILT_PAIRS; do
  PKG_DIR="${PAIR%%:*}"
  A_NAME="${PAIR##*:}"
  SRC="$OUT_DIR/build/libogc.wii/$PKG_DIR/$A_NAME"
  [ -f "$SRC" ] || fail 9 "expected built artifact missing: $SRC"
  cp "$SRC" "$STAGE_DIR/$A_NAME" || fail 9 "could not stage $A_NAME"
  REBUILT_COUNT=$((REBUILT_COUNT + 1))
done

STAGE_COUNT="$(ls -1 "$STAGE_DIR"/*.a 2>/dev/null | wc -l)"
log "stage-lib assembled: $STAGE_DIR ($STAGE_COUNT .a files, $REBUILT_COUNT rebuilt/patched, $((STAGE_COUNT - REBUILT_COUNT)) copied unmodified from official)"

# ---------------------------------------------------------------------------
# 9. Marker + summary.
# ---------------------------------------------------------------------------
{
  echo "built_by=tools/build_libogc_sidecar.sh"
  echo "built_at_epoch=$(date +%s)"
  echo "libogc_ref=$RESOLVED_DESC"
  echo "libogc_hash=$RESOLVED_HASH"
  echo "libogc_src=$CLONE_FROM"
} > "$OUT_DIR/$MARKER"

log "DONE. stage-lib ready: $STAGE_DIR"
log "next: make DEVKITPPC=\$DEVKITPPC DEVKITPRO=\$DEVKITPRO LIBOGC_LIB=$STAGE_DIR"
exit 0
