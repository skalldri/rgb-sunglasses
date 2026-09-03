# Getting started: your first extension {#getting-started}

<!-- The {#getting-started} label on the heading above is load-bearing: it makes
     Doxygen publish this page at /api/getting-started.html rather than a filename
     derived from this file's path, and external links (the rgbx-extension-template
     README) depend on that. GitHub renders the braces literally — that is the cost
     of a stable public URL. It must stay on the FIRST heading, with nothing above
     it: any content before the heading makes Doxygen ignore the label. -->

This guide walks through building a complete rgbx animation extension from
nothing, in either C or C++. The animation we build — a bar that sweeps across
the display — is deliberately unremarkable; the point is that it touches every
concept you get: **parameters** the phone can edit, **inputs** from the
hardware (motion, audio, buttons), **pixel output**, and the **good-moment**
signal.

By the end you will have a `.llext` you can copy onto the glasses and a `.wasm`
you can run in a browser.

You do not need a firmware checkout, a Zephyr toolchain, or hardware to follow
along.

## 1. Get the template

Fork or "Use this template" on
[rgbx-extension-template](https://github.com/skalldri/rgbx-extension-template),
then clone your copy and build it once to make sure the toolchain works:

```bash
./build.sh
```

The first run downloads the pinned compilers and the `rgbx-sdk` for the
firmware release you target; after that it is fast. It produces both outputs
side by side:

```
build/arm/<name>.llext     # runs on the glasses
build/wasm/<name>.wasm     # runs in the simulator
```

Your extension is **one source file** —
[`src/main.c`](https://github.com/skalldri/rgbx-extension-template/blob/main/src/main.c)
in the template, which you can rename to a `.cpp` extension to write C++
instead. One extension is one translation unit.

## 2. What an extension actually is

The firmware runs your code in a sandbox: a user-mode thread with its own
memory. Your extension never calls into the firmware. Instead you export a few
symbols, and the host reads and writes them around each frame:

1. Before every frame it fills in a `rgbx_inputs` struct — the current
   parameter values plus a snapshot of the sensors.
2. It calls your `rgbx_tick()`.
3. It copies your `rgbx_framebuffer` to the display.

That is the whole contract. It lives in
[`rgbx_api.h`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/include/rgbx/rgbx_api.h),
and every symbol in it is documented in this reference.

Because there are no function imports, there is nothing to "call wrong" — but
it also means a few rules are absolute:

- **Your dimensions must match the display** (40 × 12 on proto0) or the
  firmware refuses to load the extension.
- **Globals reset on every activation.** The extension is unloaded whenever it
  is not the active animation, so `rgbx_init()` runs fresh each time. Do not
  expect state to survive a switch away and back.
- **Overrun the per-tick budget and you get killed.** A crash or a hang aborts
  the sandbox, shows a `FAULT:` banner, and switches the animation off — the
  rest of the firmware keeps running.

C++ authors can use
[`rgbx_animation.h`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/include/rgbx/rgbx_animation.h),
a header-only wrapper that turns the above into a class with typed accessors.
It compiles down to exactly the same C symbols. We will use it for the main
walkthrough and show the raw-C equivalent at the end.

## 3. Declare your parameters

Parameters are how the companion app controls your animation. Each one becomes
a BLE characteristic and shows up automatically in the app — you write no app
code. There are five types:

| Type | Control in the app | You read it with |
| ---- | ------------------ | ---------------- |
| `RGBX_PARAM_UINT32` | number field | `paramU32(i)` |
| `RGBX_PARAM_COLOR` | color picker | `paramColor(i)` → `0x00RRGGBB` |
| `RGBX_PARAM_BOOL` | toggle | `paramBool(i)` |
| `RGBX_PARAM_STRING` | text field | `paramString(i)` |
| `RGBX_PARAM_FLOAT` | decimal field | `paramF32(i)` |

Declare them where you instantiate your animation:

```cpp
RGBX_ANIMATION(Starter, "Starter", WIDTH, HEIGHT,
               RGBX_PARAM("Speed",  RGBX_PARAM_UINT32, 20),
               RGBX_PARAM("Color",  RGBX_PARAM_COLOR,  0x00FF0080),
               RGBX_PARAM("Mirror", RGBX_PARAM_BOOL,   0),
               RGBX_PARAM_STR("Label", "rgbx"),
               RGBX_PARAM_F32("Gain", 1.0f));
```

You get up to 16 parameters, of which at most 4 may be strings (31 bytes each).

> **Float params need their own macro and accessor.** Declare with
> `RGBX_PARAM_F32(...)`, never `RGBX_PARAM("Gain", RGBX_PARAM_FLOAT, 1.5)` —
> that compiles, but stores `(uint32_t)1.5 == 1` in the union's integer
> member, which read back as float bits is ~1.4e-45, so your default is
> silently ~0. Read with `paramF32(i)`; `paramU32(i)` on a float param
> returns the raw IEEE-754 bit pattern. Float params also require firmware
> at or above the release that introduced them — an older device rejects the
> whole extension with "bad param type".

> **Index by declaration order.** `paramU32(0)` is the first parameter listed,
> and there is no name lookup. Give the indices names — an `enum` at the top of
> the file — and keep it next to the `RGBX_ANIMATION()` list, because inserting
> a parameter in the middle silently renumbers everything after it.

## 4. Initialize

`init()` runs once per activation, before the first tick:

```cpp
void init() override {
    headX_ = 0.0f;
    phase_ = 0.0f;
    printk("starter: init\n");
}
```

> **Parameters are not readable yet.** They arrive with the first `tick()`.
> Reading them in `init()` gets you defaults at best. If your setup depends on
> a parameter, do it on the first tick instead.

`printk()` works from inside the sandbox and shows up on the serial console —
handy while bringing an animation up. It comes from `<rgbx/rgbx_sys.h>`:

```cpp
#include <rgbx/rgbx_sys.h>
```

> **Never hand-write a prototype for `printk` or anything else on the allowed
> list.** `extern "C"` matches on the *name*, so a wrong signature still links —
> and then does something different on each target. The classic is
> `void printk(...)` vs `int printk(...)`: on ARM the wrong one is usually
> survivable, but WebAssembly types calls by full signature, so in the simulator
> it traps on the first call with `RuntimeError: unreachable` and nothing in the
> build says why. `<rgbx/rgbx_sys.h>` has the right declarations for the whole
> sanctioned surface (it pulls in `<string.h>` and `<math.h>` for you) — include
> it and the mistake becomes a compile error instead.
>
> For the record, `printk` returns **void**. It is Zephyr's, not `printf`'s.

## 5. Read the inputs

Everything for the current frame arrives together. **Every input reads zero
when its source is absent**, so you never need to check whether a board has an
IMU or a microphone:

```cpp
/* Motion — accel in m/s², gyro in rad/s. */
const float tilt = accelX() / 9.81f;      /* roughly -1..1 */

/* Audio — 4 coarse bands with beat flags, plus 20 spectrum buckets. */
const bool kick = isBeat(0);              /* beat in the lowest band */
const float level = bandEnergy(0);
const float bucket = displayBucket(3);    /* raw power — see rgbx_audio_bars.h for bars */

/* Buttons — pressed since the previous tick, so this fires once per press. */
if (buttonWasPressed(0)) {                /* proto0: 0=Up 1=Left 2=Right 3=Down */
    reverse_ = !reverse_;
}
```

`tick()` also receives `dt_ms`, the nominal milliseconds since the last frame.
Scale motion by it rather than assuming a frame rate.

## 6. Draw

`fill()` clears, `setPixel()` writes one pixel, and out-of-range coordinates
are ignored:

```cpp
fill(0, 0, 0);
setPixel(x, y, r, g, b);
```

> **Draw at full scale.** The firmware multiplies every pixel by a global
> brightness factor — 0.02 by default. An animation that "dims itself" to
> 32/255 is simply invisible on the panel. Use the full 0–255 range and let the
> firmware scale it.

In raw C you write the framebuffer yourself, using `RGBX_PIXEL_INDEX` to find
the offset:

```c
size_t i = RGBX_PIXEL_INDEX(WIDTH, x, y);
rgbx_framebuffer[i]     = r;
rgbx_framebuffer[i + 1] = g;
rgbx_framebuffer[i + 2] = b;
```

## 7. Signal good moments

The firmware has a shuffle mode that rotates between animations. If it switches
mid-sweep your animation looks like it glitched. `goodMoment()` lets you say
when a switch would look natural:

```cpp
bool goodMoment() const override { return wrapped_; }
```

Return `true` at real boundaries — the end of a scroll, a clip, a cycle. The
default is `true` for every frame, which is fine for animations with no
structure. In raw C this is the optional `rgbx_good_moment` export; set it to 1
or 0 during your tick.

## 8. Build

```bash
./build.sh
```

The build is gated, and the gates are the useful part. The ARM build checks
that every symbol you reference actually exists on the device, that your
section layout is loadable, and that you fit in the 24 KB extension heap. The
wasm build checks you import nothing.

The most common failure is calling something the firmware does not export. You
get the string and memory functions, `printk`/`vprintk`, **single-precision**
libm (`sinf`, `cosf`, `sqrtf`, `atan2f`, …), and the 64-bit division helpers —
the full list is
[`allowed-symbols.txt`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/sdk/arm/allowed-symbols.txt),
and `<rgbx/rgbx_sys.h>` declares all of it. Notably **all double-precision math
is unavailable**: write `sinf(x)`, not `sin(x)`, and `1.0f`, not `1.0`.

Including `<rgbx/rgbx_sys.h>` does not restrict what you can *call* — it pulls
in the real `<string.h>` and `<math.h>`, which declare plenty the device does
not export. That is what this gate is for; the header's job is making sure the
sanctioned calls have the right *signature*, which no gate can check for you.

## 9. Try it in the simulator

Drag `build/wasm/<name>.wasm` onto
<https://rgb-sunglasses.autom8ed.com/sim/>. It runs your actual source with the
firmware's tick semantics and the real audio DSP, reads your device's
microphone and motion sensors, and renders the true panel layout — no hardware
needed. This is the fast iteration loop.

> **A green simulator run does not mean it loads on the device.** The simulator
> links libc and libm statically, so a call outside the exported surface (that
> `sin()` instead of `sinf()`) works there and fails on the glasses. The ARM
> build in step 8 is what proves that — always do both.

## 10. Install it on your glasses

Copy the `.llext` into `/NAND:/ext/` on the board's USB mass-storage disk, then
reboot so the firmware re-mounts the filesystem and rescans:

```bash
cp build/arm/starter.llext /mnt/sunglasses-fs/ext/
sync && umount /mnt/sunglasses-fs
```

Your animation then appears in the companion app like any built-in one, with a
control for each parameter you declared.

Extensions must come from the same firmware release they were built against —
the ABI version and display dimensions both have to match, or the firmware
rejects the file.

## 11. Publish it

Add your repo to the registry and every firmware release will rebuild your
extension from a pinned commit and ship it to users automatically. See
[Community extension registry](../../extensions/README.md).

## The complete example (C++)

Everything above, in one file. This compiles and passes the device gates as-is.

```cpp
#include <rgbx/rgbx_animation.h>
#include <rgbx/rgbx_sys.h>

/* Must match the host display, and the values passed to RGBX_ANIMATION(). */
#define WIDTH 40
#define HEIGHT 12

/* Parameter indices. These must match the order of the RGBX_PARAM entries in
 * RGBX_ANIMATION() at the bottom of the file. */
enum {
    kSpeed = 0,  /* UINT32 — columns per second */
    kColor,      /* COLOR  — bar color, 0x00RRGGBB */
    kMirror,     /* BOOL   — also draw a mirrored bar */
    kLabel,      /* STRING — its length sets the bar width */
};

/* The pulse phase wraps here rather than free-running. See "Bound your phase
 * accumulators" on the front page: an unbounded argument makes the device's
 * sinf() progressively slower until the extension starts missing frames. */
static const float kTwoPi = 6.2831853f;

class Starter : public rgbx::Animation {
   public:
    /* Runs once per activation, before the first tick. Parameters are NOT
     * readable yet — they arrive with the first tick. */
    void init() override {
        headX_ = 0.0f;
        phase_ = 0.0f;
        reverse_ = false;
        wrapped_ = false;
        printk("starter: init\n");
    }

    void tick(uint32_t dt_ms) override {
        const float dt = static_cast<float>(dt_ms) / 1000.0f;

        /* ---- parameters ------------------------------------------------ */
        const float speed = static_cast<float>(paramU32(kSpeed));
        const uint32_t color = paramColor(kColor);
        const bool mirror = paramBool(kMirror);

        /* A string parameter is yours to interpret; here its length sets the
         * bar width. A real animation might render it with its own font. */
        int barW = 0;
        for (const char *p = paramString(kLabel); *p != '\0'; p++) {
            barW++;
        }
        if (barW < 1) {
            barW = 1;
        }

        /* ---- buttons --------------------------------------------------- */
        /* Reports presses since the previous tick, so this fires once per
         * press. Button 0 is "Up" on proto0. */
        if (buttonWasPressed(0)) {
            reverse_ = !reverse_;
        }

        /* ---- IMU ------------------------------------------------------- */
        /* Reads 0.0 when no IMU is present, so no capability check is needed.
         * Dividing by g gives roughly -1..1 for a tilt. */
        const float tilt = accelX() / 9.81f;
        const int centerRow = HEIGHT / 2 + static_cast<int>(tilt * (HEIGHT / 2));

        /* ---- audio ----------------------------------------------------- */
        const bool kick = isBeat(0); /* beat in the lowest band */

        /* ---- advance --------------------------------------------------- */
        headX_ += speed * dt * (reverse_ ? -1.0f : 1.0f);

        /* Wrap the head, and remember whether it wrapped on THIS frame —
         * that is what goodMoment() reports. */
        wrapped_ = false;
        if (headX_ >= static_cast<float>(WIDTH)) {
            headX_ -= static_cast<float>(WIDTH);
            wrapped_ = true;
        } else if (headX_ < 0.0f) {
            headX_ += static_cast<float>(WIDTH);
            wrapped_ = true;
        }

        phase_ += dt;
        if (phase_ >= kTwoPi) {
            phase_ -= kTwoPi; /* bounded — see kTwoPi above */
        }
        const float pulse = kick ? 1.0f : 0.75f + 0.25f * sinf(phase_);

        /* ---- draw ------------------------------------------------------ */
        fill(0, 0, 0);

        /* Unpack the COLOR parameter and scale it by the pulse. Draw near full
         * scale: the firmware multiplies every pixel by a global brightness
         * factor (0.02 by default), so a self-dimmed animation is invisible on
         * the panel. */
        const uint8_t r = scale((color >> 16) & 0xFFu, pulse);
        const uint8_t g = scale((color >> 8) & 0xFFu, pulse);
        const uint8_t b = scale(color & 0xFFu, pulse);

        const int head = static_cast<int>(headX_);
        for (int i = 0; i < barW; i++) {
            const int x = wrap(head - i, WIDTH);
            drawAt(x, centerRow, r, g, b);
            if (mirror) {
                drawAt(WIDTH - 1 - x, HEIGHT - 1 - centerRow, r, g, b);
            }
        }

        /* Bottom row: the audio spectrum, one column per bucket. The buckets are
         * raw FFT power spanning ~60 dB, so they go through the SDK's dB window
         * (rgbx/rgbx_audio_bars.h) rather than a linear clamp. */
        const int buckets = static_cast<int>(numDisplayBuckets());
        for (int i = 0; i < buckets && i < WIDTH; i++) {
            const float h = rgbx_audio_bar_height(displayBucket(static_cast<size_t>(i)),
                                                  static_cast<size_t>(i),
                                                  RGBX_AUDIO_BAR_FLOOR_DB, RGBX_AUDIO_BAR_RANGE_DB,
                                                  RGBX_AUDIO_BAR_TILT_DB_PER_OCTAVE);
            const uint8_t v = scale(255u, h);
            drawAt(i, HEIGHT - 1, v, v, v);
        }
    }

    /* Asked after every tick. true tells the firmware's shuffle mode this is a
     * natural point to switch away — here, when the bar finishes a sweep. */
    bool goodMoment() const override { return wrapped_; }

   private:
    /* Signed modulo that never returns a negative index. */
    static int wrap(int v, int n) { return ((v % n) + n) % n; }

    static uint8_t scale(uint32_t channel, float k) {
        if (k < 0.0f) {
            k = 0.0f;
        } else if (k > 1.0f) {
            k = 1.0f;
        }
        return static_cast<uint8_t>(static_cast<float>(channel) * k);
    }

    /* Bounds-check before drawing. setPixel() also ignores out-of-range
     * coordinates, but checking here keeps the intent obvious — and if you
     * ever index an array directly instead, this is the check you need. */
    void drawAt(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
            return;
        }
        setPixel(static_cast<size_t>(x), static_cast<size_t>(y), r, g, b);
    }

    float headX_ = 0.0f;
    float phase_ = 0.0f;
    bool reverse_ = false;
    bool wrapped_ = false;
};

/* Declares the manifest, allocates the framebuffer, and emits the five C
 * symbols the firmware looks up. The parameter order here defines the indices
 * at the top of the file. */
RGBX_ANIMATION(Starter, "Starter", WIDTH, HEIGHT,
               RGBX_PARAM("Speed", RGBX_PARAM_UINT32, 20),
               RGBX_PARAM("Color", RGBX_PARAM_COLOR, 0x00FF0080),
               RGBX_PARAM("Mirror", RGBX_PARAM_BOOL, 0),
               RGBX_PARAM_STR("Label", "rgbx"));
```

## The same thing in C

If you would rather not use C++, here is the identical animation against the
raw ABI. The wrapper generates roughly this.

```c
#include <rgbx/rgbx_api.h>
#include <rgbx/rgbx_sys.h>
#include <zephyr/llext/symbol.h>

#define WIDTH 40
#define HEIGHT 12

enum {
    P_SPEED = 0, /* UINT32 */
    P_COLOR,     /* COLOR  */
    P_MIRROR,    /* BOOL   */
    P_LABEL,     /* STRING */
};

static const float kTwoPi = 6.2831853f;

/* --- the required exports -------------------------------------------------
 * In C you declare these yourself. Every one must be EXPORT_SYMBOL'd at the
 * bottom so the firmware can resolve it. */
struct rgbx_inputs rgbx_inputs;
uint8_t rgbx_framebuffer[WIDTH * HEIGHT * 3];

static const struct rgbx_param_desc params[] = {
    RGBX_PARAM("Speed", RGBX_PARAM_UINT32, 20),
    RGBX_PARAM("Color", RGBX_PARAM_COLOR, 0x00FF0080),
    RGBX_PARAM("Mirror", RGBX_PARAM_BOOL, 0),
    RGBX_PARAM_STR("Label", "rgbx"),
};

const struct rgbx_manifest rgbx_manifest = {
    RGBX_ABI_VERSION, "Starter", WIDTH, HEIGHT,
    sizeof(params) / sizeof(params[0]), params,
};

/* The OPTIONAL good-moment export. Absent = every frame is a good moment. */
uint8_t rgbx_good_moment;

static float head_x;
static float phase;
static int reverse;

static int wrap(int v, int n) { return ((v % n) + n) % n; }

static uint8_t scale(uint32_t channel, float k) {
    if (k < 0.0f) {
        k = 0.0f;
    } else if (k > 1.0f) {
        k = 1.0f;
    }
    return (uint8_t)((float)channel * k);
}

static void draw_at(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    size_t i;
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
        return;
    }
    i = RGBX_PIXEL_INDEX(WIDTH, x, y);
    rgbx_framebuffer[i] = r;
    rgbx_framebuffer[i + 1] = g;
    rgbx_framebuffer[i + 2] = b;
}

void rgbx_init(void) {
    head_x = 0.0f;
    phase = 0.0f;
    reverse = 0;
    rgbx_good_moment = 0;
    printk("starter: init\n");
}

void rgbx_tick(void) {
    const float dt = (float)rgbx_inputs.dt_ms / 1000.0f;
    const float speed = (float)rgbx_inputs.params[P_SPEED];
    const uint32_t color = rgbx_inputs.params[P_COLOR] & 0x00FFFFFFu;
    const int mirror = rgbx_inputs.params[P_MIRROR] != 0u;
    float tilt, pulse;
    int center_row, head, bar_w = 0, i, kick;
    uint8_t r, g, b;
    const char *label;

    /* STRING params live in their own array: the i-th string-typed parameter
     * in declaration order, NOT params[P_LABEL]. "Label" is the only one, so
     * it is slot 0. (The C++ wrapper's paramString() does this mapping.) */
    label = rgbx_inputs.param_strings[0];
    while (label[bar_w] != '\0') {
        bar_w++;
    }
    if (bar_w < 1) {
        bar_w = 1;
    }

    /* Buttons: bit i = button i pressed since the previous tick. */
    if (rgbx_inputs.buttons_pressed & (1u << 0)) {
        reverse = !reverse;
    }

    /* IMU + audio read as zeros when the source is absent. */
    tilt = rgbx_inputs.accel[0] / 9.81f;
    center_row = HEIGHT / 2 + (int)(tilt * (HEIGHT / 2));
    kick = rgbx_inputs.audio_beat[0] != 0u;

    head_x += speed * dt * (reverse ? -1.0f : 1.0f);

    rgbx_good_moment = 0u;
    if (head_x >= (float)WIDTH) {
        head_x -= (float)WIDTH;
        rgbx_good_moment = 1u;
    } else if (head_x < 0.0f) {
        head_x += (float)WIDTH;
        rgbx_good_moment = 1u;
    }

    phase += dt;
    if (phase >= kTwoPi) {
        phase -= kTwoPi; /* bounded — see kTwoPi above */
    }
    pulse = kick ? 1.0f : 0.75f + 0.25f * sinf(phase);

    memset(rgbx_framebuffer, 0, sizeof(rgbx_framebuffer));

    r = scale((color >> 16) & 0xFFu, pulse);
    g = scale((color >> 8) & 0xFFu, pulse);
    b = scale(color & 0xFFu, pulse);

    head = (int)head_x;
    for (i = 0; i < bar_w; i++) {
        const int x = wrap(head - i, WIDTH);
        draw_at(x, center_row, r, g, b);
        if (mirror) {
            draw_at(WIDTH - 1 - x, HEIGHT - 1 - center_row, r, g, b);
        }
    }

    /* Raw FFT power spanning ~60 dB — the SDK's dB window, not a linear clamp. */
    for (i = 0; i < (int)RGBX_AUDIO_NUM_DISPLAY_BUCKETS && i < WIDTH; i++) {
        const float h = rgbx_audio_bar_height(rgbx_inputs.audio_display_bucket[i], (size_t)i,
                                              RGBX_AUDIO_BAR_FLOOR_DB, RGBX_AUDIO_BAR_RANGE_DB,
                                              RGBX_AUDIO_BAR_TILT_DB_PER_OCTAVE);
        const uint8_t v = scale(255u, h);
        draw_at(i, HEIGHT - 1, v, v, v);
    }
}

EXPORT_SYMBOL(rgbx_manifest);
EXPORT_SYMBOL(rgbx_inputs);
EXPORT_SYMBOL(rgbx_framebuffer);
EXPORT_SYMBOL(rgbx_init);
EXPORT_SYMBOL(rgbx_tick);
EXPORT_SYMBOL(rgbx_good_moment);
```

## Things that bite people

- **Nothing shows on the panel.** You are probably drawing dim. The global
  brightness factor is 0.02 — draw at full 0–255.
- **It builds for wasm but not for ARM.** You called something outside the
  exported surface. Double-precision math is the usual culprit: `sinf`, not
  `sin`.
- **It runs fine for a minute, then stutters.** An unbounded phase accumulator
  feeding `sinf`. Wrap it. This is real — it shipped twice; see "Bound your
  phase accumulators" in [Animation Extensions](README.md).
- **The wrong parameter changed.** Parameters are indexed by declaration order.
  Adding one in the middle renumbers the rest.
- **A string parameter reads empty.** In raw C, strings are not in
  `params[]` — the i-th *string-typed* parameter lives in
  `param_strings[i]`, counting only strings.
- **State vanished.** Globals reset on every activation; the extension is
  unloaded whenever it is not the active animation.
- **It loaded yesterday, not today.** Extension and firmware must be from the
  same release — the ABI version and display dimensions have to match.
