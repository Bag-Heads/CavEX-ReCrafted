# Ogg Vorbis fixtures for the deferred sound loader tests

Both files are synthetic sine tones generated for this project on 2026-09-19
with ffmpeg (libvorbis). They contain no third-party audio and are released
under CC0 with the rest of the project-made test data.

- `cave_mono.ogg` -- 0.7 s, 220 Hz, mono, 32 kHz:
  `ffmpeg -f lavfi -i "sine=frequency=220:sample_rate=32000:duration=0.7" -ac 1 -c:a libvorbis -q:a 2 -map_metadata -1 -fflags +bitexact -flags:a +bitexact cave_mono.ogg`
- `cave_stereo.ogg` -- 0.5 s, 330 Hz left / 495 Hz right, 44.1 kHz (different
  content per channel, so the average downmix is actually exercised):
  `ffmpeg -f lavfi -i "sine=frequency=330:sample_rate=44100:duration=0.5" -f lavfi -i "sine=frequency=495:sample_rate=44100:duration=0.5" -filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo[a]" -map "[a]" -c:a libvorbis -q:a 2 -map_metadata -1 -fflags +bitexact -flags:a +bitexact cave_stereo.ogg`

The tests never compare against stored PCM: they decode each file twice -- once
through the resident loader (`stb_vorbis_decode_memory`) and once through the
deferred loader (pull API, chunked, bounded decoder block) -- and require the
two results to be byte-identical. Re-encoding a fixture therefore cannot stale
a golden.
