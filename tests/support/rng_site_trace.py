"""Opt-in Windows attribution build; normal pools/source/build graph untouched.

python tests/support/rng_site_trace.py HEAD test_mob_windup_cue
python tests/support/rng_site_trace.py c9e25a0fa test_mob_windup_cue
Only one archive is permitted. Generated primitive translation units differ by
one observation call per primitive, plus noinline annotations for call sites.
"""
from pathlib import Path
import argparse, gzip, os, re, shutil, subprocess, time

ROOT = Path(__file__).resolve().parents[2]
EV = ROOT / 'tmp/jobs/integ-1011/evidence'
SUPPORT = ROOT / 'tests/support'
CC = 'C:/msys64/ucrt64/bin/cc.exe'
NINJA = 'C:/msys64/ucrt64/bin/ninja.exe'

def run(args, cwd, log=None, check=True, env=None):
    if log:
        with log.open('w') as f:
            return subprocess.run(args, cwd=cwd, stdout=f, stderr=subprocess.STDOUT,
                                  check=check, env=env)
    return subprocess.run(args, cwd=cwd, check=check, env=env)

def tokens(command):
    # Ninja emits these compiler paths without spaces. Preserve backslashes.
    import shlex
    return [s.strip('"') for s in shlex.split(command, posix=False)]

def compile_copy(command, source, output, includes):
    args = tokens(command)
    for flag in ['-MD', '-MT', '-MF']:
        if flag in args:
            i = args.index(flag)
            del args[i:i + (1 if flag == '-MD' else 2)]
    args[args.index('-c') + 1] = str(source)
    args[args.index('-o') + 1] = str(output)
    args.extend('-I' + str(p) for p in includes)
    run(args, output.parent)

def main():
    # MinGW tools must not put linker temporaries in C:/Windows.
    for variable in ['TMP', 'TEMP', 'TMPDIR']:
        os.environ[variable] = (ROOT / 'tmp').as_posix()
    ap = argparse.ArgumentParser(); ap.add_argument('pin'); ap.add_argument('tests', nargs='+')
    opt = ap.parse_args(); scratch = ROOT / 'scratch-P'; archive = EV / 'rng-scratch.tar'
    assert not scratch.exists(), 'one scratch archive at a time'
    tree = ROOT if opt.pin == 'HEAD' else scratch
    gen = ROOT / 'build-verify/rng-generated'; assert not gen.exists(); gen.mkdir()
    started = time.time()
    try:
        if tree == scratch:
            scratch.mkdir()
            with archive.open('wb') as f:
                subprocess.run(['git', 'archive', opt.pin], cwd=ROOT, stdout=f, check=True)
            run(['tar', '-xf', str(archive), '-C', str(scratch)], ROOT); archive.unlink()
            run(['cmake', '--preset', 'verify'], tree, EV / f'rng-{opt.pin}-configure.log')
        run(['cmake', '--build', 'build-verify', '--target', *opt.tests, '-j', '4'],
            tree, EV / f'rng-{opt.pin}-build.log')
        build = tree / 'build-verify'
        for name in opt.tests:
            exe = build / (name + '.exe')
            os.utime(exe, (1, 1))
            run([NINJA, '-d', 'keeprsp', name], build, EV / f'rng-{opt.pin}-{name}-relink.log')
        commands = subprocess.check_output([NINJA, '-t', 'commands', opt.tests[0]], cwd=build, text=True).splitlines()
        replacements = {}
        for relative in ['source/util.c', 'source/worldgen/java_random.c']:
            original = tree / relative
            command = next(c for c in commands if c.endswith('-c ' + original.as_posix()))
            text = original.read_text()
            text = '#include "rng_site_trace.h"\n#line 1 "' + original.as_posix() + '"\n' + text
            # Observation follows the exact existing transition, before return.
            if relative.endswith('/util.c'):
                marker = '\treturn g->seed;'
                assert text.count(marker) == 1
                text = text.replace(marker, '\trng_site_trace_emit(0, g, g->seed, g->seed);\n' + marker)
            else:
                marker = '\treturn (int32_t)(uint32_t)(r->seed >> (48 - bits));'
                assert text.count(marker) == 1
                text = text.replace(marker, '\trng_site_trace_emit(1, r, (uint32_t)(r->seed >> (48 - bits)), r->seed);\n' + marker)
            text = re.sub(r'^(uint32_t|int32_t|int64_t|int|float|double|bool) ((?:jrand|rand_gen)[a-z_]*\()',
                          r'__attribute__((noinline)) \1 \2', text, flags=re.M)
            generated = gen / original.name; generated.write_text(text)
            obj = gen / (original.stem + '.obj')
            compile_copy(command, generated, obj, [SUPPORT, original.parent])
            args = tokens(command); replacements[args[args.index('-o') + 1].replace('\\', '/')] = obj.as_posix()
        base_command = next(c for c in commands if c.endswith('-c ' + (tree / 'source/util.c').as_posix()))
        logger = gen / 'logger.obj'
        if 'struct server_body_rand' in (tree / 'source/network/server_local_state.h').read_text():
            base_command += ' -DRNG_TRACE_BODY_RAND -DRNG_TRACE_WORLD_RAND'
        if 'gameplay_rng' in (tree / 'source/entity/entity.h').read_text():
            base_command += ' -DRNG_TRACE_ENTITY_RAND'
        compile_copy(base_command, SUPPORT / 'rng_site_trace.c', logger, [SUPPORT])
        for name in opt.tests:
            response = (build / 'CMakeFiles' / (name + '.rsp')).read_text()
            for before, after in replacements.items():
                assert before in response.replace('\\', '/')
                response = response.replace('\\', '/').replace(before, after)
            # Compile only this owned fixture copy with optional bindings.
            if not name.startswith('test_column_dedup_'):
                commands = subprocess.check_output([NINJA, '-t', 'commands', name], cwd=build, text=True).splitlines()
                original = tree / 'tests' / (name + '.c')
                command = next(c for c in commands if c.endswith('-c ' + original.as_posix()))
                text = original.read_text().replace('trace_tick = t;', 'trace_tick = t;\n\t\trng_site_trace_bind(&s, &trace_tick);')
                text = text.replace('server_local_test_update_once(s);', 'rng_site_trace_bind(s, NULL); server_local_test_update_once(s); rng_site_trace_bind(NULL, NULL);')
                text = text.replace('inventory_destroy(&s.player.inventory', 'rng_site_trace_bind(NULL, NULL);\n\tinventory_destroy(&s.player.inventory')
                generated = gen / (name + '.c')
                generated.write_text('#include "rng_site_trace.h"\n#line 1 "' + original.as_posix() + '"\n' + text)
                obj = gen / (name + '.obj')
                compile_copy(command, generated, obj, [SUPPORT, original.parent])
                args = tokens(command); old = args[args.index('-o') + 1].replace('\\', '/')
                assert old in response; response = response.replace(old, obj.as_posix())
            # Static import libraries must follow every object using them.
            response = logger.as_posix() + ' ' + response
            rsp = gen / (name + '.rsp'); rsp.write_text(response)
            exe = gen / (name + '.exe')
            run([CC, '@' + rsp.as_posix(), '-o', exe.as_posix(), '-Wl,--out-implib,' + (gen / 'trace.dll.a').as_posix(), '-Wl,--major-image-version,0,--minor-image-version,0'], build,
                EV / f'rng-{opt.pin}-{name}-diagnostic-link.log')
            # Retain exact executable until call sites are resolved; compact nm
            # inventory survives deletion of generated archive/builds.
            with (EV / f'rng-{opt.pin}-{name}.nm').open('w') as f:
                subprocess.run(['C:/msys64/ucrt64/bin/nm.exe', '-n', str(exe)], stdout=f, check=True)
            env = os.environ.copy(); env['CW_RNG_SITE_TRACE'] = str(EV / f'rng-{opt.pin}-{name}.gz')
            args = [str(exe)]
            if name == 'test_mob_windup_cue': args += ['--sim-trace', str(EV / f'rng-{opt.pin}-windup.txt')]
            if name == 'test_perf_stress_survival_guard': args += ['--stress-trace']
            if name.startswith('test_column_dedup_'): args += ['--capture', str(EV / f'rng-{opt.pin}-walk')]
            result = run(args, tree, EV / f'rng-{opt.pin}-{name}.log', check=False, env=env)
            print(opt.pin, name, 'exit', result.returncode, 'elapsed', round(time.time() - started), flush=True)
            # Resolve every unique frame before deleting this exact executable.
            addresses = set()
            with gzip.open(env['CW_RNG_SITE_TRACE'], 'rb') as f:
                import struct
                while chunk := f.read(96 * 8192):
                    assert len(chunk) % 96 == 0
                    for rec in struct.iter_unpack('<12Q', chunk): addresses.update(rec[7:])
            addresses.discard(0); ordered = sorted(addresses)
            resolved = subprocess.check_output(['C:/msys64/ucrt64/bin/addr2line.exe', '-f', '-e', str(exe)],
                input=''.join(hex(0x140000000 + a - 1) + '\n' for a in ordered), text=True).splitlines()
            import json
            (EV / f'rng-{opt.pin}-{name}-sites.json').write_text(json.dumps({str(a): resolved[i*2:i*2+2] for i,a in enumerate(ordered)}, indent=2))
    finally:
        assert gen.resolve().is_relative_to((ROOT / 'build-verify').resolve())
        shutil.rmtree(gen)
        if scratch.exists():
            assert scratch.resolve() == ROOT / 'scratch-P'; shutil.rmtree(scratch)
        if archive.exists(): archive.unlink()

if __name__ == '__main__': main()
