/* Local guest lifetime and per-world persistence. GPL-3.0-or-later.
 * Server-thread API; no UI join action or second world simulation is enabled.
 */
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "server_local_guest.h"
#include "split_offer_piston_probe.h"
#include "vehicle_smoke_probe.h" /* host_vehicle_smoke_8m producer sites */
#include "server_transfer.h" /* A2-T: struct server_actor_transition, server_transfer_active */
#include "client_world_payload.h" /* clin_world_payloads_enabled: the guest dwell gate */
#include "client_admission.h" /* CLIN_CAP_*: the Join capacity round trip */
#include "server_local_state.h"
#include "server_local.h"
#include "server_world.h"
#include "level_archive.h"
#include "server_interface.h"
#include "client_interface.h"
#include "inventory_logic.h"
#include "window_registry.h"
#include "../item/items.h"
#include "level_template.h"
#include "../entity/combat.h"
#include "../game/achievements/game_event.h"

#define GUEST_TAG "CaveWiiGuest"

/* Allocate before replacing. Takes ownership of value only on success. */
static bool guest_set_node(nbt_node* parent, const char* name, nbt_node* value) {
	if(!parent || parent->type != TAG_COMPOUND || !parent->payload.tag_compound
	   || !value) return false;
	char* copy = malloc(strlen(name) + 1);
	if(!copy) return false;
	memcpy(copy, name, strlen(name) + 1);
	struct nbt_list* found = NULL;
	struct list_head* current;
	list_for_each(current, &parent->payload.tag_compound->entry) {
		struct nbt_list* entry = list_entry(current, struct nbt_list, entry);
		if(entry->data && entry->data->name && !strcmp(entry->data->name, name)) {
			if(found) { free(copy); return false; } /* ambiguous archive */
			found = entry;
		}
	}
	if(!found) {
		found = calloc(1, sizeof(*found));
		if(!found) { free(copy); return false; }
		list_add_tail(&found->entry, &parent->payload.tag_compound->entry);
	}
	free(value->name);
	value->name = copy;
	nbt_free(found->data);
	found->data = value;
	return true;
}

/* Private menu state only: 4 base crafting inputs, base cursor, active
 * cursor, 9 workbench inputs. Output slots are derived, never saved as loot.
 * Each stack is id:u16, damage:u16, count:u8, big endian; no C struct padding.
 * Container storage remains owned by the world archive. */
#define GUEST_PRIVATE_STACKS 15
#define GUEST_PRIVATE_BYTES (GUEST_PRIVATE_STACKS * 5)
_Static_assert(INVENTORY_SIZE_CRAFTING == 4 && CRAFTING_SIZE_INPUT == 9,
	"guest private archive layout must be versioned before crafting sizes change");

static void guest_discard_reply(const struct client_rpc* call, void* user) {
	(void)call; (void)user;
}

static bool guest_put(nbt_node* parent, const char* name, nbt_node* value) {
	nbt_node* copy = nbt_clone(value);
	if(copy && guest_set_node(parent, name, copy)) return true;
	nbt_free(copy);
	return false;
}

static bool guest_private_write(nbt_node* tag, struct server_player* p) {
	struct inventory* active = p->active_inventory;
	const struct window_desc* desc = active == &p->inventory ? NULL
		: window_registry_find(active->logic);
	/* Entity window identity needs its persistent entity key, not runtime id.
	 * An orphaned chest still owns world loot; never mislabel it player loot. */
	if(active != &p->inventory && (!desc || desc->key != WINDOW_KEY_BLOCK
	   || active->capacity != desc->capacity || active->invalidated)) return false;
	struct item_data stacks[GUEST_PRIVATE_STACKS] = {0};
	memcpy(stacks, p->inventory.items + INVENTORY_SLOT_CRAFTING,
		INVENTORY_SIZE_CRAFTING * sizeof(*stacks));
	stacks[4] = p->inventory.picked_item;
	if(desc) {
		stacks[5] = active->picked_item;
		if(active->logic == &inventory_logic_crafting)
			memcpy(stacks + 6, active->items + CRAFTING_SLOT_INPUT,
				CRAFTING_SIZE_INPUT * sizeof(*stacks));
	}
	unsigned char bytes[GUEST_PRIVATE_BYTES];
	for(size_t k = 0; k < GUEST_PRIVATE_STACKS; k++) {
		struct item_data item = stacks[k];
		if(item.id && (!item.count || !item_get(&item))) return false;
		if(!item.id) item = (struct item_data){0};
		bytes[k*5] = item.id >> 8; bytes[k*5+1] = item.id;
		bytes[k*5+2] = item.durability >> 8; bytes[k*5+3] = item.durability;
		bytes[k*5+4] = item.count;
	}
	int32_t window[8] = {0};
	if(desc) {
		window[0] = desc->windowc; window[1] = desc->wire_type;
		window[2] = active->x; window[3] = active->y; window[4] = active->z;
		window[5] = active->x2; window[6] = active->y2; window[7] = active->z2;
	}
	/* [740c583 NBTBase.createTagOfType] Beta supports tag IDs 0..10,
	 * so use byte arrays rather than the newer TAG_INT_ARRAY (11). */
	unsigned char window_bytes[32];
	for(size_t k = 0; k < 8; k++) {
		uint32_t value = (uint32_t)window[k];
		for(size_t j = 0; j < 4; j++) window_bytes[k*4+j] = value >> (24-j*8);
	}
	return guest_put(tag, "CaveWiiPrivate", &(nbt_node){.type=TAG_BYTE_ARRAY,
		.payload.tag_byte_array={.data=bytes,.length=sizeof(bytes)}})
		&& guest_put(tag, "CaveWiiWindow", &(nbt_node){.type=TAG_BYTE_ARRAY,
			.payload.tag_byte_array={.data=window_bytes,.length=sizeof(window_bytes)}});
}

static void guest_recompute_crafting(struct inventory* inv,
	struct server_player* p, size_t input) {
	set_inv_slot_t changes;
	set_inv_slot_init(changes);
	inv->logic->post_action(inv, p, input, false, false, changes);
	set_inv_slot_clear(changes);
}

/* Decode the CaveWiiWindow record's window identity. *out = NULL (and true)
 * for "no window"; false for a record no block window can have written. */
static bool guest_window_desc(const int32_t w[8], const struct window_desc** out) {
	*out = NULL;
	if(!w[0]) {
		for(size_t k = 1; k < 8; k++) if(w[k]) return false;
		return true;
	}
	if(w[0] < 0 || w[0] > UINT8_MAX) return false;
	const struct window_desc* desc = window_registry_by_windowc((uint8_t)w[0]);
	if(w[0] == WINDOWC_IRON_CHEST && w[1] == WINDOW_TYPE_CHEST)
		desc = window_registry_find(&inventory_logic_double_chest);
	if(!desc || desc->key != WINDOW_KEY_BLOCK || (int)desc->wire_type != w[1])
		return false;
	*out = desc;
	return true;
}

/* C1 (split-dimensions lane C -> A handoff): the world a restored guest
 * window belongs to. The body is admitted only into the host's live
 * dimension (the A1d row #27 refusal in guest_create), and once bound
 * server_local_world_of resolves the guest to that same context. The body is
 * NOT bound yet while its private state is restored, so resolve through the
 * host actor and require the dimensions to agree; NULL = no live committed
 * world (release -> load window), which refuses the restore. */
static struct server_world* guest_committed_world(struct server_local* s,
	const struct server_player* p) {
	/* A2-W (A2T-needs-from-B-C #2): membership is per actor, so the body's
	 * own committed slot answers -- the host-dimension-equality stand-in is
	 * retired. A body still being CREATED (guest_create restores its window
	 * before the body becomes s->guest_actor; the C1 test seam passes a
	 * caller-owned body) has no slot yet: its committed world is the live
	 * world of its own dimension (the read-only lookup, never an admission).
	 * The dimension cross-check stays. */
	if(!s || !p) return NULL;
	struct server_world* w = (p == &s->player || p == s->guest_actor)
		? server_local_world_of(s, p)
		: server_local_world_for_dimension(s, p->dimension);
	return w && w->dimension == p->dimension ? w : NULL;
}

/* Create the checkpointed block window for `p` (unpublished; the caller
 * attaches it). Like server_local_open_inventory_in, the window is bound to
 * its world BEFORE on_create runs, so the backing lookup inside on_create
 * resolves through the binding (never the unbound slot-0 fallback); a failed
 * create restores the prior binding. */
static bool guest_restore_window(struct server_local* s, struct server_player* p,
	const struct window_desc* desc, const int32_t w[8], struct inventory** out) {
	struct server_world* world = guest_committed_world(s, p);
	struct server_world_handle binding = server_world_handle_of(world);
	if(!world || !binding.generation) return false;
	struct inventory* active = malloc(sizeof(*active));
	if(!active) return false;
	struct server_world_handle prior = p->window_world;
	p->window_world = binding;
	bool ok = desc->linked
		? inventory_create_linked(active, (struct inventory_logic*)desc->logic,
			s, p, desc->capacity, w[2],w[3],w[4],w[5],w[6],w[7])
		: inventory_create(active, (struct inventory_logic*)desc->logic,
			s, p, desc->capacity, w[2],w[3],w[4]);
	if(!ok) { p->window_world = prior; free(active); return false; }
	*out = active;
	return true;
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Test-only seam (tests/test_c1_guest_window_bind.c): the restore step alone,
 * from a decoded CaveWiiWindow record, for a caller-owned player. */
bool server_local_guest_test_restore_window(struct server_local* s,
	struct server_player* p, const int32_t window[8], struct inventory** out);
bool server_local_guest_test_restore_window(struct server_local* s,
	struct server_player* p, const int32_t window[8], struct inventory** out) {
	if(out) *out = NULL;
	const struct window_desc* desc = NULL;
	if(!s || !p || !window || !out || !guest_window_desc(window, &desc) || !desc)
		return false;
	return guest_restore_window(s, p, desc, window, out);
}
#endif

/* Restoration happens before binding or publishing: no replies may escape a
 * failed admission. Existing on_create callbacks supply CURRENT world storage
 * of the guest's committed world (guest_restore_window binds it first). */
static bool guest_private_read(nbt_node* saved, struct server_local* s,
	struct server_player* p) {
	nbt_node* bytes = nbt_find_by_path(saved, GUEST_TAG ".CaveWiiPrivate");
	nbt_node* window = nbt_find_by_path(saved, GUEST_TAG ".CaveWiiWindow");
	nbt_node* schema = nbt_find_by_path(saved, GUEST_TAG ".CaveWiiGuestSchema");
	if(!bytes && !window && !schema) return true; /* prior settled-only schema */
	if(!schema || schema->type != TAG_INT || schema->payload.tag_int != 1) return false;
	/* [740c583 Entity.writeToNBT/readFromNBT] Fire/Air are shorts and
	 * OnGround is a byte. Restore these rather than resetting danger on join. */
	nbt_node* fire = nbt_find_by_path(saved, GUEST_TAG ".Fire");
	nbt_node* air = nbt_find_by_path(saved, GUEST_TAG ".Air");
	nbt_node* ground = nbt_find_by_path(saved, GUEST_TAG ".OnGround");
	if(!fire || fire->type != TAG_SHORT || !air || air->type != TAG_SHORT
	   || !ground || ground->type != TAG_BYTE
	   || (ground->payload.tag_byte != 0 && ground->payload.tag_byte != 1)) return false;
	p->fire = fire->payload.tag_short;
	p->oxygen = air->payload.tag_short;
	p->on_ground = ground->payload.tag_byte != 0;
	if(!bytes || bytes->type != TAG_BYTE_ARRAY
	   || bytes->payload.tag_byte_array.length != GUEST_PRIVATE_BYTES
	   || !bytes->payload.tag_byte_array.data || !window
	   || window->type != TAG_BYTE_ARRAY || window->payload.tag_byte_array.length != 32
	   || !window->payload.tag_byte_array.data) return false;
	struct item_data stacks[GUEST_PRIVATE_STACKS] = {0};
	const unsigned char* data = bytes->payload.tag_byte_array.data;
	for(size_t k = 0; k < GUEST_PRIVATE_STACKS; k++) {
		stacks[k] = (struct item_data){.id=(data[k*5]<<8)|data[k*5+1],
			.durability=(data[k*5+2]<<8)|data[k*5+3],.count=data[k*5+4]};
		if(stacks[k].id ? (!stacks[k].count || !item_get(&stacks[k]))
			: (stacks[k].count || stacks[k].durability)) return false;
	}
	int32_t w[8];
	const unsigned char* wd = window->payload.tag_byte_array.data;
	for(size_t k = 0; k < 8; k++) {
		uint32_t value = 0;
		for(size_t j = 0; j < 4; j++) value = (value << 8) | wd[k*4+j];
		memcpy(&w[k], &value, sizeof(value));
	}
	const struct window_desc* desc = NULL;
	if(!guest_window_desc(w, &desc)) return false;
	if(!desc && stacks[5].id) return false;
	if(!desc || desc->logic != &inventory_logic_crafting)
		for(size_t k = 6; k < GUEST_PRIVATE_STACKS; k++) if(stacks[k].id) return false;
	memcpy(p->inventory.items + INVENTORY_SLOT_CRAFTING, stacks,
		INVENTORY_SIZE_CRAFTING * sizeof(*stacks));
	p->inventory.picked_item = stacks[4];
	guest_recompute_crafting(&p->inventory, p, INVENTORY_SLOT_CRAFTING);
	if(!desc) return true;
	struct inventory* active = NULL;
	if(!guest_restore_window(s, p, desc, w, &active)) return false;
	active->reply = guest_discard_reply;
	p->active_inventory = active;
	active->picked_item = stacks[5];
	if(desc->logic == &inventory_logic_crafting) {
		memcpy(active->items + CRAFTING_SLOT_INPUT, stacks + 6,
			CRAFTING_SIZE_INPUT * sizeof(*stacks));
		guest_recompute_crafting(active, p, CRAFTING_SLOT_INPUT);
	}
	return true;
}

static nbt_node* guest_player_tree(nbt_node* saved) {
	nbt_node* tree = nbt_parse_compressed(LEVEL_DAT_TEMPLATE, sizeof(LEVEL_DAT_TEMPLATE));
	if(!tree) return NULL;
	if(saved) {
		nbt_node* copy = nbt_clone(saved);
		if(!copy || !guest_set_node(nbt_find_by_path(tree, ".Data"), "Player", copy)) {
			nbt_free(copy); nbt_free(tree); return NULL;
		}
	}
	return tree;
}

static void guest_reply(const struct client_rpc* call, void* user) {
	struct server_local* server = user;
	/* The queue copies generation now; it never reads this pointer at drain. */
	(void)clin_rpc_send_local(call, 1, server->guest_generation);
}

bool server_local_guest_settled(const struct server_local* server) {
	if(!server) return false;
	if(!server->guest_actor) return true;
	const struct server_player* p = server->guest_actor;
	if(p->active_inventory != &p->inventory || p->inventory.picked_item.id)
		return false;
	for(size_t k = INVENTORY_SLOT_CRAFTING;
		k < INVENTORY_SLOT_CRAFTING + INVENTORY_SIZE_CRAFTING; k++)
		if(p->inventory.items[k].id) return false;
	/* Riding/fishing/sleep/death ownership needs the gameplay lifecycle first. */
	return !p->riding_minecart && !p->riding_boat && !p->fish_entity_id
		&& !p->sleeping && p->health > 0;
}

/* A2-T guest admission (Astra a2t-0927 ruling: APPROVED): the guest joins the
 * world its saved Dimension names -- Beta ServerConfigurationManager.
 * playerLoggedIn:118-127 / EntityPlayer.readEntityFromNBT:312 -- including the
 * Overworld while the host is in the Nether; the record is never rewritten to
 * match the host. The host's own world answers when the dimensions agree;
 * otherwise server_local_world_admit brings the saved dimension's world LIVE
 * (EMPTY -> LOADING -> LIVE), reserving its budget FIRST: an over-budget
 * admission refuses visibly (its HUD toast) with the record and the host
 * untouched. Login never searches for or builds a portal (world_admit only
 * creates the world; the portal search is the TRAVEL transaction's). *admitted
 * reports that this call brought the world up, so a later refusal can release
 * it again. NULL = not admissible now; nothing has been written. */
static struct server_world* guest_admission_world(struct server_local* s,
	int32_t saved_dimension, bool* admitted) {
	if(admitted) *admitted = false;
	if(!s || (saved_dimension != WORLD_DIM_OVERWORLD
			  && saved_dimension != WORLD_DIM_NETHER))
		return NULL;
	struct server_world* host = server_local_world_of(s, &s->player);
	if(!host) return NULL; /* the session's routed world is not live */
	if(host->dimension == saved_dimension) return host;
	struct server_world* live
		= server_local_world_for_dimension(s, (enum world_dim)saved_dimension);
	if(live && server_local_world_live(live)) return live;
	struct server_world* w
		= server_local_world_admit(s, (enum world_dim)saved_dimension);
	if(w && admitted) *admitted = true;
	return w;
}

/* `dimension_refused` (A1d / C2 row #27) is RETIRED with the A2-T admission
 * (a record saved in the other dimension is admitted into that world now); the
 * out-parameter stays for the callers' ABI and is always false. */
static void guest_respawn_cleanup_tick(struct server_local*);

static bool guest_create(struct server_local* s, struct server_rpc_source* source,
	bool* dimension_refused) {
	if(dimension_refused) *dimension_refused = false;
	struct server_world* admission = NULL;
	bool admitted_world = false;
	if(s) guest_respawn_cleanup_tick(s);
	if(!s || !source || s->guest_actor || s->guest_owned || !s->level.data
	   || s->guest_respawn_cap.cleanup_cancel
	   || s->guest_respawn_cap.cleanup_ticket
	   || s->guest_respawn_cap.timed_out_id
	   || !s->player.has_pos || !s->player.finished_loading || s->input_ack_pending
	   || s->world_switch_phase != SERVER_WORLD_SWITCH_IDLE) return false;
	nbt_node* saved = nbt_find_by_path(s->level.data, ".Data." GUEST_TAG);
	if(saved && saved->type != TAG_COMPOUND) return false;
	struct level_archive view = {.data = guest_player_tree(saved)};
	if(!view.data) return false;
	struct server_player* p = calloc(1, sizeof(*p));
	if(!p) { nbt_free(view.data); return false; }
	if(!inventory_init(&p->inventory, &inventory_logic_player, s, p,
		INVENTORY_SIZE, 0, 0, 0)) { free(p); nbt_free(view.data); return false; }
	p->active_inventory = &p->inventory;
	p->inventory.reply = guest_discard_reply;
	p->inventory.reply_user = s;
	p->spawn_x = s->player.spawn_x; p->spawn_y = s->player.spawn_y; p->spawn_z = s->player.spawn_z;
	/* [740c583 EntityPlayer.<init>] health=20 (scaled by eight to 160 here), timeUntilPortal=20.
	 * Other transient baselines match the primary actor's initializer. */
	p->health = MAX_PLAYER_HEALTH;
	p->oxygen = MAX_OXYGEN;
	p->fire = -PLAYER_FIRE_RESISTANCE;
	p->time_until_portal = 20;
	p->next_step_distance = 1;
	p->was_in_water = true;
	/* residency-1005/2: a staged guest body starts with an empty residency
	 * record (counters included: they are this body generation's). */
	memset(&p->rz, 0, sizeof p->rz);
	p->x = s->player.x; p->y = s->player.y; p->z = s->player.z;
	p->rx = s->player.rx; p->ry = s->player.ry; p->dimension = s->player.dimension;
	bool ok = true;
	if(saved) {
		vec3 pos; vec2 rot;
		int32_t dimension;
		ok = level_archive_read(&view, LEVEL_PLAYER_DIMENSION, &dimension, 0)
			&& (dimension == 0 || dimension == -1)
			&& level_archive_read_player(&view, pos, rot, NULL, &p->dimension)
			&& level_archive_read_inventory(&view, &p->inventory)
			/* PLAYER-HEALTH-NBT: guest records are CaveWii-only (CaveWiiHotbar
			 * is mandatory below), so a marker-less one holds pre-fix units. */
			&& level_archive_read_player_health_strict(&view, true, &p->health)
			/* A saved zero is a dead body with a death/respawn pane, not a
			 * fresh healthy player. A negative value is corruption. */
			&& p->health >= 0 && p->health <= MAX_PLAYER_HEALTH
			&& level_archive_read(&view, LEVEL_PLAYER_FALL_DISTANCE, &p->fall_distance, 0)
			&& isfinite(p->fall_distance) && p->fall_distance >= 0
			&& level_archive_read_player_spawn(&view, &p->has_bed_spawn,
				&p->bed_spawn_x, &p->bed_spawn_y, &p->bed_spawn_z);
		nbt_node* hotbar = nbt_find_by_path(view.data, ".Data.Player.CaveWiiHotbar");
		ok = ok && hotbar && hotbar->type == TAG_INT
			&& hotbar->payload.tag_int >= 0 && hotbar->payload.tag_int < INVENTORY_SIZE_HOTBAR;
		if(ok) {
			p->x = pos[0]; p->y = pos[1]; p->z = pos[2];
			p->rx = rot[0]; p->ry = rot[1];
			p->inventory.hotbar_slot = hotbar->payload.tag_int;
		}
		/* A2-T (Astra a2t-0927): Beta loads a player INTO the world its saved
		 * Dimension names and never rewrites it (EntityPlayer.
		 * readEntityFromNBT:312, WorldInfo.<init>:36-38, server
		 * ServerConfigurationManager.playerLoggedIn:118-127). The body is
		 * committed to that world's slot at its saved coordinates; the world
		 * is admitted LIVE first if it is not (guest_admission_world). A
		 * refusal is lossless: the archive node is only read, the staged body
		 * is freed whole, and a world this Join admitted is released again. */
		if(ok) {
			admission = guest_admission_world(s, p->dimension, &admitted_world);
			if(!admission) ok = false;
			else p->world_slot = admission->world_index;
		}
		if(ok) ok = guest_private_read(saved, s, p);
	} else {
		/* a first Join (no record): the host's own world, as before */
		admission = server_local_world_of(s, &s->player);
		if(admission) p->world_slot = admission->world_index;
	}
	nbt_free(view.data);
	/* The provider must finish client admission/terrain ownership separately. */
	p->has_pos = true;
	p->finished_loading = false;
	/* Bind requires the empty base view; staged private state is unpublished. */
	struct inventory* active = p->active_inventory;
	struct item_data cursor = p->inventory.picked_item;
	p->active_inventory = &p->inventory;
	p->inventory.picked_item = (struct item_data){0};
	if(ok && server_local_bind_guest(s, p, source))
		(void)server_local_guest_bind_seq_bump(); /* O07: a new binding */
	else {
		if(active != &p->inventory) inventory_destroy_owned(active, p);
		inventory_destroy(&p->inventory, p); free(p);
		/* a world this Join brought up, now with no member, is released */
		if(admitted_world && admission)
			(void)server_local_world_retire(s, admission);
		return false;
	}
	p->active_inventory = active;
	p->inventory.picked_item = cursor;
	p->inventory.reply = guest_reply;
	active->reply = guest_reply;
	active->reply_user = s;
	s->guest_owned = true;
	s->guest_ticket = 0;
	s->guest_snapshot_sent = s->guest_confirmed = false;
	memset(&s->guest_join_wait, 0, sizeof s->guest_join_wait); /* R2 */
	s->guest_accept_publish.pending = s->guest_respawn_publish.pending = false;
	return true;
}

bool server_local_guest_create(struct server_local* s, struct server_rpc_source* source) {
	return guest_create(s, source, NULL);
}

/* C3 #27 checkpoint adapter: build the guest's complete private player
 * record (EntityPlayer.writeEntityToNBT:326-336 plus CaveWii's private
 * window/hotbar state) WITHOUT installing it. *out receives a fresh tree the
 * caller owns; false = the body is not settled (refuse, nothing written).
 * This is the "host/guest private records" capture A2-S's checkpoint cut
 * consumes (checkpoint_begin's struct checkpoint_cut); today
 * server_local_guest_checkpoint installs it into level.data directly. */
/* A pose the guest record is written AT instead of the live body's: the
 * A2-T COMMIT marker names the staged destination before the body moves. */
struct guest_record_pose {
	enum world_dim dimension;
	double x, y, z;
	float rx, ry;
};

static bool guest_checkpoint_record_at(struct server_local* s,
	struct server_player* p, const struct guest_record_pose* at,
	nbt_node** out);

static bool guest_checkpoint_record(struct server_local* s,
	struct server_player* p, nbt_node** out) {
	return guest_checkpoint_record_at(s, p, NULL, out);
}

static bool guest_checkpoint_record_at(struct server_local* s,
	struct server_player* p, const struct guest_record_pose* at,
	nbt_node** out) {
	*out = NULL;
	/* S1-a3 (Astra call #8 Q2, "prove checkpoint/save integrity ... and
	 * zero-health persistence"): health ZERO is a defined, persistable state,
	 * not a refusal. Before this card a guest could not reach zero at all, so
	 * `health <= 0` here was an unreachable sanity guard; once zero became
	 * reachable it would have failed EVERY later save -- this call is the
	 * first line of server_local_write_level_state. The riding/fishing guards
	 * stay exactly as they are: they are real "unsettled ownership" refusals,
	 * and the zero-health cleanup in server_local.c clears both by the time
	 * the health write returns, so a save on the zero tick already passes
	 * them. A NEGATIVE health is still corruption and still refuses. */
	/* A1d (row #3): SLEEPING no longer refuses. Guest sleep is live, and a
	 * guest asleep alone keeps the night (the quorum) -- refusing here would
	 * block every host autosave until dawn. Beta saves a sleeper and wakes it
	 * at load (EntityPlayer.readEntityFromNBT:315-318 -> wakeUpPlayer(true,
	 * true, false), stepped beside the bed); the record below is written at
	 * exactly that post-load position, without touching the live body. */
	/* G8-VEHICLE-SEAMS-01 (Astra step-1 DoR): a MOUNTED guest saves as a
	 * detached snapshot -- Beta Entity.writeToNBT / EntityPlayer.
	 * writeEntityToNBT persist Pos/Motion/Rotation/.../Inventory/Dimension and
	 * no riding relation, so the record below never names the vehicle and the
	 * live mount is untouched. The blanket riding refusal is replaced by a
	 * validation: every riding flag must be backed by a live vehicle naming
	 * this lifetime, wire id and world; anything else is corruption. */
	if(!p->active_inventory || p->active_inventory->owner != p
	   || p->health < 0 || !server_local_actor_mounts_valid(s, p)
	   || p->fish_entity_id) return false;
	/* A2-T (A2T-needs-from-B-C C#2): the record names the guest's OWN
	 * dimension (Beta EntityPlayer.writeEntityToNBT:329 Dimension); the old
	 * "same dimension as the host" refusal made every save fail once the two
	 * bodies stood in different worlds. */
	if(p->inventory.hotbar_slot < 0 || p->inventory.hotbar_slot >= INVENTORY_SIZE_HOTBAR
	   || p->health > MAX_PLAYER_HEALTH
	   || (p->dimension != WORLD_DIM_OVERWORLD && p->dimension != WORLD_DIM_NETHER))
		return false;
	struct level_archive view = {.data = guest_player_tree(NULL)};
	if(!view.data) return false;
	struct item_data items[INVENTORY_SIZE];
	memcpy(items, p->inventory.items, sizeof(items));
	struct inventory snapshot = p->inventory;
	snapshot.items = items;
	if(p->active_inventory != &p->inventory) {
		const struct window_desc* desc = window_registry_find(p->active_inventory->logic);
		if(!desc || desc->capacity != p->active_inventory->capacity) {
			nbt_free(view.data); return false;
		}
		if(desc->main_slot >= 0 && desc->hotbar_slot >= 0) {
			memcpy(items + INVENTORY_SLOT_MAIN,
				p->active_inventory->items + desc->main_slot,
				INVENTORY_SIZE_MAIN * sizeof(*items));
			memcpy(items + INVENTORY_SLOT_HOTBAR,
				p->active_inventory->items + desc->hotbar_slot,
				INVENTORY_SIZE_HOTBAR * sizeof(*items));
		}
	}
	double save_x, save_y, save_z;
	float save_rx = p->rx, save_ry = p->ry;
	enum world_dim save_dim = p->dimension;
	if(at) {
		save_x = at->x; save_y = at->y; save_z = at->z;
		save_rx = at->rx; save_ry = at->ry;
		save_dim = at->dimension;
	} else if(!server_local_sleep_wake_position(s, p, &save_x, &save_y, &save_z)) {
		nbt_free(view.data); return false;
	}
	if(!isfinite(save_x) || !isfinite(save_y) || !isfinite(save_z)
	   || !isfinite(save_rx) || !isfinite(save_ry)) {
		nbt_free(view.data); return false; /* never persist a non-finite pose */
	}
	bool ok = level_archive_write_player(&view, (vec3){save_x,save_y,save_z},
		(vec2){save_rx,save_ry}, NULL, save_dim)
		&& level_archive_write_inventory(&view, &snapshot)
		&& level_archive_write_player_health(&view, p->health)
		&& level_archive_write_player_fall_distance(&view, p->fall_distance)
		&& level_archive_write_player_spawn(&view, p->has_bed_spawn,
			p->bed_spawn_x, p->bed_spawn_y, p->bed_spawn_z);
	nbt_node hotbar = {.type=TAG_INT, .payload.tag_int=p->inventory.hotbar_slot};
	nbt_node* hotbar_copy = ok ? nbt_clone(&hotbar) : NULL;
	nbt_node* player = nbt_find_by_path(view.data, ".Data.Player");
	if(!hotbar_copy || !guest_set_node(player, "CaveWiiHotbar", hotbar_copy)) {
		nbt_free(hotbar_copy); ok = false;
	}
	ok = ok && p->fire >= INT16_MIN && p->fire <= INT16_MAX
		&& p->oxygen >= INT16_MIN && p->oxygen <= INT16_MAX
		&& guest_put(player, "Fire", &(nbt_node){.type=TAG_SHORT,.payload.tag_short=p->fire})
		&& guest_put(player, "Air", &(nbt_node){.type=TAG_SHORT,.payload.tag_short=p->oxygen})
		&& guest_put(player, "OnGround", &(nbt_node){.type=TAG_BYTE,.payload.tag_byte=p->on_ground})
		&& guest_private_write(player, p)
		&& guest_put(player, "CaveWiiGuestSchema", &(nbt_node){.type=TAG_INT,.payload.tag_int=1});
	nbt_node* copy = ok ? nbt_clone(player) : NULL;
	nbt_free(view.data);
	if(!copy) return false;
	*out = copy;
	return true;
}

bool server_local_guest_checkpoint(struct server_local* s) {
	if(!s) return false;
	if(!s->guest_actor) return true;
	if(!s->guest_owned || !s->level.data) return false;
	/* An offered actor has no admitted actions. Do not make a cancelled Join
	 * create or replace the persistent guest record during host autosave. */
	if(s->guest_ticket && !s->guest_confirmed) return true;
	nbt_node* copy = NULL;
	if(!guest_checkpoint_record(s, s->guest_actor, &copy)) return false;
	/* Install: replace the whole record node, or keep the previous one. */
	if(!guest_set_node(nbt_find_by_path(s->level.data, ".Data"), GUEST_TAG, copy)) {
		nbt_free(copy); return false;
	}
	s->level.modified = true;
	return true;
}

static bool guest_snapshot_inventory_valid(const struct inventory* inv) {
	if(!inv || !inv->items || inv->invalidated) return false;
	for(size_t k = 0; k <= inv->capacity; k++) {
		struct item_data item = k == inv->capacity
			? inv->picked_item : inv->items[k];
		if(item.id ? (!item.count || !item_get(&item))
			: (item.count || item.durability)) return false;
	}
	return true;
}

/* Zero means invalid, not queue pressure. Validate ALL views before enqueue. */
static unsigned guest_snapshot_count(struct server_local* s,
	struct server_rpc_source source) {
	if(!s || !s->guest_owned || source.slot != 1 || !source.generation
	   || source.generation != s->guest_generation || !s->guest_actor) return 0;
	struct inventory* inv = &s->guest_actor->inventory;
	struct inventory* active = s->guest_actor->active_inventory;
	if(inv->capacity != INVENTORY_SIZE || inv->hotbar_slot < 0
	   || inv->hotbar_slot >= INVENTORY_SIZE_HOTBAR
	   || !guest_snapshot_inventory_valid(inv) || !active) return 0;
	if(active == inv) return 3;
	const struct window_desc* desc = window_registry_find(active->logic);
	if(!desc || active->capacity != desc->capacity
	   || active->capacity > CRPC_WINDOW_ITEMS_MAX_SLOTS
	   || !guest_snapshot_inventory_valid(active)) return 0;
	return 6;
}

bool server_local_guest_publish(struct server_local* s, struct server_rpc_source source) {
	unsigned count = guest_snapshot_count(s, source);
	if(!count || !clin_rpc_headroom(count)) return false;
	/* Local mode has ONE inbox producer, the server thread; the client only
	 * returns tokens. No inventory callbacks run in this check/send interval.
	 * clin_settle_rpc filters these types out; column_observer.publish returns
	 * immediately for non-CHUNK; test observers must also be observation-only.
	 * Thus no callback consumes headroom and each try-send is admitted. Remote
	 * mode's producer is mutually exclusive with this local server. This is
	 * enqueue admission, not an atomic application of the snapshot by clients. */
	struct inventory* inv = &s->guest_actor->inventory;
	struct client_rpc snapshot = {.type=CRPC_WINDOW_ITEMS,
		.payload.window_items={.window=WINDOWC_INVENTORY,.count=INVENTORY_SIZE}};
	memcpy(snapshot.payload.window_items.items, inv->items,
		INVENTORY_SIZE * sizeof(*inv->items));
	if(!clin_try_rpc_send_local(&snapshot, 1, source.generation)
	   || !clin_try_rpc_send_local(&(struct client_rpc){.type=CRPC_PLAYER_HOTBAR_SLOT,
		.payload.player_hotbar_slot.slot=inv->hotbar_slot}, 1, source.generation)
	   || !clin_try_rpc_send_local(&(struct client_rpc){.type=CRPC_INVENTORY_SLOT,
		.payload.inventory_slot={.window=WINDOWC_INVENTORY,
			.slot=SPECIAL_SLOT_PICKED_ITEM,.item=inv->picked_item}}, 1, source.generation))
		return false;
	struct inventory* active = s->guest_actor->active_inventory;
	if(active == inv) return true;
	const struct window_desc* desc = window_registry_find(active->logic);
	if(!clin_try_rpc_send_local(&(struct client_rpc){.type=CRPC_OPEN_WINDOW,
		.payload.window_open={.window=desc->windowc,.type=desc->wire_type,
			.slot_count=active->capacity}}, 1, source.generation)) return false;
	snapshot.payload.window_items.window = desc->windowc;
	snapshot.payload.window_items.count = active->capacity;
	memcpy(snapshot.payload.window_items.items, active->items,
		active->capacity * sizeof(*active->items));
	return clin_try_rpc_send_local(&snapshot, 1, source.generation)
		&& clin_try_rpc_send_local(&(struct client_rpc){.type=CRPC_INVENTORY_SLOT,
			.payload.inventory_slot={.window=desc->windowc,
				.slot=SPECIAL_SLOT_PICKED_ITEM,.item=active->picked_item}}, 1, source.generation);
}

static bool guest_join_send_capacity(struct server_local*, uint8_t, uint8_t,
	uint32_t, uint32_t);
static bool guest_respawn_release_ticket(uint32_t);

/* Retire views without invoking Close/drop. Their private contents are either
 * durable (leave) or unchanged from the pre-admission archive (cancel).
 * No actor pointer or destructor replies survive the lifetime boundary. */
static void guest_release(struct server_local* s) {
	struct server_player* p = s->guest_actor;
	if(s->guest_gameplay) game_event_emit(&(struct game_event){
		.kind = GE_SESSION_END, .authority = GE_AUTH_SERVER,
		.who = {.slot = 1, .generation = s->guest_generation}});
	s->guest_accept_publish.pending = s->guest_respawn_publish.pending = false;
	/* G8-VEHICLE-SEAMS-01: the committed boundary settles every vehicle
	 * relation naming this lifetime BEFORE the body/generation is
	 * invalidated, so no vehicle keeps a departed rider. */
	server_local_release_actor_mounts(s, p, NULL);
	/* The committed leave cancels this lifetime's queued movement before
	 * the body is invalidated/freed. No credit survives to re-admission. */
	server_local_actor_motion_reset(p);
	p->riding_boat = p->riding_minecart = false;
	if(s->guest_respawn_cap.landing_armed)
		s->guest_respawn_cap.cleanup_cancel = true;
	if(s->guest_respawn_cap.ticket)
		s->guest_respawn_cap.cleanup_ticket = s->guest_respawn_cap.ticket;
	if(s->guest_respawn_cap.pending) {
		s->guest_respawn_cap.timed_out_id = s->guest_respawn_cap.request_id;
		s->guest_respawn_cap.timed_out_slot
			= s->guest_respawn_target.world_index;
	}
	s->guest_respawn_cap.pending = s->guest_respawn_cap.reserved = false;
	s->guest_respawn_cap.committed = false;
	s->guest_respawn_cap.landing_armed = s->guest_respawn_cap.landing_ready = false;
	s->guest_respawn_cap.ticket = 0;
	s->guest_respawn_phase = GUEST_RESPAWN_IDLE;
	s->guest_respawn_epoch = s->guest_respawn_generation = 0;
	s->guest_respawn_anchor = false;
	s->guest_respawn_ready = false;
	guest_respawn_cleanup_tick(s);
	p->inventory.reply = guest_discard_reply;
	if(p->active_inventory != &p->inventory) {
		p->active_inventory->reply = guest_discard_reply;
		inventory_destroy_owned(p->active_inventory, p);
	}
	s->guest_actor = NULL;
	s->a2w_guest_presentation = false;
	s->guest_generation = s->guest_ticket = 0;
	s->guest_owned = s->guest_snapshot_sent = s->guest_confirmed = false;
	memset(&s->guest_join_wait, 0, sizeof s->guest_join_wait); /* R2 */
	/* Retire the gameplay body with the session: residency drops back to one
	 * anchor on the very next tick and no stale entity id can be reused. */
	s->guest_gameplay = false;
	s->guest_entity_id = 0;
	server_local_split_policy_apply(s); /* leave: radius and far view restored */
	/* ...and drop this body's residency debt with it. The refresh pass stops
	 * the moment two-anchor mode ends, so anchor-1 debt from a DEPARTED body
	 * would otherwise sit dormant in the scheduler and be inherited by the
	 * next admitted guest. */
	server_local_residency_release_anchor(s, 1);
	inventory_destroy(&p->inventory, p);
	free(p);
}

void server_local_guest_release_checkpointed(struct server_local* s) {
	assert(s && s->guest_owned && s->guest_actor);
	guest_release(s);
}

bool server_local_guest_leave(struct server_local* s, struct server_rpc_source source) {
	if(!s || !s->guest_owned || !s->guest_actor || source.slot != 1
	   || !source.generation || source.generation != s->guest_generation)
		return false;
	if(!server_local_checkpoint_guests(s)) return false;
	guest_release(s);
	return true;
}

static void guest_status(uint64_t ticket, uint64_t generation, int result) {
	clin_rpc_send(&(struct client_rpc){.type=CRPC_LOCAL_GUEST,
		.payload.local_guest={.ticket=ticket,.generation=generation,.result=result}});
}

static bool guest_publication_current(struct server_local* s, uint64_t ticket,
	uint64_t generation, struct server_world_handle world) {
	if(!s->guest_owned || !s->guest_actor || s->guest_ticket != ticket
	   || s->guest_generation != generation) return false;
	struct server_world_handle current = server_world_handle_of(
		server_local_world_of(s, s->guest_actor));
	return current.world_index == world.world_index
		&& current.generation == world.generation;
}

static void guest_accept_publish_poll(struct server_local* s) {
	/* server_local_init initializes the route; create initializes retries. */
	if(!s->guest_owned || !s->guest_actor) {
		s->guest_accept_publish.pending = false;
		return;
	}
	if(!s->guest_accept_publish.pending) return;
	if(!guest_publication_current(s, s->guest_accept_publish.ticket,
			s->guest_accept_publish.generation, s->guest_accept_publish.world_handle)
	   || s->guest_confirmed || s->transfer[ACTOR_SLOT_GUEST].open) {
		s->guest_accept_publish.pending = false;
		return;
	}
	/* Disclosed offered-piston fixture: retain the real ACCEPT's publication
 * work while holding the body offered. Release resumes this exact request;
 * no pose, relation, install or ACK is fabricated. */
	if(sop_probe_hold_offer()) { sop_probe_offer(s); return; }
	/* A host transfer cannot supersede this guest's ticket/body/world. Keep
	 * the accepted mailbox work until the shared transfer barrier clears. */
	if(server_transfer_active(s)) {
		if(vsp_armed())
			vsp_srv_publish_blocked(s, VSP_CAUSE_TRANSFER);
		return;
	}
	/* host_vehicle_smoke_8m: the profile-armed seeded ACCEPT-batch refusal,
	 * directly before batch admission/publish, scoped to the exact armed
	 * tuple (CONTRACT-v1 ACCEPT REFUSAL). Dormant: one relaxed load. */
	if(vsp_armed() && vsp_srv_refuse_publish(s))
		return;
	struct server_rpc_source source = {.slot = 1,
		.generation = s->guest_accept_publish.generation};
	if(!s->guest_accept_publish.result) {
		unsigned count = guest_snapshot_count(s, source);
		if(!count) s->guest_accept_publish.result = LOCAL_GUEST_REJECTED;
		else {
			/* Include INSTALLED in the admission; no blocking status send. */
			if(!clin_rpc_headroom(count + 1)
			   || !server_local_guest_publish(s, source)) return;
			s->guest_snapshot_sent = true;
			s->guest_accept_publish.result = LOCAL_GUEST_INSTALLED;
		}
	}
	struct client_rpc status = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.owner = 1, .ticket = s->guest_accept_publish.ticket,
			.generation = source.generation, .result = s->guest_accept_publish.result}};
	if(clin_try_rpc_send_local(&status, 1, source.generation)) {
		s->guest_accept_publish.pending = false;
		if(vsp_armed() && status.payload.local_guest.result == LOCAL_GUEST_INSTALLED)
			vsp_srv_installed(s);
	}
}

/* ===== A2-T guest JOIN client-capacity round trip (lane C) ====================
 * Astra a2t-0927: "Reserve aggregate MEM1/MEM2, residency, client/GPU ...
 * first; insufficient budget means visible refusal with records and host
 * state unchanged" and "publish membership only after that world is LIVE and
 * its landing/client capacity is ready". The server budget is reserved by
 * server_local_world_admit; this is the CLIENT half, the same wire the
 * transfer uses (CRPC_CAPACITY_REQUEST {RESERVE|COMMIT|RELEASE} ->
 * SRPC_CAPACITY_REPLY, lane B's client_admission.h answers it):
 *   - only a Join whose admission world is NOT the host's own world asks (a
 *     same-world Join needs no second client world -- unchanged);
 *   - RESERVE goes out stamped with that world, sized like the transfer's
 *     landing (split radius columns, SERVER_TRANSFER_MEM*_PER_COLUMN);
 *   - OK: the body is created (guest_create) and the reservation COMMITted;
 *     a create that fails RELEASEs it. DISABLED (world-bound payloads off:
 *     a single-world client) proceeds with nothing to commit, as the
 *     transfer does;
 *   - any refusal, or no answer within SERVER_TRANSFER_CLIENT_WAIT_TICKS,
 *     rejects the Join VISIBLY (host HUD) with nothing changed: no body, the
 *     record untouched, and a world this Join admitted is released. A late
 *     OK for a request that already timed out is RELEASEd at once. */
static uint32_t guest_join_cap_next_id(struct server_local* s) {
	if(++s->guest_join_cap.counter == 0 || s->guest_join_cap.counter > 0x7fffffffu)
		s->guest_join_cap.counter = 1;
	return GUEST_JOIN_CAP_ID_BIT | s->guest_join_cap.counter;
}

static bool guest_join_send_capacity(struct server_local* s, uint8_t slot,
	uint8_t op, uint32_t request_id, uint32_t cap_ticket) {
	struct server_world* w = &s->worlds[slot];
	int r = clin_world_payloads_enabled() ? server_local_view_radius(s)
		: (s->split_view_radius > 0 ? s->split_view_radius : 1);
	uint32_t columns = (uint32_t)(2 * r + 1) * (uint32_t)(2 * r + 1);
	uint32_t mem1 = columns * SERVER_TRANSFER_MEM1_PER_COLUMN;
	if(clin_world_payloads_enabled()
	   && !world_local_shared_capacity_mem1_need(columns, &mem1))
		return false;
	struct client_rpc rpc = {.type = CRPC_CAPACITY_REQUEST};
	rpc.payload.capacity_request.request_id = request_id;
	rpc.payload.capacity_request.op = op;
	rpc.payload.capacity_request.slot = slot;
	rpc.payload.capacity_request.mem1_bytes = mem1;
	rpc.payload.capacity_request.mem2_bytes = columns * SERVER_TRANSFER_MEM2_PER_COLUMN;
	rpc.payload.capacity_request.ticket = cap_ticket;
	return server_local_try_send_w(w, rpc);
}

/* The saved record's Dimension (the host's when there is no record); false for
 * a record whose Dimension is missing or not 0 / -1 (guest_create refuses it
 * losslessly on the ordinary path). */
static bool guest_saved_dimension(struct server_local* s, int32_t* out) {
	nbt_node* saved = nbt_find_by_path(s->level.data, ".Data." GUEST_TAG);
	if(!saved) {
		*out = s->player.dimension;
		return true;
	}
	nbt_node* d = nbt_find_by_path(s->level.data, ".Data." GUEST_TAG ".Dimension");
	if(!d || d->type != TAG_INT
	   || (d->payload.tag_int != 0 && d->payload.tag_int != -1))
		return false;
	*out = d->payload.tag_int;
	return true;
}

/* Only the latest remembered timeout and the Join already pending when
 * cleanup begins can produce recognized tickets. New Join requests wait on
 * known cleanup, so these two owners cannot be replaced by another request. */
static void guest_join_release_capacity(struct server_local* s, uint32_t ticket) {
	if(!ticket) return;
	for(unsigned i = 0; i < 2; i++)
		if(s->guest_join_cap.cleanup_ticket[i] == ticket) return;
	if(server_transfer_release_capacity(s, ticket)) return;
	for(unsigned i = 0; i < 2; i++)
		if(!s->guest_join_cap.cleanup_ticket[i]) {
			s->guest_join_cap.cleanup_ticket[i] = ticket;
			return;
		}
	assert(!"recognized Join cleanup owner bound exceeded");
}

static void guest_join_cap_fail(struct server_local* s, const char* why) {
	uint64_t ticket = s->guest_join_cap.join_ticket;
	uint8_t slot = s->guest_join_cap.slot;
	bool admitted = s->guest_join_cap.admitted_world;
	uint32_t counter = s->guest_join_cap.counter;
	uint32_t timed_out = s->guest_join_cap.timed_out_id;
	uint8_t timed_out_slot = s->guest_join_cap.timed_out_slot;
	uint32_t cleanup[2];
	memcpy(cleanup, s->guest_join_cap.cleanup_ticket, sizeof(cleanup));
	memset(&s->guest_join_cap, 0, sizeof(s->guest_join_cap));
	memcpy(s->guest_join_cap.cleanup_ticket, cleanup, sizeof(cleanup));
	s->guest_join_cap.counter = counter;
	s->guest_join_cap.timed_out_id = timed_out;
	s->guest_join_cap.timed_out_slot = timed_out_slot;
	fprintf(stderr, "[guest_join] client capacity %s: Join refused\n", why);
	server_local_send_hud_message(s, "Player 2's world cannot open now");
	if(admitted && slot < SERVER_LOCAL_WORLD_SLOTS)
		(void)server_local_world_retire(s, &s->worlds[slot]);
	guest_status(ticket, 0, LOCAL_GUEST_REJECTED);
}

/* true = the Join is now waiting on the client (the caller returns); false =
 * no round trip needed (same world) or the admission itself refused -- the
 * caller runs the ordinary path, whose guest_create repeats the admission
 * and answers it. */
static bool guest_join_begin_capacity(struct server_local* s, uint64_t ticket) {
	int32_t dim;
	if(!guest_saved_dimension(s, &dim))
		return false;
	struct server_world* host = server_local_world_of(s, &s->player);
	if(!host || host->dimension == dim)
		return false;
	bool admitted = false;
	struct server_world* w = guest_admission_world(s, dim, &admitted);
	if(!w || w == host)
		return false;
	uint32_t counter = s->guest_join_cap.counter;
	uint32_t timed_out = s->guest_join_cap.timed_out_id;
	uint8_t timed_out_slot = s->guest_join_cap.timed_out_slot;
	uint32_t cleanup[2];
	memcpy(cleanup, s->guest_join_cap.cleanup_ticket, sizeof(cleanup));
	memset(&s->guest_join_cap, 0, sizeof(s->guest_join_cap));
	memcpy(s->guest_join_cap.cleanup_ticket, cleanup, sizeof(cleanup));
	s->guest_join_cap.counter = counter;
	s->guest_join_cap.timed_out_id = timed_out;
	s->guest_join_cap.timed_out_slot = timed_out_slot;
	s->guest_join_cap.join_ticket = ticket;
	s->guest_join_cap.slot = w->world_index;
	s->guest_join_cap.admitted_world = admitted;
	s->guest_join_cap.request_id = guest_join_cap_next_id(s);
	s->guest_join_cap.pending = true;
	if(!guest_join_send_capacity(s, w->world_index, CLIN_CAP_OP_RESERVE,
								 s->guest_join_cap.request_id, 0)) {
		guest_join_cap_fail(s, "request could not be sent");
		return true;
	}
	return true;
}

static bool guest_respawn_capacity_reply(struct server_local*, uint32_t,
	uint8_t, uint32_t);
static void guest_respawn_preflight_abort(struct server_local*);

bool server_local_guest_on_capacity_reply(struct server_local* s,
	uint32_t request_id, uint8_t result, uint32_t ticket) {
	if(guest_respawn_capacity_reply(s, request_id, result, ticket))
		return true;
	if(!s || !(request_id & GUEST_JOIN_CAP_ID_BIT))
		return false;
	if(!s->guest_join_cap.pending || request_id != s->guest_join_cap.request_id) {
		/* Not the RESERVE being waited on: the COMMIT/RELEASE acks of a
		 * finished Join are consumed here. Only a late OK for the RESERVE
		 * that TIMED OUT is answered, by returning its reservation. */
		if(result == CLIN_CAP_OK && ticket && request_id == s->guest_join_cap.timed_out_id) {
			guest_join_release_capacity(s, ticket);
			s->guest_join_cap.timed_out_id = 0;
		}
		return true;
	}
	if(result != CLIN_CAP_OK && result != CLIN_CAP_DISABLED) {
		static const char* const names[] = {"ok", "disabled", "bad slot", "busy",
			"no client world context", "no MEM1", "no MEM2"};
		char why[64];
		snprintf(why, sizeof(why), "refused (%s)", result < 7 ? names[result] : "?");
		guest_join_cap_fail(s, why);
		return true;
	}
	uint64_t join_ticket = s->guest_join_cap.join_ticket;
	uint8_t slot = s->guest_join_cap.slot;
	bool admitted = s->guest_join_cap.admitted_world;
	uint32_t rid = s->guest_join_cap.request_id;
	s->guest_join_cap.pending = false;
	struct server_rpc_source guest;
	if(!guest_create(s, &guest, NULL)) {
		if(result == CLIN_CAP_OK)
			guest_join_release_capacity(s, ticket);
		if(admitted && slot < SERVER_LOCAL_WORLD_SLOTS
		   && !(s->guest_actor && s->guest_actor->world_slot == slot))
			(void)server_local_world_retire(s, &s->worlds[slot]);
		guest_status(join_ticket, 0, LOCAL_GUEST_REJECTED);
		return true;
	}
	if(result == CLIN_CAP_OK) {
		(void)guest_join_send_capacity(s, slot, CLIN_CAP_OP_COMMIT, rid, ticket);
		/* A saved-world Join owns the same world reservation as a transfer.
		 * Retain its ticket so departure, replacement and session teardown
		 * release it even if the COMMIT send was refused. */
		server_transfer_on_world_vacated(s, slot);
		s->transfer_slot_ticket[slot] = ticket;
	}
	s->guest_ticket = join_ticket;
	guest_status(join_ticket, guest.generation, LOCAL_GUEST_OFFER);
	return true;
}

static void guest_join_wait_tick(struct server_local* s);
void server_local_guest_join_capacity_tick(struct server_local* s) {
	if(!s) return;
	guest_accept_publish_poll(s);
	guest_join_wait_tick(s);
	for(unsigned i = 0; i < 2; i++)
		if(s->guest_join_cap.cleanup_ticket[i]
		   && server_transfer_release_capacity(s, s->guest_join_cap.cleanup_ticket[i]))
			s->guest_join_cap.cleanup_ticket[i] = 0;
	if(!s->guest_join_cap.pending) return;
	if(++s->guest_join_cap.wait_ticks > SERVER_TRANSFER_CLIENT_WAIT_TICKS) {
		uint32_t rid = s->guest_join_cap.request_id;
		uint8_t slot = s->guest_join_cap.slot;
		guest_join_cap_fail(s, "reply timed out");
		s->guest_join_cap.timed_out_id = rid;
		s->guest_join_cap.timed_out_slot = slot;
	}
}

/* ---- Gameplay admission (experimental local split-screen) ----------------
 * ATTACHED certifies window ownership only. This is the SEPARATE gate the
 * two-view renderer and the second residency anchor wait on: a body that the
 * server has placed on loaded terrain, in the host's dimension, with the
 * experimental switch on. Nothing here changes a Beta rule -- a second player
 * is additive; the world's simulation is untouched. */
static bool guest_terrain_ready(struct server_local* s, struct server_player* p) {
	/* Two block reads in one column proved almost nothing: they say the server
	 * holds those voxels, not that the CLIENT has them -- and the admission
	 * immediately hands the scheduler a full MAX_VIEW_DISTANCE ring around this
	 * body. Use the same predicate the host spawn path's collision-safety gate
	 * uses (`center_ready`, server_local.c:18024,
	 * `server_world_is_chunk_sent(&s->world, px, pz)`): the body's own column
	 * must have been PUBLISHED to the client before a body may stand in it. */
	w_coord_t feet = (w_coord_t)floor(p->y - 1.62);
	if(feet < 0 || feet + 1 >= WORLD_HEIGHT) return false;
	/* A1d (D8): the body's own committed live world; none -> not ready. */
	struct server_world* w = server_local_world_of(s, p);
	return w && server_world_is_chunk_sent(w,
		WCOORD_CHUNK_OFFSET((int)floor(p->x)),
		WCOORD_CHUNK_OFFSET((int)floor(p->z)));
}

/* residency-1005/6 R2 (ASTRA-RESIDENCY-FINAL-2): the saved-pose Join's
 * terrain wait is BOUNDED, on the guest-respawn LOADING pattern (efe432219):
 * the same SERVER_TRANSFER_CLIENT_WAIT_TICKS bound, counted in server
 * updates from the first GAMEPLAY denial at the terrain gate (the body's own
 * column unpublished), terminating at +601. At the bound:
 *   - the wait is over for this Join generation (`failed`): a GAMEPLAY that
 *     was already in flight, or any later one of this generation, is
 *     answered GAMEPLAY_DENIED and can never admit the body;
 *   - ownership the wait held is released: the body's residency state is
 *     retired and server_transfer_stream_step stops streaming a slot >= 1
 *     world on its behalf (server_local_guest_join_wait_failed);
 *   - the client gets LOCAL_GUEST_JOIN_LOADING_FAILED for this generation:
 *     a visible "could not join" failure whose retry is a deliberate re-Join
 *     (Leave, then Join -- the saved record is restored again; nothing was
 *     admitted, so nothing of this attempt is live).
 * Other denials (switch off, host respawn, world switch, input hand-off)
 * are not terrain waits and keep their existing contracts. */
static bool guest_join_terrain_pending(struct server_local* s) {
	struct server_player* p = s->guest_actor;
	return p && s->guest_owned && s->guest_confirmed && !s->guest_gameplay
		&& server_local_world_of(s, p) && !guest_terrain_ready(s, p);
}

static void guest_join_wait_tick(struct server_local* s) {
	if(!s->guest_join_wait.armed)
		return;
	if(!s->guest_actor || s->guest_gameplay || s->guest_join_wait.failed) {
		s->guest_join_wait.armed = false;
		return;
	}
	if(++s->guest_join_wait.wait_ticks <= SERVER_TRANSFER_CLIENT_WAIT_TICKS)
		return;
	s->guest_join_wait.armed = false;
	s->guest_join_wait.failed = true;
	server_local_rz_retire_body(s->guest_actor);
	fprintf(stderr, "[guest_join] terrain wait timed out after %u updates: "
		"Join failed (retry = re-Join)\n",
		(unsigned)s->guest_join_wait.wait_ticks);
	clin_rpc_send(&(struct client_rpc){.type=CRPC_LOCAL_GUEST,
		.payload.local_guest={.ticket=s->guest_ticket,
			.generation=s->guest_generation,.owner=1,
			.result=LOCAL_GUEST_JOIN_LOADING_FAILED}});
}

bool server_local_guest_join_wait_failed(const struct server_local* s) {
	return s && s->guest_actor && s->guest_join_wait.failed;
}

bool server_local_guest_gameplay(struct server_local* s,
	struct server_rpc_source source) {
	if(!s || !s->guest_owned || !s->guest_actor
	   || !s->guest_confirmed || !s->residency || source.slot != 1
	   || !source.generation || source.generation != s->guest_generation
	   || s->world_switch_phase != SERVER_WORLD_SWITCH_IDLE
	   || !s->player.has_pos || !s->player.finished_loading
	   || (s->input_ack_pending && !server_transfer_guest_continuity(s)))
		return false;
	struct server_player* p = s->guest_actor;
	/* An ALREADY-ADMITTED body answers its own retry whatever the switch says.
	 * The client re-asks on later frames while its terrain streams, so with the
	 * LIVE switch (Astra option A) a duplicate request can arrive just after a
	 * live OFF -- and answering GAMEPLAY_DENIED there would clear the client's
	 * local_gameplay_ready and freeze the second view while the server kept
	 * simulating that body. OFF blocks NEW admissions (the split_enabled gate
	 * below); it never ejects a guest already playing.
	 *
	 * Equivalence with the boot-only behaviour this replaces: before the live
	 * path existed, guest_gameplay could only be true while split_enabled was
	 * also true (server_local_set_split_enabled refuses to clear it under an
	 * admitted body), so no reachable outcome changes for the boot path. */
	if(s->guest_gameplay) return p->finished_loading; /* idempotent re-request */
	/* A new body admission would change the anchor set while owner 0's
	 * same-world respawn plan is in flight. An already-admitted survivor took
	 * the idempotent branch above and remains playable. */
	if(server_local_host_respawn_active(s)) return false;
	/* A NEW admission is what the switch actually gates. */
	if(!s->split_enabled) return false;
	/* A2-T: the body plays in its OWN committed world (LIVE, of its saved
	 * dimension) -- membership publishes only once that world is live and
	 * the body's column has been published to the client
	 * (guest_terrain_ready, the landing readiness), never by host equality. */
	struct server_world* own = server_local_world_of(s, p);
	if(p->health < 0 || p->sleeping || p->riding_minecart || p->riding_boat
	   || !own || own->dimension != p->dimension || !p->has_pos)
		return false;
	if(!level_player_position_values_valid(p->x, p->y, p->z)
	   || !level_player_rotation_values_valid(p->rx, p->ry))
		return false;
	if(!guest_terrain_ready(s, p)) return false;
	uint32_t id = server_local_gen_id(s);
	if(!id) return false;
	p->finished_loading = true;
	p->correction_seq = 0;
	p->distance_walked = 0;
	p->next_step_distance = 1;
	s->guest_entity_id = id;
	s->guest_gameplay = true;
	game_event_emit(&(struct game_event){.kind = GE_SESSION_BEGIN,
		.authority = GE_AUTH_SERVER,
		.who = {.slot = 1, .generation = s->guest_generation}});
	server_local_split_policy_apply(s); /* join: radius 4, far ring off */
	return true;
}

/* Author a fresh correction token and send the body back to its last ACCEPTED
 * position. correction_seq is the same monotonic-nonzero token the host uses
 * (server_local.c:8568 server_local_next_player_correction): every client
 * sample echoes the current value, so bumping it invalidates everything that
 * was already in flight, and the reply carries the new value the guest must
 * echo from now on. Without the bump the field was set once at admission and
 * never advanced -- body_sync correction was inert. */
static bool guest_correct_position_epoch(struct server_local* s,
	uint8_t correction_flags, uint64_t respawn_epoch, bool nonblocking) {
	struct server_player* p = s->guest_actor;
	uint32_t prior_seq = p->correction_seq;
	if(++p->correction_seq == 0)
		p->correction_seq = 1;
	struct client_rpc correction = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = s->guest_ticket,
			.generation = s->guest_generation,
			.owner = 1, .respawn_epoch = respawn_epoch,
			.result = LOCAL_GUEST_CORRECTION,
			.correction_seq = p->correction_seq,
			.correction_flags = correction_flags,
			.x = p->x, .y = p->y, .z = p->z, .rx = p->rx, .ry = p->ry}};
	bool sent = nonblocking
		? clin_try_rpc_send_local(&correction, 1, s->guest_generation)
		: clin_rpc_send_local(&correction, 1, s->guest_generation);
	if(!sent) {
		p->correction_seq = prior_seq;
		return false;
	}
	if(sop_probe_armed()) sop_probe_correction(true);
	return true;
}

static void guest_correct_position(struct server_local* s,
	uint8_t correction_flags) {
	(void)guest_correct_position_epoch(s, correction_flags, 0, false);
}

/* The guest body's ONLY movement route. It applies the shared footstep leg
 * (A1d row #16: odometer, step sound, Block.onEntityWalking -- the SAME
 * server_local_actor_movement_step the host handler calls) and the shared
 * vertical/fall sampler after accepting the owner-scoped pose, and stores the
 * rider steering source (row #7). Portal dwell stays host-only (A2 owns
 * guest travel). */
void server_local_guest_position(struct server_local* s,
	const struct server_rpc* call) {
	if(!s || !call || call->type != SRPC_PLAYER_POS)
		return;
	uint32_t rx_token = call->payload.player_pos.correction_seq;
	if(!s->guest_gameplay || !s->guest_actor
	   || !s->guest_actor->finished_loading) {
		server_local_portal_diag_guest_pos(s, SPD_POS_RESULT_NOT_READY,
			rx_token, 0.0f);
		return;
	}
	struct server_player* p = s->guest_actor;
	/* The transfer HOLD rejects source-body input. The later presentation
	 * hold is released only by a valid sample echoing the fresh COMMIT token
	 * installed with the client's accepted GAMEPLAY_READY body. */
	if(server_transfer_actor_frozen(s, p)) {
		server_local_portal_diag_guest_pos(s, SPD_POS_RESULT_FROZEN,
			rx_token, 0.0f);
		return;
	}
	/* split-dimensions-plan-0924/C2a, P2 inventory row #4: the host's
	 * pre-dispatch movement barriers, applied to the guest's only movement
	 * route. Beta EntityPlayer.isMovementBlocked (EntityPlayer.java:127-129)
	 * is `health <= 0 || isPlayerSleeping()`, and EntityLiving.onLivingUpdate
	 * (EntityLiving.java:618-622) zeroes moveStrafing/moveForward/isJumping
	 * while it holds. The host enforces that on the authority ahead of its
	 * SRPC switch (server_local.c, the barriers before
	 * process_recovery_window_close) with three pure classifiers, mirrored
	 * here one for one:
	 *   - sleeping: server_rpc_blocked_while_sleeping(SRPC_PLAYER_POS) is
	 *     TRUE -- the sample is DROPPED, no correction authored, the body
	 *     stays in bed and the client's token is untouched;
	 *   - dead: server_rpc_blocked_while_dead(SRPC_PLAYER_POS) is FALSE --
	 *     corpse physics (the fall after a lethal hit, the knockback slide)
	 *     still flow, exactly as Beta's moveEntity keeps running on a dead
	 *     body with zeroed input (deviation-#18 dwell). Deliberately NOT
	 *     blocked here either;
	 *   - respawn plan in flight: server_local_host_respawn_active (any
	 *     non-IDLE phase) blocks SRPC_PLAYER_POS. The guest's LOADING phase
	 *     already dropped samples through finished_loading=false +
	 *     reject_pos_this_update; PREPARED did not, so an old-body sample
	 *     could still move the corpse after the plan had recorded its death
	 *     position. Closed for every non-IDLE phase, like the host.
	 * The action verbs already pass through the same classifiers
	 * (server_local.c server_local_guest_action_allowed); this was the one
	 * dispatch that bypassed them. Pinned by
	 * tests/test_owner_swap_pose_dead_sleep_barrier.c. */
	if(p->sleeping) {
		server_local_portal_diag_guest_pos(s, SPD_POS_RESULT_SLEEP,
			rx_token, 0.0f);
		return;
	}
	if(s->guest_respawn_phase != GUEST_RESPAWN_IDLE) {
		server_local_portal_diag_guest_pos(s, SPD_POS_RESULT_RESPAWN,
			rx_token, 0.0f);
		return;
	}
	/* R-S1a3 F2: the host's rule, applied to the guest -- a SERVER-AUTHORED
	 * correction wins until the client acknowledges it. Both legs matter and
	 * both are the host's own (server_local.c SRPC_PLAYER_POS:
	 * `!s->player.reject_pos_this_update && ... correction_seq ==
	 * s->player.correction_seq`):
	 *   - the per-actor input barrier drops samples this body queued against a
	 *     state the world has since re-authored mid-update (the piston push
	 *     raises it through server_local_raise_actor_input_barriers, which has
	 *     covered the guest since R-S1a2 F2 but had no reader on this path),
	 *   - the token compare drops samples that echo a superseded correction,
	 *     which is what keeps a knockback shove (and its one-shot vertical
	 *     pop) from being overwritten by the sample the client authored before
	 *     it knew it had been hit. */
	if(p->reject_pos_this_update) {
		server_local_portal_diag_guest_pos(s, SPD_POS_RESULT_BARRIER,
			rx_token, 0.0f);
		return;
	}
	if(rx_token != p->correction_seq) {
		server_local_portal_diag_guest_pos(s, SPD_POS_RESULT_TOKEN,
			rx_token, 0.0f);
		return;
	}
	double nx = call->payload.player_pos.x;
	double ny = call->payload.player_pos.y;
	double nz = call->payload.player_pos.z;
	if(!level_player_position_values_valid(nx, ny, nz)
	   || !level_player_rotation_values_valid(call->payload.player_pos.rx,
											  call->payload.player_pos.ry)
	   || !level_player_velocity_values_valid(call->payload.player_pos.vel_x,
											  call->payload.player_pos.vel_y,
											  call->payload.player_pos.vel_z)) {
		server_local_portal_diag_guest_pos(s, SPD_POS_RESULT_INVALID,
			rx_token, 0.0f);
		return;
	}
	/* Per-sample delta clamp, the host's own bound reused verbatim
	 * (PLAYER_MOVE_TELEPORT_DELTA_SQ, server_local_state.h; applied on the host
	 * at server_local.c's SRPC_PLAYER_POS handler). The host only suppresses
	 * its odometer across such a jump because its actor is the one the client
	 * already owns; the guest body additionally OWNS RESIDENCY ANCHOR 1, so an
	 * arbitrary jump would drag a whole MAX_VIEW_DISTANCE streaming ring with
	 * it. Reject and correct instead. */
	/* G8-VEHICLE-SEAMS-01: a RIDING guest is measured from its vehicle, not
	 * from its last body sample. Beta NetServerHandler.handleFlying seats a
	 * rider from ridingEntity.updateRiderPosition() whatever the client sent;
	 * the seat is server authority, so a sample at the seat is legitimate even
	 * when the body record lags, and the clamp still refuses a jump away from
	 * the vehicle (the residency-anchor protection is unchanged). */
	const struct entity* seat = server_local_actor_vehicle(s, p);
	double ref_x = seat ? (double)seat->pos[0] : p->x;
	double ref_z = seat ? (double)seat->pos[2] : p->z;
	double ddx = nx - ref_x, ddz = nz - ref_z;
	if(ddx * ddx + ddz * ddz >= PLAYER_MOVE_TELEPORT_DELTA_SQ) {
		server_local_portal_diag_guest_pos(s, SPD_POS_RESULT_DELTA,
			rx_token, (float)(ddx * ddx + ddz * ddz));
		guest_correct_position(s, PLAYER_CORRECTION_POSITION_ONLY
			| PLAYER_CORRECTION_ZERO_VEL_X | PLAYER_CORRECTION_ZERO_VEL_Y
			| PLAYER_CORRECTION_ZERO_VEL_Z);
		return;
	}
	/* residency-1005: the host's residency admission, on the guest's own
	 * correction route -- refused BEFORE any footstep, fall or rider side
	 * effect when the body footprint touches a non-resident column of the
	 * guest's world (server_local_actor_admit_pose, server_local.c). */
	if(!server_local_actor_admit_pose(s, p, nx, nz)) {
		server_local_portal_diag_guest_pos(s, SPD_POS_RESULT_BARRIER,
			rx_token, (float)(ddx * ddx + ddz * ddz));
		return;
	}
	/* v5: room for this packet's server half, guaranteed by the admitted
	 * drain (ACTOR_MOTION_QUEUE); false is the counted invariant violation. */
	if(!server_local_actor_motion_reserve(s, p)) {
		server_local_portal_diag_guest_pos(s, SPD_POS_RESULT_BARRIER,
			rx_token, (float)(ddx * ddx + ddz * ddz));
		return;
	}
	/* Beta Entity.moveEntity's footstep leg [Entity.java:436-457] on the
	 * pre-sample body state, before the new position is stored -- the host
	 * handler's exact order. */
	/* G8-BLAST-VELOCITY v3: the accepted sample's server physics (Beta
	 * handleFlying), replayed at the tracker phase. */
	server_local_actor_motion_enqueue(s, p, nx, ny, nz,
		call->payload.player_pos.vel_x, call->payload.player_pos.vel_z,
		call->payload.player_pos.on_ground);
	server_local_actor_movement_step(s, p, nx, ny, nz);
	double previous_y = p->y;
	p->x = nx; p->y = ny; p->z = nz;
	p->rx = call->payload.player_pos.rx;
	p->ry = call->payload.player_pos.ry;
	if(!server_local_player_vertical_sample_actor(s, p, previous_y,
		call->payload.player_pos.vel_y, call->payload.player_pos.on_ground)) {
		server_local_portal_diag_guest_pos(s, SPD_POS_RESULT_VERTICAL,
			rx_token, (float)(ddx * ddx + ddz * ddz));
		return;
	}
	server_local_actor_motion_settle_for(p);
	p->on_ground = call->payload.player_pos.on_ground;
	p->sneaking = call->payload.player_pos.sneaking;
	/* Boat rider steering SOURCE (A1d row #7), stored unconditionally like
	 * the host's: entity_boat.c reads the RECORDED rider's copy only while
	 * the boat is occupied [EntityBoat.onUpdate:195-197]. */
	p->rider_vel_x = call->payload.player_pos.vel_x;
	p->rider_vel_z = call->payload.player_pos.vel_z;
	server_local_actor_pin_pose(s, p);
	/* v5: the host route's membership refresh, for the guest body. */
	server_local_refresh_guest_membership(s);
	s->a2w_guest_presentation = false;
	server_local_portal_diag_guest_pos(s, SPD_POS_RESULT_ACCEPT,
		rx_token, (float)(ddx * ddx + ddz * ddz));
}

static void guest_respawn_loading(struct server_local* s) {
	/* The target may differ from the still-dead actor's world. This handle
	 * identifies the client's terrain and entity store for the epoch ACK. */
	struct server_world* target = server_local_guest_respawn_target(s);
	if(!target)
		return;
	struct client_rpc reply = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = s->guest_ticket,
			.generation = s->guest_generation, .owner = 1,
			.respawn_epoch = s->guest_respawn_epoch,
			.respawn_world_slot = target->world_index,
			.respawn_world_generation = target->generation,
			.respawn_dimension = target->dimension,
			.result = LOCAL_GUEST_RESPAWN_LOADING,
			.x = s->guest_respawn_x, .y = s->guest_respawn_y,
			.z = s->guest_respawn_z}};
	(void)clin_rpc_send_local(&reply, 1, s->guest_generation);
}

struct server_world* server_local_guest_respawn_target(struct server_local* s) {
	if(!s || !s->guest_respawn_anchor || !s->guest_actor
	   || s->guest_respawn_generation != s->guest_generation)
		return NULL;
	if(s->guest_respawn_phase != GUEST_RESPAWN_LOADING
	   && !(s->guest_respawn_phase == GUEST_RESPAWN_PREPARED
		&& s->guest_respawn_cap.reserved
		&& s->guest_respawn_cap.landing_armed))
		return NULL;
	return server_local_world_from_handle(s, s->guest_respawn_target);
}

static void guest_respawn_failure(struct server_local* s,
	enum local_guest_result result, uint64_t epoch) {
	struct client_rpc reply = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = s->guest_ticket,
			.generation = s->guest_generation, .owner = 1,
			.respawn_epoch = epoch, .result = result}};
	(void)clin_rpc_send_local(&reply, 1, s->guest_generation);
}

static bool guest_respawn_landing_send(struct server_local* s, bool cancel) {
	struct server_world* target
		= server_local_world_from_handle(s, s->guest_respawn_target);
	if(!target) return false;
	struct client_rpc rpc = {.type = CRPC_LANDING_ARM};
	rpc.payload.landing_arm.token = s->guest_respawn_cap.request_id;
	rpc.payload.landing_arm.x = (int32_t)floor(s->guest_respawn_x);
	rpc.payload.landing_arm.z = (int32_t)floor(s->guest_respawn_z);
	rpc.payload.landing_arm.radius = SERVER_TRANSFER_LANDING_CORE_RADIUS;
	rpc.payload.landing_arm.cancel = cancel;
	rpc.payload.landing_arm.dimension = (int8_t)target->dimension;
	return server_local_try_send_w(target, rpc);
}

static bool guest_respawn_release_ticket(uint32_t ticket) {
	if(!ticket) return true;
	struct client_rpc rpc = {.type = CRPC_CAPACITY_REQUEST};
	rpc.payload.capacity_request.op = CLIN_CAP_OP_RELEASE;
	rpc.payload.capacity_request.ticket = ticket;
	return clin_try_rpc_send(&rpc);
}

static void guest_respawn_cleanup_tick(struct server_local* s) {
	if(s->guest_respawn_cap.cleanup_cancel) {
		if(!guest_respawn_landing_send(s, true)) return;
		s->guest_respawn_cap.cleanup_cancel = false;
	}
	if(s->guest_respawn_cap.cleanup_ticket) {
		if(!guest_respawn_release_ticket(
			s->guest_respawn_cap.cleanup_ticket)) return;
		s->guest_respawn_cap.cleanup_ticket = 0;
	}
	if(s->guest_respawn_phase == GUEST_RESPAWN_IDLE)
		s->guest_respawn_target = (struct server_world_handle) {0};
}

static void guest_respawn_abort(struct server_local* s,
	enum local_guest_result result) {
	uint32_t id = s->guest_respawn_cap.request_id;
	uint32_t ticket = s->guest_respawn_cap.ticket;
	uint8_t slot = s->guest_respawn_target.world_index;
	if(s->guest_respawn_cap.landing_armed)
		s->guest_respawn_cap.cleanup_cancel = true;
	if(ticket)
		s->guest_respawn_cap.cleanup_ticket = ticket;
	if(s->guest_respawn_cap.pending) {
		s->guest_respawn_cap.timed_out_id = id;
		s->guest_respawn_cap.timed_out_slot = slot;
	}
	s->guest_respawn_cap.pending = s->guest_respawn_cap.reserved = false;
	s->guest_respawn_cap.committed = false;
	s->guest_respawn_cap.landing_armed
		= s->guest_respawn_cap.landing_ready = false;
	s->guest_respawn_cap.ticket = 0;
	s->guest_respawn_cap.wait_ticks = 0;
	s->guest_respawn_anchor = s->guest_respawn_ready = false;
	s->guest_respawn_phase = GUEST_RESPAWN_IDLE;
	s->guest_respawn_cap.loading_ticks = 0;
	guest_respawn_failure(s, result, s->guest_respawn_epoch);
	s->guest_respawn_epoch = s->guest_respawn_generation = 0;
	guest_respawn_cleanup_tick(s);
}

static void guest_respawn_preflight_abort(struct server_local* s) {
	guest_respawn_abort(s, LOCAL_GUEST_RESPAWN_RETRY);
}

/* residency-1005/5 (ASTRA-RESIDENCY-FINAL item 1): the LOADING phase is
 * BOUNDED. A plan whose arrival terrain never becomes sendable (a
 * permanently failing column) used to sit in LOADING forever -- the guest
 * on its loading screen, health 0, no failure and no way back. After
 * SERVER_TRANSFER_CLIENT_WAIT_TICKS without a commit (the same bound the
 * respawn capacity preflight and the transfer's landing wait use; it covers
 * the measured Wii near-ring settle, 7-22 s) the plan TERMINATES:
 *   - nothing was committed: the corpse keeps its death-world pose and is
 *     restored to its pre-request state (finished_loading, the recorded
 *     death position), so the ordinary death-screen request is accepted
 *     again -- a deliberate retry starts a fresh epoch;
 *   - the drops already made stay in the death world (Beta drops at death,
 *     EntityPlayer.onDeath; the inventory is empty, so a retry drops
 *     nothing twice);
 *   - ownership is released through the existing preflight abort: the
 *     landing arm is cancelled, a reserved client-capacity ticket is
 *     RELEASEd, residency anchor 1 stops following the respawn target, and
 *     the client gets LOCAL_GUEST_RESPAWN_LOADING_FAILED for THIS epoch,
 *     which ends its loading screen and re-arms the death screen's "Retry
 *     Respawn" (a plain RETRY is ignored by a LOADING client by contract:
 *     test_local_join_ready "first LOADING consumed the press"). */
static void guest_respawn_loading_timeout(struct server_local* s) {
	struct server_player* p = s->guest_actor;
	if(p) {
		p->death_x = s->guest_respawn_death_x;
		p->death_y = s->guest_respawn_death_y;
		p->death_z = s->guest_respawn_death_z;
		p->has_death_pos = true;
		p->finished_loading = true;
	}
	guest_respawn_abort(s, LOCAL_GUEST_RESPAWN_LOADING_FAILED);
}

static bool guest_respawn_capacity_reply(struct server_local* s,
	uint32_t request_id, uint8_t result, uint32_t ticket) {
	if(!s || (request_id & 0xc0000000u) != GUEST_RESPAWN_CAP_ID_BIT)
		return false;
	if(!s->guest_respawn_cap.pending
	   || request_id != s->guest_respawn_cap.request_id) {
		if(request_id == s->guest_respawn_cap.timed_out_id) {
			if(result == CLIN_CAP_OK && ticket)
				s->guest_respawn_cap.cleanup_ticket = ticket;
			s->guest_respawn_cap.timed_out_id = 0;
			guest_respawn_cleanup_tick(s);
		}
		return true;
	}
	s->guest_respawn_cap.pending = false;
	if(result != CLIN_CAP_OK || !ticket) {
		guest_respawn_preflight_abort(s);
		return true;
	}
	if(!server_local_world_from_handle(s, s->guest_respawn_target)) {
		s->guest_respawn_cap.ticket = ticket;
		guest_respawn_preflight_abort(s);
		return true;
	}
	s->guest_respawn_cap.ticket = ticket;
	s->guest_respawn_cap.reserved = true;
	s->guest_respawn_anchor = true;
	struct server_world* target
		= server_local_world_from_handle(s, s->guest_respawn_target);
	if(target->world_index != 0)
		server_transfer_stream_enable(s, target->world_index);
	s->guest_respawn_cap.landing_armed = guest_respawn_landing_send(s, false);
	return true;
}

void server_local_guest_respawn_preflight_tick(struct server_local* s) {
	if(!s) return;
	guest_respawn_cleanup_tick(s);
	if(s->guest_respawn_phase == GUEST_RESPAWN_LOADING
	   && !s->guest_respawn_publish.pending
	   && !s->guest_respawn_cap.committed) {
		if(++s->guest_respawn_cap.loading_ticks
		   > SERVER_TRANSFER_CLIENT_WAIT_TICKS)
			guest_respawn_loading_timeout(s);
		return;
	}
	if(!s || s->guest_respawn_phase != GUEST_RESPAWN_PREPARED
	   || (!s->guest_respawn_cap.pending && !s->guest_respawn_cap.reserved))
		return;
	if(++s->guest_respawn_cap.wait_ticks > SERVER_TRANSFER_CLIENT_WAIT_TICKS) {
		guest_respawn_preflight_abort(s);
		return;
	}
	if(s->guest_respawn_cap.reserved && !s->guest_respawn_cap.landing_armed)
		s->guest_respawn_cap.landing_armed
			= guest_respawn_landing_send(s, false);
	if(s->guest_respawn_cap.landing_armed) {
		struct server_world* target
			= server_local_world_from_handle(s, s->guest_respawn_target);
		if(target && target->world_index != 0
		   && !s->a2w_stream_on[target->world_index])
			server_transfer_stream_enable(s, target->world_index);
	}
}

bool server_local_guest_on_landing_ready(struct server_local* s,
	uint32_t token, uint8_t slot, uint32_t generation) {
	if(!s || (token & 0xc0000000u) != GUEST_RESPAWN_CAP_ID_BIT)
		return false;
	if(s->guest_respawn_phase != GUEST_RESPAWN_PREPARED
	   || !s->guest_respawn_cap.reserved
	   || !s->guest_respawn_cap.landing_armed
	   || token != s->guest_respawn_cap.request_id
	   || slot != s->guest_respawn_target.world_index
	   || generation != s->guest_respawn_target.generation
	   || !server_local_world_from_handle(s, s->guest_respawn_target))
		return true;
	s->guest_respawn_cap.landing_ready = true;
	struct server_rpc retry = {.type = SRPC_RESPAWN};
	retry.payload.respawn.owner = 1;
	retry.payload.respawn.generation = s->guest_generation;
	retry.payload.respawn.respawn_epoch = 0;
	(void)server_local_guest_respawn(s,
		(struct server_rpc_source) {.slot = 1,
			.generation = s->guest_generation}, &retry);
	return true;
}

static void guest_inventory_slot(struct server_player* p, int slot) {
	struct client_rpc reply = {.type = CRPC_INVENTORY_SLOT,
		.payload.inventory_slot = {.window = WINDOWC_INVENTORY, .slot = slot,
			.item = slot == SPECIAL_SLOT_PICKED_ITEM ? p->inventory.picked_item
				: p->inventory.items[slot]}};
	p->inventory.reply(&reply, p->inventory.reply_user);
}

/* A2-T (Opus review A1b): the guest's RESPAWN world. Beta respawns a player
 * that died in the Nether in the Overworld (ServerConfigurationManager.
 * recreatePlayerEntity -> dimension 0; the bed, else the world spawn), so the
 * respawn world is the LIVE Overworld: the death world itself when that is
 * the Overworld, else the session's live Overworld, else -- only when
 * `admit` -- the Overworld admitted now (budget first; a refusal is its own
 * HUD toast and the respawn answers RETRY). The plan is computed by
 * server_local_prepare_guest_respawn, which already plans the Overworld spawn
 * from the live routed world or, when the routed world is the Nether, from
 * the Overworld archive (the host's legacy Nether-death path). */
static struct server_world* guest_respawn_world(struct server_local* s,
	struct server_world* death_world, bool admit) {
	if(death_world && death_world->dimension == WORLD_DIM_OVERWORLD)
		return death_world;
	struct server_world* ow = server_local_world_for_dimension(s, WORLD_DIM_OVERWORLD);
	if(ow && server_local_world_live(ow))
		return ow;
	return admit ? server_local_world_admit(s, WORLD_DIM_OVERWORLD) : NULL;
}

bool server_local_guest_respawn(struct server_local* s,
	struct server_rpc_source source, const struct server_rpc* call) {
	/* A1d (D8): the dead body's own world is the death world (drops fall
	 * there); A2-T: the body respawns in the Overworld whatever world it died
	 * in (guest_respawn_world). */
	struct server_world* gw = s ? server_local_world_of(s, s->guest_actor) : NULL;
	if(!s || !call || call->type != SRPC_RESPAWN || source.slot != 1
	   || call->payload.respawn.owner != 1
	   || call->payload.respawn.generation != source.generation
	   || source.generation != s->guest_generation || !s->guest_actor
	   || !s->guest_owned || !s->guest_confirmed || !s->guest_gameplay || !gw)
		return false;
	struct server_player* p = s->guest_actor;
	if(s->world_switch_phase != SERVER_WORLD_SWITCH_IDLE || s->death_world_evict
	   || (s->input_ack_pending && !server_transfer_guest_continuity(s))
	   || !s->player.has_pos || !s->player.finished_loading) {
		/* The client has already latched a completed hold when this request
		 * arrives. A temporary barrier must answer a valid dead owner so it
		 * can retry; stale epochs and living/foreign bodies remain inert. */
		uint64_t epoch = call->payload.respawn.respawn_epoch;
		bool valid = p->health <= 0 && p->has_pos
			&& (s->guest_respawn_phase == GUEST_RESPAWN_IDLE
				? epoch == 0 && p->finished_loading
				: s->guest_respawn_generation == source.generation
					&& (!epoch || epoch == s->guest_respawn_epoch));
		if(valid)
			guest_respawn_failure(s, LOCAL_GUEST_RESPAWN_RETRY,
				s->guest_respawn_phase == GUEST_RESPAWN_IDLE
					? 0 : s->guest_respawn_epoch);
		return valid;
	}
	if(s->guest_respawn_phase == GUEST_RESPAWN_IDLE
	   && (s->guest_respawn_cap.cleanup_cancel
		|| s->guest_respawn_cap.cleanup_ticket
		|| s->guest_respawn_cap.timed_out_id)) {
		guest_respawn_failure(s, LOCAL_GUEST_RESPAWN_RETRY, 0);
		return true;
	}
	if(s->guest_respawn_phase == GUEST_RESPAWN_IDLE
	   && !guest_respawn_world(s, gw, true)) {
		guest_respawn_failure(s, LOCAL_GUEST_RESPAWN_RETRY, 0);
		return true;
	}
	if(s->guest_respawn_phase == GUEST_RESPAWN_LOADING) {
		if(call->payload.respawn.respawn_epoch
		   && call->payload.respawn.respawn_epoch != s->guest_respawn_epoch)
			return false;
		guest_respawn_loading(s);
		return true;
	}
	if(s->guest_respawn_phase == GUEST_RESPAWN_IDLE) {
		if(call->payload.respawn.respawn_epoch || p->health > 0
		   || !p->has_pos || !p->finished_loading)
			return false;
		if(s->guest_respawn_epoch_counter == UINT64_MAX) {
			guest_respawn_failure(s, LOCAL_GUEST_RESPAWN_TERMINAL, 0);
			return true;
		}
		if(!server_local_prepare_guest_respawn(s, p,
			&s->guest_respawn_x, &s->guest_respawn_y,
			&s->guest_respawn_z, &s->guest_respawn_keep_bed)) {
			guest_respawn_failure(s, LOCAL_GUEST_RESPAWN_RETRY, 0);
			return true;
		}
		struct server_world* target = guest_respawn_world(s, gw, false);
		if(!target) {
			guest_respawn_failure(s, LOCAL_GUEST_RESPAWN_RETRY, 0);
			return true;
		}
		s->guest_respawn_target = server_world_handle_of(target);
		s->guest_respawn_epoch = ++s->guest_respawn_epoch_counter;
		s->guest_respawn_ready = false;
		s->guest_respawn_generation = source.generation;
		if(p->has_bed_spawn && !s->guest_respawn_keep_bed) {
			struct client_rpc warning = {.type = CRPC_HUD_MESSAGE};
			snprintf(warning.payload.hud_message,
				sizeof(warning.payload.hud_message), "%s",
				"Your home bed was missing or obstructed");
			(void)clin_rpc_send_local(&warning, 1, source.generation);
		}
		s->guest_respawn_death_x = p->has_death_pos ? p->death_x : p->x;
		s->guest_respawn_death_y = p->has_death_pos ? p->death_y : p->y;
		s->guest_respawn_death_z = p->has_death_pos ? p->death_z : p->z;
		s->guest_respawn_phase = GUEST_RESPAWN_PREPARED;
	}
	if(s->guest_respawn_phase != GUEST_RESPAWN_PREPARED
	   || s->guest_respawn_generation != source.generation
	   || call->payload.respawn.respawn_epoch != 0)
		return false;
	struct server_world* target
		= server_local_world_from_handle(s, s->guest_respawn_target);
	if(!target) {
		guest_respawn_preflight_abort(s);
		return true;
	}
	if(clin_world_payloads_enabled() && target != gw) {
		/* The destination needs its own context. No drops occur until the
		 * client has reserved capacity and installed the landing core. */
		if(!s->guest_respawn_cap.landing_ready) {
			if(!s->guest_respawn_cap.pending
			   && !s->guest_respawn_cap.reserved) {
				if(server_transfer_active(s)) {
					guest_respawn_preflight_abort(s);
					return true;
				}
				uint32_t counter = ++s->guest_respawn_cap.counter;
				if(!counter || counter >= GUEST_RESPAWN_CAP_ID_BIT)
					counter = s->guest_respawn_cap.counter = 1;
				s->guest_respawn_cap.request_id
					= GUEST_RESPAWN_CAP_ID_BIT | counter;
				s->guest_respawn_cap.wait_ticks = 0;
				if(!guest_join_send_capacity(s, target->world_index,
					CLIN_CAP_OP_RESERVE,
					s->guest_respawn_cap.request_id, 0))
					guest_respawn_preflight_abort(s);
				else s->guest_respawn_cap.pending = true;
			}
			return true;
		}
	}
	w_coord_t dcx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_death_x));
	w_coord_t dcz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_death_z));
	if(!server_world_is_chunk_loaded(gw, dcx, dcz)
	   || !server_local_close_active_inventory_for(s, p)) {
		if(s->guest_respawn_cap.reserved) {
			guest_respawn_preflight_abort(s);
			return true;
		}
		guest_respawn_failure(s, LOCAL_GUEST_RESPAWN_RETRY,
			s->guest_respawn_epoch);
		return true;
	}
	vec3 death = {(float)s->guest_respawn_death_x,
		(float)s->guest_respawn_death_y, (float)s->guest_respawn_death_z};
	(void)death;
	for(int k = 0; k < INVENTORY_SIZE; k++) {
		const int i = server_local_death_drop_order[k];
		struct item_data item;
		inventory_get_slot(&p->inventory, i, &item);
		if(!item.id)
			continue;
		/* A2-T: into the DEATH world. G6: Beta EntityPlayer.onDeath ->
		 * InventoryPlayer.dropAllItems -- dropAllItems order, Beta spawn
		 * offset and death scatter (server_local_spawn_death_item_w). */
		struct entity* drop = server_local_spawn_death_item_w(gw,
			s->guest_actor, s->guest_respawn_death_x, s->guest_respawn_death_y,
			s->guest_respawn_death_z, &item);
		if(!drop)
			continue;
		inventory_clear_slot(&p->inventory, i);
		guest_inventory_slot(p, i);
	}
	struct item_data picked;
	if(inventory_get_picked_item(&p->inventory, &picked)) {
		struct entity* drop = server_local_spawn_item_w(gw, death, &picked, false);
		if(drop) {
			drop->data.item.pickup_delay = 40;
			inventory_clear_picked_item(&p->inventory);
			guest_inventory_slot(p, SPECIAL_SLOT_PICKED_ITEM);
		}
	}
	p->has_death_pos = false;
	p->finished_loading = false;
	p->reject_pos_this_update = true;
	s->guest_respawn_phase = GUEST_RESPAWN_LOADING;
	s->guest_respawn_cap.loading_ticks = 0;
	s->guest_respawn_anchor = true;
	guest_respawn_loading(s);
	return true;
}

static void guest_respawn_publish_poll(struct server_local* s) {
	if(!s->guest_respawn_publish.pending) return;
	if(!guest_publication_current(s, s->guest_respawn_publish.ticket,
			s->guest_respawn_publish.generation, s->guest_respawn_publish.world_handle)
	   || s->guest_respawn_publish.epoch != s->guest_respawn_epoch
	   || s->guest_respawn_generation != s->guest_respawn_publish.generation
	   || s->guest_respawn_phase != GUEST_RESPAWN_LOADING
	   || server_transfer_actor_frozen(s, s->guest_actor)) {
		s->guest_respawn_publish.pending = false;
		return;
	}
	struct server_rpc_source source = {.slot = 1,
		.generation = s->guest_respawn_publish.generation};
	unsigned count = guest_snapshot_count(s, source);
	/* The same exclusive-producer proof as guest_publish covers this batch.
	 * Admit health + inventory + correction + READY before sending any of it;
	 * retries never re-run the committed fresh-body/drop/membership effects. */
	if(!count || !clin_rpc_headroom(count + 3)) return;
	struct server_player* p = s->guest_actor;
	if(!clin_try_rpc_send_local(&(struct client_rpc){.type = CRPC_PLAYER_SET_HEALTH,
			.payload.player_set_health.health = p->health}, 1, source.generation)
	   || !server_local_guest_publish(s, source)
	   || !guest_correct_position_epoch(s, PLAYER_CORRECTION_POSITION_ONLY
			| PLAYER_CORRECTION_ZERO_VEL_X | PLAYER_CORRECTION_ZERO_VEL_Y
			| PLAYER_CORRECTION_ZERO_VEL_Z | PLAYER_CORRECTION_SET_ON_GROUND,
			s->guest_respawn_publish.epoch, true)) return;
	struct client_rpc ready = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = s->guest_respawn_publish.ticket,
			.generation = source.generation, .owner = 1,
			.respawn_epoch = s->guest_respawn_publish.epoch,
			.result = LOCAL_GUEST_GAMEPLAY_READY,
			.entity_id = s->guest_entity_id, .health = p->health,
			.x = p->x, .y = p->y, .z = p->z,
			.rx = p->rx, .ry = p->ry}};
	if(!clin_try_rpc_send_local(&ready, 1, source.generation)) return;
	/* The committed body remains hazard-inert until all required state and
	 * READY are queued. This is the existing finished_loading hazard gate. */
	p->finished_loading = true;
	s->guest_respawn_publish.pending = false;
	s->guest_respawn_phase = GUEST_RESPAWN_IDLE;
	s->guest_respawn_cap.landing_armed = s->guest_respawn_cap.landing_ready = false;
	s->guest_respawn_cap.request_id = 0;
	s->guest_respawn_target = (struct server_world_handle) {0};
	s->guest_respawn_epoch = s->guest_respawn_generation = 0;
	s->guest_respawn_anchor = false;
	s->guest_respawn_ready = false;
}

static void guest_respawn_commit(struct server_local* s,
	struct server_world* respawn_world) {
	struct server_player* p = s->guest_actor;
	if(s->guest_respawn_cap.reserved) {
		uint8_t slot = respawn_world->world_index;
		server_transfer_on_world_vacated(s, slot);
		s->transfer_slot_ticket[slot] = s->guest_respawn_cap.ticket;
		s->guest_respawn_cap.reserved = false;
		s->guest_respawn_cap.committed = false;
		s->guest_respawn_cap.ticket = 0;
	}
	/* A2-T: the fresh body is committed to the Overworld world (a Nether
	 * death's drops stay behind in the Nether) and its windows bind there. */
	p->world_slot = respawn_world->world_index;
	p->window_world = server_world_handle_of(respawn_world);
	p->health = MAX_PLAYER_HEALTH;
	p->x = s->guest_respawn_x; p->y = s->guest_respawn_y;
	p->z = s->guest_respawn_z;
	p->has_bed_spawn = s->guest_respawn_keep_bed;
	p->rx = p->ry = 0.0f;
	p->fall_distance = p->old_vel_y = p->vel_y = 0.0f;
	p->vel_x = p->vel_z = 0.0f;
	p->on_ground = p->sneaking = false;
	p->hurt_resist_time = p->last_damage = 0;
	p->damage_remainder = 0; p->oxygen = MAX_OXYGEN;
	p->fire = -PLAYER_FIRE_RESISTANCE;
	p->distance_walked = 0.0f; p->next_step_distance = 1;
	p->was_in_water = true; p->dimension = WORLD_DIM_OVERWORLD;
	/* A1d: a fresh EntityPlayer is awake (Entity/EntityPlayer field
	 * initialisers); the lethal hit already woke the old body, this is the
	 * belt for any path that did not. */
	p->sleeping = false; p->sleep_timer = 0;
	p->time_until_portal = 20; p->time_in_portal = 0.0f;
	p->in_portal = p->portal_travel_requested = false;
	p->added_to_chunk = false; p->recipe_craft_ticket = 0;
	p->recipe_craft_reason = RECIPE_CRAFT_INVALID_REQUEST;
	/* Beta respawn constructs a NEW EntityPlayer, whose field initialiser is
	 * `public int ticksExisted = 0;` (Entity.java:51). The host's fresh-body
	 * paths reset it (server_local.c, the W1F "fresh EntityPlayer" writes);
	 * the guest body is reused in place, so the reset has to be explicit or
	 * the Peaceful regen cadence (difficulty_peaceful_regen_due's
	 * `ticksExisted % 20 * 12 == 0`, EntityPlayer.java:173) carries the old
	 * body's phase into the new one. [split-dimensions-plan-0924/C2a] */
	p->ticks_existed = 0;
	/* F4-RNG: the replacement body's own fresh Entity.rand (RNG ruling 8). */
	p->entity_rand.seeded = false;
	/* G8-BLAST-VELOCITY v3: a fresh EntityPlayer has no pending velocity,
	 * events or server motion (the recycled struct must not carry them). */
	server_local_actor_motion_reset(p);
	p->finished_loading = false;
	server_local_rz_retire_body(p); /* residency-1005/2: fresh body pose */
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MONSTER
		   && e->data.monster.target_actor.slot == ACTOR_SLOT_GUEST
		   && e->data.monster.target_actor.generation == s->guest_generation)
			e->data.monster.aggro_time = 0;
		dict_entity_next(it);
	}
	s->guest_respawn_publish.pending = true;
	s->guest_respawn_publish.ticket = s->guest_ticket;
	s->guest_respawn_publish.generation = s->guest_generation;
	s->guest_respawn_publish.epoch = s->guest_respawn_epoch;
	s->guest_respawn_publish.world_handle = server_world_handle_of(respawn_world);
	guest_respawn_publish_poll(s);
}

void server_local_guest_respawn_poll(struct server_local* s) {
	if(!s) return;
	if(!s->guest_owned || !s->guest_actor) {
		s->guest_respawn_publish.pending = false;
		return;
	}
	if(s->guest_respawn_publish.pending) {
		guest_respawn_publish_poll(s);
		return;
	}
	/* Commit only to the exact generation offered in LOADING. */
	struct server_world* gw = server_local_guest_respawn_target(s);
	if(!s || !s->guest_respawn_ready || !s->guest_actor
	   || s->guest_respawn_phase != GUEST_RESPAWN_LOADING
	   || s->guest_respawn_generation != s->guest_generation
	   || !s->guest_owned || !s->guest_confirmed || !s->guest_gameplay
	   || !gw || s->world_switch_phase != SERVER_WORLD_SWITCH_IDLE
	   || s->death_world_evict
	   || (s->input_ack_pending && !server_transfer_guest_continuity(s))
	   || !s->player.has_pos || !s->player.finished_loading
	   || s->guest_actor->health > 0 || !s->guest_actor->has_pos
	   || s->guest_actor->finished_loading
	   || !server_world_is_chunk_sent(gw,
		WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_x)),
		WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_z))))
		return;
	if(s->guest_respawn_cap.reserved && !s->guest_respawn_cap.committed) {
		if(!guest_join_send_capacity(s, gw->world_index, CLIN_CAP_OP_COMMIT,
			s->guest_respawn_cap.request_id, s->guest_respawn_cap.ticket))
			return;
		s->guest_respawn_cap.committed = true;
	}
	guest_respawn_commit(s, gw);
}

bool server_local_guest_respawn_ready(struct server_local* s,
	struct server_rpc_source source, const struct server_rpc* call) {
	if(!s || !call || call->type != SRPC_LOCAL_GUEST || source.slot != 1
	   || call->payload.local_guest.action != LOCAL_GUEST_RESPAWN_READY
	   || call->payload.local_guest.ticket != s->guest_ticket
	   || call->payload.local_guest.owner != 1
	   || call->payload.local_guest.generation != source.generation
	   || call->payload.local_guest.respawn_epoch != s->guest_respawn_epoch
	   || source.generation != s->guest_generation
	   || s->guest_respawn_generation != source.generation
	   || s->guest_respawn_phase != GUEST_RESPAWN_LOADING
	   || !s->guest_respawn_anchor || !s->guest_actor)
		return false;
	s->guest_respawn_ready = true;
	server_local_guest_respawn_poll(s);
	return true;
}

void server_local_guest_control(struct server_local* s,
	struct server_rpc_source source, const struct server_rpc* call) {
	if(!s || !call || call->type != SRPC_LOCAL_GUEST
	   || !call->payload.local_guest.ticket) return;
	uint64_t ticket = call->payload.local_guest.ticket;
	enum local_guest_action action = call->payload.local_guest.action;
	if(action == LOCAL_GUEST_RESPAWN_READY) {
		(void)server_local_guest_respawn_ready(s, source, call);
		return;
	}
	bool primary = svin_source_equal(source, (struct server_rpc_source){0});
	if(action == LOCAL_GUEST_JOIN) {
		if(!primary) return;
		if(s->guest_owned && ticket == s->guest_ticket) {
			guest_status(ticket,s->guest_generation,s->guest_confirmed ? LOCAL_GUEST_ATTACHED
				: s->guest_snapshot_sent ? LOCAL_GUEST_INSTALLED : LOCAL_GUEST_OFFER);
			return;
		}
		if(server_local_host_respawn_active(s)) {
			guest_status(ticket,0,LOCAL_GUEST_REJECTED); return;
		}
		/* A2-T (A2T-needs-from-B-C #5, C#3): server_transfer_active is the
		 * single "an actor is mid-transfer" predicate; Join refuses while it
		 * holds, exactly as on a world switch. (The cross-dimension admission
		 * ruling -- "host in the Nether, guest saved in the Overworld" -- is
		 * still open with Astra; the Overworld-only rule below stays until it
		 * lands.) */
		if(server_transfer_active(s)) {
			guest_status(ticket,0,LOCAL_GUEST_REJECTED); return;
		}
		/* A2-T (Astra a2t-0927): the Overworld-only-host Join rule is REPLACED
		 * by "the saved dimension's world is LIVE or admissible"
		 * (guest_admission_world inside guest_create): a guest saved in the
		 * Overworld joins the Overworld while the host is in the Nether, and
		 * vice versa. An admission refusal (budget, no slot) names itself on
		 * the host HUD (server_local_world_admit's toast) and is lossless. */
		/* A2-T client capacity (Astra a2t-0927: reserve client/GPU capacity
		 * before membership): a Join into a world that is not the host's own
		 * asks the client first; the body is created on its answer
		 * (server_local_guest_on_capacity_reply). A re-sent Join while the
		 * RESERVE is out is the same Join: nothing new is sent. */
		if(s->guest_join_cap.pending) {
			if(ticket != s->guest_join_cap.join_ticket)
				guest_status(ticket,0,LOCAL_GUEST_REJECTED);
			return;
		}
		if(s->guest_join_cap.cleanup_ticket[0] || s->guest_join_cap.cleanup_ticket[1]) {
			guest_status(ticket,0,LOCAL_GUEST_REJECTED); return;
		}
		if(guest_join_begin_capacity(s, ticket))
			return;
		struct server_rpc_source guest;
		if(!guest_create(s,&guest,NULL)) {
			guest_status(ticket,0,LOCAL_GUEST_REJECTED); return;
		}
		s->guest_ticket=ticket;
		guest_status(ticket,guest.generation,LOCAL_GUEST_OFFER);
		return;
	}
	if(!s->guest_owned || !s->guest_actor || ticket != s->guest_ticket) {
		/* An early cancel queued behind a rejected Join must still retire. */
		if(primary && action == LOCAL_GUEST_CANCEL)
			guest_status(ticket,0,LOCAL_GUEST_LEFT);
		return;
	}
	struct server_rpc_source guest = {.slot=1,.generation=s->guest_generation};
	if(action == LOCAL_GUEST_CANCEL) {
		if(!primary) return;
	} else if(!svin_source_equal(source,guest)) return;
	if(action == LOCAL_GUEST_CANCEL || action == LOCAL_GUEST_LEAVE) {
		bool ok=true;
		if(s->guest_confirmed) ok=server_local_guest_leave(s,guest);
		else guest_release(s); /* no gameplay requests admitted before Confirm */
		guest_status(ticket,guest.generation,ok ? LOCAL_GUEST_LEFT : LOCAL_GUEST_LEAVE_FAILED);
	} else if(action == LOCAL_GUEST_ACCEPT) {
		if(s->guest_confirmed) {
			guest_status(ticket,guest.generation,LOCAL_GUEST_ATTACHED); return;
		}
		if(!s->guest_accept_publish.pending) {
			s->guest_accept_publish.pending = true;
			s->guest_accept_publish.ticket = ticket;
			s->guest_accept_publish.generation = guest.generation;
			s->guest_accept_publish.world_handle = server_world_handle_of(
				server_local_world_of(s, s->guest_actor));
			s->guest_accept_publish.result = s->guest_snapshot_sent
				? LOCAL_GUEST_INSTALLED : 0;
			if(vsp_armed())
				vsp_srv_guest_accept(s);
		}
		guest_accept_publish_poll(s);
	} else if(action == LOCAL_GUEST_CONFIRM && s->guest_snapshot_sent) {
		s->guest_confirmed=true;
		guest_status(ticket,guest.generation,LOCAL_GUEST_ATTACHED);
	} else if(action == LOCAL_GUEST_GAMEPLAY) {
		/* Retryable by design: the client re-asks on later frames while the
		 * body's terrain is still streaming in. A denial never retires the
		 * ATTACHED session -- the guest keeps its windows and may Leave. */
		/* R2: a Join whose terrain wait terminated is over for this
		 * generation -- a stale or late request cannot revive it. */
		if(s->guest_join_wait.failed && !s->guest_gameplay
		   && guest.generation == s->guest_generation) {
			guest_status(ticket,guest.generation,LOCAL_GUEST_GAMEPLAY_DENIED);
			return;
		}
		if(!server_local_guest_gameplay(s,guest)) {
			if(guest.generation == s->guest_generation
			   && !s->guest_join_wait.armed && guest_join_terrain_pending(s)) {
				s->guest_join_wait.armed = true;
				s->guest_join_wait.wait_ticks = 0;
			}
			guest_status(ticket,guest.generation,LOCAL_GUEST_GAMEPLAY_DENIED);
			return;
		}
		struct server_player* p = s->guest_actor;
		/* D2 (ASTRA-DEFECTS-RULING 7-10): name the world this body was
		 * admitted into. The client must install it there and bind the
		 * guest's view to it; inferring "the host's current world" put the
		 * guest's prediction in the Nether after a host transfer while the
		 * server kept it in the Overworld. */
		const struct server_world* gw = server_local_world_of(s, p);
		clin_rpc_send(&(struct client_rpc){.type=CRPC_LOCAL_GUEST,
			.payload.local_guest={.ticket=ticket,.generation=guest.generation,
				.result=LOCAL_GUEST_GAMEPLAY_READY,.entity_id=s->guest_entity_id,
				.health=p->health,
				.x=p->x,.y=p->y,.z=p->z,.rx=p->rx,.ry=p->ry,
				.correction_seq=p->correction_seq,
				.world_slot=gw?gw->world_index:0,
				.world_generation=gw?gw->generation:0,
				.world_dimension=(int8_t)(gw?gw->dimension:0),
				.guest_bind_seq=server_local_guest_bind_seq()}});
		if(vsp_armed())
			vsp_srv_guest_ready(s);
	}
}

/* ===== R-S1a3 F2: the public correction author =============================
 * The server-side knockback integrator (server_local.c
 * server_local_tick_actor_knockback) runs for EVERY present body and must
 * re-author each one on ITS OWN client route. The host's route is
 * server_local_next_player_correction + CRPC_PLAYER_POS; this is the guest's,
 * and it is deliberately the SAME guest_correct_position the delta clamp
 * already uses rather than a second copy of the envelope -- one writer for the
 * guest's correction token means a shove and a rejected jump can never author
 * two different views of "the position the guest must snap back to".
 *
 * Appended at the END of the file (not folded into guest_correct_position's
 * definition) so the S1-d lane merges without touching a line of it. */
void server_local_guest_correct_position(struct server_local* s) {
	if(!s || !s->guest_actor)
		return;
	/* damage-client-0926 (PROPOSED): keep the guest's motionY -- the
	 * knockback's vertical half is client-integrated (CRPC_LOCAL_PLAYER_VELOCITY);
	 * zeroing it here cancelled the rise on the first shove tick. */
	guest_correct_position(s, PLAYER_CORRECTION_POSITION_ONLY
		| PLAYER_CORRECTION_ZERO_VEL_X | PLAYER_CORRECTION_ZERO_VEL_Z);
}

bool server_local_guest_correct_position_for(struct server_local* s,
	uint64_t expected_generation, uint8_t correction_flags) {
	if(!s || !s->guest_actor || !expected_generation
	   || s->guest_generation != expected_generation)
		return false;
	/* A correction invalidates only this owner's already-queued samples. */
	s->guest_actor->reject_pos_this_update = true;
	return guest_correct_position_epoch(s, correction_flags, 0, true);
}

/* A1d: a HUD toast on the acting body's own route. */
void server_local_actor_hud_message(struct server_local* s,
	const struct server_player* p, const char* msg) {
	if(!s || !p || !msg) return;
	if(p == &s->player) {
		server_local_send_hud_message(s, msg);
		return;
	}
	if(p != s->guest_actor || !s->guest_generation) return;
	struct client_rpc m = {.type = CRPC_HUD_MESSAGE};
	snprintf(m.payload.hud_message, sizeof(m.payload.hud_message), "%s", msg);
	(void)clin_rpc_send_local(&m, 1, s->guest_generation);
}

/* ===== A2-T: the guest's actor-transition adapter (lane C) ==================
 * A2 (ASTRA-A2-DESIGN-0925 section 2; server_transfer.[ch]) owns the player
 * transaction -- REQUEST -> PREPARE -> COMMIT, or PREPARE -> ROLLBACK --
 * including the window closure (PREPARE), the staged pose and the membership
 * switch (commit() rebinds world_slot, dimension and the pose on the body).
 * Lane C writes no lifecycle of its own; it supplies two pieces:
 *
 *   server_local_guest_commit_record  the guest's COMMIT record, written
 *       just before A2's allocation-free commit step: the guest record
 *       (.Data.CaveWiiGuest) rebuilt AT the staged destination (dimension +
 *       pose) through the C3 #27 record builder, installed, and level.dat
 *       saved -- the guest twin of the host's Player-record write
 *       (A2T-needs-from-B-C C#2). This is the LEVEL-LAST half of A2's
 *       chunks-first/level-last contract (A2 checkpoints the destination's
 *       chunks first), NOT a durable-commit marker: there is no journal, the
 *       other LIVE world is not checkpointed in the same cut, and a crash
 *       between the chunk and level writes leaves whichever record reached
 *       the disk (Astra a2t-0927 contract correction; journal deferred). A
 *       failed save restores the prior in-memory record before A2 rolls back.
 *   server_local_guest_on_travel_transaction / _on_respawn_transaction  APPLY
 *       the published outcome to the guest body and its client route: COMMIT
 *       rebinds the window world to the destination, drops the old world's
 *       queued pose samples, restarts the odometer/fall sampler/membership,
 *       and sends the guest its generation-tagged correction at the committed
 *       pose; ROLLBACK changes nothing (lane C stages nothing before the
 *       record write, a failed write restores the prior record, and a
 *       successful one is always followed by the commit) and is acknowledged
 *       APPLIED.
 * The C3 #27 contract edges hold: a missing session, transaction or owned
 * guest body -- or an outcome for a stale generation / another slot -- is
 * REFUSED; PENDING_A2 is no longer returned. */
/* Drop the named child of a compound (the record had not existed before). */
static void guest_remove_node(nbt_node* parent, const char* name) {
	if(!parent || parent->type != TAG_COMPOUND || !parent->payload.tag_compound)
		return;
	struct list_head *current, *next;
	list_for_each_safe(current, next, &parent->payload.tag_compound->entry) {
		struct nbt_list* entry = list_entry(current, struct nbt_list, entry);
		if(entry->data && entry->data->name && !strcmp(entry->data->name, name)) {
			list_del(current);
			nbt_free(entry->data);
			free(entry);
			return;
		}
	}
}

bool server_local_guest_commit_record(struct server_local* s,
	enum world_dim dimension, double x, double y, double z, float rx, float ry) {
	if(!s || !s->guest_actor || !s->guest_owned || !s->level.data)
		return false;
	if(dimension != WORLD_DIM_OVERWORLD && dimension != WORLD_DIM_NETHER)
		return false;
	struct guest_record_pose at = {.dimension = dimension, .x = x, .y = y,
		.z = z, .rx = rx, .ry = ry};
	nbt_node* copy = NULL;
	if(!guest_checkpoint_record_at(s, s->guest_actor, &at, &copy)) return false;
	nbt_node* data = nbt_find_by_path(s->level.data, ".Data");
	nbt_node* existing = nbt_find_by_path(s->level.data, ".Data." GUEST_TAG);
	nbt_node* prior = existing ? nbt_clone(existing) : NULL;
	if(existing && !prior) { nbt_free(copy); return false; }
	if(!guest_set_node(data, GUEST_TAG, copy)) {
		nbt_free(copy); nbt_free(prior); return false;
	}
	if(server_local_light_stopped(s)) { /* G9 R3: stopped session */
		nbt_free(prior);
		return false;
	}
	bool was_modified = s->level.modified;
	s->level.modified = true;
	if(level_archive_save(&s->level)) {
		nbt_free(prior);
		return true;
	}
	/* The save failed and A2 rolls the transaction back: the IN-MEMORY
	 * record returns to exactly what it was, so no later save can publish a
	 * destination the body never reached. (What reached the disk before the
	 * failure is whatever level_archive_save left -- chunks-first/level-last
	 * promises no more; the journal is deferred.) */
	if(prior) {
		if(!guest_set_node(data, GUEST_TAG, prior)) nbt_free(prior);
	} else {
		guest_remove_node(data, GUEST_TAG);
	}
	s->level.modified = was_modified;
	return false;
}

static enum server_actor_transition_result guest_apply_transition(
	struct server_local* s, const struct server_actor_transition* tx) {
	if(!s || !tx || !s->guest_owned || !s->guest_actor
	   || tx->actor.slot != ACTOR_SLOT_GUEST
	   || tx->actor.generation != s->guest_generation)
		return SERVER_ACTOR_TRANSITION_REFUSED;
	if(tx->phase == SERVER_TRANSITION_ROLLBACK)
		return SERVER_ACTOR_TRANSITION_APPLIED; /* nothing staged, nothing to undo */
	if(tx->phase != SERVER_TRANSITION_COMMIT)
		return SERVER_ACTOR_TRANSITION_REFUSED;
	struct server_player* p = s->guest_actor;
	struct server_world* w = server_local_world_of(s, p);
	/* A2's commit() already moved the body; APPLY checks it landed where the
	 * outcome says before touching the client route. */
	if(!w || w->dimension != tx->destination_dimension
	   || p->dimension != tx->destination_dimension
	   || server_world_handle_of(w).generation != tx->destination.generation
	   || p->x != tx->x || p->y != tx->y || p->z != tx->z)
		return SERVER_ACTOR_TRANSITION_REFUSED;
	s->guest_accept_publish.pending = s->guest_respawn_publish.pending = false;
	/* Windows bind to the committed world (PREPARE closed the open one). */
	p->window_world = tx->destination;
	/* Per-body histories: samples queued for the OLD world are dropped, the
	 * footstep odometer restarts, the fall sampler starts from the arrival
	 * pose, and chunk membership is re-established in the new world. */
	p->reject_pos_this_update = true;
	p->distance_walked = 0.0f;
	p->next_step_distance = 1;
	p->fall_distance = 0.0f;
	p->added_to_chunk = false;
	/* The guest's client snaps to the committed pose (rotation included) on
	 * its own generation-tagged route. */
	if(s->guest_gameplay)
		guest_correct_position(s, PLAYER_CORRECTION_ZERO_VEL_X
			| PLAYER_CORRECTION_ZERO_VEL_Y | PLAYER_CORRECTION_ZERO_VEL_Z
			| PLAYER_CORRECTION_SET_ROTATION);
	return SERVER_ACTOR_TRANSITION_APPLIED;
}

enum server_actor_transition_result server_local_guest_on_travel_transaction(
	struct server_local* s, const struct server_actor_transition* tx) {
	return guest_apply_transition(s, tx);
}

enum server_actor_transition_result server_local_guest_on_respawn_transaction(
	struct server_local* s, const struct server_actor_transition* tx) {
	/* The same APPLY: a cross-world respawn is a COMMIT to the respawn world
	 * at the respawn pose (A2 builds the fresh body before publishing). */
	return guest_apply_transition(s, tx);
}

/* ===== A2-T: the guest's portal dwell (lane C) ==============================
 * SPLITDIMS-HOSTSWITCH-GAP-0927 blocker 2: guest movement was position-only
 * with no portal dwell, so a guest could never cross in-game. This runs the
 * HOST's exact dwell state machine on the guest body -- Beta
 * BlockPortal.onEntityCollidedWithBlock -> EntityPlayer.setInPortal (contact
 * during the cooldown pins it at 10) and EntityPlayerSP.onLivingUpdate (the
 * previous tick's contact consumed: += 0.0125F, latch at >= 1.0F with the
 * cooldown set to 10; decay -0.05F; cooldown --) -- through the shared
 * server_player_set_in_portal / server_player_portal_tick, so the dwell and
 * cooldown are byte-identical to the host's and to the portal-travel goldens
 * (81 additions + one arming tick: the latch on contact tick 82).
 * [decompile 740c583 EntityPlayerSP.onLivingUpdate:39-77;
 *  EntityPlayer.setInPortal:811-816]
 *
 * The latch is consumed alive-gated (Beta usePortal skips a dead entity, the
 * host consumer's deviation-#18 guard) and OPENS THE GUEST'S A2-T TRANSACTION
 * (server_transfer_request -> PREPARE -> lane C's marker -> commit -> this
 * file's APPLY) into the other dimension of the guest's own world. Beta calls
 * usePortal inside onLivingUpdate on the latch tick; the request is opened on
 * that same tick (the host defers its legacy switch to the next tick top only
 * because that path destroys the world -- a transaction destroys nothing).
 * A refused request (busy / admission) is named on the guest's own HUD; the
 * cooldown the latch set means standing in the frame will not re-arm until the
 * guest steps out, exactly as for the host.
 *
 * Gated on world-bound client payloads (clin_world_payloads_enabled): with the
 * flag OFF a second world cannot be shown to the guest, so the body keeps its
 * old position-only behaviour -- no contact scan, no ramp, no cooldown tick --
 * and nothing changes for any existing run. The portal.trigger ui sound stays
 * host-only (cosmetic; the guest's own sound route is lane B's). */
void server_local_guest_portal_tick(struct server_local* s) {
	if(!s || !clin_world_payloads_enabled() || !s->guest_gameplay
	   || !s->guest_actor || !s->guest_actor->has_pos
	   || !s->guest_actor->finished_loading)
		return;
	/* residency-1005/2 (ASTRA-RESIDENCY-REVIEW item 6): the same gate as the
	 * host's portal tick (host_terrain_live) -- a body in bounded residency
	 * recovery neither advances its dwell nor requests a transfer from stale
	 * contact. Ordinary refused movement keeps the body at its resident pose,
	 * where the portal behaves normally. */
	if(s->guest_actor->rz.recovering)
		return;
	struct server_player* p = s->guest_actor;
	struct server_world* w = server_local_world_of(s, p);
	if(!w)
		return;
	/* consume the previous tick's contact: ramp / decay / cooldown */
	server_local_actor_portal_tick(s, p);
	if(p->portal_travel_requested) {
		p->portal_travel_requested = false;
		/* Opus review A2: request even while the HOST's transaction is open
		 * -- server_transfer_request serializes opposite actors (a queued
		 * request answers OK) and answers DUPLICATE for this actor's own open
		 * one; every refusal is named on the guest's HUD, never dropped. */
		if(p->health > 0) {
			enum world_dim dst = w->dimension == WORLD_DIM_NETHER
				? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER;
			uint32_t id = 0;
			enum server_transfer_rc rc = server_transfer_request(s, p, dst, &id);
			if(rc != SERVER_TRANSFER_OK && rc != SERVER_TRANSFER_DUPLICATE)
				server_local_actor_hud_message(s, p,
					rc == SERVER_TRANSFER_REFUSED_BUSY
						? "Wait: the portal is busy"
						: "The other dimension cannot open now");
		}
	}
	/* this tick's contact scan: Beta Entity.moveEntity's post-move cell sweep
	 * over the player AABB (0.6 x 1.8; p->y is the EYE) with the +-0.001
	 * epsilons -- the host's scan, on the guest's OWN world */
	/* BlockPortal.java:124-129: mounted contact cannot arm setInPortal.
	 * Resolve the relation only on a portal cell, after consuming prior contact. */
	double fy = p->y - PLAYER_EYE_HEIGHT;
	w_coord_t x0 = (w_coord_t)floor(p->x - 0.3 + 0.001);
	w_coord_t x1 = (w_coord_t)floor(p->x + 0.3 - 0.001);
	w_coord_t y0 = (w_coord_t)floor(fy + 0.001);
	w_coord_t y1 = (w_coord_t)floor(fy + 1.8 - 0.001);
	w_coord_t z0 = (w_coord_t)floor(p->z - 0.3 + 0.001);
	w_coord_t z1 = (w_coord_t)floor(p->z + 0.3 - 0.001);
	for(w_coord_t x = x0; x <= x1; x++)
		for(w_coord_t y = y0; y <= y1; y++)
			for(w_coord_t z = z0; z <= z1; z++) {
				struct block_data b;
				if(server_world_get_block(w, x, y, z, &b) && b.type == BLOCK_PORTAL) {
					if(!server_local_actor_vehicle(s, p))
						server_player_set_in_portal(p);
					return;
				}
			}
}

/* ===== O07 / R04 D2-1: the guest-binding sequence ========================
 * Process-wide and monotonic (like server_guest_generation), server thread
 * only. Bumped when a guest actor is BOUND (admission) and on every guest
 * gate-1 world reset (server_transfer.c a2w_guest_gate1); GAMEPLAY_READY
 * carries the current value. The client never lets an older binding move the
 * guest (client_interface.c clin_guest_bind_stale): a world identity that is
 * still live is not proof the guest still belongs to it.
 * O10 / R05 P3 wrap: the sequence counts 1..UINT32_MAX and wraps to 1 (0 is
 * the unstamped value). The consumer orders it by serial-number arithmetic
 * (client_interface.c clin_bind_seq_after: b after a iff (int32_t)(b-a) > 0),
 * so the wrap successor is newer; one bump per guest admission/transfer keeps
 * any two live bindings far inside that 2^31 window. */
static uint32_t s_guest_bind_seq;

uint32_t server_local_guest_bind_seq_bump(void) {
	if(++s_guest_bind_seq == 0)
		s_guest_bind_seq = 1; /* 0 means unstamped */
	return s_guest_bind_seq;
}

void server_local_test_guest_bind_seq_set(uint32_t seq) {
	s_guest_bind_seq = seq; /* test seam: the wrap boundary */
}

uint32_t server_local_guest_bind_seq(void) {
	return s_guest_bind_seq;
}
