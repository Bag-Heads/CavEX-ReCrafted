/* Beta 1.7.3 sneak dip and step-up smoothing -- Entity.ySize, CAMERA ONLY.
 *
 * Beta drops the first-person EYE while sneaking or easing up a step. The decompile
 * [../mcb173-retromcp, 740c583]:
 *
 *   EntityPlayerSP.onLivingUpdate:79-80
 *       sneaking with ySize < 0.2F raises ySize to 0.2F;
 *   Entity.moveEntity:259  (FIRST statement of the move)
 *       ySize is multiplied by 0.4F;
 *   Entity.moveEntity:404-408 (only after the stepped route wins)
 *       a fractional boundingBox.minY > 0 adds fraction + 0.01 to ySize;
 *   Entity.moveEntity:413 / Entity.noClip:256
 *       posY = boundingBox.minY + yOffset - ySize;
 *   Entity.setPositionAndRotation:614
 *       ySize = 0.0F;
 *
 * Note what line 413 does and does NOT do: the EYE (posY) drops by ySize, the
 * BOUNDING BOX does not move at all. Entity.setPosition:132 puts the box back
 * at posY - yOffset + ySize, so the two cancel and the collider is exactly
 * where it would be standing. The dip is presentation, never physics.
 *
 * INTERPOLATION. EntityRenderer.orientCamera:444 renders
 * lastTickPosY + (posY - lastTickPosY) * partialTick, and both lastTickPosY
 * and prevPosY are captured at Entity.onEntityUpdate:164 -- the TOP of the
 * tick, BEFORE the sneak floor of EntityPlayerSP:79 and before the move. So
 * the previous sample is the PRE-floor value, and the rendered dip is the
 * linear blend of the previous tick's POST-move ySize and this tick's
 * POST-move ySize. Capturing it after the floor instead would make held sneak
 * render a 0.2 -> 0.08 sawtooth every single tick; capturing it before the
 * floor makes held sneak the constant 0.08 the game actually shows. That is
 * why player_ysize_pre_move, not player_ysize_move, takes the sample.
 *
 * CAVEWII DEVIATION (documented, invisible): Beta's posY carries the dip, so
 * Beta's Packet10Flying stance/posY carries it onto the wire too. CaveWii
 * keeps `entity.pos` and the wire UNCHANGED and applies the dip only to the
 * rendered camera, folding it back out in clin_update's SRPC_PLAYER_POS send
 * exactly the way the minecart camera_correction is folded out. The server
 * therefore derives identical feet (eye - 1.62) and an identical collision box
 * whether or not the player is sneaking. `applied` is that fold-out value.
 *
 * Pure: state plus the steps. No globals, no I/O -- same shape as
 * source/platform/input_sneak_toggle.h.
 */

#ifndef PLAYER_YSIZE_H
#define PLAYER_YSIZE_H

#include <stdbool.h>

/* EntityPlayerSP.onLivingUpdate:79-80 */
#define PLAYER_YSIZE_SNEAK_FLOOR 0.2F
/* Entity.moveEntity:259 */
#define PLAYER_YSIZE_MOVE_DECAY 0.4F

struct player_ysize_state {
	/* Beta Entity.ySize after this tick's move. */
	float y_size;
	/* The same field as captured at the TOP of this tick (Entity:164), i.e.
	 * the previous tick's post-move value. The render sample. */
	float y_size_prev;
	/* The dip camera_attach_owned last subtracted from the stored camera,
	 * and the exact camera Y on both sides of that subtraction. Mirrors the
	 * camera_correction feedback guard: these describe what is baked into
	 * the stored camera for the WHOLE inter-frame interval, so only an
	 * attach writes them. See player_ysize_unapply_y. */
	float applied;
	float pre_dip_y;  /* camera Y at that attach, BEFORE the subtraction */
	float post_dip_y; /* camera Y at that attach, AFTER the subtraction */
};

/* Entity.onEntityUpdate:164 (prevPosY/lastTickPosY = posY, top of tick) fused
 * with EntityPlayerSP.onLivingUpdate:79-80. Call once per client tick, BEFORE
 * the movement step, with this tick's resolved sneak level. */
static inline void player_ysize_pre_move(
	struct player_ysize_state* s, bool sneaking) {
	s->y_size_prev = s->y_size;
	if(sneaking && s->y_size < PLAYER_YSIZE_SNEAK_FLOOR)
		s->y_size = PLAYER_YSIZE_SNEAK_FLOOR;
}

/* Entity.moveEntity:259 -- the first statement of the move. Call once per
 * client tick, at the movement step, after player_ysize_pre_move. */
static inline void player_ysize_move(struct player_ysize_state* s) {
	s->y_size *= PLAYER_YSIZE_MOVE_DECAY;
}

/* Entity.moveEntity:404-408, after the stepped route wins. The integer cast
 * truncates toward zero, as in Beta; the 0.01 is inside the positive guard.
 * Entity.java:407 is ONE double sum and ONE cast back to float:
 *     this.ySize = (float)((double)this.ySize + var41 + 0.01D);
 * (CODESTYLE section 0 cast chains). Summing in float twice rounds twice and
 * differs by an ulp, e.g. sneak-held 0.2F*0.4F + a slab step.
 * box_min_y - (int)box_min_y is exact in float, so widening it is lossless. */
static inline void player_ysize_step(struct player_ysize_state* s,
	float box_min_y) {
	float fractional_min_y = box_min_y - (float)(int)box_min_y;
	if(fractional_min_y > 0.0F)
		s->y_size = (float)((double)s->y_size + (double)fractional_min_y
			+ 0.01);
}

/* Entity.setPositionAndRotation:614 -- ySize = 0.0F.
 *
 * `applied` is deliberately NOT cleared. A reset runs at tick time; the stored
 * camera still holds the dip the last attach subtracted, and clin_update may
 * send before the next attach re-derives one. Clearing the fold-out here would
 * put that stale dip on the wire -- the exact failure the guard exists to
 * prevent. The next attach overwrites it. */
static inline void player_ysize_reset(struct player_ysize_state* s) {
	s->y_size = 0.0F;
	s->y_size_prev = 0.0F;
}

/* The OTHER reset: for a writer that SETS the stored camera Y absolutely,
 * rather than shifting it.
 *
 * player_ysize_reset above runs at TICK time and deliberately leaves the
 * fold-out record alone, because the stored camera still carries the dip the
 * last attach subtracted. A writer that assigns `camera.y = T` has destroyed
 * exactly that premise: the stored camera now carries NO dip, while
 * applied/pre_dip_y/post_dip_y still describe the previous attach, so the
 * fold-out below would add a stale dip back and put T + applied on the wire.
 * An absolute write is not the "external shift carried through unchanged"
 * that the fold-out is built for -- it is a teleport.
 *
 * Beta agrees, and asks for the WHOLE state rather than only the record:
 *   NetClientHandler.handleFlying:253  the player's ySize = 0.0F; (before the call)
 *   Entity.setPositionAndRotation:614  ySize = 0.0F;
 *   ... :608-610                       prevPosY = posY = the new y
 * handleFlying is precisely the server position packet these writers are, and
 * it zeroes ySize itself and then calls the one setter that zeroes it again
 * and re-seeds the render sample. Entity.setPosition:132 -- the one absolute
 * setter that does NOT zero ySize -- is the box-only helper, never the
 * teleport. So: full reset, and the dip comes back by itself on the next tick
 * if sneak is still held, exactly as it does in Beta after a teleport.
 *
 * Call sites (client_interface.c): the full-snapshot arm of
 * clin_apply_player_position and clin_guest_body_install -- the two writers
 * that ARE Beta's teleport packet. Pinned by tests/test_player_ysize.c's
 * test_wire_byte_identity and at the call site by
 * tests/test_local_player_column_guard.c. */
static inline void player_ysize_record_clear(struct player_ysize_state* s);
static inline void player_ysize_absolute_set(struct player_ysize_state* s) {
	player_ysize_reset(s);
	player_ysize_record_clear(s);
}

/* The RECORD-ONLY half, for clin_apply_position_only_correction (review 19
 * M-1). That arm also assigns camera.y absolutely, so the stale fold-out
 * record has to go exactly as above -- but it is NOT Beta's teleport packet.
 * It is this engine's shared correction channel: piston pushes, mob
 * knockback, local and guest corrections all arrive through it, and in Beta
 * every one of those moves the player through moveEntity / velocity, which
 * never touches ySize. Zeroing the dip there would pop the camera up 0.08
 * block and back, once per correction, for as long as sneak is latched -- a
 * jitter Beta does not have. So the dip STATE (y_size, y_size_prev) is kept
 * and only the record of the last attach is cleared: the wire still carries
 * exactly the assigned Y (applied == 0 makes the fold-out the identity), and
 * the next attach re-derives the record from the unchanged state, so the
 * rendered dip does not move at all. */
static inline void player_ysize_record_clear(struct player_ysize_state* s) {
	s->applied = 0.0F;
	s->pre_dip_y = 0.0F;
	s->post_dip_y = 0.0F;
}

/* EntityRenderer.orientCamera:444 -- the rendered dip, in blocks, to SUBTRACT
 * from the interpolated eye. */
static inline float player_ysize_lerp(
	const struct player_ysize_state* s, float tick_delta) {
	return s->y_size_prev + (s->y_size - s->y_size_prev) * tick_delta;
}

/* The wire fold-out. clin_update's SRPC_PLAYER_POS Y must be what the camera
 * would have been WITHOUT the dip, so the server's feet (eye - 1.62) and
 * collision box are byte-identical standing and sneaking at the same place.
 *
 * NOT `camera_y + applied`. In IEEE-754 (a - d) + d is not generally a. For
 * TODAY's dip values it happens to be: sweeping every float eye height from
 * 0.03125 to 3e7 against all nine reachable dips (0.2F*0.4F and its eight
 * decay steps), the naive add-back is bit-exact everywhere above y = 0.25 --
 * and a player eye cannot go below ~1.62 (feet on bedrock + yOffset), so the
 * simpler form would in fact be exact in the whole reachable domain.
 *
 * It is still not what this does, because that exactness is a property of the
 * particular magnitudes involved, not of the operation -- a larger ySize (the
 * step-up remainder of Entity.moveEntity:404-408) or a different apply site
 * would quietly break it,
 * and "byte-identical" is a guarantee the wire should hold BY CONSTRUCTION.
 * So the attach records the exact float on BOTH sides of its subtraction and
 * the fold-out returns the recorded pre-dip value plus whatever moved the
 * camera SINCE that attach:
 *
 *     wire = pre_dip_y + (camera_y - post_dip_y)
 *
 * With no external shift, camera_y == post_dip_y, their difference is exactly
 * +0.0F, and the result is bit-for-bit the pre-dip camera -- i.e. exactly what
 * a standing player at the same place would have sent. When something else
 * MOVED the stored camera between attaches (the minecart snapshot install in
 * clin_install_minecart shifts it), that shift is carried through unchanged,
 * which is the same treatment camera_correction already gets.
 *
 * "Moved" means SHIFTED. A writer that SETS camera_y absolutely leaves no dip
 * in the stored camera for this to fold back out, so it must clear the record
 * first -- player_ysize_absolute_set above, which is what every absolute
 * writer calls. Forgetting it put a stale dip on the wire for the whole of a
 * cross-dimension terrain hold (hostile review 17, H-2).
 *
 * SINGLE-SOURCED on purpose: clin_update and tests/test_player_ysize.c call
 * this same function, so the byte-identity proof cannot drift from the send. */
static inline float player_ysize_unapply_y(
	float camera_y, const struct player_ysize_state* s) {
	if(s->applied == 0.0F)
		return camera_y;
	return s->pre_dip_y + (camera_y - s->post_dip_y);
}

#endif
