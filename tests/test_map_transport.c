/*
	Held-map colour transport (held-map render slice part 1, 2026-08-26):
	CRPC_MAP_DATA carries a held map's Beta colors[16384] from the server-side
	map cache (item_map part 2a) to the client-side mirror the renderer reads.
	Beta needs no such packet in SP -- MapItemRenderer.renderMap reads
	mapData.colors in-process [740c583 ItemRenderer.renderItemInFirstPerson,
	MapItemRenderer.renderMap]; this engine's client/server split makes that
	read an explicit transport. Transport-only: no world rule changes.

	Pinned here:
	  1. holding a resident map + one server item-update tick delivers the
	     cache's colour bytes to clin_held_map_colors BYTE-FOR-BYTE;
	  2. CHANGE GATING (documented deviation vs Beta's every-frame upload):
	     an unchanged scan does NOT re-send (mirror version stable across
	     ticks); a mutated colour byte DOES (version bumps, byte arrives);
	  3. an unknown map id owns a blank plane;
	  4. pool hygiene: 100 mutate+tick rounds through the 2-slot pool leak
	     nothing (every round still delivers -- a leaked slot would starve
	     the non-blocking sender into permanent deferral);
	  5. clin_drain_discard releases an in-flight map slot exactly like the
	     chunk/far slots (the drain-thread fixture class would otherwise
	     leak the pool dry).
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "entity/entity.h"
#include "game/game_state.h"
#include "graphics/render_item.h"
#include "item/items.h"
#include "item/items/item_map.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/map_data.h"
#include "network/server_interface.h" /* MAX_PLAYER_HEALTH */
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_map_transport_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

static struct server_local s;

/* Discard-drain thread for the SETTLE phase only (env-test pattern): consumes
 * the initial chunk-stream so server_local_test_update_once cannot block in
 * the 16-slot chunk pool. Stopped before the map phases, which pump the inbox
 * synchronously through clin_update_drain instead. */
static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

/* Full-processing inbox pump: clin_update_drain routes EVERY queued CRPC
 * through the real clin_process (so the map case runs exactly as in the
 * app, incl. mirror install + slot release). Delivery counting is done by
 * the caller via the mirror VERSION delta, which is precisely the renderer's
 * own change signal. Requires the headless gstate client world below so
 * stray non-map CRPCs process harmlessly (boat_env precedent). */

/* RetroMCP client MapData.java:89-109,147-158: inclusive +/-64,
 * cast-to-int truncation after +0.5, Java byte wrap; no edge arrows. */
static void marker_math_golden(void) {
	struct map_data md = {.scale = 3, .x_center = 100, .z_center = -200};
	struct item_map_marker m;
	assert(item_map_player_marker(&md, 100, -200, 90, 0, true, &m));
	assert(m.type == 0 && m.x == 0 && m.y == 0 && m.rotation == 4);
	assert(item_map_player_marker(&md, 104, -204, -90, 0, true, &m));
	assert(m.x == 1 && m.y == 0 && m.rotation == -3);
	assert(item_map_player_marker(&md, 612, -712, 0, 0, true, &m));
	assert(m.x == -128 && m.y == -127);
	assert(!item_map_player_marker(&md, 612.01, -200, 0, 0, true, &m));
	assert(!item_map_player_marker(&md, 100, -712.01, 0, 0, true, &m));
	assert(!item_map_player_marker(&md, 100, -200, 0, -1, true, &m));
	assert(!item_map_player_marker(&md, 100, -200, 0, 0, false, &m));
	md.dimension = -1; md.step = 10;
	assert(item_map_player_marker(&md, 100, -200, 180, -1, true, &m));
	assert(m.rotation == 3); /* (34187121 + 121) >> 15 & 15 */
	md.step = 100;
	assert(item_map_player_marker(&md, 100, -200, 0, -1, true, &m));
	assert(m.rotation == 10); /* Java int32 overflow, 3418713310 >> 15 & 15 */
	md.dimension = 0;
	for(int scale = 0; scale <= 4; scale++) {
		md.scale = scale;
		assert(item_map_player_marker(&md, 100 + (1 << scale) * 64,
			-200 - (1 << scale) * 64, 180, 0, true, &m));
		assert(m.x == -128 && m.y == -127 && m.rotation == 8);
		assert(!item_map_player_marker(&md, 100 + (1 << scale) * 64.1,
			-200, 0, 0, true, &m));
	}
}

static void marker_decode_golden(void) {
	struct item_map_marker m[ITEM_MAP_MAX_MARKERS];
	const uint8_t bytes[] = {1, 0x12, 3, 4, 0x80, 128, 255, 0xFF, 127, 0, 99};
	assert(item_map_decode_markers(bytes, sizeof(bytes), m) == 3);
	assert(m[0].type == 2 && m[0].rotation == 1 && m[0].x == 3 && m[0].y == 4);
	assert(m[1].type == 0 && m[1].rotation == -8 && m[1].x == -128 && m[1].y == -1);
	assert(m[2].type == -1 && m[2].rotation == 0 && m[2].x == 127 && m[2].y == 0);
	assert(item_map_decode_markers(bytes, 1, m) == 0);
	uint8_t max_packet[255] = {1};
	for(size_t i = 0; i < ITEM_MAP_MAX_MARKERS; i++) {
		max_packet[3*i+1] = 0x70;
		max_packet[3*i+2] = (uint8_t)i;
		max_packet[3*i+3] = (uint8_t)(255-i);
	}
	assert(item_map_decode_markers(max_packet, sizeof(max_packet), m) == 84);
	assert(m[83].type == 0 && m[83].rotation == 7 && m[83].x == 83 && m[83].y == -84);
}

static void marker_render_golden(void) {
	/* MapItemRenderer.java:81-103: origin=(-4.5,+4.5) pixels after
	 * scale+offset. In our twice-size, inverted-Y footprint: (119,119). */
	struct item_map_marker marker = {0, 0, 0, 0};
	mat4 matrix, view = GLM_MAT4_IDENTITY_INIT;
	uint8_t uv[4][2];
	vec4 corner = {-1,-1,0,1}, transformed;
	render_item_map_marker_transform(&marker, view, matrix, uv);
	glm_mat4_mulv(matrix, corner, transformed);
	assert(fabsf(transformed[0]*256-119) < 0.001F && fabsf(transformed[1]*256-119) < 0.001F);
	assert(transformed[2] == -9.0F/256 && matrix[2][2] == 6.0F/256);
	assert(uv[0][0] == 0 && uv[0][1] == 0 && uv[2][0] == 64 && uv[2][1] == 64);
	marker = (struct item_map_marker){5, -128, -127, 4};
	render_item_map_marker_transform(&marker, view, matrix, uv);
	glm_mat4_mulv(matrix, corner, transformed);
	assert(fabsf(transformed[0]*256+9) < 0.001F && fabsf(transformed[1]*256-264) < 0.001F);
	assert(uv[0][0] == 64 && uv[0][1] == 64 && uv[2][0] == 128 && uv[2][1] == 128);
}

static void marker_cache_golden(void) {
	/* Marker-only delivery must preserve terrain and its upload version,
	 * survive eviction, replace the whole list, and handle empty clears. */
	static uint8_t colors[MAP_DATA_COLORS_SIZE];
	struct item_map_marker markers[ITEM_MAP_MAX_MARKERS];
	colors[2] = 42;
	assert(clin_map_deliver(300, colors));
	uint32_t before, after;
	clin_held_map_colors(300, &before);
	const uint8_t bytes[] = {1,0x80,128,255,0xFF,127,0};
	assert(clin_map_markers_deliver(300, bytes, sizeof(bytes)));
	assert(clin_held_map_colors(300, &after)[2] == 42 && before == after);
	for(int id = 301; id <= 310; id++) assert(clin_map_deliver(id, colors));
	/* A color-only update of an evicted map must retain its prior icons. */
	colors[2] = 43;
	assert(clin_map_deliver(300, colors));
	for(int id = 311; id <= 320; id++) assert(clin_map_deliver(id, colors));
	assert(clin_held_map_markers(300, markers) == 2);
	assert(markers[0].rotation == -8 && markers[0].x == -128 && markers[0].y == -1);
	assert(markers[1].type == -1 && markers[1].rotation == 0);
	const uint8_t empty[] = {1};
	assert(clin_map_markers_deliver(300, empty, sizeof(empty)));
	assert(clin_held_map_markers(300, markers) == 0);
	assert(clin_held_map_colors(300, NULL)[2] == 43);
	/* A marker arriving before any terrain creates a genuine blank plane. */
	assert(clin_map_markers_deliver(321, bytes, sizeof(bytes)));
	const uint8_t* blank = clin_held_map_colors(321, NULL);
	for(size_t i = 0; i < MAP_DATA_COLORS_SIZE; i++) assert(blank[i] == 0);
}

int main(int argc, char** argv) {
	if(argc == 2 && strcmp(argv[1], "--marker-math") == 0) {
		marker_math_golden(); return 0;
	}
	if(argc == 2 && strcmp(argv[1], "--marker-decode") == 0) {
		marker_decode_golden(); return 0;
	}
	if(argc != 2 || strcmp(argv[1], "--marker-local") != 0) {
		marker_math_golden();
		marker_decode_golden();
		marker_render_golden();
	}
	test_fixture_dir("test_map_transport_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	printf("test_map_transport: CRPC_MAP_DATA server->client mirror\n");

	clin_init();
	blocks_init();
	items_init();

	/* Headless client world so clin_process can install whatever CRPCs the
	 * server fixture emits (boat_env's established pattern). */
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/data/map_0.dat"));
	remove(test_fixture_join(TMP_WORLD, "/data/idcounts.dat"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	string_init_set_str(s.level_name, TMP_WORLD);
	for(int i = 0; i < MAX_MAP_CACHE; i++) s.map_cache_id[i] = INT32_MIN;
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.active_inventory = &s.player.inventory;
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	/* SETTLE with a discard drain running: the first full updates stream the
	 * whole view distance of chunks; without a consumer the server blocks in
	 * the 16-slot chunk pool (the env-test drain-thread lesson, relearned
	 * here as a live deadlock). Once streaming settles, steady-state ticks
	 * emit only a handful of CRPCs, so the drain thread stops and the map
	 * phases below pump synchronously. */
	g_drain_run = 1;
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	for(int t = 0; t < 40; t++)
		server_local_test_update_once(&s);
	g_drain_run = 0;
	thread_join(&drain);
	clin_drain_discard(); /* flush any tail the thread missed */

	/* Create map 0 the part-2a way (onCreated allocates the id + installs
	 * the cache entry), then hold it. */
	struct item_data stack = {.id = ITEM_MAP, .durability = 0, .count = 1};
	assert(items[ITEM_MAP] && items[ITEM_MAP]->onCreated);
	/* S1: onCreated takes the acting crafter; this rig gives the map to the
	 * HOST, so the host is the body it centres on. */
	assert(items[ITEM_MAP]->onCreated(&s, &s.player, &stack));
	inventory_set_hotbar(&s.player.inventory, 0);
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR, stack);
	struct map_data* md = item_map_cache_resident(&s, (int32_t)stack.durability);
	assert(md && "map must be cache-resident after onCreated");

	/* 1. one full server update delivers the bytes, byte-for-byte. The
	 * mirror VERSION is the delivery counter (it bumps exactly once per
	 * installed CRPC_MAP_DATA -- the renderer's own change signal). */
	uint32_t before_delivery;
	assert(clin_held_map_colors((int32_t)stack.durability, &before_delivery));
	server_local_test_update_once(&s);
	clin_update_drain();
	uint32_t v1 = 0;
	const uint8_t* mirror
		= clin_held_map_colors((int32_t)stack.durability, &v1);
	assert(mirror && "mirror must hold the held map after delivery");
	assert(v1 == before_delivery + 1 && "one delivery advances the version once");
	assert(memcmp(mirror, md->colors, MAP_DATA_COLORS_SIZE) == 0);
	/* Drive the actual holder update and inbox consumer, independently of
	 * colour changes. MapData.java:89-109; ItemMap.java onUpdate. */
	struct item_map_marker marker[ITEM_MAP_MAX_MARKERS];
	double saved_x = s.player.x, saved_z = s.player.z;
	float saved_rx = s.player.rx;
	s.player.x = md->x_center + 4; s.player.z = md->z_center - 4;
	s.player.rx = -90;
	items[ITEM_MAP]->onUpdate(&s, &stack, INVENTORY_SLOT_HOTBAR, true);
	clin_update_drain();
	assert(clin_held_map_markers(stack.durability, marker) == 1);
	assert(marker[0].type == 0 && marker[0].x == 1 && marker[0].y == 0);
	assert(marker[0].rotation == -3);
	s.player.x = md->x_center + 512; s.player.z = md->z_center - 512;
	items[ITEM_MAP]->onUpdate(&s, &stack, INVENTORY_SLOT_HOTBAR, true);
	clin_update_drain();
	assert(clin_held_map_markers(stack.durability, marker) == 1);
	assert(marker[0].x == -128 && marker[0].y == -127);
	s.player.x += 0.01;
	items[ITEM_MAP]->onUpdate(&s, &stack, INVENTORY_SLOT_HOTBAR, true);
	clin_update_drain();
	assert(clin_held_map_markers(stack.durability, marker) == 0);
	s.player.x = saved_x; s.player.z = saved_z; s.player.rx = saved_rx;
	items[ITEM_MAP]->onUpdate(&s, &stack, INVENTORY_SLOT_HOTBAR, true);
	clin_update_drain();
	/* Direct scans above may also deliver colour data on the next full tick. */
	v1 = 0; clin_held_map_colors(stack.durability, &v1);
	printf("  delivery: %d bytes byte-exact, version=%u OK\n",
		   MAP_DATA_COLORS_SIZE, v1);

	/* 3. unknown id owns a blank plane. */
	const uint8_t* blank = clin_held_map_colors((int32_t)stack.durability + 1, NULL);
	assert(blank);
	for(size_t i = 0; i < MAP_DATA_COLORS_SIZE; i++) assert(blank[i] == 0);

	/* 2a. CONVERGE, then the change gate holds: the held map's onUpdate scan
	 * legitimately rewrites colour bytes every tick (Beta's progressive
	 * strip scan) until the whole superflat neighbourhood is scanned and
	 * re-scans become byte-identical -- only THEN must the gate stop
	 * sending. Tick until the mirror version is stable for 8 consecutive
	 * ticks (cap 2000: static terrain guarantees convergence). */
	uint32_t vstable = v1;
	int quiet = 0, ticks = 0;
	while(quiet < 8) {
		assert(++ticks < 2000 && "scan must converge on static terrain");
		server_local_test_update_once(&s);
		clin_update_drain();
		uint32_t v = 0;
		assert(clin_held_map_colors((int32_t)stack.durability, &v));
		quiet = (v == vstable) ? quiet + 1 : 0;
		vstable = v;
	}
	printf("  change gate: scan converged (v=%u), 8 quiet ticks, 0 re-sends "
		   "OK\n",
		   vstable);

	/* 2b + 4. mutate a colour byte each round: every round must ADVANCE the
	 * mirror (>= +1 -- the live scan may add its own correction delivery
	 * when its strip crosses the mutated cell) and end byte-identical.
	 * 100 rounds through the 2-slot pool proves nothing leaks (a leaked
	 * slot would starve the non-blocking sender: version would freeze). */
	uint32_t prev = vstable;
	for(int round = 0; round < 100; round++) {
		md->colors[round % MAP_DATA_COLORS_SIZE]
			= (uint8_t)((md->colors[round % MAP_DATA_COLORS_SIZE] + 4) & 0xFF);
		server_local_test_update_once(&s);
		clin_update_drain();
		uint32_t v = 0;
		const uint8_t* m2
			= clin_held_map_colors((int32_t)stack.durability, &v);
		assert(m2 && v >= prev + 1
			   && "every mutation round must deliver (pool starvation would "
				  "freeze the version)");
		assert(memcmp(m2, md->colors, MAP_DATA_COLORS_SIZE) == 0);
		prev = v;
	}
	printf("  pool hygiene: 100 mutate+deliver rounds, no starvation OK\n");

	/* 5. discard path releases the slot: mutate, tick (update in flight),
	 * DISCARD instead of processing, then prove the pool still serves both
	 * slots by delivering on two further mutation rounds. */
	md->colors[0] = (uint8_t)(md->colors[0] + 4);
	server_local_test_update_once(&s);
	clin_drain_discard();
	for(int k = 0; k < 2; k++) {
		md->colors[1 + k] = (uint8_t)(md->colors[1 + k] + 4);
		server_local_test_update_once(&s);
		clin_update_drain();
		uint32_t v = 0;
		assert(clin_held_map_colors((int32_t)stack.durability, &v)
			   && v >= prev + 1
			   && "discarded slot must have returned to the pool");
		prev = v;
	}
	printf("  discard release: pool intact after clin_drain_discard OK\n");

	marker_cache_golden();
	printf("test_map_transport: PASS\n");
	string_clear(s.level_name);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
