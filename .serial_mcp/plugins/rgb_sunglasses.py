"""Plugin for the RGB Sunglasses dev board's `anim` shell command.

Wraps the Zephyr shell's `anim get` / `anim set <name>` / `anim indicator clear`
commands (see fw/src/pattern_controller.cpp) so callers don't have to hand-roll
serial_write/serial_read_until sequences and ANSI/echo parsing.

IMPORTANT: a BT indicator (advertising/connecting/pairing) overlays the active
animation and overrides what's actually rendered on the LEDs. set_animation
ALWAYS clears the indicator first — see fw/CLAUDE.md "Animation shell control"
for why this is required, not optional.
"""

import re

from mcp.types import Tool

from serial_mcp_server.helpers import _ok, _err  # _ok(key=val) / _err("code", "message")
from serial_mcp_server.state import SerialState

# Import core handlers to interact with the device.
# IMPORTANT: always use these instead of conn.ser directly — a background
# thread owns the serial port for reads, and writes need a lock.
from serial_mcp_server.handlers_serial import (
    handle_write,      # send data to the device
    handle_read_until, # read until a delimiter string
    handle_flush,      # discard stale buffered bytes before a new command
)

METATA_DESCRIPTION = (
    "RGB Sunglasses dev board — Zephyr shell `anim` command wrapper "
    "(get/set animation, clear BT indicator)."
)

META = {
    "description": METATA_DESCRIPTION,
    "device_name_contains": "rgb_sunglasses",
}

PROMPT = "uart:~$ "
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

# Names accepted by `anim set <name>` (see SHELL_SUBCMD_DICT_SET_CREATE in
# fw/src/pattern_controller.cpp). bt_advertising/bt_connecting/bt_pairing are
# indicator-only states and are not settable directly.
SETTABLE_ANIMATIONS = [
    "none", "zigzag", "text", "rainbow", "my_eyes", "beat", "fft_bars",
    "glim_player", "matrix_code", "tilt", "pulse",
]

TOOLS = [
    Tool(
        name="rgb_sunglasses.clear_indicator",
        description=(
            "Clear the active BT indicator (advertising/connecting/pairing overlay) "
            "and return the display to whatever animation is currently set."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "connection_id": {"type": "string"},
            },
            "required": ["connection_id"],
        },
    ),
    Tool(
        name="rgb_sunglasses.set_animation",
        description=(
            "Switch the dev board to the named animation. Always clears any active "
            "BT indicator first (a pairing/advertising/connecting overlay would "
            "otherwise hide the animation), then sets and verifies via `anim get`."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "connection_id": {"type": "string"},
                "name": {"type": "string", "enum": SETTABLE_ANIMATIONS},
            },
            "required": ["connection_id", "name"],
        },
    ),
    Tool(
        name="rgb_sunglasses.get_animation",
        description="Print the currently active animation/indicator name (`anim get`).",
        inputSchema={
            "type": "object",
            "properties": {
                "connection_id": {"type": "string"},
            },
            "required": ["connection_id"],
        },
    ),
    Tool(
        name="rgb_sunglasses.glim_list",
        description=(
            "List the .glim files discovered under /NAND:/glim by the Glim Player "
            "animation (`glim list`), marking the currently selected one."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "connection_id": {"type": "string"},
            },
            "required": ["connection_id"],
        },
    ),
    Tool(
        name="rgb_sunglasses.glim_select",
        description="Select a Glim Player file by its registry index (`glim select <index>`).",
        inputSchema={
            "type": "object",
            "properties": {
                "connection_id": {"type": "string"},
                "index": {"type": "integer", "minimum": 0},
            },
            "required": ["connection_id", "index"],
        },
    ),
    Tool(
        name="rgb_sunglasses.sound_record",
        description=(
            "Record mic audio + per-frame beat analysis on the dev board "
            "(`sound mic record_wav`): freezes AGC gain at the given register value "
            "first so the capture is replay-comparable, then records duration_s "
            "seconds to /NAND:/sound.wav (+ .csv sidecar with per-frame analysis). "
            "Pull the files off the USB mass-storage disk afterwards; analyze with "
            "fw/tools/beat_lab (see fw/docs/beat-detection-debugging.md)."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "connection_id": {"type": "string"},
                "duration_s": {"type": "integer", "minimum": 1, "maximum": 120},
                "gain": {
                    "type": "string",
                    "description": "PDM gain register value to freeze at, e.g. '0x28' (0 dB)",
                },
                "path": {"type": "string", "description": "output path (default /NAND:/sound.wav)"},
            },
            "required": ["connection_id", "duration_s"],
        },
    ),
    Tool(
        name="rgb_sunglasses.sound_dump",
        description=(
            "Stream N frames of live beat-detection analysis off the dev board "
            "(`sound dump`) and save the capture (D-line format, see "
            "fw/tools/beat_lab/frames.py) to a host file. ~31 frames/second."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "connection_id": {"type": "string"},
                "frames": {"type": "integer", "minimum": 1, "maximum": 10000},
                "output_path": {"type": "string",
                                "description": "host file to write the capture to"},
                "buckets": {"type": "boolean",
                            "description": "include the 20 display-bucket energies"},
            },
            "required": ["connection_id", "frames", "output_path"],
        },
    ),
    Tool(
        name="rgb_sunglasses.capture_scenario",
        description=(
            "Record a REAL audio + IMU capture for use as a simulator scenario "
            "(`sound mic record_wav`, which also writes a synchronised .imu.csv "
            "sidecar; on a build with CONFIG_APP_CAPTURE_AUDIO_SIDECAR the same file also "
            "carries a per-frame D-row of the analysis the DSP computed for those "
            "samples). All streams are timestamped from the same t0 by the one "
            "capture loop, so they need no host-side alignment. Freezes AGC gain "
            "during the capture so the stimulus is reproducible. Afterwards, pull "
            "the .wav and its sidecars off the USB mass-storage disk and run "
            "fw/tools/capture_to_scenario.py to emit the scenario JSON. Full "
            "procedure: the /capture-scenario skill."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "connection_id": {"type": "string"},
                "duration_s": {"type": "integer", "minimum": 1, "maximum": 120},
                "name": {
                    "type": "string",
                    "description": (
                        "capture name; files land at "
                        "/NAND:/<name>.wav (+ a combined .csv sidecar)"
                    ),
                },
                "gain": {
                    "type": "string",
                    "description": "PDM gain register to freeze at, e.g. '0x28' (0 dB)",
                },
            },
            "required": ["connection_id", "duration_s", "name"],
        },
    ),
    Tool(
        name="rgb_sunglasses.glim_set_loop_mode",
        description=(
            "Set the Glim Player's loop mode (`glim set_loop_mode <mode>`): loop_one "
            "(replay the selected file), play_all (advance through all files), or "
            "stop_after_one (freeze on the last frame)."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "connection_id": {"type": "string"},
                "mode": {"type": "string", "enum": ["loop_one", "play_all", "stop_after_one"]},
            },
            "required": ["connection_id", "mode"],
        },
    ),
]


def _clean_response(text_after_echo: str) -> str:
    """Strip ANSI escapes and the trailing prompt from text following the echo."""
    text = _ANSI_RE.sub("", text_after_echo).replace("\r", "")
    lines = [ln for ln in text.split("\n")]
    while lines and lines[-1].strip() in ("", PROMPT.strip()):
        lines.pop()
    return "\n".join(ln for ln in lines if ln.strip())


async def _run_command(state: SerialState, connection_id: str, cmd: str,
                        timeout_ms: int = 3000, max_rounds: int = 6) -> str:
    """Send a shell command and return its cleaned output (without echo/prompt).

    The Zephyr shell redraws the `uart:~$` prompt after every async log line
    (BT notifications, GLIM decoder logs, ...), not just after a command
    finishes. A single read_until(prompt) can therefore match a stale
    prompt-redraw left over from a *previous* command's delayed logging,
    before this command's own echo has even arrived. To avoid that:
      1. Send Ctrl+C to cancel any text already sitting in the shell's own
         input line editor (e.g. a stray boot-log fragment like "rf: Preinit"
         that lands there right after a reset, before we ever write anything)
         and flush the host-side input buffer, so old noise is gone on both
         ends.
      2. Accumulate read_until(prompt) chunks until we find our own echoed
         command followed by a prompt — that's guaranteed to be our response,
         not a stray redraw.
    """
    await handle_write(state, {"connection_id": connection_id, "data": "03", "as": "hex"})
    await handle_flush(state, {"connection_id": connection_id, "what": "input"})
    await handle_write(state, {
        "connection_id": connection_id,
        "data": cmd,
        "append_newline": True,
    })

    accumulated = ""
    for _ in range(max_rounds):
        resp = await handle_read_until(state, {
            "connection_id": connection_id,
            "delimiter": PROMPT,
            "timeout_ms": timeout_ms,
        })
        chunk = resp.get("data", "") if isinstance(resp, dict) else ""
        accumulated += chunk
        plain = _ANSI_RE.sub("", accumulated).replace("\r", "")
        idx = plain.find(cmd)
        if idx != -1:
            after_echo = plain[idx + len(cmd):]
            if PROMPT.strip() in after_echo:
                return _clean_response(after_echo)
        if not chunk:
            break  # device went quiet — no point spinning further

    # Best effort: we never confirmed our own echo. Return what we have so
    # callers can still see useful diagnostic text instead of an empty string.
    return _clean_response(accumulated)


async def handle_clear_indicator(state: SerialState, args: dict) -> dict:
    connection_id = args["connection_id"]
    output = await _run_command(state, connection_id, "anim indicator clear")
    if "error" in output.lower() or "not found" in output.lower():
        return _err("indicator_clear_failed", output or "unknown error")
    return _ok(cleared=True, output=output)


async def handle_set_animation(state: SerialState, args: dict) -> dict:
    connection_id = args["connection_id"]
    name = args["name"]

    if name not in SETTABLE_ANIMATIONS:
        return _err("invalid_animation", f"{name!r} is not settable. Valid: {SETTABLE_ANIMATIONS}")

    # Always clear the indicator first — a BT overlay would otherwise mask the
    # animation we're about to switch to.
    clear_result = await handle_clear_indicator(state, {"connection_id": connection_id})
    if not clear_result.get("ok", True) and "error" in clear_result:
        return _err("indicator_clear_failed", f"Could not clear indicator before set_animation: {clear_result}")

    output = await _run_command(state, connection_id, f"anim set {name}")
    if "error" in output.lower() or "not found" in output.lower():
        return _err("set_animation_failed", output or "unknown error")

    # Verify via `anim get` rather than trusting a silent success.
    current = await _run_command(state, connection_id, "anim get")
    if current.strip() != name:
        return _err(
            "set_animation_unverified",
            f"Sent 'anim set {name}' but 'anim get' reports {current.strip()!r}",
        )

    return _ok(animation=current.strip())


async def handle_get_animation(state: SerialState, args: dict) -> dict:
    connection_id = args["connection_id"]
    output = await _run_command(state, connection_id, "anim get")
    return _ok(animation=output.strip())


async def handle_glim_list(state: SerialState, args: dict) -> dict:
    connection_id = args["connection_id"]
    output = await _run_command(state, connection_id, "glim list")
    return _ok(files=output)


async def handle_glim_select(state: SerialState, args: dict) -> dict:
    connection_id = args["connection_id"]
    index = args["index"]
    output = await _run_command(state, connection_id, f"glim select {index}")
    if "error" in output.lower() or "invalid" in output.lower():
        return _err("glim_select_failed", output or "unknown error")
    return _ok(selected=index)


async def handle_glim_set_loop_mode(state: SerialState, args: dict) -> dict:
    connection_id = args["connection_id"]
    mode = args["mode"]
    output = await _run_command(state, connection_id, f"glim set_loop_mode {mode}")
    if "error" in output.lower() or "unknown" in output.lower():
        return _err("glim_set_loop_mode_failed", output or "unknown error")
    return _ok(loop_mode=mode)


async def handle_sound_record(state: SerialState, args: dict) -> dict:
    connection_id = args["connection_id"]
    duration_s = args["duration_s"]
    gain = args.get("gain", "0x28")
    path = args.get("path", "/NAND:/sound.wav")

    # Freeze the gain first — an AGC step mid-recording would make the capture
    # unusable for device-vs-host replay comparison (see beat-detection-debugging.md).
    freeze_out = await _run_command(state, connection_id, f"sound agc gain {gain}")
    if "set to" not in freeze_out:
        return _err("agc_gain_failed", freeze_out or "no response to 'sound agc gain'")

    async def _restore_agc() -> bool:
        # 'sound agc gain' force-froze the AGC; leaving it frozen would silently
        # kill gain adaptation (and beat-reactive animations) for the rest of the
        # board's uptime. Always unfreeze, on success and failure paths alike.
        out = await _run_command(state, connection_id, "sound agc freeze off")
        return "off" in out

    # The recording itself blocks the shell for duration_s; wait generously.
    output = await _run_command(
        state, connection_id, f"sound mic record_wav {duration_s} {path}",
        timeout_ms=(duration_s + 10) * 1000, max_rounds=10,
    )
    if "Wrote" not in output:
        restored = await _restore_agc()
        # An aborted capture prints "ABORTED: capture incomplete - N of M frames".
        return _err("record_failed",
                    (output[-500:] if output else "no response from record_wav")
                    + f" (agc_restored={restored})")

    restored = await _restore_agc()
    m = re.search(r"Wrote (\d+) bytes of PCM to (\S+) \((\d+) frames, (\d+) dropped,"
                  r" (\d+) io retries\)", output)
    if not m:
        return _ok(raw=output, agc_restored=restored)
    return _ok(bytes=int(m.group(1)), wav_path=m.group(2), csv_path=m.group(2) + ".csv",
               frames=int(m.group(3)), dropped=int(m.group(4)), io_retries=int(m.group(5)),
               contiguous=(int(m.group(4)) == 0), agc_restored=restored)


async def handle_capture_scenario(state: SerialState, args: dict) -> dict:
    """Record audio + IMU together for a sim scenario.

    Deliberately reuses record_wav rather than adding a second capture command:
    the Zephyr shell is single-threaded, so two concurrent recordings are
    impossible, and one loop writing both files is what gives them a shared t0.
    """
    connection_id = args["connection_id"]
    duration_s = args["duration_s"]
    name = args["name"]
    gain = args.get("gain", "0x28")
    path = f"/NAND:/{name}.wav"

    freeze_out = await _run_command(state, connection_id, f"sound agc gain {gain}")
    if "set to" not in freeze_out:
        return _err("agc_gain_failed", freeze_out or "no response to 'sound agc gain'")

    output = await _run_command(
        state, connection_id, f"sound mic record_wav {duration_s} {path}",
        timeout_ms=(duration_s + 15) * 1000, max_rounds=12,
    )
    # Always unfreeze: leaving the AGC pinned would silently kill gain adaptation
    # (and every beat-reactive animation) for the rest of the board's uptime.
    restored = "off" in await _run_command(state, connection_id, "sound agc freeze off")

    if "Wrote" not in output:
        return _err("record_failed",
                    (output[-500:] if output else "no response from record_wav")
                    + f" (agc_restored={restored})")

    imu = re.search(r"IMU sidecar: (\d+) samples", output)
    # This is a SUCCESS-only signal, and it is a `search`, so it matches a prefix
    # anywhere in the output. audio_sidecar_close() in fw/src/sound/sound.cpp
    # relies on that: its failure paths ("Audio sidecar MISALIGNED: ...",
    # "Audio sidecar write failed - ...") are deliberately worded so they cannot
    # match here, because a hit makes every downstream consumer
    # (capture_to_scenario.py, beat_lab) ingest the file. If you loosen this
    # pattern — e.g. to `Audio sidecar[: ]+(\d+)` — re-check those strings first.
    # Two firmware paths write the analysis CSV, and they announce it
    # differently, so both signals are needed:
    #   record_wav_capture()  -> "Audio sidecar: N frames[, M IMU samples]"
    #   record_wav_tap()      -> "Wrote ... (N frames, D dropped, R io retries)"
    # The second is the one that matters here: this handler hard-fails unless
    # `sound agc gain` answers, so it only ever runs against a
    # CONFIG_APP_AUDIO_DEBUG build, where APP_CAPTURE_AUDIO_SIDECAR is
    # unavailable (it depends on !APP_AUDIO_DEBUG) and record_wav_tap() runs.
    # Note "frames" vs "blocks" is the discriminator against the capture path's
    # summary line; record_wav_direct() prints no parenthetical at all and
    # writes no analysis file, so it is correctly excluded by both.
    audio = re.search(r"Audio sidecar: (\d+) frames", output)
    tap = re.search(r"Wrote \d+ bytes of PCM to \S+ \((\d+) frames,", output)
    result = {
        "wav_path": path,
        # ".imu.csv", NOT the combined ".csv". This handler can only ever run
        # against a CONFIG_APP_AUDIO_DEBUG build — it hard-fails above unless
        # `sound agc gain` answers, and that subcommand exists only under that
        # symbol. On such a build sound_record_wav() routes to record_wav_tap(),
        # which writes "<wav>.csv" with D-rows ONLY and puts the I-rows here.
        # The combined layout belongs to record_wav_capture(), which a debug
        # build never reaches.
        "imu_csv_path": path + ".imu.csv",
        "imu_samples": int(imu.group(1)) if imu else 0,
        "agc_restored": restored,
    }
    # Reported ONLY on a positive success signal from one of those two lines.
    # Keying on the summary line's "io retries" text instead would advertise a
    # file for captures that never wrote one — the sidecar failing to open
    # (FatFs is already at CONFIG_FS_FATFS_NUM_FILES=4), being retired
    # mid-capture, or coming out misaligned. audio_sidecar_close() words those
    # ("Capture CSV could not be opened", "Capture CSV write failed",
    # "Capture CSV MISALIGNED") precisely so neither regex matches them.
    #
    # Both paths put the analysis in "<wav>.csv"; only the announcement and the
    # rest of the layout differ (the tap path also writes the separate
    # "<wav>.imu.csv" that imu_csv_path points at).
    success = audio or tap
    if success:
        result["analysis_csv_path"] = path + ".csv"
        result["analysis_frames"] = int(success.group(1))
    if not imu:
        # CONFIG_IMU off, or the sidecar could not be opened. The WAV is still
        # usable; the scenario just has no IMU track.
        result["warning"] = "no IMU sidecar reported — scenario will be audio-only"
    m = re.search(r"Wrote (\d+) bytes of PCM", output)
    if m:
        result["bytes"] = int(m.group(1))
    return _ok(**result)


async def handle_sound_dump(state: SerialState, args: dict) -> dict:
    connection_id = args["connection_id"]
    frames = args["frames"]
    output_path = args["output_path"]
    buckets = args.get("buckets", False)

    cmd = f"sound dump {frames}" + (" buckets" if buckets else "")

    # Streamed capture: _run_command's echo+prompt heuristic is wrong here — the
    # Zephyr shell redraws the prompt after EVERY line it prints, so a
    # prompt-anchored read returns after roughly one line and a long dump needs
    # ~one read round per frame (observed: 600-frame dumps truncated with the
    # old frames//50 round budget). Accumulate on the "#DONE ... dropped=" trailer
    # instead, with the round budget scaled to the stream length.
    await handle_write(state, {"connection_id": connection_id, "data": "03", "as": "hex"})
    await handle_flush(state, {"connection_id": connection_id, "what": "input"})
    await handle_write(state, {
        "connection_id": connection_id,
        "data": cmd,
        "append_newline": True,
    })

    accumulated = ""
    empty_rounds = 0
    for _ in range(frames + 120):  # ~1 round per line + slack for params/echo/logs
        resp = await handle_read_until(state, {
            "connection_id": connection_id,
            "delimiter": PROMPT,
            "timeout_ms": 3000,
        })
        chunk = resp.get("data", "") if isinstance(resp, dict) else ""
        accumulated += chunk
        if "#DONE" in accumulated and "dropped=" in accumulated.split("#DONE")[-1]:
            break
        if not chunk:
            # Frames arrive every 32 ms; a 3 s silent window twice over means the
            # stream ended without the trailer — stop rather than spin.
            empty_rounds += 1
            if empty_rounds >= 2:
                break
        else:
            empty_rounds = 0

    plain = _ANSI_RE.sub("", accumulated).replace("\r", "")
    if "#DONE" not in plain:
        return _err("dump_incomplete", (plain[-500:] if plain else "no response"))

    kept = [ln for ln in plain.split("\n")
            if ln.startswith(("D,", "#PARAMS", "#DONE"))]
    with open(output_path, "w") as f:
        f.write("\n".join(kept) + "\n")

    done = kept[-1] if kept and kept[-1].startswith("#DONE") else ""
    m = re.search(r"frames=(\d+) dropped=(\d+)", done)
    return _ok(output_path=output_path, lines=len(kept),
               frames=int(m.group(1)) if m else None,
               dropped=int(m.group(2)) if m else None)


HANDLERS = {
    "rgb_sunglasses.clear_indicator": handle_clear_indicator,
    "rgb_sunglasses.set_animation": handle_set_animation,
    "rgb_sunglasses.get_animation": handle_get_animation,
    "rgb_sunglasses.glim_list": handle_glim_list,
    "rgb_sunglasses.glim_select": handle_glim_select,
    "rgb_sunglasses.glim_set_loop_mode": handle_glim_set_loop_mode,
    "rgb_sunglasses.sound_record": handle_sound_record,
    "rgb_sunglasses.capture_scenario": handle_capture_scenario,
    "rgb_sunglasses.sound_dump": handle_sound_dump,
}
