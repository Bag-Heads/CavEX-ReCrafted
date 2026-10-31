/*
	Pins the PURE decision logic for the audit #17f emissive-tint overlay -- the
	"when / how-much to tint" that drives the FIFO-safe gfx_entity_tint() on both the
	GX (Wii) and GL (PC) backends. The GX/GL calls themselves are not unit-testable
	(they touch the live graphics pipeline), so the decision logic was factored into
	source/entity/combat.h and is the subject here:

	  - creeper_flash_amount(fuse, td): the Beta RenderCreeper white-swell pulse --
	    0 when idle / in the OFF half of the toggle; on the ON half, a CONTINUOUS
	    ramp toward the CREEPER_FLASH_TINT_AMOUNT ceiling as the raw (unclamped)
	    fuse fraction approaches 1, not a flat/fixed amount. The toggle itself
	    accelerates toward detonation. [740c583: RenderCreeper.
	    updateCreeperColorMultiplier -- parity fix 2026-08-31]
	  - hurt_flash_active(hurt_time): the red hurt-flash predicate (active iff the
	    mob's Beta hurt animation is active, hurt_time > 0).
	  - tint_amount_clamp(amount): clamps the lerp amount to [0,1]; 0 == the model
	    drawn exactly as today (the graceful-degrade contract both backends rely on).

	Binds the SHARED source/entity/combat.h so these are the exact helpers the server
	send (hurt flag) + the client render (creeper/hurt tint) use.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"

// --- tint_amount_clamp: the graceful-degrade contract ------------------------
static void test_amount_clamp(void) {
	// 0 and negatives clamp to exactly 0 -- the "render as today" no-op both
	// backends early-out on. (A GX issue can then only drop the overlay, never the
	// model.)
	assert(tint_amount_clamp(0.0f) == 0.0f);
	assert(tint_amount_clamp(-0.5f) == 0.0f);
	assert(tint_amount_clamp(-1000.0f) == 0.0f);
	// >1 clamps to fully tinted.
	assert(tint_amount_clamp(1.0f) == 1.0f);
	assert(tint_amount_clamp(2.0f) == 1.0f);
	// In-range values pass through unchanged.
	assert(tint_amount_clamp(0.2f) == 0.2f);
	assert(tint_amount_clamp(0.3f) == 0.3f);
	// The two effect amounts are valid (in-range) tint amounts.
	assert(tint_amount_clamp(CREEPER_FLASH_TINT_AMOUNT) == CREEPER_FLASH_TINT_AMOUNT);
	assert(tint_amount_clamp(HURT_FLASH_TINT_AMOUNT) == HURT_FLASH_TINT_AMOUNT);
}

// --- hurt_flash_active: the red hurt-flash predicate -------------------------
static void test_hurt_predicate(void) {
	// Active for the whole persisted hurt animation (hurt_time > 0), off at 0.
	assert(!hurt_flash_active(0));
	assert(hurt_flash_active(1));
	assert(hurt_flash_active(MOB_HURT_FLASH_TICKS)); // just-hit: full window
	// A fresh hit arms both clocks to 10, but hurtTime is the distinct animation
	// clock that drives this predicate and is the only one persisted by Beta.
	assert(MOB_HURT_FLASH_TICKS == MOB_HURT_RESIST_TICKS);
	assert(MOB_HURT_FLASH_TICKS == 10);
	// A fresh hit arms hurt_time to MOB_HURT_ANIMATION_TICKS and counts down; the flash
	// must be active across that entire countdown (10 -> 1) and off once it hits 0.
	bool ever_off_while_positive = false, off_at_zero;
	for(int t = MOB_HURT_ANIMATION_TICKS; t > 0; t--)
		if(!hurt_flash_active(t))
			ever_off_while_positive = true;
	off_at_zero = !hurt_flash_active(0);
	assert(!ever_off_while_positive); // ON for the whole 10-tick window
	assert(off_at_zero);              // OFF when the hurt animation expires
}

// --- creeper_flash_amount: the Beta white-swell pulse ------------------------
static void test_creeper_flash_idle(void) {
	// Not primed (fuse < 0) -> never flashes, regardless of td. This is the graceful
	// no-op for the overwhelming majority of creepers on screen.
	assert(creeper_flash_amount(-1, 0.0f) == 0.0f);
	assert(creeper_flash_amount(-1, 0.5f) == 0.0f);
	assert(creeper_flash_amount(-1, 0.99f) == 0.0f);
}

static void test_creeper_flash_color_is_white(void) {
	// The swell wash is pure white (RenderCreeper brightens toward white).
	assert(CREEPER_FLASH_TINT_R == 1.0f);
	assert(CREEPER_FLASH_TINT_G == 1.0f);
	assert(CREEPER_FLASH_TINT_B == 1.0f);
	// Faithful RenderCreeper 0.2f coefficient.
	assert(CREEPER_FLASH_TINT_AMOUNT == 0.2f);
}

// parity fix 2026-08-31 (dossier finding "White swell-flash tint amplitude"): this
// test used to be named test_creeper_flash_amount_is_binary and PINNED the bug --
// it asserted every "on" sample equalled the flat CREEPER_FLASH_TINT_AMOUNT. Beta's
// updateCreeperColorMultiplier has no such flat toggle: the "on" bucket's alpha
// ramps continuously with the raw fuse fraction (nearly invisible early in the fuse,
// intensifying toward the ceiling only near detonation). [740c583: RenderCreeper.
// updateCreeperColorMultiplier]
static void test_creeper_flash_amount_ramps_not_flat(void) {
	bool saw_on = false, saw_off = false;
	float min_on = 1.0f, max_on = 0.0f;
	for(int fuse = 0; fuse <= CREEPER_FUSE_TICKS; fuse++) {
		for(int k = 0; k < 4; k++) {
			float a = creeper_flash_amount(fuse, 0.25f * (float)k);
			assert(a >= 0.0f && a <= 1.0f); // always a valid backend amount
			if(a == 0.0f) {
				saw_off = true;
			} else {
				saw_on = true;
				if(a < min_on)
					min_on = a;
				if(a > max_on)
					max_on = a;
			}
		}
	}
	assert(saw_on);
	assert(saw_off);
	// A FLAT toggle (the old bug) makes every "on" sample identical. A continuous
	// ramp spans a real range across the fuse sweep.
	assert(max_on - min_on > 0.01f);
	// The dimmest "on" pulse (early in the fuse) is well below the 0.2 ceiling --
	// nearly invisible, not maxed out from the very first pulse.
	assert(min_on < CREEPER_FLASH_TINT_AMOUNT * 0.5f);
	// The brightest "on" pulse (near detonation) approaches/slightly exceeds the
	// nominal ceiling (the raw f can run up to ~30/28 ~= 1.071 in the final tick
	// before the [0,1] output clamp catches it).
	assert(max_on >= CREEPER_FLASH_TINT_AMOUNT * 0.95f);
}

static void test_creeper_flash_matches_beta_toggle(void) {
	// The amount must equal Beta's exact formula: bucket-off -> 0; bucket-on ->
	// f * CREEPER_FLASH_TINT_AMOUNT clamped to [0,1], where f = (fuse+td)/
	// (CREEPER_FUSE_TICKS-2) is left RAW/UNCLAMPED -- updateCreeperColorMultiplier
	// never clamps its var5 (unlike updateCreeperScale's f^4 term, which does).
	// Re-derive it independently here and assert equality across the range -- pins
	// the exact pulse cadence AND amplitude. [740c583: RenderCreeper.
	// updateCreeperColorMultiplier]
	for(int fuse = 0; fuse <= CREEPER_FUSE_TICKS + 4; fuse++) {
		for(int k = 0; k < 4; k++) {
			float td = 0.25f * (float)k;
			float f = ((float)fuse + td) / (float)(CREEPER_FUSE_TICKS - 2);
			float expect;
			if(((int)(f * 10.0f) & 1) == 0) {
				expect = 0.0f;
			} else {
				expect = f * CREEPER_FLASH_TINT_AMOUNT;
				if(expect < 0.0f)
					expect = 0.0f;
				if(expect > 1.0f)
					expect = 1.0f;
			}
			assert(creeper_flash_amount(fuse, td) == expect);
		}
	}
}

// --- red hurt color stays on the Beta 1.7.3 (red) side, not the 1.8 soft tint --
static void test_hurt_color_is_red(void) {
	// Beta RenderLiving.java:74 glColor4f(var25, 0, 0, 0.4F): pure red scaled by the
	// model's brightness (G == B == 0), a partial wash (< 1, never a full replace). The
	// (1,0.6,0.6) soft multiply is a 1.8+ look. Exact values: test_hurt_wash_parity.
	struct hurt_wash lit = hurt_wash_for_brightness(1.0f);
	assert(lit.r > lit.g && lit.r > lit.b);
	assert(HURT_FLASH_TINT_G == 0.0f && HURT_FLASH_TINT_B == 0.0f);
	assert(HURT_FLASH_TINT_AMOUNT > 0.0f && HURT_FLASH_TINT_AMOUNT < 1.0f);
}

// --- backend graceful-degrade: BOTH effect amounts are valid [0,1] tints --------
static void test_effect_amounts_in_range(void) {
	// Both effect coefficients must survive the backend clamp unchanged (i.e. already
	// be in [0,1]); a value outside the range would mean the backend silently altered
	// the intended look. Pins the "the named constants are usable as-is" contract.
	assert(tint_amount_clamp(CREEPER_FLASH_TINT_AMOUNT) == CREEPER_FLASH_TINT_AMOUNT);
	assert(tint_amount_clamp(HURT_FLASH_TINT_AMOUNT) == HURT_FLASH_TINT_AMOUNT);
	// Every creeper-flash amount the pulse can emit is also a valid backend amount
	// (so the GX KCOLOR0 alpha byte / GL uniform never need clamping at the call site).
	for(int fuse = -1; fuse <= CREEPER_FUSE_TICKS; fuse++)
		for(int k = 0; k < 4; k++) {
			float a = creeper_flash_amount(fuse, 0.25f * (float)k);
			assert(tint_amount_clamp(a) == a);
		}
}

// --- render-path PRECEDENCE invariant: red hurt-flash wins over creeper white ---
// entity_render picks the hurt RED tint whenever the mob is hurt, ELSE the creeper
// white pulse (a creeper you just hit flashes red, not white -- matching Beta's
// RendererLivingEntity hurt overlay drawn over the model). This pins the pure
// decision the renderer's branch encodes: re-derive the SAME selection here and
// assert the hurt branch shadows the creeper branch across the fuse range.
static void test_hurt_precedence_over_creeper(void) {
	for(int fuse = -1; fuse <= CREEPER_FUSE_TICKS; fuse++) {
		float td = 0.5f;
		// When hurt, the chosen amount is the hurt amount, regardless of fuse phase --
		// the creeper pulse is NOT consulted.
		float chosen_when_hurt
			= hurt_flash_active(MOB_HURT_FLASH_TICKS) ? HURT_FLASH_TINT_AMOUNT
													  : creeper_flash_amount(fuse, td);
		assert(chosen_when_hurt == HURT_FLASH_TINT_AMOUNT);
		// When NOT hurt, the creeper pulse drives the amount (white), never the red.
		float chosen_when_calm
			= hurt_flash_active(0) ? HURT_FLASH_TINT_AMOUNT
								   : creeper_flash_amount(fuse, td);
		assert(chosen_when_calm == creeper_flash_amount(fuse, td));
	}
}

int main(void) {
	test_amount_clamp();
	test_hurt_predicate();
	test_creeper_flash_idle();
	test_creeper_flash_color_is_white();
	test_creeper_flash_amount_ramps_not_flat();
	test_creeper_flash_matches_beta_toggle();
	test_hurt_color_is_red();
	test_effect_amounts_in_range();
	test_hurt_precedence_over_creeper();
	printf("test_emissive_tint: tint decision logic pinned (creeper white pulse ramps"
		   " to a %.1f ceiling, red hurt-flash %d-tick window, amount clamp [0,1])\n",
		   (double)CREEPER_FLASH_TINT_AMOUNT, MOB_HURT_FLASH_TICKS);
	return 0;
}
