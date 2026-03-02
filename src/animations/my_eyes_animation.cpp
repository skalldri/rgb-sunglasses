#include <animations/my_eyes_animation.h>
#include <animations/animation_is_active_binding.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/read_write_string.h>
#include <fonts/FontAtlas.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(my_eyes_animation, LOG_LEVEL_INF);

constexpr bt_uuid_128 kMyEyesConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x0500, 0x56789abd0000));

BtGattPrimaryService<kMyEyesConfigServiceUuid> myEyesPrimaryService;
BtGattAutoReadWriteNotifyCharacteristic<"Blink Speed Ms", uint32_t, 100> myEyesBlinkSpeedMs;
BtGattAutoReadWriteNotifyCharacteristic<"Color", BtGattColor, BtGattColor{0xFFFFFFFF}> myEyesColor;
BtGattAutoReadWriteNotifyCharacteristic<"Up Next", uint32_t, 0> myEyesUpNext;

BtGattServer myEyesConfigServer(
    myEyesPrimaryService,
    myEyesBlinkSpeedMs,
    myEyesColor,
    myEyesUpNext);
BT_GATT_SERVER_REGISTER(myEyesConfigServerStatic, myEyesConfigServer);

// All services implement the "IsActive" service, so declare relevant BT GATT glue logic
using MyEyesAnimationIsActive = AnimationIsActiveBinding<Animation::MyEyes, BtServiceId::MyEyes>;
BT_SVC_UUID_DEFINE(MyEyesAnimationIsActive);
BT_SVC_IS_ACTIVE_CHRC_DEFINE(MyEyesAnimationIsActive);

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

constexpr size_t kNumStringSlots = 20;

constexpr size_t kStringSlotStartChrc = 100;
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 100, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 101, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 102, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 103, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 104, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 105, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 106, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 107, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 108, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 109, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 110, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 111, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 112, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 113, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 114, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 115, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 116, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 117, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 118, MyEyesAnimation::kMaxEyeLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(MyEyesAnimation, 119, MyEyesAnimation::kMaxEyeLen);

BT_GATT_SERVICE_DEFINE(myeyes_anim_service,
                       BT_SVC_UUID_REFERENCE(MyEyesAnimationIsActive),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 100, "Slot 0"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 101, "Slot 1"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 102, "Slot 2"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 103, "Slot 3"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 104, "Slot 4"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 105, "Slot 5"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 106, "Slot 6"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 107, "Slot 7"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 108, "Slot 8"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 109, "Slot 9"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 110, "Slot 10"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 111, "Slot 11"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 112, "Slot 12"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 113, "Slot 13"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 114, "Slot 14"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 115, "Slot 15"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 116, "Slot 16"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 117, "Slot 17"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 118, "Slot 18"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 119, "Slot 19"),
                       BT_SVC_IS_ACTIVE_CHRC_REFERENCE(MyEyesAnimationIsActive));

const char *kStaticEyes[kNumStringSlots] = {
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

template <size_t tChrcId>
using StrSlot = BtReadWriteString<MyEyesAnimation::kBtServiceIdNum, tChrcId, MyEyesAnimation::kMaxEyeLen>;

namespace
{
    class MyEyesSlotSource : public MyEyesAnimationSlotSource
    {
    public:
        const char *getStringFromSlot(size_t slot) const override
        {
            switch (slot)
            {
            case 0:
                return StrSlot<100>::getInstance();
            case 1:
                return StrSlot<101>::getInstance();
            case 2:
                return StrSlot<102>::getInstance();
            case 3:
                return StrSlot<103>::getInstance();
            case 4:
                return StrSlot<104>::getInstance();
            case 5:
                return StrSlot<105>::getInstance();
            case 6:
                return StrSlot<106>::getInstance();
            case 7:
                return StrSlot<107>::getInstance();
            case 8:
                return StrSlot<108>::getInstance();
            case 9:
                return StrSlot<109>::getInstance();
            case 10:
                return StrSlot<110>::getInstance();
            case 11:
                return StrSlot<111>::getInstance();
            case 12:
                return StrSlot<112>::getInstance();
            case 13:
                return StrSlot<113>::getInstance();
            case 14:
                return StrSlot<114>::getInstance();
            case 15:
                return StrSlot<115>::getInstance();
            case 16:
                return StrSlot<116>::getInstance();
            case 17:
                return StrSlot<117>::getInstance();
            case 18:
                return StrSlot<118>::getInstance();
            case 19:
                return StrSlot<119>::getInstance();
            default:
                return "00";
            }
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
}

// Helper template to initialize everything
template <size_t tChrcId>
static void inline initStrSlot()
{
    StrSlot<tChrcId>::getInstance().setValue(kStaticEyes[tChrcId - kStringSlotStartChrc]);
    initStrSlot<tChrcId - 1>();
}

template <>
void inline initStrSlot<kStringSlotStartChrc>()
{
    StrSlot<kStringSlotStartChrc>::getInstance().setValue(kStaticEyes[0]);
}

// Helper template to initialize everything
template <size_t tChrcId>
inline const char *getStringFromSlotTemplate(size_t slot)
{
    if ((slot + kStringSlotStartChrc) == tChrcId)
    {
        return StrSlot<tChrcId>::getInstance();
    }

    return getStringFromSlotTemplate<tChrcId - 1>(slot);
}

template <>
inline const char *getStringFromSlotTemplate<kStringSlotStartChrc>(size_t slot)
{
    if ((slot + kStringSlotStartChrc) == kStringSlotStartChrc)
    {
        return StrSlot<kStringSlotStartChrc>::getInstance();
    }

    return "00";
}

const char *MyEyesAnimation::getStringFromSlot(size_t slot)
{
    if (!deps_)
    {
        setDependencies(sDefaultMyEyesDeps);
    }

    return deps_->slotSource.getStringFromSlot(slot);
}

size_t MyEyesAnimation::getUpNext()
{
    if (!deps_)
    {
        setDependencies(sDefaultMyEyesDeps);
    }

    return deps_->upNextSource.consumeCurrentAndAdvance(kNumStringSlots);
}

MyEyesAnimation::MyEyesAnimation()
{
    for (size_t i = 0; i < myeyes_anim_service.attr_count; i++)
    {
        if (myeyes_anim_service.attrs[i].uuid == &is_active_MyEyesAnimationIsActive_uuid.uuid)
        {
            LOG_INF("MyEyesAnimation isActive attr found at index %d", i);
            break;
        }
    }

    LOG_INF("MyEyesAnimation isActive init complete");

    initStrSlot<119>();
    setDependencies(sDefaultMyEyesDeps);
}

void my_eyes_animation_bind_default_dependencies()
{
    MyEyesAnimation::getInstance()->setDependencies(sDefaultMyEyesDeps);
}

void MyEyesAnimation::setDependencies(const MyEyesAnimationDependencies &deps)
{
    deps_ = &deps;
}

void MyEyesAnimation::init()
{
    strncpy(currentEyes, getStringFromSlot(getUpNext()), kMaxEyeLen);
}

void MyEyesAnimation::tick(const LedConfig *config, const size_t timeSinceLastTickMs, const size_t bufferId)
{
    if (!deps_)
    {
        setDependencies(sDefaultMyEyesDeps);
    }

    ARG_UNUSED(deps_->blinkSpeedMs);

    // Turn off all LEDs
    for (size_t x = 0; x < config->displayWidth; x++)
    {
        for (size_t y = 0; y < config->displayHeight; y++)
        {
            pattern_controller_set_pixel_in_framebuffer(config, x, y, bufferId, 0, 0, 0);
        }
    }

    int32_t charWindowPos;

    // This function gets called repeatedly to render to the display
    auto lambda = [&](size_t x, size_t y, bool filled)
    {
        int32_t realX = x + charWindowPos;

        if (realX < 0 || realX >= (int32_t)config->displayWidth)
        {
            // Bail early if this pixel is not on the display
            return;
        }

        // If pixel is filled, fill with white
        if (filled)
        {
            uint32_t color = deps_->color.get();
            uint8_t red = (color >> 16) & 0xFF;
            uint8_t green = (color >> 8) & 0xFF;
            uint8_t blue = (color >> 0) & 0xFF;
            pattern_controller_set_pixel_in_framebuffer(config, realX, y, bufferId, red, green, blue);
        }
    };

    switch (currentEyeState)
    {
    case EyeState::Open:
        // Just draw our current character in each eye
        charWindowPos = kLeftEyePos;
        FontAtlas::getInstance()->PrintChar(currentEyes[0], lambda);

        charWindowPos = kRightEyePos;
        FontAtlas::getInstance()->PrintChar(currentEyes[1], lambda);
        break;

    case EyeState::OpenInBlinkCycle:
        break;

    case EyeState::BlinkClosing:
        break;

    case EyeState::BlinkOpening:
        break;

    case EyeState::Closed:
        break;
    }
}