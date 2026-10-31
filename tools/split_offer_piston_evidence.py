"""Server-owned offered-body piston tape; imported by the Wii collector."""
import math

PROFILE = "split_offer_piston_2m"
PREFIX = "split_offer_piston."
ORDER = [("phase", 0), ("world_ready", 0), ("phase", 1), ("offered", None),
         ("phase", 2), ("fixture", None), ("phase", 3), ("before", None),
         ("extended", None), ("phase", 4), ("released", None), ("admitted", None),
         ("phase", 5), ("play_begin", None), ("phase", 6), ("play_end", None),
         ("run_ok", 6)]


class SplitOfferPistonEvidence:
    def __init__(self, run_id):
        self.run_id = run_id
        self.rows = []
        self.error = None
        self.done = False
        self.server = {}

    @staticmethod
    def require(value, reason):
        if not value:
            raise ValueError(reason)

    def note(self, row):
        if self.error:
            return
        try:
            need = self.require
            need(type(row) is dict, "row_schema")
            need(row.get("run_id") == self.run_id and row.get("profile") == PROFILE,
                 "run_profile_identity")
            need(type(row.get("schema")) is int and row["schema"] == 1, "schema_version")
            need(type(row.get("seq")) is int and row["seq"] == len(self.rows)+1,
                 "contiguous_seq")
            need(type(row.get("event")) is str, "event_schema")
            name = row["event"].removeprefix(PREFIX)
            need(not self.done, "row_after_profile_done")
            if name == "fail":
                raise ValueError("driver_" + str(row.get("reason", "failure")))
            if name == "error":
                raise ValueError("server_" + str(row.get("reason", "failure")))
            need(len(self.rows) < len(ORDER), "unexpected_row")
            event, phase = ORDER[len(self.rows)]
            need(row.get("event") == PREFIX+event, "event_order_"+event)
            if phase is not None:
                need(row.get("origin") == "driver" and type(row.get("phase")) is int
                     and row["phase"] == phase, "phase_order")
            else:
                self._server(row, name)
            self.rows.append(dict(row))
        except (ValueError, TypeError, KeyError, AttributeError) as exc:
            self.error = str(exc)

    def _server(self, row, name):
        need = self.require
        need(row.get("origin") == "server", "server_origin")
        for key in ("tick", "generation", "guest_corrections", "host_corrections",
                    "guest_damage", "host_token", "guest_token", "health", "host_health", "play_ms",
                    "difficulty", "world_slot", "world_generation", "dimension"):
            need(type(row.get(key)) is int and row[key] >= 0, "server_field_"+key)
        for key in ("bound", "confirmed", "gameplay", "finished_loading", "extended", "ok", "superflat"):
            need(type(row.get(key)) is bool, "server_field_"+key)
        for key in ("host", "guest"):
            need(type(row.get(key)) is list and len(row[key]) == 3
                 and all(type(x) in (int, float) and math.isfinite(x) for x in row[key]),
                 "server_pose_"+key)
        need(row["ok"], "server_"+str(row.get("reason", "failure")))
        need(row["difficulty"] == 0 and row["superflat"], "fixture_environment")
        need(row["world_slot"] == 0 and row["world_generation"] > 0
             and row["dimension"] == 0, "guest_world")
        need(row["bound"] and row["generation"] > 0, "guest_bound")
        need(row["guest_damage"] == 0 and row["health"] == 160, "guest_damage")
        if self.server:
            prev = next(reversed(self.server.values()))
            need(row["tick"] >= prev["tick"], "server_tick_order")
            need(row["generation"] == prev["generation"], "guest_generation")
            need(row["world_generation"] == prev["world_generation"], "guest_world_generation")
        if name in ("offered", "fixture", "before", "extended", "released"):
            need(not row["confirmed"] and not row["gameplay"] and not row["finished_loading"],
                 "guest_admitted_early")
        if name == "offered":
            need(row.get("reason") == "accept_delayed_fixture", "offer_fixture_disclosure")
        if name == "fixture":
            need(row.get("reason") == "blocks_only_fixture", "block_fixture_disclosure")
            dx, dz = row["host"][0]-row["guest"][0], row["host"][2]-row["guest"][2]
            need(dx*dx+dz*dz >= 9, "host_not_clear")
            frac = row["guest"][0] - math.floor(row["guest"][0])
            need(.80 < frac < 1, "fixture_geometry")
        if name in ("extended", "released"):
            before = self.server["before"]
            need(row["extended"] and row["tick"]-before["tick"] >= 5, "extension_incomplete")
            need(abs(row["guest"][0]-before["guest"][0]-.5625) <= .0001
                 and row["guest"][1:] == before["guest"][1:], "guest_push_distance")
            need(row["guest_corrections"]-before["guest_corrections"] == 1,
                 "guest_correction_delta")
            need(row["guest_token"] == (((before["guest_token"]+1) & 0xffffffff) or 1), "guest_token_delta")
            need(row["host_corrections"] == before["host_corrections"]
                 and row["host_token"] == before["host_token"]
                 and row["host"] == before["host"]
                 and row["host_health"] == before["host_health"], "host_route_touched")
        if name in ("admitted", "play_begin", "play_end"):
            need(row["confirmed"] and row["gameplay"] and row["finished_loading"],
                 "play_admission_lost")
        if name == "play_end":
            before = self.server["play_begin"]
            need(row["play_ms"] >= 30000 and row["tick"]-before["tick"] >= 300,
                 "play_window_short")
            dx, dz = row["guest"][0]-before["guest"][0], row["guest"][2]-before["guest"][2]
            need(dx*dx+dz*dz >= 1, "play_no_movement")
        self.server[name] = dict(row)

    def note_profile_done(self, status):
        if not self.error:
            if self.done:
                self.error = "duplicate_profile_done"
            elif status != "ok":
                self.error = "profile_done_not_ok"
            elif len(self.rows) != len(ORDER):
                self.error = "missing_" + ORDER[len(self.rows)][0]
        self.done = True

    def complete_error(self):
        return self.error or (None if self.done else "profile_done_missing")

    def summary(self):
        return {"rows": len(self.rows), "profile_done": self.done, "error": self.complete_error(),
                "server": self.server}
