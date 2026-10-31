param(
    [string]$ExecutablePath = "",
    [string]$OutRoot = "",
    [int]$Seed = 12345,
    [int]$Duration = 20,
    [string]$WorldName = "Nether 1030"
)
# make_nether_world.ps1 (nether-1030 diagnostic): create a saved world whose
# player stands in the Nether, using the GAME'S OWN create+save code
# (screen_select_world.c sworld_persist_new_archive -> level_archive_*), never a
# hand-written level.dat. Runs the headless PC build with debug.start_nether=on
# and the "+new" autoload sentinel under the far_idle_2m harness profile, waits
# for the auto-exit save, then copies the world to <OutRoot>/world/<WorldName>.
# The Wii run then loads it with -AutoloadName "<WorldName>" and every chunk the
# walker reaches is FRESH Nether generation (the region files hold only the
# spawn area), which is the streaming cost under diagnosis.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build-verify/cavewii_v2.exe'
if($ExecutablePath) { $exe = (Resolve-Path -LiteralPath $ExecutablePath).Path }
if(-not (Test-Path -LiteralPath $exe)) { throw 'Build the verify preset first (build-verify/cavewii_v2.exe).' }
if(-not $OutRoot) { $OutRoot = Join-Path $repo 'tmp/jobs/nether-1030' }
$run = New-Item -ItemType Directory -Path (Join-Path $OutRoot ('pc-run_{0}' -f (Get-Date -Format 'yyyyMMdd_HHmmss'))) -Force
Copy-Item -LiteralPath (Join-Path $repo 'config.json') -Destination (Join-Path $run.FullName 'config.json')
foreach($name in @('assets','resources')) {
    New-Item -ItemType Junction -Path (Join-Path $run.FullName $name) -Target (Join-Path $repo $name) | Out-Null
}
$configPath = Join-Path $run.FullName 'config.json'
$config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
$config | Add-Member -Force NoteProperty audio ([pscustomobject]@{enabled='off'})
$debug = if($config.PSObject.Properties['debug']) { $config.debug } else { [pscustomobject]@{} }
$debug | Add-Member -Force NoteProperty start_nether 'on'
$debug | Add-Member -Force NoteProperty world_seed $Seed
$config | Add-Member -Force NoteProperty debug $debug
$config | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 -LiteralPath $configPath
New-Item -ItemType Directory -Path (Join-Path $run.FullName 'saves') -Force | Out-Null
$priorHeadless = $env:CAVEWII_HEADLESS
$priorPath = $env:PATH
try {
    $env:CAVEWII_HEADLESS = '1'
    $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
    # far_idle_2m: "+new" autoload, no autopilot, no force_overworld (hw_run.c),
    # auto-exit after --cw-duration -> the quit path saves the regions.
    $arguments = @('--cw-run-id','nether_1030_make','--cw-tel-host','pull','--cw-tel-port','39017',
        '--cw-tel-mode','pull','--cw-profile','far_idle_2m','--cw-duration',"$Duration",'--cw-seed',"$Seed")
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $exe
    $start.Arguments = $arguments -join ' '
    $start.WorkingDirectory = $run.FullName
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    if(-not $process.Start()) { throw 'Could not start the PC build.' }
    $stdoutFile = [IO.File]::Open((Join-Path $run.FullName 'stdout.log'), 'Create', 'Write', 'ReadWrite')
    $stderrFile = [IO.File]::Open((Join-Path $run.FullName 'stderr.log'), 'Create', 'Write', 'ReadWrite')
    $stdout = $process.StandardOutput.BaseStream.CopyToAsync($stdoutFile)
    $stderr = $process.StandardError.BaseStream.CopyToAsync($stderrFile)
    $completed = $process.WaitForExit(($Duration + 240) * 1000)
    if(-not $completed) { $process.Kill() }   # our own PID only
    $process.WaitForExit()
    [void]$stdout.GetAwaiter().GetResult()
    [void]$stderr.GetAwaiter().GetResult()
    $stdoutFile.Dispose(); $stderrFile.Dispose()
    $exitCode = $process.ExitCode
    $process.Dispose()
    if(-not $completed) { throw 'PC run did not exit in time.' }
    if($exitCode -ne 0) { throw "PC build exited $exitCode; inspect $($run.FullName)/stderr.log" }
} finally {
    $env:CAVEWII_HEADLESS = $priorHeadless
    $env:PATH = $priorPath
}
$made = Get-ChildItem -LiteralPath (Join-Path $run.FullName 'saves') -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'level.dat') }
if(-not $made) { throw "no world with level.dat under $($run.FullName)/saves" }
$src = $made | Select-Object -First 1
$dst = Join-Path $OutRoot (Join-Path 'world' $WorldName)
if(Test-Path -LiteralPath $dst) { Remove-Item -Recurse -Force -LiteralPath $dst }
New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
Copy-Item -Recurse -LiteralPath $src.FullName -Destination $dst
Get-ChildItem -Recurse -File -LiteralPath $dst | ForEach-Object {
    '{0}  {1}  {2}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash, $_.Length, $_.FullName.Substring($dst.Length + 1)
} | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $OutRoot 'world.sha256.txt')
Write-Host "[make-nether-world] world: $dst (from $($src.FullName)); hashes in $OutRoot/world.sha256.txt"
