#pragma once

#include <bluetooth/animation_service.h>
#include <zephyr/bluetooth/gatt.h>

/**
 * @brief 
 * 
 * @tparam animationId 
 * @tparam chrcId 
 */
template<size_t tAnimationId, size_t tChrcId>
class BluetoothReadWriteableString {
public:
    static constexpr size_t kMaxLen = 255;

    static BluetoothReadWriteableString<tAnimationId, tChrcId>* getInstance() {
        static BluetoothReadWriteableString<tAnimationId, tChrcId> inst;
        return &inst;
    }

    static ssize_t writeString(struct bt_conn *conn, const struct bt_gatt_attr *attr, 
              const void *buf, uint16_t len, uint16_t offset, 
              uint8_t flags) 
    { 
            if (offset) { 
                printk("Animation %d, Chrc %d: error, offset\n", tAnimationId, tChrcId);
                return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET); 
            } 
            if (len >= kMaxLen) {
                printk("Animation %d, Chrc %d: error, too long\n", tAnimationId, tChrcId);
                return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN); 
            } 
            memcpy(getInstance()->str_, buf, len); 
            getInstance()->str_[len] = '\0'; 

            printk("Animation %d, Chrc %d: string updated from BT to '%s'\n", tAnimationId, tChrcId, getInstance()->str_);

            return len;
    }

    static ssize_t readString(struct bt_conn *conn, const struct bt_gatt_attr *attr, 
             void *buf, uint16_t len, uint16_t offset) 
    { 
        printk("Animation %d, Chrc %d: Read String '%s'\n", tAnimationId, tChrcId, getInstance()->str_);
        return bt_gatt_attr_read(conn, attr, buf, len, offset, &(getInstance()->str_), strlen(getInstance()->str_)); 
    }

    void setValue(const char* val) {
        strncpy(str_, val, kMaxLen);
    }

    protected:
        // Make constructor private so we enforce singleton usage
        BluetoothReadWriteableString() {
            memset(str_, 0, kMaxLen);
        }

        char str_[kMaxLen];
};

#define ANIM_SVC_READ_WRITE_STRING_CHRC_DEFINE(_animation_class, _char_num) \
    using _animation_class ## _char_num ## _ReadWriteString = BluetoothReadWriteableString<_animation_class::kAnimationIdNum, _char_num>; \
    ANIM_SVC_CHRC_DEFINE(read_write_string_ ## _animation_class ## _char_num, _animation_class::kAnimationIdNum, _char_num, BLE_GATT_CPF_FORMAT_UTF8S);

// Reference a previously declared characteristic
#define ANIM_SVC_READ_WRITE_STRING_CHRC_REFERENCE(_animation_class, _char_num, _desc) \
    ANIM_SVC_READ_WRITE_CHRC_REFERENCE( \
        read_write_string_ ## _animation_class ## _char_num, \
        _desc, \
        _animation_class ## _char_num ## _ReadWriteString::readString, \
        _animation_class ## _char_num ## _ReadWriteString::writeString)
