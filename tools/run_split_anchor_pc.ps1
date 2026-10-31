param(
    [long]$Seed = 12345,
    [ValidateRange(0,1048576)][int]$AnchorSep = 96,
    [ValidateRange(1,3600)][int]$Duration = 360,
    [string]$OutRoot = "",
    [switch]$TpsCpu,
    [switch]$Settle,
    [switch]$ColumnCensus,
    [switch]$CarveCensus,
    # Experimental local split-screen (lane/guest-body, 2026-09-07): measure a
    # REAL second gameplay body instead of the synthetic +X anchor. Arms
    # "experimental.local_splitscreen" for this run only (--cw-split-guest),
    # auto-joins the guest as soon as the world is ready and walks BOTH bodies,
    # so the two residency anchors are two players rather than one player and
    # an offset. -AnchorSep still applies: it keeps the SYNTHETIC anchor while
    # the guest is joining, and the guest takes anchor 1 the moment it lands.
    [switch]$SplitGuest,
    [string]$ExecutablePath = ""
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build-verify/cavewii_v2.exe'
if($ExecutablePath) { $exe = (Resolve-Path -LiteralPath $ExecutablePath).Path }
if(-not (Test-Path -LiteralPath $exe)) { throw 'Build the verify preset first.' }
if(-not $OutRoot) {
    $OutRoot = Join-Path $repo ('tmp/anchor_pc/{0}_{1}_{2}' -f $Seed,$AnchorSep,(Get-Date -Format 'yyyyMMdd_HHmmss'))
}
$run = New-Item -ItemType Directory -Path $OutRoot -Force
if(Test-Path -LiteralPath (Join-Path $run.FullName 'config.json')) {
    throw 'Use a fresh OutRoot: this harness always creates a fresh world.'
}
Copy-Item -LiteralPath (Join-Path $repo 'config.json') -Destination (Join-Path $run.FullName 'config.json')
foreach($name in @('assets','resources')) {
    New-Item -ItemType Junction -Path (Join-Path $run.FullName $name) -Target (Join-Path $repo $name) | Out-Null
}
$configPath = Join-Path $run.FullName 'config.json'
$config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
$config | Add-Member -Force NoteProperty audio ([pscustomobject]@{enabled='off'})
$config | Add-Member -Force NoteProperty lod ([pscustomobject]@{high_detail_radius=2;far_view_distance=6;far_storage='voxel'})
if($SplitGuest) {
    # The switch also lives in config.json so the harness world reproduces the
    # shipping read path, not just the run-argument override.
    $config | Add-Member -Force NoteProperty experimental ([pscustomobject]@{local_splitscreen=1})
}
$config | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 -LiteralPath $configPath
if($CarveCensus) {
    [ordered]@{seed=$Seed; separation=$AnchorSep; measured_seconds=$Duration;
        executable=$exe; executable_sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash;
        carve_census=$true} | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $run.FullName 'run_provenance.json')
}
$priorAgent = $env:CAVEWII_AGENT
$priorTrace = $env:CAVEWII_TRACE
$priorHeadless = $env:CAVEWII_HEADLESS
$priorPath = $env:PATH
try {
    $env:CAVEWII_AGENT = $null
    $env:CAVEWII_TRACE = '1'
    $env:CAVEWII_HEADLESS = '1'
    $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
    $profileName = if($SplitGuest) { 'split_play_2m' } else { 'split_anchor_6m' }
    $arguments = @('--cw-run-id','anchor_pc','--cw-tel-host','pull','--cw-tel-port','39017',
        '--cw-tel-mode','pull','--cw-profile',$profileName,'--cw-duration',"$Duration",
        '--cw-seed',"$Seed",'--cw-anchor-sep',"$AnchorSep")
    if($SplitGuest) { $arguments += '--cw-split-guest' }
    if($TpsCpu) { $arguments += "--cw-tps-cpu" }
    if($Settle) { $arguments += "--cw-settle" }
    if($CarveCensus) { $arguments += "--cw-carve-census" }
    if($ColumnCensus) { $arguments += "--cw-column-census" }
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
    if(-not $process.Start()) { throw 'Could not start the PC harness.' }
    $stdoutFile = [IO.File]::Open((Join-Path $run.FullName 'stdout.log'), 'Create', 'Write', 'ReadWrite')
    $stderrFile = [IO.File]::Open((Join-Path $run.FullName 'stderr.log'), 'Create', 'Write', 'ReadWrite')
    $stdout = $process.StandardOutput.BaseStream.CopyToAsync($stdoutFile)
    $stderr = $process.StandardError.BaseStream.CopyToAsync($stderrFile)
    $completed = $process.WaitForExit(($Duration + 840) * 1000)
    if(-not $completed) { $process.Kill() }
    $process.WaitForExit()
    [void]$stdout.GetAwaiter().GetResult()
    [void]$stderr.GetAwaiter().GetResult()
    $stdoutFile.Dispose()
    $stderrFile.Dispose()
    $exitCode = $process.ExitCode
    $process.Dispose()
    if(-not $completed) { throw 'PC anchor run exceeded readiness and measurement deadline.' }
    if($exitCode -ne 0) { throw "PC harness exited $exitCode; inspect $($run.FullName)/stderr.log" }
    $lines = @(Get-Content -LiteralPath (Join-Path $run.FullName 'stdout.log') | Where-Object { $_ -match '^\{"event":' })
    $lines | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $run.FullName 'telemetry.ndjson')
    $events = @($lines | ForEach-Object { $_ | ConvertFrom-Json })
    $beats = @($events | Where-Object { $_.event -eq 'heartbeat' })
    $frameTotal = 0
    foreach($event in $events) {
        if($event.PSObject.Properties.Name -contains 'anchor_frame_us') {
            if($event.anchor_frame_dropped -ne 0 -or ($event.anchor_frame_total - $frameTotal) -ne $event.anchor_frame_us.Count) {
                throw 'Incomplete per-frame telemetry; this run is not valid evidence.'
            }
            $frameTotal = $event.anchor_frame_total
        }
    }
    if(-not $beats.Count -or -not ($beats | Where-Object anchor_measuring)) { throw 'No measured anchor heartbeat: inspect the logs.' }
    if($SplitGuest) {
        # The run is only evidence for a SECOND BODY if the guest actually
        # joined: the experimental switch has to have been armed, and the
        # second anchor has to have reported a frontier of its own. A run
        # where the Join never landed is a single-player run with a longer
        # log, and must never be filed as split-screen evidence.
        $armed = @(Get-Content -LiteralPath (Join-Path $run.FullName 'stderr.log') |
            Where-Object { $_ -match 'experimental\.local_splitscreen = on' })
        if(-not $armed.Count) { throw 'experimental.local_splitscreen never armed: not split-screen evidence.' }
        $admitted = @(Get-Content -LiteralPath (Join-Path $run.FullName 'stderr.log') |
            Where-Object { $_ -match '\[splitscreen\] guest body ADMITTED' })
        if(-not $admitted.Count) { throw 'The guest body never joined: this is a single-player run, not split-screen evidence.' }
        $retired = @(Get-Content -LiteralPath (Join-Path $run.FullName 'stderr.log') |
            Where-Object { $_ -match '\[splitscreen\] guest body RETIRED' })
        [ordered]@{
            split_guest = $true
            anchor_sep = $AnchorSep
            seed = $Seed
            measured_seconds = $Duration
            guest_admitted = [string]$admitted[0]
            guest_final = [string](@(Get-Content -LiteralPath (Join-Path $run.FullName 'stderr.log') |
                Where-Object { $_ -match 'guest body FINAL' }) | Select-Object -Last 1)
            guest_retired_events = $retired.Count
            heartbeats = $beats.Count
            anchor_measuring_heartbeats = @($beats | Where-Object anchor_measuring).Count
            carve_min_kb = (@($beats | ForEach-Object { $_.mem2_carve_free_kb }) | Measure-Object -Minimum).Minimum
            anchor_near_owed_max = (@($beats | ForEach-Object { $_.anchor_near_owed[1] }) | Measure-Object -Maximum).Maximum
            oom = (@($beats | ForEach-Object { $_.oom }) | Measure-Object -Maximum).Maximum
            spill = (@($beats | ForEach-Object { $_.spill }) | Measure-Object -Maximum).Maximum
            mesh_trunc = (@($beats | ForEach-Object { $_.mesh_trunc }) | Measure-Object -Maximum).Maximum
            executable_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
            hardware_confirmed = $false
        } | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $run.FullName 'split_guest_receipt.json')
        Write-Host ([string]$admitted[0])
    }
    if($CarveCensus) {
        [ordered]@{exit_code=$exitCode; duration_seconds=$Duration; completed=$completed} |
            ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $run.FullName 'run_completion.json')
    }
    $beats | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $run.FullName 'heartbeats.json')
    Write-Host "PC harness complete: $($beats.Count) heartbeats, $frameTotal complete frame samples; hardware-unconfirmed. $($run.FullName)"
} finally {
    $env:CAVEWII_AGENT = $priorAgent
    $env:CAVEWII_TRACE = $priorTrace
    $env:CAVEWII_HEADLESS = $priorHeadless
    $env:PATH = $priorPath
}
