/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* The split_recovery_6m phase driver. See split_recovery_script.h for the
 * design note and docs/split_recovery_profile.md for the binding contract. */

#include "split_recovery_script.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../block/blocks_data.h"
#include "../entity/entity.h"

#include "../item/inventory.h"
#include "../item/window_container.h"
#include "../network/server_interface.h"
#include "../platform/controller_events.h"
#include "client_session.h"
#include "hw_run.h"
#include "split_layout.h"

/* Sub-step timings. These are WAITS, not assertions: every assertion below is
 * made against observed state, and a wait that never resolves is caught by the
 * phase timeout rather than being assumed to have succeeded. */
#define SR_COLLECT_MS 1500u	  /* fall + Beta's 10-tick EntityItem pickup delay */
/* Bounded wait for a dropped block to actually land in the digger's
 * inventory. Longer than SR_COLLECT_MS because it covers the fall, the pickup
 * delay AND the server round trip of the resulting window update. */
#define SR_PICKUP_WAIT_MS 6000u
/* Bounded wait for the separation walk. */
#define SR_SEPARATION_MS 20000u
/* A body box is 0.6 wide, so it lies inside ONE column only while the position
 * fraction is in [0.3, 0.7]; outside that it also rests on a neighbour, and
 * digging the cell under its feet does not drop it. Measured in the real PC
 * app, 2026-09-17: the walk stopped at z=7.76 (fraction 0.76), the dig removed
 * (0,3,7) correctly, and the body went on standing on (0,3,8) one block above
 * its own drop -- outside the pickup box, which has ZERO y expansion. The host,
 * standing at the spawn fraction 0.5, collected both of its blocks in the same
 * run. 0.35/0.65 keeps a 0.05 margin on the geometric bound. */
/* Consecutive frames of EXACTLY no movement before the body counts as at
 * rest. Five is a handful of frames at any cadence and costs a quarter second
 * on PC; the whole step is bounded by SR_SEPARATION_MS either way. */
#define SR_COAST_FRAMES 5u
#define SR_COLUMN_MIN 0.35F
#define SR_COLUMN_MAX 0.65F
/* Bounded wait for a placed block to be reflected on the client. Generous: it
 * covers the server edit, the column reaching the client and -- since the
 * residency check below means a non-resident column reads as "wait" rather
 * than as stone -- the column streaming in at all. */
#define SR_BLOCK_WAIT_MS 10000u
#define SR_SETTLE_FRAMES 20u  /* consecutive still frames before the pre-save baseline */
#define SR_NEUTRAL_FRAMES 10u /* present+neutral frames that must NOT auto-resume */
#define SR_STILL_EPS 0.05F
/* The layout advances once per rendered frame, so the grace/transition can
 * only be observed as finely as the frame cadence. 10 frames at a 20 fps
 * floor -- the worst cadence the split profiles have measured on hardware. */
#define SR_FRAME_TOLERANCE_MS 500u
/* The lower timing bounds credit the worst frame seen in the window (the
 * driver samples once per frame), but never more than two 20 fps frames: an
 * uncapped credit would let one frame spanning half the window pass a collapse
 * up to twice too fast (delta review 2026-09-17). */
#define SR_LOWER_CREDIT_CAP_MS 100u

struct sr_baseline {
	bool set;
	uint32_t hash, used;
	float x, y, z;
};

struct sr_block {
	bool set;
	int x, y, z;	   /* the placed cell */
	int cx, cy, cz;	   /* the cell clicked on its top face */
	uint32_t id, meta;
};

static struct {
	bool armed;
	struct split_recovery_hooks hooks;
	enum split_recovery_phase phase;
	uint32_t seq;

	bool clock_set;
	uint32_t base_ms, now_ms;
	uint32_t phase_start_ms, step_start_ms;
	unsigned step;
	unsigned frames_in_step;
	bool done_sent;

	/* One in-flight marker. A refused emit is retried VERBATIM (same seq) on
	 * the next frame, so the collector may assume a gapless sequence. */
	char pend[768];
	char pend_key[64];
	bool pend_valid;

	/* P0 */
	bool world_asked;
	bool residency_walks;
	float walk_x[2], walk_z[2];
	unsigned walk_actor;
	bool autoload_asked;	 /* H2: arm_autoload is a ONE-SHOT, emit or not */
	uint32_t last_frame_ms;	 /* previous frame stamp, for the frame-dt max */
	uint32_t max_frame_ms;	 /* worst frame gap inside the window being timed */
	unsigned cycle;
	unsigned digs;			 /* digs completed by the actor currently digging */
	uint32_t pickup_before;	 /* that actor's item count when the dig was posted */
	int guest_col_x, guest_col_y, guest_col_z; /* post-separation guest column */
	int host_col_x, host_col_y, host_col_z; /* the host's surface cell at its first dig */
	bool host_col_set;
	bool guest_col_set;
	unsigned coast_frames;	 /* consecutive frames the guest has not moved AT ALL */
	struct sr_block blocks[SPLIT_RECOVERY_BLOCKS];
	struct sr_baseline base[2], presave[2];

	/* P1 */
	uint32_t loss_ms;
	uint32_t sim_ticks_at_loss;
	unsigned frames_since_loss;
	uint32_t collapse_begin_ms;
	/* A refused observer emit (ring full) brings a step back next frame with
	 * its marker still pending. Anything a step MEASURES is therefore latched
	 * on first observation and judged on the latched value -- re-stamping or
	 * re-measuring on the retry frame would report transport back-pressure as
	 * a recovery-timing defect (delta review 2026-09-17). Cleared by sr_step. */
	bool step_latched;
	uint32_t step_reading_ms;
	uint32_t step_frame_ms; /* worst frame inside the latched window */
	/* The END of the collapse is watched from the moment its beginning is
	 * latched, even while the collapse_begin marker is still waiting for a
	 * free sender slot: the ease takes 350 ms and a refused emit can outlast
	 * it, and a reading taken only once step 4 starts would then measure the
	 * back-pressure, not the transition. */
	bool done_seen;
	uint32_t done_reading_ms, done_frame_ms, frames_at_done;
	unsigned frames_at_collapse;
	unsigned neutral_frames;
	int guest_phase_at_loss;

	/* P2 */
	uint64_t leave_gen;
	uint32_t leave_corr;
	bool suppress_join;

	/* P3 */
	uint32_t exit_epoch, exit_corr;
	bool reset_seen;
	float settle_x[2], settle_y[2], settle_z[2];
	unsigned settle_frames;

	/* P4 */
	unsigned verified_blocks;
} s;

/* ---- plumbing ---------------------------------------------------------- */

void split_recovery_reset(void) {
	memset(&s, 0, sizeof(s));
}

void split_recovery_arm(const struct hw_run_config* cfg,
						const struct split_recovery_hooks* hooks) {
	split_recovery_reset();
	/* split_guest is not optional: every phase needs a guest body to lose,
	 * leave and re-admit, and without it P0 would sit in its admission wait
	 * until the phase timeout. Refuse to arm rather than fail slowly. */
	if(!cfg || !hooks || !cfg->enabled || !cfg->split_recovery
	   || !cfg->split_guest)
		return;
	s.hooks = *hooks;
	s.armed = true;
	s.residency_walks = cfg->profile == HW_RUN_PROFILE_SPLIT_RECOVERY_6M;
}

bool split_recovery_armed(void) {
	return s.armed;
}

enum split_recovery_phase split_recovery_phase_now(void) {
	return s.phase;
}

uint32_t split_recovery_seq_now(void) {
	return s.seq;
}

unsigned split_recovery_step_now(void) {
	return s.step;
}

bool split_recovery_rig_create_allowed(const char* directory) {
	return s.armed && directory
		&& strcmp(directory, SPLIT_RECOVERY_RIG_WORLD) == 0;
}

static const char* sr_phase_name(void) {
	switch(s.phase) {
		case SPLIT_RECOVERY_P0: return "p0";
		case SPLIT_RECOVERY_P1: return "p1";
		case SPLIT_RECOVERY_P2: return "p2";
		case SPLIT_RECOVERY_P3: return "p3";
		case SPLIT_RECOVERY_P4: return "p4";
		default: return "run";
	}
}

/* Format and try to publish one marker. `key` identifies WHICH marker is in
 * flight: a caller retrying the same marker next frame re-flushes the STORED
 * line (same seq, same bytes) instead of re-formatting it, and a caller that
 * asks for a different marker while one is still pending is refused until the
 * ring drains. That is what lets the collector assume a gapless `seq`.
 *
 * The published marker NAME is the key minus its "<phase>." prefix and minus
 * any trailing digits: the three per-block markers need distinct keys
 * ("p0.block_placed0/1/2") but publish the one contract name
 * ("block_placed") with an `i` field. No contract marker name ends in a
 * digit, so the strip is unambiguous. */
static bool sr_mark_corr(const char* key, uint32_t corr, bool ok,
						 const char* fields_fmt, ...) {
	if(s.pend_valid && strcmp(s.pend_key, key) != 0)
		return false;
	if(!s.pend_valid) {
		char fields[384];
		fields[0] = 0;
		if(fields_fmt && fields_fmt[0]) {
			va_list ap;
			va_start(ap, fields_fmt);
			vsnprintf(fields, sizeof(fields), fields_fmt, ap);
			va_end(ap);
		}
		const char* dot = strchr(key, '.');
		const char* name = dot ? dot + 1 : key;
		char pretty[48];
		snprintf(pretty, sizeof(pretty), "%s", name);
		for(size_t k = strlen(pretty); k > 0; k--) {
			if(pretty[k - 1] < '0' || pretty[k - 1] > '9')
				break;
			pretty[k - 1] = 0;
		}
		int n = snprintf(s.pend, sizeof(s.pend),
			"{\"event\":\"split_recovery\",\"seq\":%u,\"phase\":\"%s\","
			"\"marker\":\"%s\",\"ms\":%u,\"corr\":%u,\"ok\":%s%s%s}",
			s.seq + 1u, sr_phase_name(), pretty, s.now_ms, corr,
			ok ? "true" : "false", fields[0] ? "," : "", fields);
		if(n <= 0 || (size_t)n >= sizeof(s.pend))
			return false;
		snprintf(s.pend_key, sizeof(s.pend_key), "%s", key);
		s.pend_valid = true;
	}
	if(!s.hooks.emit || !s.hooks.emit(s.hooks.user, s.pend))
		return false;
	s.pend_valid = false;
	s.pend_key[0] = 0;
	s.seq++;
	return true;
}

/* corr 0: this marker answers no request of the driver's own. */
#define sr_mark(key, ok, ...) sr_mark_corr((key), 0u, (ok), __VA_ARGS__)

/* Every string the driver copies into a marker comes from somewhere it does
 * not control -- the server's own "Save failed: <stage>" toast most of all --
 * so it is escaped, not pasted. Without this one quote in a toast produces a
 * line no JSON parser will take, and the collector loses the whole run's
 * evidence to a malformed-marker error instead of reading the diagnosis.
 * RFC 8259: the two mandatory escapes plus every control character. */
static void sr_json_escape(char* dst, size_t cap, const char* src) {
	size_t w = 0;
	if(!dst || !cap)
		return;
	for(size_t r = 0; src && src[r] && w + 1 < cap; r++) {
		unsigned char ch = (unsigned char)src[r];
		const char* two = NULL;
		switch(ch) {
			case '"': two = "\\\""; break;
			case '\\': two = "\\\\"; break;
			case '\n': two = "\\n"; break;
			case '\r': two = "\\r"; break;
			case '\t': two = "\\t"; break;
			case '\b': two = "\\b"; break;
			case '\f': two = "\\f"; break;
			default: break;
		}
		if(two) {
			if(w + 2 >= cap)
				break;
			dst[w++] = two[0];
			dst[w++] = two[1];
		} else if(ch < 0x20 || ch == 0x7F) {
			if(w + 6 >= cap)
				break;
			w += (size_t)snprintf(dst + w, cap - w, "\\u%04X", ch);
		} else
			dst[w++] = (char)ch;
	}
	dst[w] = 0;
}

/* End the run.
 *
 * `marker` is the contract marker the phase was AWAITING when it gave up: it
 * is published with ok:false and the diagnostic, which is what the collector
 * reads ("phase failure p1.collapse_done: collapse took 9000 ms"). NULL means
 * no single marker was outstanding, and the run publishes "<phase>.fail"
 * instead -- both forms are contract-legal, but the specific one says WHICH
 * observation never arrived.
 *
 * The marker is one best-effort attempt; profile_done is UNCONDITIONAL, so a
 * run always carries its verdict even if the observer ring refused the line. */
static void sr_fail_at(const char* marker, const char* diag) {
	char status[96];
	const char* phase = sr_phase_name();
	if(s.pend_valid) { /* a stuck marker must not block the verdict */
		s.pend_valid = false;
		s.pend_key[0] = 0;
	}
	char escaped[160];
	sr_json_escape(escaped, sizeof(escaped), diag);
	char key[80];
	snprintf(key, sizeof(key), "%s.%s", phase, marker ? marker : "fail");
	(void)sr_mark(key, false, "\"diag\":\"%s\"", escaped);
	snprintf(status, sizeof(status), "%s:%s", phase, diag);
	s.phase = SPLIT_RECOVERY_FAILED;
	if(!s.done_sent && s.hooks.profile_done) {
		s.done_sent = true;
		s.hooks.profile_done(s.hooks.user, status);
	}
}

static void sr_fail(const char* diag) {
	sr_fail_at(NULL, diag);
}

static void sr_enter(enum split_recovery_phase phase) {
	s.phase = phase;
	s.phase_start_ms = s.now_ms;
	s.step = 0;
	s.step_start_ms = s.now_ms;
	s.frames_in_step = 0;
}

static void sr_step(unsigned next) {
	s.step = next;
	s.step_start_ms = s.now_ms;
	s.frames_in_step = 0;
	s.step_latched = false;
	s.step_reading_ms = 0;
	s.step_frame_ms = 0;
}

static uint32_t sr_phase_budget(void) {
	switch(s.phase) {
		case SPLIT_RECOVERY_P0: return SPLIT_RECOVERY_P0_MS;
		case SPLIT_RECOVERY_P1: return SPLIT_RECOVERY_P1_MS;
		case SPLIT_RECOVERY_P2: return SPLIT_RECOVERY_P2_MS;
		case SPLIT_RECOVERY_P3: return SPLIT_RECOVERY_P3_MS;
		case SPLIT_RECOVERY_P4: return SPLIT_RECOVERY_P4_MS;
		default: return SPLIT_RECOVERY_TOTAL_MS;
	}
}

/* ---- observation helpers ------------------------------------------------ */

static struct inventory* sr_inventory(struct client_session* session) {
	if(!session || !session->windows[WINDOWC_INVENTORY])
		return NULL;
	return windowc_get_latest(session->windows[WINDOWC_INVENTORY]);
}

/* FNV-1a over every slot in slot order: id (LE), count, durability (LE). An
 * absent inventory hashes as all-empty rather than as a magic value, so a
 * missing window reads as "nothing here" and is caught by the count. */
static uint32_t sr_inventory_hash(struct client_session* session,
								  uint32_t* used_out) {
	struct inventory* inv = sr_inventory(session);
	uint32_t hash = 2166136261u, used = 0;
	for(size_t slot = 0; slot < INVENTORY_SIZE; slot++) {
		struct item_data item = {0};
		if(inv)
			(void)inventory_get_slot(inv, slot, &item);
		if(!item.id || !item.count)
			item = (struct item_data) {0};
		else
			used++;
		uint8_t bytes[5] = {(uint8_t)(item.id & 0xFF),
							(uint8_t)(item.id >> 8), item.count,
							(uint8_t)(item.durability & 0xFF),
							(uint8_t)(item.durability >> 8)};
		for(size_t k = 0; k < sizeof(bytes); k++) {
			hash ^= bytes[k];
			hash *= 16777619u;
		}
	}
	if(used_out)
		*used_out = used;
	return hash;
}

/* The first slot whose contents differ from the recorded raw baseline, for a
 * diagnostic a console run can act on. Kept beside the hash on purpose: a bare
 * "hash mismatch" names nothing. */
static struct item_data sr_raw[2][INVENTORY_SIZE];
static struct item_data sr_raw_presave[2][INVENTORY_SIZE];

static void sr_capture_raw(struct client_session* session,
						   struct item_data out[INVENTORY_SIZE]) {
	struct inventory* inv = sr_inventory(session);
	for(size_t slot = 0; slot < INVENTORY_SIZE; slot++) {
		struct item_data item = {0};
		if(inv)
			(void)inventory_get_slot(inv, slot, &item);
		if(!item.id || !item.count)
			item = (struct item_data) {0};
		out[slot] = item;
	}
}

static int sr_first_diff(struct client_session* session,
						 const struct item_data want[INVENTORY_SIZE]) {
	struct item_data now[INVENTORY_SIZE];
	sr_capture_raw(session, now);
	for(size_t slot = 0; slot < INVENTORY_SIZE; slot++)
		if(now[slot].id != want[slot].id || now[slot].count != want[slot].count
		   || now[slot].durability != want[slot].durability)
			return (int)slot;
	return -1;
}

static void sr_record_baseline(struct client_session* session,
							   struct sr_baseline* into,
							   struct item_data raw[INVENTORY_SIZE]) {
	uint32_t used = 0;
	into->hash = sr_inventory_hash(session, &used);
	into->used = used;
	into->x = session ? session->camera.x : 0;
	into->y = session ? session->camera.y : 0;
	into->z = session ? session->camera.z : 0;
	into->set = true;
	sr_capture_raw(session, raw);
}

/* Total items held, in slot order. The pickup observation is an INCREASE in
 * this across a dig -- the block really reached the digger, which is what
 * "accepted" means everywhere else in this codebase. */
static uint32_t sr_inventory_count(struct client_session* session) {
	struct inventory* inv = sr_inventory(session);
	uint32_t total = 0;
	for(size_t slot = 0; inv && slot < INVENTORY_SIZE; slot++) {
		struct item_data item = {0};
		if(inventory_get_slot(inv, slot, &item) && item.id && item.count)
			total += item.count;
	}
	return total;
}

/* True while the body lies inside a single column AND has stopped moving
 * entirely. Both halves are load-bearing and both were learned from the real
 * app: a straddling body does not fall into the cell it digs (so it never
 * reaches its own drop), and a body that still looks stopped to an epsilon
 * keeps creeping back onto a boundary while the digs run. */
static bool sr_planted(const struct split_recovery_ctx* ctx,
					   struct client_session* who) {
	float fx = who->camera.x - floorf(who->camera.x);
	float fz = who->camera.z - floorf(who->camera.z);
	const unsigned actor = who == ctx->host ? 0 : 1;
	bool moving = who->camera.x != s.settle_x[actor]
		|| who->camera.z != s.settle_z[actor];
	s.settle_x[actor] = who->camera.x;
	s.settle_z[actor] = who->camera.z;
	s.coast_frames = moving ? 0 : s.coast_frames + 1;
	return fx >= SR_COLUMN_MIN && fx <= SR_COLUMN_MAX && fz >= SR_COLUMN_MIN
		&& fz <= SR_COLUMN_MAX && s.coast_frames >= SR_COAST_FRAMES;
}

static bool sr_guest_admitted(const struct split_recovery_ctx* ctx) {
	return ctx->guest && ctx->guest->local_gameplay_ready
		&& ctx->guest->local_player;
}

/* The block the body is standing ON. Eye-basis position and the same 0.2
 * footing epsilon main.c's existing guest action script uses; 1.62 is Beta's
 * EntityPlayer eye height. */
static bool sr_feet_cell(struct client_session* session, int* x, int* y,
						 int* z) {
	if(!session || !session->local_player)
		return false;
	*x = (int)floorf(session->camera.x);
	*y = (int)floorf(session->camera.y - 1.62F - 0.2F);
	*z = (int)floorf(session->camera.z);
	return true;
}

static uint32_t sr_block_id(const struct split_recovery_ctx* ctx, int x, int y,
							int z, uint32_t* meta) {
	uint32_t id = 0;
	(void)ctx;
	if(meta)
		*meta = 0;
	if(!s.hooks.block_at
	   || !s.hooks.block_at(s.hooks.user, x, y, z, &id, meta))
		return 0;
	return id;
}

static bool sr_post(struct server_rpc* call) {
	return s.hooks.rpc_guest && s.hooks.rpc_guest(s.hooks.user, call);
}

static bool sr_post_host(struct server_rpc* call) {
	return s.hooks.rpc_host && s.hooks.rpc_host(s.hooks.user, call);
}

/* One finished dig of `cell`, through the owner's own production mailbox. */
static bool sr_dig(bool host, int x, int y, int z) {
	struct server_rpc call = {
		.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.finished = true, .x = (w_coord_t)x,
							  .y = (w_coord_t)y, .z = (w_coord_t)z,
							  .side = SIDE_TOP}};
	return host ? sr_post_host(&call) : sr_post(&call);
}

/* The guest's five dig cells, fixed from the column it stands in once the
 * separation walk has finished. Cells 0/1/4 are its own feet column, so it
 * sinks one layer each time (superflat is bedrock/dirt/dirt/grass at y 0..3);
 * cells 2/3 are DIAGONAL neighbours, dug while the body is already one or two
 * layers down so the dropped item shares its y range -- the pickup box is the
 * body box expanded 1.0 in x and z and ZERO in y (entity_item.c), and a
 * diagonal neighbour is 1.0 on each axis, inside that box. None of the five is
 * one of the three placement targets, which stay solid floor to be clicked. */
/* 2026-09-17 (lead, real PC app): ONLY own-column digs are deterministic. A
 * drop pops upward (vy 0.2) with up to +-0.1 of horizontal speed, so a drop
 * from a NEIGHBOUR cell (diagonal: 2 runs of 4 missed; near-side orthogonal: 1
 * of 3 missed) can clear the pit rim and come to rest on the surface at y+1,
 * above the y range of a body standing in its shaft -- the pickup box has ZERO
 * y expansion (entity_item.c). A drop from the cell under the feet is different
 * in kind: the body falls INTO that cell, and wherever the drop ends (in the
 * shaft or on the rim) it is inside the box on all three axes. Superflat gives
 * each column exactly three diggable layers (grass, dirt, dirt over bedrock). */
static void sr_guest_dig_cell(unsigned i, int* x, int* y, int* z) {
	*x = s.guest_col_x;
	*y = s.guest_col_y - (int)i;
	*z = s.guest_col_z;
}

/* A synthetic owned frame for one of the two local players. `connected` false
 * is exactly what a lost controller looks like to
 * local_menu_host_recovery_frame's no-hardware-inventory branch, which is the
 * production entry into controller_recovery_step/commit. */
static void sr_frame(struct input_player_frame* f, bool connected) {
	memset(f, 0, sizeof(*f));
	f->connected = connected;
	f->category = INPUT_CAT_NUNCHUK;
	for(int b = 0; b < IB_COUNT; b++)
		f->glyphs[b] = -1;
}

/* ---- phases ------------------------------------------------------------- */

/* P0 -- BASELINE.
 *
 * Both actors must end this phase holding a NON-EMPTY inventory, obtained the
 * only way the game offers one: dig a block and pick the drop up. Without that
 * every later "the inventory came back" assertion would be comparing two empty
 * inventories and proving nothing (Astra change 6). There is no give, no
 * creative and no direct inventory write anywhere in this driver.
 *
 * ORDER MATTERS, and the reason is the pickup box. A guest is admitted at the
 * host's position, so until the guest walks away the two bodies share a column
 * and both are inside the +-1.0 x/z pickup box of every item either of them
 * drops -- whichever the server asks first would collect the other's block.
 * So: admit, SEPARATE, then host digs, then guest digs, then guest places. */
static void sr_phase_p0(const struct split_recovery_ctx* ctx,
						struct split_recovery_actions* out) {
	/* The guest body is DRIVEN for the whole phase, not just while it walks:
	 * an undriven body has no capture_input and therefore no physics, so it
	 * could not fall into the hole it just dug and never reached its own drop
	 * (see the field comment). The walk itself is one step below. */
	out->drive_guest_body = true;
	switch(s.step) {
		case 0: /* create the disposable world, once */
			if(!s.world_asked) {
				/* BEFORE anything is deleted: if this card boot already spent
				 * screen_select_world own process-lifetime autoload latch (its
				 * config carries an "autoload" key), P4 could arm nothing and
				 * would sit out its budget in silence. Say so now, and leave
				 * the rig world on disk untouched. */
				const char* conflict = s.hooks.autoload_conflict
					? s.hooks.autoload_conflict(s.hooks.user) : NULL;
				if(conflict) {
					char diag[96];
					snprintf(diag, sizeof(diag),
							 "autoload latch already consumed: config "
							 "autoload=%s", conflict);
					sr_fail(diag);
					return;
				}
				if(!s.hooks.create_world
				   || !s.hooks.create_world(s.hooks.user,
											SPLIT_RECOVERY_RIG_WORLD)) {
					sr_fail("rig world create refused");
					return;
				}
				s.world_asked = true;
			}
			if(ctx->world_loaded && !ctx->world_switch_pending && ctx->host_body)
				sr_step(1);
			return;
		case 1: /* the existing synthetic join admits the guest */
			if(!sr_guest_admitted(ctx))
				return;
			if(sr_mark("p0.guest_admitted",
					   true, "\"gen\":%llu,\"entity\":%u,\"x\":%.2f,\"y\":%.2f,"
					   "\"z\":%.2f",
					   (unsigned long long)ctx->guest->local_generation,
					   ctx->guest->local_player->id, ctx->guest->camera.x,
					   ctx->guest->camera.y, ctx->guest->camera.z))
			{
				s.walk_x[0] = ctx->host->camera.x;
				s.walk_z[0] = ctx->host->camera.z;
				s.walk_x[1] = ctx->guest->camera.x;
				s.walk_z[1] = ctx->guest->camera.z;
				sr_step(s.residency_walks ? 20 : 2);
			}
			return;
		case 20: { /* Both ordinary walks precede every stationary baseline. */
			unsigned a = s.walk_actor;
			struct client_session* who = a ? ctx->guest : ctx->host;
			float distance = hypotf(who->camera.x - s.walk_x[a],
				who->camera.z - s.walk_z[a]);
			bool planted = sr_planted(ctx, who);
			if(distance >= 40.0F && planted) {
				if(++s.walk_actor == 2) sr_step(2);
				else sr_step(20);
				return;
			}
			if(s.now_ms - s.step_start_ms >= 30000u) {
				sr_fail("ordinary walk did not settle");
				return;
			}
			if(distance < 40.0F || s.coast_frames >= SR_COAST_FRAMES) {
				out->drive_host_walk = !a;
				out->drive_guest_walk = a;
			}
			return;
		}
		case 2: { /* walk the guest out of the host's pickup box */
			float dx = fabsf(ctx->guest->camera.x - ctx->host->camera.x);
			float dz = fabsf(ctx->guest->camera.z - ctx->host->camera.z);
			bool far_enough = dx >= SPLIT_RECOVERY_SEPARATION
				|| dz >= SPLIT_RECOVERY_SEPARATION;
			bool planted = sr_planted(ctx, ctx->guest);
			if(far_enough && planted) {
				s.digs = 0;
				sr_step(3);
				return;
			}
			if(s.now_ms - s.step_start_ms >= SR_SEPARATION_MS) {
				char diag[96];
				snprintf(diag, sizeof(diag),
						 "guest never separated: dx=%.2f dz=%.2f coast=%u", dx,
						 dz, s.coast_frames);
				sr_fail(diag);
				return;
			}
			/* Drive only while there is ground left to cover, or while the body
			 * has come to REST somewhere it cannot dig from; never while it is
			 * still coasting, or it would never settle anywhere. */
			if(!far_enough || s.coast_frames >= SR_COAST_FRAMES)
				out->drive_guest_walk = true;
			return;
		}
		case 3: { /* HOST dig: its own feet cell, sinking one layer each time */
			int fx, fy, fz;
			if(!sr_feet_cell(ctx->host, &fx, &fy, &fz) || fy < 1) {
				sr_fail("no diggable floor under host");
				return;
			}
			if(!s.host_col_set) {
				s.host_col_x = fx;
				s.host_col_y = fy;
				s.host_col_z = fz;
				s.host_col_set = true;
			}
			s.pickup_before = sr_inventory_count(ctx->host);
			if(sr_dig(true, fx, fy, fz)) /* a refused post retries next frame */
				sr_step(4);
			return;
		}
		case 4: /* the drop must be OBSERVED in the host's own inventory */
			if(sr_inventory_count(ctx->host) <= s.pickup_before) {
				if(s.now_ms - s.step_start_ms >= SR_PICKUP_WAIT_MS)
					sr_fail("pickup never observed: actor 0");
				return;
			}
			sr_step(++s.digs < SPLIT_RECOVERY_HOST_DIGS ? 3 : 5);
			return;
		case 5: /* GUEST digs, from the column it settled in after the walk */
			if(!s.guest_col_set) {
				int fx, fy, fz;
				if(!sr_feet_cell(ctx->guest, &fx, &fy, &fz) || fy < 3) {
					sr_fail("no diggable floor under guest");
					return;
				}
				s.guest_col_x = fx;
				s.guest_col_y = fy;
				s.guest_col_z = fz;
				s.guest_col_set = true;
				s.digs = 0;
				/* The three placement targets, one cell above untouched floor.
				 * The guest walks +Z and digs one cell per column, so the dug
				 * cells are all behind it on its own x line: the two side
				 * columns and the column AHEAD are all still solid, and none
				 * is the cell the body occupies -- the two-body placement
				 * obstruction sweep would rightly refuse that. Recomputed at
				 * every dig, so they belong to the LAST column it stood in. */
				/* Blocks 0 and 1 are the GUEST's, block 2 the HOST's: each
				 * actor dug three and must keep a DIFFERENT remainder (guest
				 * 1, host 2) so an ownership swap shows in the hashes. The
				 * clicked cells are diagonal surface neighbours of the
				 * placer's own column -- never a dig cell, so every clicked
				 * top face stays solid. Placement has no drop geometry. */
				const int bx[SPLIT_RECOVERY_BLOCKS] = {1, -1, 1};
				const int bz[SPLIT_RECOVERY_BLOCKS] = {1, 1, 1};
				for(unsigned i = 0; i < SPLIT_RECOVERY_BLOCKS; i++) {
					bool by_host = i == SPLIT_RECOVERY_BLOCKS - 1;
					int ox = by_host ? s.host_col_x : fx;
					int oy = by_host ? s.host_col_y : fy;
					int oz = by_host ? s.host_col_z : fz;
					s.blocks[i].cx = ox + bx[i];
					s.blocks[i].cy = oy;
					s.blocks[i].cz = oz + bz[i];
					s.blocks[i].x = s.blocks[i].cx;
					s.blocks[i].y = oy + 1;
					s.blocks[i].z = s.blocks[i].cz;
				}
			}
			{
				int dx, dy, dz;
				sr_guest_dig_cell(s.digs, &dx, &dy, &dz);
				if(dy < 1) {
					sr_fail("no diggable floor under guest");
					return;
				}
				s.pickup_before = sr_inventory_count(ctx->guest);
				if(sr_dig(false, dx, dy, dz))
					sr_step(6);
			}
			return;
		case 6: /* the drop must be OBSERVED in the guest own inventory */
			if(sr_inventory_count(ctx->guest) <= s.pickup_before) {
				if(s.now_ms - s.step_start_ms >= SR_PICKUP_WAIT_MS)
					sr_fail("pickup never observed: actor 1");
				return;
			}
			sr_step(++s.digs < SPLIT_RECOVERY_GUEST_DIGS ? 5 : 7);
			return;
		case 7: { /* hold the dug block */
			struct server_rpc call = {.type = SRPC_HOTBAR_SLOT,
									  .payload.hotbar_slot.slot = 0};
			bool by_host = s.cycle == SPLIT_RECOVERY_BLOCKS - 1;
			if(by_host ? sr_post_host(&call) : sr_post(&call))
				sr_step(8);
			return;
		}
		case 8: { /* PLACE it: click the cell below the target, top face */
			struct sr_block* b = &s.blocks[s.cycle];
			struct server_rpc call = {
				.type = SRPC_BLOCK_PLACE,
				.payload.block_place = {.x = (w_coord_t)b->cx,
										.y = (w_coord_t)b->cy,
										.z = (w_coord_t)b->cz,
										.side = SIDE_TOP,
										.has_held_item = false}};
			bool by_host = s.cycle == SPLIT_RECOVERY_BLOCKS - 1;
			if(by_host ? sr_post_host(&call) : sr_post(&call))
				sr_step(9);
			return;
		}
		case 9: { /* the SERVER's edit reflected on the client is the proof */
			struct sr_block* b = &s.blocks[s.cycle];
			uint32_t meta = 0;
			uint32_t id = sr_block_id(ctx, b->x, b->y, b->z, &meta);
			if(!id) {
				/* Bounded: an unreflected edit must name itself rather than
				 * spending the whole phase budget in silence. */
				if(s.now_ms - s.step_start_ms >= SR_BLOCK_WAIT_MS) {
					char diag[64];
					snprintf(diag, sizeof(diag), "block %u never reflected",
							 s.cycle);
					sr_fail_at("block_placed", diag);
				}
				return;
			}
			b->id = id;
			b->meta = meta;
			b->set = true;
			char key[32];
			snprintf(key, sizeof(key), "p0.block_placed%u", s.cycle);
			if(!sr_mark(key, true,
						"\"i\":%u,\"x\":%d,\"y\":%d,\"z\":%d,\"id\":%u,"
						"\"meta\":%u",
						s.cycle, b->x, b->y, b->z, b->id, b->meta))
				return;
			if(++s.cycle < SPLIT_RECOVERY_BLOCKS)
				sr_step(7);
			else
				sr_step(10);
			return;
		}
		case 10:
			sr_record_baseline(ctx->host, &s.base[0], sr_raw[0]);
			/* An empty baseline would make every later inventory assertion
			 * vacuous, so it is a FAILURE here rather than a quiet pass. */
			if(!s.base[0].used) {
				sr_fail_at("baseline", "baseline inventory empty: actor 0");
				return;
			}
			if(sr_mark("p0.baseline0", true,
					   "\"actor\":0,\"inv_hash\":%u,\"inv_used\":%u,\"x\":%.2f,"
					   "\"y\":%.2f,\"z\":%.2f",
					   s.base[0].hash, s.base[0].used, s.base[0].x,
					   s.base[0].y, s.base[0].z))
				sr_step(11);
			return;
		case 11:
			sr_record_baseline(ctx->guest, &s.base[1], sr_raw[1]);
			if(!s.base[1].used) {
				sr_fail_at("baseline", "baseline inventory empty: actor 1");
				return;
			}
			/* Two identical inventories hash alike, and then every later
			 * per-owner comparison would pass even if host and guest had been
			 * handed each other's items (seen in the first full PC pass,
			 * 2026-09-17: both held two dirt). The plan makes them differ. */
			if(s.base[1].hash == s.base[0].hash) {
				sr_fail_at("baseline", "baselines indistinguishable: actor 0 == actor 1");
				return;
			}
			if(sr_mark("p0.baseline1", true,
					   "\"actor\":1,\"inv_hash\":%u,\"inv_used\":%u,\"x\":%.2f,"
					   "\"y\":%.2f,\"z\":%.2f",
					   s.base[1].hash, s.base[1].used, s.base[1].x,
					   s.base[1].y, s.base[1].z)) {
				s.guest_phase_at_loss = (int)ctx->guest->local_phase;
				sr_enter(SPLIT_RECOVERY_P1);
			}
			return;
		default:
			sr_fail("p0 step");
			return;
	}
}

static void sr_phase_p1(const struct split_recovery_ctx* ctx,
						struct split_recovery_actions* out) {
	/* Until the deliberate resume the guest's device is GONE; afterwards it is
	 * present. Both are expressed as the owned frame main.c substitutes into
	 * the production recovery handler, never as a hand-written phase. */
	out->guest_frame_valid = true;
	sr_frame(&out->guest_frame, s.step >= 6);
	const struct controller_recovery* rec = &ctx->guest->menu_host.recovery;
	switch(s.step) {
		case 0:
			if(rec->phase == CONTROLLER_RECOVERY_NONE)
				return; /* the handler has not consumed the lost frame yet */
			if(!s.step_latched) { /* stamp ONCE, see step_latched */
				s.step_latched = true;
				s.loss_ms = s.now_ms;
				s.sim_ticks_at_loss = ctx->sim_ticks;
				s.frames_since_loss = 0;
				s.max_frame_ms = 0; /* the timed window starts here */
				s.done_seen = false;
			}
			if(sr_mark("p1.loss_injected", true,
					   "\"reason\":%u,\"rec_phase\":%u,\"owns_pause\":%s",
					   (unsigned)rec->reason, (unsigned)rec->phase,
					   rec->owns_pause ? "true" : "false"))
				sr_step(1);
			return;
		case 1:
			if((int)ctx->guest->local_phase != s.guest_phase_at_loss
			   || !sr_guest_admitted(ctx)) {
				sr_fail_at("no_leave", "guest retired on loss");
				return;
			}
			if(sr_mark("p1.no_leave", true,
					   "\"local_phase\":%u,\"gen\":%llu,\"ready\":true",
					   (unsigned)ctx->guest->local_phase,
					   (unsigned long long)ctx->guest->local_generation))
				sr_step(2);
			return;
		case 2:
			if(ctx->sim_ticks <= s.sim_ticks_at_loss)
				return; /* the phase timeout catches a world that froze */
			if(sr_mark("p1.ticks_advanced", true, "\"ticks\":%u",
					   ctx->sim_ticks - s.sim_ticks_at_loss))
				sr_step(3);
			return;
		case 3: {
			s.frames_since_loss++;
			if(!ctx->layout || ctx->layout->target < 1.0F)
				return;
			if(!s.step_latched) { /* measure ONCE, see step_latched */
				s.step_latched = true;
				s.step_reading_ms = s.now_ms - s.loss_ms;
				s.step_frame_ms = s.max_frame_ms;
				s.collapse_begin_ms = s.now_ms;
				s.frames_at_collapse = s.frames_since_loss;
				s.max_frame_ms = 0; /* the transition is its own timed window */
			}
			else if(!s.done_seen && ctx->layout->boundary >= 1.0F) {
				/* marker still pending, but the transition just ended */
				s.done_seen = true;
				s.done_reading_ms = s.now_ms - s.collapse_begin_ms;
				s.done_frame_ms = s.max_frame_ms;
				s.frames_at_done = s.frames_since_loss;
			}
			uint32_t grace = s.step_reading_ms;
			uint32_t grace_credit = s.step_frame_ms < SR_LOWER_CREDIT_CAP_MS
				? s.step_frame_ms : SR_LOWER_CREDIT_CAP_MS;
			/* The window stays a FAIL, but the diagnostic has to let a console
			 * reader tell a frame hitch from a recovery defect, so it carries
			 * the measured delta AND the worst frame gap seen inside the
			 * window. A 5730 ms grace with max_frame_ms=640 is one long frame;
			 * the same 5730 ms with max_frame_ms=50 is the machine. */
			/* Same one-frame measurement resolution on the lower bound as the
			 * transition check below (see there). */
			if(grace + grace_credit + 1u
				   < (uint32_t)(SPLIT_LAYOUT_GRACE_SECONDS * 1000.0F)
			   || grace > (uint32_t)(SPLIT_LAYOUT_GRACE_SECONDS * 1000.0F)
					   + SR_FRAME_TOLERANCE_MS) {
				char diag[96];
				snprintf(diag, sizeof(diag),
						 "collapse began at %u ms, max_frame_ms=%u, views=%u",
						 grace, s.step_frame_ms, ctx->view_count);
				sr_fail_at("collapse_begin", diag);
				return;
			}
			if(sr_mark("p1.collapse_begin", true,
					   "\"grace_ms\":%u,\"frames\":%u,\"boundary\":%.3f", grace,
					   s.frames_at_collapse, ctx->layout->boundary))
				sr_step(4);
			return;
		}
		case 4: {
			s.frames_since_loss++;
			if(!s.done_seen) { /* measure ONCE, see done_seen */
				if(!ctx->layout || ctx->layout->boundary < 1.0F)
					return;
				s.done_seen = true;
				s.done_reading_ms = s.now_ms - s.collapse_begin_ms;
				s.done_frame_ms = s.max_frame_ms;
				s.frames_at_done = s.frames_since_loss;
			}
			uint32_t transition = s.done_reading_ms;
			uint32_t transition_credit = s.done_frame_ms < SR_LOWER_CREDIT_CAP_MS
				? s.done_frame_ms : SR_LOWER_CREDIT_CAP_MS;
			/* LOWER bound: the driver samples once per frame and its clock is
			 * truncated to whole ms, so its reading of a 350 ms ease has one
			 * frame of resolution. Real Wii, 878619c_09.17 run 0917_092937:
			 * "collapse took 349 ms, max_frame_ms=17" FAILED a correct
			 * transition by 1 ms (3 earlier runs read 350). The bound is the
			 * nominal time minus the worst frame seen in the window (+1 ms of
			 * truncation): an EARLY collapse by more than one frame still
			 * fails, which is what the bound exists to catch. */
			if(transition + transition_credit + 1u
				   < (uint32_t)(SPLIT_LAYOUT_TRANSITION_SECONDS * 1000.0F)
			   || transition > (uint32_t)(SPLIT_LAYOUT_TRANSITION_SECONDS
										  * 1000.0F)
					   + SR_FRAME_TOLERANCE_MS) {
				char diag[96];
				snprintf(diag, sizeof(diag),
						 "collapse took %u ms, max_frame_ms=%u", transition,
						 s.done_frame_ms);
				sr_fail_at("collapse_done", diag);
				return;
			}
			if(sr_mark("p1.collapse_done", true,
					   "\"transition_ms\":%u,\"frames\":%u,\"boundary\":%.3f",
					   transition, s.frames_at_done - s.frames_at_collapse,
					   ctx->layout->boundary))
				sr_step(5);
			return;
		}
		case 5: /* one frame boundary so the present frame below is a NEW one */
			s.neutral_frames = 0;
			sr_step(6);
			return;
		case 6: /* present + neutral, and deliberately NO resume */
			if(rec->phase == CONTROLLER_RECOVERY_NONE) {
				sr_fail_at("no_auto_resume", "auto-resumed without input");
				return;
			}
			if(++s.neutral_frames < SR_NEUTRAL_FRAMES)
				return;
			if(rec->phase != CONTROLLER_RECOVERY_READY)
				return; /* still gating; the phase timeout bounds it */
			if(sr_mark("p1.no_auto_resume", true,
					   "\"frames\":%u,\"rec_phase\":%u", s.neutral_frames,
					   (unsigned)rec->phase))
				sr_step(7);
			return;
		case 7: /* ONE deliberate Resume press */
			out->guest_frame.pressed[IB_GUI_CLICK] = true;
			sr_step(8);
			return;
		case 8:
			if(rec->phase != CONTROLLER_RECOVERY_NONE)
				return;
			if(sr_mark("p1.resumed", true, "\"rec_phase\":%u,\"owns_pause\":%s",
					   (unsigned)rec->phase,
					   rec->owns_pause ? "true" : "false"))
				sr_step(9);
			return;
		case 9:
			if(!ctx->layout || fabsf(ctx->layout->boundary - 0.5F) > 0.01F)
				return;
			if(sr_mark("p1.view_restored", true,
					   "\"boundary\":%.3f,\"restore_ms\":%u",
					   ctx->layout->boundary, s.now_ms - s.collapse_begin_ms))
				sr_step(10);
			return;
		case 10: {
			uint32_t used = 0;
			uint32_t hash = sr_inventory_hash(ctx->guest, &used);
			if(hash != s.base[1].hash) {
				char diag[96];
				snprintf(diag, sizeof(diag), "guest inventory changed: slot %d",
						 sr_first_diff(ctx->guest, sr_raw[1]));
				sr_fail_at("inventory_match", diag);
				return;
			}
			if(sr_mark("p1.inventory_match", true,
					   "\"actor\":1,\"inv_hash\":%u,\"want\":%u", hash,
					   s.base[1].hash))
				sr_enter(SPLIT_RECOVERY_P2);
			return;
		}
		default:
			sr_fail("p1 step");
			return;
	}
}

static void sr_phase_p2(const struct split_recovery_ctx* ctx,
						struct split_recovery_actions* out) {
	out->guest_frame_valid = true;
	sr_frame(&out->guest_frame, true);
	out->suppress_autojoin = s.suppress_join;
	struct local_pause* pause = &ctx->guest->pause;
	switch(s.step) {
		case 0: /* open the guest's own Pause -- the production menu state */
			out->guest_frame.pressed[IB_HOME] = true;
			sr_step(1);
			return;
		case 1:
			if(!pause->open)
				return;
			sr_step(2);
			return;
		case 2: /* select "Leave game" */
			out->guest_frame.pressed[IB_GUI_DOWN] = true;
			out->guest_frame.pressed[IB_GUI_RIGHT] = true;
			sr_step(3);
			return;
		case 3:
			if(pause->selected != 3) {
				out->guest_frame.pressed[IB_GUI_DOWN] = true;
				out->guest_frame.pressed[IB_GUI_RIGHT] = true;
				return;
			}
			sr_step(4);
			return;
		case 4: /* activate it: local_pause.c's client_session_request_local_leave */
			out->guest_frame.pressed[IB_GUI_CLICK] = true;
			/* Shut the synthetic rejoin BEFORE the press is applied, not after
			 * the request is observed: a server that acked inside the same
			 * frame would otherwise leave a one-frame window in which the
			 * retired guest was immediately re-admitted. */
			s.suppress_join = true;
			out->suppress_autojoin = true;
			sr_step(5);
			return;
		case 5:
			if(pause->exit_failed && !pause->exit_pending) {
				sr_fail_at("leave_requested", "leave enqueue refused");
				return;
			}
			/* The production transition is observable as EITHER the pause
			 * holding a pending exit or the session already in LEAVING -- a
			 * leave acknowledged inside the requesting frame closes the pause
			 * before the next frame can look at it. */
			if(!pause->exit_pending
			   && ctx->guest->local_phase != CLIENT_GUEST_LEAVING)
				return;
			s.leave_gen = ctx->guest->local_generation;
			s.suppress_join = true;
			out->suppress_autojoin = true;
			{	/* corr of a REQUEST marker is its OWN seq, so every later
				 * observation can point back at exactly this request. */
				uint32_t corr = s.seq + 1u;
				if(sr_mark_corr("p2.leave_requested", corr, true,
								"\"gen\":%llu,\"page\":%u,\"selected\":%d",
								(unsigned long long)s.leave_gen,
								(unsigned)pause->page, pause->selected)) {
					s.leave_corr = corr;
					sr_step(6);
				}
			}
			return;
		case 6:
			if(ctx->guest->local_phase != CLIENT_GUEST_IDLE)
				return;
			if(sr_mark_corr("p2.leave_acked", s.leave_corr, true,
							"\"gen\":%llu", (unsigned long long)s.leave_gen))
				sr_step(7);
			return;
		case 7:
			if(ctx->guest->local_gameplay_ready || ctx->guest->local_player)
				return;
			if(sr_mark_corr("p2.guest_retired", s.leave_corr, true,
							"\"ready\":false"))
				sr_step(8);
			return;
		case 8:
			if(ctx->view_count != 1 || !ctx->layout
			   || ctx->layout->boundary < 1.0F)
				return;
			if(sr_mark_corr("p2.host_only_layout", s.leave_corr, true,
							"\"views\":%u,\"boundary\":%.3f", ctx->view_count,
							ctx->layout->boundary))
				sr_step(9);
			return;
		case 9: /* the phase driver, and only it, lets the guest back in */
			s.suppress_join = false;
			out->suppress_autojoin = false;
			if(!sr_guest_admitted(ctx))
				return;
			/* A rejoin is a NEW session: the same generation coming back would
			 * mean the leave never really retired it. */
			if(ctx->guest->local_generation == s.leave_gen) {
				sr_fail_at("rejoined", "rejoin reused the retired generation");
				return;
			}
			if(sr_mark("p2.rejoined", true, "\"gen\":%llu,\"entity\":%u",
					   (unsigned long long)ctx->guest->local_generation,
					   ctx->guest->local_player->id))
				sr_step(10);
			return;
		case 10: {
			uint32_t used = 0;
			uint32_t hash = sr_inventory_hash(ctx->guest, &used);
			if(hash != s.base[1].hash) {
				char diag[96];
				snprintf(diag, sizeof(diag), "guest inventory changed: slot %d",
						 sr_first_diff(ctx->guest, sr_raw[1]));
				sr_fail_at("inventory_match", diag);
				return;
			}
			if(sr_mark("p2.inventory_match", true,
					   "\"actor\":1,\"inv_hash\":%u,\"want\":%u", hash,
					   s.base[1].hash))
				sr_enter(SPLIT_RECOVERY_P3);
			return;
		}
		default:
			sr_fail("p2 step");
			return;
	}
}

static bool sr_settled(const struct split_recovery_ctx* ctx) {
	struct client_session* who[2] = {ctx->host, ctx->guest};
	bool still = true;
	for(unsigned p = 0; p < 2; p++) {
		float x = who[p] ? who[p]->camera.x : 0;
		float y = who[p] ? who[p]->camera.y : 0;
		float z = who[p] ? who[p]->camera.z : 0;
		if(fabsf(x - s.settle_x[p]) > SR_STILL_EPS
		   || fabsf(y - s.settle_y[p]) > SR_STILL_EPS
		   || fabsf(z - s.settle_z[p]) > SR_STILL_EPS)
			still = false;
		s.settle_x[p] = x;
		s.settle_y[p] = y;
		s.settle_z[p] = z;
	}
	if(!still) {
		s.settle_frames = 0;
		return false;
	}
	return ++s.settle_frames >= SR_SETTLE_FRAMES;
}

static void sr_phase_p3(const struct split_recovery_ctx* ctx,
						struct split_recovery_actions* out) {
	out->guest_frame_valid = true;
	sr_frame(&out->guest_frame, true);
	out->host_frame_valid = true;
	sr_frame(&out->host_frame, true);
	out->suppress_autojoin = s.suppress_join;
	struct local_pause* pause = &ctx->host->pause;
	switch(s.step) {
		case 0:
			if(!sr_settled(ctx))
				return;
			sr_record_baseline(ctx->host, &s.presave[0], sr_raw_presave[0]);
			if(!s.presave[0].used) {
				sr_fail("baseline inventory empty: actor 0");
				return;
			}
			if(sr_mark("p3.pre_save_baseline0", true,
					   "\"actor\":0,\"inv_hash\":%u,\"inv_used\":%u,\"x\":%.2f,"
					   "\"y\":%.2f,\"z\":%.2f",
					   s.presave[0].hash, s.presave[0].used, s.presave[0].x,
					   s.presave[0].y, s.presave[0].z))
				sr_step(1);
			return;
		case 1:
			sr_record_baseline(ctx->guest, &s.presave[1], sr_raw_presave[1]);
			if(!s.presave[1].used) {
				sr_fail("baseline inventory empty: actor 1");
				return;
			}
			if(sr_mark("p3.pre_save_baseline1", true,
					   "\"actor\":1,\"inv_hash\":%u,\"inv_used\":%u,\"x\":%.2f,"
					   "\"y\":%.2f,\"z\":%.2f",
					   s.presave[1].hash, s.presave[1].used, s.presave[1].x,
					   s.presave[1].y, s.presave[1].z))
				sr_step(2);
			return;
		case 2:
			out->host_frame.pressed[IB_HOME] = true;
			sr_step(3);
			return;
		case 3:
			if(!pause->open)
				return;
			sr_step(4);
			return;
		case 4:
			out->host_frame.pressed[IB_GUI_DOWN] = true;
			out->host_frame.pressed[IB_GUI_RIGHT] = true;
			sr_step(5);
			return;
		case 5:
			if(pause->selected != 3) {
				out->host_frame.pressed[IB_GUI_DOWN] = true;
				out->host_frame.pressed[IB_GUI_RIGHT] = true;
				return;
			}
			sr_step(6);
			return;
		case 6: /* -> LOCAL_PAUSE_HOST_CONFIRM */
			out->host_frame.pressed[IB_GUI_CLICK] = true;
			sr_step(7);
			return;
		case 7:
			if(pause->page != LOCAL_PAUSE_HOST_CONFIRM)
				return;
			sr_step(8);
			return;
		case 8: /* "Confirm save and quit" is row 1 on the confirm page */
			out->host_frame.pressed[IB_GUI_DOWN] = true;
			sr_step(9);
			return;
		case 9:
			if(pause->selected != 1)
				return;
			sr_step(10);
			return;
		case 10: /* LOCAL_PAUSE_HOST_EXIT -> main.c enqueues SRPC_UNLOAD_WORLD */
			out->host_frame.pressed[IB_GUI_CLICK] = true;
			sr_step(11);
			return;
		case 11:
			if(pause->exit_failed && !pause->exit_pending) {
				sr_fail_at("exit_requested", "unload enqueue refused");
				return;
			}
			if(!pause->exit_pending)
				return;
			s.exit_epoch = ctx->epoch;
			s.suppress_join = true;
			out->suppress_autojoin = true;
			{	/* its OWN seq, like the leave request above */
				uint32_t corr = s.seq + 1u;
				if(sr_mark_corr("p3.exit_requested", corr, true,
								"\"epoch\":%u,\"page\":%u", s.exit_epoch,
								(unsigned)pause->page)) {
					s.exit_corr = corr;
					sr_step(12);
				}
			}
			return;
		case 12:
			/* RETURN_INGAME: the pause reopens with the server's own toast. */
			if(!pause->exit_pending && pause->exit_failed) {
				char diag[96];
				snprintf(diag, sizeof(diag), "%s",
						 ctx->host->hud_message[0] ? ctx->host->hud_message :
													 "Save failed: no toast");
				sr_fail_at("world_reset", diag);
				return;
			}
			/* The title may not appear before the reset that causes it. The
			 * test is "still pending", not "reset not yet RECORDED": the
			 * production CRPC_WORLD_RESET handler settles the pause and
			 * selects the title in the same step, so the two are legitimately
			 * observable on the same frame. A title while the exit is STILL
			 * pending is the real defect -- nothing settled it. */
			if(ctx->at_title && pause->exit_pending) {
				sr_fail("title before WORLD_RESET(0)");
				return;
			}
			/* local_pause_host_exit_settled(true) has exactly one production
			 * caller: client_interface.c's CRPC_WORLD_RESET with
			 * local_entity 0. An accepted settle IS that observation. */
			if(pause->open || pause->exit_pending || pause->exit_failed
			   || ctx->world_loaded)
				return;
			s.reset_seen = true;
			if(sr_mark_corr("p3.world_reset", s.exit_corr, true, "\"epoch\":%u",
							s.exit_epoch))
				sr_step(13);
			return;
		case 13:
			if(!ctx->at_title)
				return;
			if(sr_mark_corr("p3.title_entered", s.exit_corr, true,
							"\"after_reset\":true"))
				sr_enter(SPLIT_RECOVERY_P4);
			return;
		default:
			sr_fail("p3 step");
			return;
	}
}

static void sr_phase_p4(const struct split_recovery_ctx* ctx,
						struct split_recovery_actions* out) {
	out->suppress_autojoin = s.suppress_join;
	switch(s.step) {
		case 0:
			/* ONE-SHOT, like the world create in P0: a refused marker emit
			 * brings this step back next frame, and arming the named-autoload
			 * hook twice would re-enter screen_select_world under a world that
			 * is already loading. */
			if(!s.autoload_asked) {
				if(!s.hooks.arm_autoload
				   || !s.hooks.arm_autoload(s.hooks.user,
											SPLIT_RECOVERY_RIG_WORLD)) {
					sr_fail_at("autoload_armed", "autoload arm refused");
					return;
				}
				s.autoload_asked = true;
			}
			if(sr_mark("p4.autoload_armed", true, "\"world\":\"%s\"",
					   SPLIT_RECOVERY_RIG_WORLD))
				sr_step(1);
			return;
		case 1:
			if(!ctx->world_loaded || ctx->world_switch_pending
			   || !ctx->host_body)
				return;
			/* A reload is a new world generation: the same input epoch would
			 * mean the teardown never happened. */
			if(ctx->epoch == s.exit_epoch) {
				sr_fail_at("world_loaded", "reload reused the pre-save epoch");
				return;
			}
			if(sr_mark("p4.world_loaded", true,
					   "\"epoch\":%u,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f",
					   ctx->epoch, ctx->host->camera.x, ctx->host->camera.y,
					   ctx->host->camera.z))
				sr_step(2);
			return;
		case 2: { /* BEFORE the guest is let back in, so nothing can overwrite */
			uint32_t used = 0;
			uint32_t hash = sr_inventory_hash(ctx->host, &used);
			float dx = ctx->host->camera.x - s.presave[0].x;
			float dy = ctx->host->camera.y - s.presave[0].y;
			float dz = ctx->host->camera.z - s.presave[0].z;
			if(hash != s.presave[0].hash) {
				char diag[96];
				snprintf(diag, sizeof(diag), "host inventory changed: slot %d",
						 sr_first_diff(ctx->host, sr_raw_presave[0]));
				sr_fail_at("host_verified", diag);
				return;
			}
			if(fabsf(dx) > SR_STILL_EPS || fabsf(dz) > SR_STILL_EPS
			   || fabsf(dy) > 0.5F) {
				char diag[96];
				snprintf(diag, sizeof(diag),
						 "host moved dx=%.2f dy=%.2f dz=%.2f", dx, dy, dz);
				sr_fail_at("host_verified", diag);
				return;
			}
			if(sr_mark("p4.host_verified", true,
					   "\"inv_hash\":%u,\"want\":%u,\"dx\":%.2f,\"dy\":%.2f,"
					   "\"dz\":%.2f,\"tol_xz\":%.2f,\"tol_y\":%.2f",
					   hash, s.presave[0].hash, dx, dy, dz, SR_STILL_EPS, 0.5F))
				sr_step(3);
			return;
		}
		case 3: {
			/* The host baseline is already verified. Readmit the idle guest
			 * now so its saved-position anchor can stream the remote placed
			 * blocks. Waiting for those blocks before allowing the anchor
			 * deadlocked R8 after the two 40-block walks. Marker order stays
			 * unchanged: case 4 observes admission after all block checks. */
			s.suppress_join = false;
			out->suppress_autojoin = false;
			struct sr_block* b = &s.blocks[s.verified_blocks];
			uint32_t meta = 0;
			uint32_t id = sr_block_id(ctx, b->x, b->y, b->z, &meta);
			if(!id)
				return; /* the column may still be streaming; phase timeout bounds it */
			/* ONE world-rule transition is legitimate between placement and
			 * reload: a placed DIRT block may have become GRASS. Beta
			 * BlockGrass.updateTick (decompile lines 38-45): a grass block
			 * under light >= 9 turns a random dirt block within x+-1, y-3..+1,
			 * z+-1 into grass when the cell above it is lit >= 4. The placed
			 * blocks sit one above a grass surface, inside that range (real PC
			 * app, 2026-09-17: "block 1 lost: got 2/0" on 1 run of ~8). The
			 * simulation is right and stays byte-faithful; the assertion was
			 * wrong. Anything else -- air, another id -- is still a LOST block. */
			/* Astra gate 3 (2026-09-17): the exception is EXACTLY dirt/0 ->
			 * grass/0. Grass carries no metadata in Beta, so a grass block
			 * with any other metadata is corrupt persistence, not spread. This
			 * proves compatible persistence, not byte-identical block ids. */
			bool grass_spread = b->id == BLOCK_DIRT && b->meta == 0
				&& id == BLOCK_GRASS && meta == 0;
			if((id != b->id || meta != b->meta) && !grass_spread) {
				char diag[96];
				snprintf(diag, sizeof(diag), "block %u lost: got %u/%u",
						 s.verified_blocks, id, meta);
				sr_fail_at("blocks_verified", diag);
				return;
			}
			char key[32];
			snprintf(key, sizeof(key), "p4.blocks_verified%u",
					 s.verified_blocks);
			if(!sr_mark(key, true,
						"\"i\":%u,\"x\":%d,\"y\":%d,\"z\":%d,\"id\":%u,"
						"\"meta\":%u",
						s.verified_blocks, b->x, b->y, b->z, id, meta))
				return;
			if(++s.verified_blocks >= SPLIT_RECOVERY_BLOCKS)
				sr_step(4);
			return;
		}
		case 4:
			s.suppress_join = false;
			out->suppress_autojoin = false;
			if(!sr_guest_admitted(ctx))
				return;
			if(sr_mark("p4.guest_readmitted", true,
					   "\"gen\":%llu,\"entity\":%u",
					   (unsigned long long)ctx->guest->local_generation,
					   ctx->guest->local_player->id))
				sr_step(5);
			return;
		case 5: {
			uint32_t used = 0;
			uint32_t hash = sr_inventory_hash(ctx->guest, &used);
			if(hash != s.base[1].hash) {
				char diag[96];
				snprintf(diag, sizeof(diag), "guest inventory changed: slot %d",
						 sr_first_diff(ctx->guest, sr_raw[1]));
				sr_fail_at("guest_verified", diag);
				return;
			}
			if(sr_mark("p4.guest_verified", true,
					   "\"inv_hash\":%u,\"want\":%u", hash, s.base[1].hash))
				sr_step(6);
			return;
		}
		case 6: {
			/* sr_phase_name() reads s.phase and run.ok belongs to the run
			 * phase -- but the phase may only MOVE to OK once the line has
			 * actually gone out, or a refused emit would end the run with no
			 * marker and no verdict. run.ok is the last marker of a run. */
			enum split_recovery_phase was = s.phase;
			s.phase = SPLIT_RECOVERY_OK;
			if(!sr_mark("run.ok", true, "\"phases\":5")) {
				s.phase = was;
				return;
			}
			if(!s.done_sent && s.hooks.profile_done) {
				s.done_sent = true;
				s.hooks.profile_done(s.hooks.user, "ok");
			}
			return;
		}
		default:
			sr_fail("p4 step");
			return;
	}
}

/* ---- the per-frame entry point ------------------------------------------ */

void split_recovery_frame(const struct split_recovery_ctx* ctx,
						  struct split_recovery_actions* out) {
	if(!out)
		return;
	memset(out, 0, sizeof(*out));
	if(!s.armed || !ctx || !ctx->host || !ctx->guest)
		return;
	out->host_walk_valid = true;
	/* The scripted guest has NO device, and to the production recovery handler
	 * a NULL frame IS an absent controller. Real PC app trace, 2026-09-17: the
	 * guest's recovery machine went to phase 1 on the frame after admission
	 * (its viewport is under 240 px during the opening transition, so
	 * local_menu_host_recovery_frame runs with the NULL frame), the layout
	 * counted the guest lost through all of P0 and had ALREADY collapsed when
	 * P1 injected its loss ("collapse began at 46 ms"). So while armed the
	 * driver owns the guest's device: PRESENT and neutral by default, and only
	 * P1's loss window says otherwise. Phases overwrite this frame freely.
	 * (Set further down, AFTER the run-over return: once the run is OK or
	 * FAILED the synthetic device is withdrawn -- delta review 2026-09-17.) */
	if(!s.clock_set) {
		s.clock_set = true;
		s.base_ms = ctx->now_ms;
	}
	uint32_t previous_ms = s.now_ms;
	s.now_ms = ctx->now_ms - s.base_ms; /* unsigned: wrap-safe */
	/* The worst frame gap inside whatever window is being timed, so a timing
	 * FAIL can say whether one long frame or the machine caused it (M7). */
	if(s.clock_set && s.now_ms > previous_ms
	   && s.now_ms - previous_ms > s.max_frame_ms)
		s.max_frame_ms = s.now_ms - previous_ms;
	if(s.phase == SPLIT_RECOVERY_OK || s.phase == SPLIT_RECOVERY_FAILED) {
		/* A finished run holds the guest gate where the last phase left it and
		 * stops driving anything. */
		out->suppress_autojoin = s.suppress_join;
		return;
	}
	out->guest_frame_valid = true;
	sr_frame(&out->guest_frame, true);
	if(s.now_ms >= SPLIT_RECOVERY_TOTAL_MS) {
		s.phase = SPLIT_RECOVERY_IDLE; /* the marker names the run, not a phase */
		sr_fail_at("timeout", "total timeout"); /* -> run.timeout */
		return;
	}
	/* No "armed" marker: the run phase publishes exactly two, run.ok and
	 * run.timeout, so seq 1 is always p0.guest_admitted. */
	if(s.phase == SPLIT_RECOVERY_IDLE)
		sr_enter(SPLIT_RECOVERY_P0);
	if(s.now_ms - s.phase_start_ms >= sr_phase_budget()) {
		/* A marker still sitting in the retry slot when the budget runs out is
		 * a TRANSPORT failure, not a missing observation. Name it that way and
		 * publish it on the marker that could not go out, so the collector
		 * reads "the ring refused this line" rather than "this never
		 * happened". */
		if(s.pend_valid) {
			const char* dot = strchr(s.pend_key, '.');
			char awaited[sizeof(s.pend_key)];
			snprintf(awaited, sizeof(awaited), "%s", dot ? dot + 1 : s.pend_key);
			size_t n = strlen(awaited);
			while(n > 0 && awaited[n - 1] >= '0' && awaited[n - 1] <= '9')
				awaited[--n] = 0;
			sr_fail_at(awaited, "marker emit blocked");
		} else
			sr_fail("phase timeout");
		return;
	}
	s.frames_in_step++;
	switch(s.phase) {
		case SPLIT_RECOVERY_P0: sr_phase_p0(ctx, out); break;
		case SPLIT_RECOVERY_P1: sr_phase_p1(ctx, out); break;
		case SPLIT_RECOVERY_P2: sr_phase_p2(ctx, out); break;
		case SPLIT_RECOVERY_P3: sr_phase_p3(ctx, out); break;
		case SPLIT_RECOVERY_P4: sr_phase_p4(ctx, out); break;
		default: break;
	}
}
