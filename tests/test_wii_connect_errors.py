"""Compile the actual Wii connect path against deterministic IOS responses."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'source/platform/wii/net_socket.c').read_text(encoding='utf-8')
start=source.index('net_socket_t net_socket_connect(')
end=source.index('\nint net_socket_receive(',start)
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
typedef int32_t s32;typedef uint32_t u32;typedef int net_socket_t;typedef int ptime_t;
struct sockaddr {int family;};
struct sockaddr_in {int sin_family;uint16_t sin_port;struct {u32 s_addr;} sin_addr;};
struct hostent {int h_addrtype,h_length;char* h_addr;};
#define AF_INET 2
#define SOCK_STREAM 1
#define IPPROTO_IP 0
#define IPPROTO_TCP 6
#define TCP_NODELAY 1
#define NET_SOCKET_INVALID -1
#define NET_CONNECT_TIMEOUT_MS 10000
#define CW_SD_TRACE_OFF() true
static int response,next,calls,closes;static bool wait_ok;
static u32 inet_addr(const char* h) {u32 a;const uint8_t ip[]={192,168,4,33};memcpy(&a,ip,4);return a;}
static uint16_t htons(uint16_t p){return p;}
static struct hostent* net_gethostbyname(const char* h){return NULL;}
static s32 net_socket(int a,int b,int c){return 7;}
static bool wii_set_nonblock(s32 s){return true;}
static int net_close(s32 s){closes++;return 0;}
static void wii_set_error(char* e,size_t n,const char* s){if(e&&n)snprintf(e,n,"%s",s);}
static ptime_t time_get(void){return 0;}
static ptime_t time_add_ms(ptime_t t,int ms){return t+ms;}
static s32 net_connect(s32 s,struct sockaddr* a,size_t n){calls++;int r=response;response=next;return r;}
static bool wii_wait_io(s32 s,bool w,ptime_t d){return wait_ok;}
static int net_setsockopt(s32 s,int p,int o,const void* v,size_t n){return 0;}
FUNCTION
int main(void){char error[80],expected[80];
 response=-ECONNREFUSED;
 assert(net_socket_connect("192.0.2.33",25566,error,sizeof error)==-1);
 snprintf(expected,sizeof expected,"connect %d to 192.0.2.33:25566",-ECONNREFUSED);
 assert(strcmp(error,expected)==0 && closes==1 && calls==1);
 response=-EINPROGRESS;wait_ok=false;
 assert(net_socket_connect("192.0.2.33",25566,error,sizeof error)==-1);
 snprintf(expected,sizeof expected,"connect %d to 192.0.2.33:25566",-ETIMEDOUT);
 assert(strcmp(error,expected)==0 && closes==2);
 response=-EINPROGRESS;next=-EISCONN;wait_ok=true;
 assert(net_socket_connect("192.0.2.33",25566,error,sizeof error)==7);
 assert(closes==2);
 response=-EINVAL;
 assert(net_socket_connect("192.0.2.33",25566,NULL,0)==-1);
}
'''.replace('FUNCTION',source[start:end])
with tempfile.TemporaryDirectory() as temp:
 c=Path(temp)/'connect.c';c.write_text(code);exe=c.with_suffix('.exe')
 subprocess.run(['C:/msys64/ucrt64/bin/gcc.exe','-UNDEBUG',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual Wii connect preserves refusal/destination, distinguishes timeout, retries and closes failures')
