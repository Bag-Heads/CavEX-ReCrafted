#!/bin/bash
# O04: one PC vehicle smoke ($1 = kind), replay + vsdiag2 diagnostic.
# O16: frozen in-tree copy; tools beside this script, repo two levels up.
S=$(cd "$(dirname "$0")" && pwd)
W=$(cd "$S/../.." && pwd)
cd $W
for k in "$@"; do
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File $S/run_vehicle_pc.ps1 -Kind $k 2>&1 | grep -E "^\[replay\]|^STALL|stalled="
  d=$(ls -dt $W/tmp/vehicle_pc/vs_pc_${k}_* | head -1)
  echo "RUN $k $(basename $d)"
  sha256sum $d/telemetry.ndjson | cut -c1-16
  grep -o '"reason":"[^"]*","\(code\|completed_phases\)[^,]*,[^,]*' $d/telemetry.ndjson | tail -1
  echo "-- diag past the global_pause lookup:"
  python $S/vsdiag2.py $d/telemetry.ndjson $k 2>&1  # O11: full first-error output, strict kind
  [ -f $d/stall.json ] && { echo "-- stall record:"; cat $d/stall.json; }
done
echo ALL_DONE
