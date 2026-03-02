#include <animations/text_animation.h>
#include <animations/animation_is_active_binding.h>
#include <fonts/FontAtlas.h>

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include <bluetooth/read_write_string.h>
#include <bluetooth/read_write_variable.h>

LOG_MODULE_REGISTER(text_anim, LOG_LEVEL_INF);

//////////////////
#include <bluetooth/bt_service_cpp.h>
constexpr bt_uuid_128 kMyServiceUuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xdeadbeef, 0x1234, 0x5678, 0x1234, 0x56789abcdef0));
BtGattPrimaryService<kMyServiceUuid> primaryService;

BtGattAutoReadNotifyCharacteristic<"Now Playing", uint32_t, 0> characteristicA;

BtGattServer server(primaryService, characteristicA);
BT_GATT_SERVER_REGISTER(serverStatic, server);
///////////////////

BT_SVC_UUID_DEFINE(TextAnimation);

constexpr size_t kNumStringSlots = 20;

using StepTimeMs = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(TextAnimation, 0, uint32_t, 50);
using Color = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(TextAnimation, 1, Color, 0xFFFFFFFF);
using UpNext = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(TextAnimation, 2, uint32_t, 0);

namespace
{
    class TextStepTimeSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return (uint32_t)StepTimeMs::getInstance();
        }
    };

    class TextColorSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return (uint32_t)Color::getInstance();
        }
    };
}

// Declare a bunch of read/write string instance
constexpr size_t kStringSlotStartChrc = 100;
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 100, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 101, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 102, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 103, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 104, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 105, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 106, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 107, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 108, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 109, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 110, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 111, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 112, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 113, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 114, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 115, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 116, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 117, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 118, TextAnimation::kMaxMsgLen);
BT_SVC_READ_WRITE_STRING_CHRC_DEFINE(TextAnimation, 119, TextAnimation::kMaxMsgLen);

// All services implement the "IsActive" service, so declare relevant BT GATT glue logic
using TextAnimationIsActive = AnimationIsActiveBinding<Animation::Text, BtServiceId::Text>;
BT_SVC_IS_ACTIVE_CHRC_DEFINE(TextAnimationIsActive);

BT_GATT_SERVICE_DEFINE(text_anim_service,
                       BT_SVC_UUID_REFERENCE(TextAnimation),
                       BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(TextAnimation, 0, "Step Time Ms"),
                       BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(TextAnimation, 1, "Color"),
                       BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(TextAnimation, 2, "Up Next"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 100, "Slot 0"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 101, "Slot 1"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 102, "Slot 2"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 103, "Slot 3"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 104, "Slot 4"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 105, "Slot 5"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 106, "Slot 6"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 107, "Slot 7"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 108, "Slot 8"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 109, "Slot 9"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 110, "Slot 10"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 111, "Slot 11"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 112, "Slot 12"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 113, "Slot 13"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 114, "Slot 14"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 115, "Slot 15"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 116, "Slot 16"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 117, "Slot 17"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 118, "Slot 18"),
                       BT_SVC_READ_WRITE_STRING_CHRC_REFERENCE(TextAnimation, 119, "Slot 19"),
                       BT_SVC_IS_ACTIVE_CHRC_REFERENCE(TextAnimationIsActive), );

const char *kStaticMessages[kNumStringSlots] = {
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

template <size_t tChrcId>
using StrSlot = BtReadWriteString<TextAnimation::kBtServiceIdNum, tChrcId, TextAnimation::kMaxMsgLen>;

namespace
{
    class TextSlotSource : public TextAnimationSlotSource
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
                return "INVALID STRING SLOT";
            }
        }
    };

    class TextUpNextSource : public TextAnimationUpNextSource
    {
    public:
        size_t consumeCurrentAndAdvance(size_t numSlots) override
        {
            uint32_t currUpNext = UpNext::getInstance();
            uint32_t nextUpNext = currUpNext + 1;
            if (nextUpNext >= numSlots)
            {
                nextUpNext = 0;
            }

            UpNext::getInstance() = nextUpNext;
            characteristicA = currUpNext;

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
}

// Helper template to initialize everything
template <size_t tChrcId>
static void inline initStrSlot()
{
    StrSlot<tChrcId>::getInstance().setValue(kStaticMessages[tChrcId - kStringSlotStartChrc]);
    initStrSlot<tChrcId - 1>();
}

template <>
void inline initStrSlot<kStringSlotStartChrc>()
{
    StrSlot<kStringSlotStartChrc>::getInstance().setValue(kStaticMessages[0]);
}

TextAnimation::TextAnimation()
{
    initStrSlot<119>();
    setDependencies(sDefaultTextDeps);
}

void text_animation_bind_default_dependencies()
{
    TextAnimation::getInstance()->setDependencies(sDefaultTextDeps);
}

void TextAnimation::setDependencies(const TextAnimationDependencies &deps)
{
    deps_ = &deps;
}

size_t TextAnimation::getUpNext()
{
    if (!deps_)
    {
        setDependencies(sDefaultTextDeps);
    }

    return deps_->upNextSource.consumeCurrentAndAdvance(kNumStringSlots);
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

    return "INVALID STRING SLOT";
}

const char *TextAnimation::getStringFromSlot(size_t slot)
{
    if (!deps_)
    {
        setDependencies(sDefaultTextDeps);
    }

    return deps_->slotSource.getStringFromSlot(slot);
}

void TextAnimation::init()
{
    currentCycleTimeMs = 0;
    currentTextOffset = 0;
    strncpy(currentMessage, getStringFromSlot(getUpNext()), kMaxMsgLen);
}

void TextAnimation::tick(const LedConfig *config, const size_t timeSinceLastTickMs, const size_t bufferId)
{
    if (!deps_)
    {
        setDependencies(sDefaultTextDeps);
    }

    // Turn off all LEDs
    for (size_t x = 0; x < config->displayWidth; x++)
    {
        for (size_t y = 0; y < config->displayHeight; y++)
        {
            pattern_controller_set_pixel_in_framebuffer(config, x, y, bufferId, 0, 0, 0);
        }
    }

    // We want to scroll arbitrarily long messages across the LED panel
    // we also want to do this as efficiently as possible.
    //
    // High level theory of operation:
    // We will consider the entire text string to be a single giant texture.
    // The width of the texture == strlen(str) * FontAtlas::atlasPixelWidthPerChar
    //
    // The texture will start at the right side of the display: we apply an offset to the texture
    // render location, and then slowly reduce this offset (and eventually make it negative) to cause
    // the texture to scroll across the display.
    //
    // We consider the display to be a "window" looking into the texture. To save on compute, we will only
    // attempt to render characters which are within the window + some bounding box.

    // First, lets define some useful constants

    // Optimization to avoid repeatedly calculating the strlen() of currentMessage
    const size_t currentMessageLen = strlen(currentMessage);

    // The total "width" of the virtual texture that would contain the entire string
    // const size_t renderedStringWidth = currentMessageLen * FontAtlas::atlasPixelWidthPerChar;

    // The size of the buffer on either side of the display where we will continue attempting to render
    // characters, which allows characters to partially slide onto the display, one pixel at a time
    const size_t displayEdgeBuffer = FontAtlas::atlasPixelWidthPerChar;

    // The edges of the "display window"
    const int32_t displayWindowLeftSide = -displayEdgeBuffer;
    const int32_t displayWindowRightSide = config->displayWidth + displayEdgeBuffer;

    // When we reset currentTextOffset to zero, the text will start off the edge of the display
    // but only just
    const int32_t currentTextOffsetRelativeToDisplay = currentTextOffset + config->displayWidth;

    // Predict the first character of the string that could fit on the display
    size_t firstChar = 0;

    // Until currentTextOffset goes negative, the first char to display will always be 0
    // When currentTextOffset is negative, the first character has fallen off the edge of the display
    if (currentTextOffsetRelativeToDisplay < displayWindowLeftSide)
    {
        // For every FontAtlas::atlasPixelWidthPerChar we are beyond the displayWindowLeftSide,
        // we can start one character later
        int32_t offsetRelativeToDisplayWindowLeftSide = currentTextOffsetRelativeToDisplay - displayWindowLeftSide;

        // For each FontAtlas::atlasPixelWidthPerChar we are beyond the display left edge, we can drop a character
        // Rely on integer division to round down
        int32_t charsToDrop = offsetRelativeToDisplayWindowLeftSide / (int32_t)(-FontAtlas::atlasPixelWidthPerChar);

        if (charsToDrop < 0)
        {
            LOG_ERR("Chars to drop is negative unexpectedly: %d %d %d", currentTextOffsetRelativeToDisplay, offsetRelativeToDisplayWindowLeftSide, charsToDrop);
        }
        else
        {
            firstChar += charsToDrop;
        }
    }

    // If we have finished scrolling the current message, pick the next message
    if (firstChar >= currentMessageLen)
    {
        currentTextOffset = 0;
        strncpy(currentMessage, getStringFromSlot(getUpNext()), kMaxMsgLen);
        return;
    }

    int32_t charWindowPos = 0;

    // This function gets called repeatedly to render to the display
    auto lambda = [&](size_t x, size_t y, bool filled)
    {
        int32_t realX = x + charWindowPos;

        if (realX < 0 || realX >= (int32_t)config->displayWidth)
        {
            // Bail early if this pixel is not on the display
            return;
        }

        if (filled)
        {
            uint32_t color = deps_->color.get();
            uint8_t red = (color >> 16) & 0xFF;
            uint8_t green = (color >> 8) & 0xFF;
            uint8_t blue = (color >> 0) & 0xFF;
            pattern_controller_set_pixel_in_framebuffer(config, realX, y, bufferId, red, green, blue);
        }
    };

    for (size_t i = firstChar; i < currentMessageLen; i++)
    {
        // Calculate the position of the current character within the virtual texture buffer
        const int32_t currentCharPos = i * FontAtlas::atlasPixelWidthPerChar;

        // Calculate where we would be rendering this character within the display window
        charWindowPos = currentTextOffsetRelativeToDisplay + currentCharPos;

        // If the character is within the virtual display buffer, lets render it
        if (charWindowPos >= displayWindowLeftSide && charWindowPos < displayWindowRightSide)
        {
            FontAtlas::getInstance()->PrintChar(currentMessage[i], lambda);

            // Debugging
            // printk("%c", currentMessage[i]);
        }
        else if (charWindowPos > displayWindowRightSide)
        {
            // Early optimization: if we have found a character which is off the edge of the right side of the display window,
            // we can stop iterating since no more chars will ever need to be rendered
            break;
        }
    }

    // printk("\n");

    // Add the time to our counter
    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > deps_->stepTimeMs.get())
    {
        currentCycleTimeMs = 0;
        currentTextOffset--; // Move text one pixel to the left
    }
}
