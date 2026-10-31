/* lane C #18 (2026-09-26): player-body -> minecart collision for BOTH actors.
 *
 * Beta 1.7.3 (decompile ../mcb173-retromcp/.../src_original):
 *   EntityLiving.onLivingUpdate [EntityLiving.java:643-651]: every living
 *     entity -- each player included, riding or not (updateRidden still runs
 *     onUpdate) -- queries its OWN world for entities intersecting
 *     boundingBox.expand(0.2F, 0, 0.2F) and calls var16.applyEntityCollision
 *     (this) on each canBePushed() one; EntityMinecart.canBePushed is true
 *     [EntityMinecart.java:53-55]. Nothing is host-only.
 *   EntityMinecart.applyEntityCollision [EntityMinecart.java:646-714]:
 *     skipped entirely for `var1 == this.riddenByEntity` (the rider never
 *     shoves its own cart; any OTHER body shoves an occupied cart). Else
 *     d = (dx, dz) / sqrt_double(d2)  (MathHelper.sqrt_double returns FLOAT)
 *         * min(1/dist, 1) * (double)0.1F * (1 - 0) * 0.5;
 *     cart.addVelocity(-d); player.addVelocity(+d/4).
 *   Entity.updateRidden [Entity.java]: a rider's motion is zeroed every tick
 *     and its position follows the mount, so a RIDING player's +d/4 never
 *     moves it: the counter-shove does not apply to a seated body.
 *   Order: host body first, then the guest (the server's damage-candidate
 *   order; each shove lands on the cart in turn).
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <m-lib/m-string.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_minecart.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#define YF 100
#define RY 101
#define RZ 8
#define CX 8.5F
#define CY ((float)RY + 0.5F)
#define CZ 8.5F

static struct server_local s;
static struct server_player guest;
static volatile int draining = 1;
static char dir0[128];

static void* drain_entry(void* arg) {
	(void)arg;
	while(draining) { clin_drain_discard(); thread_msleep(1); }
	clin_drain_discard();
	return NULL;
}

static void make_world(struct server_world* w, char* dir, size_t n,
					   const char* name, unsigned index) {
	test_fixture_dir(name, dir, n);
	MKDIR(dir);
	MKDIR(test_fixture_join(dir, "/region"));
	string_t level;
	string_init_set_str(level, dir);
	server_world_create(w, level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	w->session = &s;
	w->local = &s;
	w->world_index = index;
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(w, 0, 0, &sc) && sc);
	/* Flat east-west rail (meta 1) across the chunk on a stone floor. */
	for(int x = 0; x < 16; x++) {
		assert(server_world_set_block(w, x, YF, RZ,
			(struct block_data) {.type = BLOCK_STONE}));
		assert(server_world_set_block(w, x, RY, RZ,
			(struct block_data) {.type = BLOCK_RAIL, .metadata = 1}));
	}
}

static struct entity* make_cart(struct server_world* w) {
	struct entity* e = malloc(sizeof *e);
	assert(e);
	memset(e, 0xBE, sizeof *e);
	entity_minecart(4400, e, true, w);
	e->data.minecart.minecart_type = 0;
	e->teleport(e, (vec3) {CX, CY, CZ});
	e->data.minecart.speed = 0.0F;
	e->data.minecart.hx = 1;
	e->data.minecart.hz = 0;
	return e;
}

static void body_at(struct server_player* p, double x, double z) {
	p->x = x;
	p->y = RY + PLAYER_EYE_HEIGHT; /* feet on the rail plane */
	p->z = z;
	p->vel_x = p->vel_z = 0.0F;
}

static void far(struct server_player* p) { body_at(p, 5000.0, 5000.0); }

/* Beta's per-body shove for a player at (px,pz), cart at (CX,CZ). */
static void beta_d(double px, double pz, double* dx, double* dz) {
	double x = px - (double)CX, z = pz - (double)CZ;
	double d = (double)(float)sqrt(x * x + z * z);
	x /= d;
	z /= d;
	double f = 1.0 / d;
	if(f > 1.0)
		f = 1.0;
	x *= f;
	z *= f;
	x *= (double)0.1F;
	z *= (double)0.1F;
	x *= 1.0;
	z *= 1.0;
	*dx = x * 0.5;
	*dz = z * 0.5;
}

/* Tick a copy of `proto` and return the cart speed it ends with. */
static float tick_speed(const struct entity* proto) {
	struct entity e;
	memcpy(&e, proto, sizeof e);
	assert(!e.tick_server(&e, &s));
	return e.data.minecart.speed;
}

static float control_speed(const struct entity* proto) {
	double hx = s.player.x, hz = s.player.z, gx = guest.x, gz = guest.z;
	far(&s.player);
	far(&guest);
	float v = tick_speed(proto);
	body_at(&s.player, hx, hz);
	body_at(&guest, gx, gz);
	return v;
}

static void test_each_body_alone(bool use_guest) {
	struct server_player* me = use_guest ? &guest : &s.player;
	struct server_player* other = use_guest ? &s.player : &guest;
	const char* who = use_guest ? "guest" : "host";
	struct entity* cart = make_cart(&s.worlds[0]);
	far(other);
	body_at(me, CX - 0.5, CZ + 0.1);
	float base = control_speed(cart);
	float v = tick_speed(cart);
	assert(v > base && "the body on the -X side shoves the cart +X");
	double dx, dz;
	beta_d(me->x, me->z, &dx, &dz);
	assert(me->vel_x == (float)(dx / 4.0) && me->vel_z == (float)(dz / 4.0)
		   && "the quarter counter-shove is Beta's exact +d/4");
	assert(other->vel_x == 0.0F && other->vel_z == 0.0F);
	free(cart);
	printf("  %s alone: cart shoved away, exact +d/4 counter-shove PASS\n",
		   who);
}

/* O21 (lead ruling, Beta-first): the shove is Beta's cart.addVelocity(-d)
 * [CFR EntityMinecart.java:612, applyEntityCollision non-cart branch], and the
 * cart's next onUpdate projects its motion onto the rail axis KEEPING the full
 * horizontal magnitude, signed by the dot product with the MATRIX axis (+X
 * for shape 1) [:255-264]. A shove perpendicular to the rail therefore still
 * starts a parked cart (dot = 0 -> +X), and an oblique one starts it with |d|,
 * not d.h. Then the flat move (no clamp below 0.4) and unridden friction
 * (double)0.96F [:345-347]. Exact, in the engine's float arithmetic. */
static void full_magnitude_case(double bx, double bz, const char* label) {
	struct entity* cart = make_cart(&s.worlds[0]);
	far(&guest);
	body_at(&s.player, bx, bz);
	double dx, dz;
	beta_d(bx, bz, &dx, &dz);
	const float mx = 0.0F + (float)(-dx), mz = 0.0F + (float)(-dz);
	const float mag = (float)sqrt((double)mx * mx + (double)mz * mz);
	const float sgn = mx * 1.0F + mz * 0.0F < 0.0F ? -1.0F : 1.0F;
	const float want_speed = sgn * mag * 0.9599999785423279f;
	struct entity e;
	memcpy(&e, cart, sizeof e);
	assert(!e.tick_server(&e, &s));
	const float motion_x = e.data.minecart.speed * (float)e.data.minecart.hx;
	printf("  %s: motion %.9f (h %d,%d) Beta %.9f; x %.6f Beta %.6f\n", label,
		   (double)motion_x, e.data.minecart.hx, e.data.minecart.hz,
		   (double)want_speed, (double)e.pos[0], (double)(CX + sgn * mag));
	assert(e.data.minecart.hz == 0 && motion_x == want_speed
		   && "Beta keeps the shove's full magnitude along the rail");
	assert(e.pos[0] == CX + sgn * mag && e.pos[2] == CZ);
	assert(e.vel[0] == 0.0F && e.vel[2] == 0.0F && "the shove was folded");
	assert(s.player.vel_x == (float)(dx / 4.0)
		   && s.player.vel_z == (float)(dz / 4.0));
	free(cart);
}
static void test_full_magnitude(void) {
	full_magnitude_case(CX, CZ + 0.5, "perpendicular shove on a parked cart");
	full_magnitude_case(CX - 0.3, CZ + 0.4, "oblique shove keeps |d|");
	full_magnitude_case(CX + 0.4, CZ - 0.2, "oblique shove from +X runs -X");
	puts("  O21 full-magnitude shove (Beta :612 + :255-264) PASS");
}

static void test_both_bodies(void) {
	/* Mirror-image bodies: the two shoves cancel on the cart and each body
	 * takes its own quarter, host first then guest. */
	struct entity* cart = make_cart(&s.worlds[0]);
	body_at(&s.player, CX - 0.5, CZ);
	body_at(&guest, CX + 0.5, CZ);
	float base = control_speed(cart);
	float v = tick_speed(cart);
	assert(v == base && "equal and opposite shoves cancel");
	double hdx, hdz, gdx, gdz;
	beta_d(s.player.x, s.player.z, &hdx, &hdz);
	beta_d(guest.x, guest.z, &gdx, &gdz);
	assert(s.player.vel_x == (float)(hdx / 4.0) && s.player.vel_x < 0.0F);
	assert(guest.vel_x == (float)(gdx / 4.0) && guest.vel_x > 0.0F);
	free(cart);
	puts("  both bodies: two shoves, two counter-shoves PASS");
}

static void test_mounted(void) {
	/* The host rides THIS cart: its own body never shoves it (riddenBy),
	 * but the guest still does -- an occupied cart is not immune. */
	struct entity* cart = make_cart(&s.worlds[0]);
	cart->data.minecart.occupied = true;
	cart->data.minecart.occupant_id = 0; /* SERVER_LOCAL_PLAYER_ENTITY_ID */
	s.player.riding_minecart = true;
	body_at(&s.player, CX - 0.3, CZ);
	far(&guest);
	float base = control_speed(cart);
	float v = tick_speed(cart);
	assert(v == base && "the rider does not shove its cart");
	assert(s.player.vel_x == 0.0F);
	body_at(&guest, CX + 0.5, CZ);
	v = tick_speed(cart);
	assert(v < base && "the guest shoves the ridden cart -X");
	double dx, dz;
	beta_d(guest.x, guest.z, &dx, &dz);
	assert(guest.vel_x == (float)(dx / 4.0));
	free(cart);

	/* The host rides ANOTHER cart and brushes this empty one: this cart is
	 * shoved, the seated host takes no counter-shove (updateRidden). */
	cart = make_cart(&s.worlds[0]);
	body_at(&s.player, CX - 0.5, CZ);
	far(&guest);
	base = control_speed(cart);
	v = tick_speed(cart);
	assert(v > base && "a seated body still shoves a cart");
	assert(s.player.vel_x == 0.0F && s.player.vel_z == 0.0F);
	s.player.riding_minecart = false;
	free(cart);
	puts("  mounted: rider exempt from its cart, guest shoves it; seated "
		 "host shoves another cart without counter-shove PASS");
}

/* Other-world exclusion: the cart tick's M1c boundary assert pins every
 * server cart to worlds[0] (entity_minecart.c minecart_server_tick), and
 * server_local_world_of answers worlds[0] for both admitted bodies, so a
 * cart in another context cannot tick today. The production sweep still
 * gates each body on server_local_world_of(body) == the cart's world (the A0
 * actor->world contract) so the rule holds once dimensions split; what IS
 * reachable is the no-live-world answer for a non-admitted body, pinned here:
 * a guest pointer that is not the bound guest_actor never shoves. */
static void test_unadmitted_body(void) {
	struct entity* cart = make_cart(&s.worlds[0]);
	far(&s.player);
	body_at(&guest, CX - 0.5, CZ);
	s.guest_gameplay = false; /* damage_candidate(1) == NULL */
	float base = control_speed(cart);
	float v = tick_speed(cart);
	assert(v == base && guest.vel_x == 0.0F);
	s.guest_gameplay = true;
	free(cart);
	puts("  unadmitted guest: no shove PASS");
}

/* F1 (Fable C14-C18 review): the vertical gate is Beta's box test -- cart
 * box [posY - 0.35F, posY + 0.35F] (setSize(0.98F, 0.7F), yOffset = 0.35F),
 * player box [posY - 1.62F, +1.8F]. */
static void test_vertical_box(void) {
	struct entity* cart = make_cart(&s.worlds[0]);
	far(&guest);
	/* Feet 0.5 above the cart centre: inside the old [pos, pos+0.7] gate,
	 * outside Beta's box (feet 0.15 above its top) -> no shove. */
	body_at(&s.player, CX - 0.5, CZ);
	s.player.y = (double)CY + 0.5 + PLAYER_EYE_HEIGHT;
	float base = control_speed(cart);
	s.player.y = (double)CY + 0.5 + PLAYER_EYE_HEIGHT;
	float v = tick_speed(cart);
	assert(v == base && s.player.vel_x == 0.0F
		   && "a body standing above the cart box does not shove it");
	/* Head 0.1 below the cart centre (feet at pos - 1.9): below the old gate,
	 * inside Beta's box (head 0.25 above its bottom) -> shoved. */
	body_at(&s.player, CX - 0.5, CZ);
	s.player.y = (double)CY - 1.9 + PLAYER_EYE_HEIGHT;
	base = control_speed(cart);
	s.player.y = (double)CY - 1.9 + PLAYER_EYE_HEIGHT;
	v = tick_speed(cart);
	assert(v > base && s.player.vel_x < 0.0F
		   && "a body reaching into the cart box from below shoves it");
	free(cart);
	puts("  vertical box: Beta's centred cart box gates the shove PASS");
}

/* F2: the rider is whoever occupant_id names -- a cart ridden by the guest
 * (occupant = guest entity id) is shoved by the host, not by the guest. */
static void test_rider_identity(void) {
	struct entity* cart = make_cart(&s.worlds[0]);
	cart->data.minecart.occupied = true;
	cart->data.minecart.occupant_id = s.guest_entity_id;
	body_at(&guest, CX + 0.3, CZ);
	far(&s.player);
	float base = control_speed(cart);
	body_at(&guest, CX + 0.3, CZ);
	float v = tick_speed(cart);
	assert(v == base && "the rider (by occupant id) does not shove its cart");
	body_at(&s.player, CX - 0.5, CZ);
	v = tick_speed(cart);
	assert(v > base && "the non-rider host shoves the guest-ridden cart");
	free(cart);
	puts("  rider identity: occupant_id decides who is exempt PASS");
}

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	memset(&s, 0, sizeof s);
	dict_entity_init(s.entities);
	make_world(&s.worlds[0], dir0, sizeof dir0,
			   "test_minecart_body_collision_tmp", 0);
	s.scheduled_tick_budget = 0;
	s.entity_rng.seed = 0x2468ACE1u;
	s.player.has_pos = guest.has_pos = true;
	s.player.finished_loading = guest.finished_loading = true;
	s.player.health = guest.health = MAX_PLAYER_HEALTH;
	s.guest_actor = &guest;
	s.guest_gameplay = true;
	s.guest_entity_id = 77;
	assert(server_local_world_of(&s, &s.player) == &s.worlds[0]);
	assert(server_local_world_of(&s, &guest) == &s.worlds[0]);
	test_each_body_alone(false);
	test_each_body_alone(true);
	test_both_bodies();
	test_full_magnitude();
	test_mounted();
	test_unadmitted_body();
	test_vertical_box();
	test_rider_identity();
	draining = 0;
	thread_join(&drain);
	puts("test_minecart_body_collision: PASS");
	return 0;
}
