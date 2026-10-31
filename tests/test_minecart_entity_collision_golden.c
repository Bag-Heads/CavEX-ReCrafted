/* G2 (vehicle-smoke-1004): minecart <-> entity and minecart <-> minecart
 * collision, Beta 1.7.3 golden.
 *
 * GROUND TRUTH -- CFR server decompile, net/minecraft/src/EntityMinecart.java:
 *   :441-447 end of onUpdate queries the box expanded by 0.2F horizontally.
 *             Each neighbouring cart except the rider receives the collision call.
 *
 *   :554-612 applyEntityCollision on the receiving cart:
 *     Measure squared horizontal separation; require double-promoted 1.0E-4F.
 *     Normalize with the float-rounded square root and clamp its reciprocal to one.
 *     Scale by double-promoted 0.1F, one minus the zero collision reduction (one) and one half.
 *     For another cart, test the position/motion alignment before exchange.
 *     The alignment uses the mover's prevPosX, Beta's own quirk; its square
 *     above five aborts the exchange. Sum the two horizontal momenta.
 *     If only the mover is a furnace cart, damp the receiver to 0.2F,
 *     add mover momentum minus push, then damp the mover to 0.7F.
 *     If only the receiver is a furnace cart, damp the mover to 0.2F,
 *     add receiver momentum plus push, then damp the receiver to 0.7F.
 *     Otherwise damp both to 0.2F and add half their summed momentum,
 *     subtracting push on the receiver and adding it on the mover.
 *     For a non-cart, push the receiver oppositely and the other body by a quarter.
 *
 *   EntityLiving.onLivingUpdate's sweep invokes the cart collision handler for the mob
 *   for every canBePushed neighbour (carts answer true, :85-87).
 *
 * ENGINE: the sweep is entity_minecart_collide_neighbours, run at the end of
 * the server cart tick; a cart's Beta motion is speed*heading + e->vel (G1),
 * addVelocity lands in e->vel. Carts here are off-rail on a stone floor
 * (motion == e->vel exactly). Expected values: Beta's double arithmetic from
 * the engine's float state, within 1e-6.
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
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
#include "entity/entity.h"
#include "entity/entity_minecart.h"
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100
#define CART_Y 101.35F /* resting with position height 0.35 above the floor top at 101 */
#define TOL 1e-6

static struct server_local s;
static volatile int g_drain_run = 1;
static unsigned failures;

static void check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if(!ok)
		failures++;
}

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void clear_kind(int type) {
	bool again = true;
	while(again) {
		again = false;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			if(e && e->type == type) {
				dict_entity_erase(s.entities, e->id);
				again = true;
				break;
			}
			dict_entity_next(it);
		}
	}
}

static struct entity* cart_at(float x, float z, float vx, float vz,
							  float prev_x, uint8_t type) {
	struct entity* e = server_local_spawn_minecart((vec3) {x, CART_Y, z}, &s);
	assert(e);
	e->pos[0] = x;
	e->pos[1] = CART_Y;
	e->pos[2] = z;
	e->pos_old[0] = prev_x;
	e->pos_old[1] = CART_Y;
	e->pos_old[2] = z;
	e->vel[0] = vx;
	e->vel[1] = 0.0F;
	e->vel[2] = vz;
	e->on_ground = true;
	e->data.minecart.speed = 0.0F;
	e->data.minecart.minecart_type = type;
	e->data.minecart.occupied = false;
	e->data.minecart.occupant_id = 0;
	e->data.minecart.rail_meta = e->data.minecart.last_meta = 0xFF;
	return e;
}

struct bm {
	double x, z, mx, mz, prev_x;
	int type;
};
static struct bm bm_of(const struct entity* e) {
	return (struct bm) {e->pos[0], e->pos[2], e->vel[0], e->vel[2],
						e->pos_old[0], e->data.minecart.minecart_type};
}

/* Beta EntityMinecart.applyEntityCollision: receiver r, moving cart m. */
static void beta_cart_cart(struct bm* r, struct bm* m) {
	double push_x = m->x - r->x, push_z = m->z - r->z;
	double separation = push_x * push_x + push_z * push_z;
	if(!(separation >= (double)1.0E-4F))
		return;
	separation = (double)(float)sqrt(separation);
	push_x /= separation;
	push_z /= separation;
	double reciprocal_clamp = 1.0 / separation;
	if(reciprocal_clamp > 1.0)
		reciprocal_clamp = 1.0;
	push_x *= reciprocal_clamp;
	push_z *= reciprocal_clamp;
	push_x *= (double)0.1F;
	push_z *= (double)0.1F;
	push_x *= (double)(1.0F - 0.0F);
	push_z *= (double)(1.0F - 0.0F);
	push_x *= 0.5;
	push_z *= 0.5;
	double delta_x = m->x - r->x, delta_z = m->z - r->z;
	double alignment = delta_x * m->mz + delta_z * m->prev_x;
	if(alignment * alignment > 5.0)
		return;
	double momentum_x = m->mx + r->mx, momentum_z = m->mz + r->mz;
	if(m->type == 2 && r->type != 2) {
		r->mx *= (double)0.2F;
		r->mz *= (double)0.2F;
		r->mx += m->mx - push_x;
		r->mz += m->mz - push_z;
		m->mx *= (double)0.7F;
		m->mz *= (double)0.7F;
	} else if(m->type != 2 && r->type == 2) {
		m->mx *= (double)0.2F;
		m->mz *= (double)0.2F;
		m->mx += r->mx + push_x;
		m->mz += r->mz + push_z;
		r->mx *= (double)0.7F;
		r->mz *= (double)0.7F;
	} else {
		r->mx *= (double)0.2F;
		r->mz *= (double)0.2F;
		r->mx += momentum_x / 2.0 - push_x;
		r->mz += momentum_z / 2.0 - push_z;
		m->mx *= (double)0.2F;
		m->mz *= (double)0.2F;
		m->mx += momentum_x / 2.0 + push_x;
		m->mz += momentum_z / 2.0 + push_z;
	}
}

static bool near(double a, double b) {
	return fabs(a - b) <= TOL;
}

static void cart_case(const char* label, uint8_t r_type, uint8_t m_type) {
	clear_kind(ENTITY_MINECART);
	struct entity* r = cart_at(5.0F, 8.5F, 0.05F, 0.0F, 5.0F, r_type);
	struct entity* m = cart_at(5.8F, 8.5F, -0.1F, 0.02F, 5.9F, m_type);
	struct bm br = bm_of(r), bmm = bm_of(m);
	beta_cart_cart(&br, &bmm);
	entity_minecart_collide_neighbours(&s, m); /* m is the sweeping cart */
	char what[200];
	snprintf(what, sizeof(what),
			 "%s: receiver motion (%.6f,%.6f) Beta (%.6f,%.6f)", label,
			 (double)r->vel[0], (double)r->vel[2], br.mx, br.mz);
	check(near(r->vel[0], br.mx) && near(r->vel[2], br.mz), what);
	snprintf(what, sizeof(what),
			 "%s: sweeper motion (%.6f,%.6f) Beta (%.6f,%.6f)", label,
			 (double)m->vel[0], (double)m->vel[2], bmm.mx, bmm.mz);
	check(near(m->vel[0], bmm.mx) && near(m->vel[2], bmm.mz), what);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_minecart_entity_collision_golden_tmp",
					 s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	struct server_chunk* sc = NULL;
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++) {
			server_world_set_block_silent(&s.worlds[0], x, YF, z,
				(struct block_data) {.type = BLOCK_STONE});
			for(int y = YF + 1; y <= YF + 4; y++)
				server_world_set_block_silent(&s.worlds[0], x, y, z,
					(struct block_data) {.type = BLOCK_AIR});
		}

	/* (A) cart <-> cart, the three type branches (:592-611). */
	cart_case("(A1) cart/cart", 0, 0);
	cart_case("(A2) furnace sweeper -> cart", 0, 2);
	cart_case("(A3) cart sweeper -> furnace", 2, 0);

	/* (B) Beta's prevPosX quirk (:586-589): an overlapping pair offset in Z
	 * with a large prevPosX skips the exchange entirely. */
	{
		clear_kind(ENTITY_MINECART);
		struct entity* r = cart_at(5.0F, 8.5F, 0.05F, 0.0F, 5.0F, 0);
		struct entity* m = cart_at(5.6F, 9.0F, -0.1F, 0.02F, 5.6F, 0);
		struct bm br = bm_of(r), bmm = bm_of(m);
		beta_cart_cart(&br, &bmm);
		check(br.mx == (double)0.05F && bmm.mx == (double)-0.1F,
			  "(B) model: var14^2 > 5 returns early");
		entity_minecart_collide_neighbours(&s, m);
		check(r->vel[0] == 0.05F && r->vel[2] == 0.0F && m->vel[0] == -0.1F
				  && m->vel[2] == 0.02F,
			  "(B) quirk honoured: no exchange (var14^2 > 5)");
	}

	/* (C) Not overlapping (expand 0.2 only on X/Z): nothing happens. */
	{
		clear_kind(ENTITY_MINECART);
		struct entity* r = cart_at(5.0F, 8.5F, 0.05F, 0.0F, 5.0F, 0);
		struct entity* m = cart_at(6.5F, 8.5F, -0.1F, 0.0F, 6.5F, 0);
		entity_minecart_collide_neighbours(&s, m);
		check(r->vel[0] == 0.05F && m->vel[0] == -0.1F,
			  "(C) 1.5 apart (> 0.98 + 0.2): no exchange");
	}

	/* (C2) Boxes 0.12 apart (centres 1.1): only the sweep's horizontal
	 * expansion by 0.2F [:441] reaches the neighbour -- Beta still exchanges. */
	{
		clear_kind(ENTITY_MINECART);
		struct entity* r = cart_at(5.0F, 8.5F, 0.05F, 0.0F, 5.0F, 0);
		struct entity* m = cart_at(6.1F, 8.5F, -0.1F, 0.0F, 6.1F, 0);
		struct bm br = bm_of(r), bmm = bm_of(m);
		beta_cart_cart(&br, &bmm);
		entity_minecart_collide_neighbours(&s, m);
		check(near(r->vel[0], br.mx) && near(m->vel[0], bmm.mx)
				  && br.mx != (double)0.05F,
			  "(C2) the 0.2 sweep expansion reaches a cart 0.12 away");
	}

	/* (D) living -> cart: a zombie beside a parked cart. The cart receives
	 * the opposite impulse, the mob a quarter [:612-613, non-cart branch]. */
	{
		clear_kind(ENTITY_MINECART);
		struct entity* cart = cart_at(10.5F, 10.5F, 0.0F, 0.0F, 10.5F, 0);
		struct entity* mob = server_local_spawn_monster_at(
			(vec3) {11.2F, 101.0F, 10.8F}, MONSTER_ZOMBIE, &s);
		assert(mob && mob->type == ENTITY_MONSTER);
		double mob_x = mob->data.monster.motion_d.valid
			? mob->data.monster.motion_d.pos[0] : (double)mob->pos[0];
		double mob_z = mob->data.monster.motion_d.valid
			? mob->data.monster.motion_d.pos[2] : (double)mob->pos[2];
		double mvx0 = mob->data.monster.motion_d.valid
			? mob->data.monster.motion_d.motion[0] : (double)mob->vel[0];
		double mvz0 = mob->data.monster.motion_d.valid
			? mob->data.monster.motion_d.motion[2] : (double)mob->vel[2];
		double push_x = mob_x - (double)cart->pos[0];
		double push_z = mob_z - (double)cart->pos[2];
		double separation = (double)(float)sqrt(push_x * push_x + push_z * push_z);
		push_x /= separation;
		push_z /= separation;
		double reciprocal_clamp = 1.0 / separation > 1.0 ? 1.0 : 1.0 / separation;
		push_x *= reciprocal_clamp * (double)0.1F * 0.5;
		push_z *= reciprocal_clamp * (double)0.1F * 0.5;
		entity_minecart_collide_neighbours(&s, cart);
		char what[200];
		snprintf(what, sizeof(what),
				 "(D) cart shoved by the mob: (%.6f,%.6f) Beta (%.6f,%.6f)",
				 (double)cart->vel[0], (double)cart->vel[2], -push_x, -push_z);
		check(near(cart->vel[0], -push_x) && near(cart->vel[2], -push_z), what);
		double mvx = mob->data.monster.motion_d.valid
			? mob->data.monster.motion_d.motion[0] : (double)mob->vel[0];
		double mvz = mob->data.monster.motion_d.valid
			? mob->data.monster.motion_d.motion[2] : (double)mob->vel[2];
		snprintf(what, sizeof(what),
				 "(D) mob counter-shove +d/4: (%.6f,%.6f) Beta (%.6f,%.6f)",
				 mvx - mvx0, mvz - mvz0, push_x / 4.0, push_z / 4.0);
		check(near(mvx - mvx0, push_x / 4.0) && near(mvz - mvz0, push_z / 4.0), what);

		/* negative control: a mob 2 blocks away does nothing */
		clear_kind(ENTITY_MONSTER);
		cart->vel[0] = cart->vel[2] = 0.0F;
		struct entity* far = server_local_spawn_monster_at(
			(vec3) {12.6F, 101.0F, 10.5F}, MONSTER_ZOMBIE, &s);
		assert(far);
		entity_minecart_collide_neighbours(&s, cart);
		check(cart->vel[0] == 0.0F && cart->vel[2] == 0.0F,
			  "(D) a mob outside the 0.2 sweep does not push");
		clear_kind(ENTITY_MONSTER);
	}

	/* (E) production wiring: the server tick runs the sweep. Two parked
	 * overlapping carts end the tick moving apart. */
	{
		clear_kind(ENTITY_MINECART);
		struct entity* a = cart_at(5.0F, 12.5F, 0.0F, 0.0F, 5.0F, 0);
		struct entity* b = cart_at(5.7F, 12.5F, 0.0F, 0.0F, 5.7F, 0);
		a->on_ground = b->on_ground = true;
		assert(server_local_test_tick_entities(&s));
		check(a->vel[0] < 0.0F && b->vel[0] > 0.0F,
			  "(E) the server cart tick pushes overlapping carts apart");
	}

	clear_kind(ENTITY_MINECART);
	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	printf("%s test_minecart_entity_collision_golden (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
