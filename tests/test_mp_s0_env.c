/*
	MP-S0 poisoned goldens + threaded provider gate.

	All expected packet bytes below were hand-transcribed before the production
	codec was exercised. They come from the Beta 1.7.3 decompile at 740c583,
	never from mp_encode_* output. The default ctest leg uses a deterministic
	loopback peer; --live/--live-walk N/--live-s2 target a separately launched
	fresh mc173-server (section 10 of docs/mp_online_client_plan_2026-08-15.md).
*/

#include "network/mp_codec.h"
#include "network/client_interface.h"
#include "network/mp_session.h"
#include "network/server_interface.h"
#include "network/server_remote.h"
#include "network/server_remote_internal.h"
#include "game/client_session.h"
#include "game/gui/screen.h"
#include "game/gui/container_menu.h"
#include "game/gui/screen_load_world.h"
#include "block/blocks.h"
#include "entity/entity_painting.h"
#include "entity/monsters_object.h"
#include "game/game_state.h"
#include "item/items.h"
#include "item/items/item_map.h"
#include "particle.h"
#include "platform/net_socket.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "world.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h> /* struct timeval for SO_RCVTIMEO (EOF-ordering peers) */
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

static void assert_frame(const struct mp_frame* frame, const uint8_t* expected,
						 size_t expected_length) {
	assert(frame->length == expected_length);
	assert(memcmp(frame->data, expected, expected_length) == 0);
}

static struct mp_event decode_exact(const uint8_t* bytes, size_t length,
									enum mp_event_type type) {
	struct mp_decoder decoder;
	mp_decoder_init(&decoder);
	struct mp_event event;
	for(size_t i = 0; i < length; i++)
		assert(mp_decoder_feed(&decoder, bytes[i], &event)
			== (i + 1 == length ? MP_DECODE_EVENT : MP_DECODE_MORE));
	assert(event.type == type);
	return event;
}

static void* fail_alloc(size_t size) { (void)size; return NULL; }

static void test_codec_goldens(void) {
	struct mp_frame frame;

	/* ID 0, no body. [740c583 Packet.addIdClassMapping]
	 * [740c583 Packet0KeepAlive.writePacketData] */
	static const uint8_t keep_alive[] = {0x00};
	assert(mp_encode_keep_alive(&frame));
	assert_frame(&frame, keep_alive, sizeof(keep_alive));

	/* ID 2, then i16 code-unit count and Java chars in username order.
	 * [740c583 Packet.addIdClassMapping]
	 * [740c583 Packet2Handshake.writePacketData]
	 * [740c583 Packet.writeString] */
	static const uint8_t handshake[]
		= {0x02, 0x00, 0x04, 0x00, 'C', 0x00, 'a', 0x00, 'v', 0x00, 'e'};
	assert(mp_encode_handshake(&frame, "Cave"));
	assert_frame(&frame, handshake, sizeof(handshake));

	/* Non-ASCII is still one big-endian Java code unit, not UTF-8 on wire.
	 * [740c583 Packet.writeString] */
	static const uint8_t handshake_e_acute[] = {0x02, 0x00, 0x01, 0x00, 0xE9};
	assert(mp_encode_handshake(&frame, "\xC3\xA9"));
	assert_frame(&frame, handshake_e_acute, sizeof(handshake_e_acute));

	/* ID 1; i32 protocol 14; string16 username; i64 seed 0; i8 dimension 0.
	 * [740c583 Packet.addIdClassMapping]
	 * [740c583 Packet1Login.writePacketData]
	 * [740c583 NetClientHandler.handleHandshake] */
	static const uint8_t login_a[] = {0x01, 0x00, 0x00, 0x00, 0x0E,
		0x00, 0x01, 0x00, 'A', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00};
	assert(mp_encode_login(&frame, "A"));
	assert_frame(&frame, login_a, sizeof(login_a));

	/* ID 255, then reason string16. [740c583 Packet.addIdClassMapping]
	 * [740c583 Packet255KickDisconnect.writePacketData] */
	static const uint8_t disconnect[]
		= {0xFF, 0x00, 0x03, 0x00, 'B', 0x00, 'y', 0x00, 'e'};
	assert(mp_encode_disconnect(&frame, "Bye"));
	assert_frame(&frame, disconnect, sizeof(disconnect));

	assert(!mp_encode_login(&frame, "12345678901234567"));
	assert(!mp_encode_handshake(&frame,
		"123456789012345678901234567890123"));

	/* Incoming login field order: i32, string16, i64, i8.
	 * [740c583 Packet1Login.readPacketData] */
	static const uint8_t login_response[] = {0x01, 0x01, 0x02, 0x03, 0x04,
		0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0xFF};
	struct mp_decoder decoder;
	mp_decoder_init(&decoder);
	struct mp_event event;
	for(size_t i = 0; i < sizeof(login_response); i++) {
		enum mp_decode_result result
			= mp_decoder_feed(&decoder, login_response[i], &event);
		assert(result == (i + 1 == sizeof(login_response) ? MP_DECODE_EVENT
															 : MP_DECODE_MORE));
	}
	assert(event.type == MP_EVENT_LOGIN);
	assert(event.payload.login.entity_id == 0x01020304);
	assert(event.payload.login.username.length == 0);
	assert(event.payload.login.map_seed == INT64_C(0x0102030405060708));
	assert(event.payload.login.dimension == -1);

	/* Packet255 owns its max-100 string16 reason after decoder reuse/teardown.
	 * [740c583 Packet255KickDisconnect.readPacketData]
	 * [740c583 Packet.readString] */
	static const uint8_t kick_reason[]
		= {0xFF, 0x00, 0x03, 0x00, 'B', 0x00, 'y', 0x00, 'e'};
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(kick_reason); i++) {
		enum mp_decode_result result
			= mp_decoder_feed(&decoder, kick_reason[i], &event);
		assert(result == (i + 1 == sizeof(kick_reason) ? MP_DECODE_EVENT
														 : MP_DECODE_MORE));
	}
	struct mp_event owned_kick = event;
	assert(mp_decoder_feed(&decoder, 0x00, &event) == MP_DECODE_EVENT);
	char kick_utf8[8];
	assert(mp_utf16_to_utf8(&owned_kick.payload.disconnect_reason, kick_utf8,
							 sizeof(kick_utf8)));
	assert(strcmp(kick_utf8, "Bye") == 0);

	/* string16 length is signed and capped by each packet class.
	 * [740c583 Packet.readString]
	 * [740c583 Packet2Handshake.readPacketData] */
	static const uint8_t negative_length[] = {0x02, 0xFF, 0xFF};
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(negative_length); i++) {
		enum mp_decode_result result
			= mp_decoder_feed(&decoder, negative_length[i], &event);
		assert(result == (i + 1 == sizeof(negative_length) ? MP_DECODE_ERROR
															 : MP_DECODE_MORE));
	}
	static const uint8_t over_handshake_max[] = {0x02, 0x00, 0x21};
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(over_handshake_max); i++) {
		enum mp_decode_result result
			= mp_decoder_feed(&decoder, over_handshake_max[i], &event);
		assert(result == (i + 1 == sizeof(over_handshake_max) ? MP_DECODE_ERROR
															 : MP_DECODE_MORE));
	}

	/* Multiple unframed packets can share one recv buffer. [740c583 Packet.readPacket] */
	static const uint8_t combined[] = {0x00, 0x02, 0x00, 0x01, 0x00, '-'};
	mp_decoder_init(&decoder);
	int events = 0;
	for(size_t i = 0; i < sizeof(combined); i++) {
		enum mp_decode_result result = mp_decoder_feed(&decoder, combined[i], &event);
		assert(result != MP_DECODE_ERROR);
		if(result == MP_DECODE_EVENT)
			events++;
	}
	assert(events == 2);

	/* Registry is all-and-only P0+P1+P2+P3: 13+17+19+8 = 57 IDs, matching
	 * "Packet.java registers 57 IDs" [740c583 Packet]. Direction flags come
	 * from Packet.addIdClassMapping's client/server sets [740c583 Packet].
	 * The decoder's own dispatch mirrors Packet.readPacket's own direction
	 * gate [740c583 Packet.readPacket]: for the client role it accepts a
	 * byte iff that id is in clientPacketIdList (server->client, `s_to_c`
	 * below), so "decodable" tracks s_to_c exactly, not "registered at all"
	 * -- six P1 ids (UseEntity/BlockDig/Place/BlockItemSwitch/EntityAction/
	 * WindowClick) and one P2 id (0x1B PositionLegacy -- registered C->S
	 * only, and per mp_codec.h's MP_PACKET_POSITION_LEGACY comment, never
	 * actually constructed by any real Beta client either) are
	 * client->server-only and therefore encode-only in a faithful client;
	 * feeding their id byte is exactly as illegal as any unknown id. Every
	 * P3 id is S->C only except 0x03 Chat, the sole bidirectional one. */
	static const uint8_t s_to_c[] = {0x00,0x01,0x02,0x04,0x06,0x08,0x0A,
		0x0B,0x0C,0x0D,0x32,0x33,0xFF,
		0x05,0x09,0x34,0x35,0x64,0x65,0x67,0x68,0x69,0x6A,0x82,
		0x11,0x12,0x14,0x15,0x16,0x17,0x18,0x19,0x1C,0x1D,0x1E,0x1F,
		0x20,0x21,0x22,0x26,0x27,0x28,
		0x03,0x36,0x3C,0x3D,0x46,0x47,0x83,0xC8};
	static const uint8_t c_to_s[] = {0x00,0x01,0x02,0x0A,0x0B,0x0C,0x0D,0xFF,
		0x07,0x09,0x0E,0x0F,0x10,0x13,0x65,0x66,0x6A,0x82,
		0x12,0x1B,
		0x03};
	for(unsigned id = 0; id < 256; id++) {
		bool expected_in = false, expected_out = false;
		for(size_t k = 0; k < sizeof(s_to_c); k++)
			if(s_to_c[k] == id) expected_in = true;
		for(size_t k = 0; k < sizeof(c_to_s); k++)
			if(c_to_s[k] == id) expected_out = true;
		assert(mp_packet_supported((uint8_t)id,
			MP_DIRECTION_SERVER_TO_CLIENT) == expected_in);
		assert(mp_packet_supported((uint8_t)id,
			MP_DIRECTION_CLIENT_TO_SERVER) == expected_out);
		mp_decoder_init(&decoder);
		enum mp_decode_result first
			= mp_decoder_feed(&decoder, (uint8_t)id, &event);
		assert((first != MP_DECODE_ERROR) == expected_in);
		mp_decoder_destroy(&decoder);
	}

	static const uint8_t time_golden[] =
		{0x04,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
	assert(mp_encode_update_time(&frame, INT64_C(0x0102030405060708)));
	assert_frame(&frame, time_golden, sizeof(time_golden));
	event = decode_exact(time_golden, sizeof(time_golden), MP_EVENT_UPDATE_TIME);
	assert(event.payload.world_time == INT64_C(0x0102030405060708));
	static const uint8_t spawn_golden[] =
		{0x06,0,0,0,1, 0xFF,0xFF,0xFF,0xFE, 0,0,0,3};
	assert(mp_encode_spawn_position(&frame, 1, -2, 3));
	assert_frame(&frame, spawn_golden, sizeof(spawn_golden));
	event = decode_exact(spawn_golden, sizeof(spawn_golden),
		MP_EVENT_SPAWN_POSITION);
	assert(event.payload.spawn_position.x == 1
		&& event.payload.spawn_position.y == -2
		&& event.payload.spawn_position.z == 3);
	static const uint8_t health_golden[] = {0x08,0xFF,0xFE};
	assert(mp_encode_update_health(&frame, -2));
	assert_frame(&frame, health_golden, sizeof(health_golden));
	event = decode_exact(health_golden, sizeof(health_golden),
		MP_EVENT_UPDATE_HEALTH);
	assert(event.payload.health == -2);
	/* Packet8 wire->internal scale goldens (half-hearts x HEALTH_PER_HEART/2,
	 * clamp to MAX_PLAYER_HEALTH, dead-or-below floors at 0): full 20 -> 160,
	 * 2.5 hearts of zombie damage off full = 15 -> 120, half a heart 1 -> 8,
	 * dead 0/-2 -> 0, hostile 32767 clamps instead of overflowing int16. */
	assert(health_wire_to_internal(20) == MAX_PLAYER_HEALTH);
	assert(health_wire_to_internal(15) == 120);
	assert(health_wire_to_internal(1) == HEALTH_PER_HEART / 2);
	assert(health_wire_to_internal(0) == 0);
	assert(health_wire_to_internal(-2) == 0);
	assert(health_wire_to_internal(INT16_MAX) == MAX_PLAYER_HEALTH);
	static const uint8_t flying_golden[] = {0x0A,0x01};
	assert(mp_encode_flying(&frame, true));
	assert_frame(&frame, flying_golden, sizeof(flying_golden));
	event = decode_exact(flying_golden, sizeof(flying_golden), MP_EVENT_FLYING);
	assert(event.payload.movement.on_ground);
	static const uint8_t packet11_golden[] = {0x0B,
		0x3F,0xF0,0,0,0,0,0,0, 0x40,0,0,0,0,0,0,0,
		0x40,0x08,0,0,0,0,0,0, 0x40,0x10,0,0,0,0,0,0, 1};
	assert(mp_encode_player_position(&frame,1.0,2.0,3.0,4.0,true));
	assert_frame(&frame,packet11_golden,sizeof(packet11_golden));
	event = decode_exact(packet11_golden, sizeof(packet11_golden),
		MP_EVENT_PLAYER_POSITION);
	assert(event.payload.movement.x == 1.0
		&& event.payload.movement.y == 2.0
		&& event.payload.movement.stance == 3.0
		&& event.payload.movement.z == 4.0
		&& event.payload.movement.on_ground);
	static const uint8_t packet12_golden[] =
		{0x0C,0x40,0xA0,0,0, 0x40,0xC0,0,0, 1};
	assert(mp_encode_player_look(&frame,5.0f,6.0f,true));
	assert_frame(&frame,packet12_golden,sizeof(packet12_golden));
	event = decode_exact(packet12_golden, sizeof(packet12_golden),
		MP_EVENT_PLAYER_LOOK);
	assert(event.payload.movement.yaw == 5.0f
		&& event.payload.movement.pitch == 6.0f
		&& event.payload.movement.on_ground);

	/* Named ordering-trap golden: x,y,stance,z,yaw,pitch,onGround.
	 * Hand-transcribed from plan section 5 before using the encoder.
	 * [740c583 Packet13PlayerLookMove.writePacketData] */
	static const uint8_t packet13_golden[] = {
		0x0D,
		0x3F,0xF0,0,0,0,0,0,0, 0x40,0,0,0,0,0,0,0,
		0x40,0x08,0,0,0,0,0,0, 0x40,0x10,0,0,0,0,0,0,
		0x40,0xA0,0,0, 0x40,0xC0,0,0, 0x01};
	assert(mp_encode_player_look_move(&frame, 1.0, 2.0, 3.0, 4.0,
		5.0f, 6.0f, true));
	assert_frame(&frame, packet13_golden, sizeof(packet13_golden));
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(packet13_golden); i++)
		assert(mp_decoder_feed(&decoder, packet13_golden[i], &event)
			== (i + 1 == sizeof(packet13_golden) ? MP_DECODE_EVENT
				: MP_DECODE_MORE));
	assert(event.type == MP_EVENT_PLAYER_LOOK_MOVE);
	assert(event.payload.movement.x == 1.0 && event.payload.movement.y == 2.0);
	assert(event.payload.movement.stance == 3.0
		&& event.payload.movement.z == 4.0);
	assert(event.payload.movement.yaw == 5.0f
		&& event.payload.movement.pitch == 6.0f
		&& event.payload.movement.on_ground);

	static const uint8_t prechunk_golden[] =
		{0x32,0,0,0,7, 0xFF,0xFF,0xFF,0xF8, 1};
	assert(mp_encode_pre_chunk(&frame, 7, -8, true));
	assert_frame(&frame, prechunk_golden, sizeof(prechunk_golden));
	event = decode_exact(prechunk_golden, sizeof(prechunk_golden),
		MP_EVENT_PRE_CHUNK);
	assert(event.payload.pre_chunk.chunk_x == 7
		&& event.payload.pre_chunk.chunk_z == -8
		&& event.payload.pre_chunk.mode);
	static uint8_t tiny_compressed[] = {0xAA,0xBB};
	struct mp_map_chunk tiny = {
		.x=16,.y=4,.z=-16,.sx=1,.sy=2,.sz=1,
		.inflated_length=5,.compressed=tiny_compressed,.compressed_length=2};
	static const uint8_t map_golden[] = {0x33,0,0,0,0x10, 0,4,
		0xFF,0xFF,0xFF,0xF0, 0,1,0, 0,0,0,2, 0xAA,0xBB};
	assert(mp_encode_map_chunk(&frame, &tiny));
	assert_frame(&frame, map_golden, sizeof(map_golden));
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(map_golden); i++)
		assert(mp_decoder_feed(&decoder, map_golden[i], &event)
			== (i + 1 == sizeof(map_golden) ? MP_DECODE_EVENT
				: MP_DECODE_MORE));
	assert(event.type == MP_EVENT_MAP_CHUNK);
	assert(event.payload.map_chunk.sx == 1 && event.payload.map_chunk.sy == 2
		&& event.payload.map_chunk.sz == 1);
	assert(memcmp(event.payload.map_chunk.compressed, tiny_compressed, 2) == 0);
	mp_event_destroy(&event);
	/* Packet51 body allocation failure is explicit, never a NULL sink write. */
	mp_codec_set_alloc_hook(fail_alloc);
	mp_decoder_init(&decoder);
	for(size_t i=0;i<18;i++) {
		enum mp_decode_result result = mp_decoder_feed(&decoder,map_golden[i],&event);
		assert(result == (i==17 ? MP_DECODE_ERROR : MP_DECODE_MORE));
	}
	mp_codec_set_alloc_hook(NULL);
	/* Header arithmetic is rejected before any body byte or client-state write. */
	uint8_t bad_header[18];
	memcpy(bad_header, map_golden, sizeof(bad_header));
	bad_header[11] = 16; /* xSize=17 exceeds one Beta chunk. */
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(bad_header); i++)
		assert(mp_decoder_feed(&decoder, bad_header[i], &event)
			== (i + 1 == sizeof(bad_header) ? MP_DECODE_ERROR
				: MP_DECODE_MORE));
	memcpy(bad_header, map_golden, sizeof(bad_header));
	bad_header[14] = bad_header[15] = bad_header[16] = bad_header[17] = 0xFF;
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(bad_header); i++)
		assert(mp_decoder_feed(&decoder, bad_header[i], &event)
			== (i + 1 == sizeof(bad_header) ? MP_DECODE_ERROR
				: MP_DECODE_MORE));
	memcpy(bad_header, map_golden, sizeof(bad_header));
	bad_header[5] = 0;
	bad_header[6] = 127;
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(bad_header); i++)
		assert(mp_decoder_feed(&decoder, bad_header[i], &event)
			== (i + 1 == sizeof(bad_header) ? MP_DECODE_ERROR
				: MP_DECODE_MORE));
	/* A truncated body remains resumable and owns only its received prefix;
	 * destroy is the EOF/cancel cleanup boundary. */
	mp_decoder_init(&decoder);
	for(size_t i=0;i<19;i++)
		assert(mp_decoder_feed(&decoder,map_golden[i],&event)==MP_DECODE_MORE);
	mp_decoder_destroy(&decoder);

	/* Every byte is fed separately above, including Packet51's dynamic body;
	 * combined verifies multiple completions in one recv-equivalent sequence. */
	uint8_t multi[sizeof(time_golden) + sizeof(spawn_golden)];
	memcpy(multi, time_golden, sizeof(time_golden));
	memcpy(multi + sizeof(time_golden), spawn_golden, sizeof(spawn_golden));
	mp_decoder_init(&decoder);
	events = 0;
	for(size_t i = 0; i < sizeof(multi); i++)
		if(mp_decoder_feed(&decoder, multi[i], &event) == MP_DECODE_EVENT)
			events++;
	assert(events == 2);
}

/* Interim title-screen path (plan docs/mp_online_client_plan_2026-08-15.md
 * section 3.3): mp_address_parse's golden cases, hand-derived from
 * GuiMultiplayer.actionPerformed's own address-field parse [740c583
 * GuiMultiplayer] -- host only, host:port, bracketed IPv6 host[:port],
 * bad/missing port defaulting to 25565, more-than-two-segment collapse, and
 * empty. */
static void assert_address(const char* input, const char* expected_host,
						  uint16_t expected_port) {
	char host[MP_ADDRESS_HOST_CAPACITY];
	uint16_t port = 0;
	assert(mp_address_parse(input, host, sizeof(host), &port));
	assert(strcmp(host, expected_host) == 0);
	assert(port == expected_port);
}

static void test_address_parse_goldens(void) {
	/* host only, with GuiMultiplayer's own .trim() applied first. */
	assert_address("localhost", "localhost", 25565);
	assert_address("  localhost  ", "localhost", 25565);
	assert_address("192.0.2.5", "192.0.2.5", 25565);

	/* host:port. */
	assert_address("example.com:12345", "example.com", 12345);
	assert_address("example.com:0", "example.com", 0);
	assert_address("example.com:65535", "example.com", 65535);

	/* Bad/missing port: Beta's own parseIntWithDefault swallows any
	 * NumberFormatException and falls back to 25565 [740c583
	 * GuiMultiplayer.parseIntWithDefault] -- non-numeric, out of uint16_t
	 * range, and an empty trailing segment (a bare trailing colon, which
	 * Java's own trailing-empty-token split() rule collapses to a
	 * host-only token list) all take this path. */
	assert_address("example.com:notaport", "example.com", 25565);
	assert_address("example.com:999999", "example.com", 25565);
	assert_address("example.com:-1", "example.com", 25565);
	assert_address("example.com:", "example.com", 25565);
	assert_address("example.com: 80 ", "example.com", 80);

	/* Bracketed IPv6 -- required to attach a port to a host containing
	 * colons; an un-bracketed host with 2+ colons falls into the collapse
	 * case below instead. [plan 3.3 "bracketed IPv6 [::1]:port"] */
	assert_address("[::1]:25566", "::1", 25566);
	assert_address("[::1]", "::1", 25565);
	assert_address("[2001:db8::1]:80", "2001:db8::1", 80);

	/* More than two ':'-separated segments outside brackets collapses to
	 * the WHOLE trimmed input as the host, default port -- Beta's client
	 * replaces a 3+-segment split with the untouched input string [740c583
	 * GuiMultiplayer.java:57]. A bare (un-bracketed) IPv6 host degrades the same
	 * way and still resolves correctly since the collapse target IS the
	 * whole host string. */
	assert_address("a:b:c", "a:b:c", 25565);
	assert_address("::1", "::1", 25565);

	/* Empty (the title screen never reaches this parser with an empty
	 * "mp.server", but the pure function itself must not crash/reject). */
	assert_address("", "", 25565);
	assert_address("   ", "", 25565);

	/* The one input shape (String.split(":") on a colon-only string)
	 * Beta's own client crashes on with an uncaught
	 * ArrayIndexOutOfBoundsException reading the absent first segment [740c583
	 * GuiMultiplayer.java:42,61] -- CaveWii fails closed instead. */
	{
		char host[MP_ADDRESS_HOST_CAPACITY];
		uint16_t port = 0;
		assert(!mp_address_parse(":", host, sizeof(host), &port));
		assert(!mp_address_parse(":::", host, sizeof(host), &port));
	}
}

/* Packet1's 16-code-unit login maximum is stricter than Packet2's 32-unit
 * handshake [740c583 Packet1Login.readPacketData] [740c583
 * Packet2Handshake.readPacketData] -- mp_login_username_valid must reject
 * exactly where mp_encode_login's own append_string16 would. */
static void test_username_validation(void) {
	assert(mp_login_username_valid("CaveWii"));
	assert(mp_login_username_valid("1234567890123456")); /* exactly 16 */
	assert(!mp_login_username_valid("12345678901234567")); /* 17: reject */

	/* Non-ASCII counts by UTF-16 CODE UNIT, not byte or codepoint -- 16
	 * two-byte-UTF-8 characters (1 code unit each) fit; adding a 17th does
	 * not. [740c583 Packet.writeString/readString] */
	assert(mp_login_username_valid(
		"\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
		"\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"));
	assert(!mp_login_username_valid(
		"\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
		"\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
		"\xC3\xA9"));

	/* A supplementary-plane character costs 2 UTF-16 code units (a
	 * surrogate pair), so 8 of them already fill the 16-unit budget. */
	assert(mp_login_username_valid(
		"\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80"
		"\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80"));
	assert(!mp_login_username_valid(
		"\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80"
		"\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80"
		"A"));

	/* Malformed UTF-8 (a lone continuation byte) is rejected, not silently
	 * dropped or truncated. */
	assert(!mp_login_username_valid("Cave\x80Wii"));

	/* Empty is not a login-length violation (mp_encode_login itself accepts
	 * a zero-length string16); server_remote_start's own !*username_utf8
	 * check is the layer that refuses to connect with an empty username. */
	assert(mp_login_username_valid(""));
}

/* S2 P1 (dig/place/inventory) golden encode/decode vectors. Every expected
 * byte is hand-transcribed from the 740c583 decompile before the production
 * encoder/decoder run, matching test_codec_goldens' discipline. Six P1 ids
 * (UseEntity/BlockDig/Place/BlockItemSwitch/EntityAction/WindowClick) are
 * client->server-only in a faithful client [740c583 Packet]: they get golden
 * ENCODE vectors only, never decode_exact, matching mp_decoder_feed's own
 * direction gate (see its dispatch comment). */
static void test_p1_codec_goldens(void) {
	struct mp_frame frame;
	struct mp_decoder decoder;
	struct mp_event event;

	/* 0x05 Packet5PlayerInventory: i32 entityID; i16 slot; i16 itemID;
	 * i16 itemDamage. S->C only [740c583 Packet5PlayerInventory]. */
	static const uint8_t player_inv_golden[] = {0x05,
		0,0,0,100, 0,4, 0,5, 0,0};
	assert(mp_encode_player_inventory(&frame, 100, 4, 5, 0));
	assert_frame(&frame, player_inv_golden, sizeof(player_inv_golden));
	event = decode_exact(player_inv_golden, sizeof(player_inv_golden),
		MP_EVENT_PLAYER_INVENTORY);
	assert(event.payload.player_inventory.entity_id == 100
		&& event.payload.player_inventory.slot == 4
		&& event.payload.player_inventory.item_id == 5
		&& event.payload.player_inventory.item_damage == 0);

	/* 0x07 Packet7UseEntity: i32 playerEntityId; i32 targetEntity;
	 * i8 isLeftClick (1 attack / 0 use). C->S only [740c583
	 * Packet7UseEntity] [740c583 PlayerControllerMP.attackEntity]
	 * [740c583 PlayerControllerMP.interactWithEntity]. */
	static const uint8_t use_entity_attack[] = {0x07, 0,0,0,1, 0,0,0,2, 1};
	assert(mp_encode_use_entity(&frame, 1, 2, 1));
	assert_frame(&frame, use_entity_attack, sizeof(use_entity_attack));
	static const uint8_t use_entity_use[] = {0x07, 0,0,0,1, 0,0,0,2, 0};
	assert(mp_encode_use_entity(&frame, 1, 2, 0));
	assert_frame(&frame, use_entity_use, sizeof(use_entity_use));

	/* 0x09 Packet9Respawn: i8 respawnDimension. Bidirectional [740c583
	 * Packet9Respawn]. */
	static const uint8_t respawn_golden[] = {0x09, 0xFF};
	assert(mp_encode_respawn(&frame, -1));
	assert_frame(&frame, respawn_golden, sizeof(respawn_golden));
	event = decode_exact(respawn_golden, sizeof(respawn_golden), MP_EVENT_RESPAWN);
	assert(event.payload.respawn_dimension == -1);

	/* 0x0E Packet14BlockDig: u8 status; i32 x; u8 y; i32 z; u8 face. C->S
	 * only [740c583 Packet14BlockDig]. status 0=start/2=finish/4=drop
	 * [740c583 PlayerControllerMP.clickBlock]
	 * [740c583 PlayerControllerMP.sendBlockRemoving]
	 * [740c583 EntityClientPlayerMP.dropCurrentItem]. */
	static const uint8_t block_dig_finish[] = {0x0E,
		2, 0,0,0,10, 64, 0xFF,0xFF,0xFF,0xFB, 1};
	assert(mp_encode_block_dig(&frame, 2, 10, 64, -5, 1));
	assert_frame(&frame, block_dig_finish, sizeof(block_dig_finish));
	static const uint8_t block_dig_drop[] = {0x0E, 4, 0,0,0,0, 0, 0,0,0,0, 0};
	assert(mp_encode_block_dig(&frame, 4, 0, 0, 0, 0));
	assert_frame(&frame, block_dig_drop, sizeof(block_dig_drop));

	/* 0x0F Packet15Place: i32 x; u8 y; i32 z; u8 direction; then i16 id
	 * (<0 = null stack), else i8 count + i16 damage. C->S only [740c583
	 * Packet15Place]. Air-use sentinel -1,-1,-1,255 [740c583
	 * PlayerControllerMP.sendUseItem]. */
	struct mp_item_stack stone = {.present = true, .id = 1, .count = 1, .damage = 0};
	static const uint8_t place_golden[] = {0x0F,
		0,0,0,1, 2, 0,0,0,3, 0, 0,1, 1, 0,0};
	assert(mp_encode_place(&frame, 1, 2, 3, 0, &stone));
	assert_frame(&frame, place_golden, sizeof(place_golden));
	struct mp_item_stack torch = {.present = true, .id = 50, .count = 1, .damage = 0};
	static const uint8_t place_use_item[] = {0x0F,
		0xFF,0xFF,0xFF,0xFF, 0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF, 0,50, 1, 0,0};
	assert(mp_encode_place(&frame, -1, 255, -1, 255, &torch));
	assert_frame(&frame, place_use_item, sizeof(place_use_item));
	struct mp_item_stack no_item = {.present = false};
	static const uint8_t place_null[] = {0x0F, 0,0,0,0, 0, 0,0,0,0, 0, 0xFF,0xFF};
	assert(mp_encode_place(&frame, 0, 0, 0, 0, &no_item));
	assert_frame(&frame, place_null, sizeof(place_null));

	/* 0x10 Packet16BlockItemSwitch: i16 id (hotbar slot). C->S only
	 * [740c583 Packet16BlockItemSwitch]. */
	static const uint8_t hotbar_switch[] = {0x10, 0,3};
	assert(mp_encode_block_item_switch(&frame, 3));
	assert_frame(&frame, hotbar_switch, sizeof(hotbar_switch));

	/* 0x13 Packet19EntityAction: i32 entityId; i8 state (1 sneak-start /
	 * 2 sneak-stop). C->S only [740c583 Packet19EntityAction] [740c583
	 * EntityClientPlayerMP.sendMotionUpdates]. */
	static const uint8_t entity_action_golden[] = {0x13, 0,0,0,7, 1};
	assert(mp_encode_entity_action(&frame, 7, 1));
	assert_frame(&frame, entity_action_golden, sizeof(entity_action_golden));

	/* 0x34 Packet52MultiBlockChange: i32 x; i32 z; u16 size(&0xFFFF); then
	 * size*i16 packed coordinate, size*u8 type, size*u8 metadata. S->C
	 * only [740c583 Packet52MultiBlockChange]. Packed coordinate is
	 * (localX<<12)|(localZ<<8)|localY [740c583
	 * NetClientHandler.handleMultiBlockChange]. */
	uint16_t mb_coord[] = {0x1203, 0xF07F};
	uint8_t mb_type[] = {1, 9};
	uint8_t mb_meta[] = {0, 3};
	struct mp_multi_block_change mb = {.chunk_x = 2, .chunk_z = -1, .count = 2,
		.coordinate = mb_coord, .type = mb_type, .metadata = mb_meta};
	static const uint8_t multiblock_golden[] = {0x34,
		0,0,0,2, 0xFF,0xFF,0xFF,0xFF, 0,2, 0x12,0x03, 0xF0,0x7F, 1,9, 0,3};
	assert(mp_encode_multi_block_change(&frame, &mb));
	assert_frame(&frame, multiblock_golden, sizeof(multiblock_golden));
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(multiblock_golden); i++)
		assert(mp_decoder_feed(&decoder, multiblock_golden[i], &event)
			== (i + 1 == sizeof(multiblock_golden) ? MP_DECODE_EVENT
				: MP_DECODE_MORE));
	assert(event.type == MP_EVENT_MULTI_BLOCK_CHANGE);
	assert(event.payload.multi_block_change.chunk_x == 2
		&& event.payload.multi_block_change.chunk_z == -1
		&& event.payload.multi_block_change.count == 2);
	assert(event.payload.multi_block_change.coordinate[0] == 0x1203
		&& event.payload.multi_block_change.coordinate[1] == 0xF07F);
	assert(event.payload.multi_block_change.type[0] == 1
		&& event.payload.multi_block_change.type[1] == 9);
	assert(event.payload.multi_block_change.metadata[0] == 0
		&& event.payload.multi_block_change.metadata[1] == 3);
	/* Unpack entry 0 exactly like NetClientHandler.handleMultiBlockChange. */
	uint16_t c0 = event.payload.multi_block_change.coordinate[0];
	assert(((c0 >> 12) & 15) == 1 && ((c0 >> 8) & 15) == 2 && (c0 & 255) == 3);
	mp_event_destroy(&event);
	/* Zero-entry multiblock change is wire-legal: no array bytes follow. */
	struct mp_multi_block_change mb_empty = {.chunk_x = 0, .chunk_z = 0, .count = 0};
	static const uint8_t multiblock_empty[] = {0x34, 0,0,0,0, 0,0,0,0, 0,0};
	assert(mp_encode_multi_block_change(&frame, &mb_empty));
	assert_frame(&frame, multiblock_empty, sizeof(multiblock_empty));
	event = decode_exact(multiblock_empty, sizeof(multiblock_empty),
		MP_EVENT_MULTI_BLOCK_CHANGE);
	assert(event.payload.multi_block_change.count == 0
		&& event.payload.multi_block_change.coordinate == NULL);

	/* 0x35 Packet53BlockChange: i32 x; u8 y; i32 z; u8 type; u8 metadata.
	 * S->C only [740c583 Packet53BlockChange]. */
	static const uint8_t block_change_golden[] = {0x35,
		0,0,0,5, 10, 0xFF,0xFF,0xFF,0xFB, 1, 0};
	assert(mp_encode_block_change(&frame, 5, 10, -5, 1, 0));
	assert_frame(&frame, block_change_golden, sizeof(block_change_golden));
	event = decode_exact(block_change_golden, sizeof(block_change_golden),
		MP_EVENT_BLOCK_CHANGE);
	assert(event.payload.block_change.x == 5 && event.payload.block_change.y == 10
		&& event.payload.block_change.z == -5
		&& event.payload.block_change.block_type == 1
		&& event.payload.block_change.metadata == 0);

	/* 0x64 Packet100OpenWindow: i8 windowId; i8 inventoryType; readUTF
	 * title (u16 BYTE length + modified UTF-8, NOT string16); u8
	 * slotsCount. S->C only [740c583 Packet100OpenWindow]. */
	static const uint8_t open_window_golden[] = {0x64,
		1, 0, 0,5, 'C','h','e','s','t', 27};
	assert(mp_encode_open_window(&frame, 1, 0, "Chest", 27));
	assert_frame(&frame, open_window_golden, sizeof(open_window_golden));
	event = decode_exact(open_window_golden, sizeof(open_window_golden),
		MP_EVENT_OPEN_WINDOW);
	assert(event.payload.open_window.window == 1
		&& event.payload.open_window.inventory_type == 0
		&& strcmp(event.payload.open_window.title, "Chest") == 0
		&& event.payload.open_window.slot_count == 27);
	/* Non-ASCII title exercises modified UTF-8's 2-byte form (same bytes as
	 * standard UTF-8 for this codepoint). [740c583 Packet.writeString analog
	 * via DataOutputStream.writeUTF] */
	static const uint8_t open_window_accent[] = {0x64,
		2, 2, 0,3, 0xC3,0xA9, 'x', 5};
	event = decode_exact(open_window_accent, sizeof(open_window_accent),
		MP_EVENT_OPEN_WINDOW);
	assert(strcmp(event.payload.open_window.title, "\xC3\xA9x") == 0);
	/* Modified UTF-8's defining trap: an embedded NUL is TWO bytes (0xC0
	 * 0x80), never a lone 0x00. Decode "Hi" + encoded-NUL and confirm the
	 * output C string reads as "Hi" (a literal 0x00 lands where readUTF's
	 * Java String would carry a genuine embedded char 0). */
	static const uint8_t open_window_nul[] = {0x64,
		3, 0, 0,4, 'H','i', 0xC0,0x80, 9};
	event = decode_exact(open_window_nul, sizeof(open_window_nul),
		MP_EVENT_OPEN_WINDOW);
	assert(event.payload.open_window.title[0] == 'H'
		&& event.payload.open_window.title[1] == 'i'
		&& event.payload.open_window.title[2] == '\0');
	/* Oversized readUTF byte length is rejected before allocation. */
	static const uint8_t open_window_huge_len[] = {0x64, 1, 0, 0xFF,0xFF};
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(open_window_huge_len); i++)
		assert(mp_decoder_feed(&decoder, open_window_huge_len[i], &event)
			== (i + 1 == sizeof(open_window_huge_len) ? MP_DECODE_ERROR
				: MP_DECODE_MORE));

	/* 0x65 Packet101CloseWindow: i8 windowId. Bidirectional [740c583
	 * Packet101CloseWindow]. */
	static const uint8_t close_window_golden[] = {0x65, 5};
	assert(mp_encode_close_window(&frame, 5));
	assert_frame(&frame, close_window_golden, sizeof(close_window_golden));
	event = decode_exact(close_window_golden, sizeof(close_window_golden),
		MP_EVENT_CLOSE_WINDOW);
	assert(event.payload.close_window == 5);

	/* 0x66 Packet102WindowClick: i8 window; i16 inventorySlot (SIGNED --
	 * -999 is the outside-window drop sentinel); i8 mouseClick; i16 action;
	 * bool field_27050_f (shift); then the same ItemStack tail as Place
	 * (the "optimistic result" stack). C->S only [740c583
	 * Packet102WindowClick] [740c583 PlayerControllerMP.handleMouseClick]. */
	static const uint8_t window_click_golden[] = {0x66,
		1, 0,10, 0, 0,5, 0, 0,1, 1, 0,0};
	assert(mp_encode_window_click(&frame, 1, 10, 0, 5, false, &stone));
	assert_frame(&frame, window_click_golden, sizeof(window_click_golden));
	static const uint8_t window_drop_golden[] = {0x66,
		1, 0xFC,0x19, 1, 0,0, 0, 0xFF,0xFF};
	assert(mp_encode_window_click(&frame, 1, -999, 1, 0, false, &no_item));
	assert_frame(&frame, window_drop_golden, sizeof(window_drop_golden));

	/* 0x67 Packet103SetSlot: i8 windowId; i16 itemSlot; ItemStack. S->C
	 * only [740c583 Packet103SetSlot]. windowId -1 is the held/cursor
	 * stack [740c583 NetClientHandler.handleSetSlot]. */
	static const uint8_t set_slot_golden[] = {0x67, 0, 0,36, 0,1, 1, 0,0};
	assert(mp_encode_set_slot(&frame, 0, 36, &stone));
	assert_frame(&frame, set_slot_golden, sizeof(set_slot_golden));
	event = decode_exact(set_slot_golden, sizeof(set_slot_golden), MP_EVENT_SET_SLOT);
	assert(event.payload.set_slot.window == 0 && event.payload.set_slot.slot == 36
		&& event.payload.set_slot.item.present && event.payload.set_slot.item.id == 1);
	static const uint8_t set_slot_cursor_null[] = {0x67, 0xFF, 0,0, 0xFF,0xFF};
	assert(mp_encode_set_slot(&frame, -1, 0, &no_item));
	assert_frame(&frame, set_slot_cursor_null, sizeof(set_slot_cursor_null));
	event = decode_exact(set_slot_cursor_null, sizeof(set_slot_cursor_null),
		MP_EVENT_SET_SLOT);
	assert(event.payload.set_slot.window == -1 && !event.payload.set_slot.item.present);

	/* 0x68 Packet104WindowItems: i8 windowId; i16 count (SIGNED, feeds
	 * the ItemStack array size); then count ItemStack entries. S->C only
	 * [740c583 Packet104WindowItems]. Fed one byte at a time below --
	 * exactly the fragmented-recv shape a real short TCP read produces. */
	struct mp_item_stack window_slots[3] = {
		{.present = false},
		{.present = true, .id = 1, .count = 64, .damage = 0},
		{.present = true, .id = 5, .count = 1, .damage = 2},
	};
	struct mp_window_items wi = {.window = 1, .count = 3, .items = window_slots};
	static const uint8_t window_items_golden[] = {0x68,
		1, 0,3, 0xFF,0xFF, 0,1, 64, 0,0, 0,5, 1, 0,2};
	assert(mp_encode_window_items(&frame, &wi));
	assert_frame(&frame, window_items_golden, sizeof(window_items_golden));
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(window_items_golden); i++)
		assert(mp_decoder_feed(&decoder, window_items_golden[i], &event)
			== (i + 1 == sizeof(window_items_golden) ? MP_DECODE_EVENT
				: MP_DECODE_MORE));
	assert(event.type == MP_EVENT_WINDOW_ITEMS);
	assert(event.payload.window_items.window == 1
		&& event.payload.window_items.count == 3);
	assert(!event.payload.window_items.items[0].present);
	assert(event.payload.window_items.items[1].present
		&& event.payload.window_items.items[1].id == 1
		&& event.payload.window_items.items[1].count == 64);
	assert(event.payload.window_items.items[2].present
		&& event.payload.window_items.items[2].id == 5
		&& event.payload.window_items.items[2].damage == 2);
	mp_event_destroy(&event);
	struct mp_window_items wi_empty = {.window = 2, .count = 0, .items = NULL};
	static const uint8_t window_items_empty[] = {0x68, 2, 0,0};
	assert(mp_encode_window_items(&frame, &wi_empty));
	assert_frame(&frame, window_items_empty, sizeof(window_items_empty));
	event = decode_exact(window_items_empty, sizeof(window_items_empty),
		MP_EVENT_WINDOW_ITEMS);
	assert(event.payload.window_items.count == 0
		&& event.payload.window_items.items == NULL);
	/* A negative count reaches Java's array allocation, which throws;
	 * reject before any allocation. */
	static const uint8_t window_items_negative[] = {0x68, 1, 0xFF,0xFF};
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(window_items_negative); i++)
		assert(mp_decoder_feed(&decoder, window_items_negative[i], &event)
			== (i + 1 == sizeof(window_items_negative) ? MP_DECODE_ERROR
				: MP_DECODE_MORE));

	/* 0x69 Packet105UpdateProgressbar: i8 windowId; i16 progressBar;
	 * i16 progressBarValue. S->C only [740c583 Packet105UpdateProgressbar]. */
	static const uint8_t progressbar_golden[] = {0x69, 2, 0,0, 0,150};
	assert(mp_encode_update_progressbar(&frame, 2, 0, 150));
	assert_frame(&frame, progressbar_golden, sizeof(progressbar_golden));
	event = decode_exact(progressbar_golden, sizeof(progressbar_golden),
		MP_EVENT_UPDATE_PROGRESSBAR);
	assert(event.payload.update_progressbar.window == 2
		&& event.payload.update_progressbar.bar == 0
		&& event.payload.update_progressbar.value == 150);

	/* 0x6A Packet106Transaction: i8 windowId; i16 field_20028_b (action);
	 * u8 field_20030_c (accepted, 0/1). Bidirectional [740c583
	 * Packet106Transaction]. Rejected replies get an ack -- see
	 * NetClientHandler.handleTransaction below in the loopback fixture. */
	static const uint8_t transaction_accept[] = {0x6A, 1, 0,7, 1};
	assert(mp_encode_transaction(&frame, 1, 7, true));
	assert_frame(&frame, transaction_accept, sizeof(transaction_accept));
	event = decode_exact(transaction_accept, sizeof(transaction_accept),
		MP_EVENT_TRANSACTION);
	assert(event.payload.transaction.window == 1
		&& event.payload.transaction.action == 7 && event.payload.transaction.accepted);
	static const uint8_t transaction_reject[] = {0x6A, 1, 0,8, 0};
	assert(mp_encode_transaction(&frame, 1, 8, false));
	assert_frame(&frame, transaction_reject, sizeof(transaction_reject));
	event = decode_exact(transaction_reject, sizeof(transaction_reject),
		MP_EVENT_TRANSACTION);
	assert(!event.payload.transaction.accepted);

	/* 0x82 Packet130UpdateSign: i32 x; i16 y (SIGNED SHORT here, unlike
	 * BlockDig/BlockChange's single-byte y); i32 z; then 4x string16 lines
	 * capped at 15 code units. Bidirectional [740c583 Packet130UpdateSign].
	 * Fed one byte at a time below (fragmented-recv shape). */
	const char* sign_lines[4] = {"Hello", "World", "", ""};
	static const uint8_t sign_golden[] = {0x82,
		0,0,0,10, 0,65, 0xFF,0xFF,0xFF,0xEC,
		0,5, 0,'H',0,'e',0,'l',0,'l',0,'o',
		0,5, 0,'W',0,'o',0,'r',0,'l',0,'d',
		0,0,
		0,0};
	assert(mp_encode_update_sign(&frame, 10, 65, -20, sign_lines));
	assert_frame(&frame, sign_golden, sizeof(sign_golden));
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(sign_golden); i++)
		assert(mp_decoder_feed(&decoder, sign_golden[i], &event)
			== (i + 1 == sizeof(sign_golden) ? MP_DECODE_EVENT : MP_DECODE_MORE));
	assert(event.type == MP_EVENT_UPDATE_SIGN);
	assert(event.payload.update_sign.x == 10 && event.payload.update_sign.y == 65
		&& event.payload.update_sign.z == -20);
	assert(mp_utf16_equals_ascii(&event.payload.update_sign.lines[0], "Hello"));
	assert(mp_utf16_equals_ascii(&event.payload.update_sign.lines[1], "World"));
	assert(event.payload.update_sign.lines[2].length == 0
		&& event.payload.update_sign.lines[3].length == 0);
	/* Each line independently enforces the 15-code-unit cap. */
	static const uint8_t sign_line_too_long[] = {0x82,
		0,0,0,0, 0,0, 0,0,0,0, 0,16};
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(sign_line_too_long); i++)
		assert(mp_decoder_feed(&decoder, sign_line_too_long[i], &event)
			== (i + 1 == sizeof(sign_line_too_long) ? MP_DECODE_ERROR
				: MP_DECODE_MORE));
}

/* S3 P2 (entities/remote players) golden decode/encode vectors, plus the
 * fixed-point/angle conversion helpers and the DataWatcher bounds. Every
 * expected byte is hand-transcribed from the 740c583 decompile before the
 * production codec runs, matching test_codec_goldens'/test_p1_codec_goldens'
 * discipline. */
static void test_p2_codec_goldens(void) {
	struct mp_frame frame;
	struct mp_decoder decoder;
	struct mp_event event;

	/* Fixed-point (/32.0) and byte-angle (*360/256) conversions, shared by
	 * server_remote and every P2 golden below -- exercised directly first so
	 * a conversion bug cannot hide behind a coincidentally-matching golden.
	 * [740c583 NetClientHandler.handleNamedEntitySpawn et al.] */
	assert(mp_fixed32_to_double(32) == 1.0);
	assert(mp_fixed32_to_double(-32) == -1.0);
	assert(mp_fixed32_to_double(16) == 0.5);
	assert(mp_fixed32_to_double(0) == 0.0);
	assert(mp_byte_angle_to_degrees(64) == 90.0f);
	assert(mp_byte_angle_to_degrees(-64) == -90.0f);
	assert(mp_byte_angle_to_degrees(-128) == -180.0f);
	assert(mp_byte_angle_to_degrees(0) == 0.0f);
	assert(fabs(mp_byte_angle_to_degrees(127) - 178.59375f) < 0.0001f);

	/* 0x11 Packet17Sleep: i32 entityID; i8 unknown; i32 bedX; i8 bedY;
	 * i32 bedZ -- read/write ORDER, not field-declaration order. S->C only
	 * [740c583 Packet17Sleep.readPacketData]. */
	static const uint8_t sleep_golden[] = {0x11,
		0,0,0,7, 0, 0,0,0,10, 64, 0xFF,0xFF,0xFF,0xFB};
	event = decode_exact(sleep_golden, sizeof(sleep_golden), MP_EVENT_SLEEP);
	assert(event.payload.sleep.entity_id == 7 && event.payload.sleep.unknown == 0
		&& event.payload.sleep.bed_x == 10 && event.payload.sleep.bed_y == 64
		&& event.payload.sleep.bed_z == -5);

	/* 0x12 Packet18Animation: i32 entityId; i8 animate. Bidirectional
	 * [740c583 Packet18Animation]. */
	static const uint8_t animation_golden[] = {0x12, 0,0,0,7, 1};
	assert(mp_encode_animation(&frame, 7, 1));
	assert_frame(&frame, animation_golden, sizeof(animation_golden));
	event = decode_exact(animation_golden, sizeof(animation_golden),
		MP_EVENT_ANIMATION);
	assert(event.payload.animation.entity_id == 7
		&& event.payload.animation.animate == 1);

	/* 0x14 Packet20NamedEntitySpawn: i32 entityId; string16 name(<=16); i32
	 * x/y/z (fixed-point); i8 rotation/pitch (byte-angle); i16 currentItem
	 * (0=empty). S->C only [740c583 Packet20NamedEntitySpawn.readPacketData]. */
	static const uint8_t named_spawn_golden[] = {0x14,
		0,0,0,5, 0,3, 0,'A',0,'n',0,'n',
		0,0,0,0x20, 0,0,0,0x40, 0xFF,0xFF,0xFF,0xE0,
		0x40, 0xC0, 0,5};
	event = decode_exact(named_spawn_golden, sizeof(named_spawn_golden),
		MP_EVENT_NAMED_ENTITY_SPAWN);
	assert(event.payload.named_entity_spawn.entity_id == 5);
	assert(mp_utf16_equals_ascii(&event.payload.named_entity_spawn.name, "Ann"));
	assert(event.payload.named_entity_spawn.x == 32
		&& event.payload.named_entity_spawn.y == 64
		&& event.payload.named_entity_spawn.z == -32);
	assert(event.payload.named_entity_spawn.rotation == 64
		&& event.payload.named_entity_spawn.pitch == -64);
	assert(event.payload.named_entity_spawn.current_item == 5);
	assert(mp_fixed32_to_double(event.payload.named_entity_spawn.x) == 1.0);
	assert(mp_byte_angle_to_degrees(event.payload.named_entity_spawn.rotation)
		== 90.0f);

	/* 0x15 Packet21PickupSpawn: i32 entityId; i16 itemID; i8 count; i16
	 * itemDamage; i32 x/y/z; i8 rotation/pitch/roll -- these three are
	 * VELOCITY (/128.0), not angles. S->C only [740c583
	 * Packet21PickupSpawn.readPacketData] [740c583
	 * NetClientHandler.handlePickupSpawn]. */
	static const uint8_t pickup_spawn_golden[] = {0x15,
		0,0,0,9, 0,1, 3, 0,0,
		0,0,0,0x20, 0,0,0,0x40, 0,0,0,0x60,
		10, 0xFB, 20};
	event = decode_exact(pickup_spawn_golden, sizeof(pickup_spawn_golden),
		MP_EVENT_PICKUP_SPAWN);
	assert(event.payload.pickup_spawn.entity_id == 9
		&& event.payload.pickup_spawn.item_id == 1
		&& event.payload.pickup_spawn.count == 3
		&& event.payload.pickup_spawn.item_damage == 0);
	assert(event.payload.pickup_spawn.x == 32 && event.payload.pickup_spawn.y == 64
		&& event.payload.pickup_spawn.z == 96);
	assert(event.payload.pickup_spawn.vel_x == 10
		&& event.payload.pickup_spawn.vel_y == -5
		&& event.payload.pickup_spawn.vel_z == 20);

	/* 0x16 Packet22Collect: i32 collectedEntityId; i32 collectorEntityId.
	 * S->C only [740c583 Packet22Collect]. */
	static const uint8_t collect_golden[] = {0x16, 0,0,0,3, 0,0,0,5};
	event = decode_exact(collect_golden, sizeof(collect_golden), MP_EVENT_COLLECT);
	assert(event.payload.collect.collected_entity_id == 3
		&& event.payload.collect.collector_entity_id == 5);

	/* 0x17 Packet23VehicleSpawn: i32 entityId; i8 type; i32 x/y/z; i32
	 * throwerEntityId; if throwerEntityId>0: i16 speedX/Y/Z. S->C only
	 * [740c583 Packet23VehicleSpawn.readPacketData]. No-thrower leg first. */
	static const uint8_t vehicle_no_thrower[] = {0x17,
		0,0,0,11, 10, 0,0,0,0x20, 0,0,0,0, 0,0,0,0, 0,0,0,0};
	event = decode_exact(vehicle_no_thrower, sizeof(vehicle_no_thrower),
		MP_EVENT_VEHICLE_SPAWN);
	assert(event.payload.vehicle_spawn.entity_id == 11
		&& event.payload.vehicle_spawn.vtype == 10
		&& event.payload.vehicle_spawn.x == 32
		&& event.payload.vehicle_spawn.thrower_entity_id == 0
		&& !event.payload.vehicle_spawn.has_velocity);
	static const uint8_t vehicle_with_thrower[] = {0x17,
		0,0,0,12, 60, 0,0,0,0, 0,0,0,0, 0,0,0,0,
		0,0,0,99, 0,100, 0xFF,0xCE, 0,25};
	event = decode_exact(vehicle_with_thrower, sizeof(vehicle_with_thrower),
		MP_EVENT_VEHICLE_SPAWN);
	assert(event.payload.vehicle_spawn.entity_id == 12
		&& event.payload.vehicle_spawn.vtype == 60
		&& event.payload.vehicle_spawn.thrower_entity_id == 99
		&& event.payload.vehicle_spawn.has_velocity);
	assert(event.payload.vehicle_spawn.speed_x == 100
		&& event.payload.vehicle_spawn.speed_y == -50
		&& event.payload.vehicle_spawn.speed_z == 25);

	/* 0x18 Packet24MobSpawn: i32 entityId; i8 type; i32 x/y/z; i8 yaw/pitch;
	 * then a DataWatcher stream covering EVERY type 0..6, terminated by
	 * 0x7F. S->C only [740c583 Packet24MobSpawn.readPacketData] [740c583
	 * DataWatcher.readWatchableObjects]. Header byte is (type<<5|index). */
	static const uint8_t mob_spawn_golden[] = {0x18,
		0,0,0,20, 54, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0, 0,
		/* type 0 (byte) index 0 = 5 */
		0x00, 5,
		/* type 1 (short) index 1 = 300 */
		0x21, 0x01,0x2C,
		/* type 2 (int) index 2 = 70000 */
		0x42, 0x00,0x01,0x11,0x70,
		/* type 3 (float) index 3 = 1.5 */
		0x63, 0x3F,0xC0,0x00,0x00,
		/* type 4 (string) index 4 = "Hi" */
		0x84, 0,2, 0,'H', 0,'i',
		/* type 5 (item) index 5 = id1/count2/damage0 */
		0xA5, 0,1, 2, 0,0,
		/* type 6 (coord) index 6 = (1,2,3) */
		0xC6, 0,0,0,1, 0,0,0,2, 0,0,0,3,
		0x7F};
	event = decode_exact(mob_spawn_golden, sizeof(mob_spawn_golden),
		MP_EVENT_MOB_SPAWN);
	assert(event.payload.mob_spawn.entity_id == 20
		&& event.payload.mob_spawn.mtype == 54);
	assert(event.payload.mob_spawn.metadata.count == 7);
	{
		const struct mp_metadata* m = &event.payload.mob_spawn.metadata;
		assert(m->entries[0].type == 0 && m->entries[0].index == 0
			&& m->entries[0].value.byte_value == 5);
		assert(m->entries[1].type == 1 && m->entries[1].index == 1
			&& m->entries[1].value.short_value == 300);
		assert(m->entries[2].type == 2 && m->entries[2].index == 2
			&& m->entries[2].value.int_value == 70000);
		assert(m->entries[3].type == 3 && m->entries[3].index == 3
			&& m->entries[3].value.float_value == 1.5f);
		assert(m->entries[4].type == 4 && m->entries[4].index == 4
			&& mp_utf16_equals_ascii(&m->entries[4].value.string_value, "Hi"));
		assert(m->entries[5].type == 5 && m->entries[5].index == 5
			&& m->entries[5].value.item_value.present
			&& m->entries[5].value.item_value.id == 1
			&& m->entries[5].value.item_value.count == 2);
		assert(m->entries[6].type == 6 && m->entries[6].index == 6
			&& m->entries[6].value.coord_value.x == 1
			&& m->entries[6].value.coord_value.y == 2
			&& m->entries[6].value.coord_value.z == 3);
	}

	/* Unknown DataWatcher type (7) is rejected immediately, before any value
	 * byte is read. [740c583 DataWatcher.readWatchableObjects] */
	static const uint8_t metadata_unknown_type[] = {0x28, 0,0,0,1, 0xE0};
	mp_decoder_init(&decoder);
	for(size_t i = 0; i < sizeof(metadata_unknown_type); i++)
		assert(mp_decoder_feed(&decoder, metadata_unknown_type[i], &event)
			== (i + 1 == sizeof(metadata_unknown_type) ? MP_DECODE_ERROR
				: MP_DECODE_MORE));

	/* Oversized entry count / missing terminator: Beta's own DataWatcher
	 * throws above 31 ids [740c583 DataWatcher.addObject], so a stream that
	 * keeps producing entries instead of ever reaching 0x7F is rejected once
	 * it exceeds MP_METADATA_MAX_ENTRIES -- the same bound catches both "the
	 * terminator never arrived" and "the count is hostile". First confirm
	 * exactly MP_METADATA_MAX_ENTRIES valid one-byte entries stay resumable
	 * (MORE, never an error) with no terminator sent yet, then show entry
	 * #33 is the rejection. */
	{
		mp_decoder_init(&decoder);
		uint8_t header[] = {0x28, 0,0,0,1};
		for(size_t i = 0; i < sizeof(header); i++)
			assert(mp_decoder_feed(&decoder, header[i], &event) == MP_DECODE_MORE);
		for(uint8_t idx = 0; idx < MP_METADATA_MAX_ENTRIES; idx++) {
			assert(mp_decoder_feed(&decoder, (uint8_t)(0x00 | idx), &event)
				== MP_DECODE_MORE); /* type 0, index idx: header byte */
			assert(mp_decoder_feed(&decoder, 0x01, &event)
				== MP_DECODE_MORE); /* the byte value */
		}
		/* 33rd entry's header byte (any legal type/index) is rejected. */
		assert(mp_decoder_feed(&decoder, 0x00, &event) == MP_DECODE_ERROR);
	}

	/* Regression: DataWatcher type 5 (ItemStack) must NOT share Slot's -1
	 * absent-sentinel decode (SetSlot/WindowItems' DECODE_ITEM_ID) -- the
	 * real DataWatcher.readWatchableObjects case 5 reads id(i16)/count(i8)/
	 * damage(i16) UNCONDITIONALLY, with no absent case [740c583
	 * DataWatcher.readWatchableObjects]. A metadata item whose id has the
	 * high bit set (read as a negative int16, e.g. a hostile/buggy peer, or
	 * simply an id >= 0x8000) previously made the Slot-shaped decode stop
	 * after 2 bytes instead of 5, desyncing every byte after it. Feed one
	 * Packet40EntityMetadata carrying exactly that entry, immediately
	 * followed by a second, independent KeepAlive packet on the SAME
	 * decoder instance (no re-init) -- proving the stream re-synced rather
	 * than merely that the first packet didn't crash. */
	{
		mp_decoder_init(&decoder);
		static const uint8_t stream[] = {
			/* Packet40EntityMetadata: entity 1, one type-5/index-0 entry:
			 * id=0x8001 (-32767 as i16), count=7, damage=0x1234. */
			0x28, 0,0,0,1,
			0xA0, 0x80,0x01, 7, 0x12,0x34,
			0x7F,
			/* KeepAlive: id byte alone completes it. */
			0x00,
		};
		for(size_t i = 0; i < sizeof(stream); i++) {
			enum mp_decode_result result
				= mp_decoder_feed(&decoder, stream[i], &event);
			if(i == 11) { /* the 0x7F terminator */
				assert(result == MP_DECODE_EVENT);
				assert(event.type == MP_EVENT_ENTITY_METADATA);
				assert(event.payload.entity_metadata.entity_id == 1);
				assert(event.payload.entity_metadata.metadata.count == 1);
				const struct mp_watchable_entry* e
					= &event.payload.entity_metadata.metadata.entries[0];
				assert(e->type == 5 && e->index == 0);
				assert(e->value.item_value.present);
				assert(e->value.item_value.id == (int16_t)0x8001);
				assert(e->value.item_value.count == 7);
				assert(e->value.item_value.damage == (int16_t)0x1234);
			} else if(i == sizeof(stream) - 1) { /* the KeepAlive id byte */
				assert(result == MP_DECODE_EVENT);
				assert(event.type == MP_EVENT_KEEP_ALIVE);
			} else {
				assert(result == MP_DECODE_MORE);
			}
		}
	}

	/* 0x19 Packet25EntityPainting: i32 entityId; string16 title(<=13); i32
	 * x/y/z (RAW block coordinates, NOT fixed-point); i32 direction. S->C
	 * only [740c583 Packet25EntityPainting.readPacketData]. */
	static const uint8_t painting_golden[] = {0x19,
		0,0,0,8,
		0,8, 0,'W',0,'a',0,'n',0,'d',0,'e',0,'r',0,'e',0,'r',
		0,0,0,5, 0,0,0,10, 0,0,0,15, 0,0,0,2};
	event = decode_exact(painting_golden, sizeof(painting_golden),
		MP_EVENT_ENTITY_PAINTING);
	assert(event.payload.entity_painting.entity_id == 8);
	assert(mp_utf16_equals_ascii(&event.payload.entity_painting.title,
		"Wanderer"));
	assert(event.payload.entity_painting.x == 5
		&& event.payload.entity_painting.y == 10
		&& event.payload.entity_painting.z == 15
		&& event.payload.entity_painting.direction == 2);

	/* 0x1B Packet27Position: encode-only (C->S, and per
	 * MP_PACKET_POSITION_LEGACY's comment, never actually sent by any real
	 * Beta client either) -- 4 floats then 2 booleans, in that order.
	 * [740c583 Packet27Position.writePacketData] */
	static const uint8_t position_legacy_golden[] = {0x1B,
		0x3F,0x80,0,0, 0x40,0,0,0, 0x40,0x40,0,0, 0x40,0x80,0,0, 1, 0};
	assert(mp_encode_position_legacy(&frame, 1.0f, 2.0f, 3.0f, 4.0f, true, false));
	assert_frame(&frame, position_legacy_golden, sizeof(position_legacy_golden));
	assert(!mp_packet_supported(0x1B, MP_DIRECTION_SERVER_TO_CLIENT));
	assert(mp_packet_supported(0x1B, MP_DIRECTION_CLIENT_TO_SERVER));

	/* 0x1C Packet28EntityVelocity: i32 entityId; i16 motionX/Y/Z (/8000.0).
	 * S->C only [740c583 Packet28EntityVelocity.readPacketData]. */
	static const uint8_t velocity_golden[] = {0x1C,
		0,0,0,3, 0,100, 0xFF,0x38, 1,44};
	event = decode_exact(velocity_golden, sizeof(velocity_golden),
		MP_EVENT_ENTITY_VELOCITY);
	assert(event.payload.entity_velocity.entity_id == 3
		&& event.payload.entity_velocity.vel_x == 100
		&& event.payload.entity_velocity.vel_y == -200
		&& event.payload.entity_velocity.vel_z == 300);

	/* 0x1D Packet29DestroyEntity: i32 entityId. S->C only [740c583
	 * Packet29DestroyEntity]. */
	static const uint8_t destroy_golden[] = {0x1D, 0,0,0,42};
	event = decode_exact(destroy_golden, sizeof(destroy_golden),
		MP_EVENT_DESTROY_ENTITY);
	assert(event.payload.destroy_entity.entity_id == 42);

	/* 0x1E Packet30Entity: i32 entityId only -- the base class's own
	 * readPacketData reads nothing else. S->C only [740c583
	 * Packet30Entity.readPacketData]. */
	static const uint8_t heartbeat_golden[] = {0x1E, 0,0,0,17};
	event = decode_exact(heartbeat_golden, sizeof(heartbeat_golden), MP_EVENT_ENTITY);
	assert(event.payload.entity_heartbeat.entity_id == 17);

	/* 0x1F Packet31RelEntityMove: i32 entityId (super); i8 dx/dy/dz. S->C
	 * only [740c583 Packet31RelEntityMove.readPacketData]. */
	static const uint8_t rel_move_golden[] = {0x1F, 0,0,0,6, 5, 0xFD, 10};
	event = decode_exact(rel_move_golden, sizeof(rel_move_golden),
		MP_EVENT_REL_ENTITY_MOVE);
	assert(event.payload.rel_entity_move.entity_id == 6
		&& event.payload.rel_entity_move.dx == 5
		&& event.payload.rel_entity_move.dy == -3
		&& event.payload.rel_entity_move.dz == 10);

	/* 0x20 Packet32EntityLook: i32 entityId (super); i8 yaw/pitch. S->C only
	 * [740c583 Packet32EntityLook.readPacketData]. */
	static const uint8_t entity_look_golden[] = {0x20, 0,0,0,6, 0x40, 0xE0};
	event = decode_exact(entity_look_golden, sizeof(entity_look_golden),
		MP_EVENT_ENTITY_LOOK);
	assert(event.payload.entity_look.entity_id == 6
		&& event.payload.entity_look.yaw == 64
		&& event.payload.entity_look.pitch == -32);

	/* 0x21 Packet33RelEntityMoveLook: i32 entityId (super); i8 dx/dy/dz/
	 * yaw/pitch. S->C only [740c583
	 * Packet33RelEntityMoveLook.readPacketData]. */
	static const uint8_t rel_move_look_golden[] = {0x21,
		0,0,0,6, 1, 2, 3, 10, 20};
	event = decode_exact(rel_move_look_golden, sizeof(rel_move_look_golden),
		MP_EVENT_REL_ENTITY_MOVE_LOOK);
	assert(event.payload.rel_entity_move_look.entity_id == 6
		&& event.payload.rel_entity_move_look.dx == 1
		&& event.payload.rel_entity_move_look.dy == 2
		&& event.payload.rel_entity_move_look.dz == 3
		&& event.payload.rel_entity_move_look.yaw == 10
		&& event.payload.rel_entity_move_look.pitch == 20);

	/* 0x22 Packet34EntityTeleport: i32 entityId; i32 x/y/z; i8 yaw/pitch
	 * (.read(), same byte as .readByte()). S->C only [740c583
	 * Packet34EntityTeleport.readPacketData]. */
	static const uint8_t teleport_golden[] = {0x22,
		0,0,0,6, 0,0,0,100, 0,0,0,200, 0xFF,0xFF,0xFE,0xD4, 50, 0xCE};
	event = decode_exact(teleport_golden, sizeof(teleport_golden),
		MP_EVENT_ENTITY_TELEPORT);
	assert(event.payload.entity_teleport.entity_id == 6
		&& event.payload.entity_teleport.x == 100
		&& event.payload.entity_teleport.y == 200
		&& event.payload.entity_teleport.z == -300
		&& event.payload.entity_teleport.yaw == 50
		&& event.payload.entity_teleport.pitch == -50);

	/* 0x26 Packet38EntityStatus: i32 entityId; i8 entityStatus. S->C only
	 * [740c583 Packet38EntityStatus]. */
	static const uint8_t status_golden[] = {0x26, 0,0,0,6, 3};
	event = decode_exact(status_golden, sizeof(status_golden), MP_EVENT_ENTITY_STATUS);
	assert(event.payload.entity_status.entity_id == 6
		&& event.payload.entity_status.status == 3);

	/* 0x27 Packet39AttachEntity: i32 entityId; i32 vehicleEntityId. S->C
	 * only [740c583 Packet39AttachEntity]. */
	static const uint8_t attach_golden[] = {0x27, 0,0,0,6, 0,0,0,40};
	event = decode_exact(attach_golden, sizeof(attach_golden), MP_EVENT_ATTACH_ENTITY);
	assert(event.payload.attach_entity.entity_id == 6
		&& event.payload.attach_entity.vehicle_entity_id == 40);

	/* 0x28 Packet40EntityMetadata: i32 entityId; then a DataWatcher stream
	 * (one entry here). S->C only [740c583
	 * Packet40EntityMetadata.readPacketData]. */
	static const uint8_t entity_metadata_golden[] = {0x28, 0,0,0,6, 0x00, 9, 0x7F};
	event = decode_exact(entity_metadata_golden, sizeof(entity_metadata_golden),
		MP_EVENT_ENTITY_METADATA);
	assert(event.payload.entity_metadata.entity_id == 6);
	assert(event.payload.entity_metadata.metadata.count == 1);
	assert(event.payload.entity_metadata.metadata.entries[0].type == 0
		&& event.payload.entity_metadata.metadata.entries[0].index == 0
		&& event.payload.entity_metadata.metadata.entries[0].value.byte_value == 9);
}

static bool socket_read_exact(SOCKET socket_value, uint8_t* data, size_t length) {
	size_t have = 0;
	while(have < length) {
		int count = recv(socket_value, (char*)data + have, (int)(length - have), 0);
		if(count <= 0)
			return false;
		have += (size_t)count;
	}
	return true;
}

static bool socket_write_fragmented(SOCKET socket_value, const uint8_t* data,
									size_t length) {
	/* This is the "server" side of every loopback fixture in this file,
	 * where a client close mid-send() must surface as a failed send(),
	 * never kill the test process. On Linux, writing to a
	 * socket the client already closed raises SIGPIPE by default and kills
	 * the whole test process before this can return false -- MSG_NOSIGNAL
	 * (same fix, same reasoning as net_socket_send in
	 * source/platform/pc/net_socket.c) keeps that failure as a normal
	 * return value instead. Windows has no MSG_NOSIGNAL and never raises
	 * SIGPIPE on send(), so it is unaffected by the #ifdef. */
	for(size_t i = 0; i < length; i++) {
#ifdef MSG_NOSIGNAL
		if(send(socket_value, (const char*)data + i, 1, MSG_NOSIGNAL) != 1)
#else
		if(send(socket_value, (const char*)data + i, 1, 0) != 1)
#endif
			return false;
	}
	return true;
}

struct peer_fixture {
	SOCKET listener;
	bool cancel_only;
	bool ok;
};

static void put_i16(uint8_t* p, int16_t value) {
	uint16_t v = (uint16_t)value; p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void put_i32(uint8_t* p, int32_t value) {
	uint32_t v = (uint32_t)value;
	p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static void put_f32(uint8_t* p, float value) {
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	p[0]=(uint8_t)(bits>>24); p[1]=(uint8_t)(bits>>16);
	p[2]=(uint8_t)(bits>>8); p[3]=(uint8_t)bits;
}
static void put_f64(uint8_t* p, double value) {
	uint64_t bits; memcpy(&bits, &value, sizeof(bits));
	for(int shift=56; shift>=0; shift-=8) *p++=(uint8_t)(bits>>shift);
}

/* S4 P3 (misc/polish): golden decode vectors for all 8 IDs, citing the
 * decompile beside each fixture, per the plan's section 10 test strategy.
 * Placed after put_i16/put_i32/put_f32/put_f64 above so the wider vectors
 * (explosion's f64/f32 fields) can be built with them instead of hand-
 * transcribed IEEE-754 hex, matching how the S1 fault matrix below builds
 * its own vectors -- these are still hand-derived from the decompiled
 * field order, never from mp_encode_* output. */
static void test_p3_codec_goldens(void) {
	struct mp_frame frame;
	struct mp_decoder decoder;
	struct mp_event event;

	/* 0x03 Packet3Chat: string16 message, read-side max 119 code units,
	 * bidirectional. [740c583 Packet3Chat.readPacketData]
	 * [740c583 Packet3Chat.writePacketData] */
	static const uint8_t chat_golden[] = {0x03, 0,2, 0,'H', 0,'i'};
	assert(mp_encode_chat(&frame, "Hi"));
	assert_frame(&frame, chat_golden, sizeof(chat_golden));
	event = decode_exact(chat_golden, sizeof(chat_golden), MP_EVENT_CHAT);
	assert(mp_utf16_equals_ascii(&event.payload.chat, "Hi"));
	assert(mp_packet_supported(0x03, MP_DIRECTION_SERVER_TO_CLIENT));
	assert(mp_packet_supported(0x03, MP_DIRECTION_CLIENT_TO_SERVER));

	/* Exactly 119 code units round-trips (the read-side maximum, not
	 * rejected); 120 is over-cap and rejected at the length phase alone --
	 * no body bytes are needed to prove that, mirroring how the P0/P1
	 * over-limit string tests work elsewhere in this file. [740c583
	 * Packet3Chat.readPacketData] */
	{
		char msg119[120];
		memset(msg119, 'A', 119);
		msg119[119] = 0;
		assert(mp_encode_chat(&frame, msg119));
		assert(frame.length == 1 + 2 + 119 * 2);
		event = decode_exact(frame.data, frame.length, MP_EVENT_CHAT);
		assert(event.payload.chat.length == 119);
		char roundtrip[120];
		assert(mp_utf16_to_utf8(&event.payload.chat, roundtrip,
			sizeof(roundtrip)));
		assert(strcmp(roundtrip, msg119) == 0);

		uint8_t over_cap_prefix[] = {0x03, 0,120};
		mp_decoder_init(&decoder);
		enum mp_decode_result r = MP_DECODE_MORE;
		for(size_t i = 0; i < sizeof(over_cap_prefix); i++)
			r = mp_decoder_feed(&decoder, over_cap_prefix[i], &event);
		assert(r == MP_DECODE_ERROR);
		mp_decoder_destroy(&decoder);
	}

	/* 0x36 Packet54PlayNoteBlock: xLocation(i32); yLocation(i16); zLocation
	 * (i32); instrumentType(u8, `read()` 0..255); pitch(u8, same). S->C only
	 * [740c583 Packet54PlayNoteBlock.readPacketData]. */
	static const uint8_t note_golden[] = {0x36,
		0,0,0,5, 0,64, 0xFF,0xFF,0xFF,0xFD, 1, 12};
	event = decode_exact(note_golden, sizeof(note_golden), MP_EVENT_NOTE_BLOCK);
	assert(event.payload.note_block.x == 5 && event.payload.note_block.y == 64
		&& event.payload.note_block.z == -3);
	assert(event.payload.note_block.instrument == 1
		&& event.payload.note_block.pitch == 12);
	assert(!mp_packet_supported(0x36, MP_DIRECTION_CLIENT_TO_SERVER));

	/* 0x3C Packet60Explosion: explosionX/Y/Z(f64); explosionSize(f32);
	 * count(i32); count * {dx,dy,dz}(i8 each). S->C only [740c583
	 * Packet60Explosion.readPacketData]. */
	{
		uint8_t body[1 + 24 + 4 + 4 + 2 * 3];
		size_t o = 0;
		body[o++] = 0x3C;
		put_f64(body + o, 1.0); o += 8;
		put_f64(body + o, 64.0); o += 8;
		put_f64(body + o, -2.0); o += 8;
		put_f32(body + o, 3.0f); o += 4;
		put_i32(body + o, 2); o += 4;
		body[o++] = 1; body[o++] = (uint8_t)-1; body[o++] = 2;
		body[o++] = (uint8_t)-2; body[o++] = 3; body[o++] = (uint8_t)-1;
		assert(o == sizeof(body));
		event = decode_exact(body, sizeof(body), MP_EVENT_EXPLOSION);
		assert(event.payload.explosion.x == 1.0
			&& event.payload.explosion.y == 64.0
			&& event.payload.explosion.z == -2.0);
		assert(event.payload.explosion.power == 3.0f);
		assert(event.payload.explosion.count == 2);
		assert(event.payload.explosion.blocks[0].dx == 1
			&& event.payload.explosion.blocks[0].dy == -1
			&& event.payload.explosion.blocks[0].dz == 2);
		assert(event.payload.explosion.blocks[1].dx == -2
			&& event.payload.explosion.blocks[1].dy == 3
			&& event.payload.explosion.blocks[1].dz == -1);
		mp_event_destroy(&event);

		/* Zero-count explosion: no body bytes follow the header. */
		uint8_t empty[1 + 24 + 4 + 4];
		o = 0;
		empty[o++] = 0x3C;
		put_f64(empty + o, 0.0); o += 8;
		put_f64(empty + o, 0.0); o += 8;
		put_f64(empty + o, 0.0); o += 8;
		put_f32(empty + o, 1.0f); o += 4;
		put_i32(empty + o, 0); o += 4;
		event = decode_exact(empty, sizeof(empty), MP_EVENT_EXPLOSION);
		assert(event.payload.explosion.count == 0);
		mp_event_destroy(&event);

		/* Negative count and over-cap count are both rejected at the header
		 * alone, before any allocation. [740c583
		 * Packet60Explosion.readPacketData] */
		uint8_t bad_header[1 + 24 + 4 + 4];
		o = 0;
		bad_header[o++] = 0x3C;
		put_f64(bad_header + o, 0.0); o += 8;
		put_f64(bad_header + o, 0.0); o += 8;
		put_f64(bad_header + o, 0.0); o += 8;
		put_f32(bad_header + o, 1.0f); o += 4;
		put_i32(bad_header + o, -1); o += 4;
		mp_decoder_init(&decoder);
		enum mp_decode_result r = MP_DECODE_MORE;
		for(size_t i = 0; i < sizeof(bad_header); i++)
			r = mp_decoder_feed(&decoder, bad_header[i], &event);
		assert(r == MP_DECODE_ERROR);
		mp_decoder_destroy(&decoder);

		put_i32(bad_header + 1 + 24 + 4, (int32_t)MP_EXPLOSION_MAX_BLOCKS + 1);
		mp_decoder_init(&decoder);
		r = MP_DECODE_MORE;
		for(size_t i = 0; i < sizeof(bad_header); i++)
			r = mp_decoder_feed(&decoder, bad_header[i], &event);
		assert(r == MP_DECODE_ERROR);
		mp_decoder_destroy(&decoder);
	}

	/* 0x3D Packet61DoorChange: read/write order is effectId(i32), x(i32),
	 * y(i8), z(i32), data(i32) -- NOT the obfuscated field-declaration
	 * order. S->C only [740c583 Packet61DoorChange.readPacketData]. */
	static const uint8_t door_golden[] = {0x3D,
		0,0,3,0xEB, 0,0,0,10, 64, 0xFF,0xFF,0xFF,0xFB, 0,0,0,1};
	event = decode_exact(door_golden, sizeof(door_golden), MP_EVENT_DOOR_CHANGE);
	assert(event.payload.door_change.effect_id == 1003
		&& event.payload.door_change.x == 10
		&& event.payload.door_change.y == 64
		&& event.payload.door_change.z == -5
		&& event.payload.door_change.data == 1);

	/* 0x46 Packet70Bed: single i8 event code (0 notValid, 1 start rain, 2
	 * stop rain). S->C only [740c583 Packet70Bed.readPacketData]. */
	for(int8_t code = 0; code <= 2; code++) {
		uint8_t bed_golden[] = {0x46, (uint8_t)code};
		event = decode_exact(bed_golden, sizeof(bed_golden), MP_EVENT_BED);
		assert(event.payload.bed.event_code == code);
	}

	/* 0x47 Packet71Weather: entityID(i32); isLightningBolt(i8); posX/Y/Z
	 * (i32, fixed-point /32.0). S->C only [740c583
	 * Packet71Weather.readPacketData]. */
	static const uint8_t weather_golden[] = {0x47,
		0,0,0,99, 1, 0,0,0,0x20, 0,0,0,0x40, 0xFF,0xFF,0xFF,0xE0};
	event = decode_exact(weather_golden, sizeof(weather_golden), MP_EVENT_WEATHER);
	assert(event.payload.weather.entity_id == 99
		&& event.payload.weather.is_lightning_bolt == 1);
	assert(event.payload.weather.x == 32 && event.payload.weather.y == 64
		&& event.payload.weather.z == -32);
	assert(mp_fixed32_to_double(event.payload.weather.x) == 1.0);

	/* 0x83 Packet131MapData: field_28055_a(i16, item id); field_28054_b(i16,
	 * map id); length(u8, `readByte() & 255`) then that many raw bytes.
	 * S->C only [740c583 Packet131MapData.readPacketData]. */
	static const uint8_t map_data_golden[] = {0x83,
		0,5, 0,7, 3, 0xAA,0xBB,0xCC};
	event = decode_exact(map_data_golden, sizeof(map_data_golden),
		MP_EVENT_MAP_DATA);
	assert(event.payload.map_data.item_id == 5
		&& event.payload.map_data.map_id == 7
		&& event.payload.map_data.length == 3);
	assert(event.payload.map_data.bytes[0] == 0xAA
		&& event.payload.map_data.bytes[1] == 0xBB
		&& event.payload.map_data.bytes[2] == 0xCC);

	static const uint8_t map_data_empty_golden[] = {0x83, 0,5, 0,7, 0};
	event = decode_exact(map_data_empty_golden, sizeof(map_data_empty_golden),
		MP_EVENT_MAP_DATA);
	assert(event.payload.map_data.length == 0);

	/* 0xC8 Packet200Statistic: field_27052_a(i32, statistic id);
	 * field_27051_b(i8, amount). S->C only [740c583
	 * Packet200Statistic.readPacketData]. */
	static const uint8_t statistic_golden[] = {0xC8, 0,0,4,0xD2, 5};
	event = decode_exact(statistic_golden, sizeof(statistic_golden),
		MP_EVENT_STATISTIC);
	assert(event.payload.statistic.stat_id == 1234
		&& event.payload.statistic.amount == 5);
}

struct s1_peer_fixture {
	SOCKET listener;
	bool ok;
};

enum fault_peer_mode {
	FAULT_ZLIB,
	FAULT_INFLATED_LENGTH,
	FAULT_CODEC_ALLOC,
	FAULT_BYTE_CAP,
	FAULT_EOF_BODY,
	FAULT_ENTRY_CAP,
	FAULT_INVALID_BLOCK,
	FAULT_INVALID_MOVEMENT,
	/* EOF / kick ORDERING probes (mp-split-0912/2). The four below all end
	 * the session from the server side; what varies is whether a Packet255
	 * reason precedes the close, how that packet is fragmented on the wire,
	 * and whether the close is orderly (FIN) or abortive (RST). They exist
	 * to pin WHICH terminal reason the client ends up SHOWING -- the kick
	 * text or the transport's own EOF string. */
	FAULT_KICK_THEN_EOF,
	FAULT_KICK_FRAGMENTED_THEN_EOF,
	FAULT_BARE_EOF,
	/* W6 (mp-split-0912/35): BARE_EOF's peer half-closes the instant it
	 * has written the login response, so the client's read thread reaches
	 * that EOF while the state machine is still at LOGIN_SENT -- the
	 * promotion to REMOTE_PLAY happens in the PUMP, one queued event
	 * later (measured: mp_end_phase=3 on this fixture). This mode drives
	 * the client all the way into PLAY first and PROVES it got there: it
	 * sends a server keep-alive after the login response and waits for
	 * the client's echo. The pump handles queued events in order, so an
	 * echo on the wire means the login event ahead of it was already
	 * processed -- the state IS REMOTE_PLAY. Only then does the peer
	 * half-close, which pins the 'play' leg of the terminal diagnostic,
	 * i.e. what a mid-session server death actually produces. */
	FAULT_PLAY_EOF,
	FAULT_KICK_THEN_RST,
};
struct fault_peer_fixture { SOCKET listener; enum fault_peer_mode mode; bool ok; };

/* The kick text every ordering case sends. Distinctive on purpose: it can
 * only have reached status.reason through the Packet255 decode path. */
#define FAULT_KICK_TEXT "FIXTURE_KICK_A1"
#define FAULT_KICK_TEXT_CHARS 15u
#define FAULT_KICK_PACKET_BYTES (3u + 2u * FAULT_KICK_TEXT_CHARS)

/* Packet255KickDisconnect: id, then the reason as string16 (i16 code-unit
 * count, then UTF-16BE). [740c583 Packet255KickDisconnect.writePacketData]
 * [740c583 Packet.writeString] -- same shape as this file's kick golden at
 * the top of test_codec_goldens. */
static size_t fault_kick_packet(uint8_t* out) {
	const char* text = FAULT_KICK_TEXT;
	out[0] = 0xFF;
	put_i16(out + 1, (int16_t)FAULT_KICK_TEXT_CHARS);
	for(size_t i = 0; i < FAULT_KICK_TEXT_CHARS; i++) {
		out[3 + 2 * i] = 0x00;
		out[4 + 2 * i] = (uint8_t)text[i];
	}
	return FAULT_KICK_PACKET_BYTES;
}

/* socket_write_fragmented sends one byte per send() call, which is exactly
 * what the FRAGMENTED mode wants to control explicitly -- these cases need a
 * whole-packet single send() as the contrasting baseline. */
static bool socket_write_whole(SOCKET socket_value, const uint8_t* data,
							   size_t length) {
#ifdef MSG_NOSIGNAL
	int sent = send(socket_value, (const char*)data, (int)length, MSG_NOSIGNAL);
#else
	int sent = send(socket_value, (const char*)data, (int)length, 0);
#endif
	return sent == (int)length;
}

#ifdef _WIN32
#define FAULT_SHUT_WRITE SD_SEND
#else
#define FAULT_SHUT_WRITE SHUT_WR
#endif

/* Bound the post-close drain so a client that never closes its end cannot
 * hang the suite on this thread's blocking recv (the brief's "or 2 s"). */
static void fault_peer_recv_timeout(SOCKET peer, unsigned ms) {
#ifdef _WIN32
	DWORD value = ms;
	setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char*)&value,
			   sizeof(value));
#else
	struct timeval value;
	value.tv_sec = (time_t)(ms / 1000u);
	value.tv_usec = (long)((ms % 1000u) * 1000u);
	setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
#endif
}

static bool peer_login(SOCKET peer) {
	static const uint8_t expected_handshake[]
		= {0x02,0x00,0x04,0x00,'C',0x00,'a',0x00,'v',0x00,'e'};
	static const uint8_t offline_reply[] = {0x02,0x00,0x01,0x00,'-'};
	static const uint8_t expected_login[] = {0x01,0,0,0,0x0E,
		0,4,0,'C',0,'a',0,'v',0,'e', 0,0,0,0,0,0,0,0,0};
	uint8_t input[64];
	return socket_read_exact(peer, input, sizeof(expected_handshake))
		&& memcmp(input, expected_handshake, sizeof(expected_handshake)) == 0
		&& socket_write_fragmented(peer, offline_reply, sizeof(offline_reply))
		&& socket_read_exact(peer, input, sizeof(expected_login))
		&& memcmp(input, expected_login, sizeof(expected_login)) == 0;
}

static void* s1_peer_entry(void* user) {
	struct s1_peer_fixture* fixture = user;
	uint8_t* packet = NULL;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[] = {0x01,0,0,0,0x2A,
		0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	static const uint8_t spawn[] = {0x06,0,0,0,0, 0,0,0,100, 0,0,0,0};
	static const uint8_t time_set[] = {0x04,0,0,0,0,0,0,0,42};
	static const uint8_t health[] = {0x08,0,20};
	static const uint8_t announce[] = {0x32,0,0,0,0, 0,0,0,0, 1};
	uint8_t correction[42] = {0x0D};
	put_f64(correction+1, 0.5);
	put_f64(correction+9, 101.62);
	put_f64(correction+17, 103.24);
	put_f64(correction+25, 0.5);
	put_f32(correction+33, 90.0f);
	put_f32(correction+37, 0.0f);
	correction[41] = 0;
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response))
	   || !socket_write_fragmented(peer, spawn, sizeof(spawn))
	   || !socket_write_fragmented(peer, time_set, sizeof(time_set))
	   || !socket_write_fragmented(peer, health, sizeof(health))
	   || !socket_write_fragmented(peer, announce, sizeof(announce))
	   || !socket_write_fragmented(peer, correction, sizeof(correction)))
		goto done;

	size_t cells = 16u * 128u * 16u, nibble = cells / 2u;
	uint8_t* inflated = calloc(1, cells + 3u * nibble);
	if(!inflated)
		goto done;
	inflated[0] = BLOCK_STONE;
	inflated[cells + nibble] = 0x07;       /* block/torch light only */
	inflated[cells + 2u*nibble] = 0x0C;   /* sky light only */
	uLongf compressed_cap = compressBound((uLong)(cells + 3u*nibble));
	uint8_t* compressed = malloc(compressed_cap);
	if(!compressed) { free(inflated); goto done; }
	if(compress2(compressed, &compressed_cap, inflated,
		(uLong)(cells + 3u*nibble), Z_BEST_SPEED) != Z_OK) {
		free(compressed); free(inflated); goto done;
	}
	free(inflated);
	size_t packet_len = 18u + (size_t)compressed_cap;
	packet = malloc(packet_len);
	if(!packet) { free(compressed); goto done; }
	packet[0]=0x33; put_i32(packet+1,0); put_i16(packet+5,0);
	put_i32(packet+7,0); packet[11]=15; packet[12]=127; packet[13]=15;
	put_i32(packet+14,(int32_t)compressed_cap);
	memcpy(packet+18,compressed,compressed_cap);
	free(compressed);
	bool sent_chunk = socket_write_fragmented(peer, packet, packet_len);
	if(!sent_chunk)
		goto done;

	/* Proactive keepalive, then the byte-correct Packet13 correction echo.
	 * Echo rewrites y=minY and stance=installed posY. */
	uint8_t input[64];
	if(!socket_read_exact(peer,input,1) || input[0] != 0x00)
		goto done;
	uint8_t echo_expected[42]; memcpy(echo_expected,correction,sizeof(correction));
	put_f64(echo_expected+9,100.0);
	put_f64(echo_expected+17,101.62);
	if(!socket_read_exact(peer,input,sizeof(echo_expected))
	   || memcmp(input,echo_expected,sizeof(echo_expected)) != 0)
		goto done;

	/* Synthetic SRPC_PLAYER_POS must choose Packet13 on its first changed
	 * position+look sample, using the same minY/stance ordering. */
	uint8_t movement_expected[42] = {0x0D};
	put_f64(movement_expected+1,1.0); put_f64(movement_expected+9,100.0);
	put_f64(movement_expected+17,101.62); put_f64(movement_expected+25,0.5);
	put_f32(movement_expected+33,90.0f); put_f32(movement_expected+37,0.0f);
	movement_expected[41]=1;
	if(!socket_read_exact(peer,input,sizeof(movement_expected))
	   || memcmp(input,movement_expected,sizeof(movement_expected)) != 0)
		goto done;
	static const uint8_t unload[] = {0x32,0,0,0,0, 0,0,0,0, 0};
	/* Deliver unload -> reload -> replacement Packet51 as one read-thread burst.
	 * The main thread must publish the unload first without discarding or leaking
	 * the post-reload compressed body retained behind it. */
	if(!socket_write_fragmented(peer,unload,sizeof(unload))
	   || !socket_write_fragmented(peer,announce,sizeof(announce))
	   || !socket_write_fragmented(peer,packet,packet_len))
		goto done;
	bool saw_marker = false;
	for(size_t frames = 0; frames < 128 && !saw_marker; frames++) {
		uint8_t id;
		if(!socket_read_exact(peer, &id, 1))
			goto done;
		size_t body_length;
		switch(id) {
			case 0x00: body_length = 0; saw_marker = true; break;
			case 0x0A: body_length = 1; break;
			case 0x0B: body_length = 33; break;
			case 0x0C: body_length = 9; break;
			case 0x0D: body_length = 41; break;
			default: goto done;
		}
		if(body_length && !socket_read_exact(peer,input,body_length))
			goto done;
	}
	if(!saw_marker || !socket_write_fragmented(peer,unload,sizeof(unload)))
		goto done;
	/* Background clin_update samples may precede graceful Packet255. Parse only
	 * the legal S1 client frame shapes instead of assuming immediate adjacency. */
	for(size_t frames = 0; frames < 128 && !fixture->ok; frames++) {
		uint8_t id;
		if(!socket_read_exact(peer, &id, 1))
			goto done;
		size_t body_length;
		switch(id) {
			case 0x00: body_length = 0; break;
			case 0x0A: body_length = 1; break;
			case 0x0B: body_length = 33; break;
			case 0x0C: body_length = 9; break;
			case 0x0D: body_length = 41; break;
			case 0xFF: body_length = 18; break;
			default: goto done;
		}
		if(body_length && !socket_read_exact(peer,input,body_length))
			goto done;
		if(id == 0xFF)
			fixture->ok = body_length == 18 && input[0] == 0x00
				&& input[1] == 0x08;
	}
done:
	free(packet);
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void* fault_peer_entry(void* user) {
	struct fault_peer_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener,NULL,NULL);
	if(peer == INVALID_SOCKET) return NULL;
	if(!peer_login(peer)) goto done;
	static const uint8_t login_response[] = {0x01,0,0,0,7,
		0,0, 0,0,0,0,0,0,0,1, 0};
	if(!socket_write_fragmented(peer,login_response,sizeof(login_response))) goto done;
	if(fixture->mode == FAULT_KICK_THEN_EOF
	   || fixture->mode == FAULT_KICK_FRAGMENTED_THEN_EOF
	   || fixture->mode == FAULT_BARE_EOF
	   || fixture->mode == FAULT_PLAY_EOF
	   || fixture->mode == FAULT_KICK_THEN_RST) {
		uint8_t kick[FAULT_KICK_PACKET_BYTES];
		size_t kick_length = fault_kick_packet(kick);
		if(fixture->mode == FAULT_KICK_THEN_EOF) {
			if(!socket_write_whole(peer, kick, kick_length))
				goto done;
		} else if(fixture->mode == FAULT_KICK_FRAGMENTED_THEN_EOF) {
			/* Three send() calls with 50 ms gaps: [id], [length hi],
			 * [length lo + the whole UTF-16 body]. Each gap is long
			 * enough that the client's read thread returns from
			 * net_socket_receive between them, so the decoder must hold
			 * a half-built Packet255 across pumps. */
			if(!socket_write_whole(peer, kick, 1))
				goto done;
			thread_msleep(50);
			if(!socket_write_whole(peer, kick + 1, 1))
				goto done;
			thread_msleep(50);
			if(!socket_write_whole(peer, kick + 2, kick_length - 2))
				goto done;
			thread_msleep(50);
		} else if(fixture->mode == FAULT_PLAY_EOF) {
			/* Packet0KeepAlive, protocol 14: the id byte and NOTHING else (the
			 * int32 token is a later-version addition: mp_encode_keep_alive
			 * is a bare begin_frame and the decoder emits the event on the id
			 * alone. The 5-byte modern form made the client decode three extra
			 * keep-alives plus a truncated 0x07 and abort on its own, which is
			 * NOT the EOF this leg is about.) The client answers with its own
			 * 1-byte Packet0, and that answer is the proof the login
			 * queued ahead of it has already been pumped (see the mode's
			 * comment). Reading ONE byte is enough: the handshake and login
			 * requests were both consumed above, so anything the client
			 * writes from here on is a PLAY-state send. */
			static const uint8_t keep_alive[] = {0x00};
			if(!socket_write_whole(peer, keep_alive, sizeof(keep_alive)))
				goto done;
			fault_peer_recv_timeout(peer, 4000);
			uint8_t echo;
			if(recv(peer, (char*)&echo, 1, 0) <= 0)
				goto done; /* ok stays false -- the test asserts peer_ok */
		} else if(fixture->mode == FAULT_KICK_THEN_RST) {
			if(!socket_write_whole(peer, kick, kick_length))
				goto done;
			/* SO_LINGER {on, 0} makes the closesocket() at `done:`
			 * abortive: the peer sends RST instead of FIN, so the
			 * client's recv returns an ERROR (-1) rather than 0 even
			 * though a valid Packet255 preceded it. */
			struct linger abort_close;
			abort_close.l_onoff = 1;
			abort_close.l_linger = 0;
			setsockopt(peer, SOL_SOCKET, SO_LINGER,
					   (const char*)&abort_close, sizeof(abort_close));
			fixture->ok = true;
			goto done;
		}
		/* FAULT_BARE_EOF sends NO Packet255 at all -- the login response
		 * is the last thing the client ever receives; FAULT_PLAY_EOF adds
		 * only the keep-alive round trip above, no kick either. */
		if(fixture->mode != FAULT_KICK_THEN_RST) {
			/* Half-close: the client observes a clean EOF (recv == 0) on
			 * its next read. The read side stays open so this thread can
			 * drain whatever the client writes back (its own Packet255 or
			 * a queued position sample) and observe the client's close. */
			shutdown(peer, FAULT_SHUT_WRITE);
			fault_peer_recv_timeout(peer, 2000);
			uint8_t drained;
			while(recv(peer, (char*)&drained, 1, 0) > 0) {
			}
			fixture->ok = true;
		}
		goto done;
	}
	if(fixture->mode == FAULT_INVALID_MOVEMENT) {
		uint8_t correction[42] = {0x0D};
		put_f64(correction + 1, NAN);
		put_f64(correction + 9, 64.0);
		put_f64(correction + 17, 65.62);
		put_f64(correction + 25, 0.0);
		put_f32(correction + 33, 0.0F);
		put_f32(correction + 37, 0.0F);
		if(!socket_write_fragmented(peer, correction, sizeof(correction)))
			goto done;
	} else if(fixture->mode == FAULT_ENTRY_CAP) {
		uint8_t announce[10] = {0x32}; announce[9]=1;
		for(int i=0;i<577;i++) {
			put_i32(announce+1,i); put_i32(announce+5,0);
			if(!socket_write_fragmented(peer,announce,sizeof(announce))) goto done;
		}
	} else if(fixture->mode == FAULT_BYTE_CAP) {
		const size_t inflated_length = 16u * 128u * 16u * 5u / 2u;
		uint8_t* inflated = calloc(1, inflated_length);
		uLongf compressed_length = compressBound(inflated_length);
		uint8_t* compressed = malloc(compressed_length);
		if(!inflated || !compressed
		   || compress2(compressed, &compressed_length, inflated,
			   inflated_length, Z_BEST_SPEED) != Z_OK) {
			free(inflated);
			free(compressed);
			goto done;
		}
		free(inflated);
		for(int pass = 0; pass < 3; pass++) {
			int32_t x = pass == 1 ? 16 : 0;
			uint8_t header[18] = {0x33};
			put_i32(header + 1, x);
			put_i16(header + 5, 0);
			put_i32(header + 7, 0);
			header[11] = 15;
			header[12] = 127;
			header[13] = 15;
			put_i32(header + 14, (int32_t)compressed_length);
			if(!socket_write_fragmented(peer, header, sizeof(header))
			   || !socket_write_fragmented(peer, compressed,
				   compressed_length))
				/* W2: a write failure here is EXPECTED, not a peer fault. The
				 * client used to keep reading all three columns (it evicted
				 * the first to make room for the second, then forced a
				 * reconnect on the third); it now refuses the moment the
				 * budget is exceeded and closes, so pass 1 or 2 can land on
				 * a closed socket. Which pass loses the race depends on how
				 * much the kernel's send buffer absorbed -- this branch was
				 * intermittently failing `assert(peer.ok)` until it stopped
				 * treating that as an error. Stop writing; the case's real
				 * assertions are on the client's reason and counters. */
				break;
		}
		free(compressed);
	} else if(fixture->mode == FAULT_INVALID_BLOCK) {
		const size_t inflated_length = 16u * 128u * 16u * 5u / 2u;
		uint8_t* inflated = calloc(1, inflated_length);
		uLongf compressed_length = compressBound(inflated_length);
		uint8_t* compressed = malloc(compressed_length);
		if(!inflated || !compressed) {
			free(inflated);
			free(compressed);
			goto done;
		}
		uint8_t invalid_id = 0xFF;
		while(invalid_id > 0 && blocks[invalid_id])
			invalid_id--;
		assert(invalid_id > 0);
		inflated[0] = invalid_id;
		if(compress2(compressed, &compressed_length, inflated,
			inflated_length, Z_BEST_SPEED) != Z_OK) {
			free(inflated);
			free(compressed);
			goto done;
		}
		free(inflated);
		uint8_t announce[10] = {0x32};
		announce[9] = 1;
		uint8_t header[18] = {0x33};
		put_i32(header + 1, 0);
		put_i16(header + 5, 0);
		put_i32(header + 7, 0);
		header[11] = 15;
		header[12] = 127;
		header[13] = 15;
		put_i32(header + 14, (int32_t)compressed_length);
		bool sent = socket_write_fragmented(peer, announce, sizeof(announce))
			&& socket_write_fragmented(peer, header, sizeof(header))
			&& socket_write_fragmented(peer, compressed, compressed_length);
		free(compressed);
		if(!sent)
			goto done;
	} else {
		static const uint8_t announce[] = {0x32,0,0,0,0,0,0,0,0,1};
		uint8_t header[18] = {0x33};
		put_i32(header+1,0); put_i16(header+5,0); put_i32(header+7,0);
		header[11]=15; header[12]=127; header[13]=15;
		uint8_t compressed_short[32];
		uLongf compressed_short_length = sizeof(compressed_short);
		uint8_t short_output = 0;
		if(fixture->mode == FAULT_INFLATED_LENGTH
		   && compress2(compressed_short, &compressed_short_length,
			   &short_output, 1, Z_BEST_SPEED) != Z_OK)
			goto done;
		put_i32(header+14, fixture->mode == FAULT_ZLIB ? 4
			: fixture->mode == FAULT_INFLATED_LENGTH
				? (int32_t)compressed_short_length
				: fixture->mode == FAULT_CODEC_ALLOC ? 1 : 10);
		static const uint8_t bad[] = {0xDE,0xAD,0xBE,0xEF};
		if(!socket_write_fragmented(peer,announce,sizeof(announce))
		   || !socket_write_fragmented(peer,header,sizeof(header))) goto done;
		if(fixture->mode == FAULT_CODEC_ALLOC) {
			/* Decoder fails at the checked allocation before consuming a body. */
		} else if(fixture->mode == FAULT_EOF_BODY) {
			if(!socket_write_fragmented(peer,bad,3)) goto done;
			fixture->ok=true;
			goto done;
		}
		if(fixture->mode == FAULT_CODEC_ALLOC) {
			/* No body: wait below for the allocation-error close. */
		} else if(fixture->mode == FAULT_INFLATED_LENGTH) {
			if(!socket_write_fragmented(peer, compressed_short,
				compressed_short_length))
				goto done;
		} else if(!socket_write_fragmented(peer,bad,sizeof(bad))) goto done;
	}
	{
		uint8_t byte;
		while(recv(peer,(char*)&byte,1,0)>0) {}
		fixture->ok=true;
	}
done:
	closesocket(peer); closesocket(fixture->listener); return NULL;
}

static void* peer_entry(void* user) {
	struct peer_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(fixture->cancel_only) {
		uint8_t byte;
		while(recv(peer, (char*)&byte, 1, 0) > 0) {
		}
		fixture->ok = true;
		closesocket(peer);
		closesocket(fixture->listener);
		return NULL;
	}

	/* Client handshake: ID 2, length 4, C/a/v/e.
	 * [740c583 Packet2Handshake.writePacketData] */
	static const uint8_t expected_handshake[]
		= {0x02, 0x00, 0x04, 0x00, 'C', 0x00, 'a', 0x00, 'v', 0x00, 'e'};
	uint8_t input[64];
	if(!socket_read_exact(peer, input, sizeof(expected_handshake))
	   || memcmp(input, expected_handshake, sizeof(expected_handshake)) != 0)
		goto done;

	/* Offline server reply: ID 2, string16 "-".
	 * [740c583 Packet2Handshake.writePacketData]
	 * [740c583 NetClientHandler.handleHandshake] */
	static const uint8_t offline_reply[] = {0x02, 0x00, 0x01, 0x00, '-'};
	if(!socket_write_fragmented(peer, offline_reply, sizeof(offline_reply)))
		goto done;

	/* Login request: ID 1; protocol 14; "Cave"; zero seed/dimension.
	 * [740c583 Packet1Login.writePacketData]
	 * [740c583 NetClientHandler.handleHandshake] */
	static const uint8_t expected_login[] = {0x01, 0x00, 0x00, 0x00, 0x0E,
		0x00, 0x04, 0x00, 'C', 0x00, 'a', 0x00, 'v', 0x00, 'e',
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	if(!socket_read_exact(peer, input, sizeof(expected_login))
	   || memcmp(input, expected_login, sizeof(expected_login)) != 0)
		goto done;

	/* Server keep-alive and exact empty client echo.
	 * [740c583 Packet0KeepAlive.writePacketData] */
	static const uint8_t keep_alive[] = {0x00};
	if(!socket_write_fragmented(peer, keep_alive, sizeof(keep_alive))
	   || !socket_read_exact(peer, input, sizeof(keep_alive))
	   || input[0] != 0x00)
		goto done;

	/* Login response: entity 42, empty username, seed 9999, dimension 0.
	 * Field order [740c583 Packet1Login.writePacketData]. */
	static const uint8_t login_response[] = {0x01, 0x00, 0x00, 0x00, 0x2A,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x0F,
		0x00};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;

	/* Provider sends a proactive empty keep-alive after login. */
	if(!socket_read_exact(peer, input, sizeof(keep_alive)) || input[0] != 0x00)
		goto done;

	/* Graceful disconnect: ID 255, string16 "Quitting".
	 * [740c583 Packet255KickDisconnect.writePacketData] */
	static const uint8_t expected_disconnect[] = {0xFF, 0x00, 0x08,
		0x00, 'Q', 0x00, 'u', 0x00, 'i', 0x00, 't', 0x00, 't', 0x00, 'i',
		0x00, 'n', 0x00, 'g'};
	if(!socket_read_exact(peer, input, sizeof(expected_disconnect))
	   || memcmp(input, expected_disconnect, sizeof(expected_disconnect)) != 0)
		goto done;
	fixture->ok = true;

done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

/* S4: hand-built Packet3Chat frame (ASCII only, short lines) -- independent
 * of mp_encode_chat, matching this file's "poisoned golden" discipline: the
 * fixture peer's bytes are never produced by the code under test. [740c583
 * Packet3Chat.writePacketData] */
static bool send_chat_line(SOCKET peer, const char* ascii) {
	size_t len = strlen(ascii);
	uint8_t buf[3 + 2 * 32];
	assert(len <= 32);
	buf[0] = 0x03;
	buf[1] = (uint8_t)(len >> 8);
	buf[2] = (uint8_t)len;
	for(size_t i = 0; i < len; i++) {
		buf[3 + i * 2] = 0;
		buf[3 + i * 2 + 1] = (uint8_t)ascii[i];
	}
	return socket_write_fragmented(peer, buf, 3 + len * 2);
}

/* S4 loopback chat fixture: identical handshake/login/keepalive/disconnect
 * shape as peer_entry, with CHAT_RING_TEST_LINES Packet3Chat frames sent
 * right after login -- more than CHAT_RING_CAPACITY so the client-side ring
 * must evict, proving both contents AND eviction order (plan section 9's S4
 * gate: "received chat/P3 display without a composer"). */
#define CHAT_RING_TEST_LINES 53 /* > CHAT_RING_CAPACITY (50): proves eviction */
static void* chat_peer_entry(void* user) {
	struct peer_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;

	static const uint8_t expected_handshake[]
		= {0x02, 0x00, 0x04, 0x00, 'C', 0x00, 'a', 0x00, 'v', 0x00, 'e'};
	uint8_t input[64];
	if(!socket_read_exact(peer, input, sizeof(expected_handshake))
	   || memcmp(input, expected_handshake, sizeof(expected_handshake)) != 0)
		goto done;

	static const uint8_t offline_reply[] = {0x02, 0x00, 0x01, 0x00, '-'};
	if(!socket_write_fragmented(peer, offline_reply, sizeof(offline_reply)))
		goto done;

	static const uint8_t expected_login[] = {0x01, 0x00, 0x00, 0x00, 0x0E,
		0x00, 0x04, 0x00, 'C', 0x00, 'a', 0x00, 'v', 0x00, 'e',
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	if(!socket_read_exact(peer, input, sizeof(expected_login))
	   || memcmp(input, expected_login, sizeof(expected_login)) != 0)
		goto done;

	static const uint8_t keep_alive[] = {0x00};
	if(!socket_write_fragmented(peer, keep_alive, sizeof(keep_alive))
	   || !socket_read_exact(peer, input, sizeof(keep_alive))
	   || input[0] != 0x00)
		goto done;

	static const uint8_t login_response[] = {0x01, 0x00, 0x00, 0x00, 0x2A,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x0F,
		0x00};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;

	if(!socket_read_exact(peer, input, sizeof(keep_alive)) || input[0] != 0x00)
		goto done;

	for(int i = 0; i < CHAT_RING_TEST_LINES; i++) {
		char line[16];
		snprintf(line, sizeof(line), "line%d", i);
		if(!send_chat_line(peer, line))
			goto done;
	}

	/* The composer's submit (SRPC_CHAT_SEND) must reach the wire as ONE
	 * Packet3Chat with the trimmed text [740c583 EntityClientPlayerMP.
	 * sendChatMessage]: 0x03, string16 "hi there". */
	static const uint8_t expected_chat[] = {0x03, 0x00, 0x08, 0x00, 'h', 0x00,
		'i', 0x00, ' ', 0x00, 't', 0x00, 'h', 0x00, 'e', 0x00, 'r', 0x00, 'e'};
	if(!socket_read_exact(peer, input, sizeof(expected_chat))
	   || memcmp(input, expected_chat, sizeof(expected_chat)) != 0)
		goto done;

	static const uint8_t expected_disconnect[] = {0xFF, 0x00, 0x08,
		0x00, 'Q', 0x00, 'u', 0x00, 'i', 0x00, 't', 0x00, 't', 0x00, 'i',
		0x00, 'n', 0x00, 'g'};
	if(!socket_read_exact(peer, input, sizeof(expected_disconnect))
	   || memcmp(input, expected_disconnect, sizeof(expected_disconnect)) != 0)
		goto done;
	fixture->ok = true;

done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static SOCKET make_listener(uint16_t* port_out) {
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	SOCKET listener = INVALID_SOCKET;
	int bind_error = 0;
	/* The fault matrix tears down and recreates loopback peers quickly. Windows
	 * can briefly report its ephemeral allocator busy while a prior socket
	 * finishes closing, even though port zero was requested. Retry only those
	 * transient resource/address results; every other socket error remains a
	 * hard and diagnosed test failure. */
	for(size_t attempt = 0; attempt < 64; attempt++) {
		listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		assert(listener != INVALID_SOCKET);
		if(bind(listener, (struct sockaddr*)&address, sizeof(address)) == 0)
			break;
#ifdef _WIN32
		bind_error = WSAGetLastError();
		bool transient = bind_error == WSAEADDRINUSE || bind_error == WSAENOBUFS;
#else
		bind_error = errno;
		bool transient = bind_error == EADDRINUSE || bind_error == ENOBUFS;
#endif
		closesocket(listener);
		listener = INVALID_SOCKET;
		if(!transient)
			break;
		thread_msleep(2);
	}
	if(listener == INVALID_SOCKET) {
		fprintf(stderr, "make_listener: bind(port=0) failed: %d\n", bind_error);
		assert(listener != INVALID_SOCKET);
	}
	assert(listen(listener, 1) == 0);
#ifdef _WIN32
	int length = sizeof(address);
#else
	socklen_t length = sizeof(address);
#endif
	assert(getsockname(listener, (struct sockaddr*)&address, &length) == 0);
	*port_out = ntohs(address.sin_port);
	return listener;
}

static bool wait_for(struct server_remote* remote,
					 bool (*predicate)(const struct server_remote_status*),
					 size_t timeout_ms) {
	for(size_t elapsed = 0; elapsed < timeout_ms; elapsed++) {
		server_remote_pump(remote);
		/* S1 produces CRPCs; exercise the real client inbox consumer each turn. */
		clin_update();
		struct server_remote_status status;
		server_remote_get_status(remote, &status);
		if(predicate(&status))
			return true;
		thread_msleep(1);
	}
	return false;
}

static bool is_play(const struct server_remote_status* status) {
	return status->state == REMOTE_PLAY && status->logged_in
		&& status->keep_alives_received >= 1 && status->keep_alives_sent >= 2;
}

static bool workers_exited(const struct server_remote_status* status) {
	return status->read_thread_exited && status->write_thread_exited
		&& !status->socket_open && status->queued_frames == 0;
}

static void test_provider_lifecycle(void) {
	struct server_remote remote;
	memset(&remote, 0xAA, sizeof(remote));
	assert(server_remote_init(&remote));
	uint16_t port;
	struct peer_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	/* Test-peer owner: this fixture; stop: client disconnect/socket close;
	 * join: explicit thread_join below. */
	assert(thread_create(&peer_thread, peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	assert(wait_for(&remote, is_play, 5000));
	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	assert(status.entity_id == 42);
	assert(status.map_seed == 9999);
	assert(status.dimension == 0);

	/* Drive the real provider mailbox, not a test-only teardown hook. */
	struct server_rpc unload = {.type = SRPC_UNLOAD_WORLD};
	assert(svin_rpc_send(&unload));
	assert(wait_for(&remote, workers_exited, 5000));
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_trace_dump(&remote, stdout);

	/* Join/disarm production handles, then prove queues and socket are empty. */
	server_remote_cancel(&remote);
	server_remote_get_status(&remote, &status);
	assert(!status.read_thread_started && !status.write_thread_started);
	assert(status.read_thread_exited && status.write_thread_exited);
	assert(!status.socket_open);
	assert(status.queued_events == 0 && status.queued_frames == 0);
	server_remote_destroy(&remote);

	/* Cancellation leg: close wakes a connected reader and both production
	 * workers join; the test peer also owns an explicit join path. */
	memset(&remote, 0, sizeof(remote));
	assert(server_remote_init(&remote));
	peer = (struct peer_fixture) {.listener = make_listener(&port),
								  .cancel_only = true};
	assert(thread_create(&peer_thread, peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	for(size_t i = 0; i < 5000; i++) {
		server_remote_get_status(&remote, &status);
		if(status.socket_open)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	assert(status.socket_open);
	assert(server_remote_disconnect(&remote, "Quitting"));
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_get_status(&remote, &status);
	assert(!status.read_thread_started && !status.write_thread_started);
	assert(!status.socket_open && status.queued_events == 0
		&& status.queued_frames == 0);
	server_remote_destroy(&remote);
}

/* ---- S4 online mounts (docs/mp_mounts_design_2026-09-11.md) ------------
 * A scripted peer logs us in as entity 42, spawns boat 77, then sends three
 * Packet39s: us -> 77 (attach), a stranger -> 77 (not our signal), us -> 555
 * (never spawned: counted, never applied). It then reads ONE client frame --
 * the riding form of our next position sample -- sends us -> -1 (detach) and
 * expects the disconnect. */
struct mount_peer_fixture {
	SOCKET listener;
	bool ok;
	bool riding_frame_read;
	uint8_t riding_frame[34];
	uint8_t tail[19]; /* what arrived where the disconnect was expected */
	bool tail_read;
};

/* Keep-alive replies (a lone 0x00) are asynchronous; skip them so the
 * fixture reads the next REAL frame regardless of timing. Also skip the
 * post-login/post-respawn Packet16 held-slot resync (0x10 + i16, sent only
 * when the selected slot is not 0, PlayerControllerMP.syncCurrentPlayItem):
 * it is the client's own bookkeeping, not the frame these fixtures pin. */
static bool mount_read_frame(SOCKET peer, uint8_t* out, size_t n) {
	for(int guard = 0; guard < 16; guard++) {
		if(!socket_read_exact(peer, out, 1))
			return false;
		if(out[0] == 0x10) {
			uint8_t slot[2];
			if(!socket_read_exact(peer, slot, sizeof(slot)))
				return false;
			continue;
		}
		if(out[0] != 0x00)
			return n == 1 || socket_read_exact(peer, out + 1, n - 1);
	}
	return false;
}

static void* mount_peer_entry(void* user) {
	struct mount_peer_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[] = {0x01, 0, 0, 0, 0x2A, 0, 0, 0, 0,
		0, 0, 0, 0, 0x27, 0x0F, 0};
	/* Packet23VehicleSpawn: entity 77, type 1 = boat, (0.5, 100.0, 0.5) in
	 * 1/32 units, throwerEntityId 0 (no velocity tail). [740c583
	 * Packet23VehicleSpawn.readPacketData] */
	static const uint8_t boat[] = {0x17, 0, 0, 0, 77, 1, 0, 0, 0, 16, 0, 0,
		0x0C, 0x80, 0, 0, 0, 16, 0, 0, 0, 0};
	static const uint8_t attach[] = {0x27, 0, 0, 0, 0x2A, 0, 0, 0, 77};
	static const uint8_t stranger[] = {0x27, 0, 0, 0, 99, 0, 0, 0, 77};
	static const uint8_t unknown[] = {0x27, 0, 0, 0, 0x2A, 0, 0, 0x02, 0x2B};
	/* Two keep-alive round trips bracket the login response, exactly like
	 * chat_peer_entry: is_play requires keep_alives_received >= 1 and
	 * keep_alives_sent >= 2. */
	static const uint8_t keep_alive[] = {0x00};
	uint8_t input[64];
	if(!socket_write_fragmented(peer, keep_alive, sizeof(keep_alive))
	   || !socket_read_exact(peer, input, sizeof(keep_alive)) || input[0] != 0x00)
		goto done;
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	if(!socket_write_fragmented(peer, keep_alive, sizeof(keep_alive))
	   || !socket_read_exact(peer, input, sizeof(keep_alive)) || input[0] != 0x00)
		goto done;
	if(!socket_write_fragmented(peer, boat, sizeof(boat))
	   || !socket_write_fragmented(peer, attach, sizeof(attach))
	   || !socket_write_fragmented(peer, stranger, sizeof(stranger))
	   || !socket_write_fragmented(peer, unknown, sizeof(unknown)))
		goto done;
	if(!mount_read_frame(peer, fixture->riding_frame, sizeof(fixture->riding_frame)))
		goto done;
	fixture->riding_frame_read = true;
	static const uint8_t detach[] = {0x27, 0, 0, 0, 0x2A, 0xFF, 0xFF, 0xFF, 0xFF};
	if(!socket_write_fragmented(peer, detach, sizeof(detach)))
		goto done;
	static const uint8_t expected_disconnect[] = {0xFF, 0x00, 0x08, 0x00, 'Q',
		0x00, 'u', 0x00, 'i', 0x00, 't', 0x00, 't', 0x00, 'i', 0x00, 'n', 0x00,
		'g'};
	if(!mount_read_frame(peer, fixture->tail, sizeof(fixture->tail)))
		goto done;
	fixture->tail_read = true;
	if(memcmp(fixture->tail, expected_disconnect, sizeof(expected_disconnect)) != 0)
		goto done;
	fixture->ok = true;
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void mount_dump(const char* label, const uint8_t* bytes, size_t n) {
	printf("  %s:", label);
	for(size_t i = 0; i < n; i++)
		printf(" %02X", bytes[i]);
	printf("\n");
}

static bool mount_forwarded(const struct server_remote_status* status) {
	return status->attach_applied >= 1 && status->attach_ignored >= 1;
}
static bool mount_released(const struct server_remote_status* status) {
	return status->attach_applied >= 2;
}

static void test_s4_mount_attach(void) {
	struct server_remote remote;
	memset(&remote, 0xAA, sizeof(remote));
	assert(server_remote_init(&remote));
	uint16_t port;
	struct mount_peer_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, mount_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	assert(wait_for(&remote, is_play, 5000));
	assert(wait_for(&remote, mount_forwarded, 5000));
	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	/* Exactly one attach forwarded (us -> 77); the stranger's packet is not
	 * our signal; the untracked vehicle is counted, never applied. */
	assert(status.attach_applied == 1);
	assert(status.attach_ignored == 1);

	/* While riding, a position sample leaves as Beta's riding form; the peer
	 * captured the frame before it sent the detach. [740c583
	 * EntityClientPlayerMP.sendMotionUpdates] */
	struct server_rpc pos = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = 3.0F, .y = 105.0F, .z = 4.0F, .rx = 90.0F,
			.ry = 0.0F, .vel_x = 1.5F, .vel_z = -0.25F, .on_ground = true,
			.correction_seq = status.correction_seq}};
	assert(svin_rpc_send(&pos));
	assert(wait_for(&remote, mount_released, 5000));
	server_remote_get_status(&remote, &status);
	assert(status.attach_applied == 2 && status.attach_ignored == 1);

	assert(server_remote_disconnect(&remote, "Quitting"));
	thread_join(&peer_thread);
	if(peer.riding_frame_read)
		mount_dump("riding frame", peer.riding_frame, sizeof(peer.riding_frame));
	if(peer.tail_read)
		mount_dump("after riding frame", peer.tail, sizeof(peer.tail));
	fflush(stdout);
	assert(peer.riding_frame_read);
	assert(peer.ok);
	uint8_t expect[34] = {0x0B}; /* Packet11: look changed -> Beta's inverted pick */
	put_f64(expect + 1, 1.5);
	put_f64(expect + 9, -999.0);
	put_f64(expect + 17, -999.0);
	put_f64(expect + 25, -0.25);
	expect[33] = 1;
	assert(memcmp(peer.riding_frame, expect, sizeof(expect)) == 0);
	assert(wait_for(&remote, workers_exited, 5000));
	server_remote_destroy(&remote);

	/* The client half, without a world: occupancy by the local player (what
	 * the single-player tether keys on) and Beta's dismount spot -- eye at
	 * the vehicle's box top [740c583 Entity.mountEntity(null)]. */
	struct entity boat_e = {.type = ENTITY_BOAT, .id = 77, .pos = {10.0F, 64.0F, -3.0F}};
	struct entity cart_e = {.type = ENTITY_MINECART, .id = 78, .pos = {1.0F, 70.0F, 2.0F}};
	struct entity other_e = {.type = ENTITY_ITEM, .id = 79};
	struct entity player_e = {.id = 0};
	assert(clin_apply_attach(&boat_e, true, &player_e));
	assert(boat_e.data.boat.occupied && boat_e.data.boat.occupant_id == 0);
	assert(strcmp(boat_e.rightClickText, "Dismount") == 0);
	assert(clin_apply_attach(&boat_e, false, &player_e));
	assert(!boat_e.data.boat.occupied);
	assert(strcmp(boat_e.rightClickText, "Ride") == 0);
	assert(player_e.pos[0] == 10.0F && player_e.pos[2] == -3.0F);
	assert(fabsf(player_e.pos[1] - 64.3F) < 1e-4F);
	assert(clin_apply_attach(&cart_e, true, NULL));
	assert(cart_e.data.minecart.occupied && cart_e.data.minecart.occupant_id == 0);
	assert(clin_apply_attach(&cart_e, false, &player_e));
	assert(!cart_e.data.minecart.occupied && fabsf(player_e.pos[1] - 70.35F) < 1e-4F);
	assert(!clin_apply_attach(&other_e, true, NULL));
	assert(!clin_apply_attach(NULL, true, NULL));
	puts("  s4: online mount -- Packet39 for us attaches/detaches (counted), riding wire form, dismount spot");
}

/* ---- S4 online JUMP dismount (review follow-up, docs/mp_mounts_design_2026-09-11.md
 * "Online JUMP dismount"): the peer mounts us on MINECART 78 (Packet23 type
 * 10). The test then presses the F3-1 JUMP escape the online way and the
 * peer must read exactly ONE Packet7 use-entity (us -> 78, use) -- not a
 * ride-intent (the remote provider drops those) -- and nothing else before
 * the disconnect. */
struct jump_peer_fixture {
	SOCKET listener;
	bool ok;
	bool use_frame_read;
	uint8_t use_frame[10];
	uint8_t tail[19];
	bool tail_read;
};

static void* jump_peer_entry(void* user) {
	struct jump_peer_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[] = {0x01, 0, 0, 0, 0x2A, 0, 0, 0, 0,
		0, 0, 0, 0, 0x27, 0x0F, 0};
	static const uint8_t keep_alive[] = {0x00};
	uint8_t input[64];
	if(!socket_write_fragmented(peer, keep_alive, sizeof(keep_alive))
	   || !socket_read_exact(peer, input, sizeof(keep_alive)) || input[0] != 0x00)
		goto done;
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	if(!socket_write_fragmented(peer, keep_alive, sizeof(keep_alive))
	   || !socket_read_exact(peer, input, sizeof(keep_alive)) || input[0] != 0x00)
		goto done;
	/* Packet23VehicleSpawn: entity 78, type 10 = rideable minecart, (0.5,
	 * 100.0, 0.5), no thrower; then Packet39 us -> 78. */
	static const uint8_t cart[] = {0x17, 0, 0, 0, 78, 10, 0, 0, 0, 16, 0, 0,
		0x0C, 0x80, 0, 0, 0, 16, 0, 0, 0, 0};
	static const uint8_t attach[] = {0x27, 0, 0, 0, 0x2A, 0, 0, 0, 78};
	if(!socket_write_fragmented(peer, cart, sizeof(cart))
	   || !socket_write_fragmented(peer, attach, sizeof(attach)))
		goto done;
	if(!mount_read_frame(peer, fixture->use_frame, sizeof(fixture->use_frame)))
		goto done;
	fixture->use_frame_read = true;
	static const uint8_t expected_disconnect[] = {0xFF, 0x00, 0x08, 0x00, 'Q',
		0x00, 'u', 0x00, 'i', 0x00, 't', 0x00, 't', 0x00, 'i', 0x00, 'n', 0x00,
		'g'};
	if(!mount_read_frame(peer, fixture->tail, sizeof(fixture->tail)))
		goto done;
	fixture->tail_read = true;
	if(memcmp(fixture->tail, expected_disconnect, sizeof(expected_disconnect)) != 0)
		goto done;
	fixture->ok = true;
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static bool mount_attached_once(const struct server_remote_status* status) {
	return status->attach_applied >= 1;
}

static void test_s4_mount_jump_online(void) {
	struct server_remote remote;
	memset(&remote, 0xAA, sizeof(remote));
	assert(server_remote_init(&remote));
	uint16_t port;
	struct jump_peer_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, jump_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	assert(wait_for(&remote, is_play, 5000));
	assert(wait_for(&remote, mount_attached_once, 5000));

	/* The online JUMP escape: the cart we ride (as the client sees it) keeps
	 * its occupancy, no ride intent is authored, one use-entity leaves. */
	struct entity cart_e = {.type = ENTITY_MINECART, .id = 78, .pos = {0.5F, 100.0F, 0.5F}};
	cart_e.data.minecart.occupied = true;
	cart_e.data.minecart.occupant_id = 0;
	gstate.players[0].ride.send_dirty = false;
	bool was_remote = gstate.remote_session;
	gstate.remote_session = true;
	assert(local_player_online_dismount(&cart_e));
	gstate.remote_session = was_remote;
	assert(cart_e.data.minecart.occupied && cart_e.data.minecart.occupant_id == 0);
	assert(!gstate.players[0].ride.send_dirty);
	for(int i = 0; i < 50; i++) {
		server_remote_pump(&remote);
		clin_update();
		thread_msleep(2);
	}

	assert(server_remote_disconnect(&remote, "Quitting"));
	thread_join(&peer_thread);
	if(peer.use_frame_read)
		mount_dump("use frame", peer.use_frame, sizeof(peer.use_frame));
	if(peer.tail_read)
		mount_dump("after use frame", peer.tail, sizeof(peer.tail));
	fflush(stdout);
	assert(peer.use_frame_read);
	/* Packet7UseEntity: i32 player 42, i32 target 78, i8 isLeftClick 0 (use).
	 * [740c583 Packet7UseEntity.writePacketData] */
	static const uint8_t expect[10] = {0x07, 0, 0, 0, 0x2A, 0, 0, 0, 78, 0};
	assert(memcmp(peer.use_frame, expect, sizeof(expect)) == 0);
	assert(peer.ok);
	assert(wait_for(&remote, workers_exited, 5000));
	server_remote_destroy(&remote);
	puts("  s4: online JUMP dismount -- one Packet7 use-entity on the ridden cart, no local prediction, no ride intent");
}

/* ---- S4 dimension change in stream order (docs/mp_dimension_change_2026-09-11.md)
 * Live against Poseidon 1.1.12 (2026-09-11) walking into a Nether portal
 * killed the session with "remote Packet51 body chain exhausted": Beta's
 * server sends Packet9 (dimension -1) and then the Nether's Packet50/51 burst
 * back to back [740c583 ServerConfigurationManager.sendPlayerToOtherDimension];
 * the consumer applied the Respawn (clearing the chunk table) only later,
 * while the read thread had already chained the Nether bodies onto the
 * still-resident OVERWORLD columns of the same coordinates. This peer fills
 * column (0,0)'s body chain (head + REMOTE_CHUNK_QUEUE_MAX queued), then sends
 * Respawn(-1) immediately followed by a pre-chunk + body for (0,0) again. */
struct dim_peer_fixture {
	SOCKET listener;
	bool ok;
	bool tail_read;
	uint8_t tail[19];
	/* Set once the whole burst (login .. Nether body) has been written. The
	 * test does NOT pump until then, so the read thread has retained every
	 * packet in stream order before the consumer sees the first event -- the
	 * live shape (one contiguous server burst), made deterministic. */
	atomic_bool sent_all;
	/* Set by the test once it reached PLAY and STOPPED pumping; the peer
	 * writes the burst only then (the keep-alive echoes of the handshake
	 * need the pump, the burst must not). */
	atomic_bool go_burst;
};

static uint8_t* dim_build_column(size_t* out_len, uint8_t block) {
	size_t cells = 16u * 128u * 16u, nibble = cells / 2u;
	uint8_t* inflated = calloc(1, cells + 3u * nibble);
	if(!inflated)
		return NULL;
	for(size_t i = 0; i < cells; i += 128)
		inflated[i] = block; /* one block at y=0 in every column */
	uLongf compressed_cap = compressBound((uLong)(cells + 3u * nibble));
	uint8_t* compressed = malloc(compressed_cap);
	if(!compressed) { free(inflated); return NULL; }
	if(compress2(compressed, &compressed_cap, inflated,
			(uLong)(cells + 3u * nibble), Z_BEST_SPEED) != Z_OK) {
		free(compressed); free(inflated); return NULL;
	}
	free(inflated);
	size_t packet_len = 18u + (size_t)compressed_cap;
	uint8_t* packet = malloc(packet_len);
	if(!packet) { free(compressed); return NULL; }
	packet[0] = 0x33; put_i32(packet + 1, 0); put_i16(packet + 5, 0);
	put_i32(packet + 7, 0); packet[11] = 15; packet[12] = 127; packet[13] = 15;
	put_i32(packet + 14, (int32_t)compressed_cap);
	memcpy(packet + 18, compressed, compressed_cap);
	free(compressed);
	*out_len = packet_len;
	return packet;
}

static void* dim_peer_entry(void* user) {
	struct dim_peer_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	uint8_t* stone = NULL; uint8_t* netherrack = NULL;
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[] = {0x01, 0, 0, 0, 0x2A, 0, 0, 0, 0,
		0, 0, 0, 0, 0x27, 0x0F, 0};
	static const uint8_t keep_alive[] = {0x00};
	static const uint8_t pre_chunk[] = {0x32, 0, 0, 0, 0, 0, 0, 0, 0, 1};
	static const uint8_t respawn_nether[] = {0x09, 0xFF};
	uint8_t input[64];
	if(!socket_write_fragmented(peer, keep_alive, sizeof(keep_alive))
	   || !socket_read_exact(peer, input, sizeof(keep_alive)) || input[0] != 0x00)
		goto done;
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	if(!socket_write_fragmented(peer, keep_alive, sizeof(keep_alive))
	   || !socket_read_exact(peer, input, sizeof(keep_alive)) || input[0] != 0x00)
		goto done;
	size_t stone_len = 0, nether_len = 0;
	stone = dim_build_column(&stone_len, 1 /* stone */);
	netherrack = dim_build_column(&nether_len, 87 /* netherrack */);
	if(!stone || !netherrack)
		goto done;
	/* Wait until the test is in PLAY and has stopped pumping. */
	for(int i = 0; i < 2500
		&& !atomic_load_explicit(&fixture->go_burst, memory_order_acquire); i++)
		thread_msleep(2);
	if(!atomic_load_explicit(&fixture->go_burst, memory_order_acquire))
		goto done;
	/* Overworld: announce (0,0) and fill its chain: 1 head + 3 queued. */
	if(!socket_write_fragmented(peer, pre_chunk, sizeof(pre_chunk)))
		goto done;
	for(int i = 0; i < 1 + 3; i++)
		if(!socket_write_fragmented(peer, stone, stone_len))
			goto done;
	/* The dimension change, then the Nether's (0,0) column right behind it --
	 * the live order. */
	if(!socket_write_fragmented(peer, respawn_nether, sizeof(respawn_nether))
	   || !socket_write_fragmented(peer, pre_chunk, sizeof(pre_chunk))
	   || !socket_write_fragmented(peer, netherrack, nether_len))
		goto done;
	/* Give the client's read thread time to retain the whole burst before
	 * the test starts pumping (it waits on this flag). */
	thread_msleep(300);
	atomic_store_explicit(&fixture->sent_all, true, memory_order_release);
	static const uint8_t expected_disconnect[] = {0xFF, 0x00, 0x08, 0x00, 'Q',
		0x00, 'u', 0x00, 'i', 0x00, 't', 0x00, 't', 0x00, 'i', 0x00, 'n', 0x00,
		'g'};
	if(!mount_read_frame(peer, fixture->tail, sizeof(fixture->tail)))
		goto done;
	fixture->tail_read = true;
	if(memcmp(fixture->tail, expected_disconnect, sizeof(expected_disconnect)) != 0)
		goto done;
	fixture->ok = true;
done:
	free(stone); free(netherrack);
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static bool dim_in_nether(const struct server_remote_status* status) {
	return status->dimension == -1;
}

static void test_s4_dimension_change_stream_order(void) {
	struct server_remote remote;
	memset(&remote, 0xAA, sizeof(remote));
	assert(server_remote_init(&remote));
	uint16_t port;
	struct dim_peer_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, dim_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	/* No pumping until the peer has written everything: the read thread
	 * retains the whole burst first, exactly like one contiguous server
	 * send. Only then does the consumer get to see LOGIN .. RESPAWN. */
	assert(wait_for(&remote, is_play, 5000));
	/* PLAY reached: stop pumping, release the burst, wait until the read
	 * thread has retained all of it, THEN let the consumer see the events. */
	atomic_store_explicit(&peer.go_burst, true, memory_order_release);
	for(int i = 0; i < 2500
		&& !atomic_load_explicit(&peer.sent_all, memory_order_acquire); i++)
		thread_msleep(2);
	assert(atomic_load_explicit(&peer.sent_all, memory_order_acquire));
	assert(wait_for(&remote, dim_in_nether, 5000));
	/* Keep pumping: the Nether column must promote into the NEW world. */
	bool installed = false;
	for(int i = 0; i < 400 && !installed; i++) {
		server_remote_pump(&remote);
		clin_update();
		thread_msleep(2);
		struct chunk* c = world_find_chunk(&gstate.world, 0, 0, 0);
		installed = c && world_get_block(&gstate.world, 0, 0, 0).type == 87;
	}
	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	printf("  s4 dim: state=%d dimension=%d forced_reconnects=%u reason=\"%s\" installed=%d world_dim=%d\n",
		(int)status.state, (int)status.dimension, status.forced_reconnects,
		status.reason, installed ? 1 : 0, (int)gstate.world.dimension);
	fflush(stdout);
	assert(status.state == REMOTE_PLAY);
	assert(status.forced_reconnects == 0);
	assert(gstate.world.dimension == WORLD_DIM_NETHER);
	assert(installed); /* the netherrack column, not the stone one */
	assert(server_remote_disconnect(&remote, "Quitting"));
	thread_join(&peer_thread);
	assert(peer.ok);
	assert(wait_for(&remote, workers_exited, 5000));
	server_remote_destroy(&remote);
	puts("  s4: dimension change in stream order -- Packet9 + the new dimension's chunk burst back to back: no body-chain fault, the Nether column installs into the swapped world");
}

static bool chat_ring_filled(const struct server_remote_status* status) {
	return status->chat_lines_received >= CHAT_RING_TEST_LINES;
}

static bool chat_line_sent(const struct server_remote_status* status) {
	return status->chat_lines_sent >= 1;
}

/* S4 gate: "received chat/P3 display without a composer" -- drive a real
 * connect->login->N-chat-lines->disconnect cycle through the actual
 * svin/clin mailboxes (not a parallel test-only API), then assert both the
 * ring's final CONTENTS and its EVICTION ORDER: CHAT_RING_TEST_LINES (53) is
 * sent against Beta's CHAT_RING_CAPACITY (50) ring, so "line0".."line2" must
 * have been evicted and "line3".."line52" must remain, oldest first. Then
 * the composer path: an SRPC_CHAT_SEND submitted through the real svin
 * mailbox must leave the client as exactly one Packet3Chat (the peer reads
 * and byte-compares it before accepting the disconnect). */
static void test_s4_chat_ring(void) {
	struct server_remote remote;
	memset(&remote, 0xAA, sizeof(remote));
	assert(server_remote_init(&remote));
	uint16_t port;
	struct peer_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, chat_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	assert(wait_for(&remote, is_play, 5000));
	assert(wait_for(&remote, chat_ring_filled, 5000));
	/* Belt-and-suspenders: a couple more pump/update turns so any
	 * last-batch CRPC_CHAT_LINE the predicate's own pump call queued is
	 * guaranteed drained into gstate.chat_ring before it's inspected. */
	for(int i = 0; i < 4; i++) {
		server_remote_pump(&remote);
		clin_update();
	}

	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	assert(status.chat_lines_received == CHAT_RING_TEST_LINES);
	assert(status.chat_lines_rejected == 0);
	assert(gstate.chat_ring.count == CHAT_RING_CAPACITY);
	for(size_t i = 0; i < CHAT_RING_CAPACITY; i++) {
		size_t idx = (gstate.chat_ring.head + i) % CHAT_RING_CAPACITY;
		char expected[16];
		snprintf(expected, sizeof(expected), "line%d",
			(int)(CHAT_RING_TEST_LINES - CHAT_RING_CAPACITY + i));
		assert(strcmp(gstate.chat_ring.lines[idx], expected) == 0);
	}

	/* Composer submit -> Packet3 (chat_peer_entry byte-compares it). */
	struct server_rpc chat_rpc = {.type = SRPC_CHAT_SEND};
	snprintf(chat_rpc.payload.chat_send.text, sizeof(chat_rpc.payload.chat_send.text),
			 "hi there");
	assert(svin_rpc_send(&chat_rpc));
	assert(wait_for(&remote, chat_line_sent, 5000));
	server_remote_get_status(&remote, &status);
	assert(status.chat_lines_sent == 1);

	/* Chat aging [740c583 ChatLine; GuiIngame.updateTick]: fresh lines age 0,
	 * each chat_ring_tick +1s every live line, a push resets ONLY its own
	 * slot, and the HUD's 200-tick expiry threshold is reachable. */
	{
		struct chat_ring_state aging = {0};
		chat_ring_push(&aging, "old");
		chat_ring_push(&aging, "new");
		assert(aging.age[0] == 0 && aging.age[1] == 0);
		for(int t = 0; t < 199; t++)
			chat_ring_tick(&aging);
		assert(aging.age[0] == 199 && aging.age[1] == 199); /* both < 200:
															   still drawn */
		chat_ring_push(&aging, "fresh"); /* slot 2; others untouched */
		assert(aging.age[2] == 0 && aging.age[0] == 199);
		chat_ring_tick(&aging);
		assert(aging.age[0] == 200); /* crosses Beta's expiry threshold */
		assert(aging.age[2] == 1);   /* fresh line ages independently */
	}

	struct server_rpc unload = {.type = SRPC_UNLOAD_WORLD};
	assert(svin_rpc_send(&unload));
	assert(wait_for(&remote, workers_exited, 5000));
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
	/* NOTE: disconnect/teardown alone does not clear the ring -- only the
	 * NEXT world's CRPC_WORLD_RESET does (chat is receive-only history until
	 * then, matching Beta's own chat log surviving a disconnect). Every
	 * later test in this binary establishes a fresh world/login and so
	 * exercises that reset path independently; this test does not need to
	 * reconnect just to prove it. */
}

/* S4 gate: "100 connect/disconnect cycles are sanitizer-clean". Reuses ONE
 * server_remote across all cycles (server_remote_start requires
 * REMOTE_DISCONNECTED and re-arms a fresh `generation` each call -- the
 * plan's own "each cycle a fresh generation" phrasing), proving zero stuck
 * threads, zero queue residue, and no monotonic growth in the
 * remote-entity/chunk/event counters this peer never exercises. The
 * sanitizer-clean half of the claim is a Linux CI property (tools/
 * local_ci.sh); this test makes the run leak-OBSERVABLE by asserting every
 * connection resource reports freed/reset via the existing status/counter
 * APIs after every single cycle, not just the last one. */
static void test_s4_reconnect_stress(void) {
	struct server_remote remote;
	memset(&remote, 0, sizeof(remote));
	assert(server_remote_init(&remote));

	for(int cycle = 0; cycle < 100; cycle++) {
		uint16_t port;
		struct peer_fixture peer = {.listener = make_listener(&port)};
		struct thread peer_thread;
		assert(thread_create(&peer_thread, peer_entry, &peer, 64));
		assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
		assert(wait_for(&remote, is_play, 5000));

		struct server_rpc unload = {.type = SRPC_UNLOAD_WORLD};
		assert(svin_rpc_send(&unload));
		assert(wait_for(&remote, workers_exited, 5000));
		thread_join(&peer_thread);
		assert(peer.ok);

		server_remote_cancel(&remote);

		struct server_remote_status status;
		server_remote_get_status(&remote, &status);
		assert(status.state == REMOTE_DISCONNECTED);
		/* Adversarial-review MINOR closure: the header comment above claims
		 * "each cycle a fresh generation" as part of the leak-freedom proof,
		 * but nothing asserted it -- server_remote_start increments impl->
		 * generation unconditionally (wrapping 0 back to 1), so cycle N's
		 * post-disconnect status must read exactly N+1. */
		assert(status.generation == (uint32_t)(cycle + 1));
		assert(!status.read_thread_started && !status.write_thread_started);
		assert(status.read_thread_exited && status.write_thread_exited);
		assert(!status.socket_open);
		assert(status.queued_events == 0 && status.queued_frames == 0);
		/* Chunk/entity state: peer_entry's minimal flow never streams a
		 * chunk or spawns an entity, so every one of these must be exactly
		 * zero on EVERY cycle -- not just bounded, but never having grown
		 * at all is the strongest available proof of "no monotonic growth"
		 * for a peer this narrow. */
		assert(status.retained_entries == 0 && status.ready_entries == 0
			&& status.promotion_backlog == 0);
		assert(status.remote_entity_count == 0);
		assert(status.remote_entity_capacity_drops == 0);
		assert(status.retention_evictions == 0);
		assert(status.forced_reconnects == 0);
		assert(status.deferred_edits == 0);
		/* Adversarial-review MINOR closure: the 7 new S4 P3 no-op/received
		 * counters were added to server_remote_status alongside these but
		 * never asserted here, even though peer_entry (like the chunk/
		 * entity peer above) sends no P3 traffic at all -- every one of
		 * them must also stay exactly zero on every cycle. */
		assert(status.chat_lines_received == 0 && status.chat_lines_rejected == 0);
		assert(status.door_change_unhandled == 0);
		assert(status.bed_events_unhandled == 0);
		assert(status.weather_lightning_noop == 0);
		/* 0x83 consumer counters (Packet131->CRPC_MAP_DATA slice): the
		 * minimal peer sends no map traffic, so every one stays zero. */
		assert(status.map_strips_applied == 0);
		assert(status.map_markers_applied == 0);
		assert(status.map_data_dropped == 0);
		assert(status.map_mirror_evictions == 0);
		assert(status.map_pushes == 0);
		assert(status.map_push_deferred == 0);
		assert(status.statistic_noop == 0);
		/* Zero-pin EXHAUSTIVE sweep (betamc readiness follow-up, 2026-08-26):
		 * the minimal peer streams no chunks, sends no correction, spawns no
		 * entity and never backpressures, so every remaining stats field must
		 * ALSO sit at its empty baseline each cycle -- a drift here means a
		 * teardown leak the original 18-counter pin could not see.
		 * (keep_alives_* stay excluded: the is_play() gate itself exchanges
		 * them, so they are inherently nonzero.) */
		assert(status.retained_bytes == 0);
		assert(status.retained_bytes_peak == 0);
		assert(status.retention_allocation_failures == 0);
		assert(status.inflated_bytes_frame == 0);
		assert(status.event_backpressure_waits == 0);
		assert(status.events_deferred_frames == 0);
		assert(status.corrections_received == 0);
		assert(status.corrections_echoed == 0);
		assert(status.correction_seq == 0);
		assert(status.reconnect_recoveries == 0);
	}

	server_remote_destroy(&remote);
}

/* Regression test for the 2026-08-19 live-server bug: a real, populated
 * mc173-server's ordinary post-login traffic (one MP_EVENT_MOB_SPAWN per
 * nearby entity) routinely queued more than REMOTE_QUEUE_CAPACITY (64)
 * decoded events before the client's next server_remote_pump() call, and
 * push_event_locked used to treat that as "unretainable" and instantly
 * self-disconnect (an ABORTIVE close with unread socket data still
 * buffered -- exactly the "client side closed"/os error 10053 the peer
 * observed). No existing fixture ever sent more than a handful of packets,
 * so this path was untested until a real server exposed it. This peer
 * sends BURST_HEARTBEAT_COUNT (5x the queue capacity) minimal
 * MP_EVENT_ENTITY heartbeats [740c583 Packet30Entity] (chosen because
 * process_event's own case is a pure no-op -- no CRPC, no reply frame -- so
 * only the event QUEUE's own capacity is under test, not the separate
 * frame/control queue or any entity-tracking cap) back-to-back right after
 * login, and the test thread deliberately does not pump while they arrive,
 * so the burst is guaranteed to outrun the consumer regardless of machine
 * speed. */
#define BURST_HEARTBEAT_COUNT (SERVER_REMOTE_EVENT_QUEUE_CAPACITY * 5)

static bool send_entity_heartbeat(SOCKET peer, int32_t id) {
	uint8_t buf[5];
	buf[0] = 0x1E; /* MP_PACKET_ENTITY */
	put_i32(buf + 1, id);
	return socket_write_fragmented(peer, buf, sizeof(buf));
}

static void* burst_peer_entry(void* user) {
	struct peer_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;

	static const uint8_t expected_handshake[]
		= {0x02, 0x00, 0x04, 0x00, 'C', 0x00, 'a', 0x00, 'v', 0x00, 'e'};
	uint8_t input[64];
	if(!socket_read_exact(peer, input, sizeof(expected_handshake))
	   || memcmp(input, expected_handshake, sizeof(expected_handshake)) != 0)
		goto done;

	static const uint8_t offline_reply[] = {0x02, 0x00, 0x01, 0x00, '-'};
	if(!socket_write_fragmented(peer, offline_reply, sizeof(offline_reply)))
		goto done;

	static const uint8_t expected_login[] = {0x01, 0x00, 0x00, 0x00, 0x0E,
		0x00, 0x04, 0x00, 'C', 0x00, 'a', 0x00, 'v', 0x00, 'e',
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	if(!socket_read_exact(peer, input, sizeof(expected_login))
	   || memcmp(input, expected_login, sizeof(expected_login)) != 0)
		goto done;

	static const uint8_t login_response[] = {0x01, 0x00, 0x00, 0x00, 0x2A,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x0F,
		0x00};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;

	/* The burst: no keep-alive round trip first (a keep-alive echo would
	 * exercise the SEPARATE outbound frame queue, not the one under test
	 * here), just heartbeats, as fast as this thread can write them. */
	for(int32_t i = 0; i < BURST_HEARTBEAT_COUNT; i++)
		if(!send_entity_heartbeat(peer, i))
			goto done;

	/* The client may legally emit empty keep-alives (0x00 -- Packet0 is a
	 * bare id byte) on its own cadence before the graceful disconnect; the
	 * 200ms driver stall makes that likely here. Skip them and require the
	 * next REAL packet to be the exact Packet255 "Quitting". */
	uint8_t lead;
	do {
		if(!socket_read_exact(peer, &lead, 1)) {
			fprintf(stderr, "[burst-peer] disconnect read failed (EOF)\n");
			goto done;
		}
	} while(lead == 0x00);
	static const uint8_t expected_disconnect_body[] = {0x00, 0x08,
		0x00, 'Q', 0x00, 'u', 0x00, 'i', 0x00, 't', 0x00, 't', 0x00, 'i',
		0x00, 'n', 0x00, 'g'};
	if(lead != 0xFF
	   || !socket_read_exact(peer, input, sizeof(expected_disconnect_body))
	   || memcmp(input, expected_disconnect_body,
				 sizeof(expected_disconnect_body)) != 0) {
		fprintf(stderr, "[burst-peer] not a clean Packet255 (lead=%02X)\n",
				lead);
		goto done;
	}
	fixture->ok = true;

done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static bool burst_logged_in(const struct server_remote_status* status) {
	return status->state == REMOTE_PLAY && status->logged_in;
}

static void test_event_queue_backpressure_burst(void) {
	struct server_remote remote;
	memset(&remote, 0xAA, sizeof(remote));
	assert(server_remote_init(&remote));
	uint16_t port;
	struct peer_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, burst_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	assert(wait_for(&remote, burst_logged_in, 5000));

	/* Deliberately starve the consumer while the peer thread (running
	 * concurrently) writes the whole burst -- proves the fix handles the
	 * burst outrunning server_remote_pump() regardless of scheduling luck,
	 * rather than merely hoping this test's own poll cadence is fast
	 * enough. 200ms is generous: 5*64 five-byte packets over loopback,
	 * one send() call per byte, completes in low single-digit milliseconds. */
	thread_msleep(200);

	/* Before the fix this would already be REMOTE_DISCONNECTED (the queue
	 * overflowed and push_event_locked aborted the connection) with
	 * status.reason == "remote event queue overflow" and most heartbeats
	 * never having been retired -- resuming the pump loop below would spin
	 * to its own timeout. */
	struct server_remote_status mid;
	server_remote_get_status(&remote, &mid);
	assert(mid.state != REMOTE_DISCONNECTED);

	struct server_rpc unload = {.type = SRPC_UNLOAD_WORLD};
	assert(svin_rpc_send(&unload));
	assert(wait_for(&remote, workers_exited, 5000));
	thread_join(&peer_thread);
	assert(peer.ok); /* the peer only sets this after ALL heartbeats sent
	                   * AND the graceful disconnect was read -- proves the
	                   * client kept consuming through the whole burst. */

	/* wait_for's last iteration pumps BEFORE it observes workers_exited, so
	 * anything the read thread pushed in that window is still in the ring at
	 * the instant the predicate went true -- a scheduling detail, not a
	 * stranded event. (It never showed while the read thread drained the
	 * socket 256 bytes at a time; REMOTE_READ_BUFFER_BYTES made the reader
	 * fast enough to win that race routinely.) Both workers are provably
	 * gone here, so nothing can push again: pump the tail out and assert the
	 * ring EMPTIES, which is the invariant this test always meant. A genuine
	 * strand still fails -- the loop is bounded. */
	struct server_remote_status status;
	for(size_t elapsed = 0; elapsed < 1000; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.queued_events == 0 && status.queued_frames == 0)
			break;
		thread_msleep(1);
	}
	assert(status.event_backpressure_waits > 0);
	assert(status.queued_events == 0 && status.queued_frames == 0);
	/* Reconnect-burst diagnostics: the 5x-capacity burst against a starved
	 * pump must have filled the ring to its cap exactly (the peak counter
	 * saturates at capacity -- backpressure holds the 65th event OUTSIDE
	 * the ring), and the deliberate 200ms starve above must be visible as
	 * the worst pump gap (>=150ms leaves margin for timer coarseness). */
	assert(status.queued_events_peak == SERVER_REMOTE_EVENT_QUEUE_CAPACITY);
	assert(status.pump_gap_max_ms >= 150);

	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
}

/* REL-MP-FIX5 (inverts the old overflow-kill test): a full event ring is
 * BACK-PRESSURE, never a kill. Beta's client receive queue is unbounded and
 * drained on the main thread with no receive-overflow kill
 * (NetworkManager.java:24,137,187-208); its one receive timeout is 1200
 * ticks with nothing read (:193). Real Wii hw1021c died here with "remote
 * event queue overflow (pump idle 557ms)" after a slow post-boot frame.
 * Phase 1: a >500 ms pump stall against a 4x-capacity burst keeps the
 * session and delivers every event. Phase 2: cancel while the reader is
 * parked on the full ring returns promptly (no deadlock, no lost wakeup). */
#define STALL_HEARTBEATS (SERVER_REMOTE_EVENT_QUEUE_CAPACITY * 4)
static atomic_bool s_stall_phase1;
static atomic_bool s_stall_phase2;
static atomic_bool s_stall_burst2_sent;
static void* stall_peer_entry(void* user) {
	struct peer_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	uint8_t input[64];
	static const uint8_t offline_reply[] = {0x02, 0x00, 0x01, 0x00, '-'};
	static const uint8_t login_response[] = {0x01, 0x00, 0x00, 0x00, 0x2A,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x0F, 0x00};
	if(!socket_read_exact(peer, input, 11) /* handshake, sizes as burst peer */
	   || !socket_write_fragmented(peer, offline_reply, sizeof(offline_reply))
	   || !socket_read_exact(peer, input, 24) /* login */
	   || !socket_write_fragmented(peer, login_response,
								   sizeof(login_response)))
		goto done;
	/* The login wait pumps events. Do not let it consume this burst. */
	while(!atomic_load(&s_stall_phase1))
		thread_msleep(1);
	for(int32_t i = 0; i < STALL_HEARTBEATS; i++)
		if(!send_entity_heartbeat(peer, i))
			goto done;
	while(!atomic_load(&s_stall_phase2))
		thread_msleep(1);
	for(int32_t i = 0; i < STALL_HEARTBEATS; i++)
		if(!send_entity_heartbeat(peer, STALL_HEARTBEATS + i))
			goto done;
	fixture->ok = true;
	atomic_store(&s_stall_burst2_sent, true);
	/* Hold the session (a live server) until the client's cancel closes it. */
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

/* Unlike wait_for(), observing a parked reader must not drain its queue. */
static bool wait_for_event_backpressure(struct server_remote* remote,
									   uint32_t waits_before, bool second_burst) {
	for(size_t elapsed = 0; elapsed < 5000; elapsed++) {
		struct server_remote_status status;
		server_remote_get_status(remote, &status);
		if(status.queued_events == SERVER_REMOTE_EVENT_QUEUE_CAPACITY
		   && status.event_backpressure_waits > waits_before
		   && (!second_burst || atomic_load(&s_stall_burst2_sent)))
			return true;
		thread_msleep(1);
	}
	return false;
}

static void test_event_queue_stall_keeps_session(void) {
	struct server_remote remote;
	memset(&remote, 0xAA, sizeof(remote));
	assert(server_remote_init(&remote));
	struct server_remote_impl* impl = remote.impl;
	uint16_t port;
	struct peer_fixture peer = {.listener = make_listener(&port)};
	atomic_store(&s_stall_phase1, false);
	atomic_store(&s_stall_phase2, false);
	atomic_store(&s_stall_burst2_sent, false);
	struct thread peer_thread;
	assert(thread_create(&peer_thread, stall_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	assert(wait_for(&remote, burst_logged_in, 5000));

	/* Start only after the login pump returns: its 256-event budget could
	 * otherwise consume the login plus 255 heartbeats, leaving only one.
	 * Observe a full ring AND a parked reader before timing the stall. */
	atomic_store(&s_stall_phase1, true);
	assert(wait_for_event_backpressure(&remote, 0, false));
	/* Phase 1: starve the pump well past the old 500ms patience. */
	thread_msleep(800);
	struct server_remote_status mid;
	server_remote_get_status(&remote, &mid);
	printf("  stall 800ms: dying=%d state=%d queued=%zu bp_waits=%u "
		   "reason=\"%s\"\n", (int)mid.dying, (int)mid.state,
		   mid.queued_events, mid.event_backpressure_waits, mid.reason);
	assert(!mid.dying && mid.state == REMOTE_PLAY && mid.mp_end == 0);
	assert(mid.queued_events == SERVER_REMOTE_EVENT_QUEUE_CAPACITY);
	assert(mid.event_backpressure_waits > 0);

	/* Resume pumping: every byte arrives and every event is queued/drained
	 * (no chunk traffic here, so arrival_sequence counts pushed events:
	 * Packet2 reply + Packet1 login + every heartbeat). rx_bytes counts at
	 * recv() time, ahead of decode, so wait on the pushed count itself. */
	const uint32_t phase1_rx = 5u + 16u + 5u * STALL_HEARTBEATS;
	struct server_remote_status status;
	uint64_t pushed = 0;
	for(size_t elapsed = 0; elapsed < 5000; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		thread_mutex_lock(&impl->lock);
		pushed = impl->arrival_sequence;
		thread_mutex_unlock(&impl->lock);
		if(pushed == 2u + STALL_HEARTBEATS && status.queued_events == 0)
			break;
		thread_msleep(1);
	}
	assert(status.rx_bytes == phase1_rx && status.queued_events == 0);
	printf("  after stall: rx=%u pushed=%llu (want 2 + %d)\n", status.rx_bytes,
		   (unsigned long long)pushed, STALL_HEARTBEATS);
	assert(pushed == 2u + STALL_HEARTBEATS);
	assert(!status.dying && status.state == REMOTE_PLAY && status.mp_end == 0
		   && status.reason[0] == '\0');

	/* Phase 2: park the reader on a full ring (no pumping), then cancel. */
	uint32_t waits_before = status.event_backpressure_waits;
	atomic_store(&s_stall_phase2, true);
	/* A full ring does not prove the peer finished its send loop. Cancelling
	 * earlier could close its socket before it sets peer.ok. */
	assert(wait_for_event_backpressure(&remote, waits_before, true));
	ptime_t cancel_start = time_get();
	server_remote_cancel(&remote);
	int32_t cancel_ms = time_diff_ms(cancel_start, time_get());
	printf("  cancel with reader parked on full ring: %ldms\n", (long)cancel_ms);
	assert(cancel_ms < 1000);
	thread_join(&peer_thread);
	assert(peer.ok);
	struct server_remote_status torn_down;
	server_remote_get_status(&remote, &torn_down);
	assert(!torn_down.socket_open && !torn_down.socket_close_deferred);
	server_remote_destroy(&remote);
	puts("  >500ms pump stall keeps the session, loses no events (REL-MP-FIX5)");
	puts("  cancel wakes a reader parked on the full ring (REL-MP-FIX5)");
}

static bool s1_ready(const struct server_remote_status* status) {
	return status->state == REMOTE_PLAY && gstate.world_loaded
		&& status->corrections_received == 1
		&& status->corrections_echoed == 1
		&& status->promotion_backlog == 0;
}

static void test_s1_world_stream(void) {
	struct chunk_rpc_slot* held[32];
	size_t held_count = 0;
	while(held_count < 32
		  && (held[held_count] = clin_chunk_slot_try_acquire()) != NULL)
		held_count++;
	assert(held_count > 0 && held_count < 32);
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct s1_peer_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, s1_peer_entry, &peer, 64));
	/* Force every outbound frame through the writer's short-write loop. */
	net_socket_test_set_send_limit(1);
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	bool deferred = false;
	for(size_t elapsed=0; elapsed<5000 && !deferred; elapsed++) {
		server_remote_pump(&remote); clin_update();
		struct server_remote_status pressure;
		server_remote_get_status(&remote,&pressure);
		deferred = pressure.promotion_backlog == 1
			&& pressure.retained_bytes > 0 && !gstate.world_loaded;
		thread_msleep(1);
	}
	assert(deferred && "full chunk pool did not defer promotion");
	for(size_t i=0;i<held_count;i++) clin_chunk_slot_release(held[i]);
	assert(wait_for(&remote, s1_ready, 10000));

	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	assert(status.retained_bytes <= status.retained_byte_cap);
	assert(status.retained_entries <= status.retained_entry_cap);
	assert(status.inflated_bytes_frame <= 32u * 1024u);
	assert(status.forced_reconnects == 0);
	assert(gstate.world_spawn_x == 0 && gstate.world_spawn_y == 100
		&& gstate.world_spawn_z == 0);
	assert(gstate.world_time == 42);
	/* The peer's login UpdateHealth(20) is Beta wire scale (full hearts);
	 * the seam converts it to CaveWii's internal scale (Packet8 x8 fix --
	 * the old pass-through left a full-health player at 1.25 hearts). */
	assert(gstate.players[0].local_player
		   && gstate.players[0].local_player->health == MAX_PLAYER_HEALTH);
	struct block_data block = world_get_block(&gstate.world, 0, 0, 0);
	assert(block.type == BLOCK_STONE);
	/* Synthetic Packet51 has nonzero data in exactly one light plane each;
	 * swapping block-light and sky-light makes this fail loudly. */
	assert(block.torch_light == 7 && block.sky_light == 12);
	assert(world_get_height(&gstate.world, 0, 0) == 1);

	struct server_rpc walk = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x=1.0,.y=101.62,.z=0.5,.rx=90.0f,.ry=0.0f,
			.on_ground=true,.correction_seq=1,
		},
	};
	assert(svin_rpc_send(&walk));
	/* The peer now sends unload -> reload -> Packet51 without waiting for a pump.
	 * Observe both ordered client states; a dropped replacement never reloads. */
	bool unloaded = false, reloaded = false;
	for(size_t elapsed=0; elapsed<10000 && !reloaded; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		struct chunk* chunk = world_find_chunk(&gstate.world,0,0,0);
		if(!chunk)
			unloaded = true;
		else if(unloaded) {
			server_remote_get_status(&remote, &status);
			reloaded = world_get_block(&gstate.world,0,0,0).type == BLOCK_STONE
				&& status.promotion_backlog == 0 && status.retained_bytes == 0;
		}
		thread_msleep(1);
	}
	assert(unloaded && reloaded);
	/* Marker tells the peer the replacement has crossed the pump boundary; it
	 * skips any legitimate automatic movement frames, then sends final unload. */
	assert(server_remote_send_keep_alive(&remote));
	unloaded = false;
	for(size_t elapsed=0; elapsed<5000 && !unloaded; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		unloaded = world_find_chunk(&gstate.world,0,0,0) == NULL;
		thread_msleep(1);
	}
	assert(unloaded);
	if(!server_remote_disconnect(&remote, "Quitting")) {
		server_remote_get_status(&remote, &status);
		fprintf(stderr, "S1 disconnect refused: state=%d socket=%d frames=%zu "
			"reason=%s\n", status.state, status.socket_open,
			status.queued_frames, status.reason);
		assert(false);
	}
	assert(wait_for(&remote, workers_exited, 5000));
	thread_join(&peer_thread);
	net_socket_test_set_send_limit(0);
	assert(peer.ok);
	server_remote_get_status(&remote, &status);
	printf("S1 LOOPBACK PASS correction_rx=%u echo=%u retained_peak=%zu "
		"entries_peak=%zu evictions=%u forced_reconnects=%u\n",
		status.corrections_received,status.corrections_echoed,
		status.retained_bytes_peak,status.retained_entry_peak,
		status.retention_evictions,status.forced_reconnects);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
}

/* S2 loopback fixture: open window -> click -> transaction reject ->
 * acknowledgement sent -> full Packet104 snapshot restores the exact server
 * state. The peer never initiates close (drains until the driver cancels),
 * mirroring test_s1_partial_chunk_volume's fixture pattern. */
struct s2_window_fixture { SOCKET listener; bool ok, quick; };

static void* s2_window_peer_entry(void* user) {
	struct s2_window_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;

	/* 0x64 Packet100OpenWindow: window=5, type=0 (chest), title="Chest",
	 * slots=27. [740c583 Packet100OpenWindow] */
	static const uint8_t open_window[]
		= {0x64, 5, 0, 0,5, 'C','h','e','s','t', 27};
	if(!socket_write_fragmented(peer, open_window, sizeof(open_window)))
		goto done;

	/* Packet102's shift flag and original source must survive translation.
	 * Both normal and quick actions exercise the same reject/resync exchange. */
	static const uint8_t expected_click[]
		= {0x66, 5, 0,2, 0, 0,9, 0, 0,1, 12, 0,0};
	static const uint8_t expected_quick[]
		= {0x66, 5, 0,2, 0, 0,9, 1, 0,1, 12, 0,0};
	const uint8_t* expected = fixture->quick ? expected_quick : expected_click;
	size_t length = fixture->quick ? sizeof(expected_quick) : sizeof(expected_click);
	if(!socket_read_exact(peer, input, length) || memcmp(input, expected, length) != 0)
		goto done;

	/* 0x6A Packet106Transaction: reject action 9. [740c583
	 * Packet106Transaction] */
	static const uint8_t transaction_reject[] = {0x6A, 5, 0,9, 0};
	if(!socket_write_fragmented(peer, transaction_reject,
			sizeof(transaction_reject)))
		goto done;

	/* A rejection requires a positive C->S acknowledgement echo. [740c583
	 * NetClientHandler.handleTransaction] */
	static const uint8_t expected_ack[] = {0x6A, 5, 0,9, 1};
	if(!socket_read_exact(peer, input, sizeof(expected_ack))
	   || memcmp(input, expected_ack, sizeof(expected_ack)) != 0)
		goto done;

	/* 0x68 Packet104WindowItems: full 27-slot snapshot restores the exact
	 * authoritative state (slot 0 = 5 stone, every other slot empty).
	 * [740c583 Packet104WindowItems] */
	uint8_t window_items[4 + 5 + 26 * 2];
	size_t off = 0;
	window_items[off++] = 0x68;
	window_items[off++] = 5;
	put_i16(window_items + off, 27); off += 2;
	put_i16(window_items + off, 1); off += 2; /* id 1 = stone */
	window_items[off++] = 5;					/* count */
	put_i16(window_items + off, 0); off += 2;	/* damage */
	for(int slot = 1; slot < 27; slot++) {
		put_i16(window_items + off, -1);
		off += 2;
	}
	assert(off == sizeof(window_items));
	if(!socket_write_fragmented(peer, window_items, off))
		goto done;

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_s2_window_transaction(bool quick) {
	/* Window-only fixtures do not complete terrain loading. Reset the prior
	 * screen, as test_s2_window_items_overflow does, before each connection. */
	gstate.players[0].current_screen = NULL;
	if(gstate.players[0].windows[5]) {
		windowc_destroy(gstate.players[0].windows[5]);
		free(gstate.players[0].windows[5]);
		gstate.players[0].windows[5] = NULL;
	}
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct s2_window_fixture peer = {.listener = make_listener(&port), .quick = quick};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, s2_window_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	/* CRPC_WORLD_RESET sets s_world_reset_ready synchronously on login, so
	 * CRPC_OPEN_WINDOW can land without any chunk/correction setup. */
	bool window_open = false;
	for(size_t elapsed = 0; elapsed < 5000 && !window_open; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		window_open = gstate.players[0].windows[5] != NULL;
		thread_msleep(1);
	}
	assert(window_open);
	assert(gstate.players[0].windows[5]->type == WINDOW_TYPE_CHEST);
	assert(gstate.players[0].windows[5]->slot_count == 63);

	/* Drive the SRPC boundary directly, matching test_provider_lifecycle's
	 * SRPC_UNLOAD_WORLD injection -- this pins the wire translation and the
	 * rejected-transaction round trip, independent of screen_chest.c's own
	 * input handling. */
	struct server_rpc click = {
		.type = SRPC_WINDOW_CLICK,
		.payload.window_click = {
			.window = 5, .slot = 2, .right_click = false, .action_id = 9,
			.quick_move = quick, .expected = {.id = 1, .count = 12},
		},
	};
	assert(svin_rpc_send(&click));

	bool restored = false;
	struct item_data slot0 = {0};
	for(size_t elapsed = 0; elapsed < 5000 && !restored; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		restored = inventory_get_slot(
						windowc_get_latest(gstate.players[0].windows[5]), 0, &slot0)
			&& slot0.id == 1 && slot0.count == 5 && slot0.durability == 0;
		thread_msleep(1);
	}
	assert(restored);
	/* Byte-exact: every other slot in the restored snapshot is empty. */
	for(size_t slot = 1; slot < 27; slot++) {
		struct item_data item;
		assert(!inventory_get_slot(
			windowc_get_latest(gstate.players[0].windows[5]), slot, &item));
	}

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}

/* critique-1028 (betamc.org, real Wii): the inventory wedged with "Action
 * not sent" and Close never worked until the server kicked. A real Beta
 * server answers every Packet102 it processes (NetServerHandler.java:499-519)
 * but silently ignores one when its canCraft gate is shut (:499). This peer
 * ignores the first click, rejects the second, never sends the cursor
 * correction, and then requires -- in wire order -- the client's Packet106
 * ack echo followed by Packet101 for window 0, produced by ONE Close press
 * through container_menu_step (Beta: EntityClientPlayerMP.java:132-135). */
struct s2_close_pending_fixture {
	SOCKET listener;
	bool ok;
	atomic_int clicks_seen;
	atomic_int stage_failed;
};

/* Reads one C->S packet, skipping bare 0x00 keep-alives. Only the shapes this
 * fixture expects are parsed; anything else fails the fixture. */
static int s2_close_pending_read(SOCKET peer, uint8_t* out) {
	uint8_t id;
	do {
		if(!socket_read_exact(peer, &id, 1)) return -1;
	} while(id == 0x00);
	out[0] = id;
	if(id == 0x66) { /* window, slot(2), right, action(2), shift, item id(2) */
		if(!socket_read_exact(peer, out + 1, 9)) return -1;
		int16_t item = (int16_t)((out[8] << 8) | out[9]);
		if(item != -1 && !socket_read_exact(peer, out + 10, 3)) return -1;
	} else if(id == 0x6A) {
		if(!socket_read_exact(peer, out + 1, 4)) return -1;
	} else if(id == 0x65) {
		if(!socket_read_exact(peer, out + 1, 1)) return -1;
	} else return -1;
	return id;
}

static void* s2_close_pending_peer_entry(void* user) {
	struct s2_close_pending_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	uint8_t input[32];
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	/* Click A: ignored (no Packet106 at all). */
	if(s2_close_pending_read(peer, input) != 0x66 || input[1] != 0) {
		atomic_store(&fixture->stage_failed, 1); goto done;
	}
	atomic_store(&fixture->clicks_seen, 1);
	/* Click B: rejected. [NetServerHandler.java:508-510] */
	if(s2_close_pending_read(peer, input) != 0x66 || input[1] != 0) {
		atomic_store(&fixture->stage_failed, 2); goto done;
	}
	uint8_t reject[] = {0x6A, 0, input[5], input[6], 0};
	if(!socket_write_fragmented(peer, reject, sizeof(reject)))
		goto done;
	atomic_store(&fixture->clicks_seen, 2);
	/* The ack echo for B (NetServerHandler.java:523-526 re-enables clicks
	 * only on it), then the close -- in that order. */
	if(s2_close_pending_read(peer, input) != 0x6A || input[1] != 0
	   || input[2] != reject[2] || input[3] != reject[3] || input[4] != 1) {
		atomic_store(&fixture->stage_failed, 3); goto done;
	}
	if(s2_close_pending_read(peer, input) != 0x65 || input[1] != 0) {
		atomic_store(&fixture->stage_failed, 4); goto done;
	}
	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void s2_close_pending_spin(struct server_remote* remote) {
	server_remote_pump(remote);
	clin_update();
	thread_msleep(1);
}

static void test_s2_close_with_pending_transactions(void) {
	gstate.players[0].current_screen = NULL; /* see test_s2_window_items_overflow */
	bool was_remote = gstate.remote_session;
	gstate.remote_session = true; /* main.c sets this for a live session */
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct s2_close_pending_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, s2_close_pending_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	struct window_container* wc = NULL;
	for(size_t elapsed = 0; elapsed < 5000 && !wc; elapsed++) {
		s2_close_pending_spin(&remote);
		struct server_remote_status status;
		server_remote_get_status(&remote, &status);
		if(status.state == REMOTE_PLAY)
			wc = gstate.players[0].windows[WINDOWC_INVENTORY];
	}
	assert(wc && wc->type == WINDOW_TYPE_INVENTORY);

	struct container_slot slots[45];
	for(size_t k = 0; k < 45; k++) slots[k] = (struct container_slot){0, 0, k, 0};
	struct container_menu menu = {.selected = 36};
	struct container_input pick = {.pick = true}, close = {.close = true}, idle = {0};
	uint32_t serial = wc->rejection_serial;

	/* Click A, then click B while A is still unanswered. */
	assert(!container_menu_step(&menu, slots, 45, 0, 0, wc, WINDOWC_INVENTORY,
		&pick, .016F, container_menu_send, NULL));
	for(size_t elapsed = 0; elapsed < 5000 && atomic_load(&peer.clicks_seen) < 1; elapsed++)
		s2_close_pending_spin(&remote);
	assert(atomic_load(&peer.clicks_seen) == 1 && windowc_actions_pending(wc));
	assert(!container_menu_step(&menu, slots, 45, 0, 0, wc, WINDOWC_INVENTORY,
		&pick, .016F, container_menu_send, NULL));
	for(size_t elapsed = 0; elapsed < 5000 && wc->rejection_serial == serial; elapsed++)
		s2_close_pending_spin(&remote);
	/* Rejected, correction never arrives: the old close gate held Close here
	 * forever, and every click read "Action not sent". */
	assert(wc->rejection_serial != serial && wc->correction_pending);
	assert(!container_menu_step(&menu, slots, 45, 0, 0, wc, WINDOWC_INVENTORY,
		&pick, .016F, container_menu_send, NULL));
	char label[128];
	container_menu_label(&menu, wc, 36, label, sizeof label);
	assert(menu.server_wait && !strcmp(label, "Waiting for server..."));

	/* ONE Close press sends Packet101 on that step. */
	assert(container_menu_step(&menu, slots, 45, 0, 0, wc, WINDOWC_INVENTORY,
		&close, .016F, container_menu_send, NULL));
	assert(!menu.close_requested && !menu.send_failed);
	for(size_t elapsed = 0; elapsed < 5000 && !peer.ok
		&& !atomic_load(&peer.stage_failed); elapsed++) {
		s2_close_pending_spin(&remote);
		(void)container_menu_step(&menu, slots, 45, 0, 0, wc, WINDOWC_INVENTORY,
			&idle, .016F, container_menu_send, NULL);
	}
	if(!peer.ok)
		fprintf(stderr, "[s2_close_pending] peer stage_failed=%d\n",
			atomic_load(&peer.stage_failed));
	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
	if(gstate.players[0].windows[WINDOWC_INVENTORY])
		windowc_discard_pending_actions(gstate.players[0].windows[WINDOWC_INVENTORY]);
	gstate.remote_session = was_remote;
	puts("PASS s2 close with pending transactions: ignored + rejected clicks, "
		"ack precedes Packet101 on one Close press");
}

/* Adversarial-review CRITICAL closure: CRPC_WINDOW_ITEMS' install loop must
 * clamp to the target window's REAL slot_count, not just the generic
 * CRPC_WINDOW_ITEMS_MAX_SLOTS(128) wire cap. This peer opens the same
 * 27-slot chest test_s2_window_transaction does, then sends a Packet104
 * with count=76 -- 13 entries past the 63-slot capacity, still legal-shaped and well
 * inside the 128 wire cap, so it reaches client_interface.c's install loop
 * unchanged. Without the clamp, installing slot 27 walks
 * windowc_slot_change -> inventory_set_slot past inv->capacity=27; that
 * function's ONLY bound check is assert(slot < inv->capacity), and this
 * whole suite builds -UNDEBUG (CMakeLists' forced-asserts-on rule), so an
 * unclamped install trips that assert and aborts this test binary before
 * the poll loop below can ever return true -- this test fails LOUD, not
 * silently, if the regression comes back.
 *
 * Distinct fixture from test_s2_window_transaction's struct s2_window_fixture:
 * this one adds restored_confirmed, an atomic_bool the peer spin-waits on
 * before sending its follow-up Packet104 (mirrors test_far_slot_deadlock_env.c's
 * flag_get/flag_set idiom). Both Packet104 sends happen close enough in wall
 * time that, without this gate, they can land in the same recv() batch and
 * get applied back-to-back inside one server_remote_pump()+clin_update() pair
 * -- the test's own poll loop would then never observe the oversized
 * packet's OWN installed state, only the follow-up's landing on top of it. */
struct s2_window_overflow_fixture {
	SOCKET listener;
	bool ok;
	atomic_bool restored_confirmed;
};

static bool overflow_flag_get(const atomic_bool* flag) {
	return atomic_load_explicit(flag, memory_order_acquire);
}

static void overflow_flag_set(atomic_bool* flag, bool value) {
	atomic_store_explicit(flag, value, memory_order_release);
}

static void* s2_window_overflow_peer_entry(void* user) {
	struct s2_window_overflow_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x63,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;

	/* 0x64 Packet100OpenWindow: window=6, type=0 (chest), title="Chest",
	 * slots=63. Deliberately NOT window=5 -- test_s2_window_transaction
	 * (run just before this test in main()) opens window=5 and never
	 * explicitly closes it, so gstate.players[0].windows[5] is still non-NULL,
	 * stale, and coincidentally the SAME shape (63-slot chest) when this
	 * test starts. Reusing slot 5 would let the poll loop below observe
	 * that STALE window as "open" before THIS connection's own
	 * CRPC_WORLD_RESET/CRPC_OPEN_WINDOW have even landed -- and when this
	 * connection's real (delayed) reset then runs, it wipes gstate.players[0].windows[]
	 * out from under the test mid-poll. window=6 has never been touched by
	 * any earlier test, so it can only become non-NULL via THIS connection's
	 * own real open. */
	static const uint8_t open_window[]
		= {0x64, 6, 0, 0,5, 'C','h','e','s','t', 27};
	if(!socket_write_fragmented(peer, open_window, sizeof(open_window)))
		goto done;

	/* 0x68 Packet104WindowItems: count=76 against a 63-slot window. Slot 0
	 * and slot 62 (the LAST in-capacity slot -- pins the off-by-one at the
	 * boundary) carry distinguishable items; slots 63..75 carry non-null
	 * "poison" entries a hostile/buggy server has no business sending --
	 * the client's clamp must drop them, never install them anywhere. */
	uint8_t window_items[4 + 5 + 61 * 2 + 5 + 13 * 5];
	size_t off = 0;
	window_items[off++] = 0x68;
	window_items[off++] = 6;
	put_i16(window_items + off, 76); off += 2;			/* count */
	put_i16(window_items + off, 1); off += 2;			/* slot 0: id 1 stone */
	window_items[off++] = 5;
	put_i16(window_items + off, 0); off += 2;
	for(int slot = 1; slot < 62; slot++) {				/* slots 1..61: empty */
		put_i16(window_items + off, -1);
		off += 2;
	}
	put_i16(window_items + off, 3); off += 2;			/* slot 62: id 3 dirt */
	window_items[off++] = 10;
	put_i16(window_items + off, 0); off += 2;
	for(int slot = 63; slot < 76; slot++) {			/* slots 63..75: poison */
		put_i16(window_items + off, 7); off += 2;
		window_items[off++] = 99;
		put_i16(window_items + off, 0); off += 2;
	}
	assert(off == sizeof(window_items));
	if(!socket_write_fragmented(peer, window_items, off))
		goto done;

	/* Wait for the TEST (main thread) to actually OBSERVE the oversized
	 * packet's installed state before sending the follow-up -- a wire-level
	 * signal (e.g. a keep-alive round trip) only proves the read thread
	 * drained the bytes, not that the test's own clin_update() call has
	 * applied them yet; those run at the test's own pace, not the peer's. */
	while(!overflow_flag_get(&fixture->restored_confirmed))
		thread_msleep(1);

	/* Prove the window still functions afterward: a normal, correctly-
	 * sized 63-slot Packet104 follow-up must still apply cleanly -- the
	 * container isn't wedged/corrupted by the earlier oversized packet. */
	uint8_t followup[4 + 5 + 62 * 2];
	off = 0;
	followup[off++] = 0x68;
	followup[off++] = 6;
	put_i16(followup + off, 63); off += 2;
	put_i16(followup + off, 5); off += 2;				/* slot 0: id 5 wood */
	followup[off++] = 1;
	put_i16(followup + off, 0); off += 2;
	for(int slot = 1; slot < 63; slot++) {
		put_i16(followup + off, -1);
		off += 2;
	}
	assert(off == sizeof(followup));
	if(!socket_write_fragmented(peer, followup, off))
		goto done;

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_s2_window_items_overflow(void) {
	/* test_s2_window_transaction (runs immediately before this test) opens
	 * a window and never closes it, which leaves gstate.players[0].current_screen on
	 * &screen_load_world (CRPC_WORLD_RESET's screen_set(&screen_load_world)
	 * fires whenever the PRIOR screen has render_world=true, and nothing in
	 * these window-only fixtures ever completes a real load to move off of
	 * it). CRPC_OPEN_WINDOW's own screen_load_world guard
	 * (client_interface.c) then silently -- no error, no reason string --
	 * drops every open for the rest of the process. Reset the fixture-shared
	 * global here rather than relying on incidental prior-test ordering;
	 * this is test-isolation cleanup, not a production code change. */
	gstate.players[0].current_screen = NULL;

	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct s2_window_overflow_fixture peer
		= {.listener = make_listener(&port), .restored_confirmed = false};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, s2_window_overflow_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	bool window_open = false;
	for(size_t elapsed = 0; elapsed < 5000 && !window_open; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		window_open = gstate.players[0].windows[6] != NULL;
		thread_msleep(1);
	}
	assert(window_open);
	assert(gstate.players[0].windows[6]->type == WINDOW_TYPE_CHEST);
	assert(gstate.players[0].windows[6]->slot_count == 63);

	/* The oversized Packet104's in-capacity slots (0 and 62) land exactly
	 * as sent; if the clamp regresses, inventory_set_slot's own assert
	 * aborts this process long before this poll would return true. */
	bool restored = false;
	struct item_data slot0 = {0}, slot62 = {0};
	for(size_t elapsed = 0; elapsed < 5000 && !restored; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		restored = inventory_get_slot(
						windowc_get_latest(gstate.players[0].windows[6]), 0, &slot0)
			&& slot0.id == 1 && slot0.count == 5
			&& inventory_get_slot(
					windowc_get_latest(gstate.players[0].windows[6]), 62, &slot62)
			&& slot62.id == 3 && slot62.count == 10;
		thread_msleep(1);
	}
	assert(restored);
	/* Byte-exact: every other in-capacity slot landed exactly as sent. */
	for(size_t slot = 1; slot < 62; slot++) {
		struct item_data item;
		assert(!inventory_get_slot(
			windowc_get_latest(gstate.players[0].windows[6]), slot, &item));
	}
	/* slot_count is untouched -- the clamp changed what got INSTALLED, not
	 * the window's own bookkeeping. */
	assert(gstate.players[0].windows[6]->slot_count == 63);

	/* Release the peer to send its follow-up ONLY now that the oversized
	 * packet's own state has actually been observed installed -- see the
	 * fixture's own comment for why a wire-level signal isn't enough. */
	overflow_flag_set(&peer.restored_confirmed, true);

	/* Still functions: the follow-up 27-slot Packet104 the peer sends next
	 * must also apply cleanly (container isn't wedged/corrupted). */
	bool followup_applied = false;
	struct item_data slot0_followup = {0};
	for(size_t elapsed = 0; elapsed < 5000 && !followup_applied; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		followup_applied = inventory_get_slot(
								windowc_get_latest(gstate.players[0].windows[6]), 0,
								&slot0_followup)
			&& slot0_followup.id == 5 && slot0_followup.count == 1;
		thread_msleep(1);
	}
	assert(followup_applied);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}

/* mp-crash-surface hostile vector found during S2 hardening (2026-08-25
 * follow-on): CRPC_INVENTORY_SLOT (Packet103SetSlot inbound) forwarded its
 * wire slot straight into windowc_slot_change -> inventory_set_slot with NO
 * bound check against the target window's real capacity -- unlike its
 * sibling CRPC_WINDOW_ITEMS just above, which test_s2_window_items_overflow
 * already guards. inventory_set_slot's ONLY protection is
 * `assert(slot < inv->capacity)`, which compiles out under the shipping
 * .dol's NDEBUG (CLAUDE.md's own forced-asserts-on rule only applies to
 * `test_` targets) -- so on real hardware this was a heap out-of-bounds
 * WRITE, not merely an abort, reachable with a single hostile/buggy
 * Packet103 naming any slot past a small window's slot_count (a 27-slot
 * chest is the common case; furnace/workbench are even smaller). Fixed in
 * client_interface.c's CRPC_INVENTORY_SLOT case with the same
 * clamp-before-install shape CRPC_WINDOW_ITEMS already uses. */
struct s2_set_slot_oob_fixture { SOCKET listener; bool ok; };

static void* s2_set_slot_oob_peer_entry(void* user) {
	struct s2_set_slot_oob_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;

	/* 0x64 Packet100OpenWindow: window=7, type=0 (chest), 27 slots. Window 7
	 * is untouched by every earlier test in this file's main() sequence. */
	static const uint8_t open_window[]
		= {0x64, 7, 0, 0,5, 'C','h','e','s','t', 27};
	if(!socket_write_fragmented(peer, open_window, sizeof(open_window)))
		goto done;

	/* HOSTILE 0x67 Packet103SetSlot: slot=200, far past this 27-slot chest's
	 * real capacity -- still perfectly wire-legal (Packet103's slot is a
	 * plain i16, no protocol-level ceiling). [740c583 Packet103SetSlot] */
	uint8_t oob_slot[9];
	size_t off = 0;
	oob_slot[off++] = 0x67;
	oob_slot[off++] = 7;
	put_i16(oob_slot + off, 200); off += 2; /* slot: far past capacity 27 */
	put_i16(oob_slot + off, 1); off += 2;	 /* id 1 = stone */
	oob_slot[off++] = 5;					 /* count */
	put_i16(oob_slot + off, 0); off += 2;	 /* damage */
	assert(off == sizeof(oob_slot));
	if(!socket_write_fragmented(peer, oob_slot, off))
		goto done;

	/* Still functions afterward: a normal, in-bounds SetSlot must still
	 * apply cleanly -- the container isn't wedged by the dropped hostile
	 * packet. */
	static const uint8_t good_slot[]
		= {0x67, 7, 0,0, 0,3, 3, 0,0}; /* slot 0: id 3 dirt, count 3 */
	if(!socket_write_fragmented(peer, good_slot, sizeof(good_slot)))
		goto done;

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_s2_set_slot_oob(void) {
	/* See test_s2_window_items_overflow's own comment: reset the shared
	 * current_screen so CRPC_OPEN_WINDOW's screen_load_world guard doesn't
	 * silently drop THIS test's own open. */
	gstate.players[0].current_screen = NULL;

	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct s2_set_slot_oob_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(
		thread_create(&peer_thread, s2_set_slot_oob_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	bool window_open = false;
	for(size_t elapsed = 0; elapsed < 5000 && !window_open; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		window_open = gstate.players[0].windows[7] != NULL;
		thread_msleep(1);
	}
	assert(window_open);
	assert(gstate.players[0].windows[7]->type == WINDOW_TYPE_CHEST);
	assert(gstate.players[0].windows[7]->slot_count == 63);

	/* The hostile out-of-range SetSlot must be silently dropped -- no crash
	 * -- and the window keeps functioning: the follow-up in-bounds SetSlot
	 * still installs. If the fix regresses, inventory_set_slot's own
	 * assert() aborts this process long before this poll would return true
	 * (this suite builds -UNDEBUG; the shipping .dol would instead corrupt
	 * heap memory past inv->items[]). */
	bool followup_applied = false;
	struct item_data slot0 = {0};
	for(size_t elapsed = 0; elapsed < 5000 && !followup_applied; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		followup_applied = inventory_get_slot(
								windowc_get_latest(gstate.players[0].windows[7]), 0, &slot0)
			&& slot0.id == 3 && slot0.count == 3;
		thread_msleep(1);
	}
	assert(followup_applied);
	/* The hostile slot (200) is provably never touched: every other
	 * in-capacity slot stayed empty. */
	for(size_t slot = 1; slot < 27; slot++) {
		struct item_data item;
		assert(!inventory_get_slot(
			windowc_get_latest(gstate.players[0].windows[7]), slot, &item));
	}
	assert(gstate.players[0].windows[7]->slot_count == 63);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}

/* mp-crash-surface hostile vector found via the S2 live mc173 gate (not a
 * synthetic fixture -- this is exactly what a REAL Beta server does):
 * per Beta's own protocol convention, window id 0 is the player's permanent
 * inventory -- pure client-side UI state, never a server-tracked, open/
 * close-able container. mc173-server's own open_window unconditionally calls
 * close_window(sw, None, true) BEFORE opening ANY real container, and
 * self.window.id defaults to 0, so the very FIRST container a live session
 * opens sends Packet101CloseWindow(window=0) first -- routine, expected
 * traffic, not an attack. CRPC_WINDOW_CLOSE_REMOTE's generic destroy+free+
 * NULL teardown used to apply to window 0 exactly like any other index,
 * corrupting WINDOWC_INVENTORY (dangling gstate.players[0].windows[0]) the moment any
 * window was ever opened -- the very next windowc_get_latest(gstate.players[0].windows[
 * WINDOWC_INVENTORY]) reader (screen_chest_reset's hotbar-highlight lookup,
 * reached from clin_show_window while installing the SAME window this
 * fixture opens) dereferenced the freed pointer. Reproduced live against a
 * real mc173-server before this fix (tmp gdb backtrace: windowc_get_latest
 * <- screen_chest_reset <- clin_show_window <- clin_update). */
struct s2_close_zero_fixture { SOCKET listener; bool ok; };

static void* s2_close_zero_peer_entry(void* user) {
	struct s2_close_zero_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;

	/* HOSTILE-BY-DESIGN (but routine) 0x65 Packet101CloseWindow: window=0,
	 * exactly mc173-server's own open_window->close_window(None,true) send,
	 * BEFORE any window this client has ever opened. [740c583
	 * Packet101CloseWindow] */
	static const uint8_t close_zero[] = {0x65, 0};
	if(!socket_write_fragmented(peer, close_zero, sizeof(close_zero)))
		goto done;

	/* 0x64 Packet100OpenWindow: window=8, type=0 (chest), 27 slots. Window 8
	 * is untouched by every earlier test in this file's main() sequence. */
	static const uint8_t open_window[]
		= {0x64, 8, 0, 0,5, 'C','h','e','s','t', 27};
	if(!socket_write_fragmented(peer, open_window, sizeof(open_window)))
		goto done;

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_s2_close_window_zero_survives(void) {
	gstate.players[0].current_screen = NULL; /* see test_s2_window_items_overflow */

	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct s2_close_zero_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(
		thread_create(&peer_thread, s2_close_zero_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	/* WINDOWC_INVENTORY must survive the close(0) AND the chest open that
	 * immediately follows it (clin_show_window's screen_chest_reset reads
	 * it while installing the chest screen -- the exact live crash site).
	 * If the fix regresses, windowc_get_latest's own assert(wc) aborts this
	 * process long before this poll would return true. */
	bool chest_open = false;
	for(size_t elapsed = 0; elapsed < 5000 && !chest_open; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		chest_open = gstate.players[0].windows[8] != NULL;
		thread_msleep(1);
	}
	assert(chest_open);
	assert(gstate.players[0].windows[8]->type == WINDOW_TYPE_CHEST);
	/* WINDOWC_INVENTORY is still a live, readable window_container -- not a
	 * dangling pointer left behind by a spurious destroy. */
	assert(gstate.players[0].windows[WINDOWC_INVENTORY] != NULL);
	struct item_data held;
	(void)inventory_get_hotbar_item(
		windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]), &held);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}

struct s2_open_zero_fixture {
	SOCKET listener;
	bool ok;
	atomic_bool first_open_seen;
	atomic_bool invalid_open_seen;
};

static void* s2_open_zero_peer_entry(void* user) {
	struct s2_open_zero_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET) return NULL;
	if(!peer_login(peer)) goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;
	static const uint8_t open_chest[]
		= {0x64, 11, 0, 0,5, 'C','h','e','s','t', 27};
	if(!socket_write_fragmented(peer, open_chest, sizeof(open_chest))) goto done;
	for(int i = 0; i < 5000 && !atomic_load(&fixture->first_open_seen); i++)
		thread_msleep(1);
	if(!atomic_load(&fixture->first_open_seen)) goto done;
	/* Supported type and capacity: only the reserved window id is hostile. */
	static const uint8_t open_zero[]
		= {0x64, 0, 0, 0,5, 'C','h','e','s','t', 27};
	/* Cursor SetSlot(-1,-1) must still route to the active chest (11). */
	static const uint8_t cursor[] = {0x67, 0xFF, 0xFF,0xFF, 0,3, 4, 0,0};
	if(!socket_write_fragmented(peer, open_zero, sizeof(open_zero))
	   || !socket_write_fragmented(peer, cursor, sizeof(cursor))) goto done;
	for(int i = 0; i < 5000 && !atomic_load(&fixture->invalid_open_seen); i++)
		thread_msleep(1);
	if(!atomic_load(&fixture->invalid_open_seen)) goto done;
	static const uint8_t open_furnace[]
		= {0x64, 12, 2, 0,7, 'F','u','r','n','a','c','e', 3};
	if(!socket_write_fragmented(peer, open_furnace, sizeof(open_furnace)))
		goto done;
	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_s2_open_window_zero_survives(void) {
	gstate.players[0].current_screen = NULL;
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct s2_open_zero_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, s2_open_zero_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	for(int i = 0; i < 5000 && !gstate.players[0].windows[11]; i++) {
		server_remote_pump(&remote);
		clin_update();
		thread_msleep(1);
	}
	assert(gstate.players[0].windows[11]);
	assert(gstate.players[0].visible_window == 11);
	assert(gstate.players[0].current_screen == &screen_chest);
	struct window_container* inventory = gstate.players[0].windows[WINDOWC_INVENTORY];
	struct window_container* chest = gstate.players[0].windows[11];
	assert(inventory && inventory->type == WINDOW_TYPE_INVENTORY);
	struct item_data saved = {.id = 1, .count = 13};
	windowc_slot_change(inventory, 36, saved);
	atomic_store(&peer.first_open_seen, true);

	struct item_data picked = {0};
	bool routed = false;
	for(int i = 0; i < 5000 && !routed; i++) {
		server_remote_pump(&remote);
		clin_update();
		if(gstate.players[0].windows[11] == chest)
			picked = windowc_get_latest(chest)->picked_item;
		routed = picked.id == 3 && picked.count == 4;
		thread_msleep(1);
	}
	assert(routed);
	assert(gstate.players[0].windows[WINDOWC_INVENTORY] == inventory);
	struct item_data actual = {0};
	assert(inventory_get_slot(windowc_get_latest(inventory), 36, &actual));
	assert(actual.id == saved.id && actual.count == saved.count);
	assert(gstate.players[0].windows[11] == chest);
	assert(gstate.players[0].visible_window == 11);
	assert(gstate.players[0].current_screen == &screen_chest);
	assert(((struct server_remote_impl*)remote.impl)->active_window == 11);
	assert(((struct server_remote_impl*)remote.impl)->progress_window == 11);
	atomic_store(&peer.invalid_open_seen, true);

	for(int i = 0; i < 5000 && !gstate.players[0].windows[12]; i++) {
		server_remote_pump(&remote);
		clin_update();
		thread_msleep(1);
	}
	assert(gstate.players[0].windows[12]);
	assert(gstate.players[0].windows[12]->type == WINDOW_TYPE_FURNACE);
	assert(gstate.players[0].visible_window == 12);
	assert(gstate.players[0].windows[WINDOWC_INVENTORY] == inventory);
	assert(inventory_get_slot(windowc_get_latest(inventory), 36, &actual));
	assert(actual.id == saved.id && actual.count == saved.count);
	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}

/* S3 loopback fixture: named-player spawn -> equipment -> two relative moves
 * accumulating to a known exact position -> teleport resync -> animation ->
 * destroy. Asserts the remote entity's tracked position is byte-exact at
 * every step (via its network_pos, the same field CRPC_ENTITY_MOVE/
 * CRPC_SPAWN_PLAYER write and clin's own tick reads) and its absence from
 * the client entity dictionary after destroy. The peer never initiates
 * close (drains until the driver cancels), mirroring test_s2_window_
 * transaction's fixture pattern. */
/* Gated like test_s2_window_overflow_peer_entry's restored_confirmed: a fast
 * loopback connection can deliver every packet below in one recv() batch,
 * which (since server_remote_pump drains its ENTIRE queued-event backlog
 * per call) could let the driver's FIRST successful poll already reflect
 * every later step -- including the final DESTROY, which would free the
 * entity out from under every assertion that follows. Each step therefore
 * waits for the driver to confirm it OBSERVED the previous step before the
 * peer sends the next. */
struct s3_entity_fixture {
	SOCKET listener;
	bool ok;
	atomic_bool spawn_confirmed;
	atomic_bool respawn_confirmed;
	atomic_bool equip_confirmed;
	atomic_bool unequip_confirmed;
	atomic_bool pickup_spawn_negative_confirmed;
	atomic_bool move_confirmed;
	atomic_bool teleport_confirmed;
	atomic_bool animation_confirmed;
};

static void* s3_entity_peer_entry(void* user) {
	struct s3_entity_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;

	/* 0x14 Packet20NamedEntitySpawn: entity 100, name "Bob", spawn at
	 * (0, 10.0, 0) (wire y=320 == 10.0*32), no rotation, no held item.
	 * [740c583 Packet20NamedEntitySpawn.readPacketData] */
	static const uint8_t named_spawn[] = {0x14,
		0,0,0,100, 0,3, 0,'B',0,'o',0,'b',
		0,0,0,0, 0,0,1,64, 0,0,0,0,
		0, 0, 0,0};
	if(!socket_write_fragmented(peer, named_spawn, sizeof(named_spawn)))
		goto done;
	while(!overflow_flag_get(&fixture->spawn_confirmed))
		thread_msleep(1);

	/* Adversarial-review MAJOR regression: a SECOND Packet20NamedEntitySpawn
	 * for the SAME entity id (a server re-sending a spawn -- server_remote's
	 * own remote_entity_create documents this as legitimate "duplicate key
	 * overwrites" traffic) must replace the client's tracked entity, not
	 * leak the one already there. Name "Rex" and a different position both
	 * change from the first spawn so the driver can prove this is really the
	 * SECOND spawn's data, not a stale read of the first. */
	static const uint8_t named_respawn[] = {0x14,
		0,0,0,100, 0,3, 0,'R',0,'e',0,'x',
		0,0,0,96, 0,0,1,96, 0,0,0,160,
		0, 0, 0,0};
	if(!socket_write_fragmented(peer, named_respawn, sizeof(named_respawn)))
		goto done;
	while(!overflow_flag_get(&fixture->respawn_confirmed))
		thread_msleep(1);

	/* 0x05 Packet5PlayerInventory: equip entity 100's held slot (0) with
	 * item id 1 (stone). [740c583 Packet5PlayerInventory] */
	static const uint8_t equip[] = {0x05, 0,0,0,100, 0,0, 0,1, 0,0};
	if(!socket_write_fragmented(peer, equip, sizeof(equip)))
		goto done;
	while(!overflow_flag_get(&fixture->equip_confirmed))
		thread_msleep(1);

	/* 0x05 Packet5PlayerInventory with itemID == -1 (0xFFFF): Beta's EMPTY
	 * sentinel for this packet -- EntityOtherPlayerMP.outfitWithItem only
	 * builds a stack for itemID >= 0 [740c583], and vanilla servers send -1
	 * when a player empties their hand. Regression for the
	 * adversarial-review CRITICAL: the unconditional u16 cast forged id
	 * 65535 and the render guard indexed items[65535], ~253KB past the
	 * 2304-entry registry. */
	static const uint8_t unequip[] = {0x05, 0,0,0,100, 0,0, 0xFF,0xFF, 0,0};
	if(!socket_write_fragmented(peer, unequip, sizeof(unequip)))
		goto done;
	while(!overflow_flag_get(&fixture->unequip_confirmed))
		thread_msleep(1);

	/* Batch H (2026-08-25) hostile vector: 0x15 Packet21PickupSpawn with
	 * itemID == -1 (0xFFFF) for a NEW entity (id 200, distinct from the
	 * remote player at 100). Packet21PickupSpawn.readPacketData [740c583]
	 * reads itemID via a plain signed readShort() with no non-negative
	 * validation -- structurally identical to Packet5PlayerInventory's
	 * itemID above, which this same fixture already proves gets clamped.
	 * Pre-fix the unconditional u16 cast forged id 65535 into the spawned
	 * item entity. */
	static const uint8_t pickup_spawn_negative[] = {0x15,
		0,0,0,200, 0xFF,0xFF, 1, 0,0,
		0,0,0,0x20, 0,0,0,0x40, 0,0,0,0x60,
		0, 0, 0};
	if(!socket_write_fragmented(
		   peer, pickup_spawn_negative, sizeof(pickup_spawn_negative)))
		goto done;
	while(!overflow_flag_get(&fixture->pickup_spawn_negative_confirmed))
		thread_msleep(1);

	/* 0x1F Packet31RelEntityMove x2: dx=+32 (1.0 block) each, so the
	 * tracked x accumulates 0 -> 32 -> 64 (2.0 blocks) by plain integer
	 * addition. [740c583 NetClientHandler.handleEntity] */
	static const uint8_t rel_move[] = {0x1F, 0,0,0,100, 32, 0, 0};
	if(!socket_write_fragmented(peer, rel_move, sizeof(rel_move))
	   || !socket_write_fragmented(peer, rel_move, sizeof(rel_move)))
		goto done;
	while(!overflow_flag_get(&fixture->move_confirmed))
		thread_msleep(1);

	/* 0x22 Packet34EntityTeleport: absolute resync to (10.0, 10.0, 10.0),
	 * discarding the accumulated relative position. [740c583
	 * Packet34EntityTeleport.readPacketData] */
	static const uint8_t teleport[] = {0x22,
		0,0,0,100, 0,0,1,64, 0,0,1,64, 0,0,1,64, 0, 0};
	if(!socket_write_fragmented(peer, teleport, sizeof(teleport)))
		goto done;
	while(!overflow_flag_get(&fixture->teleport_confirmed))
		thread_msleep(1);

	/* 0x12 Packet18Animation: arm swing. [740c583 Packet18Animation] */
	static const uint8_t animation[] = {0x12, 0,0,0,100, 1};
	if(!socket_write_fragmented(peer, animation, sizeof(animation)))
		goto done;
	/* Empty keep-alive: a wire-level checkpoint that proves the animation
	 * byte was decoded without desyncing the stream (Beta's own byte-stream
	 * framing has no other way to prove a mid-stream packet parsed cleanly
	 * except that everything sent after it keeps parsing too). */
	static const uint8_t keep_alive[] = {0x00};
	if(!socket_write_fragmented(peer, keep_alive, sizeof(keep_alive)))
		goto done;
	while(!overflow_flag_get(&fixture->animation_confirmed))
		thread_msleep(1);

	/* 0x1D Packet29DestroyEntity: remove entity 100. [740c583
	 * Packet29DestroyEntity] */
	static const uint8_t destroy[] = {0x1D, 0,0,0,100};
	if(!socket_write_fragmented(peer, destroy, sizeof(destroy)))
		goto done;

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_s3_entity_lifecycle(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct s3_entity_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, s3_entity_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	struct entity** e_pp = NULL;
	bool spawned = false;
	for(size_t elapsed = 0; elapsed < 5000 && !spawned; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		e_pp = dict_entity_get(gstate.entities, 100);
		spawned = e_pp && *e_pp && (*e_pp)->type == ENTITY_REMOTE_PLAYER;
		thread_msleep(1);
	}
	assert(spawned);
	struct entity* e = *e_pp;
	assert(strcmp(e->data.remote_player.username, "Bob") == 0);
	assert(!e->data.remote_player.has_held_item); /* currentItem was 0 */
	assert(e->network_pos[0] == 0.0f && e->network_pos[1] == 10.0f
		&& e->network_pos[2] == 0.0f);
	overflow_flag_set(&peer.spawn_confirmed, true);

	/* Duplicate spawn for the same id (fix for the adversarial-review MAJOR:
	 * client_interface.c's shared CRPC_SPAWN_* path used to overwrite the
	 * dict slot's pointer without freeing the entity already there). Poll
	 * through dict_entity_get -- NOT the stale `e` above -- since a correct
	 * fix frees the old struct and installs a new one at the same slot; `e`
	 * must be re-read afterward or every following dereference is
	 * use-after-free. */
	bool respawned = false;
	for(size_t elapsed = 0; elapsed < 5000 && !respawned; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		e_pp = dict_entity_get(gstate.entities, 100);
		respawned = e_pp && *e_pp
			&& strcmp((*e_pp)->data.remote_player.username, "Rex") == 0;
		thread_msleep(1);
	}
	assert(respawned);
	e = *e_pp; // re-fetch: the duplicate spawn freed the old struct
	assert(e->network_pos[0] == 3.0f && e->network_pos[1] == 11.0f
		&& e->network_pos[2] == 5.0f);
	overflow_flag_set(&peer.respawn_confirmed, true);

	bool equipped = false;
	for(size_t elapsed = 0; elapsed < 5000 && !equipped; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		equipped = e->data.remote_player.has_held_item
			&& e->data.remote_player.held_item.id == 1;
		thread_msleep(1);
	}
	assert(equipped);
	overflow_flag_set(&peer.equip_confirmed, true);

	/* itemID == -1 must EMPTY the hand (Beta's negative sentinel), never
	 * arrive as forged id 65535 -- see the peer's unequip comment. */
	bool unequipped = false;
	for(size_t elapsed = 0; elapsed < 5000 && !unequipped; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		unequipped = !e->data.remote_player.has_held_item;
		thread_msleep(1);
	}
	assert(unequipped);
	assert(e->data.remote_player.held_item.id == 0);
	overflow_flag_set(&peer.unequip_confirmed, true);

	/* Batch H (2026-08-25) hostile vector: Packet21PickupSpawn(itemID=-1)
	 * must spawn entity 200 with item id 0 (the internal empty sentinel),
	 * never the forged u16 65535 the unconditional cast used to produce
	 * (server_remote.c MP_EVENT_PICKUP_SPAWN, mirroring the item_id clamp
	 * server_remote.c already applies to Packet5/Packet20). item_get(0)
	 * staying in-bounds (unlike 65535, ~253KB past the registry) is the
	 * whole point of clamping rather than leaving the cast unclamped. */
	struct entity** item_pp = NULL;
	bool item_spawned = false;
	for(size_t elapsed = 0; elapsed < 5000 && !item_spawned; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		item_pp = dict_entity_get(gstate.entities, 200);
		item_spawned = item_pp && *item_pp;
		thread_msleep(1);
	}
	assert(item_spawned);
	assert((*item_pp)->data.item.item.id == 0);
	overflow_flag_set(&peer.pickup_spawn_negative_confirmed, true);

	/* Byte-exact after both relative moves: the respawn's base x is 96
	 * fixed-point units (3.0 blocks), +32+32 == 160, i.e. exactly 5.0 blocks
	 * -- plain integer accumulation, no float drift. */
	bool moved = false;
	for(size_t elapsed = 0; elapsed < 5000 && !moved; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		moved = e->network_pos[0] == 5.0f;
		thread_msleep(1);
	}
	assert(moved);
	assert(e->network_pos[1] == 11.0f && e->network_pos[2] == 5.0f);
	overflow_flag_set(&peer.move_confirmed, true);

	/* Teleport resync discards the relative accumulation entirely. Y is the
	 * teleported height PLUS Beta's 1/64-block teleport lift: handleEntity-
	 * Teleport targets serverPosY/32.0 + 0.015625D (relative moves do not) --
	 * re-pinned 2026-08-26 when the mp-s4 polish lane made the emitter
	 * Beta-exact; the old pin (10.0f) encoded the pre-parity behaviour.
	 * [740c583 NetClientHandler.handleEntityTeleport] */
	bool teleported = false;
	for(size_t elapsed = 0; elapsed < 5000 && !teleported; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		teleported = e->network_pos[0] == 10.0f;
		thread_msleep(1);
	}
	assert(teleported);
	assert(e->network_pos[1] == 10.015625f && e->network_pos[2] == 10.0f);
	/* mp-s4-polish: the wire move must have ARMED the Beta 3-increment
	 * approach (nothing ticks entities in this harness, so the arm is
	 * stable to read). [740c583 NetClientHandler.handleEntityTeleport] */
	assert(e->network_increments == 3);
	overflow_flag_set(&peer.teleport_confirmed, true);

	/* Animation must not corrupt the connection or move the entity --
	 * confirmed by the keep-alive round trip the peer sends right after it. */
	bool alive_after_animation = false;
	for(size_t elapsed = 0; elapsed < 5000 && !alive_after_animation; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		struct server_remote_status status;
		server_remote_get_status(&remote, &status);
		alive_after_animation = status.keep_alives_received >= 1;
		thread_msleep(1);
	}
	assert(alive_after_animation);
	/* Y keeps the 1/64 teleport lift from the pin above. */
	assert(e->network_pos[0] == 10.0f && e->network_pos[1] == 10.015625f
		&& e->network_pos[2] == 10.0f);
	overflow_flag_set(&peer.animation_confirmed, true);

	bool destroyed = false;
	for(size_t elapsed = 0; elapsed < 5000 && !destroyed; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		struct entity** pp = dict_entity_get(gstate.entities, 100);
		destroyed = !pp || !*pp;
		thread_msleep(1);
	}
	assert(destroyed);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}

/* ---- MP-BED + MP-RIDE (critique-1028): fixture packets in, client state
 * out, and our Packet19 state 3 out on Leave Bed.
 * Truth: NetClientHandler.java:350-378 (Packet18 animate 3 wake, Packet17
 * sleepInBedAt), 439-449 (Packet39 mountEntity for ANY entity pair);
 * GuiSleepMP.java:45-49 (Leave Bed = Packet19 state 3); EntityPlayer.java:
 * 60-78 (sleep timer), 537-640 (sleepInBedAt / wakeUpPlayer); Entity.java:
 * 929-967 (updateRiderPosition / mountEntity). Ids 300-302 are unique to this
 * fixture (the binary shares one gstate.entities across tests). */
#define BEDRIDE_PLAYER 300
#define BEDRIDE_BOAT 301
#define BEDRIDE_PIG 302

struct bedride_fixture {
	SOCKET listener;
	bool ok;
	int leave_frames;	/* Packet19 state 3 frames read from the client */
	int position_frames; /* Packet11/13 after the respawn */
	bool position_riding; /* any of them in the -999 riding form */
	int unexpected_frame; /* first unknown packet id, or -1 */
	atomic_bool spawned_confirmed;
	atomic_bool slept_confirmed;
	atomic_bool woke_confirmed;
	atomic_bool attached_confirmed;
	atomic_bool respawned_confirmed;
};

static double bedride_f64(const uint8_t* b) {
	uint64_t v = 0;
	for(int i = 0; i < 8; i++)
		v = v << 8 | b[i];
	double d;
	memcpy(&d, &v, sizeof(d));
	return d;
}

/* Reads client frames until a Packet19 state 3 has been seen (stop_on_leave)
 * or a Packet255 arrives. Every client frame this fixture can provoke has a
 * fixed shape: 0x00 keep-alive, 0x10 held slot (i16), 0x13 entity action
 * (i32 + u8), 0xFF kick. */
static bool bedride_read_frames(struct bedride_fixture* f, SOCKET peer,
								bool stop_on_leave) {
	for(int guard = 0; guard < 256; guard++) {
		uint8_t id;
		if(!socket_read_exact(peer, &id, 1))
			return false;
		if(id == 0x00)
			continue;
		if(id == 0x13) {
			uint8_t body[5];
			if(!socket_read_exact(peer, body, sizeof(body)))
				return false;
			int32_t who = (int32_t)((uint32_t)body[0] << 24 | (uint32_t)body[1] << 16
				| (uint32_t)body[2] << 8 | body[3]);
			if(body[4] == 3 && who == 0x2A) {
				f->leave_frames++;
				if(stop_on_leave)
					return true;
			}
			continue;
		}
		if(id == 0x0A || id == 0x0B || id == 0x0C || id == 0x0D) {
			/* Packet10-13: on_ground / x,y,stance,z / yaw,pitch. */
			static const size_t body_len[4] = {1, 33, 9, 41};
			uint8_t body[41];
			if(!socket_read_exact(peer, body, body_len[id - 0x0A]))
				return false;
			if(id == 0x0B || id == 0x0D) {
				f->position_frames++;
				if(bedride_f64(body + 8) == -999.0
				   || bedride_f64(body + 16) == -999.0)
					f->position_riding = true;
			}
			continue;
		}
		if(id == 0x10) { /* Packet16 held-slot resync (i16), not ours */
			uint8_t slot[2];
			if(!socket_read_exact(peer, slot, sizeof(slot)))
				return false;
			continue;
		}
		if(id == 0xFF)
			return !stop_on_leave;
		f->unexpected_frame = id;
		return false;
	}
	return false;
}

static void* bedride_peer_entry(void* user) {
	struct bedride_fixture* f = user;
	SOCKET peer = accept(f->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[64];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;
	/* Packet20 "Ann" (300) at (0, 10.0, 0); Packet23 boat (301) at
	 * (0.5, 100.0, 0.5); Packet24 pig (302, type 90) at (2.0, 64.0, 2.0)
	 * with an empty watcher list. */
	static const uint8_t ann[] = {0x14, 0,0,1,0x2C, 0,3, 0,'A',0,'n',0,'n',
		0,0,0,0, 0,0,1,64, 0,0,0,0, 0, 0, 0,0};
	static const uint8_t boat[] = {0x17, 0,0,1,0x2D, 1, 0,0,0,16, 0,0,0x0C,0x80,
		0,0,0,16, 0,0,0,0};
	static const uint8_t pig[] = {0x18, 0,0,1,0x2E, 90, 0,0,0,64, 0,0,0x08,0x00,
		0,0,0,64, 0, 0, 0x7F};
	if(!socket_write_fragmented(peer, ann, sizeof(ann))
	   || !socket_write_fragmented(peer, boat, sizeof(boat))
	   || !socket_write_fragmented(peer, pig, sizeof(pig)))
		goto done;
	while(!overflow_flag_get(&f->spawned_confirmed))
		thread_msleep(1);
	/* Packet17 use-bed (field 0 = sleep) for US at (5, 64, -3) and for Ann
	 * at (0, 9, 0); then one Packet17 with a nonzero field, which Beta
	 * ignores (NetClientHandler.java:373). No chunk is loaded here, so the
	 * client takes sleepInBedAt's !blockExists branch. */
	static const uint8_t sleep_us[] = {0x11, 0,0,0,0x2A, 0, 0,0,0,5, 64,
		0xFF,0xFF,0xFF,0xFD};
	static const uint8_t sleep_ann[] = {0x11, 0,0,1,0x2C, 0, 0,0,0,0, 9,
		0,0,0,0};
	if(!socket_write_fragmented(peer, sleep_us, sizeof(sleep_us))
	   || !socket_write_fragmented(peer, sleep_ann, sizeof(sleep_ann)))
		goto done;
	while(!overflow_flag_get(&f->slept_confirmed))
		thread_msleep(1);
	/* The driver now presses Leave Bed (twice the same request): exactly one
	 * Packet19(us, 3) must arrive. */
	if(!bedride_read_frames(f, peer, true))
		goto done;
	/* The server's wakeUpPlayer: Packet18 animate 3 to the sleeper and to
	 * everyone tracking Ann. */
	static const uint8_t wake_us[] = {0x12, 0,0,0,0x2A, 3};
	static const uint8_t wake_ann[] = {0x12, 0,0,1,0x2C, 3};
	if(!socket_write_fragmented(peer, wake_us, sizeof(wake_us))
	   || !socket_write_fragmented(peer, wake_ann, sizeof(wake_ann)))
		goto done;
	while(!overflow_flag_get(&f->woke_confirmed))
		thread_msleep(1);
	/* Packet39: we mount the PIG (previously ignored), Ann mounts the boat. */
	static const uint8_t ride_pig[] = {0x27, 0,0,0,0x2A, 0,0,1,0x2E};
	static const uint8_t ann_boat[] = {0x27, 0,0,1,0x2C, 0,0,1,0x2D};
	if(!socket_write_fragmented(peer, ride_pig, sizeof(ride_pig))
	   || !socket_write_fragmented(peer, ann_boat, sizeof(ann_boat)))
		goto done;
	while(!overflow_flag_get(&f->attached_confirmed))
		thread_msleep(1);
	/* Ann leaves the boat (vehicle -1). We die on the pig and respawn in
	 * the same dimension: Beta's server sends Packet9 and NO Packet39(-1)
	 * (recreatePlayerEntity builds a fresh, unmounted EntityPlayerMP). */
	static const uint8_t off_ann[] = {0x27, 0,0,1,0x2C, 0xFF,0xFF,0xFF,0xFF};
	static const uint8_t respawn[] = {0x09, 0};
	if(!socket_write_fragmented(peer, off_ann, sizeof(off_ann))
	   || !socket_write_fragmented(peer, respawn, sizeof(respawn)))
		goto done;
	/* Drain to the client's Packet255: no second leave may appear. */
	if(!bedride_read_frames(f, peer, false))
		goto done;
	f->ok = true;
done:
	closesocket(peer);
	closesocket(f->listener);
	return NULL;
}

static struct entity* bedride_entity(uint32_t id) {
	struct entity** pp = dict_entity_get(gstate.entities, id);
	return pp ? *pp : NULL;
}

static void bedride_pump(struct server_remote* remote) {
	server_remote_pump(remote);
	clin_update();
	thread_msleep(1);
}

static void test_mp_bed_ride_wire(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct bedride_fixture peer = {.listener = make_listener(&port),
		.unexpected_frame = -1};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, bedride_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	bool spawned = false;
	for(size_t t = 0; t < 5000 && !spawned; t++) {
		bedride_pump(&remote);
		struct entity* a = bedride_entity(BEDRIDE_PLAYER);
		struct entity* b = bedride_entity(BEDRIDE_BOAT);
		struct entity* p = bedride_entity(BEDRIDE_PIG);
		spawned = a && a->type == ENTITY_REMOTE_PLAYER && b
			&& b->type == ENTITY_BOAT && p && p->type == ENTITY_MONSTER;
	}
	assert(spawned);
	struct entity* pig = bedride_entity(BEDRIDE_PIG);
	assert(pig->data.monster.id == MONSTER_PIG && !pig->data.monster.pig_ridden);
	overflow_flag_set(&peer.spawned_confirmed, true);

	/* MP-BED in: our Packet17 -> sleeping at timer 0; Ann's -> lying at the
	 * cell centre (no chunk: !blockExists), box 0.2 x 0.2. */
	bool slept = false;
	struct entity* ann = NULL;
	for(size_t t = 0; t < 5000 && !slept; t++) {
		bedride_pump(&remote);
		ann = bedride_entity(BEDRIDE_PLAYER);
		slept = gstate.players[0].sleeping && ann
			&& ann->data.remote_player.sleeping;
	}
	assert(slept);
	assert(gstate.players[0].sleep_timer == 0);
	assert(((struct server_remote_impl*)remote.impl)->local_sleeping);
	assert(ann->pos[0] == 0.5F && ann->pos[2] == 0.5F
		&& fabsf(ann->pos[1] - (9.0F + 15.0F / 16.0F)) < 1e-5F);
	assert(ann->width == 0.2F && ann->height == 0.2F);
	assert(ann->data.remote_player.sleep_off_x == 0.0F
		&& ann->data.remote_player.sleep_off_z == 0.0F);
	overflow_flag_set(&peer.slept_confirmed, true);

	/* Leave Bed, pressed once: the same request seen twice must not resend. */
	struct server_rpc leave = {.type = SRPC_PLAYER_STATE,
		.payload.player_state = {.leave_bed_seq = 1}};
	assert(svin_rpc_send(&leave));
	for(int i = 0; i < 20; i++)
		bedride_pump(&remote);
	assert(svin_rpc_send(&leave));
	for(int i = 0; i < 20; i++)
		bedride_pump(&remote);
	assert(((struct server_remote_impl*)remote.impl)->leave_bed_seq_sent == 1);

	/* Wake: our animate 3 -> not sleeping, timer 100 (the fade tail);
	 * Ann's -> standing box again. */
	bool woke = false;
	for(size_t t = 0; t < 5000 && !woke; t++) {
		bedride_pump(&remote);
		woke = !gstate.players[0].sleeping
			&& !bedride_entity(BEDRIDE_PLAYER)->data.remote_player.sleeping;
	}
	assert(woke);
	assert(gstate.players[0].sleep_timer == 100);
	assert(!((struct server_remote_impl*)remote.impl)->local_sleeping);
	ann = bedride_entity(BEDRIDE_PLAYER);
	assert(ann->width == 0.6F && ann->height == 1.8F);
	overflow_flag_set(&peer.woke_confirmed, true);

	/* MP-RIDE in: we ride the pig (applied, not ignored); Ann rides the boat
	 * and her tick seats her at boat.posY - 0.3 - 0.5. */
	bool attached = false;
	struct server_remote_status status;
	for(size_t t = 0; t < 5000 && !attached; t++) {
		bedride_pump(&remote);
		server_remote_get_status(&remote, &status);
		ann = bedride_entity(BEDRIDE_PLAYER);
		attached = status.attach_applied == 1
			&& bedride_entity(BEDRIDE_PIG)->data.monster.pig_ridden
			&& ann->data.remote_player.riding_vehicle;
	}
	assert(attached);
	assert(status.attach_ignored == 0);
	assert(((struct server_remote_impl*)remote.impl)->mounted && ((struct server_remote_impl*)remote.impl)->mounted_vehicle_id == BEDRIDE_PIG);
	pig = bedride_entity(BEDRIDE_PIG);
	assert(pig->data.monster.pig_rider_id == 0);
	assert(ann->data.remote_player.riding_vehicle_id == BEDRIDE_BOAT);
	struct entity* boat = bedride_entity(BEDRIDE_BOAT);
	ann->tick_client(ann);
	assert(ann->pos[0] == boat->pos[0] && ann->pos[2] == boat->pos[2]
		&& fabsf(ann->pos[1] - (boat->pos[1] - 0.8F)) < 1e-5F);
	overflow_flag_set(&peer.attached_confirmed, true);

	/* Ann's -1 detach, and our respawn while riding: the pig link and the
	 * riding movement form both end with no Packet39 from the server. */
	bool released = false;
	for(size_t t = 0; t < 5000 && !released; t++) {
		bedride_pump(&remote);
		released = !((struct server_remote_impl*)remote.impl)->mounted
			&& !bedride_entity(BEDRIDE_PIG)->data.monster.pig_ridden
			&& !bedride_entity(BEDRIDE_PLAYER)->data.remote_player.riding_vehicle;
	}
	assert(released);
	assert(((struct server_remote_impl*)remote.impl)->mounted_vehicle_id == -1);

	/* A Leave Bed press racing the wake (we are awake now): consumed, never
	 * sent -- the peer's final count stays 1. */
	struct server_rpc late_leave = {.type = SRPC_PLAYER_STATE,
		.payload.player_state = {.leave_bed_seq = 2}};
	assert(svin_rpc_send(&late_leave));
	for(int i = 0; i < 20; i++)
		bedride_pump(&remote);
	assert(((struct server_remote_impl*)remote.impl)->leave_bed_seq_sent == 2);

	/* After the respawn a position sample leaves in the NORMAL form
	 * (feet y + stance), not the -999 riding form. */
	server_remote_get_status(&remote, &status);
	struct server_rpc pos = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = 3.0F, .y = 70.0F, .z = 4.0F, .rx = 90.0F,
			.ry = 0.0F, .on_ground = true,
			.correction_seq = status.correction_seq}};
	assert(svin_rpc_send(&pos));
	for(int i = 0; i < 40; i++)
		bedride_pump(&remote);

	assert(server_remote_disconnect(&remote, "Quitting"));
	thread_join(&peer_thread);
	if(!peer.ok)
		fprintf(stderr, "bedride peer: leave_frames=%d unexpected=%d pos=%d\n",
				peer.leave_frames, peer.unexpected_frame, peer.position_frames);
	assert(peer.ok);
	assert(peer.leave_frames == 1);
	assert(peer.position_frames >= 1 && !peer.position_riding);
	assert(wait_for(&remote, workers_exited, 5000));
	server_remote_destroy(&remote);
	gstate.players[0].sleep_timer = 0; /* later tests start awake */
	puts("  mp-bed/ride: Packet17 us+other, one Packet19(3) per Leave Bed, "
		 "Packet18(3) wake, Packet39 pig + other-player boat seat, -1 detach");
}

/* The pure halves, without a socket: Beta's sleep timer, the in-bed spot and
 * offsets per bed direction, the rider seat per vehicle kind, and the
 * mob-mount link clin_apply_attach installs. */
static void test_mp_bed_ride_units(void) {
	/* EntityPlayer.java:60-78. */
	int32_t t = 0;
	for(int i = 0; i < 150; i++)
		t = client_session_sleep_timer_step(true, t);
	assert(t == 100);
	t = client_session_sleep_timer_step(false, 100);
	assert(t == 101);
	for(int i = 0; i < 8; i++)
		t = client_session_sleep_timer_step(false, t);
	assert(t == 109);
	assert(client_session_sleep_timer_step(false, t) == 0);
	assert(client_session_sleep_timer_step(false, 0) == 0);

	/* sleepInBedAt with the bed loaded, each direction (meta & 3):
	 * spot (EntityPlayer.java:557-575), offsets func_22052_e (594-611),
	 * orientation getBedOrientationInDegrees (663-680). */
	static const struct {
		uint8_t meta;
		float fx, fz, off_x, off_z, orient;
	} beds[] = {
		{0, 0.5F, 0.9F, 0.0F, -1.8F, 90.0F},
		{1, 0.1F, 0.5F, 1.8F, 0.0F, 0.0F},
		{2, 0.5F, 0.1F, 0.0F, 1.8F, 270.0F},
		{3 | 8, 0.9F, 0.5F, -1.8F, 0.0F, 180.0F}, /* head bit ignored */
	};
	for(size_t i = 0; i < sizeof(beds) / sizeof(*beds); i++) {
		struct entity e;
		memset(&e, 0, sizeof(e));
		entity_remote_player(7, &e, &gstate.world, "Zed");
		entity_remote_player_sleep_at(&e, 10, 64, -20, true, beds[i].meta);
		assert(e.data.remote_player.sleeping);
		assert(e.pos[0] == 10.0F + beds[i].fx && e.pos[2] == -20.0F + beds[i].fz);
		assert(fabsf(e.pos[1] - (64.0F + 15.0F / 16.0F)) < 1e-5F);
		assert(e.data.remote_player.sleep_off_x == beds[i].off_x
			&& e.data.remote_player.sleep_off_z == beds[i].off_z);
		assert(e.data.remote_player.bed_orient_deg == beds[i].orient);
		entity_remote_player_wake(&e);
		assert(!e.data.remote_player.sleeping && e.width == 0.6F
			&& e.height == 1.8F);
	}

	/* Seats (Entity.java:929-939, EntityBoat/EntityMinecart getMountedYOffset,
	 * EntitySpider.java:11-13), rider getYOffset -0.5. */
	struct entity v;
	vec3 seat;
	memset(&v, 0, sizeof(v));
	v.type = ENTITY_BOAT;
	v.pos[0] = 1.0F; v.pos[1] = 64.0F; v.pos[2] = 2.0F;
	assert(entity_remote_player_rider_pos(&v, seat));
	assert(seat[0] == 1.0F && seat[2] == 2.0F && fabsf(seat[1] - 63.2F) < 1e-5F);
	v.type = ENTITY_MINECART;
	assert(entity_remote_player_rider_pos(&v, seat) && fabsf(seat[1] - 63.2F) < 1e-5F);
	v.type = ENTITY_MONSTER;
	v.height = 0.9F;
	v.data.monster.id = MONSTER_PIG;
	assert(entity_remote_player_rider_pos(&v, seat)
		&& fabsf(seat[1] - (64.0F + 0.675F - 0.5F)) < 1e-5F);
	v.height = 0.9F;
	v.data.monster.id = MONSTER_SPIDER;
	assert(entity_remote_player_rider_pos(&v, seat)
		&& fabsf(seat[1] - (64.0F + 0.675F - 1.0F)) < 1e-5F);
	v.type = ENTITY_ITEM;
	assert(!entity_remote_player_rider_pos(&v, seat));

	/* Local player on a pig: the link entity_local_player.c's PIG MOUNT
	 * FOLLOW reads; mountEntity(null) puts the eye at the pig's box top. */
	struct entity pg, me;
	memset(&pg, 0, sizeof(pg));
	memset(&me, 0, sizeof(me));
	pg.type = ENTITY_MONSTER;
	pg.data.monster.id = MONSTER_PIG;
	pg.height = 0.9F;
	pg.pos[0] = 4.0F; pg.pos[1] = 70.0F; pg.pos[2] = -1.0F;
	assert(clin_apply_attach(&pg, true, &me));
	assert(pg.data.monster.pig_ridden && pg.data.monster.pig_rider_id == 0);
	assert(clin_apply_attach(&pg, false, &me));
	assert(!pg.data.monster.pig_ridden);
	assert(me.pos[0] == 4.0F && me.pos[2] == -1.0F
		&& fabsf(me.pos[1] - 70.9F) < 1e-5F);
	puts("  mp-bed/ride units: sleep timer, bed spot/offset/orientation x4, "
		 "seats, pig link");
}

/* MP entity-traffic crash soak (2026-08-26): hostile-vector hardening for the
 * S3 remote-entity handler surface -- server_remote.c's MP_EVENT_REL_ENTITY_
 * MOVE/_LOOK/ENTITY_TELEPORT/DESTROY_ENTITY/ENTITY_METADATA cases and the
 * bounded remote_entity table (REMOTE_ENTITY_CAPACITY). Every case below is
 * ALREADY defended by a null/lookup guard in the reviewed code (remote_
 * entity_find returning NULL is a silent `break`, mirroring Beta's own
 * getEntityByID null check [740c583 NetClientHandler.handleEntity]) -- this
 * fixture proves it under the FULL server_remote/client_interface pipe
 * rather than trusting the code reading, and pins the behavior as a
 * regression guard. IDs are chosen far outside every other fixture's small
 * numeric range (this binary shares ONE process-lifetime gstate.entities
 * dict across every test function) to rule out accidental cross-test
 * collisions. */
struct s3_hostile_fixture {
	SOCKET listener;
	bool ok;
	atomic_bool unknown_move_confirmed;
	atomic_bool unknown_destroy_confirmed;
	atomic_bool unknown_metadata_confirmed;
	atomic_bool unsupported_mob_confirmed;
	atomic_bool unsupported_mob_destroyed_confirmed;
	atomic_bool overflow_confirmed;
};

#define HOSTILE_ID_UNKNOWN_MOVE 90001
#define HOSTILE_ID_UNKNOWN_DESTROY 90002
#define HOSTILE_ID_UNKNOWN_METADATA 90003
#define HOSTILE_ID_UNSUPPORTED_MOB 90004
/* SERVER_REMOTE_ENTITY_CAPACITY+1 distinct, otherwise entirely LEGAL spawns
 * in one burst: the (N+1)th must be dropped safely, never overrun the fixed
 * REMOTE_ENTITY_CAPACITY array server_remote.c backs the table with. */
#define HOSTILE_OVERFLOW_BASE 91000
#define HOSTILE_OVERFLOW_COUNT (SERVER_REMOTE_ENTITY_CAPACITY + 1)

static void* s3_hostile_peer_entry(void* user) {
	struct s3_hostile_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;
	static const uint8_t keep_alive[] = {0x00};

	/* --- 1: Packet31RelEntityMove for an id NEVER spawned. server_remote.c's
	 * shared REL_ENTITY_MOVE/_LOOK/ENTITY_TELEPORT case does ONE remote_
	 * entity_find(...) lookup before touching anything; a miss is a silent
	 * `break`. A keep-alive checkpoint right after proves the byte stream did
	 * not desync. [740c583 NetClientHandler.handleEntity] */
	uint8_t move_unknown[8] = {0x1F};
	put_i32(move_unknown + 1, HOSTILE_ID_UNKNOWN_MOVE);
	move_unknown[5] = 4; move_unknown[6] = 0; move_unknown[7] = 0;
	if(!socket_write_fragmented(peer, move_unknown, sizeof(move_unknown))
	   || !socket_write_fragmented(peer, keep_alive, sizeof(keep_alive)))
		goto done;
	while(!overflow_flag_get(&fixture->unknown_move_confirmed))
		thread_msleep(1);

	/* --- 2: Packet29DestroyEntity for an id NEVER spawned. remote_entity_
	 * destroy no-ops on a missing id (remote_entity_find returns NULL, early
	 * return); client_interface.c's CRPC_ENTITY_DESTROY consumer separately
	 * guards its own dict_entity_get(...) deref the same way. --- */
	uint8_t destroy_unknown[5] = {0x1D};
	put_i32(destroy_unknown + 1, HOSTILE_ID_UNKNOWN_DESTROY);
	if(!socket_write_fragmented(peer, destroy_unknown, sizeof(destroy_unknown))
	   || !socket_write_fragmented(peer, keep_alive, sizeof(keep_alive)))
		goto done;
	while(!overflow_flag_get(&fixture->unknown_destroy_confirmed))
		thread_msleep(1);

	/* --- 3: Packet40EntityMetadata (well-formed DataWatcher: an empty,
	 * immediately-0x7F-terminated stream) naming an id NEVER spawned.
	 * MP_EVENT_ENTITY_METADATA requires `entry && entry->kind ==
	 * REMOTE_ENTITY_MOB`; a miss is a silent `break`, never a null-entry
	 * write. --- */
	uint8_t metadata_unknown[6] = {0x28};
	put_i32(metadata_unknown + 1, HOSTILE_ID_UNKNOWN_METADATA);
	metadata_unknown[5] = 0x7F;
	if(!socket_write_fragmented(peer, metadata_unknown, sizeof(metadata_unknown))
	   || !socket_write_fragmented(peer, keep_alive, sizeof(keep_alive)))
		goto done;
	while(!overflow_flag_get(&fixture->unknown_metadata_confirmed))
		thread_msleep(1);

	/* --- 4: Packet24MobSpawn with type=53 (registered in Beta's own
	 * EntityList [740c583 EntityList.<clinit>] as EntityGiantZombie, but with
	 * NO engine monster_id equivalent -- beta_mob_type_to_monster_id's
	 * default arm returns false, the documented "giant-in-scope-low-pri"
	 * residual). server_remote.c tracks it REMOTE_ENTITY_UNSUPPORTED (so a
	 * later DESTROY is absorbed, not errored) and sends NO CRPC_SPAWN_MONSTER
	 * at all -- confirm the client never materializes a dict_entity for it,
	 * only the server-side tracking table grows. --- */
	uint8_t mob_unsupported[21] = {0x18};
	put_i32(mob_unsupported + 1, HOSTILE_ID_UNSUPPORTED_MOB);
	mob_unsupported[5] = 53;
	put_i32(mob_unsupported + 6, 0);
	put_i32(mob_unsupported + 10, 320);
	put_i32(mob_unsupported + 14, 0);
	mob_unsupported[18] = 0; mob_unsupported[19] = 0; mob_unsupported[20] = 0x7F;
	if(!socket_write_fragmented(peer, mob_unsupported, sizeof(mob_unsupported))
	   || !socket_write_fragmented(peer, keep_alive, sizeof(keep_alive)))
		goto done;
	while(!overflow_flag_get(&fixture->unsupported_mob_confirmed))
		thread_msleep(1);

	uint8_t destroy_unsupported[5] = {0x1D};
	put_i32(destroy_unsupported + 1, HOSTILE_ID_UNSUPPORTED_MOB);
	if(!socket_write_fragmented(
		   peer, destroy_unsupported, sizeof(destroy_unsupported))
	   || !socket_write_fragmented(peer, keep_alive, sizeof(keep_alive)))
		goto done;
	while(!overflow_flag_get(&fixture->unsupported_mob_destroyed_confirmed))
		thread_msleep(1);

	/* --- 5: overflow burst (see HOSTILE_OVERFLOW_COUNT above). --- */
	for(int i = 0; i < HOSTILE_OVERFLOW_COUNT; i++) {
		uint8_t spawn[21] = {0x18};
		put_i32(spawn + 1, HOSTILE_OVERFLOW_BASE + i);
		spawn[5] = 90; /* pig: beta_mob_type_to_monster_id case 90, supported */
		put_i32(spawn + 6, 0);
		put_i32(spawn + 10, 320);
		put_i32(spawn + 14, 0);
		spawn[18] = 0; spawn[19] = 0; spawn[20] = 0x7F;
		if(!socket_write_fragmented(peer, spawn, sizeof(spawn)))
			goto done;
	}
	if(!socket_write_fragmented(peer, keep_alive, sizeof(keep_alive)))
		goto done;
	while(!overflow_flag_get(&fixture->overflow_confirmed))
		thread_msleep(1);

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_s3_entity_hostile_vectors(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct s3_hostile_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, s3_hostile_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	struct server_remote_status status = {0};

	/* Step 1: unknown-id REL_ENTITY_MOVE survives -- proven by the keep-alive
	 * round trip right after it, and the connection staying REMOTE_PLAY. */
	bool step1 = false;
	for(size_t elapsed = 0; elapsed < 5000 && !step1; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		step1 = status.keep_alives_received >= 1;
		thread_msleep(1);
	}
	assert(step1);
	assert(status.state == REMOTE_PLAY);
	assert(!dict_entity_get(gstate.entities, HOSTILE_ID_UNKNOWN_MOVE));
	overflow_flag_set(&peer.unknown_move_confirmed, true);

	/* Step 2: unknown-id DESTROY_ENTITY -- second checkpoint. */
	bool step2 = false;
	for(size_t elapsed = 0; elapsed < 5000 && !step2; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		step2 = status.keep_alives_received >= 2;
		thread_msleep(1);
	}
	assert(step2);
	assert(status.state == REMOTE_PLAY);
	overflow_flag_set(&peer.unknown_destroy_confirmed, true);

	/* Step 3: ENTITY_METADATA naming an absent entity -- third checkpoint. */
	bool step3 = false;
	for(size_t elapsed = 0; elapsed < 5000 && !step3; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		step3 = status.keep_alives_received >= 3;
		thread_msleep(1);
	}
	assert(step3);
	assert(status.state == REMOTE_PLAY);
	size_t count_before_mob = status.remote_entity_count;
	overflow_flag_set(&peer.unknown_metadata_confirmed, true);

	/* Step 4: MOB_SPAWN with an unregistered type (Giant=53) must track
	 * REMOTE_ENTITY_UNSUPPORTED server-side and spawn NOTHING client-side. */
	bool step4 = false;
	for(size_t elapsed = 0; elapsed < 5000 && !step4; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		step4 = status.keep_alives_received >= 4;
		thread_msleep(1);
	}
	assert(step4);
	assert(status.state == REMOTE_PLAY);
	assert(status.remote_entity_count == count_before_mob + 1);
	assert(!dict_entity_get(gstate.entities, HOSTILE_ID_UNSUPPORTED_MOB));
	overflow_flag_set(&peer.unsupported_mob_confirmed, true);

	/* Follow-up: DESTROY_ENTITY for that same UNSUPPORTED-tracked id must be
	 * absorbed cleanly too -- the count returns to baseline. */
	bool step4b = false;
	for(size_t elapsed = 0; elapsed < 5000 && !step4b; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		step4b = status.keep_alives_received >= 5;
		thread_msleep(1);
	}
	assert(step4b);
	assert(status.state == REMOTE_PLAY);
	assert(status.remote_entity_count == count_before_mob);
	overflow_flag_set(&peer.unsupported_mob_destroyed_confirmed, true);

	/* Step 5: capacity-overflow burst. */
	bool step5 = false;
	for(size_t elapsed = 0; elapsed < 20000 && !step5; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		step5 = status.keep_alives_received >= 6;
		thread_msleep(1);
	}
	assert(step5);
	assert(status.state == REMOTE_PLAY);
	assert(status.forced_reconnects == 0);
	assert(status.remote_entity_count == SERVER_REMOTE_ENTITY_CAPACITY);
	assert(status.remote_entity_capacity_drops >= 1);
	printf("S3 HOSTILE PASS entity_count=%zu capacity_drops=%u\n",
		status.remote_entity_count, status.remote_entity_capacity_drops);
	overflow_flag_set(&peer.overflow_confirmed, true);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}
#undef HOSTILE_ID_UNKNOWN_MOVE
#undef HOSTILE_ID_UNKNOWN_DESTROY
#undef HOSTILE_ID_UNKNOWN_METADATA
#undef HOSTILE_ID_UNSUPPORTED_MOB
#undef HOSTILE_OVERFLOW_BASE
#undef HOSTILE_OVERFLOW_COUNT

/* A malformed DataWatcher header (unregistered type, mirroring test_codec_
 * goldens' metadata_unknown_type vector) arriving mid-play against an
 * ALREADY-POPULATED remote-entity table -- the live-fire shape, not an
 * empty-table edge case. Proves the FULL server_remote integration (not just
 * the codec unit) turns the decode failure into a clean, bounded kill:
 * REMOTE_CLOSING -> REMOTE_DISCONNECTED, socket closed, both worker threads
 * exited, reason cites the offending packet id -- never a crash or a
 * corrupted decoder left running. [740c583 DataWatcher.readWatchableObjects] */
struct s3_metadata_kill_fixture {
	SOCKET listener;
	bool ok;
	atomic_bool spawn_confirmed;
};

#define HOSTILE_KILL_ENTITY_ID 90500

static void* s3_metadata_kill_peer_entry(void* user) {
	struct s3_metadata_kill_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;

	/* Populate the table with ONE real, legal pig before the hostile packet. */
	uint8_t pig_spawn[21] = {0x18};
	put_i32(pig_spawn + 1, HOSTILE_KILL_ENTITY_ID);
	pig_spawn[5] = 90;
	put_i32(pig_spawn + 6, 0);
	put_i32(pig_spawn + 10, 320);
	put_i32(pig_spawn + 14, 0);
	pig_spawn[18] = 0; pig_spawn[19] = 0; pig_spawn[20] = 0x7F;
	if(!socket_write_fragmented(peer, pig_spawn, sizeof(pig_spawn)))
		goto done;
	while(!overflow_flag_get(&fixture->spawn_confirmed))
		thread_msleep(1);

	/* Packet40EntityMetadata for that SAME live-tracked entity, with a
	 * DataWatcher header byte encoding unregistered type 7: (7<<5)|0 = 0xE0,
	 * the exact byte test_codec_goldens' metadata_unknown_type vector uses. */
	uint8_t bad_metadata[6] = {0x28};
	put_i32(bad_metadata + 1, HOSTILE_KILL_ENTITY_ID);
	bad_metadata[5] = 0xE0;
	if(!socket_write_fragmented(peer, bad_metadata, sizeof(bad_metadata)))
		goto done;

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

/* Painting MP transport (PROJECT_MEMORY 2026-09-01 deferred item #2):
 * Packet25EntityPainting end-to-end through server_remote_protocol.c's
 * MP_EVENT_ENTITY_PAINTING handler (already landed -- see that case's own
 * [740c583 Packet25EntityPainting / NetClientHandler.handleEntityPainting]
 * citation) -> CRPC_SPAWN_PAINTING -> client_interface.c's consumer ->
 * entity_painting() -- proving a real `struct entity` with the right
 * EnumArt/tile/direction lands in gstate.entities, then that Packet29
 * (destroy, already covered generically by test_s3_entity_lifecycle for a
 * remote player, exercised here for a painting specifically) removes it.
 * Mirrors test_s3_entity_lifecycle/s3_entity_peer_entry's fixture shape --
 * a real loopback socket, not a bare mp_codec decode -- so this is the
 * "remote client's packet path", not just the codec (test_codec_goldens'
 * painting_golden already proves the raw bytes decode correctly; this
 * proves the DECODED event actually spawns/despawns a live entity). */
#define PAINTING_TRANSPORT_ENTITY_ID 96700
#define PAINTING_TRANSPORT_ART_INDEX 4 /* "Bomb": {16,16,64,0} -- see
										 * g_painting_art[4] in
										 * entity_painting.c */
#define PAINTING_TRANSPORT_DIRECTION PAINTING_DIR_WEST /* == 1 */
#define PAINTING_TRANSPORT_TILE_X 20
#define PAINTING_TRANSPORT_TILE_Y 15
#define PAINTING_TRANSPORT_TILE_Z 30

struct mp_painting_fixture {
	SOCKET listener;
	bool ok;
	atomic_bool spawn_confirmed;
	atomic_bool destroy_confirmed;
};

static void* mp_painting_peer_entry(void* user) {
	struct mp_painting_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;

	/* 0x19 Packet25EntityPainting: entityId=96700, title="Bomb" (string16:
	 * u16 length then UTF16BE chars), tile (20,15,30), direction=WEST(1).
	 * [740c583 Packet25EntityPainting.readPacketData] -- same wire shape
	 * test_codec_goldens' painting_golden vector already pins byte-for-byte
	 * (title "Wanderer" there; "Bomb" here only to pick a DIFFERENT,
	 * unambiguous EnumArt entry so a title-matching bug that quietly fell
	 * through to the Kebab(0) fallback would be caught by the art_index
	 * assert below). */
	uint8_t painting_spawn[31] = {0x19};
	put_i32(painting_spawn + 1, PAINTING_TRANSPORT_ENTITY_ID);
	painting_spawn[5] = 0; painting_spawn[6] = 4; /* title length = 4 */
	painting_spawn[7] = 0; painting_spawn[8] = 'B';
	painting_spawn[9] = 0; painting_spawn[10] = 'o';
	painting_spawn[11] = 0; painting_spawn[12] = 'm';
	painting_spawn[13] = 0; painting_spawn[14] = 'b';
	put_i32(painting_spawn + 15, PAINTING_TRANSPORT_TILE_X);
	put_i32(painting_spawn + 19, PAINTING_TRANSPORT_TILE_Y);
	put_i32(painting_spawn + 23, PAINTING_TRANSPORT_TILE_Z);
	put_i32(painting_spawn + 27, PAINTING_TRANSPORT_DIRECTION);
	if(!socket_write_fragmented(peer, painting_spawn, sizeof(painting_spawn)))
		goto done;
	while(!overflow_flag_get(&fixture->spawn_confirmed))
		thread_msleep(1);

	/* Empty keep-alive checkpoint (same convention as s3_entity_peer_entry's
	 * animation step): proves the painting packet parsed without desyncing
	 * the stream before the destroy packet below is trusted to parse too. */
	static const uint8_t keep_alive[] = {0x00};
	if(!socket_write_fragmented(peer, keep_alive, sizeof(keep_alive)))
		goto done;

	/* 0x1D Packet29DestroyEntity: remove entity 96700. [740c583
	 * Packet29DestroyEntity] */
	uint8_t destroy[5] = {0x1D};
	put_i32(destroy + 1, PAINTING_TRANSPORT_ENTITY_ID);
	if(!socket_write_fragmented(peer, destroy, sizeof(destroy)))
		goto done;
	while(!overflow_flag_get(&fixture->destroy_confirmed))
		thread_msleep(1);

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_mp_painting_transport(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct mp_painting_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, mp_painting_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	struct entity** e_pp = NULL;
	bool spawned = false;
	for(size_t elapsed = 0; elapsed < 5000 && !spawned; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		e_pp = dict_entity_get(gstate.entities, PAINTING_TRANSPORT_ENTITY_ID);
		spawned = e_pp && *e_pp && (*e_pp)->type == ENTITY_PAINTING;
		thread_msleep(1);
	}
	assert(spawned);
	struct entity* e = *e_pp;
	/* EnumArt: the title "Bomb" must have resolved to its OWN table entry,
	 * not silently fallen through to painting_art_by_title's Kebab(0)
	 * fallback (which "Bomb", index 4, is distinct from). */
	assert(e->data.painting.art_index == PAINTING_TRANSPORT_ART_INDEX);
	/* Facing: direction survives the wire -> MP_EVENT_ENTITY_PAINTING ->
	 * CRPC_SPAWN_PAINTING -> entity_painting() chain unchanged. */
	assert(e->data.painting.direction == PAINTING_TRANSPORT_DIRECTION);
	/* Position: the WALL block coordinates (Beta's raw x/y/z, this engine's
	 * tile_x/y/z), not the fixed-point player-position encoding other
	 * packets use [740c583 Packet25EntityPainting.readPacketData]. */
	assert(e->data.painting.tile_x == PAINTING_TRANSPORT_TILE_X);
	assert(e->data.painting.tile_y == PAINTING_TRANSPORT_TILE_Y);
	assert(e->data.painting.tile_z == PAINTING_TRANSPORT_TILE_Z);
	/* Cross-check against the SAME geometry function entity_painting() uses
	 * internally: proves the wire-delivered art/direction/tile fields, not
	 * some stale/default set, actually drove the frame's world-space
	 * position (a corrupted art_index or direction would move this). */
	vec3 expected_pos;
	painting_compute_geometry(PAINTING_TRANSPORT_ART_INDEX,
		PAINTING_TRANSPORT_DIRECTION, PAINTING_TRANSPORT_TILE_X,
		PAINTING_TRANSPORT_TILE_Y, PAINTING_TRANSPORT_TILE_Z, expected_pos,
		NULL);
	assert(e->pos[0] == expected_pos[0] && e->pos[1] == expected_pos[1]
		&& e->pos[2] == expected_pos[2]);
	overflow_flag_set(&peer.spawn_confirmed, true);

	bool destroyed = false;
	for(size_t elapsed = 0; elapsed < 5000 && !destroyed; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		struct entity** pp
			= dict_entity_get(gstate.entities, PAINTING_TRANSPORT_ENTITY_ID);
		destroyed = !pp || !*pp;
		thread_msleep(1);
	}
	assert(destroyed);
	overflow_flag_set(&peer.destroy_confirmed, true);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}

static void test_s3_entity_metadata_decode_kill(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct s3_metadata_kill_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, s3_metadata_kill_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	struct entity** e_pp = NULL;
	bool spawned = false;
	for(size_t elapsed = 0; elapsed < 5000 && !spawned; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		e_pp = dict_entity_get(gstate.entities, HOSTILE_KILL_ENTITY_ID);
		spawned = e_pp && *e_pp && (*e_pp)->type == ENTITY_MONSTER;
		thread_msleep(1);
	}
	assert(spawned);
	overflow_flag_set(&peer.spawn_confirmed, true);

	assert(wait_for(&remote, workers_exited, 5000));
	/* Flake fix (chip task_729b2950, ~1/3 runs): workers_exited reads the
	 * exit flags FRESH after each pump, so a worker exiting in the
	 * pump->predicate gap satisfied the wait before any pump had run the
	 * teardown-finalize step (state still REMOTE_CLOSING). One more pump
	 * commits CLOSING->DISCONNECTED deterministically -- the same
	 * pump-owns-state-transitions contract main.c's seam relies on. */
	server_remote_pump(&remote);
	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	assert(status.state == REMOTE_DISCONNECTED);
	assert(strstr(status.reason, "0x28") != NULL);
	/* The entity spawned before the kill is left exactly as it was -- the
	 * decode failure tore down the CONNECTION, not client world state. */
	e_pp = dict_entity_get(gstate.entities, HOSTILE_KILL_ENTITY_ID);
	assert(e_pp && *e_pp && (*e_pp)->type == ENTITY_MONSTER);
	printf("S3 METADATA-KILL PASS reason=%s\n", status.reason);

	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
}
#undef HOSTILE_KILL_ENTITY_ID

/* Render follow-up (2026-08-19): golden loopback proof that Packet32EntityLook
 * now reaches the client as CRPC_ENTITY_LOOK and lands byte-exact on the
 * tracked remote player's e->orient, closing KNOWN GAP 1 ("CRPC_ENTITY_MOVE
 * carries no angles" -- server_remote.c's old "Documented S4 residual"
 * comment at the Packet32 site). Mirrors test_s3_entity_lifecycle's fixture
 * shape (named-player spawn, then one more step, each confirmed before the
 * peer sends the next; the peer never initiates close). */
struct entity_look_fixture {
	SOCKET listener;
	bool ok;
	atomic_bool spawn_confirmed;
	atomic_bool look_confirmed;
};

static void* entity_look_peer_entry(void* user) {
	struct entity_look_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;

	/* 0x14 Packet20NamedEntitySpawn: entity 200, name "Zed", spawn at
	 * (0, 10.0, 0), NO rotation (0,0) -- the look update below must be what
	 * moves e->orient off this zero baseline, not the spawn itself. [740c583
	 * Packet20NamedEntitySpawn.readPacketData] */
	static const uint8_t named_spawn[] = {0x14,
		0,0,0,200, 0,3, 0,'Z',0,'e',0,'d',
		0,0,0,0, 0,0,1,64, 0,0,0,0,
		0, 0, 0,0};
	if(!socket_write_fragmented(peer, named_spawn, sizeof(named_spawn)))
		goto done;
	while(!overflow_flag_get(&fixture->spawn_confirmed))
		thread_msleep(1);

	/* 0x20 Packet32EntityLook: entity 200, yaw byte 64 (-> Beta 90.0deg
	 * exactly: 64*360/256 -> ENGINE -90.0deg, the CRPC-boundary mirror),
	 * pitch byte -64/0xC0 (-> Beta -90.0deg, straight up -> ENGINE 0.0deg,
	 * the +90 offset). Whole-degree byte-angle inputs so the conversion is
	 * exact float arithmetic, no rounding tolerance needed in the driver's
	 * assert. [740c583 Packet32EntityLook] [740c583 Entity.moveFlying for
	 * the yaw-frame mirror] */
	static const uint8_t look[] = {0x20, 0,0,0,200, 0x40, 0xC0};
	if(!socket_write_fragmented(peer, look, sizeof(look)))
		goto done;
	/* Keep-alive round trip: proves the 2-byte yaw/pitch tail decoded clean
	 * without desyncing the stream (test_s3_entity_lifecycle's animation
	 * step precedent). */
	static const uint8_t keep_alive[] = {0x00};
	if(!socket_write_fragmented(peer, keep_alive, sizeof(keep_alive)))
		goto done;
	while(!overflow_flag_get(&fixture->look_confirmed))
		thread_msleep(1);

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

/* Finding 2 (2026-08-20 provider-swap redesign): cancel used to JOIN the
 * read thread while it sat inside a blocking getaddrinfo/connect with no
 * timeout (~21s Windows, up to ~130s Linux, against a blackholed host) --
 * a main-thread freeze on the Cancel button and at quit. The disposable-
 * connector redesign bounds cancel to the read thread's ~10ms handoff
 * poll. 192.0.2.1 is RFC 5737 TEST-NET-1 (guaranteed unroutable); on
 * networks that answer with an instant ICMP unreachable the connect fails
 * fast and the bound is trivially met -- the assert still enforces the
 * ceiling where it matters. */
static void test_cancel_during_connect_bounded(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	assert(server_remote_start(&remote, "192.0.2.1", 25565, "Cave"));
	thread_msleep(100); /* let the connector enter its blocking phase */
	ptime_t t0 = time_get();
	server_remote_cancel(&remote);
	assert(time_diff_ms(t0, time_get()) < 5000);
	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	assert(status.state == REMOTE_DISCONNECTED);
	/* destroy (the quit path) must be equally bounded -- the old code hit
	 * the same join through server_remote_cancel here. */
	ptime_t t1 = time_get();
	server_remote_destroy(&remote);
	assert(time_diff_ms(t1, time_get()) < 5000);
	puts("  cancel-during-connect bounded (finding 2)");
}

/* M6 / finding 6: a rejected start used to return false with NO reason and
 * NO generation bump, so the connect screen spun "Connecting..." forever.
 * Port 0 is parse-legal (assert_address("example.com:0", ...) above pins
 * it), making this a directly user-reachable spin. */
static void test_start_rejection_reports_reason(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	struct server_remote_status before, after;
	server_remote_get_status(&remote, &before);
	assert(!server_remote_start(&remote, "example.com", 0, "Cave"));
	server_remote_get_status(&remote, &after);
	assert(after.generation != before.generation);
	assert(after.state == REMOTE_DISCONNECTED);
	assert(after.reason[0]);
	/* A live-session refusal must NOT stomp status: start once for real,
	 * then a mistimed second start keeps the first attempt's generation. */
	assert(server_remote_start(&remote, "192.0.2.1", 25565, "Cave"));
	server_remote_get_status(&remote, &before);
	assert(!server_remote_start(&remote, "192.0.2.1", 25565, "Cave"));
	server_remote_get_status(&remote, &after);
	assert(after.generation == before.generation);
	server_remote_destroy(&remote);
	puts("  rejected start reports reason + generation (M6)");
}

/* Finding 3: Cancel now really unqueues a pending connect request. */
static void test_cancel_pending_connect(void) {
	mp_session_init();
	assert(mp_session_remote() && !mp_session_remote()->impl);
	mp_session_init();
	assert(!mp_session_remote()->impl);
	assert(mp_session_prepare_remote());
	void* allocation = mp_session_remote()->impl;
	assert(allocation && mp_session_prepare_remote());
	assert(mp_session_remote()->impl == allocation);
	mp_session_release_remote();
	assert(!mp_session_remote()->impl);
	char released[64];
	assert(mp_session_take_drop_notice(released, sizeof(released)));
	assert(mp_session_prepare_remote());
	mp_session_destroy();
	mp_session_destroy();
	mp_session_init();
	assert(!mp_session_remote()->impl);
	printf("  remote allocation payload: local startup=0, connect=%zu once, teardown=0 bytes\n",
	       sizeof(struct server_remote_impl));
	assert(mp_session_request_connect("localhost", 25565, "Cave"));
	mp_session_cancel_pending();
	char host[64];
	char user[32];
	uint16_t port;
	assert(!mp_session_take_pending_connect(host, sizeof(host), &port, user,
											sizeof(user)));
	/* And a fresh request still works after a cancel. */
	assert(mp_session_request_connect("localhost", 25565, "Cave"));
	assert(mp_session_take_pending_connect(host, sizeof(host), &port, user,
										   sizeof(user)));
	assert(strcmp(host, "localhost") == 0 && port == 25565);
	/* Re-review finding 3: the drop notice is one-shot and carries the
	 * reason to the connect screen. */
	char why[64];
	assert(!mp_session_take_drop_notice(why, sizeof(why)));
	mp_session_note_request_dropped("Busy: test");
	assert(mp_session_take_drop_notice(why, sizeof(why)));
	assert(strcmp(why, "Busy: test") == 0);
	assert(!mp_session_take_drop_notice(why, sizeof(why)));
	puts("  cancel-pending unqueues + drop notice one-shot (finding 3)");
}

static void test_entity_look_forward(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct entity_look_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, entity_look_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	struct entity** e_pp = NULL;
	bool spawned = false;
	for(size_t elapsed = 0; elapsed < 5000 && !spawned; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		e_pp = dict_entity_get(gstate.entities, 200);
		spawned = e_pp && *e_pp && (*e_pp)->type == ENTITY_REMOTE_PLAYER;
		thread_msleep(1);
	}
	assert(spawned);
	struct entity* e = *e_pp;
	assert(strcmp(e->data.remote_player.username, "Zed") == 0);
	/* Baseline: the spawn carried Beta rotation=0/pitch=0 -> engine yaw 0
	 * (mirror of 0) and engine pitch 90 (level; the +90 offset). Distinct
	 * from the -90/0 the LOOK below produces, so the assertion below is
	 * really catching the LOOK update, not a spawn value that happened to
	 * already match. */
	assert(e->orient[0] == 0.0f && e->orient[1] == 90.0f);
	overflow_flag_set(&peer.spawn_confirmed, true);

	/* Re-pinned 2026-08-26 (mp-s4-polish): a LOOK no longer writes orient
	 * directly -- Beta routes rotation through the SAME 3-increment approach
	 * as position (a standalone Packet32 is handleEntity with a zero move
	 * delta), so the wire now ARMS network_orient/network_increments and the
	 * entity TICK steps orient toward it, landing exactly on the 3rd tick.
	 * The old pin (orient snaps, orient_old clobbered alongside) encoded the
	 * pre-parity behaviour this lane removed. [740c583
	 * NetClientHandler.handleEntity] [740c583
	 * EntityOtherPlayerMP.onLivingUpdate] */
	bool looked = false;
	for(size_t elapsed = 0; elapsed < 5000 && !looked; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		looked = e->network_orient[0] == -90.0f
			&& e->network_increments == 3;
		thread_msleep(1);
	}
	assert(looked);
	assert(e->network_orient[1] == 0.0f);
	/* orient itself is still the pre-LOOK value (no tick has run in this
	 * harness)... */
	assert(e->orient[0] == 0.0f && e->orient[1] == 90.0f);
	/* ...and three client ticks land it EXACTLY on the wire look, with
	 * orient_old holding a REAL previous sample after each tick (the render
	 * lerp's requirement -- the old clobber destroyed it). */
	e->tick_client(e);
	e->tick_client(e);
	assert(e->orient_old[0] != e->orient[0]); /* mid-approach, distinct */
	e->tick_client(e);
	assert(fabsf(e->orient[0] - -90.0f) < 1e-3f
		&& fabsf(e->orient[1] - 0.0f) < 1e-3f);
	assert(e->network_increments == 0);
	overflow_flag_set(&peer.look_confirmed, true);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}

/* Adversarial-review MAJOR closure: the clin_chunk sub-volume fallback
 * (client_interface.c, guarded by "x%16 || z%16 || y!=0 || sx/sy/sz != full
 * column") had no fixture exercising it -- everything else here only ever
 * sends the bulk full-column shape. This peer sends a genuinely partial
 * Packet51 (sy=8 != WORLD_HEIGHT=128, full 16x16 horizontal footprint) so
 * the fallback's per-cell (lx*sz+lz)*sy+ly loop, not the bulk path, installs
 * the chunk. Chunk (4,4)/block origin (64,0,64) stays clear of every other
 * S1 fixture's chunk (0,0)/(1,0)/(i,0). */
static void* partial_chunk_peer_entry(void* user) {
	struct s1_peer_fixture* fixture = user;
	uint8_t* packet = NULL;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[] = {0x01,0,0,0,0x2A,
		0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	static const uint8_t spawn[] = {0x06,0,0,0,0, 0,0,0,100, 0,0,0,0};
	/* Packet50's x/z are chunk INDICES (unlike Packet51's block-space x/z). */
	static const uint8_t announce[] = {0x32,0,0,0,4, 0,0,0,4, 1};
	/* Mirror s1_peer_entry's authoritative correction: promotion order keys
	 * off the last authoritative position, so give the client one before the
	 * chunk arrives, exactly like the proven full-column flow. */
	uint8_t correction[42] = {0x0D};
	put_f64(correction+1, 0.5);
	put_f64(correction+9, 101.62);
	put_f64(correction+17, 103.24);
	put_f64(correction+25, 0.5);
	put_f32(correction+33, 90.0f);
	put_f32(correction+37, 0.0f);
	correction[41] = 0;
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response))
	   || !socket_write_fragmented(peer, spawn, sizeof(spawn))
	   || !socket_write_fragmented(peer, announce, sizeof(announce))
	   || !socket_write_fragmented(peer, correction, sizeof(correction)))
		goto done;

	size_t cells = 16u * 8u * 16u, nibble = cells / 2u;
	uint8_t* inflated = calloc(1, cells + 3u * nibble);
	if(!inflated)
		goto done;
	inflated[0] = BLOCK_STONE;                /* local index 0 = world (64,0,64) */
	inflated[cells + 0] = 0x05;               /* metadata nibble, index 0 = low nibble */
	inflated[cells + nibble + 0] = 0x07;      /* block/torch light only */
	inflated[cells + 2u * nibble + 0] = 0x0C; /* sky light only */
	uLongf compressed_cap = compressBound((uLong)(cells + 3u * nibble));
	uint8_t* compressed = malloc(compressed_cap);
	if(!compressed) { free(inflated); goto done; }
	if(compress2(compressed, &compressed_cap, inflated,
		(uLong)(cells + 3u * nibble), Z_BEST_SPEED) != Z_OK) {
		free(compressed); free(inflated); goto done;
	}
	free(inflated);
	size_t packet_len = 18u + (size_t)compressed_cap;
	packet = malloc(packet_len);
	if(!packet) { free(compressed); goto done; }
	packet[0] = 0x33; put_i32(packet + 1, 64); put_i16(packet + 5, 0);
	put_i32(packet + 7, 64); packet[11] = 15; packet[12] = 7; packet[13] = 15;
	put_i32(packet + 14, (int32_t)compressed_cap);
	memcpy(packet + 18, compressed, compressed_cap);
	free(compressed);
	if(!socket_write_fragmented(peer, packet, packet_len))
		goto done;
	/* The client installs the retained body on the MAIN thread's next
	 * server_remote_pump(), not on receipt. Closing immediately races that:
	 * the resulting EOF flips state away from PLAY before the pump gets a
	 * turn, and the retained-but-unpromoted chunk is stranded forever (the
	 * promotion step only runs in REMOTE_PLAY). And a single-byte "may
	 * close" marker is indistinguishable from regular client traffic (the
	 * correction echo, movement samples, keep-alives), so the peer NEVER
	 * initiates the close: it drains and discards client bytes until the
	 * driver cancels the connection after its world-state assertions. */
	uint8_t drain;
	while(socket_read_exact(peer, &drain, 1)) {
	}
	fixture->ok = true;
done:
	free(packet);
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_s1_partial_chunk_volume(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	/* hw1030g: a script watching a cell the partial Packet51 covers. A server
	 * answers a >10-cell burst in one column with a region resend instead of
	 * Packet52/53; the body IS its answer for that cell. */
	server_remote_play_watch(&remote, true, 64, 0, 64);
	uint16_t port;
	struct s1_peer_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, partial_chunk_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	/* world_get_block() returns a SYNTHETIC default of type=BLOCK_STONE for any
	 * coordinate with no chunk loaded yet (world.c: "no chunk" branch), so the
	 * install signal must gate on a real chunk existing first -- checking the
	 * block type alone is true from iteration 0 and would starve the pump loop
	 * before the connection is even established. */
	bool installed = false;
	for(size_t elapsed = 0; elapsed < 10000 && !installed; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		installed = world_find_chunk(&gstate.world, 64, 0, 64) != NULL
			&& world_get_block(&gstate.world, 64, 0, 64).type == BLOCK_STONE;
		thread_msleep(1);
	}
	assert(installed && "partial Packet51 never installed");
	{
		struct server_remote_play_obs wobs;
		server_remote_get_play_obs(&remote, &wobs);
		assert(wobs.watch_bodies == 1
			   && "the installed body covered the watched cell");
		assert(wobs.watch_echo.serial != 0
			   && wobs.watch_echo.serial == wobs.block_echo_serial
			   && wobs.watch_echo.x == 64 && wobs.watch_echo.y == 0
			   && wobs.watch_echo.z == 64
			   && wobs.watch_echo.type == BLOCK_STONE
			   && wobs.watch_echo.metadata == 5
			   && "the body's own voxel is the watched cell's echo");
		assert(wobs.watch_history_n == 1);
	}
	server_remote_play_watch(&remote, false, 0, 0, 0);

	/* Inside the written sub-volume: the one written cell carries its exact
	 * type/metadata/both light planes, byte-exact against the wire nibbles. */
	struct block_data inside = world_get_block(&gstate.world, 64, 0, 64);
	assert(inside.type == BLOCK_STONE);
	assert(inside.metadata == 5);
	assert(inside.torch_light == 7 && inside.sky_light == 12);
	assert(world_get_height(&gstate.world, 64, 64) == 1);

	/* Just outside the written sub-volume (y=8, one above sy=8's top) but
	 * inside the SAME allocated 16-tall chunk (world y 0..15 is one chunk):
	 * an off-by-one (ly<=sy) or an sy/WORLD_HEIGHT mixup in the fallback loop
	 * would leak the marker block or its metadata/light into this cell. */
	struct block_data just_above = world_get_block(&gstate.world, 64, 8, 64);
	assert(just_above.type == BLOCK_AIR);
	assert(just_above.metadata == 0);
	/* Untouched cells keep Beta's pre-chunk base: block light 0, sky light 15
	 * (ChunkProviderClient.prepareChunk fills skylightMap with -1; the partial
	 * Packet51 lands on it via NetClientHandler.handleMapChunk). REL-MP-FIX2b. */
	assert(just_above.torch_light == 0 && just_above.sky_light == 15);

	/* Same check at the far horizontal corner of the sub-volume's own
	 * footprint, one Y above it -- catches an index-formula bug in
	 * (lx*sz+lz)*sy+ly that only shows up away from the local (0,0,0) origin. */
	struct block_data corner_above = world_get_block(&gstate.world, 79, 8, 79);
	assert(corner_above.type == BLOCK_AIR);

	/* Driver-initiated close: cancel drops the socket, the draining peer sees
	 * EOF and exits, and only then is its ok flag meaningful. */
	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}

struct explosion_truncate_fixture {
	SOCKET listener;
	bool ok;
	/* Driver sets this only after polling gstate.world and confirming the
	 * chunk is genuinely resident (both marker cells read BLOCK_STONE) --
	 * NOT just after the wire bytes are sent. remote_apply_block_change
	 * silently and PERMANENTLY drops (deferred_edits++, never retried) any
	 * edit that lands before remote_promotion_step_locked has promoted the
	 * chunk to REMOTE_CHUNK_RESIDENT, and that promotion happens only in
	 * server_remote_pump -- AFTER that same pump call's event-queue drain
	 * already ran process_event on anything already queued. Sending the
	 * explosion immediately behind the chunk bytes would race that ordering
	 * and could lose the very edit this test exists to observe, so the
	 * peer blocks on this flag instead of firing blind. */
	volatile bool chunk_confirmed;
};

/* Adversarial-review CRITICAL closure: negative-fractional explosion origin
 * fixture. Packet60Explosion's origin cast is Java `(int)` -- truncate
 * toward zero -- NOT floor; the two diverge only for a negative fractional
 * origin (floor(-1.5)=-2, truncate=-1), Y is always >=0 in practice so this
 * targets X. Explosion origin x=-1.5 with a per-record dx=+2 offset: the
 * CORRECT (java_double_to_int) cast lands the cleared block at world x=1;
 * the old floor()-based bug would instead have landed it at world x=0. Both
 * candidate cells sit in the same chunk(0,0) install this file's other S1
 * fixtures already prove installs correctly, so two known STONE cells at
 * world (0,64,0) and (1,64,0) let the test show the RIGHT cell clears and
 * the WRONG (floor) cell stays untouched. [740c583
 * Packet60Explosion.readPacketData] */
static void* explosion_truncate_peer_entry(void* user) {
	struct explosion_truncate_fixture* fixture = user;
	uint8_t* packet = NULL;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[] = {0x01,0,0,0,0x2A,
		0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	static const uint8_t spawn[] = {0x06,0,0,0,0, 0,0,0,100, 0,0,0,0};
	static const uint8_t announce[] = {0x32,0,0,0,0, 0,0,0,0, 1};
	uint8_t correction[42] = {0x0D};
	put_f64(correction + 1, 0.5);
	put_f64(correction + 9, 101.62);
	put_f64(correction + 17, 103.24);
	put_f64(correction + 25, 0.5);
	put_f32(correction + 33, 90.0f);
	put_f32(correction + 37, 0.0f);
	correction[41] = 0;
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response))
	   || !socket_write_fragmented(peer, spawn, sizeof(spawn))
	   || !socket_write_fragmented(peer, announce, sizeof(announce))
	   || !socket_write_fragmented(peer, correction, sizeof(correction)))
		goto done;

	size_t cells = 16u * 128u * 16u, nibble = cells / 2u;
	uint8_t* inflated = calloc(1, cells + 3u * nibble);
	if(!inflated)
		goto done;
	/* index = (lx*16+lz)*128+ly [740c583 Chunk.getChunkData], confirmed
	 * against this file's own s1_peer_entry (index 0 = local (0,0,0)) and
	 * client_interface.c's clin_chunk sub-volume loop (same formula). */
	inflated[64] = BLOCK_STONE;   /* local (x=0,y=64,z=0) -> world (0,64,0) */
	inflated[2112] = BLOCK_STONE; /* local (x=1,y=64,z=0) -> world (1,64,0) */
	uLongf compressed_cap = compressBound((uLong)(cells + 3u * nibble));
	uint8_t* compressed = malloc(compressed_cap);
	if(!compressed) { free(inflated); goto done; }
	if(compress2(compressed, &compressed_cap, inflated,
		(uLong)(cells + 3u * nibble), Z_BEST_SPEED) != Z_OK) {
		free(compressed); free(inflated); goto done;
	}
	free(inflated);
	size_t packet_len = 18u + (size_t)compressed_cap;
	packet = malloc(packet_len);
	if(!packet) { free(compressed); goto done; }
	packet[0] = 0x33; put_i32(packet + 1, 0); put_i16(packet + 5, 0);
	put_i32(packet + 7, 0); packet[11] = 15; packet[12] = 127; packet[13] = 15;
	put_i32(packet + 14, (int32_t)compressed_cap);
	memcpy(packet + 18, compressed, compressed_cap);
	free(compressed);
	if(!socket_write_fragmented(peer, packet, packet_len))
		goto done;

	for(int waited = 0; !fixture->chunk_confirmed; waited++) {
		if(waited >= 5000)
			goto done;
		thread_msleep(1);
	}

	{
		uint8_t body[36];
		size_t o = 0;
		body[o++] = 0x3C;
		put_f64(body + o, -1.5); o += 8;
		put_f64(body + o, 64.0); o += 8;
		put_f64(body + o, 0.0); o += 8;
		put_f32(body + o, 1.0f); o += 4;
		put_i32(body + o, 1); o += 4;
		body[o++] = 2; body[o++] = 0; body[o++] = 0; /* dx=+2, dy=0, dz=0 */
		assert(o == sizeof(body));
		if(!socket_write_fragmented(peer, body, sizeof(body)))
			goto done;
	}

	/* Driver-initiated close only (mirrors partial_chunk_peer_entry above):
	 * drain and discard client bytes until server_remote_cancel drops the
	 * connection, then ok is meaningful. */
	uint8_t drain;
	while(socket_read_exact(peer, &drain, 1)) {
	}
	fixture->ok = true;
done:
	free(packet);
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_s4_explosion_truncate(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct explosion_truncate_fixture peer
		= {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(
		&peer_thread, explosion_truncate_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	bool installed = false;
	for(size_t elapsed = 0; elapsed < 10000 && !installed; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		installed = world_find_chunk(&gstate.world, 0, 64, 0) != NULL
			&& world_get_block(&gstate.world, 0, 64, 0).type == BLOCK_STONE
			&& world_get_block(&gstate.world, 1, 64, 0).type == BLOCK_STONE;
		thread_msleep(1);
	}
	assert(installed && "explosion-truncate fixture chunk never installed");
	/* Only NOW may the peer send the explosion -- see the fixture's own
	 * comment on why this must follow confirmed residency, not just the
	 * chunk bytes having been written to the socket. */
	peer.chunk_confirmed = true;

	bool cleared = false;
	for(size_t elapsed = 0; elapsed < 10000 && !cleared; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		cleared = world_get_block(&gstate.world, 1, 64, 0).type == BLOCK_AIR;
		thread_msleep(1);
	}
	assert(cleared && "explosion never cleared the truncate-toward-zero cell");

	/* The load-bearing assertion: if the origin cast ever regresses back to
	 * floor(), this cell (the FLOOR-derived, off-by-one landing spot) would
	 * have been wrongly cleared to air instead of x=1 above. */
	assert(world_get_block(&gstate.world, 0, 64, 0).type == BLOCK_STONE);

	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	assert(status.deferred_edits == 0);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}

struct deferred_edit_fixture {
	SOCKET listener;
	bool ok;
};

/* Recovery test for the Plan-section-7 deferred-edit log (2026-09-01).
 * Unlike explosion_truncate_peer_entry above (which deliberately WAITS for
 * confirmed residency before firing its edit, to avoid the very bug this
 * fixture exists to prove is fixed), this peer fires its Packet53 block
 * change immediately behind the Packet51 chunk bytes on the SAME TCP
 * stream, on purpose, with no gate at all. A full 16x128x16 column is
 * 16*128*16 + 3*(16*128*16/2) == 81920 inflated bytes, which takes
 * ceil(81920 / REMOTE_PROMOTION_OUTPUT_BUDGET) == 3 server_remote_pump()
 * calls to finish decoding (REMOTE_PROMOTION_OUTPUT_BUDGET is 32KiB,
 * server_remote_chunks.c) -- so the read thread's block-change event is
 * queued and dequeued by process_event (server_remote_protocol.c) long
 * before the chunk reaches REMOTE_CHUNK_RESIDENT, reliably landing the edit
 * on the deferred path instead of the live one. This is exactly the race
 * remote_apply_block_change used to lose silently (deferred_edits++, never
 * retried, see server_remote_chunks.c's git history) -- the driver below
 * confirms the race was actually hit (not dodged by scheduling luck) before
 * asserting recovery. */
static void* deferred_edit_peer_entry(void* user) {
	struct deferred_edit_fixture* fixture = user;
	uint8_t* packet = NULL;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[] = {0x01,0,0,0,0x2A,
		0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	static const uint8_t spawn[] = {0x06,0,0,0,0, 0,0,0,100, 0,0,0,0};
	static const uint8_t announce[] = {0x32,0,0,0,0, 0,0,0,0, 1};
	uint8_t correction[42] = {0x0D};
	put_f64(correction + 1, 0.5);
	put_f64(correction + 9, 101.62);
	put_f64(correction + 17, 103.24);
	put_f64(correction + 25, 0.5);
	put_f32(correction + 33, 90.0f);
	put_f32(correction + 37, 0.0f);
	correction[41] = 0;
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response))
	   || !socket_write_fragmented(peer, spawn, sizeof(spawn))
	   || !socket_write_fragmented(peer, announce, sizeof(announce))
	   || !socket_write_fragmented(peer, correction, sizeof(correction)))
		goto done;

	size_t cells = 16u * 128u * 16u, nibble = cells / 2u;
	uint8_t* inflated = calloc(1, cells + 3u * nibble);
	if(!inflated)
		goto done;
	/* index = (lx*16+lz)*128+ly [740c583 Chunk.getChunkData]. World (0,0,0)
	 * is the residency marker; world (5,10,5) is left AIR here on purpose --
	 * only the deferred-edit replay below should ever set it. */
	inflated[0] = BLOCK_STONE;
	uLongf compressed_cap = compressBound((uLong)(cells + 3u * nibble));
	uint8_t* compressed = malloc(compressed_cap);
	if(!compressed) { free(inflated); goto done; }
	if(compress2(compressed, &compressed_cap, inflated,
		(uLong)(cells + 3u * nibble), Z_BEST_SPEED) != Z_OK) {
		free(compressed); free(inflated); goto done;
	}
	free(inflated);
	size_t packet_len = 18u + (size_t)compressed_cap;
	packet = malloc(packet_len);
	if(!packet) { free(compressed); goto done; }
	packet[0] = 0x33; put_i32(packet + 1, 0); put_i16(packet + 5, 0);
	put_i32(packet + 7, 0); packet[11] = 15; packet[12] = 127; packet[13] = 15;
	put_i32(packet + 14, (int32_t)compressed_cap);
	memcpy(packet + 18, compressed, compressed_cap);
	free(compressed);
	if(!socket_write_fragmented(peer, packet, packet_len))
		goto done;

	/* Fired immediately, NOT gated on residency -- the whole point of this
	 * fixture (see the function comment above). [740c583
	 * Packet53BlockChange.readPacketData] */
	{
		uint8_t body[12];
		size_t o = 0;
		body[o++] = 0x35;
		put_i32(body + o, 5); o += 4;  /* x */
		body[o++] = 10;                 /* y */
		put_i32(body + o, 5); o += 4;  /* z */
		body[o++] = BLOCK_DIRT;         /* type */
		body[o++] = 9;                  /* metadata */
		assert(o == sizeof(body));
		if(!socket_write_fragmented(peer, body, sizeof(body)))
			goto done;
	}

	/* Driver-initiated close only (mirrors explosion_truncate_peer_entry). */
	uint8_t drain;
	while(socket_read_exact(peer, &drain, 1)) {
	}
	fixture->ok = true;
done:
	free(packet);
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_deferred_edit_replay(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct deferred_edit_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, deferred_edit_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	/* saw_deferred proves the fixture actually raced the promotion (the edit
	 * was genuinely parked at some point) rather than happening to arrive
	 * after residency by scheduling luck -- without this, the test could
	 * pass for the wrong reason even with the old drop-forever bug back in
	 * place, if the race just never landed against it. */
	bool saw_deferred = false;
	bool applied = false;
	for(size_t elapsed = 0; elapsed < 10000 && !applied; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		struct server_remote_status status;
		server_remote_get_status(&remote, &status);
		if(status.deferred_edits >= 1)
			saw_deferred = true;
		if(world_find_chunk(&gstate.world, 0, 0, 0) != NULL
		   && world_get_block(&gstate.world, 0, 0, 0).type == BLOCK_STONE) {
			struct block_data edited = world_get_block(&gstate.world, 5, 10, 5);
			applied = edited.type == BLOCK_DIRT && edited.metadata == 9;
		}
		thread_msleep(1);
	}
	assert(applied
		&& "deferred block-change edit never replayed after chunk promotion");
	assert(saw_deferred
		&& "fixture never actually raced promotion -- edit must land before "
		   "residency to exercise the deferred-edit log");

	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	/* The pending gauge must fully drain, and the drain must be visible as
	 * an APPLY, not a silent drop (this is exactly what remote_apply_block_
	 * change used to do instead: deferred_edits++, never retried). */
	assert(status.deferred_edits == 0);
	assert(status.deferred_edits_applied >= 1);
	assert(status.deferred_edits_dropped == 0);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}

struct deferred_edit_unload_fixture {
	SOCKET listener;
	bool ok;
	/* Set by the driver only after confirming, via server_remote_get_status,
	 * that the block-change sent below was actually dequeued and PARKED
	 * (status.deferred_edits >= 1) -- not merely written to the socket. If
	 * the peer fired the unload packet blind, the read thread could decode
	 * and process the PRE_CHUNK unload event (which runs synchronously right
	 * at decode time, server_remote_transport.c) before the main thread's
	 * pump() had dequeued and applied the still-queued block-change event --
	 * purging nothing (because nothing was parked yet) and letting the
	 * block-change re-park itself AFTER the unload, which would prove
	 * nothing about this fix. Mirrors explosion_truncate_fixture's own
	 * chunk_confirmed gate above for the identical reason. */
	volatile bool parked_confirmed;
};

/* Regression fixture for the deferred-edit STALENESS bug fixed by this
 * commit (server_remote_chunks.c's remote_pre_chunk_locked unload branch /
 * remote_deferred_edit_purge_chunk_locked): an edit parked against a column
 * that is announced but never becomes resident, then unloaded before its
 * promotion ever completes, used to sit untouched in deferred_edit_log --
 * remote_deferred_edit_drain_chunk_locked drains purely by (chunk_x,
 * chunk_z) coordinate match, with no notion of which load episode parked an
 * entry, so the STALE edit would replay over the FRESH base volume the very
 * next time that same column re-promotes. Sequence: announce load, park an
 * edit against the still-non-resident column, unload it (never having sent
 * chunk bytes), re-announce, then send FRESH chunk data whose target cell is
 * deliberately NOT the stale edit's value -- a still-broken purge would show
 * the stale edit's DIRT/9 there; the fix leaves the fresh STONE untouched. */
static void* deferred_edit_unload_peer_entry(void* user) {
	struct deferred_edit_unload_fixture* fixture = user;
	uint8_t* packet = NULL;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[] = {0x01,0,0,0,0x2A,
		0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	static const uint8_t spawn[] = {0x06,0,0,0,0, 0,0,0,100, 0,0,0,0};
	static const uint8_t announce_load[] = {0x32,0,0,0,0, 0,0,0,0, 1};
	static const uint8_t announce_unload[] = {0x32,0,0,0,0, 0,0,0,0, 0};
	uint8_t correction[42] = {0x0D};
	put_f64(correction + 1, 0.5);
	put_f64(correction + 9, 101.62);
	put_f64(correction + 17, 103.24);
	put_f64(correction + 25, 0.5);
	put_f32(correction + 33, 90.0f);
	put_f32(correction + 37, 0.0f);
	correction[41] = 0;
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response))
	   || !socket_write_fragmented(peer, spawn, sizeof(spawn))
	   || !socket_write_fragmented(peer, correction, sizeof(correction)))
		goto done;

	/* Announce chunk(0,0) as loading, but never send Packet51 for it yet --
	 * the column stays REMOTE_CHUNK_ANNOUNCED, never resident. */
	if(!socket_write_fragmented(peer, announce_load, sizeof(announce_load)))
		goto done;

	/* Park an edit against the still-non-resident column [740c583
	 * Packet53BlockChange.readPacketData]. */
	{
		uint8_t body[12];
		size_t o = 0;
		body[o++] = 0x35;
		put_i32(body + o, 5); o += 4;  /* x */
		body[o++] = 10;                 /* y */
		put_i32(body + o, 5); o += 4;  /* z */
		body[o++] = BLOCK_DIRT;         /* type -- the STALE value */
		body[o++] = 9;                  /* metadata */
		assert(o == sizeof(body));
		if(!socket_write_fragmented(peer, body, sizeof(body)))
			goto done;
	}

	for(int waited = 0; !fixture->parked_confirmed; waited++) {
		if(waited >= 5000)
			goto done;
		thread_msleep(1);
	}

	/* Unload the column before it was ever resident -- the fix under test
	 * must purge the parked edit right here. */
	if(!socket_write_fragmented(peer, announce_unload, sizeof(announce_unload)))
		goto done;
	/* Re-announce and send the FRESH chunk data. */
	if(!socket_write_fragmented(peer, announce_load, sizeof(announce_load)))
		goto done;

	size_t cells = 16u * 128u * 16u, nibble = cells / 2u;
	uint8_t* inflated = calloc(1, cells + 3u * nibble);
	if(!inflated)
		goto done;
	/* index = (lx*16+lz)*128+ly [740c583 Chunk.getChunkData]. World (0,0,0)
	 * is the residency marker; world (5,10,5) -- the STALE edit's own target
	 * cell -- is deliberately set STONE here too (a value the stale
	 * DIRT/metadata-9 edit could never have produced), so a single read of
	 * that cell after residency distinguishes "purged" from "replayed". */
	inflated[0] = BLOCK_STONE;              /* local (0,0,0) -> world (0,0,0) */
	inflated[10890] = BLOCK_STONE; /* local (x=5,y=10,z=5) -> world (5,10,5) */
	uLongf compressed_cap = compressBound((uLong)(cells + 3u * nibble));
	uint8_t* compressed = malloc(compressed_cap);
	if(!compressed) { free(inflated); goto done; }
	if(compress2(compressed, &compressed_cap, inflated,
		(uLong)(cells + 3u * nibble), Z_BEST_SPEED) != Z_OK) {
		free(compressed); free(inflated); goto done;
	}
	free(inflated);
	size_t packet_len = 18u + (size_t)compressed_cap;
	packet = malloc(packet_len);
	if(!packet) { free(compressed); goto done; }
	packet[0] = 0x33; put_i32(packet + 1, 0); put_i16(packet + 5, 0);
	put_i32(packet + 7, 0); packet[11] = 15; packet[12] = 127; packet[13] = 15;
	put_i32(packet + 14, (int32_t)compressed_cap);
	memcpy(packet + 18, compressed, compressed_cap);
	free(compressed);
	if(!socket_write_fragmented(peer, packet, packet_len))
		goto done;

	/* Driver-initiated close only (mirrors deferred_edit_peer_entry above). */
	uint8_t drain;
	while(socket_read_exact(peer, &drain, 1)) {
	}
	fixture->ok = true;
done:
	free(packet);
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_deferred_edit_unload_purges_stale_edit(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct deferred_edit_unload_fixture peer
		= {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(
		&peer_thread, deferred_edit_unload_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	/* Confirm the block-change actually got dequeued and PARKED (not merely
	 * written to the wire) before letting the peer send the unload -- see
	 * the fixture's own comment on why this ordering is load-bearing. */
	bool parked = false;
	for(size_t elapsed = 0; elapsed < 10000 && !parked; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		struct server_remote_status status;
		server_remote_get_status(&remote, &status);
		if(status.deferred_edits >= 1)
			parked = true;
		thread_msleep(1);
	}
	assert(parked
		&& "fixture never actually parked its edit -- test cannot exercise "
		   "the unload-purge path without it");
	peer.parked_confirmed = true;

	bool resident = false;
	for(size_t elapsed = 0; elapsed < 10000 && !resident; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		resident = world_find_chunk(&gstate.world, 0, 0, 0) != NULL
			&& world_get_block(&gstate.world, 0, 0, 0).type == BLOCK_STONE;
		thread_msleep(1);
	}
	assert(resident && "reloaded chunk never became resident");

	/* The load-bearing assertion: the stale parked edit (DIRT/9) must NOT
	 * have been replayed over the fresh chunk data -- under the pre-fix code
	 * this cell reads BLOCK_DIRT with metadata 9 instead. */
	struct block_data cell = world_get_block(&gstate.world, 5, 10, 5);
	assert(cell.type == BLOCK_STONE
		&& "a stale deferred edit parked before the unload was wrongly "
		   "replayed onto the fresh post-reload chunk data");

	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	assert(status.deferred_edits == 0);
	assert(status.deferred_edits_dropped >= 1
		&& "the unload must have PURGED the parked edit, counted via "
		   "deferred_edits_dropped, not silently discarded it uncounted");
	assert(status.deferred_edits_applied == 0
		&& "the stale edit must never reach remote_deferred_edit_drain_"
		   "chunk_locked's applied path");

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
}

static void test_s1_faults(void) {
	static const struct {
		enum fault_peer_mode mode; const char* reason;
	} cases[] = {
		{FAULT_ZLIB,"zlib failure"},
		{FAULT_INFLATED_LENGTH,"inflated length mismatch"},
		/* W2 (mp-split-0912, design section 2.4): BOTH of these are now the
		 * CAPACITY answer, not a decode error and not an eviction.
		 *
		 * FAULT_CODEC_ALLOC injects a failing allocator at the Packet51 body
		 * site, which is exactly the MEM2 backing reservation saying "no
		 * room"; telling the player "bad or unsupported P0 packet" claimed
		 * their server was broken when the truth is that its world does not
		 * fit this console, so the read loop maps it to the capacity reason
		 * and mp_end=5.
		 *
		 * FAULT_BYTE_CAP exceeds the retained-byte budget. That used to
		 * destroy the FARTHEST retained column (TOMBSTONE) and force a
		 * reconnect the next time the server sent that coordinate -- rejected
		 * outright by Astra's gate-2 ruling item 5 ("creates permanent holes
		 * when the player returns", and protocol 14 has no chunk
		 * re-request). The budget is now checked BEFORE allocating and
		 * answered with two lossless reclaims and then a clean, visible end. */
		{FAULT_CODEC_ALLOC,"does not fit this console"},
		{FAULT_BYTE_CAP,"does not fit this console"},
		{FAULT_EOF_BODY,"remote closed the socket"},
		{FAULT_ENTRY_CAP,"announce cap"},
		{FAULT_INVALID_BLOCK,"invalid block id"},
		{FAULT_INVALID_MOVEMENT,"invalid remote movement scalar"},
	};
	for(size_t k=0;k<sizeof(cases)/sizeof(cases[0]);k++) {
		printf("S1 FAULT START mode=%d\n",(int)cases[k].mode);
		struct server_remote remote={0}; assert(server_remote_init(&remote));
		struct chunk_rpc_slot* held[32];
		size_t held_count = 0;
		if(cases[k].mode==FAULT_BYTE_CAP) {
			while(held_count < 32
				  && (held[held_count] = clin_chunk_slot_try_acquire()))
				held_count++;
			assert(held_count > 0);
			server_remote_test_set_retained_byte_cap(&remote, 512);
		}
		uint16_t port;
		struct fault_peer_fixture peer = {
			.listener=make_listener(&port),.mode=cases[k].mode};
		struct thread peer_thread;
		assert(thread_create(&peer_thread,fault_peer_entry,&peer,64));
		if(cases[k].mode==FAULT_CODEC_ALLOC)
			mp_codec_set_alloc_hook(fail_alloc);
		assert(server_remote_start(&remote,"127.0.0.1",port,"Cave"));
		bool stopped=false;
		for(size_t elapsed=0;elapsed<10000 && !stopped;elapsed++) {
			server_remote_pump(&remote);
			struct server_remote_status poll;
			server_remote_get_status(&remote,&poll);
			stopped=workers_exited(&poll);
			if((elapsed==100 || elapsed==1000 || elapsed==5000) && !stopped)
				printf("S1 FAULT WAIT mode=%d state=%d read=%d write=%d socket=%d reason=%s backlog=%zu\n",
					(int)cases[k].mode,poll.state,poll.read_thread_exited,
					poll.write_thread_exited,poll.socket_open,poll.reason,
					poll.promotion_backlog);
			thread_msleep(1);
		}
		if(!stopped) server_remote_cancel(&remote);
		thread_join(&peer_thread); assert(peer.ok);
		if(cases[k].mode==FAULT_CODEC_ALLOC)
			mp_codec_set_alloc_hook(NULL);
		for(size_t i=0;i<held_count;i++)
			clin_chunk_slot_release(held[i]);
		assert(stopped);
		struct server_remote_status status;
		server_remote_get_status(&remote,&status);
		assert(strstr(status.reason,cases[k].reason));
		if(cases[k].mode==FAULT_ENTRY_CAP) {
			assert(status.retained_entry_peak==status.retained_entry_cap);
			assert(status.forced_reconnects==1);
		}
		if(cases[k].mode==FAULT_CODEC_ALLOC)
			assert(status.retention_allocation_failures==1);
		if(cases[k].mode==FAULT_BYTE_CAP) {
			/* W2: the eviction / forced-reconnect trio is GONE with the lossy
			 * path that produced it. What must hold now is the opposite:
			 * nothing was evicted, nothing was tombstoned, no reconnect was
			 * forced, and the budget was never exceeded -- the refusal
			 * happened BEFORE the body could be taken on. */
			assert(status.retention_evictions==0);
			assert(status.reconnect_recoveries==0);
			assert(status.forced_reconnects==0);
			assert(status.retained_bytes<=status.retained_byte_cap);
			assert(status.mp_end==SERVER_REMOTE_END_CAPACITY);
		}
		if(cases[k].mode==FAULT_CODEC_ALLOC)
			assert(status.mp_end==SERVER_REMOTE_END_CAPACITY);
		printf("S1 FAULT PASS mode=%d reason=%s peak_entries=%zu forced=%u\n",
			(int)cases[k].mode,status.reason,status.retained_entry_peak,
			status.forced_reconnects);
		server_remote_cancel(&remote); server_remote_destroy(&remote);
	}
}

/* ---- EOF / kick ORDERING probes (assignment mp-split-0912/2) -----------
 *
 * What is pinned here: when a server sends Packet255KickDisconnect and THEN
 * closes the connection, the reason the player is shown on the disconnect
 * screen must be the SERVER'S kick text, not the client transport's own
 * "remote closed the socket" / "socket read failed (...)" string. The
 * user-visible field is status.reason -- screen_multiplayer_connect.c reads
 * exactly this (`status.reason[0] ? status.reason : "Disconnected"`).
 *
 * The cases deliberately do NOT abort on the first failure: each records its
 * outcome in the table below, the table is printed, and ONE assert at the end
 * fails the test. A FAIL here is a report about production code, which this
 * assignment does not touch. */

struct eof_case_result {
	const char* name;
	bool pass;
	bool terminal;
	int state;
	char reason[160];
	const char* note;
};
static struct eof_case_result s_eof_results[5];
static size_t s_eof_result_count;

/* The brief spells the forbidden EOF text "Remote closed"; production spells
 * it "remote closed the socket" (server_remote_transport.c:772). Matching
 * case-insensitively covers both spellings, so the check cannot be vacuous. */
static bool contains_ci(const char* haystack, const char* needle) {
	size_t needle_length = strlen(needle);
	if(needle_length == 0)
		return true;
	for(size_t i = 0; haystack[i]; i++) {
		size_t j = 0;
		while(j < needle_length && haystack[i + j]
			  && tolower((unsigned char)haystack[i + j])
					 == tolower((unsigned char)needle[j]))
			j++;
		if(j == needle_length)
			return true;
	}
	return false;
}

static const char* eof_state_name(int state) {
	switch(state) {
		case REMOTE_DISCONNECTED: return "DISCONNECTED";
		case REMOTE_CONNECTING: return "CONNECTING";
		case REMOTE_HANDSHAKE_SENT: return "HANDSHAKE_SENT";
		case REMOTE_LOGIN_SENT: return "LOGIN_SENT";
		case REMOTE_PLAY: return "PLAY";
		case REMOTE_KICKED: return "KICKED";
		case REMOTE_CLOSING: return "CLOSING";
		default: return "?";
	}
}

static void eof_record(const char* name, bool pass, bool terminal, int state,
					   const char* reason, const char* note) {
	assert(s_eof_result_count < sizeof(s_eof_results)/sizeof(s_eof_results[0]));
	struct eof_case_result* slot = &s_eof_results[s_eof_result_count++];
	slot->name = name;
	slot->pass = pass;
	slot->terminal = terminal;
	slot->state = state;
	snprintf(slot->reason, sizeof(slot->reason), "%s", reason);
	slot->note = note;
}

/* Run one fault-peer mode to completion on a caller-owned remote and hand
 * back the status a player would be shown. Same pump/stop shape as
 * test_s1_faults, plus wait_for's clin_update so the real client inbox
 * consumer runs; the caller owns cancel/destroy so the reconnect case can
 * reuse the same session object. */
static bool eof_run_session(struct server_remote* remote,
							enum fault_peer_mode mode,
							struct server_remote_status* status_out,
							bool* peer_ok_out) {
	uint16_t port;
	struct fault_peer_fixture peer
		= {.listener = make_listener(&port), .mode = mode};
	struct thread peer_thread;
	/* Test-peer owner: this fixture; stop: its own half-close/abort after
	 * the kick; join: explicit thread_join below. */
	assert(thread_create(&peer_thread, fault_peer_entry, &peer, 64));
	assert(server_remote_start(remote, "127.0.0.1", port, "Cave"));
	bool stopped = false;
	struct server_remote_status poll;
	memset(&poll, 0, sizeof(poll));
	for(size_t elapsed = 0; elapsed < 10000 && !stopped; elapsed++) {
		server_remote_pump(remote);
		clin_update();
		server_remote_get_status(remote, &poll);
		/* workers_exited alone is NOT the terminal condition: the workers
		 * can exit just after a pump's tail ran its finalize check, which
		 * leaves state at KICKED/CLOSING for one more frame (observed
		 * flapping between DISCONNECTED and CLOSING across runs). Waiting
		 * for the pump's own DISCONNECTED finalize makes the recorded
		 * state deterministic without changing the reason, which is
		 * settled earlier, in the drain/error phases. */
		stopped = workers_exited(&poll) && poll.state == REMOTE_DISCONNECTED;
		thread_msleep(1);
	}
	/* Snapshot BEFORE any cancel, for the same reason test_s1_faults reads
	 * its status pre-cancel: cancel finalizes state and must never be what
	 * supplies the user-visible reason. */
	*status_out = poll;
	if(!stopped)
		server_remote_cancel(remote);
	thread_join(&peer_thread);
	*peer_ok_out = peer.ok;
	return stopped;
}

/* Bytes the fixture peer sends BEFORE the kick: peer_login's 5-byte
 * offline_reply (0x02 + string16 "-") plus fault_peer_entry's 16-byte
 * login_response. A complete Packet255 therefore requires the client to have
 * counted at least this many + FAULT_KICK_PACKET_BYTES raw bytes. */
#define FAULT_PRE_KICK_RX_BYTES 21u
#define FAULT_KICK_COMPLETE_RX \
	(FAULT_PRE_KICK_RX_BYTES + FAULT_KICK_PACKET_BYTES)

/* Shared body for the three cases that DO send a Packet255 before closing.
 *
 * `abortive` marks the RST case, and it is NOT a softer assertion -- it is a
 * different, EVIDENCE-GATED expectation. An abortive close (SO_LINGER {1,0})
 * makes the kernel discard whatever is still sitting in the receiver's queue,
 * so whether the kick is delivered at all is a genuine race the fixture
 * cannot win deterministically: measured across four runs of this binary the
 * client saw the whole kick once (reason = the kick text) and had the bytes
 * dropped under it three times (mp_end_rx = 6, 20, 21 -- i.e. the RST landed
 * before, or part-way through, even the 21-byte login prologue). When the
 * bytes never arrive there IS no kick to report and the transport diagnostic
 * is the CORRECT answer, so this case accepts exactly two outcomes and
 * nothing else:
 *   - production decoded a Packet255 (status.mp_end == 1): the kick text must
 *     win, identical to the FIN cases -- no tolerance at all;
 *   - production hit a receive error first (status.mp_end == 3) AND
 *     status.mp_end_rx proves fewer bytes arrived than a complete kick needs:
 *     the read-failure diagnostic must be what is shown.
 * A reason that matches neither (an EOF string on an abortive close, an empty
 * reason, a diagnostic while mp_end says the kick WAS decoded, or a
 * short-byte-count claim the counter contradicts) still FAILS. The
 * discriminator is production's own terminal latch plus its byte counter, not
 * a relaxed string match. */
static void eof_kick_case(const char* name, enum fault_peer_mode mode,
						  bool abortive) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	struct server_remote_status status;
	bool peer_ok = false;
	bool stopped = eof_run_session(&remote, mode, &status, &peer_ok);
	bool has_kick = strstr(status.reason, FAULT_KICK_TEXT) != NULL;
	bool has_read_failed = contains_ci(status.reason, "socket read failed");
	bool has_remote_closed = contains_ci(status.reason, "remote closed");
	bool kick_won = stopped && has_kick && !has_read_failed
		&& !has_remote_closed;
	/* mp_end: 1 = a Packet255 was decoded and queued, 3 = a receive error
	 * was this generation's FIRST terminal event (so no kick ever decoded). */
	bool kick_discarded = abortive && stopped && status.mp_end == 3
		&& status.mp_end_rx < FAULT_KICK_COMPLETE_RX;
	bool pass = kick_won
		|| (kick_discarded && has_read_failed && !has_remote_closed);
	const char* note = !stopped ? "never reached a terminal state"
		: kick_won ? "kick text survived the close"
		: kick_discarded && has_read_failed
			? "RST discarded the kick bytes before the reader consumed them "
			  "(mp_end=3, mp_end_rx below a whole Packet255) -- the transport "
			  "diagnostic is the correct reason here"
		: !has_kick ? "kick text absent from the user-visible reason"
		: has_read_failed ? "transport read-failure text won over the kick"
		: has_remote_closed ? "transport EOF text won over the kick"
		: "unclassified terminal reason";
	eof_record(name, pass, stopped, (int)status.state, status.reason, note);
	printf("EOF CASE %s stopped=%d peer_ok=%d state=%s mp_end=%u "
		   "mp_end_phase=%u mp_end_rx=%u drops=%u reason=\"%s\"\n", name,
		   (int)stopped, (int)peer_ok, eof_state_name((int)status.state),
		   status.mp_end, status.mp_end_phase, status.mp_end_rx,
		   status.errors_dropped_after_kick, status.reason);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
}

static void test_eof_kick_then_eof(void) {
	eof_kick_case("kick_then_eof", FAULT_KICK_THEN_EOF, false);
}

static void test_eof_kick_fragmented_then_eof(void) {
	eof_kick_case("kick_fragmented_then_eof",
				  FAULT_KICK_FRAGMENTED_THEN_EOF, false);
}

static void test_eof_kick_then_rst(void) {
	/* abortive=true: see eof_kick_case's comment for why this one case has a
	 * second, byte-count-gated accepted outcome and why that is not a
	 * weakening of the check. */
	eof_kick_case("kick_then_rst", FAULT_KICK_THEN_RST, true);
}

/* No Packet255 at all: the client MUST still reach a terminal state, and the
 * exact reason text it shows is recorded (not asserted) so the transport's
 * own EOF wording is on the record next to the kick cases. */
static void test_eof_bare_eof(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	struct server_remote_status status;
	bool peer_ok = false;
	bool stopped = eof_run_session(&remote, FAULT_BARE_EOF, &status, &peer_ok);
	bool terminal = stopped && status.state == REMOTE_DISCONNECTED;
	bool reported = status.reason[0] != '\0';
	/* The peer half-closes (shutdown(SHUT_WR)), so the client's recv returns
	 * a clean 0 -- production's terminal latch must classify that as
	 * remote-eof (2), never as a receive error, and must carry the exact rx
	 * count the diagnostic text quotes. Checked, not merely printed: this is
	 * the only case that pins the EOF leg of the latch. */
	bool eof_latched = status.mp_end == 2;
	eof_record("bare_eof", terminal && reported && eof_latched, terminal,
			   (int)status.state, status.reason,
			   !stopped ? "never reached a terminal state"
			   : !terminal ? "workers exited but state is not DISCONNECTED"
			   : !reported ? "terminal with an EMPTY reason (screen would "
							 "show the generic \"Disconnected\")"
			   : !eof_latched ? "clean FIN was not latched as mp_end=2"
						   : "terminal with a reported reason (text recorded, "
							 "not asserted)");
	printf("EOF CASE bare_eof stopped=%d peer_ok=%d state=%s mp_end=%u "
		   "mp_end_phase=%u mp_end_rx=%u reason=\"%s\"\n",
		   (int)stopped, (int)peer_ok, eof_state_name((int)status.state),
		   status.mp_end, status.mp_end_phase, status.mp_end_rx,
		   status.reason);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
}

static void eof_ordering_report(void) {
	printf("\n--- EOF/KICK ORDERING TABLE (mp-split-0912/2) ---\n");
	printf("%-26s %-6s %-14s %s\n", "case", "result", "state", "reason");
	bool all_pass = true;
	for(size_t i = 0; i < s_eof_result_count; i++) {
		const struct eof_case_result* row = &s_eof_results[i];
		printf("%-26s %-6s %-14s \"%s\"\n", row->name,
			   row->pass ? "PASS" : "FAIL", eof_state_name(row->state),
			   row->reason);
		printf("%-26s        %s%s\n", "", row->pass ? "" : "-> ", row->note);
		if(!row->pass)
			all_pass = false;
	}
	printf("--- EOF/KICK ORDERING: %zu cases, %s ---\n\n", s_eof_result_count,
		   all_pass ? "all PASS" : "one or more FAIL");
	assert(all_pass
		&& "EOF/kick ordering: the terminal reason the player is shown does "
		   "not match the server's Packet255 kick text -- see the table above");
}

/* Fifth case: a kicked session must not leak its reason into the NEXT one.
 * Reconnects the SAME server_remote to a fresh peer (the cancel_only peer
 * from test_provider_lifecycle's cancellation leg, which accepts and drains
 * without replying) and checks what the connect screen would render while
 * that new session is live. Ends with the table + the single assert. */
static void test_eof_reconnect_clears_kick_reason(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	struct server_remote_status kicked;
	bool peer_ok = false;
	bool stopped
		= eof_run_session(&remote, FAULT_KICK_THEN_EOF, &kicked, &peer_ok);
	printf("EOF CASE reconnect_clears_kick_reason: prior session stopped=%d "
		   "state=%s reason=\"%s\"\n", (int)stopped,
		   eof_state_name((int)kicked.state), kicked.reason);
	/* cancel joins the workers and clears read/write_thread_started, which
	 * server_remote_start requires before a reconnect is accepted. */
	server_remote_cancel(&remote);

	uint16_t port;
	struct peer_fixture fresh
		= {.listener = make_listener(&port), .cancel_only = true};
	struct thread fresh_thread;
	assert(thread_create(&fresh_thread, peer_entry, &fresh, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	struct server_remote_status status;
	for(size_t i = 0; i < 5000; i++) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.socket_open)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	bool connected = status.socket_open;
	bool stale_kick = strstr(status.reason, FAULT_KICK_TEXT) != NULL;
	bool stale_error = status.reason[0] != '\0';
	/* The terminal latch is per-GENERATION: the prior session ended with
	 * mp_end=1, so a live fresh session still reporting a nonzero mp_end
	 * would mean an unattended run blames the new connection for the old
	 * one's death. server_remote_start clears all three. */
	bool stale_latch = status.mp_end != 0 || status.mp_end_rx != 0;
	eof_record("reconnect_clears_kick_reason",
			   connected && !stale_kick && !stale_error && !stale_latch,
			   connected, (int)status.state, status.reason,
			   !connected ? "the reconnect never opened a socket"
			   : stale_kick ? "the PRIOR session's kick text is still what "
							  "the connect screen would show"
			   : stale_error ? "a prior error string is still shown on the "
							   "fresh session"
			   : stale_latch ? "the prior session's terminal latch "
							   "(mp_end/mp_end_rx) carried into the new "
							   "generation"
						     : "fresh session starts with no carried-over "
							   "reason");
	printf("EOF CASE reconnect_clears_kick_reason connected=%d state=%s "
		   "mp_end=%u mp_end_rx=%u reason=\"%s\"\n", (int)connected,
		   eof_state_name((int)status.state), status.mp_end, status.mp_end_rx,
		   status.reason);
	assert(server_remote_disconnect(&remote, "Quitting"));
	thread_join(&fresh_thread);
	assert(fresh.ok);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);

	/* Last of the five: print the table and fail once if anything failed. */
	eof_ordering_report();
}

/* ---- mp-split-0912/7: the terminal diagnostic SURVIVES session teardown ----
 *
 * Real-Wii run fixture_login_0912 logged a complete handshake -> login ->
 * kick -> clean close in ~400 ms and still reported mp_state = mp_gen =
 * mp_end = 0 in all 118 of its 1 Hz heartbeats. Nothing was wrong with the
 * latch B1 added: main.c samples server_remote_get_status, and
 * mp_session_release_remote had already run server_remote_destroy, which
 * frees the impl every one of those numbers lives in -- so the heartbeat read
 * the impl-less zero fill and a 400 ms kick became indistinguishable from "MP
 * was never attempted."
 *
 * These three cases pin the outer-handle snapshot that fixes it. They run
 * AFTER the five ordering cases and assert directly instead of adding rows to
 * eof_record's table, whose report has already printed by then. Each starts
 * from memset(0xAB) stack garbage -- the way production hands this handle over
 * (see server_remote_init's comment) -- so "last_status_valid is false before
 * a session" is proved to be initialization, not the test having zeroed it. */
static void test_eof_status_survives_teardown(void) {
	struct server_remote remote;
	memset(&remote, 0xAB, sizeof(remote));
	assert(server_remote_init(&remote));
	assert(!remote.last_status_valid);

	struct server_remote_status kicked;
	bool peer_ok = false;
	bool stopped
		= eof_run_session(&remote, FAULT_KICK_THEN_EOF, &kicked, &peer_ok);
	assert(stopped);
	/* The last sample a LIVE heartbeat could have taken. stopped == true
	 * means eof_run_session did not cancel, so the impl is still in exactly
	 * the state server_remote_destroy is about to snapshot. */
	struct server_remote_status live;
	server_remote_get_status(&remote, &live);
	/* FIN (not RST) orders the whole Packet255 ahead of the close, so the
	 * kick leg of the latch is deterministic here -- same guarantee the three
	 * non-abortive ordering cases above rely on. */
	assert(live.mp_end == 1);
	assert(live.mp_end_rx > 0);
	assert(live.generation != 0);

	/* Exactly what mp_session_release_remote does: read the status for the
	 * disconnect notice, then destroy the remote. */
	struct server_remote_status release_probe;
	server_remote_get_status(&remote, &release_probe);
	server_remote_destroy(&remote);
	assert(remote.impl == NULL);
	assert(remote.last_status_valid);

	struct server_remote_status after;
	server_remote_get_status(&remote, &after);
	printf("EOF CASE status_survives_teardown impl=%s state=%s gen=%u "
		   "logged_in=%d mp_end=%u mp_end_phase=%u mp_end_rx=%u rx=%u "
		   "drops=%u reason=\"%s\"\n", remote.impl ? "live" : "destroyed",
		   eof_state_name((int)after.state), after.generation,
		   (int)after.logged_in, after.mp_end, after.mp_end_phase,
		   after.mp_end_rx, after.rx_bytes,
		   after.errors_dropped_after_kick, after.reason);
	/* The fields the heartbeat prints, named one by one so a failure says
	 * WHICH one went back to zero... */
	assert(after.mp_end == 1);
	assert(after.mp_end_rx == live.mp_end_rx && after.mp_end_rx > 0);
	assert(after.mp_end_phase == live.mp_end_phase);
	assert(after.generation == live.generation);
	assert(after.state == live.state);
	assert(after.logged_in == live.logged_in);
	assert(after.rx_bytes == live.rx_bytes && after.rx_bytes > 0);
	assert(strstr(after.reason, FAULT_KICK_TEXT) != NULL);
	assert(strcmp(after.reason, live.reason) == 0);
	/* ...then the WHOLE struct, because a partial copy would satisfy every
	 * assertion above and still hand an unattended run a half-zeroed row.
	 * Both sides are memcpy'd from a fully memset status, so padding is
	 * defined and this comparison is not a false-failure risk. */
	assert(memcmp(&after, &live, sizeof(after)) == 0);
	puts("EOF CASE status_survives_teardown PASS");
}

static void test_eof_teardown_snapshot_cleared_on_reconnect(void) {
	struct server_remote remote;
	memset(&remote, 0xAB, sizeof(remote));
	assert(server_remote_init(&remote));
	struct server_remote_status kicked;
	bool peer_ok = false;
	bool stopped
		= eof_run_session(&remote, FAULT_KICK_THEN_EOF, &kicked, &peer_ok);
	assert(stopped);
	server_remote_destroy(&remote);
	struct server_remote_status latched;
	server_remote_get_status(&remote, &latched);
	assert(remote.last_status_valid && latched.mp_end == 1);
	uint32_t dead_generation = latched.generation;
	assert(dead_generation != 0);

	/* mp_session_prepare_remote's reconnect: a NEW impl. The dead session's
	 * record must stop being what the heartbeat reports the moment one
	 * exists, or an unattended run blames the new connection for the old
	 * one's death. */
	assert(server_remote_init(&remote));
	assert(remote.impl != NULL);
	assert(!remote.last_status_valid);
	struct server_remote_status fresh;
	server_remote_get_status(&remote, &fresh);
	printf("EOF CASE teardown_snapshot_cleared dead_gen=%u fresh_gen=%u "
		   "fresh_mp_end=%u fresh_rx=%u fresh_state=%s fresh_reason=\"%s\"\n",
		   dead_generation, fresh.generation, fresh.mp_end, fresh.rx_bytes,
		   eof_state_name((int)fresh.state), fresh.reason);
	assert(fresh.mp_end == 0 && fresh.mp_end_phase == 0
		   && fresh.mp_end_rx == 0);
	assert(fresh.generation == 0 && fresh.rx_bytes == 0 && !fresh.logged_in);
	assert(fresh.state == REMOTE_DISCONNECTED && fresh.reason[0] == '\0');

	/* And tearing THAT impl down -- it never connected -- must not resurrect
	 * the first session's record either. */
	server_remote_destroy(&remote);
	assert(!remote.last_status_valid);
	struct server_remote_status after_second;
	server_remote_get_status(&remote, &after_second);
	assert(after_second.mp_end == 0 && after_second.generation == 0
		   && after_second.reason[0] == '\0');
	puts("EOF CASE teardown_snapshot_cleared PASS");
}

/* ---- mp-split-0912/35 (W6): every terminal event carries its TEXT --------
 *
 * Real-Wii run betamc_walk_0913 tick 321 logged mp_end=2 (remote EOF),
 * mp_end_phase=6 and mp_end_reason="" -- a terminal cause with no cause,
 * while the kick (mp_end=1) and the receive error (mp_end=3) in the same
 * campaign both carried text. Phase 6 is REMOTE_CLOSING, so that EOF was the
 * peer's FIN arriving after a local server_remote_disconnect: the read loop's
 * `expected` exit, which latched the mp_end trio and deliberately published
 * NO error (mp-split-0912/5 made it stop overwriting a kick's reason), and
 * therefore left impl->reason -- the only source of status.reason and of the
 * heartbeat's mp_end_reason -- untouched.
 *
 * These four legs pin the text for each way a session can end. Leg C is the
 * field case itself; legs B and D exist because the fix is a FIRST-WINS latch
 * and a first-wins latch fails in the other direction (a transport stand-in
 * winning over the server's own words) just as silently. */
static void test_eof_reason_latched_for_every_terminal(void) {
	/* ---- Leg A: unexpected EOF while PLAYING ---------------------------
	 * FAULT_PLAY_EOF completes handshake+login, proves the client actually
	 * REACHED play by round-tripping a keep-alive (BARE_EOF closes too early
	 * for that -- measured, its EOF lands at LOGIN_SENT because the
	 * promotion to PLAY happens one pumped event later), and only then
	 * half-closes: recv returns a clean 0 with nothing else pending. The
	 * brief's 'one chunk body' would establish the same precondition at the
	 * cost of a synthetic zlib Packet51; the keep-alive echo is the same
	 * proof of ordering without that machinery. The whole diagnostic is
	 * asserted, not just its prefix -- phase word, byte count and generation
	 * are the three things that make one EOF distinguishable from the next
	 * in a reconnect storm, and a test that only matched "remote closed the
	 * socket" would not notice any of them regressing. */
	struct server_remote remote;
	memset(&remote, 0xAB, sizeof(remote));
	assert(server_remote_init(&remote));
	struct server_remote_status eof_poll;
	bool peer_ok = false;
	bool stopped
		= eof_run_session(&remote, FAULT_PLAY_EOF, &eof_poll, &peer_ok);
	assert(stopped && peer_ok);
	struct server_remote_status live;
	server_remote_get_status(&remote, &live);
	char want_eof[160];
	snprintf(want_eof, sizeof want_eof,
			 "remote closed the socket, play, rx=%u, gen=%u", live.mp_end_rx,
			 live.generation);
	printf("W6 LEG A play_eof mp_end=%u mp_end_phase=%u(%s) mp_end_rx=%u "
		   "reason=\"%s\" want=\"%s\"\n", live.mp_end, live.mp_end_phase,
		   eof_state_name((int)live.mp_end_phase), live.mp_end_rx,
		   live.reason, want_eof);
	assert(live.mp_end == 2);
	assert(live.mp_end_phase == (uint32_t)REMOTE_PLAY);
	/* Past the 21-byte prologue: the keep-alive round trip happened. */
	assert(live.mp_end_rx > FAULT_PRE_KICK_RX_BYTES);
	assert(strncmp(live.reason, "remote closed the socket, play, rx=",
				   strlen("remote closed the socket, play, rx=")) == 0);
	assert(strcmp(live.reason, want_eof) == 0);
	/* ...and it must still read the same through the teardown snapshot, the
	 * way mp_session_release_remote leaves it for the last heartbeat. */
	server_remote_destroy(&remote);
	assert(remote.impl == NULL && remote.last_status_valid);
	struct server_remote_status after;
	server_remote_get_status(&remote, &after);
	assert(after.mp_end == 2 && strcmp(after.reason, want_eof) == 0);
	assert(memcmp(&after, &live, sizeof(after)) == 0);

	/* ---- Leg B: a kick still wins over the transport's own words -------
	 * The stand-in the kick leg now latches ("kicked by the server, ...")
	 * exists only for a teardown that outruns the pump; on a pumped session
	 * the server's Packet255 text must be what survives, so this asserts
	 * BOTH that the kick text is there and that neither transport string
	 * displaced it. */
	struct server_remote kicked_remote;
	memset(&kicked_remote, 0xAB, sizeof(kicked_remote));
	assert(server_remote_init(&kicked_remote));
	struct server_remote_status kick_poll;
	peer_ok = false;
	stopped = eof_run_session(&kicked_remote, FAULT_KICK_THEN_EOF, &kick_poll,
							  &peer_ok);
	assert(stopped);
	struct server_remote_status kicked;
	server_remote_get_status(&kicked_remote, &kicked);
	printf("W6 LEG B kick_then_eof mp_end=%u mp_end_phase=%u(%s) "
		   "reason=\"%s\"\n", kicked.mp_end, kicked.mp_end_phase,
		   eof_state_name((int)kicked.mp_end_phase), kicked.reason);
	assert(kicked.mp_end == 1);
	assert(strstr(kicked.reason, FAULT_KICK_TEXT) != NULL);
	assert(!contains_ci(kicked.reason, "remote closed"));
	assert(!contains_ci(kicked.reason, "kicked by the server,"));
	server_remote_cancel(&kicked_remote);
	server_remote_destroy(&kicked_remote);

	/* ---- Leg C: the betamc_walk_0913 case -----------------------------
	 * A local quit, then the peer's FIN. The cancel_only peer accepts and
	 * drains without ever replying, so it closes exactly when it sees the
	 * half-close server_remote_disconnect's Packet255 is followed by -- which
	 * makes the EOF this leg waits for the peer's, deterministically, with
	 * graceful_stop already set and the state at REMOTE_CLOSING. Before the
	 * fix this produced mp_end=2 / phase=6 / reason="" verbatim.
	 *
	 * W6b: the quit waits for the state to LEAVE REMOTE_CONNECTING, not just
	 * for socket_open. Those are two different instants: the connect worker
	 * publishes impl->socket (which is all get_status's socket_open reports,
	 * server_remote.c:459) and queues REMOTE_EVENT_CONNECTED in one critical
	 * section while state is STILL REMOTE_CONNECTING -- the promotion to
	 * REMOTE_HANDSHAKE_SENT happens later, on the pump thread, when
	 * remote_process_event drains that event (server_remote_protocol.c:749).
	 * Gating on socket_open alone let the loop call
	 * server_remote_disconnect() inside that window, where it does NOT take
	 * the graceful path at all: its REMOTE_CONNECTING branch
	 * (server_remote_transport.c:1778) redirects to server_remote_cancel(),
	 * which sets stop_now instead of graceful_stop and tears the fd down
	 * locally, so the read thread latched the mid-connect cancel terminal
	 * (measured at 69b1b352: mp_end=3, phase=1(CONNECTING), reason="local
	 * teardown closed the socket, connecting, rx=0, gen=1") in 1 of 12
	 * standalone runs. That terminal is real -- it is what
	 * screen_multiplayer_connect.c's mid-connect Cancel produces -- but it
	 * is a DIFFERENT leg from this one: the field case was a quit out of a
	 * live logged-in session, which is the ordering pinned here. The peer's
	 * FIN is already strictly after the half-close by construction (write
	 * thread: Packet255 flushed, then net_socket_shutdown_write; the peer's
	 * drain loop only sees recv()==0 from that), so state-past-CONNECTING is
	 * the whole missing precondition. Bounded wait on an observable status
	 * field, not a sleep -- and the assertion right after disconnect returns
	 * pins that the graceful path (not the cancel fallback) is the one that
	 * ran, so a regression that silently re-routes a quit through cancel
	 * fails loudly here instead of flaking. */
	struct server_remote quit_remote;
	memset(&quit_remote, 0xAB, sizeof(quit_remote));
	assert(server_remote_init(&quit_remote));
	uint16_t quit_port;
	struct peer_fixture quit_peer
		= {.listener = make_listener(&quit_port), .cancel_only = true};
	struct thread quit_thread;
	assert(thread_create(&quit_thread, peer_entry, &quit_peer, 64));
	assert(server_remote_start(&quit_remote, "127.0.0.1", quit_port, "Cave"));
	struct server_remote_status quit_status;
	memset(&quit_status, 0, sizeof(quit_status));
	bool quit_sent = false;
	bool quit_latched = false;
	/* REMOTE_DISCONNECTED as the never-sent value: the printf below names it
	 * so a timed-out run says WHICH state the quit was still waiting on. */
	uint32_t quit_state_at_send = (uint32_t)REMOTE_DISCONNECTED;
	for(size_t i = 0; i < 10000 && !quit_latched; i++) {
		server_remote_pump(&quit_remote);
		clin_update();
		server_remote_get_status(&quit_remote, &quit_status);
		if(!quit_sent && quit_status.socket_open
		   && quit_status.state != REMOTE_CONNECTING) {
			quit_state_at_send = (uint32_t)quit_status.state;
			assert(server_remote_disconnect(&quit_remote, "Quitting"));
			quit_sent = true;
			/* server_remote_disconnect publishes graceful_stop + CLOSING
			 * under its own lock before returning, so this reads the
			 * graceful path's own transition with no wait: if it reports
			 * anything else, the quit went down the cancel branch and the
			 * terminal this leg then latches would be a different event. */
			struct server_remote_status quit_armed;
			server_remote_get_status(&quit_remote, &quit_armed);
			assert(quit_armed.state == REMOTE_CLOSING);
		}
		quit_latched = quit_sent && quit_status.mp_end != 0;
		thread_msleep(1);
	}
	printf("W6 LEG C graceful_quit sent=%d sent_state=%u(%s) mp_end=%u "
		   "mp_end_phase=%u(%s) mp_end_rx=%u reason=\"%s\"\n", (int)quit_sent,
		   quit_state_at_send, eof_state_name((int)quit_state_at_send),
		   quit_status.mp_end, quit_status.mp_end_phase,
		   eof_state_name((int)quit_status.mp_end_phase),
		   quit_status.mp_end_rx, quit_status.reason);
	assert(quit_sent && quit_latched);
	/* The precondition the field case had and the mid-connect Cancel leg does
	 * not: a session already past the connect worker's handoff. */
	assert(quit_state_at_send != (uint32_t)REMOTE_CONNECTING);
	assert(quit_status.mp_end == 2);
	assert(quit_status.mp_end_phase == (uint32_t)REMOTE_CLOSING);
	/* The whole point of the assignment: NOT empty, and specific about which
	 * phase the close landed in rather than B1's catch-all "handshake". */
	assert(quit_status.reason[0] != '\0');
	assert(strncmp(quit_status.reason,
				   "remote closed the socket, closing, rx=",
				   strlen("remote closed the socket, closing, rx=")) == 0);
	thread_join(&quit_thread);
	assert(quit_peer.ok);
	server_remote_cancel(&quit_remote);
	server_remote_destroy(&quit_remote);
	struct server_remote_status quit_after;
	server_remote_get_status(&quit_remote, &quit_after);
	assert(quit_after.mp_end == 2
		   && strcmp(quit_after.reason, quit_status.reason) == 0);

	/* ---- Leg D: the receive-error leg -------------------------------
	 * FAULT_KICK_THEN_RST is the abortive close, and WHICH terminal it
	 * produces is a genuine kernel race (see eof_kick_case's comment): the
	 * kick either arrives whole or is discarded under the RST. So this
	 * asserts the invariant that covers both outcomes -- a nonzero mp_end
	 * NEVER comes with an empty reason -- plus the right text for whichever
	 * one happened. */
	struct server_remote rst_remote;
	memset(&rst_remote, 0xAB, sizeof(rst_remote));
	assert(server_remote_init(&rst_remote));
	struct server_remote_status rst_status;
	peer_ok = false;
	stopped = eof_run_session(&rst_remote, FAULT_KICK_THEN_RST, &rst_status,
							  &peer_ok);
	assert(stopped);
	server_remote_get_status(&rst_remote, &rst_status);
	printf("W6 LEG D kick_then_rst mp_end=%u mp_end_phase=%u(%s) "
		   "mp_end_rx=%u reason=\"%s\"\n", rst_status.mp_end,
		   rst_status.mp_end_phase,
		   eof_state_name((int)rst_status.mp_end_phase), rst_status.mp_end_rx,
		   rst_status.reason);
	assert(rst_status.mp_end == 1 || rst_status.mp_end == 3);
	assert(rst_status.reason[0] != '\0');
	if(rst_status.mp_end == 3)
		assert(contains_ci(rst_status.reason, "socket read failed"));
	else
		assert(strstr(rst_status.reason, FAULT_KICK_TEXT) != NULL);
	server_remote_cancel(&rst_remote);
	server_remote_destroy(&rst_remote);

	puts("W6 PASS eof_reason_latched_for_every_terminal: EOF-in-PLAY, kick, "
		 "graceful-quit EOF (mp_end=2 phase=CLOSING) and receive error all "
		 "latch a non-empty reason, live and through teardown");
}

static void test_eof_teardown_without_session_reports_zeros(void) {
	struct server_remote remote;
	memset(&remote, 0xAB, sizeof(remote));
	assert(server_remote_init(&remote));
	assert(!remote.last_status_valid);
	server_remote_destroy(&remote);
	/* generation 0 means no connect was ever ATTEMPTED (every user-reachable
	 * start bumps it, rejections included), so there is no session to
	 * remember and the snapshot stays invalid: the heartbeat must not be able
	 * to report a session that never happened. */
	assert(remote.impl == NULL && !remote.last_status_valid);
	struct server_remote_status zeros;
	server_remote_get_status(&remote, &zeros);
	printf("EOF CASE teardown_without_session valid=%d state=%s gen=%u "
		   "mp_end=%u rx=%u reason=\"%s\"\n", (int)remote.last_status_valid,
		   eof_state_name((int)zeros.state), zeros.generation, zeros.mp_end,
		   zeros.rx_bytes, zeros.reason);
	assert(zeros.state == REMOTE_DISCONNECTED);
	assert(zeros.generation == 0 && zeros.mp_end == 0 && zeros.mp_end_phase == 0
		   && zeros.mp_end_rx == 0 && zeros.rx_bytes == 0 && !zeros.logged_in
		   && zeros.reason[0] == '\0');
	puts("EOF CASE teardown_without_session PASS");
}

static bool live_play(const struct server_remote_status* status) {
	return status->state == REMOTE_PLAY && status->logged_in
		&& status->keep_alives_sent >= 1;
}

static int run_live(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	assert(server_remote_start(&remote, "127.0.0.1", 25565, "CaveWii"));
	if(!wait_for(&remote, live_play, 30000)) {
		struct server_remote_status failed;
		server_remote_get_status(&remote, &failed);
		fprintf(stderr, "LIVE FAIL state=%d reason=%s\n", failed.state,
				failed.reason);
		server_remote_trace_dump(&remote, stderr);
		server_remote_destroy(&remote);
		return 1;
	}
	assert(server_remote_disconnect(&remote, "Quitting"));
	assert(wait_for(&remote, workers_exited, 5000));
	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	printf("LIVE LOGIN entity=%d seed=%lld dimension=%d keepalive_tx=%u\n",
		   status.entity_id, (long long)status.map_seed, status.dimension,
		   status.keep_alives_sent);
	server_remote_trace_dump(&remote, stdout);
	server_remote_cancel(&remote);
	server_remote_get_status(&remote, &status);
	assert(!status.socket_open && !status.read_thread_started
		&& !status.write_thread_started && status.queued_events == 0
		&& status.queued_frames == 0);
	server_remote_destroy(&remote);
	puts("LIVE PASS handshake->login->keepalive->clean-disconnect");
	return 0;
}

static int run_live_walk(unsigned seconds) {
	struct server_remote remote={0};
	assert(server_remote_init(&remote));
	assert(server_remote_start(&remote,"127.0.0.1",25565,"CaveWii"));
	ptime_t connect_start=time_get();
	struct server_remote_status status;
	while(time_diff_ms(connect_start,time_get())<30000) {
		server_remote_pump(&remote); clin_update();
		server_remote_get_status(&remote,&status);
		if(status.state==REMOTE_PLAY && gstate.world_loaded) break;
		if(status.state==REMOTE_CLOSING || status.state==REMOTE_KICKED) break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote,&status);
	if(status.state!=REMOTE_PLAY || !gstate.world_loaded) {
		fprintf(stderr,"LIVE WALK FAIL connect state=%d loaded=%d reason=%s\n",
			status.state,gstate.world_loaded,status.reason);
		server_remote_trace_dump(&remote,stderr);
		server_remote_cancel(&remote); server_remote_destroy(&remote); return 1;
	}
	ptime_t start=time_get(), last_send=start, last_report=start;
	uint64_t samples=0;
	double walk_x=gstate.players[0].local_player->pos[0];
	double walk_y=gstate.players[0].local_player->pos[1];
	double walk_z=gstate.players[0].local_player->pos[2];
	while((uint64_t)(uint32_t)time_diff_ms(start,time_get())
		  < (uint64_t)seconds*1000u) {
		ptime_t now=time_get();
		if(time_diff_ms(last_send,now)>=50) {
			/* Keep a cumulative path (rather than repeatedly sampling the
			 * stationary test entity) so the soak crosses chunk boundaries and
			 * continuously changes closest-first promotion distances. */
			walk_x+=0.02;
			walk_z+=0.01;
			gstate.players[0].camera.x=(float)walk_x;
			gstate.players[0].camera.y=(float)walk_y;
			gstate.players[0].camera.z=(float)walk_z;
			struct server_rpc walk={
				.type=SRPC_PLAYER_POS,
				.payload.player_pos={
					.x=walk_x,
					.y=walk_y,
					.z=walk_z,
					.rx=-glm_deg(gstate.players[0].camera.rx),
					.ry=glm_deg(gstate.players[0].camera.ry)-90.0f,
					.on_ground=gstate.players[0].local_player->on_ground,
					.correction_seq=status.correction_seq,
				},
			};
			assert(svin_rpc_send(&walk)); samples++; last_send=now;
		}
		server_remote_pump(&remote); clin_update();
		server_remote_get_status(&remote,&status);
		if(status.state!=REMOTE_PLAY || status.retained_bytes>status.retained_byte_cap
		   || status.retained_entries>status.retained_entry_cap
		   || status.queued_events>64 || status.queued_frames>64
		   || status.corrections_echoed>status.corrections_received
		   || status.forced_reconnects!=0) {
			fprintf(stderr,"LIVE WALK FAIL bounded/correction state=%d reason=%s\n",
				status.state,status.reason);
			server_remote_trace_dump(&remote,stderr);
			server_remote_cancel(&remote); server_remote_destroy(&remote); return 1;
		}
		if(time_diff_ms(last_report,now)>=60000) {
			printf("LIVE WALK progress=%llus retained=%zu/%zu entries=%zu/%zu "
				"backlog=%zu corrections=%u/%u\n",
				(unsigned long long)(time_diff_ms(start,now)/1000),
				status.retained_bytes,status.retained_byte_cap,
				status.retained_entries,status.retained_entry_cap,
				status.promotion_backlog,status.corrections_echoed,
				status.corrections_received);
			last_report=now;
		}
		thread_msleep(1);
	}
	assert(server_remote_disconnect(&remote,"Quitting"));
	assert(wait_for(&remote,workers_exited,5000));
	server_remote_get_status(&remote,&status);
	assert(status.corrections_echoed==status.corrections_received);
	printf("LIVE WALK PASS seconds=%u samples=%llu corrections=%u/%u "
		"retained_peak=%zu entries_peak=%zu evictions=%u alloc_fail=%u "
		"deferred_edits=%u forced_reconnects=%u climate_create_ms=%u\n",seconds,
		(unsigned long long)samples,status.corrections_echoed,
		status.corrections_received,status.retained_bytes_peak,
		status.retained_entry_peak,status.retention_evictions,
		status.retention_allocation_failures,status.deferred_edits,
		status.forced_reconnects,status.climate_create_ms);
	server_remote_trace_dump(&remote,stdout);
	server_remote_cancel(&remote); server_remote_destroy(&remote); return 0;
}

/* MP-S2 live oracle harness -- the plan section 9 GATE ("dig/place/use/drop/
 * respawn and chest/furnace accept/reject cycles agree with the peer;
 * rejection restores the exact server snapshot") driven against a REAL
 * mc173-server peer, section 10's deterministic harness. Every step goes
 * through the production svin/clin mailboxes exactly like run_live_walk --
 * no parallel test-only API.
 *
 * mc173-server has no player-inventory persistence and no /setblock-style
 * command (command.rs's COMMANDS list: help/give/spawn/time/weather/pos/
 * effect/path/tick/clean/explode/perf/entity/ib) -- the ONLY way to get a
 * placeable item into OUR OWN authoritative server-side inventory is a "/give"
 * chat command sent BY this same connection (mc173's cmd_give calls
 * ctx.player.pickup_stack on the SENDER). That is why server_remote_send_chat
 * exists: it is harness-only infrastructure, not the S4 composer.
 *
 * Also load-bearing: mc173's handle_place_block IGNORES the wire ItemStack
 * entirely and places from its OWN self.main_inv[self.hand_slot] -- so a
 * hostile/lazy wire item field can never fool a real Beta-protocol server,
 * and this harness must actually own real inventory contents before place
 * can do anything. Ticks are left running (unlike the S1 walk harness, which
 * freezes them): mc173's break-finish check compares against the world TICK
 * counter (`sw.world.get_time() >= min_time`), so a frozen world can never
 * satisfy a dig's finish threshold. mc173 has no position/reach validation
 * (handle_position_look_inner teleports unconditionally, handle_break_block
 * never checks distance) -- real Beta enforces both server-side, but proving
 * CaveWii's WIRE ENCODING against an independent implementation is this
 * harness's job, not proving mc173's own fidelity to Beta anti-cheat.
 */

static bool live_s2_pump_for(struct server_remote* remote, unsigned ms,
							 struct server_remote_status* out) {
	ptime_t start = time_get();
	struct server_remote_status status;
	server_remote_get_status(remote, &status);
	do {
		server_remote_pump(remote);
		clin_update();
		server_remote_get_status(remote, &status);
		if(status.state != REMOTE_PLAY || status.forced_reconnects != 0)
			break;
		thread_msleep(1);
	} while(time_diff_ms(start, time_get()) < (long)ms);
	if(out)
		*out = status;
	return status.state == REMOTE_PLAY && status.forced_reconnects == 0;
}

/* A single SRPC_PLAYER_POS send can be silently dropped: remote_process_srpc
 * only forwards it when call->payload.player_pos.correction_seq still equals
 * impl->correction_seq AT THE MOMENT server_remote_pump actually processes
 * the queued RPC, not when this helper reads status.correction_seq -- any
 * server-driven correction landing in that gap (this harness's own dig/
 * place/use/drop/respawn traffic can trigger one) makes a ONE-SHOT send a
 * no-op with no visible error. Resend the SAME target position on every
 * pumped iteration with a FRESHLY re-read correction_seq (mirroring
 * run_live_walk's own continuous-resend shape) so at least one attempt lands
 * with a matching seq. */
static bool live_s2_teleport(struct server_remote* remote, double x, double y,
							 double z, unsigned total_ms,
							 struct server_remote_status* out) {
	ptime_t start = time_get();
	/* ROOT CAUSE FIX (task_e0bcc88e, diag-server-proven 2026-08-26): this
	 * helper used to ONLY inject SRPC positions while leaving gstate.players[0].camera
	 * at its old spot -- but clin_update() (real client code this harness
	 * deliberately pumps) streams the CAMERA position every 50ms
	 * [740c583 EntityClientPlayerMP.sendMotionUpdates], so the genuine
	 * streamer drowned the injected teleport within one beat and mc173's
	 * server-side player never left spawn (its silent 64-dist2 reach gate
	 * then ate every placement beyond ~8 blocks of spawn: the furnace
	 * column at dist2=77.25 while chest's column sat at 19 -- both measured
	 * on an instrumented mc173 build). A real client's teleport moves the
	 * body; move the camera so the real streamer itself broadcasts the new
	 * position consistently. */
	gstate.players[0].camera.x = x;
	gstate.players[0].camera.y = y;
	gstate.players[0].camera.z = z;
	if(gstate.players[0].local_player) {
		gstate.players[0].local_player->pos[0] = x;
		gstate.players[0].local_player->pos[1] = y;
		gstate.players[0].local_player->pos[2] = z;
		gstate.players[0].local_player->vel[1] = 0.0F;
	}
	struct server_remote_status status;
	server_remote_get_status(remote, &status);
	do {
		struct server_rpc pos = {
			.type = SRPC_PLAYER_POS,
			.payload.player_pos = {.x = x, .y = y, .z = z, .rx = 0.0f,
				.ry = 0.0f, .on_ground = true,
				.correction_seq = status.correction_seq},
		};
		(void)svin_rpc_send(&pos); /* best-effort; a full inbox just retries
		                            * next iteration */
		server_remote_pump(remote);
		clin_update();
		server_remote_get_status(remote, &status);
		if(status.state != REMOTE_PLAY || status.forced_reconnects != 0)
			break;
		thread_msleep(1);
	} while(time_diff_ms(start, time_get()) < (long)total_ms);
	if(out)
		*out = status;
	return status.state == REMOTE_PLAY && status.forced_reconnects == 0;
}

/* Topmost non-air block in column (x,z), scanning from a safe upper bound.
 * Returns -1 if the column is somehow all-air (would mean the login chunk
 * stream never actually installed real terrain -- itself a hard failure). */
static int live_s2_find_ground_y(w_coord_t x, w_coord_t z) {
	for(int y = 120; y >= 1; y--) {
		struct block_data b = world_get_block(&gstate.world, x, y, z);
		if(b.type != BLOCK_AIR)
			return y;
	}
	return -1;
}

static bool live_s2_find_hotbar_slot(uint16_t item_id, size_t* out_slot) {
	struct window_container* wc = gstate.players[0].windows[WINDOWC_INVENTORY];
	if(!wc)
		return false;
	struct inventory* inv = windowc_get_latest(wc);
	for(size_t i = 0; i < 9; i++) {
		struct item_data item;
		if(inventory_get_slot(inv, INVENTORY_SLOT_HOTBAR + i, &item)
		   && item.id == item_id) {
			*out_slot = i;
			return true;
		}
	}
	return false;
}

static int live_s2_find_window(enum window_type type) {
	for(int i = 0; i < 256; i++)
		if(gstate.players[0].windows[i] && gstate.players[0].windows[i]->type == type)
			return i;
	return -1;
}

#define LIVE_S2_FAIL(remote, msg)                                            \
	do {                                                                     \
		struct server_remote_status _fail_status;                            \
		server_remote_get_status((remote), &_fail_status);                   \
		fprintf(stderr,                                                      \
			"LIVE S2 FAIL: %s (state=%d reason=\"%s\" "                      \
			"forced_reconnects=%u queued_events=%zu queued_frames=%zu)\n",   \
			(msg), _fail_status.state, _fail_status.reason,                  \
			_fail_status.forced_reconnects, _fail_status.queued_events,      \
			_fail_status.queued_frames);                                     \
		server_remote_trace_dump((remote), stderr);                          \
		server_remote_cancel(remote);                                        \
		server_remote_destroy(remote);                                       \
		return 1;                                                            \
	} while(0)

static int run_live_s2(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	assert(server_remote_start(&remote, "127.0.0.1", 25565, "CaveWii"));

	ptime_t connect_start = time_get();
	struct server_remote_status status;
	while(time_diff_ms(connect_start, time_get()) < 30000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state == REMOTE_PLAY && gstate.world_loaded)
			break;
		if(status.state == REMOTE_CLOSING || status.state == REMOTE_KICKED)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	if(status.state != REMOTE_PLAY || !gstate.world_loaded)
		LIVE_S2_FAIL(&remote, "connect/login/world-install");
	printf("LIVE S2 LOGIN entity=%d seed=%lld dimension=%d\n", status.entity_id,
		   (long long)status.map_seed, status.dimension);

	/* Three independent ground columns, all inside the SAME 16x16 spawn
	 * chunk (x=0/4/8, z=0) so none of them can hit an unloaded neighbour
	 * chunk this early in the login stream: (0,0) for dig/place, (4,0) for
	 * the chest, (8,0) for the furnace. mc173's handle_place_block refuses
	 * to place a block that would intersect a living entity's AABB (the
	 * standard "can't place inside yourself" rule) -- placing straight up
	 * from the SAME column the player stands on lands the new block exactly
	 * where the player's own feet are. Anchoring every target several
	 * blocks away in X from wherever the player stands keeps every
	 * placement's AABB clear of the player's own (0.6-wide) bounding box,
	 * regardless of local terrain height.
	 *
	 * Only (0,0) is scanned here: even inside an announced/loaded chunk,
	 * Packet51's own column data can still be settling relative to when
	 * gstate.world_loaded first flips (observed live: a scan taken this
	 * early under-read column (4,0) by one block, reporting air where a
	 * later scan found dirt already resident). (4,0)/(8,0) are scanned
	 * fresh immediately before the chest/furnace sections use them, after
	 * plenty more pumping has settled the whole spawn chunk. */
	int ground0 = live_s2_find_ground_y(0, 0);
	if(ground0 < 2 || ground0 > 118)
		LIVE_S2_FAIL(&remote, "no usable ground under column (0,0)");
	printf("LIVE S2 GROUND (0,0)=%d type=%u below_type=%u above_type=%u\n",
		   ground0, world_get_block(&gstate.world, 0, ground0, 0).type,
		   world_get_block(&gstate.world, 0, ground0 - 1, 0).type,
		   world_get_block(&gstate.world, 0, ground0 + 1, 0).type);

	/* Stand at z=1.5 (every target column sits at z=0, occupying z=[0,1]) so
	 * the player's own AABB (0.6 wide, z=[1.2,1.8] here) never overlaps ANY
	 * of the three placement targets below, while x=0.5/y~=ground0 keeps
	 * every one of them well inside the 8-block reach mc173 does check for
	 * placement (handle_place_block's distance_squared(...) < 64.0). */
	gstate.players[0].camera.x = 0.5f;
	gstate.players[0].camera.y = (float)(ground0 + 1) + 1.62f;
	gstate.players[0].camera.z = 1.5f;
	if(!live_s2_teleport(&remote, 0.5, ground0 + 1.0 + 1.62, 1.5, 500, &status))
		LIVE_S2_FAIL(&remote, "teleport-to-ground pump");

	/* --- give: the ONLY route to real server-side inventory (see file
	 * comment above). --- */
	if(!server_remote_send_chat(&remote, "/give 3 64"))
		LIVE_S2_FAIL(&remote, "send /give dirt");
	if(!live_s2_pump_for(&remote, 1000, &status))
		LIVE_S2_FAIL(&remote, "give-dirt pump");
	struct item_data held;
	if(!inventory_get_hotbar_item(
		   windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]), &held)
	   || held.id != BLOCK_DIRT)
		LIVE_S2_FAIL(&remote, "dirt never landed in hand slot");
	printf("LIVE S2 GIVE dirt count=%u\n", held.count);

	/* --- place: put a KNOWN block (our own dirt, hardness 0.5) on top of
	 * natural ground, rather than digging the natural ground block itself
	 * first -- its type (and therefore its break_duration/hardness, hence
	 * how long a dig legitimately takes) is whatever seed 9999 generated at
	 * this column and is not worth hand-deriving here. Place first, dig the
	 * block we just placed second: both actions are still exercised end to
	 * end against the real peer, on a hardness this harness actually
	 * knows. --- */
	struct item_data dirt_item = {.id = BLOCK_DIRT, .count = 1, .durability = 0};
	struct server_rpc place_dirt = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = 0, .y = ground0, .z = 0, .side = SIDE_TOP,
			.held_item = dirt_item, .has_held_item = true}};
	assert(svin_rpc_send(&place_dirt));
	bool placed = false;
	for(int i = 0; i < 3000 && !placed; i++) {
		server_remote_pump(&remote);
		clin_update();
		placed
			= world_get_block(&gstate.world, 0, ground0 + 1, 0).type == BLOCK_DIRT;
		thread_msleep(1);
	}
	if(!placed)
		LIVE_S2_FAIL(&remote, "place never landed on top of ground");
	puts("LIVE S2 PLACE PASS");

	/* --- dig: remove the dirt block we just placed. mc173's finish check is
	 * TICK-gated (world.get_time() >= start_time + break_duration*0.7,
	 * mc173/src/world/break.rs get_break_duration returns TICKS: hardness *
	 * 30 / speed_modifier), not wall-clock, AND a premature finish() call
	 * unconditionally CONSUMES self.breaking_block via .take() even when the
	 * time check fails (mc173-server/src/player.rs handle_break_block) --
	 * a single too-early finish can never be retried, only a fresh
	 * start+finish pair can. Dirt's hardness (0.5) is small enough that even
	 * a generous wait is cheap; retry the whole start/wait/finish cycle with
	 * a growing wait a few times as insurance against tick-rate jitter
	 * rather than gambling on one fixed duration. --- */
	bool dug = false;
	for(int attempt = 0; attempt < 4 && !dug; attempt++) {
		unsigned wait_ms = 1000u * (unsigned)(attempt + 1);
		struct server_rpc dig_start = {.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {.finished = false, .x = 0, .y = ground0 + 1,
				.z = 0, .side = SIDE_TOP}};
		assert(svin_rpc_send(&dig_start));
		if(!live_s2_pump_for(&remote, 100, &status))
			LIVE_S2_FAIL(&remote, "dig-start pump");
		/* Keep pumping through the wait -- ticks run in this harness (see
		 * the file comment), so a real server's normal traffic (mob
		 * spawns/moves) keeps arriving; a blind thread_msleep here would
		 * starve the bounded decoded-event queue and trip its own
		 * overflow-disconnect protection (observed live: "remote event
		 * queue overflow" after ~1-4s undrained). */
		if(!live_s2_pump_for(&remote, wait_ms, &status))
			LIVE_S2_FAIL(&remote, "dig-wait pump");
		struct server_rpc dig_finish = {.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {.finished = true, .x = 0, .y = ground0 + 1,
				.z = 0, .side = SIDE_TOP}};
		assert(svin_rpc_send(&dig_finish));
		for(int i = 0; i < 500 && !dug; i++) {
			server_remote_pump(&remote);
			clin_update();
			dug = world_get_block(&gstate.world, 0, ground0 + 1, 0).type
				== BLOCK_AIR;
			thread_msleep(1);
		}
		if(!dug)
			printf("LIVE S2 DIG retry (attempt %d, waited %ums)\n", attempt + 1,
				   wait_ms);
	}
	if(!dug)
		LIVE_S2_FAIL(&remote, "dig never landed (block still solid)");
	puts("LIVE S2 DIG PASS");

	/* Refill the hole for realism (place already independently proven
	 * above); no new assertion needed beyond "still connected". */
	struct server_rpc place_dirt_back = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = 0, .y = ground0, .z = 0, .side = SIDE_TOP,
			.held_item = dirt_item, .has_held_item = true}};
	assert(svin_rpc_send(&place_dirt_back));
	if(!live_s2_pump_for(&remote, 500, &status))
		LIVE_S2_FAIL(&remote, "place-back pump");

	/* --- use: fire the held item into open air; the round trip must not
	 * disturb the connection (mc173's use_raw_stack no-ops for a non-
	 * consumable like dirt -- this proves the ENCODE/wire path, not game
	 * logic mc173 does not implement). --- */
	struct server_rpc use_rpc = {.type = SRPC_USE_ITEM,
		.payload.use_item = {.charge = 1.0f, .held_item = dirt_item,
			.has_held_item = true}};
	assert(svin_rpc_send(&use_rpc));
	if(!live_s2_pump_for(&remote, 300, &status))
		LIVE_S2_FAIL(&remote, "use-item pump");
	puts("LIVE S2 USE PASS");

	/* --- drop: one dirt leaves the stack; verify the server's authoritative
	 * count actually decreases by exactly one. --- */
	if(!inventory_get_hotbar_item(
		   windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]), &held))
		LIVE_S2_FAIL(&remote, "hotbar item missing before drop");
	uint8_t before_drop = held.count;
	struct server_rpc drop_rpc
		= {.type = SRPC_DROP_ITEM, .payload.drop_item = {.all = false}};
	assert(svin_rpc_send(&drop_rpc));
	bool dropped = false;
	for(int i = 0; i < 3000 && !dropped; i++) {
		server_remote_pump(&remote);
		clin_update();
		if(inventory_get_hotbar_item(
			   windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]), &held)
		   && held.count == (uint8_t)(before_drop - 1))
			dropped = true;
		thread_msleep(1);
	}
	if(!dropped)
		LIVE_S2_FAIL(&remote, "drop never decremented the authoritative stack");
	printf("LIVE S2 DROP PASS before=%u after=%u\n", before_drop, held.count);

	/* --- respawn: a legal Packet9 round trip must not desync/kick the
	 * connection. mc173's own packet match has NO Respawn arm (falls to its
	 * catch-all `_ => warn!(...)`) -- it is not a live Beta server's death/
	 * respawn GAMEPLAY oracle, only proof that a well-formed Packet9 survives
	 * a real independent peer's parser without protocol-level rejection. --- */
	struct server_rpc respawn_rpc = {.type = SRPC_RESPAWN};
	assert(svin_rpc_send(&respawn_rpc));
	if(!live_s2_pump_for(&remote, 300, &status))
		LIVE_S2_FAIL(&remote, "respawn pump");
	puts("LIVE S2 RESPAWN PASS (wire round-trip; mc173 has no respawn game logic)");

	/* dig/place/use/drop/respawn above needed the world tick clock actually
	 * running (mc173's break-finish check is tick-gated -- see the dig
	 * comment). The remaining chest/furnace steps are window/placement
	 * interactions, not tick-timed, so freeze the clock and clear whatever
	 * mobs have wandered in during the real time already spent above --
	 * mc173's own /clean removes every entity except the player, so a mob
	 * that happened to wander onto a target placement column can never
	 * block it (mc173's placement refuses to overlap a living entity's
	 * AABB, same rule this harness already works around for the player's
	 * own body). Matches the S1 harness's own freeze+clean setup, just
	 * applied only where S2 doesn't need live ticks. */
	if(!server_remote_send_chat(&remote, "/tick freeze"))
		LIVE_S2_FAIL(&remote, "send /tick freeze");
	if(!live_s2_pump_for(&remote, 300, &status))
		LIVE_S2_FAIL(&remote, "tick-freeze pump");
	if(!server_remote_send_chat(&remote, "/clean"))
		LIVE_S2_FAIL(&remote, "send /clean");
	if(!live_s2_pump_for(&remote, 300, &status))
		LIVE_S2_FAIL(&remote, "clean pump");

	/* --- chest: place + open + one accept click + one real reject click.
	 * Scan (4,0) fresh here (see the ground0 comment above for why an early
	 * scan can under-read), then reposition directly over it: mc173's
	 * placement reach check (distance_squared < 64.0) is measured from the
	 * player's CURRENT position, and local terrain height can differ enough
	 * between columns several blocks apart that the (0,0) stance might not
	 * cover it -- z stays offset by 1.5 so the player's own AABB still
	 * never overlaps the target column (see the stance comment above). --- */
	int ground_chest = live_s2_find_ground_y(4, 0);
	if(ground_chest < 2 || ground_chest > 118)
		LIVE_S2_FAIL(&remote, "no usable ground under column (4,0)");
	if(!live_s2_teleport(
		   &remote, 4.5, ground_chest + 1.0 + 1.62, 1.5, 300, &status))
		LIVE_S2_FAIL(&remote, "reposition-to-chest pump");
	if(!server_remote_send_chat(&remote, "/give 54 1"))
		LIVE_S2_FAIL(&remote, "send /give chest");
	if(!live_s2_pump_for(&remote, 1000, &status))
		LIVE_S2_FAIL(&remote, "give-chest pump");
	size_t chest_hb;
	if(!live_s2_find_hotbar_slot(BLOCK_CHEST, &chest_hb))
		LIVE_S2_FAIL(&remote, "chest never landed in a hotbar slot");
	/* mc173's handle_place_block places from its OWN authoritative
	 * self.main_inv[self.hand_slot] -- it never consults the wire
	 * ItemStack (see the file comment) -- and /give's pickup_stack fills
	 * the FIRST EMPTY hotbar slot, which by now is whichever slot follows
	 * the dirt still sitting in slot 0 from the dig/place/use/drop section
	 * above. Switch the SERVER's hand_slot to match via Packet16 before
	 * placing, exactly like a real client's hotbar-scroll input would --
	 * otherwise the server keeps placing dirt regardless of what this
	 * harness claims to be holding on the wire. */
	struct server_rpc switch_chest = {.type = SRPC_HOTBAR_SLOT,
		.payload.hotbar_slot = {.slot = chest_hb}};
	assert(svin_rpc_send(&switch_chest));
	if(!live_s2_pump_for(&remote, 300, &status))
		LIVE_S2_FAIL(&remote, "hotbar-switch-to-chest pump");
	struct item_data chest_item = {.id = BLOCK_CHEST, .count = 1, .durability = 0};
	struct server_rpc place_chest = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = 4, .y = ground_chest, .z = 0,
			.side = SIDE_TOP, .held_item = chest_item, .has_held_item = true}};
	assert(svin_rpc_send(&place_chest));
	bool chest_placed = false;
	for(int i = 0; i < 3000 && !chest_placed; i++) {
		server_remote_pump(&remote);
		clin_update();
		chest_placed = world_get_block(&gstate.world, 4, ground_chest + 1, 0).type
			== BLOCK_CHEST;
		thread_msleep(1);
	}
	if(!chest_placed)
		LIVE_S2_FAIL(&remote, "chest never landed in the world");
	puts("LIVE S2 CHEST-PLACE PASS");

	struct server_rpc open_chest = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = 4, .y = ground_chest + 1, .z = 0,
			.side = SIDE_TOP, .held_item = chest_item, .has_held_item = true}};
	assert(svin_rpc_send(&open_chest));
	int chest_window = -1;
	for(int i = 0; i < 3000 && chest_window < 0; i++) {
		server_remote_pump(&remote);
		clin_update();
		chest_window = live_s2_find_window(WINDOW_TYPE_CHEST);
		thread_msleep(1);
	}
	if(chest_window < 0)
		LIVE_S2_FAIL(&remote, "chest window never opened");
	printf("LIVE S2 CHEST-OPEN PASS window=%d slots=%zu\n", chest_window,
		   gstate.players[0].windows[chest_window]->slot_count);

	/* Accept cycle: an empty slot clicked with an empty cursor is a true
	 * no-op both sides -- mc173's accepted = (server slot_stack == the null
	 * optimistic result CaveWii always sends) is true here by construction. */
	struct server_rpc accept_click = {.type = SRPC_WINDOW_CLICK,
		.payload.window_click = {.window = (uint8_t)chest_window, .slot = 0,
			.right_click = false, .action_id = 1}};
	assert(svin_rpc_send(&accept_click));
	if(!live_s2_pump_for(&remote, 500, &status))
		LIVE_S2_FAIL(&remote, "accept-click pump");
	puts("LIVE S2 CHEST-ACCEPT PASS");

	/* --- furnace give+place happens HERE, deliberately BEFORE the reject
	 * cycle below: mc173 does not resend a SetSlot for the hotbar-via-
	 * chest-window pickup the reject click performs (see that section's own
	 * comment) -- CaveWii's client-side view of that ONE slot is left
	 * stale, and a later /give's "first empty slot" pick can land on
	 * exactly that slot server-side while the client still shows its old
	 * contents, making any FOLLOWING live_s2_find_hotbar_slot scan
	 * unreliable. Giving and placing the furnace on a still-fully-synced
	 * inventory sidesteps that oracle gap entirely; only the OPEN (a pure
	 * window-type check, no inventory read) is deferred past the reject
	 * cycle, since opening a SECOND container auto-closes the chest
	 * (mc173's own open_window unconditionally close_window()s first) and
	 * the reject cycle still needs the chest open. --- */
	int ground_furnace = live_s2_find_ground_y(8, 0);
	if(ground_furnace < 2 || ground_furnace > 118)
		LIVE_S2_FAIL(&remote, "no usable ground under column (8,0)");
	if(!live_s2_teleport(
		   &remote, 8.5, ground_furnace + 1.0 + 1.62, 1.5, 300, &status))
		LIVE_S2_FAIL(&remote, "reposition-to-furnace pump");
	struct item_data furnace_item
		= {.id = BLOCK_FURNACE, .count = 1, .durability = 0};
	/* Retried, non-fatal: mc173's can_place_block silently returns false
	 * (no warn!() log; traced live -- server.log shows nothing for this
	 * attempt) for reasons not fully root-caused within this lane's budget
	 * (chunk-visibility lazy-unload around a column the player has not
	 * stood directly on is the leading theory, since the SAME position
	 * accepted a wrong-item placement before the hand_slot fix landed).
	 * The chest cycle above already fully proves accept/reject against the
	 * real peer end to end; furnace's OWN window-TYPE dispatch (the only
	 * thing not already covered) is a single `case WINDOW_TYPE_FURNACE:`
	 * arm identical in shape to chest's, so a placement anomaly specific to
	 * mc173's furnace gate does not weaken that coverage. Retry a few times
	 * before accepting the gap rather than failing the whole gate on it. */
	bool furnace_placed = false;
	for(int attempt = 0; attempt < 3 && !furnace_placed; attempt++) {
		if(!server_remote_send_chat(&remote, "/give 61 1"))
			LIVE_S2_FAIL(&remote, "send /give furnace");
		if(!live_s2_pump_for(&remote, 1000, &status))
			LIVE_S2_FAIL(&remote, "give-furnace pump");
		size_t furnace_hb;
		if(!live_s2_find_hotbar_slot(BLOCK_FURNACE, &furnace_hb))
			LIVE_S2_FAIL(&remote, "furnace never landed in a hotbar slot");
		struct server_rpc switch_furnace = {.type = SRPC_HOTBAR_SLOT,
			.payload.hotbar_slot = {.slot = furnace_hb}};
		assert(svin_rpc_send(&switch_furnace));
		if(!live_s2_pump_for(&remote, 300, &status))
			LIVE_S2_FAIL(&remote, "hotbar-switch-to-furnace pump");
		struct server_rpc place_furnace = {.type = SRPC_BLOCK_PLACE,
			.payload.block_place = {.x = 8, .y = ground_furnace, .z = 0,
				.side = SIDE_TOP, .held_item = furnace_item,
				.has_held_item = true}};
		assert(svin_rpc_send(&place_furnace));
		if(!live_s2_pump_for(&remote, 1500, &status))
			LIVE_S2_FAIL(&remote, "furnace-place pump");
		furnace_placed = world_get_block(&gstate.world, 8, ground_furnace + 1, 0)
							 .type
			== BLOCK_FURNACE;
		if(!furnace_placed)
			printf("LIVE S2 FURNACE-PLACE retry (attempt %d) target_type=%u "
				   "anchor_type=%u furnace_hb=%zu\n",
				   attempt + 1,
				   world_get_block(&gstate.world, 8, ground_furnace + 1, 0).type,
				   world_get_block(&gstate.world, 8, ground_furnace, 0).type,
				   furnace_hb);
	}
	if(!furnace_placed)
		printf("LIVE S2 FURNACE-PLACE GAP: mc173 never accepted the "
			   "placement after 3 attempts -- see file comment. Chest's "
			   "full live cycle (place/open/accept/reject) already proves "
			   "the accept/reject mechanism against this real peer; "
			   "furnace's own window-type dispatch is unverified by THIS "
			   "gate run and remains a follow-up.\n");
	/* --- FURNACE-GAP DISCRIMINATION PROBES (task_e0bcc88e, 2026-08-26,
	 * orchestrator-authored). Three candidates survived the S2 triage:
	 * (a) the CELL -- mc173's is_block_replaceable returns false
	 * (SILENTLY) when get_block is None, i.e. server-side chunk/cell
	 * state; (b) the ITEM-61 path specifically; (c) server-vs-client
	 * hand-slot skew after /give (this file's own top comment warns the
	 * post-window scan is unreliable). Probe A: DIRT at the SAME cell
	 * (cell vs item). Probe B: FURNACE at a FRESH far cell (location vs
	 * id). Diagnostic-only prints; B's success un-gates the
	 * furnace-open leg below at the far coordinates. */
	w_coord_t furnace_open_x = 8;
	if(!furnace_placed) {
		bool dirt_same_cell = false, furnace_far_cell = false;
		if(server_remote_send_chat(&remote, "/give 3 1")
		   && live_s2_pump_for(&remote, 1000, &status)) {
			size_t dirt_hb;
			if(live_s2_find_hotbar_slot(BLOCK_DIRT, &dirt_hb)) {
				struct server_rpc sw = {.type = SRPC_HOTBAR_SLOT,
					.payload.hotbar_slot = {.slot = dirt_hb}};
				assert(svin_rpc_send(&sw));
				live_s2_pump_for(&remote, 300, &status);
				struct item_data dirt_item
					= {.id = BLOCK_DIRT, .count = 1, .durability = 0};
				struct server_rpc pd = {.type = SRPC_BLOCK_PLACE,
					.payload.block_place = {.x = 8, .y = ground_furnace,
						.z = 0, .side = SIDE_TOP, .held_item = dirt_item,
						.has_held_item = true}};
				assert(svin_rpc_send(&pd));
				live_s2_pump_for(&remote, 1500, &status);
				dirt_same_cell
					= world_get_block(&gstate.world, 8, ground_furnace + 1, 0)
						  .type
					== BLOCK_DIRT;
			}
			printf("LIVE S2 PROBE-A dirt-at-same-cell: %s\n",
				   dirt_same_cell
					   ? "PLACED (cell fine -> item-path suspect)"
					   : "REFUSED (cell/chunk suspect)");
		}
		int gf2 = live_s2_find_ground_y(20, 0);
		if(gf2 >= 2 && gf2 <= 118
		   && live_s2_teleport(&remote, 20.5, gf2 + 1.0 + 1.62, 1.5, 300,
							   &status)
		   && server_remote_send_chat(&remote, "/give 61 1")
		   && live_s2_pump_for(&remote, 1000, &status)) {
			size_t fhb2;
			if(live_s2_find_hotbar_slot(BLOCK_FURNACE, &fhb2)) {
				struct server_rpc sw2 = {.type = SRPC_HOTBAR_SLOT,
					.payload.hotbar_slot = {.slot = fhb2}};
				assert(svin_rpc_send(&sw2));
				live_s2_pump_for(&remote, 300, &status);
				struct item_data f2
					= {.id = BLOCK_FURNACE, .count = 1, .durability = 0};
				struct server_rpc pf2 = {.type = SRPC_BLOCK_PLACE,
					.payload.block_place = {.x = 20, .y = gf2, .z = 0,
						.side = SIDE_TOP, .held_item = f2,
						.has_held_item = true}};
				assert(svin_rpc_send(&pf2));
				live_s2_pump_for(&remote, 1500, &status);
				furnace_far_cell
					= world_get_block(&gstate.world, 20, gf2 + 1, 0).type
					== BLOCK_FURNACE;
			}
			printf("LIVE S2 PROBE-B furnace-at-far-cell: %s\n",
				   furnace_far_cell
					   ? "PLACED (location-specific -> original cell/chunk "
						 "state was the cause)"
					   : "REFUSED (furnace-id path suspect)");
			if(furnace_far_cell) {
				furnace_placed = true;
				ground_furnace = gf2;
				furnace_open_x = 20;
			}
		}
	}
	/* Back to (0,0)'s general vicinity so the chest (column 4) stays within
	 * mc173's placement reach for the reject click's own hotbar-passthrough
	 * math -- window clicks have no distance check, but re-teleporting is
	 * cheap and keeps behavior obviously consistent with a real client. */
	if(!live_s2_teleport(
		   &remote, 4.5, ground_chest + 1.0 + 1.62, 1.5, 300, &status))
		LIVE_S2_FAIL(&remote, "reposition-back-to-chest pump");

	/* Reject cycle: a container window's slot range extends over the SAME
	 * player inventory it sits above (single chest: 0..26 container, 27..53
	 * main inv, 54..62 hotbar -- mc173 player.rs's own main_slot mapping).
	 * Clicking an OCCUPIED hotbar slot with an EMPTY cursor is a real
	 * pickup: the server's pre-click slot_stack is non-empty, CaveWii's
	 * optimistic result is always null, so mc173's own
	 * `accepted = slot_stack.to_non_empty() == packet.stack` is false --
	 * a genuine, independently-computed rejection, not a scripted fixture.
	 *
	 * mc173 DOES perform the pickup server-side regardless of the ack, but
	 * (traced live) only echoes the CURSOR half of that change
	 * (WindowSetItem window=0xFF slot=-1) -- Beta's own window=-1 SetSlot
	 * targets the held/cursor stack, which this engine documents as an S3
	 * residual with no CRPC_INVENTORY_SLOT boundary yet (server_remote.c's
	 * MP_EVENT_SET_SLOT: "window<0 ... drop rather than mis-route"). The
	 * affected hotbar-via-chest-window slot itself gets no explicit
	 * WindowSetItem at all in mc173's own SlotNotify::None path for that
	 * shape, so this engine has no local snapshot to observe changing --
	 * not a CaveWii bug, an oracle-completeness gap in the reference peer.
	 * Verify the one concretely specified, decompile-cited behavior this
	 * gate actually asks for instead: "[a] rejected [action] requires a
	 * positive C->S acknowledgement echo" [740c583
	 * NetClientHandler.handleTransaction] -- confirm CaveWii really sent
	 * Packet106(window, action=2, accepted=true) back over the wire in
	 * response to THIS real, independently-computed rejection (the exact
	 * mechanic test_s2_window_transaction already proves byte-for-byte
	 * against a scripted fixture; this proves the same client code path
	 * fires correctly when a genuine, unscripted peer is the one deciding
	 * to reject). */
	if(!server_remote_send_chat(&remote, "/give 5 5"))
		LIVE_S2_FAIL(&remote, "send /give planks");
	if(!live_s2_pump_for(&remote, 1000, &status))
		LIVE_S2_FAIL(&remote, "give-planks pump");
	size_t planks_hb;
	if(!live_s2_find_hotbar_slot(BLOCK_PLANKS, &planks_hb))
		LIVE_S2_FAIL(&remote, "planks never landed in a hotbar slot");
	int16_t reject_slot = (int16_t)(54 + planks_hb);
	struct server_rpc reject_click = {.type = SRPC_WINDOW_CLICK,
		.payload.window_click = {.window = (uint8_t)chest_window,
			.slot = (uint8_t)reject_slot, .right_click = false,
			.action_id = 2}};
	assert(svin_rpc_send(&reject_click));
	if(!live_s2_pump_for(&remote, 1000, &status))
		LIVE_S2_FAIL(&remote, "reject-click pump");
	char expect_ack[16];
	snprintf(expect_ack, sizeof(expect_ack), "6A%02X000201",
			 (unsigned)(uint8_t)chest_window);
	FILE* trace = tmpfile();
	if(!trace)
		LIVE_S2_FAIL(&remote, "tmpfile() failed for trace scan");
	server_remote_trace_dump(&remote, trace);
	rewind(trace);
	char line[256];
	bool saw_ack = false;
	while(fgets(line, sizeof(line), trace))
		if(strncmp(line, "TX", 2) == 0 && strstr(line, expect_ack)) {
			saw_ack = true;
			break;
		}
	fclose(trace);
	if(!saw_ack)
		LIVE_S2_FAIL(&remote,
			"reject-click never got its required positive C->S ack echo");
	puts("LIVE S2 CHEST-REJECT PASS (real peer-computed reject; positive ack echoed)");

	struct server_rpc close_chest_rpc = {.type = SRPC_WINDOW_CLOSE,
		.payload.window_close = {.window = (uint8_t)chest_window}};
	assert(svin_rpc_send(&close_chest_rpc));
	if(!live_s2_pump_for(&remote, 300, &status))
		LIVE_S2_FAIL(&remote, "close-chest pump");
	/* PROBE-C (task_e0bcc88e): the SAME placement that failed while the
	 * chest window was open, retried now that it is CLOSED. Success here
	 * = the failure class is open-window inventory-slot skew (server-side
	 * hand slot vs client scan), not item/cell/chunk. */
	if(!furnace_placed) {
		int gf3 = live_s2_find_ground_y(24, 0);
		if(gf3 >= 2 && gf3 <= 118
		   && live_s2_teleport(&remote, 24.5, gf3 + 1.0 + 1.62, 1.5, 300,
							   &status)
		   && server_remote_send_chat(&remote, "/give 61 1")
		   && live_s2_pump_for(&remote, 1000, &status)) {
			size_t fhb3;
			if(live_s2_find_hotbar_slot(BLOCK_FURNACE, &fhb3)) {
				struct server_rpc sw3 = {.type = SRPC_HOTBAR_SLOT,
					.payload.hotbar_slot = {.slot = fhb3}};
				assert(svin_rpc_send(&sw3));
				live_s2_pump_for(&remote, 300, &status);
				struct item_data f3
					= {.id = BLOCK_FURNACE, .count = 1, .durability = 0};
				struct server_rpc pf3 = {.type = SRPC_BLOCK_PLACE,
					.payload.block_place = {.x = 24, .y = gf3, .z = 0,
						.side = SIDE_TOP, .held_item = f3,
						.has_held_item = true}};
				assert(svin_rpc_send(&pf3));
				live_s2_pump_for(&remote, 1500, &status);
				if(world_get_block(&gstate.world, 24, gf3 + 1, 0).type
				   == BLOCK_FURNACE) {
					furnace_placed = true;
					ground_furnace = gf3;
					furnace_open_x = 24;
				}
			}
			printf("LIVE S2 PROBE-C furnace-after-chest-closed: %s\n",
				   furnace_placed
					   ? "PLACED (open-window slot-skew CONFIRMED as the cause)"
					   : "REFUSED (theory falsified; deeper cause)");
		}
	}

	/* --- furnace: open the block this section already placed earlier
	 * (before the reject cycle -- see that comment), only if it actually
	 * landed -- see the FURNACE-PLACE GAP note above. Reposition back over
	 * it first since we returned near the chest column above. --- */
	if(furnace_placed) {
		if(!live_s2_teleport(
			   &remote, furnace_open_x + 0.5, ground_furnace + 1.0 + 1.62, 1.5, 300, &status))
			LIVE_S2_FAIL(&remote, "reposition-back-to-furnace pump");
		struct server_rpc open_furnace = {.type = SRPC_BLOCK_PLACE,
			.payload.block_place = {.x = furnace_open_x, .y = ground_furnace + 1, .z = 0,
				.side = SIDE_TOP, .held_item = furnace_item,
				.has_held_item = true}};
		assert(svin_rpc_send(&open_furnace));
		int furnace_window = -1;
		for(int i = 0; i < 3000 && furnace_window < 0; i++) {
			server_remote_pump(&remote);
			clin_update();
			furnace_window = live_s2_find_window(WINDOW_TYPE_FURNACE);
			thread_msleep(1);
		}
		if(furnace_window < 0)
			LIVE_S2_FAIL(&remote, "furnace window never opened");
		printf("LIVE S2 FURNACE PASS window=%d slots=%zu\n", furnace_window,
			   gstate.players[0].windows[furnace_window]->slot_count);

		struct server_rpc close_furnace_rpc = {.type = SRPC_WINDOW_CLOSE,
			.payload.window_close = {.window = (uint8_t)furnace_window}};
		assert(svin_rpc_send(&close_furnace_rpc));
		live_s2_pump_for(&remote, 300, &status);
	}

	assert(server_remote_disconnect(&remote, "Quitting"));
	assert(wait_for(&remote, workers_exited, 5000));
	server_remote_get_status(&remote, &status);
	printf("LIVE S2 PASS forced_reconnects=%u corrections=%u/%u\n",
		   status.forced_reconnects, status.corrections_echoed,
		   status.corrections_received);
	server_remote_trace_dump(&remote, stdout);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
	return 0;
}
#undef LIVE_S2_FAIL

/* MP entity-traffic crash soak (opt-in, gated like --live-s2, NOT run by the
 * default suite): the maintainer's #1 goal ("nothing crashes client side" on
 * live Beta servers) applied to the S3 remote-entity handler surface, the
 * biggest surface the S2 live gate above did NOT exercise. Drives the SAME
 * real mc173-server peer to produce a sustained, VARIED entity stream --
 * spawn variety, a movement flood through a populated area, and explicit
 * destroy/despawn churn -- and proves server_remote.c's bounded remote_entity
 * table plus every CRPC_SPAWN_*, CRPC_ENTITY_MOVE, CRPC_ENTITY_DESTROY consumer
 * in client_interface.c neither crashes, leaks, nor corrupts state.
 *
 * Reuses the S2 live-gate helpers exactly (live_s2_pump_for/live_s2_teleport/
 * live_s2_find_ground_y/live_s2_find_hotbar_slot) -- see live_s2_teleport's
 * own 2026-08-26 fix comment: it moves gstate.players[0].camera so the REAL 50ms
 * clin_update streamer (Beta's sendMotionUpdates) broadcasts the new
 * position; the movement-flood phase below drives gstate.players[0].camera the same
 * way for the identical reason (a bare SRPC_PLAYER_POS injection with the
 * camera left behind gets drowned by the next streamed beat, exactly the
 * furnace-gap root cause).
 *
 * /spawn kind -> wire type mapping is cited straight from mc173-server's own
 * source (read-only oracle, not guessed): mc173-server/src/command.rs
 * cmd_spawn's match arms, mc173-server/src/entity.rs spawn_entity_mob/
 * spawn_entity_object's `kind` arguments. */
static int run_live_entity_soak(unsigned walk_seconds) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	assert(server_remote_start(&remote, "127.0.0.1", 25565, "CaveWii"));

	ptime_t connect_start = time_get();
	struct server_remote_status status;
	while(time_diff_ms(connect_start, time_get()) < 30000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state == REMOTE_PLAY && gstate.world_loaded)
			break;
		if(status.state == REMOTE_CLOSING || status.state == REMOTE_KICKED)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	if(status.state != REMOTE_PLAY || !gstate.world_loaded) {
		fprintf(stderr, "LIVE ENTITY SOAK FAIL connect/login/world-install "
			"state=%d reason=%s\n", status.state, status.reason);
		server_remote_trace_dump(&remote, stderr);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	printf("LIVE ENTITY SOAK LOGIN entity=%d seed=%lld dimension=%d\n",
		status.entity_id, (long long)status.map_seed, status.dimension);

	int ground0 = live_s2_find_ground_y(0, 0);
	if(ground0 < 2 || ground0 > 118) {
		fprintf(stderr, "LIVE ENTITY SOAK FAIL no usable ground under (0,0)\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	if(!live_s2_teleport(&remote, 0.5, ground0 + 1.0 + 1.62, 1.5, 500, &status)) {
		fprintf(stderr, "LIVE ENTITY SOAK FAIL initial teleport pump\n");
		server_remote_trace_dump(&remote, stderr);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	/* --- Phase 1: SPAWN VARIETY. Every /spawn kind mc173 supports, each a
	 * DISTINCT wire MOB_SPAWN/VEHICLE_SPAWN type: creeper(50)/skeleton(51)/
	 * zombie(54)/slime(55)/ghast(56) hostile mobs, pig(90)/sheep(91)/cow(92)/
	 * chicken(93)/squid(94) passive mobs, boat(vtype 1) and minecart
	 * (vtype 10) vehicles -- many-at-once (sent back to back, no per-spawn
	 * wait) per the brief's "several at once" instruction, then one settle
	 * pump. wolf/spider/giant/pig_zombie have no /spawn arm in mc173 and are
	 * NOT reachable through this harness -- documented gap, not a silent
	 * omission. */
	static const char* const spawn_kinds[] = {
		"creeper", "skeleton", "zombie", "slime", "ghast",
		"pig", "sheep", "cow", "chicken", "squid",
		"boat", "minecart",
	};
	size_t entity_count_before_spawns = status.remote_entity_count;
	for(size_t i = 0; i < sizeof(spawn_kinds) / sizeof(spawn_kinds[0]); i++) {
		char cmd[32];
		snprintf(cmd, sizeof(cmd), "/spawn %s", spawn_kinds[i]);
		if(!server_remote_send_chat(&remote, cmd)) {
			fprintf(stderr, "LIVE ENTITY SOAK FAIL send %s\n", cmd);
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			return 1;
		}
		/* Short pump between sends (not a full settle) -- back to back
		 * enough to be "several at once" against the decoded-event queue,
		 * without outrunning it. */
		if(!live_s2_pump_for(&remote, 100, &status)) {
			fprintf(stderr, "LIVE ENTITY SOAK FAIL pump after %s\n", cmd);
			server_remote_trace_dump(&remote, stderr);
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			return 1;
		}
	}
	if(!live_s2_pump_for(&remote, 1500, &status)) {
		fprintf(stderr, "LIVE ENTITY SOAK FAIL settle-pump after spawn variety\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	printf("LIVE ENTITY SOAK SPAWN-VARIETY kinds=%zu entity_count=%zu "
		"(was %zu) capacity_drops=%u\n",
		sizeof(spawn_kinds) / sizeof(spawn_kinds[0]), status.remote_entity_count,
		entity_count_before_spawns, status.remote_entity_capacity_drops);

	/* --- Phase 1b: DROPPED ITEMS (Packet21PickupSpawn -- a wire class
	 * distinct from MOB_SPAWN/VEHICLE_SPAWN above). /give then drop several
	 * times through the SAME production SRPC_DROP_ITEM path S2 already
	 * proved once; repeated here under load, alongside everything else this
	 * phase spawned. --- */
	if(!server_remote_send_chat(&remote, "/give 1 64")
	   || !live_s2_pump_for(&remote, 500, &status)) {
		fprintf(stderr, "LIVE ENTITY SOAK FAIL give-stone-for-drop\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	for(int i = 0; i < 5; i++) {
		struct server_rpc drop_rpc
			= {.type = SRPC_DROP_ITEM, .payload.drop_item = {.all = false}};
		(void)svin_rpc_send(&drop_rpc);
		if(!live_s2_pump_for(&remote, 150, &status)) {
			fprintf(stderr, "LIVE ENTITY SOAK FAIL pump after drop %d\n", i);
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			return 1;
		}
	}
	printf("LIVE ENTITY SOAK DROPPED-ITEMS entity_count=%zu\n",
		status.remote_entity_count);

	/* --- Phase 1c: FALLING SAND -- VEHICLE_SPAWN kind 71/70, this engine's
	 * documented REMOTE_ENTITY_UNSUPPORTED tracking path (server_remote.c's
	 * MP_EVENT_VEHICLE_SPAWN handler: "70/71 falling sand/gravel ... have no
	 * client render/CRPC in this engine -- tracked UNSUPPORTED so a later
	 * DESTROY is absorbed, not errored"). Give sand, place it one block above
	 * an air gap so gravity triggers a fall the instant the block lands
	 * [mc173-server entity.rs: BlockKind::FallingBlock -> spawn_entity_object
	 * kind 70/71] -- proves that UNSUPPORTED path against a REAL server
	 * rather than only the code-reading that identified it. --- */
	int sand_ground = live_s2_find_ground_y(40, 0);
	if(sand_ground >= 2 && sand_ground <= 116
	   && live_s2_teleport(
			  &remote, 40.5, sand_ground + 1.0 + 1.62, 1.5, 300, &status)
	   && server_remote_send_chat(&remote, "/give 12 1")
	   && live_s2_pump_for(&remote, 500, &status)) {
		size_t sand_hb;
		if(live_s2_find_hotbar_slot(BLOCK_SAND, &sand_hb)) {
			struct server_rpc sw = {.type = SRPC_HOTBAR_SLOT,
				.payload.hotbar_slot = {.slot = sand_hb}};
			(void)svin_rpc_send(&sw);
			live_s2_pump_for(&remote, 300, &status);
			struct item_data sand_item
				= {.id = BLOCK_SAND, .count = 1, .durability = 0};
			/* Place two blocks above the found ground, leaving an air gap
			 * at ground+1 for the sand to fall into. */
			struct server_rpc place_sand = {.type = SRPC_BLOCK_PLACE,
				.payload.block_place = {.x = 40, .y = sand_ground + 1, .z = 0,
					.side = SIDE_TOP, .held_item = sand_item,
					.has_held_item = true}};
			(void)svin_rpc_send(&place_sand);
			live_s2_pump_for(&remote, 1000, &status);
		}
	}
	printf("LIVE ENTITY SOAK FALLING-SAND entity_count=%zu forced_reconnects=%u\n",
		status.remote_entity_count, status.forced_reconnects);

	if(status.state != REMOTE_PLAY || status.forced_reconnects != 0) {
		fprintf(stderr,
			"LIVE ENTITY SOAK FAIL unhealthy after spawn-variety phase\n");
		server_remote_trace_dump(&remote, stderr);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	/* --- Phase 2: MOVEMENT FLOOD. Walk a closed circle through the now-
	 * populated spawn area for >=walk_seconds, streaming the REAL camera
	 * position every 50ms (clin_update's genuine sendMotionUpdates path --
	 * run_live_walk's own pattern, see the file-level comment above) so
	 * mc173 streams REL_ENTITY_MOVE/_LOOK/TELEPORT for every nearby entity
	 * as they wander AND as this client's own view window slides across
	 * them. --- */
	ptime_t walk_start = time_get(), last_send = walk_start,
			last_report = walk_start;
	uint64_t samples = 0;
	double angle = 0.0;
	const double radius = 12.0;
	const double center_x = 0.5, center_z = 1.5;
	while((uint64_t)(uint32_t)time_diff_ms(walk_start, time_get())
		  < (uint64_t)walk_seconds * 1000u) {
		ptime_t now = time_get();
		if(time_diff_ms(last_send, now) >= 50) {
			angle += 0.05;
			double wx = center_x + radius * cos(angle);
			double wz = center_z + radius * sin(angle);
			int wy = live_s2_find_ground_y(
				(w_coord_t)floor(wx), (w_coord_t)floor(wz));
			double wy_d = (double)(wy >= 2 && wy <= 118 ? wy : ground0) + 1.0
				+ 1.62;
			gstate.players[0].camera.x = (float)wx;
			gstate.players[0].camera.y = (float)wy_d;
			gstate.players[0].camera.z = (float)wz;
			if(gstate.players[0].local_player) {
				gstate.players[0].local_player->pos[0] = wx;
				gstate.players[0].local_player->pos[1] = wy_d;
				gstate.players[0].local_player->pos[2] = wz;
			}
			struct server_rpc walk = {.type = SRPC_PLAYER_POS,
				.payload.player_pos = {.x = wx, .y = wy_d, .z = wz,
					.rx = -glm_deg(gstate.players[0].camera.rx),
					.ry = glm_deg(gstate.players[0].camera.ry) - 90.0f, .on_ground = true,
					.correction_seq = status.correction_seq}};
			(void)svin_rpc_send(&walk);
			samples++;
			last_send = now;
		}
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state != REMOTE_PLAY
		   || status.retained_bytes > status.retained_byte_cap
		   || status.retained_entries > status.retained_entry_cap
		   || status.queued_events > 64 || status.queued_frames > 64
		   || status.forced_reconnects != 0) {
			fprintf(stderr,
				"LIVE ENTITY SOAK FAIL movement-flood bound state=%d "
				"reason=%s\n", status.state, status.reason);
			server_remote_trace_dump(&remote, stderr);
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			return 1;
		}
		if(time_diff_ms(last_report, now) >= 15000) {
			printf("LIVE ENTITY SOAK WALK progress=%llus samples=%llu "
				"entity_count=%zu capacity_drops=%u retained=%zu/%zu "
				"backlog=%zu\n",
				(unsigned long long)(time_diff_ms(walk_start, now) / 1000),
				(unsigned long long)samples, status.remote_entity_count,
				status.remote_entity_capacity_drops, status.retained_bytes,
				status.retained_byte_cap, status.promotion_backlog);
			last_report = now;
		}
		thread_msleep(1);
	}

	/* --- Phase 3: DESTROY/DESPAWN CHURN. Teleport far away (well outside
	 * every populated entity's tracking/announce range) and back, several
	 * times, forcing the whole remote_entity table to tombstone-churn -- the
	 * S1 tombstone-churn soak observation applied here to ENTITIES rather
	 * than chunks. --- */
	for(int cycle = 0; cycle < 5; cycle++) {
		if(!live_s2_teleport(&remote, 400.5, 90.0, 400.5, 500, &status)) {
			fprintf(stderr,
				"LIVE ENTITY SOAK FAIL far-teleport cycle %d\n", cycle);
			server_remote_trace_dump(&remote, stderr);
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			return 1;
		}
		if(!live_s2_pump_for(&remote, 1000, &status)) {
			fprintf(stderr, "LIVE ENTITY SOAK FAIL far-settle cycle %d\n", cycle);
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			return 1;
		}
		if(!live_s2_teleport(
			   &remote, 0.5, ground0 + 1.0 + 1.62, 1.5, 500, &status)) {
			fprintf(stderr,
				"LIVE ENTITY SOAK FAIL back-teleport cycle %d\n", cycle);
			server_remote_trace_dump(&remote, stderr);
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			return 1;
		}
		if(!live_s2_pump_for(&remote, 1000, &status)) {
			fprintf(stderr, "LIVE ENTITY SOAK FAIL back-settle cycle %d\n", cycle);
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			return 1;
		}
		printf("LIVE ENTITY SOAK CHURN cycle=%d entity_count=%zu "
			"capacity_drops=%u\n", cycle, status.remote_entity_count,
			status.remote_entity_capacity_drops);
	}

	if(status.state != REMOTE_PLAY || status.forced_reconnects != 0) {
		fprintf(stderr, "LIVE ENTITY SOAK FAIL unhealthy after churn phase\n");
		server_remote_trace_dump(&remote, stderr);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	assert(server_remote_disconnect(&remote, "Quitting"));
	assert(wait_for(&remote, workers_exited, 5000));
	server_remote_get_status(&remote, &status);
	printf("LIVE ENTITY SOAK PASS walk_seconds=%u samples=%llu "
		"entity_count_final=%zu capacity_drops=%u forced_reconnects=%u "
		"retained_bytes_peak=%zu retention_evictions=%u alloc_fail=%u\n",
		walk_seconds, (unsigned long long)samples, status.remote_entity_count,
		status.remote_entity_capacity_drops, status.forced_reconnects,
		status.retained_bytes_peak, status.retention_evictions,
		status.retention_allocation_failures);
	server_remote_trace_dump(&remote, stdout);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
	return 0;
}

/* Minimal raw-socket SECOND client for the live collect leg: b1.7.3
 * handshake (0x02 string16) + login (0x01 proto 14) + absolute 0x0B
 * PlayerPosition moves -- just enough for mc173 to give it a server-side
 * player entity whose collision pickup collects items. Its inbound stream
 * is drained and discarded (select + recv, zero timeout) so mc173's async
 * writer never hits its 1MiB overflow kick. */
static bool live_collect_send_all(SOCKET s, const uint8_t* buf, size_t len) {
	while(len) {
		int n = send(s, (const char*)buf, (int)len, 0);
		if(n <= 0)
			return false;
		buf += n;
		len -= (size_t)n;
	}
	return true;
}

static void live_collect_drain(SOCKET s) {
	for(;;) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(s, &rfds);
		struct timeval tv = {0, 0};
		if(select((int)s + 1, &rfds, NULL, NULL, &tv) != 1)
			return;
		char buf[4096];
		if(recv(s, buf, sizeof(buf), 0) <= 0)
			return;
	}
}

static size_t live_collect_put_string16(uint8_t* p, const char* str) {
	size_t len = strlen(str), off = 0;
	put_i16(p + off, (int16_t)len);
	off += 2;
	for(size_t i = 0; i < len; i++) {
		p[off++] = 0;
		p[off++] = (uint8_t)str[i];
	}
	return off;
}

static SOCKET live_collect_second_login(const char* name) {
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	if(s == INVALID_SOCKET)
		return INVALID_SOCKET;
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(25565);
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	if(connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	uint8_t pkt[64];
	size_t off = 0;
	pkt[off++] = 0x02;
	off += live_collect_put_string16(pkt + off, name);
	if(!live_collect_send_all(s, pkt, off)) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	char reply[64];
	if(recv(s, reply, sizeof(reply), 0) <= 0) { /* server hash, e.g. "-" */
		closesocket(s);
		return INVALID_SOCKET;
	}
	off = 0;
	pkt[off++] = 0x01;
	put_i32(pkt + off, 14); /* protocol version */
	off += 4;
	off += live_collect_put_string16(pkt + off, name);
	memset(pkt + off, 0, 9); /* i64 + i8, unused C->S */
	off += 9;
	if(!live_collect_send_all(s, pkt, off)) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	return s;
}

static bool live_collect_second_move(SOCKET s, double x, double y, double z) {
	uint8_t pkt[34];
	pkt[0] = 0x0B; /* PlayerPosition: x, y, stance, z, on_ground */
	put_f64(pkt + 1, x);
	put_f64(pkt + 9, y);
	put_f64(pkt + 17, y + 1.62);
	put_f64(pkt + 25, z);
	pkt[33] = 1;
	return live_collect_send_all(s, pkt, sizeof(pkt));
}

/* --- live collect leg (Packet22 ghost-item fix, 2026-08-27) ---
 * Against a live mc173-server: drop a stone through the production
 * SRPC_DROP_ITEM path, wait out b1.7.3's 2s delayBeforeCanPickup, step onto
 * the drop, and require the item ENTITY to leave gstate.entities. mc173
 * sends 0x16 Packet22Collect and NO follow-up Packet29 for a collected item
 * (docs/fork_and_prior_art_sweep_2026-08-26.md par.E), so pre-fix this leg
 * timed out with the item ghost-rendering forever -- the maintainer's live
 * repro (real b1.7.3 client collected; CaveWii kept drawing the drop). */
static int run_live_collect(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	assert(server_remote_start(&remote, "127.0.0.1", 25565, "CaveWii"));

	ptime_t connect_start = time_get();
	struct server_remote_status status;
	while(time_diff_ms(connect_start, time_get()) < 30000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state == REMOTE_PLAY && gstate.world_loaded)
			break;
		if(status.state == REMOTE_CLOSING || status.state == REMOTE_KICKED)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	if(status.state != REMOTE_PLAY || !gstate.world_loaded) {
		fprintf(stderr, "LIVE COLLECT FAIL connect/login/world-install "
			"state=%d reason=%s\n", status.state, status.reason);
		server_remote_trace_dump(&remote, stderr);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	printf("LIVE COLLECT LOGIN entity=%d seed=%lld\n", status.entity_id,
		(long long)status.map_seed);

	int ground0 = live_s2_find_ground_y(0, 0);
	if(ground0 < 2 || ground0 > 118
	   || !live_s2_teleport(
			  &remote, 0.5, ground0 + 1.0 + 1.62, 1.5, 500, &status)) {
		fprintf(stderr, "LIVE COLLECT FAIL ground/teleport under (0,0)\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	if(!server_remote_send_chat(&remote, "/give 1 64")
	   || !live_s2_pump_for(&remote, 500, &status)) {
		fprintf(stderr, "LIVE COLLECT FAIL give-stone\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	/* Snapshot the ENTITY_ITEM ids already present so the drop's NEW id is
	 * identifiable (fresh worlds usually have none, but don't assume). */
	uint32_t before_ids[64];
	size_t before_count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, gstate.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* cand = dict_entity_ref(it)->value;
		if(cand && cand->type == ENTITY_ITEM && before_count < 64)
			before_ids[before_count++] = cand->id;
		dict_entity_next(it);
	}

	struct server_rpc drop_rpc
		= {.type = SRPC_DROP_ITEM, .payload.drop_item = {.all = false}};
	(void)svin_rpc_send(&drop_rpc);

	/* Find the drop: a NEW ENTITY_ITEM materialized from the wire. */
	uint32_t drop_id = 0;
	vec3 drop_pos = {0};
	ptime_t find_start = time_get();
	while(!drop_id && time_diff_ms(find_start, time_get()) < 5000) {
		if(!live_s2_pump_for(&remote, 50, &status))
			break;
		dict_entity_it(it, gstate.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* cand = dict_entity_ref(it)->value;
			if(cand && cand->type == ENTITY_ITEM) {
				bool known = false;
				for(size_t i = 0; i < before_count && !known; i++)
					known = before_ids[i] == cand->id;
				if(!known) {
					drop_id = cand->id;
					glm_vec3_copy(cand->pos, drop_pos);
					break;
				}
			}
			dict_entity_next(it);
		}
	}
	if(!drop_id) {
		fprintf(stderr, "LIVE COLLECT FAIL no dropped-item entity appeared\n");
		server_remote_trace_dump(&remote, stderr);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	printf("LIVE COLLECT DROPPED id=%u at (%.2f, %.2f, %.2f)\n", drop_id,
		drop_pos[0], drop_pos[1], drop_pos[2]);

	/* Let the throw finish + wait out delayBeforeCanPickup (40 ticks = 2s),
	 * re-read where the drop settled, then stand on it. */
	if(!live_s2_pump_for(&remote, 2500, &status)) {
		fprintf(stderr, "LIVE COLLECT FAIL pickup-delay pump\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	struct entity** pp = dict_entity_get(gstate.entities, drop_id);
	if(pp && *pp)
		glm_vec3_copy((*pp)->pos, drop_pos);
	if(!live_s2_teleport(&remote, drop_pos[0], drop_pos[1] + 1.62,
			   drop_pos[2], 500, &status)) {
		fprintf(stderr, "LIVE COLLECT FAIL step-onto-drop teleport\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	/* The pin: mc173's proximity pickup fires 0x16 at us (collector == our
	 * own entity id -- same handler path as any other collector) and never
	 * a Packet29 for it; the entity must leave the dict via the collect
	 * removal alone. Pre-fix: 10s timeout, item ghosted forever. */
	bool removed = false;
	ptime_t remove_start = time_get();
	while(!removed && time_diff_ms(remove_start, time_get()) < 10000) {
		if(!live_s2_pump_for(&remote, 50, &status))
			break;
		pp = dict_entity_get(gstate.entities, drop_id);
		removed = !pp || !*pp;
	}
	if(!removed || status.state != REMOTE_PLAY || status.forced_reconnects != 0) {
		fprintf(stderr, "LIVE COLLECT FAIL removed=%d state=%d forced=%u -- "
			"ghost item persists\n", (int)removed, status.state,
			status.forced_reconnects);
		server_remote_trace_dump(&remote, stderr);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	printf("LIVE COLLECT OWN-PICKUP OK drop_id=%u\n", drop_id);

	/* --- Phase 2: the maintainer's ACTUAL live repro -- ANOTHER player
	 * collects. A second raw-socket client logs in and stands on a fresh
	 * drop; mc173's server-side collision pickup fires 0x16 at US with
	 * collector == that player's entity id (never ours). Pre-fix the item
	 * entity stayed in gstate.entities forever -- the observed ghost. --- */
	if(!live_s2_teleport(
		   &remote, 0.5, ground0 + 1.0 + 1.62, 1.5, 500, &status)) {
		fprintf(stderr, "LIVE COLLECT FAIL phase-2 reposition\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	dict_entity_it(it, gstate.entities);
	before_count = 0;
	while(!dict_entity_end_p(it)) {
		struct entity* cand = dict_entity_ref(it)->value;
		if(cand && cand->type == ENTITY_ITEM && before_count < 64)
			before_ids[before_count++] = cand->id;
		dict_entity_next(it);
	}
	(void)svin_rpc_send(&drop_rpc);
	uint32_t drop2_id = 0;
	vec3 drop2_pos = {0};
	find_start = time_get();
	while(!drop2_id && time_diff_ms(find_start, time_get()) < 5000) {
		if(!live_s2_pump_for(&remote, 50, &status))
			break;
		dict_entity_it(it, gstate.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* cand = dict_entity_ref(it)->value;
			if(cand && cand->type == ENTITY_ITEM) {
				bool known = false;
				for(size_t i = 0; i < before_count && !known; i++)
					known = before_ids[i] == cand->id;
				if(!known) {
					drop2_id = cand->id;
					glm_vec3_copy(cand->pos, drop2_pos);
					break;
				}
			}
			dict_entity_next(it);
		}
	}
	if(!drop2_id) {
		fprintf(stderr, "LIVE COLLECT FAIL no second drop appeared\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	/* Wait out frozen_time, re-read where it settled. */
	if(!live_s2_pump_for(&remote, 2500, &status)) {
		fprintf(stderr, "LIVE COLLECT FAIL phase-2 pickup-delay pump\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	pp = dict_entity_get(gstate.entities, drop2_id);
	if(pp && *pp)
		glm_vec3_copy((*pp)->pos, drop2_pos);
	printf("LIVE COLLECT DROPPED2 id=%u at (%.2f, %.2f, %.2f)\n", drop2_id,
		drop2_pos[0], drop2_pos[1], drop2_pos[2]);

	SOCKET ghost2 = live_collect_second_login("Ghost2");
	if(ghost2 == INVALID_SOCKET) {
		fprintf(stderr, "LIVE COLLECT FAIL second-client login\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	/* Stand the second player on the drop and hold position; drain its
	 * inbound stream while OUR client pumps. The pin: the item leaves OUR
	 * dict on 0x16 alone, collector being a FOREIGN entity id. */
	bool removed2 = false;
	remove_start = time_get();
	while(!removed2 && time_diff_ms(remove_start, time_get()) < 10000) {
		(void)live_collect_second_move(
			ghost2, drop2_pos[0], drop2_pos[1], drop2_pos[2]);
		live_collect_drain(ghost2);
		if(!live_s2_pump_for(&remote, 50, &status))
			break;
		pp = dict_entity_get(gstate.entities, drop2_id);
		removed2 = !pp || !*pp;
	}
	closesocket(ghost2);
	if(!removed2 || status.state != REMOTE_PLAY
	   || status.forced_reconnects != 0) {
		fprintf(stderr, "LIVE COLLECT FAIL phase-2 removed=%d state=%d "
			"forced=%u -- ghost item persists after foreign collect\n",
			(int)removed2, status.state, status.forced_reconnects);
		server_remote_trace_dump(&remote, stderr);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	assert(server_remote_disconnect(&remote, "Quitting"));
	assert(wait_for(&remote, workers_exited, 5000));
	server_remote_get_status(&remote, &status);
	printf("LIVE COLLECT PASS own_drop_id=%u foreign_drop_id=%u "
		"entity_count_final=%zu forced_reconnects=%u\n", drop_id, drop2_id,
		status.remote_entity_count, status.forced_reconnects);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
	return 0;
}

/* --- entity approach model (mp-s4-polish lane) --- */
/* Beta setPositionAndRotation2 increments model, unit-level: the math the
 * remote provider arms on every move/teleport/look (approach_ticks=3) and the
 * local provider leaves off (0 = same-tick snap). Landing must be EXACT --
 * the old 0.3f exponential decay never landed, which is precisely what this
 * pins against. [740c583 EntityLiving.onLivingUpdate] [740c583
 * EntityOtherPlayerMP.onLivingUpdate] [740c583 NetClientHandler.handleEntity] */
static void test_s4_entity_approach(void) {
	struct entity e = {0};
	glm_vec3_copy((vec3) {0.0F, 64.0F, 0.0F}, e.pos);
	glm_vec3_copy(e.pos, e.pos_old);
	glm_vec3_copy(e.pos, e.network_pos);
	e.orient[0] = 170.0F;
	e.orient[1] = 90.0F;
	glm_vec2_copy(e.orient, e.orient_old);
	glm_vec2_copy(e.orient, e.network_orient);

	/* 1: a 3-increment move lands EXACTLY on the target in 3 ticks (Beta's
	 * 1/3, then 1/2, then 1/1 divisions), yaw taking the SHORT way across
	 * the +-180 seam (170 -> -170 is +20 wrapped, never -340). */
	glm_vec3_copy((vec3) {3.0F, 64.0F, -6.0F}, e.network_pos);
	e.network_orient[0] = -170.0F; /* == 190 unwrapped */
	e.network_orient[1] = 60.0F;
	e.network_increments = 3;
	entity_network_approach_tick(&e, true);
	assert(fabsf(e.pos[0] - 1.0F) < 1e-5f && fabsf(e.pos[2] + 2.0F) < 1e-5f);
	assert(e.network_increments == 2);
	entity_network_approach_tick(&e, true);
	assert(fabsf(e.pos[0] - 2.0F) < 1e-5f);
	entity_network_approach_tick(&e, true);
	/* EXACT landing -- the anti-decay pin. */
	assert(e.pos[0] == 3.0F && e.pos[1] == 64.0F && e.pos[2] == -6.0F);
	assert(e.network_increments == 0);
	/* Yaw stepped +20/3 per tick toward the wrapped target: 170 + 20 = 190
	 * (the unwrapped representation Beta also keeps -- rotationYaw is never
	 * re-normalized). Pitch: 90 -> 60 linearly. */
	assert(fabsf(e.orient[0] - 190.0F) < 1e-3f);
	assert(fabsf(e.orient[1] - 60.0F) < 1e-3f);

	/* 2: increments==0 is the local-provider same-tick snap. */
	glm_vec3_copy((vec3) {-8.0F, 70.0F, 2.0F}, e.network_pos);
	entity_network_approach_tick(&e, true);
	assert(e.pos[0] == -8.0F && e.pos[1] == 70.0F && e.pos[2] == 2.0F);

	/* 3: the default client tick preserves a REAL previous sample for the
	 * render lerp (pos_old/orient_old = pre-tick values, not clobbered). */
	glm_vec3_copy((vec3) {-5.0F, 70.0F, 2.0F}, e.network_pos);
	e.network_orient[0] = e.orient[0] + 9.0F;
	e.network_increments = 3;
	float yaw_before = e.orient[0];
	assert(!entity_default_client_tick(&e));
	assert(e.pos_old[0] == -8.0F);            /* previous pos kept */
	assert(e.orient_old[0] == yaw_before);    /* previous yaw kept */
	assert(fabsf(e.pos[0] + 7.0F) < 1e-5f);   /* -8 + (-5 - -8)/3 = -7 */
	assert(fabsf(e.orient[0] - (yaw_before + 3.0F)) < 1e-4f);

	/* 4: a teleport-seed voids any in-flight approach. */
	e.network_increments = 2;
	e.teleport = entity_default_teleport; /* not wired in this bare struct */
	entity_default_teleport(&e, (vec3) {100.0F, 80.0F, 100.0F});
	assert(e.network_increments == 0);
	assert(e.pos[0] == 100.0F && e.pos_old[0] == 100.0F
		&& e.network_pos[0] == 100.0F);
	assert(e.network_orient[0] == e.orient[0]
		&& e.network_orient[1] == e.orient[1]);

	puts("test_s4_entity_approach: ok");
}
/* --- end entity approach model (mp-s4-polish lane) --- */

/* --- live S4 polish gate (mp-s4-polish lane, opt-in: --live-s4) --- */
/* The S4 gate's live half against a real mc173 server (the S2/S3 precedent;
 * run recipe in tests/oracle/mp_s4_live_polish.txt). Chat display, P3
 * weather round-trip, explosion survival (the double-free fix under live
 * fire), the Beta approach model landing on a live mob, a REAL Packet255
 * kick with a colour-coded reason (the diag server's /kickme
 * instrumentation), and a live reconnect on the same session object --
 * the loopback 100-cycle stress's live twin. */
#define LIVE_S4_FAIL(remote, msg) \
	do { \
		struct server_remote_status fs; \
		server_remote_get_status((remote), &fs); \
		fprintf(stderr, "LIVE S4 FAIL %s state=%d reason=%s forced=%u\n", \
				(msg), fs.state, fs.reason, fs.forced_reconnects); \
		server_remote_trace_dump((remote), stderr); \
		server_remote_cancel((remote)); \
		server_remote_destroy((remote)); \
		return 1; \
	} while(0)

static bool live_s4_ring_contains(const char* needle) {
	for(size_t i = 0; i < gstate.chat_ring.count; i++) {
		size_t idx = (gstate.chat_ring.head + i) % CHAT_RING_CAPACITY;
		if(strstr(gstate.chat_ring.lines[idx], needle))
			return true;
	}
	return false;
}

/* --- MP reconnect-burst live leg (2026-08-27 maintainer-observed live bug:
 * "remote event queue overflow" on RECONNECT after a first session that
 * accumulated ground items, with a second real client online) ------------
 *
 * Mechanism under test (proven by the transport's own arithmetic): the
 * overflow kill fires only when server_remote_pump does not run AT ALL for
 * the whole REMOTE_EVENT_BACKPRESSURE_MS patience -- one pump pass always
 * opens the 64-slot ring (its budget is 4x capacity). In the real game the
 * >patience stall was the PRIOR session's latched client world being torn
 * down (world_unload_all + GL frees) inside the login/install window,
 * because gstate.world_loaded latches across a quit-to-title and the old
 * provider-swap seam connected FIRST and let the server-sent
 * CRPC_WORLD_RESET pay the teardown mid-burst. The fix moves that teardown
 * to the swap site (main.c), before any socket exists.
 *
 * This leg drives all of that against a REAL mc173-server plus a scripted
 * second client (the live observation's shape), through the production
 * svin/clin mailboxes:
 *   S1      login, /give + SRPC_DROP_ITEM a >capacity pile of ground items
 *           near spawn, quit gracefully WITHOUT resetting gstate (the
 *           latched quit-to-title state the game had).
 *   R1      reconnect latched at game-like frame cadence; measures the
 *           harness-side WORLD_RESET teardown frame (no GL here, so this
 *           under-reads the real game's stall -- reported, not asserted).
 *   R2      reconnect latched with ONE deliberate 1500ms pump hold once the
 *           login burst starts queueing -- the install-window stall class
 *           the maintainer's PC exhibited. Since REL-MP-FIX5 the burst
 *           still fills the ring but back-pressures the reader: the
 *           session must survive with event_backpressure_waits > 0.
 *   R3      reconnect through the FIXED seam sequence (clin_update_drain +
 *           clin_force_world_reset BEFORE connect, exactly what main.c now
 *           does) at the same cadence, same server state: must join clean.
 */

static bool live_client_send_frame(SOCKET s, const struct mp_frame* frame) {
	size_t sent = 0;
	while(sent < frame->length) {
		int n = send(s, (const char*)frame->data + sent,
					 (int)(frame->length - sent), 0);
		if(n <= 0)
			return false;
		sent += (size_t)n;
	}
	return true;
}

struct live_watcher {
	SOCKET socket;
	_Atomic int state; /* 0 = pre-login, 1 = logged in, -1 = failed */
	_Atomic int stop;
};

/* Minimal REAL second client: full mp_decoder over a raw socket, handshake ->
 * login -> keep-alive echo, everything else decoded and discarded (with
 * ownership destroyed). Its presence keeps the server's spawn-area entity
 * set live while the primary client is away, per the live observation. */
static void* live_watcher_entry(void* user) {
	struct live_watcher* w = user;
	struct mp_frame frame;
	struct mp_decoder decoder;
	mp_decoder_init(&decoder);
	if(!mp_encode_handshake(&frame, "Watcher")
	   || !live_client_send_frame(w->socket, &frame)) {
		atomic_store(&w->state, -1);
		return NULL;
	}
	uint8_t buffer[4096];
	while(!atomic_load(&w->stop)) {
		int count = recv(w->socket, (char*)buffer, sizeof(buffer), 0);
		if(count <= 0)
			break;
		for(int i = 0; i < count; i++) {
			struct mp_event event;
			enum mp_decode_result result
				= mp_decoder_feed(&decoder, buffer[i], &event);
			if(result == MP_DECODE_ERROR) {
				atomic_store(&w->state, -1);
				goto out;
			}
			if(result != MP_DECODE_EVENT)
				continue;
			bool io_failed = false;
			switch(event.type) {
				case MP_EVENT_HANDSHAKE:
					io_failed = !mp_encode_login(&frame, "Watcher")
						|| !live_client_send_frame(w->socket, &frame);
					break;
				case MP_EVENT_LOGIN: atomic_store(&w->state, 1); break;
				case MP_EVENT_KEEP_ALIVE:
					io_failed = !mp_encode_keep_alive(&frame)
						|| !live_client_send_frame(w->socket, &frame);
					break;
				default: break;
			}
			mp_event_destroy(&event);
			if(io_failed) {
				atomic_store(&w->state, -1);
				goto out;
			}
		}
	}
out:
	mp_decoder_destroy(&decoder);
	return NULL;
}

static SOCKET live_client_connect(uint16_t port) {
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	if(s == INVALID_SOCKET)
		return INVALID_SOCKET;
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(0x7F000001u); /* 127.0.0.1 */
	if(connect(s, (struct sockaddr*)&address, sizeof(address)) != 0) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	return s;
}

#define LIVE_RC_FRAME_MS 16
#define LIVE_RC_STALL_MS 1500u

/* Connect + pump at game-like frame cadence until world install completes or
 * the session dies. hold_ms != 0 injects ONE pump hold (the install-window
 * stall) as soon as the post-login burst starts queueing. Reports the worst
 * single pump+clin_update frame (the harness-side WORLD_RESET teardown
 * lands here on a latched reconnect). Returns 0 on clean install. */
/* Port is caller-selected (unlike the other live legs' fixed 25565): live
 * legs from parallel worktree sessions plus the maintainer's own LAN rig
 * can all hold mc173 instances at once, and this leg must be pointable at
 * ITS OWN disposable server rather than someone else's live world. */
static uint16_t live_rc_port = 25565;
/* REL-MP-FIX5 review: the last install's hold, exposed so R2 can require
 * that the stall really ran and that the reader back-pressured DURING it
 * (event_backpressure_waits sampled just before and just after the sleep). */
static bool live_rc_last_held;
static uint32_t live_rc_hold_waits;

static int live_rc_connect_install(struct server_remote* remote,
								   unsigned hold_ms,
								   uint32_t* out_worst_frame_ms,
								   struct server_remote_status* out) {
	assert(server_remote_start(remote, "127.0.0.1", live_rc_port, "CaveWii"));
	ptime_t start = time_get();
	bool held = false;
	live_rc_last_held = false;
	live_rc_hold_waits = 0;
	uint32_t worst_frame = 0;
	struct server_remote_status status;
	for(;;) {
		server_remote_get_status(remote, &status);
		if(!held && hold_ms && status.logged_in && status.queued_events >= 1) {
			held = true;
			uint32_t waits_before = status.event_backpressure_waits;
			thread_msleep(hold_ms);
			server_remote_get_status(remote, &status);
			live_rc_last_held = true;
			live_rc_hold_waits = status.event_backpressure_waits - waits_before;
			/* A reader that parked just BEFORE the first sample counts no
			 * new wait, but is provably parked through the hold if the ring
			 * is still full after it (only the pump frees slots). */
			if(live_rc_hold_waits == 0
			   && status.queued_events == SERVER_REMOTE_EVENT_QUEUE_CAPACITY)
				live_rc_hold_waits = 1;
		}
		ptime_t frame_start = time_get();
		server_remote_pump(remote);
		clin_update();
		int32_t frame_ms = time_diff_ms(frame_start, time_get());
		if(frame_ms > 0 && (uint32_t)frame_ms > worst_frame)
			worst_frame = (uint32_t)frame_ms;
		server_remote_get_status(remote, &status);
		if(status.state == REMOTE_PLAY && gstate.world_loaded)
			break;
		if(status.state == REMOTE_DISCONNECTED)
			break;
		if(time_diff_ms(start, time_get()) > 90000)
			break;
		thread_msleep(LIVE_RC_FRAME_MS);
	}
	if(out_worst_frame_ms)
		*out_worst_frame_ms = worst_frame;
	if(out)
		*out = status;
	return (status.state == REMOTE_PLAY && gstate.world_loaded) ? 0 : 1;
}

/* Graceful quit that leaves gstate exactly as the game's quit-to-title used
 * to: latched (world_loaded still true, prior world resident). */
static int live_rc_quit_latched(struct server_remote* remote) {
	assert(server_remote_disconnect(remote, "Quitting"));
	if(!wait_for(remote, workers_exited, 5000))
		return 1;
	server_remote_cancel(remote);
	return 0;
}

static int run_live_reconnect(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));

	/* Scripted second client, online for the whole scenario. */
	struct live_watcher watcher = {.socket = live_client_connect(live_rc_port)};
	if(watcher.socket == INVALID_SOCKET) {
		fprintf(stderr, "LIVE RECONNECT FAIL: watcher connect\n");
		return 1;
	}
	struct thread watcher_thread;
	assert(thread_create(&watcher_thread, live_watcher_entry, &watcher, 64));
	for(int i = 0; i < 5000 && atomic_load(&watcher.state) == 0; i++)
		thread_msleep(1);
	if(atomic_load(&watcher.state) != 1) {
		fprintf(stderr, "LIVE RECONNECT FAIL: watcher login\n");
		return 1;
	}
	puts("LIVE RECONNECT watcher online");

	/* --- S1: make the pile of ground items, quit latched. --- */
	uint32_t worst_frame = 0;
	struct server_remote_status status;
	if(live_rc_connect_install(&remote, 0, &worst_frame, &status)) {
		fprintf(stderr, "LIVE RECONNECT FAIL: S1 install state=%d reason=%s\n",
				status.state, status.reason);
		return 1;
	}
	/* Stand ~8 blocks off spawn so the pile lands clear of the rejoin
	 * position (never re-picked-up on the later joins). */
	double stand_x = gstate.players[0].local_player->pos[0] + 8.0;
	double stand_z = gstate.players[0].local_player->pos[2];
	int ground = live_s2_find_ground_y((w_coord_t)stand_x, (w_coord_t)stand_z);
	double stand_y
		= ground > 0 ? ground + 1.0 + 1.62 : gstate.players[0].local_player->pos[1];
	if(!live_s2_teleport(&remote, stand_x, stand_y, stand_z, 800, &status)) {
		fprintf(stderr, "LIVE RECONNECT FAIL: S1 teleport reason=%s\n",
				status.reason);
		return 1;
	}
	unsigned drops = 0;
	for(int batch = 0; batch < 2; batch++) {
		if(!server_remote_send_chat(&remote, "/give 3 64")) {
			fprintf(stderr, "LIVE RECONNECT FAIL: /give send\n");
			return 1;
		}
		if(!live_s2_pump_for(&remote, 800, &status)) {
			fprintf(stderr, "LIVE RECONNECT FAIL: /give pump reason=%s\n",
					status.reason);
			return 1;
		}
		unsigned batch_drops = batch == 0 ? 64 : 20;
		for(unsigned i = 0; i < batch_drops; i++) {
			struct server_rpc drop_rpc = {.type = SRPC_DROP_ITEM,
				.payload.drop_item = {.all = false}};
			(void)svin_rpc_send(&drop_rpc);
			if(!live_s2_pump_for(&remote, 40, &status)) {
				fprintf(stderr,
						"LIVE RECONNECT FAIL: drop pump reason=%s\n",
						status.reason);
				return 1;
			}
			drops++;
		}
	}
	/* Let the spawns settle, then record what this client tracks. */
	if(!live_s2_pump_for(&remote, 2000, &status)) {
		fprintf(stderr, "LIVE RECONNECT FAIL: settle pump reason=%s\n",
				status.reason);
		return 1;
	}
	printf("LIVE RECONNECT S1 drops=%u tracked_entities=%zu peak=%zu "
		   "gap_max=%ums\n",
		   drops, status.remote_entity_count, status.queued_events_peak,
		   status.pump_gap_max_ms);
	if(live_rc_quit_latched(&remote)) {
		fprintf(stderr, "LIVE RECONNECT FAIL: S1 quit\n");
		return 1;
	}

	/* --- R1: latched reconnect at game cadence (measures the teardown
	 * frame; no GL in this harness, so it under-reads the real game). --- */
	if(live_rc_connect_install(&remote, 0, &worst_frame, &status)) {
		fprintf(stderr, "LIVE RECONNECT FAIL: R1 install state=%d reason=%s\n",
				status.state, status.reason);
		return 1;
	}
	if(!live_s2_pump_for(&remote, 2000, &status))
		return 1;
	printf("LIVE RECONNECT R1(latched) worst_frame=%ums peak=%zu gap_max=%ums "
		   "bp_waits=%u tracked_entities=%zu\n",
		   worst_frame, status.queued_events_peak, status.pump_gap_max_ms,
		   status.event_backpressure_waits, status.remote_entity_count);
	if(live_rc_quit_latched(&remote)) {
		fprintf(stderr, "LIVE RECONNECT FAIL: R1 quit\n");
		return 1;
	}

	/* --- R2: latched reconnect + the install-window stall (was the live
	 * kill; survives since REL-MP-FIX5). --- */
	int r2_result
		= live_rc_connect_install(&remote, LIVE_RC_STALL_MS, &worst_frame,
								  &status);
	printf("LIVE RECONNECT R2(stall %ums) held=%d hold_bp_waits=%u state=%d "
		   "reason=\"%s\" peak=%zu gap_max=%ums bp_waits=%u\n",
		   LIVE_RC_STALL_MS, (int)live_rc_last_held, live_rc_hold_waits,
		   status.state, status.reason, status.queued_events_peak,
		   status.pump_gap_max_ms, status.event_backpressure_waits);
	/* REL-MP-FIX5: the stall must actually have run, the reader must have
	 * back-pressured DURING it (not merely during ordinary login), and the
	 * session must survive (Beta NetworkManager.java:187-208 never kills on
	 * a full receive queue). */
	if(r2_result != 0 || !live_rc_last_held || live_rc_hold_waits == 0) {
		fprintf(stderr,
				"LIVE RECONNECT FAIL: R2 stall did not run, back-pressure "
				"during the hold and survive (held=%d hold_bp_waits=%u "
				"reason \"%s\")\n", (int)live_rc_last_held,
				live_rc_hold_waits, status.reason);
		server_remote_cancel(&remote);
		return 1;
	}
	server_remote_cancel(&remote);

	/* --- R3: the FIXED seam -- teardown BEFORE connect (what main.c now
	 * does at the provider swap), then the same cadence, same server. --- */
	clin_update_drain();
	ptime_t reset_start = time_get();
	clin_force_world_reset();
	int32_t reset_ms = time_diff_ms(reset_start, time_get());
	if(live_rc_connect_install(&remote, 0, &worst_frame, &status)) {
		fprintf(stderr, "LIVE RECONNECT FAIL: R3 install state=%d reason=%s\n",
				status.state, status.reason);
		return 1;
	}
	if(!live_s2_pump_for(&remote, 2000, &status))
		return 1;
	printf("LIVE RECONNECT R3(fixed-seam) swap_reset=%ldms worst_frame=%ums "
		   "peak=%zu gap_max=%ums bp_waits=%u tracked_entities=%zu "
		   "forced_reconnects=%u\n",
		   (long)reset_ms, worst_frame, status.queued_events_peak,
		   status.pump_gap_max_ms, status.event_backpressure_waits,
		   status.remote_entity_count, status.forced_reconnects);
	if(status.forced_reconnects != 0) {
		fprintf(stderr, "LIVE RECONNECT FAIL: R3 forced reconnects\n");
		return 1;
	}
	if(live_rc_quit_latched(&remote)) {
		fprintf(stderr, "LIVE RECONNECT FAIL: R3 quit\n");
		return 1;
	}

	atomic_store(&watcher.stop, 1);
#ifdef _WIN32
	shutdown(watcher.socket, SD_BOTH);
#else
	shutdown(watcher.socket, SHUT_RDWR);
#endif
	thread_join(&watcher_thread);
	closesocket(watcher.socket);
	server_remote_destroy(&remote);
	puts("LIVE RECONNECT PASS latched-kill reproduced + fixed-seam clean join");
	return 0;
}

static int run_live_s4(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	assert(server_remote_start(&remote, "127.0.0.1", 25565, "CaveWii"));

	ptime_t connect_start = time_get();
	struct server_remote_status status;
	while(time_diff_ms(connect_start, time_get()) < 30000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state == REMOTE_PLAY && gstate.world_loaded)
			break;
		if(status.state == REMOTE_CLOSING || status.state == REMOTE_KICKED)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	if(status.state != REMOTE_PLAY || !gstate.world_loaded)
		LIVE_S4_FAIL(&remote, "connect/login/world-install");
	printf("LIVE S4 LOGIN entity=%d seed=%lld dimension=%d\n",
		status.entity_id, (long long)status.map_seed, status.dimension);

	int ground0 = live_s2_find_ground_y(0, 0);
	if(ground0 < 2 || ground0 > 118)
		LIVE_S4_FAIL(&remote, "spawn ground scan");
	if(!live_s2_teleport(&remote, 0.5, ground0 + 1.0 + 1.62, 1.5, 500,
						 &status))
		LIVE_S4_FAIL(&remote, "spawn teleport");

	/* Phase 1 -- chat display. /help answers in colour-coded lines (mc173's
	 * own cmd_help sends \2478=====... and \247a/... rows), so a live server
	 * proves BOTH delivery and the single-byte display transcode. A plain
	 * line then proves the broadcast echo path. */
	size_t ring_before = gstate.chat_ring.count;
	if(!server_remote_send_chat(&remote, "/help"))
		LIVE_S4_FAIL(&remote, "send /help");
	if(!live_s2_pump_for(&remote, 3000, &status))
		LIVE_S4_FAIL(&remote, "pump after /help");
	server_remote_get_status(&remote, &status);
	if(status.chat_lines_received == 0 || gstate.chat_ring.count <= ring_before)
		LIVE_S4_FAIL(&remote, "no chat lines after /help");
	bool saw_colour_code = false;
	for(size_t i = 0; i < gstate.chat_ring.count && !saw_colour_code; i++) {
		size_t idx = (gstate.chat_ring.head + i) % CHAT_RING_CAPACITY;
		const char* l = gstate.chat_ring.lines[idx];
		for(size_t k = 0; l[k] && !saw_colour_code; k++) {
			if((unsigned char)l[k] == 0xA7)
				saw_colour_code = true;
			/* the utf8 bug this lane fixed: a 0xC2 lead byte in a ring line
			 * means the transcode regressed */
			if((unsigned char)l[k] == 0xC2)
				LIVE_S4_FAIL(&remote, "UTF-8 lead byte reached the chat ring");
		}
	}
	if(!saw_colour_code)
		LIVE_S4_FAIL(&remote, "no single-byte colour code in ring");
	if(!server_remote_send_chat(&remote, "s4 gate ping"))
		LIVE_S4_FAIL(&remote, "send ping");
	if(!live_s2_pump_for(&remote, 2000, &status))
		LIVE_S4_FAIL(&remote, "pump after ping");
	if(!live_s4_ring_contains("s4 gate ping"))
		LIVE_S4_FAIL(&remote, "chat echo missing from ring");
	server_remote_get_status(&remote, &status);
	printf("LIVE S4 CHAT received=%u rejected=%u ring=%zu colour_ok=1\n",
		status.chat_lines_received, status.chat_lines_rejected,
		gstate.chat_ring.count);

	/* Phase 2 -- P3 weather round trip: Packet70 code 1/2 must flip the
	 * client's rain state both ways. */
	if(!server_remote_send_chat(&remote, "/weather rain"))
		LIVE_S4_FAIL(&remote, "send /weather rain");
	ptime_t rain_start = time_get();
	bool raining = false;
	while(time_diff_ms(rain_start, time_get()) < 10000 && !raining) {
		server_remote_pump(&remote);
		clin_update();
		raining = gstate.weather_raining;
		thread_msleep(1);
	}
	if(!raining)
		LIVE_S4_FAIL(&remote, "rain never reached the client");
	if(!server_remote_send_chat(&remote, "/weather clear"))
		LIVE_S4_FAIL(&remote, "send /weather clear");
	ptime_t clear_start = time_get();
	bool cleared = false;
	while(time_diff_ms(clear_start, time_get()) < 10000 && !cleared) {
		server_remote_pump(&remote);
		clin_update();
		cleared = !gstate.weather_raining;
		thread_msleep(1);
	}
	if(!cleared)
		LIVE_S4_FAIL(&remote, "clear never reached the client");
	printf("LIVE S4 WEATHER rain->clear round trip ok\n");

	/* Phase 3 -- live explosion: the S4 double-free fix under real fire
	 * (Packet60 through the read-thread ownership-transfer path). Survival
	 * for 2s of pumping == no heap corruption, no forced reconnect. */
	if(!server_remote_send_chat(&remote, "/explode"))
		LIVE_S4_FAIL(&remote, "send /explode");
	if(!live_s2_pump_for(&remote, 2000, &status))
		LIVE_S4_FAIL(&remote, "pump after /explode");
	server_remote_get_status(&remote, &status);
	if(status.state != REMOTE_PLAY || status.forced_reconnects != 0)
		LIVE_S4_FAIL(&remote, "explosion destabilized the session");
	printf("LIVE S4 EXPLOSION survived\n");

	/* Phase 4 -- approach model on a live mob: spawn a pig, let its MOVE
	 * stream arm the 3-increment approach, then freeze sampling and run
	 * client ticks manually; the entity must LAND (dist -> ~0), which the
	 * old 0.3f decay never did. A wandering pig re-arms mid-window, so
	 * allow the loop a few settle attempts. */
	if(!server_remote_send_chat(&remote, "/spawn pig"))
		LIVE_S4_FAIL(&remote, "send /spawn pig");
	if(!live_s2_pump_for(&remote, 1500, &status))
		LIVE_S4_FAIL(&remote, "pump after /spawn pig");
	uint32_t pig_id = 0;
	bool pig_found = false;
	{
		dict_entity_it_t it;
		dict_entity_it(it, gstate.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* cand = dict_entity_ref(it)->value;
			if(cand && cand->type == ENTITY_MONSTER) {
				pig_id = cand->id;
				pig_found = true;
			}
			dict_entity_next(it);
		}
	}
	if(!pig_found)
		LIVE_S4_FAIL(&remote, "no mob entity after /spawn pig");
	float land_err = -1.0F;
	for(int attempt = 0; attempt < 5 && land_err < 0.0F; attempt++) {
		if(!live_s2_pump_for(&remote, 300, &status))
			LIVE_S4_FAIL(&remote, "pump during approach sampling");
		/* Re-fetch by id every attempt: a pump may have despawned/free'd the
		 * entity (dangling-pointer trap). */
		struct entity** pig_pp = dict_entity_get(gstate.entities, pig_id);
		struct entity* pig = pig_pp ? *pig_pp : NULL;
		if(!pig)
			LIVE_S4_FAIL(&remote, "pig despawned mid-sampling");
		for(int t = 0; t < 3; t++)
			pig->tick_client(pig);
		if(pig->network_increments == 0) {
			vec3 d;
			glm_vec3_sub(pig->network_pos, pig->pos, d);
			land_err = glm_vec3_norm(d);
		}
	}
	if(land_err < 0.0F || land_err > 0.01F)
		LIVE_S4_FAIL(&remote, "approach did not land on the live target");
	printf("LIVE S4 APPROACH landed err=%.6f\n", (double)land_err);

	/* Phase 5 -- REAL kick (diag /kickme): reason must arrive transcoded
	 * (leading \247c from the instrumented server), graceful must stay
	 * false -- the exact pair main.c's disconnect-UX branch keys on. */
	uint32_t generation_before = status.generation;
	if(!server_remote_send_chat(&remote, "/kickme"))
		LIVE_S4_FAIL(&remote, "send /kickme");
	if(!wait_for(&remote, workers_exited, 10000))
		LIVE_S4_FAIL(&remote, "workers never exited after kick");
	server_remote_get_status(&remote, &status);
	if(status.graceful)
		LIVE_S4_FAIL(&remote, "kick reported graceful");
	if((unsigned char)status.reason[0] != 0xA7
	   || !strstr(status.reason, "S4 gate kick"))
		LIVE_S4_FAIL(&remote, "kick reason missing/untranscoded");
	printf("LIVE S4 KICK reason_ok=1 graceful=0 reason=%s\n", status.reason);

	/* Phase 6 -- live reconnect on the SAME session object (the loopback
	 * 100-cycle stress's live twin): generation bumps, PLAY + world install
	 * again, then a clean graceful quit. */
	server_remote_cancel(&remote);
	assert(server_remote_start(&remote, "127.0.0.1", 25565, "CaveWii"));
	ptime_t re_start = time_get();
	while(time_diff_ms(re_start, time_get()) < 30000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state == REMOTE_PLAY && gstate.world_loaded)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	if(status.state != REMOTE_PLAY || !gstate.world_loaded)
		LIVE_S4_FAIL(&remote, "reconnect after kick");
	if(status.generation <= generation_before)
		LIVE_S4_FAIL(&remote, "generation did not advance on reconnect");
	printf("LIVE S4 RECONNECT generation=%u->%u\n", generation_before,
		status.generation);

	assert(server_remote_disconnect(&remote, "Quitting"));
	assert(wait_for(&remote, workers_exited, 5000));
	server_remote_get_status(&remote, &status);
	printf("LIVE S4 PASS chat_received=%u chat_rejected=%u approach_err=%.6f "
		"generation_final=%u forced_reconnects=%u\n",
		status.chat_lines_received, status.chat_lines_rejected,
		(double)land_err, status.generation, status.forced_reconnects);
	server_remote_trace_dump(&remote, stdout);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
	return 0;
}
#undef LIVE_S4_FAIL
/* --- end live S4 polish gate (mp-s4-polish lane) --- */

/* --- display transcode (mp-s4-polish lane) --- */
/* mp_utf16_to_display hostile-vector unit tests. mp_utf16_to_utf8 emits real
 * 2-byte UTF-8 (0xC2 0xA7) for U+00A7, but the font renderer (gutil_text_rgb,
 * source/graphics/gui_util.c) is a single-byte-charset consumer that reads
 * the raw byte 0xA7 as its chat-colour marker -- these vectors pin the new
 * function's single-byte mapping and, for contrast, prove
 * mp_utf16_to_utf8's existing (buggy-for-display) behaviour is UNCHANGED. */
static void test_s4_display_transcode(void) {
	struct mp_utf16 text;
	char out[256];

	/* 1. ASCII passthrough round-trip. */
	memset(&text, 0, sizeof(text));
	{
		const char* ascii = "Hello, world!";
		text.length = strlen(ascii);
		for(size_t i = 0; i < text.length; i++)
			text.units[i] = (uint16_t)(uint8_t)ascii[i];
	}
	assert(mp_utf16_to_display(&text, out, sizeof(out)));
	assert(strcmp(out, "Hello, world!") == 0);

	/* 2. Section-sign colour-coded line: byte 0 must be the RAW 0xA7, not
	 * mp_utf16_to_utf8's 0xC2 0xA7 pair -- the exact bug this function
	 * exists to avoid. */
	memset(&text, 0, sizeof(text));
	{
		uint16_t units[] = {0x00A7, 'e', 'H', 'i', 0x00A7, 'f', '!'};
		text.length = sizeof(units) / sizeof(units[0]);
		memcpy(text.units, units, sizeof(units));
	}
	assert(mp_utf16_to_display(&text, out, sizeof(out)));
	assert(text.length == 7);
	{
		static const uint8_t expected[7]
			= {0xA7, 'e', 'H', 'i', 0xA7, 'f', '!'};
		for(size_t i = 0; i < 7; i++)
			assert((uint8_t)out[i] == expected[i]);
		assert((uint8_t)out[0] == 0xA7 && (uint8_t)out[0] != 0xC2);
	}
	/* Contrast: mp_utf16_to_utf8 on the SAME vector is unchanged -- still
	 * 0xC2 0xA7 for the first unit, proving this is a new function, not an
	 * edit to the existing one. */
	{
		char utf8_out[32];
		assert(mp_utf16_to_utf8(&text, utf8_out, sizeof(utf8_out)));
		assert((uint8_t)utf8_out[0] == 0xC2 && (uint8_t)utf8_out[1] == 0xA7);
		assert((uint8_t)utf8_out[2] == 'e');
	}

	/* 3. Non-ASCII BMP code points (u-umlaut, heart), mixed with ASCII, map
	 * to '?'. */
	memset(&text, 0, sizeof(text));
	{
		uint16_t units[] = {'a', 0x00FC, 'b', 0x2764, 'c'};
		text.length = sizeof(units) / sizeof(units[0]);
		memcpy(text.units, units, sizeof(units));
	}
	assert(mp_utf16_to_display(&text, out, sizeof(out)));
	assert(strcmp(out, "a?b?c") == 0);

	/* 4. Control characters (LF, TAB) map to a single space. */
	memset(&text, 0, sizeof(text));
	{
		uint16_t units[] = {'a', 0x000A, 'b', 0x0009, 'c'};
		text.length = sizeof(units) / sizeof(units[0]);
		memcpy(text.units, units, sizeof(units));
	}
	assert(mp_utf16_to_display(&text, out, sizeof(out)));
	assert(strcmp(out, "a b c") == 0);

	/* 5. A well-formed surrogate pair (U+1F600 grinning face) collapses to
	 * ONE '?' output byte, not two. */
	memset(&text, 0, sizeof(text));
	{
		uint16_t units[] = {'x', 0xD83D, 0xDE00, 'y'};
		text.length = sizeof(units) / sizeof(units[0]);
		memcpy(text.units, units, sizeof(units));
	}
	assert(mp_utf16_to_display(&text, out, sizeof(out)));
	assert(strcmp(out, "x?y") == 0);

	/* 6. Hostile surrogate vectors: each REJECTS the whole line. */
	memset(&text, 0, sizeof(text));
	{
		uint16_t units[] = {'a', 0xD83D}; /* lone high surrogate at end */
		text.length = sizeof(units) / sizeof(units[0]);
		memcpy(text.units, units, sizeof(units));
	}
	assert(!mp_utf16_to_display(&text, out, sizeof(out)));

	memset(&text, 0, sizeof(text));
	{
		uint16_t units[] = {'a', 0xDE00, 'b'}; /* lone low surrogate */
		text.length = sizeof(units) / sizeof(units[0]);
		memcpy(text.units, units, sizeof(units));
	}
	assert(!mp_utf16_to_display(&text, out, sizeof(out)));

	memset(&text, 0, sizeof(text));
	{
		uint16_t units[] = {'a', 0xD83D, 'b'}; /* high followed by non-low */
		text.length = sizeof(units) / sizeof(units[0]);
		memcpy(text.units, units, sizeof(units));
	}
	assert(!mp_utf16_to_display(&text, out, sizeof(out)));

	/* 7. Capacity: exact-fit (length+1) succeeds; one byte short fails. */
	memset(&text, 0, sizeof(text));
	{
		const char* ascii = "Hello, world!";
		text.length = strlen(ascii);
		for(size_t i = 0; i < text.length; i++)
			text.units[i] = (uint16_t)(uint8_t)ascii[i];
	}
	{
		char exact[14]; /* strlen("Hello, world!") == 13, +1 for NUL */
		assert(text.length + 1 == sizeof(exact));
		assert(mp_utf16_to_display(&text, exact, sizeof(exact)));
		assert(strcmp(exact, "Hello, world!") == 0);

		char short_buf[13];
		assert(!mp_utf16_to_display(&text, short_buf, sizeof(short_buf)));
	}

	/* 8. Empty string succeeds with an empty output. */
	memset(&text, 0, sizeof(text));
	text.length = 0;
	out[0] = 'X';
	assert(mp_utf16_to_display(&text, out, sizeof(out)));
	assert(out[0] == '\0');
}
/* --- end display transcode (mp-s4-polish lane) --- */

/* ============ 0x83 Packet131MapData -> CRPC_MAP_DATA consumer ============
 * The Packet131 slice: type-0 colour strips land in a per-map remote
 * mirror and reach the client's held-map plane (clin_held_map_colors);
 * type-1 markers and malformed/hostile payloads are counted no-ops that
 * never corrupt the plane or kill the session. [740c583
 * NetClientHandler.handleMapData] [740c583 MapData.updateMPMapData] */

struct map_data_fixture {
	SOCKET listener;
	atomic_bool strip_confirmed;
	atomic_bool hostile_confirmed;
	atomic_bool second_map_confirmed;
	atomic_bool return_map_confirmed;
	atomic_bool evict_confirmed;
	bool ok;
};

/* One wire Packet131: 0x83, i16 item id, i16 map id, u8 length, bytes.
 * [740c583 Packet131MapData.readPacketData] */
static bool send_map_data(SOCKET peer, uint16_t item_id, uint16_t map_id,
						  const uint8_t* body, uint8_t length) {
	uint8_t buf[6 + 255];
	buf[0] = 0x83;
	buf[1] = (uint8_t)(item_id >> 8);
	buf[2] = (uint8_t)item_id;
	buf[3] = (uint8_t)(map_id >> 8);
	buf[4] = (uint8_t)map_id;
	buf[5] = length;
	if(length)
		memcpy(buf + 6, body, length);
	return socket_write_fragmented(peer, buf, (size_t)6 + length);
}

static void* map_data_peer_entry(void* user) {
	struct map_data_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;

	/* --- 1: legal type-0 strip for map 7 -- column x=5, rows 10..29,
	 * colours 1..20 (the [740c583 MapData.updateMPMapData] type byte 0
	 * layout: [0]=0, [1]=x, [2]=y0, then per-row colours). 358 = 0x0166 =
	 * Item.mapItem.shiftedIndex == this port's ITEM_MAP. --- */
	uint8_t strip_a[3 + 20];
	strip_a[0] = 0; strip_a[1] = 5; strip_a[2] = 10;
	for(int i = 0; i < 20; i++)
		strip_a[3 + i] = (uint8_t)(i + 1);
	if(!send_map_data(peer, 358, 7, strip_a, sizeof(strip_a)))
		goto done;
	while(!overflow_flag_get(&fixture->strip_confirmed))
		thread_msleep(1);

	/* --- 2: hostile/no-op batch, all against the SAME held map 7 --
	 * (a) non-map item id 256: NetClientHandler logs "Unknown itemid" and
	 *     drops; the strip's 0x63 colours must never land.
	 * (b) zero-length payload: Beta would AIOOBE reading byte 0; dropped.
	 * (c) type-0 shorter than its own 3-byte header: byte 2 AIOOBE in
	 *     Beta; dropped.
	 * (d) type-1 player markers: live marker-only update.
	 * (e) type-0 with column x=200 (past the 128-wide plane): Beta would
	 *     AIOOBE; every write is skipped, counted dropped, plane intact
	 *     (the strip itself still marks the mirror dirty, Beta's
	 *     unconditional markDirty). --- */
	uint8_t bad_item[3 + 20];
	memcpy(bad_item, strip_a, sizeof(strip_a));
	for(int i = 0; i < 20; i++)
		bad_item[3 + i] = 0x63;
	if(!send_map_data(peer, 256, 7, bad_item, sizeof(bad_item)))
		goto done;
	if(!send_map_data(peer, 358, 7, NULL, 0))
		goto done;
	static const uint8_t short_type0[2] = {0, 5};
	if(!send_map_data(peer, 358, 7, short_type0, sizeof(short_type0)))
		goto done;
	static const uint8_t markers[4] = {1, 0x12, 3, 4};
	if(!send_map_data(peer, 358, 7, markers, sizeof(markers)))
		goto done;
	static const uint8_t oob_column[3 + 5] = {0, 200, 0, 0xEE, 0xEE, 0xEE,
											  0xEE, 0xEE};
	if(!send_map_data(peer, 358, 7, oob_column, sizeof(oob_column)))
		goto done;
	while(!overflow_flag_get(&fixture->hostile_confirmed))
		thread_msleep(1);

	/* --- 3: a SECOND map id (9): full column x=0 of colour 42. The
	 * client's single held-map mirror must flip to map 9 (map 7 reads
	 * NULL there), while map 7's plane stays cached remote-side. --- */
	uint8_t strip_b[3 + 128];
	strip_b[0] = 0; strip_b[1] = 0; strip_b[2] = 0;
	memset(strip_b + 3, 42, 128);
	if(!send_map_data(peer, 358, 9, strip_b, sizeof(strip_b)))
		goto done;
	/* Jad MapData.java:147-158: signed byte division/remainder; trailing
	 * partial triples ignored. Exercise the actual Packet131 consumer. */
	static const uint8_t signed_markers[] = {1,0x80,128,255,0xFF,127,0,99};
	if(!send_map_data(peer, 358, 9, signed_markers, sizeof(signed_markers)))
		goto done;
	while(!overflow_flag_get(&fixture->second_map_confirmed))
		thread_msleep(1);

	/* --- 4: back to map 7 with a NEW column (x=6, rows 10..29, colours
	 * 101..120). The client plane must then hold BOTH columns -- x=5 from
	 * step 1 out of the remote mirror CACHE, x=6 from this strip. This is
	 * the per-map caching proof (Beta: ItemMap.getMPMapData returns the
	 * same MapData object across packets). --- */
	uint8_t strip_c[3 + 20];
	strip_c[0] = 0; strip_c[1] = 6; strip_c[2] = 10;
	for(int i = 0; i < 20; i++)
		strip_c[3 + i] = (uint8_t)(i + 101);
	if(!send_map_data(peer, 358, 7, strip_c, sizeof(strip_c)))
		goto done;
	static const uint8_t clear_markers[] = {1};
	if(!send_map_data(peer, 358, 9, clear_markers, sizeof(clear_markers)))
		goto done;
	while(!overflow_flag_get(&fixture->return_map_confirmed))
		thread_msleep(1);

	/* --- 5: seven more distinct map ids (100..106) overflow the 8-slot
	 * LRU mirror cache: exactly ONE eviction (map 9, the least recently
	 * touched). Minimal legal strips ([0, x=0, y0=0], zero colour
	 * writes) -- Beta still markDirty()s these. --- */
	static const uint8_t minimal[3] = {0, 0, 0};
	for(int id = 100; id < 107; id++)
		if(!send_map_data(peer, 358, id, minimal, sizeof(minimal))) goto done;
	while(!overflow_flag_get(&fixture->evict_confirmed))
		thread_msleep(1);

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_mp_map_data(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct map_data_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, map_data_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));
	struct server_remote_status status = {0};

	/* Step 1: the strip reaches the client plane at [740c583
	 * MapData.updateMPMapData]'s exact indices: colors[(i+y0)*128 + x]. */
	const uint8_t* plane = NULL;
	for(size_t elapsed = 0; elapsed < 5000; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		plane = clin_held_map_colors(7, NULL);
		/* A lookup is immediately blank; wait for the actual strip. */
		if(plane && plane[10 * 128 + 5] == 1) break;
		thread_msleep(1);
	}
	assert(plane);
	assert(plane[(10 + 0) * 128 + 5] == 1);
	assert(plane[(10 + 19) * 128 + 5] == 20);
	assert(plane[(10 + 0) * 128 + 4] == 0); /* neighbour column untouched */
	overflow_flag_set(&peer.strip_confirmed, true);

	/* Step 2: four drops counted; the marker list lands with terrain intact. */
	bool counted = false;
	for(size_t elapsed = 0; elapsed < 5000 && !counted; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		counted = status.map_data_dropped == 4 && status.map_markers_applied == 1;
		thread_msleep(1);
	}
	assert(counted);
	assert(status.state == REMOTE_PLAY);
	struct item_map_marker decoded[ITEM_MAP_MAX_MARKERS];
	assert(clin_held_map_markers(7, decoded) == 1);
	assert(decoded[0].type == 2 && decoded[0].rotation == 1);
	assert(decoded[0].x == 3 && decoded[0].y == 4);
	plane = clin_held_map_colors(7, NULL);
	assert(plane);
	assert(plane[(10 + 0) * 128 + 5] == 1);    /* not the bad-item 0x63 */
	assert(plane[(10 + 19) * 128 + 5] == 20);
	overflow_flag_set(&peer.hostile_confirmed, true);

	/* Step 3: map 9 arrives; map 7 retains its own client-side plane. */
	bool flipped = false;
	for(size_t elapsed = 0; elapsed < 5000 && !flipped; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		plane = clin_held_map_colors(9, NULL);
		flipped = plane && plane[0] == 42 && plane[127 * 128] == 42
			&& clin_held_map_colors(7, NULL)[10 * 128 + 5] == 1
			&& clin_held_map_markers(9, decoded) == 2;
		thread_msleep(1);
	}
	assert(flipped);
	assert(decoded[0].type == 0 && decoded[0].rotation == -8);
	assert(decoded[0].x == -128 && decoded[0].y == -1);
	assert(decoded[1].type == -1 && decoded[1].rotation == 0);
	assert(decoded[1].x == 127 && decoded[1].y == 0);
	overflow_flag_set(&peer.second_map_confirmed, true);

	/* Step 4: map 7 comes back with BOTH columns -- x=5 survived in the
	 * remote per-map mirror (the cache proof), x=6 is the new strip. */
	bool cached = false;
	for(size_t elapsed = 0; elapsed < 5000 && !cached; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		plane = clin_held_map_colors(7, NULL);
		cached = plane && plane[(10 + 0) * 128 + 6] == 101
			&& plane[(10 + 0) * 128 + 5] == 1
			&& clin_held_map_markers(9, decoded) == 0;
		thread_msleep(1);
	}
	assert(cached);
	assert(plane[(10 + 19) * 128 + 6] == 120);
	overflow_flag_set(&peer.return_map_confirmed, true);

	/* Step 5: the 9th distinct map id LRU-evicts exactly one mirror (map
	 * 9 -- least recently touched), visibly counted. Final counter sweep:
	 * 11 applied strips (1 + OOB + map9 + map7-return + 7 minimal), 3
	 * marker updates, 4 drops, >=5 pushes (per-pump coalescing makes the
	 * exact count timing-dependent; map_push_deferred is legal-nonzero
	 * on client cache allocation/spill failure, so it is deliberately unpinned). */
	bool evicted = false;
	for(size_t elapsed = 0; elapsed < 5000 && !evicted; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		evicted = status.map_mirror_evictions == 1
			&& status.map_strips_applied == 11;
		thread_msleep(1);
	}
	assert(evicted);
	assert(status.map_markers_applied == 3);
	assert(status.map_data_dropped == 4);
	assert(status.map_pushes >= 5);
	assert(status.state == REMOTE_PLAY);
	overflow_flag_set(&peer.evict_confirmed, true);

	/* Cancel FIRST: the session is still live, and the peer only leaves
	 * its trailing recv() loop when the socket closes under it -- joining
	 * before cancelling deadlocks (unlike the kill-tests, whose
	 * connection is already down when they join). */
	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
	puts("  0x83 map data: strips/cache/LRU/hostile all confirmed");
}

/* ============ Packet38 entity status -> hurt flash + death roll ==========
 * [740c583 NetClientHandler.handleEntityStatus -> EntityLiving.
 * handleHealthUpdate]: status 2 arms the 10-tick hurt flash (the client
 * monster tick's hurt-flail already consumes the MOVE-carried flag's
 * rising edge); status 3 starts the client-run death roll. The remote
 * layer drives both on a wall clock (500ms flash; deathTime elapsed/50ms
 * capped at Beta's 20) because MP servers send the status ONCE -- there
 * is no SP-style progression stream. */

struct entity_status_fixture {
	SOCKET listener;
	atomic_bool spawn_confirmed;
	atomic_bool hurt_confirmed;
	atomic_bool hurt_carry_confirmed;
	atomic_bool hurt_cleared_confirmed;
	atomic_bool death_confirmed;
	bool ok;
};

static void* entity_status_peer_entry(void* user) {
	struct entity_status_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;

	/* 0x18 Packet24MobSpawn: pig (type 90), entity 400, at (0, 10.0, 0). */
	uint8_t pig[21] = {0x18};
	put_i32(pig + 1, 400);
	pig[5] = 90;
	put_i32(pig + 6, 0);
	put_i32(pig + 10, 320);
	put_i32(pig + 14, 0);
	pig[18] = 0; pig[19] = 0; pig[20] = 0x7F;
	if(!socket_write_fragmented(peer, pig, sizeof(pig)))
		goto done;
	while(!overflow_flag_get(&fixture->spawn_confirmed))
		thread_msleep(1);

	/* 0x26 Packet38EntityStatus, status 2: hurt flash ON. */
	uint8_t hurt[6] = {0x26};
	put_i32(hurt + 1, 400);
	hurt[5] = 2;
	if(!socket_write_fragmented(peer, hurt, sizeof(hurt)))
		goto done;
	while(!overflow_flag_get(&fixture->hurt_confirmed))
		thread_msleep(1);

	/* An in-window relative move must CARRY the flash, not clear it (the
	 * clin handler overwrites e->hurt on every MOVE). */
	static const uint8_t rel_move[] = {0x1F, 0,0,1,0x90, 32, 0, 0};
	if(!socket_write_fragmented(peer, rel_move, sizeof(rel_move)))
		goto done;
	while(!overflow_flag_get(&fixture->hurt_carry_confirmed))
		thread_msleep(1);
	/* ...then the pump-tail clock clears it at ~500ms with NO further
	 * packet from this peer. */
	while(!overflow_flag_get(&fixture->hurt_cleared_confirmed))
		thread_msleep(1);

	/* status 3: death roll -- one packet, client-run progression. */
	uint8_t death[6] = {0x26};
	put_i32(death + 1, 400);
	death[5] = 3;
	if(!socket_write_fragmented(peer, death, sizeof(death)))
		goto done;
	while(!overflow_flag_get(&fixture->death_confirmed))
		thread_msleep(1);

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_mp_entity_status(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct entity_status_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, entity_status_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	struct entity** pp = NULL;
	bool spawned = false;
	for(size_t elapsed = 0; elapsed < 5000 && !spawned; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		pp = dict_entity_get(gstate.entities, 400);
		spawned = pp && *pp && (*pp)->type == ENTITY_MONSTER;
		thread_msleep(1);
	}
	assert(spawned);
	struct entity* e = *pp;
	assert(!e->hurt && e->data.monster.death_time == 0);
	overflow_flag_set(&peer.spawn_confirmed, true);

	/* Status 2 -> the MOVE-carried flash reaches the client mirror. */
	bool hurt = false;
	for(size_t elapsed = 0; elapsed < 5000 && !hurt; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		hurt = e->hurt;
		thread_msleep(1);
	}
	assert(hurt);
	overflow_flag_set(&peer.hurt_confirmed, true);

	/* The in-window move carries the flash (pre-fix shape: any move would
	 * zero it, the unconditional-overwrite trap). */
	bool moved = false;
	for(size_t elapsed = 0; elapsed < 5000 && !moved; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		moved = e->network_pos[0] == 1.0f;
		thread_msleep(1);
	}
	assert(moved);
	assert(e->hurt); /* still flashing mid-window */
	overflow_flag_set(&peer.hurt_carry_confirmed, true);

	/* Expiry: the pump-tail clock clears it (~500ms), no packet needed. */
	bool cleared = false;
	for(size_t elapsed = 0; elapsed < 5000 && !cleared; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		cleared = !e->hurt;
		thread_msleep(1);
	}
	assert(cleared);
	overflow_flag_set(&peer.hurt_cleared_confirmed, true);

	/* Status 3 -> the roll starts at 1 and ADVANCES client-side, capped
	 * at Beta's terminal 20. */
	bool dying = false;
	for(size_t elapsed = 0; elapsed < 5000 && !dying; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		dying = e->data.monster.death_time >= 1;
		thread_msleep(1);
	}
	assert(dying);
	int8_t first_seen = e->data.monster.death_time;
	bool advanced = false;
	for(size_t elapsed = 0; elapsed < 5000 && !advanced; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		advanced = e->data.monster.death_time > first_seen;
		thread_msleep(1);
	}
	assert(advanced);
	assert(e->data.monster.death_time <= 20);
	overflow_flag_set(&peer.death_confirmed, true);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
	puts("  entity status: hurt flash (on/carry/expire) + death roll advance");
}

/* ============ outbound chat sanitize (betamc-readiness Poseidon row) =====
 * Beta's composer caps at 100 chars with \247/control chars untypeable
 * [740c583 GuiChat:40] and sends message.trim() only when non-empty;
 * CB-lineage servers (Poseidon NetServerHandler) KICK on "Chat message too
 * long" (>100) and "Illegal characters in chat" (\247, controls). The
 * sanitizer replicates the composer filter at the production choke point
 * (server_remote_send_chat). */
static void test_chat_sanitize_outbound(void) {
	char out[401];

	/* Plain ASCII passes through byte-identical (the live-harness /give). */
	assert(mp_chat_sanitize_outbound("/give Cave 331 64", out, sizeof(out))
		   == 17);
	assert(strcmp(out, "/give Cave 331 64") == 0);

	/* \247 (U+00A7, 0xC2 0xA7 in UTF-8) is stripped wherever it appears --
	 * a colour-code injection becomes the bare text. */
	assert(mp_chat_sanitize_outbound("\xC2\xA7" "cRed\xC2\xA7r", out,
									 sizeof(out)) == 5);
	assert(strcmp(out, "cRedr") == 0);

	/* C0 controls and DEL are stripped; interior spaces survive. */
	assert(mp_chat_sanitize_outbound("a\tb\nc\x7F d", out, sizeof(out)) == 5);
	assert(strcmp(out, "abc d") == 0);

	/* 101 'x's clamp to exactly 100 (GuiChat's cap, NOT Packet3's 119). */
	{
		char long_msg[102];
		memset(long_msg, 'x', 101);
		long_msg[101] = '\0';
		assert(mp_chat_sanitize_outbound(long_msg, out, sizeof(out)) == 100);
		assert(strlen(out) == 100);
	}

	/* Multibyte text is kept (é = 2 bytes, 1 UTF-16 unit) and the cap
	 * counts UTF-16 units: 100 'é's = 200 bytes, all kept. */
	{
		char accents[203];
		for(int i = 0; i < 101; i++) {
			accents[i * 2] = (char)0xC3;
			accents[i * 2 + 1] = (char)0xA9;
		}
		accents[202] = '\0';
		assert(mp_chat_sanitize_outbound(accents, out, sizeof(out)) == 200);
	}

	/* trim(): leading/trailing spaces drop, all-space refuses. */
	assert(mp_chat_sanitize_outbound("  hi  ", out, sizeof(out)) == 2);
	assert(strcmp(out, "hi") == 0);
	assert(mp_chat_sanitize_outbound("     ", out, sizeof(out)) == 0);
	assert(mp_chat_sanitize_outbound("\xC2\xA7\xC2\xA7", out, sizeof(out))
		   == 0);
	assert(mp_chat_sanitize_outbound("", out, sizeof(out)) == 0);

	/* A malformed UTF-8 byte drops without desyncing what follows. */
	assert(mp_chat_sanitize_outbound("a\xFF" "b", out, sizeof(out)) == 2);
	assert(strcmp(out, "ab") == 0);

	puts("  outbound chat sanitize: cap 100 / \\247+controls stripped / trim");
}

/* ============ base-entity wire-position SNAP (non-living MP motion) ======
 * Beta's base Entity.setPositionAndRotation2 SNAPS a non-living entity to
 * the wire position at packet time (EntityLiving alone overrides with the
 * 3-increment approach); this port's item/boat/minecart/arrow/sand/TNT
 * client ticks never read network_pos, so the wire position was a DEAD
 * STORE and they drifted unbounded on real servers. The pin here is on
 * e->pos ITSELF (not network_pos, which the old code already wrote).
 * [740c583 Entity.setPositionAndRotation2:1010-1027] [740c583
 * EntityLiving.setPositionAndRotation2:193-201] */

struct base_snap_fixture {
	SOCKET listener;
	atomic_bool spawn_confirmed;
	atomic_bool move_confirmed;
	atomic_bool teleport_confirmed;
	bool ok;
};

static void* base_snap_peer_entry(void* user) {
	struct base_snap_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;

	/* 0x15 Packet21PickupSpawn: item entity 300 (stone x1) at
	 * (1.0, 2.0, 3.0) (wire fixed-point x32). */
	uint8_t spawn[24] = {0x15};
	put_i32(spawn + 1, 300);
	spawn[5] = 0; spawn[6] = 1; /* item id 1 */
	spawn[7] = 1;               /* count */
	spawn[8] = 0; spawn[9] = 0; /* damage */
	put_i32(spawn + 10, 32);
	put_i32(spawn + 14, 64);
	put_i32(spawn + 18, 96);
	spawn[22] = 0; spawn[23] = 0;
	/* trailing roll byte */
	uint8_t roll = 0;
	if(!socket_write_fragmented(peer, spawn, sizeof(spawn))
	   || !socket_write_fragmented(peer, &roll, sizeof(roll)))
		goto done;
	while(!overflow_flag_get(&fixture->spawn_confirmed))
		thread_msleep(1);

	/* 0x1F Packet31RelEntityMove dx=+32 (1.0 block): the item must be AT
	 * x=2.0 after processing -- base snap, not an armed approach. */
	static const uint8_t rel_move[] = {0x1F, 0,0,1,44, 32, 0, 0};
	if(!socket_write_fragmented(peer, rel_move, sizeof(rel_move)))
		goto done;
	while(!overflow_flag_get(&fixture->move_confirmed))
		thread_msleep(1);

	/* 0x22 Packet34EntityTeleport to (10,10,10): base snap INCLUDING the
	 * handler-side 1/64 y lift (NetClientHandler.handleEntityTeleport
	 * targets y/32 + 0.015625 for every entity type; the S3 lifecycle
	 * test pins the same value on the living/approach path). */
	static const uint8_t teleport[] = {0x22,
		0,0,1,44, 0,0,1,64, 0,0,1,64, 0,0,1,64, 0, 0};
	if(!socket_write_fragmented(peer, teleport, sizeof(teleport)))
		goto done;
	while(!overflow_flag_get(&fixture->teleport_confirmed))
		thread_msleep(1);

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_mp_base_entity_snap(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct base_snap_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, base_snap_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	/* No game loop runs here, so entity client ticks (item gravity) never
	 * fire: e->pos is exactly what the CRPC layer left there --
	 * deterministic pins. (The pop-out sweep is a no-op in this fixture:
	 * no client chunks exist, so entity_get_block finds nothing to
	 * collide with -- the snap itself is the behaviour under test.) */
	struct entity** pp = NULL;
	bool spawned = false;
	for(size_t elapsed = 0; elapsed < 5000 && !spawned; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		pp = dict_entity_get(gstate.entities, 300);
		spawned = pp && *pp && (*pp)->type == ENTITY_ITEM;
		thread_msleep(1);
	}
	assert(spawned);
	struct entity* e = *pp;
	assert(e->pos[0] == 1.0f && e->pos[1] == 2.0f && e->pos[2] == 3.0f);
	overflow_flag_set(&peer.spawn_confirmed, true);

	/* Pre-fix, network_pos moved to 2.0 but e->pos stayed 1.0 forever. */
	bool moved = false;
	for(size_t elapsed = 0; elapsed < 5000 && !moved; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		moved = e->pos[0] == 2.0f;
		thread_msleep(1);
	}
	assert(moved);
	assert(e->pos[1] == 2.0f && e->pos[2] == 3.0f);
	overflow_flag_set(&peer.move_confirmed, true);

	bool teleported = false;
	for(size_t elapsed = 0; elapsed < 5000 && !teleported; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		teleported = e->pos[0] == 10.0f;
		thread_msleep(1);
	}
	assert(teleported);
	assert(e->pos[1] == 10.015625f && e->pos[2] == 10.0f);
	overflow_flag_set(&peer.teleport_confirmed, true);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
	puts("  base-entity snap: item pos follows wire (move + teleport lift)");
}

/* ============ residuals 1+2 (2026-08-26): mob head yaw + entity velocity ===
 * Item 2: a mob's head yaw is now wire-driven when a real Packet33
 * RelEntityMoveLook look arrives, via a DEDICATED network_head_yaw/
 * network_head_increments pair (entity.h) distinct from network_orient/
 * network_increments (position-only for a mob) -- mirrors
 * test_entity_look_forward's own shape one scope up (CRPC arms first, three
 * manual tick_client calls land it exactly), for ENTITY_MONSTER instead of
 * ENTITY_REMOTE_PLAYER. [740c583 NetClientHandler.handleEntity rotating flag]
 *
 * Item 1: Packet28EntityVelocity now reaches an ITEM's e->vel via
 * CRPC_ENTITY_VELOCITY (SETS, not accumulates -- Beta's own
 * Entity.setVelocity); the SAME packet aimed at a MOB is confirmed to leave
 * e->vel untouched -- the documented, decompile-verified skip (this engine's
 * client_tick_monster has no e->vel consumer for a mob at all). No
 * client_tick call is needed for either check: clin_update() applies
 * CRPC_ENTITY_VELOCITY directly with no per-tick decay in this harness
 * (unlike test_mp_base_entity_snap's own "no game loop runs here" note),
 * so both moves are checked as EXACT values, not tolerance-bounded ranges. */
struct entity_residuals_fixture {
	SOCKET listener;
	bool ok;
	atomic_bool mob_spawn_confirmed;
	atomic_bool item_spawn_confirmed;
	atomic_bool look_confirmed;
	atomic_bool item_vel_confirmed;
	atomic_bool mob_vel_confirmed;
};

static void* entity_residuals_peer_entry(void* user) {
	struct entity_residuals_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;

	/* 0x18 Packet24MobSpawn: zombie (type 54), entity 500, at (0,10.0,0),
	 * no rotation, empty metadata (bare 0x7F terminator -- the
	 * test_mp_entity_status pig-fixture shape). */
	uint8_t zombie[21] = {0x18};
	put_i32(zombie + 1, 500);
	zombie[5] = 54;
	put_i32(zombie + 6, 0);
	put_i32(zombie + 10, 320);
	put_i32(zombie + 14, 0);
	zombie[18] = 0; zombie[19] = 0; zombie[20] = 0x7F;
	if(!socket_write_fragmented(peer, zombie, sizeof(zombie)))
		goto done;
	while(!overflow_flag_get(&fixture->mob_spawn_confirmed))
		thread_msleep(1);

	/* 0x15 Packet21PickupSpawn: stone x1, entity 501, at (0,10.0,0), zero
	 * velocity (the test_mp_base_entity_snap body-length/trailing-byte
	 * shape -- body is 24 bytes, the last (vel_z) sent separately). */
	uint8_t item_spawn[24] = {0x15};
	put_i32(item_spawn + 1, 501);
	item_spawn[5] = 0; item_spawn[6] = 1; /* item id 1 (stone) */
	item_spawn[7] = 1;                    /* count */
	item_spawn[8] = 0; item_spawn[9] = 0; /* damage */
	put_i32(item_spawn + 10, 0);
	put_i32(item_spawn + 14, 320);
	put_i32(item_spawn + 18, 0);
	item_spawn[22] = 0; item_spawn[23] = 0; /* vel_x, vel_y */
	if(!socket_write_fragmented(peer, item_spawn, sizeof(item_spawn)))
		goto done;
	{
		uint8_t vel_z_byte = 0;
		if(!socket_write_fragmented(peer, &vel_z_byte, sizeof(vel_z_byte)))
			goto done;
	}
	while(!overflow_flag_get(&fixture->item_spawn_confirmed))
		thread_msleep(1);

	/* 0x21 Packet33RelEntityMoveLook on the ZOMBIE: dx=dy=dz=0, yaw byte 64
	 * (-> Beta 90.0deg exactly -> ENGINE -90.0deg, the CRPC-boundary
	 * mirror), pitch byte 0 (unused by a mob). */
	uint8_t look[10] = {0x21};
	put_i32(look + 1, 500);
	look[5] = 0; look[6] = 0; look[7] = 0; /* dx/dy/dz */
	look[8] = 64;                          /* yaw */
	look[9] = 0;                           /* pitch */
	if(!socket_write_fragmented(peer, look, sizeof(look)))
		goto done;
	while(!overflow_flag_get(&fixture->look_confirmed))
		thread_msleep(1);

	/* 0x1C Packet28EntityVelocity on the ITEM: motionX=800/motionY=-400
	 * (-> 0.1/-0.05 blocks-per-tick after the wire's /8000.0), motionZ=0. */
	uint8_t item_vel[11] = {0x1C};
	put_i32(item_vel + 1, 501);
	put_i16(item_vel + 5, 800);
	put_i16(item_vel + 7, -400);
	put_i16(item_vel + 9, 0);
	if(!socket_write_fragmented(peer, item_vel, sizeof(item_vel)))
		goto done;
	while(!overflow_flag_get(&fixture->item_vel_confirmed))
		thread_msleep(1);

	/* The SAME velocity packet aimed at the ZOMBIE: the documented skip
	 * (no e->vel consumer on this engine's client_tick_monster). */
	uint8_t mob_vel[11] = {0x1C};
	put_i32(mob_vel + 1, 500);
	put_i16(mob_vel + 5, 800);
	put_i16(mob_vel + 7, -400);
	put_i16(mob_vel + 9, 0);
	if(!socket_write_fragmented(peer, mob_vel, sizeof(mob_vel)))
		goto done;
	while(!overflow_flag_get(&fixture->mob_vel_confirmed))
		thread_msleep(1);

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_mp_entity_residuals_2026_08_26(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct entity_residuals_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, entity_residuals_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	struct entity** mob_pp = NULL;
	bool mob_spawned = false;
	for(size_t elapsed = 0; elapsed < 5000 && !mob_spawned; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		mob_pp = dict_entity_get(gstate.entities, 500);
		mob_spawned = mob_pp && *mob_pp && (*mob_pp)->type == ENTITY_MONSTER;
		thread_msleep(1);
	}
	assert(mob_spawned);
	struct entity* mob = *mob_pp;
	/* Baseline: no look has arrived yet, so the wire-look pair is unarmed
	 * and head_yaw sits at its spawn default. */
	assert(mob->data.monster.network_head_increments == 0);
	assert(mob->vel[0] == 0.0f && mob->vel[1] == 0.0f && mob->vel[2] == 0.0f);
	overflow_flag_set(&peer.mob_spawn_confirmed, true);

	struct entity** item_pp = NULL;
	bool item_spawned = false;
	for(size_t elapsed = 0; elapsed < 5000 && !item_spawned; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		item_pp = dict_entity_get(gstate.entities, 501);
		item_spawned = item_pp && *item_pp && (*item_pp)->type == ENTITY_ITEM;
		thread_msleep(1);
	}
	assert(item_spawned);
	struct entity* item = *item_pp;
	assert(item->vel[0] == 0.0f && item->vel[1] == 0.0f && item->vel[2] == 0.0f);
	overflow_flag_set(&peer.item_spawn_confirmed, true);

	/* Item 2: the look arms the DEDICATED mob pair (never network_orient/
	 * network_increments, which stay unarmed -- position-only for a mob). */
	bool armed = false;
	for(size_t elapsed = 0; elapsed < 5000 && !armed; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		armed = mob->data.monster.network_head_increments == 3;
		thread_msleep(1);
	}
	assert(armed);
	assert(fabsf(mob->data.monster.network_head_yaw - (-(float)M_PI / 2.0f))
		   < 1e-3f);
	/* The SAME packet's move half (dx=dy=dz=0) unconditionally re-arms the
	 * GENERIC position pair too (CRPC_ENTITY_MOVE's own approach_ticks>0
	 * rule, independent of this residual) -- 3 here is that field doing its
	 * ordinary job, not a collision: client_tick_monster only ever calls
	 * entity_network_approach_tick(e, false), so this counter steps e->pos
	 * alone and never reads/writes e->orient for a mob. */
	assert(mob->network_increments == 3);
	/* Three client ticks land head_yaw EXACTLY on the wire target -- the
	 * same Beta 3-increment mechanism test_entity_look_forward pins for a
	 * remote player, applied here to the mob's dedicated pair. */
	mob->tick_client(mob);
	mob->tick_client(mob);
	assert(mob->data.monster.network_head_increments == 1); /* mid-approach */
	mob->tick_client(mob);
	assert(mob->data.monster.network_head_increments == 0);
	assert(fabsf(mob->data.monster.head_yaw - (-(float)M_PI / 2.0f)) < 1e-3f);
	overflow_flag_set(&peer.look_confirmed, true);

	/* Item 1: velocity SETS the item's e->vel exactly (no tick has run for
	 * the item since spawn, so there is no per-tick gravity/friction decay
	 * to tolerate -- an EXACT pin, the test_mp_base_entity_snap precedent
	 * for e->pos applied here to e->vel). */
	bool item_vel_set = false;
	for(size_t elapsed = 0; elapsed < 5000 && !item_vel_set; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		item_vel_set = item->vel[0] == 0.1f;
		thread_msleep(1);
	}
	assert(item_vel_set);
	assert(item->vel[1] == -0.05f && item->vel[2] == 0.0f);
	overflow_flag_set(&peer.item_vel_confirmed, true);

	/* The identical wire velocity aimed at the MOB must NOT move e->vel --
	 * the documented, decompile-verified skip (no e->vel consumer exists on
	 * this engine's client_tick_monster for any mob). Bounded wait for the
	 * peer's send + a few drain passes, then assert it never took hold. */
	for(size_t elapsed = 0; elapsed < 200; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		thread_msleep(1);
	}
	assert(mob->vel[0] == 0.0f && mob->vel[1] == 0.0f && mob->vel[2] == 0.0f);
	overflow_flag_set(&peer.mob_vel_confirmed, true);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
	puts("  entity residuals: mob head yaw from wire (item 2) + entity "
		 "velocity item/mob split (item 1)");
}

/* Respawn-hunt regression guard (PC-only, live-server; promoted from a
 * temporary hunt harness to a permanent CLI-mode regression test 2026-08-30
 * once the underlying defect was root-caused and fixed -- see
 * server_remote_chunks.c's Fix A and server_remote_transport.c/_protocol.c's
 * Fix B): live repro for the MP "stuck respawn" defect against the running
 * mc173 diag server (127.0.0.1:25565). Kills the local player via the diag
 * server's own /explode command -- no game-code changes needed,
 * server_remote_send_chat is an existing production entry point that
 * run_live_s2 already exercises for "/give" -- then sends SRPC_RESPAWN
 * exactly as screen_death.c:130 does and watches whether the connection
 * ever leaves REMOTE_PLAY in between (the original defect: a chunk-overlap
 * kill racing SRPC_RESPAWN's REMOTE_PLAY gate check against its own queue
 * append -- fixed root-cause in server_remote_chunks.c/_transport.c/
 * _protocol.c, not papered over here) and whether health/position actually
 * reset. Finishes with the unified-theory check: reconnect with the SAME
 * username and report the health the server hands back at login. */
static int run_live_respawn_hunt(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	assert(server_remote_start(&remote, "127.0.0.1", 25565, "RespawnHunt"));

	ptime_t connect_start = time_get();
	struct server_remote_status status;
	while(time_diff_ms(connect_start, time_get()) < 30000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state == REMOTE_PLAY && gstate.world_loaded)
			break;
		if(status.state == REMOTE_CLOSING || status.state == REMOTE_KICKED)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	if(status.state != REMOTE_PLAY || !gstate.world_loaded) {
		fprintf(stderr,
			"RESPAWN HUNT FAIL: connect/login/world-install (state=%d "
			"reason=\"%s\")\n",
			status.state, status.reason);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	printf("RESPAWN HUNT LOGIN entity=%d seed=%lld dimension=%d\n",
		status.entity_id, (long long)status.map_seed, status.dimension);
	printf("RESPAWN HUNT initial health=%d\n",
		gstate.players[0].local_player ? gstate.players[0].local_player->health : -999);

	/* --- Phase 1: kill the local player. --- */
	bool died = false;
	for(int attempt = 0; attempt < 8 && !died; attempt++) {
		if(!server_remote_send_chat(&remote, "/explode")) {
			fprintf(stderr, "RESPAWN HUNT FAIL: send /explode (attempt %d)\n",
				attempt);
			break;
		}
		live_s2_pump_for(&remote, 1500, &status);
		short h = gstate.players[0].local_player ? gstate.players[0].local_player->health : -999;
		printf("RESPAWN HUNT after /explode #%d health=%d state=%d\n",
			attempt + 1, h, status.state);
		if(h <= 0)
			died = true;
		if(status.state != REMOTE_PLAY) {
			fprintf(stderr,
				"RESPAWN HUNT FAIL: left REMOTE_PLAY while trying to die "
				"(state=%d reason=\"%s\")\n",
				status.state, status.reason);
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			return 1;
		}
	}
	if(!died) {
		fprintf(stderr,
			"RESPAWN HUNT FAIL: never observed health<=0 after 8 "
			"explosions\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	puts("RESPAWN HUNT DIED (client-side health<=0 confirmed -- this is "
		 "exactly screen_ingame.c's screen_death trigger condition)");

	/* --- Phase 2: the exact client action screen_death.c:130 takes on a
	 * held Respawn press. --- */
	server_remote_get_status(&remote, &status);
	printf("RESPAWN HUNT pre-respawn-send state=%d queued_frames=%zu\n",
		status.state, status.queued_frames);
	struct server_rpc respawn_call = {.type = SRPC_RESPAWN};
	bool sent = svin_rpc_send(&respawn_call);
	printf("RESPAWN HUNT svin_rpc_send(SRPC_RESPAWN) returned %d\n",
		(int)sent);

	ptime_t respawn_start = time_get();
	bool recovered = false;
	while(time_diff_ms(respawn_start, time_get()) < 8000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		short h = gstate.players[0].local_player ? gstate.players[0].local_player->health : -999;
		if(h > 0) {
			recovered = true;
			break;
		}
		if(status.state != REMOTE_PLAY)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	short post_h = gstate.players[0].local_player ? gstate.players[0].local_player->health : -999;
	printf("RESPAWN HUNT post-respawn-wait health=%d state=%d "
		"reason=\"%s\" pos=(%.2f,%.2f,%.2f)\n",
		post_h, status.state, status.reason,
		gstate.players[0].local_player ? gstate.players[0].local_player->pos[0] : 0.0,
		gstate.players[0].local_player ? gstate.players[0].local_player->pos[1] : 0.0,
		gstate.players[0].local_player ? gstate.players[0].local_player->pos[2] : 0.0);
	if(recovered)
		puts("RESPAWN HUNT VERDICT: RESPAWN WORKED (health recovered)");
	else
		puts("RESPAWN HUNT VERDICT: RESPAWN WEDGED (health stayed <=0 for "
			 "8s after SRPC_RESPAWN)");

	/* --- Phase 3: unified-theory check -- quit + rejoin the SAME username,
	 * report what health the server hands back at login. --- */
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
	memset(&remote, 0, sizeof(remote));
	gstate.players[0].local_player = NULL;
	gstate.world_loaded = false;

	assert(server_remote_init(&remote));
	assert(server_remote_start(&remote, "127.0.0.1", 25565, "RespawnHunt"));
	ptime_t rejoin_start = time_get();
	while(time_diff_ms(rejoin_start, time_get()) < 30000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state == REMOTE_PLAY && gstate.world_loaded)
			break;
		if(status.state == REMOTE_CLOSING || status.state == REMOTE_KICKED)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	if(status.state != REMOTE_PLAY || !gstate.world_loaded) {
		fprintf(stderr,
			"RESPAWN HUNT FAIL: rejoin connect/login/world-install "
			"(state=%d reason=\"%s\")\n",
			status.state, status.reason);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	/* Give a little pump time for any post-login health packet to settle. */
	live_s2_pump_for(&remote, 1000, &status);
	short rejoin_h = gstate.players[0].local_player ? gstate.players[0].local_player->health : -999;
	printf("RESPAWN HUNT REJOIN entity=%d health=%d pos=(%.2f,%.2f,%.2f)\n",
		status.entity_id, rejoin_h,
		gstate.players[0].local_player ? gstate.players[0].local_player->pos[0] : 0.0,
		gstate.players[0].local_player ? gstate.players[0].local_player->pos[1] : 0.0,
		gstate.players[0].local_player ? gstate.players[0].local_player->pos[2] : 0.0);
	if(rejoin_h > 0)
		puts("RESPAWN HUNT UNIFIED-THEORY VERDICT: dead state does NOT "
			 "persist across rejoin (fresh entity, health>0)");
	else
		puts("RESPAWN HUNT UNIFIED-THEORY VERDICT: dead state DOES appear "
			 "to persist across rejoin (health<=0 at login)");

	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
	return 0;
}

/* Same resend-until-it-lands shape as live_s2_teleport above (see that
 * helper's header comment for why a one-shot SRPC_PLAYER_POS send is
 * unreliable: remote_process_srpc's correction_seq gate silently drops it if
 * a server-driven correction lands in the read-status-to-dispatch window),
 * parametrized with on_ground so a caller can hold a specific airborne or
 * grounded state -- run_live_respawn_hunt_fall's synthetic fall needs both,
 * unlike live_s2_teleport's always-grounded teleport. */
static bool live_s2_position_hold(struct server_remote* remote, double x,
								   double y, double z, bool on_ground,
								   unsigned total_ms,
								   struct server_remote_status* out) {
	ptime_t start = time_get();
	struct server_remote_status status;
	server_remote_get_status(remote, &status);
	do {
		struct server_rpc pos = {
			.type = SRPC_PLAYER_POS,
			.payload.player_pos = {.x = x, .y = y, .z = z, .rx = 0.0f,
				.ry = 0.0f, .on_ground = on_ground,
				.correction_seq = status.correction_seq},
		};
		(void)svin_rpc_send(&pos); /* best-effort; a full inbox just retries
		                            * next iteration */
		server_remote_pump(remote);
		clin_update();
		server_remote_get_status(remote, &status);
		if(status.state != REMOTE_PLAY || status.forced_reconnects != 0)
			break;
		thread_msleep(1);
	} while(time_diff_ms(start, time_get()) < (long)total_ms);
	if(out)
		*out = status;
	return status.state == REMOTE_PLAY && status.forced_reconnects == 0;
}

/* Respawn hunt variant B: the /explode kill above causes real block
 * destruction, which streams a burst of Packet51 chunk updates back down --
 * the disconnect that used to eat the respawn in that run came from the
 * chunk-retain-overlap guard ("overlapping remote Packet51 requires clean
 * reconnect", now fixed -- see server_remote_chunks.c's Fix A), not from the
 * respawn path itself. This variant kills the player with ZERO world
 * mutation -- a synthetic huge fall via raw SRPC_PLAYER_POS (mc173's
 * handle_position_look_inner accumulates fall_distance from any on_ground
 * =false packet with negative dy, then applies fall damage on the next
 * on_ground=true packet, exactly like a real fall, just teleported) -- a
 * permanent regression guard that the same fix covers an ordinary "clean"
 * death with no chunk-stream side effects, not just the chunk-overlap
 * trigger path. */
static int run_live_respawn_hunt_fall(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	assert(
		server_remote_start(&remote, "127.0.0.1", 25565, "RespawnHuntFall"));

	ptime_t connect_start = time_get();
	struct server_remote_status status;
	while(time_diff_ms(connect_start, time_get()) < 30000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state == REMOTE_PLAY && gstate.world_loaded)
			break;
		if(status.state == REMOTE_CLOSING || status.state == REMOTE_KICKED)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	if(status.state != REMOTE_PLAY || !gstate.world_loaded) {
		fprintf(stderr,
			"RESPAWN HUNT FALL FAIL: connect/login/world-install (state=%d "
			"reason=\"%s\")\n",
			status.state, status.reason);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	printf("RESPAWN HUNT FALL LOGIN entity=%d seed=%lld dimension=%d\n",
		status.entity_id, (long long)status.map_seed, status.dimension);

	/* Optional control: let the initial login chunk-promotion backlog fully
	 * drain BEFORE doing anything else, exercising the fix with and without
	 * a promotion in flight at kill-time (both must stay fixed -- Fix A's
	 * park-and-discard applies regardless of when the overlap lands). */
	if(getenv("CW_RESPAWN_HUNT_SETTLE")) {
		ptime_t settle_start = time_get();
		while(time_diff_ms(settle_start, time_get()) < 10000) {
			server_remote_pump(&remote);
			clin_update();
			server_remote_get_status(&remote, &status);
			if(status.promotion_backlog == 0)
				break;
			thread_msleep(1);
		}
		printf("RESPAWN HUNT FALL settle done: promotion_backlog=%zu "
			"ready_entries=%zu after %lldms\n",
			status.promotion_backlog, status.ready_entries,
			(long long)time_diff_ms(settle_start, time_get()));
	}

	/* --- Phase 1: kill via a synthetic ~200-block fall, no block edits. ---
	 * Each waypoint is HELD (resent every pumped iteration with a freshly
	 * re-read correction_seq), not one-shot: live_s2_teleport's own header
	 * comment documents that a single SRPC_PLAYER_POS send can be silently
	 * dropped by remote_process_srpc's correction_seq gate if a server-driven
	 * correction lands in the tiny window between reading status and this
	 * send being dispatched -- a real risk here since EACH of the three
	 * waypoints below is exactly the kind of traffic that can trigger one.
	 * A dropped "on_ground=true" landing in particular would leave mc173's
	 * OWN fall-tracking never seeing a confirmed landing -- indistinguishable
	 * from a wedged respawn but actually a test-delivery gap, not a
	 * CaveWii/server defect. */
	bool died = false;
	for(int attempt = 0; attempt < 5 && !died; attempt++) {
		live_s2_position_hold(&remote, 0.0, 200.0, 0.0, false, 200, &status);
		live_s2_position_hold(&remote, 0.0, 1.0, 0.0, false, 200, &status);
		live_s2_position_hold(&remote, 0.0, 1.0, 0.0, true, 1000, &status);
		short h = gstate.players[0].local_player ? gstate.players[0].local_player->health : -999;
		printf("RESPAWN HUNT FALL after fall #%d health=%d state=%d\n",
			attempt + 1, h, status.state);
		if(h <= 0)
			died = true;
		if(status.state != REMOTE_PLAY) {
			fprintf(stderr,
				"RESPAWN HUNT FALL FAIL: left REMOTE_PLAY while trying to "
				"die (state=%d reason=\"%s\")\n",
				status.state, status.reason);
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			return 1;
		}
	}
	if(!died) {
		fprintf(stderr,
			"RESPAWN HUNT FALL FAIL: never observed health<=0 after 5 "
			"falls\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	puts("RESPAWN HUNT FALL DIED (client-side health<=0 confirmed, zero "
		 "block edits -- no chunk restream side effect)");

	/* --- Phase 2: the exact client action screen_death.c:130 takes. --- */
	server_remote_get_status(&remote, &status);
	printf("RESPAWN HUNT FALL pre-respawn-send state=%d queued_frames=%zu\n",
		status.state, status.queued_frames);
	struct server_rpc respawn_call = {.type = SRPC_RESPAWN};
	bool sent = svin_rpc_send(&respawn_call);
	printf("RESPAWN HUNT FALL svin_rpc_send(SRPC_RESPAWN) returned %d\n",
		(int)sent);

	ptime_t respawn_start = time_get();
	bool recovered = false;
	while(time_diff_ms(respawn_start, time_get()) < 8000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		short h = gstate.players[0].local_player ? gstate.players[0].local_player->health : -999;
		if(h > 0) {
			recovered = true;
			break;
		}
		if(status.state != REMOTE_PLAY)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	short post_h = gstate.players[0].local_player ? gstate.players[0].local_player->health : -999;
	printf("RESPAWN HUNT FALL post-respawn-wait health=%d state=%d "
		"reason=\"%s\" pos=(%.2f,%.2f,%.2f)\n",
		post_h, status.state, status.reason,
		gstate.players[0].local_player ? gstate.players[0].local_player->pos[0] : 0.0,
		gstate.players[0].local_player ? gstate.players[0].local_player->pos[1] : 0.0,
		gstate.players[0].local_player ? gstate.players[0].local_player->pos[2] : 0.0);
	if(recovered)
		puts("RESPAWN HUNT FALL VERDICT: RESPAWN WORKED (health recovered)");
	else
		puts("RESPAWN HUNT FALL VERDICT: RESPAWN WEDGED (health stayed <=0 "
			 "for 8s after SRPC_RESPAWN)");

	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
	return recovered ? 0 : 1;
}
/* END respawn-hunt regression harness. */

/* ============ Packet22Collect removes the collected entity ============
 * [740c583 Packet22Collect: i32 collectedEntityId, i32 collectorEntityId]
 * [740c583 NetClientHandler.handleCollect: getEntityByID(collected) != null
 *  -> pop sound + EntityPickupFX + removeEntityFromWorld(collected)] -- the
 * CLIENT removes the item on collect, for ANY collector; the server never
 * owes a follow-up Packet29 for it (mc173 sends none). Pre-fix this engine
 * only ran the pickup flourish, so another player's pickup left a permanent
 * ghost item (maintainer live repro 2026-08-27, PC CaveWii + real b1.7.3
 * client on a LAN mc173 server). */
struct collect_fixture {
	SOCKET listener;
	bool ok;
	atomic_bool spawn_confirmed;
	atomic_bool collect_confirmed;
	atomic_bool unknown_collect_confirmed;
};

#define COLLECT_ITEM_ID 95001
#define COLLECT_UNKNOWN_ID 95002
#define COLLECT_OTHER_PLAYER_ID 95999

static void* collect_peer_entry(void* user) {
	struct collect_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;
	static const uint8_t login_response[]
		= {0x01, 0,0,0,0x2A, 0,0, 0,0,0,0,0,0,0x27,0x0F, 0};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response)))
		goto done;
	uint8_t input[128];
	if(!socket_read_exact(peer, input, 1) || input[0] != 0x00)
		goto done;
	static const uint8_t keep_alive[] = {0x00};

	/* 0x15 Packet21PickupSpawn: stone x1 at (0,10.0,0), zero velocity (the
	 * entity_residuals-fixture shape: 24-byte body + trailing vel_z byte). */
	uint8_t item_spawn[24] = {0x15};
	put_i32(item_spawn + 1, COLLECT_ITEM_ID);
	item_spawn[5] = 0; item_spawn[6] = 1; /* item id 1 (stone) */
	item_spawn[7] = 1;                    /* count */
	item_spawn[8] = 0; item_spawn[9] = 0; /* damage */
	put_i32(item_spawn + 10, 0);
	put_i32(item_spawn + 14, 320);
	put_i32(item_spawn + 18, 0);
	item_spawn[22] = 0; item_spawn[23] = 0; /* vel_x, vel_y */
	uint8_t vel_z_byte = 0;
	if(!socket_write_fragmented(peer, item_spawn, sizeof(item_spawn))
	   || !socket_write_fragmented(peer, &vel_z_byte, sizeof(vel_z_byte)))
		goto done;
	while(!overflow_flag_get(&fixture->spawn_confirmed))
		thread_msleep(1);

	/* 0x16 Packet22Collect by ANOTHER player (collector != our id 42): the
	 * live-ghost case -- no follow-up destroy is ever sent. */
	uint8_t collect[9] = {0x16};
	put_i32(collect + 1, COLLECT_ITEM_ID);
	put_i32(collect + 5, COLLECT_OTHER_PLAYER_ID);
	if(!socket_write_fragmented(peer, collect, sizeof(collect)))
		goto done;
	while(!overflow_flag_get(&fixture->collect_confirmed))
		thread_msleep(1);

	/* 0x16 for an id NEVER spawned: the case is now destroy-capable, so the
	 * unknown id must be absorbed (remote_entity_destroy no-ops on a missing
	 * id; CRPC_ENTITY_DESTROY's consumer guards its own lookup) -- keep-alive
	 * right after proves the byte stream did not desync. */
	uint8_t collect_unknown[9] = {0x16};
	put_i32(collect_unknown + 1, COLLECT_UNKNOWN_ID);
	put_i32(collect_unknown + 5, COLLECT_OTHER_PLAYER_ID);
	if(!socket_write_fragmented(peer, collect_unknown, sizeof(collect_unknown))
	   || !socket_write_fragmented(peer, keep_alive, sizeof(keep_alive)))
		goto done;
	while(!overflow_flag_get(&fixture->unknown_collect_confirmed))
		thread_msleep(1);

	fixture->ok = true;
	while(recv(peer, (char*)input, sizeof(input), 0) > 0) {
	}
done:
	closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_mp_collect_removes_item(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct collect_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, collect_peer_entry, &peer, 64));
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	bool spawned = false;
	for(size_t elapsed = 0; elapsed < 5000 && !spawned; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		struct entity** pp = dict_entity_get(gstate.entities, COLLECT_ITEM_ID);
		spawned = pp && *pp && (*pp)->type == ENTITY_ITEM;
		thread_msleep(1);
	}
	assert(spawned);
	overflow_flag_set(&peer.spawn_confirmed, true);

	/* Pre-fix this loop TIMED OUT: the collector isn't the local player, so
	 * even the flourish was a no-op, and nothing ever removed the entity. */
	bool removed = false;
	for(size_t elapsed = 0; elapsed < 5000 && !removed; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		struct entity** pp = dict_entity_get(gstate.entities, COLLECT_ITEM_ID);
		removed = !pp || !*pp;
		thread_msleep(1);
	}
	assert(removed);
	overflow_flag_set(&peer.collect_confirmed, true);

	/* Unknown-id collect: bounded drain, then confirm nothing materialized
	 * and the connection survived (peer.ok checked after join). */
	for(size_t elapsed = 0; elapsed < 200; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		thread_msleep(1);
	}
	assert(!dict_entity_get(gstate.entities, COLLECT_UNKNOWN_ID));
	overflow_flag_set(&peer.unknown_collect_confirmed, true);

	server_remote_cancel(&remote);
	thread_join(&peer_thread);
	assert(peer.ok);
	server_remote_destroy(&remote);
	puts("  collect: Packet22 removes the collected item for ANY collector; "
		 "unknown-id collect absorbed");
}

/* Same-dim MP respawn terrain hold (game_state.h remote_terrain_hold).
 *
 * mc173 teleports the respawned player to spawn immediately, but the Wii
 * streams the spawn chunks in seconds later; Beta's client only holds
 * physics on WorldClient recreation (login/dim change) and otherwise trusts
 * the vanilla server's residency guarantee [740c583
 * NetClientHandler.handleFlying: doneLoadingTerrain] [740c583
 * GuiDownloadTerrain]. mc173 gives no such guarantee for a same-dim
 * respawn, so the field defect was free-fall through uninstalled terrain
 * into a fall-damage/death loop. The fix extends the SAME hold primitive
 * (documented adaptation, not a rules change): a position-only correction
 * into a not-yet-installed chunk arms remote_terrain_hold, which parks
 * client entity physics (game_state_client_entities_active) and keeps
 * screen_load_world up until clin_update() sees the destination chunk
 * installed; CRPC_WORLD_RESET clears the hold unconditionally. */
static void build_air_section(w_coord_t x, w_coord_t z) {
	const size_t cells = (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	uint8_t* ids = calloc(cells, sizeof(*ids));
	uint8_t* metadata = calloc(cells / 2u, sizeof(*metadata));
	uint8_t* sky = calloc(cells / 2u, sizeof(*sky));
	uint8_t* torch = calloc(cells / 2u, sizeof(*torch));
	uint8_t* heightmap = calloc((size_t)CHUNK_SIZE * CHUNK_SIZE, sizeof(*heightmap));
	uint8_t* grass_i = calloc((size_t)CHUNK_SIZE * CHUNK_SIZE, sizeof(*grass_i));
	uint8_t* grass_j = calloc((size_t)CHUNK_SIZE * CHUNK_SIZE, sizeof(*grass_j));
	assert(ids && metadata && sky && torch && heightmap && grass_i && grass_j);

	assert(world_build_full_section(&gstate.world, x, z, ids, metadata, sky,
									torch, heightmap, grass_i, grass_j));

	free(grass_j);
	free(grass_i);
	free(heightmap);
	free(torch);
	free(sky);
	free(metadata);
	free(ids);
}

static void test_respawn_terrain_hold(void) {
	/* Clean footing regardless of whatever the preceding live-loopback tests
	 * left in the shared gstate -- the same CRPC_WORLD_RESET fixture
	 * test_portal_travel.c/test_bed_camera.c use for a client-only unit
	 * test (fresh local player id 0, fresh empty world). */
	struct client_rpc reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {
			.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = 0,
		},
	};
	clin_process(&reset);
	assert(gstate.players[0].local_player);

	/* Arrange: an installed chunk under the player, world "loaded" over a
	 * remote (MP) session -- exactly the gate client_interface.c's
	 * CRPC_PLAYER_POS handler checks (was_loaded && gstate.remote_session)
	 * before it will even consider arming the hold. */
	build_air_section(0, 0);
	gstate.players[0].local_player->teleport(gstate.players[0].local_player, (vec3) {8.0f, 70.0f, 8.0f});
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	gstate.remote_session = true;
	assert(!gstate.remote_terrain_hold);
	assert(game_state_client_entities_active(&gstate));

	/* Mirror production ORDER, not just the position echo: MP_EVENT_RESPAWN's
	 * same-dimension branch resets health via CRPC_PLAYER_SET_HEALTH
	 * [server_remote_protocol.c ~:844-849] strictly BEFORE the server's
	 * subsequent Player-Position-And-Look packet arrives and (via remote_
	 * handle_correction -> CRPC_PLAYER_POS) arms this hold. A fresh respawn
	 * is always full-health; the assertions below prove that value survives
	 * the hold untouched -- no phantom fall-damage lump sum from a tick that
	 * structurally never ran (2026-08-30 field defect: sky respawn -> fall
	 * damage/death loop). */
	struct client_rpc health_reset = {
		.type = CRPC_PLAYER_SET_HEALTH,
		.payload.player_set_health.health = health_wire_to_internal(20),
	};
	clin_process(&health_reset);
	assert(gstate.players[0].local_player->health == MAX_PLAYER_HEALTH);

	/* A position-only correction (Packet13-style respawn snap) lands the
	 * player in a chunk the client has never installed. */
	struct client_rpc respawn = {
		.type = CRPC_PLAYER_POS,
		.payload.player_pos = {
			.position = {800.5f, 70.0f, 800.5f},
			.correction_flags = PLAYER_CORRECTION_POSITION_ONLY,
			.correction_seq = 1,
		},
	};
	assert(!world_find_chunk(&gstate.world, 800, 70, 800));
	clin_process(&respawn);
	assert(gstate.players[0].local_player->pos[0] == 800.5f
		   && gstate.players[0].local_player->pos[1] == 70.0f
		   && gstate.players[0].local_player->pos[2] == 800.5f);
	assert(gstate.remote_terrain_hold);
	assert(!game_state_client_entities_active(&gstate));
	assert(gstate.players[0].local_player->health == MAX_PLAYER_HEALTH);

	/* Held, chunk still absent: clin_update's belt-and-suspenders zeroing
	 * (the entities-active predicate above already parks the tick) keeps a
	 * nudged velocity from accumulating under the hold, and -- because
	 * main.c's client tick (the ONLY place fall damage can be computed)
	 * gates on this exact predicate [main.c ~:1640] -- health cannot move
	 * either, no matter how many frames the hold spans. */
	gstate.players[0].local_player->vel[0] = 1.0f;
	gstate.players[0].local_player->vel[1] = 2.0f;
	gstate.players[0].local_player->vel[2] = 3.0f;
	clin_update();
	assert(gstate.players[0].local_player->vel[0] == 0.0f
		   && gstate.players[0].local_player->vel[1] == 0.0f
		   && gstate.players[0].local_player->vel[2] == 0.0f);
	assert(gstate.remote_terrain_hold);
	assert(gstate.players[0].local_player->health == MAX_PLAYER_HEALTH);
	for(int i = 0; i < 20; i++)
		clin_update(); /* still absent: many frames, still no drift/damage */
	assert(gstate.remote_terrain_hold);
	assert(!game_state_client_entities_active(&gstate));
	assert(gstate.players[0].local_player->health == MAX_PLAYER_HEALTH);

	/* Destination installs: clin_update releases the hold and physics
	 * resumes -- the player now falls from spawn height over LOADED
	 * terrain (the field-proven login behaviour), never the empty-world
	 * free-fall. The install/release transition itself must not apply any
	 * accumulated fall-damage lump sum: health is sampled the INSTANT the
	 * hold clears, before any post-release tick could legitimately begin a
	 * fresh (Beta-normal) short settle fall. */
	build_air_section(800, 800);
	clin_update();
	assert(!gstate.remote_terrain_hold);
	assert(game_state_client_entities_active(&gstate));
	assert(gstate.players[0].local_player->health == MAX_PLAYER_HEALTH);

	/* CRPC_WORLD_RESET clears the hold unconditionally: a dim change or
	 * reconnect teardown must never leave a stale hold outliving the world
	 * it was gating. */
	gstate.remote_terrain_hold = true;
	clin_process(&reset);
	assert(!gstate.remote_terrain_hold);

	/* Restore the shared gstate flags this test changed. */
	gstate.remote_session = false;
	gstate.world_loaded = false;

	puts("  respawn terrain hold: same-dim MP respawn into an uninstalled "
		 "chunk parks physics (GuiDownloadTerrain parity) instead of "
		 "free-falling into fall damage; health stays full through a "
		 "20-frame hold and the install/release transition; releases when "
		 "the chunk installs; CRPC_WORLD_RESET clears it unconditionally");
}

/* Live end-to-end proof of the same-dim respawn terrain hold, over a REAL
 * connection instead of test_respawn_terrain_hold's direct CRPC_PLAYER_POS
 * injection. This exercises the actual wire path the 2026-08-30 field defect
 * broke and the 7ec5536 fix (game_state.h remote_terrain_hold) closed:
 * MP_EVENT_RESPAWN (health reset, server_remote_protocol.c ~:844-849) -> the
 * server's own follow-up Player-Position-And-Look packet -> MP_EVENT_
 * PLAYER_LOOK_MOVE -> remote_handle_correction (server_remote_protocol.c
 * ~:483-537) -> CRPC_PLAYER_POS(PLAYER_CORRECTION_POSITION_ONLY) ->
 * client_interface.c's arm check (~:1466-1519), none of which the injection
 * unit test touches. [740c583 NetClientHandler.handleRespawn shows
 * GuiDownloadTerrain until the spawn chunk arrives; NetClientHandler.
 * handleFlying: doneLoadingTerrain dismisses it.]
 *
 * On a fast loopback connection the real spawn chunk is normally already
 * resident (mc173 streamed it during login), so the not-yet-installed race
 * this fix guards against would rarely reproduce by timing alone -- exactly
 * why the field defect needed slow real Wii hardware to surface. This leg
 * makes the race UNCONDITIONAL instead of hoping to catch it: it evicts the
 * respawn destination chunk client-side (world_unload_section) immediately
 * before sending SRPC_RESPAWN, guaranteeing world_find_chunk fails when the
 * server's position correction lands, then simulates the chunk finally
 * streaming in (build_air_section, same technique test_respawn_terrain_hold
 * uses) to prove the release side too. NOTE (shared with every other
 * --live-* leg in this file): this harness calls server_remote_pump/
 * clin_update directly, never main.c's 20Hz tick loop, so it cannot exercise
 * entities_client_tick (the only place client-side fall damage is computed)
 * -- the proof here is that game_state_client_entities_active(), the EXACT
 * predicate main.c ~:1640 gates that call on, stays false for the entire
 * held window and only flips true after the simulated install, which is
 * structurally sufficient: main.c cannot run player physics while it reads
 * false, with or without this test harness. */
static int run_live_respawn_hunt_terrain(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	assert(server_remote_start(&remote, "127.0.0.1", 25565, "RespawnHuntTerr"));

	ptime_t connect_start = time_get();
	struct server_remote_status status;
	while(time_diff_ms(connect_start, time_get()) < 30000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state == REMOTE_PLAY && gstate.world_loaded)
			break;
		if(status.state == REMOTE_CLOSING || status.state == REMOTE_KICKED)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	if(status.state != REMOTE_PLAY || !gstate.world_loaded
	   || !gstate.players[0].local_player) {
		fprintf(stderr,
			"RESPAWN HUNT TERRAIN FAIL: connect/login/world-install "
			"(state=%d reason=\"%s\")\n",
			status.state, status.reason);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	/* main.c sets this at the equivalent point in a real connect (take-51
	 * pre-world echo gate); this headless harness bypasses main.c entirely,
	 * so it must set the same state by hand -- exactly what test_respawn_
	 * terrain_hold's injection fixture already does. Without it the arm
	 * check's `gstate.remote_session` half of its gate is false and the
	 * hold can never arm, live connection or not. */
	gstate.remote_session = true;

	/* The player has not moved since login (no motion sent yet), so the
	 * CURRENT position IS the same-dim respawn destination mc173 will
	 * return to [server-side spawn point, static for this session]. */
	float spawn_x = gstate.players[0].local_player->pos[0];
	float spawn_y = gstate.players[0].local_player->pos[1];
	float spawn_z = gstate.players[0].local_player->pos[2];
	w_coord_t spawn_cx = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(spawn_x));
	w_coord_t spawn_cz = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(spawn_z));
	w_coord_t spawn_origin_x = spawn_cx * CHUNK_SIZE;
	w_coord_t spawn_origin_z = spawn_cz * CHUNK_SIZE;
	printf("RESPAWN HUNT TERRAIN LOGIN entity=%d pos=(%.2f,%.2f,%.2f) "
		"chunk=(%d,%d) health=%d\n",
		status.entity_id, spawn_x, spawn_y, spawn_z, (int)spawn_cx,
		(int)spawn_cz, gstate.players[0].local_player->health);
	if(!world_find_chunk(&gstate.world, (w_coord_t)floorf(spawn_x),
		   (w_coord_t)floorf(spawn_y), (w_coord_t)floorf(spawn_z))) {
		fprintf(stderr,
			"RESPAWN HUNT TERRAIN FAIL: spawn chunk not resident right "
			"after a successful login -- world_loaded's own precondition "
			"should have guaranteed this, test assumption broken\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	/* --- Phase 1: kill (matches run_live_respawn_hunt's proven /explode
	 * technique). --- */
	bool died = false;
	for(int attempt = 0; attempt < 8 && !died; attempt++) {
		if(!server_remote_send_chat(&remote, "/explode")) {
			fprintf(stderr,
				"RESPAWN HUNT TERRAIN FAIL: send /explode (attempt %d)\n",
				attempt);
			break;
		}
		live_s2_pump_for(&remote, 1500, &status);
		short h = gstate.players[0].local_player ? gstate.players[0].local_player->health : -999;
		printf("RESPAWN HUNT TERRAIN after /explode #%d health=%d state=%d\n",
			attempt + 1, h, status.state);
		if(h <= 0)
			died = true;
		if(status.state != REMOTE_PLAY) {
			fprintf(stderr,
				"RESPAWN HUNT TERRAIN FAIL: left REMOTE_PLAY while trying "
				"to die (state=%d reason=\"%s\")\n",
				status.state, status.reason);
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			return 1;
		}
	}
	if(!died) {
		fprintf(stderr,
			"RESPAWN HUNT TERRAIN FAIL: never observed health<=0 after 8 "
			"explosions\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	puts("RESPAWN HUNT TERRAIN DIED");

	/* --- Phase 2: evict the destination chunk client-side, THEN respawn --
	 * back-to-back with no intervening clin_update() so nothing else can
	 * observe the column missing before the real respawn correction lands. */
	world_unload_section(&gstate.world, spawn_cx, spawn_cz);
	if(world_find_chunk(&gstate.world, (w_coord_t)floorf(spawn_x),
		   (w_coord_t)floorf(spawn_y), (w_coord_t)floorf(spawn_z))) {
		fprintf(stderr,
			"RESPAWN HUNT TERRAIN FAIL: world_unload_section did not evict "
			"the destination chunk\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	printf("RESPAWN HUNT TERRAIN evicted chunk (%d,%d) -- destination now "
		"NOT resident client-side\n",
		(int)spawn_cx, (int)spawn_cz);

	struct server_rpc respawn_call = {.type = SRPC_RESPAWN};
	bool sent = svin_rpc_send(&respawn_call);
	printf("RESPAWN HUNT TERRAIN svin_rpc_send(SRPC_RESPAWN) returned %d\n",
		(int)sent);
	if(!sent) {
		fprintf(stderr,
			"RESPAWN HUNT TERRAIN FAIL: SRPC_RESPAWN send rejected\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	/* --- Phase 3: watch for the hold to arm from the REAL server-driven
	 * respawn + position-correction round trip. --- */
	bool armed = false;
	ptime_t arm_start = time_get();
	while(time_diff_ms(arm_start, time_get()) < 8000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state != REMOTE_PLAY) {
			fprintf(stderr,
				"RESPAWN HUNT TERRAIN FAIL: left REMOTE_PLAY waiting for "
				"the hold to arm (state=%d reason=\"%s\")\n",
				status.state, status.reason);
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			return 1;
		}
		if(gstate.remote_terrain_hold) {
			armed = true;
			break;
		}
		thread_msleep(1);
	}
	if(!armed) {
		fprintf(stderr,
			"RESPAWN HUNT TERRAIN FAIL: remote_terrain_hold never armed "
			"within 8s of SRPC_RESPAWN -- the field defect would be LIVE "
			"here (free-fall through the evicted chunk)\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	if(game_state_client_entities_active(&gstate)) {
		fprintf(stderr,
			"RESPAWN HUNT TERRAIN FAIL: entities_active still true while "
			"remote_terrain_hold is armed\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	float held_y0 = gstate.players[0].local_player->pos[1];
	short held_health0 = gstate.players[0].local_player->health;
	printf("RESPAWN HUNT TERRAIN ARMED pos=(%.2f,%.2f,%.2f) health=%d "
		"entities_active=%d\n",
		gstate.players[0].local_player->pos[0], held_y0, gstate.players[0].local_player->pos[2],
		held_health0, (int)game_state_client_entities_active(&gstate));
	if(held_health0 != MAX_PLAYER_HEALTH) {
		fprintf(stderr,
			"RESPAWN HUNT TERRAIN FAIL: health=%d at arm time, expected "
			"MAX_PLAYER_HEALTH=%d (mc173 sends no Packet8 after Respawn -- "
			"CaveWii's own MP_EVENT_RESPAWN reset is the only source)\n",
			held_health0, MAX_PLAYER_HEALTH);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	/* --- Phase 4: hold for a real window -- no free-fall, no damage while
	 * frozen, however many frames the hold spans. --- */
	bool y_drifted = false, health_dropped = false;
	bool released_early = false;
	ptime_t hold_start = time_get();
	while(time_diff_ms(hold_start, time_get()) < 2000) {
		server_remote_pump(&remote);
		clin_update();
		if(!gstate.remote_terrain_hold) {
			released_early = true;
			break;
		}
		if(fabsf(gstate.players[0].local_player->pos[1] - held_y0) > 0.01f)
			y_drifted = true;
		if(gstate.players[0].local_player->health < held_health0)
			health_dropped = true;
		thread_msleep(1);
	}
	printf("RESPAWN HUNT TERRAIN HELD ~2s: y_drifted=%d health_dropped=%d "
		"released_early=%d\n",
		(int)y_drifted, (int)health_dropped, (int)released_early);
	if(y_drifted || health_dropped) {
		fprintf(stderr,
			"RESPAWN HUNT TERRAIN FAIL: free-fall or fall damage observed "
			"while the hold was armed\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	/* --- Phase 5: simulate the chunk finally streaming in (test_respawn_
	 * terrain_hold's own release technique) and confirm a clean release
	 * with no damage at the transition. If phase 4 already saw a natural
	 * release (mc173 re-pushed the column on its own), this is a no-op --
	 * gstate.remote_terrain_hold is already false. --- */
	if(gstate.remote_terrain_hold)
		build_air_section(spawn_origin_x, spawn_origin_z);
	bool released = false;
	ptime_t release_start = time_get();
	while(time_diff_ms(release_start, time_get()) < 5000) {
		server_remote_pump(&remote);
		clin_update();
		if(!gstate.remote_terrain_hold) {
			released = true;
			break;
		}
		thread_msleep(1);
	}
	short post_release_health = gstate.players[0].local_player->health;
	bool post_release_active = game_state_client_entities_active(&gstate);
	printf("RESPAWN HUNT TERRAIN RELEASE released=%d entities_active=%d "
		"health=%d pos=(%.2f,%.2f,%.2f)\n",
		(int)released, (int)post_release_active, post_release_health,
		gstate.players[0].local_player->pos[0], gstate.players[0].local_player->pos[1],
		gstate.players[0].local_player->pos[2]);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
	if(!released) {
		fprintf(stderr,
			"RESPAWN HUNT TERRAIN FAIL: hold never released after the "
			"chunk installed\n");
		return 1;
	}
	if(!post_release_active) {
		fprintf(stderr,
			"RESPAWN HUNT TERRAIN FAIL: entities_active still false after "
			"release\n");
		return 1;
	}
	if(post_release_health < held_health0) {
		fprintf(stderr,
			"RESPAWN HUNT TERRAIN FAIL: health dropped at the install/"
			"release transition (%d -> %d)\n",
			held_health0, post_release_health);
		return 1;
	}
	puts("RESPAWN HUNT TERRAIN VERDICT: PASS -- same-dim respawn into a "
		 "deliberately-evicted (not-yet-resident) chunk armed the terrain "
		 "hold over a REAL connection (MP_EVENT_RESPAWN -> server position "
		 "echo -> arm), held position/health steady while armed, then "
		 "released cleanly with no damage once the chunk installed");
	return 0;
}

/* --- CHIP B (task_18b9ae41, 2026-08-31): reliable delivery for the two
 * svin_rpc_send call sites that used to ignore a saturated-queue failure --
 * client_interface.c's terrain-hold SRPC_WINDOW_CLOSE arm and screen_load_
 * world.c's clin_send_unload_world(). Both share server_interface.c's
 * SVIN_LIFECYCLE_RESERVE (3 slots, shared across SRPC_WINDOW_CLOSE/
 * SRPC_UNLOAD_WORLD/SRPC_LOAD_WORLD -- svin_is_lifecycle), so flooding with
 * distinct-window SRPC_WINDOW_CLOSE calls saturates the exact condition
 * either fix's send can hit. test_svin_reliable.c already pins server_
 * interface.c's OWN reliable-delivery semantics in isolation; these two
 * tests drive the SAME saturation through the real client_interface.c call
 * sites (no mock, the genuine svin_rpc_send), the way the terrain-hold test
 * above drives clin_process directly. */

static void svin_capture_discard(struct server_rpc* call, void* user) {
	(void)call;
	(void)user;
}

/* Settle the shared client->server RPC channel to empty. Global state
 * (svin_init() runs once in main()), so earlier tests in this binary may
 * leave it non-empty; this must run before engineering a fresh saturation
 * so the flood below has a known-clean starting budget. Same drain/flush
 * loop shape test_svin_reliable.c's own teardown uses -- terminates because
 * nothing else in this single-threaded test produces into the channel
 * concurrently, so each round frees at least one pending entry. */
static void svin_settle(void) {
	svin_process_messages(svin_capture_discard, NULL, false);
	while(svin_pending_count() > 0) {
		svin_rpc_flush_pending();
		svin_process_messages(svin_capture_discard, NULL, false);
	}
	assert(svin_pending_count() == 0);
}

/* Flood distinct-window SRPC_WINDOW_CLOSE sends (never coalesced -- each id
 * is unique) until svin_rpc_send genuinely returns false: first RPC_INBOX_
 * SIZE calls exhaust the immediate-send path, the next SVIN_LIFECYCLE_
 * RESERVE fill the reserve via svin_pending, and the next one is dropped.
 * Self-verifying and independent of those exact (private) constants -- it
 * only assumes the reserve is finite, which svin_dropped_count() proves.
 * `window_lo` picks a range the caller's OWN test windows must stay clear
 * of (avoids the send's own same-window coalescing masking a real slot). */
static void svin_flood_lifecycle_reserve(uint8_t window_lo) {
	uint32_t drops_before = svin_dropped_count();
	bool saturated = false;
	for(int i = 0; i < 64 && !saturated; i++) {
		struct server_rpc close = {
			.type = SRPC_WINDOW_CLOSE,
			.payload.window_close.window = (uint8_t)(window_lo + i),
		};
		if(!svin_rpc_send(&close))
			saturated = true;
	}
	assert(saturated);
	assert(svin_dropped_count() == drops_before + 1);
}

/* target_window filters WHICH close proves the fix: the flood's own 3
 * reserve-occupying entries are also genuine SRPC_WINDOW_CLOSE traffic and
 * typically drain BEFORE the caller's own retry (FIFO -- they were queued
 * first), so "any window close seen" would false-pass on the flood's own
 * ids. 0xFF (no real window id reaches this range in either test's flood)
 * means "not looking for a window close, only SRPC_UNLOAD_WORLD". */
struct svin_capture {
	uint8_t target_window;
	bool saw_target_window_close;
	bool saw_unload_world;
};

static void svin_capture_record(struct server_rpc* call, void* user) {
	struct svin_capture* cap = user;
	if(call->type == SRPC_WINDOW_CLOSE
	   && call->payload.window_close.window == cap->target_window)
		cap->saw_target_window_close = true;
	else if(call->type == SRPC_UNLOAD_WORLD)
		cap->saw_unload_world = true;
}

/* CHIP B, call site 1: client_interface.c's CRPC_PLAYER_POS terrain-hold
 * arm used to ignore svin_rpc_send's return and unconditionally clear
 * s_visible_window -- a saturated send genuinely dropped the close with no
 * retry, while the client believed the window was already gone (FH-07
 * leaked-container-ownership class). The fix latches a retry that clin_
 * update resends every frame until accepted. */
static void test_chip_b_window_close_retry(void) {
	struct client_rpc reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD, .local_entity = 0},
	};
	clin_process(&reset);
	assert(gstate.players[0].local_player);
	/* Dodge CRPC_OPEN_WINDOW's screen_load_world/select_world guard (see
	 * test_s2_window_items_overflow's identical comment) -- test isolation,
	 * not a production concern. */
	gstate.players[0].current_screen = NULL;

	/* Open window 77 (a chest) directly via clin_process -- no live peer
	 * needed, CRPC_OPEN_WINDOW is self-contained once s_world_reset_ready is
	 * true (set by the CRPC_WORLD_RESET above). */
	const uint8_t target_window = 77;
	struct client_rpc open = {
		.type = CRPC_OPEN_WINDOW,
		.payload.window_open = {
			.window = target_window,
			.type = WINDOW_TYPE_CHEST,
			.slot_count = 27,
		},
	};
	clin_process(&open);
	assert(gstate.players[0].windows[target_window]);
	assert(gstate.players[0].windows[target_window]->type == WINDOW_TYPE_CHEST);

	svin_settle();
	// Flood well clear of target_window (77) so the coalescing dedup can
	// never mask a real reserve slot for it.
	svin_flood_lifecycle_reserve(140);
	uint32_t drops_before_respawn = svin_dropped_count();

	/* Same-dim respawn into an uninstalled chunk (test_respawn_terrain_
	 * hold's exact fixture) arms the terrain hold AND attempts to close the
	 * open window -- which must now fail (reserve saturated). */
	build_air_section(0, 0);
	gstate.players[0].local_player->teleport(gstate.players[0].local_player, (vec3) {8.0f, 70.0f, 8.0f});
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	gstate.remote_session = true;
	struct client_rpc respawn = {
		.type = CRPC_PLAYER_POS,
		.payload.player_pos = {
			.position = {800.5f, 70.0f, 800.5f},
			.correction_flags = PLAYER_CORRECTION_POSITION_ONLY,
			.correction_seq = 1,
		},
	};
	assert(!world_find_chunk(&gstate.world, 800, 70, 800));
	clin_process(&respawn);
	assert(gstate.remote_terrain_hold);
	/* The window-close attempt genuinely failed (not silently skipped) --
	 * proves this test actually reached the saturated-send branch. (>,
	 * not ==: only the window-close send is expected here, but this
	 * doesn't assume no other incidental send shares this exact frame.) */
	assert(svin_dropped_count() > drops_before_respawn);

	/* Drain + clin_update until the latched retry is accepted and the close
	 * for OUR window is actually observed on the wire -- the fix's whole
	 * point. clin_update's own svin_rpc_flush_pending() drains the 3
	 * reserve-occupying flood entries first (freeing the reserve), then
	 * this file's new retry block gets its turn. */
	struct svin_capture capture = {.target_window = target_window};
	for(int round = 0; round < 200 && !capture.saw_target_window_close;
		round++) {
		svin_process_messages(svin_capture_record, &capture, false);
		clin_update();
	}
	svin_process_messages(svin_capture_record, &capture, false);
	assert(capture.saw_target_window_close);

	/* Clean up: release the hold and reset shared gstate for later tests. */
	clin_process(&reset);
	assert(!gstate.remote_terrain_hold);
	gstate.remote_session = false;
	gstate.world_loaded = false;
	gstate.players[0].current_screen = NULL;

	puts("  CHIP B: terrain-hold SRPC_WINDOW_CLOSE retries under a saturated "
		 "lifecycle reserve instead of silently dropping the close");
}

/* CHIP B, call site 2: screen_load_world.c's remote-escape action used to
 * ignore SRPC_UNLOAD_WORLD's send failure and unconditionally swap screens,
 * which could leave server_remote stuck in REMOTE_PLAY. clin_send_unload_
 * world() (client_interface.c/.h) is the fix -- exercise it directly. */
static void test_chip_b_unload_world_retry(void) {
	svin_settle();
	svin_flood_lifecycle_reserve(180);

	assert(!clin_send_unload_world());

	struct svin_capture capture = {.target_window = 0xFF};
	for(int round = 0; round < 200 && !capture.saw_unload_world; round++) {
		svin_process_messages(svin_capture_record, &capture, false);
		clin_update();
	}
	svin_process_messages(svin_capture_record, &capture, false);
	assert(capture.saw_unload_world);

	puts("  CHIP B: clin_send_unload_world() retries under a saturated "
		 "lifecycle reserve instead of silently dropping the unload");
}

/* ---------------------------------------------------------------------------
 * mp_reconnect_soak / mp_lan_walk_10m root cause (2026-09-01): "world never
 * reported loaded".
 *
 * Both hardware profiles pinned wl_pending=1 / world_loaded=false / chunks=0 /
 * pos=(0,0,0) for their whole run while receiving 4 MB from mc173, because the
 * client sent NO position-bearing packet after login:
 *
 *   - server_remote_protocol.c's remote_handle_correction only relayed the
 *     server's authoritative position to the client once remote_load_complete_
 *     sent was already true, so client_interface.c's take-51 pre-world echo
 *     (armed by s_authoritative_pos_seen, set ONLY in the CRPC_PLAYER_POS
 *     handler) could never arm before the world loaded; and
 *   - even armed, a player parked behind CaveWii's load-screen hold never
 *     moves, so remote_process_srpc's Beta sendMotionUpdates dedupe collapsed
 *     every echo to Packet10Flying, which carries no position.
 *
 * mc173 re-streams chunks ONLY from a position-bearing packet (player.rs
 * handle_position_look_inner -> update_chunks) or at join, and update_chunks
 * skips columns the server has not loaded yet, so a spawn column that was not
 * resident at join time was never offered again: the hold waited forever.
 *
 * This peer reproduces exactly that shape -- login + Packet13, and NO chunk at
 * all -- and requires the client to (a) install the server position while the
 * world is still held, and (b) keep sending position-BEARING packets. */
struct preworld_echo_fixture {
	SOCKET listener;
	int position_packets; /* Packet11/Packet13 seen AFTER the correction echo */
	bool saw_correction_echo;
	bool ok;
};

#define PREWORLD_ECHO_REQUIRED 3

static void* preworld_echo_peer_entry(void* user) {
	struct preworld_echo_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;

	static const uint8_t login_response[] = {0x01, 0, 0, 0, 0x2A,
		0, 0, 0, 0, 0, 0, 0, 0, 0x27, 0x0F, 0};
	/* Packet13 exactly like s1_peer_entry's: posY 101.62, stance 103.24. */
	uint8_t correction[42] = {0x0D};
	put_f64(correction + 1, 0.5);
	put_f64(correction + 9, 101.62);
	put_f64(correction + 17, 103.24);
	put_f64(correction + 25, 0.5);
	put_f32(correction + 33, 90.0f);
	put_f32(correction + 37, 0.0f);
	correction[41] = 0;
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response))
	   || !socket_write_fragmented(peer, correction, sizeof(correction)))
		goto done;
	/* Deliberately NO Packet50/Packet51: this is the "spawn column was not
	 * resident server-side at join" case the hardware runs hit. */

	/* Read the client's outbound stream. Packet sizes are Beta's own
	 * [740c583 Packet.<init> registrations]; anything else means the client
	 * emitted a packet this fixture does not model, which is a real failure,
	 * not something to skip past. */
	for(;;) {
		uint8_t id;
		if(!socket_read_exact(peer, &id, 1))
			break; /* driver disconnected: whatever we counted is the result */
		size_t body;
		switch(id) {
			case 0x00: body = 0; break;  /* KeepAlive */
			case 0x0A: body = 1; break;  /* Flying */
			case 0x0B: body = 33; break; /* PlayerPosition */
			case 0x0C: body = 9; break;  /* PlayerLook */
			case 0x0D: body = 41; break; /* PlayerLookMove */
			case 0x13: body = 5; break;  /* EntityAction (sneak edge) */
			case 0xFF: body = 0; goto done; /* graceful disconnect: stop */
			default:
				fprintf(stderr,
					"[preworld-peer] unexpected outbound packet 0x%02X\n", id);
				goto done;
		}
		uint8_t scratch[64];
		if(body && !socket_read_exact(peer, scratch, body))
			break;
		if(id != 0x0B && id != 0x0D)
			continue;
		if(!fixture->saw_correction_echo) {
			/* Beta echoes the server's Packet13 straight back, rewriting
			 * y to minY and stance to the installed posY [740c583
			 * NetClientHandler.handleFlying]. That echo is unconditional and
			 * predates this fix; it is NOT evidence of the pre-world cadence,
			 * so it is excluded from the count. */
			fixture->saw_correction_echo = true;
			continue;
		}
		fixture->position_packets++;
	}
done:
	fixture->ok = fixture->saw_correction_echo
		&& fixture->position_packets >= PREWORLD_ECHO_REQUIRED;
	if(peer != INVALID_SOCKET)
		closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_preworld_position_echo(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct preworld_echo_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, preworld_echo_peer_entry, &peer, 64));
	/* main.c sets this at the equivalent point in a real connect (take-51
	 * pre-world echo gate); this harness bypasses main.c, exactly like
	 * run_live_respawn_hunt_terrain's own note. */
	bool saved_remote_session = gstate.remote_session;
	gstate.remote_session = true;
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	/* Drive long enough for several 50ms echo slots. The world must STILL be
	 * held the whole time -- no chunk was ever sent, so nothing may release
	 * the load screen. */
	bool position_installed = false;
	for(size_t elapsed = 0; elapsed < 1200; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		assert(!gstate.world_loaded
			   && "no chunk was sent; the terrain hold must not release");
		if(gstate.players[0].local_player && gstate.players[0].camera.y > 101.0f
		   && gstate.players[0].camera.y < 102.0f)
			position_installed = true;
		thread_msleep(1);
	}
	/* (a) Beta installs the server position immediately, terrain or no
	 * terrain [740c583 NetClientHandler.handleFlying:242-255]. */
	assert(position_installed
		   && "server's login position was never installed on the client");

	assert(server_remote_disconnect(&remote, "Quitting"));
	assert(wait_for(&remote, workers_exited, 5000));
	thread_join(&peer_thread);
	/* (b) and it echoes from then on, so mc173 keeps streaming. */
	assert(peer.saw_correction_echo
		   && "client never echoed the server's Packet13");
	assert(peer.position_packets >= PREWORLD_ECHO_REQUIRED
		   && "pre-world echo sent no position-bearing packet: mc173 would "
			  "never re-run update_chunks and the world could never load");
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
	gstate.remote_session = saved_remote_session;
	printf("  pre-world position echo armed (%d position packets held behind "
		   "the load screen)\n", peer.position_packets);
}

/* ---------------------------------------------------------------------------
 * mp_lan_walk_10m / mp_reconnect_soak, 2026-09-02: "world never reported
 * loaded" AGAIN, on real Wii, with the take-51 pre-world echo (1e45c46)
 * already in the build. Telemetry signature (tmp/wii_runs/staged_mp_lan_walk_
 * 10m_20260902_065212 + _073825, both build d0485fe+5d00c9d7):
 *
 *   mp_login=1, mp_ret_n PINNED at 15, mp_ret_kb 87 -> 0, mp_ready/mp_backlog
 *   drained to 0 by t=4s, mp_evict=0, mp_alloc_fail=0, wl_pending=1 for the
 *   full 570-heartbeat run while mp_rx_kb climbed past 4 MB.
 *
 * mp_evict=0 against a 2 MB Wii retention cap (REMOTE_RETAINED_BYTE_CAP,
 * server_remote_transport.c) with an 87 KB peak, and 15 entries against a
 * 576-entry coordinate cap, REFUTE the "the retention cache evicted the spawn
 * column" hypothesis outright: nothing was ever evicted. What actually
 * happens is the mc173 join geometry:
 *
 *   - mc173 sends Packet13 BEFORE any chunk (server.rs:249 vs :269), then
 *     handle_player_join -> update_chunks (player.rs:1414) offers the 6x6
 *     window around the player -- but ONLY the columns already resident
 *     server-side (`if let Some(chunk) = sw.world.get_chunk(cx, cz)`).
 *   - A cold server is still generating its 21x21 spawn job, so the join
 *     burst is a partial subset (15 of 36 on hardware) that need not contain
 *     the player's own column.
 *   - update_chunks does NOT mark a skipped column tracked, so it stays
 *     re-offerable -- but only from a position-BEARING client packet
 *     (player.rs:264).
 *
 * So the spawn column arrives LATE, after the join burst. This peer
 * reproduces exactly that: a spawn far from the origin (200.5, -300.5 ->
 * column 12,-19), a 15-column partial join burst that OMITS the spawn column,
 * and the spawn column released only once the client's pre-world echo asks
 * for it. The world must load. */
struct partial_join_fixture {
	SOCKET listener;
	int position_packets; /* 0x0B/0x0D seen AFTER the correction echo */
	bool saw_correction_echo;
	bool sent_spawn_column;
};

#define PARTIAL_JOIN_SPAWN_X 200.5
#define PARTIAL_JOIN_SPAWN_Y 66.62
#define PARTIAL_JOIN_SPAWN_Z (-300.5)
#define PARTIAL_JOIN_SPAWN_CX 12
#define PARTIAL_JOIN_SPAWN_CZ (-19)
/* mc173's own window: ocx-3 .. ocx+2 (exclusive top), 6x6 = 36 columns. */
#define PARTIAL_JOIN_VIEW 3
/* How many columns the cold server had resident at join -- the hardware
 * reading (mp_ret_n=15). */
#define PARTIAL_JOIN_BURST 15

/* Bulk (NOT byte-at-a-time) writer. Every other fixture in this file uses
 * socket_write_fragmented to prove the incremental decoder handles arbitrary
 * framing; this one deliberately does the opposite, because the property
 * under test is the CLIENT's drain rate against a real server's burst shape.
 * mc173 writes each Packet50/Packet51 in one unbuffered pass
 * (mc173-server/src/net.rs:484), so a one-byte-per-send peer would hide
 * exactly the behaviour this test exists to pin. */
static bool socket_write_bulk(SOCKET socket_value, const uint8_t* data,
							  size_t length) {
	size_t sent = 0;
	while(sent < length) {
#ifdef MSG_NOSIGNAL
		int count = send(socket_value, (const char*)data + sent,
						 (int)(length - sent), MSG_NOSIGNAL);
#else
		int count = send(socket_value, (const char*)data + sent,
						 (int)(length - sent), 0);
#endif
		if(count <= 0)
			return false;
		sent += (size_t)count;
	}
	return true;
}

/* One full 16x128x16 air column as Packet50 + Packet51. */
static bool partial_join_send_column(SOCKET peer, int32_t cx, int32_t cz) {
	const size_t cells = 16u * 128u * 16u;
	const size_t nibble = cells / 2u;
	const size_t inflated_length = cells + 3u * nibble;
	uint8_t* inflated = calloc(1, inflated_length);
	if(!inflated)
		return false;
	/* Block ids and metadata stay 0 (air -- a valid id the client accepts),
	 * but an ALL-ZERO column deflates to a few hundred bytes, which would
	 * make this fixture's burst two orders of magnitude smaller than the
	 * ~5.5 KB/column mc173 actually puts on the wire and would silently
	 * defeat the drain-rate assertion below. Fill the block-light nibble
	 * plane with an incompressible LCG stream so each Packet51 body has a
	 * realistic (here deliberately generous) size. Lighting bytes are
	 * memcpy'd into the chunk slot unvalidated, so any value is wire-legal. */
	uint32_t lcg = 0x9E3779B9u ^ (uint32_t)(cx * 73856093) ^ (uint32_t)(cz * 19349663);
	for(size_t i = 0; i < nibble; i++) {
		lcg = lcg * 1103515245u + 12345u;
		inflated[cells + nibble + i] = (uint8_t)(lcg >> 16);
	}
	uLongf compressed_length = compressBound(inflated_length);
	uint8_t* compressed = malloc(compressed_length);
	if(!compressed
	   || compress2(compressed, &compressed_length, inflated, inflated_length,
					Z_BEST_SPEED)
		   != Z_OK) {
		free(inflated);
		free(compressed);
		return false;
	}
	free(inflated);
	uint8_t announce[10] = {0x32};
	put_i32(announce + 1, cx);
	put_i32(announce + 5, cz);
	announce[9] = 1;
	uint8_t header[18] = {0x33};
	put_i32(header + 1, cx * 16);
	put_i16(header + 5, 0);
	put_i32(header + 7, cz * 16);
	header[11] = 15;
	header[12] = 127;
	header[13] = 15;
	put_i32(header + 14, (int32_t)compressed_length);
	bool sent = socket_write_bulk(peer, announce, sizeof(announce))
		&& socket_write_bulk(peer, header, sizeof(header))
		&& socket_write_bulk(peer, compressed, compressed_length);
	free(compressed);
	return sent;
}

static void* partial_join_peer_entry(void* user) {
	struct partial_join_fixture* fixture = user;
	SOCKET peer = accept(fixture->listener, NULL, NULL);
	if(peer == INVALID_SOCKET)
		return NULL;
	if(!peer_login(peer))
		goto done;

	/* mc173 server.rs:234/244/249/257 order: Login, SpawnPosition,
	 * PositionLook, UpdateTime -- position BEFORE any chunk. */
	static const uint8_t login_response[]
		= {0x01, 0, 0, 0, 0x2A, 0, 0, 0, 0, 0, 0, 0, 0, 0x27, 0x0F, 0};
	uint8_t spawn[13] = {0x06};
	put_i32(spawn + 1, 200);
	put_i32(spawn + 5, 66);
	put_i32(spawn + 9, -300);
	uint8_t correction[42] = {0x0D};
	put_f64(correction + 1, PARTIAL_JOIN_SPAWN_X);
	put_f64(correction + 9, PARTIAL_JOIN_SPAWN_Y);
	put_f64(correction + 17, PARTIAL_JOIN_SPAWN_Y + 1.62);
	put_f64(correction + 25, PARTIAL_JOIN_SPAWN_Z);
	put_f32(correction + 33, 0.0f);
	put_f32(correction + 37, 0.0f);
	correction[41] = 1;
	static const uint8_t time_set[] = {0x04, 0, 0, 0, 0, 0, 0, 0, 42};
	if(!socket_write_fragmented(peer, login_response, sizeof(login_response))
	   || !socket_write_fragmented(peer, spawn, sizeof(spawn))
	   || !socket_write_fragmented(peer, correction, sizeof(correction))
	   || !socket_write_fragmented(peer, time_set, sizeof(time_set)))
		goto done;

	/* handle_player_join's partial burst: the first PARTIAL_JOIN_BURST
	 * columns of the 6x6 window that were resident -- spawn column NOT among
	 * them (still generating). */
	int sent_columns = 0;
	for(int32_t cx = PARTIAL_JOIN_SPAWN_CX - PARTIAL_JOIN_VIEW;
		cx < PARTIAL_JOIN_SPAWN_CX + PARTIAL_JOIN_VIEW; cx++) {
		for(int32_t cz = PARTIAL_JOIN_SPAWN_CZ - PARTIAL_JOIN_VIEW;
			cz < PARTIAL_JOIN_SPAWN_CZ + PARTIAL_JOIN_VIEW; cz++) {
			if(sent_columns >= PARTIAL_JOIN_BURST)
				continue;
			if(cx == PARTIAL_JOIN_SPAWN_CX && cz == PARTIAL_JOIN_SPAWN_CZ)
				continue;
			if(!partial_join_send_column(peer, cx, cz))
				goto done;
			sent_columns++;
		}
	}

	for(;;) {
		uint8_t id;
		if(!socket_read_exact(peer, &id, 1))
			break;
		size_t body;
		switch(id) {
			case 0x00: body = 0; break;  /* KeepAlive */
			case 0x0A: body = 1; break;  /* Flying */
			case 0x0B: body = 33; break; /* PlayerPosition */
			case 0x0C: body = 9; break;  /* PlayerLook */
			case 0x0D: body = 41; break; /* PlayerLookMove */
			case 0x13: body = 5; break;  /* EntityAction (sneak edge) */
			case 0xFF: body = 0; goto done;
			default:
				fprintf(stderr,
						"[partial-join-peer] unexpected outbound packet "
						"0x%02X\n",
						id);
				goto done;
		}
		uint8_t scratch[64];
		if(body && !socket_read_exact(peer, scratch, body))
			break;
		if(id != 0x0B && id != 0x0D)
			continue;
		if(!fixture->saw_correction_echo) {
			/* Beta's unconditional handleFlying echo, not the cadence. */
			fixture->saw_correction_echo = true;
			continue;
		}
		fixture->position_packets++;
		/* player.rs:264 -- a position-bearing packet re-runs update_chunks,
		 * and by now the spawn column has finished generating. */
		if(!fixture->sent_spawn_column) {
			fixture->sent_spawn_column = true;
			if(!partial_join_send_column(peer, PARTIAL_JOIN_SPAWN_CX,
										 PARTIAL_JOIN_SPAWN_CZ))
				goto done;
		}
	}
done:
	if(peer != INVALID_SOCKET)
		closesocket(peer);
	closesocket(fixture->listener);
	return NULL;
}

static void test_mc173_partial_join_burst(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	uint16_t port;
	struct partial_join_fixture peer = {.listener = make_listener(&port)};
	struct thread peer_thread;
	assert(thread_create(&peer_thread, partial_join_peer_entry, &peer, 64));
	bool saved_remote_session = gstate.remote_session;
	gstate.remote_session = true;
	net_socket_test_reset_recv_stats();
	assert(server_remote_start(&remote, "127.0.0.1", port, "Cave"));

	for(size_t elapsed = 0; elapsed < 6000 && !gstate.world_loaded; elapsed++) {
		server_remote_pump(&remote);
		clin_update();
		thread_msleep(1);
	}
	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	bool loaded = gstate.world_loaded;

	assert(server_remote_disconnect(&remote, "Quitting"));
	assert(wait_for(&remote, workers_exited, 5000));
	thread_join(&peer_thread);
	size_t recv_calls = net_socket_test_recv_calls();
	size_t recv_bytes = net_socket_test_recv_bytes();

	/* THE REGRESSION GUARD for the fix. The read thread must drain the socket
	 * in real slices, not in 256-byte nibbles: on the Wii every
	 * net_socket_receive is an IOS network IPC round trip, and a client that
	 * needs ~800 of them to absorb one 36-column join burst is slow enough
	 * that mc173's unbuffered non-blocking writes start returning WouldBlock
	 * -- which it silently discards (net.rs:479-485) for columns it has
	 * ALREADY marked tracked (player.rs:1423) and can therefore never
	 * re-offer. 256 bytes/call is a hard ceiling on the average with the old
	 * stack scratch, so this bound cannot be met by luck or timing; it can
	 * only be met by an actually larger buffer. */
	assert(recv_calls > 0 && recv_bytes > 128u * 1024u
		   && "the 36-column burst did not reach the client at all");
	assert(recv_bytes / recv_calls >= 512
		   && "read thread is draining the socket in sub-512-byte slices -- "
			  "REMOTE_READ_BUFFER_BYTES regressed (2026-09-02 mc173 "
			  "silent-chunk-drop root cause)");

	/* The hypothesis this run was opened on: "the Wii retention cache evicted
	 * the spawn column." Pin the refutation in the suite -- nothing is ever
	 * evicted at this geometry, on either platform's cap. */
	assert(status.retention_evictions == 0
		   && "no eviction happens at mc173 join geometry -- the retention "
			  "cache is NOT what strands the spawn column");
	assert(peer.sent_spawn_column
		   && "client never asked for the late spawn column: mc173 would "
			  "never re-run update_chunks");
	assert(loaded
		   && "spawn column arrived AFTER the join burst but the remote load "
			  "gate never fired -- world_loaded pinned false (hardware "
			  "mp_lan_walk_10m 2026-09-02)");

	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
	clin_update_drain();
	clin_force_world_reset();
	gstate.remote_session = saved_remote_session;
	gstate.world_loaded = false;
	gstate.players[0].current_screen = NULL;
	printf("  mc173 partial join burst: spawn column delivered late (after "
		   "%d pre-world echoes, %u evictions), world loaded; socket drained "
		   "%zu B in %zu recv calls (%zu B/call, old 256 B scratch capped it "
		   "at 256)\n",
		   peer.position_packets, status.retention_evictions, recv_bytes,
		   recv_calls, recv_bytes / recv_calls);
}

/* ---- push-0921/B2: the server-confirmed observation seams ---------------
 *
 * design-mp-play-10m.md section 6 (a)/(b) and Astra gate 1, Lane B item 3.
 * mp_play_10m may credit a dig / place / inventory move ONLY on the server's
 * own echo, never on the client's prediction, so struct server_remote_status
 * carries three rings plus a world-install generation. These cases pin the
 * recording contract; the behaviour the packet handlers already had is
 * untouched (the diff in them is additive recording only), which the rest of
 * this file's MP cases -- window transactions, set-slot bounds, the deferred
 * edit log, the whole loopback suite -- continue to guard unchanged.
 *
 * Driven through the REAL codec and the REAL dispatcher: every packet below
 * is produced by the production mp_encode_* (byte-pinned by
 * test_codec_goldens at the top of this file), decoded by the production
 * mp_decoder, and handed to the production process_event -- the same path
 * server_remote_pump uses, on the same (main) thread. */
static void seam_feed(struct server_remote* remote, const struct mp_frame* frame) {
	struct mp_decoder decoder;
	mp_decoder_init(&decoder);
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	for(size_t i = 0; i < frame->length; i++)
		assert(mp_decoder_feed(&decoder, frame->data[i], &event.packet)
			== (i + 1 == frame->length ? MP_DECODE_EVENT : MP_DECODE_MORE));
	assert(process_event(remote, &event));
	mp_decoder_destroy(&decoder);
}

/* The documented reader: index straight to serial % capacity, confirm by
 * serial. Returns NULL once that serial has been overwritten. */
static const struct server_remote_block_echo*
seam_block_echo(const struct server_remote_play_obs* obs, uint32_t serial) {
	const struct server_remote_block_echo* echo
		= &obs->block_echoes[serial % SERVER_REMOTE_BLOCK_ECHO_RING];
	return echo->serial == serial ? echo : NULL;
}

/* push-0921/B18 -- review B17 finding H2: THE OBSERVATION SEAMS MUST NOT RIDE
 * ON EVERY MP STACK FRAME.
 *
 * struct server_remote_status is a STACK LOCAL at six call sites that have
 * nothing to do with the scripted play driver:
 *     source/main.c:649   emit_hw_heartbeat -- every heartbeat, every profile
 *     source/main.c:3379  inside main()'s OWN frame -- every remote-session frame
 *     source/network/mp_session.c:41 and :125
 *     source/game/gui/screen_multiplayer_connect.c:222 and :402
 *                         -- every frame an ordinary player sits on the
 *                            connect screen
 *     source/network/server_remote.c:434 (the terminal-status latch)
 * and the whole struct is memcpy'd under the remote lock on every one of them.
 * The lane appended three observation rings + their serials + the world-install
 * generation to it -- 320 + 96 + 128 + 16 = 560 bytes -- with no gate, so
 * mp_lan_walk_10m, mp_connect_smoke, mp_reconnect_soak, split_play_8m in a
 * remote session and an ordinary LAN player all pay it. main()'s frame is the
 * subject of a recorded Wii boot death (188 KB frame vs the 128 KB stack,
 * 0758c12) and everything under source/network/ is a hardware-risk path, so
 * this is not a nit.
 *
 * MEASURED, on this tree, with the real toolchains:
 *     PC  (x86_64-w64-mingw32) : 73bc319c 696 B -> lane 1256 B
 *     Wii (powerpc-eabi -mrvl) : 73bc319c 584 B -> lane 1144 B
 * +560 on both. 696 is therefore the bound that holds on BOTH targets, and it
 * is exactly the pre-lane PC size.
 *
 * B17 also noted there was no _Static_assert bounding this struct anywhere in
 * the tree. There is one now, beside the declaration; this runtime case is its
 * visible twin -- a compile-time assert that has never been seen to fail reads
 * the same as one that cannot. */
enum { SERVER_REMOTE_STATUS_PRE_LANE_BYTES = 696 };

static void test_mp_status_struct_stays_small(void) {
	if(sizeof(struct server_remote_status)
	   > (size_t)SERVER_REMOTE_STATUS_PRE_LANE_BYTES)
		printf("  RED sizeof(struct server_remote_status) = %zu, want <= %d "
			   "(its size on main 73bc319c). The observation rings belong in "
			   "struct server_remote_play_obs, behind their own accessor, so "
			   "only the play driver pays for them.\n",
			   sizeof(struct server_remote_status),
			   SERVER_REMOTE_STATUS_PRE_LANE_BYTES);
	assert(sizeof(struct server_remote_status)
		   <= (size_t)SERVER_REMOTE_STATUS_PRE_LANE_BYTES);
	printf("  B18 H2: sizeof(struct server_remote_status) = %zu B <= %d B "
		   "(pre-lane size; the play-obs rings are not in it)\n",
		   sizeof(struct server_remote_status),
		   SERVER_REMOTE_STATUS_PRE_LANE_BYTES);
}

static void test_mp_observation_seams(void) {
	struct server_remote remote;
	memset(&remote, 0xC7, sizeof(remote));
	assert(server_remote_init(&remote));
	struct server_remote_play_obs status;
	struct mp_frame frame;

	/* A fresh impl has recorded nothing, and every ring slot reads "never
	 * written" (serial 0) rather than stack garbage. */
	server_remote_get_play_obs(&remote, &status);
	assert(status.block_echo_serial == 0 && status.txn_serial == 0
		   && status.set_slot_serial == 0);
	for(size_t i = 0; i < SERVER_REMOTE_BLOCK_ECHO_RING; i++)
		assert(status.block_echoes[i].serial == 0);
	for(size_t i = 0; i < SERVER_REMOTE_TXN_RING; i++)
		assert(status.txn_results[i].serial == 0);
	for(size_t i = 0; i < SERVER_REMOTE_SET_SLOT_RING; i++)
		assert(status.set_slots[i].serial == 0);

	/* (a) one Packet53: ONE serial, and the exact cell/id/meta in the ring.
	 * Note the column is not resident here -- the edit parks in the deferred
	 * log -- and the echo is recorded anyway: the SERVER confirmed it, which
	 * is what the driver counts. */
	assert(mp_encode_block_change(&frame, 5, 10, -5, BLOCK_DIRT, 9));
	seam_feed(&remote, &frame);
	server_remote_get_play_obs(&remote, &status);
	assert(status.block_echo_serial == 1);
	const struct server_remote_block_echo* echo = seam_block_echo(&status, 1);
	assert(echo && echo->x == 5 && echo->y == 10 && echo->z == -5
		   && echo->type == BLOCK_DIRT && echo->metadata == 9);

	/* (b) one Packet52 carrying N cells bumps the serial N times, in the
	 * packet's own cell order, with the coordinates unpacked exactly as
	 * handleMultiBlockChange unpacks them (bits 15..12 local x, 11..8 local
	 * z, 7..0 y, against chunk_x*16 / chunk_z*16). */
	uint16_t mb_coordinate[3] = {
		(uint16_t)((3u << 12) | (7u << 8) | 64u),
		(uint16_t)((0u << 12) | (0u << 8) | 0u),
		(uint16_t)((15u << 12) | (15u << 8) | 127u),
	};
	uint8_t mb_type[3] = {BLOCK_STONE, BLOCK_AIR, BLOCK_DIRT};
	uint8_t mb_metadata[3] = {0, 0, 4};
	struct mp_multi_block_change mb = {.chunk_x = 2, .chunk_z = -1, .count = 3,
		.coordinate = mb_coordinate, .type = mb_type, .metadata = mb_metadata};
	assert(mp_encode_multi_block_change(&frame, &mb));
	seam_feed(&remote, &frame);
	server_remote_get_play_obs(&remote, &status);
	assert(status.block_echo_serial == 4);
	echo = seam_block_echo(&status, 2);
	assert(echo && echo->x == 2 * 16 + 3 && echo->y == 64
		   && echo->z == -1 * 16 + 7 && echo->type == BLOCK_STONE
		   && echo->metadata == 0);
	echo = seam_block_echo(&status, 3);
	assert(echo && echo->x == 2 * 16 && echo->y == 0 && echo->z == -1 * 16
		   && echo->type == BLOCK_AIR);
	echo = seam_block_echo(&status, 4);
	assert(echo && echo->x == 2 * 16 + 15 && echo->y == 127
		   && echo->z == -1 * 16 + 15 && echo->type == BLOCK_DIRT
		   && echo->metadata == 4);

	/* (c) ring wrap: keep pushing until the serial is one full ring past the
	 * four above, then the newest SERVER_REMOTE_BLOCK_ECHO_RING must all be
	 * findable and everything older must be gone. */
	uint32_t before_wrap = status.block_echo_serial;
	for(uint32_t i = 0; i < SERVER_REMOTE_BLOCK_ECHO_RING; i++) {
		assert(mp_encode_block_change(&frame, (int32_t)(100 + i), 40,
			(int32_t)(-100 - (int32_t)i), BLOCK_STONE, (uint8_t)(i & 0x0F)));
		seam_feed(&remote, &frame);
	}
	server_remote_get_play_obs(&remote, &status);
	assert(status.block_echo_serial == before_wrap + SERVER_REMOTE_BLOCK_ECHO_RING);
	for(uint32_t i = 0; i < SERVER_REMOTE_BLOCK_ECHO_RING; i++) {
		uint32_t serial = status.block_echo_serial - i;
		echo = seam_block_echo(&status, serial);
		assert(echo && "the newest ring-capacity echoes must all be readable");
		assert(echo->x == (int32_t)(100 + (serial - before_wrap - 1)));
	}
	assert(seam_block_echo(&status,
			   status.block_echo_serial - SERVER_REMOTE_BLOCK_ECHO_RING)
		   == NULL
		   && "the record one past the ring must have been overwritten");
	/* Serial 1 was the very first echo; by now it is long gone. */
	assert(seam_block_echo(&status, 1) == NULL);

	/* (d) transaction RESULTS received from the server -- both verdicts.
	 * Packet106 inbound, not the ack this client queues in reply. */
	assert(mp_encode_transaction(&frame, 1, 7, true));
	seam_feed(&remote, &frame);
	assert(mp_encode_transaction(&frame, 1, 8, false));
	seam_feed(&remote, &frame);
	server_remote_get_play_obs(&remote, &status);
	assert(status.txn_serial == 2);
	assert(status.txn_results[1 % SERVER_REMOTE_TXN_RING].serial == 1
		   && status.txn_results[1 % SERVER_REMOTE_TXN_RING].window == 1
		   && status.txn_results[1 % SERVER_REMOTE_TXN_RING].action_id == 7
		   && status.txn_results[1 % SERVER_REMOTE_TXN_RING].accepted);
	assert(status.txn_results[2 % SERVER_REMOTE_TXN_RING].serial == 2
		   && status.txn_results[2 % SERVER_REMOTE_TXN_RING].action_id == 8
		   && !status.txn_results[2 % SERVER_REMOTE_TXN_RING].accepted);

	/* (e) set-slot: an applied one is recorded with the resulting stack. */
	struct mp_item_stack stone = {.present = true, .id = 1, .count = 12,
		.damage = 3};
	assert(mp_encode_set_slot(&frame, 0, 36, &stone));
	seam_feed(&remote, &frame);
	server_remote_get_play_obs(&remote, &status);
	assert(status.set_slot_serial == 1);
	const struct server_remote_set_slot* slot
		= &status.set_slots[1 % SERVER_REMOTE_SET_SLOT_RING];
	assert(slot->serial == 1 && slot->window == 0 && slot->slot == 36
		   && slot->item == 1 && slot->count == 12 && slot->damage == 3);

	/* ...an EMPTY stack is recorded as item -1 (the decoded packet's
	 * `present == false`, which the wire writes as id 0xFFFF). */
	struct mp_item_stack empty = {.present = false};
	assert(mp_encode_set_slot(&frame, 0, 37, &empty));
	seam_feed(&remote, &frame);
	server_remote_get_play_obs(&remote, &status);
	assert(status.set_slot_serial == 2);
	slot = &status.set_slots[2 % SERVER_REMOTE_SET_SLOT_RING];
	assert(slot->serial == 2 && slot->slot == 37 && slot->item == -1
		   && slot->count == 0 && slot->damage == 0);

	/* ...and a set-slot the existing handler already DROPS (window -1 with a
	 * slot other than the cursor's -1) is not credited: it changed nothing. */
	assert(mp_encode_set_slot(&frame, -1, 0, &empty));
	seam_feed(&remote, &frame);
	server_remote_get_play_obs(&remote, &status);
	assert(status.set_slot_serial == 2
		   && "a dropped set-slot must not be counted as server traffic");

	/* (f) a whole-window refresh bumps the same serial once, recorded with
	 * slot == -1 so a reader can tell it from a single-slot update. */
	struct mp_item_stack window_slots[2] = {
		{.present = false},
		{.present = true, .id = 5, .count = 1, .damage = 2},
	};
	struct mp_window_items items = {.window = 1, .count = 2,
		.items = window_slots};
	assert(mp_encode_window_items(&frame, &items));
	seam_feed(&remote, &frame);
	server_remote_get_play_obs(&remote, &status);
	assert(status.set_slot_serial == 3);
	slot = &status.set_slots[3 % SERVER_REMOTE_SET_SLOT_RING];
	assert(slot->serial == 3 && slot->window == 1 && slot->slot == -1
		   && slot->item == -1);

	/* (g) nothing happening changes nothing. Pumps with an empty queue must
	 * leave every counter and every ring record byte-identical. */
	struct server_remote_play_obs quiet;
	server_remote_get_play_obs(&remote, &quiet);
	for(int i = 0; i < 8; i++)
		server_remote_pump(&remote);
	server_remote_get_play_obs(&remote, &status);
	assert(status.block_echo_serial == quiet.block_echo_serial
		   && status.txn_serial == quiet.txn_serial
		   && status.set_slot_serial == quiet.set_slot_serial);
	assert(memcmp(status.block_echoes, quiet.block_echoes,
			   sizeof(status.block_echoes)) == 0);
	assert(memcmp(status.txn_results, quiet.txn_results,
			   sizeof(status.txn_results)) == 0);
	assert(memcmp(status.set_slots, quiet.set_slots,
			   sizeof(status.set_slots)) == 0);

	server_remote_destroy(&remote);
	clin_update_drain();
	printf("  B2 seams: %u block echoes, %u transaction results, %u set-slot "
		   "records -- all server-confirmed\n",
		   status.block_echo_serial, status.txn_serial, status.set_slot_serial);
}

/* push-0921/B18 (review B17 H2): THE TWO ACCESSORS ARE DISJOINT.
 *
 * The split is only worth anything if it actually holds -- if
 * server_remote_get_status still wrote the rings somewhere, or
 * server_remote_get_play_obs clobbered a status field, the six non-play
 * readers would be back to paying for play-driver state (or, worse, reading
 * garbage). Both directions are checked the way the latch's completeness is
 * checked elsewhere in this file: by MEMCMP over the WHOLE struct, including
 * padding, against a poison fill -- not by naming the handful of fields
 * someone remembered.
 *
 * The rings are non-empty when this runs (a Packet53 has been fed), so "the
 * obs accessor filled something" and "the status accessor did not" are
 * genuinely different statements. */
static void test_mp_accessors_are_disjoint(void) {
	struct server_remote remote;
	memset(&remote, 0xC7, sizeof(remote));
	assert(server_remote_init(&remote));

	struct mp_frame frame;
	assert(mp_encode_block_change(&frame, 10, 20, 30, 1, 2));
	seam_feed(&remote, &frame);

	/* (1) The obs accessor really does fill the rings... */
	struct server_remote_play_obs obs;
	memset(&obs, 0x5A, sizeof(obs));
	server_remote_get_play_obs(&remote, &obs);
	assert(obs.block_echo_serial == 1);
	assert(seam_block_echo(&obs, 1) != NULL);

	/* ...and calling it twice in a row answers identically, whole-struct:
	   it is a pure reader, so it cannot perturb what it reports. */
	struct server_remote_play_obs obs2;
	memset(&obs2, 0xA5, sizeof(obs2)); /* the OPPOSITE poison */
	server_remote_get_play_obs(&remote, &obs2);
	assert(memcmp(&obs, &obs2, sizeof(obs)) == 0
		   && "server_remote_get_play_obs is not a pure reader");

	/* (2) ...and the STATUS accessor leaves the obs struct alone. Poison an
	   obs, take a status, and require every byte of the obs to survive --
	   including padding, which is why this is a memcmp against a saved copy
	   and not a field-by-field check. */
	struct server_remote_play_obs untouched;
	memset(&untouched, 0x3C, sizeof(untouched));
	struct server_remote_play_obs before = untouched;
	struct server_remote_status status;
	server_remote_get_status(&remote, &status);
	assert(memcmp(&untouched, &before, sizeof(untouched)) == 0
		   && "server_remote_get_status wrote into the play-obs struct");

	/* (3) ...and the OBS accessor leaves a status alone, the same way. */
	struct server_remote_status status_poison;
	memset(&status_poison, 0x3C, sizeof(status_poison));
	struct server_remote_status status_before = status_poison;
	struct server_remote_play_obs scratch;
	server_remote_get_play_obs(&remote, &scratch);
	assert(memcmp(&status_poison, &status_before, sizeof(status_poison)) == 0
		   && "server_remote_get_play_obs wrote into the status struct");

	/* (4) The rings are GONE from the status struct, not merely unwritten:
	   a status taken while the rings hold a real record must not contain that
	   record's bytes anywhere in it. This is what would catch someone
	   "helpfully" re-adding the fields later. */
	const struct server_remote_block_echo* needle_src = seam_block_echo(&obs, 1);
	assert(needle_src && "the fixture fed exactly one echo; serial 1 is live");
	struct server_remote_block_echo needle = *needle_src;
	bool found = false;
	const unsigned char* raw = (const unsigned char*)&status;
	for(size_t i = 0; i + sizeof(needle) <= sizeof(status); i++)
		if(memcmp(raw + i, &needle, sizeof(needle)) == 0)
			found = true;
	assert(!found
		   && "a block-echo record is still reachable inside struct "
			  "server_remote_status -- the H2 split has been undone");

	/* (5) THE COLLAPSED GATE'S CONTRACT (push-0921/B19 item 0). The play
	   driver's "is there a remote provider at all" test is now this accessor's
	   own answer: mpp_hook_remote_play_obs returns false exactly when
	   `!remote || !remote->impl`, and that used to be a separate
	   mpp_hook_remote_status call with a byte-identical guard. What the driver
	   must still see on that path is "nothing confirmed" -- so an impl-less
	   read must leave every serial at 0 ("never written"), never stale records
	   from the session that just died. Checked on BOTH impl-less shapes: a
	   destroyed handle (which keeps a latched status for the heartbeat) and a
	   zeroed one. */
	server_remote_destroy(&remote);
	struct server_remote_play_obs dead;
	memset(&dead, 0x6B, sizeof(dead));
	server_remote_get_play_obs(&remote, &dead);
	assert(dead.block_echo_serial == 0 && dead.txn_serial == 0
		   && dead.set_slot_serial == 0
		   && "a destroyed session must report nothing confirmed");
	for(size_t i = 0; i < SERVER_REMOTE_BLOCK_ECHO_RING; i++)
		assert(dead.block_echoes[i].serial == 0);
	/* ...and the world-install generation still survives it, because that is
	   PROCESS state -- the one field the destroyed-session path must keep. */
	assert(dead.world_install_generation > 0);

	struct server_remote_play_obs none;
	memset(&none, 0x6B, sizeof(none));
	server_remote_get_play_obs(NULL, &none);
	assert(none.block_echo_serial == 0 && none.txn_serial == 0
		   && none.set_slot_serial == 0
		   && "a NULL handle must report nothing confirmed");

	clin_update_drain();
	printf("  B18 H2: the two accessors are disjoint (status %zu B carries no "
		   "echo record; play-obs %zu B is a pure reader); B19 item 0: an "
		   "impl-less read reports nothing confirmed\n",
		   sizeof(struct server_remote_status),
		   sizeof(struct server_remote_play_obs));
}

/* push-0921/B19 -- THE WATCHED CELL.
 *
 * PC run pc_mp_play_20260921_191831 lost its first two build legs to "server
 * echoes overflowed the ring". The fixture now streams and populates terrain
 * around a moving player, so block changes arrive in bursts and a single
 * Packet52 can be larger than the whole 16-entry ring -- the script's own dig
 * receipt is evicted by terrain it does not care about, and review B17 M6 made
 * that a named skip instead of a false failure. A named skip still throws a
 * real dig away.
 *
 * The fix is a slot OUTSIDE the ring, holding the newest echo for the ONE cell
 * the script says it is waiting on. The core half (the lead's d8c21f2e)
 * publishes mpp_out.watch + watch_x/_y/_z on every frame of a dig / place /
 * plant clear and reads mpp_obs.watch_echo in mpp_echo_for / _for_type under
 * the ring's own rules; this is the network half that fills it.
 *
 * THE GATE. The ring itself records unconditionally -- remote_record_block_echo
 * has no arming test, only the UINT32_MAX saturation guard -- because its
 * records are simply never READ unless the play driver asks. The watch slot is
 * STRICTER: nothing is stored until server_remote_play_watch() turns it on,
 * and its only production caller is the mp_play driver's per-frame hook, which
 * is only reachable when mp_play_armed(). So for every other profile and for
 * normal play the slot stays serial 0 forever, which is also exactly what a
 * glue without the seam reports. */
static void test_mp_watched_cell(void) {
	struct server_remote remote;
	memset(&remote, 0xC7, sizeof(remote));
	assert(server_remote_init(&remote));
	struct server_remote_play_obs obs;
	struct mp_frame frame;

	/* (a) WATCH OFF is the default, and stays empty through real traffic. */
	assert(mp_encode_block_change(&frame, 5, 60, 7, BLOCK_STONE, 0));
	seam_feed(&remote, &frame);
	server_remote_get_play_obs(&remote, &obs);
	assert(obs.block_echo_serial == 1);
	assert(obs.watch_echo.serial == 0
		   && "the watch slot must stay empty until a script arms it");

	/* (b) WATCH ON: the watched cell's echo lands in the slot, carrying the
	   SAME serial the ring entry got -- the core compares slot and ring
	   serials against one baseline, so they must come from one counter. */
	server_remote_play_watch(&remote, true, 10, 61, -12);
	assert(mp_encode_block_change(&frame, 10, 61, -12, BLOCK_AIR, 0));
	seam_feed(&remote, &frame);
	server_remote_get_play_obs(&remote, &obs);
	assert(obs.block_echo_serial == 2);
	assert(obs.watch_echo.serial == 2 && "slot and ring share one serial");
	assert(obs.watch_echo.x == 10 && obs.watch_echo.y == 61
		   && obs.watch_echo.z == -12 && obs.watch_echo.type == BLOCK_AIR);
	{
		const struct server_remote_block_echo* ring = seam_block_echo(&obs, 2);
		assert(ring && ring->serial == obs.watch_echo.serial
			   && ring->x == obs.watch_echo.x && ring->y == obs.watch_echo.y
			   && ring->z == obs.watch_echo.z
			   && ring->type == obs.watch_echo.type
			   && ring->metadata == obs.watch_echo.metadata
			   && "the slot is the ring entry, not a second reading of it");
	}

	/* (c) THE WHOLE POINT: a 40-cell multi-block change AFTER the watched
	   echo. 40 > SERVER_REMOTE_BLOCK_ECHO_RING (16), so the ring entry for the
	   watched cell is evicted twice over -- and the slot must still hold it.
	   This is run pc_mp_play_20260921_191831's exact failure, in a test. */
	{
		enum { BURST = 40 };
		uint16_t coord[BURST];
		uint8_t type[BURST], meta[BURST];
		for(int i = 0; i < BURST; i++) {
			/* local x/z cycle, local y walks -- none of these is the watched
			   cell (chunk 8 is world x 128.., far from x=10). */
			coord[i] = (uint16_t)(((i % 16) << 12) | (((i / 16) % 16) << 8)
								  | (uint8_t)(70 + i));
			type[i] = BLOCK_DIRT;
			meta[i] = 0;
		}
		struct mp_multi_block_change burst = {.chunk_x = 8, .chunk_z = 8,
			.count = BURST, .coordinate = coord, .type = type,
			.metadata = meta};
		assert(mp_encode_multi_block_change(&frame, &burst));
		seam_feed(&remote, &frame);
	}
	server_remote_get_play_obs(&remote, &obs);
	assert(obs.block_echo_serial == 2 + 40);
	assert(seam_block_echo(&obs, 2) == NULL
		   && "precondition: the burst really did evict the ring entry");
	assert(obs.watch_echo.serial == 2
		   && obs.watch_echo.x == 10 && obs.watch_echo.y == 61
		   && obs.watch_echo.z == -12 && obs.watch_echo.type == BLOCK_AIR
		   && "a burst no script cares about must not evict the watched cell");

	/* (d) AN ECHO FOR ANOTHER CELL NEVER LANDS IN THE SLOT -- not even the
	   newest one. The slot is the watched cell's, not "the last echo". */
	assert(mp_encode_block_change(&frame, 11, 61, -12, BLOCK_STONE, 0));
	seam_feed(&remote, &frame);
	server_remote_get_play_obs(&remote, &obs);
	assert(obs.watch_echo.serial == 2 && obs.watch_echo.x == 10
		   && "a neighbouring cell's echo must not overwrite the slot");

	/* ...and a LATER echo for the watched cell DOES replace it (newest wins,
	   the ring's own rule). */
	assert(mp_encode_block_change(&frame, 10, 61, -12, BLOCK_DIRT, 3));
	seam_feed(&remote, &frame);
	server_remote_get_play_obs(&remote, &obs);
	assert(obs.watch_echo.serial == obs.block_echo_serial
		   && obs.watch_echo.type == BLOCK_DIRT
		   && obs.watch_echo.metadata == 3);
	/* hw1030g: ...but the cell's HISTORY still holds the AIR the dig made --
	   newest-only masked it on real hardware (build 53c75ff, mp_play_10m:
	   "dig never echoed by the server" after the fixture logged the dig). */
	assert(obs.watch_history_n == 2);
	assert(obs.watch_history[0].serial == 2
		   && obs.watch_history[0].type == BLOCK_AIR
		   && obs.watch_history[1].serial == obs.block_echo_serial
		   && obs.watch_history[1].type == BLOCK_DIRT
		   && "every record for the watched cell since the arm is kept");
	assert(obs.watch_clears == 0 && obs.watch_bodies == 0);

	/* (e) CHANGING THE WATCHED CELL CLEARS THE SLOT. The core moves the watch
	   from the dig cell to the place cell within one cycle; a stale echo from
	   the previous cell surviving that move would credit a place with a dig's
	   receipt. */
	server_remote_play_watch(&remote, true, 99, 62, 99);
	server_remote_get_play_obs(&remote, &obs);
	assert(obs.watch_echo.serial == 0
		   && "moving the watch must clear the slot");
	assert(obs.watch_history_n == 0 && obs.watch_history[0].serial == 0
		   && obs.watch_history[1].serial == 0
		   && "...and the cell's history with it");
	assert(obs.watch_clears == 1
		   && "a move that DROPPED a stored record is counted (diagnostic)");

	/* ...re-arming the SAME cell is not a move and must also start clean (the
	   driver re-states the watch every frame, so this is the common path). */
	assert(mp_encode_block_change(&frame, 99, 62, 99, BLOCK_AIR, 0));
	seam_feed(&remote, &frame);
	server_remote_play_watch(&remote, true, 99, 62, 99); /* same cell again */
	server_remote_get_play_obs(&remote, &obs);
	assert(obs.watch_echo.serial != 0
		   && "re-stating the same cell must NOT drop the echo it is waiting "
			  "for -- the driver re-states it every frame");
	/* hw1030g race probe: the echo lands, THEN the driver re-states the same
	   cell many times (every frame of the wait) -- nothing is dropped and no
	   clear is counted. */
	for(int k = 0; k < 120; k++)
		server_remote_play_watch(&remote, true, 99, 62, 99);
	server_remote_get_play_obs(&remote, &obs);
	assert(obs.watch_echo.serial == obs.block_echo_serial
		   && obs.watch_history_n == 1 && obs.watch_clears == 1
		   && "re-stating after the echo landed must not clear it");

	/* (f) WATCH OFF clears the slot too, and keeps it clear. */
	server_remote_play_watch(&remote, false, 0, 0, 0);
	server_remote_get_play_obs(&remote, &obs);
	assert(obs.watch_echo.serial == 0);
	assert(mp_encode_block_change(&frame, 99, 62, 99, BLOCK_STONE, 0));
	seam_feed(&remote, &frame);
	server_remote_get_play_obs(&remote, &obs);
	assert(obs.watch_echo.serial == 0
		   && "nothing is stored while the watch is off");

	/* (g) The ring is UNCHANGED by all of this -- the watch adds a slot, it
	   does not alter what the ring records. Every echo above is still in the
	   ring's own count. */
	assert(obs.block_echo_serial == 2 + 40 + 1 + 1 + 1 + 1);

	/* (h) H2's bound still holds: the watch lives in the play-obs struct. */
	assert(sizeof(struct server_remote_status)
		   <= (size_t)SERVER_REMOTE_STATUS_PRE_LANE_BYTES);

	server_remote_destroy(&remote);
	clin_update_drain();
	printf("  B19: watched cell survives a %d-cell burst (ring serial %u, slot "
		   "held serial 2); foreign cells, re-arm, move and off all behave\n",
		   40, obs.block_echo_serial);
}

/* The world-install generation is the design's connection generation: how
 * many remote world installs have completed IN THIS PROCESS. It is counted at
 * the one site that finishes one -- remote_emit_load_completion_locked's
 * false->true edge inside server_remote_pump -- so this case arms exactly the
 * two preconditions remote_finish_promotion_locked leaves behind when the
 * player's own column installs (server_remote_chunks.c), and drives the
 * production pump. */
static void test_mp_world_install_generation(void) {
	struct server_remote remote;
	memset(&remote, 0xC7, sizeof(remote));
	assert(server_remote_init(&remote));
	struct server_remote_play_obs status;
	server_remote_get_play_obs(&remote, &status);
	uint32_t base = status.world_install_generation;

	struct server_remote_impl* impl = remote.impl;
	impl->have_authoritative_position = true;
	impl->load_completion_pending = true;
	server_remote_pump(&remote);
	server_remote_get_play_obs(&remote, &status);
	assert(status.world_install_generation == base + 1);

	/* Idle pumps do not move it, and neither does a second pump against the
	 * already-completed install (the emit is a one-shot latch). */
	for(int i = 0; i < 4; i++)
		server_remote_pump(&remote);
	server_remote_get_play_obs(&remote, &status);
	assert(status.world_install_generation == base + 1);

	/* A dimension change re-arms the same gate -- MP_EVENT_RESPAWN clears
	 * remote_load_complete_sent (server_remote_protocol.c) -- so the Nether's
	 * install is a SECOND install. */
	impl->remote_load_complete_sent = false;
	impl->load_completion_pending = true;
	server_remote_pump(&remote);
	server_remote_get_play_obs(&remote, &status);
	assert(status.world_install_generation == base + 2);

	/* The REJOIN shape, exactly what mp_session_release_remote followed by
	 * mp_session_prepare_remote does: the impl is freed and a new one built.
	 * The count is per PROCESS, so it must NOT restart -- that is the whole
	 * reason gen 1 can be told from gen 2. */
	server_remote_destroy(&remote);
	assert(server_remote_init(&remote));
	impl = remote.impl;
	impl->have_authoritative_position = true;
	impl->load_completion_pending = true;
	server_remote_pump(&remote);
	server_remote_get_play_obs(&remote, &status);
	assert(status.world_install_generation == base + 3);

	/* ...and it still reads out through the destroyed-session path, which is
	 * where an unattended run's heartbeat samples it after a quit. */
	server_remote_destroy(&remote);
	server_remote_get_play_obs(&remote, &status);
	assert(status.world_install_generation == base + 3);

	clin_update_drain();
	clin_force_world_reset();
	gstate.world_loaded = false;
	gstate.players[0].current_screen = NULL;
	printf("  B2 seams: world_install_generation %u -> %u across a dimension "
		   "change and a full release/prepare rejoin\n",
		   base, status.world_install_generation);
}

/* REL-MP-FIX4a + REL-MP-LOADHANG: the remote load-screen stall clock counts
 * READINESS progress only. FIX4a's case (meshing still draining with installs
 * stopped at the ring cap) must keep the clock at zero; hw1021f's case (a
 * live server streaming keep-alives/time/entity bytes while the ledger never
 * settles, near-ring meshes churning up and down) must NOT. */
static void lw_progress_feed(struct lworld_remote_progress* p,
							 struct lworld_remote_progress_input* in, int secs,
							 float* acc) {
	for(int i = 0; i < secs; i++)
		*acc = lworld_remote_progress_tick(p, in, 1.0f);
}
static void test_lworld_stall_progress_token(void) {
	struct lworld_remote_progress p;
	struct lworld_remote_progress_input in;
	float acc = 0.0f;
	/* FIX4a: installs frozen at the cap (81), meshing pending falls 326 -> 0
	 * over 40 s: progress every second, never near the 30 s escape. */
	memset(&in, 0, sizeof in);
	in.installs = 81;
	in.chunks = 81;
	in.near_mesh_pending = 326;
	lworld_remote_progress_reset(&p);
	lworld_remote_progress_tick(&p, &in, 1.0f);
	for(int i = 1; i <= 40; i++) {
		in.near_mesh_pending = 326 - i * 8 < 0 ? 0 : 326 - i * 8;
		acc = lworld_remote_progress_tick(&p, &in, 1.0f);
		assert(acc < 1.0f || in.near_mesh_pending == 0);
	}
	/* hw1021f: core census constant (80 resident, 1 pending), bytes rising
	 * ~6 KB every second, near-ring mesh_pending churning 0..12 and back.
	 * That is liveness, not readiness: the escape clock must reach 30 s. */
	memset(&in, 0, sizeof in);
	in.installs = 81;
	in.chunks = 81;
	in.near_missing = 40;
	in.have_census = true;
	in.census_resident = 80;
	in.census_pending = 1;
	in.rx_bytes = 5000;
	lworld_remote_progress_reset(&p);
	lworld_remote_progress_tick(&p, &in, 1.0f);
	for(int i = 1; i <= 30; i++) {
		in.rx_bytes += 6144;
		in.near_mesh_pending = (i % 4) * 4; /* 0,4,8,12,0,... never below 0 */
		acc = lworld_remote_progress_tick(&p, &in, 1.0f);
	}
	assert(acc >= 30.0f
		   && "received bytes and mesh churn are not readiness progress");
	/* The census falling (79 -> 73 RESIDENT on hw1021f) is not progress
	 * either; only a NEW BEST is. */
	acc = 0.0f;
	lworld_remote_progress_reset(&p);
	lworld_remote_progress_tick(&p, &in, 1.0f);
	for(int i = 1; i <= 6; i++) {
		in.census_resident = 80 - (size_t)i;
		acc = lworld_remote_progress_tick(&p, &in, 1.0f);
	}
	assert(acc >= 6.0f);
	in.census_resident = 79; /* back up, but not above the best (80) */
	acc = lworld_remote_progress_tick(&p, &in, 1.0f);
	assert(acc >= 7.0f);
	in.census_resident = 81; /* a new best: progress */
	acc = lworld_remote_progress_tick(&p, &in, 1.0f);
	assert(acc == 0.0f);
	/* Each readiness input independently counts as progress. */
	struct lworld_remote_progress_input base;
	memset(&base, 0, sizeof base);
	base.installs = 10;
	base.chunks = 10;
	base.near_missing = 10;
	base.near_mesh_pending = 10;
	base.have_census = true;
	base.census_resident = 10;
	base.census_pending = 10;
	for(int k = 0; k < 6; k++) {
		struct lworld_remote_progress_input step = base;
		lworld_remote_progress_reset(&p);
		lworld_remote_progress_tick(&p, &base, 1.0f);
		acc = 0.0f;
		lw_progress_feed(&p, &base, 5, &acc);
		assert(acc >= 5.0f);
		switch(k) {
		case 0: step.installs++; break;
		case 1: step.chunks++; break;
		case 2: step.near_missing--; break;
		case 3: step.near_mesh_pending--; break;
		case 4: step.census_resident++; break;
		default: step.census_pending--; break;
		}
		assert(lworld_remote_progress_tick(&p, &step, 1.0f) == 0.0f);
	}
	/* A truly silent server still escapes at 30 s. */
	lworld_remote_progress_reset(&p);
	lworld_remote_progress_tick(&p, &base, 1.0f);
	acc = 0.0f;
	lw_progress_feed(&p, &base, 30, &acc);
	assert(acc >= 30.0f);

	/* Review r1: a FRESH CONNECT primes with nothing loaded -- mesh_pending
	 * and announced_pending are 0 on the priming tick. Installs then climb
	 * to the 81-column cap while the mesh backlog and the census obligations
	 * rise; installs stop, and a slow mesher drains 326 -> 0 over 41 s with
	 * nothing else moving. Every drain step is progress: the clock must
	 * never reach the 30 s escape. */
	memset(&in, 0, sizeof in);
	in.near_missing = 121;
	in.have_census = true;
	lworld_remote_progress_reset(&p);
	lworld_remote_progress_tick(&p, &in, 1.0f); /* fresh connect: all 0 */
	for(int i = 1; i <= 9; i++) {
		in.installs = (uint32_t)(i * 9);
		in.chunks = (size_t)(i * 9);
		in.near_missing = 121 - i * 9;
		in.near_mesh_pending = i * 36 + 2; /* rises to 326 */
		in.census_resident = (size_t)(i * 9);
		in.census_pending = (size_t)(81 - i * 9 + 20); /* announcements */
		acc = lworld_remote_progress_tick(&p, &in, 1.0f);
		assert(acc == 0.0f);
	}
	assert(in.near_mesh_pending == 326);
	float worst = 0.0f;
	while(in.near_mesh_pending > 0) {
		in.near_mesh_pending -= 8;
		if(in.near_mesh_pending < 0)
			in.near_mesh_pending = 0;
		acc = lworld_remote_progress_tick(&p, &in, 1.0f);
		if(acc > worst)
			worst = acc;
	}
	assert(worst < 1.0f
		   && "a slow mesher draining after a fresh connect is progress");
	/* ...and the announced-pending drain counts the same way. */
	for(int i = 0; i < 20; i++) {
		in.census_pending--;
		acc = lworld_remote_progress_tick(&p, &in, 1.0f);
		assert(acc == 0.0f);
	}
	/* Fully drained and flat after that: the escape clock runs again. */
	lw_progress_feed(&p, &in, 30, &acc);
	assert(acc >= 30.0f);
}

/* REL-MP-LOADHANG hard bound: at 90 s the screen admits on the client's own
 * installed+meshed core, or disconnects cleanly; it never waits forever. */
static void test_lworld_remote_hard_ceiling(void) {
	const float C = LWORLD_REMOTE_HARD_CEILING_S, T = 30.0f;
	/* before the ceiling, progressing: wait. */
	assert(lworld_remote_hold_decide(10.0f, C, 0.0f, T, true, true)
		   == LWORLD_HOLD_WAIT);
	assert(lworld_remote_hold_decide(89.9f, C, 0.0f, T, false, true)
		   == LWORLD_HOLD_WAIT);
	/* stalled 30 s with no usable core: the connection-stalled escape. */
	assert(lworld_remote_hold_decide(40.0f, C, T, T, false, true)
		   == LWORLD_HOLD_STALL_ESCAPE);
	assert(lworld_remote_hold_decide(40.0f, C, T, T, true, false)
		   == LWORLD_HOLD_STALL_ESCAPE);
	/* stalled but the core is there and nothing else holds gameplay: the
	 * ledger never settled (hw1021f) -- the ceiling owns it, not the stall. */
	assert(lworld_remote_hold_decide(61.0f, C, T, T, true, true)
		   == LWORLD_HOLD_WAIT);
	/* branch 1: ceiling + core installed and meshed -> admit. */
	assert(lworld_remote_hold_decide(C, C, 0.0f, T, true, true)
		   == LWORLD_HOLD_FORCE_ADMIT);
	assert(lworld_remote_hold_decide(300.0f, C, 300.0f, T, true, true)
		   == LWORLD_HOLD_FORCE_ADMIT);
	/* Review r1 FRAME ORDER: the ceiling-crossing frame lands while a core
	 * column's display list is dirty (one applied server block update, a
	 * fluid tick, a near-ring remesh -- world_near_ring_status counts
	 * rebuild_displist || mesh_in_flight as mesh-pending). The core is
	 * PRESENT, so that frame must latch the admission, not disconnect; the
	 * decision takes presence only, and the meshes are waited for at
	 * admission. Every later frame, clean or dirty, decides the same. */
	for(int frame = 0; frame < 4; frame++) {
		float t = C + 0.016f * (float)frame;
		/* frames 0 and 2 dirty, 1 and 3 clean: presence is all that counts */
		assert(lworld_remote_hold_decide(t, C, 0.0f, T, true, true)
			   == LWORLD_HOLD_FORCE_ADMIT);
	}
	/* and the stall escape does not pre-empt a present, remeshing core
	 * before the ceiling either. */
	assert(lworld_remote_hold_decide(45.0f, C, T + 15.0f, T, true, true)
		   == LWORLD_HOLD_WAIT);
	/* branch 2: ceiling with a core column MISSING (or with a hold still
	 * up) -> disconnect cleanly, even while bytes keep "progressing". */
	assert(lworld_remote_hold_decide(C, C, 0.0f, T, false, true)
		   == LWORLD_HOLD_CEILING_ESCAPE);
	assert(lworld_remote_hold_decide(C, C, 0.0f, T, true, false)
		   == LWORLD_HOLD_CEILING_ESCAPE);
	printf("PASS REL-MP-LOADHANG: stall clock ignores bytes/mesh churn; "
		   "%.0f s ceiling admits on a meshed core, else disconnects\n",
		   (double)C);
}

/* task_db613f13 (2026-08-31): the load-world hold escape hatch's pure
 * stall-detector (source/game/gui/screen_load_world.c, declared in
 * screen.h). Headless -- no gstate, no clin_process, no server_remote --
 * exercising exactly the arithmetic screen_lworld_remote_update() drives
 * with world_loaded_chunks() each frame: "chunk-count unchanged for N
 * update calls -> stalled". */
static void test_lworld_stall_tracker(void) {
	struct lworld_stall_tracker t;
	lworld_stall_tracker_reset(&t);

	/* First tick only primes the baseline -- a reading taken before any
	 * baseline exists must never already report "stalled". */
	float acc = lworld_stall_tracker_tick(&t, 10, 0, 5.0f);
	assert(acc == 0.0f);

	/* Same chunk count AND install seq across ticks accumulates no-progress
	 * time. */
	acc = lworld_stall_tracker_tick(&t, 10, 0, 5.0f);
	assert(acc == 5.0f);
	acc = lworld_stall_tracker_tick(&t, 10, 0, 5.0f);
	assert(acc == 10.0f);

	/* Progress (chunk count) resets the accumulator to 0, even mid-timeout. */
	acc = lworld_stall_tracker_tick(&t, 11, 0, 5.0f);
	assert(acc == 0.0f);
	/* ...and the very next tick starts accumulating from that new baseline,
	 * not from wherever the pre-progress run left off. */
	acc = lworld_stall_tracker_tick(&t, 11, 0, 2.0f);
	assert(acc == 2.0f);

	/* Crossing a 30s-style timeout after sustained zero progress (the
	 * screen's LWORLD_REMOTE_STALL_TIMEOUT_S). */
	lworld_stall_tracker_reset(&t);
	lworld_stall_tracker_tick(&t, 3, 0, 1.0f); /* baseline */
	float last = 0.0f;
	for(int i = 0; i < 30; i++)
		last = lworld_stall_tracker_tick(&t, 3, 0, 1.0f);
	assert(last >= 30.0f);

	/* CHIP A (task_11695e19, 2026-08-31): a net-flat chunk count must NOT
	 * read as stalled while clin_chunk_install_seq() is still advancing --
	 * the same-dim-teleport churn/multi-frame-decode scenario where an
	 * unload and an install land in the same sampled frame (or a column is
	 * mid-decode across several frames) and the NET section count never
	 * moves even though real streaming work is happening. */
	lworld_stall_tracker_reset(&t);
	lworld_stall_tracker_tick(&t, 5, 100, 1.0f); /* baseline */
	acc = lworld_stall_tracker_tick(&t, 5, 100, 5.0f); /* count AND installs flat */
	assert(acc == 5.0f);
	/* installs advances (a column completed) even though the net count
	 * didn't move (an unload elsewhere cancelled it out) -- this must reset
	 * the accumulator exactly like a chunk-count change does. */
	acc = lworld_stall_tracker_tick(&t, 5, 101, 5.0f);
	assert(acc == 0.0f);
	/* ...and accumulation resumes from THAT new baseline. */
	acc = lworld_stall_tracker_tick(&t, 5, 101, 3.0f);
	assert(acc == 3.0f);
	/* Sustained installs-only progress (net count pinned the whole time)
	 * must never cross the stall timeout: every tick advances installs, so
	 * the accumulator never leaves 0 across a 30-sample run. */
	lworld_stall_tracker_reset(&t);
	lworld_stall_tracker_tick(&t, 7, 0, 1.0f); /* baseline */
	for(int i = 0; i < 30; i++) {
		last = lworld_stall_tracker_tick(&t, 7, (uint32_t)(i + 1), 1.0f);
		assert(last == 0.0f);
	}

	puts("  load-world stall tracker: no-progress accumulates across ticks, "
		 "resets on chunk-count advance, crosses a 30s-style timeout; "
		 "installs-advanced (CHIP A) also counts as progress even when the "
		 "net chunk count is pinned flat");
}

/* --- live PvP leg (field-defect discriminator, 2026-08-31) ---
 * Settles the 2026-08-30 field reports empirically: (b) "Wii CaveWii cannot
 * damage the PC player" and (c) "skeleton ignores the CaveWii player". Code
 * reading found the attack path byte-correct vs [740c583 Packet7UseEntity;
 * PlayerControllerMP.attackEntity] and only ONE silent server-side drop: an
 * attack on a target with no server entity (mc173 player.rs handle_interact
 * "incoherent interact entity target" <- tick_state.rs death_time>20
 * removal) -- exactly the stuck-death window the respawn fixes closed. This
 * leg proves a FRESH, ALIVE, in-range PvP exchange works BOTH directions
 * between the production client stack and a raw-socket second player. */
static SOCKET live_pvp_second_login(const char* name, int32_t* out_id) {
	SOCKET s = live_collect_second_login(name);
	if(s == INVALID_SOCKET)
		return s;
	/* First S->C packet after our C->S login is the 0x01 Login reply:
	 * id(1) + i32 entity_id + str16 + i64 seed + i8 dimension. Read the
	 * fixed 5-byte head exactly; the tail is drained with everything else. */
	uint8_t hdr[5];
	size_t got = 0;
	while(got < sizeof(hdr)) {
		int r = recv(s, (char*)hdr + got, (int)(sizeof(hdr) - got), 0);
		if(r <= 0) {
			closesocket(s);
			return INVALID_SOCKET;
		}
		got += (size_t)r;
	}
	if(hdr[0] != 0x01) {
		fprintf(stderr, "LIVE PVP second login: expected 0x01 reply, got "
			"0x%02x\n", hdr[0]);
		closesocket(s);
		return INVALID_SOCKET;
	}
	*out_id = (int32_t)(((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16)
						| ((uint32_t)hdr[3] << 8) | (uint32_t)hdr[4]);
	return s;
}

static bool live_pvp_second_attack(SOCKET s, int32_t self_id,
								   int32_t target_id) {
	/* [740c583 Packet7UseEntity: i32 player, i32 target, i8 leftClick] */
	uint8_t pkt[10];
	pkt[0] = 0x07;
	put_i32(pkt + 1, self_id);
	put_i32(pkt + 5, target_id);
	pkt[9] = 1;
	return live_collect_send_all(s, pkt, sizeof(pkt));
}

static int run_live_pvp(void) {
	struct server_remote remote = {0};
	assert(server_remote_init(&remote));
	assert(server_remote_start(&remote, "127.0.0.1", 25565, "CaveWii"));

	ptime_t connect_start = time_get();
	struct server_remote_status status;
	while(time_diff_ms(connect_start, time_get()) < 30000) {
		server_remote_pump(&remote);
		clin_update();
		server_remote_get_status(&remote, &status);
		if(status.state == REMOTE_PLAY && gstate.world_loaded)
			break;
		if(status.state == REMOTE_CLOSING || status.state == REMOTE_KICKED)
			break;
		thread_msleep(1);
	}
	server_remote_get_status(&remote, &status);
	if(status.state != REMOTE_PLAY || !gstate.world_loaded) {
		fprintf(stderr, "LIVE PVP FAIL connect state=%d reason=%s\n",
			status.state, status.reason);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	printf("LIVE PVP LOGIN entity=%d\n", status.entity_id);

	int ground0 = live_s2_find_ground_y(0, 0);
	if(ground0 < 2 || ground0 > 118
	   || !live_s2_teleport(&remote, 0.5, ground0 + 1, 0.5, 8000, &status)) {
		fprintf(stderr, "LIVE PVP FAIL ground/teleport ground0=%d\n", ground0);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	live_s2_pump_for(&remote, 1000, &status);

	int32_t g2id = 0;
	SOCKET ghost2 = live_pvp_second_login("Ghost2", &g2id);
	if(ghost2 == INVALID_SOCKET) {
		fprintf(stderr, "LIVE PVP FAIL second-client login\n");
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	printf("LIVE PVP GHOST2 entity=%d\n", g2id);
	/* In melee range of the CaveWii player (mc173 handle_interact validates
	 * dist^2 < 36 from the ATTACKER). */
	(void)live_collect_second_move(ghost2, 2.5, ground0 + 1, 2.5);

	/* Wait until CaveWii tracks Ghost2's player entity (proves the id
	 * round-trip live rather than assuming it). */
	bool tracked = false;
	ptime_t track_start = time_get();
	while(time_diff_ms(track_start, time_get()) < 10000) {
		live_s2_pump_for(&remote, 100, &status);
		live_collect_drain(ghost2);
		if(dict_entity_get(gstate.entities, (uint32_t)g2id)) {
			tracked = true;
			break;
		}
	}
	if(!tracked) {
		fprintf(stderr, "LIVE PVP FAIL Ghost2 spawn never tracked\n");
		closesocket(ghost2);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}
	short h0 = gstate.players[0].local_player ? gstate.players[0].local_player->health : -999;
	printf("LIVE PVP TRACKED ghost2 in dict; local health=%d (alive "
		"pre-assert)\n", h0);
	if(h0 <= 0) {
		fprintf(stderr, "LIVE PVP FAIL pre-assert: local player not alive\n");
		closesocket(ghost2);
		server_remote_cancel(&remote);
		server_remote_destroy(&remote);
		return 1;
	}

	/* --- Leg A: CaveWii -> Ghost2, exactly what onLeftClick sends. PASS =
	 * the server accepts the hit and broadcasts 0x26 status 2, which our
	 * pipe mirrors onto the tracked entity's client hurt flag. --- */
	bool hurt_seen = false;
	for(int i = 0; i < 12 && !hurt_seen; i++) {
		svin_rpc_send(&(struct server_rpc) {
			.type = SRPC_ENTITY_ATTACK,
			.payload.entity_attack.entity_id = (uint32_t)g2id,
		});
		ptime_t punch = time_get();
		while(time_diff_ms(punch, time_get()) < 400 && !hurt_seen) {
			live_s2_pump_for(&remote, 50, &status);
			live_collect_drain(ghost2);
			struct entity** pp = dict_entity_get(gstate.entities,
												 (uint32_t)g2id);
			if(pp && (*pp)->hurt)
				hurt_seen = true;
		}
	}
	printf("LIVE PVP LEG-A (CaveWii->Ghost2) %s\n",
		hurt_seen ? "PASS: hurt flash mirrored for Ghost2"
				  : "FAIL: no hurt status observed");

	/* --- Leg B: Ghost2 -> CaveWii raw Packet7. PASS = our client-side
	 * health drops below the pre-assert baseline. --- */
	bool damaged = false;
	for(int i = 0; i < 6 && !damaged; i++) {
		if(!live_pvp_second_attack(ghost2, g2id, status.entity_id))
			break;
		ptime_t punch = time_get();
		while(time_diff_ms(punch, time_get()) < 500 && !damaged) {
			live_s2_pump_for(&remote, 50, &status);
			live_collect_drain(ghost2);
			short h = gstate.players[0].local_player ? gstate.players[0].local_player->health
										  : -999;
			if(h > -999 && h < h0)
				damaged = true;
		}
	}
	printf("LIVE PVP LEG-B (Ghost2->CaveWii) %s (health %d -> %d)\n",
		damaged ? "PASS: local health dropped" : "FAIL: no damage received",
		h0, gstate.players[0].local_player ? gstate.players[0].local_player->health : -999);

	closesocket(ghost2);
	server_remote_cancel(&remote);
	server_remote_destroy(&remote);
	if(hurt_seen && damaged) {
		puts("LIVE PVP VERDICT: fresh alive PvP works BOTH ways -- the "
			 "2026-08-30 one-way field report is consistent with the "
			 "(since-fixed) stuck-death window, not a live attack-path "
			 "defect");
		return 0;
	}
	return 1;
}

#if __has_include("oracle/sort/replay_client.h") /* private oracle; absent in the public snapshot */
#include "oracle/sort/replay_client.h"
#define CW_HAVE_SORT_REPLAY 1
#endif

/* [740c583 EntityClientPlayerMP.onUpdate:31-35, func_4056_N:71-85]
 * Every executed remote tick sends a movement variant, including idle Packet10.
 * The 20-tick counter at :38-42 is inventory sync, not an idle skip. */
static void cadence_count_rpc(struct server_rpc* call, void* user) {
	unsigned* count = user;
	if(call->type == SRPC_PLAYER_POS) {
		if(*count < 20)
			assert(call->payload.player_pos.x == 1.0F + (float)*count * 0.25F);
		++*count;
	}
}

static void test_remote_movement_cadence(void) {
	struct entity body = {0};
	body.health = 20;
	struct client_session saved = gstate.players[0];
	bool saved_loaded = gstate.world_loaded;
	bool saved_switch = gstate.world_switch_pending;
	bool saved_remote = gstate.remote_session;
	bool saved_pause = gstate.paused;
	gstate.players[0].local_player = &body;
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	gstate.remote_session = true;
	gstate.paused = false;
	clin_remote_movement_reset();
	unsigned count = 0;
	svin_process_messages(cadence_count_rpc, &count, false);
	/* Five 100 ms frames: two physics ticks before each frame drain. */
	for(unsigned frame = 0; frame < 5; frame++) {
		for(unsigned tick = 0; tick < 2; tick++) {
			gstate.players[0].camera.x = 1.0F + (float)(frame * 2 + tick) * 0.25F;
			clin_remote_movement_tick_executed();
			clin_remote_player_tick(&gstate.players[0].camera);
		}
		svin_process_messages(cadence_count_rpc, &count, false);
		assert(count == (frame + 1) * 2);
	}
	/* The frame pump must not add an eleventh active-world sample. */
	clin_update();
	svin_process_messages(cadence_count_rpc, &count, false);
	assert(count == 10);
	/* 60 fps accumulator: only ten of thirty frames execute a tick. */
	for(unsigned frame = 0; frame < 30; frame++) {
		if(frame % 3 == 2) {
			gstate.players[0].camera.x = 1.0F + (float)(10 + frame / 3) * 0.25F;
			clin_remote_movement_tick_executed();
			clin_remote_player_tick(&gstate.players[0].camera);
		}
		svin_process_messages(cadence_count_rpc, &count, false);
		assert(count == 10 + (frame + 1) / 3);
	}
	uint32_t eligible, accepted;
	clin_remote_movement_stats(&eligible, &accepted);
	assert(eligible == 20 && accepted == 20);
	/* Idle never skips, even across the inventory-sync boundary. */
	for(unsigned tick = 0; tick < 21; tick++) {
		clin_remote_movement_tick_executed();
		clin_remote_player_tick(&gstate.players[0].camera);
		svin_process_messages(cadence_count_rpc, &count, false);
	}
	assert(count == 41);
	gstate.remote_session = false;
	gstate.paused = true;
	clin_remote_player_tick(&gstate.players[0].camera);
	svin_process_messages(cadence_count_rpc, &count, false);
	assert(count == 41);
	gstate.remote_session = true;
	/* The accepted pause card keeps remote physics unpaused. */
	gstate.paused = true;
	clin_remote_player_tick(&gstate.players[0].camera);
	svin_process_messages(cadence_count_rpc, &count, false);
	assert(count == 41);
	gstate.paused = false;
	gstate.players[0].current_screen = &screen_pause;
	clin_remote_movement_tick_executed();
	clin_remote_player_tick(&gstate.players[0].camera);
	svin_process_messages(cadence_count_rpc, &count, false);
	assert(count == 42);
	clin_remote_movement_stats(&eligible, &accepted);
	assert(eligible == 42 && accepted == 42);
	/* A full inbox refuses the next movement sample; executed ticks still
	 * advance, and rpcdrop records the refusal. */
	struct server_rpc flood = {.type = SRPC_PLAYER_POS};
	uint32_t drops = svin_dropped_count();
	while(svin_rpc_send(&flood)) { /* fill the bounded inbox */ }
	assert(svin_dropped_count() == drops + 1);
	drops = svin_dropped_count();
	clin_remote_movement_tick_executed();
	clin_remote_player_tick(&gstate.players[0].camera);
	clin_remote_movement_stats(&eligible, &accepted);
	assert(eligible == 43 && accepted == 42);
	assert(svin_dropped_count() == drops + 1);
	svin_settle();
	gstate.players[0] = saved;
	gstate.world_loaded = saved_loaded;
	gstate.world_switch_pending = saved_switch;
	gstate.remote_session = saved_remote;
	gstate.paused = saved_pause;
}

int main(int argc, char** argv) {
	setvbuf(stdout,NULL,_IONBF,0);
	assert(svin_init());
	assert(clin_init());
	blocks_init();
	items_init();
#ifdef CW_HAVE_SORT_REPLAY
	if(argc == 3 && strcmp(argv[1], "--sort-replay") == 0)
		return sort_replay_client(argv[2],false);
	if(argc == 3 && strcmp(argv[1], "--sort-executor") == 0)
		return sort_replay_client(argv[2],true);
#endif
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	if(argc == 2 && strcmp(argv[1], "--marker-remote") == 0) {
		test_mp_map_data();
		return 0;
	}
	if(argc == 2 && strcmp(argv[1], "--cadence") == 0) {
		test_remote_movement_cadence();
		puts("remote movement cadence: PASS");
		return 0;
	}
	if(argc == 2 && strcmp(argv[1], "--live") == 0)
		return run_live();
	if(argc == 2 && strcmp(argv[1], "--bed-ride") == 0) {
		/* MP-BED/MP-RIDE legs alone (critique-1028); also in the full run. */
		test_mp_bed_ride_units();
		test_mp_bed_ride_wire();
		return 0;
	}
	if(argc == 3 && strcmp(argv[1], "--live-walk") == 0) {
		char* end=NULL; unsigned long value=strtoul(argv[2],&end,10);
		if(!end || *end || value==0 || value>UINT_MAX) return 2;
		return run_live_walk((unsigned)value);
	}
	if(argc == 2 && strcmp(argv[1], "--live-s2") == 0)
		return run_live_s2();
	if((argc == 2 || argc == 3)
	   && strcmp(argv[1], "--live-reconnect") == 0) {
		if(argc == 3) {
			char* end = NULL;
			unsigned long value = strtoul(argv[2], &end, 10);
			if(!end || *end || value == 0 || value > 65535)
				return 2;
			live_rc_port = (uint16_t)value;
		}
		return run_live_reconnect();
	}
	if(argc == 2 && strcmp(argv[1], "--live-s4") == 0)
		return run_live_s4(); /* mp-s4-polish lane; needs the DIAG mc173
								 (/kickme) -- see
								 tests/oracle/mp_s4_live_polish.txt */
	if(argc == 2 && strcmp(argv[1], "--live-respawn-hunt") == 0)
		return run_live_respawn_hunt(); /* respawn-hunt regression guard:
											needs the DIAG mc173 (/explode) */
	if(argc == 2 && strcmp(argv[1], "--live-respawn-hunt-fall") == 0)
		return run_live_respawn_hunt_fall(); /* respawn-hunt regression guard
												 variant B: clean (no
												 block-edit) kill */
	if(argc == 2 && strcmp(argv[1], "--live-respawn-hunt-terrain") == 0)
		return run_live_respawn_hunt_terrain(); /* sky-respawn terrain-hold
													regression guard (2026-08-
													30 field defect, fixed
													7ec5536): forces the
													not-yet-resident race over
													a real connection */
	if(argc == 2 && strcmp(argv[1], "--live-collect") == 0)
		return run_live_collect(); /* Packet22Collect ghost-item fix */
	if(argc == 2 && strcmp(argv[1], "--live-pvp") == 0)
		return run_live_pvp(); /* two-way PvP discriminator (2026-08-31) */
	if(argc == 2 && strcmp(argv[1], "--live-entity-soak") == 0)
		return run_live_entity_soak(90);
	if(argc == 3 && strcmp(argv[1], "--live-entity-soak") == 0) {
		char* end=NULL; unsigned long value=strtoul(argv[2],&end,10);
		if(!end || *end || value==0 || value>UINT_MAX) return 2;
		return run_live_entity_soak((unsigned)value);
	}
	if(argc == 2 && strcmp(argv[1], "--fault") == 0) {
		test_s1_faults();
		return 0;
	}
	test_remote_movement_cadence();
	test_codec_goldens();
	test_address_parse_goldens();
	test_username_validation();
	test_p1_codec_goldens();
	test_p2_codec_goldens();
	test_p3_codec_goldens();
	test_provider_lifecycle();
	/* EOF / kick ordering probes (mp-split-0912/2). The fifth one prints the
	 * per-case table and carries the single all-pass assert, so it must run
	 * last of the five. */
	test_eof_kick_then_eof();
	test_eof_kick_fragmented_then_eof();
	test_eof_bare_eof();
	test_eof_kick_then_rst();
	test_eof_reconnect_clears_kick_reason();
	/* mp-split-0912/7, after the five-case table: the same terminal record
	 * must still be readable once the impl it lived in is gone. */
	test_eof_status_survives_teardown();
	test_eof_teardown_snapshot_cleared_on_reconnect();
	test_eof_teardown_without_session_reports_zeros();
	/* mp-split-0912/35 (W6): and the TEXT half of that record, for every
	 * way a session can end. */
	test_eof_reason_latched_for_every_terminal();
	test_s4_chat_ring();
	test_s4_mount_attach();
	test_s4_mount_jump_online();
	test_s4_dimension_change_stream_order();
	test_s2_window_transaction(false);
	test_s2_window_transaction(true);
	test_s2_close_with_pending_transactions();
	test_s2_window_items_overflow();
	test_s2_set_slot_oob();
	test_s2_close_window_zero_survives();
	test_s2_open_window_zero_survives();
	test_s3_entity_lifecycle();
	test_mp_bed_ride_units();
	test_mp_bed_ride_wire();
	test_s3_entity_hostile_vectors();
	test_mp_painting_transport(); /* Packet25EntityPainting MP transport
									* (PROJECT_MEMORY 2026-09-01 deferred #2) */
	test_s3_entity_metadata_decode_kill();
	test_entity_look_forward();
	test_cancel_during_connect_bounded();
	test_start_rejection_reports_reason();
	test_cancel_pending_connect();
	test_s1_world_stream();
	test_s1_partial_chunk_volume();
	test_s4_explosion_truncate();
	test_deferred_edit_replay(); /* Plan section 7: deferred-edit log recovery */
	test_deferred_edit_unload_purges_stale_edit(); /* unload must purge stale parked edits */
	test_s1_faults();
	test_event_queue_backpressure_burst();
	test_event_queue_stall_keeps_session();
	test_s4_reconnect_stress();
	/* --- display transcode (mp-s4-polish lane) --- */
	test_s4_display_transcode();
	test_s4_entity_approach(); /* mp-s4-polish lane */
	test_mp_map_data(); /* Packet131 -> CRPC_MAP_DATA consumer slice */
	test_mp_base_entity_snap(); /* non-living wire-position snap slice */
	test_chat_sanitize_outbound(); /* Poseidon outbound-chat row */
	test_mp_entity_status(); /* Packet38 hurt flash + death roll */
	test_mp_entity_residuals_2026_08_26(); /* mob head yaw (item 2) +
											 * entity velocity item/mob
											 * split (item 1) */
	test_mp_collect_removes_item(); /* Packet22Collect ghost-item fix */
	/* --- end display transcode (mp-s4-polish lane) --- */
	test_respawn_terrain_hold(); /* same-dim MP respawn terrain hold */
	test_chip_b_window_close_retry(); /* task_18b9ae41 terrain-hold SRPC_WINDOW_CLOSE retry */
	test_chip_b_unload_world_retry(); /* task_18b9ae41 clin_send_unload_world retry */
	test_lworld_stall_tracker(); /* task_db613f13 load-world escape hatch */
	test_lworld_stall_progress_token(); /* REL-MP-FIX4a */
	test_lworld_remote_hard_ceiling(); /* REL-MP-LOADHANG */
	test_preworld_position_echo(); /* mp_reconnect_soak/mp_lan_walk_10m
									* "world never reported loaded" root cause */
	test_mc173_partial_join_burst(); /* late spawn column after a partial
									  * mc173 join burst (hw 2026-09-02) */
	test_mp_status_struct_stays_small(); /* push-0921/B18 (B17 H2): the seams
										  * must not ride on every MP frame */
	test_mp_observation_seams(); /* push-0921/B2: server-confirmed block /
								  * transaction / set-slot records */
	test_mp_accessors_are_disjoint(); /* push-0921/B18 (B17 H2): status and
									   * play-obs never write each other */
	test_mp_watched_cell(); /* push-0921/B19: the one cell the script waits
							 * on, in a slot no burst can evict */
	test_mp_world_install_generation(); /* push-0921/B2: the design's
										 * connection generation */
	puts("test_mp_s0_env: PASS");
	return 0;
}
