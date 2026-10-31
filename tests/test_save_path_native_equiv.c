/* Compile the exact PLATFORM_WII wrappers against a path-observing SDK shim.
 * The shim distinguishes FatFs cwd from POSIX cwd, as the linked SDK does.
 * Real release ABI/header compilation is checked separately, not certified by
 * this shim. No mock persistence bytes are used as a corpus oracle. */
#include "network/save_path.h"
#include "platform/thread.h"
#include <sys/iosupport.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define APP "sd:/apps/game"
#define W1 APP "/saves/world1"
#define W2 APP "/saves/world2"
#define RP W1 "/region"
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static char posix_cwd[SAVE_PATH_MAX]=APP;
static char fat_cwd[SAVE_PATH_MAX]=APP;
static bool world1_exists=true;
static bool fail_chdir;
static unsigned world_walks;
static _Thread_local struct _reent reent; /* test executable only */
struct _reent* __getreent(void) { return &reent; }
bool thread_mutex_init(struct thread_mutex* m) { m->initialized=true; return true; }
void thread_mutex_lock(struct thread_mutex* m) { assert(m->initialized); assert(pthread_mutex_lock(&mutex)==0); }
void thread_mutex_unlock(struct thread_mutex* m) { assert(m->initialized); assert(pthread_mutex_unlock(&mutex)==0); }
static void copy(char* out,const char* in) { assert(strlen(in)<SAVE_PATH_MAX); memcpy(out,in,strlen(in)+1); }
static int error(struct _reent* r,int e) { errno=e; r->_errno=e; return -1; }
static int resolve(const char* path,char out[SAVE_PATH_MAX]) {
	const char* p=strchr(path,':'); p=p ? p+1 : path;
	if(!*p) { copy(out,fat_cwd); return 0; }
	int n= (p[0]=='/' || p[0]=='\\') ? snprintf(out,SAVE_PATH_MAX,"sd:%s",p)
		: snprintf(out,SAVE_PATH_MAX,"%s/%s",fat_cwd,p);
	if(n<0 || n>=SAVE_PATH_MAX) { errno=ENAMETOOLONG; return -1; }
	for(char* c=out;*c;c++) if(*c=='\\') *c='/';
	if(p[0]=='/' && strstr(p,"/saves/")) world_walks++;
	return 0;
}
static int sdk_chdir(struct _reent* r,const char* path) {
	assert(r->deviceData==&fat_cwd);
	char p[SAVE_PATH_MAX]; if(resolve(path,p)!=0) return error(r,errno);
	if(fail_chdir) return error(r,EACCES);
	if(strcmp(p,APP) && strcmp(p,W2) && ((strcmp(p,W1) && strcmp(p,RP)) || !world1_exists)) return error(r,ENOENT);
	copy(fat_cwd,p); return 0;
}
static const devoptab_t sd={&fat_cwd,sdk_chdir};
const devoptab_t* GetDeviceOpTab(const char* path) { (void)path; return &sd; }
char* __wrap_getcwd(char* out,size_t len) {
	if(strlen(posix_cwd)>=len) { errno=ERANGE; return NULL; }
	memcpy(out,posix_cwd,strlen(posix_cwd)+1); return out;
}
bool __real_fatInitDefault(void) { return true; }
int __real_chdir(const char* path) {
	char p[SAVE_PATH_MAX]; struct _reent* r=__getreent();
	if(resolve(path,p)!=0) return error(r,errno);
	void* old=r->deviceData; r->deviceData=sd.deviceData;
	int rc=sdk_chdir(r,path); r->deviceData=old;
	if(!rc) copy(posix_cwd,p);
	return rc;
}
int __real__open_r(struct _reent* r,const char* path,int flags,int mode) {
	(void)flags;(void)mode; char p[SAVE_PATH_MAX];
	if(resolve(path,p)!=0) return error(r,errno);
	if(!strcmp(p,W1 "/ticket") && world1_exists) return 11;
	if(!strcmp(p,W2 "/ticket")) return 12;
	if(!strcmp(p,RP "/ticket") && world1_exists) return 14;
	if(!strcmp(p,APP "/assets/sound.ogg")) return 13;
	return error(r,ENOENT);
}
int __real__stat_r(struct _reent* r,const char* path,struct stat* st) {
	int fd=__real__open_r(r,path,0,0); if(fd<0) return -1;
	memset(st,0,sizeof *st); st->st_size=fd; return 0;
}
int __real__unlink_r(struct _reent* r,const char* path) { return __real__open_r(r,path,0,0)<0 ? -1 : 0; }
int __real__rmdir_r(struct _reent* r,const char* path) {
	char p[SAVE_PATH_MAX]; if(resolve(path,p)!=0) return error(r,errno);
	if(strcmp(p,W1)) return error(r,ENOENT);
	world1_exists=false; return 0;
}
int __real_rmdir(const char* path) { return __real__rmdir_r(__getreent(),path); }
int __real__rename_r(struct _reent* r,const char* from,const char* to) {
	char p[SAVE_PATH_MAX],q[SAVE_PATH_MAX];
	if(resolve(from,p)!=0 || resolve(to,q)!=0) return error(r,errno);
	if(!strcasecmp(p,W1) && !strcmp(q,APP "/archived")) { world1_exists=false; return 0; }
	if(!strcmp(p,W1 "/ticket") && !strcmp(q,W1 "/ticket_new")) return 0;
	return error(r,ENOENT);
}
int __real_mkdir(const char* path,int mode) { (void)mode; char p[SAVE_PATH_MAX]; return resolve(path,p); }
DIR* __real_opendir(const char* path) { char p[SAVE_PATH_MAX]; return resolve(path,p)==0 ? (DIR*)&sd : NULL; }
bool __wrap_fatInitDefault(void);
int __wrap__open_r(struct _reent*,const char*,int,int);
int __wrap__stat_r(struct _reent*,const char*,struct stat*);
int __wrap__unlink_r(struct _reent*,const char*);
int __wrap__rmdir_r(struct _reent*,const char*);
int __wrap_rmdir(const char*);
int __wrap__rename_r(struct _reent*,const char*,const char*);
int __wrap_mkdir(const char*,int);
DIR* __wrap_opendir(const char*);
int __wrap_chdir(const char*);
static void* audio(void* unused) {
	(void)unused; struct _reent* r=__getreent();
	for(unsigned i=0;i<1000;i++) assert(__wrap__open_r(r,"assets/sound.ogg",0,0)==13);
	return NULL;
}
static void* worlds(void* unused) {
	(void)unused; struct _reent* r=__getreent();
	for(unsigned i=0;i<1000;i++) {
		save_path_set_world(i&1 ? W1 : W2);
		assert(__wrap__open_r(r,i&1 ? W1 "/ticket" : W2 "/ticket",0,0)==(i&1 ? 11 : 12));
	}
	return NULL;
}
int main(void) {
	assert(__wrap_fatInitDefault()); struct _reent* r=__getreent();
	int sentinel_value=0; void* sentinel=&sentinel_value; r->deviceData=sentinel;
	save_path_set_world(W1); world_walks=0;
	for(unsigned i=0;i<183;i++) {
		struct stat st; assert(__wrap__stat_r(r,W1 "/ticket",&st)==0); assert(st.st_size==11);
	}
	assert(world_walks==1); assert(!strcmp(posix_cwd,APP)); assert(r->deviceData==sentinel);
	assert(__wrap__open_r(r,"assets/sound.ogg",0,0)==13);
	assert(__wrap__open_r(r,"sd:assets/sound.ogg",0,0)==13);
	assert(__wrap__open_r(r,"/apps/game/assets/sound.ogg",0,0)==13);
	assert(__wrap__open_r(r,"\\apps\\game\\assets\\sound.ogg",0,0)==13);
	/* SAVE selects its own existing level archive after another world was active. */
	save_path_set_world(W2); assert(__wrap__open_r(r,W2 "/ticket",0,0)==12);
	save_path_set_parent(W1 "/level.dat"); world_walks=0;
	for(unsigned i=0;i<156;i++) assert(__wrap__open_r(r,W1 "/ticket",0,0)==11);
	assert(world_walks==1);
	puts("PASS SAVE after other-world selection: 156 native opens, 1 world-prefix traversal");
	save_path_set_parent(RP "/r.0.0.mcr"); world_walks=0;
	for(unsigned i=0;i<156;i++) assert(__wrap__open_r(r,RP "/ticket",0,0)==14);
	assert(world_walks==1);
	puts("PASS resident journal SAVE parent selection: 156 native opens, 1 world-prefix traversal");
	/* A volume root must not be trimmed to device-relative "sd:" and reuse
	 * the preceding world's native cwd. FatFs empty paths retain current cwd. */
	save_path_set_world(W2); assert(__wrap__open_r(r,W2 "/ticket",0,0)==12);
	save_path_set_parent("sd:/level.dat"); assert(__wrap__open_r(r,W1 "/ticket",0,0)==11);
	save_path_set_world(W1);
	/* A second path on another device cannot be expanded against sd POSIX cwd.
	 * If it restores native cwd after the first path was prepared, both must
	 * use original resolution, never the now-stale relative first suffix. */
	const char* from=W1 "/ticket"; const char* to="usb:ticket";
	const char *p,*q; char ba[SAVE_PATH_MAX],bb[SAVE_PATH_MAX];
	save_path_begin();
	assert(save_path_prepare_pair(from,to,ba,bb,&p,&q));
	assert(p==from && q==to); assert(!strcmp(fat_cwd,APP));
	save_path_end();
	puts("PASS 183 native stat requests: 1 world-prefix traversal; POSIX cwd/deviceData preserved; unrelated relative paths retained");
	pthread_t a,w; assert(!pthread_create(&a,NULL,audio,NULL)); assert(!pthread_create(&w,NULL,worlds,NULL));
	assert(!pthread_join(a,NULL));assert(!pthread_join(w,NULL));
	puts("PASS exact Wii wrapper concurrent audio/world resolution (1000 requests each)");
	save_path_set_world(W1); assert(__wrap__unlink_r(r,W1 "/ticket")==0);
	assert(__wrap_mkdir(W1 "/region",0777)==0); assert(__wrap_opendir(W1 "/region"));
	assert(__wrap__rename_r(r,W1 "/ticket",W1 "/ticket_new")==0);
	assert(__wrap_chdir(W2)==0); assert(!strcmp(posix_cwd,W2));
	assert(__wrap__open_r(r,"ticket",0,0)==12); assert(__wrap_chdir(APP)==0);
	save_path_set_world(W1); assert(__wrap__open_r(r,W1 "/ticket",0,0)==11);
	fail_chdir=true;
	assert(__wrap__rename_r(r,W1,APP "/archived")==-1); assert(errno==EACCES); assert(world1_exists);
	fail_chdir=false;
	assert(__wrap__rename_r(r,APP "/saves/WORLD1",APP "/archived")==0);
	assert(!strcmp(fat_cwd,APP));
	assert(__wrap__open_r(r,W1 "/ticket",0,0)==-1); assert(errno==ENOENT);
	world1_exists=true; save_path_set_world(W1);
	assert(__wrap__open_r(r,W1 "/ticket",0,0)==11);
	assert(__wrap_rmdir(W1)==0);
	world1_exists=true; save_path_set_world(W1);
	assert(__wrap__open_r(r,W1 "/ticket",0,0)==11);
	assert(__wrap__rmdir_r(r,W1)==0); assert(!strcmp(fat_cwd,APP));
	puts("PASS native rename/unlink/mkdir/opendir/chdir/rmdir paths; failed restoration fails closed; ancestor identity invalidates");
	return 0;
}
