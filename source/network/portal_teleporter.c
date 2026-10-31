/*
	Beta 1.7.3 Teleporter.java, exact server-side port (decompile 740c583).
	See portal_teleporter.h for the contract. Transcription notes:

	- Numbered locals remain naming debt in this comment-only pass; each loop
	  can still be checked against the cited decompile.
	- The FIND scan's chunk residency deliberately diverges from Beta in a
	  RESULT-IDENTICAL way: Beta's getBlockId force-loads-or-GENERATES every
	  chunk the 257x257-column scan touches (~289 chunks -- the infamous
	  portal lag spike). Here, chunks already resident are scanned as-is and
	  chunks ON DISK are parsed one at a time into a single 32 KiB Blocks scratch
	  buffer without restoring tile entities/entities; never-generated chunks are skipped:
	  a freshly generated chunk cannot contain portal blocks (portals are only
	  ever player- or Teleporter-built), so skipping generation cannot change
	  which portal is found -- it only avoids a ~289-chunk Broadway generation
	  storm per crossing. (Chunk-generation TIMING side effects fall inside the
	  Nether's own documented non-determinism: hell_populate_rng is one
	  continuous stream by Beta design; the overworld populate is per-chunk
	  seeded, so generation order never changes its output.)
	- The CREATE scan ensure_populates its whole candidate area first (site
	  scoring needs real terrain), matching Beta's on-demand generation.
	- Pass 2's air-descend loop is MISSING the y>0 guard in the decompile
	  (Teleporter.java:164 vs :114) -- a vanilla infinite-loop hazard over a
	  bedrock-less air column. We ADD the guard: unreachable divergence (both
	  dimensions floor with bedrock), and an infinite loop on the Wii is a
	  hard freeze. Documented sanctioned deviation.
	- The frame build is repeated FOUR times verbatim (a Beta quirk) and its
	  frame INCLUDES the corners (14 obsidian + 6 portal) -- unlike player-lit
	  frames, whose corners are exempt. Do not "fix" either.

	This file is part of CaveWii_v2 (GPLv3, like the rest of source/).
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "portal_teleporter.h"
#include "region_archive.h"
#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)
#include "server_transfer.h"
#include "server_world.h"
#include "../block/blocks.h"
#include "../block/blocks_data.h"
#include "../entity/combat.h" // PLAYER_EYE_HEIGHT
#include "../chunk.h"		  // WCOORD_CHUNK_OFFSET
#include "../platform/mem2.h"
#include "../platform/time.h"

#define TP_FIND_RADIUS 128

#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
static struct portal_teleporter_find_stats s_find_stats;
static bool s_capture_find_world_gets = false;
static w_coord_t s_test_find_radius = TP_FIND_RADIUS;

void portal_teleporter_test_find_stats_reset(void) {
	s_find_stats = (struct portal_teleporter_find_stats) {0};
	s_capture_find_world_gets = false;
}

void portal_teleporter_test_find_stats_get(
	struct portal_teleporter_find_stats* out) {
	assert(out);
	*out = s_find_stats;
}

void portal_teleporter_test_set_find_radius(unsigned radius) {
	assert(radius <= TP_FIND_RADIUS);
	s_test_find_radius = (w_coord_t)radius;
}

unsigned portal_teleporter_test_default_find_radius(void) {
	return TP_FIND_RADIUS;
}

#define TP_FIND_NOTE_CHUNK_RESOLVE() (s_find_stats.chunk_resolves++)
#define TP_FIND_RAW_ID(ptr) (s_find_stats.raw_id_reads++, *(ptr))
#else
#define TP_FIND_NOTE_CHUNK_RESOLVE() ((void)0)
#define TP_FIND_RAW_ID(ptr) (*(ptr))
#endif

static w_coord_t tp_find_radius(void) {
#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
	return s_test_find_radius;
#else
	return TP_FIND_RADIUS;
#endif
}

// Beta MathHelper.floor_double.
static w_coord_t floor_d(double v) {
	w_coord_t i = (w_coord_t)v;
	return v < (double)i ? i - 1 : i;
}

// Beta World.getBlockId semantics: y out of range or an unresolved chunk reads
// as air (see the residency note in the file header).
static int tp_id(struct server_world* w, w_coord_t x, w_coord_t y,
				 w_coord_t z) {
	struct block_data b;
	if(y < 0 || y >= WORLD_HEIGHT)
		return BLOCK_AIR;
#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
	if(s_capture_find_world_gets)
		s_find_stats.world_get_block_calls++;
#endif
	return server_world_get_block(w, x, y, z, &b) ? b.type : BLOCK_AIR;
}
static bool tp_air(struct server_world* w, w_coord_t x, w_coord_t y,
				   w_coord_t z) {
	return tp_id(w, x, y, z) == BLOCK_AIR;
}
// Beta getBlockMaterial(...).isSolid().
static bool tp_solid(struct server_world* w, w_coord_t x, w_coord_t y,
					 w_coord_t z) {
	return block_beta_material_is_solid(tp_id(w, x, y, z));
}

#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
static bool s_fail_portal_write_armed = false;
static bool s_fail_portal_write_fired = false;
static unsigned s_portal_writes_before_failure = 0;
#endif

static bool tp_set(struct server_local* s, struct server_world* w, w_coord_t x,
				   w_coord_t y, w_coord_t z, uint8_t type) {
	(void)s; /* the write goes to `w`; s stays for symmetry with the callers */
	if(y < 0 || y >= WORLD_HEIGHT) // Beta setBlock no-ops out of range
		return true;
#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
	if(s_fail_portal_write_armed && s_portal_writes_before_failure == 0) {
		s_fail_portal_write_armed = false;
		s_fail_portal_write_fired = true;
		return false;
	}
#endif
	bool written = server_world_set_block(
		w, x, y, z,
		(struct block_data) {.type = type,
							 .metadata = 0,
							 .sky_light = 0,
							 .torch_light = (type == BLOCK_PORTAL) ? 11 : 0});
#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
	if(written && s_fail_portal_write_armed)
		s_portal_writes_before_failure--;
#endif
	if(written)
		return true;

	/* Beta's makePortal ignores World.setBlock's return value while repeating
	 * the same 20-cell frame pass four times. Our transactional builder must
	 * still distinguish a real write failure (for example, an unavailable
	 * chunk) from Chunk.setBlockIDWithMetadata returning false because the cell
	 * is already exact. Validate the postcondition instead of treating Beta's
	 * ordinary repeated no-op as a failed portal build. */
	struct block_data live;
	return server_world_get_block(w, x, y, z, &live)
		&& live.type == type && live.metadata == 0;
}

// ---------------------------------------------------------------- func_4106_b
// Search a 128-half-width square (257x257 columns, y 127->0) for the nearest
// bottom-of-run portal block by squared 3D distance to its center; place the
// player there (+-0.5 axis centering, yaw kept, pitch forced 0, motion
// zeroed). Returns false (entity untouched) when no portal block exists.
static bool tp_find(struct server_local* s, struct server_world* w,
					struct portal_teleporter_pose* p) {
	(void)s;
	const w_coord_t var3 = tp_find_radius(); // Beta portal search radius: 128 blocks.
	double var4 = -1.0;
	w_coord_t var6 = 0, var7 = 0, var8 = 0;
	const w_coord_t var9 = floor_d(p->x);
	const w_coord_t var10 = floor_d(p->z);

	// Residency pre-pass (see file header): load every ON-DISK chunk in the
	// search square; skip never-generated ones (they cannot hold portals).
	for(w_coord_t cx = WCOORD_CHUNK_OFFSET(var9 - var3);
		cx <= WCOORD_CHUNK_OFFSET(var9 + var3); cx++)
		for(w_coord_t cz = WCOORD_CHUNK_OFFSET(var10 - var3);
			cz <= WCOORD_CHUNK_OFFSET(var10 + var3); cz++)
			if(server_world_disk_has_chunk(w, cx, cz)) {
				struct server_chunk* sc = NULL;
				server_world_load_chunk(w, cx, cz, &sc);
			}

	// Beta loop order (tie-break relevant, strict <): x asc, z asc, y desc.
	for(w_coord_t var14 = var9 - var3; var14 <= var9 + var3; var14++) {
		double var12 = (double)var14 + 0.5 - p->x;
		for(w_coord_t var17 = var10 - var3; var17 <= var10 + var3; var17++) {
			double var15 = (double)var17 + 0.5 - p->z;
			for(w_coord_t var20 = WORLD_HEIGHT - 1; var20 >= 0; var20--) {
				if(tp_id(w, var14, var20, var17) == BLOCK_PORTAL) {
					// Descend to the BOTTOM of the contiguous portal run; the
					// enclosing loop then continues below it, so stacked
					// portals in one column each score once.
					while(tp_id(w, var14, var20 - 1, var17) == BLOCK_PORTAL)
						var20--;
					double var18 = (double)var20 + 0.5 - p->y;
					double var21 = var12 * var12 + var18 * var18 + var15 * var15;
					if(var4 < 0.0 || var21 < var4) {
						var4 = var21;
						var6 = var14;
						var7 = var20;
						var8 = var17;
					}
				}
			}
		}
	}

	if(var4 >= 0.0) {
		double var22 = (double)var6 + 0.5;
		double var16 = (double)var7 + 0.5;
		double var18 = (double)var8 + 0.5;
		// Four INDEPENDENT ifs: center the player on the portal plane along
		// its axis (both-sides shifts cancel for interior cells).
		if(tp_id(w, var6 - 1, var7, var8) == BLOCK_PORTAL)
			var22 -= 0.5;
		if(tp_id(w, var6 + 1, var7, var8) == BLOCK_PORTAL)
			var22 += 0.5;
		if(tp_id(w, var6, var7, var8 - 1) == BLOCK_PORTAL)
			var18 -= 0.5;
		if(tp_id(w, var6, var7, var8 + 1) == BLOCK_PORTAL)
			var18 += 0.5;
		// Beta places the entity at the selected portal centre with zero pitch:
		// The stored Y includes the entity offset [decompile 740c583 Entity.java:671], and the
		// server player's y IS the eye (yOffset = 1.62). Yaw (rx) preserved,
		// pitch (ry) forced 0 -- server ry 0 maps to the client's level view.
		p->x = var22;
		p->y = var16 + PLAYER_EYE_HEIGHT;
		p->z = var18;
		p->ry = 0.0f;
		// Beta: motionX = motionY = motionZ = 0.
		p->vel_y = 0.0f;
		p->old_vel_y = 0.0f;
		p->vel_x = 0.0f;
		p->vel_z = 0.0f;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------- func_4108_c
// Build a portal near the player: two candidate-scoring passes over a
// 16-half-width square, an emergency y[70..118] platform fallback, then the
// 4x-repeated corners-INCLUDED frame under editing_blocks + a manual
// neighbour-notify sweep.
static void tp_create(struct server_local* s, struct server_world* w,
					  struct portal_teleporter_pose* p, struct java_random* rng) {
	const w_coord_t var3 = 16; // Beta portal creation search radius: 16 blocks.
	double var4 = -1.0;
	const w_coord_t var6 = floor_d(p->x);
	const w_coord_t var7 = floor_d(p->y);
	const w_coord_t var8 = floor_d(p->z);
	w_coord_t var9 = var6, var10 = var7, var11 = var8;
	int var12 = 0;
	const int var13 = jrand_next_int_bound(rng, 4); // the ONE RNG draw

	// Residency: the candidate volumes probe up to 3 cells beyond the +-16
	// scan, so ensure the covering chunk square is fully populated first --
	// the scoring needs real terrain, matching Beta's generate-on-demand
	// getBlockId reads.
	for(w_coord_t cx = WCOORD_CHUNK_OFFSET(var6 - (var3 + 4));
		cx <= WCOORD_CHUNK_OFFSET(var6 + var3 + 4); cx++)
		for(w_coord_t cz = WCOORD_CHUNK_OFFSET(var8 - (var3 + 4));
			cz <= WCOORD_CHUNK_OFFSET(var8 + var3 + 4); cz++) {
			struct server_chunk* sc = NULL;
			server_world_ensure_populated(w, s, cx, cz, &sc);
		}

	// ---- Pass 1: full-clearance sites, 4 orientations ----
	for(w_coord_t var14 = var6 - var3; var14 <= var6 + var3; var14++) {
		double var15 = (double)var14 + 0.5 - p->x;
		for(w_coord_t var17 = var8 - var3; var17 <= var8 + var3; var17++) {
			double var18 = (double)var17 + 0.5 - p->z;
			for(w_coord_t var20 = WORLD_HEIGHT - 1; var20 >= 0; var20--) {
				if(!tp_air(w, var14, var20, var17))
					continue;
				// Descend to the bottom of this air run (pass 1 HAS the >0
				// guard in the decompile).
				while(var20 > 0 && tp_air(w, var14, var20 - 1, var17))
					var20--;
				for(int var21 = var13; var21 < var13 + 4; var21++) {
					w_coord_t var22 = var21 % 2;
					w_coord_t var23 = 1 - var22;
					if(var21 % 4 >= 2) {
						var22 = -var22;
						var23 = -var23;
					}
					// Volume: dy=-1 floor SOLID over the 4(axis)x3(perp)
					// footprint, dy 0..3 AIR. Any failing cell abandons ALL
					// remaining orientations at this air run (Beta's labeled
					// continue) -- goto is the direct transcription.
					for(w_coord_t var24 = 0; var24 < 3; var24++)
						for(w_coord_t var25 = 0; var25 < 4; var25++)
							for(w_coord_t var26 = -1; var26 <= 3; var26++) {
								w_coord_t var27 = var14 + (var25 - 1) * var22
									+ var24 * var23;
								w_coord_t var28 = var20 + var26;
								w_coord_t var29 = var17 + (var25 - 1) * var23
									- var24 * var22;
								if((var26 < 0
									&& !tp_solid(w, var27, var28, var29))
								   || (var26 >= 0
									   && !tp_air(w, var27, var28, var29)))
									goto pass1_next_y;
							}
					{
						double var30 = (double)var20 + 0.5 - p->y;
						double var32 = var15 * var15 + var30 * var30
							+ var18 * var18;
						if(var4 < 0.0 || var32 < var4) {
							var4 = var32;
							var9 = var14;
							var10 = var20;
							var11 = var17;
							var12 = var21 % 4;
						}
					}
				}
			pass1_next_y:;
			}
		}
	}

	// ---- Pass 2: relaxed sites (bare 4-strip floor), 2 orientations ----
	if(var4 < 0.0) {
		for(w_coord_t var14 = var6 - var3; var14 <= var6 + var3; var14++) {
			double var15 = (double)var14 + 0.5 - p->x;
			for(w_coord_t var17 = var8 - var3; var17 <= var8 + var3; var17++) {
				double var18 = (double)var17 + 0.5 - p->z;
				for(w_coord_t var20 = WORLD_HEIGHT - 1; var20 >= 0; var20--) {
					if(!tp_air(w, var14, var20, var17))
						continue;
					// SANCTIONED DEVIATION: the decompile's pass-2 descend
					// lacks the var20 > 0 guard (vanilla infinite loop over a
					// bedrock-less air column). Guard added -- unreachable
					// divergence, and a hang here would hard-freeze the Wii.
					while(var20 > 0 && tp_air(w, var14, var20 - 1, var17))
						var20--;
					for(int var21 = var13; var21 < var13 + 2; var21++) {
						w_coord_t var22 = var21 % 2;
						w_coord_t var23 = 1 - var22;
						for(w_coord_t var24 = 0; var24 < 4; var24++)
							for(w_coord_t var25 = -1; var25 <= 3; var25++) {
								w_coord_t var27
									= var14 + (var24 - 1) * var22;
								w_coord_t var28 = var20 + var25;
								w_coord_t var29
									= var17 + (var24 - 1) * var23;
								if((var25 < 0
									&& !tp_solid(w, var27, var28, var29))
								   || (var25 >= 0
									   && !tp_air(w, var27, var28, var29)))
									goto pass2_next_y;
							}
						{
							double var30 = (double)var20 + 0.5 - p->y;
							double var32 = var15 * var15 + var30 * var30
								+ var18 * var18;
							if(var4 < 0.0 || var32 < var4) {
								var4 = var32;
								var9 = var14;
								var10 = var20;
								var11 = var17;
								var12 = var21 % 2;
							}
						}
					}
				pass2_next_y:;
				}
			}
		}
	}

	// ---- Decode orientation; emergency platform when nothing scored ----
	w_coord_t var30x = var9;
	w_coord_t var16y = var10;
	w_coord_t var17z = var11;
	w_coord_t var31 = var12 % 2;
	w_coord_t var19 = 1 - var31;
	if(var12 % 4 >= 2) {
		var31 = -var31;
		var19 = -var19;
	}
	if(var4 < 0.0) {
		if(var10 < 70)
			var10 = 70;
		if(var10 > 118)
			var10 = 118;
		var16y = var10;
		// Floating 2x3 obsidian slab at dy=-1 with a 2x3x3 air pocket above,
		// built with NOTIFYING sets (Beta runs this with editingBlocks false).
		for(w_coord_t var20 = -1; var20 <= 1; var20++)
			for(w_coord_t var21 = 1; var21 < 3; var21++)
				for(w_coord_t var22 = -1; var22 < 3; var22++) {
					w_coord_t var27
						= var30x + (var21 - 1) * var31 + var20 * var19;
					w_coord_t var28 = var16y + var22;
					w_coord_t var29
						= var17z + (var21 - 1) * var19 - var20 * var31;
					tp_set(s, w, var27, var28, var29,
						   var22 < 0 ? BLOCK_OBSIDIAN : BLOCK_AIR);
				}
	}

	// ---- Frame build: 4x repeated VERBATIM (Beta quirk); corners INCLUDED ----
	for(int var20 = 0; var20 < 4; var20++) {
		w->editing_blocks = true;
		for(w_coord_t var21 = 0; var21 < 4; var21++)
			for(w_coord_t var22 = -1; var22 < 4; var22++) {
				w_coord_t var27 = var30x + (var21 - 1) * var31;
				w_coord_t var28 = var16y + var22;
				w_coord_t var29 = var17z + (var21 - 1) * var19;
				bool var34 = var21 == 0 || var21 == 3 || var22 == -1
					|| var22 == 3;
				tp_set(s, w, var27, var28, var29,
					   var34 ? BLOCK_OBSIDIAN : BLOCK_PORTAL);
			}
		w->editing_blocks = false;
		// Beta: notifyBlocksOfNeighborChange(x, y, z, getBlockId(x,y,z)) over
		// the same 20 cells -- notifyNeighbours reads the cell's CURRENT id
		// as the origin, the exact same contract.
		for(w_coord_t var21 = 0; var21 < 4; var21++)
			for(w_coord_t var22 = -1; var22 < 4; var22++) {
				w_coord_t var27 = var30x + (var21 - 1) * var31;
				w_coord_t var28 = var16y + var22;
				w_coord_t var29 = var17z + (var21 - 1) * var19;
				if(var28 >= 0 && var28 < WORLD_HEIGHT)
					notifyNeighbours_w(s, w, var27, var28, var29);
			}
	}
}

// ---------------------------------------------------------------- cooperative driver

// The original Teleporter performs millions of block reads in one call. That is
// acceptable for a synchronous PC golden, but it can starve the single Broadway
// core long enough to trip the Wii watchdog. The production driver below keeps
// the exact scan order and scoring while slicing it at column boundaries.
enum tp_job_stage {
	TP_JOB_FIND,
	TP_JOB_LOAD_WINNER,
	TP_JOB_LOAD_CREATE,
	TP_JOB_CREATE_PASS1,
	TP_JOB_CREATE_PASS2,
	TP_JOB_BUILD,
};

enum {
	TP_FIND_IDS_BYTES = CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT,
	/* A single compressed chunk can exceed this budget; always finish that one,
	 * then yield. The hard chunk cap protects coarse platform timers. */
	TP_FIND_BUDGET_MS = 8,
	TP_FIND_CHUNKS_PER_STEP_MAX = 4,
	TP_FIND_COORDS_PER_STEP_MAX = 32,
	TP_FIND_REGION_MAX = 4,
	TP_WINNER_CHUNK_MAX = 3,
};

struct tp_find_region {
	w_coord_t x, z;
	struct region_archive* archive;
	enum server_world_region_result result;
};

struct portal_teleporter_job {
	enum tp_job_stage stage;
	struct java_random rng;
	/* A2-T: the world searched/built and the travelling pose. The legacy
	 * begin() points these at slot 0 / a copy of s->player (written back on
	 * DONE); the transfer transaction supplies the destination world and its
	 * staged pose and reads the result with portal_teleporter_job_pose. */
	struct server_world* w;
	struct portal_teleporter_pose pose;
	bool legacy_player;
	w_coord_t base_x, base_y, base_z;
	w_coord_t min_x, max_x, min_z, max_z;
	w_coord_t cursor_x, cursor_z;
	double best_dist;
	w_coord_t best_x, best_y, best_z;
	int best_orientation;
	int orientation_start;
	unsigned retries;
	uint8_t* find_ids;
	struct tp_find_region find_regions[TP_FIND_REGION_MAX];
	unsigned find_region_count;
	w_coord_t winner_chunks_x[TP_WINNER_CHUNK_MAX];
	w_coord_t winner_chunks_z[TP_WINNER_CHUNK_MAX];
	unsigned winner_chunk_count;
	unsigned winner_chunk_index;
};

#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
void portal_teleporter_test_snapshot_get(const struct portal_teleporter_job* job,
	struct portal_teleporter_test_snapshot* out) {
	assert(job && out);
	*out = (struct portal_teleporter_test_snapshot) {
		.stage = (unsigned)job->stage,
		.cursor_x = job->cursor_x,
		.cursor_z = job->cursor_z,
		.retries = job->retries,
		.rng = job->rng,
		.best_dist = job->best_dist,
		.best_x = job->best_x,
		.best_y = job->best_y,
		.best_z = job->best_z,
		.best_orientation = job->best_orientation,
		.orientation_start = job->orientation_start,
	};
}
static bool s_fail_next_portal_step = false;
void portal_teleporter_test_fail_next_step(void) {
	s_fail_next_portal_step = true;
}
void portal_teleporter_test_fail_write_after(unsigned successful_writes) {
	s_portal_writes_before_failure = successful_writes;
	s_fail_portal_write_fired = false;
	s_fail_portal_write_armed = true;
}
bool portal_teleporter_test_write_failure_fired(void) {
	return s_fail_portal_write_fired;
}
#endif

enum {
	/* CREATE performs data-dependent clearance probes, so keep its batch much
	 * smaller than FIND while removing the old 545-slice scheduling floor. */
	TP_CREATE_COLUMNS_PER_STEP = 8,
};

static void tp_job_set_range(struct portal_teleporter_job* job,
							 w_coord_t min_x, w_coord_t max_x,
							 w_coord_t min_z, w_coord_t max_z) {
	job->min_x = min_x;
	job->max_x = max_x;
	job->min_z = min_z;
	job->max_z = max_z;
	job->cursor_x = min_x;
	job->cursor_z = min_z;
}

static bool tp_job_advance(struct portal_teleporter_job* job) {
	job->cursor_z++;
	if(job->cursor_z > job->max_z) {
		job->cursor_z = job->min_z;
		job->cursor_x++;
	}
	return job->cursor_x > job->max_x;
}

static void tp_job_reset_best(struct portal_teleporter_job* job) {
	job->best_dist = -1.0;
	job->best_x = job->best_y = job->best_z = 0;
	job->best_orientation = 0;
}

static void tp_place_at(struct server_world* w, struct portal_teleporter_pose* p,
						w_coord_t x, w_coord_t y, w_coord_t z) {
	double px = (double)x + 0.5;
	double py = (double)y + 0.5;
	double pz = (double)z + 0.5;
	if(tp_id(w, x - 1, y, z) == BLOCK_PORTAL)
		px -= 0.5;
	if(tp_id(w, x + 1, y, z) == BLOCK_PORTAL)
		px += 0.5;
	if(tp_id(w, x, y, z - 1) == BLOCK_PORTAL)
		pz -= 0.5;
	if(tp_id(w, x, y, z + 1) == BLOCK_PORTAL)
		pz += 0.5;
	p->x = px;
	p->y = py + PLAYER_EYE_HEIGHT;
	p->z = pz;
	p->ry = 0.0f;
	p->vel_x = 0.0f;
	p->vel_y = 0.0f;
	p->vel_z = 0.0f;
	p->old_vel_y = 0.0f;
}

static bool tp_job_candidate_better(const struct portal_teleporter_job* job,
									double dist, w_coord_t x, w_coord_t y,
									w_coord_t z) {
	if(job->best_dist < 0.0 || dist < job->best_dist)
		return true;
	if(dist > job->best_dist)
		return false;
	/* Chunk-at-a-time scanning differs from Beta's global loop order, so encode
	 * the strict-< tie rank explicitly: x asc, z asc, then higher run first. */
	if(x != job->best_x)
		return x < job->best_x;
	if(z != job->best_z)
		return z < job->best_z;
	return y > job->best_y;
}

static enum server_world_region_result tp_job_find_region(
	struct server_local* s, struct portal_teleporter_job* job,
	w_coord_t cx, w_coord_t cz, struct region_archive** out) {
	w_coord_t rx = CHUNK_REGION_COORD(cx);
	w_coord_t rz = CHUNK_REGION_COORD(cz);
	/* A2-T: a pool HANDLE pointer is a synchronous borrow (region_pool.h):
	 * the session pool is shared by both worlds and a slot can be re-used by
	 * another region between two steps, so only the stable ABSENT answer is
	 * cached; an OPEN region is re-acquired every time (an LRU touch). */
	for(unsigned i = 0; i < job->find_region_count; i++) {
		struct tp_find_region* cached = &job->find_regions[i];
		if(cached->x == rx && cached->z == rz) {
			if(cached->result == SERVER_WORLD_REGION_ABSENT) {
				*out = NULL;
				return cached->result;
			}
			enum server_world_region_result again
				= server_world_chunk_region_checked(job->w, cx, cz, out);
			if(again != SERVER_WORLD_REGION_ERROR)
				cached->result = again;
			return again;
		}
	}
	if(job->find_region_count >= TP_FIND_REGION_MAX) {
		*out = NULL;
		return SERVER_WORLD_REGION_ERROR;
	}

	struct region_archive* archive = NULL;
	enum server_world_region_result result
		= server_world_chunk_region_checked(job->w, cx, cz, &archive);
	/* An I/O error may be transient. Cache only stable success/absence so the
	 * bounded retry below re-attempts corrupt/unreadable acquisition. */
	if(result != SERVER_WORLD_REGION_ERROR) {
		job->find_regions[job->find_region_count++] = (struct tp_find_region) {
			.x = rx, .z = rz, .archive = NULL, .result = result,
		};
	}
	*out = archive;
	return result;
}

static unsigned tp_job_scan_ids(struct server_local* s,
								struct portal_teleporter_job* job,
								w_coord_t cx, w_coord_t cz,
								const uint8_t* ids) {
	assert(s && job && ids);
	w_coord_t chunk_min_x = cx * CHUNK_SIZE;
	w_coord_t chunk_min_z = cz * CHUNK_SIZE;
	w_coord_t min_x = chunk_min_x > job->base_x - tp_find_radius()
		? chunk_min_x : job->base_x - tp_find_radius();
	w_coord_t max_x = chunk_min_x + CHUNK_SIZE - 1
		< job->base_x + tp_find_radius()
		? chunk_min_x + CHUNK_SIZE - 1 : job->base_x + tp_find_radius();
	w_coord_t min_z = chunk_min_z > job->base_z - tp_find_radius()
		? chunk_min_z : job->base_z - tp_find_radius();
	w_coord_t max_z = chunk_min_z + CHUNK_SIZE - 1
		< job->base_z + tp_find_radius()
		? chunk_min_z + CHUNK_SIZE - 1 : job->base_z + tp_find_radius();
	unsigned columns = 0;
	for(w_coord_t x = min_x; x <= max_x; x++) {
		double dx = (double)x + 0.5 - job->pose.x;
		for(w_coord_t z = min_z; z <= max_z; z++) {
			double dz = (double)z + 0.5 - job->pose.z;
			const uint8_t* column = ids
				+ ((size_t)W2C_COORD(z)
				   + (size_t)W2C_COORD(x) * CHUNK_SIZE) * WORLD_HEIGHT;
			for(w_coord_t y = WORLD_HEIGHT - 1; y >= 0; y--) {
				if(TP_FIND_RAW_ID(column + y) != BLOCK_PORTAL)
					continue;
				while(y > 0
					  && TP_FIND_RAW_ID(column + y - 1) == BLOCK_PORTAL)
					y--;
				double dy = (double)y + 0.5 - job->pose.y;
				double dist = dx * dx + dy * dy + dz * dz;
				if(tp_job_candidate_better(job, dist, x, y, z)) {
					job->best_dist = dist;
					job->best_x = x;
					job->best_y = y;
					job->best_z = z;
				}
			}
			columns++;
		}
	}
	return columns;
}

static void tp_job_add_winner_chunk(struct portal_teleporter_job* job,
									w_coord_t cx, w_coord_t cz) {
	for(unsigned i = 0; i < job->winner_chunk_count; i++)
		if(job->winner_chunks_x[i] == cx && job->winner_chunks_z[i] == cz)
			return;
	assert(job->winner_chunk_count < TP_WINNER_CHUNK_MAX);
	unsigned i = job->winner_chunk_count++;
	job->winner_chunks_x[i] = cx;
	job->winner_chunks_z[i] = cz;
}

static void tp_job_prepare_winner_chunks(struct portal_teleporter_job* job) {
	w_coord_t cx = WCOORD_CHUNK_OFFSET(job->best_x);
	w_coord_t cz = WCOORD_CHUNK_OFFSET(job->best_z);
	tp_job_add_winner_chunk(job, cx, cz);
	if(W2C_COORD(job->best_x) == 0)
		tp_job_add_winner_chunk(job, cx - 1, cz);
	else if(W2C_COORD(job->best_x) == CHUNK_SIZE - 1)
		tp_job_add_winner_chunk(job, cx + 1, cz);
	if(W2C_COORD(job->best_z) == 0)
		tp_job_add_winner_chunk(job, cx, cz - 1);
	else if(W2C_COORD(job->best_z) == CHUNK_SIZE - 1)
		tp_job_add_winner_chunk(job, cx, cz + 1);
	job->winner_chunk_index = 0;
}

static enum portal_teleporter_result tp_job_find_chunks(
	struct server_local* s, struct portal_teleporter_job* job) {
	ptime_t started = time_get();
	unsigned coordinates = 0;
	unsigned scanned_chunks = 0;
	unsigned columns = 0;
	// True reads live only in the CAVEWII_PORTAL_TELEPORTER_TEST stats block
	// below; plain builds just accumulate it (+= counts as a set, not a use).
	(void)columns;
#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
	s_find_stats.find_slices++;
#endif
	while(job->cursor_x <= job->max_x
		  && coordinates < TP_FIND_COORDS_PER_STEP_MAX
		  && scanned_chunks < TP_FIND_CHUNKS_PER_STEP_MAX) {
		w_coord_t cx = job->cursor_x;
		w_coord_t cz = job->cursor_z;
		const uint8_t* ids = NULL;
		TP_FIND_NOTE_CHUNK_RESOLVE();
		struct server_chunk* resident = dict_server_chunks_get(
			job->w->chunks, S_CHUNK_ID(cx, cz));
		if(resident) {
			if(!resident->ids)
				return PORTAL_TELEPORTER_FAILED;
			ids = resident->ids;
		} else {
			struct region_archive* archive = NULL;
			enum server_world_region_result region_result
				= tp_job_find_region(s, job, cx, cz, &archive);
			if(region_result == SERVER_WORLD_REGION_ERROR) {
				if(++job->retries < 8)
					return PORTAL_TELEPORTER_WORKING;
				fprintf(stderr,
						"portal_teleporter: failed opening search region for (%d,%d)\n",
						(int)cx, (int)cz);
				return PORTAL_TELEPORTER_FAILED;
			}
			if(region_result == SERVER_WORLD_REGION_ABSENT) {
				job->retries = 0;
				coordinates++;
				tp_job_advance(job);
				continue;
			}
			enum region_archive_read_result read_result
				= region_archive_get_block_ids(archive, cx, cz, job->find_ids);
			if(read_result == REGION_ARCHIVE_READ_ERROR) {
				if(++job->retries < 8)
					return PORTAL_TELEPORTER_WORKING;
				fprintf(stderr,
						"portal_teleporter: failed reading saved Blocks for (%d,%d)\n",
						(int)cx, (int)cz);
				return PORTAL_TELEPORTER_FAILED;
			}
			if(read_result == REGION_ARCHIVE_READ_ABSENT) {
				job->retries = 0;
				coordinates++;
				tp_job_advance(job);
				continue;
			}
			ids = job->find_ids;
		}

		job->retries = 0;
		columns += tp_job_scan_ids(s, job, cx, cz, ids);
		scanned_chunks++;
		coordinates++;
		tp_job_advance(job);
		if(time_diff_ms(started, time_get()) >= TP_FIND_BUDGET_MS)
			break;
	}

	bool complete = job->cursor_x > job->max_x;
#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
	s_find_stats.find_columns += columns;
	if(columns > s_find_stats.max_columns_per_slice)
		s_find_stats.max_columns_per_slice = columns;
	if(complete)
		s_find_stats.completed = true;
#endif
	if(!complete)
		return PORTAL_TELEPORTER_WORKING;
	if(job->best_dist >= 0.0) {
		tp_job_prepare_winner_chunks(job);
		job->stage = TP_JOB_LOAD_WINNER;
		return PORTAL_TELEPORTER_WORKING;
	}

	job->orientation_start = jrand_next_int_bound(&job->rng, 4);
	tp_job_set_range(
		job, WCOORD_CHUNK_OFFSET(job->base_x - 20),
		WCOORD_CHUNK_OFFSET(job->base_x + 20),
		WCOORD_CHUNK_OFFSET(job->base_z - 20),
		WCOORD_CHUNK_OFFSET(job->base_z + 20));
	job->stage = TP_JOB_LOAD_CREATE;
	return PORTAL_TELEPORTER_WORKING;
}

static enum portal_teleporter_result tp_job_load_winner(
	struct server_local* s, struct portal_teleporter_job* job) {
	while(job->winner_chunk_index < job->winner_chunk_count) {
		unsigned i = job->winner_chunk_index;
		w_coord_t cx = job->winner_chunks_x[i];
		w_coord_t cz = job->winner_chunks_z[i];
		if(server_world_is_chunk_loaded(job->w, cx, cz)) {
			job->winner_chunk_index++;
			continue;
		}
		struct region_archive* archive = NULL;
		enum server_world_region_result region_result
			= tp_job_find_region(s, job, cx, cz, &archive);
		if(region_result == SERVER_WORLD_REGION_ERROR) {
			if(++job->retries < 8)
				return PORTAL_TELEPORTER_WORKING;
			return PORTAL_TELEPORTER_FAILED;
		}
		bool exists = false;
		if(region_result == SERVER_WORLD_REGION_ABSENT
		   || (region_archive_contains(archive, cx, cz, &exists) && !exists)) {
			job->retries = 0;
			job->winner_chunk_index++;
			continue;
		}
		struct server_chunk* loaded = NULL;
		server_transfer_column_wait_reset(job->w);
		if(!server_world_load_chunk(job->w, cx, cz, &loaded)) {
			if(server_transfer_column_waiting(job->w))
				return PORTAL_TELEPORTER_WORKING;
			if(++job->retries < 8)
				return PORTAL_TELEPORTER_WORKING;
			return PORTAL_TELEPORTER_FAILED;
		}
		job->retries = 0;
		job->winner_chunk_index++;
		return PORTAL_TELEPORTER_WORKING;
	}
#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
	s_capture_find_world_gets = true;
#endif
	tp_place_at(job->w, &job->pose, job->best_x, job->best_y, job->best_z);
#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
	s_capture_find_world_gets = false;
#endif
	return PORTAL_TELEPORTER_DONE;
}

static enum portal_teleporter_result
tp_job_load_create(struct server_local* s, struct portal_teleporter_job* job) {
	if(job->cursor_x > job->max_x) {
		tp_job_set_range(job, job->base_x - 16, job->base_x + 16,
						 job->base_z - 16, job->base_z + 16);
		tp_job_reset_best(job);
		job->stage = TP_JOB_CREATE_PASS1;
		job->retries = 0;
		return PORTAL_TELEPORTER_WORKING;
	}
	struct server_chunk* sc = NULL;
	server_transfer_column_wait_reset(job->w);
	if(!server_world_ensure_populated(job->w, s, job->cursor_x,
									  job->cursor_z, &sc)) {
		if(server_transfer_column_waiting(job->w))
			return PORTAL_TELEPORTER_WORKING;
		// ensure_populated deliberately returns false when its per-cycle
		// generation budget defers a covering quadrant. Retry the same chunk
		// on the next server slice; only a persistent failure aborts travel.
		if(++job->retries < 256)
			return PORTAL_TELEPORTER_WORKING;
		fprintf(stderr,
				"portal_teleporter: failed populating destination chunk (%d,%d)\n",
				(int)job->cursor_x, (int)job->cursor_z);
		return PORTAL_TELEPORTER_FAILED;
	}
	job->retries = 0;
	tp_job_advance(job);
	return PORTAL_TELEPORTER_WORKING;
}

static bool tp_create_volume_clear(struct server_world* w, w_coord_t x,
								   w_coord_t y, w_coord_t z,
								   int orientation, bool relaxed) {
	w_coord_t ax = orientation % 2;
	w_coord_t az = 1 - ax;
	/* Beta pass 1 rotates through four signed directions; relaxed pass 2
	 * checks only the two positive parity axes even when var13 starts at 2/3.
	 * The caller later stores o%2 and builds in that same positive direction. */
	if(!relaxed && orientation % 4 >= 2) {
		ax = -ax;
		az = -az;
	}
	if(relaxed) {
		for(w_coord_t along = 0; along < 4; along++)
			for(w_coord_t dy = -1; dy <= 3; dy++) {
				w_coord_t bx = x + (along - 1) * ax;
				w_coord_t bz = z + (along - 1) * az;
				if((dy < 0 && !tp_solid(w, bx, y + dy, bz))
				   || (dy >= 0 && !tp_air(w, bx, y + dy, bz)))
					return false;
			}
		return true;
	}

	for(w_coord_t across = 0; across < 3; across++)
		for(w_coord_t along = 0; along < 4; along++)
			for(w_coord_t dy = -1; dy <= 3; dy++) {
				w_coord_t bx = x + (along - 1) * ax + across * az;
				w_coord_t bz = z + (along - 1) * az - across * ax;
				if((dy < 0 && !tp_solid(w, bx, y + dy, bz))
				   || (dy >= 0 && !tp_air(w, bx, y + dy, bz)))
					return false;
			}
	return true;
}

#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
bool portal_teleporter_test_create_volume_clear(
	struct server_local* s, w_coord_t x, w_coord_t y, w_coord_t z,
	int orientation, bool relaxed) {
	return tp_create_volume_clear(&s->world, x, y, z, orientation, relaxed);
}
#endif

static void tp_job_create_column(struct server_local* s,
								 struct portal_teleporter_job* job,
								 bool relaxed) {
	w_coord_t x = job->cursor_x;
	w_coord_t z = job->cursor_z;
	double dx = (double)x + 0.5 - job->pose.x;
	double dz = (double)z + 0.5 - job->pose.z;
	int orientations = relaxed ? 2 : 4;
	for(w_coord_t y = WORLD_HEIGHT - 1; y >= 0; y--) {
		if(!tp_air(job->w, x, y, z))
			continue;
		while(y > 0 && tp_air(job->w, x, y - 1, z))
			y--;
		for(int o = job->orientation_start;
			o < job->orientation_start + orientations; o++) {
			if(!tp_create_volume_clear(job->w, x, y, z, o, relaxed))
				break; // Beta's labeled continue skips later orientations here.
			double dy = (double)y + 0.5 - job->pose.y;
			double dist = dx * dx + dy * dy + dz * dz;
			if(job->best_dist < 0.0 || dist < job->best_dist) {
				job->best_dist = dist;
				job->best_x = x;
				job->best_y = y;
				job->best_z = z;
				job->best_orientation = o % (relaxed ? 2 : 4);
			}
		}
	}
}

static bool tp_job_create_columns(struct server_local* s,
								  struct portal_teleporter_job* job,
								  bool relaxed) {
	int columns = 0;
	while(job->cursor_x <= job->max_x && columns < TP_CREATE_COLUMNS_PER_STEP) {
		tp_job_create_column(s, job, relaxed);
		columns++;
		tp_job_advance(job);
	}
	return job->cursor_x > job->max_x;
}

static bool tp_job_build(struct server_local* s,
					 struct portal_teleporter_job* job) {
	w_coord_t x = job->best_x;
	w_coord_t y = job->best_y;
	w_coord_t z = job->best_z;
	int orientation = job->best_orientation;
	if(job->best_dist < 0.0) {
		x = job->base_x;
		y = job->base_y;
		z = job->base_z;
		orientation = 0;
		if(y < 70)
			y = 70;
		if(y > 118)
			y = 118;
	}
	w_coord_t ax = orientation % 2;
	w_coord_t az = 1 - ax;
	if(orientation % 4 >= 2) {
		ax = -ax;
		az = -az;
	}

	if(job->best_dist < 0.0) {
		for(w_coord_t across = -1; across <= 1; across++)
			for(w_coord_t along = 1; along < 3; along++)
				for(w_coord_t dy = -1; dy < 3; dy++) {
					w_coord_t bx = x + (along - 1) * ax + across * az;
					w_coord_t bz = z + (along - 1) * az - across * ax;
					if(!tp_set(s, job->w, bx, y + dy, bz,
							   dy < 0 ? BLOCK_OBSIDIAN : BLOCK_AIR))
						return false;
				}
	}

	for(int repeat = 0; repeat < 4; repeat++) {
		job->w->editing_blocks = true;
		for(w_coord_t along = 0; along < 4; along++)
			for(w_coord_t dy = -1; dy < 4; dy++) {
				w_coord_t bx = x + (along - 1) * ax;
				w_coord_t bz = z + (along - 1) * az;
				bool frame = along == 0 || along == 3 || dy == -1 || dy == 3;
				if(!tp_set(s, job->w, bx, y + dy, bz,
							   frame ? BLOCK_OBSIDIAN : BLOCK_PORTAL)) {
					job->w->editing_blocks = false;
					return false;
				}
			}
		job->w->editing_blocks = false;
		for(w_coord_t along = 0; along < 4; along++)
			for(w_coord_t dy = -1; dy < 4; dy++)
				notifyNeighbours_w(s, job->w, x + (along - 1) * ax, y + dy,
								 z + (along - 1) * az);
	}

	// The newly-created frame has two bottom portal columns. Score them in
	// the same x/z order and with the same strict tie-break as tp_find().
	w_coord_t x1 = x, z1 = z;
	w_coord_t x2 = x + ax, z2 = z + az;
	if(x2 < x1 || (x2 == x1 && z2 < z1)) {
		w_coord_t tx = x1, tz = z1;
		x1 = x2;
		z1 = z2;
		x2 = tx;
		z2 = tz;
	}
	double dy = (double)y + 0.5 - job->pose.y;
	double dx1 = (double)x1 + 0.5 - job->pose.x;
	double dz1 = (double)z1 + 0.5 - job->pose.z;
	double dx2 = (double)x2 + 0.5 - job->pose.x;
	double dz2 = (double)z2 + 0.5 - job->pose.z;
	double d1 = dx1 * dx1 + dy * dy + dz1 * dz1;
	double d2 = dx2 * dx2 + dy * dy + dz2 * dz2;
	if(d2 < d1) {
		x1 = x2;
		z1 = z2;
	}
	tp_place_at(job->w, &job->pose, x1, y, z1);
	return true;
}

static struct portal_teleporter_pose pose_of_player(const struct server_player* p) {
	return (struct portal_teleporter_pose) {
		.x = p->x, .y = p->y, .z = p->z, .rx = p->rx, .ry = p->ry,
		.vel_x = p->vel_x, .vel_y = p->vel_y, .vel_z = p->vel_z,
		.old_vel_y = p->old_vel_y};
}

static void pose_to_player(const struct portal_teleporter_pose* p,
						   struct server_player* out) {
	out->x = p->x;
	out->y = p->y;
	out->z = p->z;
	out->rx = p->rx;
	out->ry = p->ry;
	out->vel_x = p->vel_x;
	out->vel_y = p->vel_y;
	out->vel_z = p->vel_z;
	out->old_vel_y = p->old_vel_y;
}

struct portal_teleporter_job*
portal_teleporter_begin(struct server_local* s, const struct java_random* rng) {
	if(!s || !rng)
		return NULL;
	struct portal_teleporter_pose start = pose_of_player(&s->player);
	struct portal_teleporter_job* job
		= portal_teleporter_begin_w(s, &s->worlds[0], &start, rng);
	if(job)
		job->legacy_player = true; /* DONE writes the pose back to s->player */
	return job;
}

const struct portal_teleporter_pose*
portal_teleporter_job_pose(const struct portal_teleporter_job* job) {
	return job ? &job->pose : NULL;
}

unsigned portal_teleporter_job_winner_chunks(
	const struct portal_teleporter_job* job, w_coord_t* xs, w_coord_t* zs,
	unsigned max) {
	if(!job || job->stage != TP_JOB_LOAD_WINNER)
		return 0;
	unsigned n = job->winner_chunk_count < max ? job->winner_chunk_count : max;
	for(unsigned i = 0; i < n; i++) {
		xs[i] = job->winner_chunks_x[i];
		zs[i] = job->winner_chunks_z[i];
	}
	return n;
}

struct portal_teleporter_job*
portal_teleporter_begin_w(struct server_local* s, struct server_world* w,
						  const struct portal_teleporter_pose* start,
						  const struct java_random* rng) {
	if(!s || !w || !start || !rng)
		return NULL;
	struct portal_teleporter_job* job = calloc(1, sizeof(*job));
	if(!job)
		return NULL;
	job->find_ids = mem2_alloc(TP_FIND_IDS_BYTES);
	if(!job->find_ids) {
		free(job);
		return NULL;
	}
	job->w = w;
	job->pose = *start;
	job->legacy_player = false;
	job->stage = TP_JOB_FIND;
	job->rng = *rng;
	job->base_x = floor_d(job->pose.x);
	job->base_y = floor_d(job->pose.y);
	job->base_z = floor_d(job->pose.z);
	w_coord_t find_radius = tp_find_radius();
	tp_job_set_range(job, WCOORD_CHUNK_OFFSET(job->base_x - find_radius),
					 WCOORD_CHUNK_OFFSET(job->base_x + find_radius),
					 WCOORD_CHUNK_OFFSET(job->base_z - find_radius),
					 WCOORD_CHUNK_OFFSET(job->base_z + find_radius));
	tp_job_reset_best(job);
	return job;
}

enum portal_teleporter_result
portal_teleporter_step(struct server_local* s,
					   struct portal_teleporter_job* job) {
	if(!s || !job)
		return PORTAL_TELEPORTER_FAILED;
#ifdef CAVEWII_PORTAL_TELEPORTER_TEST
	if(s_fail_next_portal_step) {
		s_fail_next_portal_step = false;
		return PORTAL_TELEPORTER_FAILED;
	}
#endif
	enum portal_teleporter_result result = PORTAL_TELEPORTER_FAILED;
	switch(job->stage) {
		case TP_JOB_FIND:
			result = tp_job_find_chunks(s, job);
			break;
		case TP_JOB_LOAD_WINNER:
			result = tp_job_load_winner(s, job);
			break;
		case TP_JOB_LOAD_CREATE:
			result = tp_job_load_create(s, job);
			break;
		case TP_JOB_CREATE_PASS1:
			if(!tp_job_create_columns(s, job, false))
				return PORTAL_TELEPORTER_WORKING;
			if(job->best_dist < 0.0) {
				tp_job_set_range(job, job->base_x - 16, job->base_x + 16,
							 job->base_z - 16, job->base_z + 16);
				job->stage = TP_JOB_CREATE_PASS2;
				return PORTAL_TELEPORTER_WORKING;
			}
			job->stage = TP_JOB_BUILD;
			return PORTAL_TELEPORTER_WORKING;
		case TP_JOB_CREATE_PASS2:
			if(!tp_job_create_columns(s, job, true))
				return PORTAL_TELEPORTER_WORKING;
			job->stage = TP_JOB_BUILD;
			return PORTAL_TELEPORTER_WORKING;
		case TP_JOB_BUILD:
			if(!tp_job_build(s, job)) {
				fprintf(stderr, "portal_teleporter: failed writing destination frame\n");
				return PORTAL_TELEPORTER_FAILED;
			}
			result = PORTAL_TELEPORTER_DONE;
			break;
	}
	/* The legacy caller's contract: placement lands on s->player at DONE --
	 * the same moment tp_place_at used to write it. */
	if(result == PORTAL_TELEPORTER_DONE && job->legacy_player)
		pose_to_player(&job->pose, &s->player);
	return result;
}

void portal_teleporter_destroy(struct portal_teleporter_job* job) {
	if(job && job->find_ids)
		mem2_free_sized(job->find_ids, TP_FIND_IDS_BYTES);
	free(job);
}

// Synchronous compatibility wrapper used by the deterministic PC goldens.
// Production uses begin/step so each server iteration remains bounded. Keeping
// this direct transcription as the oracle lets the tests compare cooperative
// placement against the original Beta loop independently.
void portal_teleporter_place(struct server_local* s, struct java_random* rng) {
	struct portal_teleporter_pose p = pose_of_player(&s->player);
	struct server_world* w = &s->worlds[0];
	if(!tp_find(s, w, &p)) {
		tp_create(s, w, &p, rng);
		tp_find(s, w, &p);
	}
	pose_to_player(&p, &s->player);
}
