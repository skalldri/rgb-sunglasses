# Serial Console (Zephyr Shell)

How to talk to the board's shell reliably. **Hold the `board` lock first** (root `CLAUDE.md`
"Hardware locking") — the `PreToolUse` hook denies `mcp__serial__*` calls without it.

## Ports

The dev board exposes two USB-CDC-ACM ports. Names shift on every reset/re-enumeration on both
OSes — **always discover them with `/check-hardware`**, never hardcode.

| Role | Linux (devcontainer) | macOS host | Baud |
| --- | --- | --- | --- |
| Zephyr interactive shell (`uart:~$` prompt) | `/dev/ttyACM*`, USB interface x.0 | `/dev/cu.usbmodem*`, lower suffix | 115200 |
| MCUmgr UART transport (firmware updates) | `/dev/ttyACM*`, USB interface x.2 | `/dev/cu.usbmodem*`, higher suffix | 115200 |

A SEGGER J-Link (VID:PID `1366:0101`) may also be connected for advanced operations (reflashing
MCUboot, GDB); see `jlink.md`. **If both ports are missing although the board shows up in `lsusb`**,
the `cdc_acm` kernel module is not loaded in the WSL docker-desktop VM: run
`wsl -d docker-desktop -- modprobe cdc_acm` from Windows, then replug the board.

## Using the `mcp__serial__*` tools

**Always use the `mcp__serial__*` MCP tools to interact with the Zephyr shell.** Never shell out via
Bash to read/write `/dev/ttyACM0` directly (`cat`/`echo` redirects, `screen`, `picocom`) — it races
with the MCP server's background reader thread for the port and produces garbled/lost data.

**Wait for boot before sending commands.** Boot log output interleaves with echoed input and causes
`command not found`. Wait until `uart:~$` appears.

**Sending newlines correctly.** `serial_write` with `data: "\r\n"` sends the four literal characters
`\`, `r`, `\`, `n` — not CR+LF. The same applies to every escape sequence: `"\x03"` sends four literal
characters, NOT Ctrl+C — and those bytes land in the shell's line editor and corrupt the next command
(`command not found` on otherwise-correct input; recover with flush + resend). To send control
characters use the `as: "hex"` form or the `rgb_sunglasses` plugin's commands (which handle Ctrl+C
internally):

```jsonc
// Option 1 — append_newline flag (preferred)
{ "data": "kernel version", "append_newline": true }

// Option 2 — explicit hex
{ "data": "kernel version\r", "as": "hex" }   // hex-encode the CR separately
```

**Graduate working shell interactions into serial MCP plugins.** Once you've figured out how to
reliably drive a shell subsystem over raw `serial_write`/`serial_read_until`, don't keep repeating
that raw sequence in future sessions. Write or extend a plugin under `.serial_mcp/plugins/` (use
`serial_plugin_template` to scaffold, `serial_plugin_load`/`serial_plugin_reload` to pick it up) so
the next interaction is a single typed tool call.

**Serial connection pool limit** — the MCP serial server defaults to 10 concurrent connections.
After several J-Link flashes + reboots, ttyACM ports accumulate and connections are never GC'd; when
you hit the limit, close all stale connections explicitly before opening the new port.

**A board reset breaks an already-open connection.** After a J-Link flash (`jlink-flash.sh` resets
the board) or any other reset, the first write on the old `connection_id` fails with
`[Errno 5] Input/output error`, and `serial_open` on the _same path_ fails with `[Errno 6] No such
device or address` because the board re-enumerated under a new minor number. `serial_close` the
stale id, re-run `/check-hardware` (or the mknod loop below) to find the shell's _new_ port, and open
that. Don't retry the old id or path.

## Animation shell control — the `rgb_sunglasses` serial MCP plugin

The `anim` shell command (`anim get` / `anim set <name>` / `anim indicator clear|get` /
`anim shuffle`, defined in `fw/src/pattern_controller.cpp`) and friends are exposed as a serial MCP
plugin at `.serial_mcp/plugins/rgb_sunglasses.py` — prefer it over hand-rolled
`serial_write`/`serial_read_until`. It requires `SERIAL_MCP_PLUGINS=rgb_sunglasses` in `.mcp.json`'s
`serial` server env (already set); reconnect via `/mcp` after enabling. As other shell subsystems get
plugin coverage, add their tools to this file (or a new one) and update `SERIAL_MCP_PLUGINS`.

Tools: `rgb_sunglasses.get_animation`, `rgb_sunglasses.set_animation` (name one of
`SETTABLE_ANIMATIONS` in the plugin: `none, zigzag, text, rainbow, my_eyes, beat, fft_bars,
glim_player, matrix_code, tilt, pulse`), `rgb_sunglasses.clear_indicator`,
`rgb_sunglasses.glim_list`, `rgb_sunglasses.glim_select`, `rgb_sunglasses.glim_set_loop_mode`, and
the audio tools `rgb_sunglasses.sound_record` (freeze AGC gain → `sound mic record_wav` → parsed result
incl. dropped-frame count), `rgb_sunglasses.sound_dump` (capture N frames of live analysis to a host
file) and `rgb_sunglasses.capture_scenario` — **the three audio tools need a `CONFIG_APP_AUDIO_DEBUG`
build** (`.claude/rules/fw-sound-capture.md`, `fw/docs/beat-detection-debugging.md`); for plain IMU
captures use the shell `capture start` instead.

**Always clear the active BT indicator before starting an animation.** A BT indicator
(advertising/connecting/pairing overlay) overrides whatever animation is set and hides it.
`rgb_sunglasses.set_animation` does this automatically (calls `clear_indicator` before `anim set` and
verifies via `anim get`) — don't bypass it by calling the shell directly.

## Shell quirks the plugin works around

**Zephyr shell prompt redraw quirk:** the shell redraws `uart:~$` after _every_ async log line (BT
notifications, GLIM decoder logs, …), not just after a command finishes. A naive
`read_until("uart:~$ ")` can match a stale redraw left over from a previous command's delayed
logging, before the current command's own echo has even arrived — this caused a real false-failure.
`_run_command` in the plugin sends Ctrl+C, flushes the input buffer, writes, then accumulates
`read_until` chunks until the command's own echo is followed by a prompt.

**Stray input right after a board reset:** a boot-log fragment (e.g. `rf: Preinit`) can land in the
shell's line editor before the first command is sent, corrupting it (`command not found` on the first
call after reset). That is why `_run_command` sends Ctrl+C before every command — cheap and general.

**TPS25750 log fires at ~10 ms after boot** — the USB PD controller always logs
`tps25750: MODE is not PTCH (got APP) Cannot download patch!` around 10 ms uptime. If the first
command after boot is read with `read_until("uart:~$")`, this log fires first, matches the redraw
prompt, and swallows the command's output. Flush the RX buffer and resend; the second call completes.

**Old boot logs flushing on port-open look like a spontaneous reboot — they aren't.** The USB CDC
shell buffers unread output while no terminal is attached; opening the port can dump a backlog that
starts with `[00:00:00.xxx]` boot logs from a reset minutes earlier. Before concluding the board just
rebooted (or crashed), run `kernel uptime` — a large uptime means you're reading backlog.

## Useful shell commands

```
kernel version          # print Zephyr/NCS version
kernel uptime           # tell a fresh boot from a flushed backlog
kernel thread list      # list all threads and their stack usage
bt_state                # SNAPSHOT of BLE link health: state, peer, security level, ATT MTU, conn
                        # params. ALWAYS RUN THIS FIRST when a BLE connection looks stuck.
bt_conn_info            # the *actual* current LE connection interval/latency/timeout
mcuboot_version         # read MCUboot version from retention registers (major.minor.rev+tweak)
mcuboot_update verify   # read /NAND:/mcuboot.bin, print GRMB header fields, compute and compare CRC
mcuboot_update sideload # open /NAND:/mcuboot.bin and validate it (no BLE upload needed)
mcuboot_update commit   # flash validated package to internal MCUboot region and reboot
mcuboot_update request_reboot  # set gpregret2=BOOT_MODE_REQ and reboot (MCUboot skips fprotect)
mcuboot_update status | abort  # updater state / abandon a staged update
fatfs reformat          # nuke and recreate the NAND FAT filesystem (all files erased)
flashdisk stats         # per-disk flash-op error counters (.claude/rules/fw-storage-usb.md)
coredump_mgr status     # pending/collected coredumps (.claude/rules/fw-coredump.md)
ext list | faults       # extension slots / latched faults (.claude/rules/fw-extensions.md)
glim list               # GLIM files found at boot
power bq status         # charger readback — power writes are DANGER (.claude/rules/fw-power.md)
```

**Debugging a stuck BLE connection ("split-brain") — run `bt_state` FIRST.** Classic symptom: the
board's status LED solid (not breathing) while the app reports a connection failure/timeout.
`Security level: L1 (UNENCRYPTED)` on a connection up more than a second → LE Secure Connections
pairing stalled (often a passkey dialog waiting on the phone, `/re-pair`). `ATT MTU: 23 (DEFAULT - MTU
exchange did not complete)` on a `CONNECTED` + `L4` link → **the split-brain**: the phone's GATT stack
is wedged. Whether the phone auto-recovers is device-dependent — full playbook in `/debug-ble`
(`references/stale-gatt-cache.md`) and per-phone behaviour in
`.claude/skills/drive-app/references/phones.md`. Resetting the *board* (`kernel reboot warm`) clears
the board's half so it advertises again, but does not fix the phone's stale cache.

## ttyACM node numbering shifts (devcontainer / WSL2 udev)

After every reboot or J-Link flash the device re-enumerates and Linux assigns the next free ACM minor
numbers — if the old ttyACM0 node wasn't cleaned up, the new device gets ttyACM1/ttyACM2. WSL2's udev
also sometimes fails to create /dev nodes for new ACM interfaces that do appear in sysfs.
`/check-hardware` (and `fw/scripts/fix-usb-dev-nodes.sh`) repairs this; by hand:

```bash
# Create any missing /dev nodes (verify a stale node is current by comparing
# `cat /sys/class/tty/ttyACMN/dev` against `ls -la /dev/ttyACMN`)
for d in /sys/class/tty/ttyACM*; do
    n=$(basename $d)
    maj_min=$(cat $d/dev)
    maj=${maj_min%:*}; min=${maj_min#*:}
    [ -e /dev/$n ] || mknod /dev/$n c $maj $min && chmod 666 /dev/$n
done
```

Then probe each new port with Ctrl+C to find the shell (look for `uart:~$`); the MCUmgr port is
whichever answers `mcumgr ... echo` (`mcumgr.md`).

## Recovering a wedged shell UART without reflashing

If the shell UART stops accepting writes (host-side `serial.write` times out) while the MCUmgr CDC
interface still works, the shell thread is likely wedged. This is a firmware hang, not a USB/WSL2
dropout (confirm via `lsusb | grep 2fe3` — the device still enumerates). The original trigger — a
`sensor stream <dev> on ...` left running on a `hw-flow-control` port — cannot recur as written on
proto0 (`CONFIG_SENSOR_SHELL=n`, and `hw-flow-control` is only on the MCUmgr port), but the recovery
is the same for any wedge. Don't reach for a full `jlink-flash.sh` reflash — reset the target CPU over
the J-Link's SWD connection:

```bash
nrfutil device reset --serial-number <jlink-serial> --reset-kind RESET_PIN
```

(`<jlink-serial>` is the S/N `/check-hardware` and `jlink-flash.sh` print, e.g. `50104975`.) This
resets the target without touching flash. The board re-enumerates afterward like any reset — poll
`lsusb`/`/dev/ttyACM*` before issuing further commands.
