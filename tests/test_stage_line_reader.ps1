$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$source = Get-Content (Join-Path $repo 'tools/cw_saves_common.ps1') -Raw
$code = [regex]::Match($source, "(?s)Add-Type -TypeDefinition @'\r?\n(using System;.*?public static class CwStageLineReader.*?)\r?\n'@").Groups[1].Value
if (-not $code) { throw 'Production line-reader source missing' }
$code += @'
public static class CwLineSocketTest {
    public static object[] Run(byte[] payload, int split) {
        var listener = new System.Net.Sockets.TcpListener(System.Net.IPAddress.Loopback, 0);
        listener.Start();
        using (var client = new System.Net.Sockets.TcpClient()) {
            client.Connect((System.Net.IPEndPoint)listener.LocalEndpoint);
            using (var peer = listener.AcceptTcpClient()) {
                listener.Stop();
                var writer = System.Threading.Tasks.Task.Run(() => {
                    var output = peer.GetStream();
                    output.Write(payload, 0, split);
                    System.Threading.Thread.Sleep(5);
                    output.Write(payload, split, payload.Length - split);
                    peer.Client.Shutdown(System.Net.Sockets.SocketShutdown.Send);
                });
                var input = client.GetStream();
                string value = null;
                bool failed = false;
                try { value = CwStageLineReader.Read(input); }
                catch (System.IO.IOException) { failed = true; }
                writer.Wait();
                var tail = new System.IO.MemoryStream();
                input.CopyTo(tail);
                return new object[] { value, failed, tail.ToArray() };
            }
        }
    }
}
'@
Add-Type -TypeDefinition $code
function Read-Reference([IO.Stream]$stream) {
    $bytes = [Collections.Generic.List[byte]]::new()
    $one = [byte[]]::new(1)
    while ($true) {
        $n = $stream.Read($one, 0, 1)
        if ($n -le 0) { throw 'stage server closed while reading line' }
        if ($one[0] -eq 10) { break }
        if ($one[0] -ne 13) { $bytes.Add($one[0]) }
    }
    [Text.Encoding]::UTF8.GetString($bytes.ToArray())
}
$cases = @(
    [byte[]]@(10,88), [byte[]]@(13,10,88), [byte[]]@(65,13,66,10,88),
    [byte[]]@(195,169,10,88), [byte[]]@(255,10,88), [byte[]]@(65),
    [byte[]]@(0,65,10,66,10)
)
$rng = [Random]::new(173)
for ($i = 0; $i -lt 50; $i++) {
    $data = [byte[]]::new(256 + $i)
    $rng.NextBytes($data)
    $cases += ,$data
}
$cases += ,([Text.Encoding]::UTF8.GetBytes(('x' * 16385) + "`r`nsecond`n"))
foreach ($data in $cases) {
    $a = [IO.MemoryStream]::new($data)
    $b = [IO.MemoryStream]::new($data)
    $ae = $false; $be = $false; $av = $null; $bv = $null
    try { $av = Read-Reference $a } catch { $ae = $true }
    try { $bv = [CwStageLineReader]::Read($b) } catch { $be = $true }
    if ($ae -ne $be -or (-not $ae -and $av -cne $bv) -or $a.Position -ne $b.Position) {
        throw 'Reader changed decoded bytes, EOF behavior, or consumed reply boundary'
    }
    foreach ($split in @(0, [int]($data.Length / 2), $data.Length)) {
        $live = [CwLineSocketTest]::Run($data, $split)
        $tail = [byte[]]$live[2]
        $expectedTail = @($data | Select-Object -Skip $a.Position)
        if ($ae -ne $live[1] -or (-not $ae -and $av -cne $live[0]) -or
            [Convert]::ToBase64String($tail) -cne [Convert]::ToBase64String([byte[]]$expectedTail)) {
            throw 'Socket peek/drain changed framing, decoded value, or unread tail'
        }
    }
    $a.Dispose(); $b.Dispose()
}
$data = [Text.Encoding]::UTF8.GetBytes(('x' * 131072) + "`n")
$a = [IO.MemoryStream]::new($data); $b = [IO.MemoryStream]::new($data)
$timer = [Diagnostics.Stopwatch]::StartNew()
$av = Read-Reference $a; $oldMs = $timer.Elapsed.TotalMilliseconds
$timer.Restart()
$bv = [CwStageLineReader]::Read($b); $newMs = $timer.Elapsed.TotalMilliseconds
if ($av -cne $bv -or $a.Position -ne $b.Position) { throw 'Large reply differs' }
$a.Dispose(); $b.Dispose()
Write-Host "PASS line-reader equivalence ($($cases.Count) cases, $($cases.Count * 3) fragmented socket cases + 128 KiB); reference=${oldMs}ms compiled=${newMs}ms"
