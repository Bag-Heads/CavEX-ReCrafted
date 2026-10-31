/*
	Multiplayer setup screen (screen_multiplayer_setup.c): the PURE, gstate/
	disk-free logic only -- config.c's config_read_server_list/
	config_write_server_list (migration + the config round-trip) against
	temp `struct config` objects built directly with parson, the screen's
	own CAVEWII_CLIENT_INTERFACE_TEST array-mutation hooks (add/edit/delete
	+ username validation) which operate on the screen's file-scope
	`servers[]` array only, and (v3, 2026-08-31 later-Minecraft-style
	upgrade) the pure 0xFF/string16 live-ping response parser + the
	latency->signal-bar mapping, both hand-fed a wire buffer built by this
	file's own build_ping_frame() -- no live socket anywhere in this test.
	None of it ever touches gstate.config_user or writes "config.json" --
	see the maintainer instruction this test honors (screen_multiplayer_
	setup.c's own header comment + config.h's config_read_server_list doc
	comment).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "game/gui/screen.h"
#include "parson/parson.h"

static struct config make_config(const char* json_text) {
	struct config c;
	c.root = json_parse_string(json_text);
	assert(c.root);
	c.ephemeral_key_count = 0;
	return c;
}

/* ---- config.c: config_read_server_list / config_write_server_list ------ */

static void test_migration_from_legacy_scalar(void) {
	struct config c = make_config("{\"mp\":{\"server\":\"host1:2\"}}");

	struct config_mp_server out[4];
	size_t count = 0;
	assert(config_read_server_list(&c, out, 4, &count));
	assert(count == 1);
	assert(strcmp(out[0].name, "Default") == 0);
	assert(strcmp(out[0].address, "host1:2") == 0);

	/* Self-healed into the live tree: mp.servers now exists. */
	JSON_Array* arr = json_object_dotget_array(json_object(c.root), "mp.servers");
	assert(arr && json_array_get_count(arr) == 1);

	/* The array is now authoritative -- mutating the legacy scalar must NOT
	 * change what a second read returns. */
	json_object_dotset_string(json_object(c.root), "mp.server", "changed:9");
	struct config_mp_server out2[4];
	size_t count2 = 0;
	assert(config_read_server_list(&c, out2, 4, &count2));
	assert(count2 == 1);
	assert(strcmp(out2[0].address, "host1:2") == 0);

	config_destroy(&c);
	puts("  migration: legacy mp.server -> servers[0] \"Default\", self-heals, then wins over the scalar");
}

static void test_no_migration_when_nothing_to_migrate(void) {
	struct config c = make_config("{}");
	struct config_mp_server out[4];
	size_t count = 99;
	assert(config_read_server_list(&c, out, 4, &count));
	assert(count == 0);
	assert(!json_object_dotget_array(json_object(c.root), "mp.servers"));
	config_destroy(&c);
	puts("  migration: absent mp.server -> empty list, no tree mutation");
}

static void test_existing_array_wins_over_legacy_scalar(void) {
	struct config c = make_config(
		"{\"mp\":{\"server\":\"1.2.3.4:1\",\"servers\":"
		"[{\"name\":\"Keep\",\"address\":\"5.6.7.8:2\"}]}}");
	struct config_mp_server out[4];
	size_t count = 0;
	assert(config_read_server_list(&c, out, 4, &count));
	assert(count == 1);
	assert(strcmp(out[0].name, "Keep") == 0);
	assert(strcmp(out[0].address, "5.6.7.8:2") == 0);
	config_destroy(&c);
	puts("  migration: an existing mp.servers array is never overwritten by the legacy scalar");
}

static void test_round_trip_and_scalars_untouched(void) {
	struct config c
		= make_config("{\"mp\":{\"server\":\"keepme:1\",\"username\":\"Keeper\"}}");

	struct config_mp_server list[3] = {
		{.name = "Alpha", .address = "a.example.com:1"},
		{.name = "Beta Server", .address = "b.example.com:25565"},
		{.name = "Gamma", .address = "192.0.2.5:25566"},
	};
	assert(config_write_server_list(&c, list, 3));

	struct config_mp_server out[8];
	size_t count = 0;
	assert(config_read_server_list(&c, out, 8, &count));
	assert(count == 3);
	for(size_t i = 0; i < 3; i++) {
		assert(strcmp(out[i].name, list[i].name) == 0);
		assert(strcmp(out[i].address, list[i].address) == 0);
	}

	/* config_write_server_list must not touch the separate active-target
	 * scalars (config.h's documented contract). */
	assert(strcmp(config_read_string(&c, "mp.server", ""), "keepme:1") == 0);
	assert(strcmp(config_read_string(&c, "mp.username", ""), "Keeper") == 0);

	config_destroy(&c);
	puts("  round trip: write -> read reproduces every entry; mp.server/mp.username untouched");
}

static void test_read_cap_enforced(void) {
	struct config c = make_config("{}");
	struct config_mp_server list[5];
	char nbuf[32], abuf[48];
	for(size_t i = 0; i < 5; i++) {
		snprintf(nbuf, sizeof(nbuf), "Srv%zu", i);
		snprintf(abuf, sizeof(abuf), "host%zu.example.com:1", i);
		snprintf(list[i].name, sizeof(list[i].name), "%s", nbuf);
		snprintf(list[i].address, sizeof(list[i].address), "%s", abuf);
	}
	assert(config_write_server_list(&c, list, 5));

	struct config_mp_server out[3];
	size_t count = 99;
	assert(config_read_server_list(&c, out, 3, &count));
	assert(count == 3); /* clamped to the caller's cap, not the stored 5 */
	config_destroy(&c);
	puts("  read cap: config_read_server_list clamps to the caller-supplied cap");
}

static void test_write_empty_list(void) {
	struct config c = make_config("{}");
	assert(config_write_server_list(&c, NULL, 0));
	struct config_mp_server out[4];
	size_t count = 99;
	assert(config_read_server_list(&c, out, 4, &count));
	assert(count == 0);
	JSON_Array* arr = json_object_dotget_array(json_object(c.root), "mp.servers");
	assert(arr && json_array_get_count(arr) == 0);
	config_destroy(&c);
	puts("  write: an empty list round-trips to an empty (present) mp.servers array");
}

/* Audit finding 3: config_write_server_list must fail ATOMICALLY -- a
 * per-row parson set that fails (invalid UTF-8 is the one reachable
 * failure mode without mocking malloc: json_value_init_string_with_len
 * rejects it, so json_object_set_string returns JSONFailure) must never
 * leave a PARTIAL row (name set, address missing, or vice versa) appended
 * to the array, and must never touch `c`'s live tree at all -- a stray lone
 * 0xFF byte (an invalid UTF-8 lead byte on its own) in either field must
 * make the whole write fail closed with mp.servers left exactly as it was
 * before the call. */
static void test_write_server_list_atomic_on_invalid_utf8(void) {
	struct config c = make_config("{}");

	struct config_mp_server bad_name[1];
	snprintf(bad_name[0].name, sizeof(bad_name[0].name), "\xFF Bad");
	snprintf(bad_name[0].address, sizeof(bad_name[0].address), "ok.example.com:1");
	assert(!config_write_server_list(&c, bad_name, 1));
	assert(!json_object_dotget_array(json_object(c.root), "mp.servers"));

	struct config_mp_server bad_addr[1];
	snprintf(bad_addr[0].name, sizeof(bad_addr[0].name), "Fine");
	/* String-literal concat terminates the \xFF hex escape at the boundary
	 * -- "\xFFbad:1" would otherwise gobble 'b' as a hex digit (GCC warns
	 * "hex escape sequence out of range" and the value is not the single
	 * 0xFF byte this test needs). */
	snprintf(bad_addr[0].address, sizeof(bad_addr[0].address),
			"\xFF" "bad:1");
	assert(!config_write_server_list(&c, bad_addr, 1));
	assert(!json_object_dotget_array(json_object(c.root), "mp.servers"));

	/* A valid write after the failed ones still works -- the failure left
	 * no residue behind to corrupt a later, well-formed call. */
	struct config_mp_server good[1] = {
		{.name = "Fine", .address = "ok.example.com:1"},
	};
	assert(config_write_server_list(&c, good, 1));
	struct config_mp_server out[4];
	size_t count = 0;
	assert(config_read_server_list(&c, out, 4, &count));
	assert(count == 1);
	assert(strcmp(out[0].name, "Fine") == 0);
	assert(strcmp(out[0].address, "ok.example.com:1") == 0);

	config_destroy(&c);
	puts("  write: invalid UTF-8 in a row fails atomically -- no partial "
		 "row, no tree mutation, later valid writes unaffected");
}

static void test_read_truncates_hostile_oversized_field(void) {
	/* Hand-built via parson directly (not through config_write_server_list,
	 * which can never itself produce an oversized field -- struct
	 * config_mp_server's own fixed buffers already bound it): proves the
	 * READ side stays memory-safe against a hostile/hand-edited
	 * config.json. */
	JSON_Value* root = json_value_init_object();
	JSON_Object* obj = json_value_get_object(root);
	JSON_Value* arr_val = json_value_init_array();
	JSON_Array* arr = json_value_get_array(arr_val);
	JSON_Value* row_val = json_value_init_object();
	JSON_Object* row = json_value_get_object(row_val);
	char big[300];
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	json_object_set_string(row, "name", "Big");
	json_object_set_string(row, "address", big);
	assert(json_array_append_value(arr, row_val) == JSONSuccess);
	assert(json_object_dotset_value(obj, "mp.servers", arr_val) == JSONSuccess);

	struct config c = {.root = root, .ephemeral_key_count = 0};
	struct config_mp_server out[4];
	size_t count = 0;
	assert(config_read_server_list(&c, out, 4, &count));
	assert(count == 1);
	assert(strcmp(out[0].name, "Big") == 0);
	assert(strlen(out[0].address) == CONFIG_MP_SERVER_ADDRESS_CAP - 1);
	assert(out[0].address[CONFIG_MP_SERVER_ADDRESS_CAP - 1 - 1] == 'x');
	config_destroy(&c);
	puts("  read: an oversized hand-edited address field truncates safely, no overflow");
}

/* ---- screen_multiplayer_setup.c: pure array-mutation hooks ------------- */

static void test_server_array_add_edit_delete(void) {
	screen_mps_test_reset_servers();
	assert(screen_mps_test_server_count() == 0);

	assert(screen_mps_test_add_server("Home", "example.com:25566"));
	assert(screen_mps_test_server_count() == 1);
	char name[64], addr[160];
	assert(screen_mps_test_server(0, name, sizeof(name), addr, sizeof(addr)));
	assert(strcmp(name, "Home") == 0);
	assert(strcmp(addr, "example.com:25566") == 0);

	/* Both fields trim (screen_cworld_java_trim, shared with world create/
	 * rename). */
	assert(screen_mps_test_add_server("  Padded Name  ", "  host2:1234  "));
	assert(screen_mps_test_server_count() == 2);
	assert(screen_mps_test_server(1, name, sizeof(name), addr, sizeof(addr)));
	assert(strcmp(name, "Padded Name") == 0);
	assert(strcmp(addr, "host2:1234") == 0);

	/* Empty name (post-trim) rejected; nothing added. */
	assert(!screen_mps_test_add_server("   ", "host3:1"));
	assert(screen_mps_test_server_count() == 2);

	/* mp_address_parse's own documented failure shape (a colon-only string
	 * -- mp_codec.h's comment above mp_address_parse). */
	assert(!screen_mps_test_add_server("Bad", ":"));
	assert(screen_mps_test_server_count() == 2);

	assert(screen_mps_test_edit_server(0, "Home Renamed", "otherhost:9999"));
	assert(screen_mps_test_server(0, name, sizeof(name), addr, sizeof(addr)));
	assert(strcmp(name, "Home Renamed") == 0);
	assert(strcmp(addr, "otherhost:9999") == 0);
	assert(!screen_mps_test_edit_server(99, "x", "y:1"));

	/* Fill to the list cap; one more must fail without changing count. */
	screen_mps_test_reset_servers();
	char nbuf[32], abuf[32];
	for(size_t i = 0; i < CONFIG_MP_SERVER_LIST_MAX; i++) {
		snprintf(nbuf, sizeof(nbuf), "Srv%zu", i);
		snprintf(abuf, sizeof(abuf), "host%zu.example.com:1", i);
		assert(screen_mps_test_add_server(nbuf, abuf));
	}
	assert(screen_mps_test_server_count() == CONFIG_MP_SERVER_LIST_MAX);
	assert(!screen_mps_test_add_server("Overflow", "overflow.example.com:1"));
	assert(screen_mps_test_server_count() == CONFIG_MP_SERVER_LIST_MAX);

	/* Delete shifts the remainder down (mps_array_delete). */
	assert(screen_mps_test_delete_server(0));
	assert(screen_mps_test_server_count() == CONFIG_MP_SERVER_LIST_MAX - 1);
	assert(screen_mps_test_server(0, name, sizeof(name), addr, sizeof(addr)));
	assert(strcmp(name, "Srv1") == 0);
	assert(!screen_mps_test_delete_server(999));

	puts("  screen: server-list add/edit/delete (trim, validate, cap, shift) all pass");
}

static void test_username_valid(void) {
	assert(screen_mps_test_username_valid("Notch"));
	assert(screen_mps_test_username_valid("Tuck_01"));
	assert(screen_mps_test_username_valid("a"));
	assert(!screen_mps_test_username_valid(""));
	assert(!screen_mps_test_username_valid(NULL));
	/* Exactly Beta's 16-code-unit login cap [740c583
	 * Packet1Login.readPacketData] -- with the Beta account alphabet. */
	assert(screen_mps_test_username_valid("SixteenCharName_"));
	assert(!screen_mps_test_username_valid("SixteenCharName!"));
	char too_long[18];
	memset(too_long, 'a', 17);
	too_long[17] = 0;
	assert(!screen_mps_test_username_valid(too_long));
	/* Maintainer rule 2026-09-11: a chosen, distinguishable name -- the
	 * project's own name is reserved in every letter case, and anything
	 * outside [A-Za-z0-9_] (spaces, slashes, colour codes) is refused. */
	assert(!screen_mps_test_username_valid("CaveWii"));
	assert(!screen_mps_test_username_valid("cavewii"));
	assert(!screen_mps_test_username_valid("CAVEWII"));
	assert(screen_mps_test_username_valid("CaveWii2")); /* not the reserved word */
	assert(!screen_mps_test_username_valid("Cave Wii"));
	assert(!screen_mps_test_username_valid("Cave/Wii"));
	assert(!screen_mps_test_username_valid("Notch\xC2\xA7a"));
	puts("  screen: username validation = Packet1 cap + Beta alphabet + no CaveWii");
}

/* The join gate: Join/Quick Connect/Direct Connect all commit through
 * mps_activate, which parks the target and opens the name editor while
 * the name is not chosen; Cancel abandons the parked join. */
static void test_join_gate(void) {
	bool opened = false, parked = false;
	assert(screen_mps_test_join_gate("", &opened, &parked));
	assert(opened && parked);
	assert(screen_mps_test_join_gate_cancel());
	assert(screen_mps_test_join_gate("CaveWii", &opened, &parked));
	assert(opened && parked);
	assert(screen_mps_test_join_gate_cancel());
	/* A chosen name is not gated (the probe refuses to run the real
	 * connect hand-off, which is the point). */
	assert(!screen_mps_test_join_gate("Tuck_01", &opened, &parked));
	puts("  screen: join gate parks the target and opens the name editor");
}

/* Audit finding 1a: Refresh must stay disabled for the whole time a ping
 * sweep is in flight (ping_cursor < server_count), so mashing it can never
 * abandon-and-restart an in-flight probe -- it only re-enables once every
 * row has resolved (ping_cursor == server_count). No thread/socket is ever
 * spun up here: screen_mps_test_set_ping_cursor pokes the file-scope
 * counter directly, matching this file's "no live socket anywhere" rule. */
static void test_refresh_disabled_while_sweep_in_flight(void) {
	screen_mps_test_reset_servers();
	screen_mps_test_set_ping_cursor(0);
	assert(!screen_mps_test_refresh_enabled()); /* no servers: stays off */

	assert(screen_mps_test_add_server("A", "a.example.com:1"));
	assert(screen_mps_test_add_server("B", "b.example.com:1"));

	screen_mps_test_set_ping_cursor(0);
	assert(!screen_mps_test_refresh_enabled()); /* sweep just starting */
	screen_mps_test_set_ping_cursor(1);
	assert(!screen_mps_test_refresh_enabled()); /* row 0 done, row 1 in flight */
	screen_mps_test_set_ping_cursor(2);
	assert(screen_mps_test_refresh_enabled()); /* every row resolved */

	puts("  screen: Refresh stays disabled for the entire in-flight sweep, "
		 "re-enables only once every row has resolved");
}

/* Audit finding 2: the pure TOCTOU-revalidation decision mps_ping_tick's
 * timeout branch consults after re-locking and re-reading the handoff a
 * second time. A revalidated done MUST win even if the (now-irrelevant)
 * recomputed deadline also looks expired; a still-not-done probe abandons
 * only if ITS revalidated deadline is reached, and otherwise waits (the
 * "phase advanced, recompute instead of abandoning" case). */
static void test_ping_toctou_decision(void) {
	assert(screen_mps_test_ping_toctou_decide(true, true) == 1);
	assert(screen_mps_test_ping_toctou_decide(true, false) == 1);
	assert(screen_mps_test_ping_toctou_decide(false, true) == 2);
	assert(screen_mps_test_ping_toctou_decide(false, false) == 0);
	puts("  screen: ping TOCTOU revalidation -- a done-in-the-gap result is "
		 "always consumed, never discarded by a stale deadline; a still-"
		 "live probe with a fresh (not-yet-reached) deadline waits instead "
		 "of being abandoned");
}

/* ---- v3: live-ping 0xFF/string16 parser + latency->bars mapping -------- */

/* Hand-builds a 0xFF + string16 frame from a plain ASCII C string (embed a
 * raw 0xA7 byte via the octal escape \247, matching this codebase's own
 * convention elsewhere -- e.g. this file's "\2477%s" motd-line prefixes).
 * Every code unit is BE and single-byte-valued (0x00XX), exactly how a real
 * server's "MOTD\247online\247max" reply is actually encoded on the wire --
 * the same shape mps_ping_parse_response's own doc comment describes. */
static size_t build_ping_frame(const char* ascii, uint8_t* out,
							   size_t out_cap) {
	size_t len = strlen(ascii);
	assert(3 + len * 2 <= out_cap);
	out[0] = 0xFF;
	out[1] = (uint8_t)((len >> 8) & 0xFF);
	out[2] = (uint8_t)(len & 0xFF);
	for(size_t i = 0; i < len; i++) {
		out[3 + i * 2] = 0;
		out[3 + i * 2 + 1] = (uint8_t)(unsigned char)ascii[i];
	}
	return 3 + len * 2;
}

static void test_ping_parse_well_formed(void) {
	uint8_t frame[256];
	size_t n = build_ping_frame("My Server\2475\24720", frame, sizeof(frame));

	char motd[64];
	int32_t online = -99, max = -99;
	assert(screen_mps_test_parse_ping_response(frame, n, motd, sizeof(motd),
											   &online, &max));
	assert(strcmp(motd, "My Server") == 0);
	assert(online == 5);
	assert(max == 20);
	puts("  ping parse: well-formed \"motd\\247online\\247max\" splits "
		 "correctly");
}

static void test_ping_parse_hostile_truncated(void) {
	/* Wrong lead byte: not a kick/disconnect frame at all. */
	uint8_t bad_lead[8] = {0x01, 0x00, 0x01, 0x00, 'x'};
	char motd[64];
	int32_t online, max;
	assert(!screen_mps_test_parse_ping_response(bad_lead, sizeof(bad_lead),
												motd, sizeof(motd), &online,
												&max));

	/* Length field CLAIMS 100 code units but the buffer holds far fewer
	 * bytes than that promises -- must fail closed, not read out of
	 * bounds. */
	uint8_t truncated[10] = {0xFF, 0x00, 100, 'h', 'i'};
	assert(!screen_mps_test_parse_ping_response(
		truncated, sizeof(truncated), motd, sizeof(motd), &online, &max));

	/* Hostile negative length (top bit set on the signed i16 length). */
	uint8_t negative_len[8] = {0xFF, 0x80, 0x00, 0, 0, 0, 0, 0};
	assert(!screen_mps_test_parse_ping_response(
		negative_len, sizeof(negative_len), motd, sizeof(motd), &online,
		&max));

	/* Too short to even hold the 3-byte header. */
	uint8_t too_short[2] = {0xFF, 0x00};
	assert(!screen_mps_test_parse_ping_response(
		too_short, sizeof(too_short), motd, sizeof(motd), &online, &max));

	puts("  ping parse: hostile/truncated frames (bad lead byte, claimed "
		 "length past the buffer, negative length, sub-header size) all "
		 "fail closed");
}

static void test_ping_parse_missing_separators(void) {
	uint8_t frame[256];
	/* A real legacy (pre-ping) Beta kick reply: 0xFF + string16, no \247
	 * separators at all -- still parses, whole string becomes motd, counts
	 * stay unknown (-1) rather than being discarded. */
	size_t n = build_ping_frame("Server is full, try again later", frame,
							   sizeof(frame));
	char motd[64];
	int32_t online = -99, max = -99;
	assert(screen_mps_test_parse_ping_response(frame, n, motd, sizeof(motd),
											   &online, &max));
	assert(strcmp(motd, "Server is full, try again later") == 0);
	assert(online == -1);
	assert(max == -1);

	/* Exactly one separator: still not the 3-field shape -- same
	 * whole-string-as-motd degrade. */
	n = build_ping_frame("Half\247Baked", frame, sizeof(frame));
	assert(screen_mps_test_parse_ping_response(frame, n, motd, sizeof(motd),
											   &online, &max));
	assert(strcmp(motd, "Half\247Baked") == 0);
	assert(online == -1 && max == -1);

	puts("  ping parse: fewer than 2 separators degrades to whole-string "
		 "motd with online/max left unknown");
}

static void test_ping_parse_huge_values(void) {
	uint8_t frame[256];
	/* 12-digit decimal counts overflow int32_t -- must clamp, never wrap
	 * or read garbage. */
	size_t n = build_ping_frame("Big\2479999999999\2478888888888", frame,
							   sizeof(frame));
	char motd[64];
	int32_t online = 0, max = 0;
	assert(screen_mps_test_parse_ping_response(frame, n, motd, sizeof(motd),
											   &online, &max));
	assert(strcmp(motd, "Big") == 0);
	assert(online == INT32_MAX);
	assert(max == INT32_MAX);

	/* Non-numeric count fields report unknown (-1), not a garbage parse. */
	n = build_ping_frame("Weird\247abc\247def", frame, sizeof(frame));
	assert(screen_mps_test_parse_ping_response(frame, n, motd, sizeof(motd),
											   &online, &max));
	assert(online == -1 && max == -1);

	puts("  ping parse: oversized numeric fields clamp to INT32_MAX, "
		 "non-numeric fields report unknown");
}

static void test_ping_bars_thresholds(void) {
	assert(screen_mps_test_ping_bars(-1) == 0);   /* unknown/not probed */
	assert(screen_mps_test_ping_bars(0) == 5);
	assert(screen_mps_test_ping_bars(149) == 5);
	assert(screen_mps_test_ping_bars(150) == 4);
	assert(screen_mps_test_ping_bars(299) == 4);
	assert(screen_mps_test_ping_bars(300) == 3);
	assert(screen_mps_test_ping_bars(599) == 3);
	assert(screen_mps_test_ping_bars(600) == 2);
	assert(screen_mps_test_ping_bars(999) == 2);
	assert(screen_mps_test_ping_bars(1000) == 1);
	assert(screen_mps_test_ping_bars(5000) == 1);
	puts("  ping bars: latency->bar-count thresholds match the spec "
		 "(5/<150, 4/<300, 3/<600, 2/<1000, 1/else)");
}

int main(void) {
	test_migration_from_legacy_scalar();
	test_no_migration_when_nothing_to_migrate();
	test_existing_array_wins_over_legacy_scalar();
	test_round_trip_and_scalars_untouched();
	test_read_cap_enforced();
	test_write_empty_list();
	test_write_server_list_atomic_on_invalid_utf8();
	test_read_truncates_hostile_oversized_field();
	test_server_array_add_edit_delete();
	test_username_valid();
	test_join_gate();
	test_refresh_disabled_while_sweep_in_flight();
	test_ping_toctou_decision();
	test_ping_parse_well_formed();
	test_ping_parse_hostile_truncated();
	test_ping_parse_missing_separators();
	test_ping_parse_huge_values();
	test_ping_bars_thresholds();

	printf("test_mp_setup_screen: OK\n");
	return 0;
}
