#!/usr/bin/env bash
# tools/gen_meta_xml.sh -- regenerate meta.xml from tools/meta.xml.in.
#
# Single source of truth for the app's advertised version/date: the repo-root
# VERSION file (version) and the target commit's author date (release_date),
# instead of the old hand-edited "0.0.1-dev" / "20260528000000" that drifted
# for months. Runs standalone (maintainers refreshing the checked-in
# meta.xml at a release cut) and is also called by tools/make_release.sh so
# every packaged zip ships a meta.xml that matches the commit inside it.
#
# Usage:
#   tools/gen_meta_xml.sh [--repo <path>] [--version <v>] [--date <YYYYMMDDHHMMSS>]
#                          [--commit <sha>] [--template <path>] [--out <path>]
#
# Exit codes: 0 = wrote <out>; 2 = bad args/missing template; 3 = git/date
# lookup failed.
set -u -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" || exit 2
REPO="$(cd "$SCRIPT_DIR/.." && pwd)" || exit 2
VERSION=""
DATE=""
COMMIT="HEAD"
TEMPLATE=""
OUT=""

usage() {
	cat <<'EOF'
Usage: tools/gen_meta_xml.sh [--repo <path>] [--version <v>] [--date <YYYYMMDDHHMMSS>]
                              [--commit <sha>] [--template <path>] [--out <path>]

Defaults: --repo the repo containing this script; --version the repo's
VERSION file content; --date the author date of --commit (default HEAD)
formatted YYYYMMDDHHMMSS (the Homebrew Channel meta.xml release_date form);
--template tools/meta.xml.in; --out meta.xml (repo root).
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--repo) REPO="$2"; shift 2 ;;
		--version) VERSION="$2"; shift 2 ;;
		--date) DATE="$2"; shift 2 ;;
		--commit) COMMIT="$2"; shift 2 ;;
		--template) TEMPLATE="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "[gen-meta-xml] FAIL: unknown argument: $1" >&2; usage >&2; exit 2 ;;
	esac
done

[ -n "$TEMPLATE" ] || TEMPLATE="$REPO/tools/meta.xml.in"
[ -n "$OUT" ] || OUT="$REPO/meta.xml"

if [ ! -f "$TEMPLATE" ]; then
	echo "[gen-meta-xml] FAIL: template not found: $TEMPLATE" >&2
	exit 2
fi

if [ -z "$VERSION" ]; then
	if [ -f "$REPO/VERSION" ]; then
		VERSION="$(tr -d '[:space:]' < "$REPO/VERSION")"
	else
		echo "[gen-meta-xml] FAIL: no --version given and $REPO/VERSION does not exist" >&2
		exit 2
	fi
fi
if [ -z "$VERSION" ]; then
	echo "[gen-meta-xml] FAIL: resolved VERSION is empty" >&2
	exit 2
fi

if [ -z "$DATE" ]; then
	DATE="$(git -C "$REPO" log -1 --format=%cd --date=format:%Y%m%d%H%M%S "$COMMIT" 2>/dev/null)" || {
		echo "[gen-meta-xml] FAIL: could not read the commit date for $COMMIT (not a git repo, or bad commit?)" >&2
		exit 3
	}
fi
if [ -z "$DATE" ]; then
	echo "[gen-meta-xml] FAIL: resolved release_date is empty" >&2
	exit 3
fi
case "$DATE" in
	[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]) ;;
	*) echo "[gen-meta-xml] FAIL: release_date '$DATE' is not YYYYMMDDHHMMSS" >&2; exit 3 ;;
esac

# sed replacement text must not contain sed metacharacters given how VERSION
# strings are formed (dotted digits, hyphen, plus, alnum) -- assert the
# expected shape rather than trying to escape an arbitrary string.
case "$VERSION" in
	*[!A-Za-z0-9.+_-]*)
		echo "[gen-meta-xml] FAIL: VERSION '$VERSION' has characters outside [A-Za-z0-9.+_-]" >&2
		exit 2
		;;
esac

sed -e "s/@CW_VERSION@/$VERSION/" -e "s/@CW_RELEASE_DATE@/$DATE/" "$TEMPLATE" > "$OUT" || {
	echo "[gen-meta-xml] FAIL: sed substitution into $OUT failed" >&2
	exit 2
}

echo "[gen-meta-xml] OK: wrote $OUT (version=$VERSION release_date=$DATE)"
exit 0
