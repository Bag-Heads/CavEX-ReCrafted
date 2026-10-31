/* Copyright (c) 2026 CaveWii_v2 contributors
 * G6 journal guard -- see checkpoint_journal_guard.h. */
#include "checkpoint_journal_guard.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD_MAGIC 0x4B434357u
#define GUARD_VERSION 1u
#define REC_INTENT 1u
#define REC_COMPLETE 2u
#define REC_RECOVERED 3u

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

static uint32_t get_u32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
		| ((uint32_t)p[3] << 24);
}

struct checkpoint_journal_verdict checkpoint_journal_inspect(string_t root) {
	struct checkpoint_journal_verdict v = {0};
	string_t path;
	string_init_printf(path, "%s/%s", string_get_cstr(root),
					   CHECKPOINT_JOURNAL_GUARD_NAME);
	FILE* f = fopen(string_get_cstr(path), "rb");
	string_clear(path);
	if(!f) {
		if(errno != ENOENT)
			v.io_error = true; /* unreadable: treat as malformed below */
		v.state = v.io_error ? CHECKPOINT_JOURNAL_MALFORMED
							 : CHECKPOINT_JOURNAL_NONE;
		return v;
	}
	if(fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		v.io_error = true;
		v.state = CHECKPOINT_JOURNAL_MALFORMED;
		return v;
	}
	long n = ftell(f);
	if(n < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		v.io_error = true;
		v.state = CHECKPOINT_JOURNAL_MALFORMED;
		return v;
	}
	uint8_t* data = malloc((size_t)n ? (size_t)n : 1);
	if(!data) {
		fclose(f);
		v.io_error = true;
		v.state = CHECKPOINT_JOURNAL_MALFORMED;
		return v;
	}
	bool ok = n == 0 || fread(data, 1, (size_t)n, f) == (size_t)n;
	fclose(f);
	if(!ok) {
		free(data);
		v.io_error = true;
		v.state = CHECKPOINT_JOURNAL_MALFORMED;
		return v;
	}
	size_t len = (size_t)n;
	if(len < 8 || get_u32(data) != GUARD_MAGIC
	   || get_u32(data + 4) != GUARD_VERSION) {
		/* M1: the deferred module truncates the journal before it writes the
		 * next epoch's header; 0-7 bytes (or anything that is not our header)
		 * therefore carries no INTENT and nothing of it was published. Inert,
		 * like a header-only file -- said once on stderr, never a refusal. */
		fprintf(stderr,
				"[server] %s: %s (%ld bytes) -- no checkpoint record; inert\n",
				CHECKPOINT_JOURNAL_GUARD_NAME,
				len < 8 ? "shorter than a header" : "foreign magic/version", n);
		free(data);
		v.inert = true;
		v.state = CHECKPOINT_JOURNAL_RESOLVED;
		return v;
	}
	bool intent = false, complete = false, recovered = false;
	size_t off = 8;
	while(off + 12 <= len) {
		uint32_t type = get_u32(data + off);
		uint32_t rlen = get_u32(data + off + 4);
		if(rlen > len - off - 12) {
			v.torn_tail = true;
			break;
		}
		uint32_t crc = crc32_update(0, data + off, 8);
		crc = crc32_update(crc, data + off + 8, rlen);
		if(crc != get_u32(data + off + 8 + rlen)) {
			v.torn_tail = true;
			break;
		}
		const uint8_t* payload = data + off + 8;
		if(type == REC_INTENT && rlen >= 4) {
			intent = true;
			complete = recovered = false;
			v.epoch = get_u32(payload);
		} else if(type == REC_COMPLETE && intent && rlen >= 4
				  && get_u32(payload) == v.epoch) {
			complete = true;
		} else if(type == REC_RECOVERED && intent && rlen >= 4
				  && get_u32(payload) == v.epoch) {
			recovered = true;
		}
		off += 12 + rlen;
	}
	if(off < len && !v.torn_tail)
		v.torn_tail = true; /* trailing bytes shorter than a record header */
	free(data);
	if(!intent)
		v.state = CHECKPOINT_JOURNAL_RESOLVED; /* header only / torn INTENT:
												* nothing was published */
	else
		v.state = (complete || recovered) ? CHECKPOINT_JOURNAL_RESOLVED
										  : CHECKPOINT_JOURNAL_UNRESOLVED;
	return v;
}

bool checkpoint_journal_allows_load(string_t root,
								   struct checkpoint_journal_verdict* out) {
	struct checkpoint_journal_verdict v = checkpoint_journal_inspect(root);
	if(out)
		*out = v;
	return v.state == CHECKPOINT_JOURNAL_NONE
		|| v.state == CHECKPOINT_JOURNAL_RESOLVED;
}

const char* checkpoint_journal_refusal_text(
	const struct checkpoint_journal_verdict* v) {
	if(!v)
		return "";
	switch(v->state) {
	case CHECKPOINT_JOURNAL_UNRESOLVED:
		return v->torn_tail ? "Save has an unfinished checkpoint (torn journal)"
							: "Save has an unfinished checkpoint journal";
	case CHECKPOINT_JOURNAL_MALFORMED:
		return "checkpoint.journal could not be read";
	default:
		return "";
	}
}
