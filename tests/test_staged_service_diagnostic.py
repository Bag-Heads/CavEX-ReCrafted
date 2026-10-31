"""Execute staged diagnostic assembly/config checks without a Wii or C build."""
import subprocess
import tempfile
import os
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PS = os.environ.get('CW_PWSH', str(Path.home() / '.cache/codex-runtimes/codex-primary-runtime/dependencies/native/powershell/pwsh.exe'))
if not Path(PS).is_file():
    PS = shutil.which('pwsh') or shutil.which('powershell')


def section(name, start, end):
    text = (ROOT / 'tools' / name).read_text()
    return text[text.index(start):text.index(end, text.index(start))]


def run(body):
    with tempfile.TemporaryDirectory(dir=ROOT / 'tmp') as td:
        script = Path(td) / 'check.ps1'
        script.write_text('$ErrorActionPreference="Stop"\n' + body)
        p = subprocess.run([PS, '-NoProfile', '-File', str(script)],
                           capture_output=True, text=True, timeout=30)
        assert p.returncode == 0, p.stdout + p.stderr
        return p.stdout


def main():
    for name in ('run_staged_wii_hardware.ps1', 'stage_wii_dol.ps1', 'collect_staged_wii.ps1'):
        assert '[switch]$ServiceDiagnostic' in (ROOT / 'tools' / name).read_text(), name
    preamble = f"$RepoRoot='{ROOT.as_posix()}'; $Profile='perf_stress_5m'; $ServiceDiagnostic=$true; $Set=@(); $RunId='diag'; $TelemetryPort=39017; $AnchorSep=0; $WiiHost='localhost'\n"
    stage = section('run_staged_wii_hardware.ps1', '$stageArgs = @(', 'Write-Host "[run-staged] staging DOL')
    collect = section('run_staged_wii_hardware.ps1', '$collectArgs = @(', '& powershell @collectArgs')
    output = run(preamble + stage + '\n$stageArgs -join "|"\n' + collect + '\n$collectArgs -join "|"')
    assert output.count('-ServiceDiagnostic') == 2, output
    collector = section('collect_staged_wii.ps1', '$args = @(', '& $pythonExe @args')
    output = run(preamble + collector + '\n$args -join "|"')
    assert '--duration|600' in output and '--require-service-exposure' in output, output
    assembly = section('stage_wii_dol.ps1', '$argBytes = $null', 'if($ServiceDiagnostic) {\n    . (Join-Path')
    setup = preamble + '$NoRunArgs=$false; $SkipLocalLaunchPlanPreflight=$true\n'
    output = run(setup + assembly + '\n$argText')
    assert '--cw-duration 600' in output and '--cw-set ' not in output, output
    output = run(setup.replace('$ServiceDiagnostic=$true', '$ServiceDiagnostic=$false') + assembly + '\n$argText')
    assert '--cw-duration 300' in output, output
    # Six is accepted; seven (including comma-packed child argv) and >511 bytes fail.
    for count in (6, 7):
        assignments = ','.join(f'debug.x{i}=1' for i in range(count))
        output = run(setup + f"$Set=@('{assignments}'); $caught=$false\ntry {{\n" + assembly + '\n} catch { $caught=$true }; "CAUGHT=$caught"')
        assert f'CAUGHT={str(count > 6)}' in output, output
    output = run(setup + "$RunId='x'*512; $caught=$false\ntry {\n" + assembly + '\n} catch { $caught=$true }; "CAUGHT=$caught"')
    assert 'CAUGHT=True' in output, output
    output = run(setup + "$Set=@('debug.service_diagnostic_int=0'); $caught=$false\ntry {\n" + assembly + '\n} catch { $caught=$true }; "CAUGHT=$caught"')
    assert 'CAUGHT=True' in output, output
    output = run(setup + "$Set=@('debug.x=1 --cw-set debug.y=1'); $caught=$false\ntry {\n" + assembly + '\n} catch { $caught=$true }; "CAUGHT=$caught"')
    assert 'CAUGHT=True' in output, output
    helper = (ROOT / 'tools/service_diagnostic_config.ps1').read_text()
    # Mock existing fetch/put tools, retain unrelated card settings, check both mirrors.
    mock = r'''
$script:card=@{}; $script:puts=0; $script:tamper=$false
foreach($path in @('sd:/config.json','sd:/apps/CavEX/config.json')) {
    $script:card[$path]='{"sentinel":42,"debug":{"autopilot":"walk","music_force":"off"}}'
}
function powershell {
    $a=@($args); $remote=$a[$a.IndexOf('-RemotePath')+1]
    if(($a -join ' ') -match 'fetch_wii_stage_file') {
        $out=$a[$a.IndexOf('-OutFile')+1]
        $value=$script:card[$remote]
        if($script:tamper -and $script:puts -gt 0) { $value=$value.Replace('"service_observer_int": 1','"service_observer_int": 0') }
        [IO.File]::WriteAllText($out,$value)
    } else {
        $local=$a[$a.IndexOf('-LocalPath')+1]
        $script:card[$remote]=[IO.File]::ReadAllText($local); $script:puts++
    }
    $global:LASTEXITCODE=0
}
'''
    output = run(helper + mock + "\nSet-StagedServiceDiagnosticConfig -WiiHost localhost -Port 1 -ConnectTimeoutSeconds 1\n" + r'''
if($script:puts -ne 2) { throw 'mirrors missing' }
foreach($value in $script:card.Values) {
    $cfg=$value | ConvertFrom-Json
    if($cfg.sentinel -ne 42) { throw 'settings lost' }
    Assert-ServiceDiagnosticConfig $cfg
    foreach($entry in (Get-ServiceDiagnosticSettings).GetEnumerator()) {
        $bad=$value | ConvertFrom-Json
        $bad.debug.PSObject.Properties.Remove($entry.Key)
        $caught=$false
        try { Assert-ServiceDiagnosticConfig $bad } catch { $caught=$true }
        if(-not $caught) { throw "missing setting accepted: $($entry.Key)" }
    }
}
$script:tamper=$true; $caught=$false
try { Set-StagedServiceDiagnosticConfig -WiiHost localhost -Port 1 -ConnectTimeoutSeconds 1 } catch { $caught=$true }
if(-not $caught) { throw 'bad readback accepted' }
'PASS config mirrors, preservation, tamper rejection'
''')
    assert 'PASS config' in output, output
    print('PASS staged ServiceDiagnostic: forwarding, 600s, exposure, config readback, 6-token/511-byte limits')


if __name__ == '__main__':
    main()
