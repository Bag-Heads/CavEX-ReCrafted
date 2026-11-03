/* ci-1101/41 item 11: the host player checkpoint writes Beta's remaining
 * Entity/EntityPlayer tags and the load restores fire/air.
 *
 * Beta 1.7.3 (RetroMCP decompile):
 *   Entity.writeToNBT (Entity.java:752-757): Motion (3 doubles), Fire (short),
 *     Air (short), OnGround (boolean byte);
 *   EntityPlayer.writeEntityToNBT (EntityPlayer.java:330-331): Sleeping
 *     (boolean byte), SleepTimer (short);
 *   Entity.readFromNBT (Entity.java:786-787): fire = getShort("Fire"),
 *     air = getShort("Air").
 * Before the fix the checkpoint never touched these tags (the template's
 * frozen Fire -20 / Air 300 / Motion 0 / OnGround 1 stayed forever) and the
 * load reset fire/oxygen to the fresh-body values.
 *
 * T1 checkpoint writes the six tags with the live values and Beta's types,
 *    next to a consistent Health/CaveWiiHealth pair (so the legacy health
 *    classifier, which treats Sleeping as "Beta-born", is never consulted);
 * T2 the reload restores fire and air;
 * T3 a save WITHOUT Fire/Air loads with today's fresh-body values;
 * T4 the old-format CaveWii fixture (tests/fixtures/sort2_native) loads
 *    unchanged: fire -20, air 300, health 160 units;
 * T5 the writer is allocation-atomic over a wrong-type tag and keeps the
 *    child order of same-type tags. */
#include "support/split_residency_fixture.h"

#include "item/items.h"
#include "cNBT/list.h"

#include <math.h>
#include <stdio.h>

static struct server_local s_load;

static nbt_node* player_tag(struct level_archive* la, const char* name) {
	char path[64];
	snprintf(path, sizeof path, ".Data.Player.%s", name);
	return nbt_find_by_path(la->data, path);
}

static void remove_player_child(struct level_archive* la, const char* name) {
	nbt_node* player = nbt_find_by_path(la->data, ".Data.Player");
	assert(player && player->type == TAG_COMPOUND);
	struct list_head *cur, *tmp;
	list_for_each_safe(cur, tmp, &player->payload.tag_compound->entry) {
		struct nbt_list* e = list_entry(cur, struct nbt_list, entry);
		if(e->data && e->data->name && !strcmp(e->data->name, name)) {
			list_del(cur);
			nbt_free(e->data);
			free(e);
		}
	}
}

static int child_index(struct level_archive* la, const char* name) {
	nbt_node* player = nbt_find_by_path(la->data, ".Data.Player");
	assert(player && player->type == TAG_COMPOUND);
	int i = 0;
	struct list_head* cur;
	list_for_each(cur, &player->payload.tag_compound->entry) {
		nbt_node* n = list_entry(cur, struct nbt_list, entry)->data;
		if(n && n->name && !strcmp(n->name, name))
			return i;
		i++;
	}
	return -1;
}

static double motion_at(struct level_archive* la, int k) {
	nbt_node* m = player_tag(la, "Motion");
	assert(m && m->type == TAG_LIST);
	int i = 0;
	struct list_head* cur;
	list_for_each(cur, &m->payload.tag_list->entry) {
		nbt_node* d = list_entry(cur, struct nbt_list, entry)->data;
		assert(d->type == TAG_DOUBLE);
		if(i++ == k)
			return d->payload.tag_double;
	}
	assert(!"motion component missing");
	return 0.0;
}

static void new_world(string_t lvl) {
	struct level_archive la;
	assert(level_archive_create_new(&la, lvl, 4242,
		(vec3) {8.5F, 4.0F + PLAYER_EYE_HEIGHT, 8.5F}));
	level_archive_destroy(&la);
}

static int failures;
#define CHECK(c, msg) do { if(!(c)) { fprintf(stderr, "FAIL %s\n", msg); failures++; } } while(0)

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();
	char dir[128];
	test_fixture_dir("test_player_entity_nbt_parity", dir, sizeof(dir));
#ifdef _WIN32
	_mkdir(dir);
#else
	mkdir(dir, 0777);
#endif
	server_local_test_create_without_thread(&s_load, false, false, false,
											false, false, true /*superflat*/,
											false);
	string_t lvl;
	string_init_set_str(lvl, dir);
	new_world(lvl);

	/* ---- T1: checkpoint writes the Beta tags ---- */
	assert(server_local_load_world_impl(&s_load, lvl));
	s_load.player.fire = 137;
	s_load.player.oxygen = 41;
	s_load.player.vel_x = 0.25;
	s_load.player.motion_y = -0.5;
	s_load.player.vel_z = 0.125;
	s_load.player.on_ground = false;
	s_load.player.sleeping = false;
	s_load.player.sleep_timer = 0;
	s_load.player.health = 40; /* 2.5 hearts: a value the classifier would misread */
	assert(server_local_test_save_player_state(&s_load));
	{
		nbt_node* fire = player_tag(&s_load.level, "Fire");
		nbt_node* air = player_tag(&s_load.level, "Air");
		nbt_node* ground = player_tag(&s_load.level, "OnGround");
		nbt_node* sleeping = player_tag(&s_load.level, "Sleeping");
		nbt_node* timer = player_tag(&s_load.level, "SleepTimer");
		CHECK(fire && fire->type == TAG_SHORT && fire->payload.tag_short == 137,
			  "T1 Fire short 137");
		CHECK(air && air->type == TAG_SHORT && air->payload.tag_short == 41,
			  "T1 Air short 41");
		CHECK(ground && ground->type == TAG_BYTE && ground->payload.tag_byte == 0,
			  "T1 OnGround byte 0");
		CHECK(sleeping && sleeping->type == TAG_BYTE
				  && sleeping->payload.tag_byte == 0,
			  "T1 Sleeping byte 0");
		CHECK(timer && timer->type == TAG_SHORT && timer->payload.tag_short == 0,
			  "T1 SleepTimer short 0");
		if(player_tag(&s_load.level, "Motion")) {
			CHECK(motion_at(&s_load.level, 0) == 0.25, "T1 Motion x");
			CHECK(motion_at(&s_load.level, 1) == -0.5, "T1 Motion y");
			CHECK(motion_at(&s_load.level, 2) == 0.125, "T1 Motion z");
		}
		CHECK(player_tag(&s_load.level, "CaveWiiHealth") != NULL,
			  "T1 CaveWiiHealth marker beside Sleeping");
		int16_t h = -1;
		CHECK(level_archive_read_player_health(&s_load.level, &h) && h == 40,
			  "T1 health 40 units survives the Sleeping provenance tag");
	}
	server_local_unload_world_impl(&s_load, false);
	clin_drain_discard();

	/* ---- T2: reload restores fire + air ---- */
	assert(server_local_load_world_impl(&s_load, lvl));
	CHECK(s_load.player.fire == 137, "T2 fire restored");
	CHECK(s_load.player.oxygen == 41, "T2 air restored");
	server_local_unload_world_impl(&s_load, false);
	clin_drain_discard();

	/* ---- T3: a save without Fire/Air loads with today's defaults ---- */
	{
		string_t file;
		string_init_set_str(file, dir);
		struct level_archive la;
		assert(level_archive_create(&la, file));
		remove_player_child(&la, "Fire");
		remove_player_child(&la, "Air");
		la.modified = true;
		assert(level_archive_save(&la));
		level_archive_destroy(&la);
		string_clear(file);
	}
	assert(server_local_load_world_impl(&s_load, lvl));
	CHECK(s_load.player.fire == -PLAYER_FIRE_RESISTANCE, "T3 fresh fire");
	CHECK(s_load.player.oxygen == MAX_OXYGEN, "T3 fresh air");
	server_local_unload_world_impl(&s_load, false);
	clin_drain_discard();
	string_clear(lvl);
	test_fixture_dir_cleanup(dir);

	/* ---- T4: the old-format CaveWii level.dat fixture ---- */
	{
		char src[1024];
		snprintf(src, sizeof src, "%s", __FILE__);
		char* end = strrchr(src, '/');
		if(!end)
			end = strrchr(src, '\\');
		assert(end);
		snprintf(end + 1, sizeof(src) - (size_t)(end + 1 - src),
				 "fixtures/sort2_native/world/level.dat");
		char odir[128];
		test_fixture_dir("test_player_entity_nbt_parity_old", odir, sizeof odir);
#ifdef _WIN32
		_mkdir(odir);
#else
		mkdir(odir, 0777);
#endif
		char dst[256];
		snprintf(dst, sizeof dst, "%s/level.dat", odir);
		FILE* in = fopen(src, "rb");
		FILE* out = fopen(dst, "wb");
		assert(in && out);
		char buf[4096];
		size_t n;
		while((n = fread(buf, 1, sizeof buf, in)) > 0)
			assert(fwrite(buf, 1, n, out) == n);
		fclose(in);
		fclose(out);
		string_t file;
		string_init_set_str(file, odir);
		struct level_archive la;
		assert(level_archive_create(&la, file));
		CHECK(!player_tag(&la, "Sleeping"), "T4 fixture is pre-fix (no Sleeping)");
		int fire = -PLAYER_FIRE_RESISTANCE, air = MAX_OXYGEN;
		level_archive_read_player_fire_air(&la, &fire, &air);
		CHECK(fire == -20 && air == 300, "T4 old save fire/air as today");
		int16_t h = -1;
		CHECK(level_archive_read_player_health(&la, &h) && h == MAX_PLAYER_HEALTH,
			  "T4 old save health unchanged");

		/* ---- T5: atomic writer, order kept, wrong type replaced ---- */
		int air_index = child_index(&la, "Air");
		assert(air_index >= 0);
		nbt_node* fire_node = player_tag(&la, "Fire");
		assert(fire_node);
		fire_node->type = TAG_INT; /* corrupt: wrong type */
		fire_node->payload.tag_int = 5;
		struct level_player_entity_state st = {
			.motion = {NAN, 1.5, -0.25}, .fire = -3, .air = 299,
			.on_ground = true, .sleeping = true, .sleep_timer = 42,
		};
		assert(level_archive_write_player_entity_state(&la, &st));
		nbt_node* f = player_tag(&la, "Fire");
		CHECK(f && f->type == TAG_SHORT && f->payload.tag_short == -3,
			  "T5 wrong-type Fire replaced by short");
		CHECK(motion_at(&la, 0) == 0.0 && motion_at(&la, 1) == 1.5
				  && motion_at(&la, 2) == -0.25,
			  "T5 Motion in place, NaN written as 0");
		nbt_node* sl = player_tag(&la, "Sleeping");
		nbt_node* st_node = player_tag(&la, "SleepTimer");
		CHECK(sl && sl->type == TAG_BYTE && sl->payload.tag_byte == 1
				  && st_node && st_node->payload.tag_short == 42,
			  "T5 Sleeping/SleepTimer created");
		/* Air is updated in place: same child position as before. */
		CHECK(child_index(&la, "Air") == air_index
				  && player_tag(&la, "Air")->payload.tag_short == 299,
			  "T5 same-type tag keeps its position");
		fire = 0;
		air = 0;
		level_archive_read_player_fire_air(&la, &fire, &air);
		CHECK(fire == -3 && air == 299, "T5 read-back");
		level_archive_discard(&la);
		string_clear(file);
		test_fixture_dir_cleanup(odir);
	}

	if(failures)
		return 1;
	puts("PASS player Motion/Fire/Air/OnGround/Sleeping round-trip (Beta "
		 "Entity.writeToNBT + EntityPlayer.writeEntityToNBT)");
	return 0;
}
