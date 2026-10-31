#!/usr/bin/env python3
"""Host-side regression for tools/check_sd0_dma_buffer.sh -- the bootdeath-0917 /
K12 post-link refusal gate on libogc's raw SD DMA buffer.

WHAT IS BEING GUARDED
Real-Wii experiments on 2026-09-17 (tmp/jobs/bootdeath-0917/EXPERIMENT-3.md,
predictions written before every run) isolated the pre-FAT zero-trace boot death
to ONE object: libogc's `__sd0_functions`, the 64-byte buffer handed raw to the
SD host controller as a DMA target during SD init. Using an experiment-only
libogc with a 32-byte shim placed directly after it, the two halves of a double
dissociation were run on hardware:

  4c  straddle WITHOUT the dying __bss_end slot  -> DIES  2/2
  4b  the dying __bss_end slot WITHOUT a straddle -> BOOTS 2/2

In 4b, __Arena1Lo, the staged argv block, the KERNEL_HEAP base, both stacks,
__conf_buffer and __sd0_cid all sat at the DYING build's exact addresses and the
image booted anyway. The `__bss_end` page-offset window this gate used to refuse
was a proxy; it has been removed, not kept alongside (it would have refused
roughly one innocent build in 128).

HOW IT TESTS
Every case drives the gate through a FAKE `nm -S` -- a short shell script this
test writes -- because a real .elf cannot be persuaded to place a library symbol
at a chosen address. That is what makes the boundaries, the size rule and every
fail-closed path testable on any host, with no devkitPPC and no Wii build.

The preserved real ELFs from the experiments are then run through the REAL
devkitPPC nm, as an end-to-end check that the fake-nm contract matches reality:
the four artifacts that killed the Wii must be refused and the four that booted
with an acceptable layout must be accepted. Those live outside the repo under
`repo-analysis/_persist/` and are never committed, so that leg SKIPS cleanly
when they or devkitPPC are absent (CI has neither).

Exit-code contract: 0 accept, 1 REFUSED (a layout a human must fix), 2 FAIL
CLOSED (tool/elf/symbol/size/parse). The two non-zero codes stay distinct so a
caller can tell "this image is dangerous" from "I could not tell".
"""

from __future__ import annotations

import argparse
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ACCEPT = 0
REFUSE = 1
FAIL_CLOSED = 2

# Preserved experiment ELFs, relative to a `_persist` root, with the verdict the
# real hardware runs justify and the __sd0_functions address nm reports.
# p256 is deliberately absent: it BOOTED, but it predates patch 0005, so its
# buffer is only 32-byte aligned and rule (c) refuses it on purpose. It is
# covered by its own case below so that intent is asserted, not assumed.
PRESERVED = [
    ("bootdeath-bf4aa1c_09.17", REFUSE, "806dffe0", "base: DIED 3/3, buffer straddles 0x806e0000"),
    ("bootdeath-exp-0917/n16", REFUSE, "806dffe0", "n16: DIED, same straddle, different binary"),
    ("bootdeath-exp-0917/nudge4096", REFUSE, "806e0fe0", "nudge4096: DIED, straddle one page up"),
    ("bootdeath-exp-0917/shim_m32", REFUSE, "806dffe0", "4c straddle WITHOUT the slot: DIED 2/2"),
    ("bootdeath-exp-0917/shim_m64", ACCEPT, "806dffc0", "4b the slot WITHOUT the straddle: BOOTS 2/2"),
    ("bootdeath-exp-0917/a64_p0", ACCEPT, "806e0000", "patch 0005 sanity build: PASS 68 hb"),
    ("bootdeath-exp-0917/m32", ACCEPT, "806dffc0", "m32: PASS 66/68 hb"),
    ("bootdeath-exp-0917/nudge32", ACCEPT, "806e0000", "nudge32: PASS 68 hb, twice"),
]

FAILURES: list[str] = []


def check(condition: bool, label: str, detail: str = "") -> None:
    if condition:
        print(f"  ok   {label}")
        return
    FAILURES.append(label)
    print(f"  FAIL {label}")
    if detail:
        for line in detail.rstrip("\n").splitlines():
            print(f"         {line}")


def write_fake_nm(path: Path, lines: list[str], exit_code: int = 0) -> Path:
    """A stand-in for `powerpc-eabi-nm -S` that prints fixed output.

    POSIX sh, not Python: the gate resolves its nm argument with `[ -x ]` (a
    path) or `command -v` (a bare name) and then execs it, exactly as the
    Makefile and build-verify.sh hand it the devkitPPC binary.
    """
    body = "#!/bin/sh\n"
    for line in lines:
        body += "printf '%s\\n' " + "'" + line.replace("'", "'\\''") + "'\n"
    body += f"exit {exit_code}\n"
    path.write_text(body, encoding="ascii", newline="\n")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


def nm_lines(sym_lines: list[str]) -> list[str]:
    """Realistic `nm -S` output: noise plus whatever the case under test needs.

    The noise is load-bearing. It carries a two-field undefined symbol, a
    three-field symbol with no size, and a symbol whose name merely CONTAINS
    `__sd0_functions`, so the gate is held to an exact four-field match on the
    name rather than a substring grep.
    """
    return [
        "80004000 00000100 T __init_wii",
        "         U memcpy",
        "806dff00 b __some_unsized_symbol",
        "806dff40 00000010 b not___sd0_functions_either",
        *sym_lines,
        "806e0020 00000010 b __sd0_cid",
        "80005000 00000200 T main",
    ]


def sym(addr: str, size: str = "00000040", typ: str = "b",
        name: str = "__sd0_functions") -> str:
    return f"{addr} {size} {typ} {name}"


def run_gate(bash: str, script: Path, elf: Path | str, nm: Path | str,
             timeout: int = 60) -> subprocess.CompletedProcess:
    return subprocess.run(
        [bash, str(script), str(elf), str(nm)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
    )


def case(bash: str, script: Path, tmp: Path, label: str, sym_lines: list[str],
         want: int, phrase: str, nm_exit: int = 0) -> str:
    """Drive one fake-nm scenario; assert the exit code AND a distinguishing phrase."""
    elf = tmp / "fake.elf"
    elf.write_bytes(b"\x7fELF not really, the fake nm never reads it")
    fake = write_fake_nm(tmp / "fake-nm.sh", nm_lines(sym_lines), nm_exit)
    proc = run_gate(bash, script, elf, fake)
    check(proc.returncode == want, f"{label}: exit {want}",
          f"got exit {proc.returncode}\n{proc.stdout}")
    check(phrase in proc.stdout, f"{label}: says {phrase!r}", proc.stdout)
    return proc.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, help="repo root holding tools/check_sd0_dma_buffer.sh")
    ap.add_argument("--bash", required=True, help="bash executable (git-bash/MSYS2 on Windows)")
    ap.add_argument("--persist", default=None,
                    help="root holding the preserved experiment ELFs; auto-detected when omitted")
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    bash = args.bash
    script = repo / "tools" / "check_sd0_dma_buffer.sh"
    if not script.is_file():
        print(f"FAIL: shared check not found: {script}")
        return 1

    tmpdir = Path(tempfile.mkdtemp(prefix="cavewii-sd0-dma-"))
    try:
        print("== accepted layouts: 64-byte aligned, wholly inside one 4 KB page ==")
        out = case(bash, script, tmpdir, "accept 0x806e0000 (page start)",
                   [sym("806e0000")], ACCEPT, "sd0-dma gate OK")
        check("__sd0_functions=0x806e0000" in out and "size=0x40" in out
              and "page_offset=0x000..0x03f" in out,
              "accept line carries address, size and page offset on ONE line", out)
        check(len([ln for ln in out.strip().splitlines() if ln.strip()]) == 1,
              "accept output is exactly one line (so every build log carries it)", out)
        case(bash, script, tmpdir, "accept 0x806dffc0 (ends exactly ON the line)",
             [sym("806dffc0")], ACCEPT, "page_offset=0xfc0..0xfff")
        case(bash, script, tmpdir, "accept 0x806e0040 (aligned, mid-page)",
             [sym("806e0040")], ACCEPT, "sd0-dma gate OK")

        print("== refused: crosses a 4 KB line (the proven death condition) ==")
        out = case(bash, script, tmpdir, "refuse 0x806dffe0 (the real dying address)",
                   [sym("806dffe0")], REFUSE, "CROSSES A 4 KB LINE")
        check("will die before FAT on real hardware" in out,
              "the dying address is told it will die before FAT", out)
        check("NOT 64-BYTE ALIGNED" in out,
              "the dying address ALSO reports the alignment failure (both rules, explicit)", out)
        check("page offset 0xfe0..0x101f" in out,
              "the refusal shows the span running past 0xfff", out)
        case(bash, script, tmpdir, "refuse 0x806e0ff0 (crosses, one page up)",
             [sym("806e0ff0")], REFUSE, "CROSSES A 4 KB LINE")

        print("== refused: 32-aligned but NOT 64-aligned, and does NOT cross ==")
        out = case(bash, script, tmpdir, "refuse 0x806e0020 (aligned 32, no crossing)",
                   [sym("806e0020")], REFUSE, "not the")
        check("sidecar carrying patch 0005" in out,
              "names the sidecar patch 0005 as the fix", out)
        check("CROSSES A 4 KB LINE" not in out,
              "a non-crossing address is NOT accused of crossing", out)
        check("Do NOT move cw_layout_probe_pad" in out,
              "tells the operator not to move the pad to get past the gate", out)
        check("build_libogc_sidecar.sh" in out,
              "names tools/build_libogc_sidecar.sh as the action", out)

        print("== fail closed: the gate must never SKIP when it cannot tell ==")
        case(bash, script, tmpdir, "symbol missing", [], FAIL_CLOSED, "is missing from")
        case(bash, script, tmpdir, "duplicate definitions",
             [sym("806e0000"), sym("806e1000")], FAIL_CLOSED, "ambiguous: 2 exact-name entries")
        case(bash, script, tmpdir, "suffixed LTO clone __sd0_functions.0",
             [sym("806e0000"), sym("806e1000", name="__sd0_functions.0")],
             FAIL_CLOSED, "suffixed clone")
        case(bash, script, tmpdir, "suffixed clone ALONE (no clean definition)",
             [sym("806e0000", name="__sd0_functions.part.1")], FAIL_CLOSED, "suffixed clone")
        case(bash, script, tmpdir, "size 0x20", [sym("806e0000", size="00000020")],
             FAIL_CLOSED, "libogc changed; re-derive the rule")
        case(bash, script, tmpdir, "size 0x80", [sym("806e0000", size="00000080")],
             FAIL_CLOSED, "libogc changed; re-derive the rule")
        case(bash, script, tmpdir, "mixed: clean definition + undefined (U) entry (Astra A1)",
             [sym("806e0000"), "         U __sd0_functions"], FAIL_CLOSED, "exact-name entries")
        case(bash, script, tmpdir, "mixed: clean definition + unsized three-field entry (Astra A1)",
             [sym("806e0000"), "806e1000 b __sd0_functions"], FAIL_CLOSED, "exact-name entries")
        case(bash, script, tmpdir, "mixed: clean definition + other-type (T) entry (Astra A1)",
             [sym("806e0000"), sym("80003100", typ="T")], FAIL_CLOSED, "exact-name entries")
        case(bash, script, tmpdir, "mixed: clean definition + W entry (Astra A1)",
             [sym("806e0000"), sym("806e2000", typ="W")], FAIL_CLOSED, "exact-name entries")
        case(bash, script, tmpdir, "undefined (U) line",
             ["         U __sd0_functions"], FAIL_CLOSED, "present but UNDEFINED")
        case(bash, script, tmpdir, "defined but UNSIZED (three fields)",
             ["806e0000 b __sd0_functions"], FAIL_CLOSED, "present but UNDEFINED")
        case(bash, script, tmpdir, "non-hex address", [sym("zzzzzzzz")],
             FAIL_CLOSED, "not hexadecimal")
        case(bash, script, tmpdir, "non-hex size", [sym("806e0000", size="00nothex")],
             FAIL_CLOSED, "not hexadecimal")
        case(bash, script, tmpdir, "address too short for a page offset", [sym("e0")],
             FAIL_CLOSED, "too short for a page offset")
        case(bash, script, tmpdir, "nm exits non-zero", [sym("806e0000")],
             FAIL_CLOSED, "exited non-zero", nm_exit=3)

        elf = tmpdir / "fake.elf"
        empty = write_fake_nm(tmpdir / "empty-nm.sh", [])
        proc = run_gate(bash, script, elf, empty)
        check(proc.returncode == FAIL_CLOSED, "nm prints nothing: exit 2",
              f"got exit {proc.returncode}\n{proc.stdout}")
        check("no output" in proc.stdout, "nm printing nothing says so", proc.stdout)

        garbage = write_fake_nm(tmpdir / "garbage-nm.sh",
                                ["this is not nm output at all", "<<< truncated >>>"])
        proc = run_gate(bash, script, elf, garbage)
        check(proc.returncode == FAIL_CLOSED, "garbage nm output: exit 2",
              f"got exit {proc.returncode}\n{proc.stdout}")

        good = write_fake_nm(tmpdir / "good-nm.sh", nm_lines([sym("806e0000")]))
        proc = run_gate(bash, script, elf, tmpdir / "no-such-nm.sh")
        check(proc.returncode == FAIL_CLOSED, "nm missing: exit 2",
              f"got exit {proc.returncode}\n{proc.stdout}")
        check("nm not found" in proc.stdout, "nm missing names the missing tool", proc.stdout)

        proc = run_gate(bash, script, tmpdir / "no-such.elf", good)
        check(proc.returncode == FAIL_CLOSED, "elf missing: exit 2",
              f"got exit {proc.returncode}\n{proc.stdout}")
        check("not found" in proc.stdout, "elf missing names the missing file", proc.stdout)

        proc = subprocess.run([bash, str(script)], text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=60)
        check(proc.returncode == FAIL_CLOSED, "no arguments at all: exit 2",
              f"got exit {proc.returncode}\n{proc.stdout}")

        print("== the preserved real ELFs, through the real devkitPPC nm ==")
        real_nm = find_real_nm()
        persist = find_persist(repo, args.persist)
        if real_nm is None or persist is None:
            reason = []
            if real_nm is None:
                reason.append("devkitPPC powerpc-eabi-nm not on this host")
            if persist is None:
                reason.append("preserved experiment ELFs not on this host")
            print(f"  SKIP  {'; '.join(reason)} -- the fake-nm cases above still prove the gate")
        else:
            print(f"  nm      = {real_nm}")
            print(f"  persist = {persist}")
            for rel, want, addr, why in PRESERVED:
                elf_path = persist / rel / "CaveWii_v2.elf"
                if not elf_path.is_file():
                    print(f"  SKIP  {rel}: {elf_path} absent")
                    continue
                proc = run_gate(bash, script, elf_path, real_nm)
                verdict = "REFUSE" if want == REFUSE else "accept"
                check(proc.returncode == want,
                      f"{rel} (__sd0_functions 0x{addr}) -> {verdict} [{why}]",
                      f"got exit {proc.returncode}\n{proc.stdout}")
                check(f"0x{addr}" in proc.stdout,
                      f"{rel}: the gate reports the real address 0x{addr}", proc.stdout)
            # p256 BOOTED on hardware yet must still be refused: it predates
            # patch 0005, so its buffer is 32-byte aligned and only escaped the
            # 4 KB line by luck of layout. Asserting this pins the deliberate
            # asymmetry between "would have died" and "is built the right way".
            p256 = persist / "bootdeath-exp-0917" / "p256" / "CaveWii_v2.elf"
            if p256.is_file():
                proc = run_gate(bash, script, p256, real_nm)
                check(proc.returncode == REFUSE,
                      "p256 -> REFUSE although it BOOTED (unpatched libogc, safe by luck)",
                      f"got exit {proc.returncode}\n{proc.stdout}")
                check("CROSSES A 4 KB LINE" not in proc.stdout
                      and "sidecar carrying patch 0005" in proc.stdout,
                      "p256 is refused for provenance (c), not for crossing (d)", proc.stdout)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    if FAILURES:
        print(f"\nFAILED ({len(FAILURES)}): " + "; ".join(FAILURES))
        return 1
    print("\nPASS: tools/check_sd0_dma_buffer.sh accepts only a 64-byte-aligned "
          "__sd0_functions of size 0x40 that stays inside one 4 KB page, and fails "
          "closed on every unverifiable input.")
    return 0


def find_real_nm() -> Path | None:
    """The devkitPPC nm the Wii build itself uses, if this host has one."""
    candidates: list[Path] = []
    dkp = os.environ.get("DEVKITPPC")
    if dkp:
        candidates.append(Path(dkp) / "bin" / "powerpc-eabi-nm")
    candidates.append(Path("C:/msys64/opt/devkitpro/devkitPPC/bin/powerpc-eabi-nm"))
    candidates.append(Path("/opt/devkitpro/devkitPPC/bin/powerpc-eabi-nm"))
    for cand in candidates:
        for probe in (cand, cand.with_suffix(".exe")):
            if probe.is_file():
                return probe
    found = shutil.which("powerpc-eabi-nm")
    return Path(found) if found else None


def find_persist(repo: Path, override: str | None) -> Path | None:
    """Locate the out-of-repo `_persist` tree holding the experiment artifacts.

    They are deliberately not committed (multi-MB Wii binaries), and a worktree
    sits several levels below the checkout that `_persist` is a sibling of, so
    walk up rather than hard-coding one host's absolute path.
    """
    if override:
        cand = Path(override)
        return cand if (cand / PRESERVED[0][0]).is_dir() else None
    env = os.environ.get("CW_PERSIST_DIR")
    if env:
        cand = Path(env)
        if (cand / PRESERVED[0][0]).is_dir():
            return cand
    for parent in [repo, *repo.parents]:
        cand = parent / "_persist"
        if (cand / PRESERVED[0][0]).is_dir():
            return cand
    return None


if __name__ == "__main__":
    sys.exit(main())
