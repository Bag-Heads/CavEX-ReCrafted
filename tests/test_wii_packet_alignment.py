"""Regression gate for the actual odd-offset FP stores seen in the Wii video fault.

Usage: python tests/test_wii_packet_alignment.py candidate.elf [known-bad.elf]
This is a targeted codegen check, not a general alignment proof or a hardware PASS.
"""
import re
import subprocess
import sys

def hazardous_stores(elf):
    dump = subprocess.check_output([
        'C:/msys64/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objdump.exe',
        '-d', '--disassemble=process_event', elf], text=True)
    assert '<process_event>:' in dump, 'Expected network function missing'
    bad = []
    for line in dump.splitlines():
        match = re.search(r'\b(stfd|stfs)\s+f\d+,(-?\d+)\(r\d+\)', line)
        if match and int(match[2]) % (8 if match[1] == 'stfd' else 4):
            bad.append(line.strip())
    return bad

if len(sys.argv) > 2:
    old = hazardous_stores(sys.argv[2])
    assert old, 'Negative control did not reproduce the observed codegen defect'
    print('KNOWN BAD:', *old, sep='\n')
bad = hazardous_stores(sys.argv[1])
assert not bad, '\n'.join(bad)
print('PASS: release process_event has no odd-offset floating-point stores')
