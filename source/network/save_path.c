#include "save_path.h"
#include "../platform/thread.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct thread_mutex s_lock;
static bool s_ready;
static int (*s_chdir)(const char*);
static char* (*s_getcwd)(char*, size_t);
static char s_world[SAVE_PATH_MAX], s_active[SAVE_PATH_MAX];
static char s_original[SAVE_PATH_MAX];
static unsigned s_selects, s_hits, s_fallbacks, s_generation;

bool save_path_init(int (*native_chdir)(const char*),
                    char* (*logical_getcwd)(char*, size_t)) {
	if(s_ready) return true;
	if(!native_chdir || !logical_getcwd || !thread_mutex_init(&s_lock))
		return false;
	s_chdir = native_chdir;
	s_getcwd = logical_getcwd;
	s_ready = true; /* boot/test initialization is before threads */
	return true;
}

void save_path_begin(void) {
	if(s_ready) thread_mutex_lock(&s_lock);
}
void save_path_end(void) {
	int saved = errno;
	if(s_ready) thread_mutex_unlock(&s_lock);
	errno = saved;
}

/* No lexical collapse: .., repeated separators, FAT trailing dots/spaces
 * retain the native resolver's semantics. Only clean paths are optimized. */
static bool clean_path(const char* p) {
	if(strchr(p, '\\')) return false;
	while(*p) {
		const char* end = strchr(p, '/');
		size_t n = end ? (size_t)(end - p) : strlen(p);
		if((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')
		   || (n && (p[n-1] == '.' || p[n-1] == ' '))) return false;
		if(!end) break;
		p = end + 1;
		if(*p == '/') return false;
	}
	return true;
}

static const char* absolute_path(const char* p, char out[SAVE_PATH_MAX]) {
	const char* colon = strchr(p, ':');
	if(colon && (colon[1] == '/' || colon[1] == '\\')) return p;
#ifndef PLATFORM_WII
	if(p[0] == '/') return p;
#ifdef _WIN32
	if(p[0] == '\\') return p;
#endif
#endif
	char cwd[SAVE_PATH_MAX];
	if(!s_getcwd(cwd, sizeof cwd)) return p;
	/* Only the current POSIX device can be cached. Other device-relative
	 * requests keep their original native cwd and pass through unchanged. */
	if(colon) {
		const char* basecolon = strchr(cwd, ':');
		if(!basecolon || colon-p != basecolon-cwd
		   || strncmp(p,cwd,(size_t)(colon-p)) != 0) return p;
		p = colon + 1;
	}
#ifdef PLATFORM_WII
	if(p[0] == '/' || p[0] == '\\') {
		const char* c = strchr(cwd, ':');
		if(!c) return p;
		int n = snprintf(out,SAVE_PATH_MAX,"%.*s:%s",(int)(c-cwd),cwd,p);
		return n >= 0 && n < SAVE_PATH_MAX ? out : p;
	}
#endif
	int n = snprintf(out,SAVE_PATH_MAX,"%s/%s",cwd,p);
	return n >= 0 && n < SAVE_PATH_MAX ? out : p;
}

void save_path_set_world(const char* world) {
	if(!s_ready) return;
	int saved = errno;
	save_path_begin();
	char tmp[SAVE_PATH_MAX];
	const char* p = absolute_path(world,tmp);
	size_t n = strlen(p);
	while(n && p[n-1] == '/') n--;
	/* Never turn an absolute volume root into device-relative "sd:". */
	if(n && p[n-1] != ':' && n < sizeof s_world && clean_path(p)) {
		memcpy(s_world,p,n); s_world[n] = 0;
	} else s_world[0] = 0;
	save_path_end();
	errno = saved;
}

void save_path_set_parent(const char* file) {
	if(!s_ready) return;
	const char* slash = strrchr(file, '/');
	size_t n = slash ? (size_t)(slash-file) : 0;
	/* Preserve absolute root syntax; set_world deliberately won't cache it. */
	if(slash && (!n || file[n-1] == ':')) n++;
	char parent[SAVE_PATH_MAX];
	if(!slash || n >= sizeof parent) { save_path_set_world("."); return; }
	memcpy(parent,file,n); parent[n]=0;
	save_path_set_world(parent);
}

const char* save_path_prepare(const char* path, char out[SAVE_PATH_MAX]) {
	if(!s_ready) return path;
	int saved = errno;
	const char* p = absolute_path(path,out);
	/* If a relative request could not be expanded, never let it accidentally
	 * resolve against the cached world. Restore the original cwd first. */
	const char* colon = strchr(p, ':');
	bool absolute = p[0] == '/' || (colon && (colon[1] == '/' || colon[1] == '\\'));
	if(!absolute && s_active[0]) {
		if(!save_path_invalidate(NULL)) return NULL;
	}
	size_t n = strlen(s_world);
	if(n && strncmp(p,s_world,n) == 0 && p[n] == '/' && p[n+1]
	   && clean_path(p)) {
#ifdef PLATFORM_WII
		const char* device_end = strchr(s_world, ':');
		size_t devlen = device_end ? (size_t)(device_end-s_world)+1 : 0;
#else
		size_t devlen = 0;
#endif
		size_t tail = strlen(p+n+1);
		if(tail >= SAVE_PATH_MAX-devlen) goto fallback;
		if(strcmp(s_active,s_world) != 0) {
			char cwd[SAVE_PATH_MAX];
			const char* c = strchr(s_world, ':');
			if(!s_getcwd(cwd,sizeof cwd)) goto fallback;
#ifdef PLATFORM_WII
			const char* basecolon = strchr(cwd, ':');
			if(!c || !basecolon || c-s_world != basecolon-cwd
			   || strncmp(s_world,cwd,(size_t)(c-s_world)) != 0) goto fallback;
#else
			(void)c;
#endif
			if(s_chdir(s_world) != 0) goto fallback;
			if(!s_active[0]) memcpy(s_original,cwd,strlen(cwd)+1);
			memcpy(s_active,s_world,n+1);
			s_selects++; s_generation++;
		}
		/* Prepare may have put p in out. Move before adding the device prefix. */

		memmove(out+devlen,p+n+1,tail+1);
		if(devlen) memcpy(out,s_world,devlen);
		s_hits++;
		errno = saved;
		return out;
	}
fallback:
	s_fallbacks++;
	errno = saved;
	return p;
}

bool save_path_invalidate(const char* path) {
	if(!s_ready || !s_active[0]) return true;
	char tmp[SAVE_PATH_MAX];
	const char* p = path ? absolute_path(path,tmp) : NULL;
	size_t n = strlen(s_active);
	/* Keep the cluster only for a clean, exact descendant mutation (journal
	 * files). FAT aliases/case folding can identify an ancestor without a
	 * byte-prefix match, so every other rename/rmdir restores conservatively. */
	if(!p || !clean_path(p) || strncmp(p,s_active,n) != 0 || p[n] != '/') {
		if(s_chdir(s_original) != 0) return false;
		s_active[0] = 0; s_generation++;
	}
	return true;
}

bool save_path_prepare_pair(const char* from,const char* to,
                            char a[SAVE_PATH_MAX],char b[SAVE_PATH_MAX],
                            const char** p,const char** q) {
	if(!save_path_invalidate(from) || !save_path_invalidate(to)) return false;
	*p = save_path_prepare(from,a);
	unsigned generation = s_generation;
	*q = save_path_prepare(to,b);
	if(!*p || !*q) return false;
	/* An unexpandable second path can restore native cwd. Never use a first
	 * path relative to the previously selected cluster after that restore. */
	if(generation != s_generation) {
		if(!save_path_invalidate(NULL)) return false;
		*p = from; *q = to;
	}
	return true;
}

void save_path_stats(unsigned out[3]) {
	save_path_begin();
	out[0]=s_selects; out[1]=s_hits; out[2]=s_fallbacks;
	save_path_end();
}

#ifdef PLATFORM_WII
#include <fat.h>
#include <dirent.h>
#include <sys/iosupport.h>
#include <unistd.h>

static int native_chdir(const char* path) {
	const devoptab_t* d = GetDeviceOpTab(path);
	if(!d || !d->chdir_r) { errno = ENOSYS; return -1; }
	struct _reent* r = __getreent();
	void* data = r->deviceData;
	r->deviceData = d->deviceData;
	int rc = d->chdir_r(r,path);
	r->deviceData = data;
	return rc;
}

bool __real_fatInitDefault(void);
bool __wrap_fatInitDefault(void) {
	bool ok = __real_fatInitDefault();
	if(ok) (void)save_path_init(native_chdir,getcwd);
	return ok;
}

int __real__open_r(struct _reent*, const char*, int, int);
int __wrap__open_r(struct _reent* r, const char* path, int flags, int mode) {
	save_path_begin();
	char buf[SAVE_PATH_MAX];
	const char* p = save_path_prepare(path,buf);
	int rc = p ? __real__open_r(r,p,flags,mode) : -1;
	save_path_end(); return rc;
}
int __real__stat_r(struct _reent*,const char*,struct stat*);
int __wrap__stat_r(struct _reent* r,const char* path,struct stat* st) {
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p = save_path_prepare(path,buf);
	int rc = p ? __real__stat_r(r,p,st) : -1;
	save_path_end(); return rc;
}
int __real__unlink_r(struct _reent*,const char*);
int __wrap__unlink_r(struct _reent* r,const char* path) {
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p = save_path_prepare(path,buf);
	int rc = p ? __real__unlink_r(r,p) : -1;
	save_path_end(); return rc;
}
int __real__rmdir_r(struct _reent*,const char*);
int __wrap__rmdir_r(struct _reent* r,const char* path) {
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p = save_path_invalidate(path) ? save_path_prepare(path,buf) : NULL;
	int rc = p ? __real__rmdir_r(r,p) : -1;
	save_path_end(); return rc;
}
/* libsysbase defines public rmdir and _rmdir_r in one object. GNU --wrap
 * cannot redirect that internal branch, so gate the public entry as well. */
int __real_rmdir(const char*);
int __wrap_rmdir(const char* path) {
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p = save_path_invalidate(path) ? save_path_prepare(path,buf) : NULL;
	int rc = p ? __real_rmdir(p) : -1;
	save_path_end(); return rc;
}
int __real__rename_r(struct _reent*,const char*,const char*);
int __wrap__rename_r(struct _reent* r,const char* from,const char* to) {
	save_path_begin(); char a[SAVE_PATH_MAX],b[SAVE_PATH_MAX];
	const char *p = NULL, *q = NULL;
	bool prepared = save_path_prepare_pair(from,to,a,b,&p,&q);
	int rc = prepared ? __real__rename_r(r,p,q) : -1;
	save_path_end(); return rc;
}
int __real_mkdir(const char*,int);
int __wrap_mkdir(const char* path,int mode) {
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p = save_path_prepare(path,buf);
	int rc = p ? __real_mkdir(p,mode) : -1;
	save_path_end(); return rc;
}
DIR* __real_opendir(const char*);
DIR* __wrap_opendir(const char* path) {
	save_path_begin(); char buf[SAVE_PATH_MAX];
	const char* p = save_path_prepare(path,buf);
	DIR* d = p ? __real_opendir(p) : NULL;
	save_path_end(); return d;
}
int __real_chdir(const char*);
int __wrap_chdir(const char* path) {
	save_path_begin();
	int rc = save_path_invalidate(NULL) ? __real_chdir(path) : -1;
	save_path_end(); return rc;
}
#endif
