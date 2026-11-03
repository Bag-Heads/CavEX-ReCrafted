param(
    [Parameter(Mandatory=$true)][string]$RunDir,
    [int]$ProxyPort = 25570,
    [int]$ServerPort = 25565,
    [int]$Seconds = 150,
    [string]$User = 'WalkCap',
    [int]$InstallRadius = 4
)
# IN-007 capture: PC client (build-verify/CavEX-ReCrafted.exe, headless, pull-mode
# telemetry on stdout) runs the real mp_lan_walk_10m profile against the local
# Project Poseidon b1.7.3 server through capture_tee.py, which records every
# inbound byte. The server must already be up (tmp/poseidon/poseidon_host.ps1).
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$exe = Join-Path $repo 'build-verify/CavEX-ReCrafted.exe'
New-Item -ItemType Directory -Path $RunDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'config.json') -Destination (Join-Path $RunDir 'config.json') -Force
foreach($name in @('assets','resources')) {
    $p = Join-Path $RunDir $name
    if(-not (Test-Path $p)) { New-Item -ItemType Junction -Path $p -Target (Join-Path $repo $name) | Out-Null }
}
$configPath = Join-Path $RunDir 'config.json'
$config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
$config | Add-Member -Force NoteProperty audio ([pscustomobject]@{enabled='off'})
# The Wii default MP admission radius is 4 (SERVER_REMOTE_ADMIT_RADIUS_DEFAULT,
# source/network/server_remote.h); PC defaults to 10. Pin the Wii value so
# columns outside 4 stay RETAINED (unpromoted) exactly as on the console.
if($InstallRadius -gt 0) {
    $mp = $config.mp
    if(-not $mp) { $mp = [pscustomobject]@{}; $config | Add-Member -Force NoteProperty mp $mp }
    $mp | Add-Member -Force NoteProperty install_radius $InstallRadius
}
$config | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 -LiteralPath $configPath

$py = (Get-Command python -ErrorAction Stop).Source
$tee = Start-Process -FilePath $py -PassThru -WindowStyle Hidden -ArgumentList @('-I', (Join-Path $PSScriptRoot 'capture_tee.py'), "$ProxyPort", '127.0.0.1', "$ServerPort", (Join-Path $RunDir 'tee'))
Start-Sleep -Seconds 2

$env:CAVEWII_HEADLESS = '1'
$env:CAVEWII_TRACE = '1'
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$args2 = @('--cw-run-id','in007_walk','--cw-tel-host','pull','--cw-tel-port','39018','--cw-tel-mode','pull',
    '--cw-profile','mp_lan_walk_10m','--cw-mp-endpoint',"127.0.0.1:$ProxyPort",'--cw-mp-username',$User)
$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $exe
$start.Arguments = $args2 -join ' '
$start.WorkingDirectory = $RunDir
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $start
[void]$proc.Start()
"app=$($proc.Id) tee=$($tee.Id)" | Set-Content (Join-Path $RunDir 'pids.txt')
$o = [IO.File]::Open((Join-Path $RunDir 'stdout.log'), 'Create', 'Write', 'ReadWrite')
$e = [IO.File]::Open((Join-Path $RunDir 'stderr.log'), 'Create', 'Write', 'ReadWrite')
$t1 = $proc.StandardOutput.BaseStream.CopyToAsync($o)
$t2 = $proc.StandardError.BaseStream.CopyToAsync($e)
if(-not $proc.WaitForExit($Seconds * 1000)) { $proc.Kill(); $proc.WaitForExit() }
try { $t1.Wait(5000) | Out-Null; $t2.Wait(5000) | Out-Null } catch {}
$o.Close(); $e.Close()
if(-not $tee.HasExited) { Start-Sleep -Seconds 2; if(-not $tee.HasExited) { Stop-Process -Id $tee.Id } }
"exit=$($proc.ExitCode)" | Add-Content (Join-Path $RunDir 'pids.txt')
