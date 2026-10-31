"""Exercise production request/transfer logic with deterministic socket/GX fakes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "source/platform/remote_capture.c").read_text()
logic = source[source.index("enum { SHOT_IDLE"):source.index("static void* run(")]
logic = logic.replace('#define SHOT_PATH "sd:/apps/CavEX/remote_capture.rgba"',
                      '#define SHOT_PATH "capture.rgba"')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#endif
#include "platform/remote_capture_protocol.h"
struct thread { int unused; };
static uint64_t clock_ms;
static const char* input;
static size_t input_at, output_at;
static char output[1024];
static bool render, fail_capture, stall_send;
static unsigned captures, sends;
static uint64_t gettime(void) { return clock_ms; }
static uint64_t ticks_to_millisecs(uint64_t n) { return n; }
static int net_fcntl(int fd,int op,int flags) { return 4; }
static int net_recv(int fd,void* p,size_t n,int flags) {
 if(!input[input_at]) { *(char*)p='+'; return 1; }
 *(char*)p=input[input_at++]; return 1;
}
static int net_send(int fd,const void* p,size_t n,int flags) {
 sends++; assert(n<=1024);
 if(stall_send) { /* IOS may queue a prefix despite its negative return. */
  memcpy(output+output_at,p,1);output_at++;return -EAGAIN;
 }

 assert(output_at+n<sizeof output);
 memcpy(output+output_at,p,n);output_at+=n;return (int)n;
}
static void thread_msleep(size_t ms);
LOGIC
static void thread_msleep(size_t ms) {
 clock_ms+=ms;
 if(render && LOAD(phase)==SHOT_REQUEST) {
  STORE(phase,SHOT_CAPTURING); captures++;
  if(!fail_capture) {
   FILE* f=fopen(SHOT_PATH,"wb");assert(f);
   const unsigned char frame[]={ 'C','W','F','B',0,0,0,1,0,0,0,1,12,34,56,255 };
   assert(fwrite(frame,1,sizeof frame,f)==sizeof frame);assert(fclose(f)==0);
  }
  STORE(phase,fail_capture ? SHOT_FAILED : SHOT_READY);
 }
}
static void reset(const char* request) {
 input=request;input_at=output_at=0;memset(output,0,sizeof output);
 clock_ms=0;captures=0;sends=0;render=true;fail_capture=stall_send=false;
 STORE(phase,SHOT_IDLE); STORE(stop,false);
 snprintf(secret,sizeof secret,"secret"); snprintf(build_id,sizeof build_id,"test");
}
int main(void) {
 const char* good="CWSHOT1 secret 0123456789abcdef0123456789abcdef\n";
 reset(good);serve(1);
 assert(captures==1 && LOAD(phase)==SHOT_IDLE);
 assert(strstr(output,"CWSHOT1 0123456789abcdef0123456789abcdef test 16\nCWFB"));
 reset("CWSHOT1 wrong 0123456789abcdef0123456789abcdef\n");serve(1);
 assert(captures==0 && output_at==0);
 reset("CWSHOT1 secret 0123456789abcdef0123456789abcdef");serve(1);
 assert(captures==0 && output_at==0);
 reset(good);render=false;serve(1);
 assert(captures==0 && output_at==0 && LOAD(phase)==SHOT_IDLE && clock_ms>=15000);
 reset(good);fail_capture=true;serve(1);
 assert(captures==1 && output_at==0 && LOAD(phase)==SHOT_IDLE);
 reset(good);stall_send=true;serve(1);
 assert(captures==1 && output_at==1 && sends==1 && LOAD(phase)==SHOT_IDLE);
 reset(good);serve(1);assert(captures==1 && output_at>0); /* Recovery after timeout. */
 puts("PASS: production request handoff, auth rejection, capture failure, receiver credit, ambiguous-send rejection and recovery");
}
'''.replace("LOGIC", logic)
with tempfile.TemporaryDirectory() as td:
    folder = Path(td)
    c = folder / "handoff.c"
    c.write_text(code)
    exe = folder / "handoff.exe"
    subprocess.run(["C:/msys64/ucrt64/bin/gcc.exe", "-UNDEBUG", "-I"+str(root/"source"),
                    str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], cwd=folder, check=True)
