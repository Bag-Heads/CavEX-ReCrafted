#!/bin/sh
# bootdeath-0917 / K12 -- post-link REFUSAL GATE on libogc's raw SD DMA buffer.
#
# WHAT THIS REFUSES
# A Wii image in which libogc's `__sd0_functions` -- the 64-byte buffer handed
# RAW to the SD host controller as the DMA target of CMD6 SWITCH_FUNC -- lies
# ACROSS a 4 KB line. Such an image dies before the FAT mount with zero boot
# trace: the Wii stays pingable, HBC and the stage server never come up, the
# telemetry collector cannot connect, and not one line of the build's own trace
# is emitted. There is nothing to debug from afterwards, which is why this is a
# build-time gate.
#
# THE EVIDENCE (tmp/jobs/bootdeath-0917/EXPERIMENT-3.md, sections 2 and 4 and
# its CONCLUSION; real Wii, heartbeat_selftest, predictions written BEFORE each
# run). A double dissociation, using an experiment-only libogc with a 32-byte
# shim placed directly after the buffer to move it independently of everything
# else in the image:
#   4c "straddle WITHOUT the dying __bss_end slot"  -> DIES 2/2
#   4b "the dying __bss_end slot WITHOUT a straddle" -> BOOTS 2/2
# In 4b every other candidate (__Arena1Lo, the staged argv block, the
# KERNEL_HEAP base, both stacks, __conf_buffer, __sd0_cid) sat at the DYING
# build's exact addresses and the image booted anyway. So the boot death is a
# function of this one buffer's placement, and the `__bss_end` page offset
# 0x3dc this gate used to refuse was only a PROXY for it. Across all nine
# artifacts with a hardware verdict on record, "crosses a 4 KB line" separates
# the four deaths from the five boots with no exceptions.
#
# THE MECHANISM IS A HYPOTHESIS. libogc v3.1.0 wiisd.c:150 declares
# `static u8 __sd0_functions[64] ATTRIBUTE_ALIGN(32);` and passes it straight to
# IOS as `request->dma_addr` (wiisd.c:526/533 -> __sdio_sendcommand, isdma = 1),
# unlike every other SD transfer, which goes through the iosAlloc'ed rw_buffer.
# That call runs during SD init, before FAT mounts -- matching the zero-trace
# signature. SD host controllers raise a DMA-boundary interrupt at the SDMA
# buffer boundary (spec default 4 KB), and a driver that never re-arms there
# never completes the transfer. That IOS-side story is NOT proven; only the
# layout correlation is. The gate stands on the correlation, not the story.
#
# WHAT A PASS DOES *NOT* MEAN (Astra: "an aligned address alone does not prove
# patch provenance"). This gate reads a LAYOUT INVARIANT out of the linked .elf.
# It proves the buffer is 64-byte aligned and does not straddle a 4 KB line. It
# does NOT prove which libogc archive was linked, that the archive really is the
# sidecar, or that the sidecar carries patch 0005 -- an unpatched library can
# land on an acceptable address by luck (artifact `p256` did exactly that and
# booted). Provenance is established by the sidecar's archive hash, recorded at
# build time; this gate is the layout half only.
#
# FAIL CLOSED. A missing tool, a missing .elf, a missing/ambiguous/undefined
# symbol, an unexpected size, or unparsable output is a refusal, not a skip --
# an unverified layout is exactly the case this gate exists to stop.
#
# Usage:   sh tools/check_sd0_dma_buffer.sh <linked.elf> [path-to-powerpc-eabi-nm]
# Exit:    0 = accepted (prints one line: address, size, page offset)
#          1 = REFUSED: the layout is one a human must fix before this ships
#          2 = FAIL CLOSED: could not establish the layout (tool/elf/symbol/parse)
#
# POSIX sh only (no bashisms): this runs from the Makefile's pass-2 sub-make
# under MSYS /bin/sh on Windows and under the CI container's /bin/sh, and from
# build-verify.sh under bash. Arithmetic is done on the LAST THREE HEX DIGITS of
# the address, never on the full 0x8xxxxxxx value, so a shell with 32-bit signed
# arithmetic cannot overflow it into a wrong answer. The low 6 bits needed for
# the alignment test live inside those three digits.

TAG='[bootdeath-0917]'
SYMBOL='__sd0_functions'
WANT_SIZE=64      # 0x40, libogc wiisd.c:150 `static u8 __sd0_functions[64]`
PAGE=4096         # the 4 KB line the SD host controller breaks transfers on

fail_closed() {
	echo "$TAG sd0-dma gate FAIL CLOSED: $1" >&2
	echo "$TAG   Refusing to certify the SD DMA buffer layout without a readable symbol" >&2
	echo "$TAG   table. This is a hard failure on purpose (bootdeath-0917 / K12)." >&2
	exit 2
}

ELF="$1"
if [ -z "$ELF" ]; then
	fail_closed "no .elf argument given (usage: check_sd0_dma_buffer.sh <linked.elf> [nm])"
fi

NM="$2"
if [ -z "$NM" ]; then
	if [ -n "${DEVKITPPC:-}" ]; then
		NM="$DEVKITPPC/bin/powerpc-eabi-nm"
	else
		NM="powerpc-eabi-nm"
	fi
fi

if [ ! -f "$ELF" ]; then
	fail_closed "linked .elf not found: $ELF"
fi

# An absolute/relative path must be executable; a bare name must resolve on PATH.
case "$NM" in
	*/*)
		[ -x "$NM" ] || fail_closed "nm not found/executable: $NM"
		;;
	*)
		command -v "$NM" >/dev/null 2>&1 || fail_closed "nm not found on PATH: $NM"
		;;
esac

# -S adds the SIZE column, which rule (b) needs.
NM_OUT=$("$NM" -S "$ELF" 2>&1) || fail_closed "nm exited non-zero on $ELF"
if [ -z "$NM_OUT" ]; then
	fail_closed "nm produced no output for $ELF"
fi

# (a) EXACTLY ONE entry with this exact name, and that entry must be a defined,
# sized definition.
#
# Astra 2026-09-17 (ruling 3, A1): count EVERY exact-name occurrence first --
# a clean definition sitting next to an undefined (U), unsized (three-field)
# or other-type entry of the same name is ambiguous, never a pass. Only after
# exactly one occurrence is established is that one line validated.
#
# `nm -S` prints "<address> <size> <type> <name>" for a defined sized symbol,
# "<address> <type> <name>" when it has no size, and "<spaces> U <name>" for an
# undefined one -- so requiring four fields is what makes "defined AND sized"
# a single test. bBdD covers .bss and .data, local and global.
EXACT=$(printf '%s\n' "$NM_OUT" | awk -v s="$SYMBOL" 'NF >= 2 && $NF == s { print }')
EXACT_COUNT=$(printf '%s' "$EXACT" | grep -c '[^[:space:]]')

# An LTO/IPA clone ("__sd0_functions.0", "__sd0_functions.part.1") means the
# object we measured is not necessarily the one IOS is handed. Never guess.
SUFFIXED=$(printf '%s\n' "$NM_OUT" | awk -v p="$SYMBOL." \
	'NF >= 2 && index($NF, p) == 1 { print $NF }')
if [ -n "$SUFFIXED" ]; then
	fail_closed "ambiguous: $ELF also defines suffixed clone(s) of $SYMBOL: $(printf '%s' "$SUFFIXED" | tr '\n' ' ')"
fi

if [ "$EXACT_COUNT" -eq 0 ]; then
	fail_closed "$SYMBOL is missing from $ELF (is this a Wii link against libogc?)"
fi
if [ "$EXACT_COUNT" -gt 1 ]; then
	fail_closed "ambiguous: $EXACT_COUNT exact-name entries for $SYMBOL in $ELF (expected exactly one; undefined/unsized/other-type entries count)"
fi

DEFINED=$(printf '%s\n' "$EXACT" | awk 'NF == 4 && $3 ~ /^[bBdD]$/ { print }')
COUNT=$(printf '%s' "$DEFINED" | grep -c '[^[:space:]]')
if [ "$COUNT" -ne 1 ]; then
	fail_closed "$SYMBOL is present but UNDEFINED or unsized in $ELF -- no address/size to check"
fi

ADDR=$(printf '%s\n' "$DEFINED" | awk '{ print $1; exit }')
SIZE_HEX=$(printf '%s\n' "$DEFINED" | awk '{ print $2; exit }')

check_hex() {
	CHECK_NAME="$1"
	CHECK_VALUE="$2"
	case "$CHECK_VALUE" in
		'' | *[!0-9a-fA-F]*)
			fail_closed "cannot parse the $SYMBOL $CHECK_NAME from nm output: '$CHECK_VALUE' (not hexadecimal)"
			;;
	esac
}
check_hex address "$ADDR"
check_hex size "$SIZE_HEX"
if [ "${#ADDR}" -lt 4 ]; then
	fail_closed "cannot parse the $SYMBOL address from nm output: '$ADDR' (too short for a page offset)"
fi

# (b) The size is the rule's whole basis: 64 bytes is what the experiments
# measured and what "crosses a 4 KB line" was derived for. A different size
# means libogc changed underneath us and the rule must be re-derived, not
# silently re-applied to a different object.
SIZE=$(( 0x$SIZE_HEX ))
if [ "$SIZE" -ne "$WANT_SIZE" ]; then
	echo "$TAG sd0-dma gate FAIL CLOSED: $SYMBOL is 0x$SIZE_HEX bytes, expected 0x40" >&2
	echo "$TAG   libogc changed; re-derive the rule. The 4 KB-crossing test in this gate was" >&2
	echo "$TAG   derived on real hardware for a 64-byte buffer (EXPERIMENT-3.md); it must not" >&2
	echo "$TAG   be applied to a different object on the assumption that it still holds." >&2
	exit 2
fi

# Last three hex digits == the 4 KB page offset. Overflow-proof (max 0xfff), and
# the low 6 bits the alignment test needs are inside it.
OFFSET_HEX=${ADDR#"${ADDR%???}"}
OFFSET=$(( 0x$OFFSET_HEX ))
MISALIGN=$(( OFFSET & 63 ))
END=$(( OFFSET + SIZE ))

# (c) and (d) are evaluated together and reported together. (c) implies (d) at
# this size, but they mean different things -- (d) is the hardware-proven death
# condition, (c) is the "patch 0005 is in this link" expectation -- so a build
# that fails both is told about both rather than only the first one tripped.
if [ "$MISALIGN" -eq 0 ] && [ "$END" -le "$PAGE" ]; then
	printf '%s sd0-dma gate OK: %s=0x%s size=0x%x page_offset=0x%03x..0x%03x (64-byte aligned, no 4 KB crossing; bootdeath-0917 / K12)\n' \
		"$TAG" "$SYMBOL" "$ADDR" "$SIZE" "$OFFSET" "$(( END - 1 ))"
	exit 0
fi

{
	echo "$TAG sd0-dma gate REFUSED: $ELF"
	printf '%s   %s = 0x%s  size 0x%x  page offset 0x%03x..0x%03x\n' \
		"$TAG" "$SYMBOL" "$ADDR" "$SIZE" "$OFFSET" "$(( END - 1 ))"
	if [ "$END" -gt "$PAGE" ]; then
		echo "$TAG   (d) CROSSES A 4 KB LINE: this image will die before FAT on real hardware."
		echo "$TAG       This is the proven K12 boot-death condition -- pre-FAT, zero boot trace,"
		echo "$TAG       no card output and no network. Four of four artifacts on record whose"
		echo "$TAG       buffer straddled a 4 KB line died; five of five that did not, booted"
		echo "$TAG       (tmp/jobs/bootdeath-0917/EXPERIMENT-3.md)."
	fi
	if [ "$MISALIGN" -ne 0 ]; then
		printf '%s   (c) NOT 64-BYTE ALIGNED (low 6 bits = 0x%02x): the linked libogc is not the\n' \
			"$TAG" "$MISALIGN"
		echo "$TAG       sidecar carrying patch 0005. Stock libogc declares this buffer"
		echo "$TAG       ATTRIBUTE_ALIGN(32), which lets it land across a 4 KB line; patch 0005"
		echo "$TAG       raises it to 64 so it structurally cannot. An unpatched library that"
		echo "$TAG       happens to land on a safe address is still refused here, on purpose."
	fi
	echo "$TAG   WHAT TO DO: rebuild the sidecar with tools/build_libogc_sidecar.sh (patch 0005)"
	echo "$TAG   and relink. Do NOT move cw_layout_probe_pad to get past this gate -- shifting"
	echo "$TAG   the image until this one buffer happens to clear the line leaves the next link"
	echo "$TAG   free to land back on it; the alignment is the fix, the pad is not."
	echo "$TAG   Background: tmp/jobs/bootdeath-0917/EXPERIMENT-3.md, docs/wii_boot_death_gate.md."
} >&2
exit 1
