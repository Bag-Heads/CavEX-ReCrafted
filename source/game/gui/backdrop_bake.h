/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

#ifndef BACKDROP_BAKE_H
#define BACKDROP_BAKE_H

/*
	BG-BAKE-1028 task 2: isolated, one-shot scene bake for the menu backdrop.

	With no world alive, the bake generates the fixed Stillwater-shore scene
	from our own worldgen (terrain + Beta populate) into bake-owned columns,
	hands them to a host-owned scratch client world, lets the host light and
	mesh them with the existing engine, renders ONE frame with the active
	pack, reads the frame back in bands and crops/scales it (nearest
	neighbour) to 1024x576 RGBA. It never touches player saves or any file:
	there is no storage at all, every column is discarded on teardown.

	The module itself is renderer-free (testable headless): everything that
	needs the client world, mesher or GPU goes through struct
	backdrop_bake_host, which main.c implements for the real game and the
	unit test fakes. Work is stepped -- backdrop_bake_step does a bounded
	amount per call so the caller keeps menus/input/heartbeat serviced.

	Writing the PNG, the cache and the pack-screen trigger are NOT here
	(task 3).
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BACKDROP_BAKE_OUT_W 1024
#define BACKDROP_BAKE_OUT_H 576
/* Recipe version: bump when any recipe constant or the pipeline changes the
 * output, so a task-3 cache keyed on it invalidates. */
#define BACKDROP_BAKE_RECIPE_VERSION 1

/* The fixed scene. Values recovered from Astra's design-round-1027 capture
 * (tmp/jobs/design-round-1027/astra/NOTES.md + prepare.py + the PC defaults
 * that capture ran with -- its config overrode none of them):
 *   seed 42; eye (0.5, 68.62, 64.5); engine yaw 270 deg (camera rx), polar
 *   pitch 94 deg (camera ry); world time 6000, clear weather;
 *   FOV 70 (main.c default), 4:3 frame (640x480 viewport);
 *   fog_distance 176 / render_distance 208 (PC far_view_distance 11:
 *   11*16, 11*16+32); lod.high_detail_radius 2 (default); lod.fog_clip 1.13;
 *   crop (0,96)-(640,456) of the 640x480 frame, nearest to 1024x576.
 * Scene extent (locked here, FEASIBILITY blocker 1): every column within
 * Chebyshev view_radius 10 of the eye column (PC MAX_VIEW_DISTANCE, the near
 * ring Astra's frame had resident) whose 16x128x16 box intersects the view
 * frustum, plus their populate halo. Astra's far ring 11 (far-skin strips,
 * >= 168 blocks out, >= 95% fogged) is not generated. Never inherits user
 * settings: the host applies these values and restores its own. */
struct backdrop_bake_recipe {
	int64_t seed;
	float eye[3];
	float yaw_deg, pitch_deg;
	uint32_t world_time;
	float fov_deg;
	int frame_w, frame_h;             /* the reference frame (aspect) */
	float render_distance, fog_distance, fog_clip;
	int lod_near_radius;
	int view_radius;                  /* chunks, Chebyshev from the eye column */
	int crop_x0, crop_y0, crop_x1, crop_y1; /* in reference-frame pixels */
};
const struct backdrop_bake_recipe* backdrop_bake_default_recipe(void);

enum backdrop_bake_state {
	BACKDROP_BAKE_TERRAIN,  /* generating bake-owned terrain columns */
	BACKDROP_BAKE_POPULATE, /* Beta populate over the bake-owned quadrants */
	BACKDROP_BAKE_INSTALL,  /* handing columns to the host scratch world */
	BACKDROP_BAKE_SETTLE,   /* host lights + meshes until complete */
	BACKDROP_BAKE_CAPTURE,  /* one frame rendered, read back, scaled */
	BACKDROP_BAKE_DONE,
	BACKDROP_BAKE_FAILED,
	BACKDROP_BAKE_CANCELLED,
};

enum backdrop_bake_error {
	BACKDROP_BAKE_OK = 0,
	BACKDROP_BAKE_ERR_WORLD_ALIVE, /* a local/remote world is alive */
	BACKDROP_BAKE_ERR_ALLOC,       /* bake-owned allocation failed */
	BACKDROP_BAKE_ERR_GENERATE,    /* terrain generation failed */
	BACKDROP_BAKE_ERR_SCENE,       /* host scratch world could not start */
	BACKDROP_BAKE_ERR_INSTALL,     /* host refused a column */
	BACKDROP_BAKE_ERR_SETTLE,      /* host reported a light/mesh failure */
	BACKDROP_BAKE_ERR_NOT_READY,   /* scene never became complete in time */
	BACKDROP_BAKE_ERR_CAPTURE,     /* readback failed or was partial */
	BACKDROP_BAKE_ERR_CANCELLED,
};

enum backdrop_bake_settle {
	BACKDROP_BAKE_SETTLE_PENDING,
	BACKDROP_BAKE_SETTLE_READY,
	BACKDROP_BAKE_SETTLE_FAILED,
};

/* Banded readback sink: rows [y0, y0+rows) of a width x height RGBA8 frame,
 * top-down, tightly packed. Return false to cancel the readback. */
typedef bool (*backdrop_bake_rows_fn)(void* user, const uint8_t* rgba,
	size_t y0, size_t rows, size_t width, size_t height);

/* The renderer side. Every callback is optional only where noted. The host
 * owns the scratch client world: scene_begin creates it, scene_end (called
 * exactly once for every successful scene_begin, on success, failure,
 * cancel and destroy) releases every world/GPU/mesher reference. */
struct backdrop_bake_host {
	void* user;
	/* true while any local or remote world is alive: the bake refuses. */
	bool (*world_alive)(void* user);
	bool (*scene_begin)(void* user, const struct backdrop_bake_recipe* r);
	/* Install one complete column (server_chunk array layout: ids 32 KiB,
	 * metadata/sky/torch nibbles, 16x16 heightmap, 256-byte tints). The
	 * host copies; the arrays stay bake-owned. `visible`: the column is in
	 * the frustum set the readiness gate must cover. */
	bool (*install_column)(void* user, int cx, int cz, const uint8_t* ids,
		const uint8_t* metadata, const uint8_t* sky, const uint8_t* torch,
		const uint8_t* heightmap, const uint8_t* grass_i,
		const uint8_t* grass_j, bool visible);
	/* All columns installed: light + mesh within budget_us. READY only when
	 * every visible column is installed, lighting has reached its fixed
	 * point and every chunk the frame will draw holds its current mesh with
	 * no job outstanding. Never READY on a partial scene. */
	enum backdrop_bake_settle (*settle_step)(void* user, uint32_t budget_us);
	/* Render the recipe frame (no HUD/hand/entities/particles) and read it
	 * back through `sink`. False on any failure or a sink cancel. */
	bool (*render_capture)(void* user, backdrop_bake_rows_fn sink,
		void* sink_user);
	void (*scene_end)(void* user);
};

/* Bake-side accounting for the memory ledger (FEASIBILITY blocker 1). Bytes
 * the BAKE owns (column payload, tints, column table, output); the host
 * reports its own world/mesh/heap figures through its telemetry line. */
struct backdrop_bake_report {
	enum backdrop_bake_state state;
	enum backdrop_bake_error error;
	unsigned columns_total, columns_visible, columns_generated;
	unsigned quads_total, quads_done, columns_installed;
	unsigned settle_steps;
	size_t owned_bytes, owned_peak_bytes;
	uint32_t phase_us[5]; /* terrain, populate, install, settle, capture */
	uint32_t step_max_us;  /* longest single backdrop_bake_step */
};

struct backdrop_bake;

/* Validates the recipe, refuses while a world is alive, and allocates the
 * column table. NULL with *err set on refusal/allocation failure (nothing
 * retained). The host struct is copied. */
struct backdrop_bake* backdrop_bake_create(const struct backdrop_bake_host* host,
	const struct backdrop_bake_recipe* recipe, enum backdrop_bake_error* err);
/* One bounded slice of work (budget_us is a soft time budget; at least one
 * unit of work is done per call). Returns the state after the slice. */
enum backdrop_bake_state backdrop_bake_step(struct backdrop_bake* b,
	uint32_t budget_us);
/* Request cancellation: the next step (or destroy) releases everything and
 * the state becomes CANCELLED. Safe in any state. */
void backdrop_bake_cancel(struct backdrop_bake* b);
enum backdrop_bake_state backdrop_bake_state(const struct backdrop_bake* b);
enum backdrop_bake_error backdrop_bake_error(const struct backdrop_bake* b);
void backdrop_bake_get_report(const struct backdrop_bake* b,
	struct backdrop_bake_report* out);
/* DONE only: hands over the 1024x576 RGBA8 pixels (alpha 255). The caller
 * frees them with backdrop_bake_pixels_free. Returns NULL otherwise. */
uint8_t* backdrop_bake_take_pixels(struct backdrop_bake* b);
void backdrop_bake_pixels_free(uint8_t* pixels);
/* Releases every bake-owned buffer and ends the host scene if it is still
 * open, in any state. NULL-safe. */
void backdrop_bake_destroy(struct backdrop_bake* b);

/* The process-wide bake slot. main.c registers its host once at boot and
 * steps the active bake once per frame (backdrop_bake_service); the task-3
 * trigger requests a bake, polls backdrop_bake_active()'s state, takes the
 * pixels and releases the slot. At most one bake exists at a time. */
void backdrop_bake_register_host(const struct backdrop_bake_host* host);
/* Creates the active bake from the registered host + default recipe. False
 * (with *err) if one is already active, no host is registered, or create
 * refuses. */
bool backdrop_bake_request(enum backdrop_bake_error* err);
struct backdrop_bake* backdrop_bake_active(void);
/* One step of the active bake, if any and not yet terminal. */
void backdrop_bake_service(uint32_t budget_us);
/* Destroys the active bake (any state), releasing everything. */
void backdrop_bake_release_active(void);

/* BG-BAKE-1028/3 seam: the pack screen's backdrop job (screen_texturepacks.c)
 * reaches the bake and the generated-backdrop cache (texture.c) ONLY through
 * these hooks, which main.c registers. Unregistered (headless screen rigs)
 * the job is inert. All operate on the CURRENTLY CONFIGURED texture pack. */
struct backdrop_job_hooks {
	/* texture.c enum tex_backdrop_state of the current pack, as int. */
	int (*state)(void);
	bool (*bake_start)(void);              /* backdrop_bake_request */
	enum backdrop_bake_state (*bake_poll)(void);
	enum backdrop_bake_error (*bake_error)(void);
	uint8_t* (*bake_take)(void);           /* DONE: hand over the pixels */
	void (*bake_release)(void);            /* destroy the active bake */
	void (*pixels_free)(uint8_t* px);
	/* texture.c tex_backdrop_publish result (0 = published) and its text. */
	int (*publish)(const uint8_t* rgba);
	const char* (*publish_reason)(int result);
	void (*reload)(void);                  /* fence + reload the slot */
	/* Task-3 main adapter supplies actual upload and scratch/mesher retirement
	 * witnesses. Missing witnesses keep the explicit readiness gate LOADING. */
	bool (*uploaded)(void);
	bool (*retired)(void);
	/* audio_registry_ready_state (0 LOADING, 1 READY, 2 FAILED) and its reason.
	 * Main polls async completion before servicing this job. */
	int (*sounds_state)(void);
	const char* (*sounds_reason)(void);
	bool (*sounds_retry)(void);
};

enum backdrop_job_readiness { BACKDROP_JOB_LOADING, BACKDROP_JOB_READY,
	BACKDROP_JOB_FAILED };
void screen_texturepacks_set_backdrop_hooks(const struct backdrop_job_hooks* h);
/* True context means a front-end load (boot, title or Apply), with actual
 * world-ownership exclusions still enforced by the bake host. */
void screen_texturepacks_backdrop_service(bool frontend_idle);
enum backdrop_job_readiness screen_texturepacks_backdrop_ready_state(void);
const char* screen_texturepacks_backdrop_failure_reason(void);
void screen_texturepacks_backdrop_retry(void);
void screen_texturepacks_backdrop_cancel(void);

/* Admission (review-2 #1): may a bake run now? Built from actual world /
 * provider OWNERSHIP and completed teardown -- never from the client's
 * world_loaded / world_switch_pending flags, which latch true across a
 * quit-to-title (switch_pending clears only when the NEXT load completes;
 * main.c's multiplayer busy gate documents the same trap). */
struct backdrop_bake_world_facts {
	bool remote_provider;     /* a remote (multiplayer) provider is active */
	bool server_owns_world;   /* local server holds a world (player.has_pos) */
	size_t client_sections;   /* resident sections in the client world */
	bool at_title;            /* the title screen is current */
	bool loading_frontend;    /* boot credits / opaque Apply, no world admission */
};
bool backdrop_bake_world_alive(const struct backdrop_bake_world_facts* f);

/* Pure helper (exposed for the test): nearest-neighbour source index of
 * output index `o` of `out_len` over a crop of `crop_len` source pixels
 * starting at `crop0` -- PIL Image.NEAREST's floor((o + 0.5) * scale). */
size_t backdrop_bake_nearest_src(size_t o, size_t out_len, size_t crop0,
	size_t crop_len);

/* Steps between the last install and a forced NOT_READY failure. */
#define BACKDROP_BAKE_SETTLE_STEP_LIMIT 36000u

/* Runtime-armed test seam (no compile profile): the n-th bake-owned
 * allocation from now fails (0 = off). Terrain generation counts as one
 * allocation per column. Never armed by the engine. */
void backdrop_bake_test_fail_alloc_after(unsigned n);

#endif
