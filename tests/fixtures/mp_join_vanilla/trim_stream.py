import io, sys, struct
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools", "kb_oracle"))
import protocol14 as p14
data = open(sys.argv[1], "rb").read()
class R(p14.Reader):
    def __init__(s, b): s.b = b; s.o = 0; s.raw = bytearray()
    def take(s, n):
        if s.o + n > len(s.b): raise EOFError
        d = s.b[s.o:s.o+n]; s.o += n; s.raw.extend(d); return d
r = R(data); end = 0; n = 0; full = 0; part = 0; loads = 0; unloads = 0
while True:
    try:
        pid, raw = r.packet()
    except EOFError:
        break
    end = r.o; n += 1
    if pid == 51:
        x, y, z, sx, sy, sz = struct.unpack(">ihiBBB", raw[1:14])
        if sx == 15 and sy == 127 and sz == 15: full += 1
        else: part += 1
    if pid == 50:
        if raw[9]: loads += 1
        else: unloads += 1
open(sys.argv[2], "wb").write(data[:end])
print("packets", n, "bytes", end, "of", len(data), "full51", full, "partial51", part, "loads", loads, "unloads", unloads, "head", data[:5])
