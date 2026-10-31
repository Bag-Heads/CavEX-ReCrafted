"""X06 host-only acceptance driver. PASS is not Wii/adoption acceptance."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True
PASS, FAIL, INVALID = 0, 1, 2
GAP_MS, TOLERANCE_MS, DEADLINE_S = 5000, 20, 50


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def unchanged(frozen):
    try:
        return all(digest(path) == sha for path, sha in frozen.items())
    except OSError:
        return False


def emit(stream, event, **fields):
    stream.write(json.dumps(dict(event=event, utc_ns=time.time_ns(),
                                 monotonic_ns=time.monotonic_ns(), **fields)) + "\n")
    stream.flush()


def load_client(path):
    spec = importlib.util.spec_from_file_location("capture_client", path)
    client = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(client)
    return client


def validate_pixels(output, receipt, width, height):
    from PIL import Image, ImageStat
    raw = output.with_suffix(".rgba").read_bytes()
    if raw[:12] != b"CWFB" + struct.pack(">II", width, height):
        raise ValueError("dimensions")
    if len(raw) != 12 + width * height * 4 or digest(output.with_suffix(".rgba")) != receipt['raw_sha256']:
        raise ValueError("size/hash")
    with Image.open(output) as image:
        image.load()
        expected = Image.frombytes("RGBA", (width, height), raw[12:]).convert("RGB")
        if image.size != (width, height) or image.convert("RGB").tobytes() != expected.tobytes():
            raise ValueError("decoded pixel mismatch")
        stats = ImageStat.Stat(image.convert("RGB"))
        if max(stats.stddev) == 0:
            raise ValueError("uniform frame")
        return dict(size=list(image.size), mean=stats.mean, stddev=stats.stddev,
                    raw_bytes=len(raw), png_sha256=digest(output))


def worker(request):
    """One process, one explicitly bound IPv4 socket, no application retry."""
    token = Path(request['token_file']).read_bytes().rstrip(b"\r\n")
    original = socket.create_connection
    attempts = 0

    def connect(address, timeout=None, source_address=None, **kwargs):
        nonlocal attempts
        attempts += 1
        if attempts != 1 or tuple(address) != (request['host'], request['port']):
            raise ValueError("unexpected connection")
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.bind(("0.0.0.0", 0))
        target = request.get("target_ns")
        if target is not None:
            time.sleep(max(0, (target - time.monotonic_ns()) / 1e9))
        emit(sys.stdout, "CONNECT", ordinal=request['ordinal'],
             local=list(sock.getsockname()), remote=list(address))
        try:
            sock.connect(address)
            emit(sys.stdout, "CONNECTED", ordinal=request['ordinal'],
                 local=list(sock.getsockname()), remote=list(sock.getpeername()))
            return sock
        except BaseException:
            sock.close()
            raise

    try:
        socket.create_connection = connect
        kind = request['kind']
        if kind == 'valid':
            client = load_client(request['client'])
            # Retained older clients accept text; current client accepts both.
            receipt = client.capture(request['host'], token.decode('ascii'),
                                     Path(request['output']), request['build'], request['port'])
            pixels = validate_pixels(Path(request['output']), receipt, request['width'], request['height'])
            emit(sys.stdout, "COMPLETE", ordinal=request['ordinal'], receipt=receipt, pixels=pixels)
        else:
            with connect((request['host'], request['port'])) as sock:
                if kind == 'invalid':
                    bad = b'X' if token != b'X' else b'Y'
                    sock.sendall(b'CWSHOT1 ' + bad + b' ' + b'0' * 32 + b'\n')
                    sock.settimeout(10)
                    # Rejection = zero bytes, then EOF or reset (the Wii closes
                    # abortively; ASTRA-X08-RULING item 1). Timeouts and any
                    # received byte still fail.
                    try:
                        data = sock.recv(1)
                        termination = 'eof'
                    except ConnectionResetError:
                        data = b''
                        termination = 'reset'
                    if data != b'':
                        raise ValueError('invalid token received bytes')
                    emit(sys.stdout, "REJECTED", ordinal=request['ordinal'], termination=termination)
                # Empty client sends nothing and closes immediately.
            emit(sys.stdout, "COMPLETE", ordinal=request['ordinal'])
        return PASS
    except Exception as error:
        # Never echo peer headers, exceptions or token material.
        emit(sys.stdout, "ERROR", ordinal=request['ordinal'], error_class=type(error).__name__)
        return FAIL
    finally:
        socket.create_connection = original


def execute(args, gap_ms=GAP_MS, tolerance_ms=TOLERANCE_MS, deadline_s=DEADLINE_S):
    """Non-default timing is used only by explicitly labelled loopback unit tests."""
    args.output.mkdir(parents=True, exist_ok=False)
    with (args.output / 'host.jsonl').open('x', encoding='utf-8') as log:
        ordinal = 0
        try:
            socket.inet_pton(socket.AF_INET, args.host)
            client = load_client(args.client)
            from PIL import Image  # dependency admission, before network
            token = args.token_file.read_bytes().rstrip(b'\r\n')
            if not 1 <= len(token) <= 128 or any(not 0x21 <= x <= 0x7e for x in token):
                raise ValueError('token domain')
            near = json.loads(args.near_zero_spec.read_text(encoding='utf-8'))
            if not near['argv'] or not 0 < near['deadline_s'] <= 300 or not near['files']:
                raise ValueError('workload manifest')
            if token.decode('ascii') in json.dumps(near['argv']):
                raise ValueError('token value in workload argv')
            for path, sha in near['files'].items():
                if digest(path) != sha:
                    raise ValueError('workload hash')
            frozen = {str(args.token_file): digest(args.token_file), str(args.client): digest(args.client),
                      str(args.near_zero_spec): digest(args.near_zero_spec),
                      **near['files']}
            if not 1 <= args.port <= 65535 or not 1 <= args.width <= 1024 or not 1 <= args.height <= 1024:
                raise ValueError('endpoint/dimensions')
            emit(log, 'ADMITTED', arm=args.arm, order=args.order, position=args.position,
                 boot_id=args.boot_id, build=args.expected_build, host=args.host, port=args.port,
                 client_sha256=digest(args.client), workload_sha256=digest(args.near_zero_spec),
                 gap_ms=gap_ms, tolerance_ms=tolerance_ms, deadline_s=deadline_s,
                 host_only=True)
        except Exception as error:
            emit(log, 'RESULT', verdict='INVALID', ordinal=0, error_class=type(error).__name__)
            return INVALID
        previous = None
        sequence = ['valid'] * 10 + ['empty', 'valid', 'invalid', 'valid']
        for ordinal, kind in enumerate(sequence, 1):
            if not unchanged(frozen):
                emit(log, 'RESULT', verdict='INVALID', ordinal=ordinal, reason='input hash changed')
                return INVALID
            target = None if previous is None else previous + int(gap_ms * 1e6)
            if target is not None:
                emit(log, 'SCHEDULE', ordinal=ordinal, target_ns=target)
            request = dict(target_ns=target, host=args.host, port=args.port, token_file=str(args.token_file),
                           client=str(args.client), ordinal=ordinal, kind=kind,
                           output=str(args.output / f'{ordinal:02d}.png'), build=args.expected_build,
                           width=args.width, height=args.height)
            emit(log, 'OPERATION', ordinal=ordinal, kind=kind, output=request['output'])
            operation_deadline = (target if target is not None else time.monotonic_ns()) + int(deadline_s * 1e9)
            try:
                result = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--worker'],
                                        input=json.dumps(request), capture_output=True, text=True,
                                        timeout=max(0, (operation_deadline - time.monotonic_ns()) / 1e9))
                rows = [json.loads(line) for line in result.stdout.splitlines()]
                for row in rows:
                    log.write(json.dumps(row) + '\n')
                log.flush()
                connects = [r for r in rows if r['event'] == 'CONNECT']
                if len(connects) != 1:
                    raise ValueError('missing/extra connect')
                if target is not None:
                    delta = (connects[0]['monotonic_ns'] - target) / 1e6
                    emit(log, 'GAP', ordinal=ordinal, deviation_ms=delta)
                    if abs(delta) > tolerance_ms:
                        emit(log, 'RESULT', verdict='INVALID', ordinal=ordinal, reason='schedule tolerance')
                        return INVALID
                complete = [r for r in rows if r['event'] == 'COMPLETE']
                if result.returncode != PASS or len(complete) != 1:
                    raise ValueError('operation failed')
                elapsed = (complete[0]['monotonic_ns'] - connects[0]['monotonic_ns']) / 1e9
                if elapsed > deadline_s:
                    raise TimeoutError('operation completion deadline')
                if args.arm == 'control' and ordinal == 2:
                    emit(log, 'RESULT', verdict='INVALID', ordinal=2, reason='control did not reproduce attempt-2 failure')
                    return INVALID
                previous = complete[0]['monotonic_ns']
            except subprocess.TimeoutExpired as error:
                # Preserve partial trace on deadline; subprocess.run kills/reaps the child.
                partial = error.stdout or b''
                if isinstance(partial, bytes):
                    partial = partial.decode('utf-8', errors='replace')
                (args.output / f'{ordinal:02d}-partial.jsonl').write_text(partial, encoding='utf-8')
                emit(log, 'RESULT', verdict='FAIL', ordinal=ordinal, reason='operation deadline')
                return FAIL
            except Exception as error:
                emit(log, 'RESULT', verdict='FAIL', ordinal=ordinal,
                     error_class=type(error).__name__, expected_control_failure=(args.arm == 'control' and ordinal == 2))
                return FAIL
        if args.arm == 'control':
            emit(log, 'RESULT', verdict='INVALID', ordinal=ordinal, reason='control did not reproduce attempt-2 failure')
            return INVALID
        # Preserve the original workload argv and files. No replacement loop or waits.
        if not unchanged(frozen):
            emit(log, 'RESULT', verdict='INVALID', reason='input hash changed')
            return INVALID
        emit(log, 'NEAR_ZERO_START', manifest_sha256=digest(args.near_zero_spec), deadline_s=near['deadline_s'])
        try:
            result = subprocess.run(near['argv'], cwd=near.get('cwd'), capture_output=True,
                                    timeout=near['deadline_s'])
            # Preserve workload output with bearer bytes redacted.
            for name, data in [('stdout', result.stdout), ('stderr', result.stderr)]:
                (args.output / f'near-zero-{name}.log').write_bytes(data.replace(token, b'[REDACTED]'))
            emit(log, 'NEAR_ZERO_END', returncode=result.returncode,
                 stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),
                 stderr_sha256=hashlib.sha256(result.stderr).hexdigest())
            if result.returncode:
                raise ValueError('original workload failed')
        except Exception as error:
            if isinstance(error, subprocess.TimeoutExpired):
                for name in ('stdout', 'stderr'):
                    data = getattr(error, name) or b''
                    (args.output / f'near-zero-{name}.log').write_bytes(data.replace(token, b'[REDACTED]'))
            emit(log, 'RESULT', verdict='FAIL', reason='near-zero failure/deadline', error_class=type(error).__name__)
            return FAIL
        if not unchanged(frozen):
            emit(log, 'RESULT', verdict='INVALID', reason='input hash changed')
            return INVALID
        emit(log, 'RESULT', verdict='PASS', scope='host only; lifecycle/SD/shutdown and matched boots pending')
        return PASS


def main():
    if sys.argv[1:] == ['--worker']:
        return worker(json.load(sys.stdin))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, help='literal IPv4; no DNS fallback')
    parser.add_argument('--port', type=int, default=39020)
    parser.add_argument('--token-file', type=Path, required=True)
    parser.add_argument('--client', type=Path, default=Path(__file__).with_name('capture_wii_live.py'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-build', required=True)
    parser.add_argument('--boot-id', required=True)
    parser.add_argument('--arm', choices=['control', 'fixed'], required=True)
    parser.add_argument('--order', choices=['control-fixed', 'fixed-control'], required=True)
    parser.add_argument('--position', type=int, choices=[1, 2], required=True)
    parser.add_argument('--near-zero-spec', type=Path, required=True)
    parser.add_argument('--width', type=int, default=640)
    parser.add_argument('--height', type=int, default=480)
    args = parser.parse_args()
    expected = args.order.split('-')[args.position - 1]
    if expected != args.arm:
        parser.error('arm does not match order/position')
    try:
        return execute(args)
    except (OSError, ValueError):
        print('INVALID: output/preflight error')
        return INVALID


if __name__ == '__main__':
    sys.exit(main())
