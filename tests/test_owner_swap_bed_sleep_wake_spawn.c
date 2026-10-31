/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924/A1d, P2 inventory row #3: sleeping in a bed,
 * the Overworld-only sleep quorum, waking and the bed spawn, owner-swapped.
 *
 * Beta citations (client Jad decompile,
 * ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   - BlockBed.blockActivated, BlockBed.java:14-85: normalize the clicked half
 *     to the foot (:19-27), the occupied scan over the world's playerEntities
 *     (:48-64, "tile.bed.occupied"), then sleepInBedAt(x, y, z) for the
 *     CLICKING player (:70) and `setBedOccupied(.., true)` on OK (:72).
 *   - EntityPlayer.sleepInBedAt, EntityPlayer.java:537-596: OTHER_PROBLEM when
 *     already asleep or dead (:539-541), NOT_POSSIBLE_HERE in the Nether
 *     (:543-545), NOT_POSSIBLE_NOW by day (:547-549), TOO_FAR_AWAY beyond
 *     |dx|>3 / |dy|>2 / |dz|>3 (:551-553); sets sleeping = true,
 *     `sleepTimer = 0`, `bedChunkCoordinates` (:583-585), zeroes motion (:586)
 *     and calls `worldObj.updateAllPlayersSleepingFlag()` (:587-589) -- EVERY
 *     player in the world is a sleeper in its own right.
 *   - EntityPlayer.wakeUpPlayer, EntityPlayer.java:613-646: `sleeping = false`
 *     (:628), and with setSpawn the player's OWN spawnChunk becomes the bed.
 *   - World.updateAllPlayersSleepingFlag, World.java:2351-2363 (server
 *     World.java:1821-1828): allPlayersSleeping = !playerEntities.isEmpty()
 *     AND every player isPlayerSleeping -- ONE awake player blocks the skip.
 *   - World.isAllPlayersFullyAsleep, World.java:2379-2394 (server :1839-1850)
 *     and World.tick, World.java:1748-1752 (server :1350-1358): only when ALL
 *     are fully asleep does the time jump to the next dawn and
 *     wakeUpAllPlayers runs `wakeUpPlayer(false, false, true)` for EACH
 *     sleeper (World.java:2365-2377 / server :1830-1837) -- every sleeper
 *     gets its own bed spawn.
 *   - Server: EntityPlayer.readEntityFromNBT, EntityPlayer.java:344-350
 *     (/c/tmp/mcb173-server-decompile): "Sleeping" is per player and a body
 *     loaded asleep is woken at once.
 *
 * CaveWii today: block_bed.c's onRightClick refuses every non-host actor
 * (S1-a1b fail-closed) and server_local_begin_sleep / server_local_sleep_tick
 * are host-singular -- the sleep timer, the quorum and the wake spawn all read
 * s->player only. So on the base: the guest cannot enter a bed (RED), the host
 * alone skips the night while the guest stands awake beside it (RED -- Beta's
 * quorum counts every Overworld player), and the guest never receives a bed
 * spawn from a shared wake (RED). The Nether legs ("Nether actors neither
 * join nor block") wait on A2's second live world and are not driven here.
 *
 * Two ctest names, one executable (argv[1]):
 *   base  -- the host reference legs that pass on the base: sleep entry, the
 *            body snap into the bed, no timer reset on a repeated click, and
 *            the bed-destroyed instant wake that leaves the spawn untouched.
 *   red   -- guest sleep entry, the two-actor quorum and the shared wake spawn.
 *            GREEN since the A1d wiring (WILL_FAIL removed).
 *   wake  -- A1d review C1: occupied both ways, sleeping-guest checkpoint,
 *            damage wakes only the hit body (both ways, lethal too). */
#include "support/owner_swap_harness.h"
#include "daytime.h"
#include "game/difficulty.h"
#include "network/level_archive.h"

#define TEST_NAME "test_owner_swap_bed_sleep_wake_spawn"

/* server_local.c test hook (declared locally, as test_sleep_spawn_golden does). */
void server_local_test_set_difficulty(int d);

/* Night: 18000 is midnight (World.isDaytime false, no weather). */
#define NIGHT_TICKS 18000u

/* A bed lying along +Z: foot at (x, 4, z) meta 0, head at (x, 4, z+1) meta
 * 0|8 (item_bed.c: metadata = dir<<1, bit 0x8 = head; block_bed.c bed_dir[0]
 * = {0, +1}). Both cells are air above the superflat grass at y == 3. */
static void lay_bed(struct server_local* s, int x, int z) {
	put_block(s, x, 4, z, BLOCK_BED, 0);
	put_block(s, x, 4, z + 1, BLOCK_BED, 8);
}
/* Right-click the bed's FOOT cell from above (no held item). */
static struct server_rpc bed_click(int x, int z) {
	return (struct server_rpc) {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = 4, .z = z, .side = SIDE_TOP,
			.has_held_item = false}};
}
static bool asleep_in(const struct server_player* p, int hx, int hz) {
	return p->sleeping && p->sleep_x == hx && p->sleep_y == 4 && p->sleep_z == hz;
}
static bool spawn_is(const struct server_player* p, int hx, int hz) {
	return p->has_bed_spawn && p->bed_spawn_x == hx && p->bed_spawn_y == 4
		&& p->bed_spawn_z == hz;
}
/* Every sleep tick publishes CRPC_SLEEP_STATE (and the wake edge a
 * CRPC_PLAYER_POS) into the 16-deep bounded client inbox, whose sender BLOCKS
 * when it is full -- there is no consumer thread on this paused rig, so the
 * inbox is drained after every tick, exactly as drain_server does per RPC. */
static void sleep_ticks(struct server_local* s, int n) {
	for(int i = 0; i < n; i++) {
		server_local_sleep_tick(s);
		clin_update_drain();
	}
}

/* Host bed at x=2 (foot z=0, head z=1); guest stands at x=6.5 with its own bed
 * at x=6. Both are inside Beta's 3/2/3 reach of their own head cell and
 * outside it for the other's. */
static struct server_local* bedroom(void) {
	struct server_local* s = admitted_fixture();
	s->world_time = NIGHT_TICKS;
	s->weather.raining = false; s->weather.thundering = false;
	s->weather.rain_strength = 0.0F; s->weather.thunder_strength = 0.0F;
	assert(!daytime_is_daytime_ticks(s->world_time, 0.0F, 0.0F));
	body_stand_at(s->guest_actor, 6.5, 0.5);
	lay_bed(s, 2, 0);
	lay_bed(s, 6, 0);
	assert(block_at(s, 2, 4, 1) == BLOCK_BED && block_at(s, 6, 4, 1) == BLOCK_BED);
	return s;
}

/* ======================================================================== */
static void case_base(void) {
	struct server_local* s = bedroom();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;

	counters_reset();
	struct server_rpc click = bed_click(2, 0);
	host_send(s, &click);
	check(asleep_in(h, 2, 1),
		"host: right-clicking the bed's foot enters sleep on its HEAD cell "
		"(BlockBed.blockActivated:19-27 -> EntityPlayer.sleepInBedAt:583-585)");
	check(h->sleep_timer == 0 && sleep_state_msgs >= 1 && last_sleep_state,
		"host: sleepTimer starts at 0 and the client is told it is asleep");
	check(fabs(h->y - (4.0 + 0.9375)) < 1e-9 && h->vel_x == 0.0F && h->vel_z == 0.0F,
		"host: the body is snapped into the bed with its motion zeroed "
		"(EntityPlayer.sleepInBedAt:586)");
	check(!g->sleeping, "guest: untouched by the host's sleep");

	/* Repeated click: OTHER_PROBLEM for an already-sleeping player -- the
	 * timer must NOT reset (EntityPlayer.sleepInBedAt:539-541). */
	sleep_ticks(s, 5);
	check(h->sleep_timer == 5, "host: five sleep ticks -> sleepTimer 5");
	host_send(s, &click);
	check(h->sleeping && h->sleep_timer == 5,
		"host: a repeated bed click does not reset the timer (OTHER_PROBLEM)");

	/* Bed destroyed mid-sleep: !isInBed -> wakeUpPlayer(true, true, false):
	 * instant wake, spawn UNTOUCHED (EntityPlayer.onUpdate:66-69). */
	bool had_spawn = h->has_bed_spawn;
	put_block(s, 2, 4, 1, BLOCK_AIR, 0);
	sleep_ticks(s, 1);
	check(!h->sleeping && h->sleep_timer == 0,
		"host: the bed vanishing wakes the sleeper at once");
	check(h->has_bed_spawn == had_spawn,
		"host: that wake leaves the bed spawn exactly as it was");
	check(s->world_time == NIGHT_TICKS,
		"host: no night skip happened (sleep ticks alone never move the clock)");
	teardown(s);
}

/* ======================================================================== */
static void case_red(void) {
	struct server_local* s = bedroom();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;

	/* (a) Owner-swapped sleep ENTRY. */
	struct server_rpc hclick = bed_click(2, 0);
	host_send(s, &hclick);
	check(asleep_in(h, 2, 1), "setup: host asleep in its own bed");
	counters_reset();
	struct server_rpc gclick = bed_click(6, 0);
	check(guest_send(s, &gclick), "queue: the guest's bed click is admitted");
	check(asleep_in(g, 6, 1),
		"guest: right-clicking ITS bed enters sleep on that bed's head "
		"(BlockBed.blockActivated:70 sleepInBedAt runs for the CLICKING player)"
		"  [row #3: RED on base -- block_bed.c refuses a non-host actor]");
	check(!g->sleeping || (fabs(g->y - (4.0 + 0.9375)) < 1e-9
		&& g->vel_x == 0.0F && g->vel_z == 0.0F),
		"guest: if asleep, the body was snapped into the bed with motion zeroed");
	check(asleep_in(h, 2, 1), "host: still asleep in ITS bed, untouched");

	/* (b) The quorum: with the guest AWAKE, 101 host sleep ticks must NOT skip
	 * the night (World.updateAllPlayersSleepingFlag:2351-2363 -- one awake
	 * Overworld player keeps allPlayersSleeping false). The guest is awake
	 * here whether or not (a) landed. */
	g->sleeping = false;
	g->sleep_timer = 0;
	uint64_t before = s->world_time;
	sleep_ticks(s, 101);
	check(h->sleeping && h->sleep_timer == 100,
		"host: still asleep with the timer saturated at 100 "
		"(EntityPlayer.onUpdate:60-65) while the guest is awake  [row #3: RED "
		"on base -- the host-only skip wakes it]");
	check(s->world_time < DAY_LENGTH_TICKS && s->world_time == before,
		"quorum: an AWAKE guest blocks the night skip "
		"(World.isAllPlayersFullyAsleep:2379-2394)  [row #3: RED on base -- "
		"the tick counts the host alone]");

	/* (c) Both asleep -> the skip, and EACH sleeper wakes with ITS OWN bed as
	 * spawn (World.wakeUpAllPlayers:2365-2377 -> wakeUpPlayer(false, false,
	 * true) per player). The host re-enters its bed through the production
	 * click if (b)'s host-only skip woke it; the guest's sleep is installed as
	 * setup state (the same injection the pose barrier test uses) so this leg
	 * measures the shared wake even while (a) is RED.
	 *
	 * A1d fix to this leg (found when it first ran on the wired tree): with
	 * hostile spawns allowed, World.tick runs performSleepSpawning FIRST
	 * (World.java:1740-1747) and a mob placed by the bed wakes that sleeper
	 * and cancels the skip -- whether it does depends on the world RNG state
	 * (a) and (b) left behind, so the leg was not measuring the wake at all.
	 * Peaceful disables that branch (difficultySetting < 1); the sleep spawn
	 * itself is golden-tested in test_sleep_spawn_golden. */
	server_local_test_set_difficulty(DIFFICULTY_PEACEFUL);
	s->world_time = NIGHT_TICKS;
	if(!h->sleeping)
		host_send(s, &hclick);
	check(asleep_in(h, 2, 1), "setup: host back asleep in its own bed at night");
	h->sleep_timer = 0;
	h->has_bed_spawn = false;
	g->has_bed_spawn = false;
	g->sleeping = true;
	g->sleep_timer = 0;
	g->sleep_x = 6; g->sleep_y = 4; g->sleep_z = 1;
	sleep_ticks(s, 101);
	/* sleep_ticks calls server_local_sleep_tick without the tick's own
	 * `world_time++`, so the skip reads tick-start T = 17999 and lands on
	 * Beta's D + 1 = 24001 (the w1 rule, World.tick:1748-1751 then :1765-1770;
	 * test_sleep_spawn_golden case_skip_end_tick). */
	check(s->world_time == DAY_LENGTH_TICKS + 1,
		"quorum: with BOTH asleep the world advances to the next dawn "
		"(World.tick:1748-1752; D + 1 = 24001)");
	check(!h->sleeping && spawn_is(h, 2, 1),
		"host: woken by the shared wake with ITS bed head as spawn");
	check(!g->sleeping && spawn_is(g, 6, 1),
		"guest: woken by the SAME wake with ITS OWN bed head as spawn "
		"(wakeUpAllPlayers runs wakeUpPlayer(.., setSpawn=true) for every "
		"sleeper)  [row #3: RED on base]");
	check(!spawn_is(g, 2, 1),
		"guest: never handed the HOST's bed as its spawn");

	/* Leave both awake for the checkpointed release. */
	h->sleeping = false; g->sleeping = false;
	teardown(s);
}

/* ======================================================================== */
/* A1d review C1 (A1d-REVIEW-opus-sub.md): committed asserts for the behaviour
 * A1d added beside the RED legs --
 *   - "This bed is occupied" both ways (BlockBed.blockActivated:48-64): the
 *     clicker gets the toast, does not sleep, and the sleeper's timer is
 *     untouched;
 *   - a sleeping guest checkpoints (so host autosave -- whose first step is
 *     server_local_guest_checkpoint -- is no longer refused) and the record's
 *     Pos is the Beta post-load wake cell (EntityPlayer.readEntityFromNBT:
 *     312-318 -> wakeUpPlayer(true, true, false)), live body untouched;
 *   - damage wakes ONLY the body that was hit, both directions
 *     (EntityPlayer.attackEntityFrom wakes `this`), and a lethal hit wakes the
 *     sleeping guest.
 * Guest bed at x=6 (head (6,4,1)): its step-out cell is (5, 4, 0) -- the first
 * normal-cube-floor / two-air cell of BlockBed.getNearestEmptyChunkCoordinates'
 * 3x3 (x outer, z inner) -- so the wake position is (5.5, 4 + 0.1 + 1.62, 0.5). */
static struct server_rpc bed_click_any(int x, int z) { return bed_click(x, z); }
static void hit(struct server_local* s, struct server_player* p, int damage) {
	p->hurt_resist_time = 0;
	bool accepted = false;
	(void)server_local_damage_actor_result(s, p, damage, 0.0F, 0.0F, &accepted);
	clin_update_drain();
	check(accepted, "setup: the hit was accepted by the production damage path");
}
static bool guest_toast(const char* needle) {
	return hud_msgs >= 1 && strstr(last_hud, needle) != NULL;
}
static bool record_pos(struct server_local* s, double out[3]) {
	nbt_node* pos = nbt_find_by_path(s->level.data, ".Data.CaveWiiGuest.Pos");
	if(!pos || pos->type != TAG_LIST || !pos->payload.tag_list)
		return false;
	int i = 0;
	struct list_head* cur;
	list_for_each(cur, &pos->payload.tag_list->entry) {
		struct nbt_list* e = list_entry(cur, struct nbt_list, entry);
		if(i >= 3 || !e->data || e->data->type != TAG_DOUBLE)
			return false;
		out[i++] = e->data->payload.tag_double;
	}
	return i == 3;
}
#define WAKE_X 5.5
#define WAKE_Y (4.0 + 0.1 + PLAYER_EYE_HEIGHT)
#define WAKE_Z 0.5

static void case_wake(void) {
	struct server_local* s = bedroom();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	server_local_test_set_difficulty(DIFFICULTY_PEACEFUL);

	/* --- occupied, guest -> host's bed --------------------------------- */
	struct server_rpc hclick = bed_click(2, 0);
	host_send(s, &hclick);
	check(asleep_in(h, 2, 1), "setup: host asleep in its own bed");
	sleep_ticks(s, 3);
	int32_t host_timer = h->sleep_timer;
	body_stand_at(g, 3.5, 0.5);
	counters_reset();
	struct server_rpc steal = bed_click_any(2, 0);
	check(guest_send(s, &steal), "queue: the guest's click on the host's bed is admitted");
	check(!g->sleeping && guest_toast("occupied"),
		"occupied: the guest clicking the bed the HOST sleeps in gets \"This bed is "
		"occupied\" and does not sleep (BlockBed.blockActivated:48-64)");
	check(asleep_in(h, 2, 1) && h->sleep_timer == host_timer,
		"occupied: the host sleeper is untouched (timer unchanged)");

	/* --- guest enters its own bed -------------------------------------- */
	body_stand_at(g, 6.5, 0.5);
	struct server_rpc gclick = bed_click(6, 0);
	check(guest_send(s, &gclick), "queue: the guest's own-bed click is admitted");
	check(asleep_in(g, 6, 1), "setup: guest asleep in its own bed");

	/* --- damage the HOST: only the host wakes -------------------------- */
	int32_t guest_timer = g->sleep_timer;
	hit(s, h, 8);
	check(!h->sleeping && h->sleep_timer == 0,
		"damage: a hit on the sleeping HOST wakes the host at once "
		"(wakeUpPlayer(true, true, false))");
	check(asleep_in(g, 6, 1) && g->sleep_timer == guest_timer,
		"damage: the GUEST sleeps on, timer untouched -- the host's hit never "
		"wakes the other body");

	/* --- occupied, host -> guest's bed --------------------------------- */
	body_stand_at(h, 6.5, -0.5);
	counters_reset();
	struct server_rpc hsteal = bed_click_any(6, 0);
	host_send(s, &hsteal);
	check(!h->sleeping && guest_toast("occupied"),
		"occupied: the host clicking the bed the GUEST sleeps in gets \"This bed "
		"is occupied\" and does not sleep");
	check(asleep_in(g, 6, 1) && g->sleep_timer == guest_timer,
		"occupied: the guest sleeper is untouched (timer unchanged)");

	/* --- sleeping-guest checkpoint -------------------------------------- */
	double gx = g->x, gy = g->y, gz = g->z;
	check(server_local_guest_checkpoint(s),
		"checkpoint: a SLEEPING guest checkpoints (host autosave, whose first step "
		"is this call, is no longer refused while P2 sleeps)");
	double rec[3] = {0, 0, 0};
	check(record_pos(s, rec) && fabs(rec[0] - WAKE_X) < 1e-4
		&& fabs(rec[1] - WAKE_Y) < 1e-4 && fabs(rec[2] - WAKE_Z) < 1e-4,
		"checkpoint: the record's Pos is the Beta post-load wake cell (5.5, 5.72, "
		"0.5) -- EntityPlayer.readEntityFromNBT:312-318 wakes a saved sleeper "
		"beside the bed");
	check(asleep_in(g, 6, 1) && g->x == gx && g->y == gy && g->z == gz,
		"checkpoint: the live guest is untouched -- still asleep at its in-bed pose");

	/* --- damage the GUEST: only the guest wakes, stepped out ------------ */
	body_stand_at(h, 2.5, 0.5);
	host_send(s, &hclick);
	check(asleep_in(h, 2, 1), "setup: host back asleep in its own bed");
	host_timer = h->sleep_timer;
	hit(s, g, 8);
	check(!g->sleeping && g->sleep_timer == 0 && fabs(g->x - WAKE_X) < 1e-9
		&& fabs(g->y - WAKE_Y) < 1e-9 && fabs(g->z - WAKE_Z) < 1e-9,
		"damage: a hit on the sleeping GUEST wakes the guest at once, stepped out "
		"to (5.5, 5.72, 0.5)");
	check(asleep_in(h, 2, 1) && h->sleep_timer == host_timer,
		"damage: the HOST sleeps on, timer untouched -- the guest's hit never "
		"wakes the other body");

	/* --- lethal hit on the sleeping guest -------------------------------- */
	struct server_rpc gclick2 = bed_click(6, 0);
	check(guest_send(s, &gclick2), "queue: the guest's second own-bed click is admitted");
	check(asleep_in(g, 6, 1), "setup: guest asleep again");
	hit(s, g, MAX_PLAYER_HEALTH);
	check(g->health == 0 && !g->sleeping,
		"damage: a LETHAL hit on the sleeping guest wakes it (no latched sleep "
		"into death/respawn)");
	check(asleep_in(h, 2, 1) && h->sleep_timer == host_timer,
		"damage: the host is untouched by the guest's death");

	h->sleeping = false;
	teardown(s);
}

/* ======================================================================== */
int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : NULL;
	if(!which || (strcmp(which, "base") && strcmp(which, "red")
				  && strcmp(which, "wake"))) {
		fprintf(stderr, "usage: %s base|red|wake  (one case per process)\n", argv[0]);
		return 2;
	}
	harness_init();
	if(!strcmp(which, "base"))
		case_base();
	else if(!strcmp(which, "red"))
		case_red();
	else
		case_wake();
	return harness_result(TEST_NAME, which);
}
