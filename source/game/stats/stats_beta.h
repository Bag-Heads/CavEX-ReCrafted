/* Copyright (c) 2026 CaveWii_v2 contributors */
/*
 * Beta 1.7.3's statistic ROUNDING, as pure functions.
 *
 * Spec: docs/achievements_design.md 1.4 rows 2000-2008 and 2002; decompile
 * `EntityPlayer.addMovementStat` / `addMountedMovementStat` / `fall(float)`
 * (net.minecraft.src.EntityPlayer, lines 822-901 of the Jad output) and
 * `MathHelper.sqrt_double` (line 30: `return (float)Math.sqrt(d)`).
 *
 * WHY A MODULE AND NOT A LINE AT EACH EMIT SITE: lane A adds five movement
 * emit sites (5.3) that must all produce the SAME centimetre Beta produces.
 * Beta's own arithmetic is not one formula but three different cast chains,
 * and the difference is observable -- 0.035 blocks of motion is 4 cm through
 * the horizontal/3-D chain and 3 cm through the climb chain, because one
 * rounds a float and the other a double (test_stats_counters pins exactly
 * that pair). Re-typing a chain per site is how one of them silently becomes
 * `(int)(dist * 100)`. So the chains live here once, tested, and every site
 * calls them.
 *
 * All of these are PURE: no state, no clock, no allocation, safe from any
 * thread. They do not emit; the caller decides whether Beta would have
 * counted (the `_counts` predicates below) and emits the centimetre.
 *
 * WHAT "Math.round" MEANS HERE. Java 6/7 -- the runtime Beta 1.7.3 shipped
 * against -- specify and implement `Math.round(float)` as
 * `(int)Math.floor(a + 0.5f)` and `Math.round(double)` as
 * `(long)Math.floor(a + 0.5d)`, which is what the two static helpers in
 * stats_beta.c reproduce (and what the rest of this engine already uses for
 * Java rounding: `furnace_logic.c:31`, `item_bed.c:46`, `item_repeater.c:37`).
 * Java 8 changed `Math.round(float)` to avoid the double-rounding of the
 * single value `0x1.fffffep-2f` (0.49999997f), which the old form rounds up
 * to 1 and the new form to 0. No distance this engine can produce lands on
 * that one float, and the Beta-era behaviour is the spec, so the old form is
 * reproduced deliberately rather than "fixed".
 *
 * TOTAL, NEVER UNDEFINED (R2 LOW 10). A C cast from a floating-point value to
 * an integer type is undefined for a NaN or an out-of-range value; Java's
 * conversions are specified for every input, and these functions reproduce
 * Java's, so a corrupt payload produces Java's number instead of whatever the
 * host happens to do (or a trap). The three rules, all JLS 5.1.3: float/double
 * -> int and -> long give 0 for NaN and SATURATE at the type's extremes (so
 * the infinities become MAX/MIN); long -> int is a NARROWING conversion that
 * discards all but the low 32 bits. `Math.round(float)` returns an int and
 * therefore saturates; `Math.round(double)` returns a long and Beta then
 * narrows it with its own `(int)`, which wraps. The two chains legitimately
 * disagree at the extremes -- `Math.round(3e9f)` is Integer.MAX_VALUE,
 * `(int)Math.round(3e9d)` is -1294967296 -- and both are reproduced as written
 * rather than reconciled into one "sane" answer.
 */

#ifndef CW_STATS_BETA_H
#define CW_STATS_BETA_H

#include <stdbool.h>
#include <stdint.h>

/* `Math.round(MathHelper.sqrt_double(d * d + d2 * d2) * 100F)`
 * [EntityPlayer.addMovementStat, the swimOneCm / walkOneCm / flyOneCm arms].
 *
 * The chain, in Beta's order: the squares and their sum in DOUBLE, the square
 * root in double and then TRUNCATED TO FLOAT (sqrt_double returns a float),
 * the * 100F in FLOAT, and a float Math.round. Rounding the double product
 * instead is a different number -- that is the point of this function. */
int32_t stats_beta_horizontal_cm(double dx, double dz);

/* `Math.round(MathHelper.sqrt_double(d * d + d1 * d1 + d2 * d2) * 100F)`
 * [EntityPlayer.addMovementStat diveOneCm arm; addMountedMovementStat for
 * minecartOneCm / boatOneCm / pigOneCm]. Same chain, three terms. */
int32_t stats_beta_3d_cm(double dx, double dy, double dz);

/* `(int)Math.round(d1 * 100D)` [EntityPlayer.addMovementStat, the isOnLadder
 * arm]. A DOUBLE chain: no square root, no float anywhere.
 *
 * Beta gates this arm on `d1 > 0.0D` -- the raw double, BEFORE rounding --
 * and then adds the rounded value with NO `> 0` check of its own, so a climb
 * of less than half a centimetre legitimately adds 0. The caller applies the
 * dy > 0 gate; this function does not second-guess it. */
int32_t stats_beta_climb_cm(double dy);

/* `if(f >= 2.0F) addStat(distanceFallenStat, (int)Math.round((double)f * 100D))`
 * [EntityPlayer.fall(float)]. The gate is part of the formula: a fall under
 * two blocks yields 0, and 0 is what the caller must not emit. The widening
 * `(double)f` is Beta's own, so the multiply and the rounding are double. */
int32_t stats_beta_fall_cm(float f);

/* The two emit gates of addMovementStat, as predicates so a call site cannot
 * re-type the wrong comparison. flyOneCm is the odd one out: Beta counts an
 * airborne move only past 25 cm (`if(l > 25)`), where every other arm counts
 * anything positive (`if(i > 0)`), which is why a stair-step hop adds walking
 * but not flying. */
bool stats_beta_fly_counts(int32_t cm);

/* `if(i > 0)` -- walkOneCm, swimOneCm, diveOneCm and all three mounted rows
 * [addMovementStat, addMountedMovementStat]. Not climbOneCm: see
 * stats_beta_climb_cm(). */
bool stats_beta_walk_counts(int32_t cm);

#endif /* CW_STATS_BETA_H */
