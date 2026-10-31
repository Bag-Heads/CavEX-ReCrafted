/*
	test_local_step_camera_golden -- next-gaps-0924/STEPCAM. FAITHFULNESS
	golden for the LOCAL player's step-up camera ease (Entity.ySize), driven
	through the real walk (entity_local_player_walk_owned) onto a real slab.

	Spec (Beta 1.7.3, ../mcb173-retromcp/minecraft/src_original/net/minecraft/src):
	  Entity.moveEntity:259      ySize *= 0.4F;   (first statement of the move)
	  Entity.moveEntity:403-409  stepped route wins ->
	      frac = boundingBox.minY - (double)((int)boundingBox.minY), in double;
	      a positive frac banks ySize = (float)((double)ySize + frac + 0.01D);
	  Entity.moveEntity:413      posY = boundingBox.minY + yOffset - ySize;
	  Entity.onEntityUpdate:163  prevPosY = posY   (top of the tick)
	  EntityRenderer.orientCamera:189 / RenderGlobal:266
	      eye = prevPosY + (posY - prevPosY) * partialTick   (yOffset - 1.62F == 0)
	  EntityPlayerSP.onLivingUpdate:79-80  sneak floor ySize = 0.2F.

	After the stepped route, boundingBox.minY is EXACTLY the slab top (the
	final -stepHeight pass is clipped by calculateYOffset onto the face), so
	frac is exactly 0.5 and the bank is the pure function of the prior ySize:
	    prior 0:            (float)(0.0 + 0.5 + 0.01)          = 0x3F028F5C
	    sneak prior 0.08F:  (float)((double)0.08F + 0.5 + 0.01) = 0x3F170A3E
	then Beta's 0.4F decay chain 0x3E50E560, 0x3DA71DE7, 0x3D05B186,
	0x3C55E8D7, 0x3BAB20AC (float multiply, same as test_player_step_ysize).

	WHY TWO HEIGHTS. This port keeps the body as a float EYE and places it on a
	face with entity_try_move's "face - lo, nudged up one ulp if short" rule,
	so the float feet (eye + lo) can land a few ulps ABOVE the face when the
	eye and the feet sit in different binades -- feet 63.5 / eye 65.12 is one
	of them (a slab on a sea-level floor). Feeding those float feet to the bank
	gave 0.5 + ulps instead of Beta's exact 0.5. The y = 101 control has no
	binade split and was already exact. The COLLISION placement is NOT changed
	by the fix: this test pins e->pos[1] to the entity_try_move placement.

	RED on the pre-fix base (measured, dadbffd1 call site): the sea-level
	walk banks 0x3F028F9C and the sneak step 0x3F170A7E (want 0x3F028F5C /
	0x3F170A3E), and at y=31 even the settle onto flat ground -- placed one
	ulp low, lifted by the step route -- banked a spurious ~0.01 that Beta
	never has (integer minY, frac == 0). The grid sweep and the y=101
	control pass on both sides.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "entity/player_ysize.h"
#include "game/camera.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/server_local_state.h" /* MAX_OXYGEN */
#include "platform/input_owner.h"
#include "world.h"

static int s_checks, s_failures;

static uint32_t bits(float value) {
	uint32_t result;
	memcpy(&result, &value, sizeof(result));
	return result;
}

static void bits_eq(const char* what, int tick, float got, uint32_t want) {
	s_checks++;
	if(bits(got) != want) {
		s_failures++;
		fprintf(stderr, "FAIL %s tick %d: got 0x%08X (%.9g) want 0x%08X\n",
				what, tick, (unsigned)bits(got), (double)got, (unsigned)want);
	}
}

static void near_eq(const char* what, float got, double want, double tol) {
	s_checks++;
	if(fabs((double)got - want) > tol) {
		s_failures++;
		fprintf(stderr, "FAIL %s: got %.9f want %.9f (tol %.3g)\n", what,
				(double)got, want, tol);
	}
}

static struct client_session s_owner;

static void cfill(int x0, int x1, int y0, int y1, int z0, int z1,
				  uint8_t type, uint8_t meta) {
	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++)
				world_set_block(&gstate.world, x, y, z,
								(struct block_data) {.type = type,
													 .metadata = meta},
								false);
}

/* The walk's relative box minimum: aabb_setsize_centered(0.6, 1.8, 0.6) then
 * aabb_translate(0, 1.8F / 2.0F - EYE_HEIGHT, 0), exactly as the walk builds
 * it (entity_local_player.c). */
static float walk_box_lo(void) {
	volatile float lo = -(1.8F / 2.0F);
	lo += 1.8F / 2.0F - 1.62F;
	return lo;
}

/* entity_try_move's downward face placement (entity.c): np = face - lo,
 * nudged up one ulp if np + lo falls short of the face. The COLLISION result
 * this change must not move. */
static float face_placement_eye(float face) {
	const float lo = walk_box_lo();
	float np = face - lo;
	if(np + lo < face)
		np = nextafterf(np, INFINITY);
	return np;
}

static void tick(struct entity* e, bool forward, bool sneak) {
	struct input_player_frame frame = {.connected = true};
	frame.down[IB_FORWARD] = forward;
	frame.down[IB_SNEAK] = sneak;
	assert(!entity_local_player_walk_owned(e, &s_owner, &frame));
}

/* Floor top at floor_y + 1, bottom slab row one block in front (+z). */
static void run_case(const char* name, int floor_y, bool sneak,
					 uint32_t want_bank) {
	const int slab_y = floor_y + 1;
	const float feet0 = (float)slab_y;
	const float face = (float)slab_y + 0.5F;
	/* Write AIR above the floor first: at the binade heights the head crosses
	 * into the next 16-high chunk section (y 64, y 32), and a section this
	 * fixture never wrote is an unloaded, unreadable cell -- a wall. */
	cfill(2, 10, slab_y, slab_y + 3, 2, 14, BLOCK_AIR, 0);
	cfill(2, 10, floor_y, floor_y, 2, 14, BLOCK_STONE, 0);
	cfill(2, 10, slab_y, slab_y, 8, 8, BLOCK_SLAB, 0);

	struct entity e;
	memset(&e, 0, sizeof e);
	entity_local_player(91, &e, &gstate.world);
	e.pos[0] = 6.5F;
	e.pos[1] = feet0 + 1.62F;
	e.pos[2] = 5.5F;
	e.orient[1] = GLM_PI_2;
	e.on_ground = true;
	e.data.local_player.capture_input = true;
	memset(&s_owner, 0, sizeof s_owner);
	s_owner.local_player = &e;
	s_owner.oxygen = MAX_OXYGEN;
	/* Settle into Beta's standing state; sneak (if any) is held from here on
	 * so the prior ySize at the step is the steady 0.2F * 0.4F. */
	for(int t = 0; t < 4; t++)
		tick(&e, false, sneak);
	assert(e.on_ground);
	e.pos[0] = 6.5F;
	e.pos[2] = 5.5F;
	e.vel[0] = e.vel[2] = 0.0F;
	const uint32_t steady = sneak ? 0x3DA3D70Bu : 0x00000000u;
	bits_eq("steady ySize before the step", -1, s_owner.camera.ysize.y_size,
			steady);

	int step_tick = -1;
	for(int t = 0; t < 120 && step_tick < 0; t++) {
		const float before = e.pos[1];
		tick(&e, true, sneak);
		if(getenv("STEPCAM_DBG"))
			fprintf(stderr, "DBG t=%d x=%.6f y=%.6f z=%.6f g=%d\n", t,
					(double)e.pos[0], (double)e.pos[1], (double)e.pos[2],
					e.on_ground);
		if(e.pos[1] > before + 0.25F)
			step_tick = t;
		else
			bits_eq("ySize while walking to the slab", t,
					s_owner.camera.ysize.y_size, steady);
	}
	s_checks++;
	if(step_tick < 0) {
		s_failures++;
		fprintf(stderr, "FAIL %s: never stepped onto the slab\n", name);
		return;
	}

	/* Collision is untouched: the eye is exactly entity_try_move's face
	 * placement, the pre-existing result. */
	bits_eq("eye = entity_try_move face placement", step_tick, e.pos[1],
			bits(face_placement_eye(face)));
	bits_eq("pre-step ySize sample (Entity:163)", step_tick,
			s_owner.camera.ysize.y_size_prev, steady);
	bits_eq("step bank (Entity.moveEntity:403-409)", step_tick,
			s_owner.camera.ysize.y_size, want_bank);

	/* Rendered eye on the step tick, Beta double math vs this port's float
	 * camera. Beta: prevPosY = feet0 + 1.62F - steady, posY = face + 1.62F -
	 * bank. The camera is float here (one float rounding of the lerp and one
	 * of the subtraction), so the comparison allows 2 float ulps at |y| < 128
	 * (2 * 2^-17); the ySize STATE above is the bit-exact part. */
	struct camera_player_view view = {.world = &gstate.world,
		.ride = &s_owner.ride, .sleeping = false, .yaw = 0.0F,
		.pitch = 0.0F};
	float steady_f;
	uint32_t steady_bits = steady;
	memcpy(&steady_f, &steady_bits, sizeof steady_f);
	float bank_f;
	memcpy(&bank_f, &want_bank, sizeof bank_f);
	const double beta_prev = (double)feet0 + (double)1.62F - (double)steady_f;
	const double beta_now = (double)face + (double)1.62F - (double)bank_f;
	static const float partial[] = {0.0F, 0.25F, 0.5F, 1.0F};
	for(unsigned i = 0; i < 4; i++) {
		camera_attach_owned(&s_owner.camera, &e, partial[i], 0.0F, &view);
		near_eq("rendered eye vs Beta orientCamera", s_owner.camera.y,
				beta_prev + (beta_now - beta_prev) * (double)partial[i],
				2.0 / 131072.0);
		/* The wire keeps the undipped eye (player_ysize.h fold-out). */
		const float undipped = e.pos_old[1]
			+ (e.pos[1] - e.pos_old[1]) * partial[i];
		bits_eq("wire fold-out = undipped eye", (int)i,
				player_ysize_unapply_y(s_owner.camera.y,
									   &s_owner.camera.ysize),
				bits(undipped));
	}

	if(!sneak) {
		/* Stand on the slab: Beta's 0.4F decay, one multiply per tick. */
		static const uint32_t decay[] = {
			0x3E50E560u, 0x3DA71DE7u, 0x3D05B186u, 0x3C55E8D7u, 0x3BAB20ACu,
		};
		uint32_t prev = want_bank;
		for(unsigned i = 0; i < sizeof decay / sizeof decay[0]; i++) {
			tick(&e, false, false);
			bits_eq("decay sample (prev)", (int)i,
					s_owner.camera.ysize.y_size_prev, prev);
			bits_eq("decay (Entity.moveEntity:259)", (int)i,
					s_owner.camera.ysize.y_size, decay[i]);
			prev = decay[i];
		}
	}

	cfill(2, 10, floor_y, slab_y + 2, 2, 14, BLOCK_AIR, 0);
	printf("%s %s (step at walk tick %d)\n",
		   s_failures ? "RED " : "PASS", name, step_tick);
	fflush(stdout);
}

/* Every 1/16-grid face at y 0..127 (all Beta block collision tops).
 * The PREMISE: the float feet derived from entity_try_move's placement eye
 * (eye + lo) miss the face at the binade splits -- which is why the local
 * step route runs on the box minimum and hands the exact minimum to the bank
 * (entity_local_player.c) instead of re-deriving it from the eye. And the
 * bank of an exact face equals Beta's double-math bank. */
static void sweep_grid_faces(void) {
	const float lo = walk_box_lo();
	int off_before = 0;
	for(int k = 0; k < 128; k++) {
		for(int s = 0; s < 16; s++) {
			const float face = (float)k + (float)s / 16.0F;
			const float eye = face_placement_eye(face);
			if(eye + lo != face)
				off_before++;
			struct player_ysize_state st = {0};
			player_ysize_step(&st, face);
			const double frac = (double)face - (double)(int)(double)face;
			const float beta = frac > 0.0 ? (float)(0.0 + frac + 0.01) : 0.0F;
			bits_eq("sweep bank vs Beta double", k * 16 + s, st.y_size,
					bits(beta));
		}
	}
	/* The premise: float feet miss the face at the binade splits. */
	s_checks++;
	if(off_before != 126) {
		s_failures++;
		fprintf(stderr, "FAIL sweep premise: %d faces off, expected 126\n",
				off_before);
	}
	printf("%s grid sweep (2048 faces, %d had float feet above the face)\n",
		   s_failures ? "RED " : "PASS", off_before);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	sweep_grid_faces();
	blocks_init();
	items_init();
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);

	/* Control: feet 101.5 / eye 103.12 share a binade -- exact pre-fix too. */
	run_case("control slab at y=101, walking", 100, false, 0x3F028F5Cu);
	/* Sea level: feet 63.5 (binade [32,64)) / eye 65.12 ([64,128)). */
	run_case("sea-level slab at y=63, walking", 62, false, 0x3F028F5Cu);
	run_case("sea-level slab at y=63, sneak held", 62, true, 0x3F170A3Eu);
	/* Feet 31.5 / eye 33.12, the next binade split down. */
	run_case("slab at y=31, walking", 30, false, 0x3F028F5Cu);

	if(s_failures) {
		fprintf(stderr, "test_local_step_camera_golden: %d of %d checks FAILED\n",
				s_failures, s_checks);
		return 1;
	}
	printf("PASS test_local_step_camera_golden (%d checks)\n", s_checks);
	return 0;
}
