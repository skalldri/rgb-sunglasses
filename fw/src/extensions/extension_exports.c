/*
 * Symbols the firmware exports to .llext extensions beyond Zephyr's stock
 * llext set (string/memory functions in subsys/llext/llext_export.c and
 * printk/vprintk in lib/os/printk.c). Issue #295.
 *
 * EXPORT_SYMBOL takes each symbol's address, which both adds it to the
 * .exported_sym table the llext loader resolves against AND pulls the
 * implementation out of the picolibc/libgcc archives into the image.
 *
 * INVARIANT (CI-asserted one way, manual the other — see fw/CLAUDE.md):
 * every addition here must be mirrored in fw/sdk/arm/allowed-symbols.txt or
 * standalone extensions still can't use it (the SDK's undefined-symbol gate
 * rejects anything not on that list); build.yaml's check-allowed-symbols.sh
 * fails if the list ever names a symbol this image stopped exporting.
 *
 * Curation rules:
 *  - Single-precision only. The FPU (fpv5-sp-d16) inlines float
 *    add/sub/mul/div/abs, and the M33 integer core provides UDIV/SDIV
 *    (so 32-bit integer division needs no helper either); what extensions
 *    actually lack are the libm transcendentals below. Double-precision math soft-floats
 *    through the large __aeabi_d* family and stays deliberately
 *    unexported — the SDK gate's rejection message steers authors to
 *    float expressions.
 *  - __aeabi_uldivmod/__aeabi_ldivmod (64-bit integer division) are libgcc
 *    helpers with a stable ARM RTABI contract (IHI0043, "Run-time ABI for
 *    the ARM architecture"), already linked into the image by kernel users.
 *  - memmove: the overlap-safe companion to the already-exported memcpy;
 *    an odd omission from Zephyr's stock list.
 *  - This list is append-only ABI surface: removing an entry breaks every
 *    shipped extension that calls it. Additions need no RGBX_ABI_VERSION
 *    bump (resolution is by symbol name at load; absent = load error, and
 *    the per-release SDK gate prevents extensions from shipping against
 *    firmware that lacks them).
 *
 * Per-tick CPU budget note: transcendentals in a per-pixel loop are
 * affordable at this display size (40x12 @ 128 MHz) but not free — the
 * budget enforcement (CONFIG_APP_EXT_TICK_CPU_BUDGET_MS) is the backstop,
 * and `ext stats` on the shell shows the actual per-tick cost.
 *
 * ...but "affordable" has a range limit, and it is not obvious. The trig
 * exported here is picolibc's fdlibm implementation, which takes a cheap
 * Cody-Waite argument reduction only while |x| <= 2^7*(pi/2) = 201.06
 * (newlib/libm/math/sf_rem_pio2.c). Above that it enters __kernel_rem_pio2f,
 * a multi-precision Payne-Hanek reduction that costs several times more and
 * keeps growing with the argument's exponent. An extension that lets a phase
 * accumulator free-run therefore starts fast and silently degrades minutes
 * later: that is exactly what happened to the plasma extension (issue #304),
 * whose per-tick cost climbed 3.4 ms -> 25 ms over the first five minutes of
 * every activation and overran the render interval on every frame. Extensions
 * must keep phase/angle accumulators bounded — see fw/extensions/README.md,
 * and tilt_animation.cpp for the in-tree precedent.
 */

#include <math.h>
#include <string.h>
#include <zephyr/llext/symbol.h>

/* libgcc's 64-bit division helpers have no public header; the real RTABI
 * signatures return a quotient/remainder pair in r0-r3. The declarations
 * here exist only to take the symbols' addresses for export — extensions
 * never call them by name either; their compiler emits the calls.
 */
extern void __aeabi_uldivmod(void);
extern void __aeabi_ldivmod(void);
extern void __aeabi_ul2f(void);
extern void __aeabi_l2f(void);
extern void __aeabi_f2ulz(void);
extern void __aeabi_f2lz(void);
extern void __aeabi_llsl(void);
extern void __aeabi_llsr(void);
extern void __aeabi_lasr(void);

/* Single-precision libm (picolibc) */
EXPORT_SYMBOL(sinf);
EXPORT_SYMBOL(cosf);
EXPORT_SYMBOL(tanf);
EXPORT_SYMBOL(atan2f);
EXPORT_SYMBOL(sqrtf);
EXPORT_SYMBOL(expf);
EXPORT_SYMBOL(logf);
EXPORT_SYMBOL(powf);
EXPORT_SYMBOL(fmodf);
EXPORT_SYMBOL(floorf);
EXPORT_SYMBOL(ceilf);
EXPORT_SYMBOL(roundf);

/* libgcc 64-bit integer helpers (ARM RTABI): division, the 64-bit-int <->
 * float conversions the FPU has no instructions for, and variable-count
 * 64-bit shifts (constant-count shifts inline; runtime shift amounts call
 * a helper). The conversions matter in practice: `(float)elapsed_ms` on a
 * uint64_t tick counter — the most natural animation phase math there is —
 * emits __aeabi_ul2f (found by this change's own gate test, not
 * hypothetical).
 */
EXPORT_SYMBOL(__aeabi_uldivmod);
EXPORT_SYMBOL(__aeabi_ldivmod);
EXPORT_SYMBOL(__aeabi_ul2f);
EXPORT_SYMBOL(__aeabi_l2f);
EXPORT_SYMBOL(__aeabi_f2ulz);
EXPORT_SYMBOL(__aeabi_f2lz);
EXPORT_SYMBOL(__aeabi_llsl);
EXPORT_SYMBOL(__aeabi_llsr);
EXPORT_SYMBOL(__aeabi_lasr);

/* Overlap-safe memcpy companion */
EXPORT_SYMBOL(memmove);
