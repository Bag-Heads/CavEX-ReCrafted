/* G7 (vehicle-smoke-1004): player-owned drops draw from the ACTOR's own
 * Random, never the world stream.
 *
 * Beta: every Entity owns its own java.util.Random rand [CFR
 * Entity.java:96]. EntityPlayer.dropPlayerItemWithRandomChoice(stack, false)
 * -- the 'Q' drop (dropCurrentItem, :286-287) and every dropPlayerItem
 * (:290-291: Container.java:95/99 cursor spill outside the window,
 * Container.java:182 cursor on close, ContainerPlayer.java:75 /
 * ContainerWorkbench.java:77 crafting-grid close) -- draws from THAT
 * player's rand [:305-315], in this order:
 *     a = rand.nextFloat() * PI * 2          (angle)
 *     m *= rand.nextFloat()                  (magnitude, 0.02 * f)
 *     motionY += (rand.nextFloat() - rand.nextFloat()) * 0.1F
 * Java evaluates the subtraction left to right. World.rand is untouched.
 *
 * This engine's stand-in for the per-player Random is
 * server_player.entity_rand (O16). Checked here on the PRODUCTION paths:
 *   (Q)  host and guest Q-drop: the actor stream advances exactly 4 draws, in
 *        Beta order (velocity reproduced from a copy of the stream); the
 *        world stream rand_src, the container stream and the OTHER actor's
 *        stream are byte-identical.
 *   (W)  SRPC_WINDOW_DROP cursor spill, host and guest: actor stream only.
 *   (C)  2x2 crafting-grid + cursor close (inventory_player inv_on_close),
 *        host and guest: actor stream only, one draw group per item.
 *   (S)  a mixed batch (CONTAINER_STORAGE + ACTIVE_CURSOR): the storage item
 *        keeps the container stream, the cursor uses the actor stream.
 *   (O)  allocation failure: the actor stream is restored to its exact
 *        pre-call state, including an actor that was never seeded (seed 0).
 * The close-path velocity FORMULA is the engine's existing 2-draw throw (not
 * Beta's 4-draw dropPlayerItem); G7 moves only the owner of the draws, and the
 * formula gap is recorded separately (G7-result.md).
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
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

#include <cglm/cglm.h>
#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/death_drop_journal.h"
#include "network/inventory_logic.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "util.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

static struct server_local s;
static struct server_player guest;
static volatile int g_drain_run = 1;
static unsigned failures;

static void check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if(!ok)
		failures++;
}

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static bool same_rng(const struct random_gen* a, const struct random_gen* b) {
	return memcmp(a, b, sizeof(*a)) == 0;
}
/* F4-RNG: the body's Entity.rand is now java.util.Random (RNG ruling 8);
 * the reference draws use the same JVM-anchored primitive. */
static bool same_jrng(const struct java_random* a, const struct java_random* b) {
	return a->seed == b->seed && a->have_next_gaussian == b->have_next_gaussian
		&& (!a->have_next_gaussian
			|| a->next_gaussian_cached == b->next_gaussian_cached);
}

static bool near3(const vec3 a, const vec3 b) {
	return fabsf(a[0] - b[0]) <= 1e-6F && fabsf(a[1] - b[1]) <= 1e-6F
		&& fabsf(a[2] - b[2]) <= 1e-6F;
}

/* Beta's dropPlayerItemWithRandomChoice(stack, false) [:305-315], in the
 * engine's float arithmetic (server_local_drop_hotbar_item_for). */
static void q_drop_expected(struct java_random* r, const struct server_player* p,
							vec3 v) {
	float yaw = p->rx * (float)M_PI / 180.0f;
	float pitch = p->ry * (float)M_PI / 180.0f;
	float f = 0.3f;
	v[0] = -sinf(yaw) * cosf(pitch) * f;
	v[2] = cosf(yaw) * cosf(pitch) * f;
	v[1] = -sinf(pitch) * f + 0.1f;
	float a = jrand_next_float(r);
	float b = jrand_next_float(r);
	float c = jrand_next_float(r);
	float d = jrand_next_float(r);
	float f1 = a * (float)M_PI * 2.0f;
	float f2 = 0.02f * b;
	v[0] += cosf(f1) * f2;
	v[1] += (c - d) * 0.1f;
	v[2] += sinf(f1) * f2;
}

/* The engine's close/cursor throw (server_local_item_velocity, throw=true):
 * two draws, rx jitter then ry jitter. */
static void throw_expected(struct random_gen* r, const struct server_player* p,
						   vec3 v) {
	float rx = glm_rad(-p->rx + (rand_gen_flt(r) - 0.5F) * 22.5F);
	float ry = glm_rad(p->ry + 90.0F + (rand_gen_flt(r) - 0.5F) * 22.5F);
	v[0] = sinf(rx) * sinf(ry) * 0.25F;
	v[1] = cosf(ry) * 0.25F;
	v[2] = cosf(rx) * sinf(ry) * 0.25F;
}

struct streams {
	struct random_gen world_rng, container;
	struct java_random host, guest;
};
static struct streams snap(void) {
	return (struct streams) {s.rand_src, s.container_drop_rng,
							 s.player.entity_rand.r, guest.entity_rand.r};
}

/* Every other stream byte-identical; `actor` advanced to exactly `want`. */
static void check_streams(const char* label, const struct streams* before,
						  const struct server_player* actor,
						  const struct java_random* want) {
	char what[160];
	snprintf(what, sizeof(what), "%s: world stream rand_src untouched", label);
	check(same_rng(&s.rand_src, &before->world_rng), what);
	snprintf(what, sizeof(what), "%s: container stream untouched", label);
	check(same_rng(&s.container_drop_rng, &before->container), what);
	const struct java_random* other_now
		= actor == &s.player ? &guest.entity_rand.r : &s.player.entity_rand.r;
	const struct java_random* other_before
		= actor == &s.player ? &before->guest : &before->host;
	snprintf(what, sizeof(what), "%s: the other actor's stream untouched",
			 label);
	check(same_jrng(other_now, other_before), what);
	snprintf(what, sizeof(what),
			 "%s: actor stream advanced by exactly the Beta draw count", label);
	check(same_jrng(&actor->entity_rand.r, want), what);
}

static void setup_player(struct server_player* p, float rx, float ry,
						 uint32_t seed) {
	assert(inventory_create(&p->inventory, &inventory_logic_player, &s, p,
							INVENTORY_SIZE, 0, 0, 0));
	p->active_inventory = &p->inventory;
	p->x = 8.5;
	p->y = 110.0;
	p->z = 8.5;
	p->rx = rx;
	p->ry = ry;
	p->has_pos = true;
	p->finished_loading = true;
	if(seed) {
		jrand_set_seed(&p->entity_rand.r, (int64_t)seed);
		p->entity_rand.seeded = true;
	} else {
		p->entity_rand = (struct server_body_rand) {0};
	}
}

static void q_case(struct server_player* p, const char* label) {
	inventory_set_slot(&p->inventory, INVENTORY_SLOT_HOTBAR,
					   (struct item_data) {.id = ITEM_APPLE, .count = 5});
	inventory_set_hotbar(&p->inventory, 0);
	struct streams before = snap();
	struct java_random want = p->entity_rand.r;
	vec3 v;
	q_drop_expected(&want, p, v);
	struct entity* e = server_local_test_drop_hotbar_item_for(&s, p, false);
	check(e != NULL, label);
	if(!e)
		return;
	char what[160];
	snprintf(what, sizeof(what),
			 "%s: velocity = Beta angle, magnitude, (a - b) from the ACTOR "
			 "stream, in order", label);
	check(near3(e->vel, v), what);
	check_streams(label, &before, p, &want);
}

static void window_case(struct server_player* p, const char* label) {
	struct item_data dropped = {.id = BLOCK_COBBLESTONE, .count = 3};
	struct streams before = snap();
	struct java_random want = p->entity_rand.r;
	vec3 v;
	q_drop_expected(&want, p, v); /* O21: Beta dropPlayerItem(stack,false) */
	struct entity* e = server_local_test_window_drop_spawn(&s, p, &dropped);
	check(e != NULL, label);
	if(!e)
		return;
	char what[160];
	snprintf(what, sizeof(what),
			 "%s: Beta dropPlayerItem velocity (4 draws) from the ACTOR stream",
			 label);
	check(near3(e->vel, v), what);
	check_streams(label, &before, p, &want);
}

/* 2x2 grid (slots 1..4) holding two stacks + the cursor: three player-owned
 * items, two draws each, in inv_on_close order (grid ascending, cursor). */
static void close_case(struct server_player* p, const char* label) {
	inventory_set_slot(&p->inventory, INVENTORY_SLOT_CRAFTING + 0,
					   (struct item_data) {.id = BLOCK_PLANKS, .count = 2});
	inventory_set_slot(&p->inventory, INVENTORY_SLOT_CRAFTING + 3,
					   (struct item_data) {.id = BLOCK_DIRT, .count = 7});
	inventory_set_picked_item(&p->inventory,
							  (struct item_data) {.id = ITEM_APPLE, .count = 1});
	struct streams before = snap();
	struct java_random want = p->entity_rand.r;
	vec3 v[3];
	for(int i = 0; i < 3; i++)
		q_drop_expected(&want, p, v[i]); /* O21: 4 draws per item */
	bool ok = p->inventory.logic->on_close(&p->inventory, p);
	check(ok, label);
	check_streams(label, &before, p, &want);
	struct item_data left;
	char what[160];
	snprintf(what, sizeof(what), "%s: grid and cursor emptied", label);
	check(!inventory_get_slot(&p->inventory, INVENTORY_SLOT_CRAFTING + 0, &left)
			  && !inventory_get_slot(&p->inventory, INVENTORY_SLOT_CRAFTING + 3,
									 &left)
			  && !inventory_get_picked_item(&p->inventory, &left),
		  what);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_player_drop_rng_golden_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	memset(&guest, 0, sizeof(guest));
	dict_entity_init(s.entities);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	struct server_chunk* sc = NULL;
	for(int cx = -1; cx <= 1; cx++)
		for(int cz = -1; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
	s.rand_src.seed = 0x5EED1234u;
	s.container_drop_rng.seed = 0x12345678u;
	setup_player(&s.player, 30.0F, -20.0F, 0xA11CE5EDu);
	setup_player(&guest, -75.0F, 10.0F, 0x6E57BEEFu);

	q_case(&s.player, "(Q host)");
	q_case(&guest, "(Q guest)");
	window_case(&s.player, "(W host)");
	window_case(&guest, "(W guest)");
	close_case(&s.player, "(C host)");
	close_case(&guest, "(C guest)");

	/* (S) mixed batch through the public sourced entry point. */
	{
		struct item_data items[2] = {{.id = BLOCK_COBBLESTONE, .count = 9},
									 {.id = ITEM_APPLE, .count = 2}};
		struct death_drop_provenance prov[2] = {
			{.kind = DEATH_DROP_OWNER_CONTAINER_STORAGE},
			{.kind = DEATH_DROP_OWNER_ACTIVE_CURSOR}};
		struct streams before = snap();
		struct random_gen want_container = s.container_drop_rng;
		struct java_random want_actor = guest.entity_rand.r;
		vec3 vs, vc;
		throw_expected(&want_container, &guest, vs);
		q_drop_expected(&want_actor, &guest, vc); /* O21 */
		bool ok = server_local_spawn_sourced_item_batch_for(
			(vec3) {8.5F, 110.0F, 8.5F}, items, prov, 2, true, true, &s,
			&guest);
		check(ok, "(S) mixed storage + cursor batch spawned");
		check(same_rng(&s.container_drop_rng, &want_container),
			  "(S) storage item: container stream, 2 draws");
		check(same_jrng(&guest.entity_rand.r, &want_actor),
			  "(S) cursor item: guest actor stream, 4 draws (dropPlayerItem)");
		check(same_rng(&s.rand_src, &before.world_rng)
				  && same_jrng(&s.player.entity_rand.r, &before.host),
			  "(S) world and host streams untouched");
	}

	/* (O) allocation failure: exact pre-call restore, even unseeded. */
	{
		struct server_player fresh;
		memset(&fresh, 0, sizeof(fresh));
		setup_player(&fresh, 0.0F, 0.0F, 0u);
		inventory_set_slot(&fresh.inventory, INVENTORY_SLOT_HOTBAR,
						   (struct item_data) {.id = ITEM_APPLE, .count = 1});
		struct streams before = snap();
		struct item_data one = {.id = BLOCK_DIRT, .count = 1};
		server_local_test_fail_next_entity_alloc();
		struct entity* e = server_local_test_window_drop_spawn(&s, &fresh, &one);
		check(e == NULL && !fresh.entity_rand.seeded
				  && same_rng(&s.rand_src, &before.world_rng),
			  "(O) failed cursor spill: unseeded actor stays unseeded, world "
			  "stream untouched");
		inventory_set_picked_item(&fresh.inventory, one);
		server_local_test_fail_next_entity_alloc();
		bool closed = fresh.inventory.logic->on_close(&fresh.inventory, &fresh);
		check(!closed && !fresh.entity_rand.seeded
				  && same_rng(&s.rand_src, &before.world_rng),
			  "(O) failed close: unseeded actor stays unseeded, world stream "
			  "untouched");
		inventory_destroy(&fresh.inventory, &fresh);
	}

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	printf("%s test_player_drop_rng_golden (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
