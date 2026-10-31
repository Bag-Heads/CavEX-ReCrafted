"""Copyright (c) 2026 CaveWii_v2 contributors.

R24 gate 4 mutation witness. Run under cw-heavy.lock after building
test_g9_shadow_cap_overflow with Ninja's -d keeprsp. Compile a private copy
with only the shadow-cap predicate removed, substitute its object in the
test's retained linker response file, and require the specific fallback RED.
Never changes source/, the normal object, or the normal test executable.
"""
import hashlib
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-verify"
SOURCE = ROOT / "source/network/region_archive.c"
OBJECT = "CMakeFiles/cavex_test_core.dir/source/network/region_archive.c.obj"
TARGET = "test_g9_shadow_cap_overflow"


def main():
    original = SOURCE.read_bytes()
    work = BUILD / "g9-shadow-cap-mutation"
    work.mkdir(exist_ok=True)
    text = original.decode("utf-8").replace("\r\n", "\n")
    predicate = "\n\t   || op->data.len > RIO_SHADOW_CAP - s_rio_shadow_bytes"
    assert text.count(predicate) == 1, "cap predicate changed; review mutation"
    text = text.replace(predicate, "", 1)
    # Preserve the copy's include resolution without copying/changing headers.
    def absolute_include(match):
        path = (SOURCE.parent / match[1]).resolve()
        return '#include "' + path.as_posix() + '"' if path.is_file() else match[0]
    text = re.sub(r'^#include "([^"\n]+)"', absolute_include, text, flags=re.M)
    copied = work / "region_archive_no_shadow_cap.c"
    copied.write_text(text, encoding="utf-8", newline="\n")
    commands = subprocess.check_output(
        ["ninja", "-C", str(BUILD), "-t", "commands", TARGET], text=True)
    commands = commands.replace("\\", "/").splitlines()
    compile_line = [line for line in commands
                    if " -c " + SOURCE.as_posix() in line and OBJECT in line]
    assert len(compile_line) == 1
    args = shlex.split(compile_line[0])
    mutant_object = work / "region_archive_no_shadow_cap.obj"
    args[args.index("-o") + 1] = mutant_object.as_posix()
    args[args.index("-c") + 1] = copied.as_posix()
    for flag in ("-MT", "-MF"):
        index = args.index(flag)
        del args[index:index + 2]
    args.remove("-MD")
    subprocess.run(args, cwd=BUILD, check=True)
    rsp = BUILD / ("CMakeFiles/" + TARGET + ".rsp")
    link_args = shlex.split(rsp.read_text().replace("\\", "/"))
    assert link_args.count(OBJECT) == 1
    link_args[link_args.index(OBJECT)] = mutant_object.as_posix()
    mutant_rsp = work / "mutant.rsp"
    mutant_rsp.write_text(subprocess.list2cmdline(link_args), encoding="utf-8")
    exe = work / (TARGET + "_no_cap.exe")
    subprocess.run([args[0], "@" + mutant_rsp.as_posix(), "-o", exe.as_posix()],
                   cwd=BUILD, check=True)
    result = subprocess.run([str(exe)], cwd=BUILD, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=30)
    print(result.stdout, end="", flush=True)
    print("mutation exit:", result.returncode)
    assert result.returncode != 0, "removed cap survived the regression"
    assert "barrier waits +0; retained hits +1" in result.stdout
    assert "shadow overflow must use draining-wait fallback" in result.stdout
    assert SOURCE.read_bytes() == original, "production source changed"
    print("source SHA256 unchanged:", hashlib.sha256(original).hexdigest())
    print("MUTATION KILLED: removed cap produced a retained hit instead of draining")


if __name__ == "__main__":
    main()
