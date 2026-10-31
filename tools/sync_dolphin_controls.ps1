param(
    [string]$UserRoot = "$env:USERPROFILE\Desktop\Tools\Dolphin\cavewii_v2-clean-user"
)

$ErrorActionPreference = "Stop"

function Set-IniValue {
    param(
        [string]$Path,
        [string]$Section,
        [string]$Key,
        [string]$Value
    )

    $lines = @()
    if(Test-Path $Path) {
        $lines = Get-Content $Path
    }

    $sectionHeader = "[$Section]"
    $sectionIndex = -1
    for($i = 0; $i -lt $lines.Count; $i++) {
        if($lines[$i] -eq $sectionHeader) {
            $sectionIndex = $i
            break
        }
    }

    if($sectionIndex -lt 0) {
        if($lines.Count -gt 0 -and $lines[$lines.Count - 1] -ne "") {
            $lines += ""
        }
        $lines += $sectionHeader
        $lines += "$Key = $Value"
        Set-Content -Path $Path -Value $lines -Encoding UTF8
        return
    }

    $insertIndex = $sectionIndex + 1
    while($insertIndex -lt $lines.Count -and -not $lines[$insertIndex].StartsWith("[")) {
        if($lines[$insertIndex] -match ('^{0}\s*=' -f [regex]::Escape($Key))) {
            $lines[$insertIndex] = "$Key = $Value"
            Set-Content -Path $Path -Value $lines -Encoding UTF8
            return
        }
        $insertIndex++
    }

    $before = @()
    $after = @()
    if($insertIndex -gt 0) {
        $before = $lines[0..($insertIndex - 1)]
    }
    if($insertIndex -lt $lines.Count) {
        $after = $lines[$insertIndex..($lines.Count - 1)]
    }

    $newLines = @($before + "$Key = $Value" + $after)
    Set-Content -Path $Path -Value $newLines -Encoding UTF8
}

if(-not (Test-Path $UserRoot)) {
    throw "Dolphin user root not found: $UserRoot"
}

$configDir = Join-Path $UserRoot "Config"
$profileDir = Join-Path $configDir "Profiles\Wiimote"
$dolphinIni = Join-Path $configDir "Dolphin.ini"
$wiimoteIni = Join-Path $configDir "WiimoteNew.ini"
$loadDir = Join-Path $UserRoot "Load"
$sdRawPath = Join-Path $loadDir "WiiSD.raw"
$sdSyncPath = Join-Path $loadDir "WiiSDSync"
$nunchukProfileName = "CaveWii_v2 Keyboard Mouse Nunchuk"
$classicProfileName = "CaveWii_v2 Keyboard Mouse Classic"
$nunchukProfilePath = Join-Path $profileDir ($nunchukProfileName + ".ini")
$classicProfilePath = Join-Path $profileDir ($classicProfileName + ".ini")

New-Item -ItemType Directory -Force -Path $profileDir | Out-Null
New-Item -ItemType Directory -Force -Path $loadDir | Out-Null
New-Item -ItemType Directory -Force -Path $sdSyncPath | Out-Null

$nunchukProfileContent = @'
[Profile]
Device = DInput/0/Keyboard Mouse
Buttons/A = `E` | `Click 2`
Buttons/B = `SPACE`
Buttons/1 = `1`
Buttons/2 = `2`
Buttons/- = `C`
Buttons/+ = `TAB`
Buttons/Home = `ESCAPE` | `Click 3`
D-Pad/Up = `W` | `Click 0` | `F` | `P`
D-Pad/Down = `S` | `Click 0` | `F` | `P`
D-Pad/Left = `A` | `Click 1` | `G` | `O`
D-Pad/Right = `D` | `Click 1` | `G` | `O`
IR/Up = `Cursor Y-`
IR/Down = `Cursor Y+`
IR/Left = `Cursor X-`
IR/Right = `Cursor X+`
Shake/X = `Click 2`
Shake/Y = `Click 2`
Shake/Z = `Click 2`
Extension = Nunchuk
Nunchuk/Buttons/C = `Click 1` | `G` | `O`
Nunchuk/Buttons/Z = `Click 0` | `F` | `P`
Nunchuk/Stick/Up = `W`
Nunchuk/Stick/Down = `S`
Nunchuk/Stick/Left = `A`
Nunchuk/Stick/Right = `D`
Nunchuk/Stick/Calibration = 100.00 141.42 100.00 141.42 100.00 141.42 100.00 141.42
Nunchuk/Shake/X = `Click 2`
Nunchuk/Shake/Y = `Click 2`
Nunchuk/Shake/Z = `Click 2`
'@
Set-Content -Path $nunchukProfilePath -Value $nunchukProfileContent -Encoding UTF8

$classicProfileContent = @'
[Profile]
Device = DInput/0/Keyboard Mouse
Buttons/A = `E` | `Click 2`
Buttons/B = `SPACE`
Buttons/1 = `1`
Buttons/2 = `2`
Buttons/- = `C`
Buttons/+ = `TAB`
Buttons/Home = `ESCAPE` | `Click 3`
D-Pad/Up = `W` | `Click 0` | `F` | `P`
D-Pad/Down = `S` | `Click 0` | `F` | `P`
D-Pad/Left = `A` | `Click 1` | `G` | `O`
D-Pad/Right = `D` | `Click 1` | `G` | `O`
IR/Up = `Cursor Y-`
IR/Down = `Cursor Y+`
IR/Left = `Cursor X-`
IR/Right = `Cursor X+`
Shake/X = `Click 2`
Shake/Y = `Click 2`
Shake/Z = `Click 2`
Extension = Classic
Classic/Buttons/A = `E`
Classic/Buttons/B = `SPACE`
Classic/Buttons/X = `F`
Classic/Buttons/Y = `G`
Classic/Buttons/ZL = `Click 1` | `G` | `O`
Classic/Buttons/ZR = `Click 0` | `F` | `P`
Classic/Buttons/- = `Q`
Classic/Buttons/+ = `TAB`
Classic/Buttons/Home = `ESCAPE`
Classic/Left Stick/Up = `W`
Classic/Left Stick/Down = `S`
Classic/Left Stick/Left = `A`
Classic/Left Stick/Right = `D`
Classic/Left Stick/Calibration = 100.00 141.42 100.00 141.42 100.00 141.42 100.00 141.42
Classic/Triggers/L = `1`
Classic/Triggers/R = `2`
Classic/D-Pad/Up = `UP`
Classic/D-Pad/Down = `DOWN`
Classic/D-Pad/Left = `LEFT`
Classic/D-Pad/Right = `RIGHT`
'@
Set-Content -Path $classicProfilePath -Value $classicProfileContent -Encoding UTF8

$wiimoteContent = @'
[Wiimote1]
Device = DInput/0/Keyboard Mouse
Buttons/A = `E` | `Click 2`
Buttons/B = `SPACE`
Buttons/1 = `1`
Buttons/2 = `2`
Buttons/- = `C`
Buttons/+ = `TAB`
Buttons/Home = `ESCAPE` | `Click 3`
D-Pad/Up = `W` | `Click 0` | `F` | `P`
D-Pad/Down = `S` | `Click 0` | `F` | `P`
D-Pad/Left = `A` | `Click 1` | `G` | `O`
D-Pad/Right = `D` | `Click 1` | `G` | `O`
IR/Up = `Cursor Y-`
IR/Down = `Cursor Y+`
IR/Left = `Cursor X-`
IR/Right = `Cursor X+`
Shake/X = `Click 2`
Shake/Y = `Click 2`
Shake/Z = `Click 2`
IRPassthrough/Object 1 X = `IR Object 1 X`
IRPassthrough/Object 1 Y = `IR Object 1 Y`
IRPassthrough/Object 1 Size = `IR Object 1 Size`
IRPassthrough/Object 2 X = `IR Object 2 X`
IRPassthrough/Object 2 Y = `IR Object 2 Y`
IRPassthrough/Object 2 Size = `IR Object 2 Size`
IRPassthrough/Object 3 X = `IR Object 3 X`
IRPassthrough/Object 3 Y = `IR Object 3 Y`
IRPassthrough/Object 3 Size = `IR Object 3 Size`
IRPassthrough/Object 4 X = `IR Object 4 X`
IRPassthrough/Object 4 Y = `IR Object 4 Y`
IRPassthrough/Object 4 Size = `IR Object 4 Size`
IMUAccelerometer/Up = `Accel Up`
IMUAccelerometer/Down = `Accel Down`
IMUAccelerometer/Left = `Accel Left`
IMUAccelerometer/Right = `Accel Right`
IMUAccelerometer/Forward = `Accel Forward`
IMUAccelerometer/Backward = `Accel Backward`
IMUGyroscope/Pitch Up = `Gyro Pitch Up`
IMUGyroscope/Pitch Down = `Gyro Pitch Down`
IMUGyroscope/Roll Left = `Gyro Roll Left`
IMUGyroscope/Roll Right = `Gyro Roll Right`
IMUGyroscope/Yaw Left = `Gyro Yaw Left`
IMUGyroscope/Yaw Right = `Gyro Yaw Right`
Extension = Nunchuk
Nunchuk/Buttons/C = `Click 1` | `G` | `O`
Nunchuk/Buttons/Z = `Click 0` | `F` | `P`
Nunchuk/Stick/Up = `W`
Nunchuk/Stick/Down = `S`
Nunchuk/Stick/Left = `A`
Nunchuk/Stick/Right = `D`
Nunchuk/Stick/Calibration = 100.00 141.42 100.00 141.42 100.00 141.42 100.00 141.42
Nunchuk/Shake/X = `Click 2`
Nunchuk/Shake/Y = `Click 2`
Nunchuk/Shake/Z = `Click 2`
[Wiimote2]
Device = DInput/0/Keyboard Mouse
[Wiimote3]
Device = DInput/0/Keyboard Mouse
[Wiimote4]
Device = DInput/0/Keyboard Mouse
[BalanceBoard]
Device = DInput/0/Keyboard Mouse
'@
Set-Content -Path $wiimoteIni -Value $wiimoteContent -Encoding UTF8

Set-IniValue -Path $dolphinIni -Section "Controls" -Key "WiimoteSource0" -Value "1"
Set-IniValue -Path $dolphinIni -Section "Controls" -Key "WiimoteSource1" -Value "0"
Set-IniValue -Path $dolphinIni -Section "Controls" -Key "WiimoteSource2" -Value "0"
Set-IniValue -Path $dolphinIni -Section "Controls" -Key "WiimoteSource3" -Value "0"
Set-IniValue -Path $dolphinIni -Section "Controls" -Key "WiimoteProfile1" -Value $nunchukProfileName
Set-IniValue -Path $dolphinIni -Section "General" -Key "WiiSDCardPath" -Value $sdRawPath
Set-IniValue -Path $dolphinIni -Section "General" -Key "WiiSDCardSyncFolder" -Value $sdSyncPath
Set-IniValue -Path $dolphinIni -Section "Core" -Key "WiiSDCard" -Value "True"
Set-IniValue -Path $dolphinIni -Section "Core" -Key "WiiSDCardAllowWrites" -Value "True"
Set-IniValue -Path $dolphinIni -Section "Core" -Key "WiiSDCardEnableFolderSync" -Value "True"

[pscustomobject]@{
    UserRoot = $UserRoot
    DolphinIni = $dolphinIni
    WiimoteIni = $wiimoteIni
    Profile = $nunchukProfilePath
    AlternateProfile = $classicProfilePath
    WiiSDCardPath = $sdRawPath
    WiiSDCardSyncFolder = $sdSyncPath
    WiimoteSource0 = 1
    WiimoteProfile1 = $nunchukProfileName
}
