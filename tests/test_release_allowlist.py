#!/usr/bin/env python3
"""R18-E1: the REAL docs/release-allowlist.json exports from this repo's HEAD.

Runs tools/clean_export.py against a sparse, shared, clean clone of HEAD (the
exporter refuses a dirty tree, and a developer's checkout often is dirty), so
every "required" input and every pin must hold at the committed snapshot. Then
checks the maintainer's 2026-10-30 scope (Q1/Q2): the only docs/ files are
release-install.md, controls_sheet.md and docs/site/; no artwork/, no README
screenshots (docs/ingame*.png, screenshots/), no agent/board files.
Exit 77 (skip) where the allowlist is absent: it is private and never exported.
"""
import fnmatch, json, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOW = ROOT / "docs" / "release-allowlist.json"
TOOL = ROOT / "tools" / "clean_export.py"
PUBLIC_DOCS = ("docs/release-install.md", "docs/controls_sheet.md")
NEVER = ["artwork/*", "screenshots/*", "docs/ingame*", ".claude/*", ".agents/*", "AGENTS.md",
         "CLAUDE.md", "KANBAN.md", "PROJECT_MEMORY.md", "BASELINE.md", "FORK_NOTES.md",
         "config.json", ".mcp.json", "*.dol", "*.jar", "tmp/*", "docs/release-allowlist.json",
         "tests/fixtures/g9_*_s12345.ndjson.bin", "tests/fixtures/g9_*_provenance.json"]
FAILS = []

def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL:", what)

def git(*args, cwd=ROOT):
    return subprocess.run(["git", "-C", str(cwd), *args], capture_output=True, text=True)

def main():
    if not ALLOW.is_file() or git("rev-parse", "--git-dir").returncode:
        print("SKIP: no private allowlist or no git checkout (public tree)")
        return 77
    allow = json.loads(ALLOW.read_text(encoding="utf-8"))
    tracked = set(git("ls-tree", "-r", "--name-only", "--full-tree", "HEAD").stdout.splitlines())
    if "docs/release-allowlist.json" not in tracked:
        print("SKIP: allowlist not committed at HEAD yet")
        return 77
    for req in allow["required"]:
        check(req in tracked, "required input not tracked at HEAD: " + req)
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        clone, dest = td / "clone", td / "export"
        for cmd in (["clone", "-q", "--shared", "--no-checkout", str(ROOT), str(clone)],
                    ["-C", str(clone), "sparse-checkout", "set", "--no-cone", "/LICENSE"],
                    ["-C", str(clone), "checkout", "-q", "HEAD"]):
            r = subprocess.run(["git", *cmd], capture_output=True, text=True)
            check(r.returncode == 0, "clone step %s: %s" % (cmd[0] if cmd[0] != "-C" else cmd[2], r.stderr))
        # Export the committed allowlist bytes, not a possibly edited working copy.
        al = td / "allow.json"
        al.write_bytes(subprocess.run(["git", "-C", str(ROOT), "show", "HEAD:docs/release-allowlist.json"],
                                      capture_output=True).stdout)
        r = subprocess.run([sys.executable, str(TOOL), "export", "--source", str(clone), "--rev", "HEAD",
                            "--allowlist", str(al), "--dest", str(dest)], capture_output=True, text=True)
        check(r.returncode == 0, "export of HEAD with the real allowlist: " + r.stdout + r.stderr)
        if r.returncode:
            return 1
        r = subprocess.run([sys.executable, str(TOOL), "verify", "--tree", str(dest)],
                           capture_output=True, text=True)
        check(r.returncode == 0, "verify: " + r.stderr)
        files = {p["path"] for p in json.loads((dest / "EXPORT_MANIFEST.json").read_text())["files"]}
    for req in allow["required"]:
        check(req in files, "required input missing from export: " + req)
    for p in sorted(files):
        if p.startswith("docs/"):
            check(p in PUBLIC_DOCS or p.startswith("docs/site/"), "private doc exported: " + p)
        check(not any(fnmatch.fnmatchcase(p, g) for g in NEVER), "never-export path exported: " + p)
    for top in ("source/", "tests/", "tools/", "assets/", "packs/", "third_party/", "LICENSES/"):
        check(any(p.startswith(top) for p in files), "nothing exported under " + top)
    print("%d files exported from HEAD with the real allowlist" % len(files))
    if FAILS:
        print("test_release_allowlist: %d failure(s)" % len(FAILS))
        return 1
    print("PASS test_release_allowlist")
    return 0

if __name__ == "__main__":
    sys.exit(main())
