#pragma once

#include <bluetooth/gatt_cpf.h>
#include <bluetooth/bt_service.h>

template <typename T, size_t tAnimationId, size_t tChrcId, uint8_t format, T def>
class BtReadWriteVariable
{
    public:
    static constexpr uint8_t kCpfFormat = format;

    // Allow variable assignment
    T& operator=(const T& other) {
        getInstance().storage_ = other;
        getInstance().btNotifyIfEnabled();
        return getInstance().storage_;
    }

    // Allow casting to our underlying instance
    operator T() {
        return getInstance().storage_;
    }

    static BtReadWriteVariable<T, tAnimationId, tChrcId, format, def>& getInstance() {
        static BtReadWriteVariable<T, tAnimationId, tChrcId, format, def> inst;
        return inst;
    }

    static ssize_t write(struct bt_conn *conn, const struct bt_gatt_attr *attr, 
            const void *buf, uint16_t len, uint16_t offset, 
            uint8_t flags) 
    { 
            if (offset) { 
                printk("Animation %d, Chrc %d: error, offset\n", tAnimationId, tChrcId);
                return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET); 
            } 
            if (len > sizeof(T)) {
                printk("Animation %d, Chrc %d: error, too long: %d > %d\n", tAnimationId, tChrcId, len, sizeof(T));
                return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN); 
            } 
            memcpy(&(getInstance().storage_), buf, len);

            printk("Animation %d, Chrc %d: value updated over BT to '%d'\n", tAnimationId, tChrcId, getInstance().storage_);

            return len;
    }

    static ssize_t read(struct bt_conn *conn, const struct bt_gatt_attr *attr, 
             void *buf, uint16_t len, uint16_t offset) 
    { 
        printk("Animation %d, Chrc %d: Reading value '%d'\n", tAnimationId, tChrcId, getInstance().storage_);
        return bt_gatt_attr_read(conn, attr, buf, len, offset, &(getInstance().storage_), sizeof(getInstance().storage_)); 
    }

    static void isActiveCccCfgChanged(const struct bt_gatt_attr *attr, uint16_t value)
    {
        getInstance().sendActiveNotifications_ = (value == BT_GATT_CCC_NOTIFY);
        printk("Anim %d chrc %d notification state: %d\n", tAnimationId, tChrcId, getInstance().sendActiveNotifications_);

        if (getInstance().sendActiveNotifications_) {
            getInstance().activeAttr_ = attr;
        } else {
            getInstance().activeAttr_ = NULL;
        }
    }

    protected:
        static void btNotifyIfEnabled() {
            if (getInstance().activeAttr_) {
                printk("Anim %d chrc %d notifying on value change\n", tAnimationId, tChrcId);
                if (bt_gatt_notify(NULL, getInstance().activeAttr_, &getInstance().storage_, sizeof(getInstance().storage_)) != 0) {
                    printk("Anim %d chrc %d notify failed\n", tAnimationId, tChrcId);
                }
            }
        }

        BtReadWriteVariable() = default;

        T storage_ = def;

        bool sendActiveNotifications_ = false;
        const struct bt_gatt_attr *activeAttr_ = NULL;
};

template <size_t tAnimationId, size_t tChrcId, bool def = false>
using BtReadWrite_bool = BtReadWriteVariable<bool, tAnimationId, tChrcId, BLE_GATT_CPF_FORMAT_BOOLEAN, def>;

template <size_t tAnimationId, size_t tChrcId, uint8_t def = 0>
using BtReadWrite_uint8_t = BtReadWriteVariable<uint8_t, tAnimationId, tChrcId, BLE_GATT_CPF_FORMAT_UINT8, def>;

template <size_t tAnimationId, size_t tChrcId, uint16_t def = 0>
using BtReadWrite_uint16_t = BtReadWriteVariable<uint16_t, tAnimationId, tChrcId, BLE_GATT_CPF_FORMAT_UINT16, def>;

template <size_t tAnimationId, size_t tChrcId, uint32_t def = 0>
using BtReadWrite_uint32_t = BtReadWriteVariable<uint32_t, tAnimationId, tChrcId, BLE_GATT_CPF_FORMAT_UINT32, def>;

template <size_t tAnimationId, size_t tChrcId, int8_t def = 0>
using BtReadWrite_int8_t = BtReadWriteVariable<int8_t, tAnimationId, tChrcId, BLE_GATT_CPF_FORMAT_SINT8, def>;

template <size_t tAnimationId, size_t tChrcId, int16_t def = 0>
using BtReadWrite_int16_t = BtReadWriteVariable<int16_t, tAnimationId, tChrcId, BLE_GATT_CPF_FORMAT_SINT16, def>;

template <size_t tAnimationId, size_t tChrcId, int32_t def = 0>
using BtReadWrite_int32_t = BtReadWriteVariable<int32_t, tAnimationId, tChrcId, BLE_GATT_CPF_FORMAT_SINT32, def>;


#define BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(_bt_service_class, _char_num, _type, _initial_value) \
    BtReadWrite_ ## _type<_bt_service_class::kBtServiceIdNum, _char_num, _initial_value>; \
    using _bt_service_class ## _char_num ## _ReadWrite = BtReadWrite_ ## _type<_bt_service_class::kBtServiceIdNum, _char_num, _initial_value>; \
    BT_SVC_CHRC_DEFINE(read_write_ ## _bt_service_class ## _char_num, _bt_service_class::kBtServiceIdNum, _char_num, _bt_service_class ## _char_num ## _ReadWrite::kCpfFormat);

// Reference a previously declared characteristic
#define BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(_bt_service_class, _char_num, _desc) \
    BT_SVC_READ_WRITE_NOTIFY_CHRC_REFERENCE( \
        read_write_ ## _bt_service_class ## _char_num, \
        _desc, \
        _bt_service_class ## _char_num ## _ReadWrite::read, \
        _bt_service_class ## _char_num ## _ReadWrite::write, \
        _bt_service_class ## _char_num ## _ReadWrite::isActiveCccCfgChanged)
