/* X05 deterministic IOS lifecycle double. Includes the whole production Wii
 * branch; only SDK/socket/clock/thread/GX boundaries are replaced. No capture
 * bytes or credits are synthesized here: real valid captures live in B02's
 * Winsock/Python pair. This fixture checks ordering/ownership, injected signed
 * operation failures, nonblocking readback, retry delays and stop boundaries.
 * It cannot establish IOS close-error semantics or bound a synchronous IOS call.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "support/remote_capture_host.h"
#ifdef NDEBUG
#error "Lifecycle assertions must remain enabled"
#endif
#define GFX_H
#define THREAD_H
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#endif
struct thread { bool created; };
static const char* scenario;
static bool owned_listener,owned_client,dead_listener,ready_listener;
static bool injected,cleanup_injected,stopped_at_op,backoff_seen;
static unsigned stage,opens,closes,accepted,client_closes,failures,nonblock_calls;
static unsigned sleep_calls,backoffs[8],backoff_count;
static uint64_t virtual_ms,failure_ms,shutdown_start;
static void set_stop(void);
static bool is_stopping(void);
static void shutdown_here(void);
static bool shutdown_entered;
static unsigned unknown_client,unknown_listener,status_writes;
static uint64_t retry_log_ms;
static bool retry_logged;
static unsigned last_fault_client,spawn_calls;

static bool operation(const char* op) {
    assert(!is_stopping()); /* No new IOS operation after observing stop. */
    if(!accepted) return false;
    char target[80]; snprintf(target,sizeof(target),"stop-%s",op);
    if(!strcmp(scenario,target)) {
        assert(!stopped_at_op); stopped_at_op=true; set_stop(); return false;
    }
    snprintf(target,sizeof(target),"fail-%s",op);
    bool fail=!strcmp(scenario,target) && !injected;
    if(!strncmp(scenario,"permanent-",10) && !strcmp(op,scenario+10)) fail=true;
    if(!strcmp(scenario,"intermittent-bind") && !strcmp(op,"bind") && accepted!=last_fault_client) {
        last_fault_client=accepted; fail=true;
    }
    if(!strcmp(scenario,"backoff-cap") && !strcmp(op,"socket") && failures<6) fail=true;
    if(!strcmp(scenario,"stop-backoff") && !strcmp(op,"socket")) fail=true;
    if((!strcmp(scenario,"fail-cleanup-close") || !strcmp(scenario,"stop-cleanup-backoff"))
       && !strcmp(op,"bind") && !injected) fail=true;
    if(!strcmp(scenario,"shutdown-backoff") && !strcmp(op,"socket")) fail=true;
    if(fail) { injected=true; failures++; failure_ms=virtual_ms; }
    return fail;
}
static uint64_t gettime(void) { return virtual_ms; }
static uint64_t ticks_to_millisecs(uint64_t value) { return value; }
static void thread_msleep(unsigned ms) {
    assert(ms<=20); sleep_calls++; virtual_ms+=ms;
    if(failures && !ready_listener) {
        backoff_seen=true;
        if(!strcmp(scenario,"stop-backoff")
           || (!strcmp(scenario,"stop-cleanup-backoff") && cleanup_injected)) set_stop();
        if(!strcmp(scenario,"shutdown-backoff")) shutdown_here();
    }
}
static bool thread_spawn_disposable(struct thread* t,void* (*entry)(void*),void* arg,uint8_t priority) {
    (void)entry; (void)arg; assert(priority==32); spawn_calls++; t->created=true; return true;
}
static void thread_dispose(struct thread* t,bool completed) {
    assert(t->created);
    /* shutdown-backoff deliberately has the worker paused in this sleep:
     * disposal must abandon it at 2 s rather than joining before done. */
    assert(completed || !strcmp(scenario,"shutdown-backoff")); t->created=false;
}
#include "../source/platform/wii/net_bringup.h"
void net_bringup_request(void) {}
enum net_bringup_state net_bringup_status(char* error,size_t size) {
    assert(!error && !size); return NET_BRINGUP_READY;
}
static int net_socket(int domain,int type,int protocol) {
    assert(domain==AF_INET && type==SOCK_STREAM && protocol==IPPROTO_IP);
    assert(!owned_listener && !owned_client);
    if(!strcmp(scenario,"backoff-cap") && failures && accepted==1) {
        assert(backoff_count<8); backoffs[backoff_count++]=(unsigned)(virtual_ms-failure_ms);
    }
    if(operation("socket")) return !strcmp(scenario,"permanent-socket") ? -2 : -EIO;
    owned_listener=true; ready_listener=false; dead_listener=false;
    opens++; stage=0; nonblock_calls=0; return 1;
}
static int net_fcntl(int fd,int command,int flags) {
    if(fd==2) { assert(owned_client); return command==F_GETFL ? 4 : 0; }
    assert(fd==1 && owned_listener);
    const char* op=nonblock_calls==0 ? "getfl" : nonblock_calls==1 ? "setfl" : "verifyfl";
    assert(stage==nonblock_calls && nonblock_calls<3);
    if(nonblock_calls==1) assert(command==F_SETFL && (flags&4));
    else assert(command==F_GETFL);
    bool fail=operation(op); nonblock_calls++; stage++;
    if(fail) return -EIO;
    if(accepted && !strcmp(scenario,"fail-verify-bit") && !injected && nonblock_calls==3) {
        injected=true; failures++; failure_ms=virtual_ms; return 0;
    }
    return command==F_GETFL ? 4 : 0;
}
static int net_setsockopt(int fd,int level,int option,const void* value,socklen_t size) {
    assert(fd==1 && owned_listener && stage==3 && accepted);
    assert(level==SOL_SOCKET && option==SO_REUSEADDR && size==sizeof(int));
    assert(*(const int*)value==1); stage=4;
    return operation("reuseaddr") ? -EIO : 0;
}
static int net_bind(int fd,const struct sockaddr* addr,socklen_t size) {
    assert(fd==1 && owned_listener && stage==(accepted ? 4u : 3u));
    const struct sockaddr_in* requested=(const struct sockaddr_in*)addr;
    assert(size==sizeof(*requested) && requested->sin_family==AF_INET);
    assert(ntohs(requested->sin_port)==39020 && requested->sin_addr.s_addr==INADDR_ANY);
    stage=5; return operation("bind") ? -EIO : 0;
}
static int net_listen(int fd,int backlog) {
    assert(fd==1 && owned_listener && stage==5 && backlog==1); stage=6;
    if(operation("listen")) return -EIO;
    ready_listener=true; return 0;
}
static int net_accept(int fd,struct sockaddr* addr,socklen_t* size) {
    (void)addr; assert(fd==1 && owned_listener && ready_listener && !dead_listener);
    assert(*size==sizeof(struct sockaddr_in) && !owned_client);
    if(accepted==(!strcmp(scenario,"many-recreates") || !strcmp(scenario,"intermittent-bind") ? 65u : 2u)) {
        set_stop(); return -EAGAIN;
    }
    owned_client=true; accepted++; return 2;
}
static int net_recv(int fd,void* data,size_t size,int flags) {
    (void)data; (void)size; (void)flags; assert(fd==2 && owned_client); return 0;
}
static int net_send(int fd,const void* data,size_t size,int flags) {
    (void)fd; (void)data; (void)size; (void)flags; assert(!"Unexpected capture send"); return -EIO;
}
static int net_close(int fd) {
    assert(fd==1 || fd==2);
    if(fd==2) {
        assert(owned_client);
        if(!is_stopping() && operation("accepted-close")) {
            owned_client=false; unknown_client++; return -EIO;
        }
        owned_client=false; client_closes++; dead_listener=true; return 0;
    }
    assert(owned_listener && !owned_client);
    if(!is_stopping() && operation("listener-close")) {
        owned_listener=false; unknown_listener++; return -EIO;
    }
    if(injected && ((!strcmp(scenario,"fail-cleanup-close") && !cleanup_injected)
                   || (!strcmp(scenario,"stop-cleanup-backoff") && !is_stopping()))) {
        cleanup_injected=true; failures++; failure_ms=virtual_ms;
        owned_listener=false; unknown_listener++; return -EIO;
    }
    owned_listener=false; ready_listener=false; closes++; return 0;
}
static bool gfx_write_framebuffer_raw(const char* path) { (void)path; assert(!"Unexpected GX"); return false; }
/* Status writes are remapped; no token or capture access is expected. */
static FILE* fixture_open(const char* path,const char* mode) {
    assert(!strcmp(path,"sd:/apps/CavEX/remote_capture_status.txt"));
    status_writes++;
    return fopen("lifecycle-status.txt",mode);
}
static int fixture_print(FILE* fp,const char* format,...) {
    va_list args; va_start(args,format);
    va_list check; va_copy(check,args);
    bool retry=!strncmp(format,"%s stage=",9) && !strcmp(va_arg(check,const char*),"retry");
    va_end(check);
    if(retry) {
        assert(!retry_logged || virtual_ms-retry_log_ms>=5000);
        retry_logged=true; retry_log_ms=virtual_ms;
    }
    int result=vfprintf(fp,format,args); va_end(args); return result;
}
#define fopen fixture_open
#define fprintf fixture_print
#ifdef REMOTE_CAPTURE_SOURCE
#include REMOTE_CAPTURE_SOURCE
#else
#include "../source/platform/remote_capture.c"
#endif
#undef fopen
#undef fprintf
static void set_stop(void) { STORE(stop,true); }
static bool is_stopping(void) { return LOAD(stop); }
static void shutdown_here(void) {
    if(backoff_seen && started && !shutdown_entered) {
        shutdown_entered=true;
        shutdown_start=virtual_ms; remote_capture_shutdown();
        assert(virtual_ms-shutdown_start==2000 && !started && !worker.created);
    }
}
int main(int argc,char** argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0,_CALL_REPORTFAULT);
#endif
    assert(argc==2); scenario=argv[1];
    started=enabled=true; worker.created=true;
    run(NULL);
    assert(LOAD(done) && !owned_listener && !owned_client && opens==closes+unknown_listener);
    assert(accepted==client_closes+unknown_client);
    FILE* log=fopen("lifecycle-status.txt","r"); assert(log);
    char line[160]; unsigned terminal=0,retry_lines=0,recreated=0;
    while(fgets(line,sizeof(line),log)) {
        if(!strncmp(scenario,"permanent-",10) &&
           (strstr(line,"retry ") || strstr(line,"capture_disabled_until_boot"))) {
            char expected[80]; snprintf(expected,sizeof(expected),"stage=%s_failed",scenario+10);
            assert(strstr(line,expected));
        }
        if(strstr(line,"capture_disabled_until_boot")) {
            assert(strstr(line,"count=20 ") && strstr(line,"total=20")); terminal++;
        }
        if(strstr(line,"retry ")) {
            if(!strncmp(scenario,"permanent-",10))
                assert(strstr(line,retry_lines ? "count=13 total=13" : "count=1 total=1"));
            retry_lines++;
        }
        if(strstr(line,"listener_recreated gen=")) {
            char expected[80]; snprintf(expected,sizeof(expected),"listener_recreated gen=%u 0",1+32*recreated);
            assert(strstr(line,expected)); recreated++;
        }
    }
    fclose(log);
    if(terminal) {
        remote_capture_frame(); remote_capture_frame();
        assert(LOAD(done) && spawn_calls==0 && started && worker.created);
    }
    if(unknown_client || unknown_listener) {
        assert(accepted==1 && terminal==(is_stopping() ? 0u : 1u) && opens<=2 && retry_lines<=3);
    }
    if(!strncmp(scenario,"fail-",5)) {
        assert(injected && accepted==(unknown_client || unknown_listener ? 1u : 2u));
        if(!strcmp(scenario,"fail-cleanup-close")) assert(cleanup_injected);
    } else if(!strncmp(scenario,"stop-",5)) {
        assert(accepted==1 && (stopped_at_op || backoff_seen || unknown_listener));
        if(!strcmp(scenario,"stop-cleanup-backoff")) assert(cleanup_injected && opens==2);
    } else if(!strcmp(scenario,"backoff-cap")) {
        const unsigned expected[]={100,200,400,500,500,500};
        assert(failures==6 && backoff_count==6 && accepted==2);
        assert(!memcmp(backoffs,expected,sizeof(expected)));
    } else if(!strcmp(scenario,"shutdown-backoff")) {
        assert(backoff_seen && accepted==1 && !started && !worker.created);
    } else if(!strncmp(scenario,"permanent-",10)) {
        assert(failures==20 && accepted==1 && terminal==1 && retry_lines==2);
        assert(status_writes<=7 && virtual_ms==8700);
    } else if(!strcmp(scenario,"many-recreates")) {
        assert(accepted==65 && recreated==3 && terminal==0);
    } else if(!strcmp(scenario,"intermittent-bind")) {
        assert(accepted==65 && failures==65 && recreated==3 && terminal==0 && retry_lines==2);
    } else assert(!strcmp(scenario,"shutdown") && accepted==2);
    if(started) {
        shutdown_start=virtual_ms; remote_capture_shutdown();
        assert(virtual_ms==shutdown_start && !started && !worker.created);
    }
    printf("PASS lifecycle %s: accepted=%u closed=%u listener=%u/%u faults=%u sleeps=%u virtual_ms=%llu done=%d\n",
           scenario,accepted,client_closes,opens,closes,failures,sleep_calls,
           (unsigned long long)virtual_ms,LOAD(done));
    return 0;
}
