<#
.SYNOPSIS
    One-time: share (bind) the forwarded USB device(s) with usbipd so the dev container
    can attach them automatically.

.DESCRIPTION
    Windows requires administrator rights to share a USB device, so this is the single
    manual step. The bind is *persistent* across reboots - you only run this once per
    device (or after re-imaging Windows).

    Run from an ELEVATED PowerShell:
        .\.devcontainer\scripts\usbip-bind.ps1

    After this, opening / rebuilding the dev container automatically attaches the device
    and loads the kernel modules (see host-usb-init.ps1).
#>
[CmdletBinding()]
param(
    # Hardware IDs (VID:PID) to bind:
    #   2fe3:0001  board running the app firmware (normal runtime)
    #   2fe3:0100  board in MCUboot serial-recovery / DFU mode (a different PID). This
    #              device only exists WHILE the board is held in DFU mode, so to bind it
    #              you must put the board in recovery mode first (hold the Left button at
    #              reset) and then run this script — otherwise it's reported and skipped.
    #   1366:0101  SEGGER J-Link debug probe
    # A device that isn't plugged in (or not currently in the matching mode) is reported
    # and skipped — bind it later when it's connected/in the right mode.
    [string[]] $HardwareIds = @('2fe3:0001', '2fe3:0100', '1366:0101')
)

$ErrorActionPreference = 'Stop'

if (-not (Get-Command usbipd -ErrorAction SilentlyContinue)) {
    Write-Error "usbipd-win not found. Install it with 'winget install usbipd' and try again."
}

# Require elevation - bind fails without it.
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error "This script must be run from an elevated (Administrator) PowerShell."
}

foreach ($hwid in $HardwareIds) {
    Write-Host "[usbip-bind] binding $hwid ..."
    & usbipd bind --hardware-id $hwid
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "bind failed for $hwid (is the device plugged in? run 'usbipd list' to check)."
    }
}

Write-Host "[usbip-bind] done. Current state:"
& usbipd list
