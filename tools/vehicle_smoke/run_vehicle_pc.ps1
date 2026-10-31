param(
    [Parameter(Mandatory=$true)][ValidateSet('boat','pig','cart')][string]$Kind,
    [string]$Tag = '',
    [int]$StallSeconds = 60,
    [int]$TotalSeconds = 600,
    # O16 (R06 P2): exercise the stall path on a deliberately stalled stand-in
    # process (prints one line, then never writes again) instead of the game.
    [switch]$StallSelfTest,
    # O16: pretend no dump tool exists, to exercise the explicit
    # "none available" record (the real tool probe is skipped).
    [switch]$NoDumpTools
)
# PC end-to-end for host_vehicle_smoke_8m (vehicle-smoke-1004 O02; O11 adds the
# stall record). Modeled on tools/run_split_anchor_pc.ps1: a fresh run
# directory with its own config, pull-mode telemetry on stdout,
# CAVEWII_HEADLESS. Hardware-unconfirmed.
# O11 / S03 #1: a STALL (no stdout growth for $StallSeconds) is recorded
# BEFORE the kill: process/stream/task health in stall.json and, when a local
# debugger exists (procdump, cdb, else MSYS2 gdb), an all-thread stack dump.
# Nothing is downloaded; "none available" is recorded when no tool exists.
$ErrorActionPreference = 'Stop'
# O16: frozen in-tree copy (tools/vehicle_smoke/); the repo is two levels up.
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$exe = Join-Path $repo 'build-verify/cavewii_v2.exe'
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$runId = "vs_pc_${Kind}_$stamp$Tag"
$out = Join-Path $repo "tmp/vehicle_pc/$runId"
$run = New-Item -ItemType Directory -Path $out -Force
Copy-Item -LiteralPath (Join-Path $repo 'config.json') -Destination (Join-Path $run.FullName 'config.json')
foreach($name in @('assets','resources')) {
    New-Item -ItemType Junction -Path (Join-Path $run.FullName $name) -Target (Join-Path $repo $name) | Out-Null
}
$configPath = Join-Path $run.FullName 'config.json'
$config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
$config | Add-Member -Force NoteProperty audio ([pscustomobject]@{enabled='off'})
$config | Add-Member -Force NoteProperty experimental ([pscustomobject]@{local_splitscreen=1})
$config | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 -LiteralPath $configPath
$env:CAVEWII_AGENT = $null
$env:CAVEWII_TRACE = '1'
$env:CAVEWII_HEADLESS = '1'
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$arguments = @('--cw-run-id',$runId,'--cw-tel-host','pull','--cw-tel-port','39017',
    '--cw-tel-mode','pull','--cw-profile','host_vehicle_smoke_8m',
    '--cw-vehicle-kind',$Kind,'--cw-vehicle-transfer','fixture_request')
$start = New-Object System.Diagnostics.ProcessStartInfo
if($StallSelfTest) {
    # A deliberately stalled child: one stdout line, then silence for 10 min.
    $start.FileName = 'powershell.exe'
    $start.Arguments = '-NoProfile -Command "Write-Output stall-selftest-started; Start-Sleep -Seconds 600"'
} else {
    $start.FileName = $exe
    $start.Arguments = $arguments -join ' '
}
$start.WorkingDirectory = $run.FullName
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $start
if(-not $process.Start()) { throw 'Could not start the PC app.' }
"pid=$($process.Id) run=$($run.FullName)" | Set-Content -LiteralPath (Join-Path $run.FullName 'pid.txt')
$stdoutPath = Join-Path $run.FullName 'stdout.log'
$stderrPath = Join-Path $run.FullName 'stderr.log'
$stdoutFile = [IO.File]::Open($stdoutPath, 'Create', 'Write', 'ReadWrite')
$stderrFile = [IO.File]::Open($stderrPath, 'Create', 'Write', 'ReadWrite')
$stdout = $process.StandardOutput.BaseStream.CopyToAsync($stdoutFile)
$stderr = $process.StandardError.BaseStream.CopyToAsync($stderrFile)

# ---- O11: poll for exit, stream progress and stalls --------------------------
$deadline = (Get-Date).AddSeconds($TotalSeconds)
$lastLen = -1; $lastGrowth = Get-Date
$completed = $false; $stalled = $false
while($true) {
    if($process.WaitForExit(2000)) { $completed = $true; break }
    $stdoutFile.Flush()
    $len = (Get-Item -LiteralPath $stdoutPath).Length
    if($len -ne $lastLen) { $lastLen = $len; $lastGrowth = Get-Date }
    if(((Get-Date) - $lastGrowth).TotalSeconds -ge $StallSeconds) { $stalled = $true; break }
    if((Get-Date) -ge $deadline) { break }
}
$stall = $null
if(-not $completed) {
    # Health BEFORE the kill: the process, its threads, both stream copies.
    $process.Refresh()
    $lastLine = ''
    try { $lastLine = [string](Get-Content -LiteralPath $stdoutPath -Tail 1) } catch {}
    $threads = @()
    try { foreach($t in $process.Threads) { $threads += [pscustomobject]@{id=$t.Id; state="$($t.ThreadState)"; wait="$(if($t.ThreadState -eq 'Wait'){$t.WaitReason}else{''})"; cpu_ms=[int]$t.TotalProcessorTime.TotalMilliseconds} } } catch {}
    $dumpTool = 'none available (procdump/cdb/gdb not installed locally)'
    $dumpFile = ''
    # O17 (R07 F2): the debugger's own result, recorded before the kill.
    $dumpExit = $null; $dumpOutput = ''
    $pd = $null; $cdb = $null; $gdb = $null
    if($NoDumpTools) {
        $dumpTool = 'none available (dump tools disabled by -NoDumpTools)'
    } else {
        $pd = Get-Command procdump -ErrorAction SilentlyContinue
        $cdb = Get-Command cdb -ErrorAction SilentlyContinue
        $gdb = Get-Command gdb -ErrorAction SilentlyContinue
    }
    try {
        if($pd) {
            $dumpFile = Join-Path $run.FullName 'stall.dmp'
            $dumpOutput = Join-Path $run.FullName 'stall_dump.log'
            & $pd.Source -accepteula -ma $process.Id $dumpFile 2>&1 | Out-File -Encoding utf8 $dumpOutput
            $dumpExit = $LASTEXITCODE
            $dumpTool = "procdump $($pd.Source)"
        } elseif($cdb) {
            $dumpFile = Join-Path $run.FullName 'stall_stacks.txt'
            & $cdb.Source -p $process.Id -c '~*kb; .detach; q' 2>&1 | Out-File -Encoding utf8 $dumpFile
            $dumpExit = $LASTEXITCODE; $dumpOutput = $dumpFile
            $dumpTool = "cdb $($cdb.Source)"
        } elseif($gdb) {
            $dumpFile = Join-Path $run.FullName 'stall_stacks.txt'
            & $gdb.Source -p $process.Id -batch -ex 'set pagination off' -ex 'info threads' -ex 'thread apply all bt' 2>&1 | Out-File -Encoding utf8 $dumpFile
            $dumpExit = $LASTEXITCODE; $dumpOutput = $dumpFile
            $dumpTool = "gdb $($gdb.Source)"
        }
    } catch { $dumpTool = "$dumpTool (failed: $($_.Exception.Message))" }
    # O17 (R07 F2): validate the artifact. A dump is OK only if the debugger
    # exited 0 AND left a non-empty artifact; otherwise it is recorded FAILED
    # (exit code + the tail of the debugger's own stdout/stderr), never silent.
    $dumpOk = $false; $dumpStatus = 'not attempted'; $dumpTail = ''
    if($dumpFile -or $dumpOutput) {
        $artifactBytes = 0
        if($dumpFile -and (Test-Path -LiteralPath $dumpFile)) { $artifactBytes = (Get-Item -LiteralPath $dumpFile).Length }
        if($dumpOutput -and (Test-Path -LiteralPath $dumpOutput)) {
            try { $dumpTail = [string]((Get-Content -LiteralPath $dumpOutput -Raw)) } catch {}
            if($dumpTail.Length -gt 600) { $dumpTail = $dumpTail.Substring($dumpTail.Length - 600) }
        }
        $dumpOk = ($dumpExit -eq 0) -and ($artifactBytes -gt 0)
        $dumpStatus = if($dumpOk) { "ok ($artifactBytes bytes)" } else { "FAILED (exit $dumpExit, artifact $artifactBytes bytes)" }
    }
    $stall = [pscustomobject]@{
        captured_before_kill = $true
        process_alive_at_capture = -not $process.HasExited
        reason = $(if($stalled){"no stdout growth for $StallSeconds s"}else{"total cap $TotalSeconds s"})
        at = (Get-Date).ToString('o')
        last_growth = $lastGrowth.ToString('o')
        pid = $process.Id
        responding = $process.Responding
        cpu_ms = [int]$process.TotalProcessorTime.TotalMilliseconds
        working_set = $process.WorkingSet64
        handles = $process.HandleCount
        thread_count = $threads.Count
        threads = $threads
        stdout_bytes = (Get-Item -LiteralPath $stdoutPath).Length
        stderr_bytes = (Get-Item -LiteralPath $stderrPath).Length
        stdout_copy = "$($stdout.Status)"
        stderr_copy = "$($stderr.Status)"
        last_stdout_line = $lastLine
        dump_tool = $dumpTool
        dump_file = $dumpFile
        dump_exit_code = $dumpExit
        dump_ok = $dumpOk
        dump_status = $dumpStatus
        dump_output_tail = $dumpTail
    }
    $stall | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $run.FullName 'stall.json')
    "STALL recorded: $($stall.reason); dump: $dumpTool; status: $dumpStatus"
    $killAt = (Get-Date).ToString('o')
    $process.Kill()
    $process.WaitForExit()
    # O16: the exit status AFTER the capture (stall.json was written first).
    [pscustomobject]@{ killed_at = $killAt; exit_code = $process.ExitCode;
        stall_json_written_before_kill = (Test-Path (Join-Path $run.FullName 'stall.json')) } |
        ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $run.FullName 'exit.json')
}
$process.WaitForExit()
[void]$stdout.GetAwaiter().GetResult()
[void]$stderr.GetAwaiter().GetResult()
$stdoutFile.Dispose()
$stderrFile.Dispose()
$exitCode = $process.ExitCode
# The tape: every JSON line exactly as emitted (LF-terminated).
$lines = @(Get-Content -LiteralPath $stdoutPath | Where-Object { $_ -match '^\{"(event"|schema":1,)' })
[IO.File]::WriteAllText((Join-Path $run.FullName 'telemetry.ndjson'), (($lines -join "`n") + "`n"))
"exit=$exitCode completed=$completed stalled=$stalled lines=$($lines.Count) run=$($run.FullName)"
if($StallSelfTest) { "stall self-test done: $($run.FullName)"; return }
# Grade the tape with the live parser's offline replay (Sol's collector; not edited).
$build = ''
foreach($l in $lines) { if($l -match '"build_id":"([^"]+)"') { $build = $Matches[1]; break } }
Push-Location $repo
& python tools/wii_telemetry_server.py --replay $run.FullName --cw-vehicle-kind $Kind --vehicle-transfer-mode fixture_request --expected-build $build --profile-override host_vehicle_smoke_8m 2>&1 | Tee-Object -FilePath (Join-Path $run.FullName 'replay.txt')
Pop-Location
