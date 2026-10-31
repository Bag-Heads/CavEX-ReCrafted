/*
	C2 P2 -- state-machine test for the DEFERRED one-shot loader
	(source/platform/audio_deferred.{c,h}).

	AUTHORED AGAINST THE CONTRACT, NOT THE IMPLEMENTATION:
	tmp/jobs/c2-audio-coverage-0917/design-p2-deferred.md (Fable, ratified by
	Astra ruling 3, 2026-09-19) plus the header's own threading contract. The
	.c is written in parallel; every expectation below is transcribed from
	that document / audio_deferred.h, never from a run of the code under test
	(CODESTYLE 4.2, the poisoned-golden rule).

	This test PLAYS THE ROLE OF audio.c. Under CAVEWII_TESTING no loader
	thread is ever spawned, so the test owns both sides of the threading
	contract: it calls the main-thread API (request / take_ready /
	note_started / note_not_started / note_voice_released / cancel / quiesce)
	and drives the loader by hand with audio_deferred_loader_step(), which
	gives deterministic interleavings no real thread could reproduce. All
	time-dependent behaviour (the DRAINING margin, load_ms, start_delay_ms)
	is driven off the fake clock, audio_deferred_test_set_clock_ms().

	Cases (one static void test_xxx per case, in main() order):
	  a happy_path_wav          request -> READY -> start -> drain -> IDLE
	  b multi_step              > 3 * CHUNK_BYTES needs >= 4 loader steps
	  c busy                    a 2nd request is refused in all 5 busy states
	  d cancel                  REQUESTED / LOADING / READY -> CANCELLED
	  e stale_epoch             the cancelled job never writes another byte
	  f released_steal_cancel   STOLEN / CANCELLED while PLAYING; wrong slot
	  g repeat_steal_reuse      5 steal/reuse cycles reconcile one-for-one
	  h culled                  note_not_started(CULLED), no drain wait
	  i failures                missing / truncated / stereo / grown / NULL
	  j unavailable             no scratch, no loader thread, then recovery
	  k quiesce                 LOADING -> UNAVAILABLE, loader writes nothing
	  l probe                   audio_deferred_probe agrees with wavh_parse
	  n ogg_happy               mono + stereo: multi-step load, READY, PCM
	                            identical to the RESIDENT whole-file decode
	  o ogg_no_decoder          no decoder block -> FAILED; a WAV still works
	  p ogg_small_decoder       1KB block -> FAILED, nothing written past it
	  q ogg_truncated           short/unopenable stream -> FAILED, never a
	                            READY with fewer frames than promised
	  r ogg_scratch_edges       capacity exactly == the sample loads; one
	                            ALIGN less FAILS with the guards intact
	  s wav_adversarial         fmt 18/40, two `data` chunks, a header
	                            straddling EOF, a size past EOF, the 32-bit
	                            wrap guard, an odd `data` size + word pad
	  m accounting              requests == sum(outcome[]) over every case

	WAV FIXTURES are generated at run time into a PID-unique directory (never
	a fixed name -- fixed names collide under `ctest -j`, the 2026-09-02
	test_fixture_dir root cause) and removed in main(). Nothing binary is
	generated into the tree. WAVs are built with wavh_write (audio_wav.h) or,
	for the adversarial ones, chunk by chunk below; wavh_parse is the
	independent reference the loaded PCM is diffed against.

	OGG FIXTURES are the two committed Vorbis streams described in
	tests/fixtures/audio_deferred/README.md (synthetic sine tones, CC0, made
	for this project). This file never modifies them: it COPIES them into its
	own per-process directory, and the truncated variants case (q) needs are
	cut from those copies. The reference the decoded PCM is diffed against is
	the RESIDENT decoder -- stb_vorbis_decode_memory over the whole file, the
	call audio_registry.c's audio_load_file makes -- so "the chunked pull-API
	load equals the resident decode" is an assertion about two genuinely
	different decode drivers, not a re-run of the code under test.
*/

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include "support/test_fixture_dir.h"

#include "platform/audio_deferred.h"
#include "platform/audio_wav.h"

/* The RESIDENT decoder, for the ogg leg's independent reference: declarations
   only. The ONE stb_vorbis implementation TU stays audio_registry.c
   (third_party/stb/PATCHES.md), which this links against through
   cavex_test_core -- the macro set below is that file's, verbatim, so the
   declarations line up with the symbols it actually emits. */
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 2
#include <stb/stb_vorbis.c>

/* ------------------------------------------------------------------ */
/* Scratch with guard regions                                          */
/* ------------------------------------------------------------------ */

/* 64 guard WORDS (128 bytes, itself a multiple of AUDIO_DEFERRED_ALIGN so
   the region handed to attach stays 32-byte aligned) on each side of the
   scratch. A single byte written outside the granted region -- the class of
   bug the "no partial write past capacity" rule exists for -- lands in a
   guard and is caught after every case. */
#define GUARD_WORDS 64
#define SCRATCH_BYTES 65536u
#define SCRATCH_WORDS (SCRATCH_BYTES / 2u)

static _Alignas(32) int16_t s_pool[GUARD_WORDS + SCRATCH_WORDS + GUARD_WORDS];

static int16_t guard_word(int i) {
	return (int16_t)(0x5A5A + i * 7);
}

static int16_t* scratch_base(void) {
	return s_pool + GUARD_WORDS;
}

static void fill_guards(void) {
	for(int i = 0; i < GUARD_WORDS; i++) {
		s_pool[i] = guard_word(i);
		s_pool[GUARD_WORDS + (int)SCRATCH_WORDS + i] = guard_word(i);
	}
}

static void check_guards(void) {
	for(int i = 0; i < GUARD_WORDS; i++) {
		assert(s_pool[i] == guard_word(i));
		assert(s_pool[GUARD_WORDS + (int)SCRATCH_WORDS + i] == guard_word(i));
	}
}

static bool attach_scratch(void) {
	return audio_deferred_attach(scratch_base(), SCRATCH_BYTES);
}

/* Grant a scratch of EXACTLY `bytes`, positioned so that its END is flush
   against the upper guard region: one byte written past the granted capacity
   then lands in a guard word and check_guards() catches it. The offset stays
   AUDIO_DEFERRED_ALIGN-aligned because SCRATCH_BYTES and `bytes` both are. */
static bool attach_scratch_bytes(size_t bytes) {
	assert(bytes >= AUDIO_DEFERRED_ALIGN && bytes <= SCRATCH_BYTES);
	assert(bytes % AUDIO_DEFERRED_ALIGN == 0);
	return audio_deferred_attach(scratch_base() + (SCRATCH_WORDS - bytes / 2u),
								 bytes);
}

/* ------------------------------------------------------------------ */
/* The OGG decoder-state block, with guards                            */
/* ------------------------------------------------------------------ */

/* The module must never malloc stb_vorbis's setup/codebook/temp state during
   a load (hostile review 06 #1): it carves it out of THIS block and nothing
   else. The block handed over is exactly as long as audio_deferred_probe_ex
   said this file needs, and guard bytes sit immediately past that length, so
   "the decoder stayed inside what the probe promised" is checked rather than
   assumed -- which is also what case (p) asserts for a block that is too
   small to open the file at all. */
#define DEC_GUARD_BYTES 128
static _Alignas(32) uint8_t
	s_dec_pool[AUDIO_DEFERRED_MAX_DECODER_BYTES + DEC_GUARD_BYTES];
static size_t s_dec_len; /* 0 = no block is armed */

static uint8_t dec_guard_byte(size_t i) {
	return (uint8_t)(0xC3u + i * 5u);
}

static void arm_decoder(size_t bytes) {
	assert(bytes > 0 && bytes + DEC_GUARD_BYTES <= sizeof(s_dec_pool));
	memset(s_dec_pool, 0, bytes);
	for(size_t i = 0; i < DEC_GUARD_BYTES; i++)
		s_dec_pool[bytes + i] = dec_guard_byte(i);
	s_dec_len = bytes;
	audio_deferred_set_decoder_memory(s_dec_pool, bytes);
}

static void check_decoder_guards(void) {
	if(s_dec_len == 0)
		return;
	for(size_t i = 0; i < DEC_GUARD_BYTES; i++)
		assert(s_dec_pool[s_dec_len + i] == dec_guard_byte(i));
}

/* ------------------------------------------------------------------ */
/* Fixture generation                                                  */
/* ------------------------------------------------------------------ */

#define MAX_FIXTURE_FRAMES 40000u

static char s_dir[192];
static char s_small[256];  /* canonical 44-byte-header WAV */
static char s_extra[256];  /* unknown ODD-sized chunk before `data` */
static char s_big[256];	   /* > 3 * AUDIO_DEFERRED_CHUNK_BYTES of PCM */
static char s_alt[256];	   /* a second good WAV, different pattern + rate */
static char s_missing[256];	 /* never created */
static char s_trunc[256];	 /* `data` claims more than the file holds */
static char s_stereo[256];	 /* fmt says 2 channels -> not PCM16-mono */
static char s_grow[256];	 /* small at probe time, huge at load time */
/* Adversarial RIFF/WAVE shapes (case s). */
static char s_fmt18[256];	 /* `fmt ` body of 18 bytes (2 extension bytes) */
static char s_fmt40[256];	 /* `fmt ` body of 40 bytes */
static char s_two_data[256]; /* two `data` chunks -- the LAST one wins */
static char s_hdr_eof[256];	 /* file ends 4 bytes into a chunk header */
static char s_past_eof[256]; /* a chunk whose declared size runs past EOF */
static char s_wrap[256];	 /* ch_size 0xFFFFFFF0: the 32-bit wrap guard */
static char s_odd[256];		 /* odd `data` size + the word pad after it */
/* OGG: copies of the two committed fixtures, plus two truncations of the
   mono one (case q). The originals are never written to. */
static char s_ogg_mono[256];
static char s_ogg_stereo[256];
static char s_ogg_cut60[256]; /* ~60%: stb_vorbis cannot even open it */
static char s_ogg_cut85[256]; /* ~85%: opens, then decodes short */

#define OGG_SRC_MONO "tests/fixtures/audio_deferred/cave_mono.ogg"
#define OGG_SRC_STEREO "tests/fixtures/audio_deferred/cave_stereo.ogg"

static int16_t s_pcm[MAX_FIXTURE_FRAMES];
static uint8_t s_build[wavh_write_size(MAX_FIXTURE_FRAMES) + 64];
static uint8_t s_read[wavh_write_size(MAX_FIXTURE_FRAMES) + 64];
/* The committed ogg fixtures are a few KB; 64K is room to spare and a hard
   cap either way (read_ogg asserts the file fit). */
static uint8_t s_ogg_buf[64u * 1024u];
#define MAX_OGG_FRAMES 65536u
static int16_t s_ogg_ref[MAX_OGG_FRAMES];

/* Deterministic, file-distinguishing sample pattern. `bias` differs per
   fixture so "the scratch holds the LAST file" is a real assertion and not
   satisfied by two identical patterns. */
static void fill_pattern(uint32_t frames, uint32_t bias) {
	assert(frames <= MAX_FIXTURE_FRAMES);
	for(uint32_t k = 0; k < frames; k++)
		s_pcm[k] = (int16_t)(k * 31u + 7u + bias);
}

/* File I/O is kept OUT of assert() on purpose: an NDEBUG build would drop
   the call itself and silently produce no fixture (the assert-under-NDEBUG
   trap CLAUDE.md's -UNDEBUG sweep exists to fight). */
static void write_bytes(const char* path, const uint8_t* data, size_t n) {
	FILE* f = fopen(path, "wb");
	size_t wrote;
	int closed;
	assert(f);
	wrote = fwrite(data, 1, n, f);
	closed = fclose(f);
	assert(wrote == n);
	assert(closed == 0);
}

static size_t build_wav(uint32_t rate, uint32_t frames, uint32_t bias) {
	fill_pattern(frames, bias);
	size_t n = wavh_write(s_build, rate, s_pcm, frames);
	assert(n == wavh_write_size(frames));
	return n;
}

static void write_wav(const char* path, uint32_t rate, uint32_t frames,
					  uint32_t bias) {
	write_bytes(path, s_build, build_wav(rate, frames, bias));
}

static void put_u32le(uint8_t* out, size_t* o, uint32_t v) {
	for(int i = 0; i < 4; i++)
		out[(*o)++] = (uint8_t)(v >> (8 * i));
}

static void put_u16le(uint8_t* out, size_t* o, uint16_t v) {
	out[(*o)++] = (uint8_t)v;
	out[(*o)++] = (uint8_t)(v >> 8);
}

/* RIFF/WAVE carrying an UNKNOWN, ODD-sized chunk between `fmt ` and `data`.
   Exercises wavh_parse's skip-unknown + word-padding arithmetic
   (pos += 8 + ch_size + (ch_size & 1)) through the deferred probe AND the
   deferred loader's re-probe, which must agree on where `data` starts. */
static void write_wav_extra_chunk(const char* path, uint32_t rate,
								  uint32_t frames, uint32_t bias) {
	static const uint8_t note[5] = { 'c', 'a', 'v', 'e', '!' }; /* odd size */
	uint32_t data_bytes = frames * 2u;
	/* WAVE + (fmt hdr+body) + (note hdr+body+pad) + (data hdr) + pcm */
	uint32_t riff = 4u + 8u + 16u + 8u + 5u + 1u + 8u + data_bytes;
	size_t o = 0;

	fill_pattern(frames, bias);
	memcpy(s_build + o, "RIFF", 4), o += 4;
	put_u32le(s_build, &o, riff);
	memcpy(s_build + o, "WAVE", 4), o += 4;

	memcpy(s_build + o, "fmt ", 4), o += 4;
	put_u32le(s_build, &o, 16u);
	put_u16le(s_build, &o, 1u); /* PCM */
	put_u16le(s_build, &o, 1u); /* mono */
	put_u32le(s_build, &o, rate);
	put_u32le(s_build, &o, rate * 2u); /* byte rate */
	put_u16le(s_build, &o, 2u);		   /* block align */
	put_u16le(s_build, &o, 16u);	   /* bits */

	memcpy(s_build + o, "LIST", 4), o += 4;
	put_u32le(s_build, &o, 5u);
	memcpy(s_build + o, note, sizeof(note)), o += sizeof(note);
	s_build[o++] = 0; /* the word pad that makes the next chunk even */

	memcpy(s_build + o, "data", 4), o += 4;
	put_u32le(s_build, &o, data_bytes);
	for(uint32_t k = 0; k < frames; k++)
		put_u16le(s_build, &o, (uint16_t)s_pcm[k]);

	assert(o == (size_t)riff + 8u);
	write_bytes(path, s_build, o);
}

/* ---- adversarial RIFF/WAVE builders (case s) --------------------------- */

/* One shared cursor over s_build, so a fixture reads as the chunk list it
   is. Every builder below starts at begin_riff() and ends at finish_riff(). */
static size_t s_o;

static void put_tag(const char* tag) {
	memcpy(s_build + s_o, tag, 4);
	s_o += 4;
}

static void put_u32(uint32_t v) {
	put_u32le(s_build, &s_o, v);
}

static void put_u16(uint16_t v) {
	put_u16le(s_build, &s_o, v);
}

static void begin_riff(void) {
	s_o = 0;
	put_tag("RIFF");
	put_u32(0u); /* patched by finish_riff */
	put_tag("WAVE");
}

static void finish_riff(const char* path) {
	size_t at = 4;
	put_u32le(s_build, &at, (uint32_t)(s_o - 8u));
	write_bytes(path, s_build, s_o);
}

/* A `fmt ` chunk whose body is `fmt_size` bytes: the 16 standard ones both
   parsers read (PCM / mono / 16-bit / rate), then `fmt_size - 16` extension
   bytes that both must SKIP -- 18 is the cbSize form every encoder emits,
   40 the WAVE_FORMAT_EXTENSIBLE-sized one. */
static void put_fmt(uint32_t rate, uint32_t fmt_size) {
	assert(fmt_size >= 16u && fmt_size % 2u == 0u);
	put_tag("fmt ");
	put_u32(fmt_size);
	put_u16(1u); /* PCM */
	put_u16(1u); /* mono */
	put_u32(rate);
	put_u32(rate * 2u); /* byte rate */
	put_u16(2u);		/* block align */
	put_u16(16u);		/* bits */
	for(uint32_t i = 16u; i < fmt_size; i++)
		s_build[s_o++] = (uint8_t)(0xE0u + i);
}

/* A `data` chunk of `frames` samples whose declared size is `ch_size` --
   normally frames * 2, but case 13 declares one byte more. */
static void put_data(uint32_t frames, uint32_t bias, uint32_t ch_size) {
	put_tag("data");
	put_u32(ch_size);
	fill_pattern(frames, bias);
	for(uint32_t k = 0; k < frames; k++)
		put_u16((uint16_t)s_pcm[k]);
}

static void make_adversarial_wavs(void) {
	/* 9a: a 16 + 2 byte `fmt ` body. */
	begin_riff();
	put_fmt(20000u, 18u);
	put_data(410u, 610u, 410u * 2u);
	finish_riff(s_fmt18);

	/* 9b: a 16 + 24 byte one. */
	begin_riff();
	put_fmt(20500u, 40u);
	put_data(390u, 620u, 390u * 2u);
	finish_riff(s_fmt40);

	/* 10: TWO `data` chunks. Both parsers keep walking and overwrite their
	   sample pointer, so the LAST chunk is the one that wins -- the counts
	   differ on purpose, so "the last one" is a distinguishable claim. */
	begin_riff();
	put_fmt(21500u, 16u);
	put_data(300u, 630u, 300u * 2u);
	put_data(180u, 631u, 180u * 2u);
	finish_riff(s_two_data);

	/* 11a: the file STOPS 4 bytes into a chunk header. `pos + 8 <= size`
	   fails, the walk simply ends, and the fmt/data already seen stand. */
	begin_riff();
	put_fmt(23000u, 16u);
	put_data(150u, 660u, 150u * 2u);
	put_tag("LIST");
	finish_riff(s_hdr_eof);

	/* 11b: a chunk header that IS complete but claims far more body than the
	   file holds -- the bounds guard, which both parsers fail closed on. */
	begin_riff();
	put_fmt(23000u, 16u);
	put_data(150u, 670u, 150u * 2u);
	put_tag("LIST");
	put_u32(4096u); /* only 8 bytes actually follow */
	put_tag("cave");
	put_u32(0u);
	finish_riff(s_past_eof);

	/* 12: ch_size 0xFFFFFFF0. The additive bounds form (pos + 8 + ch_size >
	   size) WRAPS on a 32-bit size_t and would wave this through with a ~2G
	   sample count; the subtraction form (audio_wav.h:74, adversarial review
	   F1) rejects it. Placed AFTER a valid fmt + data, so the reject cannot
	   be mistaken for "there was nothing to find". */
	begin_riff();
	put_fmt(23000u, 16u);
	put_data(150u, 680u, 150u * 2u);
	put_tag("junk");
	put_u32(0xFFFFFFF0u);
	put_tag("cave");
	finish_riff(s_wrap);

	/* 13: an ODD `data` byte count followed by another chunk. frames is
	   ch_size / 2 (the stray byte is not a sample) and the next chunk starts
	   one pad byte later: pos += 8 + ch_size + (ch_size & 1). */
	begin_riff();
	put_fmt(21000u, 16u);
	put_data(220u, 640u, 220u * 2u + 1u);
	s_build[s_o++] = 0x7F; /* the odd trailing byte, inside `data` */
	s_build[s_o++] = 0u;   /* the word pad, outside it */
	put_tag("LIST");
	put_u32(4u);
	put_tag("cave");
	finish_riff(s_odd);
}

/* ---- the committed ogg fixtures --------------------------------------- */

/* Read a whole ogg into s_ogg_buf. The test's WORKING_DIRECTORY is the repo
   root (CMakeLists.txt's add_test), so the committed fixtures resolve as the
   relative paths above; the copies case (q) cuts resolve inside s_dir. */
static size_t read_ogg(const char* path) {
	FILE* f = fopen(path, "rb");
	size_t n;
	int closed;
	assert(f);
	n = fread(s_ogg_buf, 1, sizeof(s_ogg_buf), f);
	closed = fclose(f);
	assert(closed == 0);
	assert(n > 0 && n < sizeof(s_ogg_buf));
	return n;
}

/* Decode `path` the way the RESIDENT loader does -- one whole-file
   stb_vorbis_decode_memory plus audio_load_file's average downmix -- into
   s_ogg_ref. This is the independent side of the ogg equivalence claim: a
   different decode driver over the same bytes, never a re-run of the chunked
   loader under test. Returns the frame count. */
static uint32_t ogg_reference(const char* path, uint32_t* rate) {
	size_t n = read_ogg(path);
	int channels = 0, r = 0;
	short* dec = NULL;
	int frames
		= stb_vorbis_decode_memory(s_ogg_buf, (int)n, &channels, &r, &dec);
	assert(frames > 0 && dec != NULL);
	assert(channels == 1 || channels == 2);
	assert((uint32_t)frames <= MAX_OGG_FRAMES);
	if(channels == 1) {
		memcpy(s_ogg_ref, dec, (size_t)frames * 2u);
	} else {
		for(int k = 0; k < frames; k++)
			s_ogg_ref[k]
				= (int16_t)(((int)dec[2 * k] + (int)dec[2 * k + 1]) / 2);
	}
	free(dec);
	*rate = (uint32_t)r;
	return (uint32_t)frames;
}

static void make_ogg_fixtures(void) {
	size_t n = read_ogg(OGG_SRC_MONO);
	write_bytes(s_ogg_mono, s_ogg_buf, n);
	/* Two truncations of the SAME stream. ~60% lands inside the Vorbis setup
	   header (most of a short file), so the open itself fails; ~85% is past
	   it, so the stream opens and the failure has to come from the decode
	   ending early -- the branch hostile review 06 #3 added. */
	write_bytes(s_ogg_cut60, s_ogg_buf, n * 60u / 100u);
	write_bytes(s_ogg_cut85, s_ogg_buf, n * 85u / 100u);
	n = read_ogg(OGG_SRC_STEREO);
	write_bytes(s_ogg_stereo, s_ogg_buf, n);
}

static void make_fixtures(void) {
	int made;
	test_fixture_dir("test_audio_deferred_tmp", s_dir, sizeof(s_dir));
	made = MKDIR(s_dir);
	assert(made == 0);

	snprintf(s_small, sizeof(s_small), "%.160s/small.wav", s_dir);
	snprintf(s_extra, sizeof(s_extra), "%.160s/extra.wav", s_dir);
	snprintf(s_big, sizeof(s_big), "%.160s/big.wav", s_dir);
	snprintf(s_alt, sizeof(s_alt), "%.160s/alt.wav", s_dir);
	snprintf(s_missing, sizeof(s_missing), "%.160s/missing.wav", s_dir);
	snprintf(s_trunc, sizeof(s_trunc), "%.160s/trunc.wav", s_dir);
	snprintf(s_stereo, sizeof(s_stereo), "%.160s/stereo.wav", s_dir);
	snprintf(s_grow, sizeof(s_grow), "%.160s/grow.wav", s_dir);
	snprintf(s_fmt18, sizeof(s_fmt18), "%.160s/fmt18.wav", s_dir);
	snprintf(s_fmt40, sizeof(s_fmt40), "%.160s/fmt40.wav", s_dir);
	snprintf(s_two_data, sizeof(s_two_data), "%.160s/twodata.wav", s_dir);
	snprintf(s_hdr_eof, sizeof(s_hdr_eof), "%.160s/hdreof.wav", s_dir);
	snprintf(s_past_eof, sizeof(s_past_eof), "%.160s/pasteof.wav", s_dir);
	snprintf(s_wrap, sizeof(s_wrap), "%.160s/wrap.wav", s_dir);
	snprintf(s_odd, sizeof(s_odd), "%.160s/odd.wav", s_dir);
	snprintf(s_ogg_mono, sizeof(s_ogg_mono), "%.160s/mono.ogg", s_dir);
	snprintf(s_ogg_stereo, sizeof(s_ogg_stereo), "%.160s/stereo.ogg", s_dir);
	snprintf(s_ogg_cut60, sizeof(s_ogg_cut60), "%.160s/cut60.ogg", s_dir);
	snprintf(s_ogg_cut85, sizeof(s_ogg_cut85), "%.160s/cut85.ogg", s_dir);

	write_wav(s_small, 22050u, 1000u, 0u);
	write_wav_extra_chunk(s_extra, 32000u, 777u, 300u);
	/* 56000 PCM bytes > 3 * 16384, so the load needs >= 4 chunk steps. */
	write_wav(s_big, 32000u, 28000u, 500u);
	write_wav(s_alt, 11025u, 1500u, 900u);

	/* `data` claims 2 * 600 bytes but only half of them are on disk. */
	write_bytes(s_trunc, s_build, build_wav(16000u, 600u, 0u) - 600u);

	/* Stereo: byte 22 of the canonical layout is the channel count. */
	{
		size_t n = build_wav(24000u, 512u, 0u);
		s_build[22] = 2;
		write_bytes(s_stereo, s_build, n);
	}

	write_wav(s_grow, 22050u, 800u, 0u); /* grown mid-test by case (i) */

	make_adversarial_wavs();
	make_ogg_fixtures();
}

static void drop_fixtures(void) {
	test_fixture_dir_cleanup(s_dir);
}

static size_t read_file(const char* path) {
	FILE* f = fopen(path, "rb");
	size_t n;
	int closed;
	assert(f);
	n = fread(s_read, 1, sizeof(s_read), f);
	closed = fclose(f);
	assert(closed == 0);
	assert(n > 0 && n < sizeof(s_read));
	return n;
}

/* ------------------------------------------------------------------ */
/* Contract helpers                                                    */
/* ------------------------------------------------------------------ */

#define PUMP_LIMIT 256 /* bounded: a contract break must FAIL, never hang */

static const float s_pos[3] = { 12.5f, 64.0f, -7.25f };

static int64_t s_clock;

static void set_clock(int64_t ms) {
	s_clock = ms;
	audio_deferred_test_set_clock_ms(s_clock);
}

static void advance_clock(int64_t ms) {
	set_clock(s_clock + ms);
}

/* Reset + guards + clock, with NO scratch granted yet: case (r) attaches one
   of its own size. audio_deferred_test_reset forgets the decoder block too
   (audio_deferred.h:151-154), so s_dec_len tracks that here. */
static void begin_case_no_attach(int64_t clock0) {
	audio_deferred_test_reset();
	audio_deferred_test_fail_thread(false);
	fill_guards();
	set_clock(clock0);
	s_dec_len = 0;
	assert(audio_deferred_state() == AUDIO_DEFERRED_UNAVAILABLE);
}

static void begin_case(int64_t clock0) {
	begin_case_no_attach(clock0);
	assert(attach_scratch());
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	assert(audio_deferred_voice_slot() == -1);
}

static bool make_source(struct audio_deferred_source* src, const char* path) {
	memset(src, 0, sizeof(*src));
	snprintf(src->path, sizeof(src->path), "%.255s", path);
	src->is_ogg = false;
	return audio_deferred_probe(path, false, &src->frames, &src->rate);
}

/* The ogg counterpart: probe_ex, so the case also learns how large a decoder
   block THIS file needs. `decoder_bytes` may be NULL. */
static bool make_ogg_source(struct audio_deferred_source* src, const char* path,
							size_t* decoder_bytes) {
	memset(src, 0, sizeof(*src));
	snprintf(src->path, sizeof(src->path), "%.255s", path);
	src->is_ogg = true;
	return audio_deferred_probe_ex(path, true, &src->frames, &src->rate,
								   decoder_bytes);
}

static void pump_until_ready(void) {
	int steps = 0;
	while(audio_deferred_state() != AUDIO_DEFERRED_READY) {
		audio_deferred_loader_step();
		assert(++steps <= PUMP_LIMIT);
	}
}

/* Drive the loader until the module is IDLE again, asserting on the way
   that no job is ever handed out (the cancel / failure paths). */
static void pump_until_idle(void) {
	int steps = 0;
	while(audio_deferred_state() != AUDIO_DEFERRED_IDLE) {
		struct audio_deferred_ready r;
		assert(!audio_deferred_take_ready(&r));
		audio_deferred_loader_step();
		assert(++steps <= PUMP_LIMIT);
	}
}

/* Total loader_step calls for the current job, including the final one that
   reported "no more work". */
static int pump_loader_count(void) {
	int steps = 0;
	bool more = true;
	while(more) {
		more = audio_deferred_loader_step();
		assert(++steps <= PUMP_LIMIT);
	}
	return steps;
}

static unsigned s_outcomes_seen;
static uint32_t s_total_requests;
static uint32_t s_total_outcomes;

static int drain_records(struct audio_deferred_record* buf, int max) {
	struct audio_deferred_record r;
	int n = 0;
	while(n < max && audio_deferred_pop_record(&r)) {
		buf[n++] = r;
		s_outcomes_seen |= 1u << (unsigned)r.outcome;
	}
	assert(!audio_deferred_pop_record(&r)); /* `max` was big enough */
	return n;
}

static struct audio_deferred_record
expect_one_record(enum audio_deferred_outcome want) {
	struct audio_deferred_record buf[8];
	int n = drain_records(buf, 8);
	assert(n == 1);
	assert(buf[0].outcome == want);
	return buf[0];
}

/* Every request reconciles to exactly one terminal outcome (design S3). Safe
   at any quiescent point whether or not test_reset() clears the counters:
   both sides are read from the same snapshot. */
static void check_reconciled(void) {
	struct audio_deferred_counters c;
	uint32_t sum = 0;
	audio_deferred_counters(&c);
	for(int i = 0; i < AUDIO_DEFERRED_OUT_COUNT; i++)
		sum += c.outcome[i];
	assert(c.outcome[AUDIO_DEFERRED_OUT_NONE] == 0u);
	assert(c.requests == sum);
	assert(c.max_load_ms >= -1 && c.max_step_ms >= -1);
	s_total_requests += c.requests;
	s_total_outcomes += sum;
}

static void end_case(const char* name) {
	check_guards();
	check_decoder_guards();
	check_reconciled();
	printf("PASS %s\n", name);
}

/* The loaded PCM must equal wavh_parse's samples of the same file after the
   little-endian -> host conversion -- an independent decode of the bytes on
   disk, not a re-run of the loader. */
static void expect_scratch_matches(const char* path) {
	const int16_t* pcm = NULL;
	const int16_t* scratch = scratch_base();
	uint32_t frames = 0, rate = 0;
	size_t n = read_file(path);
	assert(wavh_parse(s_read, n, &pcm, &frames, &rate));
	assert(frames * 2u <= SCRATCH_BYTES);
	for(uint32_t k = 0; k < frames; k++)
		assert(scratch[k] == wavh_sample_from_le(pcm, k));
}

/* READY -> the voice starts on `slot` -> PLAYING. Mirrors audio.c. */
static uint32_t start_ready(const struct audio_deferred_source* src, int slot) {
	struct audio_deferred_ready rdy;
	memset(&rdy, 0, sizeof(rdy));
	assert(audio_deferred_take_ready(&rdy));
	assert(audio_deferred_state() == AUDIO_DEFERRED_READY);
	assert(rdy.pcm == scratch_base());
	assert(rdy.frames == src->frames && rdy.rate == src->rate);
	audio_deferred_note_started(rdy.req_id, slot);
	assert(audio_deferred_state() == AUDIO_DEFERRED_PLAYING);
	assert(audio_deferred_voice_slot() == slot);
	return rdy.req_id;
}

static void drain_to_idle(void) {
	struct audio_deferred_ready r;
	assert(audio_deferred_state() == AUDIO_DEFERRED_DRAINING);
	advance_clock(AUDIO_DEFERRED_DRAIN_MS);
	assert(!audio_deferred_take_ready(&r));
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	assert(audio_deferred_voice_slot() == -1);
}

/* ------------------------------------------------------------------ */
/* a. happy path                                                       */
/* ------------------------------------------------------------------ */

static void test_a_happy_path_wav(void) {
	struct audio_deferred_source src;
	struct audio_deferred_ready rdy;
	struct audio_deferred_record rec;
	uint32_t req_id;

	begin_case(1000);
	assert(make_source(&src, s_small));
	assert(src.frames == 1000u && src.rate == 22050u);

	assert(audio_deferred_request(&src, 41u, 2, s_pos, 0.75f, 1.25f));
	assert(audio_deferred_state() == AUDIO_DEFERRED_REQUESTED);
	assert(!audio_deferred_take_ready(&rdy)); /* nothing resident yet */

	advance_clock(3);
	pump_until_ready();
	assert(audio_deferred_state() == AUDIO_DEFERRED_READY);

	/* take_ready hands over the trigger-time rolls unchanged (design S1). */
	memset(&rdy, 0, sizeof(rdy));
	assert(audio_deferred_take_ready(&rdy));
	assert(rdy.pcm == scratch_base());
	assert(rdy.sound_id == 41u && rdy.variant == 2);
	assert(rdy.frames == src.frames && rdy.rate == src.rate);
	assert(rdy.vol == 0.75f && rdy.pitch == 1.25f);
	assert(rdy.pos[0] == s_pos[0] && rdy.pos[1] == s_pos[1]
		   && rdy.pos[2] == s_pos[2]);
	expect_scratch_matches(s_small);
	req_id = rdy.req_id;

	advance_clock(2);
	audio_deferred_note_started(req_id, 3);
	assert(audio_deferred_state() == AUDIO_DEFERRED_PLAYING);
	assert(audio_deferred_voice_slot() == 3);

	audio_deferred_note_voice_released(3, AUDIO_DEFERRED_OUT_COMPLETED);
	assert(audio_deferred_state() == AUDIO_DEFERRED_DRAINING);

	/* Below the margin the scratch is still off limits. */
	advance_clock(AUDIO_DEFERRED_DRAIN_MS - 1);
	assert(!audio_deferred_take_ready(&rdy));
	assert(audio_deferred_state() == AUDIO_DEFERRED_DRAINING);
	advance_clock(1);
	assert(!audio_deferred_take_ready(&rdy));
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);

	rec = expect_one_record(AUDIO_DEFERRED_OUT_COMPLETED);
	assert(rec.req_id == req_id);
	assert(rec.started);
	assert(rec.sound_id == 41u && rec.variant == 2);
	assert(rec.frames == src.frames && rec.rate == src.rate);
	assert(rec.bytes == src.frames * 2u);
	assert(rec.load_ms >= 0);
	assert(rec.start_delay_ms >= 0);

	end_case("a_happy_path_wav");
}

/* ------------------------------------------------------------------ */
/* b. multi-step load                                                  */
/* ------------------------------------------------------------------ */

static void test_b_multi_step(void) {
	struct audio_deferred_source src;
	int steps;

	begin_case(2000);
	assert(make_source(&src, s_big));
	assert(src.frames * 2u > 3u * AUDIO_DEFERRED_CHUNK_BYTES);
	assert(src.frames * 2u <= SCRATCH_BYTES);

	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	steps = pump_loader_count();
	assert(steps >= 4); /* >= one per CHUNK_BYTES of PCM */
	assert(audio_deferred_state() == AUDIO_DEFERRED_READY);
	expect_scratch_matches(s_big);

	/* Reconcile so the case leaves nothing in flight. */
	(void)start_ready(&src, 1);
	audio_deferred_note_voice_released(1, AUDIO_DEFERRED_OUT_COMPLETED);
	drain_to_idle();
	assert(expect_one_record(AUDIO_DEFERRED_OUT_COMPLETED).started);

	end_case("b_multi_step");
}

/* ------------------------------------------------------------------ */
/* c. BUSY in every non-IDLE state                                     */
/* ------------------------------------------------------------------ */

static void test_c_busy(void) {
	struct audio_deferred_source src, other;
	struct audio_deferred_record buf[16];
	uint32_t req_id;
	int n, busy = 0, completed = 0, i;

	begin_case(3000);
	assert(make_source(&src, s_big));
	assert(make_source(&other, s_alt));

	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	assert(audio_deferred_state() == AUDIO_DEFERRED_REQUESTED);
	assert(!audio_deferred_request(&other, 42u, 1, s_pos, 1.0f, 1.0f));

	assert(audio_deferred_loader_step());
	assert(audio_deferred_state() == AUDIO_DEFERRED_LOADING);
	assert(!audio_deferred_request(&other, 42u, 1, s_pos, 1.0f, 1.0f));

	pump_until_ready();
	assert(!audio_deferred_request(&other, 42u, 1, s_pos, 1.0f, 1.0f));

	req_id = start_ready(&src, 4); /* refusal #4 happens while PLAYING */
	assert(!audio_deferred_request(&other, 42u, 1, s_pos, 1.0f, 1.0f));

	audio_deferred_note_voice_released(4, AUDIO_DEFERRED_OUT_COMPLETED);
	assert(audio_deferred_state() == AUDIO_DEFERRED_DRAINING);
	assert(!audio_deferred_request(&other, 42u, 1, s_pos, 1.0f, 1.0f));

	/* The refusals never disturbed the job in flight. */
	drain_to_idle();
	expect_scratch_matches(s_big);

	n = drain_records(buf, 16);
	assert(n == 6); /* 5 BUSY + the original COMPLETED */
	for(i = 0; i < n; i++) {
		int j;
		for(j = 0; j < i; j++)
			assert(buf[j].req_id != buf[i].req_id); /* ids are unique */
		if(buf[i].outcome == AUDIO_DEFERRED_OUT_BUSY) {
			busy++;
			assert(buf[i].req_id != req_id);
			assert(!buf[i].started);
			assert(buf[i].sound_id == 42u && buf[i].variant == 1);
			assert(buf[i].start_delay_ms == -1);
		} else {
			completed++;
			assert(buf[i].outcome == AUDIO_DEFERRED_OUT_COMPLETED);
			assert(buf[i].req_id == req_id && buf[i].started);
		}
	}
	assert(busy == 5 && completed == 1);

	end_case("c_busy");
}

/* ------------------------------------------------------------------ */
/* d. cancel before the voice ever runs                                */
/* ------------------------------------------------------------------ */

/* `steps_before_cancel` < 0 means "load all the way to READY first". */
static void cancel_case(const char* path, int steps_before_cancel) {
	struct audio_deferred_source src;
	struct audio_deferred_ready rdy;
	struct audio_deferred_record rec;
	int64_t clock_at_cancel;
	int i;

	assert(make_source(&src, path));
	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	if(steps_before_cancel < 0) {
		pump_until_ready();
	} else {
		for(i = 0; i < steps_before_cancel; i++)
			audio_deferred_loader_step();
	}

	audio_deferred_cancel();
	clock_at_cancel = s_clock;

	/* No voice ever read the scratch, so no drain margin is owed: the clock
	   does not move and the module still reaches IDLE. */
	assert(!audio_deferred_take_ready(&rdy));
	pump_until_idle();
	assert(s_clock == clock_at_cancel);
	assert(audio_deferred_voice_slot() == -1);
	assert(!audio_deferred_take_ready(&rdy));

	rec = expect_one_record(AUDIO_DEFERRED_OUT_CANCELLED);
	assert(!rec.started);
	assert(rec.start_delay_ms == -1);

	/* The next request works. */
	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	pump_until_ready();
	(void)start_ready(&src, 6);
	audio_deferred_note_voice_released(6, AUDIO_DEFERRED_OUT_COMPLETED);
	drain_to_idle();
	assert(expect_one_record(AUDIO_DEFERRED_OUT_COMPLETED).started);
}

static void test_d_cancel(void) {
	begin_case(4000);
	cancel_case(s_big, 0); /* REQUESTED: the loader never touched it */
	end_case("d_cancel_requested");

	begin_case(4100);
	cancel_case(s_big, 2); /* LOADING: mid-file */
	end_case("d_cancel_loading");

	begin_case(4200);
	cancel_case(s_small, -1); /* READY: the whole sample is resident */
	end_case("d_cancel_ready");
}

/* ------------------------------------------------------------------ */
/* e. stale-epoch safety                                               */
/* ------------------------------------------------------------------ */

static const struct audio_deferred_source* s_e_src;
static int s_e_hook_ran;

/* Runs INSIDE a loader step (audio_deferred_test_set_step_hook). */
static void e_cancel_inside_step(void) {
	s_e_hook_ran++;
	audio_deferred_cancel();
	/* The step owns the scratch until its epilogue: still LOADING, and a new
	   request is refused rather than handed a scratch someone is writing. */
	assert(audio_deferred_state() == AUDIO_DEFERRED_LOADING);
	assert(!audio_deferred_request(s_e_src, 41u, 0, s_pos, 1.0f, 1.0f));
	/* A second cancel while one is pending must not double-finalize. */
	audio_deferred_cancel();
}

static void test_e_stale_epoch(void) {
	struct audio_deferred_source src;
	struct audio_deferred_record rec;
	int16_t* scratch = scratch_base();
	const uint32_t sentinel_start = SCRATCH_WORDS / 2u;
	uint32_t k;

	begin_case(5000);
	assert(make_source(&src, s_big));
	/* The big file reaches past the sentinel window, so a job that kept
	   running after the cancel WOULD overwrite it. */
	assert(src.frames > sentinel_start);

	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	assert(audio_deferred_loader_step());
	assert(audio_deferred_loader_step());
	assert(audio_deferred_state() == AUDIO_DEFERRED_LOADING);

	/* Between hand-driven steps the loader is not running, so the test may
	   stamp the not-yet-written tail of the scratch. */
	for(k = sentinel_start; k < SCRATCH_WORDS; k++)
		scratch[k] = (int16_t)0x7BCD;

	/* Lead correction (design S6): a cancel BETWEEN steps is synchronous (case
	   d covers it). The asynchronous leg is a cancel that lands while the
	   loader is INSIDE a step: the step hook runs at exactly that point, in
	   the main thread's role, with the lock free and s_in_step set. */
	s_e_src = &src;
	s_e_hook_ran = 0;
	audio_deferred_test_set_step_hook(e_cancel_inside_step);
	/* The hooked step still completes its own chunk (it ends below the
	   sentinel window), then its epilogue honours the cancel: no more work. */
	assert(!audio_deferred_loader_step());
	audio_deferred_test_set_step_hook(NULL);
	assert(s_e_hook_ran == 1);
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	assert(!audio_deferred_loader_step()); /* the dropped job never resumes */
	pump_until_idle();

	for(k = sentinel_start; k < SCRATCH_WORDS; k++)
		assert(scratch[k] == (int16_t)0x7BCD); /* not one further byte */

	{
		struct audio_deferred_record buf[8];
		int n = drain_records(buf, 8), i, cancelled = 0, busy = 0;
		assert(n == 2);
		for(i = 0; i < n; i++) {
			if(buf[i].outcome == AUDIO_DEFERRED_OUT_CANCELLED)
				cancelled++;
			else if(buf[i].outcome == AUDIO_DEFERRED_OUT_BUSY)
				busy++;
			assert(!buf[i].started);
		}
		assert(cancelled == 1 && busy == 1);
	}

	/* Only now is a new request accepted, and it loads the whole file. */
	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	pump_until_ready();
	expect_scratch_matches(s_big);
	(void)start_ready(&src, 2);
	audio_deferred_note_voice_released(2, AUDIO_DEFERRED_OUT_COMPLETED);
	drain_to_idle();
	rec = expect_one_record(AUDIO_DEFERRED_OUT_COMPLETED);
	assert(rec.started);

	end_case("e_stale_epoch");
}

/* ------------------------------------------------------------------ */
/* f. STOLEN / CANCELLED release, and the wrong-slot no-op             */
/* ------------------------------------------------------------------ */

static void released_case(enum audio_deferred_outcome why) {
	struct audio_deferred_source src;
	struct audio_deferred_record rec;
	uint32_t req_id;

	assert(make_source(&src, s_small));
	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	pump_until_ready();
	req_id = start_ready(&src, 5);

	/* A release for a DIFFERENT slot is a no-op. */
	audio_deferred_note_voice_released(7, AUDIO_DEFERRED_OUT_COMPLETED);
	assert(audio_deferred_state() == AUDIO_DEFERRED_PLAYING);
	assert(audio_deferred_voice_slot() == 5);

	audio_deferred_note_voice_released(5, why);
	assert(audio_deferred_state() == AUDIO_DEFERRED_DRAINING);
	/* The margin is owed here: a voice DID read the scratch. */
	advance_clock(AUDIO_DEFERRED_DRAIN_MS - 1);
	{
		struct audio_deferred_ready r;
		assert(!audio_deferred_take_ready(&r));
	}
	assert(audio_deferred_state() == AUDIO_DEFERRED_DRAINING);
	advance_clock(1);
	{
		struct audio_deferred_ready r;
		assert(!audio_deferred_take_ready(&r));
	}
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);

	rec = expect_one_record(why);
	assert(rec.req_id == req_id);
	assert(rec.started);
	assert(rec.start_delay_ms >= 0);
}

static void test_f_released_steal_cancel(void) {
	begin_case(6000);
	released_case(AUDIO_DEFERRED_OUT_STOLEN);
	end_case("f_released_stolen");

	begin_case(6100);
	released_case(AUDIO_DEFERRED_OUT_CANCELLED);
	end_case("f_released_cancelled");
}

/* ------------------------------------------------------------------ */
/* g. repeated steal / reuse                                           */
/* ------------------------------------------------------------------ */

static void test_g_repeat_steal_reuse(void) {
	struct audio_deferred_source src;
	uint32_t ids[5];
	const char* last = NULL;
	int i;

	begin_case(7000);
	for(i = 0; i < 5; i++) {
		const char* path = (i & 1) ? s_alt : s_small;
		struct audio_deferred_record rec;
		int j;

		assert(make_source(&src, path));
		assert(audio_deferred_request(&src, (uint16_t)(50 + i), i, s_pos,
									  1.0f, 1.0f));
		pump_until_ready();
		ids[i] = start_ready(&src, 0);
		audio_deferred_note_voice_released(0, AUDIO_DEFERRED_OUT_STOLEN);
		drain_to_idle();

		rec = expect_one_record(AUDIO_DEFERRED_OUT_STOLEN);
		assert(rec.req_id == ids[i] && rec.started);
		assert(rec.sound_id == (uint16_t)(50 + i) && rec.variant == i);
		for(j = 0; j < i; j++)
			assert(ids[j] != ids[i]); /* monotonic, never recycled */
		assert(i == 0 || ids[i] > ids[i - 1]);

		expect_scratch_matches(path); /* the scratch holds THIS file */
		last = path;
		check_guards();
	}
	assert(last != NULL);
	expect_scratch_matches(last); /* ...and after five reuses, the LAST one */

	end_case("g_repeat_steal_reuse");
}

/* ------------------------------------------------------------------ */
/* h. CULLED (muted / backend dead before the start)                   */
/* ------------------------------------------------------------------ */

static void test_h_culled(void) {
	struct audio_deferred_source src;
	struct audio_deferred_ready rdy;
	struct audio_deferred_record rec;
	int64_t clock_at_cull;

	begin_case(8000);
	assert(make_source(&src, s_small));
	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	pump_until_ready();

	memset(&rdy, 0, sizeof(rdy));
	assert(audio_deferred_take_ready(&rdy));
	advance_clock(4);
	clock_at_cull = s_clock;
	audio_deferred_note_not_started(rdy.req_id, AUDIO_DEFERRED_OUT_CULLED);

	/* No voice ever read the scratch -> no drain margin, IDLE at once. */
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	assert(s_clock == clock_at_cull);
	assert(audio_deferred_voice_slot() == -1);

	rec = expect_one_record(AUDIO_DEFERRED_OUT_CULLED);
	assert(rec.req_id == rdy.req_id);
	assert(!rec.started);
	assert(rec.start_delay_ms == -1);
	assert(rec.load_ms >= 0); /* it DID finish loading */

	/* The module is reusable straight away. */
	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	pump_until_ready();
	(void)start_ready(&src, 1);
	audio_deferred_note_voice_released(1, AUDIO_DEFERRED_OUT_COMPLETED);
	drain_to_idle();
	assert(expect_one_record(AUDIO_DEFERRED_OUT_COMPLETED).started);

	end_case("h_culled");
}

/* ------------------------------------------------------------------ */
/* i. failure paths                                                    */
/* ------------------------------------------------------------------ */

static void failing_load(const struct audio_deferred_source* src) {
	struct audio_deferred_record rec;
	assert(audio_deferred_request(src, 41u, 0, s_pos, 1.0f, 1.0f));
	pump_until_idle(); /* asserts take_ready never fires on the way */
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	rec = expect_one_record(AUDIO_DEFERRED_OUT_FAILED);
	assert(!rec.started);
	assert(rec.start_delay_ms == -1);
	check_guards();
}

static void test_i_failures(void) {
	struct audio_deferred_source src;

	/* 1. the file is not there at all */
	begin_case(9000);
	memset(&src, 0, sizeof(src));
	snprintf(src.path, sizeof(src.path), "%.255s", s_missing);
	src.frames = 100u;
	src.rate = 22050u;
	assert(!audio_deferred_probe(s_missing, false, &src.frames, &src.rate));
	src.frames = 100u;
	src.rate = 22050u;
	failing_load(&src);
	end_case("i_fail_missing");

	/* 2. `data` claims more PCM than the file holds */
	begin_case(9100);
	memset(&src, 0, sizeof(src));
	snprintf(src.path, sizeof(src.path), "%.255s", s_trunc);
	src.frames = 600u;
	src.rate = 16000u;
	failing_load(&src);
	end_case("i_fail_truncated");

	/* 3. not PCM16 MONO */
	begin_case(9200);
	memset(&src, 0, sizeof(src));
	snprintf(src.path, sizeof(src.path), "%.255s", s_stereo);
	src.frames = 512u;
	src.rate = 24000u;
	failing_load(&src);
	end_case("i_fail_stereo");

	/* 4. the file GREW between the scan probe and the load: the re-probe
	      must refuse it rather than write past the scratch capacity. */
	begin_case(9300);
	assert(make_source(&src, s_grow));
	assert(src.frames == 800u);
	write_wav(s_grow, 22050u, MAX_FIXTURE_FRAMES, 0u);
	assert(MAX_FIXTURE_FRAMES * 2u > SCRATCH_BYTES);
	failing_load(&src);
	write_wav(s_grow, 22050u, 800u, 0u); /* restore for case (l) */
	end_case("i_fail_grew_past_capacity");

	/* 5. no source at all -- refused synchronously, per the header */
	begin_case(9400);
	{
		struct audio_deferred_record rec;
		assert(!audio_deferred_request(NULL, 41u, 0, s_pos, 1.0f, 1.0f));
		assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
		rec = expect_one_record(AUDIO_DEFERRED_OUT_FAILED);
		assert(!rec.started);
	}
	end_case("i_fail_null_source");
}

/* ------------------------------------------------------------------ */
/* j. UNAVAILABLE                                                      */
/* ------------------------------------------------------------------ */

static void test_j_unavailable(void) {
	struct audio_deferred_source src;
	struct audio_deferred_record rec;

	audio_deferred_test_reset();
	audio_deferred_test_fail_thread(false);
	fill_guards();
	set_clock(10000);
	assert(make_source(&src, s_small));

	/* No scratch -> UNAVAILABLE, and a request fails (not BUSY). */
	assert(!audio_deferred_attach(NULL, 0));
	assert(audio_deferred_state() == AUDIO_DEFERRED_UNAVAILABLE);
	assert(!audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	rec = expect_one_record(AUDIO_DEFERRED_OUT_FAILED);
	assert(!rec.started);

	/* A good attach recovers. */
	assert(attach_scratch());
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);

	/* Loader-thread creation failure -> UNAVAILABLE again. */
	audio_deferred_quiesce();
	assert(audio_deferred_state() == AUDIO_DEFERRED_UNAVAILABLE);
	audio_deferred_test_fail_thread(true);
	assert(!attach_scratch());
	assert(audio_deferred_state() == AUDIO_DEFERRED_UNAVAILABLE);
	assert(!audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	assert(expect_one_record(AUDIO_DEFERRED_OUT_FAILED).outcome
		   == AUDIO_DEFERRED_OUT_FAILED);

	/* ...and a later good attach recovers, all the way to a played sound. */
	audio_deferred_test_fail_thread(false);
	assert(attach_scratch());
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	pump_until_ready();
	(void)start_ready(&src, 3);
	audio_deferred_note_voice_released(3, AUDIO_DEFERRED_OUT_COMPLETED);
	drain_to_idle();
	assert(expect_one_record(AUDIO_DEFERRED_OUT_COMPLETED).started);

	end_case("j_unavailable");
}

/* ------------------------------------------------------------------ */
/* k. quiesce out of LOADING                                           */
/* ------------------------------------------------------------------ */

static void test_k_quiesce(void) {
	static int16_t snapshot[SCRATCH_WORDS];
	struct audio_deferred_source src;
	struct audio_deferred_record rec;
	int i;

	begin_case(11000);
	assert(make_source(&src, s_big));
	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	assert(audio_deferred_loader_step());
	assert(audio_deferred_loader_step());
	assert(audio_deferred_state() == AUDIO_DEFERRED_LOADING);

	audio_deferred_quiesce();
	assert(audio_deferred_state() == AUDIO_DEFERRED_UNAVAILABLE);
	rec = expect_one_record(AUDIO_DEFERRED_OUT_CANCELLED);
	assert(!rec.started);

	/* After the quiesce the arena is the caller's again: the loader must be
	   inert and must not write one word of the scratch. */
	memcpy(snapshot, scratch_base(), sizeof(snapshot));
	for(i = 0; i < 8; i++) {
		assert(!audio_deferred_loader_step());
		assert(audio_deferred_state() == AUDIO_DEFERRED_UNAVAILABLE);
	}
	assert(memcmp(snapshot, scratch_base(), sizeof(snapshot)) == 0);
	assert(!audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	assert(expect_one_record(AUDIO_DEFERRED_OUT_FAILED).outcome
		   == AUDIO_DEFERRED_OUT_FAILED);

	/* Attaching again recovers. */
	assert(attach_scratch());
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	pump_until_ready();
	expect_scratch_matches(s_big);
	(void)start_ready(&src, 2);
	audio_deferred_note_voice_released(2, AUDIO_DEFERRED_OUT_COMPLETED);
	drain_to_idle();
	assert(expect_one_record(AUDIO_DEFERRED_OUT_COMPLETED).started);

	end_case("k_quiesce");
}

/* ------------------------------------------------------------------ */
/* l. probe vs wavh_parse                                              */
/* ------------------------------------------------------------------ */

static void probe_agrees(const char* path) {
	const int16_t* pcm = NULL;
	uint32_t pf = 0, pr = 0, wf = 0, wr = 0;
	size_t n;
	assert(audio_deferred_probe(path, false, &pf, &pr));
	n = read_file(path);
	assert(wavh_parse(s_read, n, &pcm, &wf, &wr));
	assert(pf == wf);
	assert(pr == wr);
}

static void probe_rejects(const char* path) {
	uint32_t f = 0xDEADu, r = 0xBEEFu;
	assert(!audio_deferred_probe(path, false, &f, &r));
}

static void test_l_probe(void) {
	begin_case(12000);
	probe_agrees(s_small);
	probe_agrees(s_extra); /* unknown odd chunk + word pad */
	probe_agrees(s_big);
	probe_agrees(s_alt);
	probe_agrees(s_grow);
	probe_rejects(s_missing);
	probe_rejects(s_trunc);
	probe_rejects(s_stereo);
	probe_rejects(s_dir); /* a directory is not a WAV */

	/* The extra-chunk file also LOADS, byte for byte. */
	{
		struct audio_deferred_source src;
		assert(make_source(&src, s_extra));
		assert(src.frames == 777u && src.rate == 32000u);
		assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
		pump_until_ready();
		expect_scratch_matches(s_extra);
		(void)start_ready(&src, 1);
		audio_deferred_note_voice_released(1, AUDIO_DEFERRED_OUT_COMPLETED);
		drain_to_idle();
		assert(expect_one_record(AUDIO_DEFERRED_OUT_COMPLETED).started);
	}

	end_case("l_probe");
}

/* ------------------------------------------------------------------ */
/* n. OGG happy path, mono and stereo                                  */
/* ------------------------------------------------------------------ */

/* The scratch after a finished load must equal the RESIDENT decode of the
   same file: same frame count, same rate, same bytes. Returns the frames. */
static uint32_t expect_scratch_matches_ogg(const int16_t* pcm, const char* path,
										   uint32_t frames, uint32_t rate) {
	uint32_t ref_rate = 0;
	uint32_t ref_frames = ogg_reference(path, &ref_rate);
	assert(ref_frames == frames);
	assert(ref_rate == rate);
	assert(memcmp(pcm, s_ogg_ref, (size_t)ref_frames * 2u) == 0);
	return ref_frames;
}

static void ogg_happy(const char* path, const char* label) {
	struct audio_deferred_source src;
	struct audio_deferred_ready rdy;
	struct audio_deferred_record rec;
	size_t decoder = 0;
	int steps;

	assert(make_ogg_source(&src, path, &decoder));
	assert(src.frames > 0u && src.rate > 0u);
	/* The probe's own promise about the block it will need (design: a variant
	   needing more than the ceiling is never eligible). */
	assert(decoder > 0u && decoder <= AUDIO_DEFERRED_MAX_DECODER_BYTES);
	assert(src.frames * 2u <= SCRATCH_BYTES);
	arm_decoder(decoder);

	assert(audio_deferred_request(&src, 41u, 3, s_pos, 0.5f, 0.75f));
	steps = pump_loader_count();
	/* The decode is CHUNKED: the open is its own step and each burst is
	   bounded by the module's static output buffer, which is smaller than
	   AUDIO_DEFERRED_CHUNK_BYTES -- so a ~44KB sample cannot be one step. */
	assert(steps >= 4);
	assert(audio_deferred_state() == AUDIO_DEFERRED_READY);

	memset(&rdy, 0, sizeof(rdy));
	assert(audio_deferred_take_ready(&rdy));
	assert(rdy.pcm == scratch_base());
	assert(rdy.sound_id == 41u && rdy.variant == 3);
	/* What was loaded is exactly what the scan-time probe promised. */
	assert(rdy.frames == src.frames);
	assert(rdy.rate == src.rate);
	assert(rdy.vol == 0.5f && rdy.pitch == 0.75f);
	expect_scratch_matches_ogg(rdy.pcm, path, rdy.frames, rdy.rate);

	audio_deferred_note_started(rdy.req_id, 2);
	assert(audio_deferred_state() == AUDIO_DEFERRED_PLAYING);
	audio_deferred_note_voice_released(2, AUDIO_DEFERRED_OUT_COMPLETED);
	drain_to_idle();

	rec = expect_one_record(AUDIO_DEFERRED_OUT_COMPLETED);
	assert(rec.req_id == rdy.req_id && rec.started);
	assert(rec.frames == src.frames && rec.rate == src.rate);
	assert(rec.bytes == src.frames * 2u);
	printf("    %s: %u frames @ %u Hz, %u decoder bytes, %d loader steps\n",
		   label, (unsigned)src.frames, (unsigned)src.rate, (unsigned)decoder,
		   steps);
}

static void test_n_ogg_happy(void) {
	begin_case(13000);
	ogg_happy(s_ogg_mono, "mono");
	end_case("n_ogg_happy_mono");

	/* Stereo exercises the average downmix, which the fixture's different
	   per-channel content makes a real assertion (fixtures README). */
	begin_case(13100);
	ogg_happy(s_ogg_stereo, "stereo");
	end_case("n_ogg_happy_stereo");
}

/* ------------------------------------------------------------------ */
/* o. an OGG with NO decoder block                                     */
/* ------------------------------------------------------------------ */

static void test_o_ogg_no_decoder(void) {
	struct audio_deferred_source src, wav;

	begin_case(13200);
	assert(make_ogg_source(&src, s_ogg_mono, NULL));
	/* Deliberately no arm_decoder: exactly the state a scan that reserved no
	   block leaves behind. The loader must NOT fall back to a malloc. */
	failing_load(&src);
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);

	/* ...and a WAV, which needs no block at all, still works right after. */
	assert(make_source(&wav, s_small));
	assert(audio_deferred_request(&wav, 41u, 0, s_pos, 1.0f, 1.0f));
	pump_until_ready();
	expect_scratch_matches(s_small);
	(void)start_ready(&wav, 1);
	audio_deferred_note_voice_released(1, AUDIO_DEFERRED_OUT_COMPLETED);
	drain_to_idle();
	assert(expect_one_record(AUDIO_DEFERRED_OUT_COMPLETED).started);

	end_case("o_ogg_no_decoder");
}

/* ------------------------------------------------------------------ */
/* p. an OGG with a decoder block that is too small                    */
/* ------------------------------------------------------------------ */

static void test_p_ogg_small_decoder(void) {
	struct audio_deferred_source src;
	size_t needed = 0;

	begin_case(13300);
	assert(make_ogg_source(&src, s_ogg_mono, &needed));
	assert(needed > 1024u); /* anti-vacuity: 1KB really is short */
	arm_decoder(1024u);
	failing_load(&src);
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	/* The bytes just past the small block are untouched (end_case checks the
	   same guards again, after the scratch ones). */
	check_decoder_guards();

	end_case("p_ogg_small_decoder");
}

/* ------------------------------------------------------------------ */
/* q. truncated / short OGG streams                                    */
/* ------------------------------------------------------------------ */

/* A source pointing at `path` but carrying the FULL file's scan-time frame
   count -- the shape a variant takes when the file on the card changed after
   the scan probed it. */
static void short_ogg_request(const char* path, uint32_t full_frames,
							  uint32_t full_rate) {
	struct audio_deferred_source src;
	memset(&src, 0, sizeof(src));
	snprintf(src.path, sizeof(src.path), "%.255s", path);
	src.is_ogg = true;
	src.frames = full_frames;
	src.rate = full_rate;
	failing_load(&src);
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
}

static void test_q_ogg_truncated(void) {
	uint32_t full_frames = 0, full_rate = 0;
	uint32_t f = 0xDEADu, r = 0xBEEFu;
	size_t decoder = 0;

	begin_case(13400);
	assert(audio_deferred_probe_ex(s_ogg_mono, true, &full_frames, &full_rate,
								   &decoder));
	arm_decoder(decoder); /* sized for the WHOLE file: not the limiting factor */

	/* q1: cut inside the Vorbis setup header. The probe refuses it outright,
	   and a request that still carries the full frame count FAILS. */
	assert(!audio_deferred_probe(s_ogg_cut60, true, &f, &r));
	short_ogg_request(s_ogg_cut60, full_frames, full_rate);

	/* q2: cut past the setup header. stb_vorbis OPENS this one, so the load
	   gets as far as decoding real audio and then runs out -- fewer frames
	   than promised must still be FAILED, never a READY that plays short
	   (hostile review 06 #3, audio_deferred.c:342-346). */
	f = 0xDEADu;
	r = 0xBEEFu;
	assert(!audio_deferred_probe(s_ogg_cut85, true, &f, &r));
	short_ogg_request(s_ogg_cut85, full_frames, full_rate);

	/* q3: the INTACT file, asked for more frames than it holds -- the same
	   rule with the stream itself above suspicion. */
	assert(full_frames + 1000u > full_frames);
	short_ogg_request(s_ogg_mono, full_frames + 1000u, full_rate);

	/* The module is still good afterwards, and still byte-exact. */
	{
		struct audio_deferred_source src;
		struct audio_deferred_ready rdy;
		assert(make_ogg_source(&src, s_ogg_mono, NULL));
		assert(src.frames == full_frames);
		assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
		pump_until_ready();
		memset(&rdy, 0, sizeof(rdy));
		assert(audio_deferred_take_ready(&rdy));
		assert(rdy.frames == full_frames && rdy.rate == full_rate);
		expect_scratch_matches_ogg(rdy.pcm, s_ogg_mono, rdy.frames, rdy.rate);
		audio_deferred_note_not_started(rdy.req_id, AUDIO_DEFERRED_OUT_CULLED);
		assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
		(void)expect_one_record(AUDIO_DEFERRED_OUT_CULLED);
	}

	end_case("q_ogg_truncated");
}

/* ------------------------------------------------------------------ */
/* r. the scratch capacity edges, on the OGG path                      */
/* ------------------------------------------------------------------ */

static void test_r_ogg_scratch_edges(void) {
	struct audio_deferred_source src;
	struct audio_deferred_ready rdy;
	size_t decoder = 0;
	size_t exact;

	/* r1: a scratch EXACTLY the size the real scan would reserve for this
	   variant -- frames * 2 rounded up to the DSP alignment, not one byte
	   more. The ogg leg has no up-front frame count to bound itself by, so
	   this is the case that proves it stops on the capacity. */
	begin_case_no_attach(13500);
	assert(make_ogg_source(&src, s_ogg_stereo, &decoder));
	exact = ((size_t)src.frames * 2u + (AUDIO_DEFERRED_ALIGN - 1u))
		& ~(size_t)(AUDIO_DEFERRED_ALIGN - 1u);
	assert(attach_scratch_bytes(exact));
	arm_decoder(decoder);
	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	pump_until_ready();
	memset(&rdy, 0, sizeof(rdy));
	assert(audio_deferred_take_ready(&rdy));
	assert(rdy.frames == src.frames && rdy.rate == src.rate);
	expect_scratch_matches_ogg(rdy.pcm, s_ogg_stereo, rdy.frames, rdy.rate);
	audio_deferred_note_not_started(rdy.req_id, AUDIO_DEFERRED_OUT_CULLED);
	(void)expect_one_record(AUDIO_DEFERRED_OUT_CULLED);
	end_case("r_ogg_scratch_exact");

	/* r2: one alignment unit less. The sample no longer fits, so the load
	   must FAIL rather than publish a truncated one -- and the granted region
	   ends flush against the guard words, so a single byte written past the
	   capacity is caught. */
	begin_case_no_attach(13600);
	assert(make_ogg_source(&src, s_ogg_stereo, &decoder));
	assert(exact - AUDIO_DEFERRED_ALIGN < (size_t)src.frames * 2u);
	assert(attach_scratch_bytes(exact - AUDIO_DEFERRED_ALIGN));
	arm_decoder(decoder);
	failing_load(&src);
	assert(audio_deferred_state() == AUDIO_DEFERRED_IDLE);
	end_case("r_ogg_scratch_one_align_short");
}

/* ------------------------------------------------------------------ */
/* s. adversarial WAV probe fixtures                                   */
/* ------------------------------------------------------------------ */

/* wavh_parse must REFUSE exactly what audio_deferred_probe refused: the two
   are ONE parser walked two ways (audio_wav.h:99-106), so accept/reject can
   never diverge. */
static void wavh_rejects(const char* path) {
	const int16_t* pcm = NULL;
	uint32_t f = 0, r = 0;
	size_t n = read_file(path);
	assert(!wavh_parse(s_read, n, &pcm, &f, &r));
}

/* ...and for an accepted one, the whole deferred load has to reproduce
   wavh_parse's samples in host order, byte for byte. */
static void loads_identically(const char* path) {
	struct audio_deferred_source src;
	assert(make_source(&src, path));
	assert(src.frames > 0u && src.frames * 2u <= SCRATCH_BYTES);
	assert(audio_deferred_request(&src, 41u, 0, s_pos, 1.0f, 1.0f));
	pump_until_ready();
	expect_scratch_matches(path);
	(void)start_ready(&src, 1);
	audio_deferred_note_voice_released(1, AUDIO_DEFERRED_OUT_COMPLETED);
	drain_to_idle();
	assert(expect_one_record(AUDIO_DEFERRED_OUT_COMPLETED).started);
}

static void test_s_wav_adversarial(void) {
	const int16_t* pcm = NULL;
	uint32_t f = 0, r = 0;
	size_t n;

	begin_case(13700);

	/* 9: `fmt ` bodies longer than the 16 bytes either parser reads. */
	probe_agrees(s_fmt18);
	loads_identically(s_fmt18);
	probe_agrees(s_fmt40);
	loads_identically(s_fmt40);

	/* 10: two `data` chunks -- the LAST one wins, in both parsers. The frame
	   count proves WHICH one, since the two differ. */
	probe_agrees(s_two_data);
	n = read_file(s_two_data);
	assert(wavh_parse(s_read, n, &pcm, &f, &r));
	assert(f == 180u && r == 21500u); /* the second chunk, not the first */
	loads_identically(s_two_data);

	/* 11: a chunk header straddling EOF just ends the walk... */
	probe_agrees(s_hdr_eof);
	loads_identically(s_hdr_eof);
	/* ...while a declared size that runs past EOF is a reject, both sides. */
	probe_rejects(s_past_eof);
	wavh_rejects(s_past_eof);

	/* 12: the 32-bit wrap guard. */
	probe_rejects(s_wrap);
	wavh_rejects(s_wrap);

	/* 13: an odd `data` size, and a chunk after the word pad. */
	probe_agrees(s_odd);
	n = read_file(s_odd);
	assert(wavh_parse(s_read, n, &pcm, &f, &r));
	assert(f == 220u && r == 21000u); /* ch_size / 2, the stray byte dropped */
	loads_identically(s_odd);

	end_case("s_wav_adversarial");
}

/* ------------------------------------------------------------------ */
/* m. accounting                                                       */
/* ------------------------------------------------------------------ */

static void test_m_accounting(void) {
	/* end_case() already asserted requests == sum(outcome[]) after EVERY
	   case; this is the aggregate, plus the proof that all six terminal
	   outcomes were actually exercised (an always-BUSY module would satisfy
	   the equality on its own). */
	unsigned want = (1u << AUDIO_DEFERRED_OUT_BUSY)
		| (1u << AUDIO_DEFERRED_OUT_CULLED)
		| (1u << AUDIO_DEFERRED_OUT_FAILED)
		| (1u << AUDIO_DEFERRED_OUT_CANCELLED)
		| (1u << AUDIO_DEFERRED_OUT_STOLEN)
		| (1u << AUDIO_DEFERRED_OUT_COMPLETED);

	assert(s_total_requests > 0u);
	assert(s_total_requests == s_total_outcomes);
	assert((s_outcomes_seen & want) == want);
	assert((s_outcomes_seen & (1u << AUDIO_DEFERRED_OUT_NONE)) == 0u);
	check_guards();
	printf("PASS m_accounting\n");
}

/* ------------------------------------------------------------------ */

int main(void) {
	make_fixtures();

	test_a_happy_path_wav();
	test_b_multi_step();
	test_c_busy();
	test_d_cancel();
	test_e_stale_epoch();
	test_f_released_steal_cancel();
	test_g_repeat_steal_reuse();
	test_h_culled();
	test_i_failures();
	test_j_unavailable();
	test_k_quiesce();
	test_l_probe();
	test_n_ogg_happy();
	test_o_ogg_no_decoder();
	test_p_ogg_small_decoder();
	test_q_ogg_truncated();
	test_r_ogg_scratch_edges();
	test_s_wav_adversarial();
	test_m_accounting(); /* the aggregate: stays last */

	audio_deferred_shutdown();
	drop_fixtures();
	printf("ALL PASS test_audio_deferred\n");
	return 0;
}
