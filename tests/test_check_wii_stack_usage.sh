#!/usr/bin/env bash
# test_check_wii_stack_usage.sh -- KANBAN Reported "tools/check_wii_stack_usage.sh
# broken on the Windows host (09-14)": the script's `make dev` never passed
# LIBOGC_LIB, and the Makefile's sidecar gate (Makefile:40-48) only accepts it
# from the make COMMAND LINE, so the stack gate could not run on this host
# unless a caller smuggled it in through MAKEFLAGS. Fix contract pinned here:
#   1. LIBOGC_LIB (env) is forwarded as a make command-line assignment;
#   2. CW_LIBOGC_LIB (the cwbuild.ps1 variable) is the fallback;
#   3. a caller that already carries LIBOGC_LIB= in MAKEFLAGS still works;
#   4. with no sidecar and no CW_ALLOW_STOCK_LIBOGC=1 the script fails BEFORE
#      touching build-dev or calling make, naming the fix;
#   5. --scan-only parses an existing build dir without building (PASS on
#      small frames, FAIL naming an over-threshold frame), never calls make.
# Runs a COPY of the script inside a scratch repo with a stub make, so it can
# never delete a real build-dev/ or call devkitPPC. No Wii, no network.
set -u

SRC_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/cw_stackcheck.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

FAILS=0
check() { # name cond-rc
	if [ "$2" -eq 0 ]; then echo "PASS $1"; else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi
}

setup() {
	rm -rf "$TMP/repo" "$TMP/dkp" "$TMP/make_args"
	mkdir -p "$TMP/repo/tools" "$TMP/dkp/devkitPPC/bin"
	cp "$SRC_REPO/tools/check_wii_stack_usage.sh" "$TMP/repo/tools/"
	cp "$SRC_REPO/tools/wii_stack_allowlist.txt" "$TMP/repo/tools/"
	# Stub make: record the argv (one per line), build nothing.
	cat > "$TMP/make" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$@" > "$TMP/make_args"
printf 'MAKEFLAGS=%s\n' "\${MAKEFLAGS:-}" >> "$TMP/make_args"
exit 0
EOF
	chmod +x "$TMP/make"
	mkdir -p "$TMP/repo/build-dev"
	echo keep > "$TMP/repo/build-dev/sentinel"
}

run() { # env assignments... -- args...
	env -u LIBOGC_LIB -u CW_LIBOGC_LIB -u CW_ALLOW_STOCK_LIBOGC -u MAKEFLAGS \
		DEVKITPPC="$TMP/dkp/devkitPPC" DEVKITPRO="$TMP/dkp" MAKE="$TMP/make" "$@" \
		> "$TMP/out" 2>&1
}

# 1. LIBOGC_LIB forwarded on the make command line
setup
run LIBOGC_LIB=/x/stage-lib bash "$TMP/repo/tools/check_wii_stack_usage.sh"
grep -qx 'LIBOGC_LIB=/x/stage-lib' "$TMP/make_args" 2>/dev/null
check "LIBOGC_LIB env is passed to make as a command-line assignment" $?

# 2. CW_LIBOGC_LIB fallback
setup
run CW_LIBOGC_LIB=/y/stage-lib bash "$TMP/repo/tools/check_wii_stack_usage.sh"
grep -qx 'LIBOGC_LIB=/y/stage-lib' "$TMP/make_args" 2>/dev/null
check "CW_LIBOGC_LIB is the fallback source for LIBOGC_LIB" $?

# 3. MAKEFLAGS carrier (tools/local_ci.sh container leg) keeps working
setup
run MAKEFLAGS=LIBOGC_LIB=/z/stage-lib bash "$TMP/repo/tools/check_wii_stack_usage.sh"
[ -f "$TMP/make_args" ] && grep -q '^MAKEFLAGS=LIBOGC_LIB=/z/stage-lib$' "$TMP/make_args"
check "LIBOGC_LIB already in MAKEFLAGS is accepted and left to make" $?

# 4. nothing supplied: refuse before make / before deleting build-dev
setup
run bash "$TMP/repo/tools/check_wii_stack_usage.sh"
rc=$?
[ "$rc" -ne 0 ] && [ ! -f "$TMP/make_args" ] && [ -f "$TMP/repo/build-dev/sentinel" ] \
	&& grep -q 'LIBOGC_LIB' "$TMP/out"
check "no sidecar and no CW_ALLOW_STOCK_LIBOGC: fail before make, build-dev intact, names LIBOGC_LIB" $?

# 4b. explicit stock opt-in still builds (CI compile gate), without inventing a path
setup
run CW_ALLOW_STOCK_LIBOGC=1 bash "$TMP/repo/tools/check_wii_stack_usage.sh"
[ -f "$TMP/make_args" ] && ! grep -q '^LIBOGC_LIB=' "$TMP/make_args"
check "CW_ALLOW_STOCK_LIBOGC=1 builds without a LIBOGC_LIB assignment" $?

# 5. --scan-only: parse an existing build dir, never call make
setup
mkdir -p "$TMP/repo/build-dev/source"
printf 'source/main.c:10:5:main\t3816\tstatic\nsource/a.c:1:1:small\t512\tstatic\n' \
	> "$TMP/repo/build-dev/source/main.su"
run bash "$TMP/repo/tools/check_wii_stack_usage.sh" --scan-only
rc=$?
[ "$rc" -eq 0 ] && [ ! -f "$TMP/make_args" ] && grep -q 'VERDICT: PASS' "$TMP/out" \
	&& [ -f "$TMP/repo/build-dev/source/main.su" ]
check "--scan-only PASS on small frames, no make, build dir kept" $?

setup
mkdir -p "$TMP/repo/build-dev/source"
printf 'source/big.c:3:1:huge_frame\t20000\tstatic\n' > "$TMP/repo/build-dev/source/big.su"
run bash "$TMP/repo/tools/check_wii_stack_usage.sh" --scan-only
rc=$?
[ "$rc" -ne 0 ] && [ ! -f "$TMP/make_args" ] && grep -q 'huge_frame' "$TMP/out" \
	&& grep -q 'VERDICT: FAIL' "$TMP/out"
check "--scan-only FAIL names an un-allowlisted 20000 B frame" $?

setup
rm -rf "$TMP/repo/build-dev"
run bash "$TMP/repo/tools/check_wii_stack_usage.sh" --scan-only
rc=$?
[ "$rc" -ne 0 ] && [ ! -f "$TMP/make_args" ]
check "--scan-only with no build dir fails closed" $?

setup
run bash "$TMP/repo/tools/check_wii_stack_usage.sh" --bogus
rc=$?
[ "$rc" -ne 0 ] && [ ! -f "$TMP/make_args" ] && [ -f "$TMP/repo/build-dev/sentinel" ]
check "unknown argument is refused before any build step" $?

if [ "$FAILS" -ne 0 ]; then
	echo "test_check_wii_stack_usage: $FAILS case(s) FAILED"
	exit 1
fi
echo "test_check_wii_stack_usage: all cases PASS"
exit 0
