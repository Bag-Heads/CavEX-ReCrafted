"""Record the raw server->client stream of a protocol-14 join against a
FRESH vanilla Beta 1.7.3 server (REL-MP-JOINSPEED fixture).

Usage: python -I record_vanilla_join.py <run_dir> <port> <slow_s> <total_s> <rate_kbps>

Starts minecraft_server.jar (pinned jar, tools/mp_fixture/server.properties
with the port replaced) in <run_dir>, waits for "Done", logs in as a Beta
client would (handshake, Packet1, echo of the server Packet13, then one
Packet13 per 50 ms tick), and records every received byte to s2c.bin.

For the first <slow_s> seconds the socket is read at <rate_kbps> KB/s with a
small receive buffer, so the server's chunk queue backs up exactly as it does
for the Wii (EntityPlayerMP.onUpdate only adds a Packet51 while fewer than 4
chunk packets are queued); after that it reads at full speed until <total_s>.
"""
import os
import socket
import struct
import subprocess
import sys
import threading
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
JAR = os.environ.get("CW_B173_SERVER_JAR", os.path.join(REPO, "..", "mcb173-retromcp", "jars", "minecraft_server.jar"))
JAVA = r"C:\Program Files\Java\jre1.8.0_491\bin\java.exe"
sys.path.insert(0, os.path.join(REPO, "tools", "kb_oracle"))
import protocol14 as p14  # noqa: E402

run_dir, port = sys.argv[1], int(sys.argv[2])
slow_s, total_s, rate = float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
os.makedirs(run_dir, exist_ok=True)
props = open(os.path.join(REPO, "tools", "mp_fixture", "server.properties")).read()
props = props.replace("server-port=25570", "server-port=%d" % port).replace("allow-flight=false", "allow-flight=true")
open(os.path.join(run_dir, "server.properties"), "w").write(props)

log = open(os.path.join(run_dir, "recorder.log"), "w", buffering=1)
t_start = time.monotonic()


def ev(msg):
    log.write("%.3f %s\n" % (time.monotonic() - t_start, msg))
    print(msg, flush=True)


out = open(os.path.join(run_dir, "server.stdout"), "wb")
srv = subprocess.Popen([JAVA, "-Xmx1024M", "-Xms1024M", "-jar", JAR, "nogui"],
                       cwd=run_dir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
ev("server pid %d" % srv.pid)
done = threading.Event()


def drain_server():
    for line in srv.stdout:
        out.write(line)
        out.flush()
        if b"Done (" in line:
            done.set()


threading.Thread(target=drain_server, daemon=True).start()
if not done.wait(300):
    ev("server never said Done")
    srv.kill()
    sys.exit(2)
ev("server Done")

sock = socket.socket()
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384)
sock.connect(("127.0.0.1", port))
sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
s2c = open(os.path.join(run_dir, "s2c.bin"), "wb")
lock = threading.Lock()
alive = True
pos = None
t0 = time.monotonic()
got = 0


class TapReader(p14.Reader):
    """p14.Reader whose recv is throttled and taps every byte to s2c.bin."""

    def take(self, count):
        global got
        data = bytearray()
        while len(data) < count:
            want = count - len(data)
            el = time.monotonic() - t0
            if el < slow_s:
                allowed = int(rate * 1024 * el) - got
                if allowed <= 0:
                    time.sleep(0.005)
                    continue
                want = min(want, allowed, 1024)
            part = self.sock.recv(want)
            if not part:
                raise EOFError("closed")
            s2c.write(part)
            got += len(part)
            data.extend(part)
        self.raw.extend(data)
        return bytes(data)


rd = TapReader(sock)


def send(raw):
    with lock:
        sock.sendall(raw)


send(b"\x02" + p14.string("WiiTuck"))
pid, raw = rd.packet()
assert pid == 2, pid
send(b"\x01" + struct.pack(">i", 14) + p14.string("WiiTuck") + struct.pack(">qb", 0, 0))
counts = {}


def reader():
    global alive, pos
    try:
        while alive:
            pid, raw = rd.packet()
            counts[pid] = counts.get(pid, 0) + 1
            if pid == 13:
                x, eye, feet, z, yaw, pitch = struct.unpack(">ddddff", raw[1:41])
                pos = (x, feet, eye, z, yaw, pitch)
                send(b"\x0d" + struct.pack(">ddddff", x, feet, eye, z, yaw, pitch) + b"\x01")
                ev("server poslook %.2f %.2f %.2f" % (x, feet, z))
            elif pid == 255:
                ev("kick %r" % raw[3:].decode("utf-16-be"))
                alive = False
    except (EOFError, OSError) as e:
        ev("reader end %r" % e)
        alive = False


def ticker():
    while alive:
        p = pos
        try:
            if p:
                x, feet, eye, z, yaw, pitch = p
                send(b"\x0d" + struct.pack(">ddddff", x, feet, eye, z, yaw, pitch) + b"\x01")
            else:
                send(b"\x0a\x01")
        except OSError:
            return
        time.sleep(0.05)


threading.Thread(target=reader, daemon=True).start()
threading.Thread(target=ticker, daemon=True).start()
while alive and time.monotonic() - t0 < total_s:
    time.sleep(0.5)
ev("recorded %d bytes; packets %r" % (got, sorted(counts.items())))
alive = False
try:
    send(b"\xff" + p14.string("Quitting"))
except OSError:
    pass
time.sleep(0.5)
sock.close()
s2c.close()
srv.stdin.write(b"stop\n")
srv.stdin.flush()
try:
    srv.wait(60)
except subprocess.TimeoutExpired:
    ev("server did not stop; killing our own pid %d" % srv.pid)
    srv.kill()
ev("server exit %r" % srv.returncode)
