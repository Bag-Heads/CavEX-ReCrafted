#!/usr/bin/env bash
#
# local_ci.sh -- run the SAME two checks GitHub CI runs (.github/workflows/ci.yml),
# locally, BEFORE you push. The point: only push things that actually work.
#
# WHY THIS EXISTS: the CI runs on Linux gcc + an AddressSanitizer/UBSan soak, and on
# the devkitPPC container -- both STRICTER than the MSYS dev build. `ctest` passing on
# MSYS is necessary but NOT sufficient; a whole batch of CI failures (missing
# <stdbool.h>, over-aligned malloc -> AVX fault, signed-shift UB, misaligned vertex
# stores, a hash-multiply overflow) all passed on MSYS and only Linux/UBSan + devkitPPC
# caught them. Run THIS before pushing.
#
#   Job "verify"   -> Linux gcc-13 build + ctest + 8000-iter ASan/UBSan soak (via WSL).
#   Job "wii-build"-> devkitPPC build with the pinned patched libogc sidecar
#                    inside the CI Docker image; CI artifact is never staged.
#
# USAGE:
#   bash tools/local_ci.sh            # both jobs, full 8000-iter soak (what CI does on push)
#   CI_SOAK_ITERS=2000 bash tools/local_ci.sh   # faster soak for a quick gate
#   bash tools/local_ci.sh --verify-only        # skip the (slow) Wii Docker build
#   bash tools/local_ci.sh --wii-only           # only the Wii build
#
# It verifies the CURRENT WORKING TREE (what you're about to commit/push). Exit 0 means
# both jobs are green and it's safe to push; non-zero means STOP and fix.
#
# REQUIREMENTS (this Windows dev host): WSL Ubuntu with the CI apt deps, and Docker
# Desktop running. The script checks for both and tells you what's missing.
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOAK_ITERS="${CI_SOAK_ITERS:-8000}"
DKP_IMAGE="devkitpro/devkitppc:latest@sha256:44cb1a920e1ec3ec7c06767493c3b85f8d643d6137cc4661f0201895ac6e4967"

DO_VERIFY=1
DO_WII=1
case "${1:-}" in
  --verify-only) DO_WII=0 ;;
  --wii-only)    DO_VERIFY=0 ;;
  "" ) ;;
  *) echo "unknown arg: $1 (use --verify-only | --wii-only)"; exit 2 ;;
esac

# Path forms for the different runtimes (repo is on a Windows drive under git-bash).
WSL_PATH="$(printf '%s' "$REPO" | sed -E 's|^/([a-zA-Z])/|/mnt/\L\1/|')"
DOCKER_MOUNT="$(printf '%s' "$REPO" | sed -E 's|^/([a-zA-Z])/|\U\1:/|')"

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
RC_VERIFY="skip"; RC_WII="skip"

# Snapshot the worktree identity BEFORE any job runs (tools/worktree-stamp.sh --
# the SAME dirty-tree-identity recipe build-verify.sh uses for CW_BUILD_ID). The
# verdict below re-checks this against a fresh snapshot taken right before the
# marker is stamped: local_ci.sh can run for many minutes (a WSL soak plus a
# from-scratch devkitPPC build), and if the worktree changed WHILE it ran, every
# job result above was measured against a tree that no longer exists by the time
# we'd certify it.
TREE_START="$(sh "$REPO/tools/worktree-stamp.sh" "$REPO" 2>/dev/null || true)"

# Write the push-gate marker (.claude/hooks/enforce-ci-before-push.py reads it).
# $1=status  $2=tree stamp to record  $3=optional note (may be empty)
write_ci_marker() {
  _gd="$(git rev-parse --git-dir 2>/dev/null)"
  [ -n "$_gd" ] || return 0
  if [ -n "${3:-}" ]; then
    printf '{"head":"%s","status":"%s","verify":"%s","wii":"%s","tree":"%s","note":"%s","when":"%s"}\n' \
      "$(git rev-parse HEAD 2>/dev/null)" "$1" "$RC_VERIFY" "$RC_WII" "$2" "$3" \
      "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)" \
      > "$_gd/cavewii_local_ci_pass" 2>/dev/null || true
  else
    printf '{"head":"%s","status":"%s","verify":"%s","wii":"%s","tree":"%s","when":"%s"}\n' \
      "$(git rev-parse HEAD 2>/dev/null)" "$1" "$RC_VERIFY" "$RC_WII" "$2" \
      "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)" \
      > "$_gd/cavewii_local_ci_pass" 2>/dev/null || true
  fi
}

# ---------------------------------------------------------------- verify (WSL/Linux)
if [ "$DO_VERIFY" = 1 ]; then
  bold "=== [verify] Linux gcc + ctest + ${SOAK_ITERS}-iter ASan/UBSan soak (WSL) ==="
  if ! command -v wsl.exe >/dev/null 2>&1; then
    echo "[verify] FAIL: wsl.exe not found. Install WSL Ubuntu + the CI deps:"
    echo "         sudo apt-get install -y build-essential cmake ninja-build zlib1g-dev \\"
    echo "             libglfw3-dev libglew-dev libgl1-mesa-dev libglu1-mesa-dev"
    RC_VERIFY=127
  else
    # Sanitize PATH so MSYS2 dirs don't leak into the Linux toolchain; run the real ci.sh.
    # Use WSL-specific build dirs (CI_VERIFY_DIR/CI_ASAN_DIR) so this never collides with a
    # Windows/MSYS `build-verify`: a CMakeCache made with C:/ paths is rejected when reused under
    # /mnt/c, and vice versa -- sharing the dir reddened verify with a bogus cache-mismatch error.
    wsl.exe -e bash -lc "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; \
      cd '$WSL_PATH' || exit 3; \
      for p in cmake ninja gcc; do command -v \$p >/dev/null || { echo \"[verify] FAIL: \$p missing in WSL\"; exit 4; }; done; \
      CI_SOAK_ITERS=$SOAK_ITERS CI_VERIFY_DIR=build-ci-linux CI_ASAN_DIR=build-ci-asan bash tools/ci.sh"
    RC_VERIFY=$?
    [ "$RC_VERIFY" = 0 ] && bold "[verify] PASS" || bold "[verify] FAIL (rc=$RC_VERIFY)"
  fi
fi

# ------------------------------------------------------------- wii-build (Docker/devkitPPC)
if [ "$DO_WII" = 1 ]; then
  bold "=== [wii-build] devkitPPC CaveWii_v2-ci.dol (Docker $DKP_IMAGE, patched libogc) ==="
  if ! command -v docker >/dev/null 2>&1 || ! docker ps >/dev/null 2>&1; then
    echo "[wii-build] FAIL: Docker not available/running (start Docker Desktop)."
    RC_WII=127
  else
    # /priv has no .git (excluded below), so CW_GIT_HASH falls back to the
    # Makefile's own "nogit" (Makefile:66 -- `sh tools/worktree-stamp.sh
    # 2>/dev/null || echo nogit`), which is embedded in CW_BUILD_ID same as any
    # other stamp value. This is an accepted, already-designed-for degradation
    # for this CI-gate build only (it never runs against the release .dol path).
    #
    # SAFETY: the real checkout is bind-mounted READ-ONLY (:ro below) -- a prior
    # version ran `rm -rf build *.dol *.elf .buildtmp` straight on the live bind
    # mount, which could delete the repo-root release CaveWii_v2.dol another lane
    # was staging/hardware-testing, and wipe build/ out from under it. Docker
    # enforces the read-only bind at the kernel/mount level, so this is not just
    # "don't write there by convention" -- a write attempt fails outright.
    #
    # Instead: tar-copy the checkout into a private, container-local directory
    # (excluding .git, worktrees, any build-*/tmp/.buildtmp dir, and any stray
    # .dol/.elf so a leftover local artifact can never fool `make`'s up-to-date
    # check; docs/ and tests/ are also excluded -- grep-verified the Wii Makefile
    # never reads either, and together they're ~47MB of dead weight for this
    # job), build there, then copy the produced .dol/.elf OUT to a dedicated
    # read-write mount (tmp/local_ci/ in the real checkout) so the result is
    # inspectable on the host without ever touching the source mount.
    mkdir -p "$REPO/tmp/local_ci"
    rm -f "$REPO/tmp/local_ci/CaveWii_v2-ci.dol" "$REPO/tmp/local_ci/CaveWii_v2-ci.dol.provenance.txt"
    DOCKER_MOUNT_OUT="$(printf '%s' "$REPO/tmp/local_ci" | sed -E 's|^/([a-zA-Z])/|\U\1:/|')"
    MSYS_NO_PATHCONV=1 docker run --rm \
      -v "$DOCKER_MOUNT:/work:ro" \
      -v "$DOCKER_MOUNT_OUT:/out" \
      -w /work "$DKP_IMAGE" bash -c '
      set -u
      set -e
      test -f "$DEVKITPRO/libogc/include/gccore.h"
      command -v catnip
      mkdir -p /priv
      tar -C /work \
        --exclude=./.git --exclude=./.claude/worktrees \
        --exclude=./build --exclude="./build-*" --exclude="./build_*" \
        --exclude=./tmp --exclude=./.buildtmp \
        --exclude=./docs --exclude=./tests \
        --exclude="*.dol" --exclude="*.elf" \
        -cf - . | tar -C /priv -xf -
      cd /priv
      bash tools/build_libogc_sidecar.sh --container /tmp/cavewii-libogc-sidecar
      sidecar=/tmp/cavewii-libogc-sidecar/stage-lib
      test -s "$sidecar/.sidecar-provenance"
      cat "$sidecar/.sidecar-provenance"
      make DEVKITPPC=$DEVKITPRO/devkitPPC DEVKITPRO=$DEVKITPRO LIBOGC_LIB=$sidecar
      test -s CaveWii_v2.dol || { echo "[wii-build] no CaveWii_v2.dol produced"; exit 1; }
      DOL_SIZE=$(wc -c < CaveWii_v2.dol | tr -d " ")
      DOL_MD5=$(md5sum CaveWii_v2.dol | cut -d" " -f1)
      echo "[wii-build] sidecar-linked image: ${DOL_SIZE} bytes, md5 ${DOL_MD5}"
      # NOTE: devkitPPC bakes the absolute build-root path into the binary
      # (see CLAUDE.md, the ".dol md5 is PATH-SENSITIVE" note), and this build
      # ran from the private in-container path /priv -- NOT any host build
      # directory. So this md5 is a CI-GATE artifact identity (comparable
      # run-to-run of THIS script, on THIS commit), never the shippable
      # release artifact from build-wii/build-verify.sh, and never comparable
      # to a release .dol md5.
      # A failed copy-out must fail the job -- silently swallowing it would
      # leave RC_WII=0 (job "passed") with nothing landing in tmp/local_ci/,
      # so a caller trusting "PASS means the artifact is there" would be wrong.
      cp CaveWii_v2.dol /out/CaveWii_v2-ci.dol || { echo "[wii-build] copy-out failed"; exit 1; }
      cp "$sidecar/.sidecar-provenance" /out/CaveWii_v2-ci.dol.provenance.txt
      # tools/check_wii_stack_usage.sh derives its own REPO from its own path
      # and does its own from-scratch `make dev` build -- running it from /priv
      # keeps that build private too; it never touches the host bind mount.
      MAKEFLAGS="LIBOGC_LIB=$sidecar" bash tools/check_wii_stack_usage.sh'
    RC_WII=$?
    if [ "$RC_WII" = 0 ] && [ -f "$REPO/tmp/local_ci/CaveWii_v2-ci.dol" ]; then
      echo "[wii-build] copied CI-only image to tmp/local_ci/: $(md5sum "$REPO/tmp/local_ci/CaveWii_v2-ci.dol" | cut -d' ' -f1)  $(wc -c < "$REPO/tmp/local_ci/CaveWii_v2-ci.dol" | tr -d ' ') bytes"
    fi
    [ "$RC_WII" = 0 ] && bold "[wii-build] PASS" || bold "[wii-build] FAIL (rc=$RC_WII)"
  fi
fi

# ----------------------------------------------------------------------------- verdict
echo
bold "=== local_ci summary ==="
echo "  verify   : $RC_VERIFY"
echo "  wii-build: $RC_WII"
# A FULL run (both jobs actually ran, both rc 0) is the only thing that may be
# stamped GREEN -- the status the push-gate hook (.claude/hooks/enforce-ci-
# before-push.py) trusts as "safe to push". `--verify-only`/`--wii-only`
# deliberately skip one job for a faster local iteration loop; that is a
# legitimate partial check, but it must never be reported -- on the console OR
# in the marker -- as the full green the standing push directive means. A
# skipped half is stamped PARTIAL instead, carrying the same verify/wii result
# fields, so the hook can name exactly what was skipped.
FULL_RUN=0
[ "$DO_VERIFY" = 1 ] && [ "$DO_WII" = 1 ] && FULL_RUN=1
if { [ "$DO_VERIFY" = 0 ] || [ "$RC_VERIFY" = 0 ]; } && { [ "$DO_WII" = 0 ] || [ "$RC_WII" = 0 ]; }; then
  # Recompute the worktree identity NOW, right before certifying anything, and
  # compare it against the TREE_START snapshot taken before the first job ran.
  # This check runs BEFORE the normal GREEN/PARTIAL split and always wins: if
  # the worktree changed DURING the run (more edits landed while the WSL soak
  # or the devkitPPC build was still going), every result above was measured
  # against a tree that no longer exists, and NOTHING here can be certified --
  # not even the softer PARTIAL-exit-0 the skipped-job case gets.
  TREE_END="$(sh "$REPO/tools/worktree-stamp.sh" "$REPO" 2>/dev/null || true)"
  if [ "$TREE_START" != "$TREE_END" ]; then
    NOTE="tree changed during run"
    bold "PARTIAL -- ${NOTE} (was '${TREE_START:-<unknown>}', now '${TREE_END:-<unknown>}'). The tested tree no longer exists; nothing above can be certified."
    write_ci_marker "PARTIAL" "$TREE_END" "$NOTE"
    exit 1
  fi
  if [ "$FULL_RUN" = 1 ]; then
    STATUS="GREEN"
    bold "GREEN -- safe to push."
  else
    STATUS="PARTIAL"
    SKIPPED=""
    [ "$DO_VERIFY" = 0 ] && SKIPPED="verify"
    [ "$DO_WII" = 0 ] && SKIPPED="${SKIPPED:+$SKIPPED+}wii-build"
    bold "PARTIAL -- ${SKIPPED} skipped. NOT a full green; do not treat this as safe-to-push."
  fi
  # Stamp the marker the push-gate hook reads (.claude/hooks/enforce-ci-before-push.py):
  # records the HEAD that passed, AND a digest of the EXACT worktree tested --
  # not just HEAD. local_ci.sh verifies the current, possibly DIRTY, working
  # tree; binding the marker to HEAD alone would let it keep certifying a push
  # after the worktree changed again (more edits, or a reset) without HEAD
  # moving. Reuse -- do not reinvent -- the same dirty-tree-identity recipe
  # build-verify.sh already uses for CW_BUILD_ID (tools/worktree-stamp.sh): a
  # clean tree stamps just the short HEAD; a dirty one appends a digest of the
  # complete staged-equivalent worktree (tracked edits/deletes + every
  # non-ignored untracked file). The hook recomputes this at push time via the
  # same script and requires an exact string match.
  write_ci_marker "$STATUS" "$TREE_END"
  exit 0
fi
bold "RED -- do NOT push; fix the failing job above."
exit 1
