<#
.SYNOPSIS
    Undo an archive batch: put every world the manifest says is DISPLACED
    back into the root the manifest recorded for it.

.DESCRIPTION
    DRY RUN BY DEFAULT; -Execute is required to send anything.

    The manifest is the authority, not a re-resolved default (Astra change
    1): each intent carries the root it came from, and this tool sends that
    root back to the server.

    Three properties the C9 review found missing, and what they mean here:

    C1 -- an archive intent with NO RESULT is the exact case the write-ahead
    manifest exists for (the rename happened, the reply was lost). It is
    RECONCILED first, by listing both ends, and then restored. This tool
    never reports "nothing to undo" for a world that is sitting in the
    archive.

    H3 -- a completed restore REDUCES the set. A batch that stopped half way
    resumes where it stopped; a batch that is fully restored re-runs as a
    no-op and says so. One world the server refuses (its name was re-created
    by a later "+new" run, so restoring it would need an overwrite) is
    skipped and reported, never an abort that strands the rest.

    H6 -- the plan always prints "N of M archived worlds are still
    displaced", and a partial undo exits NON-ZERO naming exactly what is
    still in the archive.

    both-present exits 4, neither-present exits 5, and an end that could not
    be OBSERVED exits 9 -- never silently treated as absent.

.EXAMPLE
    pwsh -File tools/restore_wii_saves.ps1 -WiiHost 192.0.2.72 `
         -Manifest tmp/wii_saves/manifest_20260921-151200.jsonl -Execute
#>
param(
    [Parameter(Mandatory=$true)][string]$WiiHost,
    [string]$Manifest = "",
    # C15-4: the manifest directory holds the ONLY record of what is in the
    # archive and what is halted -- and the runbook used to send the
    # operator into it to delete a file. If it is lost, the archive
    # directory is itself a record: -FromArchiveBatch <batch-id> rebuilds
    # the batch by LISTING sd:/cw_saves_archive/<batch-id> through the verb
    # that already exists, marks every entry "manifest lost, from listing",
    # and restores into -Root.
    [string]$FromArchiveBatch = "",
    [ValidateSet('saves','saves_app')][string]$Root = 'saves',
    [string]$ManifestDir = "",
    [int]$Port = 39019,
    [switch]$Execute,
    [ValidateRange(1,256)][int]$PageSize = 128,
    [string]$StageToken = "",
    [int]$ConnectTimeoutSeconds = 60,
    [string]$LockPath = "C:/tmp/cw-rig-lock",
    [string]$LockHeldBy = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'cw_saves_common.ps1')
$lostReplyInjection = [string]$env:CW_SAVES_INJECT_LOST_REPLY

$fromArchive = -not [string]::IsNullOrWhiteSpace($FromArchiveBatch)
if($fromArchive -and -not [string]::IsNullOrWhiteSpace($Manifest)) {
    Write-CwFail "pass -Manifest OR -FromArchiveBatch, never both: one reads the write-ahead record, the other rebuilds it from the card."
    exit $script:CwSavesExit.Refused
}
if($fromArchive) {
    if(-not (Test-CwBatchId $FromArchiveBatch)) {
        Write-CwFail "-FromArchiveBatch '$FromArchiveBatch' is not a valid batch id"
        exit $script:CwSavesExit.Refused
    }
    if([string]::IsNullOrWhiteSpace($ManifestDir)) { $ManifestDir = Join-Path $RepoRoot "tmp\wii_saves" }
    $Manifest = Join-Path $ManifestDir ("manifest_{0}.jsonl" -f $FromArchiveBatch)
    if(-not $PSBoundParameters.ContainsKey('Root')) {
        Write-CwFail "-FromArchiveBatch requires an explicit -Root saves|saves_app; a lost manifest contains no trustworthy source-root default"
        exit $script:CwSavesExit.Refused
    }
    if((Test-Path -LiteralPath $Manifest) -and (Get-Item -LiteralPath $Manifest).Length -gt 0) {
        Write-CwFail "-FromArchiveBatch refuses to rebuild onto non-empty manifest $Manifest; use -Manifest to resume that record"
        exit $script:CwSavesExit.Refused
    }
} else {
    if([string]::IsNullOrWhiteSpace($Manifest)) {
        Write-CwFail "-Manifest is required. If the manifest directory was lost, the ARCHIVE DIRECTORY is still a record: re-run with -FromArchiveBatch <batch-id> [-Root saves] to rebuild the batch from its listing."
        exit $script:CwSavesExit.Refused
    }
    if(-not (Test-Path -LiteralPath $Manifest)) {
        Write-CwFail "manifest not found: $Manifest. If the manifest directory was lost, the ARCHIVE DIRECTORY is still a record: re-run with -FromArchiveBatch <batch-id> [-Root saves] to rebuild the batch from its listing. Never delete anything in the manifest directory -- it is the only record of what is displaced and what is halted."
        exit $script:CwSavesExit.Refused
    }
}

function Get-CwIntentState {
    <#
        Folds a manifest into one row per world: was it archived, was it put
        back, and which intents are still unresolved. Keyed on name_hex,
        which is the only lossless identifier on the wire.
    #>
    param([object[]]$Records)
    $results = @{}
    foreach($r in @($Records)) {
        if(($r.PSObject.Properties.Name -contains 'op') -and $r.op -eq 'result') {
            $results["$($r.seq)"] = $r
        }
    }
    $state = [ordered]@{}
    foreach($r in @($Records)) {
        if(-not ($r.PSObject.Properties.Name -contains 'op')) { continue }
        if($r.op -ne 'intent') { continue }
        $key = [string]$r.name_hex
        if(-not $state.Contains($key)) {
            $state[$key] = [pscustomobject]@{
                NameHex = $key; Archived = $false; Restored = $false
                Root = [string]$r.root; Batch = [string]$r.batch
                From = ""; To = ""; Unresolved = @(); LastBlocked = ""
            }
        }
        $row = $state[$key]
        $res = $null
        if($results.ContainsKey("$($r.seq)")) { $res = $results["$($r.seq)"] }
        if($null -eq $res) {
            $row.Unresolved = @($row.Unresolved) + @($r)
            continue
        }
        $moved = ($res.status -eq 'PASS')
        if($r.dir -eq 'archive') {
            if($moved) {
                $row.Archived = $true
                $row.Restored = $false   # a later restore re-sets this
                $row.Root = [string]$r.root
                $row.Batch = [string]$r.batch
                $row.From = [string]$r.from
                $row.To = [string]$r.to
            }
        } else {
            if($moved) { $row.Restored = $true }
            elseif($res.PSObject.Properties.Name -contains 'reason') {
                $row.LastBlocked = [string]$res.reason
            }
        }
    }
    return ,@($state.Values)
}

$records = @()
if(Test-Path -LiteralPath $Manifest) { $records = Read-CwManifest -Path $Manifest }
$lock = $null
$client = $null
$writer = $null
$exitCode = $script:CwSavesExit.Ok

try {
    try {
        $lock = Enter-CwRigLock -LockPath $LockPath -LockHeldBy $LockHeldBy `
            -Owner ("restore_wii_saves.ps1 / $env:USERNAME / " + (Get-Date -Format 'o'))
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

    # ---- C1: reconcile EVERY unfinished intent, archive ones included ---
    # N15: a DRY RUN is read-only with respect to the write-ahead record.
    # It still OBSERVES both ends and still STOPs, it just writes nothing.
    $readOnly = -not $Execute

    # ---- C15-4: rebuild a LOST manifest from the archive directory ------
    if($fromArchive) {
        $recoveryBinding = Read-CwRootBinding -ManifestDir $ManifestDir
        if($null -eq $recoveryBinding) {
            Write-CwFail "-FromArchiveBatch requires a valid root_binding.json; without the lost manifest, that binding is the corroboration for the original root"
            exit $script:CwSavesExit.RootAmbiguous
        }
        if([string]$recoveryBinding.root -ne $Root) {
            Write-CwFail "-FromArchiveBatch root '$Root' differs from bound root '$($recoveryBinding.root)'"
            exit $script:CwSavesExit.RootAmbiguous
        }
        try {
            $arch = Get-CwSavesInventory -Stream $stream -Root ("archive/$FromArchiveBatch") -PageSize $PageSize
        } catch {
            Write-CwFail "could not list 'archive/$FromArchiveBatch': $($_.Exception.Message). Without either a manifest or a readable archive batch there is nothing to rebuild from."
            $exitCode = $script:CwSavesExit.InventoryInconsistent
            exit $exitCode
        }
        $destInventory = Get-CwSavesInventory -Stream $stream -Root $Root -PageSize $PageSize
        $destAbs = [string](@($destInventory.Pages)[0].root_abs)
        if($destAbs -ne [string]$recoveryBinding.root_abs) {
            Write-CwFail "-FromArchiveBatch root '$Root' resolves to '$destAbs', but the durable binding records '$($recoveryBinding.root_abs)'"
            exit $script:CwSavesExit.RootAmbiguous
        }
        $destHex = @($destInventory.Entries | Where-Object {
            (Test-CwEntryIsDir -Entry $_) -and $_.stat_ok -and $_.supported
        } | ForEach-Object { [string]$_.name_hex })
        $missingKeepers = @()
        foreach($keeperHex in @($recoveryBinding.keep_hex)) {
            if(@($destHex | Where-Object { Test-CwNameEqual $_ ([string]$keeperHex) }).Count -eq 0) {
                $missingKeepers += [string]$keeperHex
            }
        }
        if(@($missingKeepers).Count -gt 0) {
            Write-CwFail "-FromArchiveBatch destination root is missing bound keeper(s) name_hex=$($missingKeepers -join ','); refusing recovery onto an unverified card"
            exit $script:CwSavesExit.RootAmbiguous
        }
        $archAbs = [string](@($arch.Pages)[0].root_abs)
        $unrestorable = @($arch.Entries | Where-Object {
            -not (Test-CwEntryMaybeDir -Entry $_) -or -not $_.supported -or -not $_.stat_ok
        })
        if(@($unrestorable).Count -gt 0) {
            Write-CwFail "-FromArchiveBatch found $(@($unrestorable).Count) unsupported, non-directory, or unreadable row(s); refusing a partial reconstruction"
            exit $script:CwSavesExit.Refused
        }
        $rebuilt = @($arch.Entries)
        Write-Host "[MANIFEST LOST] rebuilt $(@($rebuilt).Count) entry(ies) for batch '$FromArchiveBatch' by LISTING $archAbs. Every one is marked 'manifest lost, from listing': the archive directory is itself a record, which is exactly why nothing in the manifest directory may ever be deleted."
        $syn = @()
        $n = 0
        foreach($e in @($rebuilt)) {
            $n++
            $disp = Get-CwEntryDisplay -Entry $e
            Write-Host ("   - {0}  (name_hex={1})   [manifest lost, from listing]" -f $disp, [string]$e.name_hex)
            $syn += [pscustomobject]@{
                v = 1; op = 'intent'; seq = $n; dir = 'archive'; root = $Root
                batch = $FromArchiveBatch; name_hex = [string]$e.name_hex
                from = "$destAbs/$disp"; to = "$archAbs/$disp"
                manifest_lost = $true
                utc = (Get-Date).ToUniversalTime().ToString('o') }
            $syn += [pscustomobject]@{
                v = 1; op = 'result'; seq = $n; status = 'PASS'
                reason = 'observed_in_archive'; manifest_lost = $true
                utc = (Get-Date).ToUniversalTime().ToString('o') }
        }
        if(@($rebuilt).Count -eq 0) {
            Write-CwFail "-FromArchiveBatch found an empty archive batch; an empty listing cannot reconstruct a lost manifest"
            exit $script:CwSavesExit.Refused
        }
        if($readOnly) {
            $records = @($records) + @($syn)
        } else {
            $w0 = New-CwManifestWriter -Path $Manifest
            try {
                Write-CwManifestLine -Writer $w0 -Record @{
                    v = 1; op = 'batch_begin'; batch = $FromArchiveBatch
                    root = $Root; root_abs = $destAbs
                    card_identity = [string]$recoveryBinding.bound_utc
                    reconstructed_from = 'archive_listing'
                    note = 'the manifest for this batch was lost; these entries were rebuilt by listing the archive directory'
                    utc = (Get-Date).ToUniversalTime().ToString('o') }
                foreach($r in @($syn)) {
                    $h = @{}
                    foreach($prop in $r.PSObject.Properties) { $h[$prop.Name] = $prop.Value }
                    Write-CwManifestLine -Writer $w0 -Record $h
                }
            } finally { Close-CwManifestWriter -Writer $w0 }
            $records = Read-CwManifest -Path $Manifest
        }
    }
    $open = Get-CwUnfinishedIntents -Records @($records)
    if(@($open).Count -gt 0) {
        Write-Host "[RECONCILE] $(@($open).Count) intent(s) in this manifest have no recorded result; listing BOTH ends (never a blind retry)."
        if(-not $readOnly) { $writer = New-CwManifestWriter -Path $Manifest }
        foreach($i in @($open)) {
            # An ARCHIVE intent's source is the saves root and its
            # destination the batch; a RESTORE intent is the mirror image.
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
                Write-CwFail ("RECONCILE STOP: " + (Get-CwVerdictMessage -Verdict $verdict -What "seq=$($i.seq)"))
                $exitCode = Get-CwVerdictExit -Verdict $verdict
                exit $exitCode
            }
            if($readOnly) {
                # Fold the observation into THIS run's view without
                # persisting it, so the dry run still reports the truth.
                $records = @($records) + @([pscustomobject]@{
                    v = 1; op = 'result'; seq = [int]$i.seq
                    status = $(if($verdict -eq 'moved') { 'PASS' } else { 'FAIL' })
                    reason = "reconciled_$verdict"
                })
                continue
            }
            Write-CwManifestLine -Writer $writer -Record @{
                v = 1; op = 'result'; seq = [int]$i.seq
                status = $(if($verdict -eq 'moved') { 'PASS' } else { 'FAIL' })
                reason = "reconciled_$verdict"
                src_observed = $src; dst_observed = $dst
                utc = (Get-Date).ToUniversalTime().ToString('o')
            }
        }
        if($null -ne $writer) {
            Close-CwManifestWriter -Writer $writer
            $writer = $null
            $records = Read-CwManifest -Path $Manifest
        }
    }

    # ---- C15-1(b): ARCHIVED-UNCERTAIN --------------------------------
    # An archive intent whose recorded result is a HALT (both-present,
    # neither-present, could-not-observe) describes a world whose position
    # the archive tool could not establish. Get-CwIntentState marks a world
    # Archived only on a PASS, so the verify path's FAIL stamp made such a
    # world INVISIBLE here: after a stop_unknown the world really is in the
    # archive and this tool printed "nothing to undo", which is the one
    # outcome the whole design exists to prevent. These are never taken on
    # the record's word -- both ends are OBSERVED, now.
    $uncertain = Get-CwUncertainArchiveIntents -Records @($records)
    $unexplained = @()
    $unexplainedExit = $script:CwSavesExit.Ok
    if(@($uncertain).Count -gt 0) {
        Write-Host "[UNCERTAIN] $(@($uncertain).Count) archive intent(s) ended in a STOP the archive tool could not explain; OBSERVING both ends before planning anything."
        $uw = $null
        if(-not $readOnly) { $uw = New-CwManifestWriter -Path $Manifest }
        try {
            foreach($u in @($uncertain)) {
                $i = $u.Intent
                $srcRoot = [string]$i.root
                $dstRoot = "archive/" + [string]$i.batch
                $src = Get-CwNamePresence -Stream $stream -Root $srcRoot -NameHex ([string]$i.name_hex) -PageSize $PageSize
                $dst = Get-CwNamePresence -Stream $stream -Root $dstRoot -NameHex ([string]$i.name_hex) -PageSize $PageSize
                $verdict = Resolve-CwReconcileVerdict -Src $src -Dst $dst
                Write-Host "[UNCERTAIN] seq=$($i.seq) name_hex=$($i.name_hex) recorded=$($u.Reason) src=$src dst=$dst -> $verdict"
                if($verdict -eq 'moved') {
                    # It IS in the archive. Record the OBSERVATION (append
                    # only -- the halt record itself stays, because the
                    # archive tool's campaign still needs a human to clear
                    # it) and treat the world as displaced.
                    $obs = @{
                        v = 1; op = 'result'; seq = [int]$i.seq; status = 'PASS'
                        reason = 'observed_archived'; recorded_halt = [string]$u.Reason
                        src_observed = $src; dst_observed = $dst
                        utc = (Get-Date).ToUniversalTime().ToString('o') }
                    if(-not $readOnly) { Write-CwManifestLine -Writer $uw -Record $obs }
                    $records = @($records) + @([pscustomobject]$obs)
                    Write-Host "[UNCERTAIN] ...it IS in the archive; it is restorable and appears in the plan below."
                } elseif($verdict -eq 'not_moved') {
                    Write-Host "[UNCERTAIN] ...it is in '$srcRoot', NOT the archive: nothing for the undo path to do about this one. The archive tool's halt on it still needs clearing."
                } else {
                    $unexplained += ("seq=$($i.seq) name_hex=$($i.name_hex) recorded=$($u.Reason) -> $verdict")
                    $unexplainedExit = Get-CwVerdictExit -Verdict $verdict
                }
            }
        } finally {
            if($null -ne $uw) { Close-CwManifestWriter -Writer $uw }
        }
        if(-not $readOnly) { $records = Read-CwManifest -Path $Manifest }
        # STOP here rather than after the plan: with a world sitting at both
        # ends, a plan reading "archived: 0 / STILL DISPLACED: 0 of 0" is
        # worse than no plan at all.
        if(@($unexplained).Count -gt 0) {
            Write-CwFail ("UNCERTAIN AND UNEXPLAINED: " + @($unexplained).Count +
                          " archive intent(s) ended in a STOP and OBSERVING both ends did not settle it: " +
                          ($unexplained -join '; ') +
                          ". Touch nothing; investigate on the card. This is NOT 'nothing to undo' -- the world's position is genuinely unknown, and restoring it blind could overwrite something.")
            $exitCode = $unexplainedExit
            if($exitCode -eq $script:CwSavesExit.Ok) { $exitCode = $script:CwSavesExit.ServerFail }
            exit $exitCode
        }
    }

    # ---- H3 + H6: what is actually still displaced ----------------------
    $state = Get-CwIntentState -Records @($records)
    $archived = @(@($state) | Where-Object { $_.Archived })
    $displaced = @(@($archived) | Where-Object { -not $_.Restored })
    $alreadyBack = @($archived).Count - @($displaced).Count

    Write-Host ""
    Write-Host "================ RESTORE PLAN ================"
    Write-Host (" manifest   : {0}" -f $Manifest)
    Write-Host (" archived   : {0} world(s) recorded as moved into the archive" -f @($archived).Count)
    Write-Host (" STILL DISPLACED: {0} of {1}" -f @($displaced).Count, @($archived).Count)
    if($alreadyBack -gt 0) {
        Write-Host (" already restored: {0} (skipped -- a completed restore reduces the set)" -f $alreadyBack)
    }
    foreach($w in @($displaced)) {
        $blocked = if([string]::IsNullOrEmpty($w.LastBlocked)) { "" } else { "   [last attempt: $($w.LastBlocked)]" }
        Write-Host ("   {0}  ->  {1}   (root '{2}', batch '{3}'){4}" -f $w.To, $w.From, $w.Root, $w.Batch, $blocked)
    }
    Write-Host (" mode       : {0}" -f $(if($Execute) { 'EXECUTE' } else { 'DRY RUN (pass -Execute to act)' }))
    Write-Host "============================================="
    Write-Host ""

    if(@($archived).Count -eq 0) {
        if(@($uncertain).Count -gt 0) {
            Write-Host "[RESTORE] every uncertain intent was OBSERVED and none of them left a world in the archive; there is nothing displaced to undo."
            exit $script:CwSavesExit.Ok
        }
        Write-Host "[RESTORE] this manifest records no world as moved into the archive; nothing to undo."
        exit $script:CwSavesExit.Ok
    }
    if(@($displaced).Count -eq 0) {
        Write-Host "[RESTORE] 0 of $(@($archived).Count) still displaced -- every world in this batch is already restored. Nothing to do."
        exit $script:CwSavesExit.Ok
    }
    if(-not $Execute) {
        Write-Host "[DRY RUN] nothing was sent."
        exit $script:CwSavesExit.Ok
    }

    # ---- execute, in reverse archive order, skipping what is blocked ----
    $writer = New-CwManifestWriter -Path $Manifest
    $seq = 1
    foreach($r in @($records)) {
        if(($r.PSObject.Properties.Name -contains 'seq') -and [int]$r.seq -ge $seq) { $seq = [int]$r.seq + 1 }
    }
    $ordered = @($displaced)
    [array]::Reverse($ordered)
    $blockedNow = @()
    $moveLeg = 0
    foreach($w in $ordered) {
        $moveLeg++
        $nameHex = $w.NameHex
        Write-CwManifestLine -Writer $writer -Record @{
            v = 1; op = 'intent'; seq = $seq; dir = 'restore'; root = $w.Root
            batch = $w.Batch; name_hex = $nameHex; from = $w.To; to = $w.From
            utc = (Get-Date).ToUniversalTime().ToString('o')
        }
        $injectThisMove = Test-CwLostReplyInjection -Setting $lostReplyInjection -NameHex $nameHex -Leg $moveLeg
        $reply = Invoke-CwSavesMove -Stream $stream -Direction 'restore' -Root $w.Root -Batch $w.Batch -NameHex $nameHex -InjectLostReply:$injectThisMove
        Write-Host "[CWARCHIVE1] $($reply.Line)"
        if($reply.Json.status -ne 'PASS') {
            # One blocked world must NOT strand the rest of the batch: record
            # it, say so, keep going, and fail the RUN at the end (H3/H6).
            Write-CwManifestLine -Writer $writer -Record @{
                v = 1; op = 'result'; seq = $seq; status = 'FAIL'
                reason = [string]$reply.Json.reason
                utc = (Get-Date).ToUniversalTime().ToString('o')
            }
            Write-Host "[SKIPPED] $($w.To): the server refused with '$($reply.Json.reason)'; continuing with the rest of the batch."
            $blockedNow += ("{0} ({1})" -f $w.To, [string]$reply.Json.reason)
            $seq++
            continue
        }
        $src = Get-CwNamePresence -Stream $stream -Root ("archive/" + $w.Batch) -NameHex $nameHex -PageSize $PageSize
        $dst = Get-CwNamePresence -Stream $stream -Root $w.Root -NameHex $nameHex -PageSize $PageSize
        $verdict = Resolve-CwReconcileVerdict -Src $src -Dst $dst
        Write-CwManifestLine -Writer $writer -Record @{
            v = 1; op = 'result'; seq = $seq
            status = $(if($verdict -eq 'moved') { 'PASS' } else { 'FAIL' })
            reason = $verdict; src_observed = $src; dst_observed = $dst
            utc = (Get-Date).ToUniversalTime().ToString('o')
        }
        if($verdict -like 'stop_*') {
            Write-CwFail ("VERIFY STOP: " + (Get-CwVerdictMessage -Verdict $verdict -What $w.To))
            $exitCode = Get-CwVerdictExit -Verdict $verdict
            exit $exitCode
        }
        if($verdict -ne 'moved') {
            Write-CwFail "VERIFY STOP: $($w.To) did not land in $($w.Root)."
            $exitCode = $script:CwSavesExit.StopNeitherPresent
            exit $exitCode
        }
        Write-Host "[RESTORED] $($w.To) -> $($w.From)"
        $seq++
    }

    if(@($blockedNow).Count -gt 0) {
        Write-CwFail ("PARTIAL UNDO: {0} of {1} world(s) are STILL DISPLACED and were not restored: {2}. Re-run this command once the obstruction is cleared -- it resumes where it stopped." -f @($blockedNow).Count, @($archived).Count, ($blockedNow -join '; '))
        $exitCode = $script:CwSavesExit.ServerFail
        exit $exitCode
    }
    Write-Host "[RESTORE] every archived world in this batch is back in its recorded root."
    exit $script:CwSavesExit.Ok
} catch {
    Write-CwFail $_.Exception.Message
    exit $script:CwSavesExit.ServerFail
} finally {
    if($null -ne $writer) { Close-CwManifestWriter -Writer $writer }
    if($null -ne $client) { $client.Close() }
    Exit-CwRigLock -Lock $lock
}
