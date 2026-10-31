# Ogg Vorbis fixture for the music read-ahead equivalence test

`stream_stereo_12s.ogg` is synthetic audio generated for this project on
2026-09-26 with ffmpeg (libvorbis). It contains no third-party audio and is
released under CC0 with the rest of the project-made test data.

- 12 s, 44.1 kHz stereo: a 220 Hz sine on the left, seeded pink noise on the
  right (different content per channel, so the average downmix is exercised),
  ~64 kbps (q0). Long enough (~130 music chunks of 4096 frames) to wrap the
  32-half production ring four times:
  `ffmpeg -f lavfi -i "sine=frequency=220:sample_rate=44100:duration=12" -f lavfi -i "anoisesrc=color=pink:sample_rate=44100:duration=12:amplitude=0.2:seed=7" -filter_complex "[0][1]amerge=inputs=2,volume=0.8[a]" -map "[a]" -ac 2 -c:a libvorbis -q:a 0 -map_metadata -1 -fflags +bitexact -flags:a +bitexact stream_stereo_12s.ogg`

The test never compares against stored PCM: it streams the file through the
real `audio_music.c` ring under the legacy and the read-ahead policies, and
against an independent whole-file `stb_vorbis` decode, and requires all of
them to be byte-identical. Re-encoding the fixture therefore cannot stale a
golden.

`empty_stereo.ogg` is a synthetic zero-sample decoder fixture (CC0). It was
made with `ffmpeg -f lavfi -i anullsrc=r=44100:cl=stereo -t 0.001 -c:a libvorbis
tiny.ogg`, then the EOS page's 64-bit granule position was set to zero and
its Ogg CRC recomputed (polynomial `0x04c11db7`). The test confirms that the
real stb_vorbis decoder opens it and emits zero frames. This distinguishes
empty decode from corrupt/zero-byte files that fail to open, and guards the
diagnostic repeat loop against spinning on a valid empty stream.
