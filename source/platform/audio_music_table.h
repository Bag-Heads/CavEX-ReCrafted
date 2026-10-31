/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef PLATFORM_AUDIO_MUSIC_TABLE_H
#define PLATFORM_AUDIO_MUSIC_TABLE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

/*
	S3 background-music key table -- the audio_sound_table.h precedent, but
	rooted at "music/" and scoped to SoundManager's soundPoolMusic (the pool
	playRandomMusicIfReady draws from).

	Beta feeds soundPoolMusic from BOTH "music" AND "newmusic"
	[740c583: Minecraft.installResource -- `equalsIgnoreCase("music")` and
	`equalsIgnoreCase("newmusic")` both call `sndManager.addMusic`, the SAME
	method as each other; "streaming" is a SEPARATE pool (addStreaming),
	fed only from "streaming/" and played by RenderGlobal's jukebox call
	site (RenderGlobal.java:1260 -> SoundManager.playStreaming), out of
	scope here]. There is therefore only ONE background-music pool in Beta
	1.7.3, and it is NOT dimension-gated: calm/hal/nuance/piano can all play
	in the Overworld.

	The four keys below and their variant counts are the REAL Beta 1.7.3
	resources, verified against an owned .minecraft install (2026-08-31):
	  music/calm{1,2,3}.ogg       (3)
	  newmusic/hal{1,2,3,4}.ogg   (4)
	  newmusic/nuance{1,2}.ogg    (2)
	  newmusic/piano{1,2,3}.ogg   (3)
	4 keys, 12 files total (3+4+2+3 -- a prior revision of this comment said
	10, an arithmetic slip; test_audio_music_env.c's
	test_registry_scan_real_beta_layout pins the true count against exactly
	these twelve basenames so it cannot drift back). tools/make_beta_pack.ps1's
	S3 stage copies BOTH source directories into ONE <Dest>/music/ (flat, no
	renumbering needed -- the twelve real basenames never collide).

	SoundPool.getRandomSound() (the method playRandomMusicIfReady actually
	calls) picks UNIFORMLY across the pool's flat entry list, NOT per-key
	like getRandomSoundFromSoundPool -- unlike AUDIO_SOUND_TABLE's one-shot
	keys, this table's per-key grouping matters only for the SCAN (mirroring
	the digit-strip convention so a user's differently-named/numbered pack
	still resolves), never for playback odds; audio_music.c flattens every
	discovered file into one pointer table and rand_gen_int's over its
	count.
*/
#define AUDIO_MUSIC_TABLE(X)                                                  \
	X(MUSIC_CALM, "calm")                                                     \
	X(MUSIC_HAL, "hal")                                                       \
	X(MUSIC_NUANCE, "nuance")                                                 \
	X(MUSIC_PIANO, "piano")

enum audio_music_key {
#define AUDIO_MUSIC_TABLE_X(id, name) id,
	AUDIO_MUSIC_TABLE(AUDIO_MUSIC_TABLE_X)
#undef AUDIO_MUSIC_TABLE_X
		AUDIO_MUSIC_KEY_COUNT
};

/* Same digit-suffix scan cap as audio_sound_table.h's AUDIO_MAX_VARIANTS;
   Beta's largest real music key ("hal") only ships 4, so this leaves ample
   headroom for a user pack shipping more variants under one basename. */
#define AUDIO_MUSIC_MAX_VARIANTS 14

/* "calm" -> "music/calm": rooted at music/ instead of sound/. No dot/slash
   folding is needed -- every music key is a single bare word, unlike the
   sound table's dotted "step.grass"-style keys. */
static inline int audio_music_table_subpath(const char* key, char* out,
											size_t out_size) {
	return snprintf(out, out_size, "music/%s", key);
}

/*
	CREATIVE playlist -- maintainer ruling 2026-09-03 (docs/creative_mode.md):
	a creative world plays the Java 1.7.2+ creative-mode list (C418 "Volume
	Beta"): Aria Math, Biome Fest, Blind Spots, Dreiton, Haunt Muskie,
	Taswell. This is NOT a Beta 1.7.3 track -- Beta's only background-music
	pool is AUDIO_MUSIC_TABLE above, and it stays completely untouched by
	this addition (same worldgen/simulation floor as every other net-new
	feature: an addition sits ON TOP, it does not alter what already
	exists). This table is therefore a SEPARATE group with its own enum and
	its own flat catalog in audio_music.c -- a survival world's uniform pick
	over AUDIO_MUSIC_TABLE's files is completely unaffected by whether any of
	these six exist, are scanned, or are ever played.

	Rooted one level deeper than the Beta group -- "creative/aria_math" fed
	through the SAME audio_music_table_subpath() helper yields
	"music/creative/aria_math" -- so both groups share one pack subtree
	(<pack>/music/...) without name collisions (Beta's four basenames never
	start with "creative/"). The six basenames below are Mojang's own object
	names under a modern client's minecraft/sounds/music/game/creative/
	(tools/import_owned_beta_assets.py's --modern-minecraft path derives them
	from a real asset index rather than a hand-typed copy going stale).

	NEVER shipped: like the Beta music/records above, none of these six
	files is bundled under assets/ (docs/deviations.md's per-client-import-
	only legal model) -- a checkout/release with none imported is correctly,
	silently creative-musicless, and audio_music.c's per-world selection
	falls back to the Beta list whenever this catalog is empty (see
	audio_music_tick()'s "use_creative" gate). */
#define AUDIO_MUSIC_CREATIVE_TABLE(X)                                        \
	X(MUSIC_CREATIVE_ARIA_MATH, "creative/aria_math")                         \
	X(MUSIC_CREATIVE_BIOME_FEST, "creative/biome_fest")                       \
	X(MUSIC_CREATIVE_BLIND_SPOTS, "creative/blind_spots")                     \
	X(MUSIC_CREATIVE_DREITON, "creative/dreiton")                             \
	X(MUSIC_CREATIVE_HAUNT_MUSKIE, "creative/haunt_muskie")                   \
	X(MUSIC_CREATIVE_TASWELL, "creative/taswell")

enum audio_music_creative_key {
#define AUDIO_MUSIC_CREATIVE_TABLE_X(id, name) id,
	AUDIO_MUSIC_CREATIVE_TABLE(AUDIO_MUSIC_CREATIVE_TABLE_X)
#undef AUDIO_MUSIC_CREATIVE_TABLE_X
		AUDIO_MUSIC_CREATIVE_KEY_COUNT
};

/* Per-world playlist selection (maintainer ruling 2026-09-03,
   docs/creative_mode.md): a creative world draws from the creative catalog
   ONLY while it has at least one registered file; every other case --
   survival, or a creative world whose player never imported the six
   creative tracks -- falls back to the Beta survival catalog, completely
   untouched by this feature. Pure + header-only (the music_timer_force_lever
   precedent just above in audio_music_timer.h) so the decision itself is
   golden-testable independent of the world state, the client RNG stream,
   the registry scan, and the (headless-only, audio-disabled-by-default)
   decode/voice machinery -- audio_music_tick() (audio_music.c) is the one
   real call site. */
static inline bool audio_music_use_creative_catalog(bool world_is_creative,
													 int creative_catalog_count) {
	return world_is_creative && creative_catalog_count > 0;
}

/* audio_music_telemetry_key()'s creative band, in one place so the test
   suite can prove the arithmetic without a live track: 0 for an
   out-of-range key (the "should not happen" defensive case), otherwise
   AUDIO_MUSIC_KEY_COUNT+2 .. AUDIO_MUSIC_KEY_COUNT+1+
   AUDIO_MUSIC_CREATIVE_KEY_COUNT (band 1 = the Beta keys, AUDIO_MUSIC_
   KEY_COUNT+1 = a jukebox record -- see audio_music.h's hw_telemetry
   banner). */
static inline unsigned
audio_music_creative_telemetry_key(int creative_key) {
	if(creative_key < 0 || creative_key >= AUDIO_MUSIC_CREATIVE_KEY_COUNT)
		return 0;
	return (unsigned)AUDIO_MUSIC_KEY_COUNT + 2u + (unsigned)creative_key;
}

#endif
