"""REL-06a: fastcmp equivalence.py capture selection, hermetic.

equivalence.py picks its lifecycle capture from an explicit --capture, else
the FASTCMP_CAPTURE environment variable, else the saved D: capture. This
test never touches D:. It compiles fastcmp.c into a private temp folder,
writes two small valid two-dimension captures from the reference's own
synthetic generators (Beta + CaveWii tapes plus the request records the
reference derives its manifest from), and checks:

  1. explicit --capture wins over a (different, valid or invalid) environment value;
  2. FASTCMP_CAPTURE is used when no --capture is given;
  3. a nonexistent or incomplete capture is refused before any comparison,
     whether it came from --capture or from the environment;
  4. a valid overridden capture passes the existing real-tape mutant
     equivalence (reference real_mutants, every mutant caught, Python and
     native reports byte-identical) and the planted value divergence.

usage: python tests/test_fastcmp_capture.py --cc <C compiler>
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[1]
COMPARE = REPO / 'tests/oracle/lighting/g9_option_b/compare'
FASTCMP = COMPARE / 'fastcmp'
EXE_NAME = 'fastcmp-native.exe'


def load_reference():
    spec = importlib.util.spec_from_file_location('reference_first_divergence', COMPARE / 'first_divergence.py')
    ref = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ref)
    return ref


def build(cc, out):
    tmp = out / 'compiler-tmp'
    tmp.mkdir(parents=True)
    env = dict(os.environ, TMP=str(tmp), TEMP=str(tmp), TMPDIR=str(tmp))
    cmd = [cc, '-std=c11', '-O2', '-Wall', '-Wextra', str(FASTCMP / 'fastcmp.c'), '-o', str(out / EXE_NAME),
           '-Wl,-Bstatic', '-lz', '-Wl,-Bdynamic']
    if os.name == 'nt':
        cmd.append('-static-libgcc')
    run = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=300)
    assert run.returncode == 0, f'fastcmp build failed: {run.stdout}{run.stderr}'


def make_capture(ref, root):
    """A valid capture: Overworld then Nether on one tape per side, with the
    Beta requests.json/lifecycle.json and CaveWii capture-rng.txt records.

    The synthetic history's no-op store (same cell rewritten with its own
    value right after the first store) is left out on BOTH sides: with it,
    deleting the first CaveWii store replays into an equal row one event
    later, which the reference's real-tape 'delete first store' mutant (built
    for captured tapes) correctly does not count as caught at the injected
    event. Real captures have no such pair at their first store."""
    ev = {d: next(m for m in ref.mutants(ref.synthetic(d)) if m[0] == 'delete no-op store')[1] for d in (0, -1)}
    beta, cave = root / 'beta', root / 'cave'
    ref.write_tape(beta, ref.two_dimension_tape('beta', ev0=ev[0], ev1=ev[-1]), beta=True)
    ref.write_tape(cave, ref.two_dimension_tape('cave', ev0=ev[0], ev1=ev[-1]))
    coords = ref.SYN_MANIFEST['coordinates']
    (beta / 'requests.json').write_text(json.dumps(
        [{'dimension': d, 'operation': 'load', 'cx': x, 'cz': z} for d in (0, -1) for x, z in coords]))
    (beta / 'lifecycle.json').write_text('[]')
    (cave / 'capture-rng.txt').write_text(''.join(f'load {d} {x} {z} 0 0\n' for d in (0, -1) for x, z in coords))
    return root


def equivalence(out, capture_arg, env_capture):
    env = {k: v for k, v in os.environ.items() if k not in ('FASTCMP_CAPTURE', 'FASTCMP_OUT', 'FASTCMP_EXE')}
    env['FASTCMP_OUT'] = str(out)
    if env_capture is not None:
        env['FASTCMP_CAPTURE'] = str(env_capture)
    cmd = [sys.executable, str(FASTCMP / 'equivalence.py'), '--real-only']
    if capture_arg is not None:
        cmd += ['--capture', str(capture_arg)]
    return subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=900)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cc', required=True)
    a = p.parse_args()
    ref = load_reference()
    work = Path(tempfile.mkdtemp(prefix='test_fastcmp_capture-'))
    failures = []
    try:
        exe_dir = work / 'exe'
        build(a.cc, exe_dir)
        cap_a = make_capture(ref, work / 'capture-a')
        cap_b = make_capture(ref, work / 'capture-b')
        missing = work / 'does-not-exist'
        incomplete = work / 'capture-incomplete'
        (incomplete / 'beta').mkdir(parents=True)
        cases = [
            # name, --capture, FASTCMP_CAPTURE, expected capture, expected source (None = refused)
            ('explicit --capture wins over a valid environment capture', cap_a, cap_b, cap_a, 'argument --capture'),
            ('explicit --capture wins over a nonexistent environment capture', cap_a, missing, cap_a,
             'argument --capture'),
            ('FASTCMP_CAPTURE used without --capture', None, cap_b, cap_b, 'environment FASTCMP_CAPTURE'),
            ('nonexistent explicit --capture refused even with a valid environment capture', missing, cap_b, missing,
             None),
            ('nonexistent FASTCMP_CAPTURE refused', None, missing, missing, None),
            ('incomplete explicit --capture (no cave/) refused', incomplete, None, incomplete, None),
        ]
        for k, (name, arg, env_cap, want, source) in enumerate(cases):
            out = work / f'out-{k}'
            out.mkdir()
            shutil.copy(exe_dir / EXE_NAME, out / EXE_NAME)
            run = equivalence(out, arg, env_cap)
            log = run.stdout + run.stderr
            results = out / 'equivalence-results.json'
            if source is None:
                ok = (run.returncode != 0 and str(want) in log and 'EQUIVALENCE' not in log
                      and not results.exists() and ('FASTCMP_CAPTURE' in log if arg is None else '--capture' in log))
            else:
                got = json.loads(results.read_text()) if results.exists() else {}
                mutants = json.loads((out / 'reference-real-mutants.json').read_text()) \
                    if (out / 'reference-real-mutants.json').exists() else []
                planted = json.loads((out / 'planted-divergence.json').read_text()) \
                    if (out / 'planted-divergence.json').exists() else []
                ok = (run.returncode == 0 and got.get('capture') == str(want).replace('\\', '/')
                      and got.get('capture_source') == source
                      and got.get('cases') and all(c.get('identical') for c in got['cases'])
                      and len(mutants) >= 10 and all(m['caught'] for m in mutants)
                      and [x['dimension'] for x in planted] == [0, -1]
                      and all(x['reported_seq'] == x['planted_seq'] for x in planted)
                      and f'PASS: {len(got["cases"])} exact native/Python comparison calls' in log)
            print(('PASS ' if ok else 'FAIL ') + name, flush=True)
            if not ok:
                failures.append(name)
                print(f'  exit {run.returncode}\n' + '\n'.join('  ' + x for x in log.splitlines()[-25:]), flush=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print(f'FAIL: {len(failures)} capture-selection case(s): {failures}')
        return 1
    print('PASS: test_fastcmp_capture (precedence, refusal, overridden capture equivalence)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
