/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/*
	Pack digest manifest (pack-cache-0927; Astra astra-packkey-0927).

	A HELPER-MANAGED pack carries <pack-root>/.cavewii/manifest.cwm, written
	at import time from the FINAL installed bytes (tools/audio_cache_tool
	--manifest, called by the Pack Helper): one line per regular file under
	the pack root (except .cavewii/ itself) with its FNV-1a 64, size and
	relative path, sorted by path, closed by an "end" line carrying the
	FNV-1a 64 of every byte before it. That end hash is the pack's DIGEST.

	The cache keys of a managed pack use this digest instead of reading every
	source byte (the full source hash made the audio cache slower than a fresh
	decode on the shipped WAV packs). What that trades, stated as Astra
	ruled: nothing on the console re-reads the files, so a MANUAL edit of an
	installed file (same name, any size) is NOT detected -- a manually changed
	pack must be re-imported through the helper, or its .cavewii/ deleted,
	which drops it back to unmanaged. Unmanaged packs (no manifest, or one
	that fails its own checksum) keep full hashing or bypass the cache.
	The bundled root is never "managed": it is always fully hashed.

	Text format:
	  CWMANIFEST 1
	  <fnv64 hex16> <size decimal> <relpath>
	  ...
	  end <fnv64 hex16>
*/

#ifndef PACK_MANIFEST_H
#define PACK_MANIFEST_H

#include <stdbool.h>
#include <stdint.h>

#define PACK_MANIFEST_NAME ".cavewii/manifest.cwm"

enum pack_manifest_status {
	PACK_MANIFEST_ABSENT = 0, /* no manifest: an unmanaged pack */
	PACK_MANIFEST_TRUSTED,	  /* present, its own end hash verified */
	PACK_MANIFEST_UNTRUSTED,  /* present but torn/corrupt/oversized: unmanaged */
	/* present but could not be READ (no memory / I/O error). NOT unmanaged:
	   treating it so would key the pack differently for this one load and
	   thrash the two-slot store (Fable M2). Callers bypass the cache. */
	PACK_MANIFEST_ERROR,
};

struct pack_manifest {
	bool trusted;	 /* status == PACK_MANIFEST_TRUSTED */
	enum pack_manifest_status status;
	uint64_t digest; /* the end hash */
	uint32_t files;
	uint32_t sound_ogg; /* listed files "sound/...*.ogg" (the audio policy) */
};

/* Read and verify <pack_root>/.cavewii/manifest.cwm. Returns trusted; the
   status says why not. The buffer is sized by the file (bounded by a 1 MiB
   cap), never a fixed 1 MiB. */
bool pack_manifest_load(const char* pack_root, struct pack_manifest* out);
#ifdef CAVEWII_TESTING
/* Fail the manifest read's buffer allocation (Fable M2 fixture). */
void pack_manifest_test_fail_alloc(bool fail);
#endif

/* Number of files the (trusted) manifest lists whose relative path starts
   with `prefix` and ends with `suffix`; 0 when there is no trusted manifest.
   (The audio cache uses it: only packs that ship OGG sounds benefit.) */
uint32_t pack_manifest_count(const char* pack_root, const char* prefix,
							 const char* suffix);

/* Walk <pack_root> (skipping .cavewii/), hash every regular file and write
   the manifest. The producer side (host tool / helper). False when the root
   cannot be walked or the manifest cannot be written; nothing half-written
   is left in place of a previous manifest (written to a temp name, then the
   old one removed and the temp renamed -- a torn manifest fails its own end
   hash and reads as unmanaged). */
bool pack_manifest_write(const char* pack_root, struct pack_manifest* out);

#endif
