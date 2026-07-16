<#
.SYNOPSIS
    Attach USB devices into WSL2 via usbipd and load kernel modules.
    Runs interactively — press Ctrl-C to detach devices and exit.

.DESCRIPTION
    Run once on Windows boot and leave running while developing.

    On startup this script:
      1. Confirms usbipd-win is installed and the devices are bound (shared).
      2. Loads cdc-acm and related modules into the shared WSL2 kernel via modprobe.
      3. Starts `usbipd attach --auto-attach` for each device as a tracked background
         process, so devices are re-attached automatically after a replug.
      4. Blocks until you press Ctrl-C.

    On Ctrl-C:
      - Runs `usbipd detach` for each attached device.
      - Kills the background auto-attach watchers.

    One-time prerequisite: run usbip-bind.ps1 as admin to share (bind) each device.
    That step is persistent and does not need to be repeated on reboot.

.NOTES
    Module names use the on-disk .ko names (cdc-acm, usb-storage); modprobe resolves the
    cdc_acm / usb_storage aliases. These ship with the stock WSL2 kernel - no build needed.
#>
[CmdletBinding()]
param(
    # Hardware IDs (VID:PID) to attach:
    #   2fe3:0001  board running the app firmware (normal runtime)
    #   2fe3:0100  board in MCUboot serial-recovery / DFU mode (different PID — must be
    #              forwarded separately or recovery flashing is invisible in the container)
    #   1366:0101  SEGGER J-Link debug probe
    [string[]] $HardwareIds = @('2fe3:0001', '2fe3:0100', '1366:0101'),

    # Kernel modules to load into the shared WSL2 kernel.
    [string[]] $Modules = @('cdc-acm', 'usb-storage', 'vhci-hcd', 'usbip-host')
)

$ErrorActionPreference = 'Continue'

function Write-Info($m) { Write-Host "[usb-init] $m" }
function Write-Warn($m) { Write-Host "[usb-init] WARNING: $m" -ForegroundColor Yellow }

# --- 1. usbipd present? ---------------------------------------------------------------
$usbipd = Get-Command usbipd -ErrorAction SilentlyContinue
if (-not $usbipd) {
    Write-Warn "usbipd-win not found on PATH. Install it with: winget install usbipd"
    exit 1
}

# --- 2. Kill any stale auto-attach processes from a previous run ----------------------
$stale = @(Get-CimInstance Win32_Process -Filter "Name='usbipd.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match 'auto-attach' })
if ($stale.Count -gt 0) {
    Write-Info "Stopping $($stale.Count) stale auto-attach process(es) from a previous run..."
    $stale | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 1
}

# --- 3. Load kernel modules into the shared WSL2 kernel ------------------------------
# modprobe is a kernel call — it doesn't write to docker-desktop's filesystem, so the
# read-only restriction doesn't apply. All WSL2 distros share one kernel, so loading
# here affects the container too.
Write-Info "Loading modules ($($Modules -join ', '))..."
& wsl.exe -d docker-desktop -u root -- modprobe -a $Modules
if ($LASTEXITCODE -eq 0) {
    Write-Info "Modules loaded. /dev/ttyACM* and mass-storage will appear once devices attach."
} else {
    Write-Warn "modprobe exited $LASTEXITCODE. /dev/ttyACM* may not appear."
    Write-Warn "See .devcontainer/USB.md (troubleshooting) for the build fallback."
}

# --- 4. Snapshot device state and start auto-attach watchers -------------------------
$listText = (& usbipd list 2>$null) -join "`n"

function Get-DeviceState([string] $hwid) {
    foreach ($line in ($listText -split "`n")) {
        if ($line -match [regex]::Escape($hwid)) {
            if ($line -match 'Attached')  { return 'Attached' }
            if ($line -match 'Not shared') { return 'Not shared' }
            if ($line -match 'Shared')    { return 'Shared' }
        }
    }
    return $null
}

$attachedIds = [System.Collections.Generic.List[string]]::new()
$procs       = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()

foreach ($hwid in $HardwareIds) {
    $state = Get-DeviceState $hwid
    if ($null -eq $state) {
        Write-Warn "Device $hwid not detected by usbipd (is it plugged in?). Skipping."
    } elseif ($state -eq 'Not shared') {
        Write-Warn "Device $hwid is not bound. Run ONCE in an elevated PowerShell:"
        Write-Warn "    usbipd bind --hardware-id $hwid"
        Write-Warn "(or run .devcontainer/scripts/usbip-bind.ps1 as admin)"
    } else {
        $proc = Start-Process -FilePath $usbipd.Source `
            -ArgumentList @('attach', '--wsl', '--auto-attach', '--hardware-id', $hwid) `
            -WindowStyle Hidden -PassThru
        $procs.Add($proc)
        $attachedIds.Add($hwid)
        Write-Info "Started auto-attach for $hwid (PID $($proc.Id), current state: $state)"
    }
}

if ($procs.Count -eq 0) {
    Write-Warn "No devices were started. Check the warnings above."
    exit 1
}

Write-Info "USB ready. Press Ctrl-C to detach devices and exit."

# --- 5. Block until Ctrl-C, then detach and clean up ---------------------------------
try {
    while ($true) {
        Start-Sleep -Seconds 5
        foreach ($proc in $procs) {
            if ($proc.HasExited) {
                Write-Warn "auto-attach PID $($proc.Id) exited unexpectedly (code $($proc.ExitCode))."
            }
        }
    }
} finally {
    Write-Info ""
    Write-Info "Detaching USB devices..."
    foreach ($hwid in $attachedIds) {
        & usbipd detach --hardware-id $hwid 2>$null
        Write-Info "  Detached $hwid"
    }
    foreach ($proc in $procs) {
        if (-not $proc.HasExited) {
            $proc.Kill()
        }
    }
    Write-Info "Done."
}
