/* O19 (vehicle-smoke-1004): the vehicle smoke's disposable rig world slot.
 *
 * Real-Wii evidence (HW1-diag/diag_boat2): the boat smoke failed at p0 with
 * vehicle_smoke.fail code=rig_world_create_refused because a consumed
 * cw_vehicle_rig from the previous run was still on the card, and nothing in
 * the Wii run path ever archives it (a PC run gets a fresh saves/ dir per
 * run, which is why PC never saw it; reproduced on PC by re-running against
 * a used run directory). screen_sworld_create_vehicle_rig now asks
 * vehicle_smoke_rig_prepare_slot first:
 *   - nothing there                -> FREE, nothing touched;
 *   - an earlier rig DIRECTORY     -> moved (never deleted) to
 *                                     <rig>_consumed_NNN, contents intact;
 *   - a non-directory at the name  -> REFUSED, untouched;
 *   - every archive slot occupied  -> REFUSED, untouched.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif

#include "support/test_fixture_dir.h"
#include "game/vehicle_smoke_script.h"

static unsigned failures;
static void check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if(!ok)
		failures++;
}

static bool is_dir(const char* p) {
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool exists(const char* p) {
	struct stat st;
	return stat(p, &st) == 0;
}
/* Soft: under the pre-O19 code an earlier case leaves the rig directory in
 * place, and a later setup write onto that name must report, not abort. */
static void write_file(const char* p, const char* text) {
	FILE* f = fopen(p, "wb");
	if(!f) {
		printf("note setup write failed: %s\n", p);
		return;
	}
	fputs(text, f);
	fclose(f);
}
static bool file_has(const char* p, const char* text) {
	FILE* f = fopen(p, "rb");
	if(!f)
		return false;
	char buf[64] = {0};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return strcmp(buf, text) == 0;
}

int main(void) {
	char root[256];
	test_fixture_dir("test_vehicle_rig_slot_tmp", root, sizeof(root));
	MKDIR(root);
	char rig[300], path[400], archived[512];
	snprintf(rig, sizeof(rig), "%s/%s", root, VEHICLE_SMOKE_RIG_WORLD);

	/* 1. Nothing there: FREE, nothing created. */
	check(vehicle_smoke_rig_prepare_slot(rig, archived, sizeof(archived))
			  == VEHICLE_RIG_SLOT_FREE
			  && archived[0] == '\0' && !exists(rig),
		  "empty slot -> FREE, nothing touched");

	/* 2. A consumed rig directory (with a world file inside): moved aside to
	 * _consumed_000 with its contents, the name is free again. */
	MKDIR(rig);
	snprintf(path, sizeof(path), "%s/level.dat", rig);
	write_file(path, "first-run");
	enum vehicle_rig_slot slot
		= vehicle_smoke_rig_prepare_slot(rig, archived, sizeof(archived));
	char want[400];
	snprintf(want, sizeof(want), "%s_consumed_000", rig);
	snprintf(path, sizeof(path), "%s/level.dat", want);
	check(slot == VEHICLE_RIG_SLOT_ARCHIVED, "leftover rig dir -> ARCHIVED");
	check(strcmp(archived, want) == 0, "archived to <rig>_consumed_000");
	check(!exists(rig) && file_has(path, "first-run"),
		  "rig name free again, old world kept intact (never deleted)");
	check(MKDIR(rig) == 0, "the fresh rig directory can be reserved");

	/* 3. Next run's leftover takes the next free slot; 000 is untouched. */
	snprintf(path, sizeof(path), "%s/level.dat", rig);
	write_file(path, "second-run");
	slot = vehicle_smoke_rig_prepare_slot(rig, archived, sizeof(archived));
	snprintf(want, sizeof(want), "%s_consumed_001", rig);
	snprintf(path, sizeof(path), "%s/level.dat", want);
	char first[400];
	snprintf(first, sizeof(first), "%s_consumed_000/level.dat", rig);
	check(slot == VEHICLE_RIG_SLOT_ARCHIVED && strcmp(archived, want) == 0
			  && file_has(path, "second-run") && file_has(first, "first-run")
			  && !exists(rig),
		  "second leftover -> _consumed_001, _000 untouched");

	/* 4. A regular FILE holding the rig name is never moved: REFUSED. */
	write_file(rig, "not-a-world");
	slot = vehicle_smoke_rig_prepare_slot(rig, archived, sizeof(archived));
	check(slot == VEHICLE_RIG_SLOT_REFUSED && file_has(rig, "not-a-world")
			  && archived[0] == '\0',
		  "non-directory at the rig name -> REFUSED, untouched");
	remove(rig);

	/* 5. Every archive slot occupied: REFUSED, the leftover stays put. */
	for(unsigned k = 2; k < VEHICLE_RIG_ARCHIVE_MAX; k++) {
		snprintf(path, sizeof(path), "%s_consumed_%03u", rig, k);
		write_file(path, "x");
	}
	MKDIR(rig);
	snprintf(path, sizeof(path), "%s/level.dat", rig);
	write_file(path, "third-run");
	slot = vehicle_smoke_rig_prepare_slot(rig, archived, sizeof(archived));
	check(slot == VEHICLE_RIG_SLOT_REFUSED && is_dir(rig)
			  && file_has(path, "third-run"),
		  "no free archive slot -> REFUSED, leftover untouched");

	/* 6. Bad input. */
	check(vehicle_smoke_rig_prepare_slot(NULL, archived, sizeof(archived))
			  == VEHICLE_RIG_SLOT_REFUSED
			  && vehicle_smoke_rig_prepare_slot("", archived, sizeof(archived))
				  == VEHICLE_RIG_SLOT_REFUSED,
		  "NULL/empty path -> REFUSED");

	test_fixture_dir_cleanup(root);
	printf("%s test_vehicle_rig_slot (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
