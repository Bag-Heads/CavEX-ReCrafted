/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924/A1d, C2 row #27: per-guest save and reload
 * across dimensions -- the saved-dimension admission.
 *
 * Beta citations:
 *   Client Jad decompile, ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/:
 *   - EntityPlayer.readEntityFromNBT, EntityPlayer.java:308-323:
 *     dimension = the tag's getInteger("Dimension") (:312) and "Sleeping"
 *     (:313, a sleeper is woken at load :315-318); the spawn triple (:319-322).
 *   - EntityPlayer.writeEntityToNBT, EntityPlayer.java:326-336:
 *     the tag gets setInteger("Dimension", dimension) (:329) -- the player's
 *     dimension is PART OF THE PLAYER RECORD, saved with the inventory.
 *   - WorldInfo.<init>, WorldInfo.java:36-38: the SP level.dat "Player"
 *     compound is read and dimension = playerTag.getInteger("Dimension")
 *     -- the world the player is loaded INTO is chosen from that saved value
 *     (World.java:186-189 picks WorldProvider -1 or 0 from it).
 *   Server CFR decompile, /c/tmp/mcb173-server-decompile/out/net/minecraft/src/:
 *   - ServerConfigurationManager.readPlayerDataFromFile, :114-116 ->
 *     PlayerNBTManager.readPlayerData, :199-204: the player reads the saved tag (readFromNBT) --
 *     the saved record (dimension included) is applied to the joining player
 *     before it enters any world.
 *   - ServerConfigurationManager.playerLoggedIn, :118-127: the joining player's
 *     world is mcServer.getWorldManager(its dimension) -- the player joins THE
 *     WORLD ITS SAVED DIMENSION NAMES, chunk loaded under it, no relocation;
 *     playerLoggedOut, :133-138 writes the record back under the same rule.
 *   - EntityPlayer.readEntityFromNBT, :340-354 / writeEntityToNBT, :356-360:
 *     the server-side record carries "Dimension" identically.
 *
 * Beta therefore never rewrites a saved dimension at load: a player saved in
 * the Nether loads into the Nether. The interim (Astra kickoff ruling 6,
 * 2026-09-25: refuse a Nether-saved guest visibly) is SUPERSEDED by the A2-T
 * ruling (Astra a2t-0927, APPROVED): the guest is admitted into its SAVED
 * dimension's world -- admitted LIVE first, its budget reserved before
 * anything else; an over-budget admission refuses VISIBLY and LOSSLESSLY
 * (never rewrite the dimension, never relocate, never partially admit).
 *
 * Three ctest names, one executable (argv[1]):
 *   reload  -- a Nether-saved guest with the host in the Overworld: over budget
 *              it is REFUSED with the record untouched; within budget it is
 *              ADMITTED into a LIVE Nether world, record untouched; after it
 *              leaves and the record reads 0, the retry admits it unchanged in
 *              the host's Overworld.
 *   visible -- the over-budget refusal is VISIBLE: a HUD toast naming it
 *              reaches the console at the refused Join.
 *   nether  -- the host standing in the Nether, a Nether-saved guest is
 *              admitted into the Nether world with its record intact
 *              (ServerConfigurationManager.playerLoggedIn:120). */
#include "network/server_transfer.h"
#include "support/owner_swap_harness.h"
#include "network/level_archive.h"

#define TEST_NAME "test_owner_swap_dimension_checkpoint_reload"
#define GUEST_RECORD ".Data.CaveWiiGuest"
/* Mark health: internal units, and its Beta half-hearts on disk (see
 * record_health_is_mark). */
#define MARK_HEALTH_UNITS 13
#define MARK_HEALTH_BETA_HALF_HEARTS 2

/* The recognizable checkpoint: four diamonds in the first main slot, health
 * 13, a bed spawn at (7, 4, -9), standing at x 6.5. */
static const struct item_data mark_item = {.id = ITEM_DIAMOND, .count = 4};
static void mark_guest(struct server_player* g) {
	g->inventory.items[INVENTORY_SLOT_MAIN] = mark_item;
	g->health = MARK_HEALTH_UNITS;
	g->has_bed_spawn = true;
	g->bed_spawn_x = 7; g->bed_spawn_y = 4; g->bed_spawn_z = -9;
	body_stand_at(g, 6.5, 0.5);
}
static bool guest_marked(const struct server_player* g) {
	return g->inventory.items[INVENTORY_SLOT_MAIN].id == mark_item.id
		&& g->inventory.items[INVENTORY_SLOT_MAIN].count == mark_item.count
		&& g->health == MARK_HEALTH_UNITS && g->has_bed_spawn && g->bed_spawn_x == 7
		&& g->bed_spawn_y == 4 && g->bed_spawn_z == -9
		&& g->x == 6.5 && g->z == 0.5;
}
static nbt_node* record_int(struct server_local* s, const char* name) {
	char path[64];
	snprintf(path, sizeof(path), GUEST_RECORD ".%s", name);
	nbt_node* n = nbt_find_by_path(s->level.data, path);
	assert(n && (n->type == TAG_INT || n->type == TAG_SHORT));
	return n;
}
static int32_t record_dimension(struct server_local* s) {
	return record_int(s, "Dimension")->payload.tag_int;
}
static void set_record_dimension(struct server_local* s, int32_t dim) {
	record_int(s, "Dimension")->payload.tag_int = dim;
}
/* PLAYER-HEALTH-NBT (a7241b193): the record's Player.Health is Beta
 * HALF-HEARTS (EntityLiving.java:558 `setShort("Health", (short)this.health)`,
 * :565 reads it back), and the exact CaveWii value (8 units per half-heart,
 * MAX_PLAYER_HEALTH 160 == Beta 20) rides in CaveWiiHealth. The mark's 13
 * units are therefore stored as Health = ceil(13 / 8) = 2 (the writer rounds
 * a living fraction AWAY from death) plus CaveWiiHealth = 13; the guest reader
 * honours CaveWiiHealth because it still converts to the stored 2, so the
 * reloaded actor is back at exactly 13 (guest_marked() asserts that after
 * every real reload below). Both raw fields are pinned: a record that lost
 * the marker, or kept internal units in Health, fails here. */
static bool record_health_is_mark(struct server_local* s) {
	return record_int(s, "Health")->payload.tag_short
			== MARK_HEALTH_BETA_HALF_HEARTS
		&& record_int(s, "CaveWiiHealth")->payload.tag_short
			== MARK_HEALTH_UNITS;
}

/* Admit for real, mark the guest, checkpoint it, release it. Returns with the
 * record on disk-state (s->level.data) and no guest. */
static struct server_local* checkpointed(void) {
	/* (A2-T) a second world writes its files beside slot 0's */
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	mark_guest(s->guest_actor);
	assert(server_local_guest_checkpoint(s));
	assert(record_dimension(s) == 0 && record_health_is_mark(s));
	guest_release(s);
	assert(!s->guest_actor && !s->guest_owned);
	return s;
}
/* One Join attempt from the retired client half: three handshake pumps reach
 * ATTACHED on success or FAILED on a rejection. */
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
	return gstate.players[1].local_phase == CLIENT_GUEST_ATTACHED && s->guest_confirmed
		&& s->guest_actor;
}
static bool refused(const struct server_local* s) {
	return gstate.players[1].local_phase == CLIENT_GUEST_FAILED && !s->guest_actor
		&& !s->guest_owned;
}
/* Body admission on the SAVED position: the home column (0,0) is still loaded
 * and published from the first admission, and x 6.5 / z 0.5 lies inside it, so
 * the production readiness predicate passes without moving the body. */
static void admit_body(struct server_local* s) {
	assert(server_world_is_chunk_loaded(&s->worlds[0], 0, 0));
	assert(client_session_request_local_gameplay(&gstate.players[1]));
	pump_handshake(s);
}

/* ======================================================================== */
static void case_reload(void) {
	struct server_local* s = checkpointed();
	nbt_node* record = nbt_find_by_path(s->level.data, GUEST_RECORD);
	assert(record);

	/* A Nether-saved guest, host in the Overworld, OVER BUDGET: refused,
	 * losslessly (the admission reserves the world's budget first). */
	set_record_dimension(s, -1);
	counters_reset();
	server_transfer_test_set_carve_free(512u * 1024u);
	join(s);
	check(refused(s),
		"refusal: over budget, a guest saved in dimension -1 is REFUSED while the "
		"host stands in the Overworld (no body, no membership) -- never relocated");
	check(server_local_world_slot_state(s, 1) == SERVER_WORLD_SLOT_EMPTY,
		"refusal: no world was admitted");
	check(nbt_find_by_path(s->level.data, GUEST_RECORD) == record
		&& record_dimension(s) == -1 && record_health_is_mark(s),
		"lossless: the record is untouched by the refusal -- same node, "
		"Dimension still -1, Health still 2 half-hearts + CaveWiiHealth 13 "
		"(EntityPlayer.writeEntityToNBT:329 "
		"is the only writer of Dimension; a load never rewrites it)");
	/* Within budget: admitted into the Nether world, the saved dimension. */
	server_transfer_test_set_carve_free(SIZE_MAX);
	join(s);
	check(attached(s), "admission: within budget the Nether-saved guest is ADMITTED");
	if(attached(s)) {
		struct server_world* w = server_local_world_of(s, s->guest_actor);
		check(w && w != &s->worlds[0] && w->dimension == WORLD_DIM_NETHER
			&& server_local_world_live(w) && guest_marked(s->guest_actor)
			&& s->guest_actor->dimension == WORLD_DIM_NETHER,
			"admission: into a LIVE Nether world, carrying the checkpoint unchanged");
		check(record_dimension(s) == -1 && record_health_is_mark(s),
			"admission: the Join never rewrote the record");
		guest_release(s);
		check(server_local_world_retire(s, w), "the emptied Nether world retires");
	}

	/* Retry after the record reads the host's dimension again: admitted as
	 * saved (PlayerNBTManager.readPlayerData:199-204 applies the record). */
	set_record_dimension(s, 0);
	join(s);
	check(attached(s), "retry: the same record with Dimension 0 is admitted");
	if(attached(s)) {
		struct server_player* g = s->guest_actor;
		check(guest_marked(g),
			"reload: the readmitted actor carries the checkpoint unchanged before "
			"any body placement -- four diamonds, health 13, bed spawn (7,4,-9), "
			"standing at (6.5, 0.5)");
		admit_body(s);
		check(s->guest_gameplay && g->finished_loading && guest_marked(g),
			"reload: the admitted body still carries it after gameplay admission");
		check(g->dimension == 0 && s->player.dimension == 0,
			"reload: the body's dimension is the SAVED one, equal to the host's");
	}
	teardown(s);
}

/* ======================================================================== */
static void case_visible(void) {
	struct server_local* s = checkpointed();
	set_record_dimension(s, -1);
	counters_reset();
	server_transfer_test_set_carve_free(512u * 1024u);
	join(s);
	server_transfer_test_set_carve_free(SIZE_MAX);
	check(refused(s), "setup: the Nether-saved guest is refused over budget");
	check(hud_msgs >= 1 && strstr(last_hud, "Cannot open") != NULL,
		"visible: the refused Join publishes a HUD toast naming the reason "
		"(\"Cannot open the other dimension now\") on the console, not a bare "
		"LOCAL_GUEST_REJECTED");
	check(record_dimension(s) == -1 && record_health_is_mark(s),
		"lossless: the record survives the visible refusal");
	teardown(s);
}

/* ======================================================================== */
static void case_nether(void) {
	struct server_local* s = checkpointed();
	set_record_dimension(s, -1);
	/* The host stands in the Nether. On this single-world fixture that is the
	 * committed dimension of the host body and its world; A2 gives the guest
	 * a second live world to be admitted into. */
	s->player.dimension = WORLD_DIM_NETHER;
	s->worlds[0].dimension = WORLD_DIM_NETHER;
	s->worlds[0].dimension = WORLD_DIM_NETHER;
	join(s);
	check(attached(s),
		"nether: a Nether-saved guest rejoining a host who stands in the Nether "
		"is ADMITTED, not refused (ServerConfigurationManager.playerLoggedIn:120, "
		"the world of the saved dimension)");
	if(attached(s)) {
		struct server_player* g = s->guest_actor;
		admit_body(s);
		struct server_world* w = server_local_world_of(s, g);
		check(s->guest_gameplay && g->finished_loading && guest_marked(g)
			&& g->dimension == WORLD_DIM_NETHER,
			"nether: the body carries the checkpoint unchanged, dimension -1");
		check(w && w->dimension == WORLD_DIM_NETHER,
			"nether: the body's committed live world IS a Nether world");
	} else {
		check(record_dimension(s) == -1 && record_health_is_mark(s),
			"lossless: while refused, the Nether record is untouched");
	}
	s->player.dimension = WORLD_DIM_OVERWORLD;
	s->worlds[0].dimension = WORLD_DIM_OVERWORLD;
	s->worlds[0].dimension = WORLD_DIM_OVERWORLD;
	teardown(s);
}

/* ======================================================================== */
int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : NULL;
	if(!which
	   || (strcmp(which, "reload") && strcmp(which, "visible")
		   && strcmp(which, "nether"))) {
		fprintf(stderr, "usage: %s reload|visible|nether  (one case per process)\n",
			argv[0]);
		return 2;
	}
	harness_init();
	if(!strcmp(which, "reload"))
		case_reload();
	else if(!strcmp(which, "visible"))
		case_visible();
	else
		case_nether();
	return harness_result(TEST_NAME, which);
}
