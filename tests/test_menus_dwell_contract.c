/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.

	The menus_dwell STEP CONTRACT, pinned from both sides.

	tools/wii_telemetry_server.py grades a real-Wii menus_dwell run by
	comparing the heartbeat's monotonic high-water-mark step (`map_max`)
	against PROFILES["menus_dwell"]["min_map_step"]. That number is an
	ORDINAL out of menu_autopilot_script.c's `enum { ... }` step table --
	specifically the one menu_ap_script_step_required_for_pass() returns --
	so ANY step inserted above it shifts it, and a stale value is a silently
	LOOSER `>=` gate: a run that abandoned the script half way still passes
	the menus scenario gate.

	That has already happened twice (39 -> 47 -> 48 while the real ordinal
	had moved on), each time discovered by hand long afterwards, and the
	collector's own comment asks the next editor to "recompute by hand". A
	comment cannot fail a build; this test can. It reads the collector as
	text and asserts its current contract IS the C function's answer, that
	the contract is not itself listed as spent, and that every spent value
	is strictly older (smaller) than the current one.

	Pure/headless, same rationale as test_menu_autopilot_script: the script
	module has no gfx/game_state dependency, so this links just that one TU.
	It needs the repo root as its working directory (CMakeLists sets it) to
	read the collector.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/game/menu_autopilot_script.h"

#define COLLECTOR "tools/wii_telemetry_server.py"

static char* read_file(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f) {
		fprintf(stderr,
				"cannot open %s -- this test must run with the repo root as "
				"its working directory\n",
				path);
		abort();
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	assert(n > 0);
	fseek(f, 0, SEEK_SET);
	char* buf = malloc((size_t)n + 1);
	assert(buf);
	size_t got = fread(buf, 1, (size_t)n, f);
	buf[got] = '\0';
	fclose(f);
	return buf;
}

/* The integer assigned to `name = ` at the start of a line. */
static long read_assignment(const char* src, const char* name) {
	char needle[64];
	snprintf(needle, sizeof(needle), "\n%s = ", name);
	const char* at = strstr(src, needle);
	if(!at) {
		fprintf(stderr, "%s: no `%s = <int>` assignment\n", COLLECTOR, name);
		abort();
	}
	return strtol(at + strlen(needle), NULL, 10);
}

/* Every integer key of the `name = {` dict, up to its closing `\n}`. */
static size_t read_dict_keys(const char* src, const char* name, long* out,
							 size_t cap) {
	char needle[64];
	snprintf(needle, sizeof(needle), "\n%s = {", name);
	const char* at = strstr(src, needle);
	if(!at) {
		fprintf(stderr, "%s: no `%s = {` dict\n", COLLECTOR, name);
		abort();
	}
	const char* end = strstr(at + 1, "\n}");
	assert(end && "unterminated dict");
	size_t count = 0;
	for(const char* line = strchr(at + 1, '\n'); line && line < end;
		line = strchr(line + 1, '\n')) {
		const char* p = line + 1;
		while(*p == ' ' || *p == '\t')
			p++;
		if(*p < '0' || *p > '9')
			continue; /* a continuation line of a previous reason string */
		char* stop = NULL;
		long key = strtol(p, &stop, 10);
		if(!stop || *stop != ':')
			continue;
		assert(count < cap);
		out[count++] = key;
	}
	return count;
}

/* The `1 << N:` keys of the `name = {` dict, as a bitmask. The np_flags table
 * writes its keys as shifts (they are bit POSITIONS, and a decimal there would
 * be unreadable), which read_dict_keys above deliberately skips -- so this is
 * its shift-shaped twin rather than a loosening of that parser. */
static unsigned read_dict_shift_keys(const char* src, const char* name) {
	char needle[64];
	snprintf(needle, sizeof(needle), "\n%s = {", name);
	const char* at = strstr(src, needle);
	if(!at) {
		fprintf(stderr, "%s: no `%s = {` dict\n", COLLECTOR, name);
		abort();
	}
	const char* end = strstr(at + 1, "\n}");
	assert(end && "unterminated dict");
	unsigned mask = 0;
	for(const char* p = strstr(at, "1 << "); p && p < end;
		p = strstr(p + 1, "1 << ")) {
		char* stop = NULL;
		long bit = strtol(p + 5, &stop, 10);
		if(!stop || *stop != ':')
			continue;
		assert(bit >= 0 && bit < 32);
		mask |= 1u << (unsigned)bit;
	}
	return mask;
}

int main(void) {
	char* src = read_file(COLLECTOR);

	/* THE np_flags MIRROR (push-0921 A11 defined the bits, A14 added one).
	 * Every bit the walker can raise must be NAMED in the collector's table:
	 * an unnamed bit is still refused (any non-zero np_flags fails) but the
	 * verdict would then say "np_flags=0x20 -- " with nothing after the dash,
	 * and the next person would have to guess what the run did wrong. The C
	 * side is the definition (menu_autopilot_script.h); this asserts the copy
	 * covers it exactly -- no missing bit, and no invented one. */
	unsigned named = read_dict_shift_keys(src, "NP_FLAG_NAMES");
	/* MENU_AP_NP_FLAG_ALL, not a hand-written OR of the flags that existed
	 * when this line was written (review A16-8): the old form was a SECOND
	 * copy of the list, so a seventh flag added to the enum and to neither
	 * list kept this mirror green. The union now lives beside the enum, in
	 * the same header, and the walker's own _Static_assert reads it too. */
	unsigned defined = MENU_AP_NP_FLAG_ALL;

	/* THE UNION IS DERIVED, NOT COPIED (push-0921 A23, review A20-7).
	 * MENU_AP_NP_FLAG_ALL was still a hand-written OR -- in a different file
	 * from the enum, but hand-written -- so a flag added to the enum ALONE
	 * left this mirror green, the _Static_assert true, and the 9th-flag
	 * heartbeat truncation reachable through one omission. Both sides now
	 * come from menu_autopilot_script.h's X-macro table; these three lines
	 * are what says so out loud, and what fails if a row is ever given a
	 * value that is not its own single bit, or two rows the same bit. */
	unsigned rows = 0
#define MENU_AP_NP_FLAG_COUNT_ROW(name, bit) +1u
		MENU_AP_NP_FLAG_TABLE(MENU_AP_NP_FLAG_COUNT_ROW);
#undef MENU_AP_NP_FLAG_COUNT_ROW
/* `defined`, not MENU_AP_NP_FLAG_ALL: that macro expands the same table, and
 * a nested expansion of a macro already being expanded is suppressed. */
#define MENU_AP_NP_FLAG_CHECK_ROW(name, bit)                                   \
	assert((unsigned)(name) == (1u << (bit))                                   \
		   && "a walker flag is not the single bit its table row names");      \
	assert((defined & (unsigned)(name))                                        \
		   && "a walker flag is missing from the derived union");
	MENU_AP_NP_FLAG_TABLE(MENU_AP_NP_FLAG_CHECK_ROW)
#undef MENU_AP_NP_FLAG_CHECK_ROW
	unsigned bits = 0;
	for(unsigned b = 0; b < 32; b++)
		if(defined & (1u << b))
			bits++;
	if(bits != rows) {
		fprintf(stderr,
				"MENU_AP_NP_FLAG_TABLE has %u rows but the union carries %u "
				"bits (0x%02x) -- two rows share a bit number\n",
				rows, bits, defined);
		abort();
	}
	if(named != defined) {
		fprintf(stderr,
				"%s NP_FLAG_NAMES is STALE: names 0x%02x, the walker raises "
				"0x%02x (menu_autopilot_script.h's MENU_AP_NP_FLAG_*)\n",
				COLLECTOR, named, defined);
		abort();
	}

	long contract = read_assignment(src, "MENUS_DWELL_STEP_CONTRACT");
	long spent[32];
	size_t spent_n = read_dict_keys(src, "MENUS_DWELL_STEP_SPENT", spent,
									sizeof(spent) / sizeof(spent[0]));

	/* THE PIN. If this fails, a step was inserted into (or removed from) the
	 * script's enum without moving the collector's gate with it: recompute
	 * MENUS_DWELL_STEP_CONTRACT from menu_ap_script_step_required_for_pass()
	 * and add the superseded number to MENUS_DWELL_STEP_SPENT with the
	 * reason it is spent. */
	/* hw1030i hand count of the enum in menu_autopilot_script.c (the header
	 * exports its accessors): 0..23 title/packs, 24..48 Select/Create,
	 * 49..58 world/sign, 59..76 inventory, 77..107 containers,
	 * 108..126 pause/quit, 127..161 no-pointer (+2 Texture Packs scroll), DONE 162, COUNT 163.
	 * Pin independently: moving BOTH copies without auditing the spent set
	 * must not silently keep this contract green. */
	assert(contract == 161);
	assert(menu_ap_script_step_count() == 163);
	assert(menu_ap_script_step_cworld_sub_begin() == 36);
	assert(menu_ap_script_step_inv_pages() == 67);
	long required = menu_ap_script_step_required_for_pass();
	if(contract != required) {
		fprintf(stderr,
				"menus_dwell step contract is STALE: %s says %ld, "
				"menu_ap_script_step_required_for_pass() says %ld\n",
				COLLECTOR, contract, required);
		abort();
	}

	/* The gate must sit inside the script's own step table, and below the
	 * terminal step -- a threshold no run can ever reach would fail every
	 * hardware run instead of grading it. */
	assert(required > 0 && required < menu_ap_script_step_count());

	/* Every retired number is named with a reason, is strictly older than
	 * the current contract, and the current one is not among them. */
	const long expected_spent[] = {39, 47, 48, 100, 138, 139, 159};
	assert(spent_n == sizeof(expected_spent) / sizeof(expected_spent[0]));
	for(size_t k = 0; k < spent_n; k++) {
		assert(spent[k] == expected_spent[k]
			   && "the spent-version set changed without a contract audit");
		assert(spent[k] != contract
			   && "the CURRENT contract is listed as spent");
		assert(spent[k] < contract
			   && "a spent ordinal is not older than the current contract");
	}

	printf("test_menus_dwell_contract: PASS (min_map_step=%ld == "
		   "required_for_pass, %zu spent ordinal(s), step table %d, "
		   "np_flags 0x%02x named)\n",
		   contract, spent_n, menu_ap_script_step_count(), named);
	free(src);
	return 0;
}
