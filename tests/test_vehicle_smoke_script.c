/* host_vehicle_smoke_8m producer + driver (vehicle-smoke-1004, CONTRACT-v1).
 *
 * One case per process (argv[1]); RED checks print ok/FAIL and exit 1, setup
 * asserts may abort. The server cases drive the REAL production handlers
 * through the shared owner-swap harness (real admission, real host/guest
 * mailboxes, real server_local_update via tick()), with the probe armed; they
 * assert on the records the owner threads actually pushed and on the
 * simulation state, never on a fabricated observation.
 *
 * WHAT THIS TEST DOES NOT PROVE, stated rather than implied:
 *   - main.c's seams (the owned-frame substitution, autopilot look/button
 *     application, the guest action pass) -- nothing here compiles main.c;
 *   - the full p0..p5 driver against a live client/renderer: the driver
 *     cases exercise its arming, gate, failure, serializer and finish paths,
 *     not a full run (that is the PC-app prototype + real-Wii job);
 *   - real-Wii timing, the observer-transport drain rate, or the collector.
 * The fixture geometry is checked against the real interaction gate here
 * (rig_ready), which is the PC half of Astra ruling 4's geometry gate. */
#include "support/owner_swap_harness.h"
#include "entity/entity_boat.h"
#include "entity/entity_local_player.h" /* O04 D1 ride-action hook */
#include "entity/entity_minecart.h"
#include "entity/entity_monster.h"
#include "game/hw_run.h"
#include "game/vehicle_smoke_script.h"
#include "network/server_transfer.h"
#include "network/vehicle_smoke_probe.h"
#include "worldgen/beta_math.h"

/* ---- record capture ------------------------------------------------------ */
#define CAP_MAX 512
static struct vsp_record cap[CAP_MAX];
static unsigned cap_n;
static uint32_t cap_seq;
static unsigned cap_serialize_errors;

/* Pop every queued record; each must also SERIALIZE (the wire is the
 * contract). */
static void capture(void) {
	struct vsp_record r;
	static char line[VSP_LINE_CAP];
	while(vsp_pop(&r)) {
		const char* err = NULL;
		size_t n = vsp_serialize(&r, ++cap_seq, 12 + 8 + 1, line, sizeof(line), &err);
		if(!n) {
			printf("serialize error ev=%u: %s\n", (unsigned)r.event, err ? err : "?");
			cap_serialize_errors++;
		}
		if(cap_n < CAP_MAX)
			cap[cap_n++] = r;
	}
}

static int find_ev(enum vsp_event ev, unsigned from) {
	for(unsigned i = from; i < cap_n; i++)
		if(cap[i].event == ev)
			return (int)i;
	return -1;
}

static unsigned count_ev(enum vsp_event ev) {
	unsigned n = 0;
	for(unsigned i = 0; i < cap_n; i++)
		n += cap[i].event == ev;
	return n;
}

/* The last authority action_result for an op. */
static const struct vsp_record* auth_result(uint32_t op) {
	const struct vsp_record* hit = NULL;
	for(unsigned i = 0; i < cap_n; i++)
		if(cap[i].event == VSP_EV_ACTION_RESULT && cap[i].op_id == op
		   && cap[i].u.result.site == VSP_SITE_AUTHORITY)
			hit = &cap[i];
	return hit;
}

static void arm(enum vsp_kind kind) {
	vsp_arm(&(struct vsp_arm_cfg) {.kind = kind,
		.transfer_fixture_request = true, .build_id = "test_build"});
	cap_n = 0;
	cap_seq = 0;
	cap_serialize_errors = 0;
}

/* Load every column the fixture and its entity-tick area need (the entity
 * pass freezes an entity whose +-32 block box is not fully resident). */
static void load_area(struct server_local* s) {
	for(int cx = -3; cx <= 3; cx++)
		for(int cz = -3; cz <= 3; cz++) {
			struct server_chunk* c = dict_server_chunks_get(s->worlds[0].chunks,
															S_CHUNK_ID(cx, cz));
			if(!c)
				(void)split_populate(s, cx, cz);
		}
}

/* Running updates that leave the global pause flag to the real handlers. */
static void ticks_running(struct server_local* s, unsigned n) {
	for(unsigned i = 0; i < n; i++) {
		tick(s);
		client_acks_marker(s);
	}
}

static void ticks(struct server_local* s, unsigned n) {
	s->paused = false;
	for(unsigned i = 0; i < n; i++) {
		tick(s);
		client_acks_marker(s);
	}
	s->paused = true;
}

static struct server_local* rig(enum vsp_kind kind) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	load_area(s);
	arm(kind);
	return s;
}

/* The real fixture command, consumed by a real server update. */
static bool build_fixture(struct server_local* s, uint32_t op) {
	vsp_set_op(op);
	vsp_cmd_fixture(op);
	ticks(s, 1);
	return vsp_fixture_targets(NULL, NULL);
}

static uint32_t target(enum vsp_kind k) {
	struct vsp_r t[3];
	assert(vsp_fixture_targets(t, NULL));
	return t[k].id;
}

static struct entity* ent(struct server_local* s, uint32_t id) {
	struct entity** pp = dict_entity_get(s->entities, id);
	return pp ? *pp : NULL;
}

/* Host interactions through the host's own production mailbox. */
static void host_use(struct server_local* s, uint32_t op, uint32_t id, bool desired) {
	vsp_set_op(op);
	struct server_rpc call = {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = id, .desired_occupied = desired}};
	host_send(s, &call);
}
static void host_attack(struct server_local* s, uint32_t op, uint32_t id) {
	vsp_set_op(op);
	struct server_rpc call = {.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = id};
	host_send(s, &call);
}
static uint32_t ride_seq;
static void host_ride(struct server_local* s, uint32_t op, uint32_t id, bool desired) {
	vsp_set_op(op);
	struct server_rpc call = {.type = SRPC_MINECART_RIDE_INTENT,
		.payload.minecart_ride_intent = {.entity_id = id,
			.input_epoch = s->input_epoch_counter, .request_seq = ++ride_seq,
			.desired_occupied = desired}};
	host_send(s, &call);
}
static void host_mount(struct server_local* s, uint32_t op, enum vsp_kind k) {
	if(k == VSP_CART)
		host_ride(s, op, target(k), true);
	else
		host_use(s, op, target(k), true);
}
static void host_dismount(struct server_local* s, uint32_t op, enum vsp_kind k) {
	if(k == VSP_CART)
		host_ride(s, op, target(k), false);
	else
		host_use(s, op, target(k), false);
}

/* Fixture + real saddle-by-hit + pig AI ticks, then rig_ready. */
static bool rig_ready(struct server_local* s) {
	if(!build_fixture(s, 1))
		return false;
	host_attack(s, 2, target(VSP_PIG));
	ticks(s, 3);
	capture();
	return find_ev(VSP_EV_RIG_READY, 0) >= 0;
}

static bool host_on(struct server_local* s, enum vsp_kind k) {
	struct entity* e = ent(s, target(k));
	if(!e)
		return false;
	if(k == VSP_BOAT)
		return e->data.boat.occupied && actor_ident_is_host(e->data.boat.rider_actor)
			&& s->player.riding_boat;
	if(k == VSP_PIG)
		return e->data.monster.pig_ridden
			&& actor_ident_is_host(e->data.monster.pig_rider_actor);
	return e->data.minecart.occupied && s->player.riding_minecart;
}


/* ---- cases --------------------------------------------------------------- */

/* CONTRACT-v1 wire rules on the actual serializer. */
static void case_serialize(void) {
	arm(VSP_PIG);
	char line[VSP_LINE_CAP];
	const char* err = NULL;
	struct vsp_record r;
	memset(&r, 0, sizeof(r));
	r.event = VSP_EV_PHASE_BEGIN;
	r.ok = true;
	size_t n = vsp_serialize(&r, 1, 21, line, sizeof(line), &err);
	check(n > 0, "serialize: a phase_begin line serializes");
	check(strstr(line, "\"schema\":1") && strstr(line, "\"profile\":\"host_vehicle_smoke_8m\"")
		&& strstr(line, "\"kind\":\"pig\"") && strstr(line, "\"seq\":1")
		&& strstr(line, "\"event\":\"vehicle_smoke.phase_begin\"")
		&& strstr(line, "\"ok\":true") && strstr(line, "\"origin\":\"production\"")
		&& !strstr(line, "run_id"),
		"serialize: common envelope present, run_id left for hw_telemetry to append");
	check(line[0] == '{' && line[n - 1] == '}', "serialize: one JSON object");
	/* Fail-closed: zero seq, non-finite float, absent world, oversize. */
	check(!vsp_serialize(&r, 0, 21, line, sizeof(line), &err), "serialize: seq 0 refused");
	struct vsp_record d;
	memset(&d, 0, sizeof(d));
	d.event = VSP_EV_DISMOUNT;
	d.ok = true;
	d.u.dismount.host.world = (struct vsp_w) {.slot = 0, .generation = 1};
	d.u.dismount.old.target = (struct vsp_r) {.id = 5, .kind = VSP_BOAT,
		.world = {.slot = 0, .generation = 1}};
	d.u.dismount.serial = 7;
	check(vsp_serialize(&d, 2, 21, line, sizeof(line), &err) > 0
		&& strstr(line, "\"serial\":\"7\""), "serialize: G64 is a decimal JSON string");
	d.u.dismount.host.pos[1] = NAN;
	check(!vsp_serialize(&d, 2, 21, line, sizeof(line), &err) && err
		&& strcmp(err, "nonfinite") == 0, "serialize: non-finite float FAILS");
	d.u.dismount.host.pos[1] = 0;
	d.u.dismount.host.world.generation = 0;
	check(!vsp_serialize(&d, 2, 21, line, sizeof(line), &err) && err
		&& strcmp(err, "invalid_world") == 0,
		"serialize: an absent world is never a fake generation 0");
	d.u.dismount.host.world.generation = 1;
	check(!vsp_serialize(&d, 2, 2300, line, sizeof(line), &err) && err
		&& strcmp(err, "oversize") == 0,
		"serialize: line + run_id reserve over 2303 bytes FAILS (no truncation)");
	/* Kind-specific client vehicle field sets. */
	struct vsp_record c;
	memset(&c, 0, sizeof(c));
	c.event = VSP_EV_ACTION_RESULT;
	c.ok = true;
	c.u.result.site = VSP_SITE_CLIENT;
	c.u.result.target = (struct vsp_r) {.id = 9, .kind = VSP_PIG,
		.world = {.slot = 0, .generation = 3}};
	c.u.result.u.client.vehicle.target = c.u.result.target;
	c.u.result.u.client.host.present = true;
	c.u.result.u.client.host.bound = (struct vsp_w) {.slot = 0, .generation = 3};
	check(vsp_serialize(&c, 3, 21, line, sizeof(line), &err) > 0
		&& strstr(line, "\"pig_ridden\"") && !strstr(line, "\"cart_type\"")
		&& !strstr(line, "\"occupant_id\""),
		"serialize: pig VC carries exactly the pig field set");
	c.u.result.u.client.vehicle.target.kind = VSP_CART;
	check(vsp_serialize(&c, 3, 21, line, sizeof(line), &err) > 0
		&& strstr(line, "\"cart_type\"") && strstr(line, "\"replay_throttle\"")
		&& !strstr(line, "\"pig_ridden\""),
		"serialize: cart VC carries exactly the cart field set");
	c.u.result.u.client.host.present = false;
	c.u.result.u.client.host.c0_generation = 4;
	check(vsp_serialize(&c, 3, 21, line, sizeof(line), &err) > 0
		&& strstr(line, "\"present\":false") && strstr(line, "\"pending_ticket\""),
		"serialize: C0 is the distinct absent-guest schema");
	struct vsp_record f;
	memset(&f, 0, sizeof(f));
	f.event = VSP_EV_FAIL;
	snprintf(f.u.fail.code, sizeof(f.u.fail.code), "bad code!");
	check(!vsp_serialize(&f, 4, 21, line, sizeof(line), &err),
		"serialize: a fail code outside [A-Za-z0-9_.:-] is refused");
	vsp_reset();
}

/* Bounded queues: no overwrite, overflow latches. */
static void case_fifo(void) {
	arm(VSP_BOAT);
	struct vsp_record r;
	memset(&r, 0, sizeof(r));
	r.event = VSP_EV_TITLE;
	unsigned pushed = 0;
	for(unsigned i = 0; i < VSP_MAIN_FIFO + 3; i++)
		pushed += vsp_main_push(&r);
	check(pushed == VSP_MAIN_FIFO && vsp_overflow_count() == 3,
		"fifo: main FIFO admits 8, refuses (never overwrites) the rest, latches overflow");
	check(vsp_pending_records() == VSP_MAIN_FIFO, "fifo: retained records are all still queued");
	unsigned popped = 0;
	while(vsp_pop(&r))
		popped++;
	check(popped == VSP_MAIN_FIFO && vsp_overflow_count() == 3,
		"fifo: draining does not clear the terminal overflow latch");
	vsp_reset();
}

/* p0: the real fixture, real saddle-by-hit, real interaction gate. */
static void case_fixture(void) {
	struct server_local* s = rig(VSP_BOAT);
	check(build_fixture(s, 1), "fixture: one real server update builds and publishes targets");
	struct vsp_r t[3];
	int32_t a[3];
	assert(vsp_fixture_targets(t, a));
	struct entity* boat = ent(s, t[VSP_BOAT].id);
	struct entity* pig = ent(s, t[VSP_PIG].id);
	struct entity* cart = ent(s, t[VSP_CART].id);
	check(boat && boat->type == ENTITY_BOAT && pig && pig->type == ENTITY_MONSTER
		&& pig->data.monster.id == MONSTER_PIG && cart && cart->type == ENTITY_MINECART
		&& cart->data.minecart.minecart_type == 0,
		"fixture: boat, pig and type-0 cart spawned through the real spawn helpers");
	uint8_t water = block_at(s, a[0] - 2, a[1], a[2]);
	check((water == BLOCK_WATER_STILL || water == BLOCK_WATER_FLOW)
		&& block_at(s, a[0] + 1, a[1] + 1, a[2]) == BLOCK_FENCE
		/* O09 (CONTRACT-v1.3): two-ended stopped east-west cart track */
		&& block_at(s, a[0] - 2, a[1] + 1, a[2] + 2) == BLOCK_STONE
		&& block_at(s, a[0] - 1, a[1] + 1, a[2] + 2) == BLOCK_RAIL
		&& block_at(s, a[0], a[1] + 1, a[2] + 2) == BLOCK_RAIL
		&& block_at(s, a[0] + 1, a[1] + 1, a[2] + 2) == BLOCK_FENCE
		&& block_at(s, a[0], a[1] + 1, a[2] + 1) == BLOCK_AIR
		&& block_at(s, a[0], a[1] + 1, a[2] + 3) == BLOCK_AIR
		&& block_at(s, a[0], a[1] + 1, a[2]) == BLOCK_AIR,
		"fixture: basin, rails, pen and end stop placed; host column untouched");
	struct item_data held;
	(void)inventory_get_hotbar_item(&s->player.inventory, &held);
	check(held.id == ITEM_SADDLE
		&& s->player.inventory.items[INVENTORY_SLOT_MAIN].id == BLOCK_COBBLESTONE
		&& s->guest_actor->inventory.items[INVENTORY_SLOT_MAIN].id == BLOCK_PLANKS,
		"fixture: disclosed held saddle + distinct non-empty stacks seeded");
	capture();
	check(find_ev(VSP_EV_RIG_READY, 0) < 0,
		"fixture: no rig_ready before the saddle-by-hit is observed");
	host_attack(s, 2, t[VSP_PIG].id);
	ticks(s, 3);
	capture();
	const struct vsp_record* sad = auth_result(2);
	check(sad && sad->u.result.action == VSP_ACT_SADDLE
		&& sad->u.result.status == VSP_ST_APPLIED,
		"fixture: the REAL saddle-by-hit handler applied (authority result)");
	int rr = find_ev(VSP_EV_RIG_READY, 0);
	check(rr >= 0, "fixture: rig_ready after the observed saddle and pig AI ticks");
	if(rr >= 0) {
		const struct vsp_p_rig_ready* p = &cap[rr].u.rig_ready;
		check(p->water_ok && p->support_ok && p->rails_ok,
			"fixture: basin/support/rails read back as planned");
		check(p->ray_ok, "fixture: the real func_6006_a gate passes host->each target");
		check(p->saddle_consumed && p->pig_ai_updates > 0,
			"fixture: saddle consumed, pig AI actually ticked (unfrozen)");
		check(cap[rr].origin == VSP_FIXTURE && p->fixture_hash == vsp_fixture_hash_now()
			&& p->host.inv_used > 0 && p->guest.inv_used > 0
			&& p->host.inv_hash != p->guest.inv_hash,
			"fixture: fixture origin, hash receipt, distinct populated inventories");
	}
	check(cap_serialize_errors == 0, "fixture: every captured record serializes");
	vsp_reset();
	teardown(s);
}

/* p1 RIDE: real mount + server ride window census. */
static void case_ride(enum vsp_kind k) {
	struct server_local* s = rig(k);
	assert(rig_ready(s));
	host_mount(s, 10, k);
	ticks(s, 1);
	capture();
	const struct vsp_record* m = auth_result(10);
	check(m && m->u.result.status == VSP_ST_APPLIED && m->u.result.action == VSP_ACT_MOUNT
		&& host_on(s, k), "ride: real host mount applied by the authority");
	vsp_ride_window(1, target(k), true);
	ticks(s, 30);
	vsp_ride_window(1, target(k), false);
	ticks(s, 2);
	capture();
	int i = -1;
	for(unsigned j = 0; j < cap_n; j++)
		if(cap[j].event == VSP_EV_RIDE_SAMPLE && cap[j].u.ride.window_id == 1)
			i = (int)j;
	check(i >= 0, "ride: the server window closes with a ride_sample");
	if(i >= 0) {
		const struct vsp_p_ride_sample* p = &cap[i].u.ride;
		printf("ride window: eligible=%u ticks=%u missing=%u dup=%u disp=%.3f\n",
			p->eligible_updates, p->tick_calls, p->missing_calls, p->duplicate_calls,
			p->displacement_m);
		check(p->eligible_updates >= 28 && p->tick_calls == p->eligible_updates
			&& p->missing_calls == 0 && p->duplicate_calls == 0,
			"ride: one real vehicle tick per eligible mounted update");
		check(p->u.server.host.mount_count == 1
			&& p->u.server.vehicle.occupied && p->u.server.vehicle.target.id == target(k),
			"ride: ServerMounted snapshot names the target and one relation");
	}
	vsp_reset();
	teardown(s);
}

/* p1 DISMOUNT/SWITCH: the run kind's directed sequence through real handlers. */
static void case_switch(enum vsp_kind k) {
	static const uint8_t seq[3][4] = {
		{VSP_BOAT, VSP_PIG, VSP_CART, VSP_BOAT},
		{VSP_PIG, VSP_BOAT, VSP_CART, VSP_PIG},
		{VSP_CART, VSP_BOAT, VSP_PIG, VSP_CART}};
	struct server_local* s = rig(k);
	assert(rig_ready(s));
	host_mount(s, 10, k);
	ticks(s, 1);
	host_dismount(s, 11, k);
	ticks(s, 1);
	capture();
	int dm = find_ev(VSP_EV_DISMOUNT, 0);
	const struct vsp_record* dr = auth_result(11);
	check(dm >= 0 && dr && dr->u.result.action == VSP_ACT_DISMOUNT
		&& dr->u.result.status == VSP_ST_APPLIED && !host_on(s, k)
		&& !s->player.riding_boat && !s->player.riding_minecart,
		"switch: explicit dismount released the relation (authority + record)");
	if(dm >= 0)
		check(cap[dm].u.dismount.host.mount_count == 0 && !cap[dm].u.dismount.old.occupied
			&& cap[dm].u.dismount.old.steer[0] == 0 && cap[dm].u.dismount.old.ride_epoch == 0,
			"switch: dismount snapshot is ServerDetached + Released");
	host_mount(s, 12, k);
	ticks(s, 1);
	unsigned base = cap_n;
	for(unsigned j = 1; j < 4; j++) {
		host_mount(s, 20 + j, (enum vsp_kind)seq[k][j]);
		ticks(s, 1);
	}
	capture();
	unsigned switches = 0;
	bool order_ok = true, kinds_ok = true, witness_ok = true;
	for(unsigned j = base; j < cap_n; j++) {
		if(cap[j].event != VSP_EV_SWITCH)
			continue;
		const struct vsp_p_switch* p = &cap[j].u.sw;
		order_ok = order_ok && p->release_serial != 0
			&& p->release_serial < p->mount_serial
			&& !p->old.occupied && p->nu.occupied && p->host.mount_count == 1;
		kinds_ok = kinds_ok && p->from == seq[k][switches] && p->to == seq[k][switches + 1];
		/* CONTRACT-v1.1 Q9: exactly one REAL release witness under the same
		 * op, emitted BEFORE the switch, carrying the switch's release
		 * serial, with the host already detached and the old vehicle
		 * released at that moment (i.e. before the new relation existed). */
		unsigned w = 0;
		for(unsigned i = base; i < cap_n; i++) {
			if(cap[i].event != VSP_EV_DISMOUNT || cap[i].op_id != cap[j].op_id)
				continue;
			w++;
			const struct vsp_p_dismount* d = &cap[i].u.dismount;
			witness_ok = witness_ok && i < j && d->serial == p->release_serial
				&& d->old.target.id == p->old.target.id
				&& d->host.mount_count == 0 && !d->host.riding_boat
				&& !d->host.riding_cart && !d->old.occupied
				&& d->host.steer[0] == 0 && d->host.steer[1] == 0;
		}
		witness_ok = witness_ok && w == 1;
		switches++;
	}
	check(switches == 3, "switch: three directed cross-kind switches observed");
	check(order_ok, "switch: release_serial < mount_serial, old released, new uniquely owned");
	check(witness_ok, "switch: one pre-mount release witness per switch (same op, serial == release_serial, detached)");
	check(kinds_ok, "switch: from/to follow the run kind's coverage row");
	check(host_on(s, k), "switch: the sequence returns to the run kind");
	check(cap_serialize_errors == 0, "switch: every record serializes");
	vsp_reset();
	teardown(s);
}

/* P2: guest negative probes refuse at the right site; isolation brackets. */
static void case_guest_probes(void) {
	struct server_local* s = rig(VSP_BOAT);
	assert(rig_ready(s));
	host_mount(s, 10, VSP_BOAT);
	ticks(s, 1);
	/* b85bf1103 (maintainer 10-08, ride R2) admits P2 on a SADDLED pig.
	 * Keep this negative isolation bracket: RetroMCP Beta EntityPig.java:36-41,
	 * unsaddled pig is refused, like the boat owned by the other actor. */
	ent(s, target(VSP_PIG))->data.monster.saddled = false;
	vsp_iso_bracket(30, true);
	ticks(s, 1);
	uint32_t op = 31;
	for(unsigned k = 0; k < 3; k++) {
		vsp_set_op(op);
		struct server_rpc use = {.type = SRPC_ENTITY_USE,
			.payload.entity_use = {.entity_id = target((enum vsp_kind)k),
				.desired_occupied = true}};
		(void)guest_send(s, &use);
		ticks(s, 1);
		capture();
		const struct vsp_record* r = auth_result(op);
		check(r && r->u.result.status == VSP_ST_REFUSED && r->u.result.source.slot == 1,
			k == VSP_BOAT ? "probes: guest boat ENTITY_USE refused at the authority"
			: k == VSP_PIG ? "probes: guest pig ENTITY_USE refused at the authority"
			: "probes: guest cart ENTITY_USE leaves authority unchanged");
		op++;
	}
	vsp_set_op(op);
	struct server_rpc intent = {.type = SRPC_MINECART_RIDE_INTENT,
		.payload.minecart_ride_intent = {.entity_id = target(VSP_CART),
			.input_epoch = s->input_epoch_counter, .request_seq = 1,
			.desired_occupied = true}};
	bool queued = svin_rpc_send_from(guest_source(s), &intent);
	struct server_rpc control = {.type = SRPC_MINECART_CONTROL,
		.payload.minecart_control = {.entity_id = target(VSP_CART), .ride_epoch = 1,
			.input_seq = 1, .positive_steps = 1}};
	vsp_set_op(op + 1);
	bool queued2 = svin_rpc_send_from(guest_source(s), &control);
	capture();
	unsigned ingress = 0;
	for(unsigned i = 0; i < cap_n; i++)
		if(cap[i].event == VSP_EV_ACTION_RESULT
		   && cap[i].u.result.site == VSP_SITE_INGRESS
		   && cap[i].u.result.status == VSP_ST_REFUSED)
			ingress++;
	check(!queued && !queued2 && ingress == 2,
		"probes: guest cart RIDE_INTENT/CONTROL refused at the closed slot-1 mailbox");
	ticks(s, 12);
	vsp_iso_bracket(30, false);
	ticks(s, 2);
	capture();
	int before = -1, after = -1;
	for(unsigned i = 0; i < cap_n; i++)
		if(cap[i].event == VSP_EV_ISOLATION && cap[i].u.iso.attempt_op == 30) {
			if(cap[i].u.iso.stage == VSP_STAGE_BEFORE) before = (int)i;
			else after = (int)i;
		}
	check(before >= 0 && after > before && vsp_iso_server_closed(30),
		"probes: server isolation bracket before/after recorded and acknowledged");
	if(after >= 0) {
		const struct vsp_p_isolation* p = &cap[after].u.iso;
		check(p->unexpected_attach_count == 0 && p->guest_follow_count == 0
			&& p->u.server.guest.mount_count == 0 && !p->u.server.guest.riding_boat
			&& !p->u.server.guest.riding_cart,
			"probes: no guest attach/follow, guest detached across the bracket");
		check(p->u.server.host.mount_count == 1 && p->u.server.vehicle.occupied,
			"probes: host relation unchanged by the guest attempts");
	}
	check(host_on(s, VSP_BOAT), "probes: host still rides the boat");
	check(cap_serialize_errors == 0, "probes: every ingress/authority record serializes");
	vsp_reset();
	teardown(s);
}

/* Q2: the client route witness counts a deliberate host-only misroute
 * through the REAL consumer, and stays sticky after a repair. */
static void case_misroute(void) {
	struct server_local* s = rig(VSP_CART);
	assert(rig_ready(s));
	clin_update_drain();
	struct entity* guest_body = gstate.players[1].local_player;
	assert(guest_body);
	uint32_t cart = target(VSP_CART);
	struct entity** cp = dict_entity_get(world_view_entities(), cart);
	check(cp && *cp, "misroute: the client holds the fixture cart");
	vsp_client_counters_window_reset(true, false, 0);
	/* Deliberate fault: a host-only occupancy RPC naming the GUEST body. */
	struct client_rpc bad = {.type = CRPC_MINECART_OCCUPANCY,
		.payload.minecart_occupancy = {.entity_id = cart, .occupied = true,
			.occupant_id = guest_body->id, .ride_epoch = 3}};
	clin_process(&bad);
	struct vsp_client_counters cc;
	vsp_client_counters(&cc);
	check(cc.unexpected_route_count >= 1 && cc.unexpected_attach_count >= 1
		&& cc.route_violation_sticky,
		"misroute: the witness counts the applied host-only RPC attaching the guest");
	/* Repair: the same route releasing it. The violation stays counted. */
	struct client_rpc repair = bad;
	repair.payload.minecart_occupancy.occupied = false;
	repair.payload.minecart_occupancy.occupant_id = 0;
	clin_process(&repair);
	vsp_client_counters(&cc);
	check(cc.unexpected_route_count >= 1 && cc.route_violation_sticky,
		"misroute: a transient misapplication followed by repair still FAILS");
	vsp_client_counters_window_reset(true, false, 0);
	vsp_client_counters(&cc);
	check(cc.route_violation_sticky, "misroute: the run-sticky latch survives bracket reset");
	vsp_reset();
	teardown(s);
}

/* p3: the disclosed lethal fixture through the production health entry. */
static void case_death(void) {
	struct server_local* s = rig(VSP_BOAT);
	assert(rig_ready(s));
	host_mount(s, 10, VSP_BOAT);
	ticks(s, 1);
	uint32_t deaths = s->player_deaths;
	vsp_set_op(40);
	vsp_cmd_lethal(40);
	ticks(s, 1);
	capture();
	int d = find_ev(VSP_EV_DEATH, 0);
	check(d >= 0 && s->player.health <= 0 && s->player_deaths == deaths + 1,
		"death: one lethal call through server_local_set_player_health commits a death");
	if(d >= 0) {
		const struct vsp_p_death* p = &cap[d].u.death;
		check(cap[d].origin == VSP_FIXTURE && strcmp(p->u.server.cause, "fixture_lethal") == 0
			&& p->u.server.deaths_after == p->u.server.deaths_before + 1
			&& p->u.server.before_host.health > 0 && p->u.server.before_host.mount_count == 1,
			"death: fixture origin/cause, mounted before, count +1");
	}
	check(vsp_take_cmd(VSP_CMD_LETHAL) == 0 && !(vsp_hook_mask() & VSP_HOOK_LETHAL),
		"death: the lethal hook is one-shot (consumed, bit cleared)");
	uint32_t death_epoch = s->input_epoch_counter;
	/* Real Respawn: the host's own SRPC_RESPAWN (the GUI request's RPC),
	 * then the readiness ACK -- all under the death's operation. */
	struct server_rpc respawn = {.type = SRPC_RESPAWN, .payload.respawn = {.owner = 0}};
	host_send(s, &respawn);
	ticks(s, 2);
	unsigned stale_acks = 0;
	if(s->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING) {
		w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->host_respawn_x));
		w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->host_respawn_z));
		struct server_chunk* col = dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(cx, cz));
		if(col)
			col->sent = true; /* backend readiness bit only (no mesher in this rig) */
		/* Q4: a STALE loading ACK (wrong epoch) is ignored by production
		 * and must produce no ACK observation. */
		struct server_rpc stale = {.type = SRPC_RESPAWN,
			.payload.respawn = {.owner = 0, .respawn_epoch = s->host_respawn_epoch + 5}};
		host_send(s, &stale);
		ticks(s, 1);
		capture();
		for(unsigned i = 0; i < cap_n; i++)
			stale_acks += cap[i].event == VSP_EV_RESPAWN
				&& cap[i].u.respawn.stage == VSP_RS_ACK;
		check(stale_acks == 0 && s->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING,
			"respawn: a stale-epoch loading ACK is rejected (no ack observed, still LOADING)");
		struct server_rpc ready = {.type = SRPC_RESPAWN,
			.payload.respawn = {.owner = 0, .respawn_epoch = s->host_respawn_epoch}};
		host_send(s, &ready);
		ticks(s, 2);
	}
	capture();
	const struct vsp_record* st[VSP_RS_CLIENT_READY] = {0};
	unsigned count[VSP_RS_CLIENT_READY + 1] = {0};
	unsigned at[VSP_RS_CLIENT_READY] = {0};
	for(unsigned i = 0; i < cap_n; i++)
		if(cap[i].event == VSP_EV_RESPAWN && cap[i].u.respawn.stage < VSP_RS_CLIENT_READY) {
			count[cap[i].u.respawn.stage]++;
			st[cap[i].u.respawn.stage] = &cap[i];
			at[cap[i].u.respawn.stage] = i;
		}
	bool all = st[VSP_RS_RELEASED] && st[VSP_RS_REQUESTED] && st[VSP_RS_ACK] && st[VSP_RS_FRESH];
	check(all && count[VSP_RS_RELEASED] == 1 && count[VSP_RS_REQUESTED] == 1
		&& count[VSP_RS_ACK] == 1 && count[VSP_RS_FRESH] == 1,
		"respawn: exactly one released/requested/ack/fresh, each at its real site");
	if(all) {
		const struct vsp_p_respawn* R = &st[VSP_RS_RELEASED]->u.respawn;
		const struct vsp_p_respawn* Q = &st[VSP_RS_REQUESTED]->u.respawn;
		const struct vsp_p_respawn* A = &st[VSP_RS_ACK]->u.respawn;
		const struct vsp_p_respawn* F = &st[VSP_RS_FRESH]->u.respawn;
		check(at[VSP_RS_RELEASED] < at[VSP_RS_REQUESTED] && at[VSP_RS_REQUESTED] < at[VSP_RS_ACK]
			&& at[VSP_RS_ACK] < at[VSP_RS_FRESH]
			&& R->u.server.serial < Q->u.server.serial && Q->u.server.serial < A->u.server.serial
			&& A->u.server.serial < F->u.server.serial,
			"respawn: released -> requested -> ack -> fresh, in emission AND serial order");
		check(R->respawn_epoch == 0 && Q->respawn_epoch != 0
			&& A->respawn_epoch == Q->respawn_epoch && F->respawn_epoch == Q->respawn_epoch,
			"respawn: one correlated transaction epoch (0 before it, then the same)");
		check(R->old_input_epoch == death_epoch && Q->old_input_epoch == death_epoch
			&& A->old_input_epoch == death_epoch && F->old_input_epoch == death_epoch
			&& R->new_input_epoch == death_epoch && A->new_input_epoch == death_epoch
			&& F->new_input_epoch == death_epoch,
			"respawn: input epochs are the host's own (unchanged by the split route)");
		check(A->u.server.ack && !R->u.server.ack && !Q->u.server.ack && !F->u.server.ack,
			"respawn: only the matching loading handshake is the ack");
		bool ops = true;
		for(unsigned k = 0; k < VSP_RS_CLIENT_READY; k++)
			ops = ops && (!st[k] || st[k]->op_id == 40);
		check(ops, "respawn: every stage carries the death's operation");
		check(F->u.server.health == MAX_PLAYER_HEALTH && F->u.server.host.mount_count == 0
			&& !F->u.server.old.occupied,
			"respawn: fresh body full health and detached");
	}
	check(s->player.health == MAX_PLAYER_HEALTH && !s->player.riding_boat
		&& !host_on(s, VSP_BOAT), "respawn: full health, no stale attachment");
	vsp_reset();
	teardown(s);
}

/* p4 PROGRESS: one real ACCEPT tuple retained by the seeded refusal through a
 * measured global pause and a real host transfer, then installed exactly once
 * after the first eligible periodic retry. */
/* O05 / D2: the GAMEPLAY_READY (respawn_epoch 0) the client actually
 * consumed, as the production server stamped it. */
static struct { bool seen; uint8_t slot; uint32_t gen; uint32_t bind_seq; } d2_ready;
static void d2_ready_observer(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_LOCAL_GUEST
	   && call->payload.local_guest.result == LOCAL_GUEST_GAMEPLAY_READY
	   && call->payload.local_guest.respawn_epoch == 0) {
		d2_ready.seen = true;
		d2_ready.slot = call->payload.local_guest.world_slot;
		d2_ready.gen = call->payload.local_guest.world_generation;
		d2_ready.bind_seq = call->payload.local_guest.guest_bind_seq;
	}
}

static void case_pending_transfer(void) {
	struct server_local* s = rig(VSP_BOAT);
	assert(rig_ready(s));
	host_mount(s, 10, VSP_BOAT);
	ticks(s, 1);
	/* Declared Leave through the guest's own production route. */
	struct client_session* g = &gstate.players[1];
	assert(client_session_request_local_leave(g));
	pump_handshake(s);
	pump_handshake(s);
	clin_update_drain();
	check(!s->guest_actor && g->local_phase == CLIENT_GUEST_IDLE,
		"pending: the declared guest Leave retired the old lifetime");
	client_session_clear_windows(g);
	memset(g, 0, sizeof(*g));
	g->visible_window = -1;
	vsp_set_op(50);
	vsp_cmd_refusal(50);
	assert(client_session_request_local_join(g));
	for(unsigned i = 0; i < 4; i++)
		pump_handshake(s);
	capture();
	bool armed = false, blocked = false;
	for(unsigned i = 0; i < cap_n; i++)
		if(cap[i].event == VSP_EV_PENDING) {
			armed |= cap[i].u.progress.stage == VSP_PD_ARMED;
			blocked |= cap[i].u.progress.stage == VSP_PD_BLOCKED;
		}
	check(armed && blocked && s->guest_accept_publish.pending
		&& g->local_phase != CLIENT_GUEST_ATTACHED,
		"pending: the real ACCEPT tuple is retained by the armed refusal");
	/* Global pause, >= the contract's measured interval of real updates. The
	 * harness rests at paused=true (transport-only stepping); start from a
	 * running world so the toggle IS the real pause. */
	s->paused = false;
	ticks_running(s, 2);
	struct server_rpc toggle = {.type = SRPC_TOGGLE_PAUSE};
	host_send(s, &toggle);
	check(s->paused, "pending: SRPC_TOGGLE_PAUSE paused the running world");
	uint32_t active_before = vsp_active_update();
	for(unsigned i = 0; i < 20; i++) {
		tick(s); /* s->paused is the real global pause now */
		client_acks_marker(s);
	}
	uint32_t active_paused = vsp_active_update();
	host_send(s, &toggle);
	check(!s->paused, "pending: the second toggle resumed it");
	capture();
	check(find_ev(VSP_EV_PAUSE, 0) >= 0 && find_ev(VSP_EV_RESUME, 0) > find_ev(VSP_EV_PAUSE, 0),
		"pending: real SRPC_TOGGLE_PAUSE pause/resume observed");
	check(active_paused == active_before, "pending: active_update frozen across the pause");
	bool kept_pause = false;
	for(unsigned i = 0; i < cap_n; i++)
		kept_pause |= cap[i].event == VSP_EV_PENDING
			&& cap[i].u.progress.stage == VSP_PD_RETAINED
			&& cap[i].u.progress.cause == VSP_CAUSE_GLOBAL_PAUSE;
	check(kept_pause && s->guest_accept_publish.pending,
		"pending: the tuple survived the global pause (retained/global_pause)");
	/* The predeclared fixture_request transfer while mounted. */
	vsp_set_op(51);
	vsp_cmd_transfer(51);
	s->paused = false;
	uint32_t release_active = 0, install_active = 0;
	bool committed = false;
	for(unsigned i = 0; i < 4000 && !committed; i++) {
		tick(s);
		client_acks_marker(s);
		capture();
		committed = find_ev(VSP_EV_TRANSFER_COMMIT, 0) >= 0
			&& !server_transfer_active(s);
	}
	for(unsigned i = 0; i < 400 && !s->guest_snapshot_sent; i++) {
		tick(s);
		client_acks_marker(s);
		pump_handshake(s);
		capture();
		if(!release_active && find_ev(VSP_EV_BARRIER_RELEASE, 0) >= 0)
			release_active = vsp_active_update();
	}
	install_active = vsp_active_update();
	s->paused = true;
	capture();
	int tb = find_ev(VSP_EV_TRANSFER_BEGIN, 0), clr = find_ev(VSP_EV_ADMISSION_CLEAR, 0);
	int rel = find_ev(VSP_EV_TRANSFER_RELEASE, 0), com = find_ev(VSP_EV_TRANSFER_COMMIT, 0);
	int bar = find_ev(VSP_EV_BARRIER_RELEASE, 0), ins = find_ev(VSP_EV_INSTALLED, 0);
	int ret = -1;
	for(unsigned i = 0; i < cap_n; i++)
		if(cap[i].event == VSP_EV_PENDING && cap[i].u.progress.stage == VSP_PD_RETAINED
		   && cap[i].u.progress.cause == VSP_CAUSE_TRANSFER)
			ret = (int)i;
	check(tb >= 0 && cap[tb].origin == VSP_FIXTURE && cap[tb].u.transfer.mode == 1,
		"transfer: fixture_request begin through server_transfer_request (labelled)");
	check(clr > tb && ret > clr && cap[ret].u.progress.transfer_open
		&& !cap[ret].u.progress.refusal_armed,
		"progress: refusal cleared while the transfer was open, then natural retention");
	check(rel >= 0 && com > rel
		&& cap[rel].u.transfer.serial < cap[com].u.transfer.serial,
		"transfer: release before commit (authoritative serials)");
	if(com >= 0)
		check(cap[com].u.transfer.destination_mount_count == 0
			&& cap[com].u.transfer.host.mount_count == 0
			&& cap[com].u.transfer.vehicle.alive && !cap[com].u.transfer.vehicle.occupied,
			"transfer: commit leaves no host relation, source boat alive and empty");
	check(bar > com && ins > bar, "progress: barrier_release after commit, install after release");
	check(count_ev(VSP_EV_INSTALLED) == 1 && install_active - release_active <= 200,
		"progress: exactly one INSTALLED within 200 active updates of release");
	check(cap_serialize_errors == 0, "pending: every record serializes");
	/* O05 / D2 (ASTRA-DEFECTS-RULING 9): the PC smoke's production order
	 * continues -- CONFIRM, GAMEPLAY, READY -- through the real server and
	 * client handlers. The host is now in its transfer destination; the
	 * readmitted guest stays in the source world. The client must bind view 1,
	 * the body's store and its collision terrain to the world the SERVER
	 * admitted the guest into, never to the host's new world. */
	d2_ready.seen = false;
	clin_test_set_consume_observer(d2_ready_observer, NULL);
	for(unsigned i = 0; i < 8 && !s->guest_confirmed; i++)
		pump_handshake(s);
	for(unsigned i = 0; i < 40 && !g->local_gameplay_ready; i++) {
		if(g->local_phase == CLIENT_GUEST_ATTACHED && !g->local_pending)
			(void)client_session_request_local_gameplay(g);
		pump_handshake(s);
		if(i == 20 && s->guest_actor && !s->guest_gameplay)
			guest_body_home(s); /* the readiness predicate's published column */
	}
	{
		const struct server_world* gw
			= s->guest_actor ? server_local_world_of(s, s->guest_actor) : NULL;
		const struct server_world* hw = server_local_world_of(s, &s->player);
		check(gw && hw && gw != hw,
			"D2 rig: after the host transfer the readmitted guest stays in its own world");
		check(g->local_gameplay_ready && g->local_player,
			"D2 rig: the readmitted guest reaches gameplay");
		clin_test_set_consume_observer(NULL, NULL);
		check(d2_ready.seen && gw && d2_ready.slot == gw->world_index
			&& d2_ready.gen == gw->generation && d2_ready.gen != 0
			&& (d2_ready.slot != hw->world_index || d2_ready.gen != hw->generation),
			"D2 rig: the server's READY names the guest's OWN world, not the host's");
		/* O07 / R04 D2-1: the producer stamps the CURRENT guest binding (the
		 * readmission bumped it), so an older READY/reset is fenced client-side */
		check(d2_ready.seen && d2_ready.bind_seq != 0
			&& d2_ready.bind_seq == server_local_guest_bind_seq(),
			"D2 rig: READY carries the server's current nonzero guest-binding sequence");
		/* With world-bound payloads ON (the PC smoke / Wii split-dims mode)
		 * the client binds to that identity: test_b2_a2t_client D2 cases. This
		 * harness runs one client world, where the body's store IS the client
		 * world: */
		if(!clin_world_payloads_enabled() && g->local_player)
			check(g->local_player->world == &gstate.world,
				"D2 rig: single-world client keeps the guest in its one world");
		else if(gw && g->local_player) {
			struct clin_world_payload gp;
			bool have = clin_world_payload_of_slot(gw->world_index, &gp);
			check(clin_world_view_slot(1) == (int)gw->world_index && have
				&& g->local_player->world == gp.terrain,
				"D2 rig: view 1 + body terrain are the guest's world");
		}
	}
	vsp_reset();
	if(s->guest_actor)
		guest_release(s);
	server_local_worlds_discard_secondary(s);
	teardown(s);
}

/* p2: mounted checkpoint begin/durable through the real Save and quit. */
static void case_save(void) {
	struct server_local* s = rig(VSP_PIG);
	assert(rig_ready(s));
	host_mount(s, 10, VSP_PIG);
	ticks(s, 1);
	check(host_on(s, VSP_PIG), "save: host mounted before Save and quit");
	vsp_set_op(60);
	struct server_rpc unload = {.type = SRPC_UNLOAD_WORLD};
	host_send(s, &unload);
	clin_update_drain();
	capture();
	int b = -1, d = -1;
	for(unsigned i = 0; i < cap_n; i++)
		if(cap[i].event == VSP_EV_CHECKPOINT) {
			if(cap[i].u.ck.stage == VSP_CK_BEGIN) b = (int)i;
			else d = (int)i;
		}
	check(b >= 0 && d > b, "save: checkpoint begin then durable around the real release");
	if(b >= 0 && d >= 0) {
		const struct vsp_p_checkpoint* p = &cap[d].u.ck;
		check(!cap[b].u.ck.save_ok && p->save_ok && p->level_ok && p->chunks_ok
			&& p->regions_synced, "save: begin bits false, durable stage receipts all true");
		check(p->host.mount_count == 1 && p->vehicle.occupied && p->vehicle.saddled,
			"save: durable receipt still names the live mounted relation");
		float pose[3] = {(float)p->host.pos[0], (float)p->host.pos[1], (float)p->host.pos[2]};
		check(memcmp(pose, p->pose_bits, sizeof(pose)) == 0,
			"save: pose_bits are the archive's binary32 eye-basis Pos");
	}
	check(!s->player.has_pos, "save: the world really released (Save and quit committed)");
	check(vsp_hook_mask() == 0, "save: world teardown reset every mutation hook");
	/* The process ends here: the released world has no live owners left to
	 * tear down through the harness. */
	vsp_reset();
}

/* DORMANT: the same real RPC script with the probe disarmed and armed
 * (observe-only) yields the same simulation digest; disarmed yields no
 * record and no counter movement. */
static uint32_t digest_rpc;
static void digest_consumed(const struct client_rpc* rpc, void* user) {
	consumed(rpc, user);
	digest_rpc = vsp_fnv_bytes(digest_rpc, &rpc->type, sizeof(rpc->type));
}
static uint32_t digest_server(struct server_local* s) {
	uint32_t h = vsp_fnv_begin();
	for(uint32_t id = 0; id < 64; id++) {
		struct entity* e = ent(s, id);
		if(!e)
			continue;
		h = vsp_fnv_bytes(h, &e->id, sizeof(e->id));
		h = vsp_fnv_bytes(h, &e->type, sizeof(e->type));
		h = vsp_fnv_bytes(h, e->pos, sizeof(e->pos));
		h = vsp_fnv_bytes(h, e->vel, sizeof(e->vel));
		if(e->type == ENTITY_BOAT) {
			h = vsp_fnv_bytes(h, &e->data.boat.occupied, sizeof(bool));
			h = vsp_fnv_bytes(h, &e->data.boat.occupant_id, sizeof(uint32_t));
		} else if(e->type == ENTITY_MINECART) {
			h = vsp_fnv_bytes(h, &e->data.minecart.occupied, sizeof(bool));
			h = vsp_fnv_bytes(h, &e->data.minecart.ride_epoch, sizeof(uint32_t));
		} else if(e->type == ENTITY_MONSTER) {
			h = vsp_fnv_bytes(h, &e->data.monster.pig_ridden, sizeof(bool));
			h = vsp_fnv_bytes(h, &e->data.monster.saddled, sizeof(bool));
		}
	}
	double pose[6] = {s->player.x, s->player.y, s->player.z,
		s->guest_actor ? s->guest_actor->x : 0, s->guest_actor ? s->guest_actor->y : 0,
		s->guest_actor ? s->guest_actor->z : 0};
	h = vsp_fnv_bytes(h, pose, sizeof(pose));
	h = vsp_fnv_bytes(h, &s->player.health, sizeof(s->player.health));
	h = vsp_fnv_bytes(h, &s->player.riding_boat, sizeof(bool));
	h = vsp_fnv_bytes(h, &s->player.riding_minecart, sizeof(bool));
	h = vsp_fnv_bytes(h, s->player.inventory.items, sizeof(s->player.inventory.items));
	h = vsp_fnv_bytes(h, &s->rand_src, sizeof(s->rand_src));
	h = vsp_fnv_bytes(h, &s->world_time, sizeof(s->world_time));
	h = vsp_fnv_bytes(h, &s->next_entity_id, sizeof(s->next_entity_id));
	return vsp_fnv_bytes(h, &digest_rpc, sizeof(digest_rpc));
}
static uint32_t dormant_run(bool armed) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	load_area(s);
	/* The probe script is not an eviction-throughput test. Retire one support
	 * column per update, below inbox capacity, so the concurrent consumer's
	 * scheduling cannot decide how many unrelated UNLOADs enter its digest. */
	s->near_evict_budget = 1;
	server_local_test_set_near_send_minimum(server_local_test_chunk_send_batch());
	/* F4-RNG: World.rand / Entity.rand are runtime-seeded (new Random()) per
	 * process; the off/on children must start from identical streams. */
	jrand_set_seed(&s->worlds[0].rand, 4242);
	server_local_test_seed_body_rand(s, &s->player, 13579);
	if(s->guest_actor)
		server_local_test_seed_body_rand(s, s->guest_actor, 24680);
	*server_local_sound_rng() = (struct random_gen){.seed = 13579};
	digest_rpc = vsp_fnv_begin();
	clin_test_set_consume_observer(digest_consumed, NULL);
	vsp_reset();
	if(armed)
		arm(VSP_BOAT);
	/* Identical setup without any probe hook: real spawn helpers. */
	struct entity* boat = server_local_spawn_boat((vec3) {-1.0F, 4.0F, 1.0F}, s);
	struct entity* pig = server_local_spawn_monster((vec3) {2.5F, 4.0F, 0.5F}, MONSTER_PIG, s);
	assert(boat && pig);
	pig->data.monster.saddled = true; /* identical in both runs: setup only */
	ticks(s, 2);
	host_use(s, 1, boat->id, true);
	ticks(s, 5);
	host_use(s, 2, pig->id, true); /* switch */
	ticks(s, 5);
	struct server_rpc use = {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = boat->id, .desired_occupied = true}};
	(void)guest_send(s, &use);
	ticks(s, 3);
	host_use(s, 3, pig->id, false); /* dismount */
	ticks(s, 5);
	capture();
	uint32_t h = digest_server(s);
	if(!armed)
		check(cap_n == 0 && vsp_server_update() == 0 && vsp_pending_records() == 0,
			"dormant: disarmed, no record, no counter movement, no queue");
	else
		check(cap_n > 0 && vsp_overflow_count() == 0,
			"dormant: armed observe-only run produced records");
	printf("DORMANT_DIGEST %08x\n", h);
	vsp_reset();
	teardown(s);
	clin_test_set_consume_observer(consumed, NULL);
	return h;
}
/* One fixture per process: run each pass as a child of this binary, then
 * compare the digests the children printed. */
static bool dormant_child(const char* self, const char* mode, uint32_t* digest) {
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "\"\"%s\" dormant-run %s\"", self, mode);
#ifdef _WIN32
	FILE* f = _popen(cmd, "r");
#else
	snprintf(cmd, sizeof(cmd), "\"%s\" dormant-run %s", self, mode);
	FILE* f = popen(cmd, "r");
#endif
	if(!f)
		return false;
	char line[512];
	bool got = false, pass = false;
	while(fgets(line, sizeof(line), f)) {
		unsigned v;
		if(sscanf(line, "DORMANT_DIGEST %x", &v) == 1) {
			*digest = v;
			got = true;
		}
		if(strstr(line, "]: PASS"))
			pass = true;
		if(!strncmp(line, "FAIL", 4) || !strncmp(line, "ok  ", 4))
			printf("  [%s] %s", mode, line);
	}
#ifdef _WIN32
	int rc = _pclose(f);
#else
	int rc = pclose(f);
#endif
	return got && pass && rc == 0;
}
static void case_dormant(const char* self) {
	uint32_t off = 0, on = 0;
	bool a = dormant_child(self, "off", &off);
	bool b = dormant_child(self, "on", &on);
	printf("dormant digests: off=%08x on=%08x\n", off, on);
	check(a && b, "dormant: both child passes ran their own checks green");
	check(a && b && off == on,
		"dormant: observe-only probe leaves state, RPC order, inventory and RNG identical");
}

/* The driver's arming, gate and fail-closed finish paths. */
static unsigned drv_emits, drv_done;
static char drv_status[96], drv_last[VSP_LINE_CAP];
static bool drv_create_ok;
static const char* drv_conflict;
static bool drv_emit(void* u, const char* line) {
	(void)u;
	snprintf(drv_last, sizeof(drv_last), "%s", line);
	drv_emits++;
	return true;
}
static void drv_profile_done(void* u, const char* status) {
	(void)u;
	drv_done++;
	snprintf(drv_status, sizeof(drv_status), "%s", status);
}
static bool drv_create(void* u, const char* dir) {
	(void)u;
	return drv_create_ok && vehicle_smoke_rig_create_allowed(dir);
}
static const char* drv_conflict_fn(void* u) {
	(void)u;
	return drv_conflict;
}
/* O22: a dead telemetry stream (every emit refused, telemetry_lost true). */
static bool drv_lost;
static unsigned drv_refused;
static bool drv_emit_refuse(void* u, const char* line) {
	(void)u;
	(void)line;
	drv_refused++;
	return false;
}
static bool drv_lost_fn(void* u) {
	(void)u;
	return drv_lost;
}
static bool drv_create_any(void* u, const char* dir) {
	(void)u;
	return vehicle_smoke_rig_create_allowed(dir);
}
static void case_driver_telemetry_lost(void) {
	struct vehicle_smoke_hooks hooks = {.emit = drv_emit_refuse,
		.profile_done = drv_profile_done, .create_world = drv_create_any,
		.autoload_conflict = drv_conflict_fn, .telemetry_lost = drv_lost_fn};
	struct client_session host = {0}, guest = {0};
	struct vehicle_smoke_ctx ctx = {.host = &host, .guest = &guest};
	struct vehicle_smoke_actions act;
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	char* argv[] = {"x", "--cw-tel-host", "h", "--cw-profile", "host_vehicle_smoke_8m",
		"--cw-vehicle-kind", "boat", "--cw-run-id", "r9"};
	char err[128];
	check(hw_run_parse_args(&cfg, 9, argv, err, sizeof(err)), "lost: parse");
	vehicle_smoke_reset();
	drv_conflict = NULL;
	drv_lost = true;
	drv_done = 0;
	drv_refused = 0;
	vehicle_smoke_arm(&cfg, &hooks, "b", 2);
	/* 20 s of frames: well inside a stage timeout (12 s) + drain (15 s). */
	for(unsigned f = 0; f < 400 && !drv_done; f++) {
		ctx.now_ms = 50u * f;
		vehicle_smoke_frame(&ctx, &act);
	}
	printf("lost: refused=%u done=%u status=%s\n", drv_refused, drv_done, drv_status);
	check(drv_refused > 0 && drv_done == 1
		&& strcmp(drv_status, "fail:telemetry_quiesced") == 0,
		"O22: a dead telemetry stream fails BY NAME (telemetry_quiesced), "
		"not as a later unrelated stage timeout");
	/* A live stream that merely refuses (ring full) is NOT a failure. */
	vehicle_smoke_reset();
	drv_lost = false;
	drv_done = 0;
	vehicle_smoke_arm(&cfg, &hooks, "b", 2);
	for(unsigned f = 0; f < 40; f++) {
		ctx.now_ms = 50u * f;
		vehicle_smoke_frame(&ctx, &act);
	}
	check(drv_done == 0 && !vehicle_smoke_finished(),
		"O22: refusals on a live stream are retried, never a failure");
	vehicle_smoke_reset();
}

static void case_driver(void) {
	struct vehicle_smoke_hooks hooks = {.emit = drv_emit,
		.profile_done = drv_profile_done, .create_world = drv_create,
		.autoload_conflict = drv_conflict_fn};
	struct client_session host = {0}, guest = {0};
	struct vehicle_smoke_ctx ctx = {.host = &host, .guest = &guest};
	struct vehicle_smoke_actions act;
	/* Unarmed: zeroed actions, gate refuses, nothing emitted. */
	vehicle_smoke_reset();
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	vehicle_smoke_arm(&cfg, &hooks, "b", 3);
	vehicle_smoke_frame(&ctx, &act);
	check(!vehicle_smoke_armed() && !act.host_buttons_armed && !act.guest_frame_valid
		&& !vehicle_smoke_rig_create_allowed(VEHICLE_SMOKE_RIG_WORLD) && !vsp_armed(),
		"driver: a non-vehicle profile leaves driver, gate and probe disarmed");
	/* Armed from a real parsed profile. */
	char* argv[] = {"x", "--cw-tel-host", "h", "--cw-profile", "host_vehicle_smoke_8m",
		"--cw-vehicle-kind", "cart", "--cw-run-id", "r1"};
	char err[128];
	check(hw_run_parse_args(&cfg, 9, argv, err, sizeof(err)), "driver: profile + kind parse");
	vehicle_smoke_arm(&cfg, &hooks, "b", 2);
	check(vehicle_smoke_armed() && vsp_armed() && vsp_kind_now() == VSP_CART
		&& vehicle_smoke_rig_create_allowed(VEHICLE_SMOKE_RIG_WORLD)
		&& !vehicle_smoke_rig_create_allowed("my_world"),
		"driver: armed for the parsed kind; the rig gate names ONLY cw_vehicle_rig");
	/* A refused create fails BY NAME and finishes exactly once. */
	drv_create_ok = false;
	drv_emits = drv_done = 0;
	for(unsigned f = 0; f < 10; f++) {
		ctx.now_ms = 50u * f;
		vehicle_smoke_frame(&ctx, &act);
	}
	check(vehicle_smoke_finished() && drv_done == 1
		&& strcmp(drv_status, "fail:rig_world_create_refused") == 0,
		"driver: create refusal -> fail record + exactly one profile_done");
	check(strstr(drv_last, "\"event\":\"vehicle_smoke.fail\"")
		&& strstr(drv_last, "\"ok\":false") && strstr(drv_last, "\"code\":\"rig_world_create_refused\""),
		"driver: the last line is the fail record (ok=false, contract code)");
	check(drv_emits >= 3 && vehicle_smoke_seq_now() == drv_emits,
		"driver: run_start/phase_begin/fail with a contiguous seq");
	check(vsp_hook_mask() == 0 && !vsp_armed(), "driver: failure disarms hooks and the probe");
	/* A consumed autoload latch is refused BEFORE any create. */
	vehicle_smoke_arm(&cfg, &hooks, "b", 2);
	drv_conflict = "something";
	drv_create_ok = true;
	drv_done = 0;
	for(unsigned f = 0; f < 10; f++) {
		ctx.now_ms = 50u * f;
		vehicle_smoke_frame(&ctx, &act);
	}
	check(drv_done == 1 && strcmp(drv_status, "fail:autoload_latch_consumed") == 0,
		"driver: a pre-consumed autoload latch fails the run before the create");
	/* The global deadline is a FAIL, never an elapsed-time pass. */
	vehicle_smoke_arm(&cfg, &hooks, "b", 2);
	drv_conflict = NULL;
	drv_done = 0;
	ctx.now_ms = 0;
	vehicle_smoke_frame(&ctx, &act);
	ctx.now_ms = VEHICLE_SMOKE_P0_MS + 10u;
	for(unsigned f = 0; f < 4; f++)
		vehicle_smoke_frame(&ctx, &act);
	check(drv_done == 1 && strncmp(drv_status, "fail:phase_timeout", 18) == 0,
		"driver: a phase ceiling expires to a named FAIL");
	vehicle_smoke_reset();
}

/* ===== O11 / S03 #2: the receipt HELPER protocol (reserve / sent / done) ====
 * O16 (R06 P3) scope statement: this case drives vsp_dispatch_reserve /
 * vsp_dispatch_sent BY HAND around a real svin UNLOAD send, with a fast
 * server consuming it in between. It proves the helper's ordering contract
 * (a receipt reserved before the send sorts before every row the request
 * caused; no helper call = no receipt; one call = one receipt). It does NOT
 * drive main.c's actual Save consumer, nor force a real transport enqueue
 * refusal there: removing main.c's hook would survive this case. The guest
 * send seam IS driven for real by dispatch-guest (client_session_flush_local). */
static int ui_index(uint8_t action) {
	for(unsigned i = 0; i < cap_n; i++)
		if(cap[i].event == VSP_EV_UI_ACTION && cap[i].u.ui.action == action)
			return (int)i;
	return -1;
}
static void case_dispatch_seam(void) {
	struct server_local* s = rig(VSP_PIG);
	assert(rig_ready(s));
	host_mount(s, 10, VSP_PIG);
	ticks(s, 1);
	capture();
	cap_n = 0;
	/* The pre-fix observer, for contrast: a receipt pushed on the NEXT driver
	 * frame lands after the server rows (the S03 failure shape). */
	vsp_await_dispatch(VSP_UI_GUEST_LEAVE, 1, VSP_SCR_PAUSE, VSP_BTN_GUI_CLICK,
					   6, 61);
	vsp_srv_respawn_stage(s, VSP_RS_RELEASED); /* a server row first */
	vsp_dispatch_sent(VSP_UI_GUEST_LEAVE, vsp_dispatch_reserve()); /* late */
	capture();
	check(find_ev(VSP_EV_RESPAWN, 0) >= 0 && ui_index(VSP_UI_GUEST_LEAVE) >= 0
			  && find_ev(VSP_EV_RESPAWN, 0) < ui_index(VSP_UI_GUEST_LEAVE),
		  "seam (contrast): an order reserved AFTER the server row sorts after it");
	cap_n = 0;
	/* (1) Save: the exact S03 pig race (checkpoint BEGIN before the Save
	 * receipt). Refused first: nothing recorded. */
	vsp_await_dispatch(VSP_UI_SAVE_EXIT, 0, VSP_SCR_PAUSE, VSP_BTN_GUI_CLICK,
					   5, 60);
	uint32_t refused = vsp_dispatch_reserve();
	check(refused != 0, "helper: an awaited receipt reserves an order");
	/* modelled refusal (no real enqueue refusal is forced here): the helper
	 * is simply not called, as main.c does not call it on a refused send */
	capture();
	check(ui_index(VSP_UI_SAVE_EXIT) < 0 && !vsp_dispatch_done(VSP_UI_SAVE_EXIT),
		  "helper: no vsp_dispatch_sent call = no receipt (modelled refusal)");
	/* retry: reserve, ACCEPTED send, the server consumes it at once (its
	 * checkpoint rows are pushed now), only THEN the seam pushes the receipt */
	vsp_set_op(60);
	uint32_t order = vsp_dispatch_reserve();
	struct server_rpc unload = {.type = SRPC_UNLOAD_WORLD};
	check(svin_rpc_send(&unload), "seam: the UNLOAD request is queued");
	drain_server(s); /* the fast server: checkpoint begin/durable pushed */
	vsp_dispatch_sent(VSP_UI_SAVE_EXIT, order);
	clin_update_drain();
	capture();
	int rec = ui_index(VSP_UI_SAVE_EXIT);
	int ck = find_ev(VSP_EV_CHECKPOINT, 0);
	check(rec >= 0 && ck >= 0 && rec < ck,
		  "helper: a receipt reserved before the real UNLOAD send precedes the "
		  "checkpoint it caused, even when the server ran first");
	if(rec >= 0)
		check(cap[rec].op_id == 60 && cap[rec].u.ui.player == 0
				  && cap[rec].u.ui.edge_seq == 5 && cap[rec].u.ui.delivered
				  && vsp_dispatch_done(VSP_UI_SAVE_EXIT),
			  "helper: the receipt carries the awaited op/owner/edge");
	unsigned n_save = 0;
	for(unsigned i = 0; i < cap_n; i++)
		n_save += cap[i].event == VSP_EV_UI_ACTION
			&& cap[i].u.ui.action == VSP_UI_SAVE_EXIT;
	check(n_save == 1, "helper: exactly one Save receipt (one sent call)");
	/* the accepted UNLOAD released the world; the process ends here */
}

/* The guest's real accepted-send seam: client_session_flush_local. */
static void case_dispatch_guest(void) {
	struct server_local* s = rig(VSP_BOAT);
	assert(rig_ready(s));
	capture();
	cap_n = 0;
	struct client_session* g = &gstate.players[1];
	check(g->local_ticket != 0, "guest seam: an admitted guest session");
	vsp_await_dispatch(VSP_UI_GAMEPLAY, 1, VSP_SCR_INGAME, VSP_BTN_NONE, 0, 77);
	/* a CONFIRM is not the awaited receipt: no row */
	g->local_pending = true;
	g->local_pending_action = LOCAL_GUEST_CONFIRM;
	client_session_flush_local(g);
	capture();
	check(ui_index(VSP_UI_CONFIRM) < 0 && ui_index(VSP_UI_GAMEPLAY) < 0,
		  "guest seam: only the awaited receipt is recorded");
	g->local_pending = true;
	g->local_pending_action = LOCAL_GUEST_GAMEPLAY;
	client_session_flush_local(g); /* the real send seam */
	drain_server(s);               /* the server consumes it at once */
	vsp_srv_respawn_stage(s, VSP_RS_RELEASED);
	client_session_flush_local(g); /* nothing pending: no second receipt */
	capture();
	int rec = ui_index(VSP_UI_GAMEPLAY);
	check(rec >= 0 && !g->local_pending && vsp_dispatch_done(VSP_UI_GAMEPLAY),
		  "guest seam: the accepted GAMEPLAY send recorded its receipt");
	if(rec >= 0)
		check(cap[rec].op_id == 77 && cap[rec].u.ui.player == 1
				  && cap[rec].u.ui.input_epoch == vsp_client_actor_epoch(1)
				  && find_ev(VSP_EV_RESPAWN, 0) > rec,
			  "guest seam: op/owner/epoch, and ordered before later server rows");
	unsigned n = 0;
	for(unsigned i = 0; i < cap_n; i++)
		n += cap[i].event == VSP_EV_UI_ACTION && cap[i].u.ui.action == VSP_UI_GAMEPLAY;
	check(n == 1, "guest seam: exactly one GAMEPLAY receipt");
	teardown(s);
}

/* ===== O11 / S03 #3: a REAL pickup inside a P2 window still fails ========
 * A collectible spawned next to the guest: the window-open guard names it,
 * and the real Beta pickup (no rule change) changes the guest inventory
 * between the window's before/after rows -- the invariant still fails. */
static void case_pickup_negative(void) {
	struct server_local* s = rig(VSP_BOAT);
	assert(rig_ready(s));
	capture();
	struct server_player* g = s->guest_actor;
	assert(g);
	/* The host collects first when both reach an item (entity_item.c); this
	 * negative is the GUEST's, so the rig's host body (sharing the guest's
	 * column here) stands 6 m away for it. */
	s->player.x += 6.0;
	/* (a) a REAL pickup inside an open window: the window opens clean, the
	 * item lands beside the guest, ordinary Beta collection takes it. */
	check(!vsp_srv_collectible_near_guest(s), "pickup: the window opens clean");
	vsp_set_op(90);
	vsp_iso_target(0);
	vsp_iso_bracket(90, true);
	ticks(s, 1);
	check(!vsp_fixture_failure(), "pickup: a clean window open raises nothing");
	struct item_data stack = {.id = BLOCK_COBBLESTONE, .count = 3};
	struct entity* item = server_local_spawn_item(
		(vec3) {(float)g->x + 0.3F, (float)(g->y - 1.62) + 0.2F, (float)g->z},
		&stack, false, s);
	check(item != NULL, "pickup: a real item entity lands beside the guest");
	ticks(s, 40);
	vsp_iso_bracket(90, false);
	ticks(s, 2);
	capture();
	int before = -1, after = -1;
	for(unsigned i = 0; i < cap_n; i++)
		if(cap[i].event == VSP_EV_ISOLATION && cap[i].u.iso.owner == VSP_STAGE_SERVER
		   && cap[i].u.iso.attempt_op == 90) {
			if(cap[i].u.iso.stage == VSP_STAGE_BEFORE && before < 0) before = (int)i;
			if(cap[i].u.iso.stage == VSP_STAGE_AFTER) after = (int)i;
		}
	check(before >= 0 && after >= 0, "pickup: server before/after rows exist");
	if(before >= 0 && after >= 0)
		check(cap[before].u.iso.u.server.guest.inv_hash
				  != cap[after].u.iso.u.server.guest.inv_hash
				  && cap[after].u.iso.u.server.guest.inv_used
					 > cap[before].u.iso.u.server.guest.inv_used,
			  "pickup: the real collection changes the guest inventory inside the "
			  "window -- the P2 inventory invariant still FAILS");
	/* (b) a window that OPENS with a collectible in reach fails by name. */
	struct item_data stack2 = {.id = BLOCK_DIRT, .count = 1};
	(void)server_local_spawn_item(
		(vec3) {(float)g->x + 0.3F, (float)(g->y - 1.62) + 2.5F, (float)g->z},
		&stack2, false, s); /* still falling: not yet collected */
	check(vsp_srv_collectible_near_guest(s), "pickup: the guard sees it in reach");
	vsp_set_op(91);
	vsp_iso_bracket(91, true);
	ticks(s, 1);
	check(vsp_fixture_failure()
			  && !strcmp(vsp_fixture_failure(), "isolation_collectible_near_guest"),
		  "pickup: a window opening with a collectible in reach fails by name");
	teardown(s);
}

/* ===== O14 (CONTRACT-v1.3 two-home P2 fixture) ==============================
 * geom:     D/H2 distances and the corrected pickup-box guard (corner 2.016 m).
 * envelope: fixture precondition (a) -- an ice block inside the envelope makes
 *           the real fixture build fail BY NAME; the clean rig passes it.
 * far:      the server's remote-home enforcement: a guest at H2 passes and is
 *           sampled; a guest back at H1 fails guest_inside_drop_envelope.
 * lethal:   precondition (b) -- a mounted death point outside D refuses the
 *           lethal commit BY NAME (no death happens). */
static void case_two_home(const char* sub) {
	if(!strcmp(sub, "geom")) {
		check(vsp_death_region_distance(0, 0, -0.5, -12.5) == 12.5,
			  "two-home: H2 is 12.5 m from the nearest D point");
		check(vsp_death_region_distance(0, 0, -0.5, -12.5) >= VSP_DROP_N_M + 1.0,
			  "two-home: H2 clears N = 11.0 m by >= 1 m");
		check(vsp_death_region_distance(0, 0, -0.5, -1.0) == 1.0,
			  "two-home: H1 (interaction home) is 1.0 m from D");
		check(vsp_death_region_distance(0, 0, -2.0, 0.0) == 0.0
				  && vsp_death_region_distance(0, 0, 4.0, 2.5) == 0.0
				  && vsp_death_region_distance(0, 0, 3.0, 1.0) == 0.0,
			  "two-home: the corners and interior of D are inside");
		check(VSP_DROP_FLIGHT_M + VSP_PICKUP_REACH_EUCLID_M + 0.5 <= VSP_DROP_N_M,
			  "two-home: N >= flight + pickup corner + margin");
		/* the pickup box corner (1.42, 1.42) = 2.008 m: inside Beta reach,
		 * outside the old 2.0 m guard radius */
		struct server_local* s = rig(VSP_BOAT);
		assert(rig_ready(s));
		struct server_player* g = s->guest_actor;
		assert(g);
		s->player.x += 6.0;
		struct item_data stack = {.id = BLOCK_DIRT, .count = 1};
		(void)server_local_spawn_item(
			(vec3) {(float)g->x + 1.42F, (float)(g->y - 1.62) + 2.5F,
					(float)g->z + 1.42F}, &stack, false, s);
		check(vsp_srv_collectible_near_guest(s),
			  "two-home: the guard covers the pickup box corner (2.008 m)");
		teardown(s);
		return;
	}
	if(!strcmp(sub, "envelope")) {
		struct server_local* s = rig(VSP_BOAT);
		const int ax = (int)floor(s->player.x), az = (int)floor(s->player.z);
		int Y = -1;
		for(int y = (int)floor(s->player.y); y >= 0; y--) {
			struct block_data b;
			if(server_world_get_block(&s->worlds[0], ax, y, az, &b)
			   && b.type != BLOCK_AIR) {
				Y = y;
				break;
			}
		}
		assert(Y > 0);
		/* ice on the surface 10 m east: inside D grown by 9 m */
		assert(server_world_set_block_silent(&s->worlds[0], ax + 10, Y, az,
			(struct block_data) {.type = BLOCK_ICE}));
		vsp_set_op(1);
		vsp_cmd_fixture(1);
		ticks(s, 1);
		check(vsp_fixture_failure()
				  && !strcmp(vsp_fixture_failure(), "fixture_envelope_precondition"),
			  "two-home: ice inside the drop envelope fails the fixture by name");
		check(!vsp_fixture_targets(NULL, NULL),
			  "two-home: no rig is published over a failed precondition");
		teardown(s);
		return;
	}
	if(!strcmp(sub, "far")) {
		struct server_local* s = rig(VSP_BOAT);
		assert(rig_ready(s));
		int32_t anchor[3];
		struct vsp_r t[3];
		assert(vsp_fixture_targets(t, anchor));
		struct server_player* g = s->guest_actor;
		assert(g);
		g->x = anchor[0] + (double)VSP_HOME2_DX;
		g->z = anchor[2] + (double)VSP_HOME2_DZ;
		/* moved, then still: the rest mark lands one update after the move
		 * and is >= 1 update old one update later */
		ticks(s, 3);
		check(vsp_server_guest_at_rest(1), "two-home: the server acks a still guest");
		vsp_far_home_enforce();
		ticks(s, 5);
		check(!vsp_fixture_failure(), "two-home: a guest at H2 passes the envelope");
		check(vsp_far_home_min_m() > 12.49 && vsp_far_home_min_m() < 12.51,
			  "two-home: the enforced sample reads 12.5 m");
		g->x = anchor[0] - 0.5;
		g->z = anchor[2] - 1.0; /* back at H1 */
		ticks(s, 1);
		check(!vsp_server_guest_at_rest(1), "two-home: a moved guest is not at rest");
		check(vsp_fixture_failure()
				  && !strcmp(vsp_fixture_failure(), "guest_inside_drop_envelope"),
			  "two-home: a guest inside N of D fails by name");
		teardown(s);
		return;
	}
	if(!strcmp(sub, "lethal")) {
		struct server_local* s = rig(VSP_BOAT);
		assert(rig_ready(s));
		host_mount(s, 10, VSP_BOAT);
		ticks(s, 1);
		check(host_on(s, VSP_BOAT), "two-home: host mounted on the boat");
		struct entity* boat = ent(s, target(VSP_BOAT));
		assert(boat);
		/* the mounted pair carried 20 m east of D (fixture-test setup) */
		boat->pos[0] += 20.0F;
		s->player.x += 20.0;
		const int health = (int)s->player.health;
		vsp_set_op(40);
		vsp_cmd_lethal(40);
		ticks(s, 1);
		check(vsp_fixture_failure()
				  && !strcmp(vsp_fixture_failure(), "death_point_outside_region"),
			  "two-home: a death point outside D refuses the lethal by name");
		check((int)s->player.health == health && s->player.health > 0,
			  "two-home: no lethal commit happened");
		teardown(s);
		return;
	}
	check(false, "two-home: unknown sub-case");
}

/* ===== O15: the host hop aims at the near hull, not the centre ==============
 * Measured failure (vs_pc_pig_20261004_184503_o15d9, switch_timeout_st0): the
 * unsteered pig stood at the pen's far corner, the host's eye at
 * (ax+3.5, Y+3.295, az+0.45), the boat at (ax-0.9, Y+1.187, az+0.9). Aimed at
 * the boat's centre point the REAL camera entity test (entity_aabb_intersect_ray,
 * what main.c's 4.5 m raycast_entity calls) enters the box only at 4.78 m, so
 * the pick never hit for 12 s. Aimed at the near hull it enters well inside
 * 4.5 m. Same real boat entity and box function as production. */
/* O25: HW5b (real Wii) pinned the boat at x = ax-1.25 against the basin's
 * WEST rim face in the first boat window (bob trough at contact; Beta boats
 * have stepHeight 0, Entity.java:93), then the second window drove west into
 * that face for 25 s -> ride_boat_st1. A window must face the FARTHER wall.
 * The run measures the real server boat physics from the HW5b pose with the
 * rider steering the old fixed west facing and the helper's facing. */
static float boat_window_disp(struct server_local* s, const int32_t a[3],
							   float yaw, float* end_x) {
	struct entity* boat = ent(s, target(VSP_BOAT));
	/* the HW5b pose: centre x ax-1.25, z az+0.9, y 4.19 (floor top a[1]+1) */
	vec3 p = {(float)a[0] - 1.25F, (float)a[1] + 1.19F, (float)a[2] + 0.9F};
	entity_default_teleport(boat, p);
	boat->vel[0] = boat->vel[1] = boat->vel[2] = 0.0F;
	/* let it float to its own bob before the window opens */
	for(unsigned i = 0; i < 40; i++) {
		s->player.rider_vel_x = 0.0F;
		s->player.rider_vel_z = 0.0F;
		ticks(s, 1);
	}
	float x0 = boat->pos[0], z0 = boat->pos[2], best = 0.0F;
	for(unsigned i = 0; i < VEHICLE_SMOKE_RIDE_UPDATES + 10u; i++) {
		/* the client's rider input: 0.02 along the facing (HW5 ride_sample
		 * steer [-0.02, 0] at yaw -pi/2) */
		s->player.rider_vel_x = 0.02F * sinf(yaw);
		s->player.rider_vel_z = 0.0F;
		ticks(s, 1);
		float dx = boat->pos[0] - x0, dz = boat->pos[2] - z0;
		float d = sqrtf(dx * dx + dz * dz);
		if(d > best)
			best = d;
	}
	*end_x = boat->pos[0] - (float)a[0];
	return best;
}

static void case_boat_window_yaw(void) {
	check(vehicle_smoke_boat_yaw(-1.25F, 0) > 0.0F
		&& vehicle_smoke_boat_yaw(-1.817F, 0) > 0.0F
		&& vehicle_smoke_boat_yaw(-0.9F, 0) < 0.0F
		&& vehicle_smoke_boat_yaw(-0.75F, 0) < 0.0F
		&& vehicle_smoke_boat_yaw(6.75F, 8) > 0.0F,
		"boat-yaw: faces the farther basin wall (HW5b ax-1.25 -> +X; start ax-0.9 -> -X)");
	/* one rig per process (harness): both windows run on the same mounted
	 * boat, each from the re-seated HW5b pose */
	struct server_local* s = rig(VSP_BOAT);
	assert(rig_ready(s));
	int32_t a[3];
	assert(vsp_fixture_targets(NULL, a));
	host_mount(s, 10, VSP_BOAT);
	ticks(s, 1);
	check(host_on(s, VSP_BOAT), "boat-yaw: real host mount on the fixture boat");
	float ex_old, ex_new;
	float d_old = boat_window_disp(s, a, -1.5707964F, &ex_old);
	float d_new = boat_window_disp(s, a,
		vehicle_smoke_boat_yaw((float)a[0] - 1.25F, a[0]), &ex_new);
	printf("boat window from ax-1.25: west disp=%.3f end=%.3f | helper disp=%.3f end=%.3f\n",
		d_old, ex_old, d_new, ex_new);
	check(d_old < VEHICLE_SMOKE_RIDE_DISPLACEMENT,
		"boat-yaw: the old fixed west facing pins on the rim face (the HW5b trap)");
	check(d_new >= VEHICLE_SMOKE_RIDE_DISPLACEMENT,
		"boat-yaw: the helper's facing proves RIDE motion from the pinned pose");
	vsp_reset();
	teardown(s);
}

static void case_hull_aim(void) {
	struct server_local* s = rig(VSP_BOAT);
	assert(rig_ready(s));
	int32_t anchor[3];
	struct vsp_r t3[3];
	assert(vsp_fixture_targets(t3, anchor));
	struct entity* boat = ent(s, target(VSP_BOAT));
	assert(boat && boat->getBoundingBox);
	const float ax = (float)anchor[0], Y = (float)anchor[1], az = (float)anchor[2];
	boat->pos[0] = ax - 0.9F;
	boat->pos[1] = Y + 1.187F;
	boat->pos[2] = az + 0.9F;
	const float eye[3] = {ax + 3.5F, Y + 3.295F, az + 0.45F};
	struct AABB b;
	assert(boat->getBoundingBox(boat, &b) == 1);
	const float box[6] = {b.x1, b.y1, b.z1, b.x2, b.y2, b.z2};
	/* the old aim: the centre point (vs_target_point: pos + 0.3 for a boat) */
	const float centre[3] = {boat->pos[0], boat->pos[1] + 0.3F, boat->pos[2]};
	float aim[3];
	vehicle_smoke_hull_aim(box, eye, aim);
	float t_centre = -1, t_aim = -1;
	vec3 o = {eye[0], eye[1], eye[2]}, d;
	for(int k = 0; k < 2; k++) {
		const float* p = k ? aim : centre;
		float dx = p[0] - eye[0], dy = p[1] - eye[1], dz = p[2] - eye[2];
		float n = sqrtf(dx * dx + dy * dy + dz * dz);
		d[0] = dx / n;
		d[1] = dy / n;
		d[2] = dz / n;
		float t = -1;
		bool hit = entity_aabb_intersect_ray(o, d, boat, &t);
		if(k)
			t_aim = hit ? t : -1;
		else
			t_centre = hit ? t : -1;
	}
	printf("hull-aim: centre entry %.3f m, near-hull entry %.3f m (camera pick 4.5)\n",
		   t_centre, t_aim);
	check(t_centre > 4.5F, "hull-aim: the centre aim enters the boat beyond the 4.5 m pick "
						   "(the measured switch_timeout_st0 geometry)");
	check(t_aim >= 0 && t_aim <= 4.5F,
		  "hull-aim: the near-hull aim enters the boat inside the 4.5 m camera pick");
	check(aim[0] > box[0] && aim[0] < box[3] && aim[1] > box[1] && aim[1] < box[4]
			  && aim[2] > box[2] && aim[2] < box[5],
		  "hull-aim: the aim point lies inside the target's own box");
	teardown(s);
}

/* ===== O16 (R06 P2): the guard covers the pickup box's diagonal corner ======
 * R06's independent fixture: boat rig, host 6 m away, a stationary item of 3
 * cobblestone at guest X-1.423, Z-1.423, Y-0.5. Real EntityItem collection
 * takes it (corner of Beta's 1.425 m-per-axis pickup box), so the collectible
 * guard MUST report it -- the old 2.0 m radius said "clear" (radius 2.012). */
static void case_corner_guard(void) {
	struct server_local* s = rig(VSP_BOAT);
	assert(rig_ready(s));
	struct server_player* g = s->guest_actor;
	assert(g);
	s->player.x += 6.0;
	struct item_data stack = {.id = BLOCK_COBBLESTONE, .count = 3};
	struct entity* item = server_local_spawn_item(
		(vec3) {(float)g->x - 1.423F, (float)(g->y - 1.62) - 0.5F + 0.5F,
				(float)g->z - 1.423F}, &stack, false, s);
	check(item != NULL, "corner: a real item at the pickup box corner");
	item->vel[0] = item->vel[1] = item->vel[2] = 0;
	item->data.item.pickup_delay = 0;
	const double dx = item->pos[0] - g->x, dz = item->pos[2] - g->z;
	printf("corner: radius %.6f\n", sqrt(dx * dx + dz * dz));
	check(vsp_srv_collectible_near_guest(s),
		  "corner: the guard reports the corner item (not a false clear)");
	uint32_t id = item->id;
	unsigned before = 0, after = 0;
	for(size_t k = 0; k < INVENTORY_SIZE; k++)
		if(g->inventory.items[k].id == BLOCK_COBBLESTONE)
			before += g->inventory.items[k].count;
	for(int t = 0; t < 4; t++) {
		struct entity** ep = dict_entity_get(s->entities, id);
		if(!ep || !*ep)
			break;
		(void)(*ep)->tick_server(*ep, s);
	}
	for(size_t k = 0; k < INVENTORY_SIZE; k++)
		if(g->inventory.items[k].id == BLOCK_COBBLESTONE)
			after += g->inventory.items[k].count;
	printf("corner: collected %u\n", after - before);
	check(after - before == 3,
		  "corner: real EntityItem collection takes it (the guard is right to refuse)");
	teardown(s);
}

/* ===== O12 / ASTRA-DROPS item 4: death-drop spread MEASUREMENT ============
 * Not a pass/fail case (unregistered): the real mounted lethal fixture, the
 * real InventoryPlayer.dropAllItems path (server_local death drop journal,
 * Beta dropPlayerItemWithRandomChoice velocities, EntityItem physics), then
 * every resulting item's resting horizontal distance from the death point.
 * The guest stands 12 m away so it collects nothing. One line per run:
 * DROP_MEASURE kind seed n max_m max_flight_m. */
static void case_drop_measure(int kind, uint32_t seed) {
	struct server_local* s = rig((enum vsp_kind)kind);
	assert(rig_ready(s));
	host_mount(s, 10, (enum vsp_kind)kind);
	ticks(s, 1);
	if(s->guest_actor)
		s->guest_actor->x += 12.0;
	s->rand_src.seed = seed;
	const double dx0 = s->player.x, dz0 = s->player.z;
	vsp_set_op(40);
	vsp_cmd_lethal(40);
	ticks(s, 1);
	const double dx0d = s->player.has_death_pos ? s->player.death_x : dx0;
	const double dz0d = s->player.has_death_pos ? s->player.death_z : dz0;
	/* the drops materialize at the real Respawn (EntityPlayer.onDeath ->
	 * dropAllItems at the death point; server_local respawn transaction) */
	struct server_rpc respawn = {.type = SRPC_RESPAWN, .payload.respawn = {.owner = 0}};
	host_send(s, &respawn);
	double maxd = 0.0, maxflight = 0.0;
	unsigned n = 0;
	for(unsigned t = 0; t < 200; t++) {
		ticks(s, 1);
		dict_entity_it_t it;
		for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
			dict_entity_next(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			if(!e || e->type != ENTITY_ITEM || e->delay_destroy == 0)
				continue;
			double ddx = (double)e->pos[0] - dx0d, ddz = (double)e->pos[2] - dz0d;
			double d = sqrt(ddx * ddx + ddz * ddz);
			if(d > maxflight)
				maxflight = d;
		}
	}
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(!e || e->type != ENTITY_ITEM || e->delay_destroy == 0)
			continue;
		double ddx = (double)e->pos[0] - dx0d, ddz = (double)e->pos[2] - dz0d;
		double d = sqrt(ddx * ddx + ddz * ddz);
		if(d > maxd)
			maxd = d;
		n++;
	}
	printf("DROP_MEASURE kind=%d seed=%u n=%u rest_max=%.3f flight_max=%.3f\n",
		   kind, (unsigned)seed, n, maxd, maxflight);
	teardown(s);
}

/* ===== G6 (O13): the PRODUCTION host Respawn handshake's death drops ======
 * SRPC_RESPAWN -> the host loading handshake (not the legacy respawn
 * request): dropAllItems order, Beta spawn offset, death scatter from the
 * player rand (s->rand_src), delay 40, conservation. */
static void case_death_scatter_host(void) {
	struct server_local* s = rig(VSP_BOAT);
	assert(rig_ready(s));
	capture();
	struct inventory* inv = &s->player.inventory;
	for(size_t i = 0; i < INVENTORY_SIZE; i++)
		inventory_clear_slot(inv, i);
	/* diamond: the lethal hit's armor damage cannot destroy it */
	inventory_set_slot(inv, INVENTORY_SLOT_ARMOR + 0, (struct item_data) {.id = ITEM_DIAMOND_HELMET, .count = 1});
	inventory_set_slot(inv, INVENTORY_SLOT_MAIN + 4, (struct item_data) {.id = BLOCK_DIRT, .count = 9});
	inventory_set_slot(inv, INVENTORY_SLOT_HOTBAR + 7, (struct item_data) {.id = ITEM_APPLE, .count = 2});
	static const uint16_t want_ids[3] = {ITEM_APPLE, BLOCK_DIRT, ITEM_DIAMOND_HELMET};
	host_mount(s, 10, VSP_BOAT); /* the disclosed lethal fixture kills MOUNTED */
	ticks(s, 1);
	vsp_set_op(40);
	vsp_cmd_lethal(40);
	ticks(s, 1);
	check(s->player.health <= 0 && s->player.has_death_pos, "G6 host: lethal committed");
	const double dx = s->player.death_x, dy = s->player.death_y, dz = s->player.death_z;
	/* item entities present before the Respawn are not death drops */
	uint32_t before_ids[64];
	size_t nb = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && nb < 64)
			before_ids[nb++] = e->id;
	}
	/* O16 (R06 P1): the host's OWN Random (seeded now, before the copy); the
	 * world stream must come out byte-exact. */
	struct java_random ref = *server_local_player_rand(s, &s->player); /* F4-RNG */
	const struct random_gen world_before = s->rand_src;
	struct server_rpc respawn = {.type = SRPC_RESPAWN, .payload.respawn = {.owner = 0}};
	host_send(s, &respawn);
	struct entity* got[8];
	size_t n = 0;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(!e || e->type != ENTITY_ITEM)
			continue;
		bool old = false;
		for(size_t k = 0; k < nb; k++)
			old = old || before_ids[k] == e->id;
		if(!old && n < 8)
			got[n++] = e;
	}
	for(size_t i = 1; i < n; i++)
		for(size_t j = i; j > 0 && got[j - 1]->id > got[j]->id; j--) {
			struct entity* t = got[j]; got[j] = got[j - 1]; got[j - 1] = t;
		}
	if(n != 3)
		printf("G6 host: %zu item entities spawned\n", n);
	check(n == 3, "G6 host: three death drops (empty slots skipped)");
	bool ok_order = true, ok_pos = true, ok_vel = true, ok_delay = true;
	const float want_y = (float)(dy - (double)0.3F + (double)0.12F);
	for(size_t i = 0; i < n && i < 3; i++) {
		struct entity* e = got[i];
		ok_order = ok_order && e->data.item.item.id == want_ids[i];
		ok_pos = ok_pos && e->pos[0] == (float)dx && e->pos[2] == (float)dz
			&& e->pos[1] == want_y;
		float f = jrand_next_float(&ref) * 0.5F;
		float a = jrand_next_float(&ref) * (float)M_PI * 2.0F;
		ok_vel = ok_vel && e->vel[0] == -mh_sin(a) * f
			&& e->vel[2] == mh_cos(a) * f && e->vel[1] == 0.2F;
		ok_delay = ok_delay && e->data.item.pickup_delay == 40;
	}
	check(ok_order, "G6 host: dropAllItems order hotbar -> main -> armor");
	check(ok_pos, "G6 host: spawn posY - 0.3F + 0.12F at the death point");
	check(ok_vel, "G6 host: Beta death scatter from the host's OWN Random");
	check(memcmp(&s->rand_src, &world_before, sizeof(world_before)) == 0,
		  "G6 host: the world stream (rand_src) is byte-exact across the death");
	check(s->player.entity_rand.r.seed == ref.seed,
		  "G6 host: the host Random advanced exactly 2 draws per drop");
	check(ok_delay, "G6 host: pickup delay 40");
	bool empty = true;
	for(size_t i = 0; i < INVENTORY_SIZE; i++) {
		struct item_data it;
		if(inventory_get_slot(inv, i, &it) && it.id)
			empty = false;
	}
	check(empty, "G6 host: inventory conserved into the drops");
	teardown(s);
}

/* CONTRACT-v1.1 Q3: the client ride census counts the REAL dispatcher
 * (entities_client_tick) calling the target's own tick_client, with an
 * independent eligibility census; zero and repeated calls in an eligible pass
 * are detected; follow is not involved. */
static unsigned stub_ticks;
static bool stub_tick(struct entity* e) {
	(void)e;
	stub_ticks++;
	e->pos[0] += 0.1f; /* the vehicle moves on its own tick */
	return false;
}
static void case_client_dispatch(void) {
	arm(VSP_BOAT);
	dict_entity_t d;
	dict_entity_init(d);
	static struct entity boat, other;
	memset(&boat, 0, sizeof(boat));
	memset(&other, 0, sizeof(other));
	boat.id = 77;
	boat.type = ENTITY_BOAT;
	boat.delay_destroy = -1;
	boat.data.boat.occupied = true;
	boat.data.boat.occupant_id = 5; /* the host body */
	boat.tick_client = stub_tick;
	other = boat;
	other.id = 78;
	other.data.boat.occupant_id = 9;
	dict_entity_set_at(d, boat.id, &boat);
	dict_entity_set_at(d, other.id, &other);
	struct vsp_client_counters cc;
	struct vsp_dispatch_tap t;

	/* Dormant: no window open -> no tap, no count, pointer untouched. */
	vsp_client_dispatch_begin_for(&t, d, d, 5);
	check(!t.eligible && boat.tick_client == stub_tick,
		"client_dispatch: no open window = no tap installed");
	entities_client_tick(d);
	vsp_client_dispatch_end(&t, d);
	vsp_client_counters(&cc);
	check(cc.ride_eligible == 0 && cc.ride_tick_calls == 0,
		"client_dispatch: nothing booked outside a window");

	vsp_client_counters_window_reset(false, true, boat.id);
	/* 3 normal passes: one real dispatch each. */
	stub_ticks = 0;
	for(unsigned i = 0; i < 3; i++) {
		vsp_client_dispatch_begin_for(&t, d, d, 5);
		entities_client_tick(d);
		vsp_client_dispatch_end(&t, d);
	}
	vsp_client_counters(&cc);
	check(cc.ride_eligible == 3 && cc.ride_tick_calls == 3 && cc.ride_missing == 0
		&& cc.ride_duplicate == 0 && stub_ticks == 6, /* boat + other each pass */
		"client_dispatch: one counted call per eligible pass of the real dispatcher");
	check(boat.tick_client == stub_tick && other.tick_client == stub_tick,
		"client_dispatch: the original tick_client is restored after each pass");
	check(cc.ride_disp_max > 0.15f && cc.host_follow_count == 0,
		"client_dispatch: per-pass displacement accumulates; follow stays separate");

	/* Missing: the dispatcher never reaches the target (no tick_client). */
	boat.tick_client = NULL;
	vsp_client_dispatch_begin_for(&t, d, d, 5);
	entities_client_tick(d);
	vsp_client_dispatch_end(&t, d);
	boat.tick_client = stub_tick;
	vsp_client_counters(&cc);
	check(cc.ride_eligible == 4 && cc.ride_tick_calls == 3 && cc.ride_missing == 1,
		"client_dispatch: an eligible pass with ZERO calls counts as missing");

	/* Duplicate: two dispatcher passes inside one eligible update. */
	vsp_client_dispatch_begin_for(&t, d, d, 5);
	entities_client_tick(d);
	entities_client_tick(d);
	vsp_client_dispatch_end(&t, d);
	vsp_client_counters(&cc);
	check(cc.ride_eligible == 5 && cc.ride_tick_calls == 5 && cc.ride_duplicate == 1
		&& cc.ride_calls_max == 2,
		"client_dispatch: two calls in one eligible pass count as duplicate");

	/* Not eligible: another view, or the target does not name the body. */
	vsp_client_dispatch_begin_for(&t, d, NULL, 5);
	entities_client_tick(d);
	vsp_client_dispatch_end(&t, d);
	vsp_client_dispatch_begin_for(&t, d, d, 6);
	entities_client_tick(d);
	vsp_client_dispatch_end(&t, d);
	vsp_client_counters(&cc);
	check(cc.ride_eligible == 5 && boat.tick_client == stub_tick,
		"client_dispatch: non-host views and non-rider bodies are not eligible");
	check(cc.ride_missing == 1 && cc.ride_duplicate == 1,
		"client_dispatch: missing/duplicate are sticky across the window");

	vsp_client_counters_window_reset(false, true, boat.id);
	vsp_client_counters(&cc);
	check(cc.ride_eligible == 0 && cc.ride_missing == 0 && cc.ride_duplicate == 0,
		"client_dispatch: a new window starts a fresh census");
	dict_entity_clear(d);
	vsp_reset();
}

/* CONTRACT-v1.1 Q5: each named actor carries ITS OWN input epoch on its
 * owner thread. The host epoch is the real handoff counter (server) / the
 * acknowledged clin_input_epoch (client); the guest has no input-epoch
 * handoff and reports 0 -- never the host allocator -- while its identity
 * moves in generation. Divergent host epochs and a guest rejoin. */
static const struct vsp_record* last_rig(void) {
	int i = -1;
	for(unsigned j = 0; j < cap_n; j++)
		if(cap[j].event == VSP_EV_RIG_READY)
			i = (int)j;
	return i >= 0 ? &cap[i] : NULL;
}
static void case_epochs(void) {
	struct server_local* s = rig(VSP_BOAT);
	assert(build_fixture(s, 1)); /* real targets, so rig_ready serializes */
	uint32_t e0 = s->input_epoch_counter;
	check(e0 > 0 && clin_input_epoch() == e0,
		"epochs: the host handoff epoch is live and acknowledged");
	vsp_srv_rig_ready(s, 1, true, true, true, true, true);
	capture();
	const struct vsp_record* r = last_rig();
	uint64_t gen0 = s->guest_generation;
	check(r && r->u.rig_ready.host.actor.input_epoch == e0
		&& r->u.rig_ready.guest.actor.input_epoch == 0
		&& r->u.rig_ready.guest.actor.generation == gen0 && gen0 != 0,
		"epochs: server host = its counter, guest = 0 with its own generation");
	struct vsp_c hc, gc;
	check(vsp_snap_client(&gstate.players[0], 0, &hc) && hc.actor.input_epoch == e0,
		"epochs: client host snapshot = the acknowledged host epoch");
	/* The harness's guest view has no bound payload world (the snapshot then
	 * refuses rather than inventing one); the epoch source itself is what
	 * every guest-side stamp (snapshot, ingress refusal, attempt, UI) uses. */
	check(vsp_client_actor_epoch(1) == 0 && vsp_client_actor_epoch(0) == e0,
		"epochs: client epoch source = host acknowledged epoch, guest 0");
	if(vsp_snap_client(&gstate.players[1], 1, &gc))
		check(gc.actor.input_epoch == 0
			&& gc.actor.generation == gstate.players[1].local_generation,
			"epochs: a bound guest snapshot = 0, generation kept separately");
	/* Divergence: the host epoch moves on its own; the guest's stays 0. */
	s->input_epoch_counter = e0 + 7;
	vsp_srv_rig_ready(s, 2, true, true, true, true, true);
	capture();
	r = last_rig();
	check(r && r->u.rig_ready.host.actor.input_epoch == e0 + 7
		&& r->u.rig_ready.guest.actor.input_epoch == 0,
		"epochs: a host handoff never leaks into the guest's epoch");
	s->input_epoch_counter = e0;
	/* Rejoin: new guest generation, still no host epoch on the guest. */
	guest_release(s);
	guest_attach(s);
	guest_body_home(s);
	assert(client_session_request_local_gameplay(&gstate.players[1]));
	pump_handshake(s);
	assert(s->guest_gameplay && s->guest_actor);
	vsp_srv_rig_ready(s, 3, true, true, true, true, true);
	capture();
	r = last_rig();
	check(r && r->u.rig_ready.guest.actor.generation == s->guest_generation
		&& s->guest_generation != gen0
		&& r->u.rig_ready.guest.actor.input_epoch == 0
		&& r->u.rig_ready.host.actor.input_epoch == e0,
		"epochs: rejoin = new guest generation, guest epoch 0, host epoch unchanged");
	check(cap_serialize_errors == 0, "epochs: every record serializes");
	vsp_reset();
	teardown(s);
}

/* O03 PRODUCTION BUG: while the HOST rides a cart with P2 present, the
 * GUEST client must author no cart control, no prediction and no ride
 * state. The real guest walk (entity_local_player_walk_owned -> follow_mounted) is
 * run against a cart carrying an unacknowledged control tuple; the real
 * slot-1 ingress refusal site (observed by the armed probe) counts every
 * SRPC_MINECART_CONTROL the guest put on its route. */
static unsigned slot1_cart_controls(unsigned from) {
	unsigned n = 0;
	for(unsigned i = from; i < cap_n; i++)
		n += cap[i].event == VSP_EV_ACTION_RESULT
			&& cap[i].u.result.site == VSP_SITE_INGRESS
			&& cap[i].u.result.rpc == VSP_RPC_MINECART_CONTROL
			&& cap[i].u.result.source.slot == 1;
	return n;
}
static struct entity* guest_view_entity(uint32_t id) {
	struct dict_entity_s* ents = world_view_entities();
	struct clin_world_payload payload;
	if(clin_world_payloads_enabled() && clin_world_payload_of_view(1, &payload))
		ents = payload.entities;
	struct entity** pp = ents ? dict_entity_get(ents, id) : NULL;
	return pp ? *pp : NULL;
}
static void case_cart_route(void) {
	struct server_local* s = rig(VSP_CART);
	assert(rig_ready(s));
	host_mount(s, 10, VSP_CART);
	ticks(s, 3);
	clin_update_drain();
	check(host_on(s, VSP_CART), "cart_route: the host rides the cart (real ride intent)");
	struct client_session* g = &gstate.players[1];
	struct entity* body = g->local_player;
	struct entity* cart = guest_view_entity(target(VSP_CART));
	check(body && cart && cart->type == ENTITY_MINECART,
		"cart_route: the guest client sees the host-ridden cart");
	if(!body || !cart) {
		vsp_reset();
		teardown(s);
		return;
	}
	/* The host's unacknowledged cumulative tuple, as the client holds it. */
	cart->data.minecart.occupied = true;
	cart->data.minecart.occupant_id = 0; /* the host body */
	cart->data.minecart.control_ready = true;
	cart->data.minecart.control_dirty = true;
	cart->data.minecart.ride_epoch = cart->data.minecart.ride_epoch
		? cart->data.minecart.ride_epoch : 1;
	struct input_player_frame gf;
	memset(&gf, 0, sizeof(gf));
	gf.connected = true; /* P2's own connected, neutral owned frame */
	capture();
	unsigned base = cap_n;
	for(unsigned i = 0; i < 5; i++)
		(void)entity_local_player_walk_owned(body, g, &gf); /* main.c's guest path */
	server_local_test_process_messages(s);
	ticks(s, 1);
	capture();
	check(slot1_cart_controls(base) == 0,
		"cart_route: zero slot-1 MINECART_CONTROL while the HOST rides");
	check(!g->ride.pending && !g->ride.send_dirty && g->ride.target_id == 0
		&& !g->ride.desired_occupied,
		"cart_route: no guest ride state or prediction");
	check(cart->data.minecart.occupant_id == 0 && cart->data.minecart.occupied,
		"cart_route: the guest tick never re-predicts the host's cart");
	/* O04 / D1 with P2 present: the host ride-action latch is player 0's
	 * alone. The per-frame hook never latches for the guest, P2's OWN JUMP
	 * edge on its owned frame authors nothing against the host's cart, and
	 * a guest tick never consumes the host's pending latch. */
	{
		struct minecart_ride_client_state host_ride = gstate.players[0].ride;
		gstate.players[0].ride.action_latched = true;
		gstate.players[0].ride.action_cart_id = cart->id;
		entity_local_player_ride_action_frame();
		struct input_player_frame jf = gf;
		jf.pressed[IB_JUMP] = jf.down[IB_JUMP] = true;
		unsigned b2 = cap_n;
		(void)entity_local_player_walk_owned(body, g, &jf);
		server_local_test_process_messages(s);
		ticks(s, 1);
		capture();
		check(!g->ride.action_latched && !g->ride.pending && !g->ride.send_dirty
			&& g->ride.target_id == 0,
			"cart_route: D1 -- P2's own JUMP edge authors no ride state and no latch for P2");
		check(slot1_cart_controls(b2) == 0
			&& cart->data.minecart.occupied && cart->data.minecart.occupant_id == 0,
			"cart_route: D1 -- P2's JUMP never dismounts or re-predicts the host's cart");
		gstate.players[0].ride = host_ride;
		gstate.players[0].ride.action_latched = true;
		gstate.players[0].ride.action_cart_id = cart->id;
		(void)entity_local_player_walk_owned(body, g, &gf);
		check(gstate.players[0].ride.action_latched,
			"cart_route: D1 -- a guest tick never consumes the host's latch");
		gstate.players[0].ride = host_ride;
	}
	/* Misroute negative: a tuple the GUEST's own ride transaction names IS
	 * resent on the guest route -- and the production slot-1 ingress
	 * refuses it -- so the observer would have caught the leak above. */
	g->ride.target_id = cart->id;
	base = cap_n;
	(void)entity_local_player_walk_owned(body, g, &gf);
	server_local_test_process_messages(s);
	ticks(s, 1);
	capture();
	check(slot1_cart_controls(base) >= 1,
		"cart_route: misroute negative -- a guest-owned dirty tuple reaches slot-1 ingress and is refused");
	g->ride.target_id = 0;
	/* O04: the two FIFOs merge in PUSH order. A main row (an attempt) pushed
	 * before a server row (its authority result) must pop first even when
	 * both are pending in one emit pass; draining the server FIFO first was
	 * the PC-tape vehicle_action_correlation (authority seq < attempt seq). */
	{
		struct vsp_record r;
		while(vsp_pop(&r))
			;
		memset(&r, 0, sizeof(r));
		r.event = VSP_EV_TITLE;
		check(vsp_main_push(&r), "fifo order: main row A queued");
		vsp_srv_respawn_stage(s, VSP_RS_RELEASED);
		r.event = VSP_EV_TITLE;
		check(vsp_main_push(&r), "fifo order: main row B queued");
		uint8_t got[3] = {0, 0, 0};
		unsigned n = 0;
		while(n < 3 && vsp_pop(&r))
			got[n++] = r.event;
		check(n == 3 && got[0] == VSP_EV_TITLE && got[1] == VSP_EV_RESPAWN
			&& got[2] == VSP_EV_TITLE,
			"fifo order: main A, server row, main B pop in push order");
	}
	vsp_reset();
	teardown(s);
}

/* hw_run: the kind is required for the profile and refused elsewhere. */
static void case_hw_run(void) {
	struct hw_run_config cfg;
	char err[160];
	char* a1[] = {"x", "--cw-tel-host", "h", "--cw-profile", "host_vehicle_smoke_8m"};
	check(!hw_run_parse_args(&cfg, 5, a1, err, sizeof(err)) && strstr(err, "--cw-vehicle-kind"),
		"hw_run: host_vehicle_smoke_8m without a kind is a parse error");
	char* a2[] = {"x", "--cw-tel-host", "h", "--cw-vehicle-kind", "boat",
		"--cw-profile", "far_idle_2m"};
	check(!hw_run_parse_args(&cfg, 7, a2, err, sizeof(err)),
		"hw_run: a kind on any other profile is a parse error");
	char* a3[] = {"x", "--cw-tel-host", "h", "--cw-vehicle-kind", "pig",
		"--cw-profile", "host_vehicle_smoke_8m", "--cw-vehicle-transfer", "contact"};
	check(hw_run_parse_args(&cfg, 9, a3, err, sizeof(err))
		&& cfg.profile == HW_RUN_PROFILE_HOST_VEHICLE_SMOKE_8M
		&& cfg.vehicle_kind == HW_RUN_VEHICLE_PIG
		&& cfg.vehicle_transfer == HW_RUN_VEHICLE_TRANSFER_CONTACT,
		"hw_run: kind/transfer parse in either argv order");
	check(cfg.vehicle_smoke && cfg.split_guest && !cfg.split_guest_walk && cfg.superflat
		&& cfg.force_overworld && !cfg.autoload_new && cfg.driver_owns_completion
		&& cfg.seed_set && cfg.seed == 12345 && cfg.duration_seconds == 500,
		"hw_run: profile = split guest, superflat Overworld, seed 12345, driver-owned");
	check(strcmp(hw_run_profile_name(HW_RUN_PROFILE_HOST_VEHICLE_SMOKE_8M),
				 "host_vehicle_smoke_8m") == 0
		&& strcmp(hw_run_vehicle_kind_name(HW_RUN_VEHICLE_CART), "cart") == 0,
		"hw_run: canonical names");
	char* a4[] = {"x", "--cw-tel-host", "h", "--cw-profile", "host_vehicle_smoke_8m",
		"--cw-vehicle-kind", "horse"};
	check(!hw_run_parse_args(&cfg, 7, a4, err, sizeof(err)),
		"hw_run: an unknown kind is refused");
	char* a5[] = {"x", "--cw-tel-host", "h", "--cw-vehicle-kind", "boat",
		"--cw-profile", "host_vehicle_smoke_8m", "--cw-profile", "split_play_8m"};
	check(!hw_run_parse_args(&cfg, 9, a5, err, sizeof(err)),
		"hw_run: a later --cw-profile does not inherit the vehicle arm");
}

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "";
	harness_init();
	if(!strcmp(which, "serialize")) case_serialize();
	else if(!strcmp(which, "fifo")) case_fifo();
	else if(!strcmp(which, "cart_route")) case_cart_route();
	else if(!strcmp(which, "epochs")) case_epochs();
	else if(!strcmp(which, "client_dispatch")) case_client_dispatch();
	else if(!strcmp(which, "fixture")) case_fixture();
	else if(!strcmp(which, "ride") && argc > 2) case_ride((enum vsp_kind)atoi(argv[2]));
	else if(!strcmp(which, "switch") && argc > 2) case_switch((enum vsp_kind)atoi(argv[2]));
	else if(!strcmp(which, "guest-probes")) case_guest_probes();
	else if(!strcmp(which, "misroute")) case_misroute();
	else if(!strcmp(which, "death")) case_death();
	else if(!strcmp(which, "pending-transfer")) case_pending_transfer();
	else if(!strcmp(which, "save")) case_save();
	else if(!strcmp(which, "dispatch-seam")) case_dispatch_seam();
	else if(!strcmp(which, "dispatch-guest")) case_dispatch_guest();
	else if(!strcmp(which, "pickup-negative")) case_pickup_negative();
	else if(!strcmp(which, "death-scatter-host")) case_death_scatter_host();
	else if(!strcmp(which, "hull-aim")) case_hull_aim();
	else if(!strcmp(which, "boat-yaw")) case_boat_window_yaw();
	else if(!strcmp(which, "corner-guard")) case_corner_guard();
	else if(!strcmp(which, "two-home") && argc > 2) case_two_home(argv[2]);
	else if(!strcmp(which, "drop-measure") && argc > 3)
		case_drop_measure(atoi(argv[2]), (uint32_t)strtoul(argv[3], NULL, 10));
	else if(!strcmp(which, "dormant")) case_dormant(argv[0]);
	else if(!strcmp(which, "dormant-run") && argc > 2)
		(void)dormant_run(!strcmp(argv[2], "on"));
	else if(!strcmp(which, "driver")) {
		case_driver();
		case_driver_telemetry_lost();
	}
	else if(!strcmp(which, "hw-run")) case_hw_run();
	else {
		printf("unknown case '%s'\n", which);
		return 2;
	}
	return harness_result("test_vehicle_smoke_script", which);
}
