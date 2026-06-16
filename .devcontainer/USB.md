# USB device access in the dev container (serial ports + mass storage)

The sunglasses board (`fw/`, USB ID `2fe3:0001`) exposes several **CDC-ACM serial ports**
and a **USB mass-storage** volume. This explains how they reach the dev container and what
the one-time setup is.

## TL;DR

1. **Once**, in an **elevated** Windows PowerShell:

   ```powershell
   .\.devcontainer\scripts\usbip-bind.ps1
   ```

   This binds both the board (`2fe3:0001`) and the SEGGER J-Link (`1366:0101`); a device
   that isn't plugged in is just skipped. (Equivalently:
   `usbipd bind --hardware-id 2fe3:0001` and `usbipd bind --hardware-id 1366:0101`.)

2. Open the repo in **Windows VS Code** → **Reopen in Container**. Everything else is
   automatic. After it starts you should have `/dev/ttyACM*` and a new `/dev/sd*` disk.

That's it. Step 1 is persistent across reboots, so you normally only ever do step 2.

## How it works

```
Windows ──usbipd──▶ WSL2 kernel (shared) ──▶ Docker Desktop container (--privileged)
```

- The container has **no kernel of its own** — it shares the WSL2 kernel
  (`*-microsoft-standard-WSL2`). It also has no `/lib/modules`, so it can't run `modprobe`
  itself.
- The WSL2 kernel already **ships** the needed drivers as loadable modules
  (`CONFIG_USB_ACM=m`, `CONFIG_USB_STORAGE=m`) — they were just never loaded, so nothing
  bound the device's serial/storage interfaces.
- The dev container's **`initializeCommand`** (`.devcontainer/scripts/host-usb-init.ps1`)
  runs on the Windows host every time the container starts and:
  1. starts a hidden `usbipd attach --auto-attach` for the device (attaches now, and
     re-attaches automatically after a replug);
  2. runs `modprobe -a cdc-acm usb-storage vhci-hcd usbip-host` inside the `docker-desktop`
     WSL2 distro, loading the drivers into the shared kernel. (`modprobe` is a kernel call
     and doesn't write to the filesystem, so the read-only restriction doesn't apply.)
- Because the container runs `--privileged`, the resulting `/dev/ttyACM*` and `/dev/sd*`
  nodes (created in the shared devtmpfs) are visible inside it.

> The only thing that can't be automated is the initial `usbipd bind`: Windows requires
> administrator rights to share a USB device. It's persistent, so it's truly one-time.

## The J-Link debug probe

The SEGGER J-Link (`1366:0101`) is forwarded the same way and is included by default in both
scripts, so `usbipd-bind.ps1` binds it and the container auto-attaches it whenever it's
connected. This makes the probe available to the nRF Connect / J-Link tooling **inside the
container** for flashing and debugging (it won't be visible to Windows-side tools while
attached). If you don't have a J-Link, nothing happens — it's just skipped.

To forward only the board and leave the J-Link on Windows, pass an explicit list, e.g.
`usbipd bind --hardware-id 2fe3:0001` and run the init script with
`-HardwareIds 2fe3:0001`.

## Verify (inside the container)

```bash
lsusb | grep -i 2fe3          # device present:  ... NordicSemiconductor RGB Sunglasses
ls -l /dev/ttyACM*            # CDC-ACM serial ports
lsblk ; ls /dev/sd*           # the mass-storage disk
dmesg | tail                  # should show cdc_acm and usb-storage binding
# read the mass-storage volume:
sudo mkdir -p /mnt/dev && sudo mount /dev/sdX1 /mnt/dev   # replace sdX1 with the new disk
```

## Troubleshooting

- **`postStartCommand` says "No /dev/ttyACM* yet".** The most common cause is that the
  device isn't bound — run step 1 (elevated `usbipd bind`). Check the `initializeCommand`
  output in the VS Code *Dev Containers* log for `[usb-init]` warnings.
- **Device unplugged / re-flashed and disappeared.** Auto-attach should re-attach it within
  a second or two. If not, run `usbipd attach --wsl --hardware-id 2fe3:0001` on the host, or
  just rebuild the container.
- **Stuck state after `usbipd` changes.** `wsl --shutdown` on the host, then reopen the
  container — `initializeCommand` re-establishes everything.
- **`modprobe` failed for a module.** A `.ko` is missing from that WSL distro's
  `/lib/modules`. The standard WSL2 kernel ships `cdc-acm`/`usb-storage`, so this is rare;
  if it happens, build the module against the WSL2 kernel source — see
  <https://github.com/rohzb/wsl2-usb-devices>.
## References

- usbipd-win WSL support: <https://github.com/dorssel/usbipd-win/wiki/WSL-support>
- Dev container `initializeCommand`: <https://containers.dev/implementors/json_reference/>
- USB devices in a Docker dev container via usbipd:
  <https://mcuoneclipse.com/2025/10/26/using-windows-usb-devices-and-debug-probes-inside-docker-dev-container/>
