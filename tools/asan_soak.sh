#!/usr/bin/env bash
#
# Phase-0 detection layer: build + run the headless streaming soak under AddressSanitizer.
#
# WHY: the real-Wii freeze is a heap overrun / use-after-free that the no-MMU console silently
# tolerates until it wedges minutes later in an unrelated subsystem (it took ~10 hardware builds
# to localize). ASan does NOT link on the MSYS2/mingw toolchain, so this runs under WSL/Linux
# gcc, where ASan wraps every allocation in redzones and reports the bad access at the moment it
# happens, with a precise stack. The soak drives the SAME generate->mesh->evict->remesh loop the
# game uses (server + mesher + main threads sharing the heaps) over thousands of chunk crossings.
#
# RUN (from a WSL/Ubuntu shell, or `wsl bash tools/asan_soak.sh` from Windows):
#     bash tools/asan_soak.sh [iterations] [seed]
# First run installs the toolchain via apt (asks for your sudo password once).
#
# Caveat: platform/wii/displaylist.c (the GX byte-packing) is NOT compiled on PC, so a bug purely
# there isn't covered. The shared sim/mesher/server/entity surface -- the bulk, and the assessed
# prime suspects (cross-thread display-list + entity heap churn) -- IS.

set -euo pipefail

# Default ~100k crossings-worth of churn (~15-30 min; worldgen-bound). Pass a smaller number
# for a quick check or a bigger one for an overnight soak. The freeze shows after 2-5 min of
# real walking, so even 20000 here is a meaningful hunt.
ITERS="${1:-100000}"
SEED="${2:-20260608}"

# repo root = parent of this script's dir (tools/)
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
echo "[asan_soak] repo root: $ROOT"

# ---- 1. toolchain (idempotent: only apt-installs what's missing) ----
NEED=()
command -v gcc    >/dev/null 2>&1 || NEED+=(build-essential)
command -v cmake  >/dev/null 2>&1 || NEED+=(cmake)
command -v ninja  >/dev/null 2>&1 || NEED+=(ninja-build)
[ -f /usr/include/zlib.h ]        || NEED+=(zlib1g-dev)
# soak_stream links glfw/GLEW/GL (it never initialises them, but must link); these pull the
# X11/GL dev chain transitively. Needed only to LINK -- the run is headless (no display).
[ -f /usr/include/GLFW/glfw3.h ]  || NEED+=(libglfw3-dev)
[ -f /usr/include/GL/glew.h ]     || NEED+=(libglew-dev)
[ -f /usr/include/GL/gl.h ]       || NEED+=(libgl1-mesa-dev libglu1-mesa-dev)

if [ "${#NEED[@]}" -gt 0 ]; then
	echo "[asan_soak] installing: ${NEED[*]}"
	sudo apt-get update -y
	sudo apt-get install -y "${NEED[@]}"
else
	echo "[asan_soak] toolchain present"
fi

# ---- 2. configure a dedicated ASan build dir (LTO off for clean traces) ----
BUILD="build-asan"
echo "[asan_soak] configuring $BUILD (-DSANITIZE=ON)"
cmake -B "$BUILD" -S . -G Ninja -DSANITIZE=ON -DCAVEX_IPO=OFF -DCMAKE_BUILD_TYPE=Debug

# ---- 3. build the soak harness ----
echo "[asan_soak] building soak_stream"
cmake --build "$BUILD" --target soak_stream

# ---- 4. run it from the repo root (so config_pc.json resolves) ----
#   stdout -> /dev/null  (server_local.c:2067 prints TPS every cycle; pure noise here)
#   stderr -> the log    (the soak progress AND the ASan/UBSan report land here)
LOG="$ROOT/asan_soak_report.txt"
echo "[asan_soak] running: $ITERS iterations, seed $SEED"
echo "[asan_soak] stderr (incl. any ASan report) -> $LOG"
# detect_leaks=0: the harness can't join the detached server/mesher threads at exit (no
# thread_join is exposed), so the still-live world would show as false-positive "leaks". The
# TARGET -- a heap OVERRUN / use-after-free -- is caught mid-run regardless, which is the freeze
# cause. (Flip to detect_leaks=1 later, once a clean shutdown path exists, to also hunt leaks.)
export ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:detect_leaks=0:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

set +e
"./$BUILD/soak_stream" "$ITERS" "$SEED" >/dev/null 2>"$LOG"
RC=$?
set -e

echo "[asan_soak] exit code: $RC"
if grep -qiE "ERROR: AddressSanitizer|runtime error:|LeakSanitizer" "$LOG"; then
	echo "============================================================"
	echo "[asan_soak] *** SANITIZER CAUGHT SOMETHING *** (the real win)"
	echo "  Full report: $LOG"
	echo "  Top of the report:"
	echo "------------------------------------------------------------"
	grep -nE "ERROR: AddressSanitizer|runtime error:|LeakSanitizer|#[0-9]+ 0x|SUMMARY:" "$LOG" | head -40
	echo "============================================================"
elif [ "$RC" -eq 0 ]; then
	echo "[asan_soak] clean run, no sanitizer hits over $ITERS iterations."
	echo "[asan_soak] (tail of log:)"; tail -6 "$LOG"
else
	echo "[asan_soak] non-zero exit with no obvious sanitizer banner -- inspect $LOG:"
	tail -30 "$LOG"
fi
