/*
	Deviation-#18 closure: the death->respawn split.

	server_local_set_player_health(lethal) now commits only the DEAD state
	(health<=0, death-position latch, unpaused dwell) and the whole
	drop/journal/respawn transaction runs later in
	server_local_respawn_request (the death screen's Respawn button).

	Drives the real server paths through:
	  S1  alive->dead commit: no drops, no world switch, inventory retained,
	      death position latched, pause cleared;
	  S2  dead is terminal: further lethal writes, heals and hazard damage
	      are all rejected with no armor/i-frame churn
	      [740c583 EntityLiving.attackEntityFrom:312; heal:296];
	  S3  drops spawn at the DEATH position even after the corpse drifted,
	      then the fresh body owns spawn placement + reset state;
	  S4  respawn while alive is a no-op;
	  S5  respawn is gated while a world switch is in flight;
	  S6  bed spawn resolves at RESPAWN time (Beta Minecraft.respawn):
	      a bed destroyed DURING the death-screen dwell diverts to world
	      spawn + clears the bed spawn -- the old death-time resolution
	      would have used the stale bed;
	  S7  the valid-bed half: respawn lands in the bed's first empty cell;
	  S8  quit-while-dead persists health<=0 through the production unload
	      (level.dat round-trip), and a fresh dead load can respawn, with
	      the saved corpse position as the drop-origin fallback.

	The Nether/journal half of the split keeps its own coverage in
	test_death_drop_journal_env.c (every lethal site there now drives
	server_local_respawn_request explicitly). The SRPC_TOGGLE_PAUSE
	dead-guard and the client screen itself are covered by the paused=false
	asserts here plus the visual :8099 capture.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define RMDIR(p) rmdir(p)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "entity/combat.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "game/gui/screen_death.h"
#include "graphics/gui_util.h"
#include "network/inventory_logic.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/mem2.h"
#include "platform/thread.h"

static char s_fixture_WORLD_ROOT[128]; /* was #define WORLD_ROOT='test_death_screen_env_world' (a literal) */
#define WORLD_ROOT s_fixture_WORLD_ROOT
static volatile int drain_run = 1;
static void* drain_client(void* unused) {
	(void)unused;
	while(drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void remove_world_files(const char* root) {
	char path[256];
	const char* files[] = {
		"level.dat",       "level.dat_new", "level.dat_old",
		"death_drops.dat",
		/* Overworld chest/sign archives carry NO dim suffix (unlike the
		 * journal test's Nether chests_dim-1.dat). */
		"chests.dat", "chests.dat_new", "chests.dat_old",
		"signs.dat",  "signs.dat_new",  "signs.dat_old",
		"region/r.0.0.mcr",
	};
	for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", root, files[i]);
		remove(path);
	}
}

static void remove_world_dirs(const char* root) {
	char path[256];
	remove_world_files(root);
	snprintf(path, sizeof(path), "%s/region", root);
	RMDIR(path);
	RMDIR(root);
}

static void make_world_dirs(const char* root) {
	char path[256];
	MKDIR(root);
	snprintf(path, sizeof(path), "%s/region", root);
	MKDIR(path);
}

static void create_level_with_hotbar(const char* root, uint16_t item_id) {
	string_t name;
	string_init_set_str(name, root);
	struct level_archive level;
	assert(level_archive_create_new(
		&level, name, 424242, (vec3) {8.5F, 70.0F, 8.5F}));
	level_archive_destroy(&level);
	assert(level_archive_create(&level, name));
	struct item_data items[INVENTORY_SIZE] = {{0}};
	items[INVENTORY_SLOT_HOTBAR]
		= (struct item_data) {.id = item_id, .count = 7};
	struct inventory inventory = {
		.items = items,
		.capacity = INVENTORY_SIZE,
	};
	int16_t health = MAX_PLAYER_HEALTH;
	assert(level_archive_write_player(
		&level, (vec3) {8.5F, 70.0F, 8.5F}, (vec2) {0}, NULL,
		WORLD_DIM_OVERWORLD));
	assert(level_archive_write_inventory(&level, &inventory));
	assert(level_archive_write(&level, LEVEL_PLAYER_HEALTH, &health));
	assert(level_archive_save(&level));
	level_archive_destroy(&level);
	string_clear(name);
}

/* Same hand-init convention as test_death_drop_journal_env.c's init_server,
 * Overworld flavour. read_health_from_level models the production load path
 * (server_local_load_world_impl reads LEVEL_PLAYER_HEALTH verbatim). */
static void init_server_ex(struct server_local* server, const char* root,
						   bool read_health_from_level) {
	memset(server, 0, sizeof(*server));
	rand_gen_seed(&server->rand_src);
	server->container_drop_rng.seed = 0x12345678u;
	string_init_set_str(server->level_name, root);
	assert(level_archive_create(&server->level, server->level_name));
	if(!level_archive_read_death_tx_counter(&server->level,
											&server->death_tx_counter))
		server->death_tx_counter = CAVEWII_DEATH_TX_LEGACY_FLOOR;
	assert(inventory_create(
		&server->player.inventory, &inventory_logic_player, server,
		&server->player,
		INVENTORY_SIZE, 0, 0, 0));
	assert(level_archive_read_inventory(
		&server->level, &server->player.inventory));
	server->player.active_inventory = &server->player.inventory;
	server->player.x = 8.5;
	server->player.y = 70.0;
	server->player.z = 8.5;
	server->player.spawn_x = 8;
	server->player.spawn_y = 70;
	server->player.spawn_z = 8;
	server->player.health = MAX_PLAYER_HEALTH;
	if(read_health_from_level) {
		level_archive_read(&server->level, LEVEL_PLAYER_HEALTH,
						   &server->player.health, 0);
		server->player.has_death_pos = false; /* mirrors the load path */
	}
	server->player.dimension = WORLD_DIM_OVERWORLD;
	server->player.has_pos = true;
	server->player.finished_loading = true;
	server->player.fire = -PLAYER_FIRE_RESISTANCE;
	server->player.oxygen = MAX_OXYGEN;
	server->world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(server))
		abort();
	piston_moving_store_init(&server->piston_moving);
	dict_entity_init(server->entities);
	server->world_populate_scratch = server_world_populate_scratch_create();
	assert(server->world_populate_scratch);
	assert(server_world_create_with_populate_scratch(
		&server->world, server->level_name, WORLD_DIM_OVERWORLD, 424242, true,
		server->world_populate_scratch));
	server->world.local = server;
	server->worlds[0].session = server; /* A1a (D1): owning session for services/callbacks */
	assert(!server->death_drop_journal);
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&server->world, 0, 0, &chunk) && chunk);
}

static void init_server(struct server_local* server, const char* root) {
	init_server_ex(server, root, false);
}

static void free_runtime_entities(struct server_local* server) {
	dict_entity_it_t it;
	dict_entity_it(it, server->entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_clear(server->entities);
}

static void discard_server(struct server_local* server) {
	if(server->player.has_pos)
		server_world_discard(&server->world);
	server_world_populate_scratch_destroy(server->world_populate_scratch);
	server->world_populate_scratch = NULL;
	piston_moving_store_destroy(&server->piston_moving);
	free_runtime_entities(server);
	if(server->level.data)
		level_archive_discard(&server->level);
	if(server->player.active_inventory
	   && server->player.active_inventory != &server->player.inventory)
		inventory_destroy_owned(server->player.active_inventory, &server->player);
	inventory_destroy(&server->player.inventory, &server->player);
	mem2_free_sized(
		server->entity_tick_keys,
		server->entity_tick_capacity * sizeof(*server->entity_tick_keys));
	server->entity_tick_keys = NULL;
	server->entity_tick_count = 0;
	server->entity_tick_capacity = 0;
	server->entity_tick_reserved = 0;
	server->entity_tick_active = false;
	death_drop_journal_free(server->death_drop_journal);
	server->death_drop_journal = NULL;
	string_clear(server->level_name);
}

static size_t count_items_near(struct server_local* server, double x, double y,
							   double z, double radius) {
	size_t count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, server->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && fabs(e->pos[0] - x) <= radius
		   && fabs(e->pos[1] - y) <= radius && fabs(e->pos[2] - z) <= radius)
			count++;
		dict_entity_next(it);
	}
	return count;
}

static size_t count_items_total(struct server_local* server) {
	size_t count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, server->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM)
			count++;
		dict_entity_next(it);
	}
	return count;
}

static void set_fixture_block(struct server_local* server, int x, int y, int z,
							  uint8_t type, uint8_t metadata) {
	/* Fixture-trap note (d12 memory): set_block returns false on an identity
	 * write -- fixture layout must not assert on it. */
	(void)server_world_set_block_silent(
		&server->worlds[0], x, y, z,
		(struct block_data) {.type = type, .metadata = metadata});
}

/* Solid floor at y-1, air at y/y+1 across [x0..x1]x[z0..z1]. */
static void set_fixture_pad(struct server_local* server, int x0, int x1,
							int z0, int z1, int y) {
	for(int x = x0; x <= x1; x++)
		for(int z = z0; z <= z1; z++) {
			set_fixture_block(server, x, y - 1, z, 1 /* stone */, 0);
			set_fixture_block(server, x, y, z, 0 /* air */, 0);
			set_fixture_block(server, x, y + 1, z, 0, 0);
		}
}

/* Producer observer sees the real dispatch reply before the drain discards it. */
static unsigned retry_replies,return_replies;
static void respawn_reply_observer(const struct client_rpc* rpc,void* unused) {
	(void)unused;
	if(rpc->type==CRPC_RETURN_INGAME){return_replies++;return;}
	if(rpc->type!=CRPC_HOST_RESPAWN)return;
	assert(rpc->payload.host_respawn.result==HOST_RESPAWN_RETRY);
	assert(rpc->payload.host_respawn.owner==0
		&&rpc->payload.host_respawn.generation==0
		&&rpc->payload.host_respawn.respawn_epoch==0);
	retry_replies++;
}

/* Real reset/return handlers distinguish a handoff midpoint from saving. */
static void test_death_exit_client_settlement(void) {
	world_create(&gstate.world);dict_entity_init(gstate.entities);
	assert(clin_world_payloads_set_enabled(true));
	clin_process(&(struct client_rpc){.type=CRPC_WORLD_RESET});
	struct client_session* host=&gstate.players[0];
	host->death=(struct death_ui){.active=true,.exit_pending=true,
		.commit_pending=DEATH_COMMIT_EXIT};
	host->current_screen=&screen_death;
	struct client_rpc reset={.type=CRPC_WORLD_RESET,
		.payload.world_reset={.local_entity=0,.dimension=WORLD_DIM_OVERWORLD,
			.world_index=0,.world_generation=1}};
	clin_process(&reset);
	printf("S0 stamped result pending=%d active=%d title=%d\n",
		host->death.exit_pending,host->death.active,host->current_screen==&screen_title);fflush(stdout);
	assert(host->death.exit_pending&&host->death.active
		&&host->current_screen!=&screen_title);
	clin_process(&(struct client_rpc){.type=CRPC_RETURN_INGAME});
	assert(!host->death.exit_pending&&host->death.exit_failed
		&&host->death.active&&host->current_screen!=&screen_title);
	/* A later authoritative alive, settled snapshot can own a loading pane
	 * after the refused save. The global poll must release its death gate.
	 * Readiness is fixture-supplied here, not a terrain/mesher speed claim. */
	assert(host->local_player);host->local_player->health=MAX_PLAYER_HEALTH;
	gstate.world_loaded=true;gstate.world_switch_pending=false;
	host->current_screen=&screen_load_world;
	screen_death_host_respawn_poll();
	assert(!host->death.active&&client_session_host_gameplay_active(host));
	host->local_player->health=0;host->death.active=true;
	/* Fixture re-arms the retry; only untagged actual teardown completes it. */
	host->death.exit_pending=true;host->death.commit_pending=DEATH_COMMIT_EXIT;
	reset.payload.world_reset.world_generation=0;
	clin_process(&reset);
	assert(!host->death.exit_pending&&!host->death.active
		&&host->current_screen==&screen_title);
	puts("S0 real client stamped handoff/refusal/untagged death-save settlement OK");
}

/* critique-1028 death redesign (NOTES.md section 4): pure layout of the
 * two hold buttons and the footer. Widths are the 16 px font's 8 px advance
 * x2 per char (an upper bound for this font; exact widths come from
 * gutil_font_width at render time) and the A1 row's 24 px glyph cell. */
static bool rect_inside(struct menu_rect r, int x0, int y0, int x1, int y1) {
	return r.x >= x0 && r.y >= y0 && r.x + r.w <= x1 && r.y + r.h <= y1;
}
static bool rects_overlap(struct menu_rect a, struct menu_rect b) {
	return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h
		&& b.y < a.y + a.h;
}
static int text_w(const char* s) {
	return (int)strlen(s) * 16;
}
static void test_death_layout(void) {
	assert(!strcmp(DEATH_FOOTER_MOVE, "Move"));
	assert(!strcmp(DEATH_FOOTER_HOLD, "Hold to choose"));
	const int sizes[][2] = {{640, 480}, {854, 480}};
	for(int s = 0; s < 2; s++) {
		const int W = sizes[s][0], H = sizes[s][1];
		const int label_w[2] = {text_w("Respawn"), text_w("Save and quit")};
		struct death_screen_layout L;
		const float none[2] = {0.0F, 0.0F};
		screen_death_layout(W, H, label_w, none, 24, 24, text_w("Move"),
			text_w("Hold to choose"), &L);
		/* Buttons: inside the 32 px safe area, no overlap, label centred and
		 * wholly inside its button (no glyph and no "Hold" any more). */
		for(int k = 0; k < 2; k++) {
			assert(rect_inside(L.button[k], 32, 32, W - 32, H - 32));
			int lx = L.label_x[k], lw = label_w[k];
			assert(lx > L.button[k].x && lx + lw < L.button[k].x + L.button[k].w);
			assert(abs((lx - L.button[k].x) - (L.button[k].x + L.button[k].w
				- (lx + lw))) <= 1);
			assert(L.label_y[k] > L.button[k].y
				&& L.label_y[k] + 16 < L.button[k].y + L.button[k].h);
			assert(L.fill[k].w == 0 && L.line[k].w == 0);
		}
		assert(!rects_overlap(L.button[0], L.button[1]));
		/* Footer: [D-pad] Move  [A] Hold to choose -- the A1 hint row
		 * (10-31): 24 px cells, labels 5 px after the cell, 14 px from a
		 * label's end to the next cell, centred, cell row y 430 (2 px into
		 * the 428-456 strip); horizontally inside the 32 px safe area. */
		struct menu_rect dcell = {L.move_cell_x, L.footer_y, 24, 24};
		struct menu_rect acell = {L.hold_cell_x, L.footer_y, 24, 24};
		struct menu_rect mlab = {L.move_label_x, L.footer_y + 5,
			text_w("Move"), 14};
		struct menu_rect hlab = {L.hold_label_x, L.footer_y + 5,
			text_w("Hold to choose"), 14};
		assert(L.footer_y == DEATH_FOOTER_Y_FULL && L.footer_y == 430);
		assert(L.footer_y == H - GUTIL_HINT_BOTTOM_FULL - GUTIL_HINT_STRIP_H
			+ (GUTIL_HINT_STRIP_H - GUTIL_HINT_GLYPH) / 2);
		assert(L.move_label_x == L.move_cell_x + 24 + GUTIL_HINT_LABEL_GAP);
		assert(L.hold_cell_x == L.move_label_x + text_w("Move") + GUTIL_HINT_GAP);
		assert(L.hold_label_x == L.hold_cell_x + 24 + GUTIL_HINT_LABEL_GAP);
		assert(abs((L.footer_x - 0) - (W - (L.footer_x + L.footer_w))) <= 1);
		const struct menu_rect foot[] = {dcell, mlab, acell, hlab};
		for(int i = 0; i < 4; i++) {
			assert(foot[i].x >= 32 && foot[i].x + foot[i].w <= W - 32);
			/* The A1 strip ends at TV-safe 456 (24 px above the bottom). */
			assert(foot[i].y + foot[i].h <= H - GUTIL_HINT_BOTTOM_FULL);
			for(int k = 0; k < 2; k++)
				assert(!rects_overlap(foot[i], L.button[k]));
			for(int j = i + 1; j < 4; j++)
				assert(!rects_overlap(foot[i], foot[j]));
		}
		/* The hold fill: starts at the button's LEFT inner edge, grows
		 * monotonically with progress, full at 1, clamped beyond. */
		for(int k = 0; k < 2; k++) {
			int prev = -1;
			const float steps[] = {0.1F, 0.25F, 0.5F, 0.75F, 1.0F, 1.5F};
			for(size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
				float f[2] = {0, 0};
				f[k] = steps[i];
				struct death_screen_layout F;
				screen_death_layout(W, H, label_w, f, 24, 24, text_w("Move"),
					text_w("Hold to choose"), &F);
				struct menu_rect r = F.button[k], fr = F.fill[k], ln = F.line[k];
				assert(fr.x == r.x + 2 && fr.y == r.y + 2 && fr.h == r.h - 4);
				assert(ln.x == r.x + 2 && ln.y == r.y + r.h - 5 && ln.h == 3
					   && ln.w == fr.w);
				assert(steps[i] <= 1.0F ? fr.w > prev : fr.w == prev);
				if(steps[i] < 1.0F)
					assert(fr.w < r.w - 4);
				else
					assert(fr.w == r.w - 4);
				prev = fr.w;
				assert(F.fill[1 - k].w == 0);
			}
		}
	}
	/* The compact (split) pane keeps its own bar inside the pane. */
	{
		const int label_w[2] = {text_w("Respawn"), text_w("Leave")};
		struct death_screen_layout L;
		screen_death_layout(640, 240, label_w, NULL, 24, 24, text_w("Move"),
			text_w("Hold to choose"), &L);
		assert(L.footer_y == DEATH_FOOTER_Y_COMPACT
			&& L.footer_y + 24 <= 240 - GUTIL_HINT_BOTTOM_SPLIT);
		assert(!rects_overlap(L.button[0], L.button[1]));
		for(int k = 0; k < 2; k++)
			assert(L.button[k].y + L.button[k].h <= L.footer_y);
	}
	printf("death layout: buttons/labels/fill/footer OK at 640x480, 854x480, "
		   "640x240\n");
}

int main(void) {
	test_fixture_dir("test_death_screen_env_world", s_fixture_WORLD_ROOT, sizeof s_fixture_WORLD_ROOT);
	assert(clin_init());
	assert(svin_init());
	blocks_init();
	items_init();
	test_death_exit_client_settlement();
	test_death_layout();
	struct thread drain;
	thread_create(&drain, drain_client, NULL, 64);

	remove_world_dirs(WORLD_ROOT);
	make_world_dirs(WORLD_ROOT);
	create_level_with_hotbar(WORLD_ROOT, 4 /* cobblestone */);

	struct server_local server;
	init_server(&server, WORLD_ROOT);

	/* ---- S1: alive->dead commits ONLY the dead state ------------------- */
	/* Die AWAY from world spawn so the death point, the corpse drift and the
	 * respawn placement are three pairwise-distinct positions. */
	server.player.x = 5.5;
	server.player.z = 6.5;
	server.paused = true; /* a queued HOME toggle preceded the hit */
	assert(server_local_set_player_health(&server, 0));
	assert(server.player.health <= 0);
	assert(server.player.has_death_pos);
	assert(server.player.death_x == 5.5 && server.player.death_y == 70.0
		   && server.player.death_z == 6.5);
	assert(!server.paused); /* doesGuiPauseGame()==false: the dwell ticks */
	assert(server.world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
	assert(count_items_total(&server) == 0); /* no drops at death */
	{
		struct item_data held;
		assert(inventory_get_slot(&server.player.inventory,
								  INVENTORY_SLOT_HOTBAR, &held)
			   && held.id == 4 && held.count == 7); /* inventory retained */
	}
	printf("S1 dead-state commit OK\n");

	/* ---- S2: dead is terminal ------------------------------------------ */
	assert(!server_local_set_player_health(&server, 0));  /* void re-kill */
	assert(!server_local_set_player_health(&server, 10)); /* heal rejected */
	assert(server.player.health <= 0);
	{
		bool accepted = true;
		short before_resist = server.player.hurt_resist_time;
		assert(server_local_damage_player_result(&server, 8, 0.0f, 0.0f,
												 &accepted));
		assert(!accepted); /* corpse hit shaped like an i-frame absorb */
		assert(server.player.hurt_resist_time == before_resist);
		assert(server.player.health <= 0);
	}
	printf("S2 dead-terminal OK\n");

	/* ---- S3: corpse drift; drops belong to the death position ---------- */
	server.player.x = 12.5; /* the dead body slid (client corpse physics) */
	server.player.z = 13.5;
	assert(server_local_respawn_request(&server));
	assert(server.player.health == MAX_PLAYER_HEALTH);
	/* fresh body at world spawn (+0.5 centering, eye height) */
	assert(server.player.x == 8.5 && server.player.z == 8.5
		   && server.player.y == 70 + PLAYER_EYE_HEIGHT);
	assert(!server.player.has_death_pos); /* latch consumed */
	assert(server.reject_player_pos_this_update);
	assert(server.player.finished_loading == false); /* loading epoch */
	assert(server.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	/* the hotbar stack materialized at the DEATH point -- not the corpse's
	 * drifted position, not the spawn */
	/* G6: Beta spawns the death drop at posY - 0.3F + getEyeHeight() 0.12F
	 * [EntityPlayer.java:296]; posY is the eye basis (70.0 here). */
	const double drop_y = 70.0 - (double)0.3F + (double)0.12F;
	assert(count_items_near(&server, 5.5, drop_y, 6.5, 0.01) == 1);
	assert(count_items_near(&server, 12.5, drop_y, 13.5, 0.01) == 0);
	assert(count_items_near(&server, 8.5, drop_y, 8.5, 0.01) == 0);
	{
		struct item_data held;
		bool has = inventory_get_slot(&server.player.inventory,
									  INVENTORY_SLOT_HOTBAR, &held);
		assert(!has || held.id == 0); /* slot cleared by the transaction */
	}
	printf("S3 drops-at-death-position OK\n");

	/* ---- S4: respawn while alive is a no-op ---------------------------- */
	server.player.finished_loading = true;
	server.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	assert(!server_local_respawn_request(&server));
	assert(server.player.health == MAX_PLAYER_HEALTH);
	printf("S4 stale-respawn OK\n");

	/* ---- S5: respawn gated while a switch is in flight ----------------- */
	/* Harness-only: the phase is forced by hand here to isolate the gate;
	 * production only ever enters a phase through world_switch_begin. */
	assert(server_local_set_player_health(&server, 0));
	server.world_switch_phase = SERVER_WORLD_SWITCH_PORTAL;
	assert(!server_local_respawn_request(&server));
	assert(server.player.health <= 0);
	server.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	assert(server_local_respawn_request(&server));
	assert(server.player.health == MAX_PLAYER_HEALTH);
	server.player.finished_loading = true;
	server.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	printf("S5 switch-gate OK\n");

	/* ---- S6: bed resolves at RESPAWN time (the marquee) ----------------- */
	/* A valid bed head at (12,70,12) on a stone pad. */
	set_fixture_pad(&server, 11, 13, 11, 13, 70);
	set_fixture_block(&server, 12, 70, 12, BLOCK_BED, 0);
	server.player.has_bed_spawn = true;
	server.player.bed_spawn_x = 12;
	server.player.bed_spawn_y = 70;
	server.player.bed_spawn_z = 12;
	assert(server_local_set_player_health(&server, 0));
	/* The house burns down while the death screen is up: */
	set_fixture_block(&server, 12, 70, 12, 0, 0);
	assert(server_local_respawn_request(&server));
	assert(server.player.health == MAX_PLAYER_HEALTH);
	/* Beta Minecraft.respawn resolves NOW: invalid -> world spawn, bed
	 * spawn permanently cleared (Minecraft.java:1436-1463). The old
	 * death-time resolution would have landed at the stale bed pad. */
	assert(server.player.x == 8.5 && server.player.z == 8.5);
	assert(!server.player.has_bed_spawn);
	server.player.finished_loading = true;
	server.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	printf("S6 bed-at-respawn (stale bed diverts) OK\n");

	/* ---- S7: the valid-bed half ---------------------------------------- */
	set_fixture_block(&server, 12, 70, 12, BLOCK_BED, 0);
	server.player.has_bed_spawn = true;
	server.player.bed_spawn_x = 12;
	server.player.bed_spawn_y = 70;
	server.player.bed_spawn_z = 12;
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	/* BlockBed.getNearestEmptyChunkCoordinates head-centered 3x3, x outer,
	 * z inner: first candidate (11,70,11) is solid-below + air + air. */
	assert(server.player.x == 11.5 && server.player.z == 11.5);
	assert(server.player.has_bed_spawn); /* valid bed spawn is retained */
	server.player.finished_loading = true;
	server.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	server.player.has_bed_spawn = false;
	printf("S7 bed-valid-cell OK\n");

	/* ---- S8: quit-while-dead persists; a dead load can respawn ---------- */
	inventory_set_slot(&server.player.inventory, INVENTORY_SLOT_HOTBAR,
					   (struct item_data) {.id = 4, .count = 3});
	server.player.x = 9.5; /* die away from spawn so origins differ */
	server.player.y = 70.0;
	server.player.z = 10.5;
	assert(server_local_set_player_health(&server, 0));
	server_local_unload_world_impl(&server, true); /* Title menu / quit */
	{
		/* level.dat carries the dead state (Beta: health<=0 round-trips;
		 * runTick reopens GuiGameOver on the next load). */
		string_t name;
		string_init_set_str(name, WORLD_ROOT);
		struct level_archive level;
		assert(level_archive_create(&level, name));
		int16_t disk_health = 99;
		assert(level_archive_read(&level, LEVEL_PLAYER_HEALTH, &disk_health,
								  0));
		assert(disk_health <= 0);
		level_archive_destroy(&level);
		string_clear(name);
	}
	discard_server(&server);

	struct server_local reloaded;
	init_server_ex(&reloaded, WORLD_ROOT, true);
	assert(reloaded.player.health <= 0); /* loads dead */
	assert(!reloaded.player.has_death_pos);
	/* The production load restores the saved corpse position; the hand-init
	 * stands in for it here. The latch is empty, so the respawn falls back
	 * to this live position as the drop origin (the documented
	 * quit-while-dead approximation). */
	reloaded.player.x = 9.5;
	reloaded.player.y = 70.0;
	reloaded.player.z = 10.5;
	assert(server_local_respawn_request(&reloaded));
	assert(reloaded.player.health == MAX_PLAYER_HEALTH);
	assert(reloaded.player.x == 8.5 && reloaded.player.z == 8.5);
	assert(count_items_near(&reloaded, 9.5, 70.0 - (double)0.3F + (double)0.12F,
							10.5, 0.01) == 1); /* G6: Beta drop spawn y */
	printf("S8 quit-while-dead round-trip OK\n");

	discard_server(&reloaded);

	/* S9: dispatch a real solo precommit failure; preserve the corpse and
	 * crafting owner so its death UI can retry Respawn or Save and quit. */
	remove_world_dirs(WORLD_ROOT);make_world_dirs(WORLD_ROOT);
	create_level_with_hotbar(WORLD_ROOT,4);
	struct server_local refused;
	init_server(&refused,WORLD_ROOT);
	assert(server_local_set_player_health(&refused,0));
	inventory_set_slot(&refused.player.inventory,INVENTORY_SLOT_CRAFTING,
		(struct item_data){.id=4,.count=2});
	struct server_rpc respawn={.type=SRPC_RESPAWN,
		.payload.respawn={.owner=0,.generation=0,.respawn_epoch=0}};
	clin_test_set_rpc_observer(respawn_reply_observer,NULL);
	server_local_test_fail_next_entity_alloc();
	server_local_test_process_rpc(&refused,&respawn);
	assert(retry_replies==1);
	assert(refused.player.health==0&&refused.player.has_pos
		&&refused.player.finished_loading
		&&refused.world_switch_phase==SERVER_WORLD_SWITCH_IDLE);
	struct item_data retained;
	assert(inventory_get_slot(&refused.player.inventory,INVENTORY_SLOT_CRAFTING,&retained)
		&&retained.id==4&&retained.count==2);
	assert(!count_items_total(&refused));
	/* A portal barrier refuses the initial request without silently owning
	 * it. Both menus can retry when the authoritative handoff ends. */
	refused.world_switch_phase=SERVER_WORLD_SWITCH_PORTAL;
	server_local_test_process_rpc(&refused,&respawn);
	assert(retry_replies==2&&refused.player.health==0);
	struct server_rpc unload={.type=SRPC_UNLOAD_WORLD};
	server_local_test_process_rpc(&refused,&unload);
	assert(return_replies==1&&refused.player.has_pos
		&&refused.world_switch_phase==SERVER_WORLD_SWITCH_PORTAL);
	assert(inventory_get_slot(&refused.player.inventory,INVENTORY_SLOT_CRAFTING,&retained)
		&&retained.id==4&&retained.count==2&&!count_items_total(&refused));
	/* Invalid/stale request identities do not manufacture retry replies. */
	struct server_rpc invalid=respawn;invalid.payload.respawn.owner=1;
	server_local_test_process_rpc(&refused,&invalid);
	invalid=respawn;invalid.payload.respawn.generation=1;
	server_local_test_process_rpc(&refused,&invalid);
	invalid=respawn;invalid.payload.respawn.respawn_epoch=1;
	server_local_test_process_rpc(&refused,&invalid);
	assert(retry_replies==2);
	refused.world_switch_phase=SERVER_WORLD_SWITCH_IDLE;
	refused.player.finished_loading=false;
	server_local_test_process_rpc(&refused,&respawn);
	assert(retry_replies==3&&refused.player.health==0);
	refused.player.finished_loading=true;
	server_local_test_process_rpc(&refused,&respawn);
	assert(retry_replies==3&&refused.player.health==MAX_PLAYER_HEALTH
		&&refused.world_switch_phase==SERVER_WORLD_SWITCH_DEATH);
	/* The accepted death handoff owes completion, not a refusal: a stale
	 * duplicate must not retire the original request's UI owner. */
	refused.player.health=0;
	server_local_test_process_rpc(&refused,&respawn);
	assert(retry_replies==3);
	refused.world_switch_phase=SERVER_WORLD_SWITCH_IDLE;
	refused.death_world_evict=true;
	server_local_test_process_rpc(&refused,&respawn);assert(retry_replies==3);
	refused.death_world_evict=false;refused.host_respawn_phase=HOST_RESPAWN_PHASE_LOADING;
	server_local_test_process_rpc(&refused,&respawn);assert(retry_replies==3);
	refused.host_respawn_phase=HOST_RESPAWN_PHASE_IDLE;
	refused.player.health=MAX_PLAYER_HEALTH;
	/* An already alive/stale press never reports a failure. */
	refused.player.finished_loading=true;
	refused.world_switch_phase=SERVER_WORLD_SWITCH_IDLE;
	server_local_test_process_rpc(&refused,&respawn);
	assert(retry_replies==3&&refused.player.health==MAX_PLAYER_HEALTH);
	clin_test_set_rpc_observer(NULL,NULL);
	discard_server(&refused);
	printf("S9 solo dispatch refusal acknowledgement, retained crafting, retry and stale press OK\n");
	remove_world_dirs(WORLD_ROOT);

	drain_run = 0;
	thread_join(&drain);
	printf("test_death_screen_env: ALL OK\n");
	return 0;
}
