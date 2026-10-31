/*
	Beta 1.7.3 sneak camera dip -- Entity.ySize. CAMERA ONLY.

	Ground truth [740c583, ../mcb173-retromcp]:
	  EntityPlayerSP.onLivingUpdate:79-80  if(sneak && ySize < 0.2F) ySize = 0.2F;
	  Entity.moveEntity:259                ySize *= 0.4F;        (first statement)
	  Entity.moveEntity:413                posY = bb.minY + yOffset - ySize;
	  Entity.setPosition:132               bb.minY = posY - yOffset + ySize;
	  Entity.setPositionAndRotation:614    ySize = 0.0F;
	  Entity.onEntityUpdate:164            prevPosY/lastTickPosY = posY (tick top)
	  EntityRenderer.orientCamera:444      lastTickPosY + (posY - lastTickPosY)*f

	The EYE drops by ySize; the BOUNDING BOX does not move (132 and 413 cancel).

	This pins, with BIT PATTERNS rather than epsilons:
	  1. the exact float sequence -- rest, first sneak tick, 10 held ticks,
	     8 release-decay ticks, reset;
	  2. the interpolation sample, including the anti-sawtooth property that
	     held sneak renders a CONSTANT dip (the reason the previous sample is
	     taken before the 0.2F floor, per Entity:164);
	  3. the real camera_attach_owned -- byte-identical to the old arithmetic
	     when sneak is never pressed, and eye-minus-dip when it is;
	  4. the CaveWii deviation that matters: the wire position, the server's
	     feet derivation (eye - 1.62) and therefore the collision box are
	     BYTE-IDENTICAL standing and sneaking at the same place, for BOTH
	     local owners, at several world heights (a naive add-back fold would
	     miss by an ULP here -- that is what this test is for);
	  5. the block-pick ray origin DOES carry the dip (Beta takes it from posY).
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "entity/player_ysize.h"
#include "game/camera.h"
#include "game/client_session.h"
#include "game/game_state.h"

/* Beta EntityPlayer.yOffset -- entity_local_player.c's EYE_HEIGHT. */
#define EYE_HEIGHT 1.62F

static uint32_t fbits(float f) {
	uint32_t u;
	memcpy(&u, &f, sizeof(u));
	return u;
}

/* IEEE-754 single-precision goldens, computed independently of the code under
 * test from the decompile's own expressions (0.2F, 0.4F and their products). */
#define YS_FLOOR_BITS   0x3E4CCCCDu /* 0.2F                       */
#define YS_HELD_BITS    0x3DA3D70Bu /* 0.2F * 0.4F   0.0800000057 */
#define YS_DECAY1_BITS  0x3D03126Fu /* * 0.4F        0.0320000015 */
#define YS_DECAY2_BITS  0x3C51B718u /*               0.0128000006 */
#define YS_DECAY3_BITS  0x3BA7C5ADu /*               0.00512000034*/
#define YS_DECAY4_BITS  0x3B0637BEu /*               0.00204800023*/
#define YS_DECAY5_BITS  0x3A56BF97u /*               0.000819200126*/
#define YS_DECAY6_BITS  0x39ABCC79u /*               0.000327680056*/
#define YS_DECAY7_BITS  0x39097061u /*               0.000131072025*/
#define YS_DECAY8_BITS  0x385BE702u /*               5.24288116e-05*/

static void test_golden_sequence(void) {
	struct player_ysize_state s = {0};

	/* Rest: never sneaked. Beta's field initialiser is 0.0F (Entity:46). */
	assert(fbits(s.y_size) == 0u && fbits(s.y_size_prev) == 0u);
	for(int i = 0; i < 4; i++) {
		player_ysize_pre_move(&s, false);
		player_ysize_move(&s);
		assert(fbits(s.y_size) == 0u);
		assert(fbits(s.y_size_prev) == 0u);
	}

	/* First sneak tick from rest: the floor lifts 0 -> 0.2F, the move step
	 * takes it to 0.2F*0.4F. The render sample is still the pre-floor 0. */
	player_ysize_pre_move(&s, true);
	assert(fbits(s.y_size) == YS_FLOOR_BITS);   /* after the floor  */
	assert(fbits(s.y_size_prev) == 0u);         /* Entity:164 sample */
	player_ysize_move(&s);
	assert(fbits(s.y_size) == YS_HELD_BITS);

	/* 10 further HELD ticks: the floor re-lifts 0.08 -> 0.2 every tick, so
	 * the post-move value is the SAME float forever -- and the previous
	 * sample equals it, which is what makes the rendered dip constant. */
	for(int i = 0; i < 10; i++) {
		player_ysize_pre_move(&s, true);
		assert(fbits(s.y_size) == YS_FLOOR_BITS);
		assert(fbits(s.y_size_prev) == YS_HELD_BITS);
		player_ysize_move(&s);
		assert(fbits(s.y_size) == YS_HELD_BITS);
	}

	/* Release: no floor, so the move step decays by 0.4F per tick. */
	static const uint32_t decay[8] = {
		YS_DECAY1_BITS, YS_DECAY2_BITS, YS_DECAY3_BITS, YS_DECAY4_BITS,
		YS_DECAY5_BITS, YS_DECAY6_BITS, YS_DECAY7_BITS, YS_DECAY8_BITS,
	};
	uint32_t prev_expected = YS_HELD_BITS;
	for(int i = 0; i < 8; i++) {
		player_ysize_pre_move(&s, false);
		assert(fbits(s.y_size_prev) == prev_expected);
		player_ysize_move(&s);
		assert(fbits(s.y_size) == decay[i]);
		prev_expected = decay[i];
	}

	/* Entity.setPositionAndRotation:614. `applied` is deliberately preserved
	 * (the stored camera still carries the last attach's dip). */
	s.applied = 0.5F;
	s.pre_dip_y = 7.0F;
	s.post_dip_y = 6.5F;
	player_ysize_reset(&s);
	assert(fbits(s.y_size) == 0u && fbits(s.y_size_prev) == 0u);
	assert(fbits(s.applied) == fbits(0.5F));
	assert(fbits(s.pre_dip_y) == fbits(7.0F));
	assert(fbits(s.post_dip_y) == fbits(6.5F));

	/* A sneak tick from a value ABOVE the floor must NOT be lifted (Beta's
	 * `ySize < 0.2F` guard); it just decays. */
	struct player_ysize_state high = {.y_size = 0.5F};
	player_ysize_pre_move(&high, true);
	assert(fbits(high.y_size) == fbits(0.5F));
	player_ysize_move(&high);
	assert(fbits(high.y_size) == fbits(0.5F * 0.4F));

	puts("PASS ysize goldens: rest/first/held/release/reset bit-exact vs Beta");
}

static void test_lerp_sample(void) {
	/* First sneak tick: the dip grows 0 -> 0.08 across the tick. */
	struct player_ysize_state s = {0};
	player_ysize_pre_move(&s, true);
	player_ysize_move(&s);
	assert(fbits(player_ysize_lerp(&s, 0.0F)) == 0u);
	assert(fbits(player_ysize_lerp(&s, 1.0F)) == YS_HELD_BITS);
	assert(fbits(player_ysize_lerp(&s, 0.5F)) == fbits(0.0F + (0.0800000057F - 0.0F) * 0.5F));

	/* HELD: prev == cur, so EVERY partial tick renders the same dip. This is
	 * the anti-sawtooth property -- sampling after the 0.2F floor instead of
	 * before it would make this sweep run 0.2 -> 0.08 and visibly judder. */
	player_ysize_pre_move(&s, true);
	player_ysize_move(&s);
	for(int i = 0; i <= 10; i++) {
		float t = (float)i / 10.0F;
		assert(fbits(player_ysize_lerp(&s, t)) == YS_HELD_BITS);
	}

	/* Rest interpolates to exactly zero -- no dip, ever. */
	struct player_ysize_state rest = {0};
	for(int i = 0; i <= 4; i++)
		assert(fbits(player_ysize_lerp(&rest, (float)i / 4.0F)) == 0u);

	puts("PASS ysize lerp: held sneak renders a CONSTANT dip (no sawtooth)");
}

/* One attach against a body parked at `eye`, with `state` as the dip. */
static void attach_at(struct client_session* p, struct entity* body, float eye,
					  const struct player_ysize_state* state) {
	body->pos[0] = body->pos_old[0] = 12.0F;
	body->pos[1] = body->pos_old[1] = eye;
	body->pos[2] = body->pos_old[2] = -30.0F;
	p->camera.ysize = *state;
	struct camera_player_view view = {.world = &gstate.world, .ride = &p->ride,
		.sleeping = false, .yaw = 0.4F, .pitch = 1.1F};
	camera_attach_owned(&p->camera, body, 1.0F, 0.0F, &view);
}

/* A held-sneak dip state: prev == cur == 0.2F*0.4F. */
static struct player_ysize_state held_sneak_state(void) {
	struct player_ysize_state s = {0};
	player_ysize_pre_move(&s, true);
	player_ysize_move(&s);
	player_ysize_pre_move(&s, true);
	player_ysize_move(&s);
	return s;
}

static void test_camera_dip_and_equivalence(void) {
	struct client_session p = {0};
	struct entity body = {0};
	const float eye = 104.0F;

	/* EQUIVALENCE: sneak never pressed. A zeroed dip state must leave the
	 * camera bit-for-bit on the old arithmetic -- the plain position lerp,
	 * which for pos == pos_old is the position itself. */
	struct player_ysize_state zero = {0};
	attach_at(&p, &body, eye, &zero);
	assert(fbits(p.camera.y) == fbits(eye));
	assert(fbits(p.camera.ysize.applied) == 0u);
	/* ... and the fold-out is then the identity, so the wire is untouched. */
	assert(fbits(player_ysize_unapply_y(p.camera.y, &p.camera.ysize))
		   == fbits(eye));

	/* SNEAKING: the eye drops by exactly the interpolated ySize. */
	struct player_ysize_state held = held_sneak_state();
	attach_at(&p, &body, eye, &held);
	assert(fbits(p.camera.ysize.applied) == YS_HELD_BITS);
	assert(fbits(p.camera.y) == fbits(eye - 0.0800000057F));
	assert(p.camera.y < eye);
	/* The recorded fold-out pair must describe the subtraction exactly. */
	assert(fbits(p.camera.ysize.pre_dip_y) == fbits(eye));
	assert(fbits(p.camera.ysize.post_dip_y) == fbits(p.camera.y));

	/* THE BODY NEVER MOVES (Entity:132 and :413 cancel). The attach may not
	 * have touched the position or the interpolation samples. */
	assert(fbits(body.pos[1]) == fbits(eye));
	assert(fbits(body.pos_old[1]) == fbits(eye));

	puts("PASS camera dip: zero state byte-identical to the old lerp; "
		 "sneak drops the EYE only");
}

static void test_wire_byte_identity(void) {
	/* THE TRAP. Beta's posY carries ySize, so Beta's Packet10Flying stance
	 * carries the dip. CaveWii keeps the wire unchanged instead: what
	 * clin_update sends -- and therefore the server's feet (eye - 1.62) and
	 * collision box -- must be BYTE-IDENTICAL standing and sneaking at the
	 * same place. Both local owners send through the same clin_update path
	 * (host at client_interface.c ~4096, guest ~4151), so both are pinned.
	 *
	 * Several heights on purpose: a naive `camera_y + applied` fold-out is
	 * off by an ULP at ordinary world heights, which is exactly the bug this
	 * guards. */
	static const float heights[] = {
		0.5F, 63.0F, 64.0F, 104.0F, 127.3F, 200.125F, 255.9F, 1000.0F,
	};
	struct client_session owners[2] = {0};
	struct entity bodies[2] = {0};
	struct player_ysize_state zero = {0};
	struct player_ysize_state held = held_sneak_state();

	for(unsigned o = 0; o < 2; o++) {
		for(unsigned h = 0; h < sizeof(heights) / sizeof(*heights); h++) {
			const float eye = heights[h];

			attach_at(&owners[o], &bodies[o], eye, &zero);
			const float wire_standing = player_ysize_unapply_y(
				owners[o].camera.y, &owners[o].camera.ysize);

			attach_at(&owners[o], &bodies[o], eye, &held);
			const float wire_sneaking = player_ysize_unapply_y(
				owners[o].camera.y, &owners[o].camera.ysize);

			/* The camera really did dip -- otherwise this proves nothing. */
			assert(owners[o].camera.y < eye);
			assert(fbits(owners[o].camera.ysize.applied) == YS_HELD_BITS);

			/* ... and the wire did not. */
			assert(fbits(wire_sneaking) == fbits(wire_standing));
			assert(fbits(wire_sneaking) == fbits(eye));
			/* The server derives feet from the wire eye; same bits in,
			 * same feet out, same collision box. */
			assert(fbits(wire_sneaking - EYE_HEIGHT)
				   == fbits(wire_standing - EYE_HEIGHT));
		}
	}

	/* A shift of the stored camera between attaches (the minecart snapshot
	 * install does this) must still reach the wire, exactly as it does for
	 * camera_correction -- the fold-out carries it through. */
	attach_at(&owners[0], &bodies[0], 104.0F, &held);
	owners[0].camera.y += 3.0F;
	assert(fbits(player_ysize_unapply_y(owners[0].camera.y,
									    &owners[0].camera.ysize))
		   == fbits(104.0F + 3.0F));

	/* ... but an ABSOLUTE write is NOT a shift, and that is the case the
	 * fold-out used to get wrong (hostile review 17, H-2).
	 *
	 * `pre_dip_y + (camera_y - post_dip_y)` is arithmetically `camera_y +
	 * applied`. That is right for a MOVER -- every one of them is a delta
	 * (client_interface.c:1329/1343/1362 and :3054 are all += / -=). But the
	 * server teleport arms SET the camera: clin_apply_position_only_correction
	 * (:1050) and the full-snapshot arm of clin_apply_player_position (:1087)
	 * both write camera.y = T. After either, the stored camera carries NO dip,
	 * while applied/pre_dip_y/post_dip_y still describe the PREVIOUS attach --
	 * so the fold-out added a stale dip back and the wire read T + 0.08.
	 *
	 * It is not a one-frame slip. Both teleport arms reset
	 * player_sync.last_pos_update, which suppresses the echo for 50 ms, and an
	 * attach normally lands inside that window and refreshes the triple. The
	 * PRE-WORLD echo (client_interface.c:4063) is the path where it does not:
	 * it sends every 50 ms for the whole cross-dimension terrain hold while
	 * camera_attach for player 0 is gated off (main.c:3906). A player holding
	 * sneak into a nether portal therefore echoed T + 0.08 to the server for
	 * the entire hold.
	 *
	 * Beta's ground truth is unambiguous and it is a FULL reset, not just the
	 * record: NetClientHandler.handleFlying:253 does `var2.ySize = 0.0F;`
	 * itself and then calls setPositionAndRotation, which zeroes it again
	 * (Entity:614) and sets prevPosY = posY so the render sample carries no
	 * dip either. player_ysize_absolute_set is that statement. */
	{
		const float T = 71.5F;
		for(unsigned o = 0; o < 2; o++) {
			attach_at(&owners[o], &bodies[o], 104.0F, &held);
			/* The dip really is baked into the stored camera ... */
			assert(fbits(owners[o].camera.ysize.applied) == YS_HELD_BITS);
			assert(owners[o].camera.y < 104.0F);
			/* ... and then the teleport SETS the camera, absolutely, which is
			 * what the production writers above do -- each paired with this
			 * same call, which is Beta's handleFlying:253 statement. */
			owners[o].camera.y = T;
			player_ysize_absolute_set(&owners[o].camera.ysize);
			/* The WHOLE state, not just enough of it to make the fold-out
			 * come out right. `applied` doubles as the fold-out's early-out
			 * flag, so zeroing only pre/post would give the same wire answer
			 * while leaving the record describing an attach that no longer
			 * exists -- and camera.c:361 is not the only thing that may ever
			 * read it. Beta zeroes ySize itself, so every field goes. */
			assert(fbits(owners[o].camera.ysize.applied) == 0u
				   && "the absolute set left the applied dip behind");
			assert(fbits(owners[o].camera.ysize.pre_dip_y) == 0u);
			assert(fbits(owners[o].camera.ysize.post_dip_y) == 0u);
			assert(fbits(owners[o].camera.ysize.y_size) == 0u);
			assert(fbits(owners[o].camera.ysize.y_size_prev) == 0u);
			assert(fbits(player_ysize_unapply_y(owners[o].camera.y,
												&owners[o].camera.ysize))
				   == fbits(T)
				   && "an absolute camera set still folded a stale dip onto "
					  "the wire");
			/* The server derives the same feet it would for a standing
			 * player put at exactly the same place. */
			assert(fbits(player_ysize_unapply_y(owners[o].camera.y,
												&owners[o].camera.ysize)
						 - EYE_HEIGHT)
				   == fbits(T - EYE_HEIGHT));
			/* The reset is Beta's, so the DIP COMES BACK BY ITSELF while
			 * sneak is still held -- it is not a suppression. One tick puts
			 * the held value back exactly (Entity:79-80 then :259), and the
			 * next attach re-derives the record from it. */
			assert(fbits(owners[o].camera.ysize.y_size) == 0u);
			player_ysize_pre_move(&owners[o].camera.ysize, true);
			player_ysize_move(&owners[o].camera.ysize);
			assert(fbits(owners[o].camera.ysize.y_size) == YS_HELD_BITS);
			bodies[o].pos[1] = bodies[o].pos_old[1] = T;
			{
				struct camera_player_view view
					= {.world = &gstate.world, .ride = &owners[o].ride,
					   .sleeping = false, .yaw = 0.4F, .pitch = 1.1F};
				camera_attach_owned(&owners[o].camera, &bodies[o], 1.0F, 0.0F,
									&view);
			}
			assert(fbits(owners[o].camera.ysize.applied) == YS_HELD_BITS);
			assert(owners[o].camera.y < T);
			/* ... and the wire is bit-exact T again, dip and all. */
			assert(fbits(player_ysize_unapply_y(owners[o].camera.y,
												&owners[o].camera.ysize))
				   == fbits(T));
		}
	}

	/* Review 19 M-1: the POSITION-ONLY correction arm also assigns camera.y
	 * absolutely, but it is NOT Beta's teleport packet -- it is this engine's
	 * shared correction channel (piston push, knockback, local and guest
	 * corrections), and Beta moves the player through moveEntity for every one
	 * of those, which never touches ySize. So that arm clears the RECORD only
	 * (player_ysize_record_clear): the wire must still carry exactly the
	 * assigned Y, and the dip STATE must survive untouched, so a latched sneak
	 * does not pop the camera up 0.08 block and back once per correction. */
	{
		const float T = 66.25F;
		for(unsigned o = 0; o < 2; o++) {
			attach_at(&owners[o], &bodies[o], 104.0F, &held);
			assert(fbits(owners[o].camera.ysize.applied) == YS_HELD_BITS);
			const uint32_t size_bits = fbits(owners[o].camera.ysize.y_size);
			const uint32_t prev_bits = fbits(owners[o].camera.ysize.y_size_prev);
			assert(size_bits == YS_HELD_BITS);
			owners[o].camera.y = T;
			player_ysize_record_clear(&owners[o].camera.ysize);
			/* The record is gone, all of it ... */
			assert(fbits(owners[o].camera.ysize.applied) == 0u);
			assert(fbits(owners[o].camera.ysize.pre_dip_y) == 0u);
			assert(fbits(owners[o].camera.ysize.post_dip_y) == 0u);
			/* ... the dip state is NOT -- this is the whole difference from
			 * player_ysize_absolute_set ... */
			assert(fbits(owners[o].camera.ysize.y_size) == size_bits
				   && "a position-only correction zeroed the sneak dip");
			assert(fbits(owners[o].camera.ysize.y_size_prev) == prev_bits);
			/* ... the wire carries exactly the corrected Y ... */
			assert(fbits(player_ysize_unapply_y(owners[o].camera.y,
												&owners[o].camera.ysize))
				   == fbits(T)
				   && "a position-only correction folded a stale dip onto the "
					  "wire");
			/* ... and the very next attach, with NO tick in between, renders
			 * the same held dip as before the correction: no pop. */
			bodies[o].pos[1] = bodies[o].pos_old[1] = T;
			{
				struct camera_player_view view
					= {.world = &gstate.world, .ride = &owners[o].ride,
					   .sleeping = false, .yaw = 0.4F, .pitch = 1.1F};
				camera_attach_owned(&owners[o].camera, &bodies[o], 1.0F, 0.0F,
									&view);
			}
			assert(fbits(owners[o].camera.ysize.applied) == YS_HELD_BITS
				   && "the camera popped: the dip did not survive a "
					  "position-only correction");
			assert(fbits(player_ysize_unapply_y(owners[o].camera.y,
												&owners[o].camera.ysize))
				   == fbits(T));
		}
	}

	puts("PASS wire byte-identity: SRPC_PLAYER_POS Y, derived feet and the "
		 "collision box are bit-identical standing vs sneaking, both owners");
}

static void test_ray_origin_carries_dip(void) {
	/* Beta's block-pick ray starts at posY, which INCLUDES the dip, so the
	 * sneaking player reaches slightly lower. camera_get_ray takes its origin
	 * straight from the camera (camera.c:428-432), so the dip carries with no
	 * extra plumbing -- pin that it actually does. */
	struct client_session p = {0};
	struct entity body = {0};
	const float eye = 104.0F;
	struct player_ysize_state zero = {0};
	struct player_ysize_state held = held_sneak_state();
	vec3 origin_standing, origin_sneaking, dir;

	attach_at(&p, &body, eye, &zero);
	camera_get_ray(&p.camera, origin_standing, dir);
	assert(fbits(origin_standing[1]) == fbits(eye));

	attach_at(&p, &body, eye, &held);
	camera_get_ray(&p.camera, origin_sneaking, dir);
	assert(fbits(origin_sneaking[1]) == fbits(p.camera.y));
	assert(fbits(origin_sneaking[1]) == fbits(eye - 0.0800000057F));
	assert(origin_sneaking[1] < origin_standing[1]);
	/* Only the Y moved. */
	assert(fbits(origin_sneaking[0]) == fbits(origin_standing[0]));
	assert(fbits(origin_sneaking[2]) == fbits(origin_standing[2]));

	puts("PASS ray origin: the block-pick ray carries the dip (Beta posY)");
}

int main(void) {
	test_golden_sequence();
	test_lerp_sample();
	test_camera_dip_and_equivalence();
	test_wire_byte_identity();
	test_ray_origin_carries_dip();
	puts("PASS test_player_ysize");
	return 0;
}
