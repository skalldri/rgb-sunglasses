#include <animations/animation_is_active_binding.h>
#include <animations/animation_shuffle_include_binding.h>
#include <animations/color_mode_source.h>
#include <animations/my_eyes_animation.h>
#include <zephyr/random/random.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/animation_shuffle_include_characteristic.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>

#include <cstring>

constexpr bt_uuid_128 kMyEyesConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::MyEyes));

BtGattPrimaryService<kMyEyesConfigServiceUuid> myEyesPrimaryService;
BtGattPersistentCharacteristic<"my_eyes/blink_speed_ms", "Blink Speed Ms", false, uint32_t, 100>
    myEyesBlinkSpeedMs;
BtGattPersistentCharacteristic<"my_eyes/color", "Color", false, BtGattColor,
                               BtGattColor{0xFFFFFFFF}>
    myEyesColor;
// BtGattSlotUpNext / BtGattSlotString / BtGattSlotNowPlaying below are wire- and
// persistence-compatible with the plain uint32_t / BtGattString types they replaced —
// only the CPF format byte differs (issue #260: tells the app to render the slot
// playlist UI instead of raw numeric/text inputs). Settings keys and stored bytes are
// unchanged, so values persisted by older firmware load as-is.
BtGattPersistentCharacteristic<"my_eyes/up_next", "Up Next", true, BtGattSlotUpNext,
                               BtGattSlotUpNext{0}>
    myEyesUpNext;

constexpr BtGattSlotString<MyEyesAnimation::kMaxEyeLen> kEmptyEyeSlot = {};
BtGattPersistentCharacteristic<"my_eyes/slot0", "Slot 0", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot0;
BtGattPersistentCharacteristic<"my_eyes/slot1", "Slot 1", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot1;
BtGattPersistentCharacteristic<"my_eyes/slot2", "Slot 2", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot2;
BtGattPersistentCharacteristic<"my_eyes/slot3", "Slot 3", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot3;
BtGattPersistentCharacteristic<"my_eyes/slot4", "Slot 4", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot4;
BtGattPersistentCharacteristic<"my_eyes/slot5", "Slot 5", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot5;
BtGattPersistentCharacteristic<"my_eyes/slot6", "Slot 6", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot6;
BtGattPersistentCharacteristic<"my_eyes/slot7", "Slot 7", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot7;
BtGattPersistentCharacteristic<"my_eyes/slot8", "Slot 8", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot8;
BtGattPersistentCharacteristic<"my_eyes/slot9", "Slot 9", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot9;
BtGattPersistentCharacteristic<"my_eyes/slot10", "Slot 10", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot10;
BtGattPersistentCharacteristic<"my_eyes/slot11", "Slot 11", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot11;
BtGattPersistentCharacteristic<"my_eyes/slot12", "Slot 12", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot12;
BtGattPersistentCharacteristic<"my_eyes/slot13", "Slot 13", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot13;
BtGattPersistentCharacteristic<"my_eyes/slot14", "Slot 14", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot14;
BtGattPersistentCharacteristic<"my_eyes/slot15", "Slot 15", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot15;
BtGattPersistentCharacteristic<"my_eyes/slot16", "Slot 16", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot16;
BtGattPersistentCharacteristic<"my_eyes/slot17", "Slot 17", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot17;
BtGattPersistentCharacteristic<"my_eyes/slot18", "Slot 18", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot18;
BtGattPersistentCharacteristic<"my_eyes/slot19", "Slot 19", false,
                               BtGattSlotString<MyEyesAnimation::kMaxEyeLen>, kEmptyEyeSlot>
    myEyesSlot19;

using MyEyesIsActiveCharacteristic = IsActiveCharacteristic<Animation::MyEyes>;
MyEyesIsActiveCharacteristic myEyesIsActive;

constexpr BtGattString<24> kMyEyesAnimationName = makeBtGattString<24>("MyEyes");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kMyEyesAnimationName>
    myEyesAnimationName;

// APPEND-ONLY: new providers go after every existing one (fixed UUID, so auto-UUID
// positions don't shift either way — but bonded phones cache handles per table).
ShuffleIncludeCharacteristic<"my_eyes/shuffle"> myEyesShuffleInclude;

// How long each eye slot displays before the animation advances to the next (issue #260 —
// this is what makes My Eyes cycle autonomously like Text does). Plain uint32 CPF so the
// app renders it as a normal numeric input. tick() clamps values below its 500 ms floor.
BtGattPersistentCharacteristic<"my_eyes/dwell_time_ms", "Dwell Time Ms", false, uint32_t, 5000>
    myEyesDwellTimeMs;

BtGattAutoReadNotifyCharacteristic<"Now Playing", BtGattSlotNowPlaying, BtGattSlotNowPlaying{0}>
    myEyesNowPlaying;

BtGattServer myEyesConfigServer(myEyesPrimaryService, myEyesBlinkSpeedMs, myEyesColor, myEyesUpNext,
                                myEyesSlot0, myEyesSlot1, myEyesSlot2, myEyesSlot3, myEyesSlot4,
                                myEyesSlot5, myEyesSlot6, myEyesSlot7, myEyesSlot8, myEyesSlot9,
                                myEyesSlot10, myEyesSlot11, myEyesSlot12, myEyesSlot13,
                                myEyesSlot14, myEyesSlot15, myEyesSlot16, myEyesSlot17,
                                myEyesSlot18, myEyesSlot19, myEyesIsActive, myEyesAnimationName,
                                myEyesShuffleInclude, myEyesDwellTimeMs, myEyesNowPlaying);
BT_GATT_SERVER_REGISTER(myEyesConfigServerStatic, myEyesConfigServer);

namespace {
class MyEyesColorSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return static_cast<BtGattColor>(myEyesColor); }
};

MyEyesColorSource sDefaultColorSource;
// Resolves the color value's mode byte (issue #259) so the animation always sees
// an effective 0x00RRGGBB through the same interface.
ColorModeSource sMyEyesColorMode(sDefaultColorSource, sys_rand32_get, k_uptime_get);
}  // namespace

using MyEyesAnimationIsActive = AnimationIsActiveBinding<Animation::MyEyes>;

static void my_eyes_set_is_active(bool active) {
    if (active) {
        // Fires for every activation source (BLE write, shell, boot restore,
        // shuffle) — arms the RandomOnActivate re-roll / mode-state reset.
        sMyEyesColorMode.notifyActivated();
    }
    myEyesIsActive.setActive(active);
}

static bool my_eyes_shuffle_included() {
    return myEyesShuffleInclude.value();
}

struct MyEyesIsActiveBindingRegistrar {
    MyEyesIsActiveBindingRegistrar() {
        MyEyesAnimationIsActive::registerSetter(my_eyes_set_is_active);
        AnimationShuffleIncludeBinding<Animation::MyEyes>::registerGetter(
            my_eyes_shuffle_included);
    }
};

[[maybe_unused]] MyEyesIsActiveBindingRegistrar sMyEyesIsActiveBindingRegistrar;

namespace {
class MyEyesBlinkSpeedSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return myEyesBlinkSpeedMs; }
};
}  // namespace

const char *kStaticEyes[MyEyesAnimation::kNumStringSlots] = {
    "^^", "||",   "><", "XX", "--", "**", "??", "##", "@@", "!!",
    "oo", "\"\"", "==", "88", "$$", "~~", "00", "qq", "TT", "UU",
};

namespace {
static const char *getMyEyesSlot(size_t slot) {
    switch (slot) {
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

static void setMyEyesSlot(size_t slot, const char *value) {
    BtGattSlotString<MyEyesAnimation::kMaxEyeLen> storage = {};
    strncpy(storage.data(), value, MyEyesAnimation::kMaxEyeLen - 1);

    switch (slot) {
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

class MyEyesSlotSource : public MyEyesAnimationSlotSource {
   public:
    const char *getStringFromSlot(size_t slot) const override { return getMyEyesSlot(slot); }
};

class MyEyesUpNextSource : public MyEyesAnimationUpNextSource {
   public:
    size_t consumeCurrentAndAdvance(size_t numSlots) override {
        // The read-modify-write of myEyesUpNext races remote GATT writes (the app's
        // tap-to-queue) running on another thread: a write landing between the read and
        // the store below would be silently clobbered, losing the user's queued slot.
        // Single-core target, so briefly locking the scheduler makes the sequence atomic
        // against every other thread. Safe to hold across operator=: its notify() never
        // blocks (bt_gatt_notify fails immediately with -ENOMEM when no buffer is
        // available — see notify()'s failure logging — rather than waiting).
        k_sched_lock();
        // .value() (not the characteristic's operator T()) so the wrapper's
        // operator uint32_t is the only user-defined conversion in the sequence.
        uint32_t currUpNext = myEyesUpNext.value();
        uint32_t nextUpNext = currUpNext + 1;
        if (nextUpNext >= numSlots) {
            nextUpNext = 0;
        }
        myEyesUpNext = nextUpNext;
        k_sched_unlock();

        myEyesNowPlaying = currUpNext;
        return currUpNext;
    }
};

class MyEyesDwellTimeSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return myEyesDwellTimeMs; }
};

MyEyesBlinkSpeedSource sDefaultBlinkSpeedSource;
MyEyesSlotSource sDefaultSlotSource;
MyEyesUpNextSource sDefaultUpNextSource;
MyEyesDwellTimeSource sDefaultDwellTimeSource;

MyEyesAnimationDependencies sDefaultMyEyesDeps(sDefaultBlinkSpeedSource, sMyEyesColorMode,
                                               sDefaultSlotSource, sDefaultUpNextSource,
                                               sDefaultDwellTimeSource);

struct MyEyesSlotInitializer {
    MyEyesSlotInitializer() {
        for (size_t i = 0; i < MyEyesAnimation::kNumStringSlots; i++) {
            setMyEyesSlot(i, kStaticEyes[i]);
        }
    }
};

[[maybe_unused]] MyEyesSlotInitializer sMyEyesSlotInitializer;
}  // namespace

void my_eyes_animation_bind_default_dependencies() {
    MyEyesAnimation::getInstance()->setDependencies(sDefaultMyEyesDeps);
}
