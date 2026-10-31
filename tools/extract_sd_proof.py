#!/usr/bin/env python3
"""Read-only FAT proof extractor for the headless Dolphin prover.

Companion to tools/dolphin_prove.ps1. Reads Dolphin's live SD-card image
(Load/WiiSD.raw) and pulls the demo proof files the game fopen()s with
relative paths, which land on the SD ROOT (the chest_selftest_result.txt
precedent, source/network/server_local.c:2859; auto_screenshot ss_*.png,
source/main.c:1076; perf_log.txt, server_local.c:8440).

Modeled on tools/extract_dolphin_sd_logs.py (same pyfatfs cluster-chain read
idiom) with two deliberate differences:
  - the image is opened READ-ONLY (PyFat.open_fs opens 'rb+', which is not
    safe against a WiiSD.raw that a running Dolphin is still writing);
  - it walks the FAT ROOT directory, not APPS/<app>/SAVES, and copes with
    8.3 short names (PISTON~1.TXT, PERF_LOG.TXT, 178~848A.PNG) as well as
    long-file-name entries when pyfatfs exposes them.

CLI:
  extract_sd_proof.py <WiiSD.raw> probe <name>
      exit 0 if a root-dir file matching <name> exists (case-insensitive,
      8.3-aware; short-name matches are content-verified against
      KNOWN_HEADERS to disambiguate e.g. PISTON~1.TXT), else exit 1.
      Any read failure (image missing, torn FAT mid-write) also exits 1 so
      a poller just tries again. Bad usage exits 2.
  extract_sd_proof.py <WiiSD.raw> extract <outdir>
      copy the verdict txt (under its real name piston_blast_result.txt),
      all root *.png files, and perf_log.txt (if present) into <outdir>;
      print what was extracted. Exit 0 on a completed walk (even if empty),
      2 on a hard failure.
"""

import sys
from pathlib import Path

from pyfatfs.PyFat import PyFat


VERDICT_NAME = "piston_blast_result.txt"
PERF_NAME = "perf_log.txt"

# Canonical demo verdict names, for restoring a mangled 8.3 short name to its
# long form during extraction. Keep in sync with the server_local.c demos.
KNOWN_VERDICT_NAMES = (
    "piston_blast_result.txt",
    "piston_sticky_result.txt",
    "nether_roundtrip_result.txt",
    "tnt_chain_result.txt",
    "world_reload_result.txt",
    "chest_selftest_result.txt",
)

# Content verification for files matched only via a mangled 8.3 short name:
# PISTON~1.TXT could be any piston_*.txt, so the FIRST non-empty line must
# contain every token of at least one group. piston_blast_drive
# (server_local.c) writes "carrier_cell PASS|FAIL ..." first on the normal
# path and "center_carved SKIP ..." first on the skip path; the bare-VERDICT
# and titled-header groups tolerate future format drift. Names without an
# entry here are accepted on the name match alone. A caught-mid-write 0-byte
# file never matches any group, so a poller just retries.
KNOWN_HEADERS = {
    VERDICT_NAME: (
        ("CARRIER_CELL",),
        ("CENTER_CARVED",),
        ("VERDICT",),
        ("PISTON", "BLAST"),
    ),
}


def read_entry_bytes(fs, entry):
    size = entry.get_size()
    if size <= 0:
        return b""

    cluster = entry.get_cluster()
    if not cluster:
        return b""

    data = bytearray()
    for c in fs.get_cluster_chain(cluster):
        data.extend(fs.read_cluster_contents(c))
        if len(data) >= size:
            break

    return bytes(data[:size])


def entry_names(entry):
    """(short_name, long_name_or_None) — get_long_name raises when no LFN."""
    short = entry.get_short_name()
    try:
        lfn = entry.get_long_name()
    except Exception:
        lfn = None
    return short, lfn


def short_name_matches(short, target):
    """8.3-aware prefix+extension match: PISTON~1.TXT vs piston_blast_result.txt.

    Handles both manglings seen on this card: numeric tail (PISTON~1) and
    hash tail (178~848A) — the part before '~' must be a non-empty prefix of
    the target stem. All target names here are 8.3-legal charset (alnum + _),
    so no dropped-character handling is needed.
    """
    s_stem, _, s_ext = short.upper().partition(".")
    t_stem, _, t_ext = target.upper().rpartition(".")
    if s_ext != t_ext[:3]:
        return False
    if "~" not in s_stem:
        return len(t_stem) <= 8 and s_stem == t_stem
    prefix = s_stem.split("~", 1)[0]
    return bool(prefix) and t_stem.startswith(prefix)


def match_kind(short, lfn, target):
    """'exact' (full-name match), 'short' (8.3-mangled match), or None."""
    t = target.lower()
    if lfn is not None and lfn.lower() == t:
        return "exact"
    if short.lower() == t:
        return "exact"
    if short_name_matches(short, target):
        return "short"
    return None


def has_verdict_line(data):
    """Generic verdict-file sniff: any line starting 'VERDICT ' (all the
    smoke demos end their result file with 'VERDICT PASS|FAIL|SKIP')."""
    text = data[:1024].decode("ascii", "replace").upper()
    return any(ln.strip().startswith("VERDICT ") for ln in text.splitlines())


def header_ok(data, target):
    groups = KNOWN_HEADERS.get(target.lower())
    if groups is None:
        # No per-name header spec: a short-name match is still ambiguous
        # between sibling *_result.txt demos (piston_blast vs piston_sticky
        # both mangle to PISTON~N.TXT), so require the generic verdict shape.
        # The prover also sweeps every stale *_result.txt before each run,
        # and demos are mutually exclusive per boot, so at most one verdict
        # file exists in a fresh image.
        return has_verdict_line(data)
    text = data[:512].decode("ascii", "replace").upper()
    first = next((ln.strip() for ln in text.splitlines() if ln.strip()), "")
    return any(all(tok in first for tok in group) for group in groups)


def root_files(fs):
    _dirs, files, _specials = fs.root_dir.get_entries()
    return files


def find_named(fs, target):
    """Best root-dir entry for <target>: exact match beats a mangled-short
    match; short matches must pass the KNOWN_HEADERS content check."""
    best = None
    for entry in root_files(fs):
        short, lfn = entry_names(entry)
        kind = match_kind(short, lfn, target)
        if kind is None:
            continue
        if kind == "exact":
            return entry
        if best is None and header_ok(read_entry_bytes(fs, entry), target):
            best = entry
    return best


def probe(image, name):
    fs = PyFat()
    try:
        fs.open(image, read_only=True)
    except Exception as ex:
        print(f"probe: cannot open image ({ex})", file=sys.stderr)
        return 1
    try:
        entry = find_named(fs, name)
        if entry is None:
            print(f"probe: no {name} yet")
            return 1
        short, lfn = entry_names(entry)
        shown = lfn if lfn else short
        print(f"probe: HIT {shown} (short={short}, {entry.get_size()} bytes)")
        return 0
    except Exception as ex:
        # Live image: a torn FAT mid-write just reads as "not yet".
        print(f"probe: transient read failure ({ex})", file=sys.stderr)
        return 1
    finally:
        try:
            fs.close()
        except Exception:
            pass


def unique_dest(outdir, name, used):
    base = name
    n = 2
    while base.lower() in used:
        stem, _, ext = name.rpartition(".")
        if stem:
            base = f"{stem}_{n}.{ext}"
        else:
            base = f"{name}_{n}"
        n += 1
    used.add(base.lower())
    return outdir / base


def extract(image, outdir):
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    fs = PyFat()
    try:
        fs.open(image, read_only=True)
    except Exception as ex:
        print(f"extract: cannot open image ({ex})", file=sys.stderr)
        return 2
    try:
        used = set()
        count = 0

        def emit(entry, out_name):
            nonlocal count
            dest = unique_dest(outdir, out_name, used)
            dest.write_bytes(read_entry_bytes(fs, entry))
            print(f"extracted {out_name} -> {dest} ({dest.stat().st_size} bytes)")
            count += 1

        # Every verdict file present (demo *_result.txt with a VERDICT line),
        # not just one hardcoded name -- the prover is demo-agnostic now.
        emitted = set()
        for entry in root_files(fs):
            short, lfn = entry_names(entry)
            name = (lfn if lfn else short).lower()
            if not name.endswith(".txt"):
                continue
            data = read_entry_bytes(fs, entry)
            if not has_verdict_line(data):
                continue
            # Restore the canonical long name when only a mangled short
            # survives. Sibling demos can share a mangled stem (PISTON~N.TXT
            # is both piston_blast and piston_sticky), so when several known
            # names match, decide by the verdict's first field token instead
            # of first-listed-wins (adversarial-review finding: the old order
            # renamed a sticky verdict to piston_blast_result.txt and the
            # prover then graded a healthy PASS run as missing).
            out = lfn if lfn else short.lower()
            if lfn is None:
                matches = [k for k in KNOWN_VERDICT_NAMES
                           if short_name_matches(short, k)]
                if len(matches) == 1:
                    out = matches[0]
                elif matches:
                    text = data[:512].decode("ascii", "replace").upper()
                    first = next((ln.strip() for ln in text.splitlines()
                                  if ln.strip()), "")
                    FIRST_TOKENS = {
                        "CARRIER_CELL": "piston_blast_result.txt",
                        "CENTER_CARVED": "piston_blast_result.txt",
                        "EXTEND_PUSHED_WOOL": "piston_sticky_result.txt",
                        "LEG1_OVERWORLD_TO_NETHER":
                            "nether_roundtrip_result.txt",
                        "ALL_TNT_CONSUMED": "tnt_chain_result.txt",
                        "PHASE_CREATE": "world_reload_result.txt",
                        "PHASE_VERIFY_MARKERS": "world_reload_result.txt",
                    }
                    for tok, name in FIRST_TOKENS.items():
                        if first.startswith(tok) and name in matches:
                            out = name
                            break
                    else:
                        out = matches[0]
            emit(entry, out)
            emitted.add(id(entry))

        perf = find_named(fs, PERF_NAME)
        if perf is not None and id(perf) not in emitted:
            emit(perf, PERF_NAME)

        for entry in root_files(fs):
            if id(entry) in emitted or entry is perf:
                continue
            short, lfn = entry_names(entry)
            name = lfn if lfn else short
            if not name.lower().endswith(".png"):
                continue
            emit(entry, name if lfn else short.lower())

        print(f"extracted {count} file(s) -> {outdir}")
        return 0
    except Exception as ex:
        print(f"extract: failed ({ex})", file=sys.stderr)
        return 2
    finally:
        try:
            fs.close()
        except Exception:
            pass


def copy_tree(fs, dentry, dest):
    """Recursively copy one FAT directory subtree to dest. LFN names preferred
    (region files like r.0.0.mcr are not 8.3-legal, so their long names are the
    only faithful ones); '.'/'..' skipped."""
    dest.mkdir(parents=True, exist_ok=True)
    count = 0
    dirs, files, _specials = dentry.get_entries()
    for f in files:
        short, lfn = entry_names(f)
        name = lfn if lfn else short.lower()
        (dest / name).write_bytes(read_entry_bytes(fs, f))
        count += 1
    for d in dirs:
        short, lfn = entry_names(d)
        name = lfn if lfn else short.lower()
        if name in (".", ".."):
            continue
        count += copy_tree(fs, d, dest / name)
    return count


def sync_saves(image, dest_root):
    """Copy the image's saves/ tree into <dest_root>/saves -- the cold-boot
    reload prover's between-boots step: boot 1's world lives only in
    WiiSD.raw (the prover force-kills Dolphin, so no sync-back ever runs);
    this rescues it into WiiSDSync so boot 2's raw rebuild includes it."""
    fs = PyFat()
    try:
        fs.open(image, read_only=True)
    except Exception as ex:
        print(f"syncsaves: cannot open image ({ex})", file=sys.stderr)
        return 2
    try:
        dirs, _files, _specials = fs.root_dir.get_entries()
        saves = None
        for d in dirs:
            short, lfn = entry_names(d)
            if (lfn if lfn else short).lower() == "saves":
                saves = d
                break
        if saves is None:
            print("syncsaves: no saves/ directory in the image", file=sys.stderr)
            return 1
        dest = Path(dest_root) / "saves"
        count = copy_tree(fs, saves, dest)
        print(f"syncsaves: {count} file(s) -> {dest}")
        return 0
    except Exception as ex:
        print(f"syncsaves: failed ({ex})", file=sys.stderr)
        return 2
    finally:
        try:
            fs.close()
        except Exception:
            pass


def main(argv):
    if len(argv) != 4 or argv[2] not in ("probe", "extract", "syncsaves"):
        print(
            "usage: extract_sd_proof.py <WiiSD.raw> probe <name>\n"
            "       extract_sd_proof.py <WiiSD.raw> extract <outdir>\n"
            "       extract_sd_proof.py <WiiSD.raw> syncsaves <WiiSDSync-dir>",
            file=sys.stderr,
        )
        return 2
    image, mode, arg = argv[1], argv[2], argv[3]
    if mode == "probe":
        return probe(image, arg)
    if mode == "syncsaves":
        return sync_saves(image, arg)
    return extract(image, arg)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
