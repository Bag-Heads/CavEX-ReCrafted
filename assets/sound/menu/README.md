# Approved menu sound vocabulary

Except for the superseding Sync Spark connection cue below, these are byte-identical copies of the maintainer-approved project-original
package approved on 2026-09-07, not recordings from another game. The first
three preserve the frozen 2026-09-06 bytes; the other five are the approved
extension. Approval/provenance: `docs/ui_design/approved/README.md`,
`docs/ui_design/approved/specs/sound-events.md`, and that package's
`manifest.json`. The source studies explicitly state that no LCE recordings or
other game audio were sampled.

License category: project-approved original pack (repository GPL-3.0-or-later).
The older CC0 placeholder declaration in assets/README.md does not relicense
these approved samples. No Mojang audio or artwork is included here.

| Registry key | File | Frozen source | SHA-256 |
|---|---|---|---|
| menu.scroll | scroll.wav | scroll.wav | 6c4f522b9f288077c2a877b7cbde5d4e2cb3ef934d51b6b516161235e0cf5b94 |
| menu.category | category.wav | category.wav | 4ab8fc44112928de9be88bbbc63b7f106a6fbb50b3eee998259f12384c1d5fd0 |
| menu.select | select.wav | craft-confirm.wav | 71d562a40fe4dc5867020fc538e3c947265ffd7ea6bc3c94f9bb04290d54fe6e |
| menu.back | back.wav | back.wav | f77469225c045851cf6b8561291d339265b3556fa48329a2d705a07fe2ba7ad3 |
| menu.item | item.wav | item.wav | c9773d76ae9db8c2fcdfe48a7552808fb97218e287728cd2eb3ade5111ae41f8 |
| menu.type | type.wav | type.wav | 3ba7678d1237902c765e4efaa6960a9f52d72aa74e40625eee9a6eeab1b5b800 |
| menu.connected | connected.wav | controller-sync-spark (approved 2026-09-07) | eb95def2f8e29ae6a2a36fb9b73495dc0e86d459a80a810237cbef96f37d0fe4 |
| menu.attention | attention.wav | attention.wav | f75f3531daf9489a7ca6c7a7cf4f44ec876250216f2357a620d1904933150880 |

Mono 48 kHz PCM16 WAV. The existing registry resolves each complete key pool
from <paths.texturepack>/sound/menu/, falling back to this bundled pool when
absent. Decode happens at load/reload, never during menu drawing or playback.
Missing/failed samples are silent; there is no beep or random.click fallback.

The exact event meanings, guards, arbitration and low-battery gain are governed
by the approved `specs/sound-events.md`.

**Sort completion ruling, revised 2026-09-07 (chest sound corrections lane).**
The earlier "sort lifecycle completion has no sound event" ruling was a
placeholder held until an authoritative completion point existed, and no longer
stands. A sort that COMPLETES successfully now plays `menu.select`, the approved
confirmation cue, from the single emitter `client_session_sort_tick`:
`docs/ui_design/approved/specs/crafting-chests.md:27`, "Sound only on a real
selection/menu change or completed successful craft/sort", under the runtime
rule in `specs/sound-events.md`, "Actual transfer/save success hooks must come
from authoritative completed operations". The failure half is unchanged and
still silent -- rejection, admission refusal, interruption, expiry and an
unchanged plan: same line, "Failed actions, unchanged boundaries, redraws and
stew animation are silent", and `specs/sound-events.md:20`, "Empty slots,
blocked moves, failed safe-close attempts and capacity failures are silent".
Completed item transfers play `menu.item` (`specs/sound-events.md:20`) except
the crafting RESULT slot, which keeps the confirmation cue per
`specs/sound-events.md:21`, "Keep the approved crafting cues and recipes
unchanged". The full ruling text lives in `source/platform/menu_sound.h`.

## Wii memory cost of the five added cues

All eight are mono 48 kHz PCM16. Decoded PCM never lives in MEM1 `.bss`: the
registry bump-allocates it 32-byte aligned into the dedicated **MEM2** audio
arena (`audio_registry.c`, `mem2.h` `MEM2_AUDIO_REGION_BYTES`, 12 MB), which is
reserved at init and freed only at session end.

| cue | file bytes | frames | decoded PCM | arena (32B-aligned) |
| --- | --- | --- | --- | --- |
| menu.back | 11,564 | 5,760 | 11,520 | 11,520 |
| menu.item | 11,564 | 5,760 | 11,520 | 11,520 |
| menu.type | 8,684 | 4,320 | 8,640 | 8,640 |
| menu.connected | 50,924 | 25,440 | 50,880 | 50,880 |
| menu.attention | 24,044 | 12,000 | 24,000 | 24,000 |
| **five new, total** | 107,780 | 53,280 | **106,560** | **106,560** |

Every payload is already a multiple of 32 bytes, so alignment costs nothing:
the five add **106,560 bytes of MEM2 arena** (0.85% of the 12 MB region).

The MEM1 cost is not zero, but it is the fixed per-key registry bookkeeping,
never sample data: five new `AUDIO_SOUND_TABLE` rows grow
`s_variants[AUDIO_SOUND_COUNT][AUDIO_MAX_VARIANTS]` by
5 x 14 x 12 B = **840 bytes of `.bss`**, plus `s_variant_count[5]` = 20 B
`.bss` and the five `s_names` pointers + key strings (~80 B `.rodata`/`.data`).
Total MEM1 delta ~940 bytes.

## Sync Spark approval (2026-09-07)

The maintainer selected Sync Spark from the three connection-chime auditions.
It replaces only menu.connected, with original synthesized audio and no console
recordings or samples. Reproduction and measurements live in
[controller-sync-spark](../../../docs/ui_design/controller-sync-spark/README.md).
The 0.53-second cue adds 28,800 bytes of decoded MEM2 audio over the earlier
0.23-second cue; MEM1 bookkeeping and event timing are unchanged.
