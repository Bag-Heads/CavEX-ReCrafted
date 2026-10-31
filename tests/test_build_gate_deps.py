#!/usr/bin/env python3
"""Host-side regression for tools/check_build_deps.sh -- the hw1029b post-build
refusal gate on a foreign or stale Wii object directory.

WHAT IS BEING GUARDED
2026-10-29: the hw1029b release .dol was built in runner clone C:/tmp/cw-hw1029b
whose build/ had been copied from C:/tmp/cw-hw1029a. Its gcc .d files named the
OTHER clone's headers, so a client_session.h struct change rebuilt only main.o;
the link mixed struct offsets and the Wii took a DSI on the first frame. Every
existing gate (md5, build ID, TLS, test marker, sd0 DMA) passed.

HOW IT TESTS
Each case writes a fake build dir of gcc -MMD style .d files plus .o files with
chosen mtimes, then runs the real gate script through bash. It also checks that
build-verify.sh wipes build/ for release builds and wires the gate in.

Exit-code contract: 0 accept, 1 REFUSED, 2 FAIL CLOSED.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ACCEPT = 0
REFUSE = 1
FAIL_CLOSED = 2

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


def posix(p: Path) -> str:
    return p.as_posix()


class Tree:
    """A fake source tree + build dir with controllable mtimes."""

    def __init__(self, base: Path, name: str):
        self.root = base / name
        self.src = self.root / "source"
        self.build = self.root / "build"
        self.src.mkdir(parents=True)
        self.build.mkdir()
        self.t0 = 1_700_000_000

    def header(self, rel: str, age: int = 0) -> Path:
        p = self.src / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("/* h */\n", encoding="ascii")
        os.utime(p, (self.t0 + age, self.t0 + age))
        return p

    def obj(self, name: str, deps: list[str], age: int = 100) -> Path:
        """Write build/<name>.o (mtime t0+age) and a gcc -MMD .d listing deps."""
        o = self.build / f"{name}.o"
        o.write_bytes(b"\0")
        os.utime(o, (self.t0 + age, self.t0 + age))
        lines = [f"{name}.o: \\"]
        for i, d in enumerate(deps):
            lines.append(f" {d}" + (" \\" if i < len(deps) - 1 else ""))
        for d in deps[1:]:
            lines.append(f"{d}:")
        (self.build / f"{name}.d").write_text("\n".join(lines) + "\n",
                                               encoding="ascii", newline="\n")
        return o


def run_gate(bash: str, script: Path, build: Path | str, root: Path | str,
             *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [bash, str(script), str(build), str(root), *extra],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120,
    )


def case(bash: str, script: Path, tree: Tree, label: str, want: int, phrase: str,
         *extra: str, root: Path | None = None) -> str:
    proc = run_gate(bash, script, posix(tree.build), posix(root or tree.root), *extra)
    check(proc.returncode == want, f"{label}: exit {want}",
          f"got exit {proc.returncode}\n{proc.stdout}")
    check(phrase in proc.stdout, f"{label}: says {phrase!r}", proc.stdout)
    return proc.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, help="repo root holding tools/check_build_deps.sh")
    ap.add_argument("--bash", required=True, help="bash executable (git-bash/MSYS2 on Windows)")
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    bash = args.bash
    script = repo / "tools" / "check_build_deps.sh"
    if not script.is_file():
        print(f"FAIL: gate not found: {script}")
        return 1

    tmp = Path(tempfile.mkdtemp(prefix="cavewii-build-deps-"))
    try:
        print("== accept: clean build of this tree ==")
        t = Tree(tmp, "cw-hw1029b")
        h = t.header("client/client_session.h", age=0)
        c = t.header("main.c", age=0)
        t.obj("main", [posix(c), posix(h)], age=100)
        t.obj("net", [posix(t.header("net.c")), posix(h), "data_png.h"], age=100)
        (t.build / "data_png.h").write_text("/* gen */\n", encoding="ascii")
        os.utime(t.build / "data_png.h", (t.t0 + 50, t.t0 + 50))
        out = case(bash, script, t, "clean tree", ACCEPT, "[build-deps] OK")
        check("2 .d files" in out, "OK line counts the .d files", out)

        print("== accept: toolchain header under an allowed root ==")
        tc = tmp / "devkitpro" / "libogc" / "include"
        tc.mkdir(parents=True)
        (tc / "gccore.h").write_text("/* sys */\n", encoding="ascii")
        os.utime(tc / "gccore.h", (t.t0, t.t0))
        t.obj("gfx", [posix(t.header("gfx.c")), posix(tc / "gccore.h")], age=100)
        case(bash, script, t, "devkitpro header allowed", ACCEPT, "[build-deps] OK",
             posix(tmp / "devkitpro"))
        case(bash, script, t, "devkitpro header NOT allowed without the root",
             REFUSE, "foreign dependency paths: 1")

        print("== refuse: .d copied from another clone (the hw1029b case) ==")
        a = Tree(tmp, "cw-hw1029a")
        a_h = a.header("client/client_session.h")
        b = Tree(tmp, "cw-hw1029b2")
        b.header("client/client_session.h", age=500)  # edited here, newer than objects
        b.obj("main", [posix(b.header("main.c")), posix(b.src / "client/client_session.h")],
              age=600)  # main.o rebuilt by the stamp touch
        b.obj("world", [posix(a.src / "world.c"), posix(a_h)], age=100)  # copied over
        out = case(bash, script, b, "foreign-root .d", REFUSE, "REFUSED")
        check("foreign dependency paths: 2" in out, "counts both foreign paths", out)
        check("cw-hw1029a" in out, "names the foreign clone path", out)
        check("NEVER copy build/ between clones" in out, "tells the runner the rule", out)
        check("rm -rf" in out, "gives the fix", out)

        print("== refuse: .o older than a header it depends on ==")
        s = Tree(tmp, "stale")
        sh = s.header("client/client_session.h", age=500)
        s.obj("main", [posix(s.header("main.c")), posix(sh)], age=600)
        s.obj("world", [posix(s.header("world.c")), posix(sh)], age=100)
        out = case(bash, script, s, "stale .o", REFUSE, "objects older than a dependency: 1")
        check("world.o is older than" in out, "names the stale object", out)
        check("foreign dependency paths: 0" in out, "stale case is not called foreign", out)

        print("== path folding: Windows/MSYS/case spellings of the same root agree ==")
        w = Tree(tmp, "Fold")
        wh = w.header("x.h")
        w.obj("x", [posix(w.header("x.c")), posix(wh)], age=100)
        root_variant = posix(w.root).upper().replace("/", "\\")
        if len(root_variant) >= 2 and root_variant[1] == ":":
            # C:\...  -> also accept the MSYS /c/... spelling of the same root
            msys = "/" + root_variant[0].lower() + root_variant[2:].replace("\\", "/")
            case(bash, script, w, "MSYS-spelled root", ACCEPT, "[build-deps] OK",
                 root=Path(msys))
        case(bash, script, w, "upper-case root", ACCEPT, "[build-deps] OK",
             root=Path(posix(w.root).upper()) if os.name == "nt" else w.root)

        print("== fail closed ==")
        e = Tree(tmp, "empty")
        case(bash, script, e, "no .d files", FAIL_CLOSED, "no .d dependency files")
        proc = run_gate(bash, script, posix(tmp / "nope"), posix(e.root))
        check(proc.returncode == FAIL_CLOSED, "missing build dir: exit 2", proc.stdout)
        (e.build / "junk.d").write_text("not a rule\n", encoding="ascii")
        case(bash, script, e, "unparsable .d", FAIL_CLOSED, "unparsable dependency file")

        print("== build-verify.sh wiring (both skill copies) ==")
        for rel in (".agents/skills/build-wii/scripts/build-verify.sh",
                    ".claude/skills/build-wii/scripts/build-verify.sh"):
            p = repo / rel
            if not p.is_file():
                continue
            text = p.read_text(encoding="utf-8")
            check('rm -rf "$OBJ_DIR"' in text, f"{rel}: release wipes build/ before make")
            check(text.index('rm -rf "$OBJ_DIR"') < text.index('"$MAKE" DEVKITPPC='),
                  f"{rel}: the wipe runs before make")
            check("tools/check_build_deps.sh" in text and "-REFUSED-builddeps.dol" in text
                  and "exit 10" in text, f"{rel}: gate wired, renames, exits 10")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if FAILURES:
        print(f"\nFAILED ({len(FAILURES)}): " + "; ".join(FAILURES))
        return 1
    print("\nPASS: tools/check_build_deps.sh refuses foreign-root .d files and stale "
          "objects, accepts a clean tree, and fails closed on unverifiable input.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
