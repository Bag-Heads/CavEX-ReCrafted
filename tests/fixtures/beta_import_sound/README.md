# Fixtures for test_beta_import_sound (in-game Beta importer, sound phase)

Synthetic tones generated for this project on 2026-10-30 with ffmpeg
(libvorbis / pcm_s16le). They contain no third-party audio and are released
under CC0 with the rest of the project-made test data. Never Mojang files.

- `tone_mono_44k.ogg` -- 0.25 s, 440 Hz, mono, 44.1 kHz (above the 32 kHz
  cap, so the Lanczos downsample runs):
  `ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=0.25" -ac 1 -c:a libvorbis -q:a 2 -map_metadata -1 -fflags +bitexact -flags:a +bitexact tone_mono_44k.ogg`
- `tone_stereo_22k.ogg` -- 0.25 s, 330 Hz left / 495 Hz right, 22.05 kHz
  (stereo average downmix, no cap):
  `ffmpeg -f lavfi -i "sine=frequency=330:sample_rate=22050:duration=0.25" -f lavfi -i "sine=frequency=495:sample_rate=22050:duration=0.25" -filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo[a]" -map "[a]" -c:a libvorbis -q:a 2 -map_metadata -1 -fflags +bitexact -flags:a +bitexact tone_stereo_22k.ogg`
- `tone_stereo_48k.ogg` -- 0.2 s, 660 / 880 Hz, 48 kHz (downmix AND cap):
  `ffmpeg -f lavfi -i "sine=frequency=660:sample_rate=48000:duration=0.2" -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=0.2" -filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo[a]" -map "[a]" -c:a libvorbis -q:a 2 -map_metadata -1 -fflags +bitexact -flags:a +bitexact tone_stereo_48k.ogg`
- `tone_mono_11k.wav` -- 0.3 s, 220 Hz, mono PCM16, 11.025 kHz (WAV source
  path, header re-emitted canonically):
  `ffmpeg -f lavfi -i "sine=frequency=220:sample_rate=11025:duration=0.3" -ac 1 -c:a pcm_s16le -map_metadata -1 -fflags +bitexact tone_mono_11k.wav`

The test stores no golden PCM. It lays these out as a fake `resources/`
tree, runs the engine's `bi_sounds_import` on it, runs the PC Pack Helper's
`process_sounds` (strict, through the built `sound_transcode` tool) on the
same tree, and requires the two output trees to be byte-identical. Re-encoding
a fixture therefore cannot stale a golden.
