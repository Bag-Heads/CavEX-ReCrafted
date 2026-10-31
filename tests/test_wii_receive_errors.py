from pathlib import Path
import subprocess,tempfile
root=Path.cwd();s=(root/'source/platform/wii/net_socket.c').read_text();a=s.index('int net_socket_receive(');b=s.index('\nint net_socket_send(',a)
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
typedef int32_t s32;typedef intptr_t net_socket_t;typedef uint64_t ptime_t;
#define NET_IOS_MAX_CHUNK 4096
#define NET_IO_TIMEOUT_MS 30000
#define CW_SD_TRACE_OFF() true
static int result,next,waits;static bool retry;
ptime_t time_get(void){return 0;}ptime_t time_add_ms(ptime_t t,unsigned ms){return t+ms;}
s32 net_recv(s32 sock,void* d,s32 len,int flags){int r=result;result=next;return r;}
bool wii_wait_io(s32 s,bool write,ptime_t end){waits++;return retry;}
FUNCTION
int main(void){char b[8];
 result=-ECONNRESET;assert(net_socket_receive(1,b,8)==-ECONNRESET);
 result=-EAGAIN;retry=false;assert(net_socket_receive(1,b,8)==-ETIMEDOUT&&waits==1);
 result=-EAGAIN;next=4;retry=true;assert(net_socket_receive(1,b,8)==4);
 result=0;assert(net_socket_receive(1,b,8)==0);
 assert(net_socket_receive(-1,b,8)==-EINVAL);
}
'''.replace('FUNCTION',s[a:b])
with tempfile.TemporaryDirectory() as t:
 c=Path(t)/'recv.c';e=c.with_suffix('.exe');c.write_text(code)
 subprocess.run(['C:/msys64/ucrt64/bin/gcc.exe','-UNDEBUG',str(c),'-o',str(e)],check=True);subprocess.run([str(e)],check=True)
print('PASS real Wii receive: reset preserved, timeout distinct, transient retry, EOF and invalid argument')
