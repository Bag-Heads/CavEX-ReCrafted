/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-T -> A2-W (split-dimensions-plan-0924; astra-next-0926 row A6; Astra A2
 * design sec. 1-2): the actor transfer transaction and the two-world lifecycle,
 * one case per process (argv[1]) on the REAL admission harness (host + admitted
 * guest body, both in the Overworld):
 *
 *   host_to_nether    Overworld -> Nether with one player per world: the host's
 *                     request COMMITs; server_local_world_of(host) is the LIVE
 *                     Nether world (slot 1), the guest stays committed to slot 0;
 *                     both worlds LIVE; the host's pose is the 8:1 scaled one and
 *                     its inventory is intact; the guest is untouched.
 *   rollback          a fault mid-PREPARE (portal search/build fails) ROLLs BACK:
 *                     the host is exactly where it was (world, pose, inventory,
 *                     membership), the partner is untouched, the source world's
 *                     chunks are intact; a failed ADMISSION refuses before any
 *                     staging. Nothing of the destination is exposed.
 *   opposite          host -> Nether while the guest -> Overworld (the guest first
 *                     travelled to the Nether): both requests in one tick are
 *                     serialized Overworld-source first, both COMMIT, each actor
 *                     ends in the other's former world, both worlds stay LIVE.
 *   duplicate         a second request for the same open actor answers DUPLICATE
 *                     with the same id; a request into the actor's own world is
 *                     REFUSED_SAME_WORLD; nothing is staged twice.
 *   once_only         with two LIVE worlds, N updates run the world tick, the
 *                     weather pass and the spawn pass exactly N times PER WORLD
 *                     (Overworld then Nether) and the session sleep tick N times.
 *   death_respawn     the host dies in the Nether: its drops stay in the Nether
 *                     world; the respawn transaction commits it to the Overworld
 *                     (no bed -> world spawn) and the Nether world stays LIVE
 *                     while the guest remains there... or retires when empty.
 *   reload            after the host commits to the Nether, the level state
 *                     records Player.Dimension -1 and the guest record 0; a
 *                     release + load admits the host into the Nether (slot for
 *                     dimension -1 LIVE) and the guest admission resolves the
 *                     Overworld (C3 need #1: server_local_world_for_dimension).
 *   reload_reopen     Astra a2t-0927: REAL save / reopen / admission in both
 *                     arrangements. (A) host committed to the Nether: save,
 *                     release everything, reopen -> the routed world loads AS
 *                     the Nether (Player.Dimension -1, World.java:186-189), the
 *                     host stands in it, the Overworld admits LIVE beside it;
 *                     (B) host in the Overworld: reopen -> Overworld routed,
 *                     the Nether admits. Login never manufactures a portal.
 *   memory_refusal    Astra a2t-0927: the carve budget below the 1 MiB floor
 *                     refuses admission VISIBLY (HUD toast); level.dat bytes,
 *                     the host pose/inventory and slot 1 are unchanged.
 *   capacity_landing  the REAL admission path (lane B contract a33d10f2), driven
 *                     server-side by a scripted client: the transaction sends
 *                     CAPACITY_REQUEST RESERVE for the destination slot, waits;
 *                     on SRPC_CAPACITY_REPLY OK it arms the landing
 *                     (CRPC_LANDING_ARM at the staged pose, the split radius),
 *                     waits; on SRPC_LANDING_READY for that slot+generation it
 *                     COMMITs and sends CAPACITY_REQUEST COMMIT with the ticket.
 *   landing_timeout   the reply never comes: the bounded wait rolls back,
 *                     cancels the armed landing and RELEASEs the ticket; the
 *                     host is untouched in the source.
 *   landing_stale     a READY for another generation is ignored (the wait goes
 *                     on); the right one commits.
 *   retired_freed     A2-E F1: monsters spawned into a LIVE slot-1 world are
 *                     freed when it retires (dict_entity_size back to the
 *                     baseline, no entity resolves to the retired generation,
 *                     its sections reset); retire is REFUSED while an actor is
 *                     committed to it.
 *   retire_checkpoint_fail   Fable A2T-SERVER-REVIEW H1: a retire (or session
 *                     release) whose checkpoint fails RETAINS the world -- LIVE,
 *                     resident, every entity -- and reports false; it retires
 *                     once the checkpoint succeeds.
 *   retire_persists_entities H2: the retire checkpoints BEFORE freeing the
 *                     entities, so re-admitting the dimension restores exactly
 *                     them (bound to the new generation), and the other world's
 *                     same-column entities are never written into its file.
 *
 *   landing_real      gap-doc blocker 3, the REAL client (world-bound payloads
 *                     ON, default senders -- NO faked reply): the host goes
 *                     Overworld -> Nether -> Overworld. Each leg waits in
 *                     LANDING; the server streams the 3x3 arrival core stamped
 *                     with the destination world; the client prepares an
 *                     unviewed context on the ARM, installs the core and
 *                     reports READY by itself; the transfer COMMITs and view 0
 *                     joins the prepared context -- the core columns are still
 *                     there after the host's WORLD_RESET (not rebuilt empty).
 *   guest_landing_real the GUEST Overworld -> Nether: the client's own READY
 *                     ends the landing wait with the core installed in the
 *                     unviewed prepared slot. The guest COMMITs and view 1
 *                     binds it. The return to the host's existing Overworld
 *                     COMMITs into its installed core without another landing.
 *   same_world_back_to_back  lane C's dwell_host_busy, REAL client: the host
 *                     and then (two ticks later) the guest request the Nether.
 *                     The host COMMITs; the guest joins that already installed
 *                     world and COMMITs without a redundant landing handshake.
 *
 * All cases are RED by construction until SERVER_TRANSFER_API exists
 * (server_transfer.h); registered WILL_FAIL per case. */
#include "support/owner_swap_harness.h"

#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "network/server_transfer.h"
#include "network/portal_teleporter.h"
#include "network/client_admission.h"
#include "network/region_archive.h"
#include "cNBT/nbt.h"
#include "network/client_world_payload.h"
#include "game/client_world_context.h"

#include <inttypes.h>

#ifndef SERVER_TRANSFER_API
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("RED: SERVER_TRANSFER_API absent -- no actor transfer transaction / two-"
		   "world lifecycle yet (case %s)\n", argc > 1 ? argv[1] : "?");
	return 1;
}
#else

#define TEST_NAME "test_a2_transfer_transaction"
#define MAX_STEPS 4000

static const struct item_data mark_item = {.id = ITEM_DIAMOND, .count = 4};

static struct server_world* world_of(struct server_local* s,
									 struct server_player* p) {
	return server_local_world_of(s, p);
}

/* Drive the transaction to a terminal state through REAL server updates (the
 * update calls server_transfer_step once per tick). */
static enum server_transfer_state drive(struct server_local* s, uint32_t id) {
	/* the harness leaves the server paused (transport only); a paused game is
	 * frozen, so the transaction is driven through unpaused real updates */
	bool was_paused = s->paused;
	s->paused = false;
	enum server_transfer_state st = server_transfer_state_of(s, id);
	for(unsigned i = 0; i < MAX_STEPS; i++) {
		st = server_transfer_state_of(s, id);
		if(st == SERVER_TRANSFER_COMMITTED || st == SERVER_TRANSFER_ROLLED_BACK)
			break;
		tick(s);
	}
	s->paused = was_paused;
	return st;
}

static uint64_t inventory_hash(const struct server_player* p) {
	uint64_t h = UINT64_C(14695981039346656037);
	for(int i = 0; i < INVENTORY_SIZE; i++) {
		const struct item_data* it = &p->inventory.items[i];
		h ^= (uint64_t)it->id;
		h *= UINT64_C(1099511628211);
		h ^= (uint64_t)it->count;
		h *= UINT64_C(1099511628211);
		h ^= (uint64_t)it->durability;
		h *= UINT64_C(1099511628211);
	}
	return h;
}

/* The split fixture never binds the session save root (it opens its level
 * archive and world directly); a second world writes its files as siblings
 * of slot 0 under that root, so every case binds it first. */
static struct server_local* two_world_fixture(void) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	return s;
}

/* The host travels to the Nether and the request commits; shared setup. */
static uint32_t host_travels(struct server_local* s) {
	uint32_t id = 0;
	enum server_transfer_rc rc
		= server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id);
	check(rc == SERVER_TRANSFER_OK, "host -> Nether: the request is accepted");
	check(drive(s, id) == SERVER_TRANSFER_COMMITTED, "host -> Nether: COMMITTED");
	return id;
}

/* ======================================================================== */
static void case_host_to_nether(void) {
	struct server_local* s = two_world_fixture();
	s->player.inventory.items[INVENTORY_SLOT_MAIN] = mark_item;
	uint64_t host_inv = inventory_hash(&s->player);
	uint64_t guest_inv = inventory_hash(s->guest_actor);
	double gx = s->guest_actor->x, gz = s->guest_actor->z;
	double hx = s->player.x, hz = s->player.z;
	struct server_world* source = world_of(s, &s->player);
	check(source == &s->worlds[0] && world_of(s, s->guest_actor) == source,
		  "fixture: both actors committed to the Overworld (slot 0)");

	uint32_t id = host_travels(s);
	const struct server_actor_transition* tx = server_transfer_outcome(s, id);
	check(tx && tx->phase == SERVER_TRANSITION_COMMIT
			  && tx->actor.slot == ACTOR_SLOT_HOST
			  && tx->destination_dimension == WORLD_DIM_NETHER,
		  "the published transition is a COMMIT for the host into the Nether");
	struct server_world* hw = world_of(s, &s->player);
	check(hw && hw != &s->worlds[0] && hw->dimension == WORLD_DIM_NETHER
			  && server_local_world_live(hw),
		  "server_local_world_of(host) is the LIVE Nether world (slot 1)");
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_LIVE
			  && server_local_world_slot_state(s, 0) == SERVER_WORLD_SLOT_LIVE,
		  "both worlds are LIVE: one player per world");
	check(world_of(s, s->guest_actor) == &s->worlds[0]
			  && s->guest_actor->dimension == WORLD_DIM_OVERWORLD,
		  "the guest stays committed to the Overworld");
	check(s->player.dimension == WORLD_DIM_NETHER, "the host record says Nether");
	check(fabs(s->player.x - hx / 8.0) < 40.0 && fabs(s->player.z - hz / 8.0) < 40.0,
		  "the host pose is the 8:1 scaled one (within the portal search square)");
	check(inventory_hash(&s->player) == host_inv, "the host inventory is intact");
	check(inventory_hash(s->guest_actor) == guest_inv && s->guest_actor->x == gx
			  && s->guest_actor->z == gz,
		  "the partner's inventory and pose are untouched");
	check(!server_transfer_active(s) && s->world_switch_phase == SERVER_WORLD_SWITCH_IDLE,
		  "no transaction is open afterwards");
	/* Beta Entity.moveEntity:461-474 reads the moving actor's worldObj;
	 * BlockPortal.onEntityCollidedWithBlock:142-147 then arms that actor.
	 * Give the Nether host two opposite, same-coordinate block arrangements.
	 * Neither arrangement depends on the guest's Overworld membership. */
	w_coord_t px = (w_coord_t)floor(s->player.x);
	w_coord_t py = (w_coord_t)floor(s->player.y - PLAYER_EYE_HEIGHT + 0.001);
	w_coord_t pz = (w_coord_t)floor(s->player.z);
	for(w_coord_t y = py; y <= py + 2; y++) {
		struct block_data b;
		check(server_world_get_block(source, px, y, pz, &b),
			"host contact: Overworld sample is readable");
		if(b.type == BLOCK_PORTAL)
			check(server_world_set_block(source, px, y, pz,
				(struct block_data){.type = BLOCK_AIR}),
				"host contact: clear Overworld portal at actor pose");
		check(server_world_get_block(hw, px, y, pz, &b),
			"host contact: Nether sample is readable");
		if(b.type == BLOCK_PORTAL)
			check(server_world_set_block(hw, px, y, pz,
				(struct block_data){.type = BLOCK_AIR}),
				"host contact: clear Nether portal at actor pose");
	}
	check(server_world_set_block(hw, px, py, pz,
		(struct block_data){.type = BLOCK_PORTAL}),
		"host contact: portal exists only in committed Nether");
	s->player.in_portal = false;
	s->player.time_in_portal = 0.0F;
	s->player.time_until_portal = 0;
	s->player.portal_travel_requested = false;
	s->paused = false;
	tick(s);
	check(s->player.in_portal,
		"Nether host touches Nether-only portal, not empty Overworld cell");
	check(server_world_set_block(hw, px, py, pz,
		(struct block_data){.type = BLOCK_AIR})
		&& server_world_set_block(source, px, py, pz,
			(struct block_data){.type = BLOCK_PORTAL}),
		"host contact: reverse portal placement between committed worlds");
	s->player.in_portal = false;
	s->player.time_in_portal = 0.0F;
	s->player.time_until_portal = 0;
	s->player.portal_travel_requested = false;
	tick(s);
	check(!s->player.in_portal,
		"Nether host ignores Overworld-only portal at the same position");
	s->paused = true;
	teardown(s);
}

/* ======================================================================== */
static void case_rollback(void) {
	/* A reused session must not publish the previous run's sticky reasons. */
	struct server_local* fresh = calloc(1, sizeof(*fresh));
	check(fresh != NULL, "rollback diagnostic fixture allocated");
	fresh->transfer_rollback_id[0] = 11;
	fresh->transfer_rollback_id[1] = 12;
	fresh->transfer_rollback_reason[0] = SERVER_TRANSFER_ROLLBACK_CHECKPOINT;
	fresh->transfer_rollback_reason[1] = SERVER_TRANSFER_ROLLBACK_TELEPORTER_FAILED;
	server_transfer_init(fresh);
	struct server_transfer_budget_snapshot snap;
	check(server_transfer_budget_snapshot_get(&snap)
			  && !snap.rollback_id[0] && !snap.rollback_id[1]
			  && !snap.rollback_reason[0] && !snap.rollback_reason[1],
		  "session init clears stale rollback diagnostics before publishing");
	free(fresh);
	struct server_local* s = two_world_fixture();
	s->player.inventory.items[INVENTORY_SLOT_MAIN] = mark_item;
	uint64_t host_inv = inventory_hash(&s->player);
	double hx = s->player.x, hy = s->player.y, hz = s->player.z;
	uint32_t source_generation = s->worlds[0].generation;
	uint8_t under = block_at(s, 0, 3, 0);

	/* (a) admission refused: nothing staged, nothing exposed */
	server_transfer_test_fault(SERVER_TRANSFER_FAULT_ADMISSION);
	uint32_t id = 0;
	enum server_transfer_rc rc
		= server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id);
	check(rc == SERVER_TRANSFER_REFUSED_ADMISSION,
		  "(a) a destination that cannot be admitted REFUSES the request");
	check(!server_transfer_active(s)
			  && server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY,
		  "(a) nothing is open and slot 1 stays EMPTY (no partial world exposed)");

	/* (b) failure mid-PREPARE: the portal search/build fails -> ROLLBACK */
	server_transfer_test_fault(SERVER_TRANSFER_FAULT_TELEPORTER);
	rc = server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id);
	check(rc == SERVER_TRANSFER_OK, "(b) the request is accepted");
	enum server_transfer_state st = drive(s, id);
	check(st == SERVER_TRANSFER_ROLLED_BACK, "(b) the transaction ROLLED BACK");
	check(server_transfer_budget_snapshot_get(&snap)
			  && snap.rollback_id[ACTOR_SLOT_HOST] == id
			  && snap.rollback_reason[ACTOR_SLOT_HOST]
				  == SERVER_TRANSFER_ROLLBACK_TELEPORTER_FAULT,
		  "(b) host rollback reason and request survive the closed transaction");
	const struct server_actor_transition* tx = server_transfer_outcome(s, id);
	check(tx && tx->phase == SERVER_TRANSITION_ROLLBACK
			  && tx->actor.slot == ACTOR_SLOT_HOST,
		  "(b) the published transition is a ROLLBACK for the host");
	check(world_of(s, &s->player) == &s->worlds[0]
			  && s->player.dimension == WORLD_DIM_OVERWORLD,
		  "(b) the host is still a member of the source world");
	check(s->player.x == hx && s->player.y == hy && s->player.z == hz,
		  "(b) the host pose is exactly the source pose");
	check(inventory_hash(&s->player) == host_inv, "(b) the host inventory is intact");
	check(s->worlds[0].generation == source_generation
			  && server_world_is_chunk_loaded(&s->worlds[0], 0, 0)
			  && block_at(s, 0, 3, 0) == under,
		  "(b) the source world is intact (same instance, home column resident, "
		  "terrain unchanged -- nothing was torn down)");
	check(world_of(s, s->guest_actor) == &s->worlds[0] && s->guest_actor->finished_loading,
		  "(b) the partner is untouched");
	check(!server_transfer_active(s), "(b) nothing is left open");
	check(server_local_world_slot_state(s, 1) != SERVER_WORLD_SLOT_LOADING
			  && server_local_world_slot_state(s, 1) != SERVER_WORLD_SLOT_RETIRING,
		  "(b) the destination slot settled (EMPTY, or LIVE with no member)");
	server_transfer_test_fault(SERVER_TRANSFER_FAULT_TELEPORTER);
	uint32_t guest_id = 0;
	check(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &guest_id)
			  == SERVER_TRANSFER_OK,
		  "(b) the other actor can request after the host rollback");
	check(drive(s, guest_id) == SERVER_TRANSFER_ROLLED_BACK,
		  "(b) the other actor's injected portal failure rolls back");
	check(server_transfer_budget_snapshot_get(&snap)
			  && snap.rollback_id[ACTOR_SLOT_HOST] == id
			  && snap.rollback_reason[ACTOR_SLOT_HOST]
				  == SERVER_TRANSFER_ROLLBACK_TELEPORTER_FAULT
			  && snap.rollback_id[ACTOR_SLOT_GUEST] == guest_id
			  && snap.rollback_reason[ACTOR_SLOT_GUEST]
				  == SERVER_TRANSFER_ROLLBACK_TELEPORTER_FAULT,
		  "(b) each actor's closed rollback remains visible after the next request");

	/* (c) after a rollback the same actor can travel for real */
	server_transfer_test_fault(SERVER_TRANSFER_FAULT_NONE);
	host_travels(s);
	check(world_of(s, &s->player) && world_of(s, &s->player)->dimension == WORLD_DIM_NETHER,
		  "(c) a fresh request after the rollback commits normally");
	teardown(s);
}

/* ======================================================================== */
static void case_opposite(void) {
	struct server_local* s = two_world_fixture();
	/* the GUEST travels to the Nether first (one actor per world) */
	uint32_t gid = 0;
	check(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &gid)
			  == SERVER_TRANSFER_OK,
		  "setup: guest -> Nether accepted");
	check(drive(s, gid) == SERVER_TRANSFER_COMMITTED, "setup: guest -> Nether COMMITTED");
	struct server_world* nether = world_of(s, s->guest_actor);
	check(nether && nether->dimension == WORLD_DIM_NETHER
			  && world_of(s, &s->player) == &s->worlds[0],
		  "setup: guest in the Nether, host in the Overworld");

	/* opposite requests in the SAME tick: host -> Nether, guest -> Overworld */
	uint32_t hid = 0;
	gid = 0;
	enum server_transfer_rc r1
		= server_transfer_request(s, s->guest_actor, WORLD_DIM_OVERWORLD, &gid);
	enum server_transfer_rc r2
		= server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &hid);
	check(r1 == SERVER_TRANSFER_OK && r2 == SERVER_TRANSFER_OK && hid != gid,
		  "both opposite requests are accepted with distinct ids");
	check(server_transfer_active(s), "a transaction is open");
	enum server_transfer_state sh = drive(s, hid);
	enum server_transfer_state sg = drive(s, gid);
	check(sh == SERVER_TRANSFER_COMMITTED && sg == SERVER_TRANSFER_COMMITTED,
		  "both opposite transfers COMMIT");
	const struct server_actor_transition* th = server_transfer_outcome(s, hid);
	const struct server_actor_transition* tg = server_transfer_outcome(s, gid);
	check(th && tg && th->transition_epoch < tg->transition_epoch,
		  "serialization: the Overworld-source request (the host's) committed first");
	check(world_of(s, &s->player) == nether && s->player.dimension == WORLD_DIM_NETHER,
		  "the host ended in the Nether world the guest left");
	check(world_of(s, s->guest_actor) == &s->worlds[0]
			  && s->guest_actor->dimension == WORLD_DIM_OVERWORLD,
		  "the guest ended in the Overworld");
	check(server_local_world_live(&s->worlds[0]) && server_local_world_live(nether),
		  "both worlds stay LIVE across the swap");
	check(!server_transfer_active(s), "nothing is left open");
	teardown(s);
}

/* ======================================================================== */
static void case_duplicate(void) {
	struct server_local* s = two_world_fixture();
	uint32_t id = 0, again = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_OVERWORLD, &id)
			  == SERVER_TRANSFER_REFUSED_SAME_WORLD,
		  "a request into the actor's own world is REFUSED_SAME_WORLD");
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id)
			  == SERVER_TRANSFER_OK,
		  "the first request is accepted");
	enum server_transfer_rc rc
		= server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &again);
	check(rc == SERVER_TRANSFER_DUPLICATE && again == id,
		  "a second request for the same actor answers DUPLICATE with the same id");
	check(server_transfer_state_of(s, id) == SERVER_TRANSFER_REQUESTED
			  || server_transfer_state_of(s, id) == SERVER_TRANSFER_PREPARING,
		  "the original request is still the one open");
	check(drive(s, id) == SERVER_TRANSFER_COMMITTED, "it commits once");
	check(server_transfer_state_of(s, again) == SERVER_TRANSFER_COMMITTED,
		  "the duplicate id reports the existing outcome");
	teardown(s);
}

/* ======================================================================== */
static void case_once_only(void) {
	struct server_local* s = two_world_fixture();
	host_travels(s); /* two LIVE worlds, one actor each */
	s->paused = false;
	struct server_world_service_stats a0, b0, a1, b1;
	server_local_world_service_stats(s, 0, &a0);
	server_local_world_service_stats(s, 1, &b0);
	uint64_t sleep0 = server_local_session_sleep_ticks(s);
	uint32_t t0 = s->worlds[0].current_tick, t1 = s->worlds[1].current_tick;
	const unsigned N = 25;
	for(unsigned i = 0; i < N; i++)
		tick(s);
	server_local_world_service_stats(s, 0, &a1);
	server_local_world_service_stats(s, 1, &b1);
	printf("per world: ow ticks %" PRIu64 " weather %" PRIu64 " spawn %" PRIu64
		   " | nether ticks %" PRIu64 " weather %" PRIu64 " spawn %" PRIu64
		   " | sleep %" PRIu64 "\n",
		   a1.world_ticks - a0.world_ticks, a1.weather_ticks - a0.weather_ticks,
		   a1.spawn_passes - a0.spawn_passes, b1.world_ticks - b0.world_ticks,
		   b1.weather_ticks - b0.weather_ticks, b1.spawn_passes - b0.spawn_passes,
		   server_local_session_sleep_ticks(s) - sleep0);
	check(a1.world_ticks - a0.world_ticks == N && b1.world_ticks - b0.world_ticks == N,
		  "the world tick ran exactly once per world per update");
	check(s->worlds[0].current_tick == t0 + N && s->worlds[1].current_tick == t1 + N,
		  "independent world clocks each advanced N");
	check(a1.weather_ticks - a0.weather_ticks == N && b1.weather_ticks - b0.weather_ticks == N,
		  "the weather pass ran exactly once per world per update");
	/* the spawn pass keeps Beta's cycle gate (every MOB_SPAWN_CYCLE ticks):
	 * once per world on each of those ticks, never twice for one world */
	check(a1.spawn_passes - a0.spawn_passes == b1.spawn_passes - b0.spawn_passes
			  && a1.spawn_passes - a0.spawn_passes >= 1
			  && a1.spawn_passes - a0.spawn_passes <= N,
		  "the spawn pass ran once per world on each spawn-cycle tick");
	check(server_local_session_sleep_ticks(s) - sleep0 == N,
		  "the session sleep tick ran once per update (not once per world)");
	s->paused = true;
	teardown(s);
}

/* ======================================================================== */
static void case_death_respawn(void) {
	struct server_local* s = two_world_fixture();
	host_travels(s);
	struct server_world* nether = world_of(s, &s->player);
	check(nether && nether->dimension == WORLD_DIM_NETHER, "setup: host in the Nether");
	s->player.inventory.items[INVENTORY_SLOT_MAIN] = mark_item;
	s->player.has_bed_spawn = false;
	/* die in the Nether; the respawn request drops the inventory where the
	 * death happened (EntityPlayer.onDeath -> dropAllItems) */
	s->player.health = 0;
	check(server_local_respawn_request(s), "the respawn request is accepted");
	size_t items_in_nether = 0, items_in_ow = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(!e || e->type != ENTITY_ITEM)
			continue;
		struct server_world* w = server_local_entity_world(s, e);
		if(w == nether)
			items_in_nether++;
		else if(w == &s->worlds[0])
			items_in_ow++;
	}
	check(items_in_nether > 0 && items_in_ow == 0,
		  "the death-world drops stay in the Nether (never relocated to slot 0)");
	for(unsigned i = 0; i < MAX_STEPS && s->world_switch_phase != SERVER_WORLD_SWITCH_IDLE; i++)
		tick(s);
	check(s->world_switch_phase == SERVER_WORLD_SWITCH_IDLE && !server_transfer_active(s),
		  "the respawn transaction settled");
	check(world_of(s, &s->player) == &s->worlds[0]
			  && s->player.dimension == WORLD_DIM_OVERWORLD && s->player.health > 0,
		  "the host respawned into the Overworld (no bed -> world spawn)");
	check(world_of(s, s->guest_actor) == &s->worlds[0] && s->guest_actor->finished_loading,
		  "the guest is untouched by the host's death");
	enum server_world_slot_state ns = server_local_world_slot_state(s, 1);
	check(ns == SERVER_WORLD_SLOT_LIVE || ns == SERVER_WORLD_SLOT_EMPTY,
		  "the empty Nether either stays LIVE (design sec. 1) or retired cleanly");
	teardown(s);
}

/* ======================================================================== */
static void case_reload(void) {
	struct server_local* s = two_world_fixture();
	host_travels(s);
	nbt_node* hd = nbt_find_by_path(s->level.data, ".Data.Player.Dimension");
	check(hd && hd->payload.tag_int == -1, "level.dat Player.Dimension is -1 (Beta EntityPlayer.writeEntityToNBT:329)");
	/* The guest record (A2T-needs-from-B-C C#2, lane C a2t-guest-0927): the
	 * guest checkpoints its OWN dimension -- the dimension-equality stand-in
	 * is retired -- so the record says 0 while the host's says -1. */
	bool guest_ckpt = server_local_checkpoint_guests(s);
	nbt_node* gd = nbt_find_by_path(s->level.data, ".Data.CaveWiiGuest.Dimension");
	check(guest_ckpt && gd && gd->payload.tag_int == 0,
		  "the guest record checkpoints with Dimension 0 while the host's is -1");
	check(server_local_world_for_dimension(s, WORLD_DIM_OVERWORLD) == &s->worlds[0]
			  && server_local_world_for_dimension(s, WORLD_DIM_NETHER) == world_of(s, &s->player),
		  "C3 need #1: server_local_world_for_dimension answers a LIVE world for each dimension");
	teardown(s);
}

/* ======================================================================== */
static void case_retired_freed(void) {
	struct server_local* s = two_world_fixture();
	size_t baseline = dict_entity_size(s->entities);
	struct server_world* nether = server_local_world_admit(s, WORLD_DIM_NETHER);
	check(nether && nether == &s->worlds[1] && server_local_world_live(nether)
			  && server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_LIVE,
		  "admission: the Nether is LIVE in slot 1 with no member");
	check(server_local_world_admit(s, WORLD_DIM_NETHER) == nether,
		  "admission of a LIVE dimension answers the same world");
	/* populate one column and spawn monsters INTO the Nether world */
	struct server_chunk* c = NULL;
	for(unsigned n = 0; n < 1000 && !c; n++) {
		server_world_begin_update(nether);
		server_world_ensure_populated(nether, s, 0, 0, &c);
	}
	check(c != NULL, "fixture: a Nether column is populated");
	unsigned spawned = 0;
	for(int k = 0; k < 6; k++)
		spawned += server_local_spawn_monster_at_w(nether, (vec3) {0.5F + k, 100.0F, 0.5F},
												   MONSTER_PIG_ZOMBIE) != NULL;
	check(spawned == 6, "fixture: six monsters live in the Nether world");
	check(dict_entity_size(s->entities) == baseline + spawned, "they are in the entity dict");
	struct server_world_handle gen = server_world_handle_of(nether);

	/* the host travels there: retire must be REFUSED while it is a member */
	host_travels(s);
	check(!server_local_world_retire(s, nether) && server_local_world_live(nether),
		  "retire is REFUSED while an actor is committed to the world");
	/* the host travels back; the empty Nether can retire */
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_OVERWORLD, &id) == SERVER_TRANSFER_OK
			  && drive(s, id) == SERVER_TRANSFER_COMMITTED,
		  "the host returns to the Overworld");
	/* the unpaused ticks above may have spawned Overworld entities: what must
	 * be gone is every entity of the retired generation, nothing else */
	size_t slot0_before = 0;
	{
		dict_entity_it_t it0;
		for(dict_entity_it(it0, s->entities); !dict_entity_end_p(it0); dict_entity_next(it0)) {
			struct entity* e = dict_entity_ref(it0)->value;
			if(e && server_local_entity_world(s, e) == &s->worlds[0])
				slot0_before++;
		}
	}
	check(server_local_world_retire(s, nether), "the empty Nether retires");
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY, "slot 1 is EMPTY");
	printf("entities: baseline %zu, dict after retire %zu, slot-0 entities %zu\n", baseline,
		   dict_entity_size(s->entities), slot0_before);
	check(dict_entity_size(s->entities) == slot0_before,
		  "F1: the retired generation's entities are FREED (only slot-0 entities remain)");
	check(server_local_world_from_handle(s, gen) == NULL, "the retired generation no longer resolves");
	size_t stale = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->world_generation == gen.generation && e->world_slot == gen.world_index)
			stale++;
	}
	check(stale == 0, "no entity still carries the retired stamp");
	teardown(s);
}

/* ======================================================================== */
/* Fable A2T-SERVER-REVIEW H1/H2: a LIVE slot-1 Nether with six monsters in
 * its column (0,0) and two Overworld monsters in slot 0's column (0,0) -- the
 * same (x,z) in BOTH worlds so a checkpoint that gathers entities by chunk
 * coordinate alone would persist the Overworld pair into the Nether file. */
static struct server_world* populated_nether_with_monsters(struct server_local* s,
														   unsigned* nether_count,
														   unsigned* overworld_count) {
	struct server_world* nether = server_local_world_admit(s, WORLD_DIM_NETHER);
	check(nether && nether == &s->worlds[1] && server_local_world_live(nether),
		  "fixture: the Nether is LIVE in slot 1");
	struct server_chunk* c = NULL;
	for(unsigned n = 0; n < 1000 && !c; n++) {
		server_world_begin_update(nether);
		server_world_ensure_populated(nether, s, 0, 0, &c);
	}
	check(c != NULL, "fixture: Nether column (0,0) is populated");
	c = NULL;
	for(unsigned n = 0; n < 1000 && !c; n++) {
		server_world_begin_update(&s->worlds[0]);
		server_world_ensure_populated(&s->worlds[0], s, 0, 0, &c);
	}
	check(c != NULL, "fixture: Overworld column (0,0) is populated");
	*nether_count = 0;
	for(int k = 0; k < 6; k++)
		*nether_count += server_local_spawn_monster_at_w(
							 nether, (vec3) {0.5F + k, 100.0F, 0.5F}, MONSTER_PIG_ZOMBIE)
			!= NULL;
	*overworld_count = 0;
	for(int k = 0; k < 2; k++)
		*overworld_count += server_local_spawn_monster_at_w(
								&s->worlds[0], (vec3) {8.5F + k, 100.0F, 8.5F},
								MONSTER_ZOMBIE)
			!= NULL;
	check(*nether_count == 6 && *overworld_count == 2,
		  "fixture: six Nether monsters and two Overworld monsters share column (0,0)");
	return nether;
}

static size_t entities_in_world(struct server_local* s, const struct server_world* w) {
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && server_local_entity_world(s, e) == w)
			n++;
	}
	return n;
}

/* H1: a retire whose checkpoint fails must NOT discard the world -- it stays
 * LIVE and resident with every entity, and reports the refusal; the same for
 * the session release of the secondary slots. A later retire succeeds. */
static void case_retire_checkpoint_fail(void) {
	struct server_local* s = two_world_fixture();
	unsigned nether_n = 0, ow_n = 0;
	struct server_world* nether = populated_nether_with_monsters(s, &nether_n, &ow_n);
	struct server_world_handle gen = server_world_handle_of(nether);
	size_t dict_before = dict_entity_size(s->entities);
	size_t ow_before = entities_in_world(s, &s->worlds[0]);

	/* the world's column (0,0) holds entities, so its checkpoint gathers them:
	 * the next gather fails (the checkpoint's entity_gather stage) */
	server_local_test_fail_next_entity_gather();
	check(!server_local_world_retire(s, nether),
		  "H1: retire REPORTS the failed checkpoint (false)");
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_LIVE
			  && server_local_world_live(nether)
			  && server_local_world_from_handle(s, gen) == nether,
		  "H1: the world is RETAINED -- still LIVE, same generation");
	check(server_world_is_chunk_loaded(nether, 0, 0),
		  "H1: its resident column is still resident");
	check(dict_entity_size(s->entities) == dict_before
			  && entities_in_world(s, nether) == nether_n,
		  "H1: not one entity was freed by the refused retire");

	server_local_test_fail_next_entity_gather();
	check(!server_local_worlds_release_secondary(s),
		  "H1: the session release reports a failed secondary checkpoint");
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_LIVE
			  && server_local_world_live(nether) && server_world_is_chunk_loaded(nether, 0, 0)
			  && entities_in_world(s, nether) == nether_n,
		  "H1: the session release RETAINS a world it could not checkpoint");

	/* nothing armed: the retire now checkpoints and discards */
	check(server_local_world_retire(s, nether), "the retire succeeds once the checkpoint does");
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY
			  && server_local_world_from_handle(s, gen) == NULL,
		  "slot 1 is EMPTY, the generation gone");
	check(entities_in_world(s, &s->worlds[0]) == ow_before,
		  "the Overworld's entities are untouched throughout");
	teardown(s);
}

/* H2: the retire checkpoints BEFORE it frees the world's entities, so they are
 * on disk: re-admitting the dimension and loading the column brings exactly
 * them back (bound to the new generation) -- and only them: the Overworld's
 * same-column monsters were never written into the Nether's file. */
static void case_retire_persists_entities(void) {
	struct server_local* s = two_world_fixture();
	unsigned nether_n = 0, ow_n = 0;
	struct server_world* nether = populated_nether_with_monsters(s, &nether_n, &ow_n);
	size_t ow_before = entities_in_world(s, &s->worlds[0]);
	check(server_local_world_retire(s, nether), "the empty Nether retires (checkpoint ok)");
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY, "slot 1 is EMPTY");
	check(entities_in_world(s, &s->worlds[0]) == ow_before,
		  "the Overworld keeps its own entities (the retire freed only the Nether's)");

	struct server_world* again = server_local_world_admit(s, WORLD_DIM_NETHER);
	check(again && server_local_world_live(again), "the Nether re-admits LIVE");
	struct server_chunk* c = NULL;
	for(unsigned n = 0; n < 1000 && !c; n++) {
		server_world_begin_update(again);
		server_world_ensure_populated(again, s, 0, 0, &c);
	}
	check(c != NULL, "the saved Nether column (0,0) loads again");
	size_t restored = entities_in_world(s, again);
	unsigned pig_zombies = 0, others = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(!e || server_local_entity_world(s, e) != again)
			continue;
		if(e->type == ENTITY_MONSTER && e->data.monster.id == MONSTER_PIG_ZOMBIE)
			pig_zombies++;
		else
			others++;
	}
	printf("restored into the re-admitted Nether: %zu (pig zombies %u, others %u)\n",
		   restored, pig_zombies, others);
	check(pig_zombies == nether_n,
		  "H2: every monster of the retired Nether was CHECKPOINTED before it was freed");
	check(others == 0,
		  "H2: the Overworld's same-column monsters were NOT written into the Nether's file");
	check(entities_in_world(s, &s->worlds[0]) == ow_before,
		  "the Overworld's entity set is unchanged by the Nether reload");
	teardown(s);
}

/* H3: the PRODUCTION session release (save-and-quit / menu) covers every live
 * world: a secondary world that cannot checkpoint refuses the whole unload and
 * retains everything; a successful unload checkpoints and releases both worlds
 * and resets the host's committed slot; the save reopens. */
static void case_unload_secondary(void) {
	struct server_local* s = two_world_fixture();
	unsigned nether_n = 0, ow_n = 0;
	struct server_world* nether = populated_nether_with_monsters(s, &nether_n, &ow_n);
	struct server_world_handle gen = server_world_handle_of(nether);
	/* the harness guest is released by hand (its client half has no LEFT ack) */
	guest_release(s);
	/* The shared entity dictionary also holds Nether owners. Give one a
	 * genuinely resident Nether column absent from the Overworld, then use
	 * the production Save and quit path on both sides of the owner frontier. */
	const w_coord_t owner_cx = 30, owner_cz = 30;
	struct server_chunk* owner_chunk = NULL;
	check(!server_world_is_chunk_loaded(&s->worlds[0], owner_cx, owner_cz)
			&& server_world_load_chunk(nether, owner_cx, owner_cz, &owner_chunk)
			&& owner_chunk,
		  "H3: item owner column is resident only in the Nether");
	struct item_data owned_item = {.id = ITEM_STICK, .count = 1};
	struct entity* nether_item = server_local_spawn_item_w(nether,
		(vec3){owner_cx * CHUNK_SIZE + 1.5F, 100.0F,
			owner_cz * CHUNK_SIZE + 1.5F}, &owned_item, false);
	check(nether_item && server_local_entity_world(s, nether_item) == nether,
		  "H3: archivable item belongs to the live secondary generation");
	nether_item->teleport(nether_item,
		(vec3){(owner_cx + 1) * CHUNK_SIZE + 1.5F, 100.0F,
			(owner_cz + 1) * CHUNK_SIZE + 1.5F});
	check(!server_world_is_chunk_loaded(nether, owner_cx + 1, owner_cz + 1)
			&& !server_local_unload_world_impl(s, false),
		  "H3: Save and quit refuses a genuinely off-resident Nether owner");
	check(s->player.has_pos && server_local_world_from_handle(s, gen) == nether,
		  "H3: refused orphan save retains both worlds for retry");
	check(!server_world_is_chunk_loaded(nether, owner_cx + 1, owner_cz + 1),
		  "H3: refusing the orphan did not generate its missing owner column");
	nether_item->teleport(nether_item,
		(vec3){owner_cx * CHUNK_SIZE + 1.5F, 100.0F,
			owner_cz * CHUNK_SIZE + 1.5F});
	uint32_t live_generation = nether_item->world_generation;
	check(live_generation == gen.generation,
		  "H3: item carries the live Nether generation");
	nether_item->world_generation = live_generation + 1;
	check(!server_local_unload_world_impl(s, false)
			&& s->player.has_pos
			&& server_local_world_from_handle(s, gen) == nether,
		  "H3: Save and quit refuses a stale entity generation");
	nether_item->world_generation = live_generation;
	size_t dict_before = dict_entity_size(s->entities);

	server_local_test_fail_next_entity_gather();
	check(!server_local_unload_world_impl(s, false),
		  "H3: the production unload REFUSES when a secondary world cannot checkpoint");
	check(s->player.has_pos && server_local_world_live(&s->worlds[0]),
		  "H3: the routed world is retained (the host is still in game)");
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_LIVE
			  && server_local_world_from_handle(s, gen) == nether
			  && server_world_is_chunk_loaded(nether, 0, 0)
			  && dict_entity_size(s->entities) == dict_before,
		  "H3: the secondary world is retained too -- LIVE, resident, entities intact");

	bool unloaded = server_local_unload_world_impl(s, false);
	check(unloaded,
		  "H3: the unload succeeds once every world checkpoints");
	if(!unloaded) {
		teardown(s); /* Keep a RED refusal from cascading into a false reopen crash. */
		return;
	}
	check(!s->player.has_pos, "the routed world is released");
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY
			  && server_local_world_from_handle(s, gen) == NULL,
		  "H3: the secondary world is released by the PRODUCTION path (slot 1 EMPTY)");
	check(s->player.world_slot == 0, "H3: the host's committed slot is reset at release");
	check(dict_entity_size(s->entities) == 0, "no entity of either world survives the release");

	string_t name;
	string_init_set_str(name, split_path);
	check(server_local_load_world_impl(s, name), "the save reopens through the production load");
	string_clear(name);
	check(s->player.has_pos && s->player.world_slot == 0
			  && server_local_world_of(s, &s->player) == &s->worlds[0],
		  "the host is back in the routed world, slot 0");
	struct server_world* restored_nether
		= server_local_world_admit(s, WORLD_DIM_NETHER);
	struct server_chunk* restored_owner = NULL;
	check(restored_nether && server_world_load_chunk(restored_nether,
			owner_cx, owner_cz, &restored_owner) && restored_owner,
		  "H3: saved Nether owner column reopens through production load");
	unsigned restored_items = 0;
	dict_entity_it_t restored_it;
	for(dict_entity_it(restored_it, s->entities);
		!dict_entity_end_p(restored_it); dict_entity_next(restored_it)) {
		struct entity* e = dict_entity_ref(restored_it)->value;
		if(e && e->type == ENTITY_ITEM && e->data.item.item.id == ITEM_STICK
			&& e->data.item.item.count == 1
			&& server_local_entity_world(s, e) == restored_nether
			&& (w_coord_t)floor((double)e->pos[0] / CHUNK_SIZE) == owner_cx
			&& (w_coord_t)floor((double)e->pos[2] / CHUNK_SIZE) == owner_cz)
			restored_items++;
	}
	/* Chunk reload assigns a new runtime id; match the persisted item and
	 * world instead of assuming the old dictionary key survives a session. */
	check(restored_items == 1,
		  "H3: actual Save and quit preserved the item in its Nether chunk");
	s->player.finished_loading = true;
	teardown(s);
}

/* H3: the autosave interval sweeps EVERY live world, not only slot 0: a dirty
 * column of the Nether the host stands in is saved in place (still resident,
 * no longer modified) within the interval. */
static void case_autosave_secondary(void) {
	struct server_local* s = two_world_fixture();
	host_travels(s);
	struct server_world* nether = world_of(s, &s->player);
	check(nether == &s->worlds[1] && server_local_world_live(nether),
		  "fixture: the host stands in the LIVE Nether (slot 1)");
	w_coord_t cx = (w_coord_t)floor(s->player.x / CHUNK_SIZE);
	w_coord_t cz = (w_coord_t)floor(s->player.z / CHUNK_SIZE);
	struct server_chunk* c = dict_server_chunks_get(nether->chunks, S_CHUNK_ID(cx, cz));
	check(c != NULL, "fixture: the host's Nether column is resident");
	check(server_world_set_block(nether, (w_coord_t)floor(s->player.x), 110,
								 (w_coord_t)floor(s->player.z),
								 (struct block_data) {.type = BLOCK_STONE}),
		  "fixture: a block edit dirties the host's Nether column");
	check(c->modified, "fixture: the column is modified");
	s->autosave_ticks = 4;
	s->autosave_timer = 0;
	bool was_paused = s->paused;
	s->paused = false;
	for(unsigned i = 0; i < 40 && c->modified; i++)
		tick(s);
	s->paused = was_paused;
	check(dict_server_chunks_get(nether->chunks, S_CHUNK_ID(cx, cz)) == c,
		  "the column is still resident (saved in place, not evicted)");
	check(!c->modified, "H3: the autosave sweep saved the Nether column within the interval");
	teardown(s);
}

/* M1 (Fable A2T-SERVER-REVIEW): a COMMIT whose level.dat save FAILS must not
 * leave the in-memory Player record rewritten to the destination -- the next
 * bare level_archive_save (autosave, save-migrate) would persist Dimension -1
 * at the staged Nether pose while the host stands in the Overworld. The real
 * failure used here is the archive's own lossless-write refusal. */
static uint64_t level_hash(const char* root);
static void case_commit_save_fail(void) {
	struct server_local* s = two_world_fixture();
	check(level_archive_write_player(&s->level,
									 (vec3) {s->player.x, s->player.y, s->player.z},
									 (vec2) {s->player.rx, s->player.ry}, NULL,
									 WORLD_DIM_OVERWORLD)
			  && level_archive_save(&s->level),
		  "fixture: the Player record names the Overworld source pose");
	vec3 pos0, rot0;
	enum world_dim dim0 = WORLD_DIM_NETHER;
	check(level_archive_read_player(&s->level, pos0, rot0, NULL, &dim0)
			  && dim0 == WORLD_DIM_OVERWORLD,
		  "fixture: the record reads back");
	uint64_t lvl = level_hash(split_path);
	s->level.lossless_blocked = true; /* any modified save now refuses */
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK,
		  "the request is accepted");
	check(drive(s, id) == SERVER_TRANSFER_ROLLED_BACK,
		  "the level.dat save failure ROLLS BACK the transaction");
	check(world_of(s, &s->player) == &s->worlds[0]
			  && s->player.dimension == WORLD_DIM_OVERWORLD,
		  "the host is still in the Overworld");
	vec3 pos1, rot1;
	enum world_dim dim1 = WORLD_DIM_NETHER;
	check(level_archive_read_player(&s->level, pos1, rot1, NULL, &dim1),
		  "the in-memory record still reads");
	check(dim1 == WORLD_DIM_OVERWORLD,
		  "M1: the in-memory Player.Dimension is restored to the source");
	check(pos1[0] == pos0[0] && pos1[1] == pos0[1] && pos1[2] == pos0[2]
			  && rot1[0] == rot0[0] && rot1[1] == rot0[1],
		  "M1: the in-memory Player pose is restored exactly");
	check(!s->level.modified, "M1: the archive is not left marked modified by the rollback");
	check(level_hash(split_path) == lvl, "level.dat bytes are unchanged on disk");
	/* the next bare save (the autosave shape) persists nothing wrong */
	s->level.lossless_blocked = false;
	check(level_archive_save(&s->level) && level_hash(split_path) == lvl,
		  "M1: a later bare level_archive_save writes no destination record");
	teardown(s);
}

/* Save both worlds and the level, release the session worlds, reopen the save
 * through the production load path. Returns the routed world's dimension. */
static uint64_t level_hash(const char* root) {
	char lvl[300];
	snprintf(lvl, sizeof(lvl), "%s/level.dat", root);
	FILE* f = fopen(lvl, "rb");
	if(!f)
		return 0;
	uint64_t h = UINT64_C(14695981039346656037);
	int ch;
	while((ch = fgetc(f)) != EOF) {
		h ^= (uint8_t)ch;
		h *= UINT64_C(1099511628211);
	}
	fclose(f);
	return h;
}

static bool save_release_reopen(struct server_local* s) {
	/* H3: the PRODUCTION unload (save-and-quit shape, no client reset) must
	 * checkpoint and release EVERY live world itself -- the test no longer
	 * saves or releases anything by hand. The harness guest is released
	 * first (its client half has no LEFT ack). */
	if(s->guest_actor)
		guest_release(s);
	if(!server_local_unload_world_impl(s, false))
		return false;
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY
			  && !server_local_world_live(&s->worlds[1]),
		  "H3: the production unload released the secondary world");
	check(s->player.world_slot == 0 && !s->player.has_pos,
		  "H3: the production unload reset the host's committed slot");
	string_t name;
	string_init_set_str(name, split_path);
	bool ok = server_local_load_world_impl(s, name);
	string_clear(name);
	if(!ok)
		return false;
	/* a REAL reopen: the load streams to its marker and the client
	 * acknowledges the input handoff, exactly like the first join -- the
	 * session is playable (transactions step) only after that */
	s->paused = true;
	unsigned before = markers;
	/* tick(), not the harness step(): the reopened region archive is owned
	 * by THIS thread (the one that ran the production load) */
	for(unsigned i = 0; i < 2000 && markers == before; i++)
		tick(s);
	check(markers == before + 1 && s->input_ack_pending,
		  "the reopened load streams to its marker");
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	tick(s);
	check(!s->input_ack_pending, "the client acknowledges the reopened load");
	return true;
}

static void case_reload_reopen(void) {
	struct server_local* s = two_world_fixture();
	/* (A) the host commits to the Nether, then the save is reopened */
	host_travels(s);
	double hx = s->player.x, hz = s->player.z;
	check(save_release_reopen(s), "(A) save, release and reopen through the production load");
	check(s->worlds[0].dimension == WORLD_DIM_NETHER && server_local_world_live(&s->worlds[0]),
		  "(A) the routed world reopens AS the Nether (Player.Dimension -1 picks the provider)");
	check(server_local_world_of(s, &s->player) == &s->worlds[0]
			  && s->player.dimension == WORLD_DIM_NETHER,
		  "(A) the host stands in the Nether it was saved in -- never relocated");
	check(fabs(s->player.x - hx) < 0.01 && fabs(s->player.z - hz) < 0.01,
		  "(A) the saved coordinates are restored");
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY,
		  "(A) login manufactures no second world and no portal");
	struct server_world* ow = server_local_world_admit(s, WORLD_DIM_OVERWORLD);
	check(ow && ow != &s->worlds[0] && ow->dimension == WORLD_DIM_OVERWORLD
			  && server_local_world_live(ow),
		  "(A) the Overworld admits LIVE beside the routed Nether (the guest's saved dimension)");
	check(server_local_world_for_dimension(s, WORLD_DIM_OVERWORLD) == ow
			  && server_local_world_for_dimension(s, WORLD_DIM_NETHER) == &s->worlds[0],
		  "(A) world_for_dimension answers both");
	/* (B) the host returns to the Overworld and the save is reopened again */
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_OVERWORLD, &id) == SERVER_TRANSFER_OK
			  && drive(s, id) == SERVER_TRANSFER_COMMITTED,
		  "(B) the host travels back to the Overworld");
	check(save_release_reopen(s), "(B) save, release and reopen");
	check(s->worlds[0].dimension == WORLD_DIM_OVERWORLD
			  && server_local_world_of(s, &s->player) == &s->worlds[0]
			  && s->player.dimension == WORLD_DIM_OVERWORLD,
		  "(B) the routed world reopens as the Overworld with the host in it");
	struct server_world* nether = server_local_world_admit(s, WORLD_DIM_NETHER);
	check(nether && nether->dimension == WORLD_DIM_NETHER && server_local_world_live(nether),
		  "(B) the Nether admits LIVE beside it");
	s->player.finished_loading = true;
	teardown(s);
}

static void case_memory_refusal(void) {
	struct server_local* s = two_world_fixture();
	s->player.inventory.items[INVENTORY_SLOT_MAIN] = mark_item;
	uint64_t inv = inventory_hash(&s->player);
	double hx = s->player.x, hy = s->player.y, hz = s->player.z;
	assert(level_archive_save(&s->level));
	uint64_t lvl = level_hash(split_path);
	counters_reset();
	/* a Wii whose MEM2 carve holds 512 KiB: below the 1 MiB floor */
	server_transfer_test_set_carve_free(512u * 1024u);
	uint32_t id = 0;
	enum server_transfer_rc rc
		= server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id);
	check(rc == SERVER_TRANSFER_REFUSED_ADMISSION && id == 0,
		  "admission over budget REFUSES the request (no id, nothing staged)");
	clin_update_drain(); /* the toast is queued; the client consumes it */
	check(hud_msgs >= 1 && strstr(last_hud, "Cannot open") != NULL,
		  "the refusal is VISIBLE: a HUD toast names it");
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY
			  && !server_transfer_active(s),
		  "slot 1 stays EMPTY, no transaction open");
	check(level_hash(split_path) == lvl, "level.dat bytes are unchanged");
	check(inventory_hash(&s->player) == inv && s->player.x == hx && s->player.y == hy
			  && s->player.z == hz && s->player.dimension == WORLD_DIM_OVERWORLD
			  && server_local_world_of(s, &s->player) == &s->worlds[0],
		  "the host record, pose and membership are unchanged");
	/* budget back: the same request admits */
	server_transfer_test_set_carve_free(SIZE_MAX);
	host_travels(s);
	check(server_local_world_of(s, &s->player)->dimension == WORLD_DIM_NETHER,
		  "with the budget restored the same journey admits and commits");
	teardown(s);
}

/* ---- the scripted client for the real admission path --------------------
 * The harness client is a single-world client (world-bound payloads off): its
 * own answers are DISABLED. These hooks swallow them and record the requests
 * so each case can answer exactly what the contract specifies. */
static unsigned cap_requests, landing_arms;
static uint32_t last_cap_id, last_cap_ticket, last_landing_token;
static uint8_t last_cap_op, last_cap_slot, last_landing_radius;
static uint32_t last_cap_mem1, last_cap_mem2;
static int32_t last_landing_x, last_landing_z;
static bool last_landing_cancel;
static bool swallow_capacity(uint32_t id, enum clin_capacity_result r,
							 clin_capacity_ticket ticket) {
	(void)id; (void)r; (void)ticket;
	return true;
}
static bool swallow_landing(uint32_t token, unsigned slot, uint32_t generation) {
	(void)token; (void)slot; (void)generation;
	return true;
}
static void scripted_observer(const struct client_rpc* call, void* user) {
	consumed(call, user);
	if(call->type == CRPC_CAPACITY_REQUEST) {
		cap_requests++;
		last_cap_id = call->payload.capacity_request.request_id;
		last_cap_op = call->payload.capacity_request.op;
		last_cap_slot = call->payload.capacity_request.slot;
		last_cap_ticket = call->payload.capacity_request.ticket;
		last_cap_mem1 = call->payload.capacity_request.mem1_bytes;
		last_cap_mem2 = call->payload.capacity_request.mem2_bytes;
	}
	if(call->type == CRPC_LANDING_ARM) {
		landing_arms++;
		last_landing_token = call->payload.landing_arm.token;
		last_landing_x = call->payload.landing_arm.x;
		last_landing_z = call->payload.landing_arm.z;
		last_landing_radius = call->payload.landing_arm.radius;
		last_landing_cancel = call->payload.landing_arm.cancel;
	}
}
static void scripted_client_install(void) {
	cap_requests = landing_arms = 0;
	clin_test_set_consume_observer(scripted_observer, NULL);
	clin_capacity_set_reply_sender(swallow_capacity);
	clin_landing_set_sender(swallow_landing);
}
static void scripted_client_remove(void) {
	clin_capacity_set_reply_sender(NULL);
	clin_landing_set_sender(NULL);
	clin_test_set_consume_observer(consumed, NULL);
}
static void reply_capacity(struct server_local* s, uint32_t id, uint8_t result,
						   uint32_t ticket) {
	struct server_rpc call = {.type = SRPC_CAPACITY_REPLY};
	call.payload.capacity_reply.request_id = id;
	call.payload.capacity_reply.result = result;
	call.payload.capacity_reply.ticket = ticket;
	assert(svin_rpc_send(&call));
}
static void reply_landing_ready(struct server_local* s, uint32_t token, uint8_t slot,
								uint32_t generation) {
	struct server_rpc call = {.type = SRPC_LANDING_READY};
	call.payload.landing_ready.token = token;
	call.payload.landing_ready.slot = slot;
	call.payload.landing_ready.generation = generation;
	assert(svin_rpc_send(&call));
}
/* unpaused ticks until the transaction reaches `want` (or a terminal state) */
static enum server_transfer_state drive_to(struct server_local* s, uint32_t id,
										  enum server_transfer_state want,
										  unsigned max_ticks) {
	bool was_paused = s->paused;
	s->paused = false;
	enum server_transfer_state st = server_transfer_state_of(s, id);
	for(unsigned i = 0; i < max_ticks; i++) {
		st = server_transfer_state_of(s, id);
		if(st == want || st == SERVER_TRANSFER_COMMITTED
		   || st == SERVER_TRANSFER_ROLLED_BACK)
			break;
		tick(s);
	}
	s->paused = was_paused;
	return st;
}

/* The HOST's admission: capacity RESERVE, then the landing round trip, then
 * COMMIT. (Before gap-doc blocker 3 the host skipped the landing because the
 * client could only report one with the GUEST's view on the slot; the client
 * now prepares an unviewed context on the ARM and the server streams the
 * arrival core into it, so the host's landing completes -- landing_real proves
 * it with the real client.) */
static void case_capacity_landing(void) {
	struct server_local* s = two_world_fixture();
	scripted_client_install();
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK,
		  "the request is accepted");
	enum server_transfer_state st = drive_to(s, id, SERVER_TRANSFER_CAPACITY, MAX_STEPS);
	check(st == SERVER_TRANSFER_CAPACITY, "after the Teleporter the transaction WAITS for capacity");
	int r = s->split_view_radius > 0 ? s->split_view_radius : 1;
	uint32_t columns = (uint32_t)(2 * r + 1) * (uint32_t)(2 * r + 1);
	uint32_t expected_mem1 = columns * SERVER_TRANSFER_MEM1_PER_COLUMN;
	if(clin_world_payloads_enabled())
		check(world_local_shared_capacity_mem1_need(columns, &expected_mem1),
			"local shared capacity bound is representable");
	check(cap_requests == 1 && last_cap_id == id && last_cap_op == CLIN_CAP_OP_RESERVE
			  && last_cap_slot == 1 && last_cap_mem1 == expected_mem1
			  && last_cap_mem2 == columns * SERVER_TRANSFER_MEM2_PER_COLUMN,
		  "exactly one CAPACITY_REQUEST RESERVE for slot 1 sized from the landing radius");
	check(server_local_world_of(s, &s->player) == &s->worlds[0],
		  "no membership was published while waiting");
	reply_capacity(s, id, CLIN_CAP_OK, 77);
	st = drive_to(s, id, SERVER_TRANSFER_LANDING, 10);
	check(st == SERVER_TRANSFER_LANDING && landing_arms == 1 && last_landing_token == id
			  && last_landing_radius == SERVER_TRANSFER_LANDING_CORE_RADIUS,
		  "host: capacity OK -> exactly one LANDING_ARM over the arrival core");
	check(server_local_world_of(s, &s->player) == &s->worlds[0],
		  "host: no membership while the landing is armed");
	reply_landing_ready(s, id, 1, s->worlds[1].generation);
	st = drive_to(s, id, SERVER_TRANSFER_COMMITTED, 10);
	check(st == SERVER_TRANSFER_COMMITTED, "host: READY -> COMMIT");
	check(server_local_world_of(s, &s->player) == &s->worlds[1]
			  && s->player.dimension == WORLD_DIM_NETHER,
		  "membership published after the capacity reservation");
	check(cap_requests == 2 && last_cap_op == CLIN_CAP_OP_COMMIT && last_cap_ticket == 77,
		  "the reserved ticket is COMMITted after the switch");
	scripted_client_remove();
	teardown(s);
}

/* residency-1005/2 (ASTRA-RESIDENCY-REVIEW-2, R1 3/5): the REAL transaction
 * commits only onto a RESIDENT landing footprint. The arrival column is
 * evicted (and its loads made to fail) after the landing is READY: COMMIT
 * waits with the body still in the source; a recoverable load then commits;
 * a permanent one rolls back at the existing client-wait bound. */
static const struct server_transfer_txn* txn_by_id(struct server_local* s,
	uint32_t id) {
	for(size_t i = 0; i < 2; i++)
		if(s->transfer[i].id == id)
			return &s->transfer[i];
	return NULL;
}
/* residency-1005/4b: the post-COMMIT presentation the real client completes
 * before the body is live again (the CaveWii form of Beta's Packet9Respawn ->
 * GuiDownloadTerrain round trip): the host's A2 loading marker and its input
 * ACK. Until then server_transfer_request answers REFUSED_BUSY by design. */
static void finish_presentation(struct server_local* s,
	struct server_player* actor) {
	bool was_paused = s->paused;
	s->paused = false;
	if(actor == &s->player) {
		for(unsigned i = 0; i < 1500 && s->a2w_host_presentation
			 && !s->input_ack_pending; i++)
			tick(s);
		if(s->input_ack_pending) {
			screen_load_world.update(&screen_load_world, 30.0f);
			clin_update_input_readiness();
			tick(s);
		}
	} else {
		/* the guest client's echo: its first pose sample on the fresh
		 * correction token at the committed pose (the body installs on READY) */
		for(unsigned i = 0; i < 200 && s->a2w_guest_presentation; i++) {
			tick(s);
			struct server_rpc echo = pose_sample(actor->x, actor->y, actor->z,
				true, actor->correction_seq);
			(void)svin_rpc_send_from(guest_source(s), &echo);
			drain_server(s);
		}
	}
	s->paused = was_paused;
	printf("     [presentation: host=%d guest=%d ack=%u]\n",
		(int)s->a2w_host_presentation, (int)s->a2w_guest_presentation,
		(unsigned)s->input_ack_pending);
}
/* residency-1005/4 closure item 6: one leg -- `actor` travels from its
 * current world to `dim`, the arrival column evicted (its loads failing)
 * after the landing is READY. Delayed: COMMIT waits uncommitted, then
 * commits onto the loaded column and the body's old residency state is
 * retired (no pin/request/recovery of the source world survives). Permanent:
 * the wait bound rolls back -- pose, world, dimension and record untouched,
 * the capacity ticket RELEASEd -- and a stale READY afterwards cannot revive
 * it. */
static void landing_residency_leg(struct server_local* s,
	struct server_player* actor, enum world_dim dim, bool permanent,
	const char* name) {
	char label[220];
	uint32_t id = 0;
	const struct server_world* src = server_local_world_of(s, actor);
	const double sx = actor->x, sz = actor->z;
	const enum world_dim sdim = actor->dimension;
	enum server_transfer_rc rq = server_transfer_request(s, actor, dim, &id);
	printf("     [%s: request rc=%d world_switch=%d active=%d inert=%d fin=%d ack=%u]\n", name, (int)rq,
		(int)s->world_switch_phase, (int)server_transfer_active(s),
		(int)server_transfer_actor_inert(s, actor), (int)actor->finished_loading,
		(unsigned)s->input_ack_pending);
	assert(rq == SERVER_TRANSFER_OK);
	assert(drive_to(s, id, SERVER_TRANSFER_CAPACITY, MAX_STEPS)
		   == SERVER_TRANSFER_CAPACITY);
	/* the source pose the transaction HOLDS (its frozen body) */
	double hx = actor->x, hy = actor->y, hz = actor->z;
	reply_capacity(s, id, CLIN_CAP_OK, 55);
	assert(drive_to(s, id, SERVER_TRANSFER_LANDING, 10)
		   == SERVER_TRANSFER_LANDING);
	const struct server_transfer_txn* t = txn_by_id(s, id);
	assert(t);
	const uint8_t dst_slot = t->dst_slot;
	struct server_world* dst = &s->worlds[dst_slot];
	const int lcx = (int)floor(t->pose.x) >> 4, lcz = (int)floor(t->pose.z) >> 4;
	assert(server_world_chunk_resident(dst, lcx, lcz));
	server_world_test_fail_gen_attempts(100000);
	assert(server_world_save_chunk(dst, true, lcx, lcz));
	assert(!server_world_chunk_resident(dst, lcx, lcz));
	reply_landing_ready(s, id, dst_slot, dst->generation);
	enum server_transfer_state st = server_transfer_state_of(s, id);
	for(int i = 0; i < 20; i++) {
		st = drive_to(s, id, SERVER_TRANSFER_COMMITTED, 1);
		if(st == SERVER_TRANSFER_COMMITTED || st == SERVER_TRANSFER_ROLLED_BACK) break;
	}
	snprintf(label, sizeof label, "%s landing residency: COMMIT waits while the "
		"arrival footprint is not resident; the destination is NOT published "
		"(body still in its source world, pose and dimension untouched)", name);
	check(st != SERVER_TRANSFER_COMMITTED
			  && server_local_world_of(s, actor) == src && actor->x == sx
			  && actor->z == sz && actor->dimension == sdim, label);
	if(!permanent) {
		/* poison the body's transient residency state of the SOURCE world:
		 * the commit must retire it (lifecycle edge), not carry it over */
		actor->rz.req[0] = (struct rz_request) {.cx = 9, .cz = 9, .used = true,
			.prio = RZ_PRIO_NEED, .world_index = src->world_index,
			.world_generation = src->generation};
		server_world_test_fail_gen_attempts(0);
		st = drive_to(s, id, SERVER_TRANSFER_COMMITTED, 60);
		snprintf(label, sizeof label, "%s landing residency: the barrier loads "
			"the arrival column and the transaction COMMITs onto it", name);
		check(st == SERVER_TRANSFER_COMMITTED
				  && server_local_world_of(s, actor) == dst
				  && server_world_chunk_resident(dst, lcx, lcz), label);
		snprintf(label, sizeof label, "%s landing residency: the committed body "
			"carries no residency state of its old world (pin, requests, "
			"recovery retired by the real transition)", name);
		bool old_req = false;
		for(unsigned k = 0; k < RZ_REQ_MAX; k++)
			old_req |= actor->rz.req[k].used
				&& actor->rz.req[k].world_index == src->world_index
				&& actor->rz.req[k].world_generation == src->generation;
		check(!old_req && !actor->rz.recovering && !actor->rz.failed
			&& (!actor->rz.pin.valid
				|| actor->rz.pin.world_index == dst->world_index), label);
		finish_presentation(s, actor);
	} else {
		cap_requests = 0;
		/* The body is NOT held during LANDING under this scripted client (it
		 * never applies CRPC_TRANSFER_HOLD), so its own client's admitted
		 * samples may still settle it (the guest lands 4.6 cm in the Nether:
		 * 6.149 -> 6.103, an accepted guest PLAYER_POS, pinned as such).
		 * The rollback claim is that the ROLLBACK itself moves nothing:
		 * compare with the pose just before the terminal update. */
		for(unsigned i = 0; i < SERVER_TRANSFER_CLIENT_WAIT_TICKS + 60; i++) {
			hx = actor->x;
			hy = actor->y;
			hz = actor->z;
			st = drive_to(s, id, SERVER_TRANSFER_COMMITTED, 1);
			if(st == SERVER_TRANSFER_ROLLED_BACK || st == SERVER_TRANSFER_COMMITTED)
				break;
		}
		clin_update_drain();
		printf("     [%s permanent: st=%d same_world=%d dx=%g dy=%g dz=%g dim %d/%d cap_op=%d]\n",
			name, (int)st, (int)(server_local_world_of(s, actor) == src),
			actor->x - hx, actor->y - hy, actor->z - hz, (int)actor->dimension,
			(int)sdim, (int)last_cap_op);
		snprintf(label, sizeof label, "%s landing residency: a landing that "
			"never becomes resident ROLLS BACK at the wait bound (pose, world, "
			"dimension untouched; ticket RELEASEd)", name);
		check(st == SERVER_TRANSFER_ROLLED_BACK
				  && server_local_world_of(s, actor) == src && actor->x == hx
				  && actor->y == hy && actor->z == hz && actor->dimension == sdim
				  && last_cap_op == CLIN_CAP_OP_RELEASE, label);
		server_world_test_fail_gen_attempts(0);
		reply_landing_ready(s, id, dst_slot, dst->generation);
		st = drive_to(s, id, SERVER_TRANSFER_COMMITTED, 20);
		snprintf(label, sizeof label, "%s landing residency: a STALE READY after "
			"the rollback cannot revive the transaction", name);
		check(server_transfer_state_of(s, id) == SERVER_TRANSFER_ROLLED_BACK
				  && server_local_world_of(s, actor) == src, label);
	}
	server_world_test_fail_gen_attempts(0);
}
/* host and guest, Overworld -> Nether and (after a real trip) Nether ->
 * Overworld */
static void landing_residency(bool permanent) {
	struct server_local* s = two_world_fixture();
	scripted_client_install();
	landing_residency_leg(s, &s->player, WORLD_DIM_NETHER, permanent,
		"host O->N");
	if(permanent) {
		/* a real committed trip first, so the reverse leg starts there */
		landing_residency_leg(s, &s->player, WORLD_DIM_NETHER, false,
			"host O->N (setup)");
	}
	landing_residency_leg(s, &s->player, WORLD_DIM_OVERWORLD, permanent,
		"host N->O");
	/* one fixture per process (the split harness is not re-creatable in
	 * the same process): the guest legs run beside the host's end state */
	landing_residency_leg(s, s->guest_actor, WORLD_DIM_NETHER, permanent,
		"guest O->N");
	if(permanent)
		landing_residency_leg(s, s->guest_actor, WORLD_DIM_NETHER, false,
			"guest O->N (setup)");
	landing_residency_leg(s, s->guest_actor, WORLD_DIM_OVERWORLD, permanent,
		"guest N->O");
	scripted_client_remove();
	teardown(s);
}

/* World-payload admission must reserve the same ring the server streams.
 * Configured zero/full and >4 are bounded to four only in opt-in mode;
 * legacy requests preserve their original quantities. */
static uint64_t test_budget_mem1, test_budget_mem2;
static void test_capacity_budget(struct clin_capacity_budget* out) {
	out->mem1_free = test_budget_mem1;
	out->mem2_free = test_budget_mem2;
}

static void case_capacity_radius_policy(bool payloads_on, int configured) {
	struct server_local* s = two_world_fixture();
	s->split_view_radius = configured;
	scripted_client_install();
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id)
			  == SERVER_TRANSFER_OK,
		  "capacity radius: host transfer accepted");
	check(drive_to(s, id, SERVER_TRANSFER_CAPACITY, MAX_STEPS)
			  == SERVER_TRANSFER_CAPACITY,
		  "capacity radius: request reaches client reservation");
	int r = payloads_on ? (configured >= 1 && configured <= 4 ? configured : 4)
		: (configured > 0 ? configured : 1);
	uint32_t columns = (uint32_t)(2 * r + 1) * (uint32_t)(2 * r + 1);
	uint32_t expected_mem1 = columns * SERVER_TRANSFER_MEM1_PER_COLUMN;
	if(payloads_on)
		check(world_local_shared_capacity_mem1_need(columns, &expected_mem1),
			"payload capacity uses the local shared installation bound");
	check(cap_requests == 1 && last_cap_op == CLIN_CAP_OP_RESERVE
			  && last_cap_mem1 == expected_mem1
			  && last_cap_mem2 == columns * SERVER_TRANSFER_MEM2_PER_COLUMN,
		  "capacity radius: RESERVE bytes match bounded or legacy ring policy");
	if(payloads_on && configured == 5) {
		/* Use the production outgoing request at the Wii's captured refusal
		 * budget. A fresh CAP ticket must fit after the same 4/2 MiB floors;
		 * tightening either budget still refuses the correct resource. */
		clin_capacity_reset();
		clin_capacity_set_budget_provider(test_capacity_budget);
		test_budget_mem1 = 7308u * 1024u;
		test_budget_mem2 = 22000u * 1024u;
		struct clin_capacity_need need = {last_cap_mem1, last_cap_mem2};
		clin_capacity_ticket ticket = 0;
		struct clin_capacity_need old_need = {
			columns * SERVER_TRANSFER_MEM1_PER_COLUMN, need.mem2_bytes};
		check(clin_capacity_reserve_world(last_cap_slot,
				  s->worlds[1].generation, &old_need, &ticket) == CLIN_CAP_NO_MEM1,
			"captured budget reproduces the obsolete 48 KiB per-column refusal");
		check(need.mem1_bytes < (7308u - 4096u) * 1024u
				  && clin_capacity_reserve_world(last_cap_slot,
					  s->worlds[1].generation, &need, &ticket) == CLIN_CAP_OK
				  && ticket != 0,
			"captured 7308 KiB Wii MEM1 admits the production 81-column request");
		check(clin_capacity_release(ticket), "captured-budget ticket releases");
		test_budget_mem1 = CLIN_CAP_MEM1_MARGIN + need.mem1_bytes - 1u;
		check(clin_capacity_reserve_world(last_cap_slot,
				  s->worlds[1].generation, &need, &ticket) == CLIN_CAP_NO_MEM1,
			"one byte below MEM1 floor plus need still refuses");
		test_budget_mem1 = 7308u * 1024u;
		test_budget_mem2 = CLIN_CAP_MEM2_MARGIN + need.mem2_bytes - 1u;
		check(clin_capacity_reserve_world(last_cap_slot,
				  s->worlds[1].generation, &need, &ticket) == CLIN_CAP_NO_MEM2,
			"one byte below carve floor plus need still refuses");
		test_budget_mem1 = CLIN_CAP_MEM1_MARGIN - 1u;
		test_budget_mem2 = 22000u * 1024u;
		struct clin_capacity_need no_increment = {0, 0};
		check(clin_capacity_reserve_world(last_cap_slot,
				  s->worlds[1].generation, &no_increment, &ticket)
				  == CLIN_CAP_NO_MEM1,
			"unchanged 4 MiB floor refuses even a zero incremental request");
		check(!world_local_shared_capacity_mem1_need(0, &expected_mem1)
				  && !world_local_shared_capacity_mem1_need(UINT32_MAX, &expected_mem1)
				  && !world_local_shared_capacity_mem1_need(81, NULL),
			"shared bound rejects empty, overflow and missing output");
		clin_capacity_set_budget_provider(NULL);
		clin_capacity_reset();
	}
	reply_capacity(s, id, CLIN_CAP_NO_MEM2, 0);
	check(drive(s, id) == SERVER_TRANSFER_ROLLED_BACK,
		  "capacity radius: refused reservation rolls back without commitment");
	check(s->transfer_rollback_id[0] == id
			  && s->transfer_rollback_reason[0]
				  == SERVER_TRANSFER_ROLLBACK_CLIENT_NO_MEM2,
		  "sticky rollback preserves exact client refusal subtype");
	scripted_client_remove();
	teardown(s);
}

/* The GUEST's admission keeps the landing round trip (membership only once
 * the guest's view has the landing columns). The guest COMMIT itself waits for
 * lane C's transition adapter, so these cases stop at the landing decision. */
static void case_guest_landing(void) {
	struct server_local* s = two_world_fixture();
	scripted_client_install();
	uint32_t id = 0;
	check(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK,
		  "guest: the request is accepted");
	check(drive_to(s, id, SERVER_TRANSFER_CAPACITY, MAX_STEPS) == SERVER_TRANSFER_CAPACITY,
		  "guest: waits for capacity");
	reply_capacity(s, id, CLIN_CAP_OK, 78);
	enum server_transfer_state st = drive_to(s, id, SERVER_TRANSFER_LANDING, 10);
	check(st == SERVER_TRANSFER_LANDING && landing_arms == 1 && last_landing_token == id
			  && !last_landing_cancel
			  && last_landing_radius == SERVER_TRANSFER_LANDING_CORE_RADIUS,
		  "guest: OK -> exactly one LANDING_ARM with this token over the arrival core");
	check(abs(last_landing_x - (int32_t)floor(s->guest_actor->x / 8.0)) <= 40
			  && abs(last_landing_z - (int32_t)floor(s->guest_actor->z / 8.0)) <= 40,
		  "guest: the landing is armed at the staged destination pose");
	check(server_local_world_of(s, s->guest_actor) == &s->worlds[0],
		  "guest: no membership before READY");
	reply_landing_ready(s, id, 1, s->worlds[1].generation);
	st = drive_to(s, id, SERVER_TRANSFER_COMMITTED, 10);
	check(st != SERVER_TRANSFER_LANDING,
		  "guest: READY ends the landing wait (the COMMIT itself is lane C's adapter)");
	if(st == SERVER_TRANSFER_ROLLED_BACK) {
		clin_update_drain();
		check(landing_arms == 2 && last_landing_cancel && last_landing_token == id,
			  "M3: a rollback AFTER the landing was ready still cancels the armed landing");
	}
	scripted_client_remove();
	teardown(s);
}

static void case_landing_timeout(void) {
	struct server_local* s = two_world_fixture();
	scripted_client_install();
	double gx = s->guest_actor->x, gz = s->guest_actor->z;
	uint32_t id = 0;
	/* the landing belongs to the GUEST's admission (the host has none) */
	assert(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK);
	assert(drive_to(s, id, SERVER_TRANSFER_CAPACITY, MAX_STEPS) == SERVER_TRANSFER_CAPACITY);
	reply_capacity(s, id, CLIN_CAP_OK, 91);
	assert(drive_to(s, id, SERVER_TRANSFER_LANDING, 10) == SERVER_TRANSFER_LANDING);
	/* the client never reports READY */
	enum server_transfer_state st
		= drive_to(s, id, SERVER_TRANSFER_COMMITTED, SERVER_TRANSFER_CLIENT_WAIT_TICKS + 50);
	check(st == SERVER_TRANSFER_ROLLED_BACK, "a landing that never becomes ready ROLLS BACK");
	check(landing_arms == 2 && last_landing_cancel && last_landing_token == id,
		  "the armed landing is cancelled");
	check(cap_requests == 2 && last_cap_op == CLIN_CAP_OP_RELEASE && last_cap_ticket == 91,
		  "the reserved ticket is RELEASEd");
	check(server_local_world_of(s, s->guest_actor) == &s->worlds[0] && s->guest_actor->x == gx
			  && s->guest_actor->z == gz && s->guest_actor->dimension == WORLD_DIM_OVERWORLD,
		  "the guest is untouched in the source");
	/* and a capacity refusal rolls back at once */
	uint32_t id2 = 0;
	assert(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id2) == SERVER_TRANSFER_OK);
	assert(drive_to(s, id2, SERVER_TRANSFER_CAPACITY, MAX_STEPS) == SERVER_TRANSFER_CAPACITY);
	counters_reset();
	reply_capacity(s, id2, CLIN_CAP_NO_MEM2, 0);
	st = drive_to(s, id2, SERVER_TRANSFER_COMMITTED, 10);
	check(st == SERVER_TRANSFER_ROLLED_BACK, "a capacity refusal (NO_MEM2) rolls back");
	clin_update_drain();
	check(hud_msgs >= 1, "the refusal is visible (HUD toast)");
	scripted_client_remove();
	teardown(s);
}

static void case_landing_stale(void) {
	struct server_local* s = two_world_fixture();
	scripted_client_install();
	uint32_t id = 0;
	assert(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK);
	assert(drive_to(s, id, SERVER_TRANSFER_CAPACITY, MAX_STEPS) == SERVER_TRANSFER_CAPACITY);
	reply_capacity(s, id, CLIN_CAP_OK, 5);
	assert(drive_to(s, id, SERVER_TRANSFER_LANDING, 10) == SERVER_TRANSFER_LANDING);
	uint32_t gen = s->worlds[1].generation;
	reply_landing_ready(s, id, 1, gen + 1); /* another instance of slot 1 */
	enum server_transfer_state st = drive_to(s, id, SERVER_TRANSFER_COMMITTED, 20);
	check(st == SERVER_TRANSFER_LANDING, "a READY for another generation is ignored: still waiting");
	reply_landing_ready(s, id, 0, gen); /* right generation, wrong slot */
	st = drive_to(s, id, SERVER_TRANSFER_COMMITTED, 20);
	check(st == SERVER_TRANSFER_LANDING, "a READY for another slot is ignored: still waiting");
	reply_landing_ready(s, id, 1, gen);
	st = drive_to(s, id, SERVER_TRANSFER_COMMITTED, 10);
	check(st != SERVER_TRANSFER_LANDING,
		  "the matching READY ends the wait (the guest COMMIT is lane C's adapter)");
	scripted_client_remove();
	teardown(s);
}

/* M4 (Fable A2T-SERVER-REVIEW): while a transfer is open the player SEES that
 * it is travelling (a HUD notice kept up for the whole wait, not a silent
 * 30-60 s), the legacy respawn path is refused (a fresh Overworld body must
 * never be teleported to the staged Nether pose), and a body that dies
 * mid-transfer rolls the transaction back so the next Respawn press works. */
static void case_travel_gate(void) {
	struct server_local* s = two_world_fixture();
	scripted_client_install(); /* the client never answers: a real wait */
	counters_reset();
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK,
		  "the request is accepted");
	bool was_paused = s->paused;
	s->paused = false;
	tick(s);
	check(hud_msgs >= 1 && strstr(last_hud, "Travelling") != NULL,
		  "M4: the host is told it is travelling on the first step");
	/* hold the transaction in a client wait (no reply) for ~10 s: the notice
	 * must keep being refreshed (the client shows a notice for 3 s) */
	unsigned before = hud_msgs;
	for(unsigned i = 0; i < 200 && server_transfer_active(s); i++)
		tick(s);
	check(server_transfer_active(s), "fixture: the transaction is still waiting");
	check(hud_msgs >= before + 3 && strstr(last_hud, "Travelling") != NULL,
		  "M4: the travelling notice is refreshed through the wait");
	/* the host dies while travelling */
	s->player.health = 0;
	check(!server_local_respawn_request(s),
		  "M4: the legacy respawn path is REFUSED while the transfer is open");
	check(world_of(s, &s->player) == &s->worlds[0]
			  && s->world_switch_phase == SERVER_WORLD_SWITCH_IDLE,
		  "M4: nothing moved (no death world switch started)");
	for(unsigned i = 0; i < 4 && server_transfer_active(s); i++)
		tick(s);
	check(server_transfer_state_of(s, id) == SERVER_TRANSFER_ROLLED_BACK,
		  "M4: a body that dies mid-transfer ROLLS the transaction back");
	check(world_of(s, &s->player) == &s->worlds[0]
			  && s->player.dimension == WORLD_DIM_OVERWORLD,
		  "M4: the dead host is still in the source world");
	s->player.has_bed_spawn = false;
	check(server_local_respawn_request(s),
		  "M4: after the rollback the Respawn press is accepted");
	for(unsigned i = 0; i < MAX_STEPS && s->world_switch_phase != SERVER_WORLD_SWITCH_IDLE; i++)
		tick(s);
	check(s->player.health > 0 && world_of(s, &s->player) == &s->worlds[0],
		  "M4: the host respawned in the Overworld");
	s->paused = was_paused;
	scripted_client_remove();
	teardown(s);
}

/* ITEM 4 (split dims, the production caller): a host portal dwell with the
 * partner attached. Flag OFF (split.world_payloads off): the refusal is
 * exactly as before. Flag ON: the dwell opens the A2-T transaction; on COMMIT
 * the host is in the Nether, the guest stays in the Overworld, and the host's
 * client receives its barrier (BEGIN_WORLD_SWITCH for the source world) and a
 * WORLD_RESET stamped with the destination world. */
static unsigned p4_begin_msgs, p4_reset_msgs;
static uint8_t p4_begin_slot, p4_reset_slot;
static uint32_t p4_reset_generation;
static int32_t p4_reset_dim;
extern int32_t g_srv_host_dim;
static unsigned p4_producer_resets;
static int32_t p4_dim_at_reset;
static void p4_producer_observer(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_WORLD_RESET && !call->payload.world_reset.guest_world) {
		p4_producer_resets++;
		p4_dim_at_reset = __atomic_load_n(&g_srv_host_dim, __ATOMIC_RELAXED);
	}
}
static void p4_observer(const struct client_rpc* call, void* user) {
	scripted_observer(call, user);
	if(call->type == CRPC_BEGIN_WORLD_SWITCH) {
		p4_begin_msgs++;
		p4_begin_slot = call->world_index;
	}
	if(call->type == CRPC_WORLD_RESET && !call->payload.world_reset.guest_world) {
		p4_reset_msgs++;
		p4_reset_slot = call->payload.world_reset.world_index;
		p4_reset_generation = call->payload.world_reset.world_generation;
		p4_reset_dim = call->payload.world_reset.dimension;
	}
}
static void dwell_complete(struct server_local* s) {
	s->player.portal_travel_requested = true;
	bool was_paused = s->paused;
	s->paused = false;
	tick(s);
	s->paused = was_paused;
}
static void case_portal_flag_off(void) {
	struct server_local* s = two_world_fixture();
	check(!clin_world_payloads_enabled(), "fixture: world-bound payloads are OFF");
	counters_reset();
	dwell_complete(s);
	check(hud_msgs >= 1 && strstr(last_hud, "Player 2 must leave first") != NULL,
		  "flag OFF: the partner refusal is shown exactly as before");
	check(!server_transfer_active(s) && server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY,
		  "flag OFF: no transaction, no second world");
	check(world_of(s, &s->player) == &s->worlds[0] && s->player.dimension == WORLD_DIM_OVERWORLD
			  && s->world_switch_phase == SERVER_WORLD_SWITCH_IDLE,
		  "flag OFF: the host stays in the Overworld, no legacy switch");
	teardown(s);
}
static void case_portal_transfer(void) {
	check(clin_world_payloads_enabled(), "fixture: world-bound payloads ON (set before any world)");
	struct server_local* s = two_world_fixture();
	scripted_client_install();
	clin_test_set_consume_observer(p4_observer, NULL);
	counters_reset();
	double gx = s->guest_actor->x, gz = s->guest_actor->z;
	struct server_world_handle ow = server_world_handle_of(&s->worlds[0]);
	dwell_complete(s);
	check(server_transfer_active(s), "flag ON: the dwell OPENS the A2-T transaction (no refusal)");
	check(strstr(last_hud, "must leave first") == NULL, "flag ON: no partner refusal");
	uint32_t id = s->transfer[ACTOR_SLOT_HOST].id;
	check(drive_to(s, id, SERVER_TRANSFER_CAPACITY, MAX_STEPS) == SERVER_TRANSFER_CAPACITY,
		  "the host transaction reserves the client capacity");
	unsigned resets_before = p4_reset_msgs;
	reply_capacity(s, id, CLIN_CAP_OK, 44);
	check(drive_to(s, id, SERVER_TRANSFER_LANDING, 20) == SERVER_TRANSFER_LANDING,
		  "the host transaction waits for its landing");
	reply_landing_ready(s, id, 1, s->worlds[1].generation);
	p4_producer_resets = 0;
	p4_dim_at_reset = INT32_MIN;
	clin_test_set_rpc_observer(p4_producer_observer, NULL);
	check(drive_to(s, id, SERVER_TRANSFER_COMMITTED, 20) == SERVER_TRANSFER_COMMITTED,
		  "the host transfer COMMITS");
	clin_test_set_rpc_observer(NULL, NULL);
	check(p4_producer_resets == 1 && p4_dim_at_reset == WORLD_DIM_NETHER,
		  "host dimension mirror is current before the COMMIT WORLD_RESET enters the client FIFO");
	clin_update_drain();
	struct server_world* nether = world_of(s, &s->player);
	check(nether == &s->worlds[1] && s->player.dimension == WORLD_DIM_NETHER,
		  "the host is in the Nether (slot 1)");
	check(world_of(s, s->guest_actor) == &s->worlds[0]
			  && s->guest_actor->dimension == WORLD_DIM_OVERWORLD
			  && s->guest_actor->x == gx && s->guest_actor->z == gz,
		  "the guest stays in the Overworld, untouched");
	check(server_local_world_from_handle(s, ow) == &s->worlds[0],
		  "the Overworld instance is the same (nothing torn down)");
	check(p4_begin_msgs >= 1 && p4_begin_slot == 0,
		  "the host's client gets its barrier (BEGIN stamped for its source world)");
	check(p4_reset_msgs == resets_before + 1 && p4_reset_slot == nether->world_index
			  && p4_reset_generation == nether->generation && p4_reset_dim == WORLD_DIM_NETHER,
		  "the host's client gets a WORLD_RESET stamped with the Nether world");
	scripted_client_remove();
	teardown(s);
}

/* M2 (Fable A2T-SERVER-REVIEW): no client capacity ticket may leak. (a) a
 * RELEASE the transport refused is retried until admitted; (b) an OK that
 * arrives after the transaction timed out is released at once; (c) RELEASE is
 * a ticket operation and goes out unstamped (never tied to a dead instance). */
static unsigned m2_release_msgs;
static uint32_t m2_release_ticket, m2_release_generation;
static void m2_observer(const struct client_rpc* call, void* user) {
	scripted_observer(call, user);
	if(call->type == CRPC_CAPACITY_REQUEST
	   && call->payload.capacity_request.op == CLIN_CAP_OP_RELEASE) {
		m2_release_msgs++;
		m2_release_ticket = call->payload.capacity_request.ticket;
		m2_release_generation = call->world_generation;
	}
}
/* unpaused real updates with no transaction to wait for */
static void run_ticks(struct server_local* s, unsigned n) {
	bool was_paused = s->paused;
	s->paused = false;
	for(unsigned i = 0; i < n; i++) {
		tick(s);
		client_acks_marker(s);
	}
	s->paused = was_paused;
}
static void case_cap_release(void) {
	struct server_local* s = two_world_fixture();
	scripted_client_install();
	clin_test_set_consume_observer(m2_observer, NULL);
	/* (a) + (c): a host COMMIT whose record fails rolls back holding ticket
	 * 91, and the first RELEASE send is refused by the transport */
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK,
		  "(a) the request is accepted");
	check(drive_to(s, id, SERVER_TRANSFER_CAPACITY, MAX_STEPS) == SERVER_TRANSFER_CAPACITY,
		  "(a) waits for capacity");
	server_transfer_test_fault(SERVER_TRANSFER_FAULT_COMMIT_MARKER);
	server_transfer_test_refuse_capacity_sends(1);
	reply_capacity(s, id, CLIN_CAP_OK, 91);
	check(drive_to(s, id, SERVER_TRANSFER_LANDING, 10) == SERVER_TRANSFER_LANDING,
		  "(a) waits for the landing");
	reply_landing_ready(s, id, 1, s->worlds[1].generation);
	check(drive_to(s, id, SERVER_TRANSFER_COMMITTED, 10) == SERVER_TRANSFER_ROLLED_BACK,
		  "(a) the COMMIT record fails -> ROLLBACK with the ticket held");
	run_ticks(s, 5);
	clin_update_drain();
	check(m2_release_msgs == 1 && m2_release_ticket == 91,
		  "M2(a): the refused RELEASE is retried until admitted (ticket 91)");
	check(m2_release_generation == 0, "M2(c): RELEASE goes out unstamped");
	/* (b): a late OK after the capacity wait timed out */
	uint32_t id2 = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id2) == SERVER_TRANSFER_OK,
		  "(b) a second request is accepted");
	check(drive_to(s, id2, SERVER_TRANSFER_CAPACITY, MAX_STEPS) == SERVER_TRANSFER_CAPACITY,
		  "(b) waits for capacity");
	check(drive_to(s, id2, SERVER_TRANSFER_COMMITTED, SERVER_TRANSFER_CLIENT_WAIT_TICKS + 50)
			  == SERVER_TRANSFER_ROLLED_BACK,
		  "(b) the capacity wait times out");
	unsigned before = m2_release_msgs;
	reply_capacity(s, id2, CLIN_CAP_OK, 55);
	run_ticks(s, 5);
	clin_update_drain();
	check(m2_release_msgs == before + 1 && m2_release_ticket == 55,
		  "M2(b): a stale OK is RELEASEd at once (ticket 55)");
	scripted_client_remove();
	teardown(s);
}

/* M2(d): the COMMITTED reservation has a lifecycle -- with the REAL client
 * (world-bound payloads ON) the host goes Overworld -> Nether -> Overworld ->
 * Nether; the third leg must not be refused BUSY by the first leg's
 * reservation that nothing ever released. */
static void case_cap_lifecycle(void) {
	check(clin_world_payloads_enabled(), "fixture: world-bound payloads ON");
	struct server_local* s = two_world_fixture();
	for(unsigned leg = 0; leg < 3; leg++) {
		enum world_dim to = leg % 2 == 0 ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
		uint32_t id = 0;
		check(server_transfer_request(s, &s->player, to, &id) == SERVER_TRANSFER_OK,
			  "leg: the request is accepted");
		enum server_transfer_state st = drive(s, id);
		char msg[96];
		snprintf(msg, sizeof(msg), "M2(d): leg %u (%s) COMMITS", leg + 1,
				 to == WORLD_DIM_NETHER ? "-> Nether" : "-> Overworld");
		check(st == SERVER_TRANSFER_COMMITTED, msg);
		check(s->player.dimension == to, "leg: the host is in the destination");
		/* let the vacated world drain (retire_vacant) before the next leg */
		run_ticks(s, 200);
		clin_update_drain();
	}
	teardown(s);
}

/* M5 (Fable A2T-SERVER-REVIEW): with the host in the Nether (slot 1), the
 * Overworld (slot 0) random-ticks and anchors its residency around ITS OWN
 * player -- the guest -- never around the host's Nether coordinates; with
 * nobody left in the Overworld it takes no random-tick pass at all. The
 * Nether ticks around the host. */
static void case_slot0_anchor(void) {
	struct server_local* s = two_world_fixture();
	host_travels(s);
	check(world_of(s, &s->player) == &s->worlds[1], "fixture: the host is in the Nether");
	/* the guest stands far from the host's Nether chunk (x8 apart) */
	s->guest_actor->x = 200.5;
	s->guest_actor->z = -150.5;
	w_coord_t gcx = (w_coord_t)floor(s->guest_actor->x / 16.0);
	w_coord_t gcz = (w_coord_t)floor(s->guest_actor->z / 16.0);
	w_coord_t hcx = (w_coord_t)floor(s->player.x / 16.0);
	w_coord_t hcz = (w_coord_t)floor(s->player.z / 16.0);
	check(gcx != hcx || gcz != hcz, "fixture: the guest chunk differs from the host chunk");
	run_ticks(s, 3);
	struct server_world_service_stats ow, ne;
	server_local_world_service_stats(s, 0, &ow);
	server_local_world_service_stats(s, 1, &ne);
	check(ow.random_tick_passes > 0 && ow.random_tick_cx == gcx && ow.random_tick_cz == gcz,
		  "M5: the Overworld random-ticks around the GUEST (its own player)");
	check(ne.random_tick_passes > 0 && ne.random_tick_cx == hcx && ne.random_tick_cz == hcz,
		  "M5: the Nether random-ticks around the host");
	if(server_local_two_anchor(s))
		check(s->residency_anchors[0].x == gcx && s->residency_anchors[0].z == gcz,
			  "M5: the Overworld residency anchor 0 is the guest, not the host's Nether chunk");
	/* nobody left in the Overworld */
	guest_release(s);
	server_local_world_service_stats(s, 0, &ow);
	uint64_t before = ow.random_tick_passes;
	run_ticks(s, 3);
	server_local_world_service_stats(s, 0, &ow);
	check(ow.random_tick_passes == before,
		  "M5: an Overworld with no player of its own takes no random-tick pass");
	teardown(s);
}

/* ---- blocker 3: the real landing (no faked replies) ------------------- */
static unsigned lr_arms, lr_core_chunks;
static int32_t lr_arm_x, lr_arm_z; /* the ARM's own core centre */
static uint8_t lr_arm_radius, lr_arm_slot;
static int8_t lr_arm_dim;
static uint32_t lr_arm_gen;
/* O10 / R05 P2: the guest-binding sequences the REAL producers stamped, as
 * the client consumed them: the gate-1 guest reset and the GAMEPLAY_READY. */
static unsigned lr_g1_n;
static uint32_t lr_g1_seq, lr_ready_seq;
static void lr_bind_seqs(const struct client_rpc* call) {
	if(call->type == CRPC_WORLD_RESET && call->payload.world_reset.guest_world
	   && call->payload.world_reset.guest_view == 1) {
		lr_g1_n++;
		lr_g1_seq = call->payload.world_reset.guest_bind_seq;
	}
	if(call->type == CRPC_LOCAL_GUEST
	   && call->payload.local_guest.result == LOCAL_GUEST_GAMEPLAY_READY)
		lr_ready_seq = call->payload.local_guest.guest_bind_seq;
}
static void lr_observer(const struct client_rpc* call, void* user) {
	consumed(call, user);
	lr_bind_seqs(call);
	if(call->type == CRPC_LANDING_ARM && !call->payload.landing_arm.cancel) {
		lr_arms++;
		lr_arm_radius = call->payload.landing_arm.radius;
		lr_arm_dim = call->payload.landing_arm.dimension;
		lr_arm_slot = call->world_index;
		lr_arm_gen = call->world_generation;
		lr_arm_x = call->payload.landing_arm.x;
		lr_arm_z = call->payload.landing_arm.z;
		lr_core_chunks = 0;
	}
	if(call->type == CRPC_CHUNK && lr_arm_gen
	   && call->world_index == lr_arm_slot
	   && call->world_generation == lr_arm_gen)
		lr_core_chunks++;
}
/* every column of the 3x3 core around (x, z) has an installed section in the
 * client payload of `slot` */
static bool lr_core_installed(unsigned slot, double x, double z) {
	struct clin_world_payload p;
	if(!clin_world_payload_of_slot(slot, &p) || !p.terrain)
		return false;
	w_coord_t cx = (w_coord_t)floor(floor(x) / 16.0);
	w_coord_t cz = (w_coord_t)floor(floor(z) / 16.0);
	for(int dx = -1; dx <= 1; dx++)
		for(int dz = -1; dz <= 1; dz++) {
			bool any = false;
			for(w_coord_t y = 0; y < WORLD_HEIGHT && !any; y += CHUNK_SIZE)
				any = world_find_chunk(p.terrain, (cx + dx) * CHUNK_SIZE, y,
									   (cz + dz) * CHUNK_SIZE) != NULL;
			if(!any)
				return false;
		}
	return true;
}
/* Unpaused real updates until `id` is terminal; records whether the client's
 * own READY was seen and whether the prepared slot held the core before any
 * view showed it.
 * O15: those facts are recorded AT the client's real READY report (the
 * production sender, observed through clin_landing_set_sender and forwarded
 * unchanged), on the thread that sends it. They used to be sampled after a
 * whole test tick(); tick() drains the client on a concurrent consumer thread
 * DURING the server update, so READY and the server's COMMIT could both land
 * inside one tick and the post-tick sample saw the transaction already closed
 * (core_before_commit=false) -- a scheduling-dependent false failure that CPU
 * load made likely (test_a2_transfer_*landing_real, O14). A server commit
 * needs that READY, so a fact true at the send is true before the commit. */
struct lr_trace {
	bool saw_landing, saw_ready, core_before_commit, prepared_unviewed;
	enum server_transfer_state final;
};
static struct {
	unsigned arms0, actor_slot;
	bool seen, core, unviewed;
} lr_ready;
static unsigned lr_ticks_per_sample = 1; /* the coarse regression case: 2 */
static bool lr_ready_observer(uint32_t token, unsigned slot, uint32_t generation) {
	bool armed_now = lr_arms > lr_ready.arms0 && slot == lr_arm_slot
		&& generation == lr_arm_gen;
	bool core = false, unviewed = false;
	if(armed_now && !lr_ready.seen) {
		const struct client_world_context* ctx
			= client_ctx_registry_current(clin_world_contexts(), slot);
		core = lr_core_installed(slot, lr_arm_x, lr_arm_z);
		unviewed = ctx && ctx->server_generation == generation
			&& (slot == clin_world_payload_primary_slot()
				|| !(ctx->bound_views
					 & (lr_ready.actor_slot == ACTOR_SLOT_HOST ? 1u : 2u)));
	}
	bool sent = clin_landing_ready_send_default(token, slot, generation);
	if(sent && armed_now && !lr_ready.seen) {
		lr_ready.seen = true;
		lr_ready.core = core;
		lr_ready.unviewed = unviewed;
	}
	return sent;
}
static struct lr_trace lr_drive(struct server_local* s, uint32_t id,
								unsigned actor_slot) {
	struct lr_trace tr = {0};
	/* READY counts only for a landing armed during THIS drive (a previous
	 * transaction's READY state lingers until the next ARM) */
	const unsigned arms0 = lr_arms;
	memset(&lr_ready, 0, sizeof(lr_ready));
	lr_ready.arms0 = arms0;
	lr_ready.actor_slot = actor_slot;
	clin_landing_set_sender(lr_ready_observer);
	bool was_paused = s->paused;
	s->paused = false;
	for(unsigned i = 0; i < MAX_STEPS; i++) {
		enum server_transfer_state st = server_transfer_state_of(s, id);
		if(st == SERVER_TRANSFER_LANDING)
			tr.saw_landing = true;
		if(st == SERVER_TRANSFER_COMMITTED || st == SERVER_TRANSFER_ROLLED_BACK) {
			tr.final = st;
			break;
		}
		for(unsigned k = 0; k < lr_ticks_per_sample; k++) {
			tick(s);
			client_acks_marker(s);
		}
	}
	s->paused = was_paused;
	clin_landing_set_sender(NULL);
	/* The server sends a landing ARM only on entering LANDING: an ARM for this
	 * drive is that state change, whatever the sampling granularity. */
	if(lr_arms > arms0)
		tr.saw_landing = true;
	tr.saw_ready = lr_ready.seen;
	tr.core_before_commit = lr_ready.core;
	tr.prepared_unviewed = lr_ready.unviewed;
	return tr;
}
static void case_landing_real(void) {
	check(clin_world_payloads_enabled(), "fixture: world-bound payloads ON");
	struct server_local* s = two_world_fixture();
	clin_test_set_consume_observer(lr_observer, NULL);
	for(unsigned leg = 0; leg < 2; leg++) {
		enum world_dim to = leg == 0 ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
		const char* dir = leg == 0 ? "Overworld -> Nether" : "Nether -> Overworld";
		char msg[160];
		uint32_t id = 0;
		unsigned arms = lr_arms;
		check(server_transfer_request(s, &s->player, to, &id) == SERVER_TRANSFER_OK,
			  "host: the request is accepted");
		struct lr_trace tr = lr_drive(s, id, ACTOR_SLOT_HOST);
		struct server_world* dst = world_of(s, &s->player);
		snprintf(msg, sizeof(msg), "%s: the host waited in LANDING for its client", dir);
		check(tr.saw_landing && lr_arms == arms + 1, msg);
		snprintf(msg, sizeof(msg), "%s: the ARM names the 3x3 core of the destination "
				 "(radius %d, dimension %d)", dir, SERVER_TRANSFER_LANDING_CORE_RADIUS, (int)to);
		check(lr_arm_radius == SERVER_TRANSFER_LANDING_CORE_RADIUS && lr_arm_dim == (int8_t)to,
			  msg);
		/* leg 1 lands in a world nobody's client holds: the whole core must be
		 * streamed. Leg 2 returns to the Overworld, whose payload the client
		 * still holds around the old position: READY may come as soon as the
		 * core is present, streamed or already there (count printed). */
		snprintf(msg, sizeof(msg), "%s: core columns streamed stamped with the "
				 "destination world before READY (%u)", dir, lr_core_chunks);
		check(leg == 0 ? lr_core_chunks >= SERVER_TRANSFER_LANDING_CORE_COLUMNS
					   : lr_core_chunks >= 1,
			  msg);
		snprintf(msg, sizeof(msg), "%s: the client reported READY by itself with the core "
				 "installed and no host view on the destination", dir);
		check(tr.saw_ready && tr.core_before_commit && tr.prepared_unviewed, msg);
		snprintf(msg, sizeof(msg), "%s: COMMITTED with no faked reply", dir);
		check(tr.final == SERVER_TRANSFER_COMMITTED && dst && dst->dimension == to
				  && s->player.dimension == to && lr_arm_slot == dst->world_index
				  && lr_arm_gen == dst->generation,
			  msg);
		clin_update_drain();
		const struct client_world_context* ctx
			= client_ctx_registry_current(clin_world_contexts(), dst->world_index);
		snprintf(msg, sizeof(msg), "%s: view 0 JOINED the prepared context -- the core is "
				 "still installed after the host's WORLD_RESET", dir);
		check(ctx && ctx->server_generation == dst->generation && (ctx->bound_views & 1u)
				  && clin_world_payload_primary_slot() == dst->world_index
				  && clin_world_payload_prepared(dst->world_index) == 0
				  && lr_core_installed(dst->world_index, s->player.x, s->player.z),
			  msg);
		/* the vacated world drains before the next leg */
		run_ticks(s, 200);
		clin_update_drain();
	}
	teardown(s);
}
static void case_guest_landing_real(void) {
	check(clin_world_payloads_enabled(), "fixture: world-bound payloads ON");
	/* O10 / R05 P2 + P3 wrap, end to end through the REAL producers: seed the
	 * process-wide counter so the fixture's admission bump mints UINT32_MAX
	 * and the guest transfer's gate-1 bump wraps to 1. The client has applied
	 * UINT32_MAX; only serial-number ordering lets the wrapped gate-1 reset
	 * and READY move the guest (an ordinary `<` fence drops both). */
	server_local_test_guest_bind_seq_set(UINT32_MAX - 1u);
	struct server_local* s = two_world_fixture();
	check(server_local_guest_bind_seq() == UINT32_MAX,
		  "bind seq: the real admission bump minted UINT32_MAX");
	clin_test_set_consume_observer(lr_observer, NULL);
	struct clin_world_payload start_payload;
	check(clin_world_payload_of_slot(0, &start_payload)
		  && gstate.players[1].local_player
		  && gstate.players[1].local_player->world == start_payload.terrain
		  && gstate.players[1].local_gameplay_ready
		  && clin_world_view_slot(1) == 0,
		  "guest: real initial READY installs gameplay body and view in Overworld");
	uint32_t id = 0;
	check(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK,
		  "guest: the request is accepted");
	double held_guest_x = s->guest_actor->x;
	bool held_was_paused = s->paused;
	s->paused = false;
	tick(s); /* real BEGIN is enqueued before source preparation */
	s->paused = held_was_paused;
	clin_update_drain();
	check(server_transfer_actor_frozen(s, s->guest_actor)
		  && clin_transfer_actor_held(1) && !clin_transfer_actor_held(0),
		  "guest: transfer BEGIN holds only the moving owner");
	size_t held_hotbar = inventory_get_hotbar(&s->guest_actor->inventory);
	uint32_t held_recipe_ticket = s->guest_actor->recipe_craft_ticket + 1;
	struct server_rpc held_slot = {.type = SRPC_HOTBAR_SLOT,
		.payload.hotbar_slot = {.slot = (held_hotbar + 1) % INVENTORY_SIZE_HOTBAR}};
	struct server_rpc held_recipe = {.type = SRPC_RECIPE_CRAFT,
		.payload.recipe_craft = {.ticket = held_recipe_ticket,
			.recipe_index = UINT16_MAX, .player = 1,
			.window = WINDOWC_INVENTORY}};
	struct server_rpc_source guest_source = {.slot = 1,
		.generation = s->guest_generation};
	short held_health = s->guest_actor->health;
	int held_fire = s->guest_actor->fire = 20;
	int held_hurt = s->guest_actor->hurt_resist_time = 5;
	int held_age = s->guest_actor->ticks_existed;
	int live_host_age = s->player.ticks_existed;
	check(svin_rpc_send_from(guest_source, &held_slot)
		  && svin_rpc_send_from(guest_source, &held_recipe),
		  "guest: stale owned action and recipe enter the HOLD FIFO");
	s->paused = false;
	tick(s);
	s->paused = held_was_paused;
	check(inventory_get_hotbar(&s->guest_actor->inventory) == held_hotbar
		  && s->guest_actor->recipe_craft_ticket == held_recipe_ticket
		  && s->guest_actor->recipe_craft_reason == RECIPE_CRAFT_BUSY,
		  "guest: HOLD rejects stale action and answers recipe busy");
	check(s->guest_actor->health == held_health && s->guest_actor->fire == held_fire
		  && s->guest_actor->hurt_resist_time == held_hurt
		  && s->guest_actor->ticks_existed == held_age
		  && s->player.ticks_existed == live_host_age + 1,
		  "guest: held body's hazards and age pause while host gameplay ticks");
	s->guest_actor->fire = 0;
	s->guest_actor->hurt_resist_time = 0;
	guest_sample(s, held_guest_x + 0.1, s->guest_actor->y,
			s->guest_actor->z, true, s->guest_actor->correction_seq);
	check(s->guest_actor->x == held_guest_x,
		  "guest: authoritative position sample cannot move held source body");
	struct lr_trace tr = lr_drive(s, id, ACTOR_SLOT_GUEST);
	check(tr.saw_landing && lr_arm_radius == SERVER_TRANSFER_LANDING_CORE_RADIUS
			  && lr_arm_dim == (int8_t)WORLD_DIM_NETHER && lr_arm_slot == 1,
		  "guest: the ARM names the Nether's 3x3 core (slot 1)");
	check(lr_core_chunks >= SERVER_TRANSFER_LANDING_CORE_COLUMNS,
		  "guest: the core was streamed stamped with the Nether world");
	check(tr.saw_ready && tr.core_before_commit && tr.prepared_unviewed,
		  "guest: the client reported READY by itself -- core installed in the "
		  "unviewed prepared slot, no faked reply");
	clin_update_drain();
	check(!clin_transfer_actor_held(1),
		  "guest: matching END releases local movement hold after COMMIT");
	if(tr.final == SERVER_TRANSFER_COMMITTED) {
		check(s->a2w_guest_presentation
				&& server_transfer_actor_inert(s, s->guest_actor),
			"guest: COMMIT retains owner-only body hold until gameplay READY");
		int held_age = s->guest_actor->ticks_existed;
		int live_age = s->player.ticks_existed;
		bool paused_before = s->paused;
		s->paused = false;
		tick(s);
		s->paused = paused_before;
		check(s->guest_actor->ticks_existed == held_age
				&& s->player.ticks_existed == live_age + 1,
			"guest: post-COMMIT body age waits while host remains live");
		struct entity held_item = {0};
		entity_item(0x770004, &held_item, true, &s->worlds[1],
			(struct item_data){.id = ITEM_STICK, .count = 1});
		held_item.teleport(&held_item, (vec3){(float)s->guest_actor->x,
			(float)s->guest_actor->y - 1.0F,
			(float)s->guest_actor->z});
		uint64_t guest_inventory = inventory_hash(s->guest_actor);
		check(!held_item.tick_server(&held_item, s)
				&& held_item.delay_destroy == -1
				&& inventory_hash(s->guest_actor) == guest_inventory,
			"guest: post-COMMIT item cannot enter unseen inventory");
		short guest_health = s->guest_actor->health;
		float guest_vx = s->guest_actor->vel_x;
		double guest_y = s->guest_actor->y;
		/* The blast publishes each changed block into the bounded client inbox.
		 * Keep the real client consumer active while the server owns this call. */
		struct thread consumer;
		__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
		assert(thread_create(&consumer, consumer_entry, NULL, 64));
		server_world_explode(&s->worlds[1],
			(vec3){(float)s->guest_actor->x + 0.5F,
				(float)s->guest_actor->y,
				(float)s->guest_actor->z + 0.5F}, 2.0F);
		__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
		thread_join(&consumer);
		clin_update_drain();
		check(s->guest_actor->health == guest_health
				&& s->guest_actor->vel_x == guest_vx
				&& s->guest_actor->y == guest_y,
			"guest: post-COMMIT explosion cannot hit unseen held body");
		const struct client_world_context* ctx
			= client_ctx_registry_current(clin_world_contexts(), 1);
		check(world_of(s, s->guest_actor) == &s->worlds[1] && ctx
				  && (ctx->bound_views & 2u) && clin_world_payload_prepared(1) == 0,
			  "guest: COMMITTED (lane C adapter) and view 1 binds the prepared slot");
		check(!gstate.players[1].local_gameplay_ready
			  && !gstate.players[1].local_player,
			  "guest: gate-1 waits for a real post-COMMIT gameplay reply");
		clin_update_drain();
		clin_test_fail_entity_slot_after(1);
		check(client_session_request_local_gameplay(&gstate.players[1]),
			  "guest: main's public gameplay re-request is accepted after COMMIT");
		pump_handshake(s);
		check(!gstate.players[1].local_gameplay_ready
				&& !gstate.players[1].local_player
				&& s->a2w_guest_presentation,
			"guest: transient READY install refusal keeps server body inert");
		check(client_session_request_local_gameplay(&gstate.players[1]),
			"guest: transient READY can request owner-local retry");
		pump_handshake(s);
		check(s->a2w_guest_presentation
				&& server_transfer_actor_inert(s, s->guest_actor),
			"guest: READY send alone cannot release the unseen body hold");
		struct clin_world_payload nether_payload;
		check(clin_world_payload_of_slot(1, &nether_payload)
			  && gstate.players[1].local_player
			  && gstate.players[1].local_player->world == nether_payload.terrain
			  && gstate.players[1].local_gameplay_ready
			  && clin_world_view_slot(1) == 1,
			  "guest: real COMMIT restores playable body in Nether store");
		uint32_t fresh_seq = s->guest_actor->correction_seq;
		check(fresh_seq != 0
				&& gstate.players[1].player_sync.correction_seq == fresh_seq,
			"guest: installed READY carries the fresh COMMIT correction token");
		check(lr_g1_n >= 1 && lr_g1_seq == 1u
				&& server_local_guest_bind_seq() == 1u,
			"bind seq: the real gate-1 guest reset stamped the bump's wrap "
			"successor (UINT32_MAX -> 1)");
		check(lr_ready_seq == lr_g1_seq,
			"bind seq: the post-COMMIT READY carries the gate-1 binding");
		double active_x = s->guest_actor->x;
		guest_sample(s, active_x + 0.1, s->guest_actor->y,
			s->guest_actor->z, true, fresh_seq - 1);
		check(s->a2w_guest_presentation && s->guest_actor->x == active_x,
			"guest: stale source token cannot release COMMIT presentation");
		guest_sample(s, active_x + 1000.0, s->guest_actor->y,
			s->guest_actor->z, true, fresh_seq);
		check(s->a2w_guest_presentation && s->guest_actor->x == active_x,
			"guest: invalid-delta fresh token cannot release presentation");
		fresh_seq = s->guest_actor->correction_seq;
		guest_sample(s, active_x + 0.1, s->guest_actor->y,
			s->guest_actor->z, true, fresh_seq);
		check(!s->a2w_guest_presentation
				&& !server_transfer_actor_inert(s, s->guest_actor),
			"guest: valid owner pose with fresh token releases presentation");
		check(!held_item.tick_server(&held_item, s)
				&& held_item.delay_destroy > 0
				&& inventory_hash(s->guest_actor) != guest_inventory,
			"guest: same-world item is collectible after valid owner echo");
		run_ticks(s, 50);
		check(s->residency_anchors[0].dimension == WORLD_DIM_OVERWORLD
			  && s->residency_anchors[1].dimension == WORLD_DIM_NETHER,
			  "guest: residency anchors follow actual host and Nether guest worlds");
		bool guest_ghost_debt = false;
		for(unsigned b = 0; b < RESIDENCY_BUCKETS; b++)
			for(unsigned i = 0; i < RESIDENCY_ANCHORS; i++) {
				const struct residency_debt* d = &s->residency->debt[b][i];
				guest_ghost_debt |= d->used
					&& d->dimension == WORLD_DIM_OVERWORLD
					&& ((d->near_owners | d->far_owners) & (1u << 1));
			}
		check(!guest_ghost_debt,
			  "guest: Nether body owns no stale Overworld residency debt");
		uint32_t back = 0;
		check(server_transfer_request(s, s->guest_actor, WORLD_DIM_OVERWORLD, &back)
				  == SERVER_TRANSFER_OK,
			  "guest: the way back is accepted");
		struct lr_trace tb = lr_drive(s, back, ACTOR_SLOT_GUEST);
		const struct client_world_context* home
			= client_ctx_registry_current(clin_world_contexts(), 0);
		check(tb.final == SERVER_TRANSFER_COMMITTED && !tb.saw_landing
				  && world_of(s, s->guest_actor) == &s->worlds[0]
				  && home && home->server_generation == s->worlds[0].generation
				  && (home->bound_views & 2u)
				  && lr_core_installed(0, s->guest_actor->x, s->guest_actor->z),
			  "guest: Nether -> existing Overworld COMMITTED into the host's "
			  "installed, generation-matched core");
		check(client_session_request_local_gameplay(&gstate.players[1]),
			  "guest: public gameplay re-request is accepted after return COMMIT");
		pump_handshake(s);
		struct clin_world_payload home_payload;
		check(clin_world_payload_of_slot(0, &home_payload)
			  && gstate.players[1].local_player
			  && gstate.players[1].local_player->world == home_payload.terrain
			  && gstate.players[1].local_gameplay_ready
			  && clin_world_view_slot(1) == 0,
			  "guest: real return COMMIT restores playable body in host store");
		run_ticks(s, 1);
		check(s->residency_anchors[0].dimension == WORLD_DIM_OVERWORLD
			  && s->residency_anchors[1].dimension == WORLD_DIM_OVERWORLD,
			  "guest: return moves both residency anchors to Overworld");
		/* Now player 0 leaves the shared world. Its stamped reset must not
		 * retire player 1's admitted body or borrow view 1 into the Nether. */
		struct entity* surviving_body = gstate.players[1].local_player;
		uint32_t host_id = 0;
		check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER,
										  &host_id) == SERVER_TRANSFER_OK,
			  "host: transfer accepted with guest back in Overworld");
		struct lr_trace host_leg = lr_drive(s, host_id, ACTOR_SLOT_HOST);
		clin_update_drain();
		check(host_leg.final == SERVER_TRANSFER_COMMITTED
			  && gstate.players[1].local_player == surviving_body
			  && surviving_body && surviving_body->world == home_payload.terrain
			  && gstate.players[1].local_gameplay_ready
			  && clin_world_view_slot(0) == 1 && clin_world_view_slot(1) == 0,
			  "host reset preserves guest body, gameplay and Overworld view");
		run_ticks(s, 1);
		check(s->residency_anchors[0].dimension == WORLD_DIM_NETHER
			  && s->residency_anchors[1].dimension == WORLD_DIM_OVERWORLD,
			  "host away: residency anchors follow Nether host and Overworld guest");
		guest_ghost_debt = false;
		for(unsigned b = 0; b < RESIDENCY_BUCKETS; b++)
			for(unsigned i = 0; i < RESIDENCY_ANCHORS; i++) {
				const struct residency_debt* d = &s->residency->debt[b][i];
				guest_ghost_debt |= d->used
					&& d->dimension == WORLD_DIM_NETHER
					&& ((d->near_owners | d->far_owners) & (1u << 1));
			}
		check(!guest_ghost_debt,
			  "host away: Overworld guest owns no stale Nether residency debt");
		double guest_x = s->guest_actor->x;
		guest_sample(s, guest_x + 0.1, s->guest_actor->y,
				s->guest_actor->z, true, s->guest_actor->correction_seq);
		check(s->guest_actor->x == guest_x + 0.1,
			  "guest: own position route still works after host reset");
		/* A real owner-routed dig after the host leaves must edit only the
		 * guest's committed world at a coordinate both worlds have resident. */
		w_coord_t bx = (w_coord_t)floor(s->guest_actor->x);
		w_coord_t by = (w_coord_t)floor(s->guest_actor->y) - 2;
		w_coord_t bz = (w_coord_t)floor(s->guest_actor->z);
		struct server_world* guest_world = world_of(s, s->guest_actor);
		struct server_world* host_world = world_of(s, &s->player);
		check(server_world_set_block(guest_world, bx, by, bz,
								   (struct block_data){.type = BLOCK_STONE})
			  && server_world_set_block(host_world, bx, by, bz,
								   (struct block_data){.type = BLOCK_DIRT}),
			  "fixture: matching coordinate is writable in both worlds");
		struct server_rpc dig = {.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {.finished = true, .x = bx, .y = by,
				.z = bz, .side = SIDE_TOP}};
		check(guest_send(s, &dig), "guest: owner-routed dig is admitted");
		struct block_data guest_block, host_block;
		check(server_world_get_block(guest_world, bx, by, bz, &guest_block)
			  && server_world_get_block(host_world, bx, by, bz, &host_block)
			  && guest_block.type == BLOCK_AIR && host_block.type == BLOCK_DIRT,
			  "guest: dig changes only committed Overworld, not host Nether");
	} else
		check(false, "guest: Overworld -> Nether must COMMIT after real READY");
	teardown(s);
}

/* ---- A2-W: per-world streaming after COMMIT --------------------------- */
static unsigned aw_chunks[SERVER_LOCAL_WORLD_SLOTS], aw_unloads[SERVER_LOCAL_WORLD_SLOTS];
static uint32_t aw_reserve_request, aw_landing_request;
static void aw_observer(const struct client_rpc* call, void* user) {
	lr_observer(call, user);
	if(call->type == CRPC_CAPACITY_REQUEST
	   && call->payload.capacity_request.op == CLIN_CAP_OP_RESERVE)
		aw_reserve_request = call->payload.capacity_request.request_id;
	if(call->type == CRPC_LANDING_ARM && !call->payload.landing_arm.cancel)
		aw_landing_request = call->payload.landing_arm.token;
	if(call->world_generation == 0 || call->world_index >= SERVER_LOCAL_WORLD_SLOTS)
		return;
	if(call->type == CRPC_CHUNK)
		aw_chunks[call->world_index]++;
	if(call->type == CRPC_UNLOAD_CHUNK)
		aw_unloads[call->world_index]++;
}
static w_coord_t aw_cdiv(double v) {
	return (w_coord_t)floor(v / 16.0);
}
/* every column within r of (x, z) is sent in w */
static bool aw_square_sent(struct server_world* w, double x, double z, int r) {
	for(int dz = -r; dz <= r; dz++)
		for(int dx = -r; dx <= r; dx++)
			if(!server_world_is_chunk_sent(w, aw_cdiv(x) + dx, aw_cdiv(z) + dz))
				return false;
	return true;
}
/* no resident column of w lies farther than r from (x, z) */
static bool aw_none_beyond(struct server_world* w, double x, double z, int r) {
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, w->chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it)) {
		int64_t id = dict_server_chunks_ref(it)->key;
		int dx = abs(S_CHUNK_X(id) - aw_cdiv(x)), dz = abs(S_CHUNK_Z(id) - aw_cdiv(z));
		if((dx > dz ? dx : dz) > r)
			return false;
	}
	return true;
}
static void aw_run_until(struct server_local* s, bool (*done)(struct server_local*),
						 unsigned max) {
	bool was_paused = s->paused;
	s->paused = false;
	for(unsigned i = 0; i < max && !done(s); i++)
		tick(s);
	s->paused = was_paused;
}
static bool aw_marker_armed(struct server_local* s) {
	return s->input_ack_pending != 0;
}
static bool aw_host_square_sent(struct server_local* s) {
	struct server_world* w = server_local_world_of(s, &s->player);
	return w && aw_square_sent(w, s->player.x, s->player.z, server_local_view_radius(s))
		&& aw_none_beyond(w, s->player.x, s->player.z, server_local_view_radius(s));
}
static void case_a2w_owner_hazards(void) {
	check(clin_world_payloads_enabled(), "fixture: owner hazards use world payloads ON");
	struct server_local* s = two_world_fixture();
	clin_test_set_consume_observer(aw_observer, NULL);
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id)
			== SERVER_TRANSFER_OK && lr_drive(s, id, ACTOR_SLOT_HOST).final
				== SERVER_TRANSFER_COMMITTED,
		"owner hazards: host commits into Nether with guest in Overworld");
	struct server_world* ne = world_of(s, &s->player);
	struct server_world* ow = world_of(s, s->guest_actor);
	aw_run_until(s, aw_marker_armed, 1500);
	check(s->a2w_host_presentation && ne != ow,
		"owner hazards: only host is held during its real marker");
	int rail_x = (int)floor(s->player.x);
	int rail_z = (int)floor(s->player.z);
	int rail_y = (int)floor(s->player.y - PLAYER_EYE_HEIGHT);
	for(int dx = -2; dx <= 2; dx++) {
		check(server_world_set_block(ne, rail_x + dx, rail_y - 1, rail_z,
			(struct block_data){.type = BLOCK_STONE})
			&& server_world_set_block(ne, rail_x + dx, rail_y, rail_z,
				(struct block_data){.type = BLOCK_RAIL, .metadata = 1}),
			"owner hazards: cart rail fixture installed in host's world");
		clin_update_drain(); /* fixture writes share the 16-token client inbox */
	}
	struct entity cart_proto = {0};
	entity_minecart(0x770003, &cart_proto, true, ne);
	cart_proto.teleport(&cart_proto, (vec3){(float)s->player.x + 0.5F,
		(float)rail_y + 0.5F, (float)s->player.z + 0.1F});
	cart_proto.data.minecart.speed = 0.0F;
	cart_proto.data.minecart.hx = 1;
	cart_proto.data.minecart.hz = 0;
	struct entity held_cart = cart_proto;
	float before_cart_vx = s->player.vel_x, before_cart_vz = s->player.vel_z;
	check(!held_cart.tick_server(&held_cart, s)
			&& s->player.vel_x == before_cart_vx
			&& s->player.vel_z == before_cart_vz,
		"owner hazards: live minecart cannot shove held host");
	struct entity held_item = {0};
	entity_item(0x770001, &held_item, true, ne,
		(struct item_data){.id = ITEM_STICK, .count = 1});
	held_item.teleport(&held_item, (vec3){(float)s->player.x,
		(float)s->player.y - 1.0F, (float)s->player.z});
	uint64_t held_inventory = inventory_hash(&s->player);
	bool held_dead = held_item.tick_server(&held_item, s);
	check(!held_dead
			&& held_item.delay_destroy == -1
			&& inventory_hash(&s->player) == held_inventory,
		"owner hazards: item cannot enter held host inventory");
	float held_vx = s->player.vel_x, held_vz = s->player.vel_z;
	short held_health = s->player.health;
	double held_y = s->player.y;
	/* A single blast may publish more than the 16 available client tokens.
	 * Match the guest blast fixture's real-client consumer during this call. */
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	server_world_explode(ne, (vec3){(float)s->player.x + 0.5F,
		(float)s->player.y, (float)s->player.z + 0.5F}, 2.0F);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
	check(s->player.health == held_health && s->player.vel_x == held_vx
			&& s->player.vel_z == held_vz && s->player.y == held_y,
		"owner hazards: Nether explosion cannot damage or impulse held host");
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	tick(s);
	check(!s->a2w_host_presentation && s->input_ack_pending == 0,
		"owner hazards: real client ACK releases host hold");
	/* The held-side blast destroyed the first rail. Build the identical
	 * geometry one loaded column east and prove the contact is live after ACK. */
	double contact_x = s->player.x;
	s->player.x += CHUNK_SIZE;
	int live_rail_x = (int)floor(s->player.x);
	for(int dx = -2; dx <= 2; dx++) {
		check(server_world_set_block(ne, live_rail_x + dx, rail_y - 1, rail_z,
			(struct block_data){.type = BLOCK_STONE})
			&& server_world_set_block(ne, live_rail_x + dx, rail_y, rail_z,
				(struct block_data){.type = BLOCK_RAIL, .metadata = 1}),
			"owner hazards: same rail geometry installed outside blast");
		clin_update_drain(); /* keep the fixture consumer live during setup */
	}
	struct entity live_cart = cart_proto;
	live_cart.teleport(&live_cart, (vec3){(float)s->player.x + 0.5F,
		(float)rail_y + 0.5F, (float)s->player.z + 0.1F});
	check(!live_cart.tick_server(&live_cart, s)
			&& (s->player.vel_x != before_cart_vx
				|| s->player.vel_z != before_cart_vz),
		"owner hazards: same cart geometry shoves host after exact ACK");
	s->player.x = contact_x;
	s->player.vel_x = before_cart_vx;
	s->player.vel_z = before_cart_vz;
	check(!held_item.tick_server(&held_item, s)
			&& held_item.delay_destroy > 0
			&& inventory_hash(&s->player) != held_inventory,
		"owner hazards: same Nether item is collectible after exact ACK");
	/* Equal coordinates do not make an Overworld item belong to the host's
	 * Nether. Keep the guest away for the first tick, then admit it locally. */
	double gx = s->guest_actor->x, gy = s->guest_actor->y, gz = s->guest_actor->z;
	double hx = s->player.x, hy = s->player.y, hz = s->player.z;
	s->player.x = gx; s->player.y = gy; s->player.z = gz;
	s->guest_actor->x += 100.0;
	struct entity ow_item = {0};
	entity_item(0x770002, &ow_item, true, ow,
		(struct item_data){.id = ITEM_STICK, .count = 1});
	ow_item.teleport(&ow_item, (vec3){(float)gx, (float)gy - 1.0F, (float)gz});
	uint64_t host_before = inventory_hash(&s->player);
	uint64_t guest_before = inventory_hash(s->guest_actor);
	bool other_dead = ow_item.tick_server(&ow_item, s);
	check(!other_dead && ow_item.delay_destroy == -1
			&& inventory_hash(&s->player) == host_before,
		"owner hazards: host cannot collect an overlapping other-world item");
	s->guest_actor->x = gx;
	check(!ow_item.tick_server(&ow_item, s) && ow_item.delay_destroy > 0
			&& inventory_hash(s->guest_actor) != guest_before
			&& inventory_hash(&s->player) == host_before,
		"owner hazards: only same-world guest collects the overlapping item");
	s->player.x = hx; s->player.y = hy; s->player.z = hz;
	s->guest_actor->y = gy; s->guest_actor->z = gz;
	teardown(s);
}

/* The host's own terrain marker may wait while the guest starts another
 * transfer. Its capacity/landing replies are control traffic, not host input. */
static void case_owner_loading_concurrent_transfer(void) {
	check(clin_world_payloads_enabled(), "concurrent: world payloads ON");
	struct server_local* s = two_world_fixture();
	clin_test_set_consume_observer(aw_observer, NULL);
	uint32_t guest_out = 0;
	check(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER,
		&guest_out) == SERVER_TRANSFER_OK,
		"concurrent: guest requests Nether before host transfer");
	struct lr_trace first = lr_drive(s, guest_out, ACTOR_SLOT_GUEST);
	check(first.final == SERVER_TRANSFER_COMMITTED,
		"concurrent: guest commits into Nether through real admission");
	if(first.final != SERVER_TRANSFER_COMMITTED) goto done;
	clin_update_drain();
	check(client_session_request_local_gameplay(&gstate.players[1]),
		"concurrent: guest requests its committed gameplay body");
	pump_handshake(s);
	check(gstate.players[1].local_gameplay_ready
			&& s->guest_actor->finished_loading
			&& !server_transfer_actor_frozen(s, s->guest_actor),
		"concurrent: READY installed and transfer HOLD ended before pose echo");
	/* READY can drain in the update that raised the actor's one-tick input
	 * barrier. Advance that ordinary server tick before the client's pose. */
	run_ticks(s, 1);
	uint32_t fresh = s->guest_actor->correction_seq;
	guest_sample(s, s->guest_actor->x + 0.1, s->guest_actor->y,
		s->guest_actor->z, true, fresh);
	check(!s->a2w_guest_presentation && s->guest_actor->finished_loading,
		"concurrent: valid guest pose releases its initial presentation");
	if(s->a2w_guest_presentation) goto done;
	uint32_t host_out = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER,
		&host_out) == SERVER_TRANSFER_OK,
		"concurrent: host requests guest's live Nether world");
	/* Do not use lr_drive here: its usual load-screen helper would enqueue
	 * a host ACK immediately after COMMIT, defeating this overlap fixture. */
	bool host_was_paused = s->paused;
	s->paused = false;
	enum server_transfer_state host_state = SERVER_TRANSFER_IDLE;
	for(unsigned i = 0; i < MAX_STEPS; i++) {
		host_state = server_transfer_state_of(s, host_out);
		if(host_state == SERVER_TRANSFER_COMMITTED
		   || host_state == SERVER_TRANSFER_ROLLED_BACK) break;
		tick(s);
	}
	s->paused = host_was_paused;
	check(host_state == SERVER_TRANSFER_COMMITTED,
		"concurrent: host commits into guest's live Nether world");
	if(host_state != SERVER_TRANSFER_COMMITTED) goto done;
	aw_run_until(s, aw_marker_armed, 1500);
	uint32_t host_epoch = s->input_ack_pending;
	check(host_epoch && s->a2w_host_presentation
			&& server_transfer_guest_continuity(s),
		"concurrent: host marker waits while admitted guest stays live");
	if(!host_epoch) goto done;
	bool vacancy_was_paused = s->paused;
	s->paused = false;
	for(unsigned i = 0; i < 100
		&& dict_server_chunks_size(s->worlds[0].chunks) != 0; i++)
		tick(s);
	s->paused = vacancy_was_paused;
	check(dict_server_chunks_size(s->worlds[0].chunks) == 0,
		"concurrent: ordinary vacant Overworld drains before a new transfer");
	uint32_t guest_back = 0;
	check(server_transfer_request(s, s->guest_actor, WORLD_DIM_OVERWORLD,
		&guest_back) == SERVER_TRANSFER_OK,
		"concurrent: guest can request unviewed Overworld while host waits");
	enum server_transfer_state state = SERVER_TRANSFER_IDLE;
	bool was_paused = s->paused;
	s->paused = false;
	for(unsigned i = 0; i < MAX_STEPS; i++) {
		state = server_transfer_state_of(s, guest_back);
		if(state == SERVER_TRANSFER_COMMITTED
		   || state == SERVER_TRANSFER_ROLLED_BACK) break;
		tick(s); /* the client drains replies; the host marker stays unacknowledged */
	}
	s->paused = was_paused;
	/* The real client can reply and advance CAPACITY inside one tick. Record
	 * this transaction's actual control messages, not a post-tick sample of a
	 * transient state (the same rule as lr_drive's READY observer above). */
	check(aw_reserve_request == guest_back,
		"concurrent: guest really needed a capacity reply");
	check(aw_landing_request == guest_back,
		"concurrent: guest really needed a landing reply");
	check(state == SERVER_TRANSFER_COMMITTED
			&& world_of(s, s->guest_actor) == &s->worlds[0],
		"concurrent: guest commits despite the host's pending ACK");
	check(s->input_ack_pending == host_epoch && s->a2w_host_presentation,
		"concurrent: guest control replies cannot release the host marker");
done:
	teardown(s);
}

static void case_a2w_walk(void) {
	check(clin_world_payloads_enabled(), "fixture: world-bound payloads ON");
	struct server_local* s = two_world_fixture();
	clin_test_set_consume_observer(aw_observer, NULL);
	const int r = server_local_view_radius(s);
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK,
		  "the host requests the Nether");
	bool start_paused = s->paused;
	s->paused = false;
	tick(s); /* BEGIN arms the owner-specific source-body HOLD */
	check(server_transfer_actor_frozen(s, &s->player)
			&& server_transfer_state_of(s, id) != SERVER_TRANSFER_COMMITTED,
		"A2-W: host source body is held before COMMIT");
	short source_health = s->player.health;
	int source_fire = s->player.fire = 20;
	int source_hurt = s->player.hurt_resist_time = 5;
	int source_age = s->player.ticks_existed;
	int partner_age = s->guest_actor->ticks_existed;
	double source_x = s->player.x, source_y = s->player.y;
	tick(s);
	s->paused = start_paused;
	check(s->player.health == source_health && s->player.fire == source_fire
			&& s->player.hurt_resist_time == source_hurt
			&& s->player.ticks_existed == source_age
			&& s->player.x == source_x && s->player.y == source_y
			&& s->guest_actor->ticks_existed == partner_age + 1,
		"A2-W: pre-COMMIT host body inert while admitted guest advances");
	s->player.fire = 0;
	s->player.hurt_resist_time = 0;
	struct lr_trace tr = lr_drive(s, id, ACTOR_SLOT_HOST);
	check(tr.final == SERVER_TRANSFER_COMMITTED, "the host COMMITS into the Nether");
	if(tr.final != SERVER_TRANSFER_COMMITTED) {
		teardown(s);
		return; /* preserve the first failure without testing a nonexistent leg */
	}
	struct server_world* ne = world_of(s, &s->player);
	check(ne == &s->worlds[1] && ne->dimension == WORLD_DIM_NETHER, "the host is in slot 1");
	/* the load-screen release waits for the Nether's own near ring */
	aw_run_until(s, aw_marker_armed, 1500);
	check(s->input_ack_pending != 0, "A2-W: the host's marker is sent");
	check(aw_square_sent(ne, s->player.x, s->player.z, 2),
		  "A2-W: ...only after the near ring of the host's OWN world was streamed");
	/* A host-only presentation ACK must not pause the admitted guest's world.
	 * Beta's dedicated server ticks Overworld then Nether at 50 ms independently
	 * of one client's terrain screen (MinecraftServer.doTick:321-333). */
	uint32_t ow_before_ack = s->worlds[0].current_tick;
	uint32_t ne_before_ack = ne->current_tick;
	uint32_t host_epoch = s->input_ack_pending;
	short host_health = s->player.health;
	int host_fire = s->player.fire;
	int host_hurt = s->player.hurt_resist_time;
	int host_oxygen = s->player.oxygen;
	int host_age = s->player.ticks_existed;
	double host_x = s->player.x, host_y = s->player.y, host_z = s->player.z;
	size_t host_hotbar = inventory_get_hotbar(&s->player.inventory);
	size_t guest_hotbar = inventory_get_hotbar(&s->guest_actor->inventory);
	struct server_rpc guest_slot = {.type = SRPC_HOTBAR_SLOT,
		.payload.hotbar_slot = {.slot = (guest_hotbar + 1) % INVENTORY_SIZE_HOTBAR}};
	struct server_rpc host_slot = {.type = SRPC_HOTBAR_SLOT,
		.payload.hotbar_slot = {.slot = (host_hotbar + 1) % INVENTORY_SIZE_HOTBAR}};
	check(svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
		.generation = s->guest_generation}, &guest_slot),
		"the admitted guest can queue an owned action during host loading");
	check(svin_rpc_send(&host_slot),
		"the host action is queued for the server's presentation barrier");
	uint32_t guest_recipe_ticket = s->guest_actor->recipe_craft_ticket + 1;
	uint32_t host_recipe_ticket = s->player.recipe_craft_ticket + 1;
	struct server_rpc guest_recipe = {.type = SRPC_RECIPE_CRAFT,
		.payload.recipe_craft = {.ticket = guest_recipe_ticket,
			.recipe_index = UINT16_MAX, .player = 1,
			.window = WINDOWC_INVENTORY}};
	struct server_rpc host_recipe = {.type = SRPC_RECIPE_CRAFT,
		.payload.recipe_craft = {.ticket = host_recipe_ticket,
			.recipe_index = UINT16_MAX, .player = 0,
			.window = WINDOWC_INVENTORY}};
	check(svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
		.generation = s->guest_generation}, &guest_recipe)
			&& svin_rpc_send(&host_recipe),
		"guest and host recipe requests enter their owned FIFO routes");
	bool was_paused = s->paused;
	s->paused = false;
	tick(s);
	s->paused = was_paused;
	check(s->input_ack_pending == host_epoch,
		"the host marker still awaits its exact ACK");
	check(s->worlds[0].current_tick == ow_before_ack + 1
			&& ne->current_tick == ne_before_ack + 1,
		  "both live worlds keep one ordered tick during the host-only load ACK");
	check(inventory_get_hotbar(&s->guest_actor->inventory)
			== guest_slot.payload.hotbar_slot.slot
			&& inventory_get_hotbar(&s->player.inventory) == host_hotbar,
		"guest action applies while stale host action stays blocked");
	check(s->guest_actor->recipe_craft_ticket == guest_recipe_ticket
			&& s->guest_actor->recipe_craft_reason != RECIPE_CRAFT_BUSY
			&& s->player.recipe_craft_ticket == host_recipe_ticket
			&& s->player.recipe_craft_reason == RECIPE_CRAFT_BUSY,
		"guest recipe is answered while host recipe remains busy during presentation");
	check(s->player.health == host_health && s->player.fire == host_fire
			&& s->player.hurt_resist_time == host_hurt
			&& s->player.oxygen == host_oxygen
			&& s->player.ticks_existed == host_age
			&& s->player.x == host_x && s->player.y == host_y
			&& s->player.z == host_z,
		"held host body remains inert while both worlds tick");
	struct server_rpc wrong_ack = {.type = SRPC_INPUT_READY,
		.payload.input_ready = {.epoch = host_epoch + 1}};
	check(svin_rpc_send(&wrong_ack), "a wrong host ACK enters the FIFO");
	s->paused = false;
	uint32_t guest_tick_before_wrong = s->worlds[0].current_tick;
	tick(s);
	s->paused = was_paused;
	check(s->input_ack_pending == host_epoch && s->a2w_host_presentation
			&& s->worlds[0].current_tick == guest_tick_before_wrong + 1,
		"wrong host ACK cannot release the hold or stop guest simulation");
	struct server_rpc nack = {.type = SRPC_INPUT_READY,
		.payload.input_ready = {.epoch = host_epoch, .retry = true}};
	check(svin_rpc_send(&nack), "the exact host NACK enters the FIFO");
	s->paused = false;
	tick(s);
	s->paused = was_paused;
	check(s->a2w_host_presentation && s->player.finished_loading
			&& s->input_ack_pending == 0 && s->a2w_host_marker_pending,
		"host NACK retains owner hold and rearms its own-world marker");
	aw_run_until(s, aw_marker_armed, 1500);
	check(s->input_ack_pending != 0 && s->input_ack_pending != host_epoch,
		"host retry reaches a fresh own-world input epoch");
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	tick(s);
	check(s->input_ack_pending == 0 && !s->a2w_host_presentation,
		"the host's client acknowledges the retry marker and releases only its hold");
	aw_run_until(s, aw_host_square_sent, 3000);
	char msg[160];
	snprintf(msg, sizeof(msg), "A2-W: the host's whole radius-%d square streams in the "
			 "Nether (slot 1: %u chunks sent stamped)", r, aw_chunks[1]);
	check(aw_square_sent(ne, s->player.x, s->player.z, r), msg);
	/* the guest's Overworld area, before the walk */
	struct server_world* ow = &s->worlds[0];
	check(world_of(s, s->guest_actor) == ow, "the guest stays in the Overworld");
	double gx = s->guest_actor->x, gz = s->guest_actor->z;
	bool guest_ok = aw_square_sent(ow, gx, gz, r);
	unsigned ow_chunks = aw_chunks[0], ow_unloads = aw_unloads[0];
	/* the host walks 64 blocks east in the Nether */
	unsigned ne_unloads = aw_unloads[1], ne_evicted = s->a2w_evicted[1];
	s->player.x += 64.0;
	/* Isolate one production A2W evict pass from the primary SAVE/far paths.
	 * Make every departed column dirty, so its save must publish a delta. */
	dict_server_chunks_it_t evict_it;
	for(dict_server_chunks_it(evict_it, ne->chunks);
		!dict_server_chunks_end_p(evict_it); dict_server_chunks_next(evict_it)) {
		int64_t key = dict_server_chunks_ref(evict_it)->key;
		w_coord_t cx = S_CHUNK_X(key), cz = S_CHUNK_Z(key);
		if(abs(cx - aw_cdiv(s->player.x)) > r
		   || abs(cz - aw_cdiv(s->player.z)) > r) {
			assert(server_world_set_block(ne, cx * CHUNK_SIZE, 110,
				cz * CHUNK_SIZE, (struct block_data){.type = BLOCK_DIRT}));
			/* Direct fixture writes emit one client update per dirty column.
			 * Drain that bounded inbox before writing the next column. */
			clin_update_drain();
		}
	}
	clin_update_drain();
	/* G9 (ASTRA-G9-REVIEW Required 5): those dirty marks are block writes
	 * outside any update, so their light is pending and pins the departed
	 * columns. The production streamer must defer every pinned eviction
	 * (no UNLOAD, no save, still published), and the next production
	 * update's prescribed drain must release them. No manual drain. */
	{
		struct server_world_light_stats l0, l1;
		server_world_light_stats(&l0);
		const unsigned ev0 = s->a2w_evicted[1];
		const unsigned un0 = aw_unloads[1];
		const bool pending = server_world_light_pending(ne);
		server_transfer_stream_step(s);
		clin_update_drain();
		server_world_light_stats(&l1);
		check(pending && s->a2w_evicted[1] == ev0 && aw_unloads[1] == un0
				&& l1.evict_deferred > l0.evict_deferred,
			"G9: pinned departed columns are not evicted or unloaded while "
			"their light is pending");
		(void)server_transfer_flush_owned_batch(s, ne);
		s->paused = false;
		tick(s);
		s->paused = was_paused;
		check(!server_world_light_pending(ne),
			"G9: the next production update settles them at its drain");
		ne_evicted = s->a2w_evicted[1];
		ne_unloads = aw_unloads[1];
	}
	uint32_t journal_before[24], journal_after[24];
	struct client_rpc filler = {.type = CRPC_TRANSFER_HOLD,
		.payload.transfer_hold = {.actor_slot = 2}};
	unsigned filled = 0;
	while(filled < 1024 && clin_try_rpc_send(&filler))
		filled++;
	check(filled > 0 && filled < 1024,
		"A2-W fault fixture: bounded client inbox can be filled");
	server_transfer_stream_step(s);
	check(s->a2w_evicted[1] == ne_evicted && ne->save_batch_open
			&& s->a2w_batch_owned[1],
		"A2-W full UNLOAD inbox leaves victims resident in the bounded window");
	check(server_transfer_flush_owned_batch(s, ne) && !ne->save_batch_open,
		"A2-W empty owned window closes without publishing a victim");
	clin_update_drain();
	region_archive_journal_stats(journal_before);
	s->chunk_save_retry_delay = 2;
	server_transfer_stream_step(s);
	region_archive_journal_stats(journal_after);
	check(s->a2w_evicted[1] == ne_evicted
			&& journal_after[5] == journal_before[5]
			&& journal_after[8] == journal_before[8]
			&& !s->a2w_batch_owned[1],
		"A2-W save retry delay skips only eviction and opens no batch");
	s->chunk_save_retry_delay = 0;
	clin_update_drain();
	check(server_world_sync_regions(ne) && server_world_retiring_active(ne) == 0,
		"A2-W backpressure fixture starts with no earlier retained records");
	uint32_t refused_before = ne->retiring_refused;
	size_t retiring_bytes_before = ne->retiring_bytes;
	ne->retiring_bytes = SERVER_WORLD_RETIRING_MAX_BYTES;
	server_transfer_stream_step(s);
	check(ne->retiring_refused > refused_before && !s->a2w_batch_owned[1]
			&& !ne->save_batch_open,
		"A2-W full retiring budget closes its owned window and retains victims");
	ne->retiring_bytes = retiring_bytes_before;
	clin_update_drain();
	region_archive_journal_stats(journal_before);
	server_transfer_stream_step(s);
	region_archive_journal_stats(journal_after);
	check(s->a2w_evicted[1] > ne_evicted
			&& journal_after[8] == journal_before[8]
			&& journal_after[5] == journal_before[5]
			&& ne->save_batch_open && s->a2w_batch_owned[1],
		"A2-W dirty evictions stay unpublished in the owned window");
	unsigned paused_evictions = s->a2w_evicted[1];
	s->chunk_save_retry_delay = 1;
	s->save_batch_ticks = SAVE_BATCH_MAX_TICKS - 2;
	server_transfer_stream_step(s);
	region_archive_journal_stats(journal_after);
	check(s->a2w_evicted[1] == paused_evictions
			&& ne->save_batch_open && s->a2w_batch_owned[1]
			&& journal_after[5] == journal_before[5],
		"A2-W no-eviction tick keeps its window open before the 40-tick seam");
	s->chunk_save_retry_delay = 0;
	s->save_batch_ticks = SAVE_BATCH_MAX_TICKS - 1;
	server_transfer_stream_step(s);
	region_archive_journal_stats(journal_after);
	check(journal_after[5] > journal_before[5]
			&& !ne->save_batch_open && !s->a2w_batch_owned[1],
		"A2-W publishes the bounded window at the primary 40-tick seam");
	s->save_batch_ticks = 0;
	aw_run_until(s, aw_host_square_sent, 3000);
	snprintf(msg, sizeof(msg), "A2-W: after a 64-block walk the new square is streamed "
			 "(slot 1: %u chunks, %u unloads)", aw_chunks[1], aw_unloads[1]);
	check(aw_square_sent(ne, s->player.x, s->player.z, r), msg);
	check(aw_none_beyond(ne, s->player.x, s->player.z, r)
			  && s->a2w_evicted[1] > ne_evicted && aw_unloads[1] > ne_unloads,
		  "A2-W: the columns left behind are evicted from the Nether and unloaded "
		  "on the client (stamped slot 1)");
	check(!guest_ok || aw_square_sent(ow, gx, gz, r),
		  "A2-W: the guest's Overworld square is untouched by the host's walk");
	check(s->guest_actor->x == gx && s->guest_actor->z == gz
			  && world_of(s, s->guest_actor) == ow,
		  "the guest is where it was");
	snprintf(msg, sizeof(msg), "A2-W: steady state holds the two-world column budget "
			 "(%zu resident over all worlds, cap %u)", server_transfer_resident_columns(s),
			 A2W_STEADY_COLUMN_CAP);
	check(dict_server_chunks_size(ne->chunks) == (size_t)((2 * r + 1) * (2 * r + 1))
			  && server_transfer_resident_columns(s) <= A2W_STEADY_COLUMN_CAP,
		  msg);
	printf("slot 0 during the walk: %u chunks, %u unloads (resident %zu); slot 1 "
		   "resident %zu; all worlds %zu\n", aw_chunks[0] - ow_chunks,
		   aw_unloads[0] - ow_unloads, dict_server_chunks_size(ow->chunks),
		   dict_server_chunks_size(ne->chunks), server_transfer_resident_columns(s));
	/* A caller that already owns this world's batch must keep ownership of
	 * its publication seam across the A2W pass. */
	clin_update_drain();
	s->player.x += 64.0;
	unsigned before_outer = s->a2w_evicted[1];
	check(server_transfer_flush_owned_batch(s, ne),
		"A2-W closes its prior owned window before the outer-owner check");
	region_archive_journal_stats(journal_before);
	server_world_save_batch_begin(ne);
	server_transfer_stream_step(s);
	region_archive_journal_stats(journal_after);
	check(s->a2w_evicted[1] > before_outer && ne->save_batch_open
			&& journal_after[5] == journal_before[5],
		"A2-W preserves an outer batch and leaves publication to its owner");
	check(server_world_save_batch_end(ne) && !ne->save_batch_open,
		"A2-W outer owner closes its own batch");
	s->player.x += 64.0;
	s->save_batch_ticks = 0;
	server_transfer_stream_step(s);
	check(s->a2w_batch_owned[1] && ne->save_batch_open,
		"A2-W early-exit fixture starts a fresh owned window");
	s->a2w_reset_pending[1] = true;
	server_transfer_stream_step(s);
	check(!s->a2w_batch_owned[1] && !ne->save_batch_open,
		"A2-W reset pause closes its owned window before early exit");
	s->a2w_reset_pending[1] = false;
	s->player.x += 64.0;
	server_transfer_stream_step(s);
	check(s->a2w_batch_owned[1] && ne->save_batch_open,
		"A2-W checkpoint fixture starts a fresh owned window");
	check(server_local_worlds_release_secondary(s),
		"A2-W secondary checkpoint closes its owned window before release");
	check(!s->a2w_batch_owned[1]
			&& server_local_world_admit(s, WORLD_DIM_NETHER) == &s->worlds[1]
			&& !s->a2w_batch_owned[1],
		"A2-W reused secondary slot begins without prior batch ownership");
	server_local_worlds_discard_secondary(s);
	teardown(s);
}

/* A failed publication after an A2W eviction must leave the newest record
 * retained and make the existing retry clock effective on this same path. */
static void case_a2w_batch_failure(void) {
	struct server_local* s = two_world_fixture();
	clin_test_set_consume_observer(aw_observer, NULL);
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id)
			== SERVER_TRANSFER_OK,
		"A2-W fault fixture: host requests Nether");
	check(lr_drive(s, id, ACTOR_SLOT_HOST).final == SERVER_TRANSFER_COMMITTED,
		"A2-W fault fixture: host commits to secondary world");
	struct server_world* ne = &s->worlds[1];
	aw_run_until(s, aw_marker_armed, 1500);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	tick(s);
	aw_run_until(s, aw_host_square_sent, 3000);
	check(aw_host_square_sent(s), "A2-W fault fixture: original ring complete");
	/* Finish every earlier publication before switching the test-only async
	 * policy. The fault below must belong to this A2W pass, not setup. */
	assert(server_world_save_batch_end(&s->worlds[0]));
	assert(server_world_sync_regions(&s->worlds[0]));
	assert(server_world_sync_regions(ne));
	region_archive_io_async_enable(false);
	w_coord_t old_cx = aw_cdiv(s->player.x);
	s->player.x += 64.0;
	/* Every departing column gets a real changed record. The first victim's
	 * identity is captured from the dictionary after the production pass. */
	int64_t dirty_keys[128];
	size_t dirty_n = 0;
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, ne->chunks);
		!dict_server_chunks_end_p(it); dict_server_chunks_next(it)) {
		int64_t key = dict_server_chunks_ref(it)->key;
		w_coord_t cx = S_CHUNK_X(key), cz = S_CHUNK_Z(key);
		if(abs(cx - aw_cdiv(s->player.x)) > server_local_view_radius(s)
		   || abs(cz - aw_cdiv(s->player.z)) > server_local_view_radius(s)) {
			assert(dirty_n < sizeof(dirty_keys) / sizeof(dirty_keys[0]));
			dirty_keys[dirty_n++] = key;
			assert(server_world_set_block(ne, cx * CHUNK_SIZE, 110,
				cz * CHUNK_SIZE, (struct block_data){.type = BLOCK_DIRT}));
			clin_update_drain(); /* each dirty fixture write publishes a CRPC */
		}
	}
	clin_update_drain();
	/* G9: the dirty marks are fixture SETUP, settled explicitly here (the
	 * fault below must hit the A2W batch, not a light deferral). Production
	 * deferral of pinned evictions and its settle are proven in a2w_walk. */
	assert(server_world_light_drain(ne));
	uint32_t stuck = g_evict_stuck;
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE);
	s->save_batch_ticks = SAVE_BATCH_MAX_TICKS - 1;
	server_transfer_stream_step(s);
	check(g_evict_stuck == stuck + 1 && s->chunk_save_retry_delay == 20
			&& server_world_retiring_active(ne) > 0,
		"A2-W failed batch is reported and keeps pending evicted records");
	int64_t victim = 0;
	bool found_victim = false;
	for(size_t k = 0; k < dirty_n; k++)
		if(!dict_server_chunks_get(ne->chunks, dirty_keys[k])) {
			victim = dirty_keys[k];
			found_victim = true;
			break;
		}
	check(found_victim, "A2-W fault fixture: a dirty victim left the resident dict");
	uint32_t evicted = s->a2w_evicted[1], journal_before[24], journal_after[24];
	/* The body can vacate this world later in the same update. Its separate
	 * drain must honor the failed A2-W publication's shared retry clock. */
	s->player.world_slot = 0;
	s->player.dimension = WORLD_DIM_OVERWORLD;
	size_t resident_before = dict_server_chunks_size(ne->chunks);
	region_archive_journal_stats(journal_before);
	server_local_retire_vacant_worlds(s);
	region_archive_journal_stats(journal_after);
	check(dict_server_chunks_size(ne->chunks) == resident_before
			&& journal_after[8] == journal_before[8],
		"A2-W failed batch delay also blocks same-update vacant-world saves");
	s->player.world_slot = 1;
	s->player.dimension = WORLD_DIM_NETHER;
	region_archive_journal_stats(journal_before);
	server_transfer_stream_step(s);
	region_archive_journal_stats(journal_after);
	check(s->a2w_evicted[1] == evicted
			&& journal_after[5] == journal_before[5],
		"A2-W failed batch delay prevents an immediate publication retry");
	/* Existing barrier must recover the retained record before teardown. */
	(void)server_world_sync_regions(ne);
	check(server_world_sync_regions(ne)
			&& server_world_retiring_active(ne) == 0
			&& ne->retiring_dropped == 0,
		"A2-W retained records publish on retry without loss");
	if(found_victim) {
		w_coord_t vx = S_CHUNK_X(victim), vz = S_CHUNK_Z(victim);
		struct server_chunk* reloaded = NULL;
		struct block_data saved;
		check(server_world_load_chunk(ne, vx, vz, &reloaded) && reloaded
				&& server_world_get_block(ne, vx * CHUNK_SIZE, 110,
					vz * CHUNK_SIZE, &saved)
				&& saved.type == BLOCK_DIRT,
			"A2-W failed publication retry restores newest dirty block from disk");
	}
	check(old_cx != aw_cdiv(s->player.x),
		"A2-W fault fixture kept the host outside the old ring");
	check(server_local_worlds_release_secondary(s),
		"A2-W fault fixture releases its secondary world before teardown");
	teardown(s);
	region_archive_io_async_enable(true);
}

/* A2-W: a guest body committed to a slot >= 1 world by the Join or a respawn
 * (not an A2-T COMMIT) waits for its column to be published before its
 * gameplay admission. Modelled here: the guest commits to the Nether, then the
 * world is taken back to "no client context, nothing published, body not yet
 * admitted" -- exactly what the saved-dimension Join leaves. */
static unsigned ag_gate1;
static void ag_observer(const struct client_rpc* call, void* user) {
	aw_observer(call, user);
	lr_bind_seqs(call);
	if(call->type == CRPC_WORLD_RESET && call->payload.world_reset.guest_world
	   && call->payload.world_reset.guest_view == 1)
		ag_gate1++;
}
static void case_a2w_guest_pending(void) {
	check(clin_world_payloads_enabled(), "fixture: world-bound payloads ON");
	struct server_local* s = two_world_fixture();
	clin_test_set_consume_observer(ag_observer, NULL);
	uint32_t id = 0;
	check(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK,
		  "the guest requests the Nether");
	struct lr_trace tr = lr_drive(s, id, ACTOR_SLOT_GUEST);
	struct server_world* ne = world_of(s, s->guest_actor);
	check(tr.final == SERVER_TRANSFER_COMMITTED && ne == &s->worlds[1],
		  "the guest COMMITS into the Nether (slot 1); the host stays in the Overworld");
	/* the Join's state: no client context bound by a COMMIT, nothing sent,
	 * the body not yet admitted to gameplay */
	s->a2w_stream_on[1] = false;
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, ne->chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it))
		dict_server_chunks_ref(it)->value.sent = false;
	s->guest_actor->finished_loading = false;
	unsigned gate1 = ag_gate1;
	const uint32_t seq_before = server_local_guest_bind_seq();
	run_ticks(s, 60);
	check(ag_gate1 == gate1 + 1, "A2-W: the guest's world gets its gate-1 bind once");
	/* O10 / R05 P2: the stream-path gate 1 (server_transfer.c) is a new
	 * binding: stamped with the producer's bump, which is now current. */
	check(lr_g1_seq == seq_before + 1u && server_local_guest_bind_seq() == lr_g1_seq,
		  "A2-W: the stream-path gate-1 reset carries a freshly bumped binding");
	check(server_world_is_chunk_sent(ne, aw_cdiv(s->guest_actor->x), aw_cdiv(s->guest_actor->z)),
		  "A2-W: the pending guest body's own column is published in its world (its "
		  "gameplay admission can complete)");
	teardown(s);
}

static void case_same_world_back_to_back(void) {
	check(clin_world_payloads_enabled(), "fixture: world-bound payloads ON");
	struct server_local* s = two_world_fixture();
	clin_test_set_consume_observer(lr_observer, NULL);
	uint32_t hid = 0, gid = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &hid)
			  == SERVER_TRANSFER_OK, "host -> Nether requested");
	run_ticks(s, 2);
	check(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &gid)
			  == SERVER_TRANSFER_OK, "guest -> Nether requested two ticks later");
	struct lr_trace th = lr_drive(s, hid, ACTOR_SLOT_HOST);
	check(th.final == SERVER_TRANSFER_COMMITTED
			  && world_of(s, &s->player)->dimension == WORLD_DIM_NETHER,
		  "the host COMMITs into the Nether");
	struct lr_trace tg = lr_drive(s, gid, ACTOR_SLOT_GUEST);
	const struct client_world_context* shared
		= client_ctx_registry_current(clin_world_contexts(), 1);
	check(tg.final == SERVER_TRANSFER_COMMITTED && !tg.saw_landing
			  && world_of(s, s->guest_actor) == world_of(s, &s->player)
			  && shared && shared->server_generation == s->worlds[1].generation
			  && (shared->bound_views & 3u) == 3u
			  && lr_core_installed(1, s->guest_actor->x, s->guest_actor->z),
		  "both COMMITTED: guest joins the host's installed, generation-matched "
		  "Nether core without a second landing reservation");
	teardown(s);
}

/* A2-T budget: ensure_populated can insert the target and quadrant neighbours.
 * A guard that checks only its target before the call cannot enforce 176. */
static void case_cap_population(void) {
	/* Start before the ordinary load stream: that legacy single-player path
	 * deliberately holds a larger near ring than the split budget. */
	struct server_local* s = split_fixture();
	string_set_str(s->level_name, split_path);
	s->player.finished_loading = true;
	s->split_enabled = true;
	s->split_view_radius = 4;
	struct server_world* dst = server_local_world_admit(s, WORLD_DIM_NETHER);
	check(dst && dst == &s->worlds[1], "cap: destination world admitted");
	for(w_coord_t x = 1000;
		server_transfer_resident_columns(s) < SERVER_TRANSFER_RESIDENT_COLUMN_CAP - 1;
		x++) {
		struct server_chunk* sc = NULL;
		if(!server_world_load_chunk(&s->worlds[0], x, 1000, &sc) || !sc) {
			check(false, "cap: source preload succeeds");
			break;
		}
	}
	check(server_transfer_resident_columns(s) == SERVER_TRANSFER_RESIDENT_COLUMN_CAP - 1,
		  "cap: preloaded to 175 resident columns");
	server_transfer_budget_tick(s);
	check(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED,
		  "cap: admission starts in the enforced epoch");
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id)
			  == SERVER_TRANSFER_OK,
		  "cap: transfer opened against the saturated source");
	struct server_chunk* target = NULL;
	struct java_random rng_before = dst->hell_populate_rng;
	uint32_t waits_before = s->transfer_column_cap_waits;
	server_world_begin_update(dst);
	check(!server_world_ensure_populated(dst, s, 200, 200, &target),
		  "cap: a nine-column populate refuses at 175 live columns");
	check(s->transfer_column_cap_waits > waits_before
			  && s->transfer_column_reservation_n == 0
			  && dict_server_chunks_size(dst->chunks) == 0
			  && memcmp(&rng_before, &dst->hell_populate_rng, sizeof(rng_before)) == 0,
		  "cap: refusal reports WAIT and preserves reservations, world, and Nether RNG");
	for(w_coord_t x = 1000; x < 1009; x++)
		check(server_world_save_chunk(&s->worlds[0], true, x, 1000),
			  "cap: a source backing is released for retry");
	bool ready = false;
	for(unsigned i = 0; i < 512 && !ready; i++) {
		server_world_begin_update(dst);
		ready = server_world_ensure_populated(dst, s, 200, 200, &target);
	}
	check(ready && target && target->populated
			  && s->transfer_column_reservation_n == 0,
		  "cap: the same target completes after nine source columns retire");
	check(server_transfer_resident_columns(s) <= SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
		  "cap: target plus populate neighbours never exceed 176");
	teardown(s);
}

/* The refused call above proves the immediate state is unchanged. Compare a
 * later successful retry with the same seed and source-load history when no
 * budget was armed, including every byte of the target and the Nether
 * decoration RNG. This catches a hidden preflight RNG draw or partial write
 * that a simple "eventually populated" check would miss. */
struct population_column_image {
	bool resident, populated;
	uint8_t ids[SERVER_CHUNK_IDS_BYTES];
	uint8_t metadata[SERVER_CHUNK_NIBBLE_BYTES];
	uint8_t sky[SERVER_CHUNK_NIBBLE_BYTES];
	uint8_t torch[SERVER_CHUNK_NIBBLE_BYTES];
	uint8_t heightmap[SERVER_CHUNK_HEIGHTMAP_BYTES];
};
struct population_image {
	struct population_column_image footprint[9];
	struct java_random rng;
};

static void population_image_copy(struct population_image* out,
	const struct server_world* w, w_coord_t cx, w_coord_t cz) {
	memset(out, 0, sizeof(*out));
	for(int dz = -1; dz <= 1; dz++)
		for(int dx = -1; dx <= 1; dx++) {
			struct population_column_image* image
				= &out->footprint[(dz + 1) * 3 + dx + 1];
			const struct server_chunk* sc = dict_server_chunks_get(w->chunks,
				S_CHUNK_ID(cx + dx, cz + dz));
			if(!sc)
				continue;
			image->resident = true;
			image->populated = sc->populated;
			memcpy(image->ids, sc->ids, sizeof(image->ids));
			memcpy(image->metadata, sc->metadata, sizeof(image->metadata));
			memcpy(image->sky, sc->lighting_sky, sizeof(image->sky));
			memcpy(image->torch, sc->lighting_torch, sizeof(image->torch));
			memcpy(image->heightmap, sc->heightmap, sizeof(image->heightmap));
		}
	/* Keep the zeroed snapshot padding deterministic across host ABIs. A
	 * whole-struct assignment may copy indeterminate java_random padding. */
	out->rng.seed = w->hell_populate_rng.seed;
	out->rng.have_next_gaussian = w->hell_populate_rng.have_next_gaussian;
	out->rng.next_gaussian_cached = w->hell_populate_rng.next_gaussian_cached;
}

static struct server_local* cap_population_fixture(void) {
	struct server_local* s = split_fixture();
	string_set_str(s->level_name, split_path);
	s->player.finished_loading = true;
	s->split_enabled = true;
	s->split_view_radius = 4;
	check(server_local_world_admit(s, WORLD_DIM_NETHER) == &s->worlds[1],
		  "retry parity: Nether admitted");
	for(w_coord_t x = 1000;
		server_transfer_resident_columns(s) < SERVER_TRANSFER_RESIDENT_COLUMN_CAP - 1;
		x++) {
		struct server_chunk* sc = NULL;
		if(!server_world_load_chunk(&s->worlds[0], x, 1000, &sc) || !sc) {
			check(false, "retry parity: identical source preload succeeds");
			break;
		}
	}
	return s;
}

static void case_cap_population_retry_equiv(void) {
	struct population_image* retried_image = malloc(sizeof(*retried_image));
	check(retried_image != NULL, "retry parity: image allocated");
	if(!retried_image)
		return;
	/* split_fixture resets the client mode between sessions, so run the
	 * payload-on capped path first, then the otherwise identical uncapped
	 * reference. Both worlds use the same seed and source-load history. */
	struct server_local* retried = cap_population_fixture();
	struct server_world* dst = &retried->worlds[1];
	server_transfer_budget_tick(retried);
	check(retried->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED,
		  "retry parity: capped fixture enforces budget before first attempt");
	struct java_random before = dst->hell_populate_rng;
	server_world_begin_update(dst);
	struct server_chunk* target = NULL;
	check(!server_world_ensure_populated(dst, retried, 200, 200, &target)
			  && memcmp(&before, &dst->hell_populate_rng, sizeof(before)) == 0,
		  "retry parity: rejected first attempt leaves destination RNG unchanged");
	for(w_coord_t x = 1000; x < 1009; x++)
		check(server_world_save_chunk(&retried->worlds[0], true, x, 1000),
			  "retry parity: free source backing for the same target");
	bool ready = false;
	for(unsigned i = 0; i < 512 && !ready; i++) {
		server_world_begin_update(dst);
		ready = server_world_ensure_populated(dst, retried, 200, 200, &target);
	}
	check(ready && target && target->populated,
		  "retry parity: deferred target completes under the enforced cap");
	if(ready && target)
		population_image_copy(retried_image, dst, 200, 200);
	check(server_transfer_resident_columns(retried)
			  <= SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
		  "retry parity: successful retry remains inside 176 live backings");
	check(server_local_world_retire(retried, dst),
		  "retry parity: capped secondary world retires before reference fixture");
	teardown(retried);

	struct server_local* reference = cap_population_fixture();
	struct server_world* ref_dst = &reference->worlds[1];
	/* The reference still has the same world, source columns, seed and RNG,
	 * but has not opted into bounded two-anchor streaming. */
	reference->split_enabled = false;
	server_transfer_budget_tick(reference);
	check(reference->transfer_budget_phase == SERVER_TRANSFER_BUDGET_OFF,
		  "retry parity: unchanged reference path has no cap gate");
	target = NULL;
	bool reference_ready = false;
	for(unsigned i = 0; i < 512 && !reference_ready; i++) {
		server_world_begin_update(ref_dst);
		reference_ready = server_world_ensure_populated(ref_dst, reference,
			200, 200, &target);
	}
	check(reference_ready && target && target->populated,
		  "retry parity: unchanged path completes without a budget refusal");
	if(ready && reference_ready && target) {
		struct population_image* expected = malloc(sizeof(*expected));
		check(expected != NULL, "retry parity: reference image allocated");
		if(expected) {
			population_image_copy(expected, ref_dst, 200, 200);
			check(memcmp(expected, retried_image, sizeof(*expected)) == 0,
				  "retry parity: nine-column arrays, populate flags and RNG are byte-identical");
			free(expected);
		}
	}
	check(server_local_world_retire(reference, ref_dst),
		  "retry parity: reference secondary world retires");
	teardown(reference);
	free(retried_image);
}

/* A guest can leave after rollback accepted a source CHUNK but before its
 * sign sidecar fit in the 16-token client inbox. The host still needs that
 * source column; the same cursor must finish it without a second CHUNK.
 * When the last actor has gone, the orphan claim must settle promptly. */
static void case_departed_owner_restore(bool host_remains) {
	struct server_local* s = two_world_fixture();
	uint32_t id = 0;
	server_transfer_test_fault(SERVER_TRANSFER_FAULT_TELEPORTER);
	check(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &id)
			  == SERVER_TRANSFER_OK,
		  "departed restore: real guest request accepted");
	s->paused = false;
	for(unsigned i = 0; i < MAX_STEPS
		&& s->transfer[ACTOR_SLOT_GUEST].state != SERVER_TRANSFER_RESTORING; i++) {
		tick(s);
		client_acks_marker(s);
	}
	struct server_transfer_txn* t = &s->transfer[ACTOR_SLOT_GUEST];
	check(t->open && t->state == SERVER_TRANSFER_RESTORING && t->hold_sent,
		  "departed restore: production fault reaches held rollback restoration");
	if(!(t->open && t->state == SERVER_TRANSFER_RESTORING && t->hold_sent)) {
		server_transfer_test_fault(SERVER_TRANSFER_FAULT_NONE);
		teardown(s);
		return;
	}
	struct server_world* src = &s->worlds[t->src_slot];
	w_coord_t cx = t->source_cx - t->restore_radius;
	w_coord_t cz = t->source_cz - t->restore_radius;
	struct server_chunk* sc = dict_server_chunks_get(src->chunks, S_CHUNK_ID(cx, cz));
	check(sc != NULL, "departed restore: host outer-ring source column resident");
	if(!sc) {
		teardown(s);
		return;
	}
	/* Leave only the first source-ring key unpublished. This is the exact
	 * state after one CHUNK has entered the FIFO and its sidecar has not. */
	for(int dz = -t->restore_radius; dz <= t->restore_radius; dz++)
		for(int dx = -t->restore_radius; dx <= t->restore_radius; dx++) {
			struct server_chunk* resident = dict_server_chunks_get(src->chunks,
				S_CHUNK_ID(t->source_cx + dx, t->source_cz + dz));
			if(resident)
				resident->sent = true;
		}
	sc->sent = false;
	sc->a2w_pub_stage = 0;
	sc->a2w_sign_cursor = 0;
	sc->a2w_entity_after_id = 0;
	struct te_store signs = server_world_tile_store(src);
	int row = te_store_claim_free(&signs, TE_KIND_SIGN,
		cx * CHUNK_SIZE, 70, cz * CHUNK_SIZE);
	check(row >= 0, "departed restore: source sign sidecar staged");
	if(row >= 0) {
		memset(s->sign_texts[row], ' ', SIGN_SIZE);
		s->sign_texts[row][0] = 'R';
	}
	struct client_rpc filler = {.type = CRPC_TRANSFER_HOLD,
		.payload.transfer_hold = {.actor_slot = 2}};
	bool filled = true;
	for(unsigned i = 0; i < 15; i++)
		filled &= clin_try_rpc_send(&filler);
	check(filled, "departed restore: one token remains for a real source CHUNK");
	server_transfer_step(s);
	sc = dict_server_chunks_get(src->chunks, S_CHUNK_ID(cx, cz));
	check(sc && sc->a2w_pub_stage == 1 && !sc->sent,
		  "departed restore: CHUNK accepted while sign waits for a token");
	clin_update_drain();
	guest_release(s); /* authentic owner generation and body leave */
	if(!host_remains)
		s->player.has_pos = false; /* final host leave: no client world route */
	unsigned saturated = 0;
	while(saturated < 1024 && clin_try_rpc_send(&filler))
		saturated++;
	check(saturated > 0 && saturated < 1024,
		  "departed restore: client inbox saturated after guest LEFT");
	server_transfer_step(s);
	sc = dict_server_chunks_get(src->chunks, S_CHUNK_ID(cx, cz));
	if(host_remains)
		check(t->open && t->state == SERVER_TRANSFER_RESTORING && sc
				  && sc->a2w_pub_stage == 1 && !sc->sent,
			  "departed restore: live host keeps the partial source claim for retry");
	else
		check(sc && sc->a2w_pub_stage == 0 && !sc->sent,
			  "departed restore: no-route source claim is cancelled without inbox wait");
	clin_update_drain();
	for(unsigned i = 0; i < 100 && server_transfer_active(s); i++)
		server_transfer_step(s);
	check(server_transfer_state_of(s, id) == SERVER_TRANSFER_ROLLED_BACK
			  && !server_transfer_active(s),
		  "departed restore: transaction settles after owner departure");
	sc = dict_server_chunks_get(src->chunks, S_CHUNK_ID(cx, cz));
	if(host_remains)
		check(sc && sc->sent && sc->a2w_pub_stage == 3,
			  "departed restore: host source sidecar finishes through original cursor");
	else
		check(sc && sc->a2w_pub_stage == 0,
			  "departed restore: no later near pass can inherit a partial cursor");
	server_transfer_test_fault(SERVER_TRANSFER_FAULT_NONE);
	teardown(s);
}

/* The Teleporter is a cooperative Beta scan. A physical-column refusal is
 * scheduling WAIT, so neither its cursor nor its private Random may advance. */
static void case_cap_teleporter_wait(void) {
	struct server_local* s = split_fixture();
	string_set_str(s->level_name, split_path);
	s->player.finished_loading = true;
	s->split_enabled = true;
	s->split_view_radius = 4;
	struct server_world* dst = server_local_world_admit(s, WORLD_DIM_NETHER);
	check(dst && dst == &s->worlds[1], "teleporter cap: destination admitted");
	for(w_coord_t x = 1000;
		server_transfer_resident_columns(s) < SERVER_TRANSFER_RESIDENT_COLUMN_CAP;
		x++) {
		struct server_chunk* sc = NULL;
		if(!server_world_load_chunk(&s->worlds[0], x, 1000, &sc) || !sc) {
			check(false, "teleporter cap: source preload succeeds");
			break;
		}
	}
	server_transfer_budget_tick(s);
	check(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED,
		  "teleporter cap: cap epoch enforced at 176 live backings");
	struct java_random random;
	jrand_set_seed(&random, 42391);
	struct portal_teleporter_pose pose = {.x = 3200.5, .y = 70.0, .z = 3200.5};
	unsigned original_radius = portal_teleporter_test_default_find_radius();
	portal_teleporter_test_set_find_radius(0);
	struct portal_teleporter_job* job
		= portal_teleporter_begin_w(s, dst, &pose, &random);
	check(job != NULL, "teleporter cap: cooperative job opened");
	uint32_t waits_before = s->transfer_column_cap_waits;
	for(unsigned i = 0; i < 64 && s->transfer_column_cap_waits == waits_before; i++) {
		server_world_begin_update(dst);
		check(portal_teleporter_step(s, job) == PORTAL_TELEPORTER_WORKING,
			  "teleporter cap: search reaches create without failure");
	}
	check(s->transfer_column_cap_waits > waits_before,
		  "teleporter cap: creation reaches physical budget WAIT");
	struct portal_teleporter_test_snapshot held, again;
	portal_teleporter_test_snapshot_get(job, &held);
	struct java_random world_rng = dst->hell_populate_rng;
	for(unsigned i = 0; i < 3; i++) {
		server_world_begin_update(dst);
		check(portal_teleporter_step(s, job) == PORTAL_TELEPORTER_WORKING,
			  "teleporter cap: saturated step remains WORKING");
		portal_teleporter_test_snapshot_get(job, &again);
		check(again.stage == held.stage && again.cursor_x == held.cursor_x
				  && again.cursor_z == held.cursor_z && again.retries == held.retries
				  && memcmp(&again.rng, &held.rng, sizeof(held.rng)) == 0
				  && memcmp(&world_rng, &dst->hell_populate_rng,
					  sizeof(world_rng)) == 0,
			  "teleporter cap: WAIT preserves job cursor, retries and both RNGs");
	}
	for(w_coord_t x = 1000; x < 1009; x++)
		check(server_world_save_chunk(&s->worlds[0], true, x, 1000),
			  "teleporter cap: retire nine unneeded source backings");
	bool advanced = false;
	for(unsigned i = 0; i < 512 && !advanced; i++) {
		server_world_begin_update(dst);
		enum portal_teleporter_result result = portal_teleporter_step(s, job);
		portal_teleporter_test_snapshot_get(job, &again);
		advanced = again.stage != held.stage || again.cursor_x != held.cursor_x
			|| again.cursor_z != held.cursor_z || result == PORTAL_TELEPORTER_DONE;
	}
	check(advanced && server_transfer_resident_columns(s)
		  <= SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
		  "teleporter cap: same create cursor progresses after room frees within 176");
	portal_teleporter_destroy(job);
	portal_teleporter_test_set_find_radius(original_radius);
	teardown(s);
}

static void case_cap_ready_target(void) {
	struct server_local* s = split_fixture();
	string_set_str(s->level_name, split_path);
	s->player.finished_loading = true;
	s->split_enabled = true;
	s->split_view_radius = 4;
	struct server_world* dst = server_local_world_admit(s, WORLD_DIM_NETHER);
	check(dst != NULL, "ready target: Nether admitted");
	for(int dz = -1; dz <= 0; dz++)
		for(int dx = -1; dx <= 0; dx++) {
			struct server_chunk* sc = NULL;
			check(server_world_load_chunk(dst, 200 + dx, 200 + dz, &sc) && sc,
				  "ready target: covering quadrant resident");
			if(sc)
				sc->populated = true; /* saved, fully decorated quadrant */
		}
	for(w_coord_t x = 1000;
		server_transfer_resident_columns(s) < SERVER_TRANSFER_RESIDENT_COLUMN_CAP;
		x++) {
		struct server_chunk* sc = NULL;
		check(server_world_load_chunk(&s->worlds[0], x, 1000, &sc) && sc,
			  "ready target: filler reaches the physical cap");
	}
	check(!server_world_chunk_resident(dst, 201, 201),
		  "ready target: +1 support column is intentionally absent");
	struct java_random rng_before = dst->hell_populate_rng;
	uint32_t waits_before = s->transfer_column_cap_waits;
	struct server_chunk* target = NULL;
	server_world_begin_update(dst);
	check(server_world_ensure_populated(dst, s, 200, 200, &target)
			  && target && target->populated,
		  "ready target: completed resident target streams at 176 without nine-key reservation");
	check(s->transfer_column_cap_waits == waits_before
			  && s->transfer_column_reservation_n == 0
			  && memcmp(&rng_before, &dst->hell_populate_rng, sizeof(rng_before)) == 0,
		  "ready target: no cap WAIT, extra allocation, or RNG draw");
	teardown(s);
}

static void case_cap_radius_policy(bool payloads_on) {
	struct server_local* s = split_fixture();
	s->split_enabled = true;
	s->anchor_sep = 256;
	s->split_view_radius = 0;
	server_transfer_budget_tick(s);
	check(server_local_view_radius(s) == (payloads_on ? 4 : MAX_VIEW_DISTANCE),
		  "radius: zero is bounded four only with world payloads enabled");
	check(s->transfer_budget_phase == (payloads_on
		? SERVER_TRANSFER_BUDGET_ENFORCED : SERVER_TRANSFER_BUDGET_OFF),
		  "radius: physical cap epoch is opt-in");
	s->split_view_radius = 5;
	check(server_local_view_radius(s) == (payloads_on ? 4 : 5),
		  "radius: configured five stays five in legacy mode");
	s->split_view_radius = 3;
	check(server_local_view_radius(s) == 3,
		  "radius: configured three remains three in both modes");
	teardown(s);
}

static void case_cap_retained_backing(void) {
	struct server_local* s = split_fixture();
	string_set_str(s->level_name, split_path);
	s->player.finished_loading = true;
	s->split_enabled = true;
	s->split_view_radius = 4;
	w_coord_t held_x = 1000;
	struct column_backing* lease = NULL;
	for(w_coord_t x = held_x;
		server_transfer_resident_columns(s) < SERVER_TRANSFER_RESIDENT_COLUMN_CAP;
		x++) {
		struct server_chunk* sc = NULL;
		if(!server_world_load_chunk(&s->worlds[0], x, 1000, &sc) || !sc) {
			check(false, "retained: preload reaches the cap");
			break;
		}
		if(x == held_x) {
			lease = sc->backing;
			column_backing_ref(lease); /* client/RPC-owned backing after dict erase */
		}
	}
	check(lease && server_transfer_resident_columns(s)
				== SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
		  "retained: 176 keyed backings are live");
	check(server_world_save_chunk(&s->worlds[0], true, held_x, 1000),
		  "retained: dictionary eviction succeeds while client lease remains");
	uint32_t live = 0, shared = 0;
	column_backing_get_stats(&live, &shared);
	check(server_transfer_resident_columns(s) == SERVER_TRANSFER_RESIDENT_COLUMN_CAP - 1
			  && live == SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
		  "retained: dictionary drops to 175 but live backing remains 176");
	struct server_chunk* next = NULL;
	check(!server_world_load_chunk(&s->worlds[0], 2000, 1000, &next)
			  && next == NULL,
		  "retained: client-held backing blocks replacement despite free dictionary key");
	column_backing_unref(lease);
	lease = NULL;
	column_backing_get_stats(&live, &shared);
	check(live == SERVER_TRANSFER_RESIDENT_COLUMN_CAP - 1,
		  "retained: releasing client lease frees the physical budget slot");
	check(server_world_load_chunk(&s->worlds[0], 2000, 1000, &next) && next,
		  "retained: replacement is admitted only after the backing is freed");
	teardown(s);
}

static void case_cap_activation(void) {
	struct server_local* s = two_world_fixture();
	uint32_t live = 0, shared = 0;
	column_backing_get_stats(&live, &shared);
	check(live > SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
		  "activation: real pre-split stream begins above 176 live backings");
	server_transfer_budget_tick(s);
	struct server_transfer_budget_snapshot snap;
	check(server_transfer_budget_snapshot_get(&snap)
			  && snap.budget_phase == SERVER_TRANSFER_BUDGET_DRAINING
			  && snap.prebudget_peak >= live,
		  "activation: prebudget footprint is reported in DRAINING");
	uint32_t id = 0;
	check(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &id)
			  == SERVER_TRANSFER_OK,
		  "activation: transfer can wait while legacy columns retire");
	s->paused = false;
	bool saw_enforced = false, no_early_dst = true, bounded = true;
	for(unsigned i = 0; i < 2000; i++) {
		tick(s);
		if(!server_transfer_budget_snapshot_get(&snap)) {
			bounded = false;
			break;
		}
		if(snap.budget_phase == SERVER_TRANSFER_BUDGET_DRAINING)
			no_early_dst &= dict_server_chunks_size(s->worlds[1].chunks) == 0;
		if(snap.budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED) {
			saw_enforced = true;
			bounded &= snap.live_backings + snap.reserved
				<= SERVER_TRANSFER_RESIDENT_COLUMN_CAP
				&& snap.enforced_peak <= SERVER_TRANSFER_RESIDENT_COLUMN_CAP;
			break;
		}
	}
	check(no_early_dst,
		  "activation: no destination backing is allocated during DRAINING");
	printf("activation: phase=%u live=%u reserved=%u prepeak=%u enforcedpeak=%u saw=%d bounded=%d\n",
		(unsigned)snap.budget_phase, snap.live_backings, snap.reserved,
		snap.prebudget_peak, snap.enforced_peak, saw_enforced, bounded);
	check(saw_enforced && bounded,
		  "activation: live backings plus reservations drain below 176 before ENFORCED");
	/* An open held transaction is rolled back through its real restoration. */
	server_transfer_test_fault(SERVER_TRANSFER_FAULT_TELEPORTER);
	check(drive(s, id) == SERVER_TRANSFER_ROLLED_BACK,
		  "activation: waiting transfer can settle after enforcement");
	teardown(s);
}

/* harness_init with split.world_payloads ON: the switch flips only before
 * any client world context exists (clin_world_payloads_set_enabled), i.e.
 * before the harness's first WORLD_RESET -- main.c's boot order. */
static void harness_init_world_payloads(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	recipe_init();
	assert(clin_init() && svin_init());
	assert(clin_world_payloads_set_enabled(true));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);
}

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "host_to_nether";
	if(!strcmp(which, "portal_transfer") || !strcmp(which, "cap_lifecycle")
	   || !strcmp(which, "landing_real") || !strcmp(which, "guest_landing_real")
	   || !strcmp(which, "landing_real_coarse")
	   || !strcmp(which, "a2w_walk") || !strcmp(which, "owner_loading_hazards")
	   || !strcmp(which, "owner_loading_concurrent_transfer")
	   || !strcmp(which, "a2w_batch_failure")
	   || !strcmp(which, "a2w_guest_pending")
	   || !strcmp(which, "same_world_back_to_back")
	   || !strcmp(which, "cap_population")
	   || !strcmp(which, "cap_population_retry_equiv")
	   || !strcmp(which, "departed_restore_host")
	   || !strcmp(which, "departed_restore_empty")
	   || !strcmp(which, "cap_request_on0")
	   || !strcmp(which, "cap_request_on3")
	   || !strcmp(which, "cap_request_on5")
	   || !strcmp(which, "cap_retained_backing")
	   || !strcmp(which, "cap_teleporter_wait")
	   || !strcmp(which, "cap_activation") || !strcmp(which, "cap_ready_target")
	   || !strcmp(which, "cap_radius_on"))
		harness_init_world_payloads();
	else
		harness_init();
	if(!strcmp(which, "host_to_nether"))
		case_host_to_nether();
	else if(!strcmp(which, "rollback"))
		case_rollback();
	else if(!strcmp(which, "opposite"))
		case_opposite();
	else if(!strcmp(which, "duplicate"))
		case_duplicate();
	else if(!strcmp(which, "once_only"))
		case_once_only();
	else if(!strcmp(which, "death_respawn"))
		case_death_respawn();
	else if(!strcmp(which, "reload"))
		case_reload();
	else if(!strcmp(which, "retired_freed"))
		case_retired_freed();
	else if(!strcmp(which, "reload_reopen"))
		case_reload_reopen();
	else if(!strcmp(which, "memory_refusal"))
		case_memory_refusal();
	else if(!strcmp(which, "capacity_landing"))
		case_capacity_landing();
	else if(!strcmp(which, "landing_timeout"))
		case_landing_timeout();
	else if(!strcmp(which, "landing_residency"))
		landing_residency(false);
	else if(!strcmp(which, "landing_residency_timeout"))
		landing_residency(true);
	else if(!strcmp(which, "landing_stale"))
		case_landing_stale();
	else if(!strcmp(which, "retire_checkpoint_fail"))
		case_retire_checkpoint_fail();
	else if(!strcmp(which, "retire_persists_entities"))
		case_retire_persists_entities();
	else if(!strcmp(which, "unload_secondary"))
		case_unload_secondary();
	else if(!strcmp(which, "autosave_secondary"))
		case_autosave_secondary();
	else if(!strcmp(which, "commit_save_fail"))
		case_commit_save_fail();
	else if(!strcmp(which, "travel_gate"))
		case_travel_gate();
	else if(!strcmp(which, "guest_landing"))
		case_guest_landing();
	else if(!strcmp(which, "portal_flag_off"))
		case_portal_flag_off();
	else if(!strcmp(which, "portal_transfer"))
		case_portal_transfer();
	else if(!strcmp(which, "cap_release"))
		case_cap_release();
	else if(!strcmp(which, "cap_lifecycle"))
		case_cap_lifecycle();
	else if(!strcmp(which, "slot0_anchor"))
		case_slot0_anchor();
	else if(!strcmp(which, "landing_real"))
		case_landing_real();
	else if(!strcmp(which, "landing_real_coarse")) {
		/* O15 regression: the same real landing, sampled every SECOND tick --
		 * the scheduling CPU load produces. Fails on post-tick sampling. */
		lr_ticks_per_sample = 2;
		case_landing_real();
	}
	else if(!strcmp(which, "guest_landing_real"))
		case_guest_landing_real();
	else if(!strcmp(which, "a2w_walk"))
		case_a2w_walk();
	else if(!strcmp(which, "owner_loading_hazards"))
		case_a2w_owner_hazards();
	else if(!strcmp(which, "owner_loading_concurrent_transfer"))
		case_owner_loading_concurrent_transfer();
	else if(!strcmp(which, "a2w_batch_failure"))
		case_a2w_batch_failure();
	else if(!strcmp(which, "a2w_guest_pending"))
		case_a2w_guest_pending();
	else if(!strcmp(which, "same_world_back_to_back"))
		case_same_world_back_to_back();
	else if(!strcmp(which, "cap_population"))
		case_cap_population();
	else if(!strcmp(which, "cap_population_retry_equiv"))
		case_cap_population_retry_equiv();
	else if(!strcmp(which, "departed_restore_host"))
		case_departed_owner_restore(true);
	else if(!strcmp(which, "departed_restore_empty"))
		case_departed_owner_restore(false);
	else if(!strcmp(which, "cap_request_on0"))
		case_capacity_radius_policy(true, 0);
	else if(!strcmp(which, "cap_request_on3"))
		case_capacity_radius_policy(true, 3);
	else if(!strcmp(which, "cap_request_on5"))
		case_capacity_radius_policy(true, 5);
	else if(!strcmp(which, "cap_request_off0"))
		case_capacity_radius_policy(false, 0);
	else if(!strcmp(which, "cap_request_off5"))
		case_capacity_radius_policy(false, 5);
	else if(!strcmp(which, "cap_teleporter_wait"))
		case_cap_teleporter_wait();
	else if(!strcmp(which, "cap_retained_backing"))
		case_cap_retained_backing();
	else if(!strcmp(which, "cap_activation"))
		case_cap_activation();
	else if(!strcmp(which, "cap_ready_target"))
		case_cap_ready_target();
	else if(!strcmp(which, "cap_radius_on"))
		case_cap_radius_policy(true);
	else if(!strcmp(which, "cap_radius_off"))
		case_cap_radius_policy(false);
	else {
		fprintf(stderr, "unknown case %s\n", which);
		return 2;
	}
	return harness_result(TEST_NAME, which);
}
#endif
