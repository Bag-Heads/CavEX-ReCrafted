/*
	S0 audio foundation env test (the test_animal_interact_env harness
	pattern: threadless server, direct RPC processing, an rpc observer on the
	server->client sends). Decompile 740c583 is the spec for every constant.

	Coverage:
	  - Playback math goldens vs the Beta formulas (audio_mix.h): 16-block
	    base range LINEARLY extended by vol>1 (explosion 4.0F = 64 blocks),
	    linear model-2 falloff, loudness clamp-to-1 * master, the playSoundFX
	    quarter-volume UI path, UNCLAMPED pitch -> playback rate.
	  - WAV parser golden: an in-memory generated PCM16-mono RIFF (rate /
	    frames / first-sample values incl. the little-endian conversion);
	    malformed / stereo / 8-bit inputs rejected.
	  - StepSound tables: the break-vs-place name TRAP (glass breaks as
	    random.glass but places/steps as step.stone; sand breaks as
	    step.gravel), snow->cloth, metal = stone pitched 1.5F, the powder
	    default for unassigned ids (pistons/bed/web), stairs copying their
	    model block, air -> AUDIO_SOUND_NONE.
	  - Mob sound table: the 8-mob idle/hurt/death rows (skeleton death==hurt,
	    creeper NO idle, cow volume 0.4F -- the only override), silent squid
	    (get*Sound return null in b1.7.3) and silent-slime S0 deferral.
	  - Registry mapping goldens (HEADLESS audio_init(false, ...): registry +
	    arena load with NO device -- documented in audio.h): slash-to-dot key
	    -> path mapping, the digit-strip rule in reverse (grass1.wav resolves
	    "step.grass"; bare chickenplop.wav resolves "mob.chickenplop"), EVERY
	    S0 id resolves at least one placeholder via the per-file fallback,
	    variant PCM decoded into the fixed arena (arena_used > 0), uniform
	    variant pick returns a loaded sample.
	  - CRPC_SOUND emission through the env server: block BREAK (aux-2001
	    break-name (v+1)/2 @ p*0.8), block PLACE (STEP name -- glass pins the
	    trap end-to-end), explosion (random.explode CONSTANT 4.0F,
	    (1+(r-r)*0.2)*0.7), TNT prime (random.fuse 1.0/1.0), chicken egg lay
	    (mob.chickenplop 1.0, wobble), mob hurt/death via the fresh-only
	    attackEntityFrom rule (zombie hurt then death edge; cow 0.4F; a fresh
	    ZERO-damage egg-style hit still plays hurt), player random.hurt on
	    fresh damage only (the i-frame absorb window is silent).
	  - S1 transcoder core (tools/sound_transcode.c compiled in-process,
	    NO_MAIN + header-only vorbis): WAV->WAV byte identity vs this file's
	    INDEPENDENT wav_build golden (odd frame count), extra-chunk
	    normalization to the canonical 44-byte layout, the engine parser's
	    reject set incl. the review-F1 crafted-size vector + stereo/8-bit
	    (wav-stereo is REJECTED, not downmixed -- the engine's own rule, so
	    the ogg-only stereo downmix is exercised by the real-pack run, not
	    synthesizable here without an encoder), and --manifest rows matching
	    the shared table for EVERY id.
	  - S1 pack-reload drain (headless): scratch pack overriding step.grass
	    -> audio_registry_reload resolves it per-KEY (exactly ONE variant, no
	    placeholder leak), every other key still falls back, arena reset in
	    place; reload BACK to defaults re-fills the bump arena to the exact
	    same byte count. Optional CAVEWII_S1_REAL_PACK=<dir> section prints a
	    per-key table from a locally built (never committed) Beta pack.
	  - S2 (D2b + E8..E17): table/manifest completeness for the 14 S2 keys +
	    the raised 14-variant cap (a cave1..cave13 scratch pack resolves 13);
	    the player footstep odometer goldens (one step per 0.6-weighted unit,
	    sneak freeze, snow-layer->cloth, liquid silence); the livingSoundTime
	    idle ramp's deterministic first-fire tick (hand-run RNG clone) + cow
	    0.4F; slime size gates (jump >1 / land >2 via the disjoint *0.8 vs
	    /0.8 pitch windows, hurt/death 0.6F, mob.slimeattack only on an
	    ACCEPTED contact hit); piston extend/retract sounds through the real
	    torch-powered move; the harden + nether-bucket random.fizz (from the
	    stream-parity rand_src draws); item/arrow random.pop; arrow-stick
	    random.drr; and the random.splash velocity formula + dry->wet mob
	    latch.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
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

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "block/fluid_block.h" /* S2: the harden-fizz emit under test */
#include "config.h"
#include "game/game_state.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/entity_primed_tnt.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "item/items.h"
#include "item/items_object.h" /* S2: item_bucket_water for the nether evap */
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_world.h"
#include "platform/audio.h"
#include "platform/audio_mix.h"
#include "platform/thread.h"
#include "sound_transcode.h" /* the S1 tool core, compiled into this target */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_audio_env_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
static char s_fixture_TMP_PACK[128]; /* was #define TMP_PACK='test_audio_env_pack_tmp' (a literal) */
#define TMP_PACK s_fixture_TMP_PACK /* scratch sound pack (gitignored) */

extern struct inventory_logic inventory_logic_player;

static struct server_local s;
static volatile int g_drain_run = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

/* CRPC_SOUND capture: last event + total count, plus (S2) per-id counters
   with each id's LAST vol/pitch/ui so multi-sound ticks (a walking mob's
   step beside its idle grunt) can be asserted independently. */
static size_t g_snd_count = 0;
static uint16_t g_snd_id = AUDIO_SOUND_NONE;
static float g_snd_vol, g_snd_pitch;
static vec3 g_snd_pos;
static size_t g_snd_by_id[AUDIO_SOUND_COUNT];
static float g_snd_vol_by_id[AUDIO_SOUND_COUNT];
static float g_snd_pitch_by_id[AUDIO_SOUND_COUNT];
static bool g_snd_ui_by_id[AUDIO_SOUND_COUNT];

/* LIVING-SOUND-RNG: when armed, the observer snapshots a watched mob's
   gameplay_rng at the instant a matching sound is emitted, so a leg can
   step the 48-bit LCG BACK and prove which draws produced the pitch. */
static const struct java_random* g_watch_rng = NULL;
static uint64_t g_watch_seed_at_emit = 0;
static size_t g_watch_hits = 0;
static uint16_t g_watch_sid_lo = AUDIO_SOUND_NONE, g_watch_sid_hi = AUDIO_SOUND_NONE;

static void capture_sound(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type != CRPC_SOUND)
		return;
	if(g_watch_rng && call->payload.sound.sound_id >= g_watch_sid_lo
	   && call->payload.sound.sound_id <= g_watch_sid_hi) {
		g_watch_seed_at_emit = g_watch_rng->seed;
		g_watch_hits++;
	}
	g_snd_count++;
	g_snd_id = call->payload.sound.sound_id;
	g_snd_vol = call->payload.sound.vol;
	g_snd_pitch = call->payload.sound.pitch;
	glm_vec3_copy((float*)call->payload.sound.pos, g_snd_pos);
	if(g_snd_id < AUDIO_SOUND_COUNT) {
		g_snd_by_id[g_snd_id]++;
		g_snd_vol_by_id[g_snd_id] = g_snd_vol;
		g_snd_pitch_by_id[g_snd_id] = g_snd_pitch;
		g_snd_ui_by_id[g_snd_id] = call->payload.sound.ui;
	}
}

static void snd_reset(void) {
	g_snd_count = 0;
	g_snd_id = AUDIO_SOUND_NONE;
	memset(g_snd_by_id, 0, sizeof(g_snd_by_id));
	memset(g_snd_vol_by_id, 0, sizeof(g_snd_vol_by_id));
	memset(g_snd_pitch_by_id, 0, sizeof(g_snd_pitch_by_id));
	memset(g_snd_ui_by_id, 0, sizeof(g_snd_ui_by_id));
}

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-5F;
}

/* java.util.Random LCG helpers (48-bit): forward/back one step and the
   value a step produced. 0xDFE05BCB1365 is 0x5DEECE66D^-1 mod 2^48. */
#define JR_MASK ((UINT64_C(1) << 48) - 1)
static uint64_t jr_fwd(uint64_t x) {
	return (x * UINT64_C(0x5DEECE66D) + UINT64_C(0xB)) & JR_MASK;
}
static uint64_t jr_back(uint64_t x) {
	return ((x - UINT64_C(0xB)) * UINT64_C(0xDFE05BCB1365)) & JR_MASK;
}
/* nextFloat() whose step ENDED at state `after`. */
static float jr_float_at(uint64_t after) {
	return (float)(int32_t)(after >> 24) / (float)(1 << 24);
}
/* next(31) whose step ENDED at state `after`. */
static int32_t jr_bits31_at(uint64_t after) {
	return (int32_t)(after >> 17);
}
/* Beta's standard pitch from the two nextFloat steps ending at `after`. */
static float jr_pitch_at(uint64_t after) {
	float first = jr_float_at(jr_back(after));
	float second = jr_float_at(after);
	return (first - second) * 0.2F + 1.0F;
}
/* Remove a leg-local mob so later legs never see it. */
static void mob_discard(struct entity* e) {
	uint32_t id = e->id;
	dict_entity_erase(s.entities, id);
	free(e);
}
static void watch_arm(const struct java_random* r, uint16_t lo, uint16_t hi) {
	g_watch_rng = r;
	g_watch_sid_lo = lo;
	g_watch_sid_hi = hi;
	g_watch_hits = 0;
}

static struct item_data* hotbar_slot(size_t k) {
	return &s.player.inventory.items[INVENTORY_SLOT_HOTBAR + k];
}

/* S2: one client movement sample through the real SRPC_PLAYER_POS handler
   (the footstep odometer + vertical sample consume it exactly like play). */
static void send_pos(double x, double y, double z, bool on_ground,
					 bool sneaking) {
	struct server_rpc pos = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = x,
							   .y = y,
							   .z = z,
							   .rx = 0.0F,
							   .ry = 0.0F,
							   .vel_y = 0.0F,
							   .on_ground = on_ground,
							   .sneaking = sneaking,
							   .correction_seq = s.player.correction_seq},
	};
	server_local_test_process_rpc(&s, &pos);
}

/* Toggle a lever via the REAL production activation path (block_lever.c
   onRightClick) -- same helper shape as test_trapdoor_env.c's lever_toggle.
   Re-fetches the block_data fresh each call since onRightClick mutates the
   world's metadata (this local snapshot would otherwise go stale after the
   first toggle). */
static void lever_click(int x, int y, int z) {
	struct block_data lb;
	assert(server_world_get_block(&s.world, x, y, z, &lb));
	struct block_info li = {.block = &lb, .x = x, .y = y, .z = z};
	block_lever.onRightClick(&s, &s.player, NULL, &li, &li, SIDE_TOP);
}

/* Press a stone button via the REAL production activation path
   (block_button.c onRightClick) -- same shape as lever_click above. */
static void button_press(int x, int y, int z) {
	struct block_data bb;
	assert(server_world_get_block(&s.world, x, y, z, &bb));
	struct block_info bi = {.block = &bb, .x = x, .y = y, .z = z};
	block_stone_button.onRightClick(&s, &s.player, NULL, &bi, &bi, SIDE_TOP);
}

/* ===== A: pure math goldens ============================================ */

static void test_mix_math(void) {
	/* Range: 16 * max(1, vol) [SoundManager rolloff; RenderGlobal cull]. */
	assert(feq(audio_range(1.0F), 16.0F));
	assert(feq(audio_range(0.25F), 16.0F)); /* vol<1 never SHRINKS range */
	assert(feq(audio_range(4.0F), 64.0F));	/* the explosion radius */

	/* Linear model-2 falloff to zero at the range. */
	assert(feq(audio_distance_gain(0.0F, 1.0F), 1.0F));
	assert(feq(audio_distance_gain(8.0F, 1.0F), 0.5F));
	assert(feq(audio_distance_gain(16.0F, 1.0F), 0.0F));
	assert(feq(audio_distance_gain(32.0F, 4.0F), 0.5F)); /* extended range */
	assert(feq(audio_distance_gain(100.0F, 1.0F), 0.0F)); /* clamped */

	/* Loudness: clamp-to-1 THEN master; vol>1 is never louder. */
	assert(feq(audio_loudness(4.0F, 1.0F), 1.0F));
	assert(feq(audio_loudness(0.5F, 0.6F), 0.3F));
	assert(feq(audio_loudness(1.0F, 0.0F), 0.0F));

	/* UI path: quarter volume [SoundManager.playSoundFX]. */
	assert(feq(audio_ui_loudness(1.0F, 1.0F), 0.25F));
	assert(feq(audio_ui_loudness(4.0F, 1.0F), 0.25F)); /* clamped first */
	assert(feq(audio_ui_loudness(1.0F, 0.5F), 0.125F));

	/* Pitch UNCLAMPED: the fizz-class 2.6 pitch scales the rate straight
	   through [no clamp exists in b1.7.3 SoundManager]. Exact pass-through
	   product, and explicitly ABOVE the 2.0x a later-version clamp would
	   have imposed. */
	assert(audio_playback_rate(22050.0F, 2.6F) == 22050.0F * 2.6F);
	assert(audio_playback_rate(22050.0F, 2.6F) > 22050.0F * 2.0F);
	assert(feq(audio_playback_rate(22050.0F, 0.5F), 11025.0F));

	/* Pan: centered at zero offset and dead ahead; symmetric sides; the
	   near ear keeps the FULL mono gain (never louder than mono). */
	assert(feq(audio_pan(0.0F, 0.0F, 0.0F), 0.0F));
	assert(feq(audio_pan(0.0F, 5.0F, 0.0F), 0.0F)); /* dead ahead (+z fwd) */
	float pl = audio_pan(-3.0F, 0.0F, 0.0F);
	float pr = audio_pan(3.0F, 0.0F, 0.0F);
	assert(feq(pl, -pr) && fabsf(pr) > 0.9F);
	/* Handedness pin (review F2): facing +Z (yaw 0) under cglm's RIGHT-handed
	   lookat, screen-right s = f x up = (-1,0,0) -- so a source at -X sits on
	   the SCREEN-RIGHT and its pan must be POSITIVE (left ear dimmed). The
	   symmetry assert above cannot catch a mirror; this can. */
	assert(pl > 0.9F);
	float l, r;
	audio_pan_gains(0.0F, 0.8F, &l, &r);
	assert(feq(l, 0.8F) && feq(r, 0.8F));
	audio_pan_gains(1.0F, 0.8F, &l, &r);
	assert(feq(r, 0.8F) && feq(l, 0.8F * (1.0F - AUDIO_PAN_DEPTH)));
	puts("  A: mix math goldens (range/falloff/loudness/UI/pitch/pan) OK");
}

/* ===== B: WAV parser golden ============================================ */

static size_t wav_build(uint8_t* buf, uint32_t rate, const int16_t* smp,
						uint32_t frames) {
	/* Minimal RIFF/WAVE writer, little-endian by construction. */
	uint32_t data_bytes = frames * 2;
	uint32_t riff = 36 + data_bytes;
	size_t o = 0;
	memcpy(buf + o, "RIFF", 4), o += 4;
	for(int i = 0; i < 4; i++)
		buf[o++] = (uint8_t)(riff >> (8 * i));
	memcpy(buf + o, "WAVE", 4), o += 4;
	memcpy(buf + o, "fmt ", 4), o += 4;
	uint32_t fmt_sz = 16;
	for(int i = 0; i < 4; i++)
		buf[o++] = (uint8_t)(fmt_sz >> (8 * i));
	uint16_t u16s[4] = {1 /*PCM*/, 1 /*mono*/, 0, 0};
	buf[o++] = (uint8_t)u16s[0], buf[o++] = (uint8_t)(u16s[0] >> 8);
	buf[o++] = (uint8_t)u16s[1], buf[o++] = (uint8_t)(u16s[1] >> 8);
	for(int i = 0; i < 4; i++)
		buf[o++] = (uint8_t)(rate >> (8 * i));
	uint32_t byte_rate = rate * 2;
	for(int i = 0; i < 4; i++)
		buf[o++] = (uint8_t)(byte_rate >> (8 * i));
	buf[o++] = 2, buf[o++] = 0;	 /* block align */
	buf[o++] = 16, buf[o++] = 0; /* bits */
	memcpy(buf + o, "data", 4), o += 4;
	for(int i = 0; i < 4; i++)
		buf[o++] = (uint8_t)(data_bytes >> (8 * i));
	for(uint32_t k = 0; k < frames; k++) {
		uint16_t v = (uint16_t)smp[k];
		buf[o++] = (uint8_t)v;
		buf[o++] = (uint8_t)(v >> 8);
	}
	return o;
}

static void test_wav_parser(void) {
	static const int16_t smp[5] = {0, 1234, -1234, 32767, -32768};
	uint8_t buf[128];
	size_t n = wav_build(buf, 22050, smp, 5);

	const int16_t* pcm;
	uint32_t frames, rate;
	assert(audio_wav_parse(buf, n, &pcm, &frames, &rate));
	assert(frames == 5 && rate == 22050);
	for(int k = 0; k < 5; k++)
		assert(audio_pcm_sample_from_le(pcm, k) == smp[k]);

	/* Rejections: truncated header, wrong magic, stereo, 8-bit. */
	assert(!audio_wav_parse(buf, 8, &pcm, &frames, &rate));
	uint8_t bad[128];
	memcpy(bad, buf, n);
	bad[0] = 'X';
	assert(!audio_wav_parse(bad, n, &pcm, &frames, &rate));
	memcpy(bad, buf, n);
	bad[22] = 2; /* channels = 2 */
	assert(!audio_wav_parse(bad, n, &pcm, &frames, &rate));
	memcpy(bad, buf, n);
	bad[34] = 8; /* bits = 8 */
	assert(!audio_wav_parse(bad, n, &pcm, &frames, &rate));
	/* Review F1: a crafted data-chunk size near UINT32_MAX must be REJECTED,
	   not wrap the 32-bit bounds math (Wii size_t) into a ~2G-sample
	   "success" whose copy loop corrupts MEM2. The data chunk header sits at
	   offset 36 in this minimal layout; its size field is bytes 40..43. */
	memcpy(bad, buf, n);
	bad[40] = 0xF8;
	bad[41] = 0xFF;
	bad[42] = 0xFF;
	bad[43] = 0xFF;
	assert(!audio_wav_parse(bad, n, &pcm, &frames, &rate));
	puts("  B: WAV parser golden + rejects OK");
}

/* ===== B2: S1 transcoder core goldens ================================== */

static void test_transcode_core(void) {
	static const int16_t smp[5] = {0, 1234, -1234, 32767, -32768};
	uint8_t src[128];
	size_t n = wav_build(src, 22050, smp, 5); /* odd frame count */

	/* Identity: an already-canonical PCM16-mono WAV survives the transcode
	   BYTE-FOR-BYTE. wav_build here and the tool's writer (audio_wav.h) are
	   independent implementations, so this is a real golden, not an echo. */
	int16_t* pcm = NULL;
	uint32_t frames = 0, rate = 0;
	assert(sound_transcode_buffer(src, n, &pcm, &frames, &rate));
	assert(frames == 5 && rate == 22050);
	for(int k = 0; k < 5; k++)
		assert(pcm[k] == smp[k]); /* host order on this LE test host */
	uint8_t out[128];
	size_t on = sound_transcode_wav_emit(out, rate, pcm, frames);
	assert(on == n && memcmp(out, src, n) == 0);
	free(pcm);

	/* Normalize: a legal-but-padded WAV (an extra ODD-size chunk between
	   fmt and data, word-padded per RIFF) re-emits as the canonical
	   44-byte-header layout with identical samples. */
	uint8_t padded[160];
	memcpy(padded, src, 36); /* RIFF hdr + fmt chunk */
	size_t o = 36;
	memcpy(padded + o, "LIST", 4), o += 4;
	padded[o++] = 3, padded[o++] = 0, padded[o++] = 0, padded[o++] = 0;
	padded[o++] = 'x', padded[o++] = 'y', padded[o++] = 'z';
	padded[o++] = 0; /* word pad after the odd-size chunk */
	memcpy(padded + o, src + 36, n - 36), o += n - 36; /* data chunk */
	uint32_t riff = (uint32_t)(o - 8);
	for(int i = 0; i < 4; i++)
		padded[4 + i] = (uint8_t)(riff >> (8 * i));
	assert(sound_transcode_buffer(padded, o, &pcm, &frames, &rate));
	assert(frames == 5 && rate == 22050);
	uint8_t norm[128];
	size_t nn = sound_transcode_wav_emit(norm, rate, pcm, frames);
	assert(nn == n && memcmp(norm, src, n) == 0);
	free(pcm);

	/* Rejects: exactly the ENGINE's set (the parser is shared, audio_wav.h)
	   -- garbage, stereo (the tool does NOT downmix wav-stereo; only ogg
	   carries stereo through the Beta pipeline), 8-bit, and the review-F1
	   crafted data-size vector, plus an OggS magic with garbage pages. */
	assert(!sound_transcode_buffer((const uint8_t*)"nonsense!", 9, &pcm,
								   &frames, &rate));
	uint8_t bad[128];
	memcpy(bad, src, n);
	bad[22] = 2; /* channels = 2 */
	assert(!sound_transcode_buffer(bad, n, &pcm, &frames, &rate));
	memcpy(bad, src, n);
	bad[34] = 8; /* bits = 8 */
	assert(!sound_transcode_buffer(bad, n, &pcm, &frames, &rate));
	memcpy(bad, src, n); /* F1: data-chunk size near UINT32_MAX */
	bad[40] = 0xF8;
	bad[41] = 0xFF;
	bad[42] = 0xFF;
	bad[43] = 0xFF;
	assert(!sound_transcode_buffer(bad, n, &pcm, &frames, &rate));
	memcpy(bad, "OggS", 4);
	memset(bad + 4, 0x5A, 60);
	assert(!sound_transcode_buffer(bad, 64, &pcm, &frames, &rate));

	/* --manifest rows match the shared table for EVERY id: the pack builder
	   consumes these, audio_sound_name is the engine's side of the same
	   X-macro -- and the scan path is re-derived here independently. */
	for(int id = 0; id < AUDIO_SOUND_COUNT; id++) {
		char line[256], expect[256], sub[128];
		assert(sound_transcode_manifest_line(id, line, sizeof(line)) > 0);
		snprintf(sub, sizeof(sub), "%s", audio_sound_name((uint16_t)id));
		for(char* p = sub; *p; p++)
			if(*p == '.')
				*p = '/';
		snprintf(expect, sizeof(expect), "%d\t%s\tsound/%s*.wav", id,
				 audio_sound_name((uint16_t)id), sub);
		assert(strcmp(line, expect) == 0);
	}
	char none[8];
	assert(sound_transcode_manifest_line(AUDIO_SOUND_COUNT, none, sizeof(none))
		   == -1);
	assert(sound_transcode_manifest_line(-1, none, sizeof(none)) == -1);
	puts("  B2: transcoder core (identity/normalize/rejects/manifest) OK");
}

static void test_rate_cap(void) {
	/* S2b: the 32kHz pack-build rate cap (tools/sound_transcode.h). */

	/* At/below the cap: exact no-op — same buffer pointer, same bytes. */
	int16_t* pcm = malloc(8 * sizeof(int16_t));
	assert(pcm);
	for(int k = 0; k < 8; k++)
		pcm[k] = (int16_t)(k * 111 - 400);
	int16_t* orig = pcm;
	uint32_t frames = 8, rate = 22050;
	assert(sound_transcode_rate_cap(&pcm, &frames, &rate));
	assert(pcm == orig && frames == 8 && rate == 22050);
	rate = SOUND_TRANSCODE_RATE_CAP_HZ; /* exactly AT the cap: also no-op */
	assert(sound_transcode_rate_cap(&pcm, &frames, &rate));
	assert(pcm == orig && frames == 8
		   && rate == SOUND_TRANSCODE_RATE_CAP_HZ);
	free(pcm);

	/* DC golden: 64k -> 32k halves the length at unity gain (the tap
	   weight-sum normalization makes DC exact; ±1 for final rounding). */
	enum { DCN = 400 };
	pcm = malloc(DCN * sizeof(int16_t));
	assert(pcm);
	for(int k = 0; k < DCN; k++)
		pcm[k] = 1000;
	frames = DCN;
	rate = 64000;
	assert(sound_transcode_rate_cap(&pcm, &frames, &rate));
	assert(rate == 32000 && frames == DCN / 2);
	for(uint32_t k = 0; k < frames; k++)
		assert(pcm[k] >= 999 && pcm[k] <= 1001);
	free(pcm);

	/* ANTI-ALIASING: a 20.5kHz tone in 44.1kHz input lies ABOVE the capped
	   output's 16kHz Nyquist. A naive decimator would FOLD it to an audible
	   ~11.5kHz alias at near-full level; the ratio-scaled sinc kernel must
	   kill it. Threshold 0.25x RMS (≈ -12dB) is deliberately lenient —
	   Lanczos-3 lands far below it, naive folding lands far above. */
	enum { SN = 4096 };
	pcm = malloc(SN * sizeof(int16_t));
	assert(pcm);
	double in_rms = 0.0;
	for(int k = 0; k < SN; k++) {
		double v = 8000.0
			* sin(2.0 * 3.14159265358979323846 * 20500.0 * (double)k
				  / 44100.0);
		pcm[k] = (int16_t)v;
		in_rms += v * v;
	}
	in_rms = sqrt(in_rms / (double)SN);
	frames = SN;
	rate = 44100;
	assert(sound_transcode_rate_cap(&pcm, &frames, &rate));
	assert(rate == 32000
		   && frames == (uint32_t)((uint64_t)SN * 32000 / 44100));
	double out_rms = 0.0;
	for(uint32_t k = 0; k < frames; k++)
		out_rms += (double)pcm[k] * (double)pcm[k];
	out_rms = sqrt(out_rms / (double)frames);
	assert(out_rms < 0.25 * in_rms);
	free(pcm);

	puts("  B3: 32kHz rate cap (no-op at/below, DC-exact halving, "
		 "20.5kHz alias tone killed) OK");
}

/* ===== C: StepSound + mob tables ======================================= */

static void test_step_tables(void) {
	/* The break-vs-place TRAP: glass-type (glass 20, ice 79, glowstone 89,
	   portal 90) BREAKS as random.glass but WALKS/PLACES as step.stone. */
	assert(audio_block_break_sound_id(20) == SOUND_RANDOM_GLASS);
	assert(audio_block_step_sound_id(20) == SOUND_STEP_STONE);
	assert(audio_block_break_sound_id(79) == SOUND_RANDOM_GLASS);
	assert(audio_block_break_sound_id(89) == SOUND_RANDOM_GLASS);
	assert(audio_block_break_sound_id(90) == SOUND_RANDOM_GLASS);

	/* Sand-type (sand 12, soul sand 88): BREAK plays step.gravel, walking
	   stays step.sand [StepSoundSand]. */
	assert(audio_block_break_sound_id(12) == SOUND_STEP_GRAVEL);
	assert(audio_block_step_sound_id(12) == SOUND_STEP_SAND);
	assert(audio_block_break_sound_id(88) == SOUND_STEP_GRAVEL);

	/* No snow family: snow layer 78 + snow block 80 are CLOTH. */
	assert(audio_block_step_sound_id(78) == SOUND_STEP_CLOTH);
	assert(audio_block_step_sound_id(80) == SOUND_STEP_CLOTH);

	/* No metal family: metal blocks are STONE files pitched 1.5F (rails
	   66/27/28, iron door 71, gold/iron/diamond 41/42/57, spawner 52). */
	assert(audio_block_step_sound_id(66) == SOUND_STEP_STONE);
	assert(feq(audio_block_sound_pitch(66), 1.5F));
	assert(feq(audio_block_sound_pitch(71), 1.5F));
	assert(feq(audio_block_sound_pitch(41), 1.5F));
	assert(feq(audio_block_sound_pitch(1), 1.0F));

	/* Powder DEFAULT for every unassigned id: pistons 29/33, web 30, bed 26,
	   noteblock 25 -- all sound as stone; redstone wire 55 explicitly so. */
	assert(audio_block_step_sound_id(29) == SOUND_STEP_STONE);
	assert(audio_block_step_sound_id(30) == SOUND_STEP_STONE);
	assert(audio_block_step_sound_id(26) == SOUND_STEP_STONE);
	assert(audio_block_step_sound_id(55) == SOUND_STEP_STONE);

	/* Stairs copy their model block [BlockStairs ctor]: 53 wood, 67 stone. */
	assert(audio_block_step_sound_id(53) == SOUND_STEP_WOOD);
	assert(audio_block_step_sound_id(67) == SOUND_STEP_STONE);

	/* TNT is GRASS-family; grass block breaks as its own step name. */
	assert(audio_block_break_sound_id(46) == SOUND_STEP_GRASS);
	assert(audio_block_break_sound_id(2) == SOUND_STEP_GRASS);

	/* Air: no sound (the aux-2001 id>0 guard). Volume is 1.0F everywhere. */
	assert(audio_block_break_sound_id(0) == AUDIO_SOUND_NONE);
	assert(audio_block_step_sound_id(0) == AUDIO_SOUND_NONE);
	assert(feq(audio_block_sound_volume(2), 1.0F));

	/* Fence substitution [740c583: Entity.moveEntity:478-483]: standing ON a
	   fence (air foot cell, fence 85 one below) walks as fence = wood; the
	   substitution is UNCONDITIONAL, overriding even a non-air foot cell
	   (snow layer 78 atop a fence walks as wood, not cloth). Any other
	   below-block leaves the foot cell alone. */
	assert(audio_footstep_block_id(0, 85) == 85);
	assert(audio_block_step_sound_id(audio_footstep_block_id(0, 85))
		   == SOUND_STEP_WOOD);
	assert(audio_footstep_block_id(78, 85) == 85);
	assert(audio_footstep_block_id(0, 4) == 0);  /* air over cobble: silent */
	assert(audio_footstep_block_id(2, 4) == 2);  /* grass foot cell kept */
	puts("  C: StepSound tables (glass/sand traps, cloth-snow, metal-pitch, "
		 "powder default, fence-below substitution) OK");
}

/* The MP-sound-layer pure gates (task_09eb9063): the shared footstep DECISION
   (audio_footstep_params -- one function under both the SP server emit and
   the MP client's own-movement step) and the remote-arrow embed observation
   (arrow_client_settled). [740c583: Entity.moveEntity:478-499;
   EntityArrow.onUpdate] */
static void test_mp_sound_gates(void) {
	float vol, pitch;
	/* Grass foot, air above: step.grass at 1.0*0.15F / 1.0F. */
	assert(audio_footstep_params(2, 0, &vol, &pitch) == SOUND_STEP_GRASS
		   && feq(vol, 0.15F) && feq(pitch, 1.0F));
	/* Metal foot (gold 41): stone files pitched 1.5F, still *0.15F. */
	assert(audio_footstep_params(41, 0, &vol, &pitch) == SOUND_STEP_STONE
		   && feq(vol, 0.15F) && feq(pitch, 1.5F));
	/* Snow LAYER (78) at the feet overrides the foot block -> cloth at snow's
	   own params -- even over a metal foot block. */
	assert(audio_footstep_params(1, 78, &vol, &pitch) == SOUND_STEP_CLOTH
		   && feq(vol, 0.15F) && feq(pitch, 1.0F));
	assert(audio_footstep_params(41, 78, NULL, NULL) == SOUND_STEP_CLOTH);
	/* Air foot: silent BEFORE the snow override (Beta's non-air foot gate). */
	assert(audio_footstep_params(0, 0, &vol, &pitch) == AUDIO_SOUND_NONE
		   && feq(vol, 0.0F));
	assert(audio_footstep_params(0, 78, NULL, NULL) == AUDIO_SOUND_NONE);
	/* All four liquid foot ids: silent (isLiquid material). */
	for(uint8_t liq = 8; liq <= 11; liq++)
		assert(audio_footstep_params(liq, 0, NULL, NULL) == AUDIO_SOUND_NONE);
	/* Snow BLOCK (80) foot is just cloth by its own table entry. */
	assert(audio_footstep_params(80, 0, NULL, NULL) == SOUND_STEP_CLOTH);

	/* arrow_client_settled: flight -> still-for-2 fires exactly ONCE; a
	   never-moved arrow never fires; motion resets the still run. */
	bool moved = false;
	uint8_t still = 0;
	assert(!arrow_client_settled(0.0f, &moved, &still)); /* never flew */
	assert(!arrow_client_settled(0.0f, &moved, &still));
	assert(!moved && still == 0);
	assert(!arrow_client_settled(0.25f, &moved, &still)); /* flying */
	assert(moved);
	moved = false;
	still = 0;
	assert(!arrow_client_settled(0.25f, &moved, &still)); /* flying */
	assert(!arrow_client_settled(0.0f, &moved, &still));  /* still #1 */
	assert(arrow_client_settled(0.0f, &moved, &still));   /* still #2: EDGE */
	assert(!arrow_client_settled(0.0f, &moved, &still));  /* fired once only */
	moved = false;
	still = 0;
	assert(!arrow_client_settled(0.25f, &moved, &still));
	assert(!arrow_client_settled(0.0f, &moved, &still)); /* still #1 */
	assert(!arrow_client_settled(0.25f, &moved, &still)); /* moved again */
	assert(still == 0);                                   /* run reset */
	assert(!arrow_client_settled(0.0f, &moved, &still));
	assert(arrow_client_settled(0.0f, &moved, &still)); /* fresh 2-run fires */
	puts("  C2: MP sound-layer pure gates (footstep params, arrow settle) OK");
}

static void test_mob_table(void) {
	uint16_t i, h, d;
	float v;
	audio_mob_sounds(MONSTER_ZOMBIE, &i, &h, &d, &v);
	assert(i == SOUND_MOB_ZOMBIE && h == SOUND_MOB_ZOMBIEHURT
		   && d == SOUND_MOB_ZOMBIEDEATH && feq(v, 1.0F));
	audio_mob_sounds(MONSTER_SKELETON, &i, &h, &d, &v);
	assert(h == SOUND_MOB_SKELETONHURT && d == SOUND_MOB_SKELETONHURT);
	audio_mob_sounds(MONSTER_CREEPER, &i, &h, &d, &v);
	assert(i == AUDIO_SOUND_NONE /* creeper has NO idle sound */
		   && h == SOUND_MOB_CREEPER && d == SOUND_MOB_CREEPERDEATH);
	audio_mob_sounds(MONSTER_SPIDER, &i, &h, &d, &v);
	assert(i == SOUND_MOB_SPIDER && h == SOUND_MOB_SPIDER
		   && d == SOUND_MOB_SPIDERDEATH);
	audio_mob_sounds(MONSTER_SHEEP, &i, &h, &d, &v);
	assert(i == SOUND_MOB_SHEEP && h == SOUND_MOB_SHEEP
		   && d == SOUND_MOB_SHEEP);
	audio_mob_sounds(MONSTER_COW, &i, &h, &d, &v);
	assert(i == SOUND_MOB_COW && h == SOUND_MOB_COWHURT
		   && d == SOUND_MOB_COWHURT && feq(v, 0.4F));
	audio_mob_sounds(MONSTER_PIG, &i, &h, &d, &v);
	assert(i == SOUND_MOB_PIG && h == SOUND_MOB_PIG
		   && d == SOUND_MOB_PIGDEATH);
	audio_mob_sounds(MONSTER_CHICKEN, &i, &h, &d, &v);
	assert(i == SOUND_MOB_CHICKEN && h == SOUND_MOB_CHICKENHURT
		   && d == SOUND_MOB_CHICKENHURT);
	/* b1.7.3 squid is SILENT (all getters return null). */
	audio_mob_sounds(MONSTER_SQUID, &i, &h, &d, &v);
	assert(i == AUDIO_SOUND_NONE && h == AUDIO_SOUND_NONE
		   && d == AUDIO_SOUND_NONE);
	/* S2: slime hurt == death == mob.slime at 0.6F, NO idle (EntitySlime has
	   no getLivingSound; the size-gated hops are the voice) -- all three
	   sizes share the row. */
	for(int sid = MONSTER_SLIME_SMALL; sid <= MONSTER_SLIME_BIG; sid++) {
		audio_mob_sounds(sid, &i, &h, &d, &v);
		assert(i == AUDIO_SOUND_NONE && h == SOUND_MOB_SLIME
			   && d == SOUND_MOB_SLIME && feq(v, 0.6F));
	}
	/* Giant: EntityGiantZombie has no get*Sound overrides anywhere on its
	   EntityMob chain, so it inherits EntityLiving's defaults -- silent idle,
	   hurt == death == "random.hurt" at the default 1.0F volume [740c583:
	   EntityLiving:380-390]. random.hurt is a resident S0 key (the player's
	   own hurt/death voice already uses it), so this is NOT a missing-asset
	   gap; a prior revision left it silent by mistake. */
	audio_mob_sounds(MONSTER_GIANT, &i, &h, &d, &v);
	assert(i == AUDIO_SOUND_NONE && h == SOUND_RANDOM_HURT
		   && d == SOUND_RANDOM_HURT && feq(v, 1.0F));
	puts("  C: 8-mob sound table + cow 0.4F + silent squid + slime 0.6F + "
		 "giant random.hurt OK");
}

/* ===== D: registry mapping (headless load) ============================= */

/* Sound ids that deliberately ship no copied Mojang audio and no new
 * waveform: missing keys use the registry's documented silent fallback, and
 * a user-owned Beta resource pack resolves them normally. */
static bool sound_may_use_silent_fallback(uint16_t id) {
	return (id >= SOUND_MOB_WOLF_BARK && id <= SOUND_MOB_WOLF_SHAKE)
		|| id == SOUND_AMBIENT_WEATHER_THUNDER
		|| id == SOUND_AMBIENT_WEATHER_RAIN /* registered 2026-08-31; same
			no-bundled-audio stance as thunder -- a user-owned Beta resources
			import supplies rain1..4 */
		|| id == SOUND_MINECART_BASE || id == SOUND_MINECART_INSIDE;
		/* re-scoped 2026-09-03: the synthesized filler
		   (assets/sound/minecart/base{1,2,3}.wav) was retired from the
		   shipped pack -- both minecart keys now ship NO bundled default,
		   same family as wolf/thunder/rain, so a stock pack's carts are
		   genuinely silent (Beta-faithful) until the user runs
		   tools/import_owned_beta_assets.py --modern-minecraft <dir> to pull
		   in the real modern Mojang sample. See docs/deviations.md. */
}

static void test_registry(void) {
	/* HEADLESS: enabled=false loads registry + arena, opens no device. */
	audio_init(false, 100);

	/* Key strings are the Beta SoundPool keys (slash-to-dot). */
	assert(strcmp(audio_sound_name(SOUND_STEP_GRASS), "step.grass") == 0);
	assert(strcmp(audio_sound_name(SOUND_RANDOM_EXPLODE), "random.explode")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_MOB_CHICKENPLOP), "mob.chickenplop")
		   == 0);
	assert(audio_sound_name(AUDIO_SOUND_COUNT) == NULL);
	/* S2 table completeness: the 14 new keys (and the raised variant cap for
	   Beta's cave1..cave13 pool). The b1.7.3 ABSENT set stays absent by
	   construction -- there are no liquid.lava/lavapop or damage.fall* rows
	   to look up (the enum has no such ids), so nobody can wire them. */
	/* 46 Beta keys + 1 DELIBERATE non-Beta addition (minecart.base, 2026-08-04
	   at the maintainer's request). This count is a tripwire against adding a
	   sound silently, so raising it is a decision, not bookkeeping: Beta 1.7.3
	   has no minecart sound at all (no playSound in EntityMinecart, no
	   "minecart.*" key in 740c583) and the shipped sample is synthesized by
	   tools/make_minecart_sound.py, NOT imported audio. Everything else in the
	   table remains Beta-derived, and the ABSENT set stays absent by
	   construction. See docs/deviations.md. */
	/* Batch 1b: +9 Nether-native keys, ALL Beta-derived [740c583:
	   EntityPigZombie/EntityGhast get*Sound + the zpigangry bark + the ghast
	   charge/fireball attack pair]. 46 Beta + 9 Beta (Nether) + 1 deliberate
	   non-Beta (minecart.base) = 56. Wolf adds its seven Beta keys
	   (bark/growl/whine/panting/hurt/death/shake) = 63. Weather slice adds
	   ONE Beta key ("ambient.weather.thunder",
	   [740c583: EntityLightningBolt.onUpdate]) = 64. Noteblock-slice adds
	   FIVE Beta keys -- BlockNote.playBlock's instrument set ("note." +
	   harp/bd/snare/hat/bassattack) [740c583] -- = 69. Rain-sound slice
	   (2026-08-31) adds ONE Beta key ("ambient.weather.rain" [740c583:
	   EntityRenderer.renderRainSnow playSoundEffect]) = 70. Minecart re-scope
	   (2026-09-03) adds ONE more DELIBERATE non-Beta key
	   ("minecart.inside", the riding seat-loop companion to the pre-existing
	   minecart.base) = 71. Both minecart keys moved to the silent-fallback
	   allowlist below the same day -- see docs/deviations.md; the count still
	   only ever goes up on a stated decision, never silently. */
	/* Slice 6: three approved original client-menu samples, no wire change.
	   Menu-audio vocabulary (2026-09-07) adds the remaining FIVE approved
	   client-menu keys -- back/item/type/connected/attention -- = 79. Stated
	   decision, not bookkeeping: these are project-original synthesized
	   samples from the maintainer-approved package
	   (docs/ui_design/approved/specs/sound-events.md + its manifest.json),
	   NOT Beta keys and NOT imported audio, so they are deliberate non-Beta
	   additions in the same class as minecart.base. They are CLIENT-UI-only
	   (audio_menu_event / audio_menu_device_event); they add no wire packet
	   and no world/simulation sound, so Beta 1.7.3 simulation parity is
	   untouched. Unlike the wolf/thunder/rain/minecart family they DO ship
	   bundled audio
	   (assets/sound/menu/*.wav, byte-exact from the approved package), so
	   they are NOT on the silent-fallback allowlist and must resolve. */
	assert(AUDIO_SOUND_COUNT == 80);
	assert(strcmp(audio_sound_name(SOUND_MENU_SCROLL), "menu.scroll") == 0);
	assert(strcmp(audio_sound_name(SOUND_MENU_SECTION), "menu.category") == 0);
	assert(strcmp(audio_sound_name(SOUND_MENU_SELECT), "menu.select") == 0);
	assert(strcmp(audio_sound_name(SOUND_MENU_BACK), "menu.back") == 0);
	assert(strcmp(audio_sound_name(SOUND_MENU_ITEM), "menu.item") == 0);
	assert(strcmp(audio_sound_name(SOUND_MENU_TYPE), "menu.type") == 0);
	assert(strcmp(audio_sound_name(SOUND_MENU_CONNECTED), "menu.connected")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_MENU_ATTENTION), "menu.attention")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_MINECART_INSIDE), "minecart.inside")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_AMBIENT_WEATHER_RAIN),
				  "ambient.weather.rain")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_MOB_ZOMBIEPIG), "mob.zombiepig.zpig")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_MOB_ZOMBIEPIGANGRY),
				  "mob.zombiepig.zpigangry")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_MOB_GHAST_MOAN), "mob.ghast.moan")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_MOB_GHAST_FIREBALL),
				  "mob.ghast.fireball")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_MINECART_BASE), "minecart.base") == 0);
	assert(strcmp(audio_sound_name(SOUND_MOB_WOLF_BARK), "mob.wolf.bark") == 0);
	assert(strcmp(audio_sound_name(SOUND_MOB_WOLF_SHAKE), "mob.wolf.shake") == 0);
	assert(AUDIO_MAX_VARIANTS == 14);
	assert(strcmp(audio_sound_name(SOUND_RANDOM_SPLASH), "random.splash") == 0);
	assert(strcmp(audio_sound_name(SOUND_RANDOM_FIZZ), "random.fizz") == 0);
	assert(strcmp(audio_sound_name(SOUND_RANDOM_DRR), "random.drr") == 0);
	assert(strcmp(audio_sound_name(SOUND_FIRE_FIRE), "fire.fire") == 0);
	assert(strcmp(audio_sound_name(SOUND_FIRE_IGNITE), "fire.ignite") == 0);
	assert(strcmp(audio_sound_name(SOUND_LIQUID_WATER), "liquid.water") == 0);
	assert(strcmp(audio_sound_name(SOUND_PORTAL_TRIGGER), "portal.trigger")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_PORTAL_TRAVEL), "portal.travel") == 0);
	assert(strcmp(audio_sound_name(SOUND_PORTAL_PORTAL), "portal.portal") == 0);
	assert(strcmp(audio_sound_name(SOUND_TILE_PISTON_OUT), "tile.piston.out")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_TILE_PISTON_IN), "tile.piston.in")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_MOB_SLIME), "mob.slime") == 0);
	assert(strcmp(audio_sound_name(SOUND_MOB_SLIMEATTACK), "mob.slimeattack")
		   == 0);
	assert(strcmp(audio_sound_name(SOUND_AMBIENT_CAVE), "ambient.cave.cave")
		   == 0);

	/* EVERY S0 id resolves at least one placeholder file via the per-file
	   fallback -- both digit-strip directions: numbered-only files
	   (step/grass1.wav, no bare grass.wav) and bare-only files
	   (mob/chickenplop.wav, no digits). */
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		/* Wolf + weather-thunder events deliberately ship no copied Mojang
		 * audio and no new waveform. Missing keys use the registry's
		 * documented silent fallback; a user-owned Beta resource pack
		 * resolves them normally. */
		if(sound_may_use_silent_fallback(id)) {
			assert(audio_registry_variant_count(id) == 0);
			continue;
		}
		if(audio_registry_variant_count(id) < 1) {
			fprintf(stderr, "  MISSING variants for \"%s\"\n",
					audio_sound_name(id));
			assert(0 && "S0 sound id resolved no placeholder file");
		}
	}
	assert(audio_registry_variant_count(SOUND_STEP_GRASS) >= 1);
	assert(audio_registry_variant_count(SOUND_MOB_CHICKENPLOP) >= 1);

	/* Decoded PCM landed in the fixed arena, at the placeholder rate. */
	uint32_t frames = 0, rate = 0;
	assert(audio_registry_variant_info(SOUND_STEP_STONE, 0, &frames, &rate));
	assert(rate == 22050 && frames > 0);
	assert(audio_arena_used() > 0
		   && audio_arena_used() <= audio_arena_total());
	/* 12MB since S2: S1 measured the 32-key real overlay at 6.04MB; the 14
	   S2 keys' real files (13 cave moods + fire/portal/water) overflow the
	   old 8MB arena (audio_registry.c AUDIO_ARENA_BYTES; re-measure in
	   docs/deviations.md). */
	assert(audio_arena_total() == 12u * 1024u * 1024u);

	/* The uniform pick returns a loaded sample with sane fields. */
	const struct audio_sample* smp = audio_registry_pick(SOUND_RANDOM_CLICK);
	assert(smp && smp->pcm && smp->frames > 0 && smp->rate == 22050);
	assert(audio_registry_pick(AUDIO_SOUND_COUNT) == NULL);

	/* Headless no-device play paths must be safe no-ops. */
	audio_play3d(SOUND_RANDOM_CLICK, (vec3) {0, 0, 0}, 1.0F, 1.0F);
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	audio_update((vec3) {0, 0, 0}, 0.0F);
	assert(audio_voices_active() == 0);
	puts("  D: registry mapping + arena decode (headless, no device) OK");
}

/* ===== D2: S1 pack-reload drain (headless) ============================= */

static void write_file(const char* path, const uint8_t* data, size_t n) {
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(data, 1, n, f) == n);
	fclose(f);
}

/* The live config the registry reads (gstate.config_user) has no JSON root
   in this headless harness (reads fall back); give it one so
   config_set_string can steer paths.texturepack like the packs screen. */
static void config_root_ensure(void) {
	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	assert(gstate.config_user.root);
}

static void test_registry_reload(void) {
	/* Baseline: the defaults are loaded (test_registry ran audio_init). */
	size_t used_defaults = audio_arena_used();
	assert(used_defaults > 0);
	uint32_t def_frames = 0, def_rate = 0;
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &def_frames,
									   &def_rate));

	/* A scratch pack overriding ONE key (step.grass) at a distinctive
	   rate/length no placeholder uses. */
	MKDIR(TMP_PACK);
	MKDIR(test_fixture_join(TMP_PACK, "/sound"));
	MKDIR(test_fixture_join(TMP_PACK, "/sound/step"));
	static const int16_t osmp[3] = {111, -222, 333};
	uint8_t wav[64];
	size_t wn = wav_build(wav, 11025, osmp, 3);
	write_file(test_fixture_join(TMP_PACK, "/sound/step/grass1.wav"), wav, wn);

	config_root_ensure();
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 TMP_PACK));
	audio_registry_reload();

	/* Per-KEY resolution: the pack OWNS step.grass now -- exactly ONE
	   variant, the override; no default placeholder leaked into the pool. */
	assert(audio_registry_variant_count(SOUND_STEP_GRASS) == 1);
	uint32_t frames = 0, rate = 0;
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(rate == 11025 && frames == 3);
	const struct audio_sample* smp = audio_registry_pick(SOUND_STEP_GRASS);
	assert(smp && smp->rate == 11025 && smp->frames == 3);
	assert(smp->pcm[0] == 111 && smp->pcm[1] == -222 && smp->pcm[2] == 333);
	/* Every OTHER key still resolves (per-key fallback to the defaults). */
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(sound_may_use_silent_fallback(id))
			assert(audio_registry_variant_count(id) == 0);
		else
			assert(audio_registry_variant_count(id) >= 1);
	}
	/* Arena rewritten IN PLACE: same 12MB allocation, smaller fill (the
	   3-frame override replaced a real default sample). */
	assert(audio_arena_total() == 12u * 1024u * 1024u);
	size_t used_pack = audio_arena_used();
	assert(used_pack > 0 && used_pack < used_defaults);
	/* The drain is voice-safe by construction here (headless: no backend),
	   and the engine-side slots are cleared. */
	assert(audio_voices_active() == 0);

	/* Reload BACK to the defaults: a byte-exact rescan must land the bump
	   arena on EXACTLY the S0 fill -- the reset is real, not a leak. */
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "assets"));
	audio_registry_reload();
	assert(audio_arena_used() == used_defaults);
	assert(audio_registry_variant_info(SOUND_STEP_GRASS, 0, &frames, &rate));
	assert(frames == def_frames && rate == def_rate);

	remove(test_fixture_join(TMP_PACK, "/sound/step/grass1.wav"));
	puts("  D2: reload drain (per-key override in, arena reset, byte-exact "
		 "reload back) OK");
}

/* ===== D2b (S2): the two-digit variant scan ============================ */

/* Beta's cave mood pool is cave1..cave13 -- ALL thirteen map onto the ONE
   "ambient.cave.cave" key via SoundPool's digit-strip. The old
   AUDIO_MAX_VARIANTS=10 scan silently dropped cave10..13 from a real pack;
   this pins the raised cap end-to-end: a scratch pack shipping exactly
   cave1..cave13 resolves THIRTEEN variants (and per-key ownership keeps the
   bundled default cave1 placeholder OUT of the pool). */
static void test_variant_two_digit_scan(void) {
	MKDIR(TMP_PACK);
	MKDIR(test_fixture_join(TMP_PACK, "/sound"));
	MKDIR(test_fixture_join(TMP_PACK, "/sound/ambient"));
	MKDIR(test_fixture_join(TMP_PACK, "/sound/ambient/cave"));
	uint8_t wav[64];
	char path[128];
	for(int v = 1; v <= 13; v++) {
		/* v-dependent frame count so each variant is distinguishable. */
		int16_t smp[16];
		for(int k = 0; k < 3 + (v & 3); k++)
			smp[k] = (int16_t)(v * 100 + k);
		size_t wn = wav_build(wav, 11025, smp, 3 + (v & 3));
		snprintf(path, sizeof(path), test_fixture_join(TMP_PACK, "/sound/ambient/cave/cave%d.wav"),
				 v);
		write_file(path, wav, wn);
	}

	config_root_ensure();
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 TMP_PACK));
	audio_registry_reload();
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == 13);
	/* The pack owns the key: no default placeholder mixed in -- every variant
	   is one of the 11025 Hz scratch files. */
	for(int v = 0; v < 13; v++) {
		uint32_t frames = 0, rate = 0;
		assert(audio_registry_variant_info(SOUND_AMBIENT_CAVE, v, &frames,
										   &rate));
		assert(rate == 11025 && frames >= 3 && frames <= 6);
	}
	/* Every other key still falls back to a full default set. */
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		if(sound_may_use_silent_fallback(id))
			assert(audio_registry_variant_count(id) == 0);
		else
			assert(audio_registry_variant_count(id) >= 1);
	}

	/* Back to the defaults for the E sections. */
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "assets"));
	audio_registry_reload();
	assert(audio_registry_variant_count(SOUND_AMBIENT_CAVE) == 1);
	for(int v = 1; v <= 13; v++) {
		snprintf(path, sizeof(path), test_fixture_join(TMP_PACK, "/sound/ambient/cave/cave%d.wav"),
				 v);
		remove(path);
	}
	puts("  D2b: cave1..cave13 scratch pack -> 13 variants (two-digit scan) "
		 "OK");
}

/* ===== D3 (optional): a REAL locally built Beta pack =================== */

/* CAVEWII_S1_REAL_PACK=<dir> reloads the registry from a pack produced by
   tools/make_beta_pack.ps1 out of the USER'S OWN Beta resources (gitignored
   tmp/, never committed) and prints the per-key table -- the proof that the
   engine loads real transcoded Beta sounds end-to-end. Inert in CI. */
static void real_pack_report(void) {
	const char* pack = getenv("CAVEWII_S1_REAL_PACK");
	if(!pack)
		return;

	config_root_ensure();
	assert(config_set_string(&gstate.config_user, "paths.texturepack", pack));
	audio_registry_reload();

	int resolved = 0, multi = 0, no_beta_source = 0;
	printf("  D3: REAL pack \"%s\" per-key table:\n", pack);
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		int vc = audio_registry_variant_count(id);
		/* minecart.base / minecart.inside have NO Beta 1.7.3 source: Beta's
		   EntityMinecart plays no sound and its resources carry no minecart
		   file (docs/deviations.md "Minecart rolling sound"). A pack built
		   from the player's own Beta files cannot hold them; they come only
		   from the importer's --modern-minecraft add-on, else they stay
		   silent (sound_may_use_silent_fallback). Expecting every key from a
		   Beta pack was stale (2026-10-30 jar-import audit): count them
		   separately instead. */
		if(vc == 0
		   && (id == SOUND_MINECART_BASE || id == SOUND_MINECART_INSIDE)) {
			assert(sound_may_use_silent_fallback(id));
			no_beta_source++;
		}
		uint32_t frames = 0, rate = 0;
		audio_registry_variant_info(id, 0, &frames, &rate);
		printf("    %-18s variants=%d v0=%u frames @ %u Hz\n",
			   audio_sound_name(id), vc, (unsigned)frames, (unsigned)rate);
		if(vc >= 1)
			resolved++;
		if(vc > 1)
			multi++;
	}
	printf("  D3: %d/%d keys resolved, %d with multi-variant pools "
		   "(the bundled defaults are 1-per-key), arena %u/%u bytes\n",
		   resolved, (int)AUDIO_SOUND_COUNT, multi,
		   (unsigned)audio_arena_used(), (unsigned)audio_arena_total());
	printf("  D3: %d key(s) with no Beta 1.7.3 source left silent "
		   "(minecart.base/inside)\n", no_beta_source);
	assert(resolved + no_beta_source == (int)AUDIO_SOUND_COUNT);

	/* Back to the defaults so the E sections run on the canonical state. */
	assert(config_set_string(&gstate.config_user, "paths.texturepack",
							 "assets"));
	audio_registry_reload();
}

/* ===== E: CRPC_SOUND emission through the env server =================== */

int main(void) {
	test_fixture_dir("test_audio_env_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	test_fixture_dir("test_audio_env_pack_tmp", s_fixture_TMP_PACK, sizeof s_fixture_TMP_PACK);
	test_mix_math();
	test_wav_parser();
	test_transcode_core();
	test_rate_cap();
	test_step_tables();
	test_mp_sound_gates();
	test_mob_table();
	test_registry();
	test_registry_reload();
	test_variant_two_digit_scan();
	real_pack_report();

	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = UINT32_C(0xA1B2C3D4);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	/* Superflat surface probe (the animal-test pattern). */
	int gy = -1;
	for(int y = 127; y >= 0; y--) {
		struct block_data probe;
		if(server_world_get_block(&s.world, 8, y, 8, &probe)
		   && probe.type != BLOCK_AIR) {
			gy = y;
			break;
		}
	}
	assert(gy > 0);
	const float feet = (float)(gy + 1);
	const double eye = (double)(gy + 1) + PLAYER_EYE_HEIGHT;
	s.player.x = 8.5;
	s.player.y = eye;
	s.player.z = 8.5;

	clin_test_set_rpc_observer(capture_sound, NULL);

	/* --- E1: block BREAK = aux-2001 break name, (v+1)/2 @ p*0.8 --------- */
	{
		struct block_data top;
		assert(server_world_get_block(&s.world, 8, gy, 8, &top));
		assert(top.type == 2); /* superflat surface = grass */
		snd_reset();
		struct server_rpc dig = {
			.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {.x = 8, .y = gy, .z = 8,
								  .side = SIDE_TOP, .finished = true},
		};
		server_local_test_process_rpc(&s, &dig);
		assert(g_snd_count == 1);
		assert(g_snd_id == SOUND_STEP_GRASS); /* grass break name */
		assert(feq(g_snd_vol, 1.0F));		  /* (1.0F+1.0F)/2.0F */
		assert(feq(g_snd_pitch, 0.8F));		  /* 1.0F*0.8F */
		assert(feq(g_snd_pos[0], 8.5F) && feq(g_snd_pos[1], gy + 0.5F)
			   && feq(g_snd_pos[2], 8.5F));
		puts("  E1: break -> CRPC_SOUND step.grass 1.0 @ 0.8 at center");
	}

	/* --- E2: PLACE plays the STEP name; glass pins the trap ------------- */
	{
		inventory_set_hotbar(&s.player.inventory, 0);
		*hotbar_slot(0)
			= (struct item_data) {.id = 20 /* glass */, .count = 1};
		snd_reset();
		struct server_rpc place = {
			.type = SRPC_BLOCK_PLACE,
			.payload.block_place = {.x = 10, .y = gy, .z = 10,
									.side = SIDE_TOP},
		};
		server_local_test_process_rpc(&s, &place);
		struct block_data placed;
		assert(server_world_get_block(&s.world, 10, gy + 1, 10, &placed));
		assert(placed.type == 20);
		assert(g_snd_count == 1);
		assert(g_snd_id == SOUND_STEP_STONE); /* place = STEP name */
		assert(feq(g_snd_vol, 1.0F) && feq(g_snd_pitch, 0.8F));

		/* ...and breaking the same glass plays random.glass. */
		snd_reset();
		struct server_rpc dig = {
			.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {.x = 10, .y = gy + 1, .z = 10,
								  .side = SIDE_TOP, .finished = true},
		};
		server_local_test_process_rpc(&s, &dig);
		assert(g_snd_count == 1);
		assert(g_snd_id == SOUND_RANDOM_GLASS);
		assert(feq(g_snd_pitch, 0.8F));
		puts("  E2: glass place=step.stone / break=random.glass (the trap)");
	}

	/* --- E3: explosion = random.explode CONSTANT 4.0F ------------------- */
	{
		/* Mid-air blast far from the player: no block loss, no player
		   damage -- exactly one CRPC_SOUND. */
		s.player.x = 200.0;
		snd_reset();
		server_world_explode(&s.worlds[0], (vec3) {8.0F, (float)(gy + 12), 8.0F},
							 4.0F);
		assert(g_snd_count == 1);
		assert(g_snd_id == SOUND_RANDOM_EXPLODE);
		assert(feq(g_snd_vol, 4.0F)); /* hardcoded, power-independent */
		/* pitch = (1 + (r-r)*0.2) * 0.7 in [0.56, 0.84] */
		assert(g_snd_pitch >= 0.56F - 1.0e-5F
			   && g_snd_pitch <= 0.84F + 1.0e-5F);
		s.player.x = 8.5;
		puts("  E3: explosion -> random.explode 4.0F, pitch in [0.56,0.84]");
	}

	/* --- E4: TNT prime = random.fuse 1.0F/1.0F -------------------------- */
	{
		assert(server_world_set_block(
			&s.worlds[0], 4, gy + 1, 4, (struct block_data) {.type = BLOCK_TNT}));
		snd_reset();
		assert(server_local_prime_tnt_block(&s, 4, gy + 1, 4,
											TNT_FUSE_DEFAULT));
		assert(g_snd_count == 1);
		assert(g_snd_id == SOUND_RANDOM_FUSE);
		assert(feq(g_snd_vol, 1.0F) && feq(g_snd_pitch, 1.0F));
		puts("  E4: TNT prime -> random.fuse 1.0/1.0");
	}

	/* --- E5: chicken egg lay = mob.chickenplop 1.0F, wobble pitch ------- */
	{
		struct entity* chicken = server_local_spawn_monster_at(
			(vec3) {12.5F, feet, 12.5F}, MONSTER_CHICKEN, &s);
		assert(chicken);
		chicken->data.monster.egg_timer = 1;
		snd_reset();
		assert(!chicken->tick_server(chicken, &s));
		/* The tick may also emit non-sound RPCs; exactly one CRPC_SOUND. */
		assert(g_snd_count == 1);
		assert(g_snd_id == SOUND_MOB_CHICKENPLOP);
		assert(feq(g_snd_vol, 1.0F));
		assert(g_snd_pitch >= 0.8F - 1.0e-5F
			   && g_snd_pitch <= 1.2F + 1.0e-5F); /* (r-r)*0.2+1.0 */
		puts("  E5: egg lay -> mob.chickenplop 1.0, wobble pitch");
	}

	/* --- E6: mob hurt/death (fresh-only) + cow 0.4F + 0-damage hurt ----- */
	{
		struct entity* zombie = server_local_spawn_monster_at(
			(vec3) {6.5F, feet, 6.5F}, MONSTER_ZOMBIE, &s);
		assert(zombie);
		snd_reset();
		assert(entity_monster_apply_damage(zombie, 4, false)
			   == MONSTER_DAMAGE_FRESH);
		assert(g_snd_count == 1 && g_snd_id == SOUND_MOB_ZOMBIEHURT);
		assert(feq(g_snd_vol, 1.0F));
		assert(g_snd_pitch >= 0.8F - 1.0e-5F && g_snd_pitch <= 1.2F + 1.0e-5F);

		/* In-window equal hit: ABSORBED -> silent (Beta returns false). */
		snd_reset();
		assert(entity_monster_apply_damage(zombie, 4, false)
			   == MONSTER_DAMAGE_ABSORBED);
		assert(g_snd_count == 0);

		/* Death edge: a fresh lethal hit plays the DEATH sound. */
		zombie->data.monster.hurt_resist_time = 0;
		zombie->health = 2;
		snd_reset();
		assert(entity_monster_apply_damage(zombie, 4, false)
			   == MONSTER_DAMAGE_FRESH);
		assert(zombie->health <= 0);
		assert(g_snd_count == 1 && g_snd_id == SOUND_MOB_ZOMBIEDEATH);

		/* Cow: getSoundVolume 0.4F; a fresh ZERO-damage hit (the thrown
		   egg) still plays the hurt sound, exactly like Beta. */
		struct entity* cow = server_local_spawn_monster_at(
			(vec3) {5.5F, feet, 5.5F}, MONSTER_COW, &s);
		assert(cow);
		snd_reset();
		assert(entity_monster_apply_damage(cow, 0, false)
			   == MONSTER_DAMAGE_FRESH);
		assert(g_snd_count == 1 && g_snd_id == SOUND_MOB_COWHURT);
		assert(feq(g_snd_vol, 0.4F));
		puts("  E6: zombie hurt/absorb-silent/death edge + cow 0.4F + "
			 "0-damage hurt");
	}

	/* --- E6b: Giant hurt/death -> random.hurt (the fixed audio_mob_sounds
	   row, EntityLiving's uninherited defaults on the EntityMob chain) ----- */
	{
		struct entity* giant = server_local_spawn_monster_at(
			(vec3) {14.5F, feet, 6.5F}, MONSTER_GIANT, &s);
		assert(giant);
		snd_reset();
		assert(entity_monster_apply_damage(giant, 10, false)
			   == MONSTER_DAMAGE_FRESH);
		assert(g_snd_count == 1 && g_snd_id == SOUND_RANDOM_HURT);
		assert(feq(g_snd_vol, 1.0F));

		/* Death edge: a fresh lethal hit plays random.hurt too (hurt ==
		   death on this chain, same as the player). */
		giant->data.monster.hurt_resist_time = 0;
		giant->health = 4;
		snd_reset();
		assert(entity_monster_apply_damage(giant, 10, false)
			   == MONSTER_DAMAGE_FRESH);
		assert(giant->health <= 0);
		assert(g_snd_count == 1 && g_snd_id == SOUND_RANDOM_HURT);
		puts("  E6b: giant hurt+death -> random.hurt (the fixed gap)");
	}

	/* --- E7: player random.hurt on FRESH damage only -------------------- */
	{
		s.player.health = MAX_PLAYER_HEALTH;
		s.player.hurt_resist_time = 0;
		snd_reset();
		assert(server_local_damage_player(&s, 8, 0.0F, 0.0F));
		assert(g_snd_count == 1 && g_snd_id == SOUND_RANDOM_HURT);
		assert(feq(g_snd_vol, 1.0F));
		/* At the feet (playSoundAtEntity plays at posY - yOffset). */
		assert(feq(g_snd_pos[1], (float)(s.player.y - PLAYER_EYE_HEIGHT)));

		/* Same-strength hit inside the i-frame window: absorbed, SILENT. */
		snd_reset();
		assert(server_local_damage_player(&s, 8, 0.0F, 0.0F));
		assert(g_snd_count == 0);
		puts("  E7: player fresh hit -> random.hurt; i-frame absorb silent");
	}

	/* --- E8 (S2): player FOOTSTEP odometer goldens ---------------------- */
	{
		/* Clean grass lane inside the loaded chunk. 0.5-block samples add
		   0.3 each (0.6-weighted); the crossing of 1.0 lands on the 4th. */
		s.player.x = 1.5;
		s.player.y = eye;
		s.player.z = 9.5;
		s.player.on_ground = true;
		s.player.sneaking = false;
		s.player.distance_walked = 0.0F;
		s.player.next_step_distance = 1;
		snd_reset();
		for(int k = 1; k <= 3; k++)
			send_pos(1.5 + 0.5 * k, eye, 9.5, true, false);
		assert(g_snd_count == 0); /* 0.9 accumulated: below the crossing */
		send_pos(3.5, eye, 9.5, true, false);
		assert(g_snd_by_id[SOUND_STEP_GRASS] == 1 && g_snd_count == 1);
		assert(feq(g_snd_vol_by_id[SOUND_STEP_GRASS], 0.15F)); /* 1.0*0.15 */
		assert(feq(g_snd_pitch_by_id[SOUND_STEP_GRASS], 1.0F));
		/* At the feet (posY - yOffset). */
		assert(feq(g_snd_pos[1], (float)(eye - PLAYER_EYE_HEIGHT)));

		/* SNEAK freezes the WHOLE odometer (Beta gates the accumulation
		   itself on !(onGround && isSneaking))). */
		float frozen = s.player.distance_walked;
		s.player.sneaking = true; /* the pre-call snapshot the gate reads */
		snd_reset();
		for(int k = 1; k <= 6; k++)
			send_pos(3.5 + 0.5 * k, eye, 9.5, true, true);
		assert(g_snd_count == 0);
		assert(feq(s.player.distance_walked, frozen));
		s.player.sneaking = false;

		/* SNOW-layer-above override: the foot cell's step sound is replaced
		   by cloth when the cell the feet occupy holds the snow LAYER. */
		for(int x = 4; x <= 12; x++)
			assert(server_world_set_block(
				&s.worlds[0], x, gy + 1, 13, (struct block_data) {.type = BLOCK_SNOW}));
		s.player.x = 4.5;
		s.player.z = 13.5;
		s.player.distance_walked = 0.0F;
		s.player.next_step_distance = 1;
		snd_reset();
		for(int k = 1; k <= 4; k++)
			send_pos(4.5 + 0.5 * k, eye, 13.5, true, false);
		assert(g_snd_by_id[SOUND_STEP_CLOTH] == 1 && g_snd_count == 1);
		assert(feq(g_snd_vol_by_id[SOUND_STEP_CLOTH], 0.15F));

		/* LIQUID silence: a water foot cell steps silently (the odometer
		   still fires -- Beta increments nextStepDistance regardless). */
		for(int x = 4; x <= 12; x++)
			assert(server_world_set_block(
				&s.worlds[0], x, gy, 15,
				(struct block_data) {.type = BLOCK_WATER_STILL}));
		s.player.x = 4.5;
		s.player.z = 15.5;
		s.player.distance_walked = 0.0F;
		s.player.next_step_distance = 1;
		snd_reset();
		for(int k = 1; k <= 4; k++)
			send_pos(4.5 + 0.5 * k, eye, 15.5, true, false);
		assert(g_snd_count == 0);
		assert(s.player.next_step_distance == 2); /* the crossing DID fire */

		/* FENCE special case: standing ON a fence leaves the foot cell AIR
		   (fences are 1.5 tall), yet Beta substitutes the fence below into
		   the foot-block id -- the odometer fires AND steps as fence wood, where the
		   plain air gate would stay silent forever.
		   [740c583: Entity.moveEntity:478-488] */
		for(int x = 4; x <= 12; x++)
			assert(server_world_set_block(
				&s.worlds[0], x, gy + 1, 11, (struct block_data) {.type = BLOCK_FENCE}));
		s.player.x = 4.5;
		s.player.z = 11.5;
		s.player.y = eye + 1.5; /* feet on the fence top */
		s.player.distance_walked = 0.0F;
		s.player.next_step_distance = 1;
		snd_reset();
		for(int k = 1; k <= 4; k++)
			send_pos(4.5 + 0.5 * k, eye + 1.5, 11.5, true, false);
		assert(g_snd_by_id[SOUND_STEP_WOOD] == 1 && g_snd_count == 1);
		assert(s.player.next_step_distance == 2);
		/* Restore the pre-leg world AND player pos: later mob legs aim their
		   AI at the player (a moved player re-routes every slime jump, which
		   once parked a slime inside E17's arrow column). */
		for(int x = 4; x <= 12; x++)
			assert(server_world_set_block(
				&s.worlds[0], x, gy + 1, 11, (struct block_data) {.type = BLOCK_AIR}));
		s.player.x = 6.5;
		s.player.z = 15.5;
		s.player.y = eye;
		puts("  E8: footstep odometer (one step per unit, sneak freeze, "
			 "snow->cloth, liquid silence, fence-top wood)");
	}

	/* --- E9 (S2 + LIVING-SOUND-RNG): idle voice on the mob's OWN Random --
	   Beta EntityLiving.onEntityUpdate rolls nextInt(1000) against a post-
	   incremented livingSoundTime; a hit resets it to minus the talk interval
	   and plays the living sound, pitched 1.0F plus 0.2F times (first nextFloat
	   minus second nextFloat) [server EntityLiving.java:93-106].
	   For a dry, unburnt cow Entity.onEntityUpdate draws nothing, so the roll
	   is the FIRST draw of the tick: the stream at the emit must be exactly
	   3 steps past the pre-tick state (roll, float, float), the roll must be
	   below the pre-tick livingSoundTime, and the pitch must be those two
	   floats. The shared engine sound stream must not move. */
	{
		struct entity* cow = server_local_spawn_monster_at(
			(vec3) {2.5F, feet, 2.5F}, MONSTER_COW, &s);
		assert(cow);
		server_local_sound_rng()->seed = UINT32_C(0xBADC0DE5);
		const uint32_t sound_seed_before = server_local_sound_rng()->seed;
		cow->data.monster.living_sound_time = 700; /* fire within a few ticks */
		watch_arm(&cow->gameplay_rng, SOUND_MOB_COW, SOUND_MOB_COW);
		snd_reset();
		uint64_t pre = 0;
		int lst = 0, guard = 0;
		while(g_watch_hits == 0 && guard++ < 200) {
			pre = cow->gameplay_rng.seed;
			lst = cow->data.monster.living_sound_time;
			(void)cow->tick_server(cow, &s);
		}
		g_watch_rng = NULL;
		assert(g_watch_hits == 1);
		const uint64_t at = g_watch_seed_at_emit;
		/* roll; float; float -- and nothing before them this tick */
		assert(jr_fwd(jr_fwd(jr_fwd(pre))) == at);
		const int32_t bits = jr_bits31_at(jr_back(jr_back(at)));
		assert(bits - bits % 1000 + 999 >= 0); /* single nextInt iteration */
		assert(bits % 1000 < lst);
		assert(g_snd_pitch_by_id[SOUND_MOB_COW] == jr_pitch_at(at));
		assert(feq(g_snd_vol_by_id[SOUND_MOB_COW], 0.4F)); /* getSoundVolume */
		/* The reset re-arms the ramp at -getTalkInterval() (120, animal). */
		assert(cow->data.monster.living_sound_time == -120);
		/* The cow's voice never touches the engine sound stream. */
		assert(server_local_sound_rng()->seed == sound_seed_before);
		puts("  E9: cow idle voice = first 3 draws of the cow's own Random "
			 "(roll<t, pitch floats); sound stream untouched; 0.4F");
	}

#ifndef LIVING_SOUND_RED_PARENT
	/* --- E9b (LIVING-SOUND-RNG): the scheduler step's exact draw count ---
	   creeper (getLivingSound null): ONE nextInt per call even on a fire,
	   no pitch floats; wolf: roll, getLivingSound nextInt(3), 2 floats;
	   a non-firing call: the roll only. Transcribed on a java_random clone. */
	{
		struct entity* cr = server_local_spawn_monster_at(
			(vec3) {12.5F, feet, 2.5F}, MONSTER_CREEPER, &s);
		assert(cr);
		for(int k = 0; k < 64; k++) {
			struct java_random ref = cr->gameplay_rng;
			int t = cr->data.monster.living_sound_time;
			bool fire = jrand_next_int_bound(&ref, 1000) < t;
			snd_reset();
			entity_monster_living_sound_tick(cr);
			assert(cr->gameplay_rng.seed == ref.seed);
			assert(cr->data.monster.living_sound_time == (fire ? -80 : t + 1));
			assert(g_snd_count == 0); /* voiceless */
			if(k == 20)
				cr->data.monster.living_sound_time = 999; /* force fires */
		}
		struct entity* wolf = server_local_spawn_monster_at(
			(vec3) {12.5F, feet, 12.5F}, MONSTER_WOLF, &s);
		assert(wolf);
		int fires = 0;
		for(int k = 0; k < 64; k++) {
			struct java_random ref = wolf->gameplay_rng;
			int t = wolf->data.monster.living_sound_time;
			bool fire = jrand_next_int_bound(&ref, 1000) < t;
			uint16_t want = AUDIO_SOUND_NONE;
			float want_pitch = 0.0F;
			if(fire) {
				switch(wolf_living_sound_pick(wolf->data.monster.wolf_flags,
					wolf->data.monster.wolf_watched_health,
					jrand_next_int_bound(&ref, 3))) {
				case WOLF_LIVING_GROWL: want = SOUND_MOB_WOLF_GROWL; break;
				case WOLF_LIVING_WHINE: want = SOUND_MOB_WOLF_WHINE; break;
				case WOLF_LIVING_PANTING: want = SOUND_MOB_WOLF_PANTING; break;
				default: want = SOUND_MOB_WOLF_BARK; break;
				}
				float a1 = jrand_next_float(&ref);
				float a2 = jrand_next_float(&ref);
				want_pitch = (a1 - a2) * 0.2F + 1.0F;
				fires++;
			}
			snd_reset();
			entity_monster_living_sound_tick(wolf);
			assert(wolf->gameplay_rng.seed == ref.seed);
			if(fire) {
				assert(g_snd_count == 1 && g_snd_id == want);
				assert(g_snd_pitch == want_pitch);
				assert(feq(g_snd_vol, 0.4F));
			} else {
				assert(g_snd_count == 0);
			}
			if(k % 8 == 0)
				wolf->data.monster.living_sound_time = 999;
		}
		assert(fires >= 4);
		mob_discard(cr);
		mob_discard(wolf);
		puts("  E9b: scheduler step draws roll [+wolf pick +2 floats] on the "
			 "mob's own Random; voiceless creeper draws the roll only");
	}
#endif

	/* --- E9c (LIVING-SOUND-RNG): hurt / death / absorbed pitch -----------
	   EntityLiving.attackEntityFrom plays getHurtSound/getDeathSound with the
	   standard pitch on the FRESH branch only [server EntityLiving.java:
	   340/345]: exactly 2 floats on the mob's own Random; an in-window
	   absorbed hit draws nothing. */
	{
		struct entity* z = server_local_spawn_monster_at(
			(vec3) {6.5F, feet, 12.5F}, MONSTER_ZOMBIE, &s);
		assert(z);
		const uint32_t sound_seed_before = server_local_sound_rng()->seed;
		struct java_random ref = z->gameplay_rng;
		float h1 = jrand_next_float(&ref);
		float h2 = jrand_next_float(&ref);
		snd_reset();
		assert(entity_monster_apply_damage(z, 2, false) == MONSTER_DAMAGE_FRESH);
		assert(z->gameplay_rng.seed == ref.seed);
		assert(g_snd_count == 1 && g_snd_id == SOUND_MOB_ZOMBIEHURT);
		assert(g_snd_pitch == (h1 - h2) * 0.2F + 1.0F);
		/* in-window, not larger: absorbed, silent, no draw */
		snd_reset();
		assert(entity_monster_apply_damage(z, 1, false)
			   == MONSTER_DAMAGE_ABSORBED);
		assert(z->gameplay_rng.seed == ref.seed && g_snd_count == 0);
		/* window over: a lethal fresh hit plays the DEATH sound */
		z->data.monster.hurt_resist_time = 0;
		float d1 = jrand_next_float(&ref);
		float d2 = jrand_next_float(&ref);
		snd_reset();
		assert(entity_monster_apply_damage(z, 100, false)
			   == MONSTER_DAMAGE_FRESH);
		assert(z->health <= 0);
		assert(z->gameplay_rng.seed == ref.seed);
		assert(g_snd_count == 1 && g_snd_id == SOUND_MOB_ZOMBIEDEATH);
		assert(g_snd_pitch == (d1 - d2) * 0.2F + 1.0F);
		assert(server_local_sound_rng()->seed == sound_seed_before);
		mob_discard(z);
		puts("  E9c: hurt/death pitch = 2 floats of the mob's own Random; "
			 "absorbed hit draws none");
	}

	/* --- E9d (LIVING-SOUND-RNG): pig zombie bark is the tick's FIRST draw.
	   EntityPigZombie.onUpdate rolls the zpigangry pitch BEFORE
	   super.onUpdate [server EntityPigZombie.java:18-24]. */
	{
		struct entity* pz = server_local_spawn_monster_at(
			(vec3) {9.5F, feet, 9.5F}, MONSTER_PIG_ZOMBIE, &s);
		assert(pz);
		pz->data.monster.anger_sound_delay = 1;
		const uint64_t pre = pz->gameplay_rng.seed;
		watch_arm(&pz->gameplay_rng, SOUND_MOB_ZOMBIEPIGANGRY,
				  SOUND_MOB_ZOMBIEPIGANGRY);
		snd_reset();
		(void)pz->tick_server(pz, &s);
		g_watch_rng = NULL;
		assert(g_watch_hits == 1);
		assert(g_watch_seed_at_emit == jr_fwd(jr_fwd(pre)));
		assert(g_snd_pitch_by_id[SOUND_MOB_ZOMBIEPIGANGRY]
			   == jr_pitch_at(g_watch_seed_at_emit) * 1.8F);
		mob_discard(pz);
		puts("  E9d: pig zombie bark = first 2 draws of its own Random");
	}

	/* --- E10 (S2): slime size gates (jump >1, land >2) + attack --------- */
	{
		/* BIG (size 4): audible hop (pitch *0.8) THEN audible land (/0.8) --
		   the two pitch windows are disjoint ([0.64,0.96] vs [1.0,1.5]). */
		struct entity* big = server_local_spawn_monster_at(
			(vec3) {3.5F, feet, 6.5F}, MONSTER_SLIME_BIG, &s);
		assert(big);
		snd_reset();
		int guard = 0;
		while(g_snd_by_id[SOUND_MOB_SLIME] == 0 && guard++ < 30)
			(void)big->tick_server(big, &s);
		assert(g_snd_by_id[SOUND_MOB_SLIME] == 1); /* the hop */
		assert(feq(g_snd_vol_by_id[SOUND_MOB_SLIME], 0.6F));
		float hop_pitch = g_snd_pitch_by_id[SOUND_MOB_SLIME];
		assert(hop_pitch >= 0.8F * 0.8F - 1.0e-5F
			   && hop_pitch <= 1.2F * 0.8F + 1.0e-5F);
		guard = 0;
		while(g_snd_by_id[SOUND_MOB_SLIME] < 2 && guard++ < 80)
			(void)big->tick_server(big, &s);
		assert(g_snd_by_id[SOUND_MOB_SLIME] == 2); /* the landing squish */
		float land_pitch = g_snd_pitch_by_id[SOUND_MOB_SLIME];
		assert(land_pitch >= 1.0F - 1.0e-5F && land_pitch <= 1.5F + 1.0e-5F);

		/* MEDIUM (size 2): hop audible, LAND silent. jump_delay freezes
		   while airborne and decrements on the first grounded tick, so the
		   first decrement after the hop IS the landing edge -- and >= 2
		   grounded ticks remain before the next hop can re-fire. */
		struct entity* med = server_local_spawn_monster_at(
			(vec3) {11.5F, feet, 4.5F}, MONSTER_SLIME_MEDIUM, &s);
		assert(med);
		snd_reset();
		guard = 0;
		while(g_snd_by_id[SOUND_MOB_SLIME] == 0 && guard++ < 30)
			(void)med->tick_server(med, &s);
		assert(g_snd_by_id[SOUND_MOB_SLIME] == 1); /* the hop */
		int jd0 = med->data.monster.jump_delay;
		assert(jd0 >= 3); /* (rand(20)+10)/3 with the player near */
		guard = 0;
		while(med->data.monster.jump_delay == jd0 && guard++ < 80)
			(void)med->tick_server(med, &s);
		assert(med->data.monster.jump_delay == jd0 - 1); /* just landed */
		assert(g_snd_by_id[SOUND_MOB_SLIME] == 1);		 /* land was SILENT */

		/* SMALL (size 1): fully silent hops. */
		struct entity* small = server_local_spawn_monster_at(
			(vec3) {13.5F, feet, 8.5F}, MONSTER_SLIME_SMALL, &s);
		assert(small);
		snd_reset();
		for(int k = 0; k < 25; k++)
			(void)small->tick_server(small, &s);
		assert(g_snd_by_id[SOUND_MOB_SLIME] == 0);

		/* Hurt + death are both mob.slime at 0.6F (fresh-only). */
		snd_reset();
		assert(entity_monster_apply_damage(med, 4, false)
			   == MONSTER_DAMAGE_FRESH);
		assert(g_snd_by_id[SOUND_MOB_SLIME] == 1
			   && feq(g_snd_vol_by_id[SOUND_MOB_SLIME], 0.6F));
		med->data.monster.hurt_resist_time = 0;
		med->health = 1;
		snd_reset();
		assert(entity_monster_apply_damage(med, 30, false)
			   == MONSTER_DAMAGE_FRESH);
		assert(med->health <= 0);
		assert(g_snd_by_id[SOUND_MOB_SLIME] == 1); /* death == mob.slime */

		/* ATTACK: an accepted contact hit plays mob.slimeattack at the
		   EXPLICIT 1.0F (not getSoundVolume) beside the player's
		   random.hurt. */
		s.player.health = MAX_PLAYER_HEALTH;
		s.player.hurt_resist_time = 0;
		double save_y = s.player.y;
		snd_reset();
		server_local_hurt_player(&s, big);
		assert(g_snd_by_id[SOUND_MOB_SLIMEATTACK] == 1);
		assert(feq(g_snd_vol_by_id[SOUND_MOB_SLIMEATTACK], 1.0F));
		assert(g_snd_by_id[SOUND_RANDOM_HURT] == 1);
		/* An i-frame-absorbed re-hit is silent (attackEntityFrom false). */
		snd_reset();
		server_local_hurt_player(&s, big);
		assert(g_snd_by_id[SOUND_MOB_SLIMEATTACK] == 0
			   && g_snd_by_id[SOUND_RANDOM_HURT] == 0);
		s.player.health = MAX_PLAYER_HEALTH;
		s.player.hurt_resist_time = 0;
		s.player.vel_x = s.player.vel_z = 0.0F;
		s.player.y = save_y;
		/* LEG CLEANUP (the arrow-column coupling this file already documents
		   at its player-position comment ~line 1288): E10's surviving slimes
		   have hopped ~110 player-seeking ticks by now, and their FINAL
		   parking spot depends on every hop heading -- the faithful
		   strafe-wobble + per-tick facing fixes (mob-AI wave, 2026-09-01)
		   legitimately shifted it, parking `big` inside E14's arrow column
		   at (2.5, 8.5) and breaking its random.drr count. Park both
		   survivors in corners no later leg touches (later legs use only
		   x/z in {2.5, 8.5, 10.5..13.5} -- verified by sweep) so no later
		   leg's geometry ever depends on slime hop trajectories again.
		   `med` died above; nothing ticks these two after this leg, so a
		   parked slime is inert. */
		big->teleport(big, (vec3) {15.5F, feet, 15.5F});
		small->teleport(small, (vec3) {0.5F, feet, 15.5F});
		puts("  E10: slime jump>1 / land>2 gates, hurt+death 0.6F, "
			 "slimeattack on accept only");
	}

	/* --- E11 (S2): piston extend/retract through the real move ---------- */
	{
#define TEST_FACE_PX 5 /* Beta piston facing +X (test_piston_move_env) */
		assert(server_world_set_block(
			&s.worlds[0], 2, gy + 1, 2,
			(struct block_data) {.type = BLOCK_PISTON_BASE,
								 .metadata = TEST_FACE_PX}));
		snd_reset();
		/* A standing lit torch as a direct neighbour both powers AND
		   notifies the piston -> the extend fires synchronously NOW. */
		assert(server_world_set_block(
			&s.worlds[0], 2, gy + 1, 1,
			(struct block_data) {.type = BLOCK_REDSTONE_TORCH_LIT,
								 .metadata = 5,
								 .torch_light = 7}));
		assert(g_snd_by_id[SOUND_TILE_PISTON_OUT] == 1);
		assert(feq(g_snd_vol_by_id[SOUND_TILE_PISTON_OUT], 0.5F));
		assert(g_snd_pitch_by_id[SOUND_TILE_PISTON_OUT] >= 0.6F - 1.0e-5F
			   && g_snd_pitch_by_id[SOUND_TILE_PISTON_OUT]
				   <= 0.85F + 1.0e-5F); /* r*0.25+0.6 */
		assert(g_snd_by_id[SOUND_TILE_PISTON_IN] == 0);

		/* Cut the power -> the retract branch plays tile.piston.in
		   unconditionally. */
		snd_reset();
		assert(server_world_set_block(
			&s.worlds[0], 2, gy + 1, 1, (struct block_data) {.type = BLOCK_AIR}));
		assert(g_snd_by_id[SOUND_TILE_PISTON_IN] == 1);
		assert(feq(g_snd_vol_by_id[SOUND_TILE_PISTON_IN], 0.5F));
		assert(g_snd_pitch_by_id[SOUND_TILE_PISTON_IN] >= 0.6F - 1.0e-5F
			   && g_snd_pitch_by_id[SOUND_TILE_PISTON_IN]
				   <= 0.75F + 1.0e-5F); /* r*0.15+0.6 */
		assert(g_snd_by_id[SOUND_TILE_PISTON_OUT] == 0);
#undef TEST_FACE_PX
		puts("  E11: piston extend tile.piston.out / retract tile.piston.in "
			 "(0.5F, Beta pitch windows)");
	}

	/* --- E12 (S2): random.fizz -- lava-mix harden + nether bucket evap -- */
	{
		snd_reset();
		fluid_block_lava_mix_effect(&s, 6, gy + 3, 6, true);
		assert(g_snd_by_id[SOUND_RANDOM_FIZZ] == 1);
		assert(feq(g_snd_vol_by_id[SOUND_RANDOM_FIZZ], 0.5F));
		assert(g_snd_pitch_by_id[SOUND_RANDOM_FIZZ] >= 1.8F - 1.0e-5F
			   && g_snd_pitch_by_id[SOUND_RANDOM_FIZZ]
				   <= 3.4F + 1.0e-5F); /* 2.6+(r-r)*0.8 */
		/* populate-path parity: emit_client=false stays SILENT (draws only). */
		snd_reset();
		fluid_block_lava_mix_effect(&s, 6, gy + 3, 6, false);
		assert(g_snd_count == 0);

		/* Nether water-bucket evaporation: the ONLY bucket sound in b1.7.3.
		   Same fizz formula from the exact two rand_src draws; the water is
		   NOT placed and the bucket still empties. */
		s.world.dimension = WORLD_DIM_NETHER;
		struct block_data clicked;
		assert(server_world_get_block(&s.world, 14, gy, 14, &clicked));
		struct block_info on = {.block = &clicked,
								.neighbours = NULL,
								.x = 14,
								.y = gy,
								.z = 14};
		struct item_data bucket = {.id = ITEM_BUCKET, .count = 1};
		snd_reset();
		assert(item_bucket_water.onUseOnBlock(&s, &s.player, &bucket, &on, SIDE_TOP));
		assert(g_snd_by_id[SOUND_RANDOM_FIZZ] == 1);
		assert(feq(g_snd_vol_by_id[SOUND_RANDOM_FIZZ], 0.5F));
		assert(g_snd_pitch_by_id[SOUND_RANDOM_FIZZ] >= 1.8F - 1.0e-5F
			   && g_snd_pitch_by_id[SOUND_RANDOM_FIZZ] <= 3.4F + 1.0e-5F);
		assert(bucket.id == ITEM_BUCKET); /* emptied, not placed */
		struct block_data evap;
		assert(server_world_get_block(&s.world, 14, gy + 1, 14, &evap));
		assert(evap.type == BLOCK_AIR);
		s.world.dimension = WORLD_DIM_OVERWORLD;
		puts("  E12: harden fizz 0.5F pitch [1.8,3.4]; nether bucket evap "
			 "fizz, silent populate path");
	}

	/* --- E13 (S2): item pickup pop -------------------------------------- */
	{
		s.player.x = 8.5;
		s.player.y = eye;
		s.player.z = 8.5;
		struct item_data dirt = {.id = 3, .durability = 0, .count = 1};
		struct entity* ie = server_local_spawn_item(
			(vec3) {8.5F, feet + 0.5F, 8.5F}, &dirt, false, &s);
		assert(ie);
		ie->data.item.age = 8; /* past the 2s pickup delay */
		snd_reset();
		(void)ie->tick_server(ie, &s);
		assert(g_snd_by_id[SOUND_RANDOM_POP] == 1);
		assert(feq(g_snd_vol_by_id[SOUND_RANDOM_POP], 0.2F));
		assert(g_snd_pitch_by_id[SOUND_RANDOM_POP] >= 0.6F - 1.0e-5F
			   && g_snd_pitch_by_id[SOUND_RANDOM_POP]
				   <= 3.4F + 1.0e-5F); /* ((r-r)*0.7+1)*2 */
		puts("  E13: item pickup -> random.pop 0.2F");
	}

	/* --- E14 (S2): arrow random.drr on stick + pickup pop --------------- */
	{
		/* Materialize the arrow directly (server_local_spawn_arrow only
		   QUEUES into pending_arrows for the tick loop) -- the
		   test_arrow_env pattern: dict slot -> entity_arrow ctor ->
		   teleport. */
		uint32_t aid = 7001;
		struct entity** slot = dict_entity_safe_get(s.entities, aid);
		*slot = malloc(sizeof(struct entity));
		struct entity* arrow = *slot;
		assert(arrow);
		entity_arrow(aid, arrow, true, &s.world, 0, ITEM_ARROW, 4, true);
		arrow->teleport(arrow, (vec3) {2.5F, feet + 3.0F, 8.5F});
		glm_vec3_copy((vec3) {0.0F, -0.9F, 0.0F}, arrow->vel);
		snd_reset();
		int guard = 0;
		while(!arrow->data.arrow.in_ground && guard++ < 12)
			(void)arrow->tick_server(arrow, &s);
		assert(arrow->data.arrow.in_ground);
		assert(g_snd_by_id[SOUND_RANDOM_DRR] == 1);
		assert(feq(g_snd_vol_by_id[SOUND_RANDOM_DRR], 1.0F));
		/* 1.2 / (r*0.2 + 0.9) in [1.0909, 1.3334] */
		assert(g_snd_pitch_by_id[SOUND_RANDOM_DRR] >= 1.09F - 1.0e-3F
			   && g_snd_pitch_by_id[SOUND_RANDOM_DRR] <= 1.334F + 1.0e-3F);

		/* A stuck player arrow under the player's feet collects with the
		   same random.pop as an item. */
		arrow->teleport(arrow, (vec3) {8.5F, feet + 0.2F, 8.5F});
		arrow->data.arrow.shake_ticks = 0;
		snd_reset();
		assert(arrow->tick_server(arrow, &s)); /* collected -> remove */
		assert(g_snd_by_id[SOUND_RANDOM_POP] == 1);
		assert(feq(g_snd_vol_by_id[SOUND_RANDOM_POP], 0.2F));
		puts("  E14: arrow stick -> random.drr (pitch window); stuck pickup "
			 "-> random.pop");
	}

	/* --- E15 (S2): water-entry splash (formula + the mob latch) --------- */
	{
		/* Formula goldens: purely velocity-derived volume, clamp at 1. */
		snd_reset();
		server_local_emit_splash((vec3) {1.0F, 2.0F, 3.0F}, 0.0F, -1.0F,
								 0.0F);
		assert(g_snd_by_id[SOUND_RANDOM_SPLASH] == 1);
		assert(feq(g_snd_vol_by_id[SOUND_RANDOM_SPLASH], 0.2F));
		assert(g_snd_pitch_by_id[SOUND_RANDOM_SPLASH] >= 0.6F - 1.0e-5F
			   && g_snd_pitch_by_id[SOUND_RANDOM_SPLASH] <= 1.4F + 1.0e-5F);
		snd_reset();
		server_local_emit_splash((vec3) {1.0F, 2.0F, 3.0F}, 0.0F, -10.0F,
								 0.0F);
		assert(feq(g_snd_vol_by_id[SOUND_RANDOM_SPLASH], 1.0F)); /* clamped */

		/* Mob latch: dry tick clears the spawn-guard, water entry splashes
		   EXACTLY once, staying wet is silent. */
		struct entity* pig = server_local_spawn_monster_at(
			(vec3) {13.5F, feet, 3.5F}, MONSTER_PIG, &s);
		assert(pig);
		(void)pig->tick_server(pig, &s); /* spawn-guard TRUE -> dry FALSE */
		assert(!pig->data.monster.was_in_water);
		int wx = (int)floorf(pig->pos[0]), wy = (int)floorf(pig->pos[1]),
			wz = (int)floorf(pig->pos[2]);
		for(int dx = -1; dx <= 1; dx++)
			for(int dz = -1; dz <= 1; dz++)
				assert(server_world_set_block(
					&s.worlds[0], wx + dx, wy, wz + dz,
					(struct block_data) {.type = BLOCK_WATER_STILL}));
		snd_reset();
		(void)pig->tick_server(pig, &s);
		assert(g_snd_by_id[SOUND_RANDOM_SPLASH] == 1);
		assert(pig->data.monster.was_in_water);
		snd_reset();
		(void)pig->tick_server(pig, &s);
		assert(g_snd_by_id[SOUND_RANDOM_SPLASH] == 0); /* latched wet */

		/* EXTINGUISH hiss (Beta Entity.moveEntity isWet && fire>0): a
		   burning mob getting wet fizzes ONCE (0.7F, 1.6+-0.4) and the burn
		   clears; already-wet-and-unlit ticks stay silent. */
		pig->data.monster.fire_time = 100;
		snd_reset();
		(void)pig->tick_server(pig, &s);
		assert(g_snd_by_id[SOUND_RANDOM_FIZZ] == 1);
		assert(feq(g_snd_vol_by_id[SOUND_RANDOM_FIZZ], 0.7F));
		assert(g_snd_pitch_by_id[SOUND_RANDOM_FIZZ] >= 1.2F - 1.0e-5F
			   && g_snd_pitch_by_id[SOUND_RANDOM_FIZZ] <= 2.0F + 1.0e-5F);
		/* Water zeroes fire (Entity.java:195); the idle box branch then
		 * rests it at -fireResistance = -1 (:489-490, :52). */
		assert(pig->data.monster.fire_time == -1);
		snd_reset();
		(void)pig->tick_server(pig, &s);
		assert(g_snd_by_id[SOUND_RANDOM_FIZZ] == 0);

		/* Player edge through the production hazard sequence. */
		s.player.fire = 100;
		s.player.oxygen = MAX_OXYGEN;
		snd_reset();
		assert(server_local_test_apply_player_hazards(&s, true, false, false,
													  false));
		assert(g_snd_by_id[SOUND_RANDOM_FIZZ] == 1);
		assert(feq(g_snd_vol_by_id[SOUND_RANDOM_FIZZ], 0.7F));
		/* Entity.java:489-490 with EntityPlayer.fireResistance = 20. */
		assert(s.player.fire == -PLAYER_FIRE_RESISTANCE);
		snd_reset();
		assert(server_local_test_apply_player_hazards(&s, true, false, false,
													  false));
		assert(g_snd_by_id[SOUND_RANDOM_FIZZ] == 0); /* already quenched */
		s.player.oxygen = MAX_OXYGEN;
		puts("  E15: splash formula (0.2 / clamp-1) + mob dry->wet latch "
			 "once + extinguish fizz edges");
	}

	/* --- E16 (S2): the playSoundFX ui routing flag ---------------------- */
	{
		/* portal.trigger/travel ride CRPC_SOUND with ui=true -> the client
		   dispatch routes them to audio_play_ui (non-positional, quarter
		   volume); every positional emit stays ui=false (the zero default of
		   the designated initializers). */
		snd_reset();
		server_local_emit_sound_ui(SOUND_PORTAL_TRIGGER, 1.0F, 0.9F);
		assert(g_snd_by_id[SOUND_PORTAL_TRIGGER] == 1);
		assert(g_snd_ui_by_id[SOUND_PORTAL_TRIGGER]);
		server_local_emit_sound(SOUND_PORTAL_PORTAL, (vec3) {1, 2, 3}, 1.0F,
								0.9F);
		assert(g_snd_by_id[SOUND_PORTAL_PORTAL] == 1);
		assert(!g_snd_ui_by_id[SOUND_PORTAL_PORTAL]);
		puts("  E16: ui-flag routing (playSoundFX path) vs positional "
			 "default");
	}

	/* --- E17 (S2 review fix): item + arrow splash (the full Beta roster) -- */
	{
		/* Beta splashes EVERY super.onUpdate() caller, not just living
		   entities: EntityItem and EntityArrow both reach the
		   Entity.onEntityUpdate edge. [740c583: EntityItem.onUpdate:37,
		   EntityArrow.onUpdate:87] Shared server_local_splash_tick. */
		int wy = (int)floorf(feet);

		/* ITEM: dry tick clears the spawn guard, flooding its cell splashes
		   exactly once, staying wet is silent. */
		struct item_data dirt = {.id = 3, .durability = 0, .count = 1};
		struct entity* ie = server_local_spawn_item(
			(vec3) {10.5F, feet + 0.5F, 10.5F}, &dirt, false, &s);
		assert(ie);
		assert(ie->data.item.was_in_water); /* spawn guard armed */
		(void)ie->tick_server(ie, &s);
		assert(!ie->data.item.was_in_water); /* dry tick cleared it */
		assert(server_world_set_block(
			&s.worlds[0], 10, wy, 10, (struct block_data) {.type = BLOCK_WATER_STILL}));
		snd_reset();
		(void)ie->tick_server(ie, &s);
		assert(g_snd_by_id[SOUND_RANDOM_SPLASH] == 1);
		assert(ie->data.item.was_in_water);
		snd_reset();
		(void)ie->tick_server(ie, &s);
		assert(g_snd_by_id[SOUND_RANDOM_SPLASH] == 0); /* latched wet */

		/* isFirstUpdate guard: a stack spawned INTO water (underwater mining,
		   death drops) never splashes. */
		assert(server_world_set_block(
			&s.worlds[0], 11, wy, 11, (struct block_data) {.type = BLOCK_WATER_STILL}));
		struct entity* ie2 = server_local_spawn_item(
			(vec3) {11.5F, feet + 0.5F, 11.5F}, &dirt, false, &s);
		assert(ie2);
		snd_reset();
		(void)ie2->tick_server(ie2, &s);
		assert(g_snd_by_id[SOUND_RANDOM_SPLASH] == 0);
		assert(ie2->data.item.was_in_water);

		/* ARROW: shot down through a water cell splashes exactly once during
		   the descent (loud at flight speed -- volume is velocity-derived),
		   then embeds below it (the E14 materialization pattern). */
		assert(server_world_set_block(
			&s.worlds[0], 12, wy, 8, (struct block_data) {.type = BLOCK_WATER_STILL}));
		uint32_t aid = 7002;
		struct entity** slot = dict_entity_safe_get(s.entities, aid);
		*slot = malloc(sizeof(struct entity));
		struct entity* arrow = *slot;
		assert(arrow);
		entity_arrow(aid, arrow, true, &s.world, 0, ITEM_ARROW, 4, true);
		arrow->teleport(arrow, (vec3) {12.5F, feet + 3.0F, 8.5F});
		glm_vec3_copy((vec3) {0.0F, -0.9F, 0.0F}, arrow->vel);
		snd_reset();
		int guard = 0;
		while(!arrow->data.arrow.in_ground && guard++ < 12)
			(void)arrow->tick_server(arrow, &s);
		assert(arrow->data.arrow.in_ground);
		assert(g_snd_by_id[SOUND_RANDOM_SPLASH] == 1);
		puts("  E17: item flood-splash once + into-water spawn guard + arrow "
			 "flight splash once");
	}

	/* --- E18: lever click, through the REAL block_lever.c onRightClick --
	   [740c583: BlockLever.java:142] plays "random.click" at the block centre,
	   volume 0.3F, pitched by the NEW on-bit (the old bit flipped) --
	   that is the exact bit this engine's `newmeta & 8` reads, so
	   turning ON plays pitch 0.6F and turning OFF plays 0.5F, always at
	   volume 0.3F. Field regression 2026-09-03: this call was missing
	   entirely from block_lever.c -- not caused by today's
	   audio_sound_table.h minecart edit (git history has NEVER carried a
	   sound call in block_lever.c on any ref; docs/sound_parity_inventory_
	   2026-08-26.md line 68 already recorded lever/button click as
	   "deferred, out of THIS slice"). Restored in block_lever.c; this pins
	   it so it can't silently regress again. */
	{
		const int lx = 14, lz = 14;
		assert(server_world_set_block(
			&s.worlds[0], lx, gy + 1, lz,
			(struct block_data) {.type = BLOCK_LEVER, .metadata = 5}));

		snd_reset();
		lever_click(lx, gy + 1, lz); /* OFF -> ON */
		assert(g_snd_count == 1);
		assert(g_snd_id == SOUND_RANDOM_CLICK);
		assert(feq(g_snd_vol, 0.3F));
		assert(feq(g_snd_pitch, 0.6F));
		assert(feq(g_snd_pos[0], (float)lx + 0.5F)
			   && feq(g_snd_pos[1], (float)(gy + 1) + 0.5F)
			   && feq(g_snd_pos[2], (float)lz + 0.5F));

		snd_reset();
		lever_click(lx, gy + 1, lz); /* ON -> OFF */
		assert(g_snd_count == 1);
		assert(g_snd_id == SOUND_RANDOM_CLICK);
		assert(feq(g_snd_vol, 0.3F));
		assert(feq(g_snd_pitch, 0.5F));

		puts("  E18: lever click -> CRPC_SOUND random.click 0.3 @ 0.6/0.5 "
			 "(on/off) at center");
	}

	/* --- E19: stone button press + release-tick click, through the REAL
	   block_button.c onRightClick / onWorldTick path --
	   [740c583: BlockButton.blockActivated / updateTick]:
	     blockActivated: playSoundEffect(x+0.5,y+0.5,z+0.5, "random.click",
	       0.3F, 0.6F) -- a FIXED press pitch (this branch only runs when the
	       button was OFF, so there is no ON/OFF split like the lever).
	     updateTick (the tickRate()==20 self-scheduled release): the SAME
	       "random.click" 0.3F, but a FIXED 0.5F release pitch.
	   Field regression 2026-09-03 (same gap as the lever, fixed in
	   block_lever.c / 3d761da): block_button.c's onRightClick and
	   onWorldTick never called server_local_emit_sound at all. */
	{
		/* server_world_schedule_block_tick gates on an 8-BLOCK-radius loaded-
		   chunk area (server_world_scheduled_tick_area_loaded) -- main()
		   above loads only chunk (0,0), which is not enough on its own (a
		   radius-8 catchment from anywhere inside a 16-wide chunk spills into
		   a neighbour). Load the surrounding ring so the button's press-time
		   schedule call actually succeeds instead of silently failing. */
		for(int cx = -1; cx <= 1; cx++)
			for(int cz = -1; cz <= 1; cz++) {
				if(cx == 0 && cz == 0)
					continue; /* already loaded in main() */
				struct server_chunk* extra_sc = NULL;
				server_world_load_chunk(&s.world, cx, cz, &extra_sc);
			}

		const int bmx = 1, bx = 2, bz = 6; /* mount wall at bmx, button at bx (meta=1 -> mount x-1) */
		assert(server_world_set_block(
			&s.worlds[0], bmx, gy + 1, bz, (struct block_data) {.type = BLOCK_STONE}));
		assert(server_world_set_block(
			&s.worlds[0], bx, gy + 1, bz,
			(struct block_data) {.type = BLOCK_STONE_BUTTON, .metadata = 1}));

		snd_reset();
		button_press(bx, gy + 1, bz); /* press */
		assert(g_snd_count == 1);
		assert(g_snd_id == SOUND_RANDOM_CLICK);
		assert(feq(g_snd_vol, 0.3F));
		assert(feq(g_snd_pitch, 0.6F));
		assert(feq(g_snd_pos[0], (float)bx + 0.5F)
			   && feq(g_snd_pos[1], (float)(gy + 1) + 0.5F)
			   && feq(g_snd_pos[2], (float)bz + 0.5F));

		/* A second press while already pressed is Beta's blockActivated
		   early-return (pressed bit already set) -- no metadata change, no sound, no
		   re-armed timer. */
		snd_reset();
		button_press(bx, gy + 1, bz);
		assert(g_snd_count == 0);

		/* Drive the self-scheduled tickRate()==20 release via the REAL
		   scheduled-tick queue (the test_dispenser_env.c / test_trapdoor_env.c
		   pattern), asserting the release fires on EXACTLY the 20th tick,
		   not before. */
		snd_reset();
		for(int i = 0; i < 19; i++) {
			server_world_tick(&s.world, &s);
			assert(g_snd_count == 0); /* not yet -- still pressed */
		}
		server_world_tick(&s.world, &s); /* tick 20: the release fires */
		assert(g_snd_count == 1);
		assert(g_snd_id == SOUND_RANDOM_CLICK);
		assert(feq(g_snd_vol, 0.3F));
		assert(feq(g_snd_pitch, 0.5F));
		assert(feq(g_snd_pos[0], (float)bx + 0.5F)
			   && feq(g_snd_pos[1], (float)(gy + 1) + 0.5F)
			   && feq(g_snd_pos[2], (float)bz + 0.5F));

		puts("  E19: stone button press -> random.click 0.3 @ 0.6, "
			 "tickRate()==20 self-release -> random.click 0.3 @ 0.5");
	}

	clin_test_set_rpc_observer(NULL, NULL);
	g_drain_run = 0;
	thread_join(&drain);
	puts("test_audio_env: all sections OK");
	test_fixture_dir_cleanup(TMP_WORLD);
	test_fixture_dir_cleanup(TMP_PACK);
	return 0;
}
