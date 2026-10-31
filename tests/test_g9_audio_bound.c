/* Copyright (c) 2026 CaveWii_v2 contributors.
 * R18 S1 finite service; retain the original RED through --baseline-red.
 * Compile the real native Wii backend behind
 * named AESND/cache/clock boundaries. Injected costs are logic controls,
 * never measured Broadway bounds. No decoder or ring algorithm is doubled.
 */
#include <assert.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/audio.h"
#include "platform/audio_stream_ring.h"
#include "platform/time.h"

/* Use the real SDK declarations, without its PPC types/intrinsics on HOST. */
#define __GCTYPES_H__
#define __CACHE_H__
#define HW_RVL
typedef uint16_t u16;
typedef uint32_t u32;
typedef float f32;
#include <aesndlib.h>

struct aesndpb_t { unsigned slot; };
static struct aesndpb_t sdk_voices[MAX_VOICES];
static unsigned sdk_allocated, unsafe_entries, flushes, starts;
static bool admitted;
static uint32_t timer_cost_us;
static unsigned stops, gains;
static u16 last_left, last_right;
static u32 maximum_flush;
static uint32_t voice_cost_us;
static uint64_t clock_us, deadline_us;
static uint32_t flush_cost_us;
static int16_t pcm[AUDIO_STREAM_RING_SLOT_SAMPLES];

void AESND_Init(void) { sdk_allocated = 0; }
AESNDPB* AESND_AllocateVoiceWithArg(AESNDVoiceCallbackArg cb, void* arg) {
	assert(!cb && !arg && sdk_allocated < MAX_VOICES);
	sdk_voices[sdk_allocated].slot = sdk_allocated;
	return &sdk_voices[sdk_allocated++];
}
void AESND_FreeVoice(AESNDPB* pb) { assert(pb); }
void AESND_SetVoiceStop(AESNDPB* pb, bool stop) {
	assert(pb);
	(void)stop;
	stops++;
}
void AESND_SetVoiceVolume(AESNDPB* pb, u16 left, u16 right) {
	assert(pb && left <= 255 && right <= 255);
	gains++; last_left = left; last_right = right;
}
void AESND_PlayVoice(AESNDPB* pb, u32 format, const void* buffer, u32 bytes,
	f32 rate, u32 delay, bool looped) {
	assert(pb && buffer && bytes && format == VOICE_MONO16 && rate > 0);
	assert(!delay);
	(void)looped;
	starts++;
	clock_us += voice_cost_us;
}
static void DCFlushRange(void* buffer, u32 bytes) {
	assert(buffer && bytes && !((uintptr_t)buffer & 31) && !(bytes & 31));
	/* The admission requirement is checked AT ENTRY, independently of the
	 * elapsed time after the indivisible boundary has already committed. */
	if(admitted && (clock_us >= deadline_us
		|| flush_cost_us > deadline_us - clock_us))
		unsafe_entries++;
	flushes++;
	if(bytes > maximum_flush) maximum_flush = bytes;
	clock_us += flush_cost_us;
}
ptime_t time_get(void) {
	clock_us += timer_cost_us;
	return (ptime_t){.tv_sec = clock_us / 1000000,
		.tv_nsec = (clock_us % 1000000) * 1000};
}
ptime_t time_add_ms(ptime_t t, unsigned int ms) {
	t.tv_sec += ms / 1000;
	t.tv_nsec += (long)(ms % 1000) * 1000000;
	if(t.tv_nsec >= 1000000000) {
		t.tv_nsec -= 1000000000;
		t.tv_sec++;
	}
	return t;
}
int32_t time_diff_ms(ptime_t from, ptime_t to) {
	return (int32_t)(((int64_t)to.tv_sec - from.tv_sec) * 1000
		+ ((int64_t)to.tv_nsec - from.tv_nsec) / 1000000);
}

uint64_t time_diff_us(ptime_t from, ptime_t to) {
    return (uint64_t)(((int64_t)to.tv_sec - from.tv_sec) * 1000000
        + ((int64_t)to.tv_nsec - from.tv_nsec) / 1000);
}

/* Host allocation spelling only; init/shutdown are OUTSIDE admission. */
#ifdef _WIN32
#define memalign(alignment, size) __mingw_aligned_malloc((size), (alignment))
#define free __mingw_aligned_free
#endif
#define AUDIO_VOICES 16
#include "platform/wii/audio.c"
#ifdef _WIN32
#undef memalign
#undef free
#endif

static int baseline_red(void) {
	/* The backend is normally included by audio.c, which uses all hooks. */
	(void)audio_backend_voice_start;
	(void)audio_backend_voice_gain;
	(void)audio_backend_voice_playing;
	(void)audio_backend_stop_all;
	(void)audio_backend_music_last_dur_ms;
	(void)audio_backend_music_reserve_us;
	(void)audio_backend_music_native_rate;
	(void)audio_backend_music_gain;
	(void)audio_backend_music_playing;
	(void)audio_backend_music_drained;
	(void)audio_backend_music_stop;
	assert(audio_backend_init());
	for(unsigned i = 0; i < AUDIO_STREAM_RING_SLOT_SAMPLES; i++)
		pcm[i] = (int16_t)(i ^ 0x1357);
	deadline_us = clock_us + 500;
	flush_cost_us = 600; /* Known HOST cost larger than the entire turn. */
	admitted = true;
	audio_backend_music_start(pcm, AUDIO_STREAM_RING_SLOT_SAMPLES,
		48000.0F, 0.5F);
	admitted = false;
	assert(flushes == 1 && starts == 0);
	assert(!memcmp(s_wii_music_ring_buf, pcm, sizeof(pcm)));
	assert(s_wii_music_ring.pending_slots == 1);
	printf("native audio bound: unsafe_entries=%u flushes=%u "
		"deadline_us=%llu elapsed_us=%llu committed_slots=%llu\n",
		unsafe_entries, flushes, (unsigned long long)deadline_us,
		(unsigned long long)clock_us,
		(unsigned long long)s_wii_music_ring.pending_slots);
	audio_backend_shutdown();
	/* A deadline wrapper must refuse this boundary BEFORE PCM/ring commit.
	 * Keep this RED until a real bounded entry point replaces the call. */
	if(unsafe_entries) {
		puts("RED: native refill entered unsupported indivisible cache work; "
			"no bounded service / no AUDIO completion credit admissible");
		return 1;
	}
	puts("test_g9_audio_bound: PASS");
	return 0;
}

/* Every number below is a HOST-injected logic control, NOT native E. */
static struct audio_service_limits limits;
static struct audio_service_metrics metrics;
static void configure_limits(void) {
	assert(audio_service_voice_math_finite(64.0F));
	assert(audio_service_voice_math_finite(-64.0F));
	assert(!audio_service_voice_math_finite(64.01F));
	assert(!audio_service_voice_math_finite(INFINITY));
	assert(!audio_service_voice_math_finite(NAN));
    memset(&limits, 0, sizeof limits);
    memset(&metrics, 0, sizeof metrics);
    for(unsigned k=0; k<AUDIO_UNIT_COUNT; k++) limits.e_us[k]=40;
    limits.bookkeeping_us=4;
    flush_cost_us=20; voice_cost_us=0; timer_cost_us=1; maximum_flush=0;
}
static struct audio_service_budget turn(unsigned cap) {
    struct audio_service_budget b={.start=time_get(), .cap_us=cap,
        .limits=&limits, .metrics=&metrics};
    deadline_us=clock_us+cap; admitted=true;
    return b;
}
static void reset_backend(void) {
    admitted=false;
    if(s_wii_open) audio_backend_shutdown();
    assert(audio_backend_init());
    configure_limits();
    flushes=starts=stops=gains=unsafe_entries=0;
}
static void finish_chunk(unsigned frames, unsigned rate, float gain) {
    unsigned more=0;
    for(unsigned i=0; i<100; i++) {
        struct audio_service_budget b=turn(500);
        enum audio_service_result r=audio_backend_music_service(pcm,frames,rate,gain,&b);
        assert(r != AUDIO_SERVICE_UNSAFE);
        if(r==AUDIO_SERVICE_DONE) {
            assert(more && !s_wii_audio_cursor.active);
            assert(!unsafe_entries && maximum_flush <= AUDIO_SERVICE_BYTES);
            return;
        }
        more++;
    }
    assert(!"cursor did not finish");
}
static void continuation_equivalence(void) {
    reset_backend();
    for(unsigned i=0;i<AUDIO_STREAM_RING_SLOT_SAMPLES;i++) pcm[i]=(int16_t)(i^0x1357);
    finish_chunk(AUDIO_STREAM_RING_SLOT_SAMPLES,48000,0.5f);
    assert(s_wii_music_ring.pending_slots==1 && starts==0);
    assert(!memcmp(s_wii_music_ring_buf,pcm,sizeof pcm));
    unsigned first_flushes=flushes;
    finish_chunk(37,48000,0.25f);
    assert(starts==1 && s_wii_music_ring.running && s_wii_music_ring.written_slots==2);
    assert(s_wii_music_ring.written_samples==AUDIO_STREAM_RING_SLOT_SAMPLES+37);
    assert(last_left==63 && last_right==63);
    int16_t* second=s_wii_music_ring_buf+AUDIO_STREAM_RING_SLOT_SAMPLES;
    assert(!memcmp(second,pcm,37*sizeof(int16_t)));
    for(unsigned i=37;i<AUDIO_STREAM_RING_SLOT_SAMPLES;i++) assert(second[i]==0);
    assert(flushes-first_flushes==32+288); /* slot then whole-prime, sync each */
    size_t written=s_wii_music_ring.written_samples;
    /* Same input/clock tape, legacy ordinary path: byte identity and gain. */
    static int16_t bounded_ring[WII_MUSIC_RING_SAMPLES];
    memcpy(bounded_ring,s_wii_music_ring_buf,sizeof bounded_ring);
    admitted=false;
    audio_backend_music_stop();
    audio_backend_music_start(pcm,AUDIO_STREAM_RING_SLOT_SAMPLES,48000,0.5f);
    audio_backend_music_start(pcm,37,48000,0.25f);
    assert(!memcmp(bounded_ring,s_wii_music_ring_buf,sizeof bounded_ring));
    assert(s_wii_music_ring.written_samples==written && last_left==63 && last_right==63);
    puts("PASS: >one-quantum native refill, whole-prime flush, short EOF PCM/padding/order/gain match OFF");
}
static void rejection_and_recovery(void) {
    reset_backend();
    struct audio_service_budget b=turn(500);
    limits.e_us[AUDIO_UNIT_STATE]=0;
    assert(audio_backend_music_service(pcm,4096,48000,1,&b)==AUDIO_SERVICE_UNSAFE);
    assert(!s_wii_audio_cursor.active && !flushes && !starts);
    configure_limits();
    b=turn(43); /* 40 E + 4 bookkeeping does NOT fit */
    assert(audio_backend_music_service(pcm,4096,48000,1,&b)==AUDIO_SERVICE_MORE);
    assert(!s_wii_audio_cursor.active);
    b=turn(0);
    assert(audio_backend_music_service(pcm,4096,48000,1,&b)==AUDIO_SERVICE_MORE);
    b=turn(501);
    assert(audio_backend_music_service(pcm,4096,48000,1,&b)==AUDIO_SERVICE_MORE);
    b=turn(500);
    limits.e_us[AUDIO_UNIT_FLUSH]=600;
    assert(audio_backend_music_service(pcm,4096,48000,1,&b)==AUDIO_SERVICE_MORE);
    assert(s_wii_music_ring.pending_slots==0 && !flushes);
    for(unsigned i=0;i<8 && s_wii_audio_cursor.phase<4;i++) {
        b=turn(500);
        assert(audio_backend_music_service(pcm,4096,48000,1,&b)==AUDIO_SERVICE_MORE);
    }
    assert(s_wii_audio_cursor.phase==4 && !flushes);
    limits.e_us[AUDIO_UNIT_FLUSH]=40; flush_cost_us=600;
    b=turn(500);
    assert(audio_backend_music_service(pcm,4096,48000,1,&b)==AUDIO_SERVICE_UNSAFE);
    assert(metrics.overruns[AUDIO_UNIT_FLUSH]==1 && limits.e_us[AUDIO_UNIT_FLUSH]==0);
    assert(s_wii_music_ring.pending_slots==0 && s_wii_audio_cursor.offset==256);
    unsigned done_flushes=flushes;
    /* The offending unit stays committed, and ordinary escape finishes it
     * exactly once without admission or completion credit for that turn. */
    b.ordinary=true;
    assert(audio_backend_music_service(pcm,4096,48000,1,&b)==AUDIO_SERVICE_DONE);
    assert(flushes==done_flushes+31 && s_wii_music_ring.pending_slots==1);
    assert(!memcmp(s_wii_music_ring_buf,pcm,sizeof pcm));
    assert(starts==0);
    puts("PASS: unknown E/deadline reject before entry; observed breach invalidates E; ordinary recovery never repeats flushed unit");
}
static void cancel_restart_eof(void) {
    reset_backend();
    struct audio_service_budget b=turn(100);
    assert(audio_backend_music_service(pcm,4096,48000,1,&b)==AUDIO_SERVICE_MORE);
    assert(s_wii_audio_cursor.active);
    admitted=false;
    audio_backend_music_stop();
    assert(!s_wii_audio_cursor.active && !s_wii_music_ring.pending_slots);
    finish_chunk(13,44100,0.75f);
    unsigned before=starts;
    bool drained;
    for(unsigned i=0;i<50;i++) {
        b=turn(500);
        enum audio_service_result r=audio_backend_music_poll_service(true,&drained,&b);
        assert(r!=AUDIO_SERVICE_UNSAFE);
        if(r==AUDIO_SERVICE_DONE) break;
        assert(i<49 && !drained);
    }
    assert(starts==before+1 && !drained && s_wii_music_ring.running);
    b=turn(500);
    assert(audio_backend_music_poll_service(true,&drained,&b)==AUDIO_SERVICE_DONE);
    assert(starts==before+1); /* repeated EOF poll never arms twice */
    clock_us+=100000;
    b=turn(500);
    assert(audio_backend_music_poll_service(true,&drained,&b)==AUDIO_SERVICE_DONE && drained);
    admitted=false;
    audio_backend_music_stop();
    finish_chunk(4096,48000,0.5f);
    b=turn(500);
    assert(audio_backend_music_service(pcm,0,48000,0.5f,&b)==AUDIO_SERVICE_DONE);
    assert(s_wii_music_ring.pending_slots==1); /* zero-frame EOF adds no slot */
    puts("PASS: cancel/restart/short EOF and exact-multiple EOF; repeated entry starts voice exactly once");
}
static void voice_overrun_and_reserve(void) {
    reset_backend();
    finish_chunk(4096,48000,0.5f);
    voice_cost_us=100; /* unexpected SDK/IRQ cost, larger than E_START=40 */
    enum audio_service_result r=AUDIO_SERVICE_MORE;
    for(unsigned i=0; i<100 && r==AUDIO_SERVICE_MORE; i++) {
        struct audio_service_budget b=turn(500);
        r=audio_backend_music_service(pcm,4096,48000,0.5f,&b);
    }
    assert(r==AUDIO_SERVICE_UNSAFE && starts==1);
    assert(s_wii_music_ring.written_slots==2 && s_wii_audio_cursor.phase==8);
    assert(!limits.e_us[AUDIO_UNIT_START] && metrics.overruns[AUDIO_UNIT_START]==1);
    unsigned f=flushes, g=gains;
    struct audio_service_budget b=turn(500); b.ordinary=true;
    assert(audio_backend_music_service(pcm,4096,48000,0.5f,&b)==AUDIO_SERVICE_DONE);
    assert(starts==1 && gains==g && flushes==f);
    limits.max_age_us=50000; limits.wake_refill_us=300;
    limits.backend_us=4000; limits.drift_us=1000;
    struct audio_service_witness w={.valid=true,.generation=7,.voice_generation=1,
        .backend_generation=s_wii_audio_generation,.sampled_at=time_get(),
        .reserve_us=audio_backend_music_reserve_us()};
    w.latest_refill_us=w.reserve_us-5300;
    struct audio_service_witness fresh=w;
    uint32_t reserve;
    assert(audio_service_reserve_aged(&w,&fresh,&limits,time_get(),4000,&reserve));
    assert(reserve < w.reserve_us);
    uint32_t original=w.reserve_us;
    w.reserve_us=5300+4000; w.latest_refill_us=4000;
    assert(!audio_service_reserve_aged(&w,&fresh,&limits,time_get(),4000,&reserve));
    w.reserve_us=original; w.latest_refill_us=original-5300;
    clock_us+=50001;
    assert(!audio_service_reserve_aged(&w,&fresh,&limits,time_get(),4000,&reserve));
    w.sampled_at=time_get();
    admitted=false; audio_backend_music_stop();
    fresh.backend_generation=s_wii_audio_generation;
    assert(!audio_service_reserve_aged(&w,&fresh,&limits,time_get(),4000,&reserve));
    assert(audio_backend_music_reserve_us()==0);
    puts("PASS: voice-start breach commits once, denies admission, ordinary retry never re-arms; native queued reserve age/exact/exhaustion/generation checks");
}
static void expired_live_cursor(void) {
    reset_backend();
    finish_chunk(4096,48000,0.5f); finish_chunk(4096,48000,0.5f);
    struct audio_service_budget b=turn(100);
    assert(audio_backend_music_service(pcm,4096,48000,0.5f,&b)==AUDIO_SERVICE_MORE);
    assert(s_wii_audio_cursor.active && s_wii_music_ring.written_slots==2);
    clock_us+=300000; /* head outlasts old queued PCM while continuation paused */
    b=turn(500);
    assert(audio_backend_music_service(pcm,4096,48000,0.5f,&b)==AUDIO_SERVICE_UNSAFE);
    assert(s_wii_music_ring.written_slots==2); /* no partial-slot publication */
    b.ordinary=true; admitted=false;
    assert(audio_backend_music_service(pcm,4096,48000,0.5f,&b)==AUDIO_SERVICE_DONE);
    assert(!s_wii_audio_cursor.active && s_wii_music_ring.pending_slots==1);
    assert(!memcmp(s_wii_music_ring_buf,pcm,sizeof pcm));
    assert(!s_wii_music_ring.running); /* ordinary re-prime, never replay live tail */
    puts("PASS: expired live destination denies credit, retains PCM, and ordinary reset completes without wedging/duplicate publication");
}
int main(int argc, char** argv) {
    if(argc>1 && !strcmp(argv[1],"--baseline-red")) return baseline_red();
    continuation_equivalence(); rejection_and_recovery(); cancel_restart_eof();
    voice_overrun_and_reserve(); expired_live_cursor();
    admitted=false; audio_backend_shutdown();
    puts("test_g9_audio_bound: PASS (HOST controls; TIMING-UNVERIFIED)");
    return 0;
}
