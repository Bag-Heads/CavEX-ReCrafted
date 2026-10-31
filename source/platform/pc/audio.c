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

/*
	PC audio backend: miniaudio LOW-LEVEL ma_device + a manual mixer that
	replicates the Wii voice model exactly (fixed slots, per-voice cursor at
	native_rate*pitch, L/R gains from the shared audio_mix.h math). NO
	ma_engine / node graph — deliberately compiled out (MA_NO_ENGINE etc.,
	third_party/miniaudio/PATCHES.md) so the two platforms cannot drift
	behaviorally. #included by platform/audio.c (the gfx.c pattern);
	MA_IMPLEMENTATION lives in THIS translation unit only.

	Threading: the engine half (voice_start/gain/playing) runs on the main
	thread; the miniaudio data callback runs on the device thread. The
	voice array is guarded by one mutex held for microseconds per 2-4ms
	period — uncontended in practice.
*/

#include <pthread.h>

#define MA_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <miniaudio/miniaudio.h>
#pragma GCC diagnostic pop

#define PC_AUDIO_OUT_RATE 48000 /* matches the Wii DSP 48kHz output rate */

struct pc_voice {
	bool playing;
	const int16_t* pcm;
	uint32_t frames;
	double cursor; /* fractional source-frame position */
	double step;   /* source frames per output frame = rate_hz / out_rate */
	float l, r;
};

static struct pc_voice s_pcv[AUDIO_VOICES];
static pthread_mutex_t s_pc_lock = PTHREAD_MUTEX_INITIALIZER;
static ma_device s_pc_dev;
static bool s_pc_dev_open = false;

/* S3: the ONE dedicated streaming-music voice, mixed into the SAME device
   callback under the SAME lock -- reusing struct pc_voice rather than
   opening a second ma_device keeps sound-fx and music on one physical
   stream (matches the Wii backend's one-AESND_Init rule) and needs no new
   mixing code, just one more entry in the loop below. */
static struct pc_voice s_pc_music;

static void pc_audio_callback(ma_device* dev, void* out, const void* in,
							  ma_uint32 frame_count) {
	(void)dev;
	(void)in;
	float* dst = out;
	memset(dst, 0, sizeof(float) * 2 * frame_count);

	pthread_mutex_lock(&s_pc_lock);
	for(int k = 0; k < AUDIO_VOICES; k++) {
		struct pc_voice* v = &s_pcv[k];
		if(!v->playing)
			continue;
		for(ma_uint32 f = 0; f < frame_count; f++) {
			if(v->cursor >= (double)v->frames - 1.0) {
				v->playing = false;
				break;
			}
			/* Linear-interp resample at the unclamped Beta pitch rate. */
			uint32_t i0 = (uint32_t)v->cursor;
			float frac = (float)(v->cursor - (double)i0);
			float s0 = (float)v->pcm[i0];
			float s1 = (float)v->pcm[i0 + 1];
			float smp = (s0 + (s1 - s0) * frac) * (1.0F / 32768.0F);
			dst[2 * f] += smp * v->l;
			dst[2 * f + 1] += smp * v->r;
			v->cursor += v->step;
		}
	}
	{
		struct pc_voice* v = &s_pc_music;
		if(v->playing) {
			for(ma_uint32 f = 0; f < frame_count; f++) {
				if(v->cursor >= (double)v->frames - 1.0) {
					v->playing = false; /* audio_music_update's poll flips
										   audio_backend_music_playing() false
										   the instant it observes this */
					break;
				}
				uint32_t i0 = (uint32_t)v->cursor;
				float frac = (float)(v->cursor - (double)i0);
				float s0 = (float)v->pcm[i0];
				float s1 = (float)v->pcm[i0 + 1];
				float smp = (s0 + (s1 - s0) * frac) * (1.0F / 32768.0F);
				dst[2 * f] += smp * v->l;
				dst[2 * f + 1] += smp * v->r;
				v->cursor += v->step;
			}
		}
	}
	pthread_mutex_unlock(&s_pc_lock);

	/* Hard clip the mix bus — the voice gains are each <= 1, but multiple
	   simultaneous one-shots can sum past full scale. */
	for(ma_uint32 f = 0; f < 2 * frame_count; f++) {
		if(dst[f] > 1.0F)
			dst[f] = 1.0F;
		else if(dst[f] < -1.0F)
			dst[f] = -1.0F;
	}
}

static bool audio_backend_init(void) {
	ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
	cfg.playback.format = ma_format_f32;
	cfg.playback.channels = 2;
	cfg.sampleRate = PC_AUDIO_OUT_RATE;
	cfg.dataCallback = pc_audio_callback;

	if(ma_device_init(NULL, &cfg, &s_pc_dev) != MA_SUCCESS)
		return false;
	if(ma_device_start(&s_pc_dev) != MA_SUCCESS) {
		ma_device_uninit(&s_pc_dev);
		return false;
	}
	s_pc_dev_open = true;
	return true;
}

static void audio_backend_shutdown(void) {
	if(!s_pc_dev_open)
		return;
	ma_device_uninit(&s_pc_dev); /* stops + joins the device thread */
	s_pc_dev_open = false;
	memset(s_pcv, 0, sizeof(s_pcv));
	memset(&s_pc_music, 0, sizeof(s_pc_music));
}

static void audio_backend_voice_start(int slot, const struct audio_sample* smp,
									  float rate_hz, float l, float r) {
	/* NaN-rejecting guard form (review F4): (rate <= 0) is FALSE for NaN; a
	 * NaN step makes the callback's float->u32 cursor cast UB (x86 yields
	 * 0x80000000 -> a wild pcm[] read) and the voice never terminates. */
	if(!s_pc_dev_open || !(rate_hz > 0.0F))
		return;
	/* The device may have negotiated a different native rate; miniaudio
	   converts from cfg.sampleRate, so out_rate is always our request. */
	pthread_mutex_lock(&s_pc_lock);
	s_pcv[slot].pcm = smp->pcm;
	s_pcv[slot].frames = smp->frames;
	s_pcv[slot].cursor = 0.0;
	s_pcv[slot].step = (double)rate_hz / (double)PC_AUDIO_OUT_RATE;
	s_pcv[slot].l = l;
	s_pcv[slot].r = r;
	s_pcv[slot].playing = true;
	pthread_mutex_unlock(&s_pc_lock);
}

static void audio_backend_voice_gain(int slot, float l, float r) {
	pthread_mutex_lock(&s_pc_lock);
	s_pcv[slot].l = l;
	s_pcv[slot].r = r;
	pthread_mutex_unlock(&s_pc_lock);
}

static bool audio_backend_voice_playing(int slot) {
	pthread_mutex_lock(&s_pc_lock);
	bool p = s_pcv[slot].playing;
	pthread_mutex_unlock(&s_pc_lock);
	return p;
}

static void audio_backend_stop_all(void) {
	/* S1 reload drain: zero EVERY voice under the mixer lock. The device
	   callback holds the same lock for its whole mix pass, so after this
	   unlocks it can never read a voice's (about-to-be-rewritten) arena
	   pointer again. */
	pthread_mutex_lock(&s_pc_lock);
	memset(s_pcv, 0, sizeof(s_pcv));
	pthread_mutex_unlock(&s_pc_lock);
}

/* ===== S3 streaming-music voice ========================================
   Same struct/lock as the one-shot pool, mixed by the SAME callback above.
   Unlike Wii's poll-and-restart model, this is genuinely gapless: the
   real-time device callback reads `s_pc_music` under the mixer lock on
   every period, so handing it a fresh chunk any time before the current
   one's cursor reaches its end never produces a silent gap -- there is no
   "deadline poll slop" here the way there is on the AESND path. */
static void audio_backend_music_start(const int16_t* pcm, uint32_t frames,
									  float rate_hz, float gain) {
	if(!s_pc_dev_open || !(rate_hz > 0.0F) || frames == 0 || !pcm)
		return;
	pthread_mutex_lock(&s_pc_lock);
	s_pc_music.pcm = pcm;
	s_pc_music.frames = frames;
	s_pc_music.cursor = 0.0;
	s_pc_music.step = (double)rate_hz / (double)PC_AUDIO_OUT_RATE;
	s_pc_music.l = gain;
	s_pc_music.r = gain;
	s_pc_music.playing = true;
	pthread_mutex_unlock(&s_pc_lock);
}

static void audio_backend_music_gain(float gain) {
	pthread_mutex_lock(&s_pc_lock);
	s_pc_music.l = gain;
	s_pc_music.r = gain;
	pthread_mutex_unlock(&s_pc_lock);
}

static bool audio_backend_music_playing(void) {
	pthread_mutex_lock(&s_pc_lock);
	bool p = s_pc_music.playing;
	pthread_mutex_unlock(&s_pc_lock);
	return p;
}

/* On PC `playing` IS the mixer's read cursor state -- the callback flips it
   false only once it has consumed the last frame of the chunk it was given.
   So "the device has played everything handed over" is exactly "not
   playing"; there is no queued tail behind the poll the way the Wii's
   looped ring has one. */
static bool audio_backend_music_drained(void) {
	return !audio_backend_music_playing();
}

static uint32_t audio_backend_music_reserve_us(void) {
	pthread_mutex_lock(&s_pc_lock);
	double frames = s_pc_music.playing ? s_pc_music.frames-s_pc_music.cursor : 0;
	double rate = s_pc_music.step * PC_AUDIO_OUT_RATE;
	double us = rate > 0 && frames > 0 ? frames*1000000.0/rate : 0;
	pthread_mutex_unlock(&s_pc_lock);
	return us >= UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static void audio_backend_music_stop(void) {
	pthread_mutex_lock(&s_pc_lock);
	s_pc_music.playing = false;
	pthread_mutex_unlock(&s_pc_lock);
}

/* Pacing-bug probe (2026-09-01): PC's mixer has no chunk deadline -- the
 * probe is a Wii-only instrument; 0 here keeps the shared accessor linking. */
static uint32_t audio_backend_music_last_dur_ms(void) { return 0; }

/* 0 = "keep the file's native rate". miniaudio's device pipeline resamples
 * properly (and this backend's own mix cursor steps at a double-precision
 * ratio), so pre-resampling on the decode worker would only add a second
 * conversion. The Wii backend is the one that needs it -- AESND's mixer
 * does NEAREST-NEIGHBOUR only. */
static uint32_t audio_backend_music_native_rate(void) { return 0; }
