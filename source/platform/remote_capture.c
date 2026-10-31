#include "remote_capture.h"
#ifdef PLATFORM_WII
#include "remote_capture_protocol.h"
#include "gfx.h"
#include "thread.h"
#include "wii/net_bringup.h"
#include <errno.h>
#include <fcntl.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <string.h>

enum { SHOT_IDLE, SHOT_REQUEST, SHOT_CAPTURING, SHOT_READY, SHOT_FAILED };
#define SHOT_PATH "sd:/apps/CavEX/remote_capture.rgba"
#define LOAD(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n(&(p),(v),__ATOMIC_RELEASE)
static unsigned phase;
static bool stop, done, enabled, started;
static struct thread worker;
static char secret[129], build_id[80];
static void note(const char* stage,int value) {
    FILE* fp=fopen("sd:/apps/CavEX/remote_capture_status.txt","a");
    if(fp) { fprintf(fp,"%s %d\n",stage,value); fclose(fp); }
}
static uint64_t now_ms(void) { return ticks_to_millisecs(gettime()); }
static bool nonblock(int fd) {
    int f=net_fcntl(fd,F_GETFL,0);
    if(f<0 || LOAD(stop) || net_fcntl(fd,F_SETFL,f|4)<0 || LOAD(stop)) return false;
    int actual=net_fcntl(fd,F_GETFL,0);
    return actual>=0 && (actual&4)!=0;
}
static bool again(int n) {
    return n==-EAGAIN || n==-EWOULDBLOCK || n==-EINTR;
}
static bool send_bytes(int fd,const void* data,size_t size,uint64_t deadline) {
    const unsigned char* p=data;
    while(size && !LOAD(stop) && now_ms()<deadline) {
        /* IOS can queue a prefix then return -EAGAIN without its length.
         * Retrying would duplicate pixels. Match Stage Server's paced 1 KiB
         * writes, and abandon the connection on any ambiguous error. */
        size_t part=size>1024 ? 1024 : size;
        size_t sent=0;
        while(sent<part && !LOAD(stop) && now_ms()<deadline) {
            int n=net_send(fd,p+sent,part-sent,0);
            if(n<=0 || (size_t)n>part-sent) return false;
            sent+=(size_t)n;
        }
        if(sent!=part) return false;
        p+=part; size-=part;
        /* Explicit receiver credit: never queue another piece until the
         * previous one has arrived. Pacing alone still overflowed IOS in
         * the live game, unlike the otherwise idle Stage Server. */
        for(;;) {
            char ack;
            if(LOAD(stop) || now_ms()>=deadline) return false;
            int got=net_recv(fd,&ack,1,0);
            if(got==1) { if(ack!='+') return false; break; }
            if(!again(got)) return false;
            thread_msleep(1);
        }
    }
    return size==0;
}
static void serve(int fd) {
    char line[192],nonce[33]; size_t used=0;
    uint64_t deadline=now_ms()+5000;
    bool complete=false;
    while(used+1<sizeof(line) && !LOAD(stop) && now_ms()<deadline) {
        char c; int n=net_recv(fd,&c,1,0);
        if(n==1) {
            if(c=='\n') { complete=true; break; }
            if(c=='\0' || c=='\r') return;
            line[used++]=c;
        } else if(again(n)) thread_msleep(5);
        else return;
    }
    line[used]=0;
    if(!complete || !remote_capture_auth(line,secret,nonce)) return;
    STORE(phase,SHOT_REQUEST);
    deadline=now_ms()+15000;
    while(!LOAD(stop) && now_ms()<deadline
          && (LOAD(phase)==SHOT_REQUEST || LOAD(phase)==SHOT_CAPTURING))
        thread_msleep(5);
    if(LOAD(phase)!=SHOT_READY || LOAD(stop)) {
        /* Cancel an unclaimed request. Never recycle the file while main
         * is still writing; a disconnected/slow requester cannot race GX. */
        unsigned expected=SHOT_REQUEST;
        __atomic_compare_exchange_n(&phase,&expected,SHOT_IDLE,false,
                                    __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        while(LOAD(phase)==SHOT_CAPTURING && !LOAD(stop)) thread_msleep(5);
        STORE(phase,SHOT_IDLE);
        return;
    }
    FILE* fp=fopen(SHOT_PATH,"rb");
    if(fp) {
        if(fseek(fp,0,SEEK_END)==0) {
            long size=ftell(fp);
            if(size>=12 && size<=4*1024*1024 && fseek(fp,0,SEEK_SET)==0) {
                char header[160];
                int len=snprintf(header,sizeof(header),"CWSHOT1 %s %s %ld\n",nonce,build_id,size);
                deadline=now_ms()+120000;
                bool ok=len>0 && (size_t)len<sizeof(header)
                    && send_bytes(fd,header,(size_t)len,deadline);
                unsigned char buf[4096];
                while(ok && size>0) {
                    size_t want=(size_t)size>sizeof(buf) ? sizeof(buf) : (size_t)size;
                    size_t n=fread(buf,1,want,fp);
                    ok=n==want && send_bytes(fd,buf,n,deadline);
                    size-=(long)n;
                }
            }
        }
        fclose(fp);
    }
    STORE(phase,SHOT_IDLE);
}
/* Worker-only failure detail; recording a retry never writes to SD. */
static const char* failure_stage;
static int failure_value;
static uint64_t last_retry_note;
static uint64_t total_recreate_failures;
static bool retry_noted;
static bool failed(const char* stage,int value) {
    failure_stage=stage; failure_value=value; return false;
}
static void recovery_note(const char* kind,unsigned count) {
    FILE* fp=fopen("sd:/apps/CavEX/remote_capture_status.txt","a");
    if(fp) {
        fprintf(fp,"%s stage=%s value=%d count=%u total=%llu\n",kind,failure_stage,
                failure_value,count,(unsigned long long)total_recreate_failures);
        fclose(fp);
    }
}
/* -2 means ownership unknown. IOS may release an fd even when close fails;
 * never close that number again or allocate a replacement after this error.
 * If IOS retained it, it leaks until reboot: safer than closing a reused fd. */
static bool close_socket(int* fd) {
    if(*fd==-2) return false;
    if(*fd<0) return true;
    int result=net_close(*fd);
    *fd=result<0 ? -2 : -1;
    if(result<0) return failed("close_failed",result);
    return true;
}
static bool listener_open(int* listener,bool replacement) {
    /* IOS requires STREAM + IP. Preserve initial setup; replacements need
     * checked reuseaddr to bind the identical endpoint after accepted close. */
    if(LOAD(stop)) return false;
    *listener=net_socket(AF_INET,SOCK_STREAM,IPPROTO_IP);
    if(!replacement) note("socket",*listener);
    if(*listener<0) {
        int result=*listener; *listener=-1;
        return failed("socket_failed",result);
    }
    if(LOAD(stop)) return false;
    bool flagged=nonblock(*listener);
    if(!replacement) note("nonblock",flagged);
    if(!flagged) return failed("nonblock_failed",0);
    if(LOAD(stop)) return false;
    if(replacement) {
        int yes=1;
        int result=net_setsockopt(*listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
        if(result<0) return failed("reuseaddr_failed",result);
        if(LOAD(stop)) return false;
    }
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr)); addr.sin_family=AF_INET;
    addr.sin_port=htons(39020); addr.sin_addr.s_addr=INADDR_ANY;
    int bound=net_bind(*listener,(struct sockaddr*)&addr,sizeof(addr));
    if(!replacement) note("bind",bound);
    if(bound<0) return failed("bind_failed",bound);
    if(LOAD(stop)) return false;
    int listening=net_listen(*listener,1);
    if(!replacement) note("listen",listening);
    if(listening<0) return failed("listen_failed",listening);
    return !LOAD(stop);
}
static bool listener_recreate(int* listener,int* accepted) {
    unsigned delay=100,failures=0;
    while(!LOAD(stop)) {
        /* Worker alone owns accepted close, listener close and all setup.
         * Partial setup is cleaned before opening another socket. */
        if(close_socket(accepted) && !LOAD(stop) && close_socket(listener)
           && !LOAD(stop) && listener_open(listener,true)) return true;
        if(LOAD(stop)) return false;
        failures++;
        total_recreate_failures++;
        if(failures==20) {
            recovery_note("capture_disabled_until_boot",failures);
            return false;
        }
        uint64_t now=now_ms();
        if(!retry_noted || now-last_retry_note>=5000) {
            recovery_note("retry",failures); last_retry_note=now; retry_noted=true;
        }
        /* 100, 200, 400, then at most 500 ms per failed attempt, sliced at
         * 20 ms for stop. At most 20 consecutive failures; no network reset.
         * Nonblocking does not bound synchronous IOS setup/close calls. */
        for(unsigned waited=0;waited<delay && !LOAD(stop);waited+=20)
            thread_msleep(20);
        delay=delay<400 ? delay*2 : 500;
    }
    return false;
}
static void* run(void* unused) {
    (void)unused;
    int listener=-1,accepted=-1;
    unsigned generation=0;
    if(listener_open(&listener,false)) {
        while(!LOAD(stop)) {
            unsigned char peer[32] __attribute__((aligned(32)))={0};
            socklen_t len=sizeof(struct sockaddr_in);
            accepted=net_accept(listener,(struct sockaddr*)peer,&len);
            if(accepted>=0) {
                if(nonblock(accepted)) serve(accepted);
                if(!listener_recreate(&listener,&accepted)) break;
                /* First and every 32nd later recreation: bounded SD traffic. */
                if((generation++%32)==0) {
                    char marker[64];
                    snprintf(marker,sizeof(marker),"listener_recreated gen=%u",generation);
                    note(marker,0);
                }
            } else if(!again(accepted)) { note("accept_failed",accepted); break; }
            thread_msleep(20);
        }
    }
    /* Final checked cleanup is one attempt per owned descriptor, even after
     * stop. Shutdown keeps its existing 2 s deadline/abandonment policy. */
    if(accepted>=0 && !close_socket(&accepted)) note(failure_stage,failure_value);
    if(listener>=0 && !close_socket(&listener)) note(failure_stage,failure_value);
    STORE(done,true);
    return NULL;
}
void remote_capture_init(bool on,const char* build) {
    if(!on) return;
    FILE* status=fopen("sd:/apps/CavEX/remote_capture_status.txt","w");
    if(status) { fprintf(status,"build %s enabled %d\n",build,on); fclose(status); }
    if(enabled || started) return;
    FILE* fp=fopen("sd:/apps/CavEX/stage_token.txt","rb");
    if(!fp) { note("token_open_failed",errno); return; }
    char token[132]; size_t n=fread(token,1,sizeof(token)-1,fp);
    bool ok=!ferror(fp) && feof(fp); fclose(fp);
    token[n]=0;
    while(n && (token[n-1]=='\r'||token[n-1]=='\n')) token[--n]=0;
    if(!ok || !n || n>128 || memchr(token,'\0',n)
       || strpbrk(token," \t\r\n\v\f")) { note("token_invalid",(int)n); return; }
    memcpy(secret,token,n+1);
    snprintf(build_id,sizeof(build_id),"%s",build);
    enabled=true;
    net_bringup_request();
    note("network_requested",0);
}
void remote_capture_frame(void) {
    if(!enabled) return;
    if(!started) {
        int state=net_bringup_status(NULL,0);
        static int last=-1;
        if(state!=last) { last=state; note("network_state",state); }
        if(state!=NET_BRINGUP_READY) return;
        started=thread_spawn_disposable(&worker,run,NULL,32);
        note("worker_started",started);
        if(!started) enabled=false;
        return;
    }
    unsigned expected=SHOT_REQUEST;
    if(__atomic_compare_exchange_n(&phase,&expected,SHOT_CAPTURING,false,
                                   __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        bool ok=gfx_write_framebuffer_raw(SHOT_PATH);
        STORE(phase,ok ? SHOT_READY : SHOT_FAILED);
    }
}
void remote_capture_shutdown(void) {
    enabled=false; STORE(stop,true);
    if(!started) return;
    uint64_t deadline=now_ms()+2000;
    while(!LOAD(done) && now_ms()<deadline) thread_msleep(5);
    /* No unbounded join on an IOS call; static storage survives abandonment. */
    thread_dispose(&worker,LOAD(done));
    started=false;
}
#else
void remote_capture_init(bool enabled,const char* build) { (void)enabled; (void)build; }
void remote_capture_frame(void) {}
void remote_capture_shutdown(void) {}
#endif
