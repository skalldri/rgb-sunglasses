#include <animations/my_eyes_animation.h>
#include <animations/animation_is_active_binding.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

#include <zephyr/bluetooth/uuid.h>

#include <cstring>

constexpr bt_uuid_128 kMyEyesConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x0500, 0x56789abd0000));

BtGattPrimaryService<kMyEyesConfigServiceUuid> myEyesPrimaryService;
BtGattAutoReadWriteCharacteristic<"Blink Speed Ms", uint32_t, 100> myEyesBlinkSpeedMs;
BtGattAutoReadWriteCharacteristic<"Color", BtGattColor, BtGattColor{0xFFFFFFFF}> myEyesColor;
BtGattAutoReadWriteNotifyCharacteristic<"Up Next", uint32_t, 0> myEyesUpNext;

constexpr BtGattString<MyEyesAnimation::kMaxEyeLen> kEmptyEyeSlot = {};
BtGattAutoReadWriteCharacteristic<"Slot 0", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot0;
BtGattAutoReadWriteCharacteristic<"Slot 1", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot1;
BtGattAutoReadWriteCharacteristic<"Slot 2", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot2;
BtGattAutoReadWriteCharacteristic<"Slot 3", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot3;
BtGattAutoReadWriteCharacteristic<"Slot 4", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot4;
BtGattAutoReadWriteCharacteristic<"Slot 5", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot5;
BtGattAutoReadWriteCharacteristic<"Slot 6", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot6;
BtGattAutoReadWriteCharacteristic<"Slot 7", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot7;
BtGattAutoReadWriteCharacteristic<"Slot 8", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot8;
BtGattAutoReadWriteCharacteristic<"Slot 9", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot9;
BtGattAutoReadWriteCharacteristic<"Slot 10", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot10;
BtGattAutoReadWriteCharacteristic<"Slot 11", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot11;
BtGattAutoReadWriteCharacteristic<"Slot 12", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot12;
BtGattAutoReadWriteCharacteristic<"Slot 13", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot13;
BtGattAutoReadWriteCharacteristic<"Slot 14", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot14;
BtGattAutoReadWriteCharacteristic<"Slot 15", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot15;
BtGattAutoReadWriteCharacteristic<"Slot 16", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot16;
BtGattAutoReadWriteCharacteristic<"Slot 17", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot17;
BtGattAutoReadWriteCharacteristic<"Slot 18", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot18;
BtGattAutoReadWriteCharacteristic<"Slot 19", BtGattString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot> myEyesSlot19;

using MyEyesIsActiveCharacteristic = IsActiveCharacteristic<Animation::MyEyes>;
MyEyesIsActiveCharacteristic myEyesIsActive;

BtGattServer myEyesConfigServer(
    myEyesPrimaryService,
    myEyesBlinkSpeedMs,
    myEyesColor,
    myEyesUpNext,
    myEyesSlot0,
    myEyesSlot1,
    myEyesSlot2,
    myEyesSlot3,
    myEyesSlot4,
    myEyesSlot5,
    myEyesSlot6,
    myEyesSlot7,
    myEyesSlot8,
    myEyesSlot9,
    myEyesSlot10,
    myEyesSlot11,
    myEyesSlot12,
    myEyesSlot13,
    myEyesSlot14,
    myEyesSlot15,
    myEyesSlot16,
    myEyesSlot17,
    myEyesSlot18,
    myEyesSlot19,
    myEyesIsActive);
BT_GATT_SERVER_REGISTER(myEyesConfigServerStatic, myEyesConfigServer);

using MyEyesAnimationIsActive = AnimationIsActiveBinding<Animation::MyEyes>;

static void my_eyes_set_is_active(bool active)
{
    myEyesIsActive.setActive(active);
}

struct MyEyesIsActiveBindingRegistrar
{
    MyEyesIsActiveBindingRegistrar()
    {
        MyEyesAnimationIsActive::registerSetter(my_eyes_set_is_active);
    }
};

[[maybe_unused]] MyEyesIsActiveBindingRegistrar sMyEyesIsActiveBindingRegistrar;

namespace
{
    class MyEyesBlinkSpeedSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return myEyesBlinkSpeedMs;
        }
    };

    class MyEyesColorSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return static_cast<BtGattColor>(myEyesColor);
        }
    };
}

const char *kStaticEyes[MyEyesAnimation::kNumStringSlots] = {
    "^^",
    "||",
    "><",
    "XX",
    "--",
    "**",
    "??",
    "##",
    "@@",
    "!!",
    "oo",
    "\"\"",
    "==",
    "88",
    "$$",
    "~~",
    "00",
    "qq",
    "TT",
    "UU",
};

namespace
{
    static const char *getMyEyesSlot(size_t slot)
    {
        switch (slot)
        {
        case 0:
            return myEyesSlot0.value().data();
        case 1:
            return myEyesSlot1.value().data();
        case 2:
            return myEyesSlot2.value().data();
        case 3:
            return myEyesSlot3.value().data();
        case 4:
            return myEyesSlot4.value().data();
        case 5:
            return myEyesSlot5.value().data();
        case 6:
            return myEyesSlot6.value().data();
        case 7:
            return myEyesSlot7.value().data();
        case 8:
            return myEyesSlot8.value().data();
        case 9:
            return myEyesSlot9.value().data();
        case 10:
            return myEyesSlot10.value().data();
        case 11:
            return myEyesSlot11.value().data();
        case 12:
            return myEyesSlot12.value().data();
        case 13:
            return myEyesSlot13.value().data();
        case 14:
            return myEyesSlot14.value().data();
        case 15:
            return myEyesSlot15.value().data();
        case 16:
            return myEyesSlot16.value().data();
        case 17:
            return myEyesSlot17.value().data();
        case 18:
            return myEyesSlot18.value().data();
        case 19:
            return myEyesSlot19.value().data();
        default:
            return "00";
        }
    }

    static void setMyEyesSlot(size_t slot, const char *value)
    {
        BtGattString<MyEyesAnimation::kMaxEyeLen> storage = {};
        strncpy(storage.data(), value, MyEyesAnimation::kMaxEyeLen - 1);

        switch (slot)
        {
        case 0:
            myEyesSlot0 = storage;
            break;
        case 1:
            myEyesSlot1 = storage;
            break;
        case 2:
            myEyesSlot2 = storage;
            break;
        case 3:
            myEyesSlot3 = storage;
            break;
        case 4:
            myEyesSlot4 = storage;
            break;
        case 5:
            myEyesSlot5 = storage;
            break;
        case 6:
            myEyesSlot6 = storage;
            break;
        case 7:
            myEyesSlot7 = storage;
            break;
        case 8:
            myEyesSlot8 = storage;
            break;
        case 9:
            myEyesSlot9 = storage;
            break;
        case 10:
            myEyesSlot10 = storage;
            break;
        case 11:
            myEyesSlot11 = storage;
            break;
        case 12:
            myEyesSlot12 = storage;
            break;
        case 13:
            myEyesSlot13 = storage;
            break;
        case 14:
            myEyesSlot14 = storage;
            break;
        case 15:
            myEyesSlot15 = storage;
            break;
        case 16:
            myEyesSlot16 = storage;
            break;
        case 17:
            myEyesSlot17 = storage;
            break;
        case 18:
            myEyesSlot18 = storage;
            break;
        case 19:
            myEyesSlot19 = storage;
            break;
        }
    }

    class MyEyesSlotSource : public MyEyesAnimationSlotSource
    {
    public:
        const char *getStringFromSlot(size_t slot) const override
        {
            return getMyEyesSlot(slot);
        }
    };

    class MyEyesUpNextSource : public MyEyesAnimationUpNextSource
    {
    public:
        size_t consumeCurrentAndAdvance(size_t numSlots) override
        {
            uint32_t currUpNext = myEyesUpNext;
            uint32_t nextUpNext = currUpNext + 1;
            if (nextUpNext >= numSlots)
            {
                nextUpNext = 0;
            }

            myEyesUpNext = nextUpNext;
            return currUpNext;
        }
    };

    MyEyesBlinkSpeedSource sDefaultBlinkSpeedSource;
    MyEyesColorSource sDefaultColorSource;
    MyEyesSlotSource sDefaultSlotSource;
    MyEyesUpNextSource sDefaultUpNextSource;

    MyEyesAnimationDependencies sDefaultMyEyesDeps(
        sDefaultBlinkSpeedSource,
        sDefaultColorSource,
        sDefaultSlotSource,
        sDefaultUpNextSource);

    struct MyEyesSlotInitializer
    {
        MyEyesSlotInitializer()
        {
            for (size_t i = 0; i < MyEyesAnimation::kNumStringSlots; i++)
            {
                setMyEyesSlot(i, kStaticEyes[i]);
            }
        }
    };

    [[maybe_unused]] MyEyesSlotInitializer sMyEyesSlotInitializer;
}

void my_eyes_animation_bind_default_dependencies()
{
    MyEyesAnimation::getInstance()->setDependencies(sDefaultMyEyesDeps);
}
