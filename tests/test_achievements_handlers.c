/* B2 production boundaries. Gameplay stimuli use real load/ACK, Join,
 * gameplay admission, damage/respawn, A2 transfer and Leave handlers.
 * Jump/inventory facts are labelled injected consumer witnesses: C1/C3
 * producer wiring is a subsequent card. No native/Wii proof is claimed. */
#include "support/owner_swap_harness.h"
#include "network/server_transfer.h"
#include "network/region_archive.h"
#include "network/client_world_payload.h"
#include "game/achievements/achievements.h"
#include <pthread.h>

static pthread_t main_thread;
static unsigned io_calls;
static char root[128], path[200];

/* Exact signatures below match the persistence API; wrappers prove that
 * production server/consumer threads never execute achievement disk I/O. */
enum stats_save_status __real_stats_profile_save(const char*, const struct stats_profile*);
enum stats_save_status __wrap_stats_profile_save(const char* dir,
	const struct stats_profile* p) {
	assert(pthread_equal(main_thread, pthread_self()));
	io_calls++;
	return __real_stats_profile_save(dir, p);
}
void __real_stats_profile_load(const char*, const char*, struct stats_profile*,
	enum stats_load_status*);
void __wrap_stats_profile_load(const char* dir, const char* id,
	struct stats_profile* p, enum stats_load_status* status) {
	assert(pthread_equal(main_thread, pthread_self()));
	io_calls++;
	__real_stats_profile_load(dir, id, p, status);
}
bool __real_stats_save_step(struct stats_save_job*);
bool __wrap_stats_save_step(struct stats_save_job* job) {
	assert(pthread_equal(main_thread, pthread_self()));
	io_calls++;
	return __real_stats_save_step(job);
}

static struct ach_status status(unsigned slot) {
	struct ach_status st;
	assert(achievements_debug_status(slot, &st));
	return st;
}
static void fact(unsigned slot, uint64_t epoch) {
	game_event_emit(&(struct game_event){.kind = GE_JUMPED,
		.authority = GE_AUTH_CLIENT, .who = {.slot = slot, .generation = epoch}});
}
static unsigned jumps(unsigned slot) {
	return stats_counter_total(&achievements_debug_profile(slot)->general[
		stat_general_index(2010)]);
}
static void load(struct server_local* s) {
	struct server_rpc rpc = {.type = SRPC_LOAD_WORLD};
	string_init_set_str(rpc.payload.load_world.name, path);
	host_send(s, &rpc); /* handler consumes the payload string */
}
static void admitted(struct server_local* s) {
	unsigned before = markers;
	for(unsigned i = 0; i < 2000 && markers == before; i++) tick(s);
	assert(markers == before + 1 && s->input_ack_pending);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	tick(s); assert(!s->input_ack_pending);
	achievements_tick();
	assert(status(0).bound); /* RED on the pre-B2 load/ACK handlers */
	assert(status(0).generation != 0);
}
static void guest_join(struct server_local* s) {
	guest_attach(s);
	achievements_tick();
	assert(!status(1).bound); /* OFFER/ATTACHED is not gameplay admission */
	struct server_player* p = s->guest_actor;
	p->health = -1;
	assert(client_session_request_local_gameplay(&gstate.players[1]));
	pump_handshake(s);
	achievements_tick();
	assert(!s->guest_gameplay && !status(1).bound);
	p->health = MAX_PLAYER_HEALTH;
	guest_body_home(s);
	assert(client_session_request_local_gameplay(&gstate.players[1]));
	pump_handshake(s);
	achievements_tick();
	assert(status(1).bound && status(1).generation == s->guest_generation);
}
static void guest_leave(struct server_local* s) {
	assert(client_session_request_local_leave(&gstate.players[1]));
	pump_handshake(s);
	achievements_tick();
	assert(!s->guest_actor && !status(1).bound);
}

static void bodies(struct server_local* s, uint64_t host_epoch, uint64_t guest_epoch) {
	struct server_player* g = s->guest_actor;
	bool accepted = false;
	(void)server_local_damage_actor_result(s, g, MAX_PLAYER_HEALTH * 2, 0, 0, &accepted);
	clin_update_drain();
	assert(accepted && g->health == 0);
	achievements_tick();
	assert(status(1).generation == guest_epoch && status(1).bound);
	struct server_rpc respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = s->guest_generation}};
	assert(guest_send(s, &respawn));
	for(unsigned i = 0; i < 2000 && s->guest_respawn_phase != GUEST_RESPAWN_LOADING; i++) {
		s->paused = false; tick(s); s->paused = true;
	}
	assert(s->guest_respawn_phase == GUEST_RESPAWN_LOADING);
	struct server_world* w = server_local_guest_respawn_target(s); assert(w);
	w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_x));
	w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_z));
	struct server_chunk* c = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
	if(!c) assert(server_world_load_chunk(w, cx, cz, &c));
	c->sent = true; /* backend terrain witness, no native mesh claim */
	struct server_rpc ready = {.type = SRPC_LOCAL_GUEST, .payload.local_guest = {
		.ticket = s->guest_ticket, .action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
		.generation = s->guest_generation, .respawn_epoch = s->guest_respawn_epoch}};
	assert(guest_send(s, &ready));
	server_local_guest_respawn_poll(s); clin_update_drain();
	assert(g->health == MAX_PLAYER_HEALTH);
	achievements_tick();
	assert(status(0).generation == host_epoch && status(1).generation == guest_epoch);
	accepted = false;
	(void)server_local_damage_actor_result(s, &s->player, MAX_PLAYER_HEALTH * 2, 0, 0, &accepted);
	clin_update_drain(); assert(accepted && s->player.health <= 0);
	struct server_rpc host_respawn = {.type = SRPC_RESPAWN};
	host_send(s, &host_respawn);
	for(unsigned i = 0; i < 2000 && s->host_respawn_phase != HOST_RESPAWN_PHASE_LOADING; i++) {
		s->paused = false; tick(s); s->paused = true;
	}
	assert(s->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING);
	w = server_local_world_of(s, &s->player); assert(w);
	cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->host_respawn_x));
	cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->host_respawn_z));
	c = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
	if(!c) assert(server_world_load_chunk(w, cx, cz, &c));
	c->sent = true; /* backend target-column witness, as for P2 */
	host_respawn.payload.respawn.respawn_epoch = s->host_respawn_epoch;
	host_send(s, &host_respawn);
	server_local_host_respawn_poll(s); clin_update_drain();
	assert(s->player.health == MAX_PLAYER_HEALTH);
	achievements_tick();
	assert(status(0).generation == host_epoch && status(1).generation == guest_epoch);
	puts("PASS real host/guest death and owned respawn RPCs retain profiles/epochs");
}

static void transfer(struct server_local* s, unsigned owner) {
	struct server_player* p = owner ? s->guest_actor : &s->player;
	uint64_t h = status(0).generation, g = status(1).generation;
	uint32_t id = 0;
	assert(server_transfer_request(s, p, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK);
	s->paused = false;
	for(unsigned i = 0; i < 4000; i++) {
		enum server_transfer_state state = server_transfer_state_of(s, id);
		if(state == SERVER_TRANSFER_COMMITTED || state == SERVER_TRANSFER_ROLLED_BACK) break;
		tick(s); client_acks_marker(s);
	}
	s->paused = true;
	assert(server_transfer_state_of(s, id) == SERVER_TRANSFER_COMMITTED);
	achievements_tick();
	assert(status(0).bound && status(1).bound);
	assert(status(0).generation == h && status(1).generation == g);
	assert(strcmp(status(0).profile_id, "handler1") == 0);
	assert(strcmp(status(1).profile_id, "handler2") == 0);
	printf("PASS real owner %u dimension transfer retains achievement identity\n", owner);
}

static void shutdown_handler(struct server_local* s) {
	assert(server_local_start(s));
	struct server_rpc rpc = {.type = SRPC_LOAD_WORLD};
	string_init_set_str(rpc.payload.load_world.name, path);
	assert(svin_rpc_send(&rpc));
	ptime_t start = time_get();
	while(!status(0).bound) {
		clin_update();
		screen_load_world.update(&screen_load_world, 30.0f);
		clin_update_input_readiness();
		achievements_tick();
		assert(time_diff_s(start, time_get()) < 30);
		thread_msleep(1);
	}
	uint64_t epoch = status(0).generation;
	fact(0, epoch);
	stats_io_fail_at(STATS_IO_STAGE_WRITE);
	server_local_shutdown(s); /* actual producer exit + drain/join */
	assert(!game_event_owner_epoch(0));
	achievements_tick();
	assert(!status(0).bound && status(0).dirty && jumps(0) == 1);
	assert(!achievements_shutdown()); /* may not erase the retained copy */
	achievements_request_save(0);
	for(unsigned i = 0; i < 100 && !achievements_save_durable(0); i++) achievements_tick();
	assert(achievements_save_durable(0));
	assert(achievements_shutdown());
	/* Cleanup ownership handoff AFTER the real join. Region archives were
	 * created by that worker; this fixture now explicitly adopts them only
	 * to close/remove its files, never to certify a gameplay transition. */
	for(size_t i = 0; i < server_world_region_count(&s->worlds[0]); i++)
		server_world_region_at(&s->worlds[0], i)->owner_thread = (unsigned long)pthread_self();
	assert(server_local_unload_world_impl(s, false));
	printf("PASS real server thread exit/join, failed shutdown save, durable Retry; %u main IOs\n", io_calls);
}

int main(int argc, char** argv) {
	main_thread = pthread_self();
	test_fixture_dir("test_achievements_handlers", root, sizeof(root));
#ifdef _WIN32
	assert(_mkdir(root) == 0);
#else
	assert(mkdir(root, 0755) == 0);
#endif
	snprintf(path, sizeof(path), "%s/world", root);
#ifdef _WIN32
	assert(_mkdir(path) == 0);
#else
	assert(mkdir(path, 0755) == 0);
#endif
	assert(achievements_init(root));
	assert(achievements_set_profiles("handler1", "handler2"));
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init(); items_init(); recipe_init();
	assert(clin_init() && svin_init());
	assert(clin_world_payloads_set_enabled(true));
	world_create(&gstate.world); dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);
	struct server_local* s = calloc(1, sizeof(*s)); assert(s);
	server_local_test_create_without_thread(s, false, false, false, false, false, true, false);
	s->paused = true; s->split_view_radius = 4;
	string_t name; string_init_set_str(name, path);
	assert(level_archive_create_new(&s->level, name, 12345, (vec3){0.5, 4, 0.5}));
	assert(level_archive_save(&s->level)); level_archive_discard(&s->level);
	string_clear(name);
	if(argc > 1 && !strcmp(argv[1], "shutdown")) {
		achievements_set_cooperative_saves(true);
		shutdown_handler(s);
		goto cleanup;
	}
	server_local_test_fail_next_world_load();
	load(s); achievements_tick();
	assert(!s->player.has_pos && !status(0).bound);
	/* Failure UI's Back/unload recovery clears the terminal barrier. */
	s->load_failure_terminal = false;
	load(s); assert(s->player.has_pos);
	achievements_tick(); assert(!status(0).bound); /* still streaming */
	admitted(s);
	uint64_t host_epoch = status(0).generation;
	assert(strcmp(status(0).profile_id, "handler1") == 0);
	puts("PASS real failed load / load / input admission binds host only on ACK");
	s->thread_started = true; enable_split(s);
	guest_join(s);
	uint64_t guest_epoch = status(1).generation;
	fact(0, host_epoch); fact(1, guest_epoch); fact(1, guest_epoch);
	achievements_tick(); assert(jumps(0) == 1 && jumps(1) == 2);
	bodies(s, host_epoch, guest_epoch);
	/* Death UI has retained presentation flags; settle through existing READY. */
	for(unsigned i = 0; i < 2000 && s->input_ack_pending; i++) {
		client_acks_marker(s); tick(s);
	}
	transfer(s, 1);
	/* Transfer transaction is complete; owner 0 may then travel independently. */
	transfer(s, 0);
	guest_leave(s);
	guest_join(s);
	uint64_t second_guest = status(1).generation;
	assert(second_guest != guest_epoch && status(0).generation == host_epoch);
	fact(1, guest_epoch); fact(1, second_guest);
	achievements_tick(); assert(jumps(1) == 3 && jumps(0) == 1);
	guest_leave(s);
	puts("PASS real P2 leave/readmission rejects old events without changing P1");
	fact(0, host_epoch);
	stats_io_fail_at(STATS_IO_STAGE_WRITE);
	struct server_rpc unload = {.type = SRPC_UNLOAD_WORLD}; host_send(s, &unload);
	achievements_tick();
	assert(!s->player.has_pos && status(0).dirty && !game_event_owner_epoch(0));
	assert(achievements_lifecycle_pending());
	assert(jumps(0) == 2);
	fact(0, host_epoch); achievements_tick(); assert(jumps(0) == 2);
	load(s); admitted(s); /* explicit re-entry retries retained persistence */
	assert(status(0).generation != host_epoch && jumps(0) == 2);
	fact(0, host_epoch); achievements_tick(); assert(jumps(0) == 2);
	host_send(s, &unload); achievements_tick();
	assert(!status(0).bound);
	puts("PASS title/reload retries failed flush; distinct host epoch rejects delayed events");
	/* Real main-thread provider lifecycle entry point used by main.c. */
	struct client_session* host = &gstate.players[0];
	client_session_achievements_remote(host, 7, false);
	achievements_tick(); assert(!status(0).bound);
	client_session_achievements_remote(host, 7, true);
	achievements_tick(); assert(status(0).bound && status(0).mode == ACH_MODE_REMOTE_SERVER);
	uint64_t remote_epoch = status(0).generation;
	fact(0, remote_epoch); achievements_tick();
	client_session_achievements_remote(host, 7, false); achievements_tick();
	/* A reallocated remote provider may reuse its transport generation. */
	client_session_achievements_remote(host, 7, true);
	fact(0, remote_epoch); fact(0, host->achievement_epoch); achievements_tick();
	const struct stats_pair* pair = &achievements_debug_profile(0)->general[stat_general_index(2010)];
	assert(pair->local == 2 && pair->remote == 2);
	assert(status(0).generation != remote_epoch);
	client_session_achievements_remote(host, 7, false); achievements_tick();
	assert(!status(0).bound);
	assert(achievements_shutdown());
	printf("PASS remote failed connect/admission/reconnect; %u disk operations all on main\n", io_calls);
cleanup:
	server_local_worlds_discard_secondary(s);
	/* Cleanup only, after the actual unload; same ownership as split fixture. */
	server_world_populate_scratch_destroy(s->world_populate_scratch);
	inventory_destroy(&s->player.inventory, &s->player);
	dict_entity_clear(s->entities); string_clear(s->level_name);
	dict_autosave_saved_clear(s->autosave_saved_this_epoch);
	for(unsigned i = 0; i < SERVER_LOCAL_WORLD_SLOTS; i++)
		dict_autosave_saved_clear(s->autosave_saved_this_epoch_w[i]);
	if(s->residency) mem2_free(s->residency);
	free(s); test_fixture_dir_cleanup(root);
	puts("PASS test_achievements_handlers");
	return 0;
}
