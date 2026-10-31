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

#ifndef PLATFORM_AUDIO_MIX_H
#define PLATFORM_AUDIO_MIX_H

/*
	S0 playback math — Beta 1.7.3 rules, pure and header-only so both
	backends share one implementation and test_audio_env pins the exact
	production formulas (the combat.h precedent). Decompile 740c583 is the
	spec for every constant.
*/

#include <math.h>

/* Audible range: 16 blocks, LINEARLY extended by volume above 1.0 — volume
   over 1 is a RANGE multiplier, never a gain (explosion vol 4.0F = 64
   blocks). [740c583: SoundManager.playSound "rolloff = 16, if vol > 1
   rolloff *= vol"; RenderGlobal.playSound culls at the same 16*max(1,vol)] */
static inline float audio_range(float vol) {
	return 16.0F * (vol > 1.0F ? vol : 1.0F);
}

/* Linear fade to zero at the range — paulscode attenuation MODEL 2 (linear),
   not inverse-square. [740c583: SoundManager.playSound attModel 2] */
static inline float audio_distance_gain(float dist, float vol) {
	float range = audio_range(vol);
	float g = 1.0F - dist / range;
	return g < 0.0F ? 0.0F : (g > 1.0F ? 1.0F : g);
}

/* Playback loudness: volume clamped to 1.0 THEN scaled by the master option.
   [740c583: SoundManager.playSound "if(volume > 1) volume = 1; volume *=
   options.soundVolume"] */
static inline float audio_loudness(float vol, float master) {
	if(vol > 1.0F)
		vol = 1.0F;
	if(vol < 0.0F)
		vol = 0.0F;
	return vol * master;
}

/* The 2D/UI FX path: clamp to 1, then QUARTER volume, then master — playing
   random.click through the world path would be 4x too loud.
   [740c583: SoundManager.playSoundFX "volume *= 0.25F"] */
static inline float audio_ui_loudness(float vol, float master) {
	return audio_loudness(vol, master) * 0.25F;
}

/* Pitch is UNCLAMPED: playback rate = native rate * pitch. Call-site
   formulas legitimately exceed 2.0 (fizz ~2.6-3.4, pop up to ~3.4) — the
   0.5..2.0 clamp is from later versions, not b1.7.3.
   [740c583: SoundManager.playSound sets source pitch with no clamp] */
static inline float audio_playback_rate(float native_rate, float pitch) {
	return native_rate * pitch;
}

/* Camera-relative pan in [-1, 1] from the horizontal offset to the sound and
   the camera azimuth (engine convention: forward = (sin yaw, cos yaw), the
   camera_ray_pick basis in main.c). 0 = centered (also for a sound at the
   listener). Beta ran a real 3D panner; this azimuth sin is the documented
   mono+pan T2 stand-in (docs/deviations.md). */
static inline float audio_pan(float dx, float dz, float cam_yaw_rad) {
	if(dx * dx + dz * dz < 1.0e-6F)
		return 0.0F;
	/* Handedness (adversarial review F2): forward = (sin rx, ., cos rx) and
	 * cglm's RIGHT-handed lookat makes screen-right s = f x up =
	 * (-cos rx, 0, sin rx), so a sound's screen-rightness is
	 * d.s = -sin(atan2(dx,dz) - rx). The unnegated form panned every sound
	 * to the WRONG side; pinned by the facing-+Z/-X-source test assertion. */
	return sinf(cam_yaw_rad - atan2f(dx, dz));
}

/* Split one gain into L/R by pan. SUBTLE by design: the far ear is dimmed by
   at most AUDIO_PAN_DEPTH, the near ear stays at the full mono gain, so the
   mix never gets louder than the faithful mono loudness. */
#define AUDIO_PAN_DEPTH 0.5F
static inline void audio_pan_gains(float pan, float gain, float* l, float* r) {
	if(pan > 1.0F)
		pan = 1.0F;
	if(pan < -1.0F)
		pan = -1.0F;
	*l = gain * (pan > 0.0F ? 1.0F - AUDIO_PAN_DEPTH * pan : 1.0F);
	*r = gain * (pan < 0.0F ? 1.0F + AUDIO_PAN_DEPTH * pan : 1.0F);
}

#endif
