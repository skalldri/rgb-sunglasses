# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Stuart Alldritt
# Toolchain for device-loadable RGBX v2 WebAssembly packages. It reuses the
# release-pinned wasi-sdk resolver from the simulator toolchain, then replaces
# the target and flags with the firmware's freestanding memoryless profile.

include("${CMAKE_CURRENT_LIST_DIR}/wasm.cmake")

set(RGBX_TARGET "rgbx-v2" CACHE STRING "rgbx extension build target" FORCE)
set(CMAKE_C_FLAGS_INIT
    "--target=wasm32-unknown-unknown -O2 -ffreestanding -fno-builtin -fno-math-errno"
    CACHE STRING "RGBX v2 C flags" FORCE)
set(CMAKE_CXX_FLAGS_INIT
    "--target=wasm32-unknown-unknown -O2 -std=c++23 -ffreestanding -fno-builtin -fno-math-errno -fno-exceptions -fno-rtti"
    CACHE STRING "RGBX v2 C++ flags" FORCE)
