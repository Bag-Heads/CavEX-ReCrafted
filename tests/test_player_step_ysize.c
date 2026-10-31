/* Beta 1.7.3 Entity.moveEntity:259, 404-408, 413 and
 * EntityRenderer.orientCamera:444: accepted step-up ySize and eye height. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "entity/player_ysize.h"
#include "game/camera.h"
#include "game/client_session.h"
#include "game/game_state.h"

static uint32_t bits(float value) {
	uint32_t result;
	memcpy(&result, &value, sizeof(result));
	return result;
}

static void check_half_step(void) {
	struct player_ysize_state s = {0};
	/* Beta moves the box first, then banks fractional minY + 0.01D.
	 * The floor is 64.0 and the accepted slab top is 64.5. */
	player_ysize_pre_move(&s, false);
	player_ysize_move(&s);
	player_ysize_step(&s, 64.5F);
	assert(bits(s.y_size_prev) == 0x00000000u);
	assert(bits(s.y_size) == 0x3F028F5Cu); /* (float)(0 + .5 + .01) */

	struct client_session session = {0};
	struct entity body = {0};
	body.pos_old[0] = body.pos[0] = 8.0F;
	body.pos_old[1] = 64.0F;
	body.pos[1] = 64.5F;
	body.pos_old[2] = body.pos[2] = 8.0F;
	session.camera.ysize = s;
	struct camera_player_view view = {.world = &gstate.world,
		.ride = &session.ride, .sleeping = false, .yaw = 0.0F,
		.pitch = 0.0F};
	camera_attach_owned(&session.camera, &body, 0.5F, 0.0F, &view);
	assert(bits(session.camera.ysize.applied) == 0x3E828F5Cu); /* .255 */
	assert(bits(session.camera.y) == 0x427FFAE1u); /* 64.25 - .255 */
	assert(bits(player_ysize_unapply_y(session.camera.y, &session.camera.ysize))
		   == 0x42808000u); /* the wire still gets 64.25 */
	assert(bits(body.pos_old[1]) == 0x42800000u);
	assert(bits(body.pos[1]) == 0x42810000u);
	camera_attach_owned(&session.camera, &body, 1.0F, 0.0F, &view);
	assert(bits(session.camera.y) == 0x427FF5C3u); /* 64.5 - .51 */
	assert(bits(player_ysize_unapply_y(session.camera.y, &session.camera.ysize))
		   == 0x42810000u); /* the wire still gets 64.5 */

	/* At subsequent tick tops the previous sample is the old post-move
	 * value. Entity.moveEntity multiplies by 0.4F before each move. */
	static const uint32_t decay[] = {
		0x3E50E560u, 0x3DA71DE7u, 0x3D05B186u,
		0x3C55E8D7u, 0x3BAB20ACu,
	};
	uint32_t prev = 0x3F028F5Cu;
	for(unsigned i = 0; i < sizeof(decay) / sizeof(decay[0]); i++) {
		player_ysize_pre_move(&s, false);
		assert(bits(s.y_size_prev) == prev);
		player_ysize_move(&s);
		assert(bits(s.y_size) == decay[i]);
		prev = decay[i];
	}
	puts("PASS half-step ySize decay and partial-tick camera eye");
}

static void check_whole_step(void) {
	struct player_ysize_state s = {0};
	player_ysize_pre_move(&s, false);
	player_ysize_move(&s);
	player_ysize_step(&s, 65.0F);
	/* Beta's (int) cast leaves zero fractional remainder at an integer
	 * box minimum: the 0.01D term is inside the positive-remainder guard. */
	assert(bits(s.y_size) == 0x00000000u);
	struct client_session session = {0};
	struct entity body = {0};
	body.pos_old[1] = 64.0F;
	body.pos[1] = 65.0F;
	session.camera.ysize = s;
	struct camera_player_view view = {.world = &gstate.world,
		.ride = &session.ride, .sleeping = false, .yaw = 0.0F,
		.pitch = 0.0F};
	camera_attach_owned(&session.camera, &body, 0.5F, 0.0F, &view);
	assert(bits(session.camera.y) == 0x42810000u); /* 64.5 */
	camera_attach_owned(&session.camera, &body, 1.0F, 0.0F, &view);
	assert(bits(session.camera.y) == 0x42820000u); /* 65.0 */
	for(unsigned i = 0; i < 3; i++) {
		player_ysize_pre_move(&s, false);
		player_ysize_move(&s);
		assert(bits(s.y_size) == 0x00000000u);
	}
	puts("PASS integer-height step has no ySize bank (Beta guard)");
}

static void check_sneak_step_cast_chain(void) {
	/* Nonzero prior ySize: sneak held (EntityPlayerSP.onLivingUpdate:79-80
	 * floors ySize to 0.2F), Entity.moveEntity:259 decays it to
	 * 0.2F * 0.4F = 0x3DA3D70B, then the stepped route wins onto a slab top
	 * at 64.5. Entity.java:407 banks
	 *     this.ySize = (float)((double)this.ySize + var41 + 0.01D);
	 * -- ONE double sum and ONE cast. The double sum 0.08F + 0.5 + 0.01 rounds
	 * to 0x3F170A3E; summing in float twice (0.5F + 0.01F, then + ySize)
	 * rounds to 0x3F170A3D instead. */
	struct player_ysize_state s = {0};
	player_ysize_pre_move(&s, true);
	assert(bits(s.y_size) == 0x3E4CCCCDu); /* 0.2F floor */
	player_ysize_move(&s);
	assert(bits(s.y_size) == 0x3DA3D70Bu); /* 0.2F * 0.4F */
	player_ysize_step(&s, 64.5F);
	printf("sneak step ySize bits = 0x%08X (Beta 0x3F170A3E)\n",
		(unsigned)bits(s.y_size));
	assert(bits(s.y_size) == 0x3F170A3Eu);
	puts("PASS sneak step banks with Beta double sum + single cast");
}

int main(void) {
	check_half_step();
	check_whole_step();
	check_sneak_step_cast_chain();
	puts("PASS test_player_step_ysize");
	return 0;
}
