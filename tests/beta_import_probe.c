/* Driver for tests/test_beta_import.py: runs the production importer
 * (beta_import.c + beta_import_jar.c) inside a fake SD root.
 *
 *   beta_import_probe table          print the 55-texture table
 *   beta_import_probe sync   <root>  bi_run_import on the calling thread
 *   beta_import_probe async  <root>  public API: start, poll, selector
 *   beta_import_probe cancel <root>  start, cancel at once, poll
 *
 * <root> holds beta/, texturepacks/ and assets/ like the SD card / game CWD.
 * Prints "STATE <n> <msg>" and "SELECTED <value>"; exit 0 on success. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "platform/beta_import_internal.h"

static char s_selected[256];

static bool record_select(const char* value) {
	snprintf(s_selected, sizeof(s_selected), "%s", value);
	return true;
}

static int poll_to_end(void) {
	bi_progress p;
	for(int i = 0; i < 60000; i++) { /* <= 60 s */
		beta_import_poll(&p);
		if(p.state == BI_DONE || p.state == BI_FAILED
		   || p.state == BI_CANCELLED)
			break;
		usleep(1000);
	}
	beta_import_poll(&p); /* selection happens on this main-thread poll */
	printf("STATE %d %s\n", (int)p.state, p.msg);
	printf("SOUNDS %d %d\n", (int)p.sounds_imported, p.sounds_count);
	if(s_selected[0])
		printf("SELECTED %s\n", s_selected);
	return (int)p.state;
}

int main(int argc, char** argv) {
	if(argc >= 2 && !strcmp(argv[1], "table")) {
		for(size_t i = 0; i < BI_TEXTURE_COUNT; i++)
			printf("%s %s %s %u %u\n", bi_textures[i].dest,
				   bi_textures[i].source[0],
				   bi_textures[i].source[1] ? bi_textures[i].source[1] : "-",
				   bi_textures[i].w, bi_textures[i].h);
		return 0;
	}
	if(argc < 3 || chdir(argv[2]) != 0) {
		fprintf(stderr, "usage: beta_import_probe sync|async|cancel <root>\n");
		return 2;
	}
	if(!strcmp(argv[1], "sync")) {
		struct bi_paths paths;
		bi_default_paths(&paths);
		bi_ctx ctx;
		memset(&ctx, 0, sizeof(ctx));
		bi_state st = bi_run_import(&paths, &ctx);
		printf("STATE %d %s\n", (int)st, ctx.p.msg);
		return st == BI_DONE ? 0 : 1;
	}
	beta_import_set_selector(record_select);
	if(!beta_import_available()) {
		printf("STATE -1 not available\n");
		return 1;
	}
	if(!beta_import_start())
		return 1;
	if(beta_import_start()) { /* second start while busy must refuse */
		printf("DOUBLE START accepted\n");
		return 1;
	}
	if(!strcmp(argv[1], "cancel"))
		beta_import_cancel();
	int st = poll_to_end();
	if(!strcmp(argv[1], "cancel"))
		return st == BI_CANCELLED ? 0 : 1;
	return st == BI_DONE ? 0 : 1;
}
