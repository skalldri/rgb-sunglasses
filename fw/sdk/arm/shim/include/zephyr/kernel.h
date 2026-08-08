/*
 * Standalone-SDK shim for <zephyr/kernel.h> — ARM .llext builds only.
 *
 * Extensions may only call the symbols the device actually exports to llext
 * (the string/memory functions plus printk — see arm/allowed-symbols.txt);
 * of those, printk is the only one needing a declaration extensions commonly
 * pull from <zephyr/kernel.h>.
 */

#ifndef RGBX_SDK_ZEPHYR_KERNEL_H
#define RGBX_SDK_ZEPHYR_KERNEL_H

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((format(printf, 1, 2)))
void printk(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* RGBX_SDK_ZEPHYR_KERNEL_H */
