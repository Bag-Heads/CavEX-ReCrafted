#!/usr/bin/env bash
#
# CI / local check entry point (Linux: GitHub ubuntu-latest or WSL).
# Two gates, in order:
#   1. the complete CTest faithfulness/equivalence/tooling suite
#   2. the AddressSanitizer streaming soak -- builds soak_stream with ASan+UBSan and drives the
#      real generate->mesh->evict->remesh loop; a heap overrun / use-after-free anywhere in the
#      shared sim/mesher/server/entity surface aborts the run (non-zero exit) with a precise
#      stack, gating the change.
#
# Mirrors .github/workflows/ci.yml so `bash tools/ci.sh` reproduces CI locally.
# Env: CI_SOAK_ITERS (default 8000; nightly CI passes a big number for a thorough soak).
#
# NOTE: the ASan stage needs Linux (ASan does not link on MSYS2/mingw). The test stage is
# platform-agnostic but this script targets a Linux shell.

set -euo pipefail

# WSL appends the Windows PATH by default. CMake's FindGLEW probes config
# packages before its native library search, so an MSYS GLEW config under
# /mnt/c can inject Windows UCRT headers into a Linux build. CI needs one
# platform's toolchain at a time; retain the native WSL paths only.
if grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; then
  _cw_native_path=""
  IFS=: read -r -a _cw_path_parts <<< "$PATH"
  for _cw_path_part in "${_cw_path_parts[@]}"; do
    case "$_cw_path_part" in
      /mnt/[a-zA-Z]/*) ;;
      *) _cw_native_path="${_cw_native_path:+${_cw_native_path}:}${_cw_path_part}" ;;
    esac
  done
  PATH="$_cw_native_path"
  export PATH
fi
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SOAK_ITERS="${CI_SOAK_ITERS:-8000}"
# Build dirs are overridable so a local cross-toolchain run (e.g. tools/local_ci.sh driving this
# under WSL) can use dirs SEPARATE from a Windows/MSYS `build-verify` -- a CMakeCache generated with
# Windows paths (C:/...) is rejected when reused under Linux (/mnt/c/...), and vice versa. GitHub CI
# checks out fresh, so the defaults are unchanged there.
VERIFY_DIR="${CI_VERIFY_DIR:-build-verify}"
ASAN_DIR="${CI_ASAN_DIR:-build-asan}"
read -r -a CMAKE_ARGS <<< "${CI_CMAKE_ARGS:-}"

# GPU upload and Tk tests need an actual display even when their windows are
# hidden. Ubuntu CI has none; run the full suite on one software X server.
CTEST_DISPLAY=()
if [[ -z "${DISPLAY:-}" ]]; then
  command -v xvfb-run >/dev/null || {
    echo "FATAL: headless tests require xvfb-run (install xvfb and xauth)" >&2
    exit 1
  }
  CTEST_DISPLAY=(xvfb-run -a)
fi

echo "::group::build + complete CTest faithfulness suite"
cmake -B "$VERIFY_DIR" -S . -G Ninja -DBUILD_TESTING=ON "${CMAKE_ARGS[@]}"
cmake --build "$VERIFY_DIR"
TEST_COUNT="$(ctest --test-dir "$VERIFY_DIR" -N | awk '/^Total Tests:/ {print $3}')"
if [[ ! "$TEST_COUNT" =~ ^[1-9][0-9]*$ ]]; then
  echo "FATAL: CTest registered no tests in $VERIFY_DIR" >&2
  exit 1
fi
echo "configured CTest inventory: $TEST_COUNT tests"
# Known-fail accounting (docs/build_conventions.md "Known-fail tests"): the
# gate excludes ONLY tests labelled known_fail (each named by a ruling and
# owned by a parity card), then runs them separately and reports them as
# KNOWN FAIL. A known_fail test that passes is reported too (retire it by an
# explicit acceptance ruling); it never fails or passes the gate silently.
KNOWN_FAIL_COUNT="$(ctest --test-dir "$VERIFY_DIR" -N -L known_fail | awk '/^Total Tests:/ {print $3}')"
echo "gated: $((TEST_COUNT - KNOWN_FAIL_COUNT)) tests; known_fail (reported separately): $KNOWN_FAIL_COUNT"
# Capture the gated status instead of letting set -e exit here: the
# known_fail set must ALWAYS be run and reported, even when the gate fails.
GATED_RC=0
# Parallel: timing tests are RUN_SERIAL and heavy ones share RESOURCE_LOCK
# cw_heavy (CMakeLists "Parallel ctest isolation"). CW_CTEST_JOBS overrides.
"${CTEST_DISPLAY[@]}" ctest --test-dir "$VERIFY_DIR" -j "${CW_CTEST_JOBS:-4}" --output-on-failure -LE known_fail || GATED_RC=$?
if [[ "$KNOWN_FAIL_COUNT" != "0" ]]; then
  if "${CTEST_DISPLAY[@]}" ctest --test-dir "$VERIFY_DIR" -L known_fail; then
    echo "KNOWN FAIL tests PASSED -- report it; retirement needs an acceptance ruling"
  else
    echo "KNOWN FAIL: $KNOWN_FAIL_COUNT test(s) failing as recorded (not gated)"
  fi
fi
echo "::endgroup::"
if [[ "$GATED_RC" -ne 0 ]]; then
  echo "FATAL: gated ctest failed (exit $GATED_RC)" >&2
  exit "$GATED_RC"
fi

echo "::group::header hygiene (self-contained + include-cycle gate)"
# Step 7 gate: every first-party header compiles standalone and the include
# graph stays acyclic (baseline tools/header_hygiene_baseline.txt is EMPTY
# as of 2026-08-27 -- keep it that way).
python3 tools/check_header_hygiene.py
echo "::endgroup::"

echo "::group::AddressSanitizer soak build"
cmake -B "$ASAN_DIR" -S . -G Ninja -DSANITIZE=ON -DCAVEX_IPO=OFF \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF
cmake --build "$ASAN_DIR" --target soak_stream
echo "::endgroup::"

echo "::group::AddressSanitizer streaming soak ($SOAK_ITERS iterations)"
export ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:detect_leaks=0:detect_stack_use_after_return=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
# stdout = the TPS debug spam (server_local.c) -> discard; stderr = soak progress + any report.
# set -e + ASan's abort_on_error => a sanitizer hit fails this script (and CI).
"./$ASAN_DIR/soak_stream" "$SOAK_ITERS" >/dev/null
echo "soak clean: no ASan/UBSan hit over $SOAK_ITERS iterations"
echo "::endgroup::"

echo "CI OK"
