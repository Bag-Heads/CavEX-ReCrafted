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

#include "client_display_tick.h"

#include "../block/blocks.h"
#include "../particle.h"
#include "../platform/audio.h" /* S2: client-local ambience sounds */
#include "../platform/audio_music.h" /* S3: the background-music timer */
#include "../profile.h"
#include "../util.h"
#include "game_state.h"

void torch_display_pos(uint8_t meta, w_coord_t x, w_coord_t y, w_coord_t z,
					   vec3 out) {
	// Beta randomDisplayTick constants: 0.27 along the facing axis, +0.22 up.
	float fx = 0.0F, fz = 0.0F, fy = 0.0F;
	switch(meta) {
		case 1: fx = 0.27F; fy = 0.22F; break;
		case 2: fx = -0.27F; fy = 0.22F; break;
		case 3: fz = 0.27F; fy = 0.22F; break;
		case 4: fz = -0.27F; fy = 0.22F; break;
		default: break; // 5 (or anything else): standing torch, no shift
	}
	out[0] = (float)x + 0.5F + fx;
	out[1] = (float)y + 0.7F + fy;
	out[2] = (float)z + 0.5F + fz;
}

// Samples per client tick. Beta uses 1000 over the +-15 cube; the Wii's
// single core pays a world_get_block per sample (a cache-hostile random
// walk), so the low-profile build tunes the COUNT down -- a documented
// playability carve-out: fewer flame spawns per second, never a sim change
// (display ticks are cosmetic by definition). S2: the randomDisplayTick
// SOUND half (fire.fire / liquid.water / portal.portal below) rides the
// SAME sample loop, so the Wii-low profile scales ambience density by the
// same 1/4 -- the documented T2 companion to the particle cut (sparser
// crackle/burble, identical per-sample odds; docs/deviations.md).
#ifdef FCAVEX_PROFILE_WII_LOW
#define DISPLAY_TICK_SAMPLES 250
#else
#define DISPLAY_TICK_SAMPLES 1000
#endif

// ===== S2 CAVE MOOD (Beta World.tick's ambiance half, client-side) =========
// Beta: a world-global soundCounter decrements each tick; at 0, EVERY active
// chunk per tick rolls one uniform cell (16x128x16 via the chunk LCG) and the
// sound fires when the cell is AIR, raw skylight == 0, effective light <=
// rand(8), and the closest player is within 8 blocks but distSq > 4 of it;
// then soundCounter = rand(12000)+6000 (5-15 min). Beta SMP never delivered
// this to clients (no packet) -- the client-side port is exactly how SP
// players actually heard it. RATE-EXACT port: only the 3x3 chunks around the
// player can ever pass the 8-block gate, so sampling exactly those 9 per tick
// (uniform cell each, distance-gated) reproduces Beta's fire probability;
// the per-chunk LCG is replaced by the sanctioned client RNG (the LCG is not
// gameplay-visible -- scout/spec agreement). [740c583: World.tick:1901-1917]
static int s_cave_sound_counter = -1; // -1 = not yet seeded

static void cave_mood_tick(w_coord_t px, w_coord_t pz) {
	if(s_cave_sound_counter < 0)
		s_cave_sound_counter
			= rand_gen_int(&gstate.rand_src, 12000) + 6000;
	if(s_cave_sound_counter > 0) {
		s_cave_sound_counter--;
		return;
	}
	w_coord_t pcx = WCOORD_CHUNK_OFFSET(px), pcz = WCOORD_CHUNK_OFFSET(pz);
	for(int c = 0; c < 9 && s_cave_sound_counter == 0; c++) {
		w_coord_t cx = pcx + (c % 3) - 1, cz = pcz + (c / 3) - 1;
		w_coord_t x = cx * 16 + rand_gen_int(&gstate.rand_src, 16);
		w_coord_t y = rand_gen_int(&gstate.rand_src, 128);
		w_coord_t z = cz * 16 + rand_gen_int(&gstate.rand_src, 16);
		float dx = (float)x + 0.5F - (float)gstate.players[0].camera.x;
		float dy = (float)y + 0.5F - (float)gstate.players[0].camera.y;
		float dz = (float)z + 0.5F - (float)gstate.players[0].camera.z;
		float d2 = dx * dx + dy * dy + dz * dz;
		if(d2 > 64.0F || d2 <= 4.0F)
			continue; // Beta: within 8 blocks of the player but > 2 away
		struct block_data blk = world_get_block(world_view_current(), x, y, z);
		if(blk.type != BLOCK_AIR || blk.sky_light != 0)
			continue;
		/* getFullBlockLightValue <= rand(8): skylight is 0 here, so the
		   effective (subtracted) value collapses to the torch light. */
		if((int)blk.torch_light > rand_gen_int(&gstate.rand_src, 8))
			continue;
		audio_play3d(SOUND_AMBIENT_CAVE,
					 (vec3) {(float)x + 0.5F, (float)y + 0.5F,
							 (float)z + 0.5F},
					 0.7F, 0.8F + rand_gen_flt(&gstate.rand_src) * 0.2F);
		s_cave_sound_counter
			= rand_gen_int(&gstate.rand_src, 12000) + 6000;
	}
}

// C2 P2 HARDWARE-EVIDENCE LEVER (Astra ruling 3 D4). Config
// "debug.cave_sound_every_s" (absent/0 = off, the default; NEVER set in a
// release or play config). When > 0 it plays ambient.cave.cave every N seconds
// 3 blocks from the camera, walking the variants IN ORDER (so a run of
// 13*N seconds provably exercises all 13) and alternating the slowest Beta
// pitch 0.8 with 1.0. It draws NO random number and does not touch the real
// mood counter above, so the faithful trigger keeps its own cadence and RNG
// stream; the lever only adds plays. Read once per world load.
static int s_cave_lever_period = -1; // ticks; -1 = config not read yet
static int s_cave_lever_left;
static int s_cave_lever_n;

static void cave_debug_lever_tick(void) {
	if(s_cave_lever_period < 0) {
		int every_s = config_read_int(&gstate.config_user,
									  "debug.cave_sound_every_s", 0);
		s_cave_lever_period = every_s > 0 ? every_s * 20 : 0;
		s_cave_lever_left = s_cave_lever_period;
	}
	if(s_cave_lever_period == 0 || --s_cave_lever_left > 0)
		return;
	s_cave_lever_left = s_cave_lever_period;
	audio_debug_play_variant(SOUND_AMBIENT_CAVE, s_cave_lever_n,
							 (vec3) {gstate.players[0].camera.x + 3.0F,
									 gstate.players[0].camera.y,
									 gstate.players[0].camera.z},
							 0.7F, (s_cave_lever_n & 1) ? 1.0F : 0.8F);
	s_cave_lever_n++;
}

void client_display_tick(void) {
	if(!gstate.world_loaded) {
		s_cave_lever_period = -1; // re-read the lever at the next world load
		// S2 review fix: Beta rolls the ambience countdown per World
		// CONSTRUCTION, so leaving a world (menu, dimension transit -- both
		// flap world_loaded) must re-seed it. Without this a quit-to-menu
		// mid-cooldown carried up to 15 minutes of silence into the next
		// world. [740c583: World.<init> ambientTickCountdown]
		s_cave_sound_counter = -1;
		// C2 P2 (opt-in deferred cave ambience): a cave sound still LOADING
		// when the world goes away must not start in the menu or in the next
		// world. One static bool when the opt-in is off.
		audio_cancel_deferred();
		return;
	}

	w_coord_t px = (w_coord_t)gstate.players[0].camera.x;
	w_coord_t py = (w_coord_t)gstate.players[0].camera.y;
	w_coord_t pz = (w_coord_t)gstate.players[0].camera.z;

	// S2: the Beta cave-mood counter, ticked at the same 20Hz cadence as
	// World.tick ran it (this function is the !paused client tick).
	cave_mood_tick(px, pz);
	cave_debug_lever_tick(); // off unless "debug.cave_sound_every_s" > 0
	// S3: SoundManager.playRandomMusicIfReady's delay/pick timer, ticked at
	// the SAME cadence and gate as Beta's own call site
	// [PlayerControllerSP.updateController, called from Minecraft.runTick's
	// `!isGamePaused && theWorld != null` -- exactly this function's own
	// gate, world_loaded above + the caller's !paused]. Unlike the cave-mood
	// counter (a World field, reseeded on world load/unload above),
	// audio_music.c's ticksBeforeMusic is a SoundManager field and is
	// deliberately NOT reseeded here -- see audio_music.h.
	audio_music_tick();

	for(int k = 0; k < DISPLAY_TICK_SAMPLES; k++) {
		// Beta's offset distribution is nextInt(16) - nextInt(16) per axis:
		// triangular over [-15, 15], denser near the player. Two float draws
		// reproduce the shape (client RNG, client thread -- the legitimate
		// home of gstate.rand_src).
		w_coord_t x = px + (w_coord_t)(rand_gen_flt(&gstate.rand_src) * 16.0F)
			- (w_coord_t)(rand_gen_flt(&gstate.rand_src) * 16.0F);
		w_coord_t y = py + (w_coord_t)(rand_gen_flt(&gstate.rand_src) * 16.0F)
			- (w_coord_t)(rand_gen_flt(&gstate.rand_src) * 16.0F);
		w_coord_t z = pz + (w_coord_t)(rand_gen_flt(&gstate.rand_src) * 16.0F)
			- (w_coord_t)(rand_gen_flt(&gstate.rand_src) * 16.0F);

		struct block_data blk = world_get_block(world_view_current(), x, y, z);

		// POWERED redstone wire dust (GX-wedge audit fix 2026-06-11: this used
		// to be spawned from the SERVER world tick in block_redstone_wire.c --
		// an unlocked cross-thread write into the render thread's particle
		// array, the exact race class the torch fix removed. Beta emits wire
		// dust from randomDisplayTick, i.e. exactly THIS client-side sampler.)
		if(blk.type == BLOCK_REDSTONE_WIRE) {
			if(blk.metadata & 0x0F)
				particle_generate_redstone_wire(
					(vec3) {(float)x + 0.5f, (float)y, (float)z + 0.5f},
					blk.metadata & 0x0F);
			continue;
		}

		// ===== S2: the randomDisplayTick SOUND half (client-local, zero
		// CRPC -- exactly where Beta plays these; the wire-dust precedent
		// above). Per-sample odds are Beta's own; the vol>1 fire crackle
		// extends RANGE not loudness (audio_mix clamps gain at 1). =====
		// FIRE crackle: rand(24)==0 -> "fire.fire" 1.0F+r() (range 16-32),
		// pitch r()*0.7F+0.3F, at the cell center.
		// [740c583: BlockFire.randomDisplayTick:201-204]
		if(blk.type == BLOCK_FIRE) {
			if(rand_gen_int(&gstate.rand_src, 24) == 0)
				audio_play3d(SOUND_FIRE_FIRE,
							 (vec3) {(float)x + 0.5F, (float)y + 0.5F,
									 (float)z + 0.5F},
							 1.0F + rand_gen_flt(&gstate.rand_src),
							 rand_gen_flt(&gstate.rand_src) * 0.7F + 0.3F);
			continue;
		}
		// WATER burble: material==water && rand(64)==0 && metadata 1..7 --
		// FLOWING cells only, never source (0) or falling (>=8); still-water
		// lakes are all meta 0, which is why people misremember Beta water
		// as silent. "liquid.water" r()*0.25F+0.75F, r()+0.5F.
		// [740c583: BlockFluid.randomDisplayTick:210-225]
		if(blk.type == BLOCK_WATER_FLOW || blk.type == BLOCK_WATER_STILL) {
			int meta = blk.metadata & 0x0F;
			if(rand_gen_int(&gstate.rand_src, 64) == 0 && meta > 0
			   && meta < 8)
				audio_play3d(SOUND_LIQUID_WATER,
							 (vec3) {(float)x + 0.5F, (float)y + 0.5F,
									 (float)z + 0.5F},
							 rand_gen_flt(&gstate.rand_src) * 0.25F + 0.75F,
							 rand_gen_flt(&gstate.rand_src) + 0.5F);
			continue;
		}
		// NB: lava has NO ambient sound in b1.7.3 (BlockFluid's water gate
		// above is material==water; lava spawns only the visual "lava"
		// particle) -- do not "fix" the silence (docs/deviations.md).
		// PORTAL ambient: rand(100)==0 -> "portal.portal" 1.0F,
		// r()*0.4F+0.8F, positional at the block.
		// [740c583: BlockPortal.randomDisplayTick:157-161]
		if(blk.type == BLOCK_PORTAL) {
			if(rand_gen_int(&gstate.rand_src, 100) == 0)
				audio_play3d(SOUND_PORTAL_PORTAL,
							 (vec3) {(float)x + 0.5F, (float)y + 0.5F,
									 (float)z + 0.5F},
							 1.0F,
							 rand_gen_flt(&gstate.rand_src) * 0.4F + 0.8F);
			continue;
		}

		if(blk.type != BLOCK_TORCH && blk.type != BLOCK_REDSTONE_TORCH
		   && blk.type != BLOCK_REDSTONE_TORCH_LIT)
			continue;

		vec3 pos;
		torch_display_pos(blk.metadata, x, y, z, pos);

		if(blk.type == BLOCK_TORCH)
			particle_generate_torch(pos);
		else
			particle_generate_redstone_torch(pos);
	}
}
