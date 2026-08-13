"""Destructive tier: forced crashes, settings wipes, filesystem destruction.

Run explicitly (`--tier destructive`); conftest's collection hook orders
these after every other selected test, and the module teardown REPROVISIONS
the board (fw/scripts/provision-device.sh + reboot + manifest re-verify) so
it is always left usable — even when a test fails.

Side effect to know about: `factory_reset soft|now` erases ALL settings
INCLUDING BLE bonds — the shared phone must be re-paired (/re-pair) after a
destructive run.

In-file order is load-bearing:
  1. coredump crash loop      (needs a healthy, provisioned board AND a
                               clean coredump baseline — the fault tests
                               below stay coredump-free by using Hang)
  2. ext fault latch/recovery (#308 — deliberate sandbox fault via Hang)
  3. bad-manifest boot survival (#89 — plants a corrupt .llext, reboots)
  4. ext param persists by name (#303 — deletes a sibling to renumber slots)
  5. factory_reset soft       (must prove glim/ext SURVIVE — files intact)
  6. fatfs corrupt + factory_reset now  (destroys the filesystem — last)

Regressions pinned:
- #102/#80  crash → flash-partition capture → auto-reboot → drain to FAT.
- #308 (partial)  dump presence asserted via BOTH `coredump find` AND the
        FAT directory, each in its phase-correct state — `find` alone misses
        drained dumps.
- #303  extension persisted params are keyed by display name, so they follow
        the extension across a slot renumber (delete an earlier sibling).
- #268/#183  factory reset phases: soft keeps files, now wipes them.
- #327  boot-time FAT auto-format after `fatfs corrupt`.
- #325/#105  main-stack high-water bound while that boot ran f_mkfs on main.
"""

from __future__ import annotations

import os
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
    # "crash dump(s) awaiting collection", NOT just "awaiting collection" —
    # the negative message ("no dumps awaiting collection in ...") contains
    # that substring too, which made this assertion unfailable (PR #346
    # review).
    status = " ".join(rgb.exec("coredump_mgr status"))
    assert "crash dump" in status and "no dumps" not in status, status

    # Cleanup: collect (discard) the dump so later runs start clean.
    # check=False: a delete is not retry-idempotent (an echo-smear resend
    # after a successful unlink would fail on the missing file) — the status
    # line below is the real verification.
    rgb.exec(f"fs rm /NAND:/coredump/{drained_file}", check=False)
    status = " ".join(rgb.exec("coredump_mgr status"))
    assert "no dumps" in status, f"cleanup failed: {status}"


HELLO = "Hello Extension"


def _hello_slot(rgb: RgbShell) -> int:
    slots = [s for s in rgb.ext_list() if s["name"] == HELLO]
    # FAIL, not skip: hello is part of the provisioning BASELINE (a fixable
    # setup error), not a rig difference — a skip here would green a run
    # with zero coverage of this tier's subject (README rule; PR #348
    # review).
    assert slots, f"{HELLO} not installed — run fw/scripts/provision-device.sh"
    return slots[0]["slot"]


def test_ext_fault_latch_and_recovery(rgb: RgbShell):
    """The full #308 flow, using hello's Hang injector.

    NOTE which budget this exercises: hello's P_HANG is `while (1) {}` — a
    pure CPU SPIN, so the fault that fires is the 50 ms CPU budget
    (CONFIG_APP_EXT_TICK_CPU_BUDGET_MS), NOT the 500 ms wall backstop (that
    one is for extensions that BLOCK; it currently has no on-device
    coverage — hello would need a sleeping param; PR #348 review). Either
    verdict is host-detected, so no coredump side effects, unlike Crash.

    Pins: fault latched with measurements + params-reset flag; the record
    SURVIVES `ext select` recovery (that is the whole point of #308 — the
    log line scrolls away, the record must not); persisted params were reset
    to defaults (a poisoned param must not re-fault on retry, PR #89 rule);
    `ext faults clear` drops records without touching slot state.
    """
    slot = _hello_slot(rgb)
    rgb.exec("ext faults clear")

    rgb.exec(f"ext select {slot}")
    # Hang is param 3 (see fw/extensions/hello/hello.c). Params reach the
    # extension at tick time; the CPU budget then fires within ~1 tick.
    # attempts=1: the arm is EFFECT-non-idempotent — the fault it provokes
    # resets Hang to 0, so an echo-smear resend would re-arm the recovered
    # slot and fail the recovery assertions against healthy firmware.
    rgb.exec(f"ext param {slot} 3 1", attempts=1)
    try:
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            if any(s["faulted"] for s in rgb.ext_list() if s["name"] == HELLO):
                break
            time.sleep(1.0)
        else:
            pytest.fail("Hang=1 never produced a sandbox fault within 15 s")

        recs = [r for r in rgb.ext_faults() if r["name"] == HELLO]
        assert recs, f"fault not latched in `ext faults`: {rgb.ext_faults()}"
        rec = recs[0]
        assert rec["params_reset"] is True, f"tick-time fault must reset params: {rec}"
        assert rec["state"] == "FAULTED", rec
        assert rec["count"] == 1, rec

        # Recovery: ext select clears the slot's fault flag SYNCHRONOUSLY,
        # but the reload happens lazily on the pattern-controller thread's
        # next tick — so the property that proves real recovery is the tick
        # counter INCREASING, not the flag (a regression that un-flags but
        # never re-ticks would pass a flag check; PR #348 review). Poll to
        # convergence per the README rule, no fixed sleep.
        rgb.exec(f"ext select {slot}")
        base_ticks = rgb.ext_stats().get(HELLO, {}).get("ticks", 0)
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            current = [s for s in rgb.ext_list() if s["name"] == HELLO][0]
            ticks = rgb.ext_stats().get(HELLO, {}).get("ticks", 0)
            if not current["faulted"] and ticks > base_ticks:
                break
            time.sleep(1.0)
        else:
            pytest.fail(
                f"recovery did not converge: faulted={current['faulted']}, "
                f"ticks {base_ticks} -> {ticks} (never increased)"
            )
        rec = [r for r in rgb.ext_faults() if r["name"] == HELLO][0]
        assert rec["state"] == "recovered", (
            f"record must survive recovery with updated state (#308): {rec}"
        )
        # The reset params are what make the retry safe: Hang must be 0 now.
        # Read through the shared helper with the name cross-check (idx 3 ==
        # Hang) rather than a hand-rolled regex left inline in the deduped
        # file — same assertion, and it fails loudly if a manifest reorder
        # ever moved Hang off index 3 (PR #365 review).
        assert rgb.ext_param_int(slot, 3, name="Hang") == 0, (
            "Hang param not reset to default after the fault"
        )
    finally:
        # All check=False: a cleanup hiccup on a just-faulted, noisy console
        # must never REPLACE the assertion failure carrying the evidence
        # (PR #348 review).
        rgb.exec(f"ext param {slot} 3 0", check=False)  # belt-and-suspenders
        rgb.exec("anim set zigzag", check=False)
        rgb.exec("ext faults clear", check=False)
    assert rgb.ext_faults() == [], "records survived `ext faults clear`"


def test_bad_manifest_boot_survival(rgb: RgbShell, dut, device_state: dict):
    """#89: a corrupted .llext on NAND must be REJECTED at discovery — never
    kernel-mode-deref'd (the original bug halted the whole firmware). The
    board must boot, load every healthy extension, refuse the bad file, and
    latch a discovery-failure record for it (extension_host.cpp init()
    latches rejected files by REGISTRY FILE NAME — hence the zz_bad match).

    Cleanup is HOST-SIDE by construction: the regression this test detects
    is 'the board halts at boot', in which state no shell command can
    delete the file and every subsequent boot re-halts — the shared board
    would stay wedged until a human intervened (PR #348 review). The same
    USB MSC path that planted the file removes it, and a J-Link pin reset
    recovers the board without needing the firmware's cooperation.
    """
    expected_ext = {s["name"] for s in device_state["ext"]}
    provisioning.plant_corrupt_extension(name="zz_bad.llext", source="hello.llext")
    try:
        rgb.reboot()  # also satisfies the FAT-coherence contract

        loaded = {s["name"] for s in rgb.ext_list()}
        assert loaded == expected_ext, (
            f"healthy extensions did not all survive alongside the bad file: "
            f"{sorted(loaded)} vs {sorted(expected_ext)}"
        )
        assert not any(
            s["file"] == "zz_bad.llext" for s in rgb.ext_list()
        ), "the corrupted extension was LOADED — #89 validation is gone"
        discovery = [r for r in rgb.ext_faults() if "zz_bad" in r["name"]]
        assert discovery, (
            f"no discovery-failure record latched for the bad file: "
            f"{rgb.ext_faults()}"
        )
        rgb.exec("ext faults clear", check=False)
    finally:
        # Remove the file WITHOUT the board's help (it may be halted —
        # that's the failure mode under test). MSC enumerates even when the
        # app halted after USB init; if the whole USB stack died, this
        # raises and the reprovision teardown is the next line of defense.
        with provisioning.nand_mount() as mnt:
            bad = os.path.join(mnt, "ext", "zz_bad.llext")
            if os.path.exists(bad):
                os.unlink(bad)
        # FAT-coherence + rescan; fall back to a J-Link pin reset if the
        # shell is gone.
        try:
            rgb.reboot()
        except Exception:
            provisioning.hard_reset(str(dut.device_config.id))
            rgb.wait_reboot()


def _read_hello_speed(rgb: RgbShell, slot: int) -> int:
    return rgb.ext_param_int(slot, 0, name="Speed")  # hello param 0 = Speed


def test_ext_param_persists_by_name(rgb: RgbShell):
    """#303: an extension's persisted params follow its DISPLAY NAME, not its
    slot index, across a NAND file-set change that renumbers slots.

    The persistence key is `appcfg/ext/<displayName>` (extension_host.cpp
    scan_slot → extension_param_persistence::build_settings_key), and boot
    load is one `settings_load_subtree("appcfg/ext")` keyed by name — so a
    slot that renumbers must still recover its blob. If persistence were
    keyed by index, the renumbered extension would come up with defaults.

    Mechanism: delete the alphabetically-earlier baseline extension
    (cpptest.llext) so hello renumbers from slot 1 → slot 0 across a reboot.
    Both hello AND cpptest are provisioning baseline (EXPECTED_EXT), so a
    missing sibling is a fixable setup error (FAIL, not skip — a skip would
    silently retire this #303 pin, PR #359 review) rather than a rig
    difference. Uses USB MSC directly — the app-driven FILE_MGMT group-64
    delete path is a different surface, covered by E2E-04. Self-restoring
    (re-writes cpptest's own bytes); the destructive-tier reprovision is the
    backstop.
    """
    EARLIER = "cpptest.llext"  # sorts before hello.llext → occupies slot 0

    orig_slot = _hello_slot(rgb)  # FAILs (not skips) if hello is missing
    earlier = next((s for s in rgb.ext_list() if s["file"] == EARLIER), None)
    assert earlier is not None and earlier["slot"] < orig_slot, (
        f"need {EARLIER} installed at a lower slot than {HELLO} to force a "
        f"renumber — run fw/scripts/provision-device.sh; got "
        f"{[(s['file'], s['slot']) for s in rgb.ext_list()]}"
    )

    # Save cpptest's bytes so the test can restore it without a full reprovision.
    cpptest_bytes = provisioning.nand_read_ext(EARLIER)

    NEW_SPEED = 91  # distinct from hello's default (50)
    orig_speed = _read_hello_speed(rgb, orig_slot)
    marker_written = False
    deleted = False
    try:
        rgb.exec(f"ext param {orig_slot} 0 {NEW_SPEED}")
        # Set BEFORE the read-back: once the write lands the device may hold
        # the marker, so the restore must fire even if the read-back itself
        # fails (PR #359 review — else Speed=91 persists into later sessions).
        marker_written = True
        assert _read_hello_speed(rgb, orig_slot) == NEW_SPEED
        rgb.wait_persist_flush()

        # Renumber: remove the earlier-sorting sibling, reboot to re-scan.
        provisioning.nand_remove_ext(EARLIER)
        deleted = True
        rgb.reboot()

        after = rgb.ext_list()
        hello_after = next((s for s in after if s["name"] == HELLO), None)
        assert hello_after, f"{HELLO} vanished after removing {EARLIER}: {after}"
        assert not any(s["file"] == EARLIER for s in after), (
            f"{EARLIER} still present after delete+reboot: {after}"
        )
        # The renumber must actually have happened, or the test proves nothing.
        assert hello_after["slot"] < orig_slot, (
            f"{HELLO} did not renumber (still slot {hello_after['slot']}) — "
            f"can't distinguish by-name from by-index: {after}"
        )
        # THE #303 assertion: the param followed the NAME to the new slot.
        assert _read_hello_speed(rgb, hello_after["slot"]) == NEW_SPEED, (
            f"hello.Speed did not survive the slot {orig_slot}->"
            f"{hello_after['slot']} renumber — persistence is keyed by index, "
            f"not name (#303 regression)"
        )
    finally:
        # Guarded so a cleanup hiccup never REPLACES the #303 assertion
        # failure that carries the evidence (PR #348/#359 review). Only
        # rewrite cpptest if it was actually removed (`deleted`) — a "wb"
        # truncate of an un-deleted file reallocates its cluster chain under
        # the live FAT mount (double-writer corruption, fw/CLAUDE.md).
        try:
            if deleted:
                provisioning.nand_write_ext(EARLIER, cpptest_bytes)
            rgb.reboot()
            if marker_written:
                h = next((s for s in rgb.ext_list() if s["name"] == HELLO), None)
                if h:
                    rgb.exec(f"ext param {h['slot']} 0 {orig_speed}", check=False)
                    rgb.wait_persist_flush()
        except Exception:
            # Restore failed — the module reprovision teardown is the backstop.
            pass


def test_factory_reset_soft_keeps_files(rgb: RgbShell):
    glim_before = rgb.glim_list()
    ext_before = {s["name"] for s in rgb.ext_list()}
    assert glim_before and ext_before, "board must be provisioned before this test"

    # Marker: a persisted selection that a settings wipe must erase.
    target = glim_before[-1]
    rgb.glim_select_name(target)
    rgb.wait_persist_flush()

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
