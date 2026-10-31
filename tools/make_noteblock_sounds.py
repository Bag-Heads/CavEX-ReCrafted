#!/usr/bin/env python3
"""
Generate the note-block placeholder instrument tones -- ORIGINAL, synthesized
assets (noteblock-slice, 2026-08-14).

WHY THIS EXISTS. The Beta 1.7.3 "note.*" samples (harp/bd/snare/hat/
bassattack, [740c583: BlockNote.playBlock]) are Mojang IP and are never
bundled with this project; the real files ship only via the user's own
Beta jar through the Pack Builder (tools/make_beta_pack.ps1). Until then the
bundled (CC0) default sound set has no note.* files at all, which would leave the note
block completely silent even in the shipped configuration -- and the S0
"every registered sound key resolves at least one placeholder file" registry
tripwire (tests/test_audio_env.c) requires SOMETHING here (matching the
minecart.base precedent, tools/make_minecart_sound.py). So: five short,
ORIGINAL decaying tones, one per Beta instrument name, synthesized from
first principles -- no recorded source, no third-party audio. Each sample
represents the ENGINE's reference pitch (pitch 1.0, i.e. Beta note=12); the
runtime scales playback rate by the server-rolled pitch (audio_play3d), so
only one sample per instrument is needed.

Usage: python tools/make_noteblock_sounds.py [--dest assets/sound/note]
"""

import argparse
import math
import os
import random
import struct

RATE = 22050
DUR = 0.9  # seconds -- long enough for a full decay tail, short enough to
           # not overlap badly under rapid retriggering


def _envelope(t, decay):
    return math.exp(-t * decay)


def _harp(rng):
    """Plucked-string-like tone: a fundamental + a couple of harmonics,
    fast attack, moderate decay -- generic "harp" pluck, not any real
    instrument sample."""
    n = int(RATE * DUR)
    out = [0.0] * n
    f0 = 440.0
    for i in range(n):
        t = i / RATE
        env = _envelope(t, 4.5)
        s = (
            math.sin(2 * math.pi * f0 * t) * 0.6
            + math.sin(2 * math.pi * f0 * 2 * t) * 0.25
            + math.sin(2 * math.pi * f0 * 3 * t) * 0.12
        )
        out[i] = s * env
    return out


def _bd(rng):
    """Bass drum: a fast downward pitch sweep + thump, very short decay."""
    n = int(RATE * DUR)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        env = _envelope(t, 9.0)
        freq = 120.0 * math.exp(-t * 18.0) + 50.0
        out[i] = math.sin(2 * math.pi * freq * t) * env
    return out


def _snare(rng):
    """Snare: band-limited noise with a sharp transient click."""
    n = int(RATE * DUR)
    out = [0.0] * n
    lp = 0.0
    for i in range(n):
        t = i / RATE
        env = _envelope(t, 7.0)
        white = rng.uniform(-1.0, 1.0)
        lp += 0.5 * (white - lp)
        click = rng.uniform(-1.0, 1.0) * _envelope(t, 60.0) * 0.5
        out[i] = lp * env * 0.8 + click
    return out


def _hat(rng):
    """Hi-hat: bright, short, high-passed-feeling noise burst."""
    n = int(RATE * DUR)
    out = [0.0] * n
    lp = 0.0
    for i in range(n):
        t = i / RATE
        env = _envelope(t, 22.0)
        white = rng.uniform(-1.0, 1.0)
        lp += 0.35 * (white - lp)
        hp = white - lp  # crude high-pass: original minus its own low-pass
        out[i] = hp * env
    return out


def _bassattack(rng):
    """Bass-attack (wood): a punchy low-mid plucked tone, snappier attack
    than the harp, shorter decay."""
    n = int(RATE * DUR)
    out = [0.0] * n
    f0 = 220.0
    for i in range(n):
        t = i / RATE
        env = _envelope(t, 6.0)
        s = (
            math.sin(2 * math.pi * f0 * t) * 0.7
            + math.sin(2 * math.pi * f0 * 1.5 * t) * 0.2
        )
        click = rng.uniform(-1.0, 1.0) * _envelope(t, 120.0) * 0.3
        out[i] = s * env + click
    return out


INSTRUMENTS = {
    "harp": _harp,
    "bd": _bd,
    "snare": _snare,
    "hat": _hat,
    "bassattack": _bassattack,
}


def _normalize_fade(samples):
    peak = max(1e-9, max(abs(v) for v in samples))
    gain = 0.85 / peak
    out = [v * gain for v in samples]
    fade = int(RATE * 0.003)
    for i in range(fade):
        w = i / fade
        out[i] *= w
        out[-(i + 1)] *= w
    return out


def _write_wav(path, samples):
    frames = b"".join(
        struct.pack("<h", max(-32768, min(32767, int(v * 32767.0)))) for v in samples
    )
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(frames)))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", len(frames)))
        f.write(frames)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", default="assets/sound/note")
    args = ap.parse_args()
    os.makedirs(args.dest, exist_ok=True)
    for name, synth in INSTRUMENTS.items():
        rng = random.Random(hash(name) & 0xFFFFFFFF)
        samples = _normalize_fade(synth(rng))
        p = os.path.join(args.dest, "%s.wav" % name)
        _write_wav(p, samples)
        print("wrote", p)


if __name__ == "__main__":
    main()
