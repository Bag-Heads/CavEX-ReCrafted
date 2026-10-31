/* Copyright (c) 2026 CaveWii_v2 contributors
 * G6 separation (astra-journal-0926): "preserve and visibly refuse unresolved
 * experimental journals rather than ignoring them." This tree has no
 * checkpoint journal; a save carrying `checkpoint.journal` from a
 * journal-enabled build is classified by checkpoint_journal_guard:
 *   - no file                          -> loads
 *   - INTENT ... COMPLETE (resolved)   -> loads
 *   - INTENT ... RECOVERED (resolved)  -> loads
 *   - INTENT only (unresolved)         -> REFUSED, file byte-identical
 *   - INTENT + torn COMPLETE tail      -> REFUSED (unresolved), preserved
 *   - torn INTENT alone                -> loads (nothing was published)
 *   - 0-byte / 7-byte / foreign magic  -> INERT: loads (Opus G6SEP M1: the
 *                                         deferred module truncates before it
 *                                         writes the next header)
 * The refusal is exercised through the production load path
 * (server_local_load_world_impl refuses before level.dat is opened) and, for
 * the visible reason (L1), through SRPC_LOAD_WORLD -> CRPC_LOAD_FAILED. Every
 * refused load leaves the journal, level.dat AND the region file byte-identical
 * (L2). RED by construction until CHECKPOINT_JOURNAL_GUARD_API. */
#include "network/checkpoint_journal_guard.h"
#include "network/level_archive.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_interface.h"
#include "support/split_residency_fixture.h"

#include <inttypes.h>
#include <stdio.h>

#ifndef CHECKPOINT_JOURNAL_GUARD_API
int main(void) {
	puts("RED: CHECKPOINT_JOURNAL_GUARD_API absent -- experimental journals are "
		 "ignored");
	return 1;
}
#else

static void put_u32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t crc32_update(uint32_t crc, const void* data, size_t n) {
	const uint8_t* p = data;
	crc = ~crc;
	while(n--) {
		crc ^= *p++;
		for(int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

/* Append one framed record; `torn` writes the header and half the payload. */
static size_t put_record(uint8_t* out, uint32_t type, const uint8_t* payload,
						 uint32_t len, bool torn) {
	uint8_t* q = out;
	put_u32(q, type);
	put_u32(q + 4, len);
	if(torn) {
		memcpy(q + 8, payload, len / 2);
		return 8 + len / 2;
	}
	memcpy(q + 8, payload, len);
	uint32_t crc = crc32_update(0, q, 8);
	crc = crc32_update(crc, payload, len);
	put_u32(q + 8 + len, crc);
	return 12 + len;
}

static void write_journal(const char* root, const uint8_t* bytes, size_t n) {
	char path[300];
	snprintf(path, sizeof(path), "%s/%s", root, CHECKPOINT_JOURNAL_GUARD_NAME);
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(n == 0 || fwrite(bytes, 1, n, f) == n);
	fclose(f);
}

static uint64_t journal_hash(const char* root, bool* exists) {
	char path[300];
	snprintf(path, sizeof(path), "%s/%s", root, CHECKPOINT_JOURNAL_GUARD_NAME);
	FILE* f = fopen(path, "rb");
	*exists = f != NULL;
	if(!f)
		return 0;
	uint64_t h = UINT64_C(14695981039346656037);
	int c;
	while((c = fgetc(f)) != EOF) {
		h ^= (uint8_t)c;
		h *= UINT64_C(1099511628211);
	}
	fclose(f);
	return h;
}

static size_t header(uint8_t* out) {
	put_u32(out, 0x4B434357u);
	put_u32(out + 4, 1u);
	return 8;
}

static size_t intent_payload(uint8_t* out, uint32_t epoch) {
	put_u32(out, epoch);
	put_u32(out + 4, epoch - 1);
	put_u32(out + 8, 0);  /* level_exists */
	put_u32(out + 12, 0); /* level_len */
	put_u32(out + 16, 0); /* region_count */
	return 20;
}

static char s_last_failed_reason[64];
static unsigned s_load_failed_msgs;
static void observe(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_LOAD_FAILED) {
		s_load_failed_msgs++;
		memcpy(s_last_failed_reason, call->payload.load_failed_reason,
			   sizeof(s_last_failed_reason));
		s_last_failed_reason[sizeof(s_last_failed_reason) - 1] = 0;
	}
	clin_drain_discard(); /* the loads publish; keep the inbox from filling */
}

/* L2: the three authoritative files a refusal must leave untouched. */
static uint64_t save_hash(const char* root) {
	bool e1, e2, e3;
	char lvl[300], reg[300];
	snprintf(lvl, sizeof(lvl), "%s/level.dat", root);
	snprintf(reg, sizeof(reg), "%s/region/r.0.0.mcr", root);
	uint64_t h = journal_hash(root, &e1);
	FILE* f = fopen(lvl, "rb");
	e2 = f != NULL;
	if(f) {
		int c;
		while((c = fgetc(f)) != EOF) {
			h ^= (uint8_t)c;
			h *= UINT64_C(1099511628211);
		}
		fclose(f);
	}
	f = fopen(reg, "rb");
	e3 = f != NULL;
	if(f) {
		int c;
		while((c = fgetc(f)) != EOF) {
			h ^= (uint8_t)c;
			h *= UINT64_C(1099511628211);
		}
		fclose(f);
	}
	assert(e1 && e2 && e3 && "fixture: journal, level.dat and r.0.0.mcr exist");
	return h;
}

/* Drive the production load path against `root` (a real save exists there
 * from the fixture); returns whether the load was accepted. */
static bool load_accepted(struct server_local* s, const char* root) {
	string_t name;
	string_init_set_str(name, root);
	bool ok = server_local_load_world_impl(s, name);
	string_clear(name);
	return ok;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(clin_init() && svin_init());
	clin_test_set_rpc_observer(observe, NULL);
	struct server_local* s = split_fixture();
	/* a real save under split_path: level.dat + a region, so a refused load is
	 * attributable to the journal alone */
	{
		struct server_chunk* c = split_populate(s, 0, 0);
		assert(c);
		c->modified = true;
		assert(server_world_checkpoint_chunks(&s->worlds[0]));
		assert(server_world_sync_regions(&s->worlds[0]));
		assert(level_archive_save(&s->level));
	}
	server_world_discard(&s->worlds[0]);
	level_archive_discard(&s->level);
	s->player.has_pos = false;
	string_t root;
	string_init_set_str(root, split_path);
	uint8_t buf[512], pl[64];
	size_t n;
	struct checkpoint_journal_verdict v;

	/* 1. no journal */
	assert(checkpoint_journal_allows_load(root, &v) && v.state == CHECKPOINT_JOURNAL_NONE
		   && !v.inert);
	assert(load_accepted(s, split_path) && "no journal: loads");
	server_world_discard(&s->worlds[0]);
	level_archive_discard(&s->level);
	s->player.has_pos = false;
	puts("PASS no journal -> loads");

	/* 2. resolved: INTENT + COMPLETE */
	n = header(buf);
	n += put_record(buf + n, 1, pl, (uint32_t)intent_payload(pl, 7), false);
	put_u32(pl, 7);
	memset(pl + 4, 0xAB, 8);
	n += put_record(buf + n, 2, pl, 12, false);
	write_journal(split_path, buf, n);
	assert(checkpoint_journal_allows_load(root, &v)
		   && v.state == CHECKPOINT_JOURNAL_RESOLVED && v.epoch == 7);
	assert(load_accepted(s, split_path) && "COMPLETE journal: inert, loads");
	server_world_discard(&s->worlds[0]);
	level_archive_discard(&s->level);
	s->player.has_pos = false;
	puts("PASS INTENT+COMPLETE -> resolved, loads");

	/* 3. resolved: INTENT + RECOVERED */
	n = header(buf);
	n += put_record(buf + n, 1, pl, (uint32_t)intent_payload(pl, 8), false);
	put_u32(pl, 8);
	n += put_record(buf + n, 3, pl, 4, false);
	write_journal(split_path, buf, n);
	assert(checkpoint_journal_allows_load(root, &v)
		   && v.state == CHECKPOINT_JOURNAL_RESOLVED);
	puts("PASS INTENT+RECOVERED -> resolved");

	/* 4. UNRESOLVED: INTENT only -> refused, preserved (journal + level.dat +
	 * region, L2), and the reason reaches the failure card (L1) */
	n = header(buf);
	n += put_record(buf + n, 1, pl, (uint32_t)intent_payload(pl, 9), false);
	write_journal(split_path, buf, n);
	bool ex;
	uint64_t before = save_hash(split_path);
	assert(!checkpoint_journal_allows_load(root, &v)
		   && v.state == CHECKPOINT_JOURNAL_UNRESOLVED && v.epoch == 9);
	assert(!load_accepted(s, split_path) && "INTENT-only journal: REFUSED");
	assert(save_hash(split_path) == before
		   && "L2: journal, level.dat and the region file are preserved byte for byte");
	assert(strcmp(server_local_load_refusal_reason(),
				  "Save has an unfinished checkpoint journal") == 0
		   && "L1: the refusal names its reason");
	/* the production menu load: SRPC_LOAD_WORLD -> the card text */
	{
		struct server_rpc call = {.type = SRPC_LOAD_WORLD};
		string_init_set_str(call.payload.load_world.name, split_path);
		s_load_failed_msgs = 0;
		s_last_failed_reason[0] = 0;
		assert(svin_rpc_send(&call));
		server_local_test_process_messages(s);
		clin_drain_discard();
		assert(s_load_failed_msgs == 1 && s->load_failure_terminal
			   && "the menu load publishes exactly one terminal LOAD_FAILED");
		assert(strcmp(s_last_failed_reason,
					  "Save has an unfinished checkpoint journal") == 0
			   && "L1: CRPC_LOAD_FAILED carries the journal reason for the card");
		assert(server_local_load_refusal_reason()[0] == 0
			   && "the reason is consumed by the one card");
		s->load_failure_terminal = false;
		assert(save_hash(split_path) == before && "L2 holds across the menu load too");
	}
	puts("PASS INTENT only -> unresolved, refused, preserved, reason on the card");

	/* 5. UNRESOLVED: INTENT + torn COMPLETE */
	n = header(buf);
	n += put_record(buf + n, 1, pl, (uint32_t)intent_payload(pl, 10), false);
	put_u32(pl, 10);
	memset(pl + 4, 0xCD, 8);
	n += put_record(buf + n, 2, pl, 12, true);
	write_journal(split_path, buf, n);
	before = save_hash(split_path);
	assert(!checkpoint_journal_allows_load(root, &v)
		   && v.state == CHECKPOINT_JOURNAL_UNRESOLVED && v.torn_tail);
	assert(!load_accepted(s, split_path));
	assert(save_hash(split_path) == before && "L2: preserved");
	assert(strcmp(server_local_load_refusal_reason(),
				  "Save has an unfinished checkpoint (torn journal)") == 0);
	puts("PASS INTENT + torn COMPLETE -> unresolved, refused, preserved");

	/* 6. torn INTENT alone: nothing was published -> loads */
	n = header(buf);
	n += put_record(buf + n, 1, pl, (uint32_t)intent_payload(pl, 11), true);
	write_journal(split_path, buf, n);
	assert(checkpoint_journal_allows_load(root, &v)
		   && v.state == CHECKPOINT_JOURNAL_RESOLVED && v.torn_tail);
	assert(load_accepted(s, split_path) && "torn INTENT alone: loads");
	server_world_discard(&s->worlds[0]);
	level_archive_discard(&s->level);
	s->player.has_pos = false;
	puts("PASS torn INTENT alone -> loads");

	/* 7. M1: 0 bytes, 7 bytes, foreign magic -> INERT (loads; the file is
	 * left alone). The deferred module truncates the journal before it writes
	 * the next epoch's header, so a crash there leaves exactly this behind with
	 * the previous epoch COMPLETE and nothing new published. */
	static const size_t inert_lens[3] = {0, 7, 40};
	for(int k = 0; k < 3; k++) {
		memset(buf, 0x5A, sizeof(buf));
		write_journal(split_path, buf, inert_lens[k]);
		before = journal_hash(split_path, &ex);
		assert(ex);
		assert(checkpoint_journal_allows_load(root, &v)
			   && v.state == CHECKPOINT_JOURNAL_RESOLVED && v.inert && !v.io_error);
		assert(load_accepted(s, split_path) && "inert journal: loads");
		assert(journal_hash(split_path, &ex) == before && ex
			   && "an inert journal is left alone, never rewritten");
		server_world_discard(&s->worlds[0]);
		level_archive_discard(&s->level);
		s->player.has_pos = false;
		printf("PASS %zu-byte journal (no header) -> inert, loads\n", inert_lens[k]);
	}

	string_clear(root);
	{
		char path[300];
		snprintf(path, sizeof(path), "%s/%s", split_path,
				 CHECKPOINT_JOURNAL_GUARD_NAME);
		remove(path);
	}
	/* leave a live world behind for split_destroy */
	assert(load_accepted(s, split_path));
	clin_test_set_rpc_observer(NULL, NULL);
	clin_drain_discard();
	split_destroy(s);
	return 0;
}
#endif
