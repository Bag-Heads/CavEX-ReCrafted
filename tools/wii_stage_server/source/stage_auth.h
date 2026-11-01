#ifndef CAVEWII_STAGE_AUTH_H
#define CAVEWII_STAGE_AUTH_H

/* Shared subnet + token check primitives for the Wii stage server (main.c)
 * and its PC unit test (tests/test_stage_auth.c). Pure, header-only (static
 * inline), zero I/O -- deliberately dependency-free (stdint/string only) so
 * both sides compile it byte-identically.
 *
 * Backs the stage-server hardening deferred item (2026-09):
 *   (a) SUBNET gate -- accept only peers inside the console's own configured
 *       network (cw_stage_ip_in_subnet, called with the console's DHCP/static
 *       IP+netmask already parsed to host-order uint32_t by main.c's
 *       existing parse_ipv4()/configure_network()).
 *   (b) TOKEN gate -- constant-time match against the shared secret loaded
 *       from sd:/apps/CavEX/stage_token.txt (CW_STAGE_TOKEN_PATH) when
 *       that file is present on the card; a SOFT-LAUNCH no-op (never refuse,
 *       but say so via the CWAUTH1 reply) when the file is absent, because
 *       the live rig currently has no token provisioned -- see main.c's
 *       cw_stage_auth_gate() for the per-connection state machine that
 *       drives these two primitives, and stage_wii_dol.ps1 / provision_wii_
 *       sd.ps1 for the client/provisioning side.
 */

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stage_manifest_paths.h"

#define CW_STAGE_TOKEN_MAX_BYTES 128
#define CW_STAGE_TOKEN_PATH "sd:/apps/CavEX/stage_token.txt"
/* Keep the old card secret protected throughout the folder migration. */
#define CW_STAGE_TOKEN_LEGACY_PATH "sd:/apps/CaveWii_v2/stage_token.txt"

/* True iff `ip` is inside the same network as `local_ip` under `netmask`,
 * all as 32-bit host-order IPv4 addresses (e.g. from main.c's existing
 * parse_ipv4()). A netmask of 0 (network not yet configured) always returns
 * false -- refuse rather than silently accept every peer during the DHCP
 * settle window. */
static inline int cw_stage_ip_in_subnet(uint32_t ip, uint32_t local_ip,
										 uint32_t netmask) {
	if(netmask == 0)
		return 0;
	return (ip & netmask) == (local_ip & netmask);
}

/* Constant-time compare of two NUL-terminated ASCII tokens. Returns 1 if
 * equal (same length, same bytes), 0 otherwise. Iterates for
 * max(strlen(a), strlen(b)) bytes regardless of where -- or whether -- the
 * strings first differ, and folds a length mismatch into the same OR chain
 * as a byte mismatch, so neither the early-exit shape nor the return value
 * of a naive strcmp()/memcmp() leaks which prefix of a guessed token was
 * right over a LAN timing side-channel. NULL is treated as "never equal to
 * anything, including another NULL" -- a caller passing NULL has no token to
 * check against, which must never be treated as an auth pass. */
static inline int cw_stage_token_equal(const char* a, const char* b) {
	if(!a || !b)
		return 0;
	size_t la = 0, lb = 0;
	while(a[la])
		++la;
	while(b[lb])
		++lb;
	size_t max = la > lb ? la : lb;
	unsigned char diff = (unsigned char)(la != lb);
	for(size_t i = 0; i < max; ++i) {
		unsigned char ca = i < la ? (unsigned char)a[i] : 0;
		unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
		diff = (unsigned char)(diff | (ca ^ cb));
	}
	return diff == 0;
}

/* Trims trailing '\n', '\r', ' ', '\t' off a token loaded from a file, in
 * place (fread()'d file content is not NUL-terminated newline-stripped the
 * way a hand-typed header line is). Leading whitespace is left alone on
 * purpose -- a token is an opaque shared secret, not a trimmed human
 * sentence, and silently eating a leading byte the operator typed would
 * make the file and the loaded token quietly disagree. */
static inline void cw_stage_token_trim(char* s) {
	if(!s)
		return;
	size_t n = 0;
	while(s[n])
		++n;
	while(n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
					s[n - 1] == ' ' || s[n - 1] == '\t')) {
		s[--n] = '\0';
	}
}

/* Case-insensitive byte compare -- FAT (this server's only filesystem) is
 * case-insensitive, so "SD:/APPS/x" and "sd:/apps/x" name the same file. A
 * case-SENSITIVE gate here would be a real bypass of the allow-list below,
 * not just a cosmetic gap: an attacker who can't write sd:/config.json can
 * still write sd:/CONFIG.JSON and land on the same inode. */
static inline int ci_eq(char a, char b) {
	return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static inline int ci_has_prefix(const char* path, const char* prefix) {
	for(; *prefix; ++path, ++prefix) {
		if(!*path || !ci_eq(*path, *prefix))
			return 0;
	}
	return 1;
}

static inline int ci_streq(const char* a, const char* b) {
	for(; *a && *b; ++a, ++b) {
		if(!ci_eq(*a, *b))
			return 0;
	}
	return *a == *b;
}

/* 2026-08-31 DoS/unauth hardening: this used to be a ".."-blocklist, which
 * let any LAN host that can reach this DEV-ONLY tool's port read or
 * overwrite ANY file under sd:/ (including files this tool never touches --
 * priiloader config, other homebrew apps, the game's own save data outside
 * its normal write path) and, via CWRUN1, chainload an arbitrary uploaded
 * .dol as native code. This is now an ALLOW-LIST scoped to exactly the
 * targets the real PC-side drivers (the PS1 scripts under tools, see
 * stage_wii_dol.ps1 et al) and this file's own defaults use -- everything
 * else is rejected. Enumerated by grepping those PS1 drivers for
 * TargetPath/RemotePath literals plus this file's own CW_STAGE_MANIFEST_PATHS
 * table and prepare_run_ready() defaults:
 *
 *   sd:/apps/<name>/...   stage_wii_dol.ps1's -TargetPath default
 *                         sd:/apps/CavEX/boot.dol, its
 *                         sd:/apps/CavEX/cw_run_args.txt (also this
 *                         file's own CWRUNPREP1/CWRUN1 fallback dol/args
 *                         paths), and run_staged_wii_hardware.ps1's app-dir
 *                         fallback telemetry (sd:/apps/CavEX/
 *                         hang_report_N.txt, sd:/apps/CavEX/
 *                         fbguard_report.txt); run_wii_lighting_bench.ps1's
 *                         -TargetPath default sd:/apps/CaveWiiLightingBench/
 *                         boot.dol (a DIFFERENT app dir than CaveWii_v2 --
 *                         this is why the allow-list is the whole
 *                         sd:/apps/ tree, not literally just
 *                         sd:/apps/CavEX/); install_wii_proof_
 *                         launcher.ps1's own sd:/apps/CaveWiiStage/ tree
 *                         (today written by a direct card-reader copy, not
 *                         over this protocol, but kept open for a future
 *                         network reinstall and to keep CWFETCH1/CWMANIFEST1
 *                         able to read proof_launcher_manifest.json there).
 *   sd:/texturepacks/...  the 2026-08-30 splash-fix path this file's own
 *                         mkdir_parents() comment documents:
 *                         sd:/texturepacks/<pack>/title/splashes.txt and
 *                         sibling pack assets (PROJECT_MEMORY.md 2026-08-30
 *                         "SPLASH SIDE-QUEST" entry).
 *   sd:/assets/...        the game's TEXTURE PACK tree, anchored to the SD
 *                         ROOT (config.json's paths.texturepack default
 *                         "assets", commit 4b9caf4) -- e.g.
 *                         sd:/assets/art/kz.png for the painting atlas. This
 *                         is public game data the same as sd:/texturepacks/
 *                         above (shipped pack contents, never a secret), so
 *                         it needs the same CWSTAGE-write / CWFETCH1-read
 *                         access; added 2026-09-02 after the painting atlas
 *                         staged fine under sd:/apps/CavEX/assets/art/
 *                         but the game never looked there.
 *   exact root files      everything in CW_STAGE_MANIFEST_PATHS
 *                         (stage_manifest_paths.h) -- the boot-trace/hang/
 *                         fbguard/lighting-bench telemetry this tool already
 *                         declares as "known" via CWMANIFEST1, reused here as
 *                         the single source of truth instead of a second,
 *                         driftable copy of the same filename list. Covers
 *                         sd:/CAVEWII_PROOF_LAUNCHER.json,
 *                         sd:/cw_boot_trace.txt, sd:/hang_report.txt (plus
 *                         the numbered seq-suffix reports up to
 *                         WD_REPORT_SEQ_MAX), sd:/fbguard_report.txt, and
 *                         sd:/lighting_bench.txt.
 *
 * The ".." substring check is kept as a cheap belt-and-braces layer even
 * though the prefixes above already can't reach outside their own trees; a
 * bare "//" is also rejected since no legitimate driver-built path ever
 * contains an empty path segment. */
static inline int valid_sd_path(const char* path) {
	if(!path || strncmp(path, "sd:/", 4) != 0)
		return 0;
	if(strstr(path, "..") || strstr(path, "//"))
		return 0;

	/* The auth secret itself is never network-readable or -writable through
	 * this protocol, in any auth state -- otherwise an unauthenticated
	 * client could CWFETCH1 it to learn the token, or CWSTAGE-write it to
	 * plant a known one, once it IS provisioned. Checked ahead of the
	 * sd:/apps/ prefix below, which would otherwise allow it. */
	if(ci_streq(path, CW_STAGE_TOKEN_PATH) ||
	   ci_streq(path, CW_STAGE_TOKEN_LEGACY_PATH))
		return 0;

	if(ci_has_prefix(path, "sd:/apps/") ||
	   ci_has_prefix(path, "sd:/texturepacks/") ||
	   ci_has_prefix(path, "sd:/beta/") ||
	   ci_has_prefix(path, "sd:/assets/"))
		return 1;

	for(size_t i = 0; i < CW_STAGE_MANIFEST_PATH_COUNT; ++i) {
		if(ci_streq(path, CW_STAGE_MANIFEST_PATHS[i]))
			return 1;
	}
	return 0;
}

#endif
