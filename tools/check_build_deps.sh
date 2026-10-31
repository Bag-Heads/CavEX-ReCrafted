#!/usr/bin/env bash
# hw1029b -- post-build REFUSAL GATE on a foreign or stale object directory.
#
# WHAT THIS REFUSES
# A Wii build whose object directory (build/ or build-dev/) was not produced
# from THIS source tree, or holds an object older than a header it depends on.
#
# WHY (2026-10-29, hw1029b root cause): the release .dol was built in runner
# clone C:/tmp/cw-hw1029b whose build/ had been COPIED from C:/tmp/cw-hw1029a.
# Every gcc .d file still named cw-hw1029a's headers, so make compared the
# objects against the OTHER clone's (unchanged) headers. A client_session.h
# struct change rebuilt only main.o (touched by the stamp step); every other
# object kept the old struct layout -> mixed offsets -> DSI on the first frame.
# md5, build ID, TLS, test marker and sd0 DMA all passed: none of them looks at
# what the objects were compiled against. This gate does.
#
# RULES (each .d file is gcc -MMD output: "<obj>: <src> <hdr> ... \" plus
# phony "<hdr>:" lines):
#   (a) every absolute dependency path must lie under the source root or under
#       an allowed toolchain root (DEVKITPRO etc., passed as extra args);
#   (b) no dependency that exists may be newer than the object that lists it.
# Paths are compared case-insensitively after folding backslashes to '/' and
# "X:/..." to "/x/...", so MSYS, Windows and mixed spellings of a path agree.
#
# Usage: check_build_deps.sh <build-dir> <source-root> [allowed-root ...]
# Exit:  0 OK; 1 REFUSED (foreign or stale objects -- wipe the build dir and
#        rebuild); 2 FAIL CLOSED (no build dir, no .d files, unparsable input).
set -u

if [ "$#" -lt 2 ]; then
  echo "[build-deps] FAIL CLOSED: usage: check_build_deps.sh <build-dir> <source-root> [allowed-root ...]"
  exit 2
fi
BUILD_DIR="$1"
SRC_ROOT="$2"
shift 2

if [ ! -d "$BUILD_DIR" ]; then
  echo "[build-deps] FAIL CLOSED: build dir not found: $BUILD_DIR"
  exit 2
fi
if [ ! -d "$SRC_ROOT" ]; then
  echo "[build-deps] FAIL CLOSED: source root not found: $SRC_ROOT"
  exit 2
fi

# Fold one ROOT path (a handful of calls; dependency paths fold inside awk).
norm() {
  printf '%s\n' "$1" | sed -e 's#\\#/#g' -e 's#^\([A-Za-z]\):/#/\1/#' -e 's#//*#/#g' \
    -e 's#/$##' | tr 'A-Z' 'a-z'
}

NL='
'
ALLOWED=""
# MSYS: also allow the Windows spelling of each root (/opt/devkitpro is really
# C:/msys64/opt/devkitpro, and gcc writes the Windows form into .d files).
add_root() {
  [ -n "$1" ] || return 0
  ALLOWED="$ALLOWED$(norm "$1")$NL"
  if command -v cygpath >/dev/null 2>&1; then
    local W
    W=$(cygpath -m "$1" 2>/dev/null) && [ -n "$W" ] && ALLOWED="$ALLOWED$(norm "$W")$NL"
  fi
  return 0
}
add_root "$SRC_ROOT"
for R in "$@"; do add_root "$R"; done

shopt -s nullglob
DFILES=("$BUILD_DIR"/*.d)
shopt -u nullglob
if [ "${#DFILES[@]}" -eq 0 ]; then
  echo "[build-deps] FAIL CLOSED: no .d dependency files in $BUILD_DIR -- cannot prove what the objects were built against"
  exit 2
fi

# Parse the FIRST rule of each .d (continuations joined; the trailing phony
# "<hdr>:" lines only repeat the same deps) and classify every dependency:
#   F <tab> obj <tab> dep   absolute path outside every allowed root
#   C <tab> obj <tab> dep   dependency to mtime-check against obj
#   BAD <tab> file          no "<obj>:" target token
PAIRS=$(ALLOWED="$ALLOWED" awk '
  function fold(p) {
    gsub(/\\/, "/", p)
    if (p ~ /^[A-Za-z]:\//) p = "/" substr(p, 1, 1) substr(p, 3)
    gsub(/\/\/+/, "/", p)
    return tolower(p)
  }
  BEGIN { nr = split(ENVIRON["ALLOWED"], roots, "\n") }
  FNR == 1 { done = 0; rule = "" }
  done { next }
  {
    line = $0
    sub(/\r$/, "", line)
    cont = sub(/\\[ \t]*$/, "", line)
    rule = rule " " line
    if (cont) next
    done = 1
    n = split(rule, tok, /[ \t]+/)
    obj = ""
    for (i = 1; i <= n; i++) {
      t = tok[i]
      if (obj != "" && ((obj, t) in seen)) continue
      if (obj != "") seen[obj, t] = 1
      if (t == "") continue
      if (obj == "") {
        if (t ~ /:$/) { obj = substr(t, 1, length(t) - 1); continue }
        break
      }
      if (t ~ /^\// || t ~ /^[A-Za-z]:[\\\/]/) {
        f = fold(t); ok = 0
        for (r = 1; r <= nr; r++)
          if (roots[r] != "" && index(f, roots[r] "/") == 1) { ok = 1; break }
        if (!ok) { print "F\t" obj "\t" t; continue }
      }
      print "C\t" obj "\t" t
    }
    if (obj == "") print "BAD\t" FILENAME
  }' "${DFILES[@]}") || {
  echo "[build-deps] FAIL CLOSED: could not parse the .d files in $BUILD_DIR"
  exit 2
}

FOREIGN=0
STALE=0
SHOWN=0
FIRST_FOREIGN=""
while IFS=$'\t' read -r KIND OBJ DEP; do
  case "$KIND" in
    BAD)
      echo "[build-deps] FAIL CLOSED: unparsable dependency file: $OBJ"
      exit 2 ;;
    F)
      FOREIGN=$((FOREIGN + 1))
      [ -n "$FIRST_FOREIGN" ] || FIRST_FOREIGN="$DEP"
      if [ "$SHOWN" -lt 10 ]; then
        echo "[build-deps]   foreign: $OBJ <- $DEP"
        SHOWN=$((SHOWN + 1))
      fi ;;
    C)
      case "$DEP" in
        /*|[A-Za-z]:*) DPATH="$DEP" ;;
        *) DPATH="$BUILD_DIR/$DEP" ;;
      esac
      case "$OBJ" in
        /*|[A-Za-z]:*) OPATH="$OBJ" ;;
        *) OPATH="$BUILD_DIR/$OBJ" ;;
      esac
      if [ -f "$OPATH" ] && [ -e "$DPATH" ] && [ "$DPATH" -nt "$OPATH" ]; then
        STALE=$((STALE + 1))
        if [ "$SHOWN" -lt 10 ]; then
          echo "[build-deps]   stale:   $OBJ is older than $DEP"
          SHOWN=$((SHOWN + 1))
        fi
      fi ;;
  esac
done <<EOF
$PAIRS
EOF

if [ "$FOREIGN" -gt 0 ] || [ "$STALE" -gt 0 ]; then
  echo "[build-deps] REFUSED: $BUILD_DIR is not a clean build of $SRC_ROOT"
  echo "[build-deps]   foreign dependency paths: $FOREIGN  (first: ${FIRST_FOREIGN:-none})"
  echo "[build-deps]   objects older than a dependency: $STALE"
  echo "[build-deps]   A build dir copied from another clone keeps that clone's header paths, so"
  echo "[build-deps]   header edits here never rebuild its objects (hw1029b: mixed struct offsets"
  echo "[build-deps]   -> DSI on the first frame). NEVER copy build/ between clones."
  echo "[build-deps]   Fix: rm -rf $BUILD_DIR and rebuild."
  exit 1
fi
echo "[build-deps] OK: ${#DFILES[@]} .d files, every dependency under the source/toolchain roots, no stale objects"
exit 0
