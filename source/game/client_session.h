/* Player-owned presentation state. The world and entity store stay shared. */
#ifndef CLIENT_SESSION_H
#define CLIENT_SESSION_H

#include "../platform/time.h"
#include "camera.h"
#include "gui/container_compact.h"
#include "gui/recipe_browser.h"
#include "gui/local_menu_host.h"
#include "gui/sign_editor.h"
#include "gui/chat_composer.h"
#include "gui/local_pause.h"
#include "gui/screen_death.h"
#include "../network/recipe_craft.h"
#include "../network/server_interface.h"

#define CLIENT_LOCAL_PLAYERS 2
#define LOCAL_GUEST_LEAVE_ACK_DEADLINE_MS 20000

/* admit-retry-0925: first guest-body admission recovery. Binding ruling:
 * tmp/jobs/split-dimensions-plan-0924/out/ASTRA-RETIRE-RETRY-0925.md --
 * bounded RETRY for a transient allocation refusal of the first body install;
 * immediate, visible retirement for an invalid/colliding entity id; replies
 * for the refused body are retained in bounded session-owned storage and
 * replayed before gameplay opens; exhaustion, timeout or overflow retires the
 * guest through an authoritative Leave with a persistent readable reason.
 *
 * RETRY_MAX: refused first installs tolerated AFTER the first refusal (so at
 * most RETRY_MAX + 1 GAMEPLAY_READY replies are refused before retirement).
 * The retry itself is main.c's existing per-frame idempotent re-request.
 * HUD_RETAIN_MAX: ordered HUD notices retained (48 B each, 384 B/session).
 * TIMEOUT_MS: wall-clock bound on one recovery, from the first refusal.
 * NOTICE_MS: lifetime of the controller-readable retirement reason; the next
 * Join's generation bind clears it earlier (client_session_bind_local). */
#define CLIENT_GUEST_ADMIT_RETRY_MAX 8
#define CLIENT_GUEST_ADMIT_HUD_RETAIN_MAX 8
#define CLIENT_GUEST_ADMIT_TIMEOUT_MS 10000
#define CLIENT_GUEST_ADMIT_NOTICE_MS (10u * 60u * 1000u)
/* HUD notices are presented one at a time with the existing toast timing
 * (CLIENT_HUD_NOTICE_MS). Retained (replayed) notices, and every notice that
 * waited in the queue, are PROTECTED for their whole turn: a live notice that
 * arrives meanwhile queues behind them in arrival order (Astra reviews A/3).
 * Sized for the eight retained notices plus eight live ones. Overflow is
 * loss-explicit, never a silent replace: the arriving notice is dropped and
 * COUNTED, and once the queue drains a "(+N more)" notice takes its own turn.
 * (Retained-notice overflow during admission recovery never reaches this
 * queue: it retires the guest -- CLIENT_GUEST_ADMIT_HUD_RETAIN_MAX.) */
#define CLIENT_HUD_NOTICE_MS 3000u
#define CLIENT_HUD_QUEUE_MAX 16

enum client_guest_admit_refusal {
	CLIENT_GUEST_ADMIT_TRANSIENT, /* body allocation refused: bounded retry */
	CLIENT_GUEST_ADMIT_INVALID,   /* id 0 or an occupied id: retire now */
};

/* Retained state for a refused first install of ONE generation. Health, sleep
 * and fire are absolute assignments of disjoint session/body fields
 * (client_session_player_reply), so the latest value alone reproduces the
 * in-order result: those are coalesced (fire uses the C2b latch). HUD notices
 * are distinct user-visible events: kept in arrival order, never coalesced. */
struct client_guest_admit_recovery {
	ptime_t deadline;
	uint8_t refusals;
	uint8_t hud_count;
	bool active;
	/* The retirement Leave is this generation's cleanup. If the server could
	 * not checkpoint (LEAVE_FAILED -> FAILED, still bound, ownership kept),
	 * the join card's Retry re-sends it (client_session_request_local_join). */
	bool retiring;
	bool health_retained, sleep_retained, sleeping;
	int16_t health;
	int32_t sleep_timer;
	char hud[CLIENT_GUEST_ADMIT_HUD_RETAIN_MAX][48];
};

enum client_guest_phase {
	CLIENT_GUEST_IDLE, CLIENT_GUEST_REQUESTED, CLIENT_GUEST_INSTALLING,
	CLIENT_GUEST_CONFIRMING, CLIENT_GUEST_ATTACHED, CLIENT_GUEST_LEAVING,
	CLIENT_GUEST_FAILED
};

struct client_local_leave_token {
	uint64_t ticket;
	uint64_t generation; /* zero identifies an unbound Cancel */
};

enum client_local_leave_outcome {
	CLIENT_LOCAL_LEAVE_PENDING,
	CLIENT_LOCAL_LEAVE_TIMED_OUT,
	CLIENT_LOCAL_LEAVE_FAILED,
	CLIENT_LOCAL_LEAVE_LEFT,
	CLIENT_LOCAL_LEAVE_STALE,
};
enum respawn_failure_state {
	RESPAWN_FAILURE_NONE,RESPAWN_FAILURE_RETRY,RESPAWN_FAILURE_TERMINAL
};

struct screen;
struct client_rpc;
struct client_session;
struct inventory_sort_plan;
struct world;
/* Slice 2 supplies opaque/pinned records. False refuses ambiguous storage;
 * NULL uses item_data_pinned on the latest window. No persistence ownership
 * crosses this boundary. */
typedef bool (*sort_pinned_mask_fn)(void*, struct client_session*, uint8_t,
	bool*, size_t);

/* F3/F4: centralized client-side minecart ride transaction and presentation
 * state. One instance per local player (not per cart). Reset through minecart_ride_client_state_reset from clin_init,
 * CRPC_WORLD_RESET, target CRPC_ENTITY_DESTROY, death/body replacement,
 * and terminal load failure. Existing per-cart cumulative-control fields
 * remain per cart; this does not duplicate them. */
struct minecart_ride_client_state {
	uint32_t target_id;          /* cart entity id for the current intent */
	uint32_t input_epoch;        /* world/input-handoff generation */
	uint32_t next_request_seq;   /* monotonic; nonzero when a request is live */
	uint32_t pending_request_seq;/* echoed by the correlated reply */
	bool desired_occupied;       /* true = ride, false = leave */
	bool send_dirty;             /* author on next tick if still set */
	bool pending;                /* waiting for correlated reply */
	float exit_floor_y;          /* cart_y + 1.0 + 1.2 clearance floor */
	bool exit_lift_pending;      /* reapplied exactly once on matching reply */
	bool exit_repin_eligible;    /* reserved fallback latch; dismount authoring
	                             * initializes false and the current follow/
	                             * snapshot guards prevent re-pin. Any future
	                             * reconciliation that actually re-pins must set
	                             * this before the floor may raise the player. */
	vec3 camera_correction;      /* presentation-only offset, decays to zero */
	vec3 camera_correction_start;/* first displayed sample for convergence */
	float camera_correction_elapsed;
	bool camera_correction_active;
	/* D1: host ride-action (F3-1 JUMP dismount) edge latched by the per-frame
	 * capture and consumed once by the next host tick. Scoped to the input
	 * epoch and the ride transaction it was captured under (any new ride
	 * intent bumps next_request_seq); living in this struct, it is also
	 * cleared by every minecart_ride_client_state_reset (world reset, cart
	 * destroy, death/body replacement, load failure, clin_init). */
	bool action_latched;
	uint32_t action_cart_id;
	uint32_t action_ride_epoch; /* O07 D1-1: the ride lifetime it was taken in */
	uint32_t action_input_epoch;
	uint32_t action_request_seq;
};

struct client_session {
	/* Menu-only conversion receipt; it never installs a body or enters Play.
	 * Poll until settled even after Cancel, before destroying the UI owner. */
	struct svin_convert_result convert;
	uint32_t convert_serial;
	bool convert_pending;
	/* Achievement ownership is a session lifetime, independent of the world
	 * and body. Remote transport generation identifies reconnects only. */
	uint64_t achievement_epoch;
	uint32_t achievement_remote_generation;
	sort_pinned_mask_fn sort_pinned_mask;
	void* sort_pinned_user;
	/* Main-thread local-provider binding. Generation remains a stale-reply floor
	 * after detach; clearing windows never makes an old token current again. */
	uint16_t map_counter; // client World MapStorage lifetime, shared by windows
	uint64_t local_generation;
	uint64_t local_ticket;
	enum client_guest_phase local_phase;
	int local_pending_action;
	bool local_pending;
	ptime_t local_leave_deadline;
	bool local_leave_deadline_armed;
	/* An expiry handed the wait to an explicit Retry owner; if that owner is
	 * dismissed before settlement the tick takes the transport retry back. */
	bool local_leave_retry_handed_off;
	struct client_local_leave_token local_leave_receipt;
	enum client_local_leave_outcome local_leave_receipt_outcome;
	bool local_leave_receipt_active;
	bool local_leave_receipt_manual_retry;
	bool local_bound;
	bool local_gameplay_ready; /* explicit body + collision/mesh admission */
	bool local_reply_failed; /* Current-generation allocation/protocol failure: stop input. */
	/* residency-1005/6 R2: the server's bounded Join terrain wait terminated
	 * this generation (LOCAL_GUEST_JOIN_LOADING_FAILED). No further GAMEPLAY
	 * is asked; the UI releases the session (Leave) and its Retry re-Joins. */
	bool local_join_failed;
	/* split-dimensions-plan-0924/C2b: the last CRPC_LOCAL_PLAYER_FIRE this
	 * generation's guest route delivered for the guest body, kept on the
	 * SESSION so it survives a body that is not installed yet. The server
	 * publishes on change and records the send the moment the route accepts
	 * it (server_local_publish_split_fire); if the first GAMEPLAY_READY
	 * install was refused (client_interface clin_guest_body_install), the
	 * retried idempotent READY moves no resend key, so the body installed by
	 * the retry must take this value or the flame is lost until the next real
	 * change. Zeroed with the rest of the session by client_session_bind_local
	 * (a new generation has no publish yet). */
	bool local_fire_latched, local_fire;
	/* admit-retry-0925: first-body admission recovery for this generation.
	 * Zeroed by client_session_bind_local and cleared by
	 * client_session_clear_windows (leave, unbind, world reset). */
	struct client_guest_admit_recovery local_admit;
	/* Retirement reason. Its OWN slot, never the toast: drawn on the host HUD
	 * (client_session_local_admit_failure_reason) until the next Join's
	 * generation bind or client_session_local_admit_failure_clear. Survives
	 * the Leave and world reset (clear_windows). pending = the join-card edge
	 * not yet taken by client_session_local_admit_failure_present. */
	char local_admit_notice[48];
	bool local_admit_notice_pending;
	/* A respawn keeps the existing body bound but inert while the destination
	 * streams.  send_pending means no reliable envelope has been accepted yet;
	 * awaiting_commit means exactly one has. */
	bool local_respawn_loading, local_respawn_correction_installed;
	bool local_respawn_ack_send_pending, local_respawn_awaiting_commit;
	uint64_t local_respawn_epoch, local_respawn_completed_epoch;
	double local_respawn_x, local_respawn_y, local_respawn_z;
	/* Authoritative destination of this epoch, independent of view 1's old
	 * world while the dead guest waits for the target terrain. */
	uint8_t local_respawn_world_slot;
	uint32_t local_respawn_world_generation;
	int8_t local_respawn_dimension;
	/* The first accepted LOADING freezes presentation at the last settled
	 * camera. Corrections may move the hidden body/target, but rendering keeps
	 * this snapshot until matching READY or release. */
	bool local_respawn_render_camera_frozen;
	struct camera local_respawn_render_camera;
	/* Owner-0 same-world transaction has a server-lifetime epoch floor that
	 * survives ordinary world reset. */
	bool host_respawn_loading,host_respawn_correction_installed;
	bool host_respawn_ack_send_pending,host_respawn_awaiting_commit;
	uint64_t host_respawn_epoch,host_respawn_completed_epoch;
	double host_respawn_x,host_respawn_y,host_respawn_z;
	bool host_respawn_render_camera_frozen;
	struct camera host_respawn_render_camera;
	enum respawn_failure_state respawn_failure;
	uint64_t respawn_failure_epoch;
	uint32_t respawn_failure_serial;
	bool respawn_request_observation_pending;
	bool local_death_leave_manual;
	/* One selected-recipe request may be live per owning player. Completion is
	 * installed only when all three echoed ownership fields match. */
	uint32_t craft_pending_ticket;
	uint32_t craft_reply_serial;
	/* Liveness: the request is also retired locally when the window it named is
	 * gone/hidden/replaced (generation bump) or the deadline passes -- see
	 * client_session_recipe_craft_tick. */
	uint32_t craft_pending_generation;
	ptime_t craft_deadline;
	uint8_t craft_pending_player;
	uint8_t craft_pending_window;
	enum recipe_craft_reason craft_reason;
	bool craft_pending;
	/* Outgoing prediction belongs to this body/session, never another local
	 * player's correction or send cadence. Terrain admission remains separate. */
	struct {
		ptime_t last_pos_update;
		uint32_t correction_seq;
		uint32_t state_correction_seq;
		bool authoritative_pos_seen;
		bool state_known, state_on_ground, state_sneaking;
		/* MP-BED: leave_bed_seq is bumped by the remote sleep screen's Leave
		 * Bed press; state_leave_bed_seq is the last value SRPC_PLAYER_STATE
		 * carried. */
		uint32_t leave_bed_seq, state_leave_bed_seq;
	} player_sync;
	struct screen* current_screen;
	// HUD toast (CRPC_HUD_MESSAGE): shown centered above the hotbar until the deadline.
	char hud_message[48];
	ptime_t hud_message_until;
	/* Notices waiting behind hud_message, oldest at hud_queue_head. */
	char hud_queue[CLIENT_HUD_QUEUE_MAX][48];
	uint8_t hud_queue_head, hud_queue_count;
	bool hud_message_protected; /* showing a retained/queued notice's turn */
	uint16_t hud_queue_dropped; /* overflow drops, shown as "(+N more)" */
	bool in_water;
	int oxygen;
	struct digging {
		bool active;
		ptime_t start;
		ptime_t cooldown;
		w_coord_t x, y, z;
	} digging;
	struct held_anim {
		struct {
			ptime_t start;
			bool place;
			/* true = ONLY the item re-equip dip (Beta ItemRenderer
			 * func_9450_c after a sendUseItem that changed the stack:
			 * equippedProgress = 0), with NO arm swing -- Beta's
			 * Minecraft.clickMouse(1) calls swingItem only after a true
			 * sendPlaceBlock [Minecraft.java:817-820, 834-838]. Zero
			 * (every older writer) = the swing. See held_anim_swing. */
			bool equip_only;
		} punch;
		struct {
			ptime_t start;
			size_t old_slot;
		} switch_item;
	} held_item_animation;
	struct camera camera;
	struct camera_ray_result camera_hit;
	struct entity* local_player; /* borrowed from the shared entity store */
	struct window_container* windows[256]; /* exclusively owned */
	struct container_compact container;
	struct recipe_browser recipes;
	struct manual_grid manual_grid;
	struct local_menu_host menu_host;
	struct sign_editor sign;
	struct chat_composer chat; /* multiplayer chat composer, player 0 only */
	struct local_pause pause;
	struct death_ui death;
	struct minecart_ride_client_state ride;
	int32_t sleep_timer;
	bool sleeping;
	int visible_window, deferred_reopen_window; /* -1 means no window */
};

bool client_session_sort_request(struct client_session*, uint8_t,
	unsigned operation /* inventory_sort_operation */, bool remote, ptime_t,
	container_send_fn, void*);

/* False admission leaves the previous receipt intact. Poll consumes exactly
 * one matching completion; stale/repeated receipts cannot advance serial. */
bool client_session_convert_start(struct client_session*, const char* path);
bool client_session_convert_cancel(struct client_session*);
bool client_session_convert_poll(struct client_session*);

/* Main-thread provider boundaries; remote generation is authenticated by the
 * provider status. No protocol traffic, and no server-side MP achievements. */
void client_session_achievements_remote(struct client_session*, uint32_t generation,
	bool admitted);
void client_session_achievements_sync(struct client_session*, uint8_t slot);
bool client_session_achievements_warning(struct client_session*, uint8_t slot, bool ready);
/* plan must come from inventory_sort_build, remain immutable until the run
 * stops, and live in static/MEM2 storage on Wii. request() manages that lease.
 * Call tick after this frame's window input and inbound replies are consumed. */
bool client_session_sort_start(struct client_session*, uint8_t,
	const struct inventory_sort_plan*, bool chest, bool remote, ptime_t,
	container_send_fn, void*);
void client_session_sort_tick(struct client_session*, uint8_t, ptime_t,
	container_send_fn, void*);

/* Frees only this player's window snapshots, never the shared world/body.
 * Safe to repeat. Borrowed stack windows in tests must be detached first. */
void client_session_clear_windows(struct client_session* session);

/* Applies inventory data/transaction replies to the explicitly named owner.
 * Returns false for non-window RPCs. Screen/lifecycle transitions remain the
 * caller's responsibility; there is no implicit current-player selection. */
bool client_session_window_reply(struct client_session* session,
	const struct client_rpc* call);
/* Send one "craft this recipe once" request for `player`'s open crafting
 * window. `recipe_index` is the flat numbering in recipe_craft.h -- exactly the
 * enumeration the recipe guide itself walks. At most one request is in flight
 * per session; the reply retires it (see client_session_window_reply). Returns
 * false without sending when the window is not a settled, idle crafting window
 * this player owns. No slot is predicted: the server is authoritative, and its
 * slot updates precede the reply in the same FIFO. */
bool client_session_recipe_craft_request(struct client_session* session,
	uint8_t player, uint8_t window, uint16_t recipe_index,
	container_send_fn send, void* user);

/* How long the guide waits for the reply before retiring the request itself.
 * Generous on purpose: an accepted request is answered on the very next server
 * tick, so this only ever fires when the reply was LOST (a dropped envelope the
 * server could not attribute, a torn-down provider). It is longer than the
 * window-sort deadline because a craft has no correction/replay to fence -- the
 * server is authoritative and simply did nothing. */
#define CLIENT_RECIPE_CRAFT_TIMEOUT_MS 10000

/* Retire an in-flight craft request that can no longer be answered: its window
 * closed, was replaced (a new window_container generation), stopped being the
 * visible one, or the deadline above passed. Installs RECIPE_CRAFT_TIMED_OUT
 * and bumps craft_reply_serial, so a guide waiting on the completion edge sees
 * one either way. Idempotent; safe to call every frame with time_get(). */
void client_session_recipe_craft_tick(struct client_session* session, ptime_t now);

/* Body/HUD replies without world, screen or terrain side effects. A health
 * update without a body fails; callers may still install sleep/toast state. */
bool client_session_player_reply(struct client_session*, const struct client_rpc*, ptime_t now);

/* MP-BED: one Beta EntityPlayer.onUpdate sleep-timer step (EntityPlayer.java:
 * 60-78): +1 capped at 100 while sleeping; once awake, +1 from a nonzero
 * timer until it reaches 110, then 0. Remote sessions only (the local server
 * authors the timer itself and sends it in CRPC_SLEEP_STATE). Pure. */
int32_t client_session_sleep_timer_step(bool sleeping, int32_t timer);

/* Main-thread detach acknowledgements must carry the admitted generation. */
bool client_session_unbind_local(struct client_session*, uint64_t generation);
bool client_session_bind_local(struct client_session* session, uint64_t generation);
bool client_session_local_reply(struct client_session* session,
	uint64_t generation, const struct client_rpc* call);

/* Main-thread local-provider handshake. ATTACHED certifies window ownership,
 * not terrain/body readiness. A Join UI must also satisfy gameplay admission. */
bool client_session_request_local_join(struct client_session*);
/* Experimental local split-screen: ask the server to admit this session's
 * BODY. ATTACHED-only and retryable; the caller must have the experimental
 * switch on. Granting it is what finally sets local_gameplay_ready. */
bool client_session_request_local_gameplay(struct client_session*);
/* admit-retry-0925: the caller (client_interface) refused to install the body
 * a matching-generation GAMEPLAY_READY named. TRANSIENT starts or continues a
 * bounded recovery; INVALID, or a transient refusal past the retry bound,
 * retires the guest with a persistent HUD reason (see the constants above). */
void client_session_local_admit_refused(struct client_session*,
	enum client_guest_admit_refusal);
/* Main-thread, once per frame: true exactly once per admission retirement, so
 * the caller settles the join card as refused. `host` is unused since the
 * reason moved to its own slot; kept so the main.c seam stays one call. */
bool client_session_local_admit_failure_present(struct client_session* guest,
	struct client_session* host);
/* The persistent admission-failure reason the host HUD draws on its own line
 * (independent of the toast), or NULL. Cleared only by the next Join's bind
 * or client_session_local_admit_failure_clear -- never by a HUD message. */
const char* client_session_local_admit_failure_reason(
	const struct client_session* guest);
void client_session_local_admit_failure_clear(struct client_session* guest);
/* Advance the queued HUD notices once the showing one has expired. */
void client_session_hud_tick(struct client_session*, ptime_t now);
bool client_session_request_local_leave(struct client_session*);
bool client_session_local_leave_begin(struct client_session*,
	struct client_local_leave_token*);
bool client_session_local_leave_retry(struct client_session*,
	const struct client_local_leave_token*);
enum client_local_leave_outcome client_session_local_leave_observe(
	const struct client_session*, const struct client_local_leave_token*);
bool client_session_local_leave_release(struct client_session*,
	const struct client_local_leave_token*);
void client_session_local_leave_tick(struct client_session*,ptime_t);
void client_session_flush_local(struct client_session*);
void client_session_flush_window_transactions(struct client_session*);
void client_session_local_status(struct client_session*, const struct client_rpc*);
bool client_session_local_respawn_readiness(struct client_session*,
	struct world*, bool world_loaded, bool world_failed, bool terrain_hold);
bool client_session_local_respawn_hidden_camera(const struct client_session*,
	struct camera*);
struct camera* client_session_local_render_camera(struct client_session*);
bool client_session_host_respawn_hidden_camera(const struct client_session*,struct camera*);
bool client_session_host_respawn_readiness(struct client_session*,struct world*,bool,bool,bool);
void client_session_flush_host_respawn(struct client_session*);
void client_session_host_respawn_world_reset(struct client_session*);
void client_session_host_respawn_transport_reset(struct client_session*);
bool client_session_try_local_leave(struct client_session*);
bool client_session_host_gameplay_active(const struct client_session*);
bool client_session_local_gameplay_active(const struct client_session*);

/* The first-person / third-person arm swing, Beta EntityPlayer.swingItem
 * [EntityPlayer.java:483-486] as this engine's held-item punch timer. `dip`
 * adds the re-equip dip (ItemRenderer.func_9449_b on a stack-size change,
 * Minecraft.java:827-829). Every swing site goes through here so a previous
 * equip-only dip can never suppress it. */
static inline void held_anim_swing(struct held_anim* a, ptime_t now, bool dip) {
	a->punch.start = now;
	a->punch.place = dip;
	a->punch.equip_only = false;
}

/* The re-equip dip WITHOUT a swing: sendUseItem returned true
 * (ItemRenderer.func_9450_c, Minecraft.java:836-838) -- eating, a thrown
 * egg/snowball, a boat. */
static inline void held_anim_equip_dip(struct held_anim* a, ptime_t now) {
	a->punch.start = now;
	a->punch.place = true;
	a->punch.equip_only = true;
}

/* Swing progress 0..1 for the arm (0 when idle or equip-only), shared by the
 * first-person hand and the third-person body of a local split peer. */
static inline float held_anim_swing_lerp(const struct held_anim* a,
										 ptime_t now) {
	if(a->punch.equip_only)
		return 0.0F;
	float t = time_diff_s(a->punch.start, now) / 0.4F;
	return (t < 0.0F || t >= 1.0F) ? 0.0F : t;
}

#endif
