/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* ui-build-1026/T09: "Back up this world?" before a Minecraft Beta world is
 * loaded (and so converted by server_local's save-version chain). Approved
 * design: tmp/jobs/design-round-1026b/convert (Sol's dialog, Cancel focused;
 * Beta LoadingScreenRenderer bar while copying; Cancel-focused error).
 *
 * Three states:
 *   ASK      Back up & convert / Convert without backup / Cancel (Cancel
 *            first). Nothing is touched on disk until Back up is chosen, so
 *            Cancel never leaves an empty "(backup)" reservation.
 *   COPYING  one world_copy_step per frame (world_copy bounds the entries and
 *            bytes per step), real progress = copied_bytes / bytes. No Cancel
 *            (Beta progress screens have none). Back/Home are ignored.
 *   ERROR    "Couldn't back up world": Convert without backup / Cancel
 *            (Cancel first). A failed backup NEVER loads or converts on its
 *            own.
 *   CLEANING "Cleaning up..." after a failure: world_copy removes its temp
 *            tree in the same one-step-per-frame budget; only when it reports
 *            FAILED/CANCELLED is the object destroyed (nothing left to walk).
 *            No answers, not even Back, until then. A failed cleanup shows a
 *            truthful error with Cancel only.
 * ui-build-1026/T10: after a published backup (or Convert without backup)
 * the screen runs the T09a/T09b convert-only step (client_session_convert_*,
 * the unchanged migration chain on the server thread, no world entered):
 *   CONVERTING   "Converting world". The only measured fraction is the
 *                chain's read-only chunk survey (server_local_import_
 *                progress); every other stage has no denominator and shows
 *                an indeterminate bar, never a percent. No answers.
 *   DONE         SUCCEEDED: "World converted", Play world / Back to worlds.
 *   SAVE_PENDING its own receipt: the chain left a normalization for the
 *                first checkpoint, so it is NOT Done ("Finishes when you
 *                play"); Play world / Back to worlds.
 *   CONVERT_ERROR every other receipt (or a refused start): an honest
 *                error with Back to worlds only. may_have_changed=true is
 *                never worded "not changed".
 * The original backup folder is named in this text only when this flow
 * published it AND it still exists. Play runs the opener's unchanged load
 * of the ORIGINAL path exactly once; Back returns to the list (which
 * re-seats the remembered row). Nothing plays automatically. */

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../network/server_interface.h"
#include "../../network/server_local.h"
#include "../../network/world_copy.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/texture.h"
#include "../game_state.h"
#include "frontend_presentation.h"
#include "import_convert_notice.h"
#include "screen.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

/* Appended states keep the T09 ordinals the tests pin. */
enum cb_state {
	CB_ASK,
	CB_VOLUME,
	CB_COPYING,
	CB_ERROR,
	CB_CLEANING,
	CB_CONVERTING,
	CB_DONE,
	CB_SAVE_PENDING,
	CB_CONVERT_ERROR,
};

/* ui-build-1026/T10 text (approved convert-done; the other result screens
 * reuse its language). */
#define CB_CONVERTING_TITLE "Converting world"
#define CB_DONE_TITLE "World converted"
#define CB_SAVE_PENDING_TITLE "Finishes when you play"
#define CB_CONVERT_ERROR_TITLE "Couldn't convert world"
#define CB_CONVERT_STOPPED_TITLE "Conversion stopped"
#define CB_BUTTON_PLAY "Play world"
#define CB_BUTTON_BACK "Back to worlds"


/* Why the backup failed, as the error screen words it. */
enum cb_fail {
	CB_FAIL_NONE,
	CB_FAIL_VOLUME,	   /* free space / cluster size unreadable */
	CB_FAIL_NO_MEMORY, /* world_copy_survey_begin returned NULL */
	CB_FAIL_COPY,	   /* world_copy reported an error (cb_copy_error) */
	CB_FAIL_CLEANUP,   /* ... and then could not remove its own temp tree */
};

static enum cb_state cb_state = CB_ASK;
static enum cb_fail cb_fail = CB_FAIL_NONE;
static enum world_copy_error cb_copy_error = WORLD_COPY_OK;
static char cb_path[512];
static char cb_name[160];
static bool (*cb_proceed)(const char* path);
static struct world_copy* cb_copy;
static uint64_t cb_free_bytes;
static uint32_t cb_cluster_bytes;
static uint64_t cb_done_bytes, cb_total_bytes;
static bool cb_surveying;
/* ui-build-1026/T16a: the volume facts this flow used, kept after the copy
 * object is released (read by screen_convert_backup_volume). */
static uint64_t cb_required_bytes;
static bool cb_volume_known;
/* T16a rig-only seams. Armed ONLY by main.c's hw_run world_copy_convert
 * block (screen_convert_backup_rig_arm_fault / _rig_interrupt); zero/false in
 * every other run, where each use below is a no-op branch. */
static int cb_rig_fault; /* 0 none, 1 low space, 2 write error */
static bool cb_rig_parked; /* interrupt: copy abandoned, no SD work */

/* ui-build-1026/T10: the convert-only receipt and the backup this flow
 * published (if any). */
static bool cb_backup_published;  /* world_copy reported DONE this flow */
static bool cb_backup_named;	  /* ... and the folder exists at the result */
static char cb_backup_path[WORLD_COPY_PATH_MAX];
static char cb_backup_folder[WORLD_COPY_PATH_MAX];
static bool cb_convert_admitted;
static enum svin_convert_outcome cb_convert_outcome = SVIN_CONVERT_PENDING;
static bool cb_convert_changed;
static unsigned cb_anim_frames; /* indeterminate bar motion only */
/* hw1028b: world_copy_start was called on this backup (world_copy's own
 * `started`), and -- once the copy reported FAILED/CANCELLED -- whether its
 * cleanup provably ran to the end: started (so copy_fail armed cleanup), no
 * cleanup_error (the only clean exit from CLEANING), and the destination it
 * named absent on disk (one stat, at the result only). A failure whose whole
 * cleanup fits inside the failing world_copy_step (a few FAT entries, well
 * under WORLD_COPY_STEP_MAX) never shows CB_CLEANING for a frame; this is
 * the evidence that it still cleaned up. */
static bool cb_copy_started;
static bool cb_cleanup_done;

static int cb_selected;
static enum gutil_menu_origin cb_origin;
static struct gutil_menu_focus cb_focus;
static bool cb_pointer_available;
static float cb_pointer_x, cb_pointer_y, cb_pointer_angle;

/* ---- the free-space query -------------------------------------------------
 * world_copy needs the target volume's cluster size (survey) and free bytes
 * (start). Both come from ONE query at the moment the player chooses Back up:
 * libfat's statvfs may scan the FAT, so it is made once, on its own frame
 * (CB_VOLUME, drawn as "Checking the SD card"). The reservation the survey
 * then makes is counted in world_copy's required_bytes. */
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static int cb_volume_override; /* 0 real, 1 forced values, -1 forced failure */
static uint64_t cb_override_free;
static uint32_t cb_override_cluster;
#endif

static bool cb_volume_query(const char* path, uint64_t* free_bytes,
							uint32_t* cluster_bytes) {
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(cb_volume_override) {
		*free_bytes = cb_override_free;
		*cluster_bytes = cb_override_cluster;
		return cb_volume_override > 0;
	}
#endif
#ifdef _WIN32
	char full[MAX_PATH], root[MAX_PATH];
	DWORD n = GetFullPathNameA(path, sizeof(full), full, NULL);
	if(!n || n >= sizeof(full) || !GetVolumePathNameA(full, root, sizeof(root)))
		return false;
	DWORD per_cluster, per_sector, free_clusters, total_clusters;
	ULARGE_INTEGER avail;
	if(!GetDiskFreeSpaceA(root, &per_cluster, &per_sector, &free_clusters,
						  &total_clusters)
	   || !GetDiskFreeSpaceExA(full, &avail, NULL, NULL))
		return false;
	uint64_t cluster = (uint64_t)per_cluster * per_sector;
	*free_bytes = avail.QuadPart;
#else
	struct statvfs v;
	if(statvfs(path, &v) != 0)
		return false;
	uint64_t cluster = v.f_frsize ? (uint64_t)v.f_frsize : (uint64_t)v.f_bsize;
	if(cluster && (uint64_t)v.f_bavail > UINT64_MAX / cluster)
		return false;
	*free_bytes = (uint64_t)v.f_bavail * cluster;
#endif
	if(cluster < 512 || cluster > UINT32_MAX || (cluster & (cluster - 1)))
		return false;
	*cluster_bytes = (uint32_t)cluster;
	return true;
}

/* ---- buttons --------------------------------------------------------------- */
enum cb_action {
	CB_ACT_BACKUP,
	CB_ACT_NO_BACKUP,
	CB_ACT_CANCEL,
	CB_ACT_PLAY,
	CB_ACT_BACK,
};

static bool cb_is_result(void) {
	return cb_state == CB_DONE || cb_state == CB_SAVE_PENDING
		|| cb_state == CB_CONVERT_ERROR;
}

static int cb_button_count(void) {
	switch(cb_state) {
		case CB_ASK: return 3;
		/* After a failed cleanup only Cancel is offered: nothing more is
		 * done to the SD card from this screen. */
		case CB_ERROR: return cb_fail == CB_FAIL_CLEANUP ? 1 : 2;
		case CB_DONE:
		case CB_SAVE_PENDING: return 2; /* Play world / Back to worlds */
		case CB_CONVERT_ERROR: return 1; /* Back to worlds only */
		case CB_VOLUME:
		case CB_COPYING:
		case CB_CLEANING:
		case CB_CONVERTING: return 0;
	}
	return 0;
}

static enum cb_action cb_button_action(int k) {
	if(cb_state == CB_ASK)
		return (enum cb_action)k;
	if(cb_state == CB_DONE || cb_state == CB_SAVE_PENDING)
		return k == 0 ? CB_ACT_PLAY : CB_ACT_BACK;
	if(cb_state == CB_CONVERT_ERROR)
		return CB_ACT_BACK;
	return k == 0 && cb_button_count() == 2 ? CB_ACT_NO_BACKUP : CB_ACT_CANCEL;
}

static const char* cb_button_label(int k) {
	switch(cb_button_action(k)) {
		case CB_ACT_BACKUP: return CONVERT_BACKUP_BUTTON_BACKUP;
		case CB_ACT_NO_BACKUP: return CONVERT_BACKUP_BUTTON_NO_BACKUP;
		case CB_ACT_PLAY: return CB_BUTTON_PLAY;
		case CB_ACT_BACK: return CB_BUTTON_BACK;
		case CB_ACT_CANCEL: break;
	}
	return CONVERT_BACKUP_BUTTON_CANCEL;
}

/* Cancel / Back to worlds: the answer that starts nothing (last button). */
static int cb_safe_button(void) { return cb_button_count() - 1; }

/* The approved focus: Cancel on the ask and the backup error; Play world on
 * the done screen (convert-done "focus": 0); Back on a conversion error. */
static int cb_default_button(void) {
	return cb_state == CB_DONE || cb_state == CB_SAVE_PENDING
		? 0 : cb_safe_button();
}

/* The approved 640x480 composition, scaled by height and centred. */
struct cb_canvas {
	int ox;
	float u;
};
static struct cb_canvas cb_canvas_of(int width, int height) {
	struct cb_canvas c;
	c.u = height > 0 ? (float)height / 480.0F : 1.0F;
	c.ox = (width - (int)(640.0F * c.u + 0.5F)) / 2;
	if(c.ox < 0)
		c.ox = 0;
	return c;
}
static int cb_d(const struct cb_canvas* c, int v) {
	return (int)((float)v * c->u + 0.5F);
}

#define CB_TITLE_Y 64
#define CB_TITLE_FONT 24
#define CB_LINES_Y 116
#define CB_LINE_PITCH 28
#define CB_LINE_FONT 16
#define CB_BUTTONS_Y 236
#define CB_BUTTON_W 400
#define CB_BUTTON_H 36
#define CB_BUTTON_PITCH 48
#define CB_PROGRESS_Y 222
#define CB_PROGRESS_LABEL_GAP 26
#define CB_FOOTER_Y 432
#define CB_FOOTER_H 36
#define CB_FOOTER_GAP 24

static struct fep_rect cb_button_rect(int width, int height, int k) {
	struct cb_canvas c = cb_canvas_of(width, height);
	int w = cb_d(&c, CB_BUTTON_W);
	return fep_rect((width - w) / 2,
					cb_d(&c, CB_BUTTONS_Y + k * CB_BUTTON_PITCH), w,
					cb_d(&c, CB_BUTTON_H));
}

static int cb_pointer_button(void) {
	if(!cb_pointer_available)
		return GUTIL_MENU_TARGET_NONE;
	for(int k = 0; k < cb_button_count(); k++)
		if(fep_rect_hit(cb_button_rect(gfx_width(), gfx_height(), k),
						cb_pointer_x, cb_pointer_y))
			return k;
	return GUTIL_MENU_TARGET_NONE;
}

static void cb_enter_buttons(enum cb_state state) {
	cb_state = state;
	cb_selected = cb_default_button();
	cb_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	cb_focus = (struct gutil_menu_focus) {0};
}

/* ---- the copy -------------------------------------------------------------- */
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static unsigned cb_test_steps_this_tick;
static bool cb_test_temp_left_at_destroy;
static enum world_copy_fault cb_test_fault = WORLD_COPY_FAULT_NONE;
static size_t cb_test_fault_after;
#endif

/* Release a copy whose cleanup already ran to its end in bounded per-frame
 * steps (CB_CLEANING). world_copy_destroy always re-arms cleanup, but with the
 * temp tree already removed that re-run is O(1): at most the rmdir of an
 * EMPTY reservation/temp of a copy that failed before start (copied_tree
 * false) -- never a tree walk and never copy work. */
static void cb_release_copy(void) {
	if(!cb_copy)
		return;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	{
		const struct world_copy_result* r = world_copy_get_result(cb_copy);
		struct stat st;
		cb_test_temp_left_at_destroy
			= r->temporary[0] && stat(r->temporary, &st) == 0;
	}
#endif
	enum world_copy_error e = world_copy_destroy(cb_copy);
	cb_copy = NULL;
	if(e != WORLD_COPY_OK)
		fprintf(stderr, "[convert-backup] release: %s\n",
				world_copy_error_name(e));
}

/* A failure with no copy object (volume query / allocation). */
static void cb_fail_with(enum cb_fail fail) {
	cb_fail = fail;
	cb_copy_error = WORLD_COPY_OK;
	cb_enter_buttons(CB_ERROR);
}

/* The opener's unchanged load of the ORIGINAL world path, run only by the
 * player's Play world. cb_proceed is cleared first, so it runs at most once
 * per setup. When nothing could be sent or parked the player is back on the
 * list (which re-seats the remembered selection). */
static void cb_run_proceed(void) {
	bool (*proceed)(const char*) = cb_proceed;
	char path[sizeof(cb_path)];
	snprintf(path, sizeof(path), "%s", cb_path);
	cb_proceed = NULL;
	if(!proceed || !path[0] || !proceed(path))
		screen_set(&screen_select_world);
}

static void cb_begin_backup(void) {
	cb_state = CB_VOLUME;
	cb_done_bytes = cb_total_bytes = 0;
	cb_surveying = true;
	cb_copy_started = cb_cleanup_done = false;
}

/* ---- the convert-only step (ui-build-1026/T10) ---------------------------- */

/* Name the backup on the result screens only if this flow published it AND
 * the folder is still there now (one stat at the result, never per frame). */
static void cb_settle_backup_name(void) {
	struct stat st;
	cb_backup_named = cb_backup_published && cb_backup_path[0]
		&& stat(cb_backup_path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void cb_begin_convert(void) {
	cb_convert_outcome = SVIN_CONVERT_PENDING;
	cb_convert_changed = false;
	cb_anim_frames = 0;
	cb_convert_admitted
		= client_session_convert_start(&gstate.players[0], cb_path);
	if(!cb_convert_admitted) {
		fprintf(stderr, "[convert-backup] convert-only not admitted: %s\n",
				cb_path);
		cb_settle_backup_name();
		cb_enter_buttons(CB_CONVERT_ERROR);
		return;
	}
	fprintf(stderr, "[convert-backup] convert-only started: %s\n", cb_path);
	cb_state = CB_CONVERTING;
}

/* One poll per frame. The receipt is settled exactly once (client_session
 * consumes one matching completion); the screen stays here, with no answers,
 * until it is. Every outcome is named: SAVE_PENDING is never folded into
 * Done or an error. */
static void cb_convert_tick(void) {
	cb_anim_frames++;
	struct client_session* session = &gstate.players[0];
	if(!client_session_convert_poll(session))
		return;
	cb_convert_outcome = session->convert.outcome;
	cb_convert_changed = session->convert.may_have_changed;
	fprintf(stderr, "[convert-backup] convert-only receipt: outcome=%u "
			"may_have_changed=%u\n",
			(unsigned)cb_convert_outcome, (unsigned)cb_convert_changed);
	cb_settle_backup_name();
	switch(cb_convert_outcome) {
		case SVIN_CONVERT_SUCCEEDED: cb_enter_buttons(CB_DONE); return;
		case SVIN_CONVERT_SAVE_PENDING: cb_enter_buttons(CB_SAVE_PENDING); return;
		case SVIN_CONVERT_PENDING: /* a settled receipt is never PENDING */
		case SVIN_CONVERT_FAILED:
		case SVIN_CONVERT_CANCELLED:
		case SVIN_CONVERT_WORLD_ACTIVE:
		case SVIN_CONVERT_JOURNAL_REFUSED:
		case SVIN_CONVERT_OPEN_FAILED:
		case SVIN_CONVERT_PROVIDER_STOPPED:
			cb_enter_buttons(CB_CONVERT_ERROR);
			return;
	}
	cb_enter_buttons(CB_CONVERT_ERROR); /* an unknown receipt is not Done */
}

/* The convert stage label and its measured percent, or -1 when the running
 * stage has no denominator (the bar is then indeterminate). Only the chain's
 * read-only chunk survey reports a fraction (server_local_import_progress). */
static int cb_convert_stage(const char** label) {
	uint32_t pct = UINT32_MAX;
	bool running = server_local_import_progress(&pct);
	if(running && pct <= 100) {
		*label = "Checking chunks";
		return (int)pct;
	}
	*label = running ? "Converting chunks" : "Converting world files";
	return -1;
}

/* One frame of copy work: at most ONE world_copy_step (bounded entries and
 * bytes, world_copy.h) or one start. A failed copy cleans its owned temp tree
 * in the same bounded steps (world_copy enters CLEANING itself once started);
 * this screen shows that as CB_CLEANING and keeps stepping until cleanup
 * reports FAILED/CANCELLED, so destroy never has a tree left to walk. */
static void cb_copy_tick(void) {
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	cb_test_steps_this_tick = 0;
#endif
	if(cb_state == CB_VOLUME) {
		if(!cb_volume_query(cb_path, &cb_free_bytes, &cb_cluster_bytes)) {
			fprintf(stderr, "[convert-backup] free-space query failed: %s\n",
					cb_path);
			cb_fail_with(CB_FAIL_VOLUME);
			return;
		}
		cb_volume_known = true;
		if(cb_rig_fault == 1) {
			/* T16a rig: below any world's required_bytes */
			fprintf(stderr, "[convert-backup] RIG low-space: free %" PRIu64
					" -> 4096\n", cb_free_bytes);
			cb_free_bytes = 4096;
		}
		cb_copy = world_copy_survey_begin(cb_path, cb_cluster_bytes);
		if(!cb_copy) {
			cb_fail_with(CB_FAIL_NO_MEMORY);
			return;
		}
		if(cb_rig_fault == 2) /* T16a rig: fail the second write */
			world_copy_test_fail_after(cb_copy, WORLD_COPY_FAULT_WRITE, 1);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
		if(cb_test_fault != WORLD_COPY_FAULT_NONE)
			world_copy_test_fail_after(cb_copy, cb_test_fault,
									   cb_test_fault_after);
#endif
		cb_state = CB_COPYING;
		return;
	}
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	cb_test_steps_this_tick++;
#endif
	enum world_copy_state st = world_copy_step(cb_copy, WORLD_COPY_STEP_MAX);
	if(st == WORLD_COPY_READY) {
		cb_surveying = false;
		cb_copy_started = true;
		st = world_copy_start(cb_copy, cb_free_bytes);
	}
	const struct world_copy_result* r = world_copy_get_result(cb_copy);
	cb_required_bytes = r->required_bytes;
	cb_done_bytes = r->copied_bytes;
	cb_total_bytes = r->bytes;
	if(st == WORLD_COPY_DONE) {
		fprintf(stderr,
				"[convert-backup] backup published: %s (%" PRIu64 " files, %"
				PRIu64 " bytes)\n",
				r->destination, r->files, r->bytes);
		snprintf(cb_backup_path, sizeof(cb_backup_path), "%s", r->destination);
		const char* slash = strrchr(cb_backup_path, '/');
		snprintf(cb_backup_folder, sizeof(cb_backup_folder), "%s",
				 slash ? slash + 1 : cb_backup_path);
		cb_backup_published = true;
		cb_release_copy();
		cb_begin_convert();
		return;
	}
	if(st == WORLD_COPY_CLEANING) {
		if(cb_state != CB_CLEANING) {
			fprintf(stderr,
					"[convert-backup] backup failed: %s at '%s' (errno %d); "
					"cleaning up\n",
					world_copy_error_name(r->error), r->error_path,
					r->system_error);
			cb_state = CB_CLEANING;
		}
		return;
	}
	if(st == WORLD_COPY_FAILED || st == WORLD_COPY_CANCELLED) {
		cb_fail = CB_FAIL_COPY;
		cb_copy_error = r->error;
		if(cb_copy_started && !r->cleanup_error && r->destination[0]) {
			struct stat dst;
			cb_cleanup_done = stat(r->destination, &dst) != 0 && errno == ENOENT;
		}
		fprintf(stderr, "[convert-backup] cleanup %s (started=%d "
				"cleanup_error=%d shown=%d)\n",
				cb_cleanup_done ? "verified" : "NOT verified",
				(int)cb_copy_started, r->cleanup_error,
				(int)(cb_state == CB_CLEANING));
		if(r->cleanup_error) {
			/* Leave the journal/tree for recovery without retrying disk work. */
			fprintf(stderr,
					"[convert-backup] cleanup failed (errno %d) at '%s'; "
					"leftover kept for recovery, copy object released\n",
					r->cleanup_error, r->error_path);
			world_copy_release(cb_copy);
			cb_copy = NULL;
			cb_fail = CB_FAIL_CLEANUP;
		} else {
			fprintf(stderr, "[convert-backup] backup failed: %s at '%s' "
					"(errno %d)\n",
					world_copy_error_name(r->error), r->error_path,
					r->system_error);
			cb_release_copy();
		}
		cb_enter_buttons(CB_ERROR);
	}
}

/* Percent of world bytes copied, or -1 while the survey has no total yet. */
static int cb_progress_percent(void) {
	if(cb_state != CB_COPYING || cb_surveying)
		return -1;
	if(!cb_total_bytes)
		return 100; /* an empty world has nothing left to copy */
	uint64_t done = cb_done_bytes > cb_total_bytes ? cb_total_bytes
												   : cb_done_bytes;
	return (int)(done * 100 / cb_total_bytes);
}

/* ---- text -------------------------------------------------------------------- */
static void cb_title_and_lines(const char** title, char lines[3][192],
							   int* count) {
	switch(cb_state) {
		case CB_ASK:
			*title = CONVERT_BACKUP_TITLE;
			snprintf(lines[0], 192, "\"%s\" needs conversion for CavEX.",
					 cb_name);
			snprintf(lines[1], 192, "A backup keeps the original for PC.");
			snprintf(lines[2], 192, "Saves a copy as \"%s (backup)\".", cb_name);
			*count = 3;
			return;
		case CB_CLEANING:
			*title = CONVERT_BACKUP_CLEANING_TITLE;
			snprintf(lines[0], 192, "The backup couldn't be finished.");
			snprintf(lines[1], 192,
					 "Don't turn off the Wii or remove the SD card.");
			*count = 2;
			return;
		case CB_VOLUME:
		case CB_COPYING:
			*title = CONVERT_BACKUP_COPYING_TITLE;
			snprintf(lines[0], 192, "Saving a copy as \"%s (backup)\".",
					 cb_name);
			snprintf(lines[1], 192,
					 "Don't turn off the Wii or remove the SD card.");
			*count = 2;
			return;
		case CB_CONVERTING:
			*title = CB_CONVERTING_TITLE;
			if(cb_backup_published)
				snprintf(lines[0], 192, "Backup saved as \"%.150s\".",
						 cb_backup_folder);
			else
				snprintf(lines[0], 192, "\"%s\" is being converted for CavEX.",
						 cb_name);
			snprintf(lines[1], 192,
					 "Don't turn off the Wii or remove the SD card.");
			*count = 2;
			return;
		case CB_DONE:
			*title = CB_DONE_TITLE;
			snprintf(lines[0], 192, "\"%s\" is ready to play in CavEX.",
					 cb_name);
			if(cb_backup_named) {
				snprintf(lines[1], 192, "Your original is kept as \"%.150s\".",
						 cb_backup_folder);
				snprintf(lines[2], 192, "Copy that folder to play it on PC.");
				*count = 3;
			} else {
				snprintf(lines[1], 192, "No backup of the original was made.");
				*count = 2;
			}
			return;
		case CB_SAVE_PENDING:
			/* The chain converted the world but left one normalization for
			 * the first save, which convert-only deliberately never makes. */
			*title = CB_SAVE_PENDING_TITLE;
			snprintf(lines[0], 192, "\"%s\" is almost ready for CavEX.",
					 cb_name);
			snprintf(lines[1], 192,
					 "The last step is saved when you play the world.");
			if(cb_backup_named) {
				snprintf(lines[2], 192, "Your original is kept as \"%.150s\".",
						 cb_backup_folder);
				*count = 3;
			} else {
				*count = 2;
			}
			return;
		case CB_CONVERT_ERROR: {
			bool stopped = cb_convert_admitted
				&& cb_convert_outcome == SVIN_CONVERT_CANCELLED;
			*title = stopped ? CB_CONVERT_STOPPED_TITLE : CB_CONVERT_ERROR_TITLE;
			const char* why = "The world couldn't be converted.";
			if(!cb_convert_admitted)
				why = "The converter couldn't be started.";
			else
				switch(cb_convert_outcome) {
					case SVIN_CONVERT_CANCELLED:
						why = "The conversion was stopped.";
						break;
					case SVIN_CONVERT_WORLD_ACTIVE:
						why = "Another world is still open.";
						break;
					case SVIN_CONVERT_JOURNAL_REFUSED:
						why = "An unfinished save must be repaired first.";
						break;
					case SVIN_CONVERT_OPEN_FAILED:
						why = "The world's level.dat couldn't be read.";
						break;
					case SVIN_CONVERT_PROVIDER_STOPPED:
						why = "The converter stopped before it started.";
						break;
					case SVIN_CONVERT_PENDING:
					case SVIN_CONVERT_FAILED:
					case SVIN_CONVERT_SUCCEEDED:
					case SVIN_CONVERT_SAVE_PENDING: break;
				}
			snprintf(lines[0], 192, "%s", why);
			/* may_have_changed: the unchanged chain was entered, so disk can
			 * no longer be promised unchanged (this is not a rollback). */
			if(cb_convert_changed && stopped)
				snprintf(lines[1], 192, "The world may already be converted.");
			else if(cb_convert_changed)
				snprintf(lines[1], 192, "\"%s\" may be partly converted.",
						 cb_name);
			else
				snprintf(lines[1], 192, "\"%s\" was not changed.", cb_name);
			if(cb_backup_named)
				snprintf(lines[2], 192, "Your original is kept as \"%.150s\".",
						 cb_backup_folder);
			else
				snprintf(lines[2], 192, "No backup of the original was made.");
			*count = 3;
			return;
		}
		case CB_ERROR: break;
	}
	*title = CONVERT_BACKUP_ERROR_TITLE;
	bool full = cb_fail == CB_FAIL_COPY && cb_copy_error == WORLD_COPY_LOW_SPACE;
	const char* why = "The backup couldn't be written to the SD card.";
	if(full)
		why = "The SD card is full.";
	else if(cb_fail == CB_FAIL_CLEANUP)
		why = "The unfinished backup couldn't be removed.";
	else if(cb_fail == CB_FAIL_VOLUME)
		why = "The SD card's free space couldn't be read.";
	else if(cb_fail == CB_FAIL_NO_MEMORY)
		why = "There wasn't enough memory to start the backup.";
	else if(cb_copy_error == WORLD_COPY_PATH_TOO_LONG)
		why = "The world's folder path is too long to copy.";
	else if(cb_copy_error == WORLD_COPY_UNSUPPORTED_DEPTH
			|| cb_copy_error == WORLD_COPY_UNSUPPORTED_LINK
			|| cb_copy_error == WORLD_COPY_UNSUPPORTED_TYPE)
		why = "The world folder has items CavEX can't copy.";
	else if(cb_copy_error == WORLD_COPY_INVALID_LEVEL)
		why = "The world's level.dat couldn't be read.";
	else if(cb_copy_error == WORLD_COPY_NAME_EXHAUSTED
			|| cb_copy_error == WORLD_COPY_COLLISION)
		why = "No free name was left for the backup folder.";
	snprintf(lines[0], 192, "%s", why);
	snprintf(lines[1], 192, "\"%s\" was not changed.", cb_name);
	snprintf(lines[2], 192, "%s",
			 cb_fail == CB_FAIL_CLEANUP
				 ? "Some leftover backup files may remain on the SD card."
			 : full ? "Free some space, or convert without a backup."
					: "Try again, or convert without a backup.");
	*count = 3;
}

/* ---- screen ---------------------------------------------------------------- */
bool screen_convert_backup_setup(const char* world_path, const char* world_name,
								 bool (*proceed)(const char* path)) {
	assert(world_path && proceed);
	/* T10: never re-arm while a convert-only receipt is still unsettled
	 * (the screen only leaves CONVERTING once it settled, so this is a
	 * guard, not a path). */
	if(cb_copy || cb_state == CB_CONVERTING
	   || gstate.players[0].convert_pending
	   || strlen(world_path) >= sizeof(cb_path))
		return false;
	snprintf(cb_path, sizeof(cb_path), "%s", world_path);
	snprintf(cb_name, sizeof(cb_name), "%s",
			 world_name && world_name[0] ? world_name : "World");
	cb_proceed = proceed;
	cb_fail = CB_FAIL_NONE;
	cb_copy_error = WORLD_COPY_OK;
	cb_done_bytes = cb_total_bytes = 0;
	cb_required_bytes = 0;
	cb_volume_known = false;
	cb_rig_parked = false;
	cb_copy_started = cb_cleanup_done = false;
	cb_backup_published = cb_backup_named = false;
	cb_backup_path[0] = cb_backup_folder[0] = '\0';
	cb_convert_admitted = false;
	cb_convert_outcome = SVIN_CONVERT_PENDING;
	cb_convert_changed = false;
	cb_enter_buttons(CB_ASK);
	return true;
}

static void screen_cb_reset(struct screen* s, int width, int height) {
	(void)s;
	(void)width;
	(void)height;
	assert(cb_proceed); /* setup() must precede screen_set */
	input_pointer_enable(true);
	cb_pointer_available = false;
	/* Entry always re-arms the safe answer (Cancel). */
	cb_enter_buttons(CB_ASK);
}

static void cb_activate(enum cb_action action) {
	switch(action) {
		case CB_ACT_BACKUP: cb_begin_backup(); break;
		case CB_ACT_NO_BACKUP: cb_begin_convert(); break;
		case CB_ACT_PLAY: cb_run_proceed(); break; /* the ONE load */
		case CB_ACT_CANCEL:
		case CB_ACT_BACK:
			cb_proceed = NULL;
			screen_set(&screen_select_world);
			break;
	}
}

static void screen_cb_update(struct screen* s, float dt) {
	(void)s;
	(void)dt;
	if(cb_rig_parked)
		return; /* T16a rig interrupt: frozen, no copy step, no answers */
	cb_pointer_available
		= input_pointer(&cb_pointer_x, &cb_pointer_y, &cb_pointer_angle);
	if(cb_state == CB_VOLUME || cb_state == CB_COPYING
	   || cb_state == CB_CLEANING) {
		cb_copy_tick();
		return; /* no answers (not even Back) while a copy or its cleanup
				 * is open: Back can never skip the cleanup */
	}
	if(cb_state == CB_CONVERTING) {
		cb_convert_tick();
		return; /* no answers until the receipt settles (approved design:
				 * Beta progress screens have no buttons) */
	}

	int count = cb_button_count();
	bool up = input_pressed(IB_GUI_UP), down = input_pressed(IB_GUI_DOWN);
	bool navigating = up || down;
	bool pointer_owner = gutil_menu_pointer_focus(
		&cb_focus, cb_pointer_available, cb_pointer_x, cb_pointer_y, navigating);
	int hovered = cb_pointer_button();
	if(pointer_owner && hovered != GUTIL_MENU_TARGET_NONE) {
		cb_selected = hovered;
		cb_origin = GUTIL_MENU_ORIGIN_POINTER;
	}
	if(navigating) {
		cb_selected = (cb_selected + (down ? 1 : count - 1)) % count;
		cb_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	}

	/* screen_confirm's D1 rule: on the declared safe answer (Cancel) a plain
	 * A answers it; the exception never travels to the other answers, which
	 * both lead to conversion. The T10 result screens convert nothing more:
	 * Play world and Back are ordinary (non-destructive) menu buttons. */
	int safe = cb_safe_button();
	bool result = cb_is_result();
	enum gutil_menu_origin decide_origin
		= cb_selected == safe ? GUTIL_MENU_ORIGIN_DIRECTIONAL : cb_origin;
	bool pressed = input_pressed(IB_GUI_CLICK);
	struct gutil_menu_decision decision
		= gutil_menu_activate_decide((struct gutil_menu_activate_state) {
			.pressed = pressed,
			.navigated = navigating,
			.pointer_valid = cb_pointer_available,
			.selection_origin = decide_origin,
			.hovered = hovered,
			.hovered_enabled = hovered != GUTIL_MENU_TARGET_NONE,
			.selected = cb_selected,
			.selected_enabled = true,
			.selected_destructive = !result && cb_selected != safe,
			.destructive_dialog = !result,
		});
	if(decision.action == GUTIL_MENU_FOCUS_ONLY) {
		cb_selected = decision.selected;
		cb_origin = decision.selection_origin;
	}
	if(decision.action == GUTIL_MENU_ACTIVATE) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		cb_activate(cb_button_action(decision.target));
		return;
	}
	if(pressed)
		return; /* R-C: a consumed press reaches nothing else */
	if(input_pressed(IB_INVENTORY) || input_pressed(IB_HOME))
		cb_activate(result ? CB_ACT_BACK : CB_ACT_CANCEL);
}

static void cb_center_text(int width, int y, const char* text, int font,
						   uint8_t ink) {
	char fitted[192];
	fep_text_fit(fitted, sizeof(fitted), text, font, width - 32);
	gutil_text_rgb((width - gutil_font_width(fitted, font)) / 2, y, fitted, font,
				   true, ink, ink, ink);
}

static void screen_cb_render2D(struct screen* s, int width, int height) {
	(void)s;
	struct cb_canvas c = cb_canvas_of(width, height);
	gutil_bg();

	const char* title;
	char lines[3][192];
	int count;
	cb_title_and_lines(&title, lines, &count);
	cb_center_text(width, cb_d(&c, CB_TITLE_Y), title, cb_d(&c, CB_TITLE_FONT),
				   0xFF);
	for(int k = 0; k < count; k++)
		cb_center_text(width, cb_d(&c, CB_LINES_Y + k * CB_LINE_PITCH), lines[k],
					   cb_d(&c, CB_LINE_FONT), 0xE0);

	if(cb_state == CB_VOLUME || cb_state == CB_COPYING
	   || cb_state == CB_CLEANING) {
		/* Beta LoadingScreenRenderer.setLoadingProgress: a 100x2 0x808080
		 * track, a 0x80FF80 fill, the status text above (x2 GUI scale). */
		int pct = cb_progress_percent();
		char label[64];
		if(cb_state == CB_VOLUME)
			snprintf(label, sizeof(label), "Checking the SD card");
		else if(cb_state == CB_CLEANING)
			snprintf(label, sizeof(label), "Cleaning up...");
		else if(pct < 0)
			snprintf(label, sizeof(label), "Reading world files");
		else
			snprintf(label, sizeof(label), "Copying world files %d%%", pct);
		cb_center_text(width, cb_d(&c, CB_PROGRESS_Y - CB_PROGRESS_LABEL_GAP),
					   label, cb_d(&c, CB_LINE_FONT), 0xFF);
		int bw = cb_d(&c, 200), bh = cb_d(&c, 4);
		int bx = (width - bw) / 2, by = cb_d(&c, CB_PROGRESS_Y);
		fep_fill(fep_rect(bx, by, bw, bh), 0x80, 0x80, 0x80, 0xFF);
		if(pct > 0)
			fep_fill(fep_rect(bx, by, bw * pct / 100, bh), 0x80, 0xFF, 0x80,
					 0xFF);
		return; /* no buttons, footer or pointer while copying */
	}
	if(cb_state == CB_CONVERTING) {
		/* The same Beta bar. Only the chunk survey has a denominator; every
		 * other stage draws an indeterminate block sweeping the track (a
		 * liveness cue, never a completion claim) and no percent. */
		const char* stage;
		int pct = cb_convert_stage(&stage);
		char label[64];
		if(pct >= 0)
			snprintf(label, sizeof(label), "%s %d%%", stage, pct);
		else
			snprintf(label, sizeof(label), "%s...", stage);
		cb_center_text(width, cb_d(&c, CB_PROGRESS_Y - CB_PROGRESS_LABEL_GAP),
					   label, cb_d(&c, CB_LINE_FONT), 0xFF);
		int bw = cb_d(&c, 200), bh = cb_d(&c, 4);
		int bx = (width - bw) / 2, by = cb_d(&c, CB_PROGRESS_Y);
		fep_fill(fep_rect(bx, by, bw, bh), 0x80, 0x80, 0x80, 0xFF);
		if(pct > 0) {
			fep_fill(fep_rect(bx, by, bw * pct / 100, bh), 0x80, 0xFF, 0x80,
					 0xFF);
		} else if(pct < 0) {
			int seg = bw / 5, span = bw - seg;
			int period = 2 * 60; /* frames for one sweep there and back */
			int t = (int)(cb_anim_frames % (unsigned)period);
			int off = t < period / 2 ? t : period - t;
			fep_fill(fep_rect(bx + span * off / (period / 2), by, seg, bh), 0x80,
					 0xFF, 0x80, 0xFF);
		}
		return; /* no buttons, footer or pointer while converting */
	}

	int font = cb_d(&c, CB_LINE_FONT);
	for(int k = 0; k < cb_button_count(); k++) {
		struct fep_rect r = cb_button_rect(width, height, k);
		bool focused = cb_selected == k;
		fep_widget(r, focused);
		if(focused)
			fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
		char text[64];
		fep_text_fit(text, sizeof(text), cb_button_label(k), font,
					 r.w - 2 * GFX_GUI_SCALE);
		uint8_t ink = focused ? 0xFF : 0xF4;
		gutil_text_rgb(r.x + (r.w - gutil_font_width(text, font)) / 2,
					   r.y + (r.h - font) / 2, text, font, true, ink, ink,
					   focused ? 0xA0 : ink);
	}

	static const enum input_button btn[3] = {IB_GUI_UP, IB_GUI_CLICK,
											 IB_INVENTORY};
	/* convert-done footer: [1] Back; the ask and backup error: [1] Cancel. */
	const char* const lbl[3]
		= {"Move", "Select", cb_is_result() ? "Back" : "Cancel"};
	struct input_player_frame prompts;
	input_prompt_frame(0, &prompts);
	/* The centred hint row (controller UI redesign 10-31, A1 helper). */
	struct gutil_hint_item items[3];
	for(int k = 0; k < 3; k++)
		items[k] = (struct gutil_hint_item) {{btn[k]}, 1, lbl[k], k, k == 2};
	struct gutil_hint_row row;
	if(gutil_hint_row_layout(&prompts, 0, 0, width, height, false,
							 GUTIL_HINT_ALIGN_CENTRE, items, 3, &row))
		gutil_hint_row_draw(&row);

	if(cb_pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(cb_pointer_x, cb_pointer_y,
							 glm_rad(cb_pointer_angle), 0, 0, 256, 256,
							 48 * GFX_GUI_SCALE, 48 * GFX_GUI_SCALE);
	}
}

struct screen screen_convert_backup = {
	.reset = screen_cb_reset,
	.update = screen_cb_update,
	.render2D = screen_cb_render2D,
	.render3D = NULL,
	.render_world = false,
};

/* ui-build-1026/T16a: read-only observers for the world_copy_convert proof
 * route (menu_autopilot copy hooks). No side effects. */
int screen_convert_backup_state(void) { return (int)cb_state; }
int screen_convert_backup_focused_action(void) {
	return cb_button_count() ? (int)cb_button_action(cb_selected) : -1;
}
int screen_convert_backup_fail(void) { return (int)cb_fail; }
int screen_convert_backup_copy_error(void) { return (int)cb_copy_error; }
int screen_convert_backup_percent(void) { return cb_progress_percent(); }
bool screen_convert_backup_volume(uint64_t* free_bytes, uint32_t* cluster,
								  uint64_t* required) {
	*free_bytes = cb_free_bytes;
	*cluster = cb_cluster_bytes;
	*required = cb_required_bytes;
	return cb_volume_known;
}
int screen_convert_backup_convert_outcome(void) {
	return (int)cb_convert_outcome;
}
bool screen_convert_backup_backup_published(void) {
	return cb_backup_published;
}
bool screen_convert_backup_cleanup_done(void) { return cb_cleanup_done; }

/* T16a rig-only seams: called ONLY from main.c's hw_run world_copy_convert
 * block. Unarmed (every other run) they are never reached. */
void screen_convert_backup_rig_arm_fault(int mode) {
	cb_rig_fault = mode == 1 || mode == 2 ? mode : 0;
}
void screen_convert_backup_rig_interrupt(void) {
	if(cb_state != CB_COPYING || !cb_copy)
		return;
	/* No cleanup: the journal and temp tree stay, the power-cut shape. */
	world_copy_release(cb_copy);
	cb_copy = NULL;
	cb_rig_parked = true;
	fprintf(stderr, "[convert-backup] RIG interrupt: copy abandoned without "
			"cleanup\n");
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* Force the free-space query: mode 1 = these values, -1 = query fails,
 * 0 = the real volume. */
void screen_convert_backup_test_set_volume(int mode, uint64_t free_bytes,
										   uint32_t cluster_bytes) {
	cb_volume_override = mode;
	cb_override_free = free_bytes;
	cb_override_cluster = cluster_bytes;
}

/* 0 ask, 1 checking volume, 2 copying, 3 error, 4 cleaning up,
 * 5 converting, 6 done, 7 save pending, 8 conversion error. */
int screen_convert_backup_test_state(void) { return (int)cb_state; }

/* The live text exactly as render2D draws it, the button labels, and the
 * highlighted button. Returns the line count. */
int screen_convert_backup_test_text(const char** title, char lines[3][192],
									const char* labels[3], int* button_count,
									int* selected) {
	int count = 0;
	cb_title_and_lines(title, lines, &count);
	int n = cb_button_count();
	for(int k = 0; k < 3; k++)
		labels[k] = k < n ? cb_button_label(k) : NULL;
	*button_count = n;
	*selected = cb_selected;
	return count;
}

/* Activate button `k` exactly as a decided press does. */
bool screen_convert_backup_test_press(int k) {
	if(k < 0 || k >= cb_button_count())
		return false;
	cb_activate(cb_button_action(k));
	return true;
}

/* One frame of copy or convert work (the update's own calls). */
void screen_convert_backup_test_tick(void) {
	if(cb_state == CB_VOLUME || cb_state == CB_COPYING
	   || cb_state == CB_CLEANING)
		cb_copy_tick();
	else if(cb_state == CB_CONVERTING)
		cb_convert_tick();
}

/* The convert stage label exactly as render2D words it; returns its
 * percent, or -1 when the stage is indeterminate. */
int screen_convert_backup_test_convert_stage(char* label, size_t cap) {
	const char* stage;
	int pct = cb_convert_stage(&stage);
	if(label && cap && pct >= 0)
		snprintf(label, cap, "%s %d%%", stage, pct);
	else if(label && cap)
		snprintf(label, cap, "%s...", stage);
	return pct;
}

/* The footer's third (Back/Cancel) label as render2D draws it. */
const char* screen_convert_backup_test_footer_back(void) {
	return cb_is_result() ? "Back" : "Cancel";
}

int screen_convert_backup_test_percent(void) { return cb_progress_percent(); }

/* Arm a world_copy fault seam on the NEXT copy this screen creates. */
void screen_convert_backup_test_set_fault(int fault, size_t successes) {
	cb_test_fault = (enum world_copy_fault)fault;
	cb_test_fault_after = successes;
}

/* world_copy_step calls made by the last tick (the per-frame bound). */
unsigned screen_convert_backup_test_steps_last_tick(void) {
	return cb_test_steps_this_tick;
}

/* Did the temp tree still exist when the copy object was destroyed? */
bool screen_convert_backup_test_temp_left_at_destroy(void) {
	return cb_test_temp_left_at_destroy;
}
#endif
