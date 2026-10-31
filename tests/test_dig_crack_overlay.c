/*
	push-0921/B18 -- review B17 finding H1: THE CRACK OVERLAY MUST AGE ON THE
	DIG'S OWN CLOCK.

	The dig is timed with tool_dig_delay_env_ms(block, held, in_water, on_ground)
	(source/game/gui/screen_ingame.c:566, which reads both terms off the very
	session it is digging for). The ten-stage break overlay used to recompute its
	own delay with the two-argument tool_dig_delay_ms() -- Beta's DRY, ON-GROUND
	case -- so the moment the player was submerged or airborne the two clocks
	disagreed by the whole 5x / 25x penalty: the block sat visually fully cracked
	for 82% of a submerged bare-handed dirt dig (stage 9 at 675 ms of 3750) and
	96% of a submerged airborne one (675 ms of 18750). This file pins that they
	agree, for every environment and for all three tool arms.

	WHAT BETA DOES (the spec this is written against), decompile 740c583 at
	<repo-analysis>/mcb173-retromcp/minecraft/
	src/net/minecraft/src/:

	  PlayerControllerSP.java:72   curBlockDamage += the block's blockStrength(...)
	  PlayerControllerSP.java:78   curBlockDamage >= 1.0F breaks the block
	  PlayerControllerSP.java:102  t = prev + (cur - prev) * partialTick;
	                               renderGlobal.damagePartialTime = t
	  RenderGlobal.java:1099       renderBlockUsingTexture(..., 240
	                                   + (int)(damagePartialTime * 10.0F))

	There is exactly ONE accumulator: `curBlockDamage`. The break fires when it
	reaches 1.0F and the overlay is floor(that same value * 10). So the overlay
	is a pure function of dig PROGRESS, and it reaches its last stage (9) at
	progress 0.9 of whatever the break is going to take -- never earlier,
	never later, whatever the environment. `blockStrength` (Block.java:313-315)
	is where the environment enters, through getCurrentPlayerStrVsBlock, and it
	is the SAME call for both, which is the property this file asserts.

	This engine models the dig as an integer millisecond budget instead of a
	float sum, so the transcription is:

	    progress = elapsed_ms / delay_ms          (delay from tool_dig_delay_env_ms)
	    stage    = floor(progress * 10), clamped to 9

	and because tool_dig_delay_env_ms always rounds up to a whole 50 ms tick,
	delay/10 is exact and `elapsed / (delay/10)` IS floor(elapsed*10/delay) --
	the production expression, with no intermediate that could overflow.

	NOT A POISONED GOLDEN: every number below is derived from the decompile
	rule above plus the delay table that tests/test_dig_speed_parity.c
	hand-derived from EntityPlayer.java:291-302 (its file header shows the
	arithmetic). Nothing here was produced by running this engine's overlay and
	recording what it said.

	THE THREE TOOL ARMS, deliberately covered on both dirt and stone, because
	they do NOT all take the environment penalty -- a test that only checked
	"delay got 5x longer" would pass a wrong implementation:
	  * hand-harvestable + no tool bonus (dirt, bare hand)  -- env applies
	  * the matching tool                (dirt, wooden shovel / stone, wooden
	                                      pickaxe)          -- env applies
	  * the cannot-harvest arm           (stone, bare hand / stone, wooden
	                                      shovel)           -- env does NOT
	    apply: Block.blockStrength reaches getCurrentPlayerStrVsBlock only
	    through its canHarvestBlock arm, so Beta digs stone bare-handed in the
	    same 150 ticks standing, swimming or falling (test_dig_speed_parity's
	    stone rows and mc173 break.rs:38-53 both agree).
	  * dirt + wooden pickaxe is the fourth shape: a tool that is the WRONG
	    class for a block that is still hand-harvestable, which falls to the
	    plain `delay *= env` arm.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "item/items.h"
#include "item/tool.h"

/* dry+grounded, in water, airborne, in water AND airborne -- the same four
   environments and the same spelling tests/test_dig_speed_parity.c uses. */
enum { ENV_DRY, ENV_WATER, ENV_AIR, ENV_BOTH, ENV_COUNT };
static const bool env_in_water[ENV_COUNT] = {false, true, false, true};
static const bool env_on_ground[ENV_COUNT] = {true, true, false, false};
static const char* env_name[ENV_COUNT]
	= {"dry+on ground", "in water", "airborne", "in water + airborne"};

static struct item* held(uint16_t id) {
	if(!id)
		return NULL;
	struct item_data d = {.id = id, .count = 1};
	struct item* it = item_get(&d);
	assert(it && "tool item id not registered");
	return it;
}

struct dig_case {
	const char* block_name;
	struct block* blk;
	const char* tool_name;
	uint16_t item; /* 0 = bare hand */
};

static const struct dig_case cases[] = {
	{"dirt", &block_dirt, "bare hand", 0},
	{"dirt", &block_dirt, "wooden shovel", ITEM_WOOD_SHOVEL},
	{"dirt", &block_dirt, "wooden pickaxe", ITEM_WOOD_PICKAXE},
	{"stone", &block_stone, "bare hand", 0},
	{"stone", &block_stone, "wooden pickaxe", ITEM_WOOD_PICKAXE},
	{"stone", &block_stone, "wooden shovel", ITEM_WOOD_SHOVEL},
};

/* (1) THE AGREEMENT. For every case x environment: the overlay's last stage
   must begin at 90% of the delay the DIG will actually take, and the stage
   immediately before that boundary must still be 8. Both halves matter --
   "stage 9 by the time it breaks" alone is satisfied by the broken code (it
   gets there far too early), and "not 9 before the end" alone is satisfied by
   an overlay that never advances at all. */
static void check_agreement(void) {
	unsigned red = 0;
	for(unsigned c = 0; c < sizeof cases / sizeof *cases; c++) {
		const struct dig_case* k = &cases[c];
		for(int e = 0; e < ENV_COUNT; e++) {
			int delay = tool_dig_delay_env_ms(k->blk, held(k->item),
											  env_in_water[e],
											  env_on_ground[e]);
			assert(delay > 0 && "these six cases all dig");
			int bucket = delay / 10; /* exact: delay is a whole 50 ms tick */
			assert(bucket * 10 == delay
				   && "a delay that is not a whole 10 ms breaks the bucket");

			int last = tool_dig_crack_stage(k->blk, held(k->item),
											env_in_water[e], env_on_ground[e],
											9 * bucket);
			int before = tool_dig_crack_stage(k->blk, held(k->item),
											  env_in_water[e],
											  env_on_ground[e],
											  9 * bucket - 1);
			if(last != 9 || before != 8) {
				printf("  RED %s + %s, %s: delay %d ms -> stage at %d ms = %d "
					   "(want 9), at %d ms = %d (want 8)\n",
					   k->block_name, k->tool_name, env_name[e], delay,
					   9 * bucket, last, 9 * bucket - 1, before);
				red++;
			}

			/* The overlay must not run AHEAD of the dig anywhere in between,
			   either: stage is floor(progress*10) at every tick boundary. */
			for(int tick = 0; tick <= 10; tick++) {
				int elapsed = tick * bucket;
				int want = tick >= 9 ? 9 : tick;
				int got = tool_dig_crack_stage(k->blk, held(k->item),
											   env_in_water[e],
											   env_on_ground[e], elapsed);
				if(got != want) {
					printf("  RED %s + %s, %s: %d/10 of a %d ms dig -> stage "
						   "%d, want %d\n",
						   k->block_name, k->tool_name, env_name[e], tick,
						   delay, got, want);
					red++;
				}
			}
		}
	}
	assert(red == 0 && "crack overlay disagrees with the dig it is drawing");
	printf("test_dig_crack_overlay: agreement: %u cases x %d environments, "
		   "every tenth of the dig maps to its own stage and 9 begins at "
		   "exactly 90%%\n",
		   (unsigned)(sizeof cases / sizeof *cases), ENV_COUNT);
}

/* (2) THE REGRESSION, in the reviewer's own numbers. Submerged bare-handed
   dirt: the real delay is 3750 ms, the dry one is 750 ms. An overlay computed
   off the DRY delay saturates at 675 ms and then sits frozen for the remaining
   3075 ms -- 82% of the dig. Pin the two figures directly so the failure text
   names the defect rather than an anonymous stage number. */
static void check_h1_numbers(void) {
	const int dry = tool_dig_delay_env_ms(&block_dirt, NULL, false, true);
	const int wet = tool_dig_delay_env_ms(&block_dirt, NULL, true, true);
	const int both = tool_dig_delay_env_ms(&block_dirt, NULL, true, false);
	assert(dry == 750 && "test_dig_speed_parity's dirt/hand dry row");
	assert(wet == 3750 && "test_dig_speed_parity's dirt/hand in-water row");
	assert(both == 18750 && "dirt/hand in water AND airborne");

	/* At 675 ms -- the moment the DRY clock saturates -- a submerged dig is
	   675/3750 = 18% done, so Beta is on stage 1. */
	int s = tool_dig_crack_stage(&block_dirt, NULL, true, true, 675);
	if(s != 1)
		printf("  RED submerged dirt at 675 ms: stage %d, want 1 (675/3750 = "
			   "18%% of the dig; the dry clock would say 9 here and then stay "
			   "there for the remaining %d ms)\n",
			   s, wet - 675);
	assert(s == 1);

	/* Submerged AND airborne: 675 ms is 3.6% of the dig -- stage 0. */
	s = tool_dig_crack_stage(&block_dirt, NULL, true, false, 675);
	if(s != 0)
		printf("  RED submerged+airborne dirt at 675 ms: stage %d, want 0 "
			   "(675/18750 = 3.6%% of the dig)\n",
			   s);
	assert(s == 0);

	/* And the dry case is UNMOVED: this fix must not touch the overwhelmingly
	   common dig. 675 ms of 750 is 90% -- stage 9, exactly as it shipped. */
	assert(tool_dig_crack_stage(&block_dirt, NULL, false, true, 675) == 9);
	assert(tool_dig_crack_stage(&block_dirt, NULL, false, true, 674) == 8);
	assert(tool_dig_crack_stage(&block_dirt, NULL, false, true, 0) == 0);

	puts("test_dig_crack_overlay: H1 numbers: submerged dirt is stage 1 (not "
		 "9) at 675 ms of its 3750 ms dig; submerged+airborne is stage 0 of "
		 "18750 ms; the dry 750 ms dig is unmoved");
}

/* (3) THE CANNOT-HARVEST ARM TAKES NO PENALTY, and the overlay must follow it
   rather than assume every environment stretches the dig. Stone bare-handed is
   7500 ms in all four environments (Block.blockStrength's non-canHarvestBlock
   ternary arm never reaches getCurrentPlayerStrVsBlock), so its crack timeline
   is identical in all four too -- an implementation that multiplied the
   overlay by `env` independently would break exactly here. */
static void check_no_penalty_arm(void) {
	for(int e = 0; e < ENV_COUNT; e++) {
		assert(tool_dig_delay_env_ms(&block_stone, NULL, env_in_water[e],
									 env_on_ground[e])
			   == 7500);
		for(int tick = 0; tick <= 9; tick++) {
			int want = tick >= 9 ? 9 : tick;
			assert(tool_dig_crack_stage(&block_stone, NULL, env_in_water[e],
										env_on_ground[e], tick * 750)
				   == want);
		}
	}
	puts("test_dig_crack_overlay: stone bare-handed cracks identically in all "
		 "four environments (the cannot-harvest arm takes no 5x penalty)");
}

/* (4) THE DIVIDE-BY-ZERO FLOOR SURVIVED THE MOVE. A never-digs block
   (hardness <= 0) returns -1, whose /10 truncates to 0; the floor at 1 is what
   keeps that from being a SIGFPE. Pinned here as well as in
   test_block_cracks_texture_override.c because the arithmetic now lives in
   tool.c and that file mirrors it rather than calling it. */
static void check_never_digs_floor(void) {
	assert(tool_dig_delay_env_ms(&block_bedrock, NULL, false, true) == -1);
	for(int e = 0; e < ENV_COUNT; e++) {
		int s = tool_dig_crack_stage(&block_bedrock, NULL, env_in_water[e],
									 env_on_ground[e], 0);
		assert(s == 0);
		s = tool_dig_crack_stage(&block_bedrock, NULL, env_in_water[e],
								 env_on_ground[e], 500);
		assert(s == 9); /* divisor floored to 1, then clamped -- never a trap */
	}
	puts("test_dig_crack_overlay: a never-digs block (-1 delay) still computes "
		 "a stage instead of dividing by zero");
}

int main(void) {
	blocks_init();
	items_init();

	check_agreement();
	check_h1_numbers();
	check_no_penalty_arm();
	check_never_digs_floor();

	puts("test_dig_crack_overlay: all assertions passed");
	return 0;
}
