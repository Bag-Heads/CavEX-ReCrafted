@echo off
rem -- CaveWii_v2 PC build launcher.
rem
rem    Use this for iterating on gameplay (TNT damage, fire spread,
rem    inventory, controls). The PC build (cavewii_v2.exe) uses GLFW
rem    natively, so keyboard + mouse work the way Java Minecraft
rem    players expect:
rem        WASD   = movement
rem        Space  = jump
rem        Shift  = sneak
rem        E      = inventory
rem        ESC    = pause menu
rem        Mouse  = look
rem        LMB    = attack / break block
rem        RMB    = use / place block
rem        Q      = drop item
rem
rem    The Wii build (running through Dolphin) routes input through
rem    Dolphin's Wii controller emulation — that's why WASD doesn't
rem    move the player in Dolphin without per-app Wiimote bindings.
rem    Use this launcher for any test that doesn't specifically need
rem    Wii hardware behavior (perf captures, frame-time benchmarks).
rem
rem    Build the PC binary first if it isn't fresh:
rem        cmake --build build-pc -j4
rem
rem    Then double-click this file, or run from the launchers/ folder.

setlocal
set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "PROJECT_ROOT=%%~fI"

rem Locate the built exe (cmake writes it to build-verify or build-pc).
set "CAVEX_EXE="
for %%D in (build-verify build-pc .) do (
    if not defined CAVEX_EXE if exist "%PROJECT_ROOT%\%%D\cavewii_v2.exe" set "CAVEX_EXE=%PROJECT_ROOT%\%%D\cavewii_v2.exe"
)
if not defined CAVEX_EXE (
    echo cavewii_v2.exe not found. Build the PC target first:
    echo     cmake -B build-verify -S . -G Ninja -DCAVEX_IPO=OFF
    echo     cmake --build build-verify --target cavex
    echo.
    pause
    exit /b 1
)

if not exist "%PROJECT_ROOT%\config.json" (
    echo config.json not found at %PROJECT_ROOT%\config.json
    pause
    exit /b 1
)

if not exist "%PROJECT_ROOT%\assets" (
    echo assets/ folder not found at %PROJECT_ROOT%\assets
    pause
    exit /b 1
)

rem cavewii_v2.exe resolves "config.json", "assets/", and "saves/" relative
rem to its working directory, so cd into the project root before launch.
cd /d "%PROJECT_ROOT%"

rem GLFW/GLEW/zlib/pthread DLLs live in the MSYS2 ucrt64 runtime; add it to PATH
rem so the exe finds them at launch (adjust if MSYS2 is installed elsewhere).
if exist "C:\msys64\ucrt64\bin" set "PATH=C:\msys64\ucrt64\bin;%PATH%"

"%CAVEX_EXE%"

endlocal
