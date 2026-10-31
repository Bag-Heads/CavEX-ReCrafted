#!/bin/sh
# Print the build's source identity. Clean trees use the seven-character HEAD;
# dirty trees append an eight-character digest of the complete staged-equivalent
# worktree (tracked edits/deletes plus every non-ignored untracked file).
set -eu

# Direct Windows/MSYS invocations can inherit a PATH containing only Windows
# tools even though this script is running under /usr/bin/sh.
if [ -d /usr/bin ]; then
	PATH="/usr/bin:$PATH"
	export PATH
fi

repo=${1:-.}
head=$(git -C "$repo" rev-parse --short=7 HEAD)
status=$(git -C "$repo" status --porcelain --untracked-files=all)
if [ -z "$status" ]; then
	printf '%s\n' "$head"
	exit 0
fi

tmp_index=$(mktemp "${TMPDIR:-/tmp}/cavewii-stamp-index.XXXXXX")
rm -f "$tmp_index"
add_log="$tmp_index.add-log"
trap 'rm -f "$tmp_index" "$add_log"' EXIT HUP INT TERM
GIT_INDEX_FILE="$tmp_index" git -C "$repo" read-tree HEAD
if ! GIT_INDEX_FILE="$tmp_index" git -C "$repo" add -A -- . 2>"$add_log"; then
	cat "$add_log" >&2
	exit 1
fi
rm -f "$add_log"
tree=$(GIT_INDEX_FILE="$tmp_index" git -C "$repo" write-tree)
# Include porcelain-v2 state as well as the synthetic tree so a dirty submodule
# or index/worktree distinction cannot alias a different hardware candidate.
digest=$(
	{
		printf '%s\0' "$tree"
		git -C "$repo" status --porcelain=v2 -z --untracked-files=all
	} | git -C "$repo" hash-object --stdin
)
printf '%s+%.8s\n' "$head" "$digest"
