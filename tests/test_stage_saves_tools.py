#!/usr/bin/env python3
"""PC-side saves tools (push-0921/C8) against a fake stage server.

The C stage server is Wii-only and cannot be linked into the PC object
pools, so -- exactly like tests/test_fetch_wii_stage_file_sha_mismatch.py --
the PowerShell drivers are exercised against a python mock that speaks the
real framing over a real socket. Here the mock also MODELS THE CARD, so the
tests can assert on what did and did not move.

Scenarios (brief push-0921/C8 item 4):
  * dry run prints the plan and moves NOTHING
  * an empty / absent -Keep name aborts before anything is sent
  * an unsupported name is reported and refused, never touched
  * a paged inventory that changes mid-walk is REJECTED (exit 3)
  * disconnect AFTER the rename (reply lost) -> reconcile observes both ends,
    says "moved", writes the missing result, and does NOT move again
  * an interrupted (partial) manifest line is ignored and reconcile still works
  * both-present and neither-present reconcile outcomes STOP, with distinct
    exit codes (4 and 5)
  * a soft-launch server refusing MOVE with token_required
  * a server without the saves capabilities is refused (exit 6)
  * the saves-root binding refuses an ambiguous card (exit 8)
  * restore_wii_saves.ps1 puts a world back using the manifest's root
"""

from __future__ import annotations

import argparse
import os
import json
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

FULL_CAPS = [
    "put", "fetch", "manifest", "runprep", "run-experimental", "auth",
    "saves-list", "saves-hash", "saves-meta", "saves-archive",
]
OLD_CAPS = ["put", "fetch", "manifest", "runprep", "run-experimental", "auth"]

EXIT_OK = 0
EXIT_REFUSED = 2
EXIT_INVENTORY = 3
EXIT_STOP_BOTH = 4
EXIT_STOP_NEITHER = 5
EXIT_TOO_OLD = 6
EXIT_SERVER_FAIL = 7
EXIT_ROOT_AMBIGUOUS = 8
EXIT_UNKNOWN_OBSERVATION = 9
EXIT_ORPHAN_GUARD = 10

ROOT_ABS = {
    "saves": "sd:/saves",
    "saves_app": "sd:/apps/CavEX/saves",
    "archive": "sd:/cw_saves_archive",
}
_DELETE = object()


def fnv1a(text: str) -> str:
    h = 2166136261
    for b in text.encode("ascii"):
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return "%08x" % h


def make_level_dat(level_name: str) -> bytes:
    """A real gzipped Beta level.dat -- TAG_Compound "" { TAG_Compound
    "Data" { TAG_String "LevelName", TAG_Long "LastPlayed" } } -- so the
    tool's bounded gunzip + NBT walk is exercised end to end rather than
    mocked. Tag ids are written as bytes([n]) so no escape in this file
    can turn into a literal control byte."""
    import gzip
    import io
    import struct

    TAG_END, TAG_BYTE_LONG, TAG_STRING, TAG_COMPOUND = 0, 4, 8, 10

    def nstr(x: str) -> bytes:
        b = x.encode("utf-8")
        return struct.pack(">H", len(b)) + b

    body = bytes([TAG_STRING]) + nstr("LevelName") + nstr(level_name)
    body += bytes([TAG_BYTE_LONG]) + nstr("LastPlayed") + bytes(8)
    body += bytes([TAG_END])
    root = bytes([TAG_COMPOUND]) + nstr("")
    root += bytes([TAG_COMPOUND]) + nstr("Data") + body
    root += bytes([TAG_END])
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as gz:
        gz.write(root)
    return buf.getvalue()


# What libfat returns for a long name is not determinable from this repo
# (UTF-8? CP437? locale-dependent?), which is the whole reason names cross
# the wire as name_hex. The fake therefore treats a card name as BYTES and
# uses latin-1 as the str<->bytes mapping, so a test can put a name on the
# card whose bytes are NOT valid UTF-8 -- "\xe9dition" hexes to
# e9646974696f6e, which no UTF-8 decoder accepts. For every ASCII name
# latin-1 and UTF-8 agree byte for byte, so nothing else changes.
CARD_NAME_ENCODING = "latin-1"


def hex_of(name: str) -> str:
    return name.encode(CARD_NAME_ENCODING).hex()


def name_of(name_hex: str) -> str:
    return bytes.fromhex(name_hex).decode(CARD_NAME_ENCODING)


def entry(name: str, is_dir: bool = True, supported: bool = True,
          nbytes: int = 0, mtime: int = 1757000000,
          stat_ok: bool = True) -> dict:
    # T9: when the stat FAILED the server does not know the type and says
    # so -- is_dir is null on the wire, not a fabricated false. The fake
    # used to default such a row to is_dir=True, i.e. the N6 guards were
    # being tested against a shape the card cannot produce.
    return {"name": name, "is_dir": (is_dir if stat_ok else None),
            "supported": supported,
            "bytes": nbytes, "mtime": mtime, "stat_ok": stat_ok}


class Card:
    """The fake card: two saves roots plus the archive, as ordered lists."""

    def __init__(self, saves=None, saves_app=None):
        self.roots = {
            "saves": list(saves or []),
            "saves_app": list(saves_app or []),
        }
        self.archive: dict[str, list[dict]] = {}
        self.archive_root_bytes: dict[str, int] = {}

    def listing(self, root_token: str):
        if root_token in ("saves", "saves_app"):
            return self.roots[root_token]
        if root_token == "archive":
            return [entry(b, nbytes=self.archive_root_bytes.get(b, 0))
                    for b in sorted(self.archive)]
        if root_token.startswith("archive/"):
            batch = root_token[len("archive/"):]
            if batch not in self.archive:
                return None  # the directory is not there -> FAIL root_missing
            return self.archive[batch]
        return None

    def root_abs(self, root_token: str) -> str:
        if root_token.startswith("archive/"):
            return ROOT_ABS["archive"] + "/" + root_token[len("archive/"):]
        return ROOT_ABS[root_token]


class FakeStage(threading.Thread):
    def __init__(self, card: Card, caps=None, token_provisioned=True,
                 move_behavior="ok", tear_after_page=None,
                 list_fails_after=None, level_names=None,
                 paths_worlds="saves", meta_read_error=False,
                 config_fetch_fails=False, token_locked=False,
                 token_locked_in_hello=True, hello_mutations=None,
                 auth_mutations=None, meta_mutations=None, config_missing=False):
        super().__init__(daemon=True)
        self.card = card
        self.caps = list(caps if caps is not None else FULL_CAPS)
        self.token_provisioned = token_provisioned
        self.token_locked = token_locked
        self.token_locked_in_hello = token_locked_in_hello
        self.hello_mutations = dict(hello_mutations or {})
        self.auth_mutations = dict(auth_mutations or {})
        self.meta_mutations = dict(meta_mutations or {})
        self.move_behavior = move_behavior
        self.tear_after_page = tear_after_page
        # After N successful listings, every further CWSAVES1 FAILS -- the
        # "could not look" observation the tools must not read as "absent".
        self.list_fails_after = list_fails_after
        # A real card's worlds all carry a decodable level.dat, so that is
        # the default here too -- signal (c) of the root bind depends on it.
        # Passing an explicit {} is how a test says "no world decodes".
        if level_names is None:
            self.level_names = {e["name"]: "New World"
                                for root in card.roots.values() for e in root}
        else:
            self.level_names = dict(level_names)
        self.paths_worlds = paths_worlds
        self.meta_read_error = meta_read_error
        self.config_fetch_fails = config_fetch_fails
        self.config_missing = config_missing
        self.inject_root_error = False
        self.inject_list_error_batch = None
        self.saves_protocol = 1
        self.moves = 0
        self.archive_root_lists = 0
        self.connections = 0
        self.max_concurrent = 0
        self._live = 0
        self.errors: list[BaseException] = []
        self.port = None
        self._pages_served = 0
        self._ready = threading.Event()
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(4)
        self._sock.settimeout(60)
        self.port = self._sock.getsockname()[1]
        self._stop = False

    # -- wire helpers ---------------------------------------------------
    @staticmethod
    def _read_line(conn) -> str:
        data = bytearray()
        while True:
            b = conn.recv(1)
            if not b:
                raise ConnectionError("peer closed")
            if b == b"\n":
                return data.decode("ascii").rstrip("\r")
            if b != b"\r":
                data.extend(b)

    @staticmethod
    def _send(conn, text: str) -> None:
        conn.sendall(text.encode("utf-8"))

    def _send_json(self, conn, obj) -> None:
        self._send(conn, json.dumps(obj, separators=(",", ":")) + "\n")

    def _fail(self, conn, reason: str) -> None:
        self._send_json(conn, {"status": "FAIL", "reason": reason, "bytes": 0})

    # -- verbs ----------------------------------------------------------
    def _hello(self, conn):
        reply = {
            "status": "PASS", "reason": "hello", "protocol": "CWSTAGE4",
            "capabilities": self.caps, "saves_protocol": self.saves_protocol,
            "port": self.port,
            "token_provisioned": self.token_provisioned,
        }
        if self.token_locked and self.token_locked_in_hello:
            reply["token_locked"] = True
        for key, value in self.hello_mutations.items():
            if value is _DELETE:
                reply.pop(key, None)
            else:
                reply[key] = value
        self._send_json(conn, reply)

    def _saves_meta(self, conn, parts):
        root_token, name_hex = parts[1], parts[5]
        name = name_of(name_hex)
        entries = self.card.listing(root_token)
        if entries is None or not any(e["name"] == name for e in entries):
            return self._fail(conn, "src_missing")
        if name not in self.level_names:
            return self._fail(conn, "meta_missing")
        blob = make_level_dat(self.level_names[name])
        if self.meta_read_error:
            # A read that FAILED part way: the status line is already on the
            # wire and cannot be retracted, so the trailer has to carry the
            # error and the client has to honour it.
            half = blob[: max(1, len(blob) // 2)]
            self._send_json(conn, {
                "status": "PASS", "reason": "saves_meta", "root": root_token,
                "root_abs": self.card.root_abs(root_token), "name_hex": name_hex,
                "file": "level.dat", "data_hex": half.hex(),
                "bytes": len(half), "truncated": False, "read_error": True,
            })
            return
        reply = {
            "status": "PASS", "reason": "saves_meta", "root": root_token,
            "root_abs": self.card.root_abs(root_token), "name_hex": name_hex,
            "file": "level.dat", "data_hex": blob.hex(),
            "bytes": len(blob), "truncated": False, "read_error": False,
        }
        for key, value in self.meta_mutations.items():
            if value is _DELETE:
                reply.pop(key, None)
            else:
                reply[key] = value
        self._send_json(conn, reply)

    def _fetch(self, conn, parts):
        if len(parts) != 2 or parts[1] not in ("sd:/config.json", "sd:/config_wii.json"):
            return self._fail(conn, "bad_target")
        if self.config_missing and parts[1] == "sd:/config.json":
            # Real Stage fetch_file reports fopen failure without errno.
            return self._fail(conn, "open_failed")
        if self.config_fetch_fails:
            # "could not READ the config" -- a different fact from "the
            # config has no paths key", and the tool must not fold them.
            return self._fail(conn, "read_failed")
        # paths_worlds=None models a config with NO paths key at all, which
        # the GAME reads as its default "saves" (config_read_string(...,
        # "paths.worlds", "saves"), screen_select_world.c:187).
        cfg = {} if self.paths_worlds is None else {"paths": {"worlds": self.paths_worlds}}
        body = json.dumps(cfg).encode("utf-8")
        self._send_json(conn, {"status": "PASS", "reason": "fetch",
                               "bytes": len(body),
                               "sha256": __import__("hashlib").sha256(body).hexdigest()})
        conn.sendall(body)

    def _saves_list(self, conn, parts):
        if len(parts) < 4:
            return self._fail(conn, "bad_header")
        if len(parts) > 4 and parts[4] == "meta":
            if len(parts) != 6:
                return self._fail(conn, "bad_name")
            return self._saves_meta(conn, parts)
        if (self.list_fails_after is not None
                and self._pages_served >= self.list_fails_after):
            self._pages_served += 1
            return self._fail(conn, "read_failed")
        root_token, offset, limit = parts[1], int(parts[2]), int(parts[3])
        if root_token == "archive" and self.inject_root_error:
            return self._send_json(conn, {"status": "ERROR", "reason": "root_error", "errno": 5})
        if root_token == "archive":
            self.archive_root_lists += 1
        entries = self.card.listing(root_token)
        if entries is None:
            return self._fail(conn, "root_missing")
        total = len(entries)
        window_end = offset + limit
        shown = entries[offset:window_end]
        prefix_src = "".join(hex_of(e["name"]) + "\n"
                             for e in entries[:min(window_end, total)])
        count = len(shown)
        nxt = offset + count if offset + count < total else -1
        rows = []
        for e in shown:
            row = {"name_hex": hex_of(e["name"]), "is_dir": e["is_dir"],
                   "bytes": e["bytes"], "mtime": e["mtime"],
                   "supported": e["supported"], "stat_ok": e.get("stat_ok", True)}
            if all(0x20 <= c <= 0x7E and c not in (0x22, 0x5C)
                   for c in e["name"].encode(CARD_NAME_ENCODING)):
                row["name"] = e["name"]
            rows.append(row)
        self._send_json(conn, {
            "status": "PASS", "reason": "saves_list", "root": root_token,
            "root_abs": self.card.root_abs(root_token), "offset": offset,
            "limit": limit, "mode": parts[4] if len(parts) > 4 else "plain",
            "entries": rows, "count": count, "next": nxt, "total": total,
            "unsupported": sum(0 if e["supported"] else 1 for e in shown),
            "stat_failed": sum(0 if e.get("stat_ok", True) else 1 for e in shown),
            "prefix_fnv1a": fnv1a(prefix_src),
            # Over EVERY entry of this walk, so a delete paired with an
            # insert past the page boundary cannot hide behind an unchanged
            # total and a matching prefix (M2).
            "all_fnv1a": fnv1a("".join(hex_of(e["name"]) + chr(10) for e in entries)),
            "trailer_status": ("list_error" if root_token ==
                               "archive/" + str(self.inject_list_error_batch) else "PASS"),
            "errno": (5 if root_token == "archive/" +
                      str(self.inject_list_error_batch) else 0),
        })
        self._pages_served += 1
        if (self.tear_after_page is not None
                and self._pages_served == self.tear_after_page + 1):
            # Another session creates a world between two pages. The client
            # must reject the WHOLE inventory rather than execute on it.
            self.card.roots["saves"].append(entry("zzz torn world"))

    def _saves_move(self, conn, parts):
        if len(parts) != 5:
            return self._fail(conn, "bad_header")
        direction, root_token, batch, name_hex = parts[1:5]
        if self.move_behavior.startswith("fail:"):
            return self._fail(conn, self.move_behavior[len("fail:"):])
        if self.move_behavior == "drop_reply_no_move":
            # The request never took effect AND the reply was lost: the
            # not_moved arm of the reconcile truth table.
            raise ConnectionError("reply dropped, nothing moved")
        name = name_of(name_hex)
        src_list = (self.card.roots[root_token] if direction == "archive"
                    else self.card.archive.get(batch, []))
        hit = [e for e in src_list if e["name"].lower() == name.lower()]
        if not hit:
            return self._fail(conn, "src_missing")
        if self.move_behavior == "ok_no_move":
            # The reply ARRIVES and says PASS, but nothing moved. Only the
            # VERIFY path can catch this -- every reply-contradicting
            # behaviour above also drops the connection, so the reconcile
            # path handles those instead. That is precisely why the verify
            # path's halt behaviour had no test coverage at all.
            return self._send_json(conn, {
                "status": "PASS", "reason": "archived", "direction": direction,
                "root": root_token, "root_abs": self.card.root_abs(root_token),
                "batch": batch, "name_hex": name_hex,
                "from_abs": "x", "to_abs": "y"})
        # The real server refuses rather than overwriting, case-insensitively
        # -- e.g. restoring a world whose name a later "+new" run re-created.
        dst_list = (self.card.archive.setdefault(batch, [])
                    if direction == "archive" else self.card.roots[root_token])
        if any(e["name"].lower() == name.lower() for e in dst_list):
            return self._fail(conn, "dest_exists")
        src_list.remove(hit[0])
        if direction == "archive":
            self.card.archive.setdefault(batch, []).append(hit[0])
        else:
            self.card.roots[root_token].append(hit[0])
        self.moves += 1
        if self.move_behavior == "ghost_both":
            # A torn directory entry: the world is visible at BOTH ends.
            src_list.append(hit[0])
        elif self.move_behavior == "ghost_none":
            # Missing data: it is at NEITHER end.
            if direction == "archive":
                self.card.archive.setdefault(batch, []).remove(hit[0])
            else:
                self.card.roots[root_token].remove(hit[0])
        if self.move_behavior in ("drop_reply", "ghost_both", "ghost_none"):
            # The rename happened and the REPLY IS LOST -- the exact case the
            # write-ahead manifest exists for.
            raise ConnectionError("reply deliberately dropped after the move")
        # ...and the three where the reply ARRIVES and then the card
        # contradicts it. These reach the VERIFY path, not the reconcile
        # path, which is the arm nothing in this suite used to exercise.
        if self.move_behavior == "ghost_both_ok":
            src_list.append(hit[0])          # a torn directory entry
        elif self.move_behavior == "ghost_none_ok":
            if direction == "archive":
                self.card.archive.setdefault(batch, []).remove(hit[0])
            else:
                self.card.roots[root_token].remove(hit[0])
        elif self.move_behavior == "ok_then_blind":
            # One listing fails at the wrong moment -- the least exotic
            # failure in this design, and the one whose whole point is that
            # "absent" must never be guessed.
            self.list_fails_after = self._pages_served
        self._send_json(conn, {
            "status": "PASS",
            "reason": "archived" if direction == "archive" else "restored",
            "direction": direction, "root": root_token,
            "root_abs": self.card.root_abs(root_token), "batch": batch,
            "name_hex": name_hex,
            "from_abs": "x", "to_abs": "y",
        })

    def run(self):
        try:
            while not self._stop:
                try:
                    conn, _ = self._sock.accept()
                except socket.timeout:
                    return
                self.connections += 1
                self._live += 1
                self.max_concurrent = max(self.max_concurrent, self._live)
                with conn:
                    conn.settimeout(60)
                    try:
                        while True:
                            line = self._read_line(conn)
                            parts = line.split()
                            if not parts:
                                self._fail(conn, "bad_header")
                            elif parts[0] == "CWHELLO1":
                                self._hello(conn)
                            elif parts[0] == "CWAUTH1":
                                reply = {
                                    "status": "FAIL" if self.token_locked else "PASS",
                                    "reason": ("token_locked" if self.token_locked else "auth_ok" if self.token_provisioned
                                               else "no_token_provisioned"),
                                    "bytes": 0}
                                for key, value in self.auth_mutations.items():
                                    if value is _DELETE:
                                        reply.pop(key, None)
                                    else:
                                        reply[key] = value
                                self._send_json(conn, reply)
                            elif parts[0] == "CWFETCH1":
                                self._fetch(conn, parts)
                            elif parts[0] == "CWSAVES1":
                                self._saves_list(conn, parts)
                            elif parts[0] == "CWARCHIVE1":
                                self._saves_move(conn, parts)
                            else:
                                self._fail(conn, "bad_header")
                    except (ConnectionError, OSError):
                        pass
                    finally:
                        self._live -= 1
        except BaseException as exc:  # noqa: BLE001 - surfaced via errors[]
            self.errors.append(exc)

    def stop(self):
        self._stop = True
        try:
            self._sock.close()
        except OSError:
            pass


def run_ps(powershell: str, repo: Path, script: str, args: list[str],
           timeout: int = 180, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    # Older scenarios used the production -SkipRootBinding escape hatch to
    # avoid repeating first-bind setup.  That switch is intentionally
    # refused now.  Give those fixtures a real durable binding instead, so
    # the same scenarios exercise the normal identity-reverification path.
    args = list(args)
    if (script == "archive_wii_saves.ps1" and "-SkipRootBinding" in args
            and not (env or {}).get("CW_TEST_NO_FIXTURE_BINDING")):
        args.remove("-SkipRootBinding")
        if "-ManifestDir" in args:
            mdir = Path(args[args.index("-ManifestDir") + 1])
            binding = mdir / "root_binding.json"
            if not binding.exists() and (mdir.exists() or "-Execute" in args):
                root = (args[args.index("-Root") + 1]
                        if "-Root" in args else "saves")
                keeps = []
                if "-Keep" in args:
                    i = args.index("-Keep") + 1
                    while i < len(args) and not args[i].startswith("-"):
                        keeps.append(args[i])
                        i += 1
                if not keeps:
                    keeps = ["Keeper"]
                mdir.mkdir(parents=True, exist_ok=True)
                binding.write_text(json.dumps({
                    "v": 1, "root": root,
                    "root_abs": ROOT_ABS[root],
                    "bound_utc": "2026-09-21T12:00:00.0000000Z",
                    "keep_hex": [hex_of(k) for k in keeps],
                }), encoding="utf-8")
    if (script == "restore_wii_saves.ps1" and "-FromArchiveBatch" in args
            and not (env or {}).get("CW_TEST_NO_FIXTURE_BINDING")):
        if "-Root" not in args:
            args += ["-Root", "saves"]
        if "-ManifestDir" in args:
            mdir = Path(args[args.index("-ManifestDir") + 1])
            binding = mdir / "root_binding.json"
            if not binding.exists():
                mdir.mkdir(parents=True, exist_ok=True)
                binding.write_text(json.dumps({
                    "v": 1, "root": "saves", "root_abs": ROOT_ABS["saves"],
                    "bound_utc": "2026-09-21T12:00:00.0000000Z",
                    "keep_hex": [hex_of("Keeper")],
                }), encoding="utf-8")
    cmd = [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
           str(repo / "tools" / script)] + args
    return subprocess.run(cmd, cwd=repo, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout,
                          env={**os.environ, **(env or {})})


def expect(cond: bool, message: str, proc=None) -> None:
    if not cond:
        detail = ("\n--- tool output ---\n" + proc.stdout) if proc else ""
        raise AssertionError(message + detail)


def default_card() -> Card:
    c = Card(saves=[entry("New World 1", mtime=1757000001),
                    entry("New World 2", mtime=1757000002),
                    entry("New World 3", mtime=1757000003),
                    entry("CaveWii Test 1", mtime=1757000004),
                    entry("bad.", supported=False, mtime=1757000005)])
    return c


def _c20_guard_case(ps, repo, tmp, case):
    batch = "20260921-000200"
    mdir = tmp / ("c20_" + case)
    mdir.mkdir()
    identity = "2026-09-21T12:00:00+00:00"
    (mdir / "root_binding.json").write_text(json.dumps({
        "v": 1, "root": "saves", "root_abs": "sd:/saves",
        "bound_utc": identity, "keep_hex": [hex_of("Keeper")]}), encoding="utf-8")
    card = Card(saves=[entry("Keeper"), entry("Target")])
    card.archive[batch] = [entry("Archived")]
    recorded = "Archived"
    if case == "unrecorded_world":
        card.archive[batch].append(entry("Extra"))
    if case == "missing_world":
        recorded = "Gone"
    manifest_identity = "2026-09-20T12:00:00+00:00" if case == "different_card" else identity
    records = [
        {"v": 1, "op": "batch_begin", "batch": batch, "root": "saves",
         "root_abs": "sd:/saves", "card_identity": manifest_identity},
        {"v": 1, "op": "intent", "seq": 1, "dir": "archive", "root": "saves",
         "batch": batch, "name_hex": hex_of(recorded)},
        {"v": 1, "op": "result", "seq": 1, "status": "PASS", "reason": "moved"},
    ]
    (mdir / ("manifest_" + batch + ".jsonl")).write_text(
        "".join(json.dumps(r) + "\n" for r in records), encoding="utf-8")
    srv = FakeStage(card)
    srv.start()
    try:
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
            "-ManifestDir", str(mdir), "-Batch", "20260921-000201",
            "-Name", "Target", "-Keep", "Keeper",
            "-LockPath", str(tmp / ("lock_c20_" + case))])
    finally:
        srv.stop()
    return p, srv, batch, identity, manifest_identity


def test_guard_refuses_unrecorded_batch_world(ps, repo, tmp):
    p, srv, batch, _, _ = _c20_guard_case(ps, repo, tmp, "unrecorded_world")
    assert p.returncode == EXIT_ORPHAN_GUARD and srv.moves == 0 and batch in p.stdout and hex_of("Extra") in p.stdout and "manifest" in p.stdout, p.stdout


def test_guard_refuses_missing_unrestored_world(ps, repo, tmp):
    p, srv, batch, _, _ = _c20_guard_case(ps, repo, tmp, "missing_world")
    assert p.returncode == EXIT_ORPHAN_GUARD and srv.moves == 0 and batch in p.stdout and hex_of("Gone") in p.stdout and "directory" in p.stdout, p.stdout


def test_guard_refuses_different_card_identity(ps, repo, tmp):
    p, srv, batch, identity, other = _c20_guard_case(ps, repo, tmp, "different_card")
    assert p.returncode == EXIT_ORPHAN_GUARD and srv.moves == 0 and batch in p.stdout and identity.replace("Z", ".0000000Z") in p.stdout and other.replace("Z", ".0000000Z") in p.stdout, p.stdout


def test_guard_reconciled_success_line(ps, repo, tmp):
    p, srv, _, _, _ = _c20_guard_case(ps, repo, tmp, "happy")
    assert p.returncode == EXIT_OK and srv.moves == 0, p.stdout
    assert "[ORPHAN GUARD] batch names, contents, card identity, and intents reconciled" in p.stdout, p.stdout


def test_guard_halts_on_token_locked(ps, repo, tmp):
    for hello_field in (True, False):
        mdir = tmp / ("c29_locked_hello" if hello_field else "c29_locked_auth")
        srv = FakeStage(Card(saves=[entry("Keeper"), entry("Target")]),
                        token_locked=True, token_locked_in_hello=hello_field)
        srv.start()
        try:
            p = run_ps(ps, repo, "archive_wii_saves.ps1", [
                "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
                "-SkipRootBinding", "-ManifestDir", str(mdir),
                "-Batch", "20260921-000229", "-Name", "Target", "-Keep", "Keeper",
                "-LockPath", str(tmp / ("lock_c29_" + str(hello_field)))])
        finally:
            srv.stop()
        assert p.returncode != EXIT_OK, p.stdout
        assert "token_locked" in p.stdout and "stage_token.txt" in p.stdout and "reboot" in p.stdout, p.stdout
        assert "MOVING" not in p.stdout and "DRY RUN" not in p.stdout and srv.moves == 0, p.stdout
        assert not any(mdir.glob("manifest_*.jsonl")), p.stdout


def test_guard_halts_on_unreadable_batch(ps, repo, tmp):
    batch = "20260921-000210"
    mdir = tmp / "c22_unreadable"
    mdir.mkdir()
    (mdir / ("manifest_" + batch + ".jsonl")).write_text(json.dumps({
        "v": 1, "op": "batch_begin", "batch": batch, "root": "saves",
        "card_identity": "2026-09-21T12:00:00.0000000Z"}) + "\n", encoding="utf-8")
    card = Card(saves=[entry("Keeper"), entry("Target")])
    card.archive[batch] = []
    card.archive_root_bytes[batch] = 4096  # root stat contradicts a zero-entry batch PASS
    srv = FakeStage(card)
    srv.start()
    try:
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-SkipRootBinding", "-ManifestDir", str(mdir),
            "-Batch", "20260921-000211", "-Name", "Target", "-Keep", "Keeper",
            "-LockPath", str(tmp / "lock_c22_unreadable")])
    finally:
        srv.stop()
    assert p.returncode == EXIT_ORPHAN_GUARD and batch in p.stdout, p.stdout


def test_guard_halts_on_root_error(ps, repo, tmp):
    mdir = tmp / "c24_root_error"
    mdir.mkdir()
    srv = FakeStage(Card(saves=[entry("Keeper"), entry("Target")]))
    srv.inject_root_error = True
    srv.start()
    try:
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-SkipRootBinding", "-ManifestDir", str(mdir),
            "-Batch", "20260921-000241", "-Name", "Target", "-Keep", "Keeper",
            "-LockPath", str(tmp / "lock_c24_root")])
    finally:
        srv.stop()
    assert p.returncode == EXIT_ORPHAN_GUARD and srv.moves == 0, p.stdout
    assert "unreadable, not missing" in p.stdout and "errno" in p.stdout, p.stdout
    print("PASS test_guard_halts_on_root_error")


def test_guard_halts_on_list_error(ps, repo, tmp):
    batch = "20260921-000242"
    mdir = tmp / "c24_list_error"
    mdir.mkdir()
    (mdir / ("manifest_" + batch + ".jsonl")).write_text(json.dumps({
        "v": 1, "op": "batch_begin", "batch": batch, "root": "saves",
        "card_identity": "2026-09-21T12:00:00.0000000Z"}) + "\n", encoding="utf-8")
    card = Card(saves=[entry("Keeper"), entry("Target")])
    card.archive[batch] = [entry("Archived")]
    srv = FakeStage(card)
    srv.inject_list_error_batch = batch
    srv.start()
    try:
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-SkipRootBinding", "-ManifestDir", str(mdir),
            "-Batch", "20260921-000243", "-Name", "Target", "-Keep", "Keeper",
            "-LockPath", str(tmp / "lock_c24_list")])
    finally:
        srv.stop()
    assert p.returncode == EXIT_ORPHAN_GUARD and srv.moves == 0, p.stdout
    assert batch in p.stdout and "list_error" in p.stdout and "count" in p.stdout, p.stdout
    print("PASS test_guard_halts_on_list_error")


def test_guard_legacy_manifest_recovery(ps, repo, tmp):
    batch = "20260921-000212"
    mdir = tmp / "c22_legacy"
    mdir.mkdir()
    manifest = mdir / ("manifest_" + batch + ".jsonl")
    manifest.write_text(json.dumps({"v": 1, "op": "batch_begin", "batch": batch,
                                    "root": "saves"}) + "\n", encoding="utf-8")
    card = Card(saves=[entry("Keeper"), entry("Target")])
    card.archive[batch] = []
    srv = FakeStage(card)
    srv.start()
    base = ["-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-SkipRootBinding", "-ManifestDir", str(mdir), "-Name", "Target",
            "-Keep", "Keeper", "-LockPath", str(tmp / "lock_c22_legacy")]
    try:
        refused = run_ps(ps, repo, "archive_wii_saves.ps1", base + ["-Batch", "20260921-000213"])
        adopted = run_ps(ps, repo, "archive_wii_saves.ps1", base + [
            "-Batch", "20260921-000213", "-AdoptLegacyManifest", batch, "-Execute"])
        passed = run_ps(ps, repo, "archive_wii_saves.ps1", base + ["-Batch", "20260921-000213"])
    finally:
        srv.stop()
    assert refused.returncode == EXIT_ORPHAN_GUARD and "-AdoptLegacyManifest" in refused.stdout, refused.stdout
    assert adopted.returncode == EXIT_OK and passed.returncode == EXIT_OK, adopted.stdout + passed.stdout
    assert read_jsonl(manifest)[-1]["card_identity"] == "2026-09-21T12:00:00.0000000Z"


def test_guard_defers_open_intent_to_reconcile(ps, repo, tmp):
    batch = "20260921-000214"
    mdir = tmp / "c22_open"
    mdir.mkdir()
    (mdir / ("manifest_" + batch + ".jsonl")).write_text("".join(json.dumps(r) + "\n" for r in [
        {"v": 1, "op": "batch_begin", "batch": batch, "root": "saves", "card_identity": "2026-09-21T12:00:00.0000000Z"},
        {"v": 1, "op": "intent", "seq": 1, "dir": "archive", "root": "saves",
         "batch": batch, "name_hex": hex_of("Target"), "from": "sd:/saves/Target",
         "to": "sd:/cw_saves_archive/" + batch + "/Target"}]) , encoding="utf-8")
    card = Card(saves=[entry("Keeper"), entry("Target")])
    card.archive[batch] = [entry("Target")]
    srv = FakeStage(card)
    srv.start()
    try:
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port), "-SkipRootBinding",
            "-ManifestDir", str(mdir), "-Batch", "20260921-000215",
            "-Name", "Keeper2", "-Keep", "Keeper",
            "-LockPath", str(tmp / "lock_c22_open")])
    finally:
        srv.stop()
    assert p.returncode == EXIT_STOP_BOTH and "RECONCILE STOP" in p.stdout and batch in p.stdout, p.stdout


def test_c32_protocol_and_recovery_fail_closed(ps, repo, tmp):
    card = Card(saves=[entry("Keeper"), entry("Target"), entry("Third")])
    p = run_ps(ps, repo, "archive_wii_saves.ps1", [
        "-WiiHost", "127.0.0.1", "-Port", "1", "-SkipRootBinding",
        "-ManifestDir", str(tmp / "c32_skip"), "-Name", "Target",
        "-Keep", "Keeper"], env={"CW_TEST_NO_FIXTURE_BINDING": "1"})
    assert p.returncode == EXIT_ROOT_AMBIGUOUS and "no longer a mutation escape hatch" in p.stdout, p.stdout
    base_list = ["-WiiHost", "127.0.0.1", "-ConnectTimeoutSeconds", "10",
                 "-OutDir", str(tmp / "c32_list"), "-LockPath", str(tmp / "c32_lock")]
    for label, kwargs, needle in (
            ("hello_protocol", {"hello_mutations": {"protocol": _DELETE}}, "protocol"),
            ("hello_caps", {"hello_mutations": {"capabilities": "saves-list"}}, "capabilities"),
            ("hello_token_type", {"hello_mutations": {"token_provisioned": "yes"}}, "boolean"),
            ("auth_reason", {"auth_mutations": {"reason": "unexpected"}}, "auth reply reason")):
        srv = FakeStage(card, **kwargs)
        srv.start()
        try:
            p = run_ps(ps, repo, "list_wii_saves.ps1",
                       base_list + ["-Port", str(srv.port)])
        finally:
            srv.stop()
        assert p.returncode != EXIT_OK and needle in p.stdout and srv.moves == 0, label + "\n" + p.stdout

    for label, mutation, needle in (
            ("meta_missing_flag", {"read_error": _DELETE}, "read_error"),
            ("meta_length", {"bytes": 999}, "data_hex carries"),
            ("meta_status", {"status": "MAYBE"}, "unexpected status")):
        mdir = tmp / ("c32_" + label)
        srv = FakeStage(card, meta_mutations=mutation)
        srv.start()
        try:
            p = run_ps(ps, repo, "archive_wii_saves.ps1", [
                "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
                "-ConnectTimeoutSeconds", "10", "-ManifestDir", str(mdir),
                "-Name", "Target", "-Keep", "Keeper", "-ExpectWorldCountAtLeast", "2",
                "-LockPath", str(tmp / ("lock_" + label))])
        finally:
            srv.stop()
        assert p.returncode != EXIT_OK and needle in p.stdout and srv.moves == 0, label + "\n" + p.stdout

    batch = "20260921-009999"
    mdir = tmp / "c32_recovery"
    mdir.mkdir()
    (mdir / "root_binding.json").write_text(json.dumps({
        "v": 1, "root": "saves", "root_abs": "sd:/saves",
        "bound_utc": "2026-09-21T12:00:00.0000000Z",
        "keep_hex": [hex_of("Keeper")]}), encoding="utf-8")
    recovery_env = {"CW_TEST_NO_FIXTURE_BINDING": "1"}
    archive_card = Card(saves=[entry("Keeper")])
    archive_card.archive[batch] = [entry("Target")]
    srv = FakeStage(archive_card)
    srv.start()
    try:
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-FromArchiveBatch", batch, "-ManifestDir", str(mdir),
            "-LockPath", str(tmp / "lock_missing_root")], env=recovery_env)
    finally:
        srv.stop()
    assert p.returncode == EXIT_REFUSED and "explicit -Root" in p.stdout and srv.moves == 0, p.stdout

    missing_binding_dir = tmp / "c32_missing_binding"
    missing_binding_dir.mkdir()
    old_batch = "20260921-009998"
    (missing_binding_dir / ("manifest_" + old_batch + ".jsonl")).write_text(
        json.dumps({"v": 1, "op": "batch_begin", "batch": old_batch,
                    "root": "saves", "card_identity": "unbound:saves"}) + "\n",
        encoding="utf-8")
    old_card = Card(saves=[entry("Keeper"), entry("Target")])
    old_card.archive[old_batch] = []
    srv = FakeStage(old_card)
    srv.start()
    try:
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ManifestDir", str(missing_binding_dir), "-Name", "Target",
            "-Keep", "Keeper", "-LockPath", str(tmp / "lock_missing_binding")])
    finally:
        srv.stop()
    assert p.returncode == EXIT_ROOT_AMBIGUOUS and "root_binding.json is missing" in p.stdout and srv.moves == 0, p.stdout

    no_keeper_card = Card(saves=[entry("Someone Else")])
    no_keeper_card.archive[batch] = [entry("Target")]
    srv = FakeStage(no_keeper_card)
    srv.start()
    try:
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port), "-Root", "saves",
            "-FromArchiveBatch", batch, "-ManifestDir", str(mdir),
            "-LockPath", str(tmp / "lock_missing_keeper")], env=recovery_env)
    finally:
        srv.stop()
    assert p.returncode == EXIT_ROOT_AMBIGUOUS and "missing bound keeper" in p.stdout and srv.moves == 0, p.stdout

    manifest = mdir / ("manifest_" + batch + ".jsonl")
    manifest.write_text(json.dumps({"v": 1, "op": "batch_begin", "batch": batch}) + "\n", encoding="utf-8")
    srv = FakeStage(archive_card)
    srv.start()
    try:
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port), "-Root", "saves",
            "-FromArchiveBatch", batch, "-ManifestDir", str(mdir),
            "-LockPath", str(tmp / "lock_nonempty")], env=recovery_env)
    finally:
        srv.stop()
    assert p.returncode == EXIT_REFUSED and "non-empty manifest" in p.stdout and srv.moves == 0, p.stdout

    manifest.unlink()
    archive_card.archive[batch] = []
    srv = FakeStage(archive_card)
    srv.start()
    try:
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port), "-Root", "saves",
            "-FromArchiveBatch", batch, "-ManifestDir", str(mdir),
            "-LockPath", str(tmp / "lock_empty")], env=recovery_env)
    finally:
        srv.stop()
    assert p.returncode == EXIT_REFUSED and "empty archive batch" in p.stdout and srv.moves == 0, p.stdout

    archive_card.archive[batch] = [entry("Target"), entry("bad.", supported=False)]
    srv = FakeStage(archive_card)
    srv.start()
    try:
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port), "-Root", "saves",
            "-FromArchiveBatch", batch, "-ManifestDir", str(mdir),
            "-LockPath", str(tmp / "lock_partial")], env=recovery_env)
    finally:
        srv.stop()
    assert p.returncode == EXIT_REFUSED and "refusing a partial reconstruction" in p.stdout and srv.moves == 0, p.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--powershell", required=True)
    args = ap.parse_args()
    repo = Path(args.repo).resolve()
    ps = args.powershell

    subprocess.run(
        [ps, "-NoProfile", "-File", str(repo / "tests/test_stage_line_reader.ps1")],
        check=True, timeout=30,
    )

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)

        c20_failures = []
        for check in (test_guard_refuses_unrecorded_batch_world,
                      test_c32_protocol_and_recovery_fail_closed,
                      test_guard_halts_on_token_locked,
                      test_guard_halts_on_root_error,
                      test_guard_halts_on_list_error,
                      test_guard_refuses_missing_unrestored_world,
                      test_guard_refuses_different_card_identity,
                      test_guard_reconciled_success_line,
                      test_guard_halts_on_unreadable_batch,
                      test_guard_legacy_manifest_recovery,
                      test_guard_defers_open_intent_to_reconcile):
            try:
                check(ps, repo, tmp)
                print("PASS " + check.__name__)
            except AssertionError as exc:
                print("RED " + check.__name__ + ": " + str(exc).splitlines()[-1])
                c20_failures.append(check.__name__)
        if c20_failures:
            return 1

        def fresh(**kw):
            srv = FakeStage(**kw)
            srv.start()
            return srv

        def lock_args(name: str) -> list[str]:
            # NEVER the real C:/tmp/cw-rig-lock -- each case gets its own
            # temp lock path, so a test can neither block nor be blocked by a
            # live rig session.
            return ["-LockPath", str(tmp / ("lock_" + name))]

        # C18: a card batch with no manifest refuses even a dry-run plan.
        card18 = Card(saves=[entry("Keeper"), entry("W1"), entry("W2")])
        card18.archive["20260921-000018"] = [entry("Lost")]
        mdir18 = tmp / "man18"
        def arc18(srv, world, batch, execute=False, env=None):
            return run_ps(ps, repo, "archive_wii_saves.ps1", [
                "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
                "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
                "-ManifestDir", str(mdir18), "-Batch", batch,
                "-Name", world, "-Keep", "Keeper"]
                + (["-Execute"] if execute else []) + lock_args("c18" + batch),
                env=env)

        srv = fresh(card=card18)
        p = arc18(srv, "W1", "20260921-000181")
        srv.stop()
        expect(p.returncode == EXIT_ORPHAN_GUARD,
               "ORPHAN GUARD must refuse an unmanifested card batch", p)
        expect("20260921-000018" in p.stdout and "-FromArchiveBatch" in p.stdout,
               "name the batch and its recovery", p)
        expect(srv.moves == 0 and srv.archive_root_lists == 1,
               "one archive-root LIST and zero MOVEs", p)
        expect(not mdir18.exists(), "guarded dry run writes nothing", p)

        srv = fresh(card=card18)
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-FromArchiveBatch", "20260921-000018",
            "-ManifestDir", str(mdir18), "-Execute"] + lock_args("c18restore"))
        srv.stop()
        expect(p.returncode == EXIT_OK, "rebuild and restore orphan", p)
        srv = fresh(card=card18)
        p = arc18(srv, "W1", "20260921-000181")
        srv.stop()
        expect(p.returncode == EXIT_OK and srv.archive_root_lists == 1,
               "restored orphan passes guard with one root LIST", p)
        del card18.archive["20260921-000018"]
        srv = fresh(card=card18)
        p = arc18(srv, "W1", "20260921-000181")
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "a fully restored manifest needs no archive directory", p)

        missing18 = mdir18 / "manifest_20260921-000180.jsonl"
        missing18.write_text("".join(json.dumps(r) + "\n" for r in [
            {"v": 1, "op": "intent", "seq": 1, "dir": "archive",
             "root": "saves", "batch": "20260921-000180",
             "name_hex": hex_of("Missing"), "from": "sd:/saves/Missing",
             "to": "sd:/cw_saves_archive/20260921-000180/Missing"},
            {"v": 1, "op": "result", "seq": 1, "status": "PASS",
             "reason": "moved"}]), encoding="utf-8")
        srv = fresh(card=card18)
        p = arc18(srv, "W1", "20260921-000181", True)
        srv.stop()
        expect(p.returncode == EXIT_ORPHAN_GUARD and srv.moves == 0 and
               "20260921-000180" in p.stdout and "name_hex" in p.stdout,
               "a MOVE result without its batch directory is refused", p)
        missing18.unlink()

        # The injection belongs to the PC tool: the fake server replies normally.
        srv = fresh(card=card18)
        p = arc18(srv, "W1", "20260921-000182", True,
                  {"CW_SAVES_INJECT_LOST_REPLY": hex_of("W1") + "|1"})
        srv.stop()
        recs = read_jsonl(mdir18 / "manifest_20260921-000182.jsonl")
        expect(p.returncode != EXIT_OK and srv.moves == 1 and
               any(r["op"] == "intent" for r in recs) and
               not any(r["op"] == "result" for r in recs),
               "injected archive reply loss leaves a moved world and open intent", p)
        expect("CW_SAVES_INJECT_LOST_REPLY" in p.stdout,
               "injection is visible in provenance", p)
        srv = fresh(card=card18)
        p = arc18(srv, "W2", "20260921-000183", True)
        srv.stop()
        expect(p.returncode == EXIT_OK and srv.moves == 1 and
               "reconciled_moved" in str(read_jsonl(mdir18 / "manifest_20260921-000182.jsonl")),
               "next leg reconciles without a second W1 MOVE", p)

        srv = fresh(card=card18)
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-Manifest",
            str(mdir18 / "manifest_20260921-000182.jsonl"), "-Execute"]
            + lock_args("c18restoreinject"),
            env={"CW_SAVES_INJECT_LOST_REPLY": hex_of("W1") + "|1"})
        srv.stop()
        expect(p.returncode != EXIT_OK and srv.moves == 1 and
               "W1" in [e["name"] for e in card18.roots["saves"]] and
               not any(r["op"] == "result" and r.get("seq") == 2
                       for r in read_jsonl(mdir18 / "manifest_20260921-000182.jsonl")),
               "injected restore reply loss leaves no restore result", p)
        srv = fresh(card=card18)
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-Manifest",
            str(mdir18 / "manifest_20260921-000182.jsonl"), "-Execute"]
            + lock_args("c18restorereconcile"))
        srv.stop()
        expect(p.returncode == EXIT_OK and srv.moves == 0 and
               "reconciled_moved" in str(read_jsonl(mdir18 / "manifest_20260921-000182.jsonl")),
               "following leg reconciles restore without repeating it", p)

        # ---- 1. list: happy path, unsupported names reported -----------
        srv = fresh(card=default_card())
        p = run_ps(ps, repo, "list_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-OutDir", str(tmp / "inv1"), "-ConnectTimeoutSeconds", "10",
            "-PageSize", "2"] + lock_args("list1"))
        srv.stop()
        expect(p.returncode == EXIT_OK, f"list failed: rc={p.returncode}", p)
        expect("[UNSUPPORTED]" in p.stdout, "unsupported names must be reported", p)
        expect("bad." in p.stdout, "the unsupported name itself must be printed", p)
        # All five entries are directories -- an unsupported NAME does not
        # stop Select World from drawing a row for it, which is exactly why
        # those names have to be reported rather than quietly dropped.
        expect("np_rows) = 5" in p.stdout, "np_rows cross-check counts directories", p)
        inv = list((tmp / "inv1").glob("inventory_*.json"))
        expect(len(inv) == 1, "one inventory json was written", p)
        data = json.loads(inv[0].read_text(encoding="utf-8"))
        expect(data["total"] == 5 and data["unsupported"] == 1,
               f"inventory json summary wrong: {data['total']}/{data['unsupported']}", p)

        # ---- 2. list: a directory that changes between pages ------------
        srv = fresh(card=default_card(), tear_after_page=0)
        p = run_ps(ps, repo, "list_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-OutDir", str(tmp / "inv2"), "-ConnectTimeoutSeconds", "10",
            "-PageSize", "2"] + lock_args("list2"))
        srv.stop()
        expect(p.returncode == EXIT_INVENTORY,
               f"a torn inventory must exit {EXIT_INVENTORY}, got {p.returncode}", p)
        expect("modified mid-walk" in p.stdout,
               "the rejection must name the mid-walk change", p)

        # ---- 3. server without the saves capabilities, and a server whose
        # saves_protocol has moved on (a capability list can only say a verb
        # EXISTS, never that its wire shape changed underneath us).
        srv = fresh(card=default_card(), caps=OLD_CAPS)
        p = run_ps(ps, repo, "list_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-OutDir", str(tmp / "inv3"), "-ConnectTimeoutSeconds", "10"]
            + lock_args("list3"))
        srv.stop()
        expect(p.returncode == EXIT_TOO_OLD,
               f"an old server must exit {EXIT_TOO_OLD}, got {p.returncode}", p)

        srv = fresh(card=default_card())
        srv.saves_protocol = 2
        p = run_ps(ps, repo, "list_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-OutDir", str(tmp / "inv3b"), "-ConnectTimeoutSeconds", "10"]
            + lock_args("list3b"))
        srv.stop()
        expect(p.returncode == EXIT_TOO_OLD,
               f"a moved saves_protocol must exit {EXIT_TOO_OLD}, got {p.returncode}", p)

        # ---- 4. archive: DRY RUN moves nothing --------------------------
        srv = fresh(card=default_card())
        common = ["-WiiHost", "127.0.0.1", "-Port", str(srv.port),
                  "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "3",
                  "-ManifestDir", str(tmp / "man4"), "-Batch", "20260921-000004"]
        # Two keep names cannot cross a `pwsh -File` boundary as a repeated
        # parameter, and a comma-joined string is ambiguous (FAT allows a
        # comma in a world name), so a multi-name keep list goes in a file.
        keepfile = tmp / "keep4.txt"
        keepfile.write_text("CaveWii Test 1\nNew World 1\n", encoding="utf-8")
        p = run_ps(ps, repo, "archive_wii_saves.ps1", common + [
            "-Name", "New World 2", "-KeepFile", str(keepfile)] + lock_args("arc4"))
        expect(p.returncode == EXIT_OK, f"dry run failed: rc={p.returncode}", p)
        expect("DRY RUN" in p.stdout, "dry run must say so", p)
        expect("MOVING      : New World 2" in p.stdout, "the plan names the world", p)
        expect(srv.moves == 0, f"dry run moved {srv.moves} worlds", p)
        expect(not list((tmp / "man4").glob("*.jsonl")),
               "dry run must not write a manifest", p)
        srv.stop()

        # ---- 5. archive: keep-list refusals -----------------------------
        srv = fresh(card=default_card())
        common = ["-WiiHost", "127.0.0.1", "-Port", str(srv.port),
                  "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "3",
                  "-ManifestDir", str(tmp / "man5"), "-Batch", "20260921-000005"]
        p = run_ps(ps, repo, "archive_wii_saves.ps1", common + [
            "-Name", "New World 2", "-Keep", "No Such World", "-Execute"]
            + lock_args("arc5a"))
        expect(p.returncode == EXIT_REFUSED,
               f"an absent -Keep name must exit {EXIT_REFUSED}, got {p.returncode}", p)
        expect("KEEP-LIST STOP" in p.stdout, "the keep-list refusal must be named", p)
        expect(srv.moves == 0, "nothing moves on a keep-list refusal", p)

        p = run_ps(ps, repo, "archive_wii_saves.ps1", common + [
            "-Name", "New World 2", "-Keep", "", "-Execute"] + lock_args("arc5b"))
        expect(p.returncode == EXIT_REFUSED,
               f"an empty -Keep entry must exit {EXIT_REFUSED}, got {p.returncode}", p)

        p = run_ps(ps, repo, "archive_wii_saves.ps1", common + [
            "-Name", "New World 2", "-Execute"] + lock_args("arc5e"))
        expect(p.returncode == EXIT_REFUSED,
               f"NO keep list at all must exit {EXIT_REFUSED}, got {p.returncode}", p)
        expect("non-empty keep list is MANDATORY" in p.stdout,
               "the missing-keep-list refusal must say so", p)

        p = run_ps(ps, repo, "archive_wii_saves.ps1", common + [
            "-Name", "New World 2", "-Keep", "new world 2", "-Execute"]
            + lock_args("arc5c"))
        expect(p.returncode == EXIT_REFUSED,
               "-Name inside -Keep is self-contradictory and must be refused", p)

        # An unsupported name is refused and left alone.
        p = run_ps(ps, repo, "archive_wii_saves.ps1", common + [
            "-Name", "bad.", "-Keep", "New World 1", "-Execute"] + lock_args("arc5d"))
        expect(p.returncode == EXIT_REFUSED,
               f"an unsupported name must be refused, got {p.returncode}", p)
        expect("supported:false" in p.stdout, "the refusal must say why", p)
        expect(srv.moves == 0, "nothing moved across every keep-list refusal", p)
        srv.stop()

        # ---- 6. archive: root binding refuses an ambiguous card ---------
        ambiguous = Card(saves=[entry("A"), entry("B"), entry("C")],
                         saves_app=[entry("D"), entry("E"), entry("F")])
        srv = fresh(card=ambiguous)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "3",
            "-ManifestDir", str(tmp / "man6"), "-Batch", "20260921-000006",
            "-Name", "A", "-Keep", "B"] + lock_args("arc6"))
        srv.stop()
        expect(p.returncode == EXIT_ROOT_AMBIGUOUS,
               f"two plausible roots must exit {EXIT_ROOT_AMBIGUOUS}, got {p.returncode}", p)
        expect("ROOT BINDING STOP" in p.stdout, "the binding refusal must be named", p)

        # ---- 7. archive: soft-launch server refuses MOVE ----------------
        srv = fresh(card=default_card(), token_provisioned=False,
                    move_behavior="fail:token_required")
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "3",
            "-ManifestDir", str(tmp / "man7"), "-Batch", "20260921-000007",
            "-Name", "New World 2", "-Keep", "New World 1", "-Execute"]
            + lock_args("arc7"))
        srv.stop()
        expect(p.returncode == EXIT_SERVER_FAIL,
               f"a token_required refusal must exit {EXIT_SERVER_FAIL}, got {p.returncode}", p)
        expect("token_required" in p.stdout, "the server's reason must be surfaced", p)
        expect("no stage_token.txt" in p.stdout,
               "the soft-launch warning must be printed up front", p)
        recs = read_jsonl(tmp / "man7" / "manifest_20260921-000007.jsonl")
        expect(any(r["op"] == "intent" for r in recs),
               "the intent is written BEFORE the request, even for a refusal", p)
        expect(any(r["op"] == "result" and r["status"] == "FAIL" for r in recs),
               "a refusal is recorded as a result", p)

        # ---- 8. archive: reply lost AFTER the rename --------------------
        card = default_card()
        srv = fresh(card=card, move_behavior="drop_reply")
        mdir = tmp / "man8"
        base = ["-WiiHost", "127.0.0.1", "-Port", str(srv.port),
                "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "3",
                "-ManifestDir", str(mdir), "-Batch", "20260921-000008"]
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base + [
            "-Name", "New World 2", "-Keep", "New World 1", "-Execute"]
            + lock_args("arc8a"))
        expect(p.returncode != EXIT_OK, "a lost reply is not a success", p)
        expect(srv.moves == 1, f"the server did perform the move ({srv.moves})", p)
        mpath = mdir / "manifest_20260921-000008.jsonl"
        recs = read_jsonl(mpath)
        expect(any(r["op"] == "intent" for r in recs),
               "the intent survived the lost reply", p)
        expect(not any(r["op"] == "result" for r in recs),
               "no result was recorded for the lost reply", p)
        srv.stop()

        # An interrupted manifest write leaves a PARTIAL trailing line.
        with mpath.open("a", encoding="utf-8") as fh:
            fh.write('{"v":1,"op":"inte')

        # Second invocation reconciles by OBSERVING both ends.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "3",
            "-ManifestDir", str(mdir), "-Batch", "20260921-000008",
            "-Name", "New World 3", "-Keep", "New World 1"] + lock_args("arc8b"))
        expect(p.returncode == EXIT_ORPHAN_GUARD,
               f"dry run observes the lost reply but ORPHAN GUARD refuses its unrecorded intent, got {p.returncode}", p)
        expect("ORPHAN GUARD" in p.stdout and "unaccounted intent" in p.stdout,
               "the guard names the unfinished manifest intent", p)
        expect("-> moved" in p.stdout,
               "reconcile must conclude the world MOVED (src absent, dst present)", p)
        expect(srv.moves == 0,
               f"reconcile must NEVER re-issue the move ({srv.moves} moves)", p)
        # N15: that leg was a DRY RUN, so it reported the reconcile without
        # persisting it. The result lands on the next EXECUTE.
        recs = read_jsonl(mpath)
        expect(not any(r["op"] == "result" for r in recs),
               "a dry run must not write the reconcile result", p)
        srv.stop()

        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
            # its OWN batch, so scenario 9 still undoes exactly one world
            # -- the reconcile result still lands in the OLDER manifest,
            # which is the whole point of globbing the directory.
            "-ManifestDir", str(mdir), "-Batch", "20260921-000008b",
            "-Name", "New World 3", "-Keep", "New World 1", "-Execute"]
            + lock_args("arc8c"))
        srv.stop()
        recs = read_jsonl(mpath)
        expect(any(r["op"] == "result" and r.get("reason") == "reconciled_moved"
                   for r in recs), "the missing result was written on execute", p)

        # ---- 9. restore by manifest -------------------------------------
        srv = fresh(card=card)
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-Manifest", str(mpath), "-Execute"]
            + lock_args("res9"))
        expect(p.returncode == EXIT_OK, f"restore failed: rc={p.returncode}", p)
        expect(srv.moves == 1, f"restore issued {srv.moves} moves (want 1)", p)
        names = [e["name"] for e in card.roots["saves"]]
        expect("New World 2" in names,
               f"the world is back in the manifest's root: {names}", p)
        srv.stop()

        # ---- 10. reconcile STOP: both present ---------------------------
        card = default_card()
        srv = fresh(card=card, move_behavior="ghost_both")
        mdir = tmp / "man10"
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "3",
            "-ManifestDir", str(mdir), "-Batch", "20260921-000010",
            "-Name", "New World 2", "-Keep", "New World 1", "-Execute"]
            + lock_args("arc10a"))
        expect(p.returncode != EXIT_OK, "a lost reply is not a success", p)
        srv.stop()
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "3",
            "-ManifestDir", str(mdir), "-Batch", "20260921-000010",
            "-Name", "New World 3", "-Keep", "New World 1"] + lock_args("arc10b"))
        srv.stop()
        expect(p.returncode == EXIT_STOP_BOTH,
               f"both-present must exit {EXIT_STOP_BOTH}, got {p.returncode}", p)
        expect("BOTH ends" in p.stdout, "the STOP must name the condition", p)

        # ---- 11. reconcile STOP: neither present ------------------------
        card = default_card()
        srv = fresh(card=card, move_behavior="ghost_none")
        mdir = tmp / "man11"
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "3",
            "-ManifestDir", str(mdir), "-Batch", "20260921-000011",
            "-Name", "New World 2", "-Keep", "New World 1", "-Execute"]
            + lock_args("arc11a"))
        expect(p.returncode != EXIT_OK, "a lost reply is not a success", p)
        srv.stop()
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "3",
            "-ManifestDir", str(mdir), "-Batch", "20260921-000011",
            "-Name", "New World 3", "-Keep", "New World 1"] + lock_args("arc11b"))
        srv.stop()
        expect(p.returncode == EXIT_STOP_NEITHER,
               f"neither-present must exit {EXIT_STOP_NEITHER}, got {p.returncode}", p)
        expect("NEITHER end" in p.stdout, "the STOP must name the condition", p)

        # ---- 12. the rig lock is honoured -------------------------------
        srv = fresh(card=default_card())
        taken = tmp / "lock_taken"
        taken.mkdir()
        (taken / "owner.txt").write_text("somebody else", encoding="utf-8")
        p = run_ps(ps, repo, "list_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-OutDir", str(tmp / "inv12"), "-ConnectTimeoutSeconds", "10",
            "-LockPath", str(taken)])
        expect(p.returncode == EXIT_REFUSED,
               f"a held rig lock must refuse the tool, got {p.returncode}", p)
        # ...unless the caller already holds it and says so.
        p = run_ps(ps, repo, "list_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-OutDir", str(tmp / "inv12"), "-ConnectTimeoutSeconds", "10",
            "-LockPath", str(taken), "-LockHeldBy", "somebody else"])
        srv.stop()
        expect(p.returncode == EXIT_OK,
               f"-LockHeldBy must reuse the lock, got {p.returncode}", p)
        expect(taken.exists(), "a lock the tool did not take must survive", p)

        # ---- 13. C1: restore RECONCILES an archive intent with no result
        # The exact write-ahead scenario: the rename happened, the reply was
        # lost, no result line exists. The world IS in the archive. The undo
        # tool used to print "nothing to undo" and exit 0.
        card = default_card()
        srv = fresh(card=card, move_behavior="drop_reply")
        mdir = tmp / "man13"
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "3",
            "-ManifestDir", str(mdir), "-Batch", "20260921-000013",
            "-Name", "New World 2", "-Keep", "New World 1", "-Execute"]
            + lock_args("c1a"))
        expect(p.returncode != EXIT_OK, "a lost reply is not a success", p)
        expect(srv.moves == 1, "the server did move the world", p)
        srv.stop()
        mpath = mdir / "manifest_20260921-000013.jsonl"
        recs = read_jsonl(mpath)
        expect(not any(r["op"] == "result" for r in recs),
               "no result was recorded -- this is the C1 precondition", p)

        srv = fresh(card=card)
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-Manifest", str(mpath), "-Execute"]
            + lock_args("c1b"))
        expect(p.returncode == EXIT_OK,
               "restore must reconcile and undo the lost-reply move, got "
               + str(p.returncode), p)
        expect("-> moved" in p.stdout,
               "restore must RECONCILE the archive intent by listing both ends", p)
        expect("nothing to undo" not in p.stdout,
               "restore must never say 'nothing to undo' for a displaced world", p)
        names = [e["name"] for e in card.roots["saves"]]
        expect("New World 2" in names, "the world came back: " + str(names), p)
        srv.stop()

        # ---- 14. C2: reconcile globs EVERY manifest, and -Batch defaults
        # Both legs use the DEFAULT batch id (now()), which is what the
        # runbook actually does -- so leg 2 mints a different id and must
        # still find leg 1's unfinished intent.
        card = default_card()
        srv = fresh(card=card, move_behavior="drop_reply")
        mdir = tmp / "man14"
        def base14(s14):
            return ["-WiiHost", "127.0.0.1", "-Port", str(s14.port),
                    "-ConnectTimeoutSeconds", "10",
                    "-ExpectWorldCountAtLeast", "3",
                    "-ManifestDir", str(mdir)]
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base14(srv) + [
            "-Name", "New World 2", "-Keep", "New World 1", "-Execute"]
            + lock_args("c2a"))
        expect(p.returncode != EXIT_OK, "a lost reply is not a success", p)
        expect(srv.moves == 1, "the world moved", p)
        srv.stop()
        mans = sorted(mdir.glob("manifest_*.jsonl"))
        expect(len(mans) == 1, "leg 1 wrote one manifest: " + str(mans), p)

        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base14(srv) + [
            "-Name", "New World 3", "-Keep", "New World 1"] + lock_args("c2b"))
        expect("-> moved" in p.stdout,
               "a NEW default batch must still reconcile the OLDER manifest", p)
        expect(srv.moves == 0, "reconcile never re-issues a move", p)
        # N15: that leg was a dry run, so it reported without persisting.
        recs = read_jsonl(mans[0])
        expect(not any(r["op"] == "result" for r in recs),
               "a dry run must not write the reconcile result", p)
        srv.stop()

        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base14(srv) + [
            "-Name", "New World 3", "-Keep", "New World 1", "-Execute"]
            + lock_args("c2b2"))
        srv.stop()
        recs = read_jsonl(mans[0])
        expect(any(r["op"] == "result" and r.get("reason") == "reconciled_moved"
                   for r in recs),
               "the missing result was written into the OLD manifest", p)

        # An unfinished intent that did NOT move blocks a different world.
        card = default_card()
        srv = fresh(card=card, move_behavior="drop_reply_no_move")
        mdir = tmp / "man14b"
        def base14b(s14):
            return ["-WiiHost", "127.0.0.1", "-Port", str(s14.port),
                    "-ConnectTimeoutSeconds", "10",
                    "-ExpectWorldCountAtLeast", "3",
                    "-ManifestDir", str(mdir)]
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base14b(srv) + [
            "-Name", "New World 2", "-Keep", "New World 1", "-Execute"]
            + lock_args("c2c"))
        expect(p.returncode != EXIT_OK, "a dropped reply is not a success", p)
        expect(srv.moves == 0, "this leg did NOT move anything", p)
        srv.stop()
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base14b(srv) + [
            "-Name", "New World 3", "-Keep", "New World 1", "-Execute"]
            + lock_args("c2d"))
        srv.stop()
        expect(p.returncode == EXIT_REFUSED,
               "an unreconciled not_moved intent must block a DIFFERENT world, got "
               + str(p.returncode), p)
        expect("not_moved" in p.stdout, "the blocking intent must be named", p)

        # ---- 15. H3 + H6: a partial restore resumes and reports honestly
        card = Card(saves=[entry("Keeper", mtime=1757000000)])
        card.archive["20260921-000015"] = [entry("W1"), entry("W2"), entry("W3")]
        card.roots["saves"].append(entry("W2"))  # re-created by a later +new
        mdir = tmp / "man15"
        mdir.mkdir(parents=True)
        mpath = mdir / "manifest_20260921-000015.jsonl"
        lines = [{"v": 1, "op": "batch_begin", "batch": "20260921-000015",
                  "root": "saves", "root_abs": "sd:/saves",
                  "card_identity": "2026-09-21T12:00:00.0000000Z"}]
        for i, w in enumerate(("W1", "W2", "W3"), start=1):
            lines.append({"v": 1, "op": "intent", "seq": i, "dir": "archive",
                          "root": "saves", "batch": "20260921-000015",
                          "name_hex": hex_of(w), "from": "sd:/saves/" + w,
                          "to": "sd:/cw_saves_archive/20260921-000015/" + w})
            lines.append({"v": 1, "op": "result", "seq": i, "status": "PASS",
                          "reason": "moved"})
        mpath.write_text("".join(json.dumps(x) + "\n" for x in lines),
                         encoding="utf-8")

        srv = fresh(card=card)
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-Manifest", str(mpath), "-Execute"]
            + lock_args("h3a"))
        srv.stop()
        # W2's name was re-created, so the server refuses it -- that must NOT
        # abort the other two, and the run must exit non-zero naming it.
        expect(p.returncode != EXIT_OK,
               "a partial undo must NOT report success", p)
        expect("still displaced" in p.stdout.lower(),
               "the tool must name what is still in the archive", p)
        names = [e["name"] for e in card.roots["saves"]]
        expect("W1" in names and "W3" in names,
               "the unblocked worlds were restored anyway: " + str(names), p)
        left = [e["name"] for e in card.archive["20260921-000015"]]
        expect(left == ["W2"], "only the blocked world is left: " + str(left), p)

        # Re-running RESUMES: W1/W3 are already back, only W2 is attempted.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-Manifest", str(mpath)]
            + lock_args("h3b"))
        srv.stop()
        expect("1 of 3" in p.stdout,
               "the plan must say how many archived worlds are still displaced", p)

        # Unblock W2 and finish; then a further re-run is a clean no-op.
        card.roots["saves"] = [e for e in card.roots["saves"]
                               if e["name"] != "W2"]
        srv = fresh(card=card)
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-Manifest", str(mpath), "-Execute"]
            + lock_args("h3c"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "finishing the batch must succeed, got " + str(p.returncode), p)
        srv = fresh(card=card)
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-Manifest", str(mpath), "-Execute"]
            + lock_args("h3d"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "a fully restored batch re-runs as a no-op", p)
        expect("already restored" in p.stdout.lower() or "0 of 3" in p.stdout,
               "the no-op must SAY it is one", p)

        # ---- 16. H4: the root binds ONCE and is re-verified by identity
        card = Card(saves=[entry("Keeper A"), entry("Keeper B"),
                           entry("W1"), entry("W2")])
        mdir = tmp / "man16"
        keep16 = tmp / "keep16.txt"
        keep16.write_text("Keeper A\nKeeper B\n", encoding="utf-8")
        srv = fresh(card=card)
        def base16(s16):
            return ["-WiiHost", "127.0.0.1", "-Port", str(s16.port),
                    "-ConnectTimeoutSeconds", "10",
                    "-ExpectWorldCountAtLeast", "4",
                    "-ManifestDir", str(mdir), "-KeepFile", str(keep16)]
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base16(srv) + [
            "-Name", "W1", "-Execute"] + lock_args("h4a"))
        expect(p.returncode == EXIT_OK,
               "first move failed: " + str(p.returncode), p)
        expect((mdir / "root_binding.json").exists(),
               "the binding and its evidence are persisted", p)
        srv.stop()
        # The card now holds 3 directories -- BELOW the threshold that bound
        # it. The old code exited 8 here and forced -SkipRootBinding for the
        # tail of the very run the binding exists for.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base16(srv) + [
            "-Name", "W2", "-Execute"] + lock_args("h4b"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "a bound root must keep working as the batch shrinks it, got "
               + str(p.returncode), p)
        expect("re-verified by identity" in p.stdout,
               "the re-verification must be by identity, not by count", p)
        # A keep set that no longer matches the recorded one STOPS.
        keep16b = tmp / "keep16b.txt"
        keep16b.write_text("Keeper A\n", encoding="utf-8")
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "4",
            "-ManifestDir", str(mdir), "-KeepFile", str(keep16b),
            "-Name", "Keeper B"] + lock_args("h4c"))
        srv.stop()
        expect(p.returncode == EXIT_ROOT_AMBIGUOUS,
               "a changed keep set must STOP, got " + str(p.returncode), p)

        # ---- 17. M4: "could not look" is never read as "absent"
        card = default_card()
        # The five pre-move listings (archive guard, two root-binding
        # candidates, the bind probe and fresh inventory) succeed; verification
        # listings then fail, which is the "could not look" case.
        srv = fresh(card=card, list_fails_after=5)
        mdir = tmp / "man17"
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "3",
            "-ManifestDir", str(mdir), "-Batch", "20260921-000017",
            "-Name", "New World 2", "-Keep", "New World 1", "-Execute"]
            + lock_args("m4"))
        srv.stop()
        expect(p.returncode == EXIT_UNKNOWN_OBSERVATION,
               "an unobservable end must exit " + str(EXIT_UNKNOWN_OBSERVATION)
               + ", got " + str(p.returncode), p)
        expect("could not be observed" in p.stdout,
               "the tool must say it could not LOOK, not that it was absent", p)

        # ---- 18. H1 + H2: -WithNames over ONE connection, names with spaces
        card = default_card()
        srv = fresh(card=card, level_names={
            "New World 1": "New World", "New World 2": "New World",
            "New World 3": "StormWorld", "CaveWii Test 1": "CaveWii Test 1",
            "bad.": "bad."})
        p = run_ps(ps, repo, "list_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-OutDir", str(tmp / "inv18"), "-ConnectTimeoutSeconds", "10",
            "-WithNames", "-Keepers"] + lock_args("h1"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "-WithNames failed: " + str(p.returncode), p)
        expect(srv.max_concurrent == 1,
               "the tool must hold ONE connection at a time (saw "
               + str(srv.max_concurrent) + ")", p)
        inv = list((tmp / "inv18").glob("inventory_*.json"))
        data = json.loads(inv[0].read_text(encoding="utf-8"))
        by = {e["name"]: e.get("level_name") for e in data["entries"]
              if "name" in e}
        expect(by.get("New World 3") == "StormWorld",
               "a display name behind a SPACE-bearing directory decoded: "
               + str(by), p)
        expect(by.get("New World 1") == "New World",
               "every world decoded, not just space-free ones: " + str(by), p)
        expect("StormWorld" in p.stdout,
               "the renamed world reaches the keeper shortlist", p)

        # ---- 19. N1: reconcile must survive a directory of MANY manifests
        # Every -Batch-defaulting leg writes its own manifest, so from the
        # THIRD leg onward the whole pass used to go silent: foreach bound
        # the entire string[] to one loop variable, Read-CwManifest got a
        # space-joined path, Test-Path failed, and the campaign lost its
        # lost-reply recovery AND every both/neither/unknown STOP.
        card = default_card()
        mdir = tmp / "man19"

        def base19(s19, batch=None):
            # Legs 1-3 pin distinct batch ids only so the SETUP is
            # deterministic (two legs in the same second would otherwise
            # share a manifest). The leg that has to do the globbing --
            # leg 4 -- uses the DEFAULT, which is the runbook's shape and
            # the one N1 broke.
            a = ["-WiiHost", "127.0.0.1", "-Port", str(s19.port),
                 "-ConnectTimeoutSeconds", "10",
                 "-ExpectWorldCountAtLeast", "3",
                 "-ManifestDir", str(mdir), "-SkipRootBinding"]
            if batch:
                a += ["-Batch", batch]
            return a

        # leg 1: a clean move, so the directory holds a first manifest.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base19(srv, "20260921-000191") + [
            "-Name", "New World 1", "-Keep", "CaveWii Test 1", "-Execute"]
            + lock_args("n1a"))
        srv.stop()
        expect(p.returncode == EXIT_OK, "leg 1 must succeed, got " + str(p.returncode), p)

        # leg 2: the rename happens and the reply is LOST -> a second
        # manifest with an unresolved intent.
        srv = fresh(card=card, move_behavior="drop_reply")
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base19(srv, "20260921-000192") + [
            "-Name", "New World 2", "-Keep", "CaveWii Test 1", "-Execute"]
            + lock_args("n1b"))
        srv.stop()
        expect(p.returncode != EXIT_OK, "a lost reply is not a success", p)

        # leg 3: ANOTHER lost reply -> a third manifest. Reconcile has to run
        # here (two older manifests) and converge leg 2.
        srv = fresh(card=card, move_behavior="drop_reply")
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base19(srv, "20260921-000193") + [
            "-Name", "New World 3", "-Keep", "CaveWii Test 1", "-Execute"]
            + lock_args("n1c"))
        srv.stop()
        expect("-> moved" in p.stdout,
               "with TWO older manifests on disk the reconcile pass must still run", p)

        mans = sorted(mdir.glob("manifest_*.jsonl"))
        expect(len(mans) >= 3, "three legs wrote three manifests: " + str(mans), p)

        # leg 4: three-plus manifests on disk; both earlier lost replies must
        # now be resolved, and no move may be re-issued to do it.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base19(srv) + [
            "-Name", "CaveWii Test 1", "-Keep", "New World 1", "-Execute"]
            + lock_args("n1d"))
        srv.stop()
        expect(srv.moves == 0, "reconcile never re-issues a move", p)
        resolved = 0
        for m in mans:
            for r in read_jsonl(m):
                if r.get("op") == "result" and str(r.get("reason", "")).startswith("reconciled_"):
                    resolved += 1
        expect(resolved >= 2,
               "both lost replies were reconciled across the manifest set (saw "
               + str(resolved) + ")", p)

        # ...and the STOPs still reach the operator from a multi-manifest
        # directory: this is the part N1 silently disabled.
        for behaviour, want in (("ghost_both", EXIT_STOP_BOTH),
                                ("ghost_none", EXIT_STOP_NEITHER)):
            card2 = default_card()
            mdir2 = tmp / ("man19_" + behaviour)
            mdir2.mkdir(parents=True)
            # two settled manifests ahead of the interesting one
            for i in (1, 2):
                (mdir2 / ("manifest_20260921-00001%d.jsonl" % i)).write_text(
                    json.dumps({"v": 1, "op": "batch_begin",
                                "batch": "20260921-00001%d" % i,
                                "root": "saves", "root_abs": "sd:/saves",
                                "card_identity": "2026-09-21T12:00:00.0000000Z"}) + "\n",
                    encoding="utf-8")
            srv = fresh(card=card2, move_behavior=behaviour)
            p = run_ps(ps, repo, "archive_wii_saves.ps1", [
                "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
                "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
                "-ManifestDir", str(mdir2), "-Batch", "20260921-000199",
                "-Name", "New World 2", "-Keep", "New World 1", "-Execute"]
                + lock_args("n1e" + behaviour))
            srv.stop()
            expect(p.returncode != EXIT_OK, "a lost reply is not a success", p)
            srv = fresh(card=card2)
            p = run_ps(ps, repo, "archive_wii_saves.ps1", [
                "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
                "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
                "-ManifestDir", str(mdir2),
                "-Name", "New World 3", "-Keep", "New World 1"]
                + lock_args("n1f" + behaviour))
            srv.stop()
            expect(p.returncode == want,
                   behaviour + " must still STOP with " + str(want)
                   + " from a multi-manifest directory, got " + str(p.returncode), p)

        # ---- 20. N2 + N15: the block is STICKY and a dry run writes nothing
        card = default_card()
        mdir = tmp / "man20"

        def base20(s20, extra=None):
            return (["-WiiHost", "127.0.0.1", "-Port", str(s20.port),
                     "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
                     "-ManifestDir", str(mdir)] + (extra or []))

        # A request that moved NOTHING and whose reply was lost.
        srv = fresh(card=card, move_behavior="drop_reply_no_move")
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base20(srv) + [
            "-Name", "New World 2", "-Keep", "New World 1", "-Execute"]
            + lock_args("n2a"))
        srv.stop()
        expect(p.returncode != EXIT_OK, "a dropped reply is not a success", p)

        # N15: a DRY RUN must not write to the manifest at all -- and must
        # therefore not consume the block either.
        before = {m: m.read_bytes() for m in mdir.glob("manifest_*.jsonl")}
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base20(srv) + [
            "-Name", "New World 3", "-Keep", "New World 1"] + lock_args("n2b"))
        srv.stop()
        expect(p.returncode == EXIT_ORPHAN_GUARD,
               "the dry run must name its unfinished intent, got " + str(p.returncode), p)
        after = {m: m.read_bytes() for m in mdir.glob("manifest_*.jsonl")}
        expect(before == after,
               "a DRY RUN wrote to the write-ahead record; it must be read-only", p)

        # N2: the identical EXECUTE must block AGAIN -- the guard is not
        # allowed to disarm itself on the run that fires it.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base20(srv) + [
            "-Name", "New World 3", "-Keep", "New World 1", "-Execute"]
            + lock_args("n2c"))
        srv.stop()
        expect(p.returncode == EXIT_REFUSED,
               "the SAME command must block again, got " + str(p.returncode), p)
        expect(srv.moves == 0, "a blocked run moves nothing", p)

        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base20(srv) + [
            "-Name", "New World 3", "-Keep", "New World 1", "-Execute"]
            + lock_args("n2d"))
        srv.stop()
        expect(p.returncode == EXIT_REFUSED,
               "and again, indefinitely, until it is resolved", p)

        # Re-issuing THAT world is the existing allowed path, and it clears
        # the block with a logged record.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base20(srv) + [
            "-Name", "New World 2", "-Keep", "New World 1", "-Execute"]
            + lock_args("n2e"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "re-issuing the blocked world is allowed, got " + str(p.returncode), p)
        cleared = False
        for m in mdir.glob("manifest_*.jsonl"):
            for r in read_jsonl(m):
                if r.get("op") == "block_cleared":
                    cleared = True
        expect(cleared, "clearing the block is LOGGED, not silent", p)

        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", base20(srv) + [
            "-Name", "New World 3", "-Keep", "New World 1"] + lock_args("n2f"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "once cleared, a different world plans normally, got " + str(p.returncode), p)

        # ---- 21. N3: the first bind needs config + metadata, not a count
        card = Card(saves=[entry("Keeper A"), entry("Keeper B"),
                           entry("W1"), entry("W2")])
        mdir = tmp / "man21"
        keep21 = tmp / "keep21.txt"
        keep21.write_text("Keeper A\nKeeper B\n", encoding="utf-8")
        # A server whose config.json says the worlds live somewhere this tool
        # does not recognise: the COUNT still points at `saves`, so a
        # count-only bind would proceed. Astra change 1 says stop.
        srv = fresh(card=card, paths_worlds="somewhere/else",
                    level_names={"Keeper A": "Keeper A", "Keeper B": "Keeper B",
                                 "W1": "New World", "W2": "New World"})
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "4",
            "-ManifestDir", str(mdir), "-KeepFile", str(keep21),
            "-Name", "W1", "-Execute"] + lock_args("n3a"))
        srv.stop()
        expect(p.returncode == EXIT_ROOT_AMBIGUOUS,
               "an unrecognised paths.worlds must STOP the first bind, got "
               + str(p.returncode), p)
        expect("paths.worlds" in p.stdout,
               "and say which signal disagreed", p)

        # A card whose config agrees and whose worlds carry decodable
        # metadata binds, and records all three signals as evidence.
        srv = fresh(card=card, paths_worlds="saves",
                    level_names={"Keeper A": "Keeper A", "Keeper B": "Keeper B",
                                 "W1": "New World", "W2": "New World"})
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "4",
            "-ManifestDir", str(mdir), "-KeepFile", str(keep21),
            "-Name", "W1", "-Execute"] + lock_args("n3b"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "three agreeing signals must bind, got " + str(p.returncode), p)
        bind = json.loads((mdir / "root_binding.json").read_text(encoding="utf-8"))
        for field in ("config_paths_worlds", "metadata_samples", "evidence"):
            expect(field in bind, "the binding records " + field + ": " + str(bind), p)

        # A card where no world's metadata decodes cannot corroborate (c).
        card3 = Card(saves=[entry("Keeper A"), entry("Keeper B"),
                            entry("W1"), entry("W2")])
        mdir3 = tmp / "man21c"
        srv = fresh(card=card3, paths_worlds="saves", level_names={})
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "4",
            "-ManifestDir", str(mdir3), "-KeepFile", str(keep21),
            "-Name", "W1", "-Execute"] + lock_args("n3c"))
        srv.stop()
        expect(p.returncode == EXIT_ROOT_AMBIGUOUS,
               "no identifiable world metadata must STOP, got " + str(p.returncode), p)

        # ---- 22. N6: one unstat-able row must not block the undo path
        card = Card(saves=[entry("Keeper"), entry("broken", stat_ok=False)])
        card.archive["20260921-000022"] = [entry("W1")]
        mdir = tmp / "man22"
        mdir.mkdir(parents=True)
        mpath = mdir / "manifest_20260921-000022.jsonl"
        mpath.write_text("".join(json.dumps(x) + "\n" for x in [
            {"v": 1, "op": "batch_begin", "batch": "20260921-000022",
             "root": "saves", "root_abs": "sd:/saves",
             "card_identity": "2026-09-21T12:00:00.0000000Z"},
            {"v": 1, "op": "intent", "seq": 1, "dir": "archive", "root": "saves",
             "batch": "20260921-000022", "name_hex": hex_of("W1"),
             "from": "sd:/saves/W1",
             "to": "sd:/cw_saves_archive/20260921-000022/W1"},
            {"v": 1, "op": "result", "seq": 1, "status": "PASS", "reason": "moved"},
        ]), encoding="utf-8")
        srv = fresh(card=card)
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-Manifest", str(mpath), "-Execute"]
            + lock_args("n6a"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "a corrupt row unrelated to the restored world must not block the "
               "recovery tool, got " + str(p.returncode), p)
        names = [e["name"] for e in card.roots["saves"]]
        expect("W1" in names, "the world came back: " + str(names), p)

        # ...and it is still refused as a MOVE target.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
            "-ManifestDir", str(mdir),
            "-Name", "broken", "-Keep", "Keeper", "-Execute"]
            + lock_args("n6b"))
        srv.stop()
        expect(p.returncode == EXIT_REFUSED,
               "an unstat-able entry may never be MOVED, got " + str(p.returncode), p)
        expect(srv.moves == 0, "and nothing moved", p)

        # ---- 23. N5: a meta READ ERROR is not a short successful file
        card = default_card()
        srv = fresh(card=card, meta_read_error=True,
                    level_names={"New World 1": "New World"})
        p = run_ps(ps, repo, "list_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-OutDir", str(tmp / "inv23"), "-ConnectTimeoutSeconds", "10",
            "-WithNames"] + lock_args("n5"))
        srv.stop()
        expect(p.returncode == EXIT_OK, "the listing itself still succeeds", p)
        inv = list((tmp / "inv23").glob("inventory_*.json"))
        data = json.loads(inv[0].read_text(encoding="utf-8"))
        for e in data["entries"]:
            expect(not e.get("level_name"),
                   "a read error must decode to NO name, never a truncated one: "
                   + str(e.get("level_name")), p)

        # ---- 24. N12: a keep name matching two rows is as fatal as a target
        card = Card(saves=[entry("Dup"), entry("DUP"), entry("W1")])
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
            "-ManifestDir", str(tmp / "man24"),
            "-Name", "W1", "-Keep", "Dup", "-Execute"] + lock_args("n12"))
        srv.stop()
        expect(p.returncode == EXIT_REFUSED,
               "an ambiguous -Keep name must STOP like an ambiguous -Name, got "
               + str(p.returncode), p)
        expect(srv.moves == 0, "and nothing moved", p)

        # ---- 25. T1: a block that was CLEARED ONCE must still be able to
        # block again. The clear set used to be keyed on name_hex alone --
        # no seq, no utc, no per-file pairing -- so one old block_cleared
        # pre-cleared every FUTURE block for that world, permanently. The
        # guard survived exactly one carried leg and then went quiet.
        # EXACTLY ONE world is ever blocked here, and every other leg uses a
        # FRESH target that is still on the card -- otherwise a leg that
        # wrongly proceeds would archive its target and the NEXT leg would
        # refuse with "matched 0 directories", i.e. the test would pass for
        # the wrong reason. (It did, on the first run of this fixture.)
        card = Card(saves=[entry("Keeper"), entry("Blocked"), entry("A1"),
                           entry("A2"), entry("A3"), entry("A4"), entry("A5")])
        mdir = tmp / "man25"
        leg = [0]

        def run25(behaviour, args):
            """One leg, its own server, its own batch id (so every leg writes
            its own manifest -- the shape a -Batch-defaulting campaign has,
            and the one where the block has to be CARRIED from disk)."""
            leg[0] += 1
            s = fresh(card=card, move_behavior=behaviour)
            r = run_ps(ps, repo, "archive_wii_saves.ps1", [
                "-WiiHost", "127.0.0.1", "-Port", str(s.port),
                "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
                "-ManifestDir", str(mdir),
                "-Batch", "20260921-0000%02d" % (leg[0] + 40),
                "-Keep", "Keeper"] + args + lock_args("t1_%d" % leg[0]))
            s.stop()
            return r, s

        # 1. The world that is asked to move and does NOT move.
        p, _ = run25("drop_reply_no_move", ["-Name", "Blocked", "-Execute"])
        expect(p.returncode != EXIT_OK, "a dropped reply is not a success", p)

        # 2. First block: derived in this run's reconcile pass.
        p, s = run25("ok", ["-Name", "A1", "-Execute"])
        expect(p.returncode == EXIT_REFUSED, "leg 2 must BLOCK, got " + str(p.returncode), p)
        expect(s.moves == 0, "a blocked leg moves nothing", p)

        # 3-4. Resolve it deliberately; the campaign resumes.
        p, _ = run25("ok", ["-Name", "Blocked", "-ResolveBlock", "Blocked", "-Execute"])
        expect(p.returncode == EXIT_OK, "-ResolveBlock must clear it, got " + str(p.returncode), p)
        p, s = run25("ok", ["-Name", "A1", "-Execute"])
        expect(p.returncode == EXIT_OK, "after the clear the campaign resumes, got " + str(p.returncode), p)
        expect(s.moves == 1, "and that leg moved its world", p)

        # 5. A SECOND not_moved on the SAME world, with the old clear on disk.
        p, _ = run25("drop_reply_no_move", ["-Name", "Blocked", "-Execute"])
        expect(p.returncode != EXIT_OK, "a dropped reply is not a success", p)

        # 6. The block is derived IN THIS RUN -- this leg always held.
        p, s = run25("ok", ["-Name", "A2", "-Execute"])
        expect(p.returncode == EXIT_REFUSED,
               "the second block must hold on the leg that derives it, got "
               + str(p.returncode), p)
        expect(s.moves == 0, "and move nothing", p)

        # 7-8. The block is now CARRIED FROM DISK, behind an OLDER
        # block_cleared for the same world. This is where the guard used to
        # go silent: rc 0, one world archived, no [BLOCK] line at all.
        for attempt, target in ((1, "A3"), (2, "A4")):
            p, s = run25("ok", ["-Name", target, "-Execute"])
            expect(p.returncode == EXIT_REFUSED,
                   "carried block, attempt %d (%s): an old block_cleared must "
                   "NOT disarm a later block (got %d)" % (attempt, target, p.returncode), p)
            expect(s.moves == 0,
                   "carried block, attempt %d: nothing may move" % attempt, p)
            expect("BLOCKED" in p.stdout,
                   "carried block, attempt %d: the block must be PRINTED" % attempt, p)
            names = [e["name"] for e in card.roots["saves"]]
            expect(target in names,
                   "carried block, attempt %d: %s is still on the card: %s"
                   % (attempt, target, names), p)

        # 9-10. And it is still resolvable -- a SECOND time, deliberately.
        p, _ = run25("ok", ["-Name", "Blocked", "-ResolveBlock", "Blocked", "-Execute"])
        expect(p.returncode == EXIT_OK,
               "-ResolveBlock must work a SECOND time, got " + str(p.returncode), p)
        p, s = run25("ok", ["-Name", "A5", "-Execute"])
        expect(p.returncode == EXIT_OK,
               "with the block resolved the campaign runs, got " + str(p.returncode), p)
        expect(s.moves == 1, "and moves exactly one world", p)

        # ---- 26. T3: the block must be DURABLE BEFORE the result that
        # closes the intent. Both are Flush($true)-per-line, so the order on
        # the platter is the order in the source: writing the result first
        # left a one-flush window in which a power event loses the block and
        # keeps the intent closed forever.
        for crash, why in (("result", "a crash AFTER the block flush and BEFORE the result"),
                           ("block", "a crash in the window the OLD write order left open")):
            card = default_card()
            mdir = tmp / ("man26_" + crash)

            def base26(s26, batch):
                return ["-WiiHost", "127.0.0.1", "-Port", str(s26.port),
                        "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
                        "-ManifestDir", str(mdir), "-Batch", batch]

            srv = fresh(card=card, move_behavior="drop_reply_no_move")
            p = run_ps(ps, repo, "archive_wii_saves.ps1",
                       base26(srv, "20260921-000261") +
                       ["-Name", "New World 2", "-Keep", "CaveWii Test 1", "-Execute"]
                       + lock_args("t3a" + crash))
            srv.stop()
            expect(p.returncode != EXIT_OK, "a dropped reply is not a success", p)

            srv = fresh(card=card)
            p = run_ps(ps, repo, "archive_wii_saves.ps1",
                       base26(srv, "20260921-000262") +
                       ["-Name", "New World 3", "-Keep", "CaveWii Test 1", "-Execute"]
                       + lock_args("t3b" + crash))
            srv.stop()
            expect(p.returncode == EXIT_REFUSED, "the block fires, got " + str(p.returncode), p)

            # The manifest holding the unresolved intent must now carry the
            # block BEFORE the result -- that ordering IS the fix.
            mp = mdir / "manifest_20260921-000261.jsonl"
            ops = [r["op"] for r in read_jsonl(mp)]
            expect("block" in ops, "a block record was written: " + str(ops), p)
            expect(ops.index("block") < ops.index("result"),
                   "the BLOCK must be flushed before the RESULT that closes the "
                   "intent: " + str(ops), p)

            # Inject the crash: drop exactly one of the two lines.
            kept = [ln for ln in mp.read_text(encoding="utf-8").splitlines()
                    if ln.strip() and json.loads(ln).get("op") != crash]
            mp.write_text("".join(x + "\n" for x in kept), encoding="utf-8")

            srv = fresh(card=card)
            p = run_ps(ps, repo, "archive_wii_saves.ps1",
                       base26(srv, "20260921-000263") +
                       ["-Name", "New World 3", "-Keep", "CaveWii Test 1", "-Execute"]
                       + lock_args("t3c" + crash))
            srv.stop()
            expect(p.returncode == EXIT_REFUSED,
                   why + " must still leave the world blocked, got "
                   + str(p.returncode), p)
            expect(srv.moves == 0, why + ": nothing may move", p)

        # ---- 27. T1: three worlds, interleaved blocks and clears, FOUR
        # manifests. Hand-built so the ordering is exact rather than
        # whatever the clock did, and so a stale clear sits in the
        # EARLIEST-sorting file -- the arrangement the old name_hex-keyed
        # set honoured in either direction.
        card = Card(saves=[entry("Keeper"), entry("W1"), entry("W2"),
                           entry("W3"), entry("Target")])
        mdir = tmp / "man27"
        mdir.mkdir(parents=True)

        def blk(world, seq, utc):
            return {"v": 1, "op": "block", "reason": "not_moved",
                    "name_hex": hex_of(world), "from": "sd:/saves/" + world,
                    "name_display": world, "seq": seq,
                    "utc": "2026-09-21T%s.0000000Z" % utc}

        def clr(world, utc, block_manifest=None, block_seq=None):
            r = {"v": 1, "op": "block_cleared", "name_hex": hex_of(world),
                 "reason": "operator_override",
                 "utc": "2026-09-21T%s.0000000Z" % utc}
            if block_manifest is not None:
                r["block_manifest"] = str(mdir / block_manifest)
                r["block_seq"] = block_seq
            return r

        files = {
            # m1 sorts FIRST and carries a clear for W3 that PRE-DATES W3's
            # block (which lands in m4): it must not disarm it.
            "manifest_20260921-000271.jsonl": [blk("W1", 1, "10:00:00"),
                                               clr("W3", "10:01:00")],
            # W1's block is cleared here; W2 is blocked here.
            "manifest_20260921-000272.jsonl": [clr("W1", "10:05:00"),
                                               blk("W2", 2, "10:06:00")],
            # W1 is blocked AGAIN, after its clear -> active.
            "manifest_20260921-000273.jsonl": [blk("W1", 3, "11:00:00"),
                                               clr("W3", "11:01:00")],
            # W2's clear (explicitly paired) and W3's block, which is the
            # LAST word on W3.
            "manifest_20260921-000274.jsonl": [
                clr("W2", "12:00:00", "manifest_20260921-000272.jsonl", 2),
                blk("W3", 4, "12:01:00")],
        }
        for fname, recs in files.items():
            (mdir / fname).write_text(
                "".join(json.dumps(r) + "\n" for r in recs), encoding="utf-8")

        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
            "-ManifestDir", str(mdir), "-Batch", "20260921-000275",
            "-Name", "Target", "-Keep", "Keeper", "-Execute"]
            + lock_args("t1x"))
        srv.stop()
        expect(p.returncode == EXIT_REFUSED,
               "two worlds are still blocked, so the run must refuse, got "
               + str(p.returncode), p)
        expect(srv.moves == 0, "and move nothing", p)
        blocked_line = [l for l in p.stdout.splitlines() if "BLOCKED:" in l]
        expect(len(blocked_line) == 1, "exactly one BLOCKED line: " + str(blocked_line), p)
        expect("W1" in blocked_line[0], "W1 was re-blocked after its clear", p)
        expect("W3" in blocked_line[0],
               "W3's block is LATER than the stale clear in the first file", p)
        expect("W2" not in blocked_line[0],
               "W2's block was cleared by a clear that follows it: " + blocked_line[0], p)

        # ---- 28. T2 (+ N15's promise): a DRY RUN writes NOTHING.
        # N15 made the write-ahead RECORD read-only in a dry run, but the
        # run still created the manifest directory and persisted
        # root_binding.json -- the one write that cannot be taken back,
        # because the binding's keep set is its evidence and every later
        # invocation is checked against it. A dry run performed with a keep
        # list the operator was still editing locked the campaign to that
        # keep set, and the documented way out was deleting a file by hand.
        # The runbook says "A dry run writes nothing" in bold; this is what
        # makes that sentence true, over every path a dry run can take.
        def snap(d: Path):
            """Every byte under the manifest directory, or None if it does
            not exist. A dry run must not move this value."""
            if not d.exists():
                return None
            return {str(f.relative_to(d)): f.read_bytes()
                    for f in sorted(d.rglob("*")) if f.is_file()}

        keep28 = tmp / "keep28.txt"
        keep28.write_text("Keeper A\nKeeper B\n", encoding="utf-8")

        def base28(s28, mdir28):
            return ["-WiiHost", "127.0.0.1", "-Port", str(s28.port),
                    "-ConnectTimeoutSeconds", "10",
                    "-ExpectWorldCountAtLeast", "4",
                    "-ManifestDir", str(mdir28), "-KeepFile", str(keep28)]

        # (a) FIRST BIND -- the path that persisted the binding.
        card = Card(saves=[entry("Keeper A"), entry("Keeper B"),
                           entry("W1"), entry("W2"), entry("W3")])
        mdir = tmp / "man28a"
        srv = fresh(card=card, paths_worlds="saves")
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base28(srv, mdir) + ["-Name", "W1"] + lock_args("t2a"))
        srv.stop()
        expect(p.returncode == EXIT_OK, "the dry run itself succeeds, got " + str(p.returncode), p)
        expect("DRY RUN" in p.stdout, "and says it is one", p)
        expect(srv.moves == 0, "and sends no move", p)
        expect(snap(mdir) is None,
               "a dry run must not even CREATE the manifest directory (found: "
               + str(snap(mdir)) + ")", p)
        expect(not (mdir / "root_binding.json").exists(),
               "a dry run must not persist root_binding.json -- it is the one "
               "write that cannot be taken back", p)

        # ...and the -Execute that follows still binds, so nothing was lost
        # by not writing: the dry run simply promised nothing.
        srv = fresh(card=card, paths_worlds="saves")
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base28(srv, mdir) + ["-Name", "W1", "-Execute"] + lock_args("t2b"))
        srv.stop()
        expect(p.returncode == EXIT_OK, "the execute binds, got " + str(p.returncode), p)
        expect((mdir / "root_binding.json").exists(),
               "-Execute is what persists the binding", p)

        # (b) RECONCILE PENDING -- a dry run that resolves a lost reply must
        # report it and still write nothing, binding file included.
        srv = fresh(card=card, move_behavior="drop_reply")
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base28(srv, mdir) + ["-Name", "W2", "-Execute"] + lock_args("t2c"))
        srv.stop()
        expect(p.returncode != EXIT_OK, "a lost reply is not a success", p)
        before = snap(mdir)
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base28(srv, mdir) + ["-Name", "W3"] + lock_args("t2d"))
        srv.stop()
        expect(p.returncode == EXIT_ORPHAN_GUARD,
               "the dry run observes but refuses the unfinished intent, got " + str(p.returncode), p)
        expect("-> moved" in p.stdout, "and REPORTS the verdict it observed", p)
        expect(snap(mdir) == before,
               "a dry run with a reconcile pending wrote to the manifest directory", p)

        # (c) BLOCK PENDING -- the path N15 was raised about, now checked
        # over the whole directory rather than the .jsonl files alone.
        srv = fresh(card=card, move_behavior="drop_reply_no_move")
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base28(srv, mdir) + ["-Name", "W3", "-Execute"] + lock_args("t2e"))
        srv.stop()
        expect(p.returncode != EXIT_OK, "a dropped reply is not a success", p)
        before = snap(mdir)
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base28(srv, mdir) + ["-Name", "Keeper A"] + lock_args("t2f"))
        srv.stop()
        expect(p.returncode != EXIT_OK, "the dry run must still refuse", p)
        expect(snap(mdir) == before,
               "a dry run with a block pending wrote to the manifest directory", p)
        # ...including the -ResolveBlock dry run, which reports and exits.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base28(srv, mdir) + ["-Name", "W3", "-ResolveBlock", "W3"]
                   + lock_args("t2g"))
        srv.stop()
        expect(p.returncode == EXIT_OK, "a -ResolveBlock dry run reports, got " + str(p.returncode), p)
        expect(snap(mdir) == before,
               "a -ResolveBlock DRY RUN must not clear anything on disk", p)

        # ---- 29. T4 + T5: the first bind's metadata signal.
        # T4 -- signal (c) sampled `$probeDirs | Select-Object -First 3` and
        # STOPped if none of those three decoded, so a card whose first
        # three directory entries are not worlds could not be bound at all.
        # On FAT readdir order is directory-slot order, i.e. whatever the
        # card happens to hold first, not a choice. The only documented
        # escape was -SkipRootBinding, which throws away ALL THREE signals
        # -- the worst possible reaction to one of them failing.
        # T5 -- -ExpectWorldName was matched only against those same three
        # rows, but the names an operator reads off the console are the
        # KEEPERS, which on a 646-world card are almost certainly not rows
        # 1-3. The parameter that exists to strengthen the bind stopped it.
        keep29 = tmp / "keep29.txt"
        keep29.write_text("Keeper A\nKeeper B\n", encoding="utf-8")

        def bind29(srv29, mdir29, extra):
            return run_ps(ps, repo, "archive_wii_saves.ps1", [
                "-WiiHost", "127.0.0.1", "-Port", str(srv29.port),
                "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "4",
                "-ManifestDir", str(mdir29), "-KeepFile", str(keep29)] + extra)

        # (a) The first three rows are NOT worlds; real worlds sit behind
        # them. The bind must scan past the junk instead of giving up.
        card = Card(saves=[entry("junk1"), entry("junk2"), entry("junk3"),
                           entry("Keeper A"), entry("Keeper B"), entry("W1")])
        srv = fresh(card=card, paths_worlds="saves",
                    level_names={"Keeper A": "Keeper A", "Keeper B": "Keeper B",
                                 "W1": "New World"})
        p = bind29(srv, tmp / "man29a", ["-Name", "W1", "-Execute"] + lock_args("t4a"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "junk in the first three rows must not make a card unbindable, got "
               + str(p.returncode), p)
        expect("metadata reads" in p.stdout,
               "the bind must SAY how many rows it read: " + p.stdout, p)

        # (b) The scan is BOUNDED, and the bound is the operator's to raise.
        many = [entry("junk%02d" % i) for i in range(40)]
        real = [entry("Keeper A"), entry("Keeper B"), entry("W1")]
        names29 = {"Keeper A": "Keeper A", "Keeper B": "Keeper B", "W1": "New World"}
        srv = fresh(card=Card(saves=many + real), paths_worlds="saves",
                    level_names=names29)
        p = bind29(srv, tmp / "man29b", ["-Name", "W1", "-Execute"] + lock_args("t4b"))
        srv.stop()
        expect(p.returncode == EXIT_ROOT_AMBIGUOUS,
               "past the read budget the bind must STOP, not scan a whole card, got "
               + str(p.returncode), p)
        expect("-MetaSampleMaxReads" in p.stdout,
               "and the STOP must name the knob that raises the bound", p)

        srv = fresh(card=Card(saves=many + real), paths_worlds="saves",
                    level_names=names29)
        p = bind29(srv, tmp / "man29c", ["-Name", "W1", "-MetaSampleMaxReads", "64",
                                         "-Execute"] + lock_args("t4c"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "raising the bound must bind the same card, got " + str(p.returncode), p)

        # (c) A card with NO readable world anywhere still refuses, named.
        srv = fresh(card=Card(saves=[entry("a"), entry("b"), entry("c"), entry("d")]),
                    paths_worlds="saves", level_names={})
        p = bind29(srv, tmp / "man29d", ["-Name", "a", "-Execute"] + lock_args("t4d"))
        srv.stop()
        expect(p.returncode == EXIT_ROOT_AMBIGUOUS,
               "zero readable worlds must STOP, got " + str(p.returncode), p)
        expect("signal (c)" in p.stdout, "and name the signal that disagreed", p)

        # (d) T5: the keepers are at the END of the listing and are named by
        # their exact DIRECTORY name -- which needs no metadata read at all.
        tail_card = [entry("W%d" % i) for i in range(1, 6)] + [
            entry("Keeper A"), entry("Keeper B")]
        srv = fresh(card=Card(saves=tail_card), paths_worlds="saves")
        p = bind29(srv, tmp / "man29e",
                   ["-Name", "W1", "-ExpectWorldName", "Keeper A", "-Execute"]
                   + lock_args("t5a"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "-ExpectWorldName must be looked up across the WHOLE listing, got "
               + str(p.returncode), p)

        # (e) ...and by decoded LevelName too, for a world the operator
        # knows by the name the console shows rather than its directory.
        renamed = ([entry("W%d" % i) for i in range(1, 6)]
                   + [entry("New World 120"), entry("Keeper A"), entry("Keeper B")])
        srv = fresh(card=Card(saves=renamed), paths_worlds="saves",
                    level_names=dict({"W%d" % i: "New World" for i in range(1, 6)},
                                     **{"New World 120": "CaveWii Test 1",
                                        "Keeper A": "Keeper A", "Keeper B": "Keeper B"}))
        p = bind29(srv, tmp / "man29f",
                   ["-Name", "W1", "-ExpectWorldName", "CaveWii Test 1", "-Execute"]
                   + lock_args("t5b"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "a display name anywhere in the listing must resolve, got "
               + str(p.returncode), p)

        # (f) A name that is on neither side still STOPs, and says so.
        srv = fresh(card=Card(saves=tail_card), paths_worlds="saves")
        p = bind29(srv, tmp / "man29g",
                   ["-Name", "W1", "-ExpectWorldName", "Not On This Card", "-Execute"]
                   + lock_args("t5c"))
        srv.stop()
        expect(p.returncode == EXIT_ROOT_AMBIGUOUS,
               "an absent -ExpectWorldName must STOP, got " + str(p.returncode), p)
        expect("Not On This Card" in p.stdout, "and name what it could not find", p)

        # ---- 30. T6 (the second half of N9): turning a name the operator
        # TYPED into a block is a resolution against RECORDED bytes, never a
        # re-encoding. -ResolveBlock compared the block's name_hex against
        # ConvertTo-CwHex(UTF8($typed)) -- the exact construct N9 removed
        # from the re-issue path -- and it is by design the ONLY way out of
        # a block on a world that has since been archived, so for a world
        # whose on-card bytes are not UTF-8 the block was unclearable
        # without hand-editing the write-ahead record.
        NON_UTF8 = "\xe9dition"              # latin-1 on the card
        NON_UTF8_HEX = "e9646974696f6e"      # ...and NOT valid UTF-8
        PLACEHOLDER = "(non-ascii name, hex %s)" % NON_UTF8_HEX
        expect(hex_of(NON_UTF8) == NON_UTF8_HEX,
               "the fake must carry the card's own bytes: " + hex_of(NON_UTF8))

        card = Card(saves=[entry("Keeper"), entry(NON_UTF8), entry("W1"), entry("W2")])
        srv = fresh(card=card)
        p = run_ps(ps, repo, "list_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-OutDir", str(tmp / "inv30"), "-ConnectTimeoutSeconds", "10"]
            + lock_args("t6a"))
        srv.stop()
        expect(p.returncode == EXIT_OK, "listing a non-UTF-8 name must work: " + str(p.returncode), p)
        inv = list((tmp / "inv30").glob("inventory_*.json"))
        rows = {e["name_hex"]: e
                for e in json.loads(inv[0].read_text(encoding="utf-8"))["entries"]}
        expect(NON_UTF8_HEX in rows,
               "the card's own bytes reach the inventory: " + str(list(rows)), p)
        expect("name" not in rows[NON_UTF8_HEX],
               "a name that is not printable ASCII carries no readable `name` field", p)

        mdir = tmp / "man30"

        def base30(s30, batch):
            return ["-WiiHost", "127.0.0.1", "-Port", str(s30.port),
                    "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
                    "-ManifestDir", str(mdir), "-Batch", batch, "-Keep", "Keeper"]

        # Block that world, through the tool, by the only handle it has.
        srv = fresh(card=card, move_behavior="drop_reply_no_move")
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base30(srv, "20260921-000301") + ["-Name", PLACEHOLDER, "-Execute"]
                   + lock_args("t6b"))
        srv.stop()
        expect(p.returncode != EXIT_OK, "a dropped reply is not a success", p)
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base30(srv, "20260921-000302") + ["-Name", "W1", "-Execute"]
                   + lock_args("t6c"))
        srv.stop()
        expect(p.returncode == EXIT_REFUSED,
               "the non-UTF-8 world's block must fire, got " + str(p.returncode), p)
        expect(NON_UTF8_HEX in p.stdout,
               "and must print the card's HEX, which is the only handle an "
               "operator has for it: " + p.stdout, p)

        # -ResolveBlockHex is the way out for a name nobody can type.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base30(srv, "20260921-000303") +
                   ["-Name", "W1", "-ResolveBlockHex", NON_UTF8_HEX, "-Execute"]
                   + lock_args("t6d"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "-ResolveBlockHex must clear a block on a non-UTF-8 name, got "
               + str(p.returncode), p)
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base30(srv, "20260921-000304") + ["-Name", "W1", "-Execute"]
                   + lock_args("t6e"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "and the campaign resumes, got " + str(p.returncode), p)

        # The re-encode itself, pinned both ways: a block whose name_hex IS
        # UTF8(what the operator would type) but whose RECORDED name is
        # something else. The old code cleared it from the typed name and
        # refused the recorded one; the correct tool does the opposite.
        # Both strings are ASCII, so nothing here depends on argv encoding.
        mdir2 = tmp / "man30b"
        mdir2.mkdir(parents=True)
        (mdir2 / "manifest_20260921-000305.jsonl").write_text(
            json.dumps({"v": 1, "op": "block", "reason": "not_moved",
                        "name_hex": hex_of("Ghost"), "from": "sd:/saves/Phantom",
                        "name_display": "Phantom", "seq": 1,
                        "utc": "2026-09-21T10:00:00.0000000Z"}) + "\n",
            encoding="utf-8")

        def resolve30(arg, batch, tag):
            s = fresh(card=card)
            r = run_ps(ps, repo, "archive_wii_saves.ps1", [
                "-WiiHost", "127.0.0.1", "-Port", str(s.port),
                "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
                "-ManifestDir", str(mdir2), "-Batch", batch,
                "-Name", "W2", "-Keep", "Keeper",
                "-ResolveBlock", arg, "-Execute"] + lock_args(tag))
            s.stop()
            return r

        p = resolve30("Ghost", "20260921-000306", "t6f")
        expect(p.returncode == EXIT_REFUSED,
               "a UTF-8 re-encoding of the typed name must NOT resolve a block, got "
               + str(p.returncode), p)
        expect("Phantom" in p.stdout,
               "and the refusal must name what IS blocked: " + p.stdout, p)
        p = resolve30("Phantom", "20260921-000307", "t6g")
        expect(p.returncode == EXIT_OK,
               "the RECORDED name is what resolves it, got " + str(p.returncode), p)

        # ---- 31. T7: an ABSENT paths.worlds is the GAME's default, not
        # ambiguity. config_read_string(&gstate.config_user, "paths.worlds",
        # "saves") -- source/game/gui/screen_select_world.c:187 -- so a card
        # whose config.json simply has no paths key is a card the game reads
        # as "saves". The tool used to exit 8 on it, with a message
        # ("absent or unreadable") that pointed the operator at a fetch
        # failure that had not happened.
        card = Card(saves=[entry("Keeper A"), entry("Keeper B"),
                           entry("W1"), entry("W2")])
        keep31 = tmp / "keep31.txt"
        keep31.write_text("Keeper A\nKeeper B\n", encoding="utf-8")

        def bind31(s31, mdir31, tag):
            return run_ps(ps, repo, "archive_wii_saves.ps1", [
                "-WiiHost", "127.0.0.1", "-Port", str(s31.port),
                "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "4",
                "-ManifestDir", str(mdir31), "-KeepFile", str(keep31),
                "-Name", "W1", "-Execute"] + lock_args(tag))

        srv = fresh(card=card, paths_worlds=None)
        p = bind31(srv, tmp / "man31a", "t7a")
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "a config with NO paths key is the game's default 'saves' and must "
               "bind, got " + str(p.returncode), p)
        bind = json.loads((tmp / "man31a" / "root_binding.json").read_text(encoding="utf-8"))
        expect("absent" in bind["config_paths_worlds"] or bind["config_paths_worlds"] == "",
               "and the binding records that it was absent: " + str(bind["config_paths_worlds"]), p)
        expect("screen_select_world.c:187" in bind["evidence"],
               "the evidence cites the game's own default: " + bind["evidence"], p)

        # A config that could not be READ is a different fact and still STOPs.
        card2 = Card(saves=[entry("Keeper A"), entry("Keeper B"),
                            entry("W1"), entry("W2")])
        srv = fresh(card=card2, config_fetch_fails=True)
        p = bind31(srv, tmp / "man31b", "t7b")
        srv.stop()
        expect(p.returncode == EXIT_ROOT_AMBIGUOUS,
               "an unreadable config must STOP, got " + str(p.returncode), p)
        expect("could not be READ" in p.stdout,
               "and must say so rather than call it an absent key: " + p.stdout, p)

        # Fresh card: confirmed missing user config uses shipped defaults.
        for suffix, worlds, expected in (("c", "saves", EXIT_OK),
                                         ("d", "elsewhere", EXIT_ROOT_AMBIGUOUS)):
            card = Card(saves=[entry("Keeper A"), entry("Keeper B"),
                               entry("W1"), entry("W2")])
            srv = fresh(card=card, config_missing=True, paths_worlds=worlds)
            p = bind31(srv, tmp / ("man31" + suffix), "t7" + suffix)
            srv.stop()
            expect(p.returncode == expected,
                   "fresh card must bind only when config_wii.json corroborates root", p)

        # ---- 32. T9: the N6 guards must be REACHABLE against the shape the
        # real server emits. On a failed stat the server does not know the
        # type and now says so (is_dir:null); the client's directory filter
        # dropped such a row before it ever looked at stat_ok, so -Name and
        # -Keep reported "matched 0 directories" / "not in the fresh
        # listing" instead of the refusal the runbook documents.
        card = Card(saves=[entry("Keeper"), entry("broken", stat_ok=False),
                           entry("W1")])
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
            "-ManifestDir", str(tmp / "man32a"),
            "-Name", "broken", "-Keep", "Keeper", "-Execute"] + lock_args("t9a"))
        srv.stop()
        expect(p.returncode == EXIT_REFUSED,
               "an unstat-able row may never be MOVED, got " + str(p.returncode), p)
        expect("could not stat" in p.stdout,
               "and the refusal must be the DOCUMENTED one, not 'matched 0 "
               "directories': " + p.stdout, p)
        expect(srv.moves == 0, "and nothing moved", p)

        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
            "-ManifestDir", str(tmp / "man32b"),
            "-Name", "W1", "-Keep", "broken", "-Execute"] + lock_args("t9b"))
        srv.stop()
        expect(p.returncode == EXIT_REFUSED,
               "an unstat-able row may never be a KEEPER, got " + str(p.returncode), p)
        expect("could not stat these -Keep rows" in p.stdout,
               "and that refusal must be the documented one too: " + p.stdout, p)
        expect(srv.moves == 0, "and nothing moved", p)

        # It is still LISTED, still reported, and still does not invalidate
        # the rest of the listing -- nor does it count as a Select World row,
        # because the game stats every entry too.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "list_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-OutDir", str(tmp / "inv32"), "-ConnectTimeoutSeconds", "10"]
            + lock_args("t9c"))
        srv.stop()
        expect(p.returncode == EXIT_OK, "listing still works: " + str(p.returncode), p)
        expect("[UNREADABLE]" in p.stdout, "the unreadable row is reported", p)
        expect("np_rows) = 2" in p.stdout,
               "and is NOT counted as a Select World row (the game stats too): "
               + p.stdout, p)

        # ---- 33. C15-1 (CRITICAL): a VERIFY-path STOP must survive the leg
        # that raised it. The verify path wrote its `result` BEFORE looking
        # at the verdict, which CLOSES the intent -- so a stop_* printed
        # "touch nothing, escalate", exited 4/5/9, and the very next
        # default-Batch leg archived the next world with no [BLOCK] and no
        # [RECONCILE] line. Unlike T3 this needs no crash: it happens on the
        # documented loop, every time. Nothing in cases 10/11/17/20/25/26
        # reaches a verdict through the verify path, because every
        # reply-contradicting behaviour the fake had also dropped the
        # connection -- which is why three review rounds missed it.
        for behaviour, want_rc, needle in (
                ("ghost_both_ok", EXIT_STOP_BOTH, "BOTH"),
                ("ghost_none_ok", EXIT_STOP_NEITHER, "NEITHER"),
                ("ok_then_blind", EXIT_UNKNOWN_OBSERVATION, "could not be observed"),
                ("ok_no_move", EXIT_SERVER_FAIL, "not_moved")):
            card = Card(saves=[entry("Keeper"), entry("Target"), entry("Next")])
            mdir = tmp / ("man33_" + behaviour)

            def base33(s33, batch):
                return ["-WiiHost", "127.0.0.1", "-Port", str(s33.port),
                        "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
                        "-ManifestDir", str(mdir), "-Batch", batch,
                        "-Keep", "Keeper"]

            # leg 1: the reply says PASS and the card contradicts it.
            srv = fresh(card=card, move_behavior=behaviour)
            p = run_ps(ps, repo, "archive_wii_saves.ps1",
                       base33(srv, "20260921-000331") + ["-Name", "Target", "-Execute"]
                       + lock_args("c151a" + behaviour))
            srv.stop()
            expect(p.returncode == want_rc,
                   behaviour + ": the verify STOP must exit " + str(want_rc)
                   + ", got " + str(p.returncode), p)
            expect(needle in p.stdout,
                   behaviour + ": the STOP must name the condition: " + p.stdout, p)

            # The halt must be DURABLE, and written BEFORE the result that
            # closes the intent -- the same ordering T3 established.
            mp = mdir / "manifest_20260921-000331.jsonl"
            ops = [r["op"] for r in read_jsonl(mp)]
            expect("block" in ops,
                   behaviour + ": a durable halt record must be written: " + str(ops), p)
            expect(ops.index("block") < ops.index("result"),
                   behaviour + ": the halt must be flushed BEFORE the result that "
                   "closes the intent: " + str(ops), p)

            # leg 2, the NEXT world, exactly as a 646-leg campaign does.
            srv = fresh(card=card)
            p = run_ps(ps, repo, "archive_wii_saves.ps1",
                       base33(srv, "20260921-000332") + ["-Name", "Next", "-Execute"]
                       + lock_args("c151b" + behaviour))
            srv.stop()
            expect(p.returncode == EXIT_REFUSED,
                   behaviour + ": the next leg must BLOCK, got " + str(p.returncode), p)
            expect(srv.moves == 0,
                   behaviour + ": the next leg must move NOTHING", p)
            expect("BLOCKED" in p.stdout,
                   behaviour + ": and must SAY the campaign is halted: " + p.stdout, p)
            names = [e["name"] for e in card.roots["saves"]]
            expect("Next" in names,
                   behaviour + ": the next world is still on the card: " + str(names), p)

            # ...and the halt survives being carried from disk a second time.
            srv = fresh(card=card)
            p = run_ps(ps, repo, "archive_wii_saves.ps1",
                       base33(srv, "20260921-000333") + ["-Name", "Next", "-Execute"]
                       + lock_args("c151c" + behaviour))
            srv.stop()
            expect(p.returncode == EXIT_REFUSED,
                   behaviour + ": and again, indefinitely, got " + str(p.returncode), p)

            # The BACKSTOP, on the verify path this time: delete the halt
            # LINE and the RESULT alone must still halt the campaign. Without
            # this leg the explicit record would mask the allow-list
            # entirely -- which is exactly how mutation m6 (drop the verify
            # path's own verdict from the allow-list) survived both suites.
            kept = [ln for ln in mp.read_text(encoding="utf-8").splitlines()
                    if ln.strip() and json.loads(ln).get("op") != "block"]
            mp.write_text("".join(x + "\n" for x in kept), encoding="utf-8")
            expect(not any(r["op"] == "block" for r in read_jsonl(mp)),
                   behaviour + ": the halt line is gone for this leg", p)
            srv = fresh(card=card)
            p = run_ps(ps, repo, "archive_wii_saves.ps1",
                       base33(srv, "20260921-000334") + ["-Name", "Next", "-Execute"]
                       + lock_args("c151f" + behaviour))
            srv.stop()
            expect(p.returncode == EXIT_REFUSED,
                   behaviour + ": with the halt LINE lost, the result alone must "
                   "still halt the campaign, got " + str(p.returncode), p)
            expect(srv.moves == 0,
                   behaviour + ": and still move nothing with only the result", p)

            # C15-1(b): for every STOP verdict the undo path must never say
            # "nothing to undo" -- it must OBSERVE and report. (not_moved is
            # excluded on purpose: that world is still in the saves root, so
            # there genuinely is nothing for the UNDO path to do; the
            # archive tool's halt is what makes the operator deal with it.)
            if behaviour != "ok_no_move":
                srv = fresh(card=card)
                p = run_ps(ps, repo, "restore_wii_saves.ps1", [
                    "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
                    "-ConnectTimeoutSeconds", "10", "-Manifest", str(mp)]
                    + lock_args("c151d" + behaviour))
                srv.stop()
                # The REPORT that must never appear -- not the phrase, which
                # the refusal itself quotes ("This is NOT 'nothing to
                # undo'").
                expect("records no world as moved into the archive" not in p.stdout,
                       behaviour + ": the world's position is unsettled; the undo "
                       "path must never report nothing to undo: " + p.stdout, p)
                expect("UNCERTAIN" in p.stdout.upper(),
                       behaviour + ": and must report it as archived-uncertain: "
                       + p.stdout, p)

            if behaviour == "ok_then_blind":
                # The strongest form: the world is in the archive with a
                # manifest that recorded only a FAIL, and the undo must put
                # it back by OBSERVATION.
                srv = fresh(card=card)
                p = run_ps(ps, repo, "restore_wii_saves.ps1", [
                    "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
                    "-ConnectTimeoutSeconds", "10", "-Manifest", str(mp),
                    "-Execute"] + lock_args("c151e"))
                srv.stop()
                names = [e["name"] for e in card.roots["saves"]]
                expect("Target" in names,
                       "the displaced world must come back: " + str(names), p)

        # ---- 34. C15-3: -ExpectWorldName had no ambiguity rule, so on the
        # REAL card -- ~644 "New World <n>" directories whose level.dat all
        # say "New World" -- the name the operator reads off the console
        # resolved to row 1 and was written into root_binding.json as the
        # binding's permanent evidence. Compare -Keep (N12) and -Name, both
        # of which STOP on a multi-row match.
        big = ([entry("New World %d" % i) for i in range(1, 645)]
               + [entry("Keeper A"), entry("Keeper B")])
        big_names = {("New World %d" % i): "New World" for i in range(1, 645)}
        big_names["Keeper A"] = "My Survival World"
        big_names["Keeper B"] = "Keeper B"
        keep34 = tmp / "keep34.txt"
        keep34.write_text("Keeper A\nKeeper B\n", encoding="utf-8")

        def bind34(s34, mdir34, extra, tag):
            return run_ps(ps, repo, "archive_wii_saves.ps1", [
                "-WiiHost", "127.0.0.1", "-Port", str(s34.port),
                "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "100",
                "-ManifestDir", str(mdir34), "-KeepFile", str(keep34),
                "-Name", "New World 1"] + extra + lock_args(tag))

        srv = fresh(card=Card(saves=big), paths_worlds="saves", level_names=big_names)
        p = bind34(srv, tmp / "man34a",
                   ["-ExpectWorldName", "New World", "-ExpectNameMaxMetaReads", "16",
                    "-Execute"], "c153a")
        srv.stop()
        expect(p.returncode == EXIT_ROOT_AMBIGUOUS,
               "a -ExpectWorldName that 644 worlds answer to must STOP, got "
               + str(p.returncode), p)
        expect("more than one" in p.stdout,
               "and must say it is ambiguous: " + p.stdout, p)
        expect("New World 1" in p.stdout and "New World 2" in p.stdout,
               "naming the first few rows that answered: " + p.stdout, p)
        expect(srv.moves == 0, "and nothing moved", p)

        # The control: a name that matches EXACTLY one row binds -- and by
        # directory name it costs no metadata read at all, from any row.
        srv = fresh(card=Card(saves=big), paths_worlds="saves", level_names=big_names)
        p = bind34(srv, tmp / "man34b",
                   ["-ExpectWorldName", "Keeper A", "-Execute"], "c153b")
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "an unambiguous -ExpectWorldName must bind, got " + str(p.returncode), p)
        expect("by directory name" in p.stdout,
               "resolved the free way: " + p.stdout, p)

        # Two rows answering one name by DIRECTORY name is the same
        # ambiguity (impossible on FAT, reachable from a torn listing).
        dup = Card(saves=[entry("Dup"), entry("DUP"), entry("Keeper A"),
                          entry("Keeper B"), entry("W1")])
        srv = fresh(card=dup, paths_worlds="saves")
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-ExpectWorldCountAtLeast", "4",
            "-ManifestDir", str(tmp / "man34c"), "-KeepFile", str(keep34),
            "-Name", "W1", "-ExpectWorldName", "Dup", "-Execute"]
            + lock_args("c153c"))
        srv.stop()
        expect(p.returncode == EXIT_ROOT_AMBIGUOUS,
               "a -ExpectWorldName matching two directory rows must STOP, got "
               + str(p.returncode), p)

        # ---- 35. C15-4: deleting the manifest directory used to orphan
        # everything already in the archive -- restore needs the manifest by
        # path and has no other way to learn what a batch holds, and the
        # runbook sent the operator into that very directory to delete a
        # file. The archive directory itself is a record, and the LIST verb
        # already reads it.
        card = Card(saves=[entry("Keeper"), entry("W1"), entry("W2")])
        mdir = tmp / "man35"
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
            "-ManifestDir", str(mdir), "-Batch", "20260921-000351",
            "-Name", "W1", "-Keep", "Keeper", "-Execute"] + lock_args("c154a"))
        srv.stop()
        expect(p.returncode == EXIT_OK, "the archive leg succeeds: " + str(p.returncode), p)
        expect("W1" in [e["name"] for e in card.archive["20260921-000351"]],
               "the world is in the archive", p)

        # The operator loses the manifest directory.
        for f in mdir.glob("*"):
            f.unlink()

        srv = fresh(card=card)
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10",
            "-FromArchiveBatch", "20260921-000351", "-ManifestDir", str(mdir)]
            + lock_args("c154b"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "restore must rebuild the batch from the ARCHIVE listing, got "
               + str(p.returncode), p)
        expect("manifest lost" in p.stdout.lower(),
               "and mark every entry as reconstructed: " + p.stdout, p)
        expect("W1" in p.stdout, "naming the world it found: " + p.stdout, p)

        srv = fresh(card=card)
        p = run_ps(ps, repo, "restore_wii_saves.ps1", [
            "-WiiHost", "127.0.0.1", "-Port", str(srv.port),
            "-ConnectTimeoutSeconds", "10",
            "-FromArchiveBatch", "20260921-000351", "-ManifestDir", str(mdir),
            "-Execute"] + lock_args("c154c"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "and must move it back, got " + str(p.returncode), p)
        names = [e["name"] for e in card.roots["saves"]]
        expect("W1" in names, "the world came back with no manifest: " + str(names), p)

        # ---- 36. C15-5: a leg that leaves a halt open must not report
        # success. Re-issuing ONE blocked world is the documented path and
        # it correctly clears only that world's block -- but the leg then
        # exited 0 with another world's halt still on disk, so a runner
        # keying off the exit code saw a clean success.
        # TWO halts cannot be produced by running two legs: the first one's
        # block refuses the second world before it is ever asked. They
        # coexist only when the RECONCILE pass finds two unfinished intents
        # at once -- two legs whose replies were lost before either was
        # reconciled -- so that is the fixture, planted directly.
        card = Card(saves=[entry("Keeper"), entry("B1"), entry("B2"), entry("W1")])
        mdir = tmp / "man36"
        mdir.mkdir(parents=True)
        for i, w in ((1, "B1"), (2, "B2")):
            batch = "20260921-00036%d" % i
            (mdir / ("manifest_%s.jsonl" % batch)).write_text(
                "".join(json.dumps(r) + "\n" for r in [
                    {"v": 1, "op": "batch_begin", "batch": batch, "root": "saves",
                     "root_abs": "sd:/saves", "card_identity": "2026-09-21T12:00:00.0000000Z"},
                    {"v": 1, "op": "intent", "seq": 1, "dir": "archive",
                     "root": "saves", "batch": batch, "name_hex": hex_of(w),
                     "from": "sd:/saves/" + w,
                     "to": "sd:/cw_saves_archive/%s/%s" % (batch, w),
                     "utc": "2026-09-21T1%d:00:00.0000000Z" % i}]),
                encoding="utf-8")

        def base36(s36, batch):
            return ["-WiiHost", "127.0.0.1", "-Port", str(s36.port),
                    "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
                    "-ManifestDir", str(mdir), "-Batch", batch, "-Keep", "Keeper"]

        # One leg turns both unfinished intents into durable halts.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base36(srv, "20260921-000363") + ["-Name", "W1", "-Execute"]
                   + lock_args("c155b"))
        srv.stop()
        expect(p.returncode == EXIT_REFUSED,
               "two halts are open, so the leg must refuse: " + str(p.returncode), p)
        expect("B1" in p.stdout and "B2" in p.stdout,
               "and both must be named: " + p.stdout, p)
        expect(srv.moves == 0, "and nothing moved", p)

        # Re-issue ONE of them. It moves, its own halt clears -- and the
        # OTHER is still open, so this leg is NOT a success.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base36(srv, "20260921-000364") + ["-Name", "B1", "-Execute"]
                   + lock_args("c155c"))
        srv.stop()
        expect(srv.moves == 1, "the re-issued world DID move", p)
        expect(p.returncode != EXIT_OK,
               "a leg that leaves another halt open must not exit 0, got "
               + str(p.returncode), p)
        expect("still open" in p.stdout.lower(),
               "and must say what is still halted: " + p.stdout, p)
        expect("B2" in p.stdout, "naming it: " + p.stdout, p)

        # ...and once the LAST halt is resolved the campaign is healthy
        # again, so the exit code goes back to 0.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base36(srv, "20260921-000365") + ["-Name", "B2", "-Execute"]
                   + lock_args("c155d"))
        srv.stop()
        expect(srv.moves == 1, "the second re-issue moved too", p)
        expect(p.returncode == EXIT_OK,
               "with nothing left halted the leg is a success again, got "
               + str(p.returncode), p)

        # ---- 37. C15-2: the reviewer's construction, end to end. An
        # explicit pairing addressed a block by (manifest path, seq) and
        # never consulted the order -- but `-Batch` defaults to a fresh id
        # every leg and seq RESTARTS at 1 in each manifest, so (manifest, 1)
        # is a predictable coordinate, not a unique one. A single clear
        # planted with a FUTURE manifest's coordinates therefore disarmed
        # the block when it arrived, reproducing T1's exact signature:
        # holds for the leg that derives it in-run, then goes quiet on the
        # leg that carries it from disk.
        card = Card(saves=[entry("Keeper"), entry("W2"), entry("W3"), entry("W4")])
        mdir = tmp / "man37"
        mdir.mkdir(parents=True)
        target_manifest = mdir / "manifest_20260921-000999.jsonl"
        (mdir / "manifest_20260921-000001.jsonl").write_text(
            json.dumps({"v": 1, "op": "block_cleared", "name_hex": hex_of("W2"),
                        "block_manifest": str(target_manifest), "block_seq": 1,
                        "reason": "operator_override",
                        "utc": "2026-09-21T06:00:00.0000000Z"}) + "\n",
            encoding="utf-8")

        def base37(s37):
            return ["-WiiHost", "127.0.0.1", "-Port", str(s37.port),
                    "-ConnectTimeoutSeconds", "10", "-SkipRootBinding",
                    "-ManifestDir", str(mdir), "-Batch", "20260921-000999",
                    "-Keep", "Keeper"]

        srv = fresh(card=card, move_behavior="drop_reply_no_move")
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base37(srv) + ["-Name", "W2", "-Execute"] + lock_args("c152a"))
        srv.stop()
        expect(p.returncode != EXIT_OK, "a dropped reply is not a success", p)

        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base37(srv) + ["-Name", "W3", "-Execute"] + lock_args("c152b"))
        srv.stop()
        expect(p.returncode == EXIT_REFUSED,
               "the block derived in-run holds, got " + str(p.returncode), p)
        recs = read_jsonl(target_manifest)
        expect(any(r["op"] == "block" and r.get("seq") == 1 for r in recs),
               "the block landed at exactly the coordinate the planted clear "
               "names: " + str([(r["op"], r.get("seq")) for r in recs]), p)

        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base37(srv) + ["-Name", "W3", "-Execute"] + lock_args("c152c"))
        srv.stop()
        expect(p.returncode == EXIT_REFUSED,
               "a PRE-DATED clear naming a block that did not exist yet must "
               "not disarm it when it arrives, got " + str(p.returncode), p)
        expect(srv.moves == 0, "and nothing may move", p)
        names = [e["name"] for e in card.roots["saves"]]
        expect("W3" in names, "W3 is still on the card: " + str(names), p)

        # The tool's OWN clear still works, from the same coordinate,
        # because it carries the block's utc and so names the instance.
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base37(srv) + ["-Name", "W2", "-ResolveBlock", "W2", "-Execute"]
                   + lock_args("c152d"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "-ResolveBlock must still clear it, got " + str(p.returncode), p)
        srv = fresh(card=card)
        p = run_ps(ps, repo, "archive_wii_saves.ps1",
                   base37(srv) + ["-Name", "W3", "-Execute"] + lock_args("c152e"))
        srv.stop()
        expect(p.returncode == EXIT_OK,
               "and the campaign then resumes, got " + str(p.returncode), p)
        expect(srv.moves == 1, "moving exactly one world", p)

    print("PASS test_stage_saves_tools (test_guard_refuses_unrecorded_batch_world, test_guard_refuses_missing_unrestored_world, test_guard_refuses_different_card_identity, test_guard_reconciled_success_line)")
    return 0


def read_jsonl(path: Path) -> list[dict]:
    out = []
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue  # the expected trailing partial line
    return out


if __name__ == "__main__":
    sys.exit(main())
