/*
	test_lightning_render.c -- online lightning visuals vs Beta 1.7.3.

	Golden values below come from an independent Python port of the b1.7.3
	client decompile (RetroMCP, ../mcb173-retromcp): java.util.Random's LCG
	plus RenderLightningBolt.func_27002_a (java:13-92) and
	EntityLightningBolt's constructor/onUpdate (java:10-73), with
	Minecraft.runTick's `--field_27172_i` before updateEntities
	(client/Minecraft.java:1142-1147). The flash curve is World.func_4079_a
	(World.java:1050-1060), hand-computed.
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "daytime.h"
#include "graphics/render_lightning.h"
#include "worldgen/java_random.h"
#include "entity/entity.h"
#include "game/game_state.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "world.h"

static int failures;
#define CHECK(cond, ...)                                                       \
	do {                                                                       \
		if(!(cond)) {                                                          \
			failures++;                                                        \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
			printf(__VA_ARGS__);                                               \
			printf("\n");                                                      \
		}                                                                      \
	} while(0)

static bool near(float a, float b, float eps) {
	return fabsf(a - b) <= eps;
}

struct geom_golden {
	int64_t seed;
	float v[6][3]; /* vertices 0,1,2,3,500,895 */
	double sumx, sumz;
};

static const int k_idx[6] = {0, 1, 2, 3, 500, 895};

static const struct geom_golden k_geom[] = {
	{0LL,
	 {{11.34f, 112.0f, 14.34f}, {16.33f, 128.0f, 13.33f},
	  {16.67f, 128.0f, 13.33f}, {11.66f, 112.0f, 14.34f},
	  {13.15f, 64.0f, 11.85f}, {16.8f, 48.0f, 27.8f}},
	 10048.0, 12992.0},
	{123456789LL,
	 {{-3.66f, 112.0f, -6.66f}, {-3.67f, 128.0f, -7.67f},
	  {-3.33f, 128.0f, -7.67f}, {-3.34f, 112.0f, -6.66f},
	  {-7.85f, 64.0f, -9.15f}, {4.8f, 48.0f, 5.8f}},
	 608.0, -2880.0},
	{-4962768465676381896LL,
	 {{-8.66f, 112.0f, 3.34f}, {-4.67f, 128.0f, 7.33f},
	  {-4.33f, 128.0f, 7.33f}, {-8.34f, 112.0f, 3.34f},
	  {-10.85f, 64.0f, -1.15f}, {-9.2f, 48.0f, -7.2f}},
	 -9312.0, 160.0},
};

static void test_geometry(void) {
	static float v[LIGHTNING_VERTS_PER_BOLT][3];
	CHECK(LIGHTNING_VERTS_PER_BOLT == 896, "56 strips x 4 quads x 4 verts");
	for(size_t g = 0; g < sizeof(k_geom) / sizeof(k_geom[0]); g++) {
		size_t n = lightning_bolt_geometry(k_geom[g].seed, v);
		CHECK(n == 896, "seed %lld: vertex count %zu", (long long)k_geom[g].seed, n);
		for(int i = 0; i < 6; i++)
			for(int c = 0; c < 3; c++)
				CHECK(near(v[k_idx[i]][c], k_geom[g].v[i][c], 1e-4f),
					  "seed %lld v[%d][%d] = %f want %f",
					  (long long)k_geom[g].seed, k_idx[i], c,
					  v[k_idx[i]][c], k_geom[g].v[i][c]);
		double sx = 0.0, sz = 0.0;
		bool y_ok = true;
		for(size_t k = 0; k < n; k++) {
			sx += v[k][0];
			sz += v[k][2];
			float y = v[k][1];
			/* every vertex sits on a 16-block segment boundary in [0,128] */
			if(y < 0.0f || y > 128.0f || fmodf(y, 16.0f) != 0.0f)
				y_ok = false;
		}
		CHECK(y_ok, "seed %lld: y outside the 16-step 0..128 ladder",
			  (long long)k_geom[g].seed);
		CHECK(fabs(sx - k_geom[g].sumx) < 0.05 && fabs(sz - k_geom[g].sumz) < 0.05,
			  "seed %lld: sums %f,%f want %f,%f", (long long)k_geom[g].seed, sx,
			  sz, k_geom[g].sumx, k_geom[g].sumz);
	}
	/* Same seed -> same shape (Beta re-seeds per frame from field_27029_a). */
	static float w[LIGHTNING_VERTS_PER_BOLT][3];
	lightning_bolt_geometry(99, v);
	lightning_bolt_geometry(99, w);
	CHECK(memcmp(v, w, sizeof(v)) == 0, "geometry is a pure function of seed");
	/* Tessellator.setColorRGBA_F truncation */
	CHECK(LIGHTNING_COLOR_R == (int)(0.9F * 0.5F * 255.0F)
			  && LIGHTNING_COLOR_B == (int)(1.0F * 0.5F * 255.0F)
			  && LIGHTNING_COLOR_A == (int)(0.3F * 255.0F),
		  "bolt colour bytes");
}

/* Python oracle, rng seed 42: render_seed -5025562857975149833, living 1;
 * per tick (state, flash): (1,2) (0,2) (-1,1) (-2,0) (-3,0) (-4,0) (1,2)
 * (0,2) dead(1) then flash 0. Final render seed -1782466964123969572. */
static void test_lifecycle(void) {
	struct lightning_fx fx;
	memset(&fx, 0, sizeof(fx));
	lightning_fx_spawn(&fx, 10.0, 64.0, -5.0, 42, 7, 0);
	CHECK(fx.count == 1 && fx.generation == 7, "spawned");
	CHECK(fx.bolts[0].render_seed == -5025562857975149833LL, "render seed %lld",
		  (long long)fx.bolts[0].render_seed);
	CHECK(fx.bolts[0].living_time == 1 && fx.bolts[0].state == 2,
		  "living %d state %d", fx.bolts[0].living_time, fx.bolts[0].state);
	CHECK(lightning_bolt_visible(&fx.bolts[0]), "visible before first tick");

	static const int want_state[] = {1, 0, -1, -2, -3, -4, 1, 0};
	static const int want_flash[] = {2, 2, 1, 0, 0, 0, 2, 2};
	for(int t = 0; t < 8; t++) {
		lightning_fx_tick(&fx, 7, 0);
		CHECK(fx.count == 1, "tick %d alive", t);
		CHECK(fx.bolts[0].state == want_state[t], "tick %d state %d want %d", t,
			  fx.bolts[0].state, want_state[t]);
		CHECK(fx.flash == want_flash[t], "tick %d flash %d want %d", t, fx.flash,
			  want_flash[t]);
		CHECK(lightning_bolt_visible(&fx.bolts[0]) == (want_state[t] >= 0),
			  "tick %d visibility", t);
	}
	CHECK(fx.bolts[0].render_seed == -1782466964123969572LL,
		  "restrike render seed %lld", (long long)fx.bolts[0].render_seed);
	lightning_fx_tick(&fx, 7, 0);
	CHECK(fx.count == 0 && fx.flash == 1, "dies, flash 1 (count %zu flash %d)",
		  fx.count, fx.flash);
	lightning_fx_tick(&fx, 7, 0);
	CHECK(fx.flash == 0, "flash fades to 0");

	/* generation change drops bolts + flash (fresh WorldClient) */
	lightning_fx_spawn(&fx, 0, 0, 0, 1, 7, 0);
	lightning_fx_tick(&fx, 7, 0);
	CHECK(fx.count == 1 && fx.flash == 2, "re-armed");
	lightning_fx_tick(&fx, 8, 0);
	CHECK(fx.count == 0 && fx.flash == 0 && fx.generation == 8,
		  "generation reset");

	/* bounded pool: oldest replaced and counted, never silent */
	memset(&fx, 0, sizeof(fx));
	for(int i = 0; i < LIGHTNING_MAX_BOLTS + 3; i++)
		lightning_fx_spawn(&fx, (double)i, 0, 0, i, 0, 0);
	CHECK(fx.count == LIGHTNING_MAX_BOLTS && fx.dropped == 3
			  && fx.spawned == LIGHTNING_MAX_BOLTS + 3
			  && fx.bolts[0].x == 3.0,
		  "pool overflow count %zu dropped %u", fx.count, fx.dropped);
}

static void test_flash_curve(void) {
	/* f = min(flash - partial, 1) * 0.45F */
	CHECK(lightning_flash_amount(0, 0.0f) == 0.0f, "no flash");
	CHECK(lightning_flash_amount(-1, 0.5f) == 0.0f, "negative flash");
	CHECK(near(lightning_flash_amount(2, 0.0f), 0.45f, 1e-7f), "2,0");
	CHECK(near(lightning_flash_amount(2, 0.9f), 0.45f, 1e-7f), "2,0.9 clamps");
	CHECK(near(lightning_flash_amount(1, 0.0f), 0.45f, 1e-7f), "1,0");
	CHECK(near(lightning_flash_amount(1, 0.5f), 0.225f, 1e-6f), "1,0.5");
	CHECK(near(lightning_flash_amount(1, 0.75f), 0.1125f, 1e-6f), "1,0.75");

	float rgb[3] = {0.0f, 0.0f, 0.0f};
	lightning_flash_apply(0.45f, rgb);
	CHECK(near(rgb[0], 0.36f, 1e-6f) && near(rgb[1], 0.36f, 1e-6f)
			  && near(rgb[2], 0.45f, 1e-6f),
		  "black sky -> (0.36,0.36,0.45)");
	float same[3] = {0.2f, 0.4f, 0.6f};
	lightning_flash_apply(0.0f, same);
	CHECK(same[0] == 0.2f && same[1] == 0.4f && same[2] == 0.6f, "0 = identity");

	/* daytime hook: flash 0 is byte-identical to the old colour path; flash
	 * f moves the top plane (sky*255) exactly as the Beta term. */
	for(int i = 0; i < 4; i++) {
		float t = 0.1f + 0.25f * (float)i;
		vec3 a0, a1, a2, b0, b1, b2;
		daytime_sky_colors_dimension(t, 0, a0, a1, a2);
		daytime_sky_colors_flash(t, 0, 0.0f, b0, b1, b2);
		CHECK(memcmp(a0, b0, sizeof(vec3)) == 0 && memcmp(a1, b1, sizeof(vec3)) == 0
				  && memcmp(a2, b2, sizeof(vec3)) == 0,
			  "t=%f flash 0 identical", t);
		vec3 f0, f1, f2;
		daytime_sky_colors_flash(t, 0, 0.45f, f0, f1, f2);
		float sky[3] = {a0[0] / 255.0f, a0[1] / 255.0f, a0[2] / 255.0f};
		lightning_flash_apply(0.45f, sky);
		for(int c = 0; c < 3; c++)
			CHECK(near(f0[c], sky[c] * 255.0f, 1e-3f), "t=%f top[%d] %f want %f",
				  t, c, f0[c], sky[c] * 255.0f);
		CHECK(f2[0] > a2[0], "fog (red, < 0.8) moves toward the flash colour");
		/* Nether: no sky, flash ignored */
		vec3 n0, n1, n2, m0, m1, m2;
		daytime_sky_colors_dimension(t, -1, n0, n1, n2);
		daytime_sky_colors_flash(t, -1, 0.45f, m0, m1, m2);
		CHECK(memcmp(n0, m0, sizeof(vec3)) == 0, "nether unaffected");
	}
}

/* Local bolt adopts the server's post-constructor Entity.rand verbatim: a
 * server-style replay of EntityLightningBolt.onUpdate on a copy of the same
 * java_random must match the client bolt tick for tick (state, restrike
 * render seeds, death). */
static void test_local_state_replay(void) {
	struct java_random srv;
	jrand_set_seed(&srv, 0x5EEDLL);
	int64_t seed0 = jrand_next_long(&srv);
	int32_t living = jrand_next_int_bound(&srv, 3) + 1;
	for(int i = 0; i < 12; i++) /* the constructor's fire draws */
		(void)jrand_next_int_bound(&srv, 3);
	struct lightning_fx fx;
	memset(&fx, 0, sizeof(fx));
	lightning_fx_spawn_state(&fx, 4, 70, -9, living, seed0, srv.seed, 0, 0);
	CHECK(fx.count == 1 && fx.bolts[0].render_seed == seed0
			  && fx.bolts[0].living_time == living && fx.bolts[0].state == 2,
		  "local spawn state adopted");
	int32_t state = 2, lt = living;
	int64_t rs = seed0;
	bool alive = true;
	for(int t = 0; t < 80 && alive; t++) {
		if(state == 2) {
			(void)jrand_next_float(&srv);
			(void)jrand_next_float(&srv);
		}
		state--;
		if(state < 0) {
			if(lt == 0)
				alive = false;
			else if(state < -jrand_next_int_bound(&srv, 10)) {
				lt--;
				state = 1;
				rs = jrand_next_long(&srv);
			}
		}
		lightning_fx_tick(&fx, 0, 0);
		if(!alive) {
			CHECK(fx.count == 0, "tick %d: client bolt died with the server's", t);
			break;
		}
		CHECK(fx.count == 1 && fx.bolts[0].state == state
				  && fx.bolts[0].render_seed == rs,
			  "tick %d: client state %d seed %lld vs server %d %lld", t,
			  fx.count ? fx.bolts[0].state : -99,
			  fx.count ? (long long)fx.bolts[0].render_seed : 0LL, state,
			  (long long)rs);
	}
	CHECK(!alive, "replay reached death");
}

/* CRPC_LIGHTNING_BOLT through the real transport: 40 sends without a
 * drain overflow the 16-token window, so the rest are deferred into the
 * pending ring and packed as `other` records; every record reaches the
 * consumer in order with its bytes intact and spawns a client bolt. */
#define RT_N 40
static struct client_rpc s_seen[RT_N];
static unsigned s_seen_n;
static void consumed(const struct client_rpc* c, void* unused) {
	(void)unused;
	CHECK(c->type != CRPC_SET_BLOCK_PACK, "a pack reached the consumer");
	if(c->type == CRPC_LIGHTNING_BOLT && s_seen_n < RT_N)
		s_seen[s_seen_n++] = *c;
}
static void drain_all(void) {
	for(int guard = 0; guard < 1000; guard++) {
		clin_update_drain();
		const bool flushed = clin_rpc_flush_pending();
		uint32_t pending = 0;
		clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
		if(flushed && pending == 0) {
			clin_update_drain();
			return;
		}
	}
	CHECK(false, "ring never drained");
}
static void test_rpc_round_trip(void) {
	CHECK(clin_init() && svin_init(), "clin/svin init");
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc) {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD, .local_entity = 0},
	});
	clin_test_set_consume_observer(consumed, NULL);
	const uint32_t spawned0 = lightning_client_state()->spawned;
	uint32_t packs0 = 0, rec0 = 0;
	clin_rpc_pack_stats(&packs0, &rec0);
	struct client_rpc sent[RT_N];
	for(int i = 0; i < RT_N; i++) {
		const uint64_t seed = 0x0123456789ABCDEFull * (uint64_t)(i + 1);
		const uint64_t st = (0xA5A5A5A5A5ull + (uint64_t)i * 977u) & ((1ULL << 48) - 1);
		sent[i] = (struct client_rpc) {
			.type = CRPC_LIGHTNING_BOLT,
			.payload.lightning_bolt = {
				.x = 100 + i, .y = 64, .z = -200 - i,
				.living_time = 1 + i % 3,
				.render_seed_hi = (uint32_t)(seed >> 32),
				.render_seed_lo = (uint32_t)seed,
				.rng_state_hi = (uint32_t)(st >> 32),
				.rng_state_lo = (uint32_t)st,
			},
		};
		struct client_rpc c = sent[i];
		clin_rpc_send(&c);
	}
	uint32_t packs1 = 0, rec1 = 0;
	clin_rpc_pack_stats(&packs1, &rec1);
	CHECK(packs1 > packs0 && rec1 > rec0,
		  "bolts packed as other records (packs +%u, records +%u)",
		  packs1 - packs0, rec1 - rec0);
	drain_all();
	clin_test_set_consume_observer(NULL, NULL);
	CHECK(s_seen_n == RT_N, "consumed %u of %d bolt RPCs", s_seen_n, RT_N);
	for(unsigned i = 0; i < s_seen_n && i < RT_N; i++)
		CHECK(memcmp(&s_seen[i].payload.lightning_bolt,
					 &sent[i].payload.lightning_bolt,
					 sizeof(sent[i].payload.lightning_bolt)) == 0,
			  "bolt RPC %u bytes survive the pack", i);

	/* the local bolt render appears: client bolts spawned from the RPCs */
	const struct lightning_fx* fx = lightning_client_state();
	CHECK(fx->spawned - spawned0 == RT_N, "client spawned %u bolts",
		  fx->spawned - spawned0);
	CHECK(fx->count == LIGHTNING_MAX_BOLTS, "pool holds the newest %d",
		  LIGHTNING_MAX_BOLTS);
	if(fx->count) {
		const struct lightning_bolt* last = &fx->bolts[fx->count - 1];
		CHECK(last->x == 100 + RT_N - 1 && last->z == -200 - (RT_N - 1)
				  && lightning_bolt_visible(last)
				  && last->render_seed
					  == (int64_t)(0x0123456789ABCDEFull * (uint64_t)RT_N),
			  "newest local bolt carries the sent state");
	}
	lightning_client_tick(clin_remote_world_generation(), 0);
	CHECK(near(lightning_client_flash(0.0f, 0), 0.45f, 1e-6f),
		  "local bolt flashes the sky");
}

/* Review notes 1+2: a bolt belongs to the dimension it struck in -- drawn
 * and flashed only there -- and a primary-player dimension change drops
 * every bolt (Beta builds a fresh World on a portal trip). */
static void test_dimension_scope(void) {
	struct lightning_fx fx;
	memset(&fx, 0, sizeof(fx));
	lightning_fx_spawn(&fx, 0, 64, 0, 5, 0, 0); /* overworld bolt */
	lightning_fx_tick(&fx, 0, 0);
	CHECK(fx.count == 1, "overworld bolt alive");
	CHECK(lightning_bolt_drawn_in(&fx.bolts[0], 0), "drawn in an overworld view");
	CHECK(!lightning_bolt_drawn_in(&fx.bolts[0], -1), "not drawn in a Nether view");
	CHECK(near(lightning_fx_flash(&fx, 0.0f, 0), 0.45f, 1e-6f), "overworld flashes");
	CHECK(lightning_fx_flash(&fx, 0.0f, -1) == 0.0f, "Nether pane does not flash");

	/* a Nether-struck bolt (split pane in the Nether) owns its own flash */
	lightning_fx_spawn_state(&fx, 3, 40, 3, 1, 77, 12345, 0, -1);
	lightning_fx_tick(&fx, 0, 0); /* primary player still in the overworld */
	CHECK(fx.count == 2, "both bolts kept (count %zu)", fx.count);
	CHECK(lightning_bolt_drawn_in(&fx.bolts[1], -1)
			  && !lightning_bolt_drawn_in(&fx.bolts[1], 0),
		  "Nether bolt only in Nether views");

	/* portal trip: the primary player's dimension changes -> all dropped */
	lightning_fx_tick(&fx, 0, -1);
	CHECK(fx.count == 0 && fx.flash == 0, "dimension change clears bolts + flash");
	CHECK(lightning_fx_flash(&fx, 0.0f, -1) == 0.0f
			  && lightning_fx_flash(&fx, 0.0f, 0) == 0.0f,
		  "no flash survives the trip");
	/* a steady dimension never clears */
	lightning_fx_spawn(&fx, 0, 64, 0, 6, 0, -1);
	lightning_fx_tick(&fx, 0, -1);
	CHECK(fx.count == 1, "same-dimension tick keeps the bolt");
}

int main(void) {
	test_dimension_scope();
	test_geometry();
	test_lifecycle();
	test_flash_curve();
	test_local_state_replay();
	test_rpc_round_trip();
	if(failures) {
		printf("test_lightning_render: %d FAILED\n", failures);
		return 1;
	}
	printf("test_lightning_render: PASS\n");
	return 0;
}
