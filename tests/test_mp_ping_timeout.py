"""Exercise the real row timeout handler: optional status silence is not TCP failure."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "source/game/gui/screen_multiplayer_setup.c").read_text(encoding="utf-8")
start = source.index("static void mps_ping_apply_abandon(")
end = source.index("\n/*", start)
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
enum { MPS_PING_UNREACHABLE, MPS_PING_REACHABLE };
static struct {int state; int32_t latency_ms; bool has_status;} row_ping[2];
static int ping_thread, token;
static void* ping_handoff;
static bool ping_in_flight;
static size_t ping_row_index,server_count=2,ping_cursor;
static void thread_dispose(int* thread,bool done) {assert(!done);}
static void mps_ping_handoff_unref(void* handoff) {assert(handoff==&token);}
FUNCTION
int main(void) {
 ping_handoff=&token;ping_in_flight=true;row_ping[0].has_status=true;
 mps_ping_apply_abandon(true,37);
 assert(row_ping[0].state==MPS_PING_REACHABLE && row_ping[0].latency_ms==37);
 assert(!row_ping[0].has_status && !ping_in_flight && !ping_handoff && ping_cursor==1);
 ping_handoff=&token;ping_in_flight=true;ping_row_index=1;
 mps_ping_apply_abandon(false,0);
 assert(row_ping[1].state==MPS_PING_UNREACHABLE && row_ping[1].latency_ms==-1);
 assert(ping_cursor==2);
}
'''.replace("FUNCTION",source[start:end])
# The worker must publish its successful connect before it enters optional status I/O.
worker = source[source.index("static void* mps_ping_worker"):start]
assert worker.index("h->reachable = true;") < worker.index("h->phase = 1;")
assert "mps_ping_apply_abandon(reachable, latency);" in source[end:]
with tempfile.TemporaryDirectory() as temp:
    c=Path(temp)/"ping.c";c.write_text(code);exe=c.with_suffix(".exe")
    subprocess.run(["C:/msys64/ucrt64/bin/gcc.exe","-UNDEBUG",str(c),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("PASS: status timeout preserves completed TCP reachability; TCP timeout remains unreachable")
