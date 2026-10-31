/*
	test_slime -- pins the Beta 1.7.3 slime mechanics (M2 final units), all
	against the PeytonPlayz595 b1.7.3 decompile (raw-fetched + quoted
	2026-06-11, docs/m2_final_units_research.md). Binds the SHARED
	monsters_object.h / combat.h helpers the engine executes, drift-proof.

	Pinned:
	  1. Size-as-id mapping: 1 << nextInt(3) -> ids SMALL/MEDIUM/BIG; Beta
	     size numbers 1/2/4; child chain BIG -> MEDIUM -> SMALL -> none.
	  2. Hop schedule: slimeJumpDelay = nextInt(20) + 10 (10..29), INTEGER
	     divided by 3 with a player near (3..9).
	  3. Contact damage (onCollideWithPlayer): damage == size, size > 1 only
	     -- small 0 (harmless, must not even windup), medium 16, big 32 on
	     the player's 160-pt scale (Beta half-hearts * 8).
	  4. Split offsets (setEntityDead): EXACTLY 4 children at the
	     (+-size/8, +-size/8) 2x2 grid corners -- the four i in 0..3 give
	     ((i%2)-0.5)*size/4 / ((i/2)-0.5)*size/4.
	  5. The hop impulse constant exists in pre-gravity engine units and
	     nets ~1.1 blocks against GRAVITY 2.0 / 0.9 damp (the documented
	     feel mapping; the Beta byte 0.42 does not transplant).
	  6. FIX (mob-AI wave, 2026-09-01) -- updatePlayerActionState [740c583]:
	     hop speed now scales with the faithful size-scaled base (moveForward
	     = getSlimeSize()) through the EXISTING mob_speed_cap_wii threshold;
	     the strafe wobble (moveStrafing = 1-nextFloat()*2) is now drawn and
	     applied as a heading rotation whose angle is atan2(strafe, size); and
	     slime_face_player_yaw pins faceEntity(player,10,20)'s unconditional
	     16-block facing gate, independent of the hop-delay timer. RNG note:
	     the strafe roll is a NEW draw on the pinned rand_src (sim) stream,
	     positioned right after the existing jump-delay reroll -- no golden
	     test pins a full slime tick's rand_src draw sequence (grepped
	     tests/), so this is covered by the deterministic-seed behaviour
	     assertions below instead of a beta-tick-golden.
	  7. FIX (audit 2026-09-01, REGRESSION-TODAY, CONFIRMED) --
	     slime_hop_steer_heading: item 6's own wobble rotation was only ever
	     exercised with a UNIT steer input above -- an idle/out-of-range
	     slime's real steer heading is the EXACT zero vector (the ctor's and
	     mob_path_follow_tail's shared no-path convention), and rotating
	     zero stays zero at any angle, so such a slime hopped straight up
	     with NO horizontal component ("slime hops straight up"). The new
	     helper substitutes a unit heading from the slime's own facing yaw
	     whenever the steer is exactly zero, BEFORE the wobble rotation
	     runs; draws no RNG of its own and does not perturb the pinned
	     rand_src sequence from item 6.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"
#include "entity/monsters_object.h"

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-5f;
}

int main(void) {
	// --- 1. size-as-id ----------------------------------------------------------
	assert(monster_is_slime(MONSTER_SLIME_SMALL));
	assert(monster_is_slime(MONSTER_SLIME_MEDIUM));
	assert(monster_is_slime(MONSTER_SLIME_BIG));
	assert(!monster_is_slime(MONSTER_SQUID));
	assert(!monster_is_slime(MONSTER_ZOMBIE));
	assert(!monster_is_slime(MONSTER_SHEEP));
	// Beta constructor roll: 1 << nextInt(3) -> 1, 2, 4 == SMALL, MEDIUM, BIG.
	assert(slime_size_from_roll(0) == MONSTER_SLIME_SMALL);
	assert(slime_size_from_roll(1) == MONSTER_SLIME_MEDIUM);
	assert(slime_size_from_roll(2) == MONSTER_SLIME_BIG);
	// Beta size numbers.
	assert(slime_beta_size(MONSTER_SLIME_SMALL) == 1);
	assert(slime_beta_size(MONSTER_SLIME_MEDIUM) == 2);
	assert(slime_beta_size(MONSTER_SLIME_BIG) == 4);
	assert(slime_beta_size(MONSTER_ZOMBIE) == 0); // non-slimes carry no size
	assert(slime_beta_size(MONSTER_SQUID) == 0);
	// Child chain: setSlimeSize(size/2); size-1 slimes never split.
	assert(slime_child_id(MONSTER_SLIME_BIG) == MONSTER_SLIME_MEDIUM);
	assert(slime_child_id(MONSTER_SLIME_MEDIUM) == MONSTER_SLIME_SMALL);
	assert(slime_child_id(MONSTER_SLIME_SMALL) == -1);
	assert(slime_child_id(MONSTER_ZOMBIE) == -1);
	assert(slime_child_id(MONSTER_SQUID) == -1);

	// --- 2. hop schedule --------------------------------------------------------
	// nextInt(20) + 10: bounds 10..29 idle.
	assert(slime_jump_delay(0, false) == 10);
	assert(slime_jump_delay(19, false) == 29);
	// Player near: INTEGER /3 (Beta slimeJumpDelay /= 3): 10/3=3 .. 29/3=9.
	assert(slime_jump_delay(0, true) == 3);
	assert(slime_jump_delay(19, true) == 9);
	assert(slime_jump_delay(5, true) == 5);   // 15 / 3
	assert(slime_jump_delay(6, true) == 5);   // 16 / 3 -- integer division, not round
	for(int r = 0; r < 20; r++) {
		int idle = slime_jump_delay(r, false);
		int aggro = slime_jump_delay(r, true);
		assert(idle >= 10 && idle <= 29);
		assert(aggro == idle / 3); // exactly Beta's integer divide
	}

	// --- 3. contact damage ------------------------------------------------------
	assert(mob_melee_damage(MONSTER_SLIME_SMALL) == 0);   // size 1: harmless
	assert(mob_melee_damage(MONSTER_SLIME_MEDIUM) == 16); // size 2 -> 2 * 8
	assert(mob_melee_damage(MONSTER_SLIME_BIG) == 32);    // size 4 -> 4 * 8
	// The existing rows are untouched (regression guard).
	assert(mob_melee_damage(MONSTER_ZOMBIE) == 40);
	assert(mob_melee_damage(-1) == 16 && mob_melee_damage(9999) == 16);

	// --- 4. split offsets -------------------------------------------------------
	// Beta: x += ((i%2)-0.5)*size/4, z += ((i/2)-0.5)*size/4 -- the 2x2 corner
	// grid at +-size/8. Verify all four corners for both splitting sizes.
	for(int sz = 2; sz <= 4; sz += 2) {
		float want = (float)sz / 8.0f; // +-size/8 in each axis
		bool corner_seen[2][2] = {{false, false}, {false, false}};
		for(int i = 0; i < 4; i++) {
			float dx, dz;
			slime_split_offset(i, sz, &dx, &dz);
			assert(feq(fabsf(dx), want) && feq(fabsf(dz), want));
			corner_seen[dx > 0.0f][dz > 0.0f] = true;
		}
		// all four (+-,+-) combinations occur exactly once each
		assert(corner_seen[0][0] && corner_seen[0][1] && corner_seen[1][0]
			   && corner_seen[1][1]);
	}

	// --- 4b. split GATE (parity fix 2026-09-02) ---------------------------------
	// Beta EntitySlime.setEntityDead (decompile-verified, 740c583):
	//     if (!this.worldObj.multiplayerWorld && var1 > 1 && this.health == 0)
	//         { ...split... }
	// STRICT `health == 0`, not <=0: overkill damage (health driven well
	// negative by one big hit) reaches the same death-flop reap edge but must
	// NOT split. `child_id >= 0` is exactly `var1 > 1` (a size-1 slime's
	// child id is -1). The engine previously split on `child_id >= 0` alone
	// (no health term), so ANY death -- including overkill -- always split.
	{
		int big_child = slime_child_id(MONSTER_SLIME_BIG);
		int med_child = slime_child_id(MONSTER_SLIME_MEDIUM);
		int small_child = slime_child_id(MONSTER_SLIME_SMALL);
		assert(big_child >= 0 && med_child >= 0 && small_child < 0);

		// Exact health==0 death: splits (both splitting sizes).
		assert(slime_should_split(big_child, 0));
		assert(slime_should_split(med_child, 0));
		// Overkill (health driven negative): must NOT split, even though the
		// same death-flop reap edge is reached.
		assert(!slime_should_split(big_child, -1));
		assert(!slime_should_split(big_child, -20));
		assert(!slime_should_split(med_child, -5));
		// A positive health can't reach this gate in practice (the caller
		// only invokes it once health<=0 put the mob into the death flop),
		// but the predicate itself is a plain equality -- confirm it is
		// exactly as strict for a stray positive value too.
		assert(!slime_should_split(big_child, 1));
		// Size-1 (no child): never splits regardless of health, health==0
		// included -- `var1 > 1` gates it independently of the health term.
		assert(!slime_should_split(small_child, 0));
		assert(!slime_should_split(small_child, -1));
		printf("  split gate: health==0 splits, overkill (health<0) does NOT, "
			   "size-1 never does OK\n");
	}

	// --- 5. hop impulse: Beta's jump() (mob motion units S2) ------------------
	// The engine-units 3.2 hop is retired: a slime hop is EntityLiving.jump(),
	// motionY = (double)0.42F, integrated by Beta's move-then-gravity
	// ((v - 0.08D) * (double)0.98F); the apex is ~1.25 blocks.
	{
		double y = 0.0, v = (double)0.42F, apex = 0.0;
		for(int t = 0; t < 20 && v > 0.0; t++) {
			y += v;
			v = (v - 0.08) * (double)0.98F;
			if(y > apex)
				apex = y;
		}
		assert(apex > 1.2 && apex < 1.3);
	}

	// --- 6a. FIX 1: hop speed scales with size -----------------------------------
	// Beta moveForward = getSlimeSize() (1/2/4). The faithful size-scaled base
	// (def move_speed 0.7 * beta_size) now feeds mob_speed_cap_wii, whose OWN
	// 2-tier threshold (>0.7 -> raised) is unchanged: small (0.7*1=0.7, NOT >0.7)
	// stays at the base Wii cap; medium (0.7*2=1.4) and big (0.7*4=2.8) both
	// cross the threshold into the raised tier -- the bug this closes was ALL
	// THREE landing on the identical base-tier value.
	{
		float base_small = monster_def_get(MONSTER_SLIME_SMALL)->move_speed
			* (float)slime_beta_size(MONSTER_SLIME_SMALL);
		float base_medium = monster_def_get(MONSTER_SLIME_MEDIUM)->move_speed
			* (float)slime_beta_size(MONSTER_SLIME_MEDIUM);
		float base_big = monster_def_get(MONSTER_SLIME_BIG)->move_speed
			* (float)slime_beta_size(MONSTER_SLIME_BIG);
		assert(feq(base_small, 0.7f));
		assert(feq(base_medium, 1.4f));
		assert(feq(base_big, 2.8f));
		float cap_small = mob_speed_cap_wii(base_small);
		float cap_medium = mob_speed_cap_wii(base_medium);
		float cap_big = mob_speed_cap_wii(base_big);
		assert(feq(cap_small, MOB_MAX_SPEED_WII));
		assert(feq(cap_medium, MOB_MAX_SPEED_WII * MOB_SPIDER_SPEED_MULT_WII));
		assert(feq(cap_big, MOB_MAX_SPEED_WII * MOB_SPIDER_SPEED_MULT_WII));
		// The OLD bug: feeding the flat move_speed alone gave every size the
		// identical base-tier cap. Prove the fix actually changed the outcome
		// for medium/big (small is unchanged by design -- size 1 stays == 1x).
		assert(!feq(cap_medium, mob_speed_cap_wii(
			monster_def_get(MONSTER_SLIME_MEDIUM)->move_speed)));
		assert(!feq(cap_big, mob_speed_cap_wii(
			monster_def_get(MONSTER_SLIME_BIG)->move_speed)));
	}
	printf("  slime FIX 1: hop speed scales with the faithful size base "
		   "(small base-tier, medium/big raised-tier) OK\n");

	// --- 6b. FIX 2: random strafe wobble rotates the hop heading -----------------
	// Beta moveStrafing = 1.0F - nextFloat()*2.0F, range (-1,1]; the resulting
	// heading deviates from pure-forward by atan2(strafe, size) (Entity.moveFlying
	// citation in combat.h). Pin the roll formula, the rotation's zero-wobble
	// identity, the max deviation at |strafe|==1 (size-DEPENDENT: a small slime
	// wobbles far more than a big one), and that the rotation preserves the unit
	// heading's length (only the angle changes, matching the faithful magnitude
	// analysis in combat.h).
	{
		// roll01 in [0,1) -> strafe in (-1,1], endpoints and the neutral midpoint.
		assert(feq(slime_strafe_roll(0.0f), 1.0f));
		assert(feq(slime_strafe_roll(0.5f), 0.0f));
		// roll01 -> 1.0 (exclusive bound) -> strafe -> -1.0 (never reached exactly,
		// but the formula's limit is pinned so the mapping direction can't drift).
		assert(feq(slime_strafe_roll(0.999999f), 1.0f - 0.999999f * 2.0f));

		// Zero strafe -> zero rotation -> heading passes through unchanged.
		float wx, wz;
		slime_hop_wobble_dir(1.0f, 0.0f, 0.0f, 4, &wx, &wz);
		assert(feq(wx, 1.0f) && feq(wz, 0.0f));
		slime_hop_wobble_dir(0.0f, 1.0f, 0.0f, 1, &wx, &wz);
		assert(feq(wx, 0.0f) && feq(wz, 1.0f));

		// Every wobbled heading stays UNIT length (rotation only, no scale).
		for (int sz = 1; sz <= 4; sz *= 2) {
			for (float roll = -1.0f; roll <= 1.0f; roll += 0.25f) {
				slime_hop_wobble_dir(0.0f, 1.0f, roll, sz, &wx, &wz);
				assert(feq(wx * wx + wz * wz, 1.0f));
			}
		}

		// Size-dependent deviation magnitude at max strafe (|strafe|==1): a
		// small slime (size 1) deviates 45deg (atan2(1,1)); a big slime
		// (size 4) deviates far less (atan2(1,4) ~= 14deg) -- pin the ORDER
		// (small wobbles more than big), the direct bug-vs-fix contrast.
		slime_hop_wobble_dir(0.0f, 1.0f, 1.0f, 1, &wx, &wz);
		float dev_small = fabsf(atan2f(wx, wz)); // deviation from (0,1)
		slime_hop_wobble_dir(0.0f, 1.0f, 1.0f, 4, &wx, &wz);
		float dev_big = fabsf(atan2f(wx, wz));
		assert(dev_small > dev_big);
		assert(feq(dev_small, atan2f(1.0f, 1.0f))); // exactly 45deg in radians
	}
	printf("  slime FIX 2: strafe roll formula + size-dependent wobble "
		   "rotation (unit length preserved) OK\n");

	// --- 6c. FIX 3: face-player-every-tick (independent of the hop gate) --------
	// Beta faceEntity(player,10,20) fires whenever getClosestPlayerToEntity(16)
	// is non-null -- a plain 3D distance probe, no LOS. Pin: in-range yields the
	// atan2(dx,dz) heading (this engine's yaw convention); the strict '<=' 16.0
	// boundary; out-of-range leaves the yaw untouched (Beta: rotationYaw simply
	// isn't updated that tick).
	{
		float yaw;
		// Due +X at distance 5: yaw = atan2(5,0) = +90deg.
		bool in_range = slime_face_player_yaw(0.0f, 64.0f, 0.0f,
											  5.0f, 64.0f, 0.0f, &yaw);
		assert(in_range);
		assert(feq(yaw, atan2f(1.0f, 0.0f))); // exactly 90deg in radians
		// Due +Z (south) at distance 5: yaw = atan2(0,5) = 0.
		in_range = slime_face_player_yaw(0.0f, 64.0f, 0.0f,
										 0.0f, 64.0f, 5.0f, &yaw);
		assert(in_range);
		assert(feq(yaw, 0.0f));
		// Exactly at the boundary (16.0 away): 16^2 == 16^2 -- '>' gate means
		// this is STILL in range (matches getClosestPlayerToEntity's <=).
		in_range = slime_face_player_yaw(0.0f, 64.0f, 0.0f,
										 16.0f, 64.0f, 0.0f, &yaw);
		assert(in_range);
		// Just beyond: out of range, yaw untouched (sentinel proves no write).
		yaw = -999.0f;
		in_range = slime_face_player_yaw(0.0f, 64.0f, 0.0f,
										 16.01f, 64.0f, 0.0f, &yaw);
		assert(!in_range);
		assert(feq(yaw, -999.0f));
		// Vertical distance counts too (Beta's probe is full 3D, no LOS): a
		// player 20 blocks straight up is out of the 16-block sphere.
		in_range = slime_face_player_yaw(0.0f, 64.0f, 0.0f,
										 0.0f, 84.0f, 0.0f, &yaw);
		assert(!in_range);
	}
	printf("  slime FIX 3: faceEntity 16-block yaw gate (in-range heading, "
		   "boundary, out-of-range no-write, 3D distance) OK\n");

	// --- 6d. FIX 4 (audit 2026-09-01, REGRESSION-TODAY): idle zero-heading
	// hop fallback ---------------------------------------------------------
	{
		// (a) The fallback itself: zero steer heading resolves to a UNIT
		// vector from facing_yaw, in this engine's (sin yaw, cos yaw)
		// atan2f(dx,dz) convention -- two distinct yaws so this cannot pass
		// by coincidentally hardcoding one axis.
		float hx, hz;
		slime_hop_steer_heading(0.0f, 0.0f, 0.0f, &hx, &hz);
		assert(feq(hx, 0.0f) && feq(hz, 1.0f)
			   && "yaw=0 fallback must be the unit heading (sin 0, cos 0) "
				  "= (0,1)");
		slime_hop_steer_heading(0.0f, 0.0f, (float)M_PI / 2.0f, &hx, &hz);
		assert(feq(hx, 1.0f) && feq(hz, 0.0f)
			   && "yaw=pi/2 fallback must be (sin pi/2, cos pi/2) = (1,0) "
				  "-- proves the substitution genuinely reads facing_yaw, "
				  "not a hardcoded axis");

		// (b) EQUIVALENCE, non-degenerate case: a REAL steer heading must
		// pass through completely untouched, whatever facing_yaw is passed
		// (it must not even be read) -- byte-identical to calling
		// slime_hop_wobble_dir directly, exactly as items 1/2 above already
		// pin without this helper in the way.
		slime_hop_steer_heading(1.0f, 0.0f, 999.0f, &hx, &hz);
		assert(hx == 1.0f && hz == 0.0f
			   && "a nonzero steer heading passes through UNCHANGED -- "
				  "today's regression fix must not perturb a chasing or "
				  "wandering slime");
		slime_hop_steer_heading(0.0f, 1.0f, -999.0f, &hx, &hz);
		assert(hx == 0.0f && hz == 1.0f
			   && "same equivalence check, the other axis, a different "
				  "(also-ignored) facing_yaw");
		{
			// Full pipeline (fallback helper + wobble) for a real steer
			// heading must equal calling slime_hop_wobble_dir directly --
			// section 6b's own already-pinned numbers, unperturbed.
			float direct_x, direct_z;
			slime_hop_wobble_dir(0.0f, 1.0f, 1.0f, 1, &direct_x, &direct_z);
			float sx, sz, piped_x, piped_z;
			slime_hop_steer_heading(0.0f, 1.0f, 12.34f, &sx, &sz);
			slime_hop_wobble_dir(sx, sz, 1.0f, 1, &piped_x, &piped_z);
			assert(feq(piped_x, direct_x) && feq(piped_z, direct_z)
				   && "routing a real steer heading through the new "
					  "fallback helper first must be a complete no-op "
					  "before the wobble rotation runs");
		}

		// (c) THE EXACT REGRESSION, end-to-end: an idle/out-of-range slime
		// (direction={0,0}, exactly what the fresh-spawn ctor AND
		// mob_path_follow_tail's no-path case both produce) must still hop
		// with a NONZERO horizontal component whose magnitude is the
		// size-scaled hop speed -- not the pre-fix zero. strafe_roll=0.0
		// (Beta's neutral moveStrafing midpoint) keeps atan2(0,size)==0, so
		// the wobble is a true no-op rotation here and the resulting speed
		// must equal hop_speed EXACTLY, isolating this fix from item 1's
		// (already-pinned) size-scaling arithmetic.
		const int slime_ids[3]
			= {MONSTER_SLIME_SMALL, MONSTER_SLIME_MEDIUM, MONSTER_SLIME_BIG};
		for(int i = 0; i < 3; i++) {
			int id = slime_ids[i];
			int beta_size = slime_beta_size(id);
			float faithful_base
				= monster_def_get(id)->move_speed * (float)beta_size;
			float hop_speed = mob_speed_cap_wii(faithful_base);

			float steer_x, steer_z;
			slime_hop_steer_heading(
				0.0f, 0.0f, 0.0f, &steer_x, &steer_z); // idle: no path
			float wx, wz;
			slime_hop_wobble_dir(steer_x, steer_z, 0.0f, beta_size, &wx,
								 &wz);
			float vel_x = wx * hop_speed, vel_z = wz * hop_speed;
			float speed = sqrtf(vel_x * vel_x + vel_z * vel_z);
			assert(speed > 0.0f
				   && "REGRESSION GUARD: an idle out-of-range slime's hop "
					  "must have a NONZERO horizontal component -- the "
					  "pre-fix bug rotated a zero steer vector and stayed "
					  "zero regardless of size, bouncing the slime "
					  "straight up in place");
			assert(feq(speed, hop_speed)
				   && "and that horizontal speed must equal the exact "
					  "size-scaled hop speed (item 1's own value), not an "
					  "arbitrary nonzero substitute");
		}
	}
	printf("  slime FIX 4 (audit 2026-09-01): idle zero-heading hop "
		   "fallback -- nonzero, size-scaled magnitude; non-degenerate "
		   "steer heading fully unperturbed (equivalence) OK\n");

	// --- 6. slime-chunk check is BIT-EXACT java.util.Random ---------------------
	// Truth table computed INDEPENDENTLY (a Python replica of java.util.Random's
	// 48-bit LCG + the Chunk.func_997_a seed formula with Java int-overflow
	// semantics), so the C side's uint32 wraparound + widening + XOR precedence
	// are all pinned -- v2 slime chunks land on EXACTLY the chunks a real Beta
	// world produces on the same seed. Mixed positives/negatives, negative
	// coords, a negative 64-bit seed, and far coords that exercise the int
	// overflow.
	assert(slime_chunk_is_slimy(42LL, -6, 0) == true);
	assert(slime_chunk_is_slimy(42LL, -6, 6) == true);
	assert(slime_chunk_is_slimy(42LL, -3, -6) == true);
	assert(slime_chunk_is_slimy(42LL, -3, 1) == true);
	assert(slime_chunk_is_slimy(42LL, 0, 0) == false);
	assert(slime_chunk_is_slimy(42LL, 1, 0) == false);
	assert(slime_chunk_is_slimy(42LL, 5, -2) == false);
	assert(slime_chunk_is_slimy(-7761034523576012345LL, -6, -5) == true);
	assert(slime_chunk_is_slimy(-7761034523576012345LL, -5, 1) == true);
	assert(slime_chunk_is_slimy(-7761034523576012345LL, 0, 0) == false);
	assert(slime_chunk_is_slimy(-7761034523576012345LL, 3, 3) == false);
	assert(slime_chunk_is_slimy(42LL, 123456, -654321) == false);
	assert(slime_chunk_is_slimy(42LL, -2000000, 1999999) == false);
	// Density sanity: ~10% of chunks are slimy (1-in-10 nextInt) -- catch a
	// formula slip that degenerates to ~0% or ~100% without pinning a count.
	{
		int n = 0;
		for(int cx = -50; cx < 50; cx++)
			for(int cz = -50; cz < 50; cz++)
				if(slime_chunk_is_slimy(42LL, cx, cz))
					n++;
		assert(n == 984); // exact count for seed 42 over [-50,50)^2 (Python truth)
	}

	printf("test_slime: OK (size-as-id, hop schedule, contact damage, split "
		   "grid, split gate (health==0 vs overkill), hop impulse band, "
		   "bit-exact slime chunks)\n");
	return 0;
}
