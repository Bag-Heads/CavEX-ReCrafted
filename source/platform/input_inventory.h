/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef PLATFORM_INPUT_INVENTORY_H
#define PLATFORM_INPUT_INVENTORY_H

#include "input_owner.h"
#include "time.h"

#define INPUT_DEVICE_COUNT 8
#define INPUT_IDENTITY_STABLE_MS 250
#define INPUT_NOTICE_MS 3000
/* Capacity covers the normal debounced burst. Assignment/join churn is not
 * time-bounded: on saturation evict the oldest notice and count it. */
#define INPUT_NOTICE_COUNT (INPUT_DEVICE_COUNT * 19 + 4)

/* Low-battery hysteresis. The approved sound map fixes the CUE policy ("once
 * per low-battery episode, rearmed only after a stable recovery/replacement,
 * no periodic chirp, never for a wired GameCube controller") but names no
 * percentage, so these thresholds are this input layer's, chosen so a reading
 * that oscillates around the boundary cannot produce a second episode: the
 * recovery threshold sits a full 10 points above the low threshold and both
 * edges need a stable observation window. Documented as a controller
 * playability deviation in docs/wii_controller_playability.md. */
#define INPUT_BATTERY_LOW_PERCENT 20
#define INPUT_BATTERY_RECOVER_PERCENT 30
#define INPUT_BATTERY_STABLE_MS 2000
/* The IR pointer crosses the sensor boundary constantly, so its visual notice
 * debounces far longer than an identity change (and, per the sound map, is
 * silent). */
#define INPUT_POINTER_STABLE_MS 750
/* Loss needs a longer stable window than regain, and a lost notice for one
 * device is not repeated within the cooldown. */
#define INPUT_POINTER_LOST_STABLE_MS 1500
#define INPUT_POINTER_NOTICE_COOLDOWN_MS 10000

/* The nine approved controller events (docs/ui_design/approved/specs/menus.md
 * "Controller events" plus the menus.html event selector), split into the
 * DISTINCT states the approved cards need. Hardware identity (DETECTED) and
 * player assignment (CONNECTED) are deliberately different states, and a
 * device returning after a loss (RECONNECTED) is deliberately not a new
 * player. Join failure/cancel are modelled here so their cards exist; they
 * never admit a player -- see struct input_join. */
enum controller_event {
	CONTROLLER_EVENT_NONE = 0,
	CONTROLLER_EVENT_DETECTED,	 /* usable hardware appeared, unassigned */
	CONTROLLER_EVENT_CONNECTED,	 /* usable AND assigned to a player */
	CONTROLLER_EVENT_RECONNECTED, /* usable again after a loss */
	CONTROLLER_EVENT_DISCONNECTED,
	CONTROLLER_EVENT_ATTACHMENT_CHANGED, /* one usable family -> another */
	CONTROLLER_EVENT_ATTACHMENT_MISSING, /* bare Remote, nothing to play with */
	CONTROLLER_EVENT_LOW_BATTERY,
	CONTROLLER_EVENT_POINTER_LOST,
	CONTROLLER_EVENT_POINTER_REGAINED,
	CONTROLLER_EVENT_JOIN_OFFERED,
	CONTROLLER_EVENT_JOIN_FAILED,
	CONTROLLER_EVENT_JOIN_CANCELLED,
	CONTROLLER_EVENT_COUNT
};

struct input_identity {
	bool connected;
	/* WIIMOTE means a bare Remote, not a playable controller family. */
	enum input_category category;
};
struct input_device_status {
	struct input_identity observed, published;
	ptime_t deadline;
	bool initialized, armed, identity_reported;
	/* Published identity was a playable family; and has EVER been one. The
	 * pair separates a first connection from a return. */
	bool published_usable, ever_usable;
	/* Player assignment as of the last publication, so an assignment edge on
	 * an unchanged identity still publishes "Player N Connected". */
	bool published_assigned;
	unsigned published_player;
	/* Low-battery episode. `battery_low` is the PUBLISHED side of the
	 * hysteresis; `pending_low` is the candidate waiting out its window. */
	bool battery_known, battery_low, battery_pending_low;
	ptime_t battery_deadline;
	/* Pointer validity. Published from the FIRST observation without an
	 * event: a pointer that was never aimed at the sensor bar has not been
	 * "lost", and a notice on every boot would be exactly the nagging the
	 * sound map forbids audio for. */
	bool pointer_known, pointer_valid, pointer_pending_valid;
	ptime_t pointer_deadline;
	bool pointer_cooldown_armed;
	ptime_t pointer_cooldown_until;
	/* Set when this device's battery episode ends (stable recovery, wired
	 * hardware, or the device going away); consumed by
	 * controller_events_announce, which owns the audio latch. */
	bool battery_rearm;
	/* The last edge this device published -- including the pointer regain,
	 * which retires its card instead of raising one. */
	enum controller_event last_event;
};
/* Keep the naturally aligned time first; bounded enum values need one byte.
 * No packed/unaligned accesses on Broadway. 16 bytes with the Wii ABI. */
struct input_connection_notice {
	ptime_t expires;
	uint8_t kind, channel, player, event, category, previous_category;
	uint8_t active : 1;
	uint8_t assigned : 1;
	uint8_t announced : 1; /* Set after the edge's one audio decision. */
	uint8_t connected : 1;
};
#ifdef PLATFORM_WII
_Static_assert(sizeof(struct input_connection_notice) == 16, "Wii notices must stay 16 bytes");
#endif
static inline struct input_device
input_notice_device(const struct input_connection_notice* notice) {
	return (struct input_device) {notice->kind, notice->channel};
}

/* Player-2 admission, modelled so the approved join/cancel/failure cards
 * exist. NOTHING here binds an owner or sets local_gameplay_ready: the split
 * admission path is still gated, so a request always settles as FAILED unless
 * a caller supplies a real admission. */
enum input_join_state {
	INPUT_JOIN_IDLE,
	INPUT_JOIN_OFFERED, /* a usable unassigned device was detected */
	INPUT_JOIN_JOINING,
	INPUT_JOIN_FAILED
};
struct input_join {
	enum input_join_state state;
	struct input_device device;
};

struct input_inventory {
	struct input_device_status devices[INPUT_DEVICE_COUNT];
	struct input_connection_notice notices[INPUT_NOTICE_COUNT];
	struct input_join join;
	uint32_t notices_dropped; /* Saturating count of oldest-card evictions. */
	bool split_join_armed;	  /* input.c: a new pad's A is Join P2 (else it takes over P1) */
	/* input.c, once per poll: the 1-based player whose bound device has been
	 * PUBLISHED gone (presence hold + identity settle), when exactly one is;
	 * 0 = none. A new pad's A then takes that seat -- never a split join. */
	uint8_t vacant_seat;
};

/* Main-thread only. Fresh means a driver report, not a retained queue value.
 * Invalid input suppresses immediately; identity publication is independent. */
void input_inventory_poll(struct input_inventory* inventory,
						  struct input_device device,
						  struct input_identity identity, bool valid,
						  bool fresh, bool neutral, ptime_t now,
						  const struct input_owner* owners, size_t owner_count);
/* Battery percentage for ONE device, edge-triggered with hysteresis.
 * `supported` is false for wired hardware and for a wireless device that has
 * never delivered a status report; an unsupported reading resets the episode
 * without publishing anything. */
void input_inventory_battery(struct input_inventory* inventory,
							 struct input_device device, bool supported,
							 unsigned percent, ptime_t now,
							 const struct input_owner* owners,
							 size_t owner_count);
/* IR pointer validity for ONE device. `supported` is false for a family with
 * no pointer (Classic, GameCube) and for an unassigned device -- "pointer out
 * of view" is only meaningful for the pointer controller a player is using. */
void input_inventory_pointer(struct input_inventory* inventory,
							 struct input_device device, bool supported,
							 bool valid, ptime_t now,
							 const struct input_owner* owners,
							 size_t owner_count);
/* A queued identity change can occur and reverse within a single frame. */
void input_inventory_interrupt(struct input_inventory* inventory,
							   struct input_device device);
const struct input_device_status*
input_inventory_device(const struct input_inventory* inventory,
					   struct input_device device);

/* Join lifecycle. `request` moves an OFFERED device to JOINING; `settle`
 * takes the caller's real admission result and publishes JOIN_FAILED when it
 * is false; `cancel` returns to IDLE and publishes JOIN_CANCELLED. None of
 * them binds an owner. */
bool input_inventory_join_request(struct input_inventory* inventory,
								  struct input_device device, ptime_t now);
void input_inventory_join_settle(struct input_inventory* inventory,
								 bool admitted, ptime_t now);
void input_inventory_join_cancel(struct input_inventory* inventory,
								 ptime_t now);

bool input_notice_visible(const struct input_connection_notice* notice,
						  unsigned player, ptime_t now);
void input_notice_text(const struct input_connection_notice* notice, char* text,
					   size_t size);
/* Physical rectangle supplied by the viewport owner; no screen-size globals. */
bool input_notice_origin(const struct input_connection_notice* notice,
						 unsigned player, ptime_t now, int x, int y, int width,
						 int height, int* out_x, int* out_y);

/* ---- Approved card model -------------------------------------------------
 * The presentation the approved package specifies, as DATA: no drawing, no
 * screen-size globals, no texture handles. gutil_connection_notices renders
 * it; the tests assert it. */
enum controller_picture {
	CONTROLLER_PICTURE_NONE,
	CONTROLLER_PICTURE_REMOTE,
	CONTROLLER_PICTURE_NUNCHUK,
	CONTROLLER_PICTURE_CLASSIC,
	CONTROLLER_PICTURE_GAMECUBE,
	CONTROLLER_PICTURE_BATTERY
};
struct controller_card {
	enum controller_event event;
	/* "Player 2 Connected", "New controller", "Could not join", ... */
	char text[48];
	/* Approved actions, as labels. Empty when the card offers none. The
	 * confirm action is the A-family button, the cancel action the B-family
	 * one, exactly as the approved prototype draws them. */
	char action_confirm[16], action_cancel[16];
	enum controller_picture picture;
	int x, y;			  /* upper-left of the affected view */
	unsigned duration_ms; /* INPUT_NOTICE_MS */
};
/* True when this notice is live for `player` and fills `out`. `split` selects
 * the approved split-screen wording for the detected-device card ("No free
 * player slot" instead of the single-player join offer). */
bool controller_card_build(const struct input_connection_notice* notice,
						   unsigned player, bool split, ptime_t now, int x,
						   int y, int width, int height,
						   struct controller_card* out);
/* A new pad's A only joins P2 while the split join is armed; otherwise
 * input_p1_takeover makes that pad player 1's device. The detected card must
 * name the player the press drives, so the "Join P2" label becomes "Play as
 * P1" unless inventory->split_join_armed. A vacant seat (inventory->
 * vacant_seat) wins over both: the card reads "Play as P<seat>". */
void controller_card_name_join_target(struct controller_card* card,
									  const struct input_inventory* inventory);
/* The approved hardware name for a family ("Wii Remote + Nunchuk", ...). */
const char* controller_family_name(enum input_category category);
enum controller_picture controller_picture_of(struct input_device device,
											  enum input_category category);

const struct input_inventory* input_devices(void);

/* critique-1028 P1 controller takeover (AUDIT-GC-GLYPHS.md section 2).
 * May `candidate` take player 1's seat with a deliberate Confirm press?
 * Only when exactly player 1 is bound (no player 2 bound and no split seat
 * offered: split_join_armed means a new pad's A is "Join P2", never a steal),
 * the candidate is a different device, it is armed (seen neutral since it
 * appeared, so a held A can never claim), and it is a playable family (a
 * bare Remote is never one). Remote/online sessions use the same rule: the
 * input layer owns P1's device in both. */
bool input_p1_takeover_eligible(const struct input_owner* owners,
								size_t owner_count, bool split_join_armed,
								struct input_device candidate, bool armed,
								enum input_category category, bool confirm);
/* ui1031/p1-takeover (maintainer, real Wii, 8b32466): the 1-based player whose
 * bound device the inventory has PUBLISHED as disconnected (after the GC
 * presence hold and the identity settle; a device never published usable is
 * not "gone", it is still arriving), when exactly ONE bound player is in that
 * state; 0 otherwise (two missing owners are never guessed between). */
unsigned input_inventory_vacant_seat(const struct input_inventory* inventory,
									 const struct input_owner* owners,
									 size_t owner_count);
/* May `candidate` take the vacant seat (1-based) with a deliberate Confirm?
 * Split join armed or not, P2 bound or not: the seat's device is gone, so the
 * press is a takeover, never a join. The candidate must be armed, playable and
 * bound to no player. */
bool input_seat_takeover_eligible(const struct input_owner* owners,
								  size_t owner_count, unsigned vacant_seat,
								  struct input_device candidate, bool armed,
								  enum input_category category, bool confirm);
/* Record a takeover silently: `to` is now `player`'s device and `from` is no
 * longer anybody's. No "connected"/"reconnected" card is raised for either
 * device, the taking pad's DETECTED / join-offer card is retired, and both
 * devices are disarmed so the confirming press (and anything the old pad
 * still holds) needs a release before it can claim again. */
void input_inventory_takeover(struct input_inventory* inventory,
							  struct input_device from,
							  struct input_device to, unsigned player,
							  ptime_t now);

#endif
