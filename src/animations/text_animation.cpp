#include <animations/text_animation.h>
#include <animations/animation_is_active_binding.h>
#include <animations/animation_is_active_characteristic.h>
#include <fonts/FontAtlas.h>

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <cstring>

#include <bluetooth/bt_service_cpp.h>

LOG_MODULE_REGISTER(text_anim, LOG_LEVEL_INF);

//////////////////
#include <bluetooth/bt_service_cpp.h>
constexpr bt_uuid_128 kMyServiceUuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xdeadbeef, 0x1234, 0x5678, 0x1234, 0x56789abcdef0));
BtGattPrimaryService<kMyServiceUuid> primaryService;

BtGattAutoReadNotifyCharacteristic<"Now Playing", uint32_t, 0> characteristicA;

BtGattServer server(primaryService, characteristicA);
BT_GATT_SERVER_REGISTER(serverStatic, server);
///////////////////

constexpr bt_uuid_128 kTextConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x0300, 0x56789abd0000));

BtGattPrimaryService<kTextConfigServiceUuid> textPrimaryService;
BtGattAutoReadWriteNotifyCharacteristic<"Step Time Ms", uint32_t, 50> textStepTimeMs;
BtGattAutoReadWriteNotifyCharacteristic<"Color", BtGattColor, BtGattColor{0xFFFFFFFF}> textColor;
BtGattAutoReadWriteNotifyCharacteristic<"Up Next", uint32_t, 0> textUpNext;

constexpr BtGattString<TextAnimation::kMaxMsgLen> kEmptyTextSlot = {};
BtGattAutoReadWriteNotifyCharacteristic<"Slot 0", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot0;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 1", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot1;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 2", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot2;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 3", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot3;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 4", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot4;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 5", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot5;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 6", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot6;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 7", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot7;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 8", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot8;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 9", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot9;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 10", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot10;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 11", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot11;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 12", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot12;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 13", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot13;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 14", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot14;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 15", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot15;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 16", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot16;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 17", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot17;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 18", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot18;
BtGattAutoReadWriteNotifyCharacteristic<"Slot 19", BtGattString<TextAnimation::kMaxMsgLen>, kEmptyTextSlot> textSlot19;

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

constexpr size_t kNumStringSlots = 20;

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

// All services implement the "IsActive" service, so declare relevant BT GATT glue logic
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

TextAnimation::TextAnimation()
{
    for (size_t i = 0; i < kNumStringSlots; i++)
    {
        setTextSlot(i, kStaticMessages[i]);
    }

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
