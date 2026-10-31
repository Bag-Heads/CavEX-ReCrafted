/* integ-1011/1b: a real Join/offer owns a physical body before admission.
 * Beta TileEntityPiston.func_31010_a (RetroMCP b1.7.3 TileEntityPiston.java:
 * 74-86) moves every queried entity; admission is our lifecycle boundary.
 * No Beta piston timing, geometry or golden is changed here. */
#include "support/owner_swap_harness.h"
#include "block/redstone_piston_server.h"
#include "entity/entity_monster.h"
#include "support/mob_motion_fixture.h"

static size_t compare_query(struct server_local* s, struct server_world* w,
	int cx0, int cz0, int cx1, int cz1, int cy0, int cy1) {
	struct server_entity_query_ref expected[8];
	size_t n, actual;
	assert(server_local_prepare_entity_query_snapshot_bounded_w(
		s, w, cx0, cz0, cx1, cz1, cy0, cy1));
	const struct server_entity_query_ref* refs
		= server_local_entity_query_snapshot(s, &n);
	assert(n <= 8);
	/* Compare all semantic bytes, with padding explicitly zeroed. */
	memset(expected, 0, sizeof expected);
	for(size_t i = 0; i < n; i++) {
		expected[i].entity = refs[i].entity;
		expected[i].id = refs[i].id;
		expected[i].chunk_x = refs[i].chunk_x;
		expected[i].chunk_y = refs[i].chunk_y;
		expected[i].chunk_z = refs[i].chunk_z;
		expected[i].list_order = refs[i].list_order;
		expected[i].player = refs[i].player;
		expected[i].actor = refs[i].actor;
	}
	assert(server_local_entity_query_local_w(
		s, w, cx0, cz0, cx1, cz1, cy0, cy1));
	refs = server_local_entity_query_snapshot(s, &actual);
	assert(actual == n);
	for(size_t i = 0; i < n; i++) {
		struct server_entity_query_ref got;
		memset(&got, 0, sizeof got);
		got.entity = refs[i].entity;
		got.id = refs[i].id;
		got.chunk_x = refs[i].chunk_x;
		got.chunk_y = refs[i].chunk_y;
		got.chunk_z = refs[i].chunk_z;
		got.list_order = refs[i].list_order;
		got.player = refs[i].player;
		got.actor = refs[i].actor;
		assert(!memcmp(&expected[i], &got, sizeof got));
	}
	return n;
}

static void put(struct server_world* w, int x, int y, int z,
	uint8_t id, uint8_t meta) {
	struct block_data b;
	assert(server_world_get_block(w, x, y, z, &b));
	if(b.type != id || b.metadata != meta)
		assert(server_world_set_block(w, x, y, z,
			(struct block_data) {.type = id, .metadata = meta}));
}

static void prepare_piston(struct server_world* w, int x, int z) {
	for(int bx = x - 1; bx <= x + 4; bx++)
		for(int bz = z - 1; bz <= z + 1; bz++) {
			struct server_chunk* c = dict_server_chunks_get(w->chunks,
				S_CHUNK_ID(WCOORD_CHUNK_OFFSET(bx), WCOORD_CHUNK_OFFSET(bz)));
			for(unsigned attempt = 0; attempt < 200 && !c; attempt++) {
				server_world_begin_update(w);
				(void)server_world_load_chunk(w, WCOORD_CHUNK_OFFSET(bx),
					WCOORD_CHUNK_OFFSET(bz), &c);
			}
			assert(c);
			put(w, bx, 100, bz, BLOCK_STONE, 0);
			for(int y = 101; y <= 104; y++)
				put(w, bx, y, bz, BLOCK_AIR, 0);
		}
	put(w, x, 101, z, BLOCK_PISTON_BASE, 5);
	put(w, x + 1, 101, z, BLOCK_WOOL, 4);
}

static void piston_visit(struct server_local* s, struct server_world* w,
	int x, int z) {
	clin_update_drain();
	counters_reset();
	struct server_world* prev = block_world_enter(s, w);
	put(w, x, 101, z - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
	clin_update_drain();
	counters_reset();
	piston_moving_te_tick(s);
	block_world_leave(s, prev);
	clin_update_drain();
}

static void blast(struct server_local* s, struct server_player* guest) {
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	server_world_explode_precise(&s->worlds[0],
		(double[3]) {guest->x - 0.3, guest->y, guest->z}, 0.5F);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
}

static void arrow_crossing(struct server_local* s, struct server_player* guest) {
	struct entity arrow;
	entity_arrow(90003, &arrow, true, &s->worlds[0], 999, ITEM_ARROW, 2, false);
	arrow.pos[0] = (float)guest->x - 1.0F;
	arrow.pos[1] = (float)guest->y - 0.5F;
	arrow.pos[2] = (float)guest->z;
	arrow.vel[0] = 2.0F;
	arrow.vel[1] = arrow.vel[2] = 0.0F;
	(void)arrow.tick_server(&arrow, s);
	clin_update_drain();
}

static void target_tick(struct server_local* s, bool admitted) {
	struct entity mob;
	entity_monster(90004, &mob, true, &s->worlds[0], MONSTER_ZOMBIE);
	mob.pos[0] = 4.0F; mob.pos[1] = 101.0F; mob.pos[2] = 6.5F;
	test_mob_sync(&mob);
	assert(!mob.tick_server(&mob, s)); /* false = alive, not reaped */
	if(admitted) {
		assert(mob.data.monster.aggro_time > 0);
		assert(actor_ident_equal(mob.data.monster.target_actor, guest_ident(s)));
	} else {
		assert(mob.data.monster.aggro_time == 0);
	}
	clin_update_drain();
}

int main(void) {
	(void)split_populate; /* shared harness helper is unused by this fixture */
	harness_init();
	struct server_local* s = split_fixture();
	s->paused = true;
	s->split_view_radius = 4;
	stream_to_marker(s);
	acknowledge(s);
	s->thread_started = true;
	enable_split(s);
	guest_attach(s); /* real create -> bind -> offer -> Confirm; no gameplay */
	struct server_player* guest = s->guest_actor;
	assert(guest && !s->guest_gameplay && !guest->finished_loading);
	assert(guest->added_to_chunk && "initial bind registers the offered body");
	server_local_refresh_player_membership(s);
	struct server_rpc_source old_source = guest_source(s);
	struct actor_ident old_ident = guest_ident(s);
	assert(compare_query(s, &s->worlds[0], -1, -1, 1, 1, 0, 7) == 2);
	for(size_t i = 0; i < s->entity_query_count; i++) {
		const struct server_entity_query_ref* ref = &s->entity_query_refs[i];
		assert(server_local_query_ref_body(s, ref)
			== (ref->actor ? guest : &s->player));
		assert(server_local_query_ref_actor(s, ref)
			== (ref->actor ? NULL : &s->player));
	}

	s->player.x = s->player.z = 100.5;
	server_local_refresh_player_membership(s);
	const double poses[][3] = {
		{15.99, 15.99, 15.99}, {16.01, 16.01, 16.01},
		{-0.01, 31.99, -0.01}, {0.01, 32.01, 0.01},
		{-16.01, 127.99, -16.01}, {-15.99, 128.01, -15.99},
		{15.99, -0.01, 15.99}, {16.01, 0.01, 16.01},
	};
	for(size_t i = 0; i < sizeof poses / sizeof poses[0]; i++) {
		guest->x = poses[i][0]; guest->y = poses[i][1]; guest->z = poses[i][2];
		assert(compare_query(s, &s->worlds[0], -2, -2, 2, 2, 0, 7) == 1);
		assert(guest->chunk_coord_x == (int)floor(guest->x / 16.0));
		assert(guest->chunk_coord_z == (int)floor(guest->z / 16.0));
		int cy = (int)floor(guest->y / 16.0);
		if(cy < 0) cy = 0;
		if(cy > 7) cy = 7;
		assert(guest->chunk_coord_y == cy);
		assert(compare_query(s, &s->worlds[0], guest->chunk_coord_x,
			guest->chunk_coord_z, guest->chunk_coord_x, guest->chunk_coord_z,
			cy, cy) == 1);
		uint64_t order = guest->chunk_list_order;
		for(unsigned r = 0; r < 3; r++) server_local_refresh_guest_membership(s);
		assert(guest->chunk_list_order == order);
		assert(compare_query(s, &s->worlds[0], -2, -2, 2, 2, 0, 7) == 1);
	}
	puts("PASS offered section boundaries, host outside bounds, byte-exact query/dedup");

	/* Real offered body crossing x=16 under a first piston visit. */
	prepare_piston(&s->worlds[0], 14, 5);
	guest->x = 15.6; guest->y = 101.0 + PLAYER_EYE_HEIGHT; guest->z = 5.7625;
	/* Deliberately make loading permissive: admission itself must gate damage. */
	guest->finished_loading = true;
	guest->health = MAX_PLAYER_HEALTH;
	put(&s->worlds[0], 16, 100, 6, BLOCK_SAND, 0);
	put(&s->worlds[0], 16, 101, 6, BLOCK_CACTUS, 0);
	const double host_x = s->player.x;
	const uint32_t host_seq = s->player.correction_seq;
	piston_visit(s, &s->worlds[0], 14, 5);
	printf("offered piston x=%.9f correction_seq=%u guest_msgs=%u host_msgs=%u\n",
		guest->x, guest->correction_seq, guest_corrections, host_pos_msgs);
	assert(fabs(guest->x - 16.1625) < 1e-9);
	assert(guest_corrections == 1 && host_pos_msgs == 0);
	assert(s->player.x == host_x && s->player.correction_seq == host_seq);
	assert(guest->health == MAX_PLAYER_HEALTH && !guest->hurt_resist_time);
	assert(compare_query(s, &s->worlds[0], 1, 0, 1, 0, 6, 6) == 1);
	puts("PASS real offered piston push/correction, cross-section membership, no cactus damage");
	/* Downward physical contact still settles/reset fall state; the offered
	 * body cannot pay fall damage even with the loading guard permissive. */
	prepare_piston(&s->worlds[0], 12, 12);
	put(&s->worlds[0], 12, 101, 12, BLOCK_AIR, 0);
	put(&s->worlds[0], 13, 101, 12, BLOCK_AIR, 0);
	put(&s->worlds[0], 12, 104, 12, BLOCK_PISTON_BASE, 0);
	put(&s->worlds[0], 12, 103, 12, BLOCK_WOOL, 4);
	put(&s->worlds[0], 12, 103, 11, BLOCK_STONE, 0);
	guest->x = 12.5; guest->y = 101.2 + PLAYER_EYE_HEIGHT; guest->z = 12.5;
	guest->fall_distance = 20.0F;
	put(&s->worlds[0], 12, 104, 11, BLOCK_REDSTONE_TORCH_LIT, 5);
	clin_update_drain();
	counters_reset();
	piston_moving_te_tick(s);
	clin_update_drain();
	assert(fabs(guest->y - (101.0 + PLAYER_EYE_HEIGHT)) < 1e-5);
	assert(guest->on_ground && guest->fall_distance == 0);
	assert(guest->health == MAX_PLAYER_HEALTH && !guest->hurt_resist_time);
	assert(guest_corrections == 1 && host_pos_msgs == 0);
	puts("PASS offered piston landing updates fall state without fall damage");

	/* EntityMinecart width=.98 (RetroMCP EntityMinecart.<init>): its .49
	 * half-width stops at the offered player's .3 half-width. The offered
	 * player starts beyond the moving carrier and is not directly pushed. */
	prepare_piston(&s->worlds[0], 6, 10);
	guest->x = 8.9; guest->y = 101.0 + PLAYER_EYE_HEIGHT; guest->z = 10.5;
	struct entity cart;
	entity_minecart(90001, &cart, true, &s->worlds[0]);
	cart.pos[0] = 7.6F; cart.pos[1] = 101.0F + cart.y_offset; cart.pos[2] = 10.5F;
	cart.vel[0] = cart.data.minecart.speed = 0.3F;
	cart.data.minecart.hx = 1;
	*dict_entity_safe_get(s->entities, cart.id) = &cart;
	assert(server_local_test_entity_join(s, cart.id));
	piston_visit(s, &s->worlds[0], 6, 10);
	assert(fabsf(cart.pos[0] - 8.11F) < 1e-5F);
	assert(cart.vel[0] == 0 && cart.data.minecart.speed == 0);
	assert(guest->x == 8.9 && guest_corrections == 0);
	dict_entity_erase(s->entities, cart.id);
	puts("PASS offered-body minecart clipping");

	guest->x = guest->z = 6.5;
	guest->y = 101.0 + PLAYER_EYE_HEIGHT;
	struct entity mob;
	entity_monster(90002, &mob, true, &s->worlds[0], MONSTER_ZOMBIE);
	mob.pos[0] = 6.3F; mob.pos[1] = 101.0F; mob.pos[2] = 6.5F;
	assert(!server_local_damage_candidate(s, 1));
	assert(!server_local_actor_resolve(s, old_ident)); /* mob target gate */
	target_tick(s, false);
	guest->vel_x = guest->vel_z = guest->motion_y = 0;
	arrow_crossing(s, guest);
	assert(guest->health == MAX_PLAYER_HEALTH && !guest->hurt_resist_time);
	entity_living_collide_with_nearby(&mob, s);
	assert(guest->vel_x == 0 && guest->vel_z == 0 && guest->motion_y == 0);
	blast(s, guest);
	assert(guest->health == MAX_PLAYER_HEALTH && !guest->hurt_resist_time);
	assert(guest->vel_x == 0 && guest->vel_z == 0 && guest->motion_y == 0);
	puts("PASS offered excluded from damage/target resolution, ordinary impulses and explosion");

	/* The same membership survives REAL admission without an append/reorder. */
	guest->finished_loading = false;
	guest_body_home(s);
	s->player.x = s->player.z = 0.5;
	guest_body_home(s);
	server_local_refresh_player_membership(s);
	server_local_refresh_guest_membership(s);
	uint64_t offer_order = guest->chunk_list_order;
	struct server_entity_query_ref before[2];
	assert(compare_query(s, &s->worlds[0], 0, 0, 0, 0, 0, 7) == 2);
	memcpy(before, s->entity_query_refs, sizeof before);
	assert(client_session_request_local_gameplay(&gstate.players[1]));
	pump_handshake(s);
	assert(s->guest_gameplay && guest->finished_loading);
	assert(guest->chunk_list_order == offer_order);
	assert(compare_query(s, &s->worlds[0], 0, 0, 0, 0, 0, 7) == 2);
	for(unsigned i = 0; i < 2; i++) {
		assert(before[i].actor == s->entity_query_refs[i].actor);
		assert(before[i].list_order == s->entity_query_refs[i].list_order);
	}
	assert(server_local_actor_resolve(s, old_ident) == guest);
	assert(server_local_damage_candidate(s, 1) == guest);
	/* Positive control proves the production impulse pass was engaged. */
	s->player.x = s->player.z = 100.5;
	server_local_refresh_player_membership(s);
	guest->x = guest->z = 6.5; guest->y = 101.0 + PLAYER_EYE_HEIGHT;
	target_tick(s, true);
	arrow_crossing(s, guest);
	assert(guest->health < MAX_PLAYER_HEALTH);
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->vel_x = guest->vel_z = guest->motion_y = 0;
	entity_living_collide_with_nearby(&mob, s);
	assert(guest->vel_x != 0);
	guest->vel_x = guest->vel_z = guest->motion_y = 0;
	blast(s, guest);
	assert(guest->health < MAX_PLAYER_HEALTH && guest->vel_x != 0);
	puts("PASS offer->admit preserves ordering; admitted impulse/explosion positive controls");

	guest_release(s);
	size_t released_count = compare_query(s, &s->worlds[0], -2, -2, 2, 2, 0, 7);
	/* The first carrier's neighbor update broke the adjacent cactus, leaving
	 * its legitimate item entity. Neither player is in these query bounds. */
	assert(released_count == 1 && !s->entity_query_refs[0].player);
	struct entity* cactus_drop = s->entity_query_refs[0].entity;
	assert(cactus_drop->type == ENTITY_ITEM
		&& cactus_drop->data.item.item.id == BLOCK_CACTUS);
	dict_entity_erase(s->entities, cactus_drop->id);
	free(cactus_drop);
	assert(compare_query(s, &s->worlds[0], -2, -2, 2, 2, 0, 7) == 0);
	assert(!server_local_guest_correct_position_for(s, old_source.generation,
		PLAYER_CORRECTION_POSITION_ONLY));
	s->player.x = s->player.z = 0.5;
	server_local_refresh_player_membership(s);
	guest_attach(s);
	guest = s->guest_actor;
	assert(!s->guest_gameplay && guest->added_to_chunk);
	assert(s->guest_generation != old_source.generation);
	assert(!server_local_actor_resolve(s, old_ident));
	assert(!server_local_guest_correct_position_for(s, old_source.generation,
		PLAYER_CORRECTION_POSITION_ONLY));
	assert(!server_local_guest_leave(s, old_source));
	assert(compare_query(s, &s->worlds[0], 0, 0, 0, 0, 0, 7) == 2);
	puts("PASS release/rebind retires old membership and rejects stale generations");

	struct server_world* other = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(other, path, WORLD_DIM_NETHER, 12345, true);
	string_clear(path);
	other->local = other->session = s;
	other->world_index = 1;
	s->world_slot_state[1] = SERVER_WORLD_SLOT_LIVE;
	guest->world_slot = 1;
	guest->dimension = WORLD_DIM_NETHER;
	assert(compare_query(s, &s->worlds[0], 0, 0, 0, 0, 0, 7) == 1);
	assert(compare_query(s, other, 0, 0, 0, 0, 0, 7) == 1);
	server_local_entity_sections_reset_slot(s, 1);
	assert(!guest->added_to_chunk);
	assert(compare_query(s, other, 0, 0, 0, 0, 0, 7) == 1);
	guest->world_slot = SERVER_LOCAL_WORLD_SLOTS;
	assert(compare_query(s, other, 0, 0, 0, 0, 0, 7) == 0);
	assert(!guest->added_to_chunk);
	guest->world_slot = 1;
	assert(compare_query(s, other, 0, 0, 0, 0, 0, 7) == 1);
	prepare_piston(other, 6, 5);
	guest->x = 7.6; guest->y = 101.0 + PLAYER_EYE_HEIGHT; guest->z = 5.5;
	s->player.x = 7.6; s->player.y = guest->y; s->player.z = 5.5;
	server_local_refresh_player_membership(s);
	piston_visit(s, other, 6, 5);
	assert(fabs(guest->x - 8.1625) < 1e-9 && s->player.x == 7.6);
	assert(guest_corrections == 1 && host_pos_msgs == 0);
	puts("PASS offered membership/piston isolation in different committed worlds");
	guest_release(s);
	server_local_entity_sections_reset(s);
	server_world_discard(other);
	teardown(s);
	return harness_result("test_offered_guest_presence", "all");
}
