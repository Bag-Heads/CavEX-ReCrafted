#include "../settle_observer.h"
settle_callback client_settle_observe;
#include "../tps_observer.h"
tps_callback client_tps_observe;

/*
	Copyright (c) 2022 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "client_interface.h"
#include "../platform/watchdog.h"
#include "../platform/g9_worst_tick.h"
#include "../platform/service_observer.h"
#include "../platform/mem2.h" /* REL-MP-FIX3d: also on PC (plain heap) */
#include "../lighting.h"
#include "server_remote.h" /* SERVER_REMOTE_ADMIT_RADIUS_MAX (relight slots) */
#include "../platform/time.h"
#include "server_world.h"
#include "vehicle_smoke_probe.h" /* host_vehicle_smoke_8m route witness */
#include "../column_observer.h"
#include "../column_view.h"
#include "../chunk_mesher.h"
#include "map_data.h" /* MAP_DATA_COLORS_SIZE pin for the map slot */
#include "../item/items/item_map.h"
#include "mp_entity_flags.h" // animation-parity pass: mp_wire_armor_slot_to_type
#include "../block/blocks.h" // blocks[] registry (remote_base_entity_snap)
#include "../block/blocks_data.h"
#include "../block/piston_mobility.h" /* mp-effects: client piston playBlock */
#include "../block/redstone_piston.h"
#include "../entity/entity_boat.h"
#include "../entity/entity_falling_sand.h"
#include "../entity/entity_minecart.h"
#include "../entity/entity_painting.h"
#include "../entity/entity_primed_tnt.h"
#include "../game/client_world_context.h"
#include "../game/game_state.h"
#include "../game/gui/chat_hud.h"
#include "../game/gui/loading_tips.h"
#include "../game/sign_text_store.h"
#include "../particle.h"
#include "../graphics/texture_atlas.h"
#include "../graphics/render_lightning.h" // CRPC_LIGHTNING_BOLT
#include "../platform/audio.h"
#include "../platform/audio_music.h"
#include "../platform/input.h"
#include "../platform/input_owner.h"
#include "../platform/thread.h"
#include "../platform/g9_probe.h"
#include "../platform/cw_canary.h"
#include "../util.h" // rand_gen_flt on audio_cosmetic_rng (MP hit drr)
#include "server_interface.h"
#include "client_world_payload.h"
#include "client_admission.h" /* b2-a2t-cap: A2-T admission client half */

#include <assert.h>
#include <math.h>
#include <stdio.h> // printf for the OOM safety-net logging
#include <stdlib.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define RPC_INBOX_SIZE 16

/* [740c583 RenderGlobal.java:1396] SFX 2000 emits ten directed smoke
 * particles. Called on the render thread, including by the parity test. */
void clin_dispenser_smoke(vec3 center, int direction) {
	static struct random_gen cosmetic_rng = {.seed = 0x2000u};
	int dx = direction % 3 - 1;
	int dz = direction / 3 % 3 - 1;
	uint8_t tex = tex_atlas_lookup_particle(TEXAT_PARTICLE_SMOKE_7);
	for(int i = 0; i < 10; i++) {
		double pace = (double)rand_gen(&cosmetic_rng) / 4294967296.0 * 0.2 + 0.01;
		double px = (double)center[0] + (double)dx * 0.01
			+ ((double)rand_gen(&cosmetic_rng) / 4294967296.0 - 0.5)
				* (double)dz * 0.5;
		double py = (double)center[1]
			+ ((double)rand_gen(&cosmetic_rng) / 4294967296.0 - 0.5) * 0.5;
		double pz = (double)center[2] + (double)dz * 0.01
			+ ((double)rand_gen(&cosmetic_rng) / 4294967296.0 - 0.5)
				* (double)dx * 0.5;
		vec3 pos = {(float)px, (float)py, (float)pz};
		vec3 vel = {
			(float)((double)dx * pace + (double)rand_gen_gaussian(&cosmetic_rng) * 0.01),
			(float)(-0.03 + (double)rand_gen_gaussian(&cosmetic_rng) * 0.01),
			(float)((double)dz * pace + (double)rand_gen_gaussian(&cosmetic_rng) * 0.01),
		};
		particle_add(pos, vel, tex, 0.15f, 20.0f, false,
			180, 180, 180, false, TEXTURE_ATLAS_PARTICLES);
	}
}
/* S5.3 hunt PROBE (temporary, ROUND 6): guard canaries bracketing the
 * message pool + the two tchannels the round-3/4/5 hunt already fingered as
 * the ONLY producer/consumer pair a remote MP session collapses onto the
 * SAME (main) thread for the first time ever (clin_force_world_reset_login's
 * header comment). Painted once in clin_init() right after both channels
 * init successfully; scanned via clin_canary_scan() (declared in
 * client_interface.h) from main.c's per-marker checkpoint, own small budget,
 * log ONLY on clobber -- see cw_canary.h. */
#ifdef PLATFORM_WII
struct cw_canary clin_channel_canary_lead;
#endif
static struct client_rpc rpc_msg[RPC_INBOX_SIZE];
/* order 2: the pending output ring (defined beside clin_rpc_send_tagged) */
static void clin_rpc_pending_reset(void);
static void clin_rpc_release_pooled(struct client_rpc* rpc);
static bool clin_rpc_cancelled(void);
/* Identity is published and recycled with its token, never borrowed from an actor. */
static uint64_t rpc_guest_generation[RPC_INBOX_SIZE];
static struct thread_channel clin_inbox;
static struct thread_channel clin_empty_msg;

/* G9 R21 consumer half of the local-only block-update packing (the server
 * half and the contract: clin_rpc_send_tagged / "PACKED" below). Every
 * inbox reader goes through clin_inbox_take / clin_inbox_token_return. A
 * CRPC_SET_BLOCK_PACK token is kept while its records are replayed, one per
 * take and in send order, INTO that same slot as the CRPC_SET_BLOCK
 * envelope each was sent as -- so routing (HOLD included), the slot-indexed
 * generation tag, clin_process_queued and every observer see exactly the
 * unpacked stream. The token returns after the last record. Main thread. */
static void clin_rpc_unpack_record(const struct client_rpc* pack, unsigned i,
								   struct client_rpc* out);
static struct {
	struct client_rpc* slot; /* NULL = no pack in replay */
	struct client_rpc pack;  /* the delivered pack, copied out of the slot */
	unsigned next;           /* next record to replay */
} s_unpack;
static bool clin_inbox_take(void** out) {
	if(s_unpack.slot && s_unpack.next < s_unpack.pack.payload.set_block_pack.n) {
		clin_rpc_unpack_record(&s_unpack.pack, s_unpack.next++, s_unpack.slot);
		*out = s_unpack.slot;
		return true;
	}
	if(!tchannel_receive(&clin_inbox, out, false))
		return false;
	struct client_rpc* call = *out;
	if(call->type == CRPC_SET_BLOCK_PACK) {
		assert(!s_unpack.slot && call->payload.set_block_pack.n >= 2
			   && call->payload.set_block_pack.n <= CRPC_SET_BLOCK_PACK_MAX);
		s_unpack.pack = *call;
		s_unpack.slot = call;
		s_unpack.next = 0;
		clin_rpc_unpack_record(&s_unpack.pack, s_unpack.next++, call);
	}
	return true;
}
static void clin_inbox_token_return(struct client_rpc* call) {
	if(call == s_unpack.slot) {
		if(s_unpack.next < s_unpack.pack.payload.set_block_pack.n)
			return; /* more records ride this token */
		s_unpack.slot = NULL;
	}
	g9_probe_inbox((unsigned)(call - rpc_msg), false);
	tchannel_send(&clin_empty_msg, call, true);
}
#ifdef PLATFORM_WII
struct cw_canary clin_channel_canary_trail;
static unsigned cw_clin_canary_budget_lead = 20, cw_clin_canary_budget_trail = 20;

/* S5.3 hunt PROBE (temporary, ROUND 9): coordinator's fine-grained bracket
 * of clin_process's CRPC_WORLD_RESET case -- the world teardown/reinstall
 * seam MP_EVENT_LOGIN's handler drives (clin_force_world_reset_login ->
 * clin_process here). Never yet observed firing in any take (state has
 * never reached the point of consuming a decoded LOGIN reply), but if
 * round 9's wider event-drain budget (server_remote_protocol.c's
 * cw_dispatch_trace) proves it DOES fire, these five checkpoints localize
 * within it: teardown start, chunk-store/GX free (world_unload_all owns
 * both -- per-chunk display lists are chunk-store state), local window/
 * entity-dict reinit, and spawn apply (local_player installed + ready).
 * Main-thread-only (clin_process's own established constraint), own file
 * (cw_boot_trace.txt, already main-exclusive), own budget so a genuine
 * multi-attempt reset can't flood the SD. */
static void cw_reset_trace_budgeted(unsigned* budget, const char* label) {
	if(*budget == 0 || CW_SD_TRACE_OFF())
		return;
	(*budget)--;
	FILE* f = fopen("sd:/cw_boot_trace.txt", "a");
	if(!f)
		return;
	fprintf(f, "world_reset_%s\n", label);
	fflush(f);
	fclose(f);
}
static void cw_world_reset_trace(const char* label) {
	/* The five LOCALIZING markers below (teardown_start, pre/post_chunk_gx_free,
	 * pre_window_entity_reinit, spawn_apply_done). 20 / 5 = 4 complete world
	 * resets. */
	static unsigned budget = 20;
	cw_reset_trace_budgeted(&budget, label);
}
/* The settle observer's retire-in-place cost, reported OFF the reset path.
 *
 * Round 2 bracketed the retire with two of these writes. That was backwards:
 * cw_reset_trace_budgeted is a synchronous fopen/fprintf/fflush/fclose to
 * sd:/ on the main thread, so the bracket made libfat -- not the ~1 ms memset
 * it was measuring -- the worst case of the path, and a stall inside the end
 * marker's own SD write would have read as a stall inside the retire.
 *
 * The observer records the memset's count and duration in memory instead
 * (settle_observer_reset_retirements/_last_reset_us), and this writes ONE line
 * AFTER clin_process has finished the whole world reset. Nothing is written
 * when the observer is dormant or nothing retired, so a build without
 * --cw-tps-cpu spends no marker at all. PARTITIONED budget: 4 lines = 4 world
 * resets, and the five localizing markers above keep all 20 of theirs. */
static void cw_settle_retire_flush(void) {
	static unsigned budget = 4;
	static uint32_t reported;
	uint32_t count = settle_observer_reset_retirements();
	if(count == reported)
		return;
	reported = count;
	char label[64];
	snprintf(label, sizeof(label), "settle_retire_in_place n=%u us=%u",
			 (unsigned)count, (unsigned)settle_observer_last_reset_us());
	cw_reset_trace_budgeted(&budget, label);
}
#endif
/* Normally impossible to retain because server_interface.c owns a dedicated
 * ACK reserve. Keep a retry latch anyway: an ownership boundary must never be
 * silently lost if an invariant is violated under a pathological backlog. */
static uint32_t s_input_ack_retry_epoch = 0;
/* Installed primary epoch awaiting a view the player can use. This belongs to
 * local single-player admission, never the guest's window handshake or a
 * remote server's simulation. Cleared at every existing handoff reset. */
static uint32_t s_input_presentation_epoch = 0;
/* A successfully queued ACK may still sit behind 64 old actions. Keep the
 * input-less loading screen until it leaves the client pending ring; only then
 * can every fresh action be admitted behind the ACK without overflowing the
 * ordinary-action budget. */
static uint32_t s_input_ack_wait_epoch = 0;
static uint32_t s_input_nack_retry_epoch = 0;
/* Last nonzero player/input snapshot fully installed by this process. Epochs
 * are serial numbers: half-range comparison admits wrap while rejecting stale
 * or duplicate FIFO data before it can mutate any client owner. */
static uint32_t s_last_input_epoch = 0;
struct clin_transfer_hold_state {
	uint32_t active_id, completed_id;
	uint64_t generation;
	bool active, restore_end_pending;
	uint8_t restore_radius;
};
static struct clin_transfer_hold_state s_transfer_hold[2];
static void clin_transfer_hold_release_restored(void);
/* One host presentation episode serves both the pane policy and the guest's
 * movement sender. Update after the RPC drain: BEGIN+END may arrive together. */
static bool s_host_transfer_pane;
static uint32_t s_host_transfer_request_seen;

bool clin_host_transfer_pane_active(void) { return s_host_transfer_pane; }

static void clin_host_transfer_pane_update(void) {
	uint32_t request=clin_transfer_actor_request_id(0);
	if(!request) {
		s_host_transfer_pane=false;
		s_host_transfer_request_seen=0;
	} else if(request!=s_host_transfer_request_seen) {
		s_host_transfer_request_seen=request;
		s_host_transfer_pane=true;
	} else if(!clin_transfer_actor_held(0)&&gstate.world_loaded
		&& !gstate.world_switch_pending
		&& (gstate.players[0].current_screen==&screen_ingame
			||gstate.players[0].current_screen==&screen_death)
		&& gstate.players[0].local_player)
		s_host_transfer_pane=false;
}

bool clin_transfer_actor_held(unsigned actor_slot) {
	return actor_slot < 2 && s_transfer_hold[actor_slot].active;
}

uint32_t clin_transfer_actor_request_id(unsigned actor_slot) {
	if(actor_slot==0)return s_transfer_hold[0].active_id;
	if(actor_slot!=1)return 0;
	const struct client_session* guest=&gstate.players[1];
	return guest->local_bound
		&&s_transfer_hold[1].generation==guest->local_generation
		? s_transfer_hold[1].active_id : 0;
}

static bool clin_transfer_id_newer(uint32_t a, uint32_t b) {
	return a != b && (uint32_t)(a - b) < UINT32_C(0x80000000);
}

/* ci-1101/56: Beta Minecraft.usePortal (1200-1235) names the target
 * dimension before loading. Presentation only: the existing host/guest
 * readiness and rollback barriers still decide when gameplay resumes. */
static void clin_portal_loading_begin(unsigned owner, int dimension) {
	enum loading_reason reason = dimension == WORLD_DIM_NETHER
		? PORTAL_IN : PORTAL_OUT;
	if(loading_reason_get(owner) != reason)
		loading_reason_begin(owner, reason);
}

static void clin_portal_loading_end(unsigned owner) {
	enum loading_reason reason = loading_reason_get(owner);
	if(reason == PORTAL_IN || reason == PORTAL_OUT)
		loading_reason_end(owner);
}

static void clin_portal_loading_update(void) {
	/* Guest panes have no screen_set exit. Retire only their portal reason
	 * at the same existing ready/cancel boundary as the pane policy. */
	const struct client_session* portal_guest = &gstate.players[1];
	if(!portal_guest->local_bound || portal_guest->local_reply_failed
	   || portal_guest->local_phase != CLIENT_GUEST_ATTACHED
	   || (!clin_transfer_actor_held(1) && portal_guest->local_gameplay_ready
		   && !portal_guest->local_respawn_loading && portal_guest->local_player))
		clin_portal_loading_end(1);
	if(!clin_transfer_actor_held(0) && !s_host_transfer_pane
	   && gstate.players[0].current_screen != &screen_load_world)
		clin_portal_loading_end(0);
}

static void clin_transfer_hold_apply(const struct client_rpc* call) {
	const uint8_t slot = call->payload.transfer_hold.actor_slot;
	const uint32_t id = call->payload.transfer_hold.request_id;
	const uint64_t generation = call->payload.transfer_hold.actor_generation;
	if(slot >= 2 || !id || (slot == 0 && generation != 0)
	   || (slot == 1 && (!gstate.players[1].local_bound
					 || generation != gstate.players[1].local_generation)))
		return;
	struct clin_transfer_hold_state* hold = &s_transfer_hold[slot];
	if(slot == 1 && hold->generation != generation) {
		*hold = (struct clin_transfer_hold_state){.generation = generation};
	}
	if(call->payload.transfer_hold.begin) {
		if(hold->active && id == hold->active_id) return;
		if(hold->completed_id && !clin_transfer_id_newer(id, hold->completed_id))
			return;
		if(hold->active && !clin_transfer_id_newer(id, hold->active_id))
			return;
		struct clin_world_payload viewed;
		int dimension = clin_world_payload_of_view(slot, &viewed)
			? viewed.terrain->dimension : gstate.world.dimension;
		clin_portal_loading_begin(slot, dimension == WORLD_DIM_NETHER
			? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER);
		hold->active_id = id;
		hold->active = true;
		hold->restore_end_pending = false;
		hold->restore_radius = 0;
	} else if(hold->active && id == hold->active_id) {
		if(call->payload.transfer_hold.restored_source) {
			hold->restore_end_pending = true;
			hold->restore_radius = call->payload.transfer_hold.restore_radius;
			if(hold->restore_radius < 1 || hold->restore_radius > 4)
				fprintf(stderr, "[client] rollback hold %u has invalid source radius %u\n",
					(unsigned)id, (unsigned)hold->restore_radius);
			clin_transfer_hold_release_restored();
		} else {
			hold->active = false;
			hold->completed_id = id;
		}
	}
}
/* The server freezes world_time until it consumes the exact ACK. Install the
 * marker's tick immediately, but start its wall-clock interpolation only when
 * input/simulation are exposed; otherwise a queued ACK visually advances time
 * behind the loading screen. */
static bool s_input_time_rebase_pending = false;
static void* (*s_clin_alloc)(size_t) = malloc;
// Sticky completion gate for the current CRPC_WORLD_RESET. The server queues
// CRPC_PLAYER_POS after its chunk load even when a client allocation failed;
// that later position must never turn a partial/null client world "loaded".
static bool s_world_reset_ready = false;
/* Split-dims B1 phase 2: the client world contexts (one current context per
 * server world slot, plus retiring predecessors that still hold leases) and
 * the one inbox message held back while its context quiesces. Owner (main)
 * thread only. Zero-initialized = empty; the per-slot epoch marks live in
 * client_world_context.c and survive every reset/reconnect by design. */
static struct client_ctx_registry s_world_ctx;
/* The slot whose context owns the client's single world payload (gstate.world,
 * entities, particles, clock): the one the last WORLD_RESET installed. */
static unsigned s_world_payload_slot;
static struct client_rpc* s_world_held_call;
/* A drain loop routed this exact envelope already (and did not hold it), so
 * clin_process must apply that verdict instead of routing (and counting) it a
 * second time. NULL for every other entry into clin_process. */
static struct client_rpc* s_world_routed_call;
static enum client_ctx_route s_world_routed_verdict;
static struct client_world_context* s_world_routed_ctx;

/* ---- Split-dims B2: world-bound payload STORAGE (client_world_payload.h) --
 * Flag OFF (default): every slot maps to the single gstate storage and the
 * accessors below return exactly &gstate.world / gstate.entities / the
 * primary particle system -- the pre-B2 references, byte for byte. Flag ON:
 * slot 1 maps to s_extra_payload, allocated at enable time. */
static bool s_world_payloads_enabled;
/* Per-world clock + weather (flag ON). Slot 0's lives in gstate (the
 * fields main.c already reads); slot 1's lives in its extra storage. */
struct clin_world_clock {
	uint64_t world_time;
	ptime_t world_time_start;
	bool raining, thundering;
	float rain_strength, thunder_strength;
	float prev_rain_strength, prev_thunder_strength;
};
struct clin_extra_payload {
	struct world terrain;
	dict_entity_t entities;
	struct particle_system* particles;
	struct sign_text_store* signs;
	struct piston_moving_store* pistons;
	struct clin_world_clock clock;
};
static struct clin_extra_payload* s_extra_payload;
/* Flag ON bookkeeping for NON-primary payloads (the primary keeps the
 * pre-B2 stream state: s_world_reset_ready, the recovery phase and the NACK
 * latch). */
static bool s_payload_ready[CLIENT_CTX_SLOT_COUNT];
static uint32_t s_payload_install_failures[CLIENT_CTX_SLOT_COUNT];
/* Per-world "request resend" marker: a NON-primary payload lost a column the
 * server already marked sent. Raised by the failure, cleared only by that
 * slot's next reset/release. The request itself needs a world-addressed
 * recovery SRPC + per-world epoch marker (A/C); until then this is the
 * client half: never ACK/forget the hole, expose it to the owner. */
static bool s_payload_resend_pending[CLIENT_CTX_SLOT_COUNT];
/* B2 gate 2: the slot's world-addressed resend request was accepted by the
 * transport; cleared with the marker. The sender is the transport's. */
static bool s_payload_resend_requested[CLIENT_CTX_SLOT_COUNT];
static clin_world_resend_sender s_world_resend_sender;
/* B2 gates (Fable F4): a context install that fails (the pool full of
 * retiring predecessors still holding leases) leaves the slot with NO
 * context. Both gates record what they were installing and
 * clin_world_resend_poll retries it once per frame until the pool has room.
 * A primary WORLD_RESET into the slot, a release of it or a flag flip drops
 * the record. */
struct clin_payload_reinstall {
	bool armed;
	bool guest; /* gate 1 (ready at once) vs gate 2 (re-request after) */
	bool take; /* gate 1 guest move: take the views from other contexts */
	int dimension;
	uint32_t generation;
	uint8_t views;
};
static struct clin_payload_reinstall s_payload_reinstall[CLIENT_CTX_SLOT_COUNT];
/* A2-T landing (blocker 3): the generation of an UNVIEWED context this client
 * prepared in the slot for an armed landing (0 = none). Cleared when a view
 * binds it, or when the slot is released. */
static uint32_t s_payload_prepared[CLIENT_CTX_SLOT_COUNT];
/* ... and whether the prepare INSTALLED that context (only then does a cancel
 * release it; an existing unviewed world the landing merely reuses stays). */
static bool s_payload_prepared_installed[CLIENT_CTX_SLOT_COUNT];
/* The world-addressed request accepted for a slot, by generation. Unlike
 * s_payload_resend_requested it survives the slot becoming player 0's, so a
 * reply for a request sent before that switch is still recognised as the
 * answer to it (Fable F6); the matching reply clears it. */
static uint32_t s_payload_resend_inflight_gen[CLIENT_CTX_SLOT_COUNT];

/* The slot whose storage the message being applied targets, or -1 outside
 * a dispatch (= the primary: s_world_payload_slot). Set by clin_process
 * around one handler call and restored after it. */
static int s_apply_slot = -1;

static bool clin_storage_is_extra(unsigned slot) {
	return s_world_payloads_enabled && s_extra_payload && slot == 1u;
}

static unsigned clin_apply_slot(void) {
	return s_apply_slot >= 0 ? (unsigned)s_apply_slot : s_world_payload_slot;
}

static struct world* clin_world_of_slot(unsigned slot) {
	return clin_storage_is_extra(slot) ? &s_extra_payload->terrain
									   : &gstate.world;
}

static struct dict_entity_s* clin_entities_of_slot(unsigned slot) {
	return clin_storage_is_extra(slot) ? s_extra_payload->entities
									   : gstate.entities;
}

static struct particle_system* clin_particles_of_slot(unsigned slot) {
	return clin_storage_is_extra(slot) ? s_extra_payload->particles
									   : particle_system_primary();
}

/* Flag ON: free a body from whichever payload store tracks exactly this
 * pointer (a body belongs to the world it was installed into, which need not
 * be the one the current message targets). False when no store tracks it. */
static bool clin_payload_free_body(struct entity* body) {
	uint32_t id = body->id;
	for(unsigned s = 0; s < CLIENT_CTX_SLOT_COUNT; s++) {
		struct dict_entity_s* store = clin_entities_of_slot(s);
		struct entity** tracked = dict_entity_get(store, id);
		if(tracked && *tracked == body) {
			dict_entity_erase(store, id);
			free(body);
			return true;
		}
	}
	return false;
}

bool clin_guest_respawn_handoff(unsigned slot, uint32_t generation,
	int dimension) {
	if(!s_world_payloads_enabled || slot >= CLIENT_CTX_SLOT_COUNT
	   || !generation || !gstate.players[1].local_player
	   || !gstate.players[1].local_respawn_loading
	   || !gstate.players[1].local_respawn_epoch
	   || gstate.players[1].local_respawn_world_slot != slot
	   || gstate.players[1].local_respawn_world_generation != generation
	   || gstate.players[1].local_respawn_dimension != dimension)
		return false;
	struct client_world_context* ctx
		= client_ctx_registry_current(&s_world_ctx, slot);
	struct world* target_world = clin_world_of_slot(slot);
	if(!ctx || ctx->retiring || ctx->server_generation != generation
	   || ctx->dimension != dimension || target_world->dimension != dimension
	   || !clin_world_payload_ready(slot))
		return false;
	struct entity* body = gstate.players[1].local_player;
	int source_slot = -1;
	struct dict_entity_s* primary = clin_entities_of_slot(0);
	for(unsigned i = 0; i < CLIENT_CTX_SLOT_COUNT; i++) {
		struct dict_entity_s* store = clin_entities_of_slot(i);
		if(i && store == primary)
			continue; /* no extra payload: both slot accessors alias one store */
		struct entity** found = dict_entity_get(store, body->id);
		if(found && *found == body) {
			if(source_slot >= 0) return false; /* never move a double-owned body */
			source_slot = (int)i;
		}
	}
	if(source_slot < 0)
		return false;
	if(body->world != clin_world_of_slot((unsigned)source_slot))
		return false;
	struct dict_entity_s* target = clin_entities_of_slot(slot);
	struct entity** target_entry = dict_entity_get(target, body->id);
	if(target_entry && *target_entry != body)
		return false;
	bool inserted = source_slot != (int)slot;
	if(inserted) {
		if(target_entry || !dict_entity_try_reserve_insertions(target, 1)
		   || !(target_entry = dict_entity_safe_get(target, body->id)))
			return false;
		*target_entry = body;
	}
	/* No allocation beyond this point. Bind first, so failure leaves the
	 * original store as the sole owner after removing a new target key. */
	if(!clin_world_payload_bind_view(slot, 1)) {
		if(inserted) dict_entity_erase(target, body->id);
		return false;
	}
	body->world = target_world;
	if(inserted)
		dict_entity_erase(clin_entities_of_slot((unsigned)source_slot), body->id);
	return true;
}

static struct sign_text_store* clin_signs_of_slot(unsigned slot) {
	return clin_storage_is_extra(slot) ? s_extra_payload->signs
									   : sign_text_store_primary();
}

static struct piston_moving_store* clin_pistons_of_slot(unsigned slot) {
	return clin_storage_is_extra(slot) ? s_extra_payload->pistons
									   : client_piston_moving_store_primary();
}

/* Pointers to one world's clock/weather fields: gstate's for the gstate
 * storage (flag OFF: always -- the pre-B2 writes exactly), the extra
 * storage's own copy for slot 1 when the flag is ON. */
struct clin_clock_ref {
	uint64_t* world_time;
	ptime_t* world_time_start;
	bool* raining;
	bool* thundering;
	float* rain_strength;
	float* thunder_strength;
	float* prev_rain_strength;
	float* prev_thunder_strength;
};

static struct clin_clock_ref clin_clock_of_slot(unsigned slot) {
	if(clin_storage_is_extra(slot)) {
		struct clin_world_clock* k = &s_extra_payload->clock;
		return (struct clin_clock_ref) {&k->world_time, &k->world_time_start,
			&k->raining, &k->thundering, &k->rain_strength,
			&k->thunder_strength, &k->prev_rain_strength,
			&k->prev_thunder_strength};
	}
	return (struct clin_clock_ref) {&gstate.world_time,
		&gstate.world_time_start, &gstate.weather_raining,
		&gstate.weather_thundering, &gstate.weather_rain_strength,
		&gstate.weather_thunder_strength, &gstate.weather_prev_rain_strength,
		&gstate.weather_prev_thunder_strength};
}

/* Clear one payload's process-wide-looking stores (moving pistons, sign
 * text) -- flag OFF: exactly client_piston_moving_clear() and
 * sign_text_store_clear() on the one store. */
static void clin_payload_clear_stores(unsigned slot) {
	client_piston_moving_store_clear(clin_pistons_of_slot(slot));
	struct sign_text_store* prev = sign_text_store_select(clin_signs_of_slot(slot));
	sign_text_store_clear();
	sign_text_store_select(prev);
}

static struct world* clin_world(void) {
	return clin_world_of_slot(clin_apply_slot());
}

static struct dict_entity_s* clin_entities(void) {
	return clin_entities_of_slot(clin_apply_slot());
}

static struct particle_system* clin_particles(void) {
	return clin_particles_of_slot(clin_apply_slot());
}

/* Is the message being applied for player 0's world (the primary payload)?
 * Always true with the flag OFF. */
static bool clin_apply_is_primary(void) {
	return !s_world_payloads_enabled
		|| clin_apply_slot() == s_world_payload_slot;
}

#define CW_WORLD (clin_world())
#define CW_ENTITIES (clin_entities())

/* REL-MP-FIX3d: ONE remote relight reservation, made at the first remote
 * login and held until a local world reset. Slots = the remote resident
 * ceiling (2R+3)^2 at the platform's maximum admission radius
 * (server_remote_internal.h REMOTE_RESIDENT_COLUMN_CAP, which this TU may not
 * include): Wii R=5 169 slots = 695,288 B, PC R=10 529 slots
 * (tmp/jobs/triage-1020/fix3c2-ledger.md). MEM2 only (mem2_alloc_strict never
 * spills to MEM1, whose observed headroom ~464 KiB is short); a failed
 * reservation aborts the world load like the other FATAL allocations. */
#define CLIN_REMOTE_RELIGHT_SLOTS \
	((size_t)(2 * SERVER_REMOTE_ADMIT_RADIUS_MAX + 3) \
	 * (size_t)(2 * SERVER_REMOTE_ADMIT_RADIUS_MAX + 3))
static void* s_remote_relight_store;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static bool s_test_relight_alloc_fail;
void clin_test_fail_remote_relight_alloc(bool fail); /* the test declares it */
void clin_test_fail_remote_relight_alloc(bool fail) {
	s_test_relight_alloc_fail = fail;
}
#define CLIN_RELIGHT_ALLOC(n) (s_test_relight_alloc_fail ? NULL : mem2_alloc_strict(n))
#else
#define CLIN_RELIGHT_ALLOC(n) mem2_alloc_strict(n)
#endif
static bool clin_remote_relight_attach(struct world* w) {
	if(!gstate.remote_session) { /* local mode: no reservation, as before */
		world_remote_relight_end(w);
		mem2_free(s_remote_relight_store);
		s_remote_relight_store = NULL;
		return true;
	}
	if(w->remote_relight) /* world_unload_all already dropped its work */
		return true;
	size_t bytes = lighting_remote_relight_bytes(CLIN_REMOTE_RELIGHT_SLOTS);
	if(!s_remote_relight_store)
		s_remote_relight_store = CLIN_RELIGHT_ALLOC(bytes);
	return world_remote_relight_begin(w, s_remote_relight_store, bytes,
									  CLIN_REMOTE_RELIGHT_SLOTS);
}
static const struct world_lease_owner* clin_lease_owner_for(unsigned slot);
/* A CRPC_CHUNK can be consumed even when one of its eight client chunks could
 * not be allocated. Remember that loss until the epoch marker: the server has
 * already marked the column sent, so ACKing would make the hole permanent. */
static bool s_world_stream_install_failed = false;
enum clin_stream_recovery_phase {
	CLIN_STREAM_RECOVERY_NONE = 0,
	CLIN_STREAM_RECOVERY_WAIT_RESET,
	CLIN_STREAM_RECOVERY_STREAMING,
};
/* Stable-world voxel recovery keeps the player/entity/window graph alive. The
 * request is tagged with the last installed input epoch and retried until the
 * ownership-reserved client->server transport accepts it. */
static enum clin_stream_recovery_phase s_stream_recovery_phase
	= CLIN_STREAM_RECOVERY_NONE;
static uint32_t s_stream_recover_epoch = 0;
static uint32_t s_stream_recover_retry_epoch = 0;
static bool s_stream_restore_pause_screen = false;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
bool clin_test_stream_restore_pause_screen(void) { return s_stream_restore_pause_screen; }
#endif
static int s_stream_restore_window = -1;
static enum window_type s_stream_restore_window_type = WINDOW_TYPE_INVENTORY;
/* The container screen implementations own their configured id privately.
 * Remember the exact id whenever this transport exposes an authoritative
 * window so a voxel-only recovery can restore that fixed owner, not merely a
 * same-type guess. Inventory is the known fixed id zero. */
/* A failed authoritative CLOSE asks the client to REOPEN its live container.
 * If that reply races an optimistic HOME pause, rebuild ownership immediately
 * but leave the pause screen in control until a later unpaused ingame frame. */
/* CHIP B (task_18b9ae41, 2026-08-31): the terrain-hold SRPC_WINDOW_CLOSE
 * (CRPC_PLAYER_POS's POSITION_ONLY branch below) used to ignore svin_rpc_
 * send's return and unconditionally clear gstate.players[0].visible_window. svin_rpc_send
 * already retries ordinary backpressure internally (its own SVIN_LIFECYCLE_
 * RESERVE pending queue, replayed by svin_rpc_flush_pending each clin_update
 * -- server_interface.c), so a false return here means that reserve (or the
 * whole pending buffer) is genuinely saturated: the close is truly dropped,
 * with no retry, while the client believes the window is already gone --
 * exactly the FH-07 leaked-container-ownership class. -1 = no retry
 * outstanding; else the window id whose SRPC_WINDOW_CLOSE must be resent by
 * clin_update every frame until svin_rpc_send finally accepts it. */
static int s_pending_window_close_retry = -1;
/* CHIP B, second call site: screen_load_world.c's remote-escape action fires
 * SRPC_UNLOAD_WORLD then unconditionally swaps to screen_multiplayer_connect
 * -- same saturated-queue gap as the window-close above, but a dropped
 * unload here leaves server_remote in REMOTE_PLAY while the client believes
 * it already disconnected. clin_send_unload_world() (below) is the single
 * send site every caller of this retry should go through; this flag just
 * remembers whether the LAST attempt still needs a retry. */
static bool s_pending_unload_world_retry = false;

static void clin_reset_player_state_latch(void) {
	gstate.players[0].player_sync.state_known = false;
	gstate.players[0].player_sync.state_on_ground = false;
	gstate.players[0].player_sync.state_sneaking = false;
	gstate.players[0].player_sync.state_correction_seq = 0;
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static clin_test_rpc_observer s_test_rpc_observer = NULL;
static void* s_test_rpc_observer_user = NULL;
static unsigned s_test_entity_slot_fail_countdown = 0;
static clin_test_rpc_observer s_test_consume_observer;
static void* s_test_consume_user;
void clin_test_set_consume_observer(clin_test_rpc_observer observer, void* user) {
	s_test_consume_observer = observer;
	s_test_consume_user = user;
}

/* Test consumer: copy the envelope/tag and transfer payload ownership to
 * `out`. NULL discards and releases instead. Either way returns the token. */
bool clin_test_inbox_pop(struct client_rpc* out, uint64_t* generation) {
	void* call;
	if(!clin_inbox_take(&call))
		return false;
	struct client_rpc* rpc = call;
	if(out)
		*out = *rpc;
	if(generation)
		*generation = rpc_guest_generation[rpc - rpc_msg];
	/* Transfer every pooled payload with the copy. Returning the envelope
	 * token does not release the payload now owned by the test consumer. */
	if(!out)
		clin_rpc_release_pooled(rpc);
	clin_inbox_token_return(rpc);
	return true;
}
void clin_test_rpc_release(struct client_rpc* call) {
	clin_rpc_release_pooled(call);
}
void clin_test_set_rpc_observer(clin_test_rpc_observer observer, void* user) {
	s_test_rpc_observer = observer;
	s_test_rpc_observer_user = user;
}

/* Split-dims B1 test seam: arm the accepted-recovery phase so a fixture can
 * deliver CRPC_STREAM_RESET without first exhausting column storage. */
void clin_test_arm_stream_reset(void) {
	s_stream_recovery_phase = CLIN_STREAM_RECOVERY_WAIT_RESET;
}

bool clin_test_stream_install_failed(void) {
	return s_world_stream_install_failed;
}

void clin_test_reset_input_epoch_guard(void) {
	s_last_input_epoch = 0;
	gstate.players[0].player_sync.correction_seq = 0;
	clin_reset_player_state_latch();
}
#endif

uint32_t clin_input_epoch(void) {
	return s_last_input_epoch;
}

/* Last CRPC_SPLIT_STATE consumed from the inbox (S-A1 plumbing). Written only
 * by the inbox drain on the main/client thread and read by its accessor on the
 * same thread, exactly like s_last_input_epoch above. */
static struct clin_split_ack s_split_ack;

bool clin_split_last_ack(struct clin_split_ack* out) {
	if(!out || !s_split_ack.valid)
		return false;
	*out = s_split_ack;
	/* HANDED OVER, exactly once (R-A9 A9-H1/H2/M3). The slot used to stay
	 * valid for the rest of the process -- it was written per ack and cleared
	 * only by clin_init, which runs once -- so this accessor published a
	 * LEVEL, not an event. That was harmless while the requester looked at an
	 * ack only in one row state, but R-A8's reconcile deliberately adopts any
	 * current-generation ack from ANY row state, so main.c's per-frame seam
	 * re-fed the SAME ack every frame forever: the deferred config write was
	 * armed and then cancelled one frame later (the toggle was never
	 * persisted), the 5 s no-answer timeout could never age because the adopt
	 * branch returned before it, a settled refusal's caption overwrote every
	 * later row state, and a dotted-JSON config walk plus a full label
	 * reformat ran on every gameplay frame. An ack IS an event -- one server
	 * statement, delivered once -- so the read consumes it and the next read
	 * says "nothing new" until the drain stores the next one. */
	s_split_ack.valid = false;
	return true;
}

void clin_set_alloc_hook(void* (*alloc_fn)(size_t)) {
	s_clin_alloc = alloc_fn ? alloc_fn : malloc;
}

/* M-lib's safe_get has a fatal default OOM policy. Preflight every client
 * entity insertion through the transactional dictionary allocator so the Wii
 * can drop a spawn (or fail a WORLD_RESET closed) instead of aborting before
 * the existing nullable entity-struct checks run. */
static struct entity** clin_entity_slot(uint32_t id) {
	struct entity** existing = dict_entity_get(CW_ENTITIES, id);
	if(existing)
		return existing;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_test_entity_slot_fail_countdown > 0
	   && --s_test_entity_slot_fail_countdown == 0)
		return NULL;
#endif
	if(!dict_entity_try_reserve_insertions(CW_ENTITIES, 1))
		return NULL;
	return dict_entity_safe_get(CW_ENTITIES, id);
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void clin_test_fail_entity_slot_after(unsigned occurrence) {
	/* Deterministic functional admission seam, independent of spare M-lib
	 * capacity. The allocator transaction is exercised directly by its test. */
	s_test_entity_slot_fail_countdown = occurrence;
}

void clin_test_fail_entity_dict_realloc_after(unsigned occurrence) {
	dict_entity_test_fail_realloc_after(occurrence);
}
#endif

/* Shared by every CRPC_SPAWN_* consumer below. Allocates a NEW entity and
 * installs it at dict[id], FREEING whatever entity previously lived there
 * first -- so a re-spawn for an id already tracked (a server re-sending a
 * spawn packet, or S3's server-side remote-entity table's own documented
 * "duplicate key simply overwrites" semantics) never leaks the entity being
 * replaced (adversarial-review MAJOR: clin_entity_slot returns the EXISTING
 * pointer for a tracked id, and the old per-call-site `*e_ptr =
 * s_clin_alloc(...)` overwrote it unconditionally).
 *
 * On OOM, the new allocation is simply never made: if `id` was already
 * tracked, the PREVIOUS entity is left in place untouched -- a dropped
 * re-spawn is a no-op, not entity loss -- and only a genuine first-time
 * spawn's dangling reservation is erased, matching the original OOM-drop
 * behavior exactly. Returns NULL (spawn dropped, caller must `break`) on any
 * failure; the diagnostic printf differentiates the three cases so `what`
 * (e.g. "item", "monster") reads naturally in each. */
static struct entity* clin_entity_spawn_alloc(uint32_t id, const char* what) {
	struct entity** e_ptr = clin_entity_slot(id);
	if(!e_ptr) {
		printf("[client] entity dictionary alloc failed; %s dropped\n", what);
		return NULL;
	}
	struct entity* fresh = s_clin_alloc(sizeof(struct entity));
	if(!fresh) {
		if(*e_ptr) {
			// Duplicate spawn under OOM: keep the entity already tracked
			// under this id -- do NOT erase a slot that still holds a live,
			// previously-valid entity.
			printf("[client] entity alloc failed (MEM low); re-spawn of "
				   "tracked %s dropped, previous entity kept\n", what);
		} else {
			// Ordinary first-spawn OOM: nothing to preserve at this slot.
			printf("[client] entity alloc failed (MEM low); %s dropped\n",
				  what);
			dict_entity_erase(CW_ENTITIES, id);
		}
		return NULL;
	}
	if(*e_ptr)
		free(*e_ptr); // duplicate spawn: don't leak the entity being replaced
	*e_ptr = fresh;
	return fresh;
}

/* Take-51 root cause (S5.3 world-install stall): vanilla Beta clients echo
 * their position every tick from login onward (EntityClientPlayerMP
 * .sendMotionUpdates runs while chunks are still streaming in), and mc173
 * streams chunks ONLY in response to position packets (update_chunks).
 * CaveWii gated its echo on world_loaded, starving the server of position
 * packets after its single join-time pass -- so chunk streaming stopped and
 * FINISH_REMOTE_LOAD's spawn-chunk gate could never fire (Wii takes 49-51:
 * wl_pending pinned all run). This latch arms the pre-world echo once the
 * remote's authoritative position install has run; a world-switch start
 * disarms it. */

static void clin_hold_input_handoff(void) {
	gstate.world_switch_pending = true;
	gstate.players[0].player_sync.authoritative_pos_seen = false;
	if(s_world_reset_ready && gstate.players[0].local_player
	   && gstate.players[0].current_screen != &screen_load_world)
		screen_set(&screen_load_world);
}

static void clin_clear_input_handoff_latches(void) {
	s_input_presentation_epoch = 0;
	s_input_ack_retry_epoch = 0;
	s_input_ack_wait_epoch = 0;
	s_input_nack_retry_epoch = 0;
	s_input_time_rebase_pending = false;
}

static void clin_clear_stream_recovery(void) {
	s_stream_recovery_phase = CLIN_STREAM_RECOVERY_NONE;
	s_stream_recover_epoch = 0;
	s_stream_recover_retry_epoch = 0;
	s_stream_restore_pause_screen = false;
	s_stream_restore_window = -1;
	s_stream_restore_window_type = WINDOW_TYPE_INVENTORY;
}

static bool clin_input_epoch_is_newer(uint32_t epoch) {
	if(epoch == 0)
		return false;
	if(s_last_input_epoch == 0)
		return true;
	uint32_t delta = epoch - s_last_input_epoch;
	return delta != 0 && delta < UINT32_C(0x80000000);
}

static int clin_current_window_id(void) {
	if(gstate.players[0].current_screen == &screen_inventory
	   && gstate.players[0].windows[WINDOWC_INVENTORY])
		return WINDOWC_INVENTORY;
	if(gstate.players[0].visible_window < 0 || gstate.players[0].visible_window > UINT8_MAX)
		return -1;
	uint8_t window = (uint8_t)gstate.players[0].visible_window;
	struct window_container* wc = gstate.players[0].windows[window];
	if(!wc)
		return -1;
	bool screen_matches
		= (gstate.players[0].current_screen == &screen_crafting
		   && wc->type == WINDOW_TYPE_WORKBENCH)
		|| (gstate.players[0].current_screen == &screen_furnace
			&& wc->type == WINDOW_TYPE_FURNACE)
		|| (gstate.players[0].current_screen == &screen_chest
			&& wc->type == WINDOW_TYPE_CHEST)
		|| (gstate.players[0].current_screen == &screen_dispenser
			&& wc->type == WINDOW_TYPE_DISPENSER)
		|| (gstate.players[0].current_screen == &screen_iron_chest
			&& wc->type == WINDOW_TYPE_IRON_CHEST)
		|| (gstate.players[0].current_screen == &screen_sign
			&& wc->type == WINDOW_TYPE_SIGN);
	return screen_matches ? window : -1;
}

static void clin_expose_input_handoff(void) {
	if(s_input_time_rebase_pending) {
		*clin_clock_of_slot(s_world_payload_slot).world_time_start = time_get();
		s_input_time_rebase_pending = false;
	}
	gstate.world_switch_pending = false;
	if(s_world_reset_ready && gstate.players[0].local_player) {
		if(s_stream_recovery_phase != CLIN_STREAM_RECOVERY_NONE) {
			bool restored = false;
			if(s_stream_restore_pause_screen) {
				/* The SCREEN comes back exactly as it went in, in either
				 * session kind. Only the pause FLAG is local business: Beta
				 * never pauses a multiplayer world (Minecraft.java:598). */
				if(!gstate.remote_session)
					gstate.paused = true;
				screen_set(&screen_pause);
				restored = true;
			} else if(s_stream_restore_window >= 0
				  && s_stream_restore_window <= UINT8_MAX) {
				uint8_t window = (uint8_t)s_stream_restore_window;
				struct window_container* wc = gstate.players[0].windows[window];
				if(wc && wc->type == s_stream_restore_window_type)
					restored = clin_show_window(window);
			}
			if(!restored) {
				gstate.players[0].visible_window = -1;
				screen_set(&screen_ingame);
			}
			clin_clear_stream_recovery();
		} else {
			gstate.players[0].visible_window = -1;
			screen_set(&screen_ingame);
		}
	}
}

static void clin_submit_stream_recovery(uint32_t epoch) {
	assert(epoch != 0);
	struct server_rpc request = {
		.type = SRPC_STREAM_RECOVER,
		.payload.stream_recover.epoch = epoch,
	};
	if(!svin_rpc_send(&request))
		s_stream_recover_retry_epoch = epoch;
	else
		s_stream_recover_retry_epoch = 0;
}

void clin_begin_live_stream_recovery(void) {
	/* A second storage failure can empty a STREAMING world after PLAYER_POS
	 * installed its presentation latch. Keep it invalid until the retry RESET;
	 * neither the readiness fallback nor that marker may ACK the empty world. */
	if(s_stream_recovery_phase != CLIN_STREAM_RECOVERY_NONE) {
		s_world_stream_install_failed = true;
		gstate.world_loaded = false;
		return;
	}
	/* WHICH SCREEN to restore -- recorded in every session kind, so a remote
	 * recovery begun from the pause menu lands back on it (gstate.paused is
	 * always false there; the menu itself is what owns the recovery). */
	s_stream_restore_pause_screen = gstate.paused
		|| gstate.players[0].current_screen == &screen_pause
		/* GuiOptions opened from GuiIngameMenu still owns the paused world;
		 * include it anywhere pause suppresses/rebases window ownership. */
		|| (gstate.players[0].current_screen == &screen_options
			&& screen_options_from_pause());
	s_stream_restore_window
		= s_stream_restore_pause_screen ? -1 : clin_current_window_id();
	if(s_stream_restore_window >= 0) {
		struct window_container* wc
			= gstate.players[0].windows[(uint8_t)s_stream_restore_window];
		if(wc)
			s_stream_restore_window_type = wc->type;
	}
	gstate.world_loaded = false;
	gstate.world_switch_pending = true;
	/* Retain the invalid-stream fact until the server accepts this request and
	 * emits STREAM_RESET. If a newer BEGIN wins the opposite-direction race,
	 * its same-world cancel marker must NACK instead of blessing the hole. */
	s_world_stream_install_failed = true;
	screen_set(&screen_load_world);
	if(s_last_input_epoch == 0) {
		/* Every production world is entered through a tagged marker. Refuse to
		 * invent an uncheckable generation if a legacy/corrupt session violates
		 * that invariant. */
		gstate.world_load_failed = true;
		fprintf(stderr,
				"[client] live voxel recovery has no accepted input epoch\n");
		return;
	}
	s_stream_recovery_phase = CLIN_STREAM_RECOVERY_WAIT_RESET;
	s_stream_recover_epoch = s_last_input_epoch;
	clin_submit_stream_recovery(s_stream_recover_epoch);
}

/* Called on main before input/render, and after the GPU safe point if a
 * client relight requested recovery during this frame. The epoch change lets
 * an in-flight simulation write finish even if an obsolete shell is still
 * pinned by a worker or GPU. The normal STREAM_RESET/ACK restream follows. */
void clin_recover_column_storage(void) {
	if(!column_view_recovery_begin()) return;
	clin_begin_live_stream_recovery();
	client_piston_moving_store_clear(clin_pistons_of_slot(clin_apply_slot()));
	world_unload_all(CW_WORLD);
	particle_system_clear(clin_particles());
	{
		struct sign_text_store* prev
			= sign_text_store_select(clin_signs_of_slot(clin_apply_slot()));
		sign_text_store_clear();
		sign_text_store_select(prev);
	}
}

static void clin_submit_input_ack(uint32_t epoch) {
	assert(epoch != 0);
	/* A newer installed epoch retires every old transport-side wait even when
	 * its own presentation is not ready yet. No old ACK may rebase its clock. */
	s_input_nack_retry_epoch = 0;
	s_input_ack_retry_epoch = 0;
	s_input_ack_wait_epoch = 0;
	if(!gstate.remote_session && !lworld_prepare_presentation()) {
		s_input_presentation_epoch = epoch;
		clin_hold_input_handoff();
		return;
	}
	s_input_presentation_epoch = 0;
	struct server_rpc ack = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready.epoch = epoch,
		.payload.input_ready.retry = false,
	};
	if(!svin_rpc_send(&ack)) {
		/* Suppress all input until clin_update can put the ACK behind the
		 * existing FIFO backlog and before any fresh action. */
		s_input_ack_retry_epoch = epoch;
		clin_hold_input_handoff();
	} else if(svin_input_ready_pending(epoch)) {
		s_input_ack_wait_epoch = epoch;
		clin_hold_input_handoff();
	} else {
		/* Direct inbox admission: later input is guaranteed FIFO-after the ACK
		 * and the ordinary pending budget is still available. */
		clin_expose_input_handoff();
	}
}

static void clin_submit_input_retry(uint32_t epoch) {
	assert(epoch != 0);
	clin_clear_input_handoff_latches();
	if(s_stream_recovery_phase == CLIN_STREAM_RECOVERY_STREAMING)
		s_stream_recovery_phase = CLIN_STREAM_RECOVERY_WAIT_RESET;
	struct server_rpc retry = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready.epoch = epoch,
		.payload.input_ready.retry = true,
	};
	if(!svin_rpc_send(&retry))
		s_input_nack_retry_epoch = epoch;
	else
		s_input_nack_retry_epoch = 0;
	clin_hold_input_handoff();
}

/* Main-thread presentation callback. Also polled after draining replies, so a
 * queued BEGIN/RESET/FAIL retires its obsolete epoch before this can send. */
void clin_update_input_readiness(void) {
	if(s_input_presentation_epoch == 0) return;
	uint32_t epoch = s_input_presentation_epoch;
	if(s_world_stream_install_failed) {
		/* Streaming continues while presentation waits. A late failed column
		 * must invalidate this not-yet-ACKed snapshot just like a failure before
		 * its marker; otherwise the server would retain a missing sent column. */
		client_piston_moving_store_clear(clin_pistons_of_slot(clin_apply_slot()));
		world_unload_all(CW_WORLD);
		particle_system_clear(clin_particles());
		gstate.world_loaded = false;
		s_world_stream_install_failed = false;
		clin_submit_input_retry(epoch);
		return;
	}
	clin_submit_input_ack(epoch);
}

static void clin_reset_input_handoff(
	const struct client_rpc* handoff) {
	assert(handoff && handoff->type == CRPC_PLAYER_POS
		   && handoff->payload.player_pos.input_epoch != 0);
	const bool fresh_body = handoff->payload.player_pos.fresh_body;
	if(fresh_body)
		clin_reset_player_state_latch();
	/* Slot RPCs update each window's authoritative first snapshot. Any actions
	 * authored before this handoff are rejected server-side, so discard their
	 * speculative descendants before gameplay can render them again. */
	for(size_t k = 0; k < 256; k++)
		if(gstate.players[0].windows[k])
			windowc_discard_pending_actions(gstate.players[0].windows[k]);
	/* The selected slot is optimistic base state rather than a window revision.
	 * Reapply it from the epoch marker itself so a scroll between a standalone
	 * HOTBAR snapshot and this marker cannot survive the ownership boundary. */
	windowc_hotbar_change(
		gstate.players[0].windows[WINDOWC_INVENTORY],
		handoff->payload.player_pos.selected_hotbar);
	for(size_t k = 0; k < 3; k++)
		gstate.players[0].local_player->vel[k]
			= handoff->payload.player_pos.velocity[k];
	gstate.players[0].local_player->on_fire = handoff->payload.player_pos.on_fire;
	gstate.players[0].local_player->on_ground = handoff->payload.player_pos.on_ground;
	gstate.players[0].oxygen = handoff->payload.player_pos.oxygen;
	*clin_clock_of_slot(s_world_payload_slot).world_time
		= handoff->payload.player_pos.world_time;
	s_input_time_rebase_pending = true;
	/* A death handoff owns a replacement body; a dimension handoff retains the
	 * same player's body snapshot. Input-owned transients, however, belong to the
	 * old input epoch in both cases. Leaving one armed can synthesize a dig or
	 * place after its release edge was consumed by the input-less loading screen. */
	if(fresh_body) {
		gstate.players[0].local_player->data.local_player.jump_ticks = 0;
		/* F3/F4: a death handoff owns a replacement body. Clear the
		 * centralized ride state so a pre-death dirty intent or camera
		 * offset cannot survive into the replacement body and be sent
		 * under its new handoff (invariant 10).
		 *
		 * Pass NULL for the camera: the CRPC_PLAYER_POS handler has
		 * already overwritten gstate.players[0].camera with the fresh authoritative
		 * position. The old camera_correction belongs to
		 * the dead body's session and must NOT be subtracted from the
		 * new body's position. Folding here would produce
		 * camera = new_authority - old_C. */
		minecart_ride_client_state_reset(&gstate.players[0].ride, NULL, false);
		/* Overworld death does not WORLD_RESET the client entity dictionary. Clear
		 * every cart which still names the dead local entity as rider; stopping at
		 * the first would let a second stale claim snap the fresh body from spawn. */
		dict_entity_it_t it;
		dict_entity_it(it, CW_ENTITIES);
		while(!dict_entity_end_p(it)) {
			struct entity* cart = dict_entity_ref(it)->value;
			if(cart && cart->type == ENTITY_MINECART) {
				if(cart->data.minecart.occupied
				   && cart->data.minecart.occupant_id
					  == gstate.players[0].local_player->id) {
					cart->data.minecart.occupied = false;
					cart->data.minecart.occupant_id = 0;
					cart->rightClickText = "Ride";
				}
				/* A same-world fresh body does not reset the entity dictionary.
				 * Nevertheless every locally authored control tuple belongs to the
				 * dead body's input session and must be terminally abandoned. */
				cart->data.minecart.pending_throttle = 0;
				cart->data.minecart.replay_throttle = 0;
				cart->data.minecart.control_seq
					= cart->data.minecart.control_acked_seq;
				cart->data.minecart.control_positive_steps
					= cart->data.minecart.control_acked_positive_steps;
				cart->data.minecart.control_negative_steps
					= cart->data.minecart.control_acked_negative_steps;
				cart->data.minecart.control_ready = false;
				cart->data.minecart.control_dirty = false;
			}
			dict_entity_next(it);
		}
	}
	gstate.players[0].digging.active = false;
	gstate.players[0].digging.start = time_get();
	gstate.players[0].digging.cooldown = gstate.players[0].digging.start;
	gstate.players[0].held_item_animation.punch.start = gstate.players[0].digging.start;
	gstate.players[0].held_item_animation.punch.place = false;
	gstate.players[0].held_item_animation.punch.equip_only = false;
	gstate.players[0].held_item_animation.switch_item.start = gstate.players[0].digging.start;
	gstate.players[0].held_item_animation.switch_item.old_slot = inventory_get_hotbar(
		windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]));
	gstate.players[0].camera_hit.hit = false;
	gstate.players[0].camera_hit.entity_hit = false;
	gstate.players[0].camera_hit.entity_id = 0;
	gstate.autopilot.pending_place = 0;
}

// Pre-allocated chunk-data slots. ~80 KB each.
//
// A slot is owned by one CRPC_CHUNK, in the producer's pending output or the
// client inbox. The pool bounds total live snapshots independently of queue
// depth. A busy acquire returns NULL so streaming can retry without parking
// simulation behind snapshots that only this producer can publish.
#define CHUNK_RPC_POOL_SIZE RPC_INBOX_SIZE
static struct chunk_rpc_slot rpc_chunk_pool[CHUNK_RPC_POOL_SIZE];
static struct thread_channel rpc_chunk_pool_empty;

// Far-ring slot pool (M1 deep reach), ~1 KB each, sized to RPC_INBOX_SIZE like the
// chunk pool: a far slot is only live while its CRPC_FAR_CHUNK sits in the same
// 16-deep inbox, so the pool size bounds the live count exactly.
#define FAR_RPC_POOL_SIZE RPC_INBOX_SIZE
static struct far_rpc_slot far_rpc_pool[FAR_RPC_POOL_SIZE];
static struct thread_channel far_rpc_pool_empty;
// Far-pool backpressure instrument: times the far-send batch found the pool exhausted
// and DEFERRED. Non-zero is normal streaming pressure, not a fault; a run of them that
// never ENDS is a leak, and the far batch's own consecutive-bail counter is what turns
// that distinction into a report (server_local.c) -- this is the figure it quotes, and
// the observable that proves the non-blocking bail ran instead of a server-thread park.
// Plain (unguarded): the increment has ONE caller and it is the server thread, and the
// only cross-thread reader is a diagnostic that cannot care about a one-count skew.
static uint32_t s_far_slot_busy = 0;

/* Held-map colour transport (held-map render slice, 2026-08-26). TWO slots:
 * one in flight + one being filled covers the single held map this client
 * can display; the sender's non-blocking acquire simply retries next scan
 * when both are out (far-batch defer precedent, never a server-thread park).
 * The client-side MIRROR below is the renderer's read surface -- main-thread
 * writes (clin_process) + main/render-thread reads, no lock needed under the
 * engine's existing single-main-thread RPC drain. */
_Static_assert(MAP_RPC_COLORS_SIZE == MAP_DATA_COLORS_SIZE,
			   "map RPC slot must carry exactly the Beta colors[] payload");
#define MAP_RPC_POOL_SIZE 2
static struct map_rpc_slot map_rpc_pool[MAP_RPC_POOL_SIZE];
static struct thread_channel map_rpc_pool_empty;
/* [740c583 MapStorage.loadData/setData, World.loadItemData:2466-2468]:
 * Java retains loaded MapData for the session. Wii bounds RAM to eight planes
	 * (eight color planes plus 256 transient marker bytes per cached map).
 * Blank predictions own NO entry: publication/pickup simply selects the real
 * ID, so there is no speculative plane to promote, orphan, or retain on close.
 * Evicted DATA is spilled byte-exactly to session-private map_<id>.dat files
 * (raw colors, NOT world-save NBT). Reload never creates/spends a map ID and
 * does not rely on a vanilla server resending unchanged Packet131 columns.
 * See docs/deviations.md, client map residency. Main/render thread only. */
struct client_map_data {
	struct client_map_data* next;
	int32_t map_id;
	uint32_t version;
	uint8_t colors[MAP_RPC_COLORS_SIZE];
	uint8_t marker_length;
	uint8_t marker_bytes[255];
};
static struct client_map_data* s_client_maps; // MRU first, LRU last
static uint32_t s_map_version;
static unsigned s_map_count;
static uint32_t s_map_alloc_failures, s_map_io_failures;
static const uint8_t s_map_blank[MAP_RPC_COLORS_SIZE];
/* Signed-short identity domain, one bit per spilled map, no heap metadata. */
static uint8_t s_map_spilled[65536 / 8];
static char s_map_cache_dir[64];

static void clin_map_path(char path[96], int32_t id) {
	snprintf(path, 96, "%s/map_%d.dat", s_map_cache_dir, (int)id);
}

static bool clin_map_spill(const struct client_map_data* m) {
	if(!s_map_cache_dir[0]) {
		/* mkdir reserves a private directory even with concurrent PC clients. */
		for(unsigned n = 0; ; n++) {
			snprintf(s_map_cache_dir, sizeof(s_map_cache_dir), ".cw-client-maps-%u", n);
#ifdef _WIN32
			int result = _mkdir(s_map_cache_dir);
#else
			int result = mkdir(s_map_cache_dir, 0777);
#endif
			if(!result) break;
			if(errno != EEXIST || n == UINT32_MAX) {
				s_map_cache_dir[0] = 0;
				return false;
			}
		}
	}
	char path[96]; clin_map_path(path, m->map_id);
	FILE* file = fopen(path, "wb");
	if(!file) return false;
	bool ok = fwrite(m->colors, 1, sizeof(m->colors), file) == sizeof(m->colors);
	if(ok) ok = fwrite(&m->marker_length, 1, 1, file) == 1
		&& fwrite(m->marker_bytes, 1, m->marker_length, file) == m->marker_length;
	if(fclose(file)) ok = false;
	unsigned id = (uint16_t)m->map_id;
	if(ok) s_map_spilled[id / 8] |= 1u << (id % 8);
	else {
		s_map_spilled[id / 8] &= ~(1u << (id % 8));
		remove(path);
	}
	return ok;
}

static struct client_map_data* clin_map_find(int32_t id) {
	struct client_map_data** link = &s_client_maps;
	while(*link) {
		struct client_map_data* m = *link;
		if(m->map_id == id) {
			*link = m->next;
			m->next = s_client_maps;
			s_client_maps = m;
			return m;
		}
		link = &m->next;
	}
	return NULL;
}

/* Called only for received DATA or a known disk-backed plane. Reuse the LRU
 * allocation, never allocate a ninth plane even transiently. On spill failure
 * retain the old entry and render the missing identity blank; count the failure. */
static struct client_map_data* clin_map_allocate(int32_t id) {
	struct client_map_data* m;
	if(s_map_count == CLIN_MAP_PLANES) {
		struct client_map_data** link = &s_client_maps;
		while((*link)->next) link = &(*link)->next;
		m = *link;
		if(!clin_map_spill(m)) { s_map_io_failures++; return NULL; }
		*link = NULL;
	} else {
		m = s_clin_alloc(sizeof(*m));
		if(!m) { s_map_alloc_failures++; return NULL; }
		s_map_count++;
	}
	m->map_id = id;
	memset(m->colors, 0, sizeof(m->colors));
	m->marker_length = 0;
	m->version = s_map_version;
	m->next = s_client_maps;
	s_client_maps = m;
	return m;
}

static void clin_clear_map_data(void) {
	while(s_client_maps) {
		struct client_map_data* next = s_client_maps->next;
		free(s_client_maps);
		s_client_maps = next;
	}
	s_map_count = 0;
	if(s_map_cache_dir[0]) {
		for(unsigned id = 0; id < 65536; id++) {
			if(!(s_map_spilled[id / 8] & (1u << (id % 8)))) continue;
			char path[96]; clin_map_path(path, (int16_t)id);
			remove(path);
		}
#ifdef _WIN32
		_rmdir(s_map_cache_dir);
#else
		rmdir(s_map_cache_dir);
#endif
		s_map_cache_dir[0] = 0;
	}
	memset(s_map_spilled, 0, sizeof(s_map_spilled));
	/* Invalidate the uploaded texture across relog, even for the same ID. */
	s_map_version++;
}

uint32_t clin_map_allocation_failures(void) { return s_map_alloc_failures; }
uint32_t clin_map_io_failures(void) { return s_map_io_failures; }
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
size_t clin_map_test_plane_count(void) { return s_map_count; }
static bool s_map_fail_reload;
void clin_map_test_fail_reload(bool fail) { s_map_fail_reload = fail; }
#endif

// Per-drain budget for AIR-set dig-particle bursts (see CRPC_SET_BLOCK in
// clin_process): reset each clin_update, so single-block mining always bursts
// while a bulk server clear (explosion) cannot firehose the FIFO. 4 covers any
// legitimate same-frame multi-dig (instant-break spam) with margin.
#define CLIN_DIG_BURSTS_PER_DRAIN 4
static int clin_dig_burst_budget = CLIN_DIG_BURSTS_PER_DRAIN;

/* CHIP A (task_11695e19, 2026-08-31): net world_loaded_chunks() cardinality
 * (dict_wsection_size) is blind to in-flight streaming. remote_emit_pending_
 * unloads_locked (server_remote_chunks.c) drops every marked-unload_pending
 * column in ONE pump call, while remote_promotion_step_locked decodes at
 * most REMOTE_PROMOTION_OUTPUT_BUDGET (32KB) of ONE active column per pump --
 * a same-dim teleport that re-centers the streaming ring can therefore burn
 * many frames where a destination column is genuinely inching through zlib
 * decode (or several unrelated columns are unloading/installing) while the
 * NET section count never moves, or nets to the same value across samples.
 * screen_load_world.c's lworld_stall_tracker only ever sampled that net
 * count, so real streaming work could read as "no progress" toward its 30s
 * disconnect timeout. This counter bumps once per column actually installed
 * (clin_chunk's full-column path, clin_far_chunk) regardless of whether the
 * net section count moved -- the tracker now also advances on THIS, so a
 * churn/decode-latency window that hides in world_loaded_chunks() alone is
 * still visible as progress. Plain (unguarded): single-writer (main/client
 * thread only, whether the caller is clin_update's inbox drain or server_
 * remote_chunks.c's clin_process_direct -- both run on that same thread),
 * single in-process reader (screen_load_world.c, same thread). */
static uint32_t s_chunk_install_seq = 0;

uint32_t clin_chunk_install_seq(void) {
	return s_chunk_install_seq;
}

static const struct client_rpc* s_settle_install_call;
static bool clin_chunk(w_coord_t x, w_coord_t y, w_coord_t z, w_coord_t sx,
					   w_coord_t sy, w_coord_t sz,
					   struct chunk_rpc_slot* slot) {
	assert(sx > 0 && sz > 0 && y >= 0 && y + sy <= WORLD_HEIGHT);
	assert(slot);
	if(x % CHUNK_SIZE != 0 || z % CHUNK_SIZE != 0 || y != 0
	   || sx != CHUNK_SIZE || sy != WORLD_HEIGHT || sz != CHUNK_SIZE) {
		/* Packet51 permits rectangular sub-volumes. Its planes use the same
		 * [x][z][y] order as a full column; install these synchronously through
		 * the ordinary block writer so existing heightmap semantics are retained.
		 * [740c583 Chunk.getChunkData / WorldClient.setChunkData] */
		for(w_coord_t lx = 0; lx < sx; lx++)
			for(w_coord_t lz = 0; lz < sz; lz++)
				for(w_coord_t ly = 0; ly < sy; ly++) {
					size_t index = ((size_t)lx * (size_t)sz + (size_t)lz)
						* (size_t)sy + (size_t)ly;
					uint8_t shift = (index & 1u) ? 4u : 0u;
					world_set_block(CW_WORLD, x + lx, y + ly, z + lz,
						(struct block_data) {
							.type = slot->ids[index],
							.metadata = (slot->metadata[index / 2] >> shift) & 0xF,
							.sky_light = (slot->lighting_sky[index / 2] >> shift) & 0xF,
							.torch_light = (slot->lighting_torch[index / 2] >> shift) & 0xF,
						}, false);
				}
		clin_chunk_slot_release(slot);
		return true;
	}

	// BULK install. The server always sends the whole column (y==0, sy==WORLD_HEIGHT,
	// sx==sz==16), so the builder resolves the section + all 8 vertical chunks ONCE,
	// writes cells via chunk_set_block_raw, copies the server heightmap, sets the tint,
	// and dirties the section + 4 neighbours ONCE -- byte-equivalent to the old per-cell
	// world_set_block fill (it also tore down any far-ring section first; the builder
	// does that too) but ~20x cheaper. The old path's 32K world_set_block/column made the
	// client install the load-time bottleneck (and hitched the render thread per column).
	w_coord_t chunk_x = WCOORD_CHUNK_OFFSET(x);
	w_coord_t chunk_z = WCOORD_CHUNK_OFFSET(z);
	/* Publish every id-36 sidecar before the bulk builder dirties the column.
	 * A mesher/render callback can therefore never observe a new moving cell
	 * without its exact stored block and progress. */
	if(!client_piston_moving_replace_chunk(
		   chunk_x, chunk_z, slot->piston_moving.records,
		   slot->piston_moving.count)) {
		clin_chunk_slot_release(slot);
		return false;
	}
	tps_note(client_tps_observe, TPS_ENTER, 0);
	bool installed = slot->backing ? world_build_full_section_leased(
		CW_WORLD, x, z, slot->ids, slot->metadata, slot->lighting_sky,
		slot->lighting_torch, slot->heightmap, slot->grass_i, slot->grass_j,
		slot->backing) : world_build_full_section(
		CW_WORLD, x, z, slot->ids, slot->metadata, slot->lighting_sky,
		slot->lighting_torch, slot->heightmap, slot->grass_i, slot->grass_j);
	tps_note(client_tps_observe, TPS_LEAVE, 0);
	SETTLE(client_settle_observe, installed ? SET_INSTALL_OK : SET_INSTALL_FAIL,
		   NULL, s_settle_install_call, 1, 0, 0, 0);
	if(!installed)
		client_piston_moving_remove_chunk(chunk_x, chunk_z);
	else {
		if(slot->backing)
			world_local_revision(CW_WORLD, x, z, slot->local_revision, 0, true);
		s_chunk_install_seq++; // CHIP A: net section count-blind progress signal
	}

	// Return the slot to the pool. No malloc, no free — the buffers
	// stay alive as part of rpc_chunk_pool[] for the next chunk send.
	clin_chunk_slot_release(slot);
	return installed;
}

// M1 deep reach: install a far-ring chunk from its compact per-column skin surface
// (skin_y + top_id + tint, ~1 KB). We BUILD A REAL chunk with cheap synthetic voxels
// so it renders through the UNCHANGED render BFS + lod=1 skin path -- no new render
// path. x,z = the chunk-origin WORLD coords (chunk_x*CHUNK_SIZE), like clin_chunk.
static bool clin_far_chunk(w_coord_t x, w_coord_t z,
					   struct far_rpc_slot* slot) {
	assert(slot);

	// Build the far chunk directly from its per-column skin (skin_y/top_id) + tint:
	// one section resolve, one bulk fill via chunk_set_block_raw, one dirty +
	// neighbour pass, the heightmap set directly to the surface fixpoint. This
	// replaces the old ~5000 per-cell world_set_block calls (each a dict_wsection
	// hash lookup + a heightmap scan + a chunk_mark_dirty/6-neighbour trigger),
	// which dropped the client to ~2fps under fast far streaming. Byte-identical
	// blocks + heightmap (bench_far_chunk diff=0). The builder publishes only a
	// complete replacement, preserving the old section if allocation fails.
	tps_note(client_tps_observe, TPS_ENTER, 0);
	bool installed = world_build_far_section(
		CW_WORLD, x, z, slot->skin_y, slot->top_id, slot->grass_i,
		slot->grass_j);
	tps_note(client_tps_observe, TPS_LEAVE, 0);
	SETTLE(client_settle_observe, installed ? SET_INSTALL_OK : SET_INSTALL_FAIL,
		   NULL, s_settle_install_call, 1, 0, 0, 0);
	if(installed)
		s_chunk_install_seq++; // CHIP A: same progress signal as clin_chunk

	clin_far_slot_release(slot);
	return installed;
}

bool clin_show_window(uint8_t window) {
	if(!gstate.players[0].windows[window])
		return false;
	switch(gstate.players[0].windows[window]->type) {
		case WINDOW_TYPE_INVENTORY:
			screen_set(&screen_inventory);
			gstate.players[0].visible_window = window;
			return true;
		case WINDOW_TYPE_WORKBENCH:
			screen_crafting_set_windowc(window);
			screen_set(&screen_crafting);
			gstate.players[0].visible_window = window;
			return true;
		case WINDOW_TYPE_FURNACE:
			screen_furnace_set_windowc(window);
			screen_set(&screen_furnace);
			gstate.players[0].visible_window = window;
			return true;
		case WINDOW_TYPE_CHEST:
			screen_chest_set_windowc(window);
			screen_set(&screen_chest);
			gstate.players[0].visible_window = window;
			return true;
		case WINDOW_TYPE_DISPENSER:
			screen_dispenser_set_windowc(window);
			screen_set(&screen_dispenser);
			gstate.players[0].visible_window = window;
			return true;
		case WINDOW_TYPE_IRON_CHEST:
			screen_iron_chest_set_windowc(window);
			screen_set(&screen_iron_chest);
			gstate.players[0].visible_window = window;
			return true;
		case WINDOW_TYPE_SIGN:
			screen_sign_set_windowc(window);
			screen_set(&screen_sign);
			gstate.players[0].visible_window = window;
			return true;
		default:
			return false;
	}
}

void clin_apply_position_only_correction(struct entity* player,
										struct camera* camera,
										const vec3 position, uint8_t flags) {
	assert(player && camera && position);
	vec3 delta = {
		position[0] - player->pos[0],
		position[1] - player->pos[1],
		position[2] - player->pos[2],
	};
	for(int axis = 0; axis < 3; axis++) {
		player->pos[axis] += delta[axis];
		player->pos_old[axis] += delta[axis];
		player->network_pos[axis] += delta[axis];
	}
	camera->x = position[0];
	camera->y = position[1];
	camera->z = position[2];
	/* An ABSOLUTE set, not a shift: the stored camera no longer carries the
	 * dip the last attach subtracted, so the sneak fold-out RECORD has to go
	 * with it or clin_update would send position[1] + applied. Record ONLY
	 * (review 19 M-1): this arm is the shared correction channel -- piston
	 * pushes, knockback, local and guest corrections -- not Beta's teleport
	 * packet, and Beta moves the player through moveEntity for every one of
	 * those, which leaves ySize alone. Keeping the dip state means a latched
	 * sneak does not pop the camera once per correction; the full-snapshot arm
	 * below is the teleport and takes the full reset. See player_ysize.h. */
	player_ysize_record_clear(&camera->ysize);
	if(flags & PLAYER_CORRECTION_ZERO_VEL_X)
		player->vel[0] = 0.0F;
	if(flags & PLAYER_CORRECTION_ZERO_VEL_Y)
		player->vel[1] = 0.0F;
	if(flags & PLAYER_CORRECTION_ZERO_VEL_Z)
		player->vel[2] = 0.0F;
	if(flags & PLAYER_CORRECTION_SET_ON_GROUND)
		player->on_ground = (flags & PLAYER_CORRECTION_ON_GROUND) != 0;
}

/* Apply only the named body's position and correction token. The caller must
 * establish body/terrain ownership first and owns clocks, screens and ACKs. */
bool clin_apply_player_position(struct client_session* session,
	const struct client_rpc* call) {
	if(!session || !session->local_player || !call || call->type != CRPC_PLAYER_POS)
		return false;
	session->player_sync.correction_seq = call->payload.player_pos.correction_seq;
	session->player_sync.authoritative_pos_seen = true;
	if(call->payload.player_pos.correction_flags & PLAYER_CORRECTION_POSITION_ONLY) {
		clin_apply_position_only_correction(session->local_player, &session->camera,
			call->payload.player_pos.position, call->payload.player_pos.correction_flags);
		if(call->payload.player_pos.correction_flags
		   & PLAYER_CORRECTION_SET_ROTATION) {
			session->camera.rx = glm_rad(
				-call->payload.player_pos.rotation[0]);
			session->camera.ry = glm_rad(glm_clamp(
				call->payload.player_pos.rotation[1] + 90.0F,
				0.0F, 180.0F));
			session->local_player->orient[0] = session->camera.rx;
			session->local_player->orient[1] = session->camera.ry;
			glm_vec2_copy(session->local_player->orient,
				session->local_player->orient_old);
		}
	} else {
		session->camera.x = call->payload.player_pos.position[0];
		session->camera.y = call->payload.player_pos.position[1];
		session->camera.z = call->payload.player_pos.position[2];
		/* The full-snapshot arm SETS the camera, so the sneak dip's fold-out
		 * record is stale the instant it does -- same reason, same Beta
		 * citation, as the POSITION_ONLY arm above. This is the arm a
		 * cross-dimension teleport takes, and the pre-world echo below keeps
		 * sending for the WHOLE terrain hold with no attach in between, so a
		 * stale record here is not one frame of error but every 50 ms of it. */
		player_ysize_absolute_set(&session->camera.ysize);
		session->camera.rx = glm_rad(-call->payload.player_pos.rotation[0]);
		session->camera.ry = glm_rad(glm_clamp(
			call->payload.player_pos.rotation[1] + 90.0F, 0.0F, 180.0F));
		/* Main can run up to three catch-up physics ticks before camera_attach.
		 * Install both entity orientation samples now so the first fresh stick
		 * acceleration cannot use the source/default yaw. */
		session->local_player->orient[0] = session->camera.rx;
		session->local_player->orient[1] = session->camera.ry;
		glm_vec2_copy(
			session->local_player->orient, session->local_player->orient_old);
		if(session->local_player)
			session->local_player->teleport(
				session->local_player,
				(vec3) {call->payload.player_pos.position[0],
						call->payload.player_pos.position[1],
						call->payload.player_pos.position[2]});
	}
	return true;
}

/* lava-aabb-0924/B3. Deliberately NOT folded into clin_apply_player_position:
 * that routine also installs every correction CRPC_PLAYER_POS (knockback, bed,
 * LOCAL_GUEST_CORRECTION, host-respawn corrections, and the remote-MP
 * server_remote_protocol.c/server_remote_chunks.c translations), none of which
 * carries a fire value, so copying player_pos.on_fire there would clear a live
 * burn on every shove and change remote-MP behaviour. The fire flag has one
 * dedicated per-tick writer instead. */
bool clin_apply_player_fire(struct client_session* session,
	const struct client_rpc* call) {
	if(!session || !session->local_player || !call
	   || call->type != CRPC_LOCAL_PLAYER_FIRE)
		return false;
	session->local_player->on_fire = call->payload.local_player_fire.on_fire;
	return true;
}

/* damage-client-0926: NetClientHandler.handleEntityVelocity (:165-170) ->
 * Entity.setVelocity (:1000-1004) is a plain assignment. With the LOCAL server
 * only motionY is installed; the horizontal knockback stays server-integrated
 * (see the CRPC_LOCAL_PLAYER_VELOCITY enum comment, as ruled). On a REMOTE
 * Beta server (mp-velocity-0926) the server does not integrate the player's
 * horizontal motion -- the client does, exactly as Beta's EntityPlayerSP -- so
 * a remote session installs all three components, per setVelocity. The next
 * local tick moves the body by it and then applies Beta's gravity/drag
 * (EntityLiving.moveEntityWithHeading:528-535), exactly as for a jump. */
bool clin_apply_player_velocity(struct client_session* session,
	const struct client_rpc* call) {
	if(!session || !session->local_player || !call
	   || call->type != CRPC_LOCAL_PLAYER_VELOCITY)
		return false;
	/* G8: the local server no longer integrates the horizontal half, so a
	 * local session installs X/Z too, exactly as the remote one: the body's
	 * own moveEntityWithHeading decays it with Beta friction while
	 * moveFlying keeps adding stick input (EntityPlayerSP). An additive
	 * push (Entity.addVelocity) extends the motion instead of replacing it. */
	const float* v = call->payload.local_player_velocity.vel;
	struct entity* e = session->local_player;
	if(call->payload.local_player_velocity.add) {
		e->vel[0] += v[0];
		e->vel[1] += v[1];
		e->vel[2] += v[2];
	} else {
		e->vel[0] = v[0];
		e->vel[1] = v[1];
		e->vel[2] = v[2];
	}
	return true;
}

bool clin_local_body_fire_visible(const struct client_session* owner) {
	if(!owner || !owner->local_player || !owner->windows[WINDOWC_INVENTORY])
		return false;
	const struct entity* e = owner->local_player;
	if(!e->on_fire)
		return false;
	/* entity_local_player_render_other's own corpse retire
	 * (EntityLiving.onEntityUpdate deathTime > 20): no body, no flames. */
	return !(e->health <= 0 && owner->camera.hurt.death_time > 20);
}

void clin_apply_entity_physics_correction(struct entity* e,
										 const struct client_rpc* call) {
	assert(e && call && call->type == CRPC_ENTITY_PHYSICS_CORRECTION);
	const struct entity_physics_correction_rpc* p
		= &call->payload.entity_physics_correction;
	assert(p->entity_id == e->id);
	vec3 delta = {
		p->pos[0] - e->pos[0],
		p->pos[1] - e->pos[1],
		p->pos[2] - e->pos[2],
	};
	for(int axis = 0; axis < 3; axis++) {
		e->pos[axis] = p->pos[axis];
		e->pos_old[axis] += delta[axis];
		e->network_pos[axis] = p->pos[axis];
		e->vel[axis] = p->vel[axis];
	}
	e->y_size = p->y_size;
	e->fall_distance = p->fall_distance;
	e->on_ground = p->on_ground;
	e->collided_horizontally = p->collided_horizontally;
	e->collided_vertically = p->collided_vertically;
	e->collided = p->collided_horizontally || p->collided_vertically;
	e->in_web = p->in_web;
	e->on_fire = p->on_fire;
	if(e->type == ENTITY_MONSTER)
		e->data.monster.fall_distance = p->fall_distance;
	else if(e->type == ENTITY_MINECART) {
		e->data.minecart.speed = p->minecart_speed;
		e->data.minecart.hx = p->minecart_hx;
		e->data.minecart.hz = p->minecart_hz;
		e->data.minecart.rail_meta = p->minecart_rail_meta;
		e->data.minecart.last_meta = p->minecart_last_meta;
	} else if(e->type == ENTITY_PRIMED_TNT) {
		e->data.primed_tnt.fuse = p->fuse;
		e->data.primed_tnt.awaiting_destroy = false;
	}
}

/* RFC-1982-style comparison for uint32 ride epochs/counters. Exactly half the
 * serial space is ambiguous and therefore not considered newer. */
static bool clin_minecart_serial_newer(uint32_t candidate, uint32_t current) {
	uint32_t delta = candidate - current;
	return delta != 0 && delta < UINT32_C(0x80000000);
}

/* Zero is the explicit no-rider/no-session epoch. It is accepted only with an
 * authoritative empty state. Nonzero epochs otherwise follow serial ordering.
 * The same-process client channel is FIFO, so an old dismount cannot overtake a
 * later remount and erase its newer epoch. */
static bool clin_minecart_epoch_acceptable(
	uint32_t candidate, uint32_t current, bool occupied) {
	/* Occupancy and ownership are one invariant: zero means empty, while every
	 * occupied cart must carry a nonzero transient session. Reject both malformed
	 * shapes before they can update physics or make control appear usable. */
	if(occupied != (candidate != 0))
		return false;
	if(candidate == 0)
		return true;
	/* No live session exists to order against. The next authoritative mount or
	 * occupied spawn may use any nonzero uint32 epoch, including one in the upper
	 * serial half; comparing it to the zero sentinel would incorrectly reject
	 * half of the protocol's valid epoch space. FIFO ordering still prevents a
	 * retired mount from overtaking the dismount that cleared `current`. */
	if(current == 0)
		return true;
	if(candidate == current)
		return true;
	return clin_minecart_serial_newer(candidate, current);
}

/* Install one complete authoritative minecart state. A spawn is an ownership
 * boundary and always replaces constructor defaults. A later snapshot with an
 * older ride epoch is ignored wholesale so delayed state from a prior mount
 * cannot overwrite a remount. */
// Mirror a container window's PLAYER-INVENTORY section into the base
// inventory window (task_e0bcc88e root cause, 2026-08-26). Beta's Container
// slots REFERENCE the one player inventory, so a SetSlot/WindowItems naming a
// chest-window slot in the main/hotbar section mutates the player inventory
// itself [740c583 Container.putStackInSlot -> Slot.inventory]. This port's
// window containers hold detached inventory copies, so without this
// write-through every server-side inventory change that arrives while a
// container is open (item pickups, /give, the server's own click echoes)
// updates ONLY the container view -- the base window-0 hotbar/main rots stale,
// and after the container closes the client acts on ghosts (live-gate
// symptom: hand-slot desync -> mc173 silently no-ops every later placement).
// Container-own sections (storage/craft/fuel cells) are NOT mirrored.

static bool clin_apply_minecart_state(
	struct entity* e, const struct minecart_state_rpc* p, bool spawn) {
	assert(e && p && e->type == ENTITY_MINECART && p->entity_id == e->id);

	uint32_t current_epoch = spawn ? 0 : e->data.minecart.ride_epoch;
	/* F4: spawn is an ownership boundary and always replaces constructor
	 * defaults. A non-spawn snapshot must match the session installed by
	 * the reliable occupancy writer exactly -- an old or future-session
	 * snapshot is ignored rather than creating a session implicitly. */
	if(spawn) {
		if(!clin_minecart_epoch_acceptable(
			   p->ride_epoch, current_epoch, p->occupied))
			return false;
	} else {
		/* F4: reject structurally invalid epoch-zero + occupied on non-spawn
		 * snapshots too, not just spawn. An occupied cart with no session
		 * cannot be corrected by a periodic snapshot. */
		if(p->ride_epoch == 0 && p->occupied)
			return false;
		if(p->ride_epoch != current_epoch)
			return false;
	}

	int32_t replay_steps = 0;
	bool same_epoch = !spawn
		&& p->ride_epoch == e->data.minecart.ride_epoch;
	if(same_epoch) {
		uint32_t dseq = e->data.minecart.control_seq
			- p->accepted_input_seq;
		uint32_t dpos = e->data.minecart.control_positive_steps
			- p->accepted_positive_steps;
		uint32_t dneg = e->data.minecart.control_negative_steps
			- p->accepted_negative_steps;
		if(dseq == 0 && dpos == 0 && dneg == 0) {
			e->data.minecart.control_dirty = false;
		} else if(dseq > 0 && dseq <= MINECART_CONTROL_MAX_GAP
			  && dpos <= dseq && dneg <= dseq
			  && (uint64_t)dpos + (uint64_t)dneg == (uint64_t)dseq) {
			replay_steps = (int32_t)dpos - (int32_t)dneg;
			e->data.minecart.control_dirty = true;
		} else if(dseq != 0 && dseq < UINT32_C(0x80000000)) {
			/* More than the bounded replay window remains locally authored.
			 * Keep it dirty but do not synthesize an impulse; the next cumulative
			 * send makes the server rotate the epoch and publish a clean base. */
			e->data.minecart.control_dirty = true;
		} else {
			/* The authority is newer (or the half-range relation is ambiguous).
			 * Fail closed onto its tuple instead of replaying corrupt local state. */
			e->data.minecart.control_seq = p->accepted_input_seq;
			e->data.minecart.control_positive_steps
				= p->accepted_positive_steps;
			e->data.minecart.control_negative_steps
				= p->accepted_negative_steps;
			e->data.minecart.control_dirty = false;
		}
	} else {
		e->data.minecart.ride_epoch = p->ride_epoch;
		e->data.minecart.control_seq = p->accepted_input_seq;
		e->data.minecart.control_positive_steps
			= p->accepted_positive_steps;
		e->data.minecart.control_negative_steps
			= p->accepted_negative_steps;
		e->data.minecart.control_dirty = false;
	}
	e->data.minecart.control_acked_seq = p->accepted_input_seq;
	e->data.minecart.control_acked_positive_steps
		= p->accepted_positive_steps;
	e->data.minecart.control_acked_negative_steps
		= p->accepted_negative_steps;
	/* F4: control_ready is part of occupancy state. On spawn it is set
	 * from the spawn payload; on non-spawn, only CRPC_MINECART_OCCUPANCY
	 * may write it (single-writer rule). */
	if(spawn)
		e->data.minecart.control_ready = p->occupied && p->ride_epoch != 0;

	vec3 delta = {
		p->pos[0] - e->pos[0],
		p->pos[1] - e->pos[1],
		p->pos[2] - e->pos[2],
	};
	for(int axis = 0; axis < 3; axis++) {
		e->pos[axis] = p->pos[axis];
		e->pos_old[axis] += delta[axis];
		e->network_pos[axis] = p->pos[axis];
		e->vel[axis] = p->vel[axis];
	}
	/* F4: when a same-epoch snapshot corrects the cart currently carrying
	 * the local player, shift both local-player position samples by the
	 * same authoritative delta and apply the inverse (-delta) to a
	 * camera-only correction vector so the first displayed camera sample
	 * is preserved.
	 *
	 * Lifecycle criterion 12: suppress rider-body
	 * and camera correction for the entire pending-leave lifetime. While
	 * a dismount intent is pending for this cart, the rider is logically
	 * detached; a stale or respawned occupied snapshot must not re-attach
	 * the body. Cart physics (pos/vel/network_pos above) are still
	 * installed unconditionally -- only the rider correction is gated. */
	if(!spawn && same_epoch && gstate.players[0].local_player
	   && e->data.minecart.occupied
	   && e->data.minecart.occupant_id == 0 /* local player */
	   && !(gstate.players[0].ride.pending
	        && !gstate.players[0].ride.desired_occupied
	        && gstate.players[0].ride.target_id == e->id)) {
		/* A restored ride may come entirely from spawn authority, without a
		 * client-authored intent. Once a snapshot creates presentation state,
		 * associate that state with this cart so a later target destroy clears
		 * the correction. Never overwrite a live transaction's target. */
		if(!gstate.players[0].ride.pending)
			gstate.players[0].ride.target_id = e->id;
		for(int axis = 0; axis < 3; axis++) {
			gstate.players[0].local_player->pos[axis] += delta[axis];
			gstate.players[0].local_player->pos_old[axis] += delta[axis];
		}
		/* F4 invariant 8 feedback guard: the
		 * stored camera_correction must equal the vector actually applied
		 * to gstate.players[0].camera for the whole inter-frame interval, and when no
		 * presentation offset is retained the stored camera must be hard-
		 * adjusted immediately (clin_update sends before the next
		 * camera_attach). With applied offset C and body delta D:
		 *   smoothable: retain C-D, stored camera unchanged;
		 *   deadband:   keep old tail C, shift stored camera by +D;
		 *   hard snap:  shift stored camera by D-C, clear offset;
		 * The wire then reads (camera+shift)-correction = old_pos + D. */
		vec3 C_prev;
		glm_vec3_copy(gstate.players[0].ride.camera_correction, C_prev);
		float dlen = glm_vec3_norm(delta);
		if(dlen > 0.50F) {
			/* Raw correction above 0.50 block: hard-snap. Shift stored
			 * camera by D-C so the pre-attach wire sample is correct,
			 * then clear the offset. */
			gstate.players[0].camera.x += delta[0] - C_prev[0];
			gstate.players[0].camera.y += delta[1] - C_prev[1];
			gstate.players[0].camera.z += delta[2] - C_prev[2];
			glm_vec3_zero(gstate.players[0].ride.camera_correction);
			gstate.players[0].ride.camera_correction_active = false;
		} else if(dlen > 0.015625F /* deadband 1/64 */) {
			/* In-range: accumulate the inverse delta. */
			glm_vec3_sub(gstate.players[0].ride.camera_correction,
						 delta, gstate.players[0].ride.camera_correction);
			float clen = glm_vec3_norm(gstate.players[0].ride.camera_correction);
			if(clen > 0.50F) {
				/* Accumulated correction above 0.50 block: hard-snap.
				 * Shift stored camera by D-C_prev (the original
				 * offset before accumulation), clear. */
				gstate.players[0].camera.x += delta[0] - C_prev[0];
				gstate.players[0].camera.y += delta[1] - C_prev[1];
				gstate.players[0].camera.z += delta[2] - C_prev[2];
				glm_vec3_zero(gstate.players[0].ride.camera_correction);
				gstate.players[0].ride.camera_correction_active = false;
			} else {
				/* Rebase: copy the new accumulated value into start
				 * so convergence runs from the currently displayed
				 * offset, not a stale original camera position. */
				glm_vec3_copy(gstate.players[0].ride.camera_correction,
							  gstate.players[0].ride.camera_correction_start);
				gstate.players[0].ride.camera_correction_active = true;
				gstate.players[0].ride.camera_correction_elapsed = 0.0F;
			}
		} else {
			/* At or below deadband: keep old tail C (do NOT fold D
			 * into the correction), shift stored camera by +D so the
			 * tiny correction is taken directly. If no tail is
			 * active, C is zero and this is just the body shift. */
			gstate.players[0].camera.x += delta[0];
			gstate.players[0].camera.y += delta[1];
			gstate.players[0].camera.z += delta[2];
		}
	}
	e->y_size = p->y_size;
	e->fall_distance = p->fall_distance;
	e->on_ground = p->on_ground;
	e->collided_horizontally = p->collided_horizontally;
	e->collided_vertically = p->collided_vertically;
	e->collided = p->collided_horizontally || p->collided_vertically;
	e->in_web = p->in_web;
	e->on_fire = p->on_fire;
	e->data.minecart.speed = p->speed;
	e->data.minecart.hx = p->hx;
	e->data.minecart.hz = p->hz;
	e->data.minecart.rail_meta = p->rail_meta;
	e->data.minecart.last_meta = p->last_meta;
	e->data.minecart.damage_taken = p->damage_taken;
	e->data.minecart.shake_ticks = p->shake_ticks;
	e->data.minecart.shake_dir = p->shake_dir;
	/* F4: occupancy single-writer after spawn. A periodic snapshot must
	 * not write occupied/occupant_id/rightClickText -- only the reliable
	 * CRPC_MINECART_OCCUPANCY path changes attachment state. On spawn
	 * (the initial install) these fields are set from the spawn payload;
	 * on non-spawn, they are left untouched. */
	if(spawn) {
		/* Immutable after spawn -- single-writer like occupied/occupant_id
		 * right below. rightClickText only derives from occupied for type 0;
		 * type 1/2 never mount (occupied stays false for them, always), so
		 * they get a fixed per-type label instead of the Ride/Dismount toggle.
		 * Mirrors server_local_spawn_minecart_type's own derivation exactly. */
		e->data.minecart.minecart_type = p->minecart_type;
		e->data.minecart.occupied = p->occupied;
		e->data.minecart.occupant_id = p->occupant_id;
		switch(p->minecart_type) {
		case 1: e->rightClickText = "Open"; break;
		case 2: e->rightClickText = "Use"; break;
		default: e->rightClickText = p->occupied ? "Dismount" : "Ride";
		}
	}
	e->data.minecart.pending_throttle = 0;
	e->data.minecart.replay_throttle = 0;

	if(replay_steps != 0)
		entity_minecart_stage_replay_control(e, replay_steps);
	return true;
}

void clin_process(struct client_rpc* call); /* defined below; header decl is
											 * test-seam-gated */

static uint32_t s_remote_world_generation;
void clin_force_world_reset(void) {
	/* Header comment has the why; clin_process is the same dispatch the
	 * inbox drain uses, so this stays byte-identical to a server-sent
	 * WORLD_RESET. Split-dims B1: the dead remote session's identity is
	 * retired here -- remote traffic before the next login stays unstamped
	 * (the pre-B1 login order), never stamped with the dead identity. */
	s_remote_world_generation = 0;
	struct client_rpc reset = {.type = CRPC_WORLD_RESET};
	clin_process(&reset);
}

/* S5.3 hunt fix (2026-08-27, root cause hardware-proven via
 * cw_boot_trace.txt/cw_cond_trace.txt/cw_mp_trace.txt over takes 6-12,
 * mp_connect_smoke): the MP login handler used to install the fresh remote
 * world via remote_try_crpc()/clin_try_rpc_send() -- the SAME
 * clin_inbox/clin_empty_msg tchannel every OTHER caller in this codebase
 * only ever produces into from the SERVER thread (clin_try_rpc_send's own
 * header comment: "blocking the SERVER thread here... is the server half of
 * a two-way bounded-buffer DEADLOCK", i.e. it was designed around producer
 * and consumer being two DIFFERENT threads). server_remote_pump() runs the
 * MP protocol dispatch on the MAIN/client thread -- the same thread that
 * drains clin_inbox via clin_update() a few lines later, THIS SAME FRAME.
 * Round 3 hardware evidence: bracketing every primitive inside
 * clin_try_rpc_send (recv on clin_empty_msg, send on clin_inbox) showed
 * NONE of them ever ran -- the run dies in the call transition into
 * remote_try_crpc()/clin_try_rpc_send() itself, right after the LOGIN
 * case's worldgen_create() (proven to return cleanly: post_worldgen_create_ok
 * logged every take) -- consistent with this call chain (main loop -> mp
 * block -> server_remote_pump -> process_event -> remote_try_crpc ->
 * clin_try_rpc_send) being deeper than any path exercised on Wii before.
 * clin_force_world_reset() above is the ALREADY-PROVEN main-thread-direct
 * entry for exactly this class of problem (S4, e246a79: the kick-path
 * world teardown hit the identical wrong-thread-target mistake --
 * SRPC_UNLOAD_WORLD to server_local instead of the client-side world --
 * and the fix was a first-class synchronous client-thread call that
 * bypasses ALL producer/consumer indirection). This is that same fix,
 * carrying the login path's payload (clin_force_world_reset() itself omits
 * it -- correct for its own disconnect-teardown-only caller, wrong for an
 * install that must set dimension/local_entity). Two stack frames
 * (remote_try_crpc, clin_try_rpc_send) and the whole tchannel/MQ path are
 * removed from this call site; clin_process is the exact same dispatch
 * either way, so client-side behavior is unchanged -- only the transport
 * from server_remote_protocol.c to it. */
/* Split-dims B1 remote stamping (B-NOW test_b1_reset_session_reuse): a
 * remote Beta server has no world generation of its own, so the client-thread
 * remote adapter's session identity is minted HERE -- once per remote world
 * install (login/reconnect, dimension-change respawn). Process-lifetime
 * monotonic, never 0, never reused: a reconnected session can never match a
 * dead session's queued traffic. Owner (main/client) thread only, like every
 * remote_* dispatch. */
static uint32_t s_remote_world_generation_mark;

uint32_t clin_remote_world_generation_mint(void) {
	/* Remote identities live in the REMOTE half of the namespace
	 * (WORLD_GENERATION_REMOTE_BASE): the first mint is the base itself and
	 * a wrap stays inside the half, so no remote identity can equal a local
	 * server world's (G6 S1). */
	if(++s_remote_world_generation_mark < WORLD_GENERATION_REMOTE_BASE)
		s_remote_world_generation_mark = WORLD_GENERATION_REMOTE_BASE;
	assert(world_generation_is_remote(s_remote_world_generation_mark));
	s_remote_world_generation = s_remote_world_generation_mark;
	return s_remote_world_generation;
}

uint32_t clin_remote_world_generation(void) {
	return s_remote_world_generation;
}

void clin_force_world_reset_login(enum world_dim dimension,
								  uint32_t local_entity) {
	/* The remote session's world install: a fresh identity bound by the
	 * reset (world slot 0 -- a remote session has one world). */
	struct client_rpc reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {
			.dimension = dimension,
			.local_entity = local_entity,
			.world_index = 0,
			.world_generation = clin_remote_world_generation_mint(),
		},
	};
	clin_process(&reset);
}

/* S5.3 hunt fix, round 3 cont. (2026-08-27, hardware-proven take 13): fixing
 * the LOGIN case's CRPC_WORLD_RESET alone moved the death one packet later
 * in the SAME login burst -- MP_EVENT_SPAWN_POSITION's handler
 * (server_remote_protocol.c) has the IDENTICAL bug: it also called
 * remote_try_crpc()/clin_try_rpc_send() from process_event on the
 * main/client thread, the exact wrong-thread-producer pattern
 * clin_force_world_reset_login's comment documents. CRPC_WORLD_SPAWN's
 * clin_process case is a trivial 3-field gstate write (world_spawn_x/y/z)
 * with no ordering dependency on the inbox drain, so the same direct-call
 * fix applies cleanly. */
void clin_force_world_spawn(w_coord_t x, w_coord_t y, w_coord_t z) {
	struct client_rpc call = {
		.type = CRPC_WORLD_SPAWN,
		.payload.world_spawn = {.x = x, .y = y, .z = z},
	};
	clin_process(&call);
}

/* S5.3 hunt fix, round 4 (2026-08-27): a GENERIC "caller is the client
 * thread -> bypass clin_inbox" branch was tried INSIDE clin_try_rpc_send
 * itself and reverted -- it broke 4 PC tests (test_near_evict_defer,
 * test_tnt_primed_env, test_minecart_authority, test_boat_env; see that
 * revert's comment on clin_try_rpc_send) because several test harnesses
 * legitimately call clin_try_rpc_send from the same OS thread clin_init()
 * ran on while deliberately exercising the channel's queueing/saturation
 * semantics -- a thread-identity check can't tell that apart from the MP
 * remote path's wrong-thread-producer bug (round 3's root-cause finding;
 * see clin_force_world_reset_login's comment above).
 *
 * This is the SAFE generalization instead: a thin, explicitly-named direct-
 * dispatch entry that ONLY the MP remote code (server_remote_protocol.c,
 * server_remote_chunks.c, server_remote_entities.c -- all confirmed to run
 * process_event/server_remote_pump on the main/client thread, never the PC
 * test harnesses above) opts into, call site by call site, exactly
 * mirroring clin_force_world_reset_login/clin_force_world_spawn's already-
 * hardware-proven pattern. clin_try_rpc_send itself, and every OTHER
 * existing caller (server_local.c's singleplayer server thread, the PC
 * test harnesses), is completely untouched. */
bool clin_apply_attach(struct entity* vehicle, bool attach, struct entity* player) {
	if(!vehicle)
		return false;
	bool* occupied;
	uint32_t* occupant;
	/* Beta box tops: boat setSize(1.5, 0.6) with yOffset height/2, minecart
	 * setSize(0.98, 0.7) likewise -- minY + height = posY + height/2.
	 * [740c583 EntityBoat.<init>, EntityMinecart.<init>, Entity.setSize] */
	float top_above_pos;
	if(vehicle->type == ENTITY_BOAT) {
		occupied = &vehicle->data.boat.occupied;
		occupant = &vehicle->data.boat.occupant_id;
		top_above_pos = 0.3F;
	} else if(vehicle->type == ENTITY_MINECART) {
		occupied = &vehicle->data.minecart.occupied;
		occupant = &vehicle->data.minecart.occupant_id;
		top_above_pos = 0.35F;
	} else if(vehicle->type == ENTITY_MONSTER) {
		/* MP-RIDE: a mob mount (the saddled pig: EntityPig.interact ->
		 * mountEntity) is keyed exactly as the local server's pig link,
		 * pig_ridden + pig_rider_id 0 + the host actor, which is what
		 * entity_local_player.c's PIG MOUNT FOLLOW rides on. A mob's posY is
		 * its feet (yOffset 0), so the box top is posY + height. */
		vehicle->data.monster.pig_ridden = attach;
		vehicle->data.monster.pig_rider_id = 0;
		memset(&vehicle->data.monster.pig_rider_actor, 0,
			   sizeof(vehicle->data.monster.pig_rider_actor));
		if(!attach && player) {
			player->pos[0] = vehicle->pos[0];
			player->pos[1] = vehicle->pos[1] + vehicle->height;
			player->pos[2] = vehicle->pos[2];
			glm_vec3_copy(player->pos, player->pos_old);
		}
		return true;
	} else
		return false;
	if(attach) {
		*occupied = true;
		*occupant = 0; /* SERVER_LOCAL_PLAYER_ENTITY_ID: the local player */
		vehicle->rightClickText = "Dismount";
		return true;
	}
	*occupied = false;
	*occupant = 0;
	vehicle->rightClickText = "Ride";
	if(player) {
		/* mountEntity(null): setLocationAndAngles(vehicle.posX,
		 * vehicle.boundingBox.minY + vehicle.height, vehicle.posZ) -- the
		 * player's posY is its eye, exactly as this engine's pos[1]. The
		 * server's own correction (sent right after its Packet39) then
		 * settles the exact spot. */
		player->pos[0] = vehicle->pos[0];
		player->pos[1] = vehicle->pos[1] + top_above_pos;
		player->pos[2] = vehicle->pos[2];
		glm_vec3_copy(player->pos, player->pos_old);
	}
	return true;
}

void clin_process_direct(struct client_rpc* call) {
	clin_process(call);
}

/* Beta's MP position apply for NON-LIVING entities: the base
 * Entity.setPositionAndRotation2 SNAPS to the wire position immediately,
 * then pops the entity up out of any colliding box (lift the feet to the
 * tallest colliding top); only EntityLiving overrides it with the
 * 3-increment approach the monster/remote-player ticks consume via
 * entity_network_approach_tick. [740c583
 * Entity.setPositionAndRotation2:1010-1027] [740c583
 * EntityLiving.setPositionAndRotation2:193-201]
 *
 * The colliding-box sweep mirrors entity_intersection's cell enumeration
 * (floor/ceil bounds, the fence -1 Y rim, per-cell multi-box
 * getBoundingBox) rather than reusing it: that helper is an early-exit
 * boolean and this needs the max colliding TOP, and extending entity.h
 * with a max-query variant is deferred while another lane holds that
 * header. Beta seeds the max at 0.0D (a colliding box wholly below y=0
 * contributes nothing) -- replicated exactly. */
static void remote_base_entity_snap(struct entity* e) {
	glm_vec3_copy(e->network_pos, e->pos);
	struct AABB box;
	entity_make_local_bbox(e, &box);
	aabb_translate(&box, e->pos[0], e->pos[1], e->pos[2]);
	/* boundingBox.contract(0.03125D, 0.0D, 0.03125D) */
	struct AABB probe = box;
	probe.x1 += 0.03125F;
	probe.x2 -= 0.03125F;
	probe.z1 += 0.03125F;
	probe.z2 -= 0.03125F;
	w_coord_t min_x = floorf(probe.x1);
	w_coord_t min_y = (w_coord_t)(floorf(probe.y1) - 1);
	if(min_y < 0)
		min_y = 0;
	w_coord_t min_z = floorf(probe.z1);
	w_coord_t max_x = ceilf(probe.x2) + 1;
	w_coord_t max_y = (w_coord_t)(ceilf(probe.y2) + 1);
	if(max_y > WORLD_HEIGHT - 1)
		max_y = WORLD_HEIGHT - 1;
	w_coord_t max_z = ceilf(probe.z2) + 1;
	float max_top = 0.0F; /* Beta: double var11 = 0.0D */
	bool colliding = false;
	for(w_coord_t x = min_x; x < max_x; x++)
		for(w_coord_t z = min_z; z < max_z; z++) {
			/* Beta gates each COLUMN on blockExists(x, 64, z): an
			 * unloaded chunk contributes no collision boxes. Load-bearing
			 * in this port -- world_get_block substitutes SOLID STONE for
			 * a missing client chunk (the streaming placeholder), which
			 * without this gate reads as a full-height collider and pops
			 * the entity skyward. [740c583
			 * World.getCollidingBoundingBoxes:968-978] */
			if(!world_find_chunk(e->world, x, 64, z))
				continue;
			for(w_coord_t y = min_y; y < max_y; y++) {
				struct block_data blk;
				if(!entity_get_block(e, x, y, z, &blk) || !blocks[blk.type])
					continue;
				struct block_info info = {.block = &blk,
										  .neighbours = NULL,
										  .x = x,
										  .y = y,
										  .z = z};
				struct block* b = blocks[blk.type];
				size_t count = b->getBoundingBox(&info, true, NULL);
				if(count == 0)
					continue;
				struct AABB bbox[count];
				b->getBoundingBox(&info, true, bbox);
				for(size_t k = 0; k < count; k++) {
					aabb_translate(bbox + k, x, y, z);
					if(aabb_intersection(&probe, bbox + k)) {
						colliding = true;
						if(bbox[k].y2 > max_top)
							max_top = bbox[k].y2;
					}
				}
			}
		}
	if(colliding)
		/* var3 += var11 - this.boundingBox.minY; setPosition(...) */
		e->pos[1] += max_top - box.y1;
}

void (*clin_column_observe_rpc)(const struct client_rpc*);
void (*clin_column_observe_before_rpc)(const struct client_rpc*);
void (*clin_column_observe_publish)(const struct client_rpc*);
void (*clin_column_observe_slot)(int, const void*);

static void clin_settle_rpc(enum settle_stage stage,
							const struct client_rpc* call, const void* token) {
	if(!client_settle_observe)
		return;
	if(call->type == CRPC_WORLD_RESET || call->type == CRPC_STREAM_RESET) {
		if(stage == SET_ENQUEUE || stage == SET_DEQUEUE)
			/* The SET_DEQUEUE arm retires the capture IN PLACE (it used to tear
			 * the observer down and re-arm it: a 143 KB MEM2 free + a strict
			 * re-alloc from inside the callback). Nothing is bracketed around
			 * it: this is the reset path, and the only thing left on it is the
			 * memset itself -- no MEM2 traffic, no drain, and deliberately no
			 * SD write (see cw_settle_retire_flush, which reports the memset's
			 * measured cost after the reset has completed). */
			SETTLE(client_settle_observe, SET_RESET, NULL, NULL,
				   stage == SET_ENQUEUE, 0, 0, 0);
	} else if(call->type == CRPC_CHUNK || call->type == CRPC_FAR_CHUNK) {
		bool far = call->type == CRPC_FAR_CHUNK;
		int32_t x = far ? call->payload.far_chunk.x : call->payload.chunk.x;
		int32_t z = far ? call->payload.far_chunk.z : call->payload.chunk.z;
		SETTLE(client_settle_observe, stage, NULL, token, 0, x / 16, z / 16,
			   far);
	}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static size_t s_test_destroy_batch_releases;
size_t clin_test_destroy_batch_releases(void) {
	return s_test_destroy_batch_releases;
}
void clin_test_destroy_batch_releases_reset(void) {
	s_test_destroy_batch_releases = 0;
}
#endif

/* S5: the ONLY release point for a transferred CRPC_ENTITY_DESTROY_BATCH id
 * array. Idempotent by construction (it clears the pointer it frees), so the
 * apply path, the tagged-route backstop in clin_process_queued and
 * clin_drain_discard can each call it while the array is freed exactly once. */
static void clin_entity_destroy_batch_payload_release(struct client_rpc* call) {
	if(!call || call->type != CRPC_ENTITY_DESTROY_BATCH
	   || !call->payload.entity_destroy_batch.ids)
		return;
	free(call->payload.entity_destroy_batch.ids);
	call->payload.entity_destroy_batch.ids = NULL;
	call->payload.entity_destroy_batch.count = 0;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	s_test_destroy_batch_releases++;
#endif
}

/* Exactly the pre-S5 CRPC_ENTITY_DESTROY body, extracted verbatim so the
 * batch applies the SAME per-id semantics (ride reset from the payload even
 * for an unknown id, arrow drr, pickup-flight deferral). */
static void clin_entity_destroy_one(uint32_t destroyed_id) {
	// dict_entity_get returns NULL for an unknown id -- the server can send
	// DESTROY for a spawn the client dropped on OOM, so guard the deref.
	/* F3/F4 criterion #13: if the destroyed entity is the ride-intent
	 * target, clear ride state through the centralized helper with
	 * preserve_seq=true so next_request_seq is not rewound within
	 * the same input epoch. Run this from the payload even when the
	 * entity-dict slot is absent (OOM drop) -- only free/erase
	 * belongs under the pp && *pp guard. The helper folds the active
	 * presentation vector out of the stored camera before clearing it. */
	if(destroyed_id == gstate.players[0].ride.target_id)
		minecart_ride_client_state_reset(&gstate.players[0].ride, &gstate.players[0].camera, true);
	/* O07 / R04 D1-1: a pending host ride action dies with its cart. */
	if(gstate.players[0].ride.action_latched
	   && destroyed_id == gstate.players[0].ride.action_cart_id)
		gstate.players[0].ride.action_latched = false;
	struct entity** pp = dict_entity_get(
		CW_ENTITIES, destroyed_id);
	if(pp && *pp) {
		/* MP visual-parity pass item 4: an item mid pickup-flight is NOT
		 * freed here -- entity_item.c's entity_client_tick self-reports
		 * removal (return true) once the flight's 3-tick age completes
		 * (entities_client_tick, entity.c, frees+erases exactly like this
		 * branch does). The entity IS still destroyed for every collector
		 * -- just deferred, not skipped -- so the ghost-item correctness
		 * fix (packet22-collect-ghost-fix-2026-08-27) stays intact. */
		/* MP client-side entity-hit drr: a remote arrow destroyed
		 * while still FLYING (its settle latch never fired) hit an
		 * entity -- Beta's server setDeads an entity-hit arrow and
		 * wires Packet29, while an embedded one persists; the MP
		 * client's own self-simulated copy would have played
		 * "random.drr" on that hit [740c583 EntityArrow.onUpdate].
		 * Wire-driven adaptation of that play, same formula as
		 * arrow_emit_drr; the settled/in_ground gate keeps stuck-
		 * arrow despawns and pickups silent. */
		if(gstate.remote_session && (*pp)->type == ENTITY_ARROW
		   && !(*pp)->data.arrow.in_ground
		   && (*pp)->data.arrow.client_moved)
			audio_play3d(SOUND_RANDOM_DRR,
						 (vec3) {(*pp)->pos[0], (*pp)->pos[1],
								 (*pp)->pos[2]},
						 1.0F,
						 1.2F
							 / (rand_gen_flt(audio_cosmetic_rng())
									* 0.2F
								+ 0.9F));
		if((*pp)->type == ENTITY_ITEM
		   && (*pp)->data.item.pickup_flight_active) {
			/* deferred: entity_client_tick reaps it. */
		} else {
			free(*pp);
			dict_entity_erase(CW_ENTITIES, destroyed_id);
		}
	}
}

/* Release whatever separately pooled payload an envelope carries, exactly as
 * the handler that consumes it would (idempotent for the batch array). Shared
 * by the headless discard and the world gate's drop path. */
static void clin_rpc_release_pooled(struct client_rpc* rpc) {
	if(rpc->type == CRPC_CHUNK)
		clin_chunk_slot_release(rpc->payload.chunk.slot);
	else if(rpc->type == CRPC_FAR_CHUNK)
		clin_far_slot_release(rpc->payload.far_chunk.slot);
	else if(rpc->type == CRPC_MAP_DATA)
		clin_map_slot_release(rpc->payload.map_data.slot);
	else if(rpc->type == CRPC_ENTITY_DESTROY_BATCH)
		/* S5: discarding the envelope still consumes the transferred id
		 * array -- releasing ownership without applying any destroy. */
		clin_entity_destroy_batch_payload_release(rpc);
	else if(rpc->type == CRPC_LOCAL_SETTLED_LIGHT) {
		column_light_publication_release(rpc->payload.settled_light);
		rpc->payload.settled_light = NULL;
	}
}

/* Split-dims B1: is this RPC type world-scoped, i.e. routed to a client world
 * context by its (world_index, world_generation) stamp before its handler may
 * touch any payload (out/B1-rpc-audit.md section 3, class WS)? Families are
 * enabled one per commit, in the audit's order; a type not listed here keeps
 * its pre-B1 route unchanged. Actor-routed (AO) and session-lifecycle (SESS)
 * types are never world-scoped: they carry their own identity, and
 * WORLD_RESET / STREAM_RESET are what create and reset the contexts. */
static bool clin_rpc_world_scoped(enum client_rpc_type type) {
	switch(type) {
		/* Sound + cosmetic particles (audit commit 2): ephemeral effects of
		 * the world that produced them; a message for any other world is
		 * dropped, never played into this one. */
		case CRPC_SOUND: case CRPC_SMOKE_PUFF: case CRPC_ENTITY_EXPLODE:
		case CRPC_WOLF_FX: case CRPC_WOLF_SPLASH: case CRPC_RECORD_EVENT:
		case CRPC_DROWN_BUBBLES: case CRPC_LIGHTNING_BOLT:
			return true;
		/* Entities (audit commit 3): every spawn and every tracked-entity
		 * update belongs to the world whose entity it is. Entity ids are
		 * session-global (D4), so routing needs no id remap. */
		case CRPC_SPAWN_ITEM: case CRPC_PICKUP_ITEM: case CRPC_ITEM_COUNT:
		case CRPC_SPAWN_MONSTER: case CRPC_SPAWN_PLAYER:
		case CRPC_SPAWN_MINECART: case CRPC_MINECART_SNAPSHOT:
		case CRPC_MINECART_OCCUPANCY: case CRPC_SPAWN_ARROW:
		case CRPC_SPAWN_PRIMED_TNT: case CRPC_PRIMED_TNT_CORRECTION:
		case CRPC_SPAWN_BOAT: case CRPC_BOAT_CORRECTION:
		case CRPC_SPAWN_PAINTING: case CRPC_SPAWN_FALLING_SAND:
		case CRPC_ENTITY_DESTROY: case CRPC_ENTITY_DESTROY_BATCH:
		case CRPC_ENTITY_MOVE: case CRPC_ENTITY_LOOK:
		case CRPC_ENTITY_VELOCITY: case CRPC_ENTITY_PHYSICS_CORRECTION:
		case CRPC_ENTITY_EQUIPMENT: case CRPC_ENTITY_ANIMATION:
		case CRPC_ENTITY_FLAGS: case CRPC_ATTACH_ENTITY:
			return true;
		/* Time and weather (audit commit 5): each world context owns its
		 * clock and weather (A2 decides the per-world server clocks; the
		 * client only routes what each world publishes). */
		case CRPC_TIME_SET: case CRPC_WEATHER_SET:
			return true;
		/* Terrain (audit commit 4): a stale-generation unload or block write
		 * is dropped, never applied to the successor's terrain; a context
		 * parked by a scoped reset has its terrain held by the drain loop. */
		case CRPC_CHUNK: case CRPC_FAR_CHUNK: case CRPC_UNLOAD_CHUNK:
		case CRPC_SET_BLOCK: case CRPC_PISTON_MOVING:
		case CRPC_LOCAL_SETTLED_LIGHT:
		case CRPC_SIGN_TEXT: case CRPC_SIGN_TEXT_REMOVED:
			return true;
		default: return false;
	}
}

/* Route one world-scoped envelope. Returns the verdict; the caller applies
 * APPLY, holds HOLD (drain loops only) and drops the rest. Non-world-scoped
 * types always APPLY and are not counted. */
static enum client_ctx_route clin_world_route(const struct client_rpc* call,
										  struct client_world_context** ctx) {
	*ctx = NULL;
	if(!clin_rpc_world_scoped(call->type)) return CLIENT_CTX_ROUTE_APPLY;
	/* Fable F3: pre-B1, TIME_SET / WEATHER_SET had no world gate, and on
	 * remote MP the first Packet4/Packet70 can arrive (clin_process_direct)
	 * before the login WORLD_RESET. Weather is not resent periodically, so
	 * dropping it would leave rain/thunder wrong until the next change.
	 * Keep the pre-B1 behaviour exactly: an UNSTAMPED clock/weather update
	 * that finds no client world context at all is applied, not dropped.
	 * Stamped traffic, and any update once a context exists, route normally. */
	if((call->type == CRPC_TIME_SET || call->type == CRPC_WEATHER_SET)
	   && call->world_generation == 0
	   && !client_ctx_registry_current(&s_world_ctx, 0)
	   && !client_ctx_registry_current(&s_world_ctx, 1))
		return CLIENT_CTX_ROUTE_APPLY;
	/* Split-dims B2 (flag ON): every live slot owns its own payload --
	 * terrain, entities, particles, signs, moving pistons, clock and
	 * weather -- so a message routed to ANY slot's current context applies
	 * into that slot's storage. */
	if(s_world_payloads_enabled)
		return client_ctx_registry_route(&s_world_ctx, call->world_index,
										 call->world_generation, ctx);
	/* Single payload (Fable F1): only the payload slot's context may apply. */
	return client_ctx_registry_route_payload(&s_world_ctx, s_world_payload_slot,
											 call->world_index,
											 call->world_generation, ctx);
}

struct client_ctx_registry* clin_world_contexts(void) {
	return &s_world_ctx;
}

/* Split-dims B1: the lease owner installed on gstate.world, the single client
 * world payload. Its current context is the one the last WORLD_RESET
 * installed (slot s_world_payload_slot). */

static enum client_ctx_lease_kind clin_lease_kind(enum world_lease_kind kind) {
	return kind == WORLD_LEASE_GPU ? CLIENT_CTX_LEASE_GPU
								   : CLIENT_CTX_LEASE_MESHER;
}

/* user NULL: the flag-OFF owner, following the single payload's slot (the
 * pre-B2 behaviour). Otherwise it points at the fixed slot index of the
 * world-bound storage it is installed on (flag ON). */
static unsigned clin_lease_slot(void* user) {
	return user ? *(const unsigned*)user : s_world_payload_slot;
}

static bool clin_world_lease_acquire(void* user, enum world_lease_kind kind,
									 struct world_lease* out) {
	struct client_world_context* ctx
		= client_ctx_registry_current(&s_world_ctx, clin_lease_slot(user));
	if(!ctx || !client_ctx_lease_acquire(ctx, clin_lease_kind(kind)))
		return false;
	*out = (struct world_lease) {
		.ctx = ctx, .epoch = ctx->generation, .held = true};
	return true;
}

static bool clin_world_lease_release(void* user, enum world_lease_kind kind,
									 struct world_lease* lease) {
	(void)user;
	if(!lease->held) return false;
	struct client_world_context* ctx = lease->ctx;
	bool current = client_ctx_registry_owns(&s_world_ctx, ctx)
		&& client_ctx_registry_current(&s_world_ctx, ctx->world_index) == ctx
		&& client_ctx_accept(ctx, ctx->world_index, lease->epoch);
	bool released
		= client_ctx_registry_release(&s_world_ctx, ctx, clin_lease_kind(kind));
	assert(released && "world lease released against a context that never granted it");
	(void)released;
	*lease = (struct world_lease) {0};
	return current;
}

static const struct world_lease_owner s_world_lease_owner = {
	.acquire = clin_world_lease_acquire,
	.release = clin_world_lease_release,
	.user = NULL,
};

static unsigned s_lease_slot_index[CLIENT_CTX_SLOT_COUNT] = {0u, 1u};
static const struct world_lease_owner s_world_lease_owner_slot[CLIENT_CTX_SLOT_COUNT] = {
	{.acquire = clin_world_lease_acquire,
	 .release = clin_world_lease_release,
	 .user = &s_lease_slot_index[0]},
	{.acquire = clin_world_lease_acquire,
	 .release = clin_world_lease_release,
	 .user = &s_lease_slot_index[1]},
};

/* The lease owner a WORLD_RESET installs on the storage it (re)fills. */
static const struct world_lease_owner* clin_lease_owner_for(unsigned slot) {
	if(!s_world_payloads_enabled || slot >= CLIENT_CTX_SLOT_COUNT)
		return &s_world_lease_owner;
	return &s_world_lease_owner_slot[slot];
}

/* ---- Split-dims B2: world-bound payload API (client_world_payload.h) ---- */

/* Free every entity in a payload store, dropping any local-player alias that
 * points into it (a body in the other store keeps its alias). */
static void clin_payload_drop_entities(struct dict_entity_s* store) {
	for(unsigned player = 0; player < CLIENT_LOCAL_PLAYERS; player++) {
		struct entity* body = gstate.players[player].local_player;
		if(!body) continue;
		struct entity** tracked = dict_entity_get(store, body->id);
		if(tracked && *tracked == body)
			gstate.players[player].local_player = NULL;
	}
	dict_entity_it_t it;
	for(dict_entity_it(it, store); !dict_entity_end_p(it); dict_entity_next(it))
		free(dict_entity_ref(it)->value);
	dict_entity_reset(store);
}

bool clin_world_payloads_enabled(void) {
	return s_world_payloads_enabled;
}

bool clin_world_payloads_set_enabled(bool enabled) {
	if(enabled == s_world_payloads_enabled)
		return true;
	/* Only between sessions: a live or draining context still references the
	 * storage mapping it was installed under. */
	if(client_ctx_registry_in_use(&s_world_ctx) != 0)
		return false;
	if(enabled) {
		struct clin_extra_payload* p = calloc(1, sizeof(*p));
		if(!p)
			return false;
		world_create(&p->terrain);
		dict_entity_init(p->entities);
		p->particles = particle_system_create(&p->terrain);
		p->signs = sign_text_store_create();
		p->pistons = client_piston_moving_store_create();
		if(!p->particles || !p->signs || !p->pistons) {
			if(p->particles) particle_system_destroy(p->particles);
			sign_text_store_destroy(p->signs);
			client_piston_moving_store_destroy(p->pistons);
			world_destroy(&p->terrain);
			dict_entity_clear(p->entities);
			free(p);
			return false;
		}
		s_extra_payload = p;
	} else {
		struct clin_extra_payload* p = s_extra_payload;
		/* No context in use => no mesher job or GPU hand-off holds a lease
		 * against this world, so nothing in flight references its chunks. */
		clin_payload_drop_entities(p->entities);
		dict_entity_clear(p->entities);
		particle_system_destroy(p->particles);
		sign_text_store_destroy(p->signs);
		client_piston_moving_store_destroy(p->pistons);
		world_destroy(&p->terrain);
		free(p);
		s_extra_payload = NULL;
	}
	for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++) {
		s_payload_ready[slot] = false;
		s_payload_install_failures[slot] = 0;
		s_payload_resend_pending[slot] = false;
		s_payload_resend_requested[slot] = false;
		s_payload_reinstall[slot].armed = false;
		s_payload_resend_inflight_gen[slot] = 0;
		s_payload_prepared[slot] = 0;
	}
	s_world_payloads_enabled = enabled;
	/* A2-T admission: reservations and a pending landing belong to the
	   session that made them. */
	clin_capacity_reset();
	clin_landing_cancel();
	return true;
}

bool clin_world_payload_of_slot(unsigned slot, struct clin_world_payload* out) {
	if(slot >= CLIENT_CTX_SLOT_COUNT || !out)
		return false;
	*out = (struct clin_world_payload) {
		.terrain = clin_world_of_slot(slot),
		.entities = clin_entities_of_slot(slot),
		.particles = clin_particles_of_slot(slot),
		.signs = clin_signs_of_slot(slot),
		.pistons = clin_pistons_of_slot(slot),
	};
	return true;
}

bool clin_world_payload_of_view(unsigned view, struct clin_world_payload* out) {
	if(view > 1)
		return false;
	for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++) {
		const struct client_world_context* ctx
			= client_ctx_registry_current(&s_world_ctx, slot);
		if(ctx && (ctx->bound_views & (1u << view)))
			return clin_world_payload_of_slot(slot, out);
	}
	return false;
}

int clin_world_view_slot(unsigned view) {
	if(view > 1)
		return -1;
	for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++) {
		const struct client_world_context* ctx
			= client_ctx_registry_current(&s_world_ctx, slot);
		if(ctx && (ctx->bound_views & (1u << view)))
			return (int)slot;
	}
	return -1;
}

/* A rollback END follows the server's source-column sends, not their client
 * installation. Keep only that mover frozen until the corrected source
 * position has a complete, full-resolution collision ring. A failed CHUNK
 * arms the existing scoped resend; its reset can clear that marker before
 * replacement columns arrive, so residency must be checked as well. */
static void clin_transfer_hold_release_restored(void) {
	for(unsigned owner = 0; owner < 2; owner++) {
		struct clin_transfer_hold_state* hold = &s_transfer_hold[owner];
		if(!hold->active || !hold->restore_end_pending
		   || hold->restore_radius < 1 || hold->restore_radius > 4)
			continue;
		if(owner == 1 && (!gstate.players[1].local_bound
			|| hold->generation != gstate.players[1].local_generation))
			continue;
		struct entity* body = gstate.players[owner].local_player;
		int slot = clin_world_view_slot(owner);
		if(!body || slot < 0 || s_payload_resend_pending[slot]
		   || s_payload_resend_requested[slot]
		   || s_payload_reinstall[slot].armed)
			continue;
		struct client_world_context* ctx
			= client_ctx_registry_current(&s_world_ctx, (unsigned)slot);
		if(!ctx)
			continue;
		struct world* terrain = clin_world_of_slot((unsigned)slot);
		w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(body->pos[0]));
		w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(body->pos[2]));
		bool installed = true;
		for(w_coord_t dz = -(w_coord_t)hold->restore_radius;
			dz <= (w_coord_t)hold->restore_radius && installed; dz++)
			for(w_coord_t dx = -(w_coord_t)hold->restore_radius;
				dx <= (w_coord_t)hold->restore_radius && installed; dx++) {
				struct world_section* section = dict_wsection_get(terrain->sections,
					SECTION_TO_ID(cx + dx, cz + dz));
				if(!section || section->is_far_lod) {
					installed = false;
					break;
				}
				for(unsigned cy = 0; cy < COLUMN_HEIGHT; cy++)
					if(!section->column[cy]) {
						installed = false;
						break;
					}
			}
		if(installed) {
			hold->active = false;
			hold->restore_end_pending = false;
			hold->restore_radius = 0;
			hold->completed_id = hold->active_id;
		}
	}
}

bool clin_world_payload_bind_view(unsigned slot, unsigned view) {
	struct client_world_context* ctx
		= client_ctx_registry_current(&s_world_ctx, slot);
	if(!ctx || view > 1)
		return false;
	for(unsigned other = 0; other < CLIENT_CTX_SLOT_COUNT; other++) {
		struct client_world_context* o
			= client_ctx_registry_current(&s_world_ctx, other);
		if(o && o != ctx)
			(void)client_ctx_unbind_view(o, view);
	}
	if(ctx->bound_views & (1u << view))
		return true;
	return client_ctx_bind_view(ctx, view);
}

unsigned clin_world_payload_primary_slot(void) {
	return s_world_payload_slot;
}

bool clin_world_payload_ready(unsigned slot) {
	if(slot >= CLIENT_CTX_SLOT_COUNT)
		return false;
	if(!s_world_payloads_enabled || slot == s_world_payload_slot)
		return s_world_reset_ready
			&& client_ctx_registry_current(&s_world_ctx, slot) != NULL;
	return s_payload_ready[slot];
}

uint32_t clin_world_payload_install_failures(unsigned slot) {
	return slot < CLIENT_CTX_SLOT_COUNT ? s_payload_install_failures[slot] : 0;
}

/* B2 gate 2 transport (lands with SRPC_WORLD_RESEND): queue the
 * world-addressed resend request; false (retry next frame) on backpressure. */
static bool clin_send_world_resend(unsigned world_index,
								   uint32_t world_generation) {
	struct server_rpc req = {.type = SRPC_WORLD_RESEND};
	req.payload.world_resend.world_index = (uint8_t)world_index;
	req.payload.world_resend.world_generation = world_generation;
	return svin_rpc_send(&req);
}

void clin_world_payload_set_resend_sender(clin_world_resend_sender sender) {
	s_world_resend_sender = sender;
}

bool clin_world_payload_resend_requested(unsigned slot) {
	return slot < CLIENT_CTX_SLOT_COUNT && s_payload_resend_requested[slot];
}

bool clin_world_payload_resend_pending(unsigned slot) {
	return slot < CLIENT_CTX_SLOT_COUNT && s_payload_resend_pending[slot];
}

bool clin_world_payload_clock(unsigned slot,
							  struct clin_world_payload_clock* out) {
	if(slot >= CLIENT_CTX_SLOT_COUNT || !out)
		return false;
	struct clin_clock_ref k = clin_clock_of_slot(slot);
	*out = (struct clin_world_payload_clock) {
		.world_time = *k.world_time,
		.world_time_start = *k.world_time_start,
		.raining = *k.raining,
		.thundering = *k.thundering,
		.rain_strength = *k.rain_strength,
		.thunder_strength = *k.thunder_strength,
	};
	return true;
}

static void clin_payload_release_slot(unsigned slot);
bool clin_world_payload_release(unsigned slot) {
	if(!s_world_payloads_enabled || slot >= CLIENT_CTX_SLOT_COUNT
	   || slot == s_world_payload_slot
	   || !client_ctx_registry_current(&s_world_ctx, slot))
		return false;
	clin_payload_release_slot(slot);
	return true;
}

static void clin_payload_release_slot(unsigned slot) {
	/* Retire first: from here on this slot's traffic is NO_CONTEXT/STALE and
	 * its in-flight completions may not publish. The retired context stays
	 * owned until its leases drain (reclaimed by the owner poll/release). */
	client_ctx_registry_retire(&s_world_ctx, slot);
	int prev = s_apply_slot;
	s_apply_slot = (int)slot;
	/* This payload's own moving-piston and sign-text stores go with it; the
	 * other payload's stores are separate objects and untouched. */
	clin_payload_clear_stores(slot);
	world_unload_all(CW_WORLD);
	particle_system_clear(clin_particles());
	clin_payload_drop_entities(CW_ENTITIES);
	s_apply_slot = prev;
	s_payload_ready[slot] = false;
	s_payload_resend_pending[slot] = false;
	s_payload_resend_requested[slot] = false;
	s_payload_reinstall[slot].armed = false;
	s_payload_prepared[slot] = 0;
}

/* ---- Split-dims B2 activation gates ------------------------------------- */
static void clin_guest_body_release(void); /* defined with the guest arm */

/* Bind `views` of the slot's current context. A view another current context
 * already holds is taken over only when `take` (the gate-1 guest move); a
 * retry never steals a view that moved on while it waited. */
static void clin_payload_bind_views(unsigned slot, uint8_t views, bool take) {
	struct client_world_context* ctx
		= client_ctx_registry_current(&s_world_ctx, slot);
	if(!ctx)
		return;
	for(unsigned v = 0; v < 2; v++) {
		if(!(views & (1u << v)))
			continue;
		bool held = false;
		for(unsigned other = 0; other < CLIENT_CTX_SLOT_COUNT && !take;
			other++) {
			const struct client_world_context* o
				= client_ctx_registry_current(&s_world_ctx, other);
			held = held || (o && o != ctx && (o->bound_views & (1u << v)));
		}
		if(!held)
			(void)clin_world_payload_bind_view(slot, v);
	}
}

/* Gate 1 completion: the guest slot's new context is installed and bound.
 * Its terrain leases against this slot's owner (Fable F3: only the primary
 * WORLD_RESET set lease_owner, so a slot never player 0's escaped B1 lease
 * accounting). */
static void clin_guest_world_installed(unsigned slot, int dimension) {
	struct world* terrain = clin_world_of_slot(slot);
	terrain->dimension = dimension;
	terrain->lease_owner = clin_lease_owner_for(slot);
	s_payload_install_failures[slot] = 0;
	s_payload_ready[slot] = true;
}

/* A2-T landing (blocker 3): see client_world_payload.h. */
static bool clin_reset_superseded(unsigned slot, uint32_t gen);
bool clin_world_payload_prepare_landing(unsigned slot, uint32_t generation,
										int dimension) {
	if(!s_world_payloads_enabled || slot >= CLIENT_CTX_SLOT_COUNT
	   || generation == 0)
		return false;
	const struct client_world_context* cur
		= client_ctx_registry_current(&s_world_ctx, slot);
	if(cur && cur->server_generation == generation) {
		/* player 0's / the partner's world, or prepared already. An existing
		 * UNVIEWED world of another slot is the landing's too: the COMMIT
		 * must join it, not rebuild it, so it is marked (not owned). */
		if(slot != s_world_payload_slot && !cur->bound_views
		   && s_payload_prepared[slot] != generation) {
			s_payload_prepared[slot] = generation;
			s_payload_prepared_installed[slot] = false;
		}
		return true;
	}
	if(s_payload_reinstall[slot].armed
	   && s_payload_reinstall[slot].generation == generation)
		return true; /* its install is being retried */
	if(slot == s_world_payload_slot || clin_reset_superseded(slot, generation)
	   || (cur && cur->bound_views)) {
		fprintf(stderr, "[client] landing prepare of slot %u gen %u refused\n",
				slot, (unsigned)generation);
		return false;
	}
	if(cur)
		clin_payload_release_slot(slot); /* an unviewed older world */
	s_payload_prepared[slot] = generation;
	s_payload_prepared_installed[slot] = true;
	if(!client_ctx_registry_install(&s_world_ctx, dimension, slot,
									generation)) {
		s_payload_install_failures[slot]++;
		s_payload_reinstall[slot] = (struct clin_payload_reinstall) {
			.armed = true, .guest = true, .take = false,
			.dimension = dimension, .generation = generation, .views = 0};
		fprintf(stderr, "[client] landing context install failed (%u in use);"
						" retrying\n",
				client_ctx_registry_in_use(&s_world_ctx));
		return true;
	}
	clin_guest_world_installed(slot, dimension);
	return true;
}

void clin_world_payload_unprepare_landing(unsigned slot, uint32_t generation) {
	if(!s_world_payloads_enabled || slot >= CLIENT_CTX_SLOT_COUNT
	   || generation == 0 || s_payload_prepared[slot] != generation)
		return;
	if(s_payload_reinstall[slot].armed
	   && s_payload_reinstall[slot].generation == generation
	   && !s_payload_reinstall[slot].views) {
		s_payload_reinstall[slot].armed = false;
		s_payload_prepared[slot] = 0;
		return;
	}
	const struct client_world_context* cur
		= client_ctx_registry_current(&s_world_ctx, slot);
	if(!cur || cur->server_generation != generation || cur->bound_views
	   || slot == s_world_payload_slot || !s_payload_prepared_installed[slot]) {
		s_payload_prepared[slot] = 0;
		return;
	}
	struct clin_world_payload p;
	if(clin_world_payload_of_slot(slot, &p) && p.entities)
		for(unsigned player = 0; player < CLIENT_LOCAL_PLAYERS; player++) {
			const struct entity* body = gstate.players[player].local_player;
			struct entity** tracked
				= body ? dict_entity_get(p.entities, body->id) : NULL;
			if(tracked && *tracked == body) {
				s_payload_prepared[slot] = 0;
				return;
			}
		}
	clin_payload_release_slot(slot); /* clears s_payload_prepared */
}

uint32_t clin_world_payload_prepared(unsigned slot) {
	return slot < CLIENT_CTX_SLOT_COUNT ? s_payload_prepared[slot] : 0;
}

/* Gate 1: an IN-PLACE reset of the guest's world. The producer names the
 * slot, its new server generation and, for the guest's FIRST move into a slot
 * it does not view yet (the slot may be empty), guest_view = the view to
 * bind (Fable F1). Only that slot's payload is torn down and re-bound; player
 * 0's world, body, windows, screens and load state are untouched. A guest
 * body in that store is freed with it and the guest's gameplay_ready is
 * cleared: the server re-issues LOCAL_GUEST_GAMEPLAY_READY, whose body
 * install lands in the store of the slot the guest view shows (Fable F2,
 * clin_guest_body_install). Refused unless world-bound payloads are ON, the
 * reset is stamped, names a slot other than player 0's, and a guest view
 * shows that slot or is named by guest_view. */
/* Gate 1, join variant: the guest moves back INTO player 0's world. The
 * producer sends the in-place guest reset naming player 0's slot, its
 * CURRENT generation and guest_view = 1. Nothing is re-generated: view 1 is
 * bound to player 0's context and the guest's previous world (any other slot
 * view 1 showed, or one whose install is still being retried for it) is
 * released with the guest body in it. The guest's gameplay_ready is cleared;
 * the server re-issues LOCAL_GUEST_GAMEPLAY_READY and the body installs into
 * player 0's store. Refused (flag OFF, other generation, no primary context,
 * guest_view != 1); a repeat when view 1 already shows player 0's world and
 * no other slot is held for the guest is a no-op. */
/* O10 / R05 D2-1b: returns whether the join was ACCEPTED (an "already
 * there" repeat is accepted; an identity refusal is not). */
static bool clin_guest_join_primary(const struct client_rpc* call) {
	const unsigned slot = call->payload.world_reset.world_index;
	const uint32_t gen = call->payload.world_reset.world_generation;
	struct client_world_context* primary
		= client_ctx_registry_current(&s_world_ctx, slot);
	if(!primary || primary->server_generation != gen
	   || call->payload.world_reset.guest_view != 1) {
		fprintf(stderr, "[client] guest join of player 0's world dropped "
						"(slot %u gen %u)\n", slot, (unsigned)gen);
		return false;
	}
	bool moved = false;
	for(unsigned other = 0; other < CLIENT_CTX_SLOT_COUNT; other++) {
		if(other == slot)
			continue;
		const struct client_world_context* o
			= client_ctx_registry_current(&s_world_ctx, other);
		if((o && (o->bound_views & 2u))
		   || (s_payload_reinstall[other].armed
			   && (s_payload_reinstall[other].views & 2u))) {
			clin_payload_release_slot(other);
			moved = true;
		}
	}
	if(!moved && (primary->bound_views & 2u))
		return true; /* already there */
	if(gstate.players[1].local_bound)
		gstate.players[1].local_gameplay_ready = false;
	(void)clin_world_payload_bind_view(slot, 1);
	return true;
}

/* Fable B2-A2T F4: a stamped reset naming an OLDER generation than the
 * slot's live context is superseded -- applying it would tear down the newer
 * world. Generations only order within one minter's half (local server
 * generations below 0x80000000, remote session identities at or above it --
 * the G6 S1 namespace), so only same-half generations are compared. Flag OFF:
 * never superseded. */
static bool clin_reset_superseded(unsigned slot, uint32_t gen) {
	if(!s_world_payloads_enabled || gen == 0 || slot >= CLIENT_CTX_SLOT_COUNT)
		return false;
	const struct client_world_context* cur
		= client_ctx_registry_current(&s_world_ctx, slot);
	if(!cur || cur->server_generation == 0)
		return false;
	const uint32_t half = UINT32_C(0x80000000);
	return (cur->server_generation & half) == (gen & half)
		&& gen < cur->server_generation;
}

/* Fable B2-A2T F1 (client half; lane A's CRPC_WORLD_RELEASE): the server
 * releases a world the client no longer shows -- typically the one the host
 * vacated with no guest in it. Stamped {world_index, world_generation};
 * honoured only with world-bound payloads ON, for a slot that is not player
 * 0's, whose CURRENT context is exactly that generation, is shown by no view
 * and holds no local player's body. The slot's payload is then released
 * (terrain, entities, signs, pistons, particles; the context retires with any
 * leases) -- identically for a drained LIVE world (retiring false) and a
 * retiring one (retiring true); a retiring world additionally drops the
 * admission state tied to it. Anything else is ignored and logged: a stale or
 * foreign release can never take down a world someone is looking at. */
/* split_dims_6m evidence: world releases this client APPLIED (the slot's
 * payload really freed), and the last applied slot (-1 = none). Main thread
 * only; telemetry, never read back. */
static uint32_t s_world_release_applied;
static int s_world_release_last_slot = -1;
uint32_t clin_world_release_applied(int* last_slot) {
	if(last_slot)
		*last_slot = s_world_release_last_slot;
	return s_world_release_applied;
}

static void clin_world_release(const struct client_rpc* call) {
	if(!s_world_payloads_enabled)
		return;
	const unsigned slot = call->world_index;
	const uint32_t gen = call->world_generation;
	const struct client_world_context* ctx = slot < CLIENT_CTX_SLOT_COUNT
		? client_ctx_registry_current(&s_world_ctx, slot) : NULL;
	const char* why = NULL;
	if(!ctx || gen == 0 || ctx->server_generation != gen)
		why = "not the slot's live generation";
	else if(slot == s_world_payload_slot)
		why = "player 0's world";
	else if(ctx->bound_views)
		why = "a view still shows it";
	else {
		struct clin_world_payload p;
		if(clin_world_payload_of_slot(slot, &p) && p.entities)
			for(unsigned player = 0; player < CLIENT_LOCAL_PLAYERS; player++) {
				const struct entity* body = gstate.players[player].local_player;
				if(!body)
					continue;
				struct entity** tracked = dict_entity_get(p.entities, body->id);
				if(tracked && *tracked == body)
					why = "a local player's body lives in it";
			}
	}
	if(why) {
		fprintf(stderr, "[client] world release slot %u gen %u (%s) ignored: "
				"%s\n", slot, (unsigned)gen,
				call->payload.world_release.retiring ? "retiring" : "drained",
				why);
		return;
	}
	clin_payload_release_slot(slot);
	clin_admission_world_released(slot, gen,
		call->payload.world_release.retiring);
	s_world_release_applied++;
	s_world_release_last_slot = (int)slot;
}

/* O07 / R04 D2-1: the newest server guest-binding sequence this client has
 * applied (CRPC_WORLD_RESET guest_world + stamped GAMEPLAY_READY carry it;
 * server_local_guest_bind_seq). A live world identity proves the world
 * exists, not that the guest still belongs to it: a guest reset or READY
 * whose binding is OLDER than one already applied can never move the guest.
 * 0 = unstamped (legacy producers: no fence). Main thread only. */
static uint32_t s_guest_bind_seq;
/* O10 / R05 P3 wrap: the producer (server_local_guest_bind_seq_bump) counts
 * 1..UINT32_MAX and wraps to 1, skipping the unstamped 0. Sequences are
 * ordered by serial-number arithmetic (RFC 1982 style): b is AFTER a iff
 * (int32_t)(b - a) > 0, so the successor of UINT32_MAX (1) is newer and the
 * fence never closes. Valid while live bindings stay within 2^31 of each
 * other -- one bump per guest admission/transfer, never approached. */
static bool clin_bind_seq_after(uint32_t b, uint32_t a) {
	return (int32_t)(b - a) > 0;
}
static bool clin_guest_bind_stale(uint32_t seq) {
	return seq != 0 && s_guest_bind_seq != 0
		&& clin_bind_seq_after(s_guest_bind_seq, seq);
}
static void clin_guest_bind_applied(uint32_t seq) {
	if(seq != 0 && (s_guest_bind_seq == 0
					|| clin_bind_seq_after(seq, s_guest_bind_seq)))
		s_guest_bind_seq = seq;
}
/* O10 / R05 D2-1a: the last ACCEPTED guest-world reset (its sequence and
 * the guest-only world it bound). A redelivery of exactly that reset while
 * the guest still views that world -- or its install is still being retried
 * -- is a duplicate: applying it again would release the installed world
 * with the body and terrain in it. */
static struct {
	uint32_t seq;
	unsigned slot;
	uint32_t gen;
} s_guest_reset_applied;
static bool clin_guest_reset_duplicate(unsigned slot, uint32_t gen,
	uint32_t seq) {
	if(seq == 0 || seq != s_guest_reset_applied.seq
	   || slot != s_guest_reset_applied.slot
	   || gen != s_guest_reset_applied.gen || slot >= CLIENT_CTX_SLOT_COUNT)
		return false;
	const struct client_world_context* cur
		= client_ctx_registry_current(&s_world_ctx, slot);
	if(cur && cur->server_generation == gen && (cur->bound_views & 2u))
		return true;
	return s_payload_reinstall[slot].armed && s_payload_reinstall[slot].guest
		&& s_payload_reinstall[slot].generation == gen
		&& (s_payload_reinstall[slot].views & 2u);
}
static void clin_guest_reset_accepted(unsigned slot, uint32_t gen,
	uint32_t seq) {
	clin_guest_bind_applied(seq);
	if(seq != 0) {
		s_guest_reset_applied.seq = seq;
		s_guest_reset_applied.slot = slot;
		s_guest_reset_applied.gen = gen;
	}
}

static void clin_guest_world_reset(const struct client_rpc* call) {
	/* O07 / R04 D2-1: a guest reset older than the newest guest binding
	 * already applied can never move the guest back (a replayed reset to a
	 * still-live world is NOT current ownership). Unstamped: no fence. */
	if(clin_guest_bind_stale(call->payload.world_reset.guest_bind_seq)) {
		fprintf(stderr, "[client] guest-world reset bind seq %u older than "
						"%u; dropped\n",
				(unsigned)call->payload.world_reset.guest_bind_seq,
				(unsigned)s_guest_bind_seq);
		return;
	}
	/* O10 / R05 D2-1b: the fence advances only once the reset is ACCEPTED
	 * (below, per branch); a refused reset changes nothing. */
	const uint32_t bind_seq = call->payload.world_reset.guest_bind_seq;
	const unsigned slot = call->payload.world_reset.world_index;
	const uint32_t gen = call->payload.world_reset.world_generation;
	const unsigned view = call->payload.world_reset.guest_view;
	const int dimension = (int)call->payload.world_reset.dimension;
	struct client_world_context* cur = slot < CLIENT_CTX_SLOT_COUNT
		? client_ctx_registry_current(&s_world_ctx, slot) : NULL;
	uint8_t guest_views = cur ? (uint8_t)(cur->bound_views & ~1u) : 0;
	if(view > 0 && view < 2)
		guest_views |= (uint8_t)(1u << view);
	if(clin_reset_superseded(slot, gen)) {
		fprintf(stderr, "[client] guest-world reset for slot %u gen %u is older "
						"than the live world; dropped\n", slot, (unsigned)gen);
		return;
	}
	if(s_world_payloads_enabled && gen != 0 && view != 0
	   && slot == s_world_payload_slot) {
		if(clin_guest_join_primary(call))
			clin_guest_reset_accepted(slot, gen, bind_seq);
		return;
	}
	if(!s_world_payloads_enabled || gen == 0 || slot >= CLIENT_CTX_SLOT_COUNT
	   || slot == s_world_payload_slot || view >= 2 || !guest_views) {
		fprintf(stderr, "[client] guest-world reset dropped (slot %u gen %u "
						"view %u)\n", slot, (unsigned)gen, view);
		return;
	}
	if(clin_guest_reset_duplicate(slot, gen, bind_seq)) {
		fprintf(stderr, "[client] guest-world reset slot %u gen %u seq %u is "
						"a duplicate of the installed binding; kept\n", slot,
				(unsigned)gen, (unsigned)bind_seq);
		return;
	}
	/* A2-T landing (blocker 3): the guest COMMITs into the world its landing
	 * prepared -- that context already holds the arrival columns at exactly
	 * this generation. Bind view 1 to it; nothing is re-generated. */
	if(view == 1 && cur && cur->server_generation == gen
	   && s_payload_prepared[slot] == gen && !cur->bound_views
	   && s_payload_ready[slot]) {
		if(gstate.players[1].local_player)
			clin_guest_body_release();
		if(gstate.players[1].local_bound)
			gstate.players[1].local_gameplay_ready = false;
		clin_payload_bind_views(slot, 2u, true);
		s_payload_prepared[slot] = 0;
		clin_guest_reset_accepted(slot, gen, bind_seq);
		return;
	}
	/* Accepted from here on: an install that fails below is DEFERRED (the
	 * reinstall marker retries this exact binding), not refused. */
	clin_guest_reset_accepted(slot, gen, bind_seq);
	clin_payload_release_slot(slot);
	/* The guest's body went with its store -- or, on its first move, it lives
	 * in another store (player 0's) and still holds its alias: release it
	 * too, so the re-READY installs a fresh body into this slot instead of
	 * finding one HELD in player 0's world (Fable R2-1). Its gameplay waits
	 * for the server's re-READY. */
	if(gstate.players[1].local_player)
		clin_guest_body_release();
	if(gstate.players[1].local_bound)
		gstate.players[1].local_gameplay_ready = false;
	if(!client_ctx_registry_install(&s_world_ctx, dimension, slot, gen)) {
		s_payload_install_failures[slot]++;
		s_payload_reinstall[slot] = (struct clin_payload_reinstall) {
			.armed = true, .guest = true, .take = true,
			.dimension = dimension, .generation = gen, .views = guest_views};
		fprintf(stderr, "[client] guest-world context install failed (%u in "
						"use); retrying\n",
				client_ctx_registry_in_use(&s_world_ctx));
		return;
	}
	clin_payload_bind_views(slot, guest_views, true);
	clin_guest_world_installed(slot, dimension);
}

/* Gate 2 confirmation: a CRPC_STREAM_RESET stamped for a NON-primary world,
 * the answer to its world-addressed resend request. Honoured only while that
 * request is outstanding for exactly this generation (Fable F5: an
 * unsolicited or stale-stamped reset is ignored). Voxel-only, like the
 * primary re-stream: that payload's terrain, moving pistons, sign text and
 * particles are released and a SUCCESSOR context bound to the same identity
 * (fresh epoch, same views) takes the new stream at once; its predecessor
 * retires with any in-flight leases. Entities stay. The marker clears -- but
 * NOT when the successor cannot be installed: then the install is retried and
 * a fresh request follows it (Fable F4). */
static void clin_world_stream_reset(const struct client_rpc* call) {
	const unsigned slot = call->world_index;
	struct client_world_context* ctx
		= client_ctx_registry_current(&s_world_ctx, slot);
	if(!ctx || ctx->server_generation != call->world_generation
	   || !s_payload_resend_requested[slot]) {
		fprintf(stderr, "[client] world %u stream reset (gen %u) ignored: no "
						"request outstanding\n", slot,
				(unsigned)call->world_generation);
		return;
	}
	s_payload_resend_inflight_gen[slot] = 0;
	int dimension = ctx->dimension;
	uint8_t views = ctx->bound_views;
	int prev = s_apply_slot;
	s_apply_slot = (int)slot;
	clin_payload_clear_stores(slot);
	world_unload_all(CW_WORLD);
	particle_system_clear(clin_particles());
	s_apply_slot = prev;
	s_payload_resend_requested[slot] = false;
	if(!client_ctx_registry_install(&s_world_ctx, dimension, slot,
									call->world_generation)) {
		s_payload_install_failures[slot]++;
		s_payload_reinstall[slot] = (struct clin_payload_reinstall) {
			.armed = true, .guest = false, .dimension = dimension,
			.generation = call->world_generation, .views = views};
		fprintf(stderr, "[client] world %u re-stream context install failed "
						"(%u in use); marker kept, retrying\n", slot,
				client_ctx_registry_in_use(&s_world_ctx));
		return; /* s_payload_resend_pending stays raised */
	}
	clin_payload_bind_views(slot, views, false);
	s_payload_resend_pending[slot] = false;
	s_payload_install_failures[slot] = 0;
}

/* Retry one recorded install. Success: gate 1 -> the slot is ready; gate 2 ->
 * the marker (still raised) sends a fresh request below, since columns that
 * arrived while the slot had no context were dropped. */
static void clin_payload_reinstall_retry(unsigned slot) {
	struct clin_payload_reinstall* r = &s_payload_reinstall[slot];
	if(slot == s_world_payload_slot) {
		r->armed = false; /* player 0's WORLD_RESET owns the slot now */
		return;
	}
	if(!client_ctx_registry_install(&s_world_ctx, r->dimension, slot,
									r->generation))
		return; /* still no room: next frame */
	r->armed = false;
	clin_payload_bind_views(slot, r->views, r->take);
	if(r->guest) {
		clin_guest_world_installed(slot, r->dimension);
	} else {
		s_payload_install_failures[slot] = 0;
		s_payload_resend_pending[slot] = true;
	}
}

/* Once per frame: retry recorded installs, then offer each raised,
 * not-yet-accepted marker to the transport's sender, naming the world. No-op
 * with the flag OFF; no requests with no sender. */
static void clin_world_resend_poll(void) {
	if(!s_world_payloads_enabled)
		return;
	for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++)
		if(s_payload_reinstall[slot].armed)
			clin_payload_reinstall_retry(slot);
	if(!s_world_resend_sender)
		return;
	for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++) {
		if(!s_payload_resend_pending[slot] || s_payload_resend_requested[slot])
			continue;
		const struct client_world_context* ctx
			= client_ctx_registry_current(&s_world_ctx, slot);
		if(!ctx || ctx->server_generation == 0)
			continue; /* an unstamped world cannot be addressed */
		if(s_world_resend_sender(slot, ctx->server_generation)) {
			s_payload_resend_requested[slot] = true;
			s_payload_resend_inflight_gen[slot] = ctx->server_generation;
		}
	}
}

bool clin_world_payload_memory(unsigned slot,
							   struct clin_world_payload_memory* out) {
	if(slot >= CLIENT_CTX_SLOT_COUNT || !out)
		return false;
	*out = (struct clin_world_payload_memory) {0};
	world_memory_usage(clin_world_of_slot(slot), &out->terrain);
	const struct dict_entity_s* e = clin_entities_of_slot(slot);
	size_t dict_bytes = 0;
	if(e->index)
		dict_bytes += ((size_t)e->mask + 1u) * sizeof(*e->index);
	if(e->data)
		dict_bytes += (size_t)e->freelist_cap * sizeof(*e->data);
	out->entities = dict_entity_size(e);
	out->entity_mem1 = dict_bytes + out->entities * sizeof(struct entity);
	out->particle_mem2 = particle_system_mem2_bytes(clin_particles_of_slot(slot));
	out->fixed_mem1 = clin_storage_is_extra(slot)
		? sizeof(struct clin_extra_payload) + particle_system_struct_bytes()
		: 0;
	out->mem1_bytes = out->fixed_mem1 + out->terrain.mem1_bytes + out->entity_mem1;
	out->mem2_bytes = out->terrain.mem2_bytes + out->particle_mem2;
	return true;
}

/* The slot rendered by `view`: the slot whose CURRENT context has it bound,
 * else player 0's slot. Flag OFF: always the primary (gstate storage). */
static unsigned clin_view_slot(unsigned view) {
	if(s_world_payloads_enabled)
		for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++) {
			const struct client_world_context* ctx
				= client_ctx_registry_current(&s_world_ctx, slot);
			if(ctx && (ctx->bound_views & (1u << view)))
				return slot;
		}
	return s_world_payload_slot;
}

bool clin_world_view_enter(unsigned view, struct clin_view_world* out) {
	if(!out || view > 1)
		return false;
	return clin_world_slot_enter(clin_view_slot(view), out);
}

unsigned clin_world_live_slots(unsigned out[2]) {
	unsigned n = 0;
	out[n++] = s_world_payload_slot;
	if(s_world_payloads_enabled)
		for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++)
			if(slot != s_world_payload_slot
			   && client_ctx_registry_current(&s_world_ctx, slot))
				out[n++] = slot;
	return n;
}

bool clin_world_slot_enter(unsigned slot, struct clin_view_world* out) {
	if(!out || slot >= CLIENT_CTX_SLOT_COUNT)
		return false;
	*out = (struct clin_view_world) {
		.terrain = clin_world_of_slot(slot),
		.entities = clin_entities_of_slot(slot),
		.slot = slot,
	};
	out->dimension = out->terrain->dimension;
	(void)clin_world_payload_clock(slot, &out->clock);
	if(!s_world_payloads_enabled)
		return true; /* the gstate objects, nothing re-selected */
	out->selected = true;
	out->prev_particles = particle_system_select(clin_particles_of_slot(slot));
	out->prev_signs = sign_text_store_select(clin_signs_of_slot(slot));
	out->prev_pistons
		= client_piston_moving_store_select(clin_pistons_of_slot(slot));
	out->prev_terrain = world_view_current();
	out->prev_entities = world_view_entities();
	world_view_set(out->terrain, out->entities);
	return true;
}

void clin_world_view_leave(struct clin_view_world* scope) {
	if(!scope || !scope->selected)
		return;
	particle_system_select(scope->prev_particles);
	sign_text_store_select(scope->prev_signs);
	client_piston_moving_store_select(scope->prev_pistons);
	world_view_set(scope->prev_terrain, scope->prev_entities);
	scope->selected = false;
}

/* Fable R2-2 (minimum): the audio listener is player 0's (view 0). A sound
 * applied into a payload that view 0 does not show is re-expressed relative
 * to the camera of a view that DOES show it -- moved by (camera0 - cameraV) --
 * so the listener hears it at that view's distance and pan; with no view
 * bound to that world it is dropped. Flag OFF: always played as is. */
static bool clin_sound_position(const float in[3], vec3 out) {
	out[0] = in[0];
	out[1] = in[1];
	out[2] = in[2];
	if(!s_world_payloads_enabled)
		return true;
	const struct client_world_context* ctx
		= client_ctx_registry_current(&s_world_ctx, clin_apply_slot());
	if(!ctx || !ctx->bound_views)
		return false;
	if(ctx->bound_views & 1u)
		return true;
	for(unsigned v = 1; v < 2 && v < CLIENT_LOCAL_PLAYERS; v++)
		if(ctx->bound_views & (1u << v)) {
			const struct camera* cv = &gstate.players[v].camera;
			const struct camera* c0 = &gstate.players[0].camera;
			out[0] += c0->x - cv->x;
			out[1] += c0->y - cv->y;
			out[2] += c0->z - cv->z;
			return true;
		}
	return false;
}

void clin_world_payload_sync_particle_cameras(void) {
	if(!s_world_payloads_enabled)
		return;
	/* Fable B2 F4: main.c sets only the PRIMARY particle system's camera. A
	 * slot-1 system culls spawns against the camera of the (lowest) view its
	 * current context is bound to; with no bound view it keeps its last. */
	for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++) {
		/* Both storages when ON: slot 0's (primary static) system follows
		 * ITS bound view too -- the guest's when player 0 is in slot 1. */
		const struct client_world_context* ctx
			= client_ctx_registry_current(&s_world_ctx, slot);
		if(!ctx)
			continue;
		for(unsigned v = 0; v < 2 && v < CLIENT_LOCAL_PLAYERS; v++)
			if(ctx->bound_views & (1u << v)) {
				const struct camera* cam = &gstate.players[v].camera;
				particle_system_set_camera(clin_particles_of_slot(slot),
					(vec3) {cam->x, cam->y, cam->z});
				break;
			}
	}
}

bool clin_world_quiesce_pending(void) {
	for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++) {
		const struct client_world_context* ctx
			= client_ctx_registry_current(&s_world_ctx, slot);
		if(ctx && ctx->quiescing) return true;
	}
	return false;
}

bool clin_world_message_held(void) {
	return s_world_held_call != NULL;
}

static void clin_process_observed(struct client_rpc* call);
void clin_process(struct client_rpc* call) {
	/* Split-dims B1 world gate. A drain loop already routed (and did not
	 * hold) this envelope; every other caller (clin_process_direct, the
	 * login/force resets) routes here. A dropped envelope is bracketed by the
	 * observers exactly like the handlers' own "no world installed" drops
	 * (it reaches no handler, and its pooled payload is released), so the
	 * settle/column observers see the same boundary either way. HOLD cannot
	 * be honoured outside a drain loop -- the direct path has no queue to keep
	 * the message in. It is unreachable there (only a scoped STREAM_RESET
	 * quiesces, and no direct-path provider sends one), so it is dropped and
	 * logged rather than applied into a quiescing context. */
	enum client_ctx_route verdict;
	struct client_world_context* routed_ctx;
	if(s_world_routed_call == call) {
		verdict = s_world_routed_verdict;
		routed_ctx = s_world_routed_ctx;
		s_world_routed_call = NULL;
		s_world_routed_ctx = NULL;
	} else {
		verdict = clin_world_route(call, &routed_ctx);
	}
	if(client_settle_observe) {
		s_settle_install_call = call;
		clin_settle_rpc(SET_INSTALL_BEGIN, call, call);
	}
	/* S5: CRPC_ENTITY_DESTROY_BATCH is an ENVELOPE, not a client event -- its
	 * handler emits one observed CRPC_ENTITY_DESTROY event per carried id, so
	 * bracketing the envelope too would double-count the whole batch as an
	 * extra boundary. Every other type is exactly one event. */
	bool observe_envelope = call->type != CRPC_ENTITY_DESTROY_BATCH;
	if(observe_envelope && clin_column_observe_before_rpc)
		clin_column_observe_before_rpc(call);
	if(verdict == CLIENT_CTX_ROUTE_APPLY) {
		/* Split-dims B2: apply into the storage of the slot this message
		 * belongs to -- its routed context's slot, or for a WORLD_RESET the
		 * slot it (re)installs. Flag OFF: every slot is the gstate storage,
		 * the primary particle system stays selected, and this is exactly the
		 * pre-B2 call. Saved/restored so a nested clin_process (the forced
		 * resets) cannot leak its target into the caller's handler. */
		int prev_apply = s_apply_slot;
		struct particle_system* prev_particles = NULL;
		struct sign_text_store* prev_signs = NULL;
		struct piston_moving_store* prev_pistons = NULL;
		if(s_world_payloads_enabled) {
			unsigned target = routed_ctx ? routed_ctx->world_index
										 : s_world_payload_slot;
			if(call->type == CRPC_WORLD_RESET)
				target = call->payload.world_reset.world_generation
					? call->payload.world_reset.world_index : 0u;
			if(target >= CLIENT_CTX_SLOT_COUNT) target = 0u;
			s_apply_slot = (int)target;
			prev_particles = particle_system_select(clin_particles());
			prev_signs = sign_text_store_select(clin_signs_of_slot(target));
			prev_pistons = client_piston_moving_store_select(
				clin_pistons_of_slot(target));
		}
		clin_process_observed(call);
		s_apply_slot = prev_apply;
		if(prev_particles) particle_system_select(prev_particles);
		if(prev_signs) sign_text_store_select(prev_signs);
		if(prev_pistons) client_piston_moving_store_select(prev_pistons);
	} else {
		if(verdict == CLIENT_CTX_ROUTE_HOLD)
			fprintf(stderr, "[client] world gate: direct-path message for a "
							"quiescing context dropped (type %d)\n",
					(int)call->type);
		/* A gated column that MAY belong to this payload -- AMBIGUOUS
		 * (unstamped with two live slots) or a direct-path HOLD drop -- was
		 * marked SENT by the server: never let the next epoch marker ACK it
		 * as installed (Fable F1); take the NACK/restream path like a failed
		 * client allocation. STALE / NO_CONTEXT / FOREIGN columns belong to a
		 * world this payload is NOT: they leave no hole here, and arming the
		 * latch for them would restream-livelock while that other stream
		 * continues (Fable round 2). */
		if((call->type == CRPC_CHUNK || call->type == CRPC_FAR_CHUNK)
		   && (verdict == CLIENT_CTX_ROUTE_AMBIGUOUS
			   || verdict == CLIENT_CTX_ROUTE_HOLD))
			s_world_stream_install_failed = true;
		clin_rpc_release_pooled(call);
	}
	if(observe_envelope && clin_column_observe_rpc)
		clin_column_observe_rpc(call);
	if(client_settle_observe)
		clin_settle_rpc(SET_INSTALL_FAIL, call, call);
}

/* ---- Experimental local split-screen: the guest BODY ---------------------
 * A guest body is a second local player entity in the SHARED entity store.
 * It is deliberately NOT given entity_local_player's tick_client: that tick
 * is primary-only (it reads player zero's session, input and ride state).
 * main.c drives this body explicitly through entity_local_player_walk_owned
 * with player 1's owned input frame. Rendering stays NULL exactly like the
 * primary body's. */
static void clin_guest_body_release(void) {
	struct client_session* guest = &gstate.players[1];
	if(!guest->local_player) return;
	/* Split-dims B2 (flag ON): the body lives in whichever payload store it
	 * was installed into, which need not be the one this call targets. */
	if(s_world_payloads_enabled) {
		struct entity* body = guest->local_player;
		guest->local_player = NULL;
		(void)clin_payload_free_body(body);
		return;
	}
	uint32_t id = guest->local_player->id;
	struct entity** slot = dict_entity_get(CW_ENTITIES, id);
	guest->local_player = NULL;
	if(slot && *slot) {
		free(*slot);
		dict_entity_erase(CW_ENTITIES, id);
	}
}

/* admit-retry-0925 (ASTRA-RETIRE-RETRY-0925): the install result is
 * classified so client_session can apply the ruling -- bounded retry for a
 * transient allocation refusal, immediate retirement for an invalid id. */
enum clin_guest_install {
	CLIN_GUEST_INSTALLED, CLIN_GUEST_INSTALL_HELD, /* body already present */
	CLIN_GUEST_INSTALL_TRANSIENT, CLIN_GUEST_INSTALL_INVALID,
	/* O07 / R04 D2-1/D2-2: a stale or contradictory READY -- ignored WHOLLY:
	 * no body/view change, no session status, no movement-token adoption. */
	CLIN_GUEST_INSTALL_STALE
};


static enum clin_guest_install clin_guest_body_install_here(
	const struct client_rpc* call) {
	uint32_t id = call->payload.local_guest.entity_id;
	struct client_session* guest = &gstate.players[1];
	if(call->payload.local_guest.health < 0
	   || call->payload.local_guest.health > MAX_PLAYER_HEALTH)
		return CLIN_GUEST_INSTALL_INVALID;
	if(guest->local_player) return CLIN_GUEST_INSTALL_HELD;
	/* id 0 is the primary body's reserved key; an existing key would mean the
	 * server handed out a live entity's id. Refuse both rather than overwrite
	 * a body that is not ours. */
	if(!id || dict_entity_get(CW_ENTITIES, id))
		return CLIN_GUEST_INSTALL_INVALID;
	struct entity* body = clin_entity_spawn_alloc(id, "local guest body");
	if(!body) return CLIN_GUEST_INSTALL_TRANSIENT;
	entity_local_player(id, body, CW_WORLD);
	body->tick_client = NULL; /* never the primary-only tick */
	body->health = call->payload.local_guest.health;
	body->pos[0] = body->pos_old[0] = body->network_pos[0]
		= (float)call->payload.local_guest.x;
	body->pos[1] = body->pos_old[1] = body->network_pos[1]
		= (float)call->payload.local_guest.y;
	body->pos[2] = body->pos_old[2] = body->network_pos[2]
		= (float)call->payload.local_guest.z;
	glm_vec3_zero(body->vel);
	guest->camera.x = body->pos[0];
	guest->camera.y = body->pos[1];
	guest->camera.z = body->pos[2];
	/* The third absolute writer. gstate.players[1] is REUSED across joins, so
	 * a guest who left while sneaking would otherwise install this camera on
	 * top of the previous session's fold-out record and send its dip back at
	 * the server from the first echo (clin_update's guest arm below). */
	player_ysize_absolute_set(&guest->camera.ysize);
	/* Same wire convention the primary uses in reverse (see the
	 * SRPC_PLAYER_POS send below): rx = -deg(camera.rx), ry = deg(camera.ry) - 90. */
	guest->camera.rx = -glm_rad(call->payload.local_guest.rx);
	guest->camera.ry = glm_rad(call->payload.local_guest.ry + 90.0F);
	body->orient[0] = body->orient_old[0] = guest->camera.rx;
	body->orient[1] = body->orient_old[1] = guest->camera.ry;
	guest->local_player = body;
	/* split-dimensions-plan-0924/C2b: a body installed by a RETRIED
	 * GAMEPLAY_READY (the first install refused above or in
	 * clin_entity_spawn_alloc) takes the fire state the guest route already
	 * delivered for this generation: the server recorded that send and the
	 * idempotent READY moves none of its resend keys
	 * (server_local_publish_split_fire), so nothing else will paint it. A
	 * first-time install has no latch (client_session_bind_local zeroed it)
	 * and keeps entity_default_init's false; the server's publish follows the
	 * READY in FIFO order. */
	if(guest->local_fire_latched)
		body->on_fire = guest->local_fire;
	return CLIN_GUEST_INSTALLED;
}

/* Split-dims B2 (Fable F2): CRPC_LOCAL_GUEST is not world-scoped, so the
 * apply slot is player 0's. Flag ON, the guest's body belongs in the store of
 * the slot its view (1) shows; while that slot's context install is still
 * being retried the install is TRANSIENT (the admission retry re-asks). No
 * guest-viewed slot (flag OFF, or the guest shares player 0's world): the
 * apply slot, as before. */
/* D2 (ASTRA-DEFECTS-RULING 7-10): a GAMEPLAY_READY stamped with the world
 * the SERVER admitted the guest body into. The body is installed into that
 * world's store -- so its prediction and collision read that world's terrain
 * (entity_local_player(..., CW_WORLD) under that apply slot) -- and view 1 is
 * bound to exactly that world. Never "player 0's current world": the host may
 * have transferred between the guest's ACCEPT and this READY (PC smoke boat
 * tape seq 195 host transfer_begin -> 204 server READY {0,2,0} -> 207 client
 * bound {1,3,-1}), or a saved-dimension Join admitted it elsewhere (A2-T).
 *  - no live client context at exactly that identity (not installed yet,
 *    released, or a reused slot's retired generation): nothing is installed
 *    or bound anywhere -- TRANSIENT, the client's GAMEPLAY retry re-asks; an
 *    already-admitted body is kept as it is (a stale READY never moves it);
 *  - a duplicate for the world the body already lives in: HELD, view 1 is
 *    (re)asserted on that world;
 *  - a body held in ANOTHER world: the authority moved it -- that stale copy
 *    is released and the body is installed in the named world. */
static enum clin_guest_install clin_guest_body_install_at(
	const struct client_rpc* call) {
	const unsigned want = call->payload.local_guest.world_slot;
	const uint32_t gen = call->payload.local_guest.world_generation;
	const uint32_t bind_seq = call->payload.local_guest.guest_bind_seq;
	struct entity* body = gstate.players[1].local_player;
	/* O07 / R04 D2-1: an admission OLDER than the newest binding already
	 * applied (e.g. replayed after the guest's own transfer) is stale. */
	if(clin_guest_bind_stale(bind_seq)) {
		fprintf(stderr, "[client] guest READY bind seq %u older than %u; "
						"ignored\n", (unsigned)bind_seq,
				(unsigned)s_guest_bind_seq);
		return CLIN_GUEST_INSTALL_STALE;
	}
	const struct client_world_context* ctx = want < CLIENT_CTX_SLOT_COUNT
		? client_ctx_registry_current(&s_world_ctx, want) : NULL;
	/* O07 / R04 D2-3: the full announced tuple must agree with the live
	 * world -- a contradictory dimension is rejected, never adopted. */
	if(ctx && ctx->server_generation == gen
	   && ctx->dimension != (int)call->payload.local_guest.world_dimension) {
		fprintf(stderr, "[client] guest READY slot %u gen %u names dimension "
						"%d, live world is %d; rejected\n", want,
				(unsigned)gen, (int)call->payload.local_guest.world_dimension,
				ctx->dimension);
		return body ? CLIN_GUEST_INSTALL_STALE : CLIN_GUEST_INSTALL_INVALID;
	}
	if(!ctx || ctx->server_generation != gen) {
		fprintf(stderr, "[client] guest READY for world slot %u gen %u: no live "
						"client world at that identity; %s\n", want,
				(unsigned)gen, body ? "admitted body kept" : "held for retry");
		/* O07 / R04 D2-2: an admitted guest ignores it WHOLLY (STALE: no
		 * token rewind); a not-yet-admitted one retries. */
		return body ? CLIN_GUEST_INSTALL_STALE : CLIN_GUEST_INSTALL_TRANSIENT;
	}
	if(body) {
		struct clin_world_payload p;
		struct entity** tracked = clin_world_payload_of_slot(want, &p)
			&& p.entities ? dict_entity_get(p.entities, body->id) : NULL;
		if(tracked && *tracked == body && body->world == clin_world_of_slot(want)) {
			(void)clin_world_payload_bind_view(want, 1);
			clin_guest_bind_applied(bind_seq);
			return CLIN_GUEST_INSTALL_HELD;
		}
		clin_guest_body_release();
	}
	const int prev = s_apply_slot;
	s_apply_slot = (int)want;
	const enum clin_guest_install result = clin_guest_body_install_here(call);
	s_apply_slot = prev;
	if(result != CLIN_GUEST_INSTALLED)
		return result;
	if(!clin_world_payload_bind_view(want, 1)) {
		clin_guest_body_release();
		return CLIN_GUEST_INSTALL_TRANSIENT;
	}
	if(s_payload_prepared[want] == gen)
		s_payload_prepared[want] = 0; /* the landing world is now viewed */
	clin_guest_bind_applied(bind_seq);
	return CLIN_GUEST_INSTALLED;
}

static enum clin_guest_install clin_guest_body_install(
	const struct client_rpc* call) {
	if(!s_world_payloads_enabled)
		return clin_guest_body_install_here(call);
	if(call->payload.local_guest.world_generation != 0)
		return clin_guest_body_install_at(call); /* D2: authoritative */
	int guest_slot = -1;
	for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++) {
		const struct client_world_context* ctx
			= client_ctx_registry_current(&s_world_ctx, slot);
		if(ctx && (ctx->bound_views & 2u))
			guest_slot = (int)slot;
		else if(s_payload_reinstall[slot].armed
				&& (s_payload_reinstall[slot].views & 2u)
				&& !gstate.players[1].local_player)
			return CLIN_GUEST_INSTALL_TRANSIENT;
	}
	if(guest_slot < 0) {
		/* Initial admission shares player 0's world. There is no gate-1
		 * WORLD_RESET for that case, so READY must bind the guest's view to
		 * the store that receives its body. Otherwise a later host switch
		 * leaves view 1 unbound and it silently renders the host's Nether
		 * while the guest remains in the Overworld. Do this only after the
		 * body was installed: a refused admission claims no view. */
		enum clin_guest_install result = clin_guest_body_install_here(call);
		if(result == CLIN_GUEST_INSTALLED
		   && !clin_world_payload_bind_view(s_world_payload_slot, 1)) {
			/* A failed primary context install can leave a store but no
			 * bindable context. Keep gameplay closed and retry READY once the
			 * world becomes valid; never admit an invisible, wrong-world body. */
			clin_guest_body_release();
			return CLIN_GUEST_INSTALL_TRANSIENT;
		}
		return result;
	}
	const int prev = s_apply_slot;
	s_apply_slot = guest_slot;
	const enum clin_guest_install result = clin_guest_body_install_here(call);
	s_apply_slot = prev;
	return result;
}

/* One entry point for every CRPC_LOCAL_GUEST, so the body is installed BEFORE
 * client_session_local_status decides whether gameplay admission is granted
 * (it requires a body), and retired the moment admission is not held. */
static void clin_local_guest_status(struct client_rpc* call) {
	struct client_session* guest = &gstate.players[1];
	if(call->payload.local_guest.result == LOCAL_GUEST_GAMEPLAY_READY
	   && call->payload.local_guest.respawn_epoch == 0
	   && guest->local_bound && guest->local_ticket
	   && call->payload.local_guest.ticket == guest->local_ticket
	   && call->payload.local_guest.generation == guest->local_generation
	   && guest->local_phase == CLIENT_GUEST_ATTACHED) {
		enum clin_guest_install installed = clin_guest_body_install(call);
		if(installed == CLIN_GUEST_INSTALL_STALE)
			return; /* O07 / R04 D2-2: no status, no correction_seq rewind */
		if(installed == CLIN_GUEST_INSTALL_TRANSIENT)
			client_session_local_admit_refused(guest, CLIENT_GUEST_ADMIT_TRANSIENT);
		else if(installed == CLIN_GUEST_INSTALL_INVALID)
			client_session_local_admit_refused(guest, CLIENT_GUEST_ADMIT_INVALID);
	}
	const bool bound_before = guest->local_bound;
	const uint64_t generation_before = guest->local_generation;
	client_session_local_status(guest, call);
	/* A queued READY is not an accepted body: context reinstall and entity
	 * allocation may refuse it transiently. Publish the new movement token
	 * only after the matching reply actually admitted the guest body. */
	if(call->payload.local_guest.result == LOCAL_GUEST_GAMEPLAY_READY
	   && call->payload.local_guest.respawn_epoch == 0
	   && guest->local_bound && guest->local_gameplay_ready
	   && guest->local_player && guest->local_phase == CLIENT_GUEST_ATTACHED
	   && call->payload.local_guest.ticket == guest->local_ticket
	   && call->payload.local_guest.generation == guest->local_generation)
		guest->player_sync.correction_seq
			= call->payload.local_guest.correction_seq;
	/* lava-aabb-0924/B4 (Fable R4 N2): the host body's on_fire is drawn only
	 * in a guest viewport and is written only while a guest is admitted
	 * (CRPC_LOCAL_PLAYER_FIRE) or by the input handoff, so after a split OFF
	 * it may still hold a burn from the previous partner. A new guest route
	 * generation starts it cleared; the server's first publish of that
	 * generation force-resends the host's real state
	 * (server_local_publish_split_fire), so a stale flame is never drawn.
	 * split-dimensions-plan-0924/C2b: the clear rides the OFFER that binds
	 * the generation (client_session_bind_local), not the body install. The
	 * server cannot have published anything for a generation the client has
	 * not bound yet, whereas the install may be a RETRY after a refused one --
	 * by then the host's publish has already been applied and recorded as
	 * sent, and clearing it there lost the host's flame until its next real
	 * change. */
	if(guest->local_bound
	   && (!bound_before || guest->local_generation != generation_before)
	   && gstate.players[0].local_player)
		gstate.players[0].local_player->on_fire = false;
	/* A matching LEFT/CANCEL settles this guest generation. The body is
	 * released below, and its view must leave the old context too: otherwise
	 * a later Join after player 0 switches worlds finds that stale view and
	 * installs the new guest body into the abandoned world. A guest-world
	 * reset does not unbind the view; only session retirement does. */
	if(bound_before && !guest->local_bound)
		for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++) {
			struct client_world_context* ctx
				= client_ctx_registry_current(&s_world_ctx, slot);
			if(ctx)
				(void)client_ctx_unbind_view(ctx, 1);
			/* A failed gate-1 install may still carry an old guest view in
			 * its retry record. LEFT retires that claim as well. */
			s_payload_reinstall[slot].views &= (uint8_t)~2u;
		}
	if(bound_before && !guest->local_bound)
		s_transfer_hold[1] = (struct clin_transfer_hold_state){0};
	if(!guest->local_gameplay_ready && !guest->local_respawn_loading)
		clin_guest_body_release();
}

static void clin_host_respawn_status(const struct client_rpc* call) {
	struct client_session* host=&gstate.players[0];
	const uint64_t epoch=call->payload.host_respawn.respawn_epoch;
	const int result=call->payload.host_respawn.result;
	if(call->payload.host_respawn.owner!=0
	   ||call->payload.host_respawn.generation!=0||!host->local_player)return;
	if(result==HOST_RESPAWN_RETRY||result==HOST_RESPAWN_TERMINAL) {
		if(!host->respawn_request_observation_pending
		   ||host->local_player->health>0
		   ||host->host_respawn_completed_epoch>host->death.completed_floor
		   ||(epoch&&(epoch<=host->death.completed_floor
		      ||(host->host_respawn_epoch&&epoch!=host->host_respawn_epoch))))return;
		host->respawn_request_observation_pending=false;
		host->respawn_failure=result==HOST_RESPAWN_RETRY
			?RESPAWN_FAILURE_RETRY:RESPAWN_FAILURE_TERMINAL;
		host->respawn_failure_epoch=epoch;
		host->respawn_failure_serial++;
		return;
	}
	if(result==HOST_RESPAWN_LOADING) {
		if(!epoch||epoch<=host->host_respawn_completed_epoch
		   ||(host->host_respawn_epoch&&epoch<host->host_respawn_epoch))return;
		if(host->host_respawn_loading&&epoch==host->host_respawn_epoch)return;
		if(!host->host_respawn_render_camera_frozen) {
			host->host_respawn_render_camera=host->camera;
			host->host_respawn_render_camera_frozen=true;
		}
		host->host_respawn_loading=true;
		host->host_respawn_epoch=epoch;
		host->host_respawn_x=call->payload.host_respawn.x;
		host->host_respawn_y=call->payload.host_respawn.y;
		host->host_respawn_z=call->payload.host_respawn.z;
		host->host_respawn_correction_installed=false;
		host->host_respawn_ack_send_pending=false;
		host->host_respawn_awaiting_commit=false;
		host->respawn_request_observation_pending=false;
		host->respawn_failure=RESPAWN_FAILURE_NONE;
		return;
	}
	if(!host->host_respawn_loading||!epoch
	   ||epoch!=host->host_respawn_epoch)return;
	if(result==HOST_RESPAWN_CORRECTION) {
		struct client_rpc correction={.type=CRPC_PLAYER_POS,
			.payload.player_pos={
				.position={(float)call->payload.host_respawn.x,
					(float)call->payload.host_respawn.y,
					(float)call->payload.host_respawn.z},
				.rotation={call->payload.host_respawn.rx,
					call->payload.host_respawn.ry},
				.correction_seq=call->payload.host_respawn.correction_seq,
				.correction_flags=call->payload.host_respawn.correction_flags}};
		if(clin_apply_player_position(host,&correction)) {
			host->host_respawn_correction_installed=true;
			if(host->host_respawn_render_camera_frozen)
				host->camera=host->host_respawn_render_camera;
		}
	} else if(result==HOST_RESPAWN_GAMEPLAY_READY
	   &&host->host_respawn_correction_installed) {
		host->host_respawn_completed_epoch=epoch;
		host->host_respawn_loading=false;
		host->host_respawn_epoch=0;
		host->host_respawn_ack_send_pending=false;
		host->host_respawn_awaiting_commit=false;
		host->host_respawn_render_camera_frozen=false;
		host->respawn_failure=RESPAWN_FAILURE_NONE;
	}
}

/* Split-dims B2: may terrain install into the payload being applied? The
 * primary payload keeps the pre-B2 gates exactly (its WORLD_RESET completed
 * and no accepted recovery is waiting for its STREAM_RESET boundary); a
 * NON-primary payload (flag ON only) gates on its own completed reset. */
static bool clin_terrain_gate_open(void) {
	if(clin_apply_is_primary())
		return s_world_reset_ready
			&& s_stream_recovery_phase != CLIN_STREAM_RECOVERY_WAIT_RESET;
	return s_payload_ready[clin_apply_slot()];
}

/* Split-dims B2 (Fable F1): has the payload being applied completed its own
 * WORLD_RESET? The primary keeps the pre-B2 flag exactly; a NON-primary
 * payload (flag ON only) answers with its own readiness, so a failed reset
 * of player 0's world never silences the other world's traffic (and a
 * not-yet-ready guest world never borrows the primary's readiness). The
 * world-scoped entity/effect/sound handlers gate on this; player-0-scoped
 * handlers (PLAYER_POS, windows, BEGIN_WORLD_SWITCH) keep the primary flag. */
static bool clin_payload_apply_ready(void) {
	if(clin_apply_is_primary())
		return s_world_reset_ready;
	return s_payload_ready[clin_apply_slot()];
}

/* A column/far install failed after the server marked it sent. The primary
 * payload takes the pre-B2 recovery/NACK path; a NON-primary payload has no
 * epoch marker of its own yet (A2 stamps per-world markers), so the hole is
 * counted against that payload -- never folded into the primary's latch,
 * which would restream the wrong world. */
static void clin_terrain_install_failed(void) {
	if(!clin_apply_is_primary()) {
		s_payload_install_failures[clin_apply_slot()]++;
		s_payload_resend_pending[clin_apply_slot()] = true;
		fprintf(stderr, "[client] world payload %u: terrain install failed "
						"(%u since its reset)\n",
				clin_apply_slot(),
				(unsigned)s_payload_install_failures[clin_apply_slot()]);
		return;
	}
	if(gstate.world_loaded && !gstate.world_switch_pending)
		clin_begin_live_stream_recovery();
	else if(gstate.world_switch_pending)
		s_world_stream_install_failed = true;
}

static void clin_process_observed(struct client_rpc* call) {
	assert(call);
	/* host_vehicle_smoke_8m (CONTRACT-v1 Q2): the narrow observe-only route
	 * witness. It snapshots the guest's native vehicle relations, cart
	 * prediction and pose immediately around the ACTUAL consumer below and
	 * counts a host-only vehicle RPC whose application attached or moved the
	 * guest. It writes nothing the handlers read. Dormant: one relaxed load. */
	struct vsp_cw_pre vsp_cw;
	bool vsp_cw_on = vsp_armed();
	if(vsp_cw_on)
		vsp_client_witness_begin(call, &vsp_cw);

	switch(call->type) {
		case CRPC_HOST_RESPAWN:
			clin_host_respawn_status(call);
			break;
		case CRPC_LOCAL_PLAYER_FIRE:
			/* Untagged route == the host body only. A guest body's update is
			 * generation-tagged and lands in client_session_local_reply. */
			if(call->payload.local_player_fire.owner == 0)
				(void)clin_apply_player_fire(&gstate.players[0], call);
			break;
		case CRPC_LOCAL_PLAYER_VELOCITY:
			/* Untagged route == the host body only; the guest's is tagged and
			 * lands in client_session_local_reply. */
			if(call->payload.local_player_velocity.owner == 0)
				(void)clin_apply_player_velocity(&gstate.players[0], call);
			break;
		case CRPC_LOCAL_GUEST:
			clin_local_guest_status(call);
			break;
		case CRPC_WORLD_RELEASE:
			clin_world_release(call);
			break;
		case CRPC_CAPACITY_REQUEST:
			clin_admission_handle_capacity(call); /* client_admission.h */
			break;
		case CRPC_LANDING_ARM:
			clin_admission_handle_landing(call);
			break;
		case CRPC_TRANSFER_HOLD:
			clin_transfer_hold_apply(call);
			break;
		case CRPC_INVENTORY_SLOT:
		case CRPC_WINDOW_ITEMS:
		case CRPC_PLAYER_HOTBAR_SLOT:
		case CRPC_WINDOW_TRANSACTION:
		case CRPC_WINDOW_PROGRESS:
		case CRPC_WINDOW_SORT_ADMIT:
			(void)client_session_window_reply(&gstate.players[0], call);
			break;
		case CRPC_RECIPE_CRAFT:
			/* Route by the reply's OWN player field (hostile review, finding 3).
			 * A guest's craft reply normally arrives generation-TAGGED and never
			 * reaches this switch at all -- clin_process_queued hands tagged RPCs
			 * to gstate.players[1]. The exception is the answer server_local.c
			 * sends when the envelope resolved to no actor: it has no inventory
			 * to route through, so it arrives untagged, and landing it on player 0
			 * would leave the guest's pending request stranded (the very thing
			 * that answer exists to prevent). client_session_window_reply still
			 * checks ticket+player+window, so a stray reply stays inert. */
			if(call->payload.recipe_craft.player < CLIENT_LOCAL_PLAYERS)
				(void)client_session_window_reply(
					&gstate.players[call->payload.recipe_craft.player], call);
			break;
		case CRPC_MAP_DATA: {
			/* Copy into this identity's plane and recycle the pooled slot
			 * immediately -- the renderer reads the retained plane (never the slot),
			 * so slot lifetime stays exactly one inbox trip like CRPC_CHUNK.
			 * version bump gates the texture re-upload (documented deviation:
			 * change-gated vs Beta's every-frame upload). */
			struct map_rpc_slot* mslot = call->payload.map_data.slot;
			assert(mslot);
			(void)clin_map_deliver(mslot->map_id, mslot->colors);
			clin_map_slot_release(mslot);
			break;
		}
		case CRPC_MAP_MARKERS:
			(void)clin_map_markers_deliver(call->payload.map_markers.map_id,
				call->payload.map_markers.bytes, call->payload.map_markers.length);
			break;
		case CRPC_CHUNK:
			/* Primary: the accepted server RESET is the FIFO boundary for the
			 * new stream (keep the old voxel view intact until then), and a
			 * failed reset allocation must not build more client state into an
			 * aborted/OOM session while the server finishes its queued stream.
			 * Either way the pooled payload is recycled. */
			if(!clin_terrain_gate_open()) {
				clin_chunk_slot_release(call->payload.chunk.slot);
				break;
			}
			if(!clin_chunk(call->payload.chunk.x, call->payload.chunk.y,
							call->payload.chunk.z, call->payload.chunk.sx,
							call->payload.chunk.sy, call->payload.chunk.sz,
							call->payload.chunk.slot))
				clin_terrain_install_failed();
			break;
		case CRPC_FAR_CHUNK:
			if(!clin_terrain_gate_open()) {
				clin_far_slot_release(call->payload.far_chunk.slot);
				break;
			}
			/* The server has already marked this far coordinate sent. Use the
			 * same whole-stream invalidation as a failed full-column install so
			 * the retained old section cannot hide a permanently lost refresh. */
			if(!clin_far_chunk(call->payload.far_chunk.x,
							   call->payload.far_chunk.z,
							   call->payload.far_chunk.slot))
				clin_terrain_install_failed();
			break;
		case CRPC_UNLOAD_CHUNK:
			if(clin_terrain_gate_open()) {
				client_piston_moving_remove_chunk(
					call->payload.unload_chunk.x,
					call->payload.unload_chunk.z);
				world_unload_section(CW_WORLD, call->payload.unload_chunk.x,
									 call->payload.unload_chunk.z);
				// sign_text_store mirrors the voxel data's chunk residency: a
				// broken sign's removal is only ever sent for a chunk that is
				// currently loaded (server_local's onBreak has no reason to
				// notify an unloaded client chunk), so without this an
				// unload/reload cycle across a break the client never saw
				// would leave a stale grid forever. See sign_text_store.h's
				// sign_text_store_clear_chunk doc for the full reasoning.
				sign_text_store_clear_chunk(call->payload.unload_chunk.x,
											call->payload.unload_chunk.z);
			}
			break;
		case CRPC_PLAYER_POS:
			/* Reject duplicate/stale nonzero markers before ANY ownership, camera,
			 * body, hotbar or ACK-latch mutation. The server is the only producer
			 * and both directions are lossless FIFO; serial arithmetic additionally
			 * makes process-lifetime wrap well-defined. */
			if(call->payload.player_pos.input_epoch != 0
			   && !clin_input_epoch_is_newer(
				   call->payload.player_pos.input_epoch))
				break;
			if(!s_world_reset_ready || !gstate.players[0].local_player
			   || !gstate.players[0].windows[WINDOWC_INVENTORY]) {
				/* A failed WORLD_RESET owner allocation is unrecoverable without a
				 * new reset, but silently dropping the final marker strands both halves
				 * forever. NACK each exact epoch so the server's bounded retry policy
				 * reaches the visible terminal-load state while every barrier stays shut. */
				if(call->payload.player_pos.input_epoch != 0) {
					fprintf(stderr,
							"[client] input handoff withheld: reset owners missing\n");
					gstate.world_loaded = false;
					gstate.world_switch_pending = true;
					gstate.paused = false;
					screen_set(&screen_load_world);
					clin_submit_input_retry(
						call->payload.player_pos.input_epoch);
				}
				break;
			}
			if(call->payload.player_pos.input_epoch != 0) {
				clin_hold_input_handoff();
				/* A corrupt local handoff must fail closed on the loading screen;
				 * acknowledging an un-installable snapshot would expose divergent input. */
				if(call->payload.player_pos.selected_hotbar
				   >= INVENTORY_SIZE_HOTBAR) {
					fprintf(stderr,
							"[client] input handoff withheld: invalid hotbar\n");
					gstate.world_loaded = false;
					clin_submit_input_retry(
						call->payload.player_pos.input_epoch);
					break;
				}
				w_coord_t check_y
					= (w_coord_t)floorf(call->payload.player_pos.position[1]);
				if(check_y < 0)
					check_y = 0;
				else if(check_y >= WORLD_HEIGHT)
					check_y = WORLD_HEIGHT - 1;
				if(s_world_stream_install_failed || !world_find_chunk(
					   CW_WORLD,
					   (w_coord_t)floorf(call->payload.player_pos.position[0]),
					   check_y,
					   (w_coord_t)floorf(call->payload.player_pos.position[2]))) {
					fprintf(stderr,
							"[client] input handoff withheld: voxel stream install failed\n");
					/* Free the retained old ring before asking the server to invalidate
					 * every sent claim and restream. Entity/window ownership remains live;
					 * only voxel memory is reclaimed for the retry. */
					client_piston_moving_store_clear(
						clin_pistons_of_slot(clin_apply_slot()));
					world_unload_all(CW_WORLD);
					particle_system_clear(clin_particles());
					gstate.world_loaded = false;
					s_world_stream_install_failed = false;
					clin_submit_input_retry(
						call->payload.player_pos.input_epoch);
					break;
				}
			}
			(void)clin_apply_player_position(&gstate.players[0], call);
			if(call->payload.player_pos.correction_flags
			   & PLAYER_CORRECTION_POSITION_ONLY) {
				/* Sampled BEFORE any FINISH_REMOTE_LOAD below: the terrain
				 * hold only applies to MID-PLAY teleports (same-dim respawn,
				 * far /tp); the login handoff has its own fail-closed chunk
				 * check in the input_epoch branch above. */
				bool was_loaded = gstate.world_loaded;
				if(was_loaded && gstate.remote_session
				   && !gstate.remote_terrain_hold) {
					w_coord_t hold_y = (w_coord_t)floorf(
						call->payload.player_pos.position[1]);
					if(hold_y < 0)
						hold_y = 0;
					else if(hold_y >= WORLD_HEIGHT)
						hold_y = WORLD_HEIGHT - 1;
					if(!world_find_chunk(
						   CW_WORLD,
						   (w_coord_t)floorf(
							   call->payload.player_pos.position[0]),
						   hold_y,
						   (w_coord_t)floorf(
							   call->payload.player_pos.position[2]))) {
						/* Beta parity, extended: vanilla applies the
						 * post-respawn position unconditionally and trusts
						 * the server to have the destination resident
						 * [740c583 NetClientHandler.handleFlying]; mc173
						 * teleports first and streams after, so the player
						 * would free-fall through uninstalled terrain (field
						 * defect 2026-08-30: sky respawn -> fall-damage/death
						 * loop). Hold exactly like GuiDownloadTerrain --
						 * physics inert + load screen -- and release when the
						 * destination chunk installs (clin_update). */
						/* Stealing the screen must not orphan an open
						 * container: window screens only send their CLOSE
						 * from their own update() (ESC path), which never
						 * runs once we swap screens -- the server would hold
						 * the container ownership forever (the locked-chest
						 * failure class, FH-07). Close it here first. */
						if(gstate.players[0].visible_window >= 0) {
							// CHIP B: only clear client state once the server
							// has actually accepted the close; a saturated
							// send latches a per-frame retry instead
							// (clin_update, below) so ownership can't leak.
							if(svin_rpc_send(&(struct server_rpc) {
								   .type = SRPC_WINDOW_CLOSE,
								   .payload.window_close.window
								   = (uint8_t)gstate.players[0].visible_window,
							   }))
								gstate.players[0].visible_window = -1;
							else
								s_pending_window_close_retry
									= gstate.players[0].visible_window;
						}
						gstate.remote_terrain_hold = true;
						screen_set(&screen_load_world);
					}
				}
				/* Do not echo an older pre-push body sample from this clin_update. */
				gstate.players[0].player_sync.last_pos_update = time_get();
				if(call->payload.player_pos.correction_flags
				   & PLAYER_CORRECTION_FINISH_REMOTE_LOAD) {
					gstate.world_loaded = true;
					gstate.paused = false;
					clin_expose_input_handoff();
				}
				break;
			}
			/* Prevent the bottom of this same clin_update from echoing the old
			 * camera/body sample after an authoritative teleport or world load. */
			gstate.players[0].player_sync.last_pos_update = time_get();
			gstate.world_loaded = true;
			if(call->payload.player_pos.input_epoch != 0) {
				/* Oxygen's first client tick must use the installed position, not the
				 * old world's render-sampled water flag. The spawn-center send gate
				 * guarantees this voxel is present before any epoch marker. */
				struct block_data eye = world_get_block(
					CW_WORLD, floorf(gstate.players[0].camera.x),
					floorf(gstate.players[0].camera.y + 0.1F), floorf(gstate.players[0].camera.z));
				bool in_water = eye.type == BLOCK_WATER_FLOW
					|| eye.type == BLOCK_WATER_STILL;
				#ifndef GFX_FANCY_LIQUIDS
				bool water_transition = gstate.players[0].in_water != in_water;
				#endif
				gstate.players[0].in_water = in_water;
				#ifndef GFX_FANCY_LIQUIDS
				if(water_transition)
					world_redraw_chunks(CW_WORLD);
				#endif
				if(s_stream_recovery_phase == CLIN_STREAM_RECOVERY_NONE)
					gstate.paused = false;
				clin_reset_input_handoff(call);
				/* The complete snapshot is now locally authoritative. Publish the
				 * monotonic guard immediately before ordering its exact ACK. */
				s_last_input_epoch = call->payload.player_pos.input_epoch;
				clin_submit_input_ack(call->payload.player_pos.input_epoch);
			}
			break;
		case CRPC_BEGIN_WORLD_SWITCH: {
			/* Split-dims B2 (A2-T contract, flag ON): BEGIN_WORLD_SWITCH and
			 * its input-handoff epoch are PLAYER 0's transfer barrier. A begin
			 * stamped for a world that is not player 0's is not a host
			 * transfer: the guest's own transfer is signalled only by the
			 * in-place guest reset (gate 1), and player 0's session, screen
			 * and input stay untouched. Flag OFF / unstamped: unchanged. */
			if(s_world_payloads_enabled && call->world_generation != 0
			   && call->world_index != s_world_payload_slot) {
				fprintf(stderr, "[client] BEGIN_WORLD_SWITCH for world %u (not "
								"player 0's) ignored\n",
						(unsigned)call->world_index);
				break;
			}
			/* WORLD_RESET, STREAM_RESET, LOAD_FAILED, PLAYER_POS and recovery
			 * helpers also raise world_switch_pending for non-switch barriers. */
			gstate.world_switch_begin_seq++;
			/* A recovery request may lose the race to a server-authored BEGIN. Carry
			 * its known voxel hole into the reversible cancel marker; WORLD_RESET or
			 * an accepted STREAM_RESET establishes the next safe clear boundary. */
			bool carry_invalid_stream
				= s_stream_recovery_phase != CLIN_STREAM_RECOVERY_NONE
				  || s_world_stream_install_failed;
			clin_clear_input_handoff_latches();
			clin_clear_stream_recovery();
			gstate.players[0].deferred_reopen_window = -1;
			s_world_stream_install_failed = carry_invalid_stream;
			// Loading feedback only: retain the complete old world so a failed
			// portal checkpoint can return to it without reconstruction.
			if(s_world_reset_ready && gstate.players[0].local_player) {
				// Pause is optimistic client state backed by a TOGGLE RPC. The
				// server transition barrier rejects any stale toggle, so normalize
				// this half before the input-less loading screen takes ownership.
				gstate.paused = false;
				/* Death respawns also use BEGIN; they are not portal travel. */
				if(!gstate.players[0].death.active
				   && gstate.players[0].current_screen != &screen_death)
					clin_portal_loading_begin(0,
						CW_WORLD->dimension == WORLD_DIM_NETHER
							? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER);
				gstate.world_switch_pending = true;
				screen_set(&screen_load_world);
			}
			break;
		}
		case CRPC_WORLD_RESET:
			if(call->payload.world_reset.guest_world) {
				clin_guest_world_reset(call); /* B2 gate 1 */
				break;
			}
			/* Split-dims B2, concurrent client worlds (A2-T, flag ON): player
			 * 0's stamped world switch touches only player 0's side.
			 *  - HOST JOIN: the target slot is already live at exactly this
			 *    generation and the guest's view shows it (the host follows
			 *    the guest into its world). That payload is the guest's world
			 *    and stays: no teardown, no new context; view 0 joins it and
			 *    player 0's body is created in its store.
			 *  - Otherwise the target slot is (re)built as before. Player 0's
			 *    OLD world is not torn down here: the guest may stay behind in
			 *    it (its payload keeps streaming, no WORLD_RESET of the
			 *    partner's world), and a world nobody is in any more empties
			 *    through the server's own unload traffic, exactly as the B2
			 *    slot contract has it (a slot's lifetime follows its server
			 *    world, never a guess on the client).
			 * The guest's windows, body and view are never touched here unless
			 * its body lives in the store being torn down. Flag OFF /
			 * unstamped: none of this applies. */
			const bool b2_stamped = s_world_payloads_enabled
				&& call->payload.world_reset.world_generation != 0
				&& call->payload.world_reset.world_index < CLIENT_CTX_SLOT_COUNT;
			const unsigned b2_old_primary = s_world_payload_slot;
			const unsigned b2_target
				= b2_stamped ? call->payload.world_reset.world_index : 0u;
			const struct client_world_context* b2_live
				= b2_stamped && b2_target != b2_old_primary
				? client_ctx_registry_current(&s_world_ctx, b2_target) : NULL;
			/* A2-T landing (blocker 3): a context this client PREPARED for
			 * the host's landing (unviewed, exactly this generation, the
			 * arrival columns already in it) is joined the same way. */
			const bool b2_host_join = b2_live
				&& b2_live->server_generation
					== call->payload.world_reset.world_generation
				&& ((b2_live->bound_views & 2u)
					|| (s_payload_prepared[b2_target]
							== call->payload.world_reset.world_generation
						&& !b2_live->bound_views && s_payload_ready[b2_target]));
			if(b2_host_join)
				s_payload_prepared[b2_target] = 0;
			/* F4: never let a superseded (older-generation) reset tear the
			   slot's newer world down; nothing below has run yet. */
			if(b2_stamped
			   && clin_reset_superseded(b2_target,
					call->payload.world_reset.world_generation)) {
				fprintf(stderr, "[client] WORLD_RESET for slot %u gen %u is "
								"older than the live world; dropped\n",
						b2_target,
						(unsigned)call->payload.world_reset.world_generation);
				break;
			}
			/* NetClientHandler.handleRespawn (func_9448_a, 467-477): only
			 * a changed dimension opens the terrain screen. Remote packets
			 * already arrive here; no provider or keep-alive changes needed. */
			if(gstate.remote_session && gstate.players[0].local_player
			   && call->payload.world_reset.local_entity != 0
			   && CW_WORLD->dimension != call->payload.world_reset.dimension)
				clin_portal_loading_begin(0, call->payload.world_reset.dimension);
			/* Only an accepted reset ends the host credit lifetime. A
			 * superseded reset above leaves every live credit owned. */
			svin_credit_host_lifetime_bump();
			/* F5: a guest install of this very slot+generation still being
			   retried (gate 1 hit a full pool) is not disarmed by the host's
			   rebuild: the host lands in the guest's pending world, so the
			   guest's view is bound to the new context below. */
			uint8_t b2_guest_views = 0;
			if(b2_stamped && s_payload_reinstall[b2_target].armed
			   && s_payload_reinstall[b2_target].guest
			   && s_payload_reinstall[b2_target].generation
				   == call->payload.world_reset.world_generation)
				b2_guest_views
					= (uint8_t)(s_payload_reinstall[b2_target].views & ~1u);
			/* local_entity 0 is the menu unload's reset: a pending Save and
			 * quit is now truly done, so go to the title BEFORE the
			 * render_world check below would route the pause screen into the
			 * load screen. */
			if(call->payload.world_reset.local_entity == 0) {
				/* A stamped split-world transfer also carries local_entity 0;
				 * only the unstamped session teardown discards hold tokens. */
				if(!b2_stamped) {
					s_transfer_hold[0] = (struct clin_transfer_hold_state){0};
					s_transfer_hold[1] = (struct clin_transfer_hold_state){0};
					s_host_transfer_pane=false;
					s_host_transfer_request_seen=0;
				}
				screen_pause_exit_settled(true);
				/* Only the untagged session teardown completes Save and quit.
				 * A dimension/respawn reset can also carry local_entity zero. */
				if(call->payload.world_reset.world_generation==0)
					(void)screen_death_owner_exit_settled(&gstate.players[0],true);
				/* Split-screen host: the owner-local Pause's Save and quit is
				 * settled HERE, not on enqueue (main.c local_pause_frame), and
				 * this reset is what selects the title -- closing the pause
				 * alone would leave the render_world screen to route into the
				 * load screen below. */
				if(gstate.players[0].pause.open && gstate.players[0].pause.host
				   && gstate.players[0].pause.exit_pending) {
					local_pause_host_exit_settled(&gstate.players[0].pause, true);
					screen_set(&screen_title);
				}
			}
			/* A stamped handoff replaces windows/body, but cannot settle a
			 * Save and quit already queued behind it. Keep that menu owner. */
			const struct death_ui retained_death_exit=gstate.players[0].death;
			client_session_host_respawn_world_reset(&gstate.players[0]);
			column_view_recovery_reset();
			clin_clear_map_data();
			/* Arm presentation even if world-select stays visible during
			 * teardown. A later valid marker still owes terrain readiness. */
			lworld_reset_presentation();
#ifdef PLATFORM_WII
			cw_world_reset_trace("teardown_start");
#endif
			clin_reset_player_state_latch();
			clin_clear_input_handoff_latches();
			clin_clear_stream_recovery();
			minecart_ride_client_state_reset(&gstate.players[0].ride, &gstate.players[0].camera, false);
			// A dimension switch or respawn while the sleep fade is up would
			// otherwise strand the wash on screen: the new world's server never
			// resends a timer it considers already 0.
			gstate.players[0].sleep_timer = 0;
			gstate.players[0].sleeping = false;
			// A reconnect to a different world/server should not keep showing
			// the previous connection's chat lines.
			chat_ring_reset(&gstate.chat_ring);
			gstate.players[0].visible_window = -1;
			gstate.players[0].deferred_reopen_window = -1;
			s_world_stream_install_failed = false;
			gstate.world_load_failed = false;
			gstate.world_load_failed_reason[0] = 0; /* G6 L1: no stale reason on the next card */
			// A world reset supersedes any in-flight respawn terrain hold
			// (dim change or reconnect teardown); never let it outlive the
			// world it was gating.
			gstate.remote_terrain_hold = false;
			// Invalidate every public alias before tearing down the old world. An
			// allocation failure below must leave the client on the load screen,
			// never rendering a freed world or dereferencing the old player.
			gstate.world_loaded = false;
			/* WORLD_RESET is only the teardown/install midpoint. Ordinary/demo
			 * PLAYER_POS RPCs may precede the final epoch marker; keep every screen,
			 * input and entity tick gated until its exact ACK is safely ordered. */
			gstate.world_switch_pending = true;
			gstate.paused = false;
			s_world_reset_ready = false;
			/* Split-dims B2 (flag ON): player 0 is leaving its old world, whose
			 * payload may survive this reset (it is another slot's). Its old
			 * body must not stay behind in that world's entity store as an
			 * orphan; the store being reset below frees it anyway. */
			if(s_world_payloads_enabled && gstate.players[0].local_player)
				(void)clin_payload_free_body(gstate.players[0].local_player);
			gstate.players[0].local_player = NULL;
			if(gstate.players[0].current_screen && gstate.players[0].current_screen->render_world)
				screen_set(&screen_load_world);

			if(b2_host_join) {
				/* Only player 0's side: its windows go (its body was freed
				 * above); the guest's world, windows and body stay. */
				client_session_clear_windows(&gstate.players[0]);
				if(call->payload.world_reset.world_generation!=0
				   &&retained_death_exit.exit_pending
				   &&retained_death_exit.commit_pending==DEATH_COMMIT_EXIT)
					gstate.players[0].death=retained_death_exit;
				gstate.players[0].local_player = NULL;
			} else {
			client_piston_moving_clear();
#ifdef PLATFORM_WII
			cw_world_reset_trace("pre_chunk_gx_free");
#endif
			world_unload_all(CW_WORLD);
#ifdef PLATFORM_WII
			cw_world_reset_trace("post_chunk_gx_free");
#endif
			// M5: old-world particles must not survive into the new world (a
			// live dimension switch can reuse overlapping coordinates).
			particle_system_clear(clin_particles());

			for(unsigned player = 0; player < CLIENT_LOCAL_PLAYERS; player++) {
				/* Split-dims B2 (flag ON): a body tracked by the OTHER
				 * payload's entity store is not freed below and stays that
				 * world's -- only bodies in the store being torn down lose
				 * their alias, and only their owners' windows go (A2-T: the
				 * guest keeps its open container across the host's switch).
				 * Flag OFF: one store, every alias and window drops. */
				struct entity* body = gstate.players[player].local_player;
				if(s_world_payloads_enabled && body) {
					struct entity** tracked
						= dict_entity_get(CW_ENTITIES, body->id);
					if(!tracked || *tracked != body)
						continue;
				}
				client_session_clear_windows(&gstate.players[player]);
				if(player==0&&call->payload.world_reset.world_generation!=0
				   &&retained_death_exit.exit_pending
				   &&retained_death_exit.commit_pending==DEATH_COMMIT_EXIT)
					gstate.players[0].death=retained_death_exit;
				gstate.players[player].local_player = NULL;
			}

			dict_entity_it_t it;
			dict_entity_it(it, CW_ENTITIES);

			while(!dict_entity_end_p(it)) {
				free(dict_entity_ref(it)->value);
				dict_entity_next(it);
			}

			dict_entity_reset(CW_ENTITIES);
			// Every sign_text_store entry is scoped to a loaded chunk;
			// WORLD_RESET tears down every chunk (world_unload_all above), so
			// the store must not keep signs from the world being left.
			sign_text_store_clear();
			} /* !b2_host_join */
			/* Split-dims B1: a new server world is a new client world context
			 * (bound once to the payload's server identity, or to the unstamped
			 * stream while the producer is unconverted). The predecessor retires
			 * here -- after its payloads were torn down above -- and is reclaimed
			 * once its in-flight mesher/GPU leases drain (ruling #3); its
			 * completions can never publish into the successor. */
			/* Single payload (Fable F1): this reset replaces THE client world,
			 * so every other slot's context retires too -- "exactly one live
			 * slot" is enforced here, not hoped for from producer conversion
			 * order. (A second client payload lifts this.) */
			/* Split-dims B2 (flag ON): each slot owns its own payload, so a
			 * reset replaces only its own slot's context; the other slot's
			 * context and payload are untouched. */
			if(!s_world_payloads_enabled) {
				for(unsigned slot = 0; slot < CLIENT_CTX_SLOT_COUNT; slot++)
					if(slot != (call->payload.world_reset.world_generation
									? call->payload.world_reset.world_index : 0u))
						client_ctx_registry_retire(&s_world_ctx, slot);
			} else if(!call->payload.world_reset.world_generation) {
				/* An UNSTAMPED reset comes from a producer that knows no
				 * world slots: a single-world session (a new login, a legacy
				 * respawn). It owns the whole client, so every other slot's
				 * context retires and its payload is released -- otherwise a
				 * previous session's second world would stay live and turn
				 * all of this session's unstamped traffic AMBIGUOUS. */
				for(unsigned slot = 1; slot < CLIENT_CTX_SLOT_COUNT; slot++)
					if(client_ctx_registry_current(&s_world_ctx, slot))
						clin_payload_release_slot(slot);
			}
			if(!b2_host_join) {
			s_payload_ready[clin_apply_slot()] = false;
			s_payload_install_failures[clin_apply_slot()] = 0;
			s_payload_resend_pending[clin_apply_slot()] = false;
			s_payload_resend_requested[clin_apply_slot()] = false;
			s_payload_reinstall[clin_apply_slot()].armed = false;
			if(!client_ctx_registry_install(&s_world_ctx,
					(int)call->payload.world_reset.dimension,
					call->payload.world_reset.world_generation
						? call->payload.world_reset.world_index : 0u,
					call->payload.world_reset.world_generation)) {
				fprintf(stderr,
						"[client] FATAL: world context install failed (%u in "
						"use) -- world load aborted\n",
						client_ctx_registry_in_use(&s_world_ctx));
				break;
			}
			} /* !b2_host_join */
			/* Player 0's view follows player 0's world. Flag ON: the other
			 * slot's context (still live) gives view 0 up first, so a view is
			 * bound to exactly one payload. */
			if(s_world_payloads_enabled) {
				(void)clin_world_payload_bind_view(clin_apply_slot(), 0);
				if(b2_guest_views) /* F5 */
					clin_payload_bind_views(clin_apply_slot(), b2_guest_views,
											true);
			} else
				(void)client_ctx_bind_view(
					client_ctx_registry_current(&s_world_ctx,
						call->payload.world_reset.world_generation
							? call->payload.world_reset.world_index : 0u), 0);
			/* gstate.world is now this context's payload: its mesher jobs and
			 * GPU hand-offs lease against it from here on. */
			s_world_payload_slot = call->payload.world_reset.world_generation
				? call->payload.world_reset.world_index : 0u;
			CW_WORLD->lease_owner = clin_lease_owner_for(clin_apply_slot());
			if(!clin_remote_relight_attach(CW_WORLD)) {
				fprintf(stderr, "[client] FATAL: remote relight reservation "
						"(%lu B) failed -- world load aborted\n", (unsigned long)
						lighting_remote_relight_bytes(CLIN_REMOTE_RELIGHT_SLOTS));
				/* Readiness stays closed with a visible reason (the remote
				 * load screen's escape hatch); world_set_block refuses the
				 * legacy stack while no reservation is attached. */
				gstate.world_loaded = false;
				gstate.world_load_failed = true;
				snprintf(gstate.world_load_failed_reason,
						 sizeof(gstate.world_load_failed_reason),
						 "Out of memory (light)");
				break;
			}
#ifdef PLATFORM_WII
			cw_world_reset_trace("pre_window_entity_reinit");
#endif

			gstate.players[0].windows[WINDOWC_INVENTORY]
				= s_clin_alloc(sizeof(struct window_container));
			if(!gstate.players[0].windows[WINDOWC_INVENTORY]) {
				// Same as the local-player alloc below: under -DNDEBUG the assert
				// is compiled out, so a NULL here would have windowc_create write
				// through it. Abort the load loudly instead -- world_loaded stays
				// false, the load screen stalls, the heap stays intact.
				fprintf(stderr,
						"[client] FATAL: inventory window alloc failed (MEM "
						"exhausted) -- world load aborted\n");
				break;
			}
			if(!windowc_create(gstate.players[0].windows[WINDOWC_INVENTORY],
							  WINDOW_TYPE_INVENTORY, INVENTORY_SIZE)) {
				free(gstate.players[0].windows[WINDOWC_INVENTORY]);
				gstate.players[0].windows[WINDOWC_INVENTORY] = NULL;
				fprintf(stderr,
						"[client] FATAL: inventory contents alloc failed (MEM "
						"exhausted) -- world load aborted\n");
				break;
			}

			windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY])->client_map_counter
				= &gstate.players[0].map_counter;
			CW_WORLD->dimension = call->payload.world_reset.dimension;

			/* HOST JOIN: the guest's store may already carry player 0's id
			 * (its remote view of the host); the local body replaces it. */
			if(b2_host_join) {
				struct entity** seen = dict_entity_get(CW_ENTITIES,
					call->payload.world_reset.local_entity);
				if(seen && *seen && *seen != gstate.players[1].local_player) {
					free(*seen);
					dict_entity_erase(CW_ENTITIES,
									  call->payload.world_reset.local_entity);
				}
			}
			struct entity** local_player_ptr
				= clin_entity_slot(call->payload.world_reset.local_entity);
			if(!local_player_ptr) {
				windowc_destroy(gstate.players[0].windows[WINDOWC_INVENTORY]);
				free(gstate.players[0].windows[WINDOWC_INVENTORY]);
				gstate.players[0].windows[WINDOWC_INVENTORY] = NULL;
				fprintf(stderr,
						"[client] FATAL: entity dictionary alloc failed (MEM "
						"exhausted) -- world load aborted\n");
				break;
			}
			*local_player_ptr = s_clin_alloc(sizeof(struct entity));
			if(!*local_player_ptr) {
				// A NULL local player is fatal to the session either way, but
				// failing LOUDLY here beats the NDEBUG alternative (assert
				// compiled out -> entity_local_player writing through NULL =
				// wild stores). Leave world_loaded false; the load screen
				// stalls instead of the heap corrupting.
				dict_entity_erase(CW_ENTITIES,
								  call->payload.world_reset.local_entity);
				gstate.players[0].local_player = NULL;
				windowc_destroy(gstate.players[0].windows[WINDOWC_INVENTORY]);
				free(gstate.players[0].windows[WINDOWC_INVENTORY]);
				gstate.players[0].windows[WINDOWC_INVENTORY] = NULL;
				fprintf(stderr,
						"[client] FATAL: local player alloc failed (MEM "
						"exhausted) -- world load aborted\n");
				break;
			}
			gstate.players[0].local_player = *local_player_ptr;
			entity_local_player(call->payload.world_reset.local_entity,
								gstate.players[0].local_player, CW_WORLD);

			gstate.players[0].local_player->health = MAX_PLAYER_HEALTH;
			s_world_reset_ready = true;
			s_payload_ready[clin_apply_slot()] = true;
#ifdef PLATFORM_WII
			cw_world_reset_trace("spawn_apply_done");
#endif

			break;
		case CRPC_STREAM_RESET:
			/* B2 gate 2: a stream reset stamped for a NON-primary world is
			 * that world's resend confirmation; the primary's recovery path
			 * below is unchanged (flag OFF: never taken). */
			if(s_world_payloads_enabled && call->world_generation != 0
			   && call->world_index < CLIENT_CTX_SLOT_COUNT
			   && call->world_index != s_world_payload_slot) {
				clin_world_stream_reset(call);
				break;
			}
			/* Fable F6: the answer to a world-addressed request sent while
			 * this slot was not yet player 0's. The server already re-streams
			 * it (re-installs are absorbed); it is NOT the primary's recovery
			 * confirmation, which stays unstamped. */
			if(s_world_payloads_enabled && call->world_generation != 0
			   && call->world_index < CLIENT_CTX_SLOT_COUNT
			   && s_payload_resend_inflight_gen[call->world_index]
				   == call->world_generation) {
				s_payload_resend_inflight_gen[call->world_index] = 0;
				break;
			}
			if(s_stream_recovery_phase != CLIN_STREAM_RECOVERY_WAIT_RESET)
				break;
			/* This is deliberately narrower than WORLD_RESET: all non-voxel owners
			 * and the dimension remain authoritative while the old terrain is freed
			 * only after the server has atomically stopped simulation. */
			client_piston_moving_clear();
			world_unload_all(CW_WORLD);
			particle_system_clear(clin_particles());
			// Same reasoning as CRPC_WORLD_RESET: every retained sign entry
			// is scoped to a loaded chunk, and this frees every chunk too.
			sign_text_store_clear();
			/* Split-dims B1 (lead ruling, Option A, on Astra's ratified ruling
			 * #3): the re-stream installs a SUCCESSOR context bound to the SAME
			 * server identity, with a fresh epoch and its own lease accounting.
			 * The predecessor retires holding any in-flight mesher/GPU leases
			 * (e.g. a worker still pinned on an obsolete shell); those
			 * completions release against it, are rejected by epoch, and it is
			 * reclaimed only once they drain. The new stream installs at once
			 * -- the column-dedup contract "reinstall while the obsolete worker
			 * shell is still pinned" -- instead of being held behind the drain.
			 * The single-world stream re-streams the installed payload's slot
			 * (STREAM_RESET carries no stamp yet). */
			{
				struct client_world_context* ctx
					= client_ctx_registry_current(&s_world_ctx,
												  s_world_payload_slot);
				if(ctx) {
					int dimension = ctx->dimension;
					unsigned index = ctx->world_index;
					uint32_t server_generation = ctx->server_generation;
					uint8_t views = ctx->bound_views;
					struct client_world_context* successor
						= client_ctx_registry_install(&s_world_ctx, dimension,
													  index, server_generation);
					if(!successor) {
						fprintf(stderr, "[client] world context install failed on "
										"stream reset (%u in use)\n",
								client_ctx_registry_in_use(&s_world_ctx));
					} else {
						for(unsigned v = 0; v < 2; v++)
							if(views & (1u << v))
								(void)client_ctx_bind_view(successor, v);
					}
				}
			}
			s_world_stream_install_failed = false;
			column_view_recovery_reset();
			s_stream_recovery_phase = CLIN_STREAM_RECOVERY_STREAMING;
			s_stream_recover_retry_epoch = 0;
			gstate.world_loaded = false;
			gstate.world_switch_pending = true;
			gstate.world_load_failed = false;
			gstate.world_load_failed_reason[0] = 0; /* G6 L1: no stale reason on the next card */
			screen_set(&screen_load_world);
			break;
		case CRPC_WINDOW_CLOSE_REMOTE: {
			/* Packet101CloseWindow inbound: the remote server tore this
			 * container down on its own (or acked our own close). Mirrors
			 * CRPC_OPEN_WINDOW's own destroy+free+NULL teardown.
			 *
			 * Window id 0 is EXEMPT: per Beta's own protocol convention the
			 * player's permanent inventory always has id 0 and is never a
			 * server-tracked, open/close-able container -- it is UI-only
			 * client state (mc173-server's own Window::default() comment:
			 * "the player inventory is the default window that is always
			 * opened if no other window is opened, it also always has the
			 * id 0"). A REAL Beta server routinely sends CloseWindow(0):
			 * mc173's own open_window unconditionally calls
			 * close_window(sw, None, true) BEFORE opening any real
			 * container, and self.window.id defaults to 0, so the very
			 * FIRST container a session opens sends a close(0) first --
			 * completely normal traffic, not a hostile edge case. Routing
			 * it through this generic destroy+free+NULL path corrupts
			 * WINDOWC_INVENTORY (freed heap object left dangling in
			 * gstate.players[0].windows[0]) the moment ANY window is ever opened,
			 * crashing the next windowc_get_latest(gstate.players[0].windows[
			 * WINDOWC_INVENTORY]) reader (e.g. screen_chest_reset's hotbar
			 * highlight, client_interface.c). WINDOWC_INVENTORY's own
			 * lifecycle is CRPC_WORLD_RESET's alone. */
			uint8_t window = call->payload.window_close_remote.window;
   if(window!=WINDOWC_INVENTORY&&local_menu_host_active(&gstate.players[0])&&gstate.players[0].menu_host.window==window)local_menu_host_reset(&gstate.players[0]);
			if(window != WINDOWC_INVENTORY && gstate.players[0].windows[window]) {
				windowc_destroy(gstate.players[0].windows[window]);
				free(gstate.players[0].windows[window]);
				gstate.players[0].windows[window] = NULL;
			}
			if(window != WINDOWC_INVENTORY && gstate.players[0].visible_window == window) {
				gstate.players[0].visible_window = -1;
				screen_set(&screen_ingame);
			}
			break;
		}
		case CRPC_OPEN_WINDOW: {
			bool recovering
				= s_stream_recovery_phase != CLIN_STREAM_RECOVERY_NONE;
			// OPEN can race an already-queued SAVE+QUIT just like REOPEN below.
			// The server processes the earlier block click first, then closes that
			// container during UNLOAD; this stale client message must not pull
			// world-select into a render-world GUI immediately before the unload-
			// only reset. Do not emit a late CLOSE here: its window id could be
			// reused after a subsequent load.
			if(gstate.players[0].current_screen == &screen_select_world
			   || (gstate.players[0].current_screen == &screen_load_world && !recovering)) {
				gstate.players[0].deferred_reopen_window = -1;
				break;
			}
			uint8_t window = call->payload.window_open.window;
   local_menu_host_reset(&gstate.players[0]);
            gstate.players[0].sign = (struct sign_editor){0};
			gstate.players[0].visible_window = -1;
			if(recovering) s_stream_restore_window = -1;
			/* OPEN owns a new authoritative container and supersedes any deferred
			 * failed-close reply from the prior one. */
			gstate.players[0].deferred_reopen_window = -1;
			if(!s_world_reset_ready) {
				// The server publishes its container before sending OPEN. A reset
				// failure cannot silently swallow that ownership transition: reject
				// it just like either client allocation failure below.
				svin_rpc_send(&(struct server_rpc) {
					.type = SRPC_WINDOW_CLOSE,
					.payload.window_close.window = window,
				});
				break;
			}

			if(gstate.players[0].windows[window]) {
				windowc_destroy(gstate.players[0].windows[window]);
				free(gstate.players[0].windows[window]);
				gstate.players[0].windows[window] = NULL;
			}

			/* HOME changes the client screen optimistically before the server sees
			 * its TOGGLE. An earlier block click can therefore deliver OPEN after
			 * pause already owns input. Reject the hidden server container without
			 * replacing the pause screen; the lifecycle queue preserves CLOSE FIFO. */
			if((recovering && s_stream_restore_pause_screen)
			   || (!recovering
				   && (gstate.paused || gstate.players[0].current_screen == &screen_pause
					   || (gstate.players[0].current_screen == &screen_options
						   && screen_options_from_pause())))) {
				svin_rpc_send(&(struct server_rpc) {
					.type = SRPC_WINDOW_CLOSE,
					.payload.window_close.window = window,
				});
				break;
			}

			gstate.players[0].windows[window] = s_clin_alloc(sizeof(struct window_container));

			if(gstate.players[0].windows[window]) {
				if(!windowc_create(gstate.players[0].windows[window],
								   call->payload.window_open.type,
								   call->payload.window_open.slot_count)) {
					free(gstate.players[0].windows[window]);
					gstate.players[0].windows[window] = NULL;
					svin_rpc_send(&(struct server_rpc) {
						.type = SRPC_WINDOW_CLOSE,
						.payload.window_close.window = window,
					});
					break;
				}
				windowc_get_latest(gstate.players[0].windows[window])->client_map_counter
					= &gstate.players[0].map_counter;
				if(recovering) {
					s_stream_restore_window = window;
					s_stream_restore_window_type
						= call->payload.window_open.type;
					gstate.players[0].visible_window = window;
				} else {
					clin_show_window(window);
				}
			} else {
				// The server has already installed its authoritative container by
				// the time this RPC is consumed. Reject the open explicitly so it
				// closes that hidden container instead of soft-locking future GUIs.
				svin_rpc_send(&(struct server_rpc) {
					.type = SRPC_WINDOW_CLOSE,
					.payload.window_close.window = window,
				});
			}

			break;
		}
		case CRPC_REOPEN_WINDOW: {
			bool recovering
				= s_stream_recovery_phase != CLIN_STREAM_RECOVERY_NONE;
			// A stale close-failure reply may precede an already-queued successful
			// unload. Never pull world-select/loading back into a render-world GUI;
			// otherwise the following unload reset lands on a load screen with no
			// corresponding LOAD/PLAYER_POS and stalls forever.
			if(!s_world_reset_ready || gstate.players[0].current_screen == &screen_select_world
			   || (gstate.players[0].current_screen == &screen_load_world && !recovering)) {
				gstate.players[0].deferred_reopen_window = -1;
				break;
			}
			uint8_t window = call->payload.window_reopen.window;
			local_menu_host_reset(&gstate.players[0]);
            gstate.players[0].sign = (struct sign_editor){0};
			gstate.players[0].visible_window = -1;
			gstate.players[0].deferred_reopen_window = -1;
			if(recovering) s_stream_restore_window = -1;
			struct window_container* wc = gstate.players[0].windows[window];
			if(wc && (wc->type != call->payload.window_reopen.type
					  || wc->slot_count != call->payload.window_reopen.slot_count)) {
				windowc_destroy(wc);
				free(wc);
				gstate.players[0].windows[window] = NULL;
				wc = NULL;
			}
			if(!wc) {
				wc = s_clin_alloc(sizeof(*wc));
				if(!wc
				   || !windowc_create(wc, call->payload.window_reopen.type,
								  call->payload.window_reopen.slot_count)) {
					free(wc);
					gstate.players[0].windows[window] = NULL;
					svin_rpc_send(&(struct server_rpc) {
						.type = SRPC_WINDOW_CLOSE,
						.payload.window_close.window = window,
					});
					break;
				}
				windowc_get_latest(wc)->client_map_counter = &gstate.players[0].map_counter;
				gstate.players[0].windows[window] = wc;
			}
			if(recovering && s_stream_restore_pause_screen)
				gstate.players[0].deferred_reopen_window = window;
			else if(recovering) {
				gstate.players[0].deferred_reopen_window = -1;
				s_stream_restore_window = window;
				s_stream_restore_window_type
					= call->payload.window_reopen.type;
				gstate.players[0].visible_window = window;
			} else if(gstate.paused || gstate.players[0].current_screen == &screen_pause
				  || (gstate.players[0].current_screen == &screen_options
					  && screen_options_from_pause()))
				gstate.players[0].deferred_reopen_window = window;
			else {
				gstate.players[0].deferred_reopen_window = -1;
				clin_show_window(window);
			}
			break;
		}
		case CRPC_RETURN_INGAME: {
			/* A pending Save and quit answered with RETURN means the unload
			 * was refused (inventory close / checkpoint): keep the pause up
			 * with the server's reason and re-pause the live world. */
			bool death_exit=screen_death_owner_exit_settled(
				&gstate.players[0],false);
			/* IMP-QUIT-UNSAVABLE: a refusal caused only by unsavable chunks
			 * opens "Couldn't save part of the world" (Quit anyway / Try
			 * again) over the re-paused world instead of the bare toast. */
			if(call->payload.return_ingame.unsaved_chunk)
				screen_pause_exit_unsavable(
					call->payload.return_ingame.chunk_x,
					call->payload.return_ingame.chunk_z,
					call->payload.return_ingame.count);
			else
				screen_pause_exit_settled(false);
			/* Split-screen host pause: refused Save and quit -> exit_failed with
			 * the server's toast as the status line; a no-op unless a host exit
			 * is pending (unrelated RETURNs leave the pause untouched). */
			local_pause_host_exit_settled(&gstate.players[0].pause, false);
			/* Retire the closed presentation even while pause or epoch handoff
			 * keeps gameplay hidden. A later FIFO REOPEN may install its owner. */
			local_menu_host_reset(&gstate.players[0]);
            gstate.players[0].sign = (struct sign_editor){0};
			gstate.players[0].visible_window = -1;
			gstate.players[0].deferred_reopen_window = -1;
			/* Generic RETURN has no ownership generation. It may arrive after a
			 * newer accepted HOME toggle, so it must never clear pause or tear down
			 * an input handoff. It only reveals gameplay when the current client
			 * state is already unpaused and no epoch ACK is outstanding. */
			if(s_stream_recovery_phase != CLIN_STREAM_RECOVERY_NONE) {
				/* RETURN closes a previously captured GUI owner, but it is untagged
				 * and may predate a newer HOME pause. Keep that pause authoritative;
				 * only an unpaused recovery targets ingame after ACK. */
				s_stream_restore_window = -1;
				if(!s_stream_restore_pause_screen)
					gstate.paused = false;
				clin_hold_input_handoff();
			} else if(gstate.world_switch_pending || s_input_ack_retry_epoch != 0
			   || s_input_ack_wait_epoch != 0
			   || s_input_nack_retry_epoch != 0) {
				clin_hold_input_handoff();
			} else if(!death_exit) {
				gstate.world_switch_pending = false;
				if(!gstate.paused && s_world_reset_ready && gstate.players[0].local_player) {
					screen_set(&screen_ingame);
				}
			}
			break;
		}
		case CRPC_LOAD_FAILED:
			/* The server deliberately retains the epoch/input barrier. Keep every
			 * gameplay path closed, but distinguish terminal recovery exhaustion
			 * from an ordinary slow chunk stream on the visible load screen. */
			clin_clear_input_handoff_latches();
			clin_clear_stream_recovery();
			minecart_ride_client_state_reset(&gstate.players[0].ride, &gstate.players[0].camera, false);
			s_world_stream_install_failed = false;
			gstate.players[0].deferred_reopen_window = -1;
			gstate.world_load_failed = true;
			/* Bounded read: the payload array need not be NUL-terminated,
			 * so copy at most its size - 1 bytes (G6 lane B ack). */
			snprintf(gstate.world_load_failed_reason,
					 sizeof(gstate.world_load_failed_reason), "%.*s",
					 (int)(sizeof(call->payload.load_failed_reason) - 1),
					 call->payload.load_failed_reason);
			gstate.world_loaded = false;
			gstate.world_switch_pending = true;
			gstate.paused = false;
			screen_set(&screen_load_world);
			break;
		case CRPC_HUD_MESSAGE:
			(void)client_session_player_reply(&gstate.players[0], call, time_get());
			break;
		case CRPC_CHAT_LINE:
			/* GuiIngame.addChatMessage: wrap at 320 logical px through the
			 * real font metrics, then keep the last 50 lines (chat_ring.h).
			 * [740c583 NetClientHandler.handleChat -> GuiIngame.addChatMessage] */
			chat_ring_push_wrapped(&gstate.chat_ring, call->payload.chat_line.text,
								   chat_hud_line_width, CHAT_WRAP_WIDTH);
			break;
		case CRPC_SLEEP_STATE:
			(void)client_session_player_reply(&gstate.players[0], call, time_get());
			break;
		case CRPC_SPLIT_STATE:
			/* S-A1 stores the authoritative answer and does nothing else. The
			 * requester (S-A2) matches {generation, request_seq} against its own
			 * pending toggle before it believes -- and before it arms input --
			 * so a reply for someone else's request or a stale generation can
			 * never arm anything here. */
			s_split_ack.valid = true;
			s_split_ack.enabled = call->payload.split_state.enabled;
			s_split_ack.ok = call->payload.split_state.ok;
			s_split_ack.generation = call->payload.split_state.generation;
			s_split_ack.request_seq = call->payload.split_state.request_seq;
			memcpy(s_split_ack.reason, call->payload.split_state.reason,
				   sizeof(s_split_ack.reason));
			s_split_ack.reason[sizeof(s_split_ack.reason) - 1] = '\0';
			break;
		case CRPC_ATTACH_ENTITY: {
			/* Online mounts (docs/mp_mounts_design_2026-09-11.md): the
			 * vehicle is a client entity under the server's id (CRPC_SPAWN_BOAT
			 * / _MINECART). Occupancy by the local player (id 0) is exactly
			 * what the single-player tether keys on (entity_local_player.c
			 * BOAT MOUNT branch, the minecart follow above), so nothing else
			 * is needed to ride. An id we do not hold (spawn dropped before
			 * the world was ready, or destroyed in between) is a no-op. */
			if(call->payload.attach_entity.remote_rider) {
				/* MP-RIDE: another player's Entity.mountEntity
				 * (NetClientHandler.java:439-449, Entity.java:941-967). */
				struct entity** rp = dict_entity_get(
					CW_ENTITIES, call->payload.attach_entity.rider_id);
				struct entity** vp = call->payload.attach_entity.attach
					? dict_entity_get(CW_ENTITIES,
									  call->payload.attach_entity.vehicle_id)
					: NULL;
				if(rp && *rp && (*rp)->type == ENTITY_REMOTE_PLAYER)
					entity_remote_player_mount(*rp, vp ? *vp : NULL,
						world_view_entities());
				break;
			}
			struct entity** slot
				= dict_entity_get(CW_ENTITIES, call->payload.attach_entity.vehicle_id);
			if(slot && *slot)
				(void)clin_apply_attach(*slot, call->payload.attach_entity.attach,
					call->payload.attach_entity.no_reposition
						? NULL : gstate.players[0].local_player);
		} break;
		case CRPC_TIME_SET: {
			/* Split-dims B2: the clock of the world this message belongs to
			 * (flag OFF: gstate's, exactly as before). */
			struct clin_clock_ref k = clin_clock_of_slot(clin_apply_slot());
			*k.world_time = call->payload.time_set;
			*k.world_time_start = time_get();
		} break;
		case CRPC_WEATHER_SET:
			// render-slice: shadow BEFORE overwriting, mirroring Beta's own
			// World.updateWeather (`prevRainingStrength = rainingStrength;`
			// immediately before stepping `rainingStrength`) -- see the
			// weather_prev_rain_strength citation in game_state.h for why
			// this shadow lives here rather than on the wire.
			{
				struct clin_clock_ref k = clin_clock_of_slot(clin_apply_slot());
				*k.prev_rain_strength = *k.rain_strength;
				*k.prev_thunder_strength = *k.thunder_strength;
				*k.raining = call->payload.weather_set.raining;
				*k.thundering = call->payload.weather_set.thundering;
				*k.rain_strength = call->payload.weather_set.rain_strength;
				*k.thunder_strength = call->payload.weather_set.thunder_strength;
			}
			break;
		case CRPC_LOCAL_SETTLED_LIGHT: {
			struct column_light_publication* p = call->payload.settled_light;
			call->payload.settled_light = NULL;
			if(!p) break;
			if(!clin_terrain_gate_open() || gstate.remote_session
			   || !world_generation_is_local(call->world_generation)
			   || p->world_generation != call->world_generation
			   || p->world_index != call->world_index) {
				column_light_note_fallback(COLUMN_LIGHT_OWNER);
				column_light_publication_release(p);
			} else world_local_publication(CW_WORLD, p,
				call->world_index, call->world_generation);
		} break;
		case CRPC_SET_BLOCK_PACK:
			/* G9 R21: unpacked by clin_inbox_take; never a handler input. */
			assert(!"CRPC_SET_BLOCK_PACK reached clin_process");
			break;
		case CRPC_SET_BLOCK:
			if(!clin_terrain_gate_open())
				break;
			/* REL-MP-FIX3b: Beta drops an edit whose id+meta are unchanged with
			 * no write and no relight (Chunk.java:240-241); the predicate cites
			 * it. Remote only: local mode stays byte-identical. Drained parked
			 * edits reach this same case. */
			if(gstate.remote_session
			   && world_remote_block_unchanged(CW_WORLD,
				   call->payload.set_block.x, call->payload.set_block.y,
				   call->payload.set_block.z, call->payload.set_block.block))
				break;
			// DIG-BURST BUDGET (2026-06-12 audit; documented cap, not a silent one):
			// every server AIR-set fired the full per-block dig burst (64 particles,
			// particle.c PARTICLES_VOLUME) -- correct for a player dig, but a bulk
			// server clear (TNT: up to 512 cells/blast) became 6K-32K particles =
			// 0.6-3.2MB of immediate-mode FIFO bytes per frame against the 240KB
			// hi-watermark, overflow-suspending the render thread on the exact frame
			// already carrying the remesh storm. Beta is the precedent: its explosion
			// does NOT fire the dig effect per destroyed block (the blast look comes
			// from CRPC_ENTITY_EXPLODE's flash+smoke, which we already deliver). So:
			// at most CLIN_DIG_BURSTS_PER_DRAIN air-sets per clin_update drain get the
			// burst (normal mining is <=1 dig/frame -- feel unchanged); the rest of a
			// bulk clear skips particles, exactly like Beta's explosion.
			if(call->payload.set_block.block.type == BLOCK_AIR
			   && clin_dig_burst_budget > 0) {
				clin_dig_burst_budget--;
				struct block_data blk = world_get_block(
					CW_WORLD, call->payload.set_block.x,
					call->payload.set_block.y, call->payload.set_block.z);
				struct block_data neighbours[6];

				for(int k = 0; k < SIDE_MAX; k++) {
					int ox, oy, oz;
					blocks_side_offset((enum side)k, &ox, &oy, &oz);

					neighbours[k] = world_get_block(
						CW_WORLD, call->payload.set_block.x + ox,
						call->payload.set_block.y + oy,
						call->payload.set_block.z + oz);
				}

				particle_generate_block(&(struct block_info) {
					.block = &blk,
					.neighbours = neighbours,
					.x = call->payload.set_block.x,
					.y = call->payload.set_block.y,
					.z = call->payload.set_block.z,
				});
			}

			/* An ordinary overwrite settles/removes any prior id-36 owner. Drop
			 * the sidecar before chunk_set_block dirties this cell for remeshing. */
			client_piston_moving_remove(
				call->payload.set_block.x, call->payload.set_block.y,
				call->payload.set_block.z);
			world_set_block(CW_WORLD, call->payload.set_block.x,
							call->payload.set_block.y,
							call->payload.set_block.z,
							call->payload.set_block.block, true);
			world_local_revision(CW_WORLD, call->payload.set_block.x,
				call->payload.set_block.z, call->payload.set_block.local_revision,
				call->payload.set_block.local_previous_content, false);

			break;
		case CRPC_PISTON_MOVING: {
			if(!clin_terrain_gate_open())
				break;
			struct piston_moving_state moving;
			if(!piston_moving_wire_decode(
				   &call->payload.piston_moving.state, &moving)
			   || !client_piston_moving_upsert(
				   call->payload.piston_moving.x,
				   call->payload.piston_moving.y,
				   call->payload.piston_moving.z, &moving)) {
				clin_terrain_install_failed();
				break;
			}
			struct block_data current = world_get_block(
				CW_WORLD, call->payload.piston_moving.x,
				call->payload.piston_moving.y,
				call->payload.piston_moving.z);
			if(current.type != BLOCK_PISTON_MOVING
			   || current.metadata != moving.stored_meta) {
				world_set_block(
					CW_WORLD, call->payload.piston_moving.x,
					call->payload.piston_moving.y,
					call->payload.piston_moving.z,
					(struct block_data) {
						.type = BLOCK_PISTON_MOVING,
						.metadata = moving.stored_meta,
					},
					true);
			}
		} break;
		case CRPC_SPAWN_ITEM: {
			if(!clin_payload_apply_ready())
				break;
			struct entity* e = clin_entity_spawn_alloc(
				call->payload.spawn_item.entity_id, "item");
			if(!e)
				break;
			entity_item(call->payload.spawn_item.entity_id, e, false,
						CW_WORLD, call->payload.spawn_item.item);
			e->teleport(e, call->payload.spawn_item.pos);
			glm_vec3_copy(call->payload.spawn_item.vel, e->vel);
		} break;
		case CRPC_SPAWN_MONSTER: {
			if(!clin_payload_apply_ready())
				break;
			struct entity* e = clin_entity_spawn_alloc(
				call->payload.spawn_monster.entity_id, "monster");
			if(!e)
				break;
			entity_monster(call->payload.spawn_monster.entity_id, e, false,
						CW_WORLD, call->payload.spawn_monster.monster_id);
			e->data.monster.windup_cue = gstate.remote_session
				? (struct mob_windup_cue) {0}
				: call->payload.spawn_monster.windup_cue;
			e->data.monster.fleece_color
				= call->payload.spawn_monster.fleece_color;
			e->data.monster.sheared = call->payload.spawn_monster.sheared;
			e->data.monster.saddled = call->payload.spawn_monster.saddled;
			e->data.monster.wolf_flags
				= call->payload.spawn_monster.wolf_flags;
			e->data.monster.wolf_watched_health
				= call->payload.spawn_monster.wolf_health;
			e->data.monster.wolf_shake_armed
				= call->payload.spawn_monster.wolf_shake_armed;
			e->data.monster.wolf_shake_active
				= call->payload.spawn_monster.wolf_shake_active;
			e->data.monster.wolf_shake_time
				= call->payload.spawn_monster.wolf_shake_time;
			e->data.monster.wolf_prev_shake_time
				= call->payload.spawn_monster.wolf_prev_shake_time;
			e->data.monster.wolf_head_rotation
				= call->payload.spawn_monster.wolf_head_rotation;
			e->data.monster.wolf_head_rotation_old
				= call->payload.spawn_monster.wolf_head_rotation_old;
			e->data.monster.charged = call->payload.spawn_monster.charged;
			e->teleport(e, call->payload.spawn_monster.pos);
		} break;

		case CRPC_SPAWN_PLAYER: {
			if(!clin_payload_apply_ready())
				break;
			struct entity* e = clin_entity_spawn_alloc(
				call->payload.spawn_player.entity_id, "player");
			if(!e)
				break;
			entity_remote_player(call->payload.spawn_player.entity_id, e,
								 CW_WORLD, call->payload.spawn_player.username);
			e->orient[0] = call->payload.spawn_player.yaw;
			e->orient[1] = call->payload.spawn_player.pitch;
			glm_vec2_copy(e->orient, e->orient_old);
			if(call->payload.spawn_player.held_item_id != 0) {
				e->data.remote_player.has_held_item = true;
				e->data.remote_player.held_item = (struct item_data) {
					.id = call->payload.spawn_player.held_item_id,
					.durability = 0,
					.count = 1,
				};
			}
			e->teleport(e, call->payload.spawn_player.pos);
		} break;
		case CRPC_ENTITY_EQUIPMENT: {
			struct entity** pp = dict_entity_get(
				CW_ENTITIES, call->payload.entity_equipment.entity_id);
			if(!pp || !*pp || (*pp)->type != ENTITY_REMOTE_PLAYER)
				break; // no armor/equipment render exists for any OTHER
						// entity type yet; only the remote-player is tracked.
			struct entity* e = *pp;
			if(call->payload.entity_equipment.slot == 0) {
				if(call->payload.entity_equipment.item_id == 0) {
					e->data.remote_player.has_held_item = false;
					memset(&e->data.remote_player.held_item, 0,
						  sizeof(e->data.remote_player.held_item));
				} else {
					e->data.remote_player.has_held_item = true;
					e->data.remote_player.held_item = (struct item_data) {
						.id = call->payload.entity_equipment.item_id,
						.durability
						= (uint16_t)call->payload.entity_equipment.item_damage,
						.count = 1,
					};
				}
				break;
			}
			// Animation-parity pass item 5: armor slots 1..4, previously
			// discarded ("armor slots 1..4: tracked nowhere client-side yet").
			// mp_wire_armor_slot_to_type (mp_entity_flags.h) is the same
			// wire-slot -> enum armor_type mapping this comment cites,
			// factored out so tests/test_mp_animation_parity_wire.c can
			// golden-test it directly. [740c583
			// EntityPlayerMP.getEquipmentInSlot: slot N (1..4) ->
			// inventory.armorInventory[N-1]] cross-checked against
			// RenderPlayer.setArmorModel's armorItemInSlot(3-var2), which
			// proves armorInventory index 0=boots, 1=leggings, 2=chestplate,
			// 3=helmet. entity.h's entity_remote_player.armor_id field
			// comment carries the same citation.
			int at = mp_wire_armor_slot_to_type(call->payload.entity_equipment.slot);
			if(at >= 0)
				e->data.remote_player.armor_id[at]
					= call->payload.entity_equipment.item_id;
		} break;
		case CRPC_ENTITY_ANIMATION: {
			struct entity** pp = dict_entity_get(
				CW_ENTITIES, call->payload.entity_animation.entity_id);
			// MP visual-parity pass item 3: animate==1 (arm swing) now arms the
			// SAME state machine Beta's own EntityPlayer.swingItem() does
			// [740c583]: swingProgressInt=-1, isSwinging=true. Every subsequent
			// tick (entity_remote_player_client_tick) advances the 8-tick cycle
			// and rederives swing_progress, exactly mirroring
			// EntityPlayer.updatePlayerActionState. Codes 2 (hurt)/3 (wake) have
			// no remote render effect modeled (documented residual, matching
			// the pre-existing no-op); 4 (crouch) is intentionally left alone --
			// see CRPC_ENTITY_FLAGS below for why that code is a genuine Beta
			// no-op, not a gap.
			if(pp && *pp && (*pp)->type == ENTITY_REMOTE_PLAYER
			   && call->payload.entity_animation.use_bed) {
				/* MP-BED: Packet17 for another player -> sleepInBedAt
				 * (NetClientHandler.java:370-378). World.blockExists and the
				 * bed metadata come from this client's world. */
				int32_t bx = call->payload.entity_animation.bed_x;
				int32_t by = call->payload.entity_animation.bed_y;
				int32_t bz = call->payload.entity_animation.bed_z;
				bool exists = by >= 0 && by < WORLD_HEIGHT
					&& world_find_chunk(CW_WORLD, bx, by, bz) != NULL;
				uint8_t meta = exists
					? world_get_block(CW_WORLD, bx, by, bz).metadata : 0;
				entity_remote_player_sleep_at(*pp, bx, by, bz, exists, meta);
				break;
			}
			if(pp && *pp && (*pp)->type == ENTITY_REMOTE_PLAYER
			   && call->payload.entity_animation.animate == 1) {
				(*pp)->data.remote_player.swing_progress_int = -1;
				(*pp)->data.remote_player.is_swinging = true;
			}
			/* MP-BED: animate 3 = wakeUpPlayer(false, false, false)
			 * (NetClientHandler.java:359-361). */
			if(pp && *pp && (*pp)->type == ENTITY_REMOTE_PLAYER
			   && call->payload.entity_animation.animate == 3)
				entity_remote_player_wake(*pp);
		} break;
		case CRPC_ENTITY_FLAGS: {
			// MP visual-parity pass item 1, widened by the animation-parity
			// pass items 3+4: see entity.h's entity_remote_player.{sneaking,
			// is_riding} field comments, entity.h's top-level on_fire field
			// comment, and server_remote_protocol.c's
			// MP_EVENT_ENTITY_METADATA case for the full DataWatcher
			// citation. on_fire lives on the entity itself; remote mobs
			// receive it here from Packet40, while local mob MOVEs carry it.
			// sneaking/is_riding are remote_player-only render state.
			struct entity** pp = dict_entity_get(
				CW_ENTITIES, call->payload.entity_flags.entity_id);
			if(pp && *pp) {
				if((*pp)->type == ENTITY_REMOTE_PLAYER) {
					(*pp)->data.remote_player.sneaking
						= call->payload.entity_flags.sneaking;
					(*pp)->data.remote_player.is_riding
						= call->payload.entity_flags.is_riding;
					(*pp)->on_fire = call->payload.entity_flags.on_fire;
				} else if(gstate.remote_session
					  && (*pp)->type == ENTITY_MONSTER) {
					/* Beta Entity.isBurning reads watcher byte 0 bit 0;
					 * Packet40 updates the same watcher for every entity kind.
					 * [Entity:1018-1039; NetClientHandler:172-178] */
					(*pp)->on_fire = call->payload.entity_flags.on_fire;
				}
			}
		} break;
		case CRPC_ENTITY_LOOK: {
			// Same gate as CRPC_ENTITY_EQUIPMENT: only ENTITY_REMOTE_PLAYER and
			// ENTITY_MONSTER currently consume an orientation update this way
			// (2026-08-26 residual item 2 removed the players-only gate).
			// Every other type has no orientation consumer at all -- gate
			// those out explicitly rather than leave a silent dead store.
			struct entity** pp = dict_entity_get(
				CW_ENTITIES, call->payload.entity_look.entity_id);
			if(!pp || !*pp)
				break;
			struct entity* e = *pp;
			if(e->type == ENTITY_REMOTE_PLAYER) {
				if(call->payload.entity_look.approach_ticks > 0) {
					/* Beta increments approach: target only -- orient/orient_old
					 * stay owned by the entity tick so the render layer keeps a
					 * real previous sample to lerp from (the old direct write
					 * clobbered orient_old here, making rotation step per packet
					 * instead of interpolating). [740c583
					 * EntityOtherPlayerMP.onLivingUpdate] */
					e->network_orient[0] = call->payload.entity_look.yaw;
					e->network_orient[1] = call->payload.entity_look.pitch;
					e->network_increments = call->payload.entity_look.approach_ticks;
				} else {
					e->orient[0] = call->payload.entity_look.yaw;
					e->orient[1] = call->payload.entity_look.pitch;
					glm_vec2_copy(e->orient, e->orient_old);
				}
			} else if(e->type == ENTITY_MONSTER) {
				/* A mob's head yaw is a SEPARATE dedicated pair from
				 * network_orient/network_increments above -- those stay
				 * position-only for a mob and use the player-frame yaw
				 * convention; see entity.h's network_head_yaw comment and
				 * entity_monster.c's client_tick_monster consumer. yaw here
				 * is already engine-frame degrees (the same conversion
				 * CRPC_SPAWN_PLAYER's own yaw carries), so one glm_rad
				 * matches head_yaw's own atan2(dx,dz)-radians convention. */
				if(call->payload.entity_look.approach_ticks > 0) {
					e->data.monster.network_head_yaw
						= glm_rad(call->payload.entity_look.yaw);
					e->data.monster.network_head_increments
						= call->payload.entity_look.approach_ticks;
				} else {
					e->data.monster.head_yaw
						= glm_rad(call->payload.entity_look.yaw);
					e->data.monster.network_head_increments = 0;
				}
			}
		} break;

		case CRPC_SIGN_TEXT:
			sign_text_store_set(call->payload.sign_text.x,
								call->payload.sign_text.y,
								call->payload.sign_text.z,
								call->payload.sign_text.grid);
			break;
		case CRPC_SIGN_TEXT_REMOVED:
			sign_text_store_remove(call->payload.sign_text_removed.x,
								   call->payload.sign_text_removed.y,
								   call->payload.sign_text_removed.z);
			break;

		case CRPC_MINECART_OCCUPANCY: {
			/* F3: the single authoritative occupancy writer after spawn. The
			 * correlated reply retires the centralized ride intent when its
			 * epoch/sequence pair matches the pending request. An unmatched
			 * older reply cannot reverse a newer local prediction. */
			uint32_t in_epoch
				= call->payload.minecart_occupancy.input_epoch;
			uint32_t in_seq
				= call->payload.minecart_occupancy.request_seq;
			uint32_t in_entity_id
				= call->payload.minecart_occupancy.entity_id;

			/* Retire the pending intent only on exact correlation match
			 * AND target cart match. The entity_id check prevents
			 * same-epoch sequence reuse after a target-destroy reset
			 * from retiring the wrong request. */
			if(gstate.players[0].ride.pending
			   && in_entity_id == gstate.players[0].ride.target_id
			   && in_epoch == gstate.players[0].ride.input_epoch
			   && in_seq == gstate.players[0].ride.pending_request_seq) {
				/* Matching empty response: clear pending state and ensure
				 * the exit floor is respected exactly once. */
				if(!call->payload.minecart_occupancy.occupied
				   && gstate.players[0].ride.exit_lift_pending
				   && gstate.players[0].ride.exit_repin_eligible
				   && gstate.players[0].local_player) {
					/* Lifecycle criterion 12:
					 * raise the player only on an observed local
					 * re-pin, not on natural fall below the floor. */
					if(gstate.players[0].local_player->pos[1]
					   < gstate.players[0].ride.exit_floor_y) {
						gstate.players[0].local_player->pos[1]
							= gstate.players[0].ride.exit_floor_y;
						gstate.players[0].local_player->pos_old[1]
							= gstate.players[0].ride.exit_floor_y;
					}
				}
				gstate.players[0].ride.pending = false;
				gstate.players[0].ride.exit_lift_pending = false;
				gstate.players[0].ride.exit_repin_eligible = false;
				gstate.players[0].ride.send_dirty = false;
			}

			/* An unmatched reply for the SAME target cart while a newer
			 * intent is pending is an older causal result: it may not
			 * reverse the prediction. An unmatched reply for a DIFFERENT
			 * cart (e.g. cart A's zero/zero release while cart B's mount
			 * is pending) must still apply normally. */
			if(gstate.players[0].ride.pending
			   && in_entity_id == gstate.players[0].ride.target_id
			   && (in_epoch != gstate.players[0].ride.input_epoch
			   || in_seq != gstate.players[0].ride.pending_request_seq))
				break;

			/* AUTHORITY WINS over the local prediction. The server may have
			 * refused the ride intent; this is the correction, and it is
			 * unconditional in BOTH directions. */
			struct entity** e_ptr = dict_entity_get(
				CW_ENTITIES, in_entity_id);
			if(!e_ptr || !*e_ptr || (*e_ptr)->type != ENTITY_MINECART)
				break;
			struct entity* cart = *e_ptr;
			uint32_t epoch = call->payload.minecart_occupancy.ride_epoch;
			bool occupied
				= call->payload.minecart_occupancy.occupied;
			if(!clin_minecart_epoch_acceptable(
				   epoch, cart->data.minecart.ride_epoch, occupied))
				break;
			/* F4: ownership/session transitions (dismount, mount rejection,
			 * epoch change) hard-snap and clear the camera presentation
			 * offset so a stale correction cannot carry into the walking
			 * or new-session camera (design section 6). */
			if(epoch != cart->data.minecart.ride_epoch
			   || occupied != cart->data.minecart.occupied) {
				/* F4 invariant 8: ownership/session transition
				 * with no new body delta. Shift stored camera by -C (the
				 * currently-applied presentation offset) then clear it, so
				 * the pre-attach wire sample is the walking/new-session
				 * position, not the old riding position with offset baked
				 * in. */
				gstate.players[0].camera.x -= gstate.players[0].ride.camera_correction[0];
				gstate.players[0].camera.y -= gstate.players[0].ride.camera_correction[1];
				gstate.players[0].camera.z -= gstate.players[0].ride.camera_correction[2];
				glm_vec3_zero(gstate.players[0].ride.camera_correction);
				gstate.players[0].ride.camera_correction_active = false;
			}
			/* O07 / R04 D1-1: an authoritative END of the latched cart's
			 * ride, or its REPLACEMENT by another ride (a nonzero epoch
			 * changing), retires a pending host ride action taken in it. The
			 * optimistic mount's confirmation (epoch 0 -> N, still occupied)
			 * is the same ride and keeps it. */
			if(gstate.players[0].ride.action_latched
			   && gstate.players[0].ride.action_cart_id == cart->id
			   && (!occupied
				   || (cart->data.minecart.ride_epoch != 0
					   && epoch != cart->data.minecart.ride_epoch)))
				gstate.players[0].ride.action_latched = false;
			if(epoch != cart->data.minecart.ride_epoch) {
				cart->data.minecart.ride_epoch = epoch;
				cart->data.minecart.control_seq = 0;
				cart->data.minecart.control_positive_steps = 0;
				cart->data.minecart.control_negative_steps = 0;
				cart->data.minecart.control_acked_seq = 0;
				cart->data.minecart.control_acked_positive_steps = 0;
				cart->data.minecart.control_acked_negative_steps = 0;
				cart->data.minecart.control_dirty = false;
			}
			cart->data.minecart.occupied
				= call->payload.minecart_occupancy.occupied;
			cart->data.minecart.occupant_id
				= call->payload.minecart_occupancy.occupant_id;
			cart->data.minecart.pending_throttle = 0;
			cart->data.minecart.replay_throttle = 0;
			cart->data.minecart.control_ready = occupied && epoch != 0;
			cart->rightClickText
				= cart->data.minecart.occupied ? "Dismount" : "Ride";
			break;
		}
		case CRPC_SPAWN_MINECART: {
			if(!clin_payload_apply_ready())
				break;
		    struct entity* e = clin_entity_spawn_alloc(
				call->payload.spawn_minecart.entity_id, "minecart");
			if(!e)
				break;

		    entity_minecart(call->payload.spawn_minecart.entity_id,
		                    e,
		                    false,
		                    CW_WORLD);
			if(!clin_apply_minecart_state(
				   e, &call->payload.spawn_minecart, true)) {
				free(e);
				dict_entity_erase(
					CW_ENTITIES,
					call->payload.spawn_minecart.entity_id);
			}
		} break;
		case CRPC_MINECART_SNAPSHOT: {
			struct entity** pp = dict_entity_get(
				CW_ENTITIES,
				call->payload.minecart_snapshot.entity_id);
			struct entity* e = pp ? *pp : NULL;
			if(e && e->type == ENTITY_MINECART)
				(void)clin_apply_minecart_state(
					e, &call->payload.minecart_snapshot, false);
		} break;

		case CRPC_SPAWN_ARROW: {
			if(!clin_payload_apply_ready())
				break;
			struct entity* e = clin_entity_spawn_alloc(
				call->payload.spawn_arrow.entity_id, "arrow");
			if(!e)
				break;
			entity_arrow(call->payload.spawn_arrow.entity_id, e, false,
						 CW_WORLD, 0, call->payload.spawn_arrow.item_id, 0,
						 false);
			e->data.arrow.fireball = call->payload.spawn_arrow.fireball;
			e->teleport(e, call->payload.spawn_arrow.pos);
			glm_vec3_copy(call->payload.spawn_arrow.vel, e->vel);
			if((!e->data.arrow.item_id || e->data.arrow.item_id == ITEM_ARROW)
			   && !e->data.arrow.fireball && call->payload.spawn_arrow.visual.valid) {
				e->data.arrow.visual = call->payload.spawn_arrow.visual;
				e->data.arrow.visual_pending = true;
			}
		} break;

		case CRPC_SPAWN_PRIMED_TNT: {
			if(!clin_payload_apply_ready())
				break;
			struct entity* e = clin_entity_spawn_alloc(
				call->payload.spawn_primed_tnt.entity_id, "primed TNT");
			if(!e)
				break;
			entity_primed_tnt(call->payload.spawn_primed_tnt.entity_id, e,
							  false, CW_WORLD,
							  call->payload.spawn_primed_tnt.fuse);
			e->teleport(e, call->payload.spawn_primed_tnt.pos);
			glm_vec3_copy(call->payload.spawn_primed_tnt.vel, e->vel);
		} break;
		case CRPC_PRIMED_TNT_CORRECTION: {
			if(!clin_payload_apply_ready())
				break;
			struct entity** pp = dict_entity_get(
				CW_ENTITIES,
				call->payload.primed_tnt_correction.entity_id);
			struct entity* e = pp ? *pp : NULL;
			if(!e || e->type != ENTITY_PRIMED_TNT)
				break;
			glm_vec3_copy(call->payload.primed_tnt_correction.pos, e->pos);
			glm_vec3_copy(e->pos, e->pos_old);
			glm_vec3_copy(e->pos, e->network_pos);
			glm_vec3_copy(call->payload.primed_tnt_correction.vel, e->vel);
			e->data.primed_tnt.fuse
				= call->payload.primed_tnt_correction.fuse;
			e->data.primed_tnt.awaiting_destroy = false;
		} break;

		case CRPC_SPAWN_FALLING_SAND: {
			if(!clin_payload_apply_ready())
				break;
			struct entity* e = clin_entity_spawn_alloc(
				call->payload.spawn_falling_sand.entity_id, "falling sand");
			if(!e)
				break;
			entity_falling_sand(call->payload.spawn_falling_sand.entity_id, e,
								false, CW_WORLD,
								call->payload.spawn_falling_sand.block_id);
			e->teleport(e, call->payload.spawn_falling_sand.pos);
			glm_vec3_copy(call->payload.spawn_falling_sand.vel, e->vel);
		} break;

		case CRPC_SPAWN_BOAT: {
			if(!clin_payload_apply_ready())
				break;
			struct entity* e = clin_entity_spawn_alloc(
				call->payload.spawn_boat.entity_id, "boat");
			if(!e)
				break;
			entity_boat(call->payload.spawn_boat.entity_id, e, false,
					   CW_WORLD);
			e->teleport(e, call->payload.spawn_boat.pos);
			glm_vec3_copy(call->payload.spawn_boat.vel, e->vel);
			e->orient[0] = call->payload.spawn_boat.yaw;
			e->orient_old[0] = call->payload.spawn_boat.yaw;
			e->on_ground = call->payload.spawn_boat.on_ground;
			e->data.boat.damage_taken = call->payload.spawn_boat.damage_taken;
			e->data.boat.shake_ticks = call->payload.spawn_boat.shake_ticks;
			e->data.boat.shake_dir = call->payload.spawn_boat.shake_dir;
			e->data.boat.occupied = call->payload.spawn_boat.occupied;
			e->data.boat.occupant_id = call->payload.spawn_boat.occupant_id;
			e->rightClickText = e->data.boat.occupied ? "Dismount" : "Ride";
		} break;
		case CRPC_SPAWN_PAINTING: {
			if(!clin_payload_apply_ready())
				break;
			struct entity* e = clin_entity_spawn_alloc(
				call->payload.spawn_painting.entity_id, "painting");
			if(!e)
				break;
			// entity_painting() itself sets pos/orient/data.painting.* and
			// e->render (entity_painting.c) -- matches the SP/server-local
			// construction call one-for-one, just server=false.
			entity_painting(call->payload.spawn_painting.entity_id, e, false,
							CW_WORLD, call->payload.spawn_painting.art_index,
							call->payload.spawn_painting.direction,
							call->payload.spawn_painting.tile_x,
							call->payload.spawn_painting.tile_y,
							call->payload.spawn_painting.tile_z);
		} break;
		case CRPC_BOAT_CORRECTION: {
			if(!clin_payload_apply_ready())
				break;
			struct entity** pp = dict_entity_get(
				CW_ENTITIES, call->payload.boat_state.entity_id);
			struct entity* e = pp ? *pp : NULL;
			if(!e || e->type != ENTITY_BOAT)
				break;
			glm_vec3_copy(call->payload.boat_state.pos, e->pos);
			glm_vec3_copy(e->pos, e->pos_old);
			glm_vec3_copy(e->pos, e->network_pos);
			glm_vec3_copy(call->payload.boat_state.vel, e->vel);
			e->orient[0] = call->payload.boat_state.yaw;
			e->orient_old[0] = call->payload.boat_state.yaw;
			e->on_ground = call->payload.boat_state.on_ground;
			e->data.boat.damage_taken = call->payload.boat_state.damage_taken;
			e->data.boat.shake_ticks = call->payload.boat_state.shake_ticks;
			e->data.boat.shake_dir = call->payload.boat_state.shake_dir;
			e->data.boat.occupied = call->payload.boat_state.occupied;
			e->data.boat.occupant_id = call->payload.boat_state.occupant_id;
			e->data.boat.awaiting_destroy = false;
			e->rightClickText = e->data.boat.occupied ? "Dismount" : "Ride";
		} break;

		case CRPC_PICKUP_ITEM: {
			/* MP visual-parity pass item 4: arm the cosmetic pickup-flight
			 * instead of the old network_pos nudge, which was a DEAD STORE for
			 * ENTITY_ITEM (its client tick never reads network_pos -- only
			 * CRPC_ENTITY_MOVE's remote_base_entity_snap does that, and no MOVE
			 * packet is guaranteed between this event and the CRPC_ENTITY_DESTROY
			 * that follows it in the SAME MP_EVENT_COLLECT dispatch). Freeze the
			 * item's CURRENT position as the flight start [740c583
			 * EntityPickupFX ctor: entityToPickUp.posX/Y/Z, frozen once
			 * removeEntityFromWorld stops it ticking] and arm the 3-tick flight;
			 * entity_item.c's entity_client_tick/entity_render do the rest, and
			 * CRPC_ENTITY_DESTROY below now defers the free instead of doing it
			 * inline. Scope matches the pre-existing nudge: LOCAL collector only
			 * -- a bystander watching someone else's pickup still sees the
			 * instant destroy (documented residual, not a silent gap). */
			if(gstate.players[0].local_player
			   && call->payload.pickup_item.collector_id
				   == gstate.players[0].local_player->id) {
				struct entity** pp = dict_entity_get(
					CW_ENTITIES, call->payload.pickup_item.entity_id);
				/* SCOPE: MP-only (!on_server) -- a SP item is server-authoritative
				 * (entity_item.c's entity_server_tick both fires this same CRPC
				 * AND arms its own e->delay_destroy=1 one-tick reap in the SAME
				 * call; entities_client_tick's `(delay_destroy==0) ||
				 * tick_client(e)` short-circuit would free the entity before this
				 * flight's 3-tick age ever completed, wasting the arm and
				 * confusingly racing two independent destroy timers). SP keeps its
				 * existing one-tick behaviour untouched. */
				if(pp && *pp && (*pp)->type == ENTITY_ITEM && !(*pp)->on_server) {
					glm_vec3_copy((*pp)->pos,
								 (*pp)->data.item.pickup_flight_start);
					(*pp)->data.item.pickup_flight_active = true;
					(*pp)->data.item.pickup_flight_age = 0;
				}
			}
		} break;
		case CRPC_ITEM_COUNT: {
			// Partial-pickup re-sync: mirror the server's reduced ground-stack
			// count so the 1/2/3/4-sprite stack render matches what is actually
			// left. Unknown id (client dropped the spawn on OOM) is a no-op.
			struct entity** pp = dict_entity_get(
				CW_ENTITIES, call->payload.item_count.entity_id);
			if(pp && *pp && (*pp)->type == ENTITY_ITEM)
				(*pp)->data.item.item.count = call->payload.item_count.count;
		} break;
		case CRPC_ENTITY_DESTROY:
			clin_entity_destroy_one(call->payload.entity_destroy.entity_id);
			break;
		case CRPC_ENTITY_DESTROY_BATCH: {
			/* S5: one evicted chunk's whole dynamic-entity set, carried in a
			 * SINGLE inbox envelope so the server can admit it all-or-nothing
			 * without blocking (a 17-entity chunk cannot reserve 17 of the 16
			 * inbox tokens). The aggregation is TRANSPORT ONLY: the client
			 * still applies -- and still OBSERVES -- one destroy EVENT per id,
			 * in the server's order, through the same clin_entity_destroy_one
			 * body the scalar RPC uses. The per-event column-observer bracket
			 * is replayed here for that reason: dispatching N events must
			 * produce N event boundaries, exactly as N scalar RPCs did, so the
			 * client-visible stream is unchanged by the envelope change. (The
			 * settle bracket is NOT replayed -- it tracks inbox envelopes, and
			 * this is one envelope.) Then release the transferred array: this
			 * envelope is its last owner. */
			uint32_t* ids = call->payload.entity_destroy_batch.ids;
			size_t count = call->payload.entity_destroy_batch.count;
			for(size_t i = 0; i < count; i++) {
				struct client_rpc event = {
					.type = CRPC_ENTITY_DESTROY,
					.payload.entity_destroy.entity_id = ids[i],
				};
				if(clin_column_observe_before_rpc)
					clin_column_observe_before_rpc(&event);
				clin_entity_destroy_one(ids[i]);
				if(clin_column_observe_rpc)
					clin_column_observe_rpc(&event);
			}
			clin_entity_destroy_batch_payload_release(call);
		} break;
		case CRPC_ENTITY_MOVE: {
			struct entity** pp = dict_entity_get(
				CW_ENTITIES, call->payload.entity_move.entity_id);
			struct entity* e = pp ? *pp : NULL;
			if(e) {
				if(call->payload.entity_move.windup_cue_only) {
					if(e->type == ENTITY_MONSTER && !gstate.remote_session
					   && call->payload.entity_move.windup_cue.version
					       == MOB_WINDUP_CUE_VERSION)
						e->data.monster.windup_cue = call->payload.entity_move.windup_cue;
					break;
				}
				glm_vec3_copy(call->payload.entity_move.pos, e->network_pos);
				if(e->type == ENTITY_ARROW && !e->data.arrow.fireball
				   && (!e->data.arrow.item_id || e->data.arrow.item_id == ITEM_ARROW)
				   && call->payload.entity_move.arrow_visual.valid) {
					e->data.arrow.visual = call->payload.entity_move.arrow_visual;
					e->data.arrow.visual_pending = true;
				}
				/* Base (non-living) entities: Beta SNAPS these at packet
				 * time and lets their local physics run between packets --
				 * see remote_base_entity_snap's header. Their client ticks
				 * never read network_pos/network_increments, so before
				 * this the wire position was a DEAD STORE for them and
				 * items/boats/minecarts/arrows/sand/TNT drifted unbounded
				 * on real servers until destroy/respawn (the
				 * betamc-readiness residual). SP is untouched:
				 * server_local never emits CRPC_ENTITY_MOVE.
				 * (Egg/fireball/fish ride entity_default_client_tick and
				 * already follow network_pos; painting is static;
				 * monster/remote-player are the EntityLiving approach
				 * consumers.) */
				switch(e->type) {
					case ENTITY_ITEM:
					case ENTITY_BOAT:
					case ENTITY_MINECART:
					case ENTITY_ARROW:
					case ENTITY_FALLING_SAND:
					case ENTITY_PRIMED_TNT:
						remote_base_entity_snap(e);
						break;
					default: break;
				}
				/* Beta increments arming: a move WITHOUT an accompanying look
				 * targets the CURRENT rotation (handleEntity's non-rotating
				 * arm passes var2.rotationYaw back as the target), so rebase
				 * network_orient here; a paired CRPC_ENTITY_LOOK drained
				 * right after this call overwrites it with the wire look.
				 * approach_ticks==0 (server_local) leaves the same-tick snap
				 * model untouched. [740c583 NetClientHandler.handleEntity] */
				if(call->payload.entity_move.approach_ticks > 0) {
					glm_vec2_copy(e->orient, e->network_orient);
					e->network_increments
						= call->payload.entity_move.approach_ticks;
				}
				/* Remote mob fire comes from Packet40 metadata. Its MOVE RPC
				 * carries no fire bit, so retain the last watched flag. Local
				 * simulation MOVEs continue to carry their own fire state. */
				if(!gstate.remote_session || e->type != ENTITY_MONSTER)
					e->on_fire = call->payload.entity_move.on_fire;
				e->swelling = call->payload.entity_move.swelling;
				// audit #17f: mirror the server hurt flag so the render thread can
				// drive the red hurt-flash overlay (hurt_time is server-side only).
				// entity_remote_player_render (animation-parity pass item 1) now
				// reads this SAME generic field too -- e->hurt was already set
				// here for every entity type, remote players included; only the
				// render-side consumer was missing.
				e->hurt = call->payload.entity_move.hurt;
				// Animation-parity pass item 2: a remote PLAYER'S death roll
				// mirrors the same networked deathTime a dying MOB gets below,
				// but lives in the remote_player union member (not
				// data.monster) -- see entity.h's entity_remote_player.death_time
				// comment for the full [740c583] citation.
				if(e->type == ENTITY_REMOTE_PLAYER) {
					e->data.remote_player.death_time
						= call->payload.entity_move.death_time;
				}
				// Mirror the server fuse so the creeper swell flash is per-instance
				// (render reads e->data.monster.fuse). Monster-only: don't touch
				// another entity type's union.
				if(e->type == ENTITY_MONSTER) {
					e->data.monster.windup_cue = gstate.remote_session
						? (struct mob_windup_cue) {0}
						: call->payload.entity_move.windup_cue;
					e->data.monster.fuse = call->payload.entity_move.fuse;
					// audit #18 item 3: mirror deathTime so the render thread can flop the
					// dying model over (mob_death_roll_deg). 0 for a living mob.
					e->data.monster.death_time
						= call->payload.entity_move.death_time;
					// M4 S2: mirror the sheep sheared flag so a live shear drops the
					// wool overlay on the next MOVE (monster branch only: the field
					// lives in the entity_data union). fleece_color rides along so
					// a live dye recolors the wool tint too (review F1).
					e->data.monster.sheared
						= call->payload.entity_move.sheared;
					e->data.monster.saddled
						= call->payload.entity_move.saddled;
					e->data.monster.fleece_color
						= call->payload.entity_move.fleece_color;
					// batch 1b: the ghast fire-texture bit (dataWatcher 16).
					e->data.monster.ghast_attacking
						= call->payload.entity_move.ghast_attacking;
					e->data.monster.wolf_flags
						= call->payload.entity_move.wolf_flags;
					e->data.monster.wolf_watched_health
						= call->payload.entity_move.wolf_health;
					e->data.monster.wolf_shake_armed
						= call->payload.entity_move.wolf_shake_armed;
					e->data.monster.wolf_shake_active
						= call->payload.entity_move.wolf_shake_active;
					e->data.monster.wolf_shake_time
						= call->payload.entity_move.wolf_shake_time;
					e->data.monster.wolf_prev_shake_time
						= call->payload.entity_move.wolf_prev_shake_time;
					e->data.monster.wolf_head_rotation
						= call->payload.entity_move.wolf_head_rotation;
					e->data.monster.wolf_head_rotation_old
						= call->payload.entity_move.wolf_head_rotation_old;
					// weather slice: mirror the creeper "powered" flag so a live
					// lightning strike tints an already-spawned creeper immediately
					// (the sheared/fleece_color precedent).
					e->data.monster.charged = call->payload.entity_move.charged;
					// saddle-slice review fix (finding 1): mirror the pig mount
					// relationship so entity_local_player.c's client-side follow
					// (the minecart occupant_id==e->id precedent) can find it.
					/* MP-RIDE: online the pig link is owned by Packet39
					 * (CRPC_ATTACH_ENTITY -> clin_apply_attach); a remote MOVE
					 * carries no rider, so it must not clear it. Local mode
					 * keeps mirroring the server's link exactly as before. */
					if(!gstate.remote_session) {
						e->data.monster.pig_ridden
							= call->payload.entity_move.pig_ridden;
						e->data.monster.pig_rider_id
							= call->payload.entity_move.pig_rider_id;
					}
					e->data.monster.pig_zombie_angry
						= call->payload.entity_move.pig_zombie_angry;
				} else if(e->type == ENTITY_PRIMED_TNT) {
					/* Optional correction compatibility only. Normal primed TNT
					 * advances locally exactly like Beta multiplayer and receives no
					 * per-tick MOVE RPC; smoke belongs to its client tick, not network
					 * delivery. */
					e->data.primed_tnt.fuse = call->payload.entity_move.fuse;
					glm_vec3_copy(call->payload.entity_move.pos, e->pos);
					glm_vec3_copy(e->pos, e->pos_old);
				}
			}
		} break;
		case CRPC_ENTITY_VELOCITY: {
			/* MP residual item 1 (2026-08-26): Packet28EntityVelocity SETS
			 * e->vel outright (Beta's own Entity.setVelocity is a plain
			 * assignment, not an accumulation) for the exact entity kinds
			 * whose client tick already integrates e->vel every frame --
			 * remote_base_entity_snap's own switch, one scope up in
			 * CRPC_ENTITY_MOVE, names the identical set. Every other type
			 * (mob/remote-player/anything else) has no e->vel consumer on
			 * this engine's client tick and is intentionally left alone (see
			 * the CRPC_ENTITY_VELOCITY enum comment for the full citation). */
			struct entity** pp = dict_entity_get(
				CW_ENTITIES, call->payload.entity_velocity.entity_id);
			struct entity* e = pp ? *pp : NULL;
			if(e) {
				switch(e->type) {
					case ENTITY_ITEM:
					case ENTITY_BOAT:
					case ENTITY_MINECART:
					case ENTITY_ARROW:
					case ENTITY_FALLING_SAND:
					case ENTITY_PRIMED_TNT:
						glm_vec3_copy(call->payload.entity_velocity.vel, e->vel);
						break;
					default: break;
				}
			}
		} break;
		case CRPC_ENTITY_PHYSICS_CORRECTION: {
			struct entity** pp = dict_entity_get(
				CW_ENTITIES,
				call->payload.entity_physics_correction.entity_id);
			if(pp && *pp)
				clin_apply_entity_physics_correction(*pp, call);
		} break;
		case CRPC_ENTITY_EXPLODE: {
			if(!clin_payload_apply_ready())
				break;
			// Detonation puffs, generated on THIS (render) thread to avoid racing the
			// server thread on the global particle array (see entity_monster.c /
			// block_tnt.c, which now route their blasts through here).
			particle_generate_explosion_flash(call->payload.entity_explode.pos,
											   call->payload.entity_explode.power);
			particle_generate_explosion_smoke(call->payload.entity_explode.pos,
											   call->payload.entity_explode.power);
		} break;
		case CRPC_SMOKE_PUFF:
			// Render-thread emit for server-signalled cosmetic smoke (TNT fuse
			// trail) -- same boundary rule as ENTITY_EXPLODE above.
			if(clin_payload_apply_ready()) {
				float intensity = call->payload.smoke_puff.intensity;
				if(intensity < 0.0f)
					clin_dispenser_smoke(call->payload.smoke_puff.pos,
										  -(int)intensity - 1);
				else
					particle_generate_smoke(call->payload.smoke_puff.pos,
											intensity);
			}
			break;
		case CRPC_WOLF_FX:
			if(clin_payload_apply_ready())
				particle_generate_wolf_tame_fx(call->payload.wolf_fx.pos,
					call->payload.wolf_fx.hearts);
			break;
		case CRPC_LIGHTNING_BOLT:
			/* Local storm bolt: same client render bolt + flash as an
			 * online Packet71 (render_lightning.h). Local sessions carry
			 * remote generation 0, which main.c's per-tick call passes. */
			if(clin_payload_apply_ready())
				lightning_client_spawn_state(
					call->payload.lightning_bolt.x,
					call->payload.lightning_bolt.y,
					call->payload.lightning_bolt.z,
					call->payload.lightning_bolt.living_time,
					(int64_t)(((uint64_t)call->payload.lightning_bolt.render_seed_hi << 32)
						| call->payload.lightning_bolt.render_seed_lo),
					((uint64_t)call->payload.lightning_bolt.rng_state_hi << 32)
						| call->payload.lightning_bolt.rng_state_lo,
					clin_remote_world_generation(),
					call->payload.lightning_bolt.dimension);
			break;
		case CRPC_DROWN_BUBBLES:
			if(clin_payload_apply_ready())
				particle_generate_drown_bubbles(call->payload.drown_bubbles.pos,
					call->payload.drown_bubbles.vel);
			break;
		case CRPC_WOLF_SPLASH:
			if(clin_payload_apply_ready())
				particle_generate_wolf_splash(call->payload.wolf_splash.pos,
					call->payload.wolf_splash.velocity,
					call->payload.wolf_splash.count);
			break;
		case CRPC_SOUND:
			// S0: render-thread voice start for a server-caused sound -- the
			// SMOKE_PUFF boundary rule applied to the audio voice pool. The
			// server already rolled the Beta volume/pitch; the client owns
			// only the cosmetic variant pick + attenuation/pan (audio.c).
			// S2: ui==true is Beta's non-positional playSoundFX quarter-volume
			// path (portal.trigger/travel) -> audio_play_ui, pos ignored.
			if(clin_payload_apply_ready()) {
				/* Split-dims B2 (Fable R2-2): a world no view shows is not
				 * heard; a world only the guest's view shows is heard at the
				 * guest camera's distance (flag OFF: pos unchanged). */
				vec3 heard;
				if(!clin_sound_position(call->payload.sound.pos, heard))
					break;
				if(call->payload.sound.ui)
					audio_play_ui(call->payload.sound.sound_id,
								  call->payload.sound.vol,
								  call->payload.sound.pitch);
				else if(!s_world_payloads_enabled)
					audio_play3d(call->payload.sound.sound_id,
								 call->payload.sound.pos,
								 call->payload.sound.vol,
								 call->payload.sound.pitch);
				else
					audio_play3d(call->payload.sound.sound_id, heard,
								 call->payload.sound.vol,
								 call->payload.sound.pitch);
			}
			break;
		case CRPC_PLAYER_SET_HEALTH:
			(void)client_session_player_reply(&gstate.players[0], call, time_get());
			break;
		case CRPC_WORLD_SPAWN:
			gstate.world_spawn_x = call->payload.world_spawn.x;
			gstate.world_spawn_y = call->payload.world_spawn.y;
			gstate.world_spawn_z = call->payload.world_spawn.z;
			break;
		case CRPC_RECORD_EVENT:
			// Noteblock/jukebox-slice: Beta world event 1005 routed here,
			// matching CRPC_SOUND's boundary rule -- gated on
			// s_world_reset_ready the SAME way CRPC_SOUND is above, so a
			// stray event mid world-teardown/swap cannot touch the S3
			// voice. record_id 0 = eject/stop; nonzero = the inserted
			// record's item id ("start playing that record") [decompile
			// 740c583: BlockJukeBox.ejectRecord / func_28038_b_,
			// RenderGlobal.playAuxSFX case 1005 -> World.playRecord ->
			// SoundManager.playStreaming]. The jukebox's own x/y/z
			// (call->payload.record_event.pos) is not consumed here --
			// see audio_music.h's positioning-deviation note (records
			// play flat, same as background music, until a positioned
			// voice exists).
			if(clin_payload_apply_ready()) {
				if(call->payload.record_event.record_id == 0)
					audio_music_stop_record();
				else
					audio_music_play_record_for_item(
						call->payload.record_event.record_id);
			}
			break;
	}
	if(vsp_cw_on)
		vsp_client_witness_end(call, &vsp_cw);
}

bool clin_init() {
	if(!column_backing_init()) return false;
	s_guest_bind_seq = 0; /* O07: a new client session has no binding yet */
	memset(&s_guest_reset_applied, 0, sizeof s_guest_reset_applied);
	memset(s_transfer_hold, 0, sizeof(s_transfer_hold));
	s_host_transfer_pane=false;
	s_host_transfer_request_seen=0;
	client_piston_moving_clear();
	s_world_reset_ready = false;
	/* The token pool is rebuilt below, so no envelope survives as held. The
	 * context registry itself is NOT reset: retiring contexts may still owe
	 * lease releases, and the per-slot epoch marks must never restart. */
	s_world_held_call = NULL;
	s_world_routed_call = NULL;
	s_unpack.slot = NULL; /* no pack survives the rebuilt token pool */
	s_world_stream_install_failed = false;
	/* B2 gate 2: the production transport for world-addressed resends. */
	s_world_resend_sender = clin_send_world_resend;
	clin_clear_stream_recovery();
	gstate.players[0].visible_window = -1;
	gstate.players[0].deferred_reopen_window = -1;
	clin_clear_input_handoff_latches();
	s_last_input_epoch = 0;
	/* A fresh transport must not answer with the previous session's split ack:
	 * the switch itself is re-read from the config at boot. */
	s_split_ack = (struct clin_split_ack) {0};
	clin_reset_player_state_latch();
	client_session_host_respawn_transport_reset(&gstate.players[0]);
	minecart_ride_client_state_reset(&gstate.players[0].ride, &gstate.players[0].camera, false);
	gstate.world_switch_pending = false;
	gstate.world_load_failed = false;
	gstate.world_load_failed_reason[0] = 0; /* G6 L1: no stale reason on the next card */
	chat_ring_reset(&gstate.chat_ring);
	if(!tchannel_init(&clin_inbox, RPC_INBOX_SIZE))
		return false;
	if(!tchannel_init(&clin_empty_msg, RPC_INBOX_SIZE)) {
		tchannel_close(&clin_inbox);
		return false;
	}
	g9_probe_rpc_pool(&clin_empty_msg);
	/* order 2: a fresh session starts with an empty pending ring */
	clin_rpc_pending_reset();
#ifdef PLATFORM_WII
	// S5.3 hunt PROBE (temporary, ROUND 6): paint once, right after both
	// channels are live -- see the canary declaration comment above.
	cw_canary_paint(&clin_channel_canary_lead);
	cw_canary_paint(&clin_channel_canary_trail);
#endif

	// All three seeding loops below push exactly N tokens into a fresh
	// N-capacity channel (k-th send sees count k < cap), on the main thread
	// before the server thread exists -- none can find the channel full.
	for(int k = 0; k < RPC_INBOX_SIZE; k++)
		if(!tchannel_send(&clin_empty_msg, rpc_msg + k, true))
			return false;

	// Seed the chunk-RPC slot pool. All CHUNK_RPC_POOL_SIZE (16) slots start free.
	if(!tchannel_init(&rpc_chunk_pool_empty, CHUNK_RPC_POOL_SIZE))
		return false;
	for(int k = 0; k < CHUNK_RPC_POOL_SIZE; k++)
		if(!tchannel_send(&rpc_chunk_pool_empty, rpc_chunk_pool + k, true))
			return false;

	// Seed the far-ring slot pool (M1 deep reach).
	s_far_slot_busy = 0;
	if(!tchannel_init(&far_rpc_pool_empty, FAR_RPC_POOL_SIZE))
		return false;
	for(int k = 0; k < FAR_RPC_POOL_SIZE; k++)
		if(!tchannel_send(&far_rpc_pool_empty, far_rpc_pool + k, true))
			return false;

	// Seed the held-map slot pool + reset the mirror (held-map render slice).
	if(!tchannel_init(&map_rpc_pool_empty, MAP_RPC_POOL_SIZE))
		return false;
	for(int k = 0; k < MAP_RPC_POOL_SIZE; k++)
		if(!tchannel_send(&map_rpc_pool_empty, map_rpc_pool + k, true))
			return false;
	clin_clear_map_data();

	gstate.players[0].player_sync.last_pos_update = time_get();
	return true;
}

/* S5.3 hunt PROBE (temporary, ROUND 6): scan the clin_inbox/clin_empty_msg
 * guard canaries -- see the declaration comment above clin_inbox. Public so
 * main.c's per-marker checkpoint (cw_canary_checkpoint) can call it without
 * clin_inbox/clin_empty_msg/rpc_msg losing their file-scope privacy.
 * Main-thread-only (clin_init/clin_update/clin_try_rpc_send are all
 * main-thread APIs already), writes to the SAME cw_boot_trace.txt the main
 * thread's other markers use -- no new writer added to that file. */
void clin_canary_scan(const char* label) {
#ifdef PLATFORM_WII
	char tag[64];
	snprintf(tag, sizeof(tag), "clin_channel_lead@%s", label ? label : "?");
	cw_canary_scan("sd:/cw_boot_trace.txt", tag, &clin_channel_canary_lead,
				   &cw_clin_canary_budget_lead);
	snprintf(tag, sizeof(tag), "clin_channel_trail@%s", label ? label : "?");
	cw_canary_scan("sd:/cw_boot_trace.txt", tag, &clin_channel_canary_trail,
				   &cw_clin_canary_budget_trail);
#else
	(void)label;
#endif
}

struct chunk_rpc_slot* clin_chunk_slot_acquire(void) {
	/* Streaming can retry a chunk next update without moving any simulation
	 * work. Flush owned slots first, then return busy; never park a tick here. */
	(void)clin_rpc_flush_pending();
	return clin_chunk_slot_try_acquire();
}

struct chunk_rpc_slot* clin_chunk_slot_try_acquire(void) {
	void* slot;
	// Non-blocking: NULL if the pool is currently exhausted. Lets the A4 batch send an
	// extra chunk only when a slot is readily free, never adding a server-thread stall.
	if(!tchannel_receive(&rpc_chunk_pool_empty, &slot, false))
		return NULL;
	if(clin_column_observe_slot) clin_column_observe_slot(1, slot);
	return (struct chunk_rpc_slot*)slot;
}

void clin_chunk_slot_copy_local(struct chunk_rpc_slot* slot,
	const struct server_chunk* sc) {
	assert(slot && !slot->backing && sc && sc->backing);
	struct column_backing* b = sc->backing;
	column_backing_ref(b);
	slot->backing = b;
	thread_stripe_lock(b->mutex);
	memcpy(slot->ids, b->ids, sizeof(slot->ids));
	memcpy(slot->metadata, b->metadata, sizeof(slot->metadata));
	memcpy(slot->lighting_sky, b->lighting_sky, sizeof(slot->lighting_sky));
	memcpy(slot->lighting_torch, b->lighting_torch, sizeof(slot->lighting_torch));
	memcpy(slot->heightmap, b->heightmap, sizeof(slot->heightmap));
	slot->local_revision = column_light_enabled() ? b->revision
		: (struct column_revision){0};
	thread_stripe_unlock(b->mutex);
}

void clin_chunk_slot_release(struct chunk_rpc_slot* slot) {
	assert(slot);
	// Never full: the channel only ever holds the POOL_SIZE pool slots (== its
	// capacity), every acquire is released exactly once (server: acquire ->
	// exactly one CRPC_CHUNK; client: one release per install), and the slot
	// being returned is out of the channel right now.
	if(clin_column_observe_slot) clin_column_observe_slot(-1, slot);
	column_backing_unref(slot->backing);
	slot->backing = NULL;
	slot->local_revision = (struct column_revision){0};
	tchannel_send(&rpc_chunk_pool_empty, slot, true);
}

struct map_rpc_slot* clin_map_slot_try_acquire(void) {
	void* slot;
	// Non-blocking (far-slot precedent): the map sender defers to the next
	// change-scan when both slots are in flight; never a server-thread park.
	if(!tchannel_receive(&map_rpc_pool_empty, &slot, false))
		return NULL;
	return (struct map_rpc_slot*)slot;
}

void clin_map_slot_release(struct map_rpc_slot* slot) {
	assert(slot);
	tchannel_send(&map_rpc_pool_empty, slot, true);
}

/* Explicit success for the pump: failed publication leaves its mirror dirty. */
bool clin_map_deliver(int32_t id, const uint8_t* colors) {
	if(id < INT16_MIN || id > INT16_MAX) return false;
	/* A color-only update must retain a previously spilled icon list. */
	if(!clin_map_recover(id, NULL)) return false;
	struct client_map_data* m = clin_map_find(id);
	if(!m) m = clin_map_allocate(id);
	if(!m) return false;
	memcpy(m->colors, colors, MAP_RPC_COLORS_SIZE);
	m->version = ++s_map_version;
	return true;
}

size_t clin_held_map_markers(int32_t map_id, struct item_map_marker* out) {
	if(!clin_map_recover(map_id, NULL)) return 0;
	struct client_map_data* m = clin_map_find(map_id);
	return m ? item_map_decode_markers(m->marker_bytes, m->marker_length, out) : 0;
}

bool clin_map_markers_deliver(int32_t map_id, const uint8_t* bytes, uint8_t length) {
	if(map_id < INT16_MIN || map_id > INT16_MAX || !bytes || !length
	   || bytes[0] != 1 || !clin_map_recover(map_id, NULL)) return false;
	struct client_map_data* m = clin_map_find(map_id);
	if(!m) m = clin_map_allocate(map_id);
	if(!m) return false;
	memcpy(m->marker_bytes, bytes, length);
	m->marker_length = length;
	return true;
}

/* Retry overflow invalidates the entire identity: never expose stale colors
 * as a complete plane. Subsequent strips re-cover only their addressed pixels. */
void clin_map_forget(int16_t map_id) {
	struct client_map_data** link = &s_client_maps;
	while(*link) {
		struct client_map_data* m = *link;
		if(m->map_id == map_id) {
			*link = m->next;
			free(m); s_map_count--;
			break;
		}
		link = &m->next;
	}
	unsigned id = (uint16_t)map_id;
	if(s_map_spilled[id / 8] & (1u << (id % 8))) {
		char path[96]; clin_map_path(path, map_id);
		remove(path);
		s_map_spilled[id / 8] &= ~(1u << (id % 8));
	}
	++s_map_version;
}

const uint8_t* clin_map_recover(int32_t map_id, uint32_t* out_version) {
	struct client_map_data* m = clin_map_find(map_id);
	unsigned id = (uint16_t)map_id;
	if(!m && map_id >= INT16_MIN && map_id <= INT16_MAX
	   && (s_map_spilled[id / 8] & (1u << (id % 8)))) {
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
		if(s_map_fail_reload) { s_map_io_failures++; return NULL; }
#endif
		char path[96]; clin_map_path(path, map_id);
		FILE* file = fopen(path, "rb");
		if(file) {
			m = clin_map_allocate(map_id);
			if(m && (fread(m->colors, 1, sizeof(m->colors), file) != sizeof(m->colors)
				|| fread(&m->marker_length, 1, 1, file) != 1
				|| fread(m->marker_bytes, 1, m->marker_length, file) != m->marker_length)) {
				/* A failed reload must not install a partial or another ID's plane. */
				s_client_maps = m->next;
				free(m); s_map_count--; m = NULL;
				s_map_io_failures++;
			}
			if(m) m->version = ++s_map_version;
			fclose(file);
		} else s_map_io_failures++;
		if(!m) return NULL; // known data is unavailable, NOT a fresh blank
	}
	if(out_version) *out_version = m ? m->version : s_map_version;
	return m ? m->colors : s_map_blank;
}

const uint8_t* clin_held_map_colors(int32_t map_id, uint32_t* out_version) {
	const uint8_t* colors = clin_map_recover(map_id, out_version);
	if(!colors && out_version) *out_version = s_map_version;
	return colors ? colors : s_map_blank;
}

struct far_rpc_slot* clin_far_slot_try_acquire(void) {
	void* slot;
	// The ONLY far acquire. There is deliberately no blocking variant: the far ring is
	// best-effort, so parking the server thread on this pool buys nothing and risks the
	// bounded-buffer deadlock clin_try_rpc_send already removed from the far SEND. NULL
	// means "pool busy" and the caller defers to the next tick.
	if(!tchannel_receive(&far_rpc_pool_empty, &slot, false)) {
		s_far_slot_busy++;
		return NULL;
	}
	return (struct far_rpc_slot*)slot;
}

uint32_t clin_far_slot_busy_count(void) {
	return s_far_slot_busy;
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
size_t clin_far_slot_test_capacity(void) {
	return FAR_RPC_POOL_SIZE;
}
#endif

void clin_far_slot_release(struct far_rpc_slot* slot) {
	assert(slot);
	// Never full: same conservation as the chunk pool above (POOL_SIZE slots ==
	// capacity; one release per acquire -- the server's gen-deferred bail or
	// exactly one CRPC_FAR_CHUNK that the client releases after install).
	tchannel_send(&far_rpc_pool_empty, slot, true);
}

/* CHIP B (task_18b9ae41, 2026-08-31): the one send site every SRPC_
 * UNLOAD_WORLD caller that needs reliable delivery should route through.
 * svin_rpc_send already retries ordinary backpressure internally; a false
 * return here means its SVIN_LIFECYCLE_RESERVE (or the whole pending
 * buffer) is genuinely saturated -- latch a retry so clin_update keeps
 * resending until the server actually accepts the unload, instead of the
 * caller silently moving on while server_remote stays in REMOTE_PLAY.
 * Returns svin_rpc_send's own result so a caller that wants to gate its OWN
 * transition on the first attempt still can; screen_load_world.c's remote-
 * escape (this fix's target) ignores it and moves on regardless -- the
 * automatic per-frame retry below is what makes that safe. */
bool clin_send_unload_world(void) {
	if(svin_rpc_send(&(struct server_rpc) {.type = SRPC_UNLOAD_WORLD})) {
		s_pending_unload_world_retry = false;
		return true;
	}
	s_pending_unload_world_retry = true;
	return false;
}

/* M7 (2026-08-20 provider-swap redesign, hostile-review finding 7):
 * inbox-drain ONLY, for server_remote's remote_try_crpc retry path. It
 * deliberately does NOT reset clin_dig_burst_budget -- that reset is a
 * per-FRAME allowance (the bulk-clear particle firehose guard, see
 * CRPC_SET_BLOCK), and remote_try_crpc may drain many times per frame
 * under a login burst; going through full clin_update() gave a server
 * explosion 4 particle bursts per retry instead of 4 per frame. */
static void clin_process_queued(struct client_rpc* call) {
#ifdef PLATFORM_WII
	enum client_rpc_type type = call->type;
#endif
	if(client_settle_observe)
		clin_settle_rpc(SET_DEQUEUE, call, call);
	/* The guest-status arm FALLS THROUGH to the ownership backstop below
	 * rather than returning early. A CRPC_LOCAL_GUEST envelope carries no
	 * transferred id array today, so the early return leaked nothing -- but
	 * it made "every dequeued envelope passes the backstop" a fact about the
	 * current payload union instead of a property of this function. Falling
	 * through makes it structural: no route out of clin_process_queued skips
	 * the release. */
	if(call->type == CRPC_LOCAL_GUEST) {
		clin_local_guest_status(call);
	} else {
		uint64_t generation = rpc_guest_generation[call - rpc_msg];
		if(generation) {
			struct client_session* session = &gstate.players[1];
			if(session->local_bound && generation == session->local_generation
			   && !session->local_reply_failed
			   && !client_session_local_reply(session, generation, call))
				session->local_reply_failed = true;
		} else {
			clin_process(call);
#ifdef PLATFORM_WII
			/* AFTER the reset, never during it. */
			if(type == CRPC_WORLD_RESET)
				cw_settle_retire_flush();
#endif
		}
	}
	/* S5 ownership backstop before the fixed envelope is recycled. The
	 * untagged route already released this payload inside its handler (the
	 * release is idempotent); the tagged/stale and guest-status routes above
	 * never reach a handler at all, and eviction batches are never
	 * generation-tagged, so this is where such an envelope gives its array
	 * up. Reached unconditionally -- see the fall-through note above. */
	clin_entity_destroy_batch_payload_release(call);
}

/* Split-dims B1: next envelope for a drain loop -- the one held back last
 * time first (FIFO order), then the inbox. Routes it once: a HOLD verdict
 * keeps the envelope (and its token) aside and ends this drain; any other
 * verdict is handed to clin_process through s_world_routed_call. */
static bool clin_drain_next(void** out) {
	if(s_world_held_call) {
		*out = s_world_held_call;
		s_world_held_call = NULL;
	} else if(!clin_inbox_take(out)) {
		return false;
	}
	struct client_rpc* call = *out;
	struct client_world_context* ctx;
	enum client_ctx_route verdict = clin_world_route(call, &ctx);
	if(verdict == CLIENT_CTX_ROUTE_HOLD) {
		s_world_held_call = call;
		return false;
	}
	s_world_routed_call = call;
	s_world_routed_verdict = verdict;
	s_world_routed_ctx = ctx;
	return true;
}

static bool clin_update_drain_bounded(uint32_t budget_us) {
	ptime_t start = budget_us ? time_get() : (ptime_t){0};
	clin_recover_column_storage();
	(void)client_ctx_registry_poll(&s_world_ctx);
	clin_world_resend_poll(); /* B2 gate 2: no-op flag OFF */
	(void)clin_landing_poll(); /* A2-T landing ready: no-op when idle */
	void* call;
	uint32_t g9_work = 0, g9_backlog, g9_oldest;
	g9_probe_inbox_stats(&g9_backlog, &g9_oldest);
	g9_probe_main_mark(2, true, 0, g9_backlog, g9_oldest);
	bool empty = false;
	while(!budget_us || (g9_work < 16 && time_diff_us(start, time_get()) < budget_us)) {
		if(!clin_drain_next(&call)) { empty = true; break; }
		g9_work++;
		clin_process_queued(call);
		s_world_routed_call = NULL; /* tagged routes never reach clin_process */
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
		if(s_test_consume_observer) s_test_consume_observer(call, s_test_consume_user);
#endif
		/* Token discipline identical to clin_update's drain below. */
		clin_inbox_token_return(call);
	}
	g9_probe_inbox_stats(&g9_backlog, &g9_oldest);
	g9_probe_main_mark(2, false, g9_work, g9_backlog, g9_oldest);
	clin_transfer_hold_release_restored();
	clin_portal_loading_update();
	return empty;
}
void clin_update_drain(void) { (void)clin_update_drain_bounded(0); }
bool clin_update_drain_budget(uint32_t budget_us) {
	return budget_us && clin_update_drain_bounded(budget_us);
}

static enum g9_handoff_result clin_g9_rpc_step(uint32_t budget_us) {
	if(!budget_us) return G9_HANDOFF_MORE;
	if(column_view_recovery_pending() || clin_world_quiesce_pending()
	   || gstate.world_switch_pending || s_pending_unload_world_retry
	   || s_stream_recovery_phase != CLIN_STREAM_RECOVERY_NONE
	   || s_input_presentation_epoch || s_input_ack_retry_epoch
	   || s_input_ack_wait_epoch || s_input_nack_retry_epoch
	   || clin_transfer_actor_held(0) || clin_transfer_actor_held(1))
		return G9_HANDOFF_UNSAFE;
	/* Keep the real FIFO token, including separately pooled payloads. No
	 * lifecycle polling, world unload, allocation or terrain handler here.
	 * Normal service resumes immediately when an unproven head rejects ACK. */
	void* raw;
	if(s_world_held_call) {
		raw = s_world_held_call;
		s_world_held_call = NULL;
	} else if(!clin_inbox_take(&raw)) return G9_HANDOFF_DONE;
	struct client_rpc* call = raw;
	if(rpc_guest_generation[call-rpc_msg]
	   || (call->type != CRPC_TIME_SET && call->type != CRPC_WEATHER_SET)) {
		s_world_held_call = call;
		return G9_HANDOFF_UNSAFE;
	}
	struct client_world_context* ctx;
	enum client_ctx_route verdict = clin_world_route(call, &ctx);
	if(verdict == CLIENT_CTX_ROUTE_HOLD) {
		s_world_held_call = call;
		return G9_HANDOFF_UNSAFE; /* HOLD is not an empty FIFO. */
	}
	s_world_routed_call = call;
	s_world_routed_verdict = verdict;
	s_world_routed_ctx = ctx;
	clin_process_queued(call);
	s_world_routed_call = NULL;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_test_consume_observer) s_test_consume_observer(call, s_test_consume_user);
#endif
	clin_inbox_token_return(call);
	return G9_HANDOFF_MORE; /* Next seam proves that the real FIFO is empty. */
}
enum g9_handoff_result clin_g9_handoff_service(unsigned operation, uint32_t budget_us) {
	if(operation == G9_HANDOFF_RPC) return clin_g9_rpc_step(budget_us);
	if(operation == G9_HANDOFF_MESH) {
#ifdef PLATFORM_WII
		/* One whole result is a structural bound, not a bound on allocator/
		 * release/fence waits. Native admission leaves those real results to
		 * ordinary post-flip retirement until bounded entry points exist. */
		if(chunk_mesher_pending()) return G9_HANDOFF_UNSAFE;
#endif
		if(column_view_recovery_pending() || displaylist_gpu_pending())
			return G9_HANDOFF_UNSAFE;
		bool empty = chunk_mesher_receive_step(budget_us);
		return empty && !chunk_mesher_pending() ? G9_HANDOFF_DONE : G9_HANDOFF_MORE;
	}
	return G9_HANDOFF_UNSAFE;
}
void clin_g9_handoff_poll(bool mesh_safe) {
	(void)thread_g9_handoff_dispatch(G9_HANDOFF_RPC
		| (mesh_safe ? G9_HANDOFF_MESH : 0), clin_g9_handoff_service);
}

static bool s_clin_profile;
static float s_clin_work_ms[4];
void clin_work_profile_enable(bool enabled) { s_clin_profile = enabled; }
void clin_work_stats(float out[4]) { memcpy(out, s_clin_work_ms, sizeof(s_clin_work_ms)); }

/* Beta 1.7.3 EntityClientPlayerMP.onUpdate:31-35 calls func_4056_N once
 * per executed player tick; :71-85 sends a movement variant even when idle. */
/* The host movement sample of `sample` (one wire convention for the
 * remote send and the local per-tick publication). */
static struct server_rpc clin_primary_position_rpc(const struct camera* sample) {
	bool on_ground = gstate.players[0].local_player
		? gstate.players[0].local_player->on_ground : false;
	bool sneaking = gstate.players[0].local_player
		&& gstate.players[0].local_player->data.local_player.capture_input
		&& input_held(IB_SNEAK);
		/* F4: the camera presentation offset must never enter an
		 * authoritative input sample. While it is active, the
		 * SRPC_PLAYER_POS positional fields use the unsmoothed
		 * sample (camera - camera_correction); rotation still
		 * comes from the camera. */
		return (struct server_rpc) {
			.type = SRPC_PLAYER_POS,
			.payload.player_pos.x
				= sample->x
				  - gstate.players[0].ride.camera_correction[0],
			/* ... and the Beta sneak camera DIP is taken back out the
			 * same way (player_ysize.h): CaveWii drops the eye for the
			 * view only, so the wire position -- and therefore the
			 * server's feet (eye - 1.62) and collision box -- stay
			 * BYTE-IDENTICAL standing and sneaking at the same place.
			 * This is a documented, invisible deviation from Beta,
			 * whose posY/stance does carry ySize.
			 *
			 * The fold-out is byte-identical only while that record still
			 * describes the stored camera, which is why every ABSOLUTE
			 * camera writer clears it first (player_ysize_absolute_set:
			 * the two teleport arms at :1050 / :1087 and the guest
			 * install). It matters most on THIS path -- mp_preworld_echo
			 * sends every 50 ms for the whole cross-dimension terrain hold
			 * while camera_attach for player 0 is gated off
			 * (main.c:3906), so nothing re-derives the record in between
			 * and a stale one would ride every packet of the hold rather
			 * than a single frame.
			 * Pinned by tests/test_player_ysize.c. */
			.payload.player_pos.y
				= player_ysize_unapply_y(sample->y,
										 &sample->ysize)
				  - gstate.players[0].ride.camera_correction[1],
			.payload.player_pos.z
				= sample->z
				  - gstate.players[0].ride.camera_correction[2],
			.payload.player_pos.rx = -glm_deg(sample->rx),
			.payload.player_pos.ry = glm_deg(sample->ry) - 90.0F,
			.payload.player_pos.vel_y = gstate.players[0].local_player
				? gstate.players[0].local_player->vel[1]
				: 0.0F,
			// Boat rider steering SOURCE: report this tick's rider
			// input velocity (0,0 whenever not boat-mounted -- the
			// BOAT MOUNT branch in entity_local_player.c is the only
			// writer, and it resets this to 0 on zero input, mirroring
			// Beta's own zero-then-recompute every tick).
			.payload.player_pos.vel_x = gstate.players[0].local_player
				? gstate.players[0].local_player->data.local_player.boat_rider_vel_x
				: 0.0F,
			.payload.player_pos.vel_z = gstate.players[0].local_player
				? gstate.players[0].local_player->data.local_player.boat_rider_vel_z
				: 0.0F,
			.payload.player_pos.on_ground = on_ground,
			.payload.player_pos.sneaking = sneaking,
			.payload.player_pos.correction_seq = gstate.players[0].player_sync.correction_seq,
		};
}

static bool clin_send_primary_position(const struct camera* sample) {
	if(!gstate.remote_session && clin_transfer_actor_held(0)) return false;
	struct server_rpc pos = clin_primary_position_rpc(sample);
	bool accepted = svin_rpc_send(&pos);
	gstate.players[0].player_sync.last_pos_update = time_get();
	return accepted;
}

static uint32_t s_mp_cadence_ticks, s_mp_cadence_samples;
void clin_remote_movement_reset(void) {
	s_mp_cadence_ticks = s_mp_cadence_samples = 0;
}
void clin_remote_movement_stats(uint32_t* ticks, uint32_t* samples) {
	if(ticks) *ticks = s_mp_cadence_ticks;
	if(samples) *samples = s_mp_cadence_samples;
}
void clin_remote_movement_tick_executed(void) {
	s_mp_cadence_ticks++;
}

/* REL-MP-FIX4b: GuiDownloadTerrain.updateScreen:19-21 queues Packet0 every
 * 20th tick (1 s) while the terrain screen is up; CaveWii's equivalents are
 * the remote load screen and the same-dim respawn hold. A vanilla server sends
 * its own Packet0 only after 20 idle ticks (NetServerHandler.java:36) and
 * read-times-out a silent client after 30 s (NetworkManager.java:47;
 * tmp/jobs/triage-1020/close-diag.md run 2). main.c sends it when this says
 * so; the first comes 20 ticks after the screen appears, like updateCounter. */
static bool s_hold_ka_armed;
static ptime_t s_hold_ka_last;
bool clin_remote_hold_keep_alive_due(ptime_t now) {
	bool held = gstate.remote_session && (gstate.remote_terrain_hold
		|| gstate.players[0].current_screen == &screen_load_world);
	if(!held || !s_hold_ka_armed) {
		s_hold_ka_armed = held;
		s_hold_ka_last = now;
		return false;
	}
	if(time_diff_ms(s_hold_ka_last, now) < 20 * 50)
		return false;
	s_hold_ka_last = now;
	return true;
}

void clin_remote_player_tick(const struct camera* sample) {
	/* MP-BED: online the client owns the sleep timer, ticked once per
	 * EntityPlayer.onUpdate (EntityPlayer.java:60-78); the local server
	 * authors it itself in CRPC_SLEEP_STATE, so this is remote-only. */
	if(gstate.remote_session && gstate.world_loaded
	   && !gstate.world_switch_pending)
		gstate.players[0].sleep_timer = client_session_sleep_timer_step(
			gstate.players[0].sleeping, gstate.players[0].sleep_timer);
	if(!sample || gstate.paused || !gstate.remote_session
	   || !gstate.world_loaded || gstate.world_switch_pending
	   || !client_session_host_gameplay_active(&gstate.players[0]))
		return;
	if(clin_send_primary_position(sample)) s_mp_cadence_samples++;
}

/* V6 F1 (ASTRA-V5-REVIEW R1): the LOCAL position producer. The local server
 * replays one physics step per accepted sample, so the producer must never
 * author a sample the transport then loses. It asks svin_motion_room FIRST:
 * with no room it authors nothing and does not advance its cadence
 * (last_pos_update), so clin_update retries next frame -- backpressure before
 * any irreversible producer progress. An authored sample is owned by the
 * transport (inbox token or the owner's bounded motion reserve) until the
 * server drains it. Remote mode: svin_motion_room is always true and the
 * send keeps its drop semantics, so this is the unchanged pre-V6 path. */
static uint32_t s_local_pos_backpressure[2];
uint32_t clin_local_position_backpressure_count(unsigned owner) {
	return owner < 2 ? s_local_pos_backpressure[owner] : 0;
}
enum clin_local_pos_result clin_local_position_produce(unsigned owner) {
	if(owner == 0) {
		if(!svin_motion_room((struct server_rpc_source) {0})) {
			s_local_pos_backpressure[0]++;
			return CLIN_LOCAL_POS_BACKPRESSURE;
		}
		return clin_send_primary_position(&gstate.players[0].camera)
			? CLIN_LOCAL_POS_AUTHORED : CLIN_LOCAL_POS_REFUSED;
	}
	if(owner != 1 || !gstate.players[1].local_player)
		return CLIN_LOCAL_POS_REFUSED;
	if(!svin_motion_room((struct server_rpc_source) {
		   .slot = 1, .generation = gstate.players[1].local_generation})) {
		s_local_pos_backpressure[1]++;
		return CLIN_LOCAL_POS_BACKPRESSURE;
	}

	const struct input_player_frame* guest_input = input_player_frame(1);
	struct server_rpc_source guest_source
		= {.slot = 1, .generation = gstate.players[1].local_generation};
	struct server_rpc guest_pos = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = gstate.players[1].camera.x,
			/* Same sneak-dip fold-out as owner 0 above: the split guest
			 * owns its own dip state (one per client_session) and sends
			 * through this same path, so its wire position must be
			 * byte-identical standing vs sneaking too. */
			.y = player_ysize_unapply_y(gstate.players[1].camera.y,
										&gstate.players[1].camera.ysize),
			.z = gstate.players[1].camera.z,
			.rx = -glm_deg(gstate.players[1].camera.rx),
			.ry = glm_deg(gstate.players[1].camera.ry) - 90.0F,
			.vel_y = gstate.players[1].local_player->vel[1],
			.vel_x = gstate.players[1].local_player
				->data.local_player.boat_rider_vel_x,
			.vel_z = gstate.players[1].local_player
				->data.local_player.boat_rider_vel_z,
			.on_ground = gstate.players[1].local_player->on_ground,
			.sneaking = guest_input && guest_input->down[IB_SNEAK],
			.correction_seq
			= gstate.players[1].player_sync.correction_seq,
		},
	};
	if(svin_rpc_send_from(guest_source, &guest_pos)) {
		gstate.players[1].player_sync.last_pos_update = time_get();
		/* The local partner model reads this owner cache, not the remote
		 * entity flags. Mirror the accepted pose just as owner 0 does. */
		gstate.players[1].player_sync.state_sneaking
			= guest_pos.payload.player_pos.sneaking;
		return CLIN_LOCAL_POS_AUTHORED;
	}
	return CLIN_LOCAL_POS_REFUSED;
}

/* ===== V6 F1 round 3 (ASTRA-F1-DESIGN-RULING 1, 5-6, 10-11): the LOCAL
 * per-executed-tick producer. main.c calls, per owner and per executed tick
 * of the 20 Hz catch-up loop: begin (reserve one movement credit -- and its
 * publication storage -- BEFORE the owner's physics may run), the physics,
 * then end (publish exactly one immutable sample of the body's final pose).
 * A begin that fails SUSPENDS that owner's tick only: its physics and its
 * sample do not happen, nothing is owed later, the other owner, render,
 * menus, input and both RPC pumps go on. Remote sessions keep
 * clin_remote_player_tick; begin is true there and end is not called. */
static struct clin_local_tick_stats s_local_tick[2];
/* 0 = no reservation, 1 = reserved, 2 = owner physics actually ran.
 * Reuse the two reservation bytes; the physics signal adds no storage. */
static uint8_t s_local_tick_reserved[2];
void clin_local_tick_stats(unsigned owner, struct clin_local_tick_stats* out) {
	*out = owner < 2 ? s_local_tick[owner] : (struct clin_local_tick_stats) {0};
}
static struct server_rpc_source clin_local_source(unsigned owner) {
	return owner == 0 ? (struct server_rpc_source) {0}
		: (struct server_rpc_source) {.slot = 1,
			.generation = gstate.players[1].local_generation};
}
bool clin_local_tick_begin(unsigned owner) {
	if(owner > 1)
		return false;
	s_local_tick[owner].attempted++;
	assert(!s_local_tick_reserved[owner]);
	if(!svin_credit_reserve(clin_local_source(owner))) {
		s_local_tick[owner].suspended++;
		s_local_pos_backpressure[owner]++;
		return false;
	}
	s_local_tick_reserved[owner] = true;
	return true;
}
void clin_local_tick_physics_executed(unsigned owner) {
	if(owner < 2 && s_local_tick_reserved[owner])
		s_local_tick_reserved[owner] = 2;
}
bool clin_local_tick_physics_ran(unsigned owner) {
	return owner < 2 && s_local_tick_reserved[owner] == 2;
}
static struct server_rpc clin_guest_position_rpc(const struct camera* sample) {
	const struct input_player_frame* guest_input = input_player_frame(1);
	struct entity* body = gstate.players[1].local_player;
	return (struct server_rpc) {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = sample->x,
			.y = player_ysize_unapply_y(sample->y, &sample->ysize),
			.z = sample->z,
			.rx = -glm_deg(sample->rx),
			.ry = glm_deg(sample->ry) - 90.0F,
			.vel_y = body ? body->vel[1] : 0.0F,
			.vel_x = body ? body->data.local_player.boat_rider_vel_x : 0.0F,
			.vel_z = body ? body->data.local_player.boat_rider_vel_z : 0.0F,
			.on_ground = body ? body->on_ground : false,
			.sneaking = guest_input && guest_input->down[IB_SNEAK],
			.correction_seq = gstate.players[1].player_sync.correction_seq,
		},
	};
}
void clin_local_tick_end(unsigned owner, const struct camera* sample) {
	if(owner > 1 || !s_local_tick_reserved[owner])
		return;
	s_local_tick_reserved[owner] = false;
	struct server_rpc pos = owner == 0 ? clin_primary_position_rpc(sample)
									   : clin_guest_position_rpc(sample);
	svin_credit_publish(clin_local_source(owner), &pos);
	s_local_tick[owner].executed++;
	gstate.players[owner].player_sync.last_pos_update = time_get();
	if(owner == 1)
		gstate.players[1].player_sync.state_sneaking
			= pos.payload.player_pos.sneaking;
}
void clin_local_tick_cancel(unsigned owner) {
	if(owner > 1 || !s_local_tick_reserved[owner])
		return;
	s_local_tick_reserved[owner] = false;
	svin_credit_abandon(clin_local_source(owner));
	s_local_tick[owner].cancelled++;
}

void clin_update() {
	clin_recover_column_storage();
	column_view_pool_shutdown();
	client_session_flush_local(&gstate.players[1]);
	client_session_flush_window_transactions(&gstate.players[0]);
	client_session_flush_window_transactions(&gstate.players[1]);
	if(s_clin_profile) memset(s_clin_work_ms, 0, sizeof(s_clin_work_ms));
	/* Remote terrain hold release (see game_state.h remote_terrain_hold).
	 * Runs every frame regardless of active screen. Position echo keeps
	 * flowing from the held spawn position (the pre-world echo machinery), so
	 * the server keeps streaming the destination chunks; vanilla's
	 * GuiDownloadTerrain pumps the network the same way [740c583
	 * GuiDownloadTerrain.updateScreen]. */
	if(gstate.remote_terrain_hold) {
		struct entity* held = gstate.players[0].local_player;
		if(!gstate.world_loaded || !held) {
			/* World torn down under the hold (disconnect/dim change):
			 * CRPC_WORLD_RESET owns the screen from here. */
			gstate.remote_terrain_hold = false;
		} else {
			w_coord_t held_y = (w_coord_t)floorf(held->pos[1]);
			if(held_y < 0)
				held_y = 0;
			else if(held_y >= WORLD_HEIGHT)
				held_y = WORLD_HEIGHT - 1;
			if(world_find_chunk(CW_WORLD,
								(w_coord_t)floorf(held->pos[0]), held_y,
								(w_coord_t)floorf(held->pos[2]))) {
				/* Destination installed: physics resumes and
				 * screen_lworld_update returns to gameplay -- the player now
				 * falls from spawn height over LOADED terrain, exactly the
				 * field-proven login behaviour. */
				gstate.remote_terrain_hold = false;
			} else {
				/* Belt + suspenders while held: the entities-active predicate
				 * already parks physics, but keep the body inert so nothing
				 * accumulates if another system nudges velocity. */
				held->vel[0] = held->vel[1] = held->vel[2] = 0.0F;
			}
		}
	}
	// First thing each frame: retry player actions queued while the server inbox
	// was full (reliable delivery -- actions are delayed under load, never
	// dropped), so a recovering server drains the backlog at frame rate before
	// this frame's new sends pile on.
	svin_rpc_flush_pending();

	// CHIP B (task_18b9ae41): retry a SATURATED (not merely backlogged --
	// svin_rpc_flush_pending above already covers ordinary backlog) SRPC_
	// WINDOW_CLOSE / SRPC_UNLOAD_WORLD every frame until svin_rpc_send
	// finally accepts it. See s_pending_window_close_retry / clin_send_
	// unload_world's doc comments for why an outright-dropped lifecycle send
	// can't just be forgotten (FH-07 leaked-container-ownership class).
	if(s_pending_window_close_retry >= 0) {
		if(svin_rpc_send(&(struct server_rpc) {
			   .type = SRPC_WINDOW_CLOSE,
			   .payload.window_close.window
			   = (uint8_t)s_pending_window_close_retry,
		   })) {
			if(gstate.players[0].visible_window == s_pending_window_close_retry)
				gstate.players[0].visible_window = -1;
			s_pending_window_close_retry = -1;
		}
	}
	if(s_pending_unload_world_retry)
		clin_send_unload_world();

	/* F3: author ride-intent RPC when dirty. send_dirty is set by
	 * onRightClick or JUMP escape; cleared when svin_rpc_send accepts
	 * the request into its queue (delivered, queued, or coalesced).
	 * The client then waits for the correlated reply, not a duplicate. */
	if(gstate.players[0].ride.send_dirty && s_last_input_epoch != 0) {
		gstate.players[0].ride.input_epoch = s_last_input_epoch;
		struct server_rpc intent = {
			.type = SRPC_MINECART_RIDE_INTENT,
			.payload.minecart_ride_intent = {
				.entity_id = gstate.players[0].ride.target_id,
				.input_epoch = gstate.players[0].ride.input_epoch,
				.request_seq = gstate.players[0].ride.pending_request_seq,
				.desired_occupied = gstate.players[0].ride.desired_occupied,
			},
		};
		if(svin_rpc_send(&intent))
			gstate.players[0].ride.send_dirty = false;
	}
	if(s_stream_recover_retry_epoch != 0) {
		uint32_t epoch = s_stream_recover_retry_epoch;
		clin_submit_stream_recovery(epoch);
		clin_hold_input_handoff();
	}
	if(s_input_nack_retry_epoch != 0) {
		uint32_t epoch = s_input_nack_retry_epoch;
		struct server_rpc retry = {
			.type = SRPC_INPUT_READY,
			.payload.input_ready.epoch = epoch,
			.payload.input_ready.retry = true,
		};
		if(svin_rpc_send(&retry))
			s_input_nack_retry_epoch = 0;
		clin_hold_input_handoff();
	}
	if(s_input_ack_retry_epoch != 0) {
		uint32_t epoch = s_input_ack_retry_epoch;
		struct server_rpc ack = {
			.type = SRPC_INPUT_READY,
			.payload.input_ready.epoch = epoch,
		};
		if(svin_rpc_send(&ack)) {
			s_input_ack_retry_epoch = 0;
			if(svin_input_ready_pending(epoch)) {
				s_input_ack_wait_epoch = epoch;
				clin_hold_input_handoff();
			} else {
				clin_expose_input_handoff();
			}
		}
	}
	if(s_input_ack_wait_epoch != 0
	   && !svin_input_ready_pending(s_input_ack_wait_epoch)) {
		s_input_ack_wait_epoch = 0;
		clin_expose_input_handoff();
	}

	clin_dig_burst_budget = CLIN_DIG_BURSTS_PER_DRAIN; // see CRPC_SET_BLOCK

	ptime_t drain_start = s_clin_profile ? time_get() : (ptime_t){0};
	/* Split-dims B1: complete every drained quiesce (and reclaim drained
	 * predecessors) before routing this frame's traffic. */
	(void)client_ctx_registry_poll(&s_world_ctx);
	clin_world_resend_poll(); /* B2 gate 2: no-op flag OFF */
	(void)clin_landing_poll(); /* A2-T landing ready: no-op when idle */
	clin_world_payload_sync_particle_cameras();
	void* call;
	uint32_t g9_work = 0, g9_backlog, g9_oldest;
	g9_probe_inbox_stats(&g9_backlog, &g9_oldest);
	g9_probe_main_mark(2, true, 0, g9_backlog, g9_oldest);
	while(clin_drain_next(&call)) {
		g9_work++;
		ptime_t rpc_start = s_clin_profile ? time_get() : (ptime_t){0};
		int part = 2;
		if(s_clin_profile) {
			struct client_rpc* rpc = call;
			part = rpc->type == CRPC_CHUNK ? 0 : rpc->type == CRPC_FAR_CHUNK ? 1 : 2;
		}
		clin_process_queued(call);
		s_world_routed_call = NULL; /* tagged routes never reach clin_process */
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
		if(s_test_consume_observer) s_test_consume_observer(call, s_test_consume_user);
#endif
		if(s_clin_profile) s_clin_work_ms[part] += time_diff_s(rpc_start, time_get()) * 1000.0f;
		// Token held here guarantees an empty slot in the return channel.
		clin_inbox_token_return(call);
	}
	g9_probe_inbox_stats(&g9_backlog, &g9_oldest);
	g9_probe_main_mark(2, false, g9_work, g9_backlog, g9_oldest);
	clin_transfer_hold_release_restored();
	clin_host_transfer_pane_update();
	clin_portal_loading_update();

	if(s_clin_profile) {
		float drain = time_diff_s(drain_start, time_get()) * 1000.0f;
		s_clin_work_ms[3] = fmaxf(0.0f, drain - s_clin_work_ms[0] - s_clin_work_ms[1] - s_clin_work_ms[2]);
	}

	/* Liveness for the one in-flight selected-recipe request per local player:
	 * retire it if the window it named went away or the reply never came
	 * (hostile review, finding 3). Deliberately AFTER the inbox drain above: a
	 * real reply already sitting in the FIFO must always win, so closing the
	 * window in the same frame the answer arrives cannot mislabel a completed
	 * craft as timed out. */
	{
		ptime_t craft_now = time_get();
		client_session_recipe_craft_tick(&gstate.players[0], craft_now);
		client_session_recipe_craft_tick(&gstate.players[1], craft_now);
	}

	/* Drain BEGIN/RESET/FAIL first: none may leave an old prepared epoch eligible
	 * for acknowledgement. The view is prepared independently of this hold. */
	clin_update_input_readiness();

	if(gstate.players[0].deferred_reopen_window >= 0) {
		/* A world lifecycle transition invalidates the old container ownership.
		 * A loaded world waiting only for presentation retains its deferred GUI. */
		if(!s_world_reset_ready
		   || (gstate.world_switch_pending
			   && s_stream_recovery_phase == CLIN_STREAM_RECOVERY_NONE)
		   || gstate.players[0].current_screen == &screen_select_world
		   || (gstate.players[0].current_screen == &screen_load_world
			   && !gstate.world_loaded
			   && s_stream_recovery_phase == CLIN_STREAM_RECOVERY_NONE)) {
			gstate.players[0].deferred_reopen_window = -1;
		} else if(!gstate.paused && gstate.players[0].current_screen == &screen_ingame) {
			uint8_t window = (uint8_t)gstate.players[0].deferred_reopen_window;
			gstate.players[0].deferred_reopen_window = -1;
			clin_show_window(window);
		}
	}

	/* Pre-world MP echo (take-51 fix; the session's authoritative-position latch):
	 * once the remote install has placed the camera, keep the vanilla 50ms
	 * position cadence flowing even while the world is still streaming in --
	 * it is what drives mc173's chunk streaming. Remote sessions only. */
	/* REL-MP-FIX4b: the same-dim respawn hold parks main.c's per-tick producer,
	 * and vanilla streams chunks only in reply to Packet10-13
	 * (NetServerHandler.handleFlying -> onUpdateEntity(true)), so the hold
	 * must echo too. The sample is the server's installed position and
	 * onGround (clin_apply_player_position): no pull-back, no displacement. */
	bool mp_hold_echo = gstate.world_loaded && gstate.remote_terrain_hold;
	bool mp_preworld_echo
		= ((!gstate.world_loaded && gstate.world_switch_pending) || mp_hold_echo)
		&& gstate.players[0].player_sync.authoritative_pos_seen && gstate.remote_session;
	if((gstate.world_loaded && !gstate.world_switch_pending
	    && client_session_host_gameplay_active(&gstate.players[0]))
	   || mp_preworld_echo) {
		bool on_ground = gstate.players[0].local_player ? gstate.players[0].local_player->on_ground
											 : false;
		bool sneaking = gstate.players[0].local_player
			&& gstate.players[0].local_player->data.local_player.capture_input
			&& input_held(IB_SNEAK);
		if(!gstate.players[0].player_sync.state_known
		   || on_ground != gstate.players[0].player_sync.state_on_ground
		   || sneaking != gstate.players[0].player_sync.state_sneaking
		   || gstate.players[0].player_sync.correction_seq != gstate.players[0].player_sync.state_correction_seq
		   || gstate.players[0].player_sync.leave_bed_seq != gstate.players[0].player_sync.state_leave_bed_seq) {
			struct server_rpc state = {
				.type = SRPC_PLAYER_STATE,
				.payload.player_state = {
					.on_ground = on_ground,
					.sneaking = sneaking,
					.correction_seq = gstate.players[0].player_sync.correction_seq,
					/* MP-BED: the Leave Bed press counter (remote only). */
					.leave_bed_seq = gstate.players[0].player_sync.leave_bed_seq,
				},
			};
			if(svin_rpc_send(&state)) {
				gstate.players[0].player_sync.state_known = true;
				gstate.players[0].player_sync.state_on_ground = on_ground;
				gstate.players[0].player_sync.state_sneaking = sneaking;
				gstate.players[0].player_sync.state_correction_seq = gstate.players[0].player_sync.correction_seq;
				gstate.players[0].player_sync.state_leave_bed_seq = gstate.players[0].player_sync.leave_bed_seq;
			}
		}
		/* V6 F1 round 3: LOCAL movement is produced per executed tick
		 * (main.c, clin_local_tick_begin/end); only the remote pre-world
		 * echo keeps the 50 ms wall-clock cadence here. */
		if(mp_preworld_echo
		   && time_diff_ms(gstate.players[0].player_sync.last_pos_update, time_get()) >= 50)
			(void)clin_local_position_produce(0);
	}

	/* Experimental local split-screen: the guest body streams its OWN position
	 * from the GUEST source at the same 50 ms cadence and in the same wire
	 * convention as the primary above. server_local_process_owned routes it to
	 * the guest actor (server_local_guest_position); it can never reach
	 * s->player. Gated on local_gameplay_ready, which only a server
	 * GAMEPLAY_READY reply can set -- with the experimental switch off this
	 * block is unreachable and the primary's traffic is byte-identical. */
	/* Player 0's accepted A2-T presentation is owner-local: its global
	 * loading flags must not silence player 1's admitted, viewed body. The
	 * shared episode latch also drives main.c's two-pane continuation. */
	bool guest_during_host_transfer=s_host_transfer_pane
		&&s_world_payloads_enabled&&!gstate.remote_session&&!gstate.paused
		&&!gstate.world_load_failed&&gstate.world_switch_pending
		&&gstate.players[0].current_screen==&screen_load_world
		&&clin_world_view_slot(1)>=0;
	if(((gstate.world_loaded && !gstate.world_switch_pending)
		||guest_during_host_transfer)
	   && client_session_local_gameplay_active(&gstate.players[1])
	   && !clin_transfer_actor_held(1)
	   && time_diff_ms(gstate.players[1].player_sync.last_pos_update,
					   time_get())
		  >= 50) {
		/* V6 F1 round 3: the guest's movement is produced per executed tick
		 * in main.c (clin_local_tick_begin/end); no wall-clock sample. */
	}
}

static bool s_server_publication_stopped;
static uint32_t s_server_publication_dropped;
static bool s_rpc_publication_fault;
bool clin_rpc_publication_faulted(void) {
	return __atomic_load_n(&s_rpc_publication_fault, __ATOMIC_ACQUIRE);
}
void clin_server_publication_stop(bool stop) {
	if(!stop)
		__atomic_store_n(&s_rpc_publication_fault, false, __ATOMIC_RELEASE);
	__atomic_store_n(&s_server_publication_stopped, stop, __ATOMIC_RELEASE);
}
bool clin_server_publication_stopped(void) {
	return __atomic_load_n(&s_server_publication_stopped, __ATOMIC_ACQUIRE);
}
uint32_t clin_server_publication_dropped(void) {
	return __atomic_load_n(&s_server_publication_dropped, __ATOMIC_RELAXED);
}
/* G9 R3: true = the call is dropped (its pooled payload released). */
static bool clin_server_publication_gate(struct client_rpc* call) {
	if(!clin_server_publication_stopped() || call->type == CRPC_LOAD_FAILED)
		return false;
	clin_rpc_release_pooled(call);
	__atomic_add_fetch(&s_server_publication_dropped, 1, __ATOMIC_RELAXED);
	return true;
}

/* ASTRA-G9-R5 point 10: the server thread's token-pool wait inside
 * clin_rpc_send, per heartbeat window (take-and-clear by the reader). With
 * the pending output below (order 2) a wait happens ONLY at exhaustion. */
static uint32_t s_rpc_wait_us, s_rpc_wait_max_us, s_rpc_waits;
void clin_rpc_wait_take(uint32_t* sum_us, uint32_t* max_us, uint32_t* n) {
	*sum_us = __atomic_exchange_n(&s_rpc_wait_us, 0u, __ATOMIC_RELAXED);
	*max_us = __atomic_exchange_n(&s_rpc_wait_max_us, 0u, __ATOMIC_RELAXED);
	*n = __atomic_exchange_n(&s_rpc_waits, 0u, __ATOMIC_RELAXED);
}

/* ---- ASTRA-G9-R5 points 5-6 / order 2: bounded, owned, ordered PENDING
 * OUTPUT with a nonblocking transport ------------------------------------
 *
 * The 16-token inbox is the client's processing window, not a queue the
 * simulation may wait on: g9_1 found the server parked in this send for
 * 0.15-0.6 s per fixture edit tick (SPIKE-DIAG H2), inside World.tick with
 * mutations, callbacks and RNG draws behind it. Now a send takes a token
 * ONLY if one is free (nonblocking receive); otherwise the envelope is
 * appended to this server-owned ring and delivered later, in order, by
 * clin_rpc_flush_pending (every send tries to flush first, so a later send
 * can never overtake a pending one; the tick loop flushes at tick start and
 * before every idle sleep; the chunk-slot wait flushes before blocking).
 *   - OWNED: the ring holds the envelope by value exactly as the inbox slot
 *     would; a pooled payload (chunk/far/map slot, destroy-batch array) is
 *     owned by the envelope until the client consumes it or the ring is
 *     discarded (clin_rpc_pending_discard releases it exactly once, the
 *     same release as a discarded inbox entry). Nothing is copied twice.
 *   - ORDERED: FIFO; the generation tag travels with the entry and is
 *     stamped on the inbox slot at delivery, as before.
 *   - BOUNDED: RPC_PENDING_SIZE entries (792 B each: ~100 KiB at the
 *     accepted 128, allocated once on first use -- MEM2 on the Wii --
 *     never in MEM1 .bss). G9 R21: the 512 capacity candidate (bd4d246)
 *     saturated at 512 too on real hardware (h_ring1) and is withdrawn.
 *   - PACKED (G9 R21, ASTRA-G9-R21-RING alternative): a CRPC_SET_BLOCK that
 *     must be deferred is appended INTO the ring's tail entry when that
 *     entry is an open, compatible block update (same world_index and
 *     world_generation, untagged) with room: the tail becomes one
 *     CRPC_SET_BLOCK_PACK of up to CRPC_SET_BLOCK_PACK_MAX records in send
 *     order. G9 startup: an untagged CRPC_SPAWN_ITEM is a packable record
 *     too (spawn_mask marks it). Real-Wii k_pack200 showed every capped
 *     start-up tick on the perf_stress 3-blast burst: Explosion pass 3
 *     emits drop roll -> SPAWN_ITEM, then SET_BLOCK, per broken cell, so a
 *     SET_BLOCK-only pack closed at every drop (host: one power-4 blast =
 *     174 SET_BLOCK + 28 SPAWN_ITEM in 63-75 ring entries). BUG-TICK
 *     hw1030a (real Wii 2026-10-30, same signature hw1018b): the server
 *     parked in this send for 78 ms (3 token waits, ring 128) on a
 *     perf_stress burst tick while MAIN sat in 20-40 ms frames (client
 *     light + mesh) and drained nothing. The ring was filled by the
 *     perf_stress item reaper's CRPC_ENTITY_DESTROY singles (host fixture:
 *     one 3-blast burst = 391 SET_BLOCK + 118 SPAWN_ITEM packed in ~28
 *     entries PLUS 70 ENTITY_DESTROY + 3 ENTITY_EXPLODE + 3 SOUND, each a
 *     whole 792-B entry: 101 entries; real terrain reached 128). So the
 *     by-value, pointer-free ENTITY_DESTROY / ENTITY_EXPLODE / SOUND are
 *     `other` pack records too (type + payload bytes, other_mask): the
 *     same burst is 33 entries (tests/test_rpc_pool_burst.c), the
 *     consumer-effect trace and the simulation bytes identical.
 *     Only the TAIL can take records, so packed records are always
 *     consecutive in the FIFO: any other message closes the pack (it
 *     becomes the new tail), as do a world_index/world_generation change
 *     (incompatible), the size bound, an emptied ring, a discard and
 *     clin_rpc_pack_seal (every tick start: a pack never spans ticks).
 *     Nothing is dropped, merged or reordered: delivery runs the publish /
 *     settle observers once per record, and the consumer (clin_inbox_take)
 *     replays every record, in order, as the CRPC_SET_BLOCK envelope it was,
 *     through the same route / clin_process_queued / observer path, holding
 *     the one inbox token until the last record. LOCAL ONLY: the remote
 *     path never reaches this ring (clin_try_rpc_send / clin_process_direct)
 *     and no producer emits a pack. A direct (token-available) send is never
 *     packed. One token now carries up to 19 block updates, so the 16-token
 *     window serves up to 304 per client drain instead of 16. At
 *     exhaustion nothing is dropped or coalesced: the server waits for ONE
 *     token (the old blocking receive: timed, WDS_RPC_SEND, phase restored,
 *     counted in clin_rpc_pending_stats), delivers the pending head into it
 *     and appends the new envelope -- order kept, work retained, the delay
 *     measured. A failed allocation is retried on later sends. Exhaustion has a
 *     dead-consumer deadline (RPC_EXHAUSTION_TIMEOUT_MS; was one second until
 *     ci-1101/61, which closed live worlds on recoverable MAIN stalls): timeout
 *     retains the triggering envelope and aborts the session through its
 *     mutation/save barrier. Shutdown interrupts the wait. Ordinary output is released only
 *     by explicit stopped-session cleanup, with drops counted.
 *   - Server-thread only (like the old send). Publication stop drops the
 *     ring's entries the way it drops a new send (released, counted).
 * The settle/column observers see ENQUEUE / publish / SEND_END at DELIVERY
 * (entry into the inbox), SEND_BEGIN at the send, as their names say. */
#define RPC_PENDING_SIZE CLIN_RPC_PENDING_CAPACITY
struct clin_rpc_pending_entry {
	struct client_rpc call;
	uint64_t generation;
};
static struct clin_rpc_pending_entry* s_rpc_pending; /* ring, lazy */
/* Admission limit (== RPC_PENDING_SIZE outside tests; storage and modulus
 * stay RPC_PENDING_SIZE, so a lower limit is exactly a smaller FIFO). */
static unsigned s_rpc_pending_cap = RPC_PENDING_SIZE;
static unsigned s_rpc_pending_head, s_rpc_pending_n;
/* G9 R21 packing (server thread): the tail entry may take more records. */
static bool s_rpc_pack_enabled = true;
/* G9 startup: CRPC_SPAWN_ITEM records join packs too (a blast's drop rolls
 * interleave them with its SET_BLOCKs). Off = the R21 SET_BLOCK-only pack. */
static bool s_rpc_pack_spawn_items = true;
static bool s_rpc_pack_tail_open;
static uint32_t s_rpc_packs, s_rpc_packed_records;
/* The pack must fit the union that already existed: the envelope (the inbox
 * slots and every ring entry) does not grow. 792 B host and Wii alike. */
_Static_assert(sizeof(struct client_rpc) == 792,
	"CRPC_SET_BLOCK_PACK must not grow struct client_rpc");
_Static_assert(sizeof(union crpc_pack_record) == sizeof(struct crpc_set_block),
	"a CRPC_SPAWN_ITEM / other pack record must fit a CRPC_SET_BLOCK record");
_Static_assert(CRPC_SET_BLOCK_PACK_MAX <= 32, "spawn_mask / other_mask are 32 bits");
/* BUG-TICK hw1030a: the `other` record class. Every member is by value and
 * pointer-free (no pooled storage, nothing to release) and fits the bytes. */
_Static_assert(sizeof(((struct client_rpc*)0)->payload.entity_destroy) <= CRPC_PACK_OTHER_BYTES,
	"entity_destroy fits an other record");
_Static_assert(sizeof(((struct client_rpc*)0)->payload.entity_explode) <= CRPC_PACK_OTHER_BYTES,
	"entity_explode fits an other record");
_Static_assert(sizeof(((struct client_rpc*)0)->payload.sound) <= CRPC_PACK_OTHER_BYTES,
	"sound fits an other record");
_Static_assert(sizeof(((struct client_rpc*)0)->payload.lightning_bolt) <= CRPC_PACK_OTHER_BYTES,
	"lightning_bolt fits an other record");
/* The payload member an `other` record carries, by type; 0 = not packable. */
static size_t clin_rpc_pack_other_size(enum client_rpc_type type) {
	switch(type) {
	case CRPC_ENTITY_DESTROY: return sizeof(((struct client_rpc*)0)->payload.entity_destroy);
	case CRPC_ENTITY_EXPLODE: return sizeof(((struct client_rpc*)0)->payload.entity_explode);
	case CRPC_SOUND: return sizeof(((struct client_rpc*)0)->payload.sound);
	case CRPC_LIGHTNING_BOLT: return sizeof(((struct client_rpc*)0)->payload.lightning_bolt);
	default: return 0;
	}
}
/* One final envelope retained if exhaustion times out or shutdown cancels.
 * No subsequent ordinary publication is admitted after that stop. */
static struct clin_rpc_pending_entry s_rpc_overflow;
static bool s_rpc_overflow_owned;
/* ci-1101/61: the exhaustion deadline is the DEAD-consumer backstop, never a
 * slow-consumer limit. Two PC runs (2026-11-02, loaded host) lost a live world
 * to the former 1000 ms value when MAIN stalled 2.7 s / 5.2 s and recovered:
 * the server waited 1.08 s, faulted, and "World publication stopped (client
 * not consuming)" closed it. A recoverable MAIN stall is ordinary back-pressure
 * (the server thread simply stays parked here, its tick incomplete, exactly as
 * the pre-deadline blocking receive did). It must exceed every legitimate
 * synchronous MAIN operation the Wii watchdog itself tolerates (3 s + the
 * declared 60 s TEXPACKS_RELOAD_GRACE_MS), so a wedged MAIN is named by the
 * watchdog's hang report first. Shutdown still interrupts at once (cancel). */
#define RPC_EXHAUSTION_TIMEOUT_MS 120000u
#define RPC_EXHAUSTION_NOTICE_MS 1000u /* log a long (recovering) wait */
static uint32_t s_rpc_exhaustion_timeout_ms = RPC_EXHAUSTION_TIMEOUT_MS;
static bool s_rpc_pending_alloc_logged;
static uint32_t s_rpc_pending_peak, s_rpc_deferred, s_rpc_exhausted,
	s_rpc_cancelled;
/* Cancellation and the deadline terminate an exhausted callback without
 * abandoning its owned envelope. The session then fails closed. */
static bool s_rpc_cancel;
void clin_rpc_cancel(bool cancel) {
	if(cancel) thread_g9_lease_cancel();
	__atomic_store_n(&s_rpc_cancel, cancel, __ATOMIC_RELEASE);
}
static bool clin_rpc_cancelled(void) {
	return __atomic_load_n(&s_rpc_cancel, __ATOMIC_ACQUIRE);
}
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static bool s_test_rpc_blocking; /* the OLD transport, for the parity proof */
static bool s_test_pending_alloc_fail;
void clin_test_set_rpc_blocking(bool blocking) {
	s_test_rpc_blocking = blocking;
}
void clin_test_fail_pending_alloc(bool fail) {
	/* A reused test process must reach the actual allocation path too. */
	if(fail && s_rpc_pending && s_rpc_pending_n == 0) {
#ifdef PLATFORM_WII
		mem2_free(s_rpc_pending);
#else
		free(s_rpc_pending);
#endif
		s_rpc_pending = NULL;
	}
	s_test_pending_alloc_fail = fail;
}
void clin_test_set_pending_capacity(unsigned cap) {
	assert(s_rpc_pending_n == 0 && cap <= RPC_PENDING_SIZE);
	s_rpc_pending_cap = cap ? cap : RPC_PENDING_SIZE;
}
void clin_test_set_rpc_exhaustion_timeout_ms(uint32_t ms) {
	s_rpc_exhaustion_timeout_ms = ms ? ms : RPC_EXHAUSTION_TIMEOUT_MS;
}
void clin_test_set_block_pack(bool enabled) {
	assert(s_rpc_pending_n == 0);
	s_rpc_pack_enabled = enabled;
	s_rpc_pack_tail_open = false;
}
void clin_test_set_pack_spawn_items(bool enabled) {
	assert(s_rpc_pending_n == 0);
	s_rpc_pack_spawn_items = enabled;
	s_rpc_pack_tail_open = false;
}
#endif

void clin_rpc_pack_stats(uint32_t* packs, uint32_t* packed_records) {
	if(packs) *packs = s_rpc_packs;
	if(packed_records) *packed_records = s_rpc_packed_records;
}
void clin_rpc_pack_seal(void) {
	s_rpc_pack_tail_open = false;
}

/* One packed record as the CRPC_SET_BLOCK / CRPC_SPAWN_ITEM envelope it was
 * sent as (its active fields; unused union bytes zero). */
static void clin_rpc_unpack_record(const struct client_rpc* pack, unsigned i,
								   struct client_rpc* out) {
	memset(out, 0, sizeof *out);
	out->world_index = pack->world_index;
	out->world_generation = pack->world_generation;
	if(pack->payload.set_block_pack.other_mask >> i & 1u) {
		const struct crpc_pack_other* o = &pack->payload.set_block_pack.rec[i].other;
		out->type = (enum client_rpc_type)o->type;
		const size_t n = clin_rpc_pack_other_size(out->type);
		assert(n && n <= CRPC_PACK_OTHER_BYTES);
		memcpy(&out->payload, o->bytes, n); /* the member is the union head */
	} else if(pack->payload.set_block_pack.spawn_mask >> i & 1u) {
		out->type = CRPC_SPAWN_ITEM;
		out->payload.spawn_item = pack->payload.set_block_pack.rec[i].spawn_item;
	} else {
		out->type = CRPC_SET_BLOCK;
		out->payload.set_block = pack->payload.set_block_pack.rec[i].set_block;
	}
}

static void clin_rpc_pending_reset(void) {
	s_rpc_pending_head = s_rpc_pending_n = 0;
	s_rpc_pending_peak = s_rpc_deferred = s_rpc_exhausted = s_rpc_cancelled = 0;
	s_rpc_pack_tail_open = false;
	s_rpc_packs = s_rpc_packed_records = 0;
	s_rpc_pending_alloc_logged = false;
	s_rpc_overflow_owned = false;
	__atomic_store_n(&s_rpc_publication_fault, false, __ATOMIC_RELEASE);
	__atomic_store_n(&s_rpc_cancel, false, __ATOMIC_RELEASE);
}

/* The ring is allocated on first use and RE-TRIED on every later use while
 * it is missing (SOL-REVIEW-d42f minor: no permanent latch -- recovered
 * MEM2 restores nonblocking publication); the failure is logged once. */
static bool clin_rpc_pending_ring(void) {
	if(s_rpc_pending)
		return true;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_test_pending_alloc_fail)
		return false;
#endif
#ifdef PLATFORM_WII
	s_rpc_pending = mem2_alloc(sizeof *s_rpc_pending * RPC_PENDING_SIZE);
#else
	s_rpc_pending = malloc(sizeof *s_rpc_pending * RPC_PENDING_SIZE);
#endif
	if(!s_rpc_pending) {
		if(!s_rpc_pending_alloc_logged)
			fprintf(stderr, "[client_interface] rpc pending ring: allocation "
							"failed; sends wait at the token pool until it "
							"succeeds\n");
		s_rpc_pending_alloc_logged = true;
		return false;
	}
	return true;
}

/* Deliver one envelope into an inbox token the caller holds. */
static void clin_rpc_deliver(const struct client_rpc* call,
							 uint64_t generation, struct client_rpc* empty) {
	if(call->type == CRPC_SET_BLOCK_PACK) {
		/* Every record is published, in order, as its own CRPC_SET_BLOCK;
		 * the records travel together in this one token. */
		const unsigned n = call->payload.set_block_pack.n;
		struct client_rpc rec;
		for(unsigned i = 0; i < n; i++) {
			clin_rpc_unpack_record(call, i, &rec);
			if(clin_column_observe_publish)
				clin_column_observe_publish(&rec);
			if(client_settle_observe)
				clin_settle_rpc(SET_ENQUEUE, &rec, empty);
		}
		*empty = *call;
		rpc_guest_generation[empty - rpc_msg] = generation;
		g9_probe_inbox((unsigned)(empty - rpc_msg), true);
		tchannel_send(&clin_inbox, empty, true);
		if(client_settle_observe)
			for(unsigned i = 0; i < n; i++) {
				clin_rpc_unpack_record(call, i, &rec);
				clin_settle_rpc(SET_SEND_END, &rec, NULL);
			}
		return;
	}
	if(clin_column_observe_publish)
		clin_column_observe_publish((struct client_rpc*)call);
	*empty = *call;
	rpc_guest_generation[empty - rpc_msg] = generation;
	// Never full: we hold one of the RPC_INBOX_SIZE tokens, so the inbox has
	// at most RPC_INBOX_SIZE-1 (inbox + empty_msg + in-hand == capacity).
	// "clin overflow" therefore cannot drop a CRPC -- see the 2026-06-11 note
	// on render_demo_defer in server_local.h for the misattributed sighting.
	if(client_settle_observe)
		clin_settle_rpc(SET_ENQUEUE, (struct client_rpc*)call, empty);
	g9_probe_inbox((unsigned)(empty - rpc_msg), true);
	tchannel_send(&clin_inbox, empty, true);
	if(client_settle_observe)
		clin_settle_rpc(SET_SEND_END, (struct client_rpc*)call, NULL);
}

/* The exhaustion wait is timed, phase-nested and bounded (NULL on
 * shutdown or the one-second deadline). The
 * wait polls the pool once a millisecond so the cancel is seen; a token
 * that appears is taken at once. */
static struct client_rpc* clin_rpc_token_wait(void) {
	struct client_rpc* empty = NULL;
	const uint32_t prev_phase = watchdog_phase_srv_current();
	watchdog_phase_srv(WDS_RPC_SEND);
	const ptime_t wait_t0 = time_get();
	g9_probe_rpc(1, NULL);
	for(;;) {
		if(tchannel_receive(&clin_empty_msg, (void**)&empty, false))
			break;
		if(clin_rpc_cancelled()) {
			empty = NULL;
			g9_probe_rpc(4, NULL);
			break;
		}
		if((int64_t)time_diff_ms(wait_t0, time_get())
		   >= (int64_t)s_rpc_exhaustion_timeout_ms) {
			fprintf(stderr, "[client_interface] rpc publication: client drained "
							"nothing for %u ms (ring %u + inbox full); "
							"consumer presumed dead, session stops\n",
					(unsigned)s_rpc_exhaustion_timeout_ms,
					(unsigned)s_rpc_pending_n);
			empty = NULL;
			g9_probe_rpc(4, NULL);
			break;
		}
		thread_msleep(1);
	}
	{
		const uint32_t us
			= (uint32_t)(time_diff_s(wait_t0, time_get()) * 1.0e6f);
		if(empty && us >= RPC_EXHAUSTION_NOTICE_MS * 1000u)
			fprintf(stderr, "[client_interface] rpc publication waited %u ms "
							"for the client to drain (back-pressure; world "
							"kept)\n", (unsigned)(us / 1000u));
		__atomic_fetch_add(&s_rpc_wait_us, us, __ATOMIC_RELAXED);
		__atomic_fetch_add(&s_rpc_waits, 1u, __ATOMIC_RELAXED);
		g9_worst_tick_wait_us(G9WT_W_SEND, us); /* nested in RPC_SEND */
		SERVICE_OBSERVER(service_observer_wait(G9WT_W_SEND, service_observer_at(wait_t0), service_observer_now()));
		uint32_t cur = __atomic_load_n(&s_rpc_wait_max_us, __ATOMIC_RELAXED);
		while(us > cur
			  && !__atomic_compare_exchange_n(&s_rpc_wait_max_us, &cur, us,
											  true, __ATOMIC_RELAXED,
											  __ATOMIC_RELAXED))
			;
	}
	watchdog_phase_srv((enum wd_phase_srv)prev_phase);
	return empty;
}

/* SOL-REVIEW-d42f Major 2: the terminal CRPC_LOAD_FAILED is exempt from the
 * publication stop at the send gate, so it is exempt from the stop-discard
 * too: discard drops every OTHER pending entry and keeps the terminal
 * one(s) at the head, in order, for delivery. */
static bool clin_rpc_terminal(const struct client_rpc* call) {
	return call->type == CRPC_LOAD_FAILED;
}

static void clin_rpc_pending_pop_into(struct client_rpc* empty) {
	struct clin_rpc_pending_entry* e = &s_rpc_pending[s_rpc_pending_head];
	clin_rpc_deliver(&e->call, e->generation, empty);
	s_rpc_pending_head = (s_rpc_pending_head + 1) % RPC_PENDING_SIZE;
	s_rpc_pending_n--;
	if(s_rpc_pending_n == 0)
		s_rpc_pack_tail_open = false; /* the open tail was just delivered */
}

bool clin_rpc_flush_pending(void) {
	if(s_rpc_pending_n == 0 && !s_rpc_overflow_owned)
		return true;
	if(clin_server_publication_stopped())
		clin_rpc_pending_discard(); /* keeps the terminal entries */
	while(s_rpc_pending_n) {
		struct client_rpc* empty;
		if(!tchannel_receive(&clin_empty_msg, (void**)&empty, false))
			return false;
		clin_rpc_pending_pop_into(empty);
	}
	if(s_rpc_overflow_owned) {
		struct client_rpc* empty;
		if(!tchannel_receive(&clin_empty_msg, (void**)&empty, false))
			return false;
		clin_rpc_deliver(&s_rpc_overflow.call, s_rpc_overflow.generation, empty);
		s_rpc_overflow_owned = false;
	}
	return true;
}

void clin_rpc_pending_discard(void) {
	const bool stopped = clin_server_publication_stopped();
	if(s_rpc_overflow_owned && !(stopped && clin_rpc_terminal(&s_rpc_overflow.call))) {
		clin_rpc_release_pooled(&s_rpc_overflow.call);
		s_rpc_overflow_owned = false;
		if(stopped)
			__atomic_add_fetch(&s_server_publication_dropped, 1, __ATOMIC_RELAXED);
	}
	/* compact in place: drop the non-terminal entries, keep the terminal
	 * ones in their order (a publication stop keeps CRPC_LOAD_FAILED) */
	unsigned kept = 0;
	const unsigned n = s_rpc_pending_n;
	for(unsigned i = 0; i < n; i++) {
		struct clin_rpc_pending_entry* e
			= &s_rpc_pending[(s_rpc_pending_head + i) % RPC_PENDING_SIZE];
		if(stopped && clin_rpc_terminal(&e->call)) {
			struct clin_rpc_pending_entry* dst
				= &s_rpc_pending[(s_rpc_pending_head + kept) % RPC_PENDING_SIZE];
			if(dst != e)
				*dst = *e;
			kept++;
			continue;
		}
		const bool packed = e->call.type == CRPC_SET_BLOCK_PACK;
		const unsigned records = packed ? e->call.payload.set_block_pack.n : 1u;
		if(client_settle_observe)
			for(unsigned r = 0; r < records; r++) {
				struct client_rpc rec;
				if(packed) clin_rpc_unpack_record(&e->call, r, &rec);
				const struct client_rpc* c = packed ? &rec : &e->call;
				clin_settle_rpc(SET_DEQUEUE, c, c);
				clin_settle_rpc(SET_INSTALL_FAIL, c, c);
			}
		clin_rpc_release_pooled(&e->call); /* a pack owns no pooled payload */
		if(stopped) /* explicit cleanup accounting counts RECORDS */
			__atomic_add_fetch(&s_server_publication_dropped, records,
							   __ATOMIC_RELAXED);
	}
	s_rpc_pack_tail_open = false;
	s_rpc_pending_n = kept;
	if(kept == 0)
		s_rpc_pending_head = 0;
}

/* Exit transfers remaining ownership to shutdown's post-join drain. A
 * deferred terminal must survive an arbitrarily late consumer. */
void clin_rpc_pending_exit(void) {
	(void)clin_rpc_flush_pending();
}

void clin_rpc_pending_stats(uint32_t* pending, uint32_t* peak,
							uint32_t* deferred, uint32_t* exhausted) {
	if(pending)
		*pending = s_rpc_pending_n + (s_rpc_overflow_owned ? 1u : 0u);
	if(peak)
		*peak = s_rpc_pending_peak;
	if(deferred)
		*deferred = s_rpc_deferred;
	if(exhausted)
		*exhausted = s_rpc_exhausted;
}
uint32_t clin_rpc_cancelled_sends(void) {
	return s_rpc_cancelled;
}

/* G9 R21: may this envelope start / join a pack? Untagged block updates
 * and (G9 startup) untagged item spawns only -- both by-value payloads with
 * no pooled storage; guest-tagged replies and every other type never pack. */
static bool clin_rpc_pack_candidate(const struct client_rpc* call,
									uint64_t generation) {
	return s_rpc_pack_enabled && generation == 0
		&& (call->type == CRPC_SET_BLOCK
			|| (s_rpc_pack_spawn_items
				&& (call->type == CRPC_SPAWN_ITEM
					|| clin_rpc_pack_other_size(call->type) != 0)));
}
/* Store `call` (a pack candidate) as record i of a pack. */
static void clin_rpc_pack_store(struct client_rpc* pack, unsigned i,
								const struct client_rpc* call) {
	const size_t other = clin_rpc_pack_other_size(call->type);
	if(other) {
		struct crpc_pack_other* o = &pack->payload.set_block_pack.rec[i].other;
		memset(o, 0, sizeof *o);
		o->type = (uint32_t)call->type;
		memcpy(o->bytes, &call->payload, other); /* the member is the union head */
		pack->payload.set_block_pack.other_mask |= 1u << i;
	} else if(call->type == CRPC_SPAWN_ITEM) {
		pack->payload.set_block_pack.rec[i].spawn_item = call->payload.spawn_item;
		pack->payload.set_block_pack.spawn_mask |= 1u << i;
	} else
		pack->payload.set_block_pack.rec[i].set_block = call->payload.set_block;
}

/* Append `call` INTO the ring tail entry if that entry is the open,
 * compatible block update with room. True = the record is now owned by the
 * pending output (counted deferred). Server thread. */
static bool clin_rpc_pack_append(const struct client_rpc* call,
								 uint64_t generation) {
	if(!s_rpc_pack_tail_open || s_rpc_pending_n == 0 || !s_rpc_pending
	   || !clin_rpc_pack_candidate(call, generation))
		return false;
	struct clin_rpc_pending_entry* t
		= &s_rpc_pending[(s_rpc_pending_head + s_rpc_pending_n - 1)
						 % RPC_PENDING_SIZE];
	if(t->generation != 0 || t->call.world_index != call->world_index
	   || t->call.world_generation != call->world_generation)
		return false; /* a world/generation barrier: a new entry */
	if(t->call.type != CRPC_SET_BLOCK_PACK) {
		if(!clin_rpc_pack_candidate(&t->call, t->generation)) {
			s_rpc_pack_tail_open = false;
			return false;
		}
		/* the tail becomes a pack whose first record is itself */
		const struct client_rpc first = t->call;
		memset(&t->call, 0, sizeof t->call);
		t->call.type = CRPC_SET_BLOCK_PACK;
		t->call.world_index = first.world_index;
		t->call.world_generation = first.world_generation;
		t->call.payload.set_block_pack.n = 1;
		clin_rpc_pack_store(&t->call, 0, &first);
		s_rpc_packs++;
	}
	uint32_t* n = &t->call.payload.set_block_pack.n;
	assert(*n >= 1 && *n < CRPC_SET_BLOCK_PACK_MAX);
	clin_rpc_pack_store(&t->call, (*n)++, call);
	if(*n == CRPC_SET_BLOCK_PACK_MAX)
		s_rpc_pack_tail_open = false; /* size bound */
	s_rpc_packed_records++;
	s_rpc_deferred++;
	return true;
}

static void clin_rpc_send_tagged(struct client_rpc* call, uint64_t generation) {
	if(clin_server_publication_gate(call))
		return;
	if(client_settle_observe)
		clin_settle_rpc(SET_SEND_BEGIN, call, NULL);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_test_rpc_observer)
		s_test_rpc_observer(call, s_test_rpc_observer_user);
#endif
	struct client_rpc* empty;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_test_rpc_blocking) {
		/* Pre-d42f transport: the original blocking receive and value copy;
		 * it bypasses every candidate queue/wait/cancellation implementation. */
		bool got = tchannel_receive(&clin_empty_msg, (void**)&empty, true);
		assert(got);
		(void)got;
		clin_rpc_deliver(call, generation, empty);
		return;
	}
#endif
	/* in order: anything pending goes first; then this one, directly, if a
	 * token is free right now */
	if(clin_rpc_flush_pending()
	   && tchannel_receive(&clin_empty_msg, (void**)&empty, false)) {
		clin_rpc_deliver(call, generation, empty);
		return;
	}
	/* G9 R21: a block update joins the open, compatible tail entry (no new
	 * entry, no wait, still in order: the tail is the newest pending output) */
	if(clin_rpc_pack_append(call, generation)) {
		g9_worst_tick_pending(s_rpc_pending_n);
		return;
	}
	/* defer: append to the owned ring, no wait */
	if(clin_rpc_pending_ring() && s_rpc_pending_n < s_rpc_pending_cap) {
		struct clin_rpc_pending_entry* e
			= &s_rpc_pending[(s_rpc_pending_head + s_rpc_pending_n)
							 % RPC_PENDING_SIZE];
		e->call = *call;
		e->generation = generation;
		s_rpc_pending_n++;
		s_rpc_pack_tail_open = clin_rpc_pack_candidate(call, generation);
		s_rpc_deferred++;
		if(s_rpc_pending_n > s_rpc_pending_peak)
			s_rpc_pending_peak = s_rpc_pending_n;
		g9_worst_tick_pending(s_rpc_pending_n); /* probe: ring occupancy */
		return;
	}
	/* exhaustion (ring full or unavailable): retain everything, stop safely
	 * on ONE token wait (measured), keep the order */
	s_rpc_exhausted++;
	empty = clin_rpc_token_wait();
	if(!empty) {
		/* The triggering envelope stays owned until explicit stopped-session
		 * cleanup. Stop further mutations/publication/saves through the same
		 * session containment barrier used by lighting allocation failures. */
		s_rpc_overflow = (struct clin_rpc_pending_entry){*call, generation};
		s_rpc_overflow_owned = true;
		if(clin_rpc_cancelled())
			s_rpc_cancelled++;
		else
			__atomic_store_n(&s_rpc_publication_fault, true, __ATOMIC_RELEASE);
		clin_server_publication_stop(true);
		return;
	}
	if(s_rpc_pending_n) {
		clin_rpc_pending_pop_into(empty);
		struct clin_rpc_pending_entry* e
			= &s_rpc_pending[(s_rpc_pending_head + s_rpc_pending_n)
							 % RPC_PENDING_SIZE];
		e->call = *call;
		e->generation = generation;
		s_rpc_pending_n++;
		s_rpc_pack_tail_open = clin_rpc_pack_candidate(call, generation);
		g9_worst_tick_pending(s_rpc_pending_n);
	} else
		clin_rpc_deliver(call, generation, empty); /* no ring at all */
}

void clin_rpc_send(struct client_rpc* call) {
	clin_rpc_send_tagged(call, 0);
}

bool clin_rpc_send_local(const struct client_rpc* call, unsigned recipient,
	uint64_t generation) {
	if(!call || recipient != 1 || !generation) return false;
	switch(call->type) {
	case CRPC_OPEN_WINDOW: case CRPC_REOPEN_WINDOW: case CRPC_RETURN_INGAME:
	case CRPC_INVENTORY_SLOT: case CRPC_WINDOW_ITEMS: case CRPC_WINDOW_TRANSACTION:
	case CRPC_WINDOW_PROGRESS: case CRPC_PLAYER_HOTBAR_SLOT: case CRPC_WINDOW_SORT_ADMIT:
	case CRPC_RECIPE_CRAFT:
	case CRPC_PLAYER_SET_HEALTH: case CRPC_SLEEP_STATE: case CRPC_HUD_MESSAGE:
	case CRPC_LOCAL_PLAYER_FIRE: case CRPC_LOCAL_PLAYER_VELOCITY: {
		struct client_rpc copy = *call;
		clin_rpc_send_tagged(&copy, generation);
		return true;
	}
	case CRPC_LOCAL_GUEST: {
		if(call->payload.local_guest.owner != 1
		   || call->payload.local_guest.generation != generation) return false;
		struct client_rpc copy = *call;
		clin_rpc_send_tagged(&copy, generation);
		return true;
	}
	default: return false; /* Pooled pointer payloads must never enter this route. */
	}
}

bool clin_try_rpc_send_local(const struct client_rpc* call, unsigned recipient,
	uint64_t generation) {
	if(!call || recipient != 1 || !generation)
		return false;
	switch(call->type) {
	case CRPC_LOCAL_GUEST:
		if(call->payload.local_guest.owner != 1
		   || call->payload.local_guest.generation != generation) return false;
		break;
	case CRPC_WINDOW_ITEMS:
		if(call->payload.window_items.count > CRPC_WINDOW_ITEMS_MAX_SLOTS) return false;
		break;
	case CRPC_OPEN_WINDOW:
		if(call->payload.window_open.slot_count > CRPC_WINDOW_ITEMS_MAX_SLOTS) return false;
		break;
	case CRPC_PLAYER_HOTBAR_SLOT:
		if(call->payload.player_hotbar_slot.slot >= INVENTORY_SIZE_HOTBAR) return false;
		break;
	case CRPC_INVENTORY_SLOT:
	case CRPC_PLAYER_SET_HEALTH:
		break;
	default: return false; /* Pointer-bearing/pooled payloads stay excluded. */
	}
	if(clin_server_publication_stopped()) { /* G9 R3: pointer-free, dropped */
		__atomic_add_fetch(&s_server_publication_dropped, 1, __ATOMIC_RELAXED);
		return true;
	}
	if(client_settle_observe)
		clin_settle_rpc(SET_SEND_BEGIN, call, NULL);
	struct client_rpc* empty;
	/* SOL-REVIEW-d42f Major 1: a try-send may not overtake deferred output;
	 * with entries still pending after a flush it is REFUSED (the caller
	 * retries), never delivered ahead of them */
	if(!clin_rpc_flush_pending()
	   || !tchannel_receive(&clin_empty_msg, (void**)&empty, false)) {
		if(client_settle_observe)
			clin_settle_rpc(SET_STOP_POOL, call, NULL);
		return false;
	}
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_test_rpc_observer)
		s_test_rpc_observer(call, s_test_rpc_observer_user);
#endif
	if(clin_column_observe_publish) clin_column_observe_publish(call);
	*empty = *call;
	rpc_guest_generation[empty - rpc_msg] = generation;
	if(client_settle_observe)
		clin_settle_rpc(SET_ENQUEUE, call, empty);
	g9_probe_inbox((unsigned)(empty - rpc_msg), true);
	tchannel_send(&clin_inbox, empty, true); /* held token guarantees room */
	if(client_settle_observe)
		clin_settle_rpc(SET_SEND_END, call, NULL);
	return true;
}

// Non-blocking variant of clin_rpc_send: enqueue one RPC, or return false if the token pool is
// exhausted (the render thread fell behind draining clin_inbox). The far-ring evict/send paths use
// this to DEFER to next tick rather than BLOCK. Blocking the SERVER thread here while a busy main
// thread is NOT draining clin_inbox is the server half of a two-way bounded-buffer DEADLOCK -- the
// exact mirror of svin_try_send_now (server_interface.c), which already fixed the client half. It
// caused the far-ring "freeze seconds into a fresh world" (the watchdog fired with the server in
// WDS_EVICT_FAR, blocked here). Deferral is free: far evict/send are idempotent and bounded by
// FAR_EVICT_BUDGET / FAR_SEND_BUDGET, re-tried next tick.
#ifdef PLATFORM_WII
#include <ogc/lwp.h> // LWP_GetSelf
// S5.3 hunt PROBE (temporary, round 3): brackets the two primitives inside
// clin_try_rpc_send -- take 11 pinned the death to right after
// worldgen_create() succeeds, at remote_try_crpc's CRPC_WORLD_RESET send.
// This function was designed for the SERVER thread as producer (see the
// header comment above: "blocking the SERVER thread here... is the server
// half of a two-way bounded-buffer DEADLOCK"), draining always assumed to
// be a DIFFERENT thread's job (clin_update() on main). The MP login path is
// the first caller ever to run this FROM the main/client thread itself --
// producer and consumer collapsed onto one thread for the first time. One-
// shot markers tagged with LWP_GetSelf() around each primitive show exactly
// which one (if either) never returns.
static void cw_clin_trace(const char* label) {
	static unsigned budget = 12;
	if(budget == 0 || CW_SD_TRACE_OFF())
		return;
	budget--;
	FILE* f = fopen("sd:/cw_boot_trace.txt", "a");
	if(!f)
		return;
	fprintf(f, "clin=%s who=%08lx\n", label, (unsigned long)LWP_GetSelf());
	fflush(f);
	fclose(f);
}
#endif

bool clin_rpc_headroom(unsigned n) {
	struct client_rpc* taken[RPC_INBOX_SIZE];
	if(n > RPC_INBOX_SIZE)
		return false;
	/* order 2: tokens behind pending output are not headroom */
	if(!clin_rpc_flush_pending())
		return false;
	unsigned got = 0;
	while(got < n && tchannel_receive(&clin_empty_msg, (void**)&taken[got], false))
		got++;
	/* Back into the free pool (capacity RPC_INBOX_SIZE: never blocks). */
	for(unsigned k = 0; k < got; k++)
		tchannel_send(&clin_empty_msg, taken[k], true);
	return got == n;
}

static bool clin_try_rpc_send_impl(struct client_rpc* call,
	uint32_t* on_enqueue_counter) {
	if(client_settle_observe)
		clin_settle_rpc(SET_SEND_BEGIN, call, NULL);
	// S5.3 hunt (round 4): a generic "caller is the client thread -> skip
	// the channel" branch lived here briefly and was REVERTED -- it broke 4
	// PC tests (test_near_evict_defer, test_tnt_primed_env,
	// test_minecart_authority, test_boat_env; 0xc0000409, reproduced in
	// isolation, not load noise) because several test harnesses legitimately
	// call this function from the SAME OS thread clin_init() ran on while
	// deliberately exercising the channel's queueing/saturation semantics
	// (e.g. test_near_evict_defer's "client RPC token pool did not
	// saturate"). A thread-identity check here can't distinguish "the MP
	// remote path calling from the real client thread" from "a test harness
	// calling from its own single thread on purpose" -- see
	// clin_process_direct() below for the actual fix, applied ONLY at the
	// specific server_remote_protocol.c/_chunks.c call sites proven safe.
	struct client_rpc* empty;
#ifdef PLATFORM_WII
	cw_clin_trace("recv_empty_before");
#endif
	/* SOL-REVIEW-d42f Major 1: never overtake deferred output -- pending
	 * entries go first; if any remain, this try-send is refused */
	bool got_empty = clin_rpc_flush_pending()
		&& tchannel_receive(&clin_empty_msg, (void**)&empty, false);
#ifdef PLATFORM_WII
	cw_clin_trace(got_empty ? "recv_empty_after_ok" : "recv_empty_after_fail");
#endif
	if(!got_empty) {
		if(client_settle_observe)
			clin_settle_rpc(SET_STOP_POOL, call, NULL);
		return false;
	} // pool empty -> caller defers (NEVER blocks the server thread)
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_test_rpc_observer)
		s_test_rpc_observer(call, s_test_rpc_observer_user);
#endif
	if(clin_column_observe_publish) clin_column_observe_publish(call);
	*empty = *call;
	rpc_guest_generation[empty - rpc_msg] = 0;
	// We hold one of the RPC_INBOX_SIZE tokens, so the inbox has room -> this never blocks.
#ifdef PLATFORM_WII
	cw_clin_trace("send_inbox_before");
#endif
	if(client_settle_observe)
		clin_settle_rpc(SET_ENQUEUE, call, empty);
	if(on_enqueue_counter)
		__atomic_add_fetch(on_enqueue_counter, 1u, __ATOMIC_RELEASE);
	g9_probe_inbox((unsigned)(empty - rpc_msg), true);
	tchannel_send(&clin_inbox, empty, true);
	if(client_settle_observe)
		clin_settle_rpc(SET_SEND_END, call, NULL);
#ifdef PLATFORM_WII
	cw_clin_trace("send_inbox_after");
#endif
	return true;
}

bool clin_try_rpc_send(struct client_rpc* call) {
	return clin_try_rpc_send_impl(call, NULL);
}

bool clin_try_rpc_send_counted(struct client_rpc* call,
	uint32_t* on_enqueue_counter) {
	return clin_try_rpc_send_impl(call, on_enqueue_counter);
}



// S4 fix (adversarial-review MINOR closure): server_remote.c has no gstate
// access of its own by design (it only ever produces client_rpc events, it
// never reads client world state) -- this is the one read-only query it
// needs to mirror Beta's World.playNoteAt gate (getBlockId(x,y,z) > 0 before
// playing a note-block sound) [740c583 World.playNoteAt]. world_get_block()
// synthesizes a non-air "type=1" default for an unloaded chunk (see
// test_s1_partial_chunk_volume's own note on that fallback), so an explicit
// world_find_chunk() residency check comes first -- an unstreamed chunk
// must read as "block absent", not as the synthetic filler.
bool clin_world_block_present(int32_t x, int32_t y, int32_t z) {
	return world_find_chunk(CW_WORLD, x, y, z) != NULL
		&& world_get_block(CW_WORLD, x, y, z).type != BLOCK_AIR;
}

/* ---- mp-effects: Packet54 block events on the CLIENT world --------------
 * Beta runs Block.playBlock on WorldClient itself [740c583
 * NetClientHandler.handleNotePlay -> World.playNoteAt:2339-2344]. For a
 * piston that is the full BlockPistonBase.playBlock materialization
 * (tryExtend / the retract branch, BlockPistonBase.java:82-170): the client
 * builds its own TileEntityPiston cells and animates them; the server's later
 * Packet53 settle overwrites them. The redstone_piston.c transcription is the
 * same code the local server runs, here over the client world view. */
static int cbe_get_type(void* c, int x, int y, int z) {
	(void)c;
	/* An unloaded cell is never air to a push scan (the server adapter's own
	 * bedrock sentinel): a column end we cannot see refuses the push. */
	if(y < 0 || y >= WORLD_HEIGHT || !world_find_chunk(CW_WORLD, x, y, z))
		return BLOCK_BEDROCK;
	return world_get_block(CW_WORLD, x, y, z).type;
}
static int cbe_get_meta(void* c, int x, int y, int z) {
	(void)c;
	if(y < 0 || y >= WORLD_HEIGHT || !world_find_chunk(CW_WORLD, x, y, z))
		return 0;
	return world_get_block(CW_WORLD, x, y, z).metadata & 0x0F;
}
static float cbe_get_hardness(void* c, int id) {
	(void)c;
	if(id >= 0 && id < 256 && blocks[id])
		return (float)blocks[id]->digging.hardness;
	return 0.0F;
}
static int cbe_get_mobility(void* c, int id) {
	(void)c;
	return piston_mobility_flag(id);
}
static bool cbe_has_tile_entity(void* c, int x, int y, int z) {
	/* Same tile-entity id list as redstone_piston_server.c ps_has_tile_entity
	 * (Beta canPushBlock's getBlockTileEntity()==null gate). */
	int type = cbe_get_type(c, x, y, z);
	if(block_is_any_sign((uint8_t)type))
		return true;
	switch(type) {
		case BLOCK_DISPENSER:
		case BLOCK_NOTEBLOCK:
		case BLOCK_MOB_SPAWNER:
		case BLOCK_CHEST:
		case BLOCK_IRON_CHEST:
		case BLOCK_FURNACE:
		case BLOCK_FURNACE_LIT:
		case BLOCK_JUKEBOX:
		case BLOCK_PISTON_MOVING: return true;
		default: return false;
	}
}
static void cbe_set_block(void* c, int x, int y, int z, int id, int meta,
						  bool notify) {
	(void)c;
	(void)notify; /* the client world has no neighbour-notify simulation */
	if(y < 0 || y >= WORLD_HEIGHT || !world_find_chunk(CW_WORLD, x, y, z))
		return;
	client_piston_moving_remove(x, y, z);
	world_set_block(CW_WORLD, x, y, z,
		(struct block_data) {.type = (uint8_t)id,
							 .metadata = (uint8_t)(meta & 0x0F)},
		true);
}
static void cbe_add_moving_te(void* c, int x, int y, int z, int stored_id,
							  int stored_meta, int facing, bool extending,
							  bool is_source) {
	(void)c;
	if(y < 0 || y >= WORLD_HEIGHT || !world_find_chunk(CW_WORLD, x, y, z))
		return;
	struct piston_moving_state state = {
		.stored_id = (uint8_t)stored_id,
		.stored_meta = (uint8_t)(stored_meta & 0x0F),
		.facing = (uint8_t)facing,
		.flags = (uint8_t)((extending ? PISTON_MOVING_FLAG_EXTENDING : 0)
			| (is_source ? PISTON_MOVING_FLAG_SOURCE : 0)),
	};
	if(!client_piston_moving_upsert(x, y, z, &state))
		return; /* allocation refused: the cell keeps its block until the
				 * server's settle Packet53 (no orphan id-36 cell) */
	/* Beta setBlockAndMetadata(pistonMoving, meta): the cell metadata equals
	 * the stored metadata in every BlockPistonBase call site; mirrors the
	 * CRPC_PISTON_MOVING apply. */
	world_set_block(CW_WORLD, x, y, z,
		(struct block_data) {.type = BLOCK_PISTON_MOVING,
							 .metadata = state.stored_meta},
		true);
}
static void cbe_settle_moving_te(void* c, int x, int y, int z) {
	(void)c;
	/* TileEntityPiston.func_31011_l (clearPistonTileEntity). Beta's client
	 * settles its own finished entries in updateEntity; ours keep the entry
	 * until the server's Packet53, so an already-finished entry settles here
	 * too -- the cell ends in the same state Beta's client holds. */
	struct piston_moving_state state;
	if(!client_piston_moving_query(x, y, z, &state))
		return;
	client_piston_moving_remove(x, y, z);
	if(world_find_chunk(CW_WORLD, x, y, z)
	   && world_get_block(CW_WORLD, x, y, z).type == BLOCK_PISTON_MOVING)
		world_set_block(CW_WORLD, x, y, z,
			(struct block_data) {.type = state.stored_id,
								 .metadata = (uint8_t)(state.stored_meta
													   & 0x0F)},
			true);
}
static void cbe_spawn_drops(void* c, int x, int y, int z, int id, int meta) {
	/* Block.dropBlockAsItemWithChance is gated on !worldObj.multiplayerWorld
	 * [740c583 Block.java]; the server's Packet21 delivers the real drop. */
	(void)c; (void)x; (void)y; (void)z; (void)id; (void)meta;
}
static bool cbe_moving_te_state(void* c, int x, int y, int z, int* facing,
								bool* extending) {
	(void)c;
	struct piston_moving_state state;
	if(!client_piston_moving_query(x, y, z, &state))
		return false;
	if(facing)
		*facing = state.facing;
	if(extending)
		*extending = piston_moving_is_extending(&state);
	return true;
}

/* Beta EntityNoteFX (spawnParticle("note", x+0.5, y+1.2, z+0.5, pitch/24.0,
 * 0, 0), BlockNote.playBlock:69): colour from the pitch, texture index 64,
 * 6-tick life. Beta's motion is +0.2 up with *0.66 drag per tick (rises
 * ~0.54 block over its life); this particle model has no per-type drag, so
 * a constant 0.09/tick reproduces the same total rise. Cosmetic only. */
static void clin_note_particle(int32_t x, int32_t y, int32_t z, uint8_t pitch) {
	float d = (float)pitch / 24.0F;
	float rgb[3];
	for(int k = 0; k < 3; k++) {
		float v = sinf((d + (float)k / 3.0F) * 3.14159265F * 2.0F) * 0.65F
			+ 0.35F;
		rgb[k] = v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v);
	}
	vec3 pos = {(float)x + 0.5F, (float)y + 1.2F, (float)z + 0.5F};
	vec3 vel = {0.0F, 0.09F, 0.0F};
	particle_add(pos, vel, tex_atlas_lookup_particle(TEXAT_PARTICLE_NOTE),
		0.16F, 6.0F, false, (uint8_t)(rgb[0] * 255.0F),
		(uint8_t)(rgb[1] * 255.0F), (uint8_t)(rgb[2] * 255.0F), false,
		TEXTURE_ATLAS_PARTICLES);
}

enum clin_block_event_result clin_remote_block_event(
	int32_t x, int32_t y, int32_t z, uint8_t b1, uint8_t b2) {
	if(y < 0 || y >= WORLD_HEIGHT || !world_find_chunk(CW_WORLD, x, y, z))
		return CLIN_BLOCK_EVENT_NONE; /* World.playNoteAt: getBlockId > 0 */
	uint8_t id = world_get_block(CW_WORLD, x, y, z).type;
	if(id == BLOCK_NOTEBLOCK) {
		clin_note_particle(x, y, z, b2);
		return CLIN_BLOCK_EVENT_NOTE;
	}
	if(id != BLOCK_PISTON_BASE && id != BLOCK_PISTON_STICKY)
		return CLIN_BLOCK_EVENT_NONE; /* Block.playBlock is a no-op */
	/* b2 is the facing (PistonBlockTextures offsets index). Beta would throw
	 * on a facing > 5; a malformed packet is dropped here instead. */
	if(b2 > 5 || !clin_terrain_gate_open())
		return CLIN_BLOCK_EVENT_NONE;
	bool sticky = id == BLOCK_PISTON_STICKY;
	struct redstone_piston_world w = {
		.ctx = NULL,
		.get_type = cbe_get_type,
		.get_meta = cbe_get_meta,
		.get_hardness = cbe_get_hardness,
		.get_mobility_flag = cbe_get_mobility,
		.has_tile_entity = cbe_has_tile_entity,
		.set_block = cbe_set_block,
		.add_moving_te = cbe_add_moving_te,
		.settle_moving_te = cbe_settle_moving_te,
		.spawn_drops = cbe_spawn_drops,
		.moving_te_state = cbe_moving_te_state,
	};
	if(b1 == 0) {
		/* playBlock event 0: if(tryExtend) { setBlockMetadataWithNotify(
		 * facing|8); "tile.piston.out" }. */
		if(!piston_extend(&w, x, y, z, b2, sticky))
			return CLIN_BLOCK_EVENT_NONE;
		cbe_set_block(NULL, x, y, z, id, b2 | 8, true);
		return CLIN_BLOCK_EVENT_PISTON_OUT;
	}
	if(b1 == 1) {
		/* playBlock event 1: the retract branch, "tile.piston.in"
		 * unconditionally at its end. */
		piston_retract(&w, x, y, z, b2, sticky);
		return CLIN_BLOCK_EVENT_PISTON_IN;
	}
	return CLIN_BLOCK_EVENT_NONE; /* playBlock ignores other event ids */
}

int clin_selected_hotbar_slot(void) {
	if(!gstate.players[0].windows[WINDOWC_INVENTORY])
		return -1;
	struct inventory* inv
		= windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]);
	return inv ? (int)inventory_get_hotbar(inv) : -1;
}

// Headless-test drain: recycle every pending inbox token back to the empty pool WITHOUT
// processing it. The real client uses clin_update (which clin_process()es each RPC into the
// client world / gstate); a server_world-linked TEST that exercises server_world_set_block has
// no client world to process into, but STILL must keep the RPC_INBOX_SIZE inbox from filling --
// clin_rpc_send blocks (tchannel_receive on clin_empty_msg) once the token pool is exhausted, so
// a single block-set cascade that emits >RPC_INBOX_SIZE RPCs would deadlock the test thread. A
// test spins this on a dedicated drain thread (the client's consumer role) so the server thread's
// sends never block; it touches ONLY the channels, never gstate, so it needs no client setup.
// Not used by the game (the real client always runs clin_update). PC-test support only.
void clin_drain_discard(void) {
	void* call;
	for(;;) {
		/* A message the world gate held back is still owned by the inbox side:
		 * discard it first, exactly like a queued one. */
		if(s_world_held_call) {
			call = s_world_held_call;
			s_world_held_call = NULL;
		} else if(!clin_inbox_take(&call)) {
			break;
		}
		/* Discarding the envelope must also release any separately pooled
		 * payload exactly like clin_process would. Set-block-only fixtures never
		 * noticed this, but a headless full-world stream otherwise exhausts all 16
		 * chunk slots and deadlocks while its inbox tokens continue recycling. */
		struct client_rpc* rpc = call;
		if(client_settle_observe) {
			clin_settle_rpc(SET_DEQUEUE, rpc, rpc);
			clin_settle_rpc(SET_INSTALL_FAIL, rpc, rpc);
		}
		clin_rpc_release_pooled(rpc);
		clin_inbox_token_return(rpc);
	}
}

size_t clin_census_static(void) {
	return sizeof(rpc_msg) + sizeof(rpc_chunk_pool) + sizeof(far_rpc_pool) + sizeof(map_rpc_pool);
}
