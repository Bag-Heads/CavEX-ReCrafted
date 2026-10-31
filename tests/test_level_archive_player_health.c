/*
	IMP-PLAYER-DEFAULT. An imported Beta level.dat (adventure maps) may lack
	Data.Player entirely. Beta 1.7.3 then never calls readFromNBT
	(World.spawnPlayerWithLoadedChunks: only when getPlayerNBTTagCompound() !=
	null) so the fresh EntityPlayer keeps its constructor health, 20 HP.
	If Player EXISTS but has no Health key, EntityLiving.readEntityFromNBT does
	health = getShort("Health") (0) then `if(!hasKey("Health")) health = 10`,
	i.e. 10 HP, NOT 0. A present Health (even 0, a quit-while-dead save) is
	kept (on disk as Beta half-hearts since PLAYER-HEALTH-NBT; see
	test_player_nbt_beta_types). CaveWii health is 8x Beta (MAX_PLAYER_HEALTH = 160).
*/
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif
#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "cNBT/list.h"

static char s_dir[128];

void inventory_clear(struct inventory* inv) {
	assert(inv);
	for(size_t k = 0; k < inv->capacity; k++)
		inv->items[k] = (struct item_data) {0};
	inv->picked_item = (struct item_data) {0};
	inv->hotbar_slot = 0;
}

static void remove_child(nbt_node* parent, const char* name) {
	assert(parent && parent->type == TAG_COMPOUND);
	struct list_head* cur;
	struct list_head* tmp;
	list_for_each_safe(cur, tmp, &parent->payload.tag_compound->entry) {
		struct nbt_list* e = list_entry(cur, struct nbt_list, entry);
		if(e->data && e->data->name && !strcmp(e->data->name, name)) {
			list_del(cur);
			nbt_free(e->data);
			free(e);
			return;
		}
	}
	assert(!"child not found");
}

static void fresh(struct level_archive* la, string_t dir) {
	assert(level_archive_create_new(la, dir, 7, (vec3) {10, 70, 20}));
}

int main(void) {
	test_fixture_dir("test_level_player_health_tmp", s_dir, sizeof s_dir);
	MKDIR(s_dir);
	string_t dir;
	string_init_set_str(dir, s_dir);
	struct level_archive la;
	int16_t h;

	/* normal save: stored value round-trips */
	fresh(&la, dir);
	h = -1;
	assert(level_archive_read_player_health(&la, &h));
	assert(h == MAX_PLAYER_HEALTH);
	int16_t stored = 40;
	assert(level_archive_write_player_health(&la, stored));
	assert(level_archive_read_player_health(&la, &h) && h == 40);
	/* present Health 0 stays 0 (quit-while-dead save) */
	stored = 0;
	assert(level_archive_write_player_health(&la, stored));
	assert(level_archive_read_player_health(&la, &h) && h == 0);

	/* Player tag present, Health key absent -> Beta 10 HP (= 80 here) */
	remove_child(nbt_find_by_path(la.data, ".Data.Player"), "Health");
	h = -1;
	assert(level_archive_read_player_health(&la, &h));
	assert(h == MAX_PLAYER_HEALTH / 2);
	level_archive_destroy(&la);

	/* no Player tag at all -> fresh player, 20 HP (= 160), alive */
	fresh(&la, dir);
	remove_child(nbt_find_by_path(la.data, ".Data"), "Player");
	h = -1;
	assert(level_archive_read_player_health(&la, &h));
	assert(h == MAX_PLAYER_HEALTH && h > 0);
	/* and the Player-position read fails, so the load takes the spawn path */
	vec3 pos; vec2 rot; enum world_dim dim;
	assert(!level_archive_read_player(&la, pos, rot, NULL, &dim));
	level_archive_destroy(&la);

	/* IMP-SAVE-NO-PLAYER: saving a Player-less import writes a full Beta
	   Player (WorldInfo.updateTagCompound always sets "Player") instead of
	   failing, with the live values, and leaves every other Data field alone. */
	fresh(&la, dir);
	remove_child(nbt_find_by_path(la.data, ".Data"), "Player");
	nbt_node* before = nbt_clone(la.data);
	assert(before);
	vec3 wpos = {178.5F, 119.62F, -28.5F};
	vec2 wrot = {90.0F, 5.0F};
	vec3 wvel = {0.0F, 0.0F, 0.0F};
	assert(level_archive_write_player(&la, wpos, wrot, wvel, WORLD_DIM_OVERWORLD));
	int16_t live_health = 120;
	assert(level_archive_write_player_health(&la, live_health));
	nbt_node* bdata = nbt_find_by_path(before, ".Data");
	struct list_head* cur;
	list_for_each(cur, &bdata->payload.tag_compound->entry) {
		struct nbt_list* e = list_entry(cur, struct nbt_list, entry);
		if(!e->data || !e->data->name || !strcmp(e->data->name, "Player"))
			continue;
		char path[160];
		snprintf(path, sizeof path, ".Data.%s", e->data->name);
		nbt_node* now = nbt_find_by_path(la.data, path);
		assert(now && nbt_eq(now, e->data));
	}
	nbt_free(before);
	assert(level_archive_save(&la));
	level_archive_destroy(&la);
	string_t file;
	string_init_set_str(file, s_dir);
	assert(level_archive_create(&la, file));
	vec3 rpos; vec2 rrot; vec3 rvel; enum world_dim rdim;
	assert(level_archive_read_player(&la, rpos, rrot, rvel, &rdim));
	assert(fabsf(rpos[0] - 178.5F) < 1e-3F && fabsf(rpos[1] - 119.62F) < 1e-3F
		   && fabsf(rpos[2] + 28.5F) < 1e-3F);
	assert(rdim == WORLD_DIM_OVERWORLD);
	h = -1;
	assert(level_archive_read_player_health(&la, &h) && h == 120);
	assert(nbt_find_by_path(la.data, ".Data.Player.Inventory"));
	level_archive_destroy(&la);
	string_clear(file);

	string_clear(dir);
	puts("test_level_archive_player_health PASS");
	return 0;
}
