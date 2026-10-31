#!/usr/bin/env bash
# Build the gated release Wii .dol for a COMMITTED revision while the main
# checkout has uncommitted work in it (parallel loop lanes).
#
# It makes a throwaway `git clone --shared` (objects borrowed from this repo, so
# it costs only the checked-out files), runs the normal gated build there
# (tools/cwbuild.ps1 wii-release -> build-wii md5/build-ID gates), copies the
# .dol out, then deletes the clone. No worktree, no second build-verify, no
# edits ever happen in the clone (maintainer rule 2026-10-12: work happens on
# main; this is a build-only scratch copy).
#
# Usage: bash tools/wii_build_commit.sh [<rev>]       (default HEAD)
# Output: tmp/jobs/wii-builds/<short>.dol + <short>.log, md5 printed.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
rev="${1:-HEAD}"
sha="$(git -C "$repo" rev-parse --verify "$rev^{commit}")"
short="$(git -C "$repo" rev-parse --short=7 "$sha")"
out="$repo/tmp/jobs/wii-builds"
scratch="$repo/tmp/jobs/wii-build-clone-$short"
mkdir -p "$out"

if [ -e "$scratch" ]; then
	echo "[wii_build_commit] stale scratch $scratch exists; remove it first" >&2
	exit 2
fi

cleanup() { rm -rf "$scratch"; }
trap cleanup EXIT

git clone -q --shared --no-checkout "$repo" "$scratch"
git -C "$scratch" checkout -q --detach "$sha"
# The build stamps from git; make sure the clone is clean and on the right commit.
test -z "$(git -C "$scratch" status --porcelain)"
test "$(git -C "$scratch" rev-parse HEAD)" = "$sha"

log="$out/$short.log"
( cd "$scratch" && powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/cwbuild.ps1 wii-release ) >"$log" 2>&1 || {
	echo "[wii_build_commit] build FAILED for $short; see $log" >&2
	tail -5 "$log" >&2
	exit 1
}

cp "$scratch/CaveWii_v2.dol" "$out/$short.dol"
md5="$(md5sum "$out/$short.dol" | cut -d' ' -f1)"
stamp="$(grep -o 'expected F3 stamp[^:]*: [^ ]*' "$log" | tail -1 | sed 's/.*: //')"
echo "[wii_build_commit] OK $out/$short.dol md5=$md5 stamp=$stamp"
