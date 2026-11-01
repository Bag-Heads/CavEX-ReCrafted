#ifndef CAVEWII_STAGE_TOKEN_BOOTSTRAP_H
#define CAVEWII_STAGE_TOKEN_BOOTSTRAP_H

#include <errno.h>
#include <stddef.h>

enum cw_stage_token_state {
	CW_STAGE_TOKEN_PROVISIONED,
	CW_STAGE_TOKEN_BOOTSTRAP,
	CW_STAGE_TOKEN_SOFT_LAUNCH,
	CW_STAGE_TOKEN_LOCKED
};

static inline enum cw_stage_token_state cw_stage_token_state(int baked,
		int open_failed, int open_errno, int read_failed,
		size_t trimmed_len, size_t max_len) {
	if(open_failed) {
		if(baked && open_errno == ENOENT)
			return CW_STAGE_TOKEN_BOOTSTRAP;
		return baked ? CW_STAGE_TOKEN_LOCKED : CW_STAGE_TOKEN_SOFT_LAUNCH;
	}
	if(read_failed || trimmed_len == 0)
		return baked ? CW_STAGE_TOKEN_LOCKED : CW_STAGE_TOKEN_SOFT_LAUNCH;
	if(baked && trimmed_len > max_len)
		return CW_STAGE_TOKEN_LOCKED;
	return CW_STAGE_TOKEN_PROVISIONED;
}

/* C32 (inventory main.c:485-502): the loader used to feed cw_stage_token_state
 * ferror() ALONE. Two other ways of not having read the token were therefore
 * invisible and the bytes in the buffer were used as the secret regardless:
 *
 *   - fclose() failing, which on FAT is where a deferred read error surfaces;
 *   - a SHORT read -- fread stopping before the buffer is full with neither
 *     EOF nor ferror set, i.e. the file holds more than what was handed back.
 *
 * Either one means "these are not provably the token bytes", which in a BAKED
 * build is CW_STAGE_TOKEN_LOCKED, never a soft launch with a truncated secret.
 */
static inline int cw_stage_token_read_failed(int ferror_set, int fclose_failed,
											 int short_read) {
	return ferror_set != 0 || fclose_failed != 0 || short_read != 0;
}

/* Only a genuinely absent file permits first-boot provisioning. An existing
 * empty file reaches the normal loader; all other open failures remain soft. */
static inline int cw_stage_token_should_bootstrap(int open_failed, int open_errno) {
	return open_failed && open_errno == ENOENT;
}

/* 0: keep the final file; 1: temp absent, provision; 2: stale temp removed;
 * -1: temp cleanup failed. The callback never runs when the final exists. */
static inline int cw_stage_token_prepare_bootstrap(int open_failed, int open_errno,
										int (*clear_temp)(void*), void* user) {
	int cleared;
	if(!cw_stage_token_should_bootstrap(open_failed, open_errno))
		return 0;
	cleared = clear_temp(user);
	return cleared > 0 ? cleared : -1;
}

#endif
