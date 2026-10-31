/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited).

	HEADLESS STREAMING SOAK HARNESS (Phase-0 detection layer).

	Drives the REAL "generate -> mesh -> evict -> remesh" chunk-streaming loop with a
	scripted player walk that crosses chunk boundaries thousands of times -- the same
	three threads the game uses (this main thread + the server/world thread + the mesher
	worker thread), all sharing the MEM2/newlib heaps. NO graphics, NO input, NO window.

	The point: run this under AddressSanitizer (configure with -DSANITIZE=ON under WSL/Linux,
	see tools/asan_soak.sh) so a heap overrun / use-after-free in the cross-thread streaming
	churn is caught off-hardware, at the moment of the bad access, with a precise stack --
	instead of the no-MMU Wii silently tolerating it and freezing minutes later in an
	unrelated subsystem (the bug that took ~10 hardware builds to localize).

	On PC, mem2_alloc is plain malloc, so ASan wraps every allocation in its own redzones --
	exactly the guard the Wii lacks. Caveat: the GX byte-packing in platform/wii/displaylist.c
	is NOT compiled on PC (PC uses platform/pc/displaylist.c), so a bug purely in that file is
	not covered here; the shared sim/mesher/server/entity surface (the bulk, and the assessment's
	prime suspects) IS.

	Mirrors source/main.c's init (minus graphics/input) and screen_select_world.c's new-world
	creation. Run from the REPO ROOT so config_pc.json is found.
	Usage: soak_stream [iterations] [seed]
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
	#include <direct.h>
	#define soak_mkdir(p) _mkdir(p)
#else
	#include <sys/stat.h>
	#include <sys/types.h>
	#define soak_mkdir(p) mkdir((p), 0777)
#endif

#include <m-lib/m-string.h>

#include "config.h"
#include "block/blocks.h"
#include "chunk_mesher.h"
#include "game/game_state.h"
#include "item/items.h"
#include "item/recipe.h"
#include "network/client_interface.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "platform/mem2.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "world.h"
#include "worldgen/worldgen.h"

/* A2-T M6 (lane B, a2t-d4-0927): CW_SOAK_MEASURE=1 turns the soak into a
 * STANDING capture: the player stays at spawn, and at the end the resident
 * client/server state is priced per column. Mesh bytes are converted to the
 * Wii GX encoding from the PC vertex counts (PC displaylist index = vertices
 * once finished): 9 B/vertex untinted (pos 6 + light 1 + tex 2, GX_VTXFMT0),
 * 12 B/vertex tinted (+3 tint, GX_VTXFMT4), plus a 3-byte GX_QUADS header per
 * list, rounded up to the 32-byte cache line (displaylist_rung may round a
 * little further; stated as a lower bound). Unset = the soak is unchanged. */
static void soak_measure_report(struct server_local* server) {
	size_t cols = 0, cols_meshed = 0, sections = 0, far_sections = 0;
	size_t mesh_lists = 0, verts_plain = 0, verts_tint = 0, wii_mesh_bytes = 0;
	size_t max_col_bytes = 0;
	size_t n_alloc = dict_wsection_size(gstate.world.sections);
	size_t* col_list = n_alloc ? calloc(n_alloc, sizeof(size_t)) : NULL;
	size_t n_col_list = 0;
	dict_wsection_it_t it;
	for(dict_wsection_it(it, gstate.world.sections); !dict_wsection_end_p(it);
		dict_wsection_next(it)) {
		struct world_section* sec = &dict_wsection_ref(it)->value;
		if(sec->is_far_lod) {
			far_sections++;
			continue;
		}
		cols++;
		size_t col_bytes = 0;
		bool meshed = false;
		for(int y = 0; y < COLUMN_HEIGHT; y++) {
			struct chunk* c = sec->column[y];
			if(!c)
				continue;
			sections++;
			for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
				/* PC sets .finished only at the GL upload (render); a
				 * finalized list already carries index = vertices. */
				if(!c->has_displist[k])
					continue;
				size_t v = c->mesh[k].index;
				if(!v)
					continue;
				bool tinted = k >= CHUNK_MESH_PARTS;
				size_t b = 3 + v * (tinted ? 12u : 9u);
				b = (b + 31u) & ~(size_t)31u;
				mesh_lists++;
				wii_mesh_bytes += b;
				col_bytes += b;
				if(tinted)
					verts_tint += v;
				else
					verts_plain += v;
				meshed = true;
			}
		}
		if(meshed) {
			cols_meshed++;
			if(col_list && n_col_list < n_alloc)
				col_list[n_col_list++] = col_bytes;
		}
		if(col_bytes > max_col_bytes)
			max_col_bytes = col_bytes;
	}
	size_t srv_cols = dict_server_chunks_size(server->worlds[0].chunks);
	/* per meshed column distribution (insertion sort: a few hundred) */
	for(size_t i = 1; i < n_col_list; i++)
		for(size_t j = i; j > 0 && col_list[j - 1] > col_list[j]; j--) {
			size_t t = col_list[j];
			col_list[j] = col_list[j - 1];
			col_list[j - 1] = t;
		}
	if(n_col_list)
		fprintf(stderr, "[measure] mesh per meshed column: p50 %zu B, p90 %zu B, "
						"p99 %zu B\n", col_list[n_col_list / 2],
				col_list[n_col_list * 9 / 10], col_list[n_col_list * 99 / 100]);
	free(col_list);
	fprintf(stderr,
			"[measure] client: %zu full columns (%zu meshed), %zu sections, "
			"%zu far sections; server: %zu resident columns, dim %d\n",
			cols, cols_meshed, sections, far_sections, srv_cols,
			(int)server->worlds[0].dimension);
	fprintf(stderr,
			"[measure] mesh: %zu lists, %zu untinted + %zu tinted vertices -> "
			"Wii GX %zu B total, %.0f B per meshed column (max %zu)\n",
			mesh_lists, verts_plain, verts_tint, wii_mesh_bytes,
			cols_meshed ? (double)wii_mesh_bytes / (double)cols_meshed : 0.0,
			max_col_bytes);
	fprintf(stderr,
			"[measure] per column: %.1f sections x sizeof(struct chunk)=%zu "
			"(PC; PPC 704) + CHUNK_GUARD %d; server column slot %u B; view "
			"slot %u B\n",
			cols ? (double)sections / (double)cols : 0.0, sizeof(struct chunk),
			CHUNK_GUARD, MEM2_POOL_COLUMN_OBJECT, MEM2_POOL_VIEW_OBJECT);
}

int main(int argc, char** argv) {
	long iterations = (argc > 1) ? atol(argv[1]) : 30000;
	int64_t seed = (argc > 2) ? (int64_t)atoll(argv[2]) : 20260608LL;

	fprintf(stderr, "[soak] start: %ld iterations, seed %lld\n", iterations,
			(long long)seed);

	/* ---- init: mirrors source/main.c (minus input/graphics/screen) ---- */
	rand_gen_seed(&gstate.rand_src);
	config_load_user_or_default(&gstate.config_user, "config.json",
								"config_pc.json", stderr);

	gstate.config.fov = 70.0f;
	gstate.config.render_distance = MAX_VIEW_DISTANCE * 16.0f + 32.0f;
	gstate.config.fog_distance = MAX_VIEW_DISTANCE * 16.0f;

	blocks_init();
	items_init();
	recipe_init();

	world_create(&gstate.world);
	for(int k = 0; k < 256; k++)
		gstate.players[0].windows[k] = NULL;

	if(!clin_init() || !svin_init() || !chunk_mesher_init()) {
		fprintf(stderr, "failed to initialize worker channels\n");
		return 1;
	}

	dict_entity_init(gstate.entities);
	gstate.players[0].local_player = NULL;
	gstate.players[0].in_water = false;
	gstate.players[0].oxygen = MAX_OXYGEN;
	/* Keep world_loaded=false: clin_update()'s 50ms PLAYER_POS send derefs
	   gstate.players[0].local_player (NULL in this harness). We send PLAYER_POS ourselves below;
	   clin_update() still drains the CRPC inbox and builds the client chunks. */
	gstate.world_loaded = false;

	/* server_local_create starts a detached worker and the platform layer exposes
	   no join/shutdown primitive. Keep its owner alive until process termination;
	   a stack owner lets the worker race main's epilogue after the DONE line. */
	static struct server_local server;
	server_local_create(&server, false, false, false, false, false, false,
						false); /* spawns the prio-64 server/world thread */

	/* ---- create a fresh world on disk + load it (mirrors screen_sworld_create_new) ---- */
	soak_mkdir("soak_saves");
	string_t dir;
	string_init_printf(dir, "soak_saves/soak_world");
	soak_mkdir(string_get_cstr(dir));
	string_t region;
	string_init_printf(region, "%s/region", string_get_cstr(dir));
	soak_mkdir(string_get_cstr(region));
	string_clear(region);

	vec3 spawn = {0.5f, 64.0f, 0.5f};
	worldgen_find_spawn(seed, spawn);

	struct level_archive la;
	if(!level_archive_create_new(&la, dir, seed, spawn)) {
		fprintf(stderr, "[soak] level_archive_create_new FAILED\n");
		return 1;
	}
	/* M6: CW_SOAK_NETHER=1 starts the player in the Nether (the
	 * debug.start_nether create path), so the measurement prices Nether
	 * columns. Unset = unchanged. */
	if(getenv("CW_SOAK_NETHER")) {
		vec2 rot0 = {0.0F, 0.0F};
		vec3 vel0 = {0.0F, 0.0F, 0.0F};
		if(!level_archive_write_player(&la, spawn, rot0, vel0, WORLD_DIM_NETHER)) {
			fprintf(stderr, "[soak] nether player write FAILED\n");
			return 1;
		}
	}
	level_archive_destroy(&la); /* flush level.dat to disk */

	struct server_rpc load = {.type = SRPC_LOAD_WORLD};
	string_init_set(load.payload.load_world.name, dir);
	svin_rpc_send(&load);

	thread_msleep(500); /* let the server thread create the world + spawn region */

	/* ---- the walk: ping-pong along +/-X far enough to force evict + re-stream ---- */
	double px = spawn[0], py = spawn[1], pz = spawn[2];
	double dirx = 1.0;
	const bool measure = getenv("CW_SOAK_MEASURE") != NULL;
	const double STEP = measure ? 0.0 : 4.0; /* blocks per iteration */
	/* 3x the view radius so chunks fully evict behind us and regenerate on return --
	   the worst case for the streaming alloc/free + remesh churn the freeze rides on. */
	const double RANGE = MAX_VIEW_DISTANCE * 16.0 * 3.0;
	const double origin_x = px;

	ptime_t t0 = time_get();
	long crossings = 0;

	for(long it = 0; it < iterations; it++) {
		px += dirx * STEP;
		if(px > origin_x + RANGE) {
			px = origin_x + RANGE;
			dirx = -1.0;
			crossings++;
		}
		if(px < origin_x - RANGE) {
			px = origin_x - RANGE;
			dirx = 1.0;
			crossings++;
		}
		gstate.players[0].camera.x = (float)px;
		gstate.players[0].camera.y = (float)py;
		gstate.players[0].camera.z = (float)pz;

		struct server_rpc pos = {.type = SRPC_PLAYER_POS};
		pos.payload.player_pos.x = px;
		pos.payload.player_pos.y = py;
		pos.payload.player_pos.z = pz;
		pos.payload.player_pos.rx = 0.0f;
		pos.payload.player_pos.ry = 0.0f;
		pos.payload.player_pos.vel_y = 0.0f;
		svin_rpc_send(&pos);

		clin_update();			/* drain CRPC inbox -> build client chunks */
		chunk_mesher_receive(); /* integrate finished meshes (frees old display lists) */
		world_build_chunks(
			&gstate.world, CHUNK_MESHER_QLENGTH,
			true); /* send dirty chunks to the mesher; no near/far gate here */

		thread_msleep(2); /* yield so the server + mesher threads make progress */

		if((it % 2000) == 0)
			fprintf(stderr, "[soak] it=%ld px=%.0f crossings=%ld dropped=%u\n", it, px,
					crossings, svin_dropped_count());
	}

	fprintf(stderr,
			"[soak] DONE: %ld iterations, %ld crossings, %.1fs, dropped=%u\n",
			iterations, crossings, time_diff_s(t0, time_get()),
			svin_dropped_count());

	if(measure)
		soak_measure_report(&server);
	string_clear(dir);
	return 0;
}
