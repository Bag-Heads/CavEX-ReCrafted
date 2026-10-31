/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-S single-world SAVE-BYTES EQUIVALENCE. The shared region pool replaces
 * the per-world MAX_REGIONS LRU and the save path's uncached per-chunk
 * temporary handle; for a SINGLE world the region files it writes must be
 * byte-identical to the pre-A2-S tree.
 *
 * One deterministic scenario: a standalone superflat world, ten chunks in five
 * regions (so the four-handle cap evicts), each edited and checkpointed in
 * batches, then synced and torn down. Every region file's bytes -- with the
 * 4 KiB per-chunk TIMESTAMP table (sector 1, bytes 4096..8191) reduced to
 * each entry's CANONICAL FLAG, bit 31 (the low 31 bits are Beta's wall-clock
 * time(NULL), region_archive.c:420, never reproducible run to run; the high
 * bit is the journaled canonical-write marker and MUST compare, A2S-P1
 * review L2) -- and every `.mcr.idx` journal sidecar are folded
 * into one FNV-1a hash, in sorted file-name order, and compared with the
 * GOLDEN captured on the pre-A2-S base (codex/a2e-0926 4662d54e, the a2e
 * worktree build). A moved hash is a divergence to STOP on, never to recapture.
 *
 * Uses only pre-A2-S public APIs so the same source builds on the base. */
#include "block/blocks.h"
#include "network/region_archive.h"
#include "network/server_world.h"
#include "support/test_fixture_dir.h"

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif
#include <m-lib/m-string.h>

/* Captured on the base tree (see header). */
/* save-enc-1030 (2026-10-30, fast re-pin ruling): Beta save encoding (hw1030a
 * NBT diff of the real Beta world dj_orig) -- empty Entities/TileEntities lists
 * element type 1 (NBTTagList.java:25-31); LastUpdate = world time, per-class
 * mob tags and double Pos/Motion do not touch this corpus. Reverting only the
 * list-type byte reproduces the old pin 9d0b91b11b480d57 exactly; new value 8/8 runs. */
#define GOLDEN_SAVE_BYTES UINT64_C(0xa09f17af1e4e1170)

#define FNV_BASIS UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)
#define REGIONS 5
#define TIMESTAMP_SECTOR_START 4096
#define TIMESTAMP_SECTOR_END 8192

static uint64_t hash_bytes(uint64_t h, const void* data, size_t n) {
	const uint8_t* p = data;
	while(n--) {
		h ^= *p++;
		h *= FNV_PRIME;
	}
	return h;
}

/* Fold one file; `mask_timestamps` reduces every big-endian uint32 of sector
 * 1 to its bit 31 (the canonical flag), dropping the wall-clock low bits.
 * Returns false when the file does not exist (a sidecar may be absent). */
static bool hash_file(uint64_t* h, const char* path, bool mask_timestamps,
					  size_t* out_len) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	uint8_t buf[4096];
	size_t off = 0, n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		for(size_t i = 0; i < n; i++) {
			size_t at = off + i;
			if(mask_timestamps && at >= TIMESTAMP_SECTOR_START
			   && at < TIMESTAMP_SECTOR_END) {
				if((at - TIMESTAMP_SECTOR_START) % 4 != 0)
					continue; /* low bytes of the entry: wall clock */
				uint8_t canonical = buf[i] & 0x80; /* MSB of the big-endian word */
				*h = hash_bytes(*h, &canonical, 1);
				continue;
			}
			*h = hash_bytes(*h, &buf[i], 1);
		}
		off += n;
	}
	fclose(f);
	if(out_len)
		*out_len = off;
	return true;
}

static char s_root[128];

int main(void) {
	test_fixture_dir("test_a2_save_bytes_equiv_tmp", s_root, sizeof s_root);
	MKDIR(s_root);
	MKDIR(test_fixture_join(s_root, "/region"));
	for(int r = 0; r < REGIONS; r++) {
		char path[256];
		snprintf(path, sizeof(path), "%s/region/r.%d.0.mcr", s_root, r);
		remove(path);
		snprintf(path, sizeof(path), "%s/region/r.%d.0.mcr.idx", s_root, r);
		remove(path);
	}
	string_t level;
	string_init_set_str(level, s_root);

	/* ---- the scenario ---- */
	struct server_world w = {0};
	server_world_create(&w, level, WORLD_DIM_OVERWORLD, 4242, true);
	/* Batch 1: two chunks per region, regions 0..2, edited at distinct cells. */
	for(int r = 0; r < 3; r++)
		for(int k = 0; k < 2; k++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&w, (w_coord_t)(r * 32 + k), 0, &sc)
				   && sc);
			sc->ids[(r * 7 + k * 3) % 64] = BLOCK_BRICKS;
			sc->modified = true;
		}
	assert(server_world_checkpoint_chunks(&w));
	/* Batch 2: regions 3..4 -- the fifth region evicts through the cap. */
	for(int r = 3; r < REGIONS; r++)
		for(int k = 0; k < 2; k++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&w, (w_coord_t)(r * 32 + k), 0, &sc)
				   && sc);
			sc->ids[(r * 11 + k * 5) % 64] = BLOCK_BRICKS;
			sc->metadata[0] = (uint8_t)(0x11 * (k + 1));
			sc->modified = true;
		}
	assert(server_world_checkpoint_chunks(&w));
	/* Batch 3: re-edit an early region (its handle was evicted -> reopened).
	 * The chunk is RESIDENT (load_chunk refuses a resident chunk), so take it
	 * from the dictionary. */
	{
		struct server_chunk* sc
			= dict_server_chunks_get(w.chunks, S_CHUNK_ID(1, 0));
		assert(sc);
		sc->ids[40] = BLOCK_BRICKS;
		sc->modified = true;
		assert(server_world_checkpoint_chunks(&w));
	}
	assert(server_world_sync_regions(&w));
	server_world_destroy(&w);

	/* ---- fold the on-disk bytes ---- */
	uint64_t h = FNV_BASIS;
	size_t total = 0, files = 0;
	for(int r = 0; r < REGIONS; r++) {
		char path[256];
		size_t len = 0;
		snprintf(path, sizeof(path), "%s/region/r.%d.0.mcr", s_root, r);
		assert(hash_file(&h, path, true, &len) && "every region must exist");
		total += len;
		files++;
		snprintf(path, sizeof(path), "%s/region/r.%d.0.mcr.idx", s_root, r);
		if(hash_file(&h, path, false, &len)) {
			total += len;
			files++;
		}
	}
	printf("SAVE-BYTES %d regions, %zu files, %zu bytes (timestamp entries "
		   "reduced to the canonical bit): %016" PRIx64 "\n",
		   REGIONS, files, total, h);
	if(GOLDEN_SAVE_BYTES == 0) {
		puts("CAPTURE MODE: no golden pinned yet -- paste the hash above into "
			 "GOLDEN_SAVE_BYTES");
	} else if(h != GOLDEN_SAVE_BYTES) {
		fprintf(stderr,
				"DIVERGENCE: single-world save bytes %016" PRIx64
				" != golden %016" PRIx64 " (pre-A2-S base)\n",
				h, GOLDEN_SAVE_BYTES);
		return 1;
	} else {
		puts("PASS single-world save bytes identical to the pre-A2-S base");
	}
	string_clear(level);
	test_fixture_dir_cleanup(s_root);
	return GOLDEN_SAVE_BYTES == 0 ? 2 : 0;
}
