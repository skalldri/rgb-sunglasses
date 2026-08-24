/**
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Stuart Alldritt
 *
 * @file rgbx_sys.h
 * @brief Declarations for the system functions an extension is allowed to
 * call — include this instead of hand-writing a prototype.
 *
 * The rgbx ABI itself (rgbx_api.h) has zero function imports: the host writes
 * `rgbx_inputs` before each tick and reads `rgbx_framebuffer` after it. But an
 * extension still runs on top of a small sanctioned set of C library and
 * Zephyr functions, listed in `fw/sdk/arm/allowed-symbols.txt` — the subset of
 * the firmware's llext export table the device will resolve at load time.
 * This header is where those get their declarations.
 *
 * ## Why this file exists (issue #351)
 *
 * The SDK used to ship the allow-list without prototypes, so every author who
 * wanted `printk` wrote their own. A wrong one is not caught anywhere obvious:
 * `extern "C"` mangles on the name alone, so a mismatched *signature* still
 * links. On ARM a wrong return type is usually survivable (AAPCS leaves the
 * value in r0 and an ignoring caller is fine); in the WebAssembly simulator
 * calls are typed by full signature, so the same source traps on first call
 * with `RuntimeError: unreachable`. A wrong *parameter* type (float where the
 * callee takes double, or vice versa) is a genuine ABI bug on both.
 *
 * Including this header turns all of that into a compile error at the point of
 * the mistake. Do not declare these functions yourself.
 *
 * ## What is declared, and from where
 *
 * The standard functions come from the real `<string.h>` and `<math.h>`,
 * pulled in below, rather than being re-declared here. Both the ARM
 * (arm-none-eabi / picolibc) and wasm (wasi-libc) toolchains ship them with
 * the standard prototypes, and a hand-written copy would risk *conflicting*
 * with the toolchain's own — `restrict` qualifiers and the `noexcept` that C++
 * headers attach to libc declarations make an exact re-declaration fragile for
 * no benefit. Only `printk`/`vprintk`, which have no standard header, are
 * declared here.
 *
 * Note that `<string.h>` and `<math.h>` declare far *more* than the allow-list
 * (`strtol`, `snprintf`, double-precision `sin`/`pow`, …). Calling those still
 * compiles and still fails — at llext load on the device, and at build time in
 * the SDK's `check-llext.sh` undefined-symbol gate, which is the check that
 * governs. This header fixes wrong prototypes for *sanctioned* symbols; it is
 * not, and cannot be, the gate on which symbols exist.
 *
 * The `__aeabi_*` entries on the allow-list (64-bit division, 64-bit-int
 * <-> float conversion, variable-count 64-bit shifts) are deliberately **not**
 * declared. They are ARM run-time ABI helpers the compiler emits on your
 * behalf — their real signatures return multi-register pairs that C cannot
 * express, and no extension calls them by name. They are exported so that
 * ordinary `uint64_t` arithmetic links; nothing to include for them.
 *
 * @see rgbx_api.h for the ABI itself.
 */

#ifndef RGBX_SYS_H_
#define RGBX_SYS_H_

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Print a formatted message to the device console.
 *
 * On the glasses this goes to the UART shell console; in the simulator it goes
 * to the printk pane. Available from inside the sandbox and the usual way to
 * see what an extension is doing when there is no debugger.
 *
 * Returns void — this is Zephyr's `printk` (`zephyr/lib/os/printk.c`), not
 * `printf`. Nothing reports how many bytes were written.
 *
 * Float conversions are **not** formatted. The device builds Zephyr with
 * `CONFIG_CBPRINTF_FP_SUPPORT=n`, so \%f, \%g and \%e emit the literal
 * specifier rather than a number, and the simulator reproduces that
 * deliberately. Scale to an integer instead:
 *
 * @code
 * printk("v=%d\n", (int)(v * 1000.0f));
 * @endcode
 *
 * @param fmt printf-style format string. Supported conversions are
 *            \%c \%s \%p \%d \%i \%u \%x \%X and \%\%, with `l`/`ll`/`z`/`h`
 *            length modifiers and numeric width / zero padding.
 * @param ... values matching the conversions in @p fmt.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
void printk(const char *fmt, ...);

/**
 * @brief `printk` taking an already-started variadic argument list.
 *
 * The same formatting rules and the same `void` return as printk(); see there.
 * Useful for wrapping printk in your own diagnostic helper.
 *
 * @param fmt printf-style format string, as for printk().
 * @param ap  argument list started with `va_start` by the caller, which
 *            remains responsible for `va_end`.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 0)))
#endif
void vprintk(const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif /* RGBX_SYS_H_ */
