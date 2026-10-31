/* Copyright (c) 2026 CaveWii_v2 contributors
 * save_reload_3m (SPEC R12 real-Wii proof): the server-thread half.
 *
 * Armed ONLY by hw_run profile save_reload_3m (main.c); a disarmed probe is
 * one relaxed load and a return at each of its three call sites, so normal
 * play is unchanged.
 *
 *   on_load   (server thread, the load-completion pass right after the
 *              production inventory read, BEFORE the world clock resumes):
 *              first load records the world folder and starts the fixture
 *              delay; a load after the fixture was applied takes the AFTER
 *              snapshot.
 *   tick      (server thread, every tick): applies the fixed fixture once,
 *              ~5 s after the first load completed.
 *   capture / publish_save (server thread, server_local_unload_world_impl):
 *              the BEFORE snapshot is taken immediately before the production
 *              release/save and published only if the save-and-quit released
 *              the world.
 *
 * The fixture (disclosed, applied through the production world/TE stores,
 * like the debug demo fixtures): two placed blocks, two broken ground blocks,
 * a chest with four fixed stacks, a furnace loaded with iron ore + coal that
 * the normal furnace tick then smelts, and five fixed inventory stacks.
 * Nothing here changes a game rule. */
#ifndef NETWORK_SAVE_RELOAD_PROBE_H
#define NETWORK_SAVE_RELOAD_PROBE_H

#include <stdbool.h>
#include <stdint.h>

struct server_local;

#define SAVE_RELOAD_PROBE_FIXTURE_DELAY_TICKS 100 /* ~5 s after load */

struct save_reload_snap {
	uint64_t inv, chest, furnace, blocks, pos;
	int32_t burn, cook;
	double x, y, z;
	bool fixture_ok; /* every fixture cell/stack is what was applied */
	bool resident;	 /* the fixture chunks were resident when hashed */
};

struct save_reload_probe_view {
	uint32_t applied; /* 1 once the fixture edits were applied */
	uint32_t loads;	  /* world load completions observed while armed */
	uint32_t saves;	  /* save-and-quit snapshots published */
	uint32_t after_loads; /* load completions snapshotted after the fixture */
	struct save_reload_snap before, after;
	char folder[64]; /* save folder name of the first loaded world */
};

void save_reload_probe_arm(bool on);
bool save_reload_probe_armed(void);
void save_reload_probe_on_load(struct server_local* s);
void save_reload_probe_tick(struct server_local* s);
bool save_reload_probe_capture(struct server_local* s,
							   struct save_reload_snap* out);
void save_reload_probe_publish_save(const struct save_reload_snap* snap);
/* Main thread: a consistent copy of the published state (bounded retries). */
bool save_reload_probe_read(struct save_reload_probe_view* out);

/* Pure helper shared with the PC test: FNV-1a-64 over a byte range. */
uint64_t save_reload_fnv(uint64_t h, const void* p, unsigned n);
#define SAVE_RELOAD_FNV0 UINT64_C(0xCBF29CE484222325)

#endif
