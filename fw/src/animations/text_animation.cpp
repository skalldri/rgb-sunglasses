#include <animations/text_animation.h>
#include <fonts/FontAtlas.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>

#include <cstdint>
#include <cstring>

LOG_MODULE_REGISTER(text_anim, LOG_LEVEL_INF);

// A message's minimum on-screen time is the shared kMinSlotDwellMs floor
// (animation_base.h — see its comment for the issue #188 notify-flood rationale). A real
// message scrolls for seconds before it finishes (a 1-char message at the 50ms default
// step time dwells ~2.8s), so the floor never affects normal content; it only bounds the
// degenerate case where an empty slot would otherwise advance every render tick.

TextAnimation::TextAnimation() = default;

void TextAnimation::setDependencies(const TextAnimationDependencies &deps) {
    deps_ = &deps;
}

size_t TextAnimation::getUpNext() {
    __ASSERT(deps_, "TextAnimation::getUpNext before setDependencies");

    return deps_->upNextSource.consumeCurrentAndAdvance(kNumStringSlots);
}

const char *TextAnimation::getStringFromSlot(size_t slot) {
    __ASSERT(deps_, "TextAnimation::getStringFromSlot before setDependencies");

    return deps_->slotSource.getStringFromSlot(slot);
}

void TextAnimation::init() {
    currentCycleTimeMs = 0;
    currentMessageDwellMs = 0;
    currentTextOffset = 0;
    atGoodSwitchPoint_ = false;
    // Discard any advance still pending from a boundary tick — getUpNext() below is
    // this activation's one consume; a stale flag would double-consume.
    advancePending_ = false;
    remainingScrollMs_ = 0;
    strncpy(currentMessage, getStringFromSlot(getUpNext()), kMaxMsgLen);
}

void TextAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    __ASSERT(deps_, "TextAnimation::tick before setDependencies");

    // Only the tick that finishes the current message (below) is a good switch point.
    atGoodSwitchPoint_ = false;

    // Deferred consume from the previous tick's end-of-scroll boundary (same rationale
    // as SlotDwellTracker in animation_base.h): the boundary tick only REPORTS the
    // switch point; the queued next slot is consumed here, one tick later, so a shuffle
    // switch taken at the boundary never eats a slot the user queued via Up Next.
    if (advancePending_) {
        advancePending_ = false;
        currentTextOffset = 0;
        currentMessageDwellMs = 0;
        strncpy(currentMessage, getStringFromSlot(getUpNext()), kMaxMsgLen);
    }

    // Turn off all LEDs
    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            renderer.setPixel(x, y, 0, 0, 0);
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
    // render location, and then slowly reduce this offset (and eventually make it negative) to
    // cause the texture to scroll across the display.
    //
    // We consider the display to be a "window" looking into the texture. To save on compute, we
    // will only attempt to render characters which are within the window + some bounding box.

    // First, lets define some useful constants

    // Optimization to avoid repeatedly calculating the strlen() of currentMessage
    const size_t currentMessageLen = strlen(currentMessage);

    // The total "width" of the virtual texture that would contain the entire string
    // const size_t renderedStringWidth = currentMessageLen * FontAtlas::atlasPixelWidthPerChar;

    // The size of the buffer on either side of the display where we will continue attempting to
    // render characters, which allows characters to partially slide onto the display, one pixel at
    // a time
    const size_t displayEdgeBuffer = FontAtlas::atlasPixelWidthPerChar;

    // The edges of the "display window"
    const int32_t displayWindowLeftSide = -displayEdgeBuffer;
    const int32_t displayWindowRightSide = renderer.displayWidth() + displayEdgeBuffer;

    // When we reset currentTextOffset to zero, the text will start off the edge of the display
    // but only just
    const int32_t currentTextOffsetRelativeToDisplay = currentTextOffset + renderer.displayWidth();

    // Predict the first character of the string that could fit on the display
    size_t firstChar = 0;

    // Until currentTextOffset goes negative, the first char to display will always be 0
    // When currentTextOffset is negative, the first character has fallen off the edge of the
    // display
    if (currentTextOffsetRelativeToDisplay < displayWindowLeftSide) {
        // For every FontAtlas::atlasPixelWidthPerChar we are beyond the displayWindowLeftSide,
        // we can start one character later
        int32_t offsetRelativeToDisplayWindowLeftSide =
            currentTextOffsetRelativeToDisplay - displayWindowLeftSide;

        // For each FontAtlas::atlasPixelWidthPerChar we are beyond the display left edge, we can
        // drop a character Rely on integer division to round down
        int32_t charsToDrop =
            offsetRelativeToDisplayWindowLeftSide / (int32_t)(-FontAtlas::atlasPixelWidthPerChar);

        if (charsToDrop < 0) {
            LOG_ERR("Chars to drop is negative unexpectedly: %d %d %d",
                    currentTextOffsetRelativeToDisplay, offsetRelativeToDisplayWindowLeftSide,
                    charsToDrop);
        } else {
            firstChar += charsToDrop;
        }
    }

    // Accumulate how long the current message has been shown (every tick, including the
    // early-return path below) so an empty message still ages toward the dwell floor.
    currentMessageDwellMs += timeSinceLastTickMs;

    // How much longer this message needs before it finishes scrolling — shuffle's grace
    // request (see goodSwitchPointGraceMs()). Computed here rather than in that const
    // getter because the completion test below depends on renderer.displayWidth(), which
    // the getter has no access to. Without it, shuffle hard-cuts a long message the way it
    // used to cut a long GLIM clip.
    //
    // A pixel step costs about one step-time regardless of the render tick rate (the
    // carry-remainder accumulator below can take several steps in one tick). A step
    // time of 0 means "fastest" = the historical ~90 px/s wall-clock rate
    // (kFastestStepTimeMs rationale).
    size_t stepMs = deps_->stepTimeMs.get();
    if (stepMs == 0) {
        stepMs = kFastestStepTimeMs;
    }
    const size_t msPerPixel = stepMs;
    if (firstChar >= currentMessageLen) {
        // Done scrolling; only the kMinSlotDwellMs floor below is left to wait out.
        remainingScrollMs_ = (currentMessageDwellMs >= kMinSlotDwellMs)
                                 ? 0u
                                 : (uint32_t)(kMinSlotDwellMs - currentMessageDwellMs);
    } else {
        // firstChar >= currentMessageLen becomes true once currentTextOffset (<= 0, and
        // decremented one pixel at a time below) reaches -(len * charWidth + displayWidth
        // + displayEdgeBuffer) — invert that to get the pixels still to scroll.
        const int64_t endOffset =
            -(int64_t)(currentMessageLen * FontAtlas::atlasPixelWidthPerChar +
                       renderer.displayWidth() + displayEdgeBuffer);
        const int64_t pixelsRemaining = (int64_t)currentTextOffset - endOffset;
        const uint64_t ms =
            (pixelsRemaining > 0) ? (uint64_t)pixelsRemaining * msPerPixel : 0u;
        remainingScrollMs_ = (ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)ms;
    }

    // If we have finished scrolling the current message, queue the advance to the next
    // message - but not before it has been shown for at least kMinSlotDwellMs. Without
    // this floor an empty slot (currentMessageLen == 0) satisfies firstChar >=
    // currentMessageLen on every tick and advances at the full render rate, and each
    // advance calls getUpNext() which fires two GATT notifications - flooding the BT TX
    // buffer pool (notify -12).
    if (firstChar >= currentMessageLen && currentMessageDwellMs >= kMinSlotDwellMs) {
        // End of scroll: the frame the message finished is the natural boundary shuffle
        // waits for. The actual consume is deferred to the top of the NEXT tick (see
        // above) so a shuffle switch taken here leaves the queued slot untouched. (The
        // kMinSlotDwellMs floor already bounds how often the all-empty-slots case can
        // reach here — at most ~2/s.)
        atGoodSwitchPoint_ = true;
        advancePending_ = true;
        return;
    }

    int32_t charWindowPos = 0;

    // Resolve the color ONCE per tick, not per pixel: the source may be a
    // ColorModeSource (issue #259), whose get() is a stateful per-tick step that
    // can drain the audio message queue (RandomOnBeat). Calling it per lit pixel
    // put a kernel msgq drain in the inner render loop. Every other animation
    // already reads its color once per tick — match them.
    const uint32_t color = deps_->color.get();

    // This function gets called repeatedly to render to the display
    auto lambda = [&](size_t x, size_t y, bool filled) {
        int32_t realX = x + charWindowPos;

        if (realX < 0 || realX >= (int32_t)renderer.displayWidth()) {
            // Bail early if this pixel is not on the display
            return;
        }

        if (filled) {
            uint8_t red = (color >> 16) & 0xFF;
            uint8_t green = (color >> 8) & 0xFF;
            uint8_t blue = (color >> 0) & 0xFF;
            renderer.setPixel(realX, y, red, green, blue);
        }
    };

    for (size_t i = firstChar; i < currentMessageLen; i++) {
        // Calculate the position of the current character within the virtual texture buffer
        const int32_t currentCharPos = i * FontAtlas::atlasPixelWidthPerChar;

        // Calculate where we would be rendering this character within the display window
        charWindowPos = currentTextOffsetRelativeToDisplay + currentCharPos;

        // If the character is within the virtual display buffer, lets render it
        if (charWindowPos >= displayWindowLeftSide && charWindowPos < displayWindowRightSide) {
            FontAtlas::getInstance()->PrintChar(currentMessage[i], lambda);

            // Debugging
            // printk("%c", currentMessage[i]);
        } else if (charWindowPos > displayWindowRightSide) {
            // Early optimization: if we have found a character which is off the edge of the right
            // side of the display window, we can stop iterating since no more chars will ever need
            // to be rendered
            break;
        }
    }

    // printk("\n");

    // Add the time to our counter
    currentCycleTimeMs += timeSinceLastTickMs;

    // Bound the accumulator against parameter-history abuse (PR #378 review;
    // rationale in rainbow_animation.cpp — a huge-then-small remotely written
    // step time would otherwise run accumulated/step iterations in one tick).
    if (currentCycleTimeMs > stepMs + timeSinceLastTickMs) {
        currentCycleTimeMs = stepMs + timeSinceLastTickMs;
    }

    // Carry the remainder instead of resetting to 0 so the scroll rate stays
    // wall-clock correct at any render tick rate, including step times shorter
    // than the tick interval (issue #376). stepMs >= 1 (0 mapped above).
    while (currentCycleTimeMs > stepMs) {
        currentCycleTimeMs -= stepMs;
        currentTextOffset--;  // Move text one pixel to the left
    }
}
