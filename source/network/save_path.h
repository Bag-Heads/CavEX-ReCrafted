/* P04: serialize path resolution while reusing FatFs's world-directory cwd.
 * POSIX cwd is unchanged. See tmp/jobs/save-stall-1004/P04-design.md. */
#ifndef SAVE_PATH_H
#define SAVE_PATH_H
#include <stdbool.h>
#include <stddef.h>
#define SAVE_PATH_MAX 1024
/* Initialize before any I/O worker starts. PC production leaves this dormant;
 * the equivalence harness supplies its native-filesystem callbacks. */
bool save_path_init(int (*native_chdir)(const char*),
                    char* (*logical_getcwd)(char*, size_t));
void save_path_set_world(const char* world);
/* Select a file's parent before flushing an already-open archive. */
void save_path_set_parent(const char* file);
/* begin/end bracket ONE native path operation; prepare/invalidate/pair require
 * the gate held. Rewrite failure falls back to original resolution; NULL/false
 * means restoring native cwd failed, so the caller must not issue the syscall. */
void save_path_begin(void);
const char* save_path_prepare(const char* path, char out[SAVE_PATH_MAX]);
void save_path_end(void);
bool save_path_prepare_pair(const char* from,const char* to,
                            char a[SAVE_PATH_MAX],char b[SAVE_PATH_MAX],
                            const char** p,const char** q);
/* Before explicit chdir or any rename/removal outside clean exact world
 * descendants, restore native cwd; aliases cannot retain a stale cluster. */
bool save_path_invalidate(const char* path);
void save_path_stats(unsigned out[3]); /* selections, hits, fallbacks */
#endif
