/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924/A1d, P2 inventory row #6: minecart mount /
 * dismount and rider control, owner-swapped.
 *
 * Beta citations (client Jad decompile,
 * ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   - EntityMinecart.interact, EntityMinecart.java:640-668: a type-0 cart
 *     mounts the interacting player (mountEntity) unless it is ridden
 *     by SOMEONE ELSE (a non-null rider that is an
 *     EntityPlayer other than the interacting one -> true, no-op); types 1/2
 *     open/fuel instead (:655-666).
 *   - Entity.mountEntity, Entity.java:941-966: toggle-off when already riding
 *     this entity (:950-953); previous mount released (:961);
 *     ridingEntity becomes the new mount (:964) -- the rider is the caller.
 *   - EntityMinecart.onUpdate, EntityMinecart.java:303-345: while ridden the
 *     RIDER's motion feeds the cart (`riddenByEntity.motionX * 0.1D`, the
 *     0.75D * 0.75D sign product) -- control belongs to whoever rides.
 *
 * CaveWii transport: mount/dismount is SRPC_MINECART_RIDE_INTENT (idempotent
 * desired state, F3) and rider throttle is SRPC_MINECART_CONTROL (cumulative
 * step counters on the cart's ride epoch). Neither verb is in the slot-1 class
 * of svin_rpc_send_from, and server_local_process_owned dispatches neither for
 * the guest (C2inv row #6: "no guest dispatch"), so on the base the queue
 * itself refuses the guest's intent -- no cart mounts, no throttle lands.
 *
 * Two ctest names, one executable (argv[1]):
 *   base  -- host reference: intent mounts, control tuple lands as throttle,
 *            intent desired=false dismounts.
 *   red   -- the same at the guest (queue admission, mount with the guest
 *            body flagged riding, throttle) plus the other-rider refusal.
 *            WILL_FAIL until wired. */
#include "support/owner_swap_harness.h"

#define TEST_NAME "test_owner_swap_minecart_mount_control"

static uint32_t ride_seq;
static struct server_rpc ride_intent(const struct server_local* s, uint32_t id,
	bool desired) {
	return (struct server_rpc) {.type = SRPC_MINECART_RIDE_INTENT,
		.payload.minecart_ride_intent = {.entity_id = id,
			.input_epoch = s->input_epoch_counter, .request_seq = ++ride_seq,
			.desired_occupied = desired}};
}
static struct server_rpc control_tuple(const struct entity* cart, uint32_t seq,
	uint32_t positive, uint32_t negative) {
	return (struct server_rpc) {.type = SRPC_MINECART_CONTROL,
		.payload.minecart_control = {.entity_id = cart->id,
			.ride_epoch = cart->data.minecart.ride_epoch, .input_seq = seq,
			.positive_steps = positive, .negative_steps = negative}};
}
/* A rideable (type 0) cart beside a body: one block +x, at the feet (y 4). */
static struct entity* cart_beside(struct server_local* s, struct server_player* p) {
	struct entity* cart = server_local_spawn_minecart(
		(vec3) {(float)p->x + 1.0F, 4.0F, (float)p->z}, s);
	assert(cart && cart->type == ENTITY_MINECART
		&& cart->data.minecart.minecart_type == 0);
	clin_update_drain();
	return cart;
}
static bool occupied_by_host(const struct entity* cart) {
	return cart->data.minecart.occupied
		&& cart->data.minecart.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID;
}
static bool empty_cart(const struct entity* cart) {
	return !cart->data.minecart.occupied && cart->data.minecart.occupant_id == 0;
}
/* The MINECART_CONTROL gate refuses throttle while the server is paused; the
 * fixture keeps the server paused (transport only), so the tuple is delivered
 * inside an unpaused window with no tick in between. */
static void host_control(struct server_local* s, struct server_rpc* call) {
	s->paused = false;
	host_send(s, call);
	s->paused = true;
}
static bool guest_control(struct server_local* s, struct server_rpc* call) {
	s->paused = false;
	bool queued = guest_send(s, call);
	s->paused = true;
	return queued;
}

static struct server_local* siding(void) {
	struct server_local* s = admitted_fixture();
	body_stand_at(s->guest_actor, 6.5, 0.5);
	return s;
}

/* ======================================================================== */
static void case_base(void) {
	struct server_local* s = siding();
	struct server_player* h = &s->player;
	struct entity* cart = cart_beside(s, h);

	struct server_rpc mount = ride_intent(s, cart->id, true);
	host_send(s, &mount);
	check(occupied_by_host(cart) && h->riding_minecart
		&& cart->data.minecart.ride_epoch != 0,
		"host: RIDE_INTENT desired=true mounts the cart on a fresh ride epoch "
		"(EntityMinecart.interact -> Entity.mountEntity:964)");

	cart->data.minecart.pending_throttle = 0;
	struct server_rpc forward = control_tuple(cart, 1, 1, 0);
	host_control(s, &forward);
	check(cart->data.minecart.pending_throttle == 1,
		"host: one forward control step lands as +1 pending throttle "
		"(EntityMinecart.onUpdate:303-345, the rider's motion drives the cart)");

	struct server_rpc dismount = ride_intent(s, cart->id, false);
	host_send(s, &dismount);
	check(empty_cart(cart) && !h->riding_minecart,
		"host: RIDE_INTENT desired=false dismounts (Entity.mountEntity:950-953)");
	teardown(s);
}

/* ======================================================================== */
static void case_red(void) {
	struct server_local* s = siding();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct entity* host_cart = cart_beside(s, h);
	struct entity* guest_cart = cart_beside(s, g);

	struct server_rpc hmount = ride_intent(s, host_cart->id, true);
	host_send(s, &hmount);
	check(occupied_by_host(host_cart) && h->riding_minecart,
		"setup: host riding its own cart");

	/* --- the guest mounts ITS cart -------------------------------------- */
	struct server_rpc gmount = ride_intent(s, guest_cart->id, true);
	check(guest_send(s, &gmount),
		"queue: the guest's RIDE_INTENT is admitted by svin_rpc_send_from "
		"[row #6: RED on base -- not in the slot-1 verb class]");
	check(guest_cart->data.minecart.occupied && g->riding_minecart
		&& guest_cart->data.minecart.ride_epoch != 0,
		"guest: RIDE_INTENT desired=true mounts ITS cart with the guest body "
		"flagged riding (EntityMinecart.interact for the interacting player)"
		"  [row #6: RED on base]");
	check(occupied_by_host(host_cart) && h->riding_minecart,
		"host: its own ride is untouched by the guest's intent");
	check(!guest_cart->data.minecart.occupied
		|| guest_cart->data.minecart.occupant_id != SERVER_LOCAL_PLAYER_ENTITY_ID
		|| !occupied_by_host(host_cart),
		"guest: its cart is never recorded as the HOST's (one rider per body)");

	/* --- throttle from the GUEST ------------------------------------------ */
	guest_cart->data.minecart.pending_throttle = 0;
	struct server_rpc gforward = control_tuple(guest_cart, 1, 1, 0);
	check(guest_control(s, &gforward),
		"queue: the guest's MINECART_CONTROL is admitted  [row #6: RED on base]");
	check(guest_cart->data.minecart.pending_throttle == 1,
		"guest: one forward control step lands as +1 pending throttle on ITS "
		"cart  [row #6: RED on base]");
	check(host_cart->data.minecart.pending_throttle == 0,
		"host: the guest's throttle never lands on the host's cart");

	/* --- other-rider refusal both ways ----------------------------------- */
	struct server_rpc steal = ride_intent(s, host_cart->id, true);
	(void)guest_send(s, &steal);
	check(occupied_by_host(host_cart) && h->riding_minecart,
		"guest: an intent on the cart the HOST rides is a no-op "
		"(EntityMinecart.interact, riddenByEntity != var1)");
	if(guest_cart->data.minecart.occupied) {
		struct server_rpc hsteal = ride_intent(s, guest_cart->id, true);
		host_send(s, &hsteal);
		check(guest_cart->data.minecart.occupied && g->riding_minecart
			&& occupied_by_host(host_cart),
			"host: an intent on the cart the GUEST rides is a no-op the same way");
	}

	/* --- dismount --------------------------------------------------------- */
	struct server_rpc gdismount = ride_intent(s, guest_cart->id, false);
	(void)guest_send(s, &gdismount);
	check(empty_cart(guest_cart) && !g->riding_minecart,
		"guest: desired=false leaves ITS cart empty and the body not riding");
	struct server_rpc hdismount = ride_intent(s, host_cart->id, false);
	host_send(s, &hdismount);
	check(empty_cart(host_cart) && !h->riding_minecart, "host: dismounted for teardown");
	teardown(s);
}

/* ======================================================================== */
int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : NULL;
	if(!which || (strcmp(which, "base") && strcmp(which, "red"))) {
		fprintf(stderr, "usage: %s base|red  (one case per process)\n", argv[0]);
		return 2;
	}
	harness_init();
	if(!strcmp(which, "base"))
		case_base();
	else
		case_red();
	return harness_result(TEST_NAME, which);
}
