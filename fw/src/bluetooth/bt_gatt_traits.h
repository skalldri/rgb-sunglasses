#pragma once

#include <bluetooth/gatt_cpf.h>
#include <zephyr/bluetooth/gatt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

template <size_t N>
using BtGattString = std::array<char, N>;

template <typename T>
struct BtGattStringTraits {
    static constexpr bool kIsString = false;
};

template <size_t N>
struct BtGattStringTraits<BtGattString<N>> {
    static constexpr bool kIsString = true;
    static constexpr size_t kMaxLen = N;
};

// Converts a C string literal into a null-terminated BtGattString<OutN>, for use as a
// characteristic's compile-time default value. Always leaves room for the terminating '\0',
// truncating the copied content (rather than clobbering the last already-copied byte) if the
// literal doesn't fit.
template <size_t OutN, size_t InN>
constexpr BtGattString<OutN> makeBtGattString(const char (&str)[InN]) {
    static_assert(OutN > 0, "makeBtGattString requires a non-empty output buffer");
    BtGattString<OutN> out = {};
    size_t copyLen = InN < OutN - 1 ? InN : OutN - 1;
    for (size_t i = 0; i < copyLen; i++) {
        out[i] = str[i];
    }
    out[copyLen] = '\0';
    return out;
}

// Wire-compatible string value for a "drop-down list" characteristic (see
// BLE_GATT_CPF_FORMAT_DROPDOWN_LIST in gatt_cpf.h): \n-separated valid options, selected
// option listed first. A distinct type (not just BtGattString<N>) so it can carry its own
// BtGattCpfTraits specialization below instead of UTF8S.
//
// Composition, not inheritance: std::array's comparison operators are free templates that
// deduce their template arguments from the exact argument type, so they would not bind to a
// type that merely derives from std::array. Forwarding .data()/operator[]/==/!= explicitly
// keeps this a drop-in fit for BtGattCharacteristicCommon's read()/write(), which only ever
// call those operations on the storage object for string-like types.
template <size_t N>
struct BtGattDropdownList {
    BtGattString<N> value{};

    constexpr char *data() { return value.data(); }
    constexpr const char *data() const { return value.data(); }
    constexpr char &operator[](size_t i) { return value[i]; }
    constexpr const char &operator[](size_t i) const { return value[i]; }
    constexpr bool operator==(const BtGattDropdownList &other) const { return value == other.value; }
    constexpr bool operator!=(const BtGattDropdownList &other) const { return value != other.value; }
};

template <size_t N>
struct BtGattStringTraits<BtGattDropdownList<N>> {
    static constexpr bool kIsString = true;
    static constexpr size_t kMaxLen = N;
};

// Controls how many bytes notify() actually transmits, independent of what read() returns.
// Defaults to mirroring read()'s behavior (the full string content for string-backed types via
// strnlen, or the whole fixed-size value otherwise) - safe for any type whose entire current
// value is exactly what remote clients need on every change.
//
// BtGattDropdownList<N> overrides this to notify only its first "\n"-delimited token (the
// currently-selected option), not the full "Selected\nOther\nOther2..." list: bt_gatt_notify()
// sends one ATT PDU bounded by the connection's negotiated MTU and cannot fragment like a long
// write/read can, so notifying the whole list makes notify cost (and failure risk) scale with
// the total option count instead of with what actually changed (just the selection). The full
// canonical list is still rebuilt and stored on every selection change - read() still returns
// it correctly - it's just not what gets pushed via notify. A client must treat any notification
// on a dropdown-list characteristic as "go re-read", not as the new value directly; see the
// matching app-side handling in use-ble-connection.ts.
template <typename T>
struct BtGattNotifyTraits {
    static size_t length(const T &value) {
        if constexpr (BtGattStringTraits<T>::kIsString) {
            return strnlen(value.data(), BtGattStringTraits<T>::kMaxLen);
        } else {
            return sizeof(value);
        }
    }
};

template <size_t N>
struct BtGattNotifyTraits<BtGattDropdownList<N>> {
    // Bounds the notify payload to a length that's guaranteed to fit in a single ATT
    // Handle-Value Notification PDU on ANY connected, encrypted ATT bearer - negotiated or
    // not. The Bluetooth Core Spec fixes the default/minimum LE ATT_MTU at 23 octets (Vol 3,
    // Part F, 3.2.8); Zephyr's own host enforces this as a floor (BT_ATT_DEFAULT_LE_MTU == 23
    // in subsys/bluetooth/host/att_internal.h, and an MTU Exchange requesting less is rejected
    // in att.c). 3 of those 23 bytes are always consumed by the ATT_HANDLE_VALUE_NTF header (1
    // opcode + 2-byte handle), leaving 20 payload bytes that are safe unconditionally.
    //
    // Without this cap, a selected GLIM filename's first token (kMaxNameLen == 32, see
    // glim_registry.h) can exceed that 20-byte floor. When the connection hasn't negotiated a
    // larger MTU yet - or is stuck at the default because of a stale Android GATT cache after
    // a GATT-layout-changing OTA (issue #115) - bt_gatt_notify() then fails outright with
    // -ENOMEM (logged by the host as "No ATT channel for MTU ..."), and the notification is
    // silently dropped. Since the app-side contract for dropdown-list characteristics is
    // already "any notification means go re-read the canonical value" (it never trusts the
    // notified bytes directly - see the DROPDOWN_LIST handling in
    // app/hooks/use-ble-connection.ts), truncating the preview sent here costs nothing
    // functionally in exchange for a notify that can never fail to fit.
    static constexpr size_t kGuaranteedSafeNotifyLen = 20;

    static size_t length(const BtGattDropdownList<N> &value) {
        const char *data = value.value.data();
        const char *newline = static_cast<const char *>(memchr(data, '\n', N));
        size_t tokenLen =
            newline != nullptr ? static_cast<size_t>(newline - data) : strnlen(data, N);
        return std::min(tokenLen, kGuaranteedSafeNotifyLen);
    }
};

struct BtGattColor {
    constexpr BtGattColor() = default;
    constexpr BtGattColor(uint32_t raw) : value(raw) {}

    constexpr operator uint32_t() const { return value; }

    uint32_t value = 0;
};

template <typename T>
struct BtGattCpfTraits {
    static constexpr bool kSupported = false;
};

// Override path for unsupported/custom types:
// template <>
// struct BtGattCpfTraits<MyType>
// {
//     static constexpr bool kSupported = true;
//     static constexpr bt_gatt_cpf kValue = {
//         .format = BLE_GATT_CPF_FORMAT_STRUCT,
//     };
// };

template <>
struct BtGattCpfTraits<bool> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_BOOLEAN,
    };
};

template <>
struct BtGattCpfTraits<uint8_t> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_UINT8,
    };
};

template <>
struct BtGattCpfTraits<uint16_t> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_UINT16,
    };
};

template <>
struct BtGattCpfTraits<uint32_t> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_UINT32,
    };
};

template <>
struct BtGattCpfTraits<uint64_t> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_UINT64,
    };
};

template <>
struct BtGattCpfTraits<int8_t> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_SINT8,
    };
};

template <>
struct BtGattCpfTraits<int16_t> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_SINT16,
    };
};

template <>
struct BtGattCpfTraits<int32_t> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_SINT32,
    };
};

template <>
struct BtGattCpfTraits<int64_t> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_SINT64,
    };
};

template <>
struct BtGattCpfTraits<float> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_FLOAT32,
    };
};

template <>
struct BtGattCpfTraits<double> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_FLOAT64,
    };
};

template <size_t N>
struct BtGattCpfTraits<BtGattString<N>> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_UTF8S,
    };
};

template <>
struct BtGattCpfTraits<BtGattColor> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_RGB888,
    };
};

template <size_t N>
struct BtGattCpfTraits<BtGattDropdownList<N>> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_DROPDOWN_LIST,
    };
};
