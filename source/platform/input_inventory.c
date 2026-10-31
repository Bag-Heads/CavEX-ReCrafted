/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include <stdio.h>
#include <string.h>

#include "input_inventory.h"

static int device_index(struct input_device device) {
	if(device.channel >= 4)
		return -1;
	if(device.kind == INPUT_DEVICE_WPAD)
		return device.channel;
	if(device.kind == INPUT_DEVICE_GAMECUBE)
		return 4 + device.channel;
	return -1;
}

const struct input_device_status*
input_inventory_device(const struct input_inventory* inventory,
					   struct input_device device) {
	int index = device_index(device);
	return inventory && index >= 0 ? &inventory->devices[index] : NULL;
}

void input_inventory_interrupt(struct input_inventory* inventory,
							   struct input_device device) {
	int index = device_index(device);
	if(!inventory || index < 0)
		return;
	inventory->devices[index].initialized = false;
	inventory->devices[index].armed = false;
}

static bool identity_equal(struct input_identity a, struct input_identity b) {
	return a.connected == b.connected
		&& (!a.connected || a.category == b.category);
}

/* A family the player can actually play with. A bare Remote is inventory
 * state, never a playable family (input_owner_latch enforces the same rule on
 * the gameplay side). */
static bool category_usable(enum input_category category) {
	return category == INPUT_CAT_NUNCHUK
		|| category == INPUT_CAT_CLASSIC_CONTROLLER
		|| category == INPUT_CAT_GAMECUBE;
}

static bool identity_usable(struct input_identity identity) {
	return identity.connected && category_usable(identity.category);
}

static bool owner_lookup(const struct input_owner* owners, size_t count,
						 struct input_device device, unsigned* player) {
	for(size_t p = 0; owners && p < count; p++) {
		if(input_device_equal(owners[p].device, device)) {
			*player = (unsigned)p;
			return true;
		}
	}
	return false;
}

/* Take the next free (or expired) slot of the live set. One record per edge:
 * every publisher below goes through here exactly once. */
static struct input_connection_notice*
notice_publish(struct input_inventory* inventory, struct input_device device,
			   enum controller_event event, struct input_identity identity,
			   struct input_identity previous, bool assigned, unsigned player,
			   ptime_t now) {
	struct input_connection_notice* slot = NULL;
	struct input_connection_notice* oldest = &inventory->notices[0];
	for(size_t k = 0; k < INPUT_NOTICE_COUNT; k++) {
		struct input_connection_notice* notice = &inventory->notices[k];
		if(!notice->active || time_reached(now, notice->expires)) {
			slot = notice;
			break;
		}
		if(time_reached(oldest->expires, notice->expires))
			oldest = notice;
	}
	if(!slot) {
		slot = oldest;
		if(inventory->notices_dropped != UINT32_MAX)
			inventory->notices_dropped++;
	}
	*slot = (struct input_connection_notice) {
		.expires = time_add_ms(now, INPUT_NOTICE_MS),
		.kind = device.kind, .channel = device.channel,
		.player = assigned ? player : 0, .event = event,
		.category = identity.category, .previous_category = previous.category,
		.active = true, .assigned = assigned, .connected = identity.connected};
	return slot;
}

/* Retire a live notice of one kind for one device -- the approved behaviour
 * for a regained pointer is that the notice goes away, not that a second card
 * appears. */
static void notice_retire(struct input_inventory* inventory,
						  struct input_device device,
						  enum controller_event event, ptime_t now) {
	for(size_t k = 0; k < INPUT_NOTICE_COUNT; k++) {
		struct input_connection_notice* notice = &inventory->notices[k];
		if(notice->active && notice->event == event
		   && input_device_equal(input_notice_device(notice), device)
		   && !time_reached(now, notice->expires))
			notice->expires = now;
	}
}

static bool notice_live(const struct input_inventory* inventory,
						struct input_device device,
						enum controller_event event, ptime_t now) {
	for(size_t k = 0; k < INPUT_NOTICE_COUNT; k++) {
		const struct input_connection_notice* notice = &inventory->notices[k];
		if(notice->active && notice->event == event
		   && input_device_equal(input_notice_device(notice), device)
		   && !time_reached(now, notice->expires))
			return true;
	}
	return false;
}

static enum controller_event classify(const struct input_device_status* status,
									  struct input_identity identity,
									  bool assigned, bool identity_changed) {
	if(!identity.connected)
		return CONTROLLER_EVENT_DISCONNECTED;
	if(!identity_usable(identity))
		return CONTROLLER_EVENT_ATTACHMENT_MISSING;
	/* Usable, and the hardware itself did not change: this edge is the player
	 * assignment. Hardware identity and player assignment are different
	 * states (approved menus.md, "Controller events"). */
	if(!identity_changed)
		return CONTROLLER_EVENT_CONNECTED;
	if(status->published_usable)
		return CONTROLLER_EVENT_ATTACHMENT_CHANGED;
	if(status->ever_usable)
		return CONTROLLER_EVENT_RECONNECTED;
	return assigned ? CONTROLLER_EVENT_CONNECTED : CONTROLLER_EVENT_DETECTED;
}

void input_inventory_poll(struct input_inventory* inventory,
						  struct input_device device,
						  struct input_identity identity, bool valid,
						  bool fresh, bool neutral, ptime_t now,
						  const struct input_owner* owners,
						  size_t owner_count) {
	int index = device_index(device);
	if(!inventory || index < 0)
		return;
	struct input_device_status* status = &inventory->devices[index];
	if(!identity.connected)
		identity.category = INPUT_CAT_NONE;
	if(!status->initialized || !identity_equal(status->observed, identity)) {
		/* Disarm on the raw identity edge, before owner frames are published.
		 * A hostile extension-loss report is non-neutral and cannot re-arm here;
		 * a genuinely neutral report may arm for the next deliberate press. */
		status->observed = identity;
		status->deadline = time_add_ms(now, INPUT_IDENTITY_STABLE_MS);
		status->initialized = true;
		status->armed = false;
		status->identity_reported = false;
	}
	if(fresh && valid)
		status->identity_reported = true;
	bool playable = identity_usable(identity) && valid;
	if(!playable)
		status->armed = false;
	else if(fresh && neutral)
		status->armed = true;

	unsigned player = 0;
	bool assigned = owner_lookup(owners, owner_count, device, &player);
	bool identity_changed = !identity_equal(status->published, identity);
	/* An assignment GAIN on unchanged usable hardware is its own edge ("Player
	 * 2 Connected"). Losing an assignment is recorded silently: unbinding a
	 * player is not a controller event. */
	bool assignment_gained = identity_usable(identity)
		&& (!status->published_assigned
			|| (assigned && player != status->published_player))
		&& assigned;
	if((!identity_changed && !assignment_gained)
	   || !time_reached(now, status->deadline)
	   || (identity.connected && (!valid || !status->identity_reported))) {
		if(!identity_changed && !assigned)
			status->published_assigned = false;
		return;
	}

	enum controller_event event
		= classify(status, identity, assigned, identity_changed);
	notice_publish(inventory, device, event, identity, status->published,
				   assigned, player, now);
	status->last_event = event;
	status->published = identity;
	status->published_usable = identity_usable(identity);
	status->ever_usable |= status->published_usable;
	status->published_assigned = assigned;
	status->published_player = player;

	if(event == CONTROLLER_EVENT_DETECTED
	   && inventory->join.state == INPUT_JOIN_IDLE) {
		inventory->join.state = INPUT_JOIN_OFFERED;
		inventory->join.device = device;
	} else if(!identity_usable(identity)
			  && input_device_equal(inventory->join.device, device)) {
		/* The offered hardware went away: the offer, and any failure card
		 * still standing for it, go with it. */
		inventory->join.state = INPUT_JOIN_IDLE;
		inventory->join.device = (struct input_device) {0};
	}
	if(!identity.connected) {
		status->battery_known = status->battery_low
			= status->battery_pending_low = false;
		status->battery_rearm = true;
		status->pointer_known = status->pointer_valid
			= status->pointer_pending_valid = false;
	}
}

bool input_p1_takeover_eligible(const struct input_owner* owners,
								size_t owner_count, bool split_join_armed,
								struct input_device candidate, bool armed,
								enum input_category category, bool confirm) {
	if(!owners || owner_count < 1 || split_join_armed || !confirm || !armed
	   || !category_usable(category))
		return false;
	if(owners[0].device.kind == INPUT_DEVICE_NONE
	   || input_device_equal(owners[0].device, candidate))
		return false;
	for(size_t p = 1; p < owner_count; p++)
		if(owners[p].device.kind != INPUT_DEVICE_NONE)
			return false; /* a bound P2: never steal P1 */
	return device_index(candidate) >= 0;
}

unsigned input_inventory_vacant_seat(const struct input_inventory* inventory,
									 const struct input_owner* owners,
									 size_t owner_count) {
	unsigned seat = 0, gone = 0;
	for(size_t p = 0; inventory && owners && p < owner_count; p++) {
		int index = device_index(owners[p].device);
		if(index < 0)
			continue;
		const struct input_device_status* s = &inventory->devices[index];
		if(s->ever_usable && !s->published.connected) {
			seat = (unsigned)p + 1;
			gone++;
		}
	}
	return gone == 1 ? seat : 0;
}

bool input_seat_takeover_eligible(const struct input_owner* owners,
								  size_t owner_count, unsigned vacant_seat,
								  struct input_device candidate, bool armed,
								  enum input_category category, bool confirm) {
	if(!owners || !vacant_seat || vacant_seat > owner_count || !confirm
	   || !armed || !category_usable(category))
		return false;
	if(owners[vacant_seat - 1].device.kind == INPUT_DEVICE_NONE)
		return false;
	for(size_t p = 0; p < owner_count; p++)
		if(input_device_equal(owners[p].device, candidate))
			return false; /* never steal (or re-take) a bound device */
	return device_index(candidate) >= 0;
}

void input_inventory_takeover(struct input_inventory* inventory,
							  struct input_device from,
							  struct input_device to, unsigned player,
							  ptime_t now) {
	if(!inventory)
		return;
	int ti = device_index(to), fi = device_index(from);
	if(ti >= 0) {
		struct input_device_status* t = &inventory->devices[ti];
		/* Already "assigned to this player" as far as publication goes, so
		 * the next poll sees no assignment edge and raises no card. */
		t->published_assigned = true;
		t->published_player = player;
		t->armed = false;
		notice_retire(inventory, to, CONTROLLER_EVENT_DETECTED, now);
		notice_retire(inventory, to, CONTROLLER_EVENT_JOIN_OFFERED, now);
		if(input_device_equal(inventory->join.device, to)) {
			inventory->join.state = INPUT_JOIN_IDLE;
			inventory->join.device = (struct input_device) {0};
		}
	}
	if(fi >= 0 && fi != ti) {
		struct input_device_status* f = &inventory->devices[fi];
		f->published_assigned = false;
		f->armed = false;
	}
}

void input_inventory_battery(struct input_inventory* inventory,
							 struct input_device device, bool supported,
							 unsigned percent, ptime_t now,
							 const struct input_owner* owners,
							 size_t owner_count) {
	int index = device_index(device);
	if(!inventory || index < 0)
		return;
	struct input_device_status* status = &inventory->devices[index];
	if(!supported) {
		/* Wired hardware, or a wireless device that has never reported: no
		 * episode can be running, and a replacement re-arms the cue. */
		if(status->battery_known || status->battery_low)
			status->battery_rearm = true;
		status->battery_known = status->battery_low
			= status->battery_pending_low = false;
		return;
	}
	/* Hysteresis: once low, only a reading at or above the RECOVER threshold
	 * counts as good, so a pack sitting on the boundary cannot produce a
	 * second episode. */
	bool low = status->battery_low ? percent < INPUT_BATTERY_RECOVER_PERCENT :
									 percent <= INPUT_BATTERY_LOW_PERCENT;
	if(!status->battery_known) {
		status->battery_known = true;
		status->battery_pending_low = low;
		status->battery_deadline = time_add_ms(now, INPUT_BATTERY_STABLE_MS);
		status->battery_low = false;
		if(!low)
			return;
	} else if(low != status->battery_pending_low) {
		status->battery_pending_low = low;
		status->battery_deadline = time_add_ms(now, INPUT_BATTERY_STABLE_MS);
		return;
	}
	if(low == status->battery_low || !time_reached(now, status->battery_deadline))
		return;
	status->battery_low = low;
	if(!low) {
		/* Stable recovery. Silent and card-free; it only re-arms the cue. */
		status->battery_rearm = true;
		return;
	}
	unsigned player = 0;
	bool assigned = owner_lookup(owners, owner_count, device, &player);
	notice_publish(inventory, device, CONTROLLER_EVENT_LOW_BATTERY,
				   status->published, status->published, assigned, player, now);
	status->last_event = CONTROLLER_EVENT_LOW_BATTERY;
}

void input_inventory_pointer(struct input_inventory* inventory,
							 struct input_device device, bool supported,
							 bool valid, ptime_t now,
							 const struct input_owner* owners,
							 size_t owner_count) {
	int index = device_index(device);
	if(!inventory || index < 0)
		return;
	struct input_device_status* status = &inventory->devices[index];
	if(!supported) {
		status->pointer_known = status->pointer_valid
			= status->pointer_pending_valid = false;
		return;
	}
	if(!status->pointer_known) {
		/* Adopted without an event: a pointer never aimed at the sensor bar
		 * has not been lost. */
		status->pointer_known = true;
		status->pointer_valid = status->pointer_pending_valid = valid;
		status->pointer_deadline = time_add_ms(now, INPUT_POINTER_STABLE_MS);
		return;
	}
	if(valid != status->pointer_pending_valid) {
		status->pointer_pending_valid = valid;
		status->pointer_deadline = time_add_ms(
			now, valid ? INPUT_POINTER_STABLE_MS : INPUT_POINTER_LOST_STABLE_MS);
		return;
	}
	if(valid == status->pointer_valid
	   || !time_reached(now, status->pointer_deadline))
		return;
	status->pointer_valid = valid;
	if(valid) {
		status->last_event = CONTROLLER_EVENT_POINTER_REGAINED;
		notice_retire(inventory, device, CONTROLLER_EVENT_POINTER_LOST, now);
		return;
	}
	/* One live notice per device, and a cooldown between repeats. */
	if(notice_live(inventory, device, CONTROLLER_EVENT_POINTER_LOST, now)
	   || (status->pointer_cooldown_armed
		   && !time_reached(now, status->pointer_cooldown_until)))
		return;
	status->pointer_cooldown_armed = true;
	status->pointer_cooldown_until
		= time_add_ms(now, INPUT_POINTER_NOTICE_COOLDOWN_MS);
	unsigned player = 0;
	bool assigned = owner_lookup(owners, owner_count, device, &player);
	notice_publish(inventory, device, CONTROLLER_EVENT_POINTER_LOST,
				   status->published, status->published, assigned, player, now);
	status->last_event = CONTROLLER_EVENT_POINTER_LOST;
}

bool input_inventory_join_request(struct input_inventory* inventory,
								  struct input_device device, ptime_t now) {
	if(!inventory || inventory->join.state != INPUT_JOIN_OFFERED
	   || !input_device_equal(inventory->join.device, device))
		return false;
	inventory->join.state = INPUT_JOIN_JOINING;
	notice_publish(inventory, device, CONTROLLER_EVENT_JOIN_OFFERED,
				   (struct input_identity) {0}, (struct input_identity) {0},
				   false, 0, now);
	return true;
}

void input_inventory_join_settle(struct input_inventory* inventory,
								 bool admitted, ptime_t now) {
	if(!inventory || inventory->join.state != INPUT_JOIN_JOINING)
		return;
	struct input_device device = inventory->join.device;
	if(admitted) {
		/* The real admission path publishes CONNECTED through the identity
		 * poll once the owner is bound; nothing to announce here. */
		inventory->join.state = INPUT_JOIN_IDLE;
		inventory->join.device = (struct input_device) {0};
		return;
	}
	inventory->join.state = INPUT_JOIN_FAILED;
	notice_retire(inventory, device, CONTROLLER_EVENT_JOIN_OFFERED, now);
	notice_publish(inventory, device, CONTROLLER_EVENT_JOIN_FAILED,
				   (struct input_identity) {0}, (struct input_identity) {0},
				   false, 0, now);
}

void input_inventory_join_cancel(struct input_inventory* inventory,
								 ptime_t now) {
	if(!inventory
	   || (inventory->join.state != INPUT_JOIN_JOINING
		   && inventory->join.state != INPUT_JOIN_FAILED))
		return;
	struct input_device device = inventory->join.device;
	inventory->join.state = INPUT_JOIN_OFFERED;
	notice_retire(inventory, device, CONTROLLER_EVENT_JOIN_OFFERED, now);
	notice_retire(inventory, device, CONTROLLER_EVENT_JOIN_FAILED, now);
	notice_publish(inventory, device, CONTROLLER_EVENT_JOIN_CANCELLED,
				   (struct input_identity) {0}, (struct input_identity) {0},
				   false, 0, now);
}

bool input_notice_visible(const struct input_connection_notice* notice,
						  unsigned player, ptime_t now) {
	return notice && notice->active && notice->player == player
		&& !time_reached(now, notice->expires);
}

const char* controller_family_name(enum input_category category) {
	switch(category) {
		case INPUT_CAT_GAMECUBE: return "GameCube controller";
		case INPUT_CAT_CLASSIC_CONTROLLER: return "Classic Controller";
		case INPUT_CAT_NUNCHUK: return "Wii Remote + Nunchuk";
		default: return "Wii Remote";
	}
}

enum controller_picture controller_picture_of(struct input_device device,
											  enum input_category category) {
	if(device.kind == INPUT_DEVICE_GAMECUBE
	   || category == INPUT_CAT_GAMECUBE)
		return CONTROLLER_PICTURE_GAMECUBE;
	if(category == INPUT_CAT_CLASSIC_CONTROLLER)
		return CONTROLLER_PICTURE_CLASSIC;
	if(category == INPUT_CAT_NUNCHUK)
		return CONTROLLER_PICTURE_NUNCHUK;
	return CONTROLLER_PICTURE_REMOTE;
}

/* Content half of the approved card. Geometry is added by
 * controller_card_build; input_notice_text reuses just the text so there is
 * ONE wording authority.
 *
 * Deviation (documented in docs/wii_controller_playability.md): the approved
 * prototype's U+00B7 middle dot and U+2026 ellipsis are written here as ASCII
 * " - " and "..." because the shipped Beta font atlas has no glyph for
 * either. */
static void controller_card_fill(const struct input_connection_notice* notice,
								 bool split, struct controller_card* out) {
	memset(out, 0, sizeof(*out));
	out->event = notice->event;
	out->duration_ms = INPUT_NOTICE_MS;
	enum input_category family = notice->connected ?
		notice->category :
		notice->previous_category;
	out->picture = controller_picture_of(input_notice_device(notice), family);
	switch(notice->event) {
		case CONTROLLER_EVENT_DETECTED:
		case CONTROLLER_EVENT_JOIN_CANCELLED:
			/* Newly detected hardware never takes an occupied slot: the card
			 * offers a join, it does not perform one. */
			if(split) {
				snprintf(out->text, sizeof(out->text), "No free player slot");
			} else {
				snprintf(out->text, sizeof(out->text), "New controller");
				snprintf(out->action_confirm, sizeof(out->action_confirm),
						 "Join P2");
			}
			break;
		case CONTROLLER_EVENT_CONNECTED:
			snprintf(out->text, sizeof(out->text), "Player %u Connected",
					 notice->player + 1);
			break;
		case CONTROLLER_EVENT_RECONNECTED:
			if(notice->assigned)
				snprintf(out->text, sizeof(out->text), "Player %u reconnected",
						 notice->player + 1);
			else
				snprintf(out->text, sizeof(out->text),
						 "Controller reconnected");
			break;
		case CONTROLLER_EVENT_DISCONNECTED:
			if(notice->assigned)
				snprintf(out->text, sizeof(out->text), "Player %u disconnected",
						 notice->player + 1);
			else
				snprintf(out->text, sizeof(out->text),
						 "Controller disconnected");
			break;
		case CONTROLLER_EVENT_ATTACHMENT_CHANGED:
			snprintf(out->text, sizeof(out->text), "Controller changed");
			break;
		case CONTROLLER_EVENT_ATTACHMENT_MISSING:
			snprintf(out->text, sizeof(out->text), "Attach Nunchuk or Classic");
			out->picture = CONTROLLER_PICTURE_REMOTE;
			break;
		case CONTROLLER_EVENT_LOW_BATTERY:
			out->picture = CONTROLLER_PICTURE_BATTERY;
			if(notice->assigned)
				snprintf(out->text, sizeof(out->text), "Player %u - Battery low",
						 notice->player + 1);
			else
				snprintf(out->text, sizeof(out->text), "Battery low");
			break;
		case CONTROLLER_EVENT_POINTER_LOST:
			/* Stick navigation stays available, so the card says so. */
			out->picture = CONTROLLER_PICTURE_NONE;
			snprintf(out->text, sizeof(out->text),
					 "Pointer out of view - Use stick");
			break;
		case CONTROLLER_EVENT_JOIN_OFFERED:
			snprintf(out->text, sizeof(out->text), "Joining Player 2...");
			snprintf(out->action_cancel, sizeof(out->action_cancel), "Cancel");
			break;
		case CONTROLLER_EVENT_JOIN_FAILED:
			snprintf(out->text, sizeof(out->text), "Could not join");
			snprintf(out->action_confirm, sizeof(out->action_confirm), "Retry");
			snprintf(out->action_cancel, sizeof(out->action_cancel), "Back");
			break;
		default: break;
	}
}

void input_notice_text(const struct input_connection_notice* notice, char* text,
					   size_t size) {
	struct controller_card card;
	if(!notice || !text || !size)
		return;
	controller_card_fill(notice, false, &card);
	snprintf(text, size, "%s", card.text);
}

bool input_notice_origin(const struct input_connection_notice* notice,
						 unsigned player, ptime_t now, int x, int y, int width,
						 int height, int* out_x, int* out_y) {
	if(!input_notice_visible(notice, player, now) || width <= 16 || height <= 16
	   || !out_x || !out_y)
		return false;
	*out_x = x + 8;
	*out_y = y + 8;
	return true;
}

void controller_card_name_join_target(struct controller_card* card,
									  const struct input_inventory* inventory) {
	if(!card || !inventory)
		return;
	bool join = !strcmp(card->action_confirm, "Join P2");
	/* A vacant seat (its bound device published gone) is what A takes, split
	 * or not -- including the split pane's "No free player slot" wording,
	 * which is false while a seat stands empty. */
	if(inventory->vacant_seat
	   && (join || !strcmp(card->text, "No free player slot"))) {
		snprintf(card->text, sizeof(card->text), "New controller");
		snprintf(card->action_confirm, sizeof(card->action_confirm),
				 "Play as P%u", (unsigned)inventory->vacant_seat);
		return;
	}
	if(inventory->split_join_armed || !join)
		return;
	snprintf(card->action_confirm, sizeof(card->action_confirm), "Play as P1");
}

bool controller_card_build(const struct input_connection_notice* notice,
						   unsigned player, bool split, ptime_t now, int x,
						   int y, int width, int height,
						   struct controller_card* out) {
	int px, py;
	if(!out
	   || !input_notice_origin(notice, player, now, x, y, width, height, &px,
							   &py))
		return false;
	controller_card_fill(notice, split, out);
	if(!out->text[0])
		return false;
	out->x = px;
	out->y = py;
	return true;
}
