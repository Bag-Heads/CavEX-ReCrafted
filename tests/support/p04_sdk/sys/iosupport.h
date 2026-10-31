#ifndef P04_IOSUPPORT_H
#define P04_IOSUPPORT_H
#include <stddef.h>
#include <sys/stat.h>
struct _reent { int _errno; void* deviceData; };
typedef struct { void* deviceData; int (*chdir_r)(struct _reent*,const char*); } devoptab_t;
struct _reent* __getreent(void);
const devoptab_t* GetDeviceOpTab(const char*);
#endif
