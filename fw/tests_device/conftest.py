"""Shared fixtures for the on-device (HIL) suite.

Run via fw/scripts/run-device-tests.sh — never bare pytest without the
twister-harness plugin options (the `dut`/`shell` fixtures come from
pytest-twister-harness and need --device-type/--device-serial-pty/...).

The suite REQUIRES --dut-scope=session (run-device-tests.sh and
fw/testcase.yaml both pass it): the fixtures below are session-scoped, which
means one flash + one boot for the whole run; a function-scoped DUT would
reflash per test and violate the session-scope dependency chain.
"""

from __future__ import annotations

import logging
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(__file__))

from twister_harness import DeviceAdapter, Shell  # noqa: E402

from helpers import ports, provisioning  # noqa: E402
from helpers.rgb_shell import RgbShell  # noqa: E402

logger = logging.getLogger(__name__)


@pytest.fixture(scope="session")
def rgb(dut: DeviceAdapter, shell: Shell) -> RgbShell:
    """The suite's shell handle: quirk-hardened exec with retval checking."""
    r = RgbShell(dut, shell)
    r.sync()
    # Boot floods the console for seconds after the prompt appears (llext
    # relocation logs, USB bring-up), and extension discovery finishes even
    # later — wait for the full boot-settled barrier before fixtures start
    # probing, or echoes smear and `ext list` reads empty.
    r.wait_boot_settled()
    return r


@pytest.fixture(scope="session")
def device_state(rgb: RgbShell) -> dict:
    """One-time probe of the board's power/provisioning configuration.

    Power tests are configuration-aware rather than skip-happy: the CI rig
    may have no battery, a dev bench usually has battery + VBUS. Every power
    test branches its assertions on this dict; the banner below makes each
    run's actual coverage explicit in the log.
    """
    limits = rgb.bq_limits()
    policy = rgb.policy()
    state = {
        "vbat_present": bool(limits.get("VBAT_PRESENT", policy.get("vbat_present", 0))),
        "vbus_present": bool(limits.get("VBUS_PRESENT", policy.get("vbus_present", 0))),
        "charging": bool(policy.get("effective_enable", 0)),
        "limits": limits,
        "policy": policy,
        "glim": rgb.glim_list(),
        "ext": rgb.ext_list(),
    }
    logger.info(
        "device power configuration: battery=%s vbus=%s charging=%s",
        state["vbat_present"], state["vbus_present"], state["charging"],
    )
    logger.info(
        "device assets: %d glim file(s) %s, %d extension slot(s) %s",
        len(state["glim"]), state["glim"],
        len(state["ext"]), [s["name"] for s in state["ext"]],
    )
    state["provisioning_problems"] = provisioning.check_baseline(
        state["glim"], state["ext"]
    )
    if state["provisioning_problems"]:
        logger.warning("board not fully provisioned: %s", state["provisioning_problems"])
    return state


@pytest.fixture(autouse=True)
def _gate_on_device_state(request: pytest.FixtureRequest, device_state: dict):
    """Enforce the requires_* markers against the probed configuration.

    Power-configuration markers SKIP (legitimately variable between rigs);
    requires_provisioned FAILS (a fixable setup error, not a configuration —
    silently skipping it would hollow out CI coverage).
    """
    if request.node.get_closest_marker("requires_battery") and not device_state["vbat_present"]:
        pytest.skip("needs a battery attached (VBAT_PRESENT=0)")
    if request.node.get_closest_marker("requires_no_battery") and device_state["vbat_present"]:
        pytest.skip("needs the battery detached (VBAT_PRESENT=1)")
    if request.node.get_closest_marker("requires_vbus") and not device_state["vbus_present"]:
        pytest.skip("needs USB/VBUS power (VBUS_PRESENT=0)")
    if request.node.get_closest_marker("requires_charging") and not (
        device_state["vbat_present"]
        and device_state["vbus_present"]
        and device_state["charging"]
    ):
        pytest.skip("needs battery + VBUS + charging enabled")
    m = request.node.get_closest_marker("requires_ext")
    if m:
        wanted = m.args[0]
        if wanted not in {s["name"] for s in device_state["ext"]}:
            pytest.skip(f"extension '{wanted}' not installed")
    if request.node.get_closest_marker("requires_provisioned") and device_state[
        "provisioning_problems"
    ]:
        pytest.fail(
            "board is not provisioned: "
            + "; ".join(device_state["provisioning_problems"])
            + " — run fw/scripts/provision-device.sh (see /provision-device), "
            "reboot the board, then re-run",
            pytrace=False,
        )


@pytest.fixture(scope="session")
def smp_port() -> str:
    """The board's MCUmgr/SMP CDC port (function 1, NOT the shell port)."""
    port = ports.find_smp_port()
    if port is None:
        pytest.skip("board SMP port (2fe3:0001 interface 02) not found")
    return port


@pytest.fixture(scope="session")
def mcumgr(is_mcumgr_available, smp_port: str):
    """Override pytest-twister-harness's stock fixture.

    The stock one targets device_config.serial — empty under serial_pty, and
    the SHELL port even when set. This board's SMP server lives on its own
    CDC function; resolve it by USB identity instead.
    """
    from twister_harness.helpers.mcumgr import MCUmgr

    return MCUmgr.create_for_serial(smp_port)
