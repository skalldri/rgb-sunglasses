/*
 * Simulator shim for <zephyr/llext/symbol.h>.
 *
 * On the device, EXPORT_SYMBOL(x) records `x` in the ELF's llext export table
 * so the host can llext_find_sym() it. In the WASM simulator build, symbol
 * visibility is handled by wasm-ld's --export-if-defined flags instead (see
 * fw/sim/build-extensions.sh), so the macro only needs to keep the symbol
 * alive and compile to nothing.
 */

#ifndef RGBX_SIM_SHIM_LLEXT_SYMBOL_H_
#define RGBX_SIM_SHIM_LLEXT_SYMBOL_H_

#define EXPORT_SYMBOL(x)

#endif /* RGBX_SIM_SHIM_LLEXT_SYMBOL_H_ */
