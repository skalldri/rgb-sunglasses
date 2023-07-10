#pragma once

#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/animation_service.h>

/**
 * @brief A CRTP class which can be inherited by other classes to provide an "IsActive" Bluetooth Characteristic,
 * of the Animation Service.
 * 
 * @tparam T the class which has inherited us. Must provide a getInstance() function (which is provided by anything that inherits BaseAnimationTemplate)
 */
template <class T>
class IsActiveService {

public:
    // Bluetooth callback to change the notification state of the isActive characteristic
    static void isActiveCccCfgChanged(const struct bt_gatt_attr *attr, uint16_t value)
    {
        T::getInstance()->sendActiveNotifications_ = (value == BT_GATT_CCC_NOTIFY);
        printk("Anim %d isActive notification state: %d\n", T::kAnimationIdNum, T::getInstance()->sendActiveNotifications_);

        if (T::getInstance()->sendActiveNotifications_) {
            T::getInstance()->activeAttr_ = attr;
        } else {
            T::getInstance()->activeAttr_ = NULL;
        }
    }

    // Bluetooth callback to read the isActive state
    static ssize_t readIsActive(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset)
    {
        return bt_gatt_attr_read(conn, attr, buf, len, offset, &T::getInstance()->active_,
                                 sizeof(T::getInstance()->active_));
    }

    // Bluetooth callback to write the "isActive" state
    static ssize_t writeIsActive(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset,
                                 uint8_t flags)
    {
        if (offset)
        {
            printk("Err in anim %d\n", T::kAnimationIdNum);
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
        }

        if (len > sizeof(active_))
        {
            printk("Err in anim %d, incorrect len %d\n", T::kAnimationIdNum, len);
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }

        T::getInstance()->setActive(*reinterpret_cast<const bool *>(buf));

        return len;
    }

    static void setActive(bool active) {
        T::getInstance()->active_ = active;
        T::btNotifyIfEnabled();
        T::getInstance()->onActiveChange();
    }

    virtual void onActiveChange() {
        // Do nothing, allows overrides
    }

protected:
    static void btNotifyIfEnabled() {
        if (T::getInstance()->activeAttr_) {
            bt_gatt_notify(NULL, T::getInstance()->activeAttr_, &T::getInstance()->active_, sizeof(T::getInstance()->active_));
        }
    }

    bool active_ = false;
    bool sendActiveNotifications_ = false;
    const struct bt_gatt_attr *activeAttr_ = NULL;
};

// Used to define that a class as supports the IsActive<> service, and register the relevant bluetooth GATT glue
#define ANIM_SVC_IS_ACTIVE_CHRC_DEFINE(_animation_class) \
    __ANIM_SVC_CHRC_DEFINE(is_active_ ## _animation_class, _animation_class::kAnimationIdNum, ANIM_SVC_IS_ACTIVE_CHAR, BLE_GATT_CPF_FORMAT_BOOLEAN)

// Used to reference pre-defined glue that indicates a class supports the IsActive<> service
#define ANIM_SVC_IS_ACTIVE_CHRC_REFERENCE(_animation_class) \
    ANIM_SVC_READ_WRITE_NOTIFY_CHRC_REFERENCE(is_active_ ## _animation_class, "Is Active", _animation_class::readIsActive, _animation_class::writeIsActive, _animation_class::isActiveCccCfgChanged)

