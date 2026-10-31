/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Beta 1.7.3 protocol-14 P0 codec. Every body order is transcribed from the
 * 740c583 client decompile; the decoder remains resumable one byte at a time. */

#include "mp_codec.h"

/* W2: the MP compressed-body backing reservation (mp_backing_alloc / _free /
 * _owns). Only the Packet51 body path uses it; every other decoder
 * allocation stays on the ordinary heap. */
#include "../platform/mem2.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum decode_phase {
	DECODE_PACKET_ID,
	DECODE_FIXED_BODY,
	DECODE_TEXT_LENGTH,
	DECODE_TEXT_UNITS,
	DECODE_LOGIN_ENTITY,
	DECODE_LOGIN_SEED,
	DECODE_LOGIN_DIMENSION,
	DECODE_MAP_HEADER,
	DECODE_MAP_BODY,
	/* S2 P1 (world interaction/inventory). Place/WindowClick are
	 * client->server-only and have no decode phase -- see the dispatch note
	 * in mp_decoder_feed. */
	DECODE_ITEM_ID,
	DECODE_ITEM_REST,
	DECODE_MULTIBLOCK_HEADER,
	DECODE_MULTIBLOCK_BODY,
	DECODE_OPEN_WINDOW_PREFIX,
	DECODE_OPEN_WINDOW_TITLE_LEN,
	DECODE_OPEN_WINDOW_TITLE_BODY,
	DECODE_OPEN_WINDOW_SLOTCOUNT,
	DECODE_SET_SLOT_PREFIX,
	DECODE_WINDOW_ITEMS_HEADER,
	DECODE_SIGN_PREFIX,
	/* S3 P2 (entities/remote players). MP_PACKET_POSITION_LEGACY is
	 * C->S-only and never decoded -- see mp_decoder_feed's dispatch note. */
	DECODE_NAMED_SPAWN_ID,
	DECODE_NAMED_SPAWN_TAIL,
	DECODE_VEHICLE_HEADER,
	DECODE_VEHICLE_VELOCITY,
	DECODE_MOBSPAWN_HEADER,
	DECODE_PAINTING_ID,
	DECODE_PAINTING_TAIL,
	DECODE_ENTITY_METADATA_ID,
	/* DataWatcher sub-decode, shared by MOB_SPAWN and ENTITY_METADATA -- see
	 * decoder->metadata_active/metadata_target. [740c583
	 * DataWatcher.readWatchableObjects] */
	DECODE_METADATA_HEADER,
	DECODE_METADATA_VALUE,
	/* type 5 (ItemStack) only -- unconditional 5-byte read, deliberately NOT
	 * begin_item_stack/DECODE_ITEM_ID: DataWatcher's ItemStack has no Slot(-1)
	 * absent sentinel. [740c583 DataWatcher.readWatchableObjects case 5] */
	DECODE_METADATA_ITEM,
	/* S4 P3 (misc/polish). Chat's body is plain string16 (begin_text/
	 * DECODE_TEXT_LENGTH/DECODE_TEXT_UNITS, finished through finish_text
	 * below); every other P3 id gets a phase here. */
	DECODE_EXPLOSION_HEADER,
	DECODE_EXPLOSION_BODY,
	DECODE_MAPDATA_PREFIX,
	DECODE_MAPDATA_LEN,
	DECODE_MAPDATA_BODY,
};

static void* (*s_mp_alloc)(size_t) = malloc;
/* W2: whether a TEST has taken over the codec's allocations. It decides where
 * a Packet51 body comes from (mp_body_alloc below); every other allocation
 * uses s_mp_alloc unconditionally, as before. */
static bool s_mp_alloc_hooked = false;

void mp_codec_set_alloc_hook(void* (*alloc_fn)(size_t)) {
	s_mp_alloc = alloc_fn ? alloc_fn : malloc;
	s_mp_alloc_hooked = alloc_fn != NULL;
}

/* F5a (review finding H2): see struct mp_decoder::reclaim. Per decoder, so
 * two connections in one process cannot reclaim each other's reservation. */
void mp_decoder_set_reclaim_hook(struct mp_decoder* decoder,
								 bool (*reclaim_fn)(void* ctx, size_t wanted),
								 void* ctx) {
	if(!decoder)
		return;
	decoder->reclaim = reclaim_fn;
	decoder->reclaim_ctx = reclaim_fn ? ctx : NULL;
}

/* W2 (design section 2.4): where a compressed Packet51 body lives -- the MP
 * MEM2 backing reservation (platform/mem2.h), capacity-checked before it
 * hands anything out, never the MEM1 malloc heap this used to collapse (real
 * Wii betamc_0913, mem1_free_kb 16 at tick 15).
 *
 * The ONE exception is the codec's own fault-injection seam: a test that
 * installs an allocator through mp_codec_set_alloc_hook is asking exactly
 * THIS allocation to fail, and it has to keep being able to -- that hook
 * exists on this path for test_mp_s0_env's "Packet51 body allocation failure
 * is explicit, never a NULL sink write" case and nothing else. With no hook
 * installed -- every production build, always -- this is mp_backing_alloc and
 * nothing else. Either way the body is released through mp_body_free, whose
 * range test tells the two allocators apart. */
static void* mp_body_alloc(size_t length) {
	return s_mp_alloc_hooked ? s_mp_alloc(length) : mp_backing_alloc(length);
}

/* rd_u8: the ONLY way the fixed-width readers below may touch wire bytes.
 * GCC's bswap/load-merge pass recognizes big-endian byte assembly and fuses
 * it into ONE wide load; on Broadway the read_f64 chain became `lfd` straight
 * off the byte-packed wire buffer (take 48's SRR0 bar-decode landed on
 * parse_fixed's fused load, 0x800c743c-class), and a non-word-aligned lfd
 * raises the PPC alignment exception that froze every mp_connect take --
 * takes 1-48, the "wandering victim": each build moved which fused load was
 * misaligned. volatile forbids the merge: byte loads stay byte loads. The
 * codec goldens pin that the assembled VALUES are unchanged. (S5.3 root
 * cause, 2026-08-29.) */
static uint8_t rd_u8(const uint8_t* p) {
	return *(const volatile uint8_t*)p;
}

static int16_t read_i16(const uint8_t* p) {
	uint16_t value = ((uint16_t)rd_u8(p) << 8) | rd_u8(p + 1);
	return value <= INT16_MAX ? (int16_t)value
		: (int16_t)(-1 - (int16_t)(UINT16_MAX - value));
}

static uint16_t read_u16(const uint8_t* p) {
	return ((uint16_t)p[0] << 8) | p[1];
}

/* Java DataInputStream.readUTF's "modified UTF-8": 1/2/3-byte forms with NUL
 * encoded as 0xC0 0x80 and an astral character split into two 3-byte
 * surrogate halves (CESU-8), never a native 4-byte UTF-8 sequence. Decode to
 * a UTF-16 code-unit buffer first, then hand off to mp_utf16_to_utf8 so the
 * existing, already-tested surrogate-pairing logic runs exactly once.
 * [740c583 Packet100OpenWindow.readPacketData] */
static bool modified_utf8_to_utf8(const uint8_t* bytes, size_t length,
									 char* out, size_t out_cap) {
	struct mp_utf16 text;
	text.length = 0;
	size_t i = 0;
	while(i < length) {
		uint8_t b0 = bytes[i];
		uint16_t cp;
		if((b0 & 0x80) == 0) {
			cp = b0;
			i += 1;
		} else if((b0 & 0xE0) == 0xC0 && i + 1 < length
				  && (bytes[i + 1] & 0xC0) == 0x80) {
			cp = ((uint16_t)(b0 & 0x1F) << 6) | (bytes[i + 1] & 0x3F);
			i += 2;
		} else if((b0 & 0xF0) == 0xE0 && i + 2 < length
				  && (bytes[i + 1] & 0xC0) == 0x80
				  && (bytes[i + 2] & 0xC0) == 0x80) {
			cp = ((uint16_t)(b0 & 0x0F) << 12)
				| ((uint16_t)(bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F);
			i += 3;
		} else
			return false;
		if(text.length >= MP_TEXT_CAPACITY)
			return false;
		text.units[text.length++] = cp;
	}
	return mp_utf16_to_utf8(&text, out, out_cap);
}

static int32_t read_i32(const uint8_t* p) {
	uint32_t value = ((uint32_t)rd_u8(p) << 24) | ((uint32_t)rd_u8(p + 1) << 16)
		| ((uint32_t)rd_u8(p + 2) << 8) | rd_u8(p + 3);
	return value <= INT32_MAX ? (int32_t)value
		: -1 - (int32_t)(UINT32_MAX - value);
}

static int64_t read_i64(const uint8_t* p) {
	uint64_t value = ((uint64_t)rd_u8(p) << 56) | ((uint64_t)rd_u8(p + 1) << 48)
		| ((uint64_t)rd_u8(p + 2) << 40) | ((uint64_t)rd_u8(p + 3) << 32)
		| ((uint64_t)rd_u8(p + 4) << 24) | ((uint64_t)rd_u8(p + 5) << 16)
		| ((uint64_t)rd_u8(p + 6) << 8) | rd_u8(p + 7);
	return value <= INT64_MAX ? (int64_t)value
		: -1 - (int64_t)(UINT64_MAX - value);
}

static float read_f32(const uint8_t* p) {
	uint32_t bits = (uint32_t)read_i32(p);
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static double read_f64(const uint8_t* p) {
	uint64_t bits = (uint64_t)read_i64(p);
	double value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static void decoder_reset_packet(struct mp_decoder* decoder) {
	decoder->packet_id = 0;
	decoder->phase = DECODE_PACKET_ID;
	decoder->scalar_have = decoder->scalar_need = 0;
	decoder->text.length = decoder->text_max = 0;
	decoder->text_byte = 0;
	decoder->have_text_high = false;
	decoder->chunk_have = decoder->chunk_need = 0;
	memset(&decoder->item_scratch, 0, sizeof(decoder->item_scratch));
	decoder->array_index = decoder->array_count = 0;
	decoder->sign_line = 0;
	decoder->title_have = decoder->title_need = 0;
	decoder->metadata_active = false;
	decoder->metadata_target = NULL;
	memset(&decoder->event, 0, sizeof(decoder->event));
}

/* Frees any owned allocation that a partially-decoded S2 P1 packet may hold
 * outside decoder->chunk_body (which decoder_error/mp_decoder_destroy already
 * free generically). Packet104's per-entry loop is the only case: the array
 * is allocated whole at the header and released entry-by-entry into the
 * event as decoding progresses, so an abort mid-array must not leak it. */
static void decoder_release_partial(struct mp_decoder* decoder) {
	if(decoder->packet_id == MP_PACKET_WINDOW_ITEMS) {
		free(decoder->event.payload.window_items.items);
		decoder->event.payload.window_items.items = NULL;
	}
}

void mp_decoder_init(struct mp_decoder* decoder) {
	if(!decoder)
		return;
	memset(decoder, 0, sizeof(*decoder));
	decoder_reset_packet(decoder);
}

/* W2 (mp-split-0912): a Packet51 compressed body is allocated from the MP
 * MEM2 backing reservation (platform/mem2.h, design section 2.4), while the
 * multi-block and explosion bodies that share `decoder->chunk_body` -- and
 * every body of a decoder that ran before a reservation existed -- are plain
 * malloc. The range test is what keeps those two allocators from ever
 * crossing: a reservation alloc paired with a plain free() (or the reverse)
 * is heap corruption, not a leak. Used at EVERY site that can free a map
 * body: mp_decoder_destroy and decoder_error (mid-body teardown) and
 * mp_event_destroy (an emitted event the consumer drops). */
static void mp_body_free(void* body) {
	if(!body)
		return;
	if(mp_backing_owns(body)) {
		mp_backing_free(body);
		return;
	}
	free(body);
}

void mp_decoder_destroy(struct mp_decoder* decoder) {
	if(!decoder)
		return;
	mp_body_free(decoder->chunk_body);
	decoder->chunk_body = NULL;
	decoder_release_partial(decoder);
	decoder_reset_packet(decoder);
}

void mp_event_destroy(struct mp_event* event) {
	if(!event)
		return;
	if(event->type == MP_EVENT_MAP_CHUNK)
		mp_body_free(event->payload.map_chunk.compressed);
	if(event->type == MP_EVENT_MULTI_BLOCK_CHANGE) {
		free(event->payload.multi_block_change.coordinate);
		free(event->payload.multi_block_change.type);
		free(event->payload.multi_block_change.metadata);
	}
	if(event->type == MP_EVENT_WINDOW_ITEMS)
		free(event->payload.window_items.items);
	if(event->type == MP_EVENT_EXPLOSION)
		free(event->payload.explosion.blocks);
	memset(event, 0, sizeof(*event));
}

static enum mp_decode_result decoder_error(struct mp_decoder* decoder) {
	decoder->error_packet_id = decoder->packet_id;
	decoder->error_phase = decoder->phase;
	mp_body_free(decoder->chunk_body);
	decoder->chunk_body = NULL;
	decoder_release_partial(decoder);
	decoder_reset_packet(decoder);
	return MP_DECODE_ERROR;
}

static enum mp_decode_result emit_event(struct mp_decoder* decoder,
										struct mp_event* event_out) {
	*event_out = decoder->event;
	/* Packet51 ownership transfers to the emitted event. */
	decoder->chunk_body = NULL;
	decoder_reset_packet(decoder);
	return MP_DECODE_EVENT;
}

static enum mp_decode_result begin_scalar(struct mp_decoder* decoder,
										 enum decode_phase phase, size_t bytes) {
	decoder->scalar_have = 0;
	decoder->scalar_need = bytes;
	decoder->phase = (uint8_t)phase;
	return MP_DECODE_MORE;
}

static enum mp_decode_result begin_text(struct mp_decoder* decoder,
										size_t maximum) {
	decoder->text.length = 0;
	decoder->text_max = maximum;
	return begin_scalar(decoder, DECODE_TEXT_LENGTH, 2);
}

/* Forward declaration: metadata's string (type 4) sub-decode finishes
 * through finish_text, defined further down with the rest of the DataWatcher
 * machinery. */
static enum mp_decode_result finish_metadata_entry(struct mp_decoder* decoder);

static enum mp_decode_result finish_text(struct mp_decoder* decoder,
										 struct mp_event* event_out) {
	if(decoder->metadata_active) {
		decoder->metadata_active = false;
		return finish_metadata_entry(decoder);
	}
	if(decoder->packet_id == MP_PACKET_NAMED_ENTITY_SPAWN) {
		decoder->event.payload.named_entity_spawn.name = decoder->text;
		return begin_scalar(decoder, DECODE_NAMED_SPAWN_TAIL, 16);
	}
	if(decoder->packet_id == MP_PACKET_ENTITY_PAINTING) {
		decoder->event.payload.entity_painting.title = decoder->text;
		return begin_scalar(decoder, DECODE_PAINTING_TAIL, 16);
	}
	if(decoder->packet_id == MP_PACKET_HANDSHAKE) {
		decoder->event.type = MP_EVENT_HANDSHAKE;
		decoder->event.payload.handshake = decoder->text;
		return emit_event(decoder, event_out);
	}
	if(decoder->packet_id == MP_PACKET_DISCONNECT) {
		decoder->event.type = MP_EVENT_DISCONNECT;
		decoder->event.payload.disconnect_reason = decoder->text;
		return emit_event(decoder, event_out);
	}
	if(decoder->packet_id == MP_PACKET_CHAT) {
		decoder->event.type = MP_EVENT_CHAT;
		decoder->event.payload.chat = decoder->text;
		return emit_event(decoder, event_out);
	}
	if(decoder->packet_id == MP_PACKET_UPDATE_SIGN) {
		/* Four string16 lines in a fixed loop. [740c583
		 * Packet130UpdateSign.readPacketData] */
		decoder->event.payload.update_sign.lines[decoder->sign_line] = decoder->text;
		decoder->sign_line++;
		if(decoder->sign_line < MP_SIGN_LINE_COUNT)
			return begin_text(decoder, MP_SIGN_LINE_MAX);
		decoder->event.type = MP_EVENT_UPDATE_SIGN;
		return emit_event(decoder, event_out);
	}
	decoder->event.payload.login.username = decoder->text;
	return begin_scalar(decoder, DECODE_LOGIN_SEED, 8);
}

static enum mp_decode_result begin_item_stack(struct mp_decoder* decoder) {
	memset(&decoder->item_scratch, 0, sizeof(decoder->item_scratch));
	return begin_scalar(decoder, DECODE_ITEM_ID, 2);
}

/* Dispatches a completed ItemStack sub-decode to whichever P1 packet owns it.
 * Packet104 loops back for its next entry instead of emitting. Place and
 * WindowClick also use this ItemStack shape but are client->server-only, so
 * they never reach here -- see the dispatch note in mp_decoder_feed.
 * NEVER reached for DataWatcher type 5 -- that has its own unconditional
 * DECODE_METADATA_ITEM read (see the decode_phase comment), specifically
 * because this Slot-shaped path's DECODE_ITEM_ID treats a negative id as
 * "absent, no more bytes", which is wrong for DataWatcher's ItemStack (always
 * fully present on the wire). */
static enum mp_decode_result finish_item(struct mp_decoder* decoder,
										 struct mp_event* event_out) {
	switch(decoder->packet_id) {
		case MP_PACKET_SET_SLOT:
			decoder->event.payload.set_slot.item = decoder->item_scratch;
			decoder->event.type = MP_EVENT_SET_SLOT;
			return emit_event(decoder, event_out);
		case MP_PACKET_WINDOW_ITEMS:
			decoder->event.payload.window_items.items[decoder->array_index]
				= decoder->item_scratch;
			decoder->array_index++;
			if(decoder->array_index < decoder->array_count)
				return begin_item_stack(decoder);
			decoder->event.type = MP_EVENT_WINDOW_ITEMS;
			return emit_event(decoder, event_out);
		default:
			return decoder_error(decoder);
	}
}

static enum mp_decode_result finish_multiblock(struct mp_decoder* decoder,
												struct mp_event* event_out) {
	struct mp_multi_block_change* change
		= &decoder->event.payload.multi_block_change;
	uint16_t count = change->count;
	if(count == 0) {
		decoder->event.type = MP_EVENT_MULTI_BLOCK_CHANGE;
		return emit_event(decoder, event_out);
	}
	uint16_t* coordinate = s_mp_alloc((size_t)count * sizeof(uint16_t));
	uint8_t* type = s_mp_alloc((size_t)count);
	uint8_t* metadata = s_mp_alloc((size_t)count);
	if(!coordinate || !type || !metadata) {
		free(coordinate);
		free(type);
		free(metadata);
		decoder->allocation_failed = true;
		return decoder_error(decoder);
	}
	const uint8_t* raw = decoder->chunk_body;
	for(uint16_t i = 0; i < count; i++)
		coordinate[i] = read_u16(raw + (size_t)i * 2);
	memcpy(type, raw + (size_t)count * 2, count);
	memcpy(metadata, raw + (size_t)count * 3, count);
	free(decoder->chunk_body);
	decoder->chunk_body = NULL;
	change->coordinate = coordinate;
	change->type = type;
	change->metadata = metadata;
	decoder->event.type = MP_EVENT_MULTI_BLOCK_CHANGE;
	return emit_event(decoder, event_out);
}

/* Assembles the final owned mp_explosion_offset[] from the raw 3-bytes-per-
 * entry scratch buffer DECODE_EXPLOSION_BODY filled, mirroring
 * finish_multiblock's own scratch-to-owned-array shape. [740c583
 * Packet60Explosion.readPacketData] */
static enum mp_decode_result finish_explosion(struct mp_decoder* decoder,
											  struct mp_event* event_out) {
	uint16_t count = decoder->event.payload.explosion.count;
	struct mp_explosion_offset* blocks
		= s_mp_alloc((size_t)count * sizeof(struct mp_explosion_offset));
	if(!blocks) {
		decoder->allocation_failed = true;
		return decoder_error(decoder);
	}
	const uint8_t* raw = decoder->chunk_body;
	for(uint16_t i = 0; i < count; i++) {
		blocks[i].dx = (int8_t)raw[(size_t)i * 3 + 0];
		blocks[i].dy = (int8_t)raw[(size_t)i * 3 + 1];
		blocks[i].dz = (int8_t)raw[(size_t)i * 3 + 2];
	}
	free(decoder->chunk_body);
	decoder->chunk_body = NULL;
	decoder->event.payload.explosion.blocks = blocks;
	decoder->event.type = MP_EVENT_EXPLOSION;
	return emit_event(decoder, event_out);
}

/* Stores the just-finished entry's value (scalar types land here directly;
 * type 4 routes in via finish_text above; type 5 has its own
 * DECODE_METADATA_ITEM phase, not this function -- see that phase's comment),
 * advances count, and returns to the header phase for the next entry (or the
 * 0x7F terminator). [740c583 DataWatcher.readWatchableObjects] */
static enum mp_decode_result finish_metadata_entry(struct mp_decoder* decoder) {
	struct mp_metadata* m = decoder->metadata_target;
	struct mp_watchable_entry* entry = &m->entries[m->count];
	if(entry->type == 4)
		entry->value.string_value = decoder->text;
	m->count++;
	decoder->phase = DECODE_METADATA_HEADER;
	return MP_DECODE_MORE;
}

/* One DataWatcher header byte: (type<<5 | index&31), or the literal 0x7F
 * terminator. Bounds every hostile axis explicitly: unknown type (>6) is
 * rejected, and the entry count is capped at MP_METADATA_MAX_ENTRIES before
 * ever allocating/storing a value -- there is no wire count field to trust
 * instead. [740c583 DataWatcher.readWatchableObjects] */
static enum mp_decode_result handle_metadata_header(struct mp_decoder* decoder,
													uint8_t byte,
													struct mp_event* event_out) {
	if(byte == 0x7F) {
		if(decoder->packet_id == MP_PACKET_MOB_SPAWN) {
			decoder->event.type = MP_EVENT_MOB_SPAWN;
			return emit_event(decoder, event_out);
		}
		if(decoder->packet_id == MP_PACKET_ENTITY_METADATA) {
			decoder->event.type = MP_EVENT_ENTITY_METADATA;
			return emit_event(decoder, event_out);
		}
		return decoder_error(decoder);
	}
	uint8_t type = (uint8_t)((byte >> 5) & 0x07);
	uint8_t index = (uint8_t)(byte & 0x1F);
	if(type > 6)
		return decoder_error(decoder); /* unknown-type rejection */
	struct mp_metadata* m = decoder->metadata_target;
	if(!m || m->count >= MP_METADATA_MAX_ENTRIES)
		return decoder_error(decoder); /* oversized-count rejection */
	struct mp_watchable_entry* entry = &m->entries[m->count];
	memset(entry, 0, sizeof(*entry));
	entry->type = type;
	entry->index = index;
	switch(type) {
		case 0: return begin_scalar(decoder, DECODE_METADATA_VALUE, 1);
		case 1: return begin_scalar(decoder, DECODE_METADATA_VALUE, 2);
		case 2: return begin_scalar(decoder, DECODE_METADATA_VALUE, 4);
		case 3: return begin_scalar(decoder, DECODE_METADATA_VALUE, 4);
		case 4:
			decoder->metadata_active = true;
			return begin_text(decoder, MP_METADATA_STRING_MAX);
		/* type 5 (ItemStack): NOT begin_item_stack -- that shares
		 * DECODE_ITEM_ID's Slot(-1) "absent" sentinel with SetSlot/
		 * WindowItems, but DataWatcher's own ItemStack read
		 * (id/count/damage, 5 bytes) is unconditional, with no absent case,
		 * so a legitimate-but-high (>=0x8000, i.e. negative as int16) item id
		 * must not truncate the read and desync the stream. [740c583
		 * DataWatcher.readWatchableObjects case 5] */
		case 5: return begin_scalar(decoder, DECODE_METADATA_ITEM, 5);
		case 6: return begin_scalar(decoder, DECODE_METADATA_VALUE, 12);
		default: return decoder_error(decoder); /* unreachable: type>6 above */
	}
}

bool mp_packet_supported(uint8_t id, enum mp_direction direction) {
	switch(id) {
		case MP_PACKET_KEEP_ALIVE:
		case MP_PACKET_LOGIN:
		case MP_PACKET_HANDSHAKE:
		case MP_PACKET_FLYING:
		case MP_PACKET_PLAYER_POSITION:
		case MP_PACKET_PLAYER_LOOK:
		case MP_PACKET_PLAYER_LOOK_MOVE:
		case MP_PACKET_DISCONNECT:
		/* Bidirectional P1 [740c583 Packet]. */
		case MP_PACKET_RESPAWN:
		case MP_PACKET_CLOSE_WINDOW:
		case MP_PACKET_TRANSACTION:
		case MP_PACKET_UPDATE_SIGN:
		/* Bidirectional P2 [740c583 Packet]. */
		case MP_PACKET_ANIMATION:
		/* Bidirectional P3: the only one [740c583 Packet]. */
		case MP_PACKET_CHAT:
			return true;
		case MP_PACKET_UPDATE_TIME:
		case MP_PACKET_SPAWN_POSITION:
		case MP_PACKET_UPDATE_HEALTH:
		case MP_PACKET_PRE_CHUNK:
		case MP_PACKET_MAP_CHUNK:
		/* Server->client-only P1 [740c583 Packet]. */
		case MP_PACKET_PLAYER_INVENTORY:
		case MP_PACKET_MULTI_BLOCK_CHANGE:
		case MP_PACKET_BLOCK_CHANGE:
		case MP_PACKET_OPEN_WINDOW:
		case MP_PACKET_SET_SLOT:
		case MP_PACKET_WINDOW_ITEMS:
		case MP_PACKET_UPDATE_PROGRESSBAR:
		/* Server->client-only P2 [740c583 Packet]. */
		case MP_PACKET_SLEEP:
		case MP_PACKET_NAMED_ENTITY_SPAWN:
		case MP_PACKET_PICKUP_SPAWN:
		case MP_PACKET_COLLECT:
		case MP_PACKET_VEHICLE_SPAWN:
		case MP_PACKET_MOB_SPAWN:
		case MP_PACKET_ENTITY_PAINTING:
		case MP_PACKET_ENTITY_VELOCITY:
		case MP_PACKET_DESTROY_ENTITY:
		case MP_PACKET_ENTITY:
		case MP_PACKET_REL_ENTITY_MOVE:
		case MP_PACKET_ENTITY_LOOK:
		case MP_PACKET_REL_ENTITY_MOVE_LOOK:
		case MP_PACKET_ENTITY_TELEPORT:
		case MP_PACKET_ENTITY_STATUS:
		case MP_PACKET_ATTACH_ENTITY:
		case MP_PACKET_ENTITY_METADATA:
		/* Server->client-only P3 [740c583 Packet]. */
		case MP_PACKET_NOTE_BLOCK:
		case MP_PACKET_EXPLOSION:
		case MP_PACKET_DOOR_CHANGE:
		case MP_PACKET_BED:
		case MP_PACKET_WEATHER:
		case MP_PACKET_MAP_DATA:
		case MP_PACKET_STATISTIC:
			return direction == MP_DIRECTION_SERVER_TO_CLIENT;
		/* Client->server-only P1 [740c583 Packet]. */
		case MP_PACKET_USE_ENTITY:
		case MP_PACKET_BLOCK_DIG:
		case MP_PACKET_PLACE:
		case MP_PACKET_BLOCK_ITEM_SWITCH:
		case MP_PACKET_ENTITY_ACTION:
		case MP_PACKET_WINDOW_CLICK:
		/* Client->server-only P2: registered but never actually sent by any
		 * real Beta client -- see MP_PACKET_POSITION_LEGACY's comment.
		 * [740c583 Packet.addIdClassMapping] */
		case MP_PACKET_POSITION_LEGACY:
			return direction == MP_DIRECTION_CLIENT_TO_SERVER;
		default:
			return false;
	}
}

static enum mp_decode_result parse_fixed(struct mp_decoder* decoder,
										struct mp_event* event_out) {
	const uint8_t* p = decoder->scalar;
	struct mp_movement* movement = &decoder->event.payload.movement;
	switch(decoder->packet_id) {
		case MP_PACKET_UPDATE_TIME:
			decoder->event.type = MP_EVENT_UPDATE_TIME;
			decoder->event.payload.world_time = read_i64(p);
			break;
		case MP_PACKET_SPAWN_POSITION:
			decoder->event.type = MP_EVENT_SPAWN_POSITION;
			decoder->event.payload.spawn_position.x = read_i32(p);
			decoder->event.payload.spawn_position.y = read_i32(p + 4);
			decoder->event.payload.spawn_position.z = read_i32(p + 8);
			break;
		case MP_PACKET_UPDATE_HEALTH:
			decoder->event.type = MP_EVENT_UPDATE_HEALTH;
			decoder->event.payload.health = read_i16(p);
			break;
		case MP_PACKET_FLYING:
			decoder->event.type = MP_EVENT_FLYING;
			movement->on_ground = p[0] != 0;
			break;
		case MP_PACKET_PLAYER_POSITION:
			decoder->event.type = MP_EVENT_PLAYER_POSITION;
			movement->moving = true;
			movement->x = read_f64(p);
			movement->y = read_f64(p + 8);
			movement->stance = read_f64(p + 16);
			movement->z = read_f64(p + 24);
			movement->on_ground = p[32] != 0;
			break;
		case MP_PACKET_PLAYER_LOOK:
			decoder->event.type = MP_EVENT_PLAYER_LOOK;
			movement->rotating = true;
			movement->yaw = read_f32(p);
			movement->pitch = read_f32(p + 4);
			movement->on_ground = p[8] != 0;
			break;
		case MP_PACKET_PLAYER_LOOK_MOVE:
			decoder->event.type = MP_EVENT_PLAYER_LOOK_MOVE;
			movement->moving = movement->rotating = true;
			/* Ordering trap: x,y,stance,z (not x,stance,y,z).
			 * [740c583 Packet13PlayerLookMove.readPacketData] */
			movement->x = read_f64(p);
			movement->y = read_f64(p + 8);
			movement->stance = read_f64(p + 16);
			movement->z = read_f64(p + 24);
			movement->yaw = read_f32(p + 32);
			movement->pitch = read_f32(p + 36);
			movement->on_ground = p[40] != 0;
			break;
		case MP_PACKET_PRE_CHUNK:
			decoder->event.type = MP_EVENT_PRE_CHUNK;
			decoder->event.payload.pre_chunk.chunk_x = read_i32(p);
			decoder->event.payload.pre_chunk.chunk_z = read_i32(p + 4);
			decoder->event.payload.pre_chunk.mode = p[8] != 0;
			break;
		case MP_PACKET_PLAYER_INVENTORY:
			decoder->event.type = MP_EVENT_PLAYER_INVENTORY;
			decoder->event.payload.player_inventory.entity_id = read_i32(p);
			decoder->event.payload.player_inventory.slot = read_i16(p + 4);
			decoder->event.payload.player_inventory.item_id = read_i16(p + 6);
			decoder->event.payload.player_inventory.item_damage = read_i16(p + 8);
			break;
		case MP_PACKET_RESPAWN:
			decoder->event.type = MP_EVENT_RESPAWN;
			decoder->event.payload.respawn_dimension = (int8_t)p[0];
			break;
		case MP_PACKET_BLOCK_CHANGE:
			decoder->event.type = MP_EVENT_BLOCK_CHANGE;
			decoder->event.payload.block_change.x = read_i32(p);
			decoder->event.payload.block_change.y = p[4];
			decoder->event.payload.block_change.z = read_i32(p + 5);
			decoder->event.payload.block_change.block_type = p[9];
			decoder->event.payload.block_change.metadata = p[10];
			break;
		case MP_PACKET_CLOSE_WINDOW:
			decoder->event.type = MP_EVENT_CLOSE_WINDOW;
			decoder->event.payload.close_window = (int8_t)p[0];
			break;
		case MP_PACKET_UPDATE_PROGRESSBAR:
			decoder->event.type = MP_EVENT_UPDATE_PROGRESSBAR;
			decoder->event.payload.update_progressbar.window = (int8_t)p[0];
			decoder->event.payload.update_progressbar.bar = read_i16(p + 1);
			decoder->event.payload.update_progressbar.value = read_i16(p + 3);
			break;
		case MP_PACKET_TRANSACTION:
			decoder->event.type = MP_EVENT_TRANSACTION;
			decoder->event.payload.transaction.window = (int8_t)p[0];
			decoder->event.payload.transaction.action = read_i16(p + 1);
			decoder->event.payload.transaction.accepted = p[3] != 0;
			break;
		/* S3 P2: every fully-fixed, unconditional-emit body lands here, same
		 * as the P0/P1 cases above. */
		case MP_PACKET_SLEEP:
			/* entityID(i32), unknown(i8), bedX(i32), bedY(i8), bedZ(i32) --
			 * read/write order, NOT field declaration order. [740c583
			 * Packet17Sleep.readPacketData] */
			decoder->event.type = MP_EVENT_SLEEP;
			decoder->event.payload.sleep.entity_id = read_i32(p);
			decoder->event.payload.sleep.unknown = (int8_t)p[4];
			decoder->event.payload.sleep.bed_x = read_i32(p + 5);
			decoder->event.payload.sleep.bed_y = (int8_t)p[9];
			decoder->event.payload.sleep.bed_z = read_i32(p + 10);
			break;
		case MP_PACKET_ANIMATION:
			decoder->event.type = MP_EVENT_ANIMATION;
			decoder->event.payload.animation.entity_id = read_i32(p);
			decoder->event.payload.animation.animate = p[4];
			break;
		case MP_PACKET_PICKUP_SPAWN:
			/* [740c583 Packet21PickupSpawn.readPacketData] */
			decoder->event.type = MP_EVENT_PICKUP_SPAWN;
			decoder->event.payload.pickup_spawn.entity_id = read_i32(p);
			decoder->event.payload.pickup_spawn.item_id = read_i16(p + 4);
			decoder->event.payload.pickup_spawn.count = p[6];
			decoder->event.payload.pickup_spawn.item_damage = read_i16(p + 7);
			decoder->event.payload.pickup_spawn.x = read_i32(p + 9);
			decoder->event.payload.pickup_spawn.y = read_i32(p + 13);
			decoder->event.payload.pickup_spawn.z = read_i32(p + 17);
			decoder->event.payload.pickup_spawn.vel_x = (int8_t)p[21];
			decoder->event.payload.pickup_spawn.vel_y = (int8_t)p[22];
			decoder->event.payload.pickup_spawn.vel_z = (int8_t)p[23];
			break;
		case MP_PACKET_COLLECT:
			decoder->event.type = MP_EVENT_COLLECT;
			decoder->event.payload.collect.collected_entity_id = read_i32(p);
			decoder->event.payload.collect.collector_entity_id
				= read_i32(p + 4);
			break;
		case MP_PACKET_ENTITY_VELOCITY:
			decoder->event.type = MP_EVENT_ENTITY_VELOCITY;
			decoder->event.payload.entity_velocity.entity_id = read_i32(p);
			decoder->event.payload.entity_velocity.vel_x = read_i16(p + 4);
			decoder->event.payload.entity_velocity.vel_y = read_i16(p + 6);
			decoder->event.payload.entity_velocity.vel_z = read_i16(p + 8);
			break;
		case MP_PACKET_DESTROY_ENTITY:
			decoder->event.type = MP_EVENT_DESTROY_ENTITY;
			decoder->event.payload.destroy_entity.entity_id = read_i32(p);
			break;
		case MP_PACKET_ENTITY:
			/* Packet30Entity's OWN readPacketData reads only entityId; the
			 * xPosition/yPosition/zPosition/yaw/pitch/rotating fields exist
			 * on the base class only for its subclasses (31/32/33) to fill
			 * in after calling super. [740c583 Packet30Entity.readPacketData] */
			decoder->event.type = MP_EVENT_ENTITY;
			decoder->event.payload.entity_heartbeat.entity_id = read_i32(p);
			break;
		case MP_PACKET_REL_ENTITY_MOVE:
			decoder->event.type = MP_EVENT_REL_ENTITY_MOVE;
			decoder->event.payload.rel_entity_move.entity_id = read_i32(p);
			decoder->event.payload.rel_entity_move.dx = (int8_t)p[4];
			decoder->event.payload.rel_entity_move.dy = (int8_t)p[5];
			decoder->event.payload.rel_entity_move.dz = (int8_t)p[6];
			break;
		case MP_PACKET_ENTITY_LOOK:
			decoder->event.type = MP_EVENT_ENTITY_LOOK;
			decoder->event.payload.entity_look.entity_id = read_i32(p);
			decoder->event.payload.entity_look.yaw = (int8_t)p[4];
			decoder->event.payload.entity_look.pitch = (int8_t)p[5];
			break;
		case MP_PACKET_REL_ENTITY_MOVE_LOOK:
			decoder->event.type = MP_EVENT_REL_ENTITY_MOVE_LOOK;
			decoder->event.payload.rel_entity_move_look.entity_id = read_i32(p);
			decoder->event.payload.rel_entity_move_look.dx = (int8_t)p[4];
			decoder->event.payload.rel_entity_move_look.dy = (int8_t)p[5];
			decoder->event.payload.rel_entity_move_look.dz = (int8_t)p[6];
			decoder->event.payload.rel_entity_move_look.yaw = (int8_t)p[7];
			decoder->event.payload.rel_entity_move_look.pitch = (int8_t)p[8];
			break;
		case MP_PACKET_ENTITY_TELEPORT:
			/* entityId(i32), x/y/z(i32), yaw(i8 via .read()), pitch(i8 via
			 * .read()) -- .read() and .readByte() land the same byte on the
			 * wire. [740c583 Packet34EntityTeleport.readPacketData] */
			decoder->event.type = MP_EVENT_ENTITY_TELEPORT;
			decoder->event.payload.entity_teleport.entity_id = read_i32(p);
			decoder->event.payload.entity_teleport.x = read_i32(p + 4);
			decoder->event.payload.entity_teleport.y = read_i32(p + 8);
			decoder->event.payload.entity_teleport.z = read_i32(p + 12);
			decoder->event.payload.entity_teleport.yaw = (int8_t)p[16];
			decoder->event.payload.entity_teleport.pitch = (int8_t)p[17];
			break;
		case MP_PACKET_ENTITY_STATUS:
			decoder->event.type = MP_EVENT_ENTITY_STATUS;
			decoder->event.payload.entity_status.entity_id = read_i32(p);
			decoder->event.payload.entity_status.status = (int8_t)p[4];
			break;
		case MP_PACKET_ATTACH_ENTITY:
			decoder->event.type = MP_EVENT_ATTACH_ENTITY;
			decoder->event.payload.attach_entity.entity_id = read_i32(p);
			decoder->event.payload.attach_entity.vehicle_entity_id
				= read_i32(p + 4);
			break;
		/* S4 P3: every fully-fixed, unconditional-emit body lands here too. */
		case MP_PACKET_NOTE_BLOCK:
			/* xLocation(i32); yLocation(i16); zLocation(i32);
			 * instrumentType(u8); pitch(u8). [740c583
			 * Packet54PlayNoteBlock.readPacketData] */
			decoder->event.type = MP_EVENT_NOTE_BLOCK;
			decoder->event.payload.note_block.x = read_i32(p);
			decoder->event.payload.note_block.y = read_i16(p + 4);
			decoder->event.payload.note_block.z = read_i32(p + 6);
			decoder->event.payload.note_block.instrument = p[10];
			decoder->event.payload.note_block.pitch = p[11];
			break;
		case MP_PACKET_DOOR_CHANGE:
			/* Read/write order: effectId(i32), x(i32), y(i8), z(i32), data(i32).
			 * [740c583 Packet61DoorChange.readPacketData] */
			decoder->event.type = MP_EVENT_DOOR_CHANGE;
			decoder->event.payload.door_change.effect_id = read_i32(p);
			decoder->event.payload.door_change.x = read_i32(p + 4);
			decoder->event.payload.door_change.y = (int8_t)p[8];
			decoder->event.payload.door_change.z = read_i32(p + 9);
			decoder->event.payload.door_change.data = read_i32(p + 13);
			break;
		case MP_PACKET_BED:
			decoder->event.type = MP_EVENT_BED;
			decoder->event.payload.bed.event_code = (int8_t)p[0];
			break;
		case MP_PACKET_WEATHER:
			/* entityID(i32); isLightningBolt(i8); posX/Y/Z(i32). [740c583
			 * Packet71Weather.readPacketData] */
			decoder->event.type = MP_EVENT_WEATHER;
			decoder->event.payload.weather.entity_id = read_i32(p);
			decoder->event.payload.weather.is_lightning_bolt = (int8_t)p[4];
			decoder->event.payload.weather.x = read_i32(p + 5);
			decoder->event.payload.weather.y = read_i32(p + 9);
			decoder->event.payload.weather.z = read_i32(p + 13);
			break;
		case MP_PACKET_STATISTIC:
			decoder->event.type = MP_EVENT_STATISTIC;
			decoder->event.payload.statistic.stat_id = read_i32(p);
			decoder->event.payload.statistic.amount = (int8_t)p[4];
			break;
		default:
			return decoder_error(decoder);
	}
	return emit_event(decoder, event_out);
}

static enum mp_decode_result parse_map_header(struct mp_decoder* decoder,
											 struct mp_event* event_out) {
	const uint8_t* p = decoder->scalar;
	struct mp_map_chunk* chunk = &decoder->event.payload.map_chunk;
	chunk->x = read_i32(p);
	chunk->y = read_i16(p + 4);
	chunk->z = read_i32(p + 6);
	chunk->sx = (uint16_t)p[10] + 1u;
	chunk->sy = (uint16_t)p[11] + 1u;
	chunk->sz = (uint16_t)p[12] + 1u;
	int32_t compressed = read_i32(p + 13);
	/* Packet51 dimensions are bounded by one Beta chunk column. Validate every
	 * sum/product before allocating network-controlled lengths. [740c583
	 * Packet51MapChunk.readPacketData] */
	if(chunk->sx > 16 || chunk->sy > 128 || chunk->sz > 16 || chunk->y < 0
	   || (uint32_t)chunk->y + chunk->sy > 128 || compressed <= 0
	   || (uint32_t)compressed > MP_CHUNK_COMPRESSED_CAP)
		return decoder_error(decoder);
	int64_t x_end = (int64_t)chunk->x + chunk->sx - 1;
	int64_t z_end = (int64_t)chunk->z + chunk->sz - 1;
	if(x_end > INT32_MAX || x_end < INT32_MIN
	   || z_end > INT32_MAX || z_end < INT32_MIN)
		return decoder_error(decoder);
	size_t cells = (size_t)chunk->sx * chunk->sy * chunk->sz;
	if(cells > (size_t)16 * 128 * 16 || cells > SIZE_MAX / 5
	   || (cells & 1u) != 0)
		return decoder_error(decoder);
	chunk->inflated_length = cells * 5 / 2;
	if(chunk->inflated_length == 0
	   || chunk->inflated_length > MP_CHUNK_INFLATED_CAP)
		return decoder_error(decoder);
	chunk->compressed_length = (size_t)compressed;
	/* W2 (design section 2.4): THE allocation site for every compressed
	 * Packet51 body -- see mp_body_alloc. A NULL here is a CAPACITY answer,
	 * and the read loop maps it to the clean mp_end=5 session end rather
	 * than a generic decode error. */
	decoder->chunk_body = mp_body_alloc(chunk->compressed_length);
	if(!decoder->chunk_body && decoder->reclaim) {
		/* F5a (review finding H2): RECLAIM, THEN RETRY -- before the packet
		 * is discarded, not after. W2 ran the reclaim ladder from the read
		 * loop's decode-ERROR handler, i.e. once decoder_error had already
		 * thrown these bytes away: a reclaim that SUCCEEDED there freed
		 * capacity for a body that no longer existed, mutated authoritative
		 * state moments before the teardown, and then let the loop report
		 * "bad or unsupported P0 packet" with mp_end never latched. The
		 * reclaim belongs HERE, where the retry can still use it.
		 *
		 * Exactly one retry. The ladder is "one compaction OR one superseded
		 * head per call" by design (server_remote_chunks.c), so a loop here
		 * would turn one refused column into an unbounded re-deflate storm on
		 * the read thread; if one lossless reclaim is not enough the world
		 * genuinely does not fit, which is the capacity answer. */
		if(decoder->reclaim(decoder->reclaim_ctx, chunk->compressed_length))
			decoder->chunk_body = mp_body_alloc(chunk->compressed_length);
	}
	if(!decoder->chunk_body) {
		decoder->allocation_failed = true;
		decoder->alloc_wanted = (uint32_t)chunk->compressed_length;
		return decoder_error(decoder);
	}
	chunk->compressed = decoder->chunk_body;
	decoder->chunk_have = 0;
	decoder->chunk_need = chunk->compressed_length;
	decoder->phase = DECODE_MAP_BODY;
	(void)event_out;
	return MP_DECODE_MORE;
}

enum mp_decode_result mp_decoder_feed(struct mp_decoder* decoder, uint8_t byte,
									 struct mp_event* event_out) {
	if(!decoder || !event_out)
		return MP_DECODE_ERROR;

	if(decoder->phase == DECODE_PACKET_ID) {
		memset(&decoder->event, 0, sizeof(decoder->event));
		decoder->packet_id = byte;
		decoder->error_packet_id = 0;
		decoder->error_phase = 0;
		switch(byte) {
			case MP_PACKET_KEEP_ALIVE:
				decoder->event.type = MP_EVENT_KEEP_ALIVE;
				return emit_event(decoder, event_out);
			case MP_PACKET_LOGIN:
				return begin_scalar(decoder, DECODE_LOGIN_ENTITY, 4);
			case MP_PACKET_HANDSHAKE: return begin_text(decoder, 32);
			/* S4 P3. Chat's read-side maximum is 119 code units. [740c583
			 * Packet3Chat.readPacketData] */
			case MP_PACKET_CHAT: return begin_text(decoder, 119);
			case MP_PACKET_DISCONNECT: return begin_text(decoder, 100);
			case MP_PACKET_UPDATE_TIME: return begin_scalar(decoder, DECODE_FIXED_BODY, 8);
			case MP_PACKET_SPAWN_POSITION: return begin_scalar(decoder, DECODE_FIXED_BODY, 12);
			case MP_PACKET_UPDATE_HEALTH: return begin_scalar(decoder, DECODE_FIXED_BODY, 2);
			case MP_PACKET_FLYING: return begin_scalar(decoder, DECODE_FIXED_BODY, 1);
			case MP_PACKET_PLAYER_POSITION: return begin_scalar(decoder, DECODE_FIXED_BODY, 33);
			case MP_PACKET_PLAYER_LOOK: return begin_scalar(decoder, DECODE_FIXED_BODY, 9);
			case MP_PACKET_PLAYER_LOOK_MOVE: return begin_scalar(decoder, DECODE_FIXED_BODY, 41);
			case MP_PACKET_PRE_CHUNK: return begin_scalar(decoder, DECODE_FIXED_BODY, 9);
			case MP_PACKET_MAP_CHUNK: return begin_scalar(decoder, DECODE_MAP_HEADER, 17);
			/* S2 P1. */
			/* Beta's own Packet.readPacket rejects a wrong-direction id before
			 * dispatch -- for the client role (server flag false) that means only
			 * clientPacketIdList (S->C + bidirectional) ids are decodable
			 * [740c583 Packet.readPacket]. CaveWii is always the client, so
			 * client->server-only P1 ids (UseEntity/BlockDig/Place/
			 * BlockItemSwitch/EntityAction/WindowClick) have NO decode case
			 * here on purpose -- mp_encode_* is their only production path;
			 * decoding them would accept bytes a real Beta server never
			 * sends. mp_packet_supported() still reports them C->S-true. */
			case MP_PACKET_PLAYER_INVENTORY: return begin_scalar(decoder, DECODE_FIXED_BODY, 10);
			case MP_PACKET_RESPAWN: return begin_scalar(decoder, DECODE_FIXED_BODY, 1);
			case MP_PACKET_MULTI_BLOCK_CHANGE: return begin_scalar(decoder, DECODE_MULTIBLOCK_HEADER, 10);
			case MP_PACKET_BLOCK_CHANGE: return begin_scalar(decoder, DECODE_FIXED_BODY, 11);
			case MP_PACKET_OPEN_WINDOW: return begin_scalar(decoder, DECODE_OPEN_WINDOW_PREFIX, 2);
			case MP_PACKET_CLOSE_WINDOW: return begin_scalar(decoder, DECODE_FIXED_BODY, 1);
			case MP_PACKET_SET_SLOT: return begin_scalar(decoder, DECODE_SET_SLOT_PREFIX, 3);
			case MP_PACKET_WINDOW_ITEMS: return begin_scalar(decoder, DECODE_WINDOW_ITEMS_HEADER, 3);
			case MP_PACKET_UPDATE_PROGRESSBAR: return begin_scalar(decoder, DECODE_FIXED_BODY, 5);
			case MP_PACKET_TRANSACTION: return begin_scalar(decoder, DECODE_FIXED_BODY, 4);
			case MP_PACKET_UPDATE_SIGN: return begin_scalar(decoder, DECODE_SIGN_PREFIX, 10);
			/* S3 P2. MP_PACKET_POSITION_LEGACY has no decode case -- it is
			 * C->S only and, per its enum comment, never actually sent by
			 * any real Beta client; mp_encode_position_legacy is its only
			 * production path, matching the client->server-only P1 idiom
			 * above. */
			case MP_PACKET_SLEEP: return begin_scalar(decoder, DECODE_FIXED_BODY, 14);
			case MP_PACKET_ANIMATION: return begin_scalar(decoder, DECODE_FIXED_BODY, 5);
			case MP_PACKET_NAMED_ENTITY_SPAWN: return begin_scalar(decoder, DECODE_NAMED_SPAWN_ID, 4);
			case MP_PACKET_PICKUP_SPAWN: return begin_scalar(decoder, DECODE_FIXED_BODY, 24);
			case MP_PACKET_COLLECT: return begin_scalar(decoder, DECODE_FIXED_BODY, 8);
			case MP_PACKET_VEHICLE_SPAWN: return begin_scalar(decoder, DECODE_VEHICLE_HEADER, 21);
			case MP_PACKET_MOB_SPAWN: return begin_scalar(decoder, DECODE_MOBSPAWN_HEADER, 19);
			case MP_PACKET_ENTITY_PAINTING: return begin_scalar(decoder, DECODE_PAINTING_ID, 4);
			case MP_PACKET_ENTITY_VELOCITY: return begin_scalar(decoder, DECODE_FIXED_BODY, 10);
			case MP_PACKET_DESTROY_ENTITY: return begin_scalar(decoder, DECODE_FIXED_BODY, 4);
			case MP_PACKET_ENTITY: return begin_scalar(decoder, DECODE_FIXED_BODY, 4);
			case MP_PACKET_REL_ENTITY_MOVE: return begin_scalar(decoder, DECODE_FIXED_BODY, 7);
			case MP_PACKET_ENTITY_LOOK: return begin_scalar(decoder, DECODE_FIXED_BODY, 6);
			case MP_PACKET_REL_ENTITY_MOVE_LOOK: return begin_scalar(decoder, DECODE_FIXED_BODY, 9);
			case MP_PACKET_ENTITY_TELEPORT: return begin_scalar(decoder, DECODE_FIXED_BODY, 18);
			case MP_PACKET_ENTITY_STATUS: return begin_scalar(decoder, DECODE_FIXED_BODY, 5);
			case MP_PACKET_ATTACH_ENTITY: return begin_scalar(decoder, DECODE_FIXED_BODY, 8);
			case MP_PACKET_ENTITY_METADATA: return begin_scalar(decoder, DECODE_ENTITY_METADATA_ID, 4);
			/* S4 P3. Body sizes: NoteBlock i32+i16+i32+u8+u8=12; DoorChange
			 * i32+i32+i8+i32+i32=17; Bed i8=1; Weather i32+i8+i32+i32+i32=17;
			 * Statistic i32+i8=5. Explosion/MapData need multi-phase decode
			 * (variable-length tail). */
			case MP_PACKET_NOTE_BLOCK: return begin_scalar(decoder, DECODE_FIXED_BODY, 12);
			case MP_PACKET_EXPLOSION: return begin_scalar(decoder, DECODE_EXPLOSION_HEADER, 32);
			case MP_PACKET_DOOR_CHANGE: return begin_scalar(decoder, DECODE_FIXED_BODY, 17);
			case MP_PACKET_BED: return begin_scalar(decoder, DECODE_FIXED_BODY, 1);
			case MP_PACKET_WEATHER: return begin_scalar(decoder, DECODE_FIXED_BODY, 17);
			case MP_PACKET_MAP_DATA: return begin_scalar(decoder, DECODE_MAPDATA_PREFIX, 4);
			case MP_PACKET_STATISTIC: return begin_scalar(decoder, DECODE_FIXED_BODY, 5);
			default: return decoder_error(decoder);
		}
	}

	if(decoder->phase == DECODE_LOGIN_DIMENSION) {
		decoder->event.payload.login.dimension = (int8_t)byte;
		return emit_event(decoder, event_out);
	}
	if(decoder->phase == DECODE_MAP_BODY) {
		decoder->chunk_body[decoder->chunk_have++] = byte;
		if(decoder->chunk_have == decoder->chunk_need) {
			decoder->event.type = MP_EVENT_MAP_CHUNK;
			return emit_event(decoder, event_out);
		}
		return MP_DECODE_MORE;
	}
	if(decoder->phase == DECODE_MULTIBLOCK_BODY) {
		decoder->chunk_body[decoder->chunk_have++] = byte;
		if(decoder->chunk_have == decoder->chunk_need)
			return finish_multiblock(decoder, event_out);
		return MP_DECODE_MORE;
	}
	if(decoder->phase == DECODE_EXPLOSION_BODY) {
		decoder->chunk_body[decoder->chunk_have++] = byte;
		if(decoder->chunk_have == decoder->chunk_need)
			return finish_explosion(decoder, event_out);
		return MP_DECODE_MORE;
	}
	if(decoder->phase == DECODE_MAPDATA_BODY) {
		decoder->event.payload.map_data.bytes[decoder->chunk_have++] = byte;
		if(decoder->chunk_have == decoder->chunk_need) {
			decoder->event.type = MP_EVENT_MAP_DATA;
			return emit_event(decoder, event_out);
		}
		return MP_DECODE_MORE;
	}
	if(decoder->phase == DECODE_METADATA_HEADER)
		return handle_metadata_header(decoder, byte, event_out);
	if(decoder->phase == DECODE_OPEN_WINDOW_TITLE_BODY) {
		decoder->title_bytes[decoder->title_have++] = byte;
		if(decoder->title_have == decoder->title_need)
			return begin_scalar(decoder, DECODE_OPEN_WINDOW_SLOTCOUNT, 1);
		return MP_DECODE_MORE;
	}
	if(decoder->phase == DECODE_TEXT_UNITS) {
		if(!decoder->have_text_high) {
			decoder->text_byte = byte;
			decoder->have_text_high = true;
			return MP_DECODE_MORE;
		}
		decoder->have_text_high = false;
		decoder->text.units[decoder->text.length++]
			= ((uint16_t)decoder->text_byte << 8) | byte;
		if(decoder->text.length == decoder->scalar_need)
			return finish_text(decoder, event_out);
		return MP_DECODE_MORE;
	}
	if(decoder->scalar_have >= sizeof(decoder->scalar))
		return decoder_error(decoder);
	decoder->scalar[decoder->scalar_have++] = byte;
	if(decoder->scalar_have < decoder->scalar_need)
		return MP_DECODE_MORE;

	switch(decoder->phase) {
		case DECODE_FIXED_BODY: return parse_fixed(decoder, event_out);
		case DECODE_MAP_HEADER: return parse_map_header(decoder, event_out);
		case DECODE_LOGIN_ENTITY:
			decoder->event.type = MP_EVENT_LOGIN;
			decoder->event.payload.login.entity_id = read_i32(decoder->scalar);
			return begin_text(decoder, 16);
		case DECODE_LOGIN_SEED:
			decoder->event.payload.login.map_seed = read_i64(decoder->scalar);
			decoder->phase = DECODE_LOGIN_DIMENSION;
			return MP_DECODE_MORE;
		case DECODE_TEXT_LENGTH: {
			int16_t length = read_i16(decoder->scalar);
			if(length < 0 || (size_t)length > decoder->text_max
			   || (size_t)length >= MP_TEXT_CAPACITY)
				return decoder_error(decoder);
			decoder->scalar_need = (size_t)length;
			decoder->scalar_have = 0;
			decoder->text.length = 0;
			decoder->phase = DECODE_TEXT_UNITS;
			if(length == 0)
				return finish_text(decoder, event_out);
			return MP_DECODE_MORE;
		}
		/* S2 P1 multi-stage packets. Place/WindowClick are client->server-only
		 * (see the DECODE_PACKET_ID dispatch note above) so they have no
		 * prefix phase here; SetSlot/WindowItems are the S->C item-stack
		 * consumers and share DECODE_ITEM_ID/DECODE_ITEM_REST below. */
		case DECODE_ITEM_ID: {
			int16_t id = read_i16(decoder->scalar);
			if(id < 0) {
				decoder->item_scratch.present = false;
				decoder->item_scratch.id = id;
				return finish_item(decoder, event_out);
			}
			decoder->item_scratch.present = true;
			decoder->item_scratch.id = id;
			return begin_scalar(decoder, DECODE_ITEM_REST, 3);
		}
		case DECODE_ITEM_REST:
			decoder->item_scratch.count = decoder->scalar[0];
			decoder->item_scratch.damage = read_i16(decoder->scalar + 1);
			return finish_item(decoder, event_out);
		case DECODE_MULTIBLOCK_HEADER: {
			const uint8_t* p = decoder->scalar;
			struct mp_multi_block_change* change
				= &decoder->event.payload.multi_block_change;
			change->chunk_x = read_i32(p);
			change->chunk_z = read_i32(p + 4);
			change->count = read_u16(p + 8);
			if(change->count == 0)
				return finish_multiblock(decoder, event_out);
			size_t need = (size_t)change->count * 4u;
			decoder->chunk_body = s_mp_alloc(need);
			if(!decoder->chunk_body) {
				decoder->allocation_failed = true;
				return decoder_error(decoder);
			}
			decoder->chunk_have = 0;
			decoder->chunk_need = need;
			decoder->phase = DECODE_MULTIBLOCK_BODY;
			return MP_DECODE_MORE;
		}
		case DECODE_OPEN_WINDOW_PREFIX:
			decoder->event.payload.open_window.window = (int8_t)decoder->scalar[0];
			decoder->event.payload.open_window.inventory_type = decoder->scalar[1];
			decoder->title_have = 0;
			return begin_scalar(decoder, DECODE_OPEN_WINDOW_TITLE_LEN, 2);
		case DECODE_OPEN_WINDOW_TITLE_LEN: {
			uint16_t title_length = read_u16(decoder->scalar);
			if(title_length > MP_WINDOW_TITLE_BYTE_CAP)
				return decoder_error(decoder);
			decoder->title_have = 0;
			decoder->title_need = title_length;
			if(title_length == 0)
				return begin_scalar(decoder, DECODE_OPEN_WINDOW_SLOTCOUNT, 1);
			decoder->phase = DECODE_OPEN_WINDOW_TITLE_BODY;
			return MP_DECODE_MORE;
		}
		case DECODE_OPEN_WINDOW_SLOTCOUNT:
			if(!modified_utf8_to_utf8(decoder->title_bytes, decoder->title_need,
									  decoder->event.payload.open_window.title,
									  sizeof(decoder->event.payload.open_window.title)))
				return decoder_error(decoder);
			decoder->event.payload.open_window.slot_count = decoder->scalar[0];
			decoder->event.type = MP_EVENT_OPEN_WINDOW;
			return emit_event(decoder, event_out);
		case DECODE_SET_SLOT_PREFIX:
			decoder->event.payload.set_slot.window = (int8_t)decoder->scalar[0];
			decoder->event.payload.set_slot.slot = read_i16(decoder->scalar + 1);
			return begin_item_stack(decoder);
		case DECODE_WINDOW_ITEMS_HEADER: {
			int8_t window = (int8_t)decoder->scalar[0];
			int16_t count = read_i16(decoder->scalar + 1);
			/* Java's signed short count sizes the ItemStack array; negative throws.
			 * [740c583 Packet104WindowItems.readPacketData] */
			if(count < 0 || (uint16_t)count > MP_WINDOW_ITEMS_MAX_ENTRIES)
				return decoder_error(decoder);
			decoder->event.payload.window_items.window = window;
			decoder->event.payload.window_items.count = (uint16_t)count;
			decoder->array_index = 0;
			decoder->array_count = (uint16_t)count;
			if(count == 0) {
				decoder->event.type = MP_EVENT_WINDOW_ITEMS;
				return emit_event(decoder, event_out);
			}
			decoder->event.payload.window_items.items
				= s_mp_alloc((size_t)count * sizeof(struct mp_item_stack));
			if(!decoder->event.payload.window_items.items) {
				decoder->allocation_failed = true;
				return decoder_error(decoder);
			}
			return begin_item_stack(decoder);
		}
		case DECODE_SIGN_PREFIX: {
			const uint8_t* p = decoder->scalar;
			decoder->event.payload.update_sign.x = read_i32(p);
			decoder->event.payload.update_sign.y = read_i16(p + 4);
			decoder->event.payload.update_sign.z = read_i32(p + 6);
			decoder->sign_line = 0;
			return begin_text(decoder, MP_SIGN_LINE_MAX);
		}
		/* S3 P2 multi-stage packets. */
		case DECODE_NAMED_SPAWN_ID:
			decoder->event.type = MP_EVENT_NAMED_ENTITY_SPAWN;
			decoder->event.payload.named_entity_spawn.entity_id
				= read_i32(decoder->scalar);
			return begin_text(decoder, MP_NAMED_ENTITY_NAME_MAX);
		case DECODE_NAMED_SPAWN_TAIL: {
			/* x,y,z(i32) then rotation,pitch(i8) then currentItem(i16).
			 * [740c583 Packet20NamedEntitySpawn.readPacketData] */
			const uint8_t* p = decoder->scalar;
			decoder->event.payload.named_entity_spawn.x = read_i32(p);
			decoder->event.payload.named_entity_spawn.y = read_i32(p + 4);
			decoder->event.payload.named_entity_spawn.z = read_i32(p + 8);
			decoder->event.payload.named_entity_spawn.rotation = (int8_t)p[12];
			decoder->event.payload.named_entity_spawn.pitch = (int8_t)p[13];
			decoder->event.payload.named_entity_spawn.current_item
				= read_i16(p + 14);
			return emit_event(decoder, event_out);
		}
		case DECODE_VEHICLE_HEADER: {
			/* entityId(i32),type(i8),x/y/z(i32),throwerEntityId(i32).
			 * [740c583 Packet23VehicleSpawn.readPacketData] */
			const uint8_t* p = decoder->scalar;
			decoder->event.payload.vehicle_spawn.entity_id = read_i32(p);
			decoder->event.payload.vehicle_spawn.vtype = p[4];
			decoder->event.payload.vehicle_spawn.x = read_i32(p + 5);
			decoder->event.payload.vehicle_spawn.y = read_i32(p + 9);
			decoder->event.payload.vehicle_spawn.z = read_i32(p + 13);
			int32_t thrower = read_i32(p + 17);
			decoder->event.payload.vehicle_spawn.thrower_entity_id = thrower;
			if(thrower > 0) {
				decoder->event.payload.vehicle_spawn.has_velocity = true;
				return begin_scalar(decoder, DECODE_VEHICLE_VELOCITY, 6);
			}
			decoder->event.payload.vehicle_spawn.has_velocity = false;
			decoder->event.type = MP_EVENT_VEHICLE_SPAWN;
			return emit_event(decoder, event_out);
		}
		case DECODE_VEHICLE_VELOCITY: {
			const uint8_t* p = decoder->scalar;
			decoder->event.payload.vehicle_spawn.speed_x = read_i16(p);
			decoder->event.payload.vehicle_spawn.speed_y = read_i16(p + 2);
			decoder->event.payload.vehicle_spawn.speed_z = read_i16(p + 4);
			decoder->event.type = MP_EVENT_VEHICLE_SPAWN;
			return emit_event(decoder, event_out);
		}
		case DECODE_MOBSPAWN_HEADER: {
			/* entityId(i32),type(i8),x/y/z(i32),yaw(i8),pitch(i8), then a
			 * DataWatcher stream. [740c583 Packet24MobSpawn.readPacketData] */
			const uint8_t* p = decoder->scalar;
			decoder->event.payload.mob_spawn.entity_id = read_i32(p);
			decoder->event.payload.mob_spawn.mtype = p[4];
			decoder->event.payload.mob_spawn.x = read_i32(p + 5);
			decoder->event.payload.mob_spawn.y = read_i32(p + 9);
			decoder->event.payload.mob_spawn.z = read_i32(p + 13);
			decoder->event.payload.mob_spawn.yaw = (int8_t)p[17];
			decoder->event.payload.mob_spawn.pitch = (int8_t)p[18];
			decoder->event.payload.mob_spawn.metadata.count = 0;
			decoder->metadata_target = &decoder->event.payload.mob_spawn.metadata;
			decoder->phase = DECODE_METADATA_HEADER;
			return MP_DECODE_MORE;
		}
		case DECODE_PAINTING_ID:
			decoder->event.payload.entity_painting.entity_id
				= read_i32(decoder->scalar);
			return begin_text(decoder, MP_PAINTING_TITLE_MAX);
		case DECODE_PAINTING_TAIL: {
			/* x,y,z(i32) then direction(i32) -- NOT fixed-point; these are
			 * raw block coordinates. [740c583
			 * Packet25EntityPainting.readPacketData] */
			const uint8_t* p = decoder->scalar;
			decoder->event.payload.entity_painting.x = read_i32(p);
			decoder->event.payload.entity_painting.y = read_i32(p + 4);
			decoder->event.payload.entity_painting.z = read_i32(p + 8);
			decoder->event.payload.entity_painting.direction = read_i32(p + 12);
			decoder->event.type = MP_EVENT_ENTITY_PAINTING;
			return emit_event(decoder, event_out);
		}
		case DECODE_ENTITY_METADATA_ID:
			decoder->event.payload.entity_metadata.entity_id
				= read_i32(decoder->scalar);
			decoder->event.payload.entity_metadata.metadata.count = 0;
			decoder->metadata_target
				= &decoder->event.payload.entity_metadata.metadata;
			decoder->phase = DECODE_METADATA_HEADER;
			return MP_DECODE_MORE;
		case DECODE_METADATA_VALUE: {
			struct mp_metadata* m = decoder->metadata_target;
			struct mp_watchable_entry* entry = &m->entries[m->count];
			const uint8_t* p = decoder->scalar;
			switch(entry->type) {
				case 0: entry->value.byte_value = (int8_t)p[0]; break;
				case 1: entry->value.short_value = read_i16(p); break;
				case 2: entry->value.int_value = read_i32(p); break;
				case 3: entry->value.float_value = read_f32(p); break;
				case 6:
					entry->value.coord_value.x = read_i32(p);
					entry->value.coord_value.y = read_i32(p + 4);
					entry->value.coord_value.z = read_i32(p + 8);
					break;
				default: break; /* 4/5 route through finish_text/finish_item */
			}
			m->count++;
			decoder->phase = DECODE_METADATA_HEADER;
			return MP_DECODE_MORE;
		}
		case DECODE_METADATA_ITEM: {
			/* Unconditional 5-byte ItemStack read: id(i16), count(i8),
			 * damage(i16) -- no Slot(-1) absent check. [740c583
			 * DataWatcher.readWatchableObjects case 5] */
			struct mp_metadata* m = decoder->metadata_target;
			struct mp_watchable_entry* entry = &m->entries[m->count];
			const uint8_t* p = decoder->scalar;
			entry->value.item_value = (struct mp_item_stack) {
				.present = true,
				.id = read_i16(p),
				.count = p[2],
				.damage = read_i16(p + 3),
			};
			m->count++;
			decoder->phase = DECODE_METADATA_HEADER;
			return MP_DECODE_MORE;
		}
		/* S4 P3 multi-stage packets. */
		case DECODE_EXPLOSION_HEADER: {
			/* explosionX/Y/Z(f64); explosionSize(f32); count(i32) -- Beta's own
			 * writer can never exceed doExplosionA's fixed 1352-cell sample, but
			 * the reader trusts the raw i32; validate before allocating.
			 * [740c583 Packet60Explosion.readPacketData] */
			const uint8_t* p = decoder->scalar;
			decoder->event.payload.explosion.x = read_f64(p);
			decoder->event.payload.explosion.y = read_f64(p + 8);
			decoder->event.payload.explosion.z = read_f64(p + 16);
			decoder->event.payload.explosion.power = read_f32(p + 24);
			int32_t count = read_i32(p + 28);
			if(count < 0 || (uint32_t)count > MP_EXPLOSION_MAX_BLOCKS)
				return decoder_error(decoder);
			decoder->event.payload.explosion.count = (uint16_t)count;
			if(count == 0) {
				decoder->event.type = MP_EVENT_EXPLOSION;
				return emit_event(decoder, event_out);
			}
			size_t need = (size_t)count * 3u;
			decoder->chunk_body = s_mp_alloc(need);
			if(!decoder->chunk_body) {
				decoder->allocation_failed = true;
				return decoder_error(decoder);
			}
			decoder->chunk_have = 0;
			decoder->chunk_need = need;
			decoder->phase = DECODE_EXPLOSION_BODY;
			return MP_DECODE_MORE;
		}
		case DECODE_MAPDATA_PREFIX:
			/* field_28055_a(i16, item id); field_28054_b(i16, map id). [740c583
			 * Packet131MapData.readPacketData] */
			decoder->event.payload.map_data.item_id = read_i16(decoder->scalar);
			decoder->event.payload.map_data.map_id
				= read_i16(decoder->scalar + 2);
			return begin_scalar(decoder, DECODE_MAPDATA_LEN, 1);
		case DECODE_MAPDATA_LEN: {
			uint8_t length = decoder->scalar[0];
			decoder->event.payload.map_data.length = length;
			if(length == 0) {
				decoder->event.type = MP_EVENT_MAP_DATA;
				return emit_event(decoder, event_out);
			}
			decoder->chunk_have = 0;
			decoder->chunk_need = length;
			decoder->phase = DECODE_MAPDATA_BODY;
			return MP_DECODE_MORE;
		}
		default: return decoder_error(decoder);
	}
}

static bool utf8_to_utf16(const char* input, struct mp_utf16* output,
							 size_t maximum) {
	if(!input || !output)
		return false;
	output->length = 0;
	const unsigned char* p = (const unsigned char*)input;
	while(*p) {
		uint32_t cp;
		size_t count;
		if(*p < 0x80) { cp = *p; count = 1; }
		else if((*p & 0xE0) == 0xC0 && p[1]) {
			cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); count = 2;
			if(cp < 0x80 || (p[1] & 0xC0) != 0x80) return false;
		} else if((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
			cp = ((uint32_t)(p[0] & 0x0F) << 12)
				| ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); count = 3;
			if(cp < 0x800 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80
			   || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
		} else if((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
			cp = ((uint32_t)(p[0] & 7) << 18) | ((uint32_t)(p[1] & 0x3F) << 12)
				| ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); count = 4;
			if(cp < 0x10000 || cp > 0x10FFFF || (p[1] & 0xC0) != 0x80
			   || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return false;
		} else return false;
		size_t needed = cp <= 0xFFFF ? 1 : 2;
		if(output->length + needed > maximum
		   || output->length + needed >= MP_TEXT_CAPACITY) return false;
		if(needed == 1) output->units[output->length++] = (uint16_t)cp;
		else {
			cp -= 0x10000;
			output->units[output->length++] = 0xD800u | (uint16_t)(cp >> 10);
			output->units[output->length++] = 0xDC00u | (uint16_t)(cp & 0x3FF);
		}
		p += count;
	}
	return true;
}

static bool append_u8(struct mp_frame* frame, uint8_t value) {
	if(frame->length >= sizeof(frame->data)) return false;
	/* Packet fields start at arbitrary byte offsets. Broadway cannot execute
	 * an unaligned floating-point store: release LTO combined the scalar
	 * writes for Packet13 into stfd at offset 17 (video fault 8017EA4C).
	 * Volatile byte access prevents widening at this wire-output boundary;
	 * the frame itself remains ordinary memory, with identical wire bytes. */
	volatile uint8_t* bytes = frame->data;
	bytes[frame->length++] = value;
	return true;
}
static bool append_i16(struct mp_frame* f, int16_t value) {
	uint16_t v = (uint16_t)value;
	return append_u8(f, (uint8_t)(v >> 8)) && append_u8(f, (uint8_t)v);
}
static bool append_i32(struct mp_frame* f, int32_t value) {
	uint32_t v = (uint32_t)value;
	return append_u8(f, (uint8_t)(v >> 24)) && append_u8(f, (uint8_t)(v >> 16))
		&& append_u8(f, (uint8_t)(v >> 8)) && append_u8(f, (uint8_t)v);
}
static bool append_u32(struct mp_frame* f, uint32_t value) {
	return append_u8(f, (uint8_t)(value >> 24))
		&& append_u8(f, (uint8_t)(value >> 16))
		&& append_u8(f, (uint8_t)(value >> 8))
		&& append_u8(f, (uint8_t)value);
}
static bool append_i64(struct mp_frame* f, int64_t value) {
	uint64_t v = (uint64_t)value;
	for(int shift = 56; shift >= 0; shift -= 8)
		if(!append_u8(f, (uint8_t)(v >> shift))) return false;
	return true;
}
static bool append_u64(struct mp_frame* f, uint64_t value) {
	for(int shift = 56; shift >= 0; shift -= 8)
		if(!append_u8(f, (uint8_t)(value >> shift)))
			return false;
	return true;
}
static bool append_f32(struct mp_frame* f, float value) {
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return append_u32(f, bits);
}
static bool append_f64(struct mp_frame* f, double value) {
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return append_u64(f, bits);
}
static bool append_string16(struct mp_frame* f, const char* utf8, size_t maximum) {
	struct mp_utf16 text;
	if(!utf8_to_utf16(utf8, &text, maximum)
	   || !append_i16(f, (int16_t)text.length)) return false;
	for(size_t i = 0; i < text.length; i++)
		if(!append_i16(f, (int16_t)text.units[i])) return false;
	return true;
}
static bool begin_frame(struct mp_frame* frame, uint8_t id) {
	if(!frame) return false;
	frame->length = 0;
	return append_u8(frame, id);
}

/* Shared ItemStack tail used by Place/WindowClick/SetSlot/WindowItems: i16 id
 * (-1 = null stack, nothing follows), else i8 count then i16 damage.
 * [740c583 Packet15Place.writePacketData] */
static bool append_item_stack(struct mp_frame* f, const struct mp_item_stack* item) {
	if(!item || !item->present)
		return append_i16(f, -1);
	return append_i16(f, item->id) && append_u8(f, item->count)
		&& append_i16(f, item->damage);
}

/* Inverse of modified_utf8_to_utf8: encode a UTF-8 C string as Java's
 * modified UTF-8 (writeUTF), reusing utf8_to_utf16's surrogate-splitting so
 * an astral character becomes two 3-byte CESU-8 halves, never a native
 * 4-byte UTF-8 sequence. [740c583 Packet100OpenWindow.writePacketData] */
static bool append_modified_utf8(struct mp_frame* f, const char* utf8) {
	struct mp_utf16 text;
	if(!utf8_to_utf16(utf8, &text, MP_TEXT_CAPACITY - 1))
		return false;
	size_t byte_length = 0;
	for(size_t i = 0; i < text.length; i++) {
		uint16_t u = text.units[i];
		byte_length += u == 0 ? 2 : u < 0x80 ? 1 : u < 0x800 ? 2 : 3;
	}
	if(byte_length > UINT16_MAX || !append_i16(f, (int16_t)(uint16_t)byte_length))
		return false;
	for(size_t i = 0; i < text.length; i++) {
		uint16_t u = text.units[i];
		if(u == 0) {
			if(!append_u8(f, 0xC0) || !append_u8(f, 0x80)) return false;
		} else if(u < 0x80) {
			if(!append_u8(f, (uint8_t)u)) return false;
		} else if(u < 0x800) {
			if(!append_u8(f, (uint8_t)(0xC0 | (u >> 6)))
			   || !append_u8(f, (uint8_t)(0x80 | (u & 0x3F)))) return false;
		} else {
			if(!append_u8(f, (uint8_t)(0xE0 | (u >> 12)))
			   || !append_u8(f, (uint8_t)(0x80 | ((u >> 6) & 0x3F)))
			   || !append_u8(f, (uint8_t)(0x80 | (u & 0x3F)))) return false;
		}
	}
	return true;
}

bool mp_encode_keep_alive(struct mp_frame* f) { return begin_frame(f, MP_PACKET_KEEP_ALIVE); }
bool mp_encode_handshake(struct mp_frame* f, const char* s) { return begin_frame(f, MP_PACKET_HANDSHAKE) && append_string16(f, s, 32); }
bool mp_encode_login(struct mp_frame* f, const char* s) { return begin_frame(f, MP_PACKET_LOGIN) && append_i32(f, 14) && append_string16(f, s, 16) && append_i64(f, 0) && append_u8(f, 0); }
bool mp_encode_update_time(struct mp_frame* f, int64_t t) { return begin_frame(f, MP_PACKET_UPDATE_TIME) && append_i64(f, t); }
bool mp_encode_spawn_position(struct mp_frame* f, int32_t x, int32_t y, int32_t z) { return begin_frame(f, MP_PACKET_SPAWN_POSITION) && append_i32(f, x) && append_i32(f, y) && append_i32(f, z); }
bool mp_encode_update_health(struct mp_frame* f, int16_t h) { return begin_frame(f, MP_PACKET_UPDATE_HEALTH) && append_i16(f, h); }
bool mp_encode_flying(struct mp_frame* f, bool g) { return begin_frame(f, MP_PACKET_FLYING) && append_u8(f, g); }
bool mp_encode_player_position(struct mp_frame* f, double x, double y, double stance, double z, bool g) { return begin_frame(f, MP_PACKET_PLAYER_POSITION) && append_f64(f, x) && append_f64(f, y) && append_f64(f, stance) && append_f64(f, z) && append_u8(f, g); }
bool mp_encode_player_look(struct mp_frame* f, float yaw, float pitch, bool g) { return begin_frame(f, MP_PACKET_PLAYER_LOOK) && append_f32(f, yaw) && append_f32(f, pitch) && append_u8(f, g); }
bool mp_encode_player_look_move(struct mp_frame* f, double x, double y, double stance, double z, float yaw, float pitch, bool g) { return begin_frame(f, MP_PACKET_PLAYER_LOOK_MOVE) && append_f64(f, x) && append_f64(f, y) && append_f64(f, stance) && append_f64(f, z) && append_f32(f, yaw) && append_f32(f, pitch) && append_u8(f, g); }
bool mp_encode_pre_chunk(struct mp_frame* f, int32_t x, int32_t z, bool mode) { return begin_frame(f, MP_PACKET_PRE_CHUNK) && append_i32(f, x) && append_i32(f, z) && append_u8(f, mode); }
bool mp_encode_map_chunk(struct mp_frame* f, const struct mp_map_chunk* c) {
	if(!c || !c->compressed || c->sx == 0 || c->sx > 16 || c->sy == 0
	   || c->sy > 128 || c->sz == 0 || c->sz > 16 || c->compressed_length == 0
	   || c->compressed_length > INT32_MAX) return false;
	if(!begin_frame(f, MP_PACKET_MAP_CHUNK) || !append_i32(f, c->x)
	   || !append_i16(f, c->y) || !append_i32(f, c->z)
	   || !append_u8(f, (uint8_t)(c->sx - 1))
	   || !append_u8(f, (uint8_t)(c->sy - 1))
	   || !append_u8(f, (uint8_t)(c->sz - 1))
	   || !append_i32(f, (int32_t)c->compressed_length)) return false;
	for(size_t i = 0; i < c->compressed_length; i++)
		if(!append_u8(f, c->compressed[i])) return false;
	return true;
}
bool mp_encode_disconnect(struct mp_frame* f, const char* s) { return begin_frame(f, MP_PACKET_DISCONNECT) && append_string16(f, s, 100); }
/* S4 P3. See the mp_codec.h declaration for the reject-vs-truncate note. */
bool mp_encode_chat(struct mp_frame* f, const char* s) { return begin_frame(f, MP_PACKET_CHAT) && append_string16(f, s, 119); }

size_t mp_chat_sanitize_outbound(const char* message_utf8, char* out,
								 size_t out_size) {
	if(!message_utf8 || !out || out_size < 401)
		return 0;
	const unsigned char* p = (const unsigned char*)message_utf8;
	size_t written = 0;
	size_t units = 0; /* UTF-16 code units kept (GuiChat's 100 cap) */
	bool any_non_space = false;
	while(*p && units < 100) {
		size_t len;
		uint32_t cp;
		if(p[0] < 0x80) {
			cp = p[0];
			len = 1;
		} else if((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
			cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
			len = 2;
		} else if((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80
				  && (p[2] & 0xC0) == 0x80) {
			cp = ((uint32_t)(p[0] & 0x0F) << 12)
				| ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
			len = 3;
		} else if((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80
				  && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
			cp = ((uint32_t)(p[0] & 0x07) << 18)
				| ((uint32_t)(p[1] & 0x3F) << 12)
				| ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
			len = 4;
		} else {
			/* Malformed sequence: drop the byte, keep scanning (the same
			 * strip-don't-reject shape as the illegal characters below --
			 * the composer analog is "the key never registered"). */
			p++;
			continue;
		}
		size_t cp_units = (len == 4) ? 2 : 1;
		/* The illegal set: \247 plus C0 controls and DEL. */
		if(cp == 0xA7 || cp < 0x20 || cp == 0x7F) {
			p += len;
			continue;
		}
		if(units + cp_units > 100)
			break;
		if(written + len + 1 > out_size)
			break;
		for(size_t k = 0; k < len; k++)
			out[written++] = (char)p[k];
		units += cp_units;
		if(cp != 0x20)
			any_non_space = true;
		p += len;
	}
	/* GuiChat sends message.trim() and only when non-empty: strip the
	 * leading/trailing ASCII spaces and refuse an all-space (or fully
	 * stripped) result. */
	if(!any_non_space) {
		out[0] = '\0';
		return 0;
	}
	while(written > 0 && out[written - 1] == ' ')
		written--;
	size_t lead = 0;
	while(lead < written && out[lead] == ' ')
		lead++;
	if(lead) {
		memmove(out, out + lead, written - lead);
		written -= lead;
	}
	out[written] = '\0';
	return written;
}

/* P1: world interaction and inventory (S2). */
bool mp_encode_player_inventory(struct mp_frame* f, int32_t entity_id, int16_t slot, int16_t item_id, int16_t item_damage) { return begin_frame(f, MP_PACKET_PLAYER_INVENTORY) && append_i32(f, entity_id) && append_i16(f, slot) && append_i16(f, item_id) && append_i16(f, item_damage); }
bool mp_encode_use_entity(struct mp_frame* f, int32_t player_id, int32_t target_id, uint8_t is_left_click) { return begin_frame(f, MP_PACKET_USE_ENTITY) && append_i32(f, player_id) && append_i32(f, target_id) && append_u8(f, is_left_click); }
bool mp_encode_respawn(struct mp_frame* f, int8_t dimension) { return begin_frame(f, MP_PACKET_RESPAWN) && append_u8(f, (uint8_t)dimension); }
bool mp_encode_block_dig(struct mp_frame* f, uint8_t status, int32_t x, uint8_t y, int32_t z, uint8_t face) { return begin_frame(f, MP_PACKET_BLOCK_DIG) && append_u8(f, status) && append_i32(f, x) && append_u8(f, y) && append_i32(f, z) && append_u8(f, face); }
bool mp_encode_place(struct mp_frame* f, int32_t x, uint8_t y, int32_t z, uint8_t direction, const struct mp_item_stack* item) { return begin_frame(f, MP_PACKET_PLACE) && append_i32(f, x) && append_u8(f, y) && append_i32(f, z) && append_u8(f, direction) && append_item_stack(f, item); }
bool mp_encode_block_item_switch(struct mp_frame* f, int16_t slot) { return begin_frame(f, MP_PACKET_BLOCK_ITEM_SWITCH) && append_i16(f, slot); }
bool mp_encode_entity_action(struct mp_frame* f, int32_t entity_id, uint8_t state) { return begin_frame(f, MP_PACKET_ENTITY_ACTION) && append_i32(f, entity_id) && append_u8(f, state); }
bool mp_encode_multi_block_change(struct mp_frame* f, const struct mp_multi_block_change* c) {
	if(!c || (c->count > 0 && (!c->coordinate || !c->type || !c->metadata))) return false;
	if(!begin_frame(f, MP_PACKET_MULTI_BLOCK_CHANGE) || !append_i32(f, c->chunk_x)
	   || !append_i32(f, c->chunk_z) || !append_i16(f, (int16_t)(uint16_t)c->count)) return false;
	for(uint16_t i = 0; i < c->count; i++)
		if(!append_i16(f, (int16_t)c->coordinate[i])) return false;
	for(uint16_t i = 0; i < c->count; i++)
		if(!append_u8(f, c->type[i])) return false;
	for(uint16_t i = 0; i < c->count; i++)
		if(!append_u8(f, c->metadata[i])) return false;
	return true;
}
bool mp_encode_block_change(struct mp_frame* f, int32_t x, uint8_t y, int32_t z, uint8_t block_type, uint8_t metadata) { return begin_frame(f, MP_PACKET_BLOCK_CHANGE) && append_i32(f, x) && append_u8(f, y) && append_i32(f, z) && append_u8(f, block_type) && append_u8(f, metadata); }
bool mp_encode_open_window(struct mp_frame* f, int8_t window, uint8_t inventory_type, const char* title_utf8, uint8_t slot_count) { return begin_frame(f, MP_PACKET_OPEN_WINDOW) && append_u8(f, (uint8_t)window) && append_u8(f, inventory_type) && append_modified_utf8(f, title_utf8) && append_u8(f, slot_count); }
bool mp_encode_close_window(struct mp_frame* f, int8_t window) { return begin_frame(f, MP_PACKET_CLOSE_WINDOW) && append_u8(f, (uint8_t)window); }
bool mp_encode_window_click(struct mp_frame* f, int8_t window, int16_t slot, uint8_t button, int16_t action, bool shift, const struct mp_item_stack* item) { return begin_frame(f, MP_PACKET_WINDOW_CLICK) && append_u8(f, (uint8_t)window) && append_i16(f, slot) && append_u8(f, button) && append_i16(f, action) && append_u8(f, shift) && append_item_stack(f, item); }
bool mp_encode_set_slot(struct mp_frame* f, int8_t window, int16_t slot, const struct mp_item_stack* item) { return begin_frame(f, MP_PACKET_SET_SLOT) && append_u8(f, (uint8_t)window) && append_i16(f, slot) && append_item_stack(f, item); }
bool mp_encode_window_items(struct mp_frame* f, const struct mp_window_items* w) {
	if(!w || (w->count > 0 && !w->items)) return false;
	if(!begin_frame(f, MP_PACKET_WINDOW_ITEMS) || !append_u8(f, (uint8_t)w->window)
	   || !append_i16(f, (int16_t)w->count)) return false;
	for(uint16_t i = 0; i < w->count; i++)
		if(!append_item_stack(f, &w->items[i])) return false;
	return true;
}
bool mp_encode_update_progressbar(struct mp_frame* f, int8_t window, int16_t bar, int16_t value) { return begin_frame(f, MP_PACKET_UPDATE_PROGRESSBAR) && append_u8(f, (uint8_t)window) && append_i16(f, bar) && append_i16(f, value); }
bool mp_encode_transaction(struct mp_frame* f, int8_t window, int16_t action, bool accepted) { return begin_frame(f, MP_PACKET_TRANSACTION) && append_u8(f, (uint8_t)window) && append_i16(f, action) && append_u8(f, accepted); }
bool mp_encode_update_sign(struct mp_frame* f, int32_t x, int16_t y, int32_t z, const char* const lines_utf8[4]) {
	if(!lines_utf8) return false;
	if(!begin_frame(f, MP_PACKET_UPDATE_SIGN) || !append_i32(f, x) || !append_i16(f, y)
	   || !append_i32(f, z)) return false;
	for(int i = 0; i < 4; i++)
		if(!append_string16(f, lines_utf8[i] ? lines_utf8[i] : "", MP_SIGN_LINE_MAX)) return false;
	return true;
}

/* S3 P2. */
bool mp_encode_animation(struct mp_frame* f, int32_t entity_id, uint8_t animate) {
	return begin_frame(f, MP_PACKET_ANIMATION) && append_i32(f, entity_id)
		&& append_u8(f, animate);
}
bool mp_encode_position_legacy(struct mp_frame* f, float field_a, float field_b,
									 float field_e, float field_f, bool field_c,
									 bool field_d) {
	/* Wire order exactly matches Packet27Position.writePacketData: a,b,e,f
	 * then c,d. [740c583 Packet27Position.writePacketData] */
	return begin_frame(f, MP_PACKET_POSITION_LEGACY) && append_f32(f, field_a)
		&& append_f32(f, field_b) && append_f32(f, field_e)
		&& append_f32(f, field_f) && append_u8(f, field_c)
		&& append_u8(f, field_d);
}

double mp_fixed32_to_double(int32_t value) { return (double)value / 32.0; }

float mp_byte_angle_to_degrees(int8_t value) {
	/* Rotation byte times 360, over 256.0F -- the multiply happens in Java
	 * int arithmetic (byte sign-extends to int first), THEN casts to float.
	 * [740c583 NetClientHandler.handleNamedEntitySpawn, NetClientHandler.java:184] */
	return (float)((int32_t)value * 360) / 256.0f;
}

bool mp_utf16_equals_ascii(const struct mp_utf16* text, const char* ascii) {
	if(!text || !ascii || strlen(ascii) != text->length) return false;
	for(size_t i = 0; i < text->length; i++) if(text->units[i] != (uint8_t)ascii[i]) return false;
	return true;
}

bool mp_utf16_to_utf8(const struct mp_utf16* text, char* output, size_t output_size) {
	if(!text || !output || output_size == 0) return false;
	size_t out = 0;
	for(size_t i = 0; i < text->length; i++) {
		uint32_t cp = text->units[i];
		if(cp >= 0xD800 && cp <= 0xDBFF) {
			if(i + 1 >= text->length || text->units[i + 1] < 0xDC00 || text->units[i + 1] > 0xDFFF) return false;
			cp = 0x10000 + ((cp - 0xD800) << 10) + (text->units[++i] - 0xDC00);
		} else if(cp >= 0xDC00 && cp <= 0xDFFF) return false;
		uint8_t bytes[4]; size_t count;
		if(cp < 0x80) { bytes[0] = (uint8_t)cp; count = 1; }
		else if(cp < 0x800) { bytes[0] = 0xC0 | (uint8_t)(cp >> 6); bytes[1] = 0x80 | (uint8_t)(cp & 0x3F); count = 2; }
		else if(cp < 0x10000) { bytes[0] = 0xE0 | (uint8_t)(cp >> 12); bytes[1] = 0x80 | (uint8_t)((cp >> 6) & 0x3F); bytes[2] = 0x80 | (uint8_t)(cp & 0x3F); count = 3; }
		else { bytes[0] = 0xF0 | (uint8_t)(cp >> 18); bytes[1] = 0x80 | (uint8_t)((cp >> 12) & 0x3F); bytes[2] = 0x80 | (uint8_t)((cp >> 6) & 0x3F); bytes[3] = 0x80 | (uint8_t)(cp & 0x3F); count = 4; }
		if(out + count >= output_size) return false;
		memcpy(output + out, bytes, count); out += count;
	}
	output[out] = '\0';
	return true;
}

/* mp_utf16_to_utf8 above emits REAL UTF-8 for U+00A7 (Beta's section-sign
 * colour-code marker) as the 2-byte sequence 0xC2 0xA7. This engine's font
 * renderer is NOT a UTF-8 consumer: gutil_text_rgb (source/graphics/
 * gui_util.c) indexes its 256-glyph atlas one BYTE at a time and recognises
 * the single byte 0xA7 followed by one of [0-9a-f] as a chat colour code --
 * Beta's own single-byte charset convention, not Unicode. Feeding it
 * mp_utf16_to_utf8's output draws a garbage 0xC2 glyph before every colour
 * change on any line a real Beta server colours. mp_utf16_to_display is the
 * codec entry point for that single-byte display charset: same hostile-input
 * posture as mp_utf16_to_utf8 above (a lone/malformed surrogate REJECTS the
 * whole line), but the OUTPUT mapping differs -- exactly one byte per
 * accepted code point: 0x20..0x7E verbatim, U+00A7 to the raw byte 0xA7,
 * every other control code point (<0x20, incl. newline/tab) to ' ', and
 * every other code point (incl. every astral surrogate-pair result) to '?'.
 * Because the mapping never grows (surrogate PAIRS shrink two input units to
 * one output byte), `text->length + 1` output bytes always fit the NUL --
 * unlike mp_utf16_to_utf8 above, which can need up to 4x that for astral
 * code points. */
bool mp_utf16_to_display(const struct mp_utf16* text, char* output, size_t output_size) {
	if(!text || !output || output_size == 0) return false;
	size_t out = 0;
	for(size_t i = 0; i < text->length; i++) {
		uint32_t cp = text->units[i];
		if(cp >= 0xD800 && cp <= 0xDBFF) {
			if(i + 1 >= text->length || text->units[i + 1] < 0xDC00 || text->units[i + 1] > 0xDFFF) return false;
			cp = 0x10000 + ((cp - 0xD800) << 10) + (text->units[++i] - 0xDC00);
		} else if(cp >= 0xDC00 && cp <= 0xDFFF) return false;
		char byte;
		if(cp >= 0x20 && cp <= 0x7E) byte = (char)cp;
		else if(cp == 0xA7) byte = (char)0xA7;
		else if(cp < 0x20) byte = ' ';
		else byte = '?';
		if(out + 1 >= output_size) return false;
		output[out++] = byte;
	}
	output[out] = '\0';
	return true;
}

/* Small fixed-capacity token list for the ':' split below -- real server
 * addresses never approach this, and MP_ADDR_SPLIT_CAP tokens is already
 * enough to positively identify the >2-segment collapse case. */
#define MP_ADDR_SPLIT_CAP 8
struct mp_addr_token {
	size_t start, len;
};

/* Java's String.split(":") (single literal-char fast path, limit=0)
 * [java.lang.String.split]: with NO ':' present, returns {s} untouched
 * (even s==""); otherwise every ':' is a zero-width delimiter producing
 * len+1 raw segments (empty segments allowed in the middle/front), and
 * trailing EMPTY segments are then stripped off the end only. Exercised
 * verbatim by 740c583 GuiMultiplayer's own colon split (GuiMultiplayer.java:42). */
static size_t java_split_colon(const char* s, struct mp_addr_token* out,
							   size_t cap) {
	if(!strchr(s, ':')) {
		if(cap > 0) {
			out[0].start = 0;
			out[0].len = strlen(s);
		}
		return 1;
	}
	size_t n = 0, start = 0, i = 0;
	for(;; i++) {
		if(s[i] == ':' || s[i] == '\0') {
			if(n < cap) {
				out[n].start = start;
				out[n].len = i - start;
			}
			n++;
			if(s[i] == '\0')
				break;
			start = i + 1;
		}
	}
	size_t stored = n < cap ? n : cap;
	while(stored > 0 && out[stored - 1].len == 0)
		stored--;
	return stored;
}

/* Java's Integer.parseInt(String.trim()), but bounded to a legal port
 * (0..65535 -- Beta's own int has no such ceiling, but a uint16_t wire port
 * cannot represent more, and parseIntWithDefault's own contract is "any
 * parse trouble falls back to the default" [740c583 GuiMultiplayer
 * .parseIntWithDefault]). */
static uint16_t parse_int_with_default(const char* s, size_t len,
									  uint16_t fallback) {
	char buf[16];
	if(len >= sizeof(buf))
		return fallback;
	memcpy(buf, s, len);
	buf[len] = '\0';
	size_t b = 0, e = len;
	while(b < e && isspace((unsigned char)buf[b])) b++;
	while(e > b && isspace((unsigned char)buf[e - 1])) e--;
	if(b == e)
		return fallback;
	char trimmed[16];
	memcpy(trimmed, buf + b, e - b);
	trimmed[e - b] = '\0';
	char* endp = NULL;
	long value = strtol(trimmed, &endp, 10);
	if(!endp || *endp != '\0' || value < 0 || value > 65535)
		return fallback;
	return (uint16_t)value;
}

bool mp_address_parse(const char* input, char* host_out, size_t host_cap,
					  uint16_t* port_out) {
	if(!input || !host_out || host_cap == 0 || !port_out)
		return false;

	/* Java String.trim(): strip any char <= 0x20 from both ends. [740c583
	 * String.trim, exercised by GuiMultiplayer's own .trim() call] */
	size_t len = strlen(input);
	size_t b = 0, e = len;
	while(b < e && (unsigned char)input[b] <= 0x20) b++;
	while(e > b && (unsigned char)input[e - 1] <= 0x20) e--;

	char trimmed[MP_ADDRESS_INPUT_MAX + 1];
	size_t tlen = e - b;
	if(tlen > MP_ADDRESS_INPUT_MAX)
		tlen = MP_ADDRESS_INPUT_MAX; /* defensive; the GUI field itself caps input at 128 */
	memcpy(trimmed, input + b, tlen);
	trimmed[tlen] = '\0';

	const char* host_start = trimmed;
	size_t host_len = tlen;
	bool have_port = false;
	const char* port_start = NULL;
	size_t port_len = 0;
	bool bracket_handled = false;

	if(trimmed[0] == '[') {
		const char* close = strchr(trimmed, ']');
		if(close) {
			bracket_handled = true;
			host_start = trimmed + 1;
			host_len = (size_t)(close - trimmed - 1);
			const char* rem = close + 1;
			size_t rb = 0, re = strlen(rem);
			while(rb < re && (unsigned char)rem[rb] <= 0x20) rb++;
			while(re > rb && (unsigned char)rem[re - 1] <= 0x20) re--;
			if(re > rb && rem[rb] == ':') {
				have_port = true;
				port_start = rem + rb + 1;
				port_len = re - (rb + 1);
			}
		}
	}

	if(!bracket_handled) {
		struct mp_addr_token toks[MP_ADDR_SPLIT_CAP];
		size_t n = java_split_colon(trimmed, toks, MP_ADDR_SPLIT_CAP);
		if(n == 0)
			return false; /* Beta's own client crashes here; fail closed instead. */
		if(n > 2) {
			host_start = trimmed;
			host_len = tlen;
		} else {
			host_start = trimmed + toks[0].start;
			host_len = toks[0].len;
			if(n == 2) {
				have_port = true;
				port_start = trimmed + toks[1].start;
				port_len = toks[1].len;
			}
		}
	}

	if(host_len + 1 > host_cap)
		return false;
	memcpy(host_out, host_start, host_len);
	host_out[host_len] = '\0';

	*port_out = 25565;
	if(have_port)
		*port_out = parse_int_with_default(port_start, port_len, 25565);
	return true;
}

bool mp_login_username_valid(const char* username_utf8) {
	if(!username_utf8)
		return false;
	struct mp_frame scratch;
	return mp_encode_login(&scratch, username_utf8);
}

bool mp_username_chosen(const char* username_utf8) {
	if(!username_utf8 || !username_utf8[0]
	   || !mp_login_username_valid(username_utf8))
		return false;
	size_t len = 0;
	for(const char* c = username_utf8; *c; c++, len++) {
		bool ok = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z')
			|| (*c >= '0' && *c <= '9') || *c == '_';
		if(!ok || len >= 16)
			return false;
	}
	/* The reserved project name, case-insensitively: never a player. */
	static const char reserved[] = "cavewii";
	if(len == sizeof(reserved) - 1) {
		bool same = true;
		for(size_t i = 0; i < len && same; i++) {
			char c = username_utf8[i];
			if(c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
			same = c == reserved[i];
		}
		if(same)
			return false;
	}
	return true;
}
