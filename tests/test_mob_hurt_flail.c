/*
	Beta's hurt-flail: the walk amplitude slams to 1.5F on damage.

	WHAT THIS PINS. Beta 1.7.3 sets `legYaw = 1.5F` when a mob is damaged —
	DELIBERATELY above the 1.0F ceiling the ordinary lerp can ever reach
	(`legYaw += (var7 - legYaw) * 0.4F` with `var7 <= 1.0F`,
	EntityLiving.java:546) — and RenderLiving feeds it straight into the limb
	swing. So a hit Beta mob visibly scrambles its legs for a few ticks.

	WHY IT MATTERS MORE THAN IT LOOKS. Beta passive animals have NO flee or
	panic AI — verified exhaustively against both the client and server trees:
	EntityAnimal is 30 lines with no damage response, findPlayerToAttack()
	returns null, and Beta has no flee primitive anywhere. A hit cow just keeps
	wandering, and that IS faithful. This flail is therefore the ONLY visible
	"something happened" on a struck passive mob. Without it our animals read as
	inert in a way Beta's do not — which is exactly the complaint that gets
	misdiagnosed as "the mobs have no AI".

	THE TRIGGER IS AN EDGE, NOT A LEVEL. Beta's client sets this from a one-shot
	packet — handleHealthUpdate case 2 does `legYaw = 1.5F` alongside
	`hurtTime = 10` — not every tick the mob is hurt. Our `entity.hurt` flag is
	a level (true while hurt_time > 0, ~10 ticks), so the RISING EDGE is the
	faithful analogue. A second hit inside that window does not re-slam, which
	also matches Beta: an in-window hit that is not strictly stronger is
	absorbed and never reaches the setter.

	[740c583: EntityLiving.attackEntityFrom:315, .handleHealthUpdate:850-856,
	 .onLivingUpdate:546]

	ADDENDUM (mob-AI fix wave): also pins the idle head-turn locomotion fix
	(entity_monster.c's mob_idle_look_tick) via this file's existing
	client_tick harness -- see the test at the end of main().
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "game/game_state.h"

static struct entity* make_mob(struct entity* e, int monster_id) {
	/* POISON FIRST. Client mobs arrive via CRPC_SPAWN_MONSTER -> malloc, so a
	 * field the constructor forgets is recycled heap. An uninitialised
	 * hurt_prev == true would swallow the very first flail and this test would
	 * pass for the wrong reason on a zeroed heap. */
	memset(e, 0xBE, sizeof(*e));
	/* The client ctor only STORES the world pointer; the client tick under test
	 * never dereferences it. Pass the real (statically zeroed) client world
	 * rather than a fake pointer, so a future tick that does touch it fails
	 * loudly here instead of corrupting memory. */
	entity_monster(1234, e, false, &gstate.world, monster_id);
	assert(!e->data.monster.hurt_prev
		   && "hurt edge state must be initialised, not heap garbage");
	assert(e->data.monster.limb_swing == 0.0f);
	return e;
}

/* Run the client tick with the mob held still (no position delta -> the walk
 * amplitude decays toward 0), which is the harshest case for the flail: any
 * value above 0 must have come from the hurt slam, not from walking. */
static void tick_still(struct entity* e) {
	glm_vec3_copy(e->pos, e->network_pos);
	assert(e->tick_client);
	e->tick_client(e);
}

int main(void) {
	struct entity mob;

	/* ---------------------------------------------------------------
	 * A standing, unhurt mob keeps its legs at rest.
	 * --------------------------------------------------------------- */
	make_mob(&mob, MONSTER_COW);
	mob.hurt = false;
	for(int i = 0; i < 5; i++)
		tick_still(&mob);
	assert(mob.data.monster.limb_swing < 0.01f
		   && "an idle mob's legs must stay at rest");

	/* ---------------------------------------------------------------
	 * DAMAGE slams the amplitude ABOVE the ordinary 1.0 ceiling.
	 * --------------------------------------------------------------- */
	mob.hurt = true;
	tick_still(&mob);
	assert(mob.data.monster.limb_swing > 1.0f
		   && "a hit mob must flail ABOVE the normal walk ceiling (Beta 1.5F)");
	assert(fabsf(mob.data.monster.limb_swing - 1.5f) < 1e-6f
		   && "the flail magnitude is Beta's 1.5F exactly");

	/* ---------------------------------------------------------------
	 * It DECAYS over the following ticks rather than latching.
	 * --------------------------------------------------------------- */
	float prev = mob.data.monster.limb_swing;
	for(int i = 0; i < 3; i++) {
		tick_still(&mob);
		assert(mob.data.monster.limb_swing < prev
			   && "the flail must decay, not latch");
		prev = mob.data.monster.limb_swing;
	}

	/* ---------------------------------------------------------------
	 * LEVEL, NOT EDGE: while `hurt` stays true it must NOT re-slam every
	 * tick. Beta sets this from a one-shot packet; a level trigger would
	 * pin the legs at 1.5 for the whole 10-tick window.
	 * --------------------------------------------------------------- */
	assert(mob.hurt && "still inside the hurt window for this check");
	assert(mob.data.monster.limb_swing < 1.0f
		   && "a sustained hurt flag must not re-slam the flail each tick");

	/* ---------------------------------------------------------------
	 * A NEW hit (flag drops, then rises again) flails afresh.
	 * --------------------------------------------------------------- */
	mob.hurt = false;
	tick_still(&mob);
	mob.hurt = true;
	tick_still(&mob);
	assert(fabsf(mob.data.monster.limb_swing - 1.5f) < 1e-6f
		   && "a fresh hit must flail again");

	/* ---------------------------------------------------------------
	 * PASSIVE mobs flail too. They have no flee AI in Beta, so this is
	 * the only visible reaction they have -- the whole point.
	 * --------------------------------------------------------------- */
	const int passives[] = {MONSTER_PIG, MONSTER_COW, MONSTER_CHICKEN,
							MONSTER_SHEEP};
	for(size_t i = 0; i < sizeof(passives) / sizeof(passives[0]); i++) {
		struct entity p;
		make_mob(&p, passives[i]);
		p.hurt = true;
		tick_still(&p);
		assert(fabsf(p.data.monster.limb_swing - 1.5f) < 1e-6f
			   && "every passive animal must show the hurt flail");
	}

	/* ---------------------------------------------------------------
	 * IDLE HEAD-TURN LOCOMOTION (mob-AI fix wave): a genuinely idle mob
	 * (held still, no networked look armed) must eventually turn its head
	 * instead of freezing forever -- Beta's EntityLiving.
	 * updatePlayerActionState fallback is head-turn-only, not a full
	 * freeze [740c583]. EntitySquid / EntityGhast / EntitySlime each fully
	 * override updatePlayerActionState and never reach that fallback in
	 * Beta, so they must stay frozen (unchanged pre-fix behaviour) while a
	 * generic hostile (zombie) does not. See entity_monster.c's
	 * mob_idle_look_tick.
	 * --------------------------------------------------------------- */
	gstate.rand_src.seed = 0xA17DE001u;
	gstate.players[0].camera.x = 1000.0f; // far from every mob spawned below: exercises
	gstate.players[0].camera.y = 64.0f;   // the random-drift branch, not the acquire
	gstate.players[0].camera.z = 1000.0f; // branch (dist_sq always >> MOB_IDLE_LOOK_RANGE_SQ)

	struct entity zombie;
	make_mob(&zombie, MONSTER_ZOMBIE);
	float zombie_yaw0 = zombie.data.monster.head_yaw;
	bool zombie_turned = false;
	for(int i = 0; i < 2000 && !zombie_turned; i++) {
		tick_still(&zombie);
		if(fabsf(zombie.data.monster.head_yaw - zombie_yaw0) > 1.0e-4f)
			zombie_turned = true;
	}
	assert(zombie_turned
		   && "a genuinely idle zombie must eventually turn its head instead "
			  "of freezing forever");

	const int excluded[] = {MONSTER_SQUID, MONSTER_GHAST, MONSTER_SLIME_SMALL};
	for(size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++) {
		struct entity e;
		make_mob(&e, excluded[i]);
		float yaw0 = e.data.monster.head_yaw;
		for(int t = 0; t < 2000; t++)
			tick_still(&e);
		assert(fabsf(e.data.monster.head_yaw - yaw0) < 1.0e-6f
			   && "squid/ghast/slime fully override updatePlayerActionState "
				  "in Beta and must NOT get the generic idle-look turn");
	}

	printf("test_mob_hurt_flail: Beta 1.5F hurt flail, edge-triggered, decays, "
		   "all passives OK; idle head-turn engages for a generic hostile and "
		   "correctly excludes squid/ghast/slime\n");
	return 0;
}
