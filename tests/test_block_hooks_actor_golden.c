/* Copyright (c) 2026 CaveWii_v2 contributors
 * S1-a1b: the HOST-path golden for "the block/item place / activate / item-use
 * hooks now take the acting actor".
 *
 * THE CLAIM. Threading `struct server_player* actor` through onItemPlace,
 * onUseOnBlock, onRightClick and onRightClickHandled (blocks.h /
 * item_types.h), and through the shared placers they call
 * (block_place_obstructed_for, block_place_default, block_sign_place,
 * piston_server_place, redstone_repeater_place), is a PURE PARAMETER MOVE for
 * the host: every call site passes &s->player, so nothing the host can observe
 * changes. Astra gate-2 D1 was explicit that "'pure move' remains a claim
 * requiring proof", and required a host STATE golden *and* an RPC-ORDER
 * golden. This file is both.
 *
 * WHY THIS FILE COMPILES ON BOTH TREES, which is what makes the comparison
 * meaningful: it never calls a hook pointer directly. Every step goes through
 * server_local_test_process_rpc -- the slot-0 owner handler the real transport
 * calls -- so the same source text, byte for byte, builds and runs at the base
 * sha 32e9fba9 (where the hooks take no actor) and after the change. It names
 * no symbol this card added.
 *
 * WHAT THE TWO HASHES COVER.
 *   state  A rolling digest folded after EVERY scripted step (not only at the
 *          end, so a divergence that later re-converges cannot hide): the step
 *          index, every block id+metadata+sky/torch light in the bounded
 *          region below, and the host actor (pos/rot/health/oxygen/sleeping/
 *          active-window identity/hotbar index/all INVENTORY_SIZE slots).
 *   rpc    The ORDER and identity of every client_rpc the server emitted,
 *          folded in emission order by a clin_test_set_rpc_observer hook:
 *          the type ordinal for every RPC, plus the NAMED payload fields for
 *          the types this dispatch actually produces. Payload bytes are hashed
 *          FIELD BY FIELD rather than with one memcpy on purpose: three
 *          payload arms (chunk, far_chunk, map_data) carry heap POINTERS and
 *          several have interior padding, so a raw-bytes digest would be
 *          non-deterministic across processes and the golden would be noise.
 *
 * THE SCRIPT covers, with the host actor, one of each hooked BLOCK class named
 * in the S1-a1 fail-closed list (chest, furnace, workbench, sign, door, bed,
 * cake, piston, stairs, pumpkin, dispenser) and every hooked ITEM whose hook
 * reads the acting player (sign, bed, wooden door, iron door, repeater,
 * redstone dust, and all nine foods), plus three actor-free item-use classes
 * for order coverage (water bucket, hoe, seeds). Each step asserts the world
 * or the actor actually changed, so the golden can never degrade into pinning
 * an idle simulation.
 */
#include "support/split_residency_fixture.h"
#include <limits.h>

#include "item/items.h"
#include "block/redstone_piston.h" // RS_PISTON_BASE_ID
#include "platform/audio.h"

#include <inttypes.h>
#include <math.h> /* bucket_click_top aim */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The goldens, recorded from a build of this file at the base sha 32e9fba9,
 * where the hooks take no actor. See tmp/jobs/mp-split-0912/s1a1b-report.md
 * for the two command lines and both printed values. */
/* Re-baselined 2026-09-26 (blist-0926, Astra ruling 8: attributed before recapture; per-RPC eid-decoded dumps diffed index-less, base vs stew tip, tmp/jobs/split-dimensions-plan-0924/out/BLIST-0926.md): stew-0925 (1266c2e0) makes mushroom stew RETURN the empty bowl to the held slot -- ItemSoup.onItemRightClick:8-10 returns `new ItemStack(Item.bowlEmpty)` after ItemFood:14-17 consumed the stew, and PlayerController.sendUseItem:47-56 stores the returned stack in the selected slot (eat() above asserts that slot for stew). STATE first moves at step 32 "eat mushroom stew": hotbar slot 36 holds 281/1 (bowl) instead of 282/1. RPC (3069 -> 3064 rows, index-less diff = 9 rows): rows #2654-2655 CRPC_SPAWN_ITEM eid 9 item 281 at (0,4,0) + CRPC_INVENTORY_SLOT slot 36 = 282/1 become one CRPC_INVENTORY_SLOT slot 36 = 281/1; rows #2662-2664 CRPC_INVENTORY_SLOT slot 37 = 281/1, CRPC_SOUND random.pop (pitch 3.0194) and CRPC_PICKUP_ITEM, and #2677 CRPC_ENTITY_DESTROY -- the host COLLECTING that bowl entity during the step's settle (EntityItem.onCollideWithPlayer) -- vanish with the entity; and one SOUND_MOB_PIG idle grunt of pig eid 6 moves from step 35 (#2902, pitch 1.0695) to step 32 (#2684, pitch 1.0023): the collected bowl's pop pitch drew twice from server_local_sound_rng (Beta ((r-r)*0.7F+1.0F)*2.0F), so the sound-stream `nextInt(1000) < livingSoundTime++` gate (EntityLiving.onEntityUpdate:105-111) fires for the pig on an earlier roll; its own two pitch draws re-align the stream (the next two grunts, #2762/#2844, are pitch-identical to the base's #2766/#2848) and its livingSoundTime reset to -120 removes the base's step-35 grunt. Every CRPC_ENTITY_MOVE row (eid + position) is byte-identical: no simulation change. Previous values: STATE 0xdb90d334b045729f, RPC 0xb93b28e28933f7d0. */
/* Re-pinned 2026-10-09: lead evidence acceptance under GOLDEN-0db9
 * (ASTRA-GOLDEN-0db9.md), regress-1008/RB08-RESULT.md, both rows
 * FULLY-ATTRIBUTED and ACCEPTED. P reproduces the old pins; P+C and
 * candidate complete STATE/RPC streams are byte-identical, eight repeats.
 * C SHA256: 9277269772c1d4426a8beb6d4618a4f44d87d2f75bf9c7830c7bcf77f30a1448.
 * Old pins: STATE 0xc2f065e26dadf514, RPC 0xfa2ad4cf5315cde0;
 * old record counts: 35 STATE / 3088 RPC.
 * Accepted record counts: 35 STATE / 3023 RPC.
 * Fixtures, observers, determinism and assertions remain unchanged. */
#define GOLDEN_STATE UINT64_C(0xf904786df2c8786e)
/* GOLDEN_RPC re-baselined 2026-09-24 (next-gaps-0924 M2b): bisected across the M2 first-parent merges (tmp/jobs/next-gaps-0924/m2b-bisect-*.log): W1C item physics (deb39952, EntityItem.java:45-69 per-entity Random/pushOut/bounce), W1A player physics + suffocation (d4cf78ee, EntityLiving.moveEntityWithHeading:455-497, Entity.isEntityInsideOpaqueBlock:851-863) and W1B mob movement (7c7a8ff2, EntityLiving.moveEntityWithHeading:479-497) moved the observed stream; every value is the one printed by this file at 0b33e352 and again after the F1b/W1E-e merges. The STATE golden is unchanged. */
/* Re-baselined 2026-09-25 (next-gaps-0924 GOLDEN-0925, Astra kickoff ruling 8): bisected commit-by-commit inside the W2-MOVE wave on main 5f03e259 (tmp/jobs/next-gaps-0924/bisect-move.log, bisect-move2.log, bisect-column.log; per-field dumps in golden-dump/): the same two rules as test_host_damage_golden (this fixture's sheep pack sits at (-19.5, 4, 53.5)/(-19.5, 4, 52.5), the host_damage rows translated by (-3, 0, -1)): aa8bfa1c's pack push at tick 61 (first HASHED divergence, golden-dump/base- vs p4-block_hooks_actor_golden.txt: sheep 3's footstep CRPC_SOUND position at RPC #1092 (script step 7), x -19.4021 -> -19.4405, z 51.8351 -> 51.8305; the extra CRPC_ENTITY_MOVE rows are sheep 1's, 316 -> 352 over both runs -- G1 review F4 corrected the earlier 'extra move at tick 104, sheep 3' note), then b0931e6e's exact-face landing (sheep 1 y 4.004537 -> 4.000000, step 5). No later commit moves it. Pinned by tests/test_golden_move_divergence.c S2 + S4; the STATE golden is unchanged; this value is the one this file printed at the integ3 tip dadbffd1. */
/* Re-baselined 2026-09-27 (lane C mob motion units S2, docs/intent/mob-motion-units.md; Astra rulings 2026-09-27): GOLDEN_STATE is UNCHANGED (the player damage/heal pipeline is untouched); only the RPC stream moved, -> 3088 rows. The rows this script emits that depend on MOB motion are the wandering/pushed mobs' CRPC_ENTITY_MOVE rows (type-only in the hash, so a changed COUNT moves it) and their footstep CRPC_SOUND positions: with Beta's vertical physics (move, then (motionY - 0.08D) * 0.98F; jump() (double)0.42F; no engine auto-jump / ySize bank) a mob settles and walks on different ticks and cells. ATTRIBUTION NOTE (Astra ruling 8): the per-row base-vs-S2 dump was NOT produced on this pass -- the host disk was full (a second build tree could not be made); it is listed as an open item in the S2 report. Previous value: 0xd6e32ed74237f8ee. */
/* Old RPC pin at 090d8948a: 0xdfc7bcd8f2a558ed. Retain STATE unchanged.
 * P + exact f83b1074e/c5927d951/8f59aac0a code patches reproduces the
 * complete stream (tests/oracle/integ1011_lineage/pr03.py, residual 0).
 * Every tick/entity/phase envelope: pr03_ledger.py; eight full normal runs. */
/* G9X-6 re-record (REGRESS-1015, maintainer re-record ruling 2026-10-14): old pin 0xa3f5e04e93e909dc (GOLDEN_STATE unchanged). With the G9X-6 post-drain refresh disabled the old pin reproduces exactly; with it the fold stream differs ONLY by 44 (22 per run) inserted CRPC_CHUNK republishes of already-sent columns, each carrying a stored Sky/Block light change (Beta publishes every light store, CFR World.java:512-517). tmp/jobs/loop/REGRESS-1015/RERECORD.md. */
#define GOLDEN_RPC UINT64_C(0x31d96e09aba797fc)

#define FNV_BASIS UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)

/* Superflat (the fixture's world): bedrock 0, dirt 1-2, grass 3, air above. */
#define GROUND_Y 3
/* The hashed window. Wide enough in +X for the whole action row, and in Z/Y
 * for the water bucket's spread and the two-cell door/bed footprints. */
#define RX0 (-8)
#define RX1 40
#define RY0 0
#define RY1 9
#define RZ0 (-8)
#define RZ1 8

#define LOAD_TICKS 400u
/* Ticks settled between steps: enough for the fluid/fire/redstone scheduled
 * ticks a step arms to run to completion inside the SAME step's digest. */
#define SETTLE_TICKS 12u

/* Write-only attribution records; assertions and folded bytes are unchanged.
 * CW_ATTR_DUMP=<prefix> writes the complete stream and a field/offset map. */
static FILE* attr_bytes;
static FILE* attr_map;
static uint64_t attr_offset;
static void attr_record(const void* data, size_t n, const char* field,
						const char* function) {
	const char* prefix = getenv("CW_ATTR_DUMP");
	if(!prefix || !prefix[0]) return;
	if(!attr_bytes) {
		char path[1024];
		int written = snprintf(path, sizeof(path), "%s.bin", prefix);
		assert(written > 0 && (size_t)written < sizeof(path));
		attr_bytes = fopen(path, "wb"); assert(attr_bytes);
		written = snprintf(path, sizeof(path), "%s.map", prefix);
		assert(written > 0 && (size_t)written < sizeof(path));
		attr_map = fopen(path, "w"); assert(attr_map);
	}
	assert(fprintf(attr_map, "%" PRIu64 " %zu %s %s\n",
				   attr_offset, n, function, field) > 0);
	assert(fwrite(data, 1, n, attr_bytes) == n);
	attr_offset += n;
}

static uint64_t hash_bytes_at(uint64_t hash, const void* data, size_t n,
                              const char* field, const char* function) {
	attr_record(data, n, field, function);
	const uint8_t* p = data;
	while(n--) {
		hash ^= *p++;
		hash *= FNV_PRIME;
	}
	return hash;
}
#define hash_bytes(h, data, n) \
	hash_bytes_at((h), (data), (n), #data, __func__)


#define HASH_FIELD(h, v) ((h) = hash_bytes((h), &(v), sizeof(v)))

/* ---- the RPC-ORDER digest ------------------------------------------------
 * Folded from the send observer, in emission order. `rpc_count` is reported
 * (not hashed twice) so the report can show traffic actually flowed. */
static uint64_t s_rpc_hash;
static unsigned long s_rpc_count;

#include "support/lineage_rpc_trace.h"

static uint64_t hash_item(uint64_t h, const struct item_data* it) {
	HASH_FIELD(h, it->id);
	HASH_FIELD(h, it->count);
	HASH_FIELD(h, it->durability);
	return h;
}

static void observe_send(const struct client_rpc* call, void* user) {
	lineage_rpc_emit(call, user);
	(void)user;
	s_rpc_count++;
	/* Every RPC contributes its type, so the ORDER of the whole stream is
	 * pinned even for arms whose payload is not decoded below. */
	int type = (int)call->type;
	s_rpc_hash = hash_bytes(s_rpc_hash, &type, sizeof(type));
	switch(call->type) {
	case CRPC_INVENTORY_SLOT:
		HASH_FIELD(s_rpc_hash, call->payload.inventory_slot.window);
		HASH_FIELD(s_rpc_hash, call->payload.inventory_slot.slot);
		s_rpc_hash = hash_item(s_rpc_hash, &call->payload.inventory_slot.item);
		break;
	case CRPC_WINDOW_ITEMS: {
		HASH_FIELD(s_rpc_hash, call->payload.window_items.window);
		uint16_t n = call->payload.window_items.count;
		HASH_FIELD(s_rpc_hash, n);
		for(uint16_t k = 0; k < n && k < CRPC_WINDOW_ITEMS_MAX_SLOTS; k++)
			s_rpc_hash
				= hash_item(s_rpc_hash, &call->payload.window_items.items[k]);
		break;
	}
	case CRPC_OPEN_WINDOW: {
		HASH_FIELD(s_rpc_hash, call->payload.window_open.window);
		int wt = (int)call->payload.window_open.type;
		HASH_FIELD(s_rpc_hash, wt);
		HASH_FIELD(s_rpc_hash, call->payload.window_open.slot_count);
		break;
	}
	case CRPC_PLAYER_SET_HEALTH:
		HASH_FIELD(s_rpc_hash, call->payload.player_set_health.health);
		break;
	case CRPC_SET_BLOCK:
		HASH_FIELD(s_rpc_hash, call->payload.set_block.x);
		HASH_FIELD(s_rpc_hash, call->payload.set_block.y);
		HASH_FIELD(s_rpc_hash, call->payload.set_block.z);
		{
			uint8_t ty = call->payload.set_block.block.type;
			uint8_t md = call->payload.set_block.block.metadata;
			HASH_FIELD(s_rpc_hash, ty);
			HASH_FIELD(s_rpc_hash, md);
		}
		break;
	case CRPC_SOUND:
		HASH_FIELD(s_rpc_hash, call->payload.sound.sound_id);
		for(unsigned k = 0; k < 3; k++)
			HASH_FIELD(s_rpc_hash, call->payload.sound.pos[k]);
		HASH_FIELD(s_rpc_hash, call->payload.sound.vol);
		HASH_FIELD(s_rpc_hash, call->payload.sound.pitch);
		HASH_FIELD(s_rpc_hash, call->payload.sound.ui);
		break;
	case CRPC_SPAWN_ITEM:
		HASH_FIELD(s_rpc_hash, call->payload.spawn_item.entity_id);
		s_rpc_hash = hash_item(s_rpc_hash, &call->payload.spawn_item.item);
		for(unsigned k = 0; k < 3; k++) {
			HASH_FIELD(s_rpc_hash, call->payload.spawn_item.pos[k]);
			HASH_FIELD(s_rpc_hash, call->payload.spawn_item.vel[k]);
		}
		break;
	case CRPC_SIGN_TEXT:
		HASH_FIELD(s_rpc_hash, call->payload.sign_text.x);
		HASH_FIELD(s_rpc_hash, call->payload.sign_text.y);
		HASH_FIELD(s_rpc_hash, call->payload.sign_text.z);
		s_rpc_hash = hash_bytes(s_rpc_hash, call->payload.sign_text.grid, 64);
		break;
	case CRPC_SIGN_TEXT_REMOVED:
		HASH_FIELD(s_rpc_hash, call->payload.sign_text_removed.x);
		HASH_FIELD(s_rpc_hash, call->payload.sign_text_removed.y);
		HASH_FIELD(s_rpc_hash, call->payload.sign_text_removed.z);
		break;
	case CRPC_HUD_MESSAGE:
		s_rpc_hash = hash_bytes(s_rpc_hash, call->payload.hud_message,
								sizeof(call->payload.hud_message));
		break;
	case CRPC_PLAYER_HOTBAR_SLOT:
		HASH_FIELD(s_rpc_hash, call->payload.player_hotbar_slot.slot);
		break;
	default:
		/* Type ordinal only -- includes the three pointer-carrying arms. */
		break;
	}
	clin_drain_discard();
}

/* ---- the STATE digest ---------------------------------------------------- */
static uint64_t region_hash(uint64_t h, struct server_local* s) {
	for(int x = RX0; x <= RX1; x++)
		for(int z = RZ0; z <= RZ1; z++)
			for(int y = RY0; y <= RY1; y++) {
				struct block_data blk;
				bool ok = server_world_get_block(&s->world, x, y, z, &blk);
				HASH_FIELD(h, ok);
				if(!ok)
					continue;
				/* struct block_data's metadata/light are BIT-FIELDS: widen
				 * to addressable locals before folding. */
				uint8_t ty = blk.type, md = blk.metadata;
				uint8_t sl = blk.sky_light, tl = blk.torch_light;
				HASH_FIELD(h, ty);
				HASH_FIELD(h, md);
				HASH_FIELD(h, sl);
				HASH_FIELD(h, tl);
			}
	return h;
}

static uint64_t actor_hash(uint64_t h, struct server_local* s) {
	const struct server_player* p = &s->player;
	HASH_FIELD(h, p->x);
	HASH_FIELD(h, p->y);
	HASH_FIELD(h, p->z);
	HASH_FIELD(h, p->rx);
	HASH_FIELD(h, p->ry);
	HASH_FIELD(h, p->on_ground);
	HASH_FIELD(h, p->health);
	HASH_FIELD(h, p->oxygen);
	HASH_FIELD(h, p->sleeping);
	/* Which window is open, not its address: a pointer would be process-
	 * dependent. */
	bool plain = p->active_inventory == &p->inventory;
	bool none = p->active_inventory == NULL;
	HASH_FIELD(h, plain);
	HASH_FIELD(h, none);
	size_t hotbar = inventory_get_hotbar(&s->player.inventory);
	HASH_FIELD(h, hotbar);
	for(size_t slot = 0; slot < INVENTORY_SIZE; slot++) {
		struct item_data item;
		bool present = inventory_get_slot(&s->player.inventory, slot, &item);
		HASH_FIELD(h, present);
		if(present)
			h = hash_item(h, &item);
	}
	return h;
}

static uint64_t s_state_hash;
static unsigned s_step;

static void settle(struct server_local* s) {
	for(unsigned t = 0; t < SETTLE_TICKS; t++) {
		server_local_test_update_once(s);
		clin_drain_discard();
	}
}

/* One scripted step: settle, then fold. */
static void step_done(struct server_local* s, const char* what) {
	(void)what;
	settle(s);
	s_step++;
	HASH_FIELD(s_state_hash, s_step);
	s_state_hash = region_hash(s_state_hash, s);
	s_state_hash = actor_hash(s_state_hash, s);
}

/* ---- drivers ------------------------------------------------------------- */
static uint8_t type_at(struct server_local* s, int x, int y, int z) {
	struct block_data blk;
	assert(server_world_get_block(&s->world, x, y, z, &blk));
	return blk.type;
}

static uint8_t meta_at(struct server_local* s, int x, int y, int z) {
	struct block_data blk;
	assert(server_world_get_block(&s->world, x, y, z, &blk));
	return blk.metadata;
}

/* Put `it` in the selected hotbar slot; the dispatch reads exactly this. */
static void hold(struct server_local* s, uint16_t id, uint8_t count) {
	inventory_set_hotbar(&s->player.inventory, 0);
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR,
					   (struct item_data) {.id = id, .count = count});
	struct item_data held;
	assert(inventory_get_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR,
							  &held)
		   && held.id == id && held.count == count);
}

static void hold_nothing(struct server_local* s) {
	inventory_set_hotbar(&s->player.inventory, 0);
	inventory_clear_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR);
	struct item_data none;
	assert(!inventory_get_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR,
							   &none));
}

static void right_click(struct server_local* s, int x, int y, int z,
						enum side side) {
	struct server_rpc call = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = side}};
	server_local_test_process_rpc(s, &call);
}

/* A bucket click the way a Beta player makes it: ItemBucket.onItemRightClick
 * ignores the clicked block/face and runs its OWN 5.0-block ray from the eye
 * [ItemBucket.java:12-28]. The host stands far from the target cell, so for
 * the one click the body stands one block west of the cell on the ground and
 * looks at the top-face centre; the pose is restored at once. */
static void bucket_click_top(struct server_local* s, int x, int y, int z) {
	double px = s->player.x, py = s->player.y, pz = s->player.z;
	float prx = s->player.rx, pry = s->player.ry;
	s->player.x = x - 0.5;
	s->player.y = y + 1 + 1.62;
	s->player.z = z + 0.5;
	double dx = (x + 0.5) - s->player.x, dy = (y + 1.0) - s->player.y,
		   dz = (z + 0.5) - s->player.z;
	/* Beta look vector: (-sin yaw cos pitch, -sin pitch, cos yaw cos pitch). */
	s->player.rx = (float)(atan2(-dx, dz) * 180.0 / 3.141592653589793);
	s->player.ry = (float)(atan2(-dy, sqrt(dx * dx + dz * dz)) * 180.0
		/ 3.141592653589793);
	right_click(s, x, y, z, SIDE_TOP);
	s->player.x = px; s->player.y = py; s->player.z = pz;
	s->player.rx = prx; s->player.ry = pry;
}

static void close_window(struct server_local* s, uint8_t window) {
	struct server_rpc call
		= {.type = SRPC_WINDOW_CLOSE, .payload.window_close = {.window = window}};
	server_local_test_process_rpc(s, &call);
	assert(s->player.active_inventory == &s->player.inventory);
}

/* Place the held block-or-item ON TOP of (x,GROUND_Y,z) and prove it landed. */
static void place_on_ground(struct server_local* s, int x, int z,
							uint16_t held, uint8_t expect) {
	hold(s, held, 4);
	assert(type_at(s, x, GROUND_Y + 1, z) == BLOCK_AIR);
	right_click(s, x, GROUND_Y, z, SIDE_TOP);
	assert(type_at(s, x, GROUND_Y + 1, z) == expect);
}

/* Open a container window on (x,y,z) and close it again, proving both ends. */
static void activate_container(struct server_local* s, int x, int y, int z,
							   uint8_t window) {
	hold_nothing(s);
	assert(s->player.active_inventory == &s->player.inventory);
	right_click(s, x, y, z, SIDE_TOP);
	assert(s->player.active_inventory != &s->player.inventory);
	close_window(s, window);
}

/* Eat one `food`, proving the heal landed on the host actor. */
static void eat(struct server_local* s, uint16_t food) {
	assert(server_local_set_player_health(s, MAX_PLAYER_HEALTH / 2));
	short before = s->player.health;
	hold(s, food, 2);
	/* Clicking the grass under the actor: no block handles the click, so the
	 * food's own onItemPlace runs -- the Beta eat path. */
	right_click(s, 0, GROUND_Y, 0, SIDE_TOP);
	assert(s->player.health > before);
	/* ItemFood consumes exactly one on a true return, so the held stack must
	 * have gone 2 -> 1: proof the hook ran AND the dispatch consumed. */
	struct item_data left;
	assert(inventory_get_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR,
							  &left));
	if(food == ITEM_MUSHROOM_STEW) {
		/* Stew is the one food whose use RETURNS a stack: ItemSoup.
		 * onItemRightClick:8-10 hands back a fresh empty bowl after
		 * ItemFood consumed the stew, and PlayerController.sendUseItem:47-56
		 * stores that returned stack in the selected slot [blist-0926]. */
		assert(left.id == ITEM_BOWL && left.count == 1);
	} else {
		assert(left.id == food && left.count == 1);
	}
}

struct result {
	uint64_t state, rpc;
	unsigned steps;
	unsigned long rpcs;
};

static struct result run(void) {
	s_state_hash = FNV_BASIS;
	s_rpc_hash = FNV_BASIS;
	s_rpc_count = 0;
	s_step = 0;

	struct server_local* s = split_fixture();
	/* Pin every stream the simulation draws from. */
	s->rand_src = (struct random_gen) {.seed = 24680};
	/* F4-RNG: block drops and body hurt pitch have separate runtime seeds. */
	jrand_set_seed(&s->worlds[0].rand, 4242);
	server_local_test_seed_body_rand(s, &s->player, 13579);
	*server_local_sound_rng() = (struct random_gen) {.seed = 13579};
	*audio_cosmetic_rng() = (struct random_gen) {.seed = 2468};
	server_local_test_seed_math_random(1357);
	/* ...and replace every wall-clock governor with a count governor. */
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	/* The far-column gen batch is the last wall-clock governor: under host
	 * load FAR_GEN_BUDGET_MS cut a batch short and reordered the far-chunk
	 * RPC stream (flaky rpc hash on Linux). Never expire it here, so every
	 * batch runs to its FAR_SEND_BUDGET count cap. */
	s->far_gen_budget_ms = INT_MAX;
	assert(s->autosave_ticks == 0);
	s->player.active_inventory = &s->player.inventory;
	inventory_set_hotbar(&s->player.inventory, 0);

	clin_test_set_rpc_observer(observe_send, s);
	/* Stream the terrain in with the simulation held (input_ack_pending). */
	s->input_ack_pending = 1;
	s->player.finished_loading = true;
	for(unsigned load = 0; load < LOAD_TICKS; load++) {
		server_local_test_update_once(s);
		clin_drain_discard();
	}
	assert(s->world.current_tick == 0);
	s->input_ack_pending = 0;
	/* Every acted-on column must be resident, or a step would silently no-op
	 * on an unloaded chunk instead of running its hook. */
	for(int x = RX0; x <= RX1; x += 8)
		assert(type_at(s, x, GROUND_Y, 0) == BLOCK_GRASS);

	/* ---- BLOCK classes ---------------------------------------------------
	 * Chest: yaw-derived facing metadata (block_chest.c onItemPlace) then the
	 * window open (onRightClick). */
	place_on_ground(s, 2, 0, BLOCK_CHEST, BLOCK_CHEST);
	step_done(s, "chest place (facing from the actor yaw)");
	activate_container(s, 2, GROUND_Y + 1, 0, WINDOWC_CHEST);
	step_done(s, "chest activate + close");

	/* Furnace: facing metadata + the TE register; the window open stays
	 * host-only by design, and the host IS the actor here. */
	place_on_ground(s, 4, 0, BLOCK_FURNACE, BLOCK_FURNACE);
	step_done(s, "furnace place (facing from the actor yaw)");
	activate_container(s, 4, GROUND_Y + 1, 0, WINDOWC_FURNACE);
	step_done(s, "furnace activate + close");

	/* Workbench: activation only (block_place_default places it). */
	place_on_ground(s, 6, 0, BLOCK_WORKBENCH, BLOCK_WORKBENCH);
	step_done(s, "workbench place");
	activate_container(s, 6, GROUND_Y + 1, 0, WINDOWC_CRAFTING);
	step_done(s, "workbench activate + close");

	/* Dispenser: facing metadata + TE + window. */
	place_on_ground(s, 8, 0, BLOCK_DISPENSER, BLOCK_DISPENSER);
	step_done(s, "dispenser place (facing from the actor yaw)");
	activate_container(s, 8, GROUND_Y + 1, 0, WINDOWC_DISPENSER);
	step_done(s, "dispenser activate + close");

	/* Sign ITEM (items_object.h item_sign_onItemPlace): a TOP click makes a
	 * sign POST whose metadata is the actor's yaw index, and Beta's
	 * displayGUIEditSign tail opens the OSK immediately. */
	hold(s, ITEM_SIGN, 2);
	right_click(s, 10, GROUND_Y, 0, SIDE_TOP);
	assert(type_at(s, 10, GROUND_Y + 1, 0) == BLOCK_SIGN_POST);
	assert(s->player.active_inventory != &s->player.inventory);
	close_window(s, WINDOWC_SIGN);
	step_done(s, "sign item place (yaw metadata) + auto-OSK + close");
	activate_container(s, 10, GROUND_Y + 1, 0, WINDOWC_SIGN);
	step_done(s, "sign activate + close");

	/* Wooden door ITEM: facing from (yaw+180), both cells obstruction-swept. */
	hold(s, ITEM_DOOR_WOOD, 2);
	right_click(s, 12, GROUND_Y, 0, SIDE_TOP);
	assert(type_at(s, 12, GROUND_Y + 1, 0) == BLOCK_DOOR_WOOD);
	assert(type_at(s, 12, GROUND_Y + 2, 0) == BLOCK_DOOR_WOOD);
	step_done(s, "wooden door item place (yaw+180 facing)");
	/* Door activation: the shared open-bit chokepoint, including the
	 * playability nudge that now targets the acting actor. */
	{
		uint8_t before = meta_at(s, 12, GROUND_Y + 1, 0);
		hold_nothing(s);
		right_click(s, 12, GROUND_Y + 1, 0, SIDE_FRONT);
		assert(meta_at(s, 12, GROUND_Y + 1, 0) != before);
	}
	step_done(s, "door activate (open bit flips, actor not moved)");

	/* Iron door ITEM: the sibling facing formula; its activation is Beta's
	 * consume-and-do-nothing arm. */
	hold(s, ITEM_DOOR_IRON, 2);
	right_click(s, 14, GROUND_Y, 0, SIDE_TOP);
	assert(type_at(s, 14, GROUND_Y + 1, 0) == BLOCK_DOOR_IRON);
	step_done(s, "iron door item place");
	hold_nothing(s);
	right_click(s, 14, GROUND_Y + 1, 0, SIDE_FRONT);
	step_done(s, "iron door activate (consumed, no action)");

	/* Stairs + pumpkin: facing from the actor's dx/dz to the placed cell. */
	place_on_ground(s, 16, 0, BLOCK_WOODEN_STAIRS, BLOCK_WOODEN_STAIRS);
	step_done(s, "stairs place (dx/dz from the actor)");
	place_on_ground(s, 18, 0, BLOCK_PUMPKIN, BLOCK_PUMPKIN);
	step_done(s, "pumpkin place (dx/dz from the actor)");

	/* Piston: determineOrientation from the actor's eye + yaw. */
	place_on_ground(s, 20, 0, RS_PISTON_BASE_ID, RS_PISTON_BASE_ID);
	step_done(s, "piston place (determineOrientation from the actor)");

	/* Repeater ITEM: facing from the actor yaw, via redstone_repeater_place's
	 * own obstruction sweep. */
	hold(s, ITEM_REDSTONE_REPEATER, 2);
	right_click(s, 22, GROUND_Y, 0, SIDE_TOP);
	assert(type_at(s, 22, GROUND_Y + 1, 0) != BLOCK_AIR);
	step_done(s, "repeater item place (yaw facing)");

	/* Redstone dust ITEM: block_place_default + the wire settle flood. */
	hold(s, ITEM_REDSTONE, 4);
	right_click(s, 24, GROUND_Y, 0, SIDE_TOP);
	assert(type_at(s, 24, GROUND_Y + 1, 0) != BLOCK_AIR);
	step_done(s, "redstone dust place + wire settle");

	/* Bed ITEM: two cells, both obstruction-swept, head direction from the
	 * actor yaw with NO +180. */
	hold(s, ITEM_BED, 2);
	right_click(s, 1, GROUND_Y, 1, SIDE_TOP);
	assert(type_at(s, 1, GROUND_Y + 1, 1) == BLOCK_BED);
	step_done(s, "bed item place (yaw head direction, both cells swept)");
	/* Bed ACTIVATION, in reach of the actor: daytime, so Beta refuses with
	 * tile.bed.noSleep and the world time is untouched. Exercises the
	 * actor-derived reach gate and the sleeping read. */
	{
		uint64_t time_before = s->world_time;
		hold_nothing(s);
		right_click(s, 1, GROUND_Y + 1, 1, SIDE_TOP);
		/* The click is handled (no fall-through place) and refused: not
		 * sleeping, the bed still standing, and -- since nothing ticked
		 * between the two reads -- the world clock untouched, which is what a
		 * non-faithful instant night-skip would have moved. */
		assert(!s->player.sleeping);
		assert(s->world_time == time_before);
		assert(type_at(s, 1, GROUND_Y + 1, 1) == BLOCK_BED);
	}
	step_done(s, "bed activate in reach (daytime refusal, no sleep)");

	/* Cake: item_cake.c places block 92 through block_place_default, then the
	 * activation eats a slice and heals the acting actor. */
	hold(s, ITEM_CAKE, 1);
	right_click(s, 26, GROUND_Y, 0, SIDE_TOP);
	assert(type_at(s, 26, GROUND_Y + 1, 0) == BLOCK_CAKE);
	step_done(s, "cake item place");
	{
		assert(server_local_set_player_health(s, MAX_PLAYER_HEALTH / 2));
		short before = s->player.health;
		uint8_t bites = meta_at(s, 26, GROUND_Y + 1, 0);
		hold_nothing(s);
		right_click(s, 26, GROUND_Y + 1, 0, SIDE_TOP);
		assert(s->player.health > before);
		assert(meta_at(s, 26, GROUND_Y + 1, 0) == bites + 1);
	}
	step_done(s, "cake activate (host heal + bite metadata)");

	/* ---- ITEM classes whose hooks read the acting player ----------------- */
	eat(s, ITEM_APPLE);
	step_done(s, "eat apple");
	eat(s, ITEM_APPLE_GOLDEN);
	step_done(s, "eat golden apple");
	eat(s, ITEM_BREAD);
	step_done(s, "eat bread");
	eat(s, ITEM_COOKIE);
	step_done(s, "eat cookie");
	eat(s, ITEM_FISH);
	step_done(s, "eat raw fish");
	eat(s, ITEM_FISH_COOKED);
	step_done(s, "eat cooked fish");
	eat(s, ITEM_PORKCHOP);
	step_done(s, "eat raw porkchop");
	eat(s, ITEM_PORKCHOP_COOKED);
	step_done(s, "eat cooked porkchop");
	eat(s, ITEM_MUSHROOM_STEW);
	step_done(s, "eat mushroom stew (bowl returned to the slot)");

	/* ---- actor-free item-use classes, for stream-order coverage ---------- */
	hold(s, ITEM_WOOD_HOE, 1);
	right_click(s, 28, GROUND_Y, 0, SIDE_TOP);
	assert(type_at(s, 28, GROUND_Y, 0) == BLOCK_FARMLAND);
	step_done(s, "hoe till (onUseOnBlock)");

	hold(s, ITEM_BUCKET_WATER, 1);
	bucket_click_top(s, 34, GROUND_Y, 0);
	assert(type_at(s, 34, GROUND_Y + 1, 0) != BLOCK_AIR);
	step_done(s, "water bucket empty (onUseOnBlock) + fluid settle");

	hold(s, ITEM_SEED, 4);
	right_click(s, 28, GROUND_Y, 0, SIDE_TOP);
	step_done(s, "seeds on farmland");

	clin_test_set_rpc_observer(NULL, NULL);

	struct result r = {.state = s_state_hash, .rpc = s_rpc_hash,
					   .steps = s_step, .rpcs = s_rpc_count};
	/* Non-vacuity: the script must have LEFT the world changed. */
	assert(type_at(s, 2, GROUND_Y + 1, 0) == BLOCK_CHEST);
	assert(type_at(s, 12, GROUND_Y + 1, 0) == BLOCK_DOOR_WOOD);
	assert(type_at(s, 28, GROUND_Y, 0) == BLOCK_FARMLAND);
	assert(r.steps >= 30 && r.rpcs > 0);

	printf("hooks_actor state=%016" PRIx64 " rpc=%016" PRIx64
		   " steps=%u rpcs=%lu tick=%u\n",
		r.state, r.rpc, r.steps, r.rpcs, s->world.current_tick);
	fflush(stdout);
	split_destroy(s);
	if(attr_bytes) { assert(fflush(attr_bytes) == 0); assert(fflush(attr_map) == 0); }
	return r;
}

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();

	struct result a = run();
	/* Determinism first: a golden is only assertable if the script is
	 * reproducible in the SAME process. If this fails, the hash is noise and
	 * no cross-tree comparison means anything. */
	struct result b = run();
	if(a.state != b.state || a.rpc != b.rpc) {
		fprintf(stderr,
			"test_block_hooks_actor_golden: NON-DETERMINISTIC "
			"state %016" PRIx64 "/%016" PRIx64 " rpc %016" PRIx64
			"/%016" PRIx64 "\n",
			a.state, b.state, a.rpc, b.rpc);
		assert(false && "scripted host sequence is not reproducible");
	}
	printf("PASS reproducible: two runs agree (steps=%u rpcs=%lu)\n", a.steps,
		   a.rpcs);
	fflush(stdout);

	if(a.state != GOLDEN_STATE) {
		fprintf(stderr,
			"test_block_hooks_actor_golden: STATE %016" PRIx64
			" != GOLDEN %016" PRIx64 " (base 32e9fba9)\n",
			a.state, GOLDEN_STATE);
		assert(false && "host STATE diverged from the pre-actor golden");
	}
	if(a.rpc != GOLDEN_RPC) {
		fprintf(stderr,
			"test_block_hooks_actor_golden: RPC-ORDER %016" PRIx64
			" != GOLDEN %016" PRIx64 " (base 32e9fba9)\n",
			a.rpc, GOLDEN_RPC);
		assert(false && "host RPC order/payloads diverged from the golden");
	}
	printf("PASS host STATE golden %016" PRIx64 "\n", GOLDEN_STATE);
	printf("PASS host RPC-ORDER golden %016" PRIx64 "\n", GOLDEN_RPC);
	fflush(stdout);
	puts("PASS test_block_hooks_actor_golden: actor-parameterized block/item "
		 "hooks are byte-identical on the host path (S1-a1b)");
	return 0;
}
