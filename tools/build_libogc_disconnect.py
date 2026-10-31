"""Build an isolated libbte fix; never modify the installed/shared libogc.

Usage: python tools/build_libogc_disconnect.py SIDE_CAR OUTPUT_DIRECTORY
The extracted-handler regression runs against original and patched code before
replacing l2cap.o in a copy of the sidecar's libbte.a.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def patched(text):
    lookup = "for(pcb = l2cap_active_pcbs; pcb != NULL; pcb = pcb->next) {\n\t\t\t\tfor(sig = pcb->unrsp_sigs;"
    replacement = "for(pcb = l2cap_active_pcbs; pcb != NULL; pcb = pcb->next) {\n\t\t\t\t/* Identifiers are scoped to the remote Bluetooth link. */\n\t\t\t\tif(!bd_addr_cmp(&pcb->remote_bdaddr, bdaddr)) continue;\n\t\t\t\tfor(sig = pcb->unrsp_sigs;"
    reject = "case L2CAP_CMD_REJ:\n\t\t\t\t/* Remove signal"
    guard = "case L2CAP_CMD_REJ:\n\t\t\t\t/* A late/duplicate rejection can outlive its request or link.\n\t\t\t\t * Match the existing DISCONN_RSP policy: ignore unmatched replies. */\n\t\t\t\tif(pcb == NULL || sig == NULL) break;\n\t\t\t\t/* Remove signal"
    assert text.count(lookup) == text.count(reject) == 1, "sidecar source changed; review patch"
    return text.replace(lookup, replacement).replace(reject, guard)


def regression(text, out, expected_safe):
    start = text.index("\t\t\tfor(pcb = l2cap_active_pcbs", text.index("if(sighdr->code % 2)"))
    lookup = text[start:text.index("\n\t\t} else {", start)]
    start = text.index("case L2CAP_CMD_REJ:", start)
    reject = text[start:text.index("case L2CAP_CONN_REQ:", start)]
    harness = r'''
#include <assert.h>
#include <stddef.h>
struct bd_addr { int id; };
struct l2cap_sig { struct l2cap_sig *next; int sigid; void *p; };
struct l2cap_pcb { struct l2cap_pcb *next; struct l2cap_sig *unrsp_sigs; struct bd_addr remote_bdaddr; };
struct l2cap_pcb *l2cap_active_pcbs;
struct hdr { int id, code; };
enum { L2CAP_CMD_REJ=1 };
static int faults, frees, disconnects, l2cap_sigs;
#define LOG(...) ((void)0)
#define bd_addr_cmp(a,b) ((a)->id == (b)->id)
/* Detect the original dereference without crashing the test host. */
#define L2CAP_SIG_RMV(list,s) do { if(!pcb || !sig) { faults++; return; } \
 struct l2cap_sig **at=(list); while(*at && *at!=(s)) at=&(*at)->next; \
 assert(*at==(s)); *at=(s)->next; } while(0)
static void btpbuf_free(void *p) { (void)p; frees++; }
static void btmemb_free(void *pool, void *p) { (void)pool; (void)p; }
static void l2ca_disconnect_req(struct l2cap_pcb *p, void *cb) { assert(p); (void)cb; disconnects++; }
static void receive(struct bd_addr *bdaddr, int id) {
 struct hdr h={id,1}, *sighdr=&h;
 struct l2cap_pcb *pcb=NULL;
 struct l2cap_sig *sig=NULL;
 LOOKUP
 switch(sighdr->code) { REJECT }
}
int main(void) {
 struct bd_addr peer={1}, other={2};
 receive(&peer,7);
 if(!EXPECTED_SAFE) { assert(faults==1); return 0; }
 assert(faults==0 && frees==0 && disconnects==0);
 struct l2cap_sig sig={NULL,7,NULL}, sig2={NULL,7,NULL};
 struct l2cap_pcb second={NULL,&sig2,peer}, first={&second,&sig,other};
 l2cap_active_pcbs=&first;
 receive(&peer,99); assert(faults==0 && disconnects==0);
 receive(&peer,7); assert(first.unrsp_sigs==&sig && second.unrsp_sigs==NULL);
 assert(disconnects==1 && frees==1);
 receive(&peer,7); assert(disconnects==1 && frees==1 && faults==0);
 receive(&other,7); assert(disconnects==2 && frees==2 && first.unrsp_sigs==NULL);
}
'''.replace("LOOKUP", lookup).replace("REJECT", reject).replace("EXPECTED_SAFE", str(int(expected_safe)))
    c = out / ("patched_test.c" if expected_safe else "original_test.c")
    c.write_text(harness)
    exe = c.with_suffix(".exe")
    subprocess.run(["C:/msys64/ucrt64/bin/gcc.exe", "-UNDEBUG", "-Wall", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def main():
    sidecar, out = (Path(p).resolve() for p in sys.argv[1:])
    out.mkdir(parents=True, exist_ok=True)
    original = (sidecar / "lwbt/l2cap.c").read_text()
    fixed = patched(original)
    regression(original, out, False)
    regression(fixed, out, True)
    print("PASS: original unmatched reject reaches null dereference; patched late/duplicate/foreign-peer rejection cases", flush=True)
    source = out / "l2cap.c"
    source.write_text(fixed)
    libs = out / "stage-lib"
    shutil.copytree(sidecar / "stage-lib", libs, dirs_exist_ok=True)
    flags = (sidecar / "build/libogc.wii/lwbt/CMakeFiles/bte.dir/flags.make").read_text()
    args = []
    for key in ("C_DEFINES", "C_INCLUDES", "C_FLAGS"):
        value = next(line.split(" = ", 1)[1] for line in flags.splitlines() if line.startswith(key + " = "))
        args += value.replace("-I/c/", "-IC:/").split()
    obj = out / "l2cap.o"
    compiler = "C:/msys64/opt/devkitpro/devkitPPC/bin/powerpc-eabi-"
    subprocess.run([compiler + "gcc.exe", *args, "-I" + str(sidecar / "lwbt"), "-c", str(source), "-o", str(obj)], check=True)
    archive = libs / "libbte.a"
    members = subprocess.check_output([compiler + "ar.exe", "t", str(archive)], text=True).splitlines()
    assert members.count("l2cap.o") == 1, "unexpected archive layout"
    subprocess.run([compiler + "ar.exe", "r", str(archive), str(obj)], check=True)
    subprocess.run([compiler + "ranlib.exe", str(archive)], check=True)
    (out / "provenance.json").write_text(json.dumps({
        "base": str(sidecar), "source_sha256": hashlib.sha256(original.encode()).hexdigest(),
        "patched_sha256": hashlib.sha256(fixed.encode()).hexdigest(),
        "libbte_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        "regression": "PASS: original failure witnessed, patched routing/duplicates pass"
    }, indent=2))
    print("READY:", libs)


if __name__ == "__main__":
    main()
