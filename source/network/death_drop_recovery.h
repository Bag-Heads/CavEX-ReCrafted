/* Crash recovery for death_drop_journal. */

#ifndef DEATH_DROP_RECOVERY_H
#define DEATH_DROP_RECOVERY_H

#include <m-lib/m-string.h>
#include <stdbool.h>

struct level_archive;

/* Idempotent. PREPARING predates every source/entity mutation and is removed;
 * PREPARED completes every recorded drop/container edit in its recorded chunk
 * before committing the cleared respawn player to level.dat. */
bool death_drop_recover_if_needed(string_t world_name,
								  struct level_archive* level);

#ifdef CAVEWII_DEATH_DROP_TEST
void death_drop_recovery_test_fail_next_alloc(void);
void death_drop_recovery_test_fail_after_chunks(unsigned count);
#endif

#endif
