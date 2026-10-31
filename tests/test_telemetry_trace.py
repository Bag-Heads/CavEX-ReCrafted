"""Trace recovery and diagnostic liveness must not certify a heartbeat."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('tel_trace_collector', ROOT / 'tools/wii_telemetry_server.py')
m = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = m
spec.loader.exec_module(m)

def main():
    with tempfile.TemporaryDirectory() as td:
        out = Path(td)
        previous = out / 'stage_mailbox/previous_cw_tel_trace.txt'
        previous.parent.mkdir()
        previous.write_text('run=previous reason=1 errno=-11 net_send=-11\n')
        traces = m.read_telemetry_traces(out)
        assert traces == [{'path': 'stage_mailbox/previous_cw_tel_trace.txt', 'text': previous.read_text()}]
        result = {'status': 'FAIL', 'run_id': 'current', 'telemetry_traces': traces}
        summary = m.render_summary(result)
        assert 'run=previous' in summary and 'check embedded run id' in summary
        c = m.Collector(m._selftest_args(out / 'current', 'current', 'heartbeat_selftest'))
        before = c.last_heartbeat_event
        c.process({'event': 'alive', 'frame': 4294967295, 'tick': 123})
        assert c.last_heartbeat_event == before and not c.pass_reason
        assert json.loads((c.out / 'alive.ndjson').read_text())['frame'] == 4294967295
        previous.unlink()
        assert m.read_telemetry_traces(out) == []
        (out / 'cw_tel_trace.txt').write_text('x' * 10000)
        assert len(m.read_telemetry_traces(out)[0]['text']) == 4096
    # Actual TCP reader recovers after the poisoned prefix, retaining alive
    # without mistaking it for the 58th heartbeat or extending the deadline.
    from test_telemetry_recorded_pull import ReplaySocket, collector, FIXTURE
    from unittest.mock import patch
    rows = FIXTURE.read_bytes().splitlines(keepends=True)
    tail = b'{"event":"heartbeat","tick"' + b'\n{"event":"alive","frame":777,"tick":58}                         \n'
    sock = ReplaySocket(rows, 13, True, False, tail)
    with tempfile.TemporaryDirectory() as td:
        out = Path(td)
        args = collector._selftest_args(out, json.loads(rows[0])["run_id"], "split_anchor_6m",
            expected_build="502eea9_09.06", connect_timeout=1,
            tcp_connect_host="socket-double", tcp_connect_port=1)
        with patch.object(collector.socket, "create_connection", return_value=sock), \
             patch.object(collector.time, "monotonic", side_effect=lambda: sock.now):
            assert collector.run_tcp_pull(args) != 0
        report = json.loads((out/'FAIL.json').read_text())
        assert report['heartbeats'] == 57
        assert report['failure_class'] == 'heartbeat_stale_after_start'
        assert json.loads((out/'alive.ndjson').read_text())['frame'] == 777
    # Protocol allowlist and the existing runner both expose the same file.
    manifest = (ROOT/'tools/wii_stage_server/source/stage_manifest_paths.h').read_text()
    runner = (ROOT/'tools/run_staged_wii_hardware.ps1').read_text(encoding='utf-8')
    assert '"sd:/cw_tel_trace.txt"' in manifest
    assert '"sd:/cw_tel_trace.txt",' in runner
    assert '"sd:/cw_tel_trace.txt" { return "previous_cw_tel_trace.txt" }' in runner
    print('PASS trace mailbox/summary, bounded read, alive persisted without heartbeat/PASS')

if __name__ == '__main__':
    main()
