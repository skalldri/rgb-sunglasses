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
    "glim_player", "matrix_code", "tilt",
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


HANDLERS = {
    "rgb_sunglasses.clear_indicator": handle_clear_indicator,
    "rgb_sunglasses.set_animation": handle_set_animation,
    "rgb_sunglasses.get_animation": handle_get_animation,
    "rgb_sunglasses.glim_list": handle_glim_list,
    "rgb_sunglasses.glim_select": handle_glim_select,
    "rgb_sunglasses.glim_set_loop_mode": handle_glim_set_loop_mode,
}
