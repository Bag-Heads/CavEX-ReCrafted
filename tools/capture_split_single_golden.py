"""Capture the single-anchor trace using the complete pinned baseline engine.
Run: python tools/capture_split_single_golden.py
Requires Git, CMake and Ninja plus the normal PC build dependencies on PATH.
Only the shared test harness is copied from the current tree; every engine
source/header and build definition comes from the printed baseline commit.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tarfile
import tempfile


BASELINE = "2cfa18516abf1f803be77ab627a659e823ca4f91"
TARGET = "test_split_residency_single_equiv"


def main():
    root = Path(__file__).resolve().parents[1]
    out = root / "tmp/anchorfix_golden"
    out.mkdir(parents=True, exist_ok=True)
    sha = subprocess.check_output(
        ["git", "rev-parse", BASELINE + "^{commit}"], cwd=root, text=True
    ).strip()
    print("Building complete baseline " + sha, flush=True)
    # A full Git archive avoids modifying the shared repository's worktree
    # registry. Each invocation builds fresh and removes its isolated tree.
    with tempfile.TemporaryDirectory(prefix="base-", dir=root / "tmp") as td:
        temp = Path(td)
        archive = temp / "baseline.tar"
        subprocess.run(["git", "archive", "--format=tar", "-o", str(archive),
                        sha], cwd=root, check=True)
        source = temp / "src"
        with tarfile.open(archive) as tar:
            tar.extractall(source, filter="data")
        for name in (f"tests/{TARGET}.c", "tests/support/split_residency_fixture.h"):
            shutil.copyfile(root / name, source / name)
        # Add only a capture executable after the baseline's test inventory
        # gate. The baseline pool definitions and all engine files stay intact.
        with (source / "CMakeLists.txt").open("a", encoding="utf-8", newline="\n") as cmake:
            cmake.write(f"""
add_executable({TARGET} tests/{TARGET}.c)
target_link_libraries({TARGET} cavex_test_core cavex_test_world cavex_test_tickq cavex_test_input
    ${{CMAKE_THREAD_LIBS_INIT}} ZLIB::ZLIB glfw GLEW::GLEW OpenGL::GL m)
target_include_directories({TARGET} PRIVATE ${{CMAKE_CURRENT_SOURCE_DIR}}/source ${{CMAKE_CURRENT_SOURCE_DIR}}/third_party)
target_compile_definitions({TARGET} PRIVATE CW_CAPTURE_PINNED_BASELINE)
target_compile_options({TARGET} PRIVATE -UNDEBUG)
set_target_properties({TARGET} PROPERTIES C_STANDARD 99)
""")
        build = temp / "build"
        for label, command in (
            ("configure", ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                           "-DBUILD_TESTING=ON", "-DCAVEWII_REQUIRE_TEST_TOOLS=ON"]),
            ("build", ["cmake", "--build", str(build), "--target", TARGET, "-j4"]),
        ):
            with (out / (label + ".log")).open("w", encoding="utf-8") as log:
                subprocess.run(command, cwd=source, stdout=log,
                               stderr=subprocess.STDOUT, check=True)
        print("Built baseline " + sha, flush=True)
        exe = build / (TARGET + (".exe" if os.name == "nt" else ""))
        result = subprocess.run(
            [str(exe)], cwd=build, env=dict(os.environ, CW_CAPTURE_SINGLE_GOLDEN="1"),
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        (out / "trace.log").write_text("baseline=" + sha + "\n" + result.stdout,
                                      encoding="utf-8", newline="\n")
        print(result.stdout, flush=True)
        result.check_returncode()


if __name__ == "__main__":
    main()
