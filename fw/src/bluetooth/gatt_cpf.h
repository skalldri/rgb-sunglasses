#pragma once

// Possible values for the Characteristic Presentation Format attribute
// https://developer.nordicsemi.com/nRF5_SDK/nRF51_SDK_v4.x.x/doc/html/group___b_l_e___g_a_t_t___c_p_f___f_o_r_m_a_t_s.html

#define BLE_GATT_CPF_FORMAT_RFU 0x00
#define BLE_GATT_CPF_FORMAT_BOOLEAN 0x01
#define BLE_GATT_CPF_FORMAT_2BIT 0x02
#define BLE_GATT_CPF_FORMAT_NIBBLE 0x03
#define BLE_GATT_CPF_FORMAT_UINT8 0x04
#define BLE_GATT_CPF_FORMAT_UINT12 0x05
#define BLE_GATT_CPF_FORMAT_UINT16 0x06
#define BLE_GATT_CPF_FORMAT_UINT24 0x07
#define BLE_GATT_CPF_FORMAT_UINT32 0x08
#define BLE_GATT_CPF_FORMAT_UINT48 0x09
#define BLE_GATT_CPF_FORMAT_UINT64 0x0A
#define BLE_GATT_CPF_FORMAT_UINT128 0x0B
#define BLE_GATT_CPF_FORMAT_SINT8 0x0C
#define BLE_GATT_CPF_FORMAT_SINT12 0x0D
#define BLE_GATT_CPF_FORMAT_SINT16 0x0E
#define BLE_GATT_CPF_FORMAT_SINT24 0x0F
#define BLE_GATT_CPF_FORMAT_SINT32 0x10
#define BLE_GATT_CPF_FORMAT_SINT48 0x11
#define BLE_GATT_CPF_FORMAT_SINT64 0x12
#define BLE_GATT_CPF_FORMAT_SINT128 0x13
#define BLE_GATT_CPF_FORMAT_FLOAT32 0x14
#define BLE_GATT_CPF_FORMAT_FLOAT64 0x15
#define BLE_GATT_CPF_FORMAT_SFLOAT 0x16
#define BLE_GATT_CPF_FORMAT_FLOAT 0x17
#define BLE_GATT_CPF_FORMAT_DUINT16 0x18
#define BLE_GATT_CPF_FORMAT_UTF8S 0x19
#define BLE_GATT_CPF_FORMAT_UTF16S 0x1A
#define BLE_GATT_CPF_FORMAT_STRUCT 0x1B

// Extra CPF values, specific to RGB Sunglasses project
#define BLE_GATT_CPF_FORMAT_RGB888 0xE0  // 3 bytes: R, G, B

// \n-separated list of valid options as a string; the currently-selected option is always
// listed first. Write the bare text of one of the listed options (no separators) to select it.
// Instructs the app to render a drop-down picker instead of a free-text/numeric input.
#define BLE_GATT_CPF_FORMAT_DROPDOWN_LIST 0xE1

// Generic "slot machine" contract (issue #260): a service exposing characteristics with the
// three formats below gets a playlist-style UI in the app — one row per slot with a
// tap-to-queue button and a "now playing" highlight — instead of raw text/number inputs.
//
// A slot's value, as a writable UTF8S-style string. The app derives each slot's index from
// the 0-based order of appearance of the SLOT_TEXT characteristics within their service
// (GATT declaration order == ascending handle order == metadata-blob entry order); that
// ordinal is the index the SLOT_UP_NEXT / SLOT_NOW_PLAYING characteristics refer to.
#define BLE_GATT_CPF_FORMAT_SLOT_TEXT 0xE2

// uint32 (little-endian), read/write/notify: the slot index that will play next. Writing k
// queues slot k; the device notifies the new value whenever it advances on its own.
#define BLE_GATT_CPF_FORMAT_SLOT_UP_NEXT 0xE3

// uint32 (little-endian), read-only/notify: the slot index currently playing, notified on
// each advance. Rendered by the app as a highlight on the matching slot row, not as its own
// input row.
#define BLE_GATT_CPF_FORMAT_SLOT_NOW_PLAYING 0xE4