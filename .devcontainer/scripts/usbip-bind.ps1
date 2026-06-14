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
    # Hardware IDs (VID:PID) to bind. RGB Sunglasses by default; add '1366:0101' for the
    # SEGGER J-Link debug probe.
    [string[]] $HardwareIds = @('2fe3:0001')
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
