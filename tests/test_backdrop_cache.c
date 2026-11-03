/*
	BG-BAKE-1028 task 3: the generated menu-backdrop cache (texture.c).

	Real resolver/publisher (texture.c in the test_shared_controller_textures
	link shape: LTO + gc-sections drop the GPU paths) over private pack
	folders: author art is never overwritten or shadowed; the bundled pack
	never bakes; cold publish -> verified warm state; pack-content mutation
	invalidates; every interrupted publication boundary (models a power cut)
	is rejected or falls back to the previous complete pair, never a mixed
	pair; open/write/close/rename/alloc failures and a read-only folder
	report a reason and leave no temporaries; corrupt pairs are rejected;
	two packs keep independent caches across switching.
*/

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "game/game_state.h"
#include "lodepng/lodepng.h"
#include "platform/texture.h"
#include "platform/thread.h"
#include "support/test_fixture_dir.h"
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

struct game_state gstate;


enum { BUNDLED, AUTHOR, CACHED, MISSING, STALE, CORRUPT };
enum { PUBLISHED, E_AUTHOR, E_INPUT, E_ALLOC, E_DIR, E_OPEN, E_WRITE, E_CLOSE,
	   E_RENAME };
enum { OP_ALLOC, OP_OPEN, OP_WRITE, OP_CLOSE, OP_RENAME };

#define W 1024
#define H 576
static uint8_t img_a[W * H * 4], img_b[W * H * 4];
static unsigned service_calls;
static pthread_t service_owner;
static void service_publish(void* ctx) {
	assert(ctx == &service_calls);
	assert(pthread_equal(pthread_self(), service_owner));
	service_calls++;
}

static void* publish_on_worker(void* ctx) {
	assert(tex_backdrop_publish(ctx, img_a, W, H) == PUBLISHED);
	return NULL;
}

static void fill(uint8_t* p, unsigned seed) {
	for(size_t i = 0; i < (size_t)W * H; i++) {
		p[i * 4 + 0] = (uint8_t)(i * 7 + seed);
		p[i * 4 + 1] = (uint8_t)(i >> 5);
		p[i * 4 + 2] = (uint8_t)(seed * 31);
		p[i * 4 + 3] = 255;
	}
}

static bool exists(const char* pack, const char* rel) {
	char p[600];
	snprintf(p, sizeof(p), "%s/%s", pack, rel);
	FILE* f = fopen(p, "rb");
	if(f) fclose(f);
	return f != NULL;
}

static void write_bytes(const char* pack, const char* rel, const void* d,
						size_t n) {
	char p[600];
	snprintf(p, sizeof(p), "%s/%s", pack, rel);
	FILE* f = fopen(p, "wb");
	assert(f);
	assert(fwrite(d, 1, n, f) == n);
	assert(fclose(f) == 0);
}

/* Decodes the cached PNG; 0 = equals a, 1 = equals b, -1 other/absent. */
static int cached_is(const char* pack) {
	char p[600];
	snprintf(p, sizeof(p), "%s/.cavewii/backdrop/menu-background.png", pack);
	unsigned char* img = NULL;
	unsigned w, h;
	if(lodepng_decode32_file(&img, &w, &h, p) || w != W || h != H) {
		free(img);
		return -1;
	}
	int r = !memcmp(img, img_a, sizeof(img_a)) ? 0
		: !memcmp(img, img_b, sizeof(img_b)) ? 1 : -1;
	free(img);
	return r;
}

static void no_temps(const char* pack) {
	assert(!exists(pack, ".cavewii/backdrop/menu-background.png.tmp"));
	assert(!exists(pack, ".cavewii/backdrop/menu-background.id.tmp"));
}

static void mkpack(char* out, size_t size, const char* root, const char* name) {
	snprintf(out, size, "%s/%s", root, name);
	assert(MKDIR(out) == 0);
}

int main(void) {
	char root[128];
	test_fixture_dir("test_backdrop_cache", root, sizeof(root));
	assert(MKDIR(root) == 0);
	fill(img_a, 1);
	fill(img_b, 2);
	/* Service-enabled publication exercises the bounded row encoder on ALL
	 * existing roundtrip, corruption, interrupted-publish and I/O-fault legs. */
	service_owner = pthread_self();
	tex_set_load_service(service_publish, &service_calls);
	char worker_pack[256];
	mkpack(worker_pack, sizeof(worker_pack), root, "worker");
	pthread_t worker;
	assert(!pthread_create(&worker, NULL, publish_on_worker, worker_pack));
	assert(!pthread_join(worker, NULL));
	assert(service_calls == 0); /* the registering thread alone may present */
	assert(cached_is(worker_pack) == 0);
	puts("PASS texture service: worker publication never calls main presenter");

	/* Bundled pack: its own art, never baked. */
	assert(tex_backdrop_state_of("assets") == BUNDLED);
	assert(tex_backdrop_publish("assets", img_a, W, H) == E_AUTHOR);

	/* Author art: shown, never written over or beside. */
	char author[256], gui[300];
	mkpack(author, sizeof(author), root, "author");
	snprintf(gui, sizeof(gui), "%s/gui", author);
	assert(MKDIR(gui) == 0);
	static const char art[] = "author-owned bytes";
	write_bytes(author, "gui/menu-background.png", art, sizeof(art));
	assert(tex_backdrop_state_of(author) == AUTHOR);
	assert(tex_backdrop_publish(author, img_a, W, H) == E_AUTHOR);
	assert(!exists(author, ".cavewii"));
	puts("PASS author/bundled: never baked, author bytes untouched");

	/* Cold -> warm. */
	char pack[256];
	mkpack(pack, sizeof(pack), root, "plain");
	assert(tex_backdrop_state_of(pack) == MISSING);
	assert(tex_backdrop_publish(pack, img_a, 640, 480) == E_INPUT);
	assert(tex_backdrop_publish(pack, NULL, W, H) == E_INPUT);
	assert(tex_backdrop_publish(pack, img_a, W, H) == PUBLISHED);
	assert(service_calls >= H);
	assert(tex_backdrop_state_of(pack) == CACHED && cached_is(pack) == 0);
	no_temps(pack);
	/* the identity record marks generated art and is not author art */
	assert(exists(pack, ".cavewii/backdrop/menu-background.id"));
	assert(!exists(pack, "gui/menu-background.png"));
	puts("PASS cold publish -> verified warm cache (generated, apart from author art)");

	/* Pack-content mutation invalidates; republish restores. */
	uint64_t id0 = tex_backdrop_identity(pack);
	static uint8_t px[16 * 16 * 4];
	memset(px, 0x40, sizeof(px));
	char terr[300];
	snprintf(terr, sizeof(terr), "%s/terrain.png", pack);
	assert(!lodepng_encode32_file(terr, px, 16, 16));
	assert(tex_backdrop_identity(pack) != id0);
	assert(tex_backdrop_state_of(pack) == STALE);
	assert(tex_backdrop_publish(pack, img_b, W, H) == PUBLISHED);
	assert(tex_backdrop_state_of(pack) == CACHED && cached_is(pack) == 1);
	puts("PASS pack mutation -> STALE -> rebaked");

	/* Interrupted publication at every boundary, over a valid older pair. */
	for(unsigned step = 1; step <= 4; step++) {
		assert(tex_backdrop_publish(pack, img_a, W, H) == PUBLISHED);
		tex_backdrop_test_stop_after(step);
		assert(tex_backdrop_publish(pack, img_b, W, H) == PUBLISHED);
		tex_backdrop_test_stop_after(0);
		int st = tex_backdrop_state_of(pack);
		if(step <= 2) /* old complete pair still committed */
			assert(st == CACHED && cached_is(pack) == 0);
		else          /* record withdrawn before the PNG moved */
			assert(st == CORRUPT || st == MISSING);
		/* the next publication recovers over any leftovers */
		assert(tex_backdrop_publish(pack, img_b, W, H) == PUBLISHED);
		assert(tex_backdrop_state_of(pack) == CACHED && cached_is(pack) == 1);
		no_temps(pack);
	}
	puts("PASS interrupted publication at boundaries 1-4: old pair or rejected, never mixed; recovery");

	/* Each failure kind: reason, no temporaries, no half cache. */
	static const struct { int op; int want; } fails[] = {
		{OP_ALLOC, E_ALLOC}, {OP_OPEN, E_OPEN}, {OP_WRITE, E_WRITE},
		{OP_CLOSE, E_CLOSE}, {OP_RENAME, E_RENAME},
	};
	for(size_t k = 0; k < sizeof(fails) / sizeof(fails[0]); k++) {
		char fp[256];
		char name[32];
		snprintf(name, sizeof(name), "fail%zu", k);
		mkpack(fp, sizeof(fp), root, name);
		tex_backdrop_test_fail(fails[k].op, 1);
		int r = tex_backdrop_publish(fp, img_a, W, H);
		tex_backdrop_test_fail(fails[k].op, 0);
		assert(r == fails[k].want);
		assert(tex_backdrop_publish_reason(r)[0]);
		assert(tex_backdrop_state_of(fp) != CACHED);
		no_temps(fp);
		/* explicit retry succeeds */
		assert(tex_backdrop_publish(fp, img_a, W, H) == PUBLISHED);
		assert(tex_backdrop_state_of(fp) == CACHED);
	}
	/* Read-only / unwritable card: the cache folder cannot be made. */
	char ro[256];
	mkpack(ro, sizeof(ro), root, "readonly");
	write_bytes(ro, ".cavewii", "x", 1); /* a file where the folder must go */
	int r = tex_backdrop_publish(ro, img_a, W, H);
	assert(r == E_DIR);
	printf("PASS failures: alloc/open/write/close/rename/read-only -> \"%s\" etc., "
		   "no temporaries, explicit retry publishes\n",
		   tex_backdrop_publish_reason(r));

	/* Corrupt pairs after a "reboot" (fresh state reads only the files). */
	assert(tex_backdrop_publish(pack, img_a, W, H) == PUBLISHED);
	write_bytes(pack, ".cavewii/backdrop/menu-background.png", "trunc", 5);
	assert(tex_backdrop_state_of(pack) == CORRUPT);
	assert(tex_backdrop_publish(pack, img_a, W, H) == PUBLISHED);
	write_bytes(pack, ".cavewii/backdrop/menu-background.id", "garbage\n", 8);
	assert(tex_backdrop_state_of(pack) == CORRUPT);
	char idp[300];
	snprintf(idp, sizeof(idp), "%s/.cavewii/backdrop/menu-background.id", pack);
	assert(remove(idp) == 0);
	assert(tex_backdrop_state_of(pack) == CORRUPT); /* PNG without record */
	assert(tex_backdrop_publish(pack, img_a, W, H) == PUBLISHED);
	/* a record from another recipe/pack content */
	FILE* f = fopen(idp, "r+b");
	assert(f && fseek(f, 0, SEEK_SET) == 0);
	char rec[256] = {0};
	size_t n = fread(rec, 1, sizeof(rec) - 1, f);
	fclose(f);
	char* pk = strstr(rec, "pack ");
	assert(pk);
	pk[5] = pk[5] == '0' ? '1' : '0';
	write_bytes(pack, ".cavewii/backdrop/menu-background.id", rec, n);
	assert(tex_backdrop_state_of(pack) == STALE);
	puts("PASS corrupt/truncated/orphaned/foreign pairs rejected");

	/* Switching between two packs twice keeps both caches independent. */
	char other[256];
	mkpack(other, sizeof(other), root, "other");
	assert(tex_backdrop_publish(pack, img_a, W, H) == PUBLISHED);
	assert(tex_backdrop_publish(other, img_b, W, H) == PUBLISHED);
	for(int round = 0; round < 2; round++) {
		assert(tex_backdrop_state_of(pack) == CACHED && cached_is(pack) == 0);
		assert(tex_backdrop_state_of(other) == CACHED && cached_is(other) == 1);
	}
	puts("PASS two packs, switched twice: independent caches");

	/* Scene-specific filenames share the beach publisher and invalidation. */
 assert(tex_scene_backdrop_state_of(pack, true) == MISSING);
 assert(tex_scene_backdrop_publish(pack, img_b, W, H, true) == PUBLISHED);
 assert(tex_scene_backdrop_state_of(pack, true) == CACHED);
 assert(cached_is(pack) == 0); /* beach was not overwritten */
 assert(exists(pack, ".cavewii/backdrop/nether-background.png"));
 assert(exists(pack, ".cavewii/backdrop/nether-background.id"));
 write_bytes(pack, ".cavewii/backdrop/nether-background.png", "bad", 3);
 assert(tex_scene_backdrop_state_of(pack, true) == CORRUPT);
 assert(tex_backdrop_state_of(pack) == CACHED);
 write_bytes(author, "gui/nether-background.png", art, sizeof(art));
 assert(tex_scene_backdrop_state_of(author, true) == AUTHOR);
 assert(tex_scene_backdrop_publish(author, img_a, W, H, true) == E_AUTHOR);
 puts("PASS Nether: independent cache, corruption rejected, author art preserved");

	test_fixture_dir_cleanup(root);
	puts("test_backdrop_cache: all PASS");
	return 0;
}
