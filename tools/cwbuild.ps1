# cwbuild.ps1 -- thin build front end (docs/build_architecture.md step 5).
# One verb per workflow; every verb dispatches to an EXISTING canonical entry
# point (CMakePresets.json, tools/ci.sh, tools/local_ci.sh, the Wii Makefile,
# the build-wii gate script). No build logic lives here on purpose.
#
#   cwbuild dev                 fast PC app build (target cavex)  (build-pcdev)
#   cwbuild test <exact-name>   build ONE test target + run it    (build-pcdev)
#   cwbuild verify              full suite, mirrors ci.sh leg 1   (build-verify)
#   cwbuild asan                Linux suite + ASan soak via WSL   (tools/local_ci.sh --verify-only)
#   cwbuild bench <name>        build + run one bench             (build-verify; benches keep NDEBUG)
#   cwbuild pc-release          PC release app, IPO on            (build-release)
#   cwbuild wii-dev             fast Wii CaveWii_v2-dev.dol       (gated: build-verify.sh --dev)
#   cwbuild wii-release         gated release CaveWii_v2.dol      (build-wii md5/build-ID gate script)
#
# wii-dev/wii-release resolve the sidecar libogc stage-lib (CW_LIBOGC_LIB env, else
# the documented default under ..\logc256kr\stage-lib -- see
# docs/wii_libogc_kernel_heap_sidecar.md) and pass it through as LIBOGC_LIB=<path>,
# failing early with the sidecar-build instructions if it's missing.
#
# Windows PowerShell 5.1 compatible. Run from anywhere inside the repo.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('dev', 'test', 'verify', 'asan', 'bench', 'pc-release', 'wii-dev', 'wii-release')]
    [string]$Verb,
    [Parameter(Position = 1)]
    [string]$Name
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
Set-Location $Repo

$MsysBash = 'C:\msys64\usr\bin\bash.exe'

function Invoke-Step {
    param([string]$Exe, [string[]]$Args)
    & $Exe @Args
    if ($LASTEXITCODE -ne 0) {
        Write-Error "cwbuild: '$Exe $($Args -join ' ')' failed (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
}

function Invoke-MsysBash {
    param([string]$Command)
    # msys bash so devkitPPC / ci scripts see the environment they expect.
    & $MsysBash -lc "cd `"$($Repo -replace '\\', '/')`" && $Command"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "cwbuild: bash step failed (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
}

function Test-OnPath {
    param([string]$ToolName)
    return [bool](Get-Command $ToolName -ErrorAction SilentlyContinue)
}

# Windows path -> MSYS form (C:\foo\bar -> /c/foo/bar), the form LIBOGC_LIB=
# and the Makefile's -L path handling expect (docs/wii_libogc_kernel_heap_sidecar.md).
function ConvertTo-MsysPath {
    param([string]$WinPath)
    $full = [System.IO.Path]::GetFullPath($WinPath)
    if ($full -notmatch '^[A-Za-z]:\\') {
        Write-Error "cwbuild: cannot convert to an MSYS path (expected 'C:\...'): $full"
        exit 5
    }
    $drive = $full.Substring(0, 1).ToLower()
    $rest = $full.Substring(2) -replace '\\', '/'
    return "/$drive$rest"
}

# Resolve the sidecar libogc stage-lib every real-Wii build must link (the
# stock devkitPro libogc's 64KB lwp KERNEL_HEAP silently starves the Wiimote's
# attach-time allocation -- docs/wii_libogc_kernel_heap_sidecar.md,
# third_party/libogc-kernel-heap/README.md). CW_LIBOGC_LIB overrides the
# documented default stage-lib location. Fails early (before ever invoking
# bash/make) with the sidecar-build instructions if it's missing -- the
# Makefile's own gate would catch a missing/bad LIBOGC_LIB too, but only after
# `make` has already started, and cwbuild's whole point is failing fast.
function Resolve-LibogcLib {
    $DefaultLibogcLib = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'logc256kr\stage-lib')
    $WinPath = if ($env:CW_LIBOGC_LIB) { $env:CW_LIBOGC_LIB } else { $DefaultLibogcLib }
    if (-not (Test-Path $WinPath)) {
        Write-Error @"
cwbuild: sidecar libogc stage-lib not found: $WinPath
Real-Wii .dols MUST link the patched 256KB-KERNEL_HEAP libogc sidecar -- the
stock 64KB pool silently starves the Wiimote's attach-time allocation (see
docs/wii_libogc_kernel_heap_sidecar.md). Build it with:
  tools/build_libogc_sidecar.sh
then either let it land at the default path above, or set CW_LIBOGC_LIB to
point at your own stage-lib directory.
"@
        exit 5
    }
    if (-not (Test-Path (Join-Path $WinPath 'libogc.a'))) {
        Write-Error "cwbuild: $WinPath exists but has no libogc.a -- not a valid sidecar stage-lib (see docs/wii_libogc_kernel_heap_sidecar.md, tools/build_libogc_sidecar.sh)"
        exit 5
    }
    return ConvertTo-MsysPath $WinPath
}

# Fail fast with an actionable message instead of letting a missing tool
# surface as a confusing mid-build error several steps in. Only checks what
# the chosen verb actually needs.
function Assert-Preflight {
    param([string]$V)

    $NeedsCMake = @('dev', 'test', 'verify', 'bench', 'pc-release') -contains $V
    $NeedsCTest = @('test', 'verify') -contains $V
    $NeedsNinja = @('dev', 'test', 'verify', 'pc-release') -contains $V
    $NeedsMsys = @('asan', 'wii-dev', 'wii-release') -contains $V
    $NeedsDevkitPPC = @('wii-dev', 'wii-release') -contains $V

    if ($NeedsCMake -and -not (Test-OnPath 'cmake')) {
        Write-Error 'cwbuild: cmake not found on PATH -- install CMake / add to PATH'
        exit 5
    }
    if ($NeedsCTest -and -not (Test-OnPath 'ctest')) {
        Write-Error 'cwbuild: ctest not found on PATH -- install CMake / add to PATH'
        exit 5
    }
    if ($NeedsNinja -and -not (Test-OnPath 'ninja')) {
        Write-Error 'cwbuild: ninja not found on PATH -- install Ninja / add to PATH'
        exit 5
    }
    if ($NeedsMsys -and -not (Test-Path $MsysBash)) {
        Write-Error "cwbuild: MSYS bash not found at $MsysBash -- install MSYS2 to C:\msys64"
        exit 5
    }
    if ($NeedsDevkitPPC -and -not (Test-Path 'C:\msys64\opt\devkitpro\devkitPPC')) {
        Write-Error 'cwbuild: devkitPPC not found at C:\msys64\opt\devkitpro\devkitPPC -- install devkitPro/devkitPPC'
        exit 5
    }
    if ($NeedsDevkitPPC) {
        # wii-dev / wii-release ONLY: resolve + validate the sidecar stage-lib up
        # front (script-scope so the switch block below can pass it to make).
        $script:LibogcLibMsys = Resolve-LibogcLib
    }
}

Assert-Preflight -V $Verb

switch ($Verb) {
    'dev' {
        Invoke-Step 'cmake' @('--preset', 'dev')
        Invoke-Step 'cmake' @('--build', '--preset', 'dev', '--target', 'cavex')
    }
    'test' {
        if (-not $Name) { Write-Error 'usage: cwbuild test <exact-test-name>'; exit 2 }
        # Guarantee $Name is regex-metacharacter-free before it's spliced into
        # the ctest -R pattern below -- "^$Name$" then matches it literally,
        # anchored, and (given unique CMake test names) exactly one test.
        if ($Name -notmatch '^[A-Za-z0-9_]+$') {
            Write-Error "cwbuild: invalid test name '$Name' -- only letters, digits, and underscores are allowed"
            exit 2
        }
        if (-not (Test-Path (Join-Path $Repo 'build-pcdev\CMakeCache.txt'))) {
            Invoke-Step 'cmake' @('--preset', 'dev')
        }
        Invoke-Step 'cmake' @('--build', '--preset', 'dev', '--target', $Name)
        Invoke-Step 'ctest' @('--test-dir', 'build-pcdev', '-R', "^$Name$", '--output-on-failure')
    }
    'verify' {
        Invoke-Step 'cmake' @('--preset', 'verify')
        Invoke-Step 'cmake' @('--build', '--preset', 'verify')
        Invoke-Step 'ctest' @('--preset', 'verify')
    }
    'asan' {
        # ASan does not link on MSYS; the canonical path is the WSL leg of
        # local_ci.sh (Linux suite + 8000-iter ASan/UBSan soak).
        Invoke-MsysBash 'bash tools/local_ci.sh --verify-only'
    }
    'bench' {
        if (-not $Name) { Write-Error 'usage: cwbuild bench <bench-target>'; exit 2 }
        if (-not (Test-Path (Join-Path $Repo 'build-verify\CMakeCache.txt'))) {
            Invoke-Step 'cmake' @('--preset', 'verify')
        }
        Invoke-Step 'cmake' @('--build', '--preset', 'verify', '--target', $Name)
        Invoke-Step (Join-Path $Repo "build-verify\$Name.exe") @()
    }
    'pc-release' {
        Invoke-Step 'cmake' @('--preset', 'pc-release')
        Invoke-Step 'cmake' @('--build', '--preset', 'pc-release')
    }
    'wii-dev' {
        # Same guarded script as wii-release, in --dev mode (item 2): md5-changed
        # + sole-build-ID + sidecar KERNEL_HEAP gates against
        # build-dev/CaveWii_v2-dev.dol, never touching build/ or the release
        # CaveWii_v2.dol. No inline compiler-wrapper logic here -- build-verify.sh
        # owns regenerating the gitignored cc-wrap. LIBOGC_LIB (resolved by
        # Assert-Preflight above) is forwarded as a real make command-line
        # assignment via build-verify.sh's args passthrough.
        Invoke-MsysBash "bash .agents/skills/build-wii/scripts/build-verify.sh --dev LIBOGC_LIB='$script:LibogcLibMsys'"
    }
    'wii-release' {
        # The ONLY sanctioned release-.dol path: md5-changed + sole-build-ID +
        # sidecar KERNEL_HEAP gates.
        Invoke-MsysBash "bash .agents/skills/build-wii/scripts/build-verify.sh LIBOGC_LIB='$script:LibogcLibMsys'"
    }
}
