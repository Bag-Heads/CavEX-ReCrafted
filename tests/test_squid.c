/*
	test_squid -- pins the Beta 1.7.3 squid swim physics (M2 final units),
	against the PeytonPlayz595 b1.7.3 decompile (raw-fetched + quoted
	2026-06-11, docs/m2_final_units_research.md). Binds the SHARED combat.h
	helpers the engine executes, drift-proof. All values are Beta's raw bytes
	-- this engine integrates pos += vel per tick exactly like Beta.

	Pinned:
	  1. Swim-vector re-roll: angle = u*2PI; vec = {cos*0.2, -0.1 + u*0.2,
	     sin*0.2} -- horizontal magnitude exactly 0.2, vertical in [-0.1, 0.1).
	  2. Re-pick gate, Beta's exact clause order/semantics: nextInt(50)==0 ||
	     !inWater || vec-zero.
	  3. Propulsion pulse: thrust (speed=1.0) kicks ONLY past 75% of the
	     contraction stroke (phase < PI); the glide half decays *= 0.9.
	  4. Beached fall: motionY = (motionY - 0.08) * 0.98, horizontal zeroed by
	     the caller -- and its terminal velocity matches Beta (-3.92).
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-5f;
}

int main(void) {
	// --- 1. swim vector ---------------------------------------------------------
	{
		float v[3];
		// u_angle 0 -> +X; u_y 0 -> the -0.1 vertical floor.
		squid_pick_swim_vector(0.0f, 0.0f, v);
		assert(feq(v[0], 0.2f) && feq(v[1], -0.1f) && feq(v[2], 0.0f));
		// u_angle 0.25 -> +Z (angle PI/2); u_y just under 1 -> just under +0.1.
		squid_pick_swim_vector(0.25f, 0.999999f, v);
		assert(fabsf(v[0]) < 1.0e-6f && feq(v[2], 0.2f));
		assert(v[1] > 0.0999f && v[1] < 0.1f);
		// Any draw: horizontal magnitude is EXACTLY 0.2 (cos/sin * 0.2).
		for(int k = 0; k < 16; k++) {
			squid_pick_swim_vector((float)k / 16.0f, 0.5f, v);
			assert(feq(sqrtf(v[0] * v[0] + v[2] * v[2]), 0.2f));
			assert(v[1] >= -0.1f && v[1] < 0.1f);
		}
	}

	// --- 2. re-pick gate --------------------------------------------------------
	assert(squid_should_repick(true, true, false));   // the 1-in-50 hit
	assert(squid_should_repick(false, false, false)); // out of water
	assert(squid_should_repick(false, true, true));   // zero vector (fresh spawn)
	assert(!squid_should_repick(false, true, false)); // swimming on a live vector

	// --- 3. propulsion pulse ----------------------------------------------------
	{
		float pi = (float)M_PI;
		// Contraction half before the 75% kick: speed unchanged.
		assert(feq(squid_pulse_speed(0.5f * pi, 0.37f), 0.37f));
		assert(feq(squid_pulse_speed(0.74f * pi, 0.37f), 0.37f));
		// Past 75% of the stroke: thrust latches 1.0 regardless of prior speed.
		assert(feq(squid_pulse_speed(0.76f * pi, 0.0f), 1.0f));
		assert(feq(squid_pulse_speed(0.99f * pi, 0.42f), 1.0f));
		// Glide half: *= 0.9 decay.
		assert(feq(squid_pulse_speed(1.5f * pi, 1.0f), 0.9f));
		assert(feq(squid_pulse_speed(1.9f * pi, 0.9f), 0.81f));
		// Full cycle from rest: kick to 1.0, then ~PI/0.15 glide ticks of decay
		// -- speed stays in (0, 1].
		float speed = 0.0f, phase = 0.0f;
		for(int t = 0; t < 200; t++) {
			phase += 0.15f;
			if(phase > 2.0f * pi)
				phase -= 2.0f * pi;
			speed = squid_pulse_speed(phase, speed);
			assert(speed >= 0.0f && speed <= 1.0f);
		}
		assert(speed > 0.0f); // the pulse keeps re-kicking every cycle
	}

	// --- 4. beached fall --------------------------------------------------------
	assert(feq(squid_fall_step(0.0f), -0.0784f)); // (0 - 0.08) * 0.98
	{
		float v = 0.0f;
		for(int t = 0; t < 2000; t++)
			v = squid_fall_step(v);
		// Beta terminal: v = (v - 0.08) * 0.98 -> -0.08*0.98/0.02 = -3.92.
		assert(feq(v, -3.92f));
	}

	// --- 5. water-probe boundaries (2026-06-12 audit fix) ------------------------
	// Beta band derivation (combat.h): over a water cell at yw, the MOTION probe
	// (-0.6-shrunk live isInWater override) goes dry at foot > yw+0.4 (TIGHT edge);
	// the REPICK-gate probe (-0.4-shrunk cached inWater field) goes dry at
	// foot >= yw+0.6 (LOOSE edge). The old foot-cell sample stayed wet to yw+1.0
	// -- the breach-and-hover hardware report. Pin both edges either side, with
	// yw=7 as the water cell.
	{
		// MOTION (tight) edge at foot = yw + 0.4:
		assert(squid_motion_probe_cell_y(7.39f) == 7); // wet side: probes the water
		assert(squid_motion_probe_cell_y(7.41f) == 8); // dry side: probes above it
		// GATE (loose) edge at foot = yw + 0.6:
		assert(squid_gate_probe_cell_y(7.59f) == 7);
		assert(squid_gate_probe_cell_y(7.61f) == 8);
		// The hysteresis band (0.4, 0.6): motion beached, gate still wet -- the
		// heading must be HELD here (no re-roll thrash while surface-bobbing).
		assert(squid_motion_probe_cell_y(7.5f) == 8);
		assert(squid_gate_probe_cell_y(7.5f) == 7);
		// Deep water sanity: well below the surface both probes stay in water.
		assert(squid_motion_probe_cell_y(5.2f) == 5);
		assert(squid_gate_probe_cell_y(5.2f) == 5);

		// EMPTY-LOOP BAND (review fix): Beta's inverted -0.6 box covers NO cell
		// for frac(foot) in [0.4, 0.65) -- unconditionally dry even fully
		// submerged (the sink-bias mechanism). Either side covers exactly one.
		assert(squid_motion_loop_covers_cell(5.39f));  // frac 0.39: covered
		assert(!squid_motion_loop_covers_cell(5.41f)); // frac 0.41: EMPTY
		assert(!squid_motion_loop_covers_cell(5.5f));  // mid-band: EMPTY
		assert(!squid_motion_loop_covers_cell(5.64f)); // frac 0.64: EMPTY
		assert(squid_motion_loop_covers_cell(5.66f));  // frac 0.66: covered
		// Surface edge consistency: at foot 7.41 over water cell 7 the old
		// single-point probe said dry-via-air-cell; the loop says dry-via-empty
		// -- same verdict at every surface, divergence only when submerged.
		assert(!squid_motion_loop_covers_cell(7.41f));
	}

	printf("test_squid: OK (swim vector, re-pick gate, pulse, beached fall, "
		   "probe boundaries)\n");
	return 0;
}
