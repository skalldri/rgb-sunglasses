# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Stuart Alldritt
# Toolchain file for building rgbx .llext extensions with a generic ARM
# cross-compiler — no Zephyr SDK, EDK, or west required. Pass via
# -DCMAKE_TOOLCHAIN_FILE (the template's presets do this).
#
# Toolchain resolution: $RGBX_ARM_TOOLCHAIN_PATH env override (any root whose
# bin/ holds arm-none-eabi-gcc or arm-zephyr-eabi-gcc), else the pinned Arm
# GNU Toolchain is auto-installed to ~/.cache/rgb-sunglasses by
# scripts/install-arm-toolchain.sh (instant no-op when already cached, which
# matters: CMake re-executes this file for every try_compile).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# The compiler identification link test would otherwise try to build a
# bare-metal executable and fail (no crt0/nosys in a freestanding build).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# The installer re-extracts the toolchain from a pin-verified archive on every
# call (its integrity model; see install-arm-toolchain.sh). CMake re-reads this
# toolchain file for each try_compile, so cache the resolved root in the
# environment: the first evaluation does the verified extraction, and the
# try_compile children inherit the path and reuse that one verified tree within
# this configure run.
if(DEFINED ENV{_RGBX_ARM_TOOLCHAIN_ROOT})
    set(_rgbx_arm_root "$ENV{_RGBX_ARM_TOOLCHAIN_ROOT}")
else()
    execute_process(
        COMMAND "${CMAKE_CURRENT_LIST_DIR}/../../scripts/install-arm-toolchain.sh"
        OUTPUT_VARIABLE _rgbx_arm_root
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rgbx_arm_rc)
    if(NOT _rgbx_arm_rc EQUAL 0)
        message(FATAL_ERROR "rgbx-sdk: failed to resolve/install the ARM toolchain (install-arm-toolchain.sh exited ${_rgbx_arm_rc})")
    endif()
    set(ENV{_RGBX_ARM_TOOLCHAIN_ROOT} "${_rgbx_arm_root}")
endif()

# Probe both triple prefixes: arm-none-eabi (the pinned Arm GNU Toolchain)
# and arm-zephyr-eabi (lets a Zephyr SDK tree stand in via the env override).
if(EXISTS "${_rgbx_arm_root}/bin/arm-none-eabi-gcc")
    set(_rgbx_arm_prefix "arm-none-eabi")
elseif(EXISTS "${_rgbx_arm_root}/bin/arm-zephyr-eabi-gcc")
    set(_rgbx_arm_prefix "arm-zephyr-eabi")
else()
    message(FATAL_ERROR "rgbx-sdk: no arm-none-eabi-gcc or arm-zephyr-eabi-gcc under ${_rgbx_arm_root}/bin")
endif()

set(CMAKE_C_COMPILER "${_rgbx_arm_root}/bin/${_rgbx_arm_prefix}-gcc")
set(CMAKE_CXX_COMPILER "${_rgbx_arm_root}/bin/${_rgbx_arm_prefix}-g++")
# The real ld (not the gcc driver): rgbx_add_extension's mandatory `ld -r`
# partial link uses CMAKE_LINKER directly, matching fw/extensions/build.sh.
set(CMAKE_LINKER "${_rgbx_arm_root}/bin/${_rgbx_arm_prefix}-ld" CACHE FILEPATH "")
set(CMAKE_NM "${_rgbx_arm_root}/bin/${_rgbx_arm_prefix}-nm" CACHE FILEPATH "")
set(CMAKE_READELF "${_rgbx_arm_root}/bin/${_rgbx_arm_prefix}-readelf" CACHE FILEPATH "")

# The exact flag surface an on-device .llext needs (mirrors what the Zephyr
# EDK emits for this SoC, minus host paths): Cortex-M33 hard-float
# single-precision FPU, long calls for llext relocation reach, and
# LL_EXTENSION_BUILD to make EXPORT_SYMBOL emit .exported_sym entries.
# -O2 is pinned deliberately: the undefined-symbol surface (__aeabi_*
# helpers vs inline FPU/UDIV instructions) is codegen-dependent, so the
# optimization level is part of the reproducibility contract — do not let
# CMAKE_BUILD_TYPE vary it.
set(_rgbx_arm_common "-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 -mlong-calls -DLL_EXTENSION_BUILD -O2 -g")
set(CMAKE_C_FLAGS_INIT "${_rgbx_arm_common}")
set(CMAKE_CXX_FLAGS_INIT "${_rgbx_arm_common} -std=c++23 -fno-exceptions -fno-rtti")

set(RGBX_TARGET "arm" CACHE STRING "rgbx extension build target")

# Reproducibility check: template CI and the monorepo's registry CI build
# with the identical pinned toolchain, so "green in my fork" is predictive.
# Local stand-ins (e.g. Zephyr SDK GCC 12.2) warn; CI passes
# -DRGBX_STRICT_TOOLCHAIN=ON to make any deviation fatal.
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -dumpversion
    OUTPUT_VARIABLE _rgbx_arm_gccver
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT (_rgbx_arm_prefix STREQUAL "arm-none-eabi" AND _rgbx_arm_gccver MATCHES "^13\\.2"))
    if(RGBX_STRICT_TOOLCHAIN)
        message(FATAL_ERROR "rgbx-sdk: strict toolchain mode requires arm-none-eabi GCC 13.2 (Arm GNU Toolchain 13.2.Rel1); found ${_rgbx_arm_prefix} GCC ${_rgbx_arm_gccver} at ${_rgbx_arm_root}")
    elseif(NOT _RGBX_ARM_TOOLCHAIN_WARNED)
        message(WARNING "rgbx-sdk: building with ${_rgbx_arm_prefix} GCC ${_rgbx_arm_gccver} (pinned: arm-none-eabi 13.2.Rel1). Codegen may differ from CI — the undefined-symbol gate result is compiler-dependent.")
        set(_RGBX_ARM_TOOLCHAIN_WARNED ON CACHE INTERNAL "")
    endif()
endif()
