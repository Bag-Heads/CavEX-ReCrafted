/* test_entity_direct_colour_bracket -- source lint (hw1023c).
 *
 * gfx_draw_quads_flt() feeds GX a DIRECT CLR0 colour per vertex, while the
 * entity pass runs with the INDEXED CLR0 descriptor that gfx_lighting(true)
 * sets up. A direct-colour draw outside a gfx_lighting(false) ...
 * gfx_lighting(true) bracket streams vertices the GPU decodes with the wrong
 * layout: the GX FIFO desyncs and the Wii GPU wedges, every frame
 * (tmp/jobs/hw1023c/diag-dj/DIAG.md: source/entity/entity_painting.c drew
 * its quads unbracketed). PC GL does not care, so only a lint catches it.
 *
 * Rule checked: in every source/entity/ *.c and source/graphics/render_entity*.c
 * file, each gfx_draw_quads_flt( call is preceded, in the SAME function body,
 * by gfx_lighting(false) with no gfx_lighting(true) in between, and is
 * followed, in the same function body, by gfx_lighting(true).
 *
 * Usage: test_entity_direct_colour_bracket [source_root]
 * (default: CW_SOURCE_ROOT, the repo's source/ folder). A root pointing at a
 * scratch copy with the bracket removed must FAIL -- that is how the guard
 * itself is proven.
 */
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CW_SOURCE_ROOT
#define CW_SOURCE_ROOT "source"
#endif

/* Blank out comments, string and char literals (newlines kept, so line
 * numbers still match the file). */
static void strip_c(char* s) {
	size_t n = strlen(s);
	for(size_t i = 0; i < n; i++) {
		if(s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
			while(i < n && s[i] != '\n')
				s[i++] = ' ';
		} else if(s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
			s[i] = s[i + 1] = ' ';
			i += 2;
			while(i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) {
				if(s[i] != '\n')
					s[i] = ' ';
				i++;
			}
			if(i < n) {
				s[i] = ' ';
				if(i + 1 < n)
					s[++i] = ' ';
			}
		} else if(s[i] == '"' || s[i] == '\'') {
			char q = s[i++];
			while(i < n && s[i] != q && s[i] != '\n') {
				if(s[i] == '\\' && i + 1 < n)
					s[i++] = ' ';
				s[i++] = ' ';
			}
		} else if(s[i] == '#') {
			/* a preprocessor line: no braces from it count */
			bool line_start = true;
			for(size_t k = i; k > 0; k--) {
				if(s[k - 1] == '\n')
					break;
				if(!isspace((unsigned char)s[k - 1])) {
					line_start = false;
					break;
				}
			}
			if(line_start)
				while(i < n && s[i] != '\n') {
					if(s[i] == '\\' && i + 1 < n && s[i + 1] == '\n') {
						s[i] = ' ';
						i += 2;
						continue;
					}
					s[i++] = ' ';
				}
		}
	}
}

static bool ident_at(const char* s, size_t i, const char* id) {
	size_t l = strlen(id);
	if(strncmp(s + i, id, l))
		return false;
	if(i > 0 && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))
		return false;
	return !(isalnum((unsigned char)s[i + l]) || s[i + l] == '_');
}

/* After identifier `id` at i: "( arg )" -- returns 1 for true, 0 for false,
 * -1 for anything else. */
static int bool_arg(const char* s, size_t i, const char* id) {
	size_t k = i + strlen(id);
	while(isspace((unsigned char)s[k]))
		k++;
	if(s[k] != '(')
		return -1;
	k++;
	while(isspace((unsigned char)s[k]))
		k++;
	int v = -1;
	if(!strncmp(s + k, "true", 4) || (s[k] == '1' && !isalnum((unsigned char)s[k + 1]))) {
		v = 1;
		k += s[k] == '1' ? 1 : 4;
	} else if(!strncmp(s + k, "false", 5)
			  || (s[k] == '0' && !isalnum((unsigned char)s[k + 1]))) {
		v = 0;
		k += s[k] == '0' ? 1 : 5;
	} else
		return -1;
	while(isspace((unsigned char)s[k]))
		k++;
	return s[k] == ')' ? v : -1;
}

/* the self-test's planted faults are expected: no messages for them */
static bool g_quiet;
#define LINT_MSG(...) \
	do { \
		if(!g_quiet) \
			fprintf(stderr, __VA_ARGS__); \
	} while(0)

struct scan_result {
	unsigned calls;	 /* gfx_draw_quads_flt calls seen */
	unsigned faults; /* calls that break the rule */
};

static unsigned line_of(const char* s, size_t i) {
	unsigned l = 1;
	for(size_t k = 0; k < i; k++)
		l += s[k] == '\n';
	return l;
}

/* Scan one (unstripped) text. `name` is for the messages. */
static struct scan_result scan_text(const char* name, const char* text) {
	struct scan_result r = {0, 0};
	char* s = strdup(text);
	assert(s);
	strip_c(s);
	size_t n = strlen(s);
	int depth = 0;
	bool off = false;		   /* gfx_lighting(false) in force */
	unsigned pending = 0;	   /* bracketed calls awaiting gfx_lighting(true) */
	size_t pending_at[64];
	for(size_t i = 0; i < n; i++) {
		char c = s[i];
		if(c == '{') {
			if(depth == 0) {
				off = false;
				pending = 0;
			}
			depth++;
		} else if(c == '}') {
			depth--;
			if(depth == 0) {
				/* end of a function body: anything still open was never
				 * restored to the indexed descriptor */
				for(unsigned k = 0; k < pending; k++) {
					LINT_MSG(
							"  %s:%u: gfx_draw_quads_flt has no "
							"gfx_lighting(true) after it in the same function\n",
							name, line_of(s, pending_at[k]));
					r.faults++;
				}
				pending = 0;
				off = false;
			}
		} else if(depth > 0 && ident_at(s, i, "gfx_lighting")) {
			int v = bool_arg(s, i, "gfx_lighting");
			if(v == 0)
				off = true;
			else if(v == 1) {
				off = false;
				pending = 0;
			} else {
				LINT_MSG(
						"  %s:%u: gfx_lighting() with a non-literal argument: "
						"the bracket cannot be checked\n",
						name, line_of(s, i));
				r.faults++;
			}
		} else if(ident_at(s, i, "gfx_draw_quads_flt")) {
			r.calls++;
			if(depth == 0) {
				/* a prototype or a macro use outside a body: not a call */
				r.calls--;
			} else if(!off) {
				LINT_MSG(
						"  %s:%u: gfx_draw_quads_flt without a preceding "
						"gfx_lighting(false) in the same function\n",
						name, line_of(s, i));
				r.faults++;
			} else if(pending < 64u)
				pending_at[pending++] = i;
		}
	}
	free(s);
	return r;
}

static char* read_file(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* buf = malloc((size_t)len + 1u);
	assert(buf);
	size_t got = fread(buf, 1, (size_t)len, f);
	buf[got] = 0;
	fclose(f);
	return buf;
}

static bool ends_with(const char* s, const char* suf) {
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && !strcmp(s + a - b, suf);
}

/* Scan dir for *.c whose name starts with `prefix` ("" = all). */
static void scan_dir(const char* root, const char* sub, const char* prefix,
					 struct scan_result* total, unsigned* files,
					 unsigned* painting_calls) {
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/%s", root, sub);
	DIR* d = opendir(dir);
	if(!d) {
		fprintf(stderr, "FAIL: cannot open %s\n", dir);
		total->faults++;
		return;
	}
	struct dirent* e;
	while((e = readdir(d)) != NULL) {
		if(!ends_with(e->d_name, ".c")
		   || strncmp(e->d_name, prefix, strlen(prefix)))
			continue;
		char path[1400];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		char* text = read_file(path);
		if(!text) {
			fprintf(stderr, "FAIL: cannot read %s\n", path);
			total->faults++;
			continue;
		}
		char name[400];
		snprintf(name, sizeof(name), "%s/%s", sub, e->d_name);
		struct scan_result r = scan_text(name, text);
		free(text);
		total->calls += r.calls;
		total->faults += r.faults;
		(*files)++;
		if(!strcmp(e->d_name, "entity_painting.c"))
			*painting_calls += r.calls;
	}
	closedir(d);
}

/* The detector itself, on synthetic bodies. */
static void self_test(void) {
	struct scan_result r;
	g_quiet = true;
	r = scan_text("ok", "void f(void){ gfx_lighting(false);\n"
						"  for(int i=0;i<2;i++){ gfx_draw_quads_flt(4,a,b,c); }\n"
						"  gfx_lighting(true); }\n");
	assert(r.calls == 1 && r.faults == 0);
	/* THE hw1023c painting shape: no bracket at all */
	r = scan_text("bare", "void f(void){ gfx_bind_texture(t);\n"
						  "  gfx_draw_quads_flt(4,a,b,c); }\n");
	assert(r.calls == 1 && r.faults == 1);
	/* off but never restored */
	r = scan_text("open", "void f(void){ gfx_lighting(false);\n"
						  "  gfx_draw_quads_flt(4,a,b,c); }\n");
	assert(r.faults == 1);
	/* restored BEFORE the draw */
	r = scan_text("early", "void f(void){ gfx_lighting(false); "
						   "gfx_lighting(true);\n gfx_draw_quads_flt(4,a,b,c); }\n");
	assert(r.faults == 1);
	/* the bracket lives in ANOTHER function: not good enough */
	r = scan_text("split", "void g(void){ gfx_lighting(false); }\n"
						   "void f(void){ gfx_draw_quads_flt(4,a,b,c); "
						   "gfx_lighting(true); }\n");
	assert(r.faults == 1);
	/* comments, strings and prototypes are not calls or brackets */
	r = scan_text("noise", "void gfx_draw_quads_flt(int n);\n"
						   "/* gfx_draw_quads_flt(4) */ // gfx_lighting(false)\n"
						   "void f(void){ puts(\"gfx_draw_quads_flt(\"); }\n");
	assert(r.calls == 0 && r.faults == 0);
	r = scan_text("comment-bracket",
				  "void f(void){ /* gfx_lighting(false); */\n"
				  "  gfx_draw_quads_flt(4,a,b,c);\n  gfx_lighting(true); }\n");
	assert(r.faults == 1);
	g_quiet = false;
	printf("PASS self-test: the detector flags the bare, unrestored, "
		   "early-restored and cross-function shapes, and ignores comments, "
		   "strings and prototypes\n");
}

int main(int argc, char** argv) {
	const char* root = argc > 1 ? argv[1] : CW_SOURCE_ROOT;
	self_test();
	struct scan_result total = {0, 0};
	unsigned files = 0, painting_calls = 0;
	scan_dir(root, "entity", "", &total, &files, &painting_calls);
	scan_dir(root, "graphics", "render_entity", &total, &files,
			 &painting_calls);
	printf("scanned %u files under %s: %u gfx_draw_quads_flt calls, %u "
		   "unbracketed\n",
		   files, root, total.calls, total.faults);
	/* A guard that scans nothing proves nothing: the painting call that
	 * wedged hw1023c must be among the calls checked. */
	if(painting_calls == 0u) {
		fprintf(stderr, "FAIL: entity/entity_painting.c not scanned or has "
						"no gfx_draw_quads_flt call\n");
		return 1;
	}
	if(total.faults) {
		fprintf(stderr, "FAIL: %u direct-colour draw(s) outside a "
						"gfx_lighting(false)/(true) bracket (GX FIFO desync "
						"on the Wii, hw1023c)\n",
				total.faults);
		return 1;
	}
	printf("PASS every gfx_draw_quads_flt in entity/ and render_entity* sits "
		   "inside gfx_lighting(false) ... gfx_lighting(true) in its own "
		   "function\n");
	return 0;
}
