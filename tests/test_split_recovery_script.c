/* split_recovery_6m phase driver. GPL-3.0-or-later.
 *
 * Contract: docs/split_recovery_profile.md. The cases below drive
 * source/game/split_recovery_script.c through the PRODUCTION handlers it
 * exists to exercise -- local_menu_host_recovery_frame (the real
 * controller_recovery_step/commit pair), local_pause_update/
 * local_pause_host_exit_settled and client_session_local_status (the real
 * guest-leave lifetime) -- with a scripted world/server feed standing in for
 * the parts a PC test has no server for.
 *
 * WHAT THIS TEST DOES NOT PROVE, stated rather than implied:
 *   - main.c's seam. `apply_actions` below replicates the three thin call
 *     sites main.c uses (owned-frame substitution, recovery-before-pause
 *     order, the SRPC_UNLOAD_WORLD enqueue). If those hunks drift, this test
 *     still passes -- the real proof of the seam is the hardware profile.
 *   - the server. Block edits, admissions, leave acks and the world reload are
 *     scripted here; what a real server does with them is what
 *     split_recovery_6m measures on a Wii.
 *   - main.c static local_pause_frame seam (the IB_HOME open, and the
 *     SRPC_UNLOAD_WORLD enqueue that follows LOCAL_PAUSE_HOST_EXIT) and the
 *     recovery-first gate in the owner-menu loop (main.c ~3954: the recovery
 *     consumer runs BEFORE the pause, so a controller lost with a pause open
 *     cannot be clicked through). apply_actions below re-creates that ORDER,
 *     but nothing here compiles main.c, so a drift in those hunks is invisible
 *     to this test and is caught only by the real-Wii run.
 * All three are why this profile exists on hardware at all. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "game/hw_run.h"
#include "game/split_layout.h"
#include "game/split_recovery_script.h"
#include "game/split_portal_quit_script.h"
#include "item/inventory.h"
#include "item/items.h"
#include "item/window_container.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "platform/input_owner.h"
#include "platform/menu_sound.h"
#include "particle.h"
#include "world.h"

void __wrap_audio_menu_event(enum menu_sound_event event, ptime_t now) {
	(void)event;
	(void)now;
}

#define RIG_MAX_LINES 96
#define RIG_MAX_CELLS 16
#define RIG_FRAME_MS 50u /* the 20 fps floor the driver tolerances assume */
/* Comfortably past the driver 10 s bounded wait for a placed block. */
#define SR_RIG_BLOCK_WAIT_FRAMES 800u

struct rig_cell {
	int x, y, z;
	uint32_t id, meta;
};

static struct rig {
	struct client_session host, guest;
	struct entity host_body, guest_body;
	int stack[2]; /* the rig model of each actor slot-0 dirt stack */
	struct split_layout layout;
	struct split_recovery_actions act;

	uint32_t now_ms;
	uint32_t sim_ticks;
	uint32_t epoch;
	bool world_loaded, world_switch_pending, at_title, host_present;
	unsigned view_count;

	struct rig_cell cells[RIG_MAX_CELLS];
	unsigned cell_count;

	char lines[RIG_MAX_LINES][768];
	unsigned line_count;
	char done[128];
	bool done_sent;

	/* knobs the failure cases turn */
	bool emit_ok, create_ok, autoload_ok, rpc_ok, unload_enqueue_ok;
	bool admit_guest, reload_world, title_early;
	bool starve_host_pickup, starve_guest_pickup, empty_baseline;
	bool blocks_not_resident;	 /* H1: the column has not streamed in yet */
	bool reload_owner_residency; /* R8: only admitted owners stream their ring */
	unsigned reload_remote_waits;
	bool emit_backpressure_at_autoload;
	const char* autoload_conflict;	/* M5 */
	char toast[48];				 /* M8: the server own words, escaped or not */
	unsigned autoload_calls;	 /* H2: arm_autoload must be a one-shot */
	unsigned autoload_emit_tries;
	unsigned host_digs, guest_digs;
	/* 2026-09-17 real-app lessons: only OWN-column digs are deterministic, the
	 * guest places two blocks and the host one (so the baselines differ). */
	unsigned foreign_digs, host_places, guest_places;
	bool equal_baselines; /* hand the guest one extra item: 4-2 == 3-1 */
	/* The driver's clock reads this many ms EARLY on the frames where the
	 * collapse has finished (boundary 1.0 while the guest is lost): 1 models
	 * the ms truncation seen on the real Wii, a large value a genuinely early
	 * collapse. */
	uint32_t done_skew_ms;
	/* Refuse the observer line containing this text for this many tries: the
	 * sender ring being full while a TIMED marker is pending. */
	/* After the reload, placed cell 1 reads back as this id instead (0 = off):
	 * BLOCK_GRASS models Beta grass spread onto the placed dirt. */
	uint32_t reload_cell1_id;
	uint32_t reload_cell1_meta; /* metadata served with reload_cell1_id */
	const char* refuse_marker;
	unsigned refuse_tries, refused;
	bool settle_accept, settle_refuse;
	uint64_t next_generation;
	unsigned rpc_count, leave_frames, exit_frames;
} rig;

/* ---- hooks -------------------------------------------------------------- */

/* Move `delta` dirt into or out of an actor slot-0 stack, the way a real
 * pickup or placement would. The rig owns REAL inventories (windowc_create +
 * inventory_set_slot), so the driver reads them through the same production
 * accessors it uses on a console. */
static void rig_write_stack(unsigned actor) {
	struct client_session* s = actor ? &rig.guest : &rig.host;
	if(!s->windows[WINDOWC_INVENTORY])
		return;
	struct item_data item = {0};
	if(rig.stack[actor] > 0) {
		item.id = BLOCK_DIRT;
		item.count = (uint8_t)rig.stack[actor];
	}
	inventory_set_slot(windowc_get_latest(s->windows[WINDOWC_INVENTORY]), 0,
					   item);
}

static void rig_stack(unsigned actor, int delta) {
	rig.stack[actor] += delta;
	if(rig.stack[actor] < 0)
		rig.stack[actor] = 0;
	rig_write_stack(actor);
}

/* The session teardown paths free(session->windows[k]) -- a real window is a
 * heap object -- so the rig hands them heap ones and re-attaches a fresh
 * window (carrying the actor saved stack, the way a server restores a
 * rejoining or reloading player inventory) whenever one has been torn down. */
static void rig_attach_window(unsigned actor) {
	struct client_session* s = actor ? &rig.guest : &rig.host;
	if(s->windows[WINDOWC_INVENTORY])
		return;
	struct window_container* wc = malloc(sizeof(*wc));
	assert(wc && windowc_create(wc, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
	s->windows[WINDOWC_INVENTORY] = wc;
	rig_write_stack(actor);
}

static bool hook_rpc_owner(unsigned actor, struct server_rpc* call) {
	if(!rig.rpc_ok)
		return false;
	rig.rpc_count++;
	/* The scripted server. A finished dig of the body OWN column drops it one
	 * block (superflat has three diggable layers) and, like the real one,
	 * lands the block in the digger inventory a moment later; a place takes
	 * one back out and lands a dirt block on top of the clicked cell. Nothing
	 * else is modelled -- this is a feed, not a server. */
	struct client_session* s = actor ? &rig.guest : &rig.host;
	if(call->type == SRPC_BLOCK_DIG && call->payload.block_dig.finished) {
		if((int)floorf(s->camera.x) == call->payload.block_dig.x
		   && (int)floorf(s->camera.z) == call->payload.block_dig.z)
			s->camera.y -= 1.0F;
		else
			rig.foreign_digs++;
		if(actor ? !rig.starve_guest_pickup : !rig.starve_host_pickup) {
			if(actor)
				rig.guest_digs++;
			else
				rig.host_digs++;
			rig_stack(actor, 1);
			if(actor && rig.equal_baselines
			   && rig.guest_digs == SPLIT_RECOVERY_GUEST_DIGS)
				rig_stack(actor, 1);
		}
	} else if(call->type == SRPC_BLOCK_PLACE) {
		if(actor)
			rig.guest_places++;
		else
			rig.host_places++;
		rig_stack(actor, -1);
		if(rig.cell_count < RIG_MAX_CELLS)
			rig.cells[rig.cell_count++] = (struct rig_cell) {
				call->payload.block_place.x, call->payload.block_place.y + 1,
				call->payload.block_place.z, BLOCK_DIRT, 0};
	}
	return true;
}

static bool hook_rpc(void* user, struct server_rpc* call) {
	(void)user;
	return hook_rpc_owner(1, call);
}

static bool hook_rpc_host(void* user, struct server_rpc* call) {
	(void)user;
	return hook_rpc_owner(0, call);
}

static bool hook_emit(void* user, const char* line) {
	(void)user;
	/* Refuse the autoload_armed line a few times: P4 step 0 comes back next
	 * frame and must not arm the hook again (H2). */
	if(rig.emit_backpressure_at_autoload && strstr(line, "autoload_armed")
	   && rig.autoload_calls && ++rig.autoload_emit_tries <= 5)
		return false;
	if(rig.refuse_marker && strstr(line, rig.refuse_marker)
	   && rig.refused < rig.refuse_tries) {
		rig.refused++;
		return false;
	}
	if(!rig.emit_ok || rig.line_count >= RIG_MAX_LINES)
		return false;
	snprintf(rig.lines[rig.line_count++], sizeof(rig.lines[0]), "%s", line);
	return true;
}

static void hook_done(void* user, const char* status) {
	(void)user;
	assert(!rig.done_sent); /* exactly one verdict per run */
	rig.done_sent = true;
	snprintf(rig.done, sizeof(rig.done), "%s", status);
}

static bool hook_create(void* user, const char* directory) {
	(void)user;
	assert(!strcmp(directory, SPLIT_RECOVERY_RIG_WORLD));
	if(!rig.create_ok)
		return false;
	rig.world_loaded = rig.host_present = true;
	return true;
}

static bool hook_autoload(void* user, const char* directory) {
	(void)user;
	assert(!strcmp(directory, SPLIT_RECOVERY_RIG_WORLD));
	rig.autoload_calls++;
	if(!rig.autoload_ok)
		return false;
	rig.reload_world = true;
	return true;
}

static const char* hook_autoload_conflict(void* user) {
	(void)user;
	return rig.autoload_conflict;
}

/* Returning FALSE is "this column is not resident", which is the whole point
 * of H1: world_get_block would answer id 1 (STONE) there, and the driver must
 * WAIT rather than read that as a real block. */
static bool hook_block(void* user, int x, int y, int z, uint32_t* id,
					   uint32_t* meta) {
	(void)user;
	*id = 0;
	*meta = 0;
	if(rig.blocks_not_resident)
		return false;
	/* R8 recovery tape: blocks 0/1 at z=43, host at z=-40.51.
	 * The existing physics double walks along X, but preserves that >80-block
	 * separation. A world reload discards the guest ring until readmission.
	 * This is a disclosed residency feed, not a server implementation. */
	if(rig.reload_owner_residency && rig.epoch > 11
	   && (abs(x / 16 - (int)floorf(rig.host.camera.x / 16)) > 4
		   || abs(z / 16 - (int)floorf(rig.host.camera.z / 16)) > 4)
	   && !rig.guest.local_gameplay_ready) {
		rig.reload_remote_waits++;
		return false;
	}
	for(unsigned k = 0; k < rig.cell_count; k++)
		if(rig.cells[k].x == x && rig.cells[k].y == y
		   && rig.cells[k].z == z) {
			*id = rig.cells[k].id;
			*meta = rig.cells[k].meta;
			if(k == 1 && rig.reload_cell1_id && rig.epoch > 11) /* rig_init starts the epoch at 11; the reload bumps it */
			{
				*id = rig.reload_cell1_id;
				*meta = rig.reload_cell1_meta;
			}
			return true;
		}
	return true; /* resident, and empty */
}

static const struct split_recovery_hooks hooks = {
	.rpc_guest = hook_rpc, .rpc_host = hook_rpc_host, .emit = hook_emit,
	.profile_done = hook_done,
	.create_world = hook_create, .arm_autoload = hook_autoload,
	.block_at = hook_block, .autoload_conflict = hook_autoload_conflict};

/* ---- the rig ------------------------------------------------------------ */

static void admit_guest(void) {
	rig_attach_window(1);
	rig.guest.local_ticket = 7;
	rig.guest.local_generation = ++rig.next_generation;
	rig.guest.local_bound = true;
	rig.guest.local_phase = CLIENT_GUEST_ATTACHED;
	rig.guest.local_gameplay_ready = true;
	rig.guest.local_player = &rig.guest_body;
	rig.view_count = 2;
}

static void retire_guest(void) {
	rig.guest.local_gameplay_ready = false;
	rig.guest.local_player = NULL;
	rig.view_count = 1;
}

static void rig_init(void) {
	memset(&rig, 0, sizeof(rig));
	split_recovery_reset();
	for(unsigned k = 0; k < 2; k++)
		rig_attach_window(k);
	rig.emit_ok = rig.create_ok = rig.autoload_ok = rig.rpc_ok = true;
	rig.unload_enqueue_ok = rig.admit_guest = true;
	rig.settle_accept = true;
	rig.autoload_conflict = NULL;
	rig.epoch = 11;
	rig.sim_ticks = 1000;
	rig.view_count = 1;
	rig.host_body.id = 1;
	rig.guest_body.id = 3;
	rig.host.local_player = &rig.host_body; /* the host body is always present */
	rig.host.camera.x = rig.guest.camera.x = 0.5F;
	rig.host.camera.y = rig.guest.camera.y = 5.62F; /* feet 4, floor 3 */
	rig.host.camera.z = rig.guest.camera.z = 0.5F;
	split_layout_reset(&rig.layout);
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	hw_run_apply_profile(&cfg, HW_RUN_PROFILE_SPLIT_RECOVERY_6M);
	cfg.enabled = true;
	split_recovery_arm(&cfg, &hooks);
	assert(split_recovery_armed());
}

/* The three thin call sites main.c owns, replicated in main.c's own order:
 * the recovery consumer first (it may gate everything else), then the pause. */
static void apply_actions(void) {
	if(rig.act.guest_frame_valid) {
		bool gated = local_menu_host_recovery_frame(
			&rig.guest, 1, &rig.act.guest_frame, 0, 0, 640, 240);
		if(!gated) {
			if(!local_pause_active(&rig.guest.pause)) {
				if(rig.act.guest_frame.connected
				   && rig.act.guest_frame.pressed[IB_HOME])
					local_pause_open(&rig.guest.pause, false);
			} else
				(void)local_pause_update(&rig.guest.pause, &rig.guest,
										 &rig.act.guest_frame, 0, 0, 640, 240);
		}
	}
	if(rig.act.host_frame_valid) {
		if(!local_pause_active(&rig.host.pause)) {
			if(rig.act.host_frame.connected
			   && rig.act.host_frame.pressed[IB_HOME])
				local_pause_open(&rig.host.pause, true);
		} else {
			enum local_pause_result r
				= local_pause_update(&rig.host.pause, &rig.host,
									 &rig.act.host_frame, 0, 0, 640, 240);
			if(r == LOCAL_PAUSE_HOST_EXIT && !rig.unload_enqueue_ok)
				local_pause_host_exit_settled(&rig.host.pause, false);
		}
	}
}

/* Everything the scripted server would answer, applied between frames. */
static void apply_server(void) {
	/* Empty the host only AFTER both actors have finished digging, so its own
	 * pickups are still observed and the loss lands exactly where the baseline
	 * is taken -- the vacuous case, not a starved pickup. */
	if(rig.empty_baseline && rig.guest_digs >= SPLIT_RECOVERY_GUEST_DIGS
	   && rig.stack[0] > 0)
		rig_stack(0, -rig.stack[0]);
	if(rig.admit_guest && !rig.act.suppress_autojoin && rig.world_loaded
	   && !rig.guest.local_gameplay_ready
	   && rig.guest.local_phase == CLIENT_GUEST_IDLE)
		admit_guest();
	/* A leave in flight settles through the REAL client status handler, after a
	 * few frames -- a server never answers inside the requesting frame. */
	if(rig.guest.local_phase == CLIENT_GUEST_LEAVING && ++rig.leave_frames > 3) {
		struct client_rpc reply = {
			.type = CRPC_LOCAL_GUEST,
			.payload.local_guest = {.ticket = rig.guest.local_ticket,
									.generation = rig.guest.local_generation,
									.result = LOCAL_GUEST_LEFT}};
		client_session_local_status(&rig.guest, &reply);
		retire_guest();
		rig.leave_frames = 0;
	}
	/* Same for the host exit: SRPC_UNLOAD_WORLD is a REQUEST that crosses to
	 * the server thread and back, so it never settles in its own frame. */
	if(rig.host.pause.exit_pending && ++rig.exit_frames > 3) {
		if(rig.title_early)
			rig.at_title = true;
		else if(rig.settle_refuse) {
			snprintf(rig.host.hud_message, sizeof(rig.host.hud_message), "%s",
					 rig.toast[0] ? rig.toast : "Save failed: inventory close");
			local_pause_host_exit_settled(&rig.host.pause, false);
		} else if(rig.settle_accept) {
			local_pause_host_exit_settled(&rig.host.pause, true);
			rig.world_loaded = rig.host_present = false;
			retire_guest();
			rig.guest.local_phase = CLIENT_GUEST_IDLE;
			rig.at_title = true;
		}
	}
	rig_attach_window(0); /* the host window survives, or is rebuilt if freed */
	if(rig.reload_world && !rig.world_loaded) {
		rig.reload_world = false;
		rig.world_loaded = rig.host_present = true;
		rig.at_title = false;
		rig.epoch++;
	}
}

static void rig_frames(unsigned n) {
	for(unsigned k = 0; k < n; k++) {
		split_recovery_frame(&(struct split_recovery_ctx) {
			.host = &rig.host, .guest = &rig.guest, .layout = &rig.layout,
			.view_count = rig.view_count, .world_loaded = rig.world_loaded,
			.world_switch_pending = rig.world_switch_pending,
			.host_body = rig.host_present, .at_title = rig.at_title,
			.sim_ticks = rig.sim_ticks, .epoch = rig.epoch,
			.now_ms = rig.now_ms
				- ((rig.layout.boundary >= 1.0F
					&& rig.guest.menu_host.recovery.phase
						!= CONTROLLER_RECOVERY_NONE)
					   ? rig.done_skew_ms : 0)}, &rig.act);
		assert(rig.act.host_walk_valid == split_recovery_armed());
		apply_actions();
		apply_server();
		/* Physics is a disclosed double here; real admission is covered by
		 * test_player_residency_admission. Both walks precede baselines. */
		if(rig.act.drive_host_walk) rig.host.camera.x -= 0.25F;
		if(rig.act.drive_guest_walk)
			rig.guest.camera.x += 0.25F;
		bool suspended[2] = {false,
			rig.guest.menu_host.recovery.phase != CONTROLLER_RECOVERY_NONE};
		split_layout_step(&rig.layout, rig.view_count == 2, suspended,
						  RIG_FRAME_MS / 1000.0F);
		rig.now_ms += RIG_FRAME_MS;
		/* The server thread ticks at 20 Hz while the world is loaded, which is
		 * the ONLY thing that moves g_srv_sim_ticks -- a frozen world stops it,
		 * which is what P1 watches for. */
		if(rig.world_loaded)
			rig.sim_ticks++;
	}
}

/* ---- assertions --------------------------------------------------------- */

/* Every failure below is a "the driver stopped somewhere" failure, and a bare
 * assert names neither the phase nor the marker it stopped on. Dump first. */
static void rig_dump(const char* why) {
	printf("-- split_recovery rig (%s): phase %d step %u, seq %u, %u markers, "
		   "done=%d \"%s\"\n",
		   why, (int)split_recovery_phase_now(), split_recovery_step_now(),
		   split_recovery_seq_now(), rig.line_count, (int)rig.done_sent,
		   rig.done);
	for(unsigned k = 0; k < rig.line_count; k++)
		printf("   %s\n", rig.lines[k]);
	fflush(stdout);
}

static void expect_done(const char* status) {
	if(!rig.done_sent || strcmp(rig.done, status))
		rig_dump(status);
	assert(rig.done_sent);
	assert(!strcmp(rig.done, status));
}

static const char* line_with(const char* marker, unsigned nth) {
	char needle[64];
	snprintf(needle, sizeof(needle), "\"marker\":\"%s\"", marker);
	unsigned seen = 0;
	for(unsigned k = 0; k < rig.line_count; k++)
		if(strstr(rig.lines[k], needle) && seen++ == nth)
			return rig.lines[k];
	return NULL;
}

/* 6b: drive the real RPC and the same observer used by main.c. */
static bool spq_emit(const char* line) { return hook_emit(NULL, line); }
static bool spq_arm_portal(void) { return true; }
static bool spq_autoload(const char* name) {
	return strcmp(name, SPLIT_RECOVERY_RIG_WORLD) == 0;
}
static bool spq_block(int x, int y, int z, uint32_t* id, uint32_t* meta) {
	(void)x; (void)y; (void)z;
	*id = y == 4 ? BLOCK_PORTAL : BLOCK_DIRT; *meta = 0; return true;
}
static void spq_done(const char* status) { (void)status; }

static void spq_step(struct split_portal_quit_ctx* c) {
	struct split_portal_quit_actions out;
	c->tick++; c->up_ms += 50;
	split_portal_quit_frame(c, &out);
}

static void test_portal_quit_rpc_race(bool switch_first, bool pre_p3_begin) {
	memset(&rig, 0, sizeof rig);
	rig.emit_ok = true;
	rig_attach_window(0); rig_attach_window(1);
	rig.stack[0] = 3; rig.stack[1] = 2;
	rig_write_stack(0); rig_write_stack(1);
	rig.host.local_player = &rig.host_body;
	rig.guest.local_player = &rig.guest_body;
	rig.guest.local_gameplay_ready = true;
	rig.host.camera.x = rig.host.camera.z = 0.5f;
	rig.host.camera.y = 5.62f;
	rig.guest.camera.x = 12.5f;
	rig.guest.camera.z = 0.5f;
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	hw_run_apply_profile(&cfg, HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M);
	cfg.enabled = true;
	split_portal_quit_arm(&cfg, &(struct split_portal_quit_hooks){
		.emit = spq_emit, .arm_portal = spq_arm_portal,
		.arm_autoload = spq_autoload, .block_at = spq_block,
		.saved_block_at = spq_block, .done = spq_done});
	assert(split_portal_quit_armed());
	#ifdef CAVEWII_WORLD_SWITCH_BEGIN_SEQ
	uint32_t last_seen = gstate.world_switch_begin_seq; /* arm snapshot */
	if(pre_p3_begin) {
		struct client_rpc begin = {.type = CRPC_BEGIN_WORLD_SWITCH};
		clin_process(&begin);
		assert(gstate.world_switch_begin_seq == last_seen + 1);
	}
	/* P2 -> P3 transition, matching main.c's post-frame hook. */
	split_portal_quit_p3_enter(&last_seen, gstate.world_switch_begin_seq);
	#else
	(void)pre_p3_begin;
	#endif
	for(int i = 0; i < SPLIT_RECOVERY_BLOCKS; i++) {
		char line[160];
		snprintf(line, sizeof line,
			"{\"marker\":\"block_placed\",\"i\":%d,\"x\":%d,\"y\":3,\"z\":0,\"id\":3,\"meta\":0}", i, i);
		split_portal_quit_capture_recovery_marker(line);
	}
	struct split_portal_quit_ctx c = {
		.host = &rig.host, .guest = &rig.guest,
		.world_loaded = true, .dimension = 0, .tick = 100,
		.up_ms = 1000};
	spq_step(&c); /* baseline */
	spq_step(&c); /* HOME */
	rig.host.pause.open = true;
	spq_step(&c); /* host pause opens */
	spq_step(&c); /* Save selection */
	rig.host.pause.selected = 3;
	spq_step(&c); /* confirm */
	rig.host.pause.page = LOCAL_PAUSE_HOST_CONFIRM;
	rig.host.pause.selected = 1;
	spq_step(&c); /* select confirm */
	spq_step(&c); /* portal armed */
	spq_step(&c); /* portal lit */
	c.portal_progress = 1.0f;
	spq_step(&c); /* dwell trigger */
	assert(line_with("dwell_trigger", 0));
	split_portal_quit_confirm_sent(c.tick, c.up_ms);
	spq_step(&c); /* quit request */
	c.unload_requests = 1;
	spq_step(&c); /* quit_requested, case 9 next */
	assert(line_with("quit_requested", 0));
	if(switch_first) {
		struct client_rpc begin = {.type = CRPC_BEGIN_WORLD_SWITCH};
#ifdef CAVEWII_WORLD_SWITCH_BEGIN_SEQ
		uint32_t before = gstate.world_switch_begin_seq;
#endif
		clin_process(&begin);
#ifdef CAVEWII_WORLD_SWITCH_BEGIN_SEQ
		assert(gstate.world_switch_begin_seq == before + 1);
#endif
		#ifdef CAVEWII_WORLD_SWITCH_BEGIN_SEQ
		split_portal_quit_observe_switch_begin(&last_seen,
			gstate.world_switch_begin_seq, c.tick + 1, c.up_ms + 1);
		#else
		split_portal_quit_switch_begin(c.tick + 1, c.up_ms + 1);
		#endif
		assert(line_with("switch_begin", 0));
		spq_step(&c);
		assert(line_with("race_switch_first", 0));
		/* Case 10 captures a new position before title settlement. */
		rig.host.camera.x = 2.5f;
		rig.host.camera.y = 6.62f;
		rig.host.camera.z = 1.5f;
		c.world_loaded = true; c.world_switch_pending = false;
		spq_step(&c);
		#ifdef CAVEWII_WORLD_SWITCH_BEGIN_SEQ
		float x = 0, y = 0, z = 0;
		assert(split_portal_quit_post_switch_position(&x, &y, &z));
		assert(x == rig.host.camera.x && y == rig.host.camera.y
			&& z == rig.host.camera.z);
		#endif
		c.world_loaded = false; c.at_title = true;
		rig.host.pause.exit_pending = false;
		spq_step(&c); /* save_settled */
		spq_step(&c); /* title_entered */
		spq_step(&c); /* autoload */
		c.world_loaded = true; c.at_title = false;
		spq_step(&c); /* case 13 compares against captured position */
		const char* verified = line_with("reload_verified", 0);
		assert(verified && strstr(verified, "\"position_match\":true"));
		assert(!line_with("fail", 0));
	} else {
#ifdef CAVEWII_WORLD_SWITCH_BEGIN_SEQ
		uint32_t before = gstate.world_switch_begin_seq;
#endif
		struct client_rpc reset = {.type = CRPC_WORLD_RESET,
			.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD,
				.local_entity = 0}};
		gstate.players[0].current_screen = &screen_ingame;
		gstate.players[0].pause.open = true;
		gstate.players[0].pause.host = true;
		gstate.players[0].pause.exit_pending = true;
		gstate.world_loaded = true;
		clin_process(&reset);
		assert(gstate.world_switch_pending);
		assert(gstate.players[0].current_screen == &screen_title);
#ifdef CAVEWII_WORLD_SWITCH_BEGIN_SEQ
		assert(gstate.world_switch_begin_seq == before);
#endif
		#ifdef CAVEWII_WORLD_SWITCH_BEGIN_SEQ
		split_portal_quit_observe_switch_begin(&last_seen,
			gstate.world_switch_begin_seq, c.tick + 1, c.up_ms + 1);
		assert(!line_with("switch_begin", 0));
		#endif
		c.world_loaded = false;
		c.world_switch_pending = gstate.world_switch_pending;
		c.at_title = gstate.players[0].current_screen == &screen_title;
		rig.host.pause.exit_pending = false;
		spq_step(&c);
		assert(line_with("race_unload_first", 0));
		assert(!line_with("race_switch_first", 0));
		spq_step(&c); /* save_settled */
		spq_step(&c); /* title_entered */
		spq_step(&c); /* autoload */
		c.world_loaded = true; c.world_switch_pending = false;
		spq_step(&c); /* reload_verified */
		assert(line_with("save_settled", 0));
		assert(line_with("title_entered", 0));
		const char* verified = line_with("reload_verified", 0);
		assert(verified && strstr(verified, "\"position_match\":true"));
		assert(!line_with("fail", 0));
	}
	client_session_clear_windows(&rig.host);
	client_session_clear_windows(&rig.guest);
}

static unsigned field_u(const char* line, const char* key) {
	char needle[48];
	snprintf(needle, sizeof(needle), "\"%s\":", key);
	const char* at = strstr(line, needle);
	assert(at);
	unsigned v = 0;
	sscanf(at + strlen(needle), "%u", &v);
	return v;
}

/* seq is gapless from 1, every line carries the fixed prefix in order, and
 * nothing follows run.ok. */
static void assert_sequence(void) {
	for(unsigned k = 0; k < rig.line_count; k++) {
		assert(!strncmp(rig.lines[k], "{\"event\":\"split_recovery\",\"seq\":",
						31));
		assert(field_u(rig.lines[k], "seq") == k + 1);
		assert(strstr(rig.lines[k], "\"phase\":"));
		assert(strstr(rig.lines[k], "\"marker\":"));
		assert(strstr(rig.lines[k], "\"ms\":"));
		assert(strstr(rig.lines[k], "\"corr\":"));
		assert(strstr(rig.lines[k], "\"ok\":"));
	}
}

/* ---- cases -------------------------------------------------------------- */

static void test_disabled_profile(void) {
	struct hw_run_config cfg;
	memset(&rig, 0, sizeof(rig));
	split_recovery_reset();
	hw_run_config_init(&cfg);
	hw_run_apply_profile(&cfg, HW_RUN_PROFILE_SPLIT_ACTIONS_6M);
	cfg.enabled = true;
	split_recovery_arm(&cfg, &hooks);
	assert(!split_recovery_armed());
	assert(!split_recovery_rig_create_allowed(SPLIT_RECOVERY_RIG_WORLD));
	rig.view_count = 2;
	rig.world_loaded = rig.host_present = true;
	rig_frames(200);
	assert(!rig.line_count && !rig.done_sent && !rig.rpc_count);
	assert(!rig.act.suppress_autojoin && !rig.act.guest_frame_valid
		   && !rig.act.host_frame_valid && !rig.act.host_walk_valid);
	assert(split_recovery_phase_now() == SPLIT_RECOVERY_IDLE
		   && !split_recovery_seq_now());

	/* An ARMED run whose profile IS split_recovery_6m but which hw_run never
	 * enabled must stay just as inert -- cfg->enabled is a separate gate. */
	hw_run_apply_profile(&cfg, HW_RUN_PROFILE_SPLIT_RECOVERY_6M);
	cfg.enabled = false;
	split_recovery_arm(&cfg, &hooks);
	assert(!split_recovery_armed());
	rig_frames(50);
	assert(!rig.line_count && !rig.done_sent);
	puts("PASS split_recovery disabled profile: no markers, no verdict, no RPC, "
		 "no owned frames, no rig-world create");
}

static void test_rig_world_gate(void) {
	rig_init();
	/* Armed: only the one fixed directory, never a caller-chosen target. */
	assert(split_recovery_rig_create_allowed(SPLIT_RECOVERY_RIG_WORLD));
	assert(!split_recovery_rig_create_allowed("saves"));
	assert(!split_recovery_rig_create_allowed("World"));
	assert(!split_recovery_rig_create_allowed(""));
	assert(!split_recovery_rig_create_allowed(NULL));
	split_recovery_reset();
	assert(!split_recovery_rig_create_allowed(SPLIT_RECOVERY_RIG_WORLD));
	puts("PASS split_recovery rig-world gate: the world-deleting create refuses "
		 "every directory but the fixed rig name, and refuses all of them while "
		 "disarmed");
}

/* Runs P0..P4 to completion and returns with the verdict in rig.done. */
static void run_happy_path(void) {
	rig_init();
	rig_frames(1200); /* 60 s of 20 fps frames; the phases need ~25 s */
}

static void test_reload_separated_owner_residency(void) {
	rig_init();
	rig.reload_owner_residency = true;
	rig_frames(3000);
	expect_done("ok");
	assert(rig.reload_remote_waits > 0);
	assert(line_with("host_verified", 0));
	assert(line_with("blocks_verified", 2));
	assert(line_with("guest_readmitted", 0));
	puts("PASS R8 separated reload: host verified before guest readmission, "
		 "guest ring available for all three block checks within original P4 cap");
	/* A refused readmission still cannot fabricate remote blocks or extend
	 * P4. Stop the feed exactly after the immutable host baseline marker. */
	rig_init();
	rig.reload_owner_residency = true;
	for(unsigned i = 0; i < 1200 && !line_with("host_verified", 0); i++)
		rig_frames(1);
	assert(line_with("host_verified", 0));
	rig.admit_guest = false;
	rig_frames(2000);
	expect_done("p4:phase timeout");
	assert(rig.reload_remote_waits > 0 && !line_with("blocks_verified", 0));
	assert(!line_with("guest_readmitted", 0));
	puts("PASS R8 separated reload negative: no admission retains original P4 timeout");
}

static void test_happy_path(void) {
	run_happy_path();
	expect_done("ok");
	assert_sequence();

	/* Every contract marker, exactly once (three for the indexed pairs). */
	static const char* once[] = {"guest_admitted", "loss_injected", "no_leave",
		"ticks_advanced", "collapse_begin", "collapse_done", "no_auto_resume",
		"resumed", "view_restored", "leave_requested", "leave_acked",
		"guest_retired", "host_only_layout", "rejoined", "exit_requested",
		"world_reset", "title_entered", "autoload_armed", "world_loaded",
		"host_verified", "guest_readmitted", "guest_verified", "ok"};
	for(size_t k = 0; k < sizeof(once) / sizeof(once[0]); k++) {
		assert(line_with(once[k], 0));
		assert(!line_with(once[k], 1));
	}
	assert(line_with("baseline", 0) && line_with("baseline", 1)
		   && !line_with("baseline", 2));
	/* BOTH actors hold something, obtained through production dig + pickup, at
	 * the P0 baseline AND at the settled pre-save baseline -- otherwise every
	 * assertion below would be comparing two empty inventories. */
	for(unsigned a = 0; a < 2; a++) {
		assert(field_u(line_with("baseline", a), "actor") == a);
		assert(field_u(line_with("baseline", a), "inv_used") >= 1);
		assert(field_u(line_with("pre_save_baseline", a), "actor") == a);
		assert(field_u(line_with("pre_save_baseline", a), "inv_used") >= 1);
	}
	assert(rig.host_digs >= SPLIT_RECOVERY_HOST_DIGS
		   && rig.guest_digs >= SPLIT_RECOVERY_GUEST_DIGS);
	/* Real PC app, 2026-09-17: a drop from a NEIGHBOUR cell can rest on the pit
	 * rim above the body's y range and is never collected (diagonal 2 of 4
	 * runs, orthogonal 1 of 3). Every dig must be the digger's own column. */
	assert(rig.foreign_digs == 0);
	assert(rig.guest_places == 2 && rig.host_places == 1);
	/* ... and the two baselines must DIFFER, or a host/guest inventory swap
	 * would pass every later per-owner hash comparison. */
	assert(field_u(line_with("baseline", 0), "inv_hash")
		   != field_u(line_with("baseline", 1), "inv_hash"));
	/* Every later comparison is against that POPULATED baseline: the guest
	 * hash after the loss recovery, after the rejoin and after the reload, and
	 * the host hash after the reload, all equal what P0/P3 recorded. */
	{
		unsigned want = field_u(line_with("baseline", 1), "inv_hash");
		assert(field_u(line_with("inventory_match", 0), "want") == want);
		assert(field_u(line_with("inventory_match", 0), "inv_hash") == want);
		assert(field_u(line_with("inventory_match", 1), "want") == want);
		assert(field_u(line_with("guest_verified", 0), "want") == want);
		unsigned host_want
			= field_u(line_with("pre_save_baseline", 0), "inv_hash");
		assert(field_u(line_with("host_verified", 0), "want") == host_want);
		assert(field_u(line_with("host_verified", 0), "inv_hash") == host_want);
	}
	/* Two guest-inventory comparisons against the P0 baseline (after the loss
	 * recovery, and after the rejoin); the third, across the reload, is
	 * guest_verified. */
	assert(line_with("inventory_match", 1) && !line_with("inventory_match", 2));
	for(unsigned i = 0; i < SPLIT_RECOVERY_BLOCKS; i++) {
		assert(field_u(line_with("block_placed", i), "i") == i);
		assert(field_u(line_with("blocks_verified", i), "i") == i);
	}
	assert(!line_with("block_placed", SPLIT_RECOVERY_BLOCKS));
	assert(!line_with("fail", 0));

	/* run.ok is the LAST line of the run. */
	assert(!strcmp(rig.lines[rig.line_count - 1], line_with("ok", 0)));

	/* Correlation: a request marker carries its OWN seq; the observations that
	 * answer it carry that seq, never 0. */
	const char* leave_req = line_with("leave_requested", 0);
	assert(field_u(leave_req, "corr") == field_u(leave_req, "seq"));
	assert(field_u(line_with("leave_acked", 0), "corr")
		   == field_u(leave_req, "seq"));
	const char* exit_req = line_with("exit_requested", 0);
	assert(field_u(exit_req, "corr") == field_u(exit_req, "seq"));
	assert(field_u(line_with("world_reset", 0), "corr")
		   == field_u(exit_req, "seq"));

	/* The P1 timing windows the collector enforces. */
	unsigned grace = field_u(line_with("collapse_begin", 0), "grace_ms");
	unsigned transition
		= field_u(line_with("collapse_done", 0), "transition_ms");
	assert(grace >= 5000 && grace <= 5500);
	assert(transition >= 350 && transition <= 850);

	/* A rejoin is a NEW generation; a reload is a NEW input epoch. */
	assert(field_u(line_with("rejoined", 0), "gen")
		   > field_u(line_with("guest_admitted", 0), "gen"));
	assert(field_u(line_with("world_loaded", 0), "epoch")
		   > field_u(line_with("exit_requested", 0), "epoch"));

	/* The three blocks really were placed through the guest mailbox and read
	 * back from the world afterwards. */
	assert(rig.cell_count == SPLIT_RECOVERY_BLOCKS);
	for(unsigned i = 0; i < SPLIT_RECOVERY_BLOCKS; i++)
		assert(field_u(line_with("blocks_verified", i), "id")
			   == field_u(line_with("block_placed", i), "id"));
	printf("PASS split_recovery happy path: %u markers, gapless seq, ordered "
		   "phases, grace %u ms / transition %u ms, new gen + new epoch, "
		   "profile_done \"ok\"\n",
		   rig.line_count, grace, transition);
}

static void test_pickup_never_observed(void) {
	rig_init();
	rig.starve_host_pickup = true; /* the dig lands, the drop never arrives */
	rig_frames(SPLIT_RECOVERY_P0_MS / RIG_FRAME_MS + 20);
	expect_done("p0:pickup never observed: actor 0");
	assert(!line_with("baseline", 0));

	rig_init();
	rig.starve_guest_pickup = true;
	rig_frames(SPLIT_RECOVERY_P0_MS / RIG_FRAME_MS + 20);
	expect_done("p0:pickup never observed: actor 1");
	assert(!line_with("baseline", 0));
	puts("PASS split_recovery pickup never observed: a dig whose drop never "
		 "reaches the digger inventory fails P0, per actor, and publishes no "
		 "baseline");
}

static void test_baseline_inventory_empty(void) {
	/* The dig/pickup sequence runs, but the actor is empty again by the time
	 * the baseline is taken -- exactly the vacuous case this round exists to
	 * make impossible. Modelled by emptying the host stack once it has dug. */
	rig_init();
	rig.empty_baseline = true;
	rig_frames(SPLIT_RECOVERY_P0_MS / RIG_FRAME_MS + 20);
	expect_done("p0:baseline inventory empty: actor 0");
	/* The awaited marker is what carries the failure (M6): a `baseline` line
	 * with ok:false and the diagnostic, never a passing baseline. */
	assert(line_with("baseline", 0));
	assert(strstr(line_with("baseline", 0), "\"ok\":false"));
	assert(strstr(line_with("baseline", 0),
				  "\"diag\":\"baseline inventory empty: actor 0\""));
	assert(!line_with("baseline", 1));
	puts("PASS split_recovery empty baseline: an actor that reaches the "
		 "baseline holding nothing FAILS the phase instead of publishing a "
		 "comparison of two empty inventories");
}

/* Real Wii, 878619c_09.17 run 0917_092937: a correct 350 ms ease read as
 * "349 ms, max_frame_ms=17" and FAILED by one ms of clock truncation. The lower
 * bound has one frame of resolution; more than a frame early still fails. */
static void test_transition_measurement_resolution(void) {
	rig_init();
	rig.done_skew_ms = 1;
	rig_frames(1200);
	assert(line_with("collapse_done", 0));
	assert(strstr(line_with("collapse_done", 0), "\"ok\":true"));
	assert(field_u(line_with("collapse_done", 0), "transition_ms") == 349);
	rig_init();
	rig.done_skew_ms = 3 * RIG_FRAME_MS;
	rig_frames((SPLIT_RECOVERY_P0_MS + SPLIT_RECOVERY_P1_MS) / RIG_FRAME_MS);
	assert(line_with("collapse_done", 0));
	assert(strstr(line_with("collapse_done", 0), "\"ok\":false"));
	assert(strstr(line_with("collapse_done", 0), "collapse took"));
	puts("PASS split_recovery transition resolution: a 1 ms short reading of a "
		 "correct ease passes, a collapse more than a frame early FAILS");
}

/* Delta review 2026-09-17: ~600 ms of sender back-pressure while a TIMED marker
 * is pending used to re-measure the window on every retry frame and FAIL a
 * correct grace / transition. Readings are latched on first observation. */
static void test_timed_markers_survive_backpressure(void) {
	const char* timed[] = {"\"marker\":\"loss_injected\"",
						   "\"marker\":\"collapse_begin\"",
						   "\"marker\":\"collapse_done\""};
	for(unsigned k = 0; k < 3; k++) {
		rig_init();
		rig.refuse_marker = timed[k];
		rig.refuse_tries = 12; /* 12 frames of 50 ms = 600 ms > the 500 ms window */
		rig_frames(1200);
		assert(rig.refused == 12);
		expect_done("ok");
		assert_sequence();
		assert(field_u(line_with("collapse_begin", 0), "grace_ms") == 5000);
		assert(field_u(line_with("collapse_done", 0), "transition_ms") == 350);
	}
	puts("PASS split_recovery timed markers under back-pressure: grace and "
		 "transition are latched on first observation, so a refused emit "
		 "cannot turn a correct 5000/350 ms into a timing FAIL");
}

/* Real PC app 2026-09-17: a placed dirt block legitimately became GRASS before
 * the reload (Beta BlockGrass.updateTick spread) and P4 called it lost. Grass
 * spread is accepted; any other change is still a lost block. */
static void test_grass_spread_is_not_a_lost_block(void) {
	rig_init();
	rig.reload_cell1_id = BLOCK_GRASS;
	rig_frames(1200);
	expect_done("ok");
	rig_init();
	rig.reload_cell1_id = BLOCK_STONE;
	rig_frames(1200);
	expect_done("p4:block 1 lost: got 1/0");
	/* Astra gate 3: the exception is exactly dirt/0 -> grass/0; grass with any
	 * other metadata is corrupt persistence and must FAIL. */
	rig_init();
	rig.reload_cell1_id = BLOCK_GRASS;
	rig.reload_cell1_meta = 3;
	rig_frames(1200);
	expect_done("p4:block 1 lost: got 2/3");
	puts("PASS split_recovery grass spread: placed dirt read back as grass "
		 "passes, read back as anything else FAILS as a lost block");
}

/* Two identical inventories make every per-owner comparison blind to a swap. */
static void test_baselines_indistinguishable(void) {
	rig_init();
	rig.equal_baselines = true;
	rig_frames(SPLIT_RECOVERY_P0_MS / RIG_FRAME_MS + 20);
	expect_done("p0:baselines indistinguishable: actor 0 == actor 1");
	assert(strstr(line_with("baseline", 1), "\"ok\":false"));
	puts("PASS split_recovery indistinguishable baselines: equal host and "
		 "guest inventories FAIL P0 instead of hiding an ownership swap");
}

/* Real PC app trace, 2026-09-17: a NULL guest frame IS an absent controller to
 * the production recovery handler, so the scripted guest was LOST from the
 * frame it was admitted and the layout had already collapsed when P1 injected
 * its loss. While armed the driver owns a PRESENT, neutral guest device. */
static void test_guest_device_present_by_default(void) {
	rig_init();
	rig_frames(5);
	assert(rig.act.guest_frame_valid && rig.act.guest_frame.connected);
	assert(rig.guest.menu_host.recovery.phase == CONTROLLER_RECOVERY_NONE);
	puts("PASS split_recovery guest device: present and neutral by default; "
		 "only the P1 loss window reports it gone");
}

/* H1. A column that has not streamed in must read as "wait", never as the
 * STONE id world_get_block answers for a missing chunk. */
static void test_blocks_not_resident(void) {
	rig_init();
	rig.blocks_not_resident = true;
	/* Past the digs and the first placement, still inside the block wait. */
	rig_frames(150);
	assert(!rig.done_sent);				  /* no verdict: it is WAITING */
	assert(!line_with("block_placed", 0)); /* and no unobserved marker */
	assert(split_recovery_phase_now() == SPLIT_RECOVERY_P0);
	rig.blocks_not_resident = false; /* the column arrives */
	rig_frames(1400);
	expect_done("ok");
	assert(line_with("block_placed", 2) && line_with("blocks_verified", 2));
	puts("PASS split_recovery non-resident column: the driver WAITS for a "
		 "column that has not streamed instead of reading the missing-chunk "
		 "STONE fallback as a real block, and completes once it arrives");
}

/* H1, the other half: a column that never arrives must still name itself. */
static void test_block_never_reflected(void) {
	rig_init();
	rig.blocks_not_resident = true;
	rig_frames(SR_RIG_BLOCK_WAIT_FRAMES);
	expect_done("p0:block 0 never reflected");
	assert(strstr(line_with("block_placed", 0), "\"ok\":false"));
	puts("PASS split_recovery block never reflected: a placement the client "
		 "never sees fails on the awaited marker with its own diagnostic, not "
		 "on a bare phase timeout");
}

/* H2. A refused emit brings P4 step 0 back next frame; the hook must not fire
 * a second time. */
static void test_autoload_is_one_shot(void) {
	rig_init();
	rig_frames(1200);
	expect_done("ok");
	assert(rig.autoload_calls == 1);

	rig_init();
	rig.emit_backpressure_at_autoload = true;
	rig_frames(1600);
	expect_done("ok");
	assert(rig.autoload_calls == 1);
	printf("PASS split_recovery autoload one-shot: arm_autoload fires exactly "
		   "once even when the marker emit is refused (%u call)\n",
		   rig.autoload_calls);
}

/* M5. The latch is spent at boot -> say so before deleting anything. */
static void test_autoload_latch_consumed(void) {
	rig_init();
	rig.autoload_conflict = "MyWorld";
	rig_frames(200);
	expect_done("p0:autoload latch already consumed: config autoload=MyWorld");
	assert(!rig.world_loaded); /* nothing was created, nothing was deleted */
	puts("PASS split_recovery autoload latch consumed: a card whose config "
		 "spent the latch at boot fails EARLY and by name, with the rig world "
		 "left untouched");
}

/* M8. Every string the driver copies into a marker is JSON-escaped. A strict
 * scan of the published line is the check -- a quote or a backslash in the
 * server toast used to produce a line no parser would take. */
static bool json_line_valid(const char* line) {
	bool in_string = false;
	for(const char* c = line; *c; c++) {
		if(!in_string) {
			if(*c == (char)0x22)
				in_string = true;
			continue;
		}
		if(*c == (char)0x5C) {
			char e = c[1];
			if(e != (char)0x22 && e != (char)0x5C && e != (char)0x2F
			   && e != (char)0x62 && e != (char)0x66 && e != (char)0x6E
			   && e != (char)0x72 && e != (char)0x74 && e != (char)0x75)
				return false; /* not a legal escape */
			c++;
			continue;
		}
		if(*c == (char)0x22)
			in_string = false;
		else if((unsigned char)*c < 0x20)
			return false; /* a raw control character inside a string */
	}
	return !in_string;
}

static void test_toast_is_escaped(void) {
	rig_init();
	rig.settle_accept = false;
	rig.settle_refuse = true;
	snprintf(rig.toast, sizeof(rig.toast), "%s",
			 "Save failed: \"chunk\\io\" stage\tX");
	rig_frames(1200);
	assert(rig.done_sent);
	/* The verdict keeps the server words verbatim ... */
	assert(strstr(rig.done, "Save failed: \"chunk\\io\" stage"));
	/* ... and every published line is still parseable. */
	for(unsigned k = 0; k < rig.line_count; k++)
		assert(json_line_valid(rig.lines[k]));
	const char* fail = line_with("world_reset", 0);
	assert(fail && strstr(fail, "\"ok\":false"));
	assert(strstr(fail, "\\\"chunk\\\\io\\\"") && strstr(fail, "\\t"));
	puts("PASS split_recovery marker escaping: a server toast carrying quotes, "
		 "a backslash and a control character still yields lines a strict JSON "
		 "scan accepts, with the words preserved");
}

/* H3. An armed split_recovery run may delete a world and drive Save and quit,
 * and it pins a seed to keep its own runs comparable. Neither may leak into a
 * LATER --cw-profile, and a seed the user asked for must survive on either
 * side of the profile argument. */
static void test_profile_does_not_leak(void) {
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	hw_run_apply_profile(&cfg, HW_RUN_PROFILE_SPLIT_RECOVERY_6M);
	assert(cfg.split_recovery && cfg.split_guest);
	/* The driver makes its own world from the title and spends the autoload
	 * latch once, in P4: a live "+new" sentinel would race its frame-1 create. */
	assert(!cfg.autoload_new);
	assert(cfg.seed_set && cfg.seed == 12345 && cfg.seed_from_profile);
	hw_run_apply_profile(&cfg, HW_RUN_PROFILE_FAR_WALK_6M);
	assert(!cfg.split_recovery);
	/* perf-seed-pin (docs/intent/perf-seed-pin.md, 2026-09-26): far_walk_6m
	 * is itself a measurement profile that now defaults to seed 12345, so
	 * the clear-then-repin sequence (hw_run_apply_profile's top-of-function
	 * `if(cfg->seed_from_profile)` reset, then this profile's own
	 * `!cfg->seed_set` guard) lands on the SAME value here -- this is
	 * far_walk_6m pinning its own default, not split_recovery_6m's pin
	 * leaking through. */
	assert(cfg.seed_set && cfg.seed == 12345 && cfg.seed_from_profile);

	/* --cw-seed BEFORE the profile: the pin must not overwrite it. */
	/* argv[0] is the program name (the parse starts at 1), and an armed run
	 * always carries --cw-tel-host -- the parse refuses one without it. */
	char* argv_before[] = {"cavex", "--cw-tel-host", "10.0.0.1", "--cw-seed",
						   "7", "--cw-profile", "split_recovery_6m"};
	char err[128];
	hw_run_config_init(&cfg);
	assert(hw_run_parse_args(&cfg, 7, argv_before, err, sizeof(err)));
	assert(cfg.profile == HW_RUN_PROFILE_SPLIT_RECOVERY_6M);
	assert(cfg.seed_set && cfg.seed == 7 && !cfg.seed_from_profile);

	/* --cw-seed AFTER the profile: it overrides the pin. */
	char* argv_after[] = {"cavex", "--cw-tel-host", "10.0.0.1", "--cw-profile",
						  "split_recovery_6m", "--cw-seed", "7"};
	hw_run_config_init(&cfg);
	assert(hw_run_parse_args(&cfg, 7, argv_after, err, sizeof(err)));
	assert(cfg.seed_set && cfg.seed == 7 && !cfg.seed_from_profile);

	/* ... and a user seed still survives a later profile switch. */
	hw_run_apply_profile(&cfg, HW_RUN_PROFILE_FAR_WALK_6M);
	assert(cfg.seed_set && cfg.seed == 7);

	/* M9: the driver refuses to arm without a guest body to lose. */
	hw_run_config_init(&cfg);
	hw_run_apply_profile(&cfg, HW_RUN_PROFILE_SPLIT_RECOVERY_6M);
	cfg.enabled = true;
	cfg.split_guest = false;
	split_recovery_reset();
	split_recovery_arm(&cfg, &hooks);
	assert(!split_recovery_armed());
	split_recovery_reset();
	puts("PASS split_recovery profile isolation: split_recovery and the rig "
		 "seed pin never leak into a later --cw-profile, a user --cw-seed wins "
		 "in either order, and arming needs a guest body");
}

static void test_phase_timeout(void) {
	rig_init();
	rig.admit_guest = false; /* the guest never lands: P0 can never finish */
	rig_frames(SPLIT_RECOVERY_P0_MS / RIG_FRAME_MS + 20);
	expect_done("p0:phase timeout");
	assert(line_with("fail", 0));
	assert(strstr(line_with("fail", 0), "\"ok\":false"));
	assert(strstr(line_with("fail", 0), "\"diag\":\"phase timeout\""));
	assert(split_recovery_phase_now() == SPLIT_RECOVERY_FAILED);
	unsigned lines = rig.line_count;
	rig_frames(100); /* a finished run stops driving anything */
	assert(rig.line_count == lines);
	puts("PASS split_recovery phase timeout: the failing phase and its "
		 "diagnostic reach both the marker and profile_done, and the run stops");
}

/* Drive to the point where the host has asked to exit, with the settlement
 * knobs left to the caller. */
static void run_to_host_exit(void) {
	rig_init();
	rig.settle_accept = false;
	rig_frames(1200);
	assert(split_recovery_phase_now() == SPLIT_RECOVERY_P3 || rig.done_sent);
}

static void test_enqueue_refusal(void) {
	rig_init();
	rig.settle_accept = false;
	rig.unload_enqueue_ok = false; /* main.c settles failure on the same frame */
	rig_frames(1200);
	expect_done("p3:unload enqueue refused");
	assert(!line_with("world_reset", 0) && !line_with("title_entered", 0));
	puts("PASS split_recovery enqueue refusal: a refused SRPC_UNLOAD_WORLD "
		 "fails P3 with its own diagnostic and never claims a reset");
}

static void test_save_failure(void) {
	rig_init();
	rig.settle_accept = false;
	rig.settle_refuse = true; /* CRPC_RETURN_INGAME with the server toast */
	rig_frames(1200);
	/* The server toast is recorded VERBATIM, not paraphrased. */
	expect_done("p3:Save failed: inventory close");
	/* Published on the marker P3 was AWAITING (M6), not on a bare "fail". */
	assert(line_with("world_reset", 0));
	assert(strstr(line_with("world_reset", 0), "\"ok\":false"));
	assert(strstr(line_with("world_reset", 0),
				  "\"diag\":\"Save failed: inventory close\""));
	assert(!line_with("title_entered", 0)); /* and no title behind a refusal */
	puts("PASS split_recovery save failure: the server \"Save failed: <stage>\" "
		 "toast is the diagnostic, character for character");
}

static void test_premature_title(void) {
	rig_init();
	rig.settle_accept = false;
	rig.title_early = true; /* a title with no WORLD_RESET(0) behind it */
	rig_frames(1200);
	expect_done("p3:title before WORLD_RESET(0)");
	assert(!line_with("world_reset", 0) && !line_with("title_entered", 0));
	puts("PASS split_recovery premature title: a title observed before the "
		 "reset that causes it FAILS the run");
}

static void test_emit_backpressure(void) {
	rig_init();
	rig.emit_ok = false;
	rig_frames(60); /* the ring refuses every line */
	assert(!rig.line_count);
	rig.emit_ok = true;
	rig_frames(1200);
	expect_done("ok");
	assert_sequence(); /* still gapless: a refused line is retried, not dropped */
	puts("PASS split_recovery emit back-pressure: a refused observer line is "
		 "retried with the same seq, so the sequence stays gapless");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); /* a crash must not swallow the PASS log */
	(void)run_to_host_exit;
	test_disabled_profile();
	test_rig_world_gate();
	test_happy_path();
	test_reload_separated_owner_residency();
	test_blocks_not_resident();
	test_block_never_reflected();
	test_autoload_is_one_shot();
	test_autoload_latch_consumed();
	test_toast_is_escaped();
	test_profile_does_not_leak();
	test_pickup_never_observed();
	test_baseline_inventory_empty();
	test_baselines_indistinguishable();
	test_transition_measurement_resolution();
	test_timed_markers_survive_backpressure();
	test_grass_spread_is_not_a_lost_block();
	test_guest_device_present_by_default();
	test_phase_timeout();
	test_enqueue_refusal();
	test_save_failure();
	test_premature_title();
	test_emit_backpressure();
	assert(clin_init());
	assert(svin_init());
	blocks_init(); items_init();
	memset(&gstate, 0, sizeof gstate);
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	test_portal_quit_rpc_race(false, false);
	#ifdef CAVEWII_WORLD_SWITCH_BEGIN_SEQ
	test_portal_quit_rpc_race(false, true);
	#endif
	test_portal_quit_rpc_race(true, false);
	puts("ALL split_recovery script tests passed");
	return 0;
}
