/* Sign edit and break publications must follow the sign's owning world,
 * including when a sign window outlives its actor's move to another world. */
#include <assert.h>
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
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "game/sign_text_store.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "network/inventory_logic.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "world.h"

static struct server_local s;
static struct client_rpc observed[2];
static unsigned observed_count;

static void observe(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type != CRPC_SIGN_TEXT && call->type != CRPC_SIGN_TEXT_REMOVED)
		return;
	assert(observed_count < 2);
	observed[observed_count++] = *call;
}

static void ignore_reply(const struct client_rpc* call, void* user) {
	(void)call;
	(void)user;
}

static struct sign_text_store* signs(unsigned slot) {
	struct clin_world_payload p;
	assert(clin_world_payload_of_slot(slot, &p));
	return p.signs;
}

static bool sign_is(unsigned slot, int x, int y, int z, char first) {
	char grid[SIGN_SIZE];
	struct sign_text_store* old = sign_text_store_select(signs(slot));
	bool found = sign_text_store_get(x, y, z, grid);
	sign_text_store_select(old);
	return first ? found && grid[0] == first : !found;
}

static void seed_client_sign(unsigned slot, uint32_t gen,
	int x, int y, int z, char first) {
	struct client_rpc call = {.type = CRPC_SIGN_TEXT};
	clin_rpc_stamp(&call, slot, gen);
	call.payload.sign_text.x = x;
	call.payload.sign_text.y = y;
	call.payload.sign_text.z = z;
	memset(call.payload.sign_text.grid, ' ', SIGN_SIZE);
	call.payload.sign_text.grid[0] = first;
	clin_process(&call);
}

static void create_world(struct server_world* w, unsigned slot,
	const char* dir, enum world_dim dimension) {
	MKDIR(dir);
	MKDIR(test_fixture_join(dir, "/region"));
	string_t name;
	string_init_set_str(name, dir);
	server_world_create(w, name, dimension, 4242, true);
	string_clear(name);
	w->session = &s;
	w->world_index = (uint8_t)slot;
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(w, 0, 0, &chunk) && chunk);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	char dir0[128], dir1[128];
	test_fixture_dir("test_sign_world_pub_ow", dir0, sizeof dir0);
	test_fixture_dir("test_sign_world_pub_ne", dir1, sizeof dir1);
	blocks_init();
	items_init();
	assert(clin_init());
	memset(&gstate, 0, sizeof gstate);
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	assert(clin_world_payloads_set_enabled(true));
	memset(&s, 0, sizeof s);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	create_world(&s.worlds[0], 0, dir0, WORLD_DIM_OVERWORLD);
	create_world(&s.worlds[1], 1, dir1, WORLD_DIM_NETHER);
	const int x = 5, y = 70, z = 5;
	for(unsigned slot = 0; slot < 2; slot++) {
		struct server_world* w = &s.worlds[slot];
		assert(server_world_set_block(w, x, y, z,
			(struct block_data){.type = BLOCK_SIGN, .metadata = 2}));
		struct te_store tiles = server_world_tile_store(w);
		int row = te_store_claim_free(&tiles, TE_KIND_SIGN, x, y, z);
		assert(row >= 0);
		memset(s.sign_texts[row], ' ', SIGN_SIZE);
		s.sign_texts[row][0] = slot ? 'N' : 'O';
	}
	struct client_ctx_registry* reg = clin_world_contexts();
	assert(client_ctx_registry_install(reg, WORLD_DIM_OVERWORLD, 0,
		s.worlds[0].generation));
	assert(client_ctx_registry_install(reg, WORLD_DIM_NETHER, 1,
		s.worlds[1].generation));
	assert(clin_world_payload_bind_view(0, 0));
	assert(clin_world_payload_bind_view(1, 1));
	seed_client_sign(0, s.worlds[0].generation, x, y, z, 'O');
	seed_client_sign(1, s.worlds[1].generation, x, y, z, 'N');
	assert(sign_is(0, x, y, z, 'O') && sign_is(1, x, y, z, 'N'));
	clin_test_set_rpc_observer(observe, NULL);

	struct server_player actor = {0};
	assert(inventory_create(&actor.inventory, &inventory_logic_player, &s,
		&actor, INVENTORY_SIZE, 0, 0, 0));
	actor.inventory.reply = ignore_reply;
	actor.active_inventory = &actor.inventory;
	actor.window_world = server_world_handle_of(&s.worlds[1]);
	struct inventory* edit = malloc(sizeof(*edit));
	assert(edit && inventory_create(edit, &inventory_logic_sign, &s,
		&actor, SIGN_SIZE, x, y, z));
	edit->reply = ignore_reply;
	actor.active_inventory = edit;
	edit->items[0].count = 'E';
	/* The actor is now in the Overworld, but the open sign window remains
	 * bound to the Nether sign at the same coordinates. */
	actor.dimension = WORLD_DIM_OVERWORLD;
	assert(inventory_logic_sign.on_close(edit, &actor));
	assert(observed_count == 1 && observed[0].type == CRPC_SIGN_TEXT
		&& observed[0].world_index == 1
		&& observed[0].world_generation == s.worlds[1].generation);
	clin_update_drain();
	assert(sign_is(0, x, y, z, 'O') && sign_is(1, x, y, z, 'E'));

	struct block_data sign_block = {.type = BLOCK_SIGN, .metadata = 2};
	struct block_info info = {.x = x, .y = y, .z = z,
		.block = &sign_block};
	struct server_world* old = block_world_enter(&s, &s.worlds[1]);
	blocks[BLOCK_SIGN]->onBreak(&s, &info);
	block_world_leave(&s, old);
	assert(observed_count == 2 && observed[1].type == CRPC_SIGN_TEXT_REMOVED
		&& observed[1].world_index == 1
		&& observed[1].world_generation == s.worlds[1].generation);
	clin_update_drain();
	assert(sign_is(0, x, y, z, 'O') && sign_is(1, x, y, z, 0));

	/* Retired generation traffic cannot erase a new sign at the same cell. */
	assert(client_ctx_registry_install(reg, WORLD_DIM_NETHER, 1,
		s.worlds[1].generation + 1));
	seed_client_sign(1, s.worlds[1].generation + 1, x, y, z, 'R');
	clin_process(&observed[1]);
	assert(sign_is(0, x, y, z, 'O') && sign_is(1, x, y, z, 'R'));
	clin_test_set_rpc_observer(NULL, NULL);
	puts("PASS sign close/break publish only to owning world and generation");
	test_fixture_dir_cleanup(dir0);
	test_fixture_dir_cleanup(dir1);
	return 0;
}
