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
	C2 P2: the deferred one-shot loader. Contract, threading rules and the
	reason this is NOT a second streamed voice: audio_deferred.h.

	OWNERSHIP, the whole safety argument in one place
	  s_lock guards every s_* below and s_load's handles.
	  The scratch is written ONLY inside a loader step, and a step only runs
	  while the state is LOADING with s_in_step set under the lock.
	  s_load (the open file / decoder) is touched by the loader ONLY inside a
	  step, and by the main thread ONLY under the lock while !s_in_step -- so a
	  cancel between two steps may close it directly, and a cancel DURING a step
	  merely raises s_cancel_pending for the step's own epilogue to honour.
	  The lock is never held across fopen/fread/fseek or a decode call.
*/

#include <stdio.h>
#include <string.h>

#include "audio_deferred.h"
#include "thread.h"
#include "time.h"

#define WAVH_WITH_FILE_PROBE
#include "audio_wav.h"

#ifdef PLATFORM_WII
#include <ogc/cache.h> /* DCFlushRange: the DSP DMA reads the scratch */
#endif

/* The implementation lives in audio_registry.c (the resident decoder). */
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 2
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb/stb_vorbis.c>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define AUDIO_DEFERRED_THREAD_PRIO 40
#define AUDIO_DEFERRED_RECORDS 8
/* Frames per ogg decode burst: bounded decoder OUTPUT working memory (the
   static s_ogg_tmp below), whatever the stream's own block size. */
#define AUDIO_DEFERRED_OGG_FRAMES 4096

struct deferred_job {
	uint32_t req_id;
	uint32_t epoch;
	uint16_t sound_id;
	int variant;
	struct audio_deferred_source src;
	float pos[3];
	float vol;
	float pitch;
	int64_t t_request_ms;
	/* results */
	uint32_t frames;
	uint32_t rate;
	int32_t load_ms;
	int32_t start_delay_ms;
	bool started;
};

enum load_phase { LOAD_OPEN = 0, LOAD_READ };

/* Loader-side progress of the job in LOADING. See OWNERSHIP above. */
struct deferred_load {
	enum load_phase phase;
	FILE* wav;
	stb_vorbis* ogg;
	int channels;
	uint32_t total_frames; /* WAV: from the re-probe. OGG: unknown, 0 */
	uint32_t done_frames;
	uint32_t rate;
	int64_t t_start_ms;
};

static struct thread_mutex s_lock;
static struct thread_cond s_wake;	 /* loader sleeps here */
static struct thread_cond s_step_cv; /* quiesce waits for !s_in_step here */
static bool s_sync_ready;

static enum audio_deferred_state s_state = AUDIO_DEFERRED_UNAVAILABLE;
static int16_t* s_scratch;
static size_t s_capacity; /* bytes */
/* The ONLY memory an ogg load may use for stb_vorbis's setup/codebook/temp
   state (hostile review 06 #1): sized at scan from the largest ogg variant,
   carved from the arena beside the scratch. No block => an ogg request FAILS;
   the loader never mallocs decoder state during gameplay. */
static void* s_decoder_mem;
static size_t s_decoder_bytes;
static uint32_t s_epoch;
static uint32_t s_next_req = 1;
static struct deferred_job s_job;
static struct deferred_load s_load;
static bool s_in_step;
static bool s_cancel_pending;
static bool s_ready_taken;
static int s_slot = -1;
static int64_t s_release_ms;

#ifndef CAVEWII_TESTING /* tests drive the loader by hand: no thread */
static struct thread s_thread;
static bool s_thread_up;
static bool s_thread_stop;
#endif

static struct audio_deferred_counters s_counters;
static struct audio_deferred_record s_records[AUDIO_DEFERRED_RECORDS];
static unsigned s_rec_head, s_rec_count;

static int16_t s_ogg_tmp[AUDIO_DEFERRED_OGG_FRAMES * 2];

#ifdef CAVEWII_TESTING
static bool s_test_fail_thread;
static int64_t s_test_clock_ms = -1;
static void (*s_test_step_hook)(void);
#endif

/* ===== clock ============================================================ */

/* Written ONCE, by deferred_sync_init on the main thread, before the loader
   thread can exist -- never a lazily-initialised cross-thread static (hostile
   review 06 #9: a torn origin would let the drain margin be skipped). */
static ptime_t s_clock_origin;

static int64_t deferred_now_ms(void) {
#ifdef CAVEWII_TESTING
	if(s_test_clock_ms >= 0)
		return s_test_clock_ms;
#endif
	return (int64_t)time_diff_ms(s_clock_origin, time_get());
}

static int32_t deferred_span_ms(int64_t from, int64_t to) {
	int64_t d = to - from;
	if(d < 0)
		d = 0;
	if(d > INT32_MAX)
		d = INT32_MAX;
	return (int32_t)d;
}

/* ===== sync bring-up ==================================================== */

static bool deferred_sync_init(void) {
	if(s_sync_ready)
		return true;
	if(!thread_mutex_init(&s_lock))
		return false;
	if(!thread_cond_init(&s_wake)) {
		thread_mutex_destroy(&s_lock);
		return false;
	}
	if(!thread_cond_init(&s_step_cv)) {
		thread_cond_destroy(&s_wake);
		thread_mutex_destroy(&s_lock);
		return false;
	}
	s_clock_origin = time_get();
	s_sync_ready = true;
	return true;
}

/* ===== records (s_lock held) ============================================ */

static void deferred_push_record(const struct audio_deferred_record* r) {
	unsigned at = (s_rec_head + s_rec_count) % AUDIO_DEFERRED_RECORDS;
	if(s_rec_count == AUDIO_DEFERRED_RECORDS)
		s_rec_head = (s_rec_head + 1) % AUDIO_DEFERRED_RECORDS; /* drop oldest */
	else
		s_rec_count++;
	s_records[at] = *r;
	if(r->outcome > AUDIO_DEFERRED_OUT_NONE
	   && r->outcome < AUDIO_DEFERRED_OUT_COUNT)
		s_counters.outcome[r->outcome]++;
}

/* The ONE place an accepted request reaches its terminal outcome. */
static void deferred_finalize(enum audio_deferred_outcome why) {
	struct audio_deferred_record r;
	memset(&r, 0, sizeof(r));
	r.req_id = s_job.req_id;
	r.epoch = s_job.epoch;
	r.sound_id = s_job.sound_id;
	r.variant = s_job.variant;
	r.frames = s_job.frames;
	r.rate = s_job.rate;
	r.bytes = s_job.frames * 2u; /* frames <= capacity / 2: cannot wrap */
	r.load_ms = s_job.load_ms;
	r.start_delay_ms = s_job.start_delay_ms;
	r.started = s_job.started;
	r.outcome = why;
	deferred_push_record(&r);
	if(s_job.load_ms > s_counters.max_load_ms)
		s_counters.max_load_ms = s_job.load_ms;
}

/* A request that never became the job in flight (BUSY / FAILED at the door). */
static void deferred_refuse(uint16_t sound_id, int variant,
							enum audio_deferred_outcome why) {
	struct audio_deferred_record r;
	memset(&r, 0, sizeof(r));
	r.req_id = s_next_req++;
	r.epoch = s_epoch;
	r.sound_id = sound_id;
	r.variant = variant;
	r.load_ms = -1;
	r.start_delay_ms = -1;
	r.outcome = why;
	deferred_push_record(&r);
}

/* Close whatever the loader has open. s_lock held AND !s_in_step, or called
   from inside the step that owns it. */
static void deferred_close_load(void) {
	if(s_load.wav)
		fclose(s_load.wav);
	if(s_load.ogg)
		stb_vorbis_close(s_load.ogg);
	memset(&s_load, 0, sizeof(s_load));
}

static void deferred_advance_drain(void) {
	if(s_state == AUDIO_DEFERRED_DRAINING
	   && deferred_now_ms() - s_release_ms >= AUDIO_DEFERRED_DRAIN_MS)
		s_state = AUDIO_DEFERRED_IDLE;
}

/* ===== loader =========================================================== */

enum step_result { STEP_MORE = 0, STEP_DONE, STEP_FAILED };

/* One bounded unit of work on s_load / the scratch. NO lock held. `src`,
   `scratch` and `capacity` are the step's private copies taken under it. */
static enum step_result deferred_step_work(const struct audio_deferred_source*
											   src,
										   int16_t* scratch, size_t capacity,
										   void* decoder_mem,
										   size_t decoder_bytes) {
	uint32_t cap_frames = (uint32_t)(capacity / 2u);

	if(s_load.phase == LOAD_OPEN) {
		if(!src->is_ogg) {
			s_load.wav = fopen(src->path, "rb");
			uint32_t off = 0, frames = 0, rate = 0;
			if(!s_load.wav || !wavh_probe_file(s_load.wav, &off, &frames, &rate))
				return STEP_FAILED;
			/* The file may have changed since the scan-time probe: the
			   guard is the CURRENT header against the scratch, in frames
			   (no byte multiply that could wrap on a 32-bit size_t). */
			if(frames > cap_frames
			   || frames > AUDIO_DEFERRED_MAX_BYTES / 2u
			   || fseek(s_load.wav, (long)off, SEEK_SET) != 0)
				return STEP_FAILED;
			s_load.total_frames = frames;
			s_load.rate = rate;
		} else {
			/* Bounded decoder state: stb_vorbis carves its setup, codebook
			   and temp memory out of THIS block and nothing else (with an
			   alloc buffer it never calls malloc). A block that is absent
			   or too small for the file as it is NOW fails the open --
			   VORBIS_outofmem -- and the request ends FAILED. The open is
			   one step: the pull API offers no way to split it, so its
			   cost is exactly what max_step_ms reports. */
			if(!decoder_mem || decoder_bytes > (size_t)INT32_MAX)
				return STEP_FAILED;
			stb_vorbis_alloc block;
			block.alloc_buffer = (char*)decoder_mem;
			block.alloc_buffer_length_in_bytes = (int)decoder_bytes;
			int err = 0;
			s_load.ogg = stb_vorbis_open_filename(src->path, &err, &block);
			if(!s_load.ogg)
				return STEP_FAILED;
			stb_vorbis_info info = stb_vorbis_get_info(s_load.ogg);
			if((info.channels != 1 && info.channels != 2)
			   || info.sample_rate == 0)
				return STEP_FAILED;
			s_load.channels = info.channels;
			s_load.rate = info.sample_rate;
		}
		s_load.phase = LOAD_READ;
		return STEP_MORE;
	}

	if(!src->is_ogg) {
		uint32_t left = s_load.total_frames - s_load.done_frames;
		uint32_t want = AUDIO_DEFERRED_CHUNK_BYTES / 2u;
		if(want > left)
			want = left;
		int16_t* dst = scratch + s_load.done_frames;
		if(fread(dst, 2, want, s_load.wav) != want)
			return STEP_FAILED; /* truncated / unreadable: never a short play */
		/* File words are little-endian; the scratch holds HOST order, the
		   arena's rule (audio_load_file). In place: read word k, write k. */
		for(uint32_t k = 0; k < want; k++)
			dst[k] = wavh_sample_from_le(dst, k);
		s_load.done_frames += want;
		return s_load.done_frames == s_load.total_frames ? STEP_DONE
														 : STEP_MORE;
	}

	/* OGG: pull at most one tmp buffer of frames, never past the scratch. */
	uint32_t room = cap_frames - s_load.done_frames;
	uint32_t want = AUDIO_DEFERRED_OGG_FRAMES;
	if(want > room)
		want = room;
	if(want == 0) {
		/* Scratch full: legal only if the stream is exactly at its end. */
		int more = stb_vorbis_get_samples_short_interleaved(
			s_load.ogg, s_load.channels, s_ogg_tmp, s_load.channels);
		return more > 0 ? STEP_FAILED : STEP_DONE;
	}
	int got = stb_vorbis_get_samples_short_interleaved(
		s_load.ogg, s_load.channels, s_ogg_tmp, (int)want * s_load.channels);
	if(got < 0)
		return STEP_FAILED;
	if(got == 0)
		/* End of stream. Fewer frames than the scan-time probe promised is a
		   truncated or corrupt tail: FAILED, never a short play (hostile
		   review 06 #3 -- the WAV leg's short-fread rule, for ogg). */
		return (s_load.done_frames > 0 && s_load.done_frames >= src->frames)
			? STEP_DONE
			: STEP_FAILED;
	int16_t* dst = scratch + s_load.done_frames;
	if(s_load.channels == 1) {
		memcpy(dst, s_ogg_tmp, (size_t)got * 2u);
	} else {
		/* audio_load_file's average downmix, expression for expression. */
		for(int k = 0; k < got; k++)
			dst[k] = (int16_t)(((int)s_ogg_tmp[2 * k]
								+ (int)s_ogg_tmp[2 * k + 1])
							   / 2);
	}
	s_load.done_frames += (uint32_t)got;
	return STEP_MORE;
}

bool audio_deferred_loader_step(void) {
	if(!s_sync_ready)
		return false;

	thread_mutex_lock(&s_lock);
	if(s_state == AUDIO_DEFERRED_REQUESTED) {
		s_state = AUDIO_DEFERRED_LOADING;
		memset(&s_load, 0, sizeof(s_load));
		s_load.t_start_ms = deferred_now_ms();
	}
	if(s_state != AUDIO_DEFERRED_LOADING) {
		thread_mutex_unlock(&s_lock);
		return false;
	}
	s_in_step = true;
	struct audio_deferred_source src = s_job.src;
	int16_t* scratch = s_scratch;
	size_t capacity = s_capacity;
	void* decoder_mem = s_decoder_mem;
	size_t decoder_bytes = s_decoder_bytes;
	thread_mutex_unlock(&s_lock);

	int64_t t0 = deferred_now_ms();
	enum step_result res = deferred_step_work(&src, scratch, capacity,
											  decoder_mem, decoder_bytes);
#ifdef PLATFORM_WII
	if(res == STEP_DONE)
		/* The DSP reads physical memory; the loader just wrote through the
		   data cache. Flushed after the FINAL write of every load and before
		   READY is published. Every load has exactly ONE start -- take_ready
		   hands a request out once and nothing replays the scratch -- so
		   this is a fresh flush before every playback (Astra D1), kept off
		   the main thread on purpose (~600KB of DCFlushRange is a frame-time
		   cost there). A future replay path must flush again at its start. */
		DCFlushRange(scratch, (u32)s_load.done_frames * 2u);
#endif
	int32_t step_ms = deferred_span_ms(t0, deferred_now_ms());
#ifdef CAVEWII_TESTING
	/* Deterministic stand-in for "the main thread acts while a step runs":
	   the lock is free and s_in_step is set, exactly as it would be then. */
	if(s_test_step_hook)
		s_test_step_hook();
#endif

	thread_mutex_lock(&s_lock);
	s_in_step = false;
	if(step_ms > s_counters.max_step_ms)
		s_counters.max_step_ms = step_ms;
	bool more = false;
	if(s_cancel_pending) {
		/* The main thread cancelled while this step ran: it could not touch
		   s_load then, so the epilogue does what the cancel would have. */
		s_cancel_pending = false;
		deferred_close_load();
		deferred_finalize(AUDIO_DEFERRED_OUT_CANCELLED);
		s_state = AUDIO_DEFERRED_IDLE;
	} else if(res == STEP_FAILED) {
		deferred_close_load();
		deferred_finalize(AUDIO_DEFERRED_OUT_FAILED);
		s_state = AUDIO_DEFERRED_IDLE;
	} else if(res == STEP_DONE) {
		s_job.frames = s_load.done_frames;
		s_job.rate = s_load.rate;
		s_job.load_ms = deferred_span_ms(s_load.t_start_ms, deferred_now_ms());
		deferred_close_load();
		s_ready_taken = false;
		s_state = AUDIO_DEFERRED_READY;
	} else {
		more = true;
	}
	thread_cond_broadcast(&s_step_cv);
	thread_mutex_unlock(&s_lock);
	return more;
}

#ifndef CAVEWII_TESTING
static void* deferred_thread_main(void* arg) {
	(void)arg;
	for(;;) {
		thread_mutex_lock(&s_lock);
		while(!s_thread_stop && s_state != AUDIO_DEFERRED_REQUESTED
			  && s_state != AUDIO_DEFERRED_LOADING)
			thread_cond_wait(&s_wake, &s_lock);
		bool stop = s_thread_stop;
		thread_mutex_unlock(&s_lock);
		if(stop)
			break;
		audio_deferred_loader_step();
		/* A REAL sleep, not a yield (Astra D2): at priority 40 a yield
		   hands the CPU to nobody below the music worker, so only a sleep
		   bounds this thread's share against the mesher. */
		thread_msleep(AUDIO_DEFERRED_STEP_SLEEP_MS);
	}
	return NULL;
}
#endif

/* s_lock held. */
static bool deferred_thread_ensure(void) {
#ifdef CAVEWII_TESTING
	return !s_test_fail_thread; /* tests drive the loader by hand */
#else
	if(s_thread_up)
		return true;
	s_thread_stop = false;
	if(!thread_create(&s_thread, deferred_thread_main, NULL,
					  AUDIO_DEFERRED_THREAD_PRIO))
		return false;
	s_thread_up = true;
	return true;
#endif
}

/* ===== main-thread API ================================================== */

bool audio_deferred_attach(int16_t* scratch, size_t capacity_bytes) {
	if(!deferred_sync_init())
		return false;
	thread_mutex_lock(&s_lock);
	bool ok = false;
	if(s_state == AUDIO_DEFERRED_UNAVAILABLE && !s_in_step) {
		if(scratch && capacity_bytes >= AUDIO_DEFERRED_ALIGN
		   && capacity_bytes % AUDIO_DEFERRED_ALIGN == 0
		   && ((uintptr_t)scratch % AUDIO_DEFERRED_ALIGN) == 0
		   && deferred_thread_ensure()) {
			s_scratch = scratch;
			s_capacity = capacity_bytes;
			s_slot = -1;
			s_cancel_pending = false;
			s_state = AUDIO_DEFERRED_IDLE;
			ok = true;
		}
	}
	thread_mutex_unlock(&s_lock);
	return ok;
}

bool audio_deferred_request(const struct audio_deferred_source* src,
							uint16_t sound_id, int variant, const float pos[3],
							float vol, float pitch) {
	if(!deferred_sync_init())
		return false;
	thread_mutex_lock(&s_lock);
	s_counters.requests++;
	deferred_advance_drain();
	bool ok = false;
	if(!src || !pos || s_state == AUDIO_DEFERRED_UNAVAILABLE) {
		deferred_refuse(sound_id, variant, AUDIO_DEFERRED_OUT_FAILED);
	} else if(s_state != AUDIO_DEFERRED_IDLE) {
		deferred_refuse(sound_id, variant, AUDIO_DEFERRED_OUT_BUSY);
	} else {
		memset(&s_job, 0, sizeof(s_job));
		s_job.req_id = s_next_req++;
		s_job.epoch = s_epoch;
		s_job.sound_id = sound_id;
		s_job.variant = variant;
		s_job.src = *src;
		s_job.src.path[sizeof(s_job.src.path) - 1] = '\0';
		memcpy(s_job.pos, pos, sizeof(s_job.pos));
		s_job.vol = vol;
		s_job.pitch = pitch;
		s_job.t_request_ms = deferred_now_ms();
		s_job.load_ms = -1;
		s_job.start_delay_ms = -1;
		s_state = AUDIO_DEFERRED_REQUESTED;
		thread_cond_signal(&s_wake);
		ok = true;
	}
	thread_mutex_unlock(&s_lock);
	return ok;
}

bool audio_deferred_take_ready(struct audio_deferred_ready* out) {
	if(!s_sync_ready || !out)
		return false;
	thread_mutex_lock(&s_lock);
	deferred_advance_drain();
	bool ok = false;
	if(s_state == AUDIO_DEFERRED_READY && !s_ready_taken) {
		s_ready_taken = true;
		out->req_id = s_job.req_id;
		out->sound_id = s_job.sound_id;
		out->variant = s_job.variant;
		out->pcm = s_scratch;
		out->frames = s_job.frames;
		out->rate = s_job.rate;
		memcpy(out->pos, s_job.pos, sizeof(out->pos));
		out->vol = s_job.vol;
		out->pitch = s_job.pitch;
		ok = true;
	}
	thread_mutex_unlock(&s_lock);
	return ok;
}

bool audio_deferred_service_idle(void) {
	if(!s_sync_ready) return true;
	struct thread_mutex_guard guard;
	if(!thread_mutex_try_guard(&s_lock, &guard)) return false;
	bool idle = s_state == AUDIO_DEFERRED_UNAVAILABLE || s_state == AUDIO_DEFERRED_IDLE;
	thread_mutex_release_guard(&s_lock, &guard);
	return idle;
}

void audio_deferred_note_started(uint32_t req_id, int slot) {
	if(!s_sync_ready)
		return;
	thread_mutex_lock(&s_lock);
	if(s_state == AUDIO_DEFERRED_READY && s_ready_taken
	   && s_job.req_id == req_id) {
		s_job.started = true;
		s_job.start_delay_ms
			= deferred_span_ms(s_job.t_request_ms, deferred_now_ms());
		s_slot = slot;
		s_state = AUDIO_DEFERRED_PLAYING;
	}
	thread_mutex_unlock(&s_lock);
}

void audio_deferred_note_not_started(uint32_t req_id,
									 enum audio_deferred_outcome why) {
	if(!s_sync_ready)
		return;
	thread_mutex_lock(&s_lock);
	if(s_state == AUDIO_DEFERRED_READY && s_ready_taken
	   && s_job.req_id == req_id) {
		/* No voice ever read the scratch: no drain margin is owed. */
		deferred_finalize(why);
		s_state = AUDIO_DEFERRED_IDLE;
	}
	thread_mutex_unlock(&s_lock);
}

void audio_deferred_note_voice_released(int slot,
										enum audio_deferred_outcome why) {
	if(!s_sync_ready)
		return;
	thread_mutex_lock(&s_lock);
	if(s_state == AUDIO_DEFERRED_PLAYING && slot == s_slot) {
		deferred_finalize(why);
		s_slot = -1;
		s_release_ms = deferred_now_ms();
		s_state = AUDIO_DEFERRED_DRAINING;
	}
	thread_mutex_unlock(&s_lock);
}

int audio_deferred_voice_slot(void) {
	if(!s_sync_ready)
		return -1;
	thread_mutex_lock(&s_lock);
	int slot = s_state == AUDIO_DEFERRED_PLAYING ? s_slot : -1;
	thread_mutex_unlock(&s_lock);
	return slot;
}

/* s_lock held. Cancels REQUESTED / LOADING / READY; a PLAYING voice is the
   voice layer's to stop (it reports back through note_voice_released). */
static void deferred_cancel_locked(void) {
	/* The epoch names a generation of requests; it moves only when a cancel
	   actually ended one (this is called every tick while no world is loaded). */
	switch(s_state) {
		case AUDIO_DEFERRED_REQUESTED:
		case AUDIO_DEFERRED_READY:
			s_epoch++;
			deferred_finalize(AUDIO_DEFERRED_OUT_CANCELLED);
			s_state = AUDIO_DEFERRED_IDLE;
			break;
		case AUDIO_DEFERRED_LOADING:
			if(s_cancel_pending)
				break; /* already cancelled; the running step will finish it */
			s_epoch++;
			if(s_in_step) {
				s_cancel_pending = true; /* the step's epilogue finishes it */
			} else {
				deferred_close_load();
				deferred_finalize(AUDIO_DEFERRED_OUT_CANCELLED);
				s_state = AUDIO_DEFERRED_IDLE;
			}
			break;
		default: break;
	}
}

void audio_deferred_cancel(void) {
	if(!s_sync_ready)
		return;
	thread_mutex_lock(&s_lock);
	deferred_cancel_locked();
	thread_mutex_unlock(&s_lock);
}

void audio_deferred_quiesce(void) {
	if(!s_sync_ready)
		return;
	thread_mutex_lock(&s_lock);
	deferred_cancel_locked();
	/* As long as one outstanding file read; see the header. The loader's
	   epilogue clears s_in_step and broadcasts under this same lock. */
	while(s_in_step)
		thread_cond_wait(&s_step_cv, &s_lock);
	if(s_state == AUDIO_DEFERRED_PLAYING) {
		/* The caller stops every voice first (audio_stop_all); reaching
		   here PLAYING means it did not. Reconcile rather than leak. */
		deferred_finalize(AUDIO_DEFERRED_OUT_CANCELLED);
		s_slot = -1;
		s_release_ms = deferred_now_ms();
		s_state = AUDIO_DEFERRED_DRAINING;
	}
	if(s_state == AUDIO_DEFERRED_DRAINING) {
		/* The caller is about to REWRITE these bytes, so this is the one
		   path where the drain margin matters most (hostile review 06 #4):
		   sleep out whatever is left of it, once, lock released. */
		int64_t left
			= AUDIO_DEFERRED_DRAIN_MS - (deferred_now_ms() - s_release_ms);
		if(left > 0) {
			if(left > AUDIO_DEFERRED_DRAIN_MS)
				left = AUDIO_DEFERRED_DRAIN_MS;
			thread_mutex_unlock(&s_lock);
			thread_msleep((size_t)left);
			thread_mutex_lock(&s_lock);
		}
	}
	s_scratch = NULL;
	s_capacity = 0;
	s_decoder_mem = NULL;
	s_decoder_bytes = 0;
	s_state = AUDIO_DEFERRED_UNAVAILABLE;
	thread_mutex_unlock(&s_lock);
}

void audio_deferred_set_decoder_memory(void* block, size_t bytes) {
	if(!s_sync_ready)
		return;
	thread_mutex_lock(&s_lock);
	/* Only between loads: a step holds its own copy of the old block. */
	if(s_state == AUDIO_DEFERRED_IDLE && !s_in_step) {
		s_decoder_mem = bytes ? block : NULL;
		s_decoder_bytes = block ? bytes : 0;
	}
	thread_mutex_unlock(&s_lock);
}

void audio_deferred_shutdown(void) {
	if(!s_sync_ready)
		return;
	audio_deferred_quiesce();
#ifndef CAVEWII_TESTING
	thread_mutex_lock(&s_lock);
	bool join = s_thread_up;
	s_thread_stop = true;
	thread_cond_broadcast(&s_wake);
	thread_mutex_unlock(&s_lock);
	if(join) {
		thread_join(&s_thread);
		s_thread_up = false;
	}
#endif
}

enum audio_deferred_state audio_deferred_state(void) {
	if(!s_sync_ready)
		return AUDIO_DEFERRED_UNAVAILABLE;
	thread_mutex_lock(&s_lock);
	deferred_advance_drain();
	enum audio_deferred_state st = s_state;
	thread_mutex_unlock(&s_lock);
	return st;
}

void audio_deferred_counters(struct audio_deferred_counters* out) {
	if(!out)
		return;
	if(!s_sync_ready) {
		memset(out, 0, sizeof(*out));
		return;
	}
	thread_mutex_lock(&s_lock);
	*out = s_counters;
	thread_mutex_unlock(&s_lock);
}

bool audio_deferred_pop_record(struct audio_deferred_record* out) {
	if(!s_sync_ready || !out)
		return false;
	thread_mutex_lock(&s_lock);
	bool ok = s_rec_count > 0;
	if(ok) {
		*out = s_records[s_rec_head];
		s_rec_head = (s_rec_head + 1) % AUDIO_DEFERRED_RECORDS;
		s_rec_count--;
	}
	thread_mutex_unlock(&s_lock);
	return ok;
}

/* ===== scan-time probe (main thread, no module state) =================== */

bool audio_deferred_probe(const char* path, bool is_ogg, uint32_t* frames,
						  uint32_t* rate) {
	return audio_deferred_probe_ex(path, is_ogg, frames, rate, NULL);
}

bool audio_deferred_probe_ex(const char* path, bool is_ogg, uint32_t* frames,
							 uint32_t* rate, size_t* decoder_bytes) {
	if(!path || !frames || !rate)
		return false;
	if(decoder_bytes)
		*decoder_bytes = 0; /* a WAV needs no decoder state */
	if(!is_ogg) {
		FILE* f = fopen(path, "rb");
		if(!f)
			return false;
		uint32_t off = 0;
		bool ok = wavh_probe_file(f, &off, frames, rate);
		fclose(f);
		return ok;
	}
	int err = 0;
	stb_vorbis* v = stb_vorbis_open_filename(path, &err, NULL);
	if(!v)
		return false;
	stb_vorbis_info info = stb_vorbis_get_info(v);
	unsigned int total = stb_vorbis_stream_length_in_samples(v);
	stb_vorbis_close(v);
	if((info.channels != 1 && info.channels != 2) || info.sample_rate == 0
	   || total == 0)
		return false;
	*frames = total;
	*rate = info.sample_rate;
	if(decoder_bytes) {
		/* What stb_vorbis would carve from an alloc buffer for THIS file:
		   persistent setup memory from the front, and from the back the
		   larger of the setup-time and decode-time temp needs. Plus slack
		   for its 8-byte rounding of each carve. Scan time, so this probe's
		   own heap use is the resident loader's, behind the loading screen. */
		unsigned int temp = info.setup_temp_memory_required
				> info.temp_memory_required ?
			info.setup_temp_memory_required :
			info.temp_memory_required;
		*decoder_bytes = (size_t)info.setup_memory_required + (size_t)temp
			+ AUDIO_DEFERRED_DECODER_SLACK;
	}
	return true;
}

/* ===== test hooks ======================================================= */

#ifdef CAVEWII_TESTING
void audio_deferred_test_reset(void) {
	if(s_sync_ready) {
		thread_mutex_lock(&s_lock);
		deferred_close_load();
		thread_mutex_unlock(&s_lock);
	}
	s_state = AUDIO_DEFERRED_UNAVAILABLE;
	s_scratch = NULL;
	s_capacity = 0;
	s_decoder_mem = NULL;
	s_decoder_bytes = 0;
	s_epoch = 0;
	s_next_req = 1;
	memset(&s_job, 0, sizeof(s_job));
	s_in_step = false;
	s_cancel_pending = false;
	s_ready_taken = false;
	s_slot = -1;
	s_release_ms = 0;
	memset(&s_counters, 0, sizeof(s_counters));
	s_rec_head = s_rec_count = 0;
	s_test_fail_thread = false;
	s_test_clock_ms = -1;
	s_test_step_hook = NULL;
}

void audio_deferred_test_fail_thread(bool fail) { s_test_fail_thread = fail; }

void audio_deferred_test_set_clock_ms(int64_t ms) { s_test_clock_ms = ms; }

void audio_deferred_test_set_step_hook(void (*hook)(void)) {
	s_test_step_hook = hook;
}
#endif
