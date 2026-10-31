<#
    cw_saves_common.ps1 -- shared, DOT-SOURCED helpers for the three saves
    tools (list_wii_saves.ps1, archive_wii_saves.ps1, restore_wii_saves.ps1).

    Dot-sourced rather than a module because this repo has no .psm1 and every
    other tools/*.ps1 is a standalone script; `. (Join-Path $PSScriptRoot
    'cw_saves_common.ps1')` keeps the three tools' functions identical without
    introducing a module-path dependency into the ctest invocations.

    Everything here is either (a) a PURE function -- no sockets, no card, no
    clock -- so Invoke-CwSavesSelfTest can pin it under ctest without a Wii,
    or (b) a thin stage-server client mirroring tools/get_wii_stage_manifest.ps1
    (connect-with-timeout, CWHELLO1, CWAUTH1, Assert-PassJson, token
    resolution).

    Design: tmp/jobs/push-0921/design-saves-archive.md v2 (+ astra-ruling-4).
    Operator doc: docs/wii_saves_archive.md.

    NOTHING in this file performs a power action, stages a .dol, or writes to
    the SD card outside the two saves roots and sd:/cw_saves_archive/. There
    is no delete path anywhere in the protocol these helpers speak.
#>

Set-StrictMode -Version Latest

# Distinct exit codes, so a caller (and the ctest fixtures) can tell the
# refusals apart without parsing text. STOP codes 4 and 5 are the two
# reconcile outcomes that must never be auto-retried.
$script:CwSavesExit = @{
    Ok                    = 0
    Refused               = 2   # usage / keep-list / lock / plan refusal
    InventoryInconsistent = 3   # torn paged listing -- never execute on it
    StopBothPresent       = 4   # reconcile: src AND dst present
    StopNeitherPresent    = 5   # reconcile: neither present -- missing data
    ServerTooOld          = 6   # handshake lacks a required capability
    ServerFail            = 7   # the server refused an operation
    RootAmbiguous         = 8   # U1 three-signal binding did not agree
    UnknownObservation    = 9   # an end could not be OBSERVED -- never
                                # folded into "it is not there"
    OrphanGuard           = 10  # archive directory and manifest disagree
}

# The saves wire version these tools speak; must equal the server's
# CW_SAVES_PROTOCOL_VERSION (main.c). Bump both together.
$script:CwSavesProtocol = 1
$script:CwSavesArchiveRoot = 'sd:/cw_saves_archive'
$script:CwSavesRigLock = 'C:/tmp/cw-rig-lock'

<#
    Report a refusal WITHOUT throwing. Write-Error under the tools'
    $ErrorActionPreference = "Stop" is a TERMINATING error, so a
    `Write-Error ...; exit 2` pair never reaches its own exit -- the throw
    unwinds into the nearest catch and the script ends on someone else's
    code. Every refusal in these tools therefore goes through here, so the
    distinct exit codes in $CwSavesExit actually reach the caller (the ctest
    fixtures assert on them).
#>
function Write-CwFail {
    param([string]$Message)
    [Console]::Error.WriteLine("ERROR: $Message")
}

# The create/verify profile PAIRS an automatic clean-up must never archive
# between (design S8's landmine). world_reload_verify.json:143 autoloads "1"
# -- the FIRST row in readdir order, not "the world the create leg made" --
# so archiving between the two legs, or merely reordering the directory,
# silently changes what it verifies. Exported so the (separate, later)
# wiring brief can protect them; nothing here wires cleanup into a runner.
$script:CwSavesCreateVerifyPairs = @(
    @{ Create = 'world_reload_create'; Verify = 'world_reload_verify'
       Note   = 'verify autoloads "1" = first readdir row; file a follow-up to give it a named autoload' }
)

<#
    Every card job whose config does NOT autoload "+new" depends on an
    EXISTING world, by name or by index, and is therefore exposed to a
    clean-up that reorders or removes directories.

    There is no enumerable source to generate this from: these are
    stage-side job JSON names, not hw_run_profile_name entries (hw_run.c
    lists 21 profiles and neither world_reload_create nor world_reload_verify
    is among them), so the table is hand-maintained -- and GATED by
    Invoke-CwSavesSelfTest, which fails if any tools/hw_card_package/configs
    entry autoloads something other than "+new" without appearing here
    (C9 review, M7).

    autoload "1" is the sharp one: it means "the FIRST readdir row", so every
    archive move changes what it verifies.
#>
$script:CwSavesAutoloadDependentJobs = @(
    @{ Job = 'world_reload_verify'; Autoload = '1'
       Note = 'index-based: the first readdir row. ANY move changes what this verifies; never clean up between the create and verify legs.' }
)

<#
    Card jobs whose config carries NO autoload key at all. The M7 gate used
    to `continue` past those, so a future config that DROPS the key while
    still depending on directory order would slip through by omission
    (C11 re-review, N13). Listing them makes the omission a deliberate,
    reviewed act instead.
#>
$script:CwSavesAutoloadExemptJobs = @(
    @{ Job = 'free_play';     Note = 'no autoload key; the operator picks a world by hand, so nothing depends on list order' },
    @{ Job = 'free_play_kit'; Note = 'no autoload key; same as free_play' }
)

# ---------------------------------------------------------------------
# Pure helpers: hex, FNV-1a, names
# ---------------------------------------------------------------------

function ConvertTo-CwHex {
    param([byte[]]$Bytes)
    if($null -eq $Bytes) { return "" }
    $sb = [System.Text.StringBuilder]::new($Bytes.Length * 2)
    foreach($b in $Bytes) { [void]$sb.AppendFormat("{0:x2}", $b) }
    return $sb.ToString()
}

function ConvertFrom-CwHex {
    param([string]$Hex)
    if([string]::IsNullOrEmpty($Hex)) { throw "empty name_hex" }
    if(($Hex.Length % 2) -ne 0) { throw "odd-length name_hex: $Hex" }
    $out = New-Object byte[] ($Hex.Length / 2)
    for($i = 0; $i -lt $Hex.Length; $i += 2) {
        $pair = $Hex.Substring($i, 2)
        if($pair -notmatch '^[0-9a-fA-F]{2}$') { throw "non-hex name_hex: $Hex" }
        $out[$i / 2] = [Convert]::ToByte($pair, 16)
    }
    return ,$out
}

<#
    FNV-1a over each entry's name_hex followed by a single "`n", byte for
    byte identical to cw_saves_fnv1a() in
    tools/wii_stage_server/source/stage_saves.h. The separator matters: hex
    is 2 chars per byte, so "ab"+"cd" and "abc"+"d" are both well-formed hex
    streams of DIFFERENT name sets and would otherwise collide.
#>
function Get-CwFnv1aOfString {
    param([string]$Text)
    $h = [uint64]2166136261
    foreach($ch in ([System.Text.Encoding]::ASCII.GetBytes($Text))) {
        $h = $h -bxor [uint64]$ch
        $h = ($h * [uint64]16777619) -band [uint64]4294967295  # 0xFFFFFFFF; PowerShell parses the hex literal as Int32 -1
    }
    return ('{0:x8}' -f $h)
}

function Get-CwPrefixFnv1a {
    param([string[]]$NameHex)
    $sb = [System.Text.StringBuilder]::new()
    foreach($nh in @($NameHex)) { [void]$sb.Append($nh).Append("`n") }
    return (Get-CwFnv1aOfString -Text $sb.ToString())
}

# FAT is case-insensitive, so every name comparison the tools make folds --
# the same rule cw_saves_name_ci_equal() enforces server-side.
function Test-CwNameEqual {
    param([string]$A, [string]$B)
    return [string]::Equals($A, $B, [StringComparison]::OrdinalIgnoreCase)
}

function New-CwBatchId {
    param([datetime]$When = (Get-Date))
    return $When.ToString('yyyyMMdd-HHmmss')
}

function Test-CwBatchId {
    param([string]$Batch)
    if([string]::IsNullOrEmpty($Batch)) { return $false }
    if($Batch.Length -gt 32) { return $false }
    if($Batch -notmatch '^[A-Za-z0-9_.-]+$') { return $false }
    if($Batch -eq '.' -or $Batch -eq '..') { return $false }
    if($Batch.StartsWith('.')) { return $false }
    if($Batch.EndsWith('.') -or $Batch.EndsWith(' ')) { return $false }
    if($Batch.Contains('..')) { return $false }
    return $true
}

# ---------------------------------------------------------------------
# Paged-inventory consistency (design S3, Astra change 3)
# ---------------------------------------------------------------------

<#
    Rejects an inventory that could have been taken across a directory that
    changed mid-walk. Every page must agree on `total`; the accumulated
    name_hex prefix must reproduce each page's prefix_fnv1a; the pages must
    be contiguous from offset 0; and the final page must have delivered
    exactly `total` entries.

    Returns the flattened entry list. THROWS (the caller exits
    InventoryInconsistent) on any disagreement -- a batch is never executed
    against a torn list.
#>
<#
    C32 (inventory cw_saves_common.ps1:195-269,1597-1599): [int]$null is 0
    and [int]"" throws in one place and yields 0 in another, so a page field
    the server sent as null -- or did not send at all -- became a perfectly
    ordinary number and the paging loop carried on. A number that is not a
    number is a PARSE ERROR, before any plan.
#>
function Get-CwRequiredInt {
    param($Object, [string]$Field, [string]$Where)
    if(-not ($Object.PSObject.Properties.Name -contains $Field)) {
        throw "$Where is missing required field '$Field'"
    }
    $v = $Object.$Field
    if($null -eq $v) { throw "$Where field '$Field' is null, which is not a number" }
    if($v -is [bool]) { throw "$Where field '$Field' is a boolean, not a number" }
    if($v -is [string]) { throw "$Where field '$Field' is the string '$v', not a number" }
    try {
        $d = [decimal]$v
        if($d -ne [decimal]::Truncate($d)) {
            throw "$Where field '$Field' is fractional, not an integer: $v"
        }
        if($d -lt [int]::MinValue -or $d -gt [int]::MaxValue) {
            throw "$Where field '$Field' is outside the Int32 range: $v"
        }
        return [int]$d
    } catch {
        if($_.Exception.Message -like "$Where field '$Field'*") { throw }
        throw "$Where field '$Field' is not an integer: $v"
    }
}

function Get-CwRequiredString {
    param($Object, [string]$Field, [string]$Where)
    if(-not ($Object.PSObject.Properties.Name -contains $Field)) {
        throw "$Where is missing required field '$Field'"
    }
    $v = $Object.$Field
    if($null -eq $v -or -not ($v -is [string])) {
        throw "$Where field '$Field' is not a string"
    }
    return [string]$v
}

function Get-CwRequiredBool {
    param($Object, [string]$Field, [string]$Where)
    if(-not ($Object.PSObject.Properties.Name -contains $Field)) {
        throw "$Where is missing required field '$Field'"
    }
    if(-not ($Object.$Field -is [bool])) {
        throw "$Where field '$Field' is not a boolean"
    }
    return [bool]$Object.$Field
}

function Assert-CwInventoryConsistent {
    param([object[]]$Pages)

    if($null -eq $Pages -or @($Pages).Count -eq 0) {
        throw "inventory has no pages"
    }
    # NOTE: a generic List piped through ForEach-Object via @(...) throws
    # "Argument types do not match" on this PowerShell build, so every List
    # in this file is handed on with .ToArray() and never wrapped in @().
    $entries = New-Object System.Collections.Generic.List[object]
    $hexes = New-Object System.Collections.Generic.List[string]
    $expectedOffset = 0
    $total = $null
    $allHash = $null
    foreach($p in @($Pages)) {
        foreach($f in @('offset','count','next','total','prefix_fnv1a','all_fnv1a','entries','root_abs')) {
            if(-not ($p.PSObject.Properties.Name -contains $f)) {
                throw "inventory page missing required field '$f'"
            }
        }
        $null = Get-CwRequiredInt -Object $p -Field 'offset' -Where 'inventory page'
        $null = Get-CwRequiredInt -Object $p -Field 'count' -Where 'inventory page'
        $null = Get-CwRequiredInt -Object $p -Field 'next' -Where 'inventory page'
        $null = Get-CwRequiredInt -Object $p -Field 'total' -Where 'inventory page'
        if([int]$p.offset -ne $expectedOffset) {
            throw "inventory page offset $($p.offset) is not the expected $expectedOffset (pages must be contiguous from 0)"
        }
        if($null -eq $total) { $total = [int]$p.total }
        elseif([int]$p.total -ne $total) {
            throw "inventory total changed between pages ($total -> $($p.total)): the saves directory was modified mid-walk"
        }
        # all_fnv1a covers EVERY entry of the walk that produced this page,
        # so two pages disagreeing means the directory changed between them
        # -- including the one case total + prefix cannot see: a delete
        # paired with an insert at or past the page boundary, which is what
        # FAT does when a directory slot is freed and reused (M2).
        if($null -eq $allHash) { $allHash = [string]$p.all_fnv1a }
        elseif([string]$p.all_fnv1a -ne $allHash) {
            throw "inventory all_fnv1a changed between pages ($allHash -> $($p.all_fnv1a)): the saves directory was modified mid-walk"
        }
        $pageEntries = @($p.entries)
        if($pageEntries.Count -ne [int]$p.count) {
            throw "inventory page declared count $($p.count) but carried $($pageEntries.Count) entries"
        }
        foreach($e in $pageEntries) {
            foreach($f in @('name_hex','is_dir','supported','stat_ok')) {
                if(-not ($e.PSObject.Properties.Name -contains $f)) {
                    throw "inventory entry missing required field '$f'"
                }
            }
            # A row whose stat FAILED carries no usable is_dir/bytes/mtime.
            # It is DELIVERED with that state intact rather than rejecting
            # the whole listing: on a 646-entry card one corrupt directory
            # entry used to make every tool exit non-zero -- including the
            # UNDO path, which could then be blocked by an entry that has
            # nothing to do with the world being restored (C11 re-review,
            # N6). The callers refuse to MOVE such a row and refuse it as a
            # keeper; nothing else has to care.
            $entries.Add($e)
            $hexes.Add([string]$e.name_hex)
        }
        $want = Get-CwPrefixFnv1a -NameHex $hexes.ToArray()
        if($want -ne [string]$p.prefix_fnv1a) {
            throw "inventory prefix_fnv1a mismatch at offset $($p.offset): server said $($p.prefix_fnv1a), the pages received hash to $want -- the saves directory was modified mid-walk"
        }
        $expectedOffset = [int]$p.offset + [int]$p.count
    }
    $last = @($Pages)[-1]
    if([int]$last.next -ne -1) {
        throw "inventory is incomplete: the last page still advertises next=$($last.next)"
    }
    if($entries.Count -ne $total) {
        throw "inventory delivered $($entries.Count) entries but declared total=$total"
    }
    $wantAll = Get-CwPrefixFnv1a -NameHex $hexes.ToArray()
    if($wantAll -ne $allHash) {
        throw "inventory all_fnv1a mismatch: server said $allHash, the assembled inventory hashes to $wantAll -- the saves directory was modified mid-walk"
    }
    return ,$entries.ToArray()
}

<#
    Signal (a) of Astra change 1's three-signal bind: does the game's own
    config agree about where the worlds live?

    config.json's paths.worlds is read with config_read_string, NOT
    config_read_path, so it stays CWD-relative on hardware (design S0/U1).
    The two roots this protocol knows are the SD root's "saves" and the app
    directory's "apps/CavEX/saves"; anything else corroborates neither
    and is ambiguity, which the ruling says must STOP.
#>
function Test-CwConfigWorldsMatchesRoot {
    param([string]$PathsWorlds, [string]$Root, [bool]$KeyPresent = $true)
    if(-not $KeyPresent) {
        # T7: an ABSENT key is not ambiguity -- it is the GAME's default.
        # config_read_string(&gstate.config_user, "paths.worlds", "saves"),
        # source/game/gui/screen_select_world.c:187 (also :630, :668, :710,
        # and screen_create_world.c:261). A card whose config.json has no
        # paths key is a card the game reads as "saves", so refusing it
        # made the tool disagree with the game and pointed the operator at
        # a fetch failure that never happened. A config that could not be
        # READ is a different thing, and the caller STOPs on that before
        # reaching here.
        return ($Root -eq 'saves')
    }
    if([string]::IsNullOrWhiteSpace($PathsWorlds)) { return $false }
    # T8: '\\' inside PowerShell single quotes is TWO literal backslashes,
    # so this used to rewrite only a DOUBLED separator and 'saves\' or
    # '.\saves' failed to corroborate a card that plainly said "saves".
    # A single-quoted '\' is one backslash, which is what was meant.
    $v = $PathsWorlds.Trim().Replace('\', '/').Trim('/')
    if($v -like 'sd:/*') { $v = $v.Substring(4).Trim('/') }
    while($v.StartsWith('./')) { $v = $v.Substring(2).Trim('/') }
    switch($Root) {
        'saves'     { return ($v -ieq 'saves') }
        'saves_app' { return ($v -ieq 'apps/CavEX/saves') }
        default     { return $false }
    }
}

<#
    Signal (c) needs more than a display name: "identifiable world metadata"
    means the row is a real Beta world tree, so LastPlayed has to be there
    too. Returns both, and never throws -- an undecodable world is a fact
    the caller weighs, not a crash.
#>
function Get-CwLevelInfo {
    param([byte[]]$Nbt)
    $name = Get-CwLevelName -Nbt $Nbt
    $hasLastPlayed = $false
    try {
        # LastPlayed is a TAG_Long; Get-CwLevelName's walk already visits
        # every tag, so its presence is a substring question on the decoded
        # name table rather than a second parse.
        $hasLastPlayed = (Test-CwNbtHasName -Nbt $Nbt -Name 'LastPlayed')
    } catch {
        $hasLastPlayed = $false
    }
    return [pscustomobject]@{ LevelName = $name; HasLastPlayed = $hasLastPlayed }
}

# Cheap, bounded "does this NBT contain a tag with this name" -- the name is
# length-prefixed UTF-8, so a byte scan for <len><bytes> is exact and needs
# no second walk.
function Test-CwNbtHasName {
    param([byte[]]$Nbt, [string]$Name)
    if($null -eq $Nbt) { return $false }
    $needle = [System.Text.Encoding]::UTF8.GetBytes($Name)
    $hi = [byte](($needle.Length -shr 8) -band 0xff)
    $lo = [byte]($needle.Length -band 0xff)
    for($i = 0; $i -le $Nbt.Length - ($needle.Length + 2); $i++) {
        if($Nbt[$i] -ne $hi -or $Nbt[$i + 1] -ne $lo) { continue }
        $ok = $true
        for($j = 0; $j -lt $needle.Length; $j++) {
            if($Nbt[$i + 2 + $j] -ne $needle[$j]) { $ok = $false; break }
        }
        if($ok) { return $true }
    }
    return $false
}

<#
    N9: the not_moved re-issue allowance must key on the CARD's bytes. The
    first version compared the blocker against ConvertTo-CwHex(UTF8(-Name)),
    i.e. a re-encoding of what the operator typed -- harmless for the ASCII
    names on this card and wrong for any high-byte one, which is the precise
    "future edit that uses it" the earlier L3 finding warned about. The
    target hex now comes from the server's own listing, and an unresolved
    target can never claim the allowance.
#>
function Get-CwBlockReissueVerdict {
    param([string]$BlockHex, [string]$TargetHex, [string]$Reason)
    if([string]::IsNullOrWhiteSpace($TargetHex)) { return 'blocked' }
    # Only a proven not_moved outcome is safe to retry.  stop_* means the
    # card is in an unexplained state; repeating the same request cannot be
    # allowed to turn that uncertainty into a clear.
    $r = $Reason.Trim()
    if($r.StartsWith('reconciled_', [StringComparison]::OrdinalIgnoreCase)) {
        $r = $r.Substring('reconciled_'.Length)
    }
    if([string]::Equals($r, 'not_moved', [StringComparison]::OrdinalIgnoreCase) -and
       (Test-CwNameEqual $BlockHex $TargetHex)) { return 'reissue' }
    return 'blocked'
}

<#
    THE TOTAL ORDER ON MANIFEST RECORDS (C13 review, T1).

    A `block_cleared` may clear only a block it FOLLOWS. The first version
    kept a cleared set keyed on `name_hex` ALONE -- no seq, no utc, no
    per-file pairing -- which is what made "a block in manifest A, its clear
    in manifest B" work in either order, and equally what let ONE old clear
    pre-clear every FUTURE block for that world, permanently.

    Deciding "follows" across separate append-only files needs an order, so
    it is defined here explicitly rather than left to whatever the caller's
    enumeration happened to be. For two records A and B:

      1. SAME manifest file -> the LINE INDEX decides, and nothing else can
         override it. The file is opened FileMode::Append and every line is
         Flush($true)ed before the next one is written, so line order IS
         write order.
      2. DIFFERENT files -> the record's `utc` decides (ISO-8601 'o',
         parsed as UTC ticks). Both tools stamp every record they write, and
         both run on one host under one rig lock, so this is the real write
         order.
      3. EQUAL utc, different files -> the manifest FILE ORDINAL decides:
         the file's position in Get-CwManifestPaths' Sort-Object Name
         listing. Batch ids are yyyyMMdd-HHmmss, so name order is write
         order; for a hand-passed -Batch it is at least deterministic.
      4. Either record has no parseable `utc` AND they are in different
         files -> UNORDERED ($null).

    Rule 4 is the whole safety property: a clear that cannot be PROVEN later
    than a block does not clear it. The block stays up and the operator
    resolves it deliberately. Fail-closed is the only acceptable direction,
    because the failure mode being replaced is a block that silently
    disarms and lets a world drop out of the campaign with no printed line.
#>
function Add-CwManifestOrder {
    param([object[]]$Records, [string]$Manifest, [int]$FileOrdinal)
    $out = New-Object System.Collections.Generic.List[object]
    $line = 0
    foreach($r in @($Records)) {
        $line++
        Add-Member -InputObject $r -NotePropertyName 'cw_manifest' -NotePropertyValue $Manifest -Force
        Add-Member -InputObject $r -NotePropertyName 'cw_file_ordinal' -NotePropertyValue ([int]$FileOrdinal) -Force
        Add-Member -InputObject $r -NotePropertyName 'cw_line' -NotePropertyValue ([int]$line) -Force
        $out.Add($r)
    }
    return ,$out.ToArray()
}

<#
    One canonical spelling for a timestamp, whatever ConvertFrom-Json handed
    back. Depending on the PowerShell build an ISO-8601 field comes back as a
    STRING or as a DateTime, and `[string]` on a DateTime renders it in the
    host's CULTURE -- so the same instant could be written to the manifest
    two different ways and then compared as unequal. Everything that
    compares or orders a timestamp goes through here first.
#>
function Get-CwRecordUtcKey {
    param($Value)
    if($null -eq $Value) { return "" }
    if($Value -is [datetime]) { return ([datetime]$Value).ToUniversalTime().ToString('o') }
    if($Value -is [datetimeoffset]) { return ([datetimeoffset]$Value).UtcDateTime.ToString('o') }
    $s = [string]$Value
    if([string]::IsNullOrWhiteSpace($s)) { return "" }
    try {
        return ([System.DateTimeOffset]::Parse($s,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [System.Globalization.DateTimeStyles]::RoundtripKind)).UtcDateTime.ToString('o')
    } catch {
        # Unparseable: hand the raw text back so an EQUALITY test can still
        # work, while Get-CwRecordUtcTicks below still reports "unordered".
        return $s
    }
}

function Get-CwRecordUtcTicks {
    param($Record)
    if($null -eq $Record) { return $null }
    if(-not ($Record.PSObject.Properties.Name -contains 'utc')) { return $null }
    $k = Get-CwRecordUtcKey -Value $Record.utc
    if([string]::IsNullOrWhiteSpace($k)) { return $null }
    try {
        return ([System.DateTimeOffset]::Parse($k,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [System.Globalization.DateTimeStyles]::RoundtripKind)).UtcTicks
    } catch {
        return $null
    }
}

<#
    C15-1: which recorded outcomes HALT the campaign.

    Only `not_moved` used to count, so a halt survived a leg if and only if
    the verdict was not_moved. The three STOP verdicts -- both-present,
    neither-present and could-not-observe -- are the ones that say the card
    is in a state the tool cannot explain, and they were the ones that did
    not survive. Written as a predicate rather than an inline list so the
    two places that need it (the block backstop and the undo path's
    uncertain scan) cannot drift apart.

    The verify path stamps the bare verdict; the reconcile path stamps
    "reconciled_<verdict>". Both forms halt.
#>
function Test-CwReasonIsHalt {
    param([string]$Reason)
    if([string]::IsNullOrWhiteSpace($Reason)) { return $false }
    $r = $Reason.Trim()
    if($r.StartsWith('reconciled_', [StringComparison]::OrdinalIgnoreCase)) {
        $r = $r.Substring('reconciled_'.Length)
    }
    if([string]::Equals($r, 'not_moved', [StringComparison]::OrdinalIgnoreCase)) { return $true }
    return $r.StartsWith('stop_', [StringComparison]::OrdinalIgnoreCase)
}

# -1 / 0 / 1, or $null when the two records cannot be ordered at all.
function Compare-CwRecordOrder {
    param($A, $B)
    if($null -eq $A -or $null -eq $B) { return $null }
    foreach($r in @($A, $B)) {
        foreach($f in @('cw_file_ordinal','cw_line')) {
            if(-not ($r.PSObject.Properties.Name -contains $f)) {
                throw "manifest record is not ordered (missing '$f') -- read it through Add-CwManifestOrder before comparing"
            }
        }
    }
    if([int]$A.cw_file_ordinal -eq [int]$B.cw_file_ordinal) {
        if([int]$A.cw_line -lt [int]$B.cw_line) { return -1 }
        if([int]$A.cw_line -gt [int]$B.cw_line) { return 1 }
        return 0
    }
    $ta = Get-CwRecordUtcTicks -Record $A
    $tb = Get-CwRecordUtcTicks -Record $B
    if($null -eq $ta -or $null -eq $tb) { return $null }
    if($ta -lt $tb) { return -1 }
    if($ta -gt $tb) { return 1 }
    if([int]$A.cw_file_ordinal -lt [int]$B.cw_file_ordinal) { return -1 }
    return 1
}

<#
    Does this `block_cleared` clear this block?

    An EXPLICIT pairing wins and is exact: a clear that names a block
    (block_manifest + block_seq, written by both clearing paths since C13)
    clears that block and no other, whatever the clocks say. Only a clear
    with no pairing -- a legacy record, or one written by hand -- falls back
    to the total order, and then it must be STRICTLY later.
#>
function Test-CwClearClearsBlock {
    param($Clear, $Block)
    if(-not (Test-CwNameEqual ([string]$Clear.name_hex) ([string]$Block.name_hex))) { return $false }
    $hasSeq = ($Clear.PSObject.Properties.Name -contains 'block_seq')
    $hasUtc = ($Clear.PSObject.Properties.Name -contains 'block_utc') -and
              (-not [string]::IsNullOrWhiteSpace([string]$Clear.block_utc))
    if($hasSeq -and $hasUtc) {
        # CONTENT pairing (C15-2, C15-6). `-Batch` defaults to a fresh id
        # every leg and seq restarts at 1 in each manifest, so (manifest,
        # seq) is a PREDICTABLE coordinate, not a unique one across time: a
        # clear planted with a future manifest's coordinates disarmed the
        # block when it arrived. The block's own `utc` identifies the
        # INSTANCE, which makes the pairing immune to that -- and, because
        # it no longer depends on the file's PATH, a manifest rename stops
        # resurrecting a block that was already cleared.
        if([string]$Clear.block_seq -ne [string]$Block.seq) { return $false }
        return ((Get-CwRecordUtcKey -Value $Clear.block_utc) -eq
                (Get-CwRecordUtcKey -Value $Block.utc))
    }
    $hasPath = ($Clear.PSObject.Properties.Name -contains 'block_manifest') -and
               (-not [string]::IsNullOrWhiteSpace([string]$Clear.block_manifest))
    if($hasSeq -and $hasPath) {
        # LEGACY coordinate pairing -- a clear written before block_utc
        # existed, or one written by hand. Kept working, but ORDER-BOUND,
        # because a coordinate alone can name a block that does not exist
        # yet.
        if(-not (Test-CwNameEqual ([string]$Clear.block_manifest) ([string]$Block.cw_manifest))) { return $false }
        if([string]$Clear.block_seq -ne [string]$Block.seq) { return $false }
        $cmpPaired = Compare-CwRecordOrder -A $Clear -B $Block
        if($null -eq $cmpPaired) { return $false }
        return ($cmpPaired -gt 0)
    }
    $cmp = Compare-CwRecordOrder -A $Clear -B $Block
    if($null -eq $cmp) { return $false }
    return ($cmp -gt 0)
}

<#
    N2 + T1 + T3: the not_moved BLOCK is STICKY, and it is sticky per BLOCK,
    not per world.

    Writing the reconcile `result` closes the intent, so the block used to
    disarm itself on the very run that fired it (N2). It is therefore its
    own append-only record, resolved across the UNION of the manifest
    directory -- and, since T1, cleared only by a `block_cleared` that
    PAIRS with it or provably follows it.

    T3 adds the backstop. The block record is now flushed BEFORE the result
    that closes the intent, so the crash window is one-sided; this makes it
    zero-sided by ALSO deriving a block from the result itself. A `result`
    whose reason records a not_moved observation for an ARCHIVE intent IS a
    block, whether or not the block line survived. Losing the block line
    therefore cannot lose the halt -- which is the property T3 is about.

    Records must have been read through Add-CwManifestOrder; an undecorated
    record throws rather than being silently ordered by luck.
#>
function Get-CwActiveBlocks {
    param([object[]]$Records)
    $recs = @($Records)

    # Intents, keyed per FILE + seq: an implicit block needs the world's
    # name_hex, and only the intent carries it.
    $intents = @{}
    foreach($r in $recs) {
        if(-not ($r.PSObject.Properties.Name -contains 'op')) { continue }
        if($r.op -ne 'intent') { continue }
        if(-not ($r.PSObject.Properties.Name -contains 'seq')) { continue }
        if(-not ($r.PSObject.Properties.Name -contains 'cw_file_ordinal')) {
            throw "manifest record is not ordered (missing 'cw_file_ordinal') -- read it through Add-CwManifestOrder before comparing"
        }
        $intents["$($r.cw_file_ordinal)|$($r.seq)"] = $r
    }

    $candidates = New-Object System.Collections.Generic.List[object]
    $explicit = @{}
    foreach($r in $recs) {
        if(-not ($r.PSObject.Properties.Name -contains 'op')) { continue }
        if($r.op -ne 'block') { continue }
        if(-not ($r.PSObject.Properties.Name -contains 'cw_file_ordinal')) {
            throw "manifest record is not ordered (missing 'cw_file_ordinal') -- read it through Add-CwManifestOrder before comparing"
        }
        $seq = ""
        if($r.PSObject.Properties.Name -contains 'seq') { $seq = [string]$r.seq }
        $explicit["$($r.cw_file_ordinal)|$seq"] = $true
        $candidates.Add((New-CwBlockView -Record $r -NameHex ([string]$r.name_hex) -Source 'block'))
    }
    # T3: the result-derived backstop.
    foreach($r in $recs) {
        if(-not ($r.PSObject.Properties.Name -contains 'op')) { continue }
        if($r.op -ne 'result') { continue }
        if(-not ($r.PSObject.Properties.Name -contains 'reason')) { continue }
        # C15-1: EVERY halting verdict, not only not_moved. The verify path
        # stamps the bare verdict, so a stop_both_present / stop_neither_
        # present / stop_unknown result closed its intent and the halt the
        # tool had just printed was gone from its memory one leg later.
        if(-not (Test-CwReasonIsHalt -Reason ([string]$r.reason))) { continue }
        if(-not ($r.PSObject.Properties.Name -contains 'seq')) { continue }
        if(-not ($r.PSObject.Properties.Name -contains 'cw_file_ordinal')) {
            throw "manifest record is not ordered (missing 'cw_file_ordinal') -- read it through Add-CwManifestOrder before comparing"
        }
        $key = "$($r.cw_file_ordinal)|$($r.seq)"
        if($explicit.ContainsKey($key)) { continue }   # already carried explicitly
        if(-not $intents.ContainsKey($key)) { continue }
        $intent = $intents[$key]
        # A RESTORE intent that did not move is the undo path's own problem
        # (restore_wii_saves.ps1 reports it as still displaced); it is not an
        # archive block and must not stop the archive campaign.
        if(($intent.PSObject.Properties.Name -contains 'dir') -and
           ([string]$intent.dir -eq 'restore')) { continue }
        $candidates.Add((New-CwBlockView -Record $r -NameHex ([string]$intent.name_hex) `
                            -Source 'result' -Intent $intent))
    }

    $clears = New-Object System.Collections.Generic.List[object]
    foreach($r in $recs) {
        if(-not ($r.PSObject.Properties.Name -contains 'op')) { continue }
        if($r.op -ne 'block_cleared') { continue }
        $clears.Add($r)
    }

    $out = New-Object System.Collections.Generic.List[object]
    foreach($b in $candidates.ToArray()) {
        $killed = $false
        foreach($c in $clears.ToArray()) {
            if(Test-CwClearClearsBlock -Clear $c -Block $b) { $killed = $true; break }
        }
        if(-not $killed) { $out.Add($b) }
    }
    return ,$out.ToArray()
}

<#
    C15-1(b): the ARCHIVE intents the undo path must not treat as settled.

    An intent whose recorded result is a HALT -- both-present,
    neither-present, could-not-observe -- describes a world whose position
    the tool could not establish. Get-CwIntentState marks a world Archived
    only on a PASS result, so the verify path's FAIL stamp made such a world
    invisible: for stop_unknown the world IS in the archive and the undo
    path reported "nothing to undo", which is precisely the state this whole
    design exists to make impossible.

    A `not_moved` halt is deliberately NOT in here: that world is still in
    the saves root, so there is nothing for the UNDO path to do about it
    (the archive tool's block is what makes the operator deal with it).

    Returns the intent plus the reason it was halted; the caller OBSERVES
    both ends before deciding anything.
#>
function Get-CwUncertainArchiveIntents {
    param([object[]]$Records)
    $results = @{}
    foreach($r in @($Records)) {
        if(-not ($r.PSObject.Properties.Name -contains 'op')) { continue }
        if($r.op -ne 'result') { continue }
        if(-not ($r.PSObject.Properties.Name -contains 'seq')) { continue }
        # The LAST result for a seq wins: a later observation supersedes an
        # earlier halt, which is how a resolved uncertainty stays resolved.
        $results["$($r.seq)"] = $r
    }
    $out = New-Object System.Collections.Generic.List[object]
    foreach($r in @($Records)) {
        if(-not ($r.PSObject.Properties.Name -contains 'op')) { continue }
        if($r.op -ne 'intent') { continue }
        if(($r.PSObject.Properties.Name -contains 'dir') -and ([string]$r.dir -eq 'restore')) { continue }
        if(-not ($r.PSObject.Properties.Name -contains 'seq')) { continue }
        if(-not $results.ContainsKey("$($r.seq)")) { continue }  # unfinished: the reconcile pass owns it
        $res = $results["$($r.seq)"]
        if(-not ($res.PSObject.Properties.Name -contains 'reason')) { continue }
        $reason = [string]$res.reason
        if(-not (Test-CwReasonIsHalt -Reason $reason)) { continue }
        if([string]::Equals($reason, 'not_moved', [StringComparison]::OrdinalIgnoreCase) -or
           [string]::Equals($reason, 'reconciled_not_moved', [StringComparison]::OrdinalIgnoreCase)) { continue }
        $out.Add([pscustomobject]@{ Intent = $r; Reason = $reason })
    }
    return ,$out.ToArray()
}

<#
    One block, in the shape everything downstream consumes: the card's own
    name_hex, a name the operator could type, where the record lives, and
    the order fields so a clear can be paired against it.
#>
function New-CwBlockView {
    param($Record, [string]$NameHex, [string]$Source, $Intent = $null)
    $from = ""
    $display = ""
    $reason = ""
    foreach($src in @($Record, $Intent)) {
        if($null -eq $src) { continue }
        if([string]::IsNullOrEmpty($from) -and ($src.PSObject.Properties.Name -contains 'from')) {
            $from = [string]$src.from
        }
        if([string]::IsNullOrEmpty($display) -and ($src.PSObject.Properties.Name -contains 'name_display')) {
            $display = [string]$src.name_display
        }
    }
    if($Record.PSObject.Properties.Name -contains 'reason') { $reason = [string]$Record.reason }
    $seq = ""
    if($Record.PSObject.Properties.Name -contains 'seq') { $seq = [string]$Record.seq }
    $view = [pscustomobject]@{
        name_hex = $NameHex
        from = $from
        name_display = $display
        seq = $seq
        manifest = [string]$Record.cw_manifest
        reason = $reason
        source = $Source
        cw_manifest = [string]$Record.cw_manifest
        cw_file_ordinal = [int]$Record.cw_file_ordinal
        cw_line = [int]$Record.cw_line
        # CANONICAL, not [string]. ConvertFrom-Json hands `utc` back as a
        # DateTime on this build, and `[string]` renders it in the host's
        # CULTURE -- dropping the zone. The raw record keeps its zone, so a
        # view compared against a raw clear was being compared in two
        # different zones, and a clear four hours "earlier" than its block
        # stopped clearing it. Both sides go through the same normaliser.
        utc = $(if($Record.PSObject.Properties.Name -contains 'utc') { Get-CwRecordUtcKey -Value $Record.utc } else { "" })
    }
    if([string]::IsNullOrEmpty($view.name_display)) {
        $view.name_display = Get-CwPathLeaf -Path $view.from
    }
    return $view
}

# The last component of an absolute card path. Names can never contain a
# separator (cw_saves_name_check refuses one), so this is exact.
function Get-CwPathLeaf {
    param([string]$Path)
    if([string]::IsNullOrEmpty($Path)) { return "" }
    $parts = $Path.Split('/')
    return [string]$parts[$parts.Length - 1]
}

function Get-CwBlockDisplayName {
    param($Block)
    if(($Block.PSObject.Properties.Name -contains 'name_display') -and
       -not [string]::IsNullOrEmpty([string]$Block.name_display)) {
        return [string]$Block.name_display
    }
    if($Block.PSObject.Properties.Name -contains 'from') {
        return (Get-CwPathLeaf -Path ([string]$Block.from))
    }
    return ""
}

<#
    T6 (and C11's N9, which this is the second half of): turning a name the
    operator TYPED into a block is a resolution against recorded bytes, never
    a re-encoding.

    The first -ResolveBlock compared the block's name_hex against
    ConvertTo-CwHex(UTF8($typed)) -- exactly the construct N9 removed from the
    re-issue path. For any world whose on-card bytes are not UTF-8 that can
    never match, and -ResolveBlock is by design the ONLY way out of a block on
    a world that has since been archived (the re-issue path needs the world to
    still be in the listing), so such a block was unclearable without hand
    editing the write-ahead record.

    The block already carries the CARD's name_hex and the display name the
    listing gave. A typed name is matched against that display, folding case
    like FAT; -Hex (-ResolveBlockHex) addresses a block no operator can type.
    Absent or ambiguous REFUSES BY NAME -- it never guesses an encoding.
#>
function Resolve-CwBlockTarget {
    param([object[]]$Blocks, [string]$Typed = "", [string]$Hex = "")
    $all = @($Blocks)
    if(-not [string]::IsNullOrWhiteSpace($Hex)) {
        $h = $Hex.Trim()
        if(($h.Length % 2) -ne 0 -or $h -notmatch '^[0-9a-fA-F]+$') {
            return [pscustomobject]@{ Status = 'bad_hex'; Blocks = @() }
        }
        $hit = @($all | Where-Object { Test-CwNameEqual ([string]$_.name_hex) $h })
        if(@($hit).Count -eq 0) { return [pscustomobject]@{ Status = 'absent'; Blocks = @() } }
        return [pscustomobject]@{ Status = 'ok'; Blocks = @($hit) }
    }
    if([string]::IsNullOrWhiteSpace($Typed)) {
        return [pscustomobject]@{ Status = 'absent'; Blocks = @() }
    }
    $hit = @($all | Where-Object {
        $d = Get-CwBlockDisplayName -Block $_
        (-not [string]::IsNullOrEmpty($d)) -and (Test-CwNameEqual $d $Typed) })
    if(@($hit).Count -eq 0) { return [pscustomobject]@{ Status = 'absent'; Blocks = @() } }
    # Two DIFFERENT worlds answering to one typed name is ambiguity, and the
    # operator has to disambiguate with -ResolveBlockHex. Several blocks on
    # the SAME world are one decision and are all cleared together.
    $distinct = @()
    foreach($b in @($hit)) {
        $h = [string]$b.name_hex
        if(-not (@($distinct) | Where-Object { Test-CwNameEqual $_ $h })) { $distinct += $h }
    }
    if(@($distinct).Count -gt 1) {
        return [pscustomobject]@{ Status = 'ambiguous'; Blocks = @($hit) }
    }
    return [pscustomobject]@{ Status = 'ok'; Blocks = @($hit) }
}

# ---------------------------------------------------------------------
# Write-ahead manifest (Astra change 2)
# ---------------------------------------------------------------------

<#
    One JSON object per line, FLUSHED TO DISK (FileStream.Flush($true) =
    FlushFileBuffers) after every line, and the INTENT is written and flushed
    BEFORE the request goes on the wire. A crash therefore leaves at worst a
    trailing PARTIAL line, which Read-CwManifest ignores by the JSONL rule --
    and an intent with no result, which is exactly the state the reconcile
    pass is built to resolve.
#>
<#
    Drops an unterminated trailing line before appending.

    Without this, opening a crash-truncated manifest in Append mode GLUES the
    next record onto the partial one -- `{"v":1,"op":"inte` + `{"v":1,"op":
    "result"...}` on one line -- which destroys BOTH: the reader sees one
    malformed line where a valid result should be, and (because it is no
    longer the last line) refuses the whole manifest. Caught by
    tests/test_stage_saves_tools.py's interrupted-manifest case.

    Truncating the partial record is correct, not a data loss: the write-ahead
    order is "flush the intent, THEN send the request", so a record whose
    flush never completed describes a request that was never sent.
#>
function Repair-CwManifestTail {
    param([string]$Path)
    if(-not (Test-Path -LiteralPath $Path)) { return }
    $fs = [System.IO.FileStream]::new($Path, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        if($fs.Length -eq 0) { return }
        $fs.Position = $fs.Length - 1
        if($fs.ReadByte() -eq 10) { return }
        $cut = -1
        $floor = [Math]::Max(0, $fs.Length - 1MB)
        for($pos = $fs.Length - 2; $pos -ge $floor; $pos--) {
            $fs.Position = $pos
            if($fs.ReadByte() -eq 10) { $cut = $pos + 1; break }
        }
        if($cut -lt 0) {
            if($floor -gt 0) {
                throw "manifest $Path has no line terminator in its last 1MB; refusing to guess where the partial record starts"
            }
            $cut = 0
        }
        Write-Verbose "dropping a $($fs.Length - $cut)-byte partial trailing record from $Path"
        $fs.SetLength($cut)
    } finally {
        $fs.Dispose()
    }
}

function New-CwManifestWriter {
    param([string]$Path)
    $dir = Split-Path -Parent $Path
    if(-not [string]::IsNullOrWhiteSpace($dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    Repair-CwManifestTail -Path $Path
    $fs = [System.IO.FileStream]::new($Path, [System.IO.FileMode]::Append,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
    return [pscustomobject]@{ Path = $Path; Stream = $fs }
}

function Write-CwManifestLine {
    param($Writer, [hashtable]$Record)
    $json = ($Record | ConvertTo-Json -Depth 8 -Compress)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json + "`n")
    $Writer.Stream.Write($bytes, 0, $bytes.Length)
    # $true == FlushFileBuffers: the line is on the platter before the
    # request it describes is sent. Without this the whole write-ahead
    # property is a lie.
    $Writer.Stream.Flush($true)
}

function Close-CwManifestWriter {
    param($Writer)
    if($null -ne $Writer -and $null -ne $Writer.Stream) {
        $Writer.Stream.Dispose()
    }
}

<#
    Reads a manifest, tolerating exactly one malformed TRAILING line (the
    crash signature). A malformed line anywhere else is a corrupt manifest
    and throws -- silently skipping it could hide a recorded intent.
#>
function Read-CwManifest {
    param([string]$Path)
    # A path returned by Get-CwManifestPaths is evidence that a manifest
    # existed.  If it disappears before the read, treating it as an empty
    # manifest drops every intent and halt it contained.
    if(-not (Test-Path -LiteralPath $Path)) { throw "manifest disappeared before it could be read: $Path" }
    $lines = @(Get-Content -LiteralPath $Path -Encoding UTF8)
    $out = New-Object System.Collections.Generic.List[object]
    # C32 (inventory :903-926,573-693,968-982). Every consumer of these
    # records SKIPS what it does not understand -- `if($r.op -ne 'intent')
    # { continue }` -- so a record with no op, an op from a newer tool, or a
    # missing field was silently invisible to the open-intent scan, the
    # block scan and the orphan guard alike. The reader is now what refuses
    # it, once, with the record quoted.
    $knownOps = @('batch_begin','intent','result','block','block_cleared')
    $required = @{
        'batch_begin' = @('batch');
        'intent'      = @('seq','dir','name_hex');
        'result'      = @('seq','status');
        'block'       = @('name_hex');
        'block_cleared' = @('name_hex')
    }
    $intentSeqs = @{}
    for($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if([string]::IsNullOrWhiteSpace($line)) {
            # A blank line is only ever the harmless tail of the file. One
            # BETWEEN records means something other than this tool wrote
            # here, and a manifest that was edited is not a record.
            if($i -eq $lines.Count - 1) { continue }
            throw "manifest $Path has a blank line at $($i + 1) that is not the last line"
        }
        $obj = $null
        try { $obj = $line | ConvertFrom-Json } catch { $obj = $null }
        if($null -eq $obj) {
            if($i -eq $lines.Count - 1) {
                Write-Verbose "ignoring trailing partial manifest line (expected after an interrupted write)"
                continue
            }
            throw "manifest $Path has a malformed line at $($i + 1) that is not the last line"
        }
        if(-not ($obj.PSObject.Properties.Name -contains 'op') -or
           [string]::IsNullOrWhiteSpace([string]$obj.op)) {
            throw "manifest $Path line $($i + 1) has no 'op': $line"
        }
        $op = [string]$obj.op
        if(-not ($knownOps -contains $op)) {
            throw "manifest $Path line $($i + 1) has unknown op '$op': $line"
        }
        foreach($f in @($required[$op])) {
            if(-not ($obj.PSObject.Properties.Name -contains $f)) {
                throw "manifest $Path line $($i + 1) is a '$op' record missing required field '$f': $line"
            }
        }
        if($op -eq 'intent') {
            if(@('archive','restore') -notcontains [string]$obj.dir) {
                throw "manifest $Path line $($i + 1) has invalid intent direction '$($obj.dir)': $line"
            }
            $k = [string]$obj.seq
            if($intentSeqs.ContainsKey($k)) {
                throw "manifest $Path line $($i + 1) repeats intent seq $k (first seen at line $($intentSeqs[$k])): $line"
            }
            $intentSeqs[$k] = $i + 1
        }
        if($op -eq 'result' -and @('PASS','FAIL') -notcontains [string]$obj.status) {
            throw "manifest $Path line $($i + 1) has invalid result status '$($obj.status)': $line"
        }
        if($obj.op -eq 'batch_begin' -and ($obj.PSObject.Properties.Name -contains 'card_identity')) {
            # ConvertFrom-Json may turn an ISO-looking identity into local DateTime.
            $obj.card_identity = Get-CwOpaqueJsonString -Json $line -Property 'card_identity'
        }
        $out.Add($obj)
    }
    return ,$out.ToArray()
}

function Get-CwOpaqueJsonString {
    param([string]$Json, [string]$Property)
    if(-not ('System.Text.Json.JsonDocument' -as [type])) {
        # Windows PowerShell 5.1 (.NET Framework) has no System.Text.Json;
        # JavaScriptSerializer keeps ISO-looking strings as strings too.
        Add-Type -AssemblyName System.Web.Extensions
        $ser = New-Object System.Web.Script.Serialization.JavaScriptSerializer
        $root = $ser.DeserializeObject($Json)
        if(-not ($root -is [System.Collections.IDictionary]) -or -not $root.ContainsKey($Property)) {
            throw "JSON property '$Property' is missing"
        }
        $value = $root[$Property]
        if(-not ($value -is [string])) {
            throw "JSON property '$Property' must be a string"
        }
        return $value
    }
    $doc = [System.Text.Json.JsonDocument]::Parse($Json)
    try {
        $value = $doc.RootElement.GetProperty($Property)
        if($value.ValueKind -ne [System.Text.Json.JsonValueKind]::String) {
            throw "JSON property '$Property' must be a string"
        }
        return $value.GetString()
    } finally { $doc.Dispose() }
}

<#
    CALLING CONVENTION for every helper here that returns `,<array>`: ASSIGN
    the result to a variable first, then wrap that variable in @(). The
    leading comma keeps a one-element result from unrolling into a scalar,
    but it also means the call emits ONE object that IS the array -- so
    `@(Func ...)` inline yields a 1-element array containing an EMPTY array
    (not an empty array), and `Func ... | Where-Object { $_.x }` hands
    Where-Object that array instead of its elements. Both bugs were live
    here and are pinned by Invoke-CwSavesSelfTest.
#>
<#
    Every manifest under the manifest directory, newest last.

    The reconcile pass used to open exactly ONE file -- manifest_<this
    batch>.jsonl -- while the runbook never passes -Batch, so every
    invocation minted a new id, opened a file that did not exist, and
    reconciled nothing. The write-ahead record was being written correctly
    and then never read on the only documented path (C9 review, C2).
#>
function Get-CwManifestPaths {
    param([string]$ManifestDir)
    if(-not (Test-Path -LiteralPath $ManifestDir)) { return ,@() }
    $files = @(Get-ChildItem -LiteralPath $ManifestDir -Filter 'manifest_*.jsonl' -File |
               Sort-Object Name | ForEach-Object { $_.FullName })
    return ,$files
}

function Get-CwUnfinishedIntents {
    param([object[]]$Records)
    $results = @{}
    foreach($r in @($Records)) {
        if($r.PSObject.Properties.Name -contains 'op' -and $r.op -eq 'result') {
            $results["$($r.seq)"] = $true
        }
    }
    $out = New-Object System.Collections.Generic.List[object]
    foreach($r in @($Records)) {
        if($r.PSObject.Properties.Name -contains 'op' -and $r.op -eq 'intent') {
            if(-not $results.ContainsKey("$($r.seq)")) { $out.Add($r) }
        }
    }
    return ,$out.ToArray()
}

<#
    The reconcile truth table (Astra change 2). NEVER a blind retry: the
    verdict comes from OBSERVING both ends after the interruption.

      src present, dst absent  -> not_moved  (safe to re-issue ONCE)
      src absent,  dst present -> moved      (write the missing result)
      both present             -> STOP       (collision or torn dir entry)
      neither present          -> STOP       (missing data -- escalate,
                                              touch nothing)
#>
function Resolve-CwReconcileVerdict {
    param(
        [ValidateSet('present','absent','unknown')][string]$Src,
        [ValidateSet('present','absent','unknown')][string]$Dst
    )
    # "I could not look" is NOT "it is not there". Folding an unobservable
    # end into $false wrote a guess into the manifest as an observation, and
    # the truth table below then consumed it as fact (C9 review, M4).
    if($Src -eq 'unknown' -or $Dst -eq 'unknown') { return 'stop_unknown' }
    if($Src -eq 'present' -and $Dst -eq 'present') { return 'stop_both_present' }
    if($Src -eq 'absent' -and $Dst -eq 'absent') { return 'stop_neither_present' }
    if($Dst -eq 'present') { return 'moved' }
    return 'not_moved'
}

# Maps a reconcile verdict onto the exit code the tools must use, so the two
# tools cannot drift apart on what a STOP means.
function Get-CwVerdictExit {
    param([string]$Verdict)
    switch($Verdict) {
        'stop_both_present'    { return $script:CwSavesExit.StopBothPresent }
        'stop_neither_present' { return $script:CwSavesExit.StopNeitherPresent }
        'stop_unknown'         { return $script:CwSavesExit.UnknownObservation }
        default                { return $script:CwSavesExit.Ok }
    }
}

function Get-CwVerdictMessage {
    param([string]$Verdict, [string]$What)
    switch($Verdict) {
        'stop_both_present'    { return "$What is present at BOTH ends (name collision or a torn directory entry). Touch nothing; investigate on the card." }
        'stop_neither_present' { return "$What is present at NEITHER end (missing data). Touch nothing; escalate." }
        'stop_unknown'         { return "$What could not be observed at one or both ends -- the listing failed, so 'absent' is a GUESS and this tool will not make one. Re-run once the server answers." }
        default                { return "$What -> $Verdict" }
    }
}

# ---------------------------------------------------------------------
# Rig lock (design S6 / Astra change 5: no nested acquisition)
# ---------------------------------------------------------------------

<#
    mkdir is the atomic take (KANBAN.md's rig-booking line). A caller that
    ALREADY holds the lock -- a rig runner invoking the cleanup mid-batch --
    passes -LockHeldBy <owner>, which is checked against owner.txt and then
    reuses the lock instead of acquiring a second one. It never releases a
    lock it did not take.
#>
function Enter-CwRigLock {
    param(
        [string]$LockPath = $script:CwSavesRigLock,
        [string]$Owner,
        [string]$LockHeldBy = ""
    )
    $ownerFile = Join-Path $LockPath 'owner.txt'
    if(-not [string]::IsNullOrWhiteSpace($LockHeldBy)) {
        if(-not (Test-Path -LiteralPath $LockPath)) {
            throw "-LockHeldBy '$LockHeldBy' was given but $LockPath does not exist -- the caller does not actually hold the rig lock"
        }
        # EQUALITY against a whole line, never a substring: -notmatch is
        # unanchored, so "a", "/", "ps1" and "wii" all "matched" a lock held
        # by a different session and two sessions could drive renames on one
        # card (C9 review, M6).
        $lines = @()
        if(Test-Path -LiteralPath $ownerFile) {
            $lines = @(Get-Content -LiteralPath $ownerFile | ForEach-Object { $_.Trim() })
        }
        $want = $LockHeldBy.Trim()
        $hit = $false
        foreach($line in $lines) { if($line -ceq $want) { $hit = $true } }
        if(-not $hit) {
            throw "rig lock $LockPath is held by someone else (no line of owner.txt is exactly '$want'; it holds: $($lines -join ' | '))"
        }
        return [pscustomobject]@{ Path = $LockPath; Acquired = $false; Owner = $want }
    }
    if(Test-Path -LiteralPath $LockPath) {
        $text = ""
        if(Test-Path -LiteralPath $ownerFile) {
            $text = (Get-Content -Raw -LiteralPath $ownerFile)
        }
        throw "rig lock $LockPath is TAKEN: $text"
    }
    New-Item -ItemType Directory -Path $LockPath -ErrorAction Stop | Out-Null
    Set-Content -LiteralPath $ownerFile -Encoding UTF8 -Value $Owner
    return [pscustomobject]@{ Path = $LockPath; Acquired = $true; Owner = $Owner }
}

function Exit-CwRigLock {
    param($Lock)
    if($null -ne $Lock -and $Lock.Acquired) {
        Remove-Item -LiteralPath $Lock.Path -Recurse -Force -ErrorAction SilentlyContinue
    }
}

<#
    The saves root is bound ONCE for the whole clean-up and then re-verified
    BY IDENTITY, never by a count.

    The count threshold was a trap: the stated goal is 646 worlds -> 2, so
    once the bound root drops below it every later invocation exits 8 and the
    operator has to pass -SkipRootBinding for the tail of the run -- i.e. the
    safety check switches itself off exactly when the two keepers are among
    the handful of remaining directories and a wrong root is most costly
    (C9 review, H4).

    The evidence that survives the batch is the KEEP set: those directories
    are by definition never archived, so "the bound root still holds every
    keeper, and reports the same root_abs" is a stable identity test. A keep
    set that no longer matches the recorded one is itself a STOP -- the
    operator changed what they promised to protect.
#>
function Read-CwRootBinding {
    param([string]$ManifestDir)
    $path = Join-Path $ManifestDir 'root_binding.json'
    if(-not (Test-Path -LiteralPath $path)) { return $null }
    $json = Get-Content -Raw -LiteralPath $path
    $binding = $json | ConvertFrom-Json
    foreach($field in @('root','root_abs','bound_utc','keep_hex')) {
        if(-not ($binding.PSObject.Properties.Name -contains $field)) {
            throw "root binding $path is missing required field '$field'"
        }
    }
    foreach($field in @('root','root_abs','bound_utc')) {
        if([string]::IsNullOrWhiteSpace([string]$binding.$field)) {
            throw "root binding $path has blank required field '$field'"
        }
    }
    if(([string]$binding.bound_utc).StartsWith('unbound:', [StringComparison]::OrdinalIgnoreCase)) {
        throw "root binding $path has placeholder rather than card identity '$($binding.bound_utc)'"
    }
    if(@('saves','saves_app') -notcontains [string]$binding.root) {
        throw "root binding $path has unsupported root '$($binding.root)'"
    }
    $expectedAbs = $(if([string]$binding.root -eq 'saves') { 'sd:/saves' } else { 'sd:/apps/CavEX/saves' })
    if([string]$binding.root_abs -ne $expectedAbs) {
        throw "root binding $path root_abs '$($binding.root_abs)' does not match root '$($binding.root)' ($expectedAbs)"
    }
    $binding.bound_utc = Get-CwOpaqueJsonString -Json $json -Property 'bound_utc'
    if($null -eq $binding.keep_hex -or -not ($binding.keep_hex -is [array])) {
        throw "root binding $path field 'keep_hex' must be an array"
    }
    if(@($binding.keep_hex).Count -eq 0) { throw "root binding $path has an empty keep_hex set" }
    foreach($h in @($binding.keep_hex)) {
        if(-not ($h -is [string]) -or [string]$h -notmatch '^(?:[0-9a-fA-F]{2})+$') {
            throw "root binding $path contains malformed keep_hex value '$h'"
        }
    }
    return $binding
}

function Write-CwRootBinding {
    param([string]$ManifestDir, [hashtable]$Binding)
    New-Item -ItemType Directory -Force -Path $ManifestDir | Out-Null
    $path = Join-Path $ManifestDir 'root_binding.json'
    # BOM-less on Windows PowerShell 5.1 too: strict JSON readers reject a BOM.
    [System.IO.File]::WriteAllText($path, ($Binding | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding $false))
    return $path
}

# ---------------------------------------------------------------------
# Bounded level.dat decode (design S3)
# ---------------------------------------------------------------------

$script:CwGunzipMaxBytes = 1MB
$script:CwNbtMaxDepth = 32
$script:CwNbtMaxTags = 4096
$script:CwNbtMaxStringBytes = 64

function Expand-CwGzipBounded {
    param([byte[]]$Bytes)
    if($null -eq $Bytes -or $Bytes.Length -lt 2) { throw "level.dat is too short" }
    if($Bytes[0] -ne 0x1f -or $Bytes[1] -ne 0x8b) { throw "level.dat is not gzip" }
    $in = [System.IO.MemoryStream]::new($Bytes)
    $gz = [System.IO.Compression.GZipStream]::new($in, [System.IO.Compression.CompressionMode]::Decompress)
    try {
        $out = [System.IO.MemoryStream]::new()
        $buf = New-Object byte[] 8192
        while($true) {
            $n = $gz.Read($buf, 0, $buf.Length)
            if($n -le 0) { break }
            if(($out.Length + $n) -gt $script:CwGunzipMaxBytes) {
                # Decompression bomb guard: abort, never allocate past the cap.
                throw "level.dat expands past the $($script:CwGunzipMaxBytes)-byte cap"
            }
            $out.Write($buf, 0, $n)
        }
        return ,$out.ToArray()
    } finally {
        $gz.Dispose(); $in.Dispose()
    }
}

<#
    Bounded NBT walk that returns .Data.LevelName, or $null if it is not
    there. Depth 32, 4096 tags, 64-byte string cap (the game reads LevelName
    into a char[64], screen_select_world.c:754). Any breach means "name
    undecodable" for that one world -- never a crashed run.
#>
function Get-CwLevelName {
    param([byte[]]$Nbt)
    $script:_nbtPos = 0
    $script:_nbtTags = 0
    $script:_nbtFound = $null
    $len = $Nbt.Length

    function _need([int]$n) {
        if(($script:_nbtPos + $n) -gt $len) { throw "nbt truncated" }
    }
    function _u8 { _need 1; $v = $Nbt[$script:_nbtPos]; $script:_nbtPos++; return $v }
    function _u16 { _need 2; $v = ([int]$Nbt[$script:_nbtPos] -shl 8) -bor [int]$Nbt[$script:_nbtPos + 1]; $script:_nbtPos += 2; return $v }
    function _i32 {
        _need 4
        $v = ([int64]$Nbt[$script:_nbtPos] -shl 24) -bor ([int64]$Nbt[$script:_nbtPos+1] -shl 16) -bor ([int64]$Nbt[$script:_nbtPos+2] -shl 8) -bor [int64]$Nbt[$script:_nbtPos+3]
        $script:_nbtPos += 4
        if($v -gt 2147483647) { $v = $v - 4294967296 }
        return [int]$v
    }
    function _skip([int]$n) { _need $n; $script:_nbtPos += $n }
    function _str {
        $n = _u16
        _need $n
        $s = $null
        if($n -le $script:CwNbtMaxStringBytes) {
            $s = [System.Text.Encoding]::UTF8.GetString($Nbt, $script:_nbtPos, $n)
        }
        $script:_nbtPos += $n
        return $s
    }
    function _payload([int]$type, [string]$name, [int]$depth) {
        $script:_nbtTags++
        if($script:_nbtTags -gt $script:CwNbtMaxTags) { throw "nbt tag budget exceeded" }
        if($depth -gt $script:CwNbtMaxDepth) { throw "nbt too deep" }
        switch($type) {
            1 { _skip 1 }
            2 { _skip 2 }
            3 { _skip 4 }
            4 { _skip 8 }
            5 { _skip 4 }
            6 { _skip 8 }
            7 { $n = _i32; if($n -lt 0) { throw "nbt bad array" }; _skip $n }
            8 {
                $s = _str
                if($name -eq 'LevelName' -and $null -ne $s -and $null -eq $script:_nbtFound) {
                    $script:_nbtFound = $s
                }
            }
            9 {
                $et = _u8
                $n = _i32
                if($n -lt 0) { $n = 0 }
                for($i = 0; $i -lt $n; $i++) { _payload $et '' ($depth + 1) }
            }
            10 {
                while($true) {
                    $t = _u8
                    if($t -eq 0) { break }
                    $nm = _str
                    _payload $t $nm ($depth + 1)
                }
            }
            11 { $n = _i32; if($n -lt 0) { throw "nbt bad array" }; _skip (4 * $n) }
            default { throw "nbt unknown tag type $type" }
        }
    }

    try {
        $t = _u8
        if($t -ne 10) { throw "nbt root is not a compound" }
        $null = _str
        _payload 10 '' 0
    } catch {
        return $null
    }
    return $script:_nbtFound
}

# ---------------------------------------------------------------------
# Automatic clean-up: "which world did THIS run create?" (Astra change 5)
# ---------------------------------------------------------------------

<#
    There is NO marker today: nothing in hw_run.c / hw_telemetry.c / main.c
    emits the created directory name (only a stderr line on failure,
    screen_select_world.c:617,635). So identification is a LIST DIFF
    BRACKETING the run under the continuously-held rig lock, corroborated by
    all three of the design's signals. ANY disagreement returns RETAIN with
    the reason -- ambiguity never archives.

    This is the LIBRARY FUNCTION ONLY. Nothing here is wired into a rig
    runner; that is a later, separately reviewed step, and the create/verify
    pairs in $script:CwSavesCreateVerifyPairs must be protected when it is.

    Parameters:
      BeforeNames   directory names listed immediately BEFORE the run
      AfterEntries  entries listed AFTER the run and after the console is
                    back on the stage server: name, mtime, level_name
      RunStartMtime / RunEndMtime   the run window, in the console's own FAT
                    clock (never a PC UTC value -- FAT mtime is
                    timezone-free LOCAL console time at 2 s granularity)
      DialogFolder  the folder a scripted Create leg made, when known
      Protected     keepers + named fixtures, subtracted before anything else
      RunPassed     gate: a FAILED run's worlds are always preserved
      OtherSessionTouchedCard / PairInFlight   hard RETAIN-everything flags
#>
function Select-CwRunCreatedWorlds {
    param(
        [string[]]$BeforeNames = @(),
        [object[]]$AfterEntries = @(),
        [long]$RunStartMtime = 0,
        [long]$RunEndMtime = 0,
        [string]$DialogFolder = "",
        [string[]]$Protected = @(),
        # C32 (inventory :1271-1305): defaulting to "the run passed" meant a
        # caller that simply did not supply the evidence got the ARCHIVE
        # answer. $null is now "no run evidence", which RETAINS.
        $RunPassed = $null,
        [bool]$OtherSessionTouchedCard = $false,
        [bool]$PairInFlight = $false,
        # A rig leftover is ~0.05 MB (design S2). Anything materially bigger
        # is somebody's world, not this run's litter.
        [long]$MaxTinyBytes = 262144
    )

    $before = @($BeforeNames)
    $after = @($AfterEntries)
    $out = New-Object System.Collections.Generic.List[object]

    # A world that VANISHED between the brackets means something other than
    # this run changed the card: the whole diff is untrustworthy.
    $afterNames = @()
    foreach($ae in $after) { $afterNames += [string]$ae.name }
    $vanished = @($before | Where-Object { $n = $_; -not (@($afterNames) | Where-Object { Test-CwNameEqual $_ $n }) })
    $globalStop = $null
    if($vanished.Count -gt 0) {
        $globalStop = "a world present before the run is gone after it ($($vanished -join ', ')) -- another session touched the card"
    } elseif($OtherSessionTouchedCard) {
        $globalStop = "the caller reported the card was touched by another session"
    } elseif($PairInFlight) {
        $globalStop = "a create/verify profile pair is in flight (see CwSavesCreateVerifyPairs); no cleanup between the two legs"
    } elseif($null -eq $RunPassed) {
        $globalStop = "no run verdict was supplied (-RunPassed), so there is no evidence this world belongs to a passing run"
    } elseif(@($BeforeNames).Count -eq 0) {
        $globalStop = "no before-listing was supplied, so every world on the card looks new"
    } elseif(-not [bool]$RunPassed) {
        $globalStop = "the run did not PASS -- a failed run's world is evidence"
    }

    # The highest "+new" ordinal that already existed. `+new` reserves the
    # first FREE "New World <N>" by probing upward, so this run's world can
    # only be one whose ordinal is above every ordinal present before it --
    # and a freed name gets REUSED later, so an ordinal at or below this is
    # someone else's world wearing a recycled name.
    $maxBefore = 0
    foreach($b in $before) {
        if($b -match '^New World ([0-9]+)$') {
            $n = [int]$Matches[1]
            if($n -gt $maxBefore) { $maxBefore = $n }
        }
    }

    $candidates = New-Object System.Collections.Generic.List[object]
    foreach($e in $after) {
        $name = [string]$e.name
        $isNew = -not (@($before) | Where-Object { Test-CwNameEqual $_ $name })
        if(-not $isNew) { continue }
        $candidates.Add($e)
    }
    $candidateNames = @()
    foreach($c in $candidates.ToArray()) { $candidateNames += [string]$c.name }

    foreach($e in $candidates.ToArray()) {
        $name = [string]$e.name

        if($null -ne $globalStop) {
            $out.Add([pscustomobject]@{ Name = $name; Action = 'RETAIN'; Reason = $globalStop })
            continue
        }
        if(@($Protected) | Where-Object { Test-CwNameEqual $_ $name }) {
            $out.Add([pscustomobject]@{ Name = $name; Action = 'RETAIN'; Reason = 'protected (keeper or named fixture)' })
            continue
        }
        # A run creates ONE world. When two appeared in the window the
        # evidence cannot belong to both, and the rig has launched a chain
        # twice before, so the honest answer is to keep both and say so
        # (C9 review, H5b).
        if($candidates.Count -gt 1) {
            $out.Add([pscustomobject]@{ Name = $name; Action = 'RETAIN'
                Reason = "more than one candidate appeared in the run window ($($candidateNames -join ', ')) -- a run creates one world, so this evidence identifies none of them" })
            continue
        }

        $reasons = New-Object System.Collections.Generic.List[string]

        # Signal 1: the EXACT auto-name shape with an ordinal above every
        # ordinal that existed before, or this run's own dialog folder. A
        # bare "New World" is deliberately NOT enough: the in-game default
        # display name is also "New World", so a hand-made world matched all
        # three of the original signals (C9 review, H5a).
        $nameOk = $false
        if($name -match '^New World ([0-9]+)$') {
            $n = [int]$Matches[1]
            if($n -gt $maxBefore) {
                $nameOk = $true
            } else {
                $reasons.Add("'$name' has ordinal $n, which is not above every ordinal already on the card ($maxBefore) -- '+new' reuses freed names, so this could be an older world's recycled name")
            }
        } elseif(-not [string]::IsNullOrWhiteSpace($DialogFolder) -and (Test-CwNameEqual $name $DialogFolder)) {
            $nameOk = $true
        } else {
            $reasons.Add("'$name' is not the exact auto-name shape 'New World <N>' nor this run's dialog folder -- a hand-made world can carry the same default display name, so the name alone cannot tell them apart")
        }

        # Signal 2: the FAT mtime falls inside the run window.
        $mtime = 0
        if($e.PSObject.Properties.Name -contains 'mtime') { $mtime = [long]$e.mtime }
        if($RunStartMtime -le 0 -or $RunEndMtime -le 0) {
            $reasons.Add("no run window was supplied, so the mtime signal cannot be checked")
        } elseif($mtime -lt $RunStartMtime -or $mtime -gt $RunEndMtime) {
            $reasons.Add("mtime $mtime is outside the run window [$RunStartMtime, $RunEndMtime]")
        }

        # Signal 3: the display name the "+new" path always writes.
        $levelName = $null
        if($e.PSObject.Properties.Name -contains 'level_name') { $levelName = [string]$e.level_name }
        if([string]::IsNullOrEmpty($levelName)) {
            $reasons.Add("level.dat display name was not decoded")
        } elseif($levelName -cne 'New World') {
            $reasons.Add("display name '$levelName' is not the '+new' default 'New World'")
        }

        # Signal 4: it has to be TINY. A rig leftover is ~0.05 MB; a world
        # somebody built is not, and an unknown size is ambiguity.
        # C32 (inventory :1390-1402): the field being PRESENT was taken for
        # the size being KNOWN, and [long]$null is 0 -- so a row the server
        # marked "I could not walk this world" (total_bytes:null) satisfied
        # the tiny-world bound outright.
        if(-not ($e.PSObject.Properties.Name -contains 'total_bytes') -or
           $null -eq $e.total_bytes -or
           -not ($e.total_bytes -is [int] -or $e.total_bytes -is [long] -or
                 $e.total_bytes -is [double] -or $e.total_bytes -is [decimal])) {
            $reasons.Add("size is unknown (list with -Summary) -- a world that is not demonstrably tiny is not demonstrably litter")
        } elseif([long]$e.total_bytes -gt $MaxTinyBytes) {
            $reasons.Add("total_bytes $($e.total_bytes) is above the $MaxTinyBytes-byte rig-leftover bound")
        }

        if($reasons.Count -eq 0) {
            $out.Add([pscustomobject]@{ Name = $name; Action = 'ARCHIVE'; Reason = 'all signals agree' })
        } else {
            $out.Add([pscustomobject]@{ Name = $name; Action = 'RETAIN'; Reason = ($reasons.ToArray() -join '; ') })
        }
    }
    return ,$out.ToArray()
}

# ---------------------------------------------------------------------
# Stage-server client (mirrors tools/get_wii_stage_manifest.ps1)
# ---------------------------------------------------------------------

function Connect-CwStage {
    param(
        [string]$RemoteHost,
        [int]$RemotePort,
        [int]$TimeoutSeconds
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $client = $null
    while($null -eq $client -and (Get-Date) -lt $deadline) {
        $remainingMs = [int][Math]::Max(1, ($deadline - (Get-Date)).TotalMilliseconds)
        $attemptMs = [Math]::Min(1000, $remainingMs)
        $tcp = [System.Net.Sockets.TcpClient]::new()
        try {
            $iar = $tcp.BeginConnect($RemoteHost, $RemotePort, $null, $null)
            if($iar.AsyncWaitHandle.WaitOne($attemptMs, $false)) {
                $tcp.EndConnect($iar)
                $client = $tcp
            } else {
                $tcp.Close()
            }
        } catch {
            $tcp.Close()
        }
        if($null -eq $client) { Start-Sleep -Milliseconds 200 }
    }
    if($null -eq $client -or -not $client.Connected) {
        throw "could not connect to stage server ${RemoteHost}:$RemotePort"
    }
    return $client
}

function Send-CwLine {
    param([System.Net.Sockets.NetworkStream]$Stream, [string]$Line)
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Line + "`n")
    $Stream.Write($bytes, 0, $bytes.Length)
    $Stream.Flush()
}

function Read-CwLine {
    param([System.Net.Sockets.NetworkStream]$Stream)
    # Keep the identical one-byte framing (no read-ahead into another reply),
    # but avoid interpreting a PowerShell loop for every byte of an inventory.
    if (-not ('CwStageLineReader' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
public static class CwStageLineReader {
    public static string Read(Stream stream) {
        var bytes = new List<byte>();
        var one = new byte[1];
        while (true) {
            int n = stream.Read(one, 0, 1);
            if (n <= 0) throw new IOException("stage server closed while reading line");
            if (one[0] == 10) break;
            if (one[0] != 13) bytes.Add(one[0]);
        }
        return Encoding.UTF8.GetString(bytes.ToArray());
    }
}
'@
    }
    return [CwStageLineReader]::Read($Stream)
}

function Assert-CwPassJson {
    param([string]$Line, [string]$Operation)
    $reply = $null
    try { $reply = $Line | ConvertFrom-Json } catch {
        throw "$Operation returned invalid JSON: $Line"
    }
    if($reply.status -ne "PASS") { throw "$Operation failed: $Line" }
    return $reply
}

<#
    spaced-paths (2026-10-30). A stage path argument the server will take
    byte-for-byte: "sd:/...", printable ASCII only, no '"' (the server's
    quote character, and illegal on FAT), no "..", no "//", and no path
    segment that starts or ends with a space (FAT drops trailing spaces, so
    "a /b" would land somewhere else). Returns $null when fine, else why.
#>
function Get-CwStagePathProblem {
    param([string]$Path)
    if([string]::IsNullOrEmpty($Path)) { return "empty path" }
    if(-not $Path.StartsWith("sd:/")) { return "path must start with sd:/" }
    foreach($ch in $Path.ToCharArray()) {
        $c = [int]$ch
        if($c -lt 0x20 -or $c -gt 0x7e) { return ("byte 0x{0:x2} is not printable ASCII" -f $c) }
    }
    if($Path.Contains('"')) { return "a double quote cannot be sent in a path" }
    if($Path.Contains('..')) { return "'..' is refused" }
    if($Path.Substring(4).Contains('//')) { return "empty path segment '//'" }
    foreach($seg in $Path.Substring(4).Split('/')) {
        if($seg.Length -gt 0 -and ($seg.StartsWith(' ') -or $seg.EndsWith(' '))) {
            return "segment '$seg' starts or ends with a space"
        }
    }
    if($Path.Length -gt 255) { return "path longer than 255 bytes" }
    return $null
}

<#
    A server without "spaced-paths" cuts a path at its first space and would
    write/delete the wrong thing (the stray ".../saves/CaveWii" file), so a
    spaced path is never sent to it.
#>
function Assert-CwSpacedPathSupported {
    param([string[]]$Capabilities, [string]$Path)
    if($Path.Contains(' ') -and -not (@($Capabilities) -contains 'spaced-paths')) {
        throw "CW_SAVES_SERVER_TOO_OLD: path '$Path' has a space but the stage server does not advertise 'spaced-paths' -- push a stage server built from this tree first"
    }
}

<#
    CWDELETE1 <file|dir|tree> <path> on an open, handshaken connection.
    Returns the parsed reply (status PASS reason deleted|absent, or FAIL with
    the server's reason); bytes = entries removed.
#>
function Invoke-CwStageDelete {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [ValidateSet('file','dir','tree')][string]$Mode,
        [string]$RemotePath
    )
    Send-CwLine -Stream $Stream -Line ("CWDELETE1 $Mode $RemotePath")
    $line = Read-CwLine -Stream $Stream
    try { $reply = $line | ConvertFrom-Json } catch { throw "delete returned invalid JSON: $line" }
    return [pscustomobject]@{ Line = $line; Reply = $reply }
}

function Get-CwStageToken {
    param([string]$Explicit, [string]$RepoRoot)
    if(-not [string]::IsNullOrWhiteSpace($Explicit)) { return $Explicit.Trim() }
    if(-not [string]::IsNullOrWhiteSpace($env:CW_STAGE_TOKEN)) { return $env:CW_STAGE_TOKEN.Trim() }
    $tokenFile = Join-Path $RepoRoot "tools\.stage_token"
    if(Test-Path -LiteralPath $tokenFile) {
        $fileToken = (Get-Content -Raw -LiteralPath $tokenFile).Trim()
        if(-not [string]::IsNullOrWhiteSpace($fileToken)) { return $fileToken }
    }
    return ""
}

<#
    CWHELLO1 + capability gate + CWAUTH1. The capability list is how a PC
    tool refuses an OLD server (design S1): a stage server without
    saves-list/saves-hash/saves-archive simply does not have these verbs, and
    guessing would get a "bad_header" that looks like a protocol desync.
#>
function Invoke-CwHandshake {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [string]$Token,
        [string[]]$RequireCapabilities = @()
    )
    Send-CwLine -Stream $Stream -Line "CWHELLO1"
    $helloLine = Read-CwLine -Stream $Stream
    $hello = Assert-CwPassJson -Line $helloLine -Operation "hello"
    $helloReason = Get-CwRequiredString -Object $hello -Field 'reason' -Where 'hello reply'
    if($helloReason -ne 'hello') { throw "hello reply reason='$helloReason', expected 'hello'" }
    $helloProtocol = Get-CwRequiredString -Object $hello -Field 'protocol' -Where 'hello reply'
    if($helloProtocol -ne 'CWSTAGE4') { throw "hello reply protocol='$helloProtocol', expected 'CWSTAGE4'" }
    $null = Get-CwRequiredBool -Object $hello -Field 'token_provisioned' -Where 'hello reply'
    $recovery = "CW_STAGE_TOKEN_LOCKED: token_locked -- delete or fix sd:/apps/CavEX/stage_token.txt on the card, then reboot"
    if(($hello.PSObject.Properties.Name -contains 'token_locked')) {
        $null = Get-CwRequiredBool -Object $hello -Field 'token_locked' -Where 'hello reply'
    }
    if(($hello.PSObject.Properties.Name -contains 'token_locked') -and [bool]$hello.token_locked) {
        throw $recovery
    }
    if(-not ($hello.PSObject.Properties.Name -contains 'capabilities') -or
       $null -eq $hello.capabilities -or $hello.capabilities -is [string]) {
        throw "hello reply field 'capabilities' must be an array"
    }
    $caps = @($hello.capabilities)
    foreach($cap in $caps) {
        if(-not ($cap -is [string]) -or [string]::IsNullOrWhiteSpace([string]$cap)) {
            throw "hello reply field 'capabilities' contains a non-string or blank value"
        }
    }
    foreach($need in @($RequireCapabilities)) {
        if(-not (@($caps) -contains $need)) {
            throw "CW_SAVES_SERVER_TOO_OLD: stage server does not advertise capability '$need' (has: $($caps -join ', ')) -- push a stage server built from this tree"
        }
    }
    # The server publishes saves_protocol on every hello; reading it is what
    # makes it a version gate rather than a decorative field (C9 review, L2).
    # A capability list can only say "this verb exists" -- it cannot say the
    # wire shape of that verb changed underneath us.
    if(@($RequireCapabilities).Count -gt 0) {
        $sp = Get-CwRequiredInt -Object $hello -Field 'saves_protocol' -Where 'hello reply'
        if($sp -ne $script:CwSavesProtocol) {
            throw "CW_SAVES_SERVER_TOO_OLD: stage server speaks saves_protocol $sp, these tools speak $($script:CwSavesProtocol) -- push a stage server built from this tree"
        }
    }
    Send-CwLine -Stream $Stream -Line ("CWAUTH1" + $(if([string]::IsNullOrWhiteSpace($Token)) { "" } else { " $Token" }))
    $authLine = Read-CwLine -Stream $Stream
    try { $authReply = $authLine | ConvertFrom-Json } catch { throw "auth returned invalid JSON: $authLine" }
    if($authReply.reason -eq 'token_locked') { throw $recovery }
    $auth = Assert-CwPassJson -Line $authLine -Operation "auth"
    $authReason = Get-CwRequiredString -Object $auth -Field 'reason' -Where 'auth reply'
    $tokenProvisioned = Get-CwRequiredBool -Object $hello -Field 'token_provisioned' -Where 'hello reply'
    $expectedAuthReason = $(if($tokenProvisioned) { 'auth_ok' } else { 'no_token_provisioned' })
    if($authReason -ne $expectedAuthReason) {
        throw "auth reply reason='$authReason', expected '$expectedAuthReason' for token_provisioned=$tokenProvisioned"
    }
    $protocol = $helloProtocol
    return [pscustomobject]@{
        Hello = $hello
        HelloLine = $helloLine
        Auth = $auth
        AuthLine = $authLine
        Protocol = $protocol
        TokenProvisioned = $tokenProvisioned
        Capabilities = $caps
    }
}

<#
    Reads one allow-listed file through the EXISTING CWFETCH1 verb, on the
    connection already open. Nothing is widened: sd:/config.json has been in
    CW_STAGE_MANIFEST_PATHS since 2026-09-17 and `fetch` is advertised. The
    body follows the status line as raw bytes, so it is read by length.
#>
function Get-CwStageFetchBytes {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [string]$RemotePath,
        [int]$MaxBytes = 65536
    )
    Send-CwLine -Stream $Stream -Line ("CWFETCH1 " + $RemotePath)
    $line = Read-CwLine -Stream $Stream
    $reply = Assert-CwPassJson -Line $line -Operation "fetch"
    # C32 (inventory :1543-1564): a reply with no 'bytes' read as ZERO bytes,
    # i.e. an EMPTY FILE -- and an empty sd:/config.json is the input the
    # root bind reads as "the game uses its default 'saves'". The advertised
    # sha256 was never checked at all, so a body that did not match what the
    # server hashed was indistinguishable from one that did.
    $n = Get-CwRequiredInt -Object $reply -Field 'bytes' -Where "fetch of $RemotePath"
    $wantSha = Get-CwRequiredString -Object $reply -Field 'sha256' -Where "fetch of $RemotePath"
    if($wantSha -notmatch '^[0-9a-fA-F]{64}$') {
        throw "fetch of $RemotePath advertised a malformed sha256 '$wantSha'"
    }
    if($n -lt 0 -or $n -gt $MaxBytes) {
        throw "fetch of $RemotePath declared $n bytes, outside the 0..$MaxBytes budget"
    }
    $buf = New-Object byte[] $n
    $got = 0
    while($got -lt $n) {
        $r = $Stream.Read($buf, $got, $n - $got)
        if($r -le 0) { throw "stage server closed after $got of $n bytes of $RemotePath" }
        $got += $r
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $gotSha = ($sha.ComputeHash($buf) | ForEach-Object { $_.ToString('x2') }) -join '' }
    finally { $sha.Dispose() }
    if(-not [string]::Equals($gotSha, $wantSha, [StringComparison]::OrdinalIgnoreCase)) {
        throw "fetch of $RemotePath declared sha256 $wantSha but the $n bytes received hash to $gotSha"
    }
    return ,$buf
}

function Get-CwSavesPage {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [string]$Root,
        [int]$Offset,
        [int]$Limit,
        [string]$Mode = "plain"
    )
    Send-CwLine -Stream $Stream -Line ("CWSAVES1 {0} {1} {2} {3}" -f $Root, $Offset, $Limit, $Mode)
    $line = Read-CwLine -Stream $Stream
    $page = Assert-CwPassJson -Line $line -Operation "saves_list"
    # C32 (inventory :1567-1582): the trailer was consulted only when it was
    # PRESENT and only for the ONE value 'list_error'. A reply that lost its
    # trailer -- the field the server writes AFTER the walk, i.e. the only
    # place a mid-walk failure can still be reported -- was accepted as a
    # complete page. Absent or unexpected is now a halt of its own.
    $trailer = Get-CwRequiredString -Object $page -Field 'trailer_status' -Where 'saves_list page'
    if($trailer -ne 'PASS') {
        throw "saves_list trailer_status='$trailer' root=$Root count=$($page.count) errno=$($page.errno)"
    }
    # C32 (inventory :1567-1602,195-269): the page was never checked against
    # the REQUEST. A server (or a desynchronised stream) answering with some
    # other root, some other window or some other mode was consumed as the
    # answer to this question -- and 'archive/<batch>' vs 'saves' is exactly
    # the distinction every MOVE decision rests on.
    $echoRoot = Get-CwRequiredString -Object $page -Field 'root' -Where 'saves_list page'
    if($echoRoot -ne $Root) {
        throw "saves_list answered for root '$echoRoot' but '$Root' was requested"
    }
    $echoMode = Get-CwRequiredString -Object $page -Field 'mode' -Where 'saves_list page'
    if($echoMode -ne $Mode) {
        throw "saves_list answered in mode '$echoMode' but '$Mode' was requested"
    }
    $echoOffset = Get-CwRequiredInt -Object $page -Field 'offset' -Where 'saves_list page'
    if($echoOffset -ne $Offset) {
        throw "saves_list answered at offset $echoOffset but $Offset was requested"
    }
    $echoLimit = Get-CwRequiredInt -Object $page -Field 'limit' -Where 'saves_list page'
    if($echoLimit -ne $Limit) {
        throw "saves_list answered with limit $echoLimit but $Limit was requested"
    }
    return $page
}

function Get-CwSavesInventory {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [string]$Root,
        [int]$PageSize = 128,
        [string]$Mode = "plain"
    )
    $pages = New-Object System.Collections.Generic.List[object]
    $offset = 0
    while($true) {
        $page = Get-CwSavesPage -Stream $Stream -Root $Root -Offset $offset -Limit $PageSize -Mode $Mode
        $pages.Add($page)
        if([int]$page.next -lt 0) { break }
        $offset = [int]$page.next
        if($pages.Count -gt 4096) { throw "paged listing did not terminate" }
    }
    $entries = Assert-CwInventoryConsistent -Pages $pages.ToArray()
    return [pscustomobject]@{ Pages = $pages.ToArray(); Entries = @($entries) }
}

<#
    Reads ONE world's level.dat through CWSAVES1's `meta` mode, on the
    connection already open. Never a second connection and never a path: the
    server accepts exactly one client at a time and reaps an idle one only
    after 20 s, and CWFETCH1's header truncates at the first space so no
    "New World <n>" could be addressed through it at all (C9 review, H1/H2).

    Returns the raw bytes, or $null when the server has no metadata for that
    world. Bounded on both sides: the server caps and flags `truncated`, and
    the caller's gunzip + NBT walk are bounded again (design S3).
#>
function Get-CwSavesMeta {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [string]$Root,
        [string]$NameHex
    )
    Send-CwLine -Stream $Stream -Line ("CWSAVES1 {0} 0 1 meta {1}" -f $Root, $NameHex)
    $line = Read-CwLine -Stream $Stream
    $reply = $null
    try { $reply = $line | ConvertFrom-Json } catch {
        throw "saves_meta returned invalid JSON: $line"
    }
    $metaStatus = Get-CwRequiredString -Object $reply -Field 'status' -Where 'saves_meta reply'
    if($metaStatus -eq 'FAIL') {
        $null = Get-CwRequiredString -Object $reply -Field 'reason' -Where 'saves_meta FAIL reply'
        return $null
    }
    if($metaStatus -ne 'PASS') { throw "saves_meta reply has unexpected status '$metaStatus'" }
    foreach($pair in @(@('reason','saves_meta'),@('root',$Root),@('name_hex',$NameHex))) {
        $actual = Get-CwRequiredString -Object $reply -Field $pair[0] -Where 'saves_meta reply'
        if($actual -ne $pair[1]) { throw "saves_meta reply field '$($pair[0])'='$actual', expected '$($pair[1])'" }
    }
    $truncated = Get-CwRequiredBool -Object $reply -Field 'truncated' -Where 'saves_meta reply'
    $readError = Get-CwRequiredBool -Object $reply -Field 'read_error' -Where 'saves_meta reply'
    $bytes = Get-CwRequiredInt -Object $reply -Field 'bytes' -Where 'saves_meta reply'
    $dataHex = Get-CwRequiredString -Object $reply -Field 'data_hex' -Where 'saves_meta reply'
    if($dataHex -notmatch '^(?:[0-9a-fA-F]{2})*$') { throw "saves_meta reply carries malformed data_hex" }
    if(($dataHex.Length / 2) -ne $bytes) {
        throw "saves_meta reply declared $bytes bytes but data_hex carries $($dataHex.Length / 2)"
    }
    if($truncated) {
        # Over the server's cap: report it undecodable rather than decode a
        # prefix and pretend it is the whole file.
        return $null
    }
    # N5: the status line is already on the wire when a read fails, so the
    # server flags it in the TRAILER and the client is what refuses the
    # content. Decoding a short read would hand back a name derived from
    # half a file.
    if($readError) {
        return $null
    }
    if([string]::IsNullOrEmpty($dataHex)) { return $null }
    return (ConvertFrom-CwHex -Hex $dataHex)
}

<#
    One row's decoded level.dat, cached by name_hex so the two metadata
    passes of the first bind (signal (c) and -ExpectWorldName) never read a
    row twice. Returns $null for "this row is not a decodable world" -- a
    fact the caller weighs, never a crash.
#>
function Get-CwRowLevelInfo {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [string]$Root,
        $Entry,
        [hashtable]$Cache = $null
    )
    $key = [string]$Entry.name_hex
    if($null -ne $Cache -and $Cache.ContainsKey($key)) { return $Cache[$key] }
    $info = $null
    # Transport/protocol failures are campaign failures.  Only bounded
    # gzip/NBT decoding errors mean "this one row is not decodable".
    $raw = Get-CwSavesMeta -Stream $Stream -Root $Root -NameHex $key
    try {
        if($null -ne $raw) { $info = Get-CwLevelInfo -Nbt (Expand-CwGzipBounded -Bytes $raw) }
    } catch {
        $info = $null
    }
    if($null -ne $Cache) { $Cache[$key] = $info }
    return $info
}

function Get-CwSavesDisplayName {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [string]$Root,
        [string]$NameHex
    )
    try {
        $raw = Get-CwSavesMeta -Stream $Stream -Root $Root -NameHex $NameHex
        if($null -eq $raw) { return $null }
        return (Get-CwLevelName -Nbt (Expand-CwGzipBounded -Bytes $raw))
    } catch {
        return $null   # "name undecodable" for this one world, never a crash
    }
}

<#
    Does `root` currently hold a directory entry with this name_hex?

    TRI-STATE on purpose: 'present', 'absent' or 'unknown'. A listing can
    fail -- a torn page throws, a dead connection throws -- and folding that
    into "absent" wrote a guess into the manifest that the reconcile truth
    table then consumed as fact (C9 review, M4).
#>
function Get-CwNamePresence {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [string]$Root,
        [string]$NameHex,
        [int]$PageSize = 128
    )
    $inv = $null
    try {
        $inv = Get-CwSavesInventory -Stream $Stream -Root $Root -PageSize $PageSize
    } catch {
        # A root that is genuinely NOT THERE is a definite answer: an archive
        # batch directory that was never created cannot hold the name.
        # C32 (inventory :1695-1714): a SUBSTRING match on the whole
        # exception text. Any failure whose message merely CONTAINED the
        # word -- an echoed path, a world name, a nested reason -- was
        # classified as a definite "the name is not there", which is the
        # observation the reconcile truth table then consumes as fact. Only
        # the reply's own reason field may say it.
        if($_.Exception.Message -match '"reason"\s*:\s*"root_missing"') { return 'absent' }
        return 'unknown'
    }
    foreach($e in @($inv.Entries)) {
        if(Test-CwNameEqual ([string]$e.name_hex) $NameHex) { return 'present' }
    }
    return 'absent'
}

function Invoke-CwSavesMove {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [ValidateSet('archive','restore')][string]$Direction,
        [string]$Root,
        [string]$Batch,
        [string]$NameHex,
        [switch]$InjectLostReply
    )
    Send-CwLine -Stream $Stream -Line ("CWARCHIVE1 {0} {1} {2} {3}" -f $Direction, $Root, $Batch, $NameHex)
    $line = Read-CwLine -Stream $Stream
    if($InjectLostReply -and ($line | ConvertFrom-Json).status -eq 'PASS') {
        throw "CW_SAVES_INJECT_LOST_REPLY: dropped successful $Direction reply after server rename, before durable result"
    }
    return @{ Line = $line; Json = ($line | ConvertFrom-Json) }
}

function Test-CwLostReplyInjection {
    param([string]$Setting, [string]$NameHex, [int]$Leg)
    return (-not [string]::IsNullOrEmpty($Setting)) -and
           [string]::Equals($Setting, ("$NameHex|$Leg"), [StringComparison]::OrdinalIgnoreCase)
}

# A row is CONFIRMED a directory only when a stat said so. This is the set
# the game itself will draw rows for (screen_select_world.c stats every
# entry and keeps the ones S_ISDIR accepts), so it is what np_rows and the
# plan's directory count must use.
function Test-CwEntryIsDir {
    param($Entry)
    if(-not ($Entry.PSObject.Properties.Name -contains 'is_dir')) { return $false }
    return ($Entry.is_dir -eq $true)
}

<#
    T9: a row whose stat FAILED might still be a world directory -- the
    server emits is_dir:null there because it genuinely does not know.

    That distinction is what makes the N6 guards REACHABLE. Matching -Keep
    and -Name only against confirmed directories meant an unreadable row was
    reported as "not in the fresh listing" / "matched 0 directories" instead
    of the documented "the server could not stat this row" refusal: the
    behaviour was right and the diagnosis was wrong, and the message the
    runbook documents could never appear. The guards therefore match against
    rows that ARE or MIGHT BE directories, and then refuse the unreadable
    ones by name.
#>
function Test-CwEntryMaybeDir {
    param($Entry)
    if(Test-CwEntryIsDir -Entry $Entry) { return $true }
    if(-not ($Entry.PSObject.Properties.Name -contains 'stat_ok')) { return $false }
    return ($Entry.stat_ok -ne $true)
}

function Get-CwEntryDisplay {
    param($Entry)
    if($Entry.PSObject.Properties.Name -contains 'name' -and -not [string]::IsNullOrEmpty($Entry.name)) {
        return [string]$Entry.name
    }
    return "(non-ascii name, hex " + [string]$Entry.name_hex + ")"
}

# ---------------------------------------------------------------------
# Self-test for every PURE helper above (ctest: test_stage_saves_tools_selftest)
# ---------------------------------------------------------------------

function script:_CwAssert {
    param([bool]$Condition, [string]$Message)
    if(-not $Condition) { throw "SELFTEST FAILED: $Message" }
}

function script:_CwAssertThrows {
    param([scriptblock]$Action, [string]$Match, [string]$Message)
    $threw = $false
    $text = ""
    try { & $Action } catch { $threw = $true; $text = $_.Exception.Message }
    if(-not $threw) { throw "SELFTEST FAILED: $Message (nothing was thrown)" }
    if(-not [string]::IsNullOrEmpty($Match) -and $text -notmatch $Match) {
        throw "SELFTEST FAILED: $Message (threw '$text', expected to match '$Match')"
    }
}

function script:_CwPage {
    param([int]$Offset, [string[]]$NameHex, [int]$Total, [int]$Next, [string]$Prefix = "", [string]$All = "")
    $entries = @($NameHex | ForEach-Object { [pscustomobject]@{ name_hex = $_; is_dir = $true; supported = $true; stat_ok = $true } })
    # [string]$Prefix = $null would arrive as "", so test emptiness, not $null.
    if([string]::IsNullOrEmpty($Prefix)) { $Prefix = Get-CwPrefixFnv1a -NameHex @($NameHex) }
    if([string]::IsNullOrEmpty($All)) { $All = $Prefix }
    return [pscustomobject]@{
        offset = $Offset; count = @($entries).Count; next = $Next
        total = $Total; prefix_fnv1a = $Prefix; all_fnv1a = $All
        entries = $entries; root_abs = 'sd:/saves'
    }
}

# Builds a minimal but REAL gzip'd level.dat: TAG_Compound "" { TAG_Compound
# "Data" { TAG_String "LevelName" = <name>, TAG_Long "LastPlayed" } }.
function script:_CwMakeLevelDat {
    param([string]$LevelName)
    $ms = [System.IO.MemoryStream]::new()
    $w = [System.IO.BinaryWriter]::new($ms)
    function _wstr([string]$s) {
        $b = [System.Text.Encoding]::UTF8.GetBytes($s)
        $w.Write([byte](($b.Length -shr 8) -band 0xff))
        $w.Write([byte]($b.Length -band 0xff))
        if($b.Length -gt 0) { $w.Write($b) }
    }
    $w.Write([byte]10); _wstr ""              # root compound, unnamed
    $w.Write([byte]10); _wstr "Data"          # Data compound
    $w.Write([byte]8);  _wstr "LevelName"; _wstr $LevelName
    $w.Write([byte]4);  _wstr "LastPlayed"
    foreach($i in 1..8) { $w.Write([byte]0) } # long payload
    $w.Write([byte]0)                         # end Data
    $w.Write([byte]0)                         # end root
    $w.Flush()
    $raw = $ms.ToArray()
    $out = [System.IO.MemoryStream]::new()
    $gz = [System.IO.Compression.GZipStream]::new($out, [System.IO.Compression.CompressionMode]::Compress, $true)
    $gz.Write($raw, 0, $raw.Length)
    $gz.Dispose()
    return ,$out.ToArray()
}

function Invoke-CwSavesSelfTest {
    # --- hex transport -------------------------------------------------
    $hex = ConvertTo-CwHex -Bytes ([System.Text.Encoding]::ASCII.GetBytes("New World 152"))
    _CwAssert ($hex -eq "4e657720576f726c6420313532") "hex encode of a name with a space"
    $back = ConvertFrom-CwHex -Hex $hex
    _CwAssert ([System.Text.Encoding]::ASCII.GetString($back) -eq "New World 152") "hex round trip"
    $nonAscii = ConvertFrom-CwHex -Hex "e4b8962041"
    _CwAssert ($nonAscii.Length -eq 5 -and $nonAscii[0] -eq 0xe4) "non-ascii hex decode is byte-exact"
    _CwAssertThrows { ConvertFrom-CwHex -Hex "abc" } "odd-length" "odd-length hex must be refused"
    _CwAssertThrows { ConvertFrom-CwHex -Hex "zz" } "non-hex" "non-hex must be refused"

    # --- FNV-1a: pinned against the PUBLISHED 32-bit vectors, which is
    # what makes this agree with cw_saves_fnv1a() in stage_saves.h without
    # either side merely reproducing the other's bug.
    _CwAssert ((Get-CwFnv1aOfString -Text "") -eq "811c9dc5") "FNV-1a of the empty string"
    _CwAssert ($script:CwSavesExit.OrphanGuard -eq 10) "orphan guard has a distinct exit code"
    _CwAssert (Test-CwLostReplyInjection -Setting '5731|1' -NameHex '5731' -Leg 1) "lost-reply injection matches one world and leg"
    _CwAssert (-not (Test-CwLostReplyInjection -Setting '5731|1' -NameHex '5732' -Leg 1)) "lost-reply injection excludes another world"
    _CwAssert (-not (Test-CwLostReplyInjection -Setting '5731|1' -NameHex '5731' -Leg 2)) "lost-reply injection excludes another leg"
    _CwAssert (-not (Test-CwLostReplyInjection -Setting '' -NameHex '5731' -Leg 1)) "unset injection does not alter normal operation"
    _CwAssert ((Get-CwFnv1aOfString -Text "a") -eq "e40c292c") "FNV-1a of 'a'"
    _CwAssert ((Get-CwFnv1aOfString -Text "foobar") -eq "bf9cf968") "FNV-1a of 'foobar'"
    _CwAssert ((Get-CwPrefixFnv1a -NameHex @("61")) -eq (Get-CwFnv1aOfString -Text "61`n")) "prefix hash appends the separator"
    # The separator is what stops two different name splits colliding.
    _CwAssert ((Get-CwPrefixFnv1a -NameHex @("ab","cd")) -ne (Get-CwPrefixFnv1a -NameHex @("abc","d"))) "separator disambiguates hex splits"

    # --- batch ids -----------------------------------------------------
    _CwAssert (Test-CwBatchId "20260921-151200") "a generated batch id is valid"
    _CwAssert (-not (Test-CwBatchId "")) "empty batch id"
    _CwAssert (-not (Test-CwBatchId "..")) "dotdot batch id"
    _CwAssert (-not (Test-CwBatchId "trailing.")) "trailing-dot batch id (FAT drops it)"
    _CwAssert (-not (Test-CwBatchId "trailing ")) "trailing-space batch id (FAT drops it)"
    _CwAssert (-not (Test-CwBatchId "a/b")) "separator in a batch id"
    _CwAssert (-not (Test-CwBatchId ("x" * 33))) "over-long batch id"

    # --- paged inventory consistency -----------------------------------
    # Every page of one walk reports the SAME all_fnv1a (over the whole
    # directory); only prefix_fnv1a grows with what has been delivered.
    $all3 = Get-CwPrefixFnv1a -NameHex @("61","62","63")
    $p0 = _CwPage -Offset 0 -NameHex @("61","62") -Total 3 -Next 2 -All $all3
    $p1 = _CwPage -Offset 2 -NameHex @("61","62","63") -Total 3 -Next -1 -All $all3
    # page 1's prefix must cover [0, offset+count) = all three
    $p1.entries = @([pscustomobject]@{ name_hex = "63"; is_dir = $true; supported = $true; stat_ok = $true })
    $p1.count = 1
    $ok = Assert-CwInventoryConsistent -Pages @($p0, $p1)
    _CwAssert (@($ok).Count -eq 3) "a consistent two-page inventory flattens to 3 entries"

    $torn = _CwPage -Offset 2 -NameHex @("61","62","63") -Total 4 -Next -1 -All $all3
    $torn.entries = @([pscustomobject]@{ name_hex = "63"; is_dir = $true; supported = $true; stat_ok = $true })
    $torn.count = 1
    _CwAssertThrows { Assert-CwInventoryConsistent -Pages @($p0, $torn) } "total changed between pages" "a moved total must reject the inventory"

    $badHash = _CwPage -Offset 2 -NameHex @("61","62","6f") -Total 3 -Next -1 -All $all3
    $badHash.entries = @([pscustomobject]@{ name_hex = "63"; is_dir = $true; supported = $true; stat_ok = $true })
    $badHash.count = 1
    _CwAssertThrows { Assert-CwInventoryConsistent -Pages @($p0, $badHash) } "prefix_fnv1a mismatch" "a moved prefix hash must reject the inventory"

    $gap = _CwPage -Offset 4 -NameHex @("61","62","63") -Total 3 -Next -1 -All $all3
    _CwAssertThrows { Assert-CwInventoryConsistent -Pages @($p0, $gap) } "not the expected" "non-contiguous pages must be rejected"

    $unfinished = _CwPage -Offset 0 -NameHex @("61","62") -Total 3 -Next 2 -All $all3
    _CwAssertThrows { Assert-CwInventoryConsistent -Pages @($unfinished) } "incomplete" "an inventory that never reached next=-1 must be rejected"
    _CwAssertThrows { Get-CwRequiredInt -Object ([pscustomobject]@{ n = 1.5 }) -Field 'n' -Where 'integer fixture' } "fractional" "a fractional JSON number must not be rounded into an integer"
    _CwAssertThrows { Get-CwRequiredInt -Object ([pscustomobject]@{ n = 2147483648L }) -Field 'n' -Where 'integer fixture' } "Int32 range" "an overflowing JSON number must not wrap into an integer"

    # --- manifest ------------------------------------------------------
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ("cw_saves_selftest_" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    try {
        $mpath = Join-Path $root "manifest_test.jsonl"
        $w = New-CwManifestWriter -Path $mpath
        try {
            Write-CwManifestLine -Writer $w -Record @{ v = 1; op = 'batch_begin'; batch = 'b1'; root = 'saves' }
            Write-CwManifestLine -Writer $w -Record @{ v = 1; op = 'intent'; dir = 'archive'; seq = 1; name_hex = '61' }
            Write-CwManifestLine -Writer $w -Record @{ v = 1; op = 'result'; seq = 1; status = 'PASS' }
            Write-CwManifestLine -Writer $w -Record @{ v = 1; op = 'intent'; dir = 'archive'; seq = 2; name_hex = '62' }
        } finally { Close-CwManifestWriter -Writer $w }

        # An interrupted write leaves a PARTIAL trailing line; the reader
        # ignores exactly that and still reconciles.
        Add-Content -LiteralPath $mpath -Encoding UTF8 -NoNewline -Value '{"v":1,"op":"inte'
        $recs = Read-CwManifest -Path $mpath
        _CwAssert (@($recs).Count -eq 4) "a trailing partial manifest line is ignored ($(@($recs).Count) records)"
        $open = Get-CwUnfinishedIntents -Records @($recs)
        _CwAssert (@($open).Count -eq 1 -and $open[0].seq -eq 2) "exactly the un-resulted intent is reported open"

        # Re-opening the crash-truncated manifest must DROP the partial
        # record rather than append onto it, or the next record is glued to
        # it and both are lost.
        $w2 = New-CwManifestWriter -Path $mpath
        try {
            Write-CwManifestLine -Writer $w2 -Record @{ v = 1; op = 'result'; seq = 2; status = 'PASS'; reason = 'reconciled_moved' }
        } finally { Close-CwManifestWriter -Writer $w2 }
        $recs = Read-CwManifest -Path $mpath
        _CwAssert (@($recs).Count -eq 5) "the partial record was dropped, not glued to ($(@($recs).Count) records)"
        $open2 = Get-CwUnfinishedIntents -Records @($recs)
        _CwAssertThrows { Read-CwManifest -Path (Join-Path $root 'vanished.jsonl') } "disappeared" "a listed manifest that vanishes must not become an empty record"

        $badDir = Join-Path $root 'manifest_bad_dir.jsonl'
        Set-Content -LiteralPath $badDir -Encoding UTF8 -Value '{"v":1,"op":"intent","seq":1,"dir":"sideways","name_hex":"61"}'
        _CwAssertThrows { Read-CwManifest -Path $badDir } "invalid intent direction" "an unknown direction must not be folded into restore"
        $badStatus = Join-Path $root 'manifest_bad_status.jsonl'
        Set-Content -LiteralPath $badStatus -Encoding UTF8 -Value '{"v":1,"op":"result","seq":1,"status":"MAYBE"}'
        _CwAssertThrows { Read-CwManifest -Path $badStatus } "invalid result status" "an unknown result status must not be consumed"

        $bindingDir = Join-Path $root 'binding_bad'
        New-Item -ItemType Directory -Force -Path $bindingDir | Out-Null
        Set-Content -LiteralPath (Join-Path $bindingDir 'root_binding.json') -Encoding UTF8 -Value '{"root":"saves","root_abs":"sd:/saves","bound_utc":"","keep_hex":[]}'
        _CwAssertThrows { Read-CwRootBinding -ManifestDir $bindingDir } "blank required field 'bound_utc'" "a blank card identity must not become an unbound campaign"
        _CwAssert (@($open2).Count -eq 0) "the appended result closed the open intent"
        # The calling convention itself: assigning first is REQUIRED. Used
        # inline, @(Func ...) wraps the returned array instead of flattening
        # it, which is how the restore tool's reconcile pass ended up handing
        # Where-Object an array and failing on a missing 'dir' property.
        _CwAssert (@(Get-CwUnfinishedIntents -Records @($recs)).Count -eq 1) "the inline @(Func) shape still wraps -- assign first (documented above Get-CwUnfinishedIntents)" # SHAPE-GATE-EXEMPT: this line IS the demonstration

        # A malformed line that is NOT the last one is a corrupt manifest.
        $bad = Join-Path $root "manifest_bad.jsonl"
        Set-Content -LiteralPath $bad -Encoding UTF8 -Value @('{"v":1,"op":"intent","dir":"archive","seq":1,"name_hex":"61"}', 'NOT JSON', '{"v":1,"op":"result","seq":1}')
        _CwAssertThrows { Read-CwManifest -Path $bad } "malformed line" "a mid-file malformed line must throw"

        # --- rig lock --------------------------------------------------
        $lock = Join-Path $root "lock"
        $held = Enter-CwRigLock -LockPath $lock -Owner "selftest"
        try {
            _CwAssert ($held.Acquired) "the lock was acquired"
            _CwAssertThrows { Enter-CwRigLock -LockPath $lock -Owner "other" } "TAKEN" "a held lock must refuse a second acquisition"
            $nested = Enter-CwRigLock -LockPath $lock -Owner "x" -LockHeldBy "selftest"
            _CwAssert (-not $nested.Acquired) "-LockHeldBy reuses the lock instead of nesting"
            _CwAssertThrows { Enter-CwRigLock -LockPath $lock -Owner "x" -LockHeldBy "somebody-else" } "held by someone else" "-LockHeldBy must be checked against owner.txt"
            Exit-CwRigLock -Lock $nested
            _CwAssert (Test-Path -LiteralPath $lock) "releasing a lock you did not take is a no-op"
        } finally { Exit-CwRigLock -Lock $held }
        _CwAssert (-not (Test-Path -LiteralPath $lock)) "the acquired lock was released"
        _CwAssertThrows { Enter-CwRigLock -LockPath $lock -Owner "x" -LockHeldBy "selftest" } "does not exist" "-LockHeldBy on a missing lock is a lie and must throw"
    } finally {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }


    # --- bounded level.dat decode --------------------------------------
    $lvl = _CwMakeLevelDat -LevelName "New World"
    $nbt = Expand-CwGzipBounded -Bytes $lvl
    _CwAssert ((Get-CwLevelName -Nbt $nbt) -eq "New World") "LevelName decodes out of a real gzip NBT level.dat"
    $lvl2 = _CwMakeLevelDat -LevelName "StormWorld"
    _CwAssert ((Get-CwLevelName -Nbt (Expand-CwGzipBounded -Bytes $lvl2)) -eq "StormWorld") "a renamed world's display name decodes"
    _CwAssertThrows { Expand-CwGzipBounded -Bytes ([byte[]](1,2,3,4)) } "not gzip" "a non-gzip level.dat is refused, not parsed"
    # Truncated NBT returns $null ("name undecodable"), never a crash.
    $trunc = $nbt[0..([Math]::Min(6, $nbt.Length - 1))]
    _CwAssert ($null -eq (Get-CwLevelName -Nbt $trunc)) "a truncated NBT decodes to null rather than throwing"

    # --- run-created-world identification (library only) ---------------
    $before = @("New World 1", "Keeper")
    $after = @(
        [pscustomobject]@{ name = "New World 1"; mtime = 100; level_name = "New World"; total_bytes = 50000 },
        [pscustomobject]@{ name = "Keeper";      mtime = 100; level_name = "Keeper";    total_bytes = 900000 },
        [pscustomobject]@{ name = "New World 2"; mtime = 150; level_name = "New World"; total_bytes = 50000 }
    )
    $sel = Select-CwRunCreatedWorlds -BeforeNames $before -AfterEntries $after -RunStartMtime 140 -RunEndMtime 160 -RunPassed $true
    _CwAssert (@($sel).Count -eq 1) "only the NEW world is a candidate"
    _CwAssert ($sel[0].Name -eq "New World 2" -and $sel[0].Action -eq 'ARCHIVE') "every signal agreeing -> ARCHIVE"

    $sel = Select-CwRunCreatedWorlds -BeforeNames $before -AfterEntries $after -RunStartMtime 140 -RunEndMtime 160 -RunPassed $false
    _CwAssert ($sel[0].Action -eq 'RETAIN' -and $sel[0].Reason -match 'did not PASS') "a failed run's world is preserved"

    $sel = Select-CwRunCreatedWorlds -BeforeNames $before -AfterEntries $after -RunStartMtime 200 -RunEndMtime 300 -RunPassed $true
    _CwAssert ($sel[0].Action -eq 'RETAIN' -and $sel[0].Reason -match 'outside the run window') "an mtime outside the window -> RETAIN"

    $renamed = @(
        [pscustomobject]@{ name = "New World 1"; mtime = 100; level_name = "New World"; total_bytes = 50000 },
        [pscustomobject]@{ name = "Keeper";      mtime = 100; level_name = "Keeper";    total_bytes = 900000 },
        [pscustomobject]@{ name = "New World 2"; mtime = 150; level_name = "My Base";   total_bytes = 50000 }
    )
    $sel = Select-CwRunCreatedWorlds -BeforeNames $before -AfterEntries $renamed -RunStartMtime 140 -RunEndMtime 160 -RunPassed $true
    _CwAssert ($sel[0].Action -eq 'RETAIN' -and $sel[0].Reason -match "not the '\+new' default") "a world someone renamed in-game -> RETAIN"

    $sel = Select-CwRunCreatedWorlds -BeforeNames $before -AfterEntries $after -RunStartMtime 140 -RunEndMtime 160 -RunPassed $true -Protected @("new world 2")
    _CwAssert ($sel[0].Action -eq 'RETAIN' -and $sel[0].Reason -match 'protected') "the protected set is subtracted first, case-insensitively"

    $sel = Select-CwRunCreatedWorlds -BeforeNames $before -AfterEntries $after -RunStartMtime 140 -RunEndMtime 160 -RunPassed $true -PairInFlight $true
    _CwAssert ($sel[0].Action -eq 'RETAIN' -and $sel[0].Reason -match 'create/verify') "no cleanup between a create/verify pair"

    # A world that vanished between the brackets invalidates the whole diff.
    $shrunk = @(
        [pscustomobject]@{ name = "New World 1"; mtime = 100; level_name = "New World"; total_bytes = 50000 },
        [pscustomobject]@{ name = "New World 2"; mtime = 150; level_name = "New World"; total_bytes = 50000 }
    )
    $sel = Select-CwRunCreatedWorlds -BeforeNames $before -AfterEntries $shrunk -RunStartMtime 140 -RunEndMtime 160 -RunPassed $true
    _CwAssert ($sel[0].Action -eq 'RETAIN' -and $sel[0].Reason -match 'gone after it') "a vanished world makes the whole diff untrustworthy"

    $sel = Select-CwRunCreatedWorlds -BeforeNames $before -AfterEntries $after -RunPassed $true
    _CwAssert ($sel[0].Action -eq 'RETAIN' -and $sel[0].Reason -match 'no run window') "a missing run window is ambiguity, not a pass"

    # The create/verify pairs a later wiring brief must protect.
    _CwAssert (@($script:CwSavesCreateVerifyPairs).Count -ge 1) "the create/verify pair table is populated"
    _CwAssert ($script:CwSavesCreateVerifyPairs[0].Verify -eq 'world_reload_verify') "world_reload_verify is named as the pair's second leg"

    # --- M2: all_fnv1a catches a delete+insert past the page boundary ---
    # prefix_fnv1a covers only what has been delivered, so a slot freed and
    # reused at or after the current page boundary left every prefix matching
    # and `total` unchanged -- and the client accepted an inventory that had
    # silently lost a world. The trailer now also carries all_fnv1a over
    # EVERY entry of the same walk.
    $allA = Get-CwPrefixFnv1a -NameHex @("61","62","63","64")
    $allB = Get-CwPrefixFnv1a -NameHex @("61","62","63","65")
    $q0 = _CwPage -Offset 0 -NameHex @("61","62") -Total 4 -Next 2 -All $allA
    $q1 = _CwPage -Offset 2 -NameHex @("63","64") -Total 4 -Next -1 -All $allA
    $q1.prefix_fnv1a = Get-CwPrefixFnv1a -NameHex @("61","62","63","64")
    $okq = Assert-CwInventoryConsistent -Pages @($q0, $q1)
    _CwAssert (@($okq).Count -eq 4) "a consistent inventory with all_fnv1a flattens"
    # Page 1 reports a DIFFERENT whole-directory hash: entry 4 was replaced
    # between the pages, and neither `total` nor the prefix can see it.
    $r1 = _CwPage -Offset 2 -NameHex @("63","65") -Total 4 -Next -1 -All $allB
    $r1.prefix_fnv1a = Get-CwPrefixFnv1a -NameHex @("61","62","63","65")
    _CwAssertThrows { Assert-CwInventoryConsistent -Pages @($q0, $r1) } "all_fnv1a" "a whole-directory hash change must reject the inventory"
    # A stat that FAILED is still DELIVERED, carrying that state (N6
    # superseded M1's blanket rejection: on a 646-entry card one corrupt row
    # used to disable every tool including the undo path). The callers are
    # what refuse to MOVE it or accept it as a keeper.
    $bad = _CwPage -Offset 0 -NameHex @("61") -Total 1 -Next -1
    $bad.entries[0].stat_ok = $false
    $badOut = Assert-CwInventoryConsistent -Pages @($bad)
    _CwAssert (@($badOut).Count -eq 1 -and -not $badOut[0].stat_ok) "an unstat-able entry is delivered with its state, not thrown away"

    # --- M4: presence is a TRI-state, never a silent $false ---------------
    _CwAssert ((Resolve-CwReconcileVerdict -Src 'present' -Dst 'absent') -eq 'not_moved') "src only -> not moved"
    _CwAssert ((Resolve-CwReconcileVerdict -Src 'absent' -Dst 'present') -eq 'moved') "dst only -> moved (the lost-reply case)"
    _CwAssert ((Resolve-CwReconcileVerdict -Src 'present' -Dst 'present') -eq 'stop_both_present') "both -> STOP"
    _CwAssert ((Resolve-CwReconcileVerdict -Src 'absent' -Dst 'absent') -eq 'stop_neither_present') "neither -> STOP"
    # "I could not look" must never be folded into "it is not there": that is
    # what turned a torn listing or a dead connection into a recorded
    # observation the reconcile table then consumed as fact.
    foreach($pair in @(@('unknown','absent'), @('present','unknown'), @('unknown','unknown'))) {
        _CwAssert ((Resolve-CwReconcileVerdict -Src $pair[0] -Dst $pair[1]) -eq 'stop_unknown') "an unobservable end -> STOP ($($pair[0])/$($pair[1]))"
    }

    # --- M6: -LockHeldBy is an EQUALITY test, not a substring -------------
    $lroot = Join-Path ([System.IO.Path]::GetTempPath()) ("cw_lock_" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $lroot | Out-Null
    try {
        $lock = Join-Path $lroot "lock"
        $held = Enter-CwRigLock -LockPath $lock -Owner "archive_wii_saves.ps1 / tuck / 2026-09-21T16:00:00"
        try {
            # Every one of these is a SUBSTRING of the owner line and used to
            # pass, silently "reusing" a lock held by another session.
            foreach($loose in @("a", "/", "ps1", "wii", "tuck", "2026")) {
                _CwAssertThrows { Enter-CwRigLock -LockPath $lock -Owner "x" -LockHeldBy $loose } "held by someone else" "-LockHeldBy '$loose' is only a substring and must be refused"
            }
            $exact = Enter-CwRigLock -LockPath $lock -Owner "x" -LockHeldBy "archive_wii_saves.ps1 / tuck / 2026-09-21T16:00:00"
            _CwAssert (-not $exact.Acquired) "the exact owner string reuses the lock"
        } finally { Exit-CwRigLock -Lock $held }
    } finally { Remove-Item -LiteralPath $lroot -Recurse -Force -ErrorAction SilentlyContinue }

    # --- H5: ambiguity RETAINS ------------------------------------------
    $before5 = @("New World 1", "New World 2", "Keeper")
    $mk = {
        param([string]$n, [long]$mt, [string]$ln, [long]$bytes)
        [pscustomobject]@{ name = $n; mtime = $mt; level_name = $ln; total_bytes = $bytes }
    }
    $base5 = @(
        (& $mk "New World 1" 100 "New World" 50000),
        (& $mk "New World 2" 100 "New World" 50000),
        (& $mk "Keeper"      100 "Keeper"    900000)
    )
    # The one good case: a strictly higher +new ordinal, in the window, with
    # the default display name, and tiny.
    $sel = Select-CwRunCreatedWorlds -BeforeNames $before5 -AfterEntries ($base5 + @((& $mk "New World 3" 150 "New World" 50000))) -RunStartMtime 140 -RunEndMtime 160 -RunPassed $true
    _CwAssert (@($sel).Count -eq 1 -and $sel[0].Action -eq 'ARCHIVE') "the unambiguous +new world is ARCHIVE"

    # (b) TWO worlds in the same window -- the evidence cannot belong to both.
    # Not hypothetical: the rig has launched a chain twice before.
    $sel = Select-CwRunCreatedWorlds -BeforeNames $before5 -AfterEntries ($base5 + @((& $mk "New World 3" 150 "New World" 50000), (& $mk "New World 4" 151 "New World" 50000))) -RunStartMtime 140 -RunEndMtime 160 -RunPassed $true
    _CwAssert (@($sel).Count -eq 2) "both candidates are reported"
    foreach($x in @($sel)) {
        _CwAssert ($x.Action -eq 'RETAIN') "a run that created TWO worlds must RETAIN both"
        _CwAssert ($x.Reason -match 'more than one candidate') "and say why: $($x.Reason)"
    }

    # (a) A hand-made world called exactly "New World" -- the in-game default
    # display name is also "New World", so all three original signals agreed.
    $sel = Select-CwRunCreatedWorlds -BeforeNames $before5 -AfterEntries ($base5 + @((& $mk "New World" 150 "New World" 50000))) -RunStartMtime 140 -RunEndMtime 160 -RunPassed $true
    _CwAssert ($sel[0].Action -eq 'RETAIN') "a bare 'New World' cannot be told from a hand-made world"
    _CwAssert ($sel[0].Reason -match 'auto-name') "and the reason names the auto-name rule: $($sel[0].Reason)"

    # An ordinal that is NOT above every ordinal present before the run.
    $sel = Select-CwRunCreatedWorlds -BeforeNames @("New World 1", "New World 9") -AfterEntries @((& $mk "New World 1" 100 "New World" 50000), (& $mk "New World 9" 100 "New World" 50000), (& $mk "New World 5" 150 "New World" 50000)) -RunStartMtime 140 -RunEndMtime 160 -RunPassed $true
    _CwAssert ($sel[0].Action -eq 'RETAIN') "an ordinal below one already present is not this run's +new world"

    # Too big to be a rig world, and size unknown, are both ambiguity.
    $sel = Select-CwRunCreatedWorlds -BeforeNames $before5 -AfterEntries ($base5 + @((& $mk "New World 3" 150 "New World" 9000000))) -RunStartMtime 140 -RunEndMtime 160 -RunPassed $true
    _CwAssert ($sel[0].Action -eq 'RETAIN' -and $sel[0].Reason -match 'bytes') "a world too big to be a rig leftover is RETAINed"
    $noSize = [pscustomobject]@{ name = "New World 3"; mtime = 150; level_name = "New World" }
    $sel = Select-CwRunCreatedWorlds -BeforeNames $before5 -AfterEntries ($base5 + @($noSize)) -RunStartMtime 140 -RunEndMtime 160 -RunPassed $true
    _CwAssert ($sel[0].Action -eq 'RETAIN' -and $sel[0].Reason -match 'size') "an unknown size is ambiguity, not a pass"

    # --- M7: the protected-autoload table is GATED against the configs ----
    # There is no enumerable profile source to check it against (the job
    # names are stage-side JSON, not hw_run_profile_name entries), so the
    # gate is the card configs themselves: every config that does NOT
    # autoload "+new" depends on an existing world by name or index and must
    # be listed as protected.
    $cfgDir = Join-Path $RepoRoot "tools/hw_card_package/configs"
    if(Test-Path -LiteralPath $cfgDir) {
        $unlisted = @()
        foreach($f in @(Get-ChildItem -LiteralPath $cfgDir -Filter *.json)) {
            $txt = Get-Content -Raw -LiteralPath $f.FullName
            # A config with no autoload key is handled by its own gate
            # above (N13); this one only classifies the ones that have it.
            if($txt -notmatch '"autoload"\s*:\s*"([^"]*)"') { continue }
            $al = $Matches[1]
            if($al -eq '+new') { continue }
            $job = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)
            if(-not (@($script:CwSavesAutoloadDependentJobs) | Where-Object { Test-CwNameEqual $_.Job $job })) {
                $unlisted += "$job (autoload '$al')"
            }
        }
        _CwAssert (@($unlisted).Count -eq 0) "every card config that does not autoload '+new' must be listed as autoload-dependent: $($unlisted -join ', ')"
    }

    # --- N1: the inline @(Func) trap cannot be REINTRODUCED ---------------
    # Fixing the one live call site is not enough -- the shape is easy to
    # retype and its failure is silent (the loop runs once with the whole
    # array bound to the item variable, and everything downstream quietly
    # sees nothing). So the source itself is gated: no tool may call a
    # collection-returning helper inline inside @(...) or at the head of a
    # pipeline. Assign first, then wrap.
    $collectionHelpers = @(
        'ConvertFrom-CwHex', 'Assert-CwInventoryConsistent', 'Read-CwManifest',
        'Get-CwUnfinishedIntents', 'Get-CwManifestPaths',
        'Select-CwRunCreatedWorlds', 'Get-CwIntentState',
        'Add-CwManifestOrder', 'Get-CwActiveBlocks',
        'Get-CwUncertainArchiveIntents'
    )
    $offenders = @()
    foreach($tool in @('cw_saves_common.ps1','list_wii_saves.ps1','archive_wii_saves.ps1','restore_wii_saves.ps1')) {
        $toolPath = Join-Path (Join-Path $RepoRoot 'tools') $tool
        if(-not (Test-Path -LiteralPath $toolPath)) { continue }
        $n = 0
        foreach($line in @(Get-Content -LiteralPath $toolPath)) {
            $n++
            if($line -match 'SHAPE-GATE-EXEMPT') { continue }
            # A comment-only line cannot execute, and these files describe
            # the trap in prose -- scan code, not the warning about it.
            if($line.TrimStart().StartsWith('#')) { continue }
            foreach($h in $collectionHelpers) {
                if($line -match ('@\(\s*' + [regex]::Escape($h) + '[\s\-]')) {
                    $offenders += "${tool}:${n}: inline @($h ...)"
                }
                if($line -match ([regex]::Escape($h) + '[^|)]*\|')) {
                    $offenders += "${tool}:${n}: $h piped directly"
                }
            }
        }
    }
    _CwAssert (@($offenders).Count -eq 0) "a collection-returning helper is called inline -- assign it to a variable first: $($offenders -join '; ')"

    # --- N13: a config with NO autoload key is not silently exempt --------
    $cfgDir2 = Join-Path $RepoRoot "tools/hw_card_package/configs"
    if(Test-Path -LiteralPath $cfgDir2) {
        $unclassified = @()
        foreach($f in @(Get-ChildItem -LiteralPath $cfgDir2 -Filter *.json)) {
            $txt = Get-Content -Raw -LiteralPath $f.FullName
            $job = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)
            if($txt -match '"autoload"\s*:\s*"([^"]*)"') { continue }
            # No autoload key at all: it must be EXPLICITLY exempt, so that a
            # future config which drops the key while still depending on list
            # order cannot slip the gate by omission.
            if(-not (@($script:CwSavesAutoloadExemptJobs) | Where-Object { Test-CwNameEqual $_.Job $job })) {
                $unclassified += $job
            }
        }
        _CwAssert (@($unclassified).Count -eq 0) "a card config carries NO autoload key and is not listed as exempt (omission must be deliberate): $($unclassified -join ', ')"
    }

    # --- N6: one unstat-able row must not reject the whole inventory ------
    # It is reported as its own state and refused for a MOVE, but listing,
    # the presence probes and therefore the UNDO path keep working -- a
    # corrupt directory entry that has nothing to do with the world being
    # restored must not be able to block the recovery tool.
    $n6 = _CwPage -Offset 0 -NameHex @("61","62") -Total 2 -Next -1
    $n6.entries[1].stat_ok = $false
    $n6ok = Assert-CwInventoryConsistent -Pages @($n6)
    _CwAssert (@($n6ok).Count -eq 2) "an unstat-able row is still delivered, not thrown away"
    _CwAssert (-not $n6ok[1].stat_ok) "and it keeps saying its stat failed"

    # --- N3: the first bind's signals, as pure predicates -----------------
    # Astra change 1: "Count alone is insufficient: corroborate it with
    # current game configuration and identifiable world metadata. Stop on
    # ambiguity."
    _CwAssert ((Test-CwConfigWorldsMatchesRoot -PathsWorlds 'saves' -Root 'saves')) "config paths.worlds 'saves' corroborates the saves root"
    _CwAssert (-not (Test-CwConfigWorldsMatchesRoot -PathsWorlds 'saves' -Root 'saves_app')) "the same value must NOT corroborate the app root"
    _CwAssert ((Test-CwConfigWorldsMatchesRoot -PathsWorlds 'apps/CavEX/saves' -Root 'saves_app')) "an app-relative paths.worlds corroborates saves_app"
    _CwAssert (-not (Test-CwConfigWorldsMatchesRoot -PathsWorlds 'somewhere/else' -Root 'saves')) "an unrecognised paths.worlds corroborates nothing -- ambiguity STOPs"
    _CwAssert (-not (Test-CwConfigWorldsMatchesRoot -PathsWorlds '' -Root 'saves')) "a PRESENT but empty paths.worlds is ambiguity, not a pass"

    # --- T7: an ABSENT paths.worlds is the GAME's default, not ambiguity --
    # config_read_string(&gstate.config_user, "paths.worlds", "saves") --
    # source/game/gui/screen_select_world.c:187 (also :630, :668, :710, and
    # screen_create_world.c:261). A card whose config.json simply has no
    # paths key is a card the game reads as "saves", so treating that as
    # "absent or unreadable" made the tool disagree with the game and sent
    # the operator hunting a fetch failure that never happened. A config
    # that could not be READ is a different thing and STOPs before it ever
    # reaches this predicate.
    _CwAssert (Test-CwConfigWorldsMatchesRoot -PathsWorlds '' -Root 'saves' -KeyPresent:$false) "an ABSENT paths.worlds is the game's own default 'saves'"
    _CwAssert (-not (Test-CwConfigWorldsMatchesRoot -PathsWorlds '' -Root 'saves_app' -KeyPresent:$false)) "...and that default is never the app root"

    # --- T8: the path forms a Windows-side config actually carries --------
    # '\\' inside PowerShell single quotes is TWO literal backslashes, so
    # the old .Replace only ever rewrote a DOUBLED one: 'saves\' and
    # '.\saves' both failed to corroborate a card that plainly said "saves".
    foreach($form in @('saves', 'saves/', '/saves/', 'SAVES', 'sd:/saves',
                       'sd:/saves/', 'sd:/SAVES', 'saves ', 'saves\',
                       '.\saves', './saves', '.\saves\', 'sd:\saves')) {
        _CwAssert (Test-CwConfigWorldsMatchesRoot -PathsWorlds $form -Root 'saves') "paths.worlds '$form' corroborates the saves root"
    }
    foreach($form in @('apps/CavEX/saves', 'apps\CavEX\saves',
                       './apps/CavEX/saves', 'sd:/apps/CavEX/saves')) {
        _CwAssert (Test-CwConfigWorldsMatchesRoot -PathsWorlds $form -Root 'saves_app') "paths.worlds '$form' corroborates the app root"
    }
    foreach($form in @('sd:saves', 'somewhere/else', 'saves/deeper', '..\saves')) {
        _CwAssert (-not (Test-CwConfigWorldsMatchesRoot -PathsWorlds $form -Root 'saves')) "paths.worlds '$form' corroborates nothing -- ambiguity STOPs"
    }

    # --- T9: a row whose stat FAILED claims no type -----------------------
    # The server emits is_dir:null there rather than fabricating `false`,
    # because it does not know. A confirmed directory is a directory; an
    # unreadable row MIGHT be one, which is what the archive guards have to
    # match against so they can refuse it BY NAME.
    $okDir   = [pscustomobject]@{ name_hex = '61'; is_dir = $true;  supported = $true; stat_ok = $true }
    $okFile  = [pscustomobject]@{ name_hex = '62'; is_dir = $false; supported = $true; stat_ok = $true }
    $unknown = [pscustomobject]@{ name_hex = '63'; is_dir = $null;  supported = $true; stat_ok = $false }
    _CwAssert (Test-CwEntryIsDir -Entry $okDir) "a stat'd directory is a directory"
    _CwAssert (-not (Test-CwEntryIsDir -Entry $okFile)) "a stat'd file is not"
    _CwAssert (-not (Test-CwEntryIsDir -Entry $unknown)) "an unreadable row is never CONFIRMED a directory"
    _CwAssert (Test-CwEntryMaybeDir -Entry $okDir) "a directory might be a directory"
    _CwAssert (-not (Test-CwEntryMaybeDir -Entry $okFile)) "a row the server DID stat and called a file is settled"
    _CwAssert (Test-CwEntryMaybeDir -Entry $unknown) "an unreadable row might be one, so the guards must see it"

    # LastPlayed has to come out of the NBT too, or signal (c) cannot say
    # anything about a world's identity beyond its name.
    $lvlInfo = Get-CwLevelInfo -Nbt (Expand-CwGzipBounded -Bytes (_CwMakeLevelDat -LevelName "New World"))
    _CwAssert ($lvlInfo.LevelName -eq "New World") "Get-CwLevelInfo returns the display name"
    _CwAssert ($lvlInfo.HasLastPlayed) "Get-CwLevelInfo reports whether LastPlayed was present"

    # --- N9: the re-issue allowance keys on the CARD's bytes --------------
    _CwAssert ((Get-CwBlockReissueVerdict -BlockHex '4e6577' -TargetHex '4e6577' -Reason 'not_moved') -eq 'reissue') "a not_moved world's own hex re-issues it"
    _CwAssert ((Get-CwBlockReissueVerdict -BlockHex '4e6577' -TargetHex '4e6577' -Reason 'stop_unknown') -eq 'blocked') "a stop verdict needs operator resolution, not reissue"
    _CwAssert ((Get-CwBlockReissueVerdict -BlockHex '4e6577' -TargetHex '4f6c64' -Reason 'not_moved') -eq 'blocked') "a different world stays blocked"
    _CwAssert ((Get-CwBlockReissueVerdict -BlockHex '4e6577' -TargetHex '' -Reason 'not_moved') -eq 'blocked') "an unresolved target cannot claim the allowance"

    # --- T1: the TOTAL ORDER on manifest records --------------------------
    # A block_cleared may clear only a block it FOLLOWS. Get-CwActiveBlocks
    # used to keep a set keyed on name_hex ALONE, with no seq, no utc and no
    # per-file pairing, so ONE old clear pre-cleared every future block for
    # that world -- permanently. Deciding "follows" across append-only files
    # needs an explicit total order; this pins every branch of it.
    $mkOrdered = {
        param([string]$Op, [string]$Hex, [int]$Seq, [string]$Utc, [string]$File, [int]$Ord, [int]$Line)
        $o = [pscustomobject]@{ v = 1; op = $Op; name_hex = $Hex; seq = $Seq
                                from = "sd:/saves/W$Seq"; utc = $Utc }
        Add-Member -InputObject $o -NotePropertyName 'cw_manifest' -NotePropertyValue $File -Force
        Add-Member -InputObject $o -NotePropertyName 'cw_file_ordinal' -NotePropertyValue ([int]$Ord) -Force
        Add-Member -InputObject $o -NotePropertyName 'cw_line' -NotePropertyValue ([int]$Line) -Force
        return $o
    }

    # (1) SAME FILE: the LINE decides, whatever the clocks say. The file is
    # opened Append and every line is Flush($true)ed before the next, so line
    # order IS write order and no timestamp can contradict it.
    $blkA = & $mkOrdered 'block'         '61' 1 '2026-09-21T12:00:05.0000000Z' 'm1' 0 3
    $clrA = & $mkOrdered 'block_cleared' '61' 1 '2026-09-21T11:00:00.0000000Z' 'm1' 0 9
    _CwAssert ((Compare-CwRecordOrder -A $clrA -B $blkA) -eq 1) "within one append-only file the LINE decides, not the clock"
    _CwAssert ((Compare-CwRecordOrder -A $blkA -B $clrA) -eq -1) "and the comparison is antisymmetric"
    _CwAssert ((Compare-CwRecordOrder -A $blkA -B $blkA) -eq 0) "a record equals itself"

    # (2) DIFFERENT FILES: the utc decides -- a block and its clear land in
    # different manifests by construction (the block is written where the
    # unresolved intent is, the clear where the re-issue lands).
    $blkB = & $mkOrdered 'block'         '62' 2 '2026-09-21T12:00:00.0000000Z' 'm2' 1 1
    $clrB = & $mkOrdered 'block_cleared' '62' 2 '2026-09-21T12:00:01.0000000Z' 'm1' 0 2
    _CwAssert ((Compare-CwRecordOrder -A $clrB -B $blkB) -eq 1) "across files the utc decides, even when the FILE sorts earlier"
    $clrB2 = & $mkOrdered 'block_cleared' '62' 2 '2026-09-21T11:59:59.0000000Z' 'm3' 2 1
    _CwAssert ((Compare-CwRecordOrder -A $clrB2 -B $blkB) -eq -1) "a clear written EARLIER is earlier, even from a later-sorting file"

    # (3) EQUAL utc across files: the manifest FILE ORDINAL breaks the tie,
    # deterministically (batch ids are yyyyMMdd-HHmmss, so name order is
    # write order).
    $tie1 = & $mkOrdered 'block'         '63' 3 '2026-09-21T12:00:00.0000000Z' 'm1' 0 1
    $tie2 = & $mkOrdered 'block_cleared' '63' 3 '2026-09-21T12:00:00.0000000Z' 'm2' 1 1
    _CwAssert ((Compare-CwRecordOrder -A $tie2 -B $tie1) -eq 1) "equal timestamps fall back to the manifest file ordinal"
    _CwAssert ((Compare-CwRecordOrder -A $tie1 -B $tie2) -eq -1) "and that tie-break is antisymmetric too"

    # (4) NO parseable utc across files: UNORDERED. A clear that cannot be
    # PROVEN later than a block does not clear it -- fail closed, because the
    # failure this replaces is a block that silently disarms.
    $noUtc = & $mkOrdered 'block_cleared' '64' 4 '' 'm1' 0 1
    $blkC  = & $mkOrdered 'block'         '64' 4 '2026-09-21T12:00:00.0000000Z' 'm2' 1 1
    _CwAssert ($null -eq (Compare-CwRecordOrder -A $noUtc -B $blkC)) "a record with no utc cannot be ordered against another file"
    _CwAssert (-not (Test-CwClearClearsBlock -Clear $noUtc -Block $blkC)) "and an unorderable clear clears NOTHING"

    # (5) Pairing: a clear that NAMES its block clears exactly that one.
    $blkD = & $mkOrdered 'block' '65' 7 '2026-09-21T12:00:00.0000000Z' 'm1' 0 4
    $blkE = & $mkOrdered 'block' '65' 9 '2026-09-21T13:00:00.0000000Z' 'm1' 0 6
    $clrD = & $mkOrdered 'block_cleared' '65' 0 '2026-09-21T14:00:00.0000000Z' 'm2' 1 1
    Add-Member -InputObject $clrD -NotePropertyName 'block_manifest' -NotePropertyValue 'm1' -Force
    Add-Member -InputObject $clrD -NotePropertyName 'block_seq' -NotePropertyValue 7 -Force
    _CwAssert (Test-CwClearClearsBlock -Clear $clrD -Block $blkD) "an explicitly paired clear clears the block it names"
    _CwAssert (-not (Test-CwClearClearsBlock -Clear $clrD -Block $blkE)) "...and ONLY that block, even though it is later than both"
    # A different world is never cleared by another world's record.
    $blkF = & $mkOrdered 'block' '66' 7 '2026-09-21T12:00:00.0000000Z' 'm1' 0 4
    _CwAssert (-not (Test-CwClearClearsBlock -Clear $clrD -Block $blkF)) "a clear never crosses to another name_hex"

    # (6) The T1 defect itself, at the level that produced it: an OLD clear
    # must not disarm a NEW block for the same world.
    $t1 = @(
        (& $mkOrdered 'block'         '4e5731' 1 '2026-09-21T10:00:00.0000000Z' 'm1' 0 1),
        (& $mkOrdered 'block_cleared' '4e5731' 1 '2026-09-21T10:05:00.0000000Z' 'm2' 1 1),
        (& $mkOrdered 'block'         '4e5731' 2 '2026-09-21T11:00:00.0000000Z' 'm3' 2 1)
    )
    $act = Get-CwActiveBlocks -Records @($t1)
    _CwAssert (@($act).Count -eq 1) "the SECOND block survives an earlier clear ($(@($act).Count) active)"
    _CwAssert ([int]$act[0].seq -eq 2) "and it is the later one that is active"
    # The same three records with the clear LAST leave nothing active.
    $t1b = @(
        (& $mkOrdered 'block'         '4e5731' 1 '2026-09-21T10:00:00.0000000Z' 'm1' 0 1),
        (& $mkOrdered 'block'         '4e5731' 2 '2026-09-21T11:00:00.0000000Z' 'm3' 2 1),
        (& $mkOrdered 'block_cleared' '4e5731' 0 '2026-09-21T12:00:00.0000000Z' 'm4' 3 1)
    )
    $actb = Get-CwActiveBlocks -Records @($t1b)
    _CwAssert (@($actb).Count -eq 0) "a clear that follows BOTH blocks clears both ($(@($actb).Count) active)"

    # (7) T3 backstop -- the block is derived from the RESULT when the block
    # line itself was lost. Writing the block before the result makes the
    # crash window one-sided, and this makes it zero-sided: a
    # reconciled_not_moved result with no later clear IS a block.
    $t3 = @(
        (& $mkOrdered 'intent' '4e5732' 5 '2026-09-21T10:00:00.0000000Z' 'm1' 0 1)
    )
    $t3res = & $mkOrdered 'result' '4e5732' 5 '2026-09-21T10:00:01.0000000Z' 'm1' 0 2
    Add-Member -InputObject $t3res -NotePropertyName 'reason' -NotePropertyValue 'reconciled_not_moved' -Force
    Add-Member -InputObject $t3[0] -NotePropertyName 'dir' -NotePropertyValue 'archive' -Force
    $t3 += $t3res
    $act3 = Get-CwActiveBlocks -Records @($t3)
    _CwAssert (@($act3).Count -eq 1) "a not_moved RESULT with no block line still blocks ($(@($act3).Count) active)"
    _CwAssert ([string]$act3[0].name_hex -eq '4e5732') "and it carries the intent's name_hex"
    # ...and the explicit block record does not double-count with it.
    $t3blk = & $mkOrdered 'block' '4e5732' 5 '2026-09-21T10:00:01.0000000Z' 'm1' 0 2
    $t3c = @($t3[0], $t3blk, $t3res)
    $act3c = Get-CwActiveBlocks -Records @($t3c)
    _CwAssert (@($act3c).Count -eq 1) "an explicit block and its own result are ONE block ($(@($act3c).Count))"
    # A restore intent's failure is not an archive block.
    $t3r = @((& $mkOrdered 'intent' '4e5733' 6 '2026-09-21T10:00:00.0000000Z' 'm1' 0 1))
    Add-Member -InputObject $t3r[0] -NotePropertyName 'dir' -NotePropertyValue 'restore' -Force
    $t3rres = & $mkOrdered 'result' '4e5733' 6 '2026-09-21T10:00:01.0000000Z' 'm1' 0 2
    Add-Member -InputObject $t3rres -NotePropertyName 'reason' -NotePropertyValue 'not_moved' -Force
    $t3r += $t3rres
    $act3r = Get-CwActiveBlocks -Records @($t3r)
    _CwAssert (@($act3r).Count -eq 0) "a RESTORE intent's not_moved is not an archive block"

    # (8) An undecorated record is a programming error, not a silent pass:
    # the ordering is only sound over records read through Add-CwManifestOrder.
    _CwAssertThrows { Get-CwActiveBlocks -Records @([pscustomobject]@{ v = 1; op = 'block'; name_hex = '61'; seq = 1 }) } "not ordered" "an unordered record must throw rather than be guessed at"

    # (9) C15-1: the backstop allow-list must cover EVERY halting verdict.
    # The verify path stamps the bare verdict (stop_both_present /
    # stop_neither_present / stop_unknown); the reconcile path would stamp
    # "reconciled_<verdict>". Only not_moved was in the list, so a STOP the
    # tool printed and exited 4/5/9 on was forgotten by the very next leg.
    foreach($reason in @('not_moved','reconciled_not_moved',
                         'stop_both_present','reconciled_stop_both_present',
                         'stop_neither_present','reconciled_stop_neither_present',
                         'stop_unknown','reconciled_stop_unknown')) {
        $hi = & $mkOrdered 'intent' '4e5740' 9 '2026-09-21T10:00:00.0000000Z' 'm1' 0 1
        Add-Member -InputObject $hi -NotePropertyName 'dir' -NotePropertyValue 'archive' -Force
        $hr = & $mkOrdered 'result' '4e5740' 9 '2026-09-21T10:00:01.0000000Z' 'm1' 0 2
        Add-Member -InputObject $hr -NotePropertyName 'reason' -NotePropertyValue $reason -Force
        $ha = Get-CwActiveBlocks -Records @($hi, $hr)
        _CwAssert (@($ha).Count -eq 1) "a '$reason' result halts the campaign ($(@($ha).Count) active)"
        _CwAssert ((Test-CwReasonIsHalt -Reason $reason)) "'$reason' is a halting reason"
    }
    foreach($reason in @('moved','reconciled_moved','archived','dest_exists')) {
        $ni = & $mkOrdered 'intent' '4e5742' 9 '2026-09-21T10:00:00.0000000Z' 'm1' 0 1
        Add-Member -InputObject $ni -NotePropertyName 'dir' -NotePropertyValue 'archive' -Force
        $nr = & $mkOrdered 'result' '4e5742' 9 '2026-09-21T10:00:01.0000000Z' 'm1' 0 2
        Add-Member -InputObject $nr -NotePropertyName 'reason' -NotePropertyValue $reason -Force
        $na = Get-CwActiveBlocks -Records @($ni, $nr)
        _CwAssert (@($na).Count -eq 0) "a '$reason' result must not invent a halt"
        _CwAssert (-not (Test-CwReasonIsHalt -Reason $reason)) "'$reason' is not a halting reason"
    }

    # (10) C15-2 / C15-6: an explicit pairing must identify ONE BLOCK
    # INSTANCE, not a coordinate that recurs. `-Batch` defaults to a fresh
    # id every leg and seq restarts at 1 in each manifest, so (manifest,
    # seq) is predictable rather than unique across time: a clear planted
    # with a future manifest's coordinates disarmed the block when it
    # arrived. The pairing therefore carries the block's OWN utc.
    $futureBlk = & $mkOrdered 'block' '4e5741' 1 '2026-09-21T12:00:00.0000000Z' 'm9' 5 1
    $predated  = & $mkOrdered 'block_cleared' '4e5741' 0 '2026-09-21T06:00:00.0000000Z' 'm1' 0 1
    Add-Member -InputObject $predated -NotePropertyName 'block_manifest' -NotePropertyValue 'm9' -Force
    Add-Member -InputObject $predated -NotePropertyName 'block_seq' -NotePropertyValue 1 -Force
    _CwAssert (-not (Test-CwClearClearsBlock -Clear $predated -Block $futureBlk)) "a pre-dated clear naming a block that did not exist yet clears NOTHING"
    # A legacy pairing that IS later still clears -- order-bound, not banned.
    $late = & $mkOrdered 'block_cleared' '4e5741' 0 '2026-09-21T13:00:00.0000000Z' 'm1' 0 1
    Add-Member -InputObject $late -NotePropertyName 'block_manifest' -NotePropertyValue 'm9' -Force
    Add-Member -InputObject $late -NotePropertyName 'block_seq' -NotePropertyValue 1 -Force
    _CwAssert (Test-CwClearClearsBlock -Clear $late -Block $futureBlk) "a legacy pairing that follows its block still clears it"

    # The tool's own clears carry block_utc, so they identify the instance
    # and are independent of both the clock and the file name.
    $paired = & $mkOrdered 'block_cleared' '4e5741' 0 '2026-09-21T06:00:00.0000000Z' 'm1' 0 1
    Add-Member -InputObject $paired -NotePropertyName 'block_manifest' -NotePropertyValue 'm9' -Force
    Add-Member -InputObject $paired -NotePropertyName 'block_seq' -NotePropertyValue 1 -Force
    Add-Member -InputObject $paired -NotePropertyName 'block_utc' -NotePropertyValue '2026-09-21T12:00:00.0000000Z' -Force
    _CwAssert (Test-CwClearClearsBlock -Clear $paired -Block $futureBlk) "a clear carrying the block's own utc pairs with that block INSTANCE, whatever the clocks did"
    # C15-6: ...and therefore survives its manifest being RENAMED.
    $renamedBlk = & $mkOrdered 'block' '4e5741' 1 '2026-09-21T12:00:00.0000000Z' 'm9-renamed' 7 1
    _CwAssert (Test-CwClearClearsBlock -Clear $paired -Block $renamedBlk) "a cleared block does not come BACK when its manifest is renamed"
    # C15-2's core: a LATER block at the same (manifest, seq) is a different
    # instance and is not touched.
    $otherBlk = & $mkOrdered 'block' '4e5741' 1 '2026-09-21T13:00:00.0000000Z' 'm9' 5 1
    _CwAssert (-not (Test-CwClearClearsBlock -Clear $paired -Block $otherBlk)) "a LATER block at the same (manifest, seq) is a different block and survives"

    # --- T6: a typed name is resolved against RECORDED bytes, never re-encoded
    $rb = @(
        [pscustomobject]@{ name_hex = 'c3a9646974696f6e'; name_display = '(non-ascii name, hex c3a9646974696f6e)'; from = 'sd:/saves/x'; seq = 1; manifest = 'm1' },
        [pscustomobject]@{ name_hex = '4e657720576f726c642032'; name_display = 'New World 2'; from = 'sd:/saves/New World 2'; seq = 2; manifest = 'm1' }
    )
    $r1 = Resolve-CwBlockTarget -Blocks @($rb) -Typed 'New World 2'
    _CwAssert ($r1.Status -eq 'ok' -and @($r1.Blocks).Count -eq 1) "a recorded display name resolves its block"
    $r2 = Resolve-CwBlockTarget -Blocks @($rb) -Typed 'new world 2'
    _CwAssert ($r2.Status -eq 'ok') "and it folds case, like FAT"
    # THE finding: UTF8('edition' with an accent) IS the first block's
    # name_hex. Re-encoding what the operator typed would clear it; matching
    # the RECORDED name must not.
    $r3 = Resolve-CwBlockTarget -Blocks @($rb) -Typed ([char]0xe9 + "dition")
    _CwAssert ($r3.Status -eq 'absent') "a UTF-8 re-encoding of the typed name must NOT resolve a block"
    $r4 = Resolve-CwBlockTarget -Blocks @($rb) -Hex 'c3a9646974696f6e'
    _CwAssert ($r4.Status -eq 'ok' -and @($r4.Blocks).Count -eq 1) "-ResolveBlockHex reaches a block no operator can type"
    $r5 = Resolve-CwBlockTarget -Blocks @($rb) -Hex 'zz'
    _CwAssert ($r5.Status -eq 'bad_hex') "a non-hex -ResolveBlockHex is refused as such"
    $r6 = Resolve-CwBlockTarget -Blocks @($rb) -Typed 'No Such World'
    _CwAssert ($r6.Status -eq 'absent') "an absent name refuses by name"
    $dupe = @(
        [pscustomobject]@{ name_hex = '61'; name_display = 'Same'; from = 'sd:/saves/Same'; seq = 1; manifest = 'm1' },
        [pscustomobject]@{ name_hex = '62'; name_display = 'same'; from = 'sd:/saves/same'; seq = 2; manifest = 'm2' }
    )
    $r7 = Resolve-CwBlockTarget -Blocks @($dupe) -Typed 'Same'
    _CwAssert ($r7.Status -eq 'ambiguous') "two blocks sharing a display name are ambiguous, never a guess"

    # --- spaced-paths (2026-10-30) ------------------------------------
    _CwAssert ($null -eq (Get-CwStagePathProblem -Path 'sd:/apps/CavEX/saves/CaveWii Tutorial/level.dat')) "a spaced world path is sendable"
    _CwAssert ($null -eq (Get-CwStagePathProblem -Path 'sd:/texturepacks/isabella-ii/About Isabella.txt')) "a spaced pack file is sendable"
    _CwAssert ($null -ne (Get-CwStagePathProblem -Path 'sd:/apps/a"b')) "a quote is refused"
    _CwAssert ($null -ne (Get-CwStagePathProblem -Path 'sd:/apps/../x')) "'..' is refused"
    _CwAssert ($null -ne (Get-CwStagePathProblem -Path 'sd:/apps/a /b')) "a segment ending in a space is refused"
    _CwAssert ($null -ne (Get-CwStagePathProblem -Path 'sd:/apps/x ')) "a trailing space is refused"
    _CwAssert ($null -ne (Get-CwStagePathProblem -Path ('sd:/apps/' + [char]0xe9))) "non-ASCII is refused"
    _CwAssert ($null -ne (Get-CwStagePathProblem -Path 'usb:/apps/x')) "a non-sd path is refused"
    _CwAssertThrows { Assert-CwSpacedPathSupported -Capabilities @('put','fetch') -Path 'sd:/apps/a b' } "spaced-paths" "a spaced path to an old server is refused"
    Assert-CwSpacedPathSupported -Capabilities @('put') -Path 'sd:/apps/ab'
    Assert-CwSpacedPathSupported -Capabilities @('spaced-paths') -Path 'sd:/apps/a b'

    Write-Host "PASS cw_saves_common selftest"
}
