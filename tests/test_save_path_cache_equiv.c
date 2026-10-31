/* P01: real load/edit/checkpoint, exhaustive full-path call census. Linker
 * wrappers affect this executable only. Freeze time at the libc boundary so
 * EVERY saved byte, including timestamps and journals, can be compared. */
#include "block/blocks.h"
#include "network/save_path.h"
#include <pthread.h>
#include <dirent.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include "network/level_archive.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "support/test_fixture_dir.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <zlib.h>
struct save_baseline_file { const char* path; size_t offset, length; };
#include "fixtures/p01_save_bytes.h"
#include "fixtures/p03_wii_save_bytes.h"
#ifdef _WIN32
#include <direct.h>
#endif

enum { REGION, JOURNAL, LEVEL, OTHER, GROUPS };
enum { OPEN, STAT, RENAME, REMOVE, MKDIR, OPS };
static uint32_t counts[GROUPS][OPS];
static bool counting;
static const char* fail_stat_path;
static const char* fail_open_path;
static const char* names[] = {"region", "journal", "level", "other"};
static void count(const char* path, unsigned op) {
	if(!counting) return;
	unsigned group = strstr(path, ".idx") ? JOURNAL
		: strstr(path, ".mcr") ? REGION
		: strstr(path, "level.dat") ? LEVEL : OTHER;
	counts[group][op]++;
}
static void select_world_file(const char* path);
static char logical_cwd[SAVE_PATH_MAX];
static char* logical_getcwd(char* out,size_t size) {
	if(strlen(logical_cwd) >= size) { errno=ERANGE; return NULL; }
	memcpy(out,logical_cwd,strlen(logical_cwd)+1); return out;
}
static int native_chdir(const char* path) {
#ifdef _WIN32
	return _chdir(path);
#else
	return chdir(path);
#endif
}
/* Exclusive archive creation uses open/_open, not fopen. Cover it too;
 * unlike Wii native cwd, the host harness chdir changes actual POSIX cwd. */
#ifdef _WIN32
int __real__open(const char*,int,...);
int __wrap__open(const char* path,int flags,...) {
	int mode=0;
	if(flags & _O_CREAT) { va_list ap; va_start(ap,flags); mode=va_arg(ap,int); va_end(ap); }
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p=save_path_prepare(path,buf);
	int fd=p ? __real__open(p,flags,mode) : -1;
	save_path_end(); return fd;
}
int (*__wrap___imp__open)(const char*,int,...)=__wrap__open;
#else
int __real_open(const char*,int,...);
int __wrap_open(const char* path,int flags,...) {
	int mode=0;
	if(flags & O_CREAT) { va_list ap; va_start(ap,flags); mode=va_arg(ap,int); va_end(ap); }
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p=save_path_prepare(path,buf);
	int fd=p ? __real_open(p,flags,mode) : -1;
	save_path_end(); return fd;
}
#endif
FILE* __real_fopen(const char*, const char*);
FILE* __wrap_fopen(const char* path, const char* mode) {
	count(path, OPEN);
	if(strstr(path,"/level.dat")) select_world_file(path);
	if(fail_open_path && !strcmp(path, fail_open_path)) {
		errno = EACCES;
		return NULL;
	}
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p = save_path_prepare(path,buf);
	FILE* rc = p ? __real_fopen(p, mode) : NULL;
	save_path_end(); return rc;
}
static bool fail_stat(const char* path) {
	if(fail_stat_path && !strcmp(path, fail_stat_path)) {
		errno = EACCES;
		return true;
	}
	return false;
}
#ifndef _WIN32
int __real_stat(const char*, struct stat*);
int __wrap_stat(const char* path, struct stat* st) {
	count(path, STAT);
	if(fail_stat(path)) return -1;
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p = save_path_prepare(path,buf);
	int rc = p ? __real_stat(p, st) : -1;
	save_path_end(); return rc;
}
#endif
int __real_rename(const char*, const char*);
int __wrap_rename(const char* from, const char* to) {
	count(from, RENAME); /* one API call, two resolved paths */
	save_path_begin(); char a[SAVE_PATH_MAX],b[SAVE_PATH_MAX];
	const char *p=NULL,*q=NULL;
	bool prepared=save_path_prepare_pair(from,to,a,b,&p,&q);
	int rc=prepared ? __real_rename(p,q) : -1;
	save_path_end(); return rc;
}
int __real_remove(const char*);
int __wrap_remove(const char* path) {
	count(path, REMOVE);
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p = save_path_prepare(path,buf);
	int rc = p ? __real_remove(p) : -1;
	save_path_end(); return rc;
}
#ifdef _WIN32
int __real__mkdir(const char*);
int __wrap__mkdir(const char* path) {
	count(path, MKDIR);
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p = save_path_prepare(path,buf);
	int rc = p ? __real__mkdir(p) : -1;
	save_path_end(); return rc;
}
int (*__wrap___imp__mkdir)(const char*) = __wrap__mkdir;
/* MinGW's stat declaration aliases this symbol, including inline callers. */
int __real_stat64i32(const char*, struct stat*);
int __wrap_stat64i32(const char* path, struct stat* st) {
	count(path, STAT);
	if(fail_stat(path)) return -1;
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p = save_path_prepare(path,buf);
	int rc = p ? __real_stat64i32(p, st) : -1;
	save_path_end(); return rc;
}
#define MAKE_DIR(p) _mkdir(p)
#else
int __real_mkdir(const char*, mode_t);
int __wrap_mkdir(const char* path, mode_t mode) {
	count(path, MKDIR);
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p = save_path_prepare(path,buf);
	int rc = p ? __real_mkdir(p, mode) : -1;
	save_path_end(); return rc;
}
#define MAKE_DIR(p) mkdir(p, 0777)
#endif
time_t __wrap_time(time_t* out) {
	time_t fixed = 1700000000;
	if(out) *out = fixed;
	return fixed;
}
#ifdef _WIN32
__time64_t __wrap__time64(__time64_t* out) {
	__time64_t fixed = 1700000000;
	if(out) *out = fixed;
	return fixed;
}
#endif
DIR* __real_opendir(const char*);
DIR* __wrap_opendir(const char* path) {
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p=save_path_prepare(path,buf);
	DIR* d=p ? __real_opendir(p) : NULL;
	save_path_end(); return d;
}
static void select_world_file(const char* path) {
	/* Same selector used by the real Wii metadata flush, with real bytes. */
	save_path_set_parent(path);
}
static void begin(void) {
	memset(counts, 0, sizeof counts);
	counting = true;
}
static void report(const char* phase) {
	counting = false;
	for(unsigned g = 0; g < GROUPS; g++)
		printf("OPS %s %s open=%u stat=%u rename=%u remove=%u mkdir=%u\n",
			phase, names[g], counts[g][OPEN], counts[g][STAT],
			counts[g][RENAME], counts[g][REMOVE], counts[g][MKDIR]);
}

#define REGIONS 8
#define CHUNKS 4
/* P01 base, with ONLY the lead-authorized P03 SizeOnDisk correction.
 * P03-golden-correction.json proves all other NBT/file bytes unchanged. */
/* PAR-BED-META: CaveWiiSaveVersion 4 -> 5 in level.dat/_old only (fixtures/p01_save_bytes.h note). */
/* PLAYER-HEALTH-NBT: Player.Health 160 -> 20 + CaveWiiHealth (+SizeOnDisk) in level.dat/_old only (fixtures/p01_save_bytes.h note). */
/* save-enc-1030 (2026-10-30, fast re-pin ruling): Beta save encoding (hw1030a
 * NBT diff of the real Beta world dj_orig) -- empty Entities/TileEntities lists
 * element type 1 (NBTTagList.java:25-31); LastUpdate = world time, per-class
 * mob tags and double Pos/Motion do not touch this corpus. Reverting only the
 * list-type byte reproduces the old pin 0x21fedee787a7e558 exactly; new value 8/8 runs. */
#define GOLDEN UINT64_C(0x6e0b63f23aa4dad3)
static uint64_t fold(uint64_t h, const char* path) {
	FILE* f = fopen(path, "rb");
	assert(f);
	uint8_t bytes[4096];
	size_t n;
	while((n = fread(bytes, 1, sizeof bytes, f)))
		for(size_t i = 0; i < n; i++) {
			h ^= bytes[i];
			h *= UINT64_C(1099511628211);
		}
	assert(!ferror(f));
	assert(fclose(f) == 0);
	return h;
}

static void compare_saved_files(const char* level,
	const struct save_baseline_file* files, size_t count,
	const unsigned char* packed, size_t packed_size, uLongf length) {
	uLongf expected_length = length;
	uint8_t* expected = malloc(length);
	assert(expected);
	assert(uncompress(expected, &length, packed,
		packed_size) == Z_OK && length == expected_length);
	for(size_t k = 0; k < count; k++) {
		char path[512];
		snprintf(path, sizeof path, "%s/%s", level, files[k].path);
		FILE* f = fopen(path, "rb");
		assert(f);
		uint8_t bytes[4096];
		size_t offset = 0;
		while(offset < files[k].length) {
			size_t n = files[k].length - offset;
			if(n > sizeof bytes) n = sizeof bytes;
			assert(fread(bytes, 1, n, f) == n);
			assert(memcmp(bytes, expected + files[k].offset + offset, n) == 0);
			offset += n;
		}
		assert(fgetc(f) == EOF && !ferror(f));
		assert(fclose(f) == 0);
	}
	free(expected);
	printf("PASS memcmp %zu full saved files / %lu bytes, including timestamps: %s\n",
		count, (unsigned long)length, level);
}

/* The optimization must retain the two safety decisions the redundant stat
 * originally guarded: missing files never evict, unreadable existing files
 * never turn into freshly generated chunks. Also pin checked-read admission. */
static void check_probe_failures(const char* root) {
	string_t level;
	string_init_printf(level, "%s/probe", root);
	assert(MAKE_DIR(string_get_cstr(level)) == 0);
	struct server_world w = {0};
	server_world_create(&w, level, WORLD_DIM_OVERWORLD, 4242, true);
	struct region_pool* p = server_world_region_pool(&w);
	for(int r = 0; r < REGION_POOL_SLOTS; r++) {
		struct region_archive* ra = NULL;
		assert(region_pool_acquire(p, &w, r, 0, true, &ra) == STORAGE_OK);
		assert(ra);
	}
	uint32_t evictions = p->evictions;
	for(size_t k = 0; k < server_world_region_count(&w); k++)
		region_pool_pin(p, server_world_region_at(&w, k));
	struct server_chunk* sc = NULL;
	begin();
	assert(server_world_load_chunk(&w, (REGION_POOL_SLOTS + 1) * 32, 0, &sc) && sc);
	counting = false;
	assert(counts[REGION][STAT] == 1 && counts[REGION][OPEN] == 0);
	assert(p->evictions == evictions && p->busy_refusals == 0);
	for(size_t k = 0; k < server_world_region_count(&w); k++)
		region_pool_unpin(p, server_world_region_at(&w, k));
	/* A nonresident, existing empty file allows precise stat/open failure
 	 * injection without corrupting a real region or relying on ACL behavior. */
	struct region_archive other;
	assert(region_archive_create_new(&other, level, 20, 0, WORLD_DIM_OVERWORLD));
	assert(region_archive_destroy(&other));
	char path[512];
	snprintf(path, sizeof path, "%s/region/r.20.0.mcr", string_get_cstr(level));
	fail_stat_path = path;
	sc = NULL;
	assert(!server_world_load_chunk(&w, 640, 0, &sc) && !sc);
	assert(!server_world_is_chunk_loaded(&w, 640, 0));
	assert(p->evictions == evictions);
	fail_stat_path = NULL;
	fail_open_path = path;
	assert(!server_world_load_chunk(&w, 640, 0, &sc) && !sc);
	assert(!server_world_is_chunk_loaded(&w, 640, 0));
	assert(p->evictions == evictions + 1);
	fail_open_path = NULL;
	begin();
	struct region_archive* ra = NULL;
	assert(server_world_chunk_region_checked(&w, 640, 0, &ra)
		== SERVER_WORLD_REGION_OK && ra);
	counting = false;
	assert(counts[REGION][STAT] == 1 && counts[REGION][OPEN] == 1);
	/* Default-off profiling does not advance its counters. */
	uint32_t before[3], after[3];
	region_archive_path_profile_stats(before);
	assert(region_pool_acquire(p, &w, 0, 0, false, &ra) == STORAGE_OK);
	region_archive_path_profile_stats(after);
	assert(after[0] == before[0] && after[1] == before[1] && after[2] == before[2]);
	server_world_destroy(&w);
	string_clear(level);
	puts("PASS absent-before-eviction, stat/open failures, checked admission, profile-off");
}

static void check_size_sidecars(const char* root) {
	string_t level;
	string_init_printf(level, "%s/size-sidecars", root);
	assert(MAKE_DIR(string_get_cstr(level)) == 0);
	struct level_archive la;
	assert(level_archive_create_new(&la, level, 4242, (vec3){0, 64, 0}));
	assert(level_archive_save(&la));
	const char* dirs[] = {"region", "DIM-1", "DIM-1/region"};
	char path[512];
	for(size_t i = 0; i < 3; i++) {
		snprintf(path, sizeof path, "%s/%s", string_get_cstr(level), dirs[i]);
		assert(MAKE_DIR(path) == 0);
	}
	const char* regions[] = {"region/r.0.0.mcr", "DIM-1/region/r.-1.-1.mcr"};
	int64_t expected = level_archive_disk_size(&la);
	for(size_t i = 0; i < 2; i++) {
		snprintf(path, sizeof path, "%s/%s", string_get_cstr(level), regions[i]);
		FILE* f = fopen(path, "wb");
		assert(f && fwrite("beta-region", 1, 11, f) == 11);
		assert(fclose(f) == 0);
	}
	expected += 22;
	assert(level_archive_disk_size(&la) == expected);
	/* P03 lead ruling: only Beta world files count. CFR McRegionChunkLoader
	 * saveChunk updates WorldInfo from RegionFileCache's region growth;
	 * CaveWii journal/index sidecars never participate in Beta's counter. */
	const char* sidecars[] = {"region/r.0.0.mcr.idx",
		"region/r.0.0.mcr.idx_new", "DIM-1/region/r.-1.-1.mcr.idx",
		"DIM-1/region/r.-1.-1.mcr.idx_new", "region/unrelated",
		"DIM-1/region/r.0.0.mcr.bak", "region/x"};
	for(size_t i = 0; i < sizeof sidecars / sizeof sidecars[0]; i++) {
		snprintf(path, sizeof path, "%s/%s", string_get_cstr(level), sidecars[i]);
		FILE* f = fopen(path, "wb");
		assert(f && fwrite("excluded", 1, 8, f) == 8);
		assert(fclose(f) == 0);
	}
	assert(level_archive_disk_size(&la) == expected);
	/* A matching suffix alone cannot make a directory a region file. */
	snprintf(path, sizeof path, "%s/region/directory.mcr", string_get_cstr(level));
	assert(MAKE_DIR(path) == 0);
	assert(level_archive_disk_size(&la) == expected);
	level_archive_discard(&la);
	string_clear(level);
	puts("PASS SizeOnDisk includes both dimensions, unchanged with journal/index sidecars");
}

/* Saved-world replay of the split_dims_6m anchors in RSAVE rs_a1: both
 * dimensions straddle region (0,0), near radius 2, far radius 6. Stream
 * centre-out Chebyshev rings, interleaving dimensions to exercise the shared
 * pool. This is a storage working-set replay, not the portal/client driver.
 * Far reads remain unmodified; one SAVE edits only the 5x5 near squares. */
static void check_wii_window(const char* root) {
	string_t level;
	string_init_printf(level, "%s/wii-window", root);
	assert(MAKE_DIR(string_get_cstr(level)) == 0);
	struct level_archive la;
	assert(level_archive_create_new(&la, level, 12345, (vec3){0.5, 4, 0.5}));
	assert(level_archive_save(&la));
	level_archive_destroy(&la);
	struct server_chunk seed;
	server_chunk_init(&seed);
	assert(server_chunk_arrays_alloc(&seed));
	memset(seed.ids, BLOCK_AIR, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT);
	memset(seed.ids, BLOCK_STONE, CHUNK_SIZE * CHUNK_SIZE * 4);
	memset(seed.metadata, 0, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
	memset(seed.lighting_sky, 0xff, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
	memset(seed.lighting_torch, 0, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
	memset(seed.heightmap, 4, CHUNK_SIZE * CHUNK_SIZE);
	for(int d = 0; d < 2; d++)
		for(int rz = -1; rz <= 0; rz++)
			for(int rx = -1; rx <= 0; rx++) {
				struct region_archive ra;
				assert(region_archive_create_new(&ra, level, rx, rz,
					d ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD));
				for(int z = -6; z <= 6; z++)
					for(int x = -6; x <= 6; x++) {
						if(CHUNK_REGION_COORD(x) != rx || CHUNK_REGION_COORD(z) != rz)
							continue;
						assert(region_archive_set_chunk(&ra, x, z, &seed, NULL, NULL));
					}
				assert(region_archive_destroy(&ra));
			}
	server_world_chunk_destroy(&seed);
	struct server_local* session = calloc(1, sizeof(*session));
	assert(session);
	struct region_pool* pool = &session->region_pool;
	region_pool_init(pool);
	for(int d = 0; d < 2; d++) {
		struct server_world* w = &session->worlds[d];
		server_world_create(w, level,
			d ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD, 12345, true);
		w->session = session;
		w->world_index = (uint8_t)d;
	}
	begin();
	assert(level_archive_create(&la, level));
	for(int radius = 0; radius <= 6; radius++)
		for(int z = -radius; z <= radius; z++)
			for(int x = -radius; x <= radius; x++) {
				if(abs(x) != radius && abs(z) != radius) continue;
				for(int d = 0; d < 2; d++) {
					struct server_chunk* sc = NULL;
					/* R5: replay bare storage residency; decoration must not
					 * mutate the frozen synthetic save-byte corpus. */
					assert(server_world_test_load_structural_chunk(
						&session->worlds[d], x, z, &sc) && sc);
					if(radius <= 2) {
						sc->ids[5] = BLOCK_BRICKS;
						sc->modified = true;
					}
				}
			}
	report("wii_load");
	uint32_t load_evictions = pool->evictions;
	printf("WII-WORKSET near=2 far=6 load distinct=8 chunks=338 slots=%d evictions=%u peak=%u\n",
		REGION_POOL_SLOTS, load_evictions, pool->open_handles_peak);
	begin();
	for(int d = 0; d < 2; d++) {
		assert(server_world_checkpoint_chunks(&session->worlds[d]));
		assert(server_world_sync_regions(&session->worlds[d]));
	}
	uint64_t size = level_archive_disk_size(&la);
	assert(level_archive_write(&la, LEVEL_DISK_SIZE, &size));
	assert(level_archive_save(&la));
	report("wii_save");
	printf("WII-WORKSET save distinct=8 edited_chunks=50 slots=%d evictions=%u\n",
		REGION_POOL_SLOTS, pool->evictions - load_evictions);
	if(REGION_POOL_SLOTS >= 8) assert(load_evictions == 0 && pool->evictions == 0);
	for(int d = 0; d < 2; d++) server_world_destroy(&session->worlds[d]);
	region_pool_destroy(pool);
	level_archive_destroy(&la);
	free(session);
	compare_saved_files(string_get_cstr(level), p03_wii_baseline_files,
		sizeof p03_wii_baseline_files / sizeof p03_wii_baseline_files[0],
		p03_wii_baseline_z, sizeof p03_wii_baseline_z, P03_WII_BASELINE_BYTES);
	string_clear(level);
}


struct peer_args { char dir[640], file[700]; };
static char audio_relative[96];
static void* audio_peer(void* unused) {
	(void)unused;
	for(unsigned i=0;i<400;i++) {
		FILE* f=__wrap_fopen(audio_relative,"rb"); assert(f);
		assert(fgetc(f)=='A'); assert(fgetc(f)==EOF); assert(fclose(f)==0);
	}
	return NULL;
}
static void* world_peer(void* opaque) {
	struct peer_args* a=opaque;
	for(unsigned i=0;i<400;i++) {
		save_path_set_world(a->dir);
		FILE* f=__wrap_fopen(a->file,"rb"); assert(f);
		assert(fgetc(f)=='W'); assert(fgetc(f)==EOF); assert(fclose(f)==0);
	}
	return NULL;
}
static void check_peers(const char* root) {
	struct peer_args a;
	snprintf(a.dir,sizeof a.dir,"%s/cache-peer",root);
	assert(MAKE_DIR(a.dir)==0);
	snprintf(a.file,sizeof a.file,"%s/ticket",a.dir);
	FILE* f=__wrap_fopen(a.file,"wb"); assert(f); assert(fputc('W',f)=='W'); assert(fclose(f)==0);
	/* A long absolute suffix must fall back, never overflow the rewrite buffer. */
	char longpath[2*SAVE_PATH_MAX],prepared[SAVE_PATH_MAX];
	int prefix=snprintf(longpath,sizeof longpath,"%s/",a.dir); assert(prefix>0);
	memset(longpath+prefix,'q',sizeof longpath-(size_t)prefix-1);
	longpath[sizeof longpath-1]=0;
	save_path_set_world(a.dir); save_path_begin(); errno=EAGAIN;
	assert(save_path_prepare(longpath,prepared)==longpath); assert(errno==EAGAIN);
	save_path_end();
	snprintf(audio_relative,sizeof audio_relative,"p04-audio-%lu.bin",(unsigned long)getpid());
	f=__wrap_fopen(audio_relative,"wb"); assert(f); assert(fputc('A',f)=='A'); assert(fclose(f)==0);
	pthread_t audio,world;
	assert(pthread_create(&audio,NULL,audio_peer,NULL)==0);
	assert(pthread_create(&world,NULL,world_peer,&a)==0);
	assert(pthread_join(audio,NULL)==0); assert(pthread_join(world,NULL)==0);
	assert(__wrap_remove(audio_relative)==0);
	/* Directory identity mutation must invalidate before a native rename. */
	char renamed[680]; snprintf(renamed,sizeof renamed,"%s-moved",a.dir);
	assert(__wrap_rename(a.dir,renamed)==0);
	errno=0; assert(!__wrap_fopen(a.file,"rb")); assert(errno==ENOENT);
	char newfile[720]; snprintf(newfile,sizeof newfile,"%s/ticket",renamed);
	f=__wrap_fopen(newfile,"rb"); assert(f); assert(fgetc(f)=='W'); assert(fclose(f)==0);
	assert(__wrap_remove(newfile)==0);
#ifdef _WIN32
	assert(_rmdir(renamed)==0);
#else
	assert(rmdir(renamed)==0);
#endif
	puts("PASS concurrent unrelated relative audio + world I/O; ancestor rename invalidation; missing-world fallback");
}
int main(int argc, char** argv) {
	assert(getcwd(logical_cwd,sizeof logical_cwd));
	for(char* p=logical_cwd;*p;p++) if(*p=='\\') *p='/';
	assert(save_path_init(native_chdir,logical_getcwd));
	char root[256];
	bool measure = argc == 3 && strcmp(argv[2], "--measure") == 0;
	assert(argc <= 2 || measure);
	if(argc >= 2) {
		assert(strlen(argv[1]) < sizeof root);
		snprintf(root, sizeof root, "%s", argv[1]);
	} else {
		test_fixture_dir("test_save_path_cache_equiv_tmp", root, sizeof root);
	}
	/* Native PowerShell argv uses backslashes; exercise clean cacheable paths. */
	for(char* p=root;*p;p++) if(*p=='\\') *p='/';
	assert(MAKE_DIR(root) == 0);
	check_peers(root);
	check_size_sidecars(root);
	/* Deep/full ROOT; no user saves. Directory enumeration is deliberately
 	 * excluded from the measured window, as in the Wii packet. */
	for(int i = 0; i < 80; i++) {
		char path[320];
		snprintf(path, sizeof path, "%s/dummy-%02d", root, i);
		assert(MAKE_DIR(path) == 0);
	}
	string_t level;
	string_init_printf(level, "%s/world", root);
	save_path_set_world(string_get_cstr(level));
	assert(MAKE_DIR(string_get_cstr(level)) == 0);
	struct level_archive la;
	assert(level_archive_create_new(&la, level, 4242, (vec3){0, 64, 0}));
	assert(level_archive_save(&la));
	level_archive_destroy(&la);
	/* Four corners in each dimension: eight saved regions, interleaved at
 	 * load. Use real archives to seed distinct authoritative chunk data. */
	for(int r = 0; r < REGIONS; r++) {
		struct region_archive ra;
		enum world_dim dim = r & 1 ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
		int rx = (r / 2) % 2 - 1, rz = (r / 4) - 1;
		assert(region_archive_create_new(&ra, level, rx, rz, dim));
		for(int k = 0; k < CHUNKS; k++) {
			struct server_chunk sc;
			server_chunk_init(&sc);
			assert(server_chunk_arrays_alloc(&sc));
			memset(sc.ids, (r + k) % 2 ? BLOCK_STONE : BLOCK_DIRT,
				CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT);
			memset(sc.metadata, 0, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
			memset(sc.lighting_sky, 0, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
			memset(sc.lighting_torch, 0, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
			memset(sc.heightmap, 128, CHUNK_SIZE * CHUNK_SIZE);
			assert(region_archive_set_chunk(&ra, rx * 32 + k, rz * 32, &sc, NULL, NULL));
			server_world_chunk_destroy(&sc);
		}
		assert(region_archive_destroy(&ra));
	}
	uint32_t disabled_stats[3];
	assert(!region_archive_work_profile_enabled());
	region_archive_path_profile_stats(disabled_stats);
	assert(disabled_stats[0] == 0 && disabled_stats[1] == 0 && disabled_stats[2] == 0);
	struct server_world worlds[2] = {0};
	struct server_local* session = calloc(1, sizeof(*session));
	assert(session);
	struct region_pool* pool = &session->region_pool;
	region_pool_init(pool);
	for(int d = 0; d < 2; d++) {
		server_world_create(&worlds[d], level,
			d ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD, 4242, true);
		worlds[d].session = session;
		worlds[d].region_pool = pool;
		worlds[d].world_index = (uint8_t)d;
	}
	string_t file;
	string_init_printf(file, "%s/level.dat", string_get_cstr(level));
	begin();
	region_archive_work_profile_enable(true);
	assert(level_archive_create(&la, level));
	for(int k = 0; k < CHUNKS; k++)
		for(int r = 0; r < REGIONS; r++) {
			int x = ((r / 2) % 2 - 1) * 32 + k, z = (r / 4 - 1) * 32;
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&worlds[r & 1], x, z, &sc) && sc);
			assert(sc->ids[0] == ((r + k) % 2 ? BLOCK_STONE : BLOCK_DIRT));
			sc->ids[5] = BLOCK_BRICKS;
			sc->modified = true;
		}
	/* Frontier neighbours in an absent region: no disk admission is allowed,
 	 * but the old load path resolves the same missing path twice per chunk. */
	for(int d = 0; d < 2; d++)
		for(int k = 0; k < 8; k++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&worlds[d], 64 + k, 0, &sc) && sc);
		}
	uint32_t path_stats[3];
	region_archive_path_profile_stats(path_stats);
	assert(path_stats[0] == counts[REGION][OPEN] + counts[JOURNAL][OPEN]);
	assert(counts[REGION][STAT] == (REGION_POOL_SLOTS >= REGIONS
		? REGIONS : REGIONS * CHUNKS) + 16);
	report("load");
	printf("POOL slots=%d peak=%u evictions=%u archive=%zu slot=%zu pool=%zu\n",
		REGION_POOL_SLOTS, pool->open_handles_peak, pool->evictions,
		sizeof(struct region_archive), sizeof(struct region_pool_slot), sizeof(*pool));
	uint32_t load_evictions = pool->evictions;
	printf("WORKSET load existing=8 absent=2 requests=48 slots=%d evictions=%u\n",
		REGION_POOL_SLOTS, load_evictions);
	/* Checkpoint each world, settle before level.dat, then its standard disk
 	 * size scan + safe rotation. No changes to ordering or durability. */
	begin();
	for(int d = 0; d < 2; d++) {
		assert(server_world_checkpoint_chunks(&worlds[d]));
		assert(server_world_sync_regions(&worlds[d]));
	}
	uint64_t size = level_archive_disk_size(&la);
	assert(level_archive_write(&la, LEVEL_DISK_SIZE, &size));
	assert(level_archive_save(&la));
	uint32_t saved_path_stats[3];
	region_archive_path_profile_stats(saved_path_stats);
	assert(saved_path_stats[0] - path_stats[0]
		== counts[REGION][OPEN] + counts[JOURNAL][OPEN]);
	printf("PROFILE load opens=%u us=%u; SAVE opens=%u us=%u\n",
		path_stats[0], path_stats[1], saved_path_stats[0] - path_stats[0],
		saved_path_stats[1] - path_stats[1]);
	report("save");
	printf("WORKSET save distinct=10 slots=%d evictions=%u\n",
		REGION_POOL_SLOTS, pool->evictions - load_evictions);
	region_archive_work_profile_enable(false);
	for(int d = 0; d < 2; d++) server_world_destroy(&worlds[d]);
	region_pool_destroy(pool);
	free(session);
	level_archive_destroy(&la);
	uint64_t h = UINT64_C(14695981039346656037);
	for(int r = 0; r < REGIONS; r++) {
		char path[384];
		snprintf(path, sizeof path, "%s/%sregion/r.%d.%d.mcr",
			string_get_cstr(level), r & 1 ? "DIM-1/" : "",
			(r / 2) % 2 - 1, r / 4 - 1);
		h = fold(h, path);
	}
	for(int d = 0; d < 2; d++) {
		char path[384];
		snprintf(path, sizeof path, "%s/%sregion/r.2.0.mcr",
			string_get_cstr(level), d ? "DIM-1/" : "");
		h = fold(h, path);
	}
	h = fold(h, string_get_cstr(file));
	string_cat_str(file, "_old");
	h = fold(h, string_get_cstr(file));
	printf("SAVE-BYTES full unmasked files: %016" PRIx64 "\n", h);
	if(measure && h != GOLDEN)
		puts("DIVERGENCE from corrected P03 corpus (measurement only; NOT equivalence PASS)");
	else {
		assert(h == GOLDEN);
		compare_saved_files(string_get_cstr(level), p01_baseline_files,
			sizeof p01_baseline_files / sizeof p01_baseline_files[0],
			p01_baseline_z, sizeof p01_baseline_z, P01_BASELINE_BYTES);
	}
	check_probe_failures(root);
	check_wii_window(root);
	string_clear(file);
	string_clear(level);
unsigned cache[3]; save_path_stats(cache);
	printf("PATH-CACHE selects=%u hits=%u fallback=%u\n",cache[0],cache[1],cache[2]);
	assert(cache[1] > 100);
	save_path_begin(); assert(save_path_invalidate(NULL)); save_path_end();
	if(argc < 2) test_fixture_dir_cleanup(root);
	puts(measure ? "COMPLETE census (see equivalence verdict above)"
		: "PASS load/edit/save region and level bytes");
	return 0;
}
