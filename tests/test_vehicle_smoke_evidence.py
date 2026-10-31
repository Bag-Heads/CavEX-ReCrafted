#!/usr/bin/env python3
"""CONTRACT-v1.2 synthetic unit tapes, NOT collector qualification/calibration.

These are independent hand-authored wire fixtures. Astra step 11 still requires
verbatim authentic Wii calibration and independent live-parser negative replay.
No synthetic GREEN output here is a hardware PASS or producer faithfulness proof.
"""
import copy
import contextlib
import hashlib
import io
import importlib.util
import json
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("vehicle_collector", ROOT / "tools/wii_telemetry_server.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
PROFILE = "host_vehicle_smoke_8m"
KINDS = ("boat", "pig", "cart")


class Tape:
    def __init__(self, kind):
        self.kind, self.phase = kind, "p0"
        self.rows, self.ms, self.active, self.serial, self.op = [], 0, 1, 0, 0
        self.world = dict(slot=0, generation=1, dimension=0)
        self.epoch, self.generation = 1, "1"

    def a(self, guest=False):
        return dict(slot=int(guest), generation=self.generation if guest else "0",
                    wire_id=(7 if self.generation == "1" else 8) if guest else 0,
                    input_epoch=0 if guest else self.epoch)

    def target(self, kind=None, world=None):
        return dict(id=KINDS.index(kind or self.kind) + 1, kind=kind or self.kind,
                    world=copy.deepcopy(world or self.world))

    def s(self, kind=None, mounted=False, guest=False, world=None, health=160):
        return dict(actor=self.a(guest), world=copy.deepcopy(world or self.world), pos=[0., 65., 0.],
                    health=health, ready=True, confirmed=True, mount_count=int(mounted),
                    riding_boat=mounted and (kind or self.kind) == "boat",
                    riding_cart=mounted and (kind or self.kind) == "cart", steer=[0., 0.],
                    inv_hash=200 if guest else 100, inv_used=2 if guest else 1)

    def c(self, kind=None, mounted=False, guest=False, request=0, world=None):
        return dict(present=True, actor=self.a(guest), bound=copy.deepcopy(world or self.world),
                    pos=[0., 65., 0.], ready=True, mount_count=int(mounted), ride_pending=False,
                    ride_target=self.target(kind)["id"], ride_input_epoch=self.a(guest)["input_epoch"],
                    ride_request_seq=request, ride_desired_occupied=mounted, ride_send_dirty=False,
                    ride_exit_lift_pending=False, ride_exit_repin_eligible=False, steer=[0., 0.],
                    inv_hash=200 if guest else 100, inv_used=2 if guest else 1)

    def v(self, kind=None, occupied=False, world=None, control=0):
        k = kind or self.kind
        return dict(target=self.target(k, world), alive=True, pos=[0., 64., 0.], occupied=occupied,
                    rider=self.a(), ride_epoch=9 if k == "cart" and occupied else 0,
                    control_seq=control if k == "cart" else 0, positive_steps=control if k == "cart" else 0,
                    negative_steps=0, pending_throttle=0, saddled=k == "pig", steer=[0., 0.])

    def vc(self, kind=None, occupied=False, world=None, control=0):
        k = kind or self.kind
        out = dict(target=self.target(k, world), alive=True, pos=[0., 64., 0.])
        if k == "pig":
            out.update(pig_ridden=occupied, pig_rider_id=0, saddled=True)
        else:
            out.update(occupied=occupied, occupant_id=0)
            if k == "boat":
                out.update(steer=[0., 0.])
            else:
                out.update(cart_type=0, ride_epoch=9 if occupied else 0, control_ready=occupied,
                           control_dirty=False, control_seq=control, positive_steps=control,
                           negative_steps=0, pending_throttle=0, replay_throttle=0)
        return out

    def next_op(self):
        self.op += 1
        return self.op

    def next_serial(self):
        self.serial += 1
        return str(self.serial)

    def emit(self, name, op=0, origin="production", **payload):
        self.ms += 20
        row = dict(schema=1, run_id="synthetic_" + self.kind, build_id="test_10.04", profile=PROFILE,
                   kind=self.kind, seq=len(self.rows)+1, phase=self.phase, op_id=op, ms=self.ms,
                   server_update=self.active + 100, active_update=self.active,
                   event="vehicle_smoke." + name, origin=origin, ok=True, reason="", **payload)
        self.rows.append(copy.deepcopy(row))
        return row

    def phase_begin(self, i):
        self.phase = "p" + str(i)
        self.emit("phase_begin", phase_index=i, pending_markers=0, open_ops=0)

    def phase_end(self, i):
        self.emit("phase_end", phase_index=i, pending_markers=0, open_ops=0)

    def ui(self, action, op, player=0, screen="none"):
        if screen != "none":
            self.ms += 1600
        return self.emit("ui_action", op, "synthetic", action=action, player=player, screen=screen,
                         button="IB_GUI_CLICK" if screen != "none" else "none", held_ms=1200,
                         edge_seq=op, delivered=True, input_epoch=self.a(player == 1)["input_epoch"])

    def isolation(self, op, stage, kind=None, mounted=False, guest=None):
        for owner in ("server", "client"):
            server = owner == "server"
            payload = dict(stage=stage, owner=owner, attempt_op=op,
                           host=self.s(kind, mounted) if server else self.c(kind, mounted),
                           guest=guest[owner] if guest else (self.s(guest=True) if server else self.c(guest=True)),
                           vehicle=self.v(kind, mounted) if server else self.vc(kind, mounted),
                           guest_pose_delta_max_m=0., guest_follow_count=0, unexpected_attach_count=0)
            if not server:
                payload["unexpected_route_count"] = 0
            self.emit("isolation", op, **payload)

    def attempt(self, op, kind, action="mount", guest=False, channel="host_buttons", occupied=True):
        button = ("none" if channel == "owned_rpc" else
                  "IB_JUMP" if action == "dismount" and kind == "cart" else "IB_ACTION2")
        return self.emit("action_attempt", op, "synthetic", action=action, channel=channel,
                         source=self.a(guest), target=self.target(kind), button=button, edge_seq=op,
                         held_ms=20, request_seq=op if kind == "cart" else 0, desired_occupied=occupied,
                         reach_m=2., los=True, camera_hit=self.target(kind)["id"])

    def result(self, a, site, status="applied", occupied=True, serial=None):
        k = a["target"]["kind"]
        payload = dict(action=a["action"], source=a["source"], target=a["target"], site=site,
                       status=status, request_seq=a["request_seq"], ride_epoch=9 if k == "cart" and occupied else 0)
        if site == "authority":
            payload.update(serial=serial or self.next_serial(), host=self.s(k, occupied), vehicle=self.v(k, occupied))
        elif site == "client":
            payload.update(host=self.c(k, occupied, request=a["request_seq"]), vehicle=self.vc(k, occupied))
        else:
            payload.update(rpc={"guest_cart_intent": "MINECART_RIDE_INTENT", "guest_cart_control": "MINECART_CONTROL"}.get(a["action"], "ENTITY_USE"))
        return self.emit("action_result", a["op_id"], **payload)

    def positive(self, kind, action="mount", old=None):
        op = self.next_op()
        a = self.attempt(op, kind, action)
        release = None
        if old:
            release = self.next_serial()
            self.emit("dismount", op, host=self.s(), old=self.v(old), serial=release)
        s = self.result(a, "authority")
        self.result(a, "client")
        if old:
            self.emit("switch", op, **{"from": old, "to": kind}, host=self.s(kind, True),
                      old=self.v(old), new=self.v(kind, True), release_serial=release, mount_serial=s["serial"])
        return a

    def ride(self, a, count=100):
        self.active += count
        k = a["target"]["kind"]
        for stage in ("server", "client"):
            self.emit("ride_sample", a["op_id"], stage=stage, target=self.target(k), window_id=a["op_id"],
                      samples=count, eligible_updates=count, tick_calls=count, missing_calls=0, duplicate_calls=0,
                      displacement_m=.5, host=self.s(k, True) if stage == "server" else self.c(k, True, request=a["request_seq"]),
                      vehicle=self.v(k, True, control=1) if stage == "server" else self.vc(k, True, control=1),
                      seat_error_max_m=0., follow_count=count, request_seq=a["request_seq"])

    def negatives(self, kind, occupied):
        actions = ("guest_ui", "guest_use", "guest_cart_intent", "guest_cart_control") if kind == "cart" else ("guest_ui", "guest_use")
        for action in actions:
            op = self.next_op()
            self.isolation(op, "before", kind, occupied)
            a = self.attempt(op, kind, action, True, "guest_frame" if action == "guest_ui" else "owned_rpc")
            if action == "guest_ui" and kind == "cart":
                self.result(a, "client", "client_noop", occupied)
            else:
                self.result(a, "ingress", "refused" if action.startswith("guest_cart") else "sent", occupied)
                if not action.startswith("guest_cart"):
                    self.result(a, "authority", "refused", occupied)
            self.active += 10
            self.isolation(op, "after", kind, occupied)

    def build(self):
        self.emit("run_start", origin="fixture", seed="12345", difficulty=0, game_mode="survival",
                  superflat=True, split=True, world_payloads=True, world="cw_vehicle_rig",
                  transfer_mode="fixture_request", deadline_ms=480000)
        self.phase_begin(0)
        self.emit("rig_ready", self.next_op(), "fixture", fixture_hash=300,
                  targets=[self.target(k) for k in KINDS], host=self.s(), guest=self.s(guest=True),
                  water_ok=True, support_ok=True, rails_ok=True, ray_ok=True, saddle_consumed=True, pig_ai_updates=1)
        self.phase_end(0)
        self.phase_begin(1)
        for k in KINDS:
            self.negatives(k, False)
        a = self.positive(self.kind)
        self.ride(a)
        self.negatives(self.kind, True)
        op = self.next_op()
        a = self.attempt(op, self.kind, "dismount", occupied=False)
        release_serial = self.next_serial()
        self.emit("dismount", op, host=self.s(), old=self.v(), serial=release_serial)
        self.result(a, "authority", occupied=False, serial=release_serial)
        self.result(a, "client", occupied=False)
        self.positive(self.kind)
        sequences = {"boat": ("boat", "pig", "cart", "boat"), "pig": ("pig", "boat", "cart", "pig"),
                     "cart": ("cart", "boat", "pig", "cart")}
        for old, k in zip(sequences[self.kind], sequences[self.kind][1:]):
            a = self.positive(k, "switch", old)
            self.ride(a, 100 if k == self.kind else 1)
            if k != self.kind:
                self.negatives(k, True)
        self.phase_end(1)
        self.phase_begin(2)
        op = self.next_op()
        self.isolation(op, "before", mounted=True)
        self.ui("save_exit", op, screen="pause")
        for stage in ("begin", "durable"):
            self.emit("checkpoint", op, stage=stage, checkpoint_id=1, host=self.s(mounted=True), guest=self.s(guest=True),
                      vehicle=self.v(occupied=True), pose_bits=[0, 1115815936, 0], fixture_hash=300,
                      save_ok=stage == "durable", level_ok=stage == "durable", chunks_ok=stage == "durable", regions_synced=stage == "durable")
        self.emit("world_reset", op, world_reset_count=1, local_entity=0, checkpoint_id=1, source_world=self.world, world_loaded=False)
        self.emit("title", op, world_loaded=False, world_reset_count=1, checkpoint_id=1, screen="title", autoload_consumed=False)
        self.world["generation"] = 2
        self.epoch = 2
        base = dict(world_name="cw_vehicle_rig", reload_count=1, checkpoint_id=1)
        self.emit("reload", op, stage="disk", **base, world=self.world, pose_bits=[0, 1115815936, 0], disk_reopen=True,
                  player_read_ok=True, fallback_used=False, override_used=False)
        self.emit("reload", op, stage="inventory", **base, world=self.world, read_ok=True, inv_hash=100, inv_used=1)
        self.emit("reload", op, stage="entities", **base, host=self.s(), guest=self.s(guest=True), fixture_hash=300,
                  saddle=True, targets=[self.target(k) for k in KINDS], ambiguous_matches=0)
        self.emit("reload", op, stage="client_ready", **base, host=self.c(), guest=self.c(guest=True), input_ack_epoch=self.epoch)
        self.positive(self.kind)
        self.isolation(op, "after", mounted=True)
        self.phase_end(2)
        self.phase_begin(3)
        op = self.next_op()
        self.isolation(op, "before", mounted=True)
        self.emit("death", op, "fixture", stage="server", before_host=self.s(mounted=True), host=self.s(mounted=True, health=0),
                  vehicle=self.v(occupied=True), cause="fixture_lethal", deaths_before=0, deaths_after=1)
        self.emit("death", op, stage="ui", host=self.c(mounted=True), screen="death")
        self.ui("respawn", op, screen="death")
        for stage in ("released", "requested", "ack", "fresh"):
            host = self.s(health=160 if stage == "fresh" else 0)
            host["ready"] = stage == "fresh"
            self.emit("respawn", op, stage=stage, old_input_epoch=2, new_input_epoch=2,
                      respawn_epoch="0" if stage == "released" else "4294967301",
                      host=host, old=self.v(), serial=self.next_serial(),
                      health=160 if stage == "fresh" else 0, ack=stage == "ack")
        self.emit("respawn", op, stage="client_ready", old_input_epoch=2, new_input_epoch=2,
                  respawn_epoch="4294967301", host=self.c(), ready=True)
        self.positive(self.kind)
        # Unrelated guest keeps its original epoch through host respawn.
        guests = dict(server=self.s(guest=True), client=self.c(guest=True))
        self.isolation(op, "after", mounted=True, guest=guests)
        self.phase_end(3)
        self.phase_begin(4)
        transfer_op = self.next_op()
        guests = dict(server=self.s(guest=True), client=self.c(guest=True))
        self.isolation(transfer_op, "before", mounted=True, guest=guests)
        self.ui("guest_leave", self.next_op(), 1)
        self.generation = "2"
        self.ui("guest_join", self.next_op(), 1)
        pending_op = self.next_op()
        self.ui("accept", pending_op, 1)
        q = dict(ticket="99", generation="2", world=copy.deepcopy(self.world))
        state = dict(tuple=q, paused=False, loading=False, transfer_open=False, refusal_armed=True, pending=True)
        self.emit("pending", pending_op, "synthetic", stage="armed", cause="synthetic_admission", result="none", serial=self.next_serial(), **state)
        self.emit("pause", pending_op, serial=self.next_serial(), **{k: v for k, v in dict(state, paused=True).items() if k != "transfer_open"})
        self.ms += 2000
        self.emit("pending", pending_op, stage="retained", cause="global_pause", result="none", serial=self.next_serial(), **dict(state, paused=True))
        self.emit("resume", pending_op, serial=self.next_serial(), **{k: v for k, v in state.items() if k != "transfer_open"})
        src = copy.deepcopy(self.world)
        dst = dict(slot=1, generation=1, dimension=-1)
        actor = self.a()
        self.emit("transfer_begin", transfer_op, "fixture", transfer_id=1, actor=actor, src=src, dst=dst,
                  vehicle=self.v(occupied=True), mode="fixture_request", state="requested", serial=self.next_serial())
        self.emit("admission_clear", pending_op, "synthetic", tuple=q, refusal_armed=False, pending=True,
                  transfer_open=True, transfer_id=1, loading=True, serial=self.next_serial())
        self.emit("pending", pending_op, stage="retained", cause="transfer", result="none", serial=self.next_serial(),
                  **dict(state, refusal_armed=False, transfer_open=True, loading=True))
        self.emit("transfer_release", transfer_op, transfer_id=1, actor=actor, src=src, dst=dst, host=self.s(), old=self.v(), release_serial=self.next_serial())
        self.world = dst
        self.emit("transfer_commit", transfer_op, transfer_id=1, actor=actor, src=src, dst=dst, host=self.s(),
                  source_vehicle=self.v(world=src), destination_mount_count=0, commit_serial=self.next_serial(), transition_epoch=1, state="committed")
        self.active += 1
        self.emit("barrier_release", pending_op, tuple=q, transfer_id=1, pending=True, paused=False,
                  transfer_open=False, loading=True, host_ack_pending=True, serial=self.next_serial())
        self.emit("installed", pending_op, tuple=q, installed_count=1, result="installed", pending=False,
                  serial=self.next_serial(), paused=False, loading=True, transfer_open=False)
        self.ui("confirm", pending_op, 1)
        guests = dict(server=self.s(guest=True, world=src), client=self.c(guest=True, world=src))
        self.ui("gameplay", pending_op, 1)
        self.emit("gameplay_ready", pending_op, stage="server", tuple=q, guest=guests["server"], confirmed=True, ready_count=1, installed_count=1)
        self.emit("gameplay_ready", pending_op, stage="client", tuple=q, guest=guests["client"], confirmed=True, ready_count=1, installed_count=1)
        self.epoch = 3
        for stage in ("server", "client"):
            payload = dict(source_vehicle=self.v(world=src), destination_mount_count=0) if stage == "server" else dict(host=self.c())
            self.emit("transfer_ack", transfer_op, stage=stage, transfer_id=1, actor=self.a(), dst=dst,
                      input_ack_epoch=self.epoch, ready=True, **payload)
        guests = dict(server=self.s(guest=True, world=src), client=self.c(guest=True, world=src))
        self.isolation(transfer_op, "after", guest=guests)
        self.phase_end(4)
        self.phase_begin(5)
        final_op = self.next_op()
        for stage in ("server", "client"):
            self.emit("ride_sample", final_op, stage=stage, target=self.target(world=src), window_id=100,
                      samples=10, eligible_updates=10, tick_calls=10, missing_calls=0, duplicate_calls=0, displacement_m=0.,
                      host=self.s() if stage == "server" else self.c(), vehicle=self.v(world=src) if stage == "server" else self.vc(world=src),
                      seat_error_max_m=0., follow_count=0, request_seq=0)
        self.phase_end(5)
        self.phase = "done"
        self.emit("run_ok", completed_phases=6, pending_markers=0, open_ops=0, overflow_count=0, hook_mask=0)
        self.rows.append(dict(event="profile_done", run_id="synthetic_" + self.kind, status="ok"))
        return self.rows


def grade(rows, kind="boat", transfer_mode="fixture_request"):
    e = mod.VehicleSmokeEvidence("synthetic_" + kind, kind, "test_10.04", transfer_mode)
    for row in rows:
        if row.get("event") == "profile_done":
            e.note_profile_done(row.get("status"))
        else:
            e.note(row)
    return e


def resequence(rows):
    seq = 0
    for row in rows:
        if row.get("event", "").startswith("vehicle_smoke."):
            seq += 1
            row["seq"] = seq
    return rows


def rig_op(rows):
    return next(r["op_id"] for r in rows if r["event"] == "vehicle_smoke.rig_ready")


def make_manifest(root):
    manifest = dict(contract="CONTRACT-v1.2", build_id="test_10.04", release_sha256="1"*64,
                    source_sha256="2"*64, collector_sha256=hashlib.sha256((ROOT/"tools/wii_telemetry_server.py").read_bytes()).hexdigest(), kinds={})
    for kind in KINDS:
        config = root / (kind + ".json")
        config.write_text(json.dumps(dict(profile=PROFILE, kind=kind, seed=12345)), encoding="utf-8")
        manifest["kinds"][kind] = dict(transfer_mode="fixture_request", geometry_proven=True,
            config_path=str(config), config_sha256=hashlib.sha256(config.read_bytes()).hexdigest())
    # Deliberately synthetic report: exercises structural proof gates only.
    # It does not supply authentic producer coverage or certify any execution.
    proof = dict(contract="CONTRACT-v1.2", provenance="production_real_handler_tests", reviewed=True,
                 status="PASS", source_sha256=manifest["source_sha256"], kinds={})
    for kind in KINDS:
        proof["kinds"][kind] = dict(cases={case: True for case in mod.VS_ACK_PROOF_CASES},
            first_accepted_ack=dict(pending_epoch=3, ack_epoch=3, retry=False, accepted=True,
                                    accepted_index=1, emitted_count=1))
    proof_path = root / "ack-handler-proof.json"
    proof_path.write_text(json.dumps(proof), encoding="utf-8")
    manifest["transfer_ack_proof"] = dict(path=str(proof_path), sha256=hashlib.sha256(proof_path.read_bytes()).hexdigest())
    path = root / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def pc_fixture(root, name, raw=None, manifest=None):
    source = root/(name + "-source")
    source.mkdir()
    if raw is None:
        raw = b"".join((json.dumps(row, separators=(",", ":"))+"\n").encode("utf-8") for row in Tape("boat").build())
    (source/"telemetry.ndjson").write_bytes(raw)
    args = mod._selftest_args(root/name, "synthetic_boat", PROFILE,
        expected_build="test_10.04", cw_vehicle_kind="boat", vehicle_pc_replay=True,
        replay=str(source), profile_override=PROFILE, vehicle_manifest=str(manifest or ""),
        vehicle_replay_out=str(root/name))
    return args, source, raw


def heartbeat(**changes):
    row = dict(event="heartbeat", run_id="synthetic_boat", profile=PROFILE,
               build_id="test_10.04", tick=1, fps=60, oom=0, tel_fail=0, tel_drop=0)
    row.update(changes)
    return row


class EvidenceTests(unittest.TestCase):
    def assert_live_rejected(self, rows, kind, reason, transfer_mode="fixture_request"):
        self.assertIn(reason, grade(rows, kind, transfer_mode).complete_error() or "GREEN")
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            c = mod.Collector(mod._selftest_args(Path(td), "synthetic_" + kind, PROFILE,
                expected_build="test_10.04", cw_vehicle_kind=kind, vehicle_transfer_mode=transfer_mode))
            for row in rows:
                if mod.ingest_telemetry_line(c, json.dumps(row, separators=(",", ":"))):
                    break
            self.assertIn(reason, c.fail_reason or "GREEN")

    def test_green(self):
        for kind in KINDS:
            rows = Tape(kind).build()
            evidence = grade(rows, kind)
            self.assertIsNone(evidence.complete_error(), (kind, evidence.complete_error()))
            self.assertEqual(evidence.summary()["certification"], "calibration_required")
            self.assertEqual(evidence.summary()["contract"], "CONTRACT-v1.2")

    def test_client_dispatch_ticks_are_independent_of_follow(self):
        for kind in KINDS:
            for ticks, missing, duplicate in ((99, 1, 0), (101, 0, 1), (100, 1, 1),
                                               (102, 0, 1), (100, 2, 1), (0, 100, 0)):
                with self.subTest(kind=kind, ticks=ticks, missing=missing, duplicate=duplicate):
                    rows = Tape(kind).build()
                    sample = next(r for r in rows if r["event"] == "vehicle_smoke.ride_sample"
                                  and r["phase"] == "p1" and r["stage"] == "client")
                    self.assertEqual(sample["follow_count"], 100)
                    sample.update(tick_calls=ticks, missing_calls=missing, duplicate_calls=duplicate)
                    self.assert_live_rejected(rows, kind, "vehicle_ride_tick_census")

    def test_transfer_ack_uses_post_commit_host_epoch_and_transfer_identity(self):
        for kind in KINDS:
            original = Tape(kind).build()
            commit = next(r for r in original if r["event"] == "vehicle_smoke.transfer_commit")
            self.assertEqual((commit["transition_epoch"], commit["host"]["actor"]["input_epoch"]), (1, 2))
            self.assertEqual([r["input_ack_epoch"] for r in original if r["event"] == "vehicle_smoke.transfer_ack"], [3, 3])
            self.assertIsNone(grade(original, kind).complete_error())
            for mutation in ("stale", "zero", "mismatched", "actor_epoch", "wrong_transfer", "wrong_op", "early", "early_capture", "zero_transfer_epoch"):
                with self.subTest(kind=kind, mutation=mutation):
                    rows = copy.deepcopy(original)
                    server, client = [r for r in rows if r["event"] == "vehicle_smoke.transfer_ack"]
                    commit = next(r for r in rows if r["event"] == "vehicle_smoke.transfer_commit")
                    reason = "vehicle_transfer_ack"
                    if mutation in ("stale", "zero"):
                        epoch = 2 if mutation == "stale" else 0
                        for r in (server, client):
                            r["input_ack_epoch"] = r["actor"]["input_epoch"] = epoch
                        client["host"]["actor"]["input_epoch"] = epoch
                        if mutation == "zero":
                            reason = "vehicle_schema: record.input_ack_epoch"
                    elif mutation == "mismatched":
                        client["input_ack_epoch"] = 4
                    elif mutation == "actor_epoch":
                        server["actor"]["input_epoch"] = 2
                    elif mutation == "wrong_transfer":
                        server["transfer_id"] += 1
                        reason = "vehicle_transfer_identity"
                    elif mutation == "wrong_op":
                        client["op_id"] = rig_op(rows)
                        reason = "vehicle_transfer_identity"
                    elif mutation == "early":
                        rows.remove(server)
                        rows.insert(rows.index(commit), server)
                        resequence(rows)
                        reason = "vehicle_transfer_observation_order"
                    elif mutation == "early_capture":
                        server["ms"] = commit["ms"] - 1
                        reason = "vehicle_transfer_observation_order"
                    else:
                        commit["transition_epoch"] = 0
                        reason = "vehicle_transfer_commit_epoch"
                    self.assert_live_rejected(rows, kind, reason)

    def test_pc_replay_waives_only_absent_heartbeat_floor_and_cannot_certify(self):
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            root = Path(td)
            manifest = make_manifest(root)
            def collect(name, pc=True, beats=0, rows=None, manifest_path=manifest):
                source = root/(name + "-source")
                source.mkdir()
                input_rows = [dict(event="heartbeat", run_id="synthetic_boat", profile=PROFILE,
                    build_id="test_10.04", tick=i+1, fps=60, oom=0, tel_fail=0, tel_drop=0) for i in range(beats)]
                input_rows += rows if rows is not None else Tape("boat").build()
                lines = [json.dumps(row, separators=(",", ":"))+"\n" for row in input_rows]
                (source/"telemetry.ndjson").write_bytes("".join(lines).encode("utf-8"))
                c = mod.Collector(mod._selftest_args(root/name, "synthetic_boat", PROFILE,
                    expected_build="test_10.04", cw_vehicle_kind="boat", vehicle_pc_replay=pc,
                    replay=str(source), vehicle_manifest=str(manifest_path) if manifest_path else ""))
                for line in lines:
                    if mod.ingest_telemetry_line(c, line):
                        break
                if not c.fail_reason:
                    c._finish_normal("synthetic PC replay")
                return c
            self.assertEqual(collect("missing-manifest", manifest_path=None).fail_reason, "vehicle_manifest_required")
            self.assertEqual(collect("missing-calibration").fail_reason, "vehicle_calibration_required")
            # Mock only external calibration provenance to isolate the floor;
            # the same manifest/schema/claim/parser gates continue to run.
            with patch.object(mod, "vehicle_calibration_error", return_value=None):
                strict = collect("strict", pc=False)
                self.assertEqual(strict.failure_class, "too_few_heartbeats")
                pc = collect("pc")
                self.assertIsNone(pc.fail_reason)
                self.assertEqual(len(pc.not_graded), 1)
                self.assertIn("heartbeat floor", pc.not_graded[0])
                self.assertEqual(pc.write_result(), 0)
                artifact_path = root/"pc/PC_REPLAY.json"
                artifact = json.loads(artifact_path.read_text())
                self.assertEqual(artifact["status"], "PC_REPLAY_PASS")
                self.assertEqual(artifact["vehicle_certification"], "pc_replay_noncertifying")
                self.assertFalse((root/"pc/PASS.json").exists())
                with self.assertRaisesRegex(mod.VehicleSmokeError, "vehicle_packet_pc_replay"):
                    mod.vehicle_validate_packet([artifact_path], str(manifest), "boat", "test_10.04")
                artifact["status"] = "PASS"
                artifact_path.write_text(json.dumps(artifact))
                with self.assertRaisesRegex(mod.VehicleSmokeError, "vehicle_packet_pc_replay"):
                    mod.vehicle_validate_packet([artifact_path], str(manifest), "boat", "test_10.04")
                self.assertEqual(collect("present-beat", beats=1).failure_class, "too_few_heartbeats")
                fault = dict(event="heartbeat", run_id="synthetic_boat", profile=PROFILE,
                             build_id="test_10.04", tick=1, fps=60, oom=1, tel_fail=0, tel_drop=0)
                self.assertEqual(collect("heartbeat-fault", rows=[fault]+Tape("boat").build()).fail_reason,
                                 "vehicle_heartbeat_fault")
                broken = Tape("boat").build()
                broken = resequence([r for r in broken if not (r["event"] == "vehicle_smoke.transfer_ack" and r["stage"] == "client")])
                self.assertIn("vehicle_missing_or_extra", collect("missing-ack", rows=broken).fail_reason)
                broken = Tape("boat").build()
                broken[1]["seq"] += 1
                self.assertEqual(collect("bad-sequence", rows=broken).fail_reason, "vehicle_seq_gap")
                src = root/"source"
                src.mkdir()
                tape = src/"telemetry.ndjson"
                tape.write_text("".join(json.dumps(r, separators=(",", ":"))+"\n" for r in Tape("boat").build()), encoding="utf-8")
                digest = hashlib.sha256(tape.read_bytes()).hexdigest()
                args = mod._selftest_args(root, "synthetic_boat", PROFILE, replay=str(src), profile_override="",
                    expected_build="test_10.04", cw_vehicle_kind="boat", vehicle_manifest=str(manifest),
                    vehicle_pc_replay=True, vehicle_replay_out=str(root/"pc-regraded"))
                self.assertEqual(mod.replay_run_dir(args), 0)
                self.assertTrue((root/"pc-regraded/PC_REPLAY.json").is_file())
                self.assertFalse((root/"pc-regraded/PASS.json").exists())
                self.assertEqual(hashlib.sha256(tape.read_bytes()).hexdigest(), digest)
                self.assertEqual(mod.replay_run_dir(args), 2)  # no clobber
            with self.assertRaisesRegex(ValueError, "offline"):
                mod.Collector(mod._selftest_args(root/"live", "r", PROFILE, vehicle_pc_replay=True))
            with self.assertRaisesRegex(ValueError, "offline"):
                mod.Collector(mod._selftest_args(root/"other", "r", "far_idle_2m", replay="source", vehicle_pc_replay=True))
            cli = subprocess.run([sys.executable, str(ROOT/"tools/wii_telemetry_server.py"), "--vehicle-pc-replay"],
                                 text=True, capture_output=True)
            self.assertEqual(cli.returncode, 2)
            self.assertIn("requires --replay", cli.stderr)

    def test_transfer_ack_handler_proof_is_mandatory_exact_first_and_source_bound(self):
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            root = Path(td)
            manifest_path = make_manifest(root)
            manifest = json.loads(manifest_path.read_text())
            proof_path = Path(manifest["transfer_ack_proof"]["path"])
            original = json.loads(proof_path.read_text())
            def check(report, expected):
                proof_path.write_text(json.dumps(report), encoding="utf-8")
                manifest["transfer_ack_proof"]["sha256"] = hashlib.sha256(proof_path.read_bytes()).hexdigest()
                self.assertEqual(mod.vehicle_transfer_ack_proof_error(manifest, root), expected)
            self.assertIsNone(mod.vehicle_transfer_ack_proof_error(manifest, root))
            without = copy.deepcopy(manifest)
            without.pop("transfer_ack_proof")
            self.assertEqual(mod.vehicle_transfer_ack_proof_error(without, root), "vehicle_transfer_ack_handler_proof_required")
            for kind in KINDS:
                for case in mod.VS_ACK_PROOF_CASES:
                    for change in ("missing", "false"):
                        with self.subTest(kind=kind, case=case, change=change):
                            report = copy.deepcopy(original)
                            if change == "missing":
                                report["kinds"][kind]["cases"].pop(case)
                            else:
                                report["kinds"][kind]["cases"][case] = False
                            check(report, "vehicle_transfer_ack_handler_proof_coverage")
                for key, value in (("ack_epoch", 4), ("pending_epoch", 0), ("pending_epoch", True),
                                   ("retry", True), ("accepted", False), ("accepted_index", 2),
                                   ("accepted_index", True), ("emitted_count", 0), ("emitted_count", 2)):
                    report = copy.deepcopy(original)
                    report["kinds"][kind]["first_accepted_ack"][key] = value
                    check(report, "vehicle_transfer_ack_handler_proof_exact_first")
            for key, value in (("contract", "CONTRACT-v1.1"), ("reviewed", False), ("status", "FAIL"),
                               ("provenance", "snapshots"), ("source_sha256", "9"*64)):
                report = copy.deepcopy(original)
                report[key] = value
                check(report, "vehicle_transfer_ack_handler_proof_provenance")
            report = copy.deepcopy(original)
            report["kinds"].pop("cart")
            check(report, "vehicle_transfer_ack_handler_proof_kinds")
            check(original, None)
            manifest["transfer_ack_proof"]["sha256"] = "0"*64
            self.assertEqual(mod.vehicle_transfer_ack_proof_error(manifest, root), "vehicle_transfer_ack_handler_proof_required")
            # A valid larger/equal pair in a complete tape still cannot PASS
            # without reviewed production-handler coverage, even with calibration.
            manifest.pop("transfer_ack_proof")
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            args, source, raw = pc_fixture(root, "no-handler-proof", manifest=manifest_path)
            with patch.object(mod, "vehicle_calibration_error", return_value=None):
                self.assertEqual(mod.replay_run_dir(args), 1)
            result = json.loads((Path(args.vehicle_replay_out)/"PC_REPLAY.json").read_text())
            self.assertEqual(result["reason"], "vehicle_transfer_ack_handler_proof_required")

    def test_pc_replay_rejects_filter_truncate_repair_synthesize_merge(self):
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            root = Path(td)
            manifest = make_manifest(root)
            for operation in ("filter", "truncate", "repair", "synthesize", "merge"):
                with self.subTest(operation=operation):
                    hb = (json.dumps(heartbeat())+"\n").encode()
                    body = b"".join((json.dumps(r)+"\n").encode() for r in Tape("boat").build())
                    raw = hb + body
                    if operation == "repair":
                        raw = b'{"event":"heartbeat",BAD}\n' + body
                    args, source, raw = pc_fixture(root, operation, raw, manifest)
                    c = mod.Collector(args)
                    original_lines = [line.decode() for line in mod.vehicle_pc_tape_lines(raw)]
                    if operation == "filter":
                        supplied = original_lines[1:]  # drop a present heartbeat
                    elif operation == "truncate":
                        supplied = original_lines[:-1]  # remove completion
                    elif operation == "repair":
                        supplied = [hb.decode()] + original_lines[1:]
                    elif operation == "synthesize":
                        supplied = [json.dumps(dict(event="heartbeat", run_id="synthetic_boat", profile=PROFILE,
                            build_id="test_10.04", tick=2, fps=60))+"\n"] + original_lines
                    else:
                        supplied = original_lines[:1] + [json.dumps(Tape("pig").build()[0])+"\n"] + original_lines[1:]
                    for line in supplied:
                        if mod.ingest_telemetry_line(c, line):
                            break
                    if not c.fail_reason:
                        c._finish_normal("forbidden transformed evidence")
                    self.assertIn("vehicle_pc_replay_evidence_changed", c.fail_reason)
                    self.assertEqual(c.write_result(), 1)
                    out = Path(args.out)
                    self.assertEqual((out/"source-tape.ndjson").read_bytes(), raw)
                    self.assertEqual((source/"telemetry.ndjson").read_bytes(), raw)
                    self.assertFalse((out/"PASS.json").exists())
                    self.assertFalse((out/"FAIL.json").exists())
            args, source, raw = pc_fixture(root, "direct-synthesis", manifest=manifest)
            c = mod.Collector(args)
            c.process(heartbeat())  # bypassing the verbatim parser is forbidden
            self.assertEqual(c.fail_reason, "vehicle_pc_replay_requires_verbatim_parser")
            args, source, raw = pc_fixture(root, "changed-source", manifest=manifest)
            c = mod.Collector(args)
            for line in mod.vehicle_pc_tape_lines(raw):
                self.assertFalse(mod.ingest_telemetry_line(c, line.decode()))
            (source/"telemetry.ndjson").write_bytes(raw+b"\n")
            with patch.object(mod, "vehicle_calibration_error", return_value=None):
                c._finish_normal("source changed after reading")
            self.assertEqual(c.fail_reason, "vehicle_pc_replay_evidence_changed_or_incomplete")

    def test_pc_replay_malformed_heartbeats_fail_and_preserve_full_raw_source(self):
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            root = Path(td)
            manifest = make_manifest(root)
            bad = []
            for key in ("tick", "fps"):
                hb = heartbeat()
                hb.pop(key)
                bad.append((json.dumps(hb)+"\n").encode())
            for key, value in (("event", "Heartbeat"), ("event", "heartbeatt"), ("tick", True), ("tick", -1), ("tick", 1.0), ("tick", "1"),
                               ("fps", None), ("fps", True), ("fps", "60"), ("fps", -1),
                               ("oom", False), ("oom", -1), ("tel_drop", "0"), ("tel_fail", [])):
                hb = heartbeat()
                hb[key] = value
                bad.append((json.dumps(hb)+"\n").encode())
            bad += [b'{"event":"heartbeat",BAD}\n',
                    b'{"event":"heartbeat","tick":1,"tick":2}\n',
                    b'{"event":"heartbeat","fps":NaN}\n', b'{"event":"heartbeat",\xff}\n',
                    json.dumps(heartbeat()).encode()]  # unterminated; never repaired
            body = b"".join((json.dumps(r)+"\n").encode() for r in Tape("boat").build())
            for i, line in enumerate(bad):
                # Includes malformed heartbeat after successful profile_done;
                # completion cannot truncate this suffix away.
                raw = body + line
                args, source, raw = pc_fixture(root, "bad-hb-"+str(i), raw, manifest)
                sentinel = {"PASS.json": b"original pass", "FAIL.json": b"original failure"}
                for name, content in sentinel.items():
                    (source/name).write_bytes(content)
                with self.subTest(i=i), patch.object(mod, "vehicle_calibration_error", return_value=None):
                    self.assertEqual(mod.replay_run_dir(args), 1)
                out = Path(args.vehicle_replay_out)
                result = json.loads((out/"PC_REPLAY.json").read_text())
                self.assertEqual(result["status"], "PC_REPLAY_FAIL")
                self.assertTrue("vehicle_heartbeat_schema" in result["reason"] or "vehicle_malformed_line" in result["reason"])
                self.assertEqual(result["contract"], "CONTRACT-v1.2")
                self.assertTrue(result["noncertifying"])
                self.assertEqual(result["source_tape_sha256"], hashlib.sha256(raw).hexdigest())
                self.assertEqual((out/"source-tape.ndjson").read_bytes(), raw)
                self.assertEqual((source/"telemetry.ndjson").read_bytes(), raw)
                for name, content in sentinel.items():
                    self.assertEqual((source/name).read_bytes(), content)
                    self.assertFalse((out/name).exists())

    def test_pc_replay_never_substitutes_filtered_tapes_or_falls_back_automatically(self):
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            root = Path(td)
            manifest = make_manifest(root)
            body = b"".join((json.dumps(r)+"\r\n").encode() for r in Tape("boat").build())
            args, source, raw = pc_fixture(root, "canonical", (json.dumps(heartbeat())+"\r\n").encode()+body, manifest)
            (source/"vehicle-wire.ndjson").write_bytes(body)  # filtered auxiliary
            with patch.object(mod, "vehicle_calibration_error", return_value=None):
                self.assertEqual(mod.replay_run_dir(args), 1)
            result = json.loads((Path(args.vehicle_replay_out)/"PC_REPLAY.json").read_text())
            self.assertEqual(result["heartbeats"], 1)
            self.assertEqual(result["failure_class"], "too_few_heartbeats")
            self.assertFalse(result["not_graded"])
            self.assertEqual((Path(args.vehicle_replay_out)/"source-tape.ndjson").read_bytes(), raw)
            args.vehicle_replay_out = str(root/"no-canonical")
            (source/"telemetry.ndjson").unlink()  # fixture removes only its input
            self.assertEqual(mod.replay_run_dir(args), 2)
            self.assertFalse(Path(args.vehicle_replay_out).exists())
            args, source, raw = pc_fixture(root, "strict-no-fallback", body, manifest)
            (source/"vehicle-wire.ndjson").write_bytes(body)
            args.vehicle_pc_replay = False
            with patch.object(mod, "vehicle_calibration_error", return_value=None):
                self.assertEqual(mod.replay_run_dir(args), 1)
            out = Path(args.vehicle_replay_out)
            self.assertFalse((out/"PC_REPLAY.json").exists())
            self.assertEqual(json.loads((out/"FAIL.json").read_text())["failure_class"], "too_few_heartbeats")

    def test_pc_replay_artifacts_rejected_regardless_of_name_and_partial_relabeling(self):
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            root = Path(td)
            manifest = make_manifest(root)
            args, source, raw = pc_fixture(root, "pc-provenance", manifest=manifest)
            with patch.object(mod, "vehicle_calibration_error", return_value=None):
                self.assertEqual(mod.replay_run_dir(args), 0)
                out = Path(args.vehicle_replay_out)
                original = json.loads((out/"PC_REPLAY.json").read_text())
                markers = ("vehicle_pc_replay", "status", "vehicle_certification",
                           "certification_scope", "evidence_provenance", "source_tape_sha256", "source_tape", "noncertifying")
                for marker in markers:
                    artifact = dict(status="PASS", profile=PROFILE)
                    artifact[marker] = original[marker]
                    for name in ("PASS.json", "FAIL.json", "renamed-result.json"):
                        path = out/name
                        path.write_text(json.dumps(artifact), encoding="utf-8")
                        for kind in ("boat", ""):
                            with self.subTest(marker=marker, name=name, aggregate=not kind):
                                with self.assertRaisesRegex(mod.VehicleSmokeError, "vehicle_packet_pc_replay"):
                                    mod.vehicle_validate_packet([path], str(manifest), kind, "test_10.04")
            # No mocked authentication: missing authentic Wii provenance still
            # disqualifies a relabeled/renamed diagnostic packet.
            with self.assertRaisesRegex(mod.VehicleSmokeError, "vehicle_calibration_required"):
                mod.vehicle_validate_packet([out/"renamed-result.json"], str(manifest), "boat", "test_10.04")

    def test_pc_replay_live_device_staging_other_profiles_and_certification_are_refused(self):
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            root = Path(td)
            args, source, raw = pc_fixture(root, "refused-mode")
            changes = [("profile_override", "far_idle_2m"), ("replay", ""), ("tcp_connect_host", "forbidden-device"),
                       ("vehicle_dol", "forbidden.dol"), ("vehicle_check_manifest", True),
                       ("vehicle_validate_pass", ["PASS.json"]), ("vehicle_aggregate", True), ("selftest", True),
                       ("release_gate", True), ("require_tnt_evidence", True), ("require_wii_budget", True)]
            for key, value in changes:
                changed = copy.copy(args)
                setattr(changed, key, value)
                with self.subTest(key=key), patch.object(mod.socket, "socket") as udp, patch.object(mod.socket, "create_connection") as tcp:
                    self.assertEqual(mod.replay_run_dir(changed), 2)
                    udp.assert_not_called()
                    tcp.assert_not_called()
                    self.assertFalse(Path(args.out).exists())
            for entry in (mod.run, mod.run_tcp_pull):
                with patch.object(mod.socket, "socket") as udp, patch.object(mod.socket, "create_connection") as tcp:
                    with self.assertRaisesRegex(ValueError, "forbids live/device"):
                        entry(args)
                    udp.assert_not_called()
                    tcp.assert_not_called()
            for cli in (["--tcp-connect-host", "forbidden-device"], ["--vehicle-dol", "forbidden.dol"],
                        ["--vehicle-aggregate"], ["--vehicle-check-manifest"], ["--vehicle-validate-pass", "PASS.json"],
                        ["--selftest"], ["--release-gate"], ["--require-tnt-evidence"], ["--require-wii-budget"],
                        ["--profile-override", "far_idle_2m"]):
                result = subprocess.run([sys.executable, str(ROOT/"tools/wii_telemetry_server.py"),
                    "--vehicle-pc-replay", "--replay", str(source), "--profile", PROFILE,
                    "--vehicle-replay-out", str(root/"cli-output")] + cli, text=True, capture_output=True)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertFalse((root/"cli-output").exists())
            self.assertEqual((source/"telemetry.ndjson").read_bytes(), raw)

    def test_pc_replay_cannot_create_replace_delete_certification_artifacts(self):
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            root = Path(td)
            args, source, raw = pc_fixture(root, "isolated")
            for name in ("PASS.json", "FAIL.json", "PC_REPLAY.json"):
                (source/name).write_bytes(("untouched source " + name).encode())
            source_before = {p.name: p.read_bytes() for p in source.iterdir()}
            for target in (source, source/"nested", root/"existing"):
                if target == root/"existing":
                    target.mkdir()
                    (target/"PASS.json").write_bytes(b"original outside pass")
                    (target/"FAIL.json").write_bytes(b"original outside fail")
                before = {p.name: p.read_bytes() for p in target.iterdir()} if target.exists() else {}
                changed = copy.copy(args)
                changed.out = changed.vehicle_replay_out = str(target)
                self.assertEqual(mod.replay_run_dir(changed), 2)
                with self.assertRaisesRegex(ValueError, "new isolated"):
                    mod.Collector(changed)
                if target.exists():
                    self.assertEqual({p.name:p.read_bytes() for p in target.iterdir()}, before)
                else:
                    self.assertFalse(target.exists())
            for name in ("PASS.json", "FAIL.json", "PC_REPLAY.json"):
                race_args, race_source, race_raw = pc_fixture(root, "race-"+name)
                c = mod.Collector(race_args)
                sentinel = Path(c.out)/name
                sentinel.write_bytes(b"must not replace or delete")
                with self.assertRaisesRegex(ValueError, "isolation violated"):
                    c.write_result()
                self.assertEqual(sentinel.read_bytes(), b"must not replace or delete")
                for other in ("PASS.json", "FAIL.json", "PC_REPLAY.json"):
                    if other != name:
                        self.assertFalse((c.out/other).exists())
            self.assertEqual({p.name:p.read_bytes() for p in source.iterdir()}, source_before)

    def test_native_staging_contract_guard_and_validator_replay_provenance(self):
        powershell = shutil.which("powershell") or shutil.which("pwsh")
        if not powershell:
            self.skipTest("native PowerShell is unavailable")
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cases = []
            markers = dict(vehicle_pc_replay=True, status="PC_REPLAY_PASS",
                           evidence_provenance="pc_diagnostic_replay",
                           vehicle_certification="pc_replay_noncertifying",
                           certification_scope="vehicle_smoke_pc_replay_noncertifying", source_tape_sha256="0"*64,
                           source_tape="source-tape.ndjson", noncertifying=True)
            for i, (key, value) in enumerate(markers.items()):
                for profile in (PROFILE, "far_idle_2m"):
                    for name in ("PASS.json", "renamed.json"):
                        out = root/(str(i)+profile+name)
                        out.mkdir()
                        artifact = dict(status="PASS", profile=profile)
                        artifact[key] = value
                        path = out/name
                        path.write_text(json.dumps(artifact), encoding="utf-8")
                        cases.append(str(path))
            case_file = root/"cases.json"
            case_file.write_text(json.dumps(cases), encoding="utf-8")
            script = root/"native-guards.ps1"
            script.write_text(r"""param([string]$Stage,[string]$Validator,[string]$Cases)
$ErrorActionPreference='Stop'
$tokens=$null; $parseErrors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile($Stage,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count) { throw 'stage parse failed' }
$comparison=$ast.Find({ param($n) $n -is [System.Management.Automation.Language.BinaryExpressionAst] -and $n.Left.Extent.Text -eq '$vehiclePacket.contract' },$true)
if($null -eq $comparison) { throw 'contract comparison missing' }
foreach($version in @('CONTRACT-v1','CONTRACT-v1.1','CONTRACT-v1.2','CONTRACT-v1.2-proposal')) {
    $vehiclePacket=@{contract=$version}
    $rejected=& ([scriptblock]::Create($comparison.Extent.Text))
    if($rejected -ne ($version -ne 'CONTRACT-v1.2')) { throw "stage accepted/rejected wrong version: $version" }
}
$ast=[System.Management.Automation.Language.Parser]::ParseFile($Validator,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count) { throw 'validator parse failed' }
$function=$ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Assert-WiiPassArtifact' },$true)
Invoke-Expression $function.Extent.Text
foreach($path in (Get-Content -Raw -LiteralPath $Cases | ConvertFrom-Json)) {
    $rejected=$false
    try { Assert-WiiPassArtifact -Path $path | Out-Null } catch {
        if($_.Exception.Message -like '*PC replay provenance/status*') { $rejected=$true } else { throw }
    }
    if(-not $rejected) { throw "validator admitted replay provenance: $path" }
}
Write-Output 'PASS native v1.2-only stage guard and renamed/relabelled replay rejection'
""", encoding="utf-8")
            result = subprocess.run([powershell,"-NoProfile","-ExecutionPolicy","Bypass","-File",str(script),
                str(ROOT/"tools/stage_wii_dol.ps1"),str(ROOT/"tools/validate_wii_pass.ps1"),str(case_file)],
                text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("PASS native v1.2-only",result.stdout)

    def test_progress_pause_hold_requires_real_retained_observation(self):
        for kind in KINDS:
            original = Tape(kind).build()
            self.assertIsNone(grade(original, kind).complete_error())
            for mutation in ("missing", "blocked", "wrong_cause", "not_paused", "not_pending",
                             "refusal_cleared", "installed_result", "counter_advance", "short_hold", "wrong_tuple"):
                with self.subTest(kind=kind, mutation=mutation):
                    rows = copy.deepcopy(original)
                    hold = next(r for r in rows if r["event"] == "vehicle_smoke.pending" and r["cause"] == "global_pause")
                    reason = "vehicle_pause_retention"
                    if mutation == "missing":
                        rows.remove(hold)
                        resequence(rows)
                        reason = "vehicle_missing_or_extra: pending"
                    elif mutation == "blocked":
                        hold["stage"] = "blocked"
                        reason = "vehicle_missing_or_extra: pending"
                    elif mutation == "wrong_cause":
                        hold["cause"] = "loading"
                        reason = "vehicle_missing_or_extra: pending"
                    elif mutation == "not_paused":
                        hold["paused"] = False
                    elif mutation == "not_pending":
                        hold["pending"] = False
                    elif mutation == "refusal_cleared":
                        hold["refusal_armed"] = False
                    elif mutation == "installed_result":
                        hold["result"] = "installed"
                    elif mutation == "counter_advance":
                        hold["active_update"] += 1
                    elif mutation == "short_hold":
                        resume = next(r for r in rows if r["event"] == "vehicle_smoke.resume")
                        pause = next(r for r in rows if r["event"] == "vehicle_smoke.pause")
                        resume["ms"] = pause["ms"] + 1999
                    else:
                        hold["tuple"] = copy.deepcopy(hold["tuple"])
                        hold["tuple"]["ticket"] = "98"
                        reason = "vehicle_pending_tuple"
                    self.assert_live_rejected(rows, kind, reason)

    def test_progress_gameplay_request_precedes_server_and_client_readiness(self):
        for kind in KINDS:
            original = Tape(kind).build()
            self.assertIsNone(grade(original, kind).complete_error())
            for mutation in ("request_missing", "request_not_delivered", "server_before_request",
                             "request_before_confirm", "client_before_server", "client_before_request",
                             "wrong_ready_tuple", "wrong_ready_world", "late_ready",
                             "ready_before_barrier"):
                with self.subTest(kind=kind, mutation=mutation):
                    rows = copy.deepcopy(original)
                    confirm = next(r for r in rows if r["event"] == "vehicle_smoke.ui_action" and r["action"] == "confirm")
                    request = next(r for r in rows if r["event"] == "vehicle_smoke.ui_action" and r["action"] == "gameplay")
                    server, client = [r for r in rows if r["event"] == "vehicle_smoke.gameplay_ready"]
                    reason = "vehicle_progress_delivery_order"
                    if mutation == "request_missing":
                        rows.remove(request)
                        resequence(rows)
                        reason = "vehicle_missing_or_extra: ui_action"
                    elif mutation == "request_not_delivered":
                        request["delivered"] = False
                        reason = "vehicle_ui_delivery: gameplay"
                    elif mutation == "server_before_request":
                        rows.remove(server)
                        rows.insert(rows.index(request), server)
                        resequence(rows)
                    elif mutation == "request_before_confirm":
                        # Keep capture time valid so this reaches the delivery
                        # order predicate rather than the separate clock gate.
                        request["ms"] = confirm["ms"]
                        rows.remove(request)
                        rows.insert(rows.index(confirm), request)
                        resequence(rows)
                        reason = "vehicle_readmission_order"
                    elif mutation in ("client_before_server", "client_before_request"):
                        rows.remove(client)
                        rows.insert(rows.index(server if mutation == "client_before_server" else request), client)
                        resequence(rows)
                    elif mutation == "wrong_ready_tuple":
                        client["tuple"] = copy.deepcopy(client["tuple"])
                        client["tuple"]["ticket"] = "98"
                        reason = "vehicle_pending_tuple"
                    elif mutation == "wrong_ready_world":
                        client["guest"]["bound"] = copy.deepcopy(next(r["dst"] for r in rows if r["event"] == "vehicle_smoke.transfer_begin"))
                        reason = "vehicle_progress_ready"
                    elif mutation == "ready_before_barrier":
                        # O26: order is kept -- a ready receipt counted
                        # before the release barrier's pass still FAILS.
                        barrier = next(r for r in rows if r["event"] == "vehicle_smoke.barrier_release")
                        client["active_update"] = barrier["active_update"] - 1
                        reason = "vehicle_progress_deadline"
                    else:
                        # O26 (O24 lead decision): the deadline is 15000 ms of
                        # wall time after the release barrier, not a pass
                        # count. Completion at +15001 ms still FAILS; every
                        # later row keeps its relative timing so no separate
                        # clock gate fires first.
                        barrier = next(r for r in rows if r["event"] == "vehicle_smoke.barrier_release")
                        delta = barrier["ms"] + 15001 - client["ms"]
                        for later in rows[rows.index(client):]:
                            if "ms" in later:
                                later["ms"] += delta
                        reason = "vehicle_progress_deadline"
                    self.assert_live_rejected(rows, kind, reason)

    def test_progress_deadline_is_wall_time_not_pass_count(self):
        # O26: a slow host (Wii loading screen ~45 passes/s) may spend more
        # than the old 200-pass cap inside the 15000 ms window; that is GREEN.
        for kind in KINDS:
            with self.subTest(kind=kind):
                rows = Tape(kind).build()
                barrier = next(r for r in rows if r["event"] == "vehicle_smoke.barrier_release")
                server, client = [r for r in rows if r["event"] == "vehicle_smoke.gameplay_ready"]
                installed = next(r for r in rows if r["event"] == "vehicle_smoke.installed")
                for row in (installed, server, client):
                    row["active_update"] = barrier["active_update"] + 201
                self.assertIsNone(grade(rows, kind).complete_error())
                with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
                    c = mod.Collector(mod._selftest_args(Path(td), "synthetic_" + kind, PROFILE,
                        expected_build="test_10.04", cw_vehicle_kind=kind))
                    for row in rows:
                        if mod.ingest_telemetry_line(c, json.dumps(row, separators=(",", ":"))):
                            break
                    self.assertIsNone(c.fail_reason)

    def test_window_close_accumulates_counts_maxima_and_sticky_faults(self):
        for kind in KINDS:
            rows = Tape(kind).build()
            sample = next(r for r in rows if r["event"] == "vehicle_smoke.ride_sample"
                          and r["phase"] == "p1" and r["stage"] == "client")
            # The settled pose is still correct: a previous seat error remains
            # in the accumulated maximum at close, rather than disappearing.
            sample["seat_error_max_m"] = .002
            self.assert_live_rejected(rows, kind, "vehicle_ride_follow")
            for owner in ("server", "client"):
                rows = Tape(kind).build()
                after = next(r for r in rows if r["event"] == "vehicle_smoke.isolation"
                             and r["stage"] == "after" and r["owner"] == owner)
                after["guest"]["pos"][0] = .04
                self.assert_live_rejected(rows, kind, "vehicle_isolation_accumulation")
                after["guest_pose_delta_max_m"] = .04
                self.assertIsNone(grade(rows, kind).complete_error())
                after["guest"]["pos"][0] = 0.
                after["guest_pose_delta_max_m"] = .06  # excursion then return
                self.assert_live_rejected(rows, kind, "vehicle_isolation_violation")
            for field, reason in (("unexpected_route_count", "vehicle_unexpected_route"),
                                  ("unexpected_attach_count", "vehicle_isolation_violation")):
                rows = Tape(kind).build()
                index = next(i for i, r in enumerate(rows) if r["event"] == "vehicle_smoke.isolation"
                             and r["stage"] == "after" and r["owner"] == "client")
                rows[index][field] = 1
                repaired = copy.deepcopy(rows[index])
                repaired[field] = 0
                rows.insert(index + 1, repaired)
                self.assert_live_rejected(resequence(rows), kind, reason)
            rows = Tape(kind).build()
            before = next(r for r in rows if r["event"] == "vehicle_smoke.isolation" and r["stage"] == "before")
            before["guest_pose_delta_max_m"] = .01
            self.assert_live_rejected(rows, kind, "vehicle_isolation_reset")
            for stage in ("server", "client"):
                rows = Tape(kind).build()
                index = next(i for i, r in enumerate(rows) if r["event"] == "vehicle_smoke.ride_sample"
                             and r["phase"] == "p1" and r["stage"] == stage)
                rows.insert(index + 1, copy.deepcopy(rows[index]))
                self.assert_live_rejected(resequence(rows), kind, "vehicle_ride_summary_count")

    def test_actual_actor_epochs_diverge_and_rejoin_is_a_new_lifetime(self):
        for kind in KINDS:
            rows = Tape(kind).build()
            rig = next(r for r in rows if r["event"] == "vehicle_smoke.rig_ready")
            self.assertEqual((rig["host"]["actor"]["input_epoch"], rig["guest"]["actor"]["input_epoch"]), (1, 0))
            ready = next(r for r in rows if r["event"] == "vehicle_smoke.gameplay_ready" and r["stage"] == "server")
            self.assertEqual(ready["guest"]["actor"]["input_epoch"], 0)
            self.assertNotEqual(ready["guest"]["actor"]["generation"], rig["guest"]["actor"]["generation"])
            self.assertNotEqual(ready["guest"]["actor"]["wire_id"], rig["guest"]["actor"]["wire_id"])
            self.assertIsNone(grade(rows, kind).complete_error())
            for name, key in (("rig_ready", "guest"), ("action_attempt", "source"), ("gameplay_ready", "guest")):
                broken = copy.deepcopy(rows)
                r = next(r for r in broken if r["event"] == "vehicle_smoke." + name
                         and (name != "action_attempt" or r["source"]["slot"] == 1))
                actor = r[key] if key == "source" else r[key]["actor"]
                actor["input_epoch"] = 1
                self.assert_live_rejected(broken, kind, "vehicle_guest_input_epoch")
            broken = copy.deepcopy(rows)
            next(r for r in broken if r["event"] == "vehicle_smoke.ui_action" and r["player"] == 1)["input_epoch"] = 1
            self.assert_live_rejected(broken, kind, "vehicle_guest_input_epoch")
            for phase, reason in (("p1", "vehicle_ride_actor_epoch"), ("p5", "vehicle_final_actor_epoch")):
                broken = copy.deepcopy(rows)
                c = next(r for r in broken if r["event"] == "vehicle_smoke.ride_sample" and r["stage"] == "client" and r["phase"] == phase)
                c["host"]["actor"]["input_epoch"] += 1
                c["host"]["ride_input_epoch"] = c["host"]["actor"]["input_epoch"]
                self.assert_live_rejected(broken, kind, reason)
            broken = copy.deepcopy(rows)
            for r in broken:
                if r["event"] == "vehicle_smoke.gameplay_ready" and r["stage"] == "client":
                    r["guest"]["actor"]["wire_id"] += 1
                if r["event"] == "vehicle_smoke.isolation" and r["phase"] == "p4" and r["owner"] == "client" and r["stage"] == "after":
                    r["guest"]["actor"]["wire_id"] += 1
            self.assert_live_rejected(broken, kind, "vehicle_progress_guest_identity")
            broken = copy.deepcopy(rows)
            for r in broken:
                if r["event"] == "vehicle_smoke.gameplay_ready" and r["stage"] == "client":
                    r["guest"]["actor"]["generation"] = rig["guest"]["actor"]["generation"]
                if r["event"] == "vehicle_smoke.isolation" and r["phase"] == "p4" and r["owner"] == "client" and r["stage"] == "after":
                    r["guest"]["actor"]["generation"] = rig["guest"]["actor"]["generation"]
            self.assert_live_rejected(broken, kind, "vehicle_progress_ready")

    def test_unproven_contact_mode_remains_named_terminal_fail(self):
        for kind in KINDS:
            rows = Tape(kind).build()
            rows[0]["transfer_mode"] = "contact"
            failed = {key: rows[2][key] for key in mod.VS_ENVELOPE}
            failed.update(event="vehicle_smoke.fail", phase="failed", op_id=0,
                          origin="production", ok=False, code="transfer_contact_mode_unproven",
                          failed_phase="p0", overflow_count=0, hook_mask=0)
            rows = resequence(rows[:2] + [failed, rows[-1]])
            self.assert_live_rejected(rows, kind, "vehicle_producer_fail: transfer_contact_mode_unproven", "contact")

    def test_split_respawn_loading_ack_precedes_fresh_and_rejects_stale_ack(self):
        for kind in KINDS:
            rows = Tape(kind).build()
            stages = [r for r in rows if r["event"] == "vehicle_smoke.respawn"]
            self.assertEqual([r["stage"] for r in stages], ["released", "requested", "ack", "fresh", "client_ready"])
            self.assertFalse(stages[2]["host"]["ready"])
            self.assertEqual(stages[2]["health"], 0)
            self.assertGreater(int(stages[2]["respawn_epoch"]), 2**32)
            self.assertTrue(all(r["new_input_epoch"] == 2 for r in stages))
            self.assertIsNone(grade(rows, kind).complete_error())
            for field, value, reason in (("respawn_epoch", "4294967302", "vehicle_respawn_handshake_epoch"),
                                         ("respawn_epoch", "5", "vehicle_respawn_handshake_epoch"),
                                         ("old_input_epoch", 1, "vehicle_respawn_epoch"),
                                         ("new_input_epoch", 5, "vehicle_respawn_epoch"),
                                         ("ack", False, "vehicle_respawn_ack"),
                                         ("health", 160, "vehicle_respawn_health_snapshot"),
                                         ("op_id", rig_op(rows), "vehicle_respawn_operation")):
                broken = copy.deepcopy(rows)
                ack = next(r for r in broken if r["event"] == "vehicle_smoke.respawn" and r["stage"] == "ack")
                ack[field] = value
                self.assert_live_rejected(broken, kind, reason)
            for first, second in (("released", "requested"), ("ack", "fresh")):
                broken = copy.deepcopy(rows)
                i = next(i for i, r in enumerate(broken) if r["event"] == "vehicle_smoke.respawn" and r["stage"] == first)
                j = next(i for i, r in enumerate(broken) if r["event"] == "vehicle_smoke.respawn" and r["stage"] == second)
                broken[i], broken[j] = broken[j], broken[i]
                self.assert_live_rejected(resequence(broken), kind, "vehicle_respawn_order")
            broken = copy.deepcopy(rows)
            i = next(i for i, r in enumerate(broken) if r["event"] == "vehicle_smoke.ui_action" and r["action"] == "respawn")
            request = broken.pop(i)
            j = next(i for i, r in enumerate(broken) if r["event"] == "vehicle_smoke.respawn" and r["stage"] == "released")
            broken.insert(j + 1, request)
            self.assert_live_rejected(resequence(broken), kind, "vehicle_respawn_order")
            broken = copy.deepcopy(rows)
            next(r for r in broken if r["event"] == "vehicle_smoke.ui_action" and r["action"] == "respawn")["input_epoch"] = 999
            self.assert_live_rejected(broken, kind, "vehicle_respawn_epoch")

    def test_pre_mount_release_is_required_for_all_six_directed_switches(self):
        covered = set()
        for kind in KINDS:
            originals = Tape(kind).build()
            for switch in [r for r in originals if r["event"] == "vehicle_smoke.switch"]:
                covered.add((switch["from"], switch["to"]))
                for mutation in ("missing", "early", "late", "wrong_op", "wrong_serial", "wrong_old", "joint_wrong_old", "attached", "wrong_epoch", "retroactive"):
                    with self.subTest(kind=kind, pair=(switch["from"], switch["to"]), mutation=mutation):
                        rows = copy.deepcopy(originals)
                        sw = next(r for r in rows if r["event"] == "vehicle_smoke.switch" and r["op_id"] == switch["op_id"])
                        release = next(r for r in rows if r["event"] == "vehicle_smoke.dismount" and r["op_id"] == sw["op_id"])
                        reason = "vehicle_switch_release_serial"
                        if mutation == "missing":
                            rows.remove(release)
                        elif mutation == "early":
                            rows.remove(release)
                            index = next(i for i, r in enumerate(rows) if r.get("op_id") == sw["op_id"] and r["event"] == "vehicle_smoke.action_attempt")
                            rows.insert(index, release)
                            reason = "vehicle_switch_release_order"
                        elif mutation == "late":
                            rows.remove(release)
                            index = max(i for i, r in enumerate(rows) if r.get("op_id") == sw["op_id"] and r["event"] in ("vehicle_smoke.switch", "vehicle_smoke.action_result"))
                            rows.insert(index + 1, release)
                            reason = "vehicle_switch_release_order"
                        elif mutation == "wrong_op":
                            release["op_id"] = rig_op(rows)
                        elif mutation == "wrong_serial":
                            sw["release_serial"] = next(r["serial"] for r in rows if r["event"] == "vehicle_smoke.dismount" and r["op_id"] != sw["op_id"])
                        elif mutation == "wrong_old":
                            release["old"]["target"]["id"] += 100
                        elif mutation == "joint_wrong_old":
                            release["old"]["target"]["id"] += 100
                            sw["old"]["target"]["id"] += 100
                            reason = "vehicle_switch_old_relation"
                        elif mutation == "attached":
                            release["host"]["mount_count"] = 1
                        elif mutation == "wrong_epoch":
                            release["host"]["actor"]["input_epoch"] += 1
                            reason = "vehicle_switch_release_identity"
                        else:
                            release["serial"] = sw["release_serial"] = str(int(sw["mount_serial"]) + 100)
                            reason = "vehicle_switch_release"
                        self.assert_live_rejected(resequence(rows), kind, reason)
        self.assertEqual(covered, {(a, b) for a in KINDS for b in KINDS if a != b})

    def test_title_progress_uses_time_without_server_updates(self):
        for kind in KINDS:
            rows = Tape(kind).build()
            title = next(r for r in rows if r["event"] == "vehicle_smoke.title")
            reset = next(r for r in rows if r["event"] == "vehicle_smoke.world_reset")
            self.assertEqual((title["server_update"], title["active_update"]), (reset["server_update"], reset["active_update"]))
            for r in rows:
                if r.get("seq", 0) >= title["seq"]:
                    r["ms"] += 2000
            self.assertIsNone(grade(rows, kind).complete_error())
            title["ms"] = 1
            self.assert_live_rejected(rows, kind, "vehicle_main_time_order")

    def test_retry_dedupe_and_conflict(self):
        rows = Tape("boat").build()
        retry = copy.deepcopy(rows)
        retry.insert(5, copy.deepcopy(retry[4]))
        e = grade(retry)
        self.assertIsNone(e.complete_error())
        self.assertEqual(e.retries, 1)
        retry[5]["ms"] += 1
        self.assertEqual(grade(retry).complete_error(), "vehicle_conflicting_duplicate")

    def test_every_schema_field_required_and_typed(self):
        # Each distinct selected-stage/owner/kind schema, independently from
        # the collector's schema table. Recursively test every field, including
        # all envelope, authoritative, client and kind-specific nested leaves.
        seen = set()
        count = 0
        def shape(v):
            if isinstance(v, dict):
                return tuple((k, shape(x)) for k, x in sorted(v.items()))
            if isinstance(v, list):
                return tuple(shape(x) for x in v)
            return type(v).__name__
        def fields(v, path=()):
            if isinstance(v, dict):
                for k, sub in v.items():
                    yield path + (k,)
                    yield from fields(sub, path + (k,))
            elif isinstance(v, list):
                for i, sub in enumerate(v):
                    yield from fields(sub, path + (i,))
        for kind in KINDS:
            for row in Tape(kind).build()[:-1]:
                key = (row["event"], row.get("stage"), row.get("site"), row.get("owner"), shape(row))
                if key in seen:
                    continue
                seen.add(key)
                for path in fields(row):
                    for mode in ("remove", "null"):
                        changed = copy.deepcopy(row)
                        parent = changed
                        for part in path[:-1]:
                            parent = parent[part]
                        if mode == "remove":
                            del parent[path[-1]]
                        else:
                            parent[path[-1]] = None
                        with self.subTest(event=row["event"], path=path, mode=mode):
                            with self.assertRaises(mod.VehicleSmokeError):
                                mod.VehicleSmokeEvidence.validate_record(changed)
                        count += 1
        self.assertGreater(count, 3000)
        print(f"SYNTHETIC required/null schema negatives: {count}; no Wii qualification")

    def test_decisive_predicate_negatives(self):
        # Mutations keep JSON/schema types valid to exercise the actual claim
        # predicates, rather than hiding every negative behind a type error.
        cases = [
            ("run_start", {}, "seed", "12346", "vehicle_fixture_config"),
            ("rig_ready", {}, "water_ok", False, "vehicle_rig_geometry"),
            ("rig_ready", {}, "support_ok", False, "vehicle_rig_geometry"),
            ("rig_ready", {}, "rails_ok", False, "vehicle_rig_geometry"),
            ("rig_ready", {}, "ray_ok", False, "vehicle_rig_geometry"),
            ("rig_ready", {}, "saddle_consumed", False, "vehicle_rig_geometry"),
            ("rig_ready", {}, "pig_ai_updates", 0, "vehicle_rig_geometry"),
            ("rig_ready", {}, "guest.inv_hash", 100, "vehicle_rig_inventory"),
            ("rig_ready", {}, "host.inv_used", 0, "vehicle_rig_inventory"),
            ("rig_ready", {}, "guest.ready", False, "vehicle_rig_readiness"),
            ("rig_ready", {}, "guest.confirmed", False, "vehicle_rig_readiness"),
            ("ride_sample", {"phase": "p1", "stage": "server"}, "eligible_updates", 99, "vehicle_ride_tick_census"),
            ("ride_sample", {"phase": "p1", "stage": "server"}, "tick_calls", 101, "vehicle_ride_tick_census"),
            ("ride_sample", {"phase": "p1", "stage": "server"}, "missing_calls", 1, "vehicle_ride_tick_census"),
            ("ride_sample", {"phase": "p1", "stage": "server"}, "duplicate_calls", 1, "vehicle_ride_tick_census"),
            ("ride_sample", {"phase": "p1", "stage": "server"}, "displacement_m", .24, "vehicle_ride_motion"),
            ("ride_sample", {"phase": "p1", "stage": "server"}, "host.mount_count", 2, "vehicle_ride_server_mount"),
            ("ride_sample", {"phase": "p1", "stage": "server"}, "vehicle.rider.wire_id", 1, "vehicle_actor_identity"),
            ("ride_sample", {"phase": "p1", "stage": "client"}, "seat_error_max_m", .0011, "vehicle_ride_follow"),
            ("ride_sample", {"phase": "p1", "stage": "client"}, "samples", 99, "vehicle_ride_follow"),
            ("ride_sample", {"phase": "p1", "stage": "client"}, "follow_count", 99, "vehicle_ride_follow"),
            ("ride_sample", {"phase": "p1", "stage": "client"}, "host.ride_pending", True, "vehicle_ride_client_mount"),
            ("ride_sample", {"phase": "p1", "stage": "client"}, "vehicle.occupant_id", 7, "vehicle_ride_client_mount"),
            ("action_attempt", {}, "reach_m", 6.01, "vehicle_action_reach"),
            ("action_attempt", {}, "los", False, "vehicle_action_reach"),
            ("action_attempt", {}, "camera_hit", 99, "vehicle_action_reach"),
            ("action_attempt", {}, "edge_seq", 0, "vehicle_edge_sequence"),
            ("isolation", {"owner": "client", "stage": "after"}, "unexpected_route_count", 1, "vehicle_unexpected_route"),
            ("isolation", {"owner": "server", "stage": "after"}, "unexpected_attach_count", 1, "vehicle_isolation_violation"),
            ("isolation", {"owner": "client", "stage": "after"}, "guest_follow_count", 1, "vehicle_isolation_violation"),
            ("isolation", {"owner": "server", "stage": "after"}, "guest_pose_delta_max_m", .051, "vehicle_isolation_violation"),
            ("isolation", {"owner": "server", "stage": "after"}, "guest.inv_hash", 201, "vehicle_guest_identity_changed"),
            ("checkpoint", {"stage": "durable"}, "save_ok", False, "vehicle_checkpoint_durable"),
            ("checkpoint", {"stage": "durable"}, "level_ok", False, "vehicle_checkpoint_durable"),
            ("checkpoint", {"stage": "durable"}, "chunks_ok", False, "vehicle_checkpoint_durable"),
            ("checkpoint", {"stage": "durable"}, "regions_synced", False, "vehicle_checkpoint_durable"),
            ("world_reset", {}, "local_entity", 1, "vehicle_reload_reset"),
            ("title", {}, "autoload_consumed", True, "vehicle_reload_reset"),
            ("reload", {"stage": "disk"}, "disk_reopen", False, "vehicle_reload_disk_pose"),
            ("reload", {"stage": "disk"}, "player_read_ok", False, "vehicle_reload_disk_pose"),
            ("reload", {"stage": "disk"}, "fallback_used", True, "vehicle_reload_disk_pose"),
            ("reload", {"stage": "disk"}, "override_used", True, "vehicle_reload_disk_pose"),
            ("reload", {"stage": "disk"}, "pose_bits.0", 9, "vehicle_reload_disk_pose"),
            ("reload", {"stage": "inventory"}, "inv_hash", 101, "vehicle_reload_inventory"),
            ("reload", {"stage": "inventory"}, "read_ok", False, "vehicle_reload_disk_pose"),
            ("reload", {"stage": "entities"}, "fixture_hash", 301, "vehicle_reload_fixture"),
            ("reload", {"stage": "entities"}, "ambiguous_matches", 1, "vehicle_reload_fixture"),
            ("reload", {"stage": "entities"}, "saddle", False, "vehicle_reload_fixture"),
            ("reload", {"stage": "client_ready"}, "host.ride_pending", True, "vehicle_reload_detached"),
            ("reload", {"stage": "client_ready"}, "input_ack_epoch", 9, "vehicle_reload_ack"),
            ("death", {"stage": "server"}, "host.health", 1, "vehicle_death_commit"),
            ("death", {"stage": "server"}, "deaths_after", 2, "vehicle_death_commit"),
            ("respawn", {"stage": "fresh"}, "health", 159, "vehicle_respawn_ack"),
            ("respawn", {"stage": "ack"}, "ack", False, "vehicle_respawn_ack"),
            ("respawn", {"stage": "client_ready"}, "host.mount_count", 1, "vehicle_respawn_ack"),
            ("transfer_begin", {}, "mode", "contact", "vehicle_origin"),
            ("transfer_release", {}, "old.occupied", True, "vehicle_transfer_release"),
            ("transfer_commit", {}, "destination_mount_count", 1, "vehicle_transfer_source_survival"),
            ("transfer_commit", {}, "source_vehicle.alive", False, "vehicle_transfer_source_survival"),
            ("transfer_ack", {"stage": "client"}, "input_ack_epoch", 9, "vehicle_transfer_ack"),
            ("pending", {"stage": "armed"}, "tuple.ticket", "0", "vehicle_pending_tuple"),
            ("pending", {"stage": "retained", "cause": "global_pause"}, "active_update", 999, "vehicle_pause_retention"),
            ("resume", {}, "ms", 1, "vehicle_pause_retention"),
            ("admission_clear", {}, "transfer_open", False, "vehicle_admission_clear"),
            ("admission_clear", {}, "refusal_armed", True, "vehicle_admission_clear"),
            ("pending", {"stage": "retained", "cause": "transfer"}, "refusal_armed", True, "vehicle_natural_retention_missing"),
            ("barrier_release", {}, "active_update", 999, "vehicle_first_retry_pass"),
            ("installed", {}, "installed_count", 2, "vehicle_retry_install"),
            ("installed", {}, "active_update", 0, "vehicle_progress_deadline"),  # O26: order kept
            ("installed", {}, "ms", 480000, "vehicle_progress_deadline"),
            ("gameplay_ready", {"stage": "server"}, "ready_count", 2, "vehicle_progress_ready"),
            ("gameplay_ready", {"stage": "client"}, "confirmed", False, "vehicle_progress_ready"),
            ("ui_action", {"action": "respawn"}, "held_ms", 749, "vehicle_ui_hold"),
            ("run_ok", {}, "overflow_count", 1, "vehicle_overflow"),
            ("run_ok", {}, "hook_mask", 1, "vehicle_hook_armed"),
            ("run_ok", {}, "pending_markers", 1, "vehicle_run_undrained"),
            ("run_ok", {}, "open_ops", 1, "vehicle_run_undrained"),
        ]
        for name, filters, path, value, reason in cases:
            rows = Tape("boat").build()
            r = next(r for r in rows if r["event"] == "vehicle_smoke." + name and all(r.get(k) == v for k, v in filters.items()))
            parent = r
            parts = path.split(".")
            for part in parts[:-1]:
                parent = parent[int(part) if part.isdigit() else part]
            parent[int(parts[-1]) if parts[-1].isdigit() else parts[-1]] = value
            with self.subTest(name=name, filters=filters, field=path):
                self.assertIn(reason, grade(rows).complete_error() or "GREEN")
                with tempfile.TemporaryDirectory() as td:
                    c = mod.Collector(mod._selftest_args(Path(td), "synthetic_boat", PROFILE,
                        expected_build="test_10.04", cw_vehicle_kind="boat"))
                    for row in rows:
                        if mod.ingest_telemetry_line(c, json.dumps(row, separators=(",", ":"))):
                            break
                    self.assertIn(reason, c.fail_reason or "GREEN")

    def test_completion_sequence_serial_and_phase(self):
        rows = Tape("boat").build()
        self.assertEqual(grade(rows[:-1]).complete_error(), "vehicle_missing_profile_done")
        self.assertEqual(grade(rows + [rows[-1]]).complete_error(), "vehicle_extra_completion")
        self.assertEqual(grade([rows[-1]]).complete_error(), "vehicle_profile_done_before_drain")
        no_ok = rows[:-2] + [rows[-1]]
        self.assertEqual(grade(no_ok).complete_error(), "vehicle_profile_done_before_drain")
        missing = rows[:4] + rows[5:]
        self.assertEqual(grade(missing).complete_error(), "vehicle_seq_gap")
        unordered = copy.deepcopy(rows)
        unordered[4], unordered[5] = unordered[5], unordered[4]
        self.assertEqual(grade(unordered).complete_error(), "vehicle_seq_gap")
        broken = copy.deepcopy(rows)
        release = next(r for r in broken if r["event"] == "vehicle_smoke.transfer_release")
        release["release_serial"] = "999"
        self.assertEqual(grade(broken).complete_error(), "vehicle_transfer_serial_order")
        broken = copy.deepcopy(rows)
        next(r for r in broken if r["event"] == "vehicle_smoke.phase_end")["pending_markers"] = 1
        self.assertEqual(grade(broken).complete_error(), "vehicle_phase_undrained")
        broken = copy.deepcopy(rows)
        next(r for r in broken if r["event"] == "vehicle_smoke.rig_ready")["ok"] = False
        self.assertEqual(grade(broken).complete_error(), "vehicle_terminal_false")

    def test_client_predicates_kind_specific(self):
        for kind in KINDS:
            t = Tape(kind)
            c, v = t.c(mounted=True, request=5), t.vc(occupied=True)
            self.assertTrue(mod.VehicleSmokeEvidence.client_mounted(c, v, 5, 9 if kind == "cart" else 0))
            # The contract explicitly exempts host client boat-steering values
            # while riding pig/cart; detached never receives that exemption.
            c["steer"] = [1., 2.]
            if kind != "boat":
                self.assertTrue(mod.VehicleSmokeEvidence.client_mounted(c, v, 5, 9 if kind == "cart" else 0))
            c["mount_count"] = 0
            self.assertFalse(mod.VehicleSmokeEvidence.client_detached(c))
        for field in ("ride_epoch", "control_seq", "positive_steps"):
            rows = Tape("cart").build()
            r = next(r for r in rows if r["event"] == "vehicle_smoke.ride_sample" and r["phase"] == "p1" and r["stage"] == "server")
            r["vehicle"][field] = 0
            self.assertIn("vehicle_ride_client_mount" if field == "ride_epoch" else "vehicle_ride_cart", grade(rows, "cart").complete_error())

    def test_live_parser_fail_closed(self):
        bad_lines = ["{", "[]", "null", "", '{"event":"vehicle_smoke.run_start","seq":1,"seq":1}',
                     '{"event":"vehicle_smoke.ride_sample","x":NaN}',
                     '{"event":"vehicle_smoke.unknown"}']
        row = Tape("boat").build()[0]
        bad_lines.append(json.dumps(row).rstrip() + " " * 2400)
        for line in bad_lines:
            with self.subTest(line=line[:60]), tempfile.TemporaryDirectory() as td:
                c = mod.Collector(mod._selftest_args(Path(td), "synthetic_boat", PROFILE,
                    expected_build="test_10.04", cw_vehicle_kind="boat"))
                self.assertTrue(mod.ingest_telemetry_line(c, line))
                self.assertEqual(c.failure_class, "vehicle_smoke_evidence_failed")
        self.assertNotIn(PROFILE, mod.ANCHOR_PROFILES)
        self.assertNotIn(PROFILE, mod.RELEASE_FRAME_PROFILES)

    def test_live_green_is_calibration_only_without_provenance(self):
        for kind in KINDS:
            with tempfile.TemporaryDirectory() as td:
                c = mod.Collector(mod._selftest_args(Path(td), "synthetic_" + kind, PROFILE,
                    expected_build="test_10.04", cw_vehicle_kind=kind))
                for row in Tape(kind).build():
                    self.assertFalse(mod.ingest_telemetry_line(c, json.dumps(row, separators=(",", ":"))))
                self.assertFalse(c.done)  # profile_done waits through drain.
                self.assertIsNone(c.vehicle_smoke.complete_error())
                c._finish_normal("synthetic tape")
                self.assertEqual(c.fail_reason, "vehicle_manifest_required")
                self.assertIsNone(c.pass_reason)

    def test_unknown_keys_bool_numeric_and_serial_formats(self):
        for field, value in (("seq", True), ("ms", 1.0), ("ms", -1), ("ms", 2**32),
                             ("schema", "1"), ("ok", 1), ("run_id", "non ascii é"), ("reason", "x"*49)):
            row = Tape("boat").build()[0]
            row[field] = value
            self.assertIn("vehicle_schema", grade([row]).complete_error())
        for value in ("01", "-1", "18446744073709551616", 1, True):
            row = next(r for r in Tape("boat").build() if r.get("serial"))
            row["serial"] = value
            with self.assertRaises(mod.VehicleSmokeError):
                mod.VehicleSmokeEvidence.validate_record(row)
        row = Tape("boat").build()[0]
        row["unknown"] = 0
        self.assertIn("unknown", grade([row]).complete_error())
        row = next(r for r in Tape("boat").build() if r.get("owner") == "server")
        row["unexpected_route_count"] = 0
        with self.assertRaises(mod.VehicleSmokeError):
            mod.VehicleSmokeEvidence.validate_record(row)

    def test_synthetic_packet_replay_and_aggregate(self):
        # Mock ONLY the external authentic-Wii provenance prerequisite. All
        # schema, heartbeat, phase, claim, tape and manifest gates remain live.
        # This test intentionally cannot establish collector qualification.
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            root = Path(td)
            mp = make_manifest(root)
            manifest = json.loads(mp.read_text(encoding="utf-8"))
            self.assertEqual(mod.vehicle_calibration_error(manifest, root), "vehicle_calibration_required")
            passes = []
            with patch.object(mod, "vehicle_calibration_error", return_value=None):
                for kind in KINDS:
                    run_id = "synthetic_" + kind
                    out = root / kind
                    c = mod.Collector(mod._selftest_args(out, run_id, PROFILE, expected_build="test_10.04",
                        cw_vehicle_kind=kind, vehicle_manifest=str(mp)))
                    for i in range(50):
                        hb = dict(event="heartbeat", run_id=run_id, build_id="test_10.04", profile=PROFILE,
                                  tick=i+1, fps=60, oom=0, tel_fail=0, tel_drop=0, world_loaded=True)
                        self.assertFalse(mod.ingest_telemetry_line(c, json.dumps(hb)))
                    for row in Tape(kind).build():
                        self.assertFalse(mod.ingest_telemetry_line(c, json.dumps(row, separators=(",", ":"))))
                    self.assertFalse(c.done)
                    c._finish_normal("SYNTHETIC provenance-mocked parser test")
                    self.assertIsNone(c.fail_reason)
                    self.assertEqual(c.write_result(), 0)
                    passes.append(out / "PASS.json")
                packet = mod.vehicle_validate_packet(passes, str(mp), build_id="test_10.04")
                self.assertEqual(set(packet["kinds"]), set(KINDS))
                self.assertTrue(all(packet["coverage"][a][b] for a in KINDS for b in KINDS if a != b))
                replay_out = root / "regraded-boat"
                args = mod._selftest_args(root, "synthetic_boat", PROFILE, expected_build="test_10.04",
                    replay=str(passes[0].parent), profile_override="", cw_vehicle_kind="boat",
                    vehicle_manifest=str(mp), vehicle_replay_out=str(replay_out))
                self.assertEqual(mod.replay_run_dir(args), 0)
                self.assertTrue((replay_out / "PASS.json").is_file())
                mod.vehicle_validate_packet([replay_out / "PASS.json"], str(mp), "boat", "test_10.04")
                self.assertEqual(mod.replay_run_dir(args), 2)  # no clobbering an existing evidence directory
                with self.assertRaisesRegex(mod.VehicleSmokeError, "three_kinds_required"):
                    mod.vehicle_validate_packet(passes[:2], str(mp), build_id="test_10.04")
                with self.assertRaisesRegex(mod.VehicleSmokeError, "packet_kind"):
                    mod.vehicle_validate_packet([passes[0], passes[0], passes[2]], str(mp), build_id="test_10.04")
                artifact = json.loads(passes[0].read_text())
                artifact["vehicle_manifest_sha256"] = "0"*64
                passes[0].write_text(json.dumps(artifact), encoding="utf-8")
                with self.assertRaisesRegex(mod.VehicleSmokeError, "manifest_mismatch"):
                    mod.vehicle_validate_packet(passes, str(mp), build_id="test_10.04")
                artifact["vehicle_manifest_sha256"] = hashlib.sha256(mp.read_bytes()).hexdigest()
                artifact["vehicle_smoke"]["claims"]["ride"] = False
                passes[0].write_text(json.dumps(artifact), encoding="utf-8")
                with self.assertRaisesRegex(mod.VehicleSmokeError, "summary_mismatch"):
                    mod.vehicle_validate_packet(passes, str(mp), build_id="test_10.04")
                artifact["vehicle_smoke"]["claims"]["ride"] = True
                tape = passes[0].parent / "vehicle-wire.ndjson"
                with tape.open("a", encoding="utf-8") as f:
                    f.write(json.dumps(dict(event="profile_done", run_id="synthetic_boat", status="ok")) + "\n")
                artifact["vehicle_tape_sha256"] = hashlib.sha256(tape.read_bytes()).hexdigest()
                passes[0].write_text(json.dumps(artifact), encoding="utf-8")
                with self.assertRaisesRegex(mod.VehicleSmokeError, "extra_completion"):
                    mod.vehicle_validate_packet(passes, str(mp), build_id="test_10.04")

    def test_missing_decisive_observations_and_invalid_origins(self):
        for name, filters in (("rig_ready", {}), ("checkpoint", {"stage": "durable"}),
                              ("reload", {"stage": "disk"}), ("death", {"stage": "ui"}),
                              ("respawn", {"stage": "ack"}), ("transfer_ack", {"stage": "client"}),
                              ("installed", {}), ("gameplay_ready", {"stage": "client"})):
            rows = Tape("boat").build()
            index = next(i for i, r in enumerate(rows) if r["event"] == "vehicle_smoke." + name and all(r.get(k) == v for k, v in filters.items()))
            del rows[index]
            for i, r in enumerate(rows[:-1]):
                r["seq"] = i+1
            with self.subTest(missing=name):
                self.assertIn("vehicle_missing_or_extra", grade(rows).complete_error())
        for field, value, reason in (("origin", "synthetic", "vehicle_origin"),
                                     ("run_id", "stale", "vehicle_stale_run"),
                                     ("build_id", "old_10.04", "vehicle_build_mismatch"),
                                     ("kind", "pig", "vehicle_kind_mismatch")):
            rows = Tape("boat").build()
            rows[0][field] = value
            self.assertEqual(grade(rows).complete_error(), reason)

    def test_script_roundtrip_and_parameter_restrictions(self):
        for name in ("stage_wii_dol.ps1", "run_staged_wii_hardware.ps1", "collect_staged_wii.ps1", "run_wii_hardware.ps1", "validate_wii_pass.ps1"):
            text = (ROOT / "tools" / name).read_text(encoding="utf-8-sig")
            self.assertIn('"host_vehicle_smoke_8m"', text)
            self.assertIn('$VehicleKind', text)
        for name in ("stage_wii_dol.ps1", "collect_staged_wii.ps1", "run_wii_hardware.ps1"):
            self.assertIn('--cw-vehicle-kind', (ROOT / "tools" / name).read_text())
        staged = (ROOT / "tools/run_staged_wii_hardware.ps1").read_text()
        self.assertIn('$stageArgs += @("-VehicleKind", $VehicleKind', staged)
        self.assertIn('$collectArgs += @("-VehicleKind", $VehicleKind', staged)

    def test_fail_overflow_and_duration_alone(self):
        rows = Tape("boat").build()
        index = next(i for i, row in enumerate(rows) if row["event"] == "vehicle_smoke.ride_sample")
        envelope = ("schema", "run_id", "build_id", "profile", "kind", "seq", "phase", "op_id", "ms",
                    "server_update", "active_update", "event", "origin", "ok", "reason")
        failed = {key: rows[index][key] for key in envelope}
        failed.update(event="vehicle_smoke.fail", phase="failed", origin="production", ok=False,
                      code="probe_overflow", failed_phase="p1", overflow_count=1, hook_mask=0)
        self.assertEqual(grade(rows[:index] + [failed]).complete_error(), "vehicle_overflow")
        failed.update(code="lost_observation", overflow_count=0)
        self.assertIn("vehicle_producer_fail: lost_observation", grade(rows[:index] + [failed]).complete_error())
        run_ok = copy.deepcopy(rows[-2])
        run_ok["seq"] = 1
        self.assertEqual(grade([run_ok]).complete_error(), "vehicle_run_ok_order")
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            c = mod.Collector(mod._selftest_args(Path(td), "synthetic_boat", PROFILE,
                expected_build="test_10.04", cw_vehicle_kind="boat", duration=1))
            mod.ingest_telemetry_line(c, json.dumps(dict(event="heartbeat", run_id="synthetic_boat",
                profile=PROFILE, build_id="test_10.04", tick=1, fps=60)))
            c.started -= 2
            c.tick()
            self.assertFalse(c.done)
            self.assertIsNone(c.pass_reason)
            c._finish_normal("duration alone")
            self.assertEqual(c.fail_reason, "vehicle_missing_run_ok")


if __name__ == "__main__":
    unittest.main(verbosity=2)
