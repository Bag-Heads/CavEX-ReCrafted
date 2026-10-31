/*
	Beta 1.7.3 EntityBoat: buoyancy, drag, land-crash destruction, hit-damage
	accumulation + the STRICT >40 threshold, mount/dismount via interact, and
	the exact destruction drop multiset on BOTH destruction paths.

	GOLDEN-FIRST: every expected number below is hand-derived from the
	740c583 decompile arithmetic (EntityBoat.attackEntityFrom / onUpdate,
	World.isAABBInMaterial), never by running this project's own boat code.
	See BOAT_SLICE_NOTES.md for the citation of each constant.

	Beta source for the arithmetic pinned here:
	  attackEntityFrom(source, amount):
	    boatRockDirection = -boatRockDirection; boatTimeSinceHit = 10;
	    boatCurrentDamage += amount * 10; setBeenAttacked();
	    if (boatCurrentDamage > 40) { dismount; drop 3 planks, 2 sticks; die; }
	  onUpdate: if(boatTimeSinceHit>0) --; if(boatCurrentDamage>0) --;
	    buoyancy: 5 slices; for i in 0..4:
	      slice = boundingBox sliced in Y, offset -0.125, tested via
	      World.isAABBInMaterial(slice, Material.water), += 1/5 per hit.
	    if(submerged<1.0) motionY += 0.04*(2*submerged-1);
	    else { if(motionY<0) motionY/=2; motionY += 0.007; }
	    moveEntity(motionX,motionY,motionZ);
	    speed = sqrt(motionX^2+motionZ^2); // POST-move (axis-zeroed-if-blocked)
	    if (isCollidedHorizontally && speed>0.15) { die; drop 3 planks, 2 sticks; }
	    else { motionX*=0.99; motionY*=0.95; motionZ*=0.99; }
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "entity/entity_boat.h"
#include "entity/combat.h"
#include "game/game_state.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/entity_archive.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "particle.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_boat_env_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define EPS 1.0e-4f

static struct server_local s;
static volatile int g_drain_run = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

/* server_world_set_block returns FALSE for a no-op write (Beta
 * setBlockAndMetadataWithNotify: identical id+metadata -> no lighting/
 * onBlockAdded/notify) -- a legitimate, harmless outcome whenever a helper
 * "clear to air" call lands on a cell that is already air. Only the actual
 * post-condition (server_world_get_block reads back the requested value)
 * is asserted; the return value itself is not a pass/fail signal here. */
static void set_block(int x, int y, int z, int type, int meta) {
	(void)server_world_set_block(&s.worlds[0], x, y, z,
								 (struct block_data) {
									 .type = (uint8_t)type,
									 .metadata = (uint8_t)(meta & 0x0F),
								 });
	struct block_data check;
	assert(server_world_get_block(&s.world, x, y, z, &check)
		   && check.type == (uint8_t)type
		   && check.metadata == (uint8_t)(meta & 0x0F)
		   && "set_block must leave the cell in the requested state, "
			  "whether or not this particular call changed it");
}

/* --- RPC observer: captures item spawns + destroy so the drop multiset and
 * removal can be checked without racing the drain thread (deterministic;
 * clin_rpc_send publishes synchronously from the calling thread). --- */
#define MAX_OBSERVED_ITEMS 16
static struct {
	unsigned item_spawn_count;
	struct item_data spawned[MAX_OBSERVED_ITEMS];
	unsigned destroy_count;
	uint32_t last_destroyed_id;
	// Rider-tethering section (8): captured server->client spawn/correction
	// RPCs, fed through clin_process to build a REAL client-side boat entity
	// (mirrors test_minecart_authority.c's g_cart_rpc.last_spawn idiom) --
	// hand-constructing the payload would silently drift from whatever
	// server_publish_boat_spawn/boat_send_correction actually emit.
	unsigned spawn_boat_count;
	struct client_rpc last_spawn_boat;
	unsigned boat_correction_count;
	struct client_rpc last_boat_correction;
} g_obs;

static void reset_observer(void) {
	memset(&g_obs, 0, sizeof(g_obs));
}

static void observe_rpc(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_SPAWN_ITEM) {
		if(g_obs.item_spawn_count < MAX_OBSERVED_ITEMS)
			g_obs.spawned[g_obs.item_spawn_count] = call->payload.spawn_item.item;
		g_obs.item_spawn_count++;
		return;
	}
	if(call->type == CRPC_ENTITY_DESTROY) {
		g_obs.destroy_count++;
		g_obs.last_destroyed_id = call->payload.entity_destroy.entity_id;
		return;
	}
	if(call->type == CRPC_SPAWN_BOAT) {
		g_obs.spawn_boat_count++;
		g_obs.last_spawn_boat = *call;
		return;
	}
	if(call->type == CRPC_BOAT_CORRECTION) {
		g_obs.boat_correction_count++;
		g_obs.last_boat_correction = *call;
	}
}

/* Assert the observed item spawns are EXACTLY {3x(BLOCK_PLANKS,1),
 * 2x(ITEM_STICK,1)} as a multiset (order not asserted -- server_local_spawn_item
 * publishes in call order, which this project's own drop helper already fixes
 * as planks-then-sticks, but the MULTISET is the faithfulness contract). */
static void assert_drop_multiset(void) {
	assert(g_obs.item_spawn_count == 5
		   && "boat destruction must drop EXACTLY 5 item entities (3 planks + "
			  "2 sticks), never one stacked spawn of 3 and one of 2");
	unsigned planks = 0, sticks = 0;
	for(unsigned i = 0; i < g_obs.item_spawn_count; i++) {
		assert(g_obs.spawned[i].count == 1
			   && "each drop is its own 1-count dropItemWithOffset call");
		if(g_obs.spawned[i].id == BLOCK_PLANKS)
			planks++;
		else if(g_obs.spawned[i].id == ITEM_STICK)
			sticks++;
		else
			assert(0 && "unexpected dropped item id");
	}
	assert(planks == 3 && sticks == 2
		   && "[740c583: EntityBoat] 3 planks then 2 sticks, both destroy paths");
}

static uint32_t spawn_registered_boat(float x, float y, float z) {
	struct entity* e = server_local_spawn_boat((vec3) {x, y, z}, &s);
	assert(e && e->type == ENTITY_BOAT);
	return e->id;
}

static struct entity* find_entity(uint32_t id) {
	struct entity** pp = dict_entity_get(s.entities, id);
	return pp ? *pp : NULL;
}

static void attack_entity(uint32_t id) {
	struct server_rpc rpc = {
		.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = id,
	};
	server_local_test_process_rpc(&s, &rpc);
}

/* desired_occupied: adversarial-review finding #6 replaced the boat's
 * SRPC_ENTITY_USE handler with idempotent desired-state semantics (matching
 * SRPC_MINECART_RIDE_INTENT's principle) instead of an unconditional
 * !occupied toggle -- a real client now computes this once from its own
 * pre-round-trip local view (screen_ingame.c) and sends the state it WANTS,
 * not a toggle instruction. Callers here must pass it explicitly (true to
 * request mounted, false to request unmounted); a bare designated-initializer
 * default of false would silently turn every "mount" call below into a
 * dismount request against this fixed's new server semantics. */
static void use_entity(uint32_t id, bool desired_occupied) {
	struct server_rpc rpc = {
		.type = SRPC_ENTITY_USE,
		.payload.entity_use.entity_id = id,
		.payload.entity_use.desired_occupied = desired_occupied,
	};
	server_local_test_process_rpc(&s, &rpc);
}

int main(void) {
	test_fixture_dir("test_boat_env_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	clin_test_set_rpc_observer(observe_rpc, NULL);

	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	string_clear(level);
	s.world.local = &s;
	s.worlds[0].session = &s; /* A1a (D1): owning session for services/callbacks */

	/* This file spreads its test zones across world X/Z ~19..91 (deliberate
	 * separation so no two scenarios' blocks/entities can interfere). Load
	 * that range plus Beta's entity-activation +/-32 block margin -- chunks
	 * -1..7 on both axes -- rather than only the directly touched chunks;
	 * block placement AND
	 * entity_try_move/entity_get_block both silently fail-open ("unloaded")
	 * outside a loaded chunk, which would make every out-of-bounds test
	 * scenario a false pass (no collision, no water, ever). All chunks stay
	 * inside region (0,0) (32x32 chunks), matching the single .mcr removed
	 * above. */
	struct server_chunk* sc = NULL;
	for(int cx = -1; cx <= 7; cx++)
		for(int cz = -1; cz <= 7; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	/* Deviation-#18: zero-init health reads as DEAD (crosshair actions --
	 * including every ENTITY_ATTACK boat punch below -- drain inert); the
	 * harness player must be explicitly alive. */
	s.player.health = MAX_PLAYER_HEALTH;
	/* Empty hotbar throughout -> every attack/use is FIST strength 1
	 * [combat.h player_melee_damage default: return 1] so every hit adds
	 * EXACTLY amount*10 = 10 damage -- the clean, hand-derivable unit this
	 * whole file's damage arithmetic is built on. */

	/* =====================================================================
	 * (0) POISONED CONSTRUCTION -- a client boat arrives via CRPC_SPAWN_BOAT
	 * -> s_clin_alloc, a plain malloc. Poison first so a field the
	 * constructor forgets cannot pass by landing on a zero byte (the same
	 * discipline as test_minecart_authority.c's construction check).
	 * ===================================================================== */
	{
		struct entity poisoned;
		memset(&poisoned, 0xBE, sizeof(poisoned));
		entity_boat(1, &poisoned, true, &s.world);
		assert(poisoned.data.boat.damage_taken == 0
			   && poisoned.data.boat.shake_ticks == 0
			   && "hit-damage state must be INITIALISED, not heap garbage");
		assert(poisoned.data.boat.shake_dir == 1
			   && "[740c583: EntityBoat ctor] boatRockDirection starts at 1, "
				  "not 0");
		assert(!poisoned.data.boat.occupied
			   && poisoned.data.boat.occupant_id == 0
			   && !poisoned.data.boat.awaiting_destroy
			   && poisoned.data.boat.correction_ticks == 0);
		assert(!poisoned.data.boat.been_attacked
			   && "[740c583: Entity.<init> \"beenAttacked = false\"] must be "
				  "INITIALISED false, not heap garbage (finding #8)");
		assert(poisoned.width == BOAT_WIDTH && poisoned.height == BOAT_HEIGHT
			   && poisoned.y_offset == BOAT_HEIGHT * 0.5f
			   && "[740c583: EntityBoat ctor] setSize(1.5F,0.6F); yOffset = "
				  "height/2.0F");
	}

	/* =====================================================================
	 * (0b) AUDIT 2026-09-01 #1 (REGRESSION-TODAY, CONFIRMED) -- POISONED
	 * CONSTRUCTION, entity_local_player() side. Production allocates this
	 * entity via a plain malloc (client_interface.c CRPC_SPAWN_WORLD_RESET
	 * -> s_clin_alloc, s_clin_alloc default == malloc, never calloc), so a
	 * field the ctor forgets to set is genuinely indeterminate heap
	 * garbage -- same poisoning discipline as section (0) above, applied to
	 * the field the audit found missing: boat_rider_vel_x/z. Before FIX
	 * 1(a) this block would fail (0xBEBEBEBE reinterpreted as float is
	 * ~-0.3716, never 0.0).
	 * ===================================================================== */
	{
		struct entity poisoned;
		memset(&poisoned, 0xBE, sizeof(poisoned));
		/* entity_local_player() takes the CLIENT struct world* (never the
		 * server's struct server_world -- s.world above), and only stores
		 * the pointer (entity_default_init: `e->world = world;`, never
		 * dereferenced by the ctor) -- an unused local stand-in is exactly
		 * as valid here as a real client world for what this block checks. */
		struct world dummy_world;
		entity_local_player(SERVER_LOCAL_PLAYER_ENTITY_ID, &poisoned,
							&dummy_world);
		assert(poisoned.data.local_player.boat_rider_vel_x == 0.0f
			   && poisoned.data.local_player.boat_rider_vel_z == 0.0f
			   && "a freshly-constructed local player must report rider_vel "
				  "== 0.0 before any mount -- entity_local_player() must "
				  "initialize boat_rider_vel_x/z itself, not rely on the "
				  "allocator handing back zeroed memory (audit 2026-09-01 "
				  "#1, FIX 1(a))");
		/* Sibling fields the ctor already set correctly before this fix --
		 * confirms the new init lines land ALONGSIDE these, not instead of
		 * them. */
		assert(poisoned.data.local_player.jump_ticks == 0
			   && poisoned.data.local_player.walk_dist == 0.0f
			   && poisoned.data.local_player.walk_next_step == 1
			   && "sibling ctor-initialized fields must be unaffected by "
				  "FIX 1(a)");
	}

	/* =====================================================================
	 * (1) DAMAGE ACCUMULATION `+= amount*10` and the STRICT >40 boundary
	 * pair. Fist amount=1 -> +10/hit. Pre-set damage_taken to land the ONE
	 * triggering hit exactly on 40 (survives) and 41 (destroys) --
	 * hand-derived: 30+1*10=40 (NOT >40); 31+1*10=41 (>40).
	 * ===================================================================== */
	{
		uint32_t id = spawn_registered_boat(30.0f, 110.0f, 30.0f);
		s.player.x = 30.0;
		s.player.y = 110.0 + PLAYER_EYE_HEIGHT;
		s.player.z = 30.0;
		struct entity* boat = find_entity(id);
		assert(boat);
		boat->data.boat.damage_taken = 30;
		boat->data.boat.shake_ticks = 0;
		int8_t dir_before = boat->data.boat.shake_dir;
		reset_observer();
		attack_entity(id);
		boat = find_entity(id); /* re-lookup: a destroy would have freed it */
		assert(boat && "damage_taken=30, +10 = 40 (NOT > 40): must SURVIVE");
		assert(boat->data.boat.damage_taken == 40
			   && "[740c583: EntityBoat.attackEntityFrom] boatCurrentDamage "
				  "+= amount*10, amount=1 (fist) -> +10");
		assert(boat->data.boat.shake_ticks == 10
			   && boat->data.boat.shake_dir == (int8_t)-dir_before
			   && "every hit arms 10 shake ticks and flips rock direction, "
				  "survived or not");
		assert(g_obs.item_spawn_count == 0 && "a surviving hit drops nothing");
		assert(boat->data.boat.been_attacked
			   && "adversarial-review finding #8: attackEntityFrom's "
				  "VERBATIM ORDER step 5 (setBeenAttacked()) must run on "
				  "every attack, survived or not");

		boat->data.boat.damage_taken = 31;
		reset_observer();
		attack_entity(id);
		assert(find_entity(id) == NULL
			   && "damage_taken=31, +10 = 41 (STRICT > 40): must be DESTROYED "
				  "(40 survives, 41 destroys is the exact boundary pair)");
		assert_drop_multiset();
	}

	/* =====================================================================
	 * (2) 1/tick DECAY -- sustained SPACED (light) hits never destroy.
	 * Hit for 10, then tick 10 times (full decay to 0) before the next hit,
	 * repeated 5x: damage never exceeds 10 at any point, so it NEVER crosses
	 * the 40 threshold no matter how many times this repeats.
	 * [740c583: EntityBoat.onUpdate 1/tick decay -- "only rapid hits break it"]
	 *
	 * A boat with NO water anywhere near it is NOT weightless: the buoyancy
	 * branch's submerged=0 case (motionY += 0.04*(0*2-1) = -0.04 every tick,
	 * then *0.95 drag) is a real, verified emergent consequence of the
	 * ported formula -- it converges toward a ~-0.76 blocks/tick terminal
	 * fall (fixed point of v' = 0.95*(v-0.04)). Over the 50 ticks this test
	 * runs, an unsupported boat drifts far enough to leave the fixed
	 * player's 36.0 reach gate, which would silently no-op every later
	 * attack and falsely "prove" the decay claim. Rest it on a solid floor
	 * (on_ground stays true -> the pre-move halving damps it near zero) so
	 * this test isolates the damage decay it claims to test.
	 * ===================================================================== */
	{
		for(int fx = 31; fx <= 33; fx++)
			for(int fz = 31; fz <= 33; fz++)
				set_block(fx, 109, fz, BLOCK_STONE, 0);
		/* Local bbox y1=-0.3; resting exactly on the floor's top (Y=110.0)
		 * needs pos[1]-0.3 == 110.0 -> pos[1] = 110.3. */
		uint32_t id = spawn_registered_boat(32.0f, 110.3f, 32.0f);
		s.player.x = 32.0;
		s.player.y = 110.3 + PLAYER_EYE_HEIGHT;
		s.player.z = 32.0;
		for(int cycle = 0; cycle < 5; cycle++) {
			attack_entity(id);
			struct entity* boat = find_entity(id);
			assert(boat && boat->data.boat.damage_taken == 10
				   && "each spaced hit alone only ever reaches 10");
			for(int t = 0; t < 10; t++)
				assert(server_local_test_tick_entities(&s));
			boat = find_entity(id);
			assert(boat && boat->data.boat.damage_taken == 0
				   && "10 ticks fully decays a single 10-point hit back to 0");
		}
		assert(find_entity(id) != NULL
			   && "sustained light (spaced) hits must never destroy the boat");
		for(int fx = 31; fx <= 33; fx++)
			for(int fz = 31; fz <= 33; fz++)
				set_block(fx, 109, fz, BLOCK_AIR, 0);
	}

	/* =====================================================================
	 * (3) DRAG constants applied PER-AXIS (0.99 / 0.95 / 0.99). Y must be
	 * asserted DISTINCTLY -- a test that used the same tolerance for all
	 * three axes could not catch Y using 0.99 by mistake.
	 * Setup: open air (Y=110, far above the Y=100/101 superflat floor and
	 * with no water anywhere in reach), so buoyancy takes the "not fully
	 * submerged" branch with submerged=0, and moveEntity collides with
	 * nothing (vel unchanged by movement itself). Hand trace from
	 * vel=(0.20, 0.10, 0.20), on_ground=false:
	 *   buoyancy: submerged=0 -> motionY += 0.04*(0*2-1) = -0.04
	 *             -> vel[1] = 0.10 - 0.04 = 0.06
	 *   no rider coupling (unoccupied); XZ already within +-0.4 clamp
	 *   on_ground=false -> no halving
	 *   moveEntity: nothing nearby -> no collision, vel unchanged by the move
	 *   speed = sqrt(0.20^2+0.20^2) = 0.28284... ; collided_horizontally=false
	 *     -> NOT a land crash -> drag branch:
	 *   vel[0] = 0.20 * 0.99 = 0.198
	 *   vel[1] = 0.06 * 0.95 = 0.057
	 *   vel[2] = 0.20 * 0.99 = 0.198
	 * ===================================================================== */
	{
		struct entity boat;
		entity_boat(9001, &boat, true, &s.world);
		boat.pos[0] = 60.0f;
		boat.pos[1] = 110.0f;
		boat.pos[2] = 60.0f;
		boat.vel[0] = 0.20f;
		boat.vel[1] = 0.10f;
		boat.vel[2] = 0.20f;
		boat.on_ground = false;
		bool destroyed = boat_advance(&boat);
		assert(!destroyed && "open air, no collision: must not land-crash");
		assert(fabsf(boat.vel[0] - 0.198f) < EPS
			   && "X drag must be 0.99 (0.20*0.99=0.198)");
		assert(fabsf(boat.vel[1] - 0.057f) < EPS
			   && "Y drag must be 0.95, DISTINCT from X/Z (post-buoyancy "
				  "0.06*0.95=0.057, NOT 0.06*0.99=0.0594)");
		assert(fabsf(boat.vel[2] - 0.198f) < EPS
			   && "Z drag must be 0.99 (0.20*0.99=0.198)");
	}

	/* =====================================================================
	 * (4) LAND CRASH fires ONLY when BOTH isCollidedHorizontally AND the
	 * post-move speed exceeds 0.15 -- three cases including BOTH negative
	 * controls (a through-cube-style test that only checked one clause
	 * would pass a transposed AND/OR bug).
	 *
	 * A stone wall sits at (81,110,80), directly +X of a boat centered at
	 * integer (80,110,80) (local bbox +-0.75 XZ, so the wall's face at
	 * x=81 is exactly where +X motion would be blocked). +Z stays open air.
	 * Beta's moveEntity Y,X,Z order means X is resolved (and zeroed on
	 * collision) before Z is attempted, and the wall does not extend in Z,
	 * so Z motion is never obstructed by it.
	 * ===================================================================== */
	{
		set_block(81, 110, 80, BLOCK_STONE, 0);

		/* (4a) POSITIVE: collision (X blocked) AND residual free-axis (Z)
		 * speed 0.3 > 0.15 -> destroyed. */
		struct entity boat;
		entity_boat(9002, &boat, true, &s.world);
		boat.pos[0] = 80.0f;
		boat.pos[1] = 110.0f;
		boat.pos[2] = 80.0f;
		boat.vel[0] = 0.30f;
		boat.vel[1] = 0.0f;
		boat.vel[2] = 0.30f;
		boat.on_ground = false;
		assert(boat_advance(&boat)
			   && "collided (X) + post-move speed 0.3 > 0.15 on the free Z "
				  "axis: must land-crash");

		/* (4b) NEGATIVE A: same high speed, NO wall in the path (boat
		 * started well clear, moving away from the wall) -> no collision ->
		 * must NOT crash regardless of speed. */
		struct entity boat_b;
		entity_boat(9003, &boat_b, true, &s.world);
		boat_b.pos[0] = 70.0f;
		boat_b.pos[1] = 110.0f;
		boat_b.pos[2] = 70.0f;
		boat_b.vel[0] = -0.30f; /* away from anything */
		boat_b.vel[1] = 0.0f;
		boat_b.vel[2] = 0.30f;
		boat_b.on_ground = false;
		assert(!boat_advance(&boat_b)
			   && "high speed with NO collision must NOT land-crash (speed "
				  "alone is insufficient)");

		/* (4c) NEGATIVE B: same wall (collision DOES occur), but LOW speed
		 * (0.1 <= 0.15) -> must NOT crash despite a real collision. */
		struct entity boat_c;
		entity_boat(9004, &boat_c, true, &s.world);
		boat_c.pos[0] = 80.0f;
		boat_c.pos[1] = 110.0f;
		boat_c.pos[2] = 80.0f;
		boat_c.vel[0] = 0.10f;
		boat_c.vel[1] = 0.0f;
		boat_c.vel[2] = 0.10f;
		boat_c.on_ground = false;
		assert(!boat_advance(&boat_c)
			   && "collision with LOW residual speed (0.1 <= 0.15) must NOT "
				  "land-crash (collision alone is insufficient)");

		set_block(81, 110, 80, BLOCK_AIR, 0);
	}

	/* =====================================================================
	 * (4d) Land-crash drop multiset + REMOVAL, driven through the REAL
	 * server tick loop (not a direct boat_advance call), to prove the
	 * onUpdate destroy path (as opposed to attackEntityFrom's) also drops
	 * the identical 3-planks/2-sticks multiset and that the generic tick
	 * loop actually removes + announces the entity.
	 * ===================================================================== */
	{
		set_block(91, 110, 90, BLOCK_STONE, 0);
		uint32_t id = spawn_registered_boat(90.0f, 110.0f, 90.0f);
		struct entity* boat = find_entity(id);
		assert(boat);
		boat->vel[0] = 0.30f;
		boat->vel[2] = 0.30f;
		reset_observer();
		assert(server_local_test_tick_entities(&s));
		assert(find_entity(id) == NULL
			   && "the generic tick loop must remove a boat whose onUpdate "
				  "returned land-crash-destroyed");
		assert(g_obs.destroy_count == 1 && g_obs.last_destroyed_id == id
			   && "removal must announce CRPC_ENTITY_DESTROY for this boat");
		assert_drop_multiset();
		set_block(91, 110, 90, BLOCK_AIR, 0);
	}

	/* =====================================================================
	 * (5) RIDER DISMOUNTED ON DESTRUCTION. Mount via the real SRPC_ENTITY_USE
	 * path, then destroy via attackEntityFrom's >40 threshold, and check the
	 * SERVER-side footstep-gate flag (s.player.riding_boat) -- NOT the freed
	 * entity's own field, which would be a use-after-free.
	 * [740c583: EntityBoat.attackEntityFrom "riddenByEntity.mountEntity(this)"
	 * is Beta's forced-dismount idiom]
	 * ===================================================================== */
	{
		uint32_t id = spawn_registered_boat(40.0f, 110.0f, 40.0f);
		s.player.x = 40.0;
		s.player.y = 110.0 + PLAYER_EYE_HEIGHT;
		s.player.z = 40.0;
		use_entity(id, true); /* mount */
		struct entity* boat = find_entity(id);
		assert(boat && boat->data.boat.occupied
			   && boat->data.boat.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID);
		assert(s.player.riding_boat
			   && "mounting must set the footstep-gate flag");
		boat->data.boat.damage_taken = 31; /* one hit from destruction */
		attack_entity(id);
		assert(find_entity(id) == NULL && "destroyed");
		assert(!s.player.riding_boat
			   && "destruction must dismount the rider (clear the "
				  "footstep-gate flag) -- Beta's forced-mountEntity(this) "
				  "dismount idiom");
	}

	/* =====================================================================
	 * (6) INTERACT: refuses a second (different) rider, allows the CURRENT
	 * rider (which, per Beta's mountEntity(sameEntity) idiom, means
	 * re-interacting DISMOUNTS them rather than being refused).
	 * [740c583: EntityBoat.interact]
	 * ===================================================================== */
	{
		uint32_t id = spawn_registered_boat(50.0f, 110.0f, 50.0f);
		s.player.x = 50.0;
		s.player.y = 110.0 + PLAYER_EYE_HEIGHT;
		s.player.z = 50.0;

		/* Simulate "occupied by a DIFFERENT rider": no such rider exists in
		 * this single-player engine, so this is a literal transcription of
		 * Beta's `riddenByEntity != var1` clause via a synthetic non-player
		 * occupant id, per the dispatch's explicit "refuses a second
		 * player" test. */
		struct entity* boat = find_entity(id);
		assert(boat);
		boat->data.boat.occupied = true;
		boat->data.boat.occupant_id = SERVER_LOCAL_PLAYER_ENTITY_ID + 12345;
		/* desired_occupied's VALUE is moot for this sub-case: the occupied-by-
		 * another-rider check is unconditional and fires before it is ever
		 * read. Passed as false (what a real client would compute from its
		 * own synced view showing occupied=true) purely for realism. */
		use_entity(id, false);
		boat = find_entity(id);
		assert(boat && boat->data.boat.occupied
			   && boat->data.boat.occupant_id
				  == SERVER_LOCAL_PLAYER_ENTITY_ID + 12345
			   && "occupied by a DIFFERENT rider: interact must REFUSE (no "
				  "state change)");

		/* Now genuinely mount as the local player, then interact again as
		 * the SAME (current) rider: Beta's mountEntity(this) idiom means
		 * this DISMOUNTS -- it must be ALLOWED, not refused. */
		boat->data.boat.occupied = false;
		boat->data.boat.occupant_id = 0;
		use_entity(id, true); /* mount */
		boat = find_entity(id);
		assert(boat && boat->data.boat.occupied
			   && boat->data.boat.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID
			   && "the current player must be able to mount an unoccupied "
				  "boat");
		use_entity(id, false); /* the CURRENT rider interacts again -> dismount */
		boat = find_entity(id);
		assert(boat && !boat->data.boat.occupied
			   && "re-interacting as the CURRENT rider must be ALLOWED and "
				  "toggles to dismount (Beta's mountEntity(sameEntity) "
				  "idiom), never refused");
	}

	/* =====================================================================
	 * (6b) IDEMPOTENT DOUBLE-PRESS -- adversarial-review finding #6. A raw
	 * !occupied TOGGLE (the pre-fix behaviour) cannot tell "the same click
	 * arriving twice" from "two genuinely separate mount/dismount clicks":
	 * two back-to-back requests BOTH computed as desired_occupied=true (a
	 * real client's own view has not yet learned the first one succeeded,
	 * since both fire before either round trip's correction lands -- see
	 * screen_ingame.c) must converge to occupied=true, NOT flip back to
	 * false. This is the exact discriminator: a toggle implementation
	 * FAILS this (true -> mount, true -> dismount again = ends unoccupied);
	 * the idempotent desired-state implementation PASSES (true -> mount,
	 * true -> harmless re-assertion = stays mounted).
	 * ===================================================================== */
	{
		uint32_t id = spawn_registered_boat(51.0f, 110.0f, 51.0f);
		s.player.x = 51.0;
		s.player.y = 110.0 + PLAYER_EYE_HEIGHT;
		s.player.z = 51.0;
		use_entity(id, true); /* click 1: request mounted */
		use_entity(id, true); /* click 2 (a double-press race): SAME request */
		struct entity* boat = find_entity(id);
		assert(boat && boat->data.boat.occupied
			   && boat->data.boat.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID
			   && "two consecutive desired_occupied=true requests (a "
				  "double-press racing ahead of the round trip) must both "
				  "converge to MOUNTED, not toggle back off");
		assert(s.player.riding_boat);

		/* Symmetric check: two consecutive dismount requests must also
		 * converge (stay unmounted), not toggle back to mounted. */
		use_entity(id, false);
		use_entity(id, false);
		boat = find_entity(id);
		assert(boat && !boat->data.boat.occupied
			   && "two consecutive desired_occupied=false requests must "
				  "both converge to UNMOUNTED, not toggle back on");
		assert(!s.player.riding_boat);
	}

	/* =====================================================================
	 * (7) BUOYANCY slice count/offset -- boat_submerged_fraction pinned
	 * directly against World.isAABBInMaterial's exact algorithm (see the
	 * file header derivation in BOAT_SLICE_NOTES.md for the by-hand slice
	 * boundary arithmetic). Boat centered at integer (20,64,20): local bbox
	 * y in [-0.3,+0.3] -> world Y in [63.7, 64.3].
	 *
	 * Case A: a full water SOURCE (metadata 0, surface = cellY+1 = 64.0)
	 * ONLY at cell y=63 (nothing at y=64). Hand-derived per-slice minY
	 * (slice i: y1 + 0.6*i/5 - 0.125):
	 *   i=0: minY=63.575 (64.0>=63.575 TRUE)   i=1: minY=63.695 (TRUE)
	 *   i=2: minY=63.815 (TRUE)                i=3: minY=63.935 (TRUE)
	 *   i=4: minY=64.055 (64.0>=64.055 FALSE, the water's surface is
	 *        exactly 0.055 below this slice's bottom)
	 * -> 4/5 = 0.8.
	 *
	 * Case B: water source ONLY at cell y=64 (surface=65.0), y=63 EMPTY.
	 * This specifically discriminates the -0.125 offset: WITHOUT it, 3 of
	 * 5 slices would see cell 64 (0.6 fraction); WITH the correct offset,
	 * only slices 3-4's cell range [floor(minY)..floor(maxY+1)) reaches
	 * cell 64 (0.4 fraction) -- slices 0-2 only ever examine cell 63,
	 * which is air here.
	 * ===================================================================== */
	{
		struct entity boat;
		entity_boat(9005, &boat, true, &s.world);
		boat.pos[0] = 20.0f;
		boat.pos[1] = 64.0f;
		boat.pos[2] = 20.0f;

		set_block(19, 63, 19, BLOCK_WATER_STILL, 0);
		set_block(20, 63, 19, BLOCK_WATER_STILL, 0);
		set_block(19, 63, 20, BLOCK_WATER_STILL, 0);
		set_block(20, 63, 20, BLOCK_WATER_STILL, 0);
		set_block(19, 64, 19, BLOCK_AIR, 0);
		set_block(20, 64, 19, BLOCK_AIR, 0);
		set_block(19, 64, 20, BLOCK_AIR, 0);
		set_block(20, 64, 20, BLOCK_AIR, 0);
		float frac_a = boat_submerged_fraction(&boat);
		assert(fabsf(frac_a - 0.8f) < EPS
			   && "water only at cell y=63: 4 of 5 slices (i=0..3) must see "
				  "it, i=4 must not (0.8)");

		set_block(19, 63, 19, BLOCK_AIR, 0);
		set_block(20, 63, 19, BLOCK_AIR, 0);
		set_block(19, 63, 20, BLOCK_AIR, 0);
		set_block(20, 63, 20, BLOCK_AIR, 0);
		set_block(19, 64, 19, BLOCK_WATER_STILL, 0);
		set_block(20, 64, 19, BLOCK_WATER_STILL, 0);
		set_block(19, 64, 20, BLOCK_WATER_STILL, 0);
		set_block(20, 64, 20, BLOCK_WATER_STILL, 0);
		float frac_b = boat_submerged_fraction(&boat);
		assert(fabsf(frac_b - 0.4f) < EPS
			   && "water only at cell y=64: only slices i=3,4 must see it "
				  "(0.4) -- a missing/wrong -0.125 offset would give 0.6");

		set_block(19, 64, 19, BLOCK_AIR, 0);
		set_block(20, 64, 19, BLOCK_AIR, 0);
		set_block(19, 64, 20, BLOCK_AIR, 0);
		set_block(20, 64, 20, BLOCK_AIR, 0);
	}

	/* =====================================================================
	 * (8) SRPC_BLOCK_PLACE FALLBACK -- adversarial-review finding #1.
	 * Beta's ItemBoat.onItemRightClick runs its OWN independent 5.0-block
	 * liquid-stopping raycast regardless of any normal block target,
	 * because water is non-collidable (getBoundingBox()==0, block_water.c),
	 * so the CLIENT's ordinary 4.5-block camera ray sails straight through
	 * a shallow/small pond to whatever solid block lies behind/under it --
	 * routing the click to SRPC_BLOCK_PLACE, never the open-air
	 * SRPC_USE_ITEM path. This section drives SRPC_BLOCK_PLACE DIRECTLY
	 * (never SRPC_USE_ITEM), targeting an ORDINARY stone block the boat
	 * item cannot use/place against (item_boat has neither onUseOnBlock
	 * nor onItemPlace, so used=false, placed=false unconditionally), while
	 * the player's OWN look direction (rx=0,ry=0 -> dir=(0,~0,1), matching
	 * boat_place_raycast's own derivation) faces open water 2 blocks along
	 * +Z -- well inside BOAT_PLACE_REACH=5.0. A pre-fix server (no
	 * ITEM_BOAT case in the fallback) silently drops the click: zero boats
	 * spawn. The decoy target block sits far off the look axis (+X, -Z)
	 * so a bug that placed at the SRPC_BLOCK_PLACE payload's coordinates
	 * (rather than running the player's own independent look-raycast)
	 * would land the boat at the WRONG position, not merely fail to spawn
	 * one -- proving the fix reuses the real raycast, not the click target.
	 *
	 * Hand-derived hit cell: ray origin (8.0, 99.0+1.62=100.62, 8.0) ->
	 * start cell (8,100,8) [air] -> steps along +Z only (dir X/Y ~0) ->
	 * (8,100,9) [air] -> (8,100,10) [WATER, t=2.0<=5.0: HIT]. Not snow ->
	 * boat_place_position(8,100,10,false) = (8.5, 101.0, 10.5).
	 * ===================================================================== */
	{
		set_block(8, 99, 10, BLOCK_STONE, 0);
		set_block(8, 100, 10, BLOCK_WATER_STILL, 0);
		set_block(14, 100, 6, BLOCK_STONE, 0); /* decoy: SRPC_BLOCK_PLACE target */

		s.player.x = 8.0;
		s.player.y = 99.0 + PLAYER_EYE_HEIGHT;
		s.player.z = 8.0;
		s.player.rx = 0.0f;
		s.player.ry = 0.0f; /* boat_place_raycast dir = (0,~0,1): looking +Z */

		size_t hb_slot
			= INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&s.player.inventory);
		s.player.inventory.items[hb_slot]
			= (struct item_data) {.id = ITEM_BOAT, .count = 1, .durability = 0};

		reset_observer();
		struct server_rpc rpc = {
			.type = SRPC_BLOCK_PLACE,
			.payload.block_place
			= {.x = 14, .y = 100, .z = 6, .side = SIDE_TOP},
		};
		server_local_test_process_rpc(&s, &rpc);

		assert(g_obs.spawn_boat_count == 1
			   && "SRPC_BLOCK_PLACE while holding a boat, targeting an "
				  "ORDINARY block, must fall through to boat placement "
				  "exactly like the EGG/SNOWBALL fallback -- Beta's "
				  "ItemBoat.onItemRightClick always runs regardless of any "
				  "normal block target (finding #1). A pre-fix server "
				  "silently drops this click: zero boats spawn here.");
		assert(fabsf(g_obs.last_spawn_boat.payload.spawn_boat.pos[0] - 8.5f)
				   < EPS
			   && fabsf(g_obs.last_spawn_boat.payload.spawn_boat.pos[1]
						- 101.0f)
					  < EPS
			   && fabsf(g_obs.last_spawn_boat.payload.spawn_boat.pos[2]
						- 10.5f)
					  < EPS
			   && "the boat must land at the WATER surface the PLAYER'S "
				  "OWN look-raycast found ((8,100,10) -> spawn (8.5,101.0,"
				  "10.5)), NOT at the SRPC_BLOCK_PLACE payload's decoy "
				  "coordinates (14,100,6) -- proves the independent "
				  "5.0-block raycast ran off the player's rx/ry, not a "
				  "naive reuse of the click payload's target");
		assert(s.player.inventory.items[hb_slot].count == 0
			   && "placing must consume the boat item "
				  "[740c583: ItemBoat.onItemRightClick --stackSize]");

		set_block(8, 99, 10, BLOCK_AIR, 0);
		set_block(8, 100, 10, BLOCK_AIR, 0);
		set_block(14, 100, 6, BLOCK_AIR, 0);
	}

	/* =====================================================================
	 * (9) RIDER TETHERING -- adversarial-review findings #2/#3/#5. Before
	 * this fix, mounting a boat only flipped server-side flags; the local
	 * player's OWN entity (entity_local_player.c) had ZERO ENTITY_BOAT
	 * handling and kept running ordinary walk/swim/gravity physics every
	 * tick, completely disconnected from the boat. Sections (5)/(6) above
	 * only assert the SERVER-side occupancy flags and "structurally cannot
	 * detect this gap" (finding #2's own words) -- this section drives the
	 * REAL client entity_tick() (gstate.players[0].local_player->tick_client).
	 *
	 * Hand-derived expected position [740c583: EntityBoat.updateRiderPosition]:
	 *   rider.pos = boat.pos + (cos(yaw_rad)*0.4, mountedYOffset(-0.3) +
	 *                            rider.yOffset(0.0) + EYE_HEIGHT(1.62),
	 *                            sin(yaw_rad)*0.4)
	 * Two yaw sub-cases (0 and pi/2) so BOTH the cos-term (X) and the
	 * sin-term (Z) are independently proven present: at yaw=0 ALONE, X
	 * changes but Z does not (sin(0)=0), which cannot distinguish "the sin
	 * term correctly evaluates to 0" from "the sin term is missing
	 * entirely" -- a test that only checked yaw=0 would pass under both a
	 * correct implementation and a bug that hardcoded rider.pos[2] =
	 * boat.pos[2] with no sin(yaw) term at all. yaw=pi/2 flips which axis
	 * moves, discriminating both terms independently.
	 * ===================================================================== */
	{
		memset(&gstate, 0, sizeof(gstate));
		world_create(&gstate.world);
		dict_entity_init(gstate.entities);
		particle_init();
		struct client_rpc client_reset = {
			.type = CRPC_WORLD_RESET,
			.payload.world_reset = {
				.dimension = WORLD_DIM_OVERWORLD,
				.local_entity = SERVER_LOCAL_PLAYER_ENTITY_ID,
			},
		};
		clin_process(&client_reset);
		assert(gstate.players[0].local_player);
		assert(gstate.players[0].local_player->id == SERVER_LOCAL_PLAYER_ENTITY_ID
			   && "entity_tick's boat scan matches occupant_id == e->id -- "
				  "the local player's own entity id must be the sentinel 0");
		assert(gstate.players[0].local_player->y_offset == 0.0f
			   && "precondition the hand-derived Y below depends on: "
				  "entity_default_init leaves y_offset at 0.0 and "
				  "entity_local_player() never calls entity_set_size for "
				  "the local player -- if that ever changes, the expected "
				  "Y constants in this section must change with it");

		/* Spawn + mount a fresh SERVER boat, capture the REAL spawn/
		 * correction RPCs it publishes, and feed them through clin_process
		 * to build a matching CLIENT boat entity -- never hand-construct
		 * the payload (CODESTYLE.md 3.2: a shape a real function already
		 * emits should not get a second, independently-drifting copy).
		 * Position (3,64,3): a fresh in-bounds zone (X/Z < 19, distinct
		 * from every other section's test zone including section (8)'s
		 * (8,_,8..10)/(14,_,6)) inside the loaded chunk 0..5 range this
		 * file's setup requires. */
		/* F8 (Beta blockExists guard): entity_tick returns before the boat
		 * branch unless the rider column is resident, so install the boat
		 * column (0,0) in the CLIENT world -- the rider is parked inside it
		 * below. One write in its y=[64,79] section is enough. */
		world_set_block(&gstate.world, 3, 64, 3,
						(struct block_data) {.type = BLOCK_STONE}, false);
		assert(world_find_chunk(&gstate.world, 9, 64, 9)
			   && "the rider column must be resident so the F8 gate opens");

		reset_observer();
		uint32_t boat_id = spawn_registered_boat(3.0f, 64.0f, 3.0f);
		assert(g_obs.spawn_boat_count == 1);
		clin_process(&g_obs.last_spawn_boat);
		struct entity** client_boat_pp
			= dict_entity_get(gstate.entities, boat_id);
		assert(client_boat_pp && *client_boat_pp
			   && (*client_boat_pp)->type == ENTITY_BOAT);
		struct entity* client_boat = *client_boat_pp;

		s.player.x = 3.0;
		s.player.y = 64.0 + PLAYER_EYE_HEIGHT;
		s.player.z = 3.0;
		use_entity(boat_id, true); /* mount, the real SRPC_ENTITY_USE path */
		/* server_local_set_boat_occupancy already flipped occupied=true on
		 * the SERVER's boat SYNCHRONOUSLY (proven directly by sections
		 * (5)/(6) above with no retry needed) -- but its own
		 * boat_send_correction call uses clin_try_rpc_send, a NON-blocking
		 * send that can genuinely fail if the client-inbox token pool
		 * (RPC_INBOX_SIZE=16, client_interface.c) is momentarily exhausted
		 * by the async drain thread lagging behind this file's many
		 * earlier, bursty sections. This is documented, SELF-HEALING
		 * behaviour ("a dropped send self-heals via the ordinary periodic
		 * cadence" -- server_local.c), not a bug: retry the SAME real
		 * production function (never a hand-built payload) until it lands,
		 * mirroring how the engine itself recovers. Bounded so a genuine
		 * regression fails fast rather than hanging the suite (CODESTYLE.md
		 * §4.3). */
		struct entity* boat_srv = find_entity(boat_id);
		assert(boat_srv);
		for(int tries = 0;
			g_obs.boat_correction_count == 0 && tries < 500; tries++) {
			thread_msleep(2);
			(void)boat_send_correction(boat_srv);
		}
		assert(g_obs.boat_correction_count >= 1
			   && g_obs.last_boat_correction.payload.boat_state.occupied
			   && "mount must (eventually) publish an occupied=true "
				  "correction");
		clin_process(&g_obs.last_boat_correction);
		assert(client_boat->data.boat.occupied
			   && client_boat->data.boat.occupant_id
				  == SERVER_LOCAL_PLAYER_ENTITY_ID
			   && "the CLIENT boat copy must learn occupied=true from the "
				  "correction");

		/* Sub-case A: yaw = 0 -> cos=1, sin=0. Proves the X (cos) term AND
		 * that the mounted tick returns BEFORE ordinary walking physics. */
		{
			glm_vec3_copy((vec3) {3.0f, 64.0f, 3.0f}, client_boat->pos);
			client_boat->orient[0] = 0.0f;
			/* F8 (Beta blockExists guard): the old (9999,9999,9999) sentinel
			 * sits in a column that is not resident, so the gate would hold
			 * the body before the boat branch ran. Park the rider OFF the
			 * boat but INSIDE the boat column (6 blocks east/south, 20 up):
			 * still a large, measurable distance for the snap to close. */
			gstate.players[0].local_player->pos[0] = 9.0f; /* off-boat: must SNAP */
			gstate.players[0].local_player->pos[1] = 84.0f;
			gstate.players[0].local_player->pos[2] = 9.0f;
			gstate.players[0].local_player->vel[0] = 0.37f; /* nonzero: must stay untouched */
			gstate.players[0].local_player->vel[1] = -0.53f;
			gstate.players[0].local_player->vel[2] = 0.11f;
			gstate.players[0].local_player->on_ground = false;
			gstate.players[0].local_player->data.local_player.capture_input = false;
			assert(gstate.players[0].local_player->tick_client);
			bool ret = gstate.players[0].local_player->tick_client(gstate.players[0].local_player);
			assert(!ret);
			assert(fabsf(gstate.players[0].local_player->pos[0] - 3.4f) < EPS
				   && "yaw=0: X must be boat.x + cos(0)*0.4 = 3.4 -- a "
					  "MISSING boat branch would leave X at the preset "
					  "9999.0 (finding #2)");
			assert(fabsf(gstate.players[0].local_player->pos[1] - 65.32f) < EPS
				   && "Y = boat.y(64.0) + mountedYOffset(-0.3) + "
					  "yOffset(0.0) + EYE_HEIGHT(1.62) = 65.32, "
					  "yaw-independent");
			assert(fabsf(gstate.players[0].local_player->pos[2] - 3.0f) < EPS
				   && "yaw=0: Z must be boat.z + sin(0)*0.4 = 3.0 "
					  "(unchanged from the boat's own Z)");
			assert(gstate.players[0].local_player->vel[0] == 0.37f
				   && gstate.players[0].local_player->vel[1] == -0.53f
				   && gstate.players[0].local_player->vel[2] == 0.11f
				   && "a mounted tick must return BEFORE the ordinary "
					  "walking-physics block -- vel must be byte-unchanged "
					  "(no gravity/friction), matching the minecart "
					  "follow's own early-return precedent");
		}

		/* Sub-case B: yaw = pi/2 -> cos~=0, sin~=1. Proves the Z (sin)
		 * term AND that the offset genuinely tracks yaw. */
		{
			glm_vec3_copy((vec3) {3.0f, 64.0f, 3.0f}, client_boat->pos);
			client_boat->orient[0] = GLM_PIf / 2.0f;
			/* F8 (Beta blockExists guard): off-boat but inside the resident
			 * boat column, same reason as sub-case A above. */
			gstate.players[0].local_player->pos[0] = 9.0f;
			gstate.players[0].local_player->pos[1] = 84.0f;
			gstate.players[0].local_player->pos[2] = 9.0f;
			bool ret = gstate.players[0].local_player->tick_client(gstate.players[0].local_player);
			assert(!ret);
			assert(fabsf(gstate.players[0].local_player->pos[0] - 3.0f) < EPS
				   && "yaw=pi/2: X must be boat.x + cos(pi/2)*0.4 ~= 3.0 "
					  "(cos(pi/2)~=0)");
			assert(fabsf(gstate.players[0].local_player->pos[1] - 65.32f) < EPS
				   && "Y stays yaw-independent");
			assert(fabsf(gstate.players[0].local_player->pos[2] - 3.4f) < EPS
				   && "yaw=pi/2: Z must be boat.z + sin(pi/2)*0.4 = 3.4 "
					  "-- this is what actually discriminates the sin/cos "
					  "terms from a hardcoded X-only offset: sub-case A "
					  "alone could not tell 'sin term missing' from "
					  "'sin(0)=0'");
		}

		/* Fixed float bits from the source-derived double-intermediate scan in
		 * boat-pos-select.py. This is not an independent Java trig oracle. */
		{
			static const struct {
				const char* axis;
				float position;
				float yaw;
				uint32_t expected;
				int component;
			} cases[] = {
				{"X", 1.1f, 0.5f, 0x3fb9bb73u, 0},
				{"X", 1.3f, 0.3f, 0x3fd7502fu, 0},
				{"Z", 0.6f, 1.1f, 0x3f74dc11u, 2},
				{"Z", 0.7f, 0.5f, 0x3f644b0du, 2},
			};
			int bit_failures = 0;
			for(size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
				glm_vec3_copy((vec3) {3.0f, 64.0f, 3.0f}, client_boat->pos);
				client_boat->pos[cases[i].component] = cases[i].position;
				client_boat->orient[0] = cases[i].yaw;
				gstate.players[0].local_player->pos[0] = 9.0f;
				gstate.players[0].local_player->pos[1] = 84.0f;
				gstate.players[0].local_player->pos[2] = 9.0f;
				gstate.players[0].local_player->data.local_player.capture_input = false;
				bool ret = gstate.players[0].local_player->tick_client(
					gstate.players[0].local_player);
				assert(!ret);
				uint32_t actual;
				memcpy(&actual,
					   &gstate.players[0].local_player->pos[cases[i].component],
					   sizeof actual);
				fprintf(stderr,
					"boat rider %s position=%.2f yaw=%.2f expected=0x%08x actual=0x%08x\n",
					cases[i].axis, cases[i].position, cases[i].yaw,
					(unsigned)cases[i].expected, (unsigned)actual);
				if(actual != cases[i].expected)
					bit_failures++;
			}
			assert(bit_failures == 0);
		}

		/* Dismounted: the scan must NOT match a no-longer-occupied boat --
		 * must fall through to ordinary physics instead of snapping to the
		 * boat. With vel preset to exactly (0,0,0) and capture_input=false
		 * (no WASD), ordinary physics applies zero horizontal accel and
		 * entity_try_move has nothing to displace, so pos is provably
		 * UNCHANGED over one tick -- a tight, hand-derivable bound, not a
		 * loose "somewhere else" guess. */
		{
			client_boat->data.boat.occupied = false;
			client_boat->data.boat.occupant_id = 0;
			/* F8 (Beta blockExists guard): this sub-case must reach ordinary
			 * physics, so the column it stands in has to be resident too --
			 * install column (3,3) rather than move the body, since the
			 * assertions below name 55.0f directly. */
			world_set_block(&gstate.world, 55, 64, 55,
							(struct block_data) {.type = BLOCK_STONE}, false);
			gstate.players[0].local_player->pos[0] = 55.0f;
			gstate.players[0].local_player->pos[1] = 55.0f;
			gstate.players[0].local_player->pos[2] = 55.0f;
			gstate.players[0].local_player->vel[0] = 0.0f;
			gstate.players[0].local_player->vel[1] = 0.0f;
			gstate.players[0].local_player->vel[2] = 0.0f;
			gstate.players[0].local_player->on_ground = false;
			gstate.players[0].local_player->data.local_player.capture_input = false;
			/* AUDIT 2026-09-01 #1 (REGRESSION-TODAY, CONFIRMED), FIX 1(c):
			 * plant a STALE nonzero rider_vel, exactly as sub-case A/B above
			 * would have left it after a real mounted tick (before this fix,
			 * nothing ever cleared it again once mounting stopped matching).
			 * These specific values are unreachable via the dismounted
			 * tick's own math (which never touches boat_rider_vel_x/z at
			 * all), so their presence after the tick can ONLY mean the
			 * reset never ran. */
			gstate.players[0].local_player->data.local_player.boat_rider_vel_x = 0.37f;
			gstate.players[0].local_player->data.local_player.boat_rider_vel_z = -0.53f;
			bool ret = gstate.players[0].local_player->tick_client(gstate.players[0].local_player);
			assert(!ret);
			assert(fabsf(gstate.players[0].local_player->pos[0] - 55.0f) < EPS
				   && fabsf(gstate.players[0].local_player->pos[1] - 55.0f) < EPS
				   && fabsf(gstate.players[0].local_player->pos[2] - 55.0f) < EPS
				   && "dismounted (occupied=false): the boat scan must NOT "
					  "match -- with zero velocity and no input, ordinary "
					  "physics leaves pos unchanged this tick; it must NOT "
					  "snap to the unrelated boat position (3,64,3)");
			assert(gstate.players[0].local_player->data.local_player.boat_rider_vel_x
					   == 0.0f
				   && gstate.players[0].local_player->data.local_player.boat_rider_vel_z
						  == 0.0f
				   && "FIX 1(c): a tick that finds this player NOT "
					  "boat-mounted must reset boat_rider_vel_x/z to 0 -- "
					  "the field's own contract (entity.h) is '0.0 whenever "
					  "not boat-mounted', and a dismounted player must not "
					  "keep transmitting its last-mounted-tick steering "
					  "input over SRPC_PLAYER_POS");

			/* AUDIT 2026-09-01 #1, TEST 3 -- "a not-riding position RPC
			 * carries 0": mirror client_interface.c's clin_update transmit
			 * expression byte-for-byte (SRPC_PLAYER_POS .vel_x/.vel_z =
			 * gstate.players[0].local_player ? ...boat_rider_vel_x/z : 0.0F) rather
			 * than re-deriving it, so this proves the EXACT value the wire
			 * would carry -- then feeds it through the REAL server-side
			 * receive path (server_local_test_process_rpc, never a
			 * hand-assigned s.player field) to prove the whole pipe ends in
			 * a stored 0, not just the client-side field. */
			float wire_vel_x = gstate.players[0].local_player
				? gstate.players[0].local_player->data.local_player.boat_rider_vel_x
				: 0.0F;
			float wire_vel_z = gstate.players[0].local_player
				? gstate.players[0].local_player->data.local_player.boat_rider_vel_z
				: 0.0F;
			assert(wire_vel_x == 0.0f && wire_vel_z == 0.0f
				   && "the value about to go on the wire for a non-riding "
					  "player must be 0.0, not a stale mounted-tick value");

			s.player.rider_vel_x = 123.0f; /* poison: the RPC below must overwrite it */
			s.player.rider_vel_z = -456.0f;
			struct server_rpc pos_rpc = {
				.type = SRPC_PLAYER_POS,
				.payload.player_pos = {
					.x = s.player.x,
					.y = s.player.y,
					.z = s.player.z,
					.rx = s.player.rx,
					.ry = s.player.ry,
					.vel_y = 0.0f,
					.vel_x = wire_vel_x,
					.vel_z = wire_vel_z,
					.on_ground = s.player.on_ground,
					.sneaking = s.player.sneaking,
					.correction_seq = s.player.correction_seq,
				},
			};
			server_local_test_process_rpc(&s, &pos_rpc);
			assert(s.player.rider_vel_x == 0.0f && s.player.rider_vel_z == 0.0f
				   && "server_local's SRPC_PLAYER_POS handler must store "
					  "the client-reported 0 for a not-riding player, not "
					  "retain the poisoned 123.0/-456.0 pre-set above -- "
					  "end-to-end proof the 'zero while not riding' "
					  "contract holds across the wire");
		}
	}

	/* =====================================================================
	 * (10) LINE-OF-SIGHT GATE -- adversarial-review finding #10. Beta's
	 * NetServerHandler.func_6006_a gates entity interact/attack on
	 * canEntityBeSeen(target) AND distSq<36.0, not distance alone. A boat
	 * across a thin wall, within 6 blocks straight-line, must not be
	 * mountable or attackable through it. NEGATIVE CONTROL included: the
	 * SAME distance with the wall removed must succeed -- without it, a
	 * server that (bug) always refused would pass the positive half for
	 * the wrong reason. Runs BEFORE section (11)'s death test on purpose:
	 * a death leaves s->world_switch_phase != SERVER_WORLD_SWITCH_IDLE
	 * until the respawn handoff completes, and the general RPC gate in
	 * server_local_process drops EVERY SRPC (including SRPC_ENTITY_USE)
	 * while that holds -- ordering this section first keeps it independent
	 * of that unrelated post-death state instead of having to reason
	 * about resetting it correctly (found the hard way: placing this
	 * after the death section made the negative control fail for the
	 * WRONG reason -- the world-switch gate, not LOS).
	 * ===================================================================== */
	{
		uint32_t id = spawn_registered_boat(16.0f, 110.0f, 6.0f);
		/* Full-height wall directly between the player's eye and the
		 * boat's own position (a single Z-plane at z=8, X held constant at
		 * 16 like the player and boat) -- 3 blocks tall so the interpolated
		 * eye-to-boat line's Y at z=8 (hand-checked ~110.8) falls inside it
		 * regardless of DDA tie-breaking. */
		for(int wy = 109; wy <= 111; wy++)
			set_block(16, wy, 8, BLOCK_STONE, 0);
		s.player.x = 16.0;
		s.player.y = 110.0 + PLAYER_EYE_HEIGHT;
		s.player.z = 10.0; /* dz=4 from the boat: distSq=16<36, in reach */

		use_entity(id, true); /* attempt to mount THROUGH the wall */
		struct entity* boat = find_entity(id);
		assert(boat && !boat->data.boat.occupied
			   && "a boat behind a wall, though within 6 blocks "
				  "straight-line, must NOT be mountable -- canEntityBeSeen "
				  "must gate this exactly like distance does (finding #10)");

		reset_observer();
		attack_entity(id);
		boat = find_entity(id);
		assert(boat && boat->data.boat.damage_taken == 0
			   && !boat->data.boat.been_attacked
			   && g_obs.item_spawn_count == 0
			   && "a boat behind a wall must not be attackable through it "
				  "either -- the same canEntityBeSeen gate");

		/* Remove the wall; SAME distance must now succeed. */
		for(int wy = 109; wy <= 111; wy++)
			set_block(16, wy, 8, BLOCK_AIR, 0);
		use_entity(id, true);
		boat = find_entity(id);
		assert(boat && boat->data.boat.occupied
			   && boat->data.boat.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID
			   && "with the wall removed, the SAME distance must now mount "
				  "successfully -- proves finding #10's fix is a genuine "
				  "LOS gate, not an accidental always-refuse that would "
				  "make the positive check above pass for the wrong reason");
		use_entity(id, false); /* clean up: dismount */
	}

	/* =====================================================================
	 * (10b) RIDER STEERING COUPLING -- closes docs/deviations.md "Boat
	 * rider steering". Before this fix, the occupied branch's
	 * rider_vel_x/z were hardcoded to 0.0f (a mounted boat floated/decayed
	 * exactly like an empty one); this section proves (i) the coupling
	 * formula itself is exact given a nonzero e->data.boat.rider_vel_x/z,
	 * (ii) an UNOCCUPIED boat is byte-identical to the pre-fix path
	 * (equivalence: a nonzero rider_vel_x/z on an unoccupied boat must
	 * have ZERO effect -- the gate is `occupied`, not the field), and
	 * (iii) the SERVER-side plumbing (struct server_player.rider_vel_x/z
	 * -> boat_tick_server -> data.boat.rider_vel_x/z) actually reaches
	 * boat_advance's coupling term end-to-end.
	 *
	 * Hand-derived arithmetic [740c583: EntityBoat.onUpdate], starting
	 * from vel=(0,0,0), on_ground=false, open air at Y=110 (far above the
	 * Y=100/101 superflat floor, matching section (3)'s own precedent), no
	 * collision:
	 *   buoyancy: motionY += 0.04*(0*2-1) = -0.04  -> vel[1] = -0.04
	 *   coupling (occupied only): motionX += rider_vel_x*0.2;
	 *                              motionZ += rider_vel_z*0.2
	 *   clamp +-0.4: not engaged at these magnitudes
	 *   on_ground=false -> no ground-drag halving
	 *   moveEntity: open air, nothing to collide with -> vel unchanged
	 *   speed well under 0.15 -> not a land crash -> drag branch:
	 *   vel[0] *= 0.99; vel[1] *= 0.95; vel[2] *= 0.99
	 * ===================================================================== */
	{
		/* (i) formula, occupied + nonzero rider_vel_x/z. */
		struct entity boat;
		entity_boat(9101, &boat, true, &s.world);
		boat.pos[0] = 62.0f;
		boat.pos[1] = 110.0f;
		boat.pos[2] = 62.0f;
		boat.on_ground = false;
		boat.data.boat.occupied = true;
		boat.data.boat.rider_vel_x = 0.1f;
		boat.data.boat.rider_vel_z = -0.2f;
		bool destroyed = boat_advance(&boat);
		assert(!destroyed && "open air, tiny velocity: must not land-crash");
		assert(fabsf(boat.vel[0] - 0.0198f) < EPS
			   && "coupling: (0 + 0.1*0.2)*0.99 = 0.0198");
		assert(fabsf(boat.vel[2] - (-0.0396f)) < EPS
			   && "coupling: (0 + -0.2*0.2)*0.99 = -0.0396");
		assert(fabsf(boat.vel[1] - (-0.038f)) < EPS
			   && "Y unaffected by the coupling term -- buoyancy+drag only");
	}
	{
		/* (ii) EQUIVALENCE: unoccupied boat with the SAME nonzero
		 * rider_vel_x/z must be byte-identical to the pre-fix
		 * (hardcoded-zero) formula. */
		struct entity boat;
		entity_boat(9102, &boat, true, &s.world);
		assert(boat.data.boat.rider_vel_x == 0.0f
			   && boat.data.boat.rider_vel_z == 0.0f
			   && "entity_boat() must default rider_vel_x/z to 0.0");
		boat.pos[0] = 62.0f;
		boat.pos[1] = 110.0f;
		boat.pos[2] = 62.0f;
		boat.on_ground = false;
		boat.data.boat.occupied = false; /* the pre-fix, still-supported case */
		boat.data.boat.rider_vel_x = 0.1f;  /* nonzero ON PURPOSE: proves the */
		boat.data.boat.rider_vel_z = -0.2f; /* gate is `occupied`, not the field */
		bool destroyed = boat_advance(&boat);
		assert(!destroyed);
		assert(boat.vel[0] == 0.0f
			   && "unoccupied: zero coupling contribution however this "
				  "field is set -- byte-identical to the pre-fix formula");
		assert(boat.vel[2] == 0.0f
			   && "unoccupied: zero coupling contribution however this "
				  "field is set -- byte-identical to the pre-fix formula");
		assert(fabsf(boat.vel[1] - (-0.038f)) < EPS
			   && "buoyancy+drag alone, exactly matching an empty boat");
	}
	{
		/* (iii) SERVER plumbing end-to-end: struct server_player.
		 * rider_vel_x/z (as if freshly reported by SRPC_PLAYER_POS) must
		 * reach the occupied boat's coupling via boat_tick_server. */
		uint32_t id = spawn_registered_boat(70.0f, 110.0f, 70.0f);
		s.player.x = 70.0;
		s.player.y = 110.0 + PLAYER_EYE_HEIGHT;
		s.player.z = 70.0;
		use_entity(id, true); /* mount, the real SRPC_ENTITY_USE path */
		struct entity* boat = find_entity(id);
		assert(boat && boat->data.boat.occupied
			   && boat->data.boat.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID);
		boat->vel[0] = 0.0f;
		boat->vel[1] = 0.0f;
		boat->vel[2] = 0.0f;
		boat->on_ground = false;
		s.player.rider_vel_x = 0.1f;
		s.player.rider_vel_z = -0.2f;
		assert(boat->tick_server);
		bool destroyed = boat->tick_server(boat, &s);
		assert(!destroyed);
		assert(fabsf(boat->data.boat.rider_vel_x - 0.1f) < EPS
			   && "boat_tick_server must copy s->player.rider_vel_x onto "
				  "the occupied boat before boat_advance reads it");
		assert(fabsf(boat->data.boat.rider_vel_z - (-0.2f)) < EPS
			   && "boat_tick_server must copy s->player.rider_vel_z onto "
				  "the occupied boat before boat_advance reads it");
		assert(fabsf(boat->vel[0] - 0.0198f) < EPS
			   && "the SAME coupling arithmetic as (i), now proven "
				  "reachable end-to-end from struct server_player");
		assert(fabsf(boat->vel[2] - (-0.0396f)) < EPS
			   && "the SAME coupling arithmetic as (i), now proven "
				  "reachable end-to-end from struct server_player");
		use_entity(id, false); /* clean up: dismount */
	}

	/* =====================================================================
	 * (10c) boat_rider_input_velocity -- the pure forward/strafe/yaw ->
	 * rider_vel_x/z formula entity_local_player.c's BOAT MOUNT branch
	 * feeds into boat->data.boat.rider_vel_x/z every tick. Exposed
	 * (not static) specifically so it is testable here without a real
	 * input backend [740c583: Entity.moveFlying].
	 * Hand-derived: forward=1,strafe=0,yaw=0 -> dist=1,distf=1,
	 *   dx=(1*sin0-0*cos0)/1=0, dz=(0*sin0+1*cos0)/1=1
	 *   -> vel=(0.02*0, 0.02*1)=(0.0, 0.02)
	 * forward=0,strafe=1,yaw=0 -> dx=(0-1*1)/1=-1, dz=(0+0)/1=0
	 *   -> vel=(0.02*-1, 0.0)=(-0.02, 0.0)
	 * diagonal forward=1,strafe=1,yaw=0 -> dist=2,distf=sqrt(2),
	 *   dx=(0-1)/sqrt(2)=-0.70710678, dz=(1+0)/sqrt(2)=0.70710678
	 *   -> vel=(0.02*-0.70710678, 0.02*0.70710678) -- NOT
	 *   (-0.02,0.02): the sqrt(2) normalization is what proves diagonal
	 *   input isn't faster than axis-aligned input.
	 * no input -> exactly (0,0), any yaw.
	 * ===================================================================== */
	{
		float vx, vz;
		boat_rider_input_velocity(1.0f, 0.0f, 0.0f, &vx, &vz);
		assert(fabsf(vx - 0.0f) < EPS && fabsf(vz - 0.02f) < EPS
			   && "forward-only at yaw=0 must be purely +Z");

		boat_rider_input_velocity(0.0f, 1.0f, 0.0f, &vx, &vz);
		assert(fabsf(vx - (-0.02f)) < EPS && fabsf(vz - 0.0f) < EPS
			   && "strafe-only at yaw=0 must be purely -X");

		boat_rider_input_velocity(1.0f, 1.0f, 0.0f, &vx, &vz);
		assert(fabsf(vx - (-0.01414214f)) < EPS
			   && fabsf(vz - 0.01414214f) < EPS
			   && "diagonal input must be normalized by sqrt(2), not equal "
				  "magnitude to the axis-aligned cases above");

		boat_rider_input_velocity(0.0f, 0.0f, 1.23f, &vx, &vz);
		assert(vx == 0.0f && vz == 0.0f
			   && "zero input -> exactly (0,0) regardless of yaw, matching "
				  "Beta's own moveFlying no-op gate");
	}

	/* =====================================================================
	 * (11) DEATH DISMOUNT -- adversarial-review finding #4. The respawn-
	 * plan dismount loop (server_local_prepare_respawn_plan) cleared every
	 * claimed ENTITY_MINECART on death but had no ENTITY_BOAT branch.
	 * Boats give no damage immunity, and (before findings #2/#3's fix)
	 * nothing tethered the rider -- so a fatal hit taken while riding left
	 * boat.occupied/riding_boat stuck permanently true after the fresh
	 * body respawned elsewhere, silencing footsteps for the rest of the
	 * session. Drives the REAL lethal path (server_local_set_player_health,
	 * the same public entry point a real attackEntityFrom/void-kill call
	 * uses), not a direct call to the static prepare_respawn_plan --
	 * matching tests/test_player_combat_env.c's own established death-test
	 * recipe (world_switch_phase=IDLE, has_bed_spawn=false for the
	 * simplest world-spawn path). Runs LAST because it leaves
	 * world_switch_phase non-IDLE (see section (10)'s note) -- nothing
	 * after this point may depend on ordinary RPC processing.
	 * ===================================================================== */
	{
		uint32_t id = spawn_registered_boat(4.0f, 110.0f, 4.0f);
		s.player.x = 4.0;
		s.player.y = 110.0 + PLAYER_EYE_HEIGHT;
		s.player.z = 4.0;
		use_entity(id, true); /* mount */
		struct entity* boat = find_entity(id);
		assert(boat && boat->data.boat.occupied
			   && boat->data.boat.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID);
		assert(s.player.riding_boat);

		s.player.has_bed_spawn = false; /* simplest path: no bed search */
		s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
		/* Deviation-#18: zero-init health reads as already-dead and the
		 * lethal funnel rejects it; the rider must be explicitly alive. */
		s.player.health = MAX_PLAYER_HEALTH;
		assert(server_local_set_player_health(&s, 0)
			   && "a healthy, non-bed, non-journaled death (this test "
				  "world has no level archive to journal) must succeed");
		/* The corpse stays seated through the death-screen dwell; the
		 * dismount sweep runs in the RESPAWN transaction (deviation-#18
		 * split, same placement as the minecart/pig branches). */
		boat = find_entity(id);
		assert(boat && boat->data.boat.occupied);
		assert(server_local_respawn_request(&s));

		boat = find_entity(id);
		assert(boat && !boat->data.boat.occupied
			   && boat->data.boat.occupant_id == 0
			   && "the respawn must dismount a ridden boat, exactly "
				  "like the pre-existing minecart branch this loop "
				  "already covers (finding #4) -- a pre-fix server leaves "
				  "this boat stuck occupied=true forever");
		assert(!s.player.riding_boat
			   && "the footstep-gate flag must clear on respawn, or "
				  "footsteps stay silenced for the rest of the session "
				  "after respawn");
	}

	printf("test_boat_env: ALL CHECKS PASSED\n");
	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
