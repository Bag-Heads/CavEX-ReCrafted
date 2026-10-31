"""What each Packet51 body for one column changes (IN-007).

Usage: python -I -B body_changes.py <stream.bin> <c2s.bin> [cx cz]   (default 12 -9)

Applies the column's bodies in arrival order to a 16x128x16 column, using the
region layout of CFR Chunk.getChunkData
(/c/tmp/mcb173-server-decompile/out/net/minecraft/src/Chunk.java:474-520:
block ids, then metadata, block-light and sky-light nibbles, each x-outer,
z-middle, y-inner), and counts per body how many block ids, metadata, block
light and sky light values differ from the state before it. Also prints the
column's Chebyshev distance (the metric of remote_chunk_distance_locked,
server_remote_chunks.c:503-518) from the player's chunk over every
client position packet in c2s.bin.
"""
import math, os, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import parse_stream as P  # noqa: E402

NAMES = {0: "air", 1: "stone", 2: "grass", 3: "dirt", 4: "cobble", 8: "water",
         9: "still_water", 10: "lava", 11: "still_lava", 12: "sand", 13: "gravel",
         17: "log", 18: "leaves", 31: "tallgrass", 49: "obsidian", 78: "snow"}


def nib(a, i):
    return (a[i >> 1] >> ((i & 1) * 4)) & 15


def setnib(a, i, v):
    sh = (i & 1) * 4
    a[i >> 1] = (a[i >> 1] & ~(15 << sh) & 0xFF) | (v << sh)


def main(stream, c2s_path, cx, cz):
    buf = open(stream, "rb").read()
    blocks = bytearray(32768)
    arrays = [bytearray(16384), bytearray(16384), bytearray(16384)]
    n = 0
    for off, pid, _ in P.packets(buf):
        if pid != 0x33:
            continue
        r = P.R(buf, off + 1)
        x, y, z, sx, sy, sz, ln = r.u("ihiBBBi")
        sx, sy, sz = sx + 1, sy + 1, sz + 1
        if (x >> 4, z >> 4) != (cx, cz):
            continue
        raw = zlib.decompress(r.take(ln))
        x0, z0 = x & 15, z & 15
        bases = [(xx << 11 | zz << 7 | y) for xx in range(x0, x0 + sx) for zz in range(z0, z0 + sz)]
        o = 0
        dblock = 0
        trans = {}
        for base in bases:
            for k in range(sy):
                i = base + k
                v = raw[o]
                o += 1
                if blocks[i] != v:
                    dblock += 1
                    t = "%s->%s" % (NAMES.get(blocks[i], blocks[i]), NAMES.get(v, v))
                    trans[t] = trans.get(t, 0) + 1
                blocks[i] = v
        dn = []
        for arr in arrays:
            d = 0
            for base in bases:
                for k in range(sy):
                    i = base + k
                    v = (raw[o + (k >> 1)] >> ((k & 1) * 4)) & 15
                    if nib(arr, i) != v:
                        d += 1
                    setnib(arr, i, v)
                o += sy // 2
            dn.append(d)
        top = ", ".join("%s x%d" % kv for kv in sorted(trans.items(), key=lambda kv: -kv[1])) if n else "(head)"
        print("%8d body%d x=%d y=%d z=%d %dx%dx%d: blocks %d, meta %d, blocklight %d, skylight %d; %s"
              % (off, n, x, y, z, sx, sy, sz, dblock, dn[0], dn[1], dn[2], top))
        n += 1
    c2s = open(c2s_path, "rb").read()
    ds = []
    for off, pid, _ in P.packets(c2s, True):
        if pid in (0x0B, 0x0D):
            v = P.R(c2s, off + 1).u("dddd")
            ds.append(max(abs(cx - math.floor(v[0] / 16)), abs(cz - math.floor(v[3] / 16))))
    print("column (%d,%d) distance from player chunk over %d position packets: min %d, max %d"
          % (cx, cz, len(ds), min(ds), max(ds)))


if __name__ == "__main__":
    a = sys.argv
    main(a[1], a[2], int(a[3]) if len(a) > 3 else 12, int(a[4]) if len(a) > 4 else -9)
