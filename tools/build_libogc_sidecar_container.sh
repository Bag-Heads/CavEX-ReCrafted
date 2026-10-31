#!/usr/bin/env bash
# Linux/devkitPro-container companion to build_libogc_sidecar.sh.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-/tmp/cavewii-libogc-sidecar}"
ref=v3.1.0
commit=c70bdf2fc36bf6c6c12d946ae3ad70d604366d1b
upstream=https://github.com/devkitPro/libogc.git
patch_dir="$repo/third_party/libogc-kernel-heap"
# devkitpro/devkitppc ships the cross binutils in $DEVKITPPC/bin but only puts
# /opt/devkitpro/tools/bin on PATH; the objdump gates below need powerpc-eabi-*.
: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITPPC:=$DEVKITPRO/devkitPPC}"
export DEVKITPRO DEVKITPPC
export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"
for tool in git catnip cmake powerpc-eabi-objdump sha256sum; do
  command -v "$tool" >/dev/null || { echo "[sidecar] FATAL: missing $tool" >&2; exit 2; }
done
case "$out" in /|/tmp|"$repo"|"$repo"/*) echo "[sidecar] FATAL: unsafe output path: $out" >&2; exit 2;; esac
if [ -e "$out" ]; then
  [ -f "$out/.sidecar-build-marker" ] || { echo "[sidecar] FATAL: refusing unmarked output: $out" >&2; exit 2; }
  rm -rf -- "$out"
fi
echo "[sidecar] source: $upstream $ref $commit"
git clone --branch "$ref" --single-branch "$upstream" "$out"
actual="$(git -C "$out" rev-parse HEAD)"
[ "$actual" = "$commit" ] || { echo "[sidecar] FATAL: ref resolved to $actual, expected $commit" >&2; exit 3; }
echo "built_by=tools/build_libogc_sidecar.sh --container" > "$out/.sidecar-build-marker"
# Pinned patch set (G1 adversarial review F7): verify every patch against its
# recorded sha256 BEFORE applying anything; any mismatch, missing or extra
# patch fails closed. .gitattributes pins these files to eol=lf, so the hashes
# are the same on a Windows checkout and in this Linux container. Re-pin only
# together with a reviewed patch change.
expected_patches="f731fd74f8bb67a65d58e9882244070d67154c15ffda77ca7bd83afb202a6fef 0001-lwp-kernel-heap-64k-to-256k.patch
87b5e24f912a29b669561efcc7e51106456db1b6a406c5db1e27214c49ff70b4 0002-catnip-drop-cube-preset-host-has-no-gamecube-toolset.patch
87ffce3d61196410c4ce53dc955ddd9623eeb92814bf4abf5ba494ad6f168219 0003-wiisd-sd0-initio-cold-boot-retry.patch
209b27fd0fa188342bb93c17da2328bd39eb2fff6acfa9c7033eefb05dcc6bb8 0004-tuxedo-clear-lazy-fpu-owner-on-thread-exit.patch
183f3667efc01e7bd86fedebc35d9a101d1f5bc4ce6ea87e0b786c9c6d8d53b4 0005-wiisd-sd0-functions-align64.patch"
actual_names="$(cd "$patch_dir" && ls 000[1-9]-*.patch 2>/dev/null | sort | tr '\n' ' ')"
wanted_names="$(printf '%s\n' "$expected_patches" | awk '{print $2}' | sort | tr '\n' ' ')"
[ "$actual_names" = "$wanted_names" ] || { echo "[sidecar] FATAL: patch set mismatch: have [$actual_names] want [$wanted_names]" >&2; exit 4; }
while read -r want name; do
  got="$(sha256sum "$patch_dir/$name" | cut -d' ' -f1)"
  [ "$got" = "$want" ] || { echo "[sidecar] FATAL: $name sha256 $got != pinned $want" >&2; exit 4; }
done <<EOF_PATCHES
$expected_patches
EOF_PATCHES
echo '[sidecar] patch sha256 pins verified (5/5)'
# Applied WITHOUT whitespace rewriting, so the applied bytes are exactly the
# hashed patch bytes. The one "trailing whitespace" warning git prints is the
# CONTEXT line 55 of 0003 (a tab-only line in upstream wiisd.c), not an added
# line: applying 0001-0005 with --whitespace=fix and with --whitespace=nowarn
# to c70bdf2f gives byte-identical trees (verified 2026-09-25), so this is
# equivalent to the host wrapper's --whitespace=fix application.
while read -r want name; do
  git -C "$out" apply --whitespace=nowarn "$patch_dir/$name"
done <<EOF_PATCHES
$expected_patches
EOF_PATCHES
grep -q '#define KERNEL_HEAP.*256\*1024' "$out/libogc/system.c"
grep -q '__sd0_functions\[64\] ATTRIBUTE_ALIGN(64)' "$out/libogc/wiisd.c"
grep -q 'tries = 20;' "$out/libogc/wiisd.c"
grep -q 'PPCMfspr(SPRG3) == (u32)self' "$out/tuxedo/thread.c"
echo '[sidecar] building patched Wii libraries with catnip'
(cd "$out" && catnip)
archive="$out/build/libogc.wii/libogc/libogc.a"
[ -s "$archive" ] || { echo '[sidecar] FATAL: no built libogc.a' >&2; exit 5; }
dump="$out/libogc.objdump"
powerpc-eabi-objdump -dr "$archive" > "$dump"
call_line="$(awk '/R_PPC_REL24[[:space:]]*__lwp_wkspace_init/ { print NR; exit }' "$dump")"
[ -n "$call_line" ] && sed -n "$((call_line > 40 ? call_line - 40 : 1)),${call_line}p" "$dump" | grep -Eq 'lis[[:space:]]+r3,4$' || { echo '[sidecar] FATAL: 256KB kernel-heap objdump gate failed' >&2; exit 6; }
grep -q '<sd0_get_init_attempts>:' "$dump" || { echo '[sidecar] FATAL: sd0 retry accessor missing' >&2; exit 6; }
awk '/^wiisd\.o:/{inside=1; next} inside && /^[A-Za-z0-9_.]+\.o:[[:space:]]*file format/{exit} inside {print}' "$dump" > "$out/wiisd.objdump"
anchor="$(awk '/lis[[:space:]]*r[0-9]+,16432/ { print NR; exit }' "$out/wiisd.objdump")"
[ -n "$anchor" ] && sed -n "$((anchor > 60 ? anchor - 60 : 1)),${anchor}p" "$out/wiisd.objdump" | grep -Eq 'li[[:space:]]+r[0-9]+,19$' || { echo '[sidecar] FATAL: sd0 retry trip-count objdump gate failed' >&2; exit 6; }
echo '[sidecar] objdump gates PASS: kernel heap 0x40000; sd0 retry accessor and trip-count'
stage="$out/stage-lib"
mkdir -p "$stage"
cp "$DEVKITPRO"/libogc/lib/wii/*.a "$stage/"
for pair in libaesnd:libaesnd libasnd:libasnd libdb:libdb libdi:libdi libiso9660:libiso9660 libmad:libmad libmodplay:libmodplay libogc:libogc libtinysmb:libtinysmb libwiikeyboard:libwiikeyboard lwbt:libbte wiiuse:libwiiuse; do
  pkg="${pair%%:*}"; name="${pair#*:}"
  src="$out/build/libogc.wii/$pkg/$name.a"
  [ -s "$src" ] || { echo "[sidecar] FATAL: missing $src" >&2; exit 7; }
  cp "$src" "$stage/$name.a"
done
{
  echo "libogc_ref=$ref"
  echo "libogc_commit=$commit"
  echo "patch_sha256_pins=verified (5/5, applied with --whitespace=nowarn)"
  for patch in "$patch_dir"/000[1-5]-*.patch; do sha256sum "$patch"; done
  echo "libogc_archive_sha256=$(sha256sum "$stage/libogc.a" | cut -d' ' -f1)"
} | tee "$stage/.sidecar-provenance"
echo "[sidecar] PASS: $stage"
