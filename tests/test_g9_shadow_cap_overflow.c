/* Copyright (c) 2026 CaveWii_v2 contributors
 * R24 gate 4: exact 256-KiB retained-shadow boundary and draining fallback.
 * Real archive/worker/parser; synthetic records have zero trailing padding
 * after a valid zlib chunk stream to make each owned payload exactly 64 KiB.
 * No simulation values or production code are changed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "support/g9_save_io_rig.h"
#include "support/test_fixture_dir.h"
#include "cNBT/buffer.h"
#include "network/server_world.h"
#include "platform/time.h"

#define PAYLOAD_BYTES (64u * 1024u)
#define SHADOW_BYTES (256u * 1024u)
static uint8_t ids[32768], meta[16384], sky[16384], torch[16384], height[256];
static uint32_t held, release_worker;

static void hold_payload(void) {
	__atomic_store_n(&held, 1, __ATOMIC_RELEASE);
	while(!__atomic_load_n(&release_worker, __ATOMIC_ACQUIRE))
		thread_msleep(1);
}
static void* release_later(void* arg) {
	(void)arg;
	thread_msleep(100);
	__atomic_store_n(&release_worker, 1, __ATOMIC_RELEASE);
	return NULL;
}
static void await_writes(void) {
	ptime_t start = time_get();
	while(region_archive_test_io_incomplete()
		  && time_diff_ms(start, time_get()) < 5000)
		thread_msleep(1);
	assert(!region_archive_test_io_incomplete());
}
static void read_equal(struct region_archive* ra, unsigned x) {
	struct server_chunk sc = {0};
	assert(region_archive_get_chunk(ra, (w_coord_t)x, 0, &sc, NULL, NULL));
	assert(!memcmp(sc.ids, ids, sizeof ids));
	assert(!memcmp(sc.metadata, meta, sizeof meta));
	assert(!memcmp(sc.lighting_sky, sky, sizeof sky));
	assert(!memcmp(sc.lighting_torch, torch, sizeof torch));
	assert(!memcmp(sc.heightmap, height, sizeof height));
	server_chunk_arrays_free(&sc);
}

int main(void) {
	struct buffer records[5] = {0};
	struct server_chunk sc = {.ids = ids, .metadata = meta,
		.lighting_sky = sky, .lighting_torch = torch, .heightmap = height};
	for(size_t i = 0; i < sizeof ids; i++) ids[i] = (uint8_t)(1 + i % 4);
	memset(meta, 0x21, sizeof meta);
	memset(sky, 0xfe, sizeof sky);
	memset(torch, 0x43, sizeof torch);
	memset(height, 64, sizeof height);
	char root[200];
	test_fixture_dir("g9_shadow_cap", root, sizeof root);
	assert(G9IO_MKDIR(root) == 0);
	assert(G9IO_MKDIR(test_fixture_join(root, "/region")) == 0);
	string_t level;
	string_init_set_str(level, root);
	struct region_archive ra;
	g9io_arm(0);
	assert(region_archive_create_new(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));
	/* Use the actual encoder for valid coordinate-specific chunk records. */
	for(unsigned x = 0; x < 5; x++) {
		assert(region_archive_set_chunk_retain(&ra, x, 0, &sc, NULL, NULL,
			&records[x]));
		assert(records[x].len && records[x].len < PAYLOAD_BYTES);
		assert(buffer_reserve(&records[x], PAYLOAD_BYTES) == 0);
		memset(records[x].data + records[x].len, 0,
			PAYLOAD_BYTES - records[x].len);
		records[x].len = PAYLOAD_BYTES;
	}
	assert(region_archive_destroy(&ra));
	test_fixture_dir_cleanup(root);
	assert(G9IO_MKDIR(root) == 0);
	assert(G9IO_MKDIR(test_fixture_join(root, "/region")) == 0);
	g9io_arm(2);
	assert(region_archive_create_new(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));
	region_archive_batch_begin(&ra);
	unsigned long visible = ra.rio_read_bytes;
	for(unsigned x = 0; x < 5; x++) {
		assert(region_archive_recommit_record(&ra, x, 0, &records[x]));
		await_writes();
		/* Reconcile completed payloads without publishing/syncing the batch. */
		region_archive_batch_begin(&ra);
		assert(!ra.rio_ops && ra.rio_shadow);
		assert(ra.rio_read_bytes == visible);
		uint32_t value = ra.offsets[x];
		assert((uint64_t)((value >> 8) + (value & 255)) * REGION_SECTOR_SIZE
			> visible);
	}
	uint32_t before[REGION_DEFERRED_STATS], after[REGION_DEFERRED_STATS];
	region_archive_deferred_stats(before);
	for(unsigned x = 0; x < 4; x++) read_equal(&ra, x);
	region_archive_deferred_stats(after);
	assert(after[10] == before[10] + 4 && after[5] == before[5]);
	printf("PASS: four retired payload hits retain exactly %u bytes; no drains\n",
		SHADOW_BYTES);
	/* Keep THIS handle incomplete while reading the fifth, retired payload.
	 * The unrelated queued record cannot satisfy that read. */
	region_archive_test_payload_hook(hold_payload);
	assert(region_archive_recommit_record(&ra, 0, 0, &records[0]));
	ptime_t start = time_get();
	while(!__atomic_load_n(&held, __ATOMIC_ACQUIRE)
		  && time_diff_ms(start, time_get()) < 5000)
		thread_msleep(1);
	assert(__atomic_load_n(&held, __ATOMIC_ACQUIRE));
	region_archive_deferred_stats(before);
	struct thread rel;
	assert(thread_create(&rel, release_later, NULL, 64));
	read_equal(&ra, 4);
	region_archive_deferred_stats(after);
	__atomic_store_n(&release_worker, 1, __ATOMIC_RELEASE);
	thread_join(&rel);
	region_archive_test_payload_hook(NULL);
	printf("overflow read: barrier waits +%u; retained hits +%u; all decoded bytes equal\n",
		after[5] - before[5], after[10] - before[10]);
	fflush(stdout);
	assert(after[5] == before[5] + 1 && "shadow overflow must use draining-wait fallback");
	assert(after[10] == before[10] && "overflowing payload must not be a retained hit");
	assert(region_archive_batch_end(&ra) && region_archive_sync(&ra));
	/* Compare every compressed payload byte too, after covering durability. */
	uint8_t* disk = malloc(PAYLOAD_BYTES);
	assert(disk);
	for(unsigned x = 0; x < 5; x++) {
		uint8_t framing[5];
		assert(fseek(ra.file, (long)(ra.offsets[x] >> 8) * REGION_SECTOR_SIZE,
			SEEK_SET) == 0);
		assert(fread(framing, sizeof framing, 1, ra.file) == 1);
		uint32_t length = (uint32_t)framing[0] << 24
			| (uint32_t)framing[1] << 16 | (uint32_t)framing[2] << 8 | framing[3];
		assert(length == PAYLOAD_BYTES + 1 && framing[4] == 2);
		assert(fread(disk, PAYLOAD_BYTES, 1, ra.file) == 1);
		assert(!memcmp(disk, records[x].data, PAYLOAD_BYTES));
		buffer_free(&records[x]);
	}
	free(disk);
	assert(region_archive_destroy(&ra));
	string_clear(level);
	test_fixture_dir_cleanup(root);
	g9io_arm(1);
	puts("test_g9_shadow_cap_overflow: PASS (draining fallback; decoded and compressed bytes equal)");
	return 0;
}
