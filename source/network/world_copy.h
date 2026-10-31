#ifndef WORLD_COPY_H
#define WORLD_COPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Includes the NUL and, on Windows, the absolute-path prefix. Root is depth 0;
 * deeper trees fail rather than truncate. */
#define WORLD_COPY_PATH_MAX 256
#define WORLD_COPY_DEPTH_MAX 16
#ifndef WORLD_COPY_STEP_MAX
#define WORLD_COPY_STEP_MAX 32 /* Tune stat/readdir work for libfat. */
#endif
#define WORLD_COPY_BUFFER_BYTES 16384
#ifndef WORLD_COPY_BYTES_PER_STEP
#define WORLD_COPY_BYTES_PER_STEP 65536
#endif

enum world_copy_state {
	WORLD_COPY_SURVEYING,
	WORLD_COPY_READY,
	WORLD_COPY_COPYING,
	WORLD_COPY_CLEANING,
	WORLD_COPY_DONE,
	WORLD_COPY_CANCELLED,
	WORLD_COPY_FAILED
};

enum world_copy_error {
	WORLD_COPY_OK,
	WORLD_COPY_INVALID_ARGUMENT,
	WORLD_COPY_PATH_TOO_LONG,
	WORLD_COPY_UNSUPPORTED_DEPTH,
	WORLD_COPY_UNSUPPORTED_LINK,
	WORLD_COPY_UNSUPPORTED_TYPE,
	WORLD_COPY_IO_ERROR,
	WORLD_COPY_SIZE_OVERFLOW,
	WORLD_COPY_NAME_EXHAUSTED,
	WORLD_COPY_CLEANUP_ERROR,
	WORLD_COPY_LOW_SPACE,
	WORLD_COPY_INVALID_LEVEL,
	WORLD_COPY_COLLISION
};

struct world_copy_result {
	enum world_copy_state state;
	enum world_copy_error error;
	int system_error;
	int cleanup_error; /* Secondary close/removal failure; primary is preserved. */
	uint64_t copied_bytes;
	uint64_t files;
	uint64_t directories; /* Includes the source root. */
	uint64_t bytes;
	/* Conservative FAT allocation bound: rounded file data, directory LFN
	 * slots, the final directory and added parent-directory slots.
	 * Not a free-space check; cluster_bytes must describe the target volume. */
	uint64_t required_bytes;
	char destination[WORLD_COPY_PATH_MAX];
	char temporary[WORLD_COPY_PATH_MAX]; /* Deprecated compatibility field; always empty. */
	char error_path[WORLD_COPY_PATH_MAX];
};

struct world_copy;

/* Survey a quiescent world. READY only selects unused destination/journal
 * names; it creates nothing. Copy writes and syncs a journal naming the exact
 * final '<name> (backup)' / '<name> (backup N)' path, creates that directory and
 * syncs its .incomplete marker before writing payload. Display lists must hide
 * marked worlds and any destination named by a valid journal. Publication
 * removes the marker last, then the journal. Recovery removes marked trees
 * only with a valid matching journal; unmarked directories are removed only
 * after checking they are empty. Absent and completed destinations retire
 * their journals; other unmarked nonempty trees keep theirs. No renames.
 * Callers serialize survey/copy/recovery in a parent (across processes too).
 * The source stays quiescent and read-only. Paths use '/' and reject '.'/'..'.
 * NULL means allocation failed; all other failures have a named result. */
struct world_copy* world_copy_survey_begin(const char* source,
	uint32_t cluster_bytes);
/* One snapshot per list refresh; failure returns false with *journals = NULL.
 * No journal produces a successful empty snapshot. Callers serialize with copy. */
struct world_copy_journals;
bool world_copy_journals_read(const char* parent, struct world_copy_journals** journals);
bool world_copy_journals_contains(const struct world_copy_journals* journals,
	const char* destination);
void world_copy_journals_free(struct world_copy_journals* journals);
/* At most min(work_budget, WORLD_COPY_STEP_MAX) units, each visiting one
 * entry, validating one ancestor, closing one directory or trying one name.
 * Copying also transfers at most WORLD_COPY_BYTES_PER_STEP bytes per call,
 * through a fixed static buffer. Metadata parsing/compression uses the existing
 * whole-tree NBT helpers once; metadata reads/writes remain stepped. Calls are
 * serialized: the static transfer buffer is shared. A zero budget does no work. */
enum world_copy_state world_copy_step(struct world_copy* copy,
	size_t work_budget);
const struct world_copy_result* world_copy_get_result(const struct world_copy* copy);
const char* world_copy_error_name(enum world_copy_error error);
/* Start only at READY, after obtaining free bytes on the target volume.
 * mkdir refuses occupied names, including empty directories. LevelName gains
 * " (backup)". No filesystem work is performed by start itself. */
enum world_copy_state world_copy_start(struct world_copy* copy, uint64_t free_bytes);
/* Cancellation cleans the owned destination tree in bounded steps; drive CLEANING
 * to CANCELLED/FAILED with step, or destroy to drain cleanup synchronously. */
void world_copy_cancel(struct world_copy* copy);
/* Closes handles and drains owned-destination cleanup. DONE retains the backup. */
enum world_copy_error world_copy_destroy(struct world_copy* copy);

/* Bounded handle/memory release in any state, including failed cleanup.
 * No disk writes, removal, rename or cleanup retry. Leaves owned paths/journal
 * on disk for recovery by a later survey; accepts NULL. */
void world_copy_release(struct world_copy* copy);
#if defined(CAVEWII_TESTING) || (!defined(PLATFORM_PC) && !defined(PLATFORM_WII))
size_t world_copy_test_open_handles(void);
#endif

/* Dormant per-object fault seam: fail the selected operation after N successes.
 * CLOSE closes the real handle before reporting its injected failure. */
enum world_copy_fault { WORLD_COPY_FAULT_NONE, WORLD_COPY_FAULT_WRITE,
	WORLD_COPY_FAULT_FLUSH, WORLD_COPY_FAULT_CLOSE };
void world_copy_test_fail_after(struct world_copy* copy,
	enum world_copy_fault fault, size_t successes);

#endif
