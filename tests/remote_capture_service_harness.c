/* B01/B02: compile the entire production remote_capture.c PLATFORM_WII branch.
 * Test-only substitutes (no copied service logic):
 * - GFX_H/THREAD_H suppress SDK-heavy headers; only the used API is declared.
 * - Generated network.h/watchdog headers stand in for libogc; Winsock provides
 *   host socket types. Missing F_GETFL/F_SETFL are compile fixtures, not Wii ABI.
 * - B01 net calls still count/fail -EIO, spawn still runs synchronously, and its
 *   clock remains virtual. All original B01 witnesses/expectations are retained.
 * - B02 net_* are counted thin Winsock adapters, translating errors to negative
 *   errno. A slot table maps native SOCKETs to int handles; flags use ioctlsocket.
 *   Test bind maps ONLY the requested fixed listener to 127.0.0.1:0 initially,
 *   preserving that chosen port for every replacement. Listen publishes port
 *   and generation readiness files. No production endpoint change.
 * - B02 spawn/dispose/sleep use CreateThread/bounded join/Sleep. Bringup reports
 *   READY (no DHCP). GetTickCount64 plus injected offsets drives real deadlines.
 *   Missing-credit/deadline fixtures advance only the applicable deadline.
 * - Send faults use REAL socket sends for positive prefixes (or an unknown prefix
 *   followed by -EAGAIN), or return zero. Credits are received only from the peer,
 *   never synthesized. Actual credit byte offsets and retry counts are recorded.
 * - stdio adapters count calls and remap ONLY the three fixed SD paths to real
 *   host files. Token-open/read failures are injected at those adapter calls.
 * - GX is a counted fixture writer, not a renderer: B01 writes its original pixel;
 *   B02 copies fixture.rgba to the counted capture path on the actual main frame.
 * - B02 file adapters track active streams and reject capture read/write overlap.
 *   The driver pumps real init/frame/shutdown/run/serve/send_bytes, stopping after
 *   one connection (two for replay), or at the named stop/READY boundary. Cancel
 *   before claim deliberately withholds main frames and advances its deadline.
 *   Claimed shutdown is a deliberate reentrant test hook from the GX fixture
 *   while its counted output file is open; no real renderer reentrancy is claimed.
 *   Prefix stop/deadline hooks set the production atomic stop or advance time
 *   after the injected positive send, before the next production loop check.
 * - X05 marks each mock listener generation dead on accepted close for the
 *   recreate-* cases: accept returns -EAGAIN until that socket is closed. This
 *   models failed admission, not native Winsock/IOS handshake behavior. Six
 *   exact real captures wait for replacement readiness (no connection retries);
 *   recreate-mixed first checks empty/invalid-token EOF and zero GX work.
 * Production init/frame/shutdown/run and auth/token logic are included verbatim.
 * Each executable invocation is a fresh service lifetime (no static reset seam).
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

#ifdef NDEBUG
#error "The service harness must compile with assertions enabled"
#endif

#define GFX_H
#define THREAD_H
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#endif
struct thread { bool created; HANDLE native; void* (*entry)(void*); void* arg; };
typedef unsigned int socklen_t;
#define COUNT(field) __atomic_fetch_add(&calls.field,1,__ATOMIC_RELAXED)
#define ATOM(p) __atomic_load_n(&(p),__ATOMIC_ACQUIRE)
#define PUBLISH(p,v) __atomic_store_n(&(p),(v),__ATOMIC_RELEASE)

static struct {
    unsigned opens, status_opens, truncates, token_opens, reads, writes;
    unsigned prints, closes, errors, eof_checks, net, requests, status;
    unsigned sockets, threads, spawns, disposes, sleeps, gx, clock;
} calls;
static FILE* token_file;
static bool fail_open, fail_read;
static uint64_t virtual_ms;
static const char sentinel[]="previous diagnostic: preserve byte-for-byte\n";
static bool paired;
static const char* scenario;
static unsigned active_files, capture_readers, capture_writers, connections;
static FILE *capture_reader, *capture_writer;
static struct { SOCKET native; int flags; bool listener, dead; } sockets[16];
static unsigned short loopback_port;
static unsigned listener_opens, listener_closes;
static unsigned sends, fault_hits, retries_after_fault, socket_closes;
static unsigned credits, wrong_credits, credit_offsets[64], sent_bytes, header_size;
static unsigned session_credits[8], session_bytes[8], session_count;
static bool request_complete, terminal_fault, deadline_injected;
static bool header_sent;
static uint64_t first_send_ms;
static bool claimed_shutdown_seen;
static void claimed_shutdown(void);
static void prefix_stop(void);

static int socket_error(void) {
    int error=WSAGetLastError();
    return error==WSAEWOULDBLOCK ? -EAGAIN : error==WSAEINTR ? -EINTR : -EIO;
}
static int slot(SOCKET native) {
    assert(native!=INVALID_SOCKET);
    for(int i=1;i<16;i++) if(sockets[i].native==INVALID_SOCKET) {
        sockets[i].native=native; sockets[i].flags=0;
        sockets[i].listener=false; sockets[i].dead=false; return i;
    }
    assert(!"socket slot exhaustion"); return -1;
}

static FILE* io_open(const char* path,const char* mode) {
    COUNT(opens);
    const char* local;
    if(!strcmp(path,"sd:/apps/CavEX/remote_capture_status.txt")) {
        COUNT(status_opens);
        if(strchr(mode,'w')) COUNT(truncates);
        local="status.txt";
    } else if(!strcmp(path,"sd:/apps/CavEX/stage_token.txt")) {
        COUNT(token_opens);
        assert(!strcmp(mode,"rb"));
        if(fail_open) { errno=EACCES; return NULL; }
        token_file=fopen("token.bin",mode);
        if(token_file && paired) __atomic_fetch_add(&active_files,1,__ATOMIC_RELAXED);
        return token_file;
    } else {
        assert(!strcmp(path,"sd:/apps/CavEX/remote_capture.rgba"));
        local="capture.rgba";
    }
    if(paired && !strcmp(local,"capture.rgba")) {
        if(!strcmp(mode,"rb")) {
            assert(ATOM(capture_writers)==0);
            PUBLISH(capture_readers,1);
        } else {
            assert(ATOM(capture_readers)==0 && ATOM(capture_writers)==0);
            PUBLISH(capture_writers,1);
        }
    }
    FILE* fp=fopen(local,mode);
    if(fp && paired) {
        __atomic_fetch_add(&active_files,1,__ATOMIC_RELAXED);
        if(!strcmp(local,"capture.rgba")) {
            if(!strcmp(mode,"rb")) capture_reader=fp; else capture_writer=fp;
        }
    }
    return fp;
}
static size_t io_read(void* data,size_t size,size_t n,FILE* fp) {
    COUNT(reads);
    if(fp==token_file && fail_read) return 0;
    return fread(data,size,n,fp);
}
static size_t io_write(const void* data,size_t size,size_t n,FILE* fp) {
    COUNT(writes);
    return fwrite(data,size,n,fp);
}
static int io_error(FILE* fp) {
    COUNT(errors);
    return (fp==token_file && fail_read) ? 1 : ferror(fp);
}
static int io_eof(FILE* fp) { COUNT(eof_checks); return feof(fp); }
static int io_close(FILE* fp) {
    COUNT(closes);
    if(fp==token_file) token_file=NULL;
    if(paired) {
        assert(ATOM(active_files)>0);
        __atomic_fetch_sub(&active_files,1,__ATOMIC_RELAXED);
        if(fp==capture_reader) { capture_reader=NULL; PUBLISH(capture_readers,0); }
        if(fp==capture_writer) { capture_writer=NULL; PUBLISH(capture_writers,0); }
    }
    return fclose(fp);
}
/* Production fprintf uses exactly these two format/argument signatures. */
static int io_print(FILE* fp,const char* format,...) __attribute__((format(printf,2,3)));
#include <stdarg.h>
static int io_print(FILE* fp,const char* format,...) {
    COUNT(prints);
    va_list args; va_start(args,format);
    int n=vfprintf(fp,format,args);
    va_end(args);
    return n;
}
static uint64_t gettime(void) {
    COUNT(clock);
    return paired ? GetTickCount64()+ATOM(virtual_ms) : virtual_ms;
}
static uint64_t ticks_to_millisecs(uint64_t t) { return t; }
static void thread_msleep(unsigned ms) {
    COUNT(threads); COUNT(sleeps);
    if(paired) Sleep(ms); else virtual_ms+=ms;
}
static DWORD WINAPI thread_entry(LPVOID arg) {
    struct thread* t=arg; t->entry(t->arg); return 0;
}
static bool thread_spawn_disposable(struct thread* t,void* (*entry)(void*),
                                    void* arg,uint8_t priority) {
    COUNT(threads); COUNT(spawns);
    assert(priority==32); t->created=true;
    if(paired) {
        t->entry=entry; t->arg=arg;
        t->native=CreateThread(NULL,0,thread_entry,t,0,NULL);
        assert(t->native); return true;
    }
    entry(arg); return true;
}
static void thread_dispose(struct thread* t,bool completed) {
    COUNT(threads); COUNT(disposes);
    assert(t->created && completed);
    if(paired) {
        assert(WaitForSingleObject(t->native,1000)==WAIT_OBJECT_0);
        assert(CloseHandle(t->native));
    }
    t->created=false;
}
#include "../source/platform/wii/net_bringup.h"
void net_bringup_request(void) { COUNT(net); COUNT(requests); }
enum net_bringup_state net_bringup_status(char* error,size_t size) {
    COUNT(net); COUNT(status);
    assert(!error && !size); return NET_BRINGUP_READY;
}
static int net_socket(int domain,int type,int protocol) {
    COUNT(net); COUNT(sockets);
    assert(domain==AF_INET && type==SOCK_STREAM && protocol==IPPROTO_IP);
    if(!paired) return -EIO;
    SOCKET native=socket(domain,type,protocol);
    if(native==INVALID_SOCKET) return socket_error();
    int fd=slot(native); sockets[fd].listener=true; listener_opens++;
    return fd;
}
static int net_fcntl(int fd,int command,int flags) {
    COUNT(net);
    if(!paired) return -EIO;
    if(command==F_GETFL) return sockets[fd].flags;
    assert(command==F_SETFL);
    u_long on=(flags&4)!=0;
    if(ioctlsocket(sockets[fd].native,FIONBIO,&on)) return socket_error();
    sockets[fd].flags=flags; return 0;
}
static int net_setsockopt(int fd,int level,int option,const void* value,socklen_t size)
    __attribute__((unused));
static int net_setsockopt(int fd,int level,int option,const void* value,socklen_t size) {
    COUNT(net);
    assert(paired && sockets[fd].listener && level==SOL_SOCKET && option==SO_REUSEADDR);
    assert(size==sizeof(int) && *(const int*)value==1);
    return setsockopt(sockets[fd].native,level,option,value,(int)size) ? socket_error() : 0;
}
static int net_bind(int fd,const struct sockaddr* addr,socklen_t len) {
    COUNT(net);
    if(!paired) return -EIO;
    const struct sockaddr_in* requested=(const struct sockaddr_in*)addr;
    assert(len==sizeof(*requested) && ntohs(requested->sin_port)==39020);
    struct sockaddr_in local=*requested;
    local.sin_port=htons(loopback_port); local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    return bind(sockets[fd].native,(struct sockaddr*)&local,sizeof(local)) ? socket_error() : 0;
}
static int net_listen(int fd,int backlog) {
    COUNT(net);
    if(!paired) return -EIO;
    assert(backlog==1);
    if(listen(sockets[fd].native,backlog)) return socket_error();
    struct sockaddr_in local; int len=sizeof(local);
    assert(!getsockname(sockets[fd].native,(struct sockaddr*)&local,&len));
    char marker[40]; snprintf(marker,sizeof(marker),"generation-%u.ready",listener_opens);
    FILE* generation=fopen(marker,"w"); assert(generation); assert(!fclose(generation));
    if(loopback_port) { assert(loopback_port==ntohs(local.sin_port)); return 0; }
    loopback_port=ntohs(local.sin_port);
    FILE* ready=fopen("port.pending","w"); assert(ready);
    assert(fprintf(ready,"%u\n",ntohs(local.sin_port))>0); assert(!fclose(ready));
    assert(!rename("port.pending","port.txt"));
    return 0;
}
static int net_accept(int fd,struct sockaddr* addr,socklen_t* len) {
    COUNT(net);
    if(!paired) return -EIO;
    /* X04b model: accepted close leaves this generation unable to admit again.
     * Native socket remains allocated until the production listener close. */
    if(!strncmp(scenario,"recreate-",9) && sockets[fd].dead) return -EAGAIN;
    int host_len=(int)*len;
    SOCKET native=accept(sockets[fd].native,addr,&host_len);
    if(native==INVALID_SOCKET) return socket_error();
    *len=(socklen_t)host_len;
    sent_bytes=header_size=0; request_complete=false; first_send_ms=0;
    return slot(native);
}
static int net_send(int fd,const void* data,size_t len,int flags) {
    COUNT(net);
    if(!paired) return -EIO;
    sends++;
    if(terminal_fault) retries_after_fault++;
    if(!header_size) header_size=(unsigned)len;
    bool payload=sent_bytes>=header_size;
    bool target=payload ? strstr(scenario,"payload")!=NULL : strstr(scenario,"header")!=NULL;
    bool injecting=target && !fault_hits;
    size_t want=len;
    if(injecting) {
        fault_hits++;
        if(strstr(scenario,"zero")) { terminal_fault=true; return 0; }
        want=len>7 ? 7 : 1;
    }
    int n=send(sockets[fd].native,data,(int)want,flags);
    if(n>0) {
        sent_bytes+=(unsigned)n;
        if(sent_bytes>=header_size) PUBLISH(header_sent,true);
        if(!first_send_ms) first_send_ms=GetTickCount64();
    }
    if(injecting && n>0 && strstr(scenario,"-stop")) prefix_stop();
    if(injecting && n>0 && strstr(scenario,"-deadline")) {
        __atomic_fetch_add(&virtual_ms,120001,__ATOMIC_RELAXED);
        PUBLISH(deadline_injected,true);
    }
    if(injecting && strstr(scenario,"ambiguous")) {
        terminal_fault=true; return -EAGAIN;
    }
    return n==SOCKET_ERROR ? socket_error() : n;
}
static int net_recv(int fd,void* data,size_t len,int flags) {
    COUNT(net);
    if(!paired) return -EIO;
    bool expire_credit=!strcmp(scenario,"missing-credit") && request_complete && header_size;
    bool expire_request=!strcmp(scenario,"deadline") && !request_complete;
    if(!ATOM(deadline_injected) && (expire_request || (expire_credit && GetTickCount64()-first_send_ms>100))) {
        __atomic_fetch_add(&virtual_ms,expire_request ? 5001 : 120001,__ATOMIC_RELAXED);
        PUBLISH(deadline_injected,true);
    }
    int n=recv(sockets[fd].native,data,(int)len,flags);
    if(n>0) {
        assert(n==1);
        char c=*(char*)data;
        if(request_complete) {
            if(c=='+') { assert(credits<64); credit_offsets[credits++]=sent_bytes; }
            else wrong_credits++;
        } else if(c=='\n') request_complete=true;
    }
    return n==SOCKET_ERROR ? socket_error() : n;
}
static int net_close(int fd) {
    COUNT(net);
    if(!paired) return 0;
    if(request_complete && header_size) {
        assert(session_count<8);
        session_credits[session_count]=credits;
        session_bytes[session_count++]=sent_bytes;
        header_size=0;
    }
    bool listener=sockets[fd].listener;
    if(listener) listener_closes++;
    else if(!strncmp(scenario,"recreate-",9)) {
        for(unsigned i=1;i<16;i++) if(sockets[i].listener && sockets[i].native!=INVALID_SOCKET)
            sockets[i].dead=true;
    }
    assert(closesocket(sockets[fd].native)==0);
    sockets[fd].native=INVALID_SOCKET; socket_closes++;
    if(!listener) __atomic_fetch_add(&connections,1,__ATOMIC_RELEASE);
    return 0;
}
static bool gfx_write_framebuffer_raw(const char* path) {
    static const unsigned char pixel[]={ 'C','W','F','B',0,0,0,1,0,0,0,1,12,34,56,255 };
    COUNT(gx);
    FILE* fp=io_open(path,"wb");
    assert(fp);
    if(paired) {
        if(!strcmp(scenario,"claimed-stop")) claimed_shutdown();
        FILE* fixture=fopen("fixture.rgba","rb"); assert(fixture);
        unsigned char buf[4096]; size_t n;
        while((n=fread(buf,1,sizeof(buf),fixture))!=0)
            assert(io_write(buf,1,n,fp)==n);
        assert(!ferror(fixture)); assert(!fclose(fixture));
        return io_close(fp)==0;
    }
    bool ok=io_write(pixel,1,sizeof(pixel),fp)==sizeof(pixel);
    return io_close(fp)==0 && ok;
}

#define fopen io_open
#define fread io_read
#define fwrite io_write
#define fprintf io_print
#define ferror io_error
#define feof io_eof
#define fclose io_close
#ifdef REMOTE_CAPTURE_SOURCE
#include REMOTE_CAPTURE_SOURCE
#else
#include "../source/platform/remote_capture.c"
#endif
#undef fopen
#undef fread
#undef fwrite
#undef fprintf
#undef ferror
#undef feof
#undef fclose

static void prefix_stop(void) { STORE(stop,true); }
static void claimed_shutdown(void) {
    assert(LOAD(phase)==SHOT_CAPTURING && ATOM(capture_writers)==1);
    remote_capture_shutdown();
    assert(LOAD(done) && !started && ATOM(capture_readers)==0);
    claimed_shutdown_seen=true;
}

static int b01_main(int argc,char** argv) {
    assert(argc==2);
    const char* name=argv[1];
    bool unarmed=!strcmp(name,"unarmed");
    bool valid=!strcmp(name,"valid") || !strcmp(name,"max128")
        || !strcmp(name,"crlf") || !strcmp(name,"max128-crlf");
    fail_open=!strcmp(name,"open-failure");
    fail_read=!strcmp(name,"read-failure");
    FILE* fp=fopen("status.txt","wb"); assert(fp);
    assert(fwrite(sentinel,1,sizeof(sentinel)-1,fp)==sizeof(sentinel)-1);
    assert(fclose(fp)==0);

    remote_capture_init(!unarmed,"B01-test");
    bool armed=enabled;
    size_t secret_length=strlen(secret);
    remote_capture_frame();
    if(!strcmp(name,"valid")) {
        STORE(phase,SHOT_REQUEST);
        remote_capture_frame();
        assert(LOAD(phase)==SHOT_READY);
    }
    remote_capture_shutdown();
    char diagnostic[256]={0};
    fp=fopen("status.txt","rb"); assert(fp);
    size_t n=fread(diagnostic,1,sizeof(diagnostic),fp);
    assert(!ferror(fp)); assert(fclose(fp)==0);
    bool unchanged=n==sizeof(sentinel)-1 && !memcmp(diagnostic,sentinel,n);
    printf("WITNESS %s: armed=%d secret_length=%zu status_opens=%u truncates=%u "
           "opens=%u reads=%u writes=%u prints=%u net=%u requests=%u "
           "threads=%u spawns=%u gx=%u sentinel_unchanged=%d\n",
           name,armed,secret_length,calls.status_opens,calls.truncates,
           calls.opens,calls.reads,calls.writes,calls.prints,calls.net,
           calls.requests,calls.threads,calls.spawns,calls.gx,unchanged);
    fflush(stdout);
    if(unarmed) {
        assert(calls.status_opens==0 && calls.truncates==0);
        assert(calls.opens==0 && calls.reads==0 && calls.writes==0);
        assert(calls.prints==0 && calls.closes==0 && calls.errors==0 && calls.eof_checks==0);
        assert(unchanged);
        assert(!armed && calls.net==0 && calls.threads==0 && calls.gx==0 && calls.clock==0);
    } else if(!valid) {
        assert(calls.requests==0 && calls.net==0);
        assert(!armed && secret_length==0);
        assert(calls.threads==0 && calls.gx==0 && calls.clock==0);
        assert(calls.token_opens==1);
    } else {
        assert(armed && calls.requests==1 && calls.status==1 && calls.sockets==1);
        assert(calls.spawns==1 && calls.disposes==1);
        assert(secret_length==((!strcmp(name,"max128") || !strcmp(name,"max128-crlf")) ? 128 : 6));
        assert(calls.gx==(!strcmp(name,"valid") ? 1u : 0u));
        assert(calls.writes==(!strcmp(name,"valid") ? 1u : 0u));
    }
    puts("PASS: real Wii-branch init/frame/shutdown assertions");
    return 0;
}

static int protocol_main(const char* name) {
    paired=true; scenario=name;
    WSADATA data; assert(!WSAStartup(MAKEWORD(2,2),&data));
    for(unsigned i=0;i<16;i++) sockets[i].native=INVALID_SOCKET;
    unsigned wanted=!strcmp(name,"recreate-mixed") ? 8 : !strcmp(name,"recreate-six") ? 6 : !strcmp(name,"replay") ? 2 : 1;
    remote_capture_init(true,"B02-test");
    assert(enabled && calls.requests==1);
    remote_capture_frame(); /* Real worker starts the loopback-bound listener. */
    uint64_t start_ms=GetTickCount64();
    bool shutdown_called=false;
    while(ATOM(connections)<wanted && GetTickCount64()-start_ms<5000) {
        if(!strcmp(name,"cancel-unclaimed") && LOAD(phase)==SHOT_REQUEST) {
            if(!ATOM(deadline_injected)) {
                __atomic_fetch_add(&virtual_ms,15001,__ATOMIC_RELAXED);
                PUBLISH(deadline_injected,true);
            }
        } else {
            remote_capture_frame();
        }
        if((!strcmp(name,"stop") && ATOM(header_sent))
           || (!strcmp(name,"ready-stop") && calls.gx)) {
            remote_capture_shutdown(); shutdown_called=true; break;
        }
        Sleep(1);
    }
    if(!shutdown_called) remote_capture_shutdown();
    printf("SERVICE %s: sessions=%u gx=%u sends=%u fault_hits=%u retries_after_fault=%u "
           "credits=%u wrong_credits=%u socket_closes=%u active_files=%u readers=%u writers=%u "
           "done=%d phase=%u offsets=",name,ATOM(connections),calls.gx,sends,fault_hits,
           retries_after_fault,credits,wrong_credits,socket_closes,ATOM(active_files),
           ATOM(capture_readers),ATOM(capture_writers),LOAD(done),LOAD(phase));
    for(unsigned i=0;i<credits;i++) printf("%s%u",i ? "," : "",credit_offsets[i]);
    printf(" bytes=");
    for(unsigned i=0;i<session_count;i++) printf("%s%u",i ? "," : "",session_bytes[i]);
    puts(""); fflush(stdout);
    assert(LOAD(done) && !started && !worker.created);
    assert(ATOM(active_files)==0 && ATOM(capture_readers)==0 && ATOM(capture_writers)==0);
    assert(ATOM(connections)==wanted && socket_closes==wanted+listener_opens);
    assert(listener_closes==listener_opens);
    for(unsigned i=0;i<16;i++) assert(sockets[i].native==INVALID_SOCKET);
    assert(retries_after_fault==0);
    bool success=!strcmp(name,"pair") || !strcmp(name,"short-header")
        || !strcmp(name,"short-payload") || !strcmp(name,"replay") || !strcmp(name,"recreate-six") || !strcmp(name,"recreate-mixed");
    if(success) {
        unsigned captures=!strcmp(name,"recreate-mixed") ? 6 : wanted;
        assert(calls.gx==captures && credits==captures*7 && !wrong_credits);
        for(unsigned session=0;session<captures;session++) {
            assert(session_bytes[session]==55+5488 && session_credits[session]==(session+1)*7);
            for(unsigned part=0;part<7;part++) {
                unsigned offset=part==6 ? 55+5488 : 55+part*1024;
                assert(credit_offsets[session*7+part]==offset);
            }
        }
        if(strstr(name,"short")) assert(fault_hits==1 && sends==8);
        else assert(sends==captures*7);
    } else if(strstr(name,"zero") || strstr(name,"ambiguous")) {
        bool payload=strstr(name,"payload")!=NULL;
        assert(fault_hits==1 && credits==(payload ? 1u : 0u));
        assert(sends==(payload ? 2u : 1u));
        assert(session_bytes[0]==(payload ? 55u : 0u)+(strstr(name,"ambiguous") ? 7u : 0u));
    } else if(strstr(name,"short-") && (strstr(name,"-stop") || strstr(name,"-deadline"))) {
        bool payload=strstr(name,"payload")!=NULL;
        assert(fault_hits==1 && sends==(payload ? 2u : 1u) && credits==(payload ? 1u : 0u));
        assert(session_bytes[0]==(payload ? 55u+7u : 7u));
        if(strstr(name,"-deadline")) assert(ATOM(deadline_injected));
    } else if(!strcmp(name,"claimed-stop")) {
        assert(claimed_shutdown_seen && calls.gx==1 && sends==0 && credits==0);
    } else if(!strcmp(name,"wrong-credit")) {
        assert(wrong_credits==1 && credits==0 && sends==1);
    } else if(!strcmp(name,"deadline") || !strcmp(name,"cancel-unclaimed")) {
        assert(calls.gx==0 && sends==0 && credits==0 && deadline_injected);
    } else {
        assert(credits==0 && sends<=1);
        if(!strcmp(name,"missing-credit")) assert(deadline_injected);
    }
    assert(!WSACleanup());
    puts("PASS: production loopback service, credit boundaries, cleanup and bounded shutdown");
    return 0;
}

int main(int argc,char** argv) {
    /* Assertion failures must terminate, without a Windows crash-report dialog. */
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0,_CALL_REPORTFAULT);
    if(argc==3 && !strcmp(argv[1],"--pair")) return protocol_main(argv[2]);
    return b01_main(argc,argv);
}
