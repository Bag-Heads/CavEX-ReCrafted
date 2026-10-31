/* Hostile unit tests for the mp_play_10m pure phase machine. GPL-3.0-or-later.
 *
 * Contract: tmp/jobs/push-0921/design-mp-play-10m.md (becomes
 * docs/mp_play_profile.md). Code under test: source/game/mp_play_core.h.
 *
 * Every case drives mpp_tick() frame by frame against a scripted fake world
 * that REACTS to struct mpp_out the way the real game + mc173-diag fixture
 * would: a held ATTACK on the right cell earns a server block-change echo
 * after 750 ms, a USE press on a valid top face earns a place echo, GUI
 * presses walk a 9-wide container grid with the hotbar as its bottom row, and
 * a GUI_CLICK sends a numbered transaction the server accepts or rejects. The
 * fake derives press EDGES the way production does -- an action only counts as
 * pressed when it was released on the previous frame.
 *
 * WHAT THIS TEST DOES NOT PROVE, stated rather than implied:
 *   - the impure half. mp_play_script.c's fill of struct mpp_obs from real
 *     production state, the virtual pad, the binding table and the aim lock
 *     are all replaced here by the fake below. This proves the phase machine,
 *     not the seam.
 *   - the server. Echo timing, transaction acceptance and offline-inventory
 *     persistence are scripted; what mc173-diag really does is what the
 *     hardware profile measures.
 *   - the collector. Section 5's PASS rule runs in
 *     tools/wii_telemetry_server.py over the emitted marker stream; only the
 *     device half of it is checked here.
 *
 * Assertions that the CONTRACT requires but the HEADER does not satisfy are
 * recorded through EXPECT() rather than assert(): they are marked
 * `HEADER DEFECT n` at the call site, printed in the summary, and make the
 * binary exit non-zero. No case is weakened to make the header pass.
 */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/mp_play_core.h"

/* ---- defect ledger ------------------------------------------------------- */

#define MAX_DEFECTS 32

static struct {
	int id;
	const char* where;
	const char* what;
	unsigned hits;
} g_def[MAX_DEFECTS];
static int g_def_n = 0;

static void defect(int id, const char* where, const char* what) {
	for(int k = 0; k < g_def_n; k++)
		if(g_def[k].id == id) {
			g_def[k].hits++;
			return;
		}
	assert(g_def_n < MAX_DEFECTS);
	g_def[g_def_n].id = id;
	g_def[g_def_n].where = where;
	g_def[g_def_n].what = what;
	g_def[g_def_n].hits = 1;
	g_def_n++;
	printf("  HEADER DEFECT %d [%s]: %s\n", id, where, what);
}

/* Contract assertion the header may fail: records, never aborts. */
#define EXPECT(id, cond, where, what)                                          \
	do {                                                                       \
		if(!(cond))                                                            \
			defect((id), (where), (what));                                     \
	} while(0)

/* ---- fake world ---------------------------------------------------------- */

#define FRAME_MS 16u
#define FRAME_CAP 120000u /* ~32 min of simulated 16 ms frames */
#define MAX_MARKS 512
#define DIG_HOLD_MS 750u
/* How long friction takes to bring a NUDGED body back to rest. This is the
 * fake's PHYSICS, not a mirror of the core's budget -- but the two have to be
 * compatible or this fixture proves nothing, so main() asserts that the body
 * is at rest again before the core next looks (MPP_NUDGE_SETTLE_MS after the
 * tap ends). Checked, not assumed. */
#define SETTLE_MS 400u
#define SURFACE_Y 64.0f
#define DIRT 3
#define GRASS 2
#define STONE 1
#define GRAVEL 13
#define TALL_GRASS 31

struct cfg {
	/* join / login */
	uint32_t ready_delay_ms;	  /* gen 1 */
	uint32_t ready_delay2_ms;	  /* gen 2 */
	unsigned admits1, admits2;	  /* lworld_ready_timeout_admits() */
	uint32_t login_delay_ms;	  /* RUNNING -> OK */
	bool login_fail;			  /* RUNNING -> FAILED */
	bool login_never;			  /* stays RUNNING */
	/* REL-14b, the rejoin generation: the login machine is still PARKED
	 * from gen 1 and the restart never takes; the gen-2 world install never
	 * advances; the post-reconnect click is 1 rejected / 2 never answered by
	 * a Packet106; the inventory is full when the gen-2 leg opens it. */
	bool stale_login_gen2, stale_install_gen2, post_fill_inv;
	int post_click;
	/* latches (fire on the measured clock unless said otherwise) */
	int32_t zero_health_at_ms;
	int32_t death_screen_at_ms;
	int32_t lose_session_at_ms;
	int32_t fault_at_ms;
	int32_t teleport_at_ms;
	bool drop_session_in_pause_leave;
	/* build leg */
	uint8_t under_feet;
	unsigned hollow_support_cycles;
	unsigned unknown_support_cycles;
	unsigned stale_support_dig_cycles;
	unsigned support_lost_dig_cycles;
	bool never_at_rest;
	/* The nudge moves NOTHING (a body pinned against terrain): the core burns
	 * its taps and its centring budget and skips. Since c4488f36 this is a
	 * different thing from never_at_rest -- the two have their own budgets and
	 * their own skip reasons -- so the fake models them separately. */
	bool never_centred;
	/* MIRRORED facing: the fake reports fwd as (+sin yaw, cos yaw) instead of
	 * Beta's (-sin yaw, cos yaw), i.e. exactly what a sign error in the impure
	 * half's yaw mapping would produce. The core must refuse to tap. */
	bool mirror_facing;
	/* The body never gets anywhere on a walk leg (a pit, a shore, a wall). */
	bool blocked_leg;
	/* hw1021f: from this measured-play time on (0 = never), every walk leg
	 * goes nowhere and the escape gains nothing (a pocket mc173 refuses the
	 * pillar in) -- stuck for good. */
	uint32_t blocked_from_ms;
	/* THE PIT (push-0921/B16; PC run pc_mp_play_20260921_171336). After the
	 * FIRST dig the walker gets nowhere for this many legs in a row, and the
	 * body stays IN THE CELL IT DUG -- it is not put back on the surface, and
	 * what is under its feet is diggable dirt. That run dug the same column
	 * five times (y = 87 .. 83) until it stood on stone, 600 s gone, "action
	 * floors not met". UINT32_MAX = for good. */
	uint32_t pit_legs;
	/* THE SHAFT (REL-14b-walker-escape; real Wii hw1020b fell 7 blocks into a
	 * 2x3 shaft). With pit_legs set, this many blocks of pillar (jump, USE at
	 * the top of the jump, land one higher) lift the body to the rim, after
	 * which the walker leaves. 0 = a sealed box: JUMP and USE gain nothing. */
	unsigned pit_depth;
	/* THE REFUSED PILLAR (real Wii hw1021e vs mc173): the server refuses this
	 * many pillar USEs (mc173 "can_place refuse": its gravity-ticked copy of
	 * the player still overlaps the cell). No block, no rise, nothing spent. */
	unsigned pit_refuse_places;
	uint8_t pit_wall_id; /* what the escape's wall scan meets (0 = dirt/grass) */
	/* THE STRADDLED LEDGE (real Wii hw1023c mp_play_10m): the first pit leg
	 * leaves the body at this z fraction (0 = off), across two columns. While
	 * it straddles, straight down from its centre is the LOWER neighbour
	 * column, out of the 4.5 pick reach: no crosshair, and a USE places
	 * nothing (screen_ingame sends nothing without a hit). */
	float pit_straddle_frac;
	bool pit_pinned; /* ... and once straddled, a centring tap moves nothing */
	/* THE WAYS OUT OF THE HOLE (push-0921/B16; read from the saved worlds of PC
	 * runs 171336 and 172431). On a slope only SOME neighbours of the dug cell
	 * stand one block above its floor -- those are the only ones a body can
	 * climb, and they are exactly the ones whose top face the place leg can
	 * use. Run 172431 dug at (-191,69,-725): E and S were natural ground at 70
	 * (two above the floor, a wall), W and N at 69 -- and the script put a
	 * block on BOTH, walling itself in. Bit d = the core's place_dir d; 0 =
	 * flat ground, all four. A neighbour that is not an exit is a WALL: the
	 * crosshair meets it one above the feet, on a side face. */
	unsigned exit_dirs;
	/* THE DIG ITSELF BUILDS THE TRAP (PC run pc_mp_play_20260921_182523, map read
	 * from its saved world). At (-79,82,-774) the ground was 83 to the W and 84
	 * to the N -- one step up, walkable -- and 80 / 77 to the E / S. Digging one
	 * down turned W and N into two-high walls; E led into a dead-end pocket and
	 * S was a four-block drop. Boxed in, by the dig. Bit k of cliff_cycles: the
	 * column the body stands on in cycle k has NO neighbour level with it. A
	 * body that digs there is trapped for good. */
	uint32_t cliff_cycles;
	/* A leg that DID make progress and still ends in the column of the last dig
	 * (a walker sliding along an edge and coming back). */
	bool lands_in_last_dig_column;
	/* The body is in a fluid from the first build leg on. */
	bool in_water;
	/* The body drifts into a fluid PART-WAY THROUGH a dig: dry when the leg
	 * starts, wet once the core has been in MPP_PH_BUILD_DIG this many frames
	 * (0 = never). Distinct from in_water above, which is caught by the
	 * BUILD_REST gate long before a dig cell is ever chosen. */
	unsigned water_after_dig_frames;
	/* Where a walk leg leaves the body inside its column, as a fraction. 0
	 * means the default off-centre landing; 0.7000008f is the FLUSH-AGAINST-A-
	 * WALL landing the first PC run produced (one ulp outside the old 0.7
	 * window). */
	float land_frac;
	/* WHERE THE BODY IS AFTER THE DIG (push-0921/B12, 8bf54845). The dug cell
	 * is below the feet, so the body falls into it -- but not instantly, and on
	 * a slope not at all. drop_delay_frames holds it on the rim for that many
	 * frames after the echo (0 = the old at-once landing); never_lands keeps it
	 * there for good, which is the "body never landed in the dug cell" skip. */
	unsigned drop_delay_frames;
	bool never_lands;
	/* A PLANT STANDS ON THE PLACE TARGET of these cardinals (bit 0..3 = the
	 * core's place_dir). On a grass plain tall grass covers a large share of
	 * the cells, and the first full run lost 4 of 13 place legs to it.
	 * plant_id defaults to TALL_GRASS; plant_never_clears models one the
	 * server never echoes away. */
	unsigned plant_dirs;
	uint8_t plant_id;
	bool plant_never_clears;
	bool no_dig_crosshair;
	bool no_dig_echo;
	bool dig_echo_wrong_cell;
	bool dig_echo_stale;
	bool no_pickup;
	int16_t pickup_item_override; /* > 255 = a drop that cannot be placed */
	bool no_top_face;
	/* No usable cardinal for the first N CYCLES only (push-0921/B12). Distinct
	 * from skip_n_builds, which lands the body on stone and so skips the DIG
	 * too -- places then stay exactly one behind digs and the catch-up path is
	 * never reached. This one digs normally and loses only the place, which is
	 * the shape that puts places two or more behind. */
	unsigned no_top_face_cycles;
	bool no_place_echo;
	/* REVIEW B17. How many USE presses of the run the server never echoes
	 * (0 = none; no_place_echo above = all of them). M4: one lost press used to
	 * end a twenty-minute run. */
	unsigned unechoed_use_presses;
	/* M1: in the first N dig legs the dig cell turns to air two frames into the
	 * leg, by something that is NOT this script (falling sand, another agent):
	 * the block is gone, the body drops, nothing was dug by hand. */
	unsigned foreign_dig_legs;
	/* M2: in the first N place legs a NON-block echo (water, id 9) lands on the
	 * place cell just before the script's own block does. */
	unsigned foreign_place_legs;
	/* M6: in the first N dig legs a burst of 20 unrelated block changes lands
	 * in the same frame as the dig's own echo and evicts it from the 16-ring. */
	unsigned echo_burst_legs;
	/* The glue has NO watched-cell seam (the state of the tree before the
	 * network half lands): the ring is all there is. */
	bool no_watch;
	/* hw1030g: in the first N dig legs WATER flows into the dug cell right
	 * after the server's air echo (a later record for the same cell), then a
	 * 20-echo burst evicts both from the ring. The newest-only watch slot then
	 * holds water; only the cell's history still holds the air. */
	unsigned flood_after_dig_legs;
	/* ...and the network layer WITHOUT that history (build 53c75ff). */
	bool no_watch_history;
	/* A burst of 20 unrelated block changes lands at the START of every dig
	 * leg, BEFORE the dig can have completed (PC run pc_mp_play_20260921_194515:
	 * cycle 0 was skipped "server echoes overflowed the ring" WITH the watch
	 * seam live -- the rule fired on the burst alone, before the echo it was
	 * supposedly missing could exist). */
	bool early_burst;
	/* REVIEW B21 (Sol). M1: the first dig leg is skipped mid-way (the crosshair
	 * never reaches the cell) and the NEXT walk leg ends in the same column: a
	 * column that was never dug must be diggable. M2: ATTACK touches the cell
	 * for one frame, the aim then drifts off it for 400 ms, and something else
	 * empties the cell -- not a dig. M3: the hotbar has ONE empty slot; a
	 * pickup of a DIFFERENT item lands in it between the two clicks. */
	bool skip_first_dig_then_return;
	bool touch_then_drift;
	bool one_empty_hotbar_slot;
	int16_t stray_pickup_item;
	/* mutation M3 of the review: the dig cell's echo says STONE, not air. */
	bool dig_echo_nonair;
	/* inventory leg */
	bool inv_never_opens;
	bool inv_focus_stuck;
	bool reject_pick;
	bool reject_put;
	bool wrong_slot_after_put;
	/* PICKUPS DO NOT WAIT FOR THE INVENTORY SCREEN (PC run
	 * pc_mp_play_20260921_191831): three dirt items were collected in the 600
	 * ms between the pick-up click and the put-down click; with the stack on
	 * the cursor they went to the FIRST EMPTY slot -- the very slot the script
	 * had chosen as its destination -- so the put landed on 3 and made 5, and
	 * "exactly what I moved" never came true. N items arrive right after the
	 * pick (first empty slot), M more right after the put (they merge into the
	 * stack that now sits in the destination), K before the pick (they merge
	 * into the SOURCE stack, so the cursor holds more than planned). */
	unsigned pickups_after_pick, pickups_after_put, pickups_before_pick;
	bool pickups_before_pick_done;
	bool click_without_txn;
	/* menus */
	bool pause_never_opens;
	bool leave_never_ends;
	bool title_never;
	bool rejoin_title_stuck;
	int rejoin_title_start_row; /* 0: bottom Quit; 2: middle Options */
	bool rejoin_setup_stuck;
	/* post-rejoin state */
	int dim_delta;
	float post_dx, post_dy;
	int post_count_delta;
	int post_damage_delta;
	/* pause / snapshot (draft 2: snapshot_pre is taken with the menu open AND
	 * the body at rest) */
	bool never_rest_at_pause;
	uint32_t move_during_pause_ms;
	/* THE PAUSE MENU FREEZES THE BODY (PC run pc_mp_play_20260921_174423, the
	 * first run ever to reach the rejoin half). CaveWii's pause stops the
	 * client tick bodies -- in a remote session too -- so whatever the body was
	 * doing when the menu opened is what it does for as long as it is open:
	 * that run opened it mid-jump (server pos frozen at y = 65.787 over ground
	 * at 64) and waited ten seconds for an on_ground that could never come.
	 * airborne_after_walk_ms is how long the body needs to land once the
	 * walker lets go. */
	bool pause_freezes_body;
	uint32_t airborne_after_walk_ms;
	/* hw1030r: the server collects a real dig's drop AFTER p2_end -- this many
	 * ms after measured play ends, one item of late_pickup_item lands in the
	 * client inventory (0 = off). inv_never_settles: one more lands every
	 * 500 ms from p2_end on, forever. */
	uint32_t late_pickup_ms;
	int16_t late_pickup_item;
	bool inv_never_settles;
	/* harness */
	unsigned skip_n_builds; /* first N build legs land on stone */
	bool never_pop;
	bool jump_clock_in_walk;
	int fill_mode; /* 0 none, 35 = all but hotbar slots 1-2, 36 = full */
	uint32_t start_now_ms;
};

struct rec {
	uint32_t seq;
	uint32_t at_ms;
	char key[MPP_MARKER_KEY];
	char body[MPP_MARKER_BODY];
};

struct world {
	struct cfg c;
	struct mpp_state st;
	struct mpp_obs obs;
	struct mpp_out out;
	uint32_t now_ms;
	unsigned frames;
	/* session */
	bool remote_session, world_loaded, body_ticking;
	enum mpp_screen screen;
	enum mpp_focus focus;
	unsigned install_gen, want_gen;
	uint32_t join_t0_ms;
	bool joining;
	unsigned admits;
	bool fault;
	enum mpp_login login;
	uint32_t login_t0_ms;
	bool login_armed;
	/* body */
	float x, y, z;
	bool at_rest;
	int dim, health;
	uint8_t under_feet;
	bool walking;
	bool in_water;
	/* Frames the core has spent in MPP_PH_BUILD_DIG on this leg; drives
	 * cfg.water_after_dig_frames. */
	unsigned dig_frames;
	/* Frames left before the body drops into the cell it just dug (0 = not
	 * pending). Set from cfg.drop_delay_frames at the dig echo. */
	unsigned drop_pending;
	/* the pit (cfg.pit_legs): which column was dug last, how many digs landed
	 * in the SAME column as the one before, and the leg bookkeeping */
	bool have_last_dig;
	int32_t last_dig_x, last_dig_z;
	bool have_last_col; /* the column the FIRST build leg stood in */
	int32_t last_col_x, last_col_z;
	/* cfg.exit_dirs: which exits of THIS hole carry a placed block, how often
	 * the body was walled in, and whether it is walled in right now */
	unsigned walled_dirs, traps;
	bool trapped;
	/* THE BODY IS TORN DOWN WITH THE SESSION (short-play PC run
	 * pc_mp_play_20260921_180120): between the leave and the rejoin the local
	 * player reads (0, -1.62, 0), and the server puts it back where it was. */
	bool body_torn_down;
	float saved_x, saved_y, saved_z;
	int saved_health;
	unsigned use_presses, foreign_digs_done, foreign_places_done, bursts_done;
	unsigned floods_done;
	unsigned same_column_digs;
	uint32_t pit_legs_done;
	bool leg_is_pit;
	unsigned pit_rise;
	unsigned pillar_refused; /* pillar USEs the server refused */
	bool use_refused;		 /* this frame's USE edge was one of them */
	unsigned esc_mines_done; /* wall cells dug inside MPP_PH_ESCAPE */
	uint32_t mine_hold_ms;
	bool airborne;
	bool straddled; /* cfg.pit_straddle_frac has been applied */
	unsigned pillar_use_no_target; /* pillar USE edges with no feet target */
	float y_rest;
	uint32_t land_ms;
	int leg_heading;
	bool plant_gone[4];	  /* this cardinal's plant has been broken */
	uint32_t plant_hold_ms;	  /* ATTACK held on the plant, this cardinal */
	/* A USE press seen while the body was NOT yet standing in the cell it dug.
	 * The landing wait exists precisely to make this zero: a place attempted
	 * from the rim aims at a cell that is not there. */
	unsigned use_before_landed;
	/* FACING. `want_face_yaw` is what the aim lock was asked for this frame;
	 * `face_yaw` is what is actually in force, and it follows the request
	 * after ONE frame -- the real chain (autopilot_aim_set_deg ->
	 * gstate.autopilot.yaw -> camera -> e->orient[0]) is not instantaneous
	 * either, and the core's whole fwd_x/fwd_z design exists because of that
	 * lag. Degrees, in the CORE's (Beta's) convention. */
	int want_face_yaw, face_yaw;
	/* Centring taps: FORWARD held for one 50 ms tick moves the body ~0.2
	 * blocks along the facing vector, and the body then needs ~400 ms of
	 * friction to be at rest again. */
	uint32_t fwd_hold_ms;
	bool tap_moved;
	uint32_t rest_again_ms;
	unsigned taps_seen, taps_that_moved;
	/* aim + dig hold */
	bool aim_active;
	int aim_yaw, aim_pitch;
	uint32_t hold_ms;
	/* pending pickup */
	bool pickup_pending;
	uint32_t pickup_at_ms;
	int16_t pickup_item;
	/* inventory */
	struct mpp_item inv[MPP_INV_SLOTS], cursor;
	int held, focus_slot;
	int16_t click_id;
	/* rings */
	struct mpp_echo echoes[16];
	/* THE WATCHED CELL (what the network layer keeps for the script outside
	 * the ring): the newest echo for the one cell the core said it waits on */
	bool watch;
	int32_t watch_x, watch_y, watch_z;
	struct mpp_echo watch_echo;
	struct mpp_echo watch_hist[4]; /* hw1030g: every echo since armed */
	uint32_t watch_hist_n;
	uint32_t echo_serial;
	unsigned echo_w;
	struct mpp_txn txns[8];
	uint32_t txn_serial;
	unsigned txn_w;
	/* menu rows */
	int pause_row, title_row, mps_row;
	unsigned title_up_edges, title_down_edges;
	/* build-leg terrain + the pause-time rest gate */
	unsigned snap_count;
	bool pause_seen;
	uint32_t pause_t0_ms;
	bool p2_end_seen, late_pickup_done;
	uint32_t p2_end_ms, late_next_ms;
	float x_at_pause;
	/* markers */
	struct rec marks[MAX_MARKS];
	unsigned nmarks;
	/* press bookkeeping */
	unsigned prev_actions;
	unsigned n_place_legs, n_pick_legs, n_put_legs;
	unsigned use_edges_in_place, click_edges_in_pick, click_edges_in_put;
	unsigned multi_action_frames, edge_without_release;
	unsigned unsafe_support_attack_frames;
	unsigned attack_after_support_loss;
	/* flags */
	bool snapped_post;
};

static struct world W;

/* ---- small helpers ------------------------------------------------------- */

static long body_num(const char* body, const char* key) {
	const char* p = strstr(body, key);
	if(!p)
		return -1;
	return strtol(p + strlen(key), NULL, 10);
}

static bool body_has(const char* body, const char* key) {
	return strstr(body, key) != NULL;
}

static int mark_index(const struct world* w, const char* key, unsigned from) {
	for(unsigned k = from; k < w->nmarks; k++)
		if(strcmp(w->marks[k].key, key) == 0)
			return (int)k;
	return -1;
}

static unsigned mark_count(const struct world* w, const char* key) {
	unsigned n = 0;
	for(unsigned k = 0; k < w->nmarks; k++)
		if(strcmp(w->marks[k].key, key) == 0)
			n++;
	return n;
}

static bool has_mark(const struct world* w, const char* key) {
	return mark_index(w, key, 0) >= 0;
}

static const struct rec* last_mark(const struct world* w, const char* key) {
	for(unsigned k = w->nmarks; k > 0; k--)
		if(strcmp(w->marks[k - 1].key, key) == 0)
			return &w->marks[k - 1];
	return NULL;
}

static void push_echo(struct world* w, int32_t x, int32_t y, int32_t z,
					  uint8_t type, uint32_t serial_override) {
	struct mpp_echo* e = &w->echoes[w->echo_w % 16];
	if(serial_override)
		e->serial = serial_override;
	else
		e->serial = ++w->echo_serial;
	e->x = x;
	e->y = y;
	e->z = z;
	e->type = type;
	e->metadata = 0;
	w->echo_w++;
	if(w->watch && !w->c.no_watch && x == w->watch_x && y == w->watch_y
	   && z == w->watch_z) {
		w->watch_echo = *e;
		if(!w->c.no_watch_history)
			w->watch_hist[w->watch_hist_n++ % 4u] = *e;
	}
}

static void push_txn(struct world* w, int16_t action, bool accepted) {
	struct mpp_txn* t = &w->txns[w->txn_w % 8];
	t->window = 0;
	t->action_id = action;
	t->accepted = accepted;
	t->serial = ++w->txn_serial;
	w->txn_w++;
}

/* Beta storeItemStack: merge into an existing partial stack first, otherwise
 * the first free hotbar slot (the contract's own-column pickup lands here). */
static void give_item(struct world* w, int16_t item, uint8_t count) {
	for(int k = 0; k < MPP_INV_SLOTS; k++)
		if(w->inv[k].item == item && w->inv[k].count > 0
		   && w->inv[k].count < 64) {
			w->inv[k].count = (uint8_t)(w->inv[k].count + count);
			return;
		}
	for(int k = 0; k < MPP_INV_SLOTS; k++) {
		int s = k < 9 ? k : k; /* hotbar first: indices 0..8 already lead */
		if(w->inv[s].item <= 0) {
			w->inv[s].item = item;
			w->inv[s].count = count;
			w->inv[s].damage = 0;
			return;
		}
	}
}

static void grid_rc(int slot, int* row, int* col) {
	if(slot < 9) {
		*row = 3;
		*col = slot;
	} else {
		*row = slot / 9 - 1;
		*col = slot % 9;
	}
}

static int grid_slot(int row, int col) {
	return row == 3 ? col : (row + 1) * 9 + col;
}

/* Beta's forward vector for a yaw in the CORE's convention, and the MIRRORED
 * one a sign error in the impure half would produce. Written from the
 * decompile [740c583 Entity.moveFlying, strafe 0 / forward 1: motionX +=
 * -sin(yaw), motionZ += cos(yaw)], not from the code under test. */
static void fake_forward(const struct world* w, float* fx, float* fz) {
	double r = (double)w->face_yaw * 3.14159265358979323846 / 180.0;
	*fx = (float)(w->c.mirror_facing ? sin(r) : -sin(r));
	*fz = (float)(cos(r));
}

/* Which cardinal neighbours stand LEVEL with the column under the body -- the
 * ones that are one step up once the body has dug itself one down (so: the
 * exits, and the usable place targets). */
static unsigned fake_level_dirs(const struct world* w) {
	if(w->st.cycle < 32u && ((w->c.cliff_cycles >> w->st.cycle) & 1u))
		return 0u;
	return w->c.exit_dirs ? w->c.exit_dirs : 0xFu;
}

/* A walk leg ENDS here: the body stops where it stopped, inside whatever
 * column it is in -- it is NOT teleported to the middle of one. Landing
 * centred was the fake's own fiction, and it is exactly what hid the defect
 * the first PC runs found. The core has to nudge its way in from here. */
static void snap_to_column(struct world* w) {
	float frac = w->c.land_frac > 0.0f ? w->c.land_frac : 0.2f;
	w->x = (float)mpp_floor(w->x) + frac;
	w->z = (float)mpp_floor(w->z) + frac;
	w->y = SURFACE_Y;
	/* the first cfg.skip_n_builds legs land on terrain the script cannot use */
	w->under_feet = w->snap_count < w->c.skip_n_builds ? STONE : w->c.under_feet;
	if(w->snap_count == 0u) {
		w->have_last_col = true;
		w->last_col_x = mpp_floor(w->x);
		w->last_col_z = mpp_floor(w->z);
	}
	w->snap_count++;
	w->at_rest = !w->c.never_at_rest;
	w->rest_again_ms = 0;
	if(w->c.airborne_after_walk_ms && !w->c.never_at_rest) {
		w->at_rest = false;
		w->rest_again_ms = w->now_ms + w->c.airborne_after_walk_ms;
	}
	w->fwd_hold_ms = 0;
	w->tap_moved = false;
	w->in_water = w->c.in_water;
	/* A FRESH COLUMN grows its own plants: clearing one on the last cycle's
	 * cardinal says nothing about this one. Without this reset the first cycle
	 * would clear all four and every later cycle would find bare ground, so the
	 * plant cases would only ever exercise the core once. */
	memset(w->plant_gone, 0, sizeof(w->plant_gone));
	w->plant_hold_ms = 0;
	w->drop_pending = 0;
	w->walled_dirs = 0;
}

/* ---- world init ---------------------------------------------------------- */

static void world_init(struct world* w, const struct cfg* c) {
	memset(w, 0, sizeof(*w));
	w->c = *c;
	if(!w->c.under_feet)
		w->c.under_feet = GRASS;
	if(!w->c.ready_delay_ms)
		w->c.ready_delay_ms = 4000;
	if(!w->c.ready_delay2_ms)
		w->c.ready_delay2_ms = 4000;
	if(!w->c.login_delay_ms)
		w->c.login_delay_ms = 6000;
	w->c.zero_health_at_ms = c->zero_health_at_ms ? c->zero_health_at_ms : -1;
	w->c.death_screen_at_ms = c->death_screen_at_ms ? c->death_screen_at_ms : -1;
	w->c.lose_session_at_ms = c->lose_session_at_ms ? c->lose_session_at_ms : -1;
	w->c.fault_at_ms = c->fault_at_ms ? c->fault_at_ms : -1;
	w->c.teleport_at_ms = c->teleport_at_ms ? c->teleport_at_ms : -1;
	w->now_ms = c->start_now_ms;
	w->screen = MPP_SCREEN_LOAD_WORLD;
	w->focus = MPP_FOCUS_UNKNOWN;
	w->joining = true;
	w->want_gen = 1;
	w->admits = c->admits1;
	w->join_t0_ms = w->now_ms;
	w->x = 0.5f;
	w->y = SURFACE_Y;
	w->z = 0.5f;
	w->at_rest = true;
	w->under_feet = w->c.under_feet;
	w->dim = 0;
	w->health = 20;
	w->held = 0;
	w->focus_slot = -1;
	/* The session has already carried unrelated traffic before P2 -- server
	 * ring serials and the client's 0x66 action counter are NEVER zero when a
	 * leg latches its baseline. Starting them at zero would let a "stale"
	 * echo (serial == the baseline) look fresh and would hide whether the
	 * machine really re-baselines the click id per move. */
	w->echo_serial = 5;
	w->txn_serial = 5;
	w->click_id = 7;
	w->login = MPP_LOGIN_IDLE;
	/* The inventory starts EMPTY -- one fresh fixture world, contract section
	 * 1. Draft 2 moved the inventory leg between the pickup and the place, so
	 * the player is guaranteed to hold the stack it just dug and no seed is
	 * needed any more. cfg.fill_mode is only for the two cases that need a
	 * (nearly) full inventory: the 3-part snapshot and the skipped inv leg. */
	if(c->fill_mode) {
		for(int k = 0; k < MPP_INV_SLOTS; k++) {
			w->inv[k].item = DIRT;
			w->inv[k].count = 60; /* partial, so a pickup can still merge */
			w->inv[k].damage = 0;
		}
		if(c->fill_mode == 35) { /* leave TWO hotbar slots: the core refuses a
								  * region with a single empty slot (B21 M3) */
			for(int k = 1; k <= 2; k++) {
				w->inv[k].item = 0;
				w->inv[k].count = 0;
			}
		}
	}
	if(c->one_empty_hotbar_slot) {
		/* hotbar 2..8 hold OTHER stacks; slot 0 gets the dig drops, slot 1 is
		 * the only empty hotbar slot */
		for(int k = 2; k < 9; k++) {
			w->inv[k].item = GRAVEL;
			w->inv[k].count = 64;
			w->inv[k].damage = 0;
		}
	}
	mpp_reset(&w->st);
}

/* ---- per-frame world timers ---------------------------------------------- */

static void world_timers(struct world* w) {
	/* The facing catches up with what the aim lock / walker was asked for on
	 * the PREVIOUS frame -- one frame of lag, deliberately. */
	w->face_yaw = w->want_face_yaw;
	/* Friction brings a nudged body back to rest. never_at_rest never does. */
	bool frozen = w->c.pause_freezes_body && w->screen == MPP_SCREEN_PAUSE;
	if(frozen && w->rest_again_ms)
		w->rest_again_ms += FRAME_MS; /* time does not pass for the body */
	if(!w->at_rest && w->rest_again_ms && w->now_ms >= w->rest_again_ms
	   && !w->walking && !w->c.never_at_rest && !frozen) {
		w->at_rest = true;
		w->rest_again_ms = 0;
	}
	uint32_t delay = w->want_gen == 1 ? w->c.ready_delay_ms : w->c.ready_delay2_ms;
	if(w->joining && w->now_ms - w->join_t0_ms >= delay) {
		w->joining = false;
		w->remote_session = true;
		if(w->body_torn_down) {
			w->body_torn_down = false;
			w->health = w->saved_health;
			w->x = w->saved_x;
			w->y = w->saved_y;
			w->z = w->saved_z;
		}
		w->world_loaded = true;
		w->body_ticking = true;
		w->screen = MPP_SCREEN_INGAME;
		w->install_gen = w->want_gen == 2 && w->c.stale_install_gen2
			? 1u : w->want_gen;
		if(w->want_gen == 2) {
			/* RECONNECT RESET: a new remote session starts its own txn ring
			 * at serial 0 and a fresh window container's action counter at
			 * 0 (no click yet = -1, mpp_map_last_click_id). */
			w->txn_serial = 0;
			w->txn_w = 0;
			memset(w->txns, 0, sizeof(w->txns));
			w->click_id = -1;
		}
		w->admits = w->want_gen == 1 ? w->c.admits1 : w->c.admits2;
		if(w->want_gen == 2 && !w->snapped_post) {
			w->snapped_post = true;
			w->x += w->c.post_dx;
			w->y += w->c.post_dy;
			w->dim += w->c.dim_delta;
			if(w->c.post_count_delta)
				for(int k = 0; k < MPP_INV_SLOTS; k++)
					if(w->inv[k].item > 0) {
						w->inv[k].count = (uint8_t)((int)w->inv[k].count
													+ w->c.post_count_delta);
						break;
					}
			if(w->c.post_damage_delta)
				for(int k = 0; k < MPP_INV_SLOTS; k++)
					if(w->inv[k].item > 0) {
						w->inv[k].damage = (int16_t)(w->inv[k].damage
													 + w->c.post_damage_delta);
						break;
					}
		}
	}
	if(w->login_armed && w->login == MPP_LOGIN_RUNNING
	   && !w->c.login_never
	   && w->now_ms - w->login_t0_ms >= w->c.login_delay_ms)
		w->login = w->c.login_fail ? MPP_LOGIN_FAILED : MPP_LOGIN_OK;
	if(w->pickup_pending && w->now_ms >= w->pickup_at_ms) {
		w->pickup_pending = false;
		if(!w->c.no_pickup)
			give_item(w, w->pickup_item, 1);
	}
	/* hw1030r: pickups that land after measured play has ended */
	if(!w->p2_end_seen && has_mark(w, "p2_end")) {
		w->p2_end_seen = true;
		w->p2_end_ms = w->now_ms;
		w->late_next_ms = w->now_ms + 500u;
	}
	if(w->p2_end_seen && w->c.late_pickup_ms && !w->late_pickup_done
	   && w->now_ms - w->p2_end_ms >= w->c.late_pickup_ms) {
		w->late_pickup_done = true;
		give_item(w, w->c.late_pickup_item, 1);
	}
	if(w->p2_end_seen && w->c.inv_never_settles && w->remote_session
	   && w->now_ms >= w->late_next_ms) {
		w->late_next_ms += 500u;
		give_item(w, w->c.late_pickup_item, 1);
	}
	/* scripted latches, on the device's measured clock */
	if(w->st.measuring) {
		uint32_t m = w->st.measured_ms;
		if(w->c.zero_health_at_ms >= 0 && m >= (uint32_t)w->c.zero_health_at_ms)
			w->health = 0;
		if(w->c.death_screen_at_ms >= 0 && m >= (uint32_t)w->c.death_screen_at_ms)
			w->screen = MPP_SCREEN_DEATH;
		if(w->c.lose_session_at_ms >= 0 && m >= (uint32_t)w->c.lose_session_at_ms)
			w->remote_session = false;
		if(w->c.fault_at_ms >= 0 && m >= (uint32_t)w->c.fault_at_ms)
			w->fault = true;
		if(w->c.teleport_at_ms >= 0 && m >= (uint32_t)w->c.teleport_at_ms) {
			w->c.teleport_at_ms = -1;
			w->x += 40.0f; /* a server correction, far past MPP_TELEPORT_BLOCKS */
		}
	}
	if(w->c.drop_session_in_pause_leave
	   && w->st.phase == MPP_PH_PAUSE_LEAVE && w->remote_session) {
		w->remote_session = false;
		w->screen = MPP_SCREEN_TITLE;
		w->focus = MPP_FOCUS_OTHER;
		w->title_row = w->c.rejoin_title_start_row
			? w->c.rejoin_title_start_row : 3;
	}
	/* The body keeps drifting for a while after the pause menu opens, so the
	 * snapshot must wait for rest rather than record a moving position. */
	bool at_pause = w->st.phase == MPP_PH_PAUSE_OPEN
		|| w->st.phase == MPP_PH_PAUSE_FOCUS;
	if(at_pause && !w->pause_seen) {
		w->pause_seen = true;
		w->pause_t0_ms = w->now_ms;
		w->x_at_pause = w->x;
	}
	if(at_pause
	   && (w->c.never_rest_at_pause
		   || w->now_ms - w->pause_t0_ms < w->c.move_during_pause_ms)) {
		w->at_rest = false;
		w->x += 0.064f;
	} else if(at_pause && !w->c.pause_freezes_body)
		w->at_rest = true;
	if(w->c.jump_clock_in_walk && w->st.phase == MPP_PH_WALK
	   && w->st.measured_ms >= 100000u) {
		w->c.jump_clock_in_walk = false;
		w->now_ms = w->c.start_now_ms + FRAME_MS + MPP_TOTAL_CAP_MS + 100u;
	}
}

/* ---- observation --------------------------------------------------------- */

static void fill_obs(struct world* w) {
	struct mpp_obs* o = &w->obs;
	memset(o, 0, sizeof(*o));
	o->now_ms = w->now_ms;
	o->remote_session = w->remote_session;
	o->world_loaded = w->world_loaded;
	o->body_ticking = w->body_ticking;
	o->screen = w->screen;
	o->focus = w->focus;
	o->install_gen = w->install_gen;
	o->timeout_admits = w->admits;
	o->fault = w->fault;
	o->x = w->x;
	o->y = w->y;
	o->z = w->z;
	o->at_rest = w->at_rest;
	o->in_water = w->in_water;
	fake_forward(w, &o->fwd_x, &o->fwd_z);
	o->dimension = w->dim;
	o->health = w->health;
	o->under_feet_type = w->under_feet;
	o->support_x = w->st.phase == MPP_PH_BUILD_DIG
		? w->st.dig_x : mpp_floor(w->x);
	o->support_y = w->st.phase == MPP_PH_BUILD_DIG
		? w->st.dig_y - 1 : mpp_floor(w->y) - 2;
	o->support_z = w->st.phase == MPP_PH_BUILD_DIG
		? w->st.dig_z : mpp_floor(w->z);
	o->support_known = !(w->c.unknown_support_cycles & (1u << w->st.cycle));
	o->support_safe = !(w->c.hollow_support_cycles & (1u << w->st.cycle));
	if(w->st.phase == MPP_PH_BUILD_DIG && w->dig_frames >= 10u
	   && (w->c.support_lost_dig_cycles & (1u << w->st.cycle)))
		o->support_safe = false;
	if(w->st.phase == MPP_PH_BUILD_DIG
	   && (w->c.stale_support_dig_cycles & (1u << w->st.cycle)))
		o->support_x++;
	o->echo_serial = w->echo_serial;
	memcpy(o->echoes, w->echoes, sizeof(o->echoes));
	o->watch_echo = w->watch_echo;
	memcpy(o->watch_hist, w->watch_hist, sizeof(o->watch_hist));
	o->watch_hist_n = w->watch_hist_n;
	o->watch_supported = !w->c.no_watch;
	o->txn_serial = w->txn_serial;
	memcpy(o->txns, w->txns, sizeof(o->txns));
	memcpy(o->inv, w->inv, sizeof(o->inv));
	o->cursor = w->cursor;
	o->held_hotbar = w->held;
	o->inv_focus_slot = w->screen == MPP_SCREEN_INVENTORY ? w->focus_slot : -1;
	o->last_click_action_id = w->click_id;
	o->login = w->login;
	/* crosshair: whatever the aim lock was last asked for */
	if(!w->aim_active)
		return;
	if(w->aim_pitch >= 80) { /* straight down: the cell under the feet */
		bool blind = w->c.no_dig_crosshair
			|| (w->c.skip_first_dig_then_return && w->snap_count <= 1u);
		if(w->c.pit_straddle_frac > 0.0f && w->have_last_dig) {
			float fx = mpp_frac(w->x), fz = mpp_frac(w->z);
			if(fx < 0.3f || fx > 0.7f || fz < 0.3f || fz > 0.7f)
				blind = true; /* the lower column, beyond reach */
		}
		if(w->under_feet != 0 && !blind) {
			o->target_valid = true;
			o->tx = mpp_floor(w->x);
			/* Mid-jump the ray still meets the GROUND the jump left from
			 * (the cell the body cleared is air), not the cell under the
			 * raised feet. */
			o->ty = (w->airborne ? mpp_floor(w->y_rest) : mpp_floor(w->y)) - 1;
			o->tz = mpp_floor(w->z);
			o->tface = MPP_FACE_TOP;
			o->target_top_clear = true;
			o->target_type = w->under_feet;
		}
	} else if(w->aim_pitch >= 45) {
		/* THE SURVEY, from the surface before any dig: a level neighbour shows
		 * its TOP face one below the feet (or the plant standing on it, at the
		 * feet level); a higher one shows a side face; a lower one is not the
		 * neighbour cell at all. */
		static const int sdx[4] = {0, -1, 0, 1};
		static const int sdz[4] = {1, 0, -1, 0};
		int d = ((w->aim_yaw % 360) + 360) % 360 / 90;
		bool level = (fake_level_dirs(w) >> d) & 1u;
		bool plant = ((w->c.plant_dirs >> d) & 1u) && !w->plant_gone[d];
		o->target_valid = true;
		o->tx = mpp_floor(w->x) + sdx[d];
		o->tz = mpp_floor(w->z) + sdz[d];
		if(!level) {
			o->ty = mpp_floor(w->y);
			o->tface = 2; /* a side face */
			o->target_top_clear = false;
			o->target_type = DIRT;
		} else if(plant) {
			o->ty = mpp_floor(w->y);
			o->tface = MPP_FACE_TOP;
			o->target_top_clear = true;
			o->target_type = w->c.plant_id ? w->c.plant_id : TALL_GRASS;
		} else {
			o->ty = mpp_floor(w->y) - 1;
			o->tface = MPP_FACE_TOP;
			o->target_top_clear = true;
			o->target_type = w->c.under_feet ? w->c.under_feet : GRASS;
		}
	} else if(!w->c.no_top_face
			  && w->st.cycle >= w->c.no_top_face_cycles) { /* a cardinal
		neighbour at the feet level */
		/* The CORE's yaw convention, the same one fake_forward uses: 0 = +Z,
		 * 90 = -X, 180 = -Z, 270 = +X. One convention in this fake, not two. */
		static const int dxs[4] = {0, -1, 0, 1};
		static const int dzs[4] = {1, 0, -1, 0};
		int d = ((w->aim_yaw % 360) + 360) % 360 / 90;
		bool plant = ((w->c.plant_dirs >> d) & 1u) && !w->plant_gone[d];
		unsigned exits = fake_level_dirs(w);
		o->target_valid = true;
		o->tx = mpp_floor(w->x) + dxs[d];
		o->tz = mpp_floor(w->z) + dzs[d];
		if(!((exits >> d) & 1u) || ((w->walled_dirs >> d) & 1u)) {
			/* natural ground one higher, or a block this script put there:
			 * a side face, one above the feet, nothing to place on */
			o->ty = mpp_floor(w->y) + 1;
			o->tface = 2; /* a side face: anything but MPP_FACE_TOP */
			o->target_top_clear = false;
			o->target_type = DIRT;
			return;
		}
		o->tface = MPP_FACE_TOP;
		o->target_top_clear = true;
		if(plant) {
			/* A plant STANDS ON the neighbour's ground block, so the crosshair
			 * reaches it one above the feet -- which is exactly the cell the
			 * block would have gone into, and why the place could never
			 * happen while it was there. */
			o->ty = mpp_floor(w->y) + 1;
			o->target_type = w->c.plant_id ? w->c.plant_id : TALL_GRASS;
		} else {
			o->ty = mpp_floor(w->y);
			/* The neighbour's own ground, not w->under_feet: the body is
			 * standing in a dug hole whose floor is STONE by then, while the
			 * neighbour still carries the surface this run walks on. */
			o->target_type = w->c.under_feet ? w->c.under_feet : GRASS;
		}
	}
}

/* ---- reaction ------------------------------------------------------------ */

static void do_click(struct world* w) {
	bool is_pick = w->cursor.item <= 0;
	if(is_pick && !w->c.pickups_before_pick_done && w->focus_slot >= 0
	   && w->focus_slot < MPP_INV_SLOTS && w->inv[w->focus_slot].item > 0) {
		w->c.pickups_before_pick_done = true;
		for(unsigned k = 0; k < w->c.pickups_before_pick; k++)
			give_item(w, w->inv[w->focus_slot].item, 1);
	}
	bool accept = is_pick ? !w->c.reject_pick : !w->c.reject_put;
	bool gen2 = w->st.gen == 2u;
	if(gen2 && w->c.post_click == 1)
		accept = false; /* Packet106 accepted=false (NetServerHandler:543) */
	w->click_id = (int16_t)(w->click_id + 1);
	if(!w->c.click_without_txn && !(gen2 && w->c.post_click == 2))
		push_txn(w, w->click_id, accept);
	if(!accept || w->focus_slot < 0 || w->focus_slot >= MPP_INV_SLOTS)
		return;
	struct mpp_item tmp = w->inv[w->focus_slot];
	w->inv[w->focus_slot] = w->cursor;
	w->cursor = tmp;
	if(!is_pick && w->c.wrong_slot_after_put)
		w->inv[w->focus_slot].item = GRAVEL; /* not what was moved */
	if(is_pick)
		for(unsigned k = 0; k < w->c.pickups_after_pick; k++)
			give_item(w, w->cursor.item, 1);
	if(is_pick && w->c.stray_pickup_item)
		give_item(w, w->c.stray_pickup_item, 1);
	else
		for(unsigned k = 0; k < w->c.pickups_after_put; k++)
			give_item(w, w->inv[w->focus_slot].item, 1);
}

static void apply_out(struct world* w) {
	unsigned a = w->out.actions;
	unsigned edge = a & ~w->prev_actions;
	w->use_refused = false;

	/* the watch is registered BEFORE this frame's buttons are applied, as the
	 * real glue does; a new cell forgets the old cell's echo */
	if(w->out.watch
	   && (!w->watch || w->out.watch_x != w->watch_x
		   || w->out.watch_y != w->watch_y || w->out.watch_z != w->watch_z)) {
		memset(&w->watch_echo, 0, sizeof(w->watch_echo));
		memset(w->watch_hist, 0, sizeof(w->watch_hist));
		w->watch_hist_n = 0;
	}
	w->watch = w->out.watch;
	w->watch_x = w->out.watch_x;
	w->watch_y = w->out.watch_y;
	w->watch_z = w->out.watch_z;

	if(w->out.aim) {
		w->aim_active = true;
		w->aim_yaw = w->out.yaw_deg;
		w->aim_pitch = w->out.pitch_deg;
		/* Requested now, IN FORCE next frame (see want_face_yaw). */
		w->want_face_yaw = w->out.yaw_deg;
	}
	if(w->out.login_start
	   && !(w->c.stale_login_gen2 && w->want_gen == 2)) {
		w->login_armed = true;
		w->login = MPP_LOGIN_RUNNING;
		w->login_t0_ms = w->now_ms;
	}
	/* WALKING, along the leg's fixed heading (c4488f36). The walker owns the
	 * look while it runs, so the facing follows walk_heading_deg, not the aim
	 * lock -- which is what autopilot_tick really does (mode 2 writes
	 * gstate.autopilot.yaw every tick). */
	if(w->out.walk) {
		/* A NEW LEG starts when the walker starts, or when the core turns the
		 * heading without building in between (a stuck leg followed by another
		 * walk leg). A pit leg that ends EITHER way counts as done. */
		bool new_leg = !w->walking
			|| w->out.walk_heading_deg != w->leg_heading;
		if(w->walking && new_leg && w->leg_is_pit)
			w->pit_legs_done++;
		if(new_leg) {
			w->leg_heading = w->out.walk_heading_deg;
			w->leg_is_pit = w->have_last_dig
				&& !(w->c.pit_depth && w->pit_rise >= w->c.pit_depth)
				&& (w->trapped || w->pit_legs_done < w->c.pit_legs);
		}
		w->want_face_yaw = w->out.walk_heading_deg;
		bool late_block = w->c.blocked_from_ms && w->st.measuring
			&& w->st.measured_ms >= w->c.blocked_from_ms;
		if(!w->c.blocked_leg && !w->leg_is_pit && !late_block) {
			float fx, fz;
			fake_forward(w, &fx, &fz);
			/* ~4 blocks/s: a 20 s leg covers 80 blocks */
			w->x += 0.064f * fx;
			w->z += 0.064f * fz;
		}
		w->at_rest = false;
		w->walking = true;
	} else if(w->walking) {
		w->walking = false;
		if(w->c.skip_first_dig_then_return && w->snap_count == 1u
		   && w->have_last_col) {
			/* the second walk leg comes back to the first column */
			snap_to_column(w);
			w->x = (float)w->last_col_x + 0.5f;
			w->z = (float)w->last_col_z + 0.5f;
		} else if(w->leg_is_pit) {
			/* STILL IN THE HOLE: same column, same depth, dirt under the feet,
			 * at rest. Nothing is reset -- that is the whole point. */
			w->pit_legs_done++;
			w->leg_is_pit = false;
			w->under_feet = DIRT;
			if(w->c.pit_straddle_frac > 0.0f && !w->straddled) {
				w->straddled = true;
				w->z = (float)mpp_floor(w->z) + w->c.pit_straddle_frac;
			}
			w->at_rest = true;
			w->rest_again_ms = 0;
			w->fwd_hold_ms = 0;
			w->tap_moved = false;
			w->drop_pending = 0;
		} else {
			snap_to_column(w);
			if(w->c.lands_in_last_dig_column && w->have_last_dig) {
				w->x = (float)w->last_dig_x + 0.5f;
				w->z = (float)w->last_dig_z + 0.5f;
				w->under_feet = DIRT;
			}
		}
	}
	/* CENTRING TAPS. FORWARD held across one 50 ms body tick moves the body
	 * ~0.2 blocks along the facing vector -- once per tap, however many 16 ms
	 * frames the tap happens to span -- and the body is then not at rest until
	 * friction has had ~400 ms. A tap with nowhere to go (never_centred) still
	 * costs the core a tap and still breaks its rest.
	 *
	 * THE THRESHOLD IS `50 - FRAME_MS`, NOT 50, and that is not a fudge: the
	 * core's tap is MPP_NUDGE_MS = 50 ms of wall time, and on this fake's
	 * 16 ms frame grid the last frame strictly inside that window sits at
	 * 48 ms. Requiring a full 50 here would model a game whose 20 Hz body tick
	 * can never fall inside a 50 ms button assertion -- which is false: a 50 ms
	 * window always contains exactly one 50 ms tick boundary. */
	/* THE PILLAR (cfg.pit_depth): a JUMP edge from rest lifts the body 1.1; a
	 * USE edge near the top of the jump, looking down, puts a block under it,
	 * and the landing 0.5 s later is one higher. A sealed box does neither. */
	if(w->c.pit_depth && w->have_last_dig) {
		if((edge & MPP_ACT_JUMP) && !w->airborne && w->at_rest
		   && w->pit_rise < w->c.pit_depth) {
			w->airborne = true;
			w->y_rest = w->y;
			w->y += 1.1f;
			w->at_rest = false;
			w->land_ms = w->now_ms + 500u;
		}
		bool feet_hit = w->obs.target_valid
			&& w->obs.tx == mpp_floor(w->x)
			&& w->obs.ty == mpp_floor(w->y_rest) - 1
			&& w->obs.tz == mpp_floor(w->z);
		if((edge & MPP_ACT_USE) && w->airborne && !feet_hit)
			w->pillar_use_no_target++; /* nothing is sent */
		if((edge & MPP_ACT_USE) && w->airborne && w->aim_pitch >= 80
		   && feet_hit && w->y >= w->y_rest + 1.0f
		   && w->inv[w->held].item > 0) {
			if(w->pillar_refused < w->c.pit_refuse_places) {
				w->pillar_refused++;
				w->use_refused = true; /* the place below spends nothing */
			} else {
				w->pit_rise++;
				w->y_rest += 1.0f;
			}
		}
		if(w->airborne && w->now_ms >= w->land_ms) {
			w->airborne = false;
			w->y = w->y_rest;
			w->at_rest = true;
		}
	}
	if(a & MPP_ACT_FORWARD) {
		if(edge & MPP_ACT_FORWARD)
			w->taps_seen++;
		w->fwd_hold_ms += FRAME_MS;
		if(w->fwd_hold_ms >= MPP_NUDGE_MS - FRAME_MS && !w->tap_moved) {
			w->tap_moved = true;
			if(!w->c.never_centred && !(w->c.pit_pinned && w->straddled)) {
				float fx, fz;
				fake_forward(w, &fx, &fz);
				w->x += 0.2f * fx;
				w->z += 0.2f * fz;
				w->taps_that_moved++;
			}
			w->at_rest = false;
			w->rest_again_ms = w->now_ms + SETTLE_MS;
		}
	} else {
		w->fwd_hold_ms = 0;
		w->tap_moved = false;
	}
	/* the dig is a HOLD, not a press */
	if((a & MPP_ACT_ATTACK) && w->obs.target_valid
	   && w->obs.tx == mpp_floor(w->x) && w->obs.ty == mpp_floor(w->y) - 1
	   && w->obs.tz == mpp_floor(w->z) && w->under_feet != 0) {
		w->hold_ms += FRAME_MS;
		if(w->hold_ms >= DIG_HOLD_MS) {
			int32_t bx = mpp_floor(w->x), by = mpp_floor(w->y) - 1,
					bz = mpp_floor(w->z);
			uint8_t old = w->under_feet;
			w->hold_ms = 0;
			if(w->have_last_dig && bx == w->last_dig_x && bz == w->last_dig_z)
				w->same_column_digs++;
			if(fake_level_dirs(w) == 0u && !w->trapped) {
				w->trapped = true; /* every neighbour is now two high or a drop */
				w->traps++;
			}
			w->have_last_dig = true;
			w->last_dig_x = bx;
			w->last_dig_z = bz;
			if(w->c.dig_echo_wrong_cell)
				push_echo(w, bx + 5, by, bz, 0, 0);
			else if(w->c.dig_echo_stale)
				push_echo(w, bx, by, bz, 0, w->echo_serial); /* <= baseline */
			else if(w->c.dig_echo_nonair)
				push_echo(w, bx, by, bz, STONE, 0);
			else if(!w->c.no_dig_echo)
				push_echo(w, bx, by, bz, 0, 0);
			if(w->bursts_done < w->c.echo_burst_legs) {
				w->bursts_done++;
				for(int k = 0; k < 20; k++)
					push_echo(w, bx + 100 + k, by, bz, STONE, 0);
			}
			if(w->floods_done < w->c.flood_after_dig_legs) {
				w->floods_done++;
				push_echo(w, bx, by, bz, 9, 0); /* water into the hole */
				for(int k = 0; k < 20; k++)
					push_echo(w, bx + 100 + k, by, bz, STONE, 0);
			}
			if(!w->c.no_dig_echo && !w->c.dig_echo_wrong_cell
			   && !w->c.dig_echo_stale && !w->c.dig_echo_nonair) {
				w->under_feet = STONE;
				/* THE FALL INTO THE DUG CELL. Immediate by default (what this
				 * fake always did); delayed or never, to drive the core's
				 * landing wait. The body is on the RIM until it happens, so
				 * mpp_floor(y) is still dig_y + 1 and aim_ready stays false. */
				if(w->c.never_lands)
					; /* stays on the rim for good */
				else if(w->c.drop_delay_frames)
					w->drop_pending = w->c.drop_delay_frames;
				else
					w->y -= 1.0f;
				w->pickup_pending = true;
				w->pickup_at_ms = w->now_ms + 100u;
				w->pickup_item = w->c.pickup_item_override
					? w->c.pickup_item_override
					: (old == GRASS ? DIRT : (int16_t)old);
			}
		}
	} else
		w->hold_ms = 0;
	/* THE PLANT BREAK (push-0921/B12): ATTACK held on the cell ONE ABOVE the
	 * feet -- where the block is to go -- rather than the dig cell one below,
	 * so the two holds can never be confused for each other. A plant is hardness
	 * 0 in Beta and goes in one tick; two frames here keeps it well inside
	 * MPP_CLEAR_BUDGET_MS so the test measures the CORE's budget, not the
	 * fake's. plant_never_clears models a server that never echoes it away. */
	/* THE WALL DIG of the escape (hw1021e: one block in hand, a ~5-block
	 * pocket): ATTACK held on a horizontal neighbour at feet or head level
	 * breaks it like any hand dig, and its drop reaches the inventory. */
	if((a & MPP_ACT_ATTACK) && w->st.phase == MPP_PH_ESCAPE
	   && w->obs.target_valid
	   && abs(w->obs.tx - mpp_floor(w->x)) + abs(w->obs.tz - mpp_floor(w->z))
		   == 1
	   && (w->obs.ty == mpp_floor(w->y) || w->obs.ty == mpp_floor(w->y) + 1)) {
		w->mine_hold_ms += FRAME_MS;
		if(w->mine_hold_ms >= DIG_HOLD_MS) {
			w->mine_hold_ms = 0;
			w->esc_mines_done++;
			push_echo(w, w->obs.tx, w->obs.ty, w->obs.tz, 0, 0);
			w->pickup_pending = true;
			w->pickup_at_ms = w->now_ms + 100u;
			w->pickup_item = DIRT;
		}
	} else if(w->st.phase == MPP_PH_ESCAPE)
		w->mine_hold_ms = 0;
	if((a & MPP_ACT_ATTACK) && w->obs.target_valid
	   && w->st.phase != MPP_PH_ESCAPE
	   && w->obs.ty == mpp_floor(w->y) + 1 && !w->c.plant_never_clears) {
		int pd = ((w->aim_yaw % 360) + 360) % 360 / 90;
		w->plant_hold_ms += FRAME_MS;
		if(w->plant_hold_ms >= 2u * FRAME_MS) {
			w->plant_gone[pd] = true;
			push_echo(w, w->obs.tx, w->obs.ty, w->obs.tz, 0, 0);
			w->plant_hold_ms = 0;
		}
	} else if(!(a & MPP_ACT_ATTACK))
		w->plant_hold_ms = 0;
	/* A place attempted BEFORE the body is standing in the cell it dug aims at
	 * geometry that is not there yet. The landing wait exists to make this
	 * zero; the test asserts it directly rather than inferring it from timing. */
	if((a & MPP_ACT_USE) && mpp_floor(w->y) != w->st.dig_y)
		w->use_before_landed++;
	/* discrete presses */
	if(edge & MPP_ACT_HOTBAR_NEXT)
		w->held = (w->held + 1) % 9;
	if((edge & MPP_ACT_USE) && w->screen == MPP_SCREEN_INGAME) {
		w->use_presses++;
		if(w->obs.target_valid && w->obs.tface == MPP_FACE_TOP
		   && w->inv[w->held].item > 0 && !w->c.no_place_echo
		   && !w->use_refused
		   && w->use_presses > w->c.unechoed_use_presses) {
			push_echo(w, w->obs.tx, w->obs.ty + 1, w->obs.tz,
					  (uint8_t)w->inv[w->held].item, 0);
			{
				int ddx = w->obs.tx - mpp_floor(w->x);
				int ddz = w->obs.tz - mpp_floor(w->z);
				int d = ddz == 1 ? 0 : ddx == -1 ? 1 : ddz == -1 ? 2 : 3;
				unsigned exits = w->c.exit_dirs ? w->c.exit_dirs : 0xFu;
				w->walled_dirs |= 1u << d;
				if((exits & ~w->walled_dirs) == 0u && !w->trapped) {
					w->trapped = true; /* no way out of this hole any more */
					w->traps++;
				}
			}
			if(--w->inv[w->held].count == 0)
				w->inv[w->held].item = 0;
		}
	}
	if((edge & MPP_ACT_OPEN_INVENTORY) && w->screen == MPP_SCREEN_INGAME
	   && !w->c.inv_never_opens) {
		w->screen = MPP_SCREEN_INVENTORY;
		w->focus_slot = 9; /* top-left of the main grid */
		if(w->c.post_fill_inv && w->st.gen == 2u)
			for(int k = 0; k < MPP_INV_SLOTS; k++)
				if(w->inv[k].item <= 0) {
					w->inv[k].item = DIRT;
					w->inv[k].count = 1;
				}
	}
	if((edge & MPP_ACT_CLOSE) && w->screen == MPP_SCREEN_INVENTORY) {
		w->screen = MPP_SCREEN_INGAME;
		w->focus_slot = -1;
	}
	if((edge & MPP_ACT_PAUSE) && w->screen == MPP_SCREEN_INGAME
	   && !w->c.pause_never_opens) {
		w->screen = MPP_SCREEN_PAUSE;
		w->focus = MPP_FOCUS_OTHER;
		w->pause_row = 0;
	}
	unsigned dirs = MPP_ACT_GUI_UP | MPP_ACT_GUI_DOWN | MPP_ACT_GUI_LEFT
		| MPP_ACT_GUI_RIGHT;
	if(edge & dirs) {
		if(w->screen == MPP_SCREEN_INVENTORY && !w->c.inv_focus_stuck) {
			int row, col;
			grid_rc(w->focus_slot < 0 ? 9 : w->focus_slot, &row, &col);
			if(edge & MPP_ACT_GUI_DOWN)
				row = (row + 1) % 4;
			else if(edge & MPP_ACT_GUI_UP)
				row = (row + 3) % 4;
			else if(edge & MPP_ACT_GUI_RIGHT)
				col = (col + 1) % 9;
			else
				col = (col + 8) % 9;
			w->focus_slot = grid_slot(row, col);
		} else if(w->screen == MPP_SCREEN_PAUSE) {
			if(++w->pause_row >= 2)
				w->focus = MPP_FOCUS_PAUSE_DISCONNECT;
		} else if(w->screen == MPP_SCREEN_TITLE && !w->c.rejoin_title_stuck) {
			if(edge & MPP_ACT_GUI_UP) {
				w->title_up_edges++;
				if(w->title_row > 0) w->title_row--;
			} else if(edge & MPP_ACT_GUI_DOWN) {
				w->title_down_edges++;
				if(w->title_row < 3) w->title_row++;
			}
			if(w->title_row == 0)
				w->focus = MPP_FOCUS_TITLE_MULTIPLAYER;
		} else if(w->screen == MPP_SCREEN_MP_SETUP && !w->c.rejoin_setup_stuck) {
			if(++w->mps_row >= 1)
				w->focus = MPP_FOCUS_MPS_JOIN;
		}
	}
	if(edge & MPP_ACT_GUI_CLICK) {
		if(w->screen == MPP_SCREEN_INVENTORY)
			do_click(w);
		else if(w->screen == MPP_SCREEN_PAUSE
				&& w->focus == MPP_FOCUS_PAUSE_DISCONNECT
				&& !w->c.leave_never_ends) {
			w->remote_session = false;
			w->body_torn_down = true;
			/* what the real glue reports for an absent body (review B17 H3):
			 * mpp_fill_body returns early and health stays the memset 0 */
			w->body_ticking = false;
			w->saved_health = w->health;
			w->health = 0;
			w->saved_x = w->x;
			w->saved_y = w->y;
			w->saved_z = w->z;
			w->x = 0.0f;
			w->y = -1.62f;
			w->z = 0.0f;
			w->screen = w->c.title_never ? MPP_SCREEN_OTHER : MPP_SCREEN_TITLE;
			w->focus = MPP_FOCUS_OTHER;
			w->title_row = w->c.rejoin_title_start_row
				? w->c.rejoin_title_start_row : 3;
		} else if(w->screen == MPP_SCREEN_TITLE
				  && w->focus == MPP_FOCUS_TITLE_MULTIPLAYER) {
			w->screen = MPP_SCREEN_MP_SETUP;
			w->focus = MPP_FOCUS_OTHER;
			w->mps_row = 0;
		} else if(w->screen == MPP_SCREEN_MP_SETUP
				  && w->focus == MPP_FOCUS_MPS_JOIN) {
			w->screen = MPP_SCREEN_MP_CONNECT;
			w->focus = MPP_FOCUS_UNKNOWN;
			w->joining = true;
			w->want_gen = 2;
			w->join_t0_ms = w->now_ms;
			w->login = w->c.stale_login_gen2 ? MPP_LOGIN_OK : MPP_LOGIN_IDLE;
			w->login_armed = false;
			w->world_loaded = false;
			w->body_ticking = false;
		}
	}
	w->prev_actions = a;
}

/* ---- frame stepper ------------------------------------------------------- */

static void step(struct world* w) {
	w->now_ms += FRAME_MS;
	w->frames++;
	/* THE BODY DRIFTS INTO A FLUID PART-WAY THROUGH A DIG (push-0921/B11).
	 * Counted in frames of MPP_PH_BUILD_DIG rather than milliseconds because
	 * the rule it exercises is itself a FRAME count (the core's
	 * dig_on_frames >= 30), and with the crosshair on the cell the two are the
	 * same number -- the core increments dig_on_frames on exactly the frames
	 * this counter advances. Set to 0 the moment the leg ends, so a later leg
	 * starts dry again. */
	if(w->st.phase == MPP_PH_BUILD_DIG) {
		w->dig_frames++;
		if(w->c.water_after_dig_frames
		   && w->dig_frames >= w->c.water_after_dig_frames)
			w->in_water = true;
	} else {
		w->dig_frames = 0;
	}
	/* The delayed fall into the dug cell (cfg.drop_delay_frames). */
	if(w->drop_pending && --w->drop_pending == 0)
		w->y -= 1.0f;
	world_timers(w);
	fill_obs(w);
	/* cfg.pit_wall_id: the pocket's walls, as the escape's scan meets them */
	if(w->c.pit_wall_id && w->st.phase == MPP_PH_ESCAPE
	   && w->obs.target_valid && w->obs.ty >= mpp_floor(w->y))
		w->obs.target_type = w->c.pit_wall_id;
	enum mpp_phase before = w->st.phase;
	mpp_tick(&w->st, &w->obs, &w->out);
	if(w->st.cycle < 32u
	   && (w->c.hollow_support_cycles | w->c.unknown_support_cycles
		| w->c.stale_support_dig_cycles) & (1u << w->st.cycle))
		if(w->out.actions & MPP_ACT_ATTACK)
			w->unsafe_support_attack_frames++;
	if(before == MPP_PH_BUILD_DIG && !w->obs.support_safe
	   && (w->out.actions & MPP_ACT_ATTACK))
		w->attack_after_support_loss++;
	/* markers: the impure half takes them and pops only what it sent */
	if(!w->c.never_pop)
		for(;;) {
			const struct mpp_marker* m = mpp_marker_peek(&w->st);
			if(!m)
				break;
			assert(w->nmarks < MAX_MARKS);
			w->marks[w->nmarks].seq = m->seq;
			w->marks[w->nmarks].at_ms = w->st.now_ms;
			snprintf(w->marks[w->nmarks].key, MPP_MARKER_KEY, "%s", m->key);
			snprintf(w->marks[w->nmarks].body, MPP_MARKER_BODY, "%s", m->body);
			w->nmarks++;
			mpp_marker_pop(&w->st);
		}
	/* press hygiene, checked on EVERY frame of EVERY case (case e) */
	unsigned a = w->out.actions;
	if(a & (a - 1u))
		w->multi_action_frames++;
	if((a & ~w->prev_actions) && w->prev_actions != 0)
		w->edge_without_release++;
	if((a & ~w->prev_actions & MPP_ACT_USE) && before == MPP_PH_BUILD_PLACE)
		w->use_edges_in_place++;
	if(a & ~w->prev_actions & MPP_ACT_GUI_CLICK) {
		if(before == MPP_PH_INV_PICK)
			w->click_edges_in_pick++;
		if(before == MPP_PH_INV_PUT)
			w->click_edges_in_put++;
	}
	/* a terrain-population burst at the very start of the dig leg */
	if(w->c.early_burst && w->st.phase == MPP_PH_BUILD_DIG
	   && before != MPP_PH_BUILD_DIG)
		for(int k = 0; k < 20; k++)
			push_echo(w, w->st.dig_x + 100 + k, w->st.dig_y, w->st.dig_z,
					  STONE, 0);
	/* REVIEW B21 M2: one ATTACK frame on the cell, 400 ms off it, then a
	 * foreign transition to air */
	if(w->c.touch_then_drift && w->st.phase == MPP_PH_BUILD_DIG) {
		if(w->dig_frames == 2u)
			w->c.no_dig_crosshair = true; /* the aim drifts off the cell */
		if(w->dig_frames == 2u + 400u / FRAME_MS && w->under_feet != 0) {
			w->c.no_dig_crosshair = false;
			w->c.touch_then_drift = false;
			push_echo(w, w->st.dig_x, w->st.dig_y, w->st.dig_z, 0, 0);
			w->under_feet = STONE;
			w->y -= 1.0f;
			w->hold_ms = 0;
		}
	}
	/* REVIEW B17 M1: something else empties the dig cell two frames in. */
	if(w->st.phase == MPP_PH_BUILD_DIG && w->dig_frames == 2u
	   && w->foreign_digs_done < w->c.foreign_dig_legs && w->under_feet != 0) {
		w->foreign_digs_done++;
		push_echo(w, w->st.dig_x, w->st.dig_y, w->st.dig_z, 0, 0);
		w->under_feet = STONE;
		w->y -= 1.0f;
		w->hold_ms = 0;
	}
	/* REVIEW B17 M2: water reaches the place cell before the block does. */
	if(w->st.phase == MPP_PH_BUILD_PLACE && before != MPP_PH_BUILD_PLACE
	   && w->foreign_places_done < w->c.foreign_place_legs) {
		w->foreign_places_done++;
		push_echo(w, w->st.place_x, w->st.place_y, w->st.place_z, 9, 0);
	}
	if(w->st.phase != before) {
		if(w->st.phase == MPP_PH_BUILD_PLACE)
			w->n_place_legs++;
		if(w->st.phase == MPP_PH_INV_PICK)
			w->n_pick_legs++;
		if(w->st.phase == MPP_PH_INV_PUT)
			w->n_put_legs++;
	}
	apply_out(w);
}

static void run(struct world* w) {
	while(!mpp_done(&w->st) && w->frames < FRAME_CAP)
		step(w);
	assert(w->frames < FRAME_CAP); /* every path must terminate */
}

static void run_to_phase(struct world* w, enum mpp_phase p) {
	while(!mpp_done(&w->st) && w->st.phase != p && w->frames < FRAME_CAP)
		step(w);
	assert(w->frames < FRAME_CAP);
}

/* Diagnostic dump, printed whenever a case ends somewhere unexpected. */
static void dump(const struct world* w, const char* where) {
	printf("  -- %s: phase=%s step=%s diag=\"%s\" now=%u measured=%u cycle=%u "
		   "digs=%u picks=%u places=%u moves=%u skipped=%u frames=%u "
		   "pos=(%.4f,%.4f) face=%d taps=%u/%u rest=%d water=%d\n",
		   where, mpp_phase_name(w->st.phase), w->st.fail_step, w->st.fail_diag,
		   w->st.now_ms, w->st.measured_ms, w->st.cycle, w->st.digs,
		   w->st.picks, w->st.places, w->st.inv_moves, w->st.build_skipped,
		   w->frames, (double)w->x, (double)w->z, w->face_yaw,
		   w->taps_that_moved, w->taps_seen, (int)w->at_rest,
		   (int)w->in_water);
	unsigned from = w->nmarks > 8 ? w->nmarks - 8 : 0;
	for(unsigned k = from; k < w->nmarks; k++)
		printf("     [%u] %s %s\n", w->marks[k].seq, w->marks[k].key,
			   w->marks[k].body);
}

/* Assert the run failed with exactly this phase name and diag. */
static void expect_fail(struct world* w, const char* phase, const char* diag,
						const char* where) {
	if(w->st.phase != MPP_PH_FAILED || strcmp(w->st.fail_step, phase) != 0
	   || strcmp(w->st.fail_diag, diag) != 0) {
		printf("  !! %s: expected fail(%s,\"%s\"), got phase=%s step=%s "
			   "diag=\"%s\"\n",
			   where, phase, diag, mpp_phase_name(w->st.phase),
			   w->st.fail_step, w->st.fail_diag);
		assert(0 && "wrong failure");
	}
	const struct rec* f = last_mark(w, "fail");
	assert(f && "a failure must emit the fail marker");
	assert(body_has(f->body, "\"phase\":\"") && body_has(f->body, "\"diag\":\""));
}

static double body_f(const char* body, const char* key) {
	const char* p = strstr(body, key);
	return p ? strtod(p + strlen(key), NULL) : -1e30;
}

static int count_entries(const char* slots_body) {
	int n = 0;
	for(const char* p = slots_body; *p; p++)
		if(*p == '[')
			n++;
	return n - 1; /* the opening bracket of "slots":[ */
}

/* ---- (a) the happy path -------------------------------------------------- */

static int third_of(long measured_ms) {
	if(measured_ms < 0)
		return -1;
	if(measured_ms < (long)MPP_MEASURED_PLAY_MS / 3)
		return 0;
	if(measured_ms < 2 * (long)MPP_MEASURED_PLAY_MS / 3)
		return 1;
	return 2;
}

static uint32_t g_happy_total_ms;

static void run_cfg(struct cfg* c) {
	world_init(&W, c);
	run(&W);
}

static void case_happy_path(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	world_init(&W, &c); /* FRESH fixture world: the inventory starts empty */
	run(&W);

	if(W.st.phase != MPP_PH_OK)
		dump(&W, "happy_path");
	assert(W.st.phase == MPP_PH_OK);
	g_happy_total_ms = W.st.now_ms;

	/* seq gapless from 1 */
	for(unsigned k = 0; k < W.nmarks; k++)
		assert(W.marks[k].seq == k + 1u);

	/* THE LEAVE IS NOT A TELEPORT. The session teardown moves the local body
	 * to (0, -1.62, 0); the first real rejoin (short-play PC run 180120) put a
	 * "teleport" marker and "teleports":1 into an otherwise clean tape. */
	if(mark_count(&W, "teleport") != 0u)
		dump(&W, "happy_path teleport");
	assert(mark_count(&W, "teleport") == 0u);
	assert(W.st.teleports == 0u);

	/* bare marker keys, in draft-2 order (ready -> login_begin -> login_ok) */
	const char* head[] = {"session", "ready", "login_begin", "login_ok",
						  "p2_begin"};
	for(unsigned k = 0; k < sizeof(head) / sizeof(head[0]); k++)
		assert(strcmp(W.marks[k].key, head[k]) == 0);
	const char* tail[] = {"p2_end",	 "snapshot_pre", "inv",
						  "inv",		 "inv",			 "disconnected",
						  "rejoin_begin", "ready",		 "login_begin",
						  "login_ok",	 "snapshot_post", "inv",
						  "inv",		 "inv",			 "verified",
						  "post_action", "ok"};
	unsigned nt = sizeof(tail) / sizeof(tail[0]);
	assert(W.nmarks > nt);
	for(unsigned k = 0; k < nt; k++)
		assert(strcmp(W.marks[W.nmarks - nt + k].key, tail[k]) == 0);

	/* floors */
	assert(W.st.digs >= MPP_FLOOR_DIGS);
	assert(W.st.places >= MPP_FLOOR_PLACES);
	assert(W.st.inv_moves == MPP_FLOOR_INV_MOVES);
	assert(W.st.picks == W.st.digs);
	assert(W.st.build_skipped == 0);

	/* draft 2b RESERVE RULE: the first cycle digs but keeps its block, so the
	 * player never ends measured play empty-handed. reserve_kept is NOT a
	 * build_skipped and NOT a failure. */
	unsigned reserved = mark_count(&W, "reserve_kept");
	assert(reserved == 1);
	int ri = mark_index(&W, "reserve_kept", 0);
	assert(body_num(W.marks[ri].body, "\"cycle\":") == 0);
	assert(body_num(W.marks[ri].body, "\"held\":") == 1);
	assert(mark_index(&W, "place", 0) > ri); /* no place before the reserve */
	assert(W.st.places == W.st.digs - 1u);
	assert(W.st.build_skipped == 0); /* the reserve is not a skip */

	const struct rec* p2 = last_mark(&W, "p2_end");
	assert(p2);
	long measured = body_num(p2->body, "\"measured_ms\":");
	assert(measured >= (long)MPP_MEASURED_PLAY_MS);
	assert(body_num(p2->body, "\"digs\":") == (long)W.st.digs);
	assert(body_num(p2->body, "\"inv_moves\":") == (long)MPP_FLOOR_INV_MOVES);

	/* spread rule (contract section 5) on the REAL happy path */
	int dig_third[3] = {0, 0, 0}, place_third[3] = {0, 0, 0},
		inv_third[3] = {0, 0, 0};
	for(unsigned k = 0; k < W.nmarks; k++) {
		int t = third_of(body_num(W.marks[k].body, "\"measured_ms\":"));
		if(t < 0)
			continue;
		if(strcmp(W.marks[k].key, "dig") == 0)
			dig_third[t]++;
		else if(strcmp(W.marks[k].key, "place") == 0)
			place_third[t]++;
		else if(strcmp(W.marks[k].key, "inv_move") == 0)
			inv_third[t]++;
	}
	/* HEADER DEFECT 1 */
	EXPECT(1, dig_third[0] && dig_third[1] && dig_third[2], "happy_path",
		   "a matched dig is missing from at least one third of measured play");
	/* HEADER DEFECT 2 */
	EXPECT(2, place_third[0] && place_third[1] && place_third[2], "happy_path",
		   "a matched place is missing from at least one third of measured "
		   "play");
	int inv_spread = (inv_third[0] > 0) + (inv_third[1] > 0)
		+ (inv_third[2] > 0);
	/* HEADER DEFECT 3 */
	EXPECT(3, inv_spread >= 2, "happy_path",
		   "the four inventory moves do not fall in at least two different "
		   "thirds of measured play");

	assert(W.st.now_ms < MPP_TOTAL_CAP_MS);

	/* press hygiene (case e) over the whole run */
	assert(W.multi_action_frames == 0);
	assert(W.edge_without_release == 0);
	assert(W.n_place_legs == W.st.places);
	assert(W.use_edges_in_place == W.n_place_legs);
	/* + the one post-reconnect move (REL-14b), not counted in inv_moves */
	assert(W.n_pick_legs == MPP_FLOOR_INV_MOVES + 1u);
	assert(W.n_put_legs == MPP_FLOOR_INV_MOVES + 1u);
	{
		const struct rec* pa = last_mark(&W, "post_action");
		assert(pa && body_num(pa->body, "\"gen\":") == 2);
		/* fresh gen-2 ids after the reconnect reset: the first click is 0 */
		assert(body_num(pa->body, "\"action_pick\":") == 0);
		assert(body_num(pa->body, "\"action_put\":") == 1);
	}
	assert(W.click_edges_in_pick == W.n_pick_legs);
	assert(W.click_edges_in_put == W.n_put_legs);

	/* (d) no stale click id is ever taken for a new move, and draft 2's
	 * PROTOCOL slot numbers (hotbar 36..44) are what goes on the wire */
	long prev_put = -1;
	for(unsigned k = 0; k < W.nmarks; k++) {
		if(strcmp(W.marks[k].key, "inv_move") != 0)
			continue;
		long ap = body_num(W.marks[k].body, "\"action_pick\":");
		long au = body_num(W.marks[k].body, "\"action_put\":");
		long fs = body_num(W.marks[k].body, "\"from_slot\":");
		long ts = body_num(W.marks[k].body, "\"to_slot\":");
		assert(ap > 0 && au > ap && ap > prev_put);
		prev_put = au;
		/* HEADER DEFECT 4 */
		EXPECT(4, fs >= 36 && fs <= 44 && ts >= 36 && ts <= 44, "happy_path",
			   "inv_move from_slot / to_slot are not PROTOCOL window-0 slot "
			   "numbers (hotbar 36..44), so no fixture click receipt matches");
	}

	/* session body: {contract, measured_floor_ms, total_cap_ms} */
	assert(body_num(W.marks[0].body, "\"contract\":") == (long)MPP_CONTRACT);
	/* HEADER DEFECT 5 */
	EXPECT(5,
		   body_num(W.marks[0].body, "\"measured_floor_ms\":")
				   == (long)MPP_MEASURED_PLAY_MS
			   && body_num(W.marks[0].body, "\"total_cap_ms\":")
				   == (long)MPP_TOTAL_CAP_MS,
		   "happy_path",
		   "session body is not {contract, measured_floor_ms, total_cap_ms}");

	/* snapshots: head {gen,x,y,z,dim,parts:3} + MPP_INV_PARTS `inv` markers */
	int si = mark_index(&W, "snapshot_pre", 0);
	assert(si >= 0);
	/* HEADER DEFECT 6 */
	EXPECT(6,
		   body_num(W.marks[si].body, "\"parts\":") == (long)MPP_INV_PARTS
			   && !body_has(W.marks[si].body, "\"inv_hash\":"),
		   "happy_path",
		   "snapshot_pre head is not {gen,x,y,z,dim,parts:3} without "
		   "inv_hash");
	int snap_listed = 0;
	for(unsigned k = 0; k < MPP_INV_PARTS; k++) {
		const struct rec* part = &W.marks[si + 1 + k];
		assert(strcmp(part->key, "inv") == 0);
		assert(body_has(part->body, "\"of\":\"snapshot_pre\""));
		assert(body_num(part->body, "\"part\":") == (long)k);
		assert(body_has(part->body, "\"slots\":["));
		assert(strlen(part->body) < MPP_MARKER_BODY - 1); /* never truncated */
		snap_listed += count_entries(part->body);
	}
	/* the collector REQUIRES a non-empty pre-disconnect snapshot: an
	 * empty-equals-empty compare across the rejoin proves nothing */
	/* HEADER DEFECT 10 */
	EXPECT(10, snap_listed >= 1, "happy_path",
		   "snapshot_pre lists no occupied slot, so the rejoin compares an "
		   "empty inventory against an empty one");

	printf("PASS (a) happy path (fresh world): phase=ok, cycles=%u digs=%u "
		   "places=%u reserve_kept=%u inv_moves=%u build_skipped=%u, "
		   "measured=%ld ms, total=%u ms, %u markers, snapshot_pre holds %d "
		   "slot(s)\n",
		   W.st.cycle, W.st.digs, W.st.places, reserved, W.st.inv_moves,
		   W.st.build_skipped, measured, W.st.now_ms, W.nmarks, snap_listed);
	printf("    dig thirds %d/%d/%d, place thirds %d/%d/%d, inv thirds "
		   "%d/%d/%d\n",
		   dig_third[0], dig_third[1], dig_third[2], place_third[0],
		   place_third[1], place_third[2], inv_third[0], inv_third[1],
		   inv_third[2]);
}

static void case_title_up_from_lower_rows(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	run_cfg(&c); /* Quit row, below Play. */
	assert(W.st.phase == MPP_PH_OK);
	assert(W.title_up_edges >= 3 && W.title_down_edges == 0);
	memset(&c, 0, sizeof(c));
	c.rejoin_title_start_row = 2; /* Options, a middle row. */
	run_cfg(&c);
	assert(W.st.phase == MPP_PH_OK);
	assert(W.title_up_edges >= 2 && W.title_down_edges == 0);
	puts("PASS rejoin title: UP reaches observed Play from Quit and Options, then click advances");
}

static void case_pointer_ownership_boundaries(void) {
	struct mpp_state s;
	mpp_reset(&s);
	assert(!mpp_pointer_script_owns(false, &s));
	s.phase = MPP_PH_PAUSE_REST;
	assert(!mpp_pointer_script_owns(true, &s));
	s.phase = MPP_PH_PAUSE_OPEN;
	assert(mpp_pointer_script_owns(true, &s));
	s.phase = MPP_PH_REJOIN_TITLE;
	assert(mpp_pointer_script_owns(true, &s));
	s.phase = MPP_PH_REJOIN_SETUP;
	assert(mpp_pointer_script_owns(true, &s));
	s.phase = MPP_PH_READY;
	assert(!mpp_pointer_script_owns(true, &s));
	s.phase = MPP_PH_FAILED;
	assert(!mpp_pointer_script_owns(true, &s));
	s.phase = MPP_PH_OK;
	assert(!mpp_pointer_script_owns(true, &s));
	mpp_reset(&s);
	assert(!mpp_pointer_script_owns(true, &s));
	puts("PASS pointer ownership: menu phases only; unarmed, ready, failed, ok and reset restore native query");
}

/* The margin draft 2 bought by dropping the fixed cycle count: two build legs
 * lost to terrain the script cannot use must NOT cost the run. */
static void case_skip_margin(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	/* FIVE, not two (push-0921/B16): the first PC run that ever passed
	 * (pc_mp_play_20260921_181008) lost 4 of its 12 build legs to terrain
	 * (3x stone under the feet, 1x no usable top face) and met the place floor
	 * with EXACTLY 8. A proof that passes on luck is not one to spend a console
	 * window on: real terrain must be allowed to refuse five legs.
	 *
	 * NINE since 18:55: the second passing run (pc_mp_play_20260921_184003,
	 * every safety rule in) again ended on exactly 8 places -- 13 cycles, 4
	 * legs refused by terrain, 8 legs lost to shore brakes. */
	c.skip_n_builds = 9;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "skip_margin");
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.build_skipped == 9);
	assert(mark_count(&W, "reserve_kept") == 1); /* still exactly one reserve */
	assert(W.st.digs >= MPP_FLOOR_DIGS);
	assert(W.st.places >= MPP_FLOOR_PLACES);
	assert(W.st.places == W.st.digs - 1u);
	assert(W.st.inv_moves == MPP_FLOOR_INV_MOVES);
	printf("PASS (a2) skip margin: 9 skipped build legs + the reserve cycle -> "
		   "cycles=%u digs=%u places=%u reserve_kept=%u inv_moves=%u "
		   "build_skipped=%u, total=%u ms, floors still met\n",
		   W.st.cycle, W.st.digs, W.st.places, mark_count(&W, "reserve_kept"),
		   W.st.inv_moves, W.st.build_skipped, W.st.now_ms);
}

/* The inventory moves of the run in W must fall in >= 2 thirds of measured
 * play (the collector's spread rule). */
static void assert_inv_spread(void) {
	int th[3] = {0, 0, 0};
	for(unsigned k = 0; k < W.nmarks; k++) {
		if(strcmp(W.marks[k].key, "inv_move") != 0)
			continue;
		int t = third_of(body_num(W.marks[k].body, "\"measured_ms\":"));
		if(t >= 0)
			th[t]++;
	}
	assert((th[0] > 0) + (th[1] > 0) + (th[2] > 0) >= 2);
}

/* hw1020h_5fa275d_6: 11 skipped build legs (5 grass, 4 sand, 1 leaves, 1 off
 * column) cost ~22 s each at a full 20 s leg and left digs=8 places=7 inv=2.
 * A skip now retries on a short leg and every successful cycle after a skip
 * moves a stack, so the floors are reachable with 11 skips. */
static void case_skip_margin_eleven(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	c.skip_n_builds = 11;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "skip_margin_eleven");
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.build_skipped == 11);
	assert(W.st.digs >= MPP_FLOOR_DIGS);
	assert(W.st.places >= MPP_FLOOR_PLACES);
	assert(W.st.inv_moves == MPP_FLOOR_INV_MOVES);
	assert_inv_spread();
	/* a skip is followed by a short retry leg, never a full one: no build
	 * skipped, then a build skipped again, can be further apart than a short
	 * leg plus a survey if every gap after a skip stays under a full leg */
	{
		int sk = mark_index(&W, "build_skipped", 0);
		assert(sk >= 0);
		int nx = mark_index(&W, "build_skipped", (unsigned)sk + 1u);
		assert(nx > sk);
		/* the next skip's marker comes before a full leg has elapsed */
		assert(body_num(W.marks[nx].body, "\"measured_ms\":")
			   - body_num(W.marks[sk].body, "\"measured_ms\":")
			< (long)MPP_WALK_LEG_MS);
	}
	printf("PASS (a2b) skip margin: 11 skipped build legs -> cycles=%u digs=%u "
		   "places=%u inv_moves=%u, total=%u ms, floors met\n",
		   W.st.cycle, W.st.digs, W.st.places, W.st.inv_moves, W.st.now_ms);
}

/* ---- (a3) the reserve rule itself ---------------------------------------- */

static void case_reserve_rule(void) {
	struct cfg c;

	/* A drop that is not a placeable block (item > 255) can never satisfy the
	 * reserve, so no cycle ever places -- and that is a floor failure, never a
	 * skip and never a crash. */
	memset(&c, 0, sizeof(c));
	c.pickup_item_override = 300;
	run_cfg(&c);
	expect_fail(&W, "walk", "action floors not met", "non-placeable drop");
	assert(W.st.places == 0);
	assert(!has_mark(&W, "place"));
	assert(W.st.build_skipped == 0); /* a reserve is not a skip */
	assert(mark_count(&W, "reserve_kept") == W.st.cycle);
	assert(body_num(last_mark(&W, "reserve_kept")->body, "\"held\":") == 0);
	assert(W.st.digs >= MPP_FLOOR_DIGS && W.st.picks == W.st.digs);
	/* the inventory leg still ran on the odd cycles: a non-placeable stack is
	 * still a stack to move */
	assert(W.st.inv_moves == MPP_FLOOR_INV_MOVES);
	unsigned nonplaceable_cycles = W.st.cycle;

	/* The inventory leg on an odd cycle must still run when the ONLY stack is
	 * the one-block reserve. One skipped build leg puts the first pickup on
	 * cycle 1, so the leg opens holding exactly one block and the cycle then
	 * ends on the reserve instead of a place. */
	memset(&c, 0, sizeof(c));
	c.skip_n_builds = 1;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "reserve_one_block");
	assert(W.st.phase == MPP_PH_OK);
	int im = mark_index(&W, "inv_move", 0);
	assert(im >= 0);
	assert(body_num(W.marks[im].body, "\"count\":") == 1);
	int rk = mark_index(&W, "reserve_kept", (unsigned)im);
	assert(rk > im); /* the reserve ended that cycle, after the move */
	assert(mark_index(&W, "place", 0) > im); /* nothing was placed before it */
	assert(W.st.build_skipped == 1);
	assert(W.st.inv_moves == MPP_FLOOR_INV_MOVES);
	assert_inv_spread();
	assert(W.st.places == W.st.digs - 1u);

	printf("PASS (a3) reserve rule: a non-placeable drop keeps the reserve on "
		   "all %u cycles (0 places, 0 skips, %u inv_moves); a one-block "
		   "reserve still runs the odd-cycle inventory leg (first move "
		   "count=1) and then ends the cycle on reserve_kept\n",
		   nonplaceable_cycles, MPP_FLOOR_INV_MOVES);
}

/* ---- (b) one case per latch / budget ------------------------------------- */

static void case_latches(void) {
	struct cfg c;

	memset(&c, 0, sizeof(c));
	c.zero_health_at_ms = (int32_t)(MPP_WALK_LEG_MS / 2u); /* inside the FIRST walk leg, whatever its length */
	run_cfg(&c);
	expect_fail(&W, "walk", "died", "health 0");

	memset(&c, 0, sizeof(c));
	c.death_screen_at_ms = (int32_t)(MPP_WALK_LEG_MS / 2u);
	run_cfg(&c);
	expect_fail(&W, "walk", "died", "death screen");

	memset(&c, 0, sizeof(c));
	c.lose_session_at_ms = (int32_t)(MPP_WALK_LEG_MS / 2u);
	run_cfg(&c);
	expect_fail(&W, "walk", "session lost", "session lost in play");

	memset(&c, 0, sizeof(c));
	c.fault_at_ms = (int32_t)(MPP_WALK_LEG_MS / 2u);
	run_cfg(&c);
	expect_fail(&W, "walk", "fault latch", "fault latch");

	/* the same drop during MPP_PH_PAUSE_LEAVE is the NORMAL exit */
	memset(&c, 0, sizeof(c));
	c.drop_session_in_pause_leave = true;
	run_cfg(&c);
	assert(W.st.phase == MPP_PH_OK);
	assert(has_mark(&W, "disconnected"));
	assert(!has_mark(&W, "fail"));

	printf("PASS (b1) latches: health 0 / death screen / session lost / fault "
		   "all fail by name; a PAUSE_LEAVE drop does not\n");
}

static void case_ready_and_login(void) {
	struct cfg c;

	memset(&c, 0, sizeof(c));
	c.admits1 = 1;
	run_cfg(&c);
	expect_fail(&W, "ready", "load released by the 30 s backstop",
				"admits gen 1");
	assert(body_num(last_mark(&W, "ready")->body, "\"timeout_admits\":") == 1);

	memset(&c, 0, sizeof(c));
	c.admits2 = 1;
	run_cfg(&c);
	expect_fail(&W, "ready", "load released by the 30 s backstop",
				"admits gen 2");
	assert(mark_count(&W, "ready") == 2);

	memset(&c, 0, sizeof(c));
	c.login_fail = true;
	run_cfg(&c);
	expect_fail(&W, "login", "login machine failed", "login FAILED");

	memset(&c, 0, sizeof(c));
	c.login_never = true;
	run_cfg(&c);
	expect_fail(&W, "login", "login timeout", "login never answers");

	memset(&c, 0, sizeof(c));
	c.login_delay_ms = MPP_LOGIN_DEADLINE_MS + 1000u;
	run_cfg(&c);
	expect_fail(&W, "login", "login past the server deadline", "login late");
	assert(body_num(last_mark(&W, "login_ok")->body, "\"since_connect_ms\":")
		   >= (long)MPP_LOGIN_DEADLINE_MS);

	memset(&c, 0, sizeof(c));
	c.ready_delay_ms = MPP_READY_BUDGET_MS + 5000u;
	run_cfg(&c);
	expect_fail(&W, "ready", "world never became ready", "ready budget");

	/* REL-14b: a silent vanilla login (no ACK of any kind, only the
	 * machine's RUNNING -> OK) is what the happy path already rides; here a
	 * gen-2 login still PARKED from gen 1 must never be credited ... */
	memset(&c, 0, sizeof(c));
	c.stale_login_gen2 = true;
	run_cfg(&c);
	expect_fail(&W, "login", "stale login generation", "stale login gen 2");
	assert(W.st.gen == 2u && mark_count(&W, "login_ok") == 1u);

	/* ... nor a gen-2 world whose install generation never advanced */
	memset(&c, 0, sizeof(c));
	c.stale_install_gen2 = true;
	run_cfg(&c);
	expect_fail(&W, "ready", "world never became ready", "stale install");
	assert(W.st.gen == 2u && mark_count(&W, "ready") == 1u);

	printf("PASS (b2) ready/login: timeout_admits==1 both gens, login FAILED, "
		   "login timeout, since_connect_ms past the 60 s deadline, ready "
		   "budget\n");
}

static void case_build_budgets(void) {
	struct cfg c;

	/* THE TWO STORIES A DIG LEG CAN END WITH, split by 5e4f6f3c and pinned
	 * here (push-0921/B11). Both used to FAIL the run; only one of them is
	 * evidence of anything.
	 *
	 * (1) ATTACK really was held on the cell and the server never echoed the
	 * break: client and server DISAGREE about that block, which is a defect
	 * and must end the run. This is not hypothetical -- it is exactly how the
	 * Beta dig-speed parity gap was caught on 2026-09-21 (the client applies
	 * neither of Beta's water / airborne /5 penalties, so it declares the
	 * break finished while mc173 is still counting and logs "incoherent break
	 * time"; brief B10). A run that quietly skipped that leg would have hidden
	 * it, so the failure is load-bearing, not strictness for its own sake. */
	memset(&c, 0, sizeof(c));
	c.no_dig_echo = true;
	run_cfg(&c);
	expect_fail(&W, "build_dig", "dig never echoed by the server", "no echo");
	assert(W.st.digs == 0);
	/* It failed rather than skipping, and the leg really was held: the whole
	 * 12 s budget at FRAME_MS is far past the core's 30-frame threshold. */
	assert(W.st.dig_on_frames >= 30u);
	assert(mark_count(&W, "build_skipped") == 0u);

	/* (2) The crosshair was (almost) never ON the cell, so nothing was ever
	 * attempted: the body moved off its column (a slope, a current, a push).
	 * That is terrain, not a disagreement -- a NAMED SKIP, and the run carries
	 * on to fail honestly at p2_end on the action floors instead. Run
	 * pc_mp_play_20260921_144523 died here after 475 s of otherwise healthy
	 * play, which is what motivated the split. */
	memset(&c, 0, sizeof(c));
	c.no_dig_crosshair = true;
	run_cfg(&c);
	expect_fail(&W, "walk", "action floors not met", "no crosshair");
	assert(body_has(last_mark(&W, "build_skipped")->body,
					"crosshair never on the dig cell"));
	assert(W.st.dig_on_frames == 0u); /* never once on target */
	assert(W.st.digs == 0 && W.st.build_skipped >= 10u);
	assert(has_mark(&W, "p2_end"));
	assert(mark_index(&W, "fail", 0) > mark_index(&W, "p2_end", 0));

	/* (3) The body drifts into a fluid PART-WAY THROUGH the dig, before any
	 * real digging has happened. Skipped "in water" by name -- and this is the
	 * case that must NOT be allowed to reach story (1), because digging from
	 * water is precisely where the client and the server disagree today. */
	memset(&c, 0, sizeof(c));
	c.water_after_dig_frames = 1; /* wet from the first BUILD_DIG frame */
	c.no_dig_echo = true;		  /* so nothing can complete the dig instead */
	run_cfg(&c);
	expect_fail(&W, "walk", "action floors not met", "water during the dig");
	assert(body_has(last_mark(&W, "build_skipped")->body, "in water"));
	assert(W.st.digs == 0);
	assert(has_mark(&W, "p2_end"));

	/* (4) THE BOUNDARY, stated as a number so it cannot drift: the water
	 * excuse expires. Wet only AFTER the leg has held on the cell for more
	 * than 30 frames, the leg is no longer "a body that drifted off" -- it
	 * really was digging -- so it falls through to story (1) and FAILS. A
	 * looser rule here would let a genuine client/server disagreement be
	 * written off as weather. */
	memset(&c, 0, sizeof(c));
	c.water_after_dig_frames = 40;
	c.no_dig_echo = true;
	run_cfg(&c);
	expect_fail(&W, "build_dig", "dig never echoed by the server",
				"water after the threshold");
	assert(W.st.dig_on_frames >= 30u);
	assert(mark_count(&W, "build_skipped") == 0u);

	/* The no_pickup case MOVED to case_pickup_missed (push-0921/B12). It used
	 * to pin fail(build_pickup, "dug item never reached the inventory") on the
	 * FIRST miss; 8bf54845 made a single miss narration, because on a remote
	 * server the drop is the server's physics and one real run lost 1 of 4 with
	 * the block provably broken under the feet. The failure still exists, with
	 * a different diag and a threshold, and is pinned there rather than here so
	 * the two halves of that ruling sit together. */

	memset(&c, 0, sizeof(c));
	c.no_place_echo = true;
	run_cfg(&c);
	expect_fail(&W, "build_place", "place never echoed by the server",
				"no place echo");
	assert(W.st.places == 0);
	/* REVIEW B17 M4: not on the first lost press -- each leg retries once, is
	 * then skipped by name, and the THIRD such leg fails the run. */
	assert(mark_count(&W, "build_skipped") == 2u);
	assert(body_has(last_mark(&W, "build_skipped")->body,
					"place not echoed by the server"));

	printf("PASS (b3) build budgets: a held dig the server never echoes FAILS; "
		   "a crosshair never on the cell and water arriving inside the first "
		   "30 held frames are named SKIPS; water after that threshold fails "
		   "again; pickup and place echo each fail with their own diag\n");
}

static void case_inventory_budgets(void) {
	struct cfg c;

	memset(&c, 0, sizeof(c));
	c.inv_never_opens = true;
	run_cfg(&c);
	expect_fail(&W, "inv_open", "inventory never opened", "inv never opens");

	memset(&c, 0, sizeof(c));
	c.inv_focus_stuck = true;
	run_cfg(&c);
	expect_fail(&W, "inv_src", "focus never reached the slot", "focus stuck");

	memset(&c, 0, sizeof(c));
	c.reject_pick = true;
	run_cfg(&c);
	expect_fail(&W, "inv_pick", "server rejected the pick-up click",
				"pick rejected");
	assert(W.st.inv_moves == 0);

	memset(&c, 0, sizeof(c));
	c.reject_put = true;
	run_cfg(&c);
	expect_fail(&W, "inv_put", "server rejected the put-down click",
				"put rejected");
	assert(W.st.inv_moves == 0);

	/* accepted transaction, WRONG resulting slot state: must not be counted */
	memset(&c, 0, sizeof(c));
	c.wrong_slot_after_put = true;
	run_cfg(&c);
	expect_fail(&W, "inv_put", "put-down click never confirmed",
				"wrong slot state");
	assert(W.st.inv_moves == 0);
	assert(!has_mark(&W, "inv_move"));

	/* REL-14b: THE RECONNECT NEEDS A FOLLOWING ACTION. The gen-2 click is
	 * judged by its own Packet106 exactly as gen 1's: rejected, unanswered,
	 * or impossible all fail the run after a clean `verified`. */
	memset(&c, 0, sizeof(c));
	c.post_click = 1;
	run_cfg(&c);
	expect_fail(&W, "inv_pick", "server rejected the pick-up click",
				"post-reconnect pick rejected");
	assert(W.st.gen == 2u && has_mark(&W, "verified"));
	assert(!has_mark(&W, "post_action") && !has_mark(&W, "ok"));

	memset(&c, 0, sizeof(c));
	c.post_click = 2;
	run_cfg(&c);
	expect_fail(&W, "inv_pick", "pick-up click never confirmed",
				"post-reconnect click without a transaction");
	assert(W.st.gen == 2u && !has_mark(&W, "post_action"));

	memset(&c, 0, sizeof(c));
	c.post_fill_inv = true;
	run_cfg(&c);
	expect_fail(&W, "inv_open", "no post-reconnect action",
				"post-reconnect leg cannot move anything");
	assert(W.st.gen == 2u && !has_mark(&W, "inv_skipped"));

	printf("PASS (b4) inventory budgets: open, focus, both rejections and an "
		   "accepted-but-wrong slot state all refuse to count a move\n");
}

static void case_menu_budgets(void) {
	struct cfg c;

	memset(&c, 0, sizeof(c));
	c.pause_never_opens = true;
	run_cfg(&c);
	expect_fail(&W, "pause_open", "pause menu never opened", "pause");

	/* draft 2: the snapshot waits for rest, and a body that never rests is a
	 * named failure rather than a moving snapshot */
	memset(&c, 0, sizeof(c));
	c.never_rest_at_pause = true;
	run_cfg(&c);
	expect_fail(&W, "pause_focus", "body never came to rest before the leave",
				"never rests");
	assert(!has_mark(&W, "snapshot_pre"));

	memset(&c, 0, sizeof(c));
	c.leave_never_ends = true;
	run_cfg(&c);
	expect_fail(&W, "pause_leave", "leave row never ended the session",
				"leave");

	memset(&c, 0, sizeof(c));
	c.title_never = true;
	run_cfg(&c);
	expect_fail(&W, "title", "title never reached after the leave", "title");

	memset(&c, 0, sizeof(c));
	c.rejoin_title_stuck = true;
	run_cfg(&c);
	expect_fail(&W, "rejoin_title", "multiplayer screen never opened",
				"rejoin title");

	memset(&c, 0, sizeof(c));
	c.rejoin_setup_stuck = true;
	run_cfg(&c);
	expect_fail(&W, "rejoin_setup", "join never started", "rejoin setup");

	printf("PASS (b5) menu budgets: pause, the at-rest snapshot gate, leave, "
		   "title and both rejoin screens fail by name\n");
}

static void case_verify_and_caps(void) {
	struct cfg c;

	memset(&c, 0, sizeof(c));
	c.dim_delta = 1;
	run_cfg(&c);
	expect_fail(&W, "verify", "dimension changed across the rejoin", "dim");

	memset(&c, 0, sizeof(c));
	c.post_dx = MPP_POS_TOL_H + 0.5f;
	run_cfg(&c);
	expect_fail(&W, "verify", "position changed across the rejoin", "pos h");

	memset(&c, 0, sizeof(c));
	c.post_dy = MPP_POS_TOL_V + 0.5f;
	run_cfg(&c);
	expect_fail(&W, "verify", "position changed across the rejoin", "pos v");

	/* Draft 2b's reserve leaves a real stack on the wire, so these two need no
	 * fill_mode crutch any more: a FRESH run ends holding the reserve block
	 * and a count-only or damage-only drift is detectable. */
	memset(&c, 0, sizeof(c));
	c.post_count_delta = 1;
	run_cfg(&c);
	expect_fail(&W, "verify", "inventory changed across the rejoin",
				"inv count");

	memset(&c, 0, sizeof(c));
	c.post_damage_delta = 1;
	run_cfg(&c);
	expect_fail(&W, "verify", "inventory changed across the rejoin",
				"inv damage");

	memset(&c, 0, sizeof(c));
	c.ready_delay2_ms = MPP_REJOIN_BUDGET_MS + 10000u;
	run_cfg(&c);
	expect_fail(&W, "verify", "rejoin past its budget", "rejoin budget");
	assert(body_num(last_mark(&W, "verified")->body, "\"rejoin_ms\":")
		   > (long)MPP_REJOIN_BUDGET_MS);
	assert(body_num(last_mark(&W, "login_ok")->body, "\"since_connect_ms\":")
		   < (long)MPP_LOGIN_DEADLINE_MS);

	/* Total cap (1 200 000 ms). Draft 2 calls it an accepted backstop that a
	 * legitimate run does not reach -- a typical run here is ~11 min -- so the
	 * latch is proved with a white-box clock jump inside a WALK leg. */
	memset(&c, 0, sizeof(c));
	c.jump_clock_in_walk = true;
	run_cfg(&c);
	expect_fail(&W, "walk", "total timeout", "total cap");

	printf("PASS (b6) verify + caps: dimension, position (h and v), inventory "
		   "count-only and damage-only, rejoin budget, total cap\n");
}

/* Draft 2 reserves the last queue slot for `fail`, so a stuck queue still
 * reports the reason ON THE WIRE. */
static void case_marker_overflow(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	c.never_pop = true;
	world_init(&W, &c);
	run(&W);
	assert(W.st.phase == MPP_PH_FAILED);
	assert(W.st.markers_dropped > 0);
	assert(strcmp(W.st.fail_diag, "marker queue overflow") == 0);
	assert(W.st.fail_step[0] != '\0');

	/* drain the queue the way the impure half does: peek, send, pop */
	const struct mpp_marker* m;
	char last_key[MPP_MARKER_KEY] = "";
	char fail_body[MPP_MARKER_BODY] = "";
	unsigned drained = 0;
	while((m = mpp_marker_peek(&W.st)) != NULL) {
		snprintf(last_key, sizeof(last_key), "%s", m->key);
		if(strcmp(m->key, "fail") == 0)
			snprintf(fail_body, sizeof(fail_body), "%s", m->body);
		drained++;
		mpp_marker_pop(&W.st);
	}
	/* HEADER DEFECT 7 */
	EXPECT(7, strcmp(last_key, "fail") == 0 && fail_body[0], "marker_overflow",
		   "a stuck marker queue does not deliver `fail` on the wire: the "
		   "last reserved slot did not hold it");
	assert(drained == MPP_MARKER_QUEUE);
	assert(body_has(fail_body, "marker queue overflow"));
	printf("PASS (b7) marker overflow: dropped=%u, drained %u markers, the "
		   "last is fail(%s) on the wire\n",
		   W.st.markers_dropped, drained, W.st.fail_step);
}

/* ---- (c) skip branches that must not fail the run by themselves ---------- */

static void case_skips(void) {
	struct cfg c;

	/* stone: no hand drop at all, so no pickup and (draft 2) no inventory leg
	 * is ever entered -- the run still survives to p2_end */
	memset(&c, 0, sizeof(c));
	c.under_feet = STONE;
	run_cfg(&c);
	expect_fail(&W, "walk", "action floors not met", "stone floor");
	assert(W.st.build_skipped >= 10);
	assert(W.st.digs == 0 && W.st.places == 0 && W.st.inv_moves == 0);
	assert(body_has(last_mark(&W, "build_skipped")->body,
					"block under the feet is not hand-diggable"));
	/* THE SKIP NAMES WHAT IT STOOD ON (3490f74b). The first PC runs reported
	 * ten "not hand-diggable" legs with no way to see WHICH block refused, and
	 * that cost a whole run to diagnose -- so the marker carries the observed
	 * id under the feet and the measured clock. Asserted here because this is
	 * the one case whose floor id is known exactly: STONE is 1. */
	assert(body_has(last_mark(&W, "build_skipped")->body, "\"under\":1"));
	assert(body_has(last_mark(&W, "build_skipped")->body, "\"measured_ms\":"));
	assert(has_mark(&W, "p2_end"));
	assert(mark_index(&W, "fail", 0) > mark_index(&W, "p2_end", 0));

	/* GRAVEL IS IN since 3490f74b: it digs by hand in 0.9 s and drops itself
	 * nine times in ten, so a gravel plain is playable ground, not a refusal.
	 * This case used to pin the OPPOSITE (fail "action floors not met") and was
	 * left RED by that commit, which changed mpp_hand_diggable without touching
	 * this file. It now pins the behaviour that commit set out to buy -- and
	 * that is the whole point of the change, since the scouted fixture spawn
	 * (push-0921/B8) is exactly this kind of open ground. The "non-diggable
	 * floor still fails honestly" half is unchanged and lives in the STONE case
	 * above, so nothing was traded away to make this one pass. */
	memset(&c, 0, sizeof(c));
	c.under_feet = GRAVEL;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "gravel floor");
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.digs >= MPP_FLOOR_DIGS);
	assert(W.st.places >= MPP_FLOOR_PLACES);
	assert(W.st.inv_moves >= MPP_FLOOR_INV_MOVES);
	/* and not ONE leg refused for the reason this block no longer earns */
	for(unsigned k = 0; k < W.nmarks; k++)
		if(strcmp(W.marks[k].key, "build_skipped") == 0)
			assert(!body_has(W.marks[k].body,
							 "block under the feet is not hand-diggable"));

	/* TWO DISTINCT REASONS since c4488f36, with their own budgets. The first
	 * PC runs could not tell them apart -- both printed "never at rest in one
	 * column" -- and that is what cost a run to diagnose. */
	memset(&c, 0, sizeof(c));
	c.never_at_rest = true;
	run_cfg(&c);
	expect_fail(&W, "walk", "action floors not met", "never at rest");
	assert(body_has(last_mark(&W, "build_skipped")->body,
					"never came to rest"));
	assert(W.taps_seen == 0); /* a body that never rests is never nudged */

	memset(&c, 0, sizeof(c));
	c.never_centred = true;
	run_cfg(&c);
	expect_fail(&W, "walk", "action floors not met", "never centred");
	assert(W.st.build_skipped >= 8);
	assert(body_has(last_mark(&W, "build_skipped")->body,
					"never centred in one column"));
	/* It really tried: the taps were issued, they just went nowhere. */
	assert(W.taps_seen > 0 && W.taps_that_moved == 0);

	/* no usable top face: dig, pickup and the inventory leg still count */
	memset(&c, 0, sizeof(c));
	c.no_top_face = true;
	run_cfg(&c);
	expect_fail(&W, "walk", "action floors not met", "no top face");
	assert(W.st.digs >= MPP_FLOOR_DIGS && W.st.picks == W.st.digs);
	assert(W.st.places == 0);
	assert(W.st.inv_moves == MPP_FLOOR_INV_MOVES);
	assert(body_has(last_mark(&W, "build_skipped")->body,
					"no usable top face on any cardinal"));

	/* nothing to move (a full inventory has no destination slot): the leg is
	 * skipped and the build leg GOES ON to aim and place */
	memset(&c, 0, sizeof(c));
	c.fill_mode = 36;
	run_cfg(&c);
	expect_fail(&W, "walk", "action floors not met", "inv leg skipped");
	assert(has_mark(&W, "inv_skipped"));
	assert(body_has(last_mark(&W, "inv_skipped")->body, "nothing to move"));
	int isk = mark_index(&W, "inv_skipped", 0);
	int pl = mark_index(&W, "place", (unsigned)isk);
	assert(pl > isk); /* it did not abandon the cycle */
	assert(W.st.digs >= MPP_FLOOR_DIGS && W.st.places >= MPP_FLOOR_PLACES);
	assert(W.st.inv_moves == 0);

	printf("PASS (c) skips: stone (naming \"under\":1), never-at-rest, "
		   "never-centred, no-top-face and a skipped inventory leg (which still "
		   "aims and places) only fail at p2_end on \"action floors not met\"; "
		   "GRAVEL now completes the run instead (3490f74b)\n");
}

/* ---- (c2) the closed-loop centring of c4488f36 --------------------------- */

static void case_centring(void) {
	struct cfg c;

	/* THE HAPPY PATH ALREADY PROVES IT: since this commit a walk leg leaves
	 * the body wherever it stopped (snap_to_column no longer teleports it to
	 * the middle of a column), so every dig in case_happy_path is a dig the
	 * core nudged its way into. This case pins the mechanism itself. */
	memset(&c, 0, sizeof(c));
	world_init(&W, &c);
	run(&W);
	assert(W.st.phase == MPP_PH_OK);
	assert(W.taps_seen > 0 && W.taps_that_moved > 0);
	assert(W.st.digs >= MPP_FLOOR_DIGS);
	/* Every tap that was issued moved the body: the core never taps while the
	 * facing disagrees with where it wants to go. */
	assert(W.taps_that_moved == W.taps_seen);

	/* FLUSH AGAINST A WALL. The first PC run's body rested at x.7000008 --
	 * one ulp outside the old [0.3, 0.7] window, so it was never "centred" and
	 * never dug. The margin window plus the nudges must recover it. */
	memset(&c, 0, sizeof(c));
	c.land_frac = 0.7000008f;
	world_init(&W, &c);
	/* the landing really is outside the window, or this case proves nothing */
	assert(!(0.7000008f >= MPP_CENTRE_LO && 0.7000008f <= MPP_CENTRE_HI));
	run(&W);
	assert(W.st.phase == MPP_PH_OK);
	assert(W.taps_that_moved > 0);
	assert(W.st.digs >= MPP_FLOOR_DIGS && W.st.places >= MPP_FLOOR_PLACES);

	printf("PASS (c2) centring: the core nudges a body that lands off-centre "
		   "(and one flush against a wall at frac 0.7000008) into the column "
		   "and digs -- %u taps, %u of them moving\n",
		   W.taps_seen, W.taps_that_moved);
}

/* ---- (c3) the facing gate: a wrong yaw mapping never walks the wrong way -- */

static void case_mirrored_facing(void) {
	struct cfg c;

	/* The whole reason struct mpp_obs carries fwd_x / fwd_z. The fake reports
	 * the MIRRORED forward vector -- exactly what a sign error in the impure
	 * half's yaw mapping produces (mp_play_map.h's mpp_map_engine_yaw_deg has
	 * the two conventions) -- and the core must notice and REFUSE, not tap. */
	memset(&c, 0, sizeof(c));
	c.mirror_facing = true;
	world_init(&W, &c);
	run(&W);
	expect_fail(&W, "walk", "action floors not met", "mirrored facing");
	assert(body_has(last_mark(&W, "build_skipped")->body,
					"aim never faced the column centre"));
	/* NOT ONE TAP, so the body cannot have been driven the wrong way by the
	 * centring leg. (It still walks its legs -- that is the walker's heading,
	 * not the aim lock's -- so only the taps are checked here.) */
	assert(W.taps_seen == 0);
	assert(W.st.digs == 0);

	printf("PASS (c3) mirrored facing: the observed fwd vector disagrees with "
		   "the requested yaw, so the core skips \"aim never faced the column "
		   "centre\" after %u ms and never issues a tap\n",
		   MPP_FACE_BUDGET_MS);
}

/* ---- (c4) no build leg in water ------------------------------------------ */

static void case_in_water(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	c.in_water = true;
	run_cfg(&c);
	expect_fail(&W, "walk", "action floors not met", "in water");
	assert(body_has(last_mark(&W, "build_skipped")->body, "in water"));
	assert(W.st.digs == 0 && W.st.places == 0);
	assert(W.taps_seen == 0); /* not even a nudge while swimming */
	assert(has_mark(&W, "p2_end")); /* still an honest p2_end, then the floor */
	printf("PASS (c4) in water: every build leg is skipped \"in water\", with "
		   "no dig, no place and no tap; the run still reaches p2_end\n");
}

/* ---- (c4b) the place leg of 8bf54845 (push-0921/B12) --------------------- */

/* THE LANDING WAIT. The place geometry only exists once the body is standing
 * IN the cell it dug: the neighbour's ground is then at the feet level, which
 * is what o->ty == mpp_floor(o->y) asks for. Run pc_mp_play_20260921_161051
 * burned the whole aim budget in 4 of 13 cycles sweeping while the body was
 * still dropping, and lost the places floor 6-8 by it. */
static void case_landing_wait(void) {
	struct cfg c;

	/* The body takes 30 frames (~0.5 s) to fall. Nothing may be attempted in
	 * the meantime -- not one USE -- and the run must still complete. */
	memset(&c, 0, sizeof(c));
	c.drop_delay_frames = 30;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "landing wait");
	assert(W.st.phase == MPP_PH_OK);
	assert(W.use_before_landed == 0);
	assert(W.st.places >= MPP_FLOOR_PLACES);
	assert(mark_count(&W, "build_skipped") == 0u);

	/* A body that NEVER lands (a slope, a ledge) is terrain, not a defect: the
	 * leg is skipped by name after the drop budget plus the sweep budget, and
	 * the run goes on to fail honestly on the floors at p2_end. */
	memset(&c, 0, sizeof(c));
	c.never_lands = true;
	run_cfg(&c);
	expect_fail(&W, "walk", "action floors not met", "never lands");
	assert(body_has(last_mark(&W, "build_skipped")->body,
					"body never landed in the dug cell"));
	assert(W.use_before_landed == 0);
	assert(W.st.places == 0);
	assert(has_mark(&W, "p2_end"));

	printf("PASS (c4b) landing wait: a falling body is waited for with no USE "
		   "attempted, and one that never arrives is a named skip\n");
}

/* PLANT CLEARING. On a grass plain tall grass covers a large share of the
 * cells, and it stands exactly where the block is to go. Breaking it is what a
 * player does; the drop (seeds) is not placeable and changes nothing. */
static void case_plant_clearing(void) {
	struct cfg c;

	/* Every cardinal carries tall grass. The core must break one and then
	 * place, cycle after cycle, and the run completes with its floors. */
	memset(&c, 0, sizeof(c));
	c.plant_dirs = 0xFu;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "plants everywhere");
	assert(W.st.phase == MPP_PH_OK);
	assert(mark_count(&W, "plant_cleared") >= 1u);
	assert(W.st.places >= MPP_FLOOR_PLACES);
	/* The clear is reported with the cell it happened in, so a reader can tell
	 * it from the dig; and it is NOT counted as a place. */
	{
		const struct rec* m = last_mark(&W, "plant_cleared");
		assert(m && body_has(m->body, "\"n\":") && body_has(m->body, "\"x\":")
			   && body_has(m->body, "\"y\":") && body_has(m->body, "\"z\":"));
		assert(!body_has(m->body, "\"measured_ms\":"));
	}

	/* The OTHER half: ATTACK is held only while the crosshair is on the plant.
	 * With snow (78) -- also in the plant set -- on one cardinal only, the
	 * three bare cardinals must place with no clear at all. */
	memset(&c, 0, sizeof(c));
	c.plant_dirs = 0x1u;
	c.plant_id = 78;
	run_cfg(&c);
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.places >= MPP_FLOOR_PLACES);

	/* A plant the server NEVER echoes away: that cardinal is given up after
	 * MPP_CLEAR_BUDGET_MS and the sweep moves on to the next one, so a single
	 * stuck cell cannot cost the leg. With all four stuck the leg is skipped by
	 * name -- and NOT as "no usable top face", which would blame the terrain
	 * for a clear that never completed. */
	memset(&c, 0, sizeof(c));
	c.plant_dirs = 0x1u;
	c.plant_never_clears = true;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "one stuck plant");
	assert(W.st.phase == MPP_PH_OK); /* three good cardinals are enough */
	assert(W.st.places >= MPP_FLOOR_PLACES);
	assert(mark_count(&W, "plant_cleared") == 0u);

	printf("PASS (c4c) plants: the block standing on the target is broken "
		   "first and reported, a bare cardinal needs no clear, and one that "
		   "never clears costs its cardinal and not the leg\n");
}

/* THE CATCH-UP PLACE. The reserve rule keeps places exactly one behind digs;
 * further behind means an earlier leg was skipped and the blocks are sitting in
 * the inventory. One cycle may place twice to catch up -- at most once, or a
 * run could bank skipped legs and spend them all at the end. */
static void case_catch_up_place(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	/* Cycles 0..2 dig but cannot place (cycle 0 is the reserve anyway), so by
	 * cycle 3 places are two behind digs -- the state the catch-up exists for.
	 * NOT skip_n_builds: that lands the body on stone and skips the DIG too,
	 * which keeps places exactly one behind and never reaches this path. */
	c.no_top_face_cycles = 3;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "catch up");
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.build_skipped >= 2);
	assert(W.st.places >= MPP_FLOOR_PLACES);
	/* EXACTLY ONE reserve marker in the whole run. The reserve is a property of
	 * the run's first cycle, not of every cycle that happens to place twice --
	 * a second one would mean the catch-up path re-entered the reserve branch
	 * and quietly kept another block back. */
	assert(mark_count(&W, "reserve_kept") == 1u);
	/* No cycle placed more than MPP_PLACES_PER_CYCLE. Counted from the MARKER
	 * STREAM rather than the core's own places_this_cycle, so the test is not
	 * checking the implementation against itself. The `place` marker carries no
	 * cycle number, but a cycle is bounded by its dig: every place between one
	 * dig and the next belongs to that dig's cycle. A run that banked skipped
	 * legs and spent them all in one cycle shows up here as a burst. */
	{
		unsigned in_cycle = 0, worst = 0, bursts = 0;
		for(unsigned k = 0; k < W.nmarks; k++) {
			if(strcmp(W.marks[k].key, "dig") == 0) {
				if(in_cycle > worst)
					worst = in_cycle;
				in_cycle = 0;
			} else if(strcmp(W.marks[k].key, "place") == 0) {
				in_cycle++;
				if(in_cycle == MPP_PLACES_PER_CYCLE)
					bursts++;
			}
		}
		if(in_cycle > worst)
			worst = in_cycle;
		assert(worst <= MPP_PLACES_PER_CYCLE);
		/* and the catch-up really did happen -- otherwise this case would pass
		 * on a run that simply never caught up, proving nothing. */
		assert(bursts >= 1u);
	}
	printf("PASS (c4d) catch-up: two skipped legs are caught up at most one "
		   "place per cycle, with exactly one reserve_kept in the run\n");
}

/* A MISSED PICKUP. On a REMOTE server the drop is the server's physics (it hops
 * up and sideways per tick), so an own-column dig makes a pickup LIKELY, not
 * certain: run pc_mp_play_20260921_162144 lost 1 of 4 with the block provably
 * broken under the feet and the body standing in the hole. One miss is
 * narrated and the cycle goes on; losing MOST of the drops is a collect-path
 * defect and fails by name. */
static void case_pickup_missed(void) {
	struct cfg c;

	memset(&c, 0, sizeof(c));
	c.no_pickup = true;
	run_cfg(&c);
	/* EVERY pickup missed -- far past "most" -- so the run fails by name
	 * rather than on the floors, and it says so before p2_end. */
	expect_fail(&W, "build_pickup", "most dug items never reached the inventory",
				"every pickup missed");
	assert(mark_count(&W, "pickup_missed") >= 3u);
	assert(W.st.picks == 0);
	{
		const struct rec* m = last_mark(&W, "pickup_missed");
		assert(m && body_has(m->body, "\"n\":")
			   && body_has(m->body, "\"measured_ms\":"));
	}
	printf("PASS (c4e) pickup_missed: a lost drop is narrated with its cell, "
		   "and a run that loses most of them fails by name at %u misses\n",
		   mark_count(&W, "pickup_missed"));
}

/* ---- (c5) a leg that goes nowhere turns the heading ---------------------- */

static void case_heading_turn(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	c.blocked_leg = true;
	run_cfg(&c);
	/* A blocked walker cannot meet the displacement the contract wants, and
	 * that is the collector's job to judge -- what the CORE owes is the turn,
	 * every leg, by 90 degrees, reported. */
	assert(mark_count(&W, "heading_turned") >= 2u);
	{
		int i0 = mark_index(&W, "heading_turned", 0);
		int i1 = mark_index(&W, "heading_turned", (unsigned)i0 + 1u);
		long h0 = body_num(W.marks[i0].body, "\"heading_deg\":");
		long h1 = body_num(W.marks[i1].body, "\"heading_deg\":");
		assert(h0 >= 0 && h0 < 360 && h1 >= 0 && h1 < 360);
		assert(((h0 + 90) % 360) == h1);
	}
	/* THE FIRST HEADING IS THE SCOUTED ONE: north (180 = -Z). The fixture world
	 * is fixed (seed 9999, scouted spawn -64 / -896): 120 blocks SOUTH of it a
	 * cliff drops into pockets the walker can enter (two-block drops) and never
	 * leave (one-block climbs) -- PC runs 182523 and 185536 both died there --
	 * while run 184003 walked north for its whole ten minutes. */
	memset(&c, 0, sizeof(c));
	world_init(&W, &c);
	run_to_phase(&W, MPP_PH_WALK);
	step(&W);
	assert(W.out.walk);
	if(W.out.walk_heading_deg != 180)
		printf("  !! first heading %d, want 180\n", W.out.walk_heading_deg);
	assert(W.out.walk_heading_deg == 180);

	/* And an UNBLOCKED run never turns: the heading is only a fallback. */
	memset(&c, 0, sizeof(c));
	run_cfg(&c);
	assert(mark_count(&W, "heading_turned") == 0u);

	printf("PASS (c5) heading: a walk leg that moved less than %.0f blocks "
		   "turns the next leg 90 degrees and says so; a leg that made "
		   "progress never does\n",
		   (double)MPP_LEG_MIN_PROGRESS);
}

/* ---- (c6) the pit ---------------------------------------------------------
 * PC run pc_mp_play_20260921_171336: the walker could not leave the hole the
 * script had just dug, the no-progress leg turned the heading -- and the core
 * went on to BUILD anyway, in the same column. Five digs straight down, then
 * stone, then "action floors not met" with nothing in the tape saying why. */

static void case_pit(void) {
	struct cfg c;

	/* (i) trapped for good: not one more dig in that column, and the run ends
	 * BY NAME as soon as all four headings have been tried, not 600 s later. */
	memset(&c, 0, sizeof(c));
	c.pit_legs = UINT32_MAX;
	run_cfg(&c);
	if(W.same_column_digs != 0u || W.st.phase != MPP_PH_FAILED)
		dump(&W, "pit for good");
	assert(W.same_column_digs == 0u);
	assert(W.st.digs == 1u);
	expect_fail(&W, "walk", "walker made no progress on four headings in a row",
				"pit for good");
	assert(mark_count(&W, "heading_turned") == 3u);
	{
		const struct rec* d = last_mark(&W, "dig");
		const struct rec* f = last_mark(&W, "fail");
		assert(d && f);
		assert(f->at_ms - d->at_ms < 4u * MPP_WALK_LEG_MS + 30000u);
	}

	/* (ii) two stuck legs, then the walker gets out: no dig while stuck, the
	 * run carries on and completes, and the stuck count starts again. */
	memset(&c, 0, sizeof(c));
	c.pit_legs = 2;
	run_cfg(&c);
	if(W.same_column_digs != 0u || W.st.phase != MPP_PH_OK)
		dump(&W, "pit, two legs");
	assert(W.same_column_digs == 0u);
	assert(W.st.phase == MPP_PH_OK);
	assert(mark_count(&W, "heading_turned") == 2u);
	{
		int t0 = mark_index(&W, "heading_turned", 0);
		int t1 = mark_index(&W, "heading_turned", (unsigned)t0 + 1u);
		int d1 = mark_index(&W, "dig", 0);
		int d2 = mark_index(&W, "dig", (unsigned)d1 + 1u);
		assert(d1 >= 0 && d1 < t0); /* stuck AFTER the first dig */
		assert(d2 > t1);			/* and no dig until the walker is out */
		for(int k = t0; k <= t1; k++)
			assert(strcmp(W.marks[k].key, "build_skipped") != 0);
	}

	/* (iii) a leg that made progress and STILL ended in the column of the last
	 * dig: the build is skipped by name, never dug. */
	memset(&c, 0, sizeof(c));
	c.lands_in_last_dig_column = true;
	run_cfg(&c);
	assert(W.same_column_digs == 0u);
	assert(W.st.digs == 1u);
	assert(body_has(last_mark(&W, "build_skipped")->body,
					"still in the column of the last dig"));
	expect_fail(&W, "walk", "action floors not met", "lands in the last column");

	printf("PASS (c6) the pit: a leg that went nowhere never builds (0 digs in "
		   "the column of the last dig), four such legs in a row fail BY NAME, "
		   "two do not stop the run, and a leg that comes back to the dug "
		   "column skips by name\n");
}

/* ---- (c6b) the shaft -------------------------------------------------------
 * REL-14b-walker-escape. The hw1020b walker fell into a 2x3 shaft no heading
 * could leave and died on "four headings in a row". It now pillars up first. */

static void case_pit_escape(void) {
	struct cfg c;

	/* (i) a 3-deep shaft: the walker stacks three blocks and leaves; the run
	 * completes, no extra heading turns past the third, no failure. */
	memset(&c, 0, sizeof(c));
	c.pit_legs = UINT32_MAX;
	c.pit_depth = 3;
	c.fill_mode = 35; /* blocks in hand: the pillar spends them */
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK || W.pit_rise != 3u)
		dump(&W, "shaft escape");
	assert(W.st.phase == MPP_PH_OK);
	assert(W.pit_rise == 3u);
	assert(mark_count(&W, "escape_step") == 3u);
	assert(mark_count(&W, "escape_failed") == 0u);
	assert(W.same_column_digs == 0u);
	assert(W.multi_action_frames == 0);
	assert(W.edge_without_release == 0);

	/* (ii) a sealed box: jumping gains no height. The run still ends BY NAME
	 * with the stuck reason, and says why the escape gave up. */
	memset(&c, 0, sizeof(c));
	c.pit_legs = UINT32_MAX;
	c.fill_mode = 36;
	run_cfg(&c);
	expect_fail(&W, "walk", "walker made no progress on four headings in a row",
				"sealed box");
	assert(mark_count(&W, "escape_step") == 0u);
	assert(mark_count(&W, "escape_failed") == 1u);
	assert(body_has(last_mark(&W, "escape_failed")->body,
					"no height gained"));
	assert(W.pit_rise == 0u);

	/* (iii) THE hw1021e POCKET: the server refuses the first two pillar
	 * places (mc173 "can_place refuse") and the walker carries ONE block for a
	 * 3-deep climb. The old step pressed USE once and gave up: "no height
	 * gained, blocks=0". Now it jumps again, then digs wall cells for the
	 * blocks it lacks, and leaves; none of that counts toward the floors. */
	memset(&c, 0, sizeof(c)); /* fresh world: the first dig's block only */
	c.pit_legs = UINT32_MAX;
	c.pit_depth = 3;
	c.pit_refuse_places = 2;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK || W.pit_rise != 3u)
		dump(&W, "refused pillar + wall digs");
	assert(W.st.phase == MPP_PH_OK);
	assert(W.pit_rise == 3u);
	assert(W.pillar_refused == 2u);
	assert(mark_count(&W, "escape_step") == 3u);
	assert(mark_count(&W, "escape_failed") == 0u);
	assert(mark_count(&W, "escape_mine") >= 2u);
	assert(W.esc_mines_done == mark_count(&W, "escape_mine"));
	{
		const struct rec* first = NULL;
		for(unsigned k = 0; k < W.nmarks; k++)
			if(!strcmp(W.marks[k].key, "escape_step")) {
				first = &W.marks[k];
				break;
			}
		assert(first && body_has(first->body, "\"tries\":3"));
	}
	/* the escape's digs and places are not the build leg's */
	assert(W.st.digs == mark_count(&W, "dig"));
	assert(W.st.places == mark_count(&W, "place"));
	assert(W.multi_action_frames == 0);
	assert(W.edge_without_release == 0);

	/* (iv) the one block is spent, and every wall is stone: the run fails
	 * BY NAME with the stuck reason, and the marker says why. */
	memset(&c, 0, sizeof(c));
	c.pit_legs = UINT32_MAX;
	c.pit_depth = 3;
	c.pit_refuse_places = 1;
	c.pit_wall_id = STONE;
	run_cfg(&c);
	expect_fail(&W, "walk", "walker made no progress on four headings in a row",
				"stone pocket");
	assert(W.pit_rise == 1u);
	assert(mark_count(&W, "escape_step") == 1u);
	assert(mark_count(&W, "escape_mine") == 0u);
	assert(mark_count(&W, "escape_failed") == 1u);
	assert(body_has(last_mark(&W, "escape_failed")->body,
					"no placeable block and no hand-diggable wall"));

	/* (v) THE hw1023c LEDGE: the body rests straddling two columns, and
	 * straight down from its centre is the lower neighbour column, out of
	 * reach. The old step jumped there nine times and placed nothing. Now it
	 * first taps into one column (BUILD_REST's centring), and only then
	 * jumps; USE is pressed only with the cell under the feet in the
	 * crosshair, and every attempt names its target. */
	memset(&c, 0, sizeof(c));
	c.pit_legs = UINT32_MAX;
	c.pit_depth = 3;
	c.pit_straddle_frac = 0.84f;
	c.fill_mode = 35;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK || W.pit_rise != 3u)
		dump(&W, "straddled ledge");
	assert(W.straddled);
	assert(W.st.phase == MPP_PH_OK);
	assert(W.pit_rise == 3u);
	assert(mark_count(&W, "escape_centred") == 1u);
	assert(mark_count(&W, "escape_step") == 3u);
	assert(mark_count(&W, "escape_failed") == 0u);
	assert(W.taps_that_moved >= 1u);
	assert(W.pillar_use_no_target == 0u);
	{
		/* centred BEFORE the first pillar block */
		unsigned centred_at = 0, step_at = 0;
		for(unsigned k = 0; k < W.nmarks; k++) {
			if(!centred_at && !strcmp(W.marks[k].key, "escape_centred"))
				centred_at = k + 1u;
			if(!step_at && !strcmp(W.marks[k].key, "escape_step"))
				step_at = k + 1u;
		}
		assert(centred_at && step_at && centred_at < step_at);
	}
	assert(W.multi_action_frames == 0);
	assert(W.edge_without_release == 0);

	/* (vi) the same ledge, but the taps move nothing: the escape never jumps
	 * blind -- no USE without the feet target -- and the run fails BY NAME
	 * with the stuck reason; the marker says the body never centred. */
	memset(&c, 0, sizeof(c));
	c.pit_legs = UINT32_MAX;
	c.pit_depth = 3;
	c.pit_straddle_frac = 0.84f;
	c.pit_pinned = true;
	c.fill_mode = 35;
	run_cfg(&c);
	expect_fail(&W, "walk", "walker made no progress on four headings in a row",
				"straddled, pinned");
	assert(W.pit_rise == 0u);
	assert(W.pillar_use_no_target == 0u);
	assert(mark_count(&W, "escape_failed") == 1u);
	assert(body_has(last_mark(&W, "escape_failed")->body,
					"never centred in one column"));

	printf("PASS (c6b) the shaft: a 3-deep pit is pillared out of and the run "
		   "completes; a sealed box still fails by name with the stuck reason; "
		   "a refused pillar place is retried and a walker without blocks digs "
		   "the wall for them (hw1021e); a body straddling two columns centres "
		   "before it jumps and places only on the cell under its feet "
		   "(hw1023c)\n");
}

/* ---- (c6c) floors met, then stuck -----------------------------------------
 * Real Wii hw1021f: "walker made no progress on four headings in a row" with
 * the displacement floor long met. A walker stuck AFTER every walk-phase floor
 * is met ends the walk as success; one stuck before that fails as before. */

static void case_floors_met_stuck(void) {
	struct cfg c;
	struct mpp_state f;

	/* (i) stuck for good in the last third, every floor already met: the
	 * walk ends as SUCCESS by name at the stuck position, the body stands
	 * (no walk, no dig, no place, no escape) until measured play reaches its
	 * floor, and the rest of the run completes. */
	memset(&c, 0, sizeof(c));
	c.blocked_from_ms = 560000u;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "floors met, then stuck");
	assert(W.st.phase == MPP_PH_OK);
	assert(mark_count(&W, "walk_done_stuck") == 1u);
	assert(mark_count(&W, "fail") == 0u);
	assert(mark_count(&W, "escape_failed") == 0u);
	assert(mark_count(&W, "escape_step") == 0u);
	{
		int ws = mark_index(&W, "walk_done_stuck", 0);
		int pe = mark_index(&W, "p2_end", 0);
		const struct rec* m = &W.marks[ws];
		assert(ws >= 0 && pe > ws);
		for(int k = ws + 1; k < pe; k++) {
			assert(strcmp(W.marks[k].key, "dig") != 0);
			assert(strcmp(W.marks[k].key, "place") != 0);
			assert(strcmp(W.marks[k].key, "inv_move") != 0);
			assert(strcmp(W.marks[k].key, "heading_turned") != 0);
		}
		assert(body_has(m->body, "\"x\":") && body_has(m->body, "\"y\":")
			   && body_has(m->body, "\"z\":"));
		assert(body_f(m->body, "\"digs\":") >= MPP_FLOOR_DIGS);
		assert(body_f(m->body, "\"places\":") >= MPP_FLOOR_PLACES);
		assert(body_f(m->body, "\"inv_moves\":") >= MPP_FLOOR_INV_MOVES);
		assert(body_f(m->body, "\"disp\":") >= MPP_FLOOR_DISPLACEMENT);
		assert(body_f(m->body, "\"measured_ms\":") >= 560000.0);
		assert(body_f(W.marks[pe].body, "\"measured_ms\":")
			   >= MPP_MEASURED_PLAY_MS);
	}

	/* (ii) the same walker stuck for good in the middle third: the spread
	 * rule (a dig and a place in EACH third) is not met yet, so it fails
	 * exactly as before, by name, and never claims walk_done_stuck. */
	memset(&c, 0, sizeof(c));
	c.blocked_from_ms = 300000u;
	run_cfg(&c);
	expect_fail(&W, "walk", "walker made no progress on four headings in a row",
				"stuck before the floors");
	assert(mark_count(&W, "walk_done_stuck") == 0u);

	/* (iii) the pit for good after the first dig (case_pit (i)): far below
	 * every floor -- unchanged. */
	memset(&c, 0, sizeof(c));
	c.pit_legs = UINT32_MAX;
	run_cfg(&c);
	expect_fail(&W, "walk", "walker made no progress on four headings in a row",
				"pit, floors not met");
	assert(mark_count(&W, "walk_done_stuck") == 0u);

	/* (iv) the gate itself: every floor, one at a time, short by one. */
	memset(&f, 0, sizeof(f));
	f.digs = MPP_FLOOR_DIGS;
	f.places = MPP_FLOOR_PLACES;
	f.inv_moves = MPP_FLOOR_INV_MOVES;
	f.dig_thirds = f.place_thirds = 7u;
	f.inv_thirds = 5u;
	f.disp_origin = true;
	f.disp_max2 = MPP_FLOOR_DISPLACEMENT * MPP_FLOOR_DISPLACEMENT;
	assert(mpp_walk_floors_met(&f));
	{
		struct mpp_state g;
		g = f; g.digs--; assert(!mpp_walk_floors_met(&g));
		g = f; g.places--; assert(!mpp_walk_floors_met(&g));
		g = f; g.inv_moves--; assert(!mpp_walk_floors_met(&g));
		g = f; g.dig_thirds = 3u; assert(!mpp_walk_floors_met(&g));
		g = f; g.place_thirds = 6u; assert(!mpp_walk_floors_met(&g));
		g = f; g.inv_thirds = 4u; assert(!mpp_walk_floors_met(&g));
		g = f; g.disp_origin = false; assert(!mpp_walk_floors_met(&g));
		g = f; g.disp_max2 = 63.9f * 63.9f; assert(!mpp_walk_floors_met(&g));
	}

	printf("PASS (c6c) floors met, then stuck: the walk ends as success by "
		   "name (walk_done_stuck at the stuck position) and the run completes; "
		   "stuck before any floor is met still fails exactly as before "
		   "(hw1021f)\n");
}

/* ---- (c7) keep a way out ---------------------------------------------------
 * The cardinals the place leg can use are the body's ONLY exits from the hole
 * it stands in. The core has to look at all four before it places on any, and
 * leave one alone. */

static void case_keep_an_exit(void) {
	struct cfg c;

	/* (i) run 172431's hole: two exits (W and N), and the script is BEHIND on
	 * places, so the catch-up wants two blocks down in one cycle. It may have
	 * one. */
	memset(&c, 0, sizeof(c));
	c.exit_dirs = 0x6u;
	c.no_top_face_cycles = 2;
	run_cfg(&c);
	if(W.traps != 0u || W.st.phase != MPP_PH_OK)
		dump(&W, "two exits");
	assert(W.traps == 0u);
	assert(W.same_column_digs == 0u);
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.places >= MPP_FLOOR_PLACES);

	/* (ii) ONE usable exit AFTER the dig (two level neighbours, but the plant
	 * on one of them never clears): it is never built on; the leg is skipped
	 * by name. A column with one level neighbour is refused before the dig --
	 * that is case_survey's. */
	memset(&c, 0, sizeof(c));
	c.exit_dirs = 0x3u;
	c.plant_dirs = 0x2u;
	c.plant_never_clears = true;
	run_cfg(&c);
	if(W.traps != 0u)
		dump(&W, "one exit");
	assert(W.traps == 0u);
	assert(W.st.places == 0u);
	assert(body_has(last_mark(&W, "build_skipped")->body,
					"placing would wall the only way out"));
	expect_fail(&W, "walk", "action floors not met", "one exit");

	/* (iii) flat ground, behind on places: the double place still happens
	 * (two of four exits), and nobody is walled in. */
	memset(&c, 0, sizeof(c));
	c.no_top_face_cycles = 2;
	run_cfg(&c);
	assert(W.traps == 0u);
	assert(W.st.phase == MPP_PH_OK);
	{
		unsigned doubles = 0;
		for(unsigned k = 0; k + 1u < W.nmarks; k++)
			if(!strcmp(W.marks[k].key, "place")
			   && !strcmp(W.marks[k + 1u].key, "place"))
				doubles++;
		assert(doubles >= 1u);
	}

	printf("PASS (c7) keep a way out: all four cardinals are read before any "
		   "place, one usable cardinal is always left alone (0 traps with two "
		   "exits and a catch-up due), a single exit is never built on, and "
		   "flat ground still gets its double place\n");
}

/* ---- (c9) land first, then open the menu ---------------------------------- */

static void case_rest_before_pause(void) {
	struct cfg c;
	/* the walker lets go mid-jump and the pause menu freezes the body: the core
	 * has to wait for the landing BEFORE it opens the menu */
	memset(&c, 0, sizeof(c));
	c.pause_freezes_body = true;
	c.airborne_after_walk_ms = 600;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "rest before pause");
	assert(W.st.phase == MPP_PH_OK);
	assert(has_mark(&W, "snapshot_pre") && has_mark(&W, "verified"));

	printf("PASS (c9) land first: with a pause menu that freezes the body and a "
		   "walker that lets go mid-jump, the core waits for the landing before "
		   "it opens the menu and the rejoin half completes\n");
}

/* ---- (c8) the PC-only short play -------------------------------------------
 * Every full run costs 21 minutes and, by 2026-09-21 17:50, not one had reached
 * the rejoin half: pause, leave, title, rejoin, login, verify had never met the
 * real app. short_play_ms cuts measured play and waives the action floors so
 * that half can be exercised in three minutes. It can NEVER pass a gate: the
 * session marker states the measured floor the device really used, and the
 * collector refuses any value but the contract's (tests/test_mp_play_evidence.py
 * "built against a different contract"). */

static void case_short_play(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	world_init(&W, &c);
	W.st.short_play_ms = 90000u;
	run(&W);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "short play");
	assert(W.st.phase == MPP_PH_OK);
	/* the tape SAYS so, in the field the collector already refuses on */
	assert(body_num(W.marks[mark_index(&W, "session", 0)].body,
					"\"measured_floor_ms\":") == 90000);
	/* far below the floors, and still through the whole rejoin half */
	assert(W.st.digs < MPP_FLOOR_DIGS);
	assert(has_mark(&W, "p2_end") && has_mark(&W, "snapshot_pre")
		   && has_mark(&W, "snapshot_post") && has_mark(&W, "verified"));
	assert(W.st.now_ms < 200000u);

	/* and OFF means the contract, to the millisecond */
	memset(&c, 0, sizeof(c));
	run_cfg(&c);
	assert(W.st.phase == MPP_PH_OK);
	assert(body_num(W.marks[mark_index(&W, "session", 0)].body,
					"\"measured_floor_ms\":") == (long)MPP_MEASURED_PLAY_MS);
	assert(W.st.now_ms == g_happy_total_ms);

	printf("PASS (c8) short play: 90 s of measured play reaches verify with the "
		   "floors waived, the session marker states 90000 (a value the "
		   "collector refuses), and with the lever off the run is the happy "
		   "path to the millisecond\n");
}

/* ---- (c10) look before you dig --------------------------------------------- */

static void case_survey(void) {
	struct cfg c;

	/* (i) one cliff column among ordinary ones: it is surveyed, refused by
	 * name, NOT dug, and the run goes on to pass */
	memset(&c, 0, sizeof(c));
	c.cliff_cycles = 1u << 2;
	run_cfg(&c);
	if(W.traps != 0u || W.st.phase != MPP_PH_OK)
		dump(&W, "one cliff column");
	assert(W.traps == 0u);
	assert(W.st.phase == MPP_PH_OK);
	{
		int k = mark_index(&W, "build_skipped", 0);
		assert(k >= 0);
		assert(body_has(W.marks[k].body,
						"ground not level enough to dig safely"));
		assert(body_num(W.marks[k].body, "\"cycle\":") == 2);
	}

	/* (ii) ONE level neighbour is not enough either: a dig would leave a single
	 * exit and nothing to place on */
	memset(&c, 0, sizeof(c));
	c.exit_dirs = 0x1u;
	run_cfg(&c);
	assert(W.traps == 0u);
	assert(W.st.digs == 0u && W.st.places == 0u);
	assert(body_has(last_mark(&W, "build_skipped")->body,
					"ground not level enough to dig safely"));
	expect_fail(&W, "walk", "action floors not met", "one level neighbour");

	printf("PASS (c10) look before you dig: a column with fewer than two level "
		   "neighbours is refused by name before the dig (0 traps), and the "
		   "run carries on\n");
}

/* A level ring cannot make a hollow floor beneath the dig safe. The support
 * observation is bound to the exact intended cell and is rechecked on the
 * first DIG frame before any ATTACK leaves the driver. */
static void case_landing_support(void) {
	struct cfg c = {0};
	c.exit_dirs = 0x3u; /* exactly two surveyed level neighbours */
	c.hollow_support_cycles = 1u << 2;
	run_cfg(&c);
	assert(W.st.phase == MPP_PH_OK);
	int skip = mark_index(&W, "build_skipped", 0);
	assert(skip >= 0);
	assert(body_num(W.marks[skip].body, "\"cycle\":") == 2);
	assert(body_has(W.marks[skip].body, "unsafe one-block landing"));
	assert(W.unsafe_support_attack_frames == 0);
	assert(W.attack_after_support_loss == 0);

	memset(&c, 0, sizeof(c));
	c.unknown_support_cycles = 1u << 2;
	run_cfg(&c);
	assert(W.st.phase == MPP_PH_OK);
	assert(body_has(last_mark(&W, "build_skipped")->body,
				"unsafe one-block landing"));
	assert(W.unsafe_support_attack_frames == 0);
	assert(W.attack_after_support_loss == 0);

	memset(&c, 0, sizeof(c));
	c.stale_support_dig_cycles = 1u << 2;
	run_cfg(&c);
	assert(W.st.phase == MPP_PH_OK);
	assert(body_has(last_mark(&W, "build_skipped")->body,
				"unsafe one-block landing"));
	assert(W.unsafe_support_attack_frames == 0);
	assert(W.attack_after_support_loss == 0);

	memset(&c, 0, sizeof(c));
	c.support_lost_dig_cycles = 1u << 2;
	run_cfg(&c);
	assert(W.st.phase == MPP_PH_OK);
	assert(body_has(last_mark(&W, "build_skipped")->body,
				"unsafe one-block landing"));
	assert(W.attack_after_support_loss == 0);

	memset(&c, 0, sizeof(c));
	c.exit_dirs = 0x3u;
	run_cfg(&c);
	assert(W.st.phase == MPP_PH_OK && W.st.digs >= MPP_FLOOR_DIGS);
	assert(!has_mark(&W, "build_skipped"));
	puts("PASS landing support: hollow, unknown, stale and lost support stop ATTACK; known support permits the one-block drop");
}

/* ---- (c11) review B17 ------------------------------------------------------ */

static void case_review_b17(void) {
	struct cfg c;

	/* H3 rides on EVERY case from here on: the fake now reports an absent body
	 * the way the real glue does (not ticking, health 0) between the leave and
	 * the rejoin, and the happy path above must not call that a death. */

	/* M1: the dig cell emptied by something else is not a dig */
	memset(&c, 0, sizeof(c));
	c.foreign_dig_legs = 1;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "foreign dig");
	assert(W.st.phase == MPP_PH_OK);
	{
		int k = mark_index(&W, "build_skipped", 0);
		assert(k >= 0 && k < mark_index(&W, "dig", 0));
		assert(body_has(W.marks[k].body, "dig cell changed without a dig"));
		/* hw1030r: the skip carries the hold's shape */
		assert(body_has(W.marks[k].body, "\"dig_on_t0\":"));
		assert(body_has(W.marks[k].body, "\"dig_last_on\":"));
		assert(body_has(W.marks[k].body, "\"dig_on_frames\":"));
		assert(body_has(W.marks[k].body, "\"on_gap_max_ms\":"));
		assert(body_has(W.marks[k].body, "\"frame_gap_max_ms\":"));
	}

	/* M2: a foreign echo on the place cell is not a place; ours still counts,
	 * with the id of the block that was really in the hand */
	memset(&c, 0, sizeof(c));
	c.foreign_place_legs = 3;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "foreign place");
	assert(W.st.phase == MPP_PH_OK);
	for(unsigned k = 0; k < W.nmarks; k++)
		if(!strcmp(W.marks[k].key, "place"))
			assert(body_num(W.marks[k].body, "\"id\":") == DIRT);

	/* M4: ONE lost USE press is retried and the run never notices... */
	memset(&c, 0, sizeof(c));
	c.unechoed_use_presses = 1;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "one lost press");
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.build_skipped == 0u);
	/* ...TWO in a row cost that leg, by name, and nothing else */
	memset(&c, 0, sizeof(c));
	c.unechoed_use_presses = 2;
	run_cfg(&c);
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.build_skipped == 1u);
	assert(body_has(last_mark(&W, "build_skipped")->body,
					"place not echoed by the server"));

	/* M6: a burst that evicts the dig's echo from the ring is NOT a client /
	 * server disagreement */
	memset(&c, 0, sizeof(c));
	c.echo_burst_legs = 1;
	c.no_watch = true; /* the fallback: a glue without the watched-cell seam */
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "echo burst");
	assert(W.st.phase == MPP_PH_OK);
	assert(body_has(W.marks[mark_index(&W, "build_skipped", 0)].body,
					"server echoes overflowed the ring"));

	/* the review's surviving mutation: an echo that says the cell is STONE is
	 * not a dig either -- held long enough, that IS a disagreement */
	memset(&c, 0, sizeof(c));
	c.dig_echo_nonair = true;
	run_cfg(&c);
	expect_fail(&W, "build_dig", "dig never echoed by the server",
				"non-air dig echo");
	assert(W.st.digs == 0u);

	printf("PASS (c11) review B17: an absent body is not a dead one; a cell "
		   "emptied or filled by something else earns nothing; one lost USE "
		   "press is retried, two cost the leg, three legs fail the run; an "
		   "evicted echo is a named skip; a non-air dig echo is no dig\n");
}

/* ---- (c12) review B17 L1: the pure helper takes ANY heading ---------------- */

static void case_pick_place_dir(void) {
	/* headings a caller of a public inline may pass: negative, past a full
	 * turn, off the grid. Never a shift by a negative count; always a usable
	 * cardinal that is not the kept one; -1 only when fewer than two remain. */
	static const int headings[] = {-450, -360, -270, -180, -90, -1, 0, 45, 90,
								   180, 270, 360, 450, 810};
	for(unsigned m = 0; m < 16u; m++)
		for(size_t h = 0; h < sizeof(headings) / sizeof(headings[0]); h++) {
			int d = mpp_pick_place_dir(m, headings[h]);
			if(mpp_bits4(m) < 2u) {
				assert(d == -1);
				continue;
			}
			assert(d >= 0 && d < 4 && ((m >> d) & 1u));
			/* the cardinal ON the heading is the kept one whenever usable */
			int hn = ((headings[h] % 360) + 360) % 360;
			if(hn % 90 == 0 && ((m >> (hn / 90)) & 1u))
				assert(d != hn / 90);
		}
	/* -90 is 270: with E (3) and S (0) usable, E is kept and S gets the block */
	assert(mpp_pick_place_dir(0x9u, -90) == 0);

	printf("PASS (c12) mpp_pick_place_dir: every mask x 14 headings (negative, "
		   "past a turn, off the grid) picks a usable cardinal that is not the "
		   "kept one\n");
}

/* ---- (c13) pickups do not wait for the inventory screen -------------------- */

static void case_inv_pickup_race(void) {
	struct cfg c;

	/* the run-191831 shape: three items arrive between the two clicks */
	memset(&c, 0, sizeof(c));
	c.pickups_after_pick = 3;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "pickups after the pick");
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.inv_moves == MPP_FLOOR_INV_MOVES);

	/* items that merge into the moved stack right after it lands */
	memset(&c, 0, sizeof(c));
	c.pickups_after_put = 2;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "pickups after the put");
	assert(W.st.phase == MPP_PH_OK);

	/* items that merge into the source stack just before the pick: the cursor
	 * then holds MORE than planned, and the marker must say what really moved */
	memset(&c, 0, sizeof(c));
	c.pickups_before_pick = 2;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "pickups before the pick");
	assert(W.st.phase == MPP_PH_OK);
	{
		int k = mark_index(&W, "inv_move", 0);
		assert(k >= 0);
		assert(body_num(W.marks[k].body, "\"count\":") >= 3);
	}

	printf("PASS (c13) inventory race: pickups arriving before the pick, "
		   "between the clicks and after the put never break a move; the "
		   "destination is the LAST empty slot of the region, where a pickup "
		   "never lands first; a wrong ITEM in the destination still refuses\n");
}

/* ---- (c14) the watched cell ------------------------------------------------ */

/* hw1030g: the extended "never echoed" marker must FIT -- worst-case numbers
 * everywhere, four near-cell ring records -- or it would silently fall back
 * to the plain body and the evidence would be lost. */
static void case_fail_echo_fits(void) {
	static struct mpp_state st;
	static struct mpp_obs o;
	memset(&st, 0, sizeof(st));
	memset(&o, 0, sizeof(o));
	st.phase = MPP_PH_BUILD_DIG;
	st.gen = 4294967295u;
	st.measured_ms = 4294967295u;
	st.echo_seen = 4294967295u;
	st.dig_on_frames = 4294967295u;
	st.wclr_seen = 0;
	o.echo_serial = 4294967295u;
	o.watch_echo = (struct mpp_echo) {.x = -2147483647, .y = -2147483647,
		.z = -2147483647, .type = 255, .serial = 4294967295u};
	o.watch_hist_n = o.watch_bodies = o.watch_clears = 4294967295u;
	for(int k = 0; k < 16; k++)
		o.echoes[k] = (struct mpp_echo) {.x = -30000001, .y = 125,
			.z = -30000001, .type = 255, .serial = 4294967295u - (uint32_t)k};
	mpp_fail_echo(&st, &o, "place never echoed by the server", -30000000, 127,
				  -30000000);
	assert(st.phase == MPP_PH_FAILED);
	const struct mpp_marker* m = mpp_marker_peek(&st);
	assert(m && strcmp(m->key, "fail") == 0);
	assert(strstr(m->body, "\"echo_serial\":4294967295")
		   && strstr(m->body, "\"watch_echo\":[")
		   && strstr(m->body, "\"near\":[[-1,-2,-1,255,4294967295]")
		   && "worst case must keep the extended body and >= 1 near record");
	assert(m->body[strlen(m->body) - 1] == ']');
	assert(strlen(m->body) < MPP_MARKER_BODY);
	printf("PASS hw1030g: the extended never-echoed marker fits (%u of %u B)\n",
		   (unsigned)strlen(m->body), (unsigned)MPP_MARKER_BODY);
}

static void case_watched_cell(void) {
	struct cfg c;
	/* PC run pc_mp_play_20260921_191831 lost its first TWO build legs to
	 * "server echoes overflowed the ring": a server that streams and populates
	 * terrain around a moving player sends block changes in bursts, and sixteen
	 * slots are gone in one packet. Skipping is honest but it throws real digs
	 * away. The script says which ONE cell it waits on, and the network layer
	 * keeps that cell's newest echo outside the ring. EVERY dig leg here gets
	 * a 20-echo burst on top of its own echo. */
	memset(&c, 0, sizeof(c));
	c.echo_burst_legs = 1000;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "watched cell");
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.build_skipped == 0u);
	assert(W.st.digs >= MPP_FLOOR_DIGS && W.st.places >= MPP_FLOOR_PLACES);

	/* A BURST IS NOT A MISSING ECHO. Twenty block changes before the dig has
	 * even completed say nothing about the dig's own echo, which does not exist
	 * yet -- with the watch seam live, no leg may be skipped for it... */
	memset(&c, 0, sizeof(c));
	c.early_burst = true;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK || W.st.build_skipped != 0u)
		dump(&W, "early burst, watch live");
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.build_skipped == 0u);
	/* ...and WITHOUT the seam the early burst alone does not skip either: the
	 * ring still receives the dig's echo afterwards, and finds it. */
	memset(&c, 0, sizeof(c));
	c.early_burst = true;
	c.no_watch = true;
	run_cfg(&c);
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.build_skipped == 0u);

	/* anti-cheat is unchanged: the watch slot holds a SERVER echo for THAT
	 * cell; a stale one (serial at or below the baseline) earns nothing */
	memset(&c, 0, sizeof(c));
	c.dig_echo_stale = true;
	run_cfg(&c);
	assert(W.st.digs == 0u);

	/* hw1030g (real Wii, build 53c75ff): the server's AIR echo for the dug
	 * cell is followed by WATER for the same cell, and a burst evicts both
	 * from the ring. Without the cell's history the newest-only slot says
	 * water and the run fails "dig never echoed by the server" -- the exact
	 * marker of the hardware run, now carrying the evidence that explains
	 * it... */
	memset(&c, 0, sizeof(c));
	c.flood_after_dig_legs = 1;
	c.no_watch_history = true;
	run_cfg(&c);
	expect_fail(&W, "build_dig", "dig never echoed by the server",
				"flooded hole, newest-only slot");
	{
		const char* fb = last_mark(&W, "fail")->body;
		assert(body_has(fb, "\"echo_serial\":")
			   && body_has(fb, "\"echo_seen\":")
			   && body_has(fb, "\"watch_echo\":[")
			   && body_has(fb, "\"watch_n\":")
			   && body_has(fb, "\"watch_clears\":0")
			   && body_has(fb, "\"near\":[")
			   && "a dig miss must carry its own evidence");
		/* the slot really held the masking water record */
		assert(strstr(fb, ",9,") != NULL);
	}
	/* ...and with the history (this fix) the same server earns the dig. */
	memset(&c, 0, sizeof(c));
	c.flood_after_dig_legs = 1000;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "flooded hole, history");
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.digs >= MPP_FLOOR_DIGS);

	printf("PASS (c14) watched cell: with a 20-echo burst on every dig the "
		   "script still sees its own echo through the watch slot -- no leg "
		   "skipped, floors met; a stale echo still earns nothing\n");
}

/* ---- (c15) review B21 (Sol) ------------------------------------------------ */

static void case_review_b21(void) {
	struct cfg c;

	/* M1: a column that was never dug is diggable */
	memset(&c, 0, sizeof(c));
	c.skip_first_dig_then_return = true;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "undug column");
	assert(W.st.phase == MPP_PH_OK);
	{
		int k = mark_index(&W, "build_skipped", 0);
		assert(k >= 0 && body_has(W.marks[k].body, "crosshair never on the dig cell"));
		for(unsigned m = 0; m < W.nmarks; m++)
			assert(!body_has(W.marks[m].body, "still in the column of the last dig"));
	}

	/* M2: the 300 ms hold must be CONTINUOUS */
	memset(&c, 0, sizeof(c));
	c.touch_then_drift = true;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "touch then drift");
	assert(W.st.phase == MPP_PH_OK);
	{
		int k = mark_index(&W, "build_skipped", 0);
		assert(k >= 0 && k < mark_index(&W, "dig", 0));
		assert(body_has(W.marks[k].body, "dig cell changed without a dig"));
	}

	/* M3: one empty hotbar slot + a stray pickup of another item. This fake
	 * keeps the hotbar that way for the whole run, so NO move is ever safe and
	 * the inventory floor honestly fails at p2_end -- what it pins is that the
	 * script never gambles: every leg is skipped BY NAME and the swap failure
	 * ("put-down click never confirmed") never happens. On the real route the
	 * hotbar starts empty and holds two or three kinds at most. */
	memset(&c, 0, sizeof(c));
	c.one_empty_hotbar_slot = true;
	c.stray_pickup_item = 295; /* seeds */
	run_cfg(&c);
	assert(has_mark(&W, "inv_skipped"));
	assert(body_has(last_mark(&W, "inv_skipped")->body,
					"no safe destination slot"));
	assert(W.st.inv_moves == 0u && W.st.digs >= MPP_FLOOR_DIGS);
	expect_fail(&W, "walk", "action floors not met", "one empty slot");
	for(unsigned m = 0; m < W.nmarks; m++)
		assert(!body_has(W.marks[m].body, "put-down click never confirmed"));

	/* LOW: the THIRD lost place leg fails the run by the old name */
	memset(&c, 0, sizeof(c));
	c.unechoed_use_presses = 6; /* three legs x two presses each */
	run_cfg(&c);
	expect_fail(&W, "build_place", "place never echoed by the server",
				"third lost leg");
	assert(mark_count(&W, "build_skipped") == 2u);

	printf("PASS (c15) review B21: an undug column stays diggable, a broken "
		   "hold earns no dig, one empty hotbar slot skips the move by name, "
		   "and the third lost place leg fails by name\n");
}

/* ---- (d) anti-cheat ------------------------------------------------------ */

static void case_anti_cheat(void) {
	struct cfg c;

	memset(&c, 0, sizeof(c));
	c.dig_echo_stale = true;
	run_cfg(&c);
	expect_fail(&W, "build_dig", "dig never echoed by the server",
				"stale echo");
	assert(W.st.digs == 0);

	memset(&c, 0, sizeof(c));
	c.dig_echo_wrong_cell = true;
	run_cfg(&c);
	expect_fail(&W, "build_dig", "dig never echoed by the server",
				"wrong-cell echo");
	assert(W.st.digs == 0);

	memset(&c, 0, sizeof(c));
	c.click_without_txn = true;
	run_cfg(&c);
	expect_fail(&W, "inv_pick", "pick-up click never confirmed", "no txn");
	assert(W.st.inv_moves == 0);
	assert(!has_mark(&W, "inv_move"));

	memset(&c, 0, sizeof(c));
	c.teleport_at_ms = 50000;
	run_cfg(&c);
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.teleports >= 1);
	assert(has_mark(&W, "teleport"));
	assert(!has_mark(&W, "fail"));

	printf("PASS (d) anti-cheat: stale and wrong-cell echoes and an untraded "
		   "slot change earn nothing; a teleport is reported, not fatal\n");
}

/* ---- (e) press hygiene, (f) the clock wrap ------------------------------- */

static void case_press_hygiene(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	world_init(&W, &c);
	run(&W);
	assert(W.st.phase == MPP_PH_OK);
	assert(W.multi_action_frames == 0);
	assert(W.edge_without_release == 0);
	assert(W.use_edges_in_place == W.n_place_legs);
	assert(W.n_place_legs == W.st.places);
	/* four measured moves + the post-reconnect one (REL-14b) */
	assert(W.click_edges_in_pick == 5 && W.click_edges_in_put == 5);
	printf("PASS (e) press hygiene: 0 multi-action frames, 0 press edges "
		   "without a released frame, USE 1x per place leg (%u), GUI_CLICK 1x "
		   "per pick and 1x per put\n",
		   W.n_place_legs);
}

static void case_clock_wrap(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	c.start_now_ms = 0xFFFFFFFFu - 5000u;
	world_init(&W, &c);
	run(&W);
	assert(W.st.phase == MPP_PH_OK);
	assert(W.st.digs >= MPP_FLOOR_DIGS && W.st.places >= MPP_FLOOR_PLACES);
	assert(W.st.inv_moves == MPP_FLOOR_INV_MOVES);
	assert(W.st.now_ms == g_happy_total_ms); /* identical rebased timeline */
	printf("PASS (f) clock wrap: now_ms starting at UINT32_MAX-5000 wraps "
		   "during play and still completes (rebased total %u ms)\n",
		   W.st.now_ms);
}

/* ---- (g) snapshot fidelity and the at-rest gate -------------------------- */

static void case_snapshot_fidelity(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	c.fill_mode = 35; /* 34 stacks; two free hotbar slots for the inv leg */
	world_init(&W, &c);
	run_to_phase(&W, MPP_PH_PAUSE_LEAVE);
	if(mpp_done(&W.st))
		dump(&W, "snapshot_fidelity");
	assert(!mpp_done(&W.st));
	int si = mark_index(&W, "snapshot_pre", 0);
	assert(si >= 0);
	int listed = 0;
	size_t widest = 0;
	for(unsigned k = 0; k < MPP_INV_PARTS; k++) {
		const struct rec* part = &W.marks[si + 1 + k];
		assert(strcmp(part->key, "inv") == 0);
		listed += count_entries(part->body);
		if(strlen(part->body) > widest)
			widest = strlen(part->body);
	}
	int occupied = 0;
	for(int k = 0; k < MPP_INV_SLOTS; k++)
		if(W.st.pre_inv[k].item > 0)
			occupied++;
	/* HEADER DEFECT 8 */
	EXPECT(8, listed == occupied, "snapshot_fidelity",
		   "the three snapshot parts do not list every occupied slot");
	assert(widest < MPP_MARKER_BODY - 1); /* nothing truncated */
	printf("PASS (g) snapshot fidelity: %d occupied slots, %d listed across "
		   "%u parts, widest body %u of %d chars\n",
		   occupied, listed, MPP_INV_PARTS, (unsigned)widest, MPP_MARKER_BODY);
}

/* snapshot_pre must be taken at REST, not while the walker is still drifting */
static void case_snapshot_at_rest(void) {
	struct cfg c;
	memset(&c, 0, sizeof(c));
	c.move_during_pause_ms = 3000; /* keeps drifting after the menu opens */
	world_init(&W, &c);
	run(&W);
	assert(W.st.phase == MPP_PH_OK);
	int si = mark_index(&W, "snapshot_pre", 0);
	assert(si >= 0);
	double snap_x = body_f(W.marks[si].body, "\"x\":");
	/* HEADER DEFECT 9 */
	EXPECT(9, snap_x > (double)W.x_at_pause + 1.0, "snapshot_at_rest",
		   "snapshot_pre recorded the position the body had when the pause "
		   "menu opened, i.e. it was taken while the body was still moving");
	/* the marker prints pre_x to two decimals, so they must agree that closely */
	assert(snap_x - (double)W.st.pre_x < 0.01
		   && (double)W.st.pre_x - snap_x < 0.01);
	assert((double)W.st.pre_x > (double)W.x_at_pause + 1.0);
	printf("PASS (h) at-rest snapshot: body drifted %.2f -> %.2f blocks while "
		   "the menu was open; snapshot_pre recorded %.2f and the rejoin "
		   "verify still passed\n",
		   (double)W.x_at_pause, (double)W.st.pre_x, snap_x);
}

/* ---- main ---------------------------------------------------------------- */

/* ---- (hw1030r) snapshot_pre waits for the inventory to settle --------------
 * Real-Wii mp_play_10m (hw1030r_d6bccdd) failed "inventory changed across the
 * rejoin": snapshot_pre was taken ~0.5 s before the server's collect of a real
 * dig's drop reached the client. The snapshot now waits for 1 s of quiet on
 * the inventory (and on the last dig cell's air echo), bounded, by name. */
static void case_settle_before_snapshot(void) {
	struct cfg c;
	const int16_t LATE = 4; /* cobblestone: an item the play never handles */

	memset(&c, 0, sizeof(c));
	c.late_pickup_ms = 800; /* after p2_end, inside 1 s */
	c.late_pickup_item = LATE;
	run_cfg(&c);
	if(W.st.phase != MPP_PH_OK)
		dump(&W, "late pickup");
	assert(W.late_pickup_done);
	assert(W.st.phase == MPP_PH_OK);
	{
		bool in_pre = false;
		for(int k = 0; k < MPP_INV_SLOTS; k++)
			if(W.st.pre_inv[k].item == LATE && W.st.pre_inv[k].count == 1)
				in_pre = true;
		assert(in_pre); /* the late pickup IS in snapshot_pre */
		int pe = mark_index(&W, "p2_end", 0), sp = mark_index(&W, "snapshot_pre", 0);
		assert(pe >= 0 && sp > pe);
		assert(W.marks[sp].at_ms - W.marks[pe].at_ms
			   >= c.late_pickup_ms + MPP_SETTLE_QUIET_MS);
	}
	assert(has_mark(&W, "verified"));

	/* an inventory that never goes quiet is a named, bounded failure */
	memset(&c, 0, sizeof(c));
	c.inv_never_settles = true;
	c.late_pickup_item = LATE;
	run_cfg(&c);
	expect_fail(&W, "pause_focus", "inventory never settled before the leave",
				"never settles");
	assert(!has_mark(&W, "snapshot_pre"));

	printf("PASS (hw1030r) settle: a pickup landing 800 ms after p2_end is in "
		   "snapshot_pre and the rejoin verifies; an inventory that never "
		   "settles fails by name in pause_focus\n");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); /* an assert must not lose the trace */
	/* The fixture's own coherence with the header it is testing. If the core
	 * ever shortens its settle below this fake's friction, every centring case
	 * would pass for the WRONG reason (the core would look while the body is
	 * still moving and skip), so it is checked here rather than assumed. */
	assert(SETTLE_MS < MPP_NUDGE_SETTLE_MS);
	/* And one tap must fit inside the fake's frame grid at all. */
	assert(MPP_NUDGE_MS > FRAME_MS);
	case_happy_path();
	case_title_up_from_lower_rows();
	case_pointer_ownership_boundaries();
	case_skip_margin();
	case_skip_margin_eleven();
	case_reserve_rule();
	case_latches();
	case_ready_and_login();
	case_build_budgets();
	case_inventory_budgets();
	case_menu_budgets();
	case_verify_and_caps();
	case_marker_overflow();
	case_skips();
	case_centring();
	case_mirrored_facing();
	case_in_water();
	case_landing_wait();
	case_plant_clearing();
	case_catch_up_place();
	case_pickup_missed();
	case_heading_turn();
	case_pit();
	case_pit_escape();
	case_floors_met_stuck();
	case_keep_an_exit();
	case_survey();
	case_landing_support();
	case_review_b17();
	case_pick_place_dir();
	case_inv_pickup_race();
	case_watched_cell();
	case_fail_echo_fits();
	case_review_b21();
	case_rest_before_pause();
	case_short_play();
	case_anti_cheat();
	case_press_hygiene();
	case_clock_wrap();
	case_snapshot_fidelity();
	case_snapshot_at_rest();
	case_settle_before_snapshot();

	printf("defects: %d\n", g_def_n);
	for(int k = 0; k < g_def_n; k++)
		printf("  %d [%s] (%u hit%s): %s\n", g_def[k].id, g_def[k].where,
			   g_def[k].hits, g_def[k].hits == 1 ? "" : "s", g_def[k].what);
	return g_def_n ? 1 : 0;
}
