# Vendored miniaudio — provenance (m-lib PATCHES.md precedent)

- **File:** `miniaudio.h` — single-file audio playback/capture library,
  **v0.11.25** (2026-03-04) by David Reid (header banner "miniaudio -
  v0.11.25 - 2026-03-04", https://miniaud.io). Vendored 2026-07-28 for the S0
  audio foundation: the PC backend (`source/platform/pc/audio.c`) opens a
  LOW-LEVEL `ma_device` and mixes voices itself so the PC and Wii (AESND)
  backends share one behaviorally-identical voice model — `ma_engine` and the
  high-level layers are deliberately compiled out (`MA_NO_ENGINE`,
  `MA_NO_NODE_GRAPH`, `MA_NO_RESOURCE_MANAGER`, `MA_NO_GENERATION`,
  `MA_NO_DECODING`, `MA_NO_ENCODING` — decoding is stb_vorbis' job).
- **License:** dual-licensed, choose either — Public Domain (unlicense.org)
  OR MIT No Attribution (Copyright 2026 David Reid). Full text of both is
  embedded at the bottom of `miniaudio.h` itself; no separate LICENSE file
  ships upstream. CaveWii uses it under the Public Domain alternative.
- **Patches: NONE.** Pristine upstream copy, byte-identical to the release.
  `MA_IMPLEMENTATION` is defined in exactly one translation unit
  (`source/platform/pc/audio.c`); everything else sees it header-only.
- **Re-vendoring:** drop in the new upstream file, confirm the version banner,
  and re-run `test_audio_env` plus a PC game boot (device open + one-shot
  playback). There are no local patch regions to preserve.
