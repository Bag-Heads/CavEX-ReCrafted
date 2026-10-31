/* Explicit controller ownership for local players. No active-player global. */
#ifndef INPUT_OWNER_H
#define INPUT_OWNER_H

#include <math.h>
#include <stddef.h>
#include "input.h"

#define INPUT_LOCAL_PLAYERS 2
#define INPUT_BINDINGS_PER_ACTION 12

enum input_device_kind { INPUT_DEVICE_NONE, INPUT_DEVICE_KEYBOARD,
	INPUT_DEVICE_WPAD, INPUT_DEVICE_GAMECUBE };
struct input_device { enum input_device_kind kind; unsigned channel; };

struct input_player_frame {
	bool connected;
	/* Prompt adapter only: no device owns the primary player's controls yet. */
	bool join_prompt;
	enum input_category category;
	bool down[IB_COUNT], pressed[IB_COUNT], released[IB_COUNT];
	/* Atlas cells/chords encoded by input_glyph.h; never infer another owner.
	 */
	int glyphs[IB_COUNT];
	/* WPAD uses normalized coordinates remapped into its owner's viewport.
	 * A mouse uses screen pixels and must already lie inside that viewport. */
	bool pointer_valid, pointer_normalized;
	float pointer_x, pointer_y, pointer_angle;
	float look_x, look_y;
};
struct input_owner {
	struct input_device device;
	struct input_player_frame frame;
	bool blocked[IB_COUNT];
	bool look_blocked;
};

static inline bool input_device_equal(struct input_device a,
	struct input_device b) {
	return a.kind == b.kind && a.channel == b.channel;
}

static inline bool input_owner_bind(struct input_owner* owners, size_t count,
	size_t player, struct input_device device) {
	if(!owners || player >= count || device.kind <= INPUT_DEVICE_NONE
	   || device.kind > INPUT_DEVICE_GAMECUBE || device.channel >= 4
	   || (device.kind == INPUT_DEVICE_KEYBOARD && device.channel != 0))
		return false;
	for(size_t k = 0; k < count; k++)
		if(k != player && input_device_equal(owners[k].device, device))
			return false;
	if(input_device_equal(owners[player].device, device)) return true;
	owners[player] = (struct input_owner){.device = device};
	owners[player].look_blocked = true;
	for(int b = 0; b < IB_COUNT; b++) {
		owners[player].blocked[b] = true;
		owners[player].frame.glyphs[b] = -1;
	}
	return true;
}

/* Called once per hardware poll. Attach, reconnect and expansion changes
 * require release before an action can fire. Disconnect never fires Drop,
 * Close, a bow-release action, or another synthetic button edge. */
static inline void input_owner_latch(struct input_owner* owner,
	const struct input_player_frame* sample) {
	struct input_player_frame next = *sample;
	/* A bare Remote is inventory state only, never a gameplay family. */
	if(owner->device.kind == INPUT_DEVICE_WPAD
	   && sample->category == INPUT_CAT_WIIMOTE)
		next.connected = false;
	sample = &next;
	bool changed = !owner->frame.connected
		|| owner->frame.category != sample->category;
	if(changed || !sample->connected) owner->look_blocked = true;
	if(sample->connected && isfinite(sample->look_x) && isfinite(sample->look_y)
	   && fabsf(sample->look_x) <= .2F && fabsf(sample->look_y) <= .2F)
		owner->look_blocked = false;
	if(owner->look_blocked || !isfinite(next.look_x) || !isfinite(next.look_y))
		next.look_x = next.look_y = 0;
	for(int b = 0; b < IB_COUNT; b++) {
		if(changed || !sample->connected) owner->blocked[b] = true;
		if(sample->connected && !sample->down[b]) owner->blocked[b] = false;
		next.down[b] = sample->connected && sample->down[b] && !owner->blocked[b];
		next.pressed[b] = next.down[b] && !owner->frame.down[b];
		next.released[b] = sample->connected && !changed
			&& !next.down[b] && owner->frame.down[b];
	}
	next.pointer_valid = sample->connected && sample->pointer_valid
		&& sample->category != INPUT_CAT_CLASSIC_CONTROLLER
		&& isfinite(sample->pointer_x) && isfinite(sample->pointer_y)
		&& isfinite(sample->pointer_angle);
	if(!sample->connected) {
		next.category = owner->frame.category;
		for(int b = 0; b < IB_COUNT; b++) next.glyphs[b] = owner->frame.glyphs[b];
	}
	owner->frame = next;
}

static inline bool input_owner_pointer(const struct input_player_frame* frame,
	int x, int y, int width, int height, float* px, float* py, float* angle) {
	*px = *py = *angle = 0;
	if(!frame || !frame->connected || !frame->pointer_valid
	   || width <= 0 || height <= 0) return false;
	float local_x = frame->pointer_normalized ? frame->pointer_x * width
		: frame->pointer_x - x;
	float local_y = frame->pointer_normalized ? frame->pointer_y * height
		: frame->pointer_y - y;
	if(!isfinite(local_x) || !isfinite(local_y)
	   || !isfinite(frame->pointer_angle) || local_x < 0 || local_y < 0
	   || local_x >= width || local_y >= height) return false;
	*px = local_x; *py = local_y; *angle = frame->pointer_angle;
	return true;
}

typedef bool (*input_key_reader)(void* user, int key);

/* Filter the configured alternatives before reading keys OR choosing glyphs.
 * In particular, Classic buttons share a WPAD bus with the remote but are a
 * different control scheme. A connected GC pad never changes a WPAD owner's
 * prompts. Keyboard codes belong to a separate platform namespace. */
static inline bool input_owner_accepts(enum input_category category, int key) {
	if(category == INPUT_CAT_GAMECUBE)
		return (key >= 400 && key <= 411) || (key >= 930 && key <= 933)
			|| (key >= 940 && key <= 943);
	if(category == INPUT_CAT_CLASSIC_CONTROLLER)
		return (key >= 200 && key <= 214) || (key >= 910 && key <= 913)
			|| (key >= 920 && key <= 923);
	if(category == INPUT_CAT_NUNCHUK)
		return (key >= 0 && key <= 10) || key == 100 || key == 101
			|| (key >= 900 && key <= 903);
	return category == INPUT_CAT_WIIMOTE && key >= 0 && key <= 10;
}

/* Apply the existing chord/OR semantics to one assigned device. Unlike the
 * legacy path's 16-entry consumed list, this covers every configured chord.
 * Resolve all keys through one device reader; a chord cannot span players. */
static inline void input_binding_levels(
	int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION], const size_t* lengths,
	input_key_reader read, void* user, bool* levels) {
	bool consumed[1008] = {false};
	for(int b = 0; b < IB_COUNT; b++) {
		for(size_t k = 0; k < lengths[b] && k < INPUT_BINDINGS_PER_ACTION; k++) {
			int key = map[b][k];
			if(key >= 1008 && key / 1000 < 1008
			   && read(user, key / 1000) && read(user, key % 1000)) {
				consumed[key / 1000] = true;
				consumed[key % 1000] = true;
			}
		}
	}
	for(int b = 0; b < IB_COUNT; b++) {
		levels[b] = false;
		for(size_t k = 0; k < lengths[b] && k < INPUT_BINDINGS_PER_ACTION
			&& !levels[b]; k++) {
			int key = map[b][k];
			if(key >= 1008) {
				levels[b] = read(user, key / 1000) && read(user, key % 1000);
			} else {
				levels[b] = key >= 0 && key < 1008
					&& !consumed[key] && read(user, key);
			}
		}
	}
}

/* ORDER-SENSITIVE chord gate (b1-chat-opener-0919 slice 1, Astra ruling 3 D2:
 * "F3 remains specifically hold Minus, then press Plus, once per chord").
 *
 * input_binding_levels above is deliberately left alone: it is pure, it has no
 * memory, and every OTHER chord in the shipped config -- Classic 212213,
 * GameCube 409410 -- keeps its order-free simultaneously-held semantics
 * byte-for-byte. This little machine is layered on TOP of it by input.c, and
 * only for the one chord whose two components are also the family's
 * scroll_left / scroll_right pair (the codes that carry both the hotbar and
 * the chord). Everything else never reaches it.
 *
 * `hold` is the component that must ALREADY be down; `press` is the one whose
 * fresh press decides the chord. Both arriving on one poll is not "already
 * down" -- there is no observed order there, so it refuses. After firing, or
 * after a refusal, BOTH components must be seen up again before the next
 * chord can fire, so a held partner can never produce a second toggle. */
enum { INPUT_CHORD_NEUTRAL = 0, INPUT_CHORD_ACTIVE, INPUT_CHORD_SPENT };

struct input_chord_order {
	unsigned char state;
	bool hold_down, press_down; /* the previous poll's physical levels */
};

/* Returns the chord's LEVEL for this poll: true only while a correctly
 * ordered chord is still fully held. input_owner_latch turns that into the
 * single pressed edge the overlay toggles on. */
static inline bool input_chord_order_step(struct input_chord_order* c,
	bool hold, bool press) {
	bool press_fresh = press && !c->press_down;
	if(c->state == INPUT_CHORD_ACTIVE && (!hold || !press))
		c->state = INPUT_CHORD_SPENT;
	if(c->state == INPUT_CHORD_SPENT && !hold && !press)
		c->state = INPUT_CHORD_NEUTRAL;
	if(c->state == INPUT_CHORD_NEUTRAL && press_fresh)
		c->state = (hold && c->hold_down) ? INPUT_CHORD_ACTIVE
										  : INPUT_CHORD_SPENT;
	c->hold_down = hold;
	c->press_down = press;
	return c->state == INPUT_CHORD_ACTIVE;
}

/* Assignment is explicit and exclusive. No automatic reassignment on unplug.
 * Existing single-player input remains the default until a host binds devices.
 * input_poll updates every assigned owner once; frame reads are immutable. */
bool input_player_bind(unsigned player, struct input_device device);
void input_player_unbind(unsigned player);
const struct input_player_frame* input_player_frame(unsigned player);
/* The owned device itself, for callers that need the debounced hardware state
 * behind the frame. False when the player owns nothing. */
bool input_player_device(unsigned player, struct input_device* device);
/* Consume one action for this player until its keys are seen released. */
void input_player_block(unsigned player, enum input_button button);

#endif
