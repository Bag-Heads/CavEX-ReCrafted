"""Beta 1.7.3 (protocol 14) packet splitter for the IN-007 capture.

Usage: python -I parse_stream.py <stream.bin> [s2c|c2s] [--all]
Prints one line per packet: offset id summary. Packet layouts follow the
readPacketData bodies in /c/tmp/mcb173-server-decompile/out/net/minecraft/src/Packet*.java.
Default prints only packets 0x09/0x0B/0x0D/0x32/0x33/0xFF plus a census.
"""
import struct, sys, zlib, collections


class R:
    def __init__(self, b, o):
        self.b, self.o = b, o

    def take(self, n):
        if self.o + n > len(self.b):
            raise EOFError
        v = self.b[self.o:self.o + n]
        self.o += n
        return v

    def u(self, fmt):
        n = struct.calcsize(">" + fmt)
        return struct.unpack(">" + fmt, self.take(n))

    def s16(self):
        (n,) = self.u("h")
        return self.take(2 * n).decode("utf-16-be", "replace")

    def utf(self):
        (n,) = self.u("H")
        return self.take(n).decode("utf-8", "replace")

    def item(self):
        (i,) = self.u("h")
        if i >= 0:
            self.u("bh")
        return i

    def meta(self):
        while True:
            (b,) = self.u("B")
            if b == 127:
                return
            t = b >> 5
            if t == 0: self.u("b")
            elif t == 1: self.u("h")
            elif t == 2: self.u("i")
            elif t == 3: self.u("f")
            elif t == 4: self.s16()
            elif t == 5: self.u("hbh")
            elif t == 6: self.u("iii")
            else: raise ValueError("bad metadata type %d" % t)


def body(pid, r, c2s):
    u = r.u
    if pid == 0x00: return ""
    if pid == 0x01:
        a = u("i"); n = r.s16(); s = u("qb"); return "eid/proto=%d name=%r seed=%d dim=%d" % (a[0], n, s[0], s[1])
    if pid == 0x02: return r.s16()
    if pid == 0x03: return repr(r.s16())
    if pid == 0x04: return "time=%d" % u("q")
    if pid == 0x05: u("ihhh"); return ""
    if pid == 0x06: return "spawn=%r" % (u("iii"),)
    if pid == 0x07: u("ii?"); return ""
    if pid == 0x08: return "health=%d" % u("h")
    if pid == 0x09: return "dim=%d" % u("b")
    if pid == 0x0A: u("?"); return ""
    if pid == 0x0B: v = u("dddd?"); return "x=%.2f y=%.2f stance=%.2f z=%.2f g=%d" % v
    if pid == 0x0C: u("ff?"); return ""
    if pid == 0x0D:
        v = u("ddddff?")
        return "x=%.2f a=%.2f b=%.2f z=%.2f yaw=%.1f pitch=%.1f g=%d" % v
    if pid == 0x0E: u("bibib"); return ""
    if pid == 0x0F: u("ibib"); r.item(); return ""
    if pid == 0x10: u("h"); return ""
    if pid == 0x11: u("ibibi"); return ""
    if pid == 0x12: u("ib"); return ""
    if pid == 0x13: u("ib"); return ""
    if pid == 0x14: e = u("i"); n = r.s16(); u("iiibbh"); return "eid=%d %r" % (e[0], n)
    if pid == 0x15: u("ihbhiiibbb"); return ""
    if pid == 0x16: u("ii"); return ""
    if pid == 0x17:
        v = u("ibiiii")
        if v[5] > 0: u("hhh")
        return "eid=%d type=%d" % (v[0], v[1])
    if pid == 0x18: v = u("ibiiibb"); r.meta(); return "eid=%d type=%d" % (v[0], v[1])
    if pid == 0x19: u("i"); r.s16(); u("iiii"); return ""
    if pid == 0x1B: u("ffff??"); return ""
    if pid == 0x1C: u("ihhh"); return ""
    if pid == 0x1D: return "eid=%d" % u("i")
    if pid == 0x1E: u("i"); return ""
    if pid == 0x1F: u("ibbb"); return ""
    if pid == 0x20: u("ibb"); return ""
    if pid == 0x21: u("ibbbbb"); return ""
    if pid == 0x22: u("iiiibb"); return ""
    if pid == 0x26: u("ib"); return ""
    if pid == 0x27: u("ii"); return ""
    if pid == 0x28: u("i"); r.meta(); return ""
    if pid == 0x32:
        x, z, m = u("ii?"); return "cx=%d cz=%d mode=%d" % (x, z, m)
    if pid == 0x33:
        x, y, z, sx, sy, sz, n = u("ihiBBBi")
        data = r.take(n)
        sx, sy, sz = sx + 1, sy + 1, sz + 1
        try:
            raw = len(zlib.decompress(data))
        except zlib.error:
            raw = -1
        cx0, cx1 = x >> 4, (x + sx - 1) >> 4
        cz0, cz1 = z >> 4, (z + sz - 1) >> 4
        cross = " CROSSES-COLUMN" if (cx0 != cx1 or cz0 != cz1) else ""
        return "x=%d y=%d z=%d size=%dx%dx%d clen=%d raw=%d col=(%d,%d)%s" % (
            x, y, z, sx, sy, sz, n, raw, cx0, cz0, cross)
    if pid == 0x34:
        cx, cz, n = u("iih"); r.take(n * 2 + n + n); return "cx=%d cz=%d n=%d" % (cx, cz, n)
    if pid == 0x35: v = u("ibibb"); return "%r" % (v,)
    if pid == 0x36: u("ihibb"); return ""
    if pid == 0x3C:
        u("dddf"); (n,) = u("i"); r.take(3 * n); return "n=%d" % n
    if pid == 0x3D: u("iibii"); return ""
    if pid == 0x46: u("b"); return ""
    if pid == 0x47: u("ibiii"); return ""
    if pid == 0x64: u("bb"); r.utf(); u("b"); return ""
    if pid == 0x65: u("b"); return ""
    if pid == 0x66: u("bhbh?"); r.item(); return ""
    if pid == 0x67: u("bh"); r.item(); return ""
    if pid == 0x68:
        w, n = u("bh")
        for _ in range(n): r.item()
        return "win=%d n=%d" % (w, n)
    if pid == 0x69: u("bhh"); return ""
    if pid == 0x6A: u("bh?"); return ""
    if pid == 0x82: u("ihi"); [r.s16() for _ in range(4)]; return ""
    if pid == 0x83: u("hh"); (n,) = u("B"); r.take(n); return ""
    if pid == 0xC8: u("ib"); return ""
    if pid == 0xFF: return repr(r.s16())
    raise ValueError("unknown packet id 0x%02x" % pid)


def packets(buf, c2s=False):
    r = R(buf, 0)
    while r.o < len(buf):
        start = r.o
        try:
            (pid,) = r.u("B")
            s = body(pid, r, c2s)
        except EOFError:
            yield start, None, "TRUNCATED tail (%d bytes)" % (len(buf) - start)
            return
        yield start, pid, s


if __name__ == "__main__":
    path = sys.argv[1]
    c2s = len(sys.argv) > 2 and sys.argv[2] == "c2s"
    show_all = "--all" in sys.argv
    census = collections.Counter()
    buf = open(path, "rb").read()
    for off, pid, s in packets(buf, c2s):
        census[pid] += 1
        if show_all or pid in (None, 0x09, 0x0B, 0x0D, 0x32, 0x33, 0xFF) or "CROSSES" in s:
            print("%8d %s %s" % (off, "--" if pid is None else "%02x" % pid, s))
    print("census:", ", ".join("%s=%d" % ("--" if k is None else "%02x" % k, v) for k, v in sorted(census.items(), key=lambda kv: (kv[0] is None, kv[0] or 0))))
