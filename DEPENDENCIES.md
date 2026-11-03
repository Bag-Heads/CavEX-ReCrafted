# CaveWii dependency licence inventory (R18)

This is an inventory, not legal clearance. The machine copy is
`tools/release-dependencies.json` (schema 1 of `tools/release_scan.py`). Both files
must agree; `tmp/jobs/release-plan/REL-18e3/validate_inventory.py` checks that.
Facts come from the in-tree READMEs, `PATCHES.md` files and `NOTICE`, and from the
local devkitPro / MSYS2 pacman metadata read on 2026-10-20. A licence that could
not be confirmed is recorded as blocked. It is never borrowed from the project
licence.

## R18 status: licence texts complete (2026-10-30)

Every component linked into `boot.dol` now has its licence text in `LICENSES/` and an
entry in `NOTICE`. Sources, all recorded on 2026-10-30:

- libogc: libbte (lwBT-derived Bluetooth stack). BSD-3-Clause notices of EISLAB (2003) and Adam Dunkels (2001-2003), copied from the libogc v3.1.0 sources (devkitPro/libogc @ c70bdf2f, the clone `tools/build_libogc_sidecar.sh` builds from) to `LICENSES/libbte.txt`.
- libogc: libwiiuse. GPL-3.0-or-later (wiiuse file headers, Michael Laforest 2006-2007) with libogc's linking exception; `wiiuse/license_libogc.txt` from the same libogc sources, unchanged, in `LICENSES/wiiuse.txt`. libogc-written files such as `wpad.c` are under `LICENSES/libogc.txt`.
- libfat (libfat-ogc 2.1.0 = devkitPro/libdvm v2.1.0, plus FatFs). ZPL-2.1 text from https://github.com/devkitPro/libdvm/blob/v2.1.0/COPYING and the FatFs notice from https://github.com/devkitPro/fatfs-mod (dkp-mods @ 6d3e8f89, `source/ff.c`), fetched 2026-10-30, in `LICENSES/libfat.txt`. The installed `fat.h` is byte-identical to libdvm v2.1.0's.
- newlib (devkitPPC libc/libm/libsysbase). COPYING.NEWLIB from https://sourceware.org/git/?p=newlib-cygwin.git;a=blob_plain;f=COPYING.NEWLIB (identical at tag newlib-4.6.0 and HEAD b097f952), fetched 2026-10-30, in `LICENSES/newlib.txt`.
- libgcc (devkitPPC GCC runtime). GPL-3.0-or-later WITH GCC-exception-3.1 (maintainer Q6, 2026-10-30). `LICENSES/GCC-exception-3.1.txt` is byte-identical to https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-15/COPYING.RUNTIME (fetched 2026-10-30; gcc.gnu.org returned HTTP 429) and to the local MSYS2 gcc-libs copy.
- zlib (ppc-zlib 1.3.1). `LICENSES/zlib.txt` is the header of the installed devkitPro `zlib.h` 1.3.1.

Rechecked for 0.1.1 on 2026-11-01 (ci-1101/21): the existing texts above
are present; the old REL-18e3 PARKING_LOT entry was stale. Local source
checks used `../logc256kr/lwbt/{hci.c,bt.h}` at c70bdf2f,
`tmp/jobs/freeze-deep-research-0929/libdvm-v2.1.0/COPYING` at 3006b937,
and `fatfs-v0.15.3/source/ff.c` at 6d3e8f89 in that same job directory.
Both libbte notices, the complete ZPL text and the FatFs notice match the
bundled texts (comment markers/whitespace ignored for libbte). The installed
`C:/msys64/opt/devkitpro/libogc/include/fat.h` matches that libdvm source;
`portlibs/ppc/include/zlib.h` identifies 1.3.1 and matches the bundled licence
notice. The omitted zlib header paragraph describes the compression format,
not licence terms.

The installed devkitPPC `_newlib_version.h` identifies 4.6.0, and GCC
15.2.0's `include/unwind.h` declares GPLv3-or-later with Exception 3.1.
Neither toolchain source COPYING file is installed. Fresh downloads of
upstream newlib-4.6.0 `COPYING.NEWLIB` and GCC releases/gcc-15.2.0
`COPYING.RUNTIME` match `LICENSES/newlib.txt` and
`LICENSES/GCC-exception-3.1.txt` byte-for-byte after LF normalization.
This distinguishes installed-header evidence from upstream full-text evidence;
it is not a new audit of every linked newlib archive member.
The export allowlist pins these notices and their licence texts for 0.1.1.

## Redistributed components

"Source" means the file ships in the clean source export. "boot.dol" means it is
compiled or linked into `apps/CavEX/boot.dol` in the release zip. Exact export
paths and sha256 hashes (of git blob bytes) for every vendored file, 251 files in
total, are listed in the JSON.

| Component | Version / origin | Licence | Licence text | Ships as | Modifications |
|---|---|---|---|---|---|
| CavEX / fCavEX (upstream base project) | fCavEX 0.3.0_f1 lineage; xtreme8000/CavEX -> yyy257/ByteBit fCavEX -> jilleb/fCavEX | GPL-3.0-or-later | `LICENSE` | source, boot.dol | CaveWii_v2 is a modified fork; file headers kept |
| cglm | 0.9.6, recp/cglm @ 490d0bb9 | MIT | `LICENSES/cglm.txt`, `source/cglm/LICENSE.cglm` | source, boot.dol | none recorded (`source/cglm/`, 189 files) |
| cNBT (subset) | chmod222/cNBT @ 3ff982ef | Beerware (Revision 42) | `LICENSES/cNBT.txt` | source, boot.dol | subset only (`source/cNBT/`, 10 files) |
| lodepng | 20260119, lvandeve/lodepng @ 22561883 | Zlib | `LICENSES/lodepng.txt`, `source/lodepng/LICENSE.lodepng` | source, boot.dol | `lodepng.cpp` renamed `lodepng.c` |
| parson | 1.5.3, kgabis/parson @ ba29f4ed | MIT | `LICENSES/parson.txt`, `source/parson/LICENSE.parson` | source, boot.dol | none recorded |
| M*LIB (m-lib) | 0.8.1, P-p-H-d/mlib @ d0fd65a8 | BSD-2-Clause | `LICENSES/m-lib.txt`, `third_party/m-lib/LICENSE.mlib` | source, boot.dol | two OOM patches in `m-dict.h` (`third_party/m-lib/PATCHES.md`) |
| stb_vorbis | 1.22, nothings/stb | Unlicense (chosen; dual-licensed with MIT) | `LICENSES/stb-vorbis.txt` | source, boot.dol | none (pristine) |
| miniaudio | 0.11.25, mackron/miniaudio | Unlicense (chosen; dual-licensed with MIT-0) | `LICENSES/miniaudio.txt` | source only (PC backend) | none (pristine) |
| greatest | 1.5.0, silentbicycle/greatest | ISC | `LICENSES/greatest.txt`, `third_party/greatest/LICENSE` | source only (test binaries) | none recorded |
| libogc sidecar patch set (CaveWii-authored diffs) | 0001-0005 against libogc v3.1.0 | GPL-3.0-or-later (diff text, per NOTICE) applied to Zlib AND BSD-2-Clause libogc code | `LICENSE`, `LICENSES/libogc.txt` | source | it is the modification record for libogc |
| libogc (sidecar-patched: libogc, libaesnd) | 3.1.0, devkitPro/libogc @ c70bdf2f + patches 0001-0005 | Zlib AND BSD-2-Clause | `LICENSES/libogc.txt` (matches the installed libogc 3.1.0 LICENSE) | boot.dol | altered version, marked in `third_party/libogc-kernel-heap/` |
| libogc: libbte (lwBT-derived Bluetooth stack) | libogc 3.1.0 | BSD-3-Clause | `LICENSES/libbte.txt` | boot.dol | none by CaveWii |
| libogc: libwiiuse | libogc 3.1.0 | GPL-3.0-or-later with libogc linking exception | `LICENSES/wiiuse.txt` | boot.dol | none by CaveWii |
| libfat (libfat-ogc) | 2.1.0, devkitPro/libdvm v2.1.0 + fatfs-mod | ZPL-2.1 AND FatFs licence | `LICENSES/libfat.txt` | boot.dol | none |
| zlib (ppc-zlib, Wii) | 1.3.1, zlib.net | Zlib | `LICENSES/zlib.txt` (installed ppc-zlib 1.3.1 header) | boot.dol | none |
| newlib (devkitPPC libc/libm/libsysbase) | 4.6.0.20260123, sourceware newlib (devkitPro build) | newlib collection (COPYING.NEWLIB) | `LICENSES/newlib.txt` | boot.dol | none by CaveWii |
| libgcc (devkitPPC GCC runtime) | GCC 15.2.0 | GPL-3.0-or-later WITH GCC-exception-3.1 | `LICENSE`, `LICENSES/GCC-exception-3.1.txt` | boot.dol | none |

## Build-only (not redistributed; does not block R18)

| Component | Version | Licence (as declared) | Note |
|---|---|---|---|
| devkitPPC (GCC cross toolchain) | r49.2 (gcc 15.2.0, binutils 2.45.1) | GPL-3.0-or-later AND GPL-2.0-or-later AND LGPL (pacman) | compiler only |
| devkitppc-rules / devkitppc-crtls (wii_rules, rvl.ld) | 1.2.1 / 2.0.0 | custom (pacman; text not installed) | make rules and linker scripts; unverified, not shipped |
| catnip (devkitPro CMake build driver, libogc sidecar build) | devkitPro install | unknown (not checked) | builds the sidecar stage-lib |
| PC build libraries: GLFW, GLEW, OpenGL, zlib, winpthreads, MinGW gcc-libs | glfw 3.4, glew 2.2.0, zlib 1.3.2, winpthreads 13.0.0.r560, gcc-libs 15.2.0 | see JSON (pacman) | PC dev/test build only. They need redistributed records if a PC binary ever ships |
| CMake, Python 3, bash (MSYS2/git-bash) | cmake 4.2.3, Python 3.14.2 | BSD-3-Clause; PSF-2.0; GPL-3.0-or-later | tooling |

## Not covered here

Art, audio and texture packs (including Isabella II, CC BY 3.0) are covered by
`ASSET_ATTRIBUTION.md` / `tools/release-assets.json` (REL-18e4).
