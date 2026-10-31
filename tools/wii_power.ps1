# wii_power.ps1 -- remote Wii power control. Thin wrapper over the BLE tool
# (tools/wii_power_ble.py): a SwitchBot Bot on the console's POWER button,
# driven over local Bluetooth from this PC (no hub, no cloud).
# The button works at hardware level even when the software is wedged, so
# press/cycle recovers from any crash short of a mains fault.
#
# Usage:
#   powershell tools/wii_power.ps1 -Action press   # single toggle press
#   powershell tools/wii_power.ps1 -Action cycle   # press, wait, press
param(
    [ValidateSet('press','cycle')] [string]$Action = 'press',
    [int]$CycleGapSec = 8
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Action -eq 'cycle') {
    python "$here\wii_power_ble.py" cycle $CycleGapSec
} else {
    python "$here\wii_power_ble.py" press
}
exit $LASTEXITCODE
