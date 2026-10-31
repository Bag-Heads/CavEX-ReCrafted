/*
	test_cactus_contact_damage -- FAITHFULNESS env test (Beta 1.7.3, decompile
	740c583) for cactus SIDE contact, over a live superflat server_world.

	Maintainer observation (real Wii far_walk autopilot, 2026-09-07): walking into
	a cactus dealt no damage. The pre-fix server sampled two POINTS (the eye cell
	and the feet cell) for BLOCK_CACTUS -- and cactus is solid, so a player is
	never "inside" one. Side contact could therefore never damage.

	Beta's real site is the post-move cell sweep at the tail of Entity.moveEntity:

	  int i1 = floor(boundingBox.minX + 0.001D);  int k3 = floor(boundingBox.maxX - 0.001D);
	  int k1 = floor(boundingBox.minY + 0.001D);  int l3 = floor(boundingBox.maxY - 0.001D);
	  int i2 = floor(boundingBox.minZ + 0.001D);  int i4 = floor(boundingBox.maxZ - 0.001D);
	  if(worldObj.checkChunksExist(i1, k1, i2, k3, l3, i4))
	    for(j4 = i1; j4 <= k3; j4++) for(k4 = k1; k4 <= l3; k4++) for(l4 = i2; l4 <= i4; l4++)
	      Block.blocksList[id].onEntityCollidedWithBlock(worldObj, j4, k4, l4, this);
	  [740c583: Entity.moveEntity, the block-contact tail]

	and BlockCactus.onEntityCollidedWithBlock is a bare attackEntityFrom(null, 1)
	[740c583: BlockCactus.onEntityCollidedWithBlock].

	Contact is possible at all only because the cactus COLLISION box is inset
	1/16 -- (i+f, j, k+f)..((i+1)-f, (j+1)-f, (k+1)-f), f = 0.0625F
	[740c583: BlockCactus.getCollisionBoundingBoxFromPool] -- so a body stopped
	flush against a cactus reaches 0.0625 INTO the cactus's own cell and the
	contracted sweep still floors into it. The 1/16 TOP inset is what makes
	standing on a cactus hurt; the SELECTION box keeps maxY = j+1
	[740c583: BlockCactus.getSelectedBoundingBoxFromPool].

	Every expected value below is derived BY HAND from those citations (the
	poisoned-golden rule, CODESTYLE 4.2) -- never by running the port.

	Cases: the four side faces, all four corners, standing on top, the exact
	one-epsilon-outside non-contact on every face, an out-of-reach gap, the
	i-frame gate on repeated contact, and the mob path
	(entity_monster_post_move_cactus_collisions).

	Fix-round additions (2026-09-07 hostile review): the FIRE scan's chunk
	preflight on a chunk seam (World.checkChunksExist is chunk-INCLUSIVE of the
	+1 bound), fire damage while wet (Entity.moveEntity gates only fire++ on
	isWet, never dealFireDamage), and the dropped-item path
	(EntityItem.attackEntityFrom really takes the cactus hit -- health 5).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "block/aabb.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h"     // MOB_CACTUS_DAMAGE
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h" // MAX_PLAYER_HEALTH, HEALTH_PER_HEART
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

/* Beta BlockCactus.getCollisionBoundingBoxFromPool's `float f = 0.0625F`. */
#define CACTUS_INSET 0.0625
/* Player body: 0.6 wide, so half-width 0.3 (Beta EntityPlayer setSize). */
#define PLAYER_HALF 0.3

/* Superflat: solid to Y_GROUND, air above. Keep everything well inside one
 * loaded chunk so checkChunksExist can never be the reason a case fails. */
#define Y_GROUND 100
#define CY (Y_GROUND + 1) /* the cactus block's own Y */
#define CX 8
#define CZ 8

static struct server_local s;
static volatile int g_drain_run = 1;
static uint32_t g_next_id = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type) {
	struct block_data expected = {.type = (uint8_t)type, .metadata = 0};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type);
}

/* Park the player at an EYE position. server_local's scans reconstruct the body
 * box as [eye - PLAYER_EYE_HEIGHT, +PLAYER_BODY_HEIGHT] x 0.6 x 0.6, so tests
 * name the FEET height and convert here exactly once. */
static void park_player(double x, double feet_y, double z) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = x;
	s.player.y = feet_y + (double)PLAYER_EYE_HEIGHT;
	s.player.z = z;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.fire = -PLAYER_FIRE_RESISTANCE;
	s.player.oxygen = MAX_OXYGEN;
	s.player.fall_distance = 0.0F;
	s.player.vel_x = 0.0f;
	s.player.vel_y = 0.0f;
	s.player.vel_z = 0.0f;
	s.player.active_inventory = &s.player.inventory;
}

static bool cactus_contact_at(double x, double feet_y, double z) {
	park_player(x, feet_y, z);
	bool in_cactus = false;
	server_local_test_player_block_contact(&s, &in_cactus, NULL);
	return in_cactus;
}

/* ===================================================================
   (1) The collision box itself -- the precondition for every case below.
   =================================================================== */
static void test_collision_box_matches_beta(void) {
	struct block_data bd = {.type = BLOCK_CACTUS, .metadata = 0};
	struct block_info info = {.block = &bd, .neighbours = NULL,
							  .x = CX, .y = CY, .z = CZ};
	struct AABB collision, selection;
	assert(blocks[BLOCK_CACTUS]->getBoundingBox(&info, true, &collision) == 1);
	assert(blocks[BLOCK_CACTUS]->getBoundingBox(&info, false, &selection) == 1);

	/* getCollisionBoundingBoxFromPool: X/Z inset f both sides, Y from j to
	 * (j+1)-f -- box-local [0, 0.9375]. */
	assert(collision.x1 == 0.0625F && collision.x2 == 0.9375F);
	assert(collision.z1 == 0.0625F && collision.z2 == 0.9375F);
	assert(collision.y1 == 0.0F && collision.y2 == 0.9375F);
	/* getSelectedBoundingBoxFromPool: same X/Z, but maxY = j+1 (full height). */
	assert(selection.x1 == 0.0625F && selection.x2 == 0.9375F);
	assert(selection.z1 == 0.0625F && selection.z2 == 0.9375F);
	assert(selection.y1 == 0.0F && selection.y2 == 1.0F);
	printf("  collision box 0.875 x 0.9375 x 0.875, selection box full-height: "
		   "OK\n");
}

/* ===================================================================
   (2) Side contact on all four faces + the one-epsilon-outside miss.
   =================================================================== */
static void test_side_contact_four_faces(void) {
	/* Flush contact: the body is stopped by the cactus COLLISION box, so on the
	 * -X face the player centre sits at (CX + 0.0625) - 0.3. The sweep then
	 * evaluates floor(maxX - 0.001) = floor(CX + 0.0615) = CX -> the cactus. */
	const double flush_minus = (double)CX + CACTUS_INSET - PLAYER_HALF;
	const double flush_plus
		= (double)CX + 1.0 - CACTUS_INSET + PLAYER_HALF;
	const double feet = (double)CY; /* standing on the ground beside it */
	const double ctr = (double)CZ + 0.5;

	assert(cactus_contact_at(flush_minus, feet, ctr) && "-X face");
	assert(cactus_contact_at(flush_plus, feet, ctr) && "+X face");
	assert(cactus_contact_at((double)CX + 0.5, feet,
							 (double)CZ + CACTUS_INSET - PLAYER_HALF)
		   && "-Z face");
	assert(cactus_contact_at((double)CX + 0.5, feet,
							 (double)CZ + 1.0 - CACTUS_INSET + PLAYER_HALF)
		   && "+Z face");

	/* One epsilon further out on each face the sweep must NOT reach the cell.
	 * The exact boundary: contact needs floor(maxX - 0.001) >= CX, i.e.
	 * centre > CX - 0.3 + 0.001. Sitting at exactly CX - 0.3 makes
	 * maxX - 0.001 = CX - 0.001 -> floor = CX - 1. No damage. */
	assert(!cactus_contact_at((double)CX - PLAYER_HALF, feet, ctr)
		   && "-X exactly one epsilon outside must NOT damage");
	assert(!cactus_contact_at((double)CX + 1.0 + PLAYER_HALF, feet, ctr)
		   && "+X exactly one epsilon outside must NOT damage");
	assert(!cactus_contact_at((double)CX + 0.5, feet, (double)CZ - PLAYER_HALF)
		   && "-Z exactly one epsilon outside must NOT damage");
	assert(!cactus_contact_at((double)CX + 0.5, feet,
							  (double)CZ + 1.0 + PLAYER_HALF)
		   && "+Z exactly one epsilon outside must NOT damage");

	/* And a plain gap stays harmless. */
	assert(!cactus_contact_at((double)CX - 2.5, feet, ctr) && "2 blocks away");
	printf("  side contact on all four faces + exact epsilon-outside misses: "
		   "OK\n");
}

/* ===================================================================
   (3) Corner contact -- both horizontal axes reach in at once.
   =================================================================== */
static void test_corner_contact(void) {
	const double feet = (double)CY;
	const double lo_x = (double)CX + CACTUS_INSET - PLAYER_HALF;
	const double hi_x = (double)CX + 1.0 - CACTUS_INSET + PLAYER_HALF;
	const double lo_z = (double)CZ + CACTUS_INSET - PLAYER_HALF;
	const double hi_z = (double)CZ + 1.0 - CACTUS_INSET + PLAYER_HALF;
	assert(cactus_contact_at(lo_x, feet, lo_z) && "-X-Z corner");
	assert(cactus_contact_at(lo_x, feet, hi_z) && "-X+Z corner");
	assert(cactus_contact_at(hi_x, feet, lo_z) && "+X-Z corner");
	assert(cactus_contact_at(hi_x, feet, hi_z) && "+X+Z corner");
	printf("  corner contact on all four corners: OK\n");
}

/* ===================================================================
   (4) Standing ON TOP -- the case the 1/16 top inset exists for.
   =================================================================== */
static void test_standing_on_top(void) {
	/* Feet rest on the collision box top: CY + 0.9375. floor(minY + 0.001) =
	 * floor(CY + 0.9385) = CY -> the cactus cell itself. */
	assert(cactus_contact_at((double)CX + 0.5, (double)CY + 1.0 - CACTUS_INSET,
							 (double)CZ + 0.5)
		   && "standing on a cactus must damage");
	/* The pre-fix full-height 1.0 box would have parked the feet HERE, where
	 * floor(minY + 0.001) = CY + 1 (air) -- exactly why the old box hid it. */
	assert(!cactus_contact_at((double)CX + 0.5, (double)CY + 1.0,
							  (double)CZ + 0.5)
		   && "a full 1.0-high box would float the feet clear of the cell");
	printf("  standing on top damages via the 1/16 collision-top inset: OK\n");
}

/* ===================================================================
   (5) The damage itself: 1 Beta point, i-frame gated.
   =================================================================== */
static void test_damage_amount_and_iframes(void) {
	const int POINT = HEALTH_PER_HEART / 2; /* 1 Beta point == 8 hp */
	park_player((double)CX + CACTUS_INSET - PLAYER_HALF, (double)CY,
				(double)CZ + 0.5);
	short before = s.player.health;

	/* Contact tick 1: BlockCactus.onEntityCollidedWithBlock -> 1 point. */
	assert(server_local_test_apply_player_hazards(&s, false, false, false,
												  true));
	assert(s.player.health == before - POINT);

	/* Beta calls attackEntityFrom EVERY contact tick, but the hurtResistantTime
	 * window absorbs an equal-or-weaker follow-up hit
	 * [740c583: EntityLiving.attackEntityFrom -- the port's arbitration is
	 * server_local.c player_iframe_damage]. server_local_update ticks that
	 * window down ONCE per server tick, before the hazard sweep; mirror exactly
	 * that order here so the cadence is the production one. */
	assert(s.player.hurt_resist_time == PLAYER_HURT_RESIST_TICKS);
	int first_gap = -1, hits = 0;
	for(int t = 1; t <= 4 * PLAYER_HURT_RESIST_TICKS; t++) {
		if(s.player.hurt_resist_time > 0)
			s.player.hurt_resist_time--;
		short h = s.player.health;
		assert(server_local_test_apply_player_hazards(&s, false, false, false,
													  true));
		if(s.player.health < h) {
			assert(h - s.player.health == POINT);
			if(first_gap < 0)
				first_gap = t;
			hits++;
		}
	}
	/* Hand-derived: PLAYER_HURT_RESIST_TICKS is 10, not 20 (combat.h:143-146 --
	 * Beta's maxHurtResistantTime IS 20, but attackEntityFrom only absorbs an
	 * equal-or-weaker hit while hurtResistantTime > max/2, so the effective
	 * re-damage window for a repeated 1-point cactus hit is 10 ticks). The
	 * counter opens at 10 and drains one per tick, so the next hit lands on the
	 * tick it reaches 0 -- exactly every 10 ticks. */
	assert(first_gap == PLAYER_HURT_RESIST_TICKS
		   && "the second cactus hit must wait out the full i-frame window");
	assert(hits == 4
		   && "one 1-point hit per 10-tick i-frame window, not per tick");

	/* No contact -> no damage, however long we stand there. */
	park_player((double)CX - 2.5, (double)CY, (double)CZ + 0.5);
	short idle = s.player.health;
	for(int t = 0; t < 40; t++) {
		if(s.player.hurt_resist_time > 0)
			s.player.hurt_resist_time--;
		assert(server_local_test_apply_player_hazards(&s, false, false, false,
													  false));
	}
	assert(s.player.health == idle);
	printf("  1 point per contact (%d hp), one hit per %d-tick i-frame window "
		   "(%d hits in %d ticks): OK\n",
		   POINT, PLAYER_HURT_RESIST_TICKS, hits,
		   4 * PLAYER_HURT_RESIST_TICKS);
}

/* ===================================================================
   (6) The MOB path -- entity_monster_post_move_cactus_collisions runs the
       same Beta sweep against the mob's own box.
   =================================================================== */
static struct entity* spawn_entity_slot(void) {
	uint32_t id = g_next_id++;
	struct entity** e_ptr = dict_entity_safe_get(s.entities, id);
	*e_ptr = calloc(1, sizeof(struct entity));
	assert(*e_ptr);
	(*e_ptr)->id = id;
	return *e_ptr;
}

static void test_mob_side_contact(void) {
	/* A pig is 0.9 wide (half 0.45); flush against the -X face its centre sits
	 * at (CX + 0.0625) - 0.45, and floor(maxX - 0.001) = CX. */
	const float half = 0.45F;
	struct entity* mob = spawn_entity_slot();
	entity_monster(mob->id, mob, true, &s.world, MONSTER_PIG);
	mob->teleport(mob,
				  (vec3) {(float)CX + (float)CACTUS_INSET - half, (float)CY,
						  (float)CZ + 0.5F});
	short hp = mob->health;
	assert(hp > 0);
	entity_monster_post_move_cactus_collisions(mob, &s);
	assert(mob->health < hp
		   && "a mob pressed against a cactus takes MOB_CACTUS_DAMAGE");
	assert(hp - mob->health == MOB_CACTUS_DAMAGE);

	/* Same epsilon boundary as the player: one step further out, nothing. */
	struct entity* clear = spawn_entity_slot();
	entity_monster(clear->id, clear, true, &s.world, MONSTER_PIG);
	clear->teleport(clear,
					(vec3) {(float)CX - half, (float)CY, (float)CZ + 0.5F});
	short hp2 = clear->health;
	entity_monster_post_move_cactus_collisions(clear, &s);
	assert(clear->health == hp2 && "one epsilon clear of the cell: no damage");
	printf("  mob side contact deals MOB_CACTUS_DAMAGE (%d); epsilon-clear mob "
		   "untouched: OK\n",
		   MOB_CACTUS_DAMAGE);
}

/* ===================================================================
   (7) The FIRE scan's chunk preflight is CHUNK-ranged, not block-ranged.

   World.isBoundingBoxBurning hands checkChunksExist the +1 bounds it then uses
   EXCLUSIVELY for the cells (j = floor(maxX + 1.0)), and checkChunksExist
   scans i>>4 .. j>>4 INCLUSIVE [740c583: World.java:1590-1598 and :335-351].
   So a body box whose maxX+1 lands in the NEXT chunk bails in Beta whenever
   that chunk is unresolved -- even though no cell in it is ever read.
   =================================================================== */
static void test_fire_chunk_seam_preflight(void) {
	/* The fixture loads chunks (0..1, 0..1) only, so chunk 2 on X is missing.
	 * Park the player in chunk 1 hard against the seam: minX = 31.401 -> x0=31
	 * (chunk 1), and floor(maxX + 1.0) = floor(32.999) = 32 -> chunk 2. */
	const double seam_x = 31.7;
	const double feet = (double)CY;
	set(31, CY, CZ, BLOCK_FIRE);
	struct block_data placed;
	assert(server_world_get_block(&s.world, 31, CY, CZ, &placed)
		   && placed.type == BLOCK_FIRE);

	park_player(seam_x, feet, (double)CZ + 0.5);
	bool in_fire = true;
	server_local_test_player_block_contact(&s, NULL, &in_fire);
	assert(!in_fire
		   && "checkChunksExist must fail on the +1 bound's own chunk: Beta "
			  "bails, so the port must bail");

	/* Resolve exactly that chunk and the identical position now burns. This is
	 * the control that proves the bail above was the PREFLIGHT and not a
	 * missing fire block or a bad bound. */
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 2, 0, &sc) && sc);
	park_player(seam_x, feet, (double)CZ + 0.5);
	in_fire = false;
	server_local_test_player_block_contact(&s, NULL, &in_fire);
	assert(in_fire && "with the seam chunk resolved the same box burns");

	set(31, CY, CZ, BLOCK_AIR);
	printf("  fire preflight bails on the +1 bound's chunk (seam), passes once "
		   "resolved: OK\n");
}

/* ===================================================================
   (8) Fire damage is NOT suppressed by being wet -- only the ignition
       counter is. [740c583: Entity.moveEntity:505-514]
         boolean flag2 = isWet();
         if(isBoundingBoxBurning(...)) {
             dealFireDamage(1);
             if(!flag2) { fire++; if(fire == 0) fire = 300; }
         }
   =================================================================== */
static void test_fire_while_wet(void) {
	const int POINT = HEALTH_PER_HEART / 2;

	/* DRY + fire: 1 point AND the ignition counter advances. */
	park_player((double)CX - 4.5, (double)CY, (double)CZ + 0.5);
	s.player.fire = 0;
	short before = s.player.health;
	assert(server_local_test_apply_player_hazards(&s, false, false, true,
												  false));
	assert(s.player.health == before - POINT);
	assert(s.player.fire == 1 && "dry contact advances fire++");

	/* WET + fire: still 1 point (dealFireDamage sits outside the isWet guard),
	 * but no ignition -- the water branch zeroes fire and !flag2 never runs.
	 * The pre-fix `!body_in_water && in_fire` condition took BOTH away. */
	park_player((double)CX - 4.5, (double)CY, (double)CZ + 0.5);
	s.player.fire = 0;
	before = s.player.health;
	assert(server_local_test_apply_player_hazards(&s, true, false, true,
												  false));
	assert(s.player.health == before - POINT
		   && "a wet player standing in fire still takes the contact point");
	assert(s.player.fire == 0 && "isWet() suppresses fire++, nothing else");
	printf("  wet player in fire: contact damage kept, ignition suppressed: "
		   "OK\n");
}

/* ===================================================================
   (9) Dropped items take the same sweep. EntityItem OVERRIDES
       attackEntityFrom (`health -= i; if(health <= 0) setEntityDead();`)
       over a constructor health of 5 [740c583: EntityItem.java:22,43,108-116],
       so a stack resting against a cactus dies on the FIFTH contact tick.
   =================================================================== */
static struct entity* spawn_item_at(double x, double y, double z) {
	struct entity* e = spawn_entity_slot();
	entity_item(e->id, e, true, &s.world,
				(struct item_data) {.id = BLOCK_STONE, .durability = 0,
									.count = 1, .nbt_record = 0});
	/* Keep the collector out of it: the pickup gate is a 2-block radius, and a
	 * nonzero delayBeforeCanPickup is Beta's own drop default anyway. */
	e->data.item.pickup_delay = 1000;
	e->teleport(e, (vec3) {(float)x, (float)y, (float)z});
	return e;
}

static void test_item_cactus_death(void) {
	/* Item box: 0.25 cube, y_offset 0.125 -> world box is pos +/- 0.125 on
	 * every axis. Resting on the ground beside the cactus puts pos.y at
	 * CY + 0.125, so floor(minY + 0.001) = CY -- the cactus cell's own row.
	 * Flush against the -X face the stack is stopped by the 1/16-inset
	 * collision box, so pos.x = (CX + 0.0625) - 0.125 and
	 * floor(maxX - 0.001) = floor(CX + 0.0615) = CX. */
	const double item_half = 0.125;
	park_player((double)CX + 40.0, (double)CY, (double)CZ + 0.5);

	struct entity* touching
		= spawn_item_at((double)CX + CACTUS_INSET - item_half,
						(double)CY + item_half, (double)CZ + 0.5);
	assert(touching->health == 5 && "EntityItem constructor health");
	for(int tick = 1; tick <= 5; tick++) {
		/* Re-pin the resting position each tick (gravity/friction otherwise
		 * drift the stack off the face); the sweep still runs on the POST-move
		 * box, which is what is under test. */
		touching->teleport(touching,
						   (vec3) {(float)((double)CX + CACTUS_INSET
										   - item_half),
								   (float)((double)CY + item_half),
								   (float)((double)CZ + 0.5)});
		glm_vec3_zero(touching->vel);
		bool destroyed = touching->tick_server(touching, &s);
		if(tick < 5) {
			assert(!destroyed && "an item survives the first four hits");
			assert(touching->health == 5 - tick);
		} else {
			assert(destroyed && "the fifth contact tick sets it dead");
			assert(touching->health <= 0);
		}
	}
	free(touching);
	dict_entity_erase(s.entities, g_next_id - 1);

	/* One epsilon further out (pos.x = CX - 0.125 -> maxX - 0.001 = CX - 0.001
	 * -> floor = CX - 1) the sweep never reaches the cell: full health. */
	struct entity* clear = spawn_item_at((double)CX - item_half,
										 (double)CY + item_half,
										 (double)CZ + 0.5);
	for(int tick = 0; tick < 10; tick++) {
		clear->teleport(clear, (vec3) {(float)((double)CX - item_half),
									   (float)((double)CY + item_half),
									   (float)((double)CZ + 0.5)});
		glm_vec3_zero(clear->vel);
		assert(!clear->tick_server(clear, &s)
			   && "an item one epsilon clear of the cell is never damaged");
	}
	assert(clear->health == 5);
	free(clear);
	dict_entity_erase(s.entities, g_next_id - 1);
	printf("  dropped item dies on the 5th cactus contact tick; epsilon-clear "
		   "item untouched: OK\n");
}

int main(void) {
	test_fixture_dir("test_cactus_contact_damage_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	clin_init();
	assert(svin_init());
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;

	/* Sand floor + the cactus, so the block can legitimately stay
	 * (BlockCactus.canBlockStay); the surrounding cells stay air. */
	for(int dx = -3; dx <= 3; dx++)
		for(int dz = -3; dz <= 3; dz++) {
			set(CX + dx, Y_GROUND, CX + dz, BLOCK_SAND);
			set(CX + dx, CY, CZ + dz, BLOCK_AIR);
			set(CX + dx, CY + 1, CZ + dz, BLOCK_AIR);
		}
	set(CX, Y_GROUND, CZ, BLOCK_SAND);
	set(CX, CY, CZ, BLOCK_CACTUS);
	struct block_data planted;
	assert(server_world_get_block(&s.world, CX, CY, CZ, &planted)
		   && planted.type == BLOCK_CACTUS);

	printf("test_cactus_contact_damage:\n");
	test_collision_box_matches_beta();
	test_side_contact_four_faces();
	test_corner_contact();
	test_standing_on_top();
	test_damage_amount_and_iframes();
	test_mob_side_contact();
	test_fire_chunk_seam_preflight();
	test_fire_while_wet();
	test_item_cactus_death();

	g_drain_run = 0;
	thread_join(&drain);
	string_clear(level);
	printf("PASS test_cactus_contact_damage\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
