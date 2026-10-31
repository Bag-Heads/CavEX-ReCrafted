# Copyright (c) 2026 CaveWii_v2 contributors
"""Require the enqueue invariant to reject a live old tag, plus valid stale retirement."""
import subprocess
import sys

valid = subprocess.run([sys.argv[1], 'tag'], capture_output=True, timeout=30)
assert valid.returncode == 0, valid.stdout + valid.stderr
invalid = subprocess.run([sys.argv[1], 'tag-invalid'], capture_output=True, timeout=30)
assert invalid.returncode != 0, 'live old tag survived'
assert b'old_tag_stale' in invalid.stderr, invalid.stdout + invalid.stderr
print(valid.stdout.decode(), end='')
print('R5 P3: live old enqueue tag rejected by stale-only assertion PASS')
