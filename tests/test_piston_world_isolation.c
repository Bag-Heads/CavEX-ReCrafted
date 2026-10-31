/* Secondary-world completion regression (ASTRA-G9-FINAL item 1).
 * Real split admission, then both committed dimension assignments. Each owner
 * powers its piston through SRPC_BLOCK_PLACE at identical coordinates.
 * Beta TileEntityPiston.updateEntity (mcb173-retromcp minecraft/src_original):
 * progress += .5F; the third visit settles, in its own World.updateEntities.
 * Its getEntitiesWithinAABBExcludingEntity query cannot see another world.
 * Guest-only body pushing remains a separate known query-membership gap; this
 * regression requires both owners' real block actions/completion and forbids
 * a foreign body push, with a positive host-body push control.
 */
#include "support/owner_swap_harness.h"
#include "block/redstone_piston_server.h"

#define TEST_NAME "test_piston_world_isolation"
#define Y 101

static struct block_data cell(struct server_world* w, int x, int y, int z) {
	struct block_data b = {0};
	assert(server_world_get_block(w, x, y, z, &b));
	return b;
}

static void set_cell(struct server_world* w, int x, int y, int z,
	uint8_t type, uint8_t meta) {
	struct block_data b = cell(w, x, y, z);
	/* Beta returns false for an identical no-op (air-clearing in particular). */
	if(b.type != type || b.metadata != meta)
		assert(server_world_set_block_silent(w, x, y, z,
			(struct block_data) {.type = type, .metadata = meta}));
}

static void prepare_piston(struct server_world* w) {
	struct server_chunk* c = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
	for(unsigned n = 0; n < 200 && !c; n++) {
		server_world_begin_update(w);
		(void)server_world_load_chunk(w, 0, 0, &c);
	}
	assert(c);
	for(int x = 3; x <= 9; x++)
		for(int z = 3; z <= 7; z++) {
			set_cell(w, x, Y - 1, z, BLOCK_STONE, 0);
			for(int y = Y; y <= Y + 3; y++)
				set_cell(w, x, y, z, BLOCK_AIR, 0);
		}
	set_cell(w, 4, Y, 5, BLOCK_PISTON_BASE, 5); /* +X */
	set_cell(w, 5, Y, 5, BLOCK_WOOL, 7);
}

static void stand(struct server_player* p) {
	p->x = 5.6;
	p->y = Y + (double)PLAYER_EYE_HEIGHT;
	p->z = 5.5;
	p->vel_x = p->vel_y = p->vel_z = 0.0F;
	p->health = MAX_PLAYER_HEALTH;
	p->on_ground = p->has_pos = p->finished_loading = true;
}

static void power_and_complete(struct server_local* s,
	struct server_player* actor, struct server_player* foreign) {
	struct server_world* w = server_local_world_of(s, actor);
	struct server_world* other = server_local_world_of(s, foreign);
	assert(w && other && w != other);
	stand(actor);
	stand(foreign);
	server_local_refresh_player_membership(s);
	const double foreign_x = foreign->x, foreign_y = foreign->y,
		foreign_z = foreign->z;
	const uint32_t foreign_seq = foreign->correction_seq;
	const uint32_t actor_seq = actor->correction_seq;
	/* Snapshot all fixture cells in the other world. */
	struct block_data before[7][5][5];
	for(int x = 3; x <= 9; x++)
		for(int z = 3; z <= 7; z++)
			for(int y = Y - 1; y <= Y + 3; y++)
				before[x - 3][z - 3][y - Y + 1] = cell(other, x, y, z);
	inventory_set_slot(&actor->inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = BLOCK_REDSTONE_TORCH_LIT, .count = 2});
	struct server_rpc hotbar = {.type = SRPC_HOTBAR_SLOT,
		.payload.hotbar_slot.slot = 0};
	struct server_rpc place = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = 4, .y = Y - 1, .z = 4,
			.side = SIDE_TOP}};
	if(actor == &s->player) {
		host_send(s, &hotbar);
		host_send(s, &place);
	} else {
		assert(guest_send(s, &hotbar));
		assert(guest_send(s, &place));
	}
	check(cell(w, 4, Y, 4).type == BLOCK_REDSTONE_TORCH_LIT,
		"actor's place RPC powers only its own world");
	check(cell(w, 6, Y, 5).type == BLOCK_PISTON_MOVING,
		"real piston action creates its owned moving carrier");
	/* Paused message processing above does not advance tile entities. Run the
	 * production update, not a direct call to piston_moving_te_tick. */
	s->paused = false;
	for(int visit = 0; visit < 3; visit++) {
		tick(s);
		struct piston_moving_state state = {0};
		struct server_world* prev = block_world_enter(s, w);
		bool moving = piston_moving_state_at(s, 6, Y, 5, &state);
		block_world_leave(s, prev);
		if(visit < 2)
			check(moving && state.current_progress == (visit + 1) * 0.5F,
				"owned carrier advances exactly one Beta step per update");
		else
			check(!moving && cell(w, 6, Y, 5).type == BLOCK_WOOL
				&& cell(w, 6, Y, 5).metadata == 7,
				"third production tile visit completes stored block + metadata");
		if(visit == 0 && actor == &s->player)
			check(fabs(actor->x - 6.1625) < 1.0e-9,
				"first Beta sweep pushes the local body by .5 + 1/16");
		check(foreign->x == foreign_x && foreign->y == foreign_y
			&& foreign->z == foreign_z && foreign->vel_x == 0.0F
			&& foreign->vel_y == 0.0F && foreign->vel_z == 0.0F
			&& foreign->health == MAX_PLAYER_HEALTH
			&& foreign->correction_seq == foreign_seq,
			"same-coordinate foreign player receives no push or correction");
	}
	s->paused = true;
	if(actor == &s->player)
		check(actor->x > 5.6 && actor->correction_seq != actor_seq,
			"positive control: piston pushes the host in its own dimension");
	bool same = true;
	/* Light can settle during either world's regular drain; the action's
	 * isolation contract here is exact block IDs/metadata, not frozen lighting. */
	for(int x = 3; x <= 9; x++)
		for(int z = 3; z <= 7; z++)
			for(int y = Y - 1; y <= Y + 3; y++) {
				struct block_data b = cell(other, x, y, z);
				struct block_data old = before[x - 3][z - 3][y - Y + 1];
				same &= b.type == old.type && b.metadata == old.metadata;
			}
	check(same, "all foreign fixture block IDs/metadata remain byte-identical");
	printf("owner %s world %u: carrier=%u meta=%u x=%.9f foreign=%.9f\n",
		actor == &s->player ? "host" : "guest", w->world_index,
		cell(w, 6, Y, 5).type, cell(w, 6, Y, 5).metadata,
		actor->x, foreign->x);
}

int main(int argc, char** argv) {
	harness_init();
	const char* which = argc > 1 ? argv[1] : "";
	bool host_nether = !strcmp(which, "host_nether");
	if(!host_nether && strcmp(which, "guest_nether"))
		return 2;
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	s->render_demo_mob = -1;
	struct server_world* nether = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(nether, path, WORLD_DIM_NETHER, 12345, false);
	string_clear(path);
	nether->local = nether->session = s;
	nether->world_index = 1;
	s->world_slot_state[1] = SERVER_WORLD_SLOT_LIVE;
	struct server_player* guest = s->guest_actor;
	s->player.world_slot = host_nether ? 1 : 0;
	s->player.dimension = host_nether ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
	guest->world_slot = host_nether ? 0 : 1;
	guest->dimension = host_nether ? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER;
	/* Silent setup still publishes to the bounded client FIFO. */
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	prepare_piston(&s->worlds[0]);
	prepare_piston(nether);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
	power_and_complete(s, &s->player, guest);
	power_and_complete(s, guest, &s->player);
	s->player.world_slot = guest->world_slot = 0;
	s->player.dimension = guest->dimension = WORLD_DIM_OVERWORLD;
	s->world_slot_state[1] = SERVER_WORLD_SLOT_EMPTY;
	server_world_discard(nether);
	piston_moving_store_destroy(&s->piston_moving);
	teardown(s);
	return harness_result(TEST_NAME, which);
}
