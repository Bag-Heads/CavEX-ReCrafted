<#
.SYNOPSIS
    Move ONE world directory out of the Wii card's saves root into
    sd:/cw_saves_archive/<batch>/, with a write-ahead manifest that makes the
    operation recoverable after a lost reply.

.DESCRIPTION
    DRY RUN BY DEFAULT. -Execute is required to send anything.

    One world per invocation (-Name). -Keep is MANDATORY and non-empty: every
    name in it must exist in a FRESH listing taken in this same session,
    under this same still-held rig lock, or the tool aborts before sending
    anything. That is the only guard against the one loss-shaped mistake this
    design admits -- a wrong keep list -- and it is checked against reality,
    never against a cached inventory.

    Order of operations (Astra changes 1, 2 and 5):
      1. rig lock: acquire, or accept -LockHeldBy <owner> when the caller
         already holds it (NO nested acquisition).
      2. RECONCILE any unfinished intent in the manifest directory FIRST, by
         LISTING BOTH ENDS. Never a blind retry. both-present (exit 4) and
         neither-present (exit 5) STOP.
      3. handshake; refuse a server without saves-list + saves-archive.
      4. resolve and BIND the saves root (design S0/U1) -- the bound root is
         written into the manifest and sent with every request, and a restore
         always returns the world to it.
      5. fresh, consistency-checked inventory; keep-list gate; print the plan.
      6. -Execute: write the intent (fsync'd) BEFORE the request, send it,
         verify by re-listing both ends, write the result.

    This tool performs NO power action, stages no .dol, and there is no
    delete verb in the protocol it speaks. A world is displaced by exactly
    one rename and can be put back by tools/restore_wii_saves.ps1.

.EXAMPLE
    pwsh -File tools/archive_wii_saves.ps1 -WiiHost 192.0.2.72 `
         -Name "New World 152" -Keep "CaveWii Test 1","New World 120"
#>
param(
    [Parameter(Mandatory=$true)][string]$WiiHost,
    [Parameter(Mandatory=$true)][string]$Name,
    # The keep list is MANDATORY and must be non-empty -- a default of "keep
    # nothing" is how a bulk move becomes a bulk loss -- but it is enforced
    # BELOW rather than by [Parameter(Mandatory)] so the refusal is a clean
    # exit 2 instead of a prompt, and so -KeepFile can supply it instead.
    #
    # -Keep only carries MULTIPLE names when this script is invoked from
    # PowerShell directly. Across a `pwsh -File` boundary a parameter cannot
    # be repeated and a comma-joined string is ambiguous (FAT allows a comma
    # in a world name), so a caller crossing that boundary -- a runner, a
    # test -- uses -KeepFile: one exact directory name per line, which can
    # carry spaces and commas losslessly. Same trap as
    # run_staged_wii_hardware.ps1's -Set.
    [string[]]$Keep = @(),
    [string]$KeepFile = "",
    [int]$Port = 39019,
    [ValidateSet('saves','saves_app')][string]$Root = 'saves',
    [string]$Batch = "",
    [switch]$Execute,
    [switch]$SkipRootBinding,
    [ValidateRange(1,256)][int]$PageSize = 128,
    [string]$ManifestDir = "",
    [string]$StageToken = "",
    [int]$ConnectTimeoutSeconds = 60,
    [string]$LockPath = "C:/tmp/cw-rig-lock",
    [string]$LockHeldBy = "",
    [long]$ExpectWorldCountAtLeast = 100,
    # N2: the ONE way out of a sticky not_moved block other than re-issuing
    # the blocked world itself. Takes that world's exact directory name,
    # writes a logged block_cleared(reason=operator_override), and does
    # nothing else -- it never moves anything.
    #
    # T6: the name is resolved against what the LISTING recorded for the
    # block, never against a re-encoding of what was typed. A world whose
    # on-card bytes are not printable ASCII has no typeable name at all, so
    # -ResolveBlockHex addresses it by the card's own bytes -- and since
    # -ResolveBlock is by design the only way out of a block on a world
    # that has since been archived, without this such a block could only be
    # cleared by hand-editing the write-ahead record.
    [string]$ResolveBlock = "",
    [string]$ResolveBlockHex = "",
    [string]$AdoptLegacyManifest = "",
    # Screen-read corroboration for the first root bind (Astra change 1
    # signal (c)). Optional because these values come off the console, not
    # off the wire; when given, they must match or the bind STOPs.
    [string[]]$ExpectWorldName = @(),
    # T4: signal (c) samples rows that ARE worlds, found ANYWHERE in the
    # listing, rather than whatever sits in the first three directory slots
    # -- on FAT that is slot order, i.e. what the card happens to hold
    # first, not a choice. The scan is bounded: at most this many `meta`
    # reads, and it stops as soon as three rows decode. On a real card the
    # first three rows are worlds, so it costs three reads.
    [ValidateRange(1,4096)][int]$MetaSampleMaxReads = 32,
    # T5: -ExpectWorldName is resolved across the WHOLE listing. An exact
    # DIRECTORY name costs NOTHING -- the listing already carries it -- so
    # that is tried first and is the documented way to name a keeper. Only
    # a name that must be matched against a decoded LevelName costs one
    # `meta` read per row scanned, and that scan is bounded here.
    [ValidateRange(0,4096)][int]$ExpectNameMaxMetaReads = 256
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'cw_saves_common.ps1')
$lostReplyInjection = [string]$env:CW_SAVES_INJECT_LOST_REPLY

if($SkipRootBinding) {
    Write-CwFail "-SkipRootBinding is no longer a mutation escape hatch. Use a valid root_binding.json; if the binding must change, preserve the old campaign and start a new -ManifestDir."
    exit $script:CwSavesExit.RootAmbiguous
}
foreach($expectedName in @($ExpectWorldName)) {
    if([string]::IsNullOrWhiteSpace($expectedName)) {
        Write-CwFail "-ExpectWorldName contains a blank value; every supplied corroborating name must be exact and non-blank"
        exit $script:CwSavesExit.Refused
    }
}

if([string]::IsNullOrWhiteSpace($Batch)) { $Batch = New-CwBatchId }
if(-not (Test-CwBatchId $Batch)) {
    Write-CwFail "-Batch '$Batch' is not a valid batch id"
    exit $script:CwSavesExit.Refused
}
if($AdoptLegacyManifest -and ((-not (Test-CwBatchId $AdoptLegacyManifest)) -or -not $Execute)) {
    Write-CwFail "-AdoptLegacyManifest requires a valid batch id and -Execute after the operator verifies this card and batch contents"
    exit $script:CwSavesExit.Refused
}
if([string]::IsNullOrWhiteSpace($ManifestDir)) { $ManifestDir = Join-Path $RepoRoot "tmp\wii_saves" }
# T2: a DRY RUN writes NOTHING -- not the directory, not the binding, not a
# manifest line. Creating the directory here was the first of the three
# writes a dry run still made; Read-CwRootBinding and Get-CwManifestPaths
# both handle an absent directory, and every writer creates it on demand.
if($Execute) { New-Item -ItemType Directory -Force -Path $ManifestDir | Out-Null }

if(-not [string]::IsNullOrWhiteSpace($KeepFile)) {
    if(-not (Test-Path -LiteralPath $KeepFile)) {
        Write-CwFail "-KeepFile not found: $KeepFile"
        exit $script:CwSavesExit.Refused
    }
    $Keep = @(@(Get-Content -LiteralPath $KeepFile -Encoding UTF8) |
              Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
}
if(@($Keep).Count -eq 0) {
    Write-CwFail "a non-empty keep list is MANDATORY: pass -Keep <names> (from PowerShell) or -KeepFile <path> (one exact directory name per line). Refusing to plan a move with nothing declared safe."
    exit $script:CwSavesExit.Refused
}
# PROTECTED worlds are never archived or deleted by rig tooling, whatever the
# keep list says: the shipped tutorial world (the title Tutorial button opens
# saves/CavEX Tutorial, or the legacy saves/CaveWii Tutorial on 0.1.0 cards).
foreach($protected in @('CavEX Tutorial', 'CaveWii Tutorial')) {
    if(Test-CwNameEqual $protected $Name) {
        Write-CwFail "-Name '$Name' is a PROTECTED world (the shipped tutorial world); rig tooling never archives it"
        exit $script:CwSavesExit.Refused
    }
}
foreach($k in @($Keep)) {
    if([string]::IsNullOrWhiteSpace($k)) {
        Write-CwFail "the keep list contains an empty name; give the EXACT inventory directory names"
        exit $script:CwSavesExit.Refused
    }
    if(Test-CwNameEqual $k $Name) {
        Write-CwFail "-Name '$Name' is also in the keep list; refusing a self-contradictory plan"
        exit $script:CwSavesExit.Refused
    }
}

# NOTE: $nameHex is NOT derived from the operator's typed -Name. It comes
# from the server's own listing once the target is matched, so the hex
# that travels is always the card's bytes and never a re-encoding of
# what somebody typed (C9 review, L3 -- the dead pre-computation that
# used to sit here invited exactly that mistake).
$lock = $null
$client = $null
$writer = $null
$exitCode = $script:CwSavesExit.Ok

try {
    try {
        $lock = Enter-CwRigLock -LockPath $LockPath -LockHeldBy $LockHeldBy `
            -Owner ("archive_wii_saves.ps1 / $env:USERNAME / " + (Get-Date -Format 'o'))
    } catch {
        Write-CwFail $_.Exception.Message
        exit $script:CwSavesExit.Refused
    }

    $client = Connect-CwStage -RemoteHost $WiiHost -RemotePort $Port -TimeoutSeconds $ConnectTimeoutSeconds
    $stream = $client.GetStream()
    $stream.ReadTimeout = 60000
    $stream.WriteTimeout = 60000

    try {
        $hs = Invoke-CwHandshake -Stream $stream -RequireCapabilities @('saves-list','saves-archive') `
            -Token (Get-CwStageToken -Explicit $StageToken -RepoRoot $RepoRoot)
    } catch {
        if($_.Exception.Message -match 'CW_STAGE_TOKEN_LOCKED') {
            Write-CwFail $_.Exception.Message
            exit $script:CwSavesExit.ServerFail
        }
        if($_.Exception.Message -match 'CW_SAVES_SERVER_TOO_OLD') {
            Write-CwFail $_.Exception.Message
            exit $script:CwSavesExit.ServerTooOld
        }
        throw
    }
    Write-Host "[hello] $($hs.HelloLine)"
    if($lostReplyInjection) {
        Write-Host "[diagnostic provenance] CW_SAVES_INJECT_LOST_REPLY=$lostReplyInjection"
    }
    if(-not $hs.TokenProvisioned) {
        # Stated up front rather than after a confusing server refusal: in
        # soft-launch every subnet peer is auto-authed, so MOVE is refused
        # server-side with token_required (Astra Q3).
        Write-Host "[stage-auth] WARNING: the card has no stage_token.txt. The server will refuse MOVE with token_required; the plan below is still printed."
    }

    # ---- 2. Reconcile EVERY manifest in the directory first -------------
    # N1: this loop used to be `foreach($m in @(Get-CwManifestPaths ...))`,
    # the inline shape this repo documents as a trap. With TWO or more
    # manifests foreach bound the whole string[] to one variable,
    # Read-CwManifest got a space-joined path, Test-Path failed and the pass
    # silently read NOTHING -- so from the third leg of a -Batch-defaulting
    # campaign onward there was no lost-reply recovery and, worse, none of
    # the both-present / neither-present / could-not-observe STOPs that are
    # supposed to halt a 640-world run. ASSIGN FIRST; the selftest now gates
    # the shape across all four tools so it cannot be retyped.
    $manifestPath = Join-Path $ManifestDir ("manifest_{0}.jsonl" -f $Batch)
    $manifestPaths = Get-CwManifestPaths -ManifestDir $ManifestDir
    # Binding is a prerequisite for trusting ANY established campaign
    # record.  This must precede reconciliation and -ResolveBlock because
    # both append durable records in execute mode.
    $campaignBinding = Read-CwRootBinding -ManifestDir $ManifestDir
    if(@($manifestPaths).Count -gt 0 -and $null -eq $campaignBinding) {
        Write-CwFail "root_binding.json is missing while this campaign already has manifests; refusing before reconcile or block resolution"
        exit $script:CwSavesExit.RootAmbiguous
    }
    # N15: a DRY RUN is read-only with respect to the write-ahead record. It
    # still OBSERVES both ends and still refuses, it just writes nothing --
    # which also stops it consuming the block below.
    $readOnly = -not $Execute

    # A block and the record that clears it live in DIFFERENT manifests by
    # construction -- the block is written where the unresolved intent is,
    # the clear where the re-issue lands -- so blocks are resolved across
    # the UNION of the directory. T1: the whole union is read and ORDERED
    # FIRST, before anything is written, so a clear can be paired against
    # the block it actually follows rather than against a bare name_hex.
    $recordsByPath = @{}
    $unionRecords = @()
    $ordinal = 0
    foreach($mpath in @($manifestPaths)) {
        $rawRecs = Read-CwManifest -Path $mpath
        $orderedRecs = Add-CwManifestOrder -Records @($rawRecs) -Manifest $mpath -FileOrdinal $ordinal
        $recordsByPath[$mpath] = @($orderedRecs)
        $unionRecords += @($orderedRecs)
        $ordinal++
    }
    $carried = Get-CwActiveBlocks -Records @($unionRecords)
    $activeBlocks = @($carried)

    $ordinal = 0
    foreach($mpath in @($manifestPaths)) {
        $existingRecs = @($recordsByPath[$mpath])
        $lineCursor = @($existingRecs).Count
        $thisOrdinal = $ordinal
        $ordinal++
        $open = Get-CwUnfinishedIntents -Records @($existingRecs)
        if(@($open).Count -eq 0) { continue }
        Write-Host "[RECONCILE] $(@($open).Count) intent(s) in $mpath have no recorded result; listing BOTH ends (never a blind retry)."
        $writer = $null
        if(-not $readOnly) { $writer = New-CwManifestWriter -Path $mpath }
        foreach($i in @($open)) {
            if([string]$i.dir -eq 'restore') {
                $srcRoot = "archive/" + [string]$i.batch
                $dstRoot = [string]$i.root
            } else {
                $srcRoot = [string]$i.root
                $dstRoot = "archive/" + [string]$i.batch
            }
            $src = Get-CwNamePresence -Stream $stream -Root $srcRoot -NameHex ([string]$i.name_hex) -PageSize $PageSize
            $dst = Get-CwNamePresence -Stream $stream -Root $dstRoot -NameHex ([string]$i.name_hex) -PageSize $PageSize
            $verdict = Resolve-CwReconcileVerdict -Src $src -Dst $dst
            Write-Host "[RECONCILE] seq=$($i.seq) dir=$($i.dir) name_hex=$($i.name_hex) src=$src dst=$dst -> $verdict"
            if($verdict -like 'stop_*') {
                Write-CwFail ("RECONCILE STOP: " + (Get-CwVerdictMessage -Verdict $verdict -What "seq=$($i.seq) in $mpath"))
                $exitCode = Get-CwVerdictExit -Verdict $verdict
                exit $exitCode
            }
            if($verdict -eq 'not_moved') {
                $blockView = [pscustomobject]@{
                    name_hex = [string]$i.name_hex; from = [string]$i.from
                    name_display = (Get-CwPathLeaf -Path ([string]$i.from))
                    seq = [string]$i.seq; manifest = $mpath; reason = 'not_moved'
                    source = 'run'; cw_manifest = $mpath
                    cw_file_ordinal = [int]$thisOrdinal; cw_line = [int]($lineCursor + 1)
                    utc = (Get-Date).ToUniversalTime().ToString('o')
                }
                $dup = @(@($activeBlocks) | Where-Object {
                    (Test-CwNameEqual ([string]$_.name_hex) ([string]$blockView.name_hex)) -and
                    (Test-CwNameEqual ([string]$_.cw_manifest) $mpath) -and
                    ([string]$_.seq -eq [string]$blockView.seq) })
                if(-not $readOnly -and @($dup).Count -eq 0) {
                    # T3: the BLOCK is flushed BEFORE the result that closes
                    # the intent. Both are Flush($true)-per-line, so the
                    # order in this source IS the order on the platter: with
                    # the result first, a power event between the two flushes
                    # left an intent reconcile would never look at again and
                    # no block record at all -- the world silently dropped
                    # out of the campaign. The reader's result-derived
                    # backstop (Get-CwActiveBlocks) closes the other side of
                    # the same window.
                    Write-CwManifestLine -Writer $writer -Record @{
                        v = 1; op = 'block'; reason = 'not_moved'
                        name_hex = [string]$i.name_hex; from = [string]$i.from
                        name_display = [string]$blockView.name_display
                        seq = [int]$i.seq
                        utc = [string]$blockView.utc
                    }
                    $lineCursor++
                }
                if(@($dup).Count -eq 0) { $activeBlocks += $blockView }
            }
            if($readOnly) { continue }
            Write-CwManifestLine -Writer $writer -Record @{
                v = 1; op = 'result'; seq = [int]$i.seq
                status = $(if($verdict -eq 'moved') { 'PASS' } else { 'FAIL' })
                reason = "reconciled_$verdict"
                src_observed = $src; dst_observed = $dst
                utc = (Get-Date).ToUniversalTime().ToString('o')
            }
            $lineCursor++
        }
        if($null -ne $writer) { Close-CwManifestWriter -Writer $writer; $writer = $null }
    }

    # N2: an explicit, logged resolution. This is the only action other than
    # re-issuing the world itself that clears a block, and it moves nothing.
    if((-not [string]::IsNullOrWhiteSpace($ResolveBlock)) -or
       (-not [string]::IsNullOrWhiteSpace($ResolveBlockHex))) {
        if((-not [string]::IsNullOrWhiteSpace($ResolveBlock)) -and
           (-not [string]::IsNullOrWhiteSpace($ResolveBlockHex))) {
            Write-CwFail "pass -ResolveBlock OR -ResolveBlockHex, never both: they name the same thing two ways, and a disagreement between them has no safe resolution."
            $exitCode = $script:CwSavesExit.Refused
            exit $exitCode
        }
        $asked = $(if([string]::IsNullOrWhiteSpace($ResolveBlockHex))
                   { "-ResolveBlock '$ResolveBlock'" }
                   else { "-ResolveBlockHex '$ResolveBlockHex'" })
        # T6 / N9: the typed name is matched against the display name the
        # LISTING recorded for this block. It is NEVER re-encoded --
        # ConvertTo-CwHex(UTF8($typed)) is a guess about the card's
        # encoding, and this repo states in writing that it cannot know it.
        $resolution = Resolve-CwBlockTarget -Blocks @($activeBlocks) -Typed $ResolveBlock -Hex $ResolveBlockHex
        $known = @()
        foreach($b in @($activeBlocks)) {
            $known += ("{0} (name_hex={1}, seq {2})" -f
                       (Get-CwBlockDisplayName -Block $b), [string]$b.name_hex, [string]$b.seq)
        }
        $catalogue = $(if(@($known).Count -eq 0) { "there are no active blocks" }
                       else { "active blocks: " + ($known -join '; ') })
        if([string]$resolution.Status -eq 'bad_hex') {
            Write-CwFail "$asked is not an even-length hex string; it must be a name_hex exactly as the listing printed it."
            $exitCode = $script:CwSavesExit.Refused
            exit $exitCode
        }
        if([string]$resolution.Status -eq 'absent') {
            Write-CwFail "$asked does not match any active block. A name is matched against what the LISTING recorded for the block, never against a re-encoding of what you typed -- $catalogue. For a world whose name is not printable ASCII, clear it by its bytes with -ResolveBlockHex <name_hex>."
            $exitCode = $script:CwSavesExit.Refused
            exit $exitCode
        }
        if([string]$resolution.Status -eq 'ambiguous') {
            Write-CwFail "$asked matches more than one blocked world, and this tool does not guess which -- $catalogue. Disambiguate with -ResolveBlockHex <name_hex>."
            $exitCode = $script:CwSavesExit.Refused
            exit $exitCode
        }
        $hit = @($resolution.Blocks)
        if($readOnly) {
            Write-Host "[DRY RUN] $asked would clear $(@($hit).Count) block(s); pass -Execute to record it."
            exit $script:CwSavesExit.Ok
        }
        foreach($b in @($hit)) {
            $w2 = New-CwManifestWriter -Path ([string]$b.manifest)
            try {
                # T1: the clear NAMES the block it clears. Without the
                # pairing a clear was a bare name_hex, so it pre-cleared
                # every future block for that world as well as the one the
                # operator meant.
                Write-CwManifestLine -Writer $w2 -Record @{
                    v = 1; op = 'block_cleared'; name_hex = [string]$b.name_hex
                    block_manifest = [string]$b.manifest; block_seq = [string]$b.seq
                    block_utc = (Get-CwRecordUtcKey -Value $b.utc)
                    reason = 'operator_override'; operator = $env:USERNAME
                    note = $asked
                    utc = (Get-Date).ToUniversalTime().ToString('o')
                }
            } finally { Close-CwManifestWriter -Writer $w2 }
            Write-Host "[BLOCK CLEARED] $(Get-CwBlockDisplayName -Block $b) (name_hex=$($b.name_hex), seq $($b.seq)) -- operator_override, recorded in $($b.manifest)"
        }
        exit $script:CwSavesExit.Ok
    }

    # Before any archive plan, account for EVERY batch on the card. One
    # consistency-checked archive-root inventory is enough; never infer that
    # a missing manifest means an empty batch.
    try {
        $archiveInventory = Get-CwSavesInventory -Stream $stream -Root 'archive' -PageSize $PageSize
    } catch {
        if($_.Exception.Message -match '"reason"\s*:\s*"root_missing"') {
            # The first archive MOVE creates this root. A missing root is
            # therefore a verified empty archive, not an inventory failure.
            $archiveInventory = [pscustomobject]@{ Entries = @() }
        } elseif($_.Exception.Message -match '"reason"\s*:\s*"root_error"') {
            Write-CwFail "ORPHAN GUARD: sd:/cw_saves_archive unreadable, not missing; $($_.Exception.Message)"
            exit $script:CwSavesExit.OrphanGuard
        } else {
            Write-CwFail "ORPHAN GUARD: cannot list sd:/cw_saves_archive: $($_.Exception.Message)"
            exit $script:CwSavesExit.OrphanGuard
        }
    }
    $cardBatches = @{}
    $batchRootEntries = @{}
    $issues = @()
    $guardBinding = $campaignBinding
    $guardIdentity = if($null -ne $guardBinding) { [string]$guardBinding.bound_utc } else { "" }
    if($null -eq $guardBinding -and (@($manifestPaths).Count -gt 0 -or @($archiveInventory.Entries).Count -gt 0)) {
        $issues += "root_binding.json is missing while this campaign already has manifest or archive evidence; recover the trusted binding or begin a new campaign in a fresh -ManifestDir"
    }
    foreach($e in @($archiveInventory.Entries)) {
        $id = Get-CwEntryDisplay -Entry $e
        if(-not (Test-CwEntryIsDir -Entry $e)) {
            $issues += "archive root entry '$id' is not a confirmed batch directory"
            continue
        }
        $cardBatches[$id.ToLowerInvariant()] = $id
        $batchRootEntries[$id.ToLowerInvariant()] = $e
    }
    $manifestBatches = @{}
    foreach($mpath in @($manifestPaths)) {
        $id = [System.IO.Path]::GetFileNameWithoutExtension($mpath).Substring('manifest_'.Length)
        $manifestBatches[$id.ToLowerInvariant()] = $true
        $recs = Read-CwManifest -Path $mpath
        $begins = @($recs | Where-Object { $_.op -eq 'batch_begin' })
        $archiveIntents = @($recs | Where-Object { $_.op -eq 'intent' -and $_.dir -eq 'archive' })
        if(@($begins).Count -eq 0 -and @($archiveIntents).Count -eq 0 -and -not $cardBatches.ContainsKey($id.ToLowerInvariant())) {
            # A block-only record is not an archive batch manifest.
        } elseif(@($begins).Count -eq 0 -or -not ($begins[-1].PSObject.Properties.Name -contains 'card_identity') -or [string]$begins[-1].card_identity -eq '') {
            if($AdoptLegacyManifest -ne $id) {
                $issues += "manifest batch '$id' has no card_identity; verify the card and batch contents, then run archive_wii_saves.ps1 -WiiHost <host> -Name <next-world> -Keep <keeper> -ManifestDir <dir> -AdoptLegacyManifest $id -Execute"
            }
        } else {
            foreach($begin in @($begins | Where-Object { $_.PSObject.Properties.Name -contains 'card_identity' })) {
                $manifestIdentity = [string]$begin.card_identity
                if(-not [string]::Equals($manifestIdentity, $guardIdentity, [System.StringComparison]::Ordinal)) {
                    $issues += "manifest batch '$id' card_identity '$manifestIdentity' differs from current binding '$guardIdentity'"
                }
            }
        }
        $open = Get-CwUnfinishedIntents -Records @($recs)
        foreach($i in @($open)) {
            $issues += "unaccounted intent batch '$id' seq=$($i.seq) name_hex=$($i.name_hex) in $mpath"
        }
        $moved = @{}
        $restored = @{}
        $intents = @{}
        foreach($r in @($recs)) {
            if($r.op -eq 'intent') { $intents[[string]$r.seq] = $r }
            if($r.op -ne 'result' -or $r.status -ne 'PASS') { continue }
            $key = [string]$r.seq
            if(-not $intents.ContainsKey($key)) { continue }
            $i = $intents[$key]
            if($i.dir -eq 'archive') { $moved[[string]$i.name_hex] = $true; $restored.Remove([string]$i.name_hex) | Out-Null }
            if($i.dir -eq 'restore') { $restored[[string]$i.name_hex] = $true }
        }
        if($cardBatches.ContainsKey($id.ToLowerInvariant())) {
            try {
                $batchInventory = Get-CwSavesInventory -Stream $stream -Root ("archive/$id") -PageSize $PageSize
                if(@($batchInventory.Entries).Count -eq 0) {
                    $pages = @($batchInventory.Pages)
                    $rootEntry = $batchRootEntries[$id.ToLowerInvariant()]
                    if($pages.Count -ne 1 -or -not ($pages[0].PSObject.Properties.Name -contains 'count') -or
                       [int]$pages[0].count -ne 0 -or -not ($pages[0].PSObject.Properties.Name -contains 'total') -or
                       [int]$pages[0].total -ne 0 -or -not ($rootEntry.PSObject.Properties.Name -contains 'bytes') -or
                       [long]$rootEntry.bytes -ne 0) {
                        throw "empty batch listing disagrees with CWSAVES1 trailer count/total or archive-root directory bytes"
                    }
                }
                $onCard = @{}
                foreach($entry in @($batchInventory.Entries)) {
                    $hex = [string]$entry.name_hex
                    if(-not (Test-CwEntryIsDir -Entry $entry)) {
                        $issues += "batch '$id' name_hex=$hex is not a confirmed world directory"
                    } else {
                        $onCard[$hex] = $true
                        $accountedIntent = @($open | Where-Object { [string]$_.dir -eq 'archive' -and [string]$_.name_hex -eq $hex }).Count -gt 0
                        $accountedBlock = @($activeBlocks | Where-Object { [string]$_.name_hex -eq $hex -and [string]$_.manifest -eq $mpath }).Count -gt 0
                        if((-not $moved.ContainsKey($hex) -or $restored.ContainsKey($hex)) -and
                           -not $accountedIntent -and -not $accountedBlock) {
                            $issues += "batch '$id' name_hex=$hex directory on card has no unrestored PASS archive record in manifest"
                        }
                    }
                }
                foreach($hex in @($moved.Keys)) {
                    if(-not $restored.ContainsKey($hex) -and -not $onCard.ContainsKey($hex)) {
                        $issues += "batch '$id' name_hex=$hex PASS archive record in manifest has no directory on card"
                    }
                }
            } catch {
                $issues += "batch '$id' contents could not be listed: $($_.Exception.Message)"
            }
        }
        if(-not $cardBatches.ContainsKey($id.ToLowerInvariant())) {
            foreach($hex in @($moved.Keys)) {
                if(-not $restored.ContainsKey($hex)) {
                    $issues += "manifest batch '$id' records MOVE name_hex=$hex but its archive directory is absent"
                }
            }
        }
    }
    foreach($id in @($cardBatches.Values)) {
        if(-not $manifestBatches.ContainsKey($id.ToLowerInvariant())) {
            $issues += "orphan archive batch '$id' has no manifest"
        }
    }
    if(@($issues).Count -gt 0) {
        Write-CwFail ("ORPHAN GUARD: " + ($issues -join '; ') +
                      ". Recover a batch without a manifest with restore_wii_saves.ps1 -FromArchiveBatch <id> -Execute; recover an unfinished manifest with the existing reconcile path after checking both ends. No archive plan or MOVE is allowed until every batch is accounted for.")
        exit $script:CwSavesExit.OrphanGuard
    }
    if($AdoptLegacyManifest) {
        $adoptPath = Join-Path $ManifestDir ("manifest_{0}.jsonl" -f $AdoptLegacyManifest)
        if(-not (Test-Path -LiteralPath $adoptPath) -or -not $cardBatches.ContainsKey($AdoptLegacyManifest.ToLowerInvariant())) {
            Write-CwFail "ORPHAN GUARD: legacy batch '$AdoptLegacyManifest' must have both a manifest and a card directory"
            exit $script:CwSavesExit.OrphanGuard
        }
        $adoptRecs = Read-CwManifest -Path $adoptPath
        $adoptBegins = @($adoptRecs | Where-Object { $_.op -eq 'batch_begin' })
        if(@($adoptBegins).Count -eq 0 -or ($adoptBegins[-1].PSObject.Properties.Name -contains 'card_identity')) {
            Write-CwFail "ORPHAN GUARD: batch '$AdoptLegacyManifest' is not an identity-free legacy manifest"
            exit $script:CwSavesExit.OrphanGuard
        }
        $adoptWriter = New-CwManifestWriter -Path $adoptPath
        try {
            Write-CwManifestLine -Writer $adoptWriter -Record @{
                v = 1; op = 'batch_begin'; batch = $AdoptLegacyManifest
                root = [string]$adoptBegins[-1].root; card_identity = $guardIdentity
                legacy_adopted = $true; utc = (Get-Date).ToUniversalTime().ToString('o')
            }
        } finally { Close-CwManifestWriter -Writer $adoptWriter }
        Write-Host "[ORPHAN GUARD] adopted legacy manifest batch '$AdoptLegacyManifest' with card_identity '$guardIdentity'."
        exit $script:CwSavesExit.Ok
    }
    Write-Host "[ORPHAN GUARD] batch names, contents, card identity, and intents reconciled."

    if(@($activeBlocks).Count -gt 0) {
        $blockers = @()
        foreach($b in @($activeBlocks)) {
            $blockers += ("{0} (name_hex={1}, seq {2}, {3}, {4})" -f
                          (Get-CwBlockDisplayName -Block $b), [string]$b.name_hex,
                          [string]$b.seq, [string]$b.reason, [string]$b.manifest)
        }
        Write-Host "[BLOCK] unresolved halt(s): $($blockers -join '; ')"
    }
    $existing = @()
    if(Test-Path -LiteralPath $manifestPath) {
        $existing = Read-CwManifest -Path $manifestPath
    }

    # ---- 4. Bind the saves root ONCE, then verify it BY IDENTITY --------
    # A count threshold cannot survive the batch that shrinks the count: the
    # goal is 646 worlds -> 2, so the old rule exited 8 for the tail of the
    # run and forced -SkipRootBinding exactly when the keepers were among
    # the last few directories (C9 review, H4). The binding is therefore
    # made once, persisted with its evidence, and re-verified against the
    # one thing the batch never removes -- the keep set.
    $rootToken = $Root
    $pendingBinding = $null
    $binding = Read-CwRootBinding -ManifestDir $ManifestDir
    $keepHex = @()
    foreach($k in @($Keep)) { $keepHex += (ConvertTo-CwHex -Bytes ([System.Text.Encoding]::UTF8.GetBytes($k))) }

    if($SkipRootBinding) {
        Write-Host "[ROOT] -SkipRootBinding: '$Root' is taken on the caller's word (it is still written into the manifest and sent with every request)."
    } elseif($null -ne $binding) {
        if([string]$binding.root -ne $Root) {
            Write-CwFail "ROOT BINDING STOP: this manifest directory is bound to root '$($binding.root)' but -Root '$Root' was given. One clean-up campaign binds one root."
            $exitCode = $script:CwSavesExit.RootAmbiguous
            exit $exitCode
        }
        $recordedKeep = @($binding.keep_hex)
        $sameKeep = (@($recordedKeep).Count -eq @($keepHex).Count)
        if($sameKeep) {
            foreach($h in @($keepHex)) {
                if(-not (@($recordedKeep) | Where-Object { Test-CwNameEqual $_ $h })) { $sameKeep = $false }
            }
        }
        if(-not $sameKeep) {
            # C15-4: never tell the operator to DELETE anything in this
            # directory. The manifest_*.jsonl files beside the binding are
            # the only record of what is in the archive and what is halted,
            # and an operator who has just been told to delete one file is
            # one keystroke from deleting the directory.
            Write-CwFail ("ROOT BINDING STOP: the keep set does not match the one recorded when this root was bound. The keep set IS the binding evidence -- changing what you promised to protect invalidates it. To rebind DELIBERATELY, either RENAME the binding aside (" +
                          (Join-Path $ManifestDir 'root_binding.json') +
                          " -> root_binding.json.superseded-<stamp>) or start a fresh campaign with -ManifestDir <new dir>. Do NOT delete anything in this directory: the manifest_*.jsonl files beside the binding are the ONLY record of what is displaced into the archive and what is halted.")
            $exitCode = $script:CwSavesExit.RootAmbiguous
            exit $exitCode
        }
        try {
            $probe = Get-CwSavesInventory -Stream $stream -Root $rootToken -PageSize $PageSize
        } catch {
            Write-CwFail "inventory rejected while re-verifying the bound root: $($_.Exception.Message)"
            $exitCode = $script:CwSavesExit.InventoryInconsistent
            exit $exitCode
        }
        $probeAbs = [string](@($probe.Pages)[0].root_abs)
        if($probeAbs -ne [string]$binding.root_abs) {
            Write-CwFail "ROOT BINDING STOP: '$rootToken' now resolves to '$probeAbs' but was bound to '$($binding.root_abs)'."
            $exitCode = $script:CwSavesExit.RootAmbiguous
            exit $exitCode
        }
        $probeHex = @()
        foreach($e in @($probe.Entries)) { $probeHex += [string]$e.name_hex }
        $missing = @()
        foreach($h in @($recordedKeep)) {
            if(-not (@($probeHex) | Where-Object { Test-CwNameEqual $_ $h })) { $missing += $h }
        }
        if(@($missing).Count -gt 0) {
            Write-CwFail "ROOT BINDING STOP: the bound root no longer holds every keeper recorded at bind time (missing name_hex: $($missing -join ', ')). This is NOT the tree that was bound."
            $exitCode = $script:CwSavesExit.RootAmbiguous
            exit $exitCode
        }
        Write-Host "[ROOT] '$rootToken' ($probeAbs) re-verified by identity: same root_abs, and all $(@($recordedKeep).Count) recorded keeper(s) still present. No count is involved."
    } else {
        $countByRoot = @{}
        foreach($cand in @('saves','saves_app')) {
            try {
                $inv = Get-CwSavesInventory -Stream $stream -Root $cand -PageSize $PageSize
                $countByRoot[$cand] = @(@($inv.Entries) | Where-Object { $_.is_dir }).Count
            } catch {
                $countByRoot[$cand] = -1
            }
        }
        $evidence = "count: saves=$($countByRoot['saves']) saves_app=$($countByRoot['saves_app']) (threshold $ExpectWorldCountAtLeast)"
        $plausible = @(@('saves','saves_app') | Where-Object { $countByRoot[$_] -ge $ExpectWorldCountAtLeast })
        if(@($plausible).Count -ne 1) {
            Write-CwFail ("ROOT BINDING STOP: exactly one candidate root may hold >= $ExpectWorldCountAtLeast world directories at BIND time; got $(@($plausible).Count) ($evidence).")
            $exitCode = $script:CwSavesExit.RootAmbiguous
            exit $exitCode
        }
        if($plausible[0] -ne $Root) {
            Write-CwFail "ROOT BINDING STOP: -Root '$Root' was given but the counts point at '$($plausible[0])' ($evidence)."
            $exitCode = $script:CwSavesExit.RootAmbiguous
            exit $exitCode
        }
        try {
            $probe = Get-CwSavesInventory -Stream $stream -Root $rootToken -PageSize $PageSize
        } catch {
            Write-CwFail "inventory rejected while binding the root: $($_.Exception.Message)"
            $exitCode = $script:CwSavesExit.InventoryInconsistent
            exit $exitCode
        }
        $probeAbs = [string](@($probe.Pages)[0].root_abs)
        # ---- Astra change 1: COUNT ALONE IS INSUFFICIENT ----------------
        # "corroborate it with current game configuration and identifiable
        # world metadata. Stop on ambiguity." The count above is signal (b);
        # (a) and (c) follow, both on the connection already open, and ALL
        # THREE must agree or the bind STOPs (C11 re-review, N3).

        # (a) the game's own configuration. sd:/config.json has been in the
        # stage server's manifest allow-list since 2026-09-17 and `fetch` is
        # advertised, so this widens nothing.
        # T7: "could not READ the config" and "the config has no paths key"
        # are different facts and are kept apart. The first is ambiguity and
        # STOPs; the second is the GAME's documented default -- the game
        # reads paths.worlds with config_read_string(..., "saves")
        # (screen_select_world.c:187, :630, :668, :710;
        # screen_create_world.c:261) -- so a card with no paths key is a
        # card whose worlds live in "saves", and the tool must agree with
        # the game rather than send the operator hunting a fetch failure
        # that never happened.
        $pathsWorlds = ""
        $pathsWorldsPresent = $false
        $configRead = $false
        $configErr = ""
        try {
            try {
                $cfgBytes = Get-CwStageFetchBytes -Stream $stream -RemotePath 'sd:/config.json' -MaxBytes 262144
            } catch {
                # Fresh cards ship defaults only; Stage reports open_failed for absence.
                # Authentication, read, and parse failures remain ambiguous.
                if($_.Exception.Message -notmatch '^fetch failed:.*"reason"\s*:\s*"(?:not_found|open_failed)"') { throw }
                $cfgBytes = Get-CwStageFetchBytes -Stream $stream -RemotePath 'sd:/config_wii.json' -MaxBytes 262144
            }
            $cfg = [System.Text.Encoding]::UTF8.GetString($cfgBytes) | ConvertFrom-Json
            $configRead = $true
            if(($cfg.PSObject.Properties.Name -contains 'paths') -and ($null -ne $cfg.paths) -and
               ($cfg.paths.PSObject.Properties.Name -contains 'worlds')) {
                $pathsWorlds = [string]$cfg.paths.worlds
                $pathsWorldsPresent = $true
            }
        } catch {
            $configErr = $_.Exception.Message
        }
        if(-not $configRead) {
            Write-CwFail ("ROOT BINDING STOP: signal (a) could not be READ -- fetching or parsing sd:/config.json (or fresh-card config_wii.json) failed ($configErr). That is not the same as a card whose config has no paths key, and this tool will not treat it as one. Astra change 1: count alone is insufficient; stop on ambiguity.")
            $exitCode = $script:CwSavesExit.RootAmbiguous
            exit $exitCode
        }
        $pathsWorldsEvidence = $(if($pathsWorldsPresent) { "'$pathsWorlds'" }
                                 else { "(absent -- the game defaults to 'saves', screen_select_world.c:187)" })
        if(-not (Test-CwConfigWorldsMatchesRoot -PathsWorlds $pathsWorlds -Root $Root -KeyPresent:$pathsWorldsPresent)) {
            Write-CwFail ("ROOT BINDING STOP: signal (a) disagrees -- the card's config.json paths.worlds is " +
                          $pathsWorldsEvidence +
                          ", which does not corroborate root '$Root'. Astra change 1: count alone is insufficient; stop on ambiguity.")
            $exitCode = $script:CwSavesExit.RootAmbiguous
            exit $exitCode
        }

        # (c) identifiable world metadata: THREE rows that ARE worlds -- a
        # level.dat carrying both LevelName and LastPlayed -- read through
        # the meta verb on this same connection.
        #
        # T4: they are looked for ANYWHERE in the listing, not in the first
        # three directory slots. On FAT readdir order is slot order, so
        # "the first three rows" is whatever the card happens to hold first;
        # a card whose first three entries are not worlds could not be bound
        # at all, and the only escape (-SkipRootBinding) throws away all
        # three signals. The scan is bounded at -MetaSampleMaxReads reads
        # and stops as soon as three rows decode, so on a real card it costs
        # exactly three.
        $probeDirs = @($probe.Entries | Where-Object { (Test-CwEntryIsDir -Entry $_) -and $_.stat_ok })
        $metaByHex = @{}
        $metaReadsC = 0
        $samples = @()
        foreach($e in @($probeDirs)) {
            if(@($samples).Count -ge 3) { break }
            if($metaReadsC -ge $MetaSampleMaxReads) { break }
            if(-not $metaByHex.ContainsKey([string]$e.name_hex)) { $metaReadsC++ }
            $info = Get-CwRowLevelInfo -Stream $stream -Root $rootToken -Entry $e -Cache $metaByHex
            if($null -eq $info) { continue }
            if([string]::IsNullOrEmpty($info.LevelName)) { continue }
            if(-not $info.HasLastPlayed) { continue }
            $samples += [pscustomobject]@{
                dir = (Get-CwEntryDisplay -Entry $e); name_hex = [string]$e.name_hex
                level_name = [string]$info.LevelName
                has_last_played = $true
            }
        }
        if(@($samples).Count -eq 0) {
            Write-CwFail ("ROOT BINDING STOP: signal (c) disagrees -- not one of the $metaReadsC row(s) read (of $(@($probeDirs).Count) candidate directories in '$probeAbs') decoded to an identifiable world, i.e. a level.dat carrying BOTH LevelName and LastPlayed. Astra change 1 requires identifiable world metadata; stop on ambiguity. The scan is bounded at -MetaSampleMaxReads $MetaSampleMaxReads metadata read(s); if this card's worlds sit behind more unreadable rows than that, raise it -- do NOT reach for -SkipRootBinding, which discards all three signals.")
            $exitCode = $script:CwSavesExit.RootAmbiguous
            exit $exitCode
        }

        # T5: -ExpectWorldName is what the operator READS OFF THE CONSOLE --
        # in practice the keepers, which on a 646-world card are nowhere
        # near the first rows. It is therefore resolved across the WHOLE
        # listing: by exact DIRECTORY name first, which the listing already
        # carries and so costs no metadata read at all, and only then by
        # decoded LevelName under its own bounded budget.
        # C15-3: a name that more than one row answers to is AMBIGUITY, and
        # the ruling on this bind is "stop on ambiguity". Taking $hit[0] made
        # the parameter that exists to STRENGTHEN the bind hand back a pass
        # built on a world the operator had never seen: the real card is ~644
        # `New World <n>` directories whose level.dat all say "New World", so
        # the name the operator reads off the console resolved to row 1 and
        # was written into root_binding.json as permanent evidence. -Keep
        # (N12) and -Name both already STOP on a multi-row match, for the
        # same reason.
        $expectResolved = @()
        $metaReadsE = 0
        if(@($ExpectWorldName).Count -gt 0) {
            $pending = @(@($ExpectWorldName) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
            $stillPending = @()
            foreach($n in @($pending)) {
                $hit = @($probeDirs | Where-Object { Test-CwNameEqual (Get-CwEntryDisplay -Entry $_) $n })
                if(@($hit).Count -gt 1) {
                    $shown = @()
                    foreach($h in @($hit | Select-Object -First 5)) { $shown += (Get-CwEntryDisplay -Entry $h) }
                    Write-CwFail ("ROOT BINDING STOP: -ExpectWorldName '$n' matches more than one row of '$probeAbs' by directory name ($(@($hit).Count) rows: " + ($shown -join ', ') + $(if(@($hit).Count -gt 5) { ', ...' } else { '' }) + "). An ambiguous corroboration is not a corroboration; name the one you mean.")
                    $exitCode = $script:CwSavesExit.RootAmbiguous
                    exit $exitCode
                }
                if(@($hit).Count -eq 1) {
                    $expectResolved += [pscustomobject]@{
                        name = $n; matched = 'directory name'
                        dir = (Get-CwEntryDisplay -Entry $hit[0]); name_hex = [string]$hit[0].name_hex }
                } else {
                    $stillPending += $n
                }
            }
            $pending = @($stillPending)
            if(@($pending).Count -gt 0) {
                # The LevelName pass must COUNT matches, not stop at the
                # first one, or it cannot tell "the world the operator
                # means" from "one of 644 that answer to the same name". It
                # stops early the moment any name is proven ambiguous.
                $levelHits = @{}
                foreach($n in @($pending)) { $levelHits[$n] = @() }
                $scannedAll = $true
                $foundAmbiguous = $false
                foreach($e in @($probeDirs)) {
                    if($foundAmbiguous) { break }
                    if(-not $metaByHex.ContainsKey([string]$e.name_hex)) {
                        if($metaReadsE -ge $ExpectNameMaxMetaReads) { $scannedAll = $false; break }
                        $metaReadsE++
                    }
                    $info = Get-CwRowLevelInfo -Stream $stream -Root $rootToken -Entry $e -Cache $metaByHex
                    if($null -eq $info -or [string]::IsNullOrEmpty($info.LevelName)) { continue }
                    foreach($n in @($pending)) {
                        if(-not (Test-CwNameEqual ([string]$info.LevelName) $n)) { continue }
                        $levelHits[$n] += $e
                        if(@($levelHits[$n]).Count -gt 1) { $foundAmbiguous = $true }
                    }
                }
                # AMBIGUITY is reported first: the scan stops early the
                # moment any name is proven ambiguous, so a DIFFERENT name
                # may be unfinished purely because of that -- and telling
                # the operator it "was not found" would be wrong.
                foreach($n in @($pending)) {
                    $hits = @($levelHits[$n])
                    if(@($hits).Count -le 1) { continue }
                    $shown = @()
                    foreach($h in @($hits | Select-Object -First 5)) { $shown += (Get-CwEntryDisplay -Entry $h) }
                    Write-CwFail ("ROOT BINDING STOP: -ExpectWorldName '$n' matches more than one row of '$probeAbs' by decoded LevelName (at least $(@($hits).Count) after $metaReadsE metadata read(s): " + ($shown -join ', ') + "). On this card most worlds share the display name 'New World', so a console-read display name corroborates nothing -- pass the EXACT directory name from the inventory instead.")
                    $exitCode = $script:CwSavesExit.RootAmbiguous
                    exit $exitCode
                }
                foreach($n in @($pending)) {
                    $hits = @($levelHits[$n])
                    if(@($hits).Count -eq 1 -and $scannedAll) {
                        $expectResolved += [pscustomobject]@{
                            name = $n; matched = 'LevelName'
                            dir = (Get-CwEntryDisplay -Entry $hits[0]); name_hex = [string]$hits[0].name_hex }
                        continue
                    }
                    # Either nothing matched, or something did but the scan
                    # stopped before every row was read -- in which case
                    # uniqueness was never established and claiming it would
                    # be the same guess C15-3 is about.
                    $why = $(if(@($hits).Count -eq 1)
                             { "matched one row but the scan stopped after $metaReadsE of $(@($probeDirs).Count) rows, so it is not PROVEN to be the only one" }
                             else
                             { "was not found -- neither as an exact directory name anywhere in the listing, nor as a decoded LevelName within the $metaReadsE-read budget" })
                    Write-CwFail ("ROOT BINDING STOP: -ExpectWorldName '$n' $why. Pass the EXACT directory name from the inventory (it needs no metadata read and works from any row), or raise -ExpectNameMaxMetaReads (currently $ExpectNameMaxMetaReads) to at least the directory count.")
                    $exitCode = $script:CwSavesExit.RootAmbiguous
                    exit $exitCode
                }
            }
            foreach($r in @($expectResolved)) {
                Write-Host "[ROOT] -ExpectWorldName '$($r.name)' resolved by $($r.matched) to '$($r.dir)'."
            }
        }

        $evidence = "$evidence; config paths.worlds=$pathsWorldsEvidence; metadata samples=$(@($samples).Count) from $metaReadsC metadata reads over $(@($probeDirs).Count) candidate row(s), plus $metaReadsE for -ExpectWorldName"
        # The binding is NOT persisted yet: the keep-list gate below is what
        # proves the keep set is real, and a binding file recorded against a
        # keep set that does not exist would make every later invocation
        # fail the identity check for the wrong reason.
        $pendingBinding = @{
            v = 1; root = $Root; root_abs = $probeAbs
            keep = @($Keep); keep_hex = @($keepHex)
            evidence = $evidence
            config_paths_worlds = $pathsWorlds
            metadata_samples = @($samples)
            expect_world_names = @($expectResolved)
            bound_utc = (Get-Date).ToUniversalTime().ToString('o')
        }
        Write-Host "[ROOT] binding '$Root' -> '$probeAbs' on THREE agreeing signals ($evidence); it is persisted once the keep list is confirmed against a fresh listing."
    }

    # ---- 5. Fresh inventory + keep-list gate ----------------------------
    try {
        $inv = Get-CwSavesInventory -Stream $stream -Root $rootToken -PageSize $PageSize
    } catch {
        Write-CwFail "inventory rejected: $($_.Exception.Message)"
        $exitCode = $script:CwSavesExit.InventoryInconsistent
        exit $exitCode
    }
    $entries = @($inv.Entries)
    $rootAbs = [string](@($inv.Pages)[0].root_abs)
    $dirs = @($entries | Where-Object { Test-CwEntryIsDir -Entry $_ })
    # T9: a row whose stat FAILED claims no type at all -- the server emits
    # is_dir:null rather than fabricating `false` -- so it is not in $dirs,
    # which is right for the count the GAME will show. It is wrong for the
    # guards: a -Keep or -Name naming such a row was reported as "not in the
    # fresh listing" / "matched 0 directories" instead of the documented
    # "the server could not stat this row" refusal, so the N6 message the
    # runbook documents could never actually appear. The guards therefore
    # match rows that ARE or MIGHT BE directories, then refuse the
    # unreadable ones by name.
    $dirCandidates = @($entries | Where-Object { Test-CwEntryMaybeDir -Entry $_ })
    $unsupported = @($entries | Where-Object { -not $_.supported })
    $names = @()
    foreach($d in $dirs) { $names += (Get-CwEntryDisplay -Entry $d) }

    $missingKeep = @()
    $ambiguousKeep = @()
    $unreadableKeep = @()
    foreach($k in @($Keep)) {
        $hits = @($dirCandidates | Where-Object { Test-CwNameEqual (Get-CwEntryDisplay -Entry $_) $k })
        if(@($hits).Count -eq 0) { $missingKeep += $k; continue }
        # N12: the -Name target already refuses a double match; a KEEPER
        # matching two rows is the same ambiguity and is at least as
        # dangerous, because it is the name the operator is trusting to
        # survive. Impossible on FAT, reachable from a torn listing -- and
        # free to refuse.
        if(@($hits).Count -gt 1) { $ambiguousKeep += ("{0} ({1} rows)" -f $k, @($hits).Count); continue }
        # N6: a keeper whose own stat failed cannot be confirmed to be a
        # directory at all, so it cannot be confirmed safe.
        if(-not $hits[0].stat_ok) { $unreadableKeep += $k }
    }
    if(@($missingKeep).Count -gt 0) {
        Write-CwFail ("KEEP-LIST STOP: these -Keep names are not in the fresh listing of ${rootAbs}: " +
                     ($missingKeep -join ', ') + ". Nothing was sent.")
        $exitCode = $script:CwSavesExit.Refused
        exit $exitCode
    }
    if(@($ambiguousKeep).Count -gt 0) {
        Write-CwFail ("KEEP-LIST STOP: these -Keep names match more than one row: " +
                     ($ambiguousKeep -join ', ') + ". Nothing was sent.")
        $exitCode = $script:CwSavesExit.Refused
        exit $exitCode
    }
    if(@($unreadableKeep).Count -gt 0) {
        Write-CwFail ("KEEP-LIST STOP: the server could not stat these -Keep rows, so they cannot be confirmed: " +
                     ($unreadableKeep -join ', ') + ". Nothing was sent.")
        $exitCode = $script:CwSavesExit.Refused
        exit $exitCode
    }
    if($null -ne $pendingBinding) {
        # T2: gated on -Execute with everything else a dry run could write.
        # The binding's keep set is its evidence and every later invocation
        # is re-verified against it, so persisting one from a dry run locked
        # the campaign to a keep list the operator might still be editing --
        # recoverable only by deleting the file by hand.
        if($Execute) {
            $bindingPath = Write-CwRootBinding -ManifestDir $ManifestDir -Binding $pendingBinding
            Write-Host "[ROOT] binding persisted in $bindingPath; from here on it is re-verified by identity, never by a count."
        } else {
            Write-Host "[ROOT] DRY RUN: the binding above was verified but NOT persisted. -Execute is what writes it, with the keep list it was confirmed against."
        }
    }
    $target = @($dirCandidates | Where-Object { Test-CwNameEqual (Get-CwEntryDisplay -Entry $_) $Name })
    if(@($target).Count -ne 1) {
        Write-CwFail "PLAN STOP: -Name '$Name' matched $(@($target).Count) directories in the fresh listing (need exactly 1). Nothing was sent."
        $exitCode = $script:CwSavesExit.Refused
        exit $exitCode
    }
    if(-not $target[0].supported) {
        Write-CwFail "PLAN STOP: '$Name' is reported supported:false by the server -- it is left exactly where it is."
        $exitCode = $script:CwSavesExit.Refused
        exit $exitCode
    }
    # N6: an entry the server could not stat is not known to be a directory,
    # so it is listed and reported but never MOVED.
    if(-not $target[0].stat_ok) {
        Write-CwFail "PLAN STOP: the server could not stat '$Name' (stat_ok=false), so it is not known to be a world directory. It is left exactly where it is."
        $exitCode = $script:CwSavesExit.Refused
        exit $exitCode
    }
    $nameHex = [string]$target[0].name_hex

    # N2 + N9: the sticky block is enforced HERE, after the target's hex has
    # come from the server's own listing. The earlier version compared the
    # blocker against a re-encoding of what the operator typed, which is
    # wrong for any high-byte name; and it checked before the target was
    # resolved, so it had nothing authoritative to compare against.
    if(@($activeBlocks).Count -gt 0) {
        $allowed = $false
        foreach($b in @($activeBlocks)) {
            if((Get-CwBlockReissueVerdict -BlockHex ([string]$b.name_hex) -TargetHex $nameHex -Reason ([string]$b.reason)) -eq 'reissue') {
                $allowed = $true
            }
        }
        if(-not $allowed) {
            $blockers = @()
            foreach($b in @($activeBlocks)) {
                $blockers += ("{0} (name_hex={1}, {2})" -f
                              (Get-CwBlockDisplayName -Block $b), [string]$b.name_hex,
                              [string]$b.reason)
            }
            Write-CwFail ("BLOCKED: an earlier request left this campaign halted and the halt is still open: " +
                         ($blockers -join '; ') +
                         ". 'not_moved' means the world was asked to move and did not; a 'stop_*' means the card was left in a state this tool could not explain, and that one needs a HUMAN -- check the card, then clear it deliberately with -ResolveBlock '<its exact name>' or -ResolveBlockHex <name_hex>. Re-issuing the world clears a not_moved halt. Leaving any of them unresolved is how a batch loses track of what it displaced.")
            $exitCode = $script:CwSavesExit.Refused
            exit $exitCode
        }
        Write-Host "[BLOCK] this invocation re-issues the blocked world, so it is allowed through."
    }

    Write-Host ""
    Write-Host "================ PLAN ================"
    Write-Host " server      : protocol=$($hs.Protocol) token_provisioned=$($hs.TokenProvisioned)"
    Write-Host " root        : $rootToken ($rootAbs)  -- BOUND; a restore returns here"
    Write-Host " batch       : $Batch  -> $($script:CwSavesArchiveRoot)/$Batch"
    Write-Host " worlds now  : $(@($dirs).Count) directories, $(@($unsupported).Count) unsupported"
    Write-Host " MOVING      : $Name  (name_hex=$nameHex)"
    Write-Host " KEEPING     : $(@($Keep) -join ', ')"
    if(@($unsupported).Count -gt 0) {
        Write-Host " UNSUPPORTED (never touched, reported only):"
        foreach($u in $unsupported) { Write-Host "   - $(Get-CwEntryDisplay -Entry $u)" }
    }
    Write-Host " manifest    : $manifestPath"
    Write-Host " mode        : $(if($Execute) { 'EXECUTE' } else { 'DRY RUN (pass -Execute to act)' })"
    Write-Host "======================================"
    Write-Host ""

    if(-not $Execute) {
        Write-Host "[DRY RUN] nothing was sent. Re-run with -Execute to move exactly the one world above."
        exit $script:CwSavesExit.Ok
    }

    # ---- 6. Write-ahead intent, request, verified result ----------------
    $writer = New-CwManifestWriter -Path $manifestPath
    $seq = 1
    foreach($r in @($existing)) {
        if($r.PSObject.Properties.Name -contains 'seq' -and [int]$r.seq -ge $seq) { $seq = [int]$r.seq + 1 }
    }
    Write-CwManifestLine -Writer $writer -Record @{
        v = 1; op = 'batch_begin'; batch = $Batch; root = $rootToken; root_abs = $rootAbs
        card_identity = $(if($null -ne $binding) { [string]$binding.bound_utc } elseif($null -ne $pendingBinding) { [string]$pendingBinding.bound_utc } else { throw "no validated root binding identity is available for the manifest" })
        server = @{ protocol = [string]$hs.Protocol; caps = @($hs.Capabilities)
                    token_provisioned = [bool]$hs.TokenProvisioned }
        inventory_total = @($entries).Count
        keep = @($Keep)
        utc = (Get-Date).ToUniversalTime().ToString('o')
    }
    $from = "$rootAbs/$Name"
    $to = "$($script:CwSavesArchiveRoot)/$Batch/$Name"
    # INTENT FIRST, FLUSHED TO DISK, THEN the request. If the reply is lost,
    # this line is what the reconcile pass resolves against.
    Write-CwManifestLine -Writer $writer -Record @{
        v = 1; op = 'intent'; seq = $seq; dir = 'archive'; root = $rootToken
        batch = $Batch; name_hex = $nameHex; from = $from; to = $to
        utc = (Get-Date).ToUniversalTime().ToString('o')
    }

    $injectThisMove = Test-CwLostReplyInjection -Setting $lostReplyInjection -NameHex $nameHex -Leg 1
    $reply = Invoke-CwSavesMove -Stream $stream -Direction 'archive' -Root $rootToken -Batch $Batch -NameHex $nameHex -InjectLostReply:$injectThisMove
    Write-Host "[CWARCHIVE1] $($reply.Line)"
    if($reply.Json.status -ne 'PASS') {
        Write-CwManifestLine -Writer $writer -Record @{
            v = 1; op = 'result'; seq = $seq; status = 'FAIL'; reason = [string]$reply.Json.reason
            utc = (Get-Date).ToUniversalTime().ToString('o')
        }
        Write-CwFail "server refused the move: $($reply.Line)"
        $exitCode = $script:CwSavesExit.ServerFail
        exit $exitCode
    }

    # Verify by OBSERVING both ends, not by trusting the reply -- and keep
    # "I could not look" apart from "it is not there" (M4).
    $src = Get-CwNamePresence -Stream $stream -Root $rootToken -NameHex $nameHex -PageSize $PageSize
    $dst = Get-CwNamePresence -Stream $stream -Root ("archive/$Batch") -NameHex $nameHex -PageSize $PageSize
    $verdict = Resolve-CwReconcileVerdict -Src $src -Dst $dst
    if($verdict -ne 'moved') {
        # C15-1 (CRITICAL): the HALT is durable BEFORE the result that
        # closes the intent -- the same ordering T3 established for the
        # reconcile path's block, and for the same reason one level worse.
        #
        # This path used to write the `result` and only THEN look at the
        # verdict, so a stop_* printed "touch nothing, escalate", exited
        # 4/5/9, and closed its own intent. Nothing then re-opened it: the
        # reconcile pass only revisits UNFINISHED intents, and the
        # result-derived backstop's allow-list held not_moved alone. The
        # next default-Batch leg archived the next world with no [BLOCK]
        # and no [RECONCILE] line -- no crash required, every time, on the
        # documented loop. Worse, for stop_unknown and stop_both_present
        # the world is already IN the archive, where the undo path reported
        # "nothing to undo" because Get-CwIntentState only trusts a PASS.
        Write-CwManifestLine -Writer $writer -Record @{
            v = 1; op = 'block'; reason = $verdict
            name_hex = $nameHex; from = $from
            name_display = (Get-CwPathLeaf -Path $from)
            seq = $seq; src_observed = $src; dst_observed = $dst
            utc = (Get-Date).ToUniversalTime().ToString('o')
        }
    }
    Write-CwManifestLine -Writer $writer -Record @{
        v = 1; op = 'result'; seq = $seq; status = $(if($verdict -eq 'moved') { 'PASS' } else { 'FAIL' })
        reason = $verdict; src_observed = $src; dst_observed = $dst
        utc = (Get-Date).ToUniversalTime().ToString('o')
    }
    if($verdict -eq 'moved') {
        Write-Host "[ARCHIVED] $from -> $to (verified: source absent, destination present)"
        # The re-issue succeeded, so the block it was allowed through is
        # resolved -- recorded, never silent.
        $clearedHex = @()
        foreach($b in @($activeBlocks)) {
            if((Get-CwBlockReissueVerdict -BlockHex ([string]$b.name_hex) -TargetHex $nameHex -Reason ([string]$b.reason)) -ne 'reissue') { continue }
            # T1: paired to the block it clears, so it can never reach past
            # that block to a LATER one for the same world. C15-2/C15-6: the
            # pairing carries the block's OWN utc, so it names that block
            # INSTANCE rather than a (manifest, seq) coordinate that recurs
            # every time a fresh batch id restarts seq at 1.
            Write-CwManifestLine -Writer $writer -Record @{
                v = 1; op = 'block_cleared'; name_hex = [string]$b.name_hex
                block_manifest = [string]$b.cw_manifest; block_seq = [string]$b.seq
                block_utc = (Get-CwRecordUtcKey -Value $b.utc)
                reason = 'reissued'; seq = $seq
                utc = (Get-Date).ToUniversalTime().ToString('o')
            }
            Write-Host "[BLOCK CLEARED] $(Get-CwBlockDisplayName -Block $b) (name_hex=$($b.name_hex), seq $($b.seq)) -- re-issued and verified moved."
            $clearedHex += [string]$b.name_hex
        }
        # C15-5: this leg moved its world, but the campaign is only healthy
        # if NOTHING is still halted. Re-issuing one blocked world clears
        # only that world's block -- correctly -- and the leg then exited 0
        # with another world's halt open, so a runner keying off the exit
        # code saw a clean success for a run that left a halt behind.
        $stillOpen = @()
        foreach($b in @($activeBlocks)) {
            $hex = [string]$b.name_hex
            $wasCleared = $false
            foreach($c in @($clearedHex)) { if(Test-CwNameEqual $c $hex) { $wasCleared = $true } }
            if($wasCleared) { continue }
            $stillOpen += ("{0} (name_hex={1}, {2})" -f
                           (Get-CwBlockDisplayName -Block $b), $hex, [string]$b.reason)
        }
        if(@($stillOpen).Count -gt 0) {
            Write-CwFail ("HALT STILL OPEN: the move above succeeded and was verified, but this campaign still has " +
                          @($stillOpen).Count + " unresolved halt(s): " + ($stillOpen -join '; ') +
                          ". This leg is NOT a success. Resolve each one -- re-issue that world, or clear it deliberately with -ResolveBlock / -ResolveBlockHex -- before the next leg.")
            $exitCode = $script:CwSavesExit.Refused
            exit $exitCode
        }
        exit $script:CwSavesExit.Ok
    }
    Write-CwFail ("VERIFY STOP: " + (Get-CwVerdictMessage -Verdict $verdict -What "'$Name'") +
                  " A HALT has been recorded for it (op:block reason=$verdict, seq $seq, in $manifestPath) and it was flushed BEFORE the result that closes the intent, so it survives this leg: every later invocation of this campaign refuses until it is cleared deliberately with -ResolveBlock '$Name' or -ResolveBlockHex $nameHex. Do not clear it before you have looked at the card.")
    $exitCode = Get-CwVerdictExit -Verdict $verdict
    if($exitCode -eq $script:CwSavesExit.Ok) { $exitCode = $script:CwSavesExit.ServerFail }
    exit $exitCode
} catch {
    Write-CwFail $_.Exception.Message
    exit $script:CwSavesExit.ServerFail
} finally {
    if($null -ne $writer) { Close-CwManifestWriter -Writer $writer }
    if($null -ne $client) { $client.Close() }
    Exit-CwRigLock -Lock $lock
}
