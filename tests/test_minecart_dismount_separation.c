/* O08 / G4 (2026-10-04): player/cart separation after a PRODUCTION dismount.
 *
 * Beta 1.7.3 (citations: tmp/jobs/vehicle-smoke-1004/G4-citations.md):
 *   Entity.mountEntity(null) [CFR Entity.java:851-863]: the rider lands at the
 *     cart's exact posX/posZ, feet on the cart-box top; carts are not solid to
 *     it (EntityMinecart.getBoundingBox() == null, :81-83), so it drops to the
 *     floor inside the cart box.
 *   EntityLiving.onLivingUpdate [:581-589]: moveEntityWithHeading, then every
 *     canBePushed entity in boundingBox.expand(0.2F,0,0.2F) gets
 *     applyEntityCollision(this).
 *   EntityMinecart.applyEntityCollision [:554-612]: no push while
 *     d2 < (double)1.0E-4F (:562); else d = unit * min(1/dist,1) * 0.1F * 0.5,
 *     cart.addVelocity(-d), player.addVelocity(+d/4) (:607-609).
 *   The player consumes its motion in moveEntityWithHeading [:480-487]
 *     (move, then ground friction); no low-speed cutoff.
 *
 * Production path exercised here: SRPC_MINECART_RIDE_INTENT mount/dismount
 * (server_local_test_process_rpc), the server entity tick (the cart's
 * PLAYER-TO-CART NUDGE delivers both halves), and
 * server_local_tick_actor_knockback (the server-side consumer of the body's
 * +d/4), in the production per-tick order.
 *
 *   1  rest: Beta's d2 gate -- a dismount at the cart's own X/Z pushes nothing
 *      (persistent overlap at rest IS Beta), and remount works overlapped.
 *   2  residual motion (the PC smoke's case): impulse delivered bit-exact each
 *      overlapping tick, the bodies separate past Beta's box test, the body
 *      moved AWAY from the cart, then remount + switch-back succeed.
 *   3  cart pinned at an end stop: separation comes from the body alone.
 *   4  stale/world rejection + P2 isolation: a stale-epoch intent mutates
 *      nothing; an inert (mid-transfer) guest and a guest committed to an
 *      unbound world slot neither shove nor take a counter-shove; an admitted
 *      far guest is untouched by everything above.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_minecart.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

/* G8 client stand-in state (see the residual-drift section). */
static float g8_cvx, g8_cvz;
static int g8_adds;
static void g8_observe(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_LOCAL_PLAYER_VELOCITY
	   && call->payload.local_player_velocity.add
	   && call->payload.local_player_velocity.owner == 0) {
		g8_cvx += call->payload.local_player_velocity.vel[0];
		g8_cvz += call->payload.local_player_velocity.vel[2];
		g8_adds++;
	}
}
/* One client tick of the delivered motion: move, then the friction chosen
 * by onGround before the move (EntityLiving.java:450-487). */
static void g8_client_step(void);

#define YF 100
#define RY 101
#define XL 4  /* long open track, z = -28..44 */
#define XP 10 /* pinned track, z = 2..8, stone stop at z = 9 */

extern struct item item_minecart;

static char s_dir[128];
static struct server_local s;
static struct server_player guest;
static volatile int g_drain_run = 1;
static uint32_t g_seq = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)}));
}

static struct entity* cart_by_id(uint32_t id) {
	struct entity** e = dict_entity_get(s.entities, id);
	return e ? *e : NULL;
}

static uint32_t place_cart(int x, int z) {
	uint32_t before = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it);
		dict_entity_next(it))
		if(dict_entity_ref(it)->value->id > before)
			before = dict_entity_ref(it)->value->id;
	struct block_data on_b, where_b;
	assert(server_world_get_block(&s.worlds[0], x, RY, z, &on_b));
	assert(server_world_get_block(&s.worlds[0], x, RY + 1, z, &where_b));
	struct block_info on = {.block = &on_b, .x = x, .y = RY, .z = z};
	struct block_info where = {.block = &where_b, .x = x, .y = RY + 1, .z = z};
	struct item_data held = {.id = ITEM_MINECART, .count = 1};
	struct item* item = item_get(&held);
	assert(item == &item_minecart);
	/* Place from beside the rail, inside the reach/LOS gate. */
	s.player.x = x - 1.5;
	s.player.y = RY + PLAYER_EYE_HEIGHT;
	s.player.z = z + 0.5;
	assert(item->onItemPlace(&s, &s.player, &held, &where, &on, SIDE_TOP));
	uint32_t id = 0;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it);
		dict_entity_next(it))
		if(dict_entity_ref(it)->value->id > id)
			id = dict_entity_ref(it)->value->id;
	assert(id > before && cart_by_id(id)->type == ENTITY_MINECART);
	assert(server_local_test_tick_entities(&s)); /* settle on the rail */
	return id;
}

static void ride_intent(uint32_t id, bool desired, uint32_t epoch) {
	struct server_rpc rpc = {
		.type = SRPC_MINECART_RIDE_INTENT,
		.payload.minecart_ride_intent = {
			.entity_id = id,
			.input_epoch = epoch,
			.request_seq = g_seq++,
			.desired_occupied = desired,
		},
	};
	server_local_test_process_rpc(&s, &rpc);
}

/* One production server tick, in server_local_update's order: the entity
 * pass (cart tick -> nudge), then the knockback consumer. */
static void tick(void) {
	assert(server_local_test_tick_entities(&s));
	server_local_tick_actor_knockback(&s);
}

static bool host_rides(const struct entity* c) {
	return c->data.minecart.occupied
		&& c->data.minecart.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID
		&& s.player.riding_minecart;
}

/* Beta's box test (the nudge's own gate, EntityLiving :582 expand 0.2F):
 * player half-width 0.3F + 0.2F against the cart's 0.49F, feet-to-head
 * against the cart's centred 0.7F box. */
static bool overlaps(const struct server_player* p, const struct entity* c) {
	double reach = (double)0.3F + (double)0.2F;
	double pmin = p->y - (double)PLAYER_EYE_HEIGHT, pmax = pmin + (double)1.8F;
	double cmin = (double)c->pos[1] - (double)0.35F, cmax = cmin + 0.7;
	return p->x + reach > (double)c->pos[0] - (double)0.49F
		&& p->x - reach < (double)c->pos[0] + (double)0.49F
		&& p->z + reach > (double)c->pos[2] - (double)0.49F
		&& p->z - reach < (double)c->pos[2] + (double)0.49F
		&& pmax > cmin && pmin < cmax;
}

/* Beta's +d/4 for a body at (px,pz) against the cart at (cx,cz); false when
 * the d2 < 1.0E-4F gate suppresses it [EntityMinecart.java:562-575]. */
static bool beta_quarter(double px, double pz, double cx, double cz,
						 float* qx, float* qz) {
	double x = px - cx, z = pz - cz, d2 = x * x + z * z;
	if(d2 < (double)1.0E-4F)
		return false;
	double d = (double)(float)sqrt(d2);
	x /= d;
	z /= d;
	double f = 1.0 / d;
	if(f > 1.0)
		f = 1.0;
	x = x * f * (double)0.1F * 0.5;
	z = z * f * (double)0.1F * 0.5;
	*qx = (float)(x / 4.0);
	*qz = (float)(z / 4.0);
	return true;
}

/* Beta mountEntity(null): the cart's X/Z; then the fall through the
 * non-solid cart to the floor (feet on the stone top, RY). */
static void land_dismounted(const struct entity* c) {
	s.player.x = (double)c->pos[0];
	s.player.z = (double)c->pos[2];
	s.player.y = RY + PLAYER_EYE_HEIGHT;
	s.player.vel_x = s.player.vel_z = 0.0F;
}

static bool rail_under(const struct entity* c) {
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], (int)floorf(c->pos[0]), RY,
								  (int)floorf(c->pos[2]), &b));
	return b.type == BLOCK_RAIL;
}

static double g_guest_x, g_guest_z;
static void guest_far(void) {
	guest.x = g_guest_x = -20.5;
	guest.y = RY + PLAYER_EYE_HEIGHT;
	guest.z = g_guest_z = -20.5;
	guest.vel_x = guest.vel_z = 0.0F;
}
static void assert_guest_untouched(void) {
	assert(guest.x == g_guest_x && guest.z == g_guest_z
		   && guest.vel_x == 0.0F && guest.vel_z == 0.0F
		   && "P2 isolation: the far guest is never moved or shoved");
}

/* (1) at rest: Beta pushes nothing at d == 0. */
static void test_rest(uint32_t id) {
	struct entity* c = cart_by_id(id);
	s.player.x = c->pos[0] - 1.5;
	s.player.z = c->pos[2];
	ride_intent(id, true, s.input_epoch_counter);
	assert(host_rides(c));
	c->data.minecart.speed = 0.0F;
	ride_intent(id, false, s.input_epoch_counter);
	assert(!c->data.minecart.occupied && !s.player.riding_minecart);
	land_dismounted(c);
	const double hx = s.player.x, hz = s.player.z;
	const float cz = c->pos[2];
	for(int t = 0; t < 40; t++)
		tick();
	assert(s.player.x == hx && s.player.z == hz && c->pos[2] == cz
		   && c->data.minecart.speed == 0.0F && overlaps(&s.player, c)
		   && "Beta's d2 < 1.0E-4F gate: no push at the cart's own X/Z");
	ride_intent(id, true, s.input_epoch_counter);
	assert(host_rides(c) && "remount while overlapped");
	ride_intent(id, false, s.input_epoch_counter);
	assert(!c->data.minecart.occupied);
	assert_guest_untouched();
	puts("  1 rest dismount: no push at d=0 (Beta), remount overlapped PASS");
}

/* (2) residual cart motion after the dismount (the PC smoke tape:
 * vs_pc_cart_20261004_120516 seq 40-54). */
static void test_residual(uint32_t id) {
	struct entity* c = cart_by_id(id);
	s.player.x = c->pos[0] - 1.5;
	s.player.z = c->pos[2];
	ride_intent(id, true, s.input_epoch_counter);
	assert(host_rides(c));
	assert(c->data.minecart.hz != 0);
	/* Residual drift toward -Z, like the smoke's braked ride. */
	c->data.minecart.speed = -0.003F * (float)c->data.minecart.hz;
	ride_intent(id, false, s.input_epoch_counter);
	assert(!c->data.minecart.occupied && !s.player.riding_minecart);
	land_dismounted(c);
	const double hz0 = s.player.z;
	const float cz0 = c->pos[2];
	int pushes = 0, sep_tick = -1;
	/* G8: the server delivers the cart's +d/4 to the body's CLIENT as an
	 * additive velocity and no longer moves the body itself. This loop
	 * stands in for that client: it integrates the delivered motion
	 * (EntityPlayerSP moveEntityWithHeading: move, then friction chosen by
	 * onGround BEFORE the move) and reports the position back, as the
	 * client's PLAYER_POS sample would. */
	g8_cvx = g8_cvz = 0.0F;
	g8_adds = 0;
	clin_test_set_rpc_observer(g8_observe, NULL);
	for(int t = 0; t < 200 && sep_tick < 0; t++) {
		const double px = s.player.x, pz = s.player.z;
		const double cx = c->pos[0], cz = c->pos[2];
		const double vx = s.player.vel_x, vz = s.player.vel_z; /* v3: double server motion */
		const bool ov = overlaps(&s.player, c);
		float qx = 0.0F, qz = 0.0F;
		const bool push = ov && beta_quarter(px, pz, cx, cz, &qx, &qz);
		assert(server_local_test_tick_entities(&s));
		if(push) {
			pushes++;
			assert(s.player.vel_x == vx + (double)qx && s.player.vel_z == vz + (double)qz
				   && "impulse delivery: Beta's exact +d/4 on the body");
		} else {
			assert(s.player.vel_x == vx && s.player.vel_z == vz);
		}
		server_local_tick_actor_knockback(&s);
		g8_client_step();
		if(!overlaps(&s.player, c))
			sep_tick = t;
	}
	clin_test_set_rpc_observer(NULL, NULL);
	assert(g8_adds == pushes
		   && "every +d/4 reached the body's client as one additive velocity");
	printf("  2 residual: %d pushes, separated at tick %d, body dz %+.3f, "
		   "cart dz %+.3f\n", pushes, sep_tick, s.player.z - hz0,
		   (double)(c->pos[2] - cz0));
	assert(pushes > 0 && "the residual drift must break the d=0 symmetry");
	assert(sep_tick >= 0 && "physical separation past Beta's box test");
	assert(s.player.z > hz0
		   && "the client-integrated counter-shove moved the body away (+Z)");
	assert(c->pos[2] < cz0 && "the cart was pushed away (-Z)");
	assert(rail_under(c) && "the long track keeps the cart on its rail");
	/* Let the cart coast out; it stays shoved, the body does not drift. */
	for(int t = 0; t < 200; t++)
		tick();
	assert(!overlaps(&s.player, c) && fabs(s.player.vel_x) < 1e-6 /* v3: no non-Beta tail cutoff */
		   && fabs(s.player.vel_z) < 1e-6 && rail_under(c));
	/* Remount: the body walks up beside the cart (client locomotion), then
	 * the production intent. Then the switch-back: off and on again. */
	s.player.x = c->pos[0] - 1.2;
	s.player.z = c->pos[2];
	ride_intent(id, true, s.input_epoch_counter);
	assert(host_rides(c) && "remount after separation");
	ride_intent(id, false, s.input_epoch_counter);
	assert(!c->data.minecart.occupied);
	ride_intent(id, true, s.input_epoch_counter);
	assert(host_rides(c) && "switch-back remount");
	ride_intent(id, false, s.input_epoch_counter);
	assert(!c->data.minecart.occupied);
	assert_guest_untouched();
	puts("  2 residual: exact impulse, separation, remount + switch-back PASS");
}

static void g8_client_step(void) {
	const float k = s.player.on_ground ? 0.54600006F : 0.91F;
	s.player.x += (double)g8_cvx;
	s.player.z += (double)g8_cvz;
	g8_cvx *= k;
	g8_cvz *= k;
}

/* (3) ride the cart into the end stop (the smoke's +Z ride window ends
 * there), dismount AT the stop, then the body steps off toward the open
 * side (client locomotion). Beta: the cart is pinned -- every push into the
 * stop clips to 0 (calcZOffset) -- so only the body moves and it separates. */
static void test_pinned(uint32_t id) {
	struct entity* c = cart_by_id(id);
	s.player.x = c->pos[0] - 1.5;
	s.player.z = c->pos[2];
	ride_intent(id, true, s.input_epoch_counter);
	assert(host_rides(c) && c->data.minecart.hz != 0);
	c->data.minecart.speed = 0.2F * (float)c->data.minecart.hz; /* toward +Z */
	for(int t = 0; t < 40; t++)
		tick();
	const float cz = c->pos[2];
	assert(c->data.minecart.speed == 0.0F && rail_under(c)
		   && fabsf(cz - (9.0F - 0.49F)) < 1e-5F && "ridden to the stop face");
	ride_intent(id, false, s.input_epoch_counter);
	assert(!c->data.minecart.occupied && !s.player.riding_minecart);
	land_dismounted(c);
	for(int t = 0; t < 20; t++)
		tick();
	assert(c->pos[2] == cz && s.player.vel_z == 0.0F
		   && "d == 0 at the stop: no push (Beta)");
	s.player.z = (double)cz - 0.02; /* one step toward the open side */
	const double hz0 = s.player.z;
	int sep_tick = -1;
	g8_cvx = g8_cvz = 0.0F; /* G8: the client integrates the delivered push */
	clin_test_set_rpc_observer(g8_observe, NULL);
	for(int t = 0; t < 400 && sep_tick < 0; t++) {
		tick();
		g8_client_step();
		assert(c->pos[2] == cz && c->data.minecart.speed == 0.0F
			   && "the stop pins the pushed cart (Beta clips every push)");
		if(!overlaps(&s.player, c))
			sep_tick = t;
	}
	clin_test_set_rpc_observer(NULL, NULL);
	printf("  3 pinned: separated at tick %d, body dz %+.3f\n", sep_tick,
		   s.player.z - hz0);
	assert(sep_tick >= 0 && "the body alone separates from a pinned cart");
	assert(s.player.z < hz0 && rail_under(c));
	/* Remount + switch-back at the stop. */
	s.player.x = c->pos[0] - 1.2;
	ride_intent(id, true, s.input_epoch_counter);
	assert(host_rides(c));
	ride_intent(id, false, s.input_epoch_counter);
	ride_intent(id, true, s.input_epoch_counter);
	assert(host_rides(c));
	ride_intent(id, false, s.input_epoch_counter);
	assert(!c->data.minecart.occupied);
	assert_guest_untouched();
	puts("  3 dismount at the stop: cart stays pinned, body separates, "
		 "remount + switch-back PASS");
}

/* (4) stale lifetime / world rejection. */
static void test_stale(uint32_t id) {
	struct entity* c = cart_by_id(id);
	/* A stale-epoch ride intent mutates nothing. */
	s.player.x = c->pos[0] - 1.5;
	s.player.z = c->pos[2];
	assert(!c->data.minecart.occupied);
	ride_intent(id, true, s.input_epoch_counter - 1);
	assert(!c->data.minecart.occupied && !s.player.riding_minecart
		   && "a stale input epoch never mounts");
	/* Guest on the cart's +Z side, the host out of the box. Control: an
	 * admitted guest shoves the cart along its rail and takes +d/4. */
	s.player.x = c->pos[0] - 3.0;
	c->data.minecart.speed = 0.0F;
	guest.x = (double)c->pos[0];
	guest.z = (double)c->pos[2] + 0.3;
	guest.y = RY + PLAYER_EYE_HEIGHT;
	guest.vel_x = guest.vel_z = 0.0F;
	struct entity copy;
	/* G2: `copy` is a phantom duplicate of the dict cart c; the end-of-tick
	 * cart<->cart sweep (EntityMinecart :441-447) sees c as a neighbour and
	 * shoves it. Restore c so the next copy starts from the same state. */
	struct entity c_before;
	memcpy(&c_before, c, sizeof c_before);
	memcpy(&copy, c, sizeof copy);
	assert(!copy.tick_server(&copy, &s));
	assert(guest.vel_z > 0.0F && copy.data.minecart.speed != 0.0F
		   && "control: an admitted guest shoves and takes +d/4");
	memcpy(c, &c_before, sizeof c_before);
	guest.vel_x = guest.vel_z = 0.0F;
	/* Inert (mid-transfer presentation): no shove either way. */
	s.guest_owned = s.guest_confirmed = s.a2w_guest_presentation = true;
	assert(server_transfer_actor_inert(&s, &guest));
	memcpy(&copy, c, sizeof copy);
	assert(!copy.tick_server(&copy, &s));
	assert(guest.vel_z == 0.0F && copy.data.minecart.speed == 0.0F
		   && "an inert guest neither shoves nor is shoved");
	s.guest_owned = s.guest_confirmed = s.a2w_guest_presentation = false;
	/* Committed to an unbound world slot: not this cart's world. */
	guest.world_slot = 1;
	assert(server_local_world_of(&s, &guest) == NULL);
	memcpy(&copy, c, sizeof copy);
	assert(!copy.tick_server(&copy, &s));
	assert(guest.vel_z == 0.0F && copy.data.minecart.speed == 0.0F
		   && "a body of another world neither shoves nor is shoved");
	guest.world_slot = 0;
	guest_far();
	puts("  4 stale epoch, inert guest, other-world guest rejected PASS");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_minecart_dismount_separation_tmp", s_dir,
					 sizeof s_dir);
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	MKDIR(s_dir);
	MKDIR(test_fixture_join(s_dir, "/region"));
	remove(test_fixture_join(s_dir, "/region/r.0.0.mcr"));
	remove(test_fixture_join(s_dir, "/region/r.-1.-1.mcr"));
	remove(test_fixture_join(s_dir, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(s_dir, "/region/r.-1.0.mcr"));
	string_t level;
	string_init_set_str(level, s_dir);
	memset(&s, 0, sizeof s);
	dict_entity_init(s.entities);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	for(int z = -28; z <= 44; z++) {
		set(XL, YF, z, BLOCK_STONE, 0);
		set(XL, RY, z, BLOCK_RAIL, 0);
	}
	for(int z = 2; z <= 9; z++)
		set(XP, YF, z, BLOCK_STONE, 0);
	for(int z = 2; z <= 8; z++)
		set(XP, RY, z, BLOCK_RAIL, 0);
	set(XP, RY, 9, BLOCK_STONE, 0);

	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = guest.has_pos = true;
	s.player.finished_loading = guest.finished_loading = true;
	s.player.health = guest.health = MAX_PLAYER_HEALTH;
	s.input_epoch_counter = 2;
	s.guest_actor = &guest;
	s.guest_gameplay = true;
	s.guest_entity_id = 77;
	guest_far();
	assert(server_local_world_of(&s, &s.player) == &s.worlds[0]);
	assert(server_local_world_of(&s, &guest) == &s.worlds[0]);

	uint32_t long_cart = place_cart(XL, 8);
	uint32_t pin_cart = place_cart(XP, 5);
	test_rest(long_cart);
	test_residual(long_cart);
	test_pinned(pin_cart);
	test_stale(long_cart);

	g_drain_run = 0;
	thread_join(&drain);
	puts("test_minecart_dismount_separation: PASS");
	return 0;
}
