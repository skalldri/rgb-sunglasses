"""Destructive tier: forced crashes, settings wipes, filesystem destruction.

Run explicitly (`--tier destructive`); conftest's collection hook orders
these after every other selected test, and the module teardown REPROVISIONS
the board (fw/scripts/provision-device.sh + reboot + manifest re-verify) so
it is always left usable — even when a test fails.

Side effect to know about: `factory_reset soft|now` erases ALL settings
INCLUDING BLE bonds — the shared phone must be re-paired (/re-pair) after a
destructive run.

In-file order is load-bearing:
  1. coredump crash loop      (needs a healthy, provisioned board)
  2. factory_reset soft       (must prove glim/ext SURVIVE — files intact)
  3. fatfs corrupt + factory_reset now  (destroys the filesystem — last)

Regressions pinned:
- #102/#80  crash → flash-partition capture → auto-reboot → drain to FAT.
- #308 (partial)  dump presence asserted via BOTH `coredump find` AND the
        FAT directory, each in its phase-correct state — `find` alone misses
        drained dumps.
- #268/#183  factory reset phases: soft keeps files, now wipes them.
- #327  boot-time FAT auto-format after `fatfs corrupt`.
- #325/#105  main-stack high-water bound while that boot ran f_mkfs on main.
"""

from __future__ import annotations

import time

import pytest

from helpers import provisioning
from helpers.rgb_shell import RgbShell

pytestmark = pytest.mark.destructive


@pytest.fixture(scope="module", autouse=True)
def reprovision_after(rgb: RgbShell, dut):
    yield
    # Always runs, pass or fail: the board must leave this module usable.
    provisioning.reprovision(rgb, str(dut.device_config.build_dir))


def _require_crash_commands(rgb: RgbShell):
    out = rgb.exec("crash", check=False)
    if any("command not found" in line for line in out):
        pytest.skip("image lacks CONFIG_APP_CRASH_TEST_COMMANDS (wrong build?)")


def test_coredump_crash_loop(rgb: RgbShell):
    _require_crash_commands(rgb)
    status = " ".join(rgb.exec("coredump_mgr status"))
    assert "no dumps" in status, f"stale dump(s) before test: {status}"

    rgb.exec("hwinfo reset_cause clear")
    rgb.exec_oneway("crash panic")
    rgb.wait_reboot()

    causes = " ".join(rgb.exec("hwinfo reset_cause show"))
    assert "software" in causes, (
        f"post-panic reboot did not report a software reset cause: {causes}"
    )

    # Phase-correct #308 rule. Right after boot the dump lives in the flash
    # partition (`find` sees it, FAT does not); once the manager's ≤60 s
    # drain runs, the FAT file exists and the partition is invalidated
    # (`find` must NOT see it any more). Poll across the transition.
    found_in_flash = "Stored coredump found." in " ".join(
        rgb.exec("coredump find", check=False)
    )
    drained_file = None
    deadline = time.monotonic() + 90.0
    while time.monotonic() < deadline:
        ls = " ".join(rgb.exec("fs ls /NAND:/coredump", check=False))
        if "core_" in ls:
            drained_file = next(
                tok for tok in ls.replace("/", " ").split() if tok.startswith("core_")
            )
            break
        time.sleep(5.0)
    assert drained_file, (
        "coredump never drained to /NAND:/coredump within 90 s "
        f"(flash-partition find at boot: {found_in_flash})"
    )
    # The manager deliberately invalidates the partition only AFTER the FAT
    # file is safely on disk (coredump_manager_core.cpp) — so right at
    # file-appearance there is a legitimate window where `find` still says
    # found. Poll across it instead of racing it (hardware-observed).
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        post_drain = " ".join(rgb.exec("coredump find", check=False))
        if "Stored coredump NOT found." in post_drain:
            break
        time.sleep(2.0)
    else:
        pytest.fail(f"flash partition not invalidated after drain: {post_drain}")
    status = " ".join(rgb.exec("coredump_mgr status"))
    assert "awaiting collection" in status, status

    # Cleanup: collect (discard) the dump so later runs start clean.
    rgb.exec(f"fs rm /NAND:/coredump/{drained_file}")
    status = " ".join(rgb.exec("coredump_mgr status"))
    assert "no dumps" in status, f"cleanup failed: {status}"


def test_factory_reset_soft_keeps_files(rgb: RgbShell):
    glim_before = rgb.glim_list()
    ext_before = {s["name"] for s in rgb.ext_list()}
    assert glim_before and ext_before, "board must be provisioned before this test"

    # Marker: a persisted selection that a settings wipe must erase.
    target = glim_before[-1]
    rgb.glim_select_name(target)
    time.sleep(3.0)  # let the debounced store flush

    rgb.exec_oneway("factory_reset soft")
    rgb.wait_reboot()

    assert not any(
        "glim_player/selected_name" in k for k in rgb.settings_keys()
    ), "settings survived factory_reset soft"
    # THE #268 assertion: soft resets settings but keeps every file.
    assert rgb.glim_list() == glim_before, (
        f"factory_reset soft lost GLIM files: {rgb.glim_list()} vs {glim_before}"
    )
    assert {s["name"] for s in rgb.ext_list()} == ext_before, (
        "factory_reset soft lost extensions"
    )


def test_fatfs_corrupt_autoformat_then_factory_now(rgb: RgbShell):
    _require_crash_commands(rgb)  # `fatfs corrupt` shares the Kconfig gate

    # Erasing the boot sector is idempotent — plain exec is fine here.
    rgb.exec("fatfs corrupt confirm")
    rgb.reboot(timeout=150.0)  # this boot runs f_mkfs on main — slow

    assert rgb.glim_list() == [], (
        f"expected an empty, freshly-formatted FAT: {rgb.glim_list()}"
    )
    # #325/#105: the f_mkfs boot is main's worst case; its high-water must
    # stay within the right-sized stack.
    main = rgb.stacks().get("main")
    assert main, "main thread missing from stack dump"
    assert main["used"] <= 7700 and main["pct"] <= 50, (
        f"main stack high-water {main} after a boot-time f_mkfs — "
        f"the #105 right-sizing margin is gone"
    )

    rgb.exec_oneway("factory_reset now")
    rgb.wait_reboot()
    assert rgb.glim_list() == [], "factory_reset now left GLIM files"
    ext_out = " ".join(rgb.exec("ext list"))
    assert "no extensions loaded" in ext_out, f"extensions survived: {ext_out}"
    assert not any(k.startswith("appcfg/") for k in rgb.settings_keys()), (
        "appcfg settings survived factory_reset now"
    )
    # Board is now bare — the module teardown reprovisions it.
