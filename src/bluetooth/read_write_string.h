#pragma once

#include <bluetooth/bt_service.h>
#include <zephyr/bluetooth/gatt.h>

static ssize_t _writeString(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset,
                            uint8_t flags, size_t animationId, size_t characteristicId, char *str, size_t maxLen)
{
    printk("WR STR! l=%d, o=%d, f=%d\n", len, offset, flags);

    if (flags & BT_GATT_WRITE_FLAG_PREPARE)
    {
        printk("Anim %d, Chrc %d: write prepare\n", animationId, characteristicId);
        /* Return 0 to allow long writes */
        return 0;
    }

    if (len >= maxLen)
    {
        printk("Anim %d, Chrc %d, l %d, ml %d: error, too long\n", animationId, characteristicId, len, maxLen);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    // Should this be >= ?
    // Do we need to reserve a byte for the null terminator?
    if (offset + len > maxLen)
    {
        printk("Anim %d, Chrc %d: error, o %d, l %d, ml %d\n", animationId, characteristicId, offset, len, maxLen);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(str + offset, buf, len);
    str[len] = '\0'; // Ensure string is always null terminated

    // printk("Animation %d, Chrc %d: string updated from BT to '%s'\n", tAnimationId, tChrcId, getInstance().str_);

    return len;
}

/**
 * @brief
 *
 * @tparam animationId
 * @tparam chrcId
 */
template <size_t tAnimationId, size_t tChrcId, size_t tMaxLen>
class BtReadWriteString
{
public:
    // TODO: if we ever make this locally modifyable, sent BT notifications as needed

    operator const char *()
    {
        return getInstance().str_;
    }

    static BtReadWriteString<tAnimationId, tChrcId, tMaxLen> &getInstance()
    {
        static BtReadWriteString<tAnimationId, tChrcId, tMaxLen> inst;
        return inst;
    }

    static ssize_t writeString(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset,
                               uint8_t flags)
    {
        return _writeString(conn, attr, buf, len, offset, flags, tAnimationId, tChrcId, getInstance().str_, tMaxLen);
    }

    static ssize_t readString(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
    {
        // printk("Animation %d, Chrc %d: Read String '%s'\n", tAnimationId, tChrcId, getInstance().str_);
        return bt_gatt_attr_read(conn, attr, buf, len, offset, &(getInstance().str_), strlen(getInstance().str_));
    }

    void setValue(const char *val)
    {
        strncpy(str_, val, tMaxLen);
    }

    static void isActiveCccCfgChanged(const struct bt_gatt_attr *attr, uint16_t value)
    {
        getInstance().sendActiveNotifications_ = (value == BT_GATT_CCC_NOTIFY);
        // printk("Anim %d chrc %d notification state: %d\n", tAnimationId, tChrcId, getInstance().sendActiveNotifications_);

        if (getInstance().sendActiveNotifications_)
        {
            getInstance().activeAttr_ = attr;
        }
        else
        {
            getInstance().activeAttr_ = NULL;
        }
    }

protected:
    // Make constructor private so we enforce singleton usage
    BtReadWriteString()
    {
        memset(str_, 0, tMaxLen);
    }

    char str_[tMaxLen];

    bool sendActiveNotifications_ = false;
    const struct bt_gatt_attr *activeAttr_ = NULL;
};

#define BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(_bt_service_class, _char_num, _max_len)                                                   \
    using _bt_service_class##_char_num##_ReadWriteString = BtReadWriteString<_bt_service_class::kBtServiceIdNum, _char_num, _max_len>; \
    BT_SVC_CHRC_DEFINE(read_write_string_##_bt_service_class##_char_num, _bt_service_class::kBtServiceIdNum, _char_num, BLE_GATT_CPF_FORMAT_UTF8S);

// Reference a previously declared characteristic
#define BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(_bt_service_class, _char_num, _desc) \
    BT_SVC_READ_WRITE_NOTIFY_CHRC_REFERENCE(                                         \
        read_write_string_##_bt_service_class##_char_num,                            \
        _desc,                                                                       \
        _bt_service_class##_char_num##_ReadWriteString::readString,                  \
        _bt_service_class##_char_num##_ReadWriteString::writeString,                 \
        _bt_service_class##_char_num##_ReadWriteString::isActiveCccCfgChanged)
