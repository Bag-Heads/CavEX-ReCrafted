/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924 lane C, C3: the #27 contract / fault matrix for
 * the guest's saved-dimension admission and checkpoint (C2inv row #27; Astra
 * next-0926 "C3: #27 adapters first").
 *
 * Beta citations (see test_owner_swap_dimension_checkpoint_reload.c for the
 * full set): EntityPlayer.writeEntityToNBT:329 is the only writer of
 * "Dimension"; readEntityFromNBT:312 and the server's
 * ServerConfigurationManager.playerLoggedIn:118-127 load a player INTO the
 * world its saved dimension names, never relocating or rewriting it.
 *
 * Contract pinned here (the adapters in server_local_guest.c consume A2's
 * admission / checkpoint / transaction outcomes; until A2 lands the guest can
 * only be admitted into the host's committed world):
 *   refusal_preserves   a refused Join (saved in the other dimension) leaves
 *                       the saved record byte-identical, and a host autosave
 *                       with no guest body does not touch it either
 *   join_during_switch  a Join while a world switch (travel or death, incl.
 *                       its rollback leg) is in flight is refused losslessly;
 *                       the retry after IDLE admits the unchanged record
 *   failed_admission    a corrupt record refuses admission with the record
 *                       byte-identical and no body; the repaired record admits
 *   failed_checkpoint   a refused checkpoint keeps the PREVIOUS record
 *                       byte-identical and the guest owned; the next settled
 *                       checkpoint replaces it
 *   null_accessors      every guest entry point answers NULL / a missing
 *                       guest / a missing transaction without a crash or a
 *                       state change (transactions: REFUSED, not PENDING)
 * WILL_FAIL until A2 (the second live world + A2-T transactions):
 *   host_nether_guest_overworld  the host in the Nether, an Overworld-saved
 *                       guest is admitted into the OVERWORLD world
 *   checkpoint_other_dimension   a guest body standing in the other dimension
 *                       checkpoints its OWN dimension instead of refusing */
#include "support/owner_swap_harness.h"
#include "network/level_archive.h"
#include "network/server_local_guest.h"
#include "network/server_transfer.h"

#define TEST_NAME "test_c27_guest_contract"
#define GUEST_RECORD ".Data.CaveWiiGuest"

static const struct item_data mark_item = {.id = ITEM_DIAMOND, .count = 4};
static void mark_guest(struct server_player* g) {
	g->inventory.items[INVENTORY_SLOT_MAIN] = mark_item;
	g->health = 13;
	body_stand_at(g, 6.5, 0.5);
}
static nbt_node* record(struct server_local* s) {
	return nbt_find_by_path(s->level.data, GUEST_RECORD);
}
static nbt_node* record_field(struct server_local* s, const char* name) {
	char path[64];
	snprintf(path, sizeof(path), GUEST_RECORD ".%s", name);
	return nbt_find_by_path(s->level.data, path);
}
/* The saved record as bytes (the serialized form a level.dat write emits). */
struct bytes {
	unsigned char* data;
	size_t len;
};
static struct bytes record_bytes(struct server_local* s) {
	nbt_node* r = record(s);
	assert(r);
	struct buffer b = nbt_dump_binary(r);
	assert(b.data);
	return (struct bytes) {b.data, b.len};
}
static bool same_bytes(struct bytes a, struct bytes b) {
	return a.len == b.len && !memcmp(a.data, b.data, a.len);
}
static struct server_local* checkpointed(void) {
	struct server_local* s = admitted_fixture();
	/* a second world writes its files beside slot 0's: bind the save root */
	string_set_str(s->level_name, split_path);
	mark_guest(s->guest_actor);
	assert(server_local_guest_checkpoint(s));
	guest_release(s);
	assert(!s->guest_actor && !s->guest_owned && record(s));
	return s;
}
static void join(struct server_local* s) {
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
static bool attached(const struct server_local* s) {
	return gstate.players[1].local_phase == CLIENT_GUEST_ATTACHED
		&& s->guest_confirmed && s->guest_actor;
}
static bool refused(const struct server_local* s) {
	return gstate.players[1].local_phase == CLIENT_GUEST_FAILED
		&& !s->guest_actor && !s->guest_owned;
}
static void set_dimension(struct server_local* s, int32_t d) {
	nbt_node* n = record_field(s, "Dimension");
	assert(n && n->type == TAG_INT);
	n->payload.tag_int = d;
}

/* ======================================================================== */
static void case_refusal_preserves(void) {
	/* A2-T (Astra a2t-0927): a guest saved in the Nether while the host is in
	 * the Overworld is ADMITTED into the Nether -- but the budget is reserved
	 * first, and an over-budget admission refuses VISIBLY and losslessly. */
	struct server_local* s = checkpointed();
	set_dimension(s, -1);
	struct bytes before = record_bytes(s);
	server_transfer_test_set_carve_free(512u * 1024u); /* below the 1 MiB floor */
	join(s);
	check(refused(s), "over budget: the Nether-saved guest is refused (host Overworld)");
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY,
		"  ...no world was admitted (slot 1 EMPTY)");
	check(server_local_world_of(s, &s->player) == &s->worlds[0]
		&& s->player.dimension == WORLD_DIM_OVERWORLD,
		"  ...the host is untouched");
	struct bytes after = record_bytes(s);
	check(same_bytes(before, after),
		"pre-activation refusal: the saved record is byte-identical "
		"(dimension, inventory, health, private state)");
	check(server_local_guest_checkpoint(s),
		"host autosave with no guest body succeeds");
	struct bytes autosaved = record_bytes(s);
	check(same_bytes(before, autosaved),
		"and leaves the refused guest's record byte-identical");
	/* the budget back: the same Join admits into the saved dimension */
	server_transfer_test_set_carve_free(SIZE_MAX);
	join(s);
	check(attached(s), "within budget: the Nether-saved guest is ADMITTED");
	if(attached(s)) {
		struct server_world* w = server_local_world_of(s, s->guest_actor);
		check(w && w == &s->worlds[1] && w->dimension == WORLD_DIM_NETHER
			&& server_local_world_live(w) && s->guest_actor->dimension == WORLD_DIM_NETHER,
			"  ...into the LIVE Nether world (slot 1), its saved dimension");
		check(server_local_world_of(s, &s->player) == &s->worlds[0],
			"  ...the host stays in the Overworld");
		struct bytes joined = record_bytes(s);
		check(same_bytes(before, joined), "  ...and the Join never rewrote the record");
		free(joined.data);
	}
	free(before.data); free(after.data); free(autosaved.data);
	teardown(s);
}

static void case_join_during_switch(void) {
	struct server_local* s = checkpointed();
	struct bytes before = record_bytes(s);
	const enum server_world_switch_phase phases[2] = {
		SERVER_WORLD_SWITCH_PORTAL, SERVER_WORLD_SWITCH_DEATH};
	for(int i = 0; i < 2; i++) {
		s->world_switch_phase = phases[i];
		join(s);
		check(refused(s), i == 0
			? "a Join during a travel switch (incl. its rollback leg) is refused"
			: "a Join during a death switch is refused");
		struct bytes now = record_bytes(s);
		check(same_bytes(before, now), "  ...and the record is byte-identical");
		free(now.data);
	}
	s->world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	join(s);
	check(attached(s), "the retry after the switch settles admits the record");
	if(attached(s))
		check(s->guest_actor->health == 13
			&& s->guest_actor->inventory.items[INVENTORY_SLOT_MAIN].id
				== mark_item.id,
			"  ...unchanged (health 13, the diamonds)");
	free(before.data);
	teardown(s);
}

static void case_failed_admission(void) {
	struct server_local* s = checkpointed();
	nbt_node* hotbar = record_field(s, "CaveWiiHotbar");
	assert(hotbar && hotbar->type == TAG_INT);
	int32_t good = hotbar->payload.tag_int;
	hotbar->payload.tag_int = 99; /* corrupt: out of the hotbar range */
	struct bytes before = record_bytes(s);
	join(s);
	check(refused(s), "a corrupt record refuses admission (no body)");
	struct bytes after = record_bytes(s);
	check(same_bytes(before, after), "  ...and is left byte-identical");
	record_field(s, "CaveWiiHotbar")->payload.tag_int = good;
	join(s);
	check(attached(s), "the repaired record admits");
	free(before.data); free(after.data);
	teardown(s);
}

static void case_failed_checkpoint(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* g = s->guest_actor;
	mark_guest(g);
	assert(server_local_guest_checkpoint(s));
	struct bytes first = record_bytes(s);
	g->inventory.items[INVENTORY_SLOT_MAIN]
		= (struct item_data) {.id = ITEM_COAL, .count = 1};
	g->riding_boat = true; /* unsettled ownership: the checkpoint refuses */
	check(!server_local_guest_checkpoint(s), "an unsettled guest refuses the checkpoint");
	struct bytes kept = record_bytes(s);
	check(same_bytes(first, kept),
		"  ...the PREVIOUS record stays byte-identical (no partial write)");
	check(s->guest_owned && s->guest_actor == g, "  ...and the guest stays owned");
	g->riding_boat = false;
	check(server_local_guest_checkpoint(s), "the settled guest checkpoints again");
	struct bytes second = record_bytes(s);
	check(!same_bytes(first, second), "  ...replacing the record");
	free(first.data); free(kept.data); free(second.data);
	teardown(s);
}

static void case_null_accessors(void) {
	struct server_rpc_source src = {.slot = 1, .generation = 1};
	check(!server_local_guest_create(NULL, &src), "create(NULL, src) refuses");
	check(!server_local_guest_checkpoint(NULL), "checkpoint(NULL) refuses");
	check(!server_local_guest_publish(NULL, src), "publish(NULL) refuses");
	check(!server_local_guest_settled(NULL), "settled(NULL) is false");
	check(!server_local_guest_leave(NULL, src), "leave(NULL) refuses");
	check(!server_local_guest_gameplay(NULL, src), "gameplay(NULL) refuses");
	server_local_guest_respawn_poll(NULL);
	server_local_guest_correct_position(NULL);
	check(!server_local_guest_correct_position_for(NULL, 1, 0),
		"correct_position_for(NULL) refuses");
	check(server_local_guest_on_travel_transaction(NULL, NULL)
		== SERVER_ACTOR_TRANSITION_REFUSED,
		"on_travel_transaction(NULL, NULL) is REFUSED (never PENDING)");
	check(server_local_guest_on_respawn_transaction(NULL, NULL)
		== SERVER_ACTOR_TRANSITION_REFUSED,
		"on_respawn_transaction(NULL, NULL) is REFUSED");

	struct server_local* s = checkpointed();
	check(!server_local_guest_create(s, NULL), "create(s, NULL) refuses");
	check(server_local_world_of(s, NULL) == NULL, "world_of(s, NULL) is NULL");
	server_local_actor_hud_message(s, NULL, "x");
	check(server_local_guest_on_travel_transaction(s, NULL)
		== SERVER_ACTOR_TRANSITION_REFUSED,
		"on_travel_transaction without a transaction is REFUSED");
	check(server_local_guest_on_respawn_transaction(s, NULL)
		== SERVER_ACTOR_TRANSITION_REFUSED,
		"on_respawn_transaction without a guest body is REFUSED");
	/* The host's committed world not live (release -> load window): the
	 * admission accessor answers NULL and the Join refuses losslessly. */
	struct bytes before = record_bytes(s);
	uint64_t gen = s->worlds[0].generation;
	s->worlds[0].generation = 0;
	join(s);
	check(refused(s), "a Join with no live host world refuses (NULL world)");
	s->worlds[0].generation = gen;
	struct bytes after = record_bytes(s);
	check(same_bytes(before, after), "  ...and the record is byte-identical");
	free(before.data); free(after.data);
	teardown(s);
}

/* ======================================================================== */
static void case_host_nether_guest_overworld(void) {
	struct server_local* s = checkpointed();
	s->player.dimension = WORLD_DIM_NETHER;
	s->worlds[0].dimension = WORLD_DIM_NETHER;
	struct bytes before = record_bytes(s);
	join(s);
	check(attached(s),
		"A2: the host in the Nether, an Overworld-saved guest is ADMITTED "
		"into the Overworld world (playerLoggedIn:120)");
	if(attached(s)) {
		struct server_world* w = server_local_world_of(s, s->guest_actor);
		check(w && w != &s->worlds[0] && w->dimension == WORLD_DIM_OVERWORLD
			&& server_local_world_live(w)
			&& s->guest_actor->dimension == WORLD_DIM_OVERWORLD,
			"  ...its committed world is a LIVE Overworld beside the host's Nether");
		struct bytes after = record_bytes(s);
		check(same_bytes(before, after), "  ...and the record was never rewritten");
		free(after.data);
	}
	free(before.data);
	s->player.dimension = WORLD_DIM_OVERWORLD;
	s->worlds[0].dimension = WORLD_DIM_OVERWORLD;
	teardown(s);
}

static void case_checkpoint_other_dimension(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* g = s->guest_actor;
	mark_guest(g);
	g->dimension = WORLD_DIM_NETHER; /* A2: the body committed to the Nether */
	bool ok = server_local_guest_checkpoint(s);
	check(ok, "A2: a guest in the other dimension checkpoints "
		"(writeEntityToNBT:329 writes its OWN dimension)  [WILL_FAIL until A2]");
	nbt_node* d = record_field(s, "Dimension");
	check(ok && d && d->payload.tag_int == -1, "  ...with Dimension -1");
	g->dimension = WORLD_DIM_OVERWORLD;
	teardown(s);
}

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "";
	harness_init();
	if(!strcmp(which, "refusal_preserves"))
		case_refusal_preserves();
	else if(!strcmp(which, "join_during_switch"))
		case_join_during_switch();
	else if(!strcmp(which, "failed_admission"))
		case_failed_admission();
	else if(!strcmp(which, "failed_checkpoint"))
		case_failed_checkpoint();
	else if(!strcmp(which, "null_accessors"))
		case_null_accessors();
	else if(!strcmp(which, "host_nether_guest_overworld"))
		case_host_nether_guest_overworld();
	else if(!strcmp(which, "checkpoint_other_dimension"))
		case_checkpoint_other_dimension();
	else {
		fprintf(stderr, "unknown case '%s'\n", which);
		return 2;
	}
	return harness_result(TEST_NAME, which);
}
