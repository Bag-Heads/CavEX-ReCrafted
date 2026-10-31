<#
.SYNOPSIS
    Read-only inventory of the Wii card's world directories over the stage
    server's CWSAVES1 verb.

.DESCRIPTION
    Mirrors tools/get_wii_stage_manifest.ps1's client shape (connect with
    timeout, CWHELLO1, CWAUTH1, Assert-PassJson, token resolution) and adds
    the paged-inventory consistency rules from design S3: every page must
    agree on `total`, the accumulated name_hex prefix must reproduce each
    page's prefix_fnv1a, the pages must be contiguous, and the walk must
    reach next = -1. An inventory that fails any of those is REJECTED
    (exit 3) -- a batch is never planned, let alone executed, against a
    listing that could have been taken across a directory that changed.

    Writes tmp/wii_saves/inventory_<stamp>.tsv and .json.

    This tool NEVER moves, deletes or writes anything on the card. With
    -WithNames it additionally reads each world's level.dat through
    CWSAVES1's `meta` mode -- on the connection already open, addressed by
    name_hex -- and decodes the display name with the bounded gunzip + NBT
    walk in cw_saves_common.ps1. It does NOT use CWFETCH1 for that, and
    cw_saves_meta_path() (an earlier fetch-side predicate) no longer exists:
    fetch_file's allow-list is untouched by this work.

.PARAMETER Keepers
    Also print the keeper CANDIDATE shortlist (design S6). The tool only ever
    PROPOSES; the maintainer decides, by exact inventory directory name.

.EXAMPLE
    pwsh -File tools/list_wii_saves.ps1 -WiiHost 192.0.2.72 -WithNames -Keepers
#>
param(
    [string]$WiiHost = "",
    [int]$Port = 39019,
    [ValidateSet('saves','saves_app','archive')]
    [string]$Root = 'saves',
    [string]$ArchiveBatch = "",
    [ValidateRange(1,256)]
    [int]$PageSize = 128,
    [switch]$WithNames,
    [switch]$Summary,
    [switch]$Keepers,
    [string]$OutDir = "",
    [string]$StageToken = "",
    [int]$ConnectTimeoutSeconds = 60,
    [string]$LockPath = "C:/tmp/cw-rig-lock",
    [string]$LockHeldBy = "",
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'cw_saves_common.ps1')

if($SelfTest) {
    Invoke-CwSavesSelfTest
    exit 0
}
if([string]::IsNullOrWhiteSpace($WiiHost)) {
    Write-CwFail "-WiiHost is required unless -SelfTest is used"
    exit $script:CwSavesExit.Refused
}

$rootToken = $Root
if($Root -eq 'archive' -and -not [string]::IsNullOrWhiteSpace($ArchiveBatch)) {
    if(-not (Test-CwBatchId $ArchiveBatch)) {
        Write-CwFail "-ArchiveBatch '$ArchiveBatch' is not a valid batch id"
        exit $script:CwSavesExit.Refused
    }
    $rootToken = "archive/$ArchiveBatch"
}

$lock = $null
$client = $null
try {
    $lock = Enter-CwRigLock -LockPath $LockPath -LockHeldBy $LockHeldBy `
        -Owner ("list_wii_saves.ps1 / $env:USERNAME / " + (Get-Date -Format 'o'))
} catch {
    Write-CwFail $_.Exception.Message
    exit $script:CwSavesExit.Refused
}

try {
    $client = Connect-CwStage -RemoteHost $WiiHost -RemotePort $Port -TimeoutSeconds $ConnectTimeoutSeconds
    $stream = $client.GetStream()
    $stream.ReadTimeout = 30000
    $stream.WriteTimeout = 30000

    $need = @('saves-list')
    if($WithNames) { $need += 'saves-meta' }   # it uses `meta`, so it asks for `meta`
    try {
        $hs = Invoke-CwHandshake -Stream $stream -RequireCapabilities $need `
            -Token (Get-CwStageToken -Explicit $StageToken -RepoRoot $RepoRoot)
    } catch {
        if($_.Exception.Message -match 'CW_SAVES_SERVER_TOO_OLD') {
            Write-CwFail $_.Exception.Message
            exit $script:CwSavesExit.ServerTooOld
        }
        throw
    }
    Write-Host "[hello] $($hs.HelloLine)"
    if(-not $hs.TokenProvisioned) {
        Write-Host "[stage-auth] SOFT-LAUNCH: the card has no stage_token.txt. Listing is allowed; MOVE/RESTORE are NOT (token_required)."
    }

    $mode = if($Summary) { "summary" } else { "plain" }
    $pageSizeEffective = if($Summary) { [Math]::Min($PageSize, 32) } else { $PageSize }
    try {
        $inv = Get-CwSavesInventory -Stream $stream -Root $rootToken -PageSize $pageSizeEffective -Mode $mode
    } catch {
        Write-CwFail "inventory rejected: $($_.Exception.Message)"
        exit $script:CwSavesExit.InventoryInconsistent
    }
    $entries = @($inv.Entries)
    $dirs = @($entries | Where-Object { $_.is_dir })
    $unsupported = @($entries | Where-Object { -not $_.supported })
    # N6: a row the server could not stat is its own state. It is listed and
    # reported, never moved, and it does not reject the whole inventory.
    $unreadable = @($entries | Where-Object { -not $_.stat_ok })

    if($WithNames) {
        # Over the connection ALREADY OPEN, through CWSAVES1's `meta` mode.
        # The previous route opened a SECOND connection to run
        # fetch_wii_stage_file.ps1 -- against a server that services exactly
        # one client at a time and reaps an idle peer only after 20 s,
        # against that script's 5 s stream timeout -- and addressed the file
        # by PATH, which CWFETCH1 parses with sscanf("%15s %255s") and so
        # truncates at the first space. Every live world is "New World <n>",
        # so not one of them could ever be read (C9 review, H1/H2).
        foreach($e in $dirs) {
            $display = Get-CwSavesDisplayName -Stream $stream -Root $rootToken -NameHex ([string]$e.name_hex)
            Add-Member -InputObject $e -NotePropertyName 'level_name' -NotePropertyValue $display -Force
        }
        $decoded = @(@($dirs) | Where-Object { -not [string]::IsNullOrEmpty($_.level_name) }).Count
        Write-Host "[NAMES] decoded $decoded of $(@($dirs).Count) display name(s) over the single stage-server connection"
    }

    if([string]::IsNullOrWhiteSpace($OutDir)) { $OutDir = Join-Path $RepoRoot "tmp\wii_saves" }
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $tsvPath = Join-Path $OutDir "inventory_$stamp.tsv"
    $jsonPath = Join-Path $OutDir "inventory_$stamp.json"

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("name`tname_hex`tis_dir`tsupported`tbytes`tmtime_fat_local`tfiles`ttotal_bytes`tlevel_name")
    foreach($e in $entries) {
        $props = $e.PSObject.Properties.Name
        $lines.Add(( @(
            (Get-CwEntryDisplay -Entry $e)
            [string]$e.name_hex
            [string]$e.is_dir
            [string]$e.supported
            [string]$e.bytes
            [string]$e.mtime
            $(if($props -contains 'files') { [string]$e.files } else { "" })
            $(if($props -contains 'total_bytes') { [string]$e.total_bytes } else { "" })
            $(if($props -contains 'level_name' -and $null -ne $e.level_name) { [string]$e.level_name } else { "" })
        ) -join "`t"))
    }
    Set-Content -LiteralPath $tsvPath -Encoding UTF8 -Value $lines.ToArray()
    $inventoryJson = ([pscustomobject]@{
        root = $rootToken
        root_abs = [string](@($inv.Pages)[0].root_abs)
        taken_utc = (Get-Date).ToUniversalTime().ToString('o')
        server = [pscustomobject]@{
            protocol = [string]$hs.Protocol
            capabilities = @($hs.Capabilities)
            token_provisioned = [bool]$hs.TokenProvisioned
        }
        total = @($entries).Count
        directories = @($dirs).Count
        unsupported = @($unsupported).Count
        unreadable = @($unreadable).Count
        pages = @($inv.Pages)
        entries = @($entries)
    } | ConvertTo-Json -Depth 12)
    # BOM-less UTF-8 on Windows PowerShell 5.1 too (Set-Content -Encoding UTF8
    # writes a BOM there, which strict JSON readers reject).
    [System.IO.File]::WriteAllText($jsonPath, $inventoryJson, (New-Object System.Text.UTF8Encoding $false))

    Write-Host "[INVENTORY] $tsvPath"
    Write-Host "[INVENTORY] $jsonPath"
    Write-Host "[INVENTORY] root=$rootToken entries=$(@($entries).Count) directories=$(@($dirs).Count) unsupported=$(@($unsupported).Count)"
    # The np_rows cross-check: Select World builds exactly one row per
    # DIRECTORY it can stat (screen_select_world.c:189-228), so this number
    # is what the game's own list length must match.
    Write-Host "[INVENTORY] expected Select World rows (np_rows) = $(@($dirs).Count)"
    if(@($unreadable).Count -gt 0) {
        Write-Host "[UNREADABLE] the server could not stat these rows; they are listed but can never be moved, and they do not invalidate the rest of the listing:"
        foreach($u in $unreadable) { Write-Host "  - $(Get-CwEntryDisplay -Entry $u)" }
    }
    if(@($unsupported).Count -gt 0) {
        Write-Host "[UNSUPPORTED] these names cannot be moved and are left exactly where they are:"
        foreach($u in $unsupported) { Write-Host "  - $(Get-CwEntryDisplay -Entry $u)" }
    }

    if($Keepers) {
        # PROPOSES ONLY -- the maintainer decides, by exact directory name.
        Write-Host ""
        Write-Host "[KEEPER CANDIDATES] the tool proposes, the maintainer decides (design S6)."
        $byName = @($dirs | Where-Object { (Get-CwEntryDisplay -Entry $_) -match '(?i)cavewii|test.?1' })
        $bySize = @($dirs | Where-Object { $_.PSObject.Properties.Name -contains 'total_bytes' } |
                    Sort-Object { [long]$_.total_bytes } -Descending | Select-Object -First 10)
        $byFiles = @($dirs | Where-Object { $_.PSObject.Properties.Name -contains 'files' } |
                     Sort-Object { [int]$_.files } -Descending | Select-Object -First 10)
        $byNewWorld = @($dirs | Where-Object {
            $d = Get-CwEntryDisplay -Entry $_
            if($d -match '^New World ([0-9]+)$') { [int]$Matches[1] -ge 110 -and [int]$Matches[1] -le 130 } else { $false } })
        $byRenamed = @($dirs | Where-Object {
            $_.PSObject.Properties.Name -contains 'level_name' -and
            -not [string]::IsNullOrEmpty($_.level_name) -and
            $_.level_name -cne (Get-CwEntryDisplay -Entry $_) })
        foreach($grp in @(
            @{ Label = 'name matches cavewii/test1'; Items = $byName },
            @{ Label = '10 largest by total_bytes (needs -Summary)'; Items = $bySize },
            @{ Label = '10 with the most files (needs -Summary)'; Items = $byFiles },
            @{ Label = 'New World 110..130 (the "maybe 120" hint)'; Items = $byNewWorld },
            @{ Label = 'display name differs from the directory name (needs -WithNames)'; Items = $byRenamed })) {
            Write-Host "  -- $($grp.Label):"
            foreach($i in @($grp.Items)) {
                $ln = if($i.PSObject.Properties.Name -contains 'level_name') { [string]$i.level_name } else { "" }
                Write-Host ("     {0}  mtime={1}  {2}" -f (Get-CwEntryDisplay -Entry $i), $i.mtime, $ln)
            }
        }
        Write-Host "  STOP: confirm the two keepers by their EXACT directory names before any archive run."
    }
    exit $script:CwSavesExit.Ok
} catch {
    Write-CwFail $_.Exception.Message
    exit $script:CwSavesExit.ServerFail
} finally {
    if($null -ne $client) { $client.Close() }
    Exit-CwRigLock -Lock $lock
}
