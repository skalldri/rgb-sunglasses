#pragma once

#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/bt_service.h>

/**
 * @brief A CRTP class which can be inherited by other classes to provide an "IsActive" Bluetooth Characteristic,
 * of the Animation Service.
 * 
 * @tparam T the class which has inherited us. Must provide a getInstance() function (which is provided by anything that inherits BaseAnimationTemplate)
 */
template <class T>
class IsActiveCharacteristic {

public:
    // Bluetooth callback to change the notification state of the isActive characteristic
    static void isActiveCccCfgChanged(const struct bt_gatt_attr *attr, uint16_t value)
    {
        T::getInstance()->sendActiveNotifications_ = (value == BT_GATT_CCC_NOTIFY);
        printk("Anim %d isActive notification state: %d\n", T::kBtServiceIdNum, T::getInstance()->sendActiveNotifications_);

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
            printk("Err in bt service %d\n", T::kBtServiceIdNum);
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
        }

        if (len > sizeof(active_))
        {
            printk("Err in bt service %d, incorrect len %d\n", T::kBtServiceIdNum, len);
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }

        // Set the active property
        T::getInstance()->active_ = *reinterpret_cast<const bool *>(buf);

        // Call the notification function letting local software know that the property was remotely changed
        T::getInstance()->onRemoteActiveChange(T::getInstance()->active_);

        return len;
    }

    // Set the active state from a local source, triggering a remote notification if needed
    void setIsActiveState(bool active) {
        active_ = active;
        T::btNotifyIfEnabled();
    }

    virtual void onRemoteActiveChange(bool active) {
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
#define BT_SVC_IS_ACTIVE_CHRC_DEFINE(_bt_service_class) \
    __BT_SVC_CHRC_DEFINE(is_active_ ## _bt_service_class, _bt_service_class::kBtServiceIdNum, BT_SVC_IS_ACTIVE_CHAR, BLE_GATT_CPF_FORMAT_BOOLEAN)

// Used to reference pre-defined glue that indicates a class supports the IsActive<> service
#define BT_SVC_IS_ACTIVE_CHRC_REFERENCE(_bt_service_class) \
    BT_SVC_READ_WRITE_NOTIFY_CHRC_REFERENCE(is_active_ ## _bt_service_class, "Is Active", _bt_service_class::readIsActive, _bt_service_class::writeIsActive, _bt_service_class::isActiveCccCfgChanged)

