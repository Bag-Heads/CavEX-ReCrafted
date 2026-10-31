"""In-game Beta importer, sound phase (source/platform/beta_import_sound.c)
== the PC Pack Helper, byte for byte.

    test_beta_import_sound.py <test_beta_import_sound exe> <sound_transcode exe>

Builds a synthetic .minecraft-style resources/ tree (ffmpeg-made fixture
tones from tests/fixtures/beta_import_sound/ plus WAVs generated here; never
Mojang files) that exercises every rule of the helper's process_sounds:
sound/ + newsound/ union, byte-identical dedup, lexicographic variant order
(grass1 < grass10 < grass2), the 14-variant namespace (13 numbered + bare,
15th dropped), a corrupt ogg that is skipped but keeps its slot, a WAV source,
stereo downmix, and the 32 kHz cap. Then:

  1. runs the engine phase (the C driver -> bi_sounds_import) into staging A;
  2. runs tools/pack_helper/owned_importer.process_sounds (strict, through a
     transcoder, exactly as the helper GUI does) into B. The transcoder is the
     driver's own --tool form: tools/sound_transcode's main() path (whole-file
     stb_vorbis_decode_memory + wavh_write) compiled by the same compiler as
     the engine phase, so the diff isolates the two NEW paths (pull decode,
     chunked write) from compiler drift;
  3. requires A == B as trees and as bytes, and the driver's count to equal
     the helper's written-variant count;
  4. also runs the built sound_transcode binary itself on each source and
     requires its bytes too. A mismatch there with 1-3 green is a
     differently-compiled sound_transcode binary, and the known way to get
     one is the C mode: in strict ISO mode (-std=c99/c11, as
     pack_helper/build_resources.py and the Wii Makefile compile) <math.h>
     has no M_PI, so stb_vorbis uses its own FLOAT literal for its twiddle
     tables; in GNU mode (CMake's C_STANDARD 99 + extensions) M_PI is a
     double. Some samples then differ by 1 LSB (measured 2026-10-30). CMake
     builds both binaries of this test in the same mode, so they agree.

Also: no resources folder -> result 0 and a "textures only" message; cancel
mid-run -> result -1, BI_CANCELLED, no torn file left behind.
"""
from __future__ import annotations

import hashlib
import importlib.util
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
FIXTURES = REPO_ROOT / "tests" / "fixtures" / "beta_import_sound"
HELPER_PATH = REPO_ROOT / "tools" / "pack_helper" / "owned_importer.py"

BI_SOUNDS = 3
BI_FAILED = 6
BI_CANCELLED = 7


def load_helper():
    spec = importlib.util.spec_from_file_location("owned_importer", HELPER_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def make_wav(path: Path, rate: int, frames: int, freq: float, amp: int = 12000,
             phase: float = 0.0) -> None:
    """Canonical-looking PCM16 mono WAV with distinct content per call."""
    pcm = bytearray()
    for n in range(frames):
        v = int(amp * math.sin(2 * math.pi * freq * n / rate + phase))
        pcm += struct.pack("<h", max(-32768, min(32767, v)))
    hdr = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    hdr += b"data" + struct.pack("<I", len(pcm))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(hdr + pcm)


def build_resources(root: Path) -> None:
    snd = root / "sound"
    new = root / "newsound"
    mono44 = (FIXTURES / "tone_mono_44k.ogg").read_bytes()
    st22 = (FIXTURES / "tone_stereo_22k.ogg").read_bytes()
    st48 = (FIXTURES / "tone_stereo_48k.ogg").read_bytes()
    wav11 = (FIXTURES / "tone_mono_11k.wav").read_bytes()

    def put(p: Path, data: bytes) -> None:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    # step.grass: ordering + union + dedup + a WAV source + a corrupt slot.
    #   sorted names: grass1, grass10, grass2, grass3(wav), grass4(bad)
    #   newsound/grass1.ogg == sound/grass1.ogg  -> dropped (dedup)
    #   newsound/grass5.ogg distinct             -> 6th candidate
    put(snd / "step" / "grass1.ogg", mono44)
    put(snd / "step" / "grass10.ogg", st22)
    put(snd / "step" / "grass2.ogg", st48)
    put(snd / "step" / "grass3.wav", wav11)
    put(snd / "step" / "grass4.ogg", b"OggS" + bytes(range(256)) * 8)  # corrupt
    put(snd / "step" / "grassy1.ogg", mono44)   # does not match ^grass\d*\.
    put(snd / "step" / "grass1.txt", mono44)    # wrong extension
    put(new / "step" / "grass1.ogg", mono44)    # byte-identical duplicate
    make_wav(new / "step" / "grass5.wav", 16000, 500, 523.0)  # distinct, newsound only
    (snd / "step" / "grass6.ogg").mkdir(parents=True)  # a DIRECTORY with a matching name

    # step.cloth: 15 distinct WAVs -> cloth1..cloth13, cloth, 15th dropped.
    for k in range(1, 16):
        make_wav(snd / "step" / ("cloth%d.wav" % k), 8000, 400 + k, 300.0 + 7 * k)

    # mob.zombiepig.zpig: a three-level key; only in newsound/.
    put(new / "mob" / "zombiepig" / "zpig1.ogg", st22)
    put(new / "mob" / "zombiepig" / "zpig2.ogg", st48)

    # ambient.cave.cave: 13 variants named like Beta (cave1..cave13), all
    # distinct, so the lexicographic renumbering is visible in the output.
    for k in range(1, 14):
        make_wav(snd / "ambient" / "cave" / ("cave%d.wav" % k), 44100, 300 + 3 * k,
                 100.0 + 11 * k, phase=0.1 * k)

    # random.click: bare name (no digit) counts as a variant too.
    put(snd / "random" / "click.ogg", mono44)

    # A key that is NOT in the table must be ignored.
    put(snd / "nosuch" / "thing1.ogg", mono44)

    # Music: music/ + newmusic/, verbatim, first NAME wins; records:
    # streaming/ stems 13 and cat only, lower-cased, first wins.
    put(root / "music" / "calm1.ogg", st22)
    put(root / "music" / "Calm2.OGG", st48)          # glob is case-insensitive on Windows
    put(root / "music" / "readme.txt", b"not music")
    put(root / "newmusic" / "calm1.ogg", mono44)     # same NAME -> skipped (bytes differ!)
    put(root / "newmusic" / "hal1.ogg", mono44)
    put(root / "streaming" / "13.ogg", st48)
    put(root / "streaming" / "Cat.ogg", st22)        # -> records/cat.ogg
    put(root / "streaming" / "cat.ogg", mono44)      # second cat stem -> skipped
    put(root / "streaming" / "blocks.ogg", mono44)   # not a Beta record -> ignored


def tree(root: Path) -> dict:
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            out[p.relative_to(root).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    return out


def run_driver(exe: Path, resources, staging: Path, cancel_after=None) -> dict:
    args = [str(exe), str(resources) if resources else "-", str(staging)]
    if cancel_after is not None:
        args.append(str(cancel_after))
    proc = subprocess.run(args, capture_output=True, text=True, timeout=600)
    if proc.returncode != 0:
        raise AssertionError("driver exited %d\n%s\n%s" % (proc.returncode, proc.stdout, proc.stderr))
    out = {}
    for line in proc.stdout.splitlines():
        k, _, v = line.partition(" ")
        out[k] = v
    out["stderr"] = proc.stderr
    return out


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_beta_import_sound.py <driver exe> <sound_transcode exe>")
        return 2
    driver = Path(sys.argv[1])
    transcoder = Path(sys.argv[2])
    assert driver.is_file(), driver
    assert transcoder.is_file(), transcoder
    helper = load_helper()
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)
            print("FAIL:", what)

    with tempfile.TemporaryDirectory(prefix="bi_sound_") as td:
        td = Path(td)
        res = td / "resources"
        build_resources(res)

        # 1+2. engine vs helper on the same tree.
        a = td / "staging_engine"
        b = td / "staging_helper"
        a.mkdir()
        b.mkdir()
        r = run_driver(driver, res, a)
        tool_wrapper = td / ("bi_tool.cmd" if os.name == "nt" else "bi_tool.sh")
        if os.name == "nt":
            tool_wrapper.write_text('@"%s" --tool %%1 %%2\r\n' % driver)
        else:
            tool_wrapper.write_text('#!/bin/sh\nexec "%s" --tool "$1" "$2"\n' % driver)
            tool_wrapper.chmod(0o755)
        results = helper.process_sounds(res, b, False, tool_wrapper, strict_transcode=True)
        music_results = helper.process_music(res, b, False) + helper.process_records(res, b, False)
        helper_written = sum(e.get("variant_count", 0) for e in results)
        helper_written += sum(1 for e in music_results if e.get("status") == "copied")
        ta, tb = tree(a), tree(b)
        check(ta == tb, "engine tree != helper tree\n engine: %s\n helper: %s" % (
            sorted(set(ta) - set(tb)) or [k for k in ta if ta[k] != tb.get(k)],
            sorted(set(tb) - set(ta))))
        check(r["RESULT"] == str(helper_written) and helper_written > 0,
              "RESULT %s != helper written %d" % (r["RESULT"], helper_written))
        check(r["STATE"] == str(BI_SOUNDS), "final state %s" % r["STATE"])
        check(r["PROGRESS"].split("/")[0] == r["PROGRESS"].split("/")[1] != "0",
              "progress not complete: %s" % r["PROGRESS"])

        # The layout rules, spelled out (so a helper regression cannot hide
        # behind "both wrong the same way").
        names = sorted(p for p in ta if p.startswith("sound/step/grass"))
        # candidates in order: grass1(44k) grass10 grass2 grass3.wav grass4(bad)
        #   [newsound/grass1 dup dropped] grass5 -> slots 1,2,3,4,(5 skipped),6
        check(names == ["sound/step/grass1.wav", "sound/step/grass2.wav",
                        "sound/step/grass3.wav", "sound/step/grass4.wav",
                        "sound/step/grass6.wav"], "grass layout: %s" % names)
        cloth = sorted(p for p in ta if p.startswith("sound/step/cloth"))
        check(len(cloth) == 14 and "sound/step/cloth.wav" in cloth
              and "sound/step/cloth15.wav" not in cloth
              and "sound/step/cloth14.wav" not in cloth, "cloth namespace: %s" % cloth)
        cave = [p for p in ta if p.startswith("sound/ambient/cave/")]
        check(len(cave) == 13, "cave count %d" % len(cave))
        check("sound/mob/zombiepig/zpig1.wav" in ta and "sound/mob/zombiepig/zpig2.wav" in ta,
              "three-level key from newsound/")
        check("sound/random/click1.wav" in ta, "bare-name variant")
        check(not any(p.startswith("sound/nosuch") for p in ta), "unknown key ignored")
        check(not any(p.startswith("sound/") and p.endswith(".ogg") for p in ta),
              "strict mode never raw-copies a sound")
        music = sorted(p for p in ta if not p.startswith("sound/"))
        check(music == ["music/Calm2.OGG", "music/calm1.ogg", "music/hal1.ogg",
                        "records/13.ogg", "records/cat.ogg"], "music/records layout: %s" % music)
        check(ta["music/calm1.ogg"] == hashlib.sha256(
            (res / "music" / "calm1.ogg").read_bytes()).hexdigest(), "music/ wins over newmusic/ by name")
        check(ta["records/cat.ogg"] == hashlib.sha256(
            (res / "streaming" / "Cat.ogg").read_bytes()).hexdigest(), "first cat record wins")
        # 4. The built sound_transcode binary agrees on every written file
        #    (slot n <- n-th surviving candidate; the 44 kHz mono fixture is
        #    capped to 32 kHz; the WAV source has its header re-emitted).
        src_of = {}
        for e in results:
            for v in e.get("variants", ()):
                if "dest" in v:
                    src_of[v["dest"]] = Path(v["source"])
        check(set(src_of) == {p for p in ta if p.startswith("sound/")},
              "helper report lists every written sound file")
        tool_bad = []
        for dst in sorted(src_of):
            out = td / ("ref_" + dst.replace("/", "_"))
            proc = subprocess.run([str(transcoder), str(src_of[dst]), str(out)],
                                  capture_output=True, text=True)
            if proc.returncode != 0 or hashlib.sha256(out.read_bytes()).hexdigest() != ta[dst]:
                tool_bad.append(dst)
        check(not tool_bad,
              "%s differs from %s on %s. The engine phase equals the same-compiler "
              "tool core (checks 1-3), so %s was compiled differently (built %s); "
              "the known cause is the C mode (strict -std=c99/c11: stb_vorbis's float "
              "M_PI; GNU mode: <math.h>'s double M_PI -> 1-LSB sample differences). "
              "Rebuild it with the CMake target."
              % (transcoder.name, driver.name, tool_bad, transcoder.name,
                 __import__("datetime").datetime.fromtimestamp(transcoder.stat().st_mtime)))
        wav = (a / "sound/step/grass1.wav").read_bytes()
        check(struct.unpack_from("<I", wav, 24)[0] == 32000, "44.1k fixture not capped to 32k")

        # 3. No resources folder: textures-only result with a plain message.
        c = td / "staging_none"
        c.mkdir()
        r0 = run_driver(driver, None, c)
        check(r0["RESULT"] == "0", "missing folder result %s" % r0["RESULT"])
        only_music = td / "only_music"
        shutil.copytree(res / "music", only_music / "music")
        om = td / "staging_only_music"
        om.mkdir()
        rm = run_driver(driver, only_music, om)
        check(rm["RESULT"] == "2" and sorted(tree(om)) == ["music/Calm2.OGG", "music/calm1.ogg"]
              and "copied 2 music files" in rm["MSG"], "music without any sound/: %s %s" % (rm["RESULT"], rm["MSG"]))
        check("textures only" in r0["MSG"].lower() and "sd:/beta/resources" in r0["MSG"],
              "missing folder message: %s" % r0["MSG"])
        check(tree(c) == {}, "missing folder wrote files")
        r1 = run_driver(driver, td / "does_not_exist", c)
        check(r1["RESULT"] == "0" and "textures only" in r1["MSG"].lower(),
              "nonexistent folder: %s %s" % (r1["RESULT"], r1["MSG"]))
        empty = td / "empty_resources"
        (empty / "sound").mkdir(parents=True)
        r2 = run_driver(driver, empty, c)
        check(r2["RESULT"] == "0" and "textures only" in r2["MSG"].lower(),
              "empty sound folder: %s %s" % (r2["RESULT"], r2["MSG"]))
        check(tree(c) == {}, "empty folder wrote files")

        # 4. Cancel after 3 progress steps: -1, BI_CANCELLED, bounded output,
        #    and every file that IS there is a complete helper-identical one.
        d = td / "staging_cancel"
        d.mkdir()
        rc = run_driver(driver, res, d, cancel_after=3)
        check(rc["RESULT"] == "-1" and rc["STATE"] == str(BI_CANCELLED),
              "cancel: %s %s %s" % (rc["RESULT"], rc["STATE"], rc["MSG"]))
        tdd = tree(d)
        check(0 < len(tdd) <= 3, "cancel wrote %d files" % len(tdd))
        check(all(tb.get(k) == v for k, v in tdd.items()), "cancel left a torn file")

    if failures:
        print("FAIL test_beta_import_sound (%d)" % len(failures))
        return 1
    print("PASS test_beta_import_sound")
    return 0


if __name__ == "__main__":
    sys.exit(main())
