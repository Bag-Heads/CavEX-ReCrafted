#include "stage_auth.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t ipv4(unsigned a, unsigned b, unsigned c, unsigned d) {
	return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) |
		   (uint32_t)d;
}

static void test_subnet(void) {
	uint32_t local = ipv4(192, 168, 1, 42);
	uint32_t mask24 = 0xffffff00u;

	/* Same /24 -> in. */
	assert(cw_stage_ip_in_subnet(ipv4(192, 168, 1, 1), local, mask24));
	assert(cw_stage_ip_in_subnet(ipv4(192, 168, 1, 254), local, mask24));
	/* The console's own address is trivially in its own subnet. */
	assert(cw_stage_ip_in_subnet(local, local, mask24));

	/* Different /24 -> out, even one bit off. */
	assert(!cw_stage_ip_in_subnet(ipv4(192, 168, 2, 1), local, mask24));
	assert(!cw_stage_ip_in_subnet(ipv4(192, 169, 1, 1), local, mask24));
	assert(!cw_stage_ip_in_subnet(ipv4(10, 0, 0, 1), local, mask24));

	/* Wider /16 admits the /24-adjacent neighbor a /24 mask would reject. */
	uint32_t mask16 = 0xffff0000u;
	assert(cw_stage_ip_in_subnet(ipv4(192, 168, 2, 1), local, mask16));
	assert(!cw_stage_ip_in_subnet(ipv4(192, 169, 0, 1), local, mask16));

	/* Netmask 0 (not-yet-configured network) refuses everyone, including
	 * the console's own recorded address -- never a silent accept-all
	 * during the DHCP settle window. */
	assert(!cw_stage_ip_in_subnet(local, local, 0));
	assert(!cw_stage_ip_in_subnet(ipv4(0, 0, 0, 0), 0, 0));
}

static void test_token_equal(void) {
	/* Equal tokens, various lengths. */
	assert(cw_stage_token_equal("", ""));
	assert(cw_stage_token_equal("a", "a"));
	assert(cw_stage_token_equal("cavewii-stage-secret-2026",
								 "cavewii-stage-secret-2026"));

	/* Different content, same length. */
	assert(!cw_stage_token_equal("aaaaaaaa", "aaaaaaab"));
	assert(!cw_stage_token_equal("Aaaaaaaa", "aaaaaaaa"));

	/* Different length -- shorter must not be treated as a valid prefix
	 * match (a naive length-then-compare with an early strlen mismatch
	 * return is fine functionally, but this also pins that a PROPER-length
	 * empty token never matches a non-empty one, and vice versa). */
	assert(!cw_stage_token_equal("short", "shorter"));
	assert(!cw_stage_token_equal("shorter", "short"));
	assert(!cw_stage_token_equal("", "nonempty"));
	assert(!cw_stage_token_equal("nonempty", ""));

	/* NULL never matches anything, including NULL vs NULL -- a caller with
	 * no token loaded must never read as "authenticated". */
	assert(!cw_stage_token_equal(NULL, "x"));
	assert(!cw_stage_token_equal("x", NULL));
	assert(!cw_stage_token_equal(NULL, NULL));

	/* Max-length token (CW_STAGE_TOKEN_MAX_BYTES) still compares correctly
	 * end to end, including its very last byte. */
	char long_a[CW_STAGE_TOKEN_MAX_BYTES + 1];
	char long_b[CW_STAGE_TOKEN_MAX_BYTES + 1];
	for(int i = 0; i < CW_STAGE_TOKEN_MAX_BYTES; ++i) {
		long_a[i] = (char)('a' + (i % 26));
		long_b[i] = long_a[i];
	}
	long_a[CW_STAGE_TOKEN_MAX_BYTES] = '\0';
	long_b[CW_STAGE_TOKEN_MAX_BYTES] = '\0';
	assert(cw_stage_token_equal(long_a, long_b));
	long_b[CW_STAGE_TOKEN_MAX_BYTES - 1] =
		(char)(long_b[CW_STAGE_TOKEN_MAX_BYTES - 1] ^ 0x20);
	assert(!cw_stage_token_equal(long_a, long_b));
}

static void test_token_trim(void) {
	char a[64];

	strcpy(a, "abc123\n");
	cw_stage_token_trim(a);
	assert(strcmp(a, "abc123") == 0);

	strcpy(a, "abc123\r\n");
	cw_stage_token_trim(a);
	assert(strcmp(a, "abc123") == 0);

	strcpy(a, "abc123   \t\n");
	cw_stage_token_trim(a);
	assert(strcmp(a, "abc123") == 0);

	/* No trailing whitespace: untouched. */
	strcpy(a, "abc123");
	cw_stage_token_trim(a);
	assert(strcmp(a, "abc123") == 0);

	/* All-whitespace file content trims down to empty (server treats an
	 * empty loaded token as "not provisioned", tested at the main.c level
	 * via hardware/manual verification -- this only pins the trim itself). */
	strcpy(a, "  \n\r\t");
	cw_stage_token_trim(a);
	assert(strcmp(a, "") == 0);

	/* Leading whitespace is a real (if unusual) token byte and must survive
	 * the trim untouched -- only trailing junk is stripped. */
	strcpy(a, " abc123\n");
	cw_stage_token_trim(a);
	assert(strcmp(a, " abc123") == 0);

	/* NULL is a no-op, not a crash. */
	cw_stage_token_trim(NULL);
}

static void test_valid_sd_path(void) {
	/* The game's SD-root texture pack tree (config.json's paths.texturepack
	 * default "assets", commit 4b9caf4) -- e.g. the painting atlas. */
	assert(valid_sd_path("sd:/assets/art/kz.png"));
	assert(valid_sd_path("sd:/assets/"));
	/* Case-insensitivity applies to the prefix match itself, the same as
	 * every other allow-listed prefix (the leading "sd:/" scheme is matched
	 * case-sensitively by valid_sd_path's own first check, same as before
	 * this change -- only the path tail after it is compared via
	 * ci_has_prefix()). */
	assert(valid_sd_path("sd:/ASSETS/ART/KZ.PNG"));

	/* A ".." anywhere in the path is rejected before any prefix check,
	 * including inside an otherwise-allowed sd:/assets/ path. */
	assert(!valid_sd_path("sd:/assets/../apps/CavEX/stage_token.txt"));

	/* The allow-listed prefix must include the trailing slash -- a
	 * same-prefix-but-different-directory name like "sd:/assetsX/..." must
	 * NOT be admitted by a naive strncmp(path, "sd:/assets", 10). */
	assert(!valid_sd_path("sd:/assetsX/foo"));

	/* sd:/beta/ (jar importer data) is allow-listed like sd:/texturepacks/. */
	assert(valid_sd_path("sd:/beta/b1.7.3.jar"));
	assert(valid_sd_path("sd:/beta/resources/x.ogg"));
	assert(!valid_sd_path("sd:/betax/"));
	assert(!valid_sd_path("sd:/beta/../apps"));

	/* The stage-auth token itself stays denied through every prefix,
	 * sd:/apps/ included -- checked ahead of the allow-list prefixes. */
	assert(!valid_sd_path("sd:/apps/CavEX/stage_token.txt"));

	/* The live game config at the SD root and its default are allow-listed as
	 * EXACT manifest paths (stage_manifest_paths.h, 2026-09-17): fetch/put-able
	 * so a controls refresh is provable from the card. Only these two exact root
	 * files -- no other sd:/ root file, and never the token. */
	assert(valid_sd_path("sd:/config.json"));
	assert(valid_sd_path("sd:/config_wii.json"));
	assert(!valid_sd_path("sd:/config.json.bak"));
	assert(!valid_sd_path("sd:/config_other.json"));
	assert(!valid_sd_path("sd:/config.json/../apps/CavEX/stage_token.txt"));

	/* Existing prefixes/exact paths are unaffected by the new one. */
	assert(valid_sd_path("sd:/apps/CavEX/boot.dol"));
	assert(valid_sd_path("sd:/texturepacks/mypack/title/splashes.txt"));
	assert(valid_sd_path(CW_STAGE_BOOT_TRACE_PATH));
	assert(!valid_sd_path("sd:/random_file.txt"));
	assert(!valid_sd_path("not_sd_at_all"));
	assert(!valid_sd_path(NULL));
}

static void test_app_folder_migration(void) {
	const char* apps[] = {"CaveWii_v2", "CavEX"};
	char path[128];
	for(size_t i = 0; i < sizeof apps / sizeof apps[0]; ++i) {
		snprintf(path, sizeof path, "sd:/apps/%s/boot.dol", apps[i]);
		assert(valid_sd_path(path));
		snprintf(path, sizeof path, "sd:/apps/%s/cw_run_args.txt", apps[i]);
		assert(valid_sd_path(path));
		snprintf(path, sizeof path, "sd:/apps/%s/saves/New World/level.dat", apps[i]);
		assert(valid_sd_path(path));
		snprintf(path, sizeof path, "sd:/apps/%s/stage_token.txt", apps[i]);
		assert(!valid_sd_path(path));
		snprintf(path, sizeof path, "sd:/APPS/%s/STAGE_TOKEN.TXT", apps[i]);
		assert(!valid_sd_path(path));
	}
}

int main(void) {
	test_app_folder_migration();
	test_subnet();
	test_token_equal();
	test_token_trim();
	test_valid_sd_path();
	return 0;
}
