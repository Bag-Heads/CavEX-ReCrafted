"""Request a Wii frame on a trusted isolated development LAN and save its pixels.

The reusable bearer token travels in plaintext. The echoed nonce correlates a
response; it does not prevent request replay or authenticate the server. A peer
observing the token can reuse it. Protocol hardening is outside this repair.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import time


def capture(host, token, output, expected_build=None, port=39020):
    if isinstance(token, str):
        try:
            token = token.encode("ascii")
        except UnicodeEncodeError:
            raise ValueError("Invalid capture token") from None
    if not 1 <= len(token) <= 128 or any(not 0x21 <= byte <= 0x7E for byte in token):
        raise ValueError("Invalid capture token")
    nonce = secrets.token_hex(16)
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.settimeout(40)
        sock.sendall(b"CWSHOT1 " + token + b" " + nonce.encode("ascii") + b"\n")
        with sock.makefile("rb") as stream:
            header = stream.readline(256)
            if not header.endswith(b"\n"):
                raise ValueError("Missing capture acknowledgement")
            fields = header.decode("ascii").split()
            if len(fields) != 4 or fields[:2] != ["CWSHOT1", nonce]:
                raise ValueError("Capture request identity mismatch")
            build = fields[2]
            if expected_build and build != expected_build:
                raise ValueError(f"Wrong running build: {build}")
            size = int(fields[3])
            if not 12 <= size <= 4 * 1024 * 1024:
                raise ValueError("Invalid frame size")
            sock.sendall(b"+")  # Validated, complete response header.
            pieces = []
            for offset in range(0, size, 1024):
                want = min(1024, size - offset)
                piece = stream.read(want)
                if len(piece) != want:
                    raise ValueError("Incomplete or invalid frame")
                pieces.append(piece)
                sock.sendall(b"+")  # Includes the final short payload piece.
            raw = b"".join(pieces)
            if len(raw) != size or raw[:4] != b"CWFB":
                raise ValueError("Incomplete or invalid frame")
    width, height = struct.unpack(">II", raw[4:12])
    if not (1 <= width <= 1024 and 1 <= height <= 1024) or size != 12 + width * height * 4:
        raise ValueError("Frame dimensions do not match payload")
    from PIL import Image
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive paths: do not silently replace an earlier user-approved shot.
    with output.with_suffix(".rgba").open("xb") as f:
        f.write(raw)
    with output.open("xb") as f:
        Image.frombytes("RGBA", (width, height), raw[12:]).convert("RGB").save(f, format="PNG")
    receipt = dict(build=build, nonce=nonce, width=width, height=height,
                   raw_sha256=hashlib.sha256(raw).hexdigest(), received_at=time.time())
    with output.with_suffix(".json").open("x") as f:
        json.dump(receipt, f, indent=2)
    return receipt


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("WII_HOST", "192.0.2.72"))
    parser.add_argument("--token-file", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-build")
    args = parser.parse_args()
    print(json.dumps(capture(args.host, args.token_file.read_bytes().rstrip(b"\r\n"),
                             args.output, args.expected_build)))
