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

    constexpr BtGattServer(Providers &...providers)
        : attrs_(tupleToArray(concatenateAttrTuples(providers...),
                              std::make_index_sequence<kTotalAttrCount>{})) {}

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

// A class to represent a GATT primary service, that conforms to the BtGattAttributeProvider concept.
// Primary Services are represented by a single GATT attribute, with relatively well understood UUIDs.
template <bt_uuid_128 CharacteristicUuid, StringLiteral Description, bt_gatt_cpf CharacteristicCpf, typename T, T Default>
class BtGattReadWriteCharacteristic
{
public:
    // Type alias for this class to use in static methods
    using Self = BtGattReadWriteCharacteristic<CharacteristicUuid, Description, CharacteristicCpf, T, Default>;

    constexpr auto getAttrsTuple()
    {
        return std::make_tuple(
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

private:
    static constexpr bt_uuid_16 kGattChrcUuid = BT_UUID_INIT_16(BT_UUID_GATT_CHRC_VAL);
    static constexpr bt_uuid_16 kGattCudUuid = BT_UUID_INIT_16(BT_UUID_GATT_CUD_VAL);
    static constexpr bt_uuid_16 kGattCpfUuid = BT_UUID_INIT_16(BT_UUID_GATT_CPF_VAL);

    T storage_ = Default;
    bt_uuid_128 characteristic_uuid_ = CharacteristicUuid;
    bt_gatt_cpf characteristic_cpf_ = CharacteristicCpf;
    bt_gatt_chrc characteristic_ = BT_GATT_CHRC_INIT(&characteristic_uuid_.uuid, 0U, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE);
};
