#include <assert.h>
#include <string.h>

#include "block/blocks.h"
#include "network/server_world.h"

struct block* blocks[256];

static void test_tick(struct server_local* s, struct block_info* info) {
	(void)s;
	(void)info;
}

static void drain_tick(struct server_world* world, uint32_t* fires) {
	size_t entries = world->scheduled_tick_count;
	for(size_t i = 0; i < entries; i++) {
		struct scheduled_tick tick;
		assert(server_world_scheduled_tick_pop(world, &tick));
		if(tick.fire_at_tick <= world->current_tick) {
			(*fires)++;
			assert(tick.block_type == BLOCK_REDSTONE_TORCH_LIT);
			assert(server_world_schedule_tick_raw(world, tick.x, tick.y, tick.z,
											  tick.block_type, 1));
		} else {
			server_world_scheduled_tick_requeue(world, tick);
		}
	}
}

// ---- ci-1101/48: containment-set robustness + drain order equivalence -----------------

static const struct scheduled_tick* queue_at(const struct server_world* w,
											 size_t i) {
	return &w->scheduled_ticks[(w->scheduled_tick_head + i)
							   % w->scheduled_tick_capacity];
}

// (1) Far / negative coordinates up to the 30,000,000 world border: the Java
// hashCode wraps (x*2^25 keeps only x mod 128), so many of these keys share a hash
// value; identity must still be the full (x,y,z,id) tuple, and no signed overflow
// (the sanitizer build aborted on the old int32 math).
static void test_far_coordinates(void) {
	static const w_coord_t far[] = {
		30000000, -30000000, 29999999, -29999999, 30000000 - 128,
		-30000000 + 128, 0, -1, 64, -65, 1 << 24, -(1 << 24),
	};
	const size_t n = sizeof(far) / sizeof(far[0]);
	struct server_world world;
	memset(&world, 0, sizeof(world));
	server_world_scheduled_ticks_create(&world, 4096);
	size_t expect = 0;
	for(size_t i = 0; i < n; i++)
		for(size_t j = 0; j < n; j++)
			for(int id = 0; id < 2; id++) {
				uint8_t type = id ? BLOCK_WATER_FLOW : BLOCK_REDSTONE_TORCH_LIT;
				assert(server_world_schedule_tick_raw(&world, far[i], 64, far[j],
													  type, 5));
				expect++;
				assert(world.scheduled_tick_count == expect);
			}
	// Every key once more: the existing entry is kept, nothing is appended.
	for(size_t i = 0; i < n; i++)
		for(size_t j = 0; j < n; j++) {
			assert(server_world_schedule_tick_raw(&world, far[i], 64, far[j],
												  BLOCK_REDSTONE_TORCH_LIT, 1));
			assert(server_world_schedule_tick_raw(&world, far[i], 64, far[j],
												  BLOCK_WATER_FLOW, 1));
		}
	assert(world.scheduled_tick_count == expect);
	// Same x mod 128 / z mod 128 but a different y is a different key.
	assert(server_world_schedule_tick_raw(&world, 30000000, 65, -30000000,
										  BLOCK_WATER_FLOW, 1));
	assert(world.scheduled_tick_count == ++expect);
	// Popping removes exactly that key; it can then be scheduled afresh.
	struct scheduled_tick t;
	assert(server_world_scheduled_tick_pop(&world, &t));
	assert(t.x == far[0] && t.z == far[0]
		   && t.block_type == BLOCK_REDSTONE_TORCH_LIT);
	assert(server_world_schedule_tick_raw(&world, far[0], 64, far[0],
										  BLOCK_WATER_FLOW, 1));
	assert(world.scheduled_tick_count == expect - 1);
	assert(server_world_schedule_tick_raw(&world, far[0], 64, far[0],
										  BLOCK_REDSTONE_TORCH_LIT, 1));
	assert(world.scheduled_tick_count == expect);
	assert(world.scheduled_tick_drops == 0);
	server_world_scheduled_ticks_destroy(&world);
}

// (2) A ring reset that bypasses server_world_scheduled_ticks_resync must not make a
// stale containment slot report "already pending" (dropping a fresh schedule), and
// pop must still remove the right slot.
static void test_stale_reset_without_resync(void) {
	struct server_world world;
	memset(&world, 0, sizeof(world));
	server_world_scheduled_ticks_create(&world, 64);
	for(int i = 0; i < 10; i++)
		assert(server_world_schedule_tick_raw(&world, i, 70, -i,
											  BLOCK_REDSTONE_TORCH_LIT, 2));
	// Reset WITHOUT resync (what test fixtures did).
	world.scheduled_tick_count = 0;
	world.scheduled_tick_head = 0;
	// The same key must now be accepted (it is no longer pending).
	assert(server_world_schedule_tick_raw(&world, 3, 70, -3,
										  BLOCK_REDSTONE_TORCH_LIT, 2));
	assert(world.scheduled_tick_count == 1);
	// Physical slot 0 now holds key 3; the stale slot for key 0 names index 0 but
	// the entry there is a different key, so key 0 is not pending either.
	assert(server_world_schedule_tick_raw(&world, 0, 70, 0,
										  BLOCK_REDSTONE_TORCH_LIT, 2));
	assert(world.scheduled_tick_count == 2);
	// A real duplicate is still caught (keep-first).
	assert(server_world_schedule_tick_raw(&world, 3, 70, -3,
										  BLOCK_REDSTONE_TORCH_LIT, 9));
	assert(world.scheduled_tick_count == 2);
	assert(queue_at(&world, 0)->fire_at_tick == 2);
	// Index 9 is outside the window: its stale key 9 is not pending.
	assert(server_world_schedule_tick_raw(&world, 9, 70, -9,
										  BLOCK_REDSTONE_TORCH_LIT, 2));
	assert(world.scheduled_tick_count == 3);
	// Pops remove the right entries; afterwards every key is schedulable again.
	struct scheduled_tick t;
	assert(server_world_scheduled_tick_pop(&world, &t) && t.x == 3);
	assert(server_world_scheduled_tick_pop(&world, &t) && t.x == 0);
	assert(server_world_scheduled_tick_pop(&world, &t) && t.x == 9);
	assert(world.scheduled_tick_count == 0);
	for(int i = 0; i < 10; i++)
		assert(server_world_schedule_tick_raw(&world, i, 70, -i,
											  BLOCK_REDSTONE_TORCH_LIT, 2));
	assert(world.scheduled_tick_count == 10);
	// Head moved to a different offset without resync: the window check holds.
	world.scheduled_tick_head = 30;
	world.scheduled_tick_count = 0;
	assert(server_world_schedule_tick_raw(&world, 5, 70, -5,
										  BLOCK_REDSTONE_TORCH_LIT, 2));
	assert(world.scheduled_tick_count == 1);
	assert(world.scheduled_tick_drops == 0);
	server_world_scheduled_ticks_destroy(&world);
}

// (3) Drain order equivalence against a plain model of Beta's TickUpdates over the
// ring: originals not fired this drain (future, fluid sub-cap) keep their place
// ahead of the unprocessed suffix and of children; children are keep-first deduped
// against everything pending; occupancy bounds drops; at most 1000 fires with a
// budget. This is the sequence the previous pop/re-append + stable-permutation
// drain produced; the in-place drain must match it entry for entry.
#define EQ_MAX 8192
struct eq_model {
	struct scheduled_tick q[EQ_MAX];
	size_t n;
	uint32_t drops;
	size_t cap;
};
static struct eq_model g_model;
static uint32_t g_eq_tick;
static struct scheduled_tick g_mk[EQ_MAX], g_mq[EQ_MAX];
static struct scheduled_tick g_model_fired[EQ_MAX], g_world_fired[EQ_MAX];
static size_t g_model_fired_n, g_world_fired_n;

static uint32_t eq_mix(uint32_t h) {
	h ^= h >> 16;
	h *= 0x45d9f3bu;
	h ^= h >> 16;
	return h;
}

// Deterministic children of a fire: a function of the fired entry and the tick.
static size_t eq_children(struct scheduled_tick t, uint32_t now,
						  struct scheduled_tick* out) {
	uint32_t h = eq_mix((uint32_t)t.x * 73856093u ^ (uint32_t)t.z * 19349663u
						^ (uint32_t)t.y * 83492791u ^ t.block_type
						^ now * 2654435761u);
	size_t n = h % 4;
	for(size_t i = 0; i < n; i++) {
		h = eq_mix(h + 0x9e3779b9u);
		out[i] = (struct scheduled_tick) {
			.x = (w_coord_t)((h >> 4) % 24) - 12
				+ ((h & 1) ? 30000000 : -30000000),
			.y = (w_coord_t)((h >> 9) % 3) + 60,
			.z = (w_coord_t)((h >> 11) % 24) - 12,
			.fire_at_tick = now + (h >> 16) % 7,
			.block_type = (h >> 20) % 3 == 0 ? BLOCK_REDSTONE_TORCH_LIT
											 : BLOCK_WATER_FLOW,
		};
	}
	return n;
}

static bool eq_has(const struct scheduled_tick* q, size_t n,
				   struct scheduled_tick t) {
	for(size_t i = 0; i < n; i++)
		if(q[i].x == t.x && q[i].y == t.y && q[i].z == t.z
		   && q[i].block_type == t.block_type)
			return true;
	return false;
}

static void eq_model_schedule(struct eq_model* m, const struct scheduled_tick* kept,
							  size_t nk, struct scheduled_tick* q, size_t front,
							  size_t* nq, struct scheduled_tick t) {
	if(eq_has(kept, nk, t) || eq_has(q + front, *nq - front, t))
		return;
	if(nk + (*nq - front) >= m->cap) {
		m->drops++;
		return;
	}
	t.queue_flags = 0;
	q[(*nq)++] = t;
}

static void eq_model_drain(struct eq_model* m, uint32_t now, uint32_t budget) {
	size_t nk = 0, front = 0, nq = m->n;
	const size_t entries = m->n;
	memcpy(g_mq, m->q, m->n * sizeof(m->q[0]));
	uint32_t fluid_fired = 0, total_fired = 0;
	g_model_fired_n = 0;
	for(size_t k = 0; k < entries; k++) {
		if(budget && total_fired >= SERVER_WORLD_SCHEDULED_TICK_BETA_CAP)
			break;
		struct scheduled_tick t = g_mq[front++];
		bool fluid = t.block_type == BLOCK_WATER_FLOW;
		if(t.fire_at_tick > now || (budget && fluid && fluid_fired >= budget)) {
			g_mk[nk++] = t;
			continue;
		}
		total_fired++;
		if(fluid)
			fluid_fired++;
		g_model_fired[g_model_fired_n++] = t;
		struct scheduled_tick ch[4];
		size_t n = eq_children(t, now, ch);
		for(size_t c = 0; c < n; c++)
			eq_model_schedule(m, g_mk, nk, g_mq, front, &nq, ch[c]);
	}
	memcpy(m->q, g_mk, nk * sizeof(g_mk[0]));
	memcpy(m->q + nk, g_mq + front, (nq - front) * sizeof(g_mq[0]));
	m->n = nk + nq - front;
}

static void eq_fire(struct server_world* w, struct server_local* s,
					struct scheduled_tick t) {
	(void)s;
	g_world_fired[g_world_fired_n++] = t;
	struct scheduled_tick ch[4];
	size_t n = eq_children(t, g_eq_tick, ch);
	for(size_t c = 0; c < n; c++)
		(void)server_world_schedule_tick_at_raw(w, ch[c]);
}

static void eq_compare(const struct server_world* w, const struct eq_model* m) {
	assert(w->scheduled_tick_count == m->n);
	assert(w->scheduled_tick_drops == m->drops);
	assert(w->scheduled_tick_kept == 0 && w->scheduled_tick_kept_span == 0);
	for(size_t i = 0; i < m->n; i++) {
		const struct scheduled_tick* a = queue_at(w, i);
		const struct scheduled_tick* b = &m->q[i];
		assert(a->x == b->x && a->y == b->y && a->z == b->z
			   && a->block_type == b->block_type
			   && a->fire_at_tick == b->fire_at_tick && a->queue_flags == 0);
	}
	assert(g_world_fired_n == g_model_fired_n);
	for(size_t i = 0; i < g_world_fired_n; i++)
		assert(g_world_fired[i].x == g_model_fired[i].x
			   && g_world_fired[i].y == g_model_fired[i].y
			   && g_world_fired[i].z == g_model_fired[i].z
			   && g_world_fired[i].block_type == g_model_fired[i].block_type
			   && g_world_fired[i].fire_at_tick == g_model_fired[i].fire_at_tick);
}

static uint64_t g_eq_drops_seen, g_eq_fires_seen;
static void test_drain_equivalence(size_t cap, uint32_t budget, size_t seeds,
								   uint32_t ticks) {
	static struct server_world world;
	memset(&world, 0, sizeof(world));
	server_world_scheduled_ticks_create(&world, cap);
	memset(&g_model, 0, sizeof(g_model));
	g_model.cap = cap;
	uint32_t h = (uint32_t)cap * 7919u + budget;
	for(size_t i = 0; i < seeds; i++) {
		h = eq_mix(h + 1);
		struct scheduled_tick t = {
			.x = (w_coord_t)(h % 40) - 20,
			.y = 64,
			.z = (w_coord_t)((h >> 8) % 40) - 20,
			.fire_at_tick = (h >> 16) % 5,
			.block_type = (h >> 24) % 4 == 0 ? BLOCK_REDSTONE_TORCH_LIT
											 : BLOCK_WATER_FLOW,
		};
		(void)server_world_schedule_tick_at_raw(&world, t);
		size_t nq = g_model.n;
		eq_model_schedule(&g_model, NULL, 0, g_model.q, 0, &nq, t);
		g_model.n = nq;
	}
	g_world_fired_n = g_model_fired_n = 0;
	eq_compare(&world, &g_model);
	for(uint32_t tick = 1; tick <= ticks; tick++) {
		world.current_tick = tick;
		g_eq_tick = tick;
		g_world_fired_n = 0;
		server_world_drain_scheduled(&world, NULL, budget, eq_fire, NULL);
		eq_model_drain(&g_model, tick, budget);
		eq_compare(&world, &g_model);
		g_eq_fires_seen += g_world_fired_n;
		// The containment set agrees with the ring: every pending key is a duplicate.
		for(size_t i = 0; i < world.scheduled_tick_count; i += 7) {
			struct scheduled_tick dup = *queue_at(&world, i);
			dup.fire_at_tick = tick + 100;
			size_t before = world.scheduled_tick_count;
			assert(server_world_schedule_tick_at_raw(&world, dup));
			assert(world.scheduled_tick_count == before);
		}
	}
	g_eq_drops_seen += world.scheduled_tick_drops;
	server_world_scheduled_ticks_destroy(&world);
}

int main(void) {
	struct block torch;
	memset(&torch, 0, sizeof(torch));
	torch.onWorldTick = test_tick;
	torch.tick_delay = 1;
	blocks[BLOCK_REDSTONE_TORCH_LIT] = &torch;

	struct server_world world;
	memset(&world, 0, sizeof(world));
	server_world_scheduled_ticks_create(
		&world, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);

	for(int i = 0; i < 10; i++) {
		assert(server_world_schedule_block_tick_raw(
			&world, i, 32, 0, BLOCK_REDSTONE_TORCH_LIT));
	}
	assert(world.scheduled_tick_count == 10);

	uint32_t fires = 0;
	for(uint32_t tick = 1; tick <= 100; tick++) {
		world.current_tick = tick;
		drain_tick(&world, &fires);
		assert(world.scheduled_tick_count == 10);
	}
	assert(fires == 1000);
	assert(world.scheduled_tick_drops == 0);

	for(int i = 0; i < 1000; i++)
		assert(!server_world_schedule_block_tick_raw(
			&world, i, 10, 1, BLOCK_STONE));
	assert(world.scheduled_tick_count == 10);

	server_world_scheduled_ticks_destroy(&world);

	test_far_coordinates();
	test_stale_reset_without_resync();
	// Small rings: wrap-around, drops, and the mid-drain compaction of fired holes.
	test_drain_equivalence(64, 3, 60, 300);
	test_drain_equivalence(64, 0, 60, 300);
	test_drain_equivalence(37, 2, 37, 300);
	// Production ring: deep backlog, the Wii fluid sub-cap and Beta's 1000 cap.
	test_drain_equivalence(SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY, 128,
						   4000, 120);
	test_drain_equivalence(SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY, 0,
						   3000, 60);
	// The workloads must really exercise drops (full ring) and fires.
	assert(g_eq_drops_seen > 0 && g_eq_fires_seen > 1000);
	return 0;
}
