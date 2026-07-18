// Verifies BtGattNotifyTraits<BtGattDropdownList<N>>::length() caps the notify payload to a
// length that always fits a single ATT_HANDLE_VALUE_NTF PDU, even on a connection stuck at the
// BLE-spec default/minimum ATT_MTU of 23 octets (3-byte ATT header + this length must be <= 23).
// Regression coverage for the "Notify failed: -12" / "No ATT channel for MTU 23" symptom seen
// when a GLIM Selection notify's first-token (selected filename) length exceeded that floor.
// See fw/CLAUDE.md ("notify() only sends the actual string length...") for the full history.

#include <bluetooth/bt_gatt_traits.h>
#include <zephyr/ztest.h>

#include <cstring>

namespace {

// Builds a BtGattDropdownList<N> whose backing buffer is exactly `canonical` (already in the
// wire "Selected\nOther\nOther2..." form used by buildGlimSelectionValue()).
template <size_t N>
BtGattDropdownList<N> makeList(const char *canonical) {
    BtGattDropdownList<N> list{};
    size_t len = strlen(canonical);
    zassert_true(len < N, "test fixture value too long for BtGattDropdownList<%zu>", N);
    memcpy(list.value.data(), canonical, len);
    list.value[len] = '\0';
    return list;
}

}  // namespace

ZTEST(gatt_notify_traits, test_short_selected_name_not_truncated) {
    // "nyan_cat.glim" (13 bytes) is well under the 20-byte guaranteed-safe floor - notify()
    // should send it in full, matching the pre-cap behavior for realistic short filenames.
    auto list = makeList<64>("nyan_cat.glim\nbad_apple.glim");

    zassert_equal(BtGattNotifyTraits<BtGattDropdownList<64>>::length(list), strlen("nyan_cat.glim"),
                  "short selected name should be sent uncapped");
}

ZTEST(gatt_notify_traits, test_long_selected_name_capped_to_guaranteed_safe_len) {
    // A 31-char filename (kMaxNameLen (32, glim_registry.h) - 1 for the terminating NUL) is
    // exactly the longest first token buildGlimSelectionValue() can ever produce. Before the
    // fix, notify() would try to send all 31 bytes; at the BLE-spec default ATT_MTU of 23
    // (20 usable payload bytes after the 3-byte ATT header), that always failed with -ENOMEM
    // ("No ATT channel for MTU 23" from the Zephyr host) and the app never got the "list
    // changed, go re-read" signal.
    const char *longName = "this_is_a_31_char_filename.glim";
    zassert_equal(strlen(longName), 31, "test fixture drifted from the 31-char case it targets");

    auto list = makeList<64>(longName);

    size_t notifyLen = BtGattNotifyTraits<BtGattDropdownList<64>>::length(list);
    zassert_equal(notifyLen, 20, "long selected name must be capped to the guaranteed-safe length");
    // The invariant that actually matters: payload + 3-byte ATT_HANDLE_VALUE_NTF header (1
    // opcode + 2-byte handle) must fit in the BLE-spec default/minimum ATT_MTU of 23 octets,
    // which Zephyr enforces as a hard floor (BT_ATT_DEFAULT_LE_MTU in att_internal.h; an MTU
    // Exchange requesting less is rejected outright) - so this is safe on ANY connected,
    // encrypted ATT bearer, negotiated or not.
    zassert_true(notifyLen + 3 <= 23, "notify payload + ATT header must fit the guaranteed floor");
}

ZTEST(gatt_notify_traits, test_multi_option_list_uses_first_token_not_whole_list) {
    // Regression guard for the ORIGINAL bug this trait fixed (see fw/CLAUDE.md): notify() must
    // key off the first "\n"-delimited token (the current selection), never the full
    // "Selected\nOther\nOther2..." canonical list, regardless of how many options exist.
    auto list = makeList<512>("a.glim\nb.glim\nc.glim\nd.glim\ne.glim");

    zassert_equal(BtGattNotifyTraits<BtGattDropdownList<512>>::length(list), strlen("a.glim"),
                  "notify length must be the first token only, not the whole list");
}

ZTEST(gatt_notify_traits, test_string_trait_unaffected_by_dropdown_cap) {
    // The 20-byte cap is specific to BtGattDropdownList<N> (see BtGattNotifyTraits
    // specialization) - confirms the generic BtGattString<N> path (used by plain string
    // characteristics) is untouched and still reports the full strnlen-based length.
    BtGattString<64> value = {};
    const char *longValue = "a value longer than twenty bytes for sure";
    zassert_true(strlen(longValue) > 20, "test fixture must exceed 20 bytes to be meaningful");
    memcpy(value.data(), longValue, strlen(longValue) + 1);

    zassert_equal(BtGattNotifyTraits<BtGattString<64>>::length(value), strlen(longValue),
                  "plain BtGattString<N> notify length must not be capped");
}

ZTEST_SUITE(gatt_notify_traits, NULL, NULL, NULL, NULL, NULL);
