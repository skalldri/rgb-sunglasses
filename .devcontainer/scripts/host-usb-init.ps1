<#
.SYNOPSIS
    Auto-attach the forwarded USB device(s) into WSL2 and load the kernel modules
    needed for CDC-ACM serial ports and USB mass storage.

.DESCRIPTION
    Run automatically on the Windows host by the dev container's `initializeCommand`
    (see ../devcontainer.json) every time the container is created or started.

    Why this runs on the host: the dev container shares the WSL2 kernel and cannot load
    kernel modules itself (Docker Desktop's container host ships no /lib/modules). The
    usbipd attach and the `modprobe` therefore have to happen on the WSL2/Windows side.

    For each target device this script:
      1. Confirms usbipd-win is installed and the device is *bound* (shared). Binding
         needs a one-time elevated `usbipd bind` (see usbip-bind.ps1) and is persistent.
      2. Ensures a hidden `usbipd attach --auto-attach` is running so the device is
         attached now and re-attached automatically after a replug.
      3. Loads cdc-acm / usb-storage (and the usbip modules) into the shared WSL2 kernel
         by running `modprobe` inside a real WSL2 distro that ships the matching .ko files.

    The script is deliberately tolerant: any problem is reported as a warning and it exits
    0 so the container still opens.

.NOTES
    Module names use the on-disk .ko names (cdc-acm, usb-storage); modprobe resolves the
    cdc_acm / usb_storage aliases. These ship with the stock WSL2 kernel - no build needed.
#>
[CmdletBinding()]
param(
    # Hardware IDs (VID:PID) to attach. RGB Sunglasses by default; add '1366:0101' to also
    # forward the SEGGER J-Link debug probe.
    [string[]] $HardwareIds = @('2fe3:0001'),

    # Kernel modules to load into the shared WSL2 kernel.
    [string[]] $Modules = @('cdc-acm', 'usb-storage', 'vhci-hcd', 'usbip-host')
)

$ErrorActionPreference = 'Continue'

function Write-Info($m) { Write-Host "[usb-init] $m" }
function Write-Warn($m) { Write-Host "[usb-init] WARNING: $m" -ForegroundColor Yellow }

# --- 1. usbipd present? -------------------------------------------------------------
$usbipd = Get-Command usbipd -ErrorAction SilentlyContinue
if (-not $usbipd) {
    Write-Warn "usbipd-win not found on PATH. Install it once with 'winget install usbipd'"
    Write-Warn "(https://github.com/dorssel/usbipd-win), then reopen the container. Skipping USB setup."
    exit 0
}

# Snapshot of attached/shared state. `usbipd list` lines look like:
#   BUSID  VID:PID    DEVICE                         STATE
#   1-4    2fe3:0001  USB Serial Device, USB Mass..  Shared
$listText = (& usbipd list 2>$null) -join "`n"

function Get-DeviceState([string] $hwid) {
    # Returns 'Attached' | 'Shared' | 'Not shared' | $null (not present).
    foreach ($line in ($listText -split "`n")) {
        if ($line -match [regex]::Escape($hwid)) {
            if ($line -match 'Attached') { return 'Attached' }
            if ($line -match 'Not shared') { return 'Not shared' }
            if ($line -match 'Shared') { return 'Shared' }
        }
    }
    return $null
}

function Test-AutoAttachRunning([string] $hwid) {
    $procs = Get-CimInstance Win32_Process -Filter "Name='usbipd.exe'" -ErrorAction SilentlyContinue
    foreach ($p in $procs) {
        if ($p.CommandLine -and $p.CommandLine -match 'auto-attach' -and $p.CommandLine -match [regex]::Escape($hwid)) {
            return $true
        }
    }
    return $false
}

# --- 2. Attach each target device --------------------------------------------------
$anyShared = $false
foreach ($hwid in $HardwareIds) {
    $state = Get-DeviceState $hwid
    if ($null -eq $state) {
        Write-Warn "Device $hwid not detected by usbipd (is it plugged in?). Skipping."
    }
    elseif ($state -eq 'Not shared') {
        Write-Warn "Device $hwid is not bound. Run ONCE in an elevated PowerShell:"
        Write-Warn "    usbipd bind --hardware-id $hwid"
        Write-Warn "(or run .devcontainer/scripts/usbip-bind.ps1 as admin), then reopen the container."
    }
    else {
        # 'Shared' or 'Attached': make sure the auto-attach watcher is running.
        $anyShared = $true
        if (Test-AutoAttachRunning $hwid) {
            Write-Info "auto-attach already running for $hwid"
        }
        else {
            Start-Process -FilePath $usbipd.Source `
                -ArgumentList @('attach', '--wsl', '--auto-attach', '--hardware-id', $hwid) `
                -WindowStyle Hidden | Out-Null
            Write-Info "started auto-attach for $hwid (current state: $state)"
        }
    }
}

if (-not $anyShared) {
    Write-Warn "No bound device to attach; skipping module load. See messages above."
    exit 0
}

# Give the auto-attach watcher a moment to attach before we load the drivers.
Start-Sleep -Seconds 2

# --- 3. Load the kernel modules into the shared WSL2 kernel -------------------------
# Pick a real WSL2 distro (not Docker Desktop's internal ones); it ships the .ko files
# for the running kernel, and the load is global because all distros share one kernel.
$raw = & wsl.exe -l -q 2>$null
$distros = @($raw -split "`r?`n" | ForEach-Object { ($_ -replace "`0", '').Trim() } |
    Where-Object { $_ -and $_ -notmatch '^docker-desktop' })
$distro = $distros | Select-Object -First 1

if (-not $distro) {
    Write-Warn "No general-purpose WSL2 distro found to load modules (only docker-desktop)."
    Write-Warn "Install one (e.g. 'wsl --install -d Ubuntu') so the cdc-acm/usb-storage modules can be loaded."
    exit 0
}

Write-Info "loading modules ($($Modules -join ', ')) via WSL distro '$distro'..."
& wsl.exe -d $distro -u root -- modprobe -a $Modules
if ($LASTEXITCODE -eq 0) {
    Write-Info "modules loaded. /dev/ttyACM* and the mass-storage disk should now appear in the container."
}
else {
    Write-Warn "modprobe exited with code $LASTEXITCODE in '$distro'. A module may be missing from this"
    Write-Warn "distro's /lib/modules. See .devcontainer/USB.md (troubleshooting) for the build fallback."
}

exit 0
