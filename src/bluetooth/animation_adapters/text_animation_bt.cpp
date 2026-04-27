#include <animations/text_animation.h>
#include <animations/animation_is_active_binding.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

#include <zephyr/bluetooth/uuid.h>

#include <cstring>

constexpr bt_uuid_128 kNowPlayingServiceUuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xdeadbeef, 0x1234, 0x5678, 0x1234, 0x56789abcdef0));
BtGattPrimaryService<kNowPlayingServiceUuid> nowPlayingPrimaryService;

BtGattAutoReadNotifyCharacteristic<"Now Playing", uint32_t, 0> nowPlayingCharacteristic;

BtGattServer nowPlayingServer(nowPlayingPrimaryService, nowPlayingCharacteristic);
BT_GATT_SERVER_REGISTER(nowPlayingServerStatic, nowPlayingServer);

constexpr bt_uuid_128 kTextConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x0300, 0x56789abd0000));

BtGattPrimaryService<kTextConfigServiceUuid> textPrimaryService;
BtGattAutoReadWriteCharacteristic<"Step Time Ms", uint32_t, 50> textStepTimeMs;
BtGattAutoReadWriteCharacteristic<"Color", BtGattColor, BtGattColor{0xFFFFFFFF}> textColor;
BtGattAutoReadWriteNotifyCharacteristic<"Up Next", uint32_t, 0> textUpNext;

constexpr BtGattString<TextAnimation::kMaxMsgLen> kEmptyTextSlot = {};
BtGattAutoReadWriteCharacteristic<"Slot 0", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot0;
BtGattAutoReadWriteCharacteristic<"Slot 1", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot1;
BtGattAutoReadWriteCharacteristic<"Slot 2", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot2;
BtGattAutoReadWriteCharacteristic<"Slot 3", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot3;
BtGattAutoReadWriteCharacteristic<"Slot 4", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot4;
BtGattAutoReadWriteCharacteristic<"Slot 5", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot5;
BtGattAutoReadWriteCharacteristic<"Slot 6", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot6;
BtGattAutoReadWriteCharacteristic<"Slot 7", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot7;
BtGattAutoReadWriteCharacteristic<"Slot 8", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot8;
BtGattAutoReadWriteCharacteristic<"Slot 9", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot9;
BtGattAutoReadWriteCharacteristic<"Slot 10", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot10;
BtGattAutoReadWriteCharacteristic<"Slot 11", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot11;
BtGattAutoReadWriteCharacteristic<"Slot 12", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot12;
BtGattAutoReadWriteCharacteristic<"Slot 13", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot13;
BtGattAutoReadWriteCharacteristic<"Slot 14", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot14;
BtGattAutoReadWriteCharacteristic<"Slot 15", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot15;
BtGattAutoReadWriteCharacteristic<"Slot 16", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot16;
BtGattAutoReadWriteCharacteristic<"Slot 17", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot17;
BtGattAutoReadWriteCharacteristic<"Slot 18", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot18;
BtGattAutoReadWriteCharacteristic<"Slot 19", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot19;

using TextIsActiveCharacteristic = IsActiveCharacteristic<Animation::Text>;
TextIsActiveCharacteristic textIsActive;

BtGattServer textConfigServer(
    textPrimaryService,
    textStepTimeMs,
    textColor,
    textUpNext,
    textSlot0,
    textSlot1,
    textSlot2,
    textSlot3,
    textSlot4,
    textSlot5,
    textSlot6,
    textSlot7,
    textSlot8,
    textSlot9,
    textSlot10,
    textSlot11,
    textSlot12,
    textSlot13,
    textSlot14,
    textSlot15,
    textSlot16,
    textSlot17,
    textSlot18,
    textSlot19,
    textIsActive);
BT_GATT_SERVER_REGISTER(textConfigServerStatic, textConfigServer);

namespace
{
    class TextStepTimeSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return textStepTimeMs;
        }
    };

    class TextColorSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return static_cast<BtGattColor>(textColor);
        }
    };
}

using TextAnimationIsActive = AnimationIsActiveBinding<Animation::Text>;

static void text_set_is_active(bool active)
{
    textIsActive.setActive(active);
}

struct TextIsActiveBindingRegistrar
{
    TextIsActiveBindingRegistrar()
    {
        TextAnimationIsActive::registerSetter(text_set_is_active);
    }
};

[[maybe_unused]] TextIsActiveBindingRegistrar sTextIsActiveBindingRegistrar;

const char *kStaticMessages[TextAnimation::kNumStringSlots] = {
    "LIFE IS MADE OF LITTLE MOMENTS LIKE THIS",
    "WE ARE ALL WE NEED",
    "SO LONG AND THANKS FOR ALL THE FISH",
    "ANJUNAFAM",
    "EAT SLEEP RAVE REPEAT",
    "PUSH THE BUTTON",
    "FEEL THE EARTH BENEATH YOUR FEET",
    "DREAMS ARE MADE OF NIGHTS LIKE THIS",
    "LIVE FROM THE GORGE AMPHITHEATER, THIS IS ABGT WEEKENDER 2023",
    "ART SHOULD COMFORT THE DISTURBED AND DISTURB THE COMFORTABLE",
    "LIMA OSCAR VICTOR ECHO",
    "FEEL THE EARTH BENEATH YOUR FEET",
    "#ABGT2023",
    "GROUP THERAPY",
    "YOU ARE LOVED",
    "EDM IS MY CARDIO",
    "INSERT CURRENT ARTIST HERE",
    "STAY HYDRATED",
    "LOVE CONQUERS HATE",
    "THE GORGE",
};

namespace
{
    static const char *getTextSlot(size_t slot)
    {
        switch (slot)
        {
        case 0:
            return textSlot0.value().data();
        case 1:
            return textSlot1.value().data();
        case 2:
            return textSlot2.value().data();
        case 3:
            return textSlot3.value().data();
        case 4:
            return textSlot4.value().data();
        case 5:
            return textSlot5.value().data();
        case 6:
            return textSlot6.value().data();
        case 7:
            return textSlot7.value().data();
        case 8:
            return textSlot8.value().data();
        case 9:
            return textSlot9.value().data();
        case 10:
            return textSlot10.value().data();
        case 11:
            return textSlot11.value().data();
        case 12:
            return textSlot12.value().data();
        case 13:
            return textSlot13.value().data();
        case 14:
            return textSlot14.value().data();
        case 15:
            return textSlot15.value().data();
        case 16:
            return textSlot16.value().data();
        case 17:
            return textSlot17.value().data();
        case 18:
            return textSlot18.value().data();
        case 19:
            return textSlot19.value().data();
        default:
            return "INVALID STRING SLOT";
        }
    }

    static void setTextSlot(size_t slot, const char *value)
    {
        BtGattString<TextAnimation::kMaxMsgLen> storage = {};
        strncpy(storage.data(), value, TextAnimation::kMaxMsgLen - 1);

        switch (slot)
        {
        case 0:
            textSlot0 = storage;
            break;
        case 1:
            textSlot1 = storage;
            break;
        case 2:
            textSlot2 = storage;
            break;
        case 3:
            textSlot3 = storage;
            break;
        case 4:
            textSlot4 = storage;
            break;
        case 5:
            textSlot5 = storage;
            break;
        case 6:
            textSlot6 = storage;
            break;
        case 7:
            textSlot7 = storage;
            break;
        case 8:
            textSlot8 = storage;
            break;
        case 9:
            textSlot9 = storage;
            break;
        case 10:
            textSlot10 = storage;
            break;
        case 11:
            textSlot11 = storage;
            break;
        case 12:
            textSlot12 = storage;
            break;
        case 13:
            textSlot13 = storage;
            break;
        case 14:
            textSlot14 = storage;
            break;
        case 15:
            textSlot15 = storage;
            break;
        case 16:
            textSlot16 = storage;
            break;
        case 17:
            textSlot17 = storage;
            break;
        case 18:
            textSlot18 = storage;
            break;
        case 19:
            textSlot19 = storage;
            break;
        }
    }

    class TextSlotSource : public TextAnimationSlotSource
    {
    public:
        const char *getStringFromSlot(size_t slot) const override
        {
            return getTextSlot(slot);
        }
    };

    class TextUpNextSource : public TextAnimationUpNextSource
    {
    public:
        size_t consumeCurrentAndAdvance(size_t numSlots) override
        {
            uint32_t currUpNext = textUpNext;
            uint32_t nextUpNext = currUpNext + 1;
            if (nextUpNext >= numSlots)
            {
                nextUpNext = 0;
            }

            textUpNext = nextUpNext;
            nowPlayingCharacteristic = currUpNext;

            return currUpNext;
        }
    };

    TextStepTimeSource sDefaultStepTimeSource;
    TextColorSource sDefaultColorSource;
    TextSlotSource sDefaultSlotSource;
    TextUpNextSource sDefaultUpNextSource;
    TextAnimationDependencies sDefaultTextDeps(
        sDefaultStepTimeSource,
        sDefaultColorSource,
        sDefaultSlotSource,
        sDefaultUpNextSource);

    struct TextSlotInitializer
    {
        TextSlotInitializer()
        {
            for (size_t i = 0; i < TextAnimation::kNumStringSlots; i++)
            {
                setTextSlot(i, kStaticMessages[i]);
            }
        }
    };

    [[maybe_unused]] TextSlotInitializer sTextSlotInitializer;
}

void text_animation_bind_default_dependencies()
{
    TextAnimation::getInstance()->setDependencies(sDefaultTextDeps);
}
