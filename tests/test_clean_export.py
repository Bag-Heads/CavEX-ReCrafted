"""REL-18e: tools/clean_export.py on a synthetic hermetic git repo: exact file set,
deterministic manifest, bait excluded even when allowed, refusals, verify."""
import hashlib, json, os, subprocess, sys, tempfile
from pathlib import Path

H = lambda b: hashlib.sha256(b).hexdigest()

TOOL = Path(__file__).resolve().parents[1] / "tools" / "clean_export.py"
KEEP = {"CMakeLists.txt": b"project(x)\n", "LICENSE": b"MIT\n", "NOTICE": b"n\n",
        "LICENSES/zlib.txt": b"zlib\n", "source/main.c": b"int main(void){return 0;}\n",
        "source/keep.c": b"/* export-ignore bait */\n", "tests/test_a.c": b"int t;\n",
        "tests/golden/a.bin": b"\x00\x01\xff", "assets/terrain.png": b"\x89PNG-cc0",
        "packs/isabella-ii/LICENSE.txt": b"CC BY 3.0\r\n", "assets/mob/pig.png": b"cc0 pig",
        "docs/build_architecture.md": b"keep\n", "tools/run.py": b"keep\n"}
# Real tracked inputs of registered tests (Fable): presets, CaveWii-written worlds, fixture logs.
KEEP.update({p: b"test input " + p.encode() for p in (
    "config_wii.json", "config_pc.json", "docs/presets/config_wii_dev.json",
    "docs/presets/config_wii_developer.json",
    "docs/presets/config_wii_pointer_look.json", "tests/fixtures/sort2_native/edited/level.dat",
    "tests/fixtures/sort2_native/world/region/r.0.0.mcr", "tests/fixtures/mp_play/good/mc173-diag.log")})
KEEP["assets/earth-and-ash-manifest.json"] = json.dumps(
    [{"path": "mob/pig.png", "sha256": H(b"cc0 pig")}, {"path": "mob/cow.png", "sha256": "0" * 64}]).encode()
BAIT = ["b1.7.3.jar", "beta_pack/terrain.png", "assets/mob/rip.png", "assets/mob/cow.png",
        "tests/fixtures/beta_import/world/region/r.0.0.mcr", "tests/fixtures/beta_import/world/level.dat",
        "tests/fixtures/beta_import/world/level.dat_old", "tests/fixtures/beta_import/world/x.txt",
        "tests/fixtures/beta_import/r.9.9.mcr", "tools/config_capture.json", "tools/run.log",
        "artwork/cc0-pack/pc-captures/a.png", "docs/ui/captures/b.png", "tools/console_capture/c.py",
        "tests/oracle/dump.c",
        "tests/test_spawn_oracle.c", "tmp/jobs/x.log", "build-verify/a.exe", "config.json",
        ".minecraft/bin/minecraft.jar", "secrets.env", ".claude/hooks/h.py", "worktrees/w/a.c",
        "docs/decompile/World.java", "forks/lberwa/a.c", "CaveWii_v2.dol", "screenshots/s.png",
        "source/server.key", "artwork/cc0-pack/native-proof-2026-09-11/01-title.png",  # Sol: real
        "artwork/cc0-pack/native-proof-2026-09-11/earth-and-ash-native-proof.zip",  # captures
        "docs/ui_design/front-end-refined/proofs/capture-check.png", "packs/x.zip",
        "artwork/controller-glyphs/controls-proof.png", "docs/ui_design/approved/native-frontend-proof.json",
        "docs/ui_design/approved/evidence/revision-check.json"]
FAILS = []

def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL:", what)

def run(*args, cwd=None):
    return subprocess.run(list(args), cwd=cwd, capture_output=True, text=True)

def tool(*args):
    return run(sys.executable, str(TOOL), *args)

def snapshot(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*")) if p.is_file() and ".git" not in p.parts}

def main():
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        src = td / "src"
        files = dict(KEEP, **{b: b"bait " + b.encode() for b in BAIT})
        files[".gitattributes"] = b"source/keep.c export-ignore\n* -text\n"
        files[".gitignore"] = b"*.jar\ntmp/\n"
        for rel, data in files.items():
            (src / rel).parent.mkdir(parents=True, exist_ok=True)
            (src / rel).write_bytes(data)
        for cmd in (["git", "init", "-q"], ["git", "add", "-f", "-A"],
                    ["git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "s"]):
            check(run(*cmd, cwd=src).returncode == 0, "fixture " + cmd[1])
        rev = run("git", "rev-parse", "HEAD", cwd=src).stdout.strip()
        allow = {"include": ["*"], "required": ["CMakeLists.txt", "LICENSE", "LICENSES/zlib.txt",
                                                "packs/isabella-ii/LICENSE.txt", "source/main.c"],
                 "pins": {"LICENSES/zlib.txt": hashlib.sha256(b"zlib\n").hexdigest()}}
        al = td / "allow.json"
        al.write_text(json.dumps(allow))
        before = snapshot(src)

        def export(dest, allow_path=al):
            return tool("export", "--source", str(src), "--rev", rev,
                        "--allowlist", str(allow_path), "--dest", str(dest))

        r = export(td / "out1")
        check(r.returncode == 0, "export ok: " + r.stderr)
        out = td / "out1"
        got = snapshot(out) if out.is_dir() else {}
        want = set(KEEP) | {".gitattributes", ".gitignore", "EXPORT_MANIFEST.json"}
        check(set(got) == {p.replace("/", os.sep) for p in want},
              "exact file set; extra=%s" % sorted(set(got) - {p.replace("/", os.sep) for p in want}))
        for b in BAIT:
            check(not (out / b).exists(), "bait excluded: " + b)
        for rel, data in KEEP.items():
            check((out / rel).is_file() and (out / rel).read_bytes() == data, "byte-exact: " + rel)
        man = json.loads((out / "EXPORT_MANIFEST.json").read_text()) if out.is_dir() else {}
        check(man.get("source_commit") == rev, "manifest binds commit")
        check(man.get("status") == "candidate-not-final-not-legally-cleared", "no clearance claim")
        check({f["path"]: f["sha256"] for f in man.get("files", [])}.get("source/main.c")
              == H(KEEP["source/main.c"]), "manifest sha256")
        check(r.returncode == 0 and tool("verify", "--tree", str(out)).returncode == 0, "verify ok")
        r2 = export(td / "out2")
        check(r2.returncode == 0 and (td / "out2/EXPORT_MANIFEST.json").read_bytes()
              == (out / "EXPORT_MANIFEST.json").read_bytes(), "deterministic manifest")
        check(snapshot(src) == before and run("git", "status", "--porcelain", cwd=src).stdout == "",
              "source tree untouched")

        (td / "taken").mkdir()
        (td / "taken/keep.txt").write_text("x")
        check(export(td / "taken").returncode != 0 and snapshot(td / "taken") == {"keep.txt":
              H(b"x")}, "existing destination refused, untouched")
        check(export(src / "exp").returncode != 0 and not (src / "exp").exists(), "in-repo dest refused")
        for name, bad in (("missing", dict(allow, required=["NOPE.md"])),
                          ("drift", dict(allow, pins={"LICENSES/zlib.txt": "0" * 64})),
                          ("required-ip", dict(allow, required=["assets/mob/rip.png"]))):
            (td / (name + ".json")).write_text(json.dumps(bad))
            check(export(td / name, td / (name + ".json")).returncode != 0
                  and not (td / name).exists(), "refused: " + name)
        (src / "source/main.c").write_bytes(b"dirty\n")
        check(export(td / "dirty").returncode != 0 and not (td / "dirty").exists(), "dirty tree refused")
        (src / "source/main.c").write_bytes(KEEP["source/main.c"])

        if out.is_dir():
            (out / "extra.txt").write_text("x")
            check(tool("verify", "--tree", str(out)).returncode != 0, "verify catches extra file")
            (out / "extra.txt").unlink()
            (out / "LICENSE").write_bytes(b"GPL\n")
            check(tool("verify", "--tree", str(out)).returncode != 0, "verify catches tamper")
    print("test_clean_export: %s" % ("FAIL %d" % len(FAILS) if FAILS else "PASS"))
    return 1 if FAILS else 0

if __name__ == "__main__":
    sys.exit(main())
