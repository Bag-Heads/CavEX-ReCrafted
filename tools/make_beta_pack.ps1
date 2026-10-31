<#
    make_beta_pack.ps1 — convert an extracted Java Beta 1.7.3 asset folder into
    a CaveWii_v2-layout texture pack.

    CaveWii loads textures through `paths.texturepack` and falls back to the
    bundled default for any file a pack omits (tex_read, source/platform/texture.c),
    so this places each file Beta provides under the EXACT name CaveWii's loader
    reads (source/platform/texture.c lines noted in $map). Fork-only textures that
    Beta has no source for — gui/iron_chest.png, controls.png,
    pointer.png — are intentionally left to fall back to the bundled default.
    gui_2.png is a fork composite of Beta's gui/gui.png + gui/icons.png (rebuilt
    below), and those two are ALSO copied standalone (the HUD reads both).
    anim.png is also fork-only (Beta animates water/lava/fire/portal procedurally,
    not from a file), BUT it is COMPOSITED below: the fork's filmstrips are kept and
    the pack's real terrain ice tile is overlaid so authentic Beta ice shows in-game
    (the one block a pack otherwise can't override — see docs/atlas_parity_audit.md).

    S1 SOUND stage: the user's Beta resources/ audio is transcoded (via the
    built sound_transcode host tool) into <Dest>/sound/... in the exact layout
    audio_registry.c scans, per the registry's own --manifest table. See the
    stage banner below for the decompile-cited sound/newsound rules.

    S3 MUSIC stage: the user's Beta resources/music/ + resources/newmusic/
    .ogg files are COPIED (never transcoded -- streaming wants the compressed
    file) into <Dest>/music/, the layout audio_music.c's registry scan reads
    (audio_music_table.h). No bundled default exists; a pack with none found
    ships silently musicless, matching a fresh checkout with no imported pack.

    S3b RECORDS stage: the user's Beta resources/streaming/ .ogg files (the
    two jukebox discs "13" + "Cat") are likewise COPIED into <Dest>/records/,
    the layout audio_music_play_record_for_item reads. Same no-bundled-
    default rule -- a pack with none found ships jukeboxes that accept/eject/
    persist records faithfully but play nothing.

    Usage:
      pwsh tools/make_beta_pack.ps1 -Source C:\path\to\beta_assets -Dest texturepacks\beta
        [-SoundSource C:\...\.minecraft\resources]   # default: <Source>\resources
        [-Transcoder  C:\...\build\sound_transcode.exe]  # default: newest ..\build*\

    Legal: this ships no official textures OR sounds. You supply the Beta assets
    (extracted from a copy you own); the output pack is yours to keep locally.
    Do not redistribute it.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Dest,
    [string]$SoundSource = "",
    [string]$Transcoder = ""
)

Add-Type -AssemblyName System.Drawing

# Beta 1.7.3 jar path  ->  CaveWii loader read-path (source/platform/texture.c).
# The output (right) side MUST equal the loader's read-path exactly or the file is
# written but never displayed (tex_read silently falls back to the bundled default).
$map = [ordered]@{
    "terrain.png"            = "terrain.png"           # :111 block atlas (rebuilt at load)
    "particles.png"          = "particles.png"         # :133 particle atlas (rebuilt at load)
    "font/default.png"       = "font/default.png"      # :127 bitmap font
    "gui/items.png"          = "gui/items.png"         # :162 item atlas
    "gui/gui.png"            = "gui/gui.png"            # :158 hotbar/widgets (also gui_2 input)
    "gui/icons.png"          = "gui/icons.png"          # :155 HUD icons (also gui_2 input)
    "gui/inventory.png"      = "gui/inventory.png"     # :141
    "gui/crafting.png"       = "gui/crafting.png"      # :143
    "gui/furnace.png"        = "gui/furnace.png"       # :145
    "gui/container.png"      = "gui/chest.png"         # :147 (Beta names the chest GUI container.png)
    "item/arrows.png"        = "item/arrows.png"       # Beta RenderArrow projectile sheet
    "item/cart.png"          = "item/cart.png"         # :165 minecart skin
    "mob/creeper.png"        = "mob/creeper.png"       # :166 creeper + the universal mob fallback
    "mob/zombie.png"         = "mob/zombie.png"        # :170
    "mob/skeleton.png"       = "mob/skeleton.png"      # :174
    "mob/spider.png"         = "mob/spider.png"        # :175
    "mob/spider_eyes.png"    = "mob/spider_eyes.png"   # :179
    "mob/pig.png"            = "mob/pig.png"           # :181
    "mob/cow.png"            = "mob/cow.png"           # :185
    "mob/chicken.png"        = "mob/chicken.png"       # :186
    "mob/sheep.png"          = "mob/sheep.png"         # :189
    "mob/sheep_fur.png"      = "mob/sheep_fur.png"     # :190
    "mob/squid.png"          = "mob/squid.png"         # :195
    "mob/slime.png"          = "mob/slime.png"         # :196
    "mob/char.png"           = "mob/char.png"          # :230 player skin
    "environment/clouds.png" = "environment/clouds.png" # :201
    # render-slice (RENDER_DISPATCH.md): the partial-pack trap's THIRD occurrence --
    # environment/ only ever mapped clouds.png, so a pack rebuild silently omitted
    # precipitation art. EntityRenderer.renderRainSnow reads both by exact loader
    # path (source/platform/texture.c TEX_LOAD_FILE calls added alongside this map
    # entry); rain.png is genuinely blue-tinted RGBA (not grayscale -- verified
    # against the owned jar, max per-channel delta 125 among visible texels), snow.png
    # is exhaustively grayscale (delta 0), matching the format choice made at the
    # loader (RGBA16 / IA4 respectively).
    "environment/rain.png"   = "environment/rain.png"   # precip render (RENDER_DISPATCH.md)
    "environment/snow.png"   = "environment/snow.png"   # precip render (RENDER_DISPATCH.md)
    "terrain/sun.png"        = "terrain/sun.png"       # :203
    "terrain/moon.png"       = "terrain/moon.png"      # :204
    "misc/shadow.png"        = "misc/shadow.png"       # :205 entity drop shadow
    "misc/water.png"         = "misc/water.png"        # :206 underwater overlay
    "misc/grasscolor.png"    = "misc/grasscolor.png"   # biome_color.c:65 grass tint colormap
    "misc/foliagecolor.png"  = "misc/foliagecolor.png" # biome_color.c:66 foliage tint colormap
    "armor/chain_1.png"      = "armor/chain_1.png"     # :209
    "armor/chain_2.png"      = "armor/chain_2.png"     # :211
    "armor/cloth_1.png"      = "armor/cloth_1.png"     # :213
    "armor/cloth_2.png"      = "armor/cloth_2.png"     # :215
    "armor/gold_1.png"       = "armor/gold_1.png"      # :217
    "armor/gold_2.png"       = "armor/gold_2.png"      # :219
    "armor/iron_1.png"       = "armor/iron_1.png"      # :221
    "armor/iron_2.png"       = "armor/iron_2.png"      # :223
    "armor/diamond_1.png"    = "armor/diamond_1.png"   # :225
    "armor/diamond_2.png"    = "armor/diamond_2.png"   # :227
}

# Loader read-paths Beta has NO source for -> always fall back to the bundled
# default. gui_2.png AND anim.png are produced by the composite steps below
# (gui_2 from Beta's gui/icons; anim.png = fork filmstrips + the pack's real ice),
# so neither is a plain fallback -- they are gap-checked individually further down.
# pointer.png is NOT fork-only any more: since 2026-10-11 assets/ is the Earth & Ash
# default, so a Beta pack gets the Homebrew pointer from packs/beta-base (copied below).
$forkOnly = @("gui/iron_chest.png", "controls.png")

if (-not (Test-Path $Source)) { throw "Source not found: $Source" }

$defaults = Join-Path $PSScriptRoot "..\assets"  # the bundled default pack (fallback source)
# Beta-only base (packs/beta-base): Beta FX colours for anim.png + the Homebrew pointer.
$betaBase = Join-Path $PSScriptRoot "..\packs\beta-base"
foreach ($b in @("anim.png", "pointer.png")) {
    if (-not (Test-Path (Join-Path $betaBase $b))) { throw "Missing $betaBase\$b -- a Beta pack would inherit Earth & Ash art" }
}
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
Copy-Item -LiteralPath (Join-Path $betaBase "pointer.png") -Destination (Join-Path $Dest "pointer.png") -Force
Write-Host "[make_beta_pack] pointer.png <- packs/beta-base (Homebrew Channel pointer, GPLv2)"
# Beta-coloured anim.png even when no terrain.png arrives (the ice composite below
# overwrites it when one does); never fall back to the Earth & Ash assets/anim.png.
Copy-Item -LiteralPath (Join-Path $betaBase "anim.png") -Destination (Join-Path $Dest "anim.png") -Force

$copied = 0
$terrainCopied = $false   # did THIS run copy terrain.png from $Source? (gates the ice composite)
$missingSrc = @()        # mapped, but absent from the user's jar/resources tree
foreach ($src in $map.Keys) {
    $s = Join-Path $Source ($src -replace '/', '\')
    $d = Join-Path $Dest   ($map[$src] -replace '/', '\')
    if (Test-Path $s) {
        New-Item -ItemType Directory -Force -Path (Split-Path $d) | Out-Null
        Copy-Item $s $d -Force
        $copied++
        if ($map[$src] -eq 'terrain.png') { $terrainCopied = $true }
    } else {
        $missingSrc += $map[$src]
    }
}

# Composite the HUD sheet. The fork merges Beta's gui/gui.png (hotbar +
# selection) and gui/icons.png (crosshair + hearts) into one custom gui_2.png,
# so no single Beta file maps to it. Rebuild it from the fork's gui_2.png as a
# base, with Beta's art placed at the fork's slot coordinates (dest <- src).
$gui2Made = $false
$forkGui2 = Join-Path $PSScriptRoot "..\assets\gui_2.png"
$srcGui = Join-Path $Source "gui\gui.png"
$srcIco = Join-Path $Source "gui\icons.png"
if ((Test-Path $forkGui2) -and (Test-Path $srcGui) -and (Test-Path $srcIco)) {
    $base = [System.Drawing.Image]::FromFile($forkGui2)
    $gui = [System.Drawing.Image]::FromFile($srcGui)
    $ico = [System.Drawing.Image]::FromFile($srcIco)
    $bmp = New-Object System.Drawing.Bitmap $base.Width, $base.Height
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $gfx.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $gfx.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $gfx.DrawImage($base, 0, 0, $base.Width, $base.Height)
    $px = [System.Drawing.GraphicsUnit]::Pixel
    $gfx.DrawImage($gui, (New-Object System.Drawing.Rectangle 0, 0, 182, 22), (New-Object System.Drawing.Rectangle 0, 0, 182, 22), $px)   # hotbar
    $gfx.DrawImage($gui, (New-Object System.Drawing.Rectangle 208, 0, 24, 24), (New-Object System.Drawing.Rectangle 0, 22, 24, 24), $px)  # selection
    $gfx.DrawImage($ico, (New-Object System.Drawing.Rectangle 0, 229, 16, 16), (New-Object System.Drawing.Rectangle 0, 0, 16, 16), $px)   # crosshair
    $gfx.DrawImage($ico, (New-Object System.Drawing.Rectangle 16, 229, 9, 9), (New-Object System.Drawing.Rectangle 16, 0, 9, 9), $px)     # empty heart
    $gfx.DrawImage($ico, (New-Object System.Drawing.Rectangle 52, 229, 9, 9), (New-Object System.Drawing.Rectangle 52, 0, 9, 9), $px)     # full heart
    $gfx.Dispose()
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    $bmp.Save((Join-Path $Dest "gui_2.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $base.Dispose(); $gui.Dispose(); $ico.Dispose(); $bmp.Dispose()
    $gui2Made = $true
    Write-Host "[make_beta_pack] composited HUD gui_2.png (hotbar/selection/crosshair/hearts)"
} else {
    Write-Host "[make_beta_pack] skipped gui_2.png composite (needs ..\assets\gui_2.png + source gui/gui.png + gui/icons.png) -> falls back to default"
}

# Composite the ICE into the animation sheet (anim.png). anim.png is a fork-only
# sheet: Beta has no single anim.png file -- the live game generates the animated
# water/lava/fire/portal tiles PROCEDURALLY at runtime (TextureWaterFX/LavaFX/
# FireFX/PortalFX), so a texture pack ships NO animated source for them and the
# fork hand-paints those filmstrips. ICE, however, is a STATIC block: its only
# authentic art IS the pack's terrain.png ice tile (Beta blockIndexInTexture 67 =
# tile col 3, row 4). CaveWii renders ice (and the fancy water/lava/fire/portal)
# in the translucent pass bound to anim.png, sampling anim tile (4,0) for ice --
# so the authentic terrain ice never reaches it: ice is the one block a Beta pack
# cannot override (see docs/atlas_parity_audit.md, ice section).
#
# Close that gap WITHOUT touching the render path: start from the fork's bundled
# anim.png (keeps its CC water/lava/fire/portal filmstrips byte-for-byte) and
# overlay the pack's real ice tile into ICE's FULL scroll footprint -- anim
# columns 4 AND 11, rows 0..13. (world_render's shared matrix_anim scroll walks
# ice's UVs down its column and +7 columns on the X phase, visiting all 28 of
# those cells; painting them with the identical static tile keeps ice flicker-
# free.) The cell layout mirrors tex_atlas_compute EXACTLY -- 14 cells/row, 18px
# pitch, 16px tile content at col*18+3, plus a 1px clamped (edge-replicated)
# border -- so the UVs line up with the renderer's TEX_OFFSET(x)=x*18+3 sampling.
# Like the gui_2.png composite above, this writes into the LOCAL pack from the
# user's OWN Beta terrain.png; the repo's bundled anim.png stays the fork's CC art.
$animMade = $false
$forkAnim = Join-Path $betaBase "anim.png"            # Beta FX base (packs/beta-base), never the E&A default
$packTerrain = Join-Path $Dest "terrain.png"          # the pack's real terrain (copied above)
# Gate on $terrainCopied (THIS run copied terrain.png from $Source), NOT a bare Test-Path: a reused
# -Dest could carry a stale terrain.png from an earlier, DIFFERENT -Source, and overlaying ice from
# that would mismatch the rest of the pack. $terrainCopied true => $packTerrain is the fresh copy.
if ((Test-Path $forkAnim) -and $terrainCopied) {
    # Load anim.png from file as a writable 32bpp-ARGB bitmap (preserves ice's
    # translucent alpha); SetPixel over it -- no DrawImage/resampling, so the
    # untouched filmstrip columns stay byte-identical to the fork base.
    $canvas = New-Object System.Drawing.Bitmap $forkAnim
    $terr   = New-Object System.Drawing.Bitmap $packTerrain
    $ts = [int]($terr.Width / 16)   # terrain tile size (16 vanilla, 32 HD pack, ...)
    $iceCol = 3; $iceRow = 4        # Beta ice blockIndexInTexture 67 = tile (3,4)
    foreach ($col in 4, 11) {
        for ($row = 0; $row -le 13; $row++) {
            $ox = $col * 18 + 2     # 18x18 cell origin (incl. the 1px left/top border)
            $oy = $row * 18 + 2
            for ($di = 0; $di -lt 18; $di++) {
                for ($dj = 0; $dj -lt 18; $dj++) {
                    # clamp_n border: di/dj -1 and 16 fold back onto edge texels 0/15.
                    $si = [Math]::Min([Math]::Max($di - 1, 0), 15)
                    $sj = [Math]::Min([Math]::Max($dj - 1, 0), 15)
                    # nearest-sample the source ice tile (scales an HD tile down to 16px).
                    $tx = $iceCol * $ts + [int]($si * $ts / 16)
                    $ty = $iceRow * $ts + [int]($sj * $ts / 16)
                    $canvas.SetPixel($ox + $di, $oy + $dj, $terr.GetPixel($tx, $ty))
                }
            }
        }
    }
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    $canvas.Save((Join-Path $Dest "anim.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $terr.Dispose(); $canvas.Dispose()
    $animMade = $true
    Write-Host "[make_beta_pack] composited anim.png (authentic Beta ice over the fork water/lava/fire/portal filmstrips)"
} else {
    Write-Host "[make_beta_pack] skipped anim.png ice composite (needs ..\assets\anim.png + the pack's terrain.png) -> falls back to bundled fork anim.png"
}

# --- FX BAKE STAGE (2026-07-30) ------------------------------------------------------
# The ice composite above deliberately keeps the fork's water/lava/fire/portal filmstrips
# byte-for-byte. That left FOUR of the six animated cells as fork CC art while this script
# printed a reassuring "anim.png: composited" line -- the gate proved a path EXISTED, never
# what was in it, and the portal cell was literally the text "PORTAL" repeated down all 14
# rows. CaveWii has faithful C ports of Beta's TextureLavaFX / TextureFlamesFX /
# TexturePortalFX; bake_anim_fx.sh runs them offline and composites the real filmstrips.
# Without this stage a pack rebuild silently REGRESSES the bake back to fork art.
#
# Water is deliberately NOT covered: there is no TextureWaterFlowFX port. Do not claim
# "all four animated tiles are Beta" after this runs.
# Output is CC-clean -- generated by our own ports, zero Mojang bytes (M3(B) holds).
$animBaked = $false
if ($animMade) {
    $bake = Join-Path $PSScriptRoot "gen_anim_fx\bake_anim_fx.sh"
    $target = Join-Path $Dest "anim.png"
    if (Test-Path $bake) {
        # bash + a C compiler + Python/Pillow. On a box without them this is a hard fail,
        # not a shrug: shipping fork art while announcing a Beta pack is the exact defect
        # this stage exists to close.
        # Resolve Git Bash explicitly: a bare `bash` on Windows often resolves to WSL's
        # C:\Windows\system32\bash.exe, which cannot take Windows paths. Git Bash receives
        # the two paths as plain arguments, which survives spaces and apostrophes under both
        # Windows PowerShell 5.1 and PowerShell 7 (verified 2026-09-24 with "o'brien dir").
        $gitBash = $null
        $gitCmd = Get-Command git -ErrorAction SilentlyContinue
        if ($gitCmd) {
            $cand = Join-Path (Split-Path (Split-Path $gitCmd.Source -Parent) -Parent) 'bin\bash.exe'
            if (Test-Path $cand) { $gitBash = $cand }
        }
        if (-not $gitBash -and (Test-Path 'C:\Program Files\Git\bin\bash.exe')) { $gitBash = 'C:\Program Files\Git\bin\bash.exe' }
        if (-not $gitBash) {
            Write-Error "Git Bash not found (need Git for Windows' bin\bash.exe; WSL bash cannot run the Beta FX bake)."
            $global:LASTEXITCODE = 1
        } else {
            & $gitBash $bake $target
        }
        if ($LASTEXITCODE -eq 0) {
            $animBaked = $true
            Write-Host "[make_beta_pack] baked Beta FX filmstrips into anim.png (fire/lava/portal)"
        } else {
            $gaps += "anim.png (FX bake failed, exit $LASTEXITCODE)"
            Write-Warning "[make_beta_pack] anim.png FX bake FAILED -- the pack would ship fork fire/lava/portal art"
        }
    } else {
        $gaps += "anim.png (FX bake tool missing)"
        Write-Warning "[make_beta_pack] $bake not found -- cannot bake the Beta FX filmstrips"
    }
}

Write-Host "[make_beta_pack] copied $copied file(s) -> $Dest"

# --- S1 sound stage: transcode the user's Beta resource audio into the pack.
# Dir mapping per the decompile [740c583: Minecraft.installResource:1380-1394]:
# "sound" AND "newsound" BOTH feed sndManager.addSound -- ONE pool, a plain
# UNION, no override precedence ("sound3" does not exist in b1.7.3 and is
# ignored; "streaming"/"music"/"newmusic" route to the separate streaming/
# music pools -> S3, OUT of scope for this stage). [SoundPool.addSound] strips
# the extension then ALL trailing digits, so every <base><digits>.(ogg|wav) in
# either dir is one variant of key <dir.path.base>; the union is renumbered
# deterministically into <base>1..<base>13 (+ bare <base> for a 14th) -- the
# engine's scan namespace (audio_sound_table.h AUDIO_MAX_VARIANTS = 14, S2:
# raised from 10 because Beta's cave mood pool is cave1..cave13). The key
# list comes from `sound_transcode --manifest` == the SAME table the engine
# reads, so builder and loader cannot drift. The engine falls back to the
# bundled default per KEY (audio_registry.c), so a renumbered overlay never
# interleaves with placeholder files.

if (-not $SoundSource) {
    $srcRes = Join-Path $Source "resources"
    if (Test-Path $srcRes) { $SoundSource = $srcRes }
    elseif ((Test-Path (Join-Path $Source "newsound")) -or (Test-Path (Join-Path $Source "sound"))) {
        # -Source already IS a resources tree (sound/newsound at its root).
        $SoundSource = $Source
    }
}
$haveSoundSource = $false
if ($SoundSource) { if (Test-Path $SoundSource) { $haveSoundSource = $true } }

if ($haveSoundSource -and -not $Transcoder) {
    # Probe the build dirs, newest first (both build-* and build_* exist).
    $buildDirs = @(Get-ChildItem -Path (Join-Path $PSScriptRoot "..") -Directory |
        Where-Object { $_.Name -like "build*" } | Sort-Object LastWriteTime -Descending)
    foreach ($b in $buildDirs) {
        foreach ($exe in @("sound_transcode.exe", "sound_transcode")) {
            $p = Join-Path $b.FullName $exe
            if (-not $Transcoder) { if (Test-Path $p) { $Transcoder = $p } }
        }
    }
}

$soundOverlaid = @()   # keys transcoded from the user's resources (with counts)
$soundFallback = @()   # keys left to the bundled default pack
$soundGaps = @()       # keys with NEITHER source nor default (would be silent)
$soundFails = @()      # transcode invocations that FAILED (hard gate)

if (-not $haveSoundSource) {
    Write-Host "[make_beta_pack] sound stage: no sound source found (-SoundSource, <Source>\resources, or sound/newsound under <Source>); all keys use the bundled default sounds (coverage enforced by test_audio_env)"
} elseif (-not $Transcoder) {
    # Loud, not silent: writing no sounds while a source exists is exactly the
    # dead-remapping failure mode the M3(A) coverage gate was built against.
    Write-Warning "[make_beta_pack] sound source present ($SoundSource) but no sound_transcode(.exe) in ..\build*; build it (ninja sound_transcode) or pass -Transcoder"
    exit 1
} else {
    Write-Host "[make_beta_pack] sound stage: source=$SoundSource transcoder=$Transcoder"
    # Review F1: the renumbering (base1..9) is NOT idempotent across runs -- a
    # rebuild against a changed source would leave STALE numbered variants from
    # the previous run in the pack, and the per-key ownership rule would then
    # play them as live variants (the exact mixed-pool failure the per-key
    # rule exists to kill). Clean the sound output tree before every build.
    $soundOut = Join-Path $Dest "sound"
    if (Test-Path $soundOut) {
        Remove-Item -Recurse -Force -Confirm:$false $soundOut
        Write-Host "[make_beta_pack] sound stage: cleaned stale $soundOut"
    }
    $manifest = @(& $Transcoder --manifest)
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "[make_beta_pack] '$Transcoder --manifest' failed (exit $LASTEXITCODE)"
        exit 1
    }
    foreach ($row in $manifest) {
        $parts = $row.Split("`t")
        if ($parts.Count -ne 3) {
            Write-Warning "[make_beta_pack] malformed manifest row: '$row'"
            exit 1
        }
        $key = $parts[1]
        $sub = $parts[2] -replace '^sound/', '' -replace '\*\.wav$', ''  # e.g. step/grass
        $base = ($sub -split '/')[-1]
        $subdir = ""
        if ($sub.Contains("/")) { $subdir = $sub.Substring(0, $sub.LastIndexOf("/")) }

        # SoundPool digit-strip in reverse: base + ANY trailing digits, ogg or wav.
        $rx = '^' + [regex]::Escape($base) + '[0-9]*\.(ogg|wav)$'
        $srcFiles = @()
        foreach ($dir in @("newsound", "sound")) {   # union -- see the banner
            $d = Join-Path $SoundSource $dir
            if ($subdir) { $d = Join-Path $d ($subdir -replace '/', '\') }
            if (Test-Path $d) {
                $srcFiles += @(Get-ChildItem -Path $d -File | Where-Object { $_.Name -match $rx } | Sort-Object Name)
            }
        }
        # Dedup BYTE-IDENTICAL union duplicates. b1.7.3's overlap is an exact
        # mirror -- the stone/wood/gravel/grass step sets exist in BOTH dirs
        # as the same bytes, one copy each -- and for equal copy counts a
        # uniform pick over the deduped pool is distribution-IDENTICAL to the
        # duplicated one (verified vs the user resources: sound/step/* ==
        # newsound/step/* by hash). DIFFERING same-name content (hash
        # mismatch; no such case in b1.7.3) keeps both, faithfully.
        $seen = @{}
        $dedup = @()
        foreach ($f in $srcFiles) {
            $h = (Get-FileHash -Path $f.FullName -Algorithm SHA256).Hash
            if (-not $seen.ContainsKey($h)) {
                $seen[$h] = $true
                $dedup += $f
            }
        }
        $srcFiles = $dedup

        $written = 0
        if ($srcFiles.Count -gt 0) {
            $outDir = Join-Path $Dest "sound"
            if ($subdir) { $outDir = Join-Path $outDir ($subdir -replace '/', '\') }
            New-Item -ItemType Directory -Force -Path $outDir | Out-Null
            $n = 0
            foreach ($f in $srcFiles) {
                $n++
                if ($n -le 13) { $outName = "$base$n.wav" }
                elseif ($n -eq 14) { $outName = "$base.wav" }
                else {
                    Write-Warning "[make_beta_pack] $key`: '$($f.Name)' exceeds the engine's 14-variant scan (bare + 1..13), skipped"
                    continue
                }
                & $Transcoder $f.FullName (Join-Path $outDir $outName) | Out-Null
                if ($LASTEXITCODE -ne 0) {
                    $soundFails += "$key <- $($f.FullName)"
                } else {
                    $written++
                }
            }
        }

        if ($written -gt 0) {
            $soundOverlaid += ("{0} ({1})" -f $key, $written)
        } else {
            # Default coverage check mirrors the engine scan exactly: bare base
            # or base+trailing digits (1..13 incl. two-digit -- S2), .wav or
            # .ogg, under assets/sound/.
            $defDir = Join-Path $defaults "sound"
            if ($subdir) { $defDir = Join-Path $defDir ($subdir -replace '/', '\') }
            $defRx = '^' + [regex]::Escape($base) + '[0-9]*\.(wav|ogg)$'
            $hasDefault = $false
            if (Test-Path $defDir) {
                $hasDefault = @(Get-ChildItem -Path $defDir -File | Where-Object { $_.Name -match $defRx }).Count -gt 0
            }
            if ($hasDefault) { $soundFallback += $key } else { $soundGaps += $key }
        }
    }

    Write-Host ("[make_beta_pack] sound coverage: {0}/{2} overlaid from your resources, {1}/{2} bundled default" -f $soundOverlaid.Count, $soundFallback.Count, $manifest.Count)
    # Review F4: a present-but-wrong source that matches NOTHING is the
    # dead-remapping smell (M3A doctrine) -- warn loudly, don't pass silently.
    if ($soundOverlaid.Count -eq 0) {
        Write-Warning "[make_beta_pack] sound source present but 0/$($manifest.Count) keys matched anything under it -- wrong -SoundSource? (expected a .minecraft resources tree with sound/ + newsound/)"
    }
    foreach ($k in $soundOverlaid) { Write-Host "    overlaid $k" }
    foreach ($k in $soundFallback) { Write-Host "    default  $k" }
    Write-Host "[make_beta_pack] streaming/ (jukebox records) is the separate streaming pool -> <Dest>\records\, staged below [740c583: installResource / RenderGlobal.playStreaming]"
}

# --- S3 MUSIC stage: copy -- NOT transcode -- the user's Beta music/+newmusic/
# .ogg files into <Dest>/music/. Streaming wants the compressed .ogg ON DISK
# (audio_music.c pull-decodes it a small chunk at a time via
# stb_vorbis_open_filename, source/platform/audio_music.c); transcoding to wav
# here would be exactly backwards for a streaming voice -- it would balloon a
# ~2MB compressed real Beta track into ~15MB of decoded PCM sitting on the SD
# card for no benefit, the very thing streaming exists to avoid (S0-S2's
# decode-AT-LOAD-into-one-fixed-arena model is the right tool for short
# one-shots, the wrong one here -- docs/finish_line_plan.md's own S3 note).
#
# Beta feeds BOTH "music" AND "newmusic" into the SAME soundPoolMusic pool
# [740c583: Minecraft.installResource -- both `equalsIgnoreCase` branches call
# `sndManager.addMusic`, not two different pools], and playRandomMusicIfReady
# picks uniformly across the WHOLE pool with no dimension gate -- calm1-3 AND
# hal1-4/nuance1-2/piano1-3 can all play in the Overworld. audio_music_table.h's
# scan therefore expects both source dirs merged into ONE <Dest>/music/, which
# is exactly what a real Beta 1.7.3 resources tree contains: calm1-3,
# hal1-4, nuance1-2, piano1-3 -- 10 files, no name collisions between the two
# source dirs, so a flat copy needs no renumbering (unlike the sound stage's
# sound+newsound union).
#
# No bundled default exists for music (docs/deviations.md's legal model --
# NEVER bundle/commit audio -- has no CC0 substitute worth shipping for C418's
# tracks), so a pack with nothing found here is NOT a gap: it ships no
# background music, exactly like a fresh CaveWii checkout with no imported
# pack at all, faithfully silent rather than broken.
$musicCopied = @()
if ($haveSoundSource) {
    $musicOut = Join-Path $Dest "music"
    # Review F1's precedent (the sound stage's own renumbering-not-idempotent
    # note): clean before every build so a reused -Dest never carries a STALE
    # track from a previous, different -Source alongside the fresh ones.
    if (Test-Path $musicOut) {
        Remove-Item -Recurse -Force -Confirm:$false $musicOut
    }
    foreach ($dir in @("music", "newmusic")) {
        $d = Join-Path $SoundSource $dir
        if (Test-Path $d) {
            New-Item -ItemType Directory -Force -Path $musicOut | Out-Null
            Get-ChildItem -Path $d -File -Filter "*.ogg" | ForEach-Object {
                Copy-Item $_.FullName (Join-Path $musicOut $_.Name) -Force
                $musicCopied += $_.Name
            }
        }
    }
    if ($musicCopied.Count -gt 0) {
        Write-Host "[make_beta_pack] music stage: copied $($musicCopied.Count) track(s) -> $musicOut ($($musicCopied -join ', '))"
    } else {
        Write-Host "[make_beta_pack] music stage: no music/*.ogg or newmusic/*.ogg found under $SoundSource -- pack ships no background music (legal: per-client-import-only, never bundled)"
    }
} else {
    Write-Host "[make_beta_pack] music stage: no sound source found -- pack ships no background music (legal: per-client-import-only, never bundled)"
}

# --- S3b RECORDS stage: copy -- NOT transcode -- the user's Beta streaming/
# .ogg files (the two jukebox discs, "13" + "Cat") into <Dest>/records/.
# Beta keeps these in their OWN pool, separate from music/newmusic
# [740c583: Minecraft.installResource -- `equalsIgnoreCase("streaming")` ->
# sndManager.addStreaming, a DIFFERENT method than the addMusic calls the S3
# MUSIC stage above feeds; see that stage's banner for the music-pool half
# of this split], played by the jukebox via a POSITIONED
# SoundManager.playStreaming call [RenderGlobal.playRecord], not the random
# background timer. Same streaming-not-transcoding rationale as the S3
# MUSIC stage: audio_music.c pull-decodes the compressed .ogg a chunk at a
# time (source/platform/audio_music.c's audio_music_play_record), so a
# transcoded .wav on the SD card would be exactly backwards here too.
#
# Beta 1.7.3 ships exactly two: streaming/13.ogg and streaming/cat.ogg
# (item_types.h ITEM_RECORD_13=2256 / ITEM_RECORD_CAT=2257,
# audio_music_record_file_stem in audio_music.c). No bundled default exists
# (the same never-bundle-audio legal model as sound/music) -- a pack with
# neither found is NOT a gap: jukeboxes still accept/eject/persist records
# faithfully (the SIM half, block_jukebox.c/server_jukebox.c), they just
# play nothing, exactly like a fresh checkout with no imported pack.
$recordsCopied = @()
if ($haveSoundSource) {
    $recordsOut = Join-Path $Dest "records"
    $streamingSrc = Join-Path $SoundSource "streaming"
    # Review F1's precedent (the sound/music stages' own note): clean before
    # every build so a reused -Dest never carries a STALE record from an
    # earlier, different -Source alongside a fresh one.
    if (Test-Path $recordsOut) {
        Remove-Item -Recurse -Force -Confirm:$false $recordsOut
    }
    if (Test-Path $streamingSrc) {
        New-Item -ItemType Directory -Force -Path $recordsOut | Out-Null
        Get-ChildItem -Path $streamingSrc -File -Filter "*.ogg" | ForEach-Object {
            Copy-Item $_.FullName (Join-Path $recordsOut $_.Name) -Force
            $recordsCopied += $_.Name
        }
    }
    if ($recordsCopied.Count -gt 0) {
        Write-Host "[make_beta_pack] records stage: copied $($recordsCopied.Count) record(s) -> $recordsOut ($($recordsCopied -join ', '))"
    } else {
        Write-Host "[make_beta_pack] records stage: no streaming/*.ogg found under $SoundSource -- jukeboxes accept/eject records but play nothing (legal: per-client-import-only, never bundled)"
    }
} else {
    Write-Host "[make_beta_pack] records stage: no sound source found -- jukeboxes accept/eject records but play nothing (legal: per-client-import-only, never bundled)"
}

# --- Manifest verification: every loader read-path is COPIED, FALLBACK, or a GAP.
# The authoritative manifest is $map.Values (Beta-sourced) + gui_2.png (composited)
# + $forkOnly (no Beta source). A GAP = a path with neither a user-jar source NOR a
# bundled default == it will render NULL/broken; that is the one loud failure.
$gaps = @()
foreach ($out in $missingSrc) {
    if (-not (Test-Path (Join-Path $defaults ($out -replace '/', '\')))) { $gaps += $out }
}
if (-not $gui2Made -and -not (Test-Path (Join-Path $defaults "gui_2.png"))) { $gaps += "gui_2.png" }
# anim.png: composited (fork filmstrips + real ice) when a terrain.png is present;
# else it falls back to the bundled fork anim.png. Broken only if NEITHER exists.
if (-not (Test-Path (Join-Path $Dest "anim.png"))) { $gaps += "anim.png" }
foreach ($f in $forkOnly) {
    if (-not (Test-Path (Join-Path $defaults ($f -replace '/', '\')))) { $gaps += $f }
}

if ($missingSrc.Count) {
    Write-Host "[make_beta_pack] not in your source (fall back to bundled default): $($missingSrc -join ', ')"
}
Write-Host "[make_beta_pack] fork-only (always bundled default): $($forkOnly -join ', ')"
# Say exactly what is in the sheet. The old wording ("composited (fork filmstrips +
# authentic Beta ice)") was literally true and completely hid that four of the six animated
# cells were fork art -- the report read like success. Water is still fork art even after
# the bake; that is stated rather than glossed.
Write-Host "[make_beta_pack] anim.png: $(if ($animBaked) { 'composited (Beta FX bake: fire/lava/portal + authentic Beta ice; WATER is still fork art -- no TextureWaterFlowFX port)' } elseif ($animMade) { 'composited (authentic Beta ice ONLY -- fire/lava/portal/water are FORK art, the FX bake did not run)' } else { 'bundled fork default (no terrain.png to overlay ice from)' })"
if ($soundFails.Count) {
    Write-Warning "[make_beta_pack] $($soundFails.Count) sound transcode(s) FAILED (broken source or tool): $($soundFails -join '; ')"
}
if ($soundGaps.Count) {
    Write-Warning "[make_beta_pack] $($soundGaps.Count) sound key(s) have NEITHER a resource source NOR a bundled default (will be silent): $($soundGaps -join ', ')"
}
if ($gaps.Count -or $soundFails.Count -or $soundGaps.Count) {
    if ($gaps.Count) {
        Write-Warning "[make_beta_pack] $($gaps.Count) read-path(s) have NEITHER a source NOR a bundled default (will render broken): $($gaps -join ', ')"
    }
    exit 1
} else {
    Write-Host "[make_beta_pack] OK: every loader read-path and sound key is covered (your assets or the bundled default)."
}
Write-Host "Set config_wii.json paths.texturepack to '$Dest' (relative to the SD root) to use it."
