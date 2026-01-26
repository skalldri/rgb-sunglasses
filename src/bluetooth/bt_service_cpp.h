#pragma once

#include <zephyr/bluetooth/gatt.h>
#include <array>
#include <tuple>
#include <algorithm>

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

// Base class for providers that need back-references to the server's attribute array
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

// A complete GATT server that accepts a variadic list of BtGattAttributeProvider types
// as input, and constructs a flat array of all attributes provided by them at compile-time.
// This is then registered as a
template <BtGattAttributeProvider... Providers>
class BtGattServer
{
public:
    // Deduce total count from all providers' tuples
    static constexpr size_t kTotalAttrCount =
        (std::tuple_size_v<
             decltype(std::declval<Providers>().getAttrsTuple())> +
         ...);

    BtGattServer(Providers &...providers)
        : attrs_(tupleToArray(concatenateAttrTuples(providers...),
                              std::make_index_sequence<kTotalAttrCount>{}))
    {
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
    std::array<bt_gatt_attr, kTotalAttrCount> attrs_;
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

// A class to represent a GATT primary service, that conforms to the BtGattAttributeProvider concept.
// Primary Services are represented by a single GATT attribute, with relatively well understood UUIDs.
template <bt_uuid_128 ServiceUuid>
class BtGattPrimaryService
{
public:
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

// String wrapper for use as NTTP
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

// Extremely cursed struct to hold both managed CCC data and user data
// We need this because the CCC bt_gatt_attr requires that the `user_data` pointer be a bt_gatt_ccc_managed_user_data.
// However, we also need to store our own user data, since we need to recover `this` from inside the static `ccc_changed` callback.
// So we create a new struct which contains both, and use that as the `user_data` pointer.
// Since bt_gatt_ccc_managed_user_data is the first element in the struct, from the Zephyr Bluetooth code's perspective they
// are effectively the same thing.
//
// So cursed....
struct bt_gatt_ccc_managed_user_data_with_app_user_data
{
    bt_gatt_ccc_managed_user_data ccc_managed; // !!! MUST BE THE FIRST ELEMENT IN THIS STRUCT !!!
    void *app_user_data;
};

// A class to represent a GATT primary service, that conforms to the BtGattAttributeProvider concept.
// Primary Services are represented by a single GATT attribute, with relatively well understood UUIDs.
template <bt_uuid_128 CharacteristicUuid, StringLiteral Description, bt_gatt_cpf CharacteristicCpf, bool Notify, typename T, T Default>
class BtGattReadWriteCharacteristic : public BtGattAttrProviderBase
{
public:
    // Type alias for this class to use in static methods
    using Self = BtGattReadWriteCharacteristic<CharacteristicUuid, Description, CharacteristicCpf, Notify, T, Default>;

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
                .write = write,
                .user_data = this,
                .handle = 0,
                .perm = BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT | BT_GATT_PERM_PREPARE_WRITE,
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

    // Bluetooth callback to change the notification state of the isActive characteristic
    static void isActiveCccCfgChanged(const struct bt_gatt_attr *attr, uint16_t value)
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
        return bt_gatt_attr_read(conn, attr, buf, len, offset, &instance->storage_, sizeof(instance->storage_));
    }

    static ssize_t write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset,
                         uint8_t flags)
    {
        // Get a mutable `this` pointer
        Self *instance = reinterpret_cast<Self *>(const_cast<struct bt_gatt_attr *>(attr)->user_data);
        return _write(conn, attr, buf, len, offset, flags, reinterpret_cast<std::byte *>(&instance->storage_), sizeof(instance->storage_));
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

private:
    static constexpr bt_uuid_16 kGattChrcUuid = BT_UUID_INIT_16(BT_UUID_GATT_CHRC_VAL);
    static constexpr bt_uuid_16 kGattCudUuid = BT_UUID_INIT_16(BT_UUID_GATT_CUD_VAL);
    static constexpr bt_uuid_16 kGattCpfUuid = BT_UUID_INIT_16(BT_UUID_GATT_CPF_VAL);
    static constexpr bt_uuid_16 kGattCccUuid = BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);

    T storage_ = Default;
    bool sendNotifications_ = false;
    bt_uuid_128 characteristic_uuid_ = CharacteristicUuid;
    bt_gatt_cpf characteristic_cpf_ = CharacteristicCpf;
    bt_gatt_chrc characteristic_ = BT_GATT_CHRC_INIT(&characteristic_uuid_.uuid, 0U, Notify ? (BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY) : (BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE));
    bt_gatt_ccc_managed_user_data_with_app_user_data ccc_data_ = {
        .ccc_managed = BT_GATT_CCC_MANAGED_USER_DATA_INIT(isActiveCccCfgChanged, NULL, NULL),
        .app_user_data = this,
    };
};
