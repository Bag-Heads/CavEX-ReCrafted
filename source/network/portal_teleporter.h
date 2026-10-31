/*
	Beta 1.7.3 Teleporter.java, exact server-side port (decompile 740c583).
	M5 portal travel: after a dimension switch, find the nearest existing
	portal within a 128-block square (257x257-column scan) and place the
	player in it; if none exists, build one (33x33-column two-pass candidate
	scoring, emergency platform fallback, 4x-repeated corners-INCLUDED frame)
	and search again.

	This file is part of CaveWii_v2 (GPLv3, like the rest of source/).
*/

#ifndef PORTAL_TELEPORTER_H
#define PORTAL_TELEPORTER_H

#include "../world.h"
#include "../worldgen/java_random.h"

struct server_local;
struct server_world;
struct portal_teleporter_job;

/* A2-T: the one state the Teleporter reads and writes on the travelling
 * entity (Beta Entity.setLocationAndAngles + motion zeroing). The legacy
 * begin() seeds it from s->player and writes it back on DONE; the transfer
 * transaction stages it and applies it at COMMIT. */
struct portal_teleporter_pose {
	double x, y, z;
	float rx, ry;
	float vel_x, vel_y, vel_z, old_vel_y;
};

enum portal_teleporter_result {
	PORTAL_TELEPORTER_WORKING,
	PORTAL_TELEPORTER_DONE,
	PORTAL_TELEPORTER_FAILED,
};

// Cooperative production path. begin() snapshots the scaled destination
// position and RNG state. Each step performs a bounded number of direct saved-
// chunk scans and terrain probes so portal travel cannot monopolize one Wii server
// tick. The caller owns the job and must destroy it on every terminal result.
struct portal_teleporter_job*
portal_teleporter_begin(struct server_local* s, const struct java_random* rng);
/* A2-T: the same cooperative search/build against an EXPLICIT world `w` from
 * the staged pose `start` (already in destination coordinates). The result is
 * read back with portal_teleporter_job_pose after DONE; nothing on s->player
 * is touched. */
struct portal_teleporter_job*
portal_teleporter_begin_w(struct server_local* s, struct server_world* w,
						  const struct portal_teleporter_pose* start,
						  const struct java_random* rng);
const struct portal_teleporter_pose*
portal_teleporter_job_pose(const struct portal_teleporter_job* job);
enum portal_teleporter_result
portal_teleporter_step(struct server_local* s,
					   struct portal_teleporter_job* job);
void portal_teleporter_destroy(struct portal_teleporter_job* job);
/* colcap-1006 (read-only): the chunks an EXISTING-portal winner needs
 * resident while the job loads them and places the traveller
 * (TP_JOB_LOAD_WINNER: the winner's chunk plus the seam neighbour a bottom
 * portal cell on a chunk edge reads). Writes at most `max` coordinates and
 * returns how many; 0 outside that stage. Changes nothing in the job: the
 * search order, RNG and chosen frame are untouched. */
unsigned portal_teleporter_job_winner_chunks(
	const struct portal_teleporter_job* job, w_coord_t* xs, w_coord_t* zs,
	unsigned max);
#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
struct portal_teleporter_test_snapshot {
	unsigned stage;
	w_coord_t cursor_x, cursor_z;
	unsigned retries;
	struct java_random rng;
	double best_dist;
	w_coord_t best_x, best_y, best_z;
	int best_orientation, orientation_start;
};
void portal_teleporter_test_snapshot_get(const struct portal_teleporter_job* job,
	struct portal_teleporter_test_snapshot* out);
void portal_teleporter_test_fail_next_step(void);
void portal_teleporter_test_fail_write_after(unsigned successful_writes);
bool portal_teleporter_test_write_failure_fired(void);

/* Deterministic work counters for the cooperative FIND stage. Saved chunks are
 * parsed into one scratch Blocks array and never installed in the live world;
 * resident chunks are scanned in place. */
struct portal_teleporter_find_stats {
	uint64_t find_columns;
	uint64_t chunk_resolves;
	uint64_t raw_id_reads;
	uint64_t world_get_block_calls;
	unsigned find_slices;
	unsigned max_columns_per_slice;
	bool completed;
};
void portal_teleporter_test_find_stats_reset(void);
void portal_teleporter_test_find_stats_get(
	struct portal_teleporter_find_stats* out);
void portal_teleporter_test_set_find_radius(unsigned radius);
unsigned portal_teleporter_test_default_find_radius(void);
bool portal_teleporter_test_create_volume_clear(
	struct server_local* s, w_coord_t x, w_coord_t y, w_coord_t z,
	int orientation, bool relaxed);
#endif

// Beta Teleporter.func_4107_a(world, entity) on the server player: search for
// an existing portal; else create one, then search again (the second search's
// failure is silently ignored, like Beta -- the player stays at the raw scaled
// coordinates). `rng` is the Teleporter's OWN Random (Beta constructs `new
// Teleporter()` per travel with a fresh time-seeded java.util.Random; its ONE
// draw -- nextInt(4), the createPortal orientation rotation -- never touches
// the world RNG stream). Production seeds it fresh per travel; goldens inject
// a fixed seed.
void portal_teleporter_place(struct server_local* s, struct java_random* rng);

#endif
