#pragma once

#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/gatt_cpf.h>

#include <array>
#include <cstddef>
#include <cstdint>

template <size_t N>
using BtGattString = std::array<char, N>;

template <typename T>
struct BtGattStringTraits
{
    static constexpr bool kIsString = false;
};

template <size_t N>
struct BtGattStringTraits<BtGattString<N>>
{
    static constexpr bool kIsString = true;
    static constexpr size_t kMaxLen = N;
};

struct BtGattColor
{
    constexpr BtGattColor() = default;
    constexpr BtGattColor(uint32_t raw)
        : value(raw)
    {
    }

    constexpr operator uint32_t() const
    {
        return value;
    }

    uint32_t value = 0;
};

template <typename T>
struct BtGattCpfTraits
{
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
struct BtGattCpfTraits<bool>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_BOOLEAN,
    };
};

template <>
struct BtGattCpfTraits<uint8_t>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_UINT8,
    };
};

template <>
struct BtGattCpfTraits<uint16_t>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_UINT16,
    };
};

template <>
struct BtGattCpfTraits<uint32_t>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_UINT32,
    };
};

template <>
struct BtGattCpfTraits<uint64_t>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_UINT64,
    };
};

template <>
struct BtGattCpfTraits<int8_t>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_SINT8,
    };
};

template <>
struct BtGattCpfTraits<int16_t>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_SINT16,
    };
};

template <>
struct BtGattCpfTraits<int32_t>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_SINT32,
    };
};

template <>
struct BtGattCpfTraits<int64_t>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_SINT64,
    };
};

template <>
struct BtGattCpfTraits<float>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_FLOAT32,
    };
};

template <>
struct BtGattCpfTraits<double>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_FLOAT64,
    };
};

template <size_t N>
struct BtGattCpfTraits<BtGattString<N>>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_UTF8S,
    };
};

template <>
struct BtGattCpfTraits<BtGattColor>
{
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_RGB888,
    };
};
