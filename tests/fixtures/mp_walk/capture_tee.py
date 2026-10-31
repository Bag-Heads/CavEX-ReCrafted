"""Transparent TCP tee used to record tests/fixtures/mp_walk/stream.bin (IN-007).

Usage: python -I capture_tee.py <listen_port> <server_host> <server_port> <out_dir>

The client connects to 127.0.0.1:<listen_port>; every byte is forwarded
unchanged to <server_host>:<server_port> and back. Writes, for the first
(only) connection:
  s2c.bin    raw server->client bytes in arrival order (the inbound stream)
  c2s.bin    raw client->server bytes in arrival order
  events.log one line per recv: "<t_ms> <dir> <offset> <len>", plus open/close
"""
import os, socket, sys, threading, time

listen_port, host, port, out = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), sys.argv[4]
os.makedirs(out, exist_ok=True)
t0 = time.monotonic()
log = open(os.path.join(out, "events.log"), "w", buffering=1)
lock = threading.Lock()


def ev(line):
    with lock:
        log.write("%.1f %s\n" % ((time.monotonic() - t0) * 1000.0, line))


def pump(src, dst, name, fh):
    off = 0
    while True:
        try:
            data = src.recv(65536)
        except OSError as e:
            ev("%s recv_error %r" % (name, e))
            break
        if not data:
            ev("%s eof at %d" % (name, off))
            break
        fh.write(data)
        fh.flush()
        ev("%s %d %d" % (name, off, len(data)))
        off += len(data)
        try:
            dst.sendall(data)
        except OSError as e:
            ev("%s send_error %r" % (name, e))
            break
    for s in (src, dst):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


ls = socket.socket()
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("127.0.0.1", listen_port))
ls.listen(1)
ev("listening %d" % listen_port)
c, addr = ls.accept()
ev("accepted %r" % (addr,))
s = socket.create_connection((host, port))
ev("connected upstream")
for sk in (c, s):
    sk.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
f1 = open(os.path.join(out, "c2s.bin"), "wb")
f2 = open(os.path.join(out, "s2c.bin"), "wb")
a = threading.Thread(target=pump, args=(c, s, "c2s", f1))
b = threading.Thread(target=pump, args=(s, c, "s2c", f2))
a.start()
b.start()
a.join()
b.join()
ev("done")
