"""Power-subsystem regression tests — strictly read-only commands.

Configuration-aware by design (see README.md): the CI rig may have no battery
attached, so every test here runs in ANY power setup and asserts only the
invariants valid for the configuration probed into `device_state`. Nothing in
this file writes to the BQ25792 or TPS25750.

Regressions pinned:
- #106  BQ25792 IBAT/IBUS ADC values are 16-bit SIGNED; a missing sign
        extension turns small negatives into absurd positives (~65000 mA).
- #109/#111  TPS25750 I2Cm bridge transactions must be serialized; the race
        shows as plausible-but-wrong values (VBAT reading back as VBUS) and
        the old unbounded CMD1 poll hung callers instead of timing out.
- #145  IINDPM/VINDPM must be derived from the negotiated PD contract.
"""

from __future__ import annotations

import time

import pytest

from helpers.rgb_shell import RgbShell

pytestmark = pytest.mark.integration

# A sign-extension bug turns -N mA into 65536-N: anything beyond ±20 A is not
# a measurement this hardware can produce (5 A charge ceiling), it's a decode
# bug. Config-specific bands below are tighter.
ABSURD_MA = 20000


def test_ibat_sign_sane(rgb: RgbShell, device_state: dict):
    samples = []
    for _ in range(5):
        samples.append(rgb.bq_status())
        time.sleep(1.0)

    for kv in samples:
        ibat = kv["IBAT"]
        assert -ABSURD_MA < ibat < ABSURD_MA, (
            f"IBAT={ibat} mA is an unsigned-decode artifact (#106): {kv}"
        )
        if device_state["vbat_present"]:
            assert -8000 < ibat < 8000, f"IBAT={ibat} mA implausible with battery: {kv}"
            if not device_state["vbus_present"]:
                # Discharging: battery current must not read as charging.
                assert ibat <= 0, f"IBAT={ibat} mA positive while on battery only: {kv}"
        else:
            # No battery: there is no cell to source/sink real current.
            assert abs(ibat) < 500, f"IBAT={ibat} mA with no battery attached: {kv}"


def test_i2cm_bridge_stability(rgb: RgbShell, device_state: dict):
    """20 back-to-back reads racing the 500 ms charger-status thread.

    Unconditional: every read completes bounded (the #111 fix replaced an
    infinite CMD1 poll with -ETIMEDOUT) and error-free. Value plausibility is
    config-branched; exact VBAT==VBUS equality is the #111 interleave
    signature when a battery is attached (real VBAT ~3-4.5 V vs VBUS ~5 V).
    """
    vbats: list[int] = []
    for i in range(20):
        t0 = time.monotonic()
        out = rgb.exec("power bq status")
        elapsed = time.monotonic() - t0
        # Command + retval round-trip; the read itself is bounded ~1.5 s by
        # the bridge timeout — 5 s here means "hung", not "slow shell".
        assert elapsed < 5.0, f"read #{i} took {elapsed:.1f}s — bridge wedged?"
        assert not any("fail" in line.lower() for line in out), (
            f"read #{i} reported failures: {out}"
        )
        kv = RgbShell.parse_kv(out)
        vbats.append(kv["VBAT"])
        if device_state["vbat_present"]:
            assert kv["VBAT"] != kv["VBUS"], (
                f"read #{i}: VBAT == VBUS == {kv['VBAT']} mV — the #111 "
                f"interleaved-transaction signature"
            )
        time.sleep(0.25)

    spread = max(vbats) - min(vbats)
    limit = 100 if device_state["vbat_present"] else 300
    assert spread <= limit, (
        f"VBAT spread {spread} mV over 5 s (samples: {vbats}) — cross-register "
        f"bleed or ADC instability"
    )


def test_pd_contract_policy_convergence(rgb: RgbShell, device_state: dict):
    """#145: charger limits follow the PD contract — as convergence, not a
    single-shot equality (the policy reconciles on a 500 ms tick and the
    contract can renegotiate underneath us)."""
    if not device_state["vbus_present"]:
        kv = rgb.pd_contract()
        assert kv.get("connected") == 0, f"no VBUS but PD says connected: {kv}"
        policy = rgb.policy()
        assert policy["effective_enable"] == 0, (
            f"charging effective without VBUS: {policy}"
        )
        return

    last: tuple[dict, dict] | None = None
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        contract = rgb.pd_contract()
        policy = rgb.policy()
        last = (contract, policy)
        if contract.get("connected") == 1 and contract.get("available_ma"):
            expected_iindpm = min(contract["available_ma"], 3000)
            if (
                policy.get("iindpm_target") == expected_iindpm
                and 0 < policy.get("vindpm_target", 0) <= contract["available_mv"]
            ):
                return  # converged
        time.sleep(0.5)

    pytest.fail(
        f"PD contract and charger policy never converged within 5 s "
        f"(≥2 reconcile ticks): contract={last[0]} policy={last[1]}"
    )
