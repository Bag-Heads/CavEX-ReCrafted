/*
	CaveWii_v2 CAVEWII_AGENT bridge -- live state-export collector (see header).

	The engine-side half of the perceive->act bridge: fills a game_export_state from
	gstate and writes one JSON line (state_export_write, the scorer-shared pure writer)
	to stdout. PC dev rig ONLY (PLATFORM_PC) and ARMED by env (CAVEWII_AGENT=1), so a
	normal build/run is byte-identical -- nothing here runs unless explicitly enabled.

	Compiled into cavewii_v2.exe only (CMake target_sources after the _cavex_srcs
	capture), so the engine-glue below never reaches the pure unit-test exes.
*/

#ifdef PLATFORM_PC

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../block/blocks_data.h"
#include "../entity/entity.h"
#include "../item/inventory.h"
#include "../item/items.h"
#include "../item/window_container.h"
#include "../network/server_local.h"
#include "../world.h"
#include "camera.h"
#include "game_state.h"
#include "gui/screen.h"
#include "state_export.h"
#include "state_export_emit.h"

#define WFX_RAD2DEG (180.0f / 3.14159265358979323846f)

// struct screen has no name field, so map the active screen pointer to a stable name.
static const char* screen_name(const struct screen* s) {
	if(!s)
		return "none";
	if(s == &screen_ingame)
		return "ingame";
	if(s == &screen_death)
		return "death";
	if(s == &screen_inventory)
		return "inventory";
	if(s == &screen_crafting)
		return "crafting";
	if(s == &screen_furnace)
		return "furnace";
	if(s == &screen_chest)
		return "chest";
	if(s == &screen_iron_chest)
		return "iron_chest";
	if(s == &screen_sign)
		return "sign";
	if(s == &screen_pause)
		return "pause";
	if(s == &screen_options)
		return "options";
	if(s == &screen_select_world)
		return "select_world";
	if(s == &screen_load_world)
		return "load_world";
	if(s == &screen_texturepacks)
		return "texturepacks";
	return "unknown";
}

// -1 = not yet checked, 0 = off, 1 = armed. Cached so the env is read once.
static int s_armed = -1;

// OBSERVE-ONLY arming (CAVEWII_TRACE=1, 2026-09-04, keepup walker lane).
// CAVEWII_AGENT=1 arms BOTH halves of the bridge: this emitter (perceive) and
// agent_input.c (act). agent_input's hooks sit AHEAD of autopilot_force_button
// in input.c's input_held chain and own IB_FORWARD/IB_JUMP unconditionally
// once armed, so a run started with CAVEWII_AGENT=1 to *observe* an autopilot
// profile silently pins the player at spawn -- the walk driver's forced
// buttons never reach the movement code at all (measured: 1165 ticks at
// x=0.50,z=0.50 on a keepup_walk_6m run). That made the agent bridge unusable
// as a measurement instrument for exactly the walk profiles it fits best.
// CAVEWII_TRACE=1 arms ONLY this emitter; agent_input.c keeps its own,
// unchanged CAVEWII_AGENT gate, so an armed hw_run profile can be traced with
// no input hijack. Both vars unset (the default, and every shipped run) is
// byte-identical to before.
bool state_export_agent_armed(void) {
	if(s_armed < 0) {
		const char* v = getenv("CAVEWII_AGENT");
		const char* t = getenv("CAVEWII_TRACE");
		s_armed = ((v && v[0] == '1' && v[1] == '\0')
				   || (t && t[0] == '1' && t[1] == '\0'))
			? 1
			: 0;
	}
	return s_armed == 1;
}

void state_export_emit(void) {
	if(!state_export_agent_armed())
		return;

	static uint64_t tick = 0;

	struct game_export_state st;
	memset(&st, 0, sizeof(st));
	st.schema_version = STATE_EXPORT_SCHEMA_VERSION;
	st.tick = tick++;
	st.screen = screen_name(gstate.players[0].current_screen);
	st.has_player = gstate.world_loaded && gstate.players[0].local_player != NULL;

	if(st.has_player) {
		struct entity* p = gstate.players[0].local_player;
		st.px = p->pos[0];
		st.py = p->pos[1];
		st.pz = p->pos[2];
		st.yaw = p->orient[0] * WFX_RAD2DEG; // engine stores radians; schema is degrees
		st.pitch = p->orient[1] * WFX_RAD2DEG;
		st.on_ground = p->on_ground;
		st.health = p->health;
		st.in_water = gstate.players[0].in_water;
		st.oxygen = gstate.players[0].oxygen;
		struct server_local_host_weather_diagnostic weather;
		server_local_host_weather_diagnostic_request();
		(void)server_local_host_weather_diagnostic_read(&weather);
		st.wet = weather.wet;
		st.fire = weather.fire_ticks;
		st.rained_on = weather.rained_on;
		st.height = weather.height;
		st.biome_rains = weather.biome_rains;
		st.fx = weather.fx;
		st.fy = weather.fy;
		st.fz = weather.fz;

		// Selected hotbar slot/item/count from the live inventory window.
		st.hotbar_slot = -1;
		st.hotbar_item_id = -1;
		st.hotbar_count = 0;
		struct window_container* wc = gstate.players[0].windows[WINDOWC_INVENTORY];
		if(wc) {
			struct inventory* inv = windowc_get_latest(wc);
			if(inv) {
				st.hotbar_slot = (int)inventory_get_hotbar(inv);
				struct item_data it;
				if(inventory_get_hotbar_item(inv, &it)) {
					st.hotbar_item_id = (int)it.id;
					st.hotbar_count = (int)it.count;
				}
			}
		}

		// look: the engine's last crosshair raycast (camera_ray_result has no block
		// field, so resolve block_id from the hit coords ourselves).
		struct camera_ray_result* h = &gstate.players[0].camera_hit;
		st.look.hit = h->hit;
		st.look.entity_hit = h->entity_hit;
		st.look.entity_id = h->entity_hit ? (int)h->entity_id : -1;
		if(h->hit) {
			st.look.x = (int)h->x;
			st.look.y = (int)h->y;
			st.look.z = (int)h->z;
			st.look.side = (int)h->side;
			st.look.block_id
				= world_get_block(&gstate.world, h->x, h->y, h->z).type;
		} else {
			st.look.x = st.look.y = st.look.z = -1;
			st.look.side = -1;
			st.look.block_id = -1;
		}

		// 5x5 surface-height window, row-major NW origin, relative to the player's
		// own column surface (so the driver reads slopes, not absolute y).
		w_coord_t pcx = (w_coord_t)floorf(p->pos[0]);
		w_coord_t pcz = (w_coord_t)floorf(p->pos[2]);
		w_coord_t base = world_get_height(&gstate.world, pcx, pcz);
		int n = 0, half = STATE_EXPORT_HEIGHT_WINDOW / 2;
		for(int dz = -half; dz <= half; dz++)
			for(int dx = -half; dx <= half; dx++)
				st.heightmap[n++]
					= (int)(world_get_height(&gstate.world, pcx + dx, pcz + dz)
							- base);
		st.has_heightmap = true;
	}

	char buf[1024];
	state_export_write(&st, buf, sizeof(buf));
	printf("%s\n", buf);
	fflush(stdout);
}

#endif // PLATFORM_PC
