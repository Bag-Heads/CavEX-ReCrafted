#!/usr/bin/env bash
#
# upload_proof.sh -- automate the CaveWii_v2 fix-commit DoD proof upload.
#
# CLAUDE.md requires every FIX COMMIT to ship a screenshot/GIF on the ShotHost
# gallery (http://192.0.2.18:8099/) showing the claimed behaviour. Today that
# is a manual, easy-to-forget:
#     curl -X PUT --data-binary @file.png http://192.0.2.18:8099/<name>.png
#
# This wraps it into one command that (a) stamps the git short-SHA into the name so
# the commit is obvious, (b) PUTs the file, (c) HARD-VERIFIES the 201, and (d)
# prints the gallery URL ready to paste into the commit/PR body. It removes the
# per-commit friction without changing any game code (faithfulness-neutral tooling).
#
# Usage:
#   tools/upload_proof.sh <file.png|file.gif> <label>   # upload a specific file
#   tools/upload_proof.sh --latest <label>              # upload the newest ss_*.png in CWD
#                                                       #   (the debug.auto_screenshot stream)
#   tools/upload_proof.sh --list-pending                # show proofs queued after failed uploads
#   tools/upload_proof.sh --replay-pending              # retry queued proofs against GALLERY
#   tools/upload_proof.sh --selftest                    # prove the rig end-to-end (uploads a marker)
#
# Env overrides:
#   GALLERY=http://host:port                 gallery base (default the project's :8099)
#   PROOF_UPLOAD_QUEUE=tmp/proof_upload_queue  failed-upload spool directory
#   PROOF_QUEUE_ON_FAIL=0                    disable failed-upload spooling
#   PROOF_CURL_MAX_TIME=60                   curl whole-transfer timeout seconds
#
# Naming: <label>-<git-short-sha>[-dirty].<ext>  e.g.  m2-spider-leap-590561c.png
set -euo pipefail

GALLERY="${GALLERY:-http://192.0.2.18:8099}"
QUEUE_DIR="${PROOF_UPLOAD_QUEUE:-tmp/proof_upload_queue}"
QUEUE_ON_FAIL="${PROOF_QUEUE_ON_FAIL:-1}"
CURL_MAX_TIME="${PROOF_CURL_MAX_TIME:-60}"
UPLOAD_ERROR=""

die() { echo "upload_proof: $*" >&2; exit 1; }

git_stamp() {
	local sha dirty=""
	sha="$(git rev-parse --short HEAD 2>/dev/null || echo nogit)"
	# mark a dirty tree so a proof from uncommitted work is never mistaken for the commit
	if ! git diff --quiet 2>/dev/null || ! git diff --cached --quiet 2>/dev/null; then
		dirty="-dirty"
	fi
	printf '%s%s' "$sha" "$dirty"
}

# slugify a label into a safe filename fragment
slug() { echo "$1" | tr '[:upper:] ' '[:lower:]-' | tr -cd 'a-z0-9._-'; }

file_hash() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{print $1}'
	else
		cksum "$1" | awk '{print $1 ":" $2}'
	fi
}

record_get() { # <record> <key>
	sed -n "s/^$2=//p" "$1" | head -n 1
}

queue_pending() { # <file> <remote-name> <reason>
	local file="$1" name="$2" reason="$3" queued record sum now
	[ "$QUEUE_ON_FAIL" != "0" ] || return 0
	mkdir -p "$QUEUE_DIR"
	queued="$QUEUE_DIR/$name"
	record="$queued.pending"
	cp "$file" "$queued"
	sum="$(file_hash "$queued")"
	now="$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || date)"
	{
		echo "created_utc=$now"
		echo "source_file=$file"
		echo "queued_file=$queued"
		echo "remote_name=$name"
		echo "gallery=$GALLERY"
		echo "url=$GALLERY/$name"
		echo "sha256=$sum"
		echo "reason=$reason"
	} > "$record"
	echo "upload_proof: queued pending proof: $record" >&2
	echo "upload_proof: retry later with: tools/upload_proof.sh --replay-pending" >&2
}

try_upload() { # <file> <remote-name>
	local file="$1" name="$2" code
	[ -f "$file" ] || die "file not found: $file"
	UPLOAD_ERROR=""
	code="$(curl -sS -m "$CURL_MAX_TIME" -X PUT --data-binary @"$file" \
		-o /dev/null -w '%{http_code}' "$GALLERY/$name")" \
		|| { UPLOAD_ERROR="curl failed"; return 2; }
	if [ "$code" != "201" ] && [ "$code" != "200" ]; then
		UPLOAD_ERROR="upload rejected: HTTP $code for $GALLERY/$name"
		return 3
	fi
	echo "$GALLERY/$name"
}

upload() { # <file> <remote-name>
	local file="$1" name="$2" url
	if url="$(try_upload "$file" "$name")"; then
		rm -f "$QUEUE_DIR/$name" "$QUEUE_DIR/$name.pending" 2>/dev/null || true
		echo "$url"
		return 0
	fi
	queue_pending "$file" "$name" "${UPLOAD_ERROR:-upload failed}"
	die "${UPLOAD_ERROR:-upload failed} (is the gallery $GALLERY reachable?)"
}

list_pending() {
	local record any=0
	if [ ! -d "$QUEUE_DIR" ]; then
		echo "No pending proof uploads in $QUEUE_DIR"
		return 0
	fi
	for record in "$QUEUE_DIR"/*.pending; do
		[ -e "$record" ] || continue
		any=1
		echo "$(record_get "$record" url)  file=$(record_get "$record" queued_file)  sha256=$(record_get "$record" sha256)"
	done
	[ "$any" -eq 1 ] || echo "No pending proof uploads in $QUEUE_DIR"
}

replay_pending() {
	local record file name url any=0 rc=0
	if [ ! -d "$QUEUE_DIR" ]; then
		echo "No pending proof uploads in $QUEUE_DIR"
		return 0
	fi
	for record in "$QUEUE_DIR"/*.pending; do
		[ -e "$record" ] || continue
		any=1
		file="$(record_get "$record" queued_file)"
		name="$(record_get "$record" remote_name)"
		if [ ! -f "$file" ]; then
			echo "upload_proof: pending file missing for $record: $file" >&2
			rc=1
			continue
		fi
		if url="$(try_upload "$file" "$name")"; then
			rm -f "$record" "$file"
			echo "UPLOADED pending $file -> $url"
		else
			echo "upload_proof: still pending $record: ${UPLOAD_ERROR:-upload failed}" >&2
			rc=1
		fi
	done
	if [ "$any" -eq 0 ]; then
		echo "No pending proof uploads in $QUEUE_DIR"
	fi
	return "$rc"
}

case "${1:-}" in
	--selftest)
		# Prove the whole path without depending on a capture: generate a tiny marker PNG.
		tmp="$(mktemp --suffix=.png 2>/dev/null || echo "${TMPDIR:-/tmp}/upload_proof_selftest.png")"
		# minimal 1x1 transparent PNG (base64) -- no image lib needed
		base64 -d > "$tmp" <<'B64'
iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+M9QDwADhgGAWjR9
awAAAABJRU5ErkJggg==
B64
		url="$(upload "$tmp" "upload-proof-selftest-$(git_stamp).png")"
		rm -f "$tmp"
		echo "SELFTEST OK -> $url"
		;;
	--list-pending)
		list_pending
		;;
	--replay-pending)
		replay_pending
		;;
	--latest)
		[ $# -ge 2 ] || die "usage: upload_proof.sh --latest <label>"
		file="$(ls -t ss_*.png 2>/dev/null | head -1)" \
			|| true
		[ -n "${file:-}" ] || die "no ss_*.png in $(pwd) -- run the exe with debug.auto_screenshot first"
		url="$(upload "$file" "$(slug "$2")-$(git_stamp).png")"
		echo "UPLOADED $file -> $url"
		;;
	"" | -h | --help)
		sed -n '2,30p' "$0"
		;;
	*)
		[ $# -ge 2 ] || die "usage: upload_proof.sh <file> <label>   (or --latest <label> / --selftest)"
		ext="${1##*.}"
		url="$(upload "$1" "$(slug "$2")-$(git_stamp).$ext")"
		echo "UPLOADED $1 -> $url"
		;;
esac
