"""Exercise auth grammar and the real capture client's success/failure transport."""
import argparse
import importlib.util
import os
from pathlib import Path
import runpy
import socket
import struct
import subprocess
import sys
import tempfile
import threading
from unittest.mock import patch

sys.dont_write_bytecode = True

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
args = parser.parse_args()
spec = importlib.util.spec_from_file_location("capture", root / "tools/capture_wii_live.py")
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)

# Token rejection must happen before any connection or output side effect.
for token in ("", "x" * 129, "sec ret", "sec\tret", "sec\rret", "sec\nret",
              "sec\vret", "sec\fret", "a\0remaining-secret", "\0secret", "secret\0"):
    print(f"WITNESS client token={token!r}: must reject before network", flush=True)
    with patch.object(client.socket, "create_connection", side_effect=AssertionError("invalid token reached network")):
        try:
            client.capture("unused", token, Path("unused.png"))
        except ValueError as error:
            assert str(error) == "Invalid capture token"
        else:
            raise AssertionError("invalid token accepted")
print("PASS: invalid tokens (including NUL) rejected before network/output")

# Exercise the actual argparse/__main__ path and real token-file reads, with
# only socket creation substituted. A valid file stops at that counted adapter.
class SocketReached(Exception):
    pass


cli_cases = (
    ("leading-space", b" secret", False),
    ("trailing-space", b"secret ", False),
    ("surrounding-tabs", b"\tsecret\t", False),
    ("leading-cr", b"\rsecret\n", False),
    ("leading-space-max128", b" " + b"x" * 128, False),
    ("non-ascii-utf8", "\u00e9".encode("utf-8"), False),
    ("non-ascii-128-characters", "\u00e9".encode("utf-8") * 128, False),
    ("non-ascii-byte", b"\xff", False),
    ("nul", b"sec\0ret", False),
    ("control-low", b"\x1fsecret", False),
    ("control-del", b"secret\x7f", False),
    ("empty", b"", False),
    ("over129", b"x" * 129, False),
    ("crlf", b"secret\r\n", True),
    ("max128-crlf", b"x" * 128 + b"\r\n", True),
    ("printable-boundaries", b"!~", True),
)
job_dir = Path(__file__).resolve().parents[1] / "tmp/jobs/capture-fix-1003"
job_dir.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="B04-cli-", dir=job_dir) as td:
    folder = Path(td)
    failed = []
    for name, data, accepted in cli_cases:
        token_file = folder / (name + ".token")
        token_file.write_bytes(data)
        output = folder / (name + ".png")
        argv = ["capture_wii_live.py", "--host", "unused", "--token-file", str(token_file),
                "--output", str(output)]
        with patch.object(sys, "argv", argv), patch.object(socket, "create_connection", side_effect=SocketReached) as network:
            observed = "returned"
            try:
                runpy.run_path(str(root / "tools/capture_wii_live.py"), run_name="__main__")
            except SocketReached:
                observed = "socket"
            except ValueError as error:
                observed = "rejected" if str(error) == "Invalid capture token" else type(error).__name__
            expected_calls = 1 if accepted else 0
            ok = observed == ("socket" if accepted else "rejected") and network.call_count == expected_calls
            ok = ok and not any(output.with_suffix(suffix).exists() for suffix in (".png", ".rgba", ".json"))
            print(f"WITNESS CLI {name}: socket_calls={network.call_count} observed={observed} "
                  f"expected={'socket' if accepted else 'rejected'} {'PASS' if ok else 'FAIL'}", flush=True)
            if not ok:
                failed.append(name)
    assert not failed, f"CLI token contract failures: {failed}"
print("PASS: real CLI raw token files, CRLF-only trimming, byte domain before sockets")

for token in ("\u00e9", "\u00e9" * 128, b"\xff", b"\x1f", b"\x7f", b" ", b"\0", b"x" * 129):
    with patch.object(client.socket, "create_connection", side_effect=AssertionError("invalid wire token reached network")) as network:
        try:
            client.capture("unused", token, Path("unused.png"))
        except ValueError as error:
            assert str(error) == "Invalid capture token"
        else:
            raise AssertionError("invalid wire token accepted")
        assert network.call_count == 0
print("PASS: function str/byte tokens enforce the ASCII byte domain before sockets")

with tempfile.TemporaryDirectory() as td:
    folder = Path(td)
    code = r'''
#include <assert.h>
#include "platform/remote_capture_protocol.h"
int main(void) {
 char n[33];
 const char* good="CWSHOT1 secret 0123456789abcdef0123456789abcdef";
 assert(remote_capture_auth(good,"secret",n));
 assert(!remote_capture_auth(good,"",n));
 assert(!remote_capture_auth(good,"secreu",n));
 assert(!remote_capture_auth(good,"secretx",n));
 assert(!remote_capture_auth("CWSHOT1 secret short","secret",n));
 assert(!remote_capture_auth("CWSHOT1 secret 0123456789abcdef0123456789abcdef extra","secret",n));
 assert(!remote_capture_auth("CWSHOT1 secret 0123456789abcdef0123456789abcdeg","secret",n));
 assert(!remote_capture_auth("BAD secret 0123456789abcdef0123456789abcdef","secret",n));
}
'''
    c = folder / "auth.c"
    c.write_text(code)
    exe = folder / "auth.exe"
    subprocess.run([args.cc, "-UNDEBUG", "-I"+str(root/"source"),
                    str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    for mode in ("valid", "nonce", "build", "short", "dimensions", "oversize"):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            port = listener.getsockname()[1]
            def serve():
                with listener.accept()[0] as conn:
                    line = conn.makefile("rb").readline().decode().split()
                    nonce = line[2] if mode != "nonce" else "0" * 32
                    raw = b"CWFB" + struct.pack(">II", 2 if mode=="dimensions" else 1, 1) + bytes([12,34,56,255])
                    size = 999999999 if mode=="oversize" else len(raw)
                    build = "wrong" if mode=="build" else "test"
                    conn.sendall(f"CWSHOT1 {nonce} {build} {size}\n".encode())
                    if mode not in ("nonce", "build", "oversize"):
                        assert conn.recv(1) == b"+"
                        conn.sendall(raw[:-1] if mode=="short" else raw)
                        if mode != "short":
                            assert conn.recv(1) == b"+"
            thread = threading.Thread(target=serve)
            thread.start()
            out = folder / (mode + ".png")
            try:
                receipt = client.capture("127.0.0.1", "secret", out, "test", port)
                assert mode == "valid" and receipt["width"] == 1
                from PIL import Image
                assert Image.open(out).getpixel((0,0)) == (12,34,56)
            except ValueError:
                assert mode != "valid"
                assert not out.exists()
            finally:
                thread.join()
print("PASS: authentication, fresh nonce/build, exact pixels, truncation and size rejection")
