/* Copyright (c) 2026 CaveWii_v2 contributors
 * A1a (split-dimensions-plan-0924, Astra ruling astra-A1a-a item 4): the
 * OCCURRENCE-LEVEL baseline gate for the deprecated server_local `world`
 * alias.
 *
 * `struct server_local` now holds `worlds[SERVER_LOCAL_WORLD_SLOTS]`; `world`
 * survives only as a fixed anonymous-union alias of worlds[0] for the code
 * that used it before A1a. New code must name a world explicitly (a `w` it was
 * given, server_local_world_of(s, actor), a validated handle, or -- for a
 * documented Overworld-only/legacy path -- &s->worlds[0]). This test fails on
 * ANY new use of the alias, including a new use inside a file that already
 * has some: the baseline pins a count per (file, receiver expression), so a
 * file-level allowlist cannot hide growth.
 *
 * WHAT COUNTS. In every .c/.h under source/ and tests/, with comments and
 * string/char literals stripped, each `<receiver>->world` or
 * `<receiver>.world` not followed by an identifier character (so `worlds[`,
 * `world_index`, `world_time` never count). `<receiver>` is the identifier
 * (with any trailing [index]) immediately before the operator. Receivers that
 * are provably NOT a server_local -- the client game state, chunks, entities,
 * camera owners and the other structs listed in NOT_SERVER_LOCAL below, each
 * with its own unrelated `world` member -- are ignored, so other lanes'
 * client code never trips this gate.
 *
 * SPELLINGS (M3, A1a review): whitespace/newlines around the operator
 * (`s -> world`, `s->\n world`), a parenthesised receiver (`(s)->world`,
 * `( s ).world`), a dereferenced one (`(*s).world`) all normalise to the
 * receiver `s`; a call receiver (`get()->world`, `f(a, b).world`) counts as
 * the row `get()` / `f()`; any other parenthesised expression counts as the
 * row `(expr)`. Nothing that ends in `->world` / `.world` is skipped any more
 * except a designated initializer (`.world =` with no receiver).
 *
 * KNOWN LIMIT -- MACROS. The gate scans source text, not the preprocessed
 * translation unit: a macro whose BODY spells `x->world` counts once (row
 * receiver `x`, at the #define) and every later expansion is invisible. Do not
 * hide the alias behind a macro; a reviewer must reject one.
 *
 * RATCHET. A row whose count goes UP fails. A row whose count goes DOWN (or
 * disappears) passes but prints "tighten baseline" with the row, and the
 * baseline must then be regenerated so the removed use cannot come back.
 *
 * ON FAILURE: replace the new use with an explicit world. If a new receiver is
 * really not a server_local, prove its type with a scoped matcher (or a
 * file-scoped, counted exception). Do not increase an existing allowance.
 * R-B07 also recognises exact typed expressions with a live declaration;
 * these narrow matcher proofs never change an occurrence allowance.
 * The baseline (tests/world_alias_baseline.txt) may only ever shrink; a lower
 * count than the baseline is reported so the baseline can be tightened.
 * Regenerate it with:  test_world_alias_gate <repo-root> --print
 */
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char* const NOT_SERVER_LOCAL[] = {
	"gstate",          /* struct game_state: client struct world */
	"c",               /* struct chunk: client struct world* */
	"e",               /* struct entity: void* world (per-entity binding) */
	"entity",          /* struct entity */
	"ent",             /* struct entity (tests) */
	"proxy",           /* struct entity proxy (redstone_piston_server.c) */
	"owner",           /* struct camera owner (camera.c) */
	"body",            /* local player body (main.c) */
	"o",               /* mp pause-menu observation (client) */
	"in",              /* tps observer input (client) */
	"cur",             /* sign text render feed cursor (client) */
	"rows[i]",         /* tps observer rows (client) */
	"light_history[slot]", /* tps observer history (client) */
	"args",            /* test thread-argument structs */
	"tx",              /* populate transaction: struct server_world* world */
	"local_player",    /* gstate.players[i].local_player: client struct entity* body */
};

/* Count each proven non-server_local use; growth still reaches the ratchet. */
static struct {
	const char* file;
	const char* receiver;
	unsigned allowance, seen;
} NOT_SERVER_LOCAL_IN_FILE[] = {
	{"source/game/autopilot.c", "anchor", 1, 0}, /* struct autopilot_portal_return_anchor */
	{"source/game/autopilot.c", "out", 1, 0}, /* struct autopilot_portal_return_anchor */
	{"source/main.c", "s_split_dims_return_anchor", 2, 0}, /* same anchor struct */
	{"tests/test_a2_transfer_transaction.c", "surviving_body", 1, 0}, /* struct entity */
	{"tests/test_a2w_publication.c", "dead_body", 5, 0}, /* struct entity: dead guest client body */
	{"tests/test_b2_world_payloads.c", "fireball", 1, 0}, /* struct entity: client projectile */
	{"tests/test_autopilot_route_rank.c", "a", 1, 0}, /* anchor struct */
	{"source/main.c", "gb", 1, 0}, /* struct entity: guest client body (F1 dispatch) */
	{"tests/test_arrow_env.c", "foreign", 1, 0}, /* struct entity: pinned pig zombie */
	{"tests/test_f1_r4.c", "host", 1, 0}, /* struct entity: client_body(0) */
};

struct row {
	char file[256];
	char receiver[64];
	unsigned count;
};

static struct row* rows_found;
static size_t found_n, found_cap;

static bool ignored(const char* file, const char* receiver) {
	for(size_t i = 0; i < sizeof(NOT_SERVER_LOCAL) / sizeof(NOT_SERVER_LOCAL[0]); i++)
		if(!strcmp(receiver, NOT_SERVER_LOCAL[i]))
			return true;
	for(size_t i = 0; i < sizeof(NOT_SERVER_LOCAL_IN_FILE) / sizeof(NOT_SERVER_LOCAL_IN_FILE[0]); i++)
		if(!strcmp(file, NOT_SERVER_LOCAL_IN_FILE[i].file)
		   && !strcmp(receiver, NOT_SERVER_LOCAL_IN_FILE[i].receiver)) {
			if(NOT_SERVER_LOCAL_IN_FILE[i].seen++ < NOT_SERVER_LOCAL_IN_FILE[i].allowance)
				return true;
			break;
		}
	return false;
}

static void add(const char* file, const char* receiver) {
	for(size_t i = 0; i < found_n; i++)
		if(!strcmp(rows_found[i].file, file)
		   && !strcmp(rows_found[i].receiver, receiver)) {
			rows_found[i].count++;
			return;
		}
	if(found_n == found_cap) {
		found_cap = found_cap ? found_cap * 2 : 256;
		rows_found = realloc(rows_found, found_cap * sizeof(*rows_found));
		assert(rows_found);
	}
	struct row* r = &rows_found[found_n++];
	snprintf(r->file, sizeof(r->file), "%s", file);
	snprintf(r->receiver, sizeof(r->receiver), "%s", receiver);
	r->count = 1;
}

static bool ident(int ch) {
	return isalnum(ch) || ch == '_';
}

/* Blank out comments and string/char literals (newlines kept). */
static void strip(char* t, size_t n) {
	enum { CODE, LINE, BLOCK, STR, CHR } st = CODE;
	for(size_t i = 0; i < n; i++) {
		char ch = t[i];
		char nx = i + 1 < n ? t[i + 1] : 0;
		switch(st) {
		case CODE:
			if(ch == '/' && nx == '/') { st = LINE; t[i] = ' '; }
			else if(ch == '/' && nx == '*') { st = BLOCK; t[i] = ' '; t[++i] = ' '; }
			else if(ch == '"') { st = STR; }
			else if(ch == '\'') { st = CHR; }
			break;
		case LINE:
			if(ch == '\n') st = CODE; else t[i] = ' ';
			break;
		case BLOCK:
			if(ch == '*' && nx == '/') { t[i] = ' '; t[++i] = ' '; st = CODE; }
			else if(ch != '\n') t[i] = ' ';
			break;
		case STR:
		case CHR:
			if(ch == '\\' && i + 1 < n) { t[i] = ' '; if(t[i + 1] != '\n') t[++i] = ' '; }
			else if(ch == (st == STR ? '"' : '\'')) st = CODE;
			else if(ch != '\n') t[i] = ' ';
			break;
		}
	}
}

static bool space(int ch) {
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

/* Index of the '(' / '[' matching the closer at t[close], or SIZE_MAX. */
static size_t match_open(const char* t, size_t close) {
	char cl = t[close], opn = cl == ')' ? '(' : '[';
	size_t depth = 0, k = close + 1;
	while(k > 0) {
		k--;
		if(t[k] == cl) depth++;
		else if(t[k] == opn && --depth == 0) return k;
	}
	return (size_t)-1;
}

/* Copy t[a, b) into out with every whitespace run removed. */
static size_t squeeze(const char* t, size_t a, size_t b, char* out, size_t cap) {
	size_t n = 0;
	for(size_t k = a; k < b; k++)
		if(!space(t[k])) {
			if(n + 1 >= cap) return cap; /* too long */
			out[n++] = t[k];
		}
	out[n] = 0;
	return n;
}

static bool all_ident(const char* s) {
	if(!*s || isdigit((unsigned char)*s)) return false;
	for(; *s; s++)
		if(!ident((unsigned char)*s)) return false;
	return true;
}

/* R-B07: these are typed accesses, not alias allowances. Match the WHOLE
 * expression and its live declaration, not just the final member's name.
 * Declarations are whitespace-normalised; comments/literals are already gone.
 * VSP types/fields are declared in network/vehicle_smoke_probe.h. */
static const struct {
	const char* file;
	const char* expression;
	const char* declaration;
	bool global;
} typed_world_access[] = {
	{"source/game/vehicle_smoke_script.c", "vc->target.world",
		"structvsp_vc*vc", false},
	{"source/game/vehicle_smoke_script.c", "V.targets[K].world",
		"structvsp_rtargets[3];", true},
	{"source/game/vehicle_smoke_script.c", "p->tuple.world",
		"structvsp_p_gameplay_ready*p", false},
	{"source/network/vehicle_smoke_probe.c", "a->world", "structvsp_q*a", false},
	{"source/network/vehicle_smoke_probe.c", "b->world", "structvsp_q*b", false},
	{"source/network/vehicle_smoke_probe.c", "p->u.disk.world",
		"structvsp_p_reload*p", false},
	{"source/network/vehicle_smoke_probe.c", "p->u.inventory.world",
		"structvsp_p_reload*p", false},
	{"source/network/vehicle_smoke_probe.c", "found[k].world",
		"structvsp_rfound[3]", false},
	{"source/network/vehicle_smoke_probe.c", "q.world", "structvsp_qq", false},
	{"source/network/vehicle_smoke_probe.c", "q->world", "structvsp_q*q", false},
	{"source/network/vehicle_smoke_probe.c", "r->world", "structvsp_r*r", false},
	{"source/network/vehicle_smoke_probe.c", "s->world", "structvsp_s*s", false},
	{"source/network/vehicle_smoke_probe.c", "s_fixture_targets[k].world",
		"structvsp_rs_fixture_targets[3];", true},
	{"source/network/vehicle_smoke_probe.c", "t[i].world",
		"structvsp_rt[3]", false},
	{"source/network/vehicle_smoke_probe.c", "o.target.world",
		"structvsp_vo", false},
	{"source/network/vehicle_smoke_probe.c", "p->target.world",
		"structvsp_p_action_result*p", false},
	{"source/network/vehicle_smoke_probe.c", "out->target.world",
		"structvsp_vc*out", false},
	{"source/platform/thread.c", "s_g9_main.light[i].world",
		"struct{constvoid*world;uint64_tseeds[8192],active;}light[2];", true},
	{"source/world.c", "p->world", "structworld_light_pop*p", false},
	{"tests/test_a2w_publication.c", "guest_body->world",
		"structentity*guest_body", false},
	{"tests/test_arrow_env.c", "(*secondary_client)->world",
		"structentity**secondary_client", false},
	{"tests/test_vehicle_client_seams.c", "secondary.world",
		"structentitysecondary", false},
	{"tests/test_vehicle_smoke_script.c", "d.u.dismount.host.world",
		"structvsp_recordd", false},
	/* G9X-1e fast lighting-tape comparer: `world` is the recorded world-object
	 * id of its own tape `Event` struct (fastcmp.c typedef), not server_local. */
	{"tests/oracle/lighting/g9_option_b/compare/fastcmp/fastcmp.c", "be.world",
		"Eventbe={0}", false},
	{"tests/oracle/lighting/g9_option_b/compare/fastcmp/fastcmp.c", "ce.world",
		"Eventbe={0},ce={0}", false},
};

/* A declaration is live only inside its enclosing brace scope. A parameter
 * at file scope belongs to the following function body (never a prototype).
 * This is a deliberately small lexical check, not a general C type parser. */
static size_t enclosing_brace(const char* t, size_t pos) {
	size_t open = (size_t)-1;
	for(size_t k = 0; k < pos; k++) {
		if(t[k] == '{') open = k;
		else if(t[k] == '}' && open != (size_t)-1) {
			/* Find the unmatched enclosing opener again. */
			size_t depth = 1, j = open;
			open = (size_t)-1;
			while(j > 0) {
				j--;
				if(t[j] == '}') depth++;
				else if(t[j] == '{' && --depth == 0) { open = j; break; }
			}
		}
	}
	return open;
}

static bool declaration_live(const char* t, size_t decl, size_t use) {
	size_t open = enclosing_brace(t, decl);
	if(open == (size_t)-1) {
		for(size_t k = decl; k < use; k++) {
			if(t[k] == ';') return false;
			if(t[k] == '{') { open = k; break; }
		}
	}
	if(open == (size_t)-1) return false;
	size_t depth = 0;
	for(size_t k = open; k < use; k++) {
		if(t[k] == '{') depth++;
		else if(t[k] == '}' && --depth == 0) return false;
	}
	return true;
}

/* Global proofs must be actual file-scope declarations. For an anonymous
 * aggregate member, also check the aggregate's variable name: an unrelated
 * struct with a similarly named field is not evidence for V/s_g9_main. */
static bool global_binding(const char* t, size_t decl, size_t len, const char* root) {
	size_t open = enclosing_brace(t, decl);
	if(open == (size_t)-1) {
		const char* name = strstr(t + decl, root);
		return name && (size_t)(name - t) + strlen(root) <= decl + len;
	}
	if(enclosing_brace(t, open) != (size_t)-1 || open < 6
	   || memcmp(t + open - 6, "struct", 6)) return false;
	size_t depth = 1, end = open + 1;
	for(; t[end] && depth; end++) {
		if(t[end] == '{') depth++;
		else if(t[end] == '}') depth--;
	}
	size_t root_len = strlen(root);
	return !depth && !strncmp(t + end, root, root_len) && t[end + root_len] == ';';
}

static bool has_binding(const char* t, size_t use, const char* decl, const char* global_root) {
	for(const char* p = strstr(t, decl); p && (size_t)(p - t) < use;
		p = strstr(p + 1, decl)) {
		size_t end = (size_t)(p - t) + strlen(decl);
		if(ident((unsigned char)decl[strlen(decl) - 1]) && ident((unsigned char)t[end]))
			continue; /* s is not some_longer_identifier */
		if(global_root ? global_binding(t, (size_t)(p - t), strlen(decl), global_root)
			: declaration_live(t, (size_t)(p - t), use)) return true;
	}
	return false;
}

static bool server_local_shadow(const char* t, size_t use, const char* root) {
	const char* type = "structserver_local";
	size_t root_len = strlen(root);
	for(const char* p = strstr(t, type); p && (size_t)(p - t) < use;
		p = strstr(p + 1, type)) {
		const char* name = p + strlen(type);
		/* Pointer depth/qualifiers must not let a local shadow borrow a
		 * surrounding VSP/entity declaration's proof. */
		for(;;) {
			if(*name == '*') name++;
			else if(!strncmp(name, "const", 5)) name += 5;
			else if(!strncmp(name, "volatile", 8)) name += 8;
			else if(!strncmp(name, "restrict", 8)) name += 8;
			else break;
		}
		if(!strncmp(name, root, root_len)
		   && !ident((unsigned char)name[root_len])
		   && declaration_live(t, (size_t)(p - t), use)) return true;
	}
	return false;
}

static bool typed_access(const char* file, const char* compact, size_t end) {
	for(size_t k = 0; k < sizeof(typed_world_access) / sizeof(typed_world_access[0]); k++) {
		if(strcmp(file, typed_world_access[k].file)) continue;
		const char* expr = typed_world_access[k].expression;
		size_t n = strlen(expr);
		if(end < n || memcmp(compact + end - n, expr, n)) continue;
		/* Do not suffix-match another variable/member chain. */
		if(end > n && (ident((unsigned char)compact[end - n - 1])
			|| compact[end - n - 1] == '.' || compact[end - n - 1] == '>')) continue;
		/* A local server_local shadow must never borrow the type proof. */
		char root[64];
		size_t r = 0, start = 0;
		while(expr[start] == '(' || expr[start] == '*') start++;
		while(expr[start + r] && ident((unsigned char)expr[start + r]) && r + 1 < sizeof(root)) {
			root[r] = expr[start + r]; r++;
		}
		root[r] = 0;
		if(!has_binding(compact, end - n, typed_world_access[k].declaration,
			typed_world_access[k].global ? root : NULL)) continue;
		if(r && server_local_shadow(compact, end - n, root)) continue;
		return true;
	}
	return false;
}

/* Normalise the receiver ending just before t[end] (whitespace already
 * skipped by the caller). Returns false for "no receiver" (a designated
 * initializer, a bare `.world` after punctuation). */
static bool receiver_at(const char* t, size_t end, bool arrow, char* out, size_t cap) {
	if(end == 0) return false;
	size_t last = end - 1;
	if(t[last] == ')') {
		size_t open = match_open(t, last);
		if(open == (size_t)-1) { snprintf(out, cap, "(expr)"); return true; }
		size_t k = open;
		while(k > 0 && space(t[k - 1])) k--;
		if(k > 0 && ident(t[k - 1])) { /* call: f(...)->world */
			size_t s0 = k;
			while(s0 > 0 && ident(t[s0 - 1])) s0--;
			char name[48];
			size_t len = k - s0 < sizeof(name) ? k - s0 : sizeof(name) - 1;
			memcpy(name, t + s0, len);
			name[len] = 0;
			snprintf(out, cap, "%s()", name);
			return true;
		}
		char inner[64];
		size_t n = squeeze(t, open + 1, last, inner, sizeof(inner));
		if(n < sizeof(inner) && all_ident(inner)) { snprintf(out, cap, "%s", inner); return true; }
		if(!arrow && n < sizeof(inner) && inner[0] == '*' && all_ident(inner + 1)) {
			snprintf(out, cap, "%s", inner + 1); /* (*s).world == s->world */
			return true;
		}
		snprintf(out, cap, "(expr)");
		return true;
	}
	/* identifier, optionally followed by one [..] */
	size_t start = end;
	if(t[last] == ']') {
		size_t open = match_open(t, last);
		if(open == (size_t)-1) return false;
		start = open;
	}
	while(start > 0 && ident(t[start - 1]))
		start--;
	char receiver[64];
	size_t n = squeeze(t, start, end, receiver, sizeof(receiver));
	if(n == 0 || n >= sizeof(receiver))
		return false;
	if(!ident((unsigned char)receiver[0]) || isdigit((unsigned char)receiver[0]))
		return false;
	snprintf(out, cap, "%s", receiver);
	return true;
}

static void scan_text(const char* rel, char* t, size_t n) {
	strip(t, n);
	char* compact = malloc(n + 1);
	assert(compact);
	squeeze(t, 0, n, compact, n + 1);
	size_t compact_pos = 0;
	for(size_t i = 0; i + 5 <= n; i++) {
		if(i > 0 && !space((unsigned char)t[i - 1])) compact_pos++;
		if(memcmp(t + i, "world", 5) || (i + 5 < n && ident(t[i + 5]))
		   || (i > 0 && ident(t[i - 1])))
			continue;
		size_t k = i;
		while(k > 0 && space(t[k - 1])) k--;
		size_t op;
		bool arrow;
		if(k >= 2 && t[k - 2] == '-' && t[k - 1] == '>') { op = k - 2; arrow = true; }
		else if(k >= 1 && t[k - 1] == '.') { op = k - 1; arrow = false; }
		else continue;
		size_t end = op;
		while(end > 0 && space(t[end - 1])) end--;
		char receiver[64];
		if(!receiver_at(t, end, arrow, receiver, sizeof(receiver)))
			continue; /* designated initializer `.world =` etc. */
		if(typed_access(rel, compact, compact_pos + 5))
			continue;
		if(ignored(rel, receiver))
			continue;
		add(rel, receiver);
	}
	free(compact);
}

static void scan_file(const char* root, const char* rel) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", root, rel);
	FILE* f = fopen(path, "rb");
	assert(f);
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* t = malloc((size_t)n + 1);
	assert(t);
	size_t got = fread(t, 1, (size_t)n, f);
	fclose(f);
	t[got] = 0;
	scan_text(rel, t, got);
	free(t);
}

static bool source_ext(const char* name) {
	size_t n = strlen(name);
	return (n > 2 && (!strcmp(name + n - 2, ".c") || !strcmp(name + n - 2, ".h")));
}

static void walk(const char* root, const char* rel) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", root, rel);
	DIR* d = opendir(path);
	assert(d);
	struct dirent* de;
	while((de = readdir(d))) {
		if(de->d_name[0] == '.')
			continue;
		char child[768];
		snprintf(child, sizeof(child), "%s/%s", rel, de->d_name);
		char full[1024];
		snprintf(full, sizeof(full), "%s/%s", root, child);
		struct stat st;
		if(stat(full, &st))
			continue;
		if(S_ISDIR(st.st_mode))
			walk(root, child);
		else if(source_ext(de->d_name))
			scan_file(root, child);
	}
	closedir(d);
}

static int compare_rows(const void* a, const void* b) {
	const struct row *x = a, *y = b;
	int c = strcmp(x->file, y->file);
	return c ? c : strcmp(x->receiver, y->receiver);
}

static unsigned count_of(const char* receiver) {
	for(size_t i = 0; i < found_n; i++)
		if(!strcmp(rows_found[i].receiver, receiver))
			return rows_found[i].count;
	return 0;
}

static void typed_access_selftest(void) {
	char text[1024];
	for(size_t k = 0; k < sizeof(typed_world_access) / sizeof(typed_world_access[0]); k++) {
		const char* expr = typed_world_access[k].expression;
		const char* decl = typed_world_access[k].declaration;
		const char* file = typed_world_access[k].file;
		/* Exercise every matcher proof, then remove its declaration and move
		 * the identical expression to another file. Neither may be ignored. */
		if(typed_world_access[k].global) {
			if(!strncmp(expr, "V.", 2))
				snprintf(text, sizeof(text), "staticstruct{%s}V;voidf(){use(%s);}", decl, expr);
			else if(!strncmp(expr, "s_g9_main.", 10))
				snprintf(text, sizeof(text), "staticstruct{%s}s_g9_main;voidf(){use(%s);}", decl, expr);
			else
				snprintf(text, sizeof(text), "static%svoidf(){use(%s);}", decl, expr);
		} else
			snprintf(text, sizeof(text), "voidf(){%s;use(%s);}", decl, expr);
		const char* use = strstr(text, "use(") + 4;
		assert(typed_access(file, text, (size_t)(use - text) + strlen(expr)));
		assert(!typed_access("other.c", text, (size_t)(use - text) + strlen(expr)));
		snprintf(text, sizeof(text), "voidf(){use(%s);}", expr);
		use = strstr(text, "use(") + 4;
		assert(!typed_access(file, text, (size_t)(use - text) + strlen(expr)));
	}
	char scoped[] =
		"void ok(const struct vsp_s* s) { use(s->world); }\n"
		"void wrong(struct server_local* s) { use(s->world); }\n"
		"void nested(const struct vsp_s* s) { { struct server_local* s; use(s->world); } }\n"
		"void qualifier(const struct vsp_s* s) { { struct server_local* const s; use(s->world); } }\n"
		"void closed(void) { { struct vsp_s* s; } use(s->world); }\n"
		"void prefix(struct vsp_s* some_other) { use(s->world); }\n"
		"void qualified(struct vsp_vc* out) { use(out->target.world); use(other->target.world); use(out->server->world); }\n"
		"void prototype(struct vsp_s* s); void later(void) { use(s->world); }\n";
	found_n = 0;
	scan_text("source/network/vehicle_smoke_probe.c", scoped, strlen(scoped));
	assert(count_of("s") == 6); /* wrong, nested, qualifier, closed, prefix, prototype */
	assert(count_of("target") == 1); /* other's target is not out's typed field */
	assert(count_of("server") == 1); /* embedded server_local still counted */
	assert(found_n == 3);
	char shadow[] =
		"static struct vsp_r s_fixture_targets[3];\n"
		"void ok(void) { use(s_fixture_targets[k].world); }\n"
		"void wrong(void) { struct server_local s_fixture_targets[3]; use(s_fixture_targets[k].world); }\n";
	found_n = 0;
	scan_text("source/network/vehicle_smoke_probe.c", shadow, strlen(shadow));
	assert(count_of("s_fixture_targets[k]") == 1);
	assert(found_n == 1);
	char unrelated[] =
		"static struct { struct vsp_r targets[3]; } other;\n"
		"void f(void) { use(V.targets[K].world); }\n";
	found_n = 0;
	scan_text("source/game/vehicle_smoke_script.c", unrelated, strlen(unrelated));
	assert(count_of("targets[K]") == 1 && found_n == 1);
	char unrelated_global[] =
		"static struct vsp_r targets[3];\n"
		"void f(void) { use(V.targets[K].world); }\n";
	found_n = 0;
	scan_text("source/game/vehicle_smoke_script.c", unrelated_global, strlen(unrelated_global));
	assert(count_of("targets[K]") == 1 && found_n == 1);
	char dead_global[] =
		"void f(void) { struct vsp_r s_fixture_targets[3]; }\n"
		"void g(void) { use(s_fixture_targets[k].world); }\n";
	found_n = 0;
	scan_text("source/network/vehicle_smoke_probe.c", dead_global, strlen(dead_global));
	assert(count_of("s_fixture_targets[k]") == 1 && found_n == 1);
	char indirect_shadow[] =
		"void f(void) { struct entity** secondary_client;\n"
		"{ struct server_local** secondary_client; use((*secondary_client)->world); } }\n";
	found_n = 0;
	scan_text("tests/test_arrow_env.c", indirect_shadow, strlen(indirect_shadow));
	assert(count_of("(expr)") == 1 && found_n == 1);
	found_n = 0;
	fprintf(stderr, "PASS typed matcher: every proof requires its declaration/file; wrong types, scopes, chains and shadows remain gated\n");
}

/* M3 self-check: every spelling the normaliser claims to catch is caught,
 * and the non-uses stay out. Runs before the tree scan (rows reset after). */
static void spelling_selftest(void) {
	char text[] =
		"a = s->world; b = s -> world; c = s->\n\t world;\n"
		"d = (s)->world; e2 = ( s ).world; f = (*s).world;\n"
		"g = get()->world; h = f2(x, y).world; i = (p + 1)->world;\n"
		"j = arr[k]->world; k2 = t.world;\n"
		"struct x v = { .world = 0 }; w2 = s->worlds[0]; w3 = s->world_index;\n"
		"q = my_world; /* s->world */ r = \"s->world\"; z = e->world;\n";
	found_n = 0;
	scan_text("selftest.c", text, strlen(text));
	assert(count_of("s") == 6);       /* a b c d e2 f */
	assert(count_of("get()") == 1);   /* g */
	assert(count_of("f2()") == 1);    /* h */
	assert(count_of("(expr)") == 1);  /* i */
	assert(count_of("arr[k]") == 1);  /* j */
	assert(count_of("t") == 1);       /* k2 */
	assert(count_of("e") == 0);       /* ignored receiver */
	assert(found_n == 6);             /* no row for .world=, worlds[, world_index, my_world, comment, string */
	found_n = 0;
	char scoped[] = "out->world;";
	scan_text("source/game/autopilot.c", scoped, strlen(scoped));
	assert(found_n == 0);             /* anchor output, not a server_local */
	char grown[] = "out->world;";
	scan_text("source/game/autopilot.c", grown, strlen(grown));
	assert(count_of("out") == 1);     /* a second use still reaches the ratchet */
	found_n = 0;
	char elsewhere[] = "out->world;";
	scan_text("other.c", elsewhere, strlen(elsewhere));
	assert(count_of("out") == 1);     /* exclusion does not mask another file */
	found_n = 0;
	for(size_t i = 0; i < sizeof(NOT_SERVER_LOCAL_IN_FILE) / sizeof(NOT_SERVER_LOCAL_IN_FILE[0]); i++)
		NOT_SERVER_LOCAL_IN_FILE[i].seen = 0;
	fprintf(stderr, "PASS spelling self-check: whitespace, (s), (*s), call and expression receivers normalised\n");
}

int main(int argc, char** argv) {
	assert(argc >= 2);
	spelling_selftest();
	typed_access_selftest();
	const char* root = argv[1];
	bool print = argc >= 3 && !strcmp(argv[2], "--print");
	walk(root, "source");
	walk(root, "tests");
	qsort(rows_found, found_n, sizeof(*rows_found), compare_rows);
	if(print) {
		for(size_t i = 0; i < found_n; i++)
			printf("%s\t%s\t%u\n", rows_found[i].file, rows_found[i].receiver,
				rows_found[i].count);
		return 0;
	}

	char path[1024];
	snprintf(path, sizeof(path), "%s/tests/world_alias_baseline.txt", root);
	FILE* f = fopen(path, "r");
	if(!f) {
		fprintf(stderr, "missing baseline %s\n", path);
		return 1;
	}
	struct row* base = NULL;
	size_t base_n = 0, base_cap = 0;
	char line[512];
	while(fgets(line, sizeof(line), f)) {
		if(line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;
		if(base_n == base_cap) {
			base_cap = base_cap ? base_cap * 2 : 256;
			base = realloc(base, base_cap * sizeof(*base));
			assert(base);
		}
		struct row* r = &base[base_n];
		if(sscanf(line, "%255[^\t]\t%63[^\t]\t%u", r->file, r->receiver, &r->count) == 3)
			base_n++;
	}
	fclose(f);

	unsigned failures = 0, shrinkable = 0, total = 0, base_total = 0;
	for(size_t i = 0; i < base_n; i++)
		base_total += base[i].count;
	for(size_t i = 0; i < found_n; i++) {
		const struct row* r = &rows_found[i];
		total += r->count;
		unsigned allowed = 0;
		for(size_t j = 0; j < base_n; j++)
			if(!strcmp(base[j].file, r->file) && !strcmp(base[j].receiver, r->receiver)) {
				allowed = base[j].count;
				break;
			}
		if(r->count > allowed) {
			fprintf(stderr,
				"NEW legacy server_local world-alias use: %s: `%s->world`/`%s.world` "
				"x%u (baseline allows %u). Name the world explicitly (w, "
				"server_local_world_of, a validated handle, or &s->worlds[0] with "
				"an Overworld-only justification).\n",
				r->file, r->receiver, r->receiver, r->count, allowed);
			failures++;
		} else if(r->count < allowed) {
			printf("tighten baseline: %s\t%s\t%u (baseline %u)\n", r->file,
				r->receiver, r->count, allowed);
			shrinkable++;
		}
	}
	for(size_t j = 0; j < base_n; j++) {
		bool seen = false;
		for(size_t i = 0; i < found_n && !seen; i++)
			seen = !strcmp(base[j].file, rows_found[i].file)
				&& !strcmp(base[j].receiver, rows_found[i].receiver);
		if(!seen) {
			printf("tighten baseline: %s\t%s\t0 (baseline %u)\n", base[j].file,
				base[j].receiver, base[j].count);
			shrinkable++;
		}
	}
	printf("world alias uses: %u now, %u in baseline (%zu rows); "
		   "%u rows could be tightened\n", total, base_total, base_n, shrinkable);
	if(shrinkable)
		printf("tighten baseline: regenerate with --print (rows may only shrink)\n");
	free(base);
	free(rows_found);
	if(failures) {
		fprintf(stderr, "test_world_alias_gate: %u row(s) grew\n", failures);
		return 1;
	}
	puts("PASS test_world_alias_gate: no new use of the deprecated world alias");
	return 0;
}
