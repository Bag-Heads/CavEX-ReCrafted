/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Beta 1.7.3 protocol-14 P0 incremental codec. */
#ifndef MP_CODEC_H
#define MP_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* S2 P1 (world interaction/inventory) frames are bigger than S0/S1's fixed
 * movement bodies: Packet100's title + Packet102/103/104's item stacks push a
 * realistic golden vector past 256 bytes. 1024 comfortably covers every real
 * C->S production frame (all under ~150 bytes: Packet130's four 15-code-unit
 * lines is the largest) plus generous S->C test fixtures (a Packet104 golden
 * with a handful of slots). Packet51/Packet52/Packet68's own hostile-length
 * ceilings stay independent decoder caps below -- this bound only limits what
 * mp_encode_* can build into one struct mp_frame. */
#define MP_FRAME_CAPACITY 1024
#define MP_S0_FRAME_CAPACITY MP_FRAME_CAPACITY
/* Covers every string16 maximum this codec decodes: Packet255's 100-code-unit
 * disconnect reason, Packet2's 32-unit handshake, Packet1's 16-unit username,
 * Packet130's 15-unit sign lines, and (S4) Packet3's 119-unit chat cap -- the
 * largest of the set, so it sets the buffer size (120 = 119 + 1, matching the
 * "cap + 1" margin MP_WINDOW_TITLE_CAPACITY already uses below). [740c583
 * Packet3Chat.readPacketData] */
#define MP_TEXT_CAPACITY 120
#define MP_S0_TEXT_CAPACITY MP_TEXT_CAPACITY
#define MP_CHUNK_COMPRESSED_CAP (1024u * 1024u)
/* PlayerInstance.java:145-153 rounds a top-edge partial to y+sy=130;
 * World.getChunkData clamps the copied data to 128, padding this buffer. */
#define MP_CHUNK_INFLATED_CAP (16u * 130u * 16u * 5u / 2u)
/* Packet52's size field is `readShort() & 0xFFFF`: the wire's own faithful
 * range is 0..65535, not an invented cap. [740c583
 * Packet52MultiBlockChange.readPacketData] */
#define MP_MULTIBLOCK_MAX_ENTRIES 65535u
/* Packet104's count is a signed readShort(); a negative Java array size
 * throws, so only 0..32767 is a legal array length. [740c583
 * Packet104WindowItems.readPacketData] */
#define MP_WINDOW_ITEMS_MAX_ENTRIES 32767u
/* Packet100's title uses DataInputStream.readUTF (Java "modified UTF-8"):
 * a u16 BYTE-length prefix, no protocol-mandated content cap. A real window
 * title is a short localization key ("container.chest"); this is a resource
 * bound we choose to protect the decoder from a hostile/buggy peer, not a
 * value transcribed from the decompile. */
#define MP_WINDOW_TITLE_BYTE_CAP 240u
#define MP_WINDOW_TITLE_CAPACITY (MP_WINDOW_TITLE_BYTE_CAP + 1u)
/* Packet130's readString limit caps each sign line at 15 Java chars.
 * [740c583 Packet130UpdateSign.readPacketData] */
#define MP_SIGN_LINE_MAX 15u
#define MP_SIGN_LINE_COUNT 4u

/* S3 P2 (entities/other players) string caps. [740c583
 * Packet20NamedEntitySpawn.readPacketData] */
#define MP_NAMED_ENTITY_NAME_MAX 16u
/* EnumArt.maxArtTitleLength == "SkullAndRoses".length(). [740c583
 * Packet25EntityPainting.readPacketData] [740c583 EnumArt] */
#define MP_PAINTING_TITLE_MAX 13u
/* DataWatcher string entries (type 4) read via Packet.readString, limit 64.
 * [740c583 DataWatcher.readWatchableObjects] */
#define MP_METADATA_STRING_MAX 64u
/* DataWatcher's own wire loop has no entry-count field -- it runs until a
 * literal 0x7F terminator -- but a real Beta entity never owns more than 32
 * ids (`addObject` throws above 31) [740c583 DataWatcher.addObject]. Bound
 * the decoded array at that faithful ceiling so a hostile/buggy peer cannot
 * grow it unboundedly before ever sending the terminator. */
#define MP_METADATA_MAX_ENTRIES 32u

/* S4 P3 (misc/polish). Packet60's destroyedBlockPositions count has no
 * wire-mandated ceiling (Beta's own reader trusts the raw i32), but Beta's
 * OWN writer can never exceed doExplosionA's fixed 16x16x16 outer-shell
 * sample (16^3 - 14^3 = 1352 cells) regardless of explosionSize -- that grid
 * size is a compile-time constant, not scaled by power. 4096 gives >3x
 * headroom over any legitimate Beta explosion purely as a decoder resource
 * bound against a hostile/buggy peer, matching MP_WINDOW_ITEMS_MAX_ENTRIES's
 * own "deliberately smaller in-process convenience cap, not a wire limit"
 * precedent. [740c583 Explosion.doExplosionA] [740c583
 * Packet60Explosion.readPacketData] */
#define MP_EXPLOSION_MAX_BLOCKS 4096u
/* Packet131's byte-array length is `readByte() & 255` -- an unsigned byte,
 * inherently bounded to 0..255 by the wire format itself (no separate cap
 * needed before allocation; a fixed 255-byte field is used instead of a heap
 * allocation). [740c583 Packet131MapData.readPacketData] */
#define MP_MAP_DATA_MAX_BYTES 255u

enum mp_packet_id {
	MP_PACKET_KEEP_ALIVE = 0x00,
	MP_PACKET_LOGIN = 0x01,
	MP_PACKET_HANDSHAKE = 0x02,
	/* S4 P3: the only bidirectional P3 id -- appears alongside the P0/P1/P2
	 * bidirectional ids below, not with the S->C-only P3 block further down.
	 * [740c583 Packet3Chat] */
	MP_PACKET_CHAT = 0x03,
	MP_PACKET_UPDATE_TIME = 0x04,
	MP_PACKET_PLAYER_INVENTORY = 0x05,
	MP_PACKET_SPAWN_POSITION = 0x06,
	MP_PACKET_USE_ENTITY = 0x07,
	MP_PACKET_UPDATE_HEALTH = 0x08,
	MP_PACKET_RESPAWN = 0x09,
	MP_PACKET_FLYING = 0x0A,
	MP_PACKET_PLAYER_POSITION = 0x0B,
	MP_PACKET_PLAYER_LOOK = 0x0C,
	MP_PACKET_PLAYER_LOOK_MOVE = 0x0D,
	MP_PACKET_BLOCK_DIG = 0x0E,
	MP_PACKET_PLACE = 0x0F,
	MP_PACKET_BLOCK_ITEM_SWITCH = 0x10,
	MP_PACKET_ENTITY_ACTION = 0x13,
	/* S3 P2: entities and remote players. */
	MP_PACKET_SLEEP = 0x11,
	MP_PACKET_ANIMATION = 0x12,
	MP_PACKET_NAMED_ENTITY_SPAWN = 0x14,
	MP_PACKET_PICKUP_SPAWN = 0x15,
	MP_PACKET_COLLECT = 0x16,
	MP_PACKET_VEHICLE_SPAWN = 0x17,
	MP_PACKET_MOB_SPAWN = 0x18,
	MP_PACKET_ENTITY_PAINTING = 0x19,
	/* C->S only, and never actually constructed anywhere in the 740c583
	 * client tree (nothing ever constructs a Packet27Position) -- registered but
	 * dead protocol code [740c583 Packet.addIdClassMapping] [740c583
	 * NetHandler.func_22185_a]. Encode-only, kept for wire completeness;
	 * nothing in server_remote calls it (see mp_codec.c's encoder comment). */
	MP_PACKET_POSITION_LEGACY = 0x1B,
	MP_PACKET_ENTITY_VELOCITY = 0x1C,
	MP_PACKET_DESTROY_ENTITY = 0x1D,
	MP_PACKET_ENTITY = 0x1E,
	MP_PACKET_REL_ENTITY_MOVE = 0x1F,
	MP_PACKET_ENTITY_LOOK = 0x20,
	MP_PACKET_REL_ENTITY_MOVE_LOOK = 0x21,
	MP_PACKET_ENTITY_TELEPORT = 0x22,
	MP_PACKET_ENTITY_STATUS = 0x26,
	MP_PACKET_ATTACH_ENTITY = 0x27,
	MP_PACKET_ENTITY_METADATA = 0x28,
	MP_PACKET_PRE_CHUNK = 0x32,
	MP_PACKET_MAP_CHUNK = 0x33,
	MP_PACKET_MULTI_BLOCK_CHANGE = 0x34,
	MP_PACKET_BLOCK_CHANGE = 0x35,
	MP_PACKET_OPEN_WINDOW = 0x64,
	MP_PACKET_CLOSE_WINDOW = 0x65,
	MP_PACKET_WINDOW_CLICK = 0x66,
	MP_PACKET_SET_SLOT = 0x67,
	MP_PACKET_WINDOW_ITEMS = 0x68,
	MP_PACKET_UPDATE_PROGRESSBAR = 0x69,
	MP_PACKET_TRANSACTION = 0x6A,
	MP_PACKET_UPDATE_SIGN = 0x82,
	/* S4 P3: misc/polish, all S->C only [740c583 Packet]. */
	MP_PACKET_NOTE_BLOCK = 0x36,
	MP_PACKET_EXPLOSION = 0x3C,
	MP_PACKET_DOOR_CHANGE = 0x3D,
	MP_PACKET_BED = 0x46,
	MP_PACKET_WEATHER = 0x47,
	MP_PACKET_MAP_DATA = 0x83,
	MP_PACKET_STATISTIC = 0xC8,
	MP_PACKET_DISCONNECT = 0xFF,
};

enum mp_direction {
	MP_DIRECTION_SERVER_TO_CLIENT,
	MP_DIRECTION_CLIENT_TO_SERVER,
};

struct mp_frame {
	uint8_t data[MP_FRAME_CAPACITY];
	size_t length;
};

/* Client->server-only P1 ids (UseEntity/BlockDig/Place/BlockItemSwitch/
 * EntityAction/WindowClick) have no MP_EVENT_* / decode path: a faithful
 * Beta client never receives them (Packet.readPacket's own direction check
 * rejects them before dispatch [740c583 Packet]), so mp_encode_* is their
 * only production entry point. See mp_decoder_feed's dispatch note. */
enum mp_event_type {
	MP_EVENT_KEEP_ALIVE,
	MP_EVENT_LOGIN,
	MP_EVENT_HANDSHAKE,
	MP_EVENT_UPDATE_TIME,
	MP_EVENT_PLAYER_INVENTORY,
	MP_EVENT_SPAWN_POSITION,
	MP_EVENT_UPDATE_HEALTH,
	MP_EVENT_RESPAWN,
	MP_EVENT_FLYING,
	MP_EVENT_PLAYER_POSITION,
	MP_EVENT_PLAYER_LOOK,
	MP_EVENT_PLAYER_LOOK_MOVE,
	MP_EVENT_PRE_CHUNK,
	MP_EVENT_MAP_CHUNK,
	MP_EVENT_MULTI_BLOCK_CHANGE,
	MP_EVENT_BLOCK_CHANGE,
	MP_EVENT_OPEN_WINDOW,
	MP_EVENT_CLOSE_WINDOW,
	MP_EVENT_SET_SLOT,
	MP_EVENT_WINDOW_ITEMS,
	MP_EVENT_UPDATE_PROGRESSBAR,
	MP_EVENT_TRANSACTION,
	MP_EVENT_UPDATE_SIGN,
	MP_EVENT_DISCONNECT,
	/* S3 P2. MP_PACKET_POSITION_LEGACY has no event/decode case -- see the
	 * enum mp_packet_id comment and mp_decoder_feed's dispatch note. */
	MP_EVENT_SLEEP,
	MP_EVENT_ANIMATION,
	MP_EVENT_NAMED_ENTITY_SPAWN,
	MP_EVENT_PICKUP_SPAWN,
	MP_EVENT_COLLECT,
	MP_EVENT_VEHICLE_SPAWN,
	MP_EVENT_MOB_SPAWN,
	MP_EVENT_ENTITY_PAINTING,
	MP_EVENT_ENTITY_VELOCITY,
	MP_EVENT_DESTROY_ENTITY,
	MP_EVENT_ENTITY,
	MP_EVENT_REL_ENTITY_MOVE,
	MP_EVENT_ENTITY_LOOK,
	MP_EVENT_REL_ENTITY_MOVE_LOOK,
	MP_EVENT_ENTITY_TELEPORT,
	MP_EVENT_ENTITY_STATUS,
	MP_EVENT_ATTACH_ENTITY,
	MP_EVENT_ENTITY_METADATA,
	/* S4 P3 (misc/polish). */
	MP_EVENT_CHAT,
	MP_EVENT_NOTE_BLOCK,
	MP_EVENT_EXPLOSION,
	MP_EVENT_DOOR_CHANGE,
	MP_EVENT_BED,
	MP_EVENT_WEATHER,
	MP_EVENT_MAP_DATA,
	MP_EVENT_STATISTIC,
};

struct mp_utf16 {
	uint16_t units[MP_TEXT_CAPACITY];
	size_t length;
};

struct mp_movement {
	double x, y, stance, z;
	float yaw, pitch;
	bool moving, rotating, on_ground;
};

struct mp_map_chunk {
	int32_t x, z;
	int16_t y;
	uint16_t sx, sy, sz;
	size_t inflated_length;
	uint8_t* compressed;
	size_t compressed_length;
};

/* Shared ItemStack-on-the-wire shape used by Packet15/102/103/104: i16 id;
 * id < 0 means "null stack" and no further bytes follow; id >= 0 reads i8
 * count then i16 damage. [740c583 Packet15Place.readPacketData] (every other
 * user reads the identical three fields in the identical order). */
struct mp_item_stack {
	bool present;
	int16_t id;
	uint8_t count;
	int16_t damage;
};

/* DataWatcher entry: header byte is (type<<5 | index&31); type 0..6, decoded
 * per Beta's own switch. [740c583 DataWatcher.readWatchableObjects] */
struct mp_watchable_entry {
	uint8_t index; /* 0..31 */
	uint8_t type;  /* 0 byte, 1 short, 2 int, 3 float, 4 string, 5 item, 6 coord */
	union {
		int8_t byte_value;
		int16_t short_value;
		int32_t int_value;
		float float_value;
		struct mp_utf16 string_value;
		struct mp_item_stack item_value;
		struct { int32_t x, y, z; } coord_value; /* ChunkCoordinates */
	} value;
};

/* readWatchableObjects has no entry-count prefix -- it is a literal 0x7F
 * terminated stream [740c583 DataWatcher.readWatchableObjects]; count is
 * however many entries were actually decoded, bounded by
 * MP_METADATA_MAX_ENTRIES. */
struct mp_metadata {
	uint8_t count;
	struct mp_watchable_entry entries[MP_METADATA_MAX_ENTRIES];
};

/* Packet60's per-entry destroyed-block offset: three signed bytes, added to
 * the explosion origin's (int)x/y/z to get the absolute coordinate. [740c583
 * Packet60Explosion.readPacketData] */
struct mp_explosion_offset {
	int8_t dx, dy, dz;
};

struct mp_multi_block_change {
	int32_t chunk_x, chunk_z;
	uint16_t count;
	/* Each owned array is `count` entries; all three are NULL/freed together.
	 * coordinate[i] is the raw packed short: bits 15..12 local x, 11..8 local
	 * z, 7..0 local y. [740c583 NetClientHandler.handleMultiBlockChange] */
	uint16_t* coordinate;
	uint8_t* type;
	uint8_t* metadata;
};

struct mp_window_items {
	int8_t window;
	uint16_t count;
	struct mp_item_stack* items; /* owned, `count` entries */
};

struct mp_event {
	enum mp_event_type type;
	union {
		struct {
			int32_t entity_id;
			struct mp_utf16 username;
			int64_t map_seed;
			int8_t dimension;
		} login;
		struct mp_utf16 handshake;
		int64_t world_time;
		struct {
			int32_t entity_id;
			int16_t slot;
			int16_t item_id;
			int16_t item_damage;
		} player_inventory;
		struct { int32_t x, y, z; } spawn_position;
		int16_t health;
		int8_t respawn_dimension;
		struct mp_movement movement;
		struct { int32_t chunk_x, chunk_z; bool mode; } pre_chunk;
		struct mp_map_chunk map_chunk;
		struct mp_multi_block_change multi_block_change;
		struct {
			int32_t x;
			uint8_t y;
			int32_t z;
			uint8_t block_type;
			uint8_t metadata;
		} block_change;
		struct {
			int8_t window;
			uint8_t inventory_type;
			char title[MP_WINDOW_TITLE_CAPACITY]; /* decoded to plain UTF-8 */
			uint8_t slot_count;
		} open_window;
		int8_t close_window;
		struct {
			int8_t window;
			int16_t slot;
			struct mp_item_stack item;
		} set_slot;
		struct mp_window_items window_items;
		struct { int8_t window; int16_t bar; int16_t value; } update_progressbar;
		struct { int8_t window; int16_t action; bool accepted; } transaction;
		struct {
			int32_t x;
			int16_t y;
			int32_t z;
			struct mp_utf16 lines[MP_SIGN_LINE_COUNT];
		} update_sign;
		struct mp_utf16 disconnect_reason;
		/* S3 P2. */
		struct {
			int32_t entity_id;
			int8_t unknown; /* Beta only sleeps when this is 0; else dead */
			int32_t bed_x;
			int8_t bed_y;
			int32_t bed_z;
		} sleep;
		struct { int32_t entity_id; uint8_t animate; } animation;
		struct {
			int32_t entity_id;
			struct mp_utf16 name;
			int32_t x, y, z;	 /* fixed-point, /32.0 */
			int8_t rotation, pitch; /* byte-angle, *360/256 */
			int16_t current_item;   /* 0 = empty */
		} named_entity_spawn;
		struct {
			int32_t entity_id;
			int16_t item_id;
			uint8_t count;
			int16_t item_damage;
			int32_t x, y, z;			  /* fixed-point, /32.0 */
			int8_t vel_x, vel_y, vel_z; /* /128.0 -- velocity, NOT an angle */
		} pickup_spawn;
		struct { int32_t collected_entity_id, collector_entity_id; } collect;
		struct {
			int32_t entity_id;
			uint8_t vtype;
			int32_t x, y, z; /* fixed-point, /32.0 */
			int32_t thrower_entity_id;
			bool has_velocity; /* thrower_entity_id > 0 */
			int16_t speed_x, speed_y, speed_z; /* /8000.0 */
		} vehicle_spawn;
		struct {
			int32_t entity_id;
			uint8_t mtype;
			int32_t x, y, z;		 /* fixed-point, /32.0 */
			int8_t yaw, pitch;		 /* byte-angle, *360/256 */
			struct mp_metadata metadata;
		} mob_spawn;
		struct {
			int32_t entity_id;
			struct mp_utf16 title;
			int32_t x, y, z; /* raw block coordinates, NOT fixed-point */
			int32_t direction;
		} entity_painting;
		struct {
			int32_t entity_id;
			int16_t vel_x, vel_y, vel_z; /* /8000.0 */
		} entity_velocity;
		struct { int32_t entity_id; } destroy_entity;
		struct { int32_t entity_id; } entity_heartbeat;
		struct {
			int32_t entity_id;
			int8_t dx, dy, dz; /* /32.0 deltas, added to the tracked position */
		} rel_entity_move;
		struct {
			int32_t entity_id;
			int8_t yaw, pitch; /* byte-angle, *360/256 */
		} entity_look;
		struct {
			int32_t entity_id;
			int8_t dx, dy, dz;
			int8_t yaw, pitch;
		} rel_entity_move_look;
		struct {
			int32_t entity_id;
			int32_t x, y, z;   /* fixed-point, /32.0 (absolute) */
			int8_t yaw, pitch; /* byte-angle, *360/256 */
		} entity_teleport;
		struct { int32_t entity_id; int8_t status; } entity_status;
		struct {
			int32_t entity_id;
			int32_t vehicle_entity_id;
		} attach_entity;
		struct {
			int32_t entity_id;
			struct mp_metadata metadata;
		} entity_metadata;
		/* S4 P3. */
		struct mp_utf16 chat; /* [740c583 Packet3Chat] */
		struct {
			/* xLocation(i32); yLocation(i16, widened from Java's read); zLocation(i32);
			 * instrumentType(u8, Java `read()` -- 0..255, not a `readByte()` sign-extend);
			 * pitch(u8, same `read()` shape). [740c583
			 * Packet54PlayNoteBlock.readPacketData] */
			int32_t x;
			int16_t y;
			int32_t z;
			uint8_t instrument;
			uint8_t pitch;
		} note_block;
		struct {
			/* explosionX/Y/Z(f64); explosionSize(f32); count(i32, validated <=
			 * MP_EXPLOSION_MAX_BLOCKS before `blocks` is allocated). [740c583
			 * Packet60Explosion.readPacketData] */
			double x, y, z;
			float power;
			uint16_t count;
			/* Owned, `count` entries; freed by mp_event_destroy. */
			struct mp_explosion_offset* blocks;
		} explosion;
		struct {
			/* Read/write order is field_28050_a(i32, effect id), field_28053_c(i32, x),
			 * field_28052_d(i8, y), field_28051_e(i32, z), field_28049_b(i32, data) --
			 * NOT the obfuscated field-declaration order. Beta's own handler forwards
			 * these positionally into World.playAuxSFX(effectId, x, y, z, data).
			 * [740c583 Packet61DoorChange.readPacketData] [740c583
			 * NetClientHandler.handleDoorChange] [740c583 World.playAuxSFX] */
			int32_t effect_id;
			int32_t x;
			int8_t y;
			int32_t z;
			int32_t data;
		} door_change;
		/* field_25019_b: 0 = "tile.bed.notValid" chat message (no rain change);
		 * 1 = start raining; 2 = stop raining (no message either code) -- Beta's
		 * own field_25020_a string table has exactly 3 slots, only index 0
		 * non-null. [740c583 Packet70Bed] [740c583
		 * NetClientHandler.handleBedEvent] */
		struct { int8_t event_code; } bed;
		struct {
			/* entityID(i32); isLightningBolt(i8); posX/Y/Z(i32, fixed-point /32.0,
			 * same scale as every other entity position). [740c583
			 * Packet71Weather.readPacketData] */
			int32_t entity_id;
			int8_t is_lightning_bolt;
			int32_t x, y, z;
		} weather;
		struct {
			/* field_28055_a(i16, item id -- compared against the map item's
			 * registered id); field_28054_b(i16, this map's damage/instance id);
			 * length(u8, `readByte() & 255`) then that many raw bytes. [740c583
			 * Packet131MapData.readPacketData] */
			int16_t item_id;
			int16_t map_id;
			uint8_t length;
			uint8_t bytes[MP_MAP_DATA_MAX_BYTES];
		} map_data;
		/* field_27052_a(i32, statistic id); field_27051_b(i8, increment amount).
		 * [740c583 Packet200Statistic.readPacketData] */
		struct { int32_t stat_id; int8_t amount; } statistic;
	} payload;
};

enum mp_decode_result {
	MP_DECODE_MORE,
	MP_DECODE_EVENT,
	MP_DECODE_ERROR,
};

struct mp_decoder {
	uint8_t packet_id;
	uint8_t phase;
	uint8_t scalar[64];
	size_t scalar_have, scalar_need;
	struct mp_utf16 text;
	size_t text_max;
	uint8_t text_byte;
	bool have_text_high;
	struct mp_event event;
	/* Generic hostile-length-checked raw byte sink, shared by Packet51's
	 * compressed body and Packet52's coordinate+type+metadata blob (both are
	 * "read exactly N network-controlled bytes into one owned buffer"). */
	uint8_t* chunk_body;
	size_t chunk_have, chunk_need;
	/* Item-stack sub-decode (Packet15/102/103/104 share the same 3-field
	 * shape); array_index/array_count drive Packet104's per-entry loop. */
	struct mp_item_stack item_scratch;
	uint16_t array_index, array_count;
	/* Packet130's 4-line loop reuses the text machinery; sign_line selects
	 * which of update_sign.lines[] the next finished text lands in. */
	uint8_t sign_line;
	/* Packet100's title is Java modified UTF-8 (readUTF), never string16:
	 * a u16 BYTE-length prefix followed by that many raw encoded bytes,
	 * decoded to plain UTF-8 on completion. [740c583
	 * Packet100OpenWindow.readPacketData] */
	uint8_t title_bytes[MP_WINDOW_TITLE_BYTE_CAP];
	size_t title_have, title_need;
	/* S3 P2 DataWatcher sub-decode (Packet24/Packet40 share it). metadata_active
	 * distinguishes a metadata string/item-stack value sub-decode from a
	 * top-level packet's own text/item field, so finish_text/finish_item route
	 * to the right place; metadata_target points at whichever event payload's
	 * mp_metadata is being filled (mob_spawn's or entity_metadata's). [740c583
	 * DataWatcher.readWatchableObjects] */
	bool metadata_active;
	struct mp_metadata* metadata_target;
	/* Sticky until init: lets the transport distinguish a hostile header from
	 * a recoverable-resource failure for its bounded-retention telemetry. */
	bool allocation_failed;
	uint8_t error_packet_id;
	uint8_t error_phase;
	/* F5a (review finding H2): bytes the refused Packet51 body asked for, so
	 * the capacity diagnosis can name the pathological column instead of only
	 * the budget. Meaningful only while allocation_failed is set. */
	uint32_t alloc_wanted;
	/* F5a (review finding H2): THE RECLAIM SEAM. When the Packet51 body
	 * allocation is refused, the decoder does not discard the packet: it asks
	 * this hook to free capacity losslessly and RETRIES the allocation once.
	 * Set per connection by remote_attach_decoder_reclaim
	 * (server_remote_transport.c); NULL (the mp_decoder_init default) keeps
	 * the old single-attempt behaviour, which is what the codec's own golden
	 * tests want. The hook returns true when it reclaimed something and the
	 * retry is worth making; false means it has already ended the session
	 * with the capacity reason, so the retry is pointless.
	 *
	 * It runs on the read thread from inside mp_decoder_feed, which the
	 * transport calls WITHOUT the remote lock held -- so the hook is free to
	 * take that lock, which is exactly what the reclaim needs. */
	bool (*reclaim)(void* ctx, size_t wanted);
	void* reclaim_ctx;
};

void mp_decoder_init(struct mp_decoder* decoder);
void mp_decoder_destroy(struct mp_decoder* decoder);
enum mp_decode_result mp_decoder_feed(struct mp_decoder* decoder, uint8_t byte,
									 struct mp_event* event_out);
void mp_event_destroy(struct mp_event* event);
void mp_codec_set_alloc_hook(void* (*alloc_fn)(size_t));
/* F5a: install (or clear, with a NULL fn) this decoder's body-allocation
 * reclaim seam -- see struct mp_decoder::reclaim. Per decoder, never a global:
 * the hook carries the connection whose reservation is being reclaimed. */
void mp_decoder_set_reclaim_hook(struct mp_decoder* decoder,
								 bool (*reclaim_fn)(void* ctx, size_t wanted),
								 void* ctx);

bool mp_packet_supported(uint8_t id, enum mp_direction direction);
bool mp_encode_keep_alive(struct mp_frame* frame);
bool mp_encode_handshake(struct mp_frame* frame, const char* username_utf8);
bool mp_encode_login(struct mp_frame* frame, const char* username_utf8);
bool mp_encode_update_time(struct mp_frame* frame, int64_t world_time);
bool mp_encode_spawn_position(struct mp_frame* frame, int32_t x, int32_t y,
									 int32_t z);
bool mp_encode_update_health(struct mp_frame* frame, int16_t health);
bool mp_encode_flying(struct mp_frame* frame, bool on_ground);
bool mp_encode_player_position(struct mp_frame* frame, double x, double y,
									  double stance, double z, bool on_ground);
bool mp_encode_player_look(struct mp_frame* frame, float yaw, float pitch,
								  bool on_ground);
bool mp_encode_player_look_move(struct mp_frame* frame, double x, double y,
									 double stance, double z, float yaw,
									 float pitch, bool on_ground);
bool mp_encode_pre_chunk(struct mp_frame* frame, int32_t chunk_x,
								int32_t chunk_z, bool mode);
bool mp_encode_map_chunk(struct mp_frame* frame, const struct mp_map_chunk* chunk);
bool mp_encode_disconnect(struct mp_frame* frame, const char* reason_utf8);
/* S4 P3: the only C->S-legal P3 id. Caps at 119 code units, per Packet3's own
 * read-side maximum [740c583 Packet3Chat.readPacketData]. Beta's own
 * CONSTRUCTOR silently truncates an overlong outbound message instead of
 * rejecting it [740c583 Packet3Chat(String)]; this deliberately follows the
 * REJECT-over-cap convention every other mp_encode_* string already uses in
 * this codec (mp_encode_handshake/_login/_disconnect), rather than adding a
 * second, inconsistent truncate-on-encode behaviour for just this one call.
 * No composer wires this in this slice -- see server_remote's own note. */
bool mp_encode_chat(struct mp_frame* frame, const char* message_utf8);

/* Outbound-chat sanitizer (betamc-readiness "Outbound chat" row): Beta's
 * composer cannot PRODUCE a violating message -- GuiChat.keyTyped admits a
 * char only while message.length() < 100 AND it is in the renderable
 * allowedCharacters set (no \247, no control chars) [740c583 GuiChat:40],
 * and Enter sends only a trimmed non-empty string. CB-lineage servers
 * (Poseidon NetServerHandler) KICK on both violations ("Chat message too
 * long" / "Illegal characters in chat"), so the production send path must
 * never pass a raw string through. This replicates the input filter at the
 * choke point: strips U+00A7 and C0/DEL controls, caps at 100 UTF-16 code
 * units (the COMPOSER's cap -- Packet3's 119 read-side max stays
 * mp_encode_chat's own contract), and reports an all-stripped/empty result
 * as 0. Full font.txt glyph membership is deliberately not replicated: the
 * kick-relevant classes are exactly length and the illegal characters.
 * Returns the sanitized byte length written to out (NUL-terminated), or 0
 * when nothing sendable remains. out_size must be >= 401 (100 code units
 * of up to 4 UTF-8 bytes + NUL). */
size_t mp_chat_sanitize_outbound(const char* message_utf8, char* out,
								 size_t out_size);

/* P1: world interaction and inventory (S2). */
bool mp_encode_player_inventory(struct mp_frame* frame, int32_t entity_id,
										int16_t slot, int16_t item_id,
										int16_t item_damage);
bool mp_encode_use_entity(struct mp_frame* frame, int32_t player_id,
								 int32_t target_id, uint8_t is_left_click);
bool mp_encode_respawn(struct mp_frame* frame, int8_t dimension);
bool mp_encode_block_dig(struct mp_frame* frame, uint8_t status, int32_t x,
								uint8_t y, int32_t z, uint8_t face);
bool mp_encode_place(struct mp_frame* frame, int32_t x, uint8_t y, int32_t z,
							uint8_t direction, const struct mp_item_stack* item);
bool mp_encode_block_item_switch(struct mp_frame* frame, int16_t slot);
bool mp_encode_entity_action(struct mp_frame* frame, int32_t entity_id,
									uint8_t state);
bool mp_encode_multi_block_change(struct mp_frame* frame,
										 const struct mp_multi_block_change* change);
bool mp_encode_block_change(struct mp_frame* frame, int32_t x, uint8_t y,
								   int32_t z, uint8_t block_type, uint8_t metadata);
bool mp_encode_open_window(struct mp_frame* frame, int8_t window,
								  uint8_t inventory_type, const char* title_utf8,
								  uint8_t slot_count);
bool mp_encode_close_window(struct mp_frame* frame, int8_t window);
bool mp_encode_window_click(struct mp_frame* frame, int8_t window, int16_t slot,
								   uint8_t button, int16_t action, bool shift,
								   const struct mp_item_stack* item);
bool mp_encode_set_slot(struct mp_frame* frame, int8_t window, int16_t slot,
							   const struct mp_item_stack* item);
bool mp_encode_window_items(struct mp_frame* frame,
								   const struct mp_window_items* items);
bool mp_encode_update_progressbar(struct mp_frame* frame, int8_t window,
										 int16_t bar, int16_t value);
bool mp_encode_transaction(struct mp_frame* frame, int8_t window,
								  int16_t action, bool accepted);
bool mp_encode_update_sign(struct mp_frame* frame, int32_t x, int16_t y,
								  int32_t z, const char* const lines_utf8[4]);

/* S3 P2. Both are the only C->S-legal ids in this range: 0x12 is
 * bidirectional [740c583 Packet.addIdClassMapping]; 0x1B is C->S only, and
 * per MP_PACKET_POSITION_LEGACY's comment above, Beta's own client never
 * actually calls it -- kept for wire completeness, never for production use. */
bool mp_encode_animation(struct mp_frame* frame, int32_t entity_id,
								uint8_t animate);
/* Packet27Position's own field names are Java-obfuscated
 * (field_22039_a/field_22038_b/field_22041_e/field_22040_f/field_22043_c/
 * field_22042_d) and its handler (NetHandler.func_22185_a) is a bare
 * registerPacket no-op with zero overriding subclasses -- there is no
 * decompile evidence for what the 4 floats / 2 booleans actually mean, only
 * their wire order. Parameters are named positionally (field_a..field_f) on
 * purpose rather than guessing semantics no live Beta code exercises.
 * [740c583 Packet27Position.writePacketData] */
bool mp_encode_position_legacy(struct mp_frame* frame, float field_a,
									  float field_b, float field_e, float field_f,
									  bool field_c, bool field_d);

/* Shared fixed-point/angle conversions so server_remote and the codec tests
 * apply IDENTICAL arithmetic to the same wire values -- no independent
 * reimplementation to drift from Beta's own scale. [740c583
 * NetClientHandler.handleNamedEntitySpawn et al.] */
double mp_fixed32_to_double(int32_t value);				/* value / 32.0 */
float mp_byte_angle_to_degrees(int8_t value); /* (value*360)/256.0f */

bool mp_utf16_equals_ascii(const struct mp_utf16* text, const char* ascii);
bool mp_utf16_to_utf8(const struct mp_utf16* text, char* output,
						 size_t output_size);

/* Single-byte DISPLAY charset transcode -- NOT UTF-8. gutil_text_rgb
 * (source/graphics/gui_util.c) indexes its 256-glyph font atlas one byte at
 * a time and parses the single byte 0xA7 (Beta's section-sign colour-code
 * marker) + one of [0-9a-f] as a chat colour code; mp_utf16_to_utf8 above
 * emits U+00A7 as the 2-byte UTF-8 sequence 0xC2 0xA7, which draws a
 * garbage 0xC2 glyph before every colour change instead. Use this for any
 * string that reaches gutil_text_rgb/gutil_text (chat lines, signs, window
 * titles); use mp_utf16_to_utf8 only where a genuine UTF-8 consumer is on
 * the other end. Same hostile-input posture as mp_utf16_to_utf8 (a lone/
 * malformed surrogate rejects the whole line); see mp_codec.c for the
 * exact one-byte-per-code-point mapping. */
bool mp_utf16_to_display(const struct mp_utf16* text, char* output,
						  size_t output_size);

/* Interim title-screen path (plan 3.3). GuiMultiplayer's own address field
 * is capped at 128 characters [740c583 GuiMultiplayer.initGui:
 * serverTextBox.setMaxStringLength(128)]; host_out must be at least this big
 * to always fit the whole-string collapse fallback described below. */
#define MP_ADDRESS_INPUT_MAX 128u
#define MP_ADDRESS_HOST_CAPACITY (MP_ADDRESS_INPUT_MAX + 1u)

/* Port of GuiMultiplayer.actionPerformed's address-field parse [740c583
 * GuiMultiplayer]: "[host]:port" (IPv6-safe bracket form), "host:port", or
 * bare "host"; the default port is 25565 whenever a port segment is absent
 * OR fails Integer.parseInt (Beta's own parseIntWithDefault swallows that
 * exception and falls back). More than two ':'-separated segments outside
 * brackets collapses to the WHOLE trimmed input as the host with the
 * default port, exactly like Beta's own
 * fallback to the untouched input string (GuiMultiplayer.java:57).
 *
 * host_out is NUL-terminated into a buffer of host_cap bytes (use
 * MP_ADDRESS_HOST_CAPACITY). Returns false only when the host will not fit,
 * or for the one input shape (a colon-only string, e.g. ":") for which
 * Java's own `String.split(":")` produces a ZERO-length array and Beta's
 * client then throws an uncaught ArrayIndexOutOfBoundsException reading
 * the absent first segment [740c583 GuiMultiplayer.java:42,61] -- CaveWii fails closed with a visible
 * error instead of replicating that crash. */
bool mp_address_parse(const char* input, char* host_out, size_t host_cap,
					  uint16_t* port_out);

/* Wraps mp_encode_login's own string16 encode (the protocol-version/seed/
 * dimension fields of the throwaway frame are irrelevant) so "is this
 * username connectable" can never drift from the wire's actual
 * 16-code-unit login maximum [740c583 Packet1Login.readPacketData]. */
bool mp_login_username_valid(const char* username_utf8);

/* "Chosen" player name (2026-09-11 maintainer rule: every CaveWii player must
 * join under a deliberately chosen, distinguishable name -- a crowd of
 * identical default names reads as bots, and offline-mode servers key the
 * ACCOUNT by the name, so two "CaveWii"s collide). True when the name is
 * wire-valid (mp_login_username_valid), 1..16 bytes of the Beta account
 * alphabet [A-Za-z0-9_] (the minecraft.net rule every Beta-era player name
 * obeyed; the OSK can type more, servers may reject more), and is NOT the
 * project's own name "CaveWii" in any letter case (the old silent default). */
bool mp_username_chosen(const char* username_utf8);

#endif
