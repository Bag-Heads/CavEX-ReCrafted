/*
	The audio PCM arena moved OUT of the MEM2 lwp carve into its own reserved
	region (mem2.h MEM2_AUDIO_REGION_BYTES / mem2_audio_region). The Wii branch
	of mem2.c is the only place the region math runs, and the PC branch is plain
	malloc, so what this file guards is the ARITHMETIC and the SEAM, not the
	lwp_heap:

	  A. mem2_plan_regions goldens, incl. the CLIPPED case that real hardware
	     actually lands on (arena2 ~51.8MB cannot hold 36 + 12 + 4MB, so the
	     audio region is the one that clips — the documented priority), the
	     independence property the whole change is about (the carve size does
	     not depend on the audio want), and the BUDGET golden that ties the
	     shipping split to the measured sound-pack size: the clipped grant must
	     still hold the whole real Beta pack, or the console goes partly silent.
	  B. the SEAM — the arena is obtained through mem2_audio_region, NOT through
	     the general mem2_alloc carve path. Pinned by the mem2 accounting
	     counters (a registry load must request ZERO bytes from mem2_alloc) and
	     by the decoded PCM landing inside the region the seam handed out.
	  C. FAIL-SOFT on a short grant: the registry stays functional with fewer
	     resident samples, reports the shortfall through audio_arena_total()
	     (the F3 "snd: arena used/total" denominator — the only sink that
	     exists in the NDEBUG Wii build), survives a reload into the SAME short
	     region, and never hands out a sample for a key it could not load.

	ORDER IS LOAD-BEARING. mem2_audio_region is first-caller-wins by contract
	(that is what makes audio_registry_reload safe), so C claims a deliberately
	tiny region BEFORE audio_init and every later call in this process gets the
	same short grant back. The full-size grant is pinned by test_audio_env,
	which asserts audio_arena_total() == 12MB on the normal boot path.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/game_state.h"
#include "platform/audio.h"
#include "platform/mem2.h"

#define MB(n) ((size_t)(n) * 1024u * 1024u)
#define KB(n) ((size_t)(n) * 1024u)

/* The shipping wants, restated here on purpose: a test that reads the constant
   it is checking proves nothing about a change to that constant. The two that
   ARE reachable from the header are then pinned equal to the restatement, so a
   future edit to either fails HERE (loudly, with the arithmetic in front of the
   reader) instead of quietly re-clipping the audio region on hardware. */
/* 2026-09-10 repartition: carve 38.5 MiB + margin 1.5 MiB = the same 40 MiB
   the 36 + 4 split took, so the audio remainder below is unchanged. */
#define WANT_CARVE KB(39424)
#define WANT_AUDIO MB(12)
#define WANT_MARGIN KB(1536)
_Static_assert(WANT_CARVE == MEM2_HEAP_SIZE,
			   "shipping carve want changed -- re-derive the split below, and "
			   "re-check the pack-residency golden before accepting it");
_Static_assert(WANT_AUDIO == MEM2_AUDIO_REGION_BYTES,
			   "shipping audio want changed -- re-derive the split below");

/* The measured arena2: docs/platform_perf_notes.md's MEM2-migration table read
   36627KB free above the then-16MB carve, so arena2 is 36627 + 16384 = 53011KB.
   This is the case the console actually boots into. */
#define REAL_ARENA2 KB(53011)

/* The full real Beta 1.7.3 sound pack as measured on the owned pack
   (tmp/jobs/c2-audio-coverage-0917/report.md): 15,078,752 B of aligned PCM16
   resident (171 candidate files). That does NOT fit the 12,443,648 B region
   the console actually grants, so resident loading alone drops 15 files.
   (The old "9.87MB, fits" figure here was stale.) */
#define REAL_PACK_FULL_BYTES ((size_t)15078752)
/* What the shipped default (audio.deferred_cave ON, maintainer ruling
   2026-10-21) keeps resident: the full pack minus the 13 cave variants
   (4,404,544 B) plus the one cave scratch (largest variant 588,800 B) and
   the bounded decoder block -- about 11.3 MB. THIS is the number the
   carve/audio trade has to clear. */
#define REAL_PACK_BYTES ((size_t)11300000)

/* ===== A: region-plan goldens ========================================== */

static void test_plan_roomy(void) {
	struct mem2_region_plan p;
	/* A hypothetical arena2 that fits every want with room to spare. */
	assert(mem2_plan_regions(MB(64), WANT_CARVE, WANT_AUDIO, WANT_MARGIN, &p));
	assert(p.carve_off == 0);
	assert(p.carve_size == WANT_CARVE);
	assert(p.audio_off == WANT_CARVE);
	assert(p.audio_size == MB(12));
	assert(p.reserved_total == WANT_CARVE + WANT_AUDIO);
	assert(p.spill_size == MB(64) - (WANT_CARVE + WANT_AUDIO));
	/* The regions tile [0, reserved_total) with no gap and no overlap. */
	assert(p.carve_off + p.carve_size == p.audio_off);
	assert(p.audio_off + p.audio_size == p.reserved_total);
	assert(p.reserved_total + p.spill_size == MB(64));
	puts("  A1: roomy arena2 -> 36MB carve + 12MB audio + 16MB spill OK");
}

static void test_plan_real_hardware_clips_audio(void) {
	struct mem2_region_plan p;
	assert(mem2_plan_regions(REAL_ARENA2, WANT_CARVE, WANT_AUDIO, WANT_MARGIN,
							 &p));
	/* THE POINT OF THE CHANGE: the carve gets its full want... */
	assert(p.carve_size == WANT_CARVE);
	/* ...and AUDIO is what clips, because a short audio region degrades to
	   missing sounds while a short carve reproduces the display-list spill
	   storm that wedged the GP. 53011 - 39424 - 1536 = 12051KB (the same
	   12051KB the 36 + 4 split gave: the repartition moved bytes between the
	   carve and the margin only). */
	assert(p.audio_size == KB(12051));
	assert(p.audio_size < WANT_AUDIO);
	/* The margin is never spent: it is the fallback path's landing zone. */
	assert(p.spill_size == WANT_MARGIN);
	assert(p.audio_off == p.carve_size);
	assert(p.reserved_total == p.carve_size + p.audio_size);
	puts("  A2: real arena2 (53011KB) -> carve keeps 36MB, audio clips to "
		 "12051KB OK");
}

static void test_plan_real_pack_stays_resident(void) {
	/* THE BUDGET GOLDEN. The region is clipped, so "it fail-softs" is not good
	   enough on its own -- what matters is WHERE it clips relative to the real
	   sound set. A grant below REAL_PACK_BYTES means the last-registered keys
	   (the S2 cave/portal/fire moods) go silent on the console, which is a
	   user-visible quality loss no test above would catch: the fail-soft paths
	   would all still pass, quietly, with less sound.

	   This is the assertion that makes MEM2_HEAP_SIZE unsafe to raise casually.
	   At 36MB the ~11.3MB deferred set has ~0.7MB of spare (the FULL 15.08MB
	   pack never fit); the old 9.87MB-based margins no longer apply. Every megabyte added to the carve comes straight out of
	   here. */
	struct mem2_region_plan p;
	assert(mem2_plan_regions(REAL_ARENA2, WANT_CARVE, WANT_AUDIO, WANT_MARGIN,
							 &p));
	assert(p.audio_size >= REAL_PACK_BYTES);
	/* ...and the FULL pack cannot be resident: that is why deferred is the
	   default. Fail loudly if the region ever grows past it, so the
	   deviation's premise is re-examined. */
	assert(p.audio_size < REAL_PACK_FULL_BYTES);
	printf("  A2b: deferred-default resident set -- %uKB granted vs %uKB needed "
		   "(%uKB spare) OK\n",
		   (unsigned)(p.audio_size / 1024), (unsigned)(REAL_PACK_BYTES / 1024),
		   (unsigned)((p.audio_size - REAL_PACK_BYTES) / 1024));

	/* Directional proof that the dial is the carve and nothing else: the
	   retired 40MB want could NOT hold the pack at the same arena2 with the
	   4MB margin it shipped beside (53011 - 40960 - 4096 = 7955KB). Kept at
	   that historic margin on purpose: the 2026-09-10 repartition moved 2.5MB
	   from the margin INTO the carve, so 40MB + 1.5MB would now leave 10515KB
	   -- the same audio squeeze as any carve growth that is not paid for by
	   the margin. */
	struct mem2_region_plan old;
	assert(mem2_plan_regions(REAL_ARENA2, MB(40), WANT_AUDIO, MB(4), &old));
	assert(old.audio_size == KB(7955));
	assert(old.audio_size < REAL_PACK_BYTES);
	assert(old.spill_size == MB(4)); /* that margin was untouched either way */
}

static void test_plan_carve_independent_of_audio(void) {
	/* The regression this whole task exists to prevent: the audio arena must
	   not be able to shrink the carve, whatever it asks for. */
	struct mem2_region_plan none, small, want, huge;
	assert(mem2_plan_regions(REAL_ARENA2, WANT_CARVE, 0, WANT_MARGIN, &none));
	assert(mem2_plan_regions(REAL_ARENA2, WANT_CARVE, MB(1), WANT_MARGIN,
							 &small));
	assert(mem2_plan_regions(REAL_ARENA2, WANT_CARVE, WANT_AUDIO, WANT_MARGIN,
							 &want));
	assert(mem2_plan_regions(REAL_ARENA2, WANT_CARVE, MB(999), WANT_MARGIN,
							 &huge));
	assert(none.carve_size == small.carve_size);
	assert(none.carve_size == want.carve_size);
	assert(none.carve_size == huge.carve_size);
	assert(none.carve_size == WANT_CARVE);
	/* An unbounded audio want still cannot eat the margin. */
	assert(huge.spill_size == WANT_MARGIN);
	assert(none.audio_size == 0 && none.spill_size == REAL_ARENA2 - WANT_CARVE);
	puts("  A3: carve size independent of the audio want OK");
}

static void test_plan_tight_and_degenerate(void) {
	struct mem2_region_plan p;

	/* Exactly carve + margin: audio gets nothing, and says so. */
	assert(mem2_plan_regions(WANT_CARVE + WANT_MARGIN, WANT_CARVE, WANT_AUDIO,
							 WANT_MARGIN, &p));
	assert(p.carve_size == WANT_CARVE && p.audio_size == 0);
	assert(p.reserved_total == WANT_CARVE && p.spill_size == WANT_MARGIN);

	/* Below the carve want: the carve itself clips (and THAT is the state
	   mem2_stat_carve_spill exists to catch on the next console session). */
	assert(mem2_plan_regions(MB(20), WANT_CARVE, WANT_AUDIO, WANT_MARGIN, &p));
	assert(p.carve_size == MB(20) - WANT_MARGIN && p.audio_size == 0);
	assert(p.spill_size == WANT_MARGIN);

	/* One byte of usable budget is still a plan; zero is not. */
	assert(mem2_plan_regions(WANT_MARGIN + 1, WANT_CARVE, WANT_AUDIO,
							 WANT_MARGIN, &p));
	assert(p.carve_size == 1 && p.audio_size == 0);

	/* No room at all -> false, and the caller must leave arena2 alone. */
	memset(&p, 0xAB, sizeof(p));
	assert(!mem2_plan_regions(WANT_MARGIN, WANT_CARVE, WANT_AUDIO, WANT_MARGIN,
							  &p));
	assert(p.carve_size == 0 && p.audio_size == 0 && p.reserved_total == 0);
	memset(&p, 0xAB, sizeof(p));
	assert(!mem2_plan_regions(0, WANT_CARVE, WANT_AUDIO, WANT_MARGIN, &p));
	assert(p.reserved_total == 0);
	/* A zero carve want is "no usable carve", not "give it all to audio":
	   mem2_init must bail rather than publish a heapless region. */
	assert(!mem2_plan_regions(MB(64), 0, WANT_AUDIO, WANT_MARGIN, &p));
	assert(!mem2_plan_regions(MB(64), WANT_CARVE, WANT_AUDIO, MB(64), &p));
	assert(!mem2_plan_regions(MB(64), WANT_CARVE, WANT_AUDIO, WANT_MARGIN,
							  NULL));
	puts("  A4: tight / degenerate plans OK");
}

static void test_plan_audio_base_aligned(void) {
	struct mem2_region_plan p;
	/* The DSP DMA needs a 32-byte-aligned arena base, and audio_arena_alloc can
	   only PRESERVE alignment (it bumps by aligned sizes), never create it. A
	   carve size that is not a multiple of 32 must therefore be padded, and the
	   pad must come out of the audio region -- never out of the spill margin. */
	assert(mem2_plan_regions(MB(64), MB(40) + 1, WANT_AUDIO, WANT_MARGIN, &p));
	assert(p.carve_size == MB(40) + 1);
	assert(p.audio_off % MEM2_REGION_ALIGN == 0);
	assert(p.audio_off == MB(40) + MEM2_REGION_ALIGN);
	assert(p.audio_size == MB(12));
	assert(p.spill_size >= WANT_MARGIN);
	assert(p.reserved_total == p.audio_off + p.audio_size);

	/* The shipping wants are already aligned, so the pad is zero there -- pin
	   that too, or a future MEM2_HEAP_SIZE edit silently shifts every offset. */
	assert(mem2_plan_regions(REAL_ARENA2, WANT_CARVE, WANT_AUDIO, WANT_MARGIN,
							 &p));
	assert(p.audio_off == p.carve_size);
	assert(p.audio_off % MEM2_REGION_ALIGN == 0);

	/* No audio region -> no pad either: the carve alone is what gets fenced. */
	assert(mem2_plan_regions(MB(20), MB(40) + 1, WANT_AUDIO, WANT_MARGIN, &p));
	assert(p.audio_size == 0);
	assert(p.audio_off == p.carve_size && p.reserved_total == p.carve_size);
	puts("  A5: audio region base is 32-byte aligned (DSP DMA) OK");
}

/* ===== B+C: the seam, under a deliberately SHORT grant ================== */

/* Small enough that most of the 46 keys cannot fit, large enough that the
   early ones do -- so "fewer samples, still functional" is observable. */
#define SHORT_GRANT KB(64)

static size_t g_short_grant = 0;
static const uint8_t* g_region = NULL;

static void test_short_reservation_latched(void) {
	size_t got = 0;
	void* p = mem2_audio_region(SHORT_GRANT, &got);
	assert(p && got == SHORT_GRANT);
	g_region = (const uint8_t*)p;
	g_short_grant = got;

	/* FIRST CALLER WINS: this is the contract audio_registry_reload leans on
	   (it re-fills the SAME arena in place), so a later, larger request must
	   return the identical block -- not a second region. */
	size_t again = 0;
	void* q = mem2_audio_region(MB(12), &again);
	assert(q == p && again == SHORT_GRANT);
	assert(mem2_stat_audio_region_size() == SHORT_GRANT);
	puts("  B1: mem2_audio_region grant is latched (reload-safe) OK");
}

static void test_arena_bypasses_mem2_alloc(void) {
	/* THE SEAM. Before this change the arena was one mem2_alloc(12MB) out of
	   the display-list carve; now a whole registry load must not request a
	   single byte from the carve allocator. */
	size_t calls0 = mem2_stat_alloc_calls();
	size_t bytes0 = mem2_stat_alloc_bytes();

	audio_init(false, 0); /* headless: registry + arena, no device */

	assert(mem2_stat_alloc_bytes() == bytes0);
	assert(mem2_stat_alloc_calls() == calls0);
	/* And the region really is the arena the registry filled. */
	assert(audio_arena_total() == g_short_grant);
	assert(audio_arena_used() > 0 && audio_arena_used() <= audio_arena_total());
	puts("  B2: registry load requests 0 bytes from mem2_alloc OK");
}

static int loaded_key_count(void) {
	int n = 0;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++)
		if(audio_registry_variant_count(id) > 0)
			n++;
	return n;
}

static void test_short_grant_fail_soft(void) {
	/* Shortfall REPORTED, not swallowed: audio_arena_total() is the granted
	   size, which is exactly what the F3 row prints as the denominator. */
	assert(audio_arena_total() < MB(12));
	assert(audio_arena_total() == mem2_stat_audio_region_size());

	/* Fewer resident samples -- but the registry is functional, not dead. */
	int keys = loaded_key_count();
	assert(keys > 0);
	assert(keys < AUDIO_SOUND_COUNT); /* a 64KB arena cannot hold all 46 */

	/* Every key that DID load hands out a usable sample that lives inside the
	   reserved region; every key that did not is silent, not a dangling read. */
	int checked = 0;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT; id++) {
		const struct audio_sample* smp = audio_registry_pick(id);
		if(audio_registry_variant_count(id) == 0) {
			assert(smp == NULL);
			continue;
		}
		assert(smp && smp->pcm && smp->frames > 0 && smp->rate > 0);
		const uint8_t* pcm = (const uint8_t*)smp->pcm;
		assert(pcm >= g_region && pcm < g_region + g_short_grant);
		assert(pcm + (size_t)smp->frames * 2 <= g_region + g_short_grant);
		checked++;
	}
	assert(checked == keys);

	/* Play paths stay no-op-safe with a half-loaded registry. */
	audio_play3d(0, (vec3) {0, 0, 0}, 1.0F, 1.0F);
	audio_play_ui(0, 1.0F, 1.0F);
	audio_update((vec3) {0, 0, 0}, 0.0F);
	assert(audio_voices_active() == 0);
	printf("  C1: short grant fail-soft (%d/%d keys resident, %u/%uB) OK\n",
		   keys, (int)AUDIO_SOUND_COUNT, (unsigned)audio_arena_used(),
		   (unsigned)audio_arena_total());
}

static void test_reload_reuses_short_region(void) {
	size_t used0 = audio_arena_used();
	int keys0 = loaded_key_count();
	size_t calls0 = mem2_stat_alloc_calls();

	audio_registry_reload(); /* drains voices, re-fills the SAME arena */

	assert(mem2_stat_alloc_calls() == calls0); /* still no carve traffic */
	assert(audio_arena_total() == g_short_grant); /* same short region */
	assert(mem2_stat_audio_region_size() == g_short_grant);
	assert(audio_arena_used() == used0); /* byte-exact re-fill */
	assert(loaded_key_count() == keys0);
	const struct audio_sample* smp = NULL;
	for(uint16_t id = 0; id < AUDIO_SOUND_COUNT && !smp; id++)
		smp = audio_registry_pick(id);
	assert(smp);
	assert((const uint8_t*)smp->pcm >= g_region
		   && (const uint8_t*)smp->pcm < g_region + g_short_grant);
	puts("  C2: reload re-fills the SAME clipped region in place OK");
}

int main(void) {
	puts("test_mem2_audio_region");
	test_plan_roomy();
	test_plan_real_hardware_clips_audio();
	test_plan_real_pack_stays_resident();
	test_plan_carve_independent_of_audio();
	test_plan_tight_and_degenerate();
	test_plan_audio_base_aligned();

	test_short_reservation_latched();
	test_arena_bypasses_mem2_alloc();
	test_short_grant_fail_soft();
	test_reload_reuses_short_region();

	/* PC has no carve, so the accounting claim above is the whole PC-side
	   statement about it; the Wii carve size is pinned by A3's independence
	   golden, which is the same arithmetic mem2_init runs. */
	assert(mem2_stat_carve_size() == 0);
	puts("test_mem2_audio_region OK");
	return 0;
}
