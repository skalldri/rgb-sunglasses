<#
.SYNOPSIS
    One-time setup: persistently load USB kernel modules into the docker-desktop WSL2 distro.

.DESCRIPTION
    The devcontainer's initializeCommand does NOT run inside docker-desktop, so modprobe
    can't be called from there directly. Instead, this script:

      1. Loads the required modules immediately via `wsl -d docker-desktop modprobe`.
      2. Installs a [boot] command in docker-desktop's /etc/wsl.conf so the modules are
         reloaded automatically every time Docker Desktop starts (i.e. on PC reboot or
         after `wsl --shutdown`).

    Run this ONCE from a normal (non-elevated) PowerShell prompt, then reopen the
    devcontainer. You should not need to run it again unless you reinstall Docker Desktop.

.NOTES
    Requires Docker Desktop to be running. The [boot] command feature requires
    WSL 0.67.6+ (available on Windows 11 and updated Windows 10 builds).
#>

$Modules = @('cdc-acm', 'usb-storage', 'vhci-hcd', 'usbip-host')
$ModuleStr = $Modules -join ' '
$BootCmd   = "modprobe -a $ModuleStr"

function Write-Info($m) { Write-Host "[wsl-modules] $m" }
function Write-Warn($m) { Write-Host "[wsl-modules] WARNING: $m" -ForegroundColor Yellow }

# --- 1. Confirm docker-desktop distro is present ------------------------------------
$raw = wsl.exe -l -q 2>$null
$distros = @($raw -split "`r?`n" |
    ForEach-Object { ($_ -replace "`0", "").Trim() } |
    Where-Object { $_ })

if (-not ($distros -contains 'docker-desktop')) {
    Write-Warn "docker-desktop WSL2 distro not found. Is Docker Desktop installed and running?"
    exit 1
}

# --- 2. Load modules immediately into the running kernel ----------------------------
Write-Info "Loading modules now: $ModuleStr"
wsl.exe -d docker-desktop -u root -- modprobe -a $Modules
if ($LASTEXITCODE -eq 0) {
    Write-Info "Modules loaded successfully."
} else {
    Write-Warn "modprobe exited $LASTEXITCODE — modules may not have loaded (is Docker Desktop fully started?)."
}

# --- 3. Persist via /etc/wsl.conf [boot] --------------------------------------------
Write-Info "Checking /etc/wsl.conf in docker-desktop..."

# Check for [boot] section
wsl.exe -d docker-desktop -u root -- grep -qsF '[boot]' /etc/wsl.conf 2>$null
$hasBootSection = ($LASTEXITCODE -eq 0)

if ($hasBootSection) {
    # [boot] section exists — check whether our command is already there
    wsl.exe -d docker-desktop -u root -- grep -qsF $BootCmd /etc/wsl.conf 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Info "Boot command already present in /etc/wsl.conf — nothing to do."
    } else {
        Write-Warn "[boot] section exists but doesn't contain our modprobe command."
        Write-Warn "Please manually add the following line under [boot] in docker-desktop's /etc/wsl.conf:"
        Write-Warn "    command=$BootCmd"
    }
} else {
    # No [boot] section — append one
    Write-Info "Appending [boot] command to /etc/wsl.conf..."
    wsl.exe -d docker-desktop -u root -- sh -c "printf '\n[boot]\ncommand=$BootCmd\n' >> /etc/wsl.conf"
    if ($LASTEXITCODE -eq 0) {
        Write-Info "Done. Modules will load automatically on every Docker Desktop start."
        Write-Info "You can verify with: wsl -d docker-desktop cat /etc/wsl.conf"
    } else {
        Write-Warn "Failed to write /etc/wsl.conf (exit $LASTEXITCODE)."
    }
}
