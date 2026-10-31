#!/usr/bin/env python3
"""REL-18e: allowlisted clean export of ONE committed snapshot, without history.

  clean_export.py export --source REPO --rev COMMIT --allowlist JSON --dest NEWDIR
  clean_export.py verify --tree DIR [--manifest FILE]

Reads raw blobs of the commit (never the working tree or ignore rules). Allowlist
JSON: "include"/"exclude" globs, "required" paths, "pins" {path: sha256}. The
DENY_* lists always win. EXPORT_MANIFEST.json is a candidate inventory only,
never a final or legally cleared release. Nothing is pushed.
"""
import argparse, fnmatch, hashlib, json, os, subprocess, sys
from pathlib import Path

MANIFEST = "EXPORT_MANIFEST.json"
STATUS = "candidate-not-final-not-legally-cleared"
MOB_MANIFEST = "assets/earth-and-ash-manifest.json"  # lists our CC0 assets/mob/ art
DENY_PATH = ["*oracle*", "*decompile*", "*receipt*", "*secret*", "*credential*", "*id_rsa*",
             "*beta_import/*.mcr", "*beta_import/*level.dat*",  # Beta-server-written world data
             "*beta_import/world/*",  # whole path
             # Design studies modelled on real Beta textures (maintainer 2026-10-27):
             # kept locally, never in the public export.
             "artwork/cc0-pack/isometric-catalog/*", "artwork/cc0-pack/switch-audit/*",
             "artwork/cavewii-icon-concepts/*",
             "docs/ui_design/*.png", "docs/ui_design/*.jpg", "docs/ui_design/*.jpeg",
             "docs/ui_design/*.gif", "docs/ui_design/*.webp",
             # Dev-rig Stage Server app: never part of the public export or release.
             "tools/wii_stage_server/*", "*/apps/wii_stage_server/*", "apps/wii_stage_server/*"]
DENY_DIR = ["beta_pack", ".minecraft", "forks", "worktrees", ".claude", ".agents", "tmp",
            "build*", "*capture*", "screenshots", "*proof*", "*evidence*"]  # any directory component
DENY_NAME = ["*.jar", "*pass.json", "*.dol", "*.elf", "*.exe", "*.o", "*.env", "*.pem",
             "*.key", "*.p12", "*.zip", "*proof*.png", "*proof*.json", "*capture*.png",
             "*screenshot*.png"]  # basename
# basename globs denied everywhere EXCEPT these path prefixes (shipped defaults, test inputs)
DENY_NAME_EXCEPT = [("config*.json", ("tools/presets/", "docs/presets/", "config_wii.json", "config_pc.json")),
                    ("*.log", ("tests/fixtures/",))]

def die(msg):
    sys.exit("clean_export: REFUSED: " + msg)

def git(repo, *args, data=None):
    r = subprocess.run(["git", "-C", str(repo), *args], input=data, capture_output=True)
    if r.returncode:
        die("git %s failed: %s" % (args[0], r.stderr.decode(errors="replace").strip()))
    return r.stdout

def matches(path, globs):
    return any(fnmatch.fnmatchcase(path.lower(), g.lower()) for g in globs)

def denied(path):
    parts = path.split("/")
    return (matches(path, DENY_PATH) or matches(parts[-1], DENY_NAME)
            or any(matches(d, DENY_DIR) for d in parts[:-1])
            or any(matches(parts[-1], [g]) and not path.lower().startswith(ok)
                   for g, ok in DENY_NAME_EXCEPT))

def read_blobs(repo, shas):
    out, pos, blobs = git(repo, "cat-file", "--batch", data="".join(s + "\n" for s in shas).encode()), 0, []
    for sha in shas:
        eol = out.index(b"\n", pos)
        head = out[pos:eol].split()
        if head[0].decode() != sha or head[1] != b"blob":
            die("cat-file mismatch for " + sha)
        size = int(head[2])
        blobs.append(out[eol + 1:eol + 1 + size])
        pos = eol + 2 + size
    return blobs

def sha256(data):
    return hashlib.sha256(data).hexdigest()

def export(a):
    src, dest = Path(a.source).resolve(), Path(a.dest).resolve()
    allow_bytes = Path(a.allowlist).read_bytes()
    allow = json.loads(allow_bytes)
    if dest.exists():
        die("destination exists: %s" % dest)
    if dest == src or src in dest.parents:
        die("destination is inside the source repo")
    if git(src, "status", "--porcelain", "--untracked-files=no").strip():
        die("tracked working tree is dirty")
    commit = git(src, "rev-parse", "--verify", a.rev + "^{commit}").decode().strip()
    tree = git(src, "rev-parse", commit + "^{tree}").decode().strip()
    picked = []
    for entry in git(src, "ls-tree", "-r", "-z", "--full-tree", commit).split(b"\0"):
        if not entry:
            continue
        meta, path = entry.decode().split("\t", 1)
        mode, kind, sha = meta.split()
        if (kind == "blob" and mode != "120000" and matches(path, allow["include"])
                and not matches(path, allow.get("exclude", [])) and not denied(path)):
            picked.append((path, sha))
    files = dict(zip([p for p, _ in picked], read_blobs(src, [s for _, s in picked])))
    mob_ok = {"assets/" + m["path"]: m["sha256"] for m in json.loads(files.get(MOB_MANIFEST, b"[]"))}
    files = {p: d for p, d in files.items()  # assets/mob/ only if listed with matching hash
             if not p.lower().startswith("assets/mob/") or mob_ok.get(p) == sha256(d)}
    for req in allow.get("required", []):
        if req not in files:
            die("required input not exported: " + req)
    for path, want in sorted(allow.get("pins", {}).items()):
        if path not in files or sha256(files[path]) != want:
            die("pinned input drifted or missing: " + path)
    if MANIFEST in files:
        die(MANIFEST + " is reserved")
    manifest = {"schema": 1, "status": STATUS, "source_commit": commit, "source_tree": tree,
                "allowlist_sha256": sha256(allow_bytes),
                "files": [{"path": p, "size": len(files[p]), "sha256": sha256(files[p])}
                          for p in sorted(files)]}
    stage = dest.with_name(dest.name + ".partial")
    if stage.exists():
        die("stale staging dir exists: %s" % stage)
    for path, data in files.items():
        (stage / path).parent.mkdir(parents=True, exist_ok=True)
        (stage / path).write_bytes(data)
    (stage / MANIFEST).write_bytes(json.dumps(manifest, indent=1, sort_keys=True).encode() + b"\n")
    os.rename(stage, dest)
    print("clean_export: exported %d files from %s to %s" % (len(files), commit, dest))

def verify(a):
    tree = Path(a.tree)
    manifest = json.loads(Path(a.manifest or tree / MANIFEST).read_bytes())
    want = {f["path"]: f["sha256"] for f in manifest["files"]}
    have = {p.relative_to(tree).as_posix() for p in tree.rglob("*") if p.is_file()} - {MANIFEST}
    bad = sorted(have ^ set(want)) + sorted(p for p in want if p in have
                                            and sha256((tree / p).read_bytes()) != want[p])
    if bad:
        die("tree does not match manifest: " + ", ".join(bad[:20]))
    print("clean_export: verified %d files (commit %s)" % (len(want), manifest["source_commit"]))

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export")
    for opt in ("--source", "--rev", "--allowlist", "--dest"):
        e.add_argument(opt, required=True)
    v = sub.add_parser("verify")
    v.add_argument("--tree", required=True)
    v.add_argument("--manifest")
    a = ap.parse_args()
    export(a) if a.cmd == "export" else verify(a)

if __name__ == "__main__":
    main()
