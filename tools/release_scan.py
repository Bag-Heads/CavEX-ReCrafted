#!/usr/bin/env python3
"""REL-18e2: scan ONE exact export tree + release zip for quotes, secrets and rights.

  release_scan.py --tree DIR --package ZIP --dependencies JSON --assets JSON --out FILE

Checks hashes (tree vs EXPORT_MANIFEST.json, zip vs SHA256SUMS), denied paths, Java-shaped
comment/fence blocks, likely credentials, and a licence/provenance record with matching
sha256 for every vendored file and asset. Findings are type/where/file/line, never values.
Exit 1 on any finding. A clean result is a scan result only, never legal clearance.
Schema 1: dependencies {"dependencies": [{"name", "version", "origin", "licence",
"licence_file", "files": [{"path", "sha256"}]}]}; assets {"assets": [{"sha256", "paths",
"licence", "licence_file"|"licence_url", "author", "source"}]}.
"""
import argparse, fnmatch, hashlib, json, re, sys, zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from clean_export import MANIFEST, denied  # tree uses the exporter's own deny rules

STATUS = "scan-result-not-legal-clearance"
VENDORED = ("third_party/", "source/cglm/", "source/cnbt/", "source/lodepng/", "source/parson/")
ASSET_EXT = {".png", ".jpg", ".bmp", ".gif", ".wav", ".ogg", ".mp3", ".mid", ".ttf", ".otf", ".fnt"}
PKG_DENY = ["*.jar", "*.class", "*.java", "*.env", "*.pem", "*.key", "*.p12", "*.zip", "*.7z", "*pass.json",
            "*oracle*", "*decompile*", "*secret*", "*credential*", "*capture*", "*beta_pack*",
            ".git/*", "*/.git/*", ".minecraft/*", "*/.minecraft/*", "tmp/*", ".claude/*"]
MAX_MEMBER = 256 * 1024 * 1024
SECRETS = {k: re.compile(v) for k, v in {  # patterns from tmp/jobs/release-audit/scan.py
    "SECRET_PREFIX": rb"(?<![A-Za-z0-9])(?:sk-or-v1-[A-Za-z0-9_-]{16,}|sk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{24,}"
                     rb"|gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}"
                     rb"|xox[baprs]-[A-Za-z0-9-]{16,}|AIza[A-Za-z0-9_-]{30,})",
    "PRIVATE_KEY": rb"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----",
    "JWT_LIKE": rb"eyJ[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,}",
    "SECRET_ASSIGNMENT": rb"(?i)(?:(?:api[_-]?key|(?:access|auth|refresh)[_-]?token|client[_-]?secret|password"
                         rb"|passwd)[\x22\x27]?\s*[=:]\s*[\x22\x27]?|bearer\s+)([A-Za-z0-9/+=-]{12,})(?![\w.(])"}.items()}
PLACEHOLDER = re.compile(rb"(?i)example|placeholder|changeme|redacted|your[_-]|environ|getenv|xxxx")
# One Java-shaped line; a factual citation ("BlockTNT.java:41-53") matches none of these.
JAVA_LINE = re.compile(r"^\s*(?:public|private|protected)\s+[\w<>\[\], ]+[({;=]|\bvar\d+\b|\b(?:this|super)\.\w+"
                       r"|\bnew\s+[A-Z]\w*\s*\(|\b(?:boolean|World|Entity|Block|ItemStack|Random)\s+\w+\s*[=;]")
C_LIKE = {".c", ".h", ".cpp", ".hpp", ".inc", ".js", ".cs", ".java"}
HASH_LIKE = {".py", ".sh", ".cmake", ".txt", ".ps1", ".yaml", ".yml", ".toml", ""}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def comment_lines(path, text):
    """Yield (line_no, comment text or None); None breaks a block."""
    ext, in_block, fence = Path(path).suffix.lower(), False, False
    for no, line in enumerate(text.splitlines(), 1):
        if ext in (".md", ".markdown"):
            fence ^= line.lstrip().startswith("```")
            yield no, line if fence and not line.lstrip().startswith("```") else None
        elif ext in C_LIKE and (in_block or "/*" in line):
            body = line if in_block else line.split("/*", 1)[1]
            in_block = "*/" not in body
            yield no, body.split("*/")[0].lstrip(" *")
        elif ext in C_LIKE:
            yield no, line.split("//", 1)[1] if "//" in line else None
        elif ext in HASH_LIKE:
            yield no, line.split("#", 1)[1] if "#" in line else None
        else:
            yield no, None

def scan_content(add, where, path, data):
    binary = b"\0" in data[:8192]
    for kind, rx in SECRETS.items():
        for m in ([] if binary and kind in ("SECRET_ASSIGNMENT", "JWT_LIKE") else rx.finditer(data)):
            if not (kind == "SECRET_ASSIGNMENT" and PLACEHOLDER.search(m.group(1))):
                add(kind, where, path, data.count(b"\n", 0, m.start()) + 1)
    if Path(path).suffix.lower() in (".java", ".class"):
        add("JAVA_FILE", where, path)
    run = []
    for no, text in ([] if binary else list(comment_lines(path, data.decode("utf-8", "replace")))) + [(0, None)]:
        if text is not None and JAVA_LINE.search(text):
            run.append(no)
        elif text is None:
            if len(run) >= 3:
                add("JAVA_QUOTE_BLOCK", where, path, run[0])
            run = []

def incomplete(rec, keys):
    return any(not str(rec.get(k, "")).strip() or str(rec.get(k)).lower() == "unknown" for k in keys)

def check_rights(add, files, deps, assets):
    """files: {(where, path): data}. Every vendored file and asset needs a matching record."""
    tree, covered, by_path = {p: d for (w, p), d in files.items() if w == "tree"}, set(), {}
    for rec in deps.get("dependencies", []):
        name = "dependency:%s" % rec.get("name", "?")
        if incomplete(rec, ("name", "version", "origin", "licence", "licence_file")):
            add("DEP_RECORD_INCOMPLETE", "inventory", name)
        if rec.get("licence_file") and rec["licence_file"] not in tree:
            add("DEP_LICENCE_TEXT_MISSING", "inventory", name)
        for f in rec.get("files", []):
            covered.add(f.get("path"))
            if f.get("path") not in tree:
                add("DEP_FILE_MISSING", "inventory", str(f.get("path")))
            elif sha(tree[f["path"]]) != f.get("sha256"):
                add("DEP_HASH_MISMATCH", "tree", f["path"])
    for p in sorted(tree):
        if p.lower().startswith(VENDORED) and p not in covered:
            add("DEP_RECORD_MISSING", "tree", p)
    for rec in assets.get("assets", []):
        if incomplete(rec, ("sha256", "licence", "author", "source")) or not (
                rec.get("licence_file") or rec.get("licence_url")):
            add("ASSET_RECORD_INCOMPLETE", "inventory", ",".join(rec.get("paths", [])) or "?")
        by_path.update({p: rec for p in rec.get("paths", [])})
    for (where, p), data in sorted(files.items()):
        if Path(p).suffix.lower() in ASSET_EXT and p not in by_path:
            add("ASSET_RECORD_MISSING", where, p)
        elif Path(p).suffix.lower() in ASSET_EXT and by_path[p].get("sha256") != sha(data):
            add("ASSET_HASH_MISMATCH", where, p)

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    for opt in ("--tree", "--package", "--dependencies", "--assets", "--out"):
        ap.add_argument(opt, required=True)
    a = ap.parse_args()
    tree, out, files = Path(a.tree), [], {}
    add = lambda kind, where, path, line=0: out.append({"type": kind, "where": where, "file": path, "line": line})
    manifest = json.loads((tree / MANIFEST).read_bytes())
    want = {f["path"]: f["sha256"] for f in manifest["files"]}
    for p in sorted(tree.rglob("*")):
        rel = p.relative_to(tree).as_posix()
        if p.is_file() and rel != MANIFEST:
            files[("tree", rel)] = p.read_bytes()
            if want.get(rel) != sha(files[("tree", rel)]):
                add("TREE_MANIFEST_MISMATCH", "tree", rel)
            if denied(rel):
                add("DENIED_PATH", "tree", rel)
    for rel in sorted(set(want) - {p for _, p in files}):
        add("TREE_FILE_MISSING", "tree", rel)
    with zipfile.ZipFile(a.package) as z:
        for info in (i for i in z.infolist() if not i.is_dir()):
            if any(fnmatch.fnmatchcase(info.filename.lower(), g) for g in PKG_DENY):
                add("DENIED_PATH", "package", info.filename)
            if info.file_size > MAX_MEMBER:
                add("PKG_MEMBER_TOO_LARGE", "package", info.filename)
            else:
                files[("package", info.filename)] = z.read(info)
    sums = dict(m.group(2, 1) for m in re.finditer(r"(?m)^([0-9a-f]{64}) [ *](.+?)\r?$", files.get(
        ("package", "SHA256SUMS"), b"").decode("utf-8", "replace")))
    for (w, p), data in sorted(files.items()):
        if w == "package" and p != "SHA256SUMS" and sums.get(p) != sha(data):
            add("PKG_SUMS_MISMATCH", "package", p)
        scan_content(add, w, p, data)
    check_rights(add, files, json.loads(Path(a.dependencies).read_bytes()), json.loads(Path(a.assets).read_bytes()))
    report = {"schema": 1, "status": STATUS, "source_commit": manifest.get("source_commit"),
              "tree_files": sum(w == "tree" for w, _ in files),
              "package_members": sum(w == "package" for w, _ in files), "findings": out}
    Path(a.out).write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")
    print("release_scan: %d finding(s) (%s)" % (len(out), STATUS))
    return 1 if out else 0

if __name__ == "__main__":
    sys.exit(main())
