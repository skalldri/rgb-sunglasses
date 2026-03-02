#pragma once

#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/bt_gatt_traits.h>
#include <array>
#include <tuple>
#include <algorithm>
#include <type_traits>
#include <cstring>

constexpr bt_uuid_128 composeAutoCharacteristicUuid(const bt_uuid_128 &serviceUuid,
                                                    uint16_t characteristicId)
{
    bt_uuid_128 uuid = serviceUuid;
    uuid.val[0] = static_cast<uint8_t>(characteristicId & 0xFF);
    uuid.val[1] = static_cast<uint8_t>((characteristicId >> 8) & 0xFF);
    return uuid;
}

// Helper to check if all tuple elements are bt_gatt_attr
template <typename Tuple, size_t... Is>
constexpr bool allAttrsAreGattAttr(std::index_sequence<Is...>)
{
    return (std::is_same_v<std::tuple_element_t<Is, Tuple>, bt_gatt_attr> &&
            ...);
}

// Define a concept that ensures a provided type is compatible with
// the BtGattServer. Requirements are:
// - It must have a constexpr getAttrsTuple() const function, which produces a
// tuple of only bt_gatt_attr
// - getAttrsTuple() must return at least one attribute
template <typename T>
concept BtGattAttributeProvider = requires(T &t) {
    // Must have getAttrsTuple()
    { t.getAttrsTuple() };

    // The tuple must have at least one element
    requires(std::tuple_size_v<decltype(t.getAttrsTuple())> > 0);

    // All elements in the tuple must be bt_gatt_attr
    requires allAttrsAreGattAttr<decltype(t.getAttrsTuple())>(
        std::make_index_sequence<
            std::tuple_size_v<decltype(t.getAttrsTuple())>>{});
};

template <typename T>
concept BtGattServiceUuidProvider = requires(T &t) {
    t.getServiceUuid();
};

template <typename T>
concept BtGattAutoUuidAssignable = requires(T &t,
                                            const bt_uuid_128 &serviceUuid,
                                            uint16_t characteristicId) {
    t.assignAutoUuid(serviceUuid, characteristicId);
};

template <typename TInstance, typename TValue>
concept BtGattWriteHook = requires(TInstance &instance, const TValue &value) {
    instance.onWrite(value);
};

/**
 * @brief Base mixin for providers that need attribute-array back-references.
 *
 * Instances are non-owning: @ref bind stores a pointer to the server's flattened
 * attribute storage for later indexed access via @ref getAttr.
 */
class BtGattAttrProviderBase
{
public:
    void bind(bt_gatt_attr *base) { attrBase_ = base; }
    bt_gatt_attr *getAttr(size_t idx = 0) const { return attrBase_ + idx; }

protected:
    bt_gatt_attr *attrBase_ = nullptr;
};

// Helper to get the attribute count from a BtGattAttributeProvider
template <BtGattAttributeProvider T>
constexpr size_t attrCount()
{
    return std::tuple_size_v<decltype(std::declval<T>().getAttrsTuple())>;
}

// Helper to concatenate tuples from all providers
template <BtGattAttributeProvider... Providers>
constexpr auto concatenateAttrTuples(Providers &...providers)
{
    return std::tuple_cat(providers.getAttrsTuple()...);
}

// Helper to convert a tuple to an array
template <typename Tuple, size_t... Is>
constexpr auto tupleToArray(const Tuple &tuple, std::index_sequence<Is...>)
{
    return std::array<bt_gatt_attr, sizeof...(Is)>{{std::get<Is>(tuple)...}};
}

// Helper to calculate offset of provider N in the parameter pack
template <size_t N, typename... Providers>
constexpr size_t offsetOfProvider()
{
    if constexpr (N == 0)
    {
        return 0;
    }
    else
    {
        constexpr size_t counts[] = {
            std::tuple_size_v<decltype(std::declval<Providers>().getAttrsTuple())>...};
        size_t offset = 0;
        for (size_t i = 0; i < N; ++i)
        {
            offset += counts[i];
        }
        return offset;
    }
}

// Helper to bind all providers to their offsets in the array
template <size_t... Is, typename... Providers>
void bindProviders(bt_gatt_attr *base,
                   std::index_sequence<Is...>,
                   Providers &...providers)
{
    auto bindIfPossible = [&]<typename P>(P &provider, size_t offset)
    {
        if constexpr (std::is_base_of_v<BtGattAttrProviderBase, P>)
        {
            provider.bind(base + offset);
        }
    };

    (bindIfPossible(providers, offsetOfProvider<Is, Providers...>()), ...);
}

/**
 * @brief Compile-time GATT server assembler from attribute providers.
 *
 * This type consumes @ref BtGattAttributeProvider instances, assigns auto UUIDs
 * (for assignable providers) in provider order, then flattens all attributes into
 * one backing array and binds provider base pointers into that storage.
 */
template <BtGattAttributeProvider... Providers>
class BtGattServer
{
public:
    static constexpr size_t kServiceUuidProviderCount =
        (0 + ... + static_cast<size_t>(BtGattServiceUuidProvider<Providers>));

    // Deduce total count from all providers' tuples
    static constexpr size_t kTotalAttrCount =
        (std::tuple_size_v<
             decltype(std::declval<Providers>().getAttrsTuple())> +
         ...);

    BtGattServer(Providers &...providers)
    {
        static_assert(kServiceUuidProviderCount > 0,
                      "BtGattServer requires at least one provider with getServiceUuid().");

        bt_uuid_128 primaryServiceUuid{};
        bool hasPrimaryServiceUuid = false;

        auto findPrimaryServiceUuid = [&]<typename P>(P &provider)
        {
            if constexpr (BtGattServiceUuidProvider<P>)
            {
                if (!hasPrimaryServiceUuid)
                {
                    primaryServiceUuid = provider.getServiceUuid();
                    hasPrimaryServiceUuid = true;
                }
            }
        };

        (findPrimaryServiceUuid(providers), ...);

        uint16_t autoCharacteristicId = 0;
        auto assignAutoUuids = [&]<typename P>(P &provider)
        {
            if constexpr (BtGattAutoUuidAssignable<P>)
            {
                provider.assignAutoUuid(primaryServiceUuid, autoCharacteristicId);
                ++autoCharacteristicId;
            }
        };

        (assignAutoUuids(providers), ...);

        attrs_ = tupleToArray(concatenateAttrTuples(providers...),
                              std::make_index_sequence<kTotalAttrCount>{});

        // Bind providers that inherit from BtGattAttrProviderBase
        bindProviders(attrs_.data(),
                      std::index_sequence_for<Providers...>{},
                      providers...);
    }

    // Provide access to the attribute array for external registration
    constexpr bt_gatt_attr *data() { return attrs_.data(); }
    constexpr bt_gatt_attr *data() const { return attrs_.data(); }
    static constexpr size_t size() { return kTotalAttrCount; }

private:
    std::array<bt_gatt_attr, kTotalAttrCount> attrs_{};
};

#define BT_GATT_SERVER_REGISTER(_name, _server)                    \
    const STRUCT_SECTION_ITERABLE(bt_gatt_service_static, _name) = \
        {                                                          \
            .attrs = _server.data(),                               \
            .attr_count = _server.size(),                          \
    }

// Deduction guide to allow BtGattServer(provider1, provider2, ...) syntax
template <BtGattAttributeProvider... Providers>
BtGattServer(Providers &...) -> BtGattServer<Providers...>;

/// SPECIFIC GATT TYPES

/**
 * @brief Primary-service provider for @ref BtGattServer assembly.
 *
 * Participates in the provider concept by exposing a one-attribute tuple and the
 * service UUID. The UUID is copied into instance storage and owned by this object.
 */
template <bt_uuid_128 ServiceUuid>
class BtGattPrimaryService
{
public:
    constexpr bt_uuid_128 getServiceUuid() const
    {
        return service_uuid_;
    }

    constexpr auto getAttrsTuple()
    {
        return std::make_tuple(bt_gatt_attr(BT_GATT_PRIMARY_SERVICE(&service_uuid_)));
    }

private:
    bt_uuid_128 service_uuid_ = ServiceUuid;
};

// Helper function to encapsulate the common write logic for read/write characteristics
// Used to avoid exploding the binary size as the template class methods are instantiated multiple times.
static ssize_t _write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                      const void *buf, uint16_t len, uint16_t offset,
                      uint8_t flags, std::byte *out, size_t maxLen)
{
    printk("WR! l=%d, o=%d, f=%d\n", len, offset, flags);

    if (flags & BT_GATT_WRITE_FLAG_PREPARE)
    {
        /* Return 0 to allow long writes */
        return 0;
    }

    if (len > maxLen)
    {
        printk("WR-E! %d > %d\n", len, maxLen);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    if (offset + len > maxLen)
    {
        printk("WR-E! %d + %d > %d\n", len, offset, maxLen);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(out + offset, buf, len);
    return len;
}

template <typename TInstance, typename TValue>
static void _writeHook(TInstance *instance, const TValue &value)
{
    if constexpr (BtGattWriteHook<TInstance, TValue>)
    {
        instance->onWrite(value);
    }
}

template <typename TInstance, typename T>
static ssize_t _write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                      const void *buf, uint16_t len, uint16_t offset,
                      uint8_t flags, TInstance *instance, T &storage)
{
    if constexpr (BtGattStringTraits<T>::kIsString)
    {
        if (flags & BT_GATT_WRITE_FLAG_PREPARE)
        {
            return 0;
        }

        constexpr size_t maxLen = BtGattStringTraits<T>::kMaxLen;
        if (len >= maxLen)
        {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }

        if (offset + len >= maxLen)
        {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
        }

        memcpy(storage.data() + offset, buf, len);
        storage[offset + len] = '\0';

        _writeHook(instance, storage);
        return len;
    }

    ssize_t writeRet = _write(conn, attr, buf, len, offset, flags,
                              reinterpret_cast<std::byte *>(&storage), sizeof(storage));
    if (writeRet > 0)
    {
        _writeHook(instance, storage);
    }

    return writeRet;
}

/**
 * @brief Compile-time string wrapper used as a non-type template parameter.
 *
 * Stores a copied null-terminated literal in local array storage owned by the
 * wrapper instance and provides conversion to const char*.
 */
template <size_t N>
struct StringLiteral
{
    constexpr StringLiteral(const char (&str)[N])
    {
        std::copy_n(str, N, value);
    }
    char value[N];
    constexpr operator const char *() const { return value; }
};

// Deduction guide for StringLiteral
template <size_t N>
StringLiteral(const char (&)[N]) -> StringLiteral<N>;

/**
 * @brief CCC user-data bridge containing Zephyr-managed and app-managed fields.
 *
 * The first member must remain @ref bt_gatt_ccc_managed_user_data so Zephyr can
 * treat this as CCC managed storage, while app_user_data carries the owning
 * characteristic instance pointer used by static callbacks.
 */
struct bt_gatt_ccc_managed_user_data_with_app_user_data
{
    bt_gatt_ccc_managed_user_data ccc_managed; // !!! MUST BE THE FIRST ELEMENT IN THIS STRUCT !!!
    void *app_user_data;
};

/**
 * @brief Explicit-UUID GATT characteristic provider for server assembly.
 *
 * Participates in the provider concept via @ref getAttrsTuple and contributes a
 * characteristic attribute set to @ref BtGattServer. The characteristic UUID comes
 * from the template parameter and is not rewritten by server auto-assignment.
 */
template <bt_uuid_128 CharacteristicUuid, StringLiteral Description, bool Notify, bool ReadOnly, typename T, T Default>
class BtGattCharacteristic : public BtGattAttrProviderBase
{
public:
    // Type alias for this class to use in static methods
    using Self = BtGattCharacteristic<CharacteristicUuid, Description, Notify, ReadOnly, T, Default>;

    static_assert(BtGattCpfTraits<T>::kSupported,
                  "Unsupported type for BtGattCharacteristic CPF deduction. "
                  "Add a BtGattCpfTraits<T> specialization with a static constexpr bt_gatt_cpf kValue.");

    constexpr auto getAttrsTuple()
    {
        auto baseAttrs = std::make_tuple(
            // A characteristic has a minimum of two attributes.
            // First, we have the GATT Characteristic attribute. This declares that we are about to
            // start defining a new characteristic.
            bt_gatt_attr{
                .uuid = &kGattChrcUuid.uuid,
                .read = bt_gatt_attr_read_chrc,
                .write = NULL,
                .user_data = &characteristic_,
                .handle = 0,
                .perm = BT_GATT_PERM_READ,
            },
            // Next, we declare information about the actual characteristic value itself.
            // This includes read / write function pointers for the characteristic.
            bt_gatt_attr{
                .uuid = &characteristic_uuid_.uuid,
                .read = read,
                .write = ReadOnly ? nullptr : write,
                .user_data = this,
                .handle = 0,
                .perm = BT_GATT_PERM_READ_ENCRYPT | (ReadOnly ? 0 : BT_GATT_PERM_WRITE_ENCRYPT | BT_GATT_PERM_PREPARE_WRITE),
            },
            // We follow up with a Characteristic User Descripton Descriptor (CUD)
            bt_gatt_attr{
                .uuid = &kGattCudUuid.uuid,
                .read = bt_gatt_attr_read_cud,
                .write = NULL,
                .user_data = const_cast<void *>(static_cast<const void *>(&Description.value)),
                .handle = 0,
                .perm = BT_GATT_PERM_READ,
            },
            // And finally a Characteristic Presentation Format Descriptor (CPF)
            bt_gatt_attr{
                .uuid = &kGattCpfUuid.uuid,
                .read = bt_gatt_attr_read_cpf,
                .write = NULL,
                .user_data = &characteristic_cpf_,
                .handle = 0,
                .perm = BT_GATT_PERM_READ,
            });

        if constexpr (Notify)
        {
            // BT_GATT_CCC(_ccc_cfg_changed_func, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
            // Add notification support - CCC descriptor and related attributes
            auto notifyAttrs = std::make_tuple(
                // TODO: First notification attribute
                bt_gatt_attr{
                    .uuid = &kGattCccUuid.uuid,
                    .read = bt_gatt_attr_read_ccc,
                    .write = bt_gatt_attr_write_ccc,
                    .user_data = &ccc_data_,
                    .handle = 0,
                    .perm = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                });

            return std::tuple_cat(baseAttrs, notifyAttrs);
        }
        else
        {
            return baseAttrs;
        }
    }

    void notify()
    {
        if constexpr (Notify)
        {
            bt_gatt_attr *attr = getAttr(1); // Characteristic Value Attribute is always at index 1 relative to our first attribute

            if (!sendNotifications_)
            {
                printk("%p Notifications not enabled, skipping\n", attr);
                return;
            }

            printk("NOTIFY: %p\n", attr);
            int ret = bt_gatt_notify(NULL, attr, &storage_, sizeof(storage_));
            if (ret != 0)
            {
                printk("Notify failed: %d\n", ret);
            }
            else
            {
                printk("Notify succeeded\n");
            }
        }
        else
        {
            printk("NOTIFY: Notifications not enabled for this characteristic\n");
        }
    }

    // Bluetooth callback to change the notification state of this characteristic
    static void cccCfgChanged(const struct bt_gatt_attr *attr, uint16_t value)
    {
        // Get a mutable `this` pointer
        // NOTE: this is different from our read/write functions, since the CCC bt_gatt_attr user_data is pointing at our ccc_data_ member, rather than `this` directly.
        bt_gatt_ccc_managed_user_data_with_app_user_data *managed_user_data = reinterpret_cast<bt_gatt_ccc_managed_user_data_with_app_user_data *>(const_cast<struct bt_gatt_attr *>(attr)->user_data);
        Self *instance = reinterpret_cast<Self *>(managed_user_data->app_user_data);

        instance->sendNotifications_ = (value == BT_GATT_CCC_NOTIFY);
        printk("%p Notification state: %d\n", attr, instance->sendNotifications_);
    }

    static ssize_t read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
    {
        // Get a mutable `this` pointer
        Self *instance = reinterpret_cast<Self *>(const_cast<struct bt_gatt_attr *>(attr)->user_data);

        if constexpr (BtGattStringTraits<T>::kIsString)
        {
            const size_t stringLen = strnlen(instance->storage_.data(), BtGattStringTraits<T>::kMaxLen);
            return bt_gatt_attr_read(conn, attr, buf, len, offset, instance->storage_.data(), stringLen);
        }

        // TODO: I think we need to protect this read in the same way that we protect _write().
        // Ex: we need to add bounds checking and offset handling, to support long reads and prevent buffer overflows.
        // Otherwise a malicious client could cause us to read out of bounds memory.
        return bt_gatt_attr_read(conn, attr, buf, len, offset, &instance->storage_, sizeof(instance->storage_));
    }

    static ssize_t write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset,
                         uint8_t flags)
    {
        // Get a mutable `this` pointer
        Self *instance = reinterpret_cast<Self *>(const_cast<struct bt_gatt_attr *>(attr)->user_data);
        return _write(conn, attr, buf, len, offset, flags, instance, instance->storage_);
    }

    // Allow variable assignment
    T &operator=(const T &other)
    {
        // Check that the value is updated to avoid spamming BT client
        if (storage_ != other)
        {
            // Update storage first so the notification contains the updated value
            storage_ = other;
            notify();
        }

        return storage_;
    }

    // Allow casting to our underlying instance
    operator T()
    {
        return storage_;
    }

    const T &value() const
    {
        return storage_;
    }

private:
    static constexpr bt_uuid_16 kGattChrcUuid = BT_UUID_INIT_16(BT_UUID_GATT_CHRC_VAL);
    static constexpr bt_uuid_16 kGattCudUuid = BT_UUID_INIT_16(BT_UUID_GATT_CUD_VAL);
    static constexpr bt_uuid_16 kGattCpfUuid = BT_UUID_INIT_16(BT_UUID_GATT_CPF_VAL);
    static constexpr bt_uuid_16 kGattCccUuid = BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);

    T storage_ = Default;
    bool sendNotifications_ = false;
    bt_uuid_128 characteristic_uuid_ = CharacteristicUuid;
    bt_gatt_cpf characteristic_cpf_ = BtGattCpfTraits<T>::kValue;
    bt_gatt_chrc characteristic_ = BT_GATT_CHRC_INIT(&characteristic_uuid_.uuid, 0U, Notify ? (BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY) : (BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE));
    bt_gatt_ccc_managed_user_data_with_app_user_data ccc_data_ = {
        .ccc_managed = BT_GATT_CCC_MANAGED_USER_DATA_INIT(cccCfgChanged, NULL, NULL),
        .app_user_data = this,
    };
};

/**
 * @brief Auto-UUID GATT characteristic provider for server assembly.
 *
 * Participates in the provider concept via @ref getAttrsTuple and receives its
 * characteristic UUID from @ref BtGattServer through @ref assignAutoUuid using the
 * primary service UUID plus provider-order characteristic index.
 */
template <StringLiteral Description, bool Notify, bool ReadOnly, typename T, T Default>
class BtGattAutoCharacteristic : public BtGattAttrProviderBase
{
public:
    using Self = BtGattAutoCharacteristic<Description, Notify, ReadOnly, T, Default>;

    static_assert(BtGattCpfTraits<T>::kSupported,
                  "Unsupported type for BtGattCharacteristic CPF deduction. "
                  "Add a BtGattCpfTraits<T> specialization with a static constexpr bt_gatt_cpf kValue.");

    void assignAutoUuid(const bt_uuid_128 &serviceUuid, uint16_t characteristicId)
    {
        characteristic_uuid_ = composeAutoCharacteristicUuid(serviceUuid, characteristicId);
    }

    constexpr auto getAttrsTuple()
    {
        auto baseAttrs = std::make_tuple(
            bt_gatt_attr{
                .uuid = &kGattChrcUuid.uuid,
                .read = bt_gatt_attr_read_chrc,
                .write = NULL,
                .user_data = &characteristic_,
                .handle = 0,
                .perm = BT_GATT_PERM_READ,
            },
            bt_gatt_attr{
                .uuid = &characteristic_uuid_.uuid,
                .read = read,
                .write = ReadOnly ? nullptr : write,
                .user_data = this,
                .handle = 0,
                .perm = BT_GATT_PERM_READ_ENCRYPT | (ReadOnly ? 0 : BT_GATT_PERM_WRITE_ENCRYPT | BT_GATT_PERM_PREPARE_WRITE),
            },
            bt_gatt_attr{
                .uuid = &kGattCudUuid.uuid,
                .read = bt_gatt_attr_read_cud,
                .write = NULL,
                .user_data = const_cast<void *>(static_cast<const void *>(&Description.value)),
                .handle = 0,
                .perm = BT_GATT_PERM_READ,
            },
            bt_gatt_attr{
                .uuid = &kGattCpfUuid.uuid,
                .read = bt_gatt_attr_read_cpf,
                .write = NULL,
                .user_data = &characteristic_cpf_,
                .handle = 0,
                .perm = BT_GATT_PERM_READ,
            });

        if constexpr (Notify)
        {
            auto notifyAttrs = std::make_tuple(
                bt_gatt_attr{
                    .uuid = &kGattCccUuid.uuid,
                    .read = bt_gatt_attr_read_ccc,
                    .write = bt_gatt_attr_write_ccc,
                    .user_data = &ccc_data_,
                    .handle = 0,
                    .perm = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                });

            return std::tuple_cat(baseAttrs, notifyAttrs);
        }
        else
        {
            return baseAttrs;
        }
    }

    void notify()
    {
        if constexpr (Notify)
        {
            bt_gatt_attr *attr = getAttr(1);

            if (!sendNotifications_)
            {
                printk("%p Notifications not enabled, skipping\n", attr);
                return;
            }

            printk("NOTIFY: %p\n", attr);
            int ret = bt_gatt_notify(NULL, attr, &storage_, sizeof(storage_));
            if (ret != 0)
            {
                printk("Notify failed: %d\n", ret);
            }
            else
            {
                printk("Notify succeeded\n");
            }
        }
        else
        {
            printk("NOTIFY: Notifications not enabled for this characteristic\n");
        }
    }

    static void cccCfgChanged(const struct bt_gatt_attr *attr, uint16_t value)
    {
        bt_gatt_ccc_managed_user_data_with_app_user_data *managed_user_data = reinterpret_cast<bt_gatt_ccc_managed_user_data_with_app_user_data *>(const_cast<struct bt_gatt_attr *>(attr)->user_data);
        Self *instance = reinterpret_cast<Self *>(managed_user_data->app_user_data);

        instance->sendNotifications_ = (value == BT_GATT_CCC_NOTIFY);
        printk("%p Notification state: %d\n", attr, instance->sendNotifications_);
    }

    static ssize_t read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
    {
        Self *instance = reinterpret_cast<Self *>(const_cast<struct bt_gatt_attr *>(attr)->user_data);

        if constexpr (BtGattStringTraits<T>::kIsString)
        {
            const size_t stringLen = strnlen(instance->storage_.data(), BtGattStringTraits<T>::kMaxLen);
            return bt_gatt_attr_read(conn, attr, buf, len, offset, instance->storage_.data(), stringLen);
        }

        return bt_gatt_attr_read(conn, attr, buf, len, offset, &instance->storage_, sizeof(instance->storage_));
    }

    static ssize_t write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset,
                         uint8_t flags)
    {
        Self *instance = reinterpret_cast<Self *>(const_cast<struct bt_gatt_attr *>(attr)->user_data);
        return _write(conn, attr, buf, len, offset, flags, instance, instance->storage_);
    }

    T &operator=(const T &other)
    {
        if (storage_ != other)
        {
            storage_ = other;
            notify();
        }

        return storage_;
    }

    operator T()
    {
        return storage_;
    }

    const T &value() const
    {
        return storage_;
    }

private:
    static constexpr bt_uuid_16 kGattChrcUuid = BT_UUID_INIT_16(BT_UUID_GATT_CHRC_VAL);
    static constexpr bt_uuid_16 kGattCudUuid = BT_UUID_INIT_16(BT_UUID_GATT_CUD_VAL);
    static constexpr bt_uuid_16 kGattCpfUuid = BT_UUID_INIT_16(BT_UUID_GATT_CPF_VAL);
    static constexpr bt_uuid_16 kGattCccUuid = BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);

    T storage_ = Default;
    bool sendNotifications_ = false;
    bt_uuid_128 characteristic_uuid_{};
    bt_gatt_cpf characteristic_cpf_ = BtGattCpfTraits<T>::kValue;
    bt_gatt_chrc characteristic_ = BT_GATT_CHRC_INIT(&characteristic_uuid_.uuid, 0U, Notify ? (BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY) : (BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE));
    bt_gatt_ccc_managed_user_data_with_app_user_data ccc_data_ = {
        .ccc_managed = BT_GATT_CCC_MANAGED_USER_DATA_INIT(cccCfgChanged, NULL, NULL),
        .app_user_data = this,
    };
};

// Specialized version of BtGattCharacteristic for combinations of read-only/read-write, notify/no-notify characteristics

template <bt_uuid_128 CharacteristicUuid, StringLiteral Description, typename T, T Default>
using BtGattReadWriteCharacteristic = BtGattCharacteristic<CharacteristicUuid, Description, false /* Notify */, false /* ReadOnly */, T, Default>;

template <bt_uuid_128 CharacteristicUuid, StringLiteral Description, typename T, T Default>
using BtGattReadWriteNotifyCharacteristic = BtGattCharacteristic<CharacteristicUuid, Description, true /* Notify */, false /* ReadOnly */, T, Default>;

template <bt_uuid_128 CharacteristicUuid, StringLiteral Description, typename T, T Default>
using BtGattReadNotifyCharacteristic = BtGattCharacteristic<CharacteristicUuid, Description, true /* Notify */, true /* ReadOnly */, T, Default>;

template <bt_uuid_128 CharacteristicUuid, StringLiteral Description, typename T, T Default>
using BtGattReadOnlyCharacteristic = BtGattCharacteristic<CharacteristicUuid, Description, false /* Notify */, true /* ReadOnly */, T, Default>;

// Auto aliases derive UUIDs from BtGattServer provider order; use explicit UUID aliases if UUID stability across reordering is required.
template <StringLiteral Description, typename T, T Default>
using BtGattAutoReadWriteCharacteristic = BtGattAutoCharacteristic<Description, false /* Notify */, false /* ReadOnly */, T, Default>;

template <StringLiteral Description, typename T, T Default>
using BtGattAutoReadWriteNotifyCharacteristic = BtGattAutoCharacteristic<Description, true /* Notify */, false /* ReadOnly */, T, Default>;

template <StringLiteral Description, typename T, T Default>
using BtGattAutoReadNotifyCharacteristic = BtGattAutoCharacteristic<Description, true /* Notify */, true /* ReadOnly */, T, Default>;

template <StringLiteral Description, typename T, T Default>
using BtGattAutoReadOnlyCharacteristic = BtGattAutoCharacteristic<Description, false /* Notify */, true /* ReadOnly */, T, Default>;