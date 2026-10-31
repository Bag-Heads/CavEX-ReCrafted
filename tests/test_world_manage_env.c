/*
	Copyright (c) 2023 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

/* World management (menu queue item 4): the recursive delete
 * [740c583 ISaveFormat.deleteWorldDirectory via GuiSelectWorld.deleteWorld]
 * and the rename apply op [740c583 ISaveFormat.renameWorld via
 * GuiRenameWorld.actionPerformed] -- LevelName rewritten IN PLACE,
 * TRIMMED, folder untouched. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#define GETPID() _getpid()
#define GETCWD(p, n) _getcwd(p, n)
#define RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define GETPID() getpid()
#define GETCWD(p, n) getcwd(p, n)
#define RMDIR(p) rmdir(p)
#endif

#include "../source/game/gui/screen.h"
#include "../source/network/level_archive.h"

static void write_file(const char* path, const char* content) {
	FILE* f = fopen(path, "wb");
	assert(f);
	fputs(content, f);
	fclose(f);
}

static bool path_exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static bool link_exists(const char* path) {
#ifdef _WIN32
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
	struct stat st;
	return lstat(path, &st) == 0;
#endif
}

static bool make_directory_link(const char* link, const char* target) {
#ifdef _WIN32
	char command[512];
	snprintf(command, sizeof(command), "cmd /c mklink /J \"%s\" \"%s\" >NUL", link, target);
	return system(command) == 0;
#else
	return symlink(target, link) == 0;
#endif
}

static void test_delete_links(void) {
	char outside[96], root[96], root_link[96], dangling[96], target[192], link[192];
	char cwd[256], outside_absolute[384], dangling_absolute[384];
	assert(GETCWD(cwd, sizeof(cwd)));
	snprintf(outside, sizeof(outside), "tmp_manage_outside_%ld", (long)GETPID());
	snprintf(root, sizeof(root), "tmp_manage_links_%ld", (long)GETPID());
	snprintf(root_link, sizeof(root_link), "tmp_manage_root_link_%ld", (long)GETPID());
	snprintf(dangling, sizeof(dangling), "tmp_manage_missing_%ld", (long)GETPID());
#ifdef _WIN32
	snprintf(outside_absolute, sizeof(outside_absolute), "%s\\%s", cwd, outside);
	snprintf(dangling_absolute, sizeof(dangling_absolute), "%s\\%s", cwd, dangling);
#else
	snprintf(outside_absolute, sizeof(outside_absolute), "%s/%s", cwd, outside);
	snprintf(dangling_absolute, sizeof(dangling_absolute), "%s/%s", cwd, dangling);
#endif
	assert(MKDIR(outside) == 0);
	snprintf(target, sizeof(target), "%s/sentinel.txt", outside);
	write_file(target, "must survive");
	assert(MKDIR(root) == 0);
	snprintf(link, sizeof(link), "%s/linked", root);
	if(make_directory_link(link, outside_absolute)) {
		screen_sworld_test_delete_tree(root);
		assert(path_exists(target));
		assert(!link_exists(link));
		assert(!path_exists(root));
		fprintf(stderr, "nested directory link OK\n");
	} else {
		fprintf(stderr, "SKIP nested directory link: link creation failed\n");
		screen_sworld_test_delete_tree(root);
	}
	if(make_directory_link(root_link, outside_absolute)) {
		screen_sworld_test_delete_tree(root_link);
		assert(path_exists(target));
		assert(!link_exists(root_link));
		fprintf(stderr, "root directory link OK\n");
	} else {
		fprintf(stderr, "SKIP root directory link: link creation failed\n");
	}
	snprintf(link, sizeof(link), "%s/dangling", root);
	assert(MKDIR(root) == 0);
	if(make_directory_link(link, dangling_absolute)) {
		assert(link_exists(link));
		screen_sworld_test_delete_tree(root);
		assert(!link_exists(link));
		assert(!path_exists(root));
		fprintf(stderr, "dangling directory link OK\n");
	} else {
		fprintf(stderr, "SKIP dangling directory link: link creation failed\n");
		screen_sworld_test_delete_tree(root);
	}
	assert(path_exists(target));
	assert(remove(target) == 0);
	assert(RMDIR(outside) == 0);
}

/* ---- S1: recursive delete ---------------------------------------------- */

static void test_delete_tree(void) {
	char root[96];
	snprintf(root, sizeof(root), "tmp_manage_del_%ld", (long)GETPID());
	char p[192];

	assert(MKDIR(root) == 0);
	snprintf(p, sizeof(p), "%s/level.dat", root);
	write_file(p, "x");
	snprintf(p, sizeof(p), "%s/session.lock", root);
	write_file(p, "x");
	snprintf(p, sizeof(p), "%s/region", root);
	assert(MKDIR(p) == 0);
	snprintf(p, sizeof(p), "%s/region/r.0.0.mcr", root);
	write_file(p, "region bytes");
	snprintf(p, sizeof(p), "%s/region/r.-1.0.mcr", root);
	write_file(p, "region bytes");
	/* deeper nesting than any Beta save has -- the remover must not care */
	snprintf(p, sizeof(p), "%s/extra", root);
	assert(MKDIR(p) == 0);
	snprintf(p, sizeof(p), "%s/extra/nested", root);
	assert(MKDIR(p) == 0);
	snprintf(p, sizeof(p), "%s/extra/nested/file.txt", root);
	write_file(p, "x");

	screen_sworld_test_delete_tree(root);

	assert(!path_exists(root));
	fprintf(stderr, "S1 delete tree OK\n");
}

/* ---- S2: rename apply --------------------------------------------------- */

static void test_rename_apply(void) {
	char root[96];
	snprintf(root, sizeof(root), "tmp_manage_ren_%ld", (long)GETPID());
	assert(MKDIR(root) == 0);

	string_t dir;
	string_init_set_str(dir, root);
	struct level_archive la;
	vec3 spawn = {8.0F, 70.0F, 8.0F};
	assert(level_archive_create_new(&la, dir, 777, spawn));
	assert(level_archive_write(&la, LEVEL_NAME, (void*)"Old Name"));
	assert(level_archive_save(&la));
	level_archive_discard(&la);

	/* the SAVED name is TRIMMED [740c583 GuiRenameWorld.actionPerformed:
	 * renameWorld(folder, text.trim())] */
	assert(screen_rworld_apply(root, "  Fancy Name  "));

	struct level_archive re;
	assert(level_archive_create(&re, dir));
	char name[64] = {0};
	assert(level_archive_read(&re, LEVEL_NAME, name, sizeof(name)));
	assert(strcmp(name, "Fancy Name") == 0);
	level_archive_discard(&re);
	string_clear(dir);

	/* whitespace-only rejects (the enable rule's backstop) */
	assert(!screen_rworld_apply(root, "   "));
	/* missing world rejects cleanly */
	assert(!screen_rworld_apply("tmp_manage_ren_missing_xyz", "New"));

	screen_sworld_test_delete_tree(root);
	assert(!path_exists(root));
	fprintf(stderr, "S2 rename apply OK\n");
}

/* ---- S3: the exported Java trim ----------------------------------------- */

static void test_trim_export(void) {
	char out[32];
	screen_cworld_java_trim("  a b  ", out, sizeof(out));
	assert(strcmp(out, "a b") == 0);
	screen_cworld_java_trim("\t x \r\n", out, sizeof(out));
	assert(strcmp(out, "x") == 0);
	screen_cworld_java_trim("   ", out, sizeof(out));
	assert(out[0] == 0);
	fprintf(stderr, "S3 trim export OK\n");
}

int main(void) {
	test_delete_tree();
	test_delete_links();
	test_rename_apply();
	test_trim_export();
	fprintf(stderr, "test_world_manage_env: all OK\n");
	return 0;
}
