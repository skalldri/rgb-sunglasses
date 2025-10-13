#include <tuple>
#include <type_traits>

#include <cstdint>

// Let's build a templated class to make handling internal device registers easier

// This class is the default class for unit conversion of a register field. It applies no conversion
// and simply returns the register value directly
class Unitless
{
public:
    static inline const char *unit()
    {
        return "";
    }

    static inline int64_t conversion(uint32_t val)
    {
        return val;
    }
};

template <const char *name, size_t firstBit, size_t bitWidth, class UnitConversion = Unitless>
class RegisterField
{
    static_assert(bitWidth > 0 && bitWidth <= 32,
                  "bitWidth must be 1-32");
    static_assert(firstBit >= 0 && firstBit < 31,
                  "firstBit must be 0-31");
    static_assert((firstBit + bitWidth) <= 32,
                  "firstBit + bitWidth must be <= 32");

public:
    /**
     * @brief Get the register field value from the provided uint32_t based on the firstBit and bitWidth settings
     *
     * @param reg the full value of the register
     * @return uint32_t the value of this field in the register
     */
    static uint32_t getValue(uint32_t reg)
    {
        return (reg >> firstBit) & ((1 << bitWidth) - 1);
    }

    /**
     * @brief Change the value of this field within the register
     *
     * @param reg
     * @return uint32_t
     */
    static uint32_t setValue(uint32_t reg, uint32_t value)
    {
        uint32_t mask = (~(((1 << bitWidth) - 1) << firstBit));
        reg &= mask;
        reg |= (value << firstBit);

        return reg;
    }

    static const char *getName()
    {
        return name;
    }

    static const char *getUnit()
    {
        return UnitConversion::unit();
    }

    static int64_t conversion(uint32_t val)
    {
        return UnitConversion::conversion(val);
    }
};

/*
template <typename T>
struct is_RegisterField : std::false_type
{
};

template <const char *name, size_t firstBit, size_t bitWidth, class UnitConversion = Unitless>
struct is_RegisterField<RegisterField<name, firstBit, bitWidth, UnitConversion>> : std::true_type
{
};

template <typename... Ts>
struct are_unique_impl
{
};

template <typename T, typename... Ts>
struct are_unique_impl<T, Ts...>
    : std::conditional_t<(is_RegisterField<T>::value &&
                          are_unique_impl<Ts...>::value &&
                          !std::disjunction_v<std::is_same<T, Ts>...>),
                         std::true_type, std::false_type>
{
};

template <>
struct are_unique_impl<> : std::true_type
{
};

template <typename... Ts>
using are_unique = are_unique_impl<Ts...>;
*/

template <const char *name, size_t registerWidth, typename U, typename... Us>
class DeviceRegister
{
    /*
    static_assert(is_RegisterField<U>::value,
                  "U must be of type RegisterField");
    static_assert((is_RegisterField<Us>::value && ...),
                  "Us must be of type RegisterField");
    static_assert(are_unique<U, Us...>::value,
                  "Template parameters must be unique types.");
    static_assert(registerWidth == 8 || registerWidth == 16 || registerWidth == 32,
                  "Only support 8, 16, or 32-bit registers.");
    */

public:
    DeviceRegister() : storage_(0) {}

protected:
    /**
     * @brief Set a register field value in the internal storage
     *
     * @tparam T
     * @return uint32_t
     */
    template <typename T>
    uint32_t getRegFieldFromStorage(void)
    {
        /*static_assert(is_RegisterField<T>::value,
                      "T must be of type RegisterField");*/

        return T::getValue(storage_);
    }

    template <typename T>
    void setRegFieldInStorage(uint32_t value)
    {
        /*static_assert(is_RegisterField<T>::value,
                      "T must be of type RegisterField");*/

        storage_ = T::setValue(storage_, value);
    }

    uint32_t *data()
    {
        return &storage_;
    }

    const char *getName()
    {
        return name;
    }

    uint32_t storage_;

    static constexpr size_t regWidth_ = registerWidth;
};

/**
 * @brief
 *
 * @tparam reg
 * @tparam registerWidth
 * @tparam U
 * @tparam Us
 */
template <const char *name, uint8_t reg, size_t registerWidth, typename U, typename... Us>
class I2CDeviceRegister : public DeviceRegister<name, registerWidth, U, Us...>
{
public:
    /**
     * @brief Construct a new I2CDeviceRegister object. Performs an initial read of the register.
     *
     * @param i2c I2C device object to use for interaction
     */
    I2CDeviceRegister(const struct i2c_dt_spec *i2c) : i2c_(i2c)
    {
        read();
    }

    /**
     * @brief Destroy the I2CDeviceRegister object. If the object contains writes that have not been flushed, write them now.
     *
     */
    ~I2CDeviceRegister()
    {
        if (dirty_)
        {
            flush();
        }
    }

    /**
     * @brief Print the contents of all fields in the register
     *
     * @param read optional, if true perform a read before dumping
     */
    void dump(bool read = false)
    {
#if defined(CONFIG_DUMP_DEVICE_REGISTERS)
        if (read)
        {
            this->read();
        }

        LOG_INF("%s", this->getName());

        dumpHelper<U, Us...>();
#endif
    }

    /**
     * @brief Writeback the contents of storage to the device register
     *
     * @return int 0 on success, -errno on failure
     */
    int flush()
    {
        if (this->regWidth_ == 8)
        {
            LOG_INF("%s: writing 0x%02x", this->getName(), this->storage_);
        }
        else if (this->regWidth_ == 16)
        {
            LOG_INF("%s: writing 0x%04x", this->getName(), this->storage_);
        }
        else
        {
            LOG_INF("%s: writing 0x%08x", this->getName(), this->storage_);
        }

        // TODO: probably also need endian-swapping logic here

        int ret = i2c_burst_write_dt(
            i2c_,
            reg,
            reinterpret_cast<uint8_t *>(&this->storage_),
            this->regWidth_ / 8);

        if (ret)
        {
            LOG_ERR("I2C Write failed: %d", ret);
            return ret;
        }

        dirty_ = false;
        return 0;
    }

    /**
     * @brief Readback the contents of the device register to internal storage
     *
     * @return int 0 on success, -errno on failure
     */
    int read()
    {
        int ret = i2c_burst_read_dt(
            i2c_,
            reg,
            reinterpret_cast<uint8_t *>(&this->storage_),
            this->regWidth_ / 8);

        if (ret)
        {
            LOG_ERR("I2C Read failed: %d", ret);
            return ret;
        }

        // TODO: might need to make this logic configurable...?
        // Handle big/little endian conversion
        if (this->regWidth_ == 16)
        {
            uint8_t *storage = reinterpret_cast<uint8_t *>(&this->storage_);
            uint8_t tmp = storage[0];
            storage[0] = storage[1];
            storage[1] = tmp;
        }
        else if (this->regWidth_ == 32)
        {
            uint8_t *storage = reinterpret_cast<uint8_t *>(&this->storage_);

            // Swap first and last byte
            uint8_t tmp = storage[0];
            storage[0] = storage[3];
            storage[3] = tmp;

            // Swap middle bytes
            tmp = storage[1];
            storage[1] = storage[2];
            storage[2] = tmp;
        }

        // Cannot be dirty if we just read from the device!
        dirty_ = false;
        return 0;
    }

    /**
     * @brief
     *
     * @tparam T
     * @param val
     * @param flush
     */
    template <typename T>
    int set(const uint32_t &val, bool flush = false)
    {
        this->template setRegFieldInStorage<T>(val);
        dirty_ = true;

        if (flush)
        {
            return this->flush();
        }

        return 0;
    }

    /**
     * @brief
     *
     * @tparam T
     * @param val
     * @param read
     * @return uint32_t
     */
    template <typename T>
    uint32_t get(const uint32_t &val, const bool &read = false)
    {
        if (read)
        {
            this->read();
        }

        return this->template getRegFieldFromStorage<T>();
    }

private:
    template <typename Ta, typename Tb, typename... Ts>
    inline void dumpHelper()
    {
        this->template dumpHelper<Ta>();
        this->template dumpHelper<Tb, Ts...>();
    }

    template <typename T>
    inline void dumpHelper()
    {
        /*static_assert(is_RegisterField<T>::value,
                      "T must be of type RegisterField");*/

        LOG_INF("\t%s: %lld %s", T::getName(), T::conversion(this->template getRegFieldFromStorage<T>()), T::getUnit());
    }

private:
    const struct i2c_dt_spec *i2c_;
    bool dirty_ = false; // Indicates if a write is needed
};

/*
template <typename U, typename... Us>
class DumpableDevice
{
public:
    void dump()
    {
        dumpHelper<U, Us...>();
    }

private:
    template <typename Ta, typename Tb, typename... Ts>
    void dumpHelper()
    {
    }

    template <typename T>
    inline void dumpHelper() {}
};
*/