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

#ifndef PLATFORM_AUDIO_SOUND_TABLE_H
#define PLATFORM_AUDIO_SOUND_TABLE_H

/*
	S1: the ONE sound id/key table, shared by the engine registry
	(audio_registry.c via audio.h) AND the host pack tool
	(tools/sound_transcode.c). Pure data + string helpers — NO engine deps —
	so the pack builder's manifest and the engine's scan cannot drift: both
	are generated from the same X-macro, and the enum itself is derived from
	the table (reordering one reorders the other by construction).

	Keys are Beta's SoundPool names [740c583: SoundPool.addSound strips the
	extension, then ALL trailing digits, then maps '/'->'.']; the engine
	scans the reverse mapping sound/<key-with-dots-as-slashes>{,1..13}.{wav,ogg}
	(audio_registry.c; AUDIO_MAX_VARIANTS below). SOURCE dirs for the pack builder [740c583:
	Minecraft.installResource:1380-1394]: "sound" AND "newsound" both feed
	sndManager.addSound — ONE pool, a plain UNION, no override precedence
	("sound3" does not exist in b1.7.3; "streaming"/"music"/"newmusic" go to
	the separate streaming/music pools — S3, out of scope here).
*/

#include <stddef.h>
#include <stdio.h>

/* The S0 proof-event set (see audio.h for the model):
	 - the SIX step families that exist in Beta (stone/wood/gravel/grass/
	   cloth/sand — there are no metal/snow/sand-break families; metal =
	   stone files pitched 1.5F, snow = cloth, sand-break = gravel
	   [Block.java:7-15; StepSoundStone/StepSoundSand]),
	 - the random.* one-shots S0 wires,
	 - the 8-mob idle/hurt/death table [EntityZombie..EntityChicken
	   get*Sound]. */
#define AUDIO_SOUND_TABLE(X)                                                   \
	X(SOUND_STEP_STONE, "step.stone")                                          \
	X(SOUND_STEP_WOOD, "step.wood")                                            \
	X(SOUND_STEP_GRAVEL, "step.gravel")                                        \
	X(SOUND_STEP_GRASS, "step.grass")                                          \
	X(SOUND_STEP_CLOTH, "step.cloth")                                          \
	X(SOUND_STEP_SAND, "step.sand")                                            \
	X(SOUND_RANDOM_GLASS, "random.glass") /* glass-type BREAK override */      \
	X(SOUND_RANDOM_EXPLODE, "random.explode")                                  \
	X(SOUND_AMBIENT_WEATHER_THUNDER, "ambient.weather.thunder") /* lightning */\
	X(SOUND_AMBIENT_WEATHER_RAIN, "ambient.weather.rain") /* splash loop      \
	   [740c583 EntityRenderer.renderRainSnow: playSoundEffect               \
	   "ambient.weather.rain"] -- registered 2026-08-31, closing the         \
	   documented render_weather.c skip */                                   \
	X(SOUND_RANDOM_CLICK, "random.click")                                      \
	X(SOUND_RANDOM_FUSE, "random.fuse")                                        \
	X(SOUND_RANDOM_BOW, "random.bow")                                          \
	X(SOUND_RANDOM_HURT, "random.hurt") /* player hurt AND death */            \
	X(SOUND_RANDOM_POP, "random.pop")	/* item pickup — S2 wiring */          \
	X(SOUND_RANDOM_DOOR_OPEN, "random.door_open")                              \
	X(SOUND_RANDOM_DOOR_CLOSE, "random.door_close")                            \
	/* DELIBERATE NON-BETA ADD-ON (maintainer-requested 2026-08-04; re-scoped  \
	   2026-09-03). Beta 1.7.3 has NO minecart sound: EntityMinecart makes no  \
	   playSound call and no "minecart.*" key exists anywhere in 740c583      \
	   (confirmed again against the real jar/resources tree at that commit -- \
	   the only "minecart/{base,inside}.ogg" pair in that repo lives under    \
	   its OWN "jars/resources/sound3/" folder, which the module header above \
	   already flags as NOT part of b1.7.3: sound3 also carries "fireworks"   \
	   and "dig", both years post-Beta). So a faithful cart is SILENT and     \
	   there is no authentic 1.7.3 asset to import, from a user-owned Beta    \
	   jar or otherwise. 2026-08-04 shipped a SYNTHESIZED filler              \
	   (tools/make_minecart_sound.py) as the bundled default; 2026-09-03      \
	   retired that filler from the shipped pack (clean provenance, but not   \
	   "the proper sound" the maintainer asked for) in favour of importing    \
	   the REAL modern Mojang sample the user owns, via                       \
	   tools/import_owned_beta_assets.py --modern-minecraft <dir>. So THIS    \
	   KEY NOW SHIPS NO BUNDLED DEFAULT AT ALL (silent_fallback in            \
	   test_audio_env.c, like the wolf/thunder/rain keys) -- Beta's true      \
	   silence is what a stock, unmodified pack plays; the rolling loop only  \
	   speaks once the user has imported their own minecraft/sounds/         \
	   minecart/base.ogg. Retriggered on the cart's distance odometer rather  \
	   than looped: the mixer is one-shot voices only.                       \
	   Toggle: config debug.minecart_sound "off" forces Beta silence even     \
	   with an imported asset. Documented in docs/deviations.md. */           \
	X(SOUND_MINECART_BASE, "minecart.base")                                    \
	/* Companion "you are riding" seat loop for the SAME add-on -- modern     \
	   Mojang minecraft/sounds/minecart/inside.ogg [measured locally via      \
	   ffprobe against the user's own owned copy, hash f11f499...: 5.770181s; \
	   never committed]. Ships no bundled default either; only the local     \
	   player's OWN occupied cart plays it (see                               \
	   entity_minecart.c:minecart_inside_sound). */                           \
	X(SOUND_MINECART_INSIDE, "minecart.inside")                                \
	X(SOUND_MOB_ZOMBIE, "mob.zombie")                                          \
	X(SOUND_MOB_ZOMBIEHURT, "mob.zombiehurt")                                  \
	X(SOUND_MOB_ZOMBIEDEATH, "mob.zombiedeath")                                \
	X(SOUND_MOB_SKELETON, "mob.skeleton")                                      \
	X(SOUND_MOB_SKELETONHURT, "mob.skeletonhurt") /* death == hurt */          \
	X(SOUND_MOB_CREEPER, "mob.creeper") /* hurt; creeper has NO idle */        \
	X(SOUND_MOB_CREEPERDEATH, "mob.creeperdeath")                              \
	X(SOUND_MOB_SPIDER, "mob.spider") /* idle AND hurt */                      \
	X(SOUND_MOB_SPIDERDEATH, "mob.spiderdeath")                                \
	X(SOUND_MOB_SHEEP, "mob.sheep") /* idle/hurt/death */                      \
	X(SOUND_MOB_COW, "mob.cow")                                                \
	X(SOUND_MOB_COWHURT, "mob.cowhurt") /* hurt AND death; vol 0.4F */         \
	X(SOUND_MOB_PIG, "mob.pig")			/* idle AND hurt */                    \
	X(SOUND_MOB_PIGDEATH, "mob.pigdeath")                                      \
	X(SOUND_MOB_CHICKEN, "mob.chicken")                                        \
	X(SOUND_MOB_CHICKENHURT, "mob.chickenhurt") /* hurt AND death */           \
	X(SOUND_MOB_CHICKENPLOP, "mob.chickenplop") /* egg lay */                  \
	/* Nether natives (batch 1b) [740c583: EntityPigZombie/EntityGhast         \
	   get*Sound]. zpigangry fires when randomSoundDelay expires. */           \
	X(SOUND_MOB_ZOMBIEPIG, "mob.zombiepig.zpig")                               \
	X(SOUND_MOB_ZOMBIEPIGHURT, "mob.zombiepig.zpighurt")                       \
	X(SOUND_MOB_ZOMBIEPIGDEATH, "mob.zombiepig.zpigdeath")                     \
	X(SOUND_MOB_ZOMBIEPIGANGRY, "mob.zombiepig.zpigangry")                     \
	X(SOUND_MOB_GHAST_MOAN, "mob.ghast.moan")                                  \
	X(SOUND_MOB_GHAST_SCREAM, "mob.ghast.scream")                              \
	X(SOUND_MOB_GHAST_DEATH, "mob.ghast.death")                                \
	X(SOUND_MOB_GHAST_CHARGE, "mob.ghast.charge")                              \
	X(SOUND_MOB_GHAST_FIREBALL, "mob.ghast.fireball")                          \
	/* Wolf event keys [740c583: EntityWolf get*Sound/onUpdate]. No bundled   \
	   copied audio: missing variants intentionally use registry silence. */   \
	X(SOUND_MOB_WOLF_BARK, "mob.wolf.bark")                                    \
	X(SOUND_MOB_WOLF_GROWL, "mob.wolf.growl")                                  \
	X(SOUND_MOB_WOLF_WHINE, "mob.wolf.whine")                                  \
	X(SOUND_MOB_WOLF_PANTING, "mob.wolf.panting")                              \
	X(SOUND_MOB_WOLF_HURT, "mob.wolf.hurt")                                    \
	X(SOUND_MOB_WOLF_DEATH, "mob.wolf.death")                                  \
	X(SOUND_MOB_WOLF_SHAKE, "mob.wolf.shake")                                  \
	/* ===== S2 event set (decompile-cited per key; see the wiring sites) ==== \
	   NOT here on purpose (b1.7.3 has NO such sounds -- the absent list):     \
	   liquid.lava / liquid.lavapop (lava is SILENT [740c583: BlockFluid.      \
	   randomDisplayTick -- visual "lava" particle only]), damage.fallsmall /  \
	   damage.fallbig (landing = block step sound at vol*0.5 [EntityLiving.    \
	   fall]), random.bowhit (arrow hits play random.drr). */                  \
	X(SOUND_RANDOM_SPLASH, "random.splash") /* water entry [Entity:205-232] */ \
	X(SOUND_RANDOM_FIZZ, "random.fizz") /* lava-mix harden + nether bucket     \
										   evap [BlockFluid.checkForHarden;   \
										   ItemBucket.onItemRightClick] */    \
	X(SOUND_RANDOM_DRR, "random.drr") /* arrow entity/block hit -- NOT         \
										 "random.bowhit", which does not      \
										 exist in b1.7.3 [EntityArrow] */     \
	X(SOUND_FIRE_FIRE, "fire.fire") /* crackle [BlockFire.randomDisplayTick] */\
	X(SOUND_FIRE_IGNITE, "fire.ignite") /* flint&steel [ItemFlintAndSteel] */  \
	X(SOUND_LIQUID_WATER, "liquid.water") /* FLOWING-water burble, meta 1..7   \
											 only [BlockFluid.randomDisplay-  \
											 Tick] */                         \
	X(SOUND_PORTAL_TRIGGER, "portal.trigger") /* ramp start, playSoundFX       \
												 [EntityPlayerSP:39-62] */    \
	X(SOUND_PORTAL_TRAVEL, "portal.travel") /* dwell latch, playSoundFX */     \
	X(SOUND_PORTAL_PORTAL, "portal.portal") /* ambient [BlockPortal:157-161] */\
	X(SOUND_TILE_PISTON_OUT, "tile.piston.out") /* extend [BlockPistonBase     \
												   .playBlock ev 0] */        \
	X(SOUND_TILE_PISTON_IN, "tile.piston.in") /* retract, unconditional at the \
												 retract branch [playBlock    \
												 ev 1] */                     \
	X(SOUND_MOB_SLIME, "mob.slime") /* hurt/death 0.6F + size-gated jump (>1)  \
									   and land (>2) [EntitySlime] */         \
	X(SOUND_MOB_SLIMEATTACK, "mob.slimeattack") /* contact hit accepted --     \
												   IS in b1.7.3 [EntitySlime  \
												   .onCollideWithPlayer] */   \
	X(SOUND_AMBIENT_CAVE, "ambient.cave.cave") /* mood counter, client-side    \
												  [World.tick:1901-1917] */   \
	/* Noteblock-slice: BlockNote.playBlock's 5-instrument key set             \
	   [740c583], "note." + {harp,bd,snare,hat,bassattack} -- instrument 0    \
	   (default/else) through 4. Vol 3.0F, pitch = 2^((note-12)/12)          \
	   UNCLAMPED (server-rolled, see server_local.h's noteblock trigger). */ \
	X(SOUND_NOTE_HARP, "note.harp")                                            \
	X(SOUND_NOTE_BD, "note.bd")                                                \
	X(SOUND_NOTE_SNARE, "note.snare")                                          \
	X(SOUND_NOTE_HAT, "note.hat")                                              \
	X(SOUND_NOTE_BASSATTACK, "note.bassattack")                                 \
	/* Project-original softer samples, GPL-3.0-or-later (not CC0). */         \
	/* assets/sound/menu/README.md; select = approved craft-confirm.wav. */   \
	X(SOUND_MENU_SCROLL, "menu.scroll")                                       \
	X(SOUND_MENU_SECTION, "menu.category")                                    \
	X(SOUND_MENU_SELECT, "menu.select")                                       \
	X(SOUND_MENU_BACK, "menu.back")                                           \
	X(SOUND_MENU_ITEM, "menu.item")                                           \
	X(SOUND_MENU_TYPE, "menu.type")                                           \
	X(SOUND_MENU_CONNECTED, "menu.connected")                                 \
	X(SOUND_MENU_ATTENTION, "menu.attention")                                  \
	X(SOUND_MENU_ACHIEVEMENT, "menu.achievement")

/* Registry ids, derived FROM the table (index == table row, always). */
enum audio_sound_id {
#define AUDIO_SOUND_TABLE_X(id, name) id,
	AUDIO_SOUND_TABLE(AUDIO_SOUND_TABLE_X)
#undef AUDIO_SOUND_TABLE_X
		AUDIO_SOUND_COUNT
};

/* Variant cap: the engine scans the bare base name plus trailing digits
   1..13 (two-digit suffixes included -- the "%d" scan is digit-count
   agnostic). 14 because Beta's LARGEST real variant set is the cave mood
   pool cave1..cave13 [740c583: resources; SoundPool.addSound's digit-strip
   maps ALL of them onto the one "ambient.cave.cave" key] -- the old cap of
   10 silently dropped cave10..13 from a real pack. The pack builder MUST
   stay inside this namespace (base, base1..base13). */
#define AUDIO_MAX_VARIANTS 14

/* "step.grass" -> "sound/step/grass": the SoundPool slash-to-dot rule run in
   reverse, prefixed with the registry's scan root. Returns the (untruncated)
   length, snprintf-style. Shared so the engine scan, the tool's --manifest
   and the tests all derive one identical path per key. */
static inline int audio_sound_table_subpath(const char* key, char* out,
											size_t out_size) {
	int n = snprintf(out, out_size, "sound/%s", key);
	for(int k = 0; k < n && (size_t)k + 1 < out_size; k++)
		if(out[k] == '.')
			out[k] = '/';
	return n;
}

#endif
