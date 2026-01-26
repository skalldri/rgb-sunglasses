#include <animations/my_eyes_animation.h>
#include <bluetooth/read_write_variable.h>
#include <bluetooth/read_write_string.h>
#include <fonts/FontAtlas.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(my_eyes_animation, LOG_LEVEL_INF);

BT_SVC_UUID_DEFINE(MyEyesAnimation);

// All services implement the "IsActive" service, so declare relevant BT GATT glue logic
BT_SVC_IS_ACTIVE_CHRC_DEFINE(MyEyesAnimation);

using BlinkSpeedMs = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(MyEyesAnimation, 0, uint32_t, 100);
using Color = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(MyEyesAnimation, 1, Color, 0xFFFFFFFF);
using UpNext = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(MyEyesAnimation, 2, uint32_t, 0);

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
                       BT_SVC_UUID_REFERENCE(MyEyesAnimation),                                     // Attr 0
                       BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(MyEyesAnimation, 0, "Blink Speed Ms"), // Attr 1, 2
                       BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(MyEyesAnimation, 1, "Color"),          // Attr 3, 4
                       BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(MyEyesAnimation, 2, "Up Next"),        // Attr 5, 6
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(MyEyesAnimation, 100, "Slot 0"),    // Attr 7, 8
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
                       BT_SVC_IS_ACTIVE_CHRC_REFERENCE(MyEyesAnimation));

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
    if (slot >= kNumStringSlots)
    {
        return "00";
    }

    return getStringFromSlotTemplate<119>(slot);
}

MyEyesAnimation::MyEyesAnimation()
{
    for (size_t i = 0; i < myeyes_anim_service.attr_count; i++)
    {
        if (myeyes_anim_service.attrs[i].uuid == &is_active_MyEyesAnimation_uuid.uuid)
        {
            LOG_INF("MyEyesAnimation isActive attr found at index %d", i);
            break;
        }
    }

    LOG_INF("MyEyesAnimation isActive init complete");

    initStrSlot<119>();
}

void MyEyesAnimation::init()
{
    strncpy(currentEyes, kStaticEyes[2], kMaxEyeLen);
}

void MyEyesAnimation::tick(const LedConfig *config, const size_t timeSinceLastTickMs, const size_t bufferId)
{
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
            uint32_t color = Color::getInstance();
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