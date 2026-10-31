/*
	Copyright (c) 2023 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

/* OSK widget + create-world dialog pure logic
 * (docs/ui_design/osk_create_world_2026-08-15.md section 6).
 *
 * Every golden here is a HAND-COMPUTED constant from the decompile's
 * semantics (the POISONED-GOLDEN rule: never derive expected values by
 * re-running the unit under test):
 *   - folder derivation  [740c583 GuiCreateWorld.calcSaveDirName +
 *     ChatAllowedCharacters.allowedCharactersArray]
 *   - seed resolution    [740c583 GuiCreateWorld.actionPerformed]:
 *     Java 6 Long.parseLong (NO '+' sign -- that leniency is Java 7+,
 *     after Beta) and Java String.hashCode (h = 31*h + c, int32 wrap,
 *     sign-extended by the (long) cast)
 *   - edit semantics     [740c583 GuiTextField.textboxKeyTyped]: append-
 *     only, backspace, printable filter, `len < max || max == 0`
 *   - "-" folder dedupe  [740c583 GuiCreateWorld.generateUnusedFolderName]
 *   - LevelName persistence (the TAG_STRING archive writer the dialog
 *     introduced -- nothing wrote the tag before). */

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#define GETPID() _getpid()
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define RMDIR(p) rmdir(p)
#define GETPID() getpid()
#endif

#include "../source/game/gui/screen.h"
#include "../source/game/gui/sign_osk_model.h"
#include "../source/game/gui/widget_osk.h"
#include "../source/network/level_archive.h"

/* ---- S1: osk_feed_char edit semantics ---------------------------------- */

static void test_feed_semantics(void) {
	char buf[8];
	struct osk_state o;

	/* append + NUL discipline */
	buf[0] = 0;
	osk_reset(&o, buf, sizeof(buf), 0);
	assert(osk_feed_char(&o, 'a') == OSK_EV_CHANGED);
	assert(osk_feed_char(&o, 'b') == OSK_EV_CHANGED);
	assert(strcmp(buf, "ab") == 0);

	/* backspace removes the LAST char only; empty is a no-op */
	assert(osk_feed_char(&o, '\b') == OSK_EV_CHANGED);
	assert(strcmp(buf, "a") == 0);
	assert(osk_feed_char(&o, '\b') == OSK_EV_CHANGED);
	assert(osk_feed_char(&o, '\b') == OSK_EV_NONE);
	assert(buf[0] == 0);

	/* control events */
	assert(osk_feed_char(&o, '\t') == OSK_EV_NEXT);
	assert(osk_feed_char(&o, '\r') == OSK_EV_DONE);
	assert(osk_feed_char(&o, '\n') == OSK_EV_DONE);
	assert(buf[0] == 0); /* none of those touched the buffer */

	/* printable filter: 31 and 127 rejected, 32 and 126 accepted */
	assert(osk_feed_char(&o, (char)31) == OSK_EV_NONE);
	assert(osk_feed_char(&o, (char)127) == OSK_EV_NONE);
	assert(osk_feed_char(&o, ' ') == OSK_EV_CHANGED);
	assert(osk_feed_char(&o, '~') == OSK_EV_CHANGED);
	assert(strcmp(buf, " ~") == 0);

	/* Beta max rule [740c583 GuiTextField.textboxKeyTyped]:
	 * `length < max || max == 0` */
	buf[0] = 0;
	osk_reset(&o, buf, sizeof(buf), 3);
	assert(osk_feed_char(&o, 'x') == OSK_EV_CHANGED);
	assert(osk_feed_char(&o, 'y') == OSK_EV_CHANGED);
	assert(osk_feed_char(&o, 'z') == OSK_EV_CHANGED);
	assert(osk_feed_char(&o, 'w') == OSK_EV_NONE); /* at max: rejected */
	assert(strcmp(buf, "xyz") == 0);

	/* max == 0: unbounded up to the BUFFER (cap-1 chars) */
	buf[0] = 0;
	osk_reset(&o, buf, sizeof(buf), 0);
	for(int k = 0; k < 7; k++)
		assert(osk_feed_char(&o, '0' + k) == OSK_EV_CHANGED);
	assert(osk_feed_char(&o, '9') == OSK_EV_NONE); /* buffer floor */
	assert(strlen(buf) == 7);

	fprintf(stderr, "S1 feed semantics OK\n");
}

/* ---- S1b: sign screen's Beta-15 clamp (maintainer ruling 2026-08-17) --- */

static void test_sign_line_gate(void) {
	/* screen_sign.c wires SIGN_BETA_MAX_CHARS -- NOT the 16-wide grid --
	 * into osk_reset, so the shared osk_feed_char mutator (same S1 mechanism
	 * above) enforces exactly [740c583 GuiEditSign.keyTyped]'s
	 * `signText[editLine].length() < 15` for the sign screen: character-
	 * count only. Verified against the decompile: GuiEditSign never calls
	 * FontRenderer.getStringWidth, so there is no pixel-width term -- the
	 * maintainer ruling's own "verify, not trust" dual-gate expectation does
	 * not hold here, and this test pins the single character-count gate
	 * instead of a fabricated width check. */
	char buf[SIGN_OSK_COLS + 1]; /* mirrors screen_sign.c's shadow_line cap */
	struct osk_state o;

	buf[0] = 0;
	osk_reset(&o, buf, sizeof(buf), SIGN_BETA_MAX_CHARS);
	for(int k = 0; k < SIGN_BETA_MAX_CHARS - 1; k++)
		assert(osk_feed_char(&o, (char)('a' + (k % 26))) == OSK_EV_CHANGED);
	assert(strlen(buf) == (size_t)(SIGN_BETA_MAX_CHARS - 1)); /* 14 so far */

	/* the 15th character (bringing length 14 -> 15) is accepted */
	assert(osk_feed_char(&o, 'Z') == OSK_EV_CHANGED);
	assert(strlen(buf) == (size_t)SIGN_BETA_MAX_CHARS);
	assert(buf[SIGN_BETA_MAX_CHARS - 1] == 'Z');

	/* the 16th character is refused -- Beta's own gate, not the 16-column
	 * grid width (SIGN_OSK_COLS, still 1 larger than buf's used length) */
	assert(osk_feed_char(&o, 'Q') == OSK_EV_NONE);
	assert(strlen(buf) == (size_t)SIGN_BETA_MAX_CHARS);
	assert(buf[SIGN_BETA_MAX_CHARS - 1] == 'Z'); /* not overwritten by 'Q' */

	fprintf(stderr, "S1b sign line gate OK\n");
}

/* ---- S2: folder derivation --------------------------------------------- */

static void expect_folder(const char* in, const char* want) {
	char out[96];
	screen_cworld_test_derive_folder(in, out, sizeof(out));
	if(strcmp(out, want) != 0) {
		fprintf(stderr, "derive_folder('%s') = '%s', want '%s'\n", in, out,
				want);
		assert(0);
	}
}

static void test_folder_derivation(void) {
	/* Java trim strips <= 0x20 from both ends; interior spaces SURVIVE
	 * (Beta replaces none of them). */
	expect_folder("My World", "My World");
	expect_folder("  My World  ", "My World");
	expect_folder("", "World");
	expect_folder("   ", "World");

	/* each forbidden char -> '_' [740c583 ChatAllowedCharacters
	 * .allowedCharactersArray -- the FORBIDDEN filename set] */
	expect_folder("a/b", "a_b");
	expect_folder("a:b?", "a_b_");
	expect_folder("x`y", "x_y");
	expect_folder("x*y", "x_y");
	expect_folder("x\\y", "x_y");
	expect_folder("x<y>z", "x_y_z");
	expect_folder("x|y", "x_y");
	expect_folder("x\"y", "x_y");
	/* control chars: trim eats them at the ENDS; interior ones are in the
	 * forbidden set */
	expect_folder("a\tb", "a_b");
	expect_folder("a\nb", "a_b");
	expect_folder("a\rb", "a_b");
	expect_folder("a\fb", "a_b");

	/* replacement happens AFTER trim, BEFORE the empty check: all-slashes
	 * becomes underscores, not "World" [calcSaveDirName order] */
	expect_folder("///", "___");

	fprintf(stderr, "S2 folder derivation OK\n");
}

/* ---- S3: seed resolution ----------------------------------------------- */

static void expect_seed(const char* text, bool want_set, int64_t want) {
	int64_t out = 0;
	bool set = screen_cworld_test_resolve_seed(text, &out);
	if(set != want_set || (set && out != want)) {
		fprintf(stderr,
				"resolve_seed('%s') = (%d, %" PRId64 "), want (%d, %" PRId64
				")\n",
				text, (int)set, out, (int)want_set, want);
		assert(0);
	}
}

static void test_seed_resolution(void) {
	/* empty -> random [740c583 GuiCreateWorld.actionPerformed] */
	expect_seed("", false, 0);

	/* parseLong path; 0 keeps the random seed (the `!= 0L` gate) */
	expect_seed("0", false, 0);
	expect_seed("-0", false, 0);
	expect_seed("007", true, 7);
	expect_seed("-1", true, -1);
	expect_seed("9223372036854775807", true, INT64_MAX);
	expect_seed("-9223372036854775808", true, INT64_MIN);

	/* hashCode path: hand-computed Java String.hashCode constants.
	 * "a" = 97; "ab" = 31*97+98 = 3105;
	 * "Glacier": 71,108,97,99,105,101,114 chained = 1772835215;
	 * "aaaaaa" wraps int32: h5 = 92567585, h6 = 31*92567585+97
	 *   = 2869595232 -> int32 = -1425372064 (sign-extended);
	 * "12x" = ((49*31)+50)*31+120 = 48759;
	 * " 12" (leading space fails parseLong) = ((32*31)+49)*31+50 = 32321;
	 * "+5" -- Java 6 parseLong REJECTS '+' (leniency is Java 7+, after
	 *   Beta) -> hashCode = 43*31+53 = 1386. */
	expect_seed("a", true, 97);
	expect_seed("ab", true, 3105);
	expect_seed("Glacier", true, 1772835215);
	expect_seed("aaaaaa", true, -1425372064);
	expect_seed("12x", true, 48759);
	expect_seed(" 12", true, 32321);
	expect_seed("+5", true, 1386);

	/* out-of-range numerics are NumberFormatException in Java -> hashCode.
	 * Every hashCode result is a sign-extended int32 -- assert the
	 * property (the exact 19-digit chain is impractical to hand-compute). */
	{
		int64_t out = 0;
		assert(screen_cworld_test_resolve_seed("9223372036854775808", &out));
		assert((int64_t)(int32_t)out == out);
	}

	fprintf(stderr, "S3 seed resolution OK\n");
}

/* ---- S4: page tables ---------------------------------------------------- */

static void test_page_tables(void) {
	/* feed accepts the whole printable range */
	char buf[4];
	struct osk_state o;
	for(int c = 32; c <= 126; c++) {
		buf[0] = 0;
		osk_reset(&o, buf, sizeof(buf), 0);
		assert(osk_feed_char(&o, (char)c) == OSK_EV_CHANGED);
		assert(buf[0] == (char)c && buf[1] == 0);
	}

	/* REACHABILITY: on the Wii the OSK is the only text input, so every
	 * printable ASCII char must appear in some page cell (space comes from
	 * the function row). Duplicates across pages are by design (digits +
	 * punctuation repeat on the Shift page); a non-printable table entry
	 * is a typo. */
	bool seen[128] = {false};
	seen[' '] = true; /* the function-row Space key */
	for(int p = 0; p < OSK_PAGES; p++) {
		for(int r = 0; r < OSK_ROWS; r++) {
			for(int c = 0; c < OSK_COLS; c++) {
				char ch = osk_page_char(p, r, c);
				if(ch == 0)
					continue;
				assert(ch >= 32 && ch <= 126);
				seen[(int)ch] = true;
			}
		}
	}
	for(int c = 32; c <= 126; c++) {
		if(!seen[c]) {
			fprintf(stderr, "OSK pages cannot type '%c' (%d)\n", (char)c, c);
			assert(0);
		}
	}
	fprintf(stderr, "S4 printable coverage OK\n");
}

/* ---- S5: "-" dedupe via the mkdir reservation --------------------------- */

static void test_beta_reservation(void) {
	char saves[128];
	snprintf(saves, sizeof(saves), "tmp_osk_saves_%ld", (long)GETPID());

	char r1[256], r2[256], r3[256];
	assert(screen_sworld_test_reserve_beta(saves, "World", r1, sizeof(r1)));
	assert(screen_sworld_test_reserve_beta(saves, "World", r2, sizeof(r2)));
	assert(screen_sworld_test_reserve_beta(saves, "World", r3, sizeof(r3)));

	size_t n1 = strlen(r1), n2 = strlen(r2), n3 = strlen(r3);
	assert(strcmp(r1 + n1 - 5, "World") == 0);
	assert(strcmp(r2 + n2 - 6, "World-") == 0);
	assert(strcmp(r3 + n3 - 7, "World--") == 0);

	/* the reservation IS the occupation: all three exist as directories */
	struct stat st;
	assert(stat(r1, &st) == 0 && S_ISDIR(st.st_mode));
	assert(stat(r3, &st) == 0 && S_ISDIR(st.st_mode));

	(void)RMDIR(r1);
	(void)RMDIR(r2);
	(void)RMDIR(r3);
	(void)RMDIR(saves);
	fprintf(stderr, "S5 beta reservation OK\n");
}

/* ---- S6: LevelName round-trip (the TAG_STRING writer) ------------------- */

static void test_levelname_roundtrip(void) {
	char dirbuf[128];
	snprintf(dirbuf, sizeof(dirbuf), "tmp_osk_world_%ld", (long)GETPID());
	assert(MKDIR(dirbuf) == 0);

	string_t dir;
	string_init_set_str(dir, dirbuf);

	struct level_archive la;
	vec3 spawn = {8.0F, 70.0F, 8.0F};
	assert(level_archive_create_new(&la, dir, 12345, spawn));
	/* the RAW typed display name (Beta does not trim it) */
	assert(level_archive_write(&la, LEVEL_NAME, (void*)" My World "));
	assert(level_archive_save(&la));
	level_archive_discard(&la);

	struct level_archive re;
	assert(level_archive_create(&re, dir));
	char name[64] = {0};
	assert(level_archive_read(&re, LEVEL_NAME, name, sizeof(name)));
	assert(strcmp(name, " My World ") == 0);
	level_archive_discard(&re);

	string_clear(dir);

	char lvl[160];
	snprintf(lvl, sizeof(lvl), "%s/level.dat", dirbuf);
	(void)remove(lvl);
	snprintf(lvl, sizeof(lvl), "%s/level.dat_old", dirbuf);
	(void)remove(lvl);
	snprintf(lvl, sizeof(lvl), "%s/level.dat_new", dirbuf);
	(void)remove(lvl);
	(void)RMDIR(dirbuf);
	fprintf(stderr, "S6 LevelName round-trip OK\n");
}

/* ---- S7: LEVEL_GAME_MODE round-trip + missing-tag = survival ------------ */

static void test_game_mode_roundtrip(void) {
	char dirbuf[128];
	snprintf(dirbuf, sizeof(dirbuf), "tmp_osk_mode_%ld", (long)GETPID());
	assert(MKDIR(dirbuf) == 0);

	string_t dir;
	string_init_set_str(dir, dirbuf);

	/* A world created WITHOUT ever writing the tag is survival: the
	 * level.dat template carries no CaveWiiGameMode, and the reader's
	 * absent-tag rule is what makes every pre-2026-09-01 world survival
	 * (docs/ui_design/create_world_console_2026-09-01.md section 3.1). */
	struct level_archive la;
	vec3 spawn = {8.0F, 70.0F, 8.0F};
	assert(level_archive_create_new(&la, dir, 4242, spawn));
	assert(!level_archive_is_creative(&la));
	assert(level_archive_save(&la));
	level_archive_discard(&la);

	struct level_archive legacy;
	assert(level_archive_create(&legacy, dir));
	assert(!level_archive_is_creative(&legacy)); /* MISSING TAG = SURVIVAL */
	level_archive_discard(&legacy);

	/* Creative round-trip: the create-if-absent writer migrates the very
	 * tree that just proved it had no tag, and the value survives a full
	 * close/reopen cycle (the acceptance-(a) power-cycle property). */
	struct level_archive up;
	assert(level_archive_create(&up, dir));
	assert(level_archive_write_game_mode(&up, true));
	assert(level_archive_is_creative(&up)); /* visible before the flush */
	assert(level_archive_save(&up));
	level_archive_discard(&up);

	struct level_archive re;
	assert(level_archive_create(&re, dir));
	assert(level_archive_is_creative(&re));
	/* and back down to survival on an already-present tag (the coerce path) */
	assert(level_archive_write_game_mode(&re, false));
	assert(!level_archive_is_creative(&re));
	assert(level_archive_save(&re));
	level_archive_discard(&re);

	struct level_archive re2;
	assert(level_archive_create(&re2, dir));
	assert(!level_archive_is_creative(&re2));
	level_archive_discard(&re2);

	string_clear(dir);

	char lvl[160];
	snprintf(lvl, sizeof(lvl), "%s/level.dat", dirbuf);
	(void)remove(lvl);
	snprintf(lvl, sizeof(lvl), "%s/level.dat_old", dirbuf);
	(void)remove(lvl);
	snprintf(lvl, sizeof(lvl), "%s/level.dat_new", dirbuf);
	(void)remove(lvl);
	(void)RMDIR(dirbuf);
	fprintf(stderr, "S7 game-mode round-trip OK\n");
}

int main(void) {
	test_feed_semantics();
	test_sign_line_gate();
	test_folder_derivation();
	test_seed_resolution();
	test_page_tables();
	test_beta_reservation();
	test_levelname_roundtrip();
	test_game_mode_roundtrip();
	fprintf(stderr, "test_osk_env: all OK\n");
	return 0;
}
