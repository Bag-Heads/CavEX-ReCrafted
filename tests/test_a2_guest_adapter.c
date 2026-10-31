/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-T lane C: the guest's actor-transition adapter (a2t-guest-0927;
 * tmp/jobs/split-dimensions-plan-0924/out/A2T-needs-from-B-C.md "Needed from
 * C"), on the REAL admission harness (host + admitted guest body):
 *
 *   rollback  a guest transfer that faults mid-PREPARE rolls back with the
 *             guest's world, pose and saved record untouched (no marker was
 *             written: lane C stages nothing before the marker);
 *   marker    a guest transfer to the Nether COMMITs through lane C's commit
 *             record (level-last; no durable-marker guarantee, Astra a2t-0927):
 *             the guest record (.Data.CaveWiiGuest) names Dimension -1
 *             and the committed pose, while the host record is untouched;
 *   apply     the APPLY rebinds the guest's window world to the destination
 *             handle and drops the old world's queued pose samples;
 *   ckpt      with the guest in the Nether and the host in the Overworld the
 *             guest record checkpoints its OWN dimension (the host-equality
 *             refusal is retired, C#2);
 *   refused   an outcome for a stale guest generation or the host slot is
 *             REFUSED by the adapter; the current outcome re-applies
 *             idempotently and a ROLLBACK outcome changes nothing.
 *   dwell     (case "dwell") the guest's portal dwell: with world-bound
 *             payloads OFF a portal under the guest changes nothing (no ramp,
 *             no transfer); ON, the host's Beta dwell runs on the guest body --
 *             contact during the cooldown pins it, the latch lands on contact
 *             tick 82 (81 x 0.0125F + the arming tick) -- and opens the guest's
 *             A2-T transaction: the guest ends in the Nether, the host stays.
 *   reopen_*  REAL save / release / reopen through the production load, then a
 *             real Join (Astra a2t-0927: admit into the SAVED dimension):
 *               reopen_nether     guest committed to the Nether, host in the
 *                                 Overworld: the reopened guest joins a LIVE
 *                                 Nether world at its saved coordinates;
 *               reopen_overworld  host committed to the Nether, guest in the
 *                                 Overworld: the save reopens routed to the
 *                                 Nether and the guest joins a LIVE Overworld;
 *               reopen_refusal    reopen_overworld over budget: the Join is
 *                                 refused visibly, level.dat and the record are
 *                                 byte-identical, no world admitted; with the
 *                                 budget restored the same Join admits.
 *             Login never builds a portal and never rewrites the record.
 *   nether_respawn   the guest dies in the Nether: its drops stay in the
 *             Nether world, the respawn commits the fresh body to the
 *             Overworld (Beta recreatePlayerEntity -> dimension 0), windows
 *             bound there (Opus review A1b);
 *   record_restore   a failed level.dat save during the guest's commit record
 *             leaves the IN-MEMORY record byte-identical (review A3);
 *   dwell_host_busy  the guest's latch while the HOST's transaction is open
 *             opens the guest's own (queued) request -- not dropped (A2).
 *   join_cap_*  the Join's CLIENT-capacity round trip (Astra a2t-0927: reserve
 *             client/GPU capacity before membership), against lane B's real
 *             client (client_admission.c): a Nether-saved guest, host in the
 *             Overworld, world-bound payloads ON --
 *               join_cap_ok       RESERVE granted -> the body is created, the
 *                                 reservation COMMITted (the client holds it);
 *               join_cap_refused  the client has no MEM1 -> the Join is
 *                                 REJECTED visibly, nothing changed (no body,
 *                                 record untouched, the admitted world
 *                                 released, no client reservation);
 *               join_cap_timeout  the client never answers -> after
 *                                 SERVER_TRANSFER_CLIENT_WAIT_TICKS the same
 *                                 visible, lossless refusal.
 * (The Join gate's server_transfer_active refusal is a one-line guard in
 * server_local_guest.c's LOCAL_GUEST_JOIN branch; it is not driven here.) */
#include "support/owner_swap_harness.h"

#include "cNBT/nbt.h"
#include "network/server_local_guest.h"
#include "network/server_transfer.h"
#include "network/client_world_payload.h"
#include "network/client_admission.h"
#include "block/blocks.h"

#define TEST_NAME "test_a2_guest_adapter"
#define MAX_STEPS 4000

static enum server_transfer_state drive(struct server_local* s, uint32_t id) {
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

static int guest_record_dim(struct server_local* s, bool* present) {
	nbt_node* d = nbt_find_by_path(s->level.data, ".Data.CaveWiiGuest.Dimension");
	*present = d != NULL;
	return d ? d->payload.tag_int : 0;
}

static void unpaused_tick(struct server_local* s) {
	bool was = s->paused;
	s->paused = false;
	tick(s);
	s->paused = was;
}

/* The guest 3 blocks from the host (same published column) with a portal
 * under its feet; returns the portal cell. */
static void guest_over_portal(struct server_local* s, w_coord_t* fx, w_coord_t* fy,
							  w_coord_t* fz) {
	struct server_player* g = s->guest_actor;
	g->x = s->player.x + 3.0;
	check(WCOORD_CHUNK_OFFSET((int)floor(g->x)) == WCOORD_CHUNK_OFFSET((int)floor(s->player.x)),
		  "setup: the guest stands in the host's column, 3 blocks away");
	*fx = (w_coord_t)floor(g->x);
	*fy = (w_coord_t)floor(g->y - PLAYER_EYE_HEIGHT + 0.001);
	*fz = (w_coord_t)floor(g->z);
}

/* flag OFF (the default): the guest keeps its position-only behaviour */
static void case_dwell_off(void) {
	const struct block_data portal = {.type = BLOCK_PORTAL};
	w_coord_t fx, fy, fz;
	check(!clin_world_payloads_enabled(), "flag OFF (the default)");
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	struct server_player* g = s->guest_actor;
	guest_over_portal(s, &fx, &fy, &fz);
	int cooldown0 = g->time_until_portal;
	(void)server_world_set_block(&s->worlds[0], fx, fy, fz, portal);
	for(int i = 0; i < 120; i++)
		unpaused_tick(s);
	check(g->time_in_portal == 0.0F && !g->in_portal && !server_transfer_active(s)
			  && g->time_until_portal == cooldown0
			  && server_local_world_of(s, g) == &s->worlds[0],
		  "flag OFF: a portal under the guest changes nothing (no ramp, no cooldown "
		  "tick, no transfer)");
	teardown(s);
}

/* flag ON (set before the session, as the switch must be): the Beta dwell ->
 * the guest's A2-T transaction */
static void case_dwell(void) {
	const struct block_data portal = {.type = BLOCK_PORTAL};
	w_coord_t fx, fy, fz;
	check(clin_world_payloads_enabled(),
		  "flag ON for this session (set before it starts)");
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	struct server_player* g = s->guest_actor;
	guest_over_portal(s, &fx, &fy, &fz);
	for(int i = 0; i < 40 && g->time_until_portal > 0; i++)
		unpaused_tick(s);
	check(g->time_until_portal == 0 && g->time_in_portal == 0.0F,
		  "the spawn cooldown runs out off the portal");
	(void)server_world_set_block(&s->worlds[0], fx, fy, fz, portal);
	int ticks = 0, latch_tick = -1, landing_tick = -1;
	bool capacity_seen = false;
	while(ticks < 600) {
		unpaused_tick(s);
		ticks++;
		struct server_transfer_txn* t = &s->transfer[ACTOR_SLOT_GUEST];
		if(latch_tick < 0 && server_transfer_active(s))
			latch_tick = ticks;
		if(t->open && t->state == SERVER_TRANSFER_CAPACITY)
			capacity_seen = true;
		/* With world-bound payloads ON the transaction reserves client
		 * capacity (answered by the real client admission above) and then
		 * waits for the client's LANDING report -- the destination columns
		 * resident in the client's slot. Streaming the destination slot to
		 * the client before COMMIT is lane A/B's open item
		 * (SPLITDIMS-HOSTSWITCH-GAP-0927 blocker 3), so this harness client
		 * never gets there; the test stands in for exactly that one client
		 * reply, through the production entry point, stamped with the
		 * destination slot and generation. */
		if(t->open && t->state == SERVER_TRANSFER_LANDING && landing_tick < 0) {
			landing_tick = ticks;
			capacity_seen = capacity_seen || t->capacity_reserved;
			server_transfer_on_landing_ready(s, t->id, t->dst_slot,
				s->worlds[t->dst_slot].generation);
		}
		struct server_world* gw = server_local_world_of(s, g);
		if(gw && gw->dimension == WORLD_DIM_NETHER && !server_transfer_active(s))
			break;
	}
	printf("dwell: transfer opened on tick %d after the portal appeared; client "
		   "landing awaited from tick %d; guest in the Nether after %d ticks\n",
		   latch_tick, landing_tick, ticks);
	check(latch_tick == 82,
		  "the guest's latch opens its transfer on contact tick 82 (the host's Beta dwell)");
	check(capacity_seen && landing_tick > latch_tick,
		  "payloads ON: the client capacity was reserved, then the landing awaited "
		  "(the stand-in answers only the landing report)");
	struct server_world* gw = server_local_world_of(s, g);
	check(gw && gw->dimension == WORLD_DIM_NETHER && g->dimension == WORLD_DIM_NETHER,
		  "the dwell carried the guest into the Nether (transfer COMMITTED)");
	check(server_local_world_of(s, &s->player) == &s->worlds[0]
			  && s->player.dimension == WORLD_DIM_OVERWORLD,
		  "the host stays in the Overworld");
	check(!server_transfer_active(s), "nothing is left open");
	teardown(s);
}

/* ---- save / release / reopen / rejoin ------------------------------------ */
static uint64_t file_hash(const char* root, const char* name) {
	char path[300];
	snprintf(path, sizeof(path), "%s/%s", root, name);
	FILE* f = fopen(path, "rb");
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

static uint64_t record_hash(struct server_local* s) {
	nbt_node* r = nbt_find_by_path(s->level.data, ".Data.CaveWiiGuest");
	if(!r)
		return 0;
	struct buffer b = nbt_dump_binary(r);
	uint64_t h = UINT64_C(14695981039346656037);
	for(size_t i = 0; i < b.len; i++) {
		h ^= b.data[i];
		h *= UINT64_C(1099511628211);
	}
	free(b.data);
	return h;
}

/* The guest checkpoints (its OWN dimension) and leaves; both worlds' chunks
 * and the level are written; the session releases and the save reopens
 * through the production load (lane A's case_reload_reopen shape). */
static bool save_release_reopen(struct server_local* s) {
	if(s->guest_actor) {
		if(!server_local_checkpoint_guests(s))
			return false;
		guest_release(s);
	}
	for(uint8_t i = 0; i < SERVER_LOCAL_WORLD_SLOTS; i++)
		if(server_local_world_live(&s->worlds[i])
		   && (!server_world_checkpoint_chunks(&s->worlds[i])
			   || !server_world_sync_regions(&s->worlds[i])))
			return false;
	if(!level_archive_write_player(&s->level,
								   (vec3) {s->player.x, s->player.y, s->player.z},
								   (vec2) {s->player.rx, s->player.ry}, NULL,
								   s->player.dimension)
	   || !level_archive_save(&s->level))
		return false;
	(void)server_local_worlds_release_secondary(s);
	server_world_discard(&s->worlds[0]);
	level_archive_discard(&s->level);
	s->player.has_pos = false;
	s->player.world_slot = 0;
	string_t name;
	string_init_set_str(name, split_path);
	bool ok = server_local_load_world_impl(s, name);
	string_clear(name);
	if(ok) {
		/* the reopened world streams to the client's readiness marker, as a
		 * boot does; the harness then acknowledges it (30 s fallback) */
		for(unsigned i = 0; i < 2000 && !s->input_ack_pending; i++)
			step(s);
		if(s->input_ack_pending)
			acknowledge(s);
		s->player.finished_loading = true;
		ok = !s->input_ack_pending;
	}
	return ok;
}

static void join_try(struct server_local* s) {
	struct client_session* client = &gstate.players[1];
	assert(client_session_request_local_join(client));
	/* A2-T: a Join into another world first round-trips the client's
	 * capacity (CRPC_CAPACITY_REQUEST -> SRPC_CAPACITY_REPLY): two more pumps. */
	pump_handshake(s);
	pump_handshake(s);
	pump_handshake(s);
	pump_handshake(s);
	pump_handshake(s);
}

static bool guest_attached(const struct server_local* s) {
	return gstate.players[1].local_phase == CLIENT_GUEST_ATTACHED
		&& s->guest_confirmed && s->guest_actor;
}

/* the guest travels to the Nether (its transfer commits), host stays */
static void guest_to_nether(struct server_local* s) {
	uint32_t id = 0;
	check(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &id)
				  == SERVER_TRANSFER_OK
			  && drive(s, id) == SERVER_TRANSFER_COMMITTED,
		  "setup: the guest travels to the Nether");
}

static void host_to_nether(struct server_local* s) {
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id)
				  == SERVER_TRANSFER_OK
			  && drive(s, id) == SERVER_TRANSFER_COMMITTED,
		  "setup: the host travels to the Nether");
}

static void case_reopen_nether(void) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	guest_to_nether(s);
	double gx = s->guest_actor->x, gy = s->guest_actor->y, gz = s->guest_actor->z;
	check(save_release_reopen(s), "save, release and reopen through the production load");
	check(s->worlds[0].dimension == WORLD_DIM_OVERWORLD
			  && server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY,
		  "the save reopens routed to the host's Overworld; no second world yet");
	uint64_t rec = record_hash(s);
	join_try(s);
	check(guest_attached(s), "the Nether-saved guest JOINS (host in the Overworld)");
	if(guest_attached(s)) {
		struct server_player* g = s->guest_actor;
		struct server_world* w = server_local_world_of(s, g);
		check(w && w == &s->worlds[1] && w->dimension == WORLD_DIM_NETHER
				  && server_local_world_live(w) && g->dimension == WORLD_DIM_NETHER,
			  "  ...into a LIVE Nether world (slot 1): its saved dimension");
		/* the record stores Pos through the float vec3 level_archive API */
		check((float)g->x == (float)gx && (float)g->y == (float)gy
				  && (float)g->z == (float)gz,
			  "  ...at its saved coordinates (no portal, no relocation)");
		check(record_hash(s) == rec, "  ...the Join never rewrote the record");
	}
	check(server_local_world_of(s, &s->player) == &s->worlds[0]
			  && s->player.dimension == WORLD_DIM_OVERWORLD,
		  "the host stays in the Overworld");
	teardown(s);
}

static void case_reopen_overworld(bool refuse_first) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	double gx = s->guest_actor->x, gy = s->guest_actor->y, gz = s->guest_actor->z;
	host_to_nether(s);
	check(save_release_reopen(s), "save, release and reopen through the production load");
	check(s->worlds[0].dimension == WORLD_DIM_NETHER
			  && server_local_world_of(s, &s->player) == &s->worlds[0]
			  && s->player.dimension == WORLD_DIM_NETHER,
		  "the save reopens routed to the host's Nether (Player.Dimension -1)");
	uint64_t rec = record_hash(s);
	if(refuse_first) {
		uint64_t lvl = file_hash(split_path, "level.dat");
		server_transfer_test_set_carve_free(512u * 1024u);
		join_try(s);
		check(!guest_attached(s) && !s->guest_actor && !s->guest_owned,
			  "over budget: the Overworld-saved guest is REFUSED");
		clin_update_drain();
		check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY,
			  "  ...no world was admitted (slot 1 EMPTY)");
		check(file_hash(split_path, "level.dat") == lvl && record_hash(s) == rec,
			  "  ...level.dat and the guest record are byte-identical");
		check(server_local_world_of(s, &s->player) == &s->worlds[0]
				  && s->player.dimension == WORLD_DIM_NETHER,
			  "  ...the host is untouched");
		server_transfer_test_set_carve_free(SIZE_MAX);
		memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
		gstate.players[1].visible_window = -1;
	}
	join_try(s);
	check(guest_attached(s), "the Overworld-saved guest JOINS while the host is in the Nether");
	if(guest_attached(s)) {
		struct server_player* g = s->guest_actor;
		struct server_world* w = server_local_world_of(s, g);
		check(w && w != &s->worlds[0] && w->dimension == WORLD_DIM_OVERWORLD
				  && server_local_world_live(w) && g->dimension == WORLD_DIM_OVERWORLD,
			  "  ...into a LIVE Overworld world beside the routed Nether");
		/* the record stores Pos through the float vec3 level_archive API */
		check((float)g->x == (float)gx && (float)g->y == (float)gy
				  && (float)g->z == (float)gz,
			  "  ...at its saved coordinates (no portal, no relocation)");
		check(record_hash(s) == rec, "  ...the Join never rewrote the record");
	}
	teardown(s);
}

/* ---- Opus review fixes ----------------------------------------------------- */
static void process(struct server_local* s) {
	server_local_test_process_messages(s);
	clin_update_drain();
}

static size_t items_in(struct server_local* s, const struct server_world* w) {
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && server_local_entity_world(s, e) == w)
			n++;
	}
	return n;
}

static void case_nether_respawn(void) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	guest_to_nether(s);
	struct server_player* g = s->guest_actor;
	struct server_world* nether = server_local_world_of(s, g);
	check(nether && nether->dimension == WORLD_DIM_NETHER, "setup: the guest is in the Nether");
	g->inventory.items[INVENTORY_SLOT_MAIN] = (struct item_data) {.id = ITEM_DIAMOND, .count = 4};
	size_t nether_items = items_in(s, nether);
	bool hit = false;
	(void)server_local_damage_actor_result(s, g, MAX_PLAYER_HEALTH, 0.0f, 0.0f, &hit);
	check(hit && g->health == 0, "the guest dies in the Nether");
	struct server_rpc_source src = {.slot = 1, .generation = s->guest_generation};
	struct server_rpc rq = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = src.generation}};
	check(svin_rpc_send_from(src, &rq), "respawn request queued");
	process(s);
	check(s->guest_respawn_phase == GUEST_RESPAWN_LOADING,
		  "the Nether death respawn is ACCEPTED (no longer soft-locked)");
	check(items_in(s, nether) > nether_items,
		  "the death drops stay in the Nether world (Beta dropAllItems at the death)");
	struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = s->guest_ticket,
			.action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
			.generation = src.generation,
			.respawn_epoch = s->guest_respawn_epoch}};
	check(svin_rpc_send_from(src, &ready), "respawn ready queued");
	process(s);
	for(int i = 0; i < 40 && s->guest_respawn_phase != GUEST_RESPAWN_IDLE; i++)
		tick(s);
	struct server_world* w = server_local_world_of(s, g);
	check(s->guest_respawn_phase == GUEST_RESPAWN_IDLE && g->health == MAX_PLAYER_HEALTH
			  && g->finished_loading,
		  "the fresh guest body is live");
	check(w == &s->worlds[0] && w->dimension == WORLD_DIM_OVERWORLD
			  && g->dimension == WORLD_DIM_OVERWORLD,
		  "it respawned in the Overworld (Beta: a Nether death respawns in dimension 0)");
	check(g->window_world.generation == server_world_handle_of(w).generation
			  && g->window_world.world_index == w->world_index,
		  "its windows bind to the Overworld");
	check(server_local_world_of(s, &s->player) == &s->worlds[0], "the host is untouched");
	teardown(s);
}

static void case_record_restore(void) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	check(server_local_checkpoint_guests(s), "setup: the guest record exists");
	uint64_t before = record_hash(s);
	string_t saved;
	string_init_set(saved, s->level.file_name);
	string_cat_str(s->level.file_name, "/missing"); /* level.dat cannot be written */
	check(!server_local_guest_commit_record(s, WORLD_DIM_NETHER, 1.0, 70.0, 1.0, 0, 0),
		  "the commit record reports the failed level.dat save");
	string_set(s->level.file_name, saved);
	string_clear(saved);
	nbt_node* d = nbt_find_by_path(s->level.data, ".Data.CaveWiiGuest.Dimension");
	check(record_hash(s) == before && d && d->payload.tag_int == 0,
		  "the in-memory record is restored byte-identical (still Dimension 0): no "
		  "later save can publish the Nether");
	teardown(s);
}

static void case_dwell_host_busy(void) {
	const struct block_data portal = {.type = BLOCK_PORTAL};
	w_coord_t fx, fy, fz;
	check(clin_world_payloads_enabled(), "flag ON for this session");
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	struct server_player* g = s->guest_actor;
	guest_over_portal(s, &fx, &fy, &fz);
	/* contact DURING the cooldown pins it at 10 (EntityPlayer.setInPortal) */
	(void)server_world_set_block(&s->worlds[0], fx, fy, fz, portal);
	for(int i = 0; i < 5; i++)
		unpaused_tick(s);
	check(g->time_until_portal == 10 && g->time_in_portal == 0.0F,
		  "contact during the spawn cooldown pins it at 10, no ramp");
	(void)server_world_set_block(&s->worlds[0], fx, fy, fz,
		(struct block_data) {.type = BLOCK_AIR});
	for(int i = 0; i < 40 && g->time_until_portal > 0; i++)
		unpaused_tick(s);
	/* the HOST opens its transaction first; it waits for its client landing */
	uint32_t hid = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &hid) == SERVER_TRANSFER_OK,
		  "the host's request is open");
	(void)server_world_set_block(&s->worlds[0], fx, fy, fz, portal);
	int latch = -1;
	for(int t = 1; t <= 120 && latch < 0; t++) {
		unpaused_tick(s);
		if(s->transfer[ACTOR_SLOT_GUEST].open)
			latch = t;
	}
	check(latch == 82 && s->transfer[ACTOR_SLOT_HOST].open,
		  "while the host's transaction is open the guest's latch opens its OWN request "
		  "on contact tick 82 (queued, not dropped)");
	uint32_t gid = s->transfer[ACTOR_SLOT_GUEST].id;
	/* let both finish: answer each client landing as it comes up */
	for(int t = 0; t < 1200 && server_transfer_active(s); t++) {
		for(int a = 0; a < 2; a++) {
			struct server_transfer_txn* x = &s->transfer[a];
			if(x->open && x->state == SERVER_TRANSFER_LANDING)
				server_transfer_on_landing_ready(s, x->id, x->dst_slot,
					s->worlds[x->dst_slot].generation);
		}
		unpaused_tick(s);
		client_acks_marker(s); /* the host's A2-T marker, as the load screen */
	}
	/* The host's commits first (serialized). The guest's then reaches its
	 * own client round trip; whether the client grants a second reservation
	 * of the same slot is lane B's admission contract (this harness client
	 * answers "busy"), so the guest's outcome is only required to be
	 * terminal and visible -- the point here is that it was not dropped. */
	enum server_transfer_state gst = server_transfer_state_of(s, gid);
	check(server_transfer_state_of(s, hid) == SERVER_TRANSFER_COMMITTED
			  && (gst == SERVER_TRANSFER_COMMITTED || gst == SERVER_TRANSFER_ROLLED_BACK)
			  && !server_transfer_active(s),
		  "the host's commits first; the guest's request then runs to a terminal state");
	teardown(s);
}

/* ---- the Join's client-capacity round trip ------------------------------- */
static void tiny_budget(struct clin_capacity_budget* out) {
	out->mem1_free = 1024; /* far below CLIN_CAP_MEM1_MARGIN */
	out->mem2_free = UINT64_MAX / 4;
}
/* The client's answer never reaches the server (a refused send): lane B's
 * client then returns the reservation it made itself (client_admission.c:
 * "the server never learns of this ticket: do not strand the bytes"). */
static bool drop_reply(uint32_t request_id, enum clin_capacity_result result,
					   clin_capacity_ticket ticket) {
	(void)request_id; (void)result; (void)ticket;
	return false;
}

/* The guest checkpointed with its record marked Dimension -1, then gone. */
static struct server_local* nether_saved_guest(void) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	check(server_local_checkpoint_guests(s), "setup: the guest record exists");
	guest_release(s);
	nbt_node* d = nbt_find_by_path(s->level.data, ".Data.CaveWiiGuest.Dimension");
	assert(d && d->type == TAG_INT);
	d->payload.tag_int = -1;
	return s;
}

static void join_pump(struct server_local* s, int pumps) {
	struct client_session* client = &gstate.players[1];
	assert(client_session_request_local_join(client));
	for(int i = 0; i < pumps; i++)
		pump_handshake(s);
}

static void case_join_cap(int mode) {
	check(clin_world_payloads_enabled(), "world-bound payloads ON for this session");
	struct server_local* s = nether_saved_guest();
	/* Both out-of-range configured values stream the effective radius four.
	 * The client's held ticket records the production Join CAP request. */
	if(mode == 0) s->split_view_radius = 5;
	if(mode == 1) s->split_view_radius = 0;
	uint64_t rec = record_hash(s);
	if(mode == 1)
		clin_capacity_set_budget_provider(tiny_budget);
	if(mode == 2)
		clin_capacity_set_reply_sender(drop_reply);
	counters_reset();
	join_pump(s, 6);
	uint64_t mem1 = 0, mem2 = 0;
	unsigned contexts = 0;
	if(mode == 0) {
		check(guest_attached(s), "join_cap_ok: the client granted capacity; the guest JOINS");
		struct server_world* w = s->guest_actor ? server_local_world_of(s, s->guest_actor) : NULL;
		check(w && w == &s->worlds[1] && w->dimension == WORLD_DIM_NETHER,
			  "join_cap_ok: into the LIVE Nether world (its saved dimension)");
		clin_capacity_held(&mem1, &mem2, &contexts);
		uint32_t bound = 0;
		check(world_local_shared_capacity_mem1_need(81, &bound)
				  && contexts == 1 && mem1 == bound
				  && mem2 == 81u * SERVER_TRANSFER_MEM2_PER_COLUMN,
			  "join_cap_ok: configured radius 5 sends and holds the effective 81-column request");
		check(!s->guest_join_cap.pending, "join_cap_ok: nothing left pending");
		teardown(s);
		return;
	}
	if(mode == 2) {
		check(!guest_attached(s) && !s->guest_actor && s->guest_join_cap.pending,
			  "join_cap_timeout: with no answer the Join WAITS (no body yet)");
		int waited = 0;
		for(; waited <= SERVER_TRANSFER_CLIENT_WAIT_TICKS + 2 && s->guest_join_cap.pending; waited++)
			unpaused_tick(s);
		check(waited > SERVER_TRANSFER_CLIENT_WAIT_TICKS - 2,
			  "join_cap_timeout: the Join waited the full client window");
		for(int i = 0; i < 3; i++)
			pump_handshake(s);
	}
	clin_update_drain();
	const char* tag = mode == 1 ? "join_cap_refused" : "join_cap_timeout";
	char msg[160];
	snprintf(msg, sizeof(msg), "%s: the Join is REJECTED (no body, the client sees FAILED)", tag);
	check(!s->guest_actor && !s->guest_owned && !s->guest_join_cap.pending
			  && gstate.players[1].local_phase == CLIENT_GUEST_FAILED, msg);
	snprintf(msg, sizeof(msg), "%s: VISIBLY (a host HUD toast names it)", tag);
	check(hud_msgs >= 1 && strstr(last_hud, "cannot open") != NULL, msg);
	snprintf(msg, sizeof(msg), "%s: the admitted Nether world is released (slot 1 EMPTY)", tag);
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY, msg);
	snprintf(msg, sizeof(msg), "%s: the record is byte-identical", tag);
	check(record_hash(s) == rec, msg);
	clin_capacity_held(&mem1, &mem2, &contexts);
	snprintf(msg, sizeof(msg), "%s: the client holds no reservation", tag);
	check(contexts == 0 && mem1 == 0 && mem2 == 0, msg);
	check(server_local_world_of(s, &s->player) == &s->worlds[0]
			  && s->player.dimension == WORLD_DIM_OVERWORLD, "the host is untouched");
	clin_capacity_set_budget_provider(NULL);
	clin_capacity_set_reply_sender(NULL);
	/* the same Join admits once the client answers again */
	memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
	gstate.players[1].visible_window = -1;
	join_pump(s, 6);
	snprintf(msg, sizeof(msg), "%s: the retry admits once the client can grant", tag);
	check(guest_attached(s), msg);
	if(mode == 1) {
		uint32_t bound = 0;
		clin_capacity_held(&mem1, &mem2, &contexts);
		check(world_local_shared_capacity_mem1_need(81, &bound)
				  && contexts == 1 && mem1 == bound
				  && mem2 == 81u * SERVER_TRANSFER_MEM2_PER_COLUMN,
			"join_cap_refused: configured radius 0 retries with effective 81-column request");
	}
	teardown(s);
}

static void case_adapter(void) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	struct server_player* g = s->guest_actor;

	/* ---- rollback ------------------------------------------------------ */
	bool had_record = false;
	int dim_before = guest_record_dim(s, &had_record);
	double gx = g->x, gy = g->y, gz = g->z;
	server_transfer_test_fault(SERVER_TRANSFER_FAULT_TELEPORTER);
	uint32_t id = 0;
	check(server_transfer_request(s, g, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK,
		  "rollback: the guest's request is accepted");
	check(drive(s, id) == SERVER_TRANSFER_ROLLED_BACK, "rollback: a PREPARE fault ROLLS BACK");
	server_transfer_test_fault(SERVER_TRANSFER_FAULT_NONE);
	bool has_after = false;
	int dim_after = guest_record_dim(s, &has_after);
	check(server_local_world_of(s, g) == &s->worlds[0] && g->dimension == WORLD_DIM_OVERWORLD
			  && g->x == gx && g->y == gy && g->z == gz,
		  "rollback: the guest is exactly where it was (world, dimension, pose)");
	check(has_after == had_record && dim_after == dim_before,
		  "rollback: the guest record is untouched (no marker was written)");

	/* ---- marker + apply ------------------------------------------------ */
	nbt_node* host_dim = nbt_find_by_path(s->level.data, ".Data.Player.Dimension");
	int host_dim_before = host_dim ? host_dim->payload.tag_int : 0;
	id = 0;
	check(server_transfer_request(s, g, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK,
		  "marker: guest -> Nether accepted");
	check(drive(s, id) == SERVER_TRANSFER_COMMITTED, "marker: guest -> Nether COMMITTED");
	const struct server_actor_transition* o = server_transfer_outcome(s, id);
	struct server_world* nether = server_local_world_of(s, g);
	check(o && nether && nether->dimension == WORLD_DIM_NETHER && g->dimension == WORLD_DIM_NETHER,
		  "marker: the guest is committed to the Nether world");
	bool present = false;
	check(guest_record_dim(s, &present) == -1 && present,
		  "marker: the guest record names Dimension -1 (its own)");
	nbt_node* pos = nbt_find_by_path(s->level.data, ".Data.CaveWiiGuest.Pos");
	bool pos_ok = false;
	if(o && pos && pos->type == TAG_LIST) {
		struct list_head* first = pos->payload.tag_list->entry.flink;
		nbt_node* px = list_entry(first, struct nbt_list, entry)->data;
		pos_ok = px && px->type == TAG_DOUBLE && px->payload.tag_double == o->x;
	}
	check(pos_ok, "marker: the guest record's Pos is the committed pose");
	host_dim = nbt_find_by_path(s->level.data, ".Data.Player.Dimension");
	check((host_dim ? host_dim->payload.tag_int : 0) == host_dim_before,
		  "marker: the host record is untouched");
	check(o && g->window_world.generation == o->destination.generation
			  && g->window_world.world_index == o->destination.world_index,
		  "apply: the guest's window world is the destination handle");

	/* ---- ckpt ---------------------------------------------------------- */
	check(server_local_world_of(s, &s->player) == &s->worlds[0],
		  "ckpt: the host stays in the Overworld");
	check(server_local_checkpoint_guests(s), "ckpt: the guest checkpoints from another world");
	check(guest_record_dim(s, &present) == -1 && present,
		  "ckpt: its record keeps its OWN dimension (-1)");

	/* ---- refused ------------------------------------------------------- */
	if(o) {
		struct server_actor_transition stale = *o;
		stale.actor.generation = s->guest_generation + 1;
		check(server_local_guest_on_travel_transaction(s, &stale)
				  == SERVER_ACTOR_TRANSITION_REFUSED,
			  "refused: a stale guest generation is REFUSED");
		struct server_actor_transition host = *o;
		host.actor.slot = ACTOR_SLOT_HOST;
		check(server_local_guest_on_travel_transaction(s, &host)
				  == SERVER_ACTOR_TRANSITION_REFUSED,
			  "refused: a host-slot outcome is REFUSED");
		struct server_actor_transition again = *o;
		check(server_local_guest_on_travel_transaction(s, &again)
				  == SERVER_ACTOR_TRANSITION_APPLIED,
			  "apply: the current outcome re-applies idempotently");
		struct server_actor_transition rb = *o;
		rb.phase = SERVER_TRANSITION_ROLLBACK;
		double ox = g->x;
		check(server_local_guest_on_travel_transaction(s, &rb)
					  == SERVER_ACTOR_TRANSITION_APPLIED
				  && g->x == ox && server_local_world_of(s, g) == nether,
			  "rollback outcome: acknowledged, nothing changes");
	}
	check(!server_transfer_active(s), "nothing is left open");
	teardown(s);
}

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "adapter";
	/* The payload switch flips only while no client world context exists, so
	 * the dwell case sets it before the harness installs one. */
	bool payloads_ok = true;
	if(!strcmp(which, "dwell") || !strcmp(which, "dwell_host_busy")
	   || !strncmp(which, "join_cap_", 9))
		payloads_ok = clin_world_payloads_set_enabled(true);
	harness_init();
	check(payloads_ok, "setup: the payload switch accepted its value");
	if(!strcmp(which, "adapter"))
		case_adapter();
	else if(!strcmp(which, "dwell"))
		case_dwell();
	else if(!strcmp(which, "dwell_off"))
		case_dwell_off();
	else if(!strcmp(which, "reopen_nether"))
		case_reopen_nether();
	else if(!strcmp(which, "reopen_overworld"))
		case_reopen_overworld(false);
	else if(!strcmp(which, "reopen_refusal"))
		case_reopen_overworld(true);
	else if(!strcmp(which, "nether_respawn"))
		case_nether_respawn();
	else if(!strcmp(which, "record_restore"))
		case_record_restore();
	else if(!strcmp(which, "dwell_host_busy"))
		case_dwell_host_busy();
	else if(!strcmp(which, "join_cap_ok"))
		case_join_cap(0);
	else if(!strcmp(which, "join_cap_refused"))
		case_join_cap(1);
	else if(!strcmp(which, "join_cap_timeout"))
		case_join_cap(2);
	else {
		fprintf(stderr, "unknown case %s\n", which);
		return 2;
	}
	return harness_result(TEST_NAME, which);
}
