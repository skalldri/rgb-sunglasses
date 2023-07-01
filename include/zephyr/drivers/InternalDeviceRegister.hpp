#include <tuple>
#include <type_traits>

#include <cstdint>

// Let's build a templated class to make handling internal device registers easier

template <size_t firstBit, size_t bitWidth>
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
    static uint32_t setValue(uint32_t reg, uin32_t value)
    {
        uint32_t mask = (~(((1 << bitWidth) - 1) << firstBit));
        reg &= mask;
        reg |= (value << firstBit);

        return reg;
    }
};

template <typename T>
struct is_RegisterField : std::false_type
{
};

template <size_t firstBit, size_t bitWidth>
struct is_RegisterField<RegisterField<firstBit, bitWidth>> : std::true_type
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

template <size_t registerWidth, typename U, typename... Us>
class DeviceRegister
{
    static_assert(is_RegisterField<U>::value,
                  "U must be of type RegisterField");
    static_assert((is_RegisterField<Us>::value && ...),
                  "Us must be of type RegisterField");
    static_assert(are_unique<U, Us...>::value,
                  "Template parameters must be unique types.");
    static_assert(registerWidth == 8 || registerWidth == 16 || registerWidth == 32,
                  "Only support 8, 16, or 32-bit registers.");

public:
    template <typename T>
    uint32_t get()
    {
        static_assert(is_RegisterField<T>::value,
                      "T must be of type RegisterField");

        return T::getValue(storage_);
    }

    template <typename T>
    void set(uint32_t value)
    {
        static_assert(is_RegisterField<T>::value,
                      "T must be of type RegisterField");

        storage_ = T::setValue(storage_, value);
    }

    uint32_t *data()
    {
        return &storage_;
    }

private:
    uint32_t storage_;
};