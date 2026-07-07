#pragma once

#include <animations/animation_types.h>
#include <bluetooth/bt_gatt_traits.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstring>
#include <tuple>
#include <type_traits>

/**
 * @brief True if at least one connected LE peer meets the encryption level every
 * auto characteristic's perm bits require (BT_GATT_PERM_READ_ENCRYPT, set below).
 *
 * bt_gatt_notify() runs this exact check internally (via the internal-only
 * bt_gatt_check_perm(), see gatt_internal.h - not something app code can call
 * directly) and, on failure, unconditionally logs LOG_WRN("Link is not
 * encrypted") before returning -EPERM. Right after a fresh connect there's a
 * real window - the length of LE Secure Connections pairing, several seconds -
 * where the link isn't encrypted yet; any periodic notify (e.g. battery
 * voltage) fired in that window used to spam one WRN + one "Notify failed"
 * printk apiece until pairing completed. Checking here lets notify() skip the
 * call (and both logs) silently, the same way it already skips a call to an
 * unsubscribed characteristic below.
 */
inline bool bleAnyConnEncrypted() {
    bool anyEncrypted = false;
    bt_conn_foreach(
        BT_CONN_TYPE_LE,
        [](struct bt_conn *conn, void *data) {
            if (bt_conn_get_security(conn) >= BT_SECURITY_L2) {
                *static_cast<bool *>(data) = true;
            }
        },
        &anyEncrypted);
    return anyEncrypted;
}

constexpr bt_uuid_128 composeAutoCharacteristicUuid(const bt_uuid_128 &serviceUuid,
                                                    uint16_t characteristicId) {
    bt_uuid_128 uuid = serviceUuid;
    uuid.val[0] = static_cast<uint8_t>(characteristicId & 0xFF);
    uuid.val[1] = static_cast<uint8_t>((characteristicId >> 8) & 0xFF);
    return uuid;
}

/**
 * @brief Helper macro to generate unique GATT service UUIDs for animations.
 *
 * Each animation gets a unique service UUID based on its Animation enum ID.
 * The ID is placed in the third parameter (upper byte) to ensure uniqueness
 * in the first 14 bytes, which protects against characteristic UUID collisions
 * when composeAutoCharacteristicUuid() modifies the first 2 bytes.
 */
#define BT_ANIMATION_SERVICE_UUID(anim_id)                                                      \
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, ((uint16_t)(anim_id) << 8), \
                                        0x56789abd0000))

/**
 * @brief Fixed UUID for the "Animation Name" characteristic, reused identically across every
 * animation's BtGattServer. Group 4 (0xaaaa) is chosen to never collide with any anim_id<<8
 * used by BT_ANIMATION_SERVICE_UUID (anim_id only ranges 0-11 today), and characteristic UUIDs
 * are not required to be globally unique across services — only within one — so reusing the
 * same literal in 8 different services is valid GATT.
 */
constexpr bt_uuid_128 kAnimationNameCharacteristicUuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0xaaaa, 0x56789abd0000));

/**
 * @brief Fixed UUID for the "Is Active" characteristic, reused identically across every
 * animation's BtGattServer, mirroring kAnimationNameCharacteristicUuid above. Group 0xbbbb is
 * chosen to never collide with any anim_id<<8 used by BT_ANIMATION_SERVICE_UUID (anim_id only
 * ranges 0-11 today) nor with group 0xaaaa (Animation Name). Without a fixed UUID, "Is Active"
 * would get an auto-assigned UUID whose position varies by animation (depends on how many other
 * characteristics that animation declares before it), making it impossible for the app to find
 * reliably across animations without hardcoding per-animation positions.
 */
constexpr bt_uuid_128 kIsActiveCharacteristicUuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0xbbbb, 0x56789abd0000));

/**
 * @brief Fixed UUID for the auto-synthesized "bulk metadata" characteristic that
 * @ref BtGattServer appends to every service it assembles (issue #41 follow-up).
 * Group 0xcccc is chosen to never collide with kAnimationNameCharacteristicUuid's 0xaaaa,
 * kIsActiveCharacteristicUuid's 0xbbbb, or any anim_id<<8 used by BT_ANIMATION_SERVICE_UUID
 * (anim_id only ranges 0-11 today). Reused identically across every service, same rationale
 * as kAnimationNameCharacteristicUuid.
 *
 * The app reads this once per service to learn every sibling characteristic's CUD name +
 * CPF format in a single ATT round-trip, instead of two descriptor reads per characteristic.
 * See MetadataBlobBuilder below for the wire format and BtGattServer for how this gets
 * appended to the attribute table.
 */
constexpr bt_uuid_128 kMetadataCharacteristicUuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0xcccc, 0x56789abd0000));

/**
 * @brief Wire-format version for the bulk metadata blob (see MetadataBlobBuilder). Bump this
 * if the byte layout ever changes, so an app build that doesn't understand a newer version can
 * detect the mismatch and fall back to per-descriptor reads instead of misparsing the blob.
 * Must match METADATA_BLOB_VERSION in app/constants/bluetooth.ts.
 */
constexpr uint8_t kMetadataBlobVersion = 1;

// Helper to check if all tuple elements are bt_gatt_attr
template <typename Tuple, size_t... Is>
constexpr bool allAttrsAreGattAttr(std::index_sequence<Is...>) {
    return (std::is_same_v<std::tuple_element_t<Is, Tuple>, bt_gatt_attr> && ...);
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
        std::make_index_sequence<std::tuple_size_v<decltype(t.getAttrsTuple())>>{});
};

template <typename T>
concept BtGattServiceUuidProvider = requires(T &t) { t.getServiceUuid(); };

template <typename T>
concept BtGattAutoUuidAssignable =
    requires(T &t, const bt_uuid_128 &serviceUuid, uint16_t characteristicId) {
        t.assignAutoUuid(serviceUuid, characteristicId);
    };

/**
 * @brief Detects whether a provider TYPE (not instance) exposes static compile-time
 * CUD/CPF metadata accessors. Satisfied by every concrete characteristic built on
 * @ref BtGattCharacteristicCommon (via its getDescription()/getCpf() static methods,
 * inherited transparently through ordinary derived-class lookup - no instance or
 * virtual dispatch needed). Deliberately NOT satisfied by @ref BtGattPrimaryService,
 * which is exactly the property @ref MetadataBlobBuilder uses to skip it automatically
 * when building a service's bulk metadata blob.
 */
template <typename P>
concept BtGattMetadataBearingProvider = requires {
    { P::getDescription() } -> std::convertible_to<const char *>;
    { P::getCpf() } -> std::convertible_to<bt_gatt_cpf>;
};

/**
 * @brief Compile-time fold that packs every metadata-bearing provider's CUD name + CPF
 * format into a single byte blob, in @p Ps declaration order (issue #41 follow-up).
 *
 * Wire format (excluding the 2-byte [version][entry_count] header written by the
 * caller, see BtGattServer::kMetadataBlob below):
 *   per entry: [cpf_format: 1 byte][name_len: 1 byte][name_bytes: name_len bytes, no NUL]
 *
 * Operates purely on types (Ps...), not instances - getDescription()/getCpf() are
 * static, so this needs no provider objects to exist yet. Non-metadata-bearing
 * providers (the primary service) are skipped automatically via `if constexpr`.
 *
 * IMPORTANT ordering assumption: entries are written in Ps... (i.e. BtGattServer's
 * Providers... pack) order, which becomes GATT attribute/handle order. The app's
 * positional zip (use-ble-connection.ts) assumes characteristicsForService() returns
 * characteristics in that same handle order - true by the ATT spec's "Read By Type"
 * ascending-handle-order guarantee for characteristic discovery, not just a platform
 * convention. See the matching comment in use-ble-connection.ts for the full reasoning
 * and the one unverified (library, not protocol) link in this chain.
 */
template <typename... Ps>
struct MetadataBlobBuilder;

template <>
struct MetadataBlobBuilder<> {
    static constexpr size_t size() { return 0; }
    static constexpr size_t count() { return 0; }
    static constexpr void write(uint8_t *, size_t &) {}
};

template <typename P, typename... Rest>
struct MetadataBlobBuilder<P, Rest...> {
    static constexpr size_t size() {
        if constexpr (BtGattMetadataBearingProvider<P>) {
            static_assert(
                strlen(P::getDescription()) <= 255,
                "CUD description too long for 1-byte length-prefixed metadata blob entry");
            return 2 + strlen(P::getDescription()) +
                   MetadataBlobBuilder<Rest...>::size();
        } else {
            return MetadataBlobBuilder<Rest...>::size();
        }
    }

    static constexpr size_t count() {
        return (BtGattMetadataBearingProvider<P> ? 1 : 0) + MetadataBlobBuilder<Rest...>::count();
    }

    static constexpr void write(uint8_t *buf, size_t &pos) {
        if constexpr (BtGattMetadataBearingProvider<P>) {
            const char *desc = P::getDescription();
            size_t len = strlen(desc);
            buf[pos++] = static_cast<uint8_t>(P::getCpf().format);
            buf[pos++] = static_cast<uint8_t>(len);
            for (size_t i = 0; i < len; i++) {
                buf[pos++] = static_cast<uint8_t>(desc[i]);
            }
        }
        MetadataBlobBuilder<Rest...>::write(buf, pos);
    }
};

/**
 * @brief Builds the full [version][entry_count][entries...] blob for @p Providers... at
 * compile time. See MetadataBlobBuilder above for the entry format and ordering rationale.
 */
template <typename... Providers>
constexpr auto buildMetadataBlob() {
    static_assert(MetadataBlobBuilder<Providers...>::count() <= 255,
                  "Too many metadata-bearing characteristics for a 1-byte entry_count");
    constexpr size_t kBlobSize = 2 + MetadataBlobBuilder<Providers...>::size();
    std::array<uint8_t, kBlobSize> blob{};
    blob[0] = kMetadataBlobVersion;
    blob[1] = static_cast<uint8_t>(MetadataBlobBuilder<Providers...>::count());
    size_t pos = 2;
    MetadataBlobBuilder<Providers...>::write(blob.data(), pos);
    return blob;
}

/**
 * @brief Detects whether a characteristic type exposes an `onWrite` hook.
 *
 * If satisfied, common write handlers invoke `instance.onWrite(value)` after a
 * successful write to app storage.
 */
template <typename TInstance, typename TValue>
concept BtGattWriteHook =
    requires(TInstance &instance, const TValue &value) { instance.onWrite(value); };

/**
 * @brief Detects whether a characteristic type exposes a *fallible* write hook.
 *
 * `int onWriteChecked(const T&)` is invoked after the value is copied into app
 * storage; a non-zero return rejects the remote write — storage is restored to
 * its previous value and the ATT operation fails with
 * BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED). Use this when accepting the write
 * depends on a side effect that can fail (e.g. an I2C register write), so the
 * app's optimistic UI update reverts deterministically instead of relying on a
 * corrective notify (see the "Refusing a GATT write" rule in fw/CLAUDE.md).
 *
 * A characteristic type must define either onWrite or onWriteChecked, not both.
 */
template <typename TInstance, typename TValue>
concept BtGattCheckedWriteHook = requires(TInstance &instance, const TValue &value) {
    { instance.onWriteChecked(value) } -> std::convertible_to<int>;
};

/**
 * @brief Base mixin for providers that need attribute-array back-references.
 *
 * Instances are non-owning: @ref bind stores a pointer to the server's flattened
 * attribute storage for later indexed access via @ref getAttr.
 */
class BtGattAttrProviderBase {
   public:
    void bind(bt_gatt_attr *base) { attrBase_ = base; }
    bt_gatt_attr *getAttr(size_t idx = 0) const { return attrBase_ + idx; }

   protected:
    bt_gatt_attr *attrBase_ = nullptr;
};

// Helper to get the attribute count from a BtGattAttributeProvider
template <BtGattAttributeProvider T>
constexpr size_t attrCount() {
    return std::tuple_size_v<decltype(std::declval<T>().getAttrsTuple())>;
}

// Helper to concatenate tuples from all providers
template <BtGattAttributeProvider... Providers>
constexpr auto concatenateAttrTuples(Providers &...providers) {
    return std::tuple_cat(providers.getAttrsTuple()...);
}

// Helper to convert a tuple to an array
template <typename Tuple, size_t... Is>
constexpr auto tupleToArray(const Tuple &tuple, std::index_sequence<Is...>) {
    return std::array<bt_gatt_attr, sizeof...(Is)>{{std::get<Is>(tuple)...}};
}

// Helper to calculate offset of provider N in the parameter pack
template <size_t N, typename... Providers>
constexpr size_t offsetOfProvider() {
    if constexpr (N == 0) {
        return 0;
    } else {
        constexpr size_t counts[] = {
            std::tuple_size_v<decltype(std::declval<Providers>().getAttrsTuple())>...};
        size_t offset = 0;
        for (size_t i = 0; i < N; ++i) {
            offset += counts[i];
        }
        return offset;
    }
}

// Helper to bind all providers to their offsets in the array
template <size_t... Is, typename... Providers>
void bindProviders(bt_gatt_attr *base, std::index_sequence<Is...>, Providers &...providers) {
    auto bindIfPossible = [&]<typename P>(P &provider, size_t offset) {
        if constexpr (std::is_base_of_v<BtGattAttrProviderBase, P>) {
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
class BtGattServer {
   public:
    static constexpr size_t kServiceUuidProviderCount =
        (0 + ... + static_cast<size_t>(BtGattServiceUuidProvider<Providers>));

    // Deduce attribute count contributed by the providers passed in by the caller.
    static constexpr size_t kProviderAttrCount =
        (std::tuple_size_v<decltype(std::declval<Providers>().getAttrsTuple())> + ...);

    // +2 for the bulk metadata characteristic BtGattServer synthesizes and appends below
    // (issue #41 follow-up) - one characteristic-declaration attr + one value attr, same
    // shape as any other read-only characteristic, but built from the OTHER providers'
    // own compile-time metadata rather than being listed by the caller. See
    // getMetadataAttrsTuple()/kMetadataBlob below and MetadataBlobBuilder above.
    //
    // Gated by CONFIG_APP_BT_METADATA_CHARACTERISTIC (default y): the blob duplicates
    // every characteristic's CUD description string as packed binary data, which doesn't
    // fit in rgb_sunglasses_dk's internal-flash image slot (confirmed: imgtool "Image
    // size ... exceeds requested size" with this enabled on that board) - see its board
    // .conf. DK is legacy and doesn't get new features per fw/CLAUDE.md.
    static constexpr size_t kTotalAttrCount =
        kProviderAttrCount + (IS_ENABLED(CONFIG_APP_BT_METADATA_CHARACTERISTIC) ? 2 : 0);

    BtGattServer(Providers &...providers) {
        static_assert(kServiceUuidProviderCount > 0,
                      "BtGattServer requires at least one provider with getServiceUuid().");

        bt_uuid_128 primaryServiceUuid{};
        bool hasPrimaryServiceUuid = false;

        auto findPrimaryServiceUuid = [&]<typename P>(P &provider) {
            if constexpr (BtGattServiceUuidProvider<P>) {
                if (!hasPrimaryServiceUuid) {
                    primaryServiceUuid = provider.getServiceUuid();
                    hasPrimaryServiceUuid = true;
                }
            }
        };

        (findPrimaryServiceUuid(providers), ...);

        uint16_t autoCharacteristicId = 0;
        auto assignAutoUuids = [&]<typename P>(P &provider) {
            if constexpr (BtGattAutoUuidAssignable<P>) {
                provider.assignAutoUuid(primaryServiceUuid, autoCharacteristicId);
                ++autoCharacteristicId;
            }
        };

        (assignAutoUuids(providers), ...);

        // Metadata attrs are appended AFTER all provider attrs, so they never shift the
        // handles of any characteristic declared earlier in this same service. Gated by
        // CONFIG_APP_BT_METADATA_CHARACTERISTIC - see kTotalAttrCount's comment above for
        // why. When disabled, getMetadataAttrsTuple()/kMetadataBlob are never referenced
        // and so are never instantiated (class template members are only instantiated
        // when used), costing zero flash on boards where this is off.
        if constexpr (IS_ENABLED(CONFIG_APP_BT_METADATA_CHARACTERISTIC)) {
            // Single N+1-ary tuple_cat (all provider tuples + the metadata tuple in one
            // call) rather than nesting a second top-level tuple_cat around an
            // already-flattened intermediate result. Concatenation is
            // associativity-invariant so the resulting tuple type/values are identical --
            // this just avoids libstdc++'s __tuple_concater re-walking the already-flat
            // provider tuple a second time, which was generating a large amount of
            // redundant template-instantiated code (issue #79 flash investigation;
            // dominant cost on services with many providers, e.g. text/my_eyes).
            attrs_ = tupleToArray(
                std::tuple_cat(providers.getAttrsTuple()..., getMetadataAttrsTuple()),
                std::make_index_sequence<kTotalAttrCount>{});
        } else {
            attrs_ = tupleToArray(concatenateAttrTuples(providers...),
                                  std::make_index_sequence<kTotalAttrCount>{});
        }

        // Bind providers that inherit from BtGattAttrProviderBase
        bindProviders(attrs_.data(), std::index_sequence_for<Providers...>{}, providers...);
    }

    // Provide access to the attribute array for external registration
    constexpr bt_gatt_attr *data() { return attrs_.data(); }
    constexpr bt_gatt_attr *data() const { return attrs_.data(); }
    static constexpr size_t size() { return kTotalAttrCount; }

   private:
    static constexpr bt_uuid_16 kGattChrcUuid = BT_UUID_INIT_16(BT_UUID_GATT_CHRC_VAL);

    // Compile-time blob packing every OTHER provider's CUD name + CPF format, in
    // Providers... declaration order (= GATT handle order - see MetadataBlobBuilder's
    // doc comment for the full ATT ordering-guarantee rationale and its one unverified
    // link). Skips the primary-service provider automatically (it isn't
    // BtGattMetadataBearingProvider). Not auto-UUID-assigned - this characteristic uses
    // the fixed, shared kMetadataCharacteristicUuid instead, same pattern as
    // kAnimationNameCharacteristicUuid.
    static constexpr auto kMetadataBlob = buildMetadataBlob<Providers...>();

    static constexpr bt_gatt_chrc kMetadataChrc =
        BT_GATT_CHRC_INIT(&kMetadataCharacteristicUuid.uuid, 0U, BT_GATT_CHRC_READ);

    static ssize_t readMetadata(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                                uint16_t len, uint16_t offset) {
        return bt_gatt_attr_read(conn, attr, buf, len, offset, kMetadataBlob.data(),
                                 kMetadataBlob.size());
    }

    static constexpr auto getMetadataAttrsTuple() {
        return std::make_tuple(
            bt_gatt_attr{
                .uuid = &kGattChrcUuid.uuid,
                .read = bt_gatt_attr_read_chrc,
                .write = NULL,
                .user_data = const_cast<void *>(static_cast<const void *>(&kMetadataChrc)),
                .handle = 0,
                .perm = BT_GATT_PERM_READ,
            },
            bt_gatt_attr{
                .uuid = &kMetadataCharacteristicUuid.uuid,
                .read = readMetadata,
                .write = NULL,
                .user_data = nullptr,
                .handle = 0,
                .perm = BT_GATT_PERM_READ_ENCRYPT,
            });
    }

    std::array<bt_gatt_attr, kTotalAttrCount> attrs_{};
};

#define BT_GATT_SERVER_REGISTER(_name, _server)                      \
    const STRUCT_SECTION_ITERABLE(bt_gatt_service_static, _name) = { \
        .attrs = _server.data(),                                     \
        .attr_count = _server.size(),                                \
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
class BtGattPrimaryService {
   public:
    constexpr bt_uuid_128 getServiceUuid() const { return service_uuid_; }

    constexpr auto getAttrsTuple() {
        return std::make_tuple(bt_gatt_attr(BT_GATT_PRIMARY_SERVICE(&service_uuid_)));
    }

   private:
    bt_uuid_128 service_uuid_ = ServiceUuid;
};

// Helper function to encapsulate the common write logic for read/write characteristics
// Used to avoid exploding the binary size as the template class methods are instantiated multiple
// times.
static ssize_t _write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                      uint16_t len, uint16_t offset, uint8_t flags, std::byte *out, size_t maxLen) {
    // printk("WR! l=%d, o=%d, f=%d\n", len, offset, flags);

    if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
        /* Return 0 to allow long writes */
        return 0;
    }

    if (len > maxLen) {
        printk("WR-E! %d > %d\n", len, maxLen);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    if (offset + len > maxLen) {
        printk("WR-E! %d + %d > %d\n", len, offset, maxLen);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(out + offset, buf, len);
    return len;
}

/**
 * @brief Conditionally dispatches characteristic `onWrite` callback.
 *
 * This helper keeps write-path callback behavior centralized and avoids
 * duplicated `if constexpr` logic across characteristic implementations.
 */
template <typename TInstance, typename TValue>
static void _writeHook(TInstance *instance, const TValue &value) {
    if constexpr (BtGattWriteHook<TInstance, TValue>) {
        instance->onWrite(value);
    }
}

/**
 * @brief Type-aware write helper for GATT value attributes.
 *
 * Handles string and non-string storage variants, validates offsets/lengths,
 * updates local storage, and invokes optional write hooks.
 */
template <typename TInstance, typename T>
static ssize_t _write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                      uint16_t len, uint16_t offset, uint8_t flags, TInstance *instance,
                      T &storage) {
    static_assert(!(BtGattWriteHook<TInstance, T> && BtGattCheckedWriteHook<TInstance, T>),
                  "Define either onWrite or onWriteChecked on a characteristic type, not both");

    if constexpr (BtGattStringTraits<T>::kIsString) {
        // Checked write hooks are not supported for string-backed types: long/prepared
        // writes land in storage chunk-by-chunk across multiple ATT ops, so there is no
        // single point where "the written value" can be validated and atomically rolled
        // back. Extend this if a string characteristic ever needs rejectable writes.
        static_assert(!BtGattCheckedWriteHook<TInstance, T>,
                      "onWriteChecked is not supported for string-backed characteristics");

        if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
            return 0;
        }

        constexpr size_t maxLen = BtGattStringTraits<T>::kMaxLen;
        if (len >= maxLen) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }

        if (offset + len >= maxLen) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
        }

        memcpy(storage.data() + offset, buf, len);
        storage[offset + len] = '\0';

        _writeHook(instance, storage);
        return len;
    }

    // Snapshot taken before the copy so a failing onWriteChecked hook can restore it.
    // Cheap for the POD types this branch handles.
    T previous = storage;

    ssize_t writeRet = _write(conn, attr, buf, len, offset, flags,
                              reinterpret_cast<std::byte *>(&storage), sizeof(storage));
    if (writeRet > 0) {
        if constexpr (BtGattCheckedWriteHook<TInstance, T>) {
            int hookRet = instance->onWriteChecked(storage);
            if (hookRet != 0) {
                storage = previous;
                return BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED);
            }
        } else {
            _writeHook(instance, storage);
        }
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
struct StringLiteral {
    constexpr StringLiteral(const char (&str)[N]) { std::copy_n(str, N, value); }
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
struct bt_gatt_ccc_managed_user_data_with_app_user_data {
    bt_gatt_ccc_managed_user_data ccc_managed;  // !!! MUST BE THE FIRST ELEMENT IN THIS STRUCT !!!
    void *app_user_data;
};

/**
 * @brief Shared implementation for explicit and auto UUID characteristics.
 *
 * Implements common attribute tuple generation, read/write callbacks,
 * notification plumbing, value storage, and assignment semantics.
 */
template <typename Self, StringLiteral Description, bool Notify, bool ReadOnly, typename T,
          T Default>
class BtGattCharacteristicCommon : public BtGattAttrProviderBase {
   public:
    /**
     * @brief Returns all attributes contributed by this characteristic.
     *
     * Includes characteristic declaration/value/CUD/CPF and conditionally CCC
     * when notifications are enabled.
     */
    static_assert(
        BtGattCpfTraits<T>::kSupported,
        "Unsupported type for BtGattCharacteristic CPF deduction. "
        "Add a BtGattCpfTraits<T> specialization with a static constexpr bt_gatt_cpf kValue.");

    /**
     * @brief Exposes this characteristic's compile-time CUD description string without
     * needing an instance. Used by MetadataBlobBuilder (issue #41 follow-up) to build a
     * per-service bulk metadata blob purely from provider TYPES. Every concrete
     * characteristic type (BtGattCharacteristic, BtGattAutoCharacteristicExt-derived CRTP
     * subclasses, BtGattPersistentCharacteristic, etc.) inherits this transparently via
     * ordinary derived-class static-member lookup.
     */
    static constexpr const char *getDescription() { return Description.value; }

    /**
     * @brief Exposes this characteristic's compile-time CPF format byte - the same value
     * written into the standard 0x2904 CPF descriptor below. See getDescription() above.
     */
    static constexpr bt_gatt_cpf getCpf() { return BtGattCpfTraits<T>::kValue; }

    constexpr auto getAttrsTuple() {
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
                .perm = BT_GATT_PERM_READ_ENCRYPT |
                        (ReadOnly ? 0 : BT_GATT_PERM_WRITE_ENCRYPT | BT_GATT_PERM_PREPARE_WRITE),
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

        if constexpr (Notify) {
            auto notifyAttrs = std::make_tuple(bt_gatt_attr{
                .uuid = &kGattCccUuid.uuid,
                .read = bt_gatt_attr_read_ccc,
                .write = bt_gatt_attr_write_ccc,
                .user_data = &ccc_data_,
                .handle = 0,
                .perm = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
            });

            return std::tuple_cat(baseAttrs, notifyAttrs);
        } else {
            return baseAttrs;
        }
    }

    void notify() {
        if constexpr (Notify) {
            bt_gatt_attr *attr = getAttr(1);

            if (!sendNotifications_) {
                // Expected whenever no central is subscribed to this characteristic (not
                // connected, or connected but the app hasn't written the CCCD yet) - not an
                // error, so don't log it. Left commented (not deleted) to match the sibling
                // debug traces below, which are also silenced by default for the same reason.
                // printk("%p Notifications not enabled, skipping\n", attr);
                return;
            }

            if (!bleAnyConnEncrypted()) {
                // Expected right after a fresh connect, for the duration of LE Secure
                // Connections pairing - bt_gatt_notify() would reject this the same
                // way (see bleAnyConnEncrypted()'s doc comment) and log a WRN doing
                // it. Skip silently and let the next periodic update retry once
                // encryption completes; nothing is lost, this characteristic always
                // holds its latest value for read() regardless of notify outcome.
                return;
            }

            // printk("NOTIFY: %p\n", attr);
            // BtGattNotifyTraits controls how much of storage_ actually gets sent - by default
            // the full string length (matching read()), but BtGattDropdownList<N> overrides
            // this to just its first token, since notify (unlike read) can't fragment across
            // multiple ATT PDUs and must fit within the connection's negotiated MTU. See
            // BtGattNotifyTraits in bt_gatt_traits.h for the full rationale.
            int ret;
            size_t payloadLen = BtGattNotifyTraits<T>::length(storage_);
            if constexpr (BtGattStringTraits<T>::kIsString) {
                ret = bt_gatt_notify(NULL, attr, storage_.data(), payloadLen);
            } else {
                ret = bt_gatt_notify(NULL, attr, &storage_, payloadLen);
            }
            if (ret != 0) {
                // Names the characteristic so an MTU-related failure (the kernel's own
                // "No ATT channel for MTU ..." / "No buffer available" warnings don't say which
                // characteristic triggered them) can be traced back to its Description without
                // guessing. ret == -ENOMEM here most often means the payload (payloadLen + 3-byte
                // ATT header) exceeds the connection's negotiated ATT MTU.
                printk("Notify failed for \"%s\": %d (payload %zu bytes)\n", Description.value,
                      ret, payloadLen);
            } else {
                // printk("Notify succeeded\n");
            }
        } else {
            // printk("NOTIFY: Notifications not enabled for this characteristic\n");
        }
    }

    static void cccCfgChanged(const struct bt_gatt_attr *attr, uint16_t value) {
        bt_gatt_ccc_managed_user_data_with_app_user_data *managed_user_data =
            reinterpret_cast<bt_gatt_ccc_managed_user_data_with_app_user_data *>(
                const_cast<struct bt_gatt_attr *>(attr)->user_data);
        Self *instance = reinterpret_cast<Self *>(managed_user_data->app_user_data);

        instance->sendNotifications_ = (value == BT_GATT_CCC_NOTIFY);
        // printk("%p Notification state: %d\n", attr, instance->sendNotifications_);
    }

    static ssize_t read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                        uint16_t len, uint16_t offset) {
        Self *instance =
            reinterpret_cast<Self *>(const_cast<struct bt_gatt_attr *>(attr)->user_data);

        if constexpr (BtGattStringTraits<T>::kIsString) {
            const size_t stringLen =
                strnlen(instance->storage_.data(), BtGattStringTraits<T>::kMaxLen);
            return bt_gatt_attr_read(conn, attr, buf, len, offset, instance->storage_.data(),
                                     stringLen);
        }

        return bt_gatt_attr_read(conn, attr, buf, len, offset, &instance->storage_,
                                 sizeof(instance->storage_));
    }

    static ssize_t write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                         uint16_t len, uint16_t offset, uint8_t flags) {
        Self *instance =
            reinterpret_cast<Self *>(const_cast<struct bt_gatt_attr *>(attr)->user_data);
        return _write(conn, attr, buf, len, offset, flags, instance, instance->storage_);
    }

    T &operator=(const T &other) {
        if (storage_ != other) {
            storage_ = other;
            notify();
        }

        return storage_;
    }

    operator T() { return storage_; }

    const T &value() const { return storage_; }

   protected:
    bt_uuid_128 characteristic_uuid_{};

   private:
    static constexpr bt_uuid_16 kGattChrcUuid = BT_UUID_INIT_16(BT_UUID_GATT_CHRC_VAL);
    static constexpr bt_uuid_16 kGattCudUuid = BT_UUID_INIT_16(BT_UUID_GATT_CUD_VAL);
    static constexpr bt_uuid_16 kGattCpfUuid = BT_UUID_INIT_16(BT_UUID_GATT_CPF_VAL);
    static constexpr bt_uuid_16 kGattCccUuid = BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);

    T storage_ = Default;
    bool sendNotifications_ = false;
    bt_gatt_cpf characteristic_cpf_ = BtGattCpfTraits<T>::kValue;
    // The properties byte must reflect ReadOnly: advertising WRITE on a read-only
    // characteristic made BLE clients (e.g. react-native-ble-plx's isWritableWithResponse)
    // believe every characteristic was writable, so the companion app rendered editable
    // inputs for values whose writes could only ever fail at the ATT permission layer.
    bt_gatt_chrc characteristic_ =
        BT_GATT_CHRC_INIT(&characteristic_uuid_.uuid, 0U,
                          BT_GATT_CHRC_READ | (ReadOnly ? 0U : BT_GATT_CHRC_WRITE) |
                              (Notify ? BT_GATT_CHRC_NOTIFY : 0U));
    bt_gatt_ccc_managed_user_data_with_app_user_data ccc_data_ = {
        .ccc_managed = BT_GATT_CCC_MANAGED_USER_DATA_INIT(cccCfgChanged, NULL, NULL),
        .app_user_data = this,
    };
};

/**
 * @brief Explicit-UUID characteristic wrapper over @ref BtGattCharacteristicCommon.
 *
 * Sets UUID storage to the compile-time `CharacteristicUuid` value.
 */
template <bt_uuid_128 CharacteristicUuid, StringLiteral Description, bool Notify, bool ReadOnly,
          typename T, T Default>
class BtGattCharacteristic
    : public BtGattCharacteristicCommon<
          BtGattCharacteristic<CharacteristicUuid, Description, Notify, ReadOnly, T, Default>,
          Description, Notify, ReadOnly, T, Default> {
   public:
    using Base = BtGattCharacteristicCommon<
        BtGattCharacteristic<CharacteristicUuid, Description, Notify, ReadOnly, T, Default>,
        Description, Notify, ReadOnly, T, Default>;
    using Base::operator=;

    BtGattCharacteristic() { this->characteristic_uuid_ = CharacteristicUuid; }
};

/**
 * @brief CRTP-extensible auto-UUID characteristic wrapper.
 *
 * Lets external characteristic types provide their own `Self` type while
 * retaining auto UUID assignment support.
 */
template <typename Self, StringLiteral Description, bool Notify, bool ReadOnly, typename T,
          T Default>
class BtGattAutoCharacteristicExt
    : public BtGattCharacteristicCommon<Self, Description, Notify, ReadOnly, T, Default> {
   public:
    using Base = BtGattCharacteristicCommon<Self, Description, Notify, ReadOnly, T, Default>;
    using Base::operator=;

    void assignAutoUuid(const bt_uuid_128 &serviceUuid, uint16_t characteristicId) {
        this->characteristic_uuid_ = composeAutoCharacteristicUuid(serviceUuid, characteristicId);
    }
};

/**
 * @brief Auto-UUID characteristic wrapper over @ref BtGattCharacteristicCommon.
 *
 * UUID is assigned by @ref BtGattServer via @ref assignAutoUuid.
 */
template <StringLiteral Description, bool Notify, bool ReadOnly, typename T, T Default>
class BtGattAutoCharacteristic
    : public BtGattAutoCharacteristicExt<
          BtGattAutoCharacteristic<Description, Notify, ReadOnly, T, Default>, Description, Notify,
          ReadOnly, T, Default> {
   public:
    using Base = BtGattAutoCharacteristicExt<
        BtGattAutoCharacteristic<Description, Notify, ReadOnly, T, Default>, Description, Notify,
        ReadOnly, T, Default>;
    using Base::operator=;
};

// Specialized version of BtGattCharacteristic for combinations of read-only/read-write,
// notify/no-notify characteristics

template <bt_uuid_128 CharacteristicUuid, StringLiteral Description, typename T, T Default>
using BtGattReadWriteCharacteristic =
    BtGattCharacteristic<CharacteristicUuid, Description, false /* Notify */, false /* ReadOnly */,
                         T, Default>;

template <bt_uuid_128 CharacteristicUuid, StringLiteral Description, typename T, T Default>
using BtGattReadWriteNotifyCharacteristic =
    BtGattCharacteristic<CharacteristicUuid, Description, true /* Notify */, false /* ReadOnly */,
                         T, Default>;

template <bt_uuid_128 CharacteristicUuid, StringLiteral Description, typename T, T Default>
using BtGattReadNotifyCharacteristic =
    BtGattCharacteristic<CharacteristicUuid, Description, true /* Notify */, true /* ReadOnly */, T,
                         Default>;

template <bt_uuid_128 CharacteristicUuid, StringLiteral Description, typename T, T Default>
using BtGattReadOnlyCharacteristic =
    BtGattCharacteristic<CharacteristicUuid, Description, false /* Notify */, true /* ReadOnly */,
                         T, Default>;

// Auto aliases derive UUIDs from BtGattServer provider order; use explicit UUID aliases if UUID
// stability across reordering is required.
template <StringLiteral Description, typename T, T Default>
using BtGattAutoReadWriteCharacteristic =
    BtGattAutoCharacteristic<Description, false /* Notify */, false /* ReadOnly */, T, Default>;

template <StringLiteral Description, typename T, T Default>
using BtGattAutoReadWriteNotifyCharacteristic =
    BtGattAutoCharacteristic<Description, true /* Notify */, false /* ReadOnly */, T, Default>;

template <StringLiteral Description, typename T, T Default>
using BtGattAutoReadNotifyCharacteristic =
    BtGattAutoCharacteristic<Description, true /* Notify */, true /* ReadOnly */, T, Default>;

template <StringLiteral Description, typename T, T Default>
using BtGattAutoReadOnlyCharacteristic =
    BtGattAutoCharacteristic<Description, false /* Notify */, true /* ReadOnly */, T, Default>;