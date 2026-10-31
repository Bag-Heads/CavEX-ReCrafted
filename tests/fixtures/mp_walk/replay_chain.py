"""Replay stream.bin through CaveWii's Packet51 body-chain rules (IN-007).

Usage: python -I replay_chain.py <stream.bin>

Models, per column, what source/network/server_remote_chunks.c does for a
column that is never promoted (REMOTE_CHUNK_RETAINED for its whole life):
  - Packet50 mode=1 opens an empty entry; mode=0 drops it (unload).
  - The first Packet51 for a column becomes its head body.
  - A later Packet51 goes through remote_chunk_append_body_locked
    (server_remote_chunks.c:285-320): queued bodies it covers are dropped
    (remote_volume_covers, :241-251); if the queue is then empty and it covers
    the head, it replaces the head; otherwise it is appended, and if 3 bodies
    are already queued (REMOTE_CHUNK_QUEUE_MAX, server_remote_internal.h:134)
    the append fails -> forced_reconnects++ (:781).
Promotion is not modelled. Promotion only ever empties a chain (the column
installs and later bodies apply directly), so the first overflow this replay
reports for a column that is never inside the admission radius is the real
one, and no column overflows earlier in the real client than it does here.
Prints the first overflow and the chain of that column.
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import parse_stream as P  # noqa: E402

QUEUE_MAX = 3


def covers(a, b):
    ax, ay, az, asx, asy, asz = a
    bx, by, bz, bsx, bsy, bsz = b
    return (ax <= bx and bx + bsx <= ax + asx and az <= bz and bz + bsz <= az + asz
            and ay <= by and by + bsy <= ay + asy)


def main(path):
    buf = open(path, "rb").read()
    cols = {}
    history = {}
    depth_max = 0
    for off, pid, _ in P.packets(buf):
        if pid == 0x32:
            r = P.R(buf, off + 1)
            cx, cz, mode = r.u("ii?")
            if mode:
                cols[(cx, cz)] = {"head": None, "queue": []}
                history[(cx, cz)] = [(off, "Packet50 load")]
            else:
                cols.pop((cx, cz), None)
            continue
        if pid != 0x33:
            continue
        r = P.R(buf, off + 1)
        x, y, z, sx, sy, sz, _n = r.u("ihiBBBi")
        vol = (x, y, z, sx + 1, sy + 1, sz + 1)
        key = (x >> 4, z >> 4)
        c = cols.setdefault(key, {"head": None, "queue": []})
        history.setdefault(key, []).append((off, "Packet51 x=%d y=%d z=%d %dx%dx%d" % vol))
        if c["head"] is None:
            c["head"] = vol
            continue
        c["queue"] = [q for q in c["queue"] if not covers(vol, q)]
        if not c["queue"] and covers(vol, c["head"]):
            c["head"] = vol
            continue
        if len(c["queue"]) >= QUEUE_MAX:
            print("first overflow at offset %d, column %r (head + %d queued)" % (off, key, len(c["queue"])))
            for o, s in history[key]:
                print("  %8d %s" % (o, s))
            return 0
        c["queue"].append(vol)
        depth_max = max(depth_max, len(c["queue"]))
    print("no overflow; deepest queue %d" % depth_max)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
