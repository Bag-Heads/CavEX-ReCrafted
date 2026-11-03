#!/usr/bin/env bash
# tools/make_release.sh -- package a hardware-certified, sidecar-linked
# CaveWii_v2 .dol into an SD-card-shaped release zip with checksums and
# provenance. Runs under MSYS2/git-bash on Windows and under plain bash on
# Linux (CI). LOCAL ONLY: this tool never pushes to git and never publishes
# anywhere -- see --publish below.
#
# Usage:
#   tools/make_release.sh [--dol <path>] [--elf <path>] [--pass <PASS.json>]
#                          [--allow-uncertified] [--version <v>] [--out <dir>]
#                          [--commit <sha>] [--repo <path>] [--devkitppc <dir>]
#                          [--skip-objdump-gate-for-tests] [--publish]
#
#   --repo <path>    repository root whose git-tracked content (assets,
#                     LICENSE, VERSION, config templates, ...) is packaged,
#                     and whose git history --commit/--version resolve
#                     against. Default: this script's own repository (two
#                     directories up from tools/make_release.sh) -- i.e.
#                     ordinary in-place use is unaffected. Exists so the
#                     tool is testable hermetically (a scratch git repo in a
#                     temp dir, see tests/test_make_release.py) and so a
#                     worktree whose .git file cannot be resolved from the
#                     current OS (e.g. a Windows gitdir seen from WSL -- see
#                     the git-resolution hint below) can be worked around by
#                     pointing at a native checkout instead.
#   --dol <path>     Wii .dol to package. Default: <repo>/CaveWii_v2.dol
#   --elf <path>     matching linked .elf (for the gates below). Default:
#                     the --dol path with .dol replaced by .elf
#   --pass <path>    a validated tmp/wii_runs/<run>/PASS.json for this exact
#                     .dol (see tools/validate_wii_pass.ps1). Required unless
#                     --allow-uncertified is given.
#   --allow-uncertified
#                    package without a PASS.json. The zip name and
#                    provenance.json are both stamped UNCERTIFIED so it can
#                    never be mistaken for a hardware-certified release.
#   --version <v>    override the release version string. Default:
#                     "<VERSION file>+<git describe --tags --always>" (or
#                     just the git describe output if there is no VERSION
#                     file).
#   --out <dir>      output directory for the zip. Default: <repo>/dist
#   --commit <ref>   git commit whose TRACKED content (assets, LICENSE,
#                     config templates, ...) is packaged. Default: HEAD.
#                     Read via `git show`, never the working tree, so a
#                     dirty/untracked file can never leak into a release
#                     (this is also how a locally-modified or gitignored
#                     assets/mob/*.png -- the documented user-supplied-Beta-
#                     rip path -- can never end up in a shipped zip).
#   --devkitppc <dir> devkitPPC install root (has bin/powerpc-eabi-objdump
#                     and bin/powerpc-eabi-readelf). Default: $DEVKITPPC,
#                     else $DEVKITPRO/devkitPPC, else a couple of documented
#                     fallback locations.
#   --skip-objdump-gate-for-tests
#                    TEST-ONLY escape hatch for the KERNEL_HEAP/TLS/test-
#                    marker gates below, so the test suite can exercise this
#                    script without a real devkitPPC toolchain or a real
#                    linked .elf. Refused (fatal) whenever --pass is also
#                    given, and requires --allow-uncertified -- a real,
#                    hardware-certified release (the only path that can ever
#                    carry --pass) can never take this shortcut.
#   --publish        NOT IMPLEMENTED. This tool is local-only: it never
#                     pushes to git and never uploads/publishes the zip
#                     anywhere. The flag exists (per the project's release
#                     process) so a caller can name the intent, but passing
#                     it is always a fatal error -- there is no publish path
#                     to opt into.
#
# Exit codes: 0 success; 2 bad usage; 3 refused input (stock or dev-stamped
# .dol, missing PASS without --allow-uncertified, a stability-only or
# malformed PASS, a missing licence/pack/icon/fallback config/tutorial-world
# builder at --commit, a
# fallback config that is not a usable player default, ...); 4 a
# verification gate failed (incl. a PASS.json for a different build);
# 5 missing required tool; 6 packaging step failed (incl. the tutorial
# world's acceptance test or build, step 12d3).
set -u -o pipefail

# ---------------------------------------------------------------------------
# 0. Locate the repo and seed PATH the same way build-verify.sh does (MSYS2's
#    core `strings`/`sha256sum`/`md5sum` live under /usr/bin; UCRT64 supplies
#    `strings` on some MSYS2 installs that lack it under /usr/bin).
# ---------------------------------------------------------------------------
if [ -d /c/msys64/ucrt64/bin ]; then
	export PATH="/c/msys64/ucrt64/bin:$PATH"
fi
if [ -d /usr/bin ]; then
	export PATH="/usr/bin:$PATH"
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" || {
	echo "[make-release] FAIL: cannot resolve this script's directory" >&2
	exit 2
}
REPO="$(cd "$SCRIPT_DIR/.." && pwd)" || {
	echo "[make-release] FAIL: cannot resolve the repository root" >&2
	exit 2
}

# --repo <path> overrides $REPO before any $REPO-dependent default below is
# computed (a plain pass over the main argument-parsing loop, further down,
# would run too late for that). Scanned here, in a dedicated pre-pass, the
# same way build-verify.sh pre-strips --dev before computing DEV_MODE-
# dependent state. The main loop below still matches --repo (as a no-op) so
# it isn't rejected as an unknown argument wherever it appears in "$@".
_CW_ARGV=("$@")
_CW_N=${#_CW_ARGV[@]}
_CW_I=0
while [ "$_CW_I" -lt "$_CW_N" ]; do
	if [ "${_CW_ARGV[$_CW_I]}" = "--repo" ]; then
		_CW_J=$((_CW_I + 1))
		if [ "$_CW_J" -ge "$_CW_N" ]; then
			echo "[make-release] FAIL: --repo requires a path argument" >&2
			exit 2
		fi
		REPO="$(cd "${_CW_ARGV[$_CW_J]}" 2>/dev/null && pwd)" || {
			echo "[make-release] FAIL: --repo path not found: ${_CW_ARGV[$_CW_J]}" >&2
			exit 2
		}
		break
	fi
	_CW_I=$((_CW_I + 1))
done
unset _CW_ARGV _CW_N _CW_I _CW_J

# ---------------------------------------------------------------------------
# 1. Argument parsing
# ---------------------------------------------------------------------------
DOL="$REPO/CaveWii_v2.dol"
ELF=""
PASS_JSON=""
ALLOW_UNCERTIFIED=0
VERSION_ARG=""
OUT_DIR="$REPO/dist"
COMMIT="HEAD"
DEVKITPPC_ARG=""
SKIP_GATE_FOR_TESTS=0
PUBLISH=0

usage() {
	sed -n '2,72p' "${BASH_SOURCE[0]}"
}

while [ $# -gt 0 ]; do
	case "$1" in
		--dol) DOL="$2"; shift 2 ;;
		--elf) ELF="$2"; shift 2 ;;
		--pass) PASS_JSON="$2"; shift 2 ;;
		--allow-uncertified) ALLOW_UNCERTIFIED=1; shift ;;
		--version) VERSION_ARG="$2"; shift 2 ;;
		--out) OUT_DIR="$2"; shift 2 ;;
		--commit) COMMIT="$2"; shift 2 ;;
		--repo) shift 2 ;; # already resolved by the pre-pass above
		--devkitppc) DEVKITPPC_ARG="$2"; shift 2 ;;
		--skip-objdump-gate-for-tests) SKIP_GATE_FOR_TESTS=1; shift ;;
		--publish) PUBLISH=1; shift ;;
		-h|--help) usage; exit 0 ;;
		*) echo "[make-release] FAIL: unknown argument: $1" >&2; usage >&2; exit 2 ;;
	esac
done

if [ "$PUBLISH" = "1" ]; then
	echo "[make-release] FAIL: --publish is not implemented. This tool is LOCAL ONLY" >&2
	echo "[make-release]       (never pushes to git, never uploads/publishes the zip)." >&2
	echo "[make-release]       Remove --publish; publishing is a separate, human decision." >&2
	exit 2
fi

if [ -z "$ELF" ]; then
	case "$DOL" in
		*.dol) ELF="${DOL%.dol}.elf" ;;
		*) ELF="$DOL.elf" ;;
	esac
fi

# ---------------------------------------------------------------------------
# 2. Required-tool preflight
# ---------------------------------------------------------------------------
for TOOL in git python3 sha256sum md5sum strings; do
	command -v "$TOOL" >/dev/null 2>&1 || {
		echo "[make-release] FAIL: required tool not on PATH: $TOOL" >&2
		exit 5
	}
done
PY="python3"

# ---------------------------------------------------------------------------
# 3. Basic input validation: .dol exists, and its name is EXACTLY the release
#    artifact name the Wii Makefile writes (CaveWii_v2.dol). ALLOWLIST, not a
#    denylist (G1 adversarial review F7): every other name the build family
#    emits is non-release by construction -- -stock (CW_ALLOW_STOCK_LIBOGC=1),
#    -ci (tools/local_ci.sh / CI sidecar compile evidence), -dev (make dev),
#    -REFUSED-sd0dma (boot-death gate) -- and a future suffix must not slip
#    through because nobody added it to a list.
# ---------------------------------------------------------------------------
if [ ! -f "$DOL" ]; then
	echo "[make-release] FAIL: .dol not found: $DOL" >&2
	exit 3
fi
DOL_BASENAME="$(basename "$DOL")"
DOL_BASENAME_LC="$(printf '%s' "$DOL_BASENAME" | tr '[:upper:]' '[:lower:]')"
case "$DOL_BASENAME_LC" in
	*-stock*)
		echo "[make-release] FAIL: refusing to package a stock-libogc .dol ($DOL_BASENAME)." >&2
		echo "[make-release]       Stock libogc (64KB KERNEL_HEAP) is CI-compile-smoke-test-only" >&2
		echo "[make-release]       and is NOT hardware-proof -- see" >&2
		echo "[make-release]       docs/wii_libogc_kernel_heap_sidecar.md. Build with the sidecar" >&2
		echo "[make-release]       (LIBOGC_LIB=<stage-lib>, no CW_ALLOW_STOCK_LIBOGC=1) instead." >&2
		exit 3
		;;
	cavewii_v2.dol)
		;;
	*)
		echo "[make-release] FAIL: refusing to package $DOL_BASENAME -- only the release artifact" >&2
		echo "[make-release]       name CaveWii_v2.dol is accepted (the Wii Makefile's release output)." >&2
		echo "[make-release]       -ci / -dev / -stock / -REFUSED-* and any renamed .dol are not" >&2
		echo "[make-release]       release artifacts. Package the repo-root CaveWii_v2.dol from a" >&2
		echo "[make-release]       sidecar release build (build-wii skill / cwbuild wii-release)." >&2
		exit 3
		;;
esac

if [ "$SKIP_GATE_FOR_TESTS" = "1" ]; then
	if [ -n "$PASS_JSON" ]; then
		echo "[make-release] FAIL: --skip-objdump-gate-for-tests cannot be combined with --pass." >&2
		echo "[make-release]       A hardware-certified release always runs the real gates; this" >&2
		echo "[make-release]       flag exists only for testing make_release.sh itself against a" >&2
		echo "[make-release]       fake .dol/.elf pair with no PASS.json (i.e. --allow-uncertified)." >&2
		exit 3
	fi
	if [ "$ALLOW_UNCERTIFIED" != "1" ]; then
		echo "[make-release] FAIL: --skip-objdump-gate-for-tests requires --allow-uncertified" >&2
		echo "[make-release]       (it is a test-only escape hatch, not a way to certify a real" >&2
		echo "[make-release]       release without running the gates)." >&2
		exit 3
	fi
else
	if [ ! -f "$ELF" ]; then
		echo "[make-release] FAIL: .elf not found: $ELF (needed for the KERNEL_HEAP/TLS/test-marker gates)" >&2
		exit 3
	fi
fi

if [ -z "$PASS_JSON" ] && [ "$ALLOW_UNCERTIFIED" != "1" ]; then
	echo "[make-release] FAIL: no --pass PASS.json given. A release must be backed by a real-Wii" >&2
	echo "[make-release]       hardware-certified PASS.json (see docs/run-wii-hardware / CLAUDE.md" >&2
	echo "[make-release]       'Definition of done for FIX COMMITS'). Pass --allow-uncertified to" >&2
	echo "[make-release]       package anyway -- the zip and its provenance are both marked" >&2
	echo "[make-release]       UNCERTIFIED so it can never be mistaken for a certified release." >&2
	exit 3
fi
if [ -n "$PASS_JSON" ] && [ ! -f "$PASS_JSON" ]; then
	echo "[make-release] FAIL: --pass file not found: $PASS_JSON" >&2
	exit 3
fi

# ---------------------------------------------------------------------------
# 4. git identity of the commit being packaged
# ---------------------------------------------------------------------------
GIT_SHA="$(git -C "$REPO" rev-parse "$COMMIT" 2>/dev/null)" || {
	echo "[make-release] FAIL: cannot resolve --commit '$COMMIT' in $REPO" >&2
	echo "[make-release]       hint: a worktree checked out on another OS (e.g. a Windows .git" >&2
	echo "[make-release]       gitdir resolved from WSL) often cannot be read this way -- run" >&2
	echo "[make-release]       from a native checkout, or pass --repo <path> to a repo git can" >&2
	echo "[make-release]       actually resolve from here." >&2
	exit 3
}
GIT_SHA_SHORT="$(git -C "$REPO" rev-parse --short=7 "$COMMIT")"
GIT_DIRTY=false
if [ -n "$(git -C "$REPO" status --porcelain 2>/dev/null)" ]; then
	GIT_DIRTY=true
fi

# ---------------------------------------------------------------------------
# 4b. Required release inputs at $COMMIT (REL-01b, 2026-10-17). A public
#     release must never be cut with part of its legal text, its optional
#     pack, its icon or its fallback config missing, so each is checked here,
#     before any gate runs, and a gap is a refused input (exit 3), not a
#     warning. config_wii.json is the shipped FALLBACK TEMPLATE: source/
#     config.c's config_load_user_or_default() loads it when config.json is
#     missing or corrupt (REL-01a), so it must also be a usable player
#     default -- a JSON object with a Wii input map and none of the capture
#     rig's debug settings (tools/config_capture.json: autopilot,
#     auto_screenshot, forced overlay), which break normal play.
# ---------------------------------------------------------------------------
for REQ in LICENSE icon.png config_wii.json \
	"LICENSES/Isabella-II/About Isabella.txt" "LICENSES/Isabella-II/LICENSE.txt" \
	"packs/isabella-ii/About Isabella.txt" "packs/isabella-ii/LICENSE.txt" \
	"packs/isabella-ii/README.txt" "packs/isabella-ii/.cavewii-pack.json"; do
	if ! git -C "$REPO" cat-file -e "$COMMIT:$REQ" 2>/dev/null; then
		echo "[make-release] FAIL: required release input missing at commit $GIT_SHA_SHORT: $REQ" >&2
		exit 3
	fi
done
"$PY" - "$REPO" "$COMMIT" <<'PYEOF' || {
import json, subprocess, sys
raw = subprocess.run(["git", "-C", sys.argv[1], "show", f"{sys.argv[2]}:config_wii.json"],
                     capture_output=True, check=True).stdout
try:
    cfg = json.loads(raw)
except ValueError as e:
    sys.exit(f"config_wii.json is not valid JSON: {e}")
if not isinstance(cfg, dict):
    sys.exit("config_wii.json root is not a JSON object")
if not isinstance(cfg.get("input"), dict) or not cfg["input"]:
    sys.exit("config_wii.json has no input map (controllers would be unmapped)")
debug = cfg.get("debug") or {}
if not isinstance(debug, dict):
    sys.exit("config_wii.json debug block is not an object")
for key in ("autopilot", "auto_screenshot"):
    if key in debug:
        sys.exit(f"config_wii.json carries capture-rig setting debug.{key}")
if debug.get("overlay", "off") != "off":
    sys.exit("config_wii.json forces the debug overlay on")
# The zip ships this file as the ONLY config (12c below), so its paths must
# name the bundled default pack and saves folder this script packages, never
# a dev/user pack path that does not exist on a clean release card.
paths = cfg.get("paths") or {}
if not isinstance(paths, dict) or paths.get("texturepack") != "assets":
    sys.exit("config_wii.json paths.texturepack must be the bundled 'assets' pack")
if paths.get("worlds") != "saves":
    sys.exit("config_wii.json paths.worlds must be 'saves'")
PYEOF
	echo "[make-release] FAIL: config_wii.json at commit $GIT_SHA_SHORT is not a usable shipped default (see above)." >&2
	exit 3
}

# ---------------------------------------------------------------------------
# 5. Version resolution: --version, else "<VERSION file>+<git describe>",
#    else just <git describe> if there is no VERSION file.
# ---------------------------------------------------------------------------
if [ -n "$VERSION_ARG" ]; then
	RELEASE_VERSION="$VERSION_ARG"
else
	GIT_DESCRIBE="$(git -C "$REPO" describe --tags --always 2>/dev/null)" || GIT_DESCRIBE="$GIT_SHA_SHORT"
	if git -C "$REPO" cat-file -e "$COMMIT:VERSION" 2>/dev/null; then
		VERSION_FILE_CONTENT="$(git -C "$REPO" show "$COMMIT:VERSION" | tr -d '[:space:]')"
		RELEASE_VERSION="${VERSION_FILE_CONTENT}+${GIT_DESCRIBE}"
	else
		RELEASE_VERSION="$GIT_DESCRIBE"
	fi
fi
# Keep the resolved version filesystem/zip-name-safe (also feeds gen_meta_xml.sh,
# which independently asserts this same charset).
case "$RELEASE_VERSION" in
	*[!A-Za-z0-9.+_-]*)
		echo "[make-release] FAIL: resolved version '$RELEASE_VERSION' has characters outside [A-Za-z0-9.+_-]" >&2
		echo "[make-release]       pass --version explicitly with a clean value." >&2
		exit 2
		;;
esac

echo "[make-release] packaging commit=$GIT_SHA (dirty=$GIT_DIRTY) version=$RELEASE_VERSION"

# ---------------------------------------------------------------------------
# 6. devkitPPC toolchain resolution (objdump/readelf) for the gates below.
# ---------------------------------------------------------------------------
DEVKITPPC="$DEVKITPPC_ARG"
if [ -z "$DEVKITPPC" ]; then
	for CAND in "${DEVKITPPC:-}" "${DEVKITPRO:-}/devkitPPC" \
		/c/msys64/opt/devkitpro/devkitPPC /opt/devkitpro/devkitPPC; do
		if [ -n "$CAND" ] && [ -x "$CAND/bin/powerpc-eabi-objdump" ]; then
			DEVKITPPC="$CAND"
			break
		fi
	done
fi
OBJDUMP="${DEVKITPPC:+$DEVKITPPC/bin/powerpc-eabi-objdump}"
READELF="${DEVKITPPC:+$DEVKITPPC/bin/powerpc-eabi-readelf}"
GCC_VERSION=""
if [ -n "$DEVKITPPC" ] && [ -x "$DEVKITPPC/bin/powerpc-eabi-gcc" ]; then
	GCC_VERSION="$("$DEVKITPPC/bin/powerpc-eabi-gcc" --version 2>/dev/null | head -1)"
fi

# ---------------------------------------------------------------------------
# 7. Verification gates. These re-verify the ALREADY-LINKED .elf/.dol
#    independently of whatever the build did -- this script does not trust
#    "it was built with the sidecar", it checks the artifact itself.
#
#    KERNEL_HEAP gate: copied (not sourced) from
#    .claude/skills/build-wii/scripts/build-verify.sh's run_kernel_heap_gate,
#    same objdump signature. TLS + test-marker gates: copied from the
#    Makefile's $(OUTPUT).dol recipe (readelf -SW/-sW checks). DELIBERATE
#    REPLICATION, not a shared include: build-verify.sh is a tested,
#    security-relevant skill script with its own MSYS trampoline ordering
#    (win_to_msys_path/is_wsl run before any shared file could be sourced);
#    refactoring it to source a new lib file risked destabilizing a script
#    other flows depend on, for a one-time packaging tool. See the release
#    packet for this tradeoff spelled out.
# ---------------------------------------------------------------------------
KERNEL_HEAP_GATE_RESULT="SKIPPED_FOR_TESTS"
TLS_GATE_RESULT="SKIPPED_FOR_TESTS"
TEST_MARKER_GATE_RESULT="SKIPPED_FOR_TESTS"

run_kernel_heap_gate() {
	local ELF_PATH="$1" DUMP CALL_LINE CONTEXT GATE_LINE
	DUMP="$("$OBJDUMP" -dr "$ELF_PATH" 2>&1)" || {
		echo "[make-release] FAIL: objdump failed to run on $ELF_PATH" >&2
		return 1
	}
	CALL_LINE="$(printf '%s\n' "$DUMP" | grep -n 'bl[[:space:]].*<__lwp_wkspace_init>' | head -1 | cut -d: -f1)"
	if [ -z "$CALL_LINE" ]; then
		CALL_LINE="$(printf '%s\n' "$DUMP" | grep -n 'R_PPC_REL24[[:space:]]*__lwp_wkspace_init' | head -1 | cut -d: -f1)"
	fi
	if [ -z "$CALL_LINE" ]; then
		echo "[make-release] FAIL: KERNEL_HEAP gate: no call site to __lwp_wkspace_init found in $ELF_PATH" >&2
		return 1
	fi
	CONTEXT="$(printf '%s\n' "$DUMP" | sed -n "$((CALL_LINE > 40 ? CALL_LINE - 40 : 1)),${CALL_LINE}p")"
	GATE_LINE="$(printf '%s\n' "$CONTEXT" | grep -E 'lis[[:space:]]+r3,4$' | tail -1)"
	if [ -z "$GATE_LINE" ]; then
		echo "[make-release] FAIL: KERNEL_HEAP gate: no 'lis r3,4' (256KB) feeding __lwp_wkspace_init in $ELF_PATH" >&2
		echo "[make-release]       -- stock 64KB libogc ('lis r3,1') appears to be linked. Not hardware-proof." >&2
		return 1
	fi
	echo "[make-release] KERNEL_HEAP gate PASSED: $GATE_LINE"
	return 0
}

run_tls_and_marker_gates() {
	local ELF_PATH="$1" TLS
	"$READELF" -h "$ELF_PATH" >/dev/null 2>&1 || {
		echo "[make-release] FAIL: readelf cannot read $ELF_PATH" >&2
		return 1
	}
	TLS="$( { "$READELF" -SW "$ELF_PATH" | grep -E '^ *\[ *[0-9]+\] +\.t(data|bss)([. ]|$)'; \
	          "$READELF" -sW "$ELF_PATH" | awk '$4 == "TLS"'; } )"
	if [ -n "$TLS" ]; then
		echo "[make-release] FAIL: TLS gate: thread-local storage linked into $(basename "$ELF_PATH"):" >&2
		printf '%s\n' "$TLS" >&2
		return 1
	fi
	echo "[make-release] TLS gate PASSED: no .tdata/.tbss sections, no TLS-typed symbols"
	if "$READELF" -sW "$ELF_PATH" | grep -q cavewii_test_engine_marker; then
		echo "[make-release] FAIL: test-marker gate: test-instrumented engine objects linked into $(basename "$ELF_PATH")" >&2
		return 1
	fi
	echo "[make-release] test-marker gate PASSED: no test-instrumented objects"
	return 0
}

if [ "$SKIP_GATE_FOR_TESTS" = "1" ]; then
	echo "[make-release] WARNING: --skip-objdump-gate-for-tests set -- KERNEL_HEAP/TLS/test-marker" >&2
	echo "[make-release]          gates SKIPPED. TEST-ONLY; never valid for a real release (see" >&2
	echo "[make-release]          the refusal rules above: this path requires --allow-uncertified" >&2
	echo "[make-release]          and forbids --pass, so a certified release can never take it)." >&2
else
	if [ -z "$OBJDUMP" ] || [ ! -x "$OBJDUMP" ]; then
		echo "[make-release] FAIL: devkitPPC objdump not found (looked for \$DEVKITPPC, --devkitppc," >&2
		echo "[make-release]       \$DEVKITPRO/devkitPPC, and the documented fallback paths)." >&2
		exit 5
	fi
	if [ -z "$READELF" ] || [ ! -x "$READELF" ]; then
		echo "[make-release] FAIL: devkitPPC readelf not found (same search as objdump above)." >&2
		exit 5
	fi
	if ! run_kernel_heap_gate "$ELF"; then
		echo "[make-release] FAIL: KERNEL_HEAP gate did not pass on $ELF -- refusing to package." >&2
		exit 4
	fi
	KERNEL_HEAP_GATE_RESULT="PASSED"
	if ! run_tls_and_marker_gates "$ELF"; then
		echo "[make-release] FAIL: TLS/test-marker gate did not pass on $ELF -- refusing to package." >&2
		exit 4
	fi
	TLS_GATE_RESULT="PASSED"
	TEST_MARKER_GATE_RESULT="PASSED"
fi

# ---------------------------------------------------------------------------
# 8. CW_BUILD_ID extraction from the .dol (same tokenizing/regex family as
#    build-verify.sh's extract_build_ids, widened to accept EITHER the
#    release form (hex_MM.DD) or the dev form (hex_MM.DD-dev) since this
#    script doesn't know in advance which one a given .dol carries).
# ---------------------------------------------------------------------------
BUILD_ID_CORE='(([0-9a-f]{4,40}(\+([0-9a-f]{8})?)?|nogit)_[0-9]{2}\.[0-9]{2}(-dev)?|unstamped)'
DOL_BUILD_IDS="$(strings -a "$DOL" | tr -cs '[:alnum:]_+.-' '\n' | grep -E "^${BUILD_ID_CORE}$" | sort -u)"
if [ -z "$DOL_BUILD_IDS" ]; then
	echo "[make-release] WARNING: no CW_BUILD_ID token found embedded in $DOL" >&2
	echo "[make-release] embedded CW_BUILD_ID(s): <none>"
else
	echo "[make-release] embedded CW_BUILD_ID(s): $(printf '%s' "$DOL_BUILD_IDS" | tr '\n' ' ')"
fi
# REL-01b: a `make dev` build stamps its CW_BUILD_ID with a -dev suffix. The
# file-name allowlist above refuses CaveWii_v2-dev.dol, but a dev build
# copied or renamed to CaveWii_v2.dol would pass it, so the embedded stamp is
# checked too. A dev build is never a release artifact, certified or not.
if printf '%s\n' "$DOL_BUILD_IDS" | grep -q -- '-dev$'; then
	echo "[make-release] FAIL: $DOL_BASENAME carries a dev build stamp ($(printf '%s' "$DOL_BUILD_IDS" | tr '\n' ' '))." >&2
	echo "[make-release]       make dev output is not a release artifact; package a release build." >&2
	exit 3
fi

# ---------------------------------------------------------------------------
# 9. PASS.json validation + build_id cross-check (skipped entirely when
#    --allow-uncertified with no --pass).
# ---------------------------------------------------------------------------
PASS_RUN_ID=""
PASS_PROFILE=""
PASS_BUILD_ID=""
CERT_STATUS="UNCERTIFIED"
if [ -n "$PASS_JSON" ]; then
	PASS_FIELDS="$("$PY" - "$PASS_JSON" "$REPO" "$COMMIT" <<'PYEOF'
import json, subprocess, sys
path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)
if data.get("status") != "PASS":
    sys.stderr.write(f"PASS.json status is not PASS: {data.get('status')!r}\n")
    sys.exit(1)
if data.get("release_performance_pass") is not True:
    sys.stderr.write("stability-only PASS is not release-performance evidence; collect with --release-gate\n")
    sys.exit(1)
if data.get("profile") not in ("keepup_walk_6m", "far_walk_6m"):
    sys.stderr.write("release evidence requires a six-minute walk profile\n")
    sys.exit(1)
hb = data.get("last_heartbeat") or {}
cfg = json.loads(subprocess.check_output(
    ["git", "-C", sys.argv[2], "show", f"{sys.argv[3]}:config_wii.json"]))
lod = cfg.get("lod", {})
storage = {"voxel": 0, "skin": 1}.get(lod.get("far_storage", "voxel"))
if (storage is None or hb.get("far_storage") != storage
        or hb.get("far_view_distance") != lod.get("far_view_distance", 5)):
    sys.stderr.write("tested LOD reach/storage do not match the packaged Wii configuration\n")
    sys.exit(1)
run_id = data.get("run_id", "")
profile = data.get("profile", "")
build_id = (data.get("last_heartbeat") or {}).get("build_id", "")
if not run_id or not profile or not build_id:
    sys.stderr.write(
        f"PASS.json missing run_id/profile/last_heartbeat.build_id "
        f"(got run_id={run_id!r} profile={profile!r} build_id={build_id!r})\n"
    )
    sys.exit(1)
print(run_id)
print(profile)
print(build_id)
PYEOF
)" || {
		echo "[make-release] FAIL: --pass $PASS_JSON failed validation (see above)." >&2
		exit 3
	}
	PASS_RUN_ID="$(printf '%s\n' "$PASS_FIELDS" | sed -n '1p')"
	PASS_PROFILE="$(printf '%s\n' "$PASS_FIELDS" | sed -n '2p')"
	PASS_BUILD_ID="$(printf '%s\n' "$PASS_FIELDS" | sed -n '3p')"

	if [ "$DOL_BUILD_IDS" != "$PASS_BUILD_ID" ]; then
		DOL_BUILD_IDS_DISPLAY="${DOL_BUILD_IDS:-<none>}"
		echo "[make-release] FAIL: PASS.json build_id ('$PASS_BUILD_ID') does not match the .dol's" >&2
		echo "[make-release]       embedded CW_BUILD_ID(s) ($(printf '%s' "$DOL_BUILD_IDS_DISPLAY" | tr '\n' ' '))." >&2
		echo "[make-release]       This PASS.json does not certify THIS .dol -- refusing to package" >&2
		echo "[make-release]       it as a certified release. Re-run the hardware profile against" >&2
		echo "[make-release]       this exact .dol, or pass --allow-uncertified if that is intended." >&2
		exit 4
	fi
	CERT_STATUS="PERFORMANCE_VERIFIED"
	echo "[make-release] PASS.json OK: run_id=$PASS_RUN_ID profile=$PASS_PROFILE build_id=$PASS_BUILD_ID (matches .dol)"
fi

# ---------------------------------------------------------------------------
# 10. Hashes: .dol sha256+md5, .elf sha256, sidecar libogc.a sha256 if the
#     caller's environment names one (LIBOGC_LIB / CW_LIBOGC_LIB, same
#     variables the Makefile/build-wii skill use).
# ---------------------------------------------------------------------------
DOL_SHA256="$(sha256sum "$DOL" | cut -d' ' -f1)"
DOL_MD5="$(md5sum "$DOL" | cut -d' ' -f1)"
ELF_SHA256=""
if [ -f "$ELF" ]; then
	ELF_SHA256="$(sha256sum "$ELF" | cut -d' ' -f1)"
fi
SIDECAR_DIR="${LIBOGC_LIB:-${CW_LIBOGC_LIB:-}}"
SIDECAR_LIBOGC_A=""
SIDECAR_LIBOGC_A_SHA256=""
if [ -n "$SIDECAR_DIR" ] && [ -f "$SIDECAR_DIR/libogc.a" ]; then
	SIDECAR_LIBOGC_A="$SIDECAR_DIR/libogc.a"
	SIDECAR_LIBOGC_A_SHA256="$(sha256sum "$SIDECAR_LIBOGC_A" | cut -d' ' -f1)"
fi

# ---------------------------------------------------------------------------
# 11. Shippable asset roster. HARD LEGAL RULE (never relaxed by a flag):
#     only git-TRACKED files under assets/ at $COMMIT are eligible, and only
#     git `show`n content is used (never the working tree) -- this is the
#     structural guard against ever shipping a locally-modified or
#     gitignored file (assets/mob/*.png is gitignored specifically because
#     it is where a user's own extracted, real Beta 1.7.3 mob rips live;
#     git ls-tree can never see them).
#
#     RESOLVED 2026-09-02 (maintainer decision): assets/controls.png and all
#     10 assets/armor/*.png were REPLACED with original, from-scratch CC0
#     art (tools/gen_cc0_controls.py / tools/gen_cc0_armor.py -- see
#     assets/README.md's entries for both) instead of staying excluded --
#     both previously carried unresolved/unknown provenance (WiiBrew-
#     unknown-license controls.png; suspected-Mojang-rip armor/*.png per
#     docs/m3_legal_pack_plan.md S3), and neither exclusion applies to the
#     new files. They now ship like any other tracked asset.
#
#     REVISED 2026-09-02 (coordinator follow-up, same day): the previous
#     revision of this rule EXCLUDED assets/misc/{foliagecolor,grasscolor,
#     water}.png after the Isabella-II provenance audit
#     (docs/asset_provenance_2026-09-02.md) found all three 0% pixel-match
#     to an archived copy of that pack. That exclusion is CORRECT for
#     misc/water.png (a genuinely optional overlay -- source/main.c's own
#     comment: "misc/water.png is absent on a default install"; the
#     underwater quad is skipped via `gstate.players[0].in_water && texture_water.data`,
#     a graceful no-op) but WRONG for the two colormaps:
#     source/graphics/biome_color.c's biome_color_init() loads
#     misc/grasscolor.png and misc/foliagecolor.png UNCONDITIONALLY at
#     every tex_init(), and their absence makes every grass/leaf face
#     render pure white (biome_color.c's sample() returns 0xFFFFFF with no
#     colormap loaded) -- a visibly broken render, not a graceful missing-
#     overlay case. Replaced with original CC0 art instead
#     (tools/gen_cc0_colormaps.py -- see assets/README.md's entries for
#     both), matching the treatment controls.png/armor/*.png already got in
#     this same lane, and the exclusion lifted for both. Only
#     the historical assets/misc/water.png remains excluded (the reviewed
#     2026-09-11 CC0 replacement is hash-allowed below), EXPLICITLY (not a
#     wildcard) so the next reader sees exactly which file and why:
#       - assets/misc/water.png (underwater overlay; genuinely optional --
#         `.data`-guarded, absence is a documented no-op, not a broken
#         render; still untraceable to Isabella-II or any other credited
#         source per docs/asset_provenance_2026-09-02.md)
# ---------------------------------------------------------------------------
ASSET_EXCLUDE_MATCH() {
	case "$1" in
		assets/misc/water.png)
			# 2026-09-11: admit only the reviewed Earth & Ash CC0 replacement.
			# Read the selected commit, so older refs and local overlays cannot
			# inherit this clearance merely by sharing its filename.
			local digest
			digest="$(git -C "$REPO" show "$COMMIT:$1" | sha256sum | cut -d' ' -f1)"
			[ "$digest" != "7a1a30372469a5db1567f7451365dcbde1abb71b0d85aea7cec10043bd34677f" ] ;;
		*) return 1 ;;
	esac
}

ALL_TRACKED_ASSETS="$(git -C "$REPO" ls-tree -r --name-only "$COMMIT" -- assets)"
INCLUDED_ASSETS=""
EXCLUDED_ASSETS=""
while IFS= read -r REL; do
	[ -n "$REL" ] || continue
	if ASSET_EXCLUDE_MATCH "$REL"; then
		EXCLUDED_ASSETS="${EXCLUDED_ASSETS}${REL}"$'\n'
	else
		INCLUDED_ASSETS="${INCLUDED_ASSETS}${REL}"$'\n'
	fi
done <<EOF
$ALL_TRACKED_ASSETS
EOF

echo "[make-release] shippable assets: $(printf '%s' "$INCLUDED_ASSETS" | grep -c .) included, $(printf '%s' "$EXCLUDED_ASSETS" | grep -c .) excluded (see provenance.json)"

# ---------------------------------------------------------------------------
# 12. Staging directory
# ---------------------------------------------------------------------------
STAGE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cavewii-release.XXXXXX")" || {
	echo "[make-release] FAIL: could not create a staging temp dir" >&2
	exit 6
}
cleanup_stage() { rm -rf "$STAGE_ROOT"; }
trap cleanup_stage EXIT

APP_DIR="$STAGE_ROOT/apps/CavEX"
mkdir -p "$APP_DIR" "$STAGE_ROOT/texturepacks" || exit 6

git_show_file() {
	# git_show_file <path-at-COMMIT> <dest-file>
	local SRC="$1" DEST="$2"
	mkdir -p "$(dirname "$DEST")" || return 1
	git -C "$REPO" show "$COMMIT:$SRC" > "$DEST" 2>/dev/null || {
		echo "[make-release] FAIL: could not read $SRC from commit $GIT_SHA_SHORT" >&2
		return 1
	}
}

# 12a. boot.dol / icon.png / the .dol's own build artifacts.
cp "$DOL" "$APP_DIR/boot.dol" || exit 6
git_show_file "icon.png" "$APP_DIR/icon.png" || exit 6

# 12b. meta.xml generated fresh from the template for THIS commit/version
#      (never the possibly-stale checked-in one), per tools/meta.xml.in.
"$SCRIPT_DIR/gen_meta_xml.sh" --repo "$REPO" --version "$RELEASE_VERSION" \
	--commit "$COMMIT" --out "$APP_DIR/meta.xml" || {
	echo "[make-release] FAIL: gen_meta_xml.sh failed" >&2
	exit 6
}

# 12c. Default config. source/main.c loads "config.json" relative to
#      whatever CWD a bare .dol boot resolves to, and
#      docs/platform_perf_notes.md's "Running on Dolphin" section states
#      plainly that this CWD "can be either" the SD card root or the app's
#      own subfolder -- "staging both is harmless" -- exactly what
#      tools/provision_wii_sd.ps1 already does for a physical card. So this
#      script stages the fallback template at both locations.
#
#      The zip ships ONLY config_wii.json, never a config.json (ci-1101/05):
#      config.json is the PLAYER'S file -- the game writes the multiplayer
#      name, server list and every Options change into it -- and a release
#      zip unzipped over an installed card used to overwrite it, wiping
#      those. The game needs no shipped config.json: on the Wii,
#      source/main.c:4789 sets default_config = "config_wii.json" and
#      source/config.c:235-247 config_load_user_or_default() loads it when
#      config.json is missing or corrupt; the first Options save then
#      creates config.json from it. The gate in 4b above already requires the
#      template's paths to name the bundled pack and saves folder, so the
#      template IS the release default with no derived copy.
#
#      The CONTENT is config_wii.json, NOT the repo-root config.json:
#      config.json's "input" block is GLFW PC-keyboard codes (87/83/65/68 =
#      W/A/S/D; see source/platform/input.c's GLFW_KEY_* mapping) -- it is
#      the PC dev/test config, not a Wii control scheme. config_wii.json
#      carries the real Wiimote/Nunchuk/Classic/GameCube button codes
#      (source/platform/input.c's WPAD_BUTTON_*/PAD_BUTTON_* mapping,
#      0-10/400s/900s) and is exactly what provision_wii_sd.ps1 uses as its
#      $baseConfigPath for a real card. Shipping the PC config to Wii
#      hardware would leave every physical controller unmapped.
if ! git -C "$REPO" cat-file -e "$COMMIT:config_wii.json" 2>/dev/null; then
	echo "[make-release] FAIL: config_wii.json not found at commit $GIT_SHA_SHORT" >&2
	exit 6
fi

for ROOT_DIR in "$STAGE_ROOT" "$APP_DIR"; do
	git_show_file "config_wii.json" "$ROOT_DIR/config_wii.json" || exit 6
done

# 12d. Shippable assets, staged at both the SD root and the app subfolder
#      (same "staging both is harmless" reasoning as the config above).
while IFS= read -r REL; do
	[ -n "$REL" ] || continue
	git_show_file "$REL" "$STAGE_ROOT/$REL" || exit 6
	git_show_file "$REL" "$APP_DIR/$REL" || exit 6
done <<EOF
$INCLUDED_ASSETS
EOF

# 12d2. Optional Isabella II pack (card ISA-1, 2026-10-10): every file TRACKED
#       under packs/isabella-ii/ at $COMMIT (never the working tree, so an
#       untracked/dirty file cannot ship) is staged as
#       texturepacks/isabella-ii/<rel>, where the in-game Texture Packs
#       screen lists it as 'isabella-ii'. It is a PARTIAL pack (files it
#       lacks fall back to assets/, the Earth & Ash default since
#       2026-10-10); the default config still selects assets/.
#       File names may contain spaces ("About Isabella.txt"), hence the
#       IFS= read -r loop and quoted paths throughout.
ISA_PACK_SRC="packs/isabella-ii"
ISA_PACK_DEST="texturepacks/isabella-ii"
ISA_PACK_FILES=""
ISA_PACK_TRACKED="$(git -C "$REPO" ls-tree -r --name-only "$COMMIT" -- "$ISA_PACK_SRC")"
while IFS= read -r REL; do
	[ -n "$REL" ] || continue
	DEST_REL="$ISA_PACK_DEST/${REL#"$ISA_PACK_SRC"/}"
	git_show_file "$REL" "$STAGE_ROOT/$DEST_REL" || exit 6
	ISA_PACK_FILES="${ISA_PACK_FILES}${DEST_REL}"$'\n'
done <<EOF
$ISA_PACK_TRACKED
EOF
if [ -n "$ISA_PACK_FILES" ]; then
	echo "[make-release] optional pack $ISA_PACK_DEST/: $(printf '%s' "$ISA_PACK_FILES" | grep -c .) file(s) staged"
else
	# Unreachable after the step-4b preflight; kept fatal so the README and
	# NOTICE text that names this pack can never ship without it.
	echo "[make-release] FAIL: no tracked $ISA_PACK_SRC/ at commit $GIT_SHA_SHORT." >&2
	exit 6
fi

# 12d3. The tutorial world (card tutorial-1021). saves/ is gitignored, so the
#       world is BUILT here, never committed: every file tracked under
#       tools/tutorial_world/ plus tests/test_tutorial_build.py at $COMMIT
#       (never the working tree, same rule as every other packaged input) is
#       exported to a temp dir, the builder's own acceptance test runs there,
#       and only if it passes does build_tutorial.py write the world
#       (deterministic: no randomness, fixed seed). A failing test or build
#       writes no zip (exit 6); a commit without the builder is refused
#       (exit 3). The world is staged as saves/<TUTORIAL_WORLD_DIR>/ at the
#       SD root AND under apps/CavEX/ -- the game opens "paths.worlds"
#       = "saves" relative to its CWD, which can resolve to either place (the
#       same "staging both is harmless" rule as config_wii.json and assets/).
TUTORIAL_WORLD_DIR="CavEX Tutorial" # build_tutorial.py's DEFAULT_OUT dir name
TUTORIAL_BUILDER="tools/tutorial_world/build_tutorial.py"
TUTORIAL_TEST="tests/test_tutorial_build.py"
for REL in "$TUTORIAL_BUILDER" "$TUTORIAL_TEST"; do
	if ! git -C "$REPO" cat-file -e "$COMMIT:$REL" 2>/dev/null; then
		echo "[make-release] FAIL: $REL not found at commit $GIT_SHA_SHORT; the release must ship the tutorial world." >&2
		exit 3
	fi
done
TUT_WORK="$(mktemp -d "${TMPDIR:-/tmp}/cavewii-tutorial.XXXXXX")" || {
	echo "[make-release] FAIL: could not create a tutorial-world temp dir" >&2
	exit 6
}
cleanup_stage() { rm -rf "$STAGE_ROOT" "$TUT_WORK"; }
TUT_TRACKED="$(git -C "$REPO" ls-tree -r --name-only "$COMMIT" -- tools/tutorial_world)"
while IFS= read -r REL; do
	[ -n "$REL" ] || continue
	git_show_file "$REL" "$TUT_WORK/src/$REL" || exit 6
done <<EOF2
$TUT_TRACKED
$TUTORIAL_TEST
EOF2
echo "[make-release] tutorial world: running $TUTORIAL_TEST (builds the whole world, ~1-2 min)"
if ! "$PY" -B "$TUT_WORK/src/$TUTORIAL_TEST"; then
	echo "[make-release] FAIL: $TUTORIAL_TEST failed at commit $GIT_SHA_SHORT; no release without a passing tutorial world." >&2
	exit 6
fi
TUT_OUT="$TUT_WORK/out/$TUTORIAL_WORLD_DIR"
if ! "$PY" -B "$TUT_WORK/src/$TUTORIAL_BUILDER" --out "$TUT_OUT"; then
	echo "[make-release] FAIL: $TUTORIAL_BUILDER failed at commit $GIT_SHA_SHORT" >&2
	exit 6
fi
TUT_REGION_OK=0
for MCR in "$TUT_OUT/region/"*.mcr; do
	[ -s "$MCR" ] && TUT_REGION_OK=1
done
if [ ! -s "$TUT_OUT/level.dat" ] || [ "$TUT_REGION_OK" != 1 ]; then
	echo "[make-release] FAIL: $TUTORIAL_BUILDER wrote no level.dat + region/*.mcr under $TUT_OUT" >&2
	exit 6
fi
for ROOT_DIR in "$STAGE_ROOT" "$APP_DIR"; do
	mkdir -p "$ROOT_DIR/saves" || exit 6
	cp -R "$TUT_OUT" "$ROOT_DIR/saves/" || exit 6
done
TUTORIAL_FILES="$(cd "$STAGE_ROOT" && find "saves/$TUTORIAL_WORLD_DIR" -type f | LC_ALL=C sort)"
echo "[make-release] tutorial world saves/$TUTORIAL_WORLD_DIR/: $(printf '%s\n' "$TUTORIAL_FILES" | grep -c .) file(s) staged (SD root + app folder)"

# 12e. texturepacks/README.txt -- no Beta pack is ever bundled; the player
#      supplies their own from a copy of Minecraft Beta 1.7.3 they own.
cat > "$STAGE_ROOT/texturepacks/README.txt" <<'EOF'
CaveWii_v2 ships NO Minecraft Beta 1.7.3 assets -- only the project's own
original/CC0/CC-BY default art and sound under assets/ (see
assets/README.md for exact licenses and credits). This is a hard legal rule
(no Mojang bytes ship in this release), not a missing feature.

Default textures: Earth & Ash, original art made for CaveWii_v2 and
dedicated under CC0 1.0 (assets/earth-and-ash-manifest.json lists every
file with its hash; licence text assets/CC0-1.0.txt).

Optional Isabella II pack: this folder also holds texturepacks/isabella-ii/,
the Isabella II art as a pack of its own. Credit: "Isabella II" by
Bonemouse, licensed under the Creative Commons Attribution 3.0 Unported
License (CC BY 3.0 Unported), http://creativecommons.org/licenses/by/3.0/
-- forum thread: http://www.minecraftforum.net/topic/242175-Isabella/ .
Some of the Isabella II textures shipped here have been modified for
CaveWii_v2. Its README.txt, unedited "About Isabella.txt" and LICENSE.txt
are inside that folder; the same About file and licence text also ship in
LICENSES/Isabella-II/. Pick it on the in-game Texture Packs screen, where it
is listed as 'isabella-ii'. It is a PARTIAL pack: every file it lacks (mobs,
armor, chest screens, etc.) comes from the Earth & Ash default in assets/.

Using your own Beta 1.7.3 look and sound (optional)
---------------------------------------------------
You can only do this if you own a legitimate copy of Minecraft Beta 1.7.3
(its minecraft.jar, and ideally the .minecraft resources folder that holds
the sounds and music). The tools below read YOUR files on YOUR computer and
write a pack onto YOUR SD card. Nothing is downloaded, and nobody else's
copy is involved. Textures AND sounds/music come from the same two sources:
the textures from your jar, the sounds and the C418 music from your
resources folder. Without the resources folder you get textures only.

Route 1 (recommended): the Pack Helper, a small desktop program
  The Pack Helper (CaveWii Asset Importer) is a separate download. It is
  NOT inside this SD-card zip, so you only need it on your PC. If you got a
  combined download, it is the "Pack Helper" folder next to the game files.
  It is three steps on one screen:
    1. Choose your Beta 1.7.3 jar (or texture folder) and, for sounds and
       music, your Minecraft resources folder.
    2. Choose your SD card (its top folder).
    3. Click "Import assets".
  It converts the files for the Wii, checks what it copied, and switches
  CaveWii_v2 to the new pack (paths.texturepack and paths.audiopack in
  config.json) for your next start. Your originals, the bundled art and
  your saves are never touched. Beta-era Minecraft only: modern Java
  resource packs are not supported. Details: docs/pack_helper.md.

Route 2: the command line (needs Python 3 and the CaveWii_v2 source)
  Run tools/import_owned_beta_assets.py with --jar (your minecraft.jar),
  --resources (your resources folder, optional) and --sd (your SD card).
  It writes texturepacks/beta-1.7.3/ on the card. Add --dry-run first to
  see what it would do. Then pick the pack in the game (see "Picking a
  pack" below) or set it in config.json by hand:

         "paths": { "texturepack": "texturepacks/beta-1.7.3" }

  Sounds and music in a pack are used automatically unless you also set
  paths.audiopack to a different pack. Walkthrough, including where to
  find your jar on each system: docs/import_owned_assets.md.

Route 3: by hand
  Put your own extracted Beta 1.7.3 textures, sound/ and music/ folders in
  texturepacks/beta-1.7.3/ on the card (same layout as the Isabella II
  folder here), then pick the pack as below. Sounds must already be in a
  format the game reads, so Route 1 or 2 is much easier.

Picking a pack
  Open the Texture Packs screen in the game and choose a pack ('isabella-ii',
  'beta-1.7.3', or any other folder in texturepacks/). The change applies
  right away, sounds and music included. Earth & Ash stays the default
  until you pick something else.

If a file is missing
  A pack does not have to be complete. Any texture or sound the pack does
  not have is taken from the bundled Earth & Ash art and sound, one file at
  a time, so the game never shows a blank block. A few things have no
  bundled version (the C418 music and jukebox records): without them the
  game is simply silent there. The importer tells you what it skipped.
  The music streams from the SD card.

CaveWii_v2 never downloads, extracts, or redistributes Minecraft assets on
your behalf -- moving your own legally-owned files onto your own SD card is
entirely your responsibility. Nothing in this project reads network content
to obtain them.
EOF

# 12f. LICENSE (must exist -- a release without one is refused) + NOTICE
#      (reuse the repo's own NOTICE if it exists; otherwise synthesize a
#      skeleton naming the vendored third-party components under
#      third_party/ and source/*, marking MAINTAINER TO REVIEW wherever no
#      license file is present in-tree for that component -- never inventing
#      license text).
if ! git -C "$REPO" cat-file -e "$COMMIT:LICENSE" 2>/dev/null; then
	echo "[make-release] FAIL: LICENSE not found at commit $GIT_SHA_SHORT -- refusing to package without one." >&2
	exit 6
fi
git_show_file "LICENSE" "$STAGE_ROOT/LICENSE" || exit 6

NOTICE_SOURCE="repo NOTICE"
if git -C "$REPO" cat-file -e "$COMMIT:NOTICE" 2>/dev/null; then
	git_show_file "NOTICE" "$STAGE_ROOT/NOTICE" || exit 6
else
	NOTICE_SOURCE="generated skeleton (no repo NOTICE at this commit)"
	# has_file <path-at-COMMIT> -> 0/1, used to decide MAINTAINER TO REVIEW.
	has_file() { git -C "$REPO" cat-file -e "$COMMIT:$1" 2>/dev/null; }
	lic_line() {
		local comp="$1" path="$2" lic_path="$3" note="$4"
		if [ -n "$lic_path" ] && has_file "$lic_path"; then
			printf -- '- %s (%s)\n  License file: %s\n  %s\n\n' "$comp" "$path" "$lic_path" "$note"
		else
			printf -- '- %s (%s)\n  License file: NONE FOUND IN-TREE -- MAINTAINER TO REVIEW\n  %s\n\n' "$comp" "$path" "$note"
		fi
	}
	{
		cat <<'EOF'
CaveWii_v2 -- third-party component notices
============================================

This is a GENERATED skeleton (no repo-root NOTICE file existed at packaging
time) listing the third-party source/library components vendored under
third_party/ and source/*, with the license file that ships alongside each
one in this repository, if any. No license text is reproduced or invented
here -- see the named file for the actual terms. Entries marked
"MAINTAINER TO REVIEW" have no license file checked in for that component
in this tree and need a maintainer decision before a public release.

Runtime/binary assets (textures, sounds) shipped under assets/ have their
own licenses, documented in assets/README.md (shipped alongside this file
in the release) -- that file, not this one, is authoritative for asset
credits and covers CC0/CC-BY/GPLv2 grants per file. The bundled default
textures are Earth & Ash, original art dedicated under CC0 1.0
(assets/earth-and-ash-manifest.json).

- Optional texture pack: "Isabella II" by Bonemouse
  License: Creative Commons Attribution 3.0 Unported (CC BY 3.0 Unported),
  http://creativecommons.org/licenses/by/3.0/
  Forum thread: http://www.minecraftforum.net/topic/242175-Isabella/
  Textures by Bonemouse. Files derived from Isabella II (some of them
  modified for CaveWii_v2) ship ONLY in the optional pack folder
  texturepacks/isabella-ii/ (a pick-able, partial pack; files it lacks
  fall back to the Earth & Ash default in assets/): terrain.png,
  particles.png, gui/items.png, terrain/sun.png, terrain/moon.png,
  environment/clouds.png, gui/crafting.png, gui/furnace.png,
  gui/gui.png, gui/icons.png, gui/inventory.png and anim.png (adapted:
  Beta animation motion in Isabella II colours, its own ice tile). The
  folder's pointer.png and font/default.png are original CC0 artwork.
  The pack's own "About Isabella.txt" ships UNEDITED in
  LICENSES/Isabella-II/ and in texturepacks/isabella-ii/.
  License file: LICENSES/Isabella-II/LICENSE.txt (also
  texturepacks/isabella-ii/LICENSE.txt)

EOF
		lic_line "cglm" "source/cglm/" "source/cglm/LICENSE.cglm" \
			"MIT License. Copyright (c) 2015 Recep Aslantas. https://github.com/recp/cglm"
		lic_line "cNBT (vendored subset, chmod222/cNBT)" "source/cNBT/" "" \
			"Only the NBT archive source files CaveWii uses are vendored (source/cNBT/README.md), commit 3ff982eff62b01f7a67079a365b8fbf8a4b47423. Verified 2026-09-02 by reading the header embedded at the top of the vendored files themselves: \"THE BEER-WARE LICENSE\" (Revision 42), Lukas Niederbremer <webmaster@flippeh.de> and Clark Gaebel <cg.wowus.cg@gmail.com> -- NOT zlib, an earlier guess this note has corrected. Full text: LICENSES/cNBT.txt in a real release."
		lic_line "lodepng" "source/lodepng/" "source/lodepng/LICENSE.lodepng" \
			"zlib-style license. Copyright (c) 2005-2018 Lode Vandevenne. https://github.com/lvandeve/lodepng"
		lic_line "parson" "source/parson/" "source/parson/LICENSE.parson" \
			"MIT License. Copyright (c) 2012-2022 Krzysztof Gabis. https://github.com/kgabis/parson"
		lic_line "m-lib" "third_party/m-lib/" "third_party/m-lib/LICENSE.mlib" \
			"BSD 2-Clause License. Copyright (c) 2017-2025, Patrick Pelissier. https://github.com/P-p-H-d/mlib"
		lic_line "miniaudio" "third_party/miniaudio/" "" \
			"No separate LICENSE file; dual-licensed text is embedded verbatim at the bottom of the vendored miniaudio.h itself (v0.11.25, https://miniaud.io) -- verified 2026-09-02 by reading it there. CaveWii uses the Public Domain (unlicense.org) alternative; the file also offers MIT No Attribution (Copyright 2026 David Reid). Full text: LICENSES/miniaudio.txt in a real release."
		lic_line "stb (stb_vorbis.c)" "third_party/stb/" "" \
			"No separate LICENSE file; dual-licensed text is embedded verbatim at the bottom of the vendored stb_vorbis.c itself (v1.22, http://nothings.org/stb_vorbis/) -- verified 2026-09-02 by reading it there. CaveWii uses the Public Domain (unlicense.org) alternative; the file also offers MIT (Copyright (c) 2017 Sean Barrett). Full text: LICENSES/stb-vorbis.txt in a real release."
		lic_line "libogc KERNEL_HEAP sidecar patch set" "third_party/libogc-kernel-heap/" "" \
			"Patches (diffs) against upstream devkitPro/libogc v3.1.0, commit c70bdf2fc36bf6c6c12d946ae3ad70d604366d1b (third_party/libogc-kernel-heap/README.md); libogc itself is NOT vendored into this repo, only the patch files and build recipe. libogc's own license, verified 2026-09-02 against the upstream repo's root LICENSE file: a zlib-style license (Copyright (C) 2004-2025 Michael Wiedenbauer and Dave Murphy) for most of the codebase, BSD-2-Clause for the RTEMS-derived threading code (Copyright OAR / Eric Valette) that patch 0004 touches. Full text: LICENSES/libogc.txt in a real release. CaveWii's own patch text is covered by this project's own LICENSE (GPLv3)."
		lic_line "zlib" "(system-provided by devkitPPC's ppc-zlib package; not vendored in-tree)" "" \
			"CaveWii's Wii build links devkitPPC's ppc-zlib, not a copy of zlib source in this repo. zlib License, Copyright (C) 1995-2026 Jean-loup Gailly and Mark Adler -- verified 2026-09-02 against the canonical https://www.zlib.net/zlib_license.html. Full text: LICENSES/zlib.txt in a real release."
		cat <<'EOF'
- greatest (test framework, third_party/greatest/)
  License file: third_party/greatest/LICENSE (ISC-style permissive)
  Test-only: greatest.h is compiled into the ctest suite's test binaries,
  never into the shipped Wii .dol or the PC app -- not part of this
  release's linked binary, listed here for completeness only.

EOF
	} > "$STAGE_ROOT/NOTICE"
fi
echo "[make-release] NOTICE source: $NOTICE_SOURCE"

# 12f2. LICENSES/ -- full verbatim license text for every third-party
#       component NOTICE cites (GPL-2.0 for the Beta-pack pointer, plus every vendored
#       library's license). Required since REL-01b (2026-10-17): NOTICE
#       and README-RELEASE.txt point at these files, so a release without
#       them would ship broken attribution. The step-4b preflight already
#       refused a commit lacking the Isabella II texts.
if git -C "$REPO" ls-tree -r --name-only "$COMMIT" -- LICENSES | grep -q .; then
	while IFS= read -r REL; do
		[ -n "$REL" ] || continue
		git_show_file "$REL" "$STAGE_ROOT/$REL" || exit 6
	done <<EOF
$(git -C "$REPO" ls-tree -r --name-only "$COMMIT" -- LICENSES)
EOF
	echo "[make-release] LICENSES/: $(git -C "$REPO" ls-tree -r --name-only "$COMMIT" -- LICENSES | grep -c .) file(s) staged"
else
	echo "[make-release] FAIL: no LICENSES/ directory found at commit $GIT_SHA_SHORT." >&2
	exit 6
fi

# 12g. README-RELEASE.txt: what this zip is, how to install it, status.
CERT_LINE="UNCERTIFIED -- no real-Wii PASS.json was supplied for this .dol (--allow-uncertified). Treat this build as a fix candidate, hardware-unconfirmed, per CLAUDE.md's Definition of Done."
if [ "$CERT_STATUS" = "PERFORMANCE_VERIFIED" ]; then
	CERT_LINE="PERFORMANCE VERIFIED -- real-Wii run_id=$PASS_RUN_ID profile=$PASS_PROFILE (see provenance.json). This is profile evidence, not complete visual, controller, parity or splitscreen release acceptance."
fi
cat > "$STAGE_ROOT/README-RELEASE.txt" <<EOF
CaveWii_v2 $RELEASE_VERSION (commit $GIT_SHA_SHORT) -- release zip
====================================================================

Status: $CERT_LINE

Installing from this release zip
---------------------------------
1. Unzip this archive directly onto the ROOT of your Wii's SD card (the
   apps/, assets/, texturepacks/, and config_wii.json entries all land where
   the Homebrew Channel and the game itself expect them). Updating over an
   older install keeps your config.json (multiplayer name, server list and
   settings): the zip carries no config.json, only the config_wii.json
   defaults the game falls back to when yours is missing.
2. Launch "CaveWii_v2" from the Homebrew Channel.
3. New to the game? Choose "Play game", stay on the Load tab, pick the
   world "Tutorial Island" (folder saves/$TUTORIAL_WORLD_DIR/) and press
   "Play world". It is a guided island trail of numbered
   stops with signs and chests that teach the controls, crafting, mining,
   farming, redstone and more. Unzipping a newer release over an old one
   replaces that folder, so copy it elsewhere first to keep your progress.
4. (Optional, recommended) Supply your own legally-owned Minecraft Beta
   1.7.3 texture pack -- see texturepacks/README.txt. Without one, the
   game runs with its bundled default art and sound: the Earth & Ash
   textures (original, CC0) plus the files credited one by one in
   assets/README.md (full per-file credits and licenses).

What's in this zip
-------------------
  apps/CavEX/boot.dol       the game (Homebrew Channel entry point)
  apps/CavEX/meta.xml       Homebrew Channel metadata
  apps/CavEX/icon.png       Homebrew Channel icon
  apps/CavEX/config_wii.json  default settings (Wii controller mapping); the
                              game reads it when there is no config.json and
                              writes your own settings to config.json
  apps/CavEX/assets/        bundled default art+sound (duplicate of below)
  apps/CavEX/saves/$TUTORIAL_WORLD_DIR/  the tutorial world (duplicate of below)
  saves/$TUTORIAL_WORLD_DIR/        the tutorial world "Tutorial Island"
  config_wii.json, assets/        same content at the SD card root
                                  (the game's CWD on boot can resolve to
                                  either location; both are staged so it
                                  works either way -- see
                                  docs/platform_perf_notes.md)
  texturepacks/README.txt        how to add your own Beta 1.7.3 pack
  texturepacks/isabella-ii/      optional Isabella II pack (Bonemouse,
                                  CC BY 3.0 Unported; partial -- missing
                                  files fall back to assets/). Pick it on
                                  the Texture Packs screen; see its
                                  README.txt for credit and licence
  LICENSE                        GPLv3 (this project's license)
  NOTICE                         third-party component attributions
  LICENSES/                      full verbatim license text NOTICE cites
                                  (GPL-2.0.txt for the Beta-pack pointer, plus
                                  every vendored library's license)
  SHA256SUMS                     sha256 of every file in this zip
  provenance.json                exact build/verification record (see below)

No Minecraft Beta assets are included
--------------------------------------
This release ships ZERO Mojang-derived bytes. assets/ is either original
art (CC0 -- including controls.png, armor/*.png, and misc/{grasscolor,
foliagecolor}.png, all replaced 2026-09-02 with from-scratch art after
their prior versions carried unresolved provenance), a GPLv2 asset
(none since 2026-10-11: the Homebrew pointer now ships only in Beta packs
the Pack Helper prepares, see NOTICE), or excluded
entirely where the license could not be confirmed -- see provenance.json's
"assets.excluded" list for exactly what was left out and why. The default
textures are the original CC0 Earth & Ash art, with their hashes and
license in assets/earth-and-ash-manifest.json and assets/CC0-1.0.txt. The
CC-BY-3.0 Isabella II texture-pack art (credited in NOTICE and
texturepacks/isabella-ii/README.txt) ships only in the optional
texturepacks/isabella-ii/ pack, never in assets/.
Older, unverified misc/water.png content remains excluded; the reviewed CC0
replacement is admitted by its exact hash from the selected commit.

Verifying this download
------------------------
  sha256sum -c SHA256SUMS     (run from inside the unzipped directory)

provenance.json records the exact git commit, the .dol's own sha256/md5,
the KERNEL_HEAP/TLS/test-marker gate results, and (when PERFORMANCE_VERIFIED) the
real-Wii PASS.json run this build was verified against.
EOF

# ---------------------------------------------------------------------------
# 13. provenance.json. Every dynamic value crosses the shell/Python boundary
#     as an argv element (bash quotes each "$VAR" as one exact token; no
#     value is ever spliced into Python source text), so arbitrary content
#     (backslashes, quotes, a Windows path) can never break or inject into
#     the generated JSON -- json.dumps() does all the escaping.
# ---------------------------------------------------------------------------
PASS_BASENAME=""
[ -n "$PASS_JSON" ] && PASS_BASENAME="$(basename "$PASS_JSON")"

"$PY" - \
	--out "$STAGE_ROOT/provenance.json" \
	--version "$RELEASE_VERSION" \
	--cert-status "$CERT_STATUS" \
	--git-commit "$GIT_SHA" \
	--git-commit-short "$GIT_SHA_SHORT" \
	--git-commit-ref-arg "$COMMIT" \
	--git-dirty "$GIT_DIRTY" \
	--dol-basename "$DOL_BASENAME" \
	--dol-sha256 "$DOL_SHA256" \
	--dol-md5 "$DOL_MD5" \
	--dol-build-ids "$DOL_BUILD_IDS" \
	--elf-sha256 "$ELF_SHA256" \
	--gate-kernel-heap "$KERNEL_HEAP_GATE_RESULT" \
	--gate-tls "$TLS_GATE_RESULT" \
	--gate-test-marker "$TEST_MARKER_GATE_RESULT" \
	--sidecar-path "$SIDECAR_LIBOGC_A" \
	--sidecar-sha256 "$SIDECAR_LIBOGC_A_SHA256" \
	--gcc-version "$GCC_VERSION" \
	--pass-basename "$PASS_BASENAME" \
	--pass-run-id "$PASS_RUN_ID" \
	--pass-profile "$PASS_PROFILE" \
	--pass-build-id "$PASS_BUILD_ID" \
	--assets-included "$INCLUDED_ASSETS" \
	--assets-excluded "$EXCLUDED_ASSETS" \
	--stage-root "$STAGE_ROOT" \
	--optional-pack-files "$ISA_PACK_FILES" \
	--tutorial-world-dir "$TUTORIAL_WORLD_DIR" \
	--tutorial-world-files "$TUTORIAL_FILES" \
<<'PYEOF'
import argparse, datetime, hashlib, json, os, sys

p = argparse.ArgumentParser()
for name in ["out", "version", "cert-status", "git-commit", "git-commit-short",
             "git-commit-ref-arg", "git-dirty", "dol-basename", "dol-sha256",
             "dol-md5", "dol-build-ids", "elf-sha256", "gate-kernel-heap",
             "gate-tls", "gate-test-marker", "sidecar-path", "sidecar-sha256",
             "gcc-version", "pass-basename", "pass-run-id", "pass-profile",
             "pass-build-id", "assets-included", "assets-excluded",
             "stage-root", "optional-pack-files", "tutorial-world-dir",
             "tutorial-world-files"]:
    p.add_argument("--" + name, default="")
a = p.parse_args()


def lines(s):
    return [l for l in s.splitlines() if l]


excluded_reasons = {
    "assets/misc/water.png": (
        "Not the reviewed Earth & Ash CC0 hash. Historical water artwork was "
        "not traceable to Isabella II (0% pixel match against an archived "
        "copy despite matching 16x16 dimensions) or to any other documented "
        "CC0/CC-BY/GPLv2 credit entry -- see "
        "docs/asset_provenance_2026-09-02.md's Action Items. Genuinely "
        "optional (source/main.c: absent on a default install, the "
        "underwater overlay quad is skipped when texture_water.data is "
        "NULL) -- unlike misc/foliagecolor.png/grasscolor.png, which were "
        "found to have the SAME untraceable-provenance problem but are "
        "loaded unconditionally by biome_color_init() and were therefore "
        "regenerated as original CC0 art (tools/gen_cc0_colormaps.py) and "
        "included, not excluded."
    ),
}

excluded_entries = []
for path in lines(a.assets_excluded):
    reason = excluded_reasons.get(path)
    if reason is None:
        reason = "Excluded by tools/make_release.sh's asset roster; see the script's inline comments for the current exclusion rules."
    excluded_entries.append({"path": path, "reason": reason})

def staged_sha256(rel):
    h = hashlib.sha256()
    with open(os.path.join(a.stage_root, *rel.split("/")), "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


# Optional, pick-able packs staged under texturepacks/ (card ISA-1): every
# file is recorded by its ZIP path and the sha256 of the staged bytes.
optional_pack_files = lines(a.optional_pack_files)
optional_packs = []
if optional_pack_files:
    optional_packs.append({
        "name": "isabella-ii",
        "directory": "texturepacks/isabella-ii",
        "source": "packs/isabella-ii",
        "credit": ("Textures by Bonemouse, 'Isabella II', CC BY 3.0 Unported "
                   "(http://creativecommons.org/licenses/by/3.0/); modified for CaveWii_v2. "
                   "Partial pack: files it lacks fall back to the bundled default assets/."),
        "default": False,
        "files": [{"path": rel, "sha256": staged_sha256(rel)} for rel in optional_pack_files],
    })

data = {
    "schema": "cavewii-release-provenance/1",
    "generated_at_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "tool": "tools/make_release.sh",
    "version": a.version,
    "certification_status": a.cert_status,
    "git": {
        "commit": a.git_commit,
        "commit_short": a.git_commit_short,
        "commit_ref_arg": a.git_commit_ref_arg,
        "working_tree_dirty_at_packaging_time": a.git_dirty == "true",
    },
    "dol": {
        "path_basename": a.dol_basename,
        "sha256": a.dol_sha256,
        "md5": a.dol_md5,
        "embedded_build_ids": lines(a.dol_build_ids) or None,
    },
    "elf": {
        "sha256": a.elf_sha256 or None,
    },
    "gates": {
        "kernel_heap_objdump_gate": a.gate_kernel_heap,
        "tls_gate": a.gate_tls,
        "test_marker_gate": a.gate_test_marker,
    },
    "sidecar_libogc": {
        "path": a.sidecar_path or None,
        "sha256": a.sidecar_sha256 or None,
    },
    "devkitppc_gcc_version": a.gcc_version or None,
    "hardware_pass": (
        {
            "path_basename": a.pass_basename,
            "run_id": a.pass_run_id,
            "profile": a.pass_profile,
            "build_id": a.pass_build_id,
        }
        if a.cert_status == "PERFORMANCE_VERIFIED"
        else None
    ),
    "assets": {
        "included": lines(a.assets_included),
        "excluded": excluded_entries,
    },
    "optional_texturepacks": optional_packs,
    # Card tutorial-1021: built at packaging time from the commit's
    # tools/tutorial_world/ (after tests/test_tutorial_build.py passed);
    # staged at saves/<dir>/ and apps/CavEX/saves/<dir>/ (same bytes).
    "tutorial_world": {
        "directory": "saves/" + a.tutorial_world_dir,
        "app_directory": "apps/CavEX/saves/" + a.tutorial_world_dir,
        "builder": "tools/tutorial_world/build_tutorial.py",
        "acceptance_test": "tests/test_tutorial_build.py",
        # The builder is deterministic in CONTENT (blocks, NBT); the zlib
        # bytes depend on the zlib build (e.g. MSYS2 zlib 1.3.2 vs CPython's
        # zlib-ng), so the file hashes reproduce only with the same one. This
        # is the same $PY that ran the builder.
        "built_with": {"python": sys.version.split()[0],
                       "zlib_runtime": __import__("zlib").ZLIB_RUNTIME_VERSION},
        "files": [{"path": rel, "sha256": staged_sha256(rel)}
                  for rel in lines(a.tutorial_world_files)],
    },
}
with open(a.out, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2, sort_keys=False)
    f.write("\n")
PYEOF
if [ $? -ne 0 ]; then
	echo "[make-release] FAIL: could not write provenance.json" >&2
	exit 6
fi

# ---------------------------------------------------------------------------
# 14. SHA256SUMS over every staged file (computed before zipping, paths
#     relative to the staging root == the eventual zip root).
# ---------------------------------------------------------------------------
(
	cd "$STAGE_ROOT" || exit 6
	find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum \
		| sed 's#\./##' > SHA256SUMS
) || { echo "[make-release] FAIL: could not compute SHA256SUMS" >&2; exit 6; }

# ---------------------------------------------------------------------------
# 15. Zip it up.
# ---------------------------------------------------------------------------
# Python/PowerShell callers supply native Windows paths. Bash's cd accepts
# them, but POSIX mkdir does not reliably interpret backslashes as separators.
# Convert before creating the directory, including when it does not exist yet.
if command -v cygpath >/dev/null 2>&1; then
	OUT_DIR="$(cygpath -u -- "$OUT_DIR")" || exit 6
fi
mkdir -p "$OUT_DIR" || { echo "[make-release] FAIL: could not create --out dir: $OUT_DIR" >&2; exit 6; }
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
# Public product name "CavEX: ReCrafted" (maintainer 11-03; no ':' in a file
# name). A real-Wii-verified zip carries the plain release name; anything else
# keeps the sha and an UNCERTIFIED tag so it can never pass for a release.
if [ "$CERT_STATUS" = "PERFORMANCE_VERIFIED" ]; then
	ZIP_NAME="CavEX-ReCrafted-${RELEASE_VERSION}-Wii"
else
	ZIP_NAME="CavEX-ReCrafted-${RELEASE_VERSION}-${GIT_SHA_SHORT}-Wii-UNCERTIFIED"
fi
ZIP_PATH="$OUT_DIR/${ZIP_NAME}.zip"
rm -f "$ZIP_PATH"

"$PY" - "$STAGE_ROOT" "$ZIP_PATH" <<'PYEOF'
import os, sys, zipfile

stage_root, zip_path = sys.argv[1], sys.argv[2]
files = []
for dirpath, dirnames, filenames in os.walk(stage_root):
    dirnames.sort()
    for name in sorted(filenames):
        full = os.path.join(dirpath, name)
        rel = os.path.relpath(full, stage_root).replace(os.sep, "/")
        files.append((full, rel))
files.sort(key=lambda t: t[1])

with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
    for full, rel in files:
        zf.write(full, rel)
print(f"wrote {len(files)} files to {zip_path}")
PYEOF
if [ $? -ne 0 ]; then
	echo "[make-release] FAIL: zip packaging failed" >&2
	exit 6
fi

# Fail closed: the zip itself must carry the tutorial world at the exact path
# the title Tutorial button opens (saves/<dir>/level.dat + region/*.mcr), at
# both boot CWDs (SD root and apps/CavEX/).
"$PY" - "$ZIP_PATH" "$TUTORIAL_WORLD_DIR" <<'PYEOF2'
import sys, zipfile
zip_path, world = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zip_path) as zf:
    sizes = {i.filename: i.file_size for i in zf.infolist()}
bad = []
for root in ("", "apps/CavEX/"):
    base = f"{root}saves/{world}/"
    if not sizes.get(base + "level.dat"):
        bad.append(base + "level.dat")
    if not any(n.startswith(base + "region/") and n.endswith(".mcr") and sz
               for n, sz in sizes.items()):
        bad.append(base + "region/*.mcr")
if bad:
    print("missing/empty in zip: " + ", ".join(bad), file=sys.stderr)
    sys.exit(1)
PYEOF2
if [ $? -ne 0 ]; then
	echo "[make-release] FAIL: release zip lacks the tutorial world (level.dat + region/*.mcr) at the path the game opens" >&2
	rm -f "$ZIP_PATH"
	exit 6
fi

ZIP_SHA256="$(sha256sum "$ZIP_PATH" | cut -d' ' -f1)"

echo "[make-release] OK: $ZIP_PATH"
echo "[make-release] zip sha256: $ZIP_SHA256"
echo "[make-release] certification status: $CERT_STATUS"
exit 0
