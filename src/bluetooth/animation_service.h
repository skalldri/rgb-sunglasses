#pragma once

#include <zephyr/bluetooth/gatt.h>

// Don't use __* macros externally of this file

// Define a UUID for a Characteristic that is part of an animation service instance that can later be referenced
#define __ANIM_SVC_CHRC_UUID(_name, _animation_number, _char_num) \
    static struct bt_uuid_128 _name = BT_UUID_INIT_128( \
        BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, _animation_number, (0x56789abc0001 + _char_num)));

// Define a Characteristic Presentation Format structure that can later be referenced
#define __ANIM_SVC_CHRC_CPF(_name, _format) \
    static const struct bt_gatt_cpf _name = { \
        .format = _format, \
    };

#define __ANIM_SVC_CHRC_DEFINE(_prefix, _animation_number, _char_num, _format) \
    __ANIM_SVC_CHRC_UUID(_prefix ## _uuid, _animation_number, _char_num); \
    __ANIM_SVC_CHRC_CPF(_prefix ## _cpf, _format);


#define __ANIM_SVC_STRING_CHRC_DEFINE_WRITE_FUNC(_func_name, _var_name, _var_size) \
    static ssize_t _func_name(struct bt_conn *conn, const struct bt_gatt_attr *attr, \
              const void *buf, uint16_t len, uint16_t offset, \
              uint8_t flags) \
    { \
            if (offset) { \
                return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET); \
            } \
            if (len >= _var_size) { \
                return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN); \
            } \
            memcpy(_var_name, buf, len); \
            _var_name[len] = '\0'; \
            return len;\
    }

#define __ANIM_SVC_BOOL_CHRC_DEFINE_READ_FUNC(_func_name, _var_name) \
    static ssize_t _func_name(struct bt_conn *conn, const struct bt_gatt_attr *attr, \
             void *buf, uint16_t len, uint16_t offset) \
    { \
        return bt_gatt_attr_read(conn, attr, buf, len, offset, &(_var_name), sizeof(_var_name)); \
    }

// Special reserved characteristic IDs
#define ANIM_SVC_RSVD_CHAR_ID_START 0xf000

// Special default animation service handlers
#define ANIM_SVC_IS_ACTIVE_CHAR (ANIM_SVC_RSVD_CHAR_ID_START + 1)

// Define an instance of the animation service, with a specified animation number
#define ANIM_SVC_UUID(_name, _animation_number) \
    static struct bt_uuid_128 _name = BT_UUID_INIT_128( \
        BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, _animation_number, 0x56789abc0000));

// Define an characteristic which is part of an instance of the animation service
#define ANIM_SVC_CHRC_DEFINE(_prefix, _animation_number, _char_num, _format) \
    static_assert(_char_num < ANIM_SVC_RSVD_CHAR_ID_START); \
    __ANIM_SVC_CHRC_DEFINE(_prefix, _animation_number, _char_num, _format);

#define ANIM_SVC_WRITEABLE_STRING_CHRC_DEFINE(_prefix, _animation_number, _char_num, _str_len) \
    static char _prefix ## _storage[_str_len]; \
    __ANIM_SVC_STRING_CHRC_DEFINE_WRITE_FUNC(_prefix ## _write, _prefix ## _storage, _str_len); \
    ANIM_SVC_CHRC_DEFINE(_prefix, _animation_number, _char_num, BLE_GATT_CPF_FORMAT_UTF8S);

// Reference a previously declared writeable string characteristic
#define ANIM_SVC_WRITEABLE_STRING_CHRC_REFERENCE(_prefix, _desc) \
    BT_GATT_CHARACTERISTIC(&_prefix ## _uuid.uuid, \
                   BT_GATT_CHRC_WRITE, \
                   BT_GATT_PERM_WRITE_ENCRYPT, \
                   NULL, \
                   _prefix ## _write, \
                   NULL), \
    BT_GATT_CUD(_desc, BT_GATT_PERM_READ), \
    BT_GATT_CPF(&_prefix ## _cpf)

// Reference a boolean characteristic, and indicate to the peer it has read/write/notify capabilities
#define ANIM_SVC_READ_WRITE_NOTIFY_CHRC_REFERENCE(_prefix, _desc, _read_func, _write_func, _ccc_cfg_changed_func) \
    BT_GATT_CHARACTERISTIC(&_prefix ## _uuid.uuid, \
                   BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY, \
                   BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, \
                   _read_func, \
                   _write_func, \
                   NULL), \
    BT_GATT_CUD(_desc, BT_GATT_PERM_READ), \
    BT_GATT_CPF(&_prefix ## _cpf), \
    BT_GATT_CCC(_ccc_cfg_changed_func, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)

// Reference a string characteristic, and indicate to the peer it has read/write capabilities
#define ANIM_SVC_READ_WRITE_CHRC_REFERENCE(_prefix, _desc, _read_func, _write_func) \
    BT_GATT_CHARACTERISTIC(&_prefix ## _uuid.uuid, \
                   BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE, \
                   BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, \
                   _read_func, \
                   _write_func, \
                   NULL), \
    BT_GATT_CUD(_desc, BT_GATT_PERM_READ), \
    BT_GATT_CPF(&_prefix ## _cpf)
