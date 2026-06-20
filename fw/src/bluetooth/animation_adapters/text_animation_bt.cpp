#include <animations/animation_is_active_binding.h>
#include <animations/text_animation.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>

#include <cstring>

constexpr bt_uuid_128 kTextConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Text));

BtGattPrimaryService<kTextConfigServiceUuid> textPrimaryService;
BtGattPersistentCharacteristic<"text/step_time_ms", "Step Time Ms", false, uint32_t, 50>
    textStepTimeMs;
BtGattPersistentCharacteristic<"text/color", "Color", false, BtGattColor, BtGattColor{0xFFFFFFFF}>
    textColor;
BtGattPersistentCharacteristic<"text/up_next", "Up Next", true, uint32_t, 0> textUpNext;

constexpr BtGattString<TextAnimation::kMaxMsgLen> kEmptyTextSlot = {};
BtGattPersistentCharacteristic<"text/slot0", "Slot 0", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot0;
BtGattPersistentCharacteristic<"text/slot1", "Slot 1", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot1;
BtGattPersistentCharacteristic<"text/slot2", "Slot 2", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot2;
BtGattPersistentCharacteristic<"text/slot3", "Slot 3", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot3;
BtGattPersistentCharacteristic<"text/slot4", "Slot 4", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot4;
BtGattPersistentCharacteristic<"text/slot5", "Slot 5", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot5;
BtGattPersistentCharacteristic<"text/slot6", "Slot 6", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot6;
BtGattPersistentCharacteristic<"text/slot7", "Slot 7", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot7;
BtGattPersistentCharacteristic<"text/slot8", "Slot 8", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot8;
BtGattPersistentCharacteristic<"text/slot9", "Slot 9", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot9;
BtGattPersistentCharacteristic<"text/slot10", "Slot 10", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot10;
BtGattPersistentCharacteristic<"text/slot11", "Slot 11", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot11;
BtGattPersistentCharacteristic<"text/slot12", "Slot 12", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot12;
BtGattPersistentCharacteristic<"text/slot13", "Slot 13", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot13;
BtGattPersistentCharacteristic<"text/slot14", "Slot 14", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot14;
BtGattPersistentCharacteristic<"text/slot15", "Slot 15", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot15;
BtGattPersistentCharacteristic<"text/slot16", "Slot 16", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot16;
BtGattPersistentCharacteristic<"text/slot17", "Slot 17", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot17;
BtGattPersistentCharacteristic<"text/slot18", "Slot 18", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot18;
BtGattPersistentCharacteristic<"text/slot19", "Slot 19", false,
                               BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot>
    textSlot19;

using TextIsActiveCharacteristic = IsActiveCharacteristic<Animation::Text>;
TextIsActiveCharacteristic textIsActive;

constexpr BtGattString<24> kTextAnimationName = makeBtGattString<24>("Text");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kTextAnimationName>
    textAnimationName;

BtGattAutoReadNotifyCharacteristic<"Now Playing", uint32_t, 0> nowPlayingCharacteristic;

BtGattServer textConfigServer(textPrimaryService, textStepTimeMs, textColor, textUpNext, textSlot0,
                              textSlot1, textSlot2, textSlot3, textSlot4, textSlot5, textSlot6,
                              textSlot7, textSlot8, textSlot9, textSlot10, textSlot11, textSlot12,
                              textSlot13, textSlot14, textSlot15, textSlot16, textSlot17,
                              textSlot18, textSlot19, textIsActive, textAnimationName,
                              nowPlayingCharacteristic);
BT_GATT_SERVER_REGISTER(textConfigServerStatic, textConfigServer);

namespace {
class TextStepTimeSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return textStepTimeMs; }
};

class TextColorSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return static_cast<BtGattColor>(textColor); }
};
}  // namespace

using TextAnimationIsActive = AnimationIsActiveBinding<Animation::Text>;

static void text_set_is_active(bool active) {
    textIsActive.setActive(active);
}

struct TextIsActiveBindingRegistrar {
    TextIsActiveBindingRegistrar() { TextAnimationIsActive::registerSetter(text_set_is_active); }
};

[[maybe_unused]] TextIsActiveBindingRegistrar sTextIsActiveBindingRegistrar;

const char* kStaticMessages[TextAnimation::kNumStringSlots] = {
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

namespace {
static const char* getTextSlot(size_t slot) {
    switch (slot) {
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

static void setTextSlot(size_t slot, const char* value) {
    BtGattString<TextAnimation::kMaxMsgLen> storage = {};
    strncpy(storage.data(), value, TextAnimation::kMaxMsgLen - 1);

    switch (slot) {
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

class TextSlotSource : public TextAnimationSlotSource {
   public:
    const char* getStringFromSlot(size_t slot) const override { return getTextSlot(slot); }
};

class TextUpNextSource : public TextAnimationUpNextSource {
   public:
    size_t consumeCurrentAndAdvance(size_t numSlots) override {
        uint32_t currUpNext = textUpNext;
        uint32_t nextUpNext = currUpNext + 1;
        if (nextUpNext >= numSlots) {
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
TextAnimationDependencies sDefaultTextDeps(sDefaultStepTimeSource, sDefaultColorSource,
                                           sDefaultSlotSource, sDefaultUpNextSource);

struct TextSlotInitializer {
    TextSlotInitializer() {
        for (size_t i = 0; i < TextAnimation::kNumStringSlots; i++) {
            setTextSlot(i, kStaticMessages[i]);
        }
    }
};

[[maybe_unused]] TextSlotInitializer sTextSlotInitializer;
}  // namespace

void text_animation_bind_default_dependencies() {
    TextAnimation::getInstance()->setDependencies(sDefaultTextDeps);
}
