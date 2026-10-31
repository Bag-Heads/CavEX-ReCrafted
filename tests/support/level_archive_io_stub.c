/* Copyright (c) 2026 CaveWii_v2 contributors
 * G9 R19: the lean level-archive tests compile level_archive.c without the
 * region layer. Its only region-layer dependencies are the deferred save IO
 * seams, which are OFF here: deferred mode never enabled, nothing queued,
 * every barrier a no-op -- exactly the behaviour region_archive.c has while
 * experimental.g9_save_io_int is 0. */
#include <stdbool.h>
#include <stddef.h>

struct region_io_call;

bool region_archive_deferred_io_enabled(void) { return false; }
struct region_io_call* region_archive_io_call_submit(bool (*fn)(void* ctx),
													 void* ctx) {
	(void)fn;
	(void)ctx;
	return NULL;
}
bool region_archive_io_call_poll(const struct region_io_call* c, bool* ok) {
	(void)c;
	if(ok)
		*ok = false;
	return true;
}
bool region_archive_io_call_wait(struct region_io_call* c) {
	(void)c;
	return false;
}
void region_archive_io_barrier_all(void) {}
bool region_archive_logical_file_size(const char* path, unsigned long* bytes) {
	(void)path;
	(void)bytes;
	return false;
}
