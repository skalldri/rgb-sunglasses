"""
Regression tests for re-pair.py's SAFE, device-specific forget logic — the part that
must NEVER unpair the wrong device on a phone with many bonded devices.

The fixtures in ./fixtures/ are real `uiautomator dump` XML captured from the Bluetooth
Settings of a OnePlus 9 Pro (OxygenOS) and a Pixel 9 Pro (stock Android), then SANITIZED:
every personal device name was replaced with a neutral "Demo …" placeholder. The RGB
Sunglasses board entries and all UI structure (resource-ids, bounds, the per-device gear
nodes, the "Unpair"/"Forget" labels) are preserved, so these exercise the exact selectors
the script uses on real hardware — including the OEM differences that broke it once:
  - Pixel gear:    rid=settings_button, content-desc "<name>. Configure device detail."
  - OxygenOS gear: rid=deviceDetails,  content-desc generic "Device Settings"
  - action label:  "Forget" (AOSP) vs "Unpair" (OxygenOS)

Run:  pytest scripts/tests/
"""
import importlib.util
import os
import xml.etree.ElementTree as ET

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_FIXTURES = os.path.join(_HERE, "fixtures")

# re-pair.py has a hyphen -> load it by path rather than `import`.
_spec = importlib.util.spec_from_file_location("re_pair", os.path.join(_HERE, "..", "re-pair.py"))
re_pair = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(re_pair)

RGB = "RGB Sunglasses Proto0 8996"


def _ui(fixture, screen_h):
    ui = re_pair.UI(adb=None, screen_h=screen_h)
    root = ET.parse(os.path.join(_FIXTURES, fixture)).getroot()
    ui.nodes = [re_pair.Node(e) for e in root.iter("node")]
    return ui


def _rp(ui, name):
    # Bypass __init__ (which needs adb/serial); we only exercise pure UI helpers.
    rp = re_pair.RePair.__new__(re_pair.RePair)
    rp.ui = ui
    rp.name = name
    return rp


# ---- OnePlus 9 Pro / OxygenOS ------------------------------------------------
OP_LIST = ("oneplus-oxygenos-device-list.xml", 2412)
OP_DETAILS = ("oneplus-oxygenos-device-details.xml", 2412)


def test_oneplus_targets_the_rgb_gear_not_a_neighbor():
    rp = _rp(_ui(*OP_LIST), RGB)
    gear = rp._find_device_gear(RGB)
    assert gear is not None
    # RGB's OWN gear (rid=deviceDetails) in its row - at the row's right edge, not the
    # row body (which would toggle connect).
    x, y = gear
    assert x > 900  # right-edge gear column, never the ~540px row-body center


def test_oneplus_each_device_resolves_to_its_own_gear():
    ui = _ui(*OP_LIST)
    # Different devices resolve to gears at DIFFERENT rows (never a shared/first gear).
    g_rgb = _rp(ui, RGB)._find_device_gear(RGB)
    g_car = _rp(ui, "Demo Car A")._find_device_gear("Demo Car A")
    g_hp = _rp(ui, "Demo Headphones B")._find_device_gear("Demo Headphones B")
    assert g_rgb and g_car and g_hp
    assert len({g_rgb[1], g_car[1], g_hp[1]}) == 3  # three distinct rows


def test_oneplus_partial_name_never_matches():
    # The ambiguous prefix must match nothing (multiple boards can share it).
    assert _rp(_ui(*OP_LIST), "RGB Sunglasses")._find_device_gear("RGB Sunglasses") is None


def test_oneplus_absent_device_returns_none():
    assert _rp(_ui(*OP_LIST), "Not Paired 9999")._find_device_gear("Not Paired 9999") is None


def test_oneplus_details_gate_accepts_target_rejects_others():
    ui = _ui(*OP_DETAILS)  # this is RGB's details page ("Unpair", no bt_header_device_name)
    assert _rp(ui, RGB)._details_page_for(RGB) is True
    # SAFETY: the gate must reject any other device name on this page.
    assert _rp(ui, "Demo Car A")._details_page_for("Demo Car A") is False


def test_oneplus_list_is_not_a_details_page_but_details_is():
    assert _rp(_ui(*OP_LIST), RGB)._is_some_details_page() is False
    assert _rp(_ui(*OP_DETAILS), RGB)._is_some_details_page() is True


# ---- Pixel 9 Pro / stock Android ---------------------------------------------
PX_LIST = ("pixel-stock-device-list.xml", 2142)
PX_DETAILS = ("pixel-stock-device-details.xml", 2142)


def test_pixel_targets_a_device_gear_by_settings_button():
    gear = _rp(_ui(*PX_LIST), "Demo Headset A")._find_device_gear("Demo Headset A")
    assert gear is not None and gear[0] > 700  # right-edge settings_button gear


def test_pixel_device_not_on_first_screen_returns_none():
    # RGB lives behind "See all" on stock Android, so it must NOT be found on this screen
    # (and must not mis-resolve to some other device's gear).
    assert _rp(_ui(*PX_LIST), RGB)._find_device_gear(RGB) is None


def test_pixel_details_gate_rejects_the_wrong_device():
    ui = _ui(*PX_DETAILS)  # a NON-RGB device's details page
    assert _rp(ui, "Demo Headset A")._details_page_for("Demo Headset A") is True
    # SAFETY: opening the wrong device's details must never pass the RGB gate.
    assert _rp(ui, RGB)._details_page_for(RGB) is False


def test_pixel_list_is_not_a_details_page_but_details_is():
    assert _rp(_ui(*PX_LIST), RGB)._is_some_details_page() is False
    assert _rp(_ui(*PX_DETAILS), RGB)._is_some_details_page() is True


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
