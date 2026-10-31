#!/usr/bin/env python3
"""Multi-seed test matrix (ASTRA-SEED-POLICY-0925).

Tests compile under CAVEWII_RAND_SEED_TEST (source/util.h): every clock seed is
a fixed default unless CW_TEST_SEED overrides it; a scenario test's pinned seed
(rand_test_scenario_seed) moves only under CW_TEST_SEED_FORCE. This tool:

  1. ALWAYS replays the retained regression-seed corpus (tests/seed_corpus.tsv)
     first -- every seed that ever failed is kept and re-run, with its
     classification, so a fixed default can never quietly hide it again;
  2. then explores N seeds per selected test via CW_TEST_SEED (rotating: a
     fresh random base per run unless --seed-base pins it).

Every row records commit, test, env var, effective seed, exit code, the first
failure line and a copy-paste replay command. Each run goes through ctest (the
test's own WORKING_DIRECTORY/args/ENVIRONMENT), ONE test process at a time
(serial) -- the suite has fixed-name fixture dirs and timing tests.

Examples:
  python tools/test_seed_matrix.py --build-dir build-verify --seeds 20
  python tools/test_seed_matrix.py --tests test_lava_aabb_golden --seeds 50 --seed-base 1
  python tools/test_seed_matrix.py --corpus-only
Exit status: 0 when every exploration row passed and every corpus row matched
its recorded expectation; 1 otherwise (new failing seeds are printed as corpus
lines to append -- classify them before committing).
"""
import argparse
import datetime
import glob
import json
import os
import random
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CORPUS = os.path.join(ROOT, "tests", "seed_corpus.tsv")
SEED_RE = re.compile(r"^(0[xX][0-9a-fA-F]{1,8}|[0-9]{1,10})$")


def parse_seed(text):
    """Same acceptance rule as util.c's rand_test_seed_env: 1..0xFFFFFFFF."""
    if not SEED_RE.match(text):
        raise ValueError(f"malformed seed {text!r}")
    value = int(text[2:], 16) if text[:2].lower() == "0x" else int(text, 10)  # never octal
    if value == 0 or value > 0xFFFFFFFF:
        raise ValueError(f"seed {text!r} out of range 1..0xFFFFFFFF (0 zero-locks)")
    return value


def git_commit():
    try:
        head = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short=8", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True).stdout.strip()
        return head + ("+dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def default_tests():
    """Tests whose own source reaches a clock seed or a pinned scenario seed."""
    names = []
    for path in sorted(glob.glob(os.path.join(ROOT, "tests", "test_*.c"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if "rand_gen_seed(" in text or "rand_test_scenario_seed(" in text:
            names.append(os.path.splitext(os.path.basename(path))[0])
    return names


def scenario_tests():
    """Tests whose pinned seed goes through rand_test_scenario_seed."""
    names = set()
    for path in glob.glob(os.path.join(ROOT, "tests", "test_*.c")):
        with open(path, encoding="utf-8", errors="replace") as f:
            if re.search(r"seed\s*=\s*rand_test_scenario_seed\(", f.read()):
                names.add(os.path.splitext(os.path.basename(path))[0])
    return names


def load_corpus(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            cols = line.split("\t")
            if len(cols) < 5:
                raise SystemExit(f"{path}:{n}: need 5 tab-separated columns "
                                 "(test, var, seed, expect, classification[, note])")
            test, var, seed, expect, klass = cols[:5]
            note = cols[5] if len(cols) > 5 else ""
            if var not in ("CW_TEST_SEED", "CW_TEST_SEED_FORCE"):
                raise SystemExit(f"{path}:{n}: var must be CW_TEST_SEED or CW_TEST_SEED_FORCE")
            if expect not in ("pass", "fail"):
                raise SystemExit(f"{path}:{n}: expect must be pass or fail")
            rows.append(dict(test=test, var=var, seed=parse_seed(seed), expect=expect,
                             klass=klass, note=note))
    return rows


def first_failure(output):
    for line in output.splitlines():
        if re.search(r"Assert|assert|RED|FAIL|error|Error|rejected", line):
            return line.strip()[:200]
    return ""


def run_one(build_dir, test, var, seed, timeout):
    """Run ONE registered test through ctest, so its WORKING_DIRECTORY,
    arguments and ENVIRONMENT properties are exactly the suite's."""
    env = dict(os.environ)
    env.pop("CW_TEST_SEED", None)
    env.pop("CW_TEST_SEED_FORCE", None)
    env[var] = f"0x{seed:08X}"
    cmd = ["ctest", "--test-dir", build_dir, "-R", f"^{re.escape(test)}$",
           "--output-on-failure", "--timeout", str(timeout)]
    try:
        p = subprocess.run(cmd, env=env, capture_output=True, text=True,
                           errors="replace", timeout=timeout + 60)
        rc, out = p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        rc, out = "timeout", (e.stdout or "") if isinstance(e.stdout, str) else ""
    if "No tests were found" in out:
        return dict(rc=None, first="not a registered ctest test")
    # fixed-name fixture dirs left by an aborted run poison the next one
    for d in glob.glob(os.path.join(build_dir, test + "_[0-9]*")):
        shutil.rmtree(d, ignore_errors=True)
    return dict(rc=rc, first=first_failure(out) if rc != 0 else "")


def replay_cmd(build_dir, test, var, seed):
    return f'{var}=0x{seed:08X} ctest --test-dir "{build_dir}" -R "^{test}$" --output-on-failure'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build-verify"))
    ap.add_argument("--tests", nargs="*", help="test names (default: every test that seeds from the clock or pins a scenario seed)")
    ap.add_argument("--tests-file", help="file with one test name per line")
    ap.add_argument("--seeds", type=int, default=20, help="exploration seeds per test (0 = corpus only)")
    ap.add_argument("--seed-base", help="pin the exploration seed generator (default: random, recorded)")
    ap.add_argument("--corpus", default=DEFAULT_CORPUS)
    ap.add_argument("--corpus-only", action="store_true")
    ap.add_argument("--explore-scenarios", action="store_true",
                    help="also explore pinned scenario tests through CW_TEST_SEED_FORCE (finds seeds their fixtures do not hold for)")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--out", help="JSONL result file (default: <build-dir>/seed_matrix_<stamp>.jsonl)")
    args = ap.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    if args.tests_file:
        with open(args.tests_file, encoding="utf-8") as f:
            tests = [l.strip() for l in f if l.strip() and not l.startswith("#")]
    elif args.tests:
        tests = args.tests
    else:
        tests = default_tests()
    corpus = load_corpus(args.corpus)
    commit = git_commit()
    base = args.seed_base if args.seed_base is not None else str(random.SystemRandom().getrandbits(32))
    rng = random.Random(f"{base}")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_path = args.out or os.path.join(build_dir, f"seed_matrix_{stamp}.jsonl")
    ok = True
    new_fail_lines = []
    print(f"seed matrix: commit={commit} seed_base={base} tests={len(tests)} "
          f"seeds/test={0 if args.corpus_only else args.seeds} corpus={len(corpus)} -> {out_path}")
    with open(out_path, "w", encoding="utf-8") as out:
        def record(kind, test, var, seed, res, expect=None, klass=None):
            row = dict(kind=kind, commit=commit, test=test, var=var, seed=f"0x{seed:08X}",
                       rc=res["rc"], passed=res["rc"] == 0, first_failure=res["first"],
                       replay=replay_cmd(build_dir, test, var, seed), seed_base=base)
            if expect:
                row.update(expect=expect, classification=klass)
            out.write(json.dumps(row) + "\n")
            out.flush()
            return row

        for c in corpus:
            res = run_one(build_dir, c["test"], c["var"], c["seed"], args.timeout)
            row = record("corpus", c["test"], c["var"], c["seed"], res, c["expect"], c["klass"])
            got = "pass" if row["passed"] else "fail"
            match = got == c["expect"]
            ok &= match
            print(f"  corpus {c['test']} {c['var']}=0x{c['seed']:08X} expect={c['expect']} "
                  f"got={got} [{c['klass']}]{'' if match else '  <-- CHANGED'}"
                  + (f"  {res['first']}" if not row['passed'] else ""))
        if not args.corpus_only:
            scenario = scenario_tests()
            for test in tests:
                fails = 0
                var = "CW_TEST_SEED"
                if test in scenario:
                    if not args.explore_scenarios:
                        print(f"  {test}: pinned scenario seed -- CW_TEST_SEED does not move it "
                              "(corpus replays only; --explore-scenarios to sweep)")
                        continue
                    var = "CW_TEST_SEED_FORCE"
                for _ in range(args.seeds):
                    seed = rng.randrange(1, 0x100000000)
                    res = run_one(build_dir, test, var, seed, args.timeout)
                    row = record("explore", test, var, seed, res)
                    if not row["passed"]:
                        fails += 1
                        ok = False
                        new_fail_lines.append(f"{test}\t{var}\t0x{seed:08X}\tfail\tUNCLASSIFIED\t{res['first']}")
                        print(f"  FAIL {test} {var}=0x{seed:08X}: {res['first']}\n       replay: {row['replay']}")
                print(f"  {test}: {args.seeds - fails}/{args.seeds} pass ({var})")
    if new_fail_lines:
        print("\nNew failing seeds -- classify (engine-defect / fixture-assumption) and append to the corpus:")
        print("\n".join(new_fail_lines))
    print(f"result: {'PASS' if ok else 'FAIL'}  ({out_path})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
