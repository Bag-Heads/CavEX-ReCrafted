/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-S (astra-a2s-design-0926): the ONE result convention of the storage
 * layer -- region pool admission, archive prepare/preflight/commit, and the
 * checkpoint/ticket APIs that follow. Kept in its own header so the tile
 * store and the archives can name it without pulling region_archive.h. */
#ifndef STORAGE_RC_H
#define STORAGE_RC_H

enum storage_rc {
	STORAGE_OK = 0,
	STORAGE_PENDING,
	STORAGE_BUSY,
	STORAGE_ABSENT,
	STORAGE_STALE,
	STORAGE_CAPACITY,
	STORAGE_IO_ERROR,
	STORAGE_CORRUPT,
	STORAGE_UNKNOWN,
};

#endif
