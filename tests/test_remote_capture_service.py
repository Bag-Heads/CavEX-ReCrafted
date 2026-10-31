"""Production Wii-branch B01 isolation/token and B02 paired loopback witnesses.

Standalone runner compiles the whole production file through the disclosed C
harness, asserts enabled, and fails on every timeout/nonzero child result.
Default execution MUST include the real Python client/production service pair.
Optional probe-* RED witnesses use an explicit boundary-reading network peer to
expose payload short sends while the baseline real client lacks header credit.
Credits originate only from socket peers, never from the C receive adapter.
All generated SDK shims, binaries and SD fixtures live under the job folder.
"""
from pathlib import Path
import hashlib
import importlib.util
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "tmp/jobs/capture-fix-1003"
GCC = Path("C:/msys64/ucrt64/bin/gcc.exe")
CASES = {
    "unarmed": b"secret",
    "nul": b"a\0remaining-secret",
    "nul-first": b"\0secret",
    "nul-last": b"secret\0",
    "nul-max128": b"a\0" + b"x" * 126,
    "empty": b"",
    "valid": b"secret",
    "max128": b"x" * 128,
    "over129": b"x" * 129,
    "crlf": b"secret\r\n",
    "max128-crlf": b"x" * 128 + b"\r\n",
    "crlf-only": b"\r\n",
    "space": b"sec ret",
    "tab": b"sec\tret",
    "leading-space": b" secret",
    "trailing-space": b"secret ",
    "interior-cr": b"sec\rret",
    "interior-lf": b"sec\nret",
    "vertical-tab": b"sec\vret",
    "form-feed": b"sec\fret",
    "open-failure": b"secret",
    "read-failure": b"secret",
}
PAIRS = ["pair", "short-header", "short-payload", "zero-header", "ambiguous-header",
         "zero-payload", "ambiguous-payload", "replay", "missing-credit", "wrong-credit",
         "disconnect", "deadline", "cancel-unclaimed", "stop", "ready-stop", "claimed-stop",
         "short-header-stop", "short-payload-stop", "short-header-deadline", "short-payload-deadline",
         "recreate-six", "recreate-mixed"]
PROBES = ["probe-short-header", "probe-short-payload", "probe-zero-payload", "probe-ambiguous-payload"]
NEGATIVE = {"zero-header", "ambiguous-header", "zero-payload", "ambiguous-payload",
            "short-header-stop", "short-payload-stop", "short-header-deadline", "short-payload-deadline"}
CLIENT_CASES = set(PAIRS[:8]) | NEGATIVE | {"recreate-six", "recreate-mixed"}
# Deterministic operation-level IOS doubles; real captures use PAIRS above.
LIFECYCLE_OPS = ["socket", "getfl", "setfl", "verifyfl", "reuseaddr", "bind", "listen",
                 "accepted-close", "listener-close"]
LIFECYCLES = [f"fail-{op}" for op in LIFECYCLE_OPS] + [f"stop-{op}" for op in LIFECYCLE_OPS] + [
    "fail-verify-bit", "fail-cleanup-close", "backoff-cap", "stop-backoff",
    "stop-cleanup-backoff", "shutdown-backoff", "shutdown",
    "permanent-socket", "permanent-bind", "permanent-listen", "permanent-reuseaddr",
    "many-recreates", "intermittent-bind"]
NONCE = "0123456789abcdef0123456789abcdef"
RAW = b"CWFB" + struct.pack(">II", 37, 37) + bytes((i * 17 + 23) % 256 for i in range(37 * 37 * 4))


def capture_worker(port, scenario):
    spec = importlib.util.spec_from_file_location("capture", ROOT / "tools/capture_wii_live.py")
    client = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(client)
    count = 6 if scenario.startswith("recreate-") else 2 if scenario == "replay" else 1

    def await_generation(generation):
        deadline = time.monotonic() + 1
        while not Path(f"generation-{generation}.ready").exists():
            assert time.monotonic() < deadline, "replacement listener never became ready"
            time.sleep(0.001)

    offset = 0
    if scenario == "recreate-mixed":
        for ordinal, request in enumerate((b"", f"CWSHOT1 wrong {NONCE}\n".encode()), 1):
            await_generation(ordinal)
            with socket.create_connection(("127.0.0.1", port), timeout=1) as rejected:
                if request:
                    rejected.sendall(request)
                else:
                    rejected.shutdown(socket.SHUT_WR)
                assert rejected.recv(1) == b"", "rejected client got a response"
            print(f"PASS: rejected {'invalid-token' if request else 'empty'} client", flush=True)
        offset = 2
    for index in range(count):
        if scenario.startswith("recreate-"):
            await_generation(index + offset + 1)
        output = Path(f"output-{index}.png")
        print(f"CLIENT real capture(): scenario={scenario} request={index+1}", flush=True)
        try:
            with patch.object(client.secrets, "token_hex", return_value=NONCE):
                receipt = client.capture("127.0.0.1", "secret", output, "B02-test", port)
        except (ValueError, OSError) as error:
            assert scenario in NEGATIVE, f"unexpected capture failure: {error}"
            assert not any(Path(".").glob("output-*"))
            print(f"PASS: {scenario} abort={type(error).__name__}, no raw/image/receipt", flush=True)
            return
        assert scenario not in NEGATIVE, "fault injection unexpectedly produced an image"
        assert output.with_suffix(".rgba").read_bytes() == RAW
        assert receipt["raw_sha256"] == hashlib.sha256(RAW).hexdigest()
        assert receipt["width"] == receipt["height"] == 37 and receipt["nonce"] == NONCE
        from PIL import Image
        with Image.open(output) as actual:
            expected = Image.frombytes("RGBA", (37, 37), RAW[12:]).convert("RGB")
            assert actual.tobytes() == expected.tobytes()
        print(f"PASS: {scenario} exact 5488 raw bytes, all 1369 pixels, nonce={NONCE}", flush=True)
    if scenario == "replay":
        print("LIMITATION PASS: repeated valid nonce accepted on two connections", flush=True)


def peer_worker(port, name):
    scenario = name.removeprefix("probe-")
    with socket.create_connection(("127.0.0.1", port), timeout=1) as sock:
        sock.settimeout(1)
        if scenario != "deadline":
            sock.sendall(f"CWSHOT1 secret {NONCE}\n".encode())
        with sock.makefile("rb") as stream:
            header = stream.readline(256)
            print(f"PEER {name}: header={header!r}", flush=True)
            if scenario in ("deadline", "cancel-unclaimed", "ready-stop", "claimed-stop"):
                assert stream.read() == b""
                print(f"PASS: {name} bounded EOF, no payload", flush=True)
                return
            assert header == f"CWSHOT1 {NONCE} B02-test {len(RAW)}\n".encode()
            if scenario == "disconnect":
                print("PASS: peer closes without credit", flush=True)
                return
            if name.startswith("probe-"):
                # A real receiver grants credit ONLY after each declared boundary.
                sock.sendall(b"+")
                if scenario in NEGATIVE:
                    prefix = stream.read()
                    assert prefix == (RAW[:7] if "ambiguous" in scenario else b"")
                    print(f"PASS: {name} aborted after {len(prefix)} payload bytes", flush=True)
                    return
                raw = b""
                while len(raw) < len(RAW):
                    piece = stream.read(min(1024, len(RAW) - len(raw)))
                    assert piece, "probe saw aborted payload"
                    assert len(piece) == min(1024, len(RAW) - len(raw)), "probe saw short payload"
                    raw += piece
                    sock.sendall(b"+")
                assert raw == RAW
                print(f"PASS: explicit peer {name} exact frame", flush=True)
            else:
                if scenario == "wrong-credit":
                    sock.sendall(b"!")
                assert stream.read() == b""
                print(f"PASS: {scenario} bounded EOF, no payload or receipt", flush=True)


def paired_case(exe, work, env, name):
    scenario = name.removeprefix("probe-")
    (work / "token.bin").write_bytes(b"secret")
    (work / "fixture.rgba").write_bytes(RAW)
    service = subprocess.Popen([str(exe), "--pair", scenario], cwd=work, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    failure = None
    try:
        ready = work / "port.txt"
        end = time.monotonic() + 2
        while not ready.exists() and service.poll() is None and time.monotonic() < end:
            time.sleep(0.01)
        assert ready.exists(), "service did not publish a listening port"
        # Closing the readiness file precedes client launch; it is outside counted SD I/O.
        end = time.monotonic() + 2
        while True:
            try:
                value = ready.read_text()
                if not value.endswith("\n"):
                    raise ValueError("service published an incomplete listening port")
                port = int(value)
                break
            except (PermissionError, ValueError):
                if time.monotonic() >= end:
                    raise
                time.sleep(0.01)
        mode = "--capture" if name in CLIENT_CASES else "--peer"
        command = [sys.executable, str(Path(__file__).resolve()), mode, str(port), name]
        print("CHILD: " + subprocess.list2cmdline(command), flush=True)
        result = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True, timeout=2)
        print(result.stdout, end="", flush=True)
        print(result.stderr, end="", flush=True)
        if result.returncode:
            failure = f"peer/client exit {result.returncode}"
    except subprocess.TimeoutExpired as error:
        print((error.stdout or b"").decode() if isinstance(error.stdout, bytes) else error.stdout or "", end="", flush=True)
        failure = "peer/client child timeout (2s)"
    except (AssertionError, ValueError) as error:
        failure = str(error)
    finally:
        try:
            stdout, stderr = service.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            service.kill()
            stdout, stderr = service.communicate(timeout=1)
            failure = "service child timeout (3s after client)"
        print(stdout, end="", flush=True)
        print(stderr, end="", flush=True)
        if service.returncode:
            failure = f"{failure or ''}; service exit {service.returncode}"
    if failure:
        print(f"FAIL: {name}: {failure}", flush=True)
        return False
    print(f"PASS WITNESS: {name}", flush=True)
    return True


def main():
    selected = sys.argv[1:] or list(CASES) + PAIRS + LIFECYCLES
    if any(name not in CASES and name not in PAIRS and name not in PROBES and name not in LIFECYCLES for name in selected):
        raise ValueError("Unknown service case")
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["PATH"] = str(GCC.parent) + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="B01-service-", dir=ARTIFACTS) as td:
        folder = Path(td)
        (folder / "ogc").mkdir()
        shim = "/* Test SDK shim: declarations/substitutes are disclosed in the harness. */\n"
        (folder / "network.h").write_text(shim)
        (folder / "ogc/lwp_watchdog.h").write_text(shim)
        exe = folder / "service.exe"
        command = [str(GCC), "-std=c11", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
                   "-DPLATFORM_WII", "-I" + str(folder),
                   str(ROOT / "tests/remote_capture_service_harness.c"), "-lws2_32", "-o", str(exe)]
        print("COMPILE: " + subprocess.list2cmdline(command), flush=True)
        source = Path(os.environ.get("REMOTE_CAPTURE_SOURCE", ROOT / "source/platform/remote_capture.c"))
        if "REMOTE_CAPTURE_SOURCE" in os.environ:
            command[1:1] = ['-DREMOTE_CAPTURE_SOURCE="' + source.as_posix() + '"',
                            "-I" + str(ROOT / "source/platform")]
        print("PRODUCTION_SHA256: " + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
        print("CLIENT_SHA256: " + hashlib.sha256((ROOT / "tools/capture_wii_live.py").read_bytes()).hexdigest(), flush=True)
        subprocess.run(command, check=True, timeout=30, env=env)
        failed = []
        lifecycle_exe = folder / "lifecycle.exe"
        if any(name in LIFECYCLES for name in selected):
            lifecycle_command = [str(GCC), "-std=c99", "-pedantic", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
                                 "-DPLATFORM_WII", "-I" + str(folder),
                                 str(ROOT / "tests/remote_capture_recreate_harness.c"),
                                 "-lws2_32", "-o", str(lifecycle_exe)]
            print("WII-BRANCH HOST C99 COMPILE: " + subprocess.list2cmdline(lifecycle_command), flush=True)
            if "REMOTE_CAPTURE_SOURCE" in os.environ:
                lifecycle_command[1:1] = ['-DREMOTE_CAPTURE_SOURCE="' + source.as_posix() + '"',
                                          "-I" + str(ROOT / "source/platform")]
            subprocess.run(lifecycle_command, check=True, timeout=30, env=env)
        for name in selected:
            work = folder / ("case-" + name)  # NUL is a reserved Windows filename.
            work.mkdir()
            if name in LIFECYCLES:
                try:
                    result = subprocess.run([str(lifecycle_exe), name], cwd=work, env=env,
                                            capture_output=True, text=True, timeout=2)
                except subprocess.TimeoutExpired:
                    failed.append(name)
                    print(f"FAIL lifecycle: {name}: child timeout (2s)", flush=True)
                    continue
                print(result.stdout, end="", flush=True)
                print(result.stderr, end="", flush=True)
                if result.returncode:
                    failed.append(name)
                    print(f"FAIL lifecycle: {name}: child exit {result.returncode}", flush=True)
                continue
            if name not in CASES:
                if not paired_case(exe, work, env, name):
                    failed.append(name)
                continue
            (work / "token.bin").write_bytes(CASES[name])
            try:
                result = subprocess.run([str(exe), name], cwd=work, env=env,
                                        capture_output=True, text=True, timeout=5)
                print(result.stdout, end="", flush=True)
                if result.stderr:
                    print(result.stderr, end="", flush=True)
                if result.returncode:
                    failed.append(name)
                    print(f"FAIL: {name}: child exit {result.returncode}", flush=True)
            except subprocess.TimeoutExpired:
                failed.append(name)
                print(f"FAIL: {name}: child timeout (5s)", flush=True)
        print(f"SUMMARY: {len(selected)-len(failed)}/{len(selected)} passed; failed={failed}", flush=True)
        return 1 if failed else 0


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--capture":
        capture_worker(int(sys.argv[2]), sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "--peer":
        peer_worker(int(sys.argv[2]), sys.argv[3])
    else:
        sys.exit(main())
