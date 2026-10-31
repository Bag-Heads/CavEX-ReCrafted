"""REL-18e2: tools/release_scan.py on hermetic synthetic export trees + zips: planted
quotes/credentials/bait/missing records/hash drift found; benign citations, constants
and synthetic audio accepted; findings never echo credential values."""
import hashlib, io, json, math, struct, subprocess, sys, tempfile, wave, zipfile
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / "tools" / "release_scan.py"
H = lambda b: hashlib.sha256(b).hexdigest()
FAILS = []

def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL:", what)

def tone():  # valid synthetic 8 kHz mono sine WAV
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1), w.setsampwidth(2), w.setframerate(8000)
        w.writeframes(b"".join(struct.pack("<h", int(9000 * math.sin(i / 7))) for i in range(4000)))
    return buf.getvalue()

WAV, KEY, PNG = tone(), "sk-or-v1-" + "Q7f3Zp9LmX2vB8nK4tR6wY1c", b"\x89PNG\r\n\x1a\n\x00"
TREE = {"LICENSE": b"MIT\n", "LICENSES/greatest.txt": b"ISC\n", "third_party/greatest/greatest.h": b"#pragma once\n",
        "assets/sound/click.wav": WAV, "source/main.c": b"/* Beta order: entity before AIR (BlockTNT.java:41-53).\n"
        b" * Dwell is 81 adds of 0.0125F (Entity.java:812), see net.minecraft.src notes. */\n"
        b"#define MAX_TOKEN_LEN 64\nstatic const float DWELL = 0.0125F; // World.java:169 clock seed\n"
        b"void f(struct cfg *c) { c->password = getenv(\"CW_PW\"); c->api_key = c->password_len; }\n",
        "docs/notes.md": b"Cite `BlockTNT.java:41-53`.\n\n```c\nint a = 1;\nif (a) b();\nreturn;\n```\n"}
PKG = {"apps/CavEX/boot.dol": b"\x00DOL\x01" * 40, "apps/CavEX/assets/sound/click.wav": WAV,
       "LICENSE": b"MIT\n", "README-RELEASE.txt": b"Unzip onto the SD card root.\n"}
D0 = {"name": "greatest", "version": "1.5.0", "origin": "https://github.com/silentbicycle/greatest", "licence": "ISC",
      "licence_file": "LICENSES/greatest.txt", "files": [{"path": "third_party/greatest/greatest.h",
                                                         "sha256": H(b"#pragma once\n")}]}
A0 = {"sha256": H(WAV), "paths": ["assets/sound/click.wav", "apps/CavEX/assets/sound/click.wav"],
      "licence": "CC0-1.0", "licence_url": "https://creativecommons.org/publicdomain/zero/1.0/",
      "author": "CaveWii", "source": "synthesized (test)"}
JAVA = b"/* ported:\n * public void updateTick(World var1, int var2) {\n *   int var5 = var1.getBlockId(var2);\n" \
       b" *   this.dropBlock(var1, var5);\n * }\n */\nint x;\n"
CASES = [  # name, scan() overrides, one finding that must be reported
    ("quote", dict(tree={"source/port.c": JAVA}), ("JAVA_QUOTE_BLOCK", "tree", "source/port.c", 2)),
    ("fence", dict(tree={"docs/b.md": b"x\n```java\nboolean var3 = f();\nthis.drop(w);\nnew Item(3);\n```\n"}),
     ("JAVA_QUOTE_BLOCK", "tree", "docs/b.md", 3)),
    ("pkgquote", dict(pkg={"x/n.txt": b"# this.drop(a)\n# int var5 = 1;\n# var6\n"}),
     ("JAVA_QUOTE_BLOCK", "package", "x/n.txt", 1)),
    ("cred", dict(tree={"tools/k.py": b"import os\nAPI = '%s'\n" % KEY.encode()}), ("SECRET_PREFIX", "tree", "tools/k.py", 2)),
    ("assign", dict(pkg={"config.json": b'{\n "password": "%s"\n}\n' % KEY[9:].encode()}),
     ("SECRET_ASSIGNMENT", "package", "config.json", 2)),
    ("pem", dict(pkg={"LICENSE": b"-----BEGIN RSA PRIVATE KEY-----\n"}), ("PRIVATE_KEY", "package", "LICENSE", 1)),
    ("bait", dict(pkg={"tools/config_capture.json": b"{}"}), ("DENIED_PATH", "package", "tools/config_capture.json", 0)),
    ("jar", dict(pkg={"b1.7.3.jar": b"PK"}), ("DENIED_PATH", "package", "b1.7.3.jar", 0)),
    ("treebait", dict(tree={"secrets.env": b"x=1\n"}), ("DENIED_PATH", "tree", "secrets.env", 0)),
    ("noasset", dict(pkg={"a/icon.png": PNG}), ("ASSET_RECORD_MISSING", "package", "a/icon.png", 0)),
    ("nodep", dict(tree={"third_party/z/z.h": b"int z;\n"}), ("DEP_RECORD_MISSING", "tree", "third_party/z/z.h", 0)),
    ("deplic", dict(deps=[dict(D0, licence="unknown")]), ("DEP_RECORD_INCOMPLETE", "inventory", "dependency:greatest", 0)),
    ("assetlic", dict(assets=[dict(A0, licence_url="")]), ("ASSET_RECORD_INCOMPLETE", "inventory", ",".join(A0["paths"]), 0)),
    ("tamper", dict(tamper={"LICENSE": b"GPL\n"}), ("TREE_MANIFEST_MISMATCH", "tree", "LICENSE", 0)),
    ("assethash", dict(assets=[dict(A0, sha256="0" * 64)]), ("ASSET_HASH_MISMATCH", "tree", "assets/sound/click.wav", 0)),
    ("dephash", dict(tamper={"third_party/greatest/greatest.h": b"x\n"}),
     ("DEP_HASH_MISMATCH", "tree", "third_party/greatest/greatest.h", 0)),
    ("nosums", dict(sums=False), ("PKG_SUMS_MISMATCH", "package", "LICENSE", 0))]

def scan(td, name, tree=None, pkg=None, deps=(D0,), assets=(A0,), tamper=None, sums=True):
    root, tree, pkg = td / name, dict(TREE, **(tree or {})), dict(PKG, **(pkg or {}))
    for rel, data in tree.items():
        (root / "t" / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / "t" / rel).write_bytes(data)
    (root / "t/EXPORT_MANIFEST.json").write_text(json.dumps({"source_commit": "c" * 40, "files": [
        {"path": p, "sha256": H(d)} for p, d in sorted(tree.items())]}))
    for rel, data in (tamper or {}).items():
        (root / "t" / rel).write_bytes(data)
    with zipfile.ZipFile(root / "p.zip", "w") as z:
        for rel, data in pkg.items():
            z.writestr(rel, data)
        z.writestr("SHA256SUMS", "".join("%s  %s\n" % (H(d), p) for p, d in sorted(pkg.items()) if sums))
    (root / "d.json").write_text(json.dumps({"dependencies": list(deps)}))
    (root / "a.json").write_text(json.dumps({"assets": list(assets)}))
    r = subprocess.run([sys.executable, str(TOOL), "--tree", str(root / "t"), "--package", str(root / "p.zip"),
                        "--dependencies", str(root / "d.json"), "--assets", str(root / "a.json"),
                        "--out", str(root / "r.json")], capture_output=True, text=True)
    text = (root / "r.json").read_text() if (root / "r.json").is_file() else "{}"
    check(KEY[9:] not in text + r.stdout + r.stderr, name + ": credential value never reported")
    return r.returncode, {(f["type"], f["where"], f["file"], f["line"]) for f in json.loads(text).get("findings", [])}, text

def main():
    with tempfile.TemporaryDirectory() as td:
        rc, got, text = scan(Path(td), "clean")
        check(rc == 0 and got == set(), "clean export accepted: %s" % sorted(got))
        check('"status": "scan-result-not-legal-clearance"' in text, "report never claims legal clearance")
        for name, kw, want in CASES:
            rc, got, _ = scan(Path(td), name, **kw)
            check(rc == 1 and want in got, "%s detected %s; got %s" % (name, want, sorted(got)))
    print("test_release_scan: %s" % ("FAIL %d" % len(FAILS) if FAILS else "PASS"))
    return 1 if FAILS else 0

if __name__ == "__main__":
    sys.exit(main())
