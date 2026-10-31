# Vendored stb_vorbis — provenance (m-lib PATCHES.md precedent)

- **File:** `stb_vorbis.c` — Ogg Vorbis audio decoder, **v1.22** by Sean Barrett
  (header line 1: "Ogg Vorbis audio decoder - v1.22 - public domain",
  http://nothings.org/stb_vorbis/). Vendored 2026-07-28 for the S0 audio
  foundation (decode-at-load of `.ogg` sound-pack files into the PCM arena;
  the decode runs in the load path only, never per-frame).
- **License:** dual-licensed, "choose whichever you prefer" — MIT (Copyright
  (c) 2017 Sean Barrett) OR Public Domain (unlicense.org). The full text of
  both is embedded at the bottom of `stb_vorbis.c` itself; no separate
  LICENSE file ships upstream. CaveWii uses it under the Public Domain
  alternative.
- **Patches: NONE.** This is a pristine upstream copy — byte-identical to the
  single-file release. It is compiled by `#include`-ing the `.c` into ONE
  translation unit PER BINARY, the intended stb usage; no build-system source
  entry exists for it. Engine binaries: `source/platform/audio_registry.c`.
  The S1 host pack tool (`tools/sound_transcode.c`, its own executable) is
  the implementation TU there; when that same file is compiled INTO
  `test_audio_env` it defines `STB_VORBIS_HEADER_ONLY` and links the
  registry's copy instead (`SOUND_TRANSCODE_EXTERN_VORBIS`).
- **Re-vendoring:** drop in the new upstream file, confirm the version banner
  on line 1, and re-run the audio suite (`test_audio_env`). There are no
  local patch regions to preserve.
