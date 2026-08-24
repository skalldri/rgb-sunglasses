/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Stuart Alldritt
 *
 * Simulator-side support code compiled into every extension .wasm module.
 *
 * Provides printk()/vprintk(): the Zephyr symbols the device exports to
 * extensions (fw/sdk/arm/allowed-symbols.txt). On the device they go to the
 * UART console; here they format into an exported append buffer
 * (rgbx_sim_log_buf/rgbx_sim_log_len) that the harness drains after each
 * init/tick call and surfaces as the printk console.
 *
 * Both return void, matching Zephyr (zephyr/include/zephyr/sys/printk.h).
 * printk used to return an int byte count here, which no caller ever read
 * and which made the simulator the odd one out: an extension declaring the
 * device-correct `void printk(...)` linked against this `int` definition and
 * trapped on first call with `RuntimeError: unreachable`, because wasm calls
 * are typed by full signature. Issue #351. The declarations now come from
 * <rgbx/rgbx_sys.h> — included below precisely so a future divergence is a
 * compile error here rather than a runtime trap in someone's extension.
 *
 * The formatter is deliberately self-contained (wasi-libc's vsnprintf pulls
 * in stdio FILE machinery and with it fd_write/fd_seek/fd_close imports,
 * which would break the module's zero-import contract). It supports the
 * integer/string subset Zephyr's printk offers on the device build:
 * %c %s %p %d %i %u %x %X %% with l/ll/z/h length modifiers and numeric
 * width / zero padding. Like the device (CONFIG_CBPRINTF_FP_SUPPORT=n),
 * %f/%g/%e are NOT formatted — the literal specifier is emitted, so an
 * extension relying on float printk looks identically broken here and
 * on-target.
 */

#include <rgbx/rgbx_sys.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#define RGBX_SIM_LOG_BUF_SIZE 2048u

/* Exported via wasm-ld --export-if-defined (see build-extensions.sh). The
 * harness reads rgbx_sim_log_len bytes out of rgbx_sim_log_buf after each
 * init/tick call, then resets rgbx_sim_log_len to 0. Output past the buffer
 * end within one drain interval is dropped (never wrapped): a partial,
 * in-order log beats an interleaved one for debugging. */
uint8_t rgbx_sim_log_buf[RGBX_SIM_LOG_BUF_SIZE];
uint32_t rgbx_sim_log_len = 0;

static void emit_char(char c)
{
    if (rgbx_sim_log_len < RGBX_SIM_LOG_BUF_SIZE) {
        rgbx_sim_log_buf[rgbx_sim_log_len++] = (uint8_t)c;
    }
}

static void emit_str(const char *s)
{
    while (*s != '\0') {
        emit_char(*s++);
    }
}

/* Formats `value` in `base` (10 or 16) with optional zero/space padding to
 * `width`. `upper` selects ABCDEF for %X. */
static void emit_unsigned(uint64_t value, unsigned int base, bool upper, bool zero_pad,
                          int width, bool negative)
{
    char digits[24]; /* enough for 64-bit binary-coded decimal + sign */
    int len = 0;
    const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    do {
        digits[len++] = hex[value % base];
        value /= base;
    } while (value != 0 && len < (int)sizeof(digits));

    if (negative && zero_pad) {
        emit_char('-');
        width--; /* the sign consumed one column of the field */
    }
    for (int pad = width - len - ((negative && !zero_pad) ? 1 : 0); pad > 0; pad--) {
        emit_char(zero_pad ? '0' : ' ');
    }
    if (negative && !zero_pad) {
        emit_char('-');
    }
    while (len > 0) {
        emit_char(digits[--len]);
    }
}

void vprintk(const char *fmt, va_list ap)
{
    while (*fmt != '\0') {
        if (*fmt != '%') {
            emit_char(*fmt++);
            continue;
        }

        const char *spec_start = fmt; /* points at '%', for unknown-spec replay */
        fmt++;

        bool zero_pad = false;
        int width = 0;
        if (*fmt == '0') {
            zero_pad = true;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Length modifiers: h/hh are promoted through varargs anyway; l, ll,
         * z widen the read. */
        int longs = 0;
        bool size_mod = false;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') {
            if (*fmt == 'l') {
                longs++;
            } else if (*fmt == 'z') {
                size_mod = true;
            }
            fmt++;
        }

        char conv = *fmt;
        if (conv == '\0') {
            emit_char('%');
            break;
        }
        fmt++;

        switch (conv) {
        case '%':
            emit_char('%');
            break;
        case 'c':
            emit_char((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            emit_str(s != 0 ? s : "(null)");
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            emit_str("0x");
            emit_unsigned(v, 16, false, true, 8, false);
            break;
        }
        case 'd':
        case 'i': {
            int64_t v;
            if (longs >= 2) {
                v = va_arg(ap, long long);
            } else if (longs == 1) {
                v = va_arg(ap, long);
            } else {
                v = va_arg(ap, int);
            }
            bool neg = v < 0;
            emit_unsigned(neg ? (uint64_t)(-v) : (uint64_t)v, 10, false, zero_pad, width,
                          neg);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v;
            if (longs >= 2) {
                v = va_arg(ap, unsigned long long);
            } else if (longs == 1) {
                v = va_arg(ap, unsigned long);
            } else if (size_mod) {
                v = va_arg(ap, uintptr_t);
            } else {
                v = va_arg(ap, unsigned int);
            }
            emit_unsigned(v, conv == 'u' ? 10 : 16, conv == 'X', zero_pad, width, false);
            break;
        }
        default:
            /* Unknown conversion (incl. %f/%g/%e — float formatting is
             * compiled out on the device): replay the specifier verbatim,
             * matching the device's observable behavior. The matching
             * vararg (if any) is NOT consumed, same as Zephyr's minimal
             * printk path. */
            while (spec_start < fmt) {
                emit_char(*spec_start++);
            }
            break;
        }
    }
}

void printk(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
}
